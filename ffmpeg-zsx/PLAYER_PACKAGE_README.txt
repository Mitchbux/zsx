ZSV PLAYER FOR WINDOWS x86_64
==============================

This is an unsigned, experimental Windows 10/11 x86_64 package.  Extract the
ZIP into a new folder; it is standalone and has no installer, registry step,
or system-wide change.  Keep zsvplay.exe and SDL2.dll together.

Run the included ZSV2 sample from that extracted folder:

    zsvplay.exe sample.zsv

The current player is CLI-only.  It does not include a file picker.  A local
`.zsv` path may be supplied on the command line, or a `.zsv` file may be
dragged onto zsvplay.exe in Windows Explorer.  The player accepts only the
native ZSV2 NUT profile (`zsv_profile=2`), with exactly one ZSX DCT video
stream (`ZSXD`) and one ZSX MDCT audio stream (`ZSXM`).  It is not the older
ZSV1/AAC format and is not a general-format media player.

The normal ffplay controls are retained by the current integration patch:

    Space       pause/resume
    Left/Right  seek
    Up/Down     larger seek
    F            fullscreen
    Q or Esc    quit

The Windows x64 build compiled successfully, and the native codec tests
(including reconstruction, corruption rejection, and seeking) passed.
Windows playback remains unverified: Wine initialization could not complete
after a temporary-filesystem quota failure left required Windows DLLs missing.
Native Windows desktop hardware has not been tested.  This is an experimental
build, not a claim of verified Windows desktop or GUI playback.

LICENSE NOTICES
---------------

`COPYING.LGPLv2.1` and `FFMPEG-LICENSE.md` contain the FFmpeg notices extracted
from the pristine FFmpeg 6.1.2 release archive.  `SDL2-LICENSE.txt` is
extracted from the SDL2 development SDK.  FFmpeg is built with static FFmpeg
libraries, so the complete corresponding FFmpeg source and rebuild inputs are
included below `source`.  SDL2 remains a runtime DLL and its SDK archive is
included for the rebuild.

No additional project license is invented here.  Repository source files
retain their original copyright and license headers.  See the included
FFmpeg and SDL2 notices for those components' terms.

COMPLETE CORRESPONDING SOURCE AND REBUILD
-----------------------------------------

The `source` directory is self-contained for rebuilding this player:

* `source/zsx.c` is the production ZSX core.
* `source/ffmpeg-zsx/src/` contains all checked-in codec integration C and
  header files.
* `source/ffmpeg-zsx/patches/` contains every FFmpeg and ffplay integration
  patch, and `source/ffmpeg-zsx/scripts/` contains the build scripts,
  including `build.sh`, `build-windows-player.sh`, and this packager.
* `source/ffmpeg-zsx/Makefile`, README files, and WINDOWS_PLAYER.md document
  the integration and build.
* `source/ffmpeg-6.1.2.tar.xz` is the pristine FFmpeg 6.1.2 source archive.
  Its SHA-256 is
  3b624649725ecdc565c903ca6643d41f33bd49239922e45c9b1442c63dca4e38.
* `source/temporarybuildtree/windows-zsv-player/downloads/` contains the
  SDL2-devel-2.30.11-mingw.zip archive required by the existing build script.
  Its SHA-256 is
  6011545c022e5aac38077e4b2adf9ebef737aca6fbb645050b5b182f44bae581.

The source archive is intentionally kept as a pristine vendor tarball.  No
modified FFmpeg build tree, object files, PDB files, Wine prefix, cache, or
unrelated project fixture is part of this source offer.

BUILDING THE WINDOWS PLAYER
---------------------------

Use a POSIX shell with the package directory as the current directory.  The
Windows recipe currently requires Zig 0.14.1, `make`, `tar`, `patch`,
`sha256sum`, `unzip`, `curl`, and the usual POSIX shell tools.  The recipe
cross-compiles with Zig's `x86_64-windows-gnu` target.  Read
`source/ffmpeg-zsx/scripts/build-windows-player.sh` for the authoritative
compiler and tooling requirements if that recipe changes.

For a rebuild using the bundled pristine FFmpeg archive and bundled SDL SDK:

    FFMPEG_ARCHIVE="$PWD/source/ffmpeg-6.1.2.tar.xz" \
      JOBS=2 sh source/ffmpeg-zsx/scripts/build-windows-player.sh

This command creates the rebuilt files under
`source/outputs/windows-zsv-player/`.  It does not install anything or modify
the Windows registry.  The build script verifies both pinned archives before
extraction, applies the bundled patches, and produces zsvplay.exe beside
SDL2.dll.  A rebuilt executable's hash can vary with the local toolchain even
when all source inputs match.

For the native Linux FFmpeg tools, the corresponding recipe is
`source/ffmpeg-zsx/scripts/build.sh`; it also accepts the same explicit
archive form:

    FFMPEG_ARCHIVE="$PWD/source/ffmpeg-6.1.2.tar.xz" \
      JOBS=2 sh source/ffmpeg-zsx/scripts/build.sh

`SHA256SUMS` records hashes for the package payload, and
`package-metadata.json` records payload sizes and hashes.  The outer package
hash and size are recorded in the JSON file emitted beside the ZIP.