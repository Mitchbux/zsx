package zsd;

import haxe.Json;
import haxe.crypto.Sha256;
import haxe.io.Bytes;
import haxe.ds.StringMap;
import zsd.ZsdProtocol.ZsdTransferOptions;

typedef ZsdShortResponse = {
  var status:Int;
  var body:String;
  var retryAfterMs:Float;
  @:optional var cancelled:Bool;
}

/**
 * Short.io stores URLs, not arbitrary blobs. New uploads use the pex.wf
 * standard-Base64 carrier, not the account's shortening domain.
 * No redirects or provider-supplied URLs are followed.
 */
class ZsdShortIo {
  public static inline var CHUNK_BYTES = 1450;
  public static inline var MAX_URL_BYTES = 2048;
  public static inline var MAX_BATCH = 1000;
  public static inline var MAX_BODY_BYTES = 2097152;
  public static inline var API = "https://api.short.io/";
  public var domain(default, null):String;
  var options:ZsdTransferOptions;
  var origin:String;
  var credential:String;
  var transport:String->String->String->String->Int->ZsdShortResponse;
  var now:Void->Float;
  var sleep:Float->Void;
  var jitter:Void->Float;
  var cancellation:ZsdShortIoCancellation;
  var bulkCompleted:Float = -1;
  var lookupCompleted:Float = -1;
  var notBefore:Float = 0;
  var requests = 0;
  var known:StringMap<String>;

  public function new(options:ZsdTransferOptions,
      ?transport:String->String->String->String->Int->ZsdShortResponse,
      ?now:Void->Float, ?sleep:Float->Void, ?jitter:Void->Float) {
    this.options = options;
    if (options.timeoutMs < 1000 || options.timeoutMs > 300000)
      ZsdBinary.fail("timeout must be 1000..300000 ms");
    if (options.retries < 0 || options.retries > 8) ZsdBinary.fail("retries must be 0..8");
    if (options.paceMs < 0 || options.paceMs > 600000) ZsdBinary.fail("pace-ms must be 0..600000");
    domain = options.domain;
    if (domain == null || domain == "") domain = ZsdProtocol.DEFAULT_SHORT_IO_DOMAIN;
    if (domain.length > 253 ||
        !~/^[A-Za-z0-9](?:[A-Za-z0-9.-]*[A-Za-z0-9])?$/.match(domain) ||
        domain.indexOf(".") < 0)
      ZsdBinary.fail("invalid Short.io domain; use --domain HOST or SHORT_IO_DOMAIN (a domain configured in your Short.io account)");
    domain = domain.toLowerCase();
    ZsdProtocol.endpoint("https://" + domain + "/");
    origin = options.endpoint == ZsdProtocol.DEFAULT_ENDPOINT ? API : ZsdProtocol.endpoint(options.endpoint);
    // Credentials may only go to the official API or an explicitly selected loopback test server.
    if (origin != API && !~/^http:\/\/(?:localhost|127\.[0-9.]+|\[::1\])(?::[0-9]+)?\/$/.match(origin))
      ZsdBinary.fail("Short.io endpoint must be https://api.short.io/ or an HTTP loopback test origin");
    if (transport == null) {
      #if (cs && zsd_embedded_key)
      // Generated only by the explicitly private build; never a runtime fallback.
      credential = untyped __cs__("global::PrivateCredential.ApiKey");
      #else
      credential = Sys.getEnv("SHORT_IO_API_KEY");
      if (credential == null || StringTools.trim(credential) == "")
        ZsdBinary.fail("Short.io requires SHORT_IO_API_KEY in the runtime environment; do not put credentials in source or command arguments");
      #end
      if (credential.indexOf("\r") >= 0 || credential.indexOf("\n") >= 0)
        ZsdBinary.fail("SHORT_IO_API_KEY contains invalid header characters");
    } else credential = "";
    cancellation = new ZsdShortIoCancellation();
    this.transport = transport == null ? function(method, url, credential, body, timeout)
      return ZsdShortIoHttp.request(method, url, credential, body, timeout, cancellation) : transport;
    this.now = now == null ? ZsdShortIoHttp.monotonicMs : now;
    this.sleep = sleep == null ? function(ms) Sys.sleep(ms / 1000) : sleep;
    this.jitter = jitter == null ? Math.random : jitter;
    known = new StringMap<String>();
  }

