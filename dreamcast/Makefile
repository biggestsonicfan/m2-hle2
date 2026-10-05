# The Dreamcast build (Pinboard #340), with KallistiOS: source its environ.sh,
# then from the repo root
#
#   make -C dreamcast [VENDOR=<a vendor/ with sokol>] [OUT=<dir>]
#
# OUT (default ../build_dc) gets m2hle2.elf and 1ST_READ.BIN; mkdisc.sh makes a
# disc of them and the PS3 ROM files. Only the board is compiled for the SH-4:
# the board and the profiles are headers and main_dc.c is the one unit. No zip
# is ever opened, so miniz stays out (M2HLE_NO_ZIP). Sound (dc_sound.h) is
# KOS's own (snd_stream, snd_sfx).
#
# AOT=<rom_code1.bin> compiles the i960 code STF runs to C ahead of time
# (Pinboard #394, src/core/i960_aot.h): tools/i960_aot.py reads the code at the
# addresses sfight.aotmap lists out of the player's own program ROM, into
# $(OUT)/aot_gen.h. That file holds ROM words: it stays in OUT, never committed.
# AOT_COVER is the share of the map's counts compiled (the hottest code first):
# all of it is 4.8 MB of SH-4 and leaves the pager no room. Recorded with Gems
# on, 79% of the counts are the two instructions of `_idle`'s vblank spin, which
# the spin hook skips, so 0.99 here is about what 0.95 of the rest would be.
# AOT_MAP=pacman.aotmap compiles m2-pacman instead (AOT= its game.bin): homebrew
# has none of STF's hooks, so only the map's are kept (Pinboard #463).

REPO   := ..
VENDOR ?= $(REPO)/vendor
OUT    ?= $(REPO)/build_dc

