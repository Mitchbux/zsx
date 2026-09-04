#!/usr/bin/env node

const assert = require('assert');
const fs = require('fs');
const path = require('path');
const { ZsxRange, ZsxArchive, haxe_io_Bytes } = require('./zsx_lib.js');

function toHaxeBytes(buffer) {
  const bytes = haxe_io_Bytes.alloc(buffer.length);
  for (let i = 0; i < buffer.length; i++) bytes.set(i, buffer[i]);
  return bytes;
}

function toBuffer(bytes) {
  const output = Buffer.alloc(bytes.length);
  for (let i = 0; i < bytes.length; i++) output[i] = bytes.get(i);
  return output;
}

function assertBytes(name, actual, expected) {
  assert.strictEqual(actual.length, expected.length, `${name}: length mismatch`);
  assert.ok(actual.equals(expected), `${name}: byte mismatch`);
}

function assertFails(name, fn) {
  assert.throws(fn, undefined, `${name}: malformed data was accepted`);
}

function archiveRoundTrip(name, input) {
  const encoded = ZsxArchive.encode(toHaxeBytes(input));
  const decoded = toBuffer(ZsxArchive.decode(encoded));
  assertBytes(name, decoded, input);
  console.log(`${name.padEnd(24)} ${String(input.length).padStart(9)} -> ${String(encoded.length).padStart(9)} bytes`);
  return toBuffer(encoded);
}

const fixtureInput = Buffer.alloc(10000);
for (let i = 0; i < fixtureInput.length; i++) fixtureInput[i] = i % 31 === 0 ? 66 : 65;
const fixtureHex = fs.readFileSync(
  path.join(__dirname, 'fixtures', 'zsx-range-compressible-10k.zsx.hex'),
  'utf8'
).trim();
const fixtureChunk = Buffer.from(fixtureHex, 'hex');

const fixtureDecoded = toBuffer(ZsxRange.decodeChunk(toHaxeBytes(fixtureChunk)));
assertBytes('C fixture decode', fixtureDecoded, fixtureInput);
const fixtureEncoded = toBuffer(ZsxRange.encodeChunk(toHaxeBytes(fixtureInput)));
assertBytes('C fixture encode', fixtureEncoded, fixtureChunk);
console.log(`C chunk compatibility    ${fixtureInput.length} -> ${fixtureChunk.length} bytes`);

const c64Archive = Buffer.alloc(8 + fixtureChunk.length);
c64Archive.writeBigUInt64LE(BigInt(fixtureChunk.length), 0);
fixtureChunk.copy(c64Archive, 8);
assertBytes('C 64-bit archive', toBuffer(ZsxArchive.decode(toHaxeBytes(c64Archive))), fixtureInput);

const c32Archive = Buffer.alloc(4 + fixtureChunk.length);
c32Archive.writeUInt32LE(fixtureChunk.length, 0);
fixtureChunk.copy(c32Archive, 4);
assertBytes('C 32-bit archive', toBuffer(ZsxArchive.decode(toHaxeBytes(c32Archive))), fixtureInput);

const emptyArchive = archiveRoundTrip('empty archive', Buffer.alloc(0));
const random = Buffer.alloc(4096);
let state = 0x12345678;
for (let i = 0; i < random.length; i++) {
  state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
  random[i] = state >>> 24;
}
const rawArchive = archiveRoundTrip('raw fallback', random);
assert.strictEqual(rawArchive.readInt32LE(16 + 4 + 8), -1, 'raw chunk must use literal count -1');

const repeated = Buffer.alloc(65536);
for (let i = 0; i < repeated.length; i++) repeated[i] = i % 17 === 0 ? 0x42 : 0x41;
const compressedArchive = archiveRoundTrip('compressed archive', repeated);
assert.ok(compressedArchive.length < repeated.length, 'compressible input did not shrink');

const multi = Buffer.alloc(0xC00000 + 257);
for (let i = 0; i < multi.length; i++) multi[i] = i % 31 === 0 ? 0x42 : 0x41;
const multiArchive = archiveRoundTrip('multi-chunk archive', multi);
assert.strictEqual(multiArchive.readUInt32LE(8), 2, 'multi-chunk archive must contain two chunks');
const packedChunks = [];
let packedAt = 16;
for (let i = 0; i < multiArchive.readUInt32LE(8); i++) {
  const length = multiArchive.readUInt32LE(packedAt);
  packedAt += 4;
  packedChunks.push(toHaxeBytes(multiArchive.subarray(packedAt, packedAt + length)));
  packedAt += length;
}
const repacked = toBuffer(ZsxArchive.packChunks(multi.length, packedChunks));
assertBytes('parallel chunk order', repacked, multiArchive);

assertFails('truncated archive header', () => ZsxArchive.decode(toHaxeBytes(emptyArchive.subarray(0, 15))));
assertFails('bad archive magic', () => {
  const broken = Buffer.from(emptyArchive);
  broken[0] ^= 0xff;
  ZsxArchive.decode(toHaxeBytes(broken));
});
assertFails('truncated archive chunk', () => {
  ZsxArchive.decode(toHaxeBytes(compressedArchive.subarray(0, compressedArchive.length - 1)));
});
assertFails('trailing archive data', () => {
  ZsxArchive.decode(toHaxeBytes(Buffer.concat([compressedArchive, Buffer.from([0])])));
});
assertFails('truncated range chunk', () => {
  ZsxRange.decodeChunk(toHaxeBytes(fixtureChunk.subarray(0, fixtureChunk.length - 1)));
});
for (let padding = 1; padding <= 5; padding++) {
  assertFails(`range chunk with ${padding} trailing byte(s)`, () => {
    const padded = Buffer.concat([fixtureChunk, Buffer.alloc(padding)]);
    padded.writeUInt32LE(fixtureChunk.readUInt32LE(4) + padding, 4);
    ZsxRange.decodeChunk(toHaxeBytes(padded));
  });
}
assertFails('oversized declared output', () => {
  const oversized = Buffer.alloc(16);
  oversized.write('ZSX1', 0, 'ascii');
  oversized.writeUInt32LE(0x10000001, 4);
  oversized.writeUInt32LE(22, 8);
  ZsxArchive.decode(toHaxeBytes(oversized));
});

console.log('PASS: C compatibility/framing, chunk/archive round trips, size limits, and malformed data');