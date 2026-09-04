import haxe.io.Bytes;
import haxe.io.BytesBuffer;
import js.lib.Int32Array;
import js.lib.Uint16Array;

/**
 * JavaScript implementation of the range-coded ZSX chunk format.
 *
 * This is deliberately a chunk codec: the size_t framing used by the sample
 * file program is outside this class.
 */
class ZsxRange {
  static inline var HEADER_SIZE:Int = 16;
  static inline var MAX_CHUNK:Int = 0xC00000;
  static inline var RANK_MIN:Int = 10;

  public static function encodeChunk(input:Bytes):Bytes {
    if (input == null) fail("null input");
    if (input.length > MAX_CHUNK) fail("chunk exceeds 12 MiB");

    if (input.length == 0)
      return makeChunk(0, Bytes.alloc(0), -1);

    var rc = new ZsxRangeEncoder();
    var literalCount = encodePredict(input, rc);
    rc.flush();
    var coded = rc.finish();

    if (coded.length >= input.length)
      return makeChunk(input.length, input, -1);
    return makeChunk(input.length, coded, literalCount);
  }

  public static function decodeChunk(chunk:Bytes):Bytes {
    if (chunk == null) fail("null input");
    if (chunk.length < HEADER_SIZE) fail("truncated ZSX header");

    var originalLength = readI32LE(chunk, 0);
    var streamLength = readI32LE(chunk, 4);
    var literalCount = readI32LE(chunk, 8);
    var crc = readI32LE(chunk, 12);

    if (originalLength < 0 || originalLength > MAX_CHUNK)
      fail("invalid original length");
    if (streamLength < 0 || streamLength > chunk.length - HEADER_SIZE)
      fail("invalid stream length");
    if (chunk.length != HEADER_SIZE + streamLength)
      fail("chunk length does not match header");
    if (crc != 0) fail("unsupported nonzero CRC");

    if (literalCount == -1) {
      if (streamLength != originalLength)
        fail("raw chunk lengths do not match");
      var raw = Bytes.alloc(originalLength);
      if (originalLength != 0)
        raw.blit(0, chunk, HEADER_SIZE, originalLength);
      return raw;
    }

    if (originalLength == 0)
      fail("empty chunks must use raw representation");
    if (literalCount < 0 || literalCount > originalLength)
      fail("invalid literal count");
    // A real coded stream always includes the priming byte and five-byte flush.
    if (streamLength < 5) fail("truncated range stream");

    var payload = chunk.sub(HEADER_SIZE, streamLength);
    return decodePredict(payload, originalLength, literalCount);
  }

  static function encodePredict(data:Bytes, rc:ZsxRangeEncoder):Int {
    var count = new Int32Array(0x10000);
    var value = new Int32Array(0x10000);
    var index = new Uint16Array(0x10000);
    var pairs = data.length >> 1;
    for (i in 0...pairs) {
      var p = i << 1;
      var word = data.get(p) | (data.get(p + 1) << 8);
      count[word] = count[word] + 1;
    }
    for (i in 0...0x10000) value[i] = i;
    quicksortCount(value, count, 0, 0x10000);

    var ranked = 0;
    while (ranked < 0xff && count[value[ranked]] >= RANK_MIN) ranked++;
    rc.codeDirect(ranked, 8);
    for (i in 0...ranked) {
      var word = value[i];
      rc.codeDirect(word, 16);
      index[word] = i + 1;
    }

    var m = new ZsxRangeModels();
    var was = 0;
    var r = 0;
    var s = 0;
    var x = 0;
    var pos = 0;
    var literals = 0;

    while (pos < data.length) {
      var first = data.get(pos++);
      var isIndex = 0;
      if (pos < data.length) {
        var word = first | (data.get(pos) << 8);
        var ix = index[word];
        if (ix != 0) {
          first = ix;
          isIndex = 1;
          pos++;
        }
      }

      var packAt = x * 2 + was;
      rc.codeBit(m.pack, packAt, isIndex);
      was = isIndex;

      var h3 = mostContext(r, s, x);
      var top = 0;
      if (m.most[h3] != 0) {
        top = m.most[h3] - 1 == first ? 1 : 0;
        var at = (s << 8) + x;
        if (m.tops[at] == 0) m.tops[at] = m.tip[x];
        rc.codeBit(m.tops, at, top);
        update(m.tip, x, top);
      }

      if (top == 0) {
        var predictionAt = (s << 8) + x;
        var hit = 0;
        if (m.predicted[predictionAt] != 0) {
          hit = m.predicted[predictionAt] - 1 == first ? 1 : 0;
          if (m.hits[predictionAt] == 0) m.hits[predictionAt] = m.hint[x];
          rc.codeBit(m.hits, predictionAt, hit);
          update(m.hint, x, hit);
        }
        if (hit == 0) {
          rc.codeByte(m.deep, ((s << 8) + x) << 8, m.miss, x << 8, first);
          literals++;
        }
      }

      m.predicted[(s << 8) + x] = first + 1;
      m.most[h3] = first + 1;
      r = s;
      s = x;
      x = first;
    }
    return literals;
  }

