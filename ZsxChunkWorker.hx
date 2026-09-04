import haxe.io.Bytes;
import js.lib.ArrayBuffer;

/**
 * One range-coder worker. The coordinator keeps these alive and assigns one
 * independent 12 MiB chunk at a time.
 */
class ZsxChunkWorker {
  static function main():Void {
    var scope:Dynamic = js.Syntax.code("self");
    scope.onmessage = function(event:Dynamic):Void {
      try {
        var index:Int = event.data.index;
        var input = Bytes.ofData(cast(event.data.buffer, ArrayBuffer));
        var output = ZsxRange.encodeChunk(input);
        var data = output.getData();
        var response = {index: index, bytes: data, inputLength: input.length};
        untyped scope.postMessage(response, [data]);
      } catch (error:Dynamic) {
        scope.postMessage({type: "error", message: Std.string(error)});
      }
    };
  }
}