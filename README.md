# ZSX

ZSX is a lossless file compressor with compatible implementations in Haxe/JavaScript and portable C.

- The Haxe build provides the browser application, Web Workers, and the archive API.
- The C build provides a standalone command-line program for creating and extracting stored `.zsx` archives.
- Both implementations read and write the portable `ZSX1` archive format.

## Zsd: compressed upload and download (.NET Framework 4.0)

`Zsd.hx` is the command-line application written in Haxe. Its generated
[`outputs/Zsd.cs`](outputs/Zsd.cs) is one self-contained C# source file:
compile it with the .NET Framework 4.0 C# compiler, without Haxe or third-party
runtime DLLs on the destination machine.

```bat
cd outputs
C:\Windows\Microsoft.NET\Framework\v4.0.30319\csc.exe /target:exe /out:Zsd.exe Zsd.cs
Zsd.exe pack --output backup.zsd "C:\MyFolder"
Zsd.exe unpack backup.zsd --output restored
Zsd.exe upload --allow-public-upload "C:\MyFolder" "C:\Logs\*.txt"
Zsd.exe get ENTRY_ID --output restored
```

Uploads compress the collected files before storing small encoded URL entries;
downloads validate and decompress the archive before extraction. Folder structure
and empty directories are preserved. Quoted wildcards are expanded by Zsd.
See [Zsd usage, limits, and protocol](zsd/README.md) for details.

**Privacy and durability:** is.gd entries are public, are not encrypted, cannot
be deleted by their creator, and are not guaranteed storage. Public uploads
therefore require `--allow-public-upload`. Do not use this for confidential files
or as your only backup. The test suite uses a local mock service and makes no
public uploads.

To regenerate the C# file, install Haxe, `hxcs` (`haxelib install hxcs`), and Mono,
then run `./tools/zsd/build-zsd.sh`. The build validates both C# 4 syntax and the
.NET 4.0 reference assemblies. Source and runtime notices are embedded in the
generated file.

### Updated native ZSX compatibility

The latest supplied native C source is preserved verbatim in
[`reference/zsx-latest.c`](reference/zsx-latest.c). Its Haxe port is
[`ZsxLatest.hx`](ZsxLatest.hx), used by Zsd. The newer native format has a
different chunk header, so it is kept separate from existing `ZSX1` and ZSV
formats. New portable byte-stream encodes use `ZSL1`; old readers and media
codecs have not been replaced. See [format details and cross-language
tests](ZSX_LATEST_FORMAT.md).

## Archive compatibility

New archives use fixed-width little-endian fields, so they can be exchanged between 32-bit and 64-bit systems:

```text
4 bytes   "ZSX1"
u32 LE    original file length
u32 LE    chunk count
u32 LE    flags (currently zero)

for each chunk:
  u32 LE  encoded chunk length
  bytes   encoded chunk
```

Each uncompressed chunk is at most 12 MiB. A chunk contains its own 16-byte header:

```text
u32 LE    original chunk length
u32 LE    range-stream or raw-data length
i32 LE    literal count, or -1 for an uncompressed chunk
u32 LE    CRC placeholder (currently zero)
```

The Haxe decoder also accepts archives produced by the older C sample, which placed a native 32-bit or 64-bit `size_t` before each encoded chunk. The portable C program only writes `ZSX1`.

## Building the Haxe/JavaScript programs

### Requirements

