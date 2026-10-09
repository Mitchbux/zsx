package zsd;

import haxe.io.Bytes;
import haxe.io.BytesOutput;
import zsd.ZsdBinary.ZsdReader;

/**
 * ZSX3 v1: one ZSL1 stream over the complete ZSDP v1 record stream.
 * See ZSX3_FORMAT.md. ZSDC v1 remains a separate, read-only legacy format.
 */
class ZsdContainer {
  static inline var VERSION = 1;
  public static inline var HEADER_BYTES = 54;

  public static function pack(archive:Bytes):Bytes {
    if (archive.length > ZsxLatest.MAX_OUTPUT_SIZE)
      ZsdBinary.fail("archive exceeds ZsxLatest's 256 MiB input bound");
    requireArchiveHeader(archive);
    // Exactly one compressor call, after all paths and file bytes are assembled.
    // ZSL1's internal 12 MiB blocks are not aligned to individual files.
    var compressed = ZsxLatest.encode(archive);
    if (compressed.length > ZsdBinary.MAX_BYTES - HEADER_BYTES) ZsdBinary.fail("compressed archive exceeds ZSD bounds");
    var out = new BytesOutput();
    out.writeString("ZSX3");
    ZsdBinary.u16(out, VERSION);
    ZsdBinary.length(out, archive.length);
    ZsdBinary.raw(out, ZsdBinary.digest(archive));
    ZsdBinary.length(out, compressed.length);
    ZsdBinary.raw(out, compressed);
    return out.getBytes();
  }

  public static function unpack(container:Bytes):Bytes {
    if (container.length > ZsdBinary.MAX_BYTES) ZsdBinary.fail("container exceeds ZSD bounds");
    var input = new ZsdReader(container);
    var magic = input.raw(4).toString();
    if (magic != "ZSX3" && magic != "ZSDC")
      ZsdBinary.fail("not a ZSX3 or legacy ZSDC container");
    if (input.u16() != VERSION) ZsdBinary.fail("unsupported " + magic + " container version");
    var originalLength = input.length();
    if (originalLength > ZsxLatest.MAX_OUTPUT_SIZE)
      ZsdBinary.fail("archive exceeds ZsxLatest's 256 MiB output bound");
    var originalDigest = input.raw(32);
    var compressedLength = input.length();
    if (compressedLength != container.length - HEADER_BYTES)
      ZsdBinary.fail("compressed length does not match container");
    var compressed = input.raw(compressedLength);
    input.end();
    if (magic == "ZSX3") {
      // Version 1 fixes the codec to ZSL1, never native size_t framing.
      if (compressed.length < 16 || compressed.sub(0, 4).toString() != "ZSL1")
        ZsdBinary.fail("ZSX3 v1 requires the ZSL1 codec");
      var codecLength = compressed.get(4) | (compressed.get(5) << 8) |
        (compressed.get(6) << 16) | (compressed.get(7) << 24);
      if (codecLength != originalLength)
        ZsdBinary.fail("ZSX3 and ZSL1 output lengths disagree");
    }
    var archive:Bytes = try ZsxLatest.decode(compressed)
    catch (e:Dynamic) throw "ZSD: ZsxLatest decompression failed: " + Std.string(e);
    if (archive.length != originalLength) ZsdBinary.fail("decompressed length does not match container");
    if (!ZsdBinary.equals(originalDigest, ZsdBinary.digest(archive)))
      ZsdBinary.fail("decompressed digest does not match container");
    if (magic == "ZSX3") requireArchiveHeader(archive);
    return archive;
  }

  static function requireArchiveHeader(archive:Bytes):Void {
    var input = new ZsdReader(archive);
    if (input.raw(4).toString() != "ZSDP" || input.u16() != VERSION)
      ZsdBinary.fail("ZSX3 v1 requires a ZSDP v1 archive");
    input.require(4); // Entry count; full record/digest validation precedes extraction.
  }
}