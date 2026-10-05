#!/bin/sh
# get-lazyboot.sh -- the parts of Conkwer's Lazyboot (the Windows selfboot
# toolkit) that mkdisc.sh's CDI=1 uses on Linux:
#
#   dreamcast/tools/get-lazyboot.sh [dir]     # default ~/build/tools/dc/lazyboot
#
# tools/boots3, the IP.BIN Lazyboot puts on a KallistiOS game's disc, and
# tools/mkcdi.py, its CDI writer without EDC/ECC (its "fast" preset). The
# rest of it is .cmd scripts and Windows programs; mkdisc.sh does what its
# .cmd does for a KOS game. A sparse checkout: the whole tree is ~230 MB.
# Nothing of it is committed here.
set -eu
dir=${1:-$HOME/build/tools/dc/lazyboot}
rev=b75e56053049f31cd3f6059c45505fc5f9e45f5e

if [ ! -d "$dir/.git" ]; then
    git -c core.autocrlf=false clone -q --filter=blob:none --no-checkout https://github.com/Conkwer/lazyboot "$dir"
    git -C "$dir" sparse-checkout set --no-cone /tools/boots3 /tools/mkcdi.py /readme.txt /license.txt
fi
git -C "$dir" -c core.autocrlf=false checkout -q -f "$rev"
ls "$dir/tools/boots3" "$dir/tools/mkcdi.py"
