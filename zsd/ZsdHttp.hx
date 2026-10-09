package zsd;


/**
 * The only target-specific operation: a bounded synchronous GET built on
 * HttpWebRequest so generated C# remains compatible with .NET Framework 4.0.
 */
class ZsdHttp {
  /** Text responses are provider control data, never transfer payloads. */
  public static inline var MAX_RESPONSE_BYTES = 16384;

  public static function get(url:String, timeoutMs:Int):String {
    if (timeoutMs < 1000 || timeoutMs > 300000) ZsdBinary.fail("timeout must be 1000..300000 ms");
    #if cs
    return ZsdHttpCs.get(url, timeoutMs);
    #else
    var http = new sys.Http(url);
    http.cnxTimeout = timeoutMs / 1000.0;
    var body:Null<String> = null;
    var error:Null<String> = null;
    http.onData = function(value) body = value;
    http.onError = function(value) error = value;
    http.request(false);
    if (error != null) ZsdBinary.fail("HTTP request failed: " + error);
    if (body == null) ZsdBinary.fail("HTTP request returned no body");
    if (body.length > MAX_RESPONSE_BYTES) ZsdBinary.fail("HTTP response exceeds 16 KiB bound");
    return body;
    #end
  }
}

#if cs
private class ZsdHttpCs {
  @:functionCode('
    try {
      if (url.StartsWith("https://", global::System.StringComparison.OrdinalIgnoreCase)) {
        // The TLS 1.2 numeric value is usable with .NET 4 reference assemblies;
        // its named enum member was added later. Never fall back to SSL/TLS 1.0.
        try {
          global::System.Net.ServicePointManager.SecurityProtocol =
            (global::System.Net.SecurityProtocolType)3072;
        } catch (global::System.Exception tlsError) {
          throw new global::System.Exception(
            "HTTPS requires TLS 1.2 support in your installed .NET/Windows security updates.", tlsError);
        }
      }
      var request = (global::System.Net.HttpWebRequest)global::System.Net.WebRequest.Create(url);
      request.Method = "GET";
      request.Timeout = timeoutMs;
      request.ReadWriteTimeout = timeoutMs;
      request.AllowAutoRedirect = false;
      request.UserAgent = "Zsd/1.0 (.NET Framework 4.0)";
      using (var response = (global::System.Net.HttpWebResponse)request.GetResponse()) {
        const int MaxResponseBytes = 16384;
        if (response.ContentLength > MaxResponseBytes)
          throw new global::System.Exception("HTTP response exceeds 16 KiB bound");
        using (var stream = response.GetResponseStream())
        using (var collected = new global::System.IO.MemoryStream()) {
          var buffer = new byte[1024];
          int total = 0;
          int count;
          while ((count = stream.Read(buffer, 0, buffer.Length)) > 0) {
            if (count > MaxResponseBytes - total)
              throw new global::System.Exception("HTTP response exceeds 16 KiB bound");
            collected.Write(buffer, 0, count);
            total += count;
          }
          return global::System.Text.Encoding.UTF8.GetString(collected.GetBuffer(), 0, total);
        }
      }
    } catch (global::System.Net.WebException e) {
      var code = e.Response == null ? 0 : (int)((global::System.Net.HttpWebResponse)e.Response).StatusCode;
      if (e.Response != null) e.Response.Close();
      throw new global::System.Exception("HTTP status " + code + ": " + e.Message, e);
    } catch (global::System.Exception e) {
      throw new global::System.Exception("HTTP failure: " + e.Message, e);
    }
  ')
  public static function get(url:String, timeoutMs:Int):String return null;
}
#end