import sys.io.File;
import sys.FileSystem;
import zsd.ZsdArchive;
import zsd.ZsdBinary;
import zsd.ZsdContainer;
import zsd.ZsdFs;
import zsd.ZsdInputs;
import zsd.ZsdProtocol;
import zsd.ZsdProtocol.ZsdTransferOptions;

/**
 * ZSD command line uploader/downloader. This file intentionally has no build
 * directives: the C# single-file packaging step owns those separately.
 */
class Zsd {
  static function main():Void {
    try {
      var args = Sys.args();
      if (args.length == 0 || args[0] == "--help" || args[0] == "-h" || args[0] == "help") {
        Sys.println(usage());
        return;
      }
      var command = args.shift();
      switch (command) {
        case "upload": upload(args);
        case "get", "download": get(args);
        case "pack": pack(args);
        case "unpack": unpack(args);
        default: ZsdBinary.fail("unknown command: " + command + "\n" + usage());
      }
    } catch (e:Dynamic) {
      Sys.println("ERROR: " + Std.string(e));
      Sys.println("Use `zsd --help` for usage.");
      Sys.exit(2);
    }
  }

  static function upload(args:Array<String>):Void {
    var parsed = transferOptions(args, false);
    if (parsed.rest.length == 0) ZsdBinary.fail("upload needs at least one file, folder, or wildcard");
    if (ZsdProtocol.isPublicEndpoint(parsed.options.endpoint) && !parsed.publicAllowed)
      ZsdBinary.fail("refusing public is.gd upload without --allow-public-upload (entries are public and non-durable)");
    var archive = ZsdArchive.fromSources(ZsdInputs.collect(parsed.rest));
    var container = ZsdContainer.pack(archive); // Compression happens before any network request.
    Sys.println("Uploading " + container.length + " compressed bytes in <= " + ZsdProtocol.CHUNK_BYTES + "-byte chunks...");
    var id = new ZsdProtocol(parsed.options).upload(container);
    Sys.println("ZSD entry: " + id);
    Sys.println("Retrieve with: zsd get --endpoint " + parsed.options.endpoint + " " + id);
  }

  static function get(args:Array<String>):Void {
    var parsed = transferOptions(args, true);
    if (parsed.rest.length != 1) ZsdBinary.fail("get needs exactly one short id");
    var packed = new ZsdProtocol(parsed.options).download(parsed.rest[0]);
    var entries = ZsdArchive.decode(ZsdContainer.unpack(packed)); // Fully verify before extraction.
    ZsdArchive.extract(entries, parsed.output, parsed.force);
    Sys.println("Extracted " + entries.length + " entries to " + parsed.output);
  }

  static function pack(args:Array<String>):Void {
    var output:Null<String> = null;
    var force = false;
    var inputs = new Array<String>();
    var i = 0;
    while (i < args.length) {
      var arg = args[i++];
      if (arg == "--output" || arg == "-o") {
        if (i >= args.length) ZsdBinary.fail(arg + " needs a filename");
        output = args[i++];
      } else if (arg == "--force") {
        force = true;
      } else if (StringTools.startsWith(arg, "--")) {
        ZsdBinary.fail("unknown pack option: " + arg);
      } else inputs.push(arg);
    }
    if (output == null) ZsdBinary.fail("pack requires --output FILE.zsd");
    if (inputs.length == 0) ZsdBinary.fail("pack needs at least one file, folder, or wildcard");
    var sources = ZsdInputs.collect(inputs);
    var absoluteOutput = ZsdFs.normalize(FileSystem.fullPath(output));
    var outputParent = ZsdFs.parent(output);
    ZsdFs.ensureDir(outputParent);
    if (FileSystem.exists(output)) {
      ZsdFs.assertNotLink(output);
      if (FileSystem.isDirectory(output)) ZsdBinary.fail("pack output is a directory: " + output);
      if (!force) ZsdBinary.fail("refusing to overwrite existing file: " + output + " (use --force)");
    }
    for (source in sources) {
      var absoluteSource = ZsdFs.normalize(FileSystem.fullPath(source.sourcePath));
      if (absoluteSource == absoluteOutput ||
        (source.directory && StringTools.startsWith(absoluteOutput, absoluteSource + "/")))
        ZsdBinary.fail("pack output is included by an input path: " + output);
    }
    var stamp = Std.string(Std.int(Sys.time() * 1000));
    var stage = ZsdFs.join(outputParent, ".zsd-pack-" + stamp);
    var suffix = 0;
    while (FileSystem.exists(stage) || FileSystem.exists(stage + ".previous"))
      stage = ZsdFs.join(outputParent, ".zsd-pack-" + stamp + "-" + (++suffix));
    // Generate and flush a sibling temporary file before altering any output.
    try {
      File.saveBytes(stage, ZsdContainer.pack(ZsdArchive.fromSources(sources)));
    } catch (e:Dynamic) {
      if (FileSystem.exists(stage)) {
        try {
          FileSystem.deleteFile(stage);
        } catch (_:Dynamic) {}
      }
      throw e;
    }
    var previous = stage + ".previous";
    var movedPrevious = false;
    try {
      if (FileSystem.exists(output)) {
        // Move, never delete, the old result until the staged result is safely
        // in place. This is especially important on Windows where rename does
        // not replace an existing file.
        ZsdFs.assertNotLink(output);
        FileSystem.rename(output, previous);
        movedPrevious = true;
      }
      FileSystem.rename(stage, output);
      if (movedPrevious) FileSystem.deleteFile(previous);
    } catch (e:Dynamic) {
      var rollbackError:Null<String> = null;
      try {
        if (FileSystem.exists(output) && !ZsdFs.isLink(output) && !FileSystem.isDirectory(output))
          FileSystem.deleteFile(output);
        if (movedPrevious && FileSystem.exists(previous)) FileSystem.rename(previous, output);
      } catch (rollback:Dynamic) rollbackError = Std.string(rollback);
      if (FileSystem.exists(stage)) {
        try {
          FileSystem.deleteFile(stage);
        } catch (_:Dynamic) {}
      }
      if (rollbackError != null)
        ZsdBinary.fail("pack output replacement failed and rollback failed: " + rollbackError + "; original error: " + Std.string(e));
      ZsdBinary.fail("pack output replacement failed; existing output was restored: " + Std.string(e));
    }
    Sys.println("Wrote " + output);
  }

