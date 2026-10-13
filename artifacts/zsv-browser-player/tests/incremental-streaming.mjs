import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { createContext, Script } from "node:vm";
import ts from "typescript";
import factory from "../public/decoder/zsv_decoder.js";

const bytes = readFileSync(new URL("../public/sample/snowboard-gray-25fps.zsv", import.meta.url));
const m = await factory({ wasmBinary: readFileSync(new URL("../public/decoder/zsv_decoder.wasm", import.meta.url)) });
let opens = 0;
const ccall = m.ccall;
m.ccall = (...args) => { if (args[0] === "zsv_open_stream") opens++; return ccall(...args); };
const helper = readFileSync(new URL("../src/workers/incremental-source.ts", import.meta.url), "utf8").replace("export class", "class");
const source = readFileSync(new URL("../src/workers/zsv-worker.ts", import.meta.url), "utf8")
  .replace('import { IncrementalSource } from "./incremental-source";', "")
  .replace(/const module = \(\) => modulePromise \|\|= \(async \(\) => \{[\s\S]*?\}\)\(\);/,
    "const module = () => Promise.resolve(globalThis.__decoder);");
const js = ts.transpileModule(helper + source, { compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.None } }).outputText;
const messages = [];
const self = { location: { origin: "http://test" }, postMessage: x => messages.push(x) };
const context = createContext({ console, self, postMessage: self.postMessage, __decoder: m,
  Blob, URL, Uint8Array, ArrayBuffer, setTimeout, clearTimeout });
new Script(js).runInContext(context);
async function next(type) {
  const start = Date.now();
  while (Date.now() - start < 120000) {
    const error = messages.find(m => m.type === "error");
    if (error) throw Error(error.message);
    const index = messages.findIndex(m => m.type === type);
    if (index >= 0) return messages.splice(index, 1)[0];
    await new Promise(r => setTimeout(r, 5));
  }
  throw Error(`Timed out: ${type}`);
}
class ChunkedBlob extends Blob {
  stream() {
    let offset = 0;
    return new ReadableStream({
      pull(controller) {
        if (offset === bytes.length) { controller.close(); return; }
        const end = Math.min(offset + 16384, bytes.length);
        controller.enqueue(new Uint8Array(bytes.subarray(offset, end)));
        offset = end;
      },
    });
  }
}
self.onmessage({ data: { type: "stream", file: new ChunkedBlob([bytes]), name: "test.zsv" } });
const metadata = await next("metadata");
assert.equal(metadata.fps, 25);
assert.equal(metadata.streaming, true);
assert.equal(metadata.durationKnown, false);
assert.ok(new Script("state.source.bytes").runInContext(context) < bytes.length,
  "metadata must arrive before the whole file is read");
self.onmessage({ data: { type: "seek", time: 0, epoch: 1 } });
let frames = 0, samples = 0, gray = 0;
for (;;) {
  const batch = await next("batch");
  frames += batch.frames.length;
  gray += batch.frames.filter(f => f.grayscale).length;
  samples += batch.audio.reduce((n, a) => n + a.samples, 0);
  if (batch.eof) { assert.equal(batch.duration, 30); break; }
  self.onmessage({ data: { type: "pull", epoch: 1 } });
}
assert.equal(opens, 1, "no validation pass or initial decode replay");
assert.equal(frames, 750);
assert.equal(gray, 750);
assert.equal(samples, 1323000);
self.onmessage({ data: { type: "seek", time: 28, epoch: 2 } });
assert.equal((await next("batch")).epoch, 2);
console.log("Incremental streaming passed: early metadata, single pass, 750 grayscale frames, stereo audio, cached seek.");
