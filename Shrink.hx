import haxe.io.Bytes;
import BitWriter;
import BitReader;

class Shrink
{
  static inline var MAX_DECODED_LENGTH:Int = 0x10000000;

  public static function classify(inData:Bytes, key:BitWriter) : Bytes
  {
    var read:Int = 0;
    var z,s,x:Int;
    var stack = new Array<Int>();
    var index = new Array<Int>();
    var nbit:Int = 1;
    var nmax:Int = 2;
    var next:Int = 0;

    for(s in 0...0x100) index[s] = 0;

    while(read < inData.length)
    {
      x = inData.get(read++);

      key.writeBool(index[x]>0);
      if(index[x]>0)
      {
         key.writeValue(x,8);
      }
      else
      {
        key.writeBool(false);
        stack[next] = x;
        index[x] = ++next;
      }

      if(next == nmax)
      {
        nmax <<= 1;
        nbit++;
      }

      if(next==0x6f)
      {
           key.writeBool(false);
    key.writeBool(true);
    for(s in 0...next){
      key.writeValue(stack[s], 8);
      index[stack[s]]=0;
    }
    key.flushWrite();
        next=0;
      }
    }

    // New marker + end marker, followed by the active dictionary.
    key.writeBool(false);
    key.writeBool(true);
    for(s in 0...next)
      key.writeValue(stack[s], 8);
    key.flushWrite();

    return Bytes.alloc(0);
  }

  public static function predict(inData:Bytes, bits:BitWriter, key:BitWriter)
  {
    var count: Array<Int> = new Array<Int>();
    var full:Int = 0; 
    var stop:Int = 0;
    var read:Int = 0;
    var z,s,x:Int;          
    for(s in 0...0x10000)
        count[s]=0;

    var max:Int =0;
    z=0;
    // Count overlapping pairs. The tokenizer can start a pair at any byte
    // after a literal, so fixed even-position counting misses useful pairs.
    while(read + 1 < inData.length)
    {
      x = inData.get(read)+(inData.get(read+1)<<8);
      read++;
      count[x]++;
      if(max<count[x])max=count[x];
    }

    var next:Int = 0;
    var reverse: Array<Array<Int>> = new Array<Array<Int>>();
    for(s in 0...0x10000)
    {
      x = count[s];
      if(x>0)
      {
        if (reverse[x] == null)
          reverse[x] = new Array<Int>();
        reverse[x].push(s);
        next++;
      }
    }

    var indices:Array<Int> = new Array<Int>();
    for(s in 0...0x10000)
        indices[s]=0;

    s=max;
    next=0;
    while(true)
    {
      // A dictionary entry costs 16 bits in the header and can reduce the
      // classifier's repetition patterns. Eight occurrences is conservative
      // enough to avoid inflating short periodic or high-entropy inputs.
      if(reverse[s] != null && s >= 8 && next < 0xFF )
      {
        for(x in 0...reverse[s].length)
        {
          var value:Int = reverse[s][x];
          indices[value]=++next;
          bits.writeValue(value, 16);
          if(next == 0xFF) break;
        }
      }
      s--;
      if(s<=0)break;
    }

    key.writeValue(next, 8);

    var last:Int = 0;
    var prev:Int = 0;
    var predicted: Array<Array<Int>> = new Array<Array<Int>>();
    for(s in 0...0x100)
    {
        predicted[s] = new Array<Int>();
    }
    var total: Array<Int> = new Array<Int>();
    while(full < inData.length)
    {
      var is:Bool = false;
      x = inData.get(full++);
      if(full < inData.length)
      {
        s = inData.get(full++);
        z = x+(s<<8);
        if(indices[z]>0)
        {
          x = indices[z];
          is = true;
        }
        else
          full--;
      }

      key.writeBool(is);

      key.writeBool(predicted[prev][last]==x);
      if(predicted[prev][last]!=x)
      {
        predicted[prev][last]=x;
        bits.writeValue(x, 8);
      }
      prev = last;
      last = x;
    }
    key.flushWrite();
    key.inter = key.start;
    bits.flushWrite();
    bits.inter = bits.start;
  }

