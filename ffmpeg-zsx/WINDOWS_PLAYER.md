# ZSV2 Windows player

`outputs/windows-zsv-player/zsvplay.exe` is a Windows x86_64 SDL2 GUI build
derived from FFmpeg 6.1.2 `ffplay`.  It is deliberately **not** a general
media player: it accepts only a local `.zsv` path (case-insensitive extension,
including a file dropped on the EXE by Windows Explorer).

The player opens input with FFmpeg's `file` protocol and has networking and
all other protocols disabled.  It forces the NUT demuxer and validates the
opened input, rather than trusting its extension:

* the container must be NUT;
* container metadata must contain exactly one `zsv_profile=2`;
* there must be exactly two streams, with no subtitles, data, attachments, or
  other extra streams;
* the video stream must be `AV_CODEC_ID_ZSX_DCT_VIDEO`, NUT tag `ZSXD`, and
  version-1 `ZSXD` extradata;
* the audio stream must be `AV_CODEC_ID_ZSX_MDCT_AUDIO`, NUT tag `ZSXM`, and
  version-2 `ZSXM` extradata.

Only `zsx_dct_video` and `zsx_mdct_audio` decoders are compiled and permitted.
The normal decoder-forcing options (`-codec`, `-vcodec`, `-acodec`, and
`-scodec`) and forced-format option are not present.  This rejects normal
files, renamed MP4/NUT files, NUT with wrong tags/codecs, and the older
ZSV1/AAC form.  Rejection in the input thread sets a nonzero process status.

## Build from source

From the repository root:

```sh
JOBS=2 ./ffmpeg-zsx/scripts/build-windows-player.sh
```

The recipe:

1. verifies the official FFmpeg 6.1.2 archive at
   `https://ffmpeg.org/releases/ffmpeg-6.1.2.tar.xz` against SHA-256
   `3b624649725ecdc565c903ca6643d41f33bd49239922e45c9b1442c63dca4e38`;
2. fetches the official SDL2 MinGW development archive from
   `https://github.com/libsdl-org/SDL/releases/download/release-2.30.11/SDL2-devel-2.30.11-mingw.zip`
   and verifies SHA-256
   `6011545c022e5aac38077e4b2adf9ebef737aca6fbb645050b5b182f44bae581`;
3. extracts a separate modified tree under
   `temporarybuildtree/windows-zsv-player`, applies the codec integration
   patch and `patches/ffplay-zsv-only.patch`, and cross-compiles with
   `zig cc -target x86_64-windows-gnu`;
4. writes `zsvplay.exe` and the required `SDL2.dll` beside it in
   `outputs/windows-zsv-player`.

The build intentionally uses no more than two jobs by default.  It requires
`zig`, `make`, `tar`, `patch`, `curl`, `unzip`, and standard POSIX shell tools.
It does not alter `/tmp/ffmpeg-zsx-6.1.2`, which is reserved for the native
Linux codec build.

## Running

Use a Windows local path:

```bat
zsvplay.exe C:\media\clip.zsv
```

Dragging a `.zsv` file onto `zsvplay.exe` supplies the same command-line
argument.  No file picker is included; this avoids adding an unreviewed
Unicode/path conversion path to the small restricted player.

For a Wine smoke test that still initializes the video decode/render path,
use SDL dummy drivers and **do not** use `-nodisp`:

```sh
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy SDL_RENDER_DRIVER=software \
  wine outputs/windows-zsv-player/zsvplay.exe sample.zsv -autoexit
```

This is a headless functional check, not a verification of desktop GUI
rendering on Windows.

## License and source offer

FFmpeg 6.1.2 is licensed under the GNU Lesser General Public License, version
2.1 or later.  Its complete corresponding source and `LICENSE.md` are in the
verified official release archive described above; the exact rebuild recipe,
patches, and integration sources are included in this repository.  Preserve
FFmpeg's LGPL notices and provide those sources and this rebuild information
when distributing the executable and `SDL2.dll`.

SDL2 is distributed under the zlib license; see the `LICENSE.txt` included in
the official SDL archive.  The project ZSX integration sources retain their
own project licensing.