import haxe.io.Bytes;
import Shrink;
import js.node.Fs;
import js.node.Buffer;

class TestNode {
    static function main() {
        var args = js.Lib.require('process').argv;
        
        if (args.length < 3) {
            trace("Usage: node test_zsx.js <filename>");
            return;
        }
        
        var filename = args[2];
        trace("Testing with file: " + filename);
        
        try {
            var buffer:Buffer = Fs.readFileSync(filename);
            var inData:Bytes = Bytes.alloc(buffer.length);
            for (i in 0...buffer.length) {
                inData.set(i, buffer[i]);
            }
            
            trace("Input size: " + inData.length + " bytes");
            
            var startEncode = Date.now().getTime();
            var encoded = Shrink.encode(inData);
            var endEncode = Date.now().getTime();
            
            trace("Encoded size: " + encoded.length + " bytes");
            trace("Compression ratio: " + Std.string(encoded.length / inData.length * 100) + "%");
            trace("Encode time: " + (endEncode - startEncode) + "ms");
            
            var startDecode = Date.now().getTime();
            var decoded = Shrink.decode(encoded);
            var endDecode = Date.now().getTime();
            
            trace("Decoded size: " + decoded.length + " bytes");
            trace("Decode time: " + (endDecode - startDecode) + "ms");
            
            var match = true;
            if (decoded.length != inData.length) {
                trace("ERROR: Length mismatch! Expected " + inData.length + ", got " + decoded.length);
                match = false;
            } else {
                for (i in 0...inData.length) {
                    if (decoded.get(i) != inData.get(i)) {
                        trace("ERROR: Mismatch at position " + i + ": expected " + inData.get(i) + ", got " + decoded.get(i));
                        match = false;
                        break;
                    }
                }
            }
            
            if (match) {
                trace("SUCCESS: Decoded data matches original!");
            } else {
                trace("FAILURE: Decoded data does not match original");
            }
            
        } catch (e:Dynamic) {
            trace("Error: " + e);
        }
    }
}
