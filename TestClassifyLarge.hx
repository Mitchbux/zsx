import haxe.io.Bytes;
import BitWriter;
import BitReader;
import sys.io.File;

class TestClassifyLarge {
    static function main() {
        var originalData:Bytes = File.getBytes("test_small.txt");
        
        var key:Bytes = Bytes.alloc(originalData.length*2);
        var keyWriter = new BitWriter(key);
        var bytes:Bytes = Bytes.alloc(originalData.length + 512);
        var bytesWriter = new BitWriter(bytes);
        
        Shrink.predict(originalData, bytesWriter, keyWriter);
        bytes = bytesWriter.toBytes();
        
        trace("Bytes to classify: " + bytes.length);
        trace("First 30 bytes: " + bytesToString(bytes.sub(0, 30)));
        
        var classifyKey:Bytes = Bytes.alloc(bytes.length * 4);
        var classifyWriter = new BitWriter(classifyKey);
        
        trace("");
        trace("=== CLASSIFY ===");
        classifyDebug(bytes, classifyWriter);
        
        var encoded = classifyWriter.toBytes();
        trace("");
        trace("Encoded length: " + encoded.length);
        
        trace("");
        trace("=== DECLASSIFY ===");
        var outBuf:Bytes = Bytes.alloc(bytes.length * 2);
        var reader = new BitReader(encoded);
        
        var decoded = declassifyDebug(reader, outBuf, outBuf.length);
        
        trace("");
        trace("Decoded length: " + decoded.length);
        trace("Original length: " + bytes.length);
        trace("Match: " + (bytes.compare(decoded) == 0));
        
        if (bytes.compare(decoded) != 0) {
            for (i in 0...bytes.length) {
                if (i >= decoded.length || bytes.get(i) != decoded.get(i)) {
                    trace("First mismatch at " + i + ": expected " + bytes.get(i) + ", got " + (i < decoded.length ? decoded.get(i) : -1));
                    break;
                }
            }
        }
    }
    
    static var encodePos = 0;
    static var flushCount = 0;
    
    static function classifyDebug(inData:Bytes, key:BitWriter) {
        var read:Int = 0;
        var s,x:Int;
        var stack = new Array<Int>();
        var index = new Array<Int>();
        
        var load:Int = 0;
        var nbit:Int = 1;
        var nmax:Int = 2;
        
        for(s in 0...0x100) index[s] = 0;
        
        encodePos = 0;
        flushCount = 0;
        
        while(read < inData.length) {
            x = inData.get(read++);
            encodePos++;
            key.writeBit(index[x] > 0 ? 0 : 1);
            
            if(index[x] > 0) {
                var idx:Int = index[x];
                key.writeValue(idx, nbit);
                if(idx < (load >> 1)) {
                    flushCount++;
                    trace("  PARTIAL FLUSH #" + flushCount + " at pos " + encodePos + ": idx=" + idx + ", load=" + load);
                    for(s in idx-1...load) {
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
                stack[load] = x;
                index[x] = ++load;
            }
            
            if(load == nmax) { nmax <<= 1; nbit++; }
        }
        
        trace("  FINAL FLUSH at pos " + encodePos + ": load=" + load);
        key.writeBit(0);
        key.writeValue(0, nbit);
        while(load > 0) key.writeValue(stack[--load], 8);
        
        key.flushWrite();
    }
    
    static var decodePos = 0;
    static var decodeFlushCount = 0;
    
    static function declassifyDebug(key:BitReader, outData:Bytes, maxLength:Int):Bytes {
        var s,x:Int;
        var at:Array<Array<Int>> = new Array<Array<Int>>();
        for(s in 0...0x100) at[s] = new Array<Int>();
        
        var pos:Int = 0;
        var load:Int = 0;
        var nbit:Int = 1;
        var nmax:Int = 2;
        
        decodePos = 0;
        decodeFlushCount = 0;
        
        while(key.canRead()) {
            var bit = key.readBit();
            decodePos++;
            
            if(bit == 0) {
                var idx:Int = key.readValue(nbit);
                
                if(idx == 0) {
                    trace("  FINAL FLUSH at decodePos " + decodePos + ": load=" + load + ", pos=" + pos);
                    for(s in 0...load) {
                        var value:Int = key.readValue(8);
                        var positions:Array<Int> = at[load - s];
                        for(x in 0...positions.length)
                            outData.set(positions[x], value);
                    }
                    return outData.sub(0, pos);
                }
                
                at[idx].push(pos++);
                
                if(idx < (load >> 1)) {
                    decodeFlushCount++;
                    trace("  PARTIAL FLUSH #" + decodeFlushCount + " at decodePos " + decodePos + ": idx=" + idx + ", load=" + load + ", pos=" + pos);
                    for(s in idx-1...load) {
                        var value:Int = key.readValue(8);
                        var positions:Array<Int> = at[s + 1];
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
                at[load].push(pos++);
                if(load == nmax) { nmax <<= 1; nbit++; }
            }
        }
        
        trace("  Ran out of data at decodePos " + decodePos + ", pos=" + pos);
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
}