- [Haxe](https://haxe.org/) 4.x
- Node.js for the Node test module and regression tests
- .NET 8 for the legacy `zsxe.js` finalization step in the complete build
- Python 3 only if you want to serve the static browser application locally

No additional Haxelib package is required for the JavaScript codec.

### Complete browser build

On Linux, macOS, or a Unix-like shell:

```sh
chmod +x build.sh
./build.sh
```

The build creates:

| Output | Purpose |
|---|---|
| `public/zsx.js` | Browser interface |
| `public/zsx_worker.js` | Archive decompression worker |
| `public/zsx_chunk_worker.js` | Reusable compression worker for one 12 MiB chunk |
| `public/zsxe.js` | Legacy finalized encoder bundle |
| `test/zsx_lib.js` | Node-compatible codec used by tests |

The equivalent core Haxe commands are:

```sh
haxe buildjs.hxml
haxe -m ZsxWorker -js zsx_worker.js
haxe -m ZsxChunkWorker -js zsx_chunk_worker.js
haxe buildnode.hxml
node test/patch_for_node.js
```

`build.sh` additionally builds the .NET finalizer, creates `zsxe.js`, and copies browser files into `public/`.

### Run the browser application

After building:

```sh
python -m http.server 5000 --directory public
```

Open `http://localhost:5000/`. Compression downloads `original-name.zsx`; decompression removes the `.zsx` suffix from the downloaded filename.

Browser compression reads the source through `File.slice()` and transfers independent chunks to reusable workers. The worker count and accepted input size are limited according to reported device memory.

## Building the C program

`zsx.c` is a self-contained C11 program. It does not require Haxe or Node.js.

### Linux and macOS

Using the Makefile:

```sh
make
```

Or directly:

```sh
cc -O2 -Wall -Wextra -Wpedantic -std=c11 zsx.c -o zsx
```

### Windows with MinGW-w64

From a MinGW-w64 shell:

```sh
gcc -O2 -Wall -Wextra -Wpedantic -std=c11 zsx.c -o zsx.exe
```

The Windows implementation uses binary file mode and `MoveFileEx` when replacing a completed destination.

### C command-line usage

Compress a file:

```sh
./zsx input.bin
```

This creates `input.bin.zsx`. An explicit destination may be supplied:

```sh
./zsx input.bin archive.zsx
```

Decompress a stored archive:

```sh
./zsx -d archive.zsx
```

The default output removes the final `.zsx` suffix. If the input has no `.zsx` suffix, `.out` is appended. An explicit destination may also be supplied:

```sh
./zsx -d archive.zsx restored.bin
```

Show command help:

```sh
./zsx --help
```

Compression and decompression are separate operations. The CLI writes a persistent archive and does not immediately decode it as an internal round-trip.

## Compression process

### 1. Split the file into independent chunks

The file is divided into chunks of at most 12 MiB. Independent chunks provide:

- bounded codec memory;
- corruption boundaries;
- browser compression in parallel workers;
- streaming file I/O in the C CLI.

Predictor state is reset at every chunk boundary.

### 2. Build a frequent-pair dictionary

The encoder counts aligned two-byte values and ranks the most frequent pairs. Pairs occurring fewer than the minimum threshold are discarded, and at most 255 ranks are retained.

A retained pair is represented as one token. Other input bytes remain literal tokens. The ranked dictionary is stored at the beginning of the range stream so the decoder can rebuild the same token mapping.

### 3. Predict the next token

ZSX grows its models while processing tokens. Nothing model-specific has to be stored in the archive.

For every token, the encoder records whether it is a pair rank or a literal byte and then tries two deterministic predictions:

1. A hashed order-3 context predicts from the three preceding tokens.
2. If that prediction misses or has no history, an exact order-2 table predicts from the two preceding tokens.

Only the hit or miss decision is range-coded when a prediction exists. A successful prediction therefore does not require the token itself to be written.

### 4. Encode unpredictable tokens

When both predictors miss, the token is encoded one bit at a time through an adaptive binary tree. A deeper tree is selected using recent context, while a shallower byte model supplies initial probabilities for contexts that have not been visited.

All probabilities use 11-bit fixed-point integers. After every decision, the selected probability moves one-sixteenth of the remaining distance toward the observed bit. Encoder and decoder perform the same updates in the same order.

### 5. Range-code the decisions

The adaptive decisions and literal bits are written into one binary range-coded stream:

- a 32-bit range identifies the current interval;
- a 64-bit low value preserves carry information;
- pending `0xFF` bytes are resolved by carry propagation;
- five final carry steps terminate the stream.

The decoder rejects truncated streams, appended data, inconsistent literal counts, invalid pair ranks, and noncanonical terminal states.

### 6. Use raw fallback when compression does not help

If the range-coded payload is not smaller than the original chunk, ZSX stores the chunk verbatim and writes a literal count of `-1`. Incompressible input therefore grows only by archive and chunk framing rather than by a larger coded representation.

### 7. Assemble the portable archive

The encoded chunks are placed in order under the `ZSX1` archive header. The original length and expected chunk count let the decoder validate the archive before allocating or producing the final output.

## Decompression process

For each chunk, the decoder:

1. validates archive and chunk lengths;
2. copies a raw-fallback chunk directly, or initializes the range decoder;
3. restores the frequent-pair dictionary;
4. recreates the same adaptive prediction models from decoded decisions;
5. expands pair-rank tokens back to their original two bytes;
6. verifies the declared output length, literal count, and range-stream termination.

Chunks are concatenated in archive order. The C program writes decoded chunks incrementally. The browser validates the complete archive and enforces its configured output limit before allocating the output buffer.

## Safe output handling

The C CLI writes to a newly created temporary file beside the requested destination. It replaces the destination only after the whole operation, validation, and file close succeed.

This prevents a malformed archive or failed compression from destroying an existing destination. Input/output aliases through hard links or symbolic links are rejected.

## Tests

Build both implementations and run their compatibility tests:

```sh
make test
```

The suite covers:

- byte-for-byte compatibility with the original C chunk fixture;
- C-to-Haxe and Haxe-to-C stored archives;
- empty, compressed, raw-fallback, and multi-chunk files;
- 32-bit and 64-bit legacy C framing in the Haxe decoder;
- malformed, truncated, padded, and trailing archive data;
- default output filenames and destination-preservation behavior;
- input/output hard-link and symbolic-link aliases.

Clean the C executable:

```sh
make clean
```

## Current limits

- Portable `ZSX1` stores the total original length as a 32-bit unsigned value.
- Browser decoding currently limits restored output to 256 MiB.
- Browser compression applies lower device-dependent safety limits.
- Each individual chunk is at most 12 MiB.
- The CRC field is reserved and must currently be zero.