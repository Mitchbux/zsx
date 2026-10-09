#!/usr/bin/env node
/**
 * Deterministic compression regression suite.
 * Run after building the Node module:
 *   haxe buildnode.hxml && node test/patch_for_node.js && node test/zsx_regression.js
 */

const fs = require('fs');
const path = require('path');
const { Shrink, haxe_io_Bytes, BitWriter, BitReader } = require('./zsx_lib.js');

function toHaxeBytes(buffer) {
    const bytes = haxe_io_Bytes.alloc(buffer.length);
    for (let i = 0; i < buffer.length; i++) {
        bytes.set(i, buffer[i]);
    }
    return bytes;
}

function seededBytes(length, seed) {
    const output = Buffer.alloc(length);
    let state = seed >>> 0;

    for (let i = 0; i < output.length; i++) {
        state = (state * 1664525 + 1013904223) >>> 0;
        output[i] = state >>> 24;
    }

    return output;
}

function assertRoundTrip(name, input) {
    const encoded = Shrink.encode(toHaxeBytes(input));
    const decoded = Shrink.decode(encoded);

    if (decoded.length !== input.length) {
        throw new Error(`${name}: length mismatch (${input.length} != ${decoded.length})`);
    }

    for (let i = 0; i < input.length; i++) {
        if (decoded.get(i) !== input[i]) {
            throw new Error(`${name}: mismatch at byte ${i} (${input[i]} != ${decoded.get(i)})`);
        }
    }

    console.log(`${name.padEnd(22)} ${String(input.length).padStart(6)} -> ${String(encoded.length).padStart(6)} bytes`);
}

function assertClassifyRoundTrip(name, input) {
    const keyBuffer = haxe_io_Bytes.alloc(input.length * 2 + 512);
    const writer = new BitWriter(keyBuffer);

    Shrink.classify(toHaxeBytes(input), writer);
    const decoded = Shrink.declassify(
        new BitReader(writer.toBytes()),
        haxe_io_Bytes.alloc(input.length),
        input.length
    );

    if (decoded.length !== input.length) {
        throw new Error(`${name}: classifier length mismatch (${input.length} != ${decoded.length})`);
    }

    for (let i = 0; i < input.length; i++) {
        if (decoded.get(i) !== input[i]) {
            throw new Error(`${name}: classifier mismatch at byte ${i} (${input[i]} != ${decoded.get(i)})`);
        }
    }
}

function assertDecodeFails(name, encoded) {
    let failed = false;
    try {
        Shrink.decode(toHaxeBytes(encoded));
    } catch (error) {
        failed = true;
    }

    if (!failed) {
        throw new Error(`${name}: malformed stream was accepted`);
    }
}

const LEGACY_FIXTURE = Buffer.from('DAAAAAQAAAAAAAAAgAAAIAP+f4CAR//yAAB/QMCA', 'base64');

function assertLegacyFixture() {
    const expected = Buffer.from([0, 0xff, 1, 0xfe, 2, 0, 0xff, 3, 0xfd, 1, 0, 0xff]);
    const decoded = Shrink.decode(toHaxeBytes(LEGACY_FIXTURE));

    if (decoded.length !== expected.length) {
        throw new Error(`legacy fixture: length mismatch (${expected.length} != ${decoded.length})`);
    }

    for (let i = 0; i < expected.length; i++) {
        if (decoded.get(i) !== expected[i]) {
            throw new Error(`legacy fixture: mismatch at byte ${i} (${expected[i]} != ${decoded.get(i)})`);
        }
    }
}

function assertPublicWorkerIsCurrent() {
    const workerPath = path.join(__dirname, '..', 'public', 'zsxe.js');
    const worker = fs.readFileSync(workerPath, 'utf8');
    const requiredFragments = [
        'Missing prediction dictionary size',
        'Compressed data declares an unsupported output size',
        'isCurrentClassifier = (interField & -2147483648) != 0',
    ];

    for (const fragment of requiredFragments) {
        if (!worker.includes(fragment)) {
            throw new Error(`public worker is stale: missing "${fragment}"`);
        }
    }
}

const cases = [
    ['empty', Buffer.alloc(0)],
    ['one-byte', Buffer.from([0])],
    ['two-byte', Buffer.from([0, 0xff])],
    ['odd-length binary', Buffer.from([0, 0xff, 1, 0xfe, 2])],
    ['index boundary 255', Buffer.from(Array.from({ length: 255 }, (_, i) => i))],
    ['index boundary 256', Buffer.from(Array.from({ length: 256 }, (_, i) => i))],
    ['index boundary 257', Buffer.from(Array.from({ length: 257 }, (_, i) => i & 0xff))],
    ['descending byte values', Buffer.from(Array.from({ length: 256 }, (_, i) => 255 - i))],
    ['repeated byte', Buffer.alloc(4096, 0x41)],
    ['periodic sequence', Buffer.from(Array.from({ length: 4096 }, (_, i) => i % 7))],
    ['all byte values', Buffer.from(Array.from({ length: 4096 }, (_, i) => i & 0xff))],
    ['seeded random', seededBytes(4096, 0x12345678)],
];

for (const [name, input] of cases) {
    assertClassifyRoundTrip(name, input);
    assertRoundTrip(name, input);
}

assertLegacyFixture();
assertPublicWorkerIsCurrent();
assertDecodeFails('truncated legacy classifier stream', LEGACY_FIXTURE.subarray(0, LEGACY_FIXTURE.length - 1));
assertDecodeFails(
    'legacy classifier stream with trailing data',
    Buffer.concat([LEGACY_FIXTURE, Buffer.from([0])])
);

const malformedSource = Buffer.from([0, 0xff, 1, 0xfe, 2, 0, 0xff, 3, 0xfd]);
const malformedEncoded = Shrink.encode(toHaxeBytes(malformedSource));
const truncated = Buffer.alloc(malformedEncoded.length - 1);
for (let i = 0; i < truncated.length; i++) {
    truncated[i] = malformedEncoded.get(i);
}
assertDecodeFails('truncated classifier stream', truncated);
assertDecodeFails(
    'missing predictor data',
    Buffer.from([1, 0, 0, 0, 0, 0, 0, 0x80, 0x40])
);
assertDecodeFails('truncated container header', Buffer.alloc(7));

const oversizedHeader = Buffer.alloc(8);
oversizedHeader.writeInt32LE(0x10000001, 0);
assertDecodeFails('oversized declared output', oversizedHeader);

const invalidSplitHeader = Buffer.alloc(8);
invalidSplitHeader.writeInt32LE(1, 0);
invalidSplitHeader.writeInt32LE(1, 4);
assertDecodeFails('split beyond container data', invalidSplitHeader);

console.log(`PASS: ${cases.length} direct classifier, full round-trip, legacy, and malformed-stream cases`);