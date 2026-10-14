// The SD-card build's workers. The page (app.js) starts one for each task:
//   "prepare": unzip AM2R 1.1, check it, apply the community patch (xdelta3); sends back the game files for the zip,
//              and data.win and the music as blobs for the converters.
//   "convert": run n3ds-preprocess (WebAssembly) on part of the work (--parts / --music-shard), sending each finished
//              file back as it's written.
// tools/web/bundle.mjs puts fflate, the two WebAssembly modules (createXdelta, createPreprocess) and PATCH_MANIFEST in
// front of this file.
/* global fflate, createXdelta, createPreprocess, PATCH_MANIFEST */
"use strict";

const PATCH_BASE = `https://raw.githubusercontent.com/${PATCH_MANIFEST.repo}/${PATCH_MANIFEST.commit}/`;

let lastPost = 0;
function progress(fields, force) {
    const now = Date.now();
    if (!force && !fields.add && now - lastPost < 100) return;
    lastPost = now;
    postMessage({type: "progress", ...fields});
}

async function sha256(bytes) {
    const digest = await crypto.subtle.digest("SHA-256", bytes);
    return Array.from(new Uint8Array(digest), (b) => b.toString(16).padStart(2, "0")).join("");
}

function fail(message) {
    throw Object.assign(new Error(message), {userFacing: true});
}

// Sends a file to the page for the zip; the bytes move over without a copy when they own their buffer.
function sendFile(path, bytes) {
    const own = bytes.byteOffset === 0 && bytes.byteLength === bytes.buffer.byteLength;
    const data = own ? bytes : bytes.slice();
    postMessage({type: "file", path, data}, [data.buffer]);
}

// ===[ prepare ]===

function readGameZip(buffer) {
    let entries;
    try {
        entries = fflate.unzipSync(new Uint8Array(buffer));
    } catch (e) {
        fail("That file isn't a zip archive that can be read here. Pick the AM2R 1.1 zip itself (AM2R_11.zip).");
    }
    // The game may sit in a folder inside the zip: its root is wherever data.win is.
    const dataWinPath = Object.keys(entries).find((p) => /(^|\/)data\.win$/i.test(p) && !p.startsWith("__MACOSX/"));
    if (!dataWinPath) fail("There's no data.win in that zip. Pick the AM2R 1.1 zip (AM2R_11.zip).");
    const root = dataWinPath.slice(0, dataWinPath.length - "data.win".length);
    const files = new Map();
    for (const [path, bytes] of Object.entries(entries)) {
        if (!path.startsWith(root) || path.endsWith("/")) continue;
        files.set(path.slice(root.length), bytes);
    }
    return files;
}

async function fetchPatchFile(entry, onBytes) {
    const url = PATCH_BASE + entry.path.split("/").map(encodeURIComponent).join("/");
    let response;
    try {
        response = await fetch(url);
    } catch (e) {
        fail("Couldn't download the AM2R Community Updates patch (no connection?). Connect to the internet, or pick the patch zip under \"I have the patch files\".");
    }
    if (!response.ok) fail(`Downloading the patch failed (${response.status} for ${entry.path}). Try again, or pick the patch zip under "I have the patch files".`);
    const reader = response.body.getReader();
    const bytes = new Uint8Array(entry.size);
    let offset = 0;
    for (;;) {
        const {done, value} = await reader.read();
        if (done) break;
        if (offset + value.length > bytes.length) fail(`The downloaded patch file ${entry.path} isn't the expected one.`);
        bytes.set(value, offset);
        offset += value.length;
        onBytes(value.length);
    }
    if (offset !== entry.size) fail(`The downloaded patch file ${entry.path} is incomplete. Try again.`);
    return bytes;
}

// Drops the CR of each CRLF: a copy of the patch checked out on Windows may have them, the published files don't.
function withoutCr(bytes) {
    const out = new Uint8Array(bytes.length);
    let n = 0;
    for (let i = 0; i < bytes.length; i++) {
        if (bytes[i] === 13 && bytes[i + 1] === 10) continue;
        out[n++] = bytes[i];
    }
    return out.slice(0, n);
}

