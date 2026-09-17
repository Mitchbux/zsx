import haxe.io.Bytes;
import haxe.io.BytesBuffer;

/**
 * Portable implementation of the latest attached ZSX predictor.
 *
 * This class deliberately does not change ZsxArchive or ZsxRange: those retain
 * the established ZSX1/16-byte-chunk wire format.  New data from this API uses
 * the distinct `ZSL1` frame described in ZSX_LATEST_FORMAT.md.
 */
class ZsxLatest {
  public static inline var CHUNK_SIZE:Int = 0xC00000;
  public static inline var MAX_OUTPUT_SIZE:Int = 0x10000000;
  static inline var ARCHIVE_HEADER:Int = 16;
  static inline var CHUNK_HEADER:Int = 12;
  static inline var U32:Float = 4294967296.0;
  static inline var TOP:Float = 16777216.0;

  /** Encodes with portable ZSL1 framing, never with native C size_t framing. */
  public static function encode(data:Bytes):Bytes {
    if (data == null) fail("null input");
    if (data.length > MAX_OUTPUT_SIZE) fail("input exceeds 256 MiB limit");
    var count = data.length == 0 ? 0 : Std.int((data.length + CHUNK_SIZE - 1) / CHUNK_SIZE);
    var chunks = new Array<Bytes>();
    var total:Float = ARCHIVE_HEADER + count * 4;
    var at = 0;
    for (_ in 0...count) {
      var length = data.length - at;
      if (length > CHUNK_SIZE) length = CHUNK_SIZE;
      var chunk = encodeChunk(data.sub(at, length));
      chunks.push(chunk);
      total += chunk.length;
      if (total > 0x7fffffff) fail("encoded archive exceeds supported size");
      at += length;
    }
    var out = Bytes.alloc(Std.int(total));
    out.set(0, "Z".code); out.set(1, "S".code);
    out.set(2, "L".code); out.set(3, "1".code);
    writeU32(out, 4, data.length);
    writeU32(out, 8, count);
    writeU32(out, 12, 0);
    at = ARCHIVE_HEADER;
    for (chunk in chunks) {
      writeU32(out, at, chunk.length);
      at += 4;
      out.blit(at, chunk, 0, chunk.length);
      at += chunk.length;
    }
    return out;
  }

  /**
   * Decodes ZSL1, or a nonempty Windows little-endian C sequence consisting of
   * native 32/64-bit size_t chunk lengths followed by latest 12-byte chunks.
   * ZSX1 is intentionally left to ZsxArchive.decode().
   */
  public static function decode(data:Bytes):Bytes {
    if (data == null) fail("null input");
    if (hasMagic(data, "ZSL1")) return decodePortable(data);
    if (hasMagic(data, "ZSX1"))
      fail("ZSX1 is a different format; use ZsxArchive.decode");
    if (hasMagic(data, "ZSX2"))
      fail("ZSX2 is a multi-file native archive, not a byte stream");
    return decodeNativeFramed(data);
  }

  /**
   * Latest C chunk: u32 original length, u32 payload length, i32 literals.
   * A literal count of -1 denotes an uncompressed payload.
   */
  public static function encodeChunk(data:Bytes):Bytes {
    if (data == null) fail("null chunk");
    if (data.length > CHUNK_SIZE) fail("chunk exceeds 12 MiB");
    if (data.length == 0) return makeChunk(0, data, -1);
    var encoder = new LatestEncoder();
    var literals = encodePredict(data, encoder);
    encoder.flush();
    var payload = encoder.finish();
    return payload.length >= data.length ? makeChunk(data.length, data, -1)
      : makeChunk(data.length, payload, literals);
  }

  public static function decodeChunk(chunk:Bytes):Bytes {
    if (chunk == null) fail("null chunk");
    if (chunk.length < CHUNK_HEADER) fail("truncated latest chunk header");
    var length = readU32(chunk, 0);
    var payloadLength = readU32(chunk, 4);
    var literals = readI32(chunk, 8);
    if (length < 0 || length > CHUNK_SIZE) fail("invalid chunk output length");
    if (payloadLength < 0 || payloadLength != chunk.length - CHUNK_HEADER)
      fail("chunk payload length does not match header");
    if (literals == -1) {
      if (payloadLength != length) fail("raw chunk lengths do not match");
      return chunk.sub(CHUNK_HEADER, length);
    }
    if (length == 0 || literals < 0 || literals > length)
      fail("invalid latest chunk literal count");
    if (payloadLength < 5) fail("truncated latest range stream");
    return decodePredict(chunk.sub(CHUNK_HEADER, payloadLength), length, literals);
  }

