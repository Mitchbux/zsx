package zsd;

import haxe.io.Bytes;
import haxe.io.BytesOutput;
import sys.FileSystem;
import sys.io.File;
import zsd.ZsdBinary.ZsdReader;

typedef ZsdEntry = {
  var path:String;
  var directory:Bool;
  var data:Bytes;
  var digest:Bytes;
}

typedef ZsdSource = {
  var sourcePath:String;
  var archivePath:String;
  var directory:Bool;
}

/**
 * ZSDP is a versioned, length-prefixed manifest followed by file bytes.
 * It never serializes paths using delimiter-separated text.
 */
class ZsdArchive {
  static inline var VERSION = 1;
  static inline var MAX_ENTRIES = 100000;

  public static function fromSources(sources:Array<ZsdSource>):Bytes {
    if (sources.length == 0) ZsdBinary.fail("no files or folders matched");
    sources.sort(function(a, b) return a.archivePath < b.archivePath ? -1 : (a.archivePath > b.archivePath ? 1 : 0));
    var entries = new Array<ZsdEntry>();
    for (source in sources) {
      // Collection and reading are separate operations; reject a source that
      // became a reparse point or changed kind in between.
      ZsdFs.assertNotLink(source.sourcePath);
      if (FileSystem.isDirectory(source.sourcePath) != source.directory)
        ZsdBinary.fail("input changed type while packing: " + source.sourcePath);
      var data = source.directory ? Bytes.alloc(0) : File.getBytes(source.sourcePath);
      if (data.length > ZsdBinary.MAX_BYTES) ZsdBinary.fail("file exceeds ZSD bounds: " + source.sourcePath);
      entries.push({
        path: source.archivePath,
        directory: source.directory,
        data: data,
        digest: ZsdBinary.digest(data)
      });
    }
    return encode(entries);
  }

  public static function encode(entries:Array<ZsdEntry>):Bytes {
    if (entries.length > MAX_ENTRIES) ZsdBinary.fail("too many archive entries");
    var previous = "";
    var portableSeen = new Map<String, Bool>();
    var out = new BytesOutput();
    out.writeString("ZSDP");
    ZsdBinary.u16(out, VERSION);
    ZsdBinary.u32(out, entries.length);
    for (entry in entries) {
      ZsdFs.validateArchivePath(entry.path);
      if (previous != "" && entry.path <= previous) ZsdBinary.fail("archive paths are not strictly ordered");
      var portablePath = entry.path.toLowerCase();
      if (portableSeen.exists(portablePath)) ZsdBinary.fail("archive paths collide on portable filesystems");
      portableSeen.set(portablePath, true);
      previous = entry.path;
      if (entry.directory && entry.data.length != 0) ZsdBinary.fail("directory has data");
      if (entry.digest.length != 32) ZsdBinary.fail("invalid digest length");
      ZsdBinary.u8(out, entry.directory ? 1 : 0);
      ZsdBinary.string(out, entry.path);
      ZsdBinary.length(out, entry.data.length);
      ZsdBinary.raw(out, entry.digest);
      ZsdBinary.raw(out, entry.data);
    }
    return out.getBytes();
  }

