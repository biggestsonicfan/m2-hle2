#!/bin/sh
# mkdisc.sh -- a GD-ROM image (.gdi) of the Dreamcast build and a pack:
#
#   dreamcast/mkdisc.sh <out dir with 1ST_READ.BIN> <M2PACK.BIN> <disc dir>
#
# Three tracks, as a real GD-ROM: track 1 a small ISO in the low-density area
# (an emulator reads none of it), track 2 an empty audio track, track 3 the
# high-density data track at LBA 45000 with IP.BIN in its system area and
# 1ST_READ.BIN and M2PACK.BIN in its file system. A GD image boots its
# program unscrambled. Needs genisoimage and KallistiOS's makeip ($KOS_BASE).
# The pack is never committed: it is made from the player's own ROM set.
set -eu
out=$1 pack=$2 disc=$3
makeip=${MAKEIP:-$KOS_BASE/utils/makeip/makeip}

mkdir -p "$disc/files" "$disc/low"
cp "$out/1ST_READ.BIN" "$pack" "$disc/files/"
"$makeip" -f -g "M2-HLE2" -c "PINBOARD" -e "V0.100" "$disc/IP.BIN" >/dev/null
echo "m2-hle2 Dreamcast: this is the data disc's low-density area." > "$disc/low/README.TXT"

genisoimage -quiet -V M2HLE2 -l -o "$disc/track01.iso" "$disc/low"
dd if=/dev/zero of="$disc/track02.raw" bs=2352 count=300 status=none
genisoimage -quiet -C 0,45000 -V M2HLE2 -G "$disc/IP.BIN" -l -o "$disc/track03.iso" "$disc/files"
printf '3\n1 0 4 2048 track01.iso 0\n2 450 0 2352 track02.raw 0\n3 45000 4 2048 track03.iso 0\n' > "$disc/m2hle2.gdi"

find "$disc/files" "$disc/low" -delete
rm -f "$disc/IP.BIN"
echo "$disc/m2hle2.gdi"
