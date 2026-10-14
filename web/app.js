// The page: picks the files, runs the build in workers (worker.js, embedded in this page): one prepares and patches the
// game, then several convert it at once. It zips what they send back and saves it. Nothing is uploaded.
/* global fflate, qrcode, WORKER_SOURCE_BASE64, SD_FILES, CIA_URL, PATCH_VERSION, SD_DATA_REV */
"use strict";

const $ = (id) => document.getElementById(id);
const els = {
    drop: $("drop"),
    gameInput: $("game-input"),
    gameName: $("game-name"),
    patchDownload: $("patch-download"),
    patchLocal: $("patch-local"),
    patchInput: $("patch-input"),
    patchName: $("patch-name"),
    start: $("start"),
    setup: $("setup"),
    working: $("working"),
    status: $("status"),
    bar: $("bar"),
    percent: $("percent"),
    elapsed: $("elapsed"),
    error: $("error"),
    errorText: $("error-text"),
    errorLog: $("error-log"),
    retry: $("retry"),
    done: $("done"),
    saved: $("saved"),
    download: $("download"),
    qr: $("qr"),
    ciaLink: $("cia-link"),
};

let gameZip = null;
let patchZip = null;
let running = false;

function formatSize(bytes) {
    return bytes >= 1e9 ? (bytes / 1e9).toFixed(2) + " GB" : (bytes / 1e6).toFixed(0) + " MB";
}

function formatTime(seconds) {
    const m = Math.floor(seconds / 60);
    const s = Math.floor(seconds % 60);
    return m > 0 ? `${m} min ${s} s` : `${s} s`;
}

function updateStart() {
    const patchReady = els.patchDownload.checked || patchZip !== null;
    els.start.disabled = gameZip === null || !patchReady || running;
}

function pickGame(file) {
    if (!file) return;
    gameZip = file;
    els.gameName.textContent = `${file.name} (${formatSize(file.size)})`;
    els.drop.classList.add("chosen");
    updateStart();
}

els.gameInput.addEventListener("change", () => pickGame(els.gameInput.files[0]));
els.drop.addEventListener("dragover", (e) => {
    e.preventDefault();
    els.drop.classList.add("over");
});
els.drop.addEventListener("dragleave", () => els.drop.classList.remove("over"));
els.drop.addEventListener("drop", (e) => {
    e.preventDefault();
    els.drop.classList.remove("over");
    pickGame(e.dataTransfer.files[0]);
});
// A file dropped anywhere else on the page shouldn't navigate away from it.
window.addEventListener("dragover", (e) => e.preventDefault());
window.addEventListener("drop", (e) => e.preventDefault());

for (const radio of [els.patchDownload, els.patchLocal]) {
    radio.addEventListener("change", () => {
        els.patchInput.disabled = !els.patchLocal.checked;
        updateStart();
    });
}
els.patchInput.addEventListener("change", () => {
    patchZip = els.patchInput.files[0] || null;
    els.patchName.textContent = patchZip ? `${patchZip.name} (${formatSize(patchZip.size)})` : "";
    updateStart();
});

// ===[ Output ]===

// Chrome and Edge write the zip straight to a file the user picks; other browsers collect it and offer a download.
async function openOutput() {
    if (window.showSaveFilePicker) {
        try {
            const handle = await window.showSaveFilePicker({
                suggestedName: "am2r-3ds-sd.zip",
                types: [{description: "Zip archive", accept: {"application/zip": [".zip"]}}],
            });
            const writable = await handle.createWritable();
            let chain = Promise.resolve();
            return {
                name: handle.name,
                write: (data) => (chain = chain.then(() => writable.write(data))),
                finish: async () => {
                    await chain;
                    await writable.close();
                    return null;
                },
                abort: () => chain.then(() => writable.abort()).catch(() => {}),
            };
        } catch (e) {
            if (e.name === "AbortError") return null;
            // Not allowed here (e.g. a page opened from a file in some browsers): fall back to a download.
        }
    }
    const parts = [];
    return {
        name: "am2r-3ds-sd.zip",
        write: (data) => parts.push(new Blob([data])),
        finish: async () => new Blob(parts, {type: "application/zip"}),
        abort: () => (parts.length = 0),
    };
}

