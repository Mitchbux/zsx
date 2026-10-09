#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
CC=${CC:-cc}
FLAGS="-std=c11 -g -O1 -Wall -Wextra ${SANITIZE:--fsanitize=address,undefined}"
$CC $FLAGS -DZSX_LIBRARY -Dmain=zsx_cli_main -c "$ROOT/zsx.c" -o "$WORK/core.o"
$CC $FLAGS "$ROOT/ffmpeg-zsx/tests/stream_safety.c" \
    "$ROOT/ffmpeg-zsx/src/zsx_stream.c" "$ROOT/ffmpeg-zsx/src/zsx_core_bridge.c" \
    "$WORK/core.o" -Wl,--wrap=malloc,--wrap=calloc,--wrap=free -o "$WORK/check"
"$WORK/check"