  static function unpack(args:Array<String>):Void {
    var output = ".";
    var force = false;
    var input:Null<String> = null;
    var i = 0;
    while (i < args.length) {
      var arg = args[i++];
      if (arg == "--output" || arg == "-o") {
        if (i >= args.length) ZsdBinary.fail(arg + " needs a directory");
        output = args[i++];
      } else if (arg == "--force") force = true;
      else if (StringTools.startsWith(arg, "--")) ZsdBinary.fail("unknown unpack option: " + arg);
      else {
        if (input != null) ZsdBinary.fail("unpack needs exactly one archive");
        input = arg;
      }
    }
    if (input == null) ZsdBinary.fail("unpack needs a .zsd archive");
    ZsdArchive.extract(ZsdArchive.decode(ZsdContainer.unpack(File.getBytes(input))), output, force);
    Sys.println("Unpacked " + input + " to " + output);
  }

  static function transferOptions(args:Array<String>, extraction:Bool):{options:ZsdTransferOptions, rest:Array<String>, output:String, force:Bool, publicAllowed:Bool} {
    var endpoint = ZsdProtocol.DEFAULT_ENDPOINT;
    var timeoutMs = 30000;
    var retries = 2;
    var paceMs = 500;
    var output = ".";
    var force = false;
    var publicAllowed = false;
    var rest = new Array<String>();
    var i = 0;
    while (i < args.length) {
      var arg = args[i++];
      switch (arg) {
        case "--endpoint":
          if (i >= args.length) ZsdBinary.fail("--endpoint needs a URL");
          endpoint = args[i++];
        case "--timeout-ms":
          if (i >= args.length) ZsdBinary.fail("--timeout-ms needs an integer");
          timeoutMs = integer(args[i++], "--timeout-ms");
        case "--retries":
          if (i >= args.length) ZsdBinary.fail("--retries needs an integer");
          retries = integer(args[i++], "--retries");
        case "--pace-ms":
          if (i >= args.length) ZsdBinary.fail("--pace-ms needs an integer");
          paceMs = integer(args[i++], "--pace-ms");
        case "--output", "-o":
          if (!extraction) ZsdBinary.fail(arg + " is only valid for get/unpack");
          if (i >= args.length) ZsdBinary.fail(arg + " needs a directory");
          output = args[i++];
        case "--force":
          if (!extraction) ZsdBinary.fail("--force is only valid for get/unpack");
          force = true;
        case "--allow-public-upload":
          if (extraction) ZsdBinary.fail("--allow-public-upload is only valid for upload");
          publicAllowed = true;
        default:
          if (StringTools.startsWith(arg, "--")) ZsdBinary.fail("unknown option: " + arg);
          rest.push(arg);
      }
    }
    return {
      options: {endpoint: endpoint, timeoutMs: timeoutMs, retries: retries, paceMs: paceMs},
      rest: rest, output: output, force: force, publicAllowed: publicAllowed
    };
  }

  static function integer(value:String, label:String):Int {
    var n = Std.parseInt(value);
    if (n == null || Std.string(n) != value) ZsdBinary.fail(label + " needs an integer");
    return n;
  }

  static function usage():String return
    "ZSD - compressed tiny-URL archive transport\n\n" +
    "Usage:\n" +
    "  zsd upload [NETWORK OPTIONS] --allow-public-upload <file|folder|wildcard>...\n" +
    "  zsd get [NETWORK OPTIONS] [--output DIR] [--force] SHORT_ID\n" +
    "  zsd pack --output FILE.zsd [--force] <file|folder|wildcard>...\n" +
    "  zsd unpack [--output DIR] [--force] FILE.zsd\n\n" +
    "NETWORK OPTIONS: --endpoint URL  --timeout-ms N  --retries N  --pace-ms N\n\n" +
    "The default endpoint is is.gd. Its entries are publicly readable, cannot be\n" +
    "deleted or modified, and may disappear without notice. Do not upload private\n" +
    "data there; --allow-public-upload is deliberately required. Use --endpoint\n" +
    "with a compatible local/mock create.php and forward.php service for tests.\n" +
    "Only new ZSD versioned entries are accepted; legacy csdownload entries are\n" +
    "never guessed or silently interpreted.";
}