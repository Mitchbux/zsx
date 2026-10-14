#!/bin/sh
# Cross-build a standalone Windows x64 C program with static FFmpeg libraries.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
WORK=${ZSV_BUILD_ROOT:-$ROOT/temporarybuildtree/zsv-converter}
SRC=$WORK/ffmpeg-6.1.2
OUT=$ROOT/generated-artifacts/zsv-windows
ARCHIVE=${FFMPEG_ARCHIVE:-/tmp/ffmpeg-6.1.2.tar.xz}
JOBS=${JOBS:-2}
mkdir -p "$WORK" "$OUT"
if [ ! -f "$ARCHIVE" ]; then
    curl -fL --retry 3 https://ffmpeg.org/releases/ffmpeg-6.1.2.tar.xz -o "$ARCHIVE"
fi
echo "3b624649725ecdc565c903ca6643d41f33bd49239922e45c9b1442c63dca4e38  $ARCHIVE" | sha256sum -c -
if [ ! -f "$SRC/.zsv-configured" ]; then
    test ! -d "$SRC" || { echo "Incomplete build directory; choose a new ZSV_BUILD_ROOT." >&2; exit 1; }
    tar -xf "$ARCHIVE" -C "$WORK"
    cp "$ROOT"/ffmpeg-zsx/src/*.[ch] "$SRC/libavcodec/"
    cp "${ZSV_CORE_SOURCE:-$ROOT/zsx.c}" "$SRC/libavcodec/zsx_core.c"
    patch --batch -d "$SRC" -p1 < "$ROOT/ffmpeg-zsx/patches/ffmpeg-6.1.2-zsx.patch"
    # Zig LLD does not support the GNU ld --pic-executable spelling.
    sed -i 's/ -Wl,--pic-executable,-e,_mainCRTStartup//;s/ -Wl,--pic-executable,-e,mainCRTStartup//' "$SRC/configure"
    (cd "$SRC" && CFLAGS= CPPFLAGS= LDFLAGS= ./configure \
        --arch=x86_64 --target-os=mingw32 --enable-cross-compile \
        --cc='zig cc -target x86_64-windows-gnu' --ar='zig ar' --ranlib='zig ranlib' \
        --nm=nm --strip=strip --windres='zig rc' \
        --disable-autodetect --disable-debug --disable-doc --disable-programs \
        --disable-avdevice --disable-network --disable-iconv --disable-x86asm \
        --disable-everything --enable-small \
        --enable-protocol=file --enable-demuxer=mov,matroska,avi,mpegts,mpegps,nut,flv,ogg \
        --enable-parser=h264,hevc,mpeg4video,mpegaudio,aac,aac_latm,opus,vorbis,vp8,vp9,mpegvideo \
        --enable-decoder=h264,hevc,mpeg4,mpeg2video,mjpeg,vp8,vp9,aac,aac_latm,mp3,mp3float,opus,vorbis,flac,alac,pcm_s16le,pcm_s24le,pcm_s32le,pcm_f32le,zsx_dct_video,zsx_mdct_audio \
        --enable-encoder=zsx_dct_video,zsx_mdct_audio --enable-muxer=nut \
        --enable-filter=buffer,buffersink,abuffer,abuffersink,scale,crop,setsar,fps,format,aresample,aformat,asetnsamples)
    touch "$SRC/.zsv-configured"
fi
(cd "$SRC" && make -j"$JOBS")
zig cc -target x86_64-windows-gnu -municode -O2 -Wall -Wextra \
    -I"$SRC" -c "$ROOT/tools/zsv/zsv.c" -o "$OUT/zsv.o"
zig cc -target x86_64-windows-gnu -municode -O2 -o "$OUT/zsv.exe" "$OUT/zsv.o" \
    -Wl,--start-group "$SRC/libavformat/libavformat.a" "$SRC/libavcodec/libavcodec.a" \
    "$SRC/libavfilter/libavfilter.a" "$SRC/libswscale/libswscale.a" \
    "$SRC/libswresample/libswresample.a" "$SRC/libavutil/libavutil.a" \
    -Wl,--end-group -lws2_32 -lbcrypt -lsecur32 -lm
cp "$SRC/COPYING.LGPLv2.1" "$OUT/FFMPEG-LGPL-2.1.txt"
cp "$ROOT/tools/zsv/README.md" "$OUT/README.md"
echo "Built $OUT/zsv.exe"
