#!/usr/bin/env bash
# Add the m2-hle libretro core to a batocera.linux checkout, or move the one
# already there to another m2-hle2 commit. What it changes is what the Batocera
# wiki's "How to add an emulator" asks of a libretro core (README.md here):
#
#   package/batocera/emulators/retroarch/libretro/libretro-m2hle/
#       Config.in, libretro-m2hle.mk (from the .mk.in, pinned to --rev and the
#       submodule commits that revision records), m2hle.libretro.core.yml
#   Config.in                                    sources the package
#   package/batocera/core/batocera-system/Config.in
#                                                selects it with the arcade systems
#   python-src/batocera-launch-libretro/pyproject.toml
#                                                names the core for batocera-launch
#
# Usage: add-to-batocera.sh <batocera.linux checkout> [--rev <m2-hle2 commit>]
# --rev defaults to HEAD of the m2-hle2 checkout this script is in. It must be a
# commit GitHub has: the package downloads that commit's tarball. Re-running is
# safe; only the package directory is rewritten.
set -euo pipefail

usage() { echo "usage: $0 <batocera.linux checkout> [--rev <m2-hle2 commit>]" >&2; exit 2; }

BATO=""
REV=HEAD
while [ $# -gt 0 ]; do
    case "$1" in
        --rev) [ $# -ge 2 ] || usage; REV=$2; shift 2 ;;
        -h|--help) usage ;;
        *) [ -z "$BATO" ] || usage; BATO=$1; shift ;;
    esac
done
[ -n "$BATO" ] || usage

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(git -C "$HERE" rev-parse --show-toplevel)
for f in Config.in package/batocera/core/batocera-system/Config.in \
         python-src/batocera-launch-libretro/pyproject.toml; do
    [ -f "$BATO/$f" ] || { echo "$BATO is not a batocera.linux checkout (no $f)" >&2; exit 1; }
done

COMMIT=$(git -C "$REPO" rev-parse --verify "$REV^{commit}")
SHORT=$(git -C "$REPO" rev-parse --short=7 "$COMMIT")
# The canary's version stamp (.github/workflows/canary.yml): rNNN-<sha>.
DESCRIBE="r$(git -C "$REPO" rev-list --count "$COMMIT")-$SHORT"

submodule() {
    local sha
    sha=$(git -C "$REPO" ls-tree "$COMMIT" "vendor/$1" | awk '$2 == "commit" { print $3 }')
    [ -n "$sha" ] || { echo "vendor/$1 is not a submodule at $SHORT" >&2; exit 1; }
    echo "$sha"
}

PKG="$BATO/package/batocera/emulators/retroarch/libretro/libretro-m2hle"
mkdir -p "$PKG"
cp "$HERE/libretro-m2hle/Config.in" "$HERE/libretro-m2hle/m2hle.libretro.core.yml" "$PKG/"
sed -e "s/@M2HLE_COMMIT@/$COMMIT/g" \
    -e "s/@M2HLE_COMMIT_SHORT@/$SHORT/g" \
    -e "s/@M2HLE_DESCRIBE@/$DESCRIBE/g" \
    -e "s/@SOKOL@/$(submodule sokol)/g" \
    -e "s/@MINIZ@/$(submodule miniz)/g" \
    -e "s/@STB@/$(submodule stb)/g" \
    -e "s/@LUA@/$(submodule lua)/g" \
    -e "s/@GGPO@/$(submodule ggpo)/g" \
    "$HERE/libretro-m2hle/libretro-m2hle.mk.in" > "$PKG/libretro-m2hle.mk"
if grep -n '@[A-Z_]*@' "$PKG/libretro-m2hle.mk"; then
    echo "libretro-m2hle.mk.in has a placeholder this script does not fill" >&2; exit 1
fi

# Each edit below adds one line beside a neighbour batocera already has, and
# only if the line is not there yet.
add_after() {   # <file> <line to add> <grep -E pattern of the line it follows>
    local file=$1 line=$2 anchor=$3 n
    grep -qxF -- "$line" "$file" && return 0
    n=$(grep -nE -- "$anchor" "$file" | head -1 | cut -d: -f1)
    [ -n "$n" ] || { echo "$file: no line matches '$anchor' to put the m2hle line after" >&2; exit 1; }
    sed -i "${n}a\\
$line" "$file"
}

add_after "$BATO/Config.in" \
    '      source "$BR2_EXTERNAL_BATOCERA_PATH/package/batocera/emulators/retroarch/libretro/libretro-m2hle/Config.in"' \
    'libretro/libretro-flycast/Config.in"'

# Beside sm2-emu, the standalone Model 2 emulator, in the arcade systems.
SYS="$BATO/package/batocera/core/batocera-system/Config.in"
if ! grep -q 'BR2_PACKAGE_LIBRETRO_M2HLE' "$SYS"; then
    add_after "$SYS" '	select BR2_PACKAGE_LIBRETRO_M2HLE		if BR2_PACKAGE_BATOCERA_TARGET_X86_64_ANY || BR2_PACKAGE_BATOCERA_GLES3' \
        'select BR2_PACKAGE_SM2_EMU'
    add_after "$SYS" '	# Sega Model 2 libretro core (m2-hle)' 'select BR2_PACKAGE_SM2_EMU'
fi

# batocera-launch gives a core no entry the bare Core class, not RetroArch's.
PY="$BATO/python-src/batocera-launch-libretro/pyproject.toml"
if ! grep -q '^m2hle = ' "$PY"; then
    # in its place among the others, which are in order of name
    python3 - "$PY" <<'EOF'
import sys
path = sys.argv[1]
lines = open(path).read().split('\n')
at = lines.index('[project.entry-points."batocera_launch_libretro.cores"]') + 1
while at < len(lines) and '=' in lines[at] and lines[at].split('=')[0].strip() < 'm2hle':
    at += 1
lines.insert(at, "m2hle = 'batocera_launch_libretro:RACore'")
open(path, 'w').write('\n'.join(lines))
EOF
fi

echo "libretro-m2hle $DESCRIBE ($COMMIT) is in $BATO"