ifeq ($(KOS_BASE),)
$(error source KallistiOS's environ.sh first)
endif

JIT ?= 0
JIT_TEST ?= 0
JIT_ON ?= 1
HASH_FRAME ?= 0
# LINK=1: the replay fight held against MAME over the serial port (dc_link.h,
# tools/dc-lockstep.py). LINK_GEMS=1 keeps Gems on in it.
LINK ?= 0
LINK_GEMS ?= 0
# The link's floats have to be the board's, so no a*b+c is fused into the
# SH-4's FMAC (the desktop builds' -ffp-contract=off, CMakeLists.txt).
LINK_CFLAGS := $(if $(filter 1,$(LINK)),-ffp-contract=off)
# HOST_MATH=1: the COP's sin, cos and square roots from the SH-4's FSCA and
# FSQRT (dc_math.h), Gems' COP included. Off for LINK, which must be the board's.
HOST_MATH ?= $(if $(filter 1,$(LINK)),0,1)
# FRAME512=1: a 512x384 frame set in the middle of the 640x480 signal, where
# the default draws the board centred in a 640x480 one (dc_pvr.h). Flycast
# stretches a frame to fill its window, so check it on a Dreamcast.
FRAME512 ?= 0
# OPTAB=1: the interpreter dispatches through a handler table (I960_OPTABLE).
# Bit-exact, but with Gems on it saved 38 ms of 5874 over the bench's 400
# frames and cost 72 KB of text, 64 KB of heap: off.
OPTAB ?= 0
AOT ?=
# HUD=min: only a small frame counter on screen (DC_HUD_MIN, main_dc.c).
HUD ?=
# m2-pacman's map is flatter: 0.99 of it is 33,000 instructions and runs the
# board out of RAM; 0.9 is 14,600 (2.4 MB of text).
# pacman_geo's (the GEO-sprite build) at 0.95 is 16,300.
AOT_COVER ?= $(if $(filter pacman.aotmap,$(AOT_MAP)),0.9,$(if $(filter pacman_geo.aotmap,$(AOT_MAP)),0.95,0.99))
# The block runner's pool. The AOT build needs less of it, but at 4096 ops a
# fight's blocks overflowed it and were rebuilt every frame (3.7% of the frame).
IB_POOL ?= $(if $(AOT),8192u,16384u)

CFLAGS := -std=gnu11 -O2 -fno-strict-aliasing -Wall -Wno-format -Wno-address -include dc_math.h -include dc_paged.h \
          -DM2HLE_VERSION='"dc"' -DM2HLE_DEV_TOOLS=0 \
          -DGEO_CAPTURE_SIZE=1 -DLOG_MAX_LINES=64 -DNDEBUG -DM2HLE_NO_ZIP \
          -DGEO3D_MAX_LINES=16 -DGEO3D_MAX_TRIS=4096 -DGEO3D_TRI_BUF=1 -DGEO3D_DC_SINK -DGEO3D_MESH_CACHE_SLOTS=256u \
          -DGEO3D_MESH_ARENA=0x100000u -DMEM_COUNT=0 -DMEM_PAGE_SHIFT=14 -DMEM_PAGES=0x4C00u -DMEM_LE_DIRECT=1 -DIRQT_COUNT_T=int32_t -DI960_BLOCKS=1 -DIB_ENTRIES=2048u -DIB_POOL=$(IB_POOL) -DI960_JIT=$(JIT) -DIB_JIT_SELFTEST=$(JIT_TEST) -DIB_JIT_ON=$(JIT_ON) -DDC_HASH_FRAME=$(HASH_FRAME) -DDC_LINK=$(LINK) -DDC_LINK_GEMS=$(LINK_GEMS) -DI960_OPTABLE=$(OPTAB) -DDC_HOST_MATH=$(HOST_MATH) -DDC_FRAME512=$(FRAME512) -DDC_HUD_MIN=$(if $(filter min,$(HUD)),1,0) $(LINK_CFLAGS) $(EXTRA) \
          -DSOUND_RAM_SIZE=0x1000u -DSOUND_OUT_FRAMES=1024u -DM68K_ROM_SIZE=0x10000u -DGEO_PUB_COPIES=1 -DM2_NO_NETPLAY \
          -I$(REPO)/src -I$(REPO)/src/board -I$(REPO)/src/core -I$(REPO)/src/ui \
          -I$(REPO)/src/profiles -I$(REPO)/src/net -I. \
          -I$(VENDOR)/sokol

# GEMS=<dir> builds in Sega's C from Sonic Gems Collection (src/core/gems.h,
# Pinboard #454), on by default: its trapped i960 functions and its COP
# commands. That C is private and is never committed; the default is where it
# sits beside this checkout, and GEMS= (empty) leaves it out. Its trap sites are
# hooks, so the AOT must not compile over them.
GEMS ?= $(shell d="$(HOME)/source/repos/ai/Sonic Gems Collection/m2hle"; [ -f "$$d/gems_impl.h" ] && echo "$$d")
GEMS := $(GEMS)
GEMS_HOOKS :=
ifneq ($(GEMS),)
CFLAGS     += -DM2HLE_GEMS -I"$(GEMS)"
GEMS_HOOKS := $(shell grep -o '{ 0x[0-9A-Fa-f]*, gfn_' "$(GEMS)/gems_all.h" | sed 's/{ \(0x[0-9A-Fa-f]*\),.*/--hook \1/')
endif

AOT_GEN :=
ifneq ($(AOT),)
CFLAGS  += -DI960_AOT=1 -DI960_AOT_GEN='"aot_gen.h"' -I$(OUT)
AOT_GEN := $(OUT)/aot_gen.h
endif

OBJS   := $(OUT)/main_dc.o

all: $(OUT)/1ST_READ.BIN

# The options a build was made with: a change (JIT=, OPTAB=, HASH_FRAME=, GEMS=,
# AOT_COVER= ...) rebuilds, where the object used to be left as it was. A stamp
# file is rewritten only when its text changes.
AOT_MAP  ?= sfight.aotmap
AOT_ARGS := $(if $(filter sfight.aotmap,$(AOT_MAP)),--hook 0x3F268 --hook 0x4BE58 --hook 0x4C008 $(GEMS_HOOKS)) --cover $(AOT_COVER)
stamp = $(shell mkdir -p $(OUT); t='$(subst ','\'',$(2))'; [ "$$(cat $(1) 2>/dev/null)" = "$$t" ] || printf '%s' "$$t" > $(1))
$(call stamp,$(OUT)/flags.stamp,$(CFLAGS))
$(call stamp,$(OUT)/aot.stamp,$(AOT) $(AOT_MAP) $(AOT_ARGS))

# 0x3F268 is dc_sound's hook (main_dc.c), and 0x4BE58 / 0x4C008 the profile's
# timed waits (m2_spin.h), and the Gems trap sites, on top of the hooks in the map.
$(OUT)/aot_gen.h: $(AOT_MAP) $(AOT) $(REPO)/tools/i960_aot.py $(REPO)/src/board/i960_exec.h Makefile $(OUT)/aot.stamp
	mkdir -p $(OUT)
	python3 $(REPO)/tools/i960_aot.py --map $(AOT_MAP) --rom "$(AOT)" $(AOT_ARGS) --out $@

$(OUT)/main_dc.o: main_dc.c dc_math.h dc_paged.h dc_pager.h dc_layout.h dc_sound.h dc_pvr.h dc_jit_test.h dc_link.h $(wildcard $(REPO)/src/*/*.h) $(AOT_GEN) $(OUT)/flags.stamp
	mkdir -p $(OUT)
	kos-cc $(CFLAGS) -c $< -o $@

$(OUT)/m2hle2.elf: $(OBJS)
	kos-cc -o $@ $(OBJS) -lGL -lpthread -lm

# A GD-ROM image boots its program unscrambled (a CD-R would need scramble).
$(OUT)/1ST_READ.BIN: $(OUT)/m2hle2.elf
	$(KOS_OBJCOPY) -R .stack -O binary $< $@
	$(KOS_CC_BASE)/bin/$(KOS_CC_PREFIX)-size $<

clean:
	rm -f $(OBJS) $(OUT)/m2hle2.elf $(OUT)/1ST_READ.BIN $(OUT)/aot_gen.h $(OUT)/flags.stamp $(OUT)/aot.stamp

.PHONY: all clean
