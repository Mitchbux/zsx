import haxe.io.Bytes;
import js.lib.ArrayBuffer;

class ZsxWorker {
  static function main():Void {
    var scope:Dynamic = js.Syntax.code("self");
    scope.onmessage = function(event:Dynamic):Void {
      handle(scope, event.data);
    };
  }

  static function handle(scope:Dynamic, request:Dynamic):Void {
    try {
      if (request == null || request.buffer == null)
        throw new haxe.Exception("Missing input data");

      var input = Bytes.ofData(cast(request.buffer, ArrayBuffer));
      var mode:String = request.mode;
      var progress = function(done:Int, total:Int):Void {
        var percent = total == 0 ? 100 : Math.floor(done * 100 / total);
        scope.postMessage({type: "progress", value: percent});
      };

      switch (mode) {
        case "decode": postDone(scope, mode, input.length, ZsxArchive.decode(input, progress));
        default: throw new haxe.Exception("Unsupported worker operation");
      }
    } catch (error:Dynamic) {
      scope.postMessage({type: "error", message: Std.string(error)});
    }
  }

  static function postDone(scope:Dynamic, mode:String, inputLength:Int, output:Bytes):Void {
    var data = output.getData();
    var response = {
      type: "done",
      mode: mode,
      bytes: data,
      inputLength: inputLength,
      outputLength: output.length
    };
    untyped scope.postMessage(response, [data]);
  }
}