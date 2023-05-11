import haxe.io.Bytes;
import sys.io.File;
import Shrink;
import sys.FileSystem;
import haxe.zip.Compress;
class Zsx {
  static function main() {
   Sys.println("Encoding...");
 var filename=Sys.args()[0];
 var chk=32*1024*1024;
 var fst=FileSystem.stat(filename);
 var dta=File.getBytes(filename);
 var out=Bytes.alloc(chk);
 var dec=Bytes.alloc(chk);
 var readsize=0;
 var a = File.write(filename +".zsx",true);

 while(readsize<fst.size)
 {
  var chunk:Bytes;

 if(readsize+chk<fst.size)
 chunk =dta.sub(readsize,chk);
 else
 chunk =dta.sub(readsize,fst.size-readsize);
 readsize+=chunk.length; 
 var prevlen = chunk.length;
 //var zipped = Compress.run(chunk,9);
 var encoded = Shrink.encode(chunk,out);
 
 while(encoded.length<prevlen)
 {
   prevlen=encoded.length;
   encoded = Shrink.encode(encoded,out);
 }

 Sys.println(Std.int((readsize/fst.size*100.0))+"%");

//var decoded = Shrink.decode(encoded,dec);

  a.writeBytes(encoded,0,encoded.length);
 // a.writeBytes(encoded_ecc,0,encoded_ecc.length);

 }
 
    a.flush();
    a.close();  
    Sys.println("Encoded to "+filename+".zsx");
  }
}

