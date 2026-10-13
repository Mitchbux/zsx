# Browser demos

## Active mobile demo

The active demo now uses the **entire original** snowboard recording
(approximately 4:32), not the excerpt described below. It is served from
App Storage through `/api/demo/snowboard-full.zsv`; the 59,720,317-byte
asset is not committed to Git. It remains 270×480, grayscale, 25 fps with
stereo audio and candidate16 quality 70. Load Sample resumes the audio
context during the user gesture and starts playback after 30 seconds are
buffered. Controls overlay the video and disappear after 2 seconds.

To reproduce, use the encoding commands below but omit `-ss 30` and `-t 30`.
The full encode belongs in `generated-artifacts/mobile-demo/` and then
App Storage, not this public directory.

### Previous 30-second grayscale fixture

`snowboard-gray-25fps.zsv` is the previous short demo: 270×480 portrait,
grayscale, 25 fps, source 00:30–01:00, original stereo 44.1 kHz audio.
It uses quality 70, candidate16, and MDCT audio step 0.016.

The player downloads and decodes incrementally in one pass, buffering
30 seconds before playback (or all remaining media at EOF). A 30-second
demo therefore buffers completely before starting. Packet checks remain;
there is no separate full-file validation pass. Seeking replays cached
encoded bytes. Encoded input is capped at 64 MiB; decoded queues at 192 MiB.
Grayscale frames retain only luma in the browser queue.

Recreate the source with stock FFmpeg:

```sh
ffmpeg -y -ss 30 -i attached_assets/snowboard_1791565641602.mp4 -t 30 \
  -map 0:v:0 -map 0:a:0 \
  -vf 'crop=270:480:292:0,fps=25:start_time=0,hue=s=0,setsar=1' \
  -pix_fmt yuv420p -c:v rawvideo -c:a pcm_f32le -f nut /tmp/gray25.nut
ffmpeg-zsx/bin/ffmpeg -y -i /tmp/gray25.nut -vsync 0 \
  -c:v zsx_dct_video -zsx_quality 70 -zsx_entropy 3 \
  -c:a zsx_mdct_audio -zsx_step 0.016 -metadata zsv_profile=2 -f nut \
  artifacts/zsv-browser-player/public/sample/snowboard-gray-25fps.zsv
```

## Previous color mobile demo

`snowboard-mobile-270x480-c16.zsv` is the previous color demo.
It center-crops the original 00:30–01:00 excerpt to 270×480 portrait (9:16)
without upscaling, at 15 fps. Original stereo 44.1 kHz audio is retained
through MDCT encoding, and independent audio playback remains enabled.
Video uses quality 70 and `-zsx_entropy 3` (single candidate16 profile).

Reproduce all five compared encodes using `python3 experimental/bench_mobile_demo.py`
and WASM timings using `node experimental/bench_mobile_wasm.mjs`, from the
repository root. The selected output is `generated-artifacts/mobile-demo/candidate16.zsv`.
See `experimental/MOBILE_VIDEO_BENCHMARK.md` for results and limitations.

## Previous full-resolution demo

`snowboard-0030-0100-854x480.zsv` is retained as the full-resolution demo.
It uses source timecodes **00:00:30–00:01:00**, preserving the original
854×480 resolution, 30000/1001 frame rate, and stereo 44.1 kHz audio.
Video is encoded at quality 70; audio uses MDCT step 0.016. These are lossy
codecs; retaining resolution and audio format does not mean bit-exact media.

```sh
ffmpeg -v error -nostdin -y -threads 2 -ss 30 \
  -i attached_assets/snowboard_1791565641602.mp4 -t 30 \
  -map 0:v:0 -map 0:a:0 -pix_fmt yuv420p \
  -c:v rawvideo -c:a pcm_f32le -f nut /tmp/snowboard-original-crop.nut
ffmpeg-zsx/bin/ffmpeg -v error -nostdin -y \
  -i /tmp/snowboard-original-crop.nut -map 0:v:0 -map 0:a:0 \
  -c:v zsx_dct_video -zsx_quality 70 -g 60 -bf 0 \
  -c:a zsx_mdct_audio -zsx_step 0.016 -metadata zsv_profile=2 -f nut \
  artifacts/zsv-browser-player/public/sample/snowboard-0030-0100-854x480.zsv
```

## Earlier low-resolution snowboard excerpt

`snowboard-stream-30s.zsv` is the previous demo: the first 30 seconds
of the uploaded snowboard video, at 320×180, 15 fps, with mono 16 kHz audio.
It uses streaming ZSXD at quality 60 and MDCT audio with step 0.016.
The original upload is unchanged.

Regenerate from the repository root:

```sh
ffmpeg -v error -nostdin -y -threads 2 \
  -i attached_assets/snowboard_1791565641602.mp4 -t 30 \
  -map 0:v:0 -map 0:a:0 \
  -vf 'fps=15,scale=320:180:flags=area,setsar=1' \
  -pix_fmt yuv420p -c:v rawvideo -ar 16000 -ac 1 -c:a pcm_f32le \
  -f nut /tmp/snowboard-demo-input.nut
ffmpeg-zsx/bin/ffmpeg -v error -nostdin -y \
  -i /tmp/snowboard-demo-input.nut -map 0:v:0 -map 0:a:0 \
  -c:v zsx_dct_video -zsx_quality 60 -g 150 -bf 0 \
  -c:a zsx_mdct_audio -zsx_step 0.016 -metadata zsv_profile=2 -f nut \
  artifacts/zsv-browser-player/public/sample/snowboard-stream-30s.zsv

ZSV_PARITY_SAMPLE="$PWD/artifacts/zsv-browser-player/public/sample/snowboard-stream-30s.zsv" \
  ZSV_EXPECT_FRAMES=450 ZSV_EXPECT_AUDIO_SAMPLES=480000 \
  node artifacts/zsv-browser-player/tests/wasm-native-parity.mjs
```

## Previous demo

`demo-stream-30s.zsv` is retained as a regression fixture, encoded with the
streaming entropy-profile selector in the native ZSXD encoder. It contains
150 video frames (160×90, 5 fps) and 30 seconds of mono 8 kHz audio.

Regenerate from the retained legacy demo at the repository root:

```sh
ffmpeg-zsx/bin/ffmpeg -v error -y \
  -i artifacts/zsv-browser-player/public/sample/demo-small-30s.zsv \
  -map 0:v:0 -map 0:a:0 \
  -c:v zsx_dct_video -zsx_quality 20 -c:a copy -f nut \
  artifacts/zsv-browser-player/public/sample/demo-stream-30s.zsv
```

Audio is copied unchanged. Video is re-encoded from an already lossy source,
so file-size differences are not an isolated entropy-compression comparison.
The original demo remains available for legacy-decoder regression tests.

Verify the active demo against native decoding, including corruption checks:

```sh
ZSV_PARITY_SAMPLE="$PWD/artifacts/zsv-browser-player/public/sample/demo-stream-30s.zsv" \
  node artifacts/zsv-browser-player/tests/wasm-native-parity.mjs
```
