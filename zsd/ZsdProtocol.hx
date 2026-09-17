package zsd;

import haxe.crypto.Base64;
import haxe.ds.StringMap;
import haxe.io.Bytes;
import haxe.io.BytesOutput;
import zsd.ZsdBinary.ZsdReader;

typedef ZsdTransferOptions = {
  var endpoint:String;
  var timeoutMs:Int;
  var retries:Int;
  var paceMs:Int;
}

private typedef ZsdTreeItem = {
  var id:String;
  var length:Int;
  var digest:Bytes;
}

private typedef ZsdTreeNode = {
  var children:Bool;
  var length:Int;
  var digest:Bytes;
  var items:Array<ZsdTreeItem>;
}

private typedef ZsdDownloadedNode = {
  var data:Bytes;
  var digest:Bytes;
}

private typedef ZsdEndpointParts = {
  var scheme:String;
  var host:String;
  var port:Null<Int>;
}

/**
 * is.gd simple-API transport. Data is only embedded in a harmless encoded
 * dummy URL; response bodies are parsed as text and are never navigated to.
 */
class ZsdProtocol {
  public static inline var DEFAULT_ENDPOINT = "https://is.gd/";
  public static inline var CHUNK_BYTES = 1900;
  /** Protocol transfer limit; this is intentionally lower than ZSD's file-format limit. */
  public static inline var MAX_TRANSFER_BYTES = 134217728; // 128 MiB
  static inline var GROUP_ITEMS = 20; // Max node is below the ~2 KiB chunk policy.
  static inline var MAX_DEPTH = 32;
  // 128 MiB has at most 70,641 leaves; including all 20-way metadata levels
  // stays below this value. It also bounds hostile, non-canonical trees.
  static inline var MAX_NODES = 80000;
  static inline var MAX_REQUESTS = 80000;
  // A parent output and one large child may coexist while validating a tree.
  static inline var MAX_WORKING_BYTES = 268435456; // 256 MiB

  var options:ZsdTransferOptions;
  var lastRequest:Float;
  var requestCount:Int;
  var nodeCount:Int;
  var workingBytes:Int;
  var activeNodes:StringMap<Bool>;

  public function new(options:ZsdTransferOptions) {
    if (options.retries < 0 || options.retries > 8) ZsdBinary.fail("retries must be 0..8");
    if (options.paceMs < 0 || options.paceMs > 600000) ZsdBinary.fail("pace-ms must be 0..600000");
    this.options = options;
    this.options.endpoint = endpoint(options.endpoint);
    lastRequest = -1;
    requestCount = 0;
    nodeCount = 0;
    workingBytes = 0;
    activeNodes = new StringMap<Bool>();
  }

  public static function endpoint(value:String):String {
    if (value == null || value == "") ZsdBinary.fail("endpoint is required");
    var parts = strictEndpoint(value);
    if (parts.scheme != "https" && !isLoopback(parts.host))
      ZsdBinary.fail("endpoint must use HTTPS unless it is a loopback test endpoint");
    return parts.scheme + "://" + parts.host + (parts.port == null ? "" : ":" + parts.port) + "/";
  }

