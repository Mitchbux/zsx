# Latest attached-C codec (`ZsxLatest`)

`ZsxLatest` is an explicit, separate codec API:

```haxe
ZsxLatest.encode(data:haxe.io.Bytes):haxe.io.Bytes
ZsxLatest.decode(data:haxe.io.Bytes):haxe.io.Bytes
```

It exists because the supplied C version (preserved in `reference/zsx-latest.c`) is **not** the same
format as root `zsx.c`.  Root `ZSX1` and `ZsxRange` remain unchanged, including
their hardened 16-byte chunk header and CRC-placeholder field.  Call
`ZsxArchive.decode` for `ZSX1`; `ZsxLatest.decode` rejects a `ZSX1` magic
instead of guessing or silently changing formats.  `ZSX2` is the attached
program's multi-file native archive and is also not a `Bytes -> Bytes` stream.

## Delta verified against root `zsx.c`

The attached source changes a chunk header from four native `int` fields to
three:

```text
root:     original length, payload length, literal count, CRC placeholder
attached: original length, payload length, literal count
```

Thus a latest chunk has a 12-byte header, not a 16-byte one.  It also increases
the C work buffer from 32 MiB to 64 MiB, uses the latest source's native
little-endian `ushort` aligned-pair counting, and removes the root decoder's
range-stream truncation/canonical-end checks.  The latter checks are
intentionally retained by `ZsxLatest`; accepting zero-filled, incomplete C
streams is not safe interoperability.

## New portable `ZSL1` frame

`encode` always writes this versioned framing rather than the C program's
native `size_t` framing:

```text
4 bytes       ASCII "ZSL1"
u32 LE        uncompressed byte length
u32 LE        chunk count
u32 LE        flags (zero)
repeated:
  u32 LE      encoded latest-chunk length
  bytes       latest chunk
```

Uncompressed chunks are at most 12 MiB.  A latest chunk is:

```text
u32 LE        uncompressed chunk length
u32 LE        payload length
i32 LE        literal count (-1 means raw payload)
bytes         payload
```

The predictor and range coder are a direct portable port of the attached C
codec.  The pair dictionary uses little-endian two-byte values, as it does on
the supplied Windows C implementation.  The port uses only Haxe `Bytes`,
`BytesBuffer`, `Array`, integer operations, and `Float` for exact unsigned
32-bit range arithmetic; it has no JavaScript typed-array or `Math.imul`
dependency and compiles for JS and C# (`-D net-ver=40`).

## Native C input accepted by `decode`

For migration, `decode` also accepts the well-defined subset emitted by the
attached program's simple file mode:

```text
native little-endian size_t encoded-chunk length
latest 12-byte chunk
... repeated until EOF
```

Both 32-bit and 64-bit `size_t` length prefixes are recognized when every
length is below 2 GiB.  Empty native streams are rejected because they have no
width marker; use the unambiguous empty `ZSL1` frame.  Native framing is never
emitted by `encode`.

All `ZSL1` and native inputs are checked before allocation/decompression:
truncated headers and payloads, inconsistent lengths/counts, trailing bytes,
out-of-range chunks, invalid ranks, noncanonical range tails, and output above
256 MiB fail with an exception.

## Regression coverage

```sh
haxe test_latest.hxml
node test_zsx_latest.js
sh test/zsx_latest_c_cross.sh
```

`TestZsxLatest` covers empty, odd-length, deterministic random, repeated,
native-32/native-64, truncated, and corrupt inputs.  The cross script extracts
only the codec portion of the attached Windows source, removes the UI-only
Windows include, builds that isolated C11 harness, then verifies C-to-Haxe
decode, byte-identical compressed chunks, and Haxe-to-C decode.