  public function cancel():Void cancellation.cancel();

  public function begin():Void {
    cancellation.reset();
    requests = 0;
    known = new StringMap<String>();
  }

  public function putMany(urls:Array<String>):Array<String> {
    var ids = new Array<String>();
    var pending = new Array<Dynamic>();
    var included = new StringMap<Bool>();
    for (url in urls) {
      check();
      if (!StringTools.startsWith(url, ZsdProtocol.SHORT_IO_CARRIER_PREFIX) || Bytes.ofString(url).length > MAX_URL_BYTES)
        ZsdBinary.fail("Short.io target URL exceeds the conservative 2048-byte client bound or has an invalid prefix");
      ZsdProtocol.decodeTargetUrl(url, CHUNK_BYTES);
      var path = "zsd_" + Sha256.encode(url).substr(0, 26);
      ids.push(path);
      if (!known.exists(path) && !included.exists(path)) {
        pending.push({originalURL: url, path: path});
        included.set(path, true);
      }
    }
    var at = 0;
    while (at < pending.length) {
      var batch = new Array<Dynamic>();
      var size = Bytes.ofString(Json.stringify({domain: domain, allowDuplicates: false, links: []})).length;
      while (at < pending.length && batch.length < MAX_BATCH) {
        var extra = Bytes.ofString(Json.stringify(pending[at])).length + 1;
        if (size + extra > MAX_BODY_BYTES) break;
        size += extra;
        batch.push(pending[at++]);
      }
      if (batch.length == 0) ZsdBinary.fail("Short.io item exceeds request body bound");
      sendBatch(batch);
    }
    return ids;
  }

  function sendBatch(batch:Array<Dynamic>):Void {
    var unresolved = batch.copy();
    var attempt = 0;
    while (unresolved.length > 0) {
      var response = call("POST", "links/bulk", Json.stringify({
        domain: domain, allowDuplicates: false, links: unresolved
      }));
      fatalStatus(response.status);
      if (response.status >= 200 && response.status < 300) {
        var parsed:Dynamic = parse(response.body);
        if (!Std.isOfType(parsed, Array) || (cast parsed:Array<Dynamic>).length != unresolved.length)
          ZsdBinary.fail("Short.io malformed bulk response; upload incomplete, no root published. Rerun identical input to recover deterministic links");
        var results:Array<Dynamic> = cast parsed;
        var remaining = new Array<Dynamic>();
        for (i in 0...unresolved.length) {
          var item = results[i];
          var wanted = unresolved[i];
          if (item != null && Reflect.field(item, "error") == null && Reflect.field(item, "success") != false &&
              Reflect.field(item, "originalURL") == wanted.originalURL && Reflect.field(item, "path") == wanted.path) {
            known.set(wanted.path, wanted.originalURL);
          } else {
            var code:Dynamic = item == null ? null : Reflect.field(item, "statusCode");
            if (code == null && item != null) code = Reflect.field(item, "status");
            if (code == 401 || code == 403 || code == "401" || code == "403")
              ZsdBinary.fail("Short.io authorization failed in bulk item; upload incomplete");
            remaining.push(wanted);
          }
        }
        unresolved = remaining;
        if (unresolved.length == 0) return;
      } else if (!transient(response.status)) {
        ZsdBinary.fail("Short.io HTTP " + response.status + "; upload incomplete, no root published");
      }
      // A timeout/partial response does not mean creation failed. Reconcile each
      // unresolved deterministic path before attempting any further creation.
      if (attempt >= options.retries)
        ZsdBinary.fail("Short.io upload incomplete after bounded retries (" + unresolved.length +
          " unresolved links); no root published. Rerun identical input/domain to reuse deterministic links");
      defer(attempt++);
      var remaining = new Array<Dynamic>();
      for (wanted in unresolved) {
        var existing = lookup(wanted.path, true);
        if (existing == null) remaining.push(wanted);
        else if (existing != wanted.originalURL)
          ZsdBinary.fail("Short.io deterministic path collision or modified link; upload incomplete");
        else known.set(wanted.path, wanted.originalURL);
      }
      unresolved = remaining;
    }
  }

