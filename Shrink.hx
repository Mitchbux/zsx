import haxe.io.Bytes;
import BitWriter;
import BitReader;

class Shrink
{
       public static var sx_bits:Int = 8;
       public static var sx_values:Int = 256;
       public static var sx_top:Int = 10;

   public static function lf(v:Int):Int
{
   var m=1;
   while((1<<m)<v)m++;
   return m;
}
   public static function encode(inData:Bytes,outData:Bytes):Bytes
   {
       // Sys.println("input size :"+inData.length);
	var reader = new BitReader(inData);
        var writer = new BitWriter(outData);

        var list = new Array<UInt>();
        var code = new Array<Int>();
        var solo = new Array<UInt>();

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
         var sort=new Array<Int>();
        for(k in 0...sx_top)sort.push(0);
        k=0;
        while(k<new_len)
        {
           
         
     var last=0;
         for(n in 0...sx_top)sort[n]=0;
         while(k<new_len&&last<sx_top)
         {
           var q=0;
           x = list[k++];
           s=0;
           while(s<solo[x]){
           if(sort[s]>0)q++;
           s++;
           }
           writer.writeBit(sort[x]==0?1:0);
           writer.writeValue(q,lf(last));
           //writer.writeValue(solo[x],lf(sx_top));
           if(sort[x]==0)last++;
           sort[x]++;
           
         if(solo[x]==0)
         {
           list[items++]=x;
           code[x]++;
         }}


         
           s=0;
           while(s<sx_top)
           {
           writer.writeBit(sort[s++]>0?1:0);
           }
        
        }
        }
        return writer.toBytes();
   }
   public static function decode(inData:Bytes,outData:Bytes):Bytes
   {
      var reader = new BitReader(inData);
      var writer = new BitWriter(outData);
        var code = new Array<Int>();
        var solo = new Array<UInt>();

        var x:Int,s:Int,k:Int,n:Int;
        for(s in 0...sx_values)
        {
          code.push(0);
          solo.push(0);
        }

      for(s in 1...sx_top)
        solo[s]=reader.readValue(sx_bits);
      
      return writer.toBytes();
   }
}
