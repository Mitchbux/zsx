import haxe.io.Bytes;
import BitWriter;
import BitReader;

class Shrink
{
       public static var sx_bits:Int = 8;
       public static var sx_values:Int = 256;
       public static var sx_top:Int = 256;

   public static function lf(v:Int):Int
{
   var m=1;
   while((1<<m)<v)m++;
   return m;
}
   public static function encode(inData:Bytes,outData:Bytes,ecc:Bool):Bytes
   {
        Sys.println("input size :"+inData.length);
	//Sys.print(".");
        var reader = new BitReader(inData);
        var writer = new BitWriter(outData);

        var list = new Array<UInt>();
        var code = new Array<Int>();
        var solo = new Array<UInt>();

        writer.writeValue(inData.length,32);
        var x:Int,s:Int,k:Int,n:Int;
        for(s in 0...sx_values)
        {
          code.push(0);
          solo.push(0);
        }
        var items=0; 
        while(reader.canRead())
        {
         x = reader.readValue(sx_bits);
         list.push(x);
         items++;
         code[x]++;
        }
       var index=0;
        while(items>0)
        {
         var new_len=items;
        for(s in 0...sx_values)solo[s]=0;
        for(s in 1...sx_top) {
           var max=code[0];var index=0;
          for(k in 0...sx_values)
           if (code[k]>max)
           {
              max=code[k];
              index=k;
           }
          code[index]=-1;
          solo[index]=s;
          writer.writeValue(index,sx_bits);
        }
        
        for(s in 0...sx_values)code[s]=0;
        items=0;

        k=0;s=0;
        var sorted =new Array<Int>();
        for(s in 0...sx_top)sorted.push(0);
        var coded=0;var last=0;
        while(k<new_len)
        {         
           
           x = list[k++];
           
         s=solo[x];

writer.writeBit(s%5==0?1:0);
if(s%5==0)
{

list[items++]=x;
code[x]++;
}else
{

s=Math.floor(s/5);
while(s-->0)writer.writeBit(1);
writer.writeBit(0);
writer.writeValue(s%5-1,2);
}

        }index++;
        }
        return writer.toBytes();
   }
   public static function decode(inData:Bytes,outData:Bytes):Bytes
   {
      var reader = new BitReader(inData);
      var writer = new BitWriter(outData);
        var code = new Array<Array<Int>>();
        var solo = new Array<UInt>();
        var size:Int = reader.readValue(32);
        var count = new Array<Int>();
        var x:Int,s:Int,k:Int,n:Int;
        for(s in 0...sx_values)
        {
          solo.push(0);
        }
       var new_size:Int; 
     var items:Int=size;
     var index:Int=0;
       while(items>0&&reader.canRead())
       {
      for(s in 1...sx_top)
        solo[s]=reader.readValue(sx_bits);
      count.push(items);
      new_size=items;
      items=0;
      k=0;
      code.push(new Array<Int>());
    
      while(k<new_size&&reader.canRead())
      {
        s=reader.readValue(lf(sx_top));
      
       
            if(s==0){
             code[index].push(-1);
         items++;
            }else code[index].push(solo[s]);
          
       
       k++;
      }
      // trace(items);
        index++;
      }
      while(index>0)
      {
        index--;
        k=0;
        for(s in 0...count[index])
        if(code[index][s]==(-1))
         code[index][s]=code[index+1][k++];

      }

      for(s in 0...count[0])
      writer.writeValue(code[0][s],sx_bits);
      return writer.toBytes();
   }
}