  static function decodePredict(stream:Bytes, length:Int, expectedLiterals:Int):Bytes {
    var rc = new ZsxRangeDecoder(stream);
    var index = new Uint16Array(0x100);
    var m = new ZsxRangeModels();

    var ranked = rc.decodeDirect(8);
    for (i in 1...ranked + 1)
      index[i] = rc.decodeDirect(16);

    var result = Bytes.alloc(length);
    var decoded = 0;
    var literals = 0;
    var was = 0;
    var r = 0;
    var s = 0;
    var x = 0;

    while (decoded < length) {
      var isIndex = rc.decodeBit(m.pack, x * 2 + was);
      was = isIndex;

      var h3 = mostContext(r, s, x);
      var top = 0;
      var z:Int;
      if (m.most[h3] != 0) {
        var at = (s << 8) + x;
        if (m.tops[at] == 0) m.tops[at] = m.tip[x];
        top = rc.decodeBit(m.tops, at);
        update(m.tip, x, top);
      }

      if (top != 0) {
        z = m.most[h3] - 1;
      } else {
        var predictionAt = (s << 8) + x;
        var hit = 0;
        if (m.predicted[predictionAt] != 0) {
          if (m.hits[predictionAt] == 0) m.hits[predictionAt] = m.hint[x];
          hit = rc.decodeBit(m.hits, predictionAt);
          update(m.hint, x, hit);
        }
        if (hit != 0) {
          z = m.predicted[predictionAt] - 1;
        } else {
          z = rc.decodeByte(m.deep, ((s << 8) + x) << 8, m.miss, x << 8);
          literals++;
          if (literals > expectedLiterals) fail("literal count exceeds header");
        }
      }

      m.predicted[(s << 8) + x] = z + 1;
      m.most[h3] = z + 1;

      if (isIndex != 0) {
        if (z <= 0 || z > ranked) fail("invalid word rank");
        if (decoded + 2 > length) fail("word token overruns output");
        var word = index[z];
        result.set(decoded++, word & 0xff);
        result.set(decoded++, word >>> 8);
      } else {
        result.set(decoded++, z);
      }
      r = s;
      s = x;
      x = z;
    }

    if (literals != expectedLiterals)
      fail("literal count does not match header");
    rc.assertFinished();
    return result;
  }

