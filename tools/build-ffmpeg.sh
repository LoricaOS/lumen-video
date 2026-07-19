#!/bin/bash
# tools/build-ffmpeg.sh — configure and compile trimmed static FFmpeg decode
# libs for the lumen-video player: libavformat + libavcodec + libswscale +
# libswresample + libavutil, nothing else (no programs, no encoders, no
# network, no filters, no devices).
#
# Cross-parametrizable like build-curl.sh: set CC/ARCH/STRIP/SUFFIX for arm64
# (CC=<aarch64 musl gcc> ARCH=aarch64 SUFFIX=-arm64). x86 asm needs nasm.
# Install lands in build/ffmpeg-install${SUFFIX}/{lib,include}.
set -e

REPO="${REPO:-$(git rev-parse --show-toplevel)}"
CC="${CC:-musl-gcc}"
ARCH="${ARCH:-x86_64}"
SUFFIX="${SUFFIX:-}"
FFMPEG_SRC="$REPO/references/ffmpeg"
BUILD_DIR="$REPO/build/ffmpeg-build${SUFFIX}"
OUT="$REPO/build/ffmpeg-install${SUFFIX}"

# Cross builds: configure can't run the target test binary, so it must be told
# explicitly. Set CROSS_PREFIX to the binutils prefix (e.g. aarch64-linux-gnu-)
# so ar/nm/ranlib/strip resolve. Empty (native) → no cross flags.
CROSS_PREFIX="${CROSS_PREFIX:-}"
cross_args=""
[ -n "$CROSS_PREFIX" ] && cross_args="--enable-cross-compile --cross-prefix=$CROSS_PREFIX"

[ -f "$FFMPEG_SRC/configure" ] || bash "$REPO/tools/fetch-ffmpeg.sh"

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# --disable-everything + explicit enables keeps the closure auditable: only
# decode paths a video player needs. Codecs: h264/hevc/vp9/mpeg4/mpeg2 video;
# aac/mp3/ac3/opus/vorbis/flac/pcm audio (opus+vorbis so webm/mkv audio works).
# Demuxers: mov(=mp4/m4a)/matroska(=mkv+webm)/avi/mpegts. file protocol only.
"$FFMPEG_SRC/configure" \
  --cc="$CC" \
  --arch="$ARCH" \
  $cross_args \
  --target-os=linux \
  --prefix="$OUT" \
  --extra-cflags="-O2 -fno-pie" \
  --extra-ldflags="-static -no-pie" \
  --pkg-config=false \
  --enable-static --disable-shared \
  --disable-everything \
  --disable-autodetect \
  --disable-programs \
  --disable-doc \
  --disable-network \
  --disable-avdevice \
  --disable-avfilter \
  --disable-postproc \
  --disable-encoders \
  --disable-muxers \
  --disable-bsfs \
  --disable-debug \
  --enable-swscale \
  --enable-swresample \
  --enable-decoder=h264,hevc,vp9,mpeg4,mpeg2video,aac,mp3,ac3,opus,vorbis,flac,pcm_s16le \
  --enable-parser=h264,hevc,vp9,mpegvideo,mpeg4video,aac,mpegaudio,ac3,opus,vorbis,flac \
  --enable-demuxer=mov,matroska,avi,mpegts \
  --enable-protocol=file

make -j"$(nproc)" 2>&1 | tail -3
make install >/dev/null

echo "[ffmpeg] installed: $OUT"
ls -l "$OUT/lib"
