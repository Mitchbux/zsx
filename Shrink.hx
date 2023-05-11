import haxe.io.Bytes;
import BitWriter;
import BitReader;
import Node;

class Shrink
{

	public static function lf(v:Int):Int
	{
		var m=1;
		while((1<<m)<v)m++;
		return m;
	}
	
	public static function encode(inData:Bytes,outData:Bytes):Bytes
	{
		Sys.print(":");
		var reader = new BitReader(inData);
		var writer = new BitWriter(outData);

		var root = new Node();
		writer.writeValue(inData.length,32);
		
		var last = 0;
		while(reader.canRead())
		{
			var list:Array<UInt> = [reader.readValue(Node.sx_bits),
									reader.readValue(Node.sx_bits),
									reader.readValue(Node.sx_bits)];
									
			var got = root.get(list.copy());
			writer.writeBit(got != null ? 1 : 0);
			if(got!=null)
			{
				writer.writeValue(got.value - 1, lf(last));
			}else
			{
				root.set(list.copy(), ++last);
			}
			
			if(last == Node.sx_win)
			{
				root.dump(writer, last);
				root = new Node();
				last = 0;
			}
			
		}
		
		root.dump(writer, last);
	
		return writer.toBytes();
	}
	
	
	
	public static function decode(inData:Bytes,outData:Bytes):Bytes
	{
		var reader = new BitReader(inData);
		var writer = new BitWriter(outData);
		var size:Int = reader.readValue(32);
		var z:Int, s:Int, x:Int;
		var cz:Array<Bool> = new Array<Bool>();
		var cs:Array<Int> = new Array<Int>();
		var cx:Array<Int> = new Array<Int>();
		var zsx:Array<Int> = new Array<Int>();
		var cs_counter:Int = 0, cx_counter:Int = 0;
		
		var codes:Array<Int> = new Array<Int>();
		
		var decoded:Int=0;
		var last:Int = 0;
		
		while(decoded < size && reader.canRead())
		{
			if(reader.readBit()==1)
			{
				codes.push(reader.readValue(lf(last)));
			}else
			{
				codes.push(last++);
			}
			
			if(last==Node.sx_win || decoded + codes.length*3 >= size)
			{
				for (z in 0...Node.sx_values)
				{
					if (reader.readBit()==1)
					{
						cz.push(true);
						while (reader.readBit()==1)
						{
							cs.push(reader.readValue(4));
							while(reader.readBit()==1)
							{
								cx.push(reader.readValue(4));
							}
						}
					}else
						cz.push(false);
				}
			}
			
			
			var cc:Array<Int> = new Array<Int>();
			var ck:Array<Int> = new Array<Int>();

			for(s in 0...cs.length)
			{
				cc.push(reader.readValue(4));
			}
			
			for(s in 0...cx.length)
			{
				ck.push(reader.readValue(4));
			}
			
			var counter:Int = 0;
			var kounter:Int = 0;
			var dico:Array<Array<Int>> = new Array<Array<Int>>();
			
			for (z in 0...Node.sx_values) if (cz[z])
			{
				for (s in 0...Node.sx_values) if (cs[cs_counter]==(s>>4))
				{
					if (counter == cc[cs_counter])
					{
						cs_counter++;						
						counter = 0;
						for (x in 0...Node.sx_values) if (cx[cx_counter]==(x>>4))
						{							
							if (kounter == ck[cx_counter])
							{
								dico.push([z,s,x]);
								kounter = 0;
								cx_counter++;
							}
							kounter++;							
						}
					}
					counter++;
				}
			}
			
			
		}
		


		return writer.toBytes();
   }
}