  static function decodePortable(data:Bytes):Bytes {
    if (data.length < ARCHIVE_HEADER) fail("truncated ZSL1 header");
    var length = readU32(data, 4);
    var count = readU32(data, 8);
    if (length < 0 || length > MAX_OUTPUT_SIZE) fail("invalid archive output length");
    if (count < 0 || readU32(data, 12) != 0) fail("invalid archive flags or count");
    var expected = length == 0 ? 0 : Std.int((length + CHUNK_SIZE - 1) / CHUNK_SIZE);
    if (count != expected) fail("archive chunk count does not match output length");
    if (count > Std.int((data.length - ARCHIVE_HEADER) / (CHUNK_HEADER + 4)))
      fail("archive chunk table is truncated");
    var chunks = new Array<Bytes>();
    var at = ARCHIVE_HEADER;
    var produced = 0;
    for (_ in 0...count) {
      if (at > data.length - 4) fail("truncated archive chunk length");
      var chunkLength = readU32(data, at);
      at += 4;
      if (chunkLength < CHUNK_HEADER || chunkLength > data.length - at)
        fail("invalid archive chunk length");
      var declared = readU32(data, at);
      var wanted = length - produced;
      if (wanted > CHUNK_SIZE) wanted = CHUNK_SIZE;
      if (declared != wanted) fail("chunk output length does not match archive");
      chunks.push(data.sub(at, chunkLength));
      at += chunkLength;
      produced += declared;
    }
    if (at != data.length || produced != length) fail("trailing or missing archive data");
    return joinDecoded(chunks, length);
  }

  static function decodeNativeFramed(data:Bytes):Bytes {
    if (data.length < 4) fail("truncated native C framing");
    // The supplied source is Windows C. Its size_t fields and int headers are
    // little endian; only sizes below 2^31 and chunks within CHUNK_SIZE are
    // accepted, so this representation has an unambiguous portable subset.
    var frame = data.length >= 8 && readU32(data, 4) == 0 ? 8 : 4;
    var at = 0;
    var length = 0;
    var chunks = new Array<Bytes>();
    while (at < data.length) {
      if (at > data.length - frame) fail("truncated native C chunk length");
      var chunkLength = readU32(data, at);
      if (frame == 8 && readU32(data, at + 4) != 0)
        fail("native C chunk length exceeds supported range");
      at += frame;
      if (chunkLength < CHUNK_HEADER || chunkLength > data.length - at)
        fail("invalid native C chunk length");
      var declared = readU32(data, at);
      if (declared < 0 || declared > CHUNK_SIZE ||
          length > MAX_OUTPUT_SIZE - declared)
        fail("native C output exceeds supported limit");
      chunks.push(data.sub(at, chunkLength));
      at += chunkLength;
      length += declared;
    }
    if (chunks.length == 0) fail("empty native C stream is ambiguous");
    return joinDecoded(chunks, length);
  }

  static function joinDecoded(chunks:Array<Bytes>, length:Int):Bytes {
    var output = Bytes.alloc(length);
    var at = 0;
    for (chunk in chunks) {
      var decoded = decodeChunk(chunk);
      if (decoded.length > length - at) fail("decoded chunk overruns archive");
      output.blit(at, decoded, 0, decoded.length);
      at += decoded.length;
    }
    if (at != length) fail("decoded output length does not match archive");
    return output;
  }

  static function encodePredict(data:Bytes, rc:LatestEncoder):Int {
    var counts = new Array<Int>();
    var values = new Array<Int>();
    var index = new Bytes16(0x10000);
    for (i in 0...0x10000) {
      counts[i] = 0;
      values[i] = i;
    }
    var pairs = data.length >> 1;
    for (i in 0...pairs) {
      var at = i << 1;
      var word = data.get(at) | (data.get(at + 1) << 8);
      counts[word]++;
    }
    quicksort(values, counts, 0, 0x10000);
    var ranked = 0;
    while (ranked < 0xff && counts[values[ranked]] >= 10) ranked++;
    rc.codeDirect(ranked, 8);
    for (i in 0...ranked) {
      var word = values[i];
      rc.codeDirect(word, 16);
      index.set(word, i + 1);
    }

    var model = new LatestModels();
    var previous2 = 0;
    var previous = 0;
    var token = 0;
    var wasIndex = 0;
    var literals = 0;
    var pos = 0;
    while (pos < data.length) {
      var first = data.get(pos++);
      var isIndex = 0;
      if (pos < data.length) {
        var word = first | (data.get(pos) << 8);
        var rank = index.get(word);
        if (rank != 0) {
          first = rank;
          isIndex = 1;
          pos++;
        }
      }
      rc.codeBit(model.pack, token * 2 + wasIndex, isIndex);
      wasIndex = isIndex;
      var h3 = mostContext(previous2, previous, token);
      var top = 0;
      var hitAt = (previous << 8) + token;
      if (model.most.get(h3) != 0) {
        top = model.most.get(h3) - 1 == first ? 1 : 0;
        if (model.tops.get(hitAt) == 0) model.tops.set(hitAt, model.tip.get(token));
        rc.codeBit(model.tops, hitAt, top);
        update(model.tip, token, top);
      }
      if (top == 0) {
        var hit = 0;
        if (model.predicted.get(hitAt) != 0) {
          hit = model.predicted.get(hitAt) - 1 == first ? 1 : 0;
          if (model.hits.get(hitAt) == 0) model.hits.set(hitAt, model.hint.get(token));
          rc.codeBit(model.hits, hitAt, hit);
          update(model.hint, token, hit);
        }
        if (hit == 0) {
          rc.codeByte(model.deep, hitAt << 8, model.miss, token << 8, first);
          literals++;
        }
      }
      model.predicted.set(hitAt, first + 1);
      model.most.set(h3, first + 1);
      previous2 = previous;
      previous = token;
      token = first;
    }
    return literals;
  }

