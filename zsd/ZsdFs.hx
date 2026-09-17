package zsd;

import sys.FileSystem;

/**
 * Filesystem policy shared by packing and extraction. Reparse points are
 * rejected rather than resolved, so an archive operation cannot escape its
 * requested root through a symlink/junction. This is a best-effort check for
 * a trusted local tree; it cannot make filesystem operations race-free when
 * another process can replace entries concurrently.
 */
#if !cs
#error "ZSD filesystem safety requires the C# target"
#end

#if cs
import cs.system.io.File;
import cs.system.io.FileAttributes;
#end

class ZsdFs {
  public static function isLink(path:String):Bool {
    var attributes = File.GetAttributes(path);
    return new cs.Flags(attributes).has(FileAttributes.ReparsePoint);
  }

  public static function assertNotLink(path:String):Void {
    var current = normalize(FileSystem.fullPath(path));
    while (true) {
      if (FileSystem.exists(current) && isLink(current))
        ZsdBinary.fail("symbolic link or reparse point is not allowed: " + current);
      var next = parent(current);
      if (next == "" || next == current) return;
      current = next;
    }
  }

  public static function normalize(path:String):String {
    return path.split("\\").join("/");
  }

  public static function basename(path:String):String {
    var n = normalize(path);
    while (n.length > 1 && StringTools.endsWith(n, "/")) n = n.substr(0, n.length - 1);
    var p = n.lastIndexOf("/");
    return p < 0 ? n : n.substr(p + 1);
  }

  public static function parent(path:String):String {
    var n = normalize(path);
    while (n.length > 1 && StringTools.endsWith(n, "/")
      && !(n.length == 3 && n.charAt(1) == ":")) n = n.substr(0, n.length - 1);
    var p = n.lastIndexOf("/");
    if (p < 0) return "";
    if (p == 0) return "/";
    // Keep a drive root intact: the parent of C:/child is C:/, not C:.
    if (p == 2 && n.charAt(1) == ":") return n.substr(0, 3);
    return n.substr(0, p);
  }

  public static function join(a:String, b:String):String {
    if (a == "" || a == ".") return b;
    if (b == "") return a;
    return a + "/" + b;
  }

  public static function ensureDir(path:String):Void {
    if (path == "" || path == ".") return;
    // This also inspects every existing ancestor of a missing path.
    assertNotLink(path);
    if (FileSystem.exists(path)) {
      if (!FileSystem.isDirectory(path)) ZsdBinary.fail("expected directory: " + path);
      return;
    }
    var p = parent(path);
    if (p != path) ensureDir(p);
    FileSystem.createDirectory(path);
    // Detect a replacement that happened before the directory became usable.
    assertNotLink(path);
  }

  /**
   * Archive components must be legal, unambiguous Windows filenames. In
   * particular, colon is rejected rather than allowing an NTFS alternate data
   * stream.
   */
  public static function validateArchivePath(path:String):Array<String> {
    if (path == "" || path.charAt(0) == "/" || path.indexOf("\\") >= 0) ZsdBinary.fail("unsafe archive path");
    var parts = path.split("/");
    for (part in parts) {
      if (part == "" || part == "." || part == ".." || part.length > 128)
        ZsdBinary.fail("unsafe archive path component");
      if (StringTools.endsWith(part, ".") || StringTools.endsWith(part, " "))
        ZsdBinary.fail("Windows archive path component has trailing dot or space: " + part);
      if (~/[<>:"|?*\x00-\x1f]/.match(part))
        ZsdBinary.fail("invalid Windows archive path component: " + part);
      // Windows treats these devices as devices even when followed by an
      // extension (for example, CON.txt and NUL.txt).
      var dot = part.indexOf(".");
      var device = (dot < 0 ? part : part.substr(0, dot)).toUpperCase();
      if (device == "CON" || device == "PRN" || device == "AUX" || device == "NUL"
        || ~/^COM[1-9]$/.match(device) || ~/^LPT[1-9]$/.match(device))
        ZsdBinary.fail("reserved Windows archive path component: " + part);
    }
    return parts;
  }

  public static function safeJoin(root:String, archivePath:String):String {
    var parts = validateArchivePath(archivePath);
    var canonicalRoot = normalize(FileSystem.fullPath(root));
    assertNotLink(canonicalRoot);
    var current = canonicalRoot;
    for (part in parts) current = join(current, part);
    var destination = normalize(FileSystem.fullPath(current));
    var prefix = StringTools.endsWith(canonicalRoot, "/") ? canonicalRoot : canonicalRoot + "/";
    if (!StringTools.startsWith(destination, prefix))
      ZsdBinary.fail("archive path escaped destination root: " + archivePath);
    return destination;
  }

  public static function sortedNames(path:String):Array<String> {
    var result = FileSystem.readDirectory(path);
    result.sort(function(a:String, b:String):Int return a < b ? -1 : (a > b ? 1 : 0));
    return result;
  }
}
