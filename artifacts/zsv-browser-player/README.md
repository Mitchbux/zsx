# ZSV browser player

The current demo uses the full snowboard recording, approximately 271.6 seconds,
center-cropped to 270×480 portrait grayscale at 25 fps with stereo 44.1 kHz audio.
Video uses DCT-ZSX quality 70 and the CPU-friendly candidate16 entropy profile
(`-zsx_entropy 3`); audio uses MDCT-ZSX with `-zsx_step 0.016`.
See [sample encoding instructions](public/sample/README.md) and the
[mobile benchmark](../../experimental/MOBILE_VIDEO_BENCHMARK.md).

## Playback

- Click **Load Sample** to begin streaming; playback starts after 30 seconds of
  decoded audio and video are ready (or the remaining duration for a shorter file).
- Controls overlay the video, with centered play/pause and a 5 px seek track.
  They hide after two seconds and reappear on pointer, touch, or keyboard activity.
- Fullscreen, volume, seeking, and local ZSV2 files are supported.
- Web Audio schedules sound independently of video rendering. Buffered sound
  continues during video stalls; actual audio depletion requires rebuffering.
- There is no whole-file preflight validation. Packet integrity and decoder
  checks remain, but late corruption can fail after partial playback.

## Build and run

From the repository root, use Node.js 24 and pnpm:

```sh
pnpm install --frozen-lockfile
pnpm --filter @workspace/zsv-browser-player typecheck
PORT=18103 BASE_PATH=/ pnpm --filter @workspace/zsv-browser-player build
PORT=18103 BASE_PATH=/ pnpm --filter @workspace/zsv-browser-player dev
```

The generated Haxe JavaScript and WASM decoder are included. To rebuild them,
use Haxe 4.3.6 and Emscripten 4.0.8 respectively:

```sh
pnpm --filter @workspace/zsv-browser-player build:haxe
sh artifacts/zsv-browser-player/decoder/build-wasm.sh
```

The decoder build downloads pinned FFmpeg 6.1.2 and applies the repository's
custom codec patches. Codec license notices are in `decoder/LICENSES.txt`.

For the demo API:

```sh
pnpm --filter @workspace/api-server build
PORT=8080 pnpm --filter @workspace/api-server start
```

Replit artifact manifests route `/` to the player and `/api` to the API.
Outside Replit, configure a same-origin reverse proxy for `/api`; Vite alone
does not proxy that route.

## Demo storage and publishing

The 59,720,317-byte full demo is **not stored in GitHub**. In Replit App Storage,
upload the encoded file under `public/zsv/snowboard-full-gray-25fps.zsv`
and configure `DEFAULT_OBJECT_STORAGE_BUCKET_ID` for the API environment.
The supplied storage client uses Replit's managed sidecar authentication;
it is not a standalone local-cloud credential setup.

`GET /api/demo/snowboard-full.zsv` streams the stored file. Keep this response
chunked, without `Content-Length`, and do not buffer it in a reverse proxy.
The earlier fixed-length response failed on the published endpoint while
working in preview. The chunked-response change passes a full byte-for-byte
download check in preview; live verification requires republishing.

Changing GitHub does not itself republish the Replit site.

## Memory and seeking

Encoded input is limited to 64 MiB; its incremental byte cache supports replay.
WASM memory is capped at 256 MiB. The rolling decoded queue is capped at
192 MiB / 8192 records, with pull batches of at most 4 MiB / 128 outputs.
Grayscale frames transfer luma only. Web Audio buffers and canvas storage are
additional allocations, not part of the decoded-queue budget.

Seeking restarts decoding from the cached stream beginning to preserve codec
state; late seeks can take tens of seconds. Seek generations discard obsolete
results. File replacement terminates the old worker and audio context.

## Verification

```sh
pnpm --filter @workspace/zsv-browser-player test:streaming
pnpm --filter @workspace/zsv-browser-player test:browser
```

Streaming tests cover early metadata, single-pass startup, the real WASM
decoder, backpressure, queue limits, audio, replacement, seeking, and EOF.
The 30-second grayscale fixture is included for these tests. Browser tests
require a running routed site, its full storage-backed demo, and Chromium;
see the test script for environment options. They exercise automatic playback,
overlay timing, play/pause, seeking, EOF, and malformed/replacement files.
