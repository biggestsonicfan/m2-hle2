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
# SINCOS=<file> adds the COP's sin/cos tables (tools/mksincos.py, from the
# player's arcade set), which the link to MAME needs (dc_link.h).
#
# Three tracks, as a real GD-ROM: track 1 a small ISO in the low-density area
# (an emulator reads none of it), track 2 an empty audio track, track 3 the
# high-density data track at LBA 45000 with IP.BIN in its system area and
# 1ST_READ.BIN and the ROM files in its file system. A GD image boots its
# program unscrambled. Needs genisoimage and KallistiOS's makeip ($KOS_BASE).
#
# CDI=1 makes a self-booting CD-R image (.cdi, DiscJuggler) instead, for the
# emulators and players that take no GDI: one audio/data CD with the same files
# on its data track at LBA 11702 (msinfo 0,11702) and 1ST_READ.BIN scrambled,
# as a MIL-CD boots it. Needs KOS's scramble and cdi4dc (img4dc; Linux build:
# dreamcast/tools/build-cdi4dc.sh); $SCRAMBLE and $CDI4DC override the paths.
# The ROM files are never committed: they come from the player's own copy.
set -eu
out=$1 roms=$2 disc=$3 afs=${4:-}
makeip=${MAKEIP:-$KOS_BASE/utils/makeip/makeip}
scramble=${SCRAMBLE:-$KOS_BASE/utils/scramble/scramble}
cdi4dc=${CDI4DC:-$(command -v cdi4dc || echo "$HOME/build/tools/dc/img4dc/cdi4dc/cdi4dc")}

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
if [ -z "${NOPAK:-}" ]; then
    here=$(dirname "$0")
    python3 "$here/../tools/dc_mdlpack.py" --map "$here/sfight.mdlmap" --roms "$roms" --out "$disc/MODELS.PAK" >/dev/null
    set -- "$@" "MODELS.PAK=$disc/MODELS.PAK"
fi
if [ -n "${CDI:-}" ]; then
    "$scramble" "$out/1ST_READ.BIN" "$disc/1ST_READ.SCR"
    genisoimage -quiet -f -C 0,11702 -V M2HLE2 -G "$disc/IP.BIN" -l -graft-points \
        -o "$disc/data.iso" 1ST_READ.BIN="$disc/1ST_READ.SCR" "$@"
    "$cdi4dc" "$disc/data.iso" "$disc/m2hle2.cdi" >/dev/null
    rm -f "$disc/data.iso" "$disc/1ST_READ.SCR"
    img=m2hle2.cdi
else
    genisoimage -quiet -f -C 0,45000 -V M2HLE2 -G "$disc/IP.BIN" -l -graft-points \
        -o "$disc/track03.iso" 1ST_READ.BIN="$out/1ST_READ.BIN" "$@"
    printf '3\n1 0 4 2048 track01.iso 0\n2 450 0 2352 track02.raw 0\n3 45000 4 2048 track03.iso 0\n' > "$disc/m2hle2.gdi"
    img=m2hle2.gdi
fi

find "$disc/low" -delete
rm -f "$disc/IP.BIN" "$disc/MODELS.PAK"
echo "$disc/$img"
