# B of the Dreamcast A/B (Pinboard #520): the same board, Gems C and AOT as
# dreamcast/ (A), but drawn the way KallistiOS's examples draw, through GLdc
# (kos-ports' OpenGL) instead of the PVR direct. Source environ.sh, then
#
#   make -C dreamcast_kos [OUT=<dir>] [AOT=<rom_code1.bin>] [HASH_FRAME=n]
#
# It shares A's headers (pager, sound, layout, texture pack, AOT map), so its
# disc is A's disc with this 1ST_READ.BIN (dreamcast/mkdisc.sh). DC-AB.md
# has what was measured.

REPO   := ..
DC     := $(REPO)/dreamcast
VENDOR ?= $(REPO)/vendor
OUT    ?= $(REPO)/build_dc_kos

ifeq ($(KOS_BASE),)
$(error source KallistiOS's environ.sh first)
endif

HASH_FRAME ?= 0
FPS_CAP ?= 60
AOT ?=
AOT_COVER ?= 0.99
IB_POOL ?= $(if $(AOT),8192u,16384u)

CFLAGS := -std=gnu11 -O2 -fno-strict-aliasing -Wall -Wno-format -Wno-address -include dc_math.h -include dc_paged.h \
          -DM2HLE_VERSION='"dc-kos"' -DM2HLE_DEV_TOOLS=0 \
          -DGEO_CAPTURE_SIZE=1 -DLOG_MAX_LINES=64 -DNDEBUG -DM2HLE_NO_ZIP \
          -DGEO3D_MAX_LINES=16 -DGEO3D_MAX_TRIS=4096 -DGEO3D_TRI_BUF=1 -DGEO3D_DC_SINK -DGEO3D_MESH_CACHE_SLOTS=256u \
          -DGEO3D_MESH_ARENA=0x100000u -DMEM_COUNT=0 -DMEM_PAGE_SHIFT=14 -DMEM_PAGES=0x4C00u -DMEM_LE_DIRECT=1 -DIRQT_COUNT_T=int32_t \
          -DI960_BLOCKS=1 -DIB_ENTRIES=2048u -DIB_POOL=$(IB_POOL) -DI960_JIT=0 -DIB_JIT_SELFTEST=0 -DIB_JIT_ON=0 \
          -DDC_HASH_FRAME=$(HASH_FRAME) -DDC_HOST_MATH=1 -DDC_FPS_CAP=$(FPS_CAP) -DI960_OPTABLE=0 $(EXTRA) \
          -DSOUND_RAM_SIZE=0x1000u -DSOUND_OUT_FRAMES=1024u -DM68K_ROM_SIZE=0x10000u -DGEO_PUB_COPIES=1 -DM2_NO_NETPLAY \
          -I$(REPO)/src -I$(REPO)/src/board -I$(REPO)/src/core -I$(REPO)/src/ui \
          -I$(REPO)/src/profiles -I$(REPO)/src/net -I. -I$(DC) \
          -I$(VENDOR)/sokol

# Gems' C, as in dreamcast/Makefile: private, never committed.
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

OBJS := $(OUT)/main_kos.o

all: $(OUT)/1ST_READ.BIN

AOT_MAP  := $(DC)/sfight.aotmap
PROFILE_HOOKS := $(shell grep -hoE '\{ *0x[0-9A-Fa-f]+u?, *[A-Za-z0-9_]*hook' $(REPO)/src/profiles/sfight.h $(REPO)/src/profiles/sfight_console.h | sed -E 's/\{ *(0x[0-9A-Fa-f]+).*/--hook \1/' | sort -u)
AOT_ARGS := --hook 0x3F268 --hook 0x4BE58 --hook 0x4C008 $(PROFILE_HOOKS) $(GEMS_HOOKS) --cover $(AOT_COVER)
stamp = $(shell mkdir -p $(OUT); t='$(subst ','\'',$(2))'; [ "$$(cat $(1) 2>/dev/null)" = "$$t" ] || printf '%s' "$$t" > $(1))
$(call stamp,$(OUT)/flags.stamp,$(CFLAGS))
$(call stamp,$(OUT)/aot.stamp,$(AOT) $(AOT_MAP) $(AOT_ARGS))

# aot_gen.h holds ROM words: it stays in OUT, never committed.
$(OUT)/aot_gen.h: $(AOT_MAP) $(AOT) $(REPO)/tools/i960_aot.py $(REPO)/src/board/i960_exec.h Makefile $(OUT)/aot.stamp
	mkdir -p $(OUT)
	python3 $(REPO)/tools/i960_aot.py --map $(AOT_MAP) --rom "$(AOT)" $(AOT_ARGS) --out $@

$(OUT)/main_kos.o: main_kos.c $(wildcard $(DC)/*.h) $(wildcard $(REPO)/src/*/*.h) $(AOT_GEN) $(OUT)/flags.stamp
	mkdir -p $(OUT)
	kos-cc $(CFLAGS) -c $< -o $@

$(OUT)/m2hle2.elf: $(OBJS)
	kos-cc -o $@ $(OBJS) -lGL -lpthread -lm

$(OUT)/1ST_READ.BIN: $(OUT)/m2hle2.elf
	$(KOS_OBJCOPY) -R .stack -O binary $< $@
	$(KOS_CC_BASE)/bin/$(KOS_CC_PREFIX)-size $<

clean:
	rm -f $(OBJS) $(OUT)/m2hle2.elf $(OUT)/1ST_READ.BIN $(OUT)/aot_gen.h $(OUT)/flags.stamp $(OUT)/aot.stamp

.PHONY: all clean
