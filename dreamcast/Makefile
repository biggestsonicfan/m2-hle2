# The Dreamcast build (Pinboard #340), with KallistiOS: source its environ.sh,
# then from the repo root
#
#   make -C dreamcast [VENDOR=<a vendor/ with sokol>] [OUT=<dir>]
#
# OUT (default ../build_dc) gets m2hle2.elf and 1ST_READ.BIN; mkdisc.sh makes a
# disc of them and the PS3 ROM files. Only the board is compiled for the SH-4:
# the board and the profiles are headers and main_dc.c is the one unit. No zip
# is ever opened, so miniz stays out (M2HLE_NO_ZIP). Sound (dc_sound.h) needs
# SDL2 for KallistiOS (GPF's dreamcastSDL2, in $(KOS_BASE)/addons; README.md).

REPO   := ..
VENDOR ?= $(REPO)/vendor
OUT    ?= $(REPO)/build_dc

ifeq ($(KOS_BASE),)
$(error source KallistiOS's environ.sh first)
endif

CFLAGS := -std=gnu11 -O2 -fno-strict-aliasing -Wall -Wno-format -Wno-address \
          -DM2HLE_VERSION='"dc"' -DM2HLE_DEV_TOOLS=0 \
          -DGEO_CAPTURE_SIZE=4096 -DLOG_MAX_LINES=64 -DNDEBUG -DM2HLE_NO_ZIP \
          -DGEO3D_MAX_LINES=16 -DGEO3D_MAX_TRIS=4096 -DGEO3D_MESH_CACHE_SLOTS=256u \
          -DGEO3D_MESH_CACHE_BYTES=0x100000u \
          -DSOUND_RAM_SIZE=0x1000u -DSOUND_OUT_FRAMES=1024u -DM68K_ROM_SIZE=0x10000u -DGEO_PUB_COPIES=1 \
          -I$(REPO)/src -I$(REPO)/src/board -I$(REPO)/src/core -I$(REPO)/src/ui \
          -I$(REPO)/src/profiles -I$(REPO)/src/net -I. \
          -I$(VENDOR)/sokol

OBJS   := $(OUT)/main_dc.o

all: $(OUT)/1ST_READ.BIN

$(OUT)/main_dc.o: main_dc.c dc_pager.h dc_layout.h dc_sound.h dc_pvr.h $(wildcard $(REPO)/src/*/*.h)
	mkdir -p $(OUT)
	kos-cc $(CFLAGS) -c $< -o $@

$(OUT)/m2hle2.elf: $(OBJS)
	kos-cc -o $@ $(OBJS) -lSDL2 -lGL -lpthread -lm

# A GD-ROM image boots its program unscrambled (a CD-R would need scramble).
$(OUT)/1ST_READ.BIN: $(OUT)/m2hle2.elf
	$(KOS_OBJCOPY) -R .stack -O binary $< $@
	$(KOS_CC_BASE)/bin/$(KOS_CC_PREFIX)-size $<

clean:
	rm -f $(OBJS) $(OUT)/m2hle2.elf $(OUT)/1ST_READ.BIN

.PHONY: all clean
