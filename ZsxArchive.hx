import haxe.io.Bytes;
/**
 * Portable multi-chunk ZSX archive.
 *
 * Layout:
 *   4 bytes  magic "ZSX1"
 *   u32 LE   original byte length
 *   u32 LE   chunk count
 *   u32 LE   reserved (zero)
 *   repeated: u32 LE chunk length, followed by one C-compatible ZSX chunk
 *
 * Decode also accepts the attached C CLI's native size_t framing in both
 * 64-bit and 32-bit forms. New browser archives always use the portable form.
 */
class ZsxArchive {
  public static inline var CHUNK_SIZE:Int = 0xC00000;
  public static inline var MAX_OUTPUT_SIZE:Int = 0x10000000;
  static inline var HEADER_SIZE:Int = 16;
  static inline var ENTRY_SIZE:Int = 4;

  public static function encode(input:Bytes, ?progress:(Int, Int) -> Void):Bytes {
    if (input == null) fail("null input");
    if (input.length > MAX_OUTPUT_SIZE) fail("input exceeds 256 MiB browser limit");

    var chunkCount = input.length == 0 ? 0 : Std.int(Math.ceil(input.length / CHUNK_SIZE));
    var chunks = new Array<Bytes>();
    var payloadSize:Float = HEADER_SIZE + chunkCount * ENTRY_SIZE;
    var offset = 0;

    for (i in 0...chunkCount) {
      var length = input.length - offset;
      if (length > CHUNK_SIZE) length = CHUNK_SIZE;
      var encoded = ZsxRange.encodeChunk(input.sub(offset, length));
      chunks.push(encoded);
      payloadSize += encoded.length;
      if (payloadSize > 0x7fffffff) fail("archive exceeds supported size");
      offset += length;
      if (progress != null) progress(offset, input.length);
    }

    return packChunks(input.length, chunks);
  }

  public static function packChunks(originalLength:Int, chunks:Array<Bytes>):Bytes {
    if (originalLength < 0 || originalLength > MAX_OUTPUT_SIZE)
      fail("input exceeds 256 MiB browser limit");
    var expectedChunks = originalLength == 0 ? 0 : Std.int(Math.ceil(originalLength / CHUNK_SIZE));
    if (chunks == null || chunks.length != expectedChunks)
      fail("encoded chunk count does not match original length");

    var payloadSize:Float = HEADER_SIZE + chunks.length * ENTRY_SIZE;
    for (chunk in chunks) {
      if (chunk == null || chunk.length < 16) fail("invalid encoded chunk");
      payloadSize += chunk.length;
      if (payloadSize > 0x7fffffff) fail("archive exceeds supported size");
    }

    var output = Bytes.alloc(Std.int(payloadSize));
    output.set(0, "Z".code);
    output.set(1, "S".code);
    output.set(2, "X".code);
    output.set(3, "1".code);
    writeU32(output, 4, originalLength);
    writeU32(output, 8, chunks.length);
    writeU32(output, 12, 0);

    var offset = HEADER_SIZE;
    for (chunk in chunks) {
      writeU32(output, offset, chunk.length);
      offset += ENTRY_SIZE;
      output.blit(offset, chunk, 0, chunk.length);
      offset += chunk.length;
    }
    return output;
  }

  public static function decode(archive:Bytes, ?progress:(Int, Int) -> Void):Bytes {
    if (archive == null) fail("null input");
    if (archive.length >= 4 && archive.get(0) == "Z".code && archive.get(1) == "S".code
      && archive.get(2) == "X".code && archive.get(3) == "1".code)
      return decodePortable(archive, progress);
    return decodeCFramed(archive, progress);
  }

