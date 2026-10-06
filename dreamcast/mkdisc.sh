#!/bin/sh
# mkdisc.sh -- a GD-ROM image (.gdi) of the Dreamcast build and a ROM set:
#
#   dreamcast/mkdisc.sh <out dir with 1ST_READ.BIN> <stf_rom dir> <disc dir> [STF.AFS]
#
# <stf_rom dir> holds the PS3 release's ROM files (rom_code1.bin, rom_data.bin,
# rom_ep.bin, rom_pol.bin, rom_tex.bin); they go onto the disc as they are,
# grafted in, never copied (dc_layout.h says where each one lands). STF.AFS,
# the sound (tools/mksound.py), is grafted in the same way; without it the
# game runs silent. MODELS.PAK, the meshes and UV streams the game draws laid
# out scene by scene (tools/dc_mdlpack.py, dc_pager.h), is made here from
# rom_pol.bin and rom_tex.bin and dreamcast/sfight.mdlmap; NOPAK=1 leaves it out.
# STRIPS.PAK, the same meshes already walked into Tile Accelerator strips
# (tools/dc_strips.c, dc_strips.h), is made here too, from the ROM files and
# dreamcast/sfight.strips, with the host's C compiler ($CC, else cc);
# NOSTRIPS=1 leaves it out.
# TEXPAK=<file> adds TEXTURES.PAK, the PVR textures the recorded frames cut
# (det_digest --tex-pack, dc_texpak.h); it is texture RAM as the game filled
# it, so recorded from the ROM files on the host, not made here.
# SINCOS=<file> adds the COP's sin/cos tables (tools/mksincos.py, from the
# player's arcade set), which the link to MAME needs (dc_link.h).
#
# Three tracks, as a real GD-ROM: track 1 a small ISO in the low-density area
# (an emulator reads none of it), track 2 an empty audio track, track 3 the
# high-density data track at LBA 45000 with IP.BIN in its system area and
# 1ST_READ.BIN and the ROM files in its file system. A GD image boots its
# program unscrambled. Needs genisoimage and KallistiOS's makeip ($KOS_BASE).
#
# CDI=1 makes a self-booting CD-R image (.cdi, DiscJuggler) instead, for a
# burned CD-R and the emulators that take no GDI, the way Lazyboot (Conkwer's
# selfboot toolkit) makes one for a KallistiOS game with its "mastering"
# preset: one audio/data CD, the files on its data track at LBA 11702 (msinfo
# 0,11702) with Joliet and Rock Ridge names, Lazyboot's KOS IP.BIN, 1ST_READ.BIN
# scrambled as a MIL-CD boots it, no binhack, and a hidden dummy file (0.0)
# filling the disc to 80 minutes ahead of the game's files, which puts them on
# the disc's outer edge, where a drive reads fastest. The CDI is written by
# cdi4dc, with EDC/ECC (img4dc; Linux build: dreamcast/tools/build-cdi4dc.sh).
# DUMMY=0 leaves the dummy out (a ~190 MB image, not ~810 MB); FAST=1 writes
# the CDI with Lazyboot's mkcdi.py instead, without EDC/ECC (its "fast"
# preset: for emulators, not for burning). Needs KOS's scramble and Lazyboot's
# files (dreamcast/tools/get-lazyboot.sh); $SCRAMBLE, $LAZYBOOT and $CDI4DC
# override the paths.
# The ROM files are never committed: they come from the player's own copy.
set -eu
out=$1 roms=$2 disc=$3 afs=${4:-}
makeip=${MAKEIP:-$KOS_BASE/utils/makeip/makeip}
scramble=${SCRAMBLE:-$KOS_BASE/utils/scramble/scramble}
cdi4dc=${CDI4DC:-$(command -v cdi4dc || echo "$HOME/build/tools/dc/img4dc/cdi4dc/cdi4dc")}
lazyboot=${LAZYBOOT:-$HOME/build/tools/dc/lazyboot}

mkdir -p "$disc/low"
"$makeip" -f -g "M2-HLE2" -c "PINBOARD" -e "V0.100" "$disc/IP.BIN" >/dev/null
echo "m2-hle2 Dreamcast: this is the data disc's low-density area." > "$disc/low/README.TXT"

