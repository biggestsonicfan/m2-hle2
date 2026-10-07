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
# SCANSPLIT=1: the bench splits the 3D scan ("sc") by function (dc_scansplit.h, #535).
SCANSPLIT ?= 0
# FACECOUNT=1: the bench counts the face loop's branches (dc_scansplit.h, #537).
FACECOUNT ?= 0
# LINK=1: the replay fight held against MAME over the serial port (dc_link.h,
# tools/dc-lockstep.py). LINK_GEMS=1 keeps Gems on in it.
LINK ?= 0
LINK_GEMS ?= 0
# SDLOG=1: everything printed, stats rows and the board's log included, to
# M2LOGnnn.TXT on a FAT SD card on the serial port (dc_sdlog.h). The port is
# the card's: no LINK with it.
SDLOG ?= 0
ifeq ($(SDLOG)$(LINK),11)
$(error SDLOG=1 and LINK=1 both want the serial port)
endif
SDLOG_LIBS := $(if $(filter 1,$(SDLOG)),-lkosfat)
# The link's floats have to be the board's, so no a*b+c is fused into the
# SH-4's FMAC (the desktop builds' -ffp-contract=off, CMakeLists.txt).
LINK_CFLAGS := $(if $(filter 1,$(LINK)),-ffp-contract=off)
# HOST_MATH=1: the COP's sin, cos and square roots from the SH-4's FSCA and
# FSQRT (dc_math.h), Gems' COP included. Off for LINK, which must be the board's.
HOST_MATH ?= $(if $(filter 1,$(LINK)),0,1)
# FRAME512=1: a 512x384 frame set in the middle of the 640x480 signal, where
# the default draws the board centred in a 640x480 one (dc_pvr.h). Flycast
# stretches a frame to fill its window unless its core has
# tools/flycast-frame512.patch (#499); Redream shows it as a Dreamcast does.
FRAME512 ?= 0
# VIEW=x,y,w,h: show only that rectangle of the board's 496x384, scaled to fill
# the frame (dc_pvr.h). m2-sonic's Mega Drive picture: VIEW=88,80,320,224 (2x);
# m2-pacman's 224x288: VIEW=136,48,224,288 (5/3, the frame's full height).
VIEW ?=
# FILL=1: stretch the view over the whole frame, its shape not kept (DC_FILL,
# dc_pvr.h, #503): STF's 496x384 to 640x480, as a 4:3 arcade monitor shows it.
# On by default for the whole board; a VIEW (a homebrew's own screen) keeps its
# shape unless FILL=1 is given too.
FILL ?= $(if $(VIEW),0,1)
comma := ,
VIEW_N := $(subst $(comma), ,$(VIEW))
VIEW_CFLAGS := $(if $(VIEW),-DDC_VIEW_X=$(word 1,$(VIEW_N)) -DDC_VIEW_Y=$(word 2,$(VIEW_N)) -DDC_VIEW_W=$(word 3,$(VIEW_N)) -DDC_VIEW_H=$(word 4,$(VIEW_N)))
# OPTAB=1: the interpreter dispatches through a handler table (I960_OPTABLE).
# Bit-exact, but with Gems on it saved 38 ms of 5874 over the bench's 400
# frames and cost 72 KB of text, 64 KB of heap: off.
OPTAB ?= 0
AOT ?=
# STRIPS=0: no strip pack (STRIPS.PAK, dc_strips.h): every mesh is walked from the ROM.
STRIPS ?= 1
# FPS_CAP=n: at most n board frames a second (DC_FPS_CAP, main_dc.c); 0: no cap.
# For emulators whose SH-4 outruns a Dreamcast's (Redream).
FPS_CAP ?= 60
# HUD=min: only a small frames-a-second counter on screen (DC_HUD_MIN, main_dc.c).
# HUD=none: nothing over the game at all, for a picture to hold against MAME's
# (DC_HUD_NONE); boot errors still show.
HUD ?=
# FPS=1: that counter, top right, with any HUD but none (DC_HUD_FPS): beside
# HUD=prof's panel, and still there when R hides it. HUD=min turns it on.
FPS ?= $(if $(filter min,$(HUD)),1,0)
# HUD=prof: the full stats and the hardware profile (dc_prof.h, Pinboard #495):
# the SH-4's counters, the PVR's times, a PC sampler by symbol and group, as a
# panel of small checked lines a capture of the video can be read back from
# (#518: tools/hud_read.py, README.md). The
# sampler's symbol map is made from the linked elf, so the program is linked
# twice (tools/dc_profmap.py; the map is data, the code's addresses hold).
PROF := $(if $(filter prof,$(HUD)),1,0)
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
          -DGEO3D_MESH_ARENA=0x100000u $(if $(filter 1,$(STRIPS)),-DGEO3D_STRIPS) -DMEM_COUNT=0 -DMEM_PAGE_SHIFT=14 -DMEM_PAGES=0x4C00u -DMEM_LE_DIRECT=1 -DIRQT_COUNT_T=int32_t -DI960_BLOCKS=1 -DIB_ENTRIES=2048u -DIB_POOL=$(IB_POOL) -DI960_JIT=$(JIT) -DIB_JIT_SELFTEST=$(JIT_TEST) -DIB_JIT_ON=$(JIT_ON) -DDC_HASH_FRAME=$(HASH_FRAME) -DDC_SCAN_SPLIT=$(SCANSPLIT) -DDC_FACE_COUNT=$(FACECOUNT) -DDC_LINK=$(LINK) -DDC_SDLOG=$(SDLOG) -DDC_LINK_GEMS=$(LINK_GEMS) -DI960_OPTABLE=$(OPTAB) -DDC_HOST_MATH=$(HOST_MATH) -DDC_FRAME512=$(FRAME512) $(VIEW_CFLAGS) -DDC_FILL=$(FILL) -DDC_FPS_CAP=$(FPS_CAP) -DDC_HUD_MIN=$(if $(filter min none,$(HUD)),1,0) -DDC_HUD_NONE=$(if $(filter none,$(HUD)),1,0) -DDC_HUD_PROF=$(PROF) -DDC_HUD_FPS=$(if $(filter none,$(HUD)),0,$(FPS)) $(LINK_CFLAGS) $(EXTRA) \
          -DSOUND_RAM_SIZE=0x1000u -DSOUND_OUT_FRAMES=1024u -DM68K_ROM_SIZE=0x10000u -DGEO_PUB_COPIES=1 -DM2_NO_NETPLAY \
          -I$(REPO)/src -I$(REPO)/src/board -I$(REPO)/src/core -I$(REPO)/src/ui \
          -I$(REPO)/src/profiles -I$(REPO)/src/net -I. \
          -I$(VENDOR)/sokol

