#!/usr/bin/env python3
"""Compare complete ZSV2 mobile-demo encodes on identical cropped input."""
import json
from pathlib import Path
import resource
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "generated-artifacts/mobile-demo"
NATIVE = ROOT / "ffmpeg-zsx/bin/ffmpeg"


def timed(command):
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    start = time.monotonic()
    subprocess.run(list(map(str, command)), check=True)
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    return {"wall_seconds": time.monotonic() - start,
            "cpu_seconds": after.ru_utime + after.ru_stime - before.ru_utime - before.ru_stime}


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    raw = OUT / "source.nut"
    timed(["ffmpeg", "-v", "error", "-nostdin", "-y", "-ss", "30",
           "-i", ROOT / "attached_assets/snowboard_1791565641602.mp4",
           "-t", "30", "-map", "0:v:0", "-map", "0:a:0",
           "-vf", "crop=270:480:292:0,fps=15,setsar=1",
           "-pix_fmt", "yuv420p", "-c:v", "rawvideo", "-c:a", "pcm_f32le",
           "-f", "nut", raw])
    results = []
    for profile, name in [(0, "size-first"), (1, "byte16"), (2, "byte20"),
                          (3, "candidate16"), (4, "candidate20")]:
        encoded = OUT / f"{name}.zsv"
        encode = timed([NATIVE, "-v", "error", "-nostdin", "-y", "-i", raw,
                        "-map", "0:v:0", "-map", "0:a:0", "-c:v", "zsx_dct_video",
                        "-zsx_quality", "70", "-zsx_entropy", profile,
                        "-g", "30", "-bf", "0", "-c:a", "zsx_mdct_audio",
                        "-zsx_step", "0.016", "-metadata", "zsv_profile=2",
                        "-f", "nut", encoded])
        # Exclude audio decode and disk output from the video CPU comparison.
        decode = [timed([NATIVE, "-v", "error", "-xerror", "-nostdin",
                          "-i", encoded, "-map", "0:v:0", "-vsync", "0",
                          "-f", "rawvideo", "/dev/null", "-y"]) for _ in range(3)]
        result = dict(profile=profile, name=name, file_bytes=encoded.stat().st_size,
                      encode=encode, video_decode=decode)
        results.append(result)
        print(json.dumps(result), flush=True)
    (OUT / "results.json").write_text(json.dumps({
        "scope": "30s center crop, 270x480 15fps, quality70, stereo44100Hz. "
                 "Native Linux CPU; 1 encode and 3 video-only decode runs per profile. "
                 "File sizes include audio and container. Not mobile-device measurements.",
        "results": results}, indent=2) + "\n")


if __name__ == "__main__":
    main()