if [ -z "${CDI:-}" ]; then
    genisoimage -quiet -V M2HLE2 -l -o "$disc/track01.iso" "$disc/low"
    dd if=/dev/zero of="$disc/track02.raw" bs=2352 count=300 status=none
fi
set --
for f in rom_code1 rom_data rom_ep rom_pol rom_tex; do
    [ -f "$roms/$f.bin" ] || { echo "no $roms/$f.bin" >&2; exit 1; }
    set -- "$@" "$(echo $f | tr a-z A-Z).BIN=$roms/$f.bin"
done
[ -z "$afs" ] || set -- "$@" "STF.AFS=$afs"
[ -z "${SINCOS:-}" ] || set -- "$@" "SINCOS.BIN=$SINCOS"
[ -z "${TEXPAK:-}" ] || set -- "$@" "TEXTURES.PAK=$TEXPAK"
if [ -z "${NOPAK:-}" ]; then
    here=$(dirname "$0")
    python3 "$here/../tools/dc_mdlpack.py" --map "$here/sfight.mdlmap" --roms "$roms" --out "$disc/MODELS.PAK" >/dev/null
    set -- "$@" "MODELS.PAK=$disc/MODELS.PAK"
fi
if [ -z "${NOSTRIPS:-}" ]; then
    src=$(dirname "$0")/..
    ${CC:-cc} -O2 -w -o "$disc/dc_strips" "$src/tools/dc_strips.c" -I"$src/src" -I"$src/src/board" -I"$src/src/core" \
        -I"$src/src/net" -I"$src/src/ui" -I"$src/src/profiles" -I"$src/dreamcast" -lm
    "$disc/dc_strips" --roms "$roms" --keys "$src/dreamcast/sfight.strips" --out "$disc/STRIPS.PAK"
    set -- "$@" "STRIPS.PAK=$disc/STRIPS.PAK"
fi
if [ -n "${CDI:-}" ]; then
    [ -f "$lazyboot/tools/boots3" ] || { echo "no $lazyboot/tools/boots3: run dreamcast/tools/get-lazyboot.sh" >&2; exit 1; }
    "$scramble" "$out/1ST_READ.BIN" "$disc/1ST_READ.SCR"
    set -- 1ST_READ.BIN="$disc/1ST_READ.SCR" "$@"
    hide=
    if [ "${DUMMY:-1}" != 0 ]; then
        # Lazyboot's dummy: an 80-minute disc (712841213 bytes) less 7 MB, less the data.
        data=0
        for g; do data=$((data + $(stat -L -c %s "${g#*=}"))); done
        rm -f "$disc/0.0"
        truncate -s $((712841213 - 7340032 - data)) "$disc/0.0"
        set -- 0.0="$disc/0.0" "$@"
        # genisoimage puts a hidden file last; the weight puts it first.
        echo "$disc/0.0 1" > "$disc/sort.txt"
        hide="-hide 0.0 -hide-joliet 0.0 -sort $disc/sort.txt"
    fi
    genisoimage -quiet -f -C 0,11702 -V M2HLE2 $hide -G "$lazyboot/tools/boots3" -l -J -r -graft-points \
        -o "$disc/data.iso" "$@"
    if [ -n "${FAST:-}" ]; then
        python3 "$lazyboot/tools/mkcdi.py" "$disc/data.iso" "$disc/m2hle2.cdi" -l 11702 >/dev/null
    else
        "$cdi4dc" "$disc/data.iso" "$disc/m2hle2.cdi" >/dev/null
    fi
    rm -f "$disc/data.iso" "$disc/1ST_READ.SCR" "$disc/0.0" "$disc/sort.txt"
    img=m2hle2.cdi
else
    genisoimage -quiet -f -C 0,45000 -V M2HLE2 -G "$disc/IP.BIN" -l -graft-points \
        -o "$disc/track03.iso" 1ST_READ.BIN="$out/1ST_READ.BIN" "$@"
    printf '3\n1 0 4 2048 track01.iso 0\n2 450 0 2352 track02.raw 0\n3 45000 4 2048 track03.iso 0\n' > "$disc/m2hle2.gdi"
    img=m2hle2.gdi
fi

find "$disc/low" -delete
rm -f "$disc/IP.BIN" "$disc/MODELS.PAK" "$disc/STRIPS.PAK" "$disc/dc_strips"
echo "$disc/$img"