# GEMS=<dir> builds in Sega's C from Sonic Gems Collection (src/core/gems.h,
# Pinboard #454), on by default: its trapped i960 functions and its COP
# commands. That C is the submodule vendor/gems-c (#532, init it with
# --recursive); the default is this checkout's copy, else VENDOR's, and GEMS=
# (empty) leaves it out (as does a tree without the submodule). Its gems_all.h
# is generated: gen_all.py writes it here, before the hook list is read from it.
# Its trap sites are hooks, so the AOT must not compile over them.
GEMS ?= $(firstword $(foreach d,$(REPO)/vendor/gems-c $(VENDOR)/gems-c,$(if $(wildcard $(d)/gems_impl.h),$(abspath $(d)))))
GEMS := $(GEMS)
GEMS_HOOKS :=
ifneq ($(GEMS),)
CFLAGS     += -DM2HLE_GEMS -I"$(GEMS)"
GEMS_GEN   := $(shell python3 "$(GEMS)/gen_all.py" >/dev/null 2>&1 || echo failed)
ifneq ($(GEMS_GEN),)
$(error $(GEMS)/gen_all.py failed (a submodule checkout needs: git submodule update --init --recursive vendor/gems-c))
endif
GEMS_HOOKS := $(shell grep -o '{ 0x[0-9A-Fa-f]*, gfn_' "$(GEMS)/gems_all.h" | sed 's/{ \(0x[0-9A-Fa-f]*\),.*/--hook \1/')
endif

