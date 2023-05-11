import BitWriter;
import BitReader;

class Merge
{
	public static function Sort(w:BitWriter, a:Array<Int>):Array<Int>
	{
		if(a.length < 2) return a;
		else
		{
			var middle:UInt = Std.int(a.length/2);
			
			var left:Array<Int> = new Array<Int>();
			var right:Array<Int> = new Array<Int>();
			var i:UInt, j:UInt = 0, k:UInt = 0;
			
			for(i in 0...middle)
				left[j++]=a[i];

			for(i in middle...a.length)
				right[k++]=a[i];

			left = Sort(w, left);
			right = Sort(w, right);
			
			a = Merge(w, left, right);
			return a;
		}
	}

	public static function Merge(w:BitWriter, left:Array<Int>, right:Array<Int>):Array<Int>
	{
		var result:Array<Int> = new Array<Int>();
		var j:UInt = 0, k:UInt = 0, m:UInt = 0;

		while(j < left.length && k < right.length)
		{
			if(left[j] <= right[k])
			{
				w.writeBit(1);
				result[m++] = left[j++];
			}
			else
			{
				w.writeBit(0);
				result[m++] = right[k++];
			}
		}

		for(j in j...left.length)
			result[m++] = left[j];
		for(k in k...right.length)
			result[m++] = right[k];
		return result;
	}

	public static function SortRead(r:BitReader, a:Array<Int>):Array<Int>
	{
		if(a.length < 2) return a;
		else
		{
			var middle:UInt = Std.int(a.length/2);
			
			var left:Array<Int> = new Array<Int>();
			var right:Array<Int> = new Array<Int>();
			var i:UInt, j:UInt = 0, k:UInt = 0;
			
			for(i in 0...middle)
				left[j++]=a[i];

			for(i in middle...a.length)
				right[k++]=a[i];

			left = SortRead(r, left);
			right = SortRead(r ,right);
			
			a = MergeRead(r, left, right);
			return a;
		}
	}

	public static function MergeRead(r:BitReader, left:Array<Int>, right:Array<Int>):Array<Int>
	{
		var result:Array<Int> = new Array<Int>();
		var j:UInt = 0, k:UInt = 0, m:UInt = 0;

		while(j < left.length && k < right.length)
		{
			if(r.readBit()==1)
				result[m++] = left[j++];
			else
				result[m++] = right[k++];
		}

		for(j in j...left.length)
			result[m++] = left[j];
		for(k in k...right.length)
			result[m++] = right[k];
		return result;
	}
}