// ===[ Running ]===

function setProgress(fraction, text) {
    const percent = Math.max(0, Math.min(100, fraction * 100));
    els.bar.style.width = percent.toFixed(1) + "%";
    els.bar.parentElement.setAttribute("aria-valuenow", percent.toFixed(0));
    els.percent.textContent = Math.floor(percent) + "%";
    if (text) els.status.textContent = text;
}

function makeWorker() {
    const source = atob(WORKER_SOURCE_BASE64);
    const bytes = Uint8Array.from(source, (c) => c.charCodeAt(0));
    const url = URL.createObjectURL(new Blob([bytes], {type: "text/javascript"}));
    const worker = new Worker(url);
    URL.revokeObjectURL(url);
    return worker;
}

// Runs one worker task; onMessage sees its progress and files. Resolves with the message that ends it.
function runTask(task, onMessage, workers) {
    return new Promise((resolve, reject) => {
        const worker = makeWorker();
        workers.push(worker);
        worker.onmessage = (event) => {
            const msg = event.data;
            if (msg.type === "error") reject(Object.assign(new Error(msg.message), {userFacing: true}));
            else if (msg.type === "prepared" || msg.type === "converted") {
                worker.terminate();
                resolve(msg);
            } else onMessage(msg);
        };
        worker.onerror = (e) => reject(new Error(e.message || "a worker stopped"));
        worker.postMessage(task);
    });
}

// Each step's share of the bar, in rough seconds of work on one core, and how many items it has (AM2R 1.5.5's counts;
// the bar stays close for it). The music is most of the work, so it is split over several workers.
const WORK = {
    read: {seconds: 2},
    patchFiles: {seconds: 6},
    patch: {seconds: 2},
    copy: {seconds: 1},
    frames: {seconds: 8, items: 7420},
    pages: {seconds: 6, items: 508},
    sprites: {seconds: 10, items: 1739},
    sfx: {seconds: 10},
    music: {seconds: 250, items: 46},
};

function warnOnLeave(e) {
    e.preventDefault();
    e.returnValue = "";
}

