#!/bin/sh
set -eu
# Rebuilds a clean, custom-only FFmpeg 6.1.2 tree with emscripten.
ROOT=$(CDPATH= cd -- "$(dirname "$0")/../../.." && pwd)
HERE=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
ARCHIVE=${FFMPEG_ARCHIVE:-/tmp/ffmpeg-6.1.2.tar.xz}
SRC=${FFMPEG_SOURCE_DIR:-/tmp/zsv-ffmpeg-6.1.2}
BUILD=${FFMPEG_BUILD_DIR:-/tmp/zsv-ffmpeg-build-6.1.2}
# Replit's Nix Emscripten cache is complete but read-only. Clone it into /tmp
# so Emscripten can add the small system libraries needed by this link.
if [ -z "${EM_CACHE:-}" ]; then
  EMCC_ROOT=$(dirname "$(dirname "$(readlink -f "$(command -v emcc)")")")/share/emscripten
  if [ -d "$EMCC_ROOT/cache" ] && [ ! -w "$EMCC_ROOT/cache" ]; then
    EM_CACHE=/tmp/zsv-emscripten-cache
    if [ ! -d "$EM_CACHE" ]; then
      cp -a "$EMCC_ROOT/cache" "$EM_CACHE"
      chmod -R u+w "$EM_CACHE"
    fi
    export EM_CACHE
  fi
fi
export EM_FROZEN_CACHE=${EM_FROZEN_CACHE:-0}
test -f "$ARCHIVE" || curl -L --fail -o "$ARCHIVE" https://ffmpeg.org/releases/ffmpeg-6.1.2.tar.xz
test "$(sha256sum "$ARCHIVE"|cut -d' ' -f1)" = 3b624649725ecdc565c903ca6643d41f33bd49239922e45c9b1442c63dca4e38
rm -rf "$SRC" "$BUILD"; mkdir -p "$BUILD"; tar -xf "$ARCHIVE" -C "$BUILD"; mv "$BUILD/ffmpeg-6.1.2" "$SRC"
for file in "$ROOT"/ffmpeg-zsx/src/zsx_*.c "$ROOT"/ffmpeg-zsx/src/zsx_*.h; do
  cp "$file" "$SRC/libavcodec/"
done
cp "$ROOT/zsx.c" "$SRC/libavcodec/zsx_core.c"
patch -d "$SRC" -p1 < "$ROOT/ffmpeg-zsx/patches/ffmpeg-6.1.2-zsx.patch"
(cd "$SRC" && emconfigure ./configure --disable-autodetect --disable-everything --disable-programs --disable-doc --disable-network --disable-x86asm --disable-asm --disable-pthreads --disable-shared --enable-avcodec --enable-avformat --enable-avutil --enable-demuxer=nut --enable-decoder=zsx_dct_video,zsx_mdct_audio --enable-cross-compile --arch=wasm32 --target-os=none --cc=emcc --cxx=em++ --ar=emar --ranlib=emranlib --extra-cflags="-I$HERE")
emmake make -C "$SRC" -j"${JOBS:-2}"
emcc "$HERE/zsv_decoder.c" "$SRC/libavformat/libavformat.a" "$SRC/libavcodec/libavcodec.a" "$SRC/libavutil/libavutil.a" -I"$SRC" -O3 -sWASM=1 -sASYNCIFY=1 -sASYNCIFY_STACK_SIZE=131072 -sMODULARIZE=1 -sEXPORT_ES6=1 -sENVIRONMENT=web,worker -sALLOW_MEMORY_GROWTH=1 -sABORTING_MALLOC=0 -sINITIAL_MEMORY=33554432 -sMAXIMUM_MEMORY=268435456 -sEXPORTED_FUNCTIONS='["_malloc","_free","_zsv_open","_zsv_open_stream","_zsv_next","_zsv_free","_zsv_close","_zsv_error"]' -sEXPORTED_RUNTIME_METHODS='["HEAPU8","HEAP32","UTF8ToString","ccall"]' -o "$HERE/../public/decoder/zsv_decoder.js"