  public static function decode(bytes:Bytes):Array<ZsdEntry> {
    if (bytes.length > ZsdBinary.MAX_BYTES) ZsdBinary.fail("archive exceeds ZSD bounds");
    var input = new ZsdReader(bytes);
    if (input.raw(4).toString() != "ZSDP") ZsdBinary.fail("not a ZSD archive (legacy formats are not accepted)");
    if (input.u16() != VERSION) ZsdBinary.fail("unsupported ZSD archive version");
    var count = input.u32();
    if (count > MAX_ENTRIES) ZsdBinary.fail("too many archive entries");
    var entries = new Array<ZsdEntry>();
    var seen = new Map<String, Bool>();
    var portableSeen = new Map<String, Bool>();
    var previous = "";
    for (_ in 0...count) {
      var kind = input.u8();
      if (kind != 0 && kind != 1) ZsdBinary.fail("unknown archive entry type");
      var path = input.string();
      ZsdFs.validateArchivePath(path);
      if (previous != "" && path <= previous) ZsdBinary.fail("archive paths are not strictly ordered");
      previous = path;
      if (seen.exists(path)) ZsdBinary.fail("duplicate archive path");
      seen.set(path, true);
      var portablePath = path.toLowerCase();
      if (portableSeen.exists(portablePath)) ZsdBinary.fail("archive paths collide on portable filesystems");
      portableSeen.set(portablePath, true);
      var length = input.length();
      var digest = input.raw(32);
      if (kind == 1 && length != 0) ZsdBinary.fail("directory entry has data");
      var data = input.raw(length);
      if (!ZsdBinary.equals(digest, ZsdBinary.digest(data))) ZsdBinary.fail("digest mismatch for " + path);
      entries.push({path: path, directory: kind == 1, data: data, digest: digest});
    }
    input.end();
    var types = new Map<String, Bool>();
    for (entry in entries) {
      var parts = entry.path.split("/");
      var parent = "";
      for (i in 0...(parts.length - 1)) {
        parent = parent == "" ? parts[i] : parent + "/" + parts[i];
        if (types.exists(parent) && !types.get(parent))
          ZsdBinary.fail("file conflicts with child path: " + parent);
      }
      types.set(entry.path, entry.directory);
    }
    return entries;
  }

  /**
   * Extract only after the entire archive has parsed and every digest has
   * verified. Files are first written under a sibling staging directory.
   */
  public static function extract(entries:Array<ZsdEntry>, output:String, force:Bool):Void {
    ZsdFs.ensureDir(output);
    ZsdFs.assertNotLink(output);
    var seen = new Map<String, Bool>();
    var previousPath = "";
    for (entry in entries) {
      ZsdFs.validateArchivePath(entry.path);
      if (previousPath != "" && entry.path <= previousPath)
        ZsdBinary.fail("extraction entries are not strictly ordered");
      previousPath = entry.path;
      var portablePath = entry.path.toLowerCase();
      if (seen.exists(portablePath)) ZsdBinary.fail("extraction paths collide on portable filesystems");
      seen.set(portablePath, true);
      var destination = ZsdFs.safeJoin(output, entry.path);
      if (FileSystem.exists(destination) && !entry.directory && !force)
        ZsdBinary.fail("refusing to overwrite existing file: " + destination + " (use --force)");
      if (FileSystem.exists(destination) && ZsdFs.isLink(destination))
        ZsdBinary.fail("refusing to write through reparse point: " + destination);
    }
    var stamp = Std.string(Std.int(Sys.time() * 1000));
    var stage = ZsdFs.join(output, ".zsd-stage-" + stamp);
    var suffix = 0;
    while (FileSystem.exists(stage) || FileSystem.exists(stage + "-rollback"))
      stage = ZsdFs.join(output, ".zsd-stage-" + stamp + "-" + (++suffix));
    var rollbackRoot = stage + "-rollback";
    ZsdFs.ensureDir(stage);
    var backups = new Array<{target:String, backup:String}>();
    var committed = new Array<String>();
    var createdDirectories = new Array<String>();
    try {
      // Stage every byte first. A malformed archive therefore cannot affect
      // the destination even if the malformed data was supplied directly.
      for (entry in entries) {
        ZsdFs.validateArchivePath(entry.path);
        if (!ZsdBinary.equals(entry.digest, ZsdBinary.digest(entry.data)))
          ZsdBinary.fail("entry changed or has an invalid digest: " + entry.path);
        var target = ZsdFs.safeJoin(stage, entry.path);
        if (entry.directory) ZsdFs.ensureDir(target);
        else {
          ZsdFs.ensureDir(ZsdFs.parent(target));
          File.saveBytes(target, entry.data);
        }
      }

      // Recheck each component immediately before commit. This is a safety
      // check, not a claim to make filesystem races impossible.
      for (entry in entries) if (entry.directory)
        ensureDestinationDirectory(output, entry.path, createdDirectories);
      for (entry in entries) if (!entry.directory) {
        ensureDestinationParent(output, entry.path, createdDirectories);
        var target = ZsdFs.safeJoin(output, entry.path);
        assertCommitPath(output, entry.path);
        if (FileSystem.exists(target)) {
          if (!force) ZsdBinary.fail("destination changed during extraction: " + target);
          if (ZsdFs.isLink(target) || FileSystem.isDirectory(target))
            ZsdBinary.fail("cannot replace unsafe or directory destination: " + target);
          var backup = ZsdFs.safeJoin(rollbackRoot, entry.path);
          ZsdFs.ensureDir(ZsdFs.parent(backup));
          FileSystem.rename(target, backup);
          backups.push({target: target, backup: backup});
        }
        FileSystem.rename(ZsdFs.safeJoin(stage, entry.path), target);
        committed.push(target);
      }
      // Empty directories have already been committed. Remove staging and old
      // backups only after every target move succeeded.
      removeEmpty(stage);
      removeEmpty(rollbackRoot);
    } catch (e:Dynamic) {
      var rollbackError:Null<String> = null;
      try {
        for (i in 0...committed.length) {
          var target = committed[committed.length - 1 - i];
          if (FileSystem.exists(target) && !FileSystem.isDirectory(target) && !ZsdFs.isLink(target))
            FileSystem.deleteFile(target);
        }
        for (i in 0...backups.length) {
          var item = backups[backups.length - 1 - i];
          if (FileSystem.exists(item.backup)) FileSystem.rename(item.backup, item.target);
        }
        for (i in 0...createdDirectories.length) {
          var directory = createdDirectories[createdDirectories.length - 1 - i];
          if (FileSystem.exists(directory) && FileSystem.isDirectory(directory) && ZsdFs.sortedNames(directory).length == 0)
            FileSystem.deleteDirectory(directory);
        }
      } catch (rollback:Dynamic) {
        rollbackError = Std.string(rollback);
      }
      // Remaining stage/rollback material is only under our generated names.
      try {
        removeEmpty(stage);
      } catch (_:Dynamic) {}
      try {
        removeEmpty(rollbackRoot);
      } catch (_:Dynamic) {}
      if (rollbackError != null)
        ZsdBinary.fail("extraction failed and rollback also failed: " + rollbackError + "; original error: " + Std.string(e));
      ZsdBinary.fail("extraction failed; staged changes were rolled back: " + Std.string(e));
    }
  }

