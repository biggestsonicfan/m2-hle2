#!/bin/bash
# What es_systems_m2hle.cfg puts in segamodel2's <command>, in place of
# /usr/bin/runemu.sh.
#
# runemu.sh does everything a launch needs -- the CPU governor, the controller
# config, the logging, the exit code ES reads -- and we want all of it. The one
# thing it cannot do for us is find a standalone emulator's launcher: it runs
#
#     ${RUN_SHELL} "/usr/bin/start_${CORE%-*}.sh" "${ROMNAME}" "${PLATFORM}"
#
# and /usr is a read-only squashfs, so start_m2hle.sh cannot go there. It has to
# live in /storage/.local/bin.
#
# The obvious fix is a forked copy of runemu.sh with that path changed, and that
# is what was here before: /storage/.local/bin/runemu.sh, copied once. The
# trouble is that it is a copy of the runemu.sh of the day it was made, and every
# OS upgrade moves the real one on without it -- by 7.0.2 the fork had missed the
# exit-code handling that decides whether ES records a game as played. So derive
# the copy instead, from whatever runemu.sh the OS currently ships, and re-derive
# it whenever that file is newer than the copy. One sed, on a 16 KB file, once
# per upgrade.
#
# Nothing else is launched through here: a libretro core (--emulator=retroarch)
# and a .sh rom both take paths in runemu.sh that never look at /usr/bin/start_*,
# so for those this is exactly /usr/bin/runemu.sh.
set -uo pipefail

STOCK=/usr/bin/runemu.sh
PATCHED=/storage/.local/share/m2hle/runemu.sh

[ -x "$STOCK" ] || { echo "m2hle-runemu: no $STOCK" >&2; exit 2; }

if [ ! -f "$PATCHED" ] || [ "$STOCK" -nt "$PATCHED" ]; then
  mkdir -p "$(dirname "$PATCHED")"
  tmp="$PATCHED.new"
  # Only the one occurrence: the string is the whole of what makes the fork a
  # fork. If a future runemu.sh spells that line differently the sed is a no-op,
  # so say so rather than launching something that cannot work -- and still fall
  # through to the stock script, which is right for every other emulator.
  sed 's|"/usr/bin/start_${CORE|"/storage/.local/bin/start_${CORE|' "$STOCK" > "$tmp"
  if grep -q '"/storage/.local/bin/start_\${CORE' "$tmp"; then
    chmod 755 "$tmp" && mv -f "$tmp" "$PATCHED"
  else
    rm -f "$tmp"
    echo "m2hle-runemu: $STOCK no longer has the /usr/bin/start_\${CORE} line;" \
         "the standalone emulator will not launch until this is updated" >&2
    exec "$STOCK" "$@"
  fi
fi

exec "$PATCHED" "$@"