// The patch's data.xdelta and files_to_copy, checked against the manifest: from the Community Developers' repository
// at the pinned commit, or from a zip of it the user picked (GitHub's "Download ZIP" of AM2R-Autopatcher-Windows).
async function loadPatch(patchZip) {
    const wanted = [{...PATCH_MANIFEST.dataPatch, key: "xdelta"}].concat(
        PATCH_MANIFEST.filesToCopy.map((f) => ({...f, path: "data/files_to_copy/" + f.path, key: f.path})),
    );
    const totalBytes = wanted.reduce((sum, f) => sum + f.size, 0);
    const result = new Map();
    const version = PATCH_MANIFEST.version;

    if (patchZip) {
        progress({step: "patchFiles", done: 0, text: "Reading the patch zip"}, true);
        let entries;
        try {
            entries = fflate.unzipSync(new Uint8Array(await patchZip.arrayBuffer()));
        } catch (e) {
            fail("The patch file isn't a zip archive that can be read here.");
        }
        const xdeltaPath = Object.keys(entries).find((p) => /(^|\/)data\/data\.xdelta$/.test(p));
        if (!xdeltaPath) fail("That zip doesn't have the patch in it (data/data.xdelta). Pick a zip of AM2R-Autopatcher-Windows.");
        const root = xdeltaPath.slice(0, xdeltaPath.length - "data/data.xdelta".length);
        for (const f of wanted) {
            let bytes = entries[root + f.path];
            if (!bytes) fail(`The patch zip is missing ${f.path}. It must be version ${version} of AM2R-Autopatcher-Windows.`);
            if (f.key !== "xdelta" && (await sha256(bytes)) !== f.sha256) bytes = withoutCr(bytes);
            result.set(f.key, bytes);
        }
    } else {
        const text = `Downloading the AM2R ${version} Community Updates patch`;
        progress({step: "patchFiles", done: 0, text}, true);
        let received = 0;
        const onBytes = (n) => {
            received += n;
            progress({step: "patchFiles", done: received / totalBytes, text: `${text} (${(received / 1e6).toFixed(1)} of ${(totalBytes / 1e6).toFixed(1)} MB)`});
        };
        // A few at a time: one large file and about a hundred small ones.
        const queue = wanted.slice();
        await Promise.all(Array.from({length: 6}, async () => {
            while (queue.length > 0) {
                const f = queue.shift();
                result.set(f.key, await fetchPatchFile(f, onBytes));
            }
        }));
    }

    progress({step: "patchFiles", done: 1, text: "Checking the patch files"}, true);
    for (const f of wanted) {
        if ((await sha256(result.get(f.key))) !== f.sha256) {
            fail(`The patch file ${f.path} doesn't match AM2R ${version}'s. ${patchZip ? "Use a zip of AM2R-Autopatcher-Windows at the version this page was made for, or let the page download it." : "Try again."}`);
        }
    }
    return result;
}

async function applyPatch(sourceBytes, patchBytes) {
    const xd = await createXdelta();
    const src = xd._malloc(sourceBytes.length);
    const pat = xd._malloc(patchBytes.length);
    const outPtr = xd._malloc(4);
    xd.HEAPU8.set(sourceBytes, src);
    xd.HEAPU8.set(patchBytes, pat);
    const capacity = PATCH_MANIFEST.patchedDataWinSize + 16 * 1024 * 1024;
    const size = Number(xd._xd3wasmDecode(pat, patchBytes.length, src, sourceBytes.length, capacity, outPtr));
    if (size < 0) fail("Patching data.win failed. The patch only applies to AM2R 1.1.");
    const out = xd.getValue(outPtr, "i32");
    return xd.HEAPU8.slice(out, out + size);
}

// make_sd.sh: the Windows binaries and the music (converted to audio/) stay off the card.
const SKIPPED = /\.(exe|dll|ogg|wav|mp3)$/;

