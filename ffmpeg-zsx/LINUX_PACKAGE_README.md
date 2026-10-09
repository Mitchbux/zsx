# ZSV FFmpeg for Linux x86-64

Custom FFmpeg 6.1.2 with H.264/AAC MP4 input and ZSV2 video/audio encoding.
The provided executables are statically linked with musl. They do not need
Nix, an installed FFmpeg, a shared-library package, an API key, or network
access. This is an **x86-64** Linux build, not ARM64.

## Quick conversion

Extract the archive, enter its directory, then run:

```sh
./transcode-mp4.sh input.mp4 output.zsv
```

The helper uses the first video and audio streams, preserves frame rate
and even-sized video dimensions (odd dimensions round down by one pixel),
and encodes stereo 44.1 kHz audio. Existing output files are not overwritten.
It requires an audio stream and reports an error if none is present.

## Direct FFmpeg command

```sh
./bin/ffmpeg -i input.mp4 \
  -map 0:v:0 -map 0:a:0 -pix_fmt yuv420p \
  -c:v zsx_dct_video -zsx_quality 70 -zsx_entropy 3 \
  -c:a zsx_mdct_audio -zsx_step 0.016 -ar 44100 -ac 2 \
  -metadata zsv_profile=2 -f nut output.zsv
```

To convert only source timecodes 00:30–01:00, put `-ss 30` before `-i`
and `-t 30` after the input.

- `-zsx_entropy 3`: CPU-friendly 16-bit two-candidate entropy profile.
- `-zsx_entropy 0`: try multiple profiles and select the smallest; slower.
- `-zsx_quality 70`: lossy video quality, 1–100.
- `-zsx_step 0.016`: lossy MDCT audio quantization step.
- `-metadata zsv_profile=2 -f nut`: required container profile. The `.zsv`
  suffix alone does not select the muxer.

ZSV here is custom ZSV2 codec data in a NUT container. Ordinary FFmpeg and
standard MP4 players cannot decode it; use the ZSV browser player or these
included executables. Input is local-file based; network protocols are disabled.
This is a focused transcoder, not a build containing every FFmpeg codec/filter.
Browser playback also enforces its own input, audio-memory and frame-size limits;
successfully encoding a very large file does not guarantee browser acceptance.

## Inspect or validate

```sh
./bin/ffprobe -hide_banner output.zsv
./bin/ffmpeg -v error -xerror -i output.zsv -map 0:v:0 -vsync 0 \
  -f rawvideo -y /dev/null
./bin/ffmpeg -v error -xerror -i output.zsv -map 0:a:0 \
  -f f32le -y /dev/null
sha256sum -c SHA256SUMS
```

## Sources and rebuilding

`source/` contains the pinned original FFmpeg archive, the custom codec source,
the FFmpeg patch, and the actual build script. Requirements: Zig 0.14.1,
make, tar, patch, sha256sum and a POSIX shell. No source download is needed:

```sh
cd source
ZSX_STATIC_LINUX=1 FFMPEG_ARCHIVE="$PWD/ffmpeg-6.1.2.tar.xz" \
  JOBS=4 sh ffmpeg-zsx/scripts/build.sh
```

The output is `source/ffmpeg-zsx/bin/`. To build using your system C compiler
and libc instead, omit `ZSX_STATIC_LINUX=1` (the result may not be portable to
other distributions). Build outputs depend on the compiler/toolchain.

FFmpeg's LGPL license and musl copyright notices are in `licenses/`.
The original FFmpeg archive includes its complete licensing notices.
