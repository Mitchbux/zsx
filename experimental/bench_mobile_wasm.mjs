// Whole-decoder wall time in Node/WASM, not physical mobile-device timing.
import { readFileSync, writeFileSync } from "node:fs";
import { performance } from "node:perf_hooks";
import factory from "../artifacts/zsv-browser-player/public/decoder/zsv_decoder.js";

const base = new URL("../generated-artifacts/mobile-demo/", import.meta.url);
const m = await factory({
  wasmBinary: readFileSync(new URL("../artifacts/zsv-browser-player/public/decoder/zsv_decoder.wasm", import.meta.url)),
});
const results = [];
for (const profile of ["size-first", "byte16", "byte20", "candidate16", "candidate20"]) {
  const bytes = readFileSync(new URL(`${profile}.zsv`, base));
  const times = [];
  for (let run = 0; run < 3; run++) {
    const input = m._malloc(bytes.length);
    if (!input) throw Error("Input allocation failed");
    m.HEAPU8.set(bytes, input);
    const slots = Array.from({ length: 9 }, () => m._malloc(4));
    if (slots.some(p => !p)) throw Error("Scratch allocation failed");
    let h = 0;
    let frames = 0, samples = 0;
    const start = performance.now();
    try {
      h = m._zsv_open(input, bytes.length);
      if (!h) throw Error("Open failed");
      for (;;) {
        const status = m._zsv_next(h, ...slots);
        if (!status) break;
        if (status < 0) throw Error(m.UTF8ToString(m._zsv_error(h)));
        if (m.HEAP32[slots[0] >> 2] === 1) frames++;
        else samples += m.HEAP32[slots[5] >> 2];
        m._zsv_free(m.HEAP32[slots[7] >> 2]);
      }
      times.push((performance.now() - start) / 1000);
      if (frames !== 450 || samples !== 1323000) throw Error(`Unexpected counts ${frames}/${samples}`);
    } finally {
      if (h) m._zsv_close(h);
      slots.forEach(p => m._free(p));
      m._free(input);
    }
  }
  const result = { profile, seconds: times, median: [...times].sort((a, b) => a - b)[1] };
  results.push(result);
  console.log(result);
}
writeFileSync(new URL("wasm-results.json", base), JSON.stringify(results, null, 2) + "\n");
