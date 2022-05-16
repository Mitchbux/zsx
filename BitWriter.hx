import haxe.io.Bytes;

class BitWriter
{
  var power:Int;
  var bits:Int;
  var start:Int;
  var data:Bytes;

  public function new(fromData:Bytes) 
  {
    data = fromData;
    start=0;
    bits=0;
    power=0;
  }
  
  public function writeBit(v:Int)
  {
    bits+=(v&1)<<(8-(++power));
  if(power>=8)
  {
     data.set(start++,bits);
     power=0;
     bits=0;
  }
  }
  public function writeValue(v:Int, b:Int)
  {
      while(b>0)
      {
        b--;
        writeBit((v>>b)&1);
      }
  }
  public function flushWrite() 
  {
    while(power>0)
      writeBit(0);
  }

  public function toString() 
  {
    return data.toString();
  }

  public function toBytes()
  {
   if(start<data.length)
    return data.sub(0,start);
   else return data;
  }

}

