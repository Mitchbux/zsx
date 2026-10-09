# Video entropy profiles

ZSXD video version **1** remains read-only compatible and accepts only production
ZSX1 chunks. New encodes use ZSXD version **2**, in both extradata and every
packet. The decoder requires these versions to agree. Audio is unchanged.
Packet integrity CRC and reconstructed-frame CRC still cover every frame.
Browser playback still validates the entire recording before reporting metadata
or enabling playback; the worker's bounded-buffer protocol is unchanged.

Version 2 accepts either an unchanged ZSX1 baseline chunk or a ZSE1 chunk.
ZSX1 begins with an original length at most 12 MiB, so it cannot collide with
the four-byte ASCII `ZSE1` discriminator. A version-1 decoder never dispatches
to ZSE1. Unknown profiles, flags, versions, lengths and pass counts fail.

## ZSE1 envelope (all integers little-endian uint32)

| Offset | Meaning |
|---|---|
| 0 | ASCII `ZSE1` (entropy envelope version 1) |
| 4 | Profile: 1 byte/16-bit hash; 2 byte/20-bit; 3 two-candidate/16-bit; 4 two-candidate/20-bit |
| 8 | Original byte length, at most 12 MiB |
| 12 | Exact payload length |
| 16 | Literal miss count, or `0xffffffff` for stored bytes |
| 20 | Pass count; must be exactly 1 |
| 24 | Payload |

These profiles preserve the uploaded predictors' probability updates and hash
arithmetic. They do **not** implement uploaded ZSC2 archive framing, recursive
passes, or its 24-bit default hash. Production ZSX1's fourth word remains zero;
it must never be interpreted as the uploaded pass count.

The encoder tries ZSX1, byte/16, and two-candidate/16 sequentially, comparing
complete chunks including envelopes. Ties retain the baseline. Optional-trial
allocation failure retains an already-valid result. Profiles 2 and 4 are
explicit library options with decoding support, not default encoding trials.
No packet grows beyond the baseline's raw length + 16-byte maximum.

## Safety and ownership

The portable streaming library uses checked allocations, no process exits,
no global predictor state, strict range EOF/initial/terminal-code checks and
exact literal counts. Return values are 0, -1 (invalid) and -2 (OOM); error
returns leave caller outputs untouched. Streaming outputs use ordinary `free`.
The bridge's outputs instead use `zsx_ffmpeg_free`.

Compile the legacy core with `-DZSX_LIBRARY`: the FFmpeg bridge supplies a
thread-local allocation scope capped at 112 MiB per call and recovers via
setjmp from legacy unchecked allocations, freeing partially built tables.
This excludes standalone CLI/archive features from the media library.
New models require under 39 MiB of tables; trials run sequentially. Total
playback memory additionally includes compressed input, frames, audio and
FFmpeg state. WASM retains its 128 MiB maximum with non-aborting malloc.

## Verification

```
sh ffmpeg-zsx/tests/stream-safety.sh
python3 ffmpeg-zsx/tests/stream_profiles.py outputs/demo-small-30s.zsv outputs/zsv-player-tests/sample.zsv
node artifacts/zsv-browser-player/tests/wasm-native-parity.mjs
ZSV_PARITY_SAMPLE=/tmp/stream-v2.zsv node artifacts/zsv-browser-player/tests/wasm-native-parity.mjs
ZSV_PARITY_SAMPLE=/tmp/stream-v2.zsv node artifacts/zsv-browser-player/tests/worker-streaming.mjs
```

The safety suite uses ASan/UBSan and fails each allocation in turn, checks
truncation even with adjusted length headers, rejects invalid profiles and
range termination, verifies leak-free recovery and optional-trial fallback.
The profile suite compiles the uploaded predictors as isolated test oracles
and checks all four profiles byte-for-byte on real coefficient packets.

Measured complete ZSXD packet totals (unchanged audio/container excluded):

| Input | Frames | Baseline bytes | Selected bytes |
|---|---:|---:|---:|
| demo-small-30s | 150 | 195397 | 183876 |
| player-test sample | 72 | 625880 | 540230 |

These are 5.90% and 13.68% packet savings, not whole-file claims.
