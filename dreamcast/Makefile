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
# all of it is 4.8 MB of SH-4 and leaves the pager no room.

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
AOT ?=
AOT_COVER ?= 0.98
# The block runner's pool: the AOT build hardly uses it, and the heap wants it.
IB_POOL ?= $(if $(AOT),2048u,16384u)

CFLAGS := -std=gnu11 -O2 -fno-strict-aliasing -Wall -Wno-format -Wno-address -include dc_math.h \
          -DM2HLE_VERSION='"dc"' -DM2HLE_DEV_TOOLS=0 \
          -DGEO_CAPTURE_SIZE=4096 -DLOG_MAX_LINES=64 -DNDEBUG -DM2HLE_NO_ZIP \
          -DGEO3D_MAX_LINES=16 -DGEO3D_MAX_TRIS=4096 -DGEO3D_MESH_CACHE_SLOTS=256u \
          -DGEO3D_MESH_ARENA=0xC0000u -DMEM_COUNT=0 -DMEM_PAGES=0x1300u -DMEM_LE_DIRECT=1 -DIRQT_COUNT_T=int32_t -DI960_BLOCKS=1 -DIB_ENTRIES=2048u -DIB_POOL=$(IB_POOL) -DI960_JIT=$(JIT) -DIB_JIT_SELFTEST=$(JIT_TEST) -DIB_JIT_ON=$(JIT_ON) -DDC_HASH_FRAME=$(HASH_FRAME) $(EXTRA) \
          -DSOUND_RAM_SIZE=0x1000u -DSOUND_OUT_FRAMES=1024u -DM68K_ROM_SIZE=0x10000u -DGEO_PUB_COPIES=1 -DM2_NO_NETPLAY \
          -I$(REPO)/src -I$(REPO)/src/board -I$(REPO)/src/core -I$(REPO)/src/ui \
          -I$(REPO)/src/profiles -I$(REPO)/src/net -I. \
          -I$(VENDOR)/sokol

AOT_GEN :=
ifneq ($(AOT),)
CFLAGS  += -DI960_AOT=1 -DI960_AOT_GEN='"aot_gen.h"' -I$(OUT)
AOT_GEN := $(OUT)/aot_gen.h
endif

OBJS   := $(OUT)/main_dc.o

all: $(OUT)/1ST_READ.BIN

# 0x3F268 is dc_sound's hook (main_dc.c), on top of the profile's in the map.
$(OUT)/aot_gen.h: sfight.aotmap $(AOT) $(REPO)/tools/i960_aot.py $(REPO)/src/board/i960_exec.h
	mkdir -p $(OUT)
	python3 $(REPO)/tools/i960_aot.py --map sfight.aotmap --rom "$(AOT)" --hook 0x3F268 --cover $(AOT_COVER) --out $@

$(OUT)/main_dc.o: main_dc.c dc_math.h dc_pager.h dc_layout.h dc_sound.h dc_pvr.h dc_jit_test.h $(wildcard $(REPO)/src/*/*.h) $(AOT_GEN)
	mkdir -p $(OUT)
	kos-cc $(CFLAGS) -c $< -o $@

$(OUT)/m2hle2.elf: $(OBJS)
	kos-cc -o $@ $(OBJS) -lGL -lpthread -lm

# A GD-ROM image boots its program unscrambled (a CD-R would need scramble).
$(OUT)/1ST_READ.BIN: $(OUT)/m2hle2.elf
	$(KOS_OBJCOPY) -R .stack -O binary $< $@
	$(KOS_CC_BASE)/bin/$(KOS_CC_PREFIX)-size $<

clean:
	rm -f $(OBJS) $(OUT)/m2hle2.elf $(OUT)/1ST_READ.BIN $(OUT)/aot_gen.h

.PHONY: all clean
