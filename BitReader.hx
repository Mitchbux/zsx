import haxe.io.Bytes;

class BitReader
{
  var power:Int;
  var start:Int;
  var bits:Int;
  var length:Int;
  var hasByte:Bool;
  public var data:Bytes;

  public function new (fromData :Bytes)
  {
    data=fromData;
    length=data.length;
    start=0;
    hasByte=length>0;
    bits=hasByte ? data.get(start++) : 0;
    power=0;
  }
  public function setLength(l:Int)
  {
     length=l;
  }
  public function canRead():Bool
  {
      return hasByte;
   }
   public function canReadBits(count:Int):Bool
   {
      return remainingBits()>=count;
   }
   public function remainingBits():Int
   {
      if(!hasByte)return 0;
      return (8-power)+((length-start)<<3);
  }

  public function readBit():Int
  {
    var v:Int = (bits>>(8-(++power))&1);
    if(power>=8)
    {
        power=0;
         if(start<length)
        bits=data.get(start++);
        else
         {
        bits=0;
         hasByte=false;
         }
    }
    return v;
  }
  public function readBool():Bool
  {
	var v:Int = readBit();
	return v==1;
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

