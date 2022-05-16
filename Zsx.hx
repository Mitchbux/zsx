import haxe.io.Bytes;
import sys.io.File;
import Shrink;
import sys.FileSystem;
import haxe.zip.Compress;
class Zsx {
  static function main() {
   
 var filename=Sys.args()[0];
 var chk=10*1000*1000;
 var fst=FileSystem.stat(filename);
 var dta=File.getBytes(filename);
 var out=Bytes.alloc(chk*2);
 var rout=Bytes.alloc(chk*2);
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
 Sys.println((readsize/fst.size*100.0)+"%");

 var prevlen = chunk.length;
 var encoded = Shrink.encode(chunk,out);
 var zipped = Compress.run(encoded,9);

// while(zipped.length<prevlen)
// {
//   prevlen=zipped.length;
//   encoded = Shrink.encode(zipped,out);
//   zipped = Compress.run(encoded,9);  
// }

var decoded = Shrink.decode(encoded,rout);

  a.writeBytes(decoded,0,decoded.length);

 }
 
    a.flush();
    a.close();  
    Sys.println("encoded to "+filename+".zsx");
  }
}