els.start.addEventListener("click", async () => {
    if (running) return;
    const output = await openOutput();
    if (output === null) return;
    running = true;
    updateStart();
    els.error.hidden = true;
    els.errorLog.hidden = true;
    els.setup.hidden = true;
    els.working.hidden = false;
    setProgress(0, "Starting");
    window.addEventListener("beforeunload", warnOnLeave);
    const started = Date.now();
    const clock = setInterval(() => (els.elapsed.textContent = formatTime((Date.now() - started) / 1000)), 1000);

    // The zip of 3ds/am2r/, stored (the textures and audio barely compress), written out as it grows.
    let zipBytes = 0;
    const zip = new fflate.Zip((err, chunk) => {
        if (err) throw err;
        zipBytes += chunk.length;
        output.write(chunk);
    });
    const addFile = (path, data) => {
        const entry = new fflate.ZipPassThrough("3ds/am2r/" + path);
        zip.add(entry);
        entry.push(data, true);
    };

    const done = Object.fromEntries(Object.keys(WORK).map((k) => [k, 0]));
    const totalSeconds = Object.values(WORK).reduce((sum, w) => sum + w.seconds, 0);
    let texturesText = null;
    const update = (text) => {
        let seconds = 0;
        for (const [step, w] of Object.entries(WORK)) seconds += w.seconds * Math.min(done[step], 0.99);
        const music = WORK.music.items ? Math.round(done.music * WORK.music.items) : 0;
        let shown = text;
        if (done.copy > 0) {
            const musicText = `${music} of ${WORK.music.items} music tracks done`;
            shown = texturesText ? `${texturesText} · ${musicText}` : `Converting the music (${musicText})`;
        }
        setProgress(seconds / totalSeconds, shown);
    };
    const onProgress = (msg) => {
        if (msg.add) done[msg.step] = Math.min(1, done[msg.step] + msg.add / WORK[msg.step].items);
        else if (msg.done !== undefined) done[msg.step] = msg.done;
        if (["frames", "pages", "sprites", "sfx"].includes(msg.step)) texturesText = msg.text;
        update(msg.text);
    };
    const onMessage = (msg) => {
        if (msg.type === "progress") onProgress(msg);
        else if (msg.type === "file") addFile(msg.path, msg.data);
        else if (msg.type === "log") {
            els.errorLog.textContent = msg.lines.join("\n");
            els.errorLog.hidden = false;
        }
    };

    const workers = [];
    try {
        const prepared = await runTask({task: "prepare", gameZip, patchZip: els.patchLocal.checked ? patchZip : null}, onMessage, workers);
        for (const step of ["read", "patchFiles", "patch", "copy"]) done[step] = 1;
        texturesText = "Converting the textures";
        update();

        // One worker for the textures and sound effects, the rest share the music tracks.
        const musicWorkers = Math.max(1, Math.min(6, (navigator.hardwareConcurrency || 4) - 2));
        const tasks = [
            runTask({task: "convert", inputs: prepared.inputs, args: ["--parts", "textures,sfx"]}, onMessage, workers).then(() => {
                for (const step of ["frames", "pages", "sprites", "sfx"]) done[step] = 1;
                texturesText = null;
                update();
            }),
        ];
        for (let i = 0; i < musicWorkers; i++) {
            const args = ["--parts", "music", "--music-shard", `${i}/${musicWorkers}`];
            tasks.push(runTask({task: "convert", inputs: prepared.inputs, args}, onMessage, workers));
        }
        await Promise.all(tasks);

        for (const [path, base64] of Object.entries(SD_FILES)) addFile(path, Uint8Array.from(atob(base64), (c) => c.charCodeAt(0)));
        zip.end();
        setProgress(1, "Saving the zip");
        const blob = await output.finish();
        showDone(output.name, blob, {bytes: zipBytes, seconds: (Date.now() - started) / 1000});
    } catch (e) {
        output.abort();
        showError(e.userFacing ? e.message : `Something went wrong: ${e.message}`);
    } finally {
        for (const worker of workers) worker.terminate();
        clearInterval(clock);
        running = false;
        window.removeEventListener("beforeunload", warnOnLeave);
        updateStart();
    }
});

function showError(message) {
    els.errorText.textContent = message;
    els.working.hidden = true;
    els.setup.hidden = false;
    els.error.hidden = false;
}

els.retry.addEventListener("click", () => {
    els.error.hidden = true;
    els.errorLog.hidden = true;
});

function showDone(name, blob, msg) {
    els.working.hidden = true;
    els.setup.hidden = true;
    els.done.hidden = false;
    const summary = `${formatSize(msg.bytes)}, made in ${formatTime(msg.seconds)}`;
    if (blob) {
        const url = URL.createObjectURL(blob);
        els.download.href = url;
        els.download.download = name;
        els.download.hidden = false;
        els.download.click();
        els.saved.textContent = `Your SD card zip is ready (${summary}). If the download didn't start, use the button.`;
    } else {
        els.download.hidden = true;
        els.saved.textContent = `Saved ${name} (${summary}).`;
    }
}

// ===[ CIA QR code ]===

(function drawQr() {
    els.ciaLink.href = CIA_URL;
    els.ciaLink.textContent = CIA_URL;
    const qr = qrcode(0, "M");
    qr.addData(CIA_URL);
    qr.make();
    const count = qr.getModuleCount();
    const margin = 4;
    const scale = 6;
    const canvas = els.qr;
    canvas.width = canvas.height = (count + 2 * margin) * scale;
    const ctx = canvas.getContext("2d");
    ctx.fillStyle = "#fff";
    ctx.fillRect(0, 0, canvas.width, canvas.height);
    ctx.fillStyle = "#000";
    for (let y = 0; y < count; y++) {
        for (let x = 0; x < count; x++) {
            if (qr.isDark(y, x)) ctx.fillRect((x + margin) * scale, (y + margin) * scale, scale, scale);
        }
    }
})();

for (const el of document.querySelectorAll("[data-patch-version]")) el.textContent = PATCH_VERSION;
for (const el of document.querySelectorAll("[data-sd-rev]")) el.textContent = SD_DATA_REV;
updateStart();
