#!/bin/sh
# build.sh — qtlink.bin, the image qt960_link.lua types into NINDY (and qtlink.elf).
# Needs the i960-elf toolchain (/opt/i960/bin in the dev container, or I960_BIN).
set -e
cd "$(dirname "$0")"
BIN=${I960_BIN:-/opt/i960/bin}
OUT=${OUT:-.}
"$BIN/i960-elf-gcc" -mkb -ffreestanding -nostdlib -Os -Wall -T qtlink.ld \
    -o "$OUT/qtlink.elf" crt0.s stubs.s qtlink.c -lgcc
"$BIN/i960-elf-objcopy" -O binary -R .bss "$OUT/qtlink.elf" "$OUT/qtlink.bin"
"$BIN/i960-elf-size" "$OUT/qtlink.elf"
