#!/bin/sh
# Downloads the Mednafen release scsp_vs_mednafen was written against into
# ./mednafen (git-ignored). Mednafen is GPL-2.0-or-later and stays out of the
# repository; nothing built from it is distributed.
set -e
cd "$(dirname "$0")"
VER=1.32.1
SHA=de7eb94ab66212ae7758376524368a8ab208234b33796625ca630547dbc83832
[ -f mednafen/src/ss/scsp.inc ] && exit 0
curl -fsSLo mednafen-$VER.tar.xz https://mednafen.github.io/releases/files/mednafen-$VER.tar.xz
echo "$SHA  mednafen-$VER.tar.xz" | sha256sum -c -
tar xJf mednafen-$VER.tar.xz mednafen/src/ss/scsp.h mednafen/src/ss/scsp.inc mednafen/COPYING
rm mednafen-$VER.tar.xz