  /**
   * Deliberately checks the URI authority's host rather than comparing the
   * endpoint string: case, port 443, HTTP, and a suffix path must not turn the
   * public provider into an apparent custom endpoint for an opt-in check.
   */
  public static function isPublicEndpoint(value:String):Bool {
    if (value == null) return false;
    var re = ~/^[A-Za-z][A-Za-z0-9+.-]*:\/\/([^\/?#]*)/;
    if (!re.match(value)) return false;
    var authority = re.matched(1);
    var at = authority.lastIndexOf("@");
    if (at >= 0) authority = authority.substr(at + 1);
    var host = authority;
    if (StringTools.startsWith(host, "[")) {
      var close = host.indexOf("]");
      if (close < 0) return false;
      host = host.substr(0, close + 1);
    } else {
      var colon = host.lastIndexOf(":");
      if (colon >= 0) host = host.substr(0, colon);
    }
    return host.toLowerCase() == "is.gd";
  }

  public function upload(data:Bytes):String {
    if (data.length > MAX_TRANSFER_BYTES) ZsdBinary.fail("upload exceeds 128 MiB transfer bound");
    beginTransfer();
    var chunks = new Array<ZsdTreeItem>();
    var at = 0;
    // A zero-length container cannot occur, but making it represented is useful
    // for protocol verification and keeps tree semantics unambiguous.
    do {
      var n = data.length - at;
      if (n > CHUNK_BYTES) n = CHUNK_BYTES;
      var part = data.sub(at, n);
      chunks.push({id: put(part), length: n, digest: ZsdBinary.digest(part)});
      at += n;
    } while (at < data.length);
    return buildNodes(chunks, false).id;
  }

  public function download(id:String):Bytes {
    validateId(id);
    beginTransfer();
    return readNode(id, 0).data;
  }

  function buildNodes(items:Array<ZsdTreeItem>, children:Bool):ZsdTreeItem {
    var level = new Array<ZsdTreeItem>();
    var at = 0;
    while (at < items.length) {
      var take = items.length - at;
      if (take > GROUP_ITEMS) take = GROUP_ITEMS;
      var group = items.slice(at, at + take);
      var length = totalLength(group);
      var digest = combinedDigest(group); // metadata binds the exact payload sequence.
      var node:ZsdTreeNode = {children: children, length: length, digest: digest, items: group};
      var encoded = encodeNode(node);
      if (encoded.length > CHUNK_BYTES) ZsdBinary.fail("internal tree node exceeded chunk bound");
      level.push({id: put(encoded), length: length, digest: digest});
      at += take;
    }
    // Even a single leaf is referenced from this ZSDT metadata node. Never
    // expose a raw data chunk as the download root.
    return level.length == 1 ? level[0] : buildNodes(level, true);
  }

  function readNode(id:String, depth:Int):ZsdDownloadedNode {
    if (depth > MAX_DEPTH) ZsdBinary.fail("chunk tree is deeper than the supported bound");
    if (nodeCount >= MAX_NODES) ZsdBinary.fail("chunk tree exceeds node-count bound");
    if (activeNodes.exists(id)) ZsdBinary.fail("chunk tree contains a cycle");
    nodeCount++;
    activeNodes.set(id, true);
    var node = decodeNode(get(id));
    reserveWorking(node.length);
    try {
      var out = Bytes.alloc(node.length);
      var at = 0;
      for (item in node.items) {
        var part:Bytes;
        if (node.children) {
          var child = readNode(item.id, depth + 1);
          part = child.data;
          if (part.length != item.length || !ZsdBinary.equals(item.digest, child.digest))
            ZsdBinary.fail("child tree length or digest mismatch");
        } else {
          part = get(item.id);
          if (part.length != item.length || !ZsdBinary.equals(item.digest, ZsdBinary.digest(part)))
            ZsdBinary.fail("chunk length or digest mismatch");
        }
        if (at > out.length - part.length) ZsdBinary.fail("chunk tree overrun");
        out.blit(at, part, 0, part.length);
        at += part.length;
      }
      if (at != node.length || !ZsdBinary.equals(node.digest, combinedDigest(node.items)))
        ZsdBinary.fail("chunk tree root digest mismatch");
      workingBytes -= node.length;
      activeNodes.remove(id);
      return {data: out, digest: node.digest};
    } catch (e:Dynamic) {
      workingBytes -= node.length;
      activeNodes.remove(id);
      throw e;
    }
  }

  function encodeNode(node:ZsdTreeNode):Bytes {
    if (node.items.length < 1 || node.items.length > GROUP_ITEMS) ZsdBinary.fail("invalid tree item count");
    var out = new BytesOutput();
    out.writeString("ZSDT");
    ZsdBinary.u16(out, 1);
    ZsdBinary.u8(out, node.children ? 1 : 0);
    ZsdBinary.length(out, node.length);
    ZsdBinary.raw(out, node.digest);
    ZsdBinary.u16(out, node.items.length);
    for (item in node.items) {
      validateId(item.id);
      ZsdBinary.length(out, item.length);
      ZsdBinary.raw(out, item.digest);
      ZsdBinary.string(out, item.id);
    }
    return out.getBytes();
  }

  function decodeNode(data:Bytes):ZsdTreeNode {
    if (data.length > CHUNK_BYTES) ZsdBinary.fail("tree metadata chunk exceeded bound");
    var input = new ZsdReader(data);
    if (input.raw(4).toString() != "ZSDT") ZsdBinary.fail("root does not identify a ZSD chunk tree");
    if (input.u16() != 1) ZsdBinary.fail("unsupported ZSD chunk tree version");
    var kind = input.u8();
    if (kind != 0 && kind != 1) ZsdBinary.fail("unknown ZSD chunk tree node type");
    var length = input.length();
    if (length > MAX_TRANSFER_BYTES) ZsdBinary.fail("tree metadata exceeds 128 MiB transfer bound");
    var digest = input.raw(32);
    var count = input.u16();
    if (count < 1 || count > GROUP_ITEMS) ZsdBinary.fail("invalid ZSD chunk tree item count");
    var items = new Array<ZsdTreeItem>();
    for (_ in 0...count) {
      var itemLength = input.length();
      if (kind == 0 && itemLength > CHUNK_BYTES)
        ZsdBinary.fail("leaf chunk length exceeds chunk bound");
      var itemDigest = input.raw(32);
      var itemId = input.string();
      validateId(itemId);
      items.push({id: itemId, length: itemLength, digest: itemDigest});
    }
    input.end();
    if (length != totalLength(items)) ZsdBinary.fail("tree node length does not match children");
    return {children: kind == 1, length: length, digest: digest, items: items};
  }

  function put(data:Bytes):String {
    if (data.length > CHUNK_BYTES) ZsdBinary.fail("attempted upload chunk exceeds bound");
    var payload = Base64.encode(data).split("+").join(".").split("/").join("_");
    var url = options.endpoint + "create.php?format=simple&url=" + StringTools.urlEncode("http://ok.com/?" + payload);
    return shortId(request(url));
  }

  function get(id:String):Bytes {
    validateId(id);
    var response = StringTools.trim(request(options.endpoint + "forward.php?format=simple&shorturl=" + id));
    var prefix = "http://ok.com/?";
    if (!StringTools.startsWith(response, prefix))
      ZsdBinary.fail("provider returned a non-dummy response; it was not followed");
    var encoded = response.substr(prefix.length);
    if (!~/^[A-Za-z0-9._=]*$/.match(encoded)) ZsdBinary.fail("provider returned invalid encoded data");
    // Do this before Base64.decode allocates: a legal 1,900-byte chunk needs
    // no more than 2,536 Base64 characters including padding.
    if (encoded.length > 2536) ZsdBinary.fail("provider returned chunk exceeding bound");
    var decoded:Bytes;
    try {
      decoded = Base64.decode(encoded.split(".").join("+").split("_").join("/"));
    } catch (e:Dynamic) {
      ZsdBinary.fail("provider returned invalid base64: " + Std.string(e));
      return Bytes.alloc(0); // ZsdBinary.fail always throws; required by Haxe's return analysis.
    }
    if (decoded.length > CHUNK_BYTES) ZsdBinary.fail("provider returned chunk exceeding bound");
    return decoded;
  }

  function request(url:String):String {
    var attempt = 0;
    while (true) {
      if (requestCount >= MAX_REQUESTS) ZsdBinary.fail("transfer exceeds request-count bound");
      requestCount++;
      pace();
      try {
        var body = ZsdHttp.get(url, options.timeoutMs);
        if (isProviderError(body)) {
          if (isPublicEndpoint(options.endpoint) && isRateLimit(body)) {
            // is.gd documents HTTP 502 / code 3 as a rate limit: wait a full
            // minute before any further request, rather than rapid retries.
            if (attempt >= options.retries)
              ZsdBinary.fail("provider rate limited this request; wait at least one minute before retrying");
            waitMs(60000);
          } else if (attempt >= options.retries) {
            ZsdBinary.fail("provider rejected request: " + StringTools.trim(body));
          } else {
            waitMs(retryDelayMs(attempt));
          }
          attempt++;
          continue;
        }
        return body;
      } catch (e:Dynamic) {
        var message = Std.string(e);
        if (attempt >= options.retries) ZsdBinary.fail("request failed after " + (attempt + 1) + " attempt(s): " + message);
        var rate = isPublicEndpoint(options.endpoint) && isHttp502(message);
        waitMs(rate ? 60000 : retryDelayMs(attempt));
        attempt++;
      }
    }
    return ""; // unreachable
  }

  function pace():Void {
    if (lastRequest >= 0 && options.paceMs > 0) {
      var elapsed = Std.int((Sys.time() - lastRequest) * 1000);
      if (elapsed < options.paceMs) waitMs(options.paceMs - elapsed);
    }
    lastRequest = Sys.time();
  }

  function waitMs(ms:Int):Void {
    if (ms > 0) Sys.sleep(ms / 1000.0);
    lastRequest = Sys.time();
  }

  static function totalLength(items:Array<ZsdTreeItem>):Int {
    var total = 0;
    for (item in items) {
      if (item.length < 0 || item.length > MAX_TRANSFER_BYTES || total > MAX_TRANSFER_BYTES - item.length)
        ZsdBinary.fail("tree length exceeds 128 MiB transfer bound");
      total += item.length;
    }
    return total;
  }

  function beginTransfer():Void {
    requestCount = 0;
    nodeCount = 0;
    workingBytes = 0;
    activeNodes = new StringMap<Bool>();
  }

  function reserveWorking(length:Int):Void {
    if (length < 0 || length > MAX_TRANSFER_BYTES || workingBytes > MAX_WORKING_BYTES - length)
      ZsdBinary.fail("chunk tree exceeds working-memory bound");
    workingBytes += length;
  }

  static function retryDelayMs(attempt:Int):Int {
    // Timeout and ordinary provider failures back off quickly without turning
    // a transient error into an excessive multi-minute delay.
    var seconds = 1 << (attempt > 3 ? 3 : attempt);
    return seconds * 1000;
  }

  static function strictEndpoint(value:String):ZsdEndpointParts {
    var re = ~/^([A-Za-z][A-Za-z0-9+.-]*):\/\/([A-Za-z0-9.-]+|\[[0-9A-Fa-f:.]+\])(?::([0-9]{1,5}))?(\/?)$/;
    if (!re.match(value)) ZsdBinary.fail("endpoint must be an absolute http(s) origin without query, fragment, credentials, or path");
    var scheme = re.matched(1).toLowerCase();
    var host = re.matched(2).toLowerCase();
    var portText = re.matched(3);
    if (scheme != "http" && scheme != "https") ZsdBinary.fail("endpoint must use http or https");
    if (!validHost(host)) ZsdBinary.fail("endpoint host is invalid");
    var port:Null<Int> = null;
    if (portText != null) {
      port = Std.parseInt(portText);
      if (port == null || port < 1 || port > 65535) ZsdBinary.fail("endpoint port is invalid");
    }
    return {scheme: scheme, host: host, port: port};
  }

  static function validHost(host:String):Bool {
    if (StringTools.startsWith(host, "[")) return host == "[::1]" || ~/^\[[0-9a-f:.]+\]$/.match(host);
    if (host.length < 1 || host.length > 253) return false;
    var labels = host.split(".");
    for (label in labels) {
      if (label.length < 1 || label.length > 63 || !~/^[a-z0-9]([a-z0-9-]*[a-z0-9])?$/.match(label))
        return false;
    }
    return true;
  }

  static function isLoopback(host:String):Bool {
    if (host == "localhost" || host == "[::1]") return true;
    var octets = host.split(".");
    if (octets.length != 4 || octets[0] != "127") return false;
    for (octet in octets) {
      if (!~/^[0-9]{1,3}$/.match(octet)) return false;
      var n = Std.parseInt(octet);
      if (n == null || n < 0 || n > 255) return false;
    }
    return true;
  }

  static function combinedDigest(items:Array<ZsdTreeItem>):Bytes {
    // A canonical Merkle digest of the ordered child length/digest tuples.
    // Leaf digests are SHA-256(payload), so this still binds every byte while
    // allowing parents to be produced without buffering their whole subtree.
    var out = new BytesOutput();
    for (item in items) {
      ZsdBinary.length(out, item.length);
      ZsdBinary.raw(out, item.digest);
    }
    return ZsdBinary.digest(out.getBytes());
  }

  static function validateId(id:String):Void {
    if (!~/^[A-Za-z0-9_]{5,30}$/.match(id)) ZsdBinary.fail("invalid short id");
  }

  static function shortId(body:String):String {
    var result = StringTools.trim(body);
    if (~/^[A-Za-z0-9_]{5,30}$/.match(result)) return result;
    var q = result.indexOf("?");
    if (q >= 0) result = result.substr(0, q);
    var slash = result.lastIndexOf("/");
    if (slash >= 0) result = result.substr(slash + 1);
    validateId(result);
    return result;
  }

  static function isProviderError(body:String):Bool {
    var b = StringTools.trim(body).toLowerCase();
    return StringTools.startsWith(b, "error") || b.indexOf("errorcode=") >= 0;
  }

  static function isRateLimit(body:String):Bool {
    var b = body.toLowerCase();
    return b.indexOf("errorcode=3") >= 0 || b.indexOf("error: 3") >= 0 || b.indexOf("rate limit") >= 0;
  }

  static function isHttp502(message:String):Bool {
    return ~/(^|[^0-9])502([^0-9]|$)/.match(message);
  }
}