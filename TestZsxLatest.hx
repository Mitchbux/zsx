import haxe.io.Bytes;
import sys.io.File;

/**
 * Target-neutral regression and command-line helper for ZsxLatest.
 *
 * The optional chunk commands are consumed by test/zsx_latest_c_cross.sh so
 * the attached C source can be compared without a JS-only test dependency.
 */
class TestZsxLatest {
  static function main() {
    var args = Sys.args();
    if (args.length == 3 && args[0] == "--chunk-encode") {
      File.saveBytes(args[2], ZsxLatest.encodeChunk(File.getBytes(args[1])));
      return;
    }
    if (args.length == 3 && args[0] == "--chunk-decode") {
      File.saveBytes(args[2], ZsxLatest.decodeChunk(File.getBytes(args[1])));
      return;
    }
    run();
    Sys.println("ZsxLatest tests passed");
  }

  static function run():Void {
    var cases = new Array<Bytes>();
    cases.push(Bytes.alloc(0));
    cases.push(bytes([0]));
    cases.push(bytes([0, 255, 1, 254, 2]));
    cases.push(Bytes.ofString("odd length latest ZSX test."));
    var repeated = Bytes.alloc(4097);
    for (i in 0...repeated.length) repeated.set(i, "abracadabra".charCodeAt(i % 11));
    cases.push(repeated);
    var random = Bytes.alloc(4099);
    var state = 0x13579b;
    for (i in 0...random.length) {
      state = (state * 1103515245 + 12345) & 0x7fffffff;
      random.set(i, state >>> 16);
    }
    for (source in cases) {
      var archive = ZsxLatest.encode(source);
      assertEqual(source, ZsxLatest.decode(archive), "portable round trip");
      if (source.length != 0) {
        var chunk = ZsxLatest.encodeChunk(source);
        assertEqual(source, ZsxLatest.decodeChunk(chunk), "chunk round trip");
        assertEqual(source, ZsxLatest.decode(nativeFrame(chunk, 4)), "32-bit native frame");
        assertEqual(source, ZsxLatest.decode(nativeFrame(chunk, 8)), "64-bit native frame");
      }
    }
    var valid = ZsxLatest.encode(repeated);
    expectFailure(valid.sub(0, valid.length - 1), "truncated archive");
    var corrupt = valid.sub(0, valid.length);
    corrupt.set(corrupt.length - 1, corrupt.get(corrupt.length - 1) ^ 0x80);
    expectFailure(corrupt, "corrupt archive");
    var zsx1 = Bytes.ofString("ZSX1");
    expectFailure(zsx1, "ZSX1 must remain explicit");
  }

  static function nativeFrame(chunk:Bytes, width:Int):Bytes {
    var out = Bytes.alloc(width + chunk.length);
    var size = chunk.length;
    out.set(0, size);
    out.set(1, size >>> 8);
    out.set(2, size >>> 16);
    out.set(3, size >>> 24);
    // All test chunks are in the portable subset (< 2^32).
    for (i in 4...width) out.set(i, 0);
    out.blit(width, chunk, 0, chunk.length);
    return out;
  }

  static function bytes(values:Array<Int>):Bytes {
    var out = Bytes.alloc(values.length);
    for (i in 0...values.length) out.set(i, values[i]);
    return out;
  }

  static function assertEqual(expected:Bytes, actual:Bytes, message:String):Void {
    if (expected.length != actual.length) throw message + ": length mismatch";
    for (i in 0...expected.length)
      if (expected.get(i) != actual.get(i))
        throw message + ": mismatch at byte " + i;
  }

  static function expectFailure(data:Bytes, message:String):Void {
    var failed = false;
    try ZsxLatest.decode(data) catch (_:Dynamic) failed = true;
    if (!failed) throw message + ": accepted malformed data";
  }
}