import js.Browser;
import js.lib.ArrayBuffer;
import haxe.io.Bytes;
import js.html.Worker;

class ZsxJs {

  static function main() {
                var btn = Browser.document.createButtonElement();
                btn.innerText = "Compress";
                btn.className = "button is-link";
                btn.onclick = Go;
                btn.style.marginRight = "10px";
                Browser.document.getElementById("go").appendChild(btn);
                var dec = Browser.document.createButtonElement();
                dec.innerText = "Decompress";
                dec.className = "button is-success";
                dec.onclick = Dego;
                Browser.document.getElementById("go").appendChild(dec);
        }
        
        static function Go(event)
        {
                 var upload = cast (Browser.document.getElementById("FileToCompress"), js.html.InputElement);
                 if (upload.files.length>0)
                        Encode(cast(upload.files[0], File));
        }
        
        static function Dego(event)
        {
                 var upload = cast (Browser.document.getElementById("FileToCompress"), js.html.InputElement);
                 if (upload.files.length>0)
                        Decode(cast(upload.files[0], File));
        }

        static function DownloadBytes(name:String, encoded:Bytes)
        {
                DownloadBlob(name, new js.html.Blob([encoded.getData()], {type: "application/octet-stream"}));
        }

        static function DownloadBlob(name:String, b:js.html.Blob)
        {
                var link = cast(Browser.document.createElement('a') , js.html.AnchorElement);
                var url = js.html.URL.createObjectURL(b);
                link.href = url;
                link.download = name;
                link.style.display = "none";
                Browser.document.body.appendChild(link);
                link.click();
                link.remove();
                Browser.window.setTimeout(function() js.html.URL.revokeObjectURL(url), 0);
        }
                
        static function Decode(data:File)
        {
                runWorker("decode", data, decodedName(data.name));
        }
        
        static function Encode(data:File)
        {
                encodeParallel(data);
        }

        static function runWorker(mode:String, data:File, outputName:String):Void
        {
                setBusy(true, "Reading archive…");
                data.arrayBuffer().then((value:ArrayBuffer) -> {
                        var worker = new Worker("zsx_worker.js");
                        worker.onmessage = function(event:js.html.MessageEvent) {
                                var message:Dynamic = event.data;
                                switch (message.type) {
                                        case "progress":
                                                setProgress(message.value, "Decompressing…");
                                        case "done":
                                                var output = Bytes.ofData(cast message.bytes);
                                                DownloadBytes(outputName, output);
                                                var inputLength:Int = message.inputLength;
                                                var outputLength:Int = message.outputLength;
                                                var summary = "Done: " + formatBytes(outputLength) + " restored";
                                                setProgress(100, summary);
                                                setBusy(false, summary);
                                                worker.terminate();
                                        case "error":
                                                setProgress(0, "Error: " + message.message);
                                                setBusy(false, "Error: " + message.message);
                                                worker.terminate();
                                        default:
                                }
                        };
                        worker.onerror = function(event:js.html.ErrorEvent) {
                                setProgress(0, "Worker error: " + event.message);
                                setBusy(false, "Worker error: " + event.message);
                                worker.terminate();
                        };
                        untyped worker.postMessage({mode: mode, buffer: value}, [value]);
                });
        }

