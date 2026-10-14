# zsv.exe — B&W&R converter

Standalone Windows x64 C command-line application. No Python, external
ffmpeg.exe, or FFmpeg DLL installation is required. The program links the
patched FFmpeg libraries directly.

```bat
zsv.exe "input.mp4" "output.zsv"
```

Output: ZSV2/NUT, 270×480 center-cropped portrait, 25 fps, candidate16
ZSX DCT quality 70, stereo 44.1 kHz ZSX MDCT audio (step 0.016).
Silent videos receive a silent stereo track for browser compatibility.
This is lossy encoding, not lossless compression.

## B&W&R

Frames are converted to RGB, then only saturated red hues are retained.
Hue distance from red is fully retained within 10 degrees, feathered to
gray by 15 degrees. Saturation is feathered from 0.35 to 0.50. All other
pixels use BT.601 grayscale. The filtered image is encoded as YUV420P so
the existing ZSV browser player displays the red accents without a new
codec or shader. Color subsampling can soften red edges.

This matches the website's red-accent sample processing. Existing grayscale
files cannot recover red that was removed from their original source.

## Inputs and safeguards

Common local MP4/MOV, MKV/WebM, AVI, MPEG, FLV, Ogg and NUT inputs.
Included video decoders: H.264, HEVC, MPEG-4, MPEG-2, MJPEG, VP8, VP9, ZSV.
Audio: AAC, MP3, Opus, Vorbis, FLAC, ALAC and common PCM formats.
AV1, encrypted/DRM content and network URLs are not supported by this build.
Download online videos separately before conversion.

The converter does not overwrite an existing destination or `.part` file.
Failed conversions remove their temporary output and exit nonzero.
Input timestamps are preserved relative to the container start; malformed
timestamp-less input is rejected. Rotation metadata, HDR tone mapping,
subtitles and additional audio/video tracks are not processed. First/best
video and audio tracks only. Output may exceed the browser's 64 MiB limit
for long or complex recordings; split long recordings before conversion.

## Rebuild / relink

Run `sh tools/zsv/build-windows.sh` from a checkout of this repository on
Linux, with Zig 0.14.1, make, curl, tar, patch, sed, binutils, and sha256sum.
The recipe downloads and checksum-verifies FFmpeg 6.1.2, applies the
repository codec sources and patch, then links the Windows executable.
No repository secrets or private outputs are needed.

Outputs live under `generated-artifacts/zsv-windows/`. `zsv.o` is supplied
in the release package to permit relinking against modified FFmpeg libraries.
The rebuild script's final link command can also be rerun using your own
compatible libraries. Use a new `ZSV_BUILD_ROOT` after changing codec sources
or FFmpeg configuration; existing configured build trees are reused.

## Licensing

This wrapper is LGPL-2.1-or-later. FFmpeg is built without GPL/nonfree options.
FFmpeg's LGPL v2.1 text and the application object file accompany the binary.
Corresponding wrapper, codec source, patch and reproducible build recipe are
in this repository; upstream FFmpeg source is available at
https://ffmpeg.org/releases/ffmpeg-6.1.2.tar.xz (hash pinned in the recipe).
See the repository's existing codec licensing notices as well.