  public static function encode(inData:Bytes):Bytes
  {               
    var key:Bytes = Bytes.alloc(inData.length*4 + 512);
    var keyWriter = new BitWriter(key);
    var bytes:Bytes = Bytes.alloc(inData.length + 512);
    var bytesWriter = new BitWriter(bytes);

    predict(inData, bytesWriter, keyWriter);

    bytes = bytesWriter.toBytes();

    classify(bytes, keyWriter);
    key = keyWriter.toBytes();

    var result:Bytes = Bytes.alloc(key.length + 8);
    result.setInt32(0, inData.length);
    // The high bit marks the sorted-dictionary classifier stream.  Legacy
    // payloads used an unmarked split position and remain decodable below.
    result.setInt32(4, keyWriter.inter | 0x80000000);
    result.blit(8, key, 0, key.length); 
    return result;
  }


  public static function predict_read(inData:Bytes, outData:Bytes, length:Int, key:BitReader):Bytes
  {
    var restack: Array<Array<Int>> = new Array<Array<Int>>();
    var predicted: Array<Array<Bool>> = new Array<Array<Bool>>();
    var indices: Array<Int> = new Array<Int>();
    var bits = new BitReader(inData);
    if(!key.canReadBits(8)) throw "Missing prediction dictionary size";
    var size = key.readValue(8);
    var s:Int;

    for(s in 0...size)
    {
      if(!bits.canReadBits(16)) throw "Incomplete prediction dictionary";
      indices[s+1] = bits.readValue(16);
    }

    for(s in 0...0x100)
    {
      restack[s] = new Array<Int>();
      predicted[s] = new Array<Bool>();
    }

    var decoded:Int = 0;
    var x:Int;
    var prev:Int = 0;
    var last:Int = 0;

    while(decoded < length)
    {
      if(!key.canReadBits(2)) throw "Incomplete prediction key";
      var is:Bool = key.readBool();
      if(key.readBool())
      {
        if(!predicted[prev][last]) throw "Unknown predicted value";
        x = restack[prev][last];
      }
      else
      {
        if(!bits.canReadBits(8)) throw "Missing predicted literal";
        x = bits.readValue(8);
        restack[prev][last] = x;
        predicted[prev][last] = true;
      }

      if(is)
      {
        if(x <= 0 || x > size || decoded + 2 > length)
          throw "Invalid predicted pair";
        outData.set(decoded++, indices[x] & 0xFF);
        outData.set(decoded++, (indices[x] >> 8) & 0xFF);
      }
      else
      {
        if(decoded >= length) throw "Prediction output exceeds its declared size";
        outData.set(decoded++, x);
      }

      prev = last;
      last = x;
    }

    if(bits.remainingBits() != 0) throw "Unexpected prediction data";
    if(key.remainingBits() > 7) throw "Unexpected prediction key data";
    while(key.canRead())
      if(key.readBit() != 0) throw "Invalid prediction key padding";

    return outData.sub(0, decoded);
  }


  public static function declassify(key:BitReader, outData:Bytes, maxLength:Int):Bytes
  {
    var s,x:Int;
    var at:Array<Array<Int>> = new Array<Array<Int>>();
    for(s in 0...0x100) at[s] = new Array<Int>();

    var pos:Int = 0;
    var next:Int = 0;
    var nbit:Int = 1;
    var nmax:Int = 2;

    while(key.canRead())
    {
      if(!key.canReadBits(1)) throw "Invalid classifier stream";
      var have_value = key.readBool();
      if(have_value)
      {
        if(!key.canReadBits(nbit)) throw "Incomplete classifier reference";
        var idx:Int = key.readValue(nbit);
        if(idx <= 0 || idx > next || pos >= maxLength)
          throw "Invalid classifier reference";
        at[idx - 1].push(pos++);

        var evictAt = idx - 1;
        while(true)
        {
          if(!key.canReadBits(1)) throw "Missing classifier eviction marker";
          if(!key.readBool()) break;
          if(!key.canReadBits(8)) throw "Incomplete classifier eviction";
          if(evictAt >= next) throw "Invalid classifier eviction";
          var value:Int = key.readValue(8);
          var positions:Array<Int> = at[evictAt];
          for(x in 0...positions.length)
            outData.set(positions[x], value);
          var z = evictAt;
          while(z < next - 1)
          {
            at[z] = at[z + 1];
            z++;
          }
          at[next - 1] = new Array<Int>();
          next--;
        }
        while(nbit > 1 && next < (nmax >> 1))
        {
          nmax >>= 1;
          nbit--;
        }
      }
      else
      {
        if(!key.canReadBits(1)) throw "Missing classifier end marker";
        if(key.readBool())
        {
          if(!key.canReadBits(next << 3)) throw "Incomplete classifier dictionary";
          for(s in 0...next)
          {
            var value:Int = key.readValue(8);
            var positions:Array<Int> = at[s];
            for(x in 0...positions.length)
              outData.set(positions[x], value);
          }
          if(key.remainingBits() > 7) throw "Unexpected classifier trailing data";
          while(key.canRead())
            if(key.readBit() != 0) throw "Invalid classifier padding";
          return outData.sub(0, pos);
        }

        if(next >= 0x100 || pos >= maxLength)
          throw "Classifier output exceeds its declared size";
        at[next].push(pos++);
        next++;

        if(next == nmax)
        {
          nmax <<= 1;
          nbit++;
        }
      }
    }

    throw "Classifier stream ended before its final dictionary";
  }

