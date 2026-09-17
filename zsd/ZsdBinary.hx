package zsd;

import haxe.crypto.Sha256;
import haxe.io.Bytes;
import haxe.io.BytesOutput;

/** Small, deliberately boring binary primitives used by the ZSD formats. */
class ZsdBinary {
  public static inline var MAX_BYTES:Int = 1073741824; // Haxe Bytes and .NET arrays are Int indexed.

  public static function output():BytesOutput return new BytesOutput();

  public static function u8(out:BytesOutput, n:Int):Void {
    if (n < 0 || n > 255) fail("u8 out of range");
    out.writeByte(n);
  }

  public static function u16(out:BytesOutput, n:Int):Void {
    if (n < 0 || n > 65535) fail("u16 out of range");
    out.writeByte(n >>> 8);
    out.writeByte(n & 255);
  }

  public static function u32(out:BytesOutput, n:Int):Void {
    if (n < 0) fail("negative u32");
    out.writeByte((n >>> 24) & 255);
    out.writeByte((n >>> 16) & 255);
    out.writeByte((n >>> 8) & 255);
    out.writeByte(n & 255);
  }

  /** Writes an unsigned 64-bit quantity restricted to a safe, bounded Int. */
  public static function length(out:BytesOutput, n:Int):Void {
    if (n < 0 || n > MAX_BYTES) fail("length is outside ZSD bounds");
    u32(out, 0);
    u32(out, n);
  }

  public static function raw(out:BytesOutput, b:Bytes):Void out.writeBytes(b, 0, b.length);

  public static function string(out:BytesOutput, value:String):Void {
    var b = Bytes.ofString(value);
    if (b.length > 65535) fail("string is too long");
    u16(out, b.length);
    raw(out, b);
  }

  public static function digest(b:Bytes):Bytes return Sha256.make(b);

  public static function equals(a:Bytes, b:Bytes):Bool {
    if (a.length != b.length) return false;
    for (i in 0...a.length) if (a.get(i) != b.get(i)) return false;
    return true;
  }

  public static function hex(b:Bytes):String {
    var s = new StringBuf();
    for (i in 0...b.length) s.add(StringTools.hex(b.get(i), 2).toLowerCase());
    return s.toString();
  }

  public static function fail(message:String):Dynamic throw "ZSD: " + message;
}

class ZsdReader {
  public var bytes(default, null):Bytes;
  public var pos(default, null):Int;

  public function new(bytes:Bytes) {
    this.bytes = bytes;
    pos = 0;
  }

  public function require(count:Int):Void {
    if (count < 0 || pos > bytes.length - count) ZsdBinary.fail("truncated binary data");
  }

  public function u8():Int {
    require(1);
    return bytes.get(pos++);
  }

  public function u16():Int return (u8() << 8) | u8();

  public function u32():Int {
    var a = u8(), b = u8(), c = u8(), d = u8();
    if (a > 127) ZsdBinary.fail("u32 exceeds supported signed range");
    return (a << 24) | (b << 16) | (c << 8) | d;
  }

  public function length():Int {
    var hi = u32();
    var lo = u32();
    if (hi != 0 || lo > ZsdBinary.MAX_BYTES) ZsdBinary.fail("length exceeds ZSD bounds");
    return lo;
  }

  public function raw(length:Int):Bytes {
    require(length);
    var result = bytes.sub(pos, length);
    pos += length;
    return result;
  }

  public function string():String {
    var n = u16();
    return raw(n).toString();
  }

  public function end():Void {
    if (pos != bytes.length) ZsdBinary.fail("unexpected trailing binary data");
  }
}