  // Faithful Hoare partition, including its tie ordering (dictionary order is
  // part of the compressed wire format).
  static function quicksortCount(list:Int32Array, count:Int32Array, start:Int, len:Int):Void {
    var base = start;
    var size = len;
    while (size > 1) {
      var left = 0;
      var right = size - 1;
      var pivot = count[list[base + (size >> 1)]];
      while (left <= right) {
        while (count[list[base + left]] > pivot) left++;
        while (count[list[base + right]] < pivot) right--;
        if (left > right) break;
        var t = list[base + left];
        list[base + left] = list[base + right];
        list[base + right] = t;
        left++;
        right--;
      }
      if (right + 1 < size - left) {
        quicksortCount(list, count, base, right + 1);
        base += left;
        size -= left;
      } else {
        quicksortCount(list, count, base + left, size - left);
        size = right + 1;
      }
    }
  }

  static inline function mostContext(r:Int, s:Int, x:Int):Int {
    var key = (r << 16) + (s << 8) + x;
    return js.Syntax.code("Math.imul({0}, 0x9E3779B1) >>> 8", key);
  }

  @:allow(ZsxRangeEncoder)
  @:allow(ZsxRangeDecoder)
  static inline function update(prob:Uint16Array, at:Int, bit:Int):Void {
    var p = prob[at];
    prob[at] = bit == 0 ? p + ((2048 - p) >> 4) : p - (p >> 4);
  }

  static function makeChunk(originalLength:Int, payload:Bytes, literals:Int):Bytes {
    var out = Bytes.alloc(HEADER_SIZE + payload.length);
    writeI32LE(out, 0, originalLength);
    writeI32LE(out, 4, payload.length);
    writeI32LE(out, 8, literals);
    writeI32LE(out, 12, 0);
    if (payload.length != 0) out.blit(HEADER_SIZE, payload, 0, payload.length);
    return out;
  }

  static inline function readI32LE(b:Bytes, at:Int):Int {
    return b.get(at) | (b.get(at + 1) << 8) | (b.get(at + 2) << 16) | (b.get(at + 3) << 24);
  }

  static inline function writeI32LE(b:Bytes, at:Int, value:Int):Void {
    b.set(at, value);
    b.set(at + 1, value >>> 8);
    b.set(at + 2, value >>> 16);
    b.set(at + 3, value >>> 24);
  }

  @:allow(ZsxRangeDecoder)
  static function fail(message:String):Dynamic {
    throw new haxe.Exception("Invalid ZSX chunk: " + message);
  }
}

private class ZsxRangeModels {
  public var predicted:Uint16Array;
  public var pack:Uint16Array;
  public var hits:Uint16Array;
  public var hint:Uint16Array;
  public var miss:Uint16Array;
  public var deep:Uint16Array;
  public var most:Uint16Array;
  public var tops:Uint16Array;
  public var tip:Uint16Array;

  public function new() {
    predicted = new Uint16Array(0x10000);
    pack = probabilities(0x200);
    hits = new Uint16Array(0x10000);
    hint = probabilities(0x100);
    miss = probabilities(0x10000);
    deep = new Uint16Array(0x1000000);
    most = new Uint16Array(0x1000000);
    tops = new Uint16Array(0x10000);
    tip = probabilities(0x100);
  }

  static function probabilities(size:Int):Uint16Array {
    var result = new Uint16Array(size);
    result.fill(1024);
    return result;
  }
}

private class ZsxRangeEncoder {
  static inline var TOP:Int = 0x1000000;
  var low:Float = 0;
  var range:Int = -1;
  var cache:Int = 0;
  var pending:Int = 1;
  var output:BytesBuffer = new BytesBuffer();

  public function new() {}

  public function codeBit(prob:Uint16Array, at:Int, bit:Int):Void {
    var p = prob[at];
    var bound:Int = js.Syntax.code("((({0} >>> 11) * {1}) | 0)", range, p);
    if (bit == 0) {
      range = bound;
    } else {
      low += unsigned(bound);
      range = range - bound;
    }
    ZsxRange.update(prob, at, bit);
    while (unsigned(range) < TOP) {
      range = range << 8;
      carry();
    }
  }

