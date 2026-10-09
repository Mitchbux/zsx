#!/bin/sh
#
# Build the deliberately narrow Windows x86_64 ZSV2 player.  This recipe
# always starts with FFmpeg's verified 6.1.2 source archive and never touches
# the Linux native-code build tree.
#
set -eu

SELF_DIR=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
ROOT_DIR=$(CDPATH= cd -- "$SELF_DIR/.." && pwd)
VERSION=6.1.2
FFMPEG_ARCHIVE=${FFMPEG_ARCHIVE:-/tmp/ffmpeg-${VERSION}.tar.xz}
WORK_ROOT=${WINDOWS_PLAYER_WORK_ROOT:-$ROOT_DIR/temporarybuildtree/windows-zsv-player}
SOURCE=$WORK_ROOT/ffmpeg-${VERSION}
SDL_ROOT=$WORK_ROOT/SDL2-2.30.11/x86_64-w64-mingw32
SDL_ARCHIVE=$WORK_ROOT/downloads/SDL2-devel-2.30.11-mingw.zip
OUTPUT=$ROOT_DIR/outputs/windows-zsv-player
JOBS=${JOBS:-2}

FFMPEG_SHA256=3b624649725ecdc565c903ca6643d41f33bd49239922e45c9b1442c63dca4e38
SDL_URL=https://github.com/libsdl-org/SDL/releases/download/release-2.30.11/SDL2-devel-2.30.11-mingw.zip
SDL_SHA256=6011545c022e5aac38077e4b2adf9ebef737aca6fbb645050b5b182f44bae581

need() {
    command -v "$1" >/dev/null 2>&1 ||
        { echo "windows-player build: required tool not found: $1" >&2; exit 1; }
}

for tool in tar patch sha256sum unzip zig make curl; do
    need "$tool"
done

if [ ! -f "$FFMPEG_ARCHIVE" ]; then
    curl -L --fail --retry 3 -o "$FFMPEG_ARCHIVE" \
        "https://ffmpeg.org/releases/ffmpeg-${VERSION}.tar.xz"
fi
test "$(sha256sum "$FFMPEG_ARCHIVE" | awk '{print $1}')" = "$FFMPEG_SHA256" ||
    { echo "windows-player build: FFmpeg archive SHA-256 mismatch" >&2; exit 1; }

mkdir -p "$WORK_ROOT/downloads"
if [ ! -f "$SDL_ARCHIVE" ]; then
    curl -L --fail --retry 3 -o "$SDL_ARCHIVE" "$SDL_URL"
fi
test "$(sha256sum "$SDL_ARCHIVE" | awk '{print $1}')" = "$SDL_SHA256" ||
    { echo "windows-player build: SDL2 archive SHA-256 mismatch" >&2; exit 1; }

rm -rf "$SOURCE" "$WORK_ROOT/SDL2-2.30.11" "$WORK_ROOT/tool-bin"
tar -xf "$FFMPEG_ARCHIVE" -C "$WORK_ROOT"
unzip -q "$SDL_ARCHIVE" -d "$WORK_ROOT"
# Zig's Windows driver searches libSDL2.a before libSDL2.dll.a.  Preserve the
# static archive under an explicit name and place SDL's official import
# library at that search name so ffplay is dynamically linked to SDL2.dll.
mv "$SDL_ROOT/lib/libSDL2.a" "$SDL_ROOT/lib/libSDL2-static.a"
cp "$SDL_ROOT/lib/libSDL2.dll.a" "$SDL_ROOT/lib/libSDL2.a"
mkdir -p "$WORK_ROOT/tool-bin"
cat >"$WORK_ROOT/tool-bin/x86_64-w64-mingw32-sdl2-config" <<EOF
#!/bin/sh
exec "$SDL_ROOT/bin/sdl2-config" "\$@"
EOF
chmod 755 "$WORK_ROOT/tool-bin/x86_64-w64-mingw32-sdl2-config"

