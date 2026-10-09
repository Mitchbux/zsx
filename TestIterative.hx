
import haxe.io.Bytes;
import Shrink;

class TestIterative {
    static function main() {
        
        // Test with enwik8 if it exists
        trace("\n\n=== TESTING WITH ENWIK8 ===");
        
        if (sys.FileSystem.exists("enwik8")) {
            var fileData = sys.io.File.getBytes("enwik8");
            trace("File size: " + fileData.length + " bytes");
            testIterativeCompression(fileData, "enwik8");
        } else {
            trace("enwik8 not found, skipping file test");
        }
        
    }
    
    static function testIterativeCompression(originalData:Bytes, dataName:String):Void {
        var data = originalData;
        var iteration = 0;
        var previousSize = data.length;
        
        trace("\n" + dataName + " - Iteration 0 (Original): " + data.length + " bytes");
        
        while (true) {
            iteration++;
            
            // Encode
            var encoded = Shrink.encode(data);
            var currentSize = encoded.length;
            var ratio = (currentSize / previousSize) * 100;
            var totalRatio = (currentSize / originalData.length) * 100;
            
            trace("Iteration " + iteration + ": " + currentSize + " bytes");
            trace("  Step ratio: " + formatPercent(ratio) + "%");
            trace("  Total ratio: " + formatPercent(totalRatio) + "%");
            trace("  Reduction: " + formatPercent(100 - ratio) + "%");
            
            // Check if it's getting larger
            if (currentSize >= previousSize) {
                trace("\n*** Compression stopped at iteration " + iteration + " (size increased) ***");
                trace("Best compression: " + previousSize + " bytes (" + 
                      formatPercent((previousSize / originalData.length) * 100) + "% of original)");
                trace("Total reduction: " + formatPercent(100 - (previousSize / originalData.length) * 100) + "%");
                break;
            }
            
            // Safety limit
            if (iteration >= 50) {
                trace("\n*** Stopped at iteration limit (50) ***");
                trace("Final size: " + currentSize + " bytes");
                trace("Total ratio: " + formatPercent(totalRatio) + "%");
                break;
            }
            
            // Prepare for next iteration
            previousSize = currentSize;
            data = encoded;
        }
        
        trace("\n=== Summary for " + dataName + " ===");
        trace("Total iterations: " + iteration);
        trace("Original size: " + originalData.length + " bytes");
        trace("Final compressed size: " + previousSize + " bytes");
        trace("Compression ratio: " + formatPercent((previousSize / originalData.length) * 100) + "%");
    }
    
    static function formatPercent(value:Float):String {
        return Std.string(Math.round(value * 100) / 100);
    }
}
