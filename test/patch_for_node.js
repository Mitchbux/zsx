#!/usr/bin/env node
// Patches zsx_node.js to create a CommonJS module

const fs = require('fs');
const path = require('path');

const inputPath = path.join(__dirname, '..', 'zsx_node.js');
const outputPath = path.join(__dirname, 'zsx_lib.js');

let code = fs.readFileSync(inputPath, 'utf8');

// Add variable declarations at the top
code = code.replace(
    '(function ($global) { "use strict";',
    'var Shrink, ZsxRange, ZsxArchive, haxe_io_Bytes, BitWriter, BitReader, QuickSort;\n(function ($global) { "use strict";'
);

// Replace the IIFE closing to export the classes
const iifClose = /\}\)\(typeof window.*\);?\s*$/;
code = code.replace(iifClose, `
    // Export classes
    if (typeof module !== 'undefined') {
        module.exports.Shrink = Shrink;
        module.exports.ZsxRange = ZsxRange;
        module.exports.ZsxArchive = ZsxArchive;
        module.exports.haxe_io_Bytes = haxe_io_Bytes;
        module.exports.BitWriter = BitWriter;
        module.exports.BitReader = BitReader;
    }
})(typeof global !== "undefined" ? global : this);
`);

fs.writeFileSync(outputPath, code);
console.log('Created', outputPath);
