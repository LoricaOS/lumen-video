# lumen-video

A video player for [LoricaOS](https://github.com/LoricaOS/LoricaOS), built on
the ported static FFmpeg decode libraries and the glyph/Lumen toolkit.

## How it works

- **Decode** is FFmpeg (avformat/avcodec/swscale/swresample) — the trimmed
  static build the LoricaOS tree produces (`tools/build-ffmpeg.sh`).
- **A/V sync** rides the kernel's playback clock. An audio thread decodes and
  resamples the audio track to 48 kHz/s16/stereo and feeds `/dev/audio`, which
  paces itself via backpressure and becomes the master clock (`sys_audio_position`,
  syscall 505 = ms actually played). The main thread decodes video frames and
  presents each one when the clock reaches its timestamp, dropping frames that
  fall behind.
- **The window** is a glyph/Lumen surface: each decoded frame is swscaled into
  the letterboxed video rect and blitted, with a control bar (play indicator,
  progress, elapsed/total) underneath.

The audio and video streams are read through two independent `AVFormatContext`s
open on the same file, so there is no shared demux or packet queue between the
threads — the file is read twice, which is free for local playback.

## Controls

`Space` play/stop · `q` / `Esc` quit. v1 has no pause or seek (the audio sink
has no pause primitive — that's a kernel follow-up).

## Build

```
make                 # fetch glyph toolkit + build FFmpeg + build video.elf + pack .hpkg
MUSL_CC=<musl-gcc>   # musl cross-compiler (default: PATH musl-gcc)
HERALD_KEY=<key>     # sign the package
```

## Test

```
sh tools/shot-test.sh <clip.mp4>   # offscreen render check — no compositor needed
```

`video -shot <file> [frame]` renders one frame through the real
fit→scale→blit→controls pipeline into an offscreen surface and prints content
stats plus a base64 PPM thumbnail. It asserts the video rect carries real image
content (luminance variance) and that the controls drew (accent color present),
so the render path is verifiable without booting the desktop stack.
