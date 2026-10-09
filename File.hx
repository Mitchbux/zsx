@:native("File")
extern class File extends Blob {
	
	/**
		Returns the name of the file referenced by the `File` object.
	**/
	var name(default,null) : String;
	
	/**
		Returns the last modified time of the file, in millisecond since the UNIX epoch (January 1st, 1970 at Midnight).
	**/
	var lastModified(default,null) : Int;
	
	/** @throws DOMError */
	function new( fileBits : Array<haxe.extern.EitherType<haxe.extern.EitherType<js.lib.ArrayBufferView,js.lib.ArrayBuffer>,haxe.extern.EitherType<Blob,String>>>, fileName : String, ?options : Any ) : Void;
}