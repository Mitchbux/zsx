# CPU-oriented mobile ZSV2 demo

Input: snowboard source timecodes 00:30–01:00, center crop
`crop=270:480:292:0`, 15 fps, no upscale. Quality 70, unchanged stereo
44.1 kHz MDCT audio at step 0.016. All profiles use the same source and
quality setting. Size-first is the unchanged default for general encoding.

| Entropy setting | Complete file bytes | Encode CPU seconds | Median native video decode CPU seconds | Median WASM A/V decode seconds |
|---|---:|---:|---:|---:|
| 0: size-first trials | 3,823,111 | 35.376 | 2.830 | 3.648 |
| 1: byte16 | 3,997,501 | 12.825 | 2.600 | 3.321 |
| 2: byte20 | 3,997,317 | 12.722 | 2.685 | 3.281 |
| 3: candidate16 | 3,823,196 | 13.139 | 2.761 | 3.226 |
| 4: candidate20 | 3,824,118 | 13.031 | 2.731 | 3.473 |

Selected **candidate16** for the browser demo: about 62.9% less encode CPU
time and 11.6% lower median WASM decoding time than size-first, with only
85 additional bytes. Byte16 was faster in native video-only decoding, but
candidate16 gave the best observed WASM result and a smaller file.

These are Linux/native and Node/WASM measurements, not phone measurements.
One encoding run and three decode runs per profile; small timing differences
may be noise. WASM timings include both video and audio; native decode CPU
excludes audio. Cropping and frame-rate reduction contribute much more to
workload reduction versus the previous full-resolution demo, and must not
be presented as an entropy-algorithm-only improvement.

## Reproduction

Build native FFmpeg using `sh ffmpeg-zsx/scripts/build.sh`.
Run `python3 experimental/bench_mobile_demo.py`, then
`node experimental/bench_mobile_wasm.mjs`.
Raw media and JSON measurements are written under ignored
`generated-artifacts/mobile-demo/`.

`-zsx_entropy 0` retains existing smallest-output trials. Values 1–4 select
one already-supported ZSE1 profile, avoiding the extra encoder trials.
Incompressible chunks that would exceed the legacy chunk-size budget fall
back to the established core. Format discriminators and decoder validation
are unchanged; no browser decoder rebuild is needed.

The selected complete file passes native/WASM frame/audio hash parity and
corrupt/truncated input rejection tests: 450 video frames and 1,323,000 stereo
audio sample frames. The audio hash matches the earlier full-resolution demo.
