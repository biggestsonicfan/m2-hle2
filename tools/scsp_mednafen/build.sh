#!/bin/sh
# Builds scsp_vs_mednafen with gcc/g++ (Linux, WSL, MSYS2 or macOS clang).
# Usage: tools/scsp_mednafen/build.sh [out-dir=tools/scsp_mednafen/build]
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$HERE/../.."
OUT="${1:-$HERE/build}"
sh "$HERE/fetch.sh"
mkdir -p "$OUT"
${CXX:-g++} -O2 -std=gnu++17 -I"$HERE/mednafen/src/ss" -c "$HERE/mdfn_scsp.cpp" -o "$OUT/mdfn_scsp.o"
${CC:-gcc} -O2 -std=c11 -I"$HERE" -I"$ROOT/src/board" -I"$ROOT/src/core" -I"$ROOT/src" \
    -c "$HERE/scsp_vs_mednafen.c" -o "$OUT/scsp_vs_mednafen.o"
${CXX:-g++} "$OUT/scsp_vs_mednafen.o" "$OUT/mdfn_scsp.o" -o "$OUT/scsp_vs_mednafen" -lm
# snd_lockstep: the whole sound board (68000 + driver), which needs the ROM
# loader's miniz. Its export header is a CMake product, so a stub stands in.
mkdir -p "$OUT/inc"
[ -f "$OUT/inc/miniz_export.h" ] || echo '#define MINIZ_EXPORT' > "$OUT/inc/miniz_export.h"
${CC:-gcc} -O2 -std=gnu11 -I"$HERE" -I"$OUT/inc" -I"$ROOT/src" -I"$ROOT/src/board" -I"$ROOT/src/core" \
    -I"$ROOT/src/profiles" -I"$ROOT/vendor/miniz" -c "$HERE/snd_lockstep.c" -o "$OUT/snd_lockstep.o"
for m in miniz miniz_tdef miniz_tinfl miniz_zip; do
    ${CC:-gcc} -O2 -I"$OUT/inc" -I"$ROOT/vendor/miniz" -c "$ROOT/vendor/miniz/$m.c" -o "$OUT/$m.o"
done
${CXX:-g++} "$OUT/snd_lockstep.o" "$OUT/mdfn_scsp.o" "$OUT"/miniz*.o -o "$OUT/snd_lockstep" -lm
echo "$OUT/scsp_vs_mednafen"
echo "$OUT/snd_lockstep"
