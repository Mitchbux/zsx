package zsd.test;

import haxe.io.Bytes;
import zsd.ZsdArchive;
import zsd.ZsdArchive.ZsdEntry;
import zsd.ZsdBinary;
import zsd.ZsdBinary.ZsdReader;
import zsd.ZsdContainer;

/**
 * Offline format regression test. It requires no network endpoint and checks
 * the unambiguous binary manifest and digest rejection path.
 */
class ZsdArchiveTest {
  static function main():Void {
    var hello = Bytes.ofString("hello\n");
    var entries:Array<ZsdEntry> = [
      {path: "folder", directory: true, data: Bytes.alloc(0), digest: ZsdBinary.digest(Bytes.alloc(0))},
      {path: "folder/a.txt", directory: false, data: hello, digest: ZsdBinary.digest(hello)},
      {path: "folder/b.txt", directory: false, data: hello, digest: ZsdBinary.digest(hello)},
      {path: "folder/empty", directory: true, data: Bytes.alloc(0), digest: ZsdBinary.digest(Bytes.alloc(0))},
      {path: "folder/zero", directory: false, data: Bytes.alloc(0), digest: ZsdBinary.digest(Bytes.alloc(0))}
    ];
    var archive = ZsdArchive.encode(entries);
    var decoded = ZsdArchive.decode(archive);
    require(decoded.length == 5, "entry count");
    require(decoded[0].directory && decoded[0].path == "folder", "directory record");
    require(decoded[1].data.toString() == "hello\n", "file contents");

    var packed = ZsdContainer.pack(archive);
    require(packed.sub(0, 6).toHex() == "5a5358330001", "exact ZSX3 magic and big-endian version");
    var header = new ZsdReader(packed);
    header.raw(6);
    require(header.length() == archive.length, "u64 original length");
    require(header.raw(32).compare(ZsdBinary.digest(archive)) == 0, "whole-stream SHA-256");
    var size = header.length();
    require(size == packed.length - 54, "u64 compressed length");
    var stream = header.raw(size);
    header.end();
    require(stream.sub(0, 4).toString() == "ZSL1", "fixed portable codec");
    require(stream.compare(ZsxLatest.encode(archive)) == 0, "one compression of complete record stream");
    require(ZsxLatest.decode(stream).compare(archive) == 0, "whole-stream decode before record parsing");
    require(ZsdContainer.unpack(packed).compare(archive) == 0, "ZSX3 round trip");
    require(ZsdArchive.decode(ZsdContainer.unpack(packed))[2].data.compare(hello) == 0,
      "shared data across two file records");

    // Independently reproduce the old writer, without invoking the new pack().
    var legacy = ZsdBinary.output();
    legacy.writeString("ZSDC");
    ZsdBinary.u16(legacy, 1);
    ZsdBinary.length(legacy, archive.length);
    ZsdBinary.raw(legacy, ZsdBinary.digest(archive));
    ZsdBinary.length(legacy, stream.length);
    ZsdBinary.raw(legacy, stream);
    require(ZsdContainer.unpack(legacy.getBytes()).compare(archive) == 0, "legacy ZSDC v1 decode");

    for (length in [0, 3, 5, 13, 45, 53, 54, packed.length - 1])
      fails(function() ZsdContainer.unpack(packed.sub(0, length)), "truncation at " + length);
    for (offset in [0, 5, 6, 13, 14, 46, 53, 54, 58, packed.length - 1]) {
      var damaged = packed.sub(0, packed.length);
      damaged.set(offset, damaged.get(offset) ^ 1);
      fails(function() ZsdContainer.unpack(damaged), "corruption at " + offset);
    }
    var corrupt = archive.sub(0, archive.length);
    corrupt.set(corrupt.length - 1, corrupt.get(corrupt.length - 1) ^ 1);
    fails(function() ZsdArchive.decode(corrupt), "digest mismatch must reject archive");
    Sys.println("ZsdArchiveTest OK: ZSX3 v1 bytes, global ZSL1 stream, shared files/empty entries, legacy ZSDC, bounds/integrity");
  }

  static function fails(action:Void->Void, label:String):Void {
    var rejected = false;
    try action() catch (_:Dynamic) rejected = true;
    require(rejected, label);
  }

  static function require(value:Bool, label:String):Void {
    if (!value) throw "ZsdArchiveTest failed: " + label;
  }
}