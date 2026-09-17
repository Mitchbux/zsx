#!/usr/bin/env bash
# Build Zsd.hx into one standalone .NET Framework 4.0 C# source file.
set -euo pipefail
IFS=$'\n\t'

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
project_dir=$(cd -- "$script_dir/../.." && pwd)
hxml="$project_dir/buildzsd.hxml"
generated_dir="$project_dir/tools/zsd/generated"
output_dir="$project_dir/outputs"
output_cs="$output_dir/Zsd.cs"
source_map="$output_dir/Zsd.source-map.txt"
notices="$output_dir/Zsd.NOTICES.txt"
validation_dir=""

cleanup() {
  rm -rf "$generated_dir"
  if [[ -n "${validation_dir:-}" ]]; then
    rm -rf "$validation_dir"
  fi
}
trap cleanup EXIT

require_command() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "Required command is not available: $1" >&2
    exit 1
  fi
}

require_command haxe
require_command haxelib
require_command mcs

mkdir -p "$output_dir"
# A prior result is never allowed to look like the result of this invocation.
# New deliverables stay in validation_dir until every validation succeeds.
rm -f "$output_cs" "$source_map" "$notices"
validation_dir=$(mktemp -d "$output_dir/.zsd-net40-validation.XXXXXX")
staged_cs="$validation_dir/Zsd.cs"
staged_source_map="$validation_dir/Zsd.source-map.txt"
staged_notices="$validation_dir/Zsd.NOTICES.txt"

if [[ ! -f "$hxml" ]]; then
  echo "Missing Haxe build configuration: $hxml" >&2
  exit 1
fi

if [[ ! -f "$project_dir/Zsd.hx" ]]; then
  echo "Missing required Haxe entry point: $project_dir/Zsd.hx" >&2
  echo "Zsd.cs is generated only from Zsd.hx; no substitute C# source is used." >&2
  exit 1
fi

if ! haxelib path hxcs >/dev/null 2>&1; then
  echo "Missing haxelib hxcs. Install it with: haxelib install hxcs" >&2
  exit 1
fi

rm -rf "$generated_dir"

(
  cd "$project_dir"
  haxe "$hxml"
)

"$script_dir/combine-cs.sh" "$generated_dir/src" "$staged_cs" "$staged_source_map"
cp "$script_dir/THIRD-PARTY-NOTICES.txt" "$staged_notices"

# Compile exactly as a .NET 4/C# 4 executable with no hxcs assembly reference.
# The resulting executable is disposable; Zsd.cs is the deliverable.
mcs -sdk:4 -langversion:4 -target:exe \
  -out:"$validation_dir/Zsd.exe" \
  "$staged_cs"

if command -v monodis >/dev/null 2>&1 &&
  monodis --assemblyref "$validation_dir/Zsd.exe" | grep -qi 'hxcs'; then
  echo "Validation failed: the executable references an hxcs runtime assembly." >&2
  exit 1
fi

if grep -RIl --include='*.cs' 'hxcs' "$generated_dir/src" >/dev/null 2>&1; then
  echo "Validation failed: generated C# unexpectedly references hxcs." >&2
  exit 1
fi

mv -f "$staged_cs" "$output_cs"
mv -f "$staged_source_map" "$source_map"
mv -f "$staged_notices" "$notices"

printf '%s\n' \
  "Built standalone source: $output_cs" \
  "Source map manifest:    $source_map" \
  "License notices:        $notices" \
  "Validated with: mcs -sdk:4 -langversion:4 -target:exe"
