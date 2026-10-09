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
    // Invoking upload is consent; keep the warning without an interactive gate.
    Sys.println("Warning: uploads use public, non-durable URL storage. Do not upload secrets.");
    var protocol = new ZsdProtocol(parsed.options);
    var archive = ZsdArchive.fromSources(ZsdInputs.collect(parsed.rest));
    var container = ZsdContainer.pack(archive); // Compression happens before any network request.
    var chunkLimit = parsed.options.provider == "shortio" ? zsd.ZsdShortIo.CHUNK_BYTES : ZsdProtocol.CHUNK_BYTES;
    Sys.println("Uploading ZSX3 v1: " + container.length + " compressed bytes in <= " + chunkLimit + "-byte chunks...");
    var id = protocol.upload(container);
    var envDomain = Sys.getEnv("SHORT_IO_DOMAIN");
    var defaultPrefix = "zsd-sio-v1:" + ZsdProtocol.DEFAULT_SHORT_IO_DOMAIN + ":";
    if (StringTools.startsWith(id, defaultPrefix) &&
        (envDomain == null || envDomain == "" || envDomain.toLowerCase() == ZsdProtocol.DEFAULT_SHORT_IO_DOMAIN))
      id = id.substr(defaultPrefix.length);
    Sys.println("ZSD entry: " + id);
    Sys.println("Retrieve with: zsd get" + (parsed.options.provider == "legacy" ? " --provider legacy" : "") +
      (parsed.options.endpoint == ZsdProtocol.DEFAULT_ENDPOINT ?
      "" : " --endpoint " + parsed.options.endpoint) + " " + id);
  }

  static function get(args:Array<String>):Void {
    var parsed = transferOptions(args, true);
    if (parsed.rest.length != 1) ZsdBinary.fail("get needs exactly one short id");
    if (StringTools.startsWith(parsed.rest[0], "zsd-sio-v1:")) {
      var fields = parsed.rest[0].split(":");
      if (fields.length != 3) ZsdBinary.fail("invalid Short.io ZSD reference");
      if (parsed.domainSpecified && parsed.options.domain.toLowerCase() != fields[1].toLowerCase())
        ZsdBinary.fail("Short.io reference domain conflicts with explicit --domain");
      // A typed reference is self-contained: neither env nor built-in defaults
      // may override its domain. Resolve before constructing the API client.
      parsed.options.provider = "shortio";
      parsed.options.domain = fields[1];
    }
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
    Sys.println("Wrote ZSX3 v1 archive: " + output);
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

  static function transferOptions(args:Array<String>, extraction:Bool):{options:ZsdTransferOptions, rest:Array<String>, output:String, force:Bool, domainSpecified:Bool} {
    var endpoint = ZsdProtocol.DEFAULT_ENDPOINT;
    var provider = "shortio";
    var domain = Sys.getEnv("SHORT_IO_DOMAIN");
    if (domain == null || domain == "") domain = ZsdProtocol.DEFAULT_SHORT_IO_DOMAIN;
    var domainSpecified = false;
    var timeoutMs = 30000;
    var retries = 2;
    var paceMs = 500;
    var paceSpecified = false;
    var output = ".";
    var force = false;
    var rest = new Array<String>();
    var i = 0;
    while (i < args.length) {
      var arg = args[i++];
      switch (arg) {
        case "--provider":
          if (i >= args.length) ZsdBinary.fail("--provider needs legacy or shortio");
          provider = args[i++];
        case "--domain":
          if (i >= args.length) ZsdBinary.fail("--domain needs a hostname");
          domain = args[i++];
          domainSpecified = true;
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
          paceSpecified = true;
        case "--output", "-o":
          if (!extraction) ZsdBinary.fail(arg + " is only valid for get/unpack");
          if (i >= args.length) ZsdBinary.fail(arg + " needs a directory");
          output = args[i++];
        case "--force":
          if (!extraction) ZsdBinary.fail("--force is only valid for get/unpack");
          force = true;
        case "--allow-public-upload":
          if (extraction) ZsdBinary.fail("--allow-public-upload is only valid for upload");
          // Accepted for old scripts; upload itself now provides consent.
        default:
          if (StringTools.startsWith(arg, "--")) ZsdBinary.fail("unknown option: " + arg);
          rest.push(arg);
      }
    }
    if (provider != "shortio" && provider != "legacy") ZsdBinary.fail("provider must be legacy or shortio");
    if (!paceSpecified && (provider == "shortio" ||
        (rest.length == 1 && StringTools.startsWith(rest[0], "zsd-sio-v1:")))) paceMs = 100;
    return {
      options: {endpoint: endpoint, timeoutMs: timeoutMs, retries: retries, paceMs: paceMs, provider: provider, domain: domain},
      rest: rest, output: output, force: force, domainSpecified: domainSpecified
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
    "  zsd upload [NETWORK OPTIONS] <file|folder|wildcard>...\n" +
    "  zsd get [NETWORK OPTIONS] [--output DIR] [--force] SHORT_ID\n" +
    "  zsd pack --output FILE.zsd [--force] <file|folder|wildcard>...\n" +
    "  zsd unpack [--output DIR] [--force] FILE.zsd\n\n" +
    "pack/upload write ZSX3 v1: all files in one tar-like ZSDP v1 stream,\n" +
    "then one ZSL1 compression pass (not POSIX tar or native ZSX2).\n" +
    "get/unpack verify ZSX3 v1 or legacy ZSDC v1 fully before extraction.\n\n" +
    "NETWORK OPTIONS: --provider legacy|shortio  --domain HOST\n" +
    "                 --endpoint URL  --timeout-ms N  --retries N  --pace-ms N\n\n" +
    "Defaults: Short.io on pex.s.gy; get extracts into the CURRENT directory.\n" +
    "Examples: zsd upload file1 folder2    zsd get et56g4\n" +
    #if (cs && zsd_embedded_key)
    "PRIVATE BUILD: upload/get use an embedded credential, not the environment.\n" +
    "Anyone receiving this source or executable can extract and use that key.\n" +
    #else
    "Set SHORT_IO_API_KEY securely in the runtime environment for upload/get.\n" +
    #end
    "Domain priority for bare IDs/uploads: --domain > SHORT_IO_DOMAIN > pex.s.gy.\n" +
    "Full zsd-sio-v1:HOST:PATH references use HOST, ignoring env/default domain;\n" +
    "a conflicting explicit --domain fails. Custom-domain uploads print full refs.\n" +
    "Bare legacy IDs need --provider legacy (is.gd). Old full refs still work.\n" +
    "Existing unrelated files are retained; overwrites require --force.\n" +
    "Carrier URLs: https://pex.wf/# + standard Base64 (including +, /, and =).\n" +
    "The carrier host is NOT the configured Short.io shortening domain.\n" +
    "Batches contain up to 1000 chunk URLs; bulk calls have a 3000ms gate,\n" +
    "lookups a separate 100ms gate. Retry-After delays both endpoints.\n" +
    "--timeout-ms bounds the total Short.io request, including slow body reads.\n" +
    "URLs are limited to 2048 bytes by this client; chunks are <=1450 bytes.\n" +
    "Failed uploads print no root; rerunning identical input reuses stable paths.\n" +
    #if (cs && zsd_embedded_key)
    "Short.io get uses the compiled credential; domain is in the reference.\n\n" +
    #else
    "Short.io get needs the same runtime credential; domain is in the reference.\n\n" +
    #end
    "Uploads are public and non-durable; never upload secrets or your only backup.\n" +
    "Invoking upload is consent; no prompt or --allow-public-upload is required\n" +
    "(the old flag is accepted for compatibility). Short.io uses api.short.io;\n" +
    "--endpoint can select an HTTP loopback mock for tests.\n" +
    "Only new ZSD versioned entries are accepted; legacy csdownload entries are\n" +
    "never guessed or silently interpreted.";
}