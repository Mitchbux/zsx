package zsd.test;

import haxe.io.Bytes;
import zsd.ZsdArchive;
import zsd.ZsdArchive.ZsdEntry;
import zsd.ZsdBinary;

/**
 * Offline format regression test. It requires no network endpoint and checks
 * the unambiguous binary manifest and digest rejection path.
 */
class ZsdArchiveTest {
  static function main():Void {
    var hello = Bytes.ofString("hello\n");
    var entries:Array<ZsdEntry> = [
      {path: "folder", directory: true, data: Bytes.alloc(0), digest: ZsdBinary.digest(Bytes.alloc(0))},
      {path: "folder/file.txt", directory: false, data: hello, digest: ZsdBinary.digest(hello)}
    ];
    var archive = ZsdArchive.encode(entries);
    var decoded = ZsdArchive.decode(archive);
    require(decoded.length == 2, "entry count");
    require(decoded[0].directory && decoded[0].path == "folder", "directory record");
    require(decoded[1].data.toString() == "hello\n", "file contents");

    var corrupt = archive.sub(0, archive.length);
    corrupt.set(corrupt.length - 1, corrupt.get(corrupt.length - 1) ^ 1);
    var rejected = false;
    try ZsdArchive.decode(corrupt) catch (_:Dynamic) rejected = true;
    require(rejected, "digest mismatch must reject archive");
    Sys.println("ZsdArchiveTest OK");
  }

  static function require(value:Bool, label:String):Void {
    if (!value) throw "ZsdArchiveTest failed: " + label;
  }
}