# The integration patch and sources are owned by the native codec build.  The
# player uses only its ZSX DCT video and ZSX MDCT audio pieces, and refuses to
# configure if those required current sources are absent.
for source_file in zsx_core_api.h zsx_core_bridge.c zsx_stream.c zsx_stream.h zsx_mdct_audio.c \
                   zsx_dct_video.c zsx_coefficient_rle.c zsx_coefficient_rle.h; do
    test -f "$SELF_DIR/src/$source_file" ||
        { echo "windows-player build: missing required codec source $source_file" >&2; exit 1; }
    cp "$SELF_DIR/src/$source_file" "$SOURCE/libavcodec/$source_file"
done
cp "$ROOT_DIR/zsx.c" "$SOURCE/libavcodec/zsx_core.c"
patch --batch -d "$SOURCE" -p1 <"$SELF_DIR/patches/ffmpeg-6.1.2-zsx.patch"
patch --batch -d "$SOURCE" -p1 <"$SELF_DIR/patches/ffplay-zsv-only.patch"

cd "$SOURCE"
# FFmpeg 6.1.2 adds GNU ld's --pic-executable for MinGW release builds.  Zig
# 0.14's lld does not implement that spelling; PE executables are already
# position-independent under the Zig target, so remove only that incompatible
# test/link flag in this disposable extracted source tree.
sed -i 's/ -Wl,--pic-executable,-e,_mainCRTStartup//' configure
sed -i 's/ -Wl,--pic-executable,-e,mainCRTStartup//' configure
PATH="$WORK_ROOT/tool-bin:$PATH" CFLAGS= CPPFLAGS= LDFLAGS= ./configure \
    --arch=x86_64 \
    --target-os=mingw32 \
    --enable-cross-compile \
    --cross-prefix=x86_64-w64-mingw32- \
    --cc='zig cc -target x86_64-windows-gnu' \
    --ar='zig ar' \
    --ranlib='zig ranlib' \
    --nm="$(command -v nm)" \
    --strip="$(command -v strip)" \
    --windres='zig rc' \
    --disable-autodetect \
    --disable-avdevice \
    --disable-debug \
    --disable-doc \
    --disable-everything \
    --disable-iconv \
    --disable-network \
    --disable-x86asm \
    --disable-ffmpeg \
    --disable-ffprobe \
    --disable-ffplay \
    --enable-avcodec \
    --enable-avfilter \
    --enable-avformat \
    --enable-avutil \
    --enable-swresample \
    --enable-swscale \
    --enable-ffplay \
    --enable-sdl2 \
    --enable-protocol=file \
    --enable-demuxer=nut \
    --enable-decoder=zsx_dct_video,zsx_mdct_audio \
    --enable-filter=anull,aresample,format,null \
    --enable-small

make -j"$JOBS" ffplay.exe

rm -rf "$OUTPUT"
mkdir -p "$OUTPUT"
cp ffplay.exe "$OUTPUT/zsvplay.exe"
cp "$SDL_ROOT/bin/SDL2.dll" "$OUTPUT/SDL2.dll"
cp LICENSE.md "$OUTPUT/FFMPEG-LICENSE.md"
cp "$WORK_ROOT/SDL2-2.30.11/LICENSE.txt" "$OUTPUT/SDL2-LICENSE.txt"
cp "$SELF_DIR/WINDOWS_PLAYER.md" "$OUTPUT/SOURCE-AND-REBUILD.md"
chmod 755 "$OUTPUT/zsvplay.exe"

echo "built Windows player:"
echo "  $OUTPUT/zsvplay.exe"
echo "runtime DLL:"
echo "  $OUTPUT/SDL2.dll"
echo "notices and rebuild instructions:"
echo "  $OUTPUT/FFMPEG-LICENSE.md"
echo "  $OUTPUT/SDL2-LICENSE.txt"
echo "  $OUTPUT/SOURCE-AND-REBUILD.md"