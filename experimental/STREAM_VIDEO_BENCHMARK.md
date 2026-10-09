# Streaming ZSX video experiment

This is an **experimental evaluation**, not a replacement for the native or
browser video decoder. Existing production codecs, media files, and players
have not been changed.

## Reproduce

```sh
python3 experimental/bench_stream_video.py \
  attached_assets/zsx-stream-candidates-windows_1791563114539.zip \
  outputs/demo-small-30s.zsv outputs/zsv-player-tests/sample.zsv \
  --packets 150 --output outputs/stream-video-benchmark.json
```

The script extracts only the uploaded codec and model into temporary storage,
replaces Win32 allocation-counter locking with single-threaded benchmark
stubs, and builds Linux shared libraries with `cc -O2`. It does not execute
the Windows executable, import its archive/filesystem implementation, or
modify production codecs.

The existing production decoder recovers coefficient/RLE bytes from actual
ZSXD packets. Each alternative compresses and decodes those identical bytes;
every decoded result is compared byte-for-byte and tracked allocations must
return to zero. Prediction resets remain at existing chunk boundaries.

## Measured results

All sizes below include the 16-byte entropy chunk headers. They exclude the
unchanged ZSXD/NUT framing and audio: these are **not whole-video size savings**.

| Recording | Frames | Current chunks | Uploaded byte control | Uploaded two-candidate | Two-candidate, 16-bit hash |
|---|---:|---:|---:|---:|---:|
| demo-small-30s | 150 | 188,797 | 181,124 | 176,086 | 176,081 |
| player-test sample | 72 | 622,712 | 537,340 | 540,232 | 540,781 |

The uploaded two-candidate model reduces coefficient chunk storage by **6.73%**
and **13.25%**, respectively. The byte control beats the two-candidate model
on the second recording, so the candidate predictor is not universally best.

An experimental smaller hash (`-Dzsx_mc=16`, versus the supplied 24) preserves
nearly all of that compression on these samples while substantially reducing
allocation size and CPU time:

| Measurement | Uploaded two-candidate | 16-bit variant |
|---|---:|---:|
| Demo encode CPU seconds | 0.2842 | 0.0929 |
| Demo decode CPU seconds | 0.2898 | 0.0936 |
| Test encode CPU seconds | 0.4451 | 0.1566 |
| Test decode CPU seconds | 0.4608 | 0.1684 |
| Maximum tracked bytes, including buffers | 102,193,824 | 35,347,104 |

These are single-run, in-process Linux CPU timings, including model allocation;
they are not native Windows timings, playback latency, RSS, or comparisons
against the production encoder's speed. More diverse clips and repeated runs
are needed before selecting a default.

## Integration constraints

* The uploaded stream is not the production ZSX1 entropy format. Its fourth
  header word is a pass count, whereas the production bridge requires its
  reserved/CRC word to be zero. Never reinterpret the same header silently.
* Hash-size variants are also mutually incompatible unless the profile is
  explicitly identified in the encoded stream.
* The uploaded byte decoder substitutes zero beyond its input and the candidate
  decoder does not check all range-stream failure conditions. Its allocator
  exits the process on failure. These behaviors must be hardened before
  accepting untrusted video in a native library or browser.
* A production integration must update both native and WASM decoders, retain
  old-media support and whole-recording validation, and benchmark complete
  output packets including any new metadata. Keep a baseline fallback.
* The uploaded model's approximately 97 MiB allocation is significant relative
  to the current browser module's 128 MiB maximum, before frame buffers.
  Do not adopt it merely on the strength of its text/enwik8 benchmark.

Machine-readable measurements and the input ZIP digest are in
`outputs/stream-video-benchmark.json`.