  static function declassifyLegacy(key:BitReader, outData:Bytes, maxLength:Int):Bytes
  {
    var s,x:Int;
    var at:Array<Array<Int>> = new Array<Array<Int>>();
    for(s in 0...0x100) at[s] = new Array<Int>();

    var pos:Int = 0;
    var next:Int = 0;
    var nbit:Int = 1;
    var nmax:Int = 2;

    while(key.canRead())
    {
      if(!key.canReadBits(1)) throw "Invalid legacy classifier stream";
      var have_value = key.readBool();
      if(have_value)
      {
        if(!key.canReadBits(nbit + 8)) throw "Incomplete legacy classifier value";
        var idx:Int = key.readValue(nbit);
        var value:Int = key.readValue(8);
        if(idx > next || pos >= maxLength) throw "Invalid legacy classifier value";
        outData.set(pos++, value);
        if(idx > 0)
          idx--;
        else
        {
          if(!key.canReadBits(8)) throw "Incomplete legacy classifier eviction";
          value = key.readValue(8);
        }
        var positions:Array<Int> = at[idx];
        for(x in 0...positions.length)
          outData.set(positions[x], value);
        if(next > 0)
        {
          next--;
          for(s in idx...next)
            at[s] = at[s + 1];
          at[next] = new Array<Int>();
          if(next < (nmax >> 1))
          {
            nmax >>= 1;
            nbit--;
          }
        }
      }
      else
      {
        if(!key.canReadBits(1)) throw "Missing legacy classifier marker";
        var have_index = key.readBool();
        if(have_index)
        {
          if(!key.canReadBits(nbit)) throw "Incomplete legacy classifier index";
          var idx:Int = key.readValue(nbit);
          if(idx == 0)
          {
            if(!key.canReadBits(next << 3)) throw "Incomplete legacy classifier dictionary";
            for(s in 1...(next + 1))
            {
              var value:Int = key.readValue(8);
              var positions:Array<Int> = at[next - s];
              for(x in 0...positions.length)
                outData.set(positions[x], value);
            }
            if(key.remainingBits() > 7) throw "Unexpected legacy classifier trailing data";
            while(key.canRead())
              if(key.readBit() != 0) throw "Invalid legacy classifier padding";
            return outData.sub(0, pos);
          }
          if(idx < 1 || idx > next || pos >= maxLength)
            throw "Invalid legacy classifier index";
          at[idx - 1].push(pos++);
        }
        else
        {
          if(next >= 0x100 || pos >= maxLength)
            throw "Legacy classifier output exceeds its declared size";
          at[next++].push(pos++);
          if(next == nmax)
          {
            nmax <<= 1;
            nbit++;
          }
        }
      }
    }

    throw "Legacy classifier stream ended before its final dictionary";
  }

  public static function decode(inData:Bytes): Bytes
  {
    if(inData.length < 8) throw "Compressed data is missing its header";

    var length      = inData.getInt32(0);
    if(length < 0 || length > MAX_DECODED_LENGTH)
      throw "Compressed data declares an unsupported output size";

    var interField  = inData.getInt32(4);
    var isCurrentClassifier = (interField & 0x80000000) != 0;
    var inter       = interField & 0x7FFFFFFF;
    if(inter > inData.length - 8)
      throw "Compressed data has an invalid stream boundary";

    var keyData     = inData.sub(8, inter);
    var classifyData = inData.sub(8 + inter, inData.length - 8 - inter);

    var keyReader = new BitReader(keyData);

    var bytesDecoded:Bytes = Bytes.alloc(length * 2);
    var classifyReader = new BitReader(classifyData);

    var bytes = isCurrentClassifier
      ? declassify(classifyReader, bytesDecoded, bytesDecoded.length)
      : declassifyLegacy(classifyReader, bytesDecoded, bytesDecoded.length);

    var outData:Bytes = Bytes.alloc(length);
    var result = predict_read(bytes, outData, length, keyReader);

    return result;
  }
}
