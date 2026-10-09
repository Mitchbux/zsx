#!/usr/bin/env node
const fs = require('fs');
const path = require('path');

const { Shrink, haxe_io_Bytes } = require('./zsx_lib.js');

function formatSize(bytes) {
    if (bytes < 1024) return bytes + ' B';
    if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(2) + ' KB';
    return (bytes / (1024 * 1024)).toFixed(2) + ' MB';
}

function bufferToHaxeBytes(buffer) {
    const bytes = haxe_io_Bytes.alloc(buffer.length);
    for (let i = 0; i < buffer.length; i++) {
        bytes.set(i, buffer[i]);
    }
    return bytes;
}

function haxeBytesToBuffer(bytes) {
    const len = bytes.length;
    const buffer = Buffer.alloc(len);
    for (let i = 0; i < len; i++) {
        buffer[i] = bytes.get(i);
    }
    return buffer;
}

const CHUNK_SIZE = 1 * 1024 * 1024;
const inputFile = path.join(__dirname, '..', 'enwik8');

if (!fs.existsSync(inputFile)) {
    console.error('enwik8 not found');
    process.exit(1);
}

const data = fs.readFileSync(inputFile);
console.log('='.repeat(60));
console.log('ZSX COMPRESSION TEST - enwik8');
console.log('='.repeat(60));
console.log(`Input size:  ${formatSize(data.length)}`);
console.log(`Chunk size:  ${formatSize(CHUNK_SIZE)}`);
console.log(`Chunks:      ${Math.ceil(data.length / CHUNK_SIZE)}`);
console.log('');

const totalChunks = Math.ceil(data.length / CHUNK_SIZE);
let totalCompressed = 0;
let totalOriginal = 0;
const startTime = Date.now();
let allMatch = true;

for (let i = 0; i < totalChunks; i++) {
    const offset = i * CHUNK_SIZE;
    const end = Math.min(offset + CHUNK_SIZE, data.length);
    const chunk = data.slice(offset, end);

    const chunkStart = Date.now();
    const inputBytes = bufferToHaxeBytes(chunk);
    const encoded = Shrink.encode(inputBytes);
    const compressTime = Date.now() - chunkStart;

    const decodeStart = Date.now();
    const decoded = Shrink.decode(encoded);
    const decodeTime = Date.now() - decodeStart;

    const match = chunk.length === decoded.length;
    let contentMatch = match;
    if (match) {
        for (let j = 0; j < chunk.length; j++) {
            if (inputBytes.get(j) !== decoded.get(j)) {
                contentMatch = false;
                console.log(`  MISMATCH at byte ${j}: expected ${inputBytes.get(j)} got ${decoded.get(j)}`);
                break;
            }
        }
    }

    if (!contentMatch) allMatch = false;

    totalOriginal += chunk.length;
    totalCompressed += encoded.length;

    const ratio = ((1 - encoded.length / chunk.length) * 100).toFixed(1);
    const speed = (chunk.length / 1024 / (compressTime / 1000)).toFixed(0);
    console.log(
        `Chunk ${(i + 1).toString().padStart(3)}/${totalChunks}: ` +
        `${formatSize(chunk.length).padStart(10)} -> ${formatSize(encoded.length).padStart(10)} ` +
        `(${ratio.padStart(5)}% reduction) ` +
        `enc: ${(compressTime + 'ms').padStart(8)} dec: ${(decodeTime + 'ms').padStart(8)} ` +
        `${speed.padStart(6)} KB/s ` +
        `${contentMatch ? 'OK' : 'FAIL'}`
    );
}

const elapsed = Date.now() - startTime;
const totalRatio = ((1 - totalCompressed / totalOriginal) * 100).toFixed(2);
const avgSpeed = (totalOriginal / 1024 / (elapsed / 1000)).toFixed(0);

console.log('');
console.log('='.repeat(60));
console.log('RESULTS');
console.log('='.repeat(60));
console.log(`Original size:    ${formatSize(totalOriginal)}`);
console.log(`Compressed size:  ${formatSize(totalCompressed)}`);
console.log(`Reduction:        ${totalRatio}%`);
console.log(`Total time:       ${(elapsed / 1000).toFixed(1)}s`);
console.log(`Average speed:    ${avgSpeed} KB/s`);
console.log(`Round-trip:       ${allMatch ? 'ALL CHUNKS MATCH' : 'SOME CHUNKS FAILED'}`);
console.log('='.repeat(60));

process.exit(allMatch ? 0 : 1);
