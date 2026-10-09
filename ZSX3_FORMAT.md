# ZSX3 archive format — version 1

ZSX3 is a **new archive container**, not a new compression algorithm. Zsd
`pack` and `upload` now write it by default, retaining the `.zsd` extension.
The pipeline is:

```text
collect/sort all directories and files
  -> one deterministic ZSDP v1 record stream (paths, sizes, hashes, file bytes)
  -> one ZsxLatest.encode(complete stream), producing ZSL1
  -> ZSX3 v1 container
  -> transport chunks and ZSDT v1 metadata
```

The inner stream is tar-like concatenation, **not POSIX tar**: no tar headers,
512-byte padding, timestamps, permissions, or symlinks. Files are not separately
compressed. The existing codec still partitions the concatenated stream into
12 MiB blocks, resetting its predictor at those block boundaries, not at file
boundaries. “Whole archive compression” does not promise an unlimited solid
dictionary or a particular compression ratio.

## Exact outer wire bytes

All outer integers are unsigned **big-endian**. There is no padding.

| Offset | Bytes | Meaning |
| ---: | ---: | --- |
| 0 | 4 | ASCII `ZSX3` (`5a 53 58 33`) |
| 4 | 2 | Format version, exactly `00 01` |
| 6 | 8 | Uncompressed ZSDP stream length, `U` |
| 14 | 32 | Raw SHA-256 of all `U` uncompressed bytes |
| 46 | 8 | Compressed stream length, `C` |
| 54 | `C` | Exactly one complete `ZSL1` byte stream |

Version 1 **fixes both descriptors**: inner archive = `ZSDP` version 1;
codec = portable `ZSL1` as specified in [ZSX_LATEST_FORMAT.md](ZSX_LATEST_FORMAT.md).
No descriptor inference, native `size_t` fallback, extra flags, or concatenated
codec frames are allowed in ZSX3 v1. Incompatible layouts require another
explicit version/discriminator, not a reinterpretation of these bytes.

The first 16 bytes at offset 54 are ASCII `ZSL1`, `u32 LE U`, `u32 LE block
count`, `u32 LE zero flags`. The block count is `ceil(U / 12582912)`. Each
block has `u32 LE encoded-block length`, then `u32 LE original-block length`,
`u32 LE payload length`, `i32 LE literal count`, and the payload. A literal
count of `-1` denotes raw bytes; other blocks use the existing validated
ZsxLatest predictor/range coder. **Codec integers are little-endian**, unlike
the outer container and inner archive integers. The codec magic is unchanged.

## Exact inner record stream (existing ZSDP v1)

All integers below are unsigned big-endian, with no padding:

```text
4 bytes        ASCII "ZSDP"
u16            version = 1
u32            entry count (0..100000)
repeat entry count times:
  u8           kind: 0 = file, 1 = directory
  u16          UTF-8 path byte length
  bytes        relative path, '/' separated, no trailing slash
  u64          data byte length
  32 bytes     raw SHA-256 of this entry's data
  bytes        data (immediately followed by the next entry)
```

Directories have zero data length and SHA-256 of the empty byte string; an
empty file is distinct by kind. Paths are strictly increasing in the existing
Haxe string ordering, with case-insensitive collisions rejected. Input
collection sorts before encoding. Record ordering and bytes contain no clock
values. The whole-stream digest includes every header, path, digest and byte
of file content. The decoder also checks each file/directory digest.

## Bounds, validation and extraction

- Outer length fields use bounded u64 encoding: high 32 bits zero, low value
  at most 1 GiB (`1073741824`). The entire container is at most 1 GiB.
- Uncompressed data is further capped at 256 MiB (`268435456`), including
  record overhead. The inner header is at least 10 bytes.
- `C` must equal the actual file size minus 54; there are no trailing bytes.
  ZSL1's declared output size must equal `U` before decoding.
- ZSL1 validates flags, count, block/output sizes, range-stream termination
  and bounds. Its output length and SHA-256 must match the outer fields.
- Inner decoding checks version, count, kinds, exact end of stream, lengths,
  digests, ordering and path conflicts. Unsafe, absolute/traversal, reserved
  Windows paths, symlinks/reparse points and portable-name collisions are
  rejected by existing collection/extraction checks.
- `get`/`unpack` finish **all** container and record validation before creating
  an extraction destination or changing files. Staging/rollback follows that
  validation; full upfront validation is intentional even for late corruption.

