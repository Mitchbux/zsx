package zsd.test;

import haxe.Json;
import haxe.io.Bytes;
import haxe.ds.StringMap;
import zsd.ZsdShortIo;
import zsd.ZsdShortIo.ZsdShortResponse;
import zsd.ZsdShortIo.ZsdShortIoHttp;
import zsd.ZsdShortIo.ZsdShortIoCancellation;
import zsd.ZsdProtocol;
import zsd.ZsdProtocol.ZsdTransferOptions;

class ZsdShortIoTest {
  static function require(value:Bool, message:String):Void {
    if (!value) throw "ZsdShortIoTest: " + message;
  }

  static function options():ZsdTransferOptions return {
    endpoint: "http://127.0.0.1:1/", domain: "offline.example", provider: "shortio",
    timeoutMs: 1000, retries: 2, paceMs: 0
  };

  static function urls(count:Int):Array<String> {
    return [for (i in 0...count) ZsdProtocol.SHORT_IO_CARRIER_PREFIX +
      haxe.crypto.Base64.encode(Bytes.ofString("offline chunk " + i))];
  }

  static function response(status:Int, value:Dynamic, wait:Float = 0):ZsdShortResponse
    return {status: status, body: Json.stringify(value), retryAfterMs: wait};

  static function fails(action:Void->Void, label:String):Void {
    var failed = false;
    try action() catch (_:Dynamic) failed = true;
    require(failed, label);
  }

  static function main():Void {
    if (Sys.args().length == 2 && Sys.args()[0] == "--wire") {
      wire(Sys.args()[1]);
      return;
    }
    carrierEncoding();
    defaultDomain();
    deadlineCrossing();
    batchAndRoundTrip();
    retryAndPartial();
    largeReconciliation();
    fatalAndMalformed();
    cancellation();
    Sys.println("ZsdShortIoTest OK: exact standard-Base64 carrier, old/legacy compatibility, invalid carriers, batch boundaries, byte-exact tree, stable recovery, retry floors, fatal errors, malformed responses, cancellation");
  }

  static function carrierEncoding():Void {
    var data = Bytes.ofHex("fbff");
    var url = "https://pex.wf/#+/8=";
    require(@:privateAccess ZsdProtocol.targetUrl(data, "shortio") == url, "literal + / = carrier");
    require(@:privateAccess ZsdProtocol.targetUrl(Bytes.ofHex("fb"), "shortio") == "https://pex.wf/#+w==",
      "double padding retained");
    require(@:privateAccess ZsdProtocol.targetUrl(Bytes.ofHex("fbffff"), "shortio") == "https://pex.wf/#+///",
      "no padding for a three-byte group");
    require(@:privateAccess ZsdProtocol.targetUrl(data) == "http://ok.com/?._8=", "legacy writing unchanged");
    require(ZsdProtocol.decodeTargetUrl(url).compare(data) == 0, "new carrier byte-exact decode");
    require(ZsdProtocol.decodeTargetUrl("http://ok.com/?._8=").compare(data) == 0,
      "old Short.io and legacy custom alphabet decode");
    require(ZsdProtocol.decodeTargetUrl("https://pex.wf/#").length == 0, "canonical empty transport chunk");
    var limit = Bytes.alloc(ZsdShortIo.CHUNK_BYTES);
    var largest = ZsdProtocol.SHORT_IO_CARRIER_PREFIX + haxe.crypto.Base64.encode(limit);
    require(largest.length <= ZsdShortIo.MAX_URL_BYTES &&
      ZsdProtocol.decodeTargetUrl(largest, ZsdShortIo.CHUNK_BYTES).length == 1450, "1450-byte carrier allowed");

    var calls = 0;
    var clock:Float = 0;
    var backend = new ZsdShortIo(options(), function(method, url, credential, body, timeout) {
      calls++;
      require(Json.parse(body).domain == "offline.example", "carrier must not replace shortening domain");
      return response(200, Json.parse(body).links);
    }, function() return clock, function(ms) clock += ms);
    require(backend.putMany([url, largest]).length == 2 && calls == 1, "valid standard carriers submitted");
    for (invalid in [
      "https://pex.wf/#._8=", "https://pex.wf/#-_8=", "https://pex.wf/#%2B%2F8%3D",
      "https://pex.wf/#+/8", "https://pex.wf/#+/8==", "https://pex.wf/#=+/8",
      "https://pex.wf/#+/9=", "https://pex.wf/#+x==", "https://pex.wf/#A",
      "https://pex.wf/#+ /8=", "https://pex.wf/#+/8=\n", " https://pex.wf/#+/8=",
      "http://pex.wf/#+/8=", "https://pex.wf/?+/8=", "https://pex.wf.evil/#+/8=",
      "https://PEX.WF/#+/8="
    ]) {
      fails(function() ZsdProtocol.decodeTargetUrl(invalid), "invalid carrier read rejected");
      fails(function() backend.putMany([invalid]), "invalid carrier write rejected before POST");
    }
    fails(function() backend.putMany(["http://ok.com/?._8="]), "old prefix is read-only for Short.io");
    var oversizedChunk = ZsdProtocol.SHORT_IO_CARRIER_PREFIX + haxe.crypto.Base64.encode(Bytes.alloc(1451));
    fails(function() backend.putMany([oversizedChunk]), "1451-byte payload rejected even when URL is under 2048");
    fails(function() ZsdProtocol.decodeTargetUrl(oversizedChunk, 1450), "oversized new read rejected");
    fails(function() ZsdProtocol.decodeTargetUrl("http://ok.com/?" +
      haxe.crypto.Base64.encode(Bytes.alloc(1451)), 1450), "oversized old Short.io read rejected");
    fails(function() backend.putMany([ZsdProtocol.SHORT_IO_CARRIER_PREFIX + StringTools.lpad("", "A", 2048)]),
      "2048-byte URL cap preserved");
    require(calls == 1, "invalid carriers never reach the provider");

    // Check the actual Short.io read path does not trim or normalize originalURL.
    var returned = url;
    var reader = new ZsdShortIo(options(), function(method, url, credential, body, timeout) {
      return response(200, {path: "mock_path", originalURL: returned});
    }, function() return clock, function(ms) clock += ms);
    var config = options();
    config.provider = "legacy";
    var protocol = new ZsdProtocol(config);
    @:privateAccess protocol.shortIo = reader;
    require((@:privateAccess protocol.get("mock_path")).compare(data) == 0, "provider read uses new decoder");
    returned = "http://ok.com/?._8=";
    require((@:privateAccess protocol.get("mock_path")).compare(data) == 0, "provider read preserves old decoder");
    returned = " " + url;
    fails(function() { @:privateAccess protocol.get("mock_path"); }, "provider originalURL prefix is exact");
  }