  public function get(path:String):String {
    var result = lookup(path, false);
    if (result == null) ZsdBinary.fail("Short.io link not found");
    return result;
  }

  function lookup(path:String, missingAllowed:Bool):Null<String> {
    var attempt = 0;
    while (true) {
      var response = call("GET", "links/expand?domain=" + StringTools.urlEncode(domain) +
        "&path=" + StringTools.urlEncode(path), null);
      fatalStatus(response.status);
      if (response.status == 404 && missingAllowed) return null;
      if (response.status == 200) {
        var item = parse(response.body);
        var url:Dynamic = Reflect.field(item, "originalURL");
        if (!Std.isOfType(url, String) || Reflect.field(item, "path") != path ||
            Bytes.ofString(cast url).length > MAX_URL_BYTES)
          ZsdBinary.fail("Short.io malformed link response");
        return cast url;
      }
      if (!transient(response.status) || attempt >= options.retries)
        ZsdBinary.fail("Short.io lookup HTTP " + response.status + "; transfer incomplete");
      defer(attempt++);
    }
    return null;
  }

  function call(method:String, relative:String, body:String):ZsdShortResponse {
    check();
    if (++requests > 400000) ZsdBinary.fail("Short.io transfer exceeds request-count bound");
    var completed = method == "POST" ? bulkCompleted : lookupCompleted;
    var pace = Math.max(method == "POST" ? 3000 : 100, options.paceMs);
    var due = Math.max(notBefore, completed < 0 ? 0 : completed + pace);
    while (true) {
      check();
      // Read the clock once: crossing the deadline between two reads could
      // otherwise pass -1 ms to .NET Thread.Sleep, which means sleep forever.
      var remaining = due - now();
      if (remaining <= 0) break;
      sleep(Math.min(1000, Math.ceil(remaining)));
    }
    check();
    var result:ZsdShortResponse;
    try {
      result = transport(method, origin + relative, credential, body, options.timeoutMs);
    } catch (_:Dynamic) {
      // Never expose provider bodies, headers, URL payloads, or network exceptions.
      result = {status: 0, body: "", retryAfterMs: 0};
    }
    completed = now();
    if (method == "POST") bulkCompleted = completed; else lookupCompleted = completed;
    if (result.retryAfterMs > 0) notBefore = Math.max(notBefore, completed + result.retryAfterMs);
    if (result.cancelled == true) cancellation.cancel();
    check();
    return result;
  }

  function defer(attempt:Int):Void {
    var delay = Math.min(60000, 1000 * Math.pow(2, attempt)) + Math.floor(jitter() * 1000);
    notBefore = Math.max(notBefore, now() + delay);
  }

  function check():Void {
    if (cancellation.isCancelled()) ZsdBinary.fail("Short.io transfer cancelled; incomplete links may remain; rerun identical input to recover");
  }

  static function transient(status:Int):Bool return status == 0 || status == 429 || (status >= 500 && status <= 599);

  static function fatalStatus(status:Int):Void {
    if (status == 401 || status == 403)
      ZsdBinary.fail("Short.io authorization failed (HTTP " + status + "); verify runtime credential and domain access");
  }

