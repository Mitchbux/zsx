# ZSX

ZSX is a lossless file compressor with compatible implementations in Haxe/JavaScript and portable C.

- The Haxe build provides the browser application, Web Workers, and the archive API.
- The C build provides a standalone command-line program for creating and extracting stored `.zsx` archives.
- Both implementations read and write the portable `ZSX1` archive format.

## Source components

- **Zsd / ZSX3:** [`zsd/README.md`](zsd/README.md), root `Zsd.hx`, and
  `tools/zsd/`. The public standalone C# source is `outputs/Zsd.cs`.
  Credentials are not included; private key-embedded builds must remain private.
- **Haxe Shrink:** `Shrink.hx`, `BitReader.hx`, `BitWriter.hx`, the other root
  Haxe sources, build configurations, and regression tests under `test/`.
- **FFmpeg / ZSV2:** [`ffmpeg-zsx/README.md`](ffmpeg-zsx/README.md) documents
  the custom codec sources, FFmpeg 6.1.2 patches, builds, and tests.
  See [`ffmpeg-zsx/LINUX_PACKAGE_README.md`](ffmpeg-zsx/LINUX_PACKAGE_README.md)
  for H.264/AAC MP4 conversion and the portable Linux build.

FFmpeg codec binaries and large demo media are generated separately, not
committed here. The source build verifies and downloads the pinned FFmpeg
release before applying the included patches.

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
Zsd.exe upload "C:\MyFolder" "C:\Logs\*.txt"
Zsd.exe get ENTRY_ID
```

New packs/uploads write **ZSX3 v1**: all files are concatenated into one
deterministic tar-like ZSDP v1 record stream (not POSIX tar), then compressed
once with the existing ZSL1 byte-stream codec before storing small URL entries.
Legacy ZSDC v1 archives remain readable; native ZSX2 is a separate per-file
archive format, not a Zsd input format. See [ZSX3 wire bytes](ZSX3_FORMAT.md).
Short.io `originalURL` values are exactly `https://pex.wf/#` plus standard Base64;
the configured shortening domain remains separate, and old carriers remain readable.
Downloads validate and decompress the archive before extraction. Folder structure
and empty directories are preserved. Quoted wildcards are expanded by Zsd.
See [Zsd usage, limits, and protocol](zsd/README.md) for details.

**Defaults (shareable build):** Short.io on **pex.s.gy**, with `SHORT_IO_API_KEY`
supplied securely in the runtime environment (not embedded). `zsd upload file1 folder2`
prints a bare root path, usable as `zsd get SAME_ID`. `get` extracts into the
**current directory**, retaining unrelated files; overwrites require `--force`.
Use `--output DIR` to choose another destination. Domain priority is explicit
`--domain` > `SHORT_IO_DOMAIN` > compiled `pex.s.gy`, so standalone binaries need
no domain environment variable. Custom-domain uploads print full
`zsd-sio-v1:domain:path` references: these use their own domain, ignoring env/default
domain, and reject a conflicting explicit `--domain`. Bare old is.gd IDs need
`--provider legacy`; old full Short.io references and old carriers remain readable.

**Privacy and durability:** URL entries are public, not encrypted, and not
guaranteed storage. Invoking `upload` is consent: there is no blocking prompt or
required public-upload flag (the old flag remains accepted for compatibility).
Do not use this for confidential files or as your only backup. The test suite
uses local mock services and makes no public uploads.

To regenerate the C# file, install Haxe, `hxcs` (`haxelib install hxcs`), and Mono,
then run `./tools/zsd/build-zsd.sh`. The build validates both C# 4 syntax and the
.NET 4.0 reference assemblies. Source and runtime notices are embedded in the
generated file.

