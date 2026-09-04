#!/bin/bash

echo "=== ZSX Build Pipeline ==="

echo "Step 1: Building zsx.js with Haxe..."
haxe buildjs.hxml
if [ $? -ne 0 ]; then
    echo "ERROR: Failed to build zsx.js"
    exit 1
fi
echo "zsx.js built successfully"

echo "Step 2: Building browser worker..."
haxe -m ZsxWorker -js zsx_worker.js
if [ $? -ne 0 ]; then
    echo "ERROR: Failed to build zsx_worker.js"
    exit 1
fi
echo "zsx_worker.js built successfully"

echo "Step 3: Building chunk worker..."
haxe -m ZsxChunkWorker -js zsx_chunk_worker.js
if [ $? -ne 0 ]; then
    echo "ERROR: Failed to build zsx_chunk_worker.js"
    exit 1
fi
echo "zsx_chunk_worker.js built successfully"

echo "Step 4: Building legacy zsx_encode.js with Haxe..."
haxe buildencode.hxml
if [ $? -ne 0 ]; then
    echo "ERROR: Failed to build zsx_encode.js"
    exit 1
fi
echo "zsx_encode.js built successfully"

echo "Step 5: Building finalize tool..."
cd finalize_tool
dotnet build -c Release --nologo -v q
if [ $? -ne 0 ]; then
    echo "ERROR: Failed to build finalize tool"
    exit 1
fi
cd ..
echo "Finalize tool built successfully"

echo "Step 6: Creating zsxe.js..."
dotnet finalize_tool/bin/Release/net8.0/finalize_tool.dll zsx_encode.js zsxe > zsxe.js
if [ $? -ne 0 ]; then
    echo "ERROR: Failed to create zsxe.js"
    exit 1
fi
echo "zsxe.js created successfully"

echo "Step 7: Copying files to public folder..."
cp zsx.js zsx_worker.js zsx_chunk_worker.js zsxe.js public/
echo "Files copied to public/"

echo "Step 8: Building Node.js test module..."
haxe buildnode.hxml
if [ $? -ne 0 ]; then
    echo "ERROR: Failed to build zsx_node.js"
    exit 1
fi
node test/patch_for_node.js
echo "Node.js test module created"

echo ""
echo "=== Build Complete ==="
echo "Output files:"
ls -la public/*.js
echo ""
echo "Test with: node test/zsx_test.js benchmark"