async function prepare({gameZip, patchZip}) {
    progress({step: "read", done: 0, text: "Reading the AM2R 1.1 zip"}, true);
    const game = readGameZip(await gameZip.arrayBuffer());
    progress({step: "read", done: 0.5, text: "Checking data.win"}, true);
    const sourceHash = await sha256(game.get("data.win"));
    const version = PATCH_MANIFEST.version;
    if (sourceHash === PATCH_MANIFEST.patchedDataWinSha256) {
        fail(`That zip already has AM2R ${version} in it. Pick the original AM2R 1.1 zip (AM2R_11.zip).`);
    }
    if (sourceHash !== PATCH_MANIFEST.sourceDataWinSha256) {
        fail("That zip's data.win isn't AM2R 1.1's. The patch only applies to the original AM2R 1.1 (AM2R_11.zip).");
    }

    const patch = await loadPatch(patchZip);

    progress({step: "patch", done: 0, text: `Patching data.win to AM2R ${version}`}, true);
    const dataWin = await applyPatch(game.get("data.win"), patch.get("xdelta"));
    if ((await sha256(dataWin)) !== PATCH_MANIFEST.patchedDataWinSha256) fail("Patching data.win gave the wrong result.");
    game.set("data.win", dataWin);
    for (const f of PATCH_MANIFEST.filesToCopy) game.set(f.path, patch.get(f.path));

    // The converters read data.win and the music straight from blobs (WORKERFS). They're made before the files go to
    // the page, which takes their bytes.
    const inputs = [{name: "data.win", data: new Blob([dataWin])}];
    for (const [path, bytes] of game) {
        if (!path.includes("/") && path.toLowerCase().endsWith(".ogg")) inputs.push({name: path, data: new Blob([bytes])});
    }

    progress({step: "copy", done: 0, text: "Copying the game files"}, true);
    for (const [path, bytes] of [...game].sort(([a], [b]) => (a < b ? -1 : 1))) {
        if (SKIPPED.test(path.toLowerCase()) || path.startsWith("gfx/") || path.startsWith("audio/")) continue;
        sendFile(path, bytes);
    }
    postMessage({type: "prepared", inputs});
}

// ===[ convert ]===

function onConverterLine(line) {
    let m;
    if ((m = /^n3ds-preprocess: sprite frame (\d+)\/(\d+)(?: \((.*)\))?/.exec(line))) {
        progress({step: "frames", add: 1, text: `Converting sprites${m[3] ? " (" + m[3] + ")" : ""}`});
    } else if ((m = /^Wrote packed atlas page (\d+)/.exec(line))) {
        progress({step: "pages", add: 1, text: `Converting texture pages (page ${Number(m[1]) + 1})`});
    } else if ((m = /^n3ds-preprocess: sprite (\d+) -> \S+ \[(\w+)\](?: \((.*)\))?/.exec(line))) {
        progress({step: "sprites", add: 1, text: `Packing sprites into 3DS textures${m[3] ? " (" + m[3] + ")" : ""}`});
    } else if ((m = /^n3ds-preprocess: music \d+ -> .*\/([^/]+)\.bcwav/.exec(line))) {
        progress({step: "music", add: 1, text: `Converting the music (${m[1]})`});
    } else if (/^n3ds-preprocess: processing audio/.test(line)) {
        progress({step: "sfx", done: 0, text: "Converting the sound effects"}, true);
    }
}

async function convert({inputs, args}) {
    const log = [];
    const module = await createPreprocess({
        print: (line) => log.push(line),
        printErr: (line) => {
            log.push(line);
            if (log.length > 400) log.splice(0, log.length - 200);
            onConverterLine(line);
        },
        // The finished files leave the in-memory file system at each checkpoint (after the textures, after each music
        // track, at the end), so it never holds the whole output.
        onCheckpoint: () => drainOutput(module.FS, "/out", ""),
    });
    const FS = module.FS;
    FS.mkdir("/in");
    FS.mount(module.WORKERFS, {blobs: inputs}, "/in");
    FS.mkdir("/out");
    let rc;
    try {
        rc = module.callMain(["/in/data.win", "/out", ...args]);
    } catch (e) {
        rc = e && e.status !== undefined ? e.status : -1;
        if (rc === -1) log.push(String(e && e.stack ? e.stack : e));
    }
    if (rc !== 0) {
        postMessage({type: "log", lines: log.slice(-40)});
        fail("The 3DS conversion failed. The details are below; please report them.");
    }
    postMessage({type: "converted"});
}

function drainOutput(FS, dir, rel) {
    for (const entry of FS.readdir(dir).sort()) {
        if (entry === "." || entry === "..") continue;
        const path = dir + "/" + entry;
        if (FS.isDir(FS.stat(path).mode)) {
            drainOutput(FS, path, rel + entry + "/");
            continue;
        }
        sendFile(rel + entry, FS.readFile(path));
        FS.unlink(path);
    }
}

onmessage = async (event) => {
    try {
        if (event.data.task === "prepare") await prepare(event.data);
        else await convert(event.data);
    } catch (e) {
        postMessage({type: "error", message: e.userFacing ? e.message : `Something went wrong: ${e && e.message ? e.message : e}`});
    }
};