# The commit built (-dirty: with changes not in it), for HUD=prof's ID line
# and the assertion screen; the build number (r + the commits up to it, as the
# canary's desktop builds count) and RELEASE="Alpha 0.3", the release's name,
# for HUD=prof's VR line (#526).
CFLAGS += -DDC_GIT='"$(shell git -C $(REPO) describe --always --abbrev=7 --dirty 2>/dev/null || echo unknown)"'
CFLAGS += -DDC_BUILD='"r$(shell git -C $(REPO) rev-list --count HEAD 2>/dev/null || echo 0)"'
RELEASE ?=
CFLAGS += $(if $(RELEASE),-DDC_RELEASE='"$(RELEASE)"')

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
# The STF profiles' hooks, read out of their tables (the `{ 0x..., *_hook` rows
# of sfight.h and sfight_console.h): aot_check turns the whole AOT off when the
# profile has a hook the code was not told about, and a hook added after the
# map was recorded (enemy_rank_table, #509) did just that, silently.
PROFILE_HOOKS := $(shell grep -hoE '\{ *0x[0-9A-Fa-f]+u?, *[A-Za-z0-9_]*hook' $(REPO)/src/profiles/sfight.h $(REPO)/src/profiles/sfight_console.h | sed -E 's/\{ *(0x[0-9A-Fa-f]+).*/--hook \1/' | sort -u)
AOT_ARGS := $(if $(filter sfight.aotmap,$(AOT_MAP)),--hook 0x3F268 --hook 0x4BE58 --hook 0x4C008 $(PROFILE_HOOKS) $(GEMS_HOOKS)) --cover $(AOT_COVER)
stamp = $(shell mkdir -p $(OUT); t='$(subst ','\'',$(2))'; [ "$$(cat $(1) 2>/dev/null)" = "$$t" ] || printf '%s' "$$t" > $(1))
$(call stamp,$(OUT)/flags.stamp,$(CFLAGS))
$(call stamp,$(OUT)/aot.stamp,$(AOT) $(AOT_MAP) $(AOT_ARGS))

# 0x3F268 is dc_sound's hook (main_dc.c), and 0x4BE58 / 0x4C008 the profile's
# timed waits (m2_spin.h), the profiles' own hooks and the Gems trap sites, on
# top of the hooks in the map.
$(OUT)/aot_gen.h: $(AOT_MAP) $(AOT) $(REPO)/tools/i960_aot.py $(REPO)/src/board/i960_exec.h Makefile $(OUT)/aot.stamp
	mkdir -p $(OUT)
	python3 $(REPO)/tools/i960_aot.py --map $(AOT_MAP) --rom "$(AOT)" $(AOT_ARGS) --out $@

$(OUT)/main_dc.o: main_dc.c dc_math.h dc_paged.h dc_pager.h dc_layout.h dc_strips.h dc_sound.h dc_pvr.h dc_scansplit.h dc_jit_test.h dc_link.h dc_sdlog.h dc_prof.h dc_hudfont.h $(wildcard $(REPO)/src/*/*.h) $(AOT_GEN) $(OUT)/flags.stamp
	mkdir -p $(OUT)
	kos-cc $(CFLAGS) -c $< -o $@

ifeq ($(PROF),1)
NM := $(KOS_CC_BASE)/bin/$(KOS_CC_PREFIX)-nm
$(OUT)/dc_prof_stub.c: tools/dc_profmap.py
	mkdir -p $(OUT)
	python3 tools/dc_profmap.py --stub > $@
$(OUT)/dc_prof_stub.o $(OUT)/dc_prof_map.o: $(OUT)/%.o: $(OUT)/%.c
	kos-cc -O1 -c $< -o $@
$(OUT)/m2hle2_pass1.elf: $(OBJS) $(OUT)/dc_prof_stub.o
	kos-cc -o $@ $^ -lpthread -lm
$(OUT)/dc_prof_map.c: $(OUT)/m2hle2_pass1.elf tools/dc_profmap.py
	$(NM) -n -S $< | python3 tools/dc_profmap.py > $@
$(OUT)/m2hle2.elf: $(OBJS) $(OUT)/dc_prof_map.o
	kos-cc -o $@ $^ -lpthread -lm
	$(NM) -n $(OUT)/m2hle2_pass1.elf | grep ' [tTW] ' > $(OUT)/pass1.syms
	$(NM) -n $@ | grep ' [tTW] ' > $(OUT)/pass2.syms
	cmp -s $(OUT)/pass1.syms $(OUT)/pass2.syms || { echo "HUD=prof: the code moved between the two links"; rm -f $@; exit 1; }
else
$(OUT)/m2hle2.elf: $(OBJS)
	kos-cc -o $@ $(OBJS) -lpthread -lm
endif

# A GD-ROM image boots its program unscrambled (a CD-R would need scramble).
$(OUT)/1ST_READ.BIN: $(OUT)/m2hle2.elf
	$(KOS_OBJCOPY) -R .stack -O binary $< $@
	$(KOS_CC_BASE)/bin/$(KOS_CC_PREFIX)-size $<

clean:
	rm -f $(OBJS) $(OUT)/m2hle2.elf $(OUT)/1ST_READ.BIN $(OUT)/aot_gen.h $(OUT)/flags.stamp $(OUT)/aot.stamp \
	      $(OUT)/m2hle2_pass1.elf $(OUT)/dc_prof_stub.[co] $(OUT)/dc_prof_map.[co] $(OUT)/pass[12].syms

.PHONY: all clean
