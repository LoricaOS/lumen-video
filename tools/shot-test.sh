#!/bin/sh
# shot-test.sh — offscreen render check for the player (no compositor needed).
#
# Builds a static `video -shot` binary, renders one frame through the real
# fit→scale→blit→controls pipeline, and asserts the render succeeded (the video
# rect has real image content and the controls drew). Also writes shot.ppm so
# you can eyeball the frame. Requires the glyph toolkit + FFmpeg statics already
# present (run `make` once, or point at an existing build/ffmpeg-install).
set -eu
CLIP="${1:?usage: shot-test.sh <clip.mp4> [frame]}"
FRAME="${2:-30}"
MUSL_CC="${MUSL_CC:-musl-gcc}"
FFI="${FFI:-build/ffmpeg-install}"

[ -f toolkit/include/glyph.h ] || sh tools/fetch-glyph.sh "$(cat GLYPH_VERSION)" toolkit
[ -f "$FFI/lib/libavcodec.a" ] || sh tools/build-ffmpeg.sh

"$MUSL_CC" -O2 -static -Wall -Itoolkit/include -I"$FFI/include" \
    -o video-shot src/main.c \
    -L"$FFI/lib" -lavformat -lavcodec -lswscale -lswresample -lavutil \
    -Ltoolkit/lib -lcitadel -laudio -lauth -lglyph -lpthread -lm

# stderr carries the stats + PASS/FAIL; stdout carries the base64 PPM.
./video-shot -shot "$CLIP" "$FRAME" 2>shot.log | tr -d '\n' | base64 -d > shot.ppm || true
grep '\[VIDEO\] shot:' shot.log | grep -v ' b64:$'

if grep -q '\[VIDEO\] shot: PASS' shot.log; then
    echo "[shot-test] PASS (frame written to shot.ppm)"
    exit 0
fi
echo "[shot-test] FAIL"; exit 1
