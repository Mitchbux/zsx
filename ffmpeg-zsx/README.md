# Native ZSX codecs for FFmpeg 6.1.2

This directory builds four real libavcodec codecs, not a media-wrapper:

* `zsx_video` — one canonical production ZSX1 chunk per video frame.
* `zsx_dct_video` — deterministic native YUV420P 8×8 DCT video with I and
  reference-residual P0 pictures.
* `zsx_audio` — one canonical production ZSX1 chunk per interleaved
  `AV_SAMPLE_FMT_FLT` audio frame.
* `zsx_mdct_audio` — an experimental, lossy, independently decodable
  sine-window MDCT audio codec.

The callbacks use the exact production payload in the repository-root
`zsx.c`. `scripts/build.sh` copies that file verbatim into the temporary
FFmpeg source tree as `libavcodec/zsx_core.c` and compiles it with its CLI
`main` renamed. The checked-in bridge only adapts FFmpeg pointer/size buffers
to the production allocator; it does not contain a second predictor or
experimental/nibble stage.

## Build

Requirements are an existing C compiler, `make`, `tar`, and `curl` for the
initial source download. `nasm` is not required. The official source archive
is downloaded to `/tmp` by default and the extracted/modified source and
object files remain in `/tmp` as well:

```sh
make -C ffmpeg-zsx
# or, from this directory:
JOBS=2 ./scripts/build.sh
```

The runnable binaries are `ffmpeg-zsx/bin/ffmpeg` and
`ffmpeg-zsx/bin/ffprobe`. `FFMPEG_ARCHIVE`, `FFMPEG_SOURCE_DIR`, and
`FFMPEG_BUILD_DIR` can override the temporary paths. FFmpeg libraries are
linked statically, while libc uses the host environment. For a self-contained
x86-64 Linux executable, use `ZSX_STATIC_LINUX=1` with Zig 0.14.1 installed;
this statically links musl as well. `FFMPEG_BIN_DIR` selects a separate binary
output directory. See `LINUX_PACKAGE_README.md` for MP4-to-ZSV commands.
The build does not autodetect external libraries, disables x86 assembly, and enables
only the requested codecs, NUT/MOV/MP3/WAV/raw demuxers, NUT/WAV/raw/f32
muxers, `file`/`pipe` protocols, and the required filters.  In particular,
the binary can read MP3 directly (`mp3` demuxer, `mpegaudio` parser, and
`mp3float` decoder); it does not require a system FFmpeg to prepare input.

The official `https://ffmpeg.org/releases/ffmpeg-6.1.2.tar.xz` archive is
pinned before extraction to SHA-256
`3b624649725ecdc565c903ca6643d41f33bd49239922e45c9b1442c63dca4e38`.
`FFMPEG_ARCHIVE` overrides are accepted only when they have this exact digest.
The recipe always extracts a verified pristine release before patching, so a
clean build is reproducible.  It uses at most two compiler jobs (`JOBS=2` by
default); the resulting executable hash naturally depends on the local C
toolchain.

## Native NUT schema

### ZSV2 profile

New `.zsv` archives use ordinary NUT packets with global metadata
`zsv_profile=2` and **exactly two streams**: `zsx_dct_video` (`ZSXD`) and
`zsx_mdct_audio` (`ZSXM`).  This is a new native profile, not old Python
`ZSV1`/AAC compatibility, and existing archives must not be overwritten.

```sh
ffmpeg-zsx/bin/ffmpeg -i input.mp4 -map 0:v:0 -map 0:a:0 \
  -c:v zsx_dct_video -pix_fmt yuv420p -zsx_quality 70 \
  -c:a zsx_mdct_audio -metadata zsv_profile=2 -f nut output.zsv
```

`zsx_dct_video` accepts even-sized YUV420P only, up to 4096×4096.  Its
extradata binds the version, colour format, dimensions, and quantisation
quality.  Packets begin with `ZSXD`, version, I/P0 flag, quality, dimensions,
frame index, RLE and compressed lengths, reconstruction CRC, and a version-1
header-CRC field.  That CRC is calculated over the **entire packet** (with its
own four-byte field zeroed), so it authenticates all side information, chunk
framing, and production-ZSX bytes; reconstruction CRC separately guards the
decoded reference pixels.
P0 is a residual from the last decoded reconstruction; a decoder with no
reference rejects it.  An I picture is emitted at least every 24 pictures, so
seek/flush starts safely at an I packet.

The integer transform uses a literal, precomputed orthonormal 8×8 basis scaled
by 16384, int64 two-pass sums, and symmetric signed rounding.  There is no
runtime cosine, floating-point transform, or floating-point IDCT.  JPEG
luminance/chrominance quantisation tables use quality 1–100 (default 70).
Coefficient-major zig-zag RLE DC-deltas are production-ZSX compressed.  RLE
larger than one 12 MiB production chunk is explicitly framed into checked
12 MiB-or-smaller bridge chunks; bounded RLE above 128 MiB is rejected.
This preserves Linux/Windows reference CRC determinism rather than silently
depending on platform rounding or unbounded allocations.

The intended command is:

```sh
ffmpeg-zsx/bin/ffmpeg -i input.mp4 \
  -map 0:v:0 -map 0:a:0 \
  -c:v zsx_video -pix_fmt yuv420p \
  -c:a zsx_audio -sample_fmt flt -f nut output.zsx.nut
```

