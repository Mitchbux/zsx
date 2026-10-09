#!/bin/sh
# Packaged next to bin/ffmpeg. No system FFmpeg or external services required.
set -eu
if [ "$#" -ne 2 ]; then
    echo "Usage: $0 input.mp4 output.zsv" >&2
    exit 2
fi
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec "$HERE/bin/ffmpeg" -hide_banner -nostdin -n \
    -i "$1" -map 0:v:0 -map 0:a:0 \
    -vf 'scale=trunc(iw/2)*2:trunc(ih/2)*2' -pix_fmt yuv420p \
    -c:v zsx_dct_video -zsx_quality 70 -zsx_entropy 3 \
    -c:a zsx_mdct_audio -zsx_step 0.016 -ar 44100 -ac 2 \
    -metadata zsv_profile=2 -f nut "$2"