  static function defaultDomain():Void {
    var config = options();
    config.domain = null;
    var called = false;
    var backend = new ZsdShortIo(config, function(method, url, credential, body, timeout) {
      called = true;
      require(Json.parse(body).domain == "pex.s.gy", "compiled domain fallback without config");
      return response(200, Json.parse(body).links);
    });
    backend.putMany(urls(1));
    require(called && backend.domain == "pex.s.gy", "default Short.io domain");
  }

  static function deadlineCrossing():Void {
    var clock:Float = 97;
    var sleeps = new Array<Float>();
    var backend = new ZsdShortIo(options(), function(method, url, credential, body, timeout)
      return response(200, {path: "clocktest", originalURL: urls(1)[0]}),
      function() { clock += 2; return clock; },
      function(ms) {
        require(ms > 0 && ms <= 1000, "pacing must never pass a nonpositive wait to .NET");
        sleeps.push(ms);
      });
    @:privateAccess backend.lookupCompleted = 0;
    require(backend.get("clocktest") == urls(1)[0], "deadline-crossing lookup completes");
    require(sleeps.length == 1 && sleeps[0] == 1, "single clock sample bounds the deadline-crossing wait");
  }

  static function wire(origin:String):Void {
    var seconds = ZsdShortIoHttp.request("GET", origin + "seconds", "offline-test-only", null, 1000);
    require(seconds.status == 429 && seconds.retryAfterMs == 7000, "wire Retry-After seconds");
    var date = ZsdShortIoHttp.request("GET", origin + "date", "offline-test-only", null, 1000);
    require(date.status == 503 && date.retryAfterMs >= 118000 && date.retryAfterMs <= 120000, "wire Retry-After HTTP date");
    var redirect = ZsdShortIoHttp.request("GET", origin + "redirect", "offline-test-only", null, 1000);
    require(redirect.status == 302, "wire does not follow redirect");
    #if cs
    var start = ZsdShortIoHttp.monotonicMs();
    var drip = ZsdShortIoHttp.request("GET", origin + "slowdrip", "offline-test-only", null, 1000);
    var duration = ZsdShortIoHttp.monotonicMs() - start;
    require(drip.status == 0 && duration >= 800 && duration < 2500, "slow-drip response has a hard total deadline");
    start = ZsdShortIoHttp.monotonicMs();
    var headers = ZsdShortIoHttp.request("GET", origin + "slowheaders", "offline-test-only", null, 1000);
    var headerDuration = ZsdShortIoHttp.monotonicMs() - start;
    require(headers.status == 0 && headerDuration >= 800 && headerDuration < 2500, "headers share hard total deadline");
    start = ZsdShortIoHttp.monotonicMs();
    var oversized = ZsdShortIoHttp.request("GET", origin + "oversize", "offline-test-only", null, 1000);
    require(oversized.body == "" && ZsdShortIoHttp.monotonicMs() - start < 1500,
      "oversized response rejected without unbounded disposal/draining");
    start = ZsdShortIoHttp.monotonicMs();
    var denied = ZsdShortIoHttp.request("GET", origin + "denieddrip", "offline-test-only", null, 5000);
    var deniedDuration = ZsdShortIoHttp.monotonicMs() - start;
    require(denied.status == 401 && deniedDuration < 1500,
      "authorization failure does not wait for a slow-drip error body (status=" + denied.status +
      ", elapsed ms=" + Std.int(deniedDuration) + ")");
    var state = new ZsdShortIoCancellation();
    var timer = cancelAfter(state);
    start = ZsdShortIoHttp.monotonicMs();
    var cancelled = ZsdShortIoHttp.request("GET", origin + "slowdrip", "offline-test-only", null, 5000, state);
    var cancelDuration = ZsdShortIoHttp.monotonicMs() - start;
    stopTimer(timer);
    require(cancelled.cancelled == true && cancelled.status == -2 &&
      cancelDuration >= 150 && cancelDuration < 1800, "in-flight cancellation aborts response stream before deadline");
    var config = options();
    config.endpoint = origin;
    config.timeoutMs = 5000;
    var backend = new ZsdShortIo(config);
    timer = cancelBackendAfter(backend);
    start = ZsdShortIoHttp.monotonicMs();
    var message = "";
    try backend.get("slowdrip") catch (e:Dynamic) message = Std.string(e);
    var backendDuration = ZsdShortIoHttp.monotonicMs() - start;
    stopTimer(timer);
    require(message.indexOf("cancelled") >= 0 && backendDuration < 1800,
      "backend.cancel aborts in-flight request without retry");
    Sys.println("Wire monotonic elapsed ms: slow-drip deadline=" + Std.int(duration) +
      ", header deadline=" + Std.int(headerDuration) + ", direct cancellation=" + Std.int(cancelDuration) +
      ", backend cancellation=" + Std.int(backendDuration));
    #end
    Sys.println("ZsdShortIoWireTest OK: Retry-After seconds/date, redirects, hard deadline and in-flight cancellation");
  }