These are bounded in-memory operations, not streaming backups. Transport has
its stricter existing 128 MiB container limit and independent chunk/tree bounds.
SHA-256 detects corruption; it is not a signature, authentication or encryption.

## Compatibility and transport

The prior Zsd archive was **ZSDC v1 wrapping ZSL1 over ZSDP v1**, not native
ZSX2. Its distinct `ZSDC` magic and established decoder remain readable. Its
54-byte field layout is retained for ZSX3, but new writes actually begin with
the new four-byte `ZSX3` magic and enforce the fixed v1 descriptors. There is
no legacy-encode CLI option. Old Zsd binaries that only recognize ZSDC will
not read ZSX3; use the updated reader.

Native **ZSX2** is a separate multi-file archive with independent per-file
compression. Zsd does **not** gain a native ZSX2 archive reader. Native sources,
ZSX1, ZSL1 and media codecs are not changed by ZSX3.

Short.io receives each data chunk and ZSDT metadata node as `originalURL` =
**`https://pex.wf/#` + canonical standard Base64(bytes)**. Literal `+`, `/`,
and `=` are retained, with no custom alphabet or URL escaping. Carrier URLs
are never visited. The shortening domain is independent of `pex.wf`.
The CLI now defaults to Short.io on the compiled hostname **pex.s.gy**:
`zsd upload file1 folder2`, then `zsd get ROOT_PATH` into the current directory.
The API credential is still required securely in `SHORT_IO_API_KEY`.
`--domain` overrides `SHORT_IO_DOMAIN`, which overrides the compiled default.
Full `zsd-sio-v1:domain:path` references use their own domain instead of the
environment/default; an explicit conflicting `--domain` is rejected.
Custom-domain uploads print full references; bare old is.gd IDs require
`--provider legacy`. `--output DIR` and `--force` remain available.
Invoking upload is consent to public, non-durable storage; no public-upload
flag or prompt is required. None of these CLI defaults change archive bytes.
Old Short.io `http://ok.com/?` carriers remain readable, and the legacy provider
retains its old carrier writer/reader. The `zsd-sio-v1` reference and `ZSDT`
metadata formats, 1450-byte Short.io chunks, 1000-link bulk cap, 3000 ms bulk
gate, retries and cancellation/deadline timers are unchanged.

## Offline regression coverage

```sh
haxe -cp . -main zsd.test.ZsdShortIoTest --interp
HAXELIB_PATH="$PWD/haxelib" ./tools/zsd/build-zsd.sh
mcs -sdk:4 -langversion:4 -out:/tmp/Zsd-verified.exe outputs/Zsd.cs
ZSD_EXE=/tmp/Zsd-verified.exe python3 zsd/test/cli_regression.py
```

Archive filesystem safety requires the C# target (not `--interp`). Build both
test harnesses using the same standalone .NET 4/C# 4 path:

```sh
for name in ZsdArchiveTest ZsdShortIoTest; do
  HAXELIB_PATH="$PWD/haxelib" haxe -cp . -main "zsd.test.$name" \
    -cs "/tmp/zsd-$name" -D net-ver=40 -D no-compilation -dce full
  ./tools/zsd/combine-cs.sh "/tmp/zsd-$name/src" "/tmp/$name.cs" "/tmp/$name.map"
  mcs -sdk:4 -langversion:4 -out:"/tmp/$name.exe" "/tmp/$name.cs"
  mono "/tmp/$name.exe"
done
python3 zsd/test/shortio_e2e.py /tmp/Zsd-verified.exe /tmp/ZsdShortIoTest.exe
```

The format tests compare the payload with one compression of the complete
record stream, exercise shared data in multiple files and empty entries, and
check wire fields, truncation, corruption, bounds and fail-before-extract.
`zsd/test/format_fixtures.py` independently constructs a reproducible pre-ZSX3
ZSDC/ZSL1 raw-block fixture for actual CLI extraction, without calling the new
writer or reading any generated C# or configuration.
`zsd/test/shortio_e2e.py` additionally tests the compiled .NET program with a
loopback-only Short.io mock: exact originalURLs, reconstructed ZSX3 bytes,
multiple roots/shared files, byte-exact restoration, and old-carrier reads.
It also removes `SHORT_IO_DOMAIN` from the child environment to verify compiled
provider/domain defaults, bare-ID output, current-directory extraction,
reference/env/flag precedence, collision/force safety, malformed IDs,
corrupt-container fail-before-write and symlink protection.
See [zsd/README.md](zsd/README.md) for its compiled harness invocation.