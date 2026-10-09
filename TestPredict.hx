import haxe.io.Bytes;
import BitWriter;
import BitReader;

class TestPredict {
    static function main() {
        var input = Bytes.ofString("Hello World!");
        trace("Input: " + input.toString());
        trace("Input length: " + input.length);
        
        var key:Bytes = Bytes.alloc(input.length * 2);
        var keyWriter = new BitWriter(key);
        var bytes:Bytes = Bytes.alloc(input.length + 512);
        var bytesWriter = new BitWriter(bytes);
        
        trace("");
        trace("=== PREDICT TRACE ===");
        predictDebug(input, bytesWriter, keyWriter);
        
        bytes = bytesWriter.toBytes();
        trace("");
        trace("After predict:");
        trace("  bytesWriter.start: " + bytesWriter.start);
        trace("  bytes length: " + bytes.length);
        trace("  bytes: " + bytesToString(bytes));
        trace("  keyWriter.inter: " + keyWriter.inter);
    }
    
    static function predictDebug(inData:Bytes, bits:BitWriter, key:BitWriter) {
        var count: Array<Int> = new Array<Int>();
        var full:Int = 0; 
        var stop:Int = 0;
        var read:Int = 0;
        var z,s,x:Int;
        for(s in 0...0x10000) count[s]=0;
        
        var max:Int = 0;
        z=0;
        trace("Reading pairs:");
        while(true) {
            x = inData.get(read++)+(inData.get(read++)<<8);
            count[x]++;
            trace("  Pair " + (read/2-1) + ": " + x + " (count=" + count[x] + ")");
            if(max<count[x]) max=count[x];
            if(read>=inData.length) break;
        }
        trace("Max count: " + max);
        
        var next:Int = 0;
        var reverse: Array<Array<Int>> = new Array<Array<Int>>();
        for(s in 0...0x10000) {
            x = count[s];
            if(x>0) {
                if (reverse[x] == null) reverse[x] = new Array<Int>();
                reverse[x].push(s);
                next++;
            }
        }
        trace("Unique pairs: " + next);
        
        var indices:Array<Int> = new Array<Int>();
        for(s in 0...0x10000) indices[s]=0;
        
        s=max;
        next=0;
        trace("Writing index table:");
        while(true) {
            if(reverse[s] != null && next < 0xFF) {
                for(x in 0...reverse[s].length) {
                    var value:Int = reverse[s][x];
                    indices[value]=++next;
                    trace("  Index " + next + " = " + value + " (0x" + StringTools.hex(value, 4) + ")");
                    bits.writeValue(value, 16);
                    if(next == 0xFF) break;
                }
            }
            s--;
            if(s==0) break;
        }
        trace("Total indices: " + next);
        trace("bits.start after index table: " + bits.start);
        
        key.writeValue(next, 8);
        trace("Wrote index count " + next + " to key");
        
        var last:Int = 0;
        var prev:Int = 0;
        var predicted: Array<Array<Int>> = new Array<Array<Int>>();
        for(s in 0...0x100) predicted[s] = new Array<Int>();
        
        trace("");
        trace("Processing pairs:");
        while(true) {
            while(true) {
                var is:Bool = false;
                x = inData.get(full++);
                s = inData.get(full++);
                z = x+(s<<8);
                if(indices[z]>0) {
                    x = indices[z];
                    is = true;
                } else full--;
                
                key.writeBool(is);
                
                var predVal = predicted[prev][last];
                var predMatch = (predVal == x);
                trace("  full=" + full + " z=" + z + " is=" + is + " x=" + x + " prev=" + prev + " last=" + last + " predVal=" + predVal + " match=" + predMatch);
                
                key.writeBool(predMatch);
                if(!predMatch) {
                    predicted[prev][last] = x;
                    bits.writeValue(x, 8);
                    trace("    -> wrote prediction miss: " + x);
                }
                prev = last;
                last = x;
                
                if(full >= inData.length) break;
            }
            if(full >= inData.length) break;
        }
        
        trace("");
        trace("bits.start after processing: " + bits.start);
        key.flushWrite();
        key.inter = key.start;
        bits.flushWrite();
        bits.inter = bits.start;
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
