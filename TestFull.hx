import haxe.io.Bytes;
import Shrink;
import BitWriter;
import BitReader;

class TestFull {
    static function main() {
        var testData = "Hello World!";
        var inData:Bytes = Bytes.ofString(testData);
        
        trace("=== FULL ENCODE/DECODE TEST ===");
        trace("Input: " + testData);
        trace("Input length: " + inData.length);
        trace("Input bytes: " + bytesToString(inData));
        
        trace("");
        trace("=== ENCODE ===");
        var encoded = Shrink.encode(inData);
        
        var storedLength = encoded.getInt32(0);
        var inter = encoded.getInt32(4);
        trace("Stored length: " + storedLength);
        trace("Inter: " + inter);
        trace("Total encoded length: " + encoded.length);
        trace("Encoded bytes: " + bytesToString(encoded));
        
        trace("");
        trace("=== DECODE ===");
        var decoded = Shrink.decode(encoded);
        trace("Decoded length: " + decoded.length);
        trace("Decoded: " + decoded.toString());
        trace("Decoded bytes: " + bytesToString(decoded));
        
        trace("");
        if (inData.compare(decoded) == 0) {
            trace("SUCCESS!");
        } else {
            trace("FAILURE!");
            for (i in 0...inData.length) {
                if (i >= decoded.length) {
                    trace("  Missing at " + i + ": expected " + inData.get(i));
                } else if (inData.get(i) != decoded.get(i)) {
                    trace("  Mismatch at " + i + ": expected " + inData.get(i) + ", got " + decoded.get(i));
                }
            }
        }
    }
    
    static function bytesToString(b:Bytes):String {
        var s = "[";
        var limit = b.length < 50 ? b.length : 50;
        for (i in 0...limit) {
            if (i > 0) s += ", ";
            s += b.get(i);
        }
        if (b.length > 50) s += "...";
        return s + "]";
    }
}
