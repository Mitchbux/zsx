import haxe.io.Bytes;
import Shrink;
import BitWriter;
import BitReader;
import sys.io.File;

class TestDebug {
    static function main() {
        var inData:Bytes = File.getBytes("test_small.txt");
        trace("Input size: " + inData.length);
        trace("First 20 bytes: " + bytesToString(inData.sub(0, 20)));
        
        trace("");
        trace("=== MANUAL ENCODE ===");
        
        var key:Bytes = Bytes.alloc(inData.length*2);
        var keyWriter = new BitWriter(key);
        var bytes:Bytes = Bytes.alloc(inData.length + 512);
        var bytesWriter = new BitWriter(bytes);
        
        Shrink.predict(inData, bytesWriter, keyWriter);
        
        bytes = bytesWriter.toBytes();
        trace("After predict:");
        trace("  bytesWriter.start: " + bytesWriter.start);
        trace("  bytes length: " + bytes.length);
        trace("  keyWriter.inter: " + keyWriter.inter);
        
        Shrink.classify(bytes, keyWriter);
        key = keyWriter.toBytes();
        trace("After classify:");
        trace("  keyWriter.start: " + keyWriter.start);
        trace("  key length: " + key.length);
        
        trace("");
        trace("=== MANUAL DECODE ===");
        
        var inter = keyWriter.inter;
        var keyData = key.sub(0, inter);
        var classifyData = key.sub(inter, key.length - inter);
        
        trace("keyData (predict) length: " + keyData.length);
        trace("classifyData length: " + classifyData.length);
        
        var bytesDecoded:Bytes = Bytes.alloc(inData.length * 2);
        var classifyReader = new BitReader(classifyData);
        
        var decodedBytes = Shrink.declassify(classifyReader, bytesDecoded, bytesDecoded.length);
        trace("Declassified length: " + decodedBytes.length);
        trace("Original bytes length: " + bytes.length);
        trace("Bytes match: " + (bytes.compare(decodedBytes) == 0));
        
        if (bytes.compare(decodedBytes) != 0) {
            trace("Finding first mismatch:");
            for (i in 0...bytes.length) {
                if (i >= decodedBytes.length || bytes.get(i) != decodedBytes.get(i)) {
                    trace("  Mismatch at " + i + ": expected " + bytes.get(i) + ", got " + (i < decodedBytes.length ? decodedBytes.get(i) : -1));
                    break;
                }
            }
        }
        
        var keyReader = new BitReader(keyData);
        var outData:Bytes = Bytes.alloc(inData.length);
        
        var result = Shrink.predict_read(decodedBytes, outData, inData.length, keyReader);
        trace("");
        trace("Result length: " + result.length);
        trace("Result first 20 bytes: " + bytesToString(result.sub(0, 20)));
        trace("Expected first 20 bytes: " + bytesToString(inData.sub(0, 20)));
        
        var match = (inData.compare(result) == 0);
        trace("Match: " + match);
        
        if (!match) {
            for (i in 0...inData.length) {
                if (i >= result.length || inData.get(i) != result.get(i)) {
                    trace("First mismatch at " + i + ": expected " + inData.get(i) + ", got " + (i < result.length ? result.get(i) : -1));
                    break;
                }
            }
        }
    }
    
    static function bytesToString(b:Bytes):String {
        var s = "[";
        for (i in 0...b.length) {
            if (i > 0) s += ", ";
            s += b.get(i);
        }
        return s + "]";
    }
}
