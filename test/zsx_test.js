#!/usr/bin/env node
/**
 * ZSX Compression Test Tool
 * Tests compression and decompression against enwik6 and other files
 */

const fs = require('fs');
const path = require('path');

// Load the patched ZSX library
const { Shrink, haxe_io_Bytes } = require('./zsx_lib.js');

if (!Shrink || !haxe_io_Bytes) {
    console.error('Error: ZSX classes not properly loaded');
    process.exit(1);
}

// Utility functions
function formatSize(bytes) {
    if (bytes < 1024) return bytes + ' B';
    if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(2) + ' KB';
    return (bytes / (1024 * 1024)).toFixed(2) + ' MB';
}

function formatTime(ms) {
    if (ms < 1000) return ms.toFixed(0) + ' ms';
    return (ms / 1000).toFixed(2) + ' s';
}

// Convert Node.js Buffer to Haxe Bytes
function bufferToHaxeBytes(buffer) {
    const bytes = haxe_io_Bytes.alloc(buffer.length);
    for (let i = 0; i < buffer.length; i++) {
        bytes.set(i, buffer[i]);
    }
    return bytes;
}

// Convert Haxe Bytes to Node.js Buffer
function haxeBytesToBuffer(bytes) {
    const len = bytes.length;
    const buffer = Buffer.alloc(len);
    for (let i = 0; i < len; i++) {
        buffer[i] = bytes.get(i);
    }
    return buffer;
}

// Compression test
function compress(inputFile) {
    console.log('\n' + '='.repeat(60));
    console.log('COMPRESSION TEST');
    console.log('='.repeat(60));
    
    if (!fs.existsSync(inputFile)) {
        console.error(`Error: File not found: ${inputFile}`);
        return null;
    }
    
    const data = fs.readFileSync(inputFile);
    console.log(`Input:  ${inputFile}`);
    console.log(`Size:   ${formatSize(data.length)}`);
    
    console.log('\nCompressing...');
    const startTime = Date.now();
    
    try {
        const inputBytes = bufferToHaxeBytes(data);
        const encoded = Shrink.encode(inputBytes);
        const elapsed = Date.now() - startTime;
        
        const outputBuffer = haxeBytesToBuffer(encoded);
        const ratio = ((1 - outputBuffer.length / data.length) * 100).toFixed(2);
        const outputFile = inputFile + '.zsx';
        
        fs.writeFileSync(outputFile, outputBuffer);
        
        console.log(`\nResults:`);
        console.log(`Output: ${outputFile}`);
        console.log(`Size:   ${formatSize(outputBuffer.length)}`);
        console.log(`Ratio:  ${ratio}% reduction`);
        console.log(`Speed:  ${formatTime(elapsed)}`);
        console.log(`Rate:   ${(data.length / 1024 / (elapsed / 1000)).toFixed(2)} KB/s`);
        console.log('='.repeat(60));
        
        return outputFile;
    } catch (e) {
        console.error('Compression error:', e.message);
        console.error(e.stack);
        return null;
    }
}

// Decompression test
function decompress(inputFile) {
    console.log('\n' + '='.repeat(60));
    console.log('DECOMPRESSION TEST');
    console.log('='.repeat(60));
    
    if (!fs.existsSync(inputFile)) {
        console.error(`Error: File not found: ${inputFile}`);
        return null;
    }
    
    const data = fs.readFileSync(inputFile);
    console.log(`Input:  ${inputFile}`);
    console.log(`Size:   ${formatSize(data.length)}`);
    
    console.log('\nDecompressing...');
    const startTime = Date.now();
    
    try {
        const inputBytes = bufferToHaxeBytes(data);
        const decoded = Shrink.decode(inputBytes);
        const elapsed = Date.now() - startTime;
        
        const outputBuffer = haxeBytesToBuffer(decoded);
        const outputFile = inputFile.replace('.zsx', '.out');
        
        fs.writeFileSync(outputFile, outputBuffer);
        
        console.log(`\nResults:`);
        console.log(`Output: ${outputFile}`);
        console.log(`Size:   ${formatSize(outputBuffer.length)}`);
        console.log(`Speed:  ${formatTime(elapsed)}`);
        console.log('='.repeat(60));
        
        return outputFile;
    } catch (e) {
        console.error('Decompression error:', e.message);
        console.error(e.stack);
        return null;
    }
}

// Verify decompression matches original
function verify(originalFile, decompressedFile) {
    console.log('\n' + '='.repeat(60));
    console.log('VERIFICATION');
    console.log('='.repeat(60));
    
    const original = fs.readFileSync(originalFile);
    const decompressed = fs.readFileSync(decompressedFile);
    
    if (original.length !== decompressed.length) {
        console.log(`FAIL: Size mismatch (${original.length} vs ${decompressed.length})`);
        return false;
    }
    
    for (let i = 0; i < original.length; i++) {
        if (original[i] !== decompressed[i]) {
            console.log(`FAIL: Content mismatch at byte ${i}`);
            return false;
        }
    }
    
    console.log('PASS: Decompressed file matches original');
    console.log('='.repeat(60));
    return true;
}

// Full round-trip test
function runFullTest(inputFile) {
    console.log('\n');
    console.log('*'.repeat(60));
    console.log('ZSX COMPRESSION ROUND-TRIP TEST');
    console.log('*'.repeat(60));
    console.log(`Testing: ${inputFile}`);
    
    // Compress
    const compressedFile = compress(inputFile);
    if (!compressedFile) return false;
    
    // Decompress
    const decompressedFile = decompress(compressedFile);
    if (!decompressedFile) return false;
    
    // Verify
    const success = verify(inputFile, decompressedFile);
    
    // Cleanup
    if (success) {
        console.log('\nCleaning up test files...');
        fs.unlinkSync(compressedFile);
        fs.unlinkSync(decompressedFile);
        console.log('Done.');
    }
    
    return success;
}

// Parse command line arguments
const args = process.argv.slice(2);

if (args.length < 1) {
    console.log(`
ZSX Compression Test Tool
=========================

Usage: node test/zsx_test.js <command> [filename]

Commands:
  compress <file>      Compress a file
  decompress <file>    Decompress a .zsx file
  test <file>          Full round-trip test (compress -> decompress -> verify)
  benchmark            Run benchmark against enwik6

Examples:
  node test/zsx_test.js compress enwik6
  node test/zsx_test.js decompress enwik6.zsx
  node test/zsx_test.js test enwik6
  node test/zsx_test.js benchmark
`);
    process.exit(0);
}

const command = args[0];
const filename = args[1];

switch (command) {
    case 'compress':
        if (!filename) {
            console.error('Error: Please specify a file to compress');
            process.exit(1);
        }
        compress(filename);
        break;
        
    case 'decompress':
        if (!filename) {
            console.error('Error: Please specify a file to decompress');
            process.exit(1);
        }
        decompress(filename);
        break;
        
    case 'test':
        if (!filename) {
            console.error('Error: Please specify a file to test');
            process.exit(1);
        }
        const success = runFullTest(filename);
        process.exit(success ? 0 : 1);
        break;
        
    case 'benchmark':
        console.log('Running benchmark against enwik6...');
        process.exit(runFullTest('enwik6') ? 0 : 1);
        break;
        
    default:
        console.error(`Unknown command: ${command}`);
        process.exit(1);
}