  static function parse(body:String):Dynamic {
    try {
      return Json.parse(body);
    } catch (_:Dynamic) {
      ZsdBinary.fail("Short.io malformed JSON response; transfer incomplete. Rerun identical input to recover deterministic links");
    }
    return null;
  }
}

#if cs
@:classCode('public volatile bool IsCancelled;')
#end
class ZsdShortIoCancellation {
  var stopped = false;
  public function new() {}
  #if cs
  @:functionCode('this.IsCancelled = true;')
  #end
  public function cancel():Void stopped = true;
  #if cs
  @:functionCode('this.IsCancelled = false;')
  #end
  public function reset():Void stopped = false;
  #if cs
  @:functionCode('return this.IsCancelled;')
  #end
  public function isCancelled():Bool return stopped;
}

class ZsdShortIoHttp {
  #if cs
  @:functionCode('return (double)global::System.Diagnostics.Stopwatch.GetTimestamp() * 1000.0 / global::System.Diagnostics.Stopwatch.Frequency;')
  #end
  public static function monotonicMs():Float return haxe.Timer.stamp() * 1000;

  public static function request(method:String, url:String, credential:String, body:String, timeoutMs:Int,
      ?cancellation:ZsdShortIoCancellation):ZsdShortResponse {
    if (timeoutMs < 1000 || timeoutMs > 300000) ZsdBinary.fail("timeout must be 1000..300000 ms");
    #if cs
    if (cancellation == null) cancellation = new ZsdShortIoCancellation();
    var wire = raw(method, url, credential, body, timeoutMs, cancellation);
    var first = wire.indexOf("\n");
    var second = wire.indexOf("\n", first + 1);
    var status = Std.parseInt(wire.substr(0, first));
    return {status: status, cancelled: status == -2,
      retryAfterMs: Std.parseFloat(wire.substring(first + 1, second)), body: wire.substr(second + 1)};
    #else
    ZsdBinary.fail("Short.io network transport requires the standalone .NET build");
    return null;
    #end
  }

