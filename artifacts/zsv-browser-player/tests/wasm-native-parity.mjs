import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { execFileSync, spawnSync } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import ModuleFactory from "../public/decoder/zsv_decoder.js";

const golden = {
  video: "ed995edd4dfbb07e8ec1b391dc0efc9975f56c6e4b6ae9bdafb1f00fada1ba5d",
  audio: "4a99f4d2793d590b0eb62d085a897de15cd93475e23064038a34b54be28c409f",
};

const module = await ModuleFactory({
  wasmBinary: readFileSync(new URL("../public/decoder/zsv_decoder.wasm", import.meta.url)),
});
const sample = new Uint8Array(
  readFileSync(process.env.ZSV_PARITY_SAMPLE ?? new URL("../public/sample/demo-small-30s.zsv", import.meta.url)),
);
const nativeDirectory = mkdtempSync(join(tmpdir(), "zsv-native-parity-"));
const nativeVideo = join(nativeDirectory, "video.yuv");
const nativeAudio = join(nativeDirectory, "audio.f32");
const nativeFfmpeg = new URL("../../../ffmpeg-zsx/bin/ffmpeg", import.meta.url).pathname;
const samplePath = process.env.ZSV_PARITY_SAMPLE ?? new URL("../public/sample/demo-small-30s.zsv", import.meta.url).pathname;
try {
  execFileSync(nativeFfmpeg, [
    "-v", "error", "-xerror", "-threads", "1", "-i", samplePath,
    "-map", "0:v:0", "-vsync", "0", "-f", "rawvideo", nativeVideo,
    "-map", "0:a:0", "-c:a", "pcm_f32le", "-f", "f32le", nativeAudio,
  ]);
} catch (error) {
  rmSync(nativeDirectory, { recursive: true, force: true });
  throw error;
}
const native = {
  video: createHash("sha256").update(readFileSync(nativeVideo)).digest("hex"),
  audio: createHash("sha256").update(readFileSync(nativeAudio)).digest("hex"),
};
rmSync(nativeDirectory, { recursive: true, force: true });
const input = module._malloc(sample.byteLength);
module.HEAPU8.set(sample, input);
const handle = module._zsv_open(input, sample.byteLength);
if (!handle) throw new Error("WASM decoder rejected the bundled sample");

const slots = Array.from({ length: 9 }, () => module._malloc(4));
const [kind, width, height, channels, rate, samples, pts, output, outputSize] = slots;
const video = createHash("sha256");
const audio = createHash("sha256");
let frameCount = 0;
let audioSampleCount = 0;

for (;;) {
  const result = module._zsv_next(
    handle,
    kind,
    width,
    height,
    channels,
    rate,
    samples,
    pts,
    output,
    outputSize,
  );
  if (result === 0) break;
  if (result < 0) {
    throw new Error(module.UTF8ToString(module._zsv_error(handle)));
  }
  const pointer = module.HEAP32[output >> 2];
  const size = module.HEAP32[outputSize >> 2];
  const bytes = Buffer.from(module.HEAPU8.slice(pointer, pointer + size));
  if (module.HEAP32[kind >> 2] === 1) {
    video.update(bytes);
    frameCount++;
  } else {
    audio.update(bytes);
    audioSampleCount += module.HEAP32[samples >> 2];
  }
  module._zsv_free(pointer);
}

module._zsv_close(handle);
module._free(input);

const actual = { video: video.digest("hex"), audio: audio.digest("hex") };
if (!process.env.ZSV_PARITY_SAMPLE && (native.video !== golden.video || native.audio !== golden.audio)) {
  throw new Error(`native decoder changed from the sample golden hashes: ${JSON.stringify(native)}`);
}
if (actual.video !== native.video || actual.audio !== native.audio) {
  throw new Error(`native/WASM hash mismatch: ${JSON.stringify({ native, wasm: actual })}`);
}
if (frameCount !== Number(process.env.ZSV_EXPECT_FRAMES ?? 150) ||
    audioSampleCount !== Number(process.env.ZSV_EXPECT_AUDIO_SAMPLES ?? 240000)) {
  throw new Error(`unexpected decoded lengths: ${frameCount} frames, ${audioSampleCount} samples`);
}

const malformed = module._malloc(32);
module.HEAPU8.fill(0xa5, malformed, malformed + 32);
const malformedHandle = module._zsv_open(malformed, 32);
module._free(malformed);
if (malformedHandle) {
  module._zsv_close(malformedHandle);
  throw new Error("malformed input unexpectedly opened");
}

