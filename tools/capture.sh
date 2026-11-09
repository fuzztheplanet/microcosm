#!/usr/bin/env bash
# capture.sh CAPTURE-BIN VIDEO WORKDIR - record the show as the release video.
#
# CAPTURE-BIN is the demo's capture build (make capture builds it and runs
# this script). It renders every frame offline at 1920x1080 and 60 fps, from
# 2160 lines unless MC_CAP_SCALE says otherwise, and it writes the soundtrack
# too (MC_CAP_AUDIO). Nothing is shown or played.
#
# There are two steps, so the slow render only happens once:
#   1. The frames go into a near-lossless master in WORKDIR (x264 at CRF 8,
#      about 2.5 GB for the whole show). If an earlier run left a finished
#      master for the same binary and settings, it's used again.
#   2. The master is encoded with x264 in two passes, at an average of
#      BITRATE (10M) with peaks held to 40 Mb/s, and the soundtrack goes in
#      as AAC at 256 kb/s. That's VIDEO, about 360 MB for the whole show.
# WORKDIR is deleted once VIDEO is written.
#
#   MC_CAP_DUR=10       only the first 10 seconds, for a quick test
#   MC_CAP_SCALE=100    render from 1080 lines instead of 2160: about twice as
#                       fast, for a preview
#   BITRATE=10M         the video's average bitrate
#
# On an Intel UHD 620 the master for the whole show took 57 minutes, and the
# two passes add to that. It needs about 3 GB free for WORKDIR, and ffmpeg
# with libx264. The picture is converted to BT.709 and tagged as such.
set -euo pipefail

usage='usage: capture.sh CAPTURE-BIN VIDEO WORKDIR'
bin=${1:?$usage} out=${2:?$usage} work=${3:?$usage}
command -v ffmpeg >/dev/null || { echo "capture.sh: needs ffmpeg" >&2; exit 2; }
case $bin in */*) ;; *) bin=./$bin ;; esac
rate=${BITRATE:-10M}
export MC_CAP_SCALE=${MC_CAP_SCALE:-200}
key="$(sha256sum < "$bin" | cut -c1-16) scale=$MC_CAP_SCALE dur=${MC_CAP_DUR:-all}"
mkdir -p -- "$work" "$(dirname -- "$out")"

if [ "$(cat -- "$work/master.key" 2>/dev/null)" = "$key" ]; then
    echo "capture.sh: using the finished master in $work" >&2
else
    rm -f -- "$work/master.key" "$work/master.mkv" "$work/sound.f32"
    MC_CAP_AUDIO=$work/sound.f32 "$bin"
    echo "capture.sh: rendering the master at ${MC_CAP_SCALE}% -> $work/master.mkv" >&2
    "$bin" | ffmpeg -hide_banner -loglevel error -nostdin -y \
        -f rawvideo -pix_fmt rgba -s 1920x1080 -r 60 -i - \
        -vf scale=out_color_matrix=bt709:out_range=tv \
        -colorspace bt709 -color_primaries bt709 -color_trc bt709 \
        -c:v libx264 -preset faster -crf 8 -pix_fmt yuv420p "$work/master.mkv"
    echo "$key" > "$work/master.key"
fi

x264=(-c:v libx264 -preset slow -tune film -x264-params aq-mode=3
      -b:v "$rate" -maxrate 40M -bufsize 60M -profile:v high -level 4.2 -pix_fmt yuv420p)
echo "capture.sh: encoding, pass 1 of 2" >&2
ffmpeg -hide_banner -loglevel error -nostdin -y -i "$work/master.mkv" -map 0:v \
    "${x264[@]}" -pass 1 -passlogfile "$work/x264" -an -f null /dev/null
echo "capture.sh: encoding, pass 2 of 2" >&2
ffmpeg -hide_banner -loglevel error -nostdin -y -i "$work/master.mkv" \
    -f f32le -ar 44100 -ac 2 -i "$work/sound.f32" -map 0:v -map 1:a \
    "${x264[@]}" -colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv \
    -pass 2 -passlogfile "$work/x264" -c:a aac -b:a 256k -shortest \
    -movflags +faststart -f mp4 "$out.part"
mv -- "$out.part" "$out"
rm -rf -- "${work:?}"
echo "capture.sh: wrote $out ($(du -h -- "$out" | cut -f1))" >&2
