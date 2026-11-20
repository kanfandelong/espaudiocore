#!/bin/sh
# Generate deterministic test assets. Requires ffmpeg (any recent build with
# libmp3lame, aac, flac, libvorbis, libopus and wavpack encoders).
#
#   ./tests/gen_assets.sh [ffmpeg] [out_dir]
set -eu

FF=${1:-ffmpeg}
OUT=${2:-$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/assets}

mkdir -p "$OUT"
"$FF" -hide_banner -loglevel error -y -f lavfi \
    -i "sine=frequency=440:sample_rate=44100:duration=3" \
    -ac 2 -c:a pcm_s16le "$OUT/t.wav"
"$FF" -hide_banner -loglevel error -y -i "$OUT/t.wav" -c:a libmp3lame -b:a 192k "$OUT/t.mp3"
"$FF" -hide_banner -loglevel error -y -i "$OUT/t.wav" -c:a aac -b:a 192k "$OUT/t.aac"
"$FF" -hide_banner -loglevel error -y -i "$OUT/t.wav" -c:a aac -b:a 192k "$OUT/t.m4a"
"$FF" -hide_banner -loglevel error -y -i "$OUT/t.wav" -c:a flac "$OUT/t.flac"
"$FF" -hide_banner -loglevel error -y -i "$OUT/t.wav" -c:a libvorbis -q:a 5 "$OUT/t.ogg"
"$FF" -hide_banner -loglevel error -y -i "$OUT/t.wav" -c:a libopus -b:a 96k "$OUT/t.opus"
"$FF" -hide_banner -loglevel error -y -i "$OUT/t.wav" -c:a wavpack "$OUT/t.wv"

# 24-bit variants for bit-depth conversion checks
"$FF" -hide_banner -loglevel error -y -f lavfi \
    -i "sine=frequency=440:sample_rate=44100:duration=3" \
    -ac 2 -c:a pcm_s24le "$OUT/t24.wav"
"$FF" -hide_banner -loglevel error -y -i "$OUT/t24.wav" -c:a flac "$OUT/t24.flac"

# Reference PCM for tests/compare_pcm.sh (little-endian, interleaved)
"$FF" -hide_banner -loglevel error -y -i "$OUT/t.wav"   -f s16le -ac 2 "$OUT/t.wav.ref16.pcm"
"$FF" -hide_banner -loglevel error -y -i "$OUT/t.flac"  -f s16le -ac 2 "$OUT/t.flac.ref16.pcm"
"$FF" -hide_banner -loglevel error -y -i "$OUT/t.wav"   -f s24le -ac 2 "$OUT/t.wav.ref24.pcm"
"$FF" -hide_banner -loglevel error -y -i "$OUT/t24.wav" -f s24le -ac 2 "$OUT/t24.wav.ref24.pcm"
"$FF" -hide_banner -loglevel error -y -i "$OUT/t24.flac" -f s24le -ac 2 "$OUT/t24.flac.ref24.pcm"

ls -l "$OUT"