  static function decodePredict(stream:Bytes, length:Int, expectedLiterals:Int):Bytes {
    var rc = new LatestDecoder(stream);
    var index = new Bytes16(0x100);
    var ranked = rc.decodeDirect(8);
    for (i in 1...ranked + 1) index.set(i, rc.decodeDirect(16));
    var model = new LatestModels();
    var result = Bytes.alloc(length);
    var previous2 = 0;
    var previous = 0;
    var token = 0;
    var wasIndex = 0;
    var decoded = 0;
    var literals = 0;
    while (decoded < length) {
      var isIndex = rc.decodeBit(model.pack, token * 2 + wasIndex);
      wasIndex = isIndex;
      var h3 = mostContext(previous2, previous, token);
      var hitAt = (previous << 8) + token;
      var top = 0;
      if (model.most.get(h3) != 0) {
        if (model.tops.get(hitAt) == 0) model.tops.set(hitAt, model.tip.get(token));
        top = rc.decodeBit(model.tops, hitAt);
        update(model.tip, token, top);
      }
      var value:Int;
      if (top != 0) {
        value = model.most.get(h3) - 1;
      } else {
        var hit = 0;
        if (model.predicted.get(hitAt) != 0) {
          if (model.hits.get(hitAt) == 0) model.hits.set(hitAt, model.hint.get(token));
          hit = rc.decodeBit(model.hits, hitAt);
          update(model.hint, token, hit);
        }
        if (hit != 0) value = model.predicted.get(hitAt) - 1;
        else {
          value = rc.decodeByte(model.deep, hitAt << 8, model.miss, token << 8);
          literals++;
          if (literals > expectedLiterals) fail("literal count exceeds header");
        }
      }
      model.predicted.set(hitAt, value + 1);
      model.most.set(h3, value + 1);
      if (isIndex != 0) {
        if (value <= 0 || value > ranked || decoded + 2 > length)
          fail("invalid word rank or output overrun");
        var word = index.get(value);
        result.set(decoded++, word & 0xff);
        result.set(decoded++, word >>> 8);
      } else {
        result.set(decoded++, value);
      }
      previous2 = previous;
      previous = token;
      token = value;
    }
    if (literals != expectedLiterals) fail("literal count does not match header");
    rc.assertFinished();
    return result;
  }

  static function quicksort(values:Array<Int>, counts:Array<Int>, start:Int, length:Int):Void {
    var base = start;
    var size = length;
    while (size > 1) {
      var left = 0;
      var right = size - 1;
      var pivot = counts[values[base + (size >> 1)]];
      while (left <= right) {
        while (counts[values[base + left]] > pivot) left++;
        while (counts[values[base + right]] < pivot) right--;
        if (left > right) break;
        var temp = values[base + left];
        values[base + left] = values[base + right];
        values[base + right] = temp;
        left++;
        right--;
      }
      if (right + 1 < size - left) {
        quicksort(values, counts, base, right + 1);
        base += left;
        size -= left;
      } else {
        quicksort(values, counts, base + left, size - left);
        size = right + 1;
      }
    }
  }

  static inline function mostContext(a:Int, b:Int, c:Int):Int {
    // Written as a Float because the Haxe Int literal 0x9e3779b1 is signed.
    return Std.int(mulU32(a * 65536.0 + b * 256.0 + c, 2654435761.0) / 256.0);
  }