  #if cs
  @:functionCode('
    global::System.Net.HttpWebRequest request = null;
    global::System.Net.HttpWebResponse response = null;
    global::System.Threading.Timer timer = null;
    bool fullyRead = false;
    var elapsed = global::System.Diagnostics.Stopwatch.StartNew();
    int abortReason = 0;
    double wait = 0;
    global::System.Func<string> failure = delegate {
      return (cancellation.IsCancelled || global::System.Threading.Interlocked.CompareExchange(ref abortReason, 0, 0) == 2
        ? "-2" : "0") + "\\n" + wait.ToString(global::System.Globalization.CultureInfo.InvariantCulture) + "\\n";
    };
    try {
      if (cancellation.IsCancelled) return failure();
      if (url.StartsWith("https://", global::System.StringComparison.OrdinalIgnoreCase))
        global::System.Net.ServicePointManager.SecurityProtocol = (global::System.Net.SecurityProtocolType)3072;
      // GetResponse must surface error headers without internally buffering a
      // slow error body before throwing its WebException (including on Mono).
      global::System.Net.HttpWebRequest.DefaultMaximumErrorResponseLength = 0;
      request = (global::System.Net.HttpWebRequest)global::System.Net.WebRequest.Create(url);
      global::System.Action checkDeadline = delegate {
        int reason = cancellation.IsCancelled ? 2 : elapsed.ElapsedMilliseconds >= timeoutMs ? 1 : 0;
        if (reason != 0 && global::System.Threading.Interlocked.CompareExchange(ref abortReason, reason, 0) == 0) {
          try { request.Abort(); } catch (global::System.Exception) {}
        }
      };
      // One pooled timer covers request-stream acquisition/write, headers, and every
      // response read. Per-read socket timeouts alone do not bound slow-drip bodies.
      timer = new global::System.Threading.Timer(delegate(object state) { checkDeadline(); }, null, 0, 25);
      request.Method = method;
      request.Timeout = timeoutMs;
      request.ReadWriteTimeout = timeoutMs;
      request.AllowAutoRedirect = false;
      request.UserAgent = "Zsd/1.1 (.NET Framework 4.0)";
      request.Headers["Authorization"] = credential;
      checkDeadline();
      if (global::System.Threading.Interlocked.CompareExchange(ref abortReason, 0, 0) != 0) return failure();
      if (body != null) {
        byte[] bytes = global::System.Text.Encoding.UTF8.GetBytes(body);
        request.ContentType = "application/json";
        request.ContentLength = bytes.Length;
        using (var stream = request.GetRequestStream()) {
          stream.Write(bytes, 0, bytes.Length);
        }
      }
      checkDeadline();
      try { response = (global::System.Net.HttpWebResponse)request.GetResponse(); }
      catch (global::System.Net.WebException e) {
        response = e.Response as global::System.Net.HttpWebResponse;
        if (response == null) return failure();
      }
      checkDeadline();
      string retry = response.Headers["Retry-After"];
      double seconds;
      global::System.DateTimeOffset date;
      if (retry != null && global::System.Double.TryParse(retry, global::System.Globalization.NumberStyles.None,
          global::System.Globalization.CultureInfo.InvariantCulture, out seconds) && seconds >= 0) {
        wait = global::System.Math.Ceiling(seconds * 1000);
      } else if (retry != null && global::System.DateTimeOffset.TryParse(retry,
          global::System.Globalization.CultureInfo.InvariantCulture,
          global::System.Globalization.DateTimeStyles.AssumeUniversal, out date)) {
        wait = global::System.Math.Max(0, global::System.Math.Ceiling((date - global::System.DateTimeOffset.UtcNow).TotalMilliseconds));
      }
      string prefix = ((int)response.StatusCode).ToString(global::System.Globalization.CultureInfo.InvariantCulture)
        + "\\n" + wait.ToString(global::System.Globalization.CultureInfo.InvariantCulture) + "\\n";
      // Errors need only status and Retry-After. In particular, a slow-drip
      // 401/403 body must never turn immediate rejection into a network retry.
      if ((int)response.StatusCode < 200 || (int)response.StatusCode >= 300) return prefix;
      const int Limit = 8388608;
      if (response.ContentLength > Limit) return prefix;
      using (var stream = response.GetResponseStream())
      using (var collected = new global::System.IO.MemoryStream()) {
        var buffer = new byte[8192];
        int count;
        while ((count = stream.Read(buffer, 0, buffer.Length)) > 0) {
          checkDeadline();
          if (global::System.Threading.Interlocked.CompareExchange(ref abortReason, 0, 0) != 0) return failure();
          if (collected.Length + count > Limit) return prefix;
          collected.Write(buffer, 0, count);
        }
        string result = prefix + global::System.Text.Encoding.UTF8.GetString(collected.ToArray());
        checkDeadline();
        if (global::System.Threading.Interlocked.CompareExchange(ref abortReason, 0, 0) != 0) return failure();
        fullyRead = true;
        return result;
      }
    } catch (global::System.Exception) {
      return failure();
    } finally {
      // A rejected/oversized or incomplete body must not be drained indefinitely
      // by response disposal. Abort first, while the deadline timer is still live.
      if (!fullyRead && request != null) {
        try { request.Abort(); } catch (global::System.Exception) {}
      }
      if (response != null) {
        try { response.Close(); } catch (global::System.Exception) {}
      }
      // Wait for any queued callback before returning to the caller. No
      // per-request worker thread or live timer survives this finally block.
      if (timer != null) {
        using (var drained = new global::System.Threading.ManualResetEvent(false)) {
          if (timer.Dispose(drained)) drained.WaitOne();
        }
      }
      elapsed.Stop();
    }
  ')
  static function raw(method:String, url:String, credential:String, body:String, timeoutMs:Int, cancellation:ZsdShortIoCancellation):String return null;
  #end
}