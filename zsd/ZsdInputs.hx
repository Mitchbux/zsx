package zsd;

import sys.FileSystem;
import zsd.ZsdArchive.ZsdSource;

/** Deterministic file, directory and simple (*, ?) wildcard expansion. */
class ZsdInputs {
  public static function collect(specs:Array<String>):Array<ZsdSource> {
    var result = new Array<ZsdSource>();
    var used = new Map<String, String>();
    for (spec in specs) {
      if (spec == null || spec == "") ZsdBinary.fail("empty input path");
      if (hasWildcards(spec)) collectWildcard(spec, result, used);
      else {
        if (!FileSystem.exists(spec)) ZsdBinary.fail("input does not exist: " + spec);
        ZsdFs.assertNotLink(spec);
        addTree(spec, ZsdFs.basename(spec), result, used);
      }
    }
    if (result.length == 0) ZsdBinary.fail("no files or folders matched");
    return result;
  }

  static function collectWildcard(spec:String, result:Array<ZsdSource>, used:Map<String, String>):Void {
    var normalized = ZsdFs.normalize(spec);
    var star = firstWildcard(normalized);
    var slash = normalized.substr(0, star).lastIndexOf("/");
    var base = slash < 0 ? "." : normalized.substr(0, slash);
    var pattern = slash < 0 ? normalized : normalized.substr(slash + 1);
    if (base == "") base = "/";
    if (!FileSystem.exists(base) || !FileSystem.isDirectory(base))
      ZsdBinary.fail("wildcard base directory does not exist: " + base);
    ZsdFs.assertNotLink(base);
    var root = base == "." || base == "/" ? "" : ZsdFs.basename(base);
    var candidates = new Array<String>();
    walk(base, base, function(path:String, relative:String) {
      if (glob(pattern, relative)) candidates.push(path);
    });
    candidates.sort(function(a, b) return a < b ? -1 : (a > b ? 1 : 0));
    for (path in candidates) {
      var relative = relativeTo(base, path);
      var archivePath = root == "" ? relative : ZsdFs.join(root, relative);
      addWildcardParents(base, path, root, result, used);
      addTree(path, archivePath, result, used);
    }
  }

  static function walk(root:String, current:String, visit:String->String->Void):Void {
    for (name in ZsdFs.sortedNames(current)) {
      var child = ZsdFs.join(current, name);
      ZsdFs.assertNotLink(child);
      var relative = relativeTo(root, child);
      visit(child, relative);
      if (FileSystem.isDirectory(child)) walk(root, child, visit);
    }
  }

  static function addTree(path:String, archivePath:String, result:Array<ZsdSource>, used:Map<String, String>):Void {
    ZsdFs.assertNotLink(path);
    var directory = addOne(path, archivePath, result, used);
    if (directory) {
      for (name in ZsdFs.sortedNames(path))
        addTree(ZsdFs.join(path, name), ZsdFs.join(archivePath, name), result, used);
    }
  }

  static function addWildcardParents(base:String, path:String, root:String, result:Array<ZsdSource>, used:Map<String, String>):Void {
    if (root == "") return;
    addOne(base, root, result, used);
    var relative = relativeTo(base, path).split("/");
    var actual = base;
    var archived = root;
    for (i in 0...(relative.length - 1)) {
      actual = ZsdFs.join(actual, relative[i]);
      archived = ZsdFs.join(archived, relative[i]);
      addOne(actual, archived, result, used);
    }
  }

  /** Adds just one entry; directory recursion is intentionally handled by addTree. */
  static function addOne(path:String, archivePath:String, result:Array<ZsdSource>, used:Map<String, String>):Bool {
    ZsdFs.assertNotLink(path);
    ZsdFs.validateArchivePath(archivePath);
    // Windows filesystems are normally case-insensitive; make archive names
    // portable even when packing on a case-sensitive host.
    var key = archivePath.toLowerCase();
    var previous = used.get(key);
    if (previous != null) {
      if (previous != path) ZsdBinary.fail("input paths collide after archiving: " + archivePath);
      return FileSystem.isDirectory(path);
    }
    var directory = FileSystem.isDirectory(path);
    used.set(key, path);
    result.push({sourcePath: path, archivePath: archivePath, directory: directory});
    return directory;
  }

  static function relativeTo(base:String, path:String):String {
    var b = ZsdFs.normalize(FileSystem.fullPath(base));
    var p = ZsdFs.normalize(FileSystem.fullPath(path));
    if (!StringTools.endsWith(b, "/")) b += "/";
    if (p.substr(0, b.length) != b) ZsdBinary.fail("path escaped wildcard root");
    return p.substr(b.length);
  }

  static inline function hasWildcards(s:String):Bool return s.indexOf("*") >= 0 || s.indexOf("?") >= 0;

  static function firstWildcard(s:String):Int {
    var a = s.indexOf("*"), b = s.indexOf("?");
    if (a < 0) return b;
    if (b < 0) return a;
    return a < b ? a : b;
  }

  // `*` and `?` match a filename component, never a path separator.
  static function glob(pattern:String, value:String):Bool {
    var p = 0, v = 0, star = -1, restore = 0;
    while (v < value.length) {
      if (p < pattern.length && (pattern.charAt(p) == value.charAt(v) && pattern.charAt(p) != "*" && pattern.charAt(p) != "?")) {
        p++; v++;
      } else if (p < pattern.length && pattern.charAt(p) == "?" && value.charAt(v) != "/") {
        p++; v++;
      } else if (p < pattern.length && pattern.charAt(p) == "*") {
        star = p++; restore = v;
      } else if (star >= 0 && value.charAt(restore) != "/") {
        p = star + 1; v = ++restore;
      } else return false;
    }
    while (p < pattern.length && pattern.charAt(p) == "*") p++;
    return p == pattern.length;
  }
}