  static function assertCommitPath(output:String, archivePath:String):Void {
    ZsdFs.assertNotLink(output);
    var current = output;
    var parts = ZsdFs.validateArchivePath(archivePath);
    for (i in 0...(parts.length - 1)) {
      current = ZsdFs.join(current, parts[i]);
      if (!FileSystem.exists(current) || !FileSystem.isDirectory(current) || ZsdFs.isLink(current))
        ZsdBinary.fail("destination parent changed during extraction: " + current);
    }
  }

  static function ensureDestinationParent(output:String, archivePath:String, created:Array<String>):Void {
    var parts = ZsdFs.validateArchivePath(archivePath);
    var current = output;
    for (i in 0...(parts.length - 1)) {
      current = ZsdFs.join(current, parts[i]);
      if (FileSystem.exists(current)) {
        if (!FileSystem.isDirectory(current) || ZsdFs.isLink(current))
          ZsdBinary.fail("unsafe destination parent: " + current);
      } else {
        FileSystem.createDirectory(current);
        created.push(current);
      }
    }
  }

  static function ensureDestinationDirectory(output:String, archivePath:String, created:Array<String>):Void {
    ensureDestinationParent(output, archivePath, created);
    var directory = ZsdFs.safeJoin(output, archivePath);
    if (FileSystem.exists(directory)) {
      if (!FileSystem.isDirectory(directory) || ZsdFs.isLink(directory))
        ZsdBinary.fail("cannot create destination directory: " + directory);
    } else {
      FileSystem.createDirectory(directory);
      created.push(directory);
    }
    assertCommitPath(output, archivePath);
  }

  static function removeEmpty(path:String):Void {
    if (!FileSystem.exists(path)) return;
    for (name in ZsdFs.sortedNames(path)) {
      var child = ZsdFs.join(path, name);
      if (FileSystem.isDirectory(child)) removeEmpty(child);
      else FileSystem.deleteFile(child);
    }
    FileSystem.deleteDirectory(path);
  }
}