  public function codeByte(deep:Uint16Array, deepBase:Int, wide:Uint16Array, wideBase:Int, value:Int):Void {
    var node = 1;
    var i = 7;
    while (i >= 0) {
      var bit = (value >>> i) & 1;
      if (deep[deepBase + node] == 0)
        deep[deepBase + node] = wide[wideBase + node];
      codeBit(deep, deepBase + node, bit);
      ZsxRange.update(wide, wideBase + node, bit);
      node = (node << 1) + bit;
      i--;
    }
  }

  public function codeDirect(value:Int, bits:Int):Void {
    var n = bits;
    while (n-- > 0) {
      range = range >>> 1;
      if (((value >>> n) & 1) != 0) low += unsigned(range);
      while (unsigned(range) < TOP) {
        range = range << 8;
        carry();
      }
    }
  }

  function carry():Void {
    var high = Math.floor(low / 4294967296.0);
    var low32 = low - high * 4294967296.0;
    if (high != 0 || low32 < 4278190080.0) {
      var t = cache;
      do {
        output.addByte((t + Std.int(high)) & 0xff);
        t = 0xff;
        pending--;
      } while (pending != 0);
      cache = Std.int(Math.floor(low32 / 16777216.0)) & 0xff;
    }
    pending++;
    low = (low32 * 256.0) % 4294967296.0;
  }

  public function flush():Void {
    for (_ in 0...5) carry();
  }

  public function finish():Bytes {
    return output.getBytes();
  }

  static inline function unsigned(v:Int):Float {
    return v < 0 ? v + 4294967296.0 : v;
  }
}

private class ZsxRangeDecoder {
  static inline var TOP:Int = 0x1000000;
  var range:Int = -1;
  var code:Int = 0;
  var data:Bytes;
  var pos:Int = 1;

  public function new(data:Bytes) {
    this.data = data;
    if (data.length < 5) ZsxRange.fail("truncated range stream");
    // Byte zero is the coder's primed cache and is intentionally skipped.
    for (_ in 0...4) code = (code << 8) | next();
  }

  public function decodeBit(prob:Uint16Array, at:Int):Int {
    var p = prob[at];
    var bound:Int = js.Syntax.code("((({0} >>> 11) * {1}) | 0)", range, p);
    var bit:Int;
    if (unsignedLess(code, bound)) {
      range = bound;
      bit = 0;
    } else {
      code = code - bound;
      range = range - bound;
      bit = 1;
    }
    ZsxRange.update(prob, at, bit);
    while (unsigned(range) < TOP) {
      range = range << 8;
      code = (code << 8) | next();
    }
    return bit;
  }

  public function decodeDirect(bits:Int):Int {
    var value = 0;
    var n = bits;
    while (n-- > 0) {
      range = range >>> 1;
      var bit = 0;
      if (!unsignedLess(code, range)) {
        code = code - range;
        bit = 1;
      }
      value = (value << 1) | bit;
      while (unsigned(range) < TOP) {
        range = range << 8;
        code = (code << 8) | next();
      }
    }
    return value;
  }

  public function decodeByte(deep:Uint16Array, deepBase:Int, wide:Uint16Array, wideBase:Int):Int {
    var node = 1;
    while (node < 0x100) {
      if (deep[deepBase + node] == 0)
        deep[deepBase + node] = wide[wideBase + node];
      var bit = decodeBit(deep, deepBase + node);
      ZsxRange.update(wide, wideBase + node, bit);
      node = (node << 1) + bit;
    }
    return node - 0x100;
  }

  function next():Int {
    if (pos >= data.length) ZsxRange.fail("truncated range stream");
    return data.get(pos++);
  }

  public function assertFinished():Void {
    if (pos != data.length || code != 0)
      ZsxRange.fail("range stream did not end canonically");
  }

  static inline function unsigned(v:Int):Float {
    return v < 0 ? v + 4294967296.0 : v;
  }

  static inline function unsignedLess(a:Int, b:Int):Bool {
    return (a ^ 0x80000000) < (b ^ 0x80000000);
  }
}