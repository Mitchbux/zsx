import BitWriter;
import Shrink;
import Merge;

class Node
{
	public static var sx_bits:Int = 8;
	public static var sx_values:Int = 256;
	public static var sx_win:Int = 256*1024;

	public var value:UInt;
	public var data:Array<Node> ;
	
	public function new()
	{
		data = [];
		value = 0;
	}
	
	public function get(list:Array<UInt>):Node
	{
		var index:UInt = list.shift();
		var destination = data[index];
		
		if(destination == null)
			return null;
		
		if(list.length == 0)
		{
			return destination;
		}else
		{
			return destination.get(list);
		}
	}
	
	public function set(list:Array<UInt>, number:UInt):Node
	{
		var index:UInt = list.shift();
		var destination = data[index];
		
		if(destination == null)
		{
			destination = new Node();
			data[index] = destination;
		}	
		
		if(list.length == 0)
		{

			destination.value = number;
			return destination;
		}else
		{
			return destination.set(list, number);
		}
	}
	
	public function dump(writer:BitWriter, last:UInt)
	{
		var z:Int, s:Int, x:Int;
		var cz:Array<Bool> = new Array<Bool>();
		var cs:Array<Int> = new Array<Int>();
		var cx:Array<Int> = new Array<Int>();
		var zsx:Array<Int> = new Array<Int>();
		var cs_counter:Int, cx_counter:Int;
		var zsx_counter:Int;
		
		
		for (z in 0...sx_values)
		{
			var znode:Node = data[z];
			writer.writeBit(znode != null ?1:0);
			if (znode != null)
			{
				cz.push(true);
				for (s in 0...sx_values)
				{	
					var snode:Node = znode.data[s];
					if (snode != null)
					{
						writer.writeBit(1);
						cs.push(s>>4);
						for (x in 0...sx_values)
						{
							var xnode:Node = snode.data[x];
							if (xnode != null)
							{
								writer.writeBit(1);
								cx.push(x>>4);
							}
						}
						writer.writeBit(0);
					}
				}
				writer.writeBit(0);
			}else
				cz.push(false);
		}
		
		var counter:Int = 0;
		var kounter:Int = 0;
		var cc:Array<Int> = new Array<Int>();
		var ck:Array<Int> = new Array<Int>();

		cs_counter = 0;
		cx_counter = 0;
		zsx_counter = 0;
		
		for (z in 0...sx_values) if (cz[z])
		{
			var znode:Node = data[z];
			for (s in 0...sx_values) if (cs[cs_counter]==(s>>4))
			{
				counter++;
				var snode:Node = znode.data[s];
				if (snode != null)
				{
					cc[cs_counter++] = counter - 1;
					counter = 0;
					for (x in 0...sx_values) if (cx[cx_counter]==(x>>4))
					{
						kounter++;
						var xnode:Node = snode.data[x];
						if (xnode != null)
						{
							ck[cx_counter++] = kounter - 1;
							zsx[xnode.value - 1] = zsx_counter++;
							kounter = 0;							
						}
					}
				}
			}
		}
		
		for(s in 0...cs.length)
			writer.writeValue(cc[s], 4);
			
		for(s in 0...cx.length)
			writer.writeValue(ck[s], 4);
		
		Merge.Sort(writer, zsx);
		
	}
}