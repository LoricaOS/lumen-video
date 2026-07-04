# lumen-video — build the player against the pinned glyph toolkit + the ported
# static FFmpeg decode libs, then pack a signed herald package (.hpkg).
#
#   make                fetch toolkit + build ffmpeg + build video.elf + pack
#   HERALD_KEY=<key>    signing key for the package (required to sign)
#   MUSL_CC=<musl-gcc>  musl cross-compiler (defaults to PATH musl-gcc)
#
# Unlike the other Lumen components this one also depends on FFmpeg; it fetches
# and builds a trimmed static FFmpeg the same way LoricaOS does (tools/*ffmpeg*).
MUSL_CC ?= musl-gcc
VERSION       := $(shell cat VERSION)
GLYPH_VERSION := $(shell cat GLYPH_VERSION)

FFI = build/ffmpeg-install
CFLAGS = -O2 -fno-pie -no-pie -Wl,--build-id=none -Wall \
         -DAEGIS_VERSION=\"$(VERSION)\" -Itoolkit/include -I$(FFI)/include
SRCS = $(wildcard src/*.c)

# FFmpeg first (it references libm/pthread symbols), then the glyph toolkit.
FFLIBS = -L$(FFI)/lib -lavformat -lavcodec -lswscale -lswresample -lavutil
LIBS   = $(FFLIBS) -Ltoolkit/lib -lcitadel -laudio -lauth -lglyph -lpthread -lm

all: package

toolkit/include/glyph.h:
	sh tools/fetch-glyph.sh $(GLYPH_VERSION) toolkit

$(FFI)/lib/libavcodec.a:
	sh tools/build-ffmpeg.sh

video.elf: $(SRCS) toolkit/include/glyph.h $(FFI)/lib/libavcodec.a
	$(MUSL_CC) $(CFLAGS) -o $@ $(SRCS) $(LIBS)

package: video.elf
	sh tools/pack.sh

clean:
	rm -f video.elf *.hpkg *.hpkg.sig
	rm -rf toolkit

# Deliberately NOT cleaning build/ffmpeg-install or references/ffmpeg — the
# FFmpeg build is slow; `make clean` keeps it (like a fetched dependency).