// Use demuxed packet positions, not a magic-string search that could hit
// codec extradata or coincidental compressed bytes.
const probe = JSON.parse(execFileSync(
  new URL("../../../ffmpeg-zsx/bin/ffprobe", import.meta.url).pathname,
  ["-v", "error", "-show_packets", "-show_entries", "packet=codec_type,pos,size",
    "-of", "json", samplePath],
  { encoding: "utf8", timeout: 30000 },
));
const packets = probe.packets.map((packet) => ({
  type: packet.codec_type, start: Number(packet.pos), size: Number(packet.size),
}));
const sampleBuffer = Buffer.from(sample);
for (const packet of packets) {
  assert(Number.isSafeInteger(packet.start) && packet.start >= 0);
  assert(packet.size > 44 && packet.start + packet.size <= sample.length);
  assert.equal(sampleBuffer.toString("ascii", packet.start, packet.start + 4),
    packet.type === "audio" ? "ZSXM" : "ZSXD");
  if (packet.type === "audio") {
    assert.equal(sampleBuffer[packet.start + 4], 2, "expected MDCT-ZSX v2");
    assert.equal(sampleBuffer.readUInt32LE(packet.start + 36) + 44, packet.size);
  }
}

function assertWasmRejects(bytes, label) {
  const pointer = module._malloc(bytes.length);
  let decoder = 0;
  try {
    module.HEAPU8.set(bytes, pointer);
    decoder = module._zsv_open(pointer, bytes.length);
    if (!decoder) return; // Explicit open failure is also a rejection.
    for (let count = 0; count <= packets.length; count++) {
      const result = module._zsv_next(
        decoder, kind, width, height, channels, rate, samples, pts, output, outputSize,
      );
      if (result < 0) {
        assert.match(module.UTF8ToString(module._zsv_error(decoder)), /\S/,
          `${label}: WASM must report an error, not just stop decoding`);
        return;
      }
      assert.notEqual(result, 0, `${label}: WASM accepted damaged input as successful EOF`);
      module._zsv_free(module.HEAP32[output >> 2]);
    }
    assert.fail(`${label}: WASM never terminated`);
  } finally {
    if (decoder) module._zsv_close(decoder);
    module._free(pointer);
  }
}

function assertNativeRejects(bytes, label) {
  const directory = mkdtempSync(join(tmpdir(), "zsv-damaged-parity-"));
  try {
    const path = join(directory, "damaged.zsv");
    writeFileSync(path, bytes);
    const result = spawnSync(nativeFfmpeg, [
      "-v", "error", "-xerror", "-threads", "1", "-i", path,
      "-map", "0:v:0", "-f", "rawvideo", join(directory, "video.yuv"),
      "-map", "0:a:0", "-c:a", "pcm_f32le", "-f", "f32le", join(directory, "audio.f32"),
    ], { encoding: "utf8", timeout: 120000 });
    assert.ifError(result.error);
    assert.equal(result.signal, null, `${label}: native decoder crashed`);
    assert(result.status > 0, `${label}: native accepted damaged input as successful EOF`);
    assert.match(result.stderr, /invalid data|corrupt|truncat|end of file|EOF/i,
      `${label}: expected an explicit media error, got: ${result.stderr}`);
  } finally {
    rmSync(directory, { recursive: true, force: true });
  }
}

const damagedCases = [["truncated container header", sample.subarray(0, 32)]];
// Bypass the packet checksum to prove the separate reconstruction CRC rejects
// a plausible, fully decoded frame with a mismatched declared checksum.
{
  const packet = packets.filter((p) => p.type === "video").at(-1);
  const corrupt = Buffer.from(sample);
  corrupt[packet.start + 24] ^= 1;
  corrupt.fill(0, packet.start + 28, packet.start + 32);
  let crc = 0;
  for (const byte of corrupt.subarray(packet.start, packet.start + packet.size)) {
    crc ^= byte;
    for (let bit = 0; bit < 8; bit++) crc = (crc >>> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
  }
  corrupt.writeUInt32LE(crc >>> 0, packet.start + 28);
  damagedCases.push(["late reconstructed-frame CRC mismatch (valid packet CRC)", corrupt]);
}
for (const type of ["video", "audio"]) {
  const streamPackets = packets.filter((packet) => packet.type === type);
  assert(streamPackets.length > 1, `expected multiple ${type} packets`);
  // Test late damage too: valid earlier frames must not mask the error.
  for (const [position, packet] of [
    ["first", streamPackets[0]], ["last", streamPackets.at(-1)],
  ]) {
    const corrupt = Uint8Array.from(sample);
    // The checksum is at +36 for ZSXD, +40 for the 44-byte ZSXM header.
    corrupt[packet.start + (type === "audio" ? 40 : 36)] ^= 0x40;
    damagedCases.push([`${position} ${type} packet CRC corruption`, corrupt]);
  }
  const last = streamPackets.at(-1);
  const headerSize = type === "audio" ? 44 : 40;
  for (const [part, length] of [
    ["header", last.start + 12],
    ["payload", last.start + headerSize + Math.floor((last.size - headerSize) / 2)],
    ["final byte", last.start + last.size - 1],
  ]) {
    damagedCases.push([`truncated last ${type} packet ${part}`, sample.subarray(0, length)]);
  }
}
try {
  for (const [label, bytes] of damagedCases) {
    assertNativeRejects(bytes, label);
    assertWasmRejects(bytes, label);
    console.log(`Rejected in native and WASM: ${label}`);
  }
} finally {
  for (const slot of slots) module._free(slot);
}

console.log(
  `WASM parity passed: ${frameCount} frames, ${audioSampleCount} samples, video ${actual.video}, audio ${actual.audio}`,
);