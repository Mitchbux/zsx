package zsd;

import haxe.io.Bytes;
import haxe.io.BytesOutput;
import zsd.ZsdBinary.ZsdReader;

/**
 * ZSDC wraps the ZsxLatest byte stream with an independent version, size and
 * digest. This makes an offline .zsd file self-identifying before decompression.
 */
class ZsdContainer {
  static inline var VERSION = 1;

  public static function pack(archive:Bytes):Bytes {
    if (archive.length > ZsxLatest.MAX_OUTPUT_SIZE)
      ZsdBinary.fail("archive exceeds ZsxLatest's 256 MiB input bound");
    var compressed = ZsxLatest.encode(archive);
    if (compressed.length > ZsdBinary.MAX_BYTES) ZsdBinary.fail("compressed archive exceeds ZSD bounds");
    var out = new BytesOutput();
    out.writeString("ZSDC");
    ZsdBinary.u16(out, VERSION);
    ZsdBinary.length(out, archive.length);
    ZsdBinary.raw(out, ZsdBinary.digest(archive));
    ZsdBinary.length(out, compressed.length);
    ZsdBinary.raw(out, compressed);
    return out.getBytes();
  }

  public static function unpack(container:Bytes):Bytes {
    var input = new ZsdReader(container);
    if (input.raw(4).toString() != "ZSDC")
      ZsdBinary.fail("not a ZSDC container (legacy downloader entries are not accepted)");
    if (input.u16() != VERSION) ZsdBinary.fail("unsupported ZSD container version");
    var originalLength = input.length();
    var originalDigest = input.raw(32);
    var compressedLength = input.length();
    var compressed = input.raw(compressedLength);
    input.end();
    var archive:Bytes = try ZsxLatest.decode(compressed)
    catch (e:Dynamic) throw "ZSD: ZsxLatest decompression failed: " + Std.string(e);
    if (archive.length != originalLength) ZsdBinary.fail("decompressed length does not match container");
    if (!ZsdBinary.equals(originalDigest, ZsdBinary.digest(archive)))
      ZsdBinary.fail("decompressed digest does not match container");
    return archive;
  }
}