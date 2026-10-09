
import haxe.io.Bytes;
import Shrink;

class TestIterativeVerify {
    static function main() {
        trace("=== ITERATIVE ENCODING TEST WITH VERIFICATION ===\n");
        
        // Test with different sized samples
        runTest("Small String", Bytes.ofString("Hello World! This is a test."));
        runTest("Medium String", Bytes.ofString(generateTestData(100)));
        runTest("Large String", Bytes.ofString(generateTestData(1000)));
        
        #if sys
        if (sys.FileSystem.exists("enwik6")) {
            var fileData = sys.io.File.getBytes("enwik6");
            runTest("enwik6 (1MB Wikipedia)", fileData);
        }
        #end
    }
    
    static function generateTestData(size:Int):String {
        var data = "";
        var base = "The quick brown fox jumps over the lazy dog. ";
        while (data.length < size) {
            data += base;
        }
        return data.substr(0, size);
    }
    
    static function runTest(name:String, originalData:Bytes):Void {
        trace("=== " + name + " ===");
        trace("Original size: " + originalData.length + " bytes\n");
        
        var compressionSteps:Array<Bytes> = [originalData];
        var iteration = 0;
        var data = originalData;
        
        // Compress iteratively until size increases
        while (true) {
            iteration++;
            var encoded = Shrink.encode(data);
            
            if (encoded.length >= data.length) {
                trace("Stopped at iteration " + iteration + " (size would increase)");
                break;
            }
            
            compressionSteps.push(encoded);
            
            var ratio = (encoded.length / originalData.length) * 100;
            trace("Pass " + iteration + ": " + encoded.length + " bytes (" + 
                  formatPercent(ratio) + "% of original, " + 
                  formatPercent(100 - ratio) + "% reduction)");
            
            data = encoded;
            
            if (iteration >= 20) {
                trace("Reached iteration limit");
                break;
            }
        }
        
        trace("\nBest result:");
        trace("  Iterations: " + (compressionSteps.length - 1));
        trace("  Size: " + compressionSteps[compressionSteps.length - 1].length + " bytes");
        trace("  Ratio: " + formatPercent((compressionSteps[compressionSteps.length - 1].length / originalData.length) * 100) + "%");
        trace("  Compression: " + formatPercent(100 - (compressionSteps[compressionSteps.length - 1].length / originalData.length) * 100) + "%");
        
        // Verify decompression
        trace("\nVerifying decompression...");
        var decompressed = compressionSteps[compressionSteps.length - 1];
        for (i in 0...(compressionSteps.length - 1)) {
            var step = compressionSteps.length - 2 - i;
            decompressed = Shrink.decode(decompressed);
            trace("  Decompression pass " + (i + 1) + ": " + decompressed.length + " bytes");
        }
        
        if (decompressed.compare(originalData) == 0) {
            trace("✓ Verification successful - data matches original!");
        } else {
            trace("✗ Verification FAILED - data does not match!");
        }
        
        trace("\n" + "=".repeat(60) + "\n");
    }
    
    static function formatPercent(value:Float):String {
        return Std.string(Math.round(value * 100) / 100);
    }
}
