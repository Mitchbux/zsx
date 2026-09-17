#!/bin/sh
# Build a small, portable test executable from the codec portion of the user's
# attached Windows C source.  No project C source is substituted or modified.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/zsx-latest-c.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

# Line 737 is the end of the latest zsx_decode implementation.  The remaining
# attached source is Win32 file/archive UI.  Removing its Windows-only include
# leaves the codec itself as ordinary C11 for this isolated test harness.
sed -n '1,737p' "$root/reference/zsx-latest.c" |
  sed '/^#define _WIN32_WINNT /d; /^#define NTDDI_VERSION /d; /^#include <windows.h>/d' \
  > "$work/reference.c"
cat >> "$work/reference.c" <<'EOF'
int main(int argc, char **argv) {
	byte *data;
	size_t length;
	int mode;
	if (argc != 2 || (argv[1][0] != 'e' && argv[1][0] != 'd')) return 2;
	mode = argv[1][0];
	data = zsx_new(byte, zsx_buffer);
	length = fread(data, 1, zsx_buffer, stdin);
	if (ferror(stdin) || fgetc(stdin) != EOF) return 3;
	zsx_len(data) = length;
	if (mode == 'e') zsx_encode(data);
	else zsx_decode(data);
	if (fwrite(data, 1, zsx_len(data), stdout) != zsx_len(data)) return 4;
	zsx_del(data);
	return 0;
}
EOF
cc -O2 -std=c11 -Wall -Wextra -Wpedantic "$work/reference.c" -o "$work/reference"

haxe "$root/test_latest.hxml"

check_case() {
  name=$1
  input=$2
  "$work/reference" e < "$input" > "$work/$name.c.chunk"
  node "$root/test_zsx_latest.js" --chunk-encode "$input" "$work/$name.haxe.chunk"
  # Byte-identical output is a vector check, not merely a round trip.
  cmp "$work/$name.c.chunk" "$work/$name.haxe.chunk"
  node "$root/test_zsx_latest.js" --chunk-decode "$work/$name.c.chunk" "$work/$name.haxe.decoded"
  cmp "$input" "$work/$name.haxe.decoded"
  "$work/reference" d < "$work/$name.haxe.chunk" > "$work/$name.c.decoded"
  cmp "$input" "$work/$name.c.decoded"
}

# Empty and random inputs exercise raw chunks; repeated data forces the
# predictor path.  The repeated fixture has an odd length for pair counting.
: > "$work/empty"
check_case empty "$work/empty"
dd if=/dev/urandom of="$work/random" bs=4099 count=1 2>/dev/null
check_case random "$work/random"
: > "$work/repeated"
while [ "$(wc -c < "$work/repeated")" -lt 5001 ]; do
  printf 'latest C/Haxe cross-check: abracadabra! ' >> "$work/repeated"
done
dd if="$work/repeated" of="$work/repeated-odd" bs=1 count=5001 2>/dev/null
check_case repeated-odd "$work/repeated-odd"

# The C reference accepts malformed streams by zero-filling past EOF.  The
# Haxe decoder must reject a reference-shaped chunk with a broken length.
cp "$work/repeated-odd.c.chunk" "$work/corrupt.chunk"
printf '\000' | dd of="$work/corrupt.chunk" bs=1 seek=4 count=1 conv=notrunc 2>/dev/null
if node "$root/test_zsx_latest.js" --chunk-decode "$work/corrupt.chunk" "$work/should-not-exist" >/dev/null 2>&1; then
  echo 'Haxe accepted a corrupt latest C chunk' >&2
  exit 1
fi
printf '%s\n' 'latest C/Haxe cross-compatibility passed'