NUT stream tags are `ZSXV` for legacy lossless `zsx_video`, `ZSXD` for native
DCT video, `ZSXA` for lossless `zsx_audio`, and `ZSXM` for `zsx_mdct_audio`.
NUT carries width, height, sample rate, timestamps, and frame duration in its
stream headers/packets. The native stream extradata carries the remaining
format identity: `ZSXV`, version 1, and one pixel-format byte for video; and
`ZSXA`, version 1, `AV_SAMPLE_FMT_FLT`, channel-order byte, channel count, and
an eight-byte native-layout mask for audio. Thus the FFmpeg decoder restores
the exact format before allocating each frame. Video is packed with
`av_image_copy_to_buffer()` before encoding and copied back into normal
FFmpeg frame strides while decoding. `yuv420p` is the required production
path; `rgb24` and `gray8` are also advertised by the native video codec.
Audio is interleaved little-endian float32 on the wire (native-endian
`AV_SAMPLE_FMT_FLT` is byte-swapped on big-endian hosts). Native and
unspecified channel orders are supported; arbitrary custom channel maps are
rejected rather than silently losing their layout.

For byte-for-byte verification, the custom build also includes the native
`rawvideo` and `pcm_f32le` encoders plus `rawvideo` and `f32le` muxers:

```sh
ffmpeg-zsx/bin/ffmpeg -i output.zsx.nut -map 0:v:0 -c:v rawvideo \
  -f rawvideo video.yuv
ffmpeg-zsx/bin/ffmpeg -i output.zsx.nut -map 0:a:0 -c:a pcm_f32le \
  -f f32le audio.f32
```

No MP4 registration is provided; native ZSX streams are supported in NUT.
Each production chunk has a 12 MiB uncompressed limit, so a 3,790,800-byte
1080x2340 `yuv420p` frame fits. The production header's CRC field is
canonically zero and is validated as such by both the production decoder and
the FFmpeg bridge. Malformed packet lengths, stream lengths, miss counts,
CRC, and decoded output sizes are rejected before handing data to FFmpeg.

`-threads 1` is recommended for deterministic low-memory verification. The
codec itself has no inter-frame dependency or delayed frames, but the
production range coder allocates fixed working tables for each active
encode/decode call.

## Experimental MDCT audio

`zsx_mdct_audio` is a native-C codec, not a Python wrapper and not AAC.  It
accepts and returns packed/interleaved `AV_SAMPLE_FMT_FLT` PCM.  Its long
transform is the same normalized formulation used by the audio experiment:
1024 MDCT bins, a 2048-sample AAC sine window, a 1024-sample hop, analysis
normalization `1/sqrt(N)`, and synthesis `2/sqrt(N)`.  The implementation
uses a radix-2 4096-point FFT formulation rather than a cosine matrix.  It
has numerical TDAC closure with the sine window; every packet also has its
own virtual zero prefix and suffix and its final extra MDCT block.

Each packet represents exactly 1 through 16384 input samples (`frame_size`
is 16384 and `AV_CODEC_CAP_SMALL_LAST_FRAME` is advertised).  It has no
cross-packet predictor, delay, flush state, or overlap state, so a packet is
safe to seek to and its decoded frame has the packet's exact sample count.
Packet PTS advances by the input sample count. Mono is coded directly. Stereo defaults to the orthonormal
`M=(L+R)/sqrt(2), S=(L-R)/sqrt(2)` representation and can be switched to LR.

NUT's probed container duration can stop at the last packet's starting PTS;
this also happens with ordinary PCM in NUT. Verify the complete audio duration
from decoded sample counts, not that estimate. The 12-second MP3 smoke test
decodes to exactly 529200 samples per channel at 44100 Hz.

```sh
# Directly read an MP3 and write native experimental audio in NUT.
ffmpeg-zsx/bin/ffmpeg -i input.mp3 -map 0:a:0 -ac 2 -sample_fmt flt \
  -c:a zsx_mdct_audio -zsx_step 0.016 -zsx_ms 1 -f nut mdct.zsx.nut

# Decode to a normal playable float WAV (or use pcm_s16le for integer WAV).
ffmpeg-zsx/bin/ffmpeg -i mdct.zsx.nut -c:a pcm_f32le decoded.wav
```

`-zsx_step` is a positive finite quality/quantisation control (default
`0.016`), **not a bitrate promise**.  Coefficients use signed power-3/4
quantisation and power-4/3 inverse quantisation, unsigned zig-zag varints,
then the production ZSX entropy codec.  `-zsx_ms` is a boolean and defaults
to true for stereo (it is ignored for mono).  This is only an experimental
quality reference: it has no psychoacoustic model, TNS, block switching, or
claim of superiority to AAC.

The native packet format is intentionally different from Python `AMZSX02`;
there is no Python-format compatibility claim.  Packet bytes begin with
`ZSXM`, version 2, flags/channel mode, channel count and channel-layout
order/mask, original sample count, MDCT frame count and bin count, finite
float32 quantisation step, uncompressed coefficient length, compressed
length, and a CRC-32.  The CRC is calculated over the entire reconstruction
metadata and production-ZSX payload with only its own field zeroed.  Decoders
reject unknown flags, bad dimensions/layouts/lengths, integer overflow,
non-finite steps, malformed or overflowing varints, checksum failures, and
non-finite reconstructed samples before exposing an audio frame.

Use `.nut` only.  `ZSXM` is registered only as a NUT tag; it is not ordinary
AAC and must not be written to `.aac`, `.m4a`, or MP4.

## Provenance and licenses

* FFmpeg 6.1.2: official release archive from `https://ffmpeg.org/releases/`,
  licensed under the GNU LGPL (see the FFmpeg source `LICENSE.md`).
* Production ZSX core: repository-root `zsx.c`, project MIT license.
* `ffmpeg-6.1.2-zsx.patch` and the adapter sources are project integration
  code; they do not alter the root production core.