  #if cs
  @:functionCode('return new global::System.Threading.Timer(delegate(object ignored) { state.cancel(); }, null, 250, global::System.Threading.Timeout.Infinite);')
  static function cancelAfter(state:ZsdShortIoCancellation):Dynamic return null;
  @:functionCode('return new global::System.Threading.Timer(delegate(object ignored) { backend.cancel(); }, null, 250, global::System.Threading.Timeout.Infinite);')
  static function cancelBackendAfter(backend:ZsdShortIo):Dynamic return null;
  @:functionCode('
    using (var drained = new global::System.Threading.ManualResetEvent(false)) {
      if (((global::System.Threading.Timer)timer).Dispose(drained)) drained.WaitOne();
    }
  ')
  static function stopTimer(timer:Dynamic):Void {}
  #end

  static function batchAndRoundTrip():Void {
    var clock:Float = 0;
    var bulkCompletion:Float = -1;
    var lookupCompletion:Float = -1;
    var batches = new Array<Int>();
    var stored = new StringMap<String>();
    var mock = function(method:String, url:String, credential:String, body:String, timeout:Int):ZsdShortResponse {
      require(credential == "", "injected transport must not load environment credential");
      var completion = method == "POST" ? bulkCompletion : lookupCompletion;
      require(completion < 0 || clock - completion >= (method == "POST" ? 3000 : 100), "per-endpoint completion-to-start pacing");
      clock += 275;
      if (method == "POST") bulkCompletion = clock; else lookupCompletion = clock;
      if (method == "GET") {
        var path = StringTools.urlDecode(url.substr(url.lastIndexOf("&path=") + 6));
        return stored.exists(path) ? response(200, {path: path, originalURL: stored.get(path)}) : response(404, {});
      }
      require(Bytes.ofString(body).length <= ZsdShortIo.MAX_BODY_BYTES, "body budget");
      var links:Array<Dynamic> = Json.parse(body).links;
      require(Json.parse(body).allowDuplicates == false, "duplicates disabled");
      batches.push(links.length);
      for (link in links) {
        require(link.originalURL.length <= 2048, "target URL limit");
        require(StringTools.startsWith(link.originalURL, "https://pex.wf/#"), "new carrier for every chunk and metadata node");
        ZsdProtocol.decodeTargetUrl(link.originalURL, ZsdShortIo.CHUNK_BYTES);
        stored.set(link.path, link.originalURL);
      }
      return response(200, links);
    };
    var backend = new ZsdShortIo(options(), mock, function() return clock, function(ms) clock += ms, function() return 0);
    var refs = backend.putMany(urls(1001));
    require(refs.length == 1001 && batches.length == 2 && batches[0] == 1000 && batches[1] == 1, "1000 + remainder");
    for (i in [0, 999, 1000]) require(backend.get(refs[i]) == urls(1001)[i], "ordered references");
    backend.begin();
    var repeated = backend.putMany(urls(1001));
    require(Json.stringify(refs) == Json.stringify(repeated), "stable references across rerun");
    require([for (k in stored.keys()) k].length == 1001, "rerun has no extra links");

    // Inject only the provider, keeping the real versioned tree/base64/digest codec.
    var legacyOptions = options();
    legacyOptions.provider = "legacy";
    var protocol = new ZsdProtocol(legacyOptions);
    @:privateAccess protocol.shortIo = backend;
    var data = Bytes.alloc(1450 * 1001 + 7);
    var state = 42;
    for (i in 0...data.length) {
      state = (state * 1664525 + 1013904223);
      data.set(i, state >>> 24);
    }
    var root = protocol.upload(data);
    require(StringTools.startsWith(root, "zsd-sio-v1:offline.example:"), "provider/domain versioned reference");
    require(protocol.download(root).compare(data) == 0, "1001+ chunks byte-exact tree roundtrip");
    var stableRoot = protocol.upload(data);
    require(stableRoot == root, "stable root on rerun");
    // Re-encode the complete stored tree using historical carriers, including
    // every metadata level. Versioned roots remain readable without rewriting.
    for (path in stored.keys()) {
      var bytes = ZsdProtocol.decodeTargetUrl(stored.get(path), ZsdShortIo.CHUNK_BYTES);
      @:privateAccess stored.set(path, ZsdProtocol.targetUrl(bytes));
    }
    require(protocol.download(root).compare(data) == 0, "old Short.io multilevel tree remains readable");
  }

  static function retryAndPartial():Void {
    for (status in [0, 429, 500, 503]) {
      var clock:Float = 0;
      var posts = 0;
      var gets = 0;
      var backend = new ZsdShortIo(options(), function(method, url, credential, body, timeout) {
        if (method == "GET") {
          gets++;
          require(clock >= 90000, "Retry-After floor is not capped to backoff");
          return response(404, {});
        }
        posts++;
        if (posts == 1) return response(status, {}, 90000);
        require(clock >= 90000, "bulk retry obeys shared Retry-After floor across endpoint changes");
        return response(200, Json.parse(body).links);
      }, function() return clock, function(ms) clock += ms, function() return 0.75);
      require(backend.putMany(urls(1)).length == 1 && posts == 2 && gets == 1, "transient retry with reconciliation");
    }
    var clock:Float = 0;
    var posts = 0;
    var gets = 0;
    var backend = new ZsdShortIo(options(), function(method, url, credential, body, timeout) {
      if (method == "GET") { gets++; return response(404, {}); }
      posts++;
      var links:Array<Dynamic> = Json.parse(body).links;
      if (posts == 1) return response(200, [links[0], {error: "temporary", statusCode: 503}, links[2]]);
      require(links.length == 1 && links[0].originalURL == urls(3)[1], "only unresolved item retried");
      return response(200, links);
    }, function() return clock, function(ms) clock += ms, function() return 0);
    require(backend.putMany(urls(3)).length == 3 && posts == 2 && gets == 1, "partial success preserved");

    // Simulate committed server state followed by a lost response. Lookup must
    // recover it, with no second POST and no duplicated link.
    var committed:Dynamic = null;
    posts = 0;
    backend = new ZsdShortIo(options(), function(method, url, credential, body, timeout) {
      if (method == "GET") return response(200, committed);
      posts++;
      committed = Json.parse(body).links[0];
      throw "simulated lost response";
    }, function() return clock, function(ms) clock += ms, function() return 0);
    require(backend.putMany(urls(1)).length == 1 && posts == 1, "timeout reconciled without duplicate creation");

    var retryOptions = options();
    retryOptions.retries = 8;
    var starts = new Array<Float>();
    clock = 0;
    backend = new ZsdShortIo(retryOptions, function(method, url, credential, body, timeout) {
      starts.push(clock);
      return starts.length <= 8 ? response(503, {}) :
        response(200, {path: "mock_path", originalURL: urls(1)[0]});
    }, function() return clock, function(ms) clock += ms, function() return 0.75);
    require(backend.get("mock_path") == urls(1)[0], "bounded exponential recovery");
    require(starts.length == 9 && starts[3] - starts[2] == 4750 &&
      starts[8] - starts[7] == 60750, "exponential backoff, jitter, and cap");
  }

  static function largeReconciliation():Void {
    var clock:Float = 0;
    var posts = 0;
    var gets = 0;
    var lastBulk:Float = -1;
    var backend = new ZsdShortIo(options(), function(method, url, credential, body, timeout) {
      if (method == "GET") { gets++; return response(404, {}); }
      require(lastBulk < 0 || clock - lastBulk >= 3000, "bulk gate survives intervening GETs");
      lastBulk = clock;
      posts++;
      return posts == 1 ? response(503, {}) : response(200, Json.parse(body).links);
    }, function() return clock, function(ms) clock += ms, function() return 0);
    require(backend.putMany(urls(1000)).length == 1000 && gets == 1000 && posts == 2, "1000 unresolved links reconciled");
    require(clock == 100900, "1000 lookups take about 100 seconds, not 3000 seconds");

    // A successful GET with Retry-After must also delay a subsequent bulk
    // creation, even when the bulk endpoint has no prior local gate.
    clock = 0;
    backend = new ZsdShortIo(options(), function(method, url, credential, body, timeout) {
      if (method == "GET") return response(200, {path: "mock_path", originalURL: urls(1)[0]}, 45000);
      require(clock >= 45000, "GET Retry-After is a global floor for bulk POST");
      return response(200, Json.parse(body).links);
    }, function() return clock, function(ms) clock += ms, function() return 0);
    backend.get("mock_path");
    require(backend.putMany(urls(1)).length == 1, "global floor retained when switching endpoints");
    Sys.println("Virtual-clock reconciliation: 1000 unresolved links = 100900 ms; independent bulk gate >=3000 ms");
  }

  static function fatalAndMalformed():Void {
    for (status in [401, 403, 400, 302]) {
      var calls = 0;
      var backend = new ZsdShortIo(options(), function(method, url, credential, body, timeout) {
        calls++;
        return response(status, {});
      });
      fails(function() backend.putMany(urls(1)), "fatal status rejected");
      require(calls == 1, "fatal response must not retry or follow redirect");
    }
    for (status in [401, 403]) {
      var calls = 0;
      var backend = new ZsdShortIo(options(), function(method, url, credential, body, timeout) {
        calls++;
        return response(200, [{error: "denied", statusCode: status}]);
      });
      fails(function() backend.putMany(urls(1)), "item authorization failure rejected");
      require(calls == 1, "item authorization failure must not retry");
    }
    for (body in ["not JSON", "{}", "[]", "[null]"]) {
      var clock:Float = 0;
      var calls = 0;
      var backend = new ZsdShortIo(options(), function(method, url, credential, requestBody, timeout) {
        calls++;
        return {status: 200, body: body, retryAfterMs: 0};
      }, function() return clock, function(ms) clock += ms);
      fails(function() backend.putMany(urls(1)), "malformed response cannot succeed");
      require(calls <= 2, "malformed data bounded");
    }
    var clock:Float = 0;
    var calls = 0;
    var backend = new ZsdShortIo(options(), function(method, url, credential, body, timeout) {
      calls++;
      return response(503, {});
    }, function() return clock, function(ms) clock += ms);
    fails(function() backend.putMany(urls(1)), "persistent server failure bounded");
    require(calls == 4, "one bulk plus bounded lookup attempts");
  }

  static function cancellation():Void {
    var clock:Float = 0;
    var calls = 0;
    var backend:ZsdShortIo = null;
    backend = new ZsdShortIo(options(), function(method, url, credential, body, timeout) {
      calls++;
      return response(429, {}, 100000);
    }, function() return clock, function(ms) {
      clock += ms;
      backend.cancel();
    });
    fails(function() backend.putMany(urls(1)), "cancellation stops retry wait");
    require(calls == 1 && clock <= 1000, "cancellation checked during wait");
  }
}