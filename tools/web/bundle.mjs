// Puts the SD-card page together as one self-contained index.html, so it also works saved and opened from disk:
//   node tools/web/bundle.mjs <wasm build dir> <out.html>
// The worker (fflate, the two WebAssembly modules with their .wasm inlined, the patch manifest and web/worker.js) is
// embedded as base64 and started from blob: URLs.
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const repo = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const web = path.join(repo, "web");
const [wasmDir, outPath] = process.argv.slice(2);
const read = (p) => fs.readFileSync(p, "utf8");

const manifest = JSON.parse(read(path.join(web, "patch-manifest.json")));
const sdDataRev = read(path.join(repo, "tools/n3ds/sd_data_rev.txt")).trim();
// The files make_sd.sh adds: the SD data revision the CIA checks, and the 3DS control config (res/n3ds/am2r/sd).
const sdFiles = {"sd_data_rev.txt": fs.readFileSync(path.join(repo, "tools/n3ds/sd_data_rev.txt")).toString("base64")};
const seed = path.join(repo, "res/n3ds/am2r/sd");
for (const name of fs.readdirSync(seed, {recursive: true})) {
    const file = path.join(seed, name);
    if (fs.statSync(file).isFile()) sdFiles[name.split(path.sep).join("/")] = fs.readFileSync(file).toString("base64");
}

const worker = [
    read(path.join(web, "vendor/fflate.js")),
    read(path.join(wasmDir, "xdelta3.js")),
    read(path.join(wasmDir, "n3ds-preprocess.js")),
    `const PATCH_MANIFEST = ${JSON.stringify(manifest)};`,
    read(path.join(web, "worker.js")),
].join("\n;\n");

// The QR code installs this exact am2r.cia: the one from the newest Codeberg release when the page is built, so a saved
// copy of the page keeps giving a CIA that reads the SD data it makes. AM2R_CIA_URL overrides it.
const ciaRepo = process.env.AM2R_CIA_REPO || "NattensMadrigal/AM2R_3DS";
async function newestCiaUrl() {
    const response = await fetch(`https://codeberg.org/api/v1/repos/${ciaRepo}/releases?limit=20`);
    if (!response.ok) throw new Error(`Codeberg releases: HTTP ${response.status}`);
    for (const release of await response.json()) {
        const asset = !release.draft && release.assets.find((a) => a.name === "am2r.cia");
        if (asset) return asset.browser_download_url;
    }
    throw new Error(`no Codeberg release of ${ciaRepo} has an am2r.cia`);
}
const ciaUrl = process.env.AM2R_CIA_URL || (await newestCiaUrl());
const config = [
    `const WORKER_SOURCE_BASE64 = ${JSON.stringify(Buffer.from(worker).toString("base64"))};`,
    `const SD_FILES = ${JSON.stringify(sdFiles)};`,
    `const CIA_URL = ${JSON.stringify(ciaUrl)};`,
    `const CIA_REPO = ${JSON.stringify(ciaRepo)};`,
    `const PATCH_VERSION = ${JSON.stringify(manifest.version)};`,
    `const SD_DATA_REV = ${JSON.stringify(sdDataRev)};`,
].join("\n");

const inline = {
    "style.css": read(path.join(web, "style.css")),
    "config": config,
    "fflate.js": read(path.join(web, "vendor/fflate.js")),
    "qrcode.js": read(path.join(web, "vendor/qrcode.js")),
    "app.js": read(path.join(web, "app.js")),
};
let html = read(path.join(web, "index.html"));
for (const [name, text] of Object.entries(inline)) {
    if (/<\/(script|style)/i.test(text)) throw new Error(`${name} contains a closing tag and can't be inlined`);
    const marker = `/*INLINE ${name}*/`;
    if (!html.includes(marker)) throw new Error(`index.html has no ${marker}`);
    html = html.replace(marker, () => text);
}
fs.mkdirSync(path.dirname(path.resolve(outPath)), {recursive: true});
fs.writeFileSync(outPath, html);
console.log(`${outPath}: ${(html.length / 1e6).toFixed(2)} MB`);
