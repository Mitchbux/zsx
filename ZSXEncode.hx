import Shrink;
import haxe.io.Bytes;
class ZSXEncode
{	
  static function main() {
		var chunk:Bytes = Bytes.alloc(1024);
		Shrink.encode(chunk);
		ZsxRange.encodeChunk(Bytes.alloc(0));
		ZsxArchive.encode(Bytes.alloc(0));
	}
}