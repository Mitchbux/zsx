#!/bin/sh
#
# Reproducibly build the native ZSX codecs against pristine FFmpeg 6.1.2.
# The FFmpeg tree is deliberately outside this repository; this directory
# contains only the small codec sources, patch, and the resulting bin files.
#
set -eu

SELF_DIR=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
ROOT_DIR=$(CDPATH= cd -- "$SELF_DIR/.." && pwd)
VERSION=6.1.2
ARCHIVE=${FFMPEG_ARCHIVE:-/tmp/ffmpeg-${VERSION}.tar.xz}
SOURCE=${FFMPEG_SOURCE_DIR:-/tmp/ffmpeg-zsx-${VERSION}}
BUILD=${FFMPEG_BUILD_DIR:-/tmp/ffmpeg-zsx-build-${VERSION}}
JOBS=${JOBS:-2}
INSTALL=${FFMPEG_INSTALL_DIR:-$BUILD/install}
BIN_DIR=${FFMPEG_BIN_DIR:-$SELF_DIR/bin}
EXPECTED_FFMPEG_SHA256=3b624649725ecdc565c903ca6643d41f33bd49239922e45c9b1442c63dca4e38

if [ ! -f "$ARCHIVE" ]; then
    command -v curl >/dev/null 2>&1 ||
        { echo "build: curl is required to download FFmpeg ${VERSION}" >&2; exit 1; }
    curl -L --fail --retry 3 \
        -o "$ARCHIVE" \
        "https://ffmpeg.org/releases/ffmpeg-${VERSION}.tar.xz"
fi

command -v sha256sum >/dev/null 2>&1 ||
    { echo "build: sha256sum is required to verify FFmpeg ${VERSION}" >&2; exit 1; }
ACTUAL_FFMPEG_SHA256=$(sha256sum "$ARCHIVE" | awk '{ print $1 }')
if [ "$ACTUAL_FFMPEG_SHA256" != "$EXPECTED_FFMPEG_SHA256" ]; then
    echo "build: FFmpeg archive SHA-256 mismatch" >&2
    echo "build: expected $EXPECTED_FFMPEG_SHA256" >&2
    echo "build: actual   $ACTUAL_FFMPEG_SHA256" >&2
    exit 1
fi

rm -rf "$SOURCE" "$BUILD"
mkdir -p "$BUILD"
tar -xf "$ARCHIVE" -C "$BUILD"
mv "$BUILD/ffmpeg-${VERSION}" "$SOURCE"

# Copy the production root verbatim.  No experimental source is consulted.
cp "$ROOT_DIR/zsx.c" "$SOURCE/libavcodec/zsx_core.c"
cp "$SELF_DIR/src/zsx_core_api.h" "$SOURCE/libavcodec/zsx_core_api.h"
cp "$SELF_DIR/src/zsx_core_bridge.c" "$SOURCE/libavcodec/zsx_core_bridge.c"
cp "$SELF_DIR/src/zsx_stream.c" "$SELF_DIR/src/zsx_stream.h" "$SOURCE/libavcodec/"
cp "$SELF_DIR/src/zsx_video.c" "$SOURCE/libavcodec/zsx_video.c"
cp "$SELF_DIR/src/zsx_dct_video.c" "$SOURCE/libavcodec/zsx_dct_video.c"
cp "$SELF_DIR/src/zsx_coefficient_rle.c" "$SOURCE/libavcodec/zsx_coefficient_rle.c"
cp "$SELF_DIR/src/zsx_coefficient_rle.h" "$SOURCE/libavcodec/zsx_coefficient_rle.h"
cp "$SELF_DIR/src/zsx_audio.c" "$SOURCE/libavcodec/zsx_audio.c"
cp "$SELF_DIR/src/zsx_mdct_audio.c" "$SOURCE/libavcodec/zsx_mdct_audio.c"
patch --batch -d "$SOURCE" -p1 < "$SELF_DIR/patches/ffmpeg-6.1.2-zsx.patch"

cd "$SOURCE"

# Optional portable x86-64 Linux build, with musl statically linked by Zig.
# The default build links FFmpeg libraries statically but uses the host libc.
set --
if [ "${ZSX_STATIC_LINUX:-0}" = 1 ]; then
    command -v zig >/dev/null 2>&1 ||
        { echo "build: portable Linux build requires Zig (tested with 0.14.1)" >&2; exit 1; }
    set -- --cc="zig cc -target x86_64-linux-musl" \
        --ar="zig ar" --ranlib="zig ranlib" --extra-ldflags=-static \
        --arch=x86_64 --target-os=linux --enable-cross-compile
fi

# Avoid optional/external dependencies.
# --disable-x86asm is intentional: native C is used when nasm is absent.
./configure "$@" \
    --bindir=bin \
    --disable-autodetect \
    --disable-avdevice \
    --disable-debug \
    --disable-doc \
    --disable-everything \
    --disable-iconv \
    --disable-network \
    --disable-shared \
    --disable-x86asm \
    --prefix="$INSTALL" \
    --enable-avcodec \
    --enable-avfilter \
    --enable-avformat \
    --enable-avutil \
    --enable-swresample \
    --enable-swscale \
    --enable-ffmpeg \
    --enable-ffprobe \
    --enable-protocol=file,pipe \
    --enable-demuxer=mov,mp3,nut,rawvideo,pcm_f32le,wav \
    --enable-muxer=nut,rawvideo,pcm_f32le,wav \
    --enable-decoder=aac,h264,mp3float,pcm_f32le,pcm_s16le,rawvideo,zsx_audio,zsx_mdct_audio,zsx_video,zsx_dct_video \
    --enable-encoder=pcm_f32le,pcm_s16le,rawvideo,zsx_audio,zsx_mdct_audio,zsx_video,zsx_dct_video \
    --enable-parser=aac,h264,mpegaudio \
    --enable-filter=anull,aresample,format,null,scale \
    --enable-small

make -j"$JOBS"

# Do not install FFmpeg's SDK or static libraries into this source directory.
# The programs are already linked in the temporary source tree; copy only
# the two requested runnable artifacts into this directory.
mkdir -p "$BIN_DIR"
cp "$SOURCE/ffmpeg" "$SOURCE/ffprobe" "$BIN_DIR/"
chmod 755 "$BIN_DIR/ffmpeg" "$BIN_DIR/ffprobe"

echo "built:"
echo "  $BIN_DIR/ffmpeg"
echo "  $BIN_DIR/ffprobe"