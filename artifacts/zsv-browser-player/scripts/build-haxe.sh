#!/usr/bin/env sh
set -eu
cd "$(dirname "$0")/.."
if ! command -v haxe >/dev/null 2>&1; then
  echo "haxe is required to compile haxe/build.hxml" >&2
  exit 1
fi
haxe haxe/build.hxml