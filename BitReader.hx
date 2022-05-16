import haxe.io.Bytes;

class BitReader
{
  var power:Int;
  var start:Int;
  var bits:Int;
  var length:Int;
  var data:Bytes;

  public function new (fromData :Bytes)
  {
    data=fromData;
    length=data.length;
    start=1;
    bits=data.get(0);
    power=0;
  }
  public function setLength(l:Int)
  {
     length=l;
  }
  public function canRead():Bool
  {
     return start<=length;
  }

  public function readBit() 
  {
    var v:Int = (bits>>(8-(++power))&1);
    if(power>=8)
    {
        power=0;
        if(canRead())
        bits=data.get(start++);
        else
        bits=0;
    }
    return v;
  }
  public function readValue(b:Int):Int
  {
    var v:Int=0;
    while(b>0)
    {
      b--;
      v=v<<1;
      v+=readBit();
    }
    return v;
  }
}

