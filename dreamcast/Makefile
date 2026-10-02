# The Dreamcast build (Pinboard #340), with KallistiOS: source its environ.sh,
# then from the repo root
#
#   make -C dreamcast [VENDOR=<a vendor/ with miniz and sokol>] [OUT=<dir>]
#
# OUT (default ../build_dc) gets m2hle2.elf and 1ST_READ.BIN; mkdisc.sh makes a
# disc of them and a pack (m2pack). Only the board is compiled for the SH-4:
# the board and the profiles are headers, main_dc.c is the one unit, and
# miniz comes along because the profiles' zip loaders are in the table.

REPO   := ..
VENDOR ?= $(REPO)/vendor
OUT    ?= $(REPO)/build_dc

ifeq ($(KOS_BASE),)
$(error source KallistiOS's environ.sh first)
endif

CFLAGS := -std=gnu11 -O2 -fno-strict-aliasing -Wall -Wno-format -Wno-address \
          -DM2HLE_VERSION='"dc"' -DM2HLE_DEV_TOOLS=0 \
          -DGEO_CAPTURE_SIZE=4096 -DLOG_MAX_LINES=64 -DNDEBUG \
          -I$(REPO)/src -I$(REPO)/src/board -I$(REPO)/src/core -I$(REPO)/src/ui \
          -I$(REPO)/src/profiles -I$(REPO)/src/net -I. \
          -I$(VENDOR)/sokol -I$(VENDOR)/miniz -I$(OUT)/gen

MINIZ  := miniz miniz_tdef miniz_tinfl miniz_zip
OBJS   := $(OUT)/main_dc.o $(MINIZ:%=$(OUT)/%.o)

all: $(OUT)/1ST_READ.BIN

$(OUT)/gen/miniz_export.h:
	mkdir -p $(OUT)/gen
	printf '#ifndef MINIZ_EXPORT_H\n#define MINIZ_EXPORT_H\n#define MINIZ_EXPORT\n#define MINIZ_NO_EXPORT\n#endif\n' > $@

$(OUT)/main_dc.o: main_dc.c dc_pager.h dc_pack.h $(wildcard $(REPO)/src/*/*.h) $(OUT)/gen/miniz_export.h
	kos-cc $(CFLAGS) -c $< -o $@

$(OUT)/%.o: $(VENDOR)/miniz/%.c $(OUT)/gen/miniz_export.h
	kos-cc $(CFLAGS) -c $< -o $@

$(OUT)/m2hle2.elf: $(OBJS)
	kos-cc -o $@ $(OBJS) -lpthread -lm

# A GD-ROM image boots its program unscrambled (a CD-R would need scramble).
$(OUT)/1ST_READ.BIN: $(OUT)/m2hle2.elf
	$(KOS_OBJCOPY) -R .stack -O binary $< $@
	$(KOS_CC_BASE)/bin/$(KOS_CC_PREFIX)-size $<

clean:
	rm -f $(OBJS) $(OUT)/m2hle2.elf $(OUT)/1ST_READ.BIN

.PHONY: all clean