  // Exact low 32 bits of a product without JS Math.imul or 64-bit target APIs.
  static function mulU32(a:Float, b:Float):Float {
    var al = a % 65536.0;
    var ah = Math.floor(a / 65536.0);
    var bl = b % 65536.0;
    var bh = Math.floor(b / 65536.0);
    return (al * bl + ((ah * bl + al * bh) % 65536.0) * 65536.0) % U32;
  }

  public static inline function unsigned(value:Int):Float {
    return value < 0 ? value + U32 : value;
  }

  // Keep the range coder's 32-bit registers in signed Int form.  This avoids
  // depending on a target's treatment of overflowing integer expressions.
  public static inline function signedU32(value:Float):Int {
    value = value % U32;
    return value > 2147483647.0 ? Std.int(value - U32) : Std.int(value);
  }

  public static inline function update(values:Bytes16, at:Int, bit:Int):Void {
    var p = values.get(at);
    values.set(at, bit == 0 ? p + ((2048 - p) >> 4) : p - (p >> 4));
  }

  static function makeChunk(length:Int, payload:Bytes, literals:Int):Bytes {
    var chunk = Bytes.alloc(CHUNK_HEADER + payload.length);
    writeU32(chunk, 0, length);
    writeU32(chunk, 4, payload.length);
    writeI32(chunk, 8, literals);
    if (payload.length != 0) chunk.blit(CHUNK_HEADER, payload, 0, payload.length);
    return chunk;
  }

  static inline function hasMagic(data:Bytes, magic:String):Bool {
    return data.length >= 4 && data.get(0) == magic.charCodeAt(0) &&
      data.get(1) == magic.charCodeAt(1) && data.get(2) == magic.charCodeAt(2) &&
      data.get(3) == magic.charCodeAt(3);
  }

  static inline function readU32(data:Bytes, at:Int):Int {
    return data.get(at) | (data.get(at + 1) << 8) |
      (data.get(at + 2) << 16) | (data.get(at + 3) << 24);
  }

  static inline function readI32(data:Bytes, at:Int):Int return readU32(data, at);

  static inline function writeU32(data:Bytes, at:Int, value:Int):Void {
    data.set(at, value);
    data.set(at + 1, value >>> 8);
    data.set(at + 2, value >>> 16);
    data.set(at + 3, value >>> 24);
  }

  static inline function writeI32(data:Bytes, at:Int, value:Int):Void {
    writeU32(data, at, value);
  }

  public static function fail(message:String):Dynamic {
    throw new haxe.Exception("Invalid latest ZSX data: " + message);
  }
}

private class Bytes16 {
  var data:Bytes;
  public function new(length:Int, ?initial:Int = 0) {
    data = Bytes.alloc(length * 2);
    if (initial != 0) for (i in 0...length) set(i, initial);
  }
  public inline function get(at:Int):Int return data.get(at << 1) | (data.get((at << 1) + 1) << 8);
  public inline function set(at:Int, value:Int):Void {
    data.set(at << 1, value);
    data.set((at << 1) + 1, value >>> 8);
  }
}

private class LatestModels {
  public var predicted:Bytes16 = new Bytes16(0x10000);
  public var pack:Bytes16 = new Bytes16(0x200, 1024);
  public var hits:Bytes16 = new Bytes16(0x10000);
  public var hint:Bytes16 = new Bytes16(0x100, 1024);
  public var miss:Bytes16 = new Bytes16(0x10000, 1024);
  public var deep:Bytes16 = new Bytes16(0x1000000);
  public var most:Bytes16 = new Bytes16(0x1000000);
  public var tops:Bytes16 = new Bytes16(0x10000);
  public var tip:Bytes16 = new Bytes16(0x100, 1024);
  public function new() {}
}

