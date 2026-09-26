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
echo "$OUT/scsp_vs_mednafen"