  static function decodePortable(archive:Bytes, ?progress:(Int, Int) -> Void):Bytes {
    if (archive.length < HEADER_SIZE) fail("truncated archive header");

    var originalLength = readU32(archive, 4);
    var chunkCount = readU32(archive, 8);
    var reserved = readU32(archive, 12);
    if (originalLength < 0) fail("original length exceeds supported size");
    if (originalLength > MAX_OUTPUT_SIZE) fail("output exceeds 256 MiB browser limit");
    if (chunkCount < 0) fail("chunk count exceeds supported size");
    if (reserved != 0) fail("unsupported archive flags");

    var expectedChunks = originalLength == 0 ? 0 : Std.int(Math.ceil(originalLength / CHUNK_SIZE));
    if (chunkCount != expectedChunks) fail("chunk count does not match original length");
    if (chunkCount > Std.int((archive.length - HEADER_SIZE) / (ENTRY_SIZE + 16)))
      fail("chunk table exceeds archive");

    var inputAt = HEADER_SIZE;
    var declaredOutput = 0;

    for (i in 0...chunkCount) {
      if (inputAt > archive.length - ENTRY_SIZE) fail("truncated chunk length");
      var chunkLength = readU32(archive, inputAt);
      inputAt += ENTRY_SIZE;
      if (chunkLength < 16 || chunkLength > archive.length - inputAt)
        fail("invalid chunk length");
      var chunkOutput = readU32(archive, inputAt);
      var expectedLength = originalLength - declaredOutput;
      if (expectedLength > CHUNK_SIZE) expectedLength = CHUNK_SIZE;
      if (chunkOutput != expectedLength) fail("chunk header does not match archive");
      declaredOutput += chunkOutput;
      inputAt += chunkLength;
    }
    if (inputAt != archive.length) fail("trailing archive data");
    if (declaredOutput != originalLength) fail("declared length does not match archive");

    var output = Bytes.alloc(originalLength);
    inputAt = HEADER_SIZE;
    var outputAt = 0;
    for (i in 0...chunkCount) {
      var chunkLength = readU32(archive, inputAt);
      inputAt += ENTRY_SIZE;
      var decoded = ZsxRange.decodeChunk(archive.sub(inputAt, chunkLength));
      output.blit(outputAt, decoded, 0, decoded.length);
      outputAt += decoded.length;
      inputAt += chunkLength;
      if (progress != null) progress(outputAt, originalLength);
    }

    if (outputAt != originalLength) fail("decoded length does not match archive");
    return output;
  }

  static function decodeCFramed(archive:Bytes, ?progress:(Int, Int) -> Void):Bytes {
    if (archive.length < 4) fail("truncated C archive framing");
    var frameSize = 4;
    if (archive.length >= 8 && readU32(archive, 4) == 0) frameSize = 8;

    var chunkOffsets = new Array<Int>();
    var chunkLengths = new Array<Int>();
    var at = 0;
    var originalLength = 0;
    while (at < archive.length) {
      if (at > archive.length - frameSize) fail("truncated C chunk length");
      var chunkLength = readU32(archive, at);
      if (frameSize == 8 && readU32(archive, at + 4) != 0)
        fail("C chunk length exceeds supported size");
      at += frameSize;
      if (chunkLength < 16 || chunkLength > archive.length - at)
        fail("invalid C chunk length");
      var chunkOutput = readU32(archive, at);
      if (chunkOutput < 0 || chunkOutput > CHUNK_SIZE)
        fail("invalid C chunk output length");
      if (originalLength > MAX_OUTPUT_SIZE - chunkOutput)
        fail("output exceeds 256 MiB browser limit");
      originalLength += chunkOutput;
      chunkOffsets.push(at);
      chunkLengths.push(chunkLength);
      at += chunkLength;
    }
    if (chunkOffsets.length == 0) fail("empty C archive");

    var output = Bytes.alloc(originalLength);
    var outputAt = 0;
    for (i in 0...chunkOffsets.length) {
      var decoded = ZsxRange.decodeChunk(archive.sub(chunkOffsets[i], chunkLengths[i]));
      output.blit(outputAt, decoded, 0, decoded.length);
      outputAt += decoded.length;
      if (progress != null) progress(outputAt, originalLength);
    }
    return output;
  }

  static inline function readU32(bytes:Bytes, at:Int):Int {
    return bytes.get(at)
      | (bytes.get(at + 1) << 8)
      | (bytes.get(at + 2) << 16)
      | (bytes.get(at + 3) << 24);
  }

  static inline function writeU32(bytes:Bytes, at:Int, value:Int):Void {
    bytes.set(at, value);
    bytes.set(at + 1, value >>> 8);
    bytes.set(at + 2, value >>> 16);
    bytes.set(at + 3, value >>> 24);
  }

  static function fail(message:String):Dynamic {
    throw new haxe.Exception("Invalid ZSX archive: " + message);
  }
}