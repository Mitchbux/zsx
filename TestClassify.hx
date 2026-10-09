import haxe.io.Bytes;
import BitWriter;
import BitReader;

class Shrink2 {
    public static function classify(inData:Bytes, key:BitWriter) {
        var read:Int = 0;
        var s,x:Int;
        var stack = new Array<Int>();
        var index = new Array<Int>();
        
        var load:Int = 0;
        var nbit:Int = 1;
        var nmax:Int = 2;
        
        for(s in 0...0x100) index[s] = 0;
        
        while(read < inData.length) {
            x = inData.get(read++);
            key.writeBit(index[x] > 0 ? 0 : 1);
            
            if(index[x] > 0) {
                key.writeValue(index[x], nbit);
                if(index[x] < (load >> 1)) {
                    for(s in index[x]-1...load) {
                        key.writeValue(stack[s], 8);
                        index[stack[s]] = 0;
                    }
                    load = index[x] - 1;
                    nbit = 1;
                    nmax = 2;
                    while(nmax <= load) { nmax <<= 1; nbit++; }
                }
            } else {
                stack[load] = x;
                index[x] = ++load;
            }
            
            if(load == nmax) { nmax <<= 1; nbit++; }
        }
        
        key.writeBit(0);
        key.writeValue(0, nbit);
        while(load > 0) key.writeValue(stack[--load], 8);
        
        key.flushWrite();
    }
    
    public static function declassify(key:BitReader, outData:Bytes, maxLength:Int):Bytes {
        var write:Int = 0;
        var s,x:Int;
        var stack = new Array<Int>();
        var index = new Array<Int>();
        var ops = new Array<Int>();
        
        var load:Int = 0;
        var nbit:Int = 1;
        var nmax:Int = 2;
        var opCount:Int = 0;
        var newCount:Int = 0;
        var done:Bool = false;
        
        for(s in 0...0x100) index[s] = 0;
        
        while(!done && key.canRead()) {
            var isNew:Int = key.readBit();
            
            if(isNew == 1) {
                ops[opCount++] = -1;
                newCount++;
                load++;
                if(load == nmax) { nmax <<= 1; nbit++; }
            } else {
                var idx:Int = key.readValue(nbit);
                
                if(idx == 0) {
                    for(s in 0...load) {
                        x = key.readValue(8);
                        stack[load - 1 - s] = x;
                    }
                    
                    var newIdx:Int = 0;
                    for(s in 0...opCount) {
                        if(ops[s] == -1)
                            outData.set(write++, stack[newIdx++]);
                        else
                            outData.set(write++, stack[ops[s] - 1]);
                    }
                    
                    done = true;
                } else if(idx < (load >> 1)) {
                    var flushStart:Int = idx - 1;
                    for(s in flushStart...load) {
                        x = key.readValue(8);
                        stack[s] = x;
                        index[x] = s + 1;
                    }
                    
                    var newIdx:Int = 0;
                    for(s in 0...opCount) {
                        if(ops[s] == -1)
                            outData.set(write++, stack[newIdx++]);
                        else
                            outData.set(write++, stack[ops[s] - 1]);
                    }
                    
                    for(s in flushStart...load)
                        index[stack[s]] = 0;
                    
                    load = flushStart;
                    nbit = 1;
                    nmax = 2;
                    while(nmax <= load) { nmax <<= 1; nbit++; }
                    opCount = 0;
                    newCount = 0;
                } else {
                    ops[opCount++] = idx;
                }
            }
        }
        
        return outData.sub(0, write);
    }
}

class TestClassify {
    static function main() {
        var testData = "Hello World!";
        var inData:Bytes = Bytes.ofString(testData);
        
        trace("Input: " + testData);
        trace("Input bytes: " + bytesToString(inData));
        
        var keyBuf:Bytes = Bytes.alloc(inData.length * 4);
        var keyWriter = new BitWriter(keyBuf);
        
        Shrink2.classify(inData, keyWriter);
        
        var encoded:Bytes = keyWriter.toBytes();
        trace("Encoded length: " + encoded.length);
        trace("Encoded bytes: " + bytesToString(encoded));
        
        var outBuf:Bytes = Bytes.alloc(inData.length * 2);
        var keyReader = new BitReader(encoded);
        
        var decoded = Shrink2.declassify(keyReader, outBuf, outBuf.length);
        trace("Decoded length: " + decoded.length);
        trace("Decoded bytes: " + bytesToString(decoded));
        trace("Decoded string: " + decoded.toString());
        
        var match = true;
        if (decoded.length != inData.length) {
            trace("Length mismatch!");
            match = false;
        } else {
            for (i in 0...inData.length) {
                if (decoded.get(i) != inData.get(i)) {
                    trace("Mismatch at " + i + ": expected " + inData.get(i) + ", got " + decoded.get(i));
                    match = false;
                    break;
                }
            }
        }
        
        if (match) trace("SUCCESS!");
        else trace("FAILURE!");
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
