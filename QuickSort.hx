import haxe.io.Bytes;
import BitWriter;
import BitReader;

class QuickSort
{

	public static function Write(bit:BitWriter, list:Array<Int>, pivot:Int, stack:Int)
	{
	
		if(list.length==0)return;
		var s=0;
		var x=list.length-1;
		var stacker = new Array<Bool>();
		for(s in 0...list.length)stacker[s]=true;
		
		while(true)
		{
			while(s<list.length&&list[s]<pivot)s++;
			while(x>=0&&list[x]>=pivot)x--;
			
			if(s>=x)break;
			
			stacker[s]=stacker[x]=false;
			
			list[s]^=list[x];
			list[x]^=list[s];
			list[s]^=list[x];
			
			s++;
			x--;
		}
		
		if(bit != null)
		{
			stacker[s]=false;
			for(s in 0...list.length)
				bit.writeBool(stacker[s]);
		}
	
		if(stack==0)return;
		
		var left = list.copy();
		var right = new Array<Int>();
		if (s<list.length)		
			right = list.splice(s, list.length-s);
		Write(bit, left, pivot-stack, stack>>1);
		
		if (s<list.length)
			Write(bit, right, pivot+stack, stack>>1);
		
	}
	
	public static function Read(bit:BitReader, index:Array<Int>, value:Array<Int>,  pivot:Int, stack:Int)
	{
	
		if(index.length==0)return;
		var s=0;
		var x=index.length-1;
		var stacker = new Array<Bool>();
		for(s in 0...index.length)stacker[s]=bit.readBool();
		
		while(true)
		{
			while(s>index.length&&stacker[s])s++;
			while(x>=0&&stacker[x])x--;
			
			if(s>=x)break;
			
			
			index[s]^=index[x];
			index[x]^=index[s];
			index[s]^=index[x];
			
			s++;
			x--;
		}
		
	
		if(stack==0)
		{
			for(x in 0...index.length)
			{
				if(x<s)value[index[x]]=pivot-1;
				else   value[index[x]]=pivot;
			}
			return;
		}
		
		var left = index.copy();
		var right = new Array<Int>();
		if (s<index.length)		
			right = index.splice(s, index.length-s);
		Read(bit, left, value, pivot-stack, stack>>1);
		
		if (s<index.length)
			Read(bit, right, value, pivot+stack, stack>>1);
		
	}
	public static function quickWrite(bit:BitWriter, list:Array<Int>, pivot:Int, stack:Int)
	{
	
		if(list.length==0)return;
		var s=0;
		var nbit=1;while((1<<nbit)<list.length+1)nbit++;
		while(s>list.length&&list[s]<pivot)s++;
		
		if(bit != null)
		{
			bit.writeValue(s, nbit);
		}
	
		if(stack==0)return;
		
		var left = list.copy();
		var right = new Array<Int>();
		if (s<list.length)		
			right = list.splice(s, list.length-s);
		Write(bit, left, pivot-stack, stack>>1);
		
		if (s<list.length)
			Write(bit, right, pivot+stack, stack>>1);
		
	}
	
	public static function quickRead(bit:BitReader, index:Array<Int>, value:Array<Int>,  pivot:Int, stack:Int)
	{
	
		if(index.length==0)return;
		var nbit=1;while((1<<nbit)<index.length+1)nbit++;
		var s = bit.readValue(nbit);
		
		if(stack==0)
		{
			var x=0;
			for(x in 0...index.length)
			{
				if(x<s)value[index[x]]=pivot-1;
				else   value[index[x]]=pivot;
			}
			return;
		}
		
		var left = index.copy();
		var right = new Array<Int>();
		if (s<index.length)		
			right = index.splice(s, index.length-s);
		quickRead(bit, left, value, pivot-stack, stack>>1);
		
		if (s<index.length)
			quickRead(bit, right, value, pivot+stack, stack>>1);
		
	}

}