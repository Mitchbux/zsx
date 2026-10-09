import haxe.io.Bytes;
import BitWriter;
import BitReader;
import Shrink;

class TestEncode {
    static function main() {
        var testData = "Hello World!";
        var inData:Bytes = Bytes.ofString(testData);
        
        trace("=== ENCODE DETAILS ===");
        trace("Input: " + testData);
        trace("Input length: " + inData.length);
        trace("Input bytes: " + bytesToString(inData));
        
        var key:Bytes = Bytes.alloc(inData.length*2);
        var keyWriter = new BitWriter(key);
        var bytes:Bytes = Bytes.alloc(inData.length);
        var bytesWriter = new BitWriter(bytes);
        
        Shrink.predict(inData, bytesWriter, keyWriter);
        
        trace("");
        trace("After predict:");
        trace("  keyWriter.start: " + keyWriter.start);
        trace("  keyWriter.inter: " + keyWriter.inter);
        trace("  bytesWriter.start: " + bytesWriter.start);
        trace("  bytesWriter.inter: " + bytesWriter.inter);
        
        bytes = bytesWriter.toBytes();
        trace("  bytes length: " + bytes.length);
        trace("  bytes: " + bytesToString(bytes));
        
        var keyAfterPredict = keyWriter.toBytes();
        trace("  key after predict length: " + keyAfterPredict.length);
        trace("  key after predict: " + bytesToString(keyAfterPredict));
        
        Shrink.classify(bytes, keyWriter);
        
        trace("");
        trace("After classify:");
        trace("  keyWriter.start: " + keyWriter.start);
        
        key = keyWriter.toBytes();
        trace("  total key length: " + key.length);
        trace("  total key: " + bytesToString(key));
        
        var inter = keyWriter.inter;
        trace("");
        trace("Data layout:");
        trace("  inter (predict part): " + inter);
        trace("  classify part: " + (key.length - inter));
        trace("  predict key: " + bytesToString(key.sub(0, inter)));
        trace("  classify key: " + bytesToString(key.sub(inter, key.length - inter)));
        
        trace("");
        trace("=== VERIFY CLASSIFY/DECLASSIFY ===");
        var classifyData = key.sub(inter, key.length - inter);
        var classifyReader = new BitReader(classifyData);
        var outBuf:Bytes = Bytes.alloc(bytes.length * 2);
        var declassified = Shrink.declassify(classifyReader, outBuf, outBuf.length);
        trace("Original bytes: " + bytesToString(bytes));
        trace("Declassified:   " + bytesToString(declassified));
        trace("Match: " + (bytes.compare(declassified) == 0));
    }
    
    static function bytesToString(b:Bytes):String {
        var s = "[";
        var limit = b.length < 30 ? b.length : 30;
        for (i in 0...limit) {
            if (i > 0) s += ", ";
            s += b.get(i);
        }
        if (b.length > 30) s += "...";
        return s + "]";
    }
}