private class LatestEncoder {
  var low:Float = 0;
  var range:Int = -1;
  var cache:Int = 0;
  var pending:Int = 1;
  var out:BytesBuffer = new BytesBuffer();
  public function new() {}
  public function codeBit(probabilities:Bytes16, at:Int, bit:Int):Void {
    var bound = ZsxLatest.signedU32(Math.floor(ZsxLatest.unsigned(range) / 2048.0) * probabilities.get(at));
    if (bit == 0) range = bound;
    else {
      low += ZsxLatest.unsigned(bound);
      range = ZsxLatest.signedU32(ZsxLatest.unsigned(range) - ZsxLatest.unsigned(bound));
    }
    ZsxLatest.update(probabilities, at, bit);
    while (ZsxLatest.unsigned(range) < 16777216.0) {
      range = ZsxLatest.signedU32(ZsxLatest.unsigned(range) * 256.0);
      carry();
    }
  }
  public function codeDirect(value:Int, bits:Int):Void {
    var n = bits;
    while (n-- > 0) {
      range = Std.int(ZsxLatest.unsigned(range) / 2.0);
      if (((value >>> n) & 1) != 0) low += ZsxLatest.unsigned(range);
      while (ZsxLatest.unsigned(range) < 16777216.0) {
        range = ZsxLatest.signedU32(ZsxLatest.unsigned(range) * 256.0);
        carry();
      }
    }
  }
  public function codeByte(deep:Bytes16, deepBase:Int, wide:Bytes16, wideBase:Int, value:Int):Void {
    var node = 1;
    var bitAt = 7;
    while (bitAt >= 0) {
      var bit = (value >>> bitAt) & 1;
      if (deep.get(deepBase + node) == 0) deep.set(deepBase + node, wide.get(wideBase + node));
      codeBit(deep, deepBase + node, bit);
      ZsxLatest.update(wide, wideBase + node, bit);
      node = (node << 1) + bit;
      bitAt--;
    }
  }
  function carry():Void {
    var high = Std.int(Math.floor(low / 4294967296.0));
    var low32 = low - high * 4294967296.0;
    if (high != 0 || low32 < 4278190080.0) {
      var value = cache;
      do {
        out.addByte((value + high) & 0xff);
        value = 0xff;
        pending--;
      } while (pending != 0);
      cache = Std.int(Math.floor(low32 / 16777216.0)) & 0xff;
    }
    pending++;
    low = (low32 * 256.0) % 4294967296.0;
  }
  public function flush():Void for (_ in 0...5) carry();
  public function finish():Bytes return out.getBytes();
}

private class LatestDecoder {
  var range:Int = -1;
  var code:Int = 0;
  var stream:Bytes;
  var at:Int = 1;
  public function new(stream:Bytes) {
    this.stream = stream;
    if (stream.length < 5) ZsxLatest.fail("truncated latest range stream");
    for (_ in 0...4) code = ZsxLatest.signedU32(ZsxLatest.unsigned(code) * 256.0 + next());
  }
  public function decodeBit(probabilities:Bytes16, index:Int):Int {
    var bound = ZsxLatest.signedU32(Math.floor(ZsxLatest.unsigned(range) / 2048.0) * probabilities.get(index));
    var bit:Int;
    if (ZsxLatest.unsigned(code) < ZsxLatest.unsigned(bound)) {
      range = bound;
      bit = 0;
    } else {
      code = ZsxLatest.signedU32(ZsxLatest.unsigned(code) - ZsxLatest.unsigned(bound));
      range = ZsxLatest.signedU32(ZsxLatest.unsigned(range) - ZsxLatest.unsigned(bound));
      bit = 1;
    }
    ZsxLatest.update(probabilities, index, bit);
    while (ZsxLatest.unsigned(range) < 16777216.0) {
      range = ZsxLatest.signedU32(ZsxLatest.unsigned(range) * 256.0);
      code = ZsxLatest.signedU32(ZsxLatest.unsigned(code) * 256.0 + next());
    }
    return bit;
  }
  public function decodeDirect(bits:Int):Int {
    var value = 0;
    var n = bits;
    while (n-- > 0) {
      range = Std.int(ZsxLatest.unsigned(range) / 2.0);
      var bit = 0;
      if (ZsxLatest.unsigned(code) >= ZsxLatest.unsigned(range)) {
        code = ZsxLatest.signedU32(ZsxLatest.unsigned(code) - ZsxLatest.unsigned(range));
        bit = 1;
      }
      value = (value << 1) | bit;
      while (ZsxLatest.unsigned(range) < 16777216.0) {
        range = ZsxLatest.signedU32(ZsxLatest.unsigned(range) * 256.0);
        code = ZsxLatest.signedU32(ZsxLatest.unsigned(code) * 256.0 + next());
      }
    }
    return value;
  }
  public function decodeByte(deep:Bytes16, deepBase:Int, wide:Bytes16, wideBase:Int):Int {
    var node = 1;
    while (node < 0x100) {
      if (deep.get(deepBase + node) == 0) deep.set(deepBase + node, wide.get(wideBase + node));
      var bit = decodeBit(deep, deepBase + node);
      ZsxLatest.update(wide, wideBase + node, bit);
      node = (node << 1) + bit;
    }
    return node - 0x100;
  }
  function next():Int {
    if (at >= stream.length) ZsxLatest.fail("truncated latest range stream");
    return stream.get(at++);
  }
  public function assertFinished():Void {
    if (at != stream.length || code != 0) ZsxLatest.fail("latest range stream did not end canonically");
  }
}