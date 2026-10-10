# MKWrap / MKSetup ビルド用 Makefile
#   Linux (Debian/Ubuntu): sudo apt install gcc-mingw-w64-i686 libz-mingw-w64-dev make zip
#   Windows (MSYS2 MINGW32): pacman -S mingw-w64-i686-gcc mingw-w64-i686-zlib make zip
#                            make CROSS=
#   make        → build/dinput.dll, build/MKSetup.exe
#   make dist   → dist/MKWrap-<version>.zip (配布用)

CROSS   ?= i686-w64-mingw32-
CC      := $(CROSS)gcc
WINDRES := $(CROSS)windres
BUILD   := build
VERSION := $(shell sed -n 's/^\#define MKW_VERSION "\(.*\)"/\1/p' src/wrap/mkw.h)

WARN    := -Wall -Wno-unused-function -Wno-format-truncation
WRAP_SRC := $(wildcard src/wrap/*.c)
WRAP_CFLAGS := -O2 $(WARN) -std=gnu99 -DWINVER=0x0601 -D_WIN32_WINNT=0x0601
WRAP_LIBS := -static -static-libgcc -lgdi32 -luser32 -lwinmm -ldxguid -luuid -ladvapi32

US_SRC  := $(wildcard third_party/unshield/*.c) third_party/unshield/md5/md5c.c
SETUP_SRC := src/setup/mksetup.c src/setup/is3.c src/setup/is6.c third_party/blast/blast.c $(US_SRC)
SETUP_CFLAGS := -O2 $(WARN) -std=gnu99 -DUNICODE -D_UNICODE -Isrc/setup -Ithird_party/blast \
                -Ithird_party/unshield -Ithird_party/unshield/md5
SETUP_LIBS := -static -lz -lshell32 -lole32 -lcomctl32

.PHONY: all clean dist
all: $(BUILD)/dinput.dll $(BUILD)/MKSetup.exe

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/dinput.dll: $(WRAP_SRC) src/wrap/mkw.h src/wrap/exports.def | $(BUILD)
	$(CC) $(WRAP_CFLAGS) -shared -o $@ $(WRAP_SRC) src/wrap/exports.def $(WRAP_LIBS) -Wl,--enable-stdcall-fixup -s

$(BUILD)/mksetup_res.o: src/setup/mksetup.rc src/setup/app.manifest $(BUILD)/dinput.dll
	$(WINDRES) -I$(BUILD) -Isrc/setup src/setup/mksetup.rc -O coff -o $@

$(BUILD)/MKSetup.exe: $(SETUP_SRC) src/setup/common.h src/setup/profiles.h $(BUILD)/mksetup_res.o
	$(CC) $(SETUP_CFLAGS) -municode -mwindows -o $@ $(SETUP_SRC) $(BUILD)/mksetup_res.o $(SETUP_LIBS) -s

dist: all
	rm -rf dist/MKWrap-$(VERSION) && mkdir -p dist/MKWrap-$(VERSION)
	cp $(BUILD)/MKSetup.exe $(BUILD)/dinput.dll dist/MKWrap-$(VERSION)/
	cp docs/README_release.txt dist/MKWrap-$(VERSION)/README.txt
	cp LICENSE THIRD_PARTY_NOTICES.md dist/MKWrap-$(VERSION)/
	cd dist && rm -f MKWrap-$(VERSION).zip && zip -qr MKWrap-$(VERSION).zip MKWrap-$(VERSION)

clean:
	rm -rf $(BUILD) dist
