#!/usr/bin/env node

const assert = require('assert');
const childProcess = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { ZsxArchive, haxe_io_Bytes } = require('./zsx_lib.js');

const executable = path.resolve(process.argv[2] || './zsx');
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'zsx-cross-'));

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

function run(args, shouldPass = true) {
  const result = childProcess.spawnSync(executable, args, { encoding: 'utf8' });
  if (shouldPass) {
    assert.strictEqual(result.status, 0, `${args.join(' ')} failed: ${result.stderr}`);
  } else {
    assert.notStrictEqual(result.status, 0, `${args.join(' ')} accepted malformed input`);
  }
}

function crossCase(name, input) {
  const source = path.join(temporary, `${name}.bin`);
  const cArchive = path.join(temporary, `${name}.c.zsx`);
  const haxeArchive = path.join(temporary, `${name}.haxe.zsx`);
  const decoded = path.join(temporary, `${name}.decoded`);
  fs.writeFileSync(source, input);

  run([source, cArchive]);
  const fromC = toBuffer(ZsxArchive.decode(toHaxeBytes(fs.readFileSync(cArchive))));
  assert.ok(fromC.equals(input), `${name}: Haxe failed to decode C archive`);

  fs.writeFileSync(haxeArchive, toBuffer(ZsxArchive.encode(toHaxeBytes(input))));
  run(['-d', haxeArchive, decoded]);
  assert.ok(fs.readFileSync(decoded).equals(input), `${name}: C failed to decode Haxe archive`);
  console.log(`${name}: C <-> Haxe (${input.length} bytes)`);
}

try {
  crossCase('empty', Buffer.alloc(0));

  const random = Buffer.alloc(4096);
  let state = 0x17883231;
  for (let i = 0; i < random.length; i++) {
    state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
    random[i] = state >>> 24;
  }
  crossCase('raw', random);

  const compressed = Buffer.alloc(65536);
  for (let i = 0; i < compressed.length; i++) compressed[i] = i % 31 ? 0x41 : 0x42;
  crossCase('compressed', compressed);

  const multi = Buffer.alloc(0xC00000 + 257);
  for (let i = 0; i < multi.length; i++) multi[i] = i % 31 ? 0x41 : 0x42;
  crossCase('multi-chunk', multi);

  const valid = path.join(temporary, 'compressed.c.zsx');
  const bytes = fs.readFileSync(valid);
  const malformed = [
    ['truncated', bytes.subarray(0, bytes.length - 1)],
    ['trailing', Buffer.concat([bytes, Buffer.from([0])])],
    ['flags', (() => { const b = Buffer.from(bytes); b.writeUInt32LE(1, 12); return b; })()],
    ['count', (() => { const b = Buffer.from(bytes); b.writeUInt32LE(2, 8); return b; })()],
    ['length', (() => { const b = Buffer.from(bytes); b.writeUInt32LE(0xffffffff, 16); return b; })()]
  ];
  for (const [name, archive] of malformed) {
    const input = path.join(temporary, `${name}.zsx`);
    fs.writeFileSync(input, archive);
    run(['-d', input, path.join(temporary, `${name}.out`)], false);
  }
  const sentinel = path.join(temporary, 'sentinel.out');
  fs.writeFileSync(sentinel, 'do not replace');
  const malformedInput = path.join(temporary, 'sentinel-malformed.zsx');
  fs.writeFileSync(malformedInput, bytes.subarray(0, bytes.length - 1));
  run(['-d', malformedInput, sentinel], false);
  assert.strictEqual(fs.readFileSync(sentinel, 'utf8'), 'do not replace',
    'failed decode replaced pre-existing output');

  const source = path.join(temporary, 'alias-source.bin');
  fs.writeFileSync(source, compressed);
  run([source, source], false);
  assert.ok(fs.readFileSync(source).equals(compressed), 'lexical same-path clobbered input');
  for (const kind of ['hard', 'symlink']) {
    const alias = path.join(temporary, `alias-${kind}.bin`);
    if (kind === 'hard') fs.linkSync(source, alias);
    else fs.symlinkSync(source, alias);
    run([source, alias], false);
    assert.ok(fs.readFileSync(source).equals(compressed), `${kind}-link alias clobbered input`);
  }
  for (const kind of ['hard', 'symlink']) {
    const alias = path.join(temporary, `archive-alias-${kind}.zsx`);
    if (kind === 'hard') fs.linkSync(valid, alias);
    else fs.symlinkSync(valid, alias);
    run(['-d', valid, alias], false);
    assert.ok(fs.readFileSync(valid).equals(bytes), `${kind}-link archive alias clobbered input`);
  }

  const defaultInput = path.join(temporary, 'default.bin');
  fs.writeFileSync(defaultInput, Buffer.from('default output test'));
  run([defaultInput]);
  const defaultArchive = `${defaultInput}.zsx`;
  assert.ok(fs.existsSync(defaultArchive), 'compression default .zsx name missing');
  const defaultDecodeArchive = path.join(temporary, 'default-decode.zsx');
  fs.copyFileSync(defaultArchive, defaultDecodeArchive);
  run(['-d', defaultDecodeArchive]);
  assert.ok(fs.readFileSync(path.join(temporary, 'default-decode')).equals(fs.readFileSync(defaultInput)),
    'decompression default suffix stripping failed');

  // A padded range stream whose declared sizes include the padding is not canonical.
  const tail = Buffer.from(bytes);
  const frameLength = tail.readUInt32LE(16);
  tail.writeUInt32LE(frameLength + 1, 16);
  const chunkAt = 20;
  tail.writeUInt32LE(tail.readUInt32LE(chunkAt + 4) + 1, chunkAt + 4);
  const padded = Buffer.concat([tail, Buffer.from([0])]);
  const paddedPath = path.join(temporary, 'canonical-tail.zsx');
  fs.writeFileSync(paddedPath, padded);
  run(['-d', paddedPath, path.join(temporary, 'canonical-tail.out')], false);
  console.log('malformed archives: rejected');
  console.log('PASS: portable C/Haxe archive cross-compatibility');
} finally {
  fs.rmSync(temporary, { recursive: true, force: true });
}