An explicitly requested **private** source/executable build is also available:
`bash tools/zsd/build-private-zsd.sh`. It consumes the already-configured
`SHORT_IO_API_KEY` only at build time and writes `privateZsd.cs`, `Zsd.exe`, and
`zsd-private-net40.zip` beneath the git-ignored `outputs/private-zsd/`.
Both private source and executable deliberately contain the literal credential;
any recipient can extract and use it. Keep them private; never upload them via Zsd.
The private executable does not look up or fall back to a runtime API-key variable.
Changing the environment or rebuilding does not revoke keys in older copies:
revoke/rotate the old key at Short.io, then rebuild and replace private copies.
The script also regenerates the separate, credential-free
`outputs/zsd-net40-source.zip` from an explicit public file list and checks every
entry before replacement. See [private build details](zsd/README.md#private-embedded-credential-build).

### Updated native ZSX compatibility

The latest supplied native C source is preserved verbatim in
[`reference/zsx-latest.c`](reference/zsx-latest.c). Its Haxe port is
[`ZsxLatest.hx`](ZsxLatest.hx), used by Zsd. The newer native format has a
different chunk header, so it is kept separate from existing `ZSX1` and ZSV
formats. New portable byte-stream encodes use `ZSL1`; old readers and media
codecs have not been replaced. See [format details and cross-language
tests](ZSX_LATEST_FORMAT.md).

## Archive compatibility

### Experimental `.zxh`: trusted computed word/letter reconstruction

This **native-only**, enwik8-oriented experiment uses a distinct `ZXH1` format,
not `ZSX1` or `ZSH1`. It does not change normal compression, `-h`, or `-d`.
`zsx -d` deliberately rejects it. Browser/Haxe code is unchanged.

The bundle contains **generated computational C (including its numeric frequency
model), one small external word dictionary, and ONE packed arithmetic correction
stream**. "External dictionary" means outside the C section but **inside the same
archive**, not a file needed at decode time. No original document, generator
template, sidecar plan, hash, or verification metadata is needed to reconstruct.
Case choices, prediction hit/miss decisions, word substitutions and escaped
bytes are all interleaved into the single correction stream.

**Security:** `-x --trust-generated-code` compiles and executes archived C with
real libtcc. It is **not sandboxed**. Buffer/framing checks protect against
ordinary format errors, not malicious C. Do not execute untrusted archives.
Without checksums, a mutation that remains structurally valid need not be
detected. Validate files with external hashes when necessary.

#### Build and reproduce

Ordinary portable builds still require only a C compiler:

```sh
make -B zsx CFLAGS=-O2                 # -x reports "unsupported"
```

Opt-in libtcc build on Debian/Ubuntu (matching development header/library):

```sh
sudo apt-get install build-essential python3 tcc libtcc-dev
make -B zsx CFLAGS=-O2 WITH_TCC=1
```

The installed Nix TinyCC here is **2025-01-06**, with a one-argument
`tcc_relocate`. The native host includes the actual `libtcc.h`; it recognizes
older two-argument versions using that header's `TCC_RELOCATE_AUTO`, rather than
guessing declarations or substituting a system compiler at extraction.
Exact build for the currently installed environment:

```sh
TCC_INCLUDE=/nix/store/h8yfzbw8xarzim81hlv840rhhafhvjgh-tcc-0.9.27-unstable-2025-01-06-dev/include
TCC_LIB="$(tcc -print-search-dirs | sed -n 's/^install: //p')/.."
make -B zsx CFLAGS=-O2 WITH_TCC=1 \
  TCC_CPPFLAGS="-I$TCC_INCLUDE" \
  TCC_LDLIBS="-L$TCC_LIB -Wl,-rpath,$TCC_LIB -ltcc -ldl"

mkdir -p outputs/zxh-frequency
python3 experimental/zxh_generate.py enwik8 outputs/zxh-frequency/enwik8.zxh \
  --words 4096 --trained-contexts --report outputs/zxh-frequency/generated.json
./zsx -x --trust-generated-code outputs/zxh-frequency/enwik8.zxh \
  outputs/zxh-frequency/enwik8.out
sha256sum enwik8 outputs/zxh-frequency/enwik8.out

python3 experimental/test_zxh.py outputs/zxh-frequency/tests.json
python3 experimental/benchmark_zxh.py  # three dictionary candidates + one trained refinement
# To retain a previous experiment's exact measurements and refine only that result:
# python3 experimental/benchmark_zxh.py --refine-existing
```

Use `make -B` when changing build options, since make does not track command-line
flags as dependencies. The opted-in executable links libtcc; the default build
does not. Generation uses `cc` only to accelerate encoding. Extraction uses
libtcc exclusively. Both empty input and arbitrary binary/UTF-8 input up to
100,000,000 bytes are lossless; only ASCII letter runs participate in the word
dictionary. No Unicode normalization is performed. Inputs/outputs must differ;
hard-link/symlink aliases are rejected and outputs are atomically replaced after
successful reconstruction.

#### What actually predicts

The generator counts corpus-wide lowercase word frequencies, selects frequent
spellings under a **32 KiB dictionary budget**, and counts the corpus-wide byte
frequencies of escaped/non-dictionary content. It emits a smoothed global
word/byte cumulative-frequency table as **counted C/model source**, not hidden
payload. Three shared adaptive context tables learn the last observed next token
at orders 1, 2 and 3 (higher orders are hashed). Tokens can be dictionary words
or individual bytes. There are no per-position generated instructions.

The winning **`--trained-contexts`** refinement additionally trains an offline
prior-two-token model. Each exact context retains its four most frequent next
tokens and counts; a fifth outcome escapes to the adaptive predictors. Contexts
must occur at least 64 times, with a fixed maximum of 65,536 rows ranked by
frequency. This is one tested model configuration, not a parameter sweep.
The full corpus retained **22,977 rows**. All context keys, predictions, counts
and totals are in the generated C and included in its size. They are aggregate
statistics, not output positions or a serialized instruction stream.
Generation temporarily sorts observed triples to train these statistics; that
temporary training data is not archived or needed at decode. Omit the flag to
use only the original adaptive/global model.

The archived C's actual loop is structurally:

```c
/* Binary-search trained[] by older*N + previous, then arithmetic-decode
   one of its four predicted tokens or escape using the counted frequencies. */
for (level = ORDER; miss && level > 0; level--) {
    k = level - 1;
    if (!seen[k][contexts[k]]) continue;
    prediction = next[k][contexts[k]];
    /* Skip predictions already excluded by the trained model's escape. */
    miss = binary(actual != prediction, prob[k][contexts[k]]);
    if (!miss) { s = prediction; hits++; break; }
}
/* If every prediction misses, arithmetic-decode using the global CDF. */
while (left + 1 < right) {
    middle = (left + right) / 2;
    if (cdf[middle] <= t) left = middle;
    else right = middle;
}
/* Update shared contexts, emit dictionary word or escaped byte,
   and consume interleaved case corrections. Repeat until wanted bytes. */
```

`actual` is supplied only when encoding; on extraction `binary()` reads the
arithmetic-coded decision. The generated `reconstruct()` itself executes these
loops, global-frequency lookup, adaptive updates, dictionary expansion and case
restoration. The fixed native host only unpacks sections, compiles the C,
allocates buffers, invokes it, and commits output.

The **inner entropy coder** is a 32-bit arithmetic coder shared by all events
and case corrections. This is separate from, and does not modify, the existing
outer ZSX coder. Each of the three sections is stored as ordinary `ZSX1` only
if smaller than raw. The residual is already entropy-coded and remained raw
in the measured archives. There are no separate length/index/case streams.

#### Measured result — not a 1 MiB total archive

Exact reproducible data, counts, prediction rates, timings and independent test
results are in [`experimental/zxh_enwik8.json`](experimental/zxh_enwik8.json).
Candidate selection compares **complete** archives, not nominal token widths:

| Requested dictionary words | Complete first-1-MiB archive |
| ---: | ---: |
| 512 | 440,130 bytes |
| 2,048 | 423,453 bytes |
| 4,096, original adaptive/global model | 420,884 bytes |
| 4,096, **one trained-context refinement** | **398,543 bytes** |

The first three measurements are the preserved original implementation's
dictionary selection. The trained candidate kept that dictionary configuration
and improved complete prefix size by **22,341 bytes**, so it was evaluated once
on full enwik8. Its 1,134 trained prefix rows cost 31,465 stored source bytes;
its single prefix residual was 348,225 bytes. No additional model-budget sweep
was performed.

The selected full **100,000,000-byte enwik8** bundle contains:

| Component | Raw bytes | Stored bytes |
| --- | ---: | ---: |
| Generated C including numeric models | 951,067 | 297,855 |
| Dictionary: 4,029 words | 32,768 | 18,826 |
| **One** arithmetic correction stream | 33,476,766 | 33,476,766 |
| Framing | 44 | 44 |
| **Final `.zxh`** | | **33,793,491** |

Of the raw source, 944,048 bytes are model declarations (913,748 contextual,
30,300 global) and 7,019 bytes are shared algorithm code. They are compressed
together and fully charged to the source section. The compiled model tables'
binary equivalent is 936,224 bytes, not an additional archive section. Adaptive
runtime tables are learned, not hidden serialized data. The compressed code/model
meets the **at-most-1-MiB budget** without padding.

The trained model is the winner: **4,784,737 bytes smaller** than the original
38,578,228-byte ZXH experiment, and **3,485,154 bytes smaller** than the prior
37,278,645-byte computed experiment. The final bundle remains **935,036 bytes
larger** than the supplied 32,858,455-byte baseline. The hoped-for tiny correction
stream **was not achieved**.

Full-input prediction hits improved from 55.99% to **73.89% overall**, 20.51% to
**40.17% dictionary-word events**, and 64.22% to **81.71% escaped-byte events**
(the report's `letter_events` includes punctuation and binary bytes). The trained
model itself handled 59,876,972 lookups with 39,116,155 hits and 20,760,817 escapes;
the adaptive/global fallback handles the remaining cases in the same residual.
Generation took about **68.13 s**, including offline context training/compilation;
traced native extraction took **14.67 s**. This is a bounded experiment, **not a
smallest-possible claim**.

`outputs/zxh-frequency/enwik8.zxh` is the selected trained winner, not the older
archive. Its external SHA-256 is
`81e624ee77f034f8451031d0459fcb8b8e693df6fbf211968daf47f97b5a8387`.
The JSON's `previous_model_full` preserves the original measurements,
`trained_refinement` records the single refinement, and `full`/`selected_model`
identify the winner. The old best archive is retained as
`outputs/zxh-frequency/original-model-best.zxh`.

Independent full-output SHA-256 matched
`2b49720ec4d78c3c9fabaee6e4179a5e997302b3a70029f30f2d582218c024a8`.
`strace` extraction from an unrelated empty directory observed no original,
generator, C-template or header reads. Tests cover real libtcc source authority,
prediction hits/misses, case/binary/Unicode/empty inputs, dictionary budgets,
trained model hits/escapes and table/source bounds, structural corruption,
trust opt-in, aliases, atomic preservation, portable builds,
normal/high CLI regressions and existing C/Haxe cross-compatibility.

Wire format: `ZXH1`, little-endian `u32 output_bytes`, then three
`(u32 raw_bytes, u32 stored_bytes, u32 mode)` descriptors (source, dictionary,
residual), followed by their payloads. Mode 0 is raw, mode 1 is an entire ordinary
`ZSX1` archive. Total framing is 44 bytes. Dictionary entries are a one-byte
length and lowercase ASCII spelling, in frequency-model token order. Source
exports `int reconstruct(unsigned char *residual, unsigned residual_bytes,
unsigned char *dictionary, unsigned dictionary_bytes, unsigned char *output,
unsigned wanted)`. There is deliberately no duplicate instruction plan or
stored validation checksum. See `experimental/zxh_codec.c` and `zsx_x.h`.

### Native high mode: character-frequency word hashes

The portable native CLI supports `zsx -h INPUT [OUTPUT]`. Bare `zsx -h` and
`zsx --help` still show help. This is a reversible word-dictionary transform,
not the supplied Windows sample's repeated compression of already-coded bytes:

1. Count character/byte frequencies across the whole original chunk.
2. Find recurring words and assign compact hashes derived from their characters.
3. Emit hash tokens for dictionary hits. On a hash collision, emit an explicit
   dictionary-reference event that replaces the slot's mapping; the decoder
   performs exactly the same replacement.
4. Pass the **entire** dictionary, hash-token stream, and collision events through
   the unchanged baseline ZSX predictor.
5. Compare full encoded chunk sizes. Keep ordinary ZSX if the hash candidate is
   not strictly smaller.

```sh
make
./zsx -h document.txt document.high.zsx
./zsx -d document.high.zsx restored.txt
```

An archive using any hash-transformed chunk has a distinct `ZSH1` magic. Its
16-byte archive header is otherwise the same as `ZSX1`; bit 31 of each encoded
chunk-length field marks hash-transformed chunks. The stored chunk is still
baseline ZSX compressed data, decoded first and then passed through the inverse
dictionary transform. Unmarked chunks decode normally. When no hash candidate
wins, the result is byte-identical to ordinary `ZSX1`.

This mode currently belongs to the **native CLI**. Older ZSX1-only readers,
the browser codecs, and Zsd do not decode `ZSH1`; their existing formats and
behavior are unchanged. Hashes are not assumed to outperform sequential
indices: `experimental/high_dictionary_bench.c` compares both transforms
after the same baseline coder, including all metadata and collision overhead.

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