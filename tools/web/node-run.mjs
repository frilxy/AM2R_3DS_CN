// Runs the WebAssembly converter in Node, for checking it against the native build:
//   node tools/web/node-run.mjs <build-web>/n3ds-preprocess.js <game dir with data.win> <out dir>
import fs from "node:fs";
import path from "node:path";
import { createRequire } from "node:module";

const [moduleJs, gameDir, outDir] = process.argv.slice(2);
const createPreprocess = createRequire(import.meta.url)(path.resolve(moduleJs));

let peak = 0;
const note = () => { peak = Math.max(peak, process.memoryUsage().rss); };
const Module = await createPreprocess({
  print: (s) => {},
  printErr: (s) => { if (!s.startsWith("n3ds-preprocess: sprite frame")) {} },
  onCheckpoint(stage) {
    note();
    const FS = Module.FS;
    let count = 0, bytes = 0;
    const drain = (dir) => {
      for (const name of FS.readdir(dir)) {
        if (name === "." || name === "..") continue;
        const p = dir + "/" + name;
        if (FS.isDir(FS.stat(p).mode)) { drain(p); continue; }
        const data = FS.readFile(p);
        const dst = path.join(outDir, p.slice("/out/".length));
        fs.mkdirSync(path.dirname(dst), { recursive: true });
        fs.writeFileSync(dst, data);
        FS.unlink(p);
        count++; bytes += data.length;
      }
    };
    drain("/out");
    console.log(`checkpoint ${stage}: ${count} files, ${(bytes / 1e6).toFixed(1)} MB, rss peak ${(peak / 1e6).toFixed(0)} MB`);
  },
});
const FS = Module.FS;
FS.mkdir("/in");
for (const name of fs.readdirSync(gameDir)) {
  if (name === "data.win" || name.toLowerCase().endsWith(".ogg")) FS.writeFile("/in/" + name, fs.readFileSync(path.join(gameDir, name)));
}
FS.mkdir("/out");
const timer = setInterval(note, 500);
const t0 = Date.now();
const rc = Module.callMain(["/in/data.win", "/out"]);
clearInterval(timer);
console.log(`exit ${rc} in ${((Date.now() - t0) / 1000).toFixed(1)} s, wasm heap ${(Module.HEAP8 ? Module.HEAP8.length / 1e6 : 0).toFixed(0)} MB`);
