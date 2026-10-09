import haxe.io.Bytes;
import BitWriter;
import BitReader;

class TestClassify2 {
    static function main() {
        var testBytes = Bytes.alloc(12);
        var srcBytes = [32, 111, 33, 100, 101, 72, 108, 108, 108, 114, 111, 87];
        for(i in 0...12) testBytes.set(i, srcBytes[i]);
        trace("Testing classify/declassify with: " + testBytes.toString());
        trace("Input bytes: " + bytesToString(testBytes));
        
        var keyBuf:Bytes = Bytes.alloc(testBytes.length * 4);
        var keyWriter = new BitWriter(keyBuf);
        
        trace("");
        trace("=== ENCODE ===");
        classifyDebug(testBytes, keyWriter);
        
        var encoded:Bytes = keyWriter.toBytes();
        trace("Encoded length: " + encoded.length);
        trace("Encoded bytes: " + bytesToString(encoded));
        
        trace("");
        trace("=== DECODE ===");
        var outBuf:Bytes = Bytes.alloc(testBytes.length * 2);
        var keyReader = new BitReader(encoded);
        
        var decoded = declassifyDebug(keyReader, outBuf, outBuf.length);
        trace("");
        trace("Decoded length: " + decoded.length);
        trace("Decoded bytes: " + bytesToString(decoded));
        trace("Decoded string: " + decoded.toString());
        
        trace("");
        if (testBytes.compare(decoded) == 0) trace("SUCCESS!");
        else trace("FAILURE!");
    }
    
    static function classifyDebug(inData:Bytes, key:BitWriter) {
        var read:Int = 0;
        var s,x:Int;
        var stack = new Array<Int>();
        var index = new Array<Int>();
        
        var load:Int = 0;
        var nbit:Int = 1;
        var nmax:Int = 2;
        
        for(s in 0...0x100) index[s] = 0;
        
        while(read < inData.length) {
            x = inData.get(read);
            trace("  Byte " + read + ": " + x + " (" + String.fromCharCode(x) + ")");
            
            if(index[x] > 0) {
                var idx = index[x];
                trace("    Existing, index=" + idx + ", writing bit 0, then " + nbit + "-bit index");
                key.writeBit(0);
                key.writeValue(idx, nbit);
                
                if(idx < (load >> 1)) {
                    trace("    PARTIAL FLUSH: idx=" + idx + " < load/2=" + (load>>1));
                    for(s in idx-1...load) {
                        trace("      Flushing stack[" + s + "]=" + stack[s]);
                        key.writeValue(stack[s], 8);
                        index[stack[s]] = 0;
                    }
                    load = idx - 1;
                    nbit = 1;
                    nmax = 2;
                    while(nmax <= load) { nmax <<= 1; nbit++; }
                    trace("    After flush: load=" + load + ", nbit=" + nbit);
                }
            } else {
                trace("    New, writing bit 1, stack[" + load + "]=" + x + ", index[" + x + "]=" + (load+1));
                key.writeBit(1);
                stack[load] = x;
                index[x] = ++load;
            }
            
            if(load == nmax) { 
                nmax <<= 1; 
                nbit++; 
                trace("    Upgraded: nmax=" + nmax + ", nbit=" + nbit);
            }
            read++;
        }
        
        trace("  FINAL FLUSH: bit 0, " + nbit + "-bit index 0");
        key.writeBit(0);
        key.writeValue(0, nbit);
        var flushOrder = "";
        while(load > 0) {
            flushOrder += " stack[" + (load-1) + "]=" + stack[load-1];
            key.writeValue(stack[--load], 8);
        }
        trace("    Flush order:" + flushOrder);
        
        key.flushWrite();
    }
    
    static function declassifyDebug(key:BitReader, outData:Bytes, maxLength:Int):Bytes {
        var s,x:Int;
        var at:Array<Array<Int>> = new Array<Array<Int>>();
        for(s in 0...0x100) at[s] = new Array<Int>();
        
        var pos:Int = 0;
        var load:Int = 0;
        var nbit:Int = 1;
        var nmax:Int = 2;
        
        while(key.canRead()) {
            var bit = key.readBit();
            trace("  Pos " + pos + ": bit=" + bit);
            
            if(bit == 0) {
                var idx:Int = key.readValue(nbit);
                trace("    Existing, idx=" + idx + " (read " + nbit + " bits)");
                
                if(idx == 0) {
                    trace("    FINAL FLUSH");
                    for(s in 0...load) {
                        var value:Int = key.readValue(8);
                        var positions:Array<Int> = at[load - s];
                        trace("      Read value=" + value + ", at[" + (load-s) + "]=" + arrayToString(positions));
                        for(x in 0...positions.length)
                            outData.set(positions[x], value);
                    }
                    return outData.sub(0, pos);
                }
                
                at[idx].push(pos);
                trace("    at[" + idx + "] now = " + arrayToString(at[idx]));
                pos++;
                
                if(idx < (load >> 1)) {
                    trace("    PARTIAL FLUSH: idx=" + idx + " < load/2=" + (load>>1));
                    for(s in idx-1...load) {
                        var value:Int = key.readValue(8);
                        var positions:Array<Int> = at[s + 1];
                        trace("      Read value=" + value + ", at[" + (s+1) + "]=" + arrayToString(positions));
                        for(x in 0...positions.length)
                            outData.set(positions[x], value);
                        at[s + 1] = new Array<Int>();
                    }
                    load = idx - 1;
                    nbit = 1;
                    nmax = 2;
                    while(nmax <= load) { nmax <<= 1; nbit++; }
                    trace("    After flush: load=" + load + ", nbit=" + nbit);
                }
            } else {
                load++;
                at[load].push(pos);
                trace("    New, load=" + load + ", at[" + load + "]=" + arrayToString(at[load]));
                pos++;
                if(load == nmax) { 
                    nmax <<= 1; 
                    nbit++; 
                    trace("    Upgraded: nmax=" + nmax + ", nbit=" + nbit);
                }
            }
        }
        
        return outData.sub(0, pos);
    }
    
    static function bytesToString(b:Bytes):String {
        var s = "[";
        for (i in 0...b.length) {
            if (i > 0) s += ", ";
            s += b.get(i);
        }
        return s + "]";
    }
    
    static function arrayToString(a:Array<Int>):String {
        var s = "[";
        for (i in 0...a.length) {
            if (i > 0) s += ", ";
            s += a[i];
        }
        return s + "]";
    }
}
