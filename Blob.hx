
extern class ArrayBufferPromise
{
	function then(fn:js.lib.ArrayBuffer->Void) : js.lib.ArrayBuffer;
}

@:native("Blob")
extern class Blob
{

	/**
		The size, in bytes, of the data contained in the `Blob` object.
	**/
	var size(default,null) : Int;

	/**
		A string indicating the MIME type of the data contained in the `Blob`. If the type is unknown, this string is empty.
	**/
	var type(default,null) : String;

	/** @throws DOMError */
	function new( ?blobParts : Array<haxe.extern.EitherType<js.lib.ArrayBuffer,haxe.extern.EitherType<js.lib.ArrayBufferView,haxe.extern.EitherType<Blob,String>>>>, ?options : Any ) : Void;

	/**
		Returns a new `Blob` object containing the data in the specified range of bytes of the source `Blob`.
		@throws DOMError
	**/
	function slice( ?start : Int, ?end : Int, ?contentType : String ) : Blob;

	@:native("arrayBuffer")
	function arrayBuffer() : ArrayBufferPromise;
}