        static function encodeParallel(data:File):Void
        {
                var total:Int = data.size;
                var navigator:Dynamic = Browser.navigator;
                var hardware:Int = navigator.hardwareConcurrency == null ? 2 : navigator.hardwareConcurrency;
                var memory:Float = navigator.deviceMemory == null ? 0 : navigator.deviceMemory;
                var memoryLimit:Int;
                var browserLimit:Int;
                if (memory >= 8) {
                        memoryLimit = 4;
                        browserLimit = 0x8000000;
                } else if (memory >= 4) {
                        memoryLimit = 2;
                        browserLimit = 0x4000000;
                } else {
                        memoryLimit = 1;
                        browserLimit = 0x2000000;
                }
                if (total > browserLimit) {
                        var limitMiB = Std.int(browserLimit / 0x100000);
                        setProgress(0, "Error: file exceeds this device's " + limitMiB + " MiB safe compression limit");
                        return;
                }

                var chunkCount = total == 0 ? 0 : Std.int(Math.ceil(total / ZsxArchive.CHUNK_SIZE));
                if (chunkCount == 0) {
                        var header = archiveHeader(0, 0);
                        DownloadBlob(data.name + ".zsx", new js.html.Blob([header.getData()], {type: "application/octet-stream"}));
                        setProgress(100, "Done: 0 B → 16 B");
                        return;
                }

                var poolSize = hardware;
                if (poolSize > memoryLimit) poolSize = memoryLimit;
                if (poolSize > chunkCount) poolSize = chunkCount;
                if (poolSize < 1) poolSize = 1;

                setBusy(true, poolSize > 1
                  ? "Compressing with " + poolSize + " workers…"
                  : "Compressing…");

                var chunks = new Array<ArrayBuffer>();
                var chunkLengths = new Array<Int>();
                chunks.resize(chunkCount);
                chunkLengths.resize(chunkCount);
                var workers = new Array<Worker>();
                var nextChunk = 0;
                var completed = 0;
                var completedBytes = 0;
                var stopped = false;

                var fail = function(message:String):Void {
                        if (stopped) return;
                        stopped = true;
                        for (worker in workers) worker.terminate();
                        setProgress(0, "Error: " + message);
                        setBusy(false, "Error: " + message);
                };

                var dispatch:Dynamic = null;
                dispatch = function(worker:Worker):Void {
                        if (stopped || nextChunk >= chunkCount) return;
                        var index = nextChunk++;
                        var offset = index * ZsxArchive.CHUNK_SIZE;
                        var length = total - offset;
                        if (length > ZsxArchive.CHUNK_SIZE) length = ZsxArchive.CHUNK_SIZE;
                        data.slice(offset, offset + length).arrayBuffer().then((value:ArrayBuffer) -> {
                                if (!stopped)
                                        untyped worker.postMessage({index: index, buffer: value}, [value]);
                        });
                };

                for (_ in 0...poolSize) {
                        var worker = new Worker("zsx_chunk_worker.js");
                        workers.push(worker);
                        worker.onmessage = function(event:js.html.MessageEvent):Void {
                                if (stopped) return;
                                var message:Dynamic = event.data;
                                if (message.type == "error") {
                                        fail(message.message);
                                        return;
                                }

                                var index:Int = message.index;
                                if (index < 0 || index >= chunkCount || chunks[index] != null) {
                                        fail("Invalid chunk worker response");
                                        return;
                                }
                                chunks[index] = cast message.bytes;
                                chunkLengths[index] = message.bytes.byteLength;
                                completed++;
                                completedBytes += message.inputLength;
                                setProgress(Math.floor(completedBytes * 100 / total),
                                  poolSize > 1 ? "Compressing with " + poolSize + " workers…" : "Compressing…");

                                if (completed == chunkCount) {
                                        stopped = true;
                                        for (active in workers) active.terminate();
                                        var parts = new Array<Dynamic>();
                                        parts.push(archiveHeader(total, chunkCount).getData());
                                        var outputLength = 16;
                                        for (i in 0...chunkCount) {
                                                var lengthBytes = Bytes.alloc(4);
                                                writeU32(lengthBytes, 0, chunkLengths[i]);
                                                parts.push(lengthBytes.getData());
                                                parts.push(chunks[i]);
                                                outputLength += 4 + chunkLengths[i];
                                        }
                                        DownloadBlob(data.name + ".zsx",
                                          new js.html.Blob(cast parts, {type: "application/octet-stream"}));
                                        var summary = "Done: " + formatBytes(total) + " → " + formatBytes(outputLength);
                                        setProgress(100, summary);
                                        setBusy(false, summary);
                                } else {
                                        dispatch(worker);
                                }
                        };
                        worker.onerror = function(event:js.html.ErrorEvent):Void {
                                fail(event.message == null ? "Chunk worker failed" : event.message);
                        };
                        dispatch(worker);
                }
        }

        static function archiveHeader(originalLength:Int, chunkCount:Int):Bytes
        {
                var header = Bytes.alloc(16);
                header.set(0, "Z".code);
                header.set(1, "S".code);
                header.set(2, "X".code);
                header.set(3, "1".code);
                writeU32(header, 4, originalLength);
                writeU32(header, 8, chunkCount);
                writeU32(header, 12, 0);
                return header;
        }

        static inline function writeU32(bytes:Bytes, at:Int, value:Int):Void
        {
                bytes.set(at, value);
                bytes.set(at + 1, value >>> 8);
                bytes.set(at + 2, value >>> 16);
                bytes.set(at + 3, value >>> 24);
        }

        static function decodedName(name:String):String
        {
                return StringTools.endsWith(name.toLowerCase(), ".zsx")
                  ? name.substr(0, name.length - 4)
                  : name + ".decoded";
        }

        static function setProgress(value:Int, status:String):Void
        {
                var progress = cast(Browser.document.getElementById("progress"), js.html.ProgressElement);
                progress.value = value;
                var text = Browser.document.getElementById("statusText");
                if (text != null) text.textContent = status;
        }

        static function setBusy(busy:Bool, status:String):Void
        {
                var buttons = Browser.document.querySelectorAll("#go button");
                for (i in 0...buttons.length)
                        untyped buttons.item(i).disabled = busy;
                var text = Browser.document.getElementById("statusText");
                if (text != null) text.textContent = status;
        }

        static function formatBytes(value:Int):String
        {
                if (value < 1024) return value + " B";
                if (value < 1024 * 1024) return Math.round(value / 1024) + " KiB";
                return (Math.round(value / 104857.6) / 10) + " MiB";
        }
}
