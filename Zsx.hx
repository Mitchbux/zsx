import haxe.io.Bytes;
import sys.io.File;
import Shrink;
import sys.FileSystem;
import haxe.zip.Compress;
class Zsx {
  static function main() {
   
 var filename=Sys.args()[0];
 var chk=20*1000*1000;
 var fst=FileSystem.stat(filename);
 var dta=File.getBytes(filename);
 var out=Bytes.alloc(chk*2);
 var dec=Bytes.alloc(chk*2);
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
 Sys.println("="+(5<<3));
 var prevlen = chunk.length*4;
 var encoded = Shrink.encode(chunk,out,false);
 
 while(encoded.length<prevlen)
 {
   prevlen=encoded.length;
   encoded = Shrink.encode(encoded,out,false);
 }


//var decoded = Shrink.decode(encoded,dec);

  a.writeBytes(encoded,0,encoded.length);
 // a.writeBytes(encoded_ecc,0,encoded_ecc.length);

 }
 
    a.flush();
    a.close();  
    Sys.println("encoded to "+filename+".zsx");
  }
}

