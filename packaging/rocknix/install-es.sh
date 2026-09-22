#!/bin/bash
# Installs m2hle on ROCKNIX: the binary, its launcher, the updater, and the
# EmulationStation configuration that puts Sega Model 2 in the carousel.
# Run it from the unpacked release (it takes its files from its own directory);
# the old cross-build flow's /tmp copies still work.
# Idempotent: re-running only refreshes what it installs.
#
# Everything it writes is under /storage and outside the paths ROCKNIX manages,
# so an OS upgrade leaves it alone. That is the whole point of the two
# es_*_m2hle.cfg drop-ins -- see the comment block in es_systems_m2hle.cfg.
set -euo pipefail
ES=/storage/.emulationstation
STAMP=$(date +%Y%m%d-%H%M%S)
HERE=$(cd "$(dirname "$0")" && pwd)

# The release names it m2hle; the cross-build script leaves /tmp/m2hle-aarch64.
for cand in "$HERE/m2hle" /tmp/m2hle-aarch64 /tmp/m2hle; do
  [ -f "$cand" ] && BIN="$cand" && break
done
[ -n "${BIN:-}" ] || { echo "no m2hle binary beside this script or in /tmp" >&2; exit 1; }
for cand in "$HERE/start_m2hle.sh" /tmp/start_m2hle.sh; do
  [ -f "$cand" ] && LAUNCHER="$cand" && break
done
[ -n "${LAUNCHER:-}" ] || { echo "no start_m2hle.sh beside this script or in /tmp" >&2; exit 1; }

mkdir -p /storage/.local/share/m2hle /storage/.local/bin
# Keep the binary that is being replaced: a canary is a development build, and
# stepping back to the one that worked should not need the network.
[ -f /storage/.local/share/m2hle/m2hle ] && cp -a /storage/.local/share/m2hle/m2hle "/storage/.local/share/m2hle/m2hle.bak-$STAMP"
install -m 755 "$BIN" /storage/.local/share/m2hle/m2hle
install -m 755 "$LAUNCHER" /storage/.local/bin/start_m2hle.sh
echo "installed $(basename "$BIN") -> /storage/.local/share/m2hle/m2hle"

# Which canary this is (m2hle-update.sh compares it), the updater itself, and
# its game-list entry. The old cross-build flow has none of these: skip them.
if [ -f "$HERE/VERSION.txt" ]; then
  install -m 644 "$HERE/VERSION.txt" /storage/.local/share/m2hle/VERSION.txt
  echo "version: $(cat "$HERE/VERSION.txt")"
fi
if [ -f "$HERE/m2hle-update.sh" ]; then
  install -m 755 "$HERE/m2hle-update.sh" /storage/.local/bin/m2hle-update.sh
fi

# ---- EmulationStation.
#
# ROCKNIX owns es_systems.cfg and es_features.cfg: /usr/share/post-update moves
# each aside to last_<name>.cfg and symlinks the read-only /usr/config copy over
# it on EVERY OS upgrade, "so they are managed with OS updates". An edit to
# either is therefore temporary -- 7.0.2 (20260919) is what proved it, and the
# same upgrade dropped the segamodel2 system and the sm2-emu package from the
# stock files, so the AM2 tile went with them.
#
# ES also merges every es_systems_*.cfg and es_features_*.cfg it finds beside
# them, nothing renames those, and a system defined in one takes precedence over
# the same name in the managed file (measured on 7.0.2: a drop-in segamodel2 beat
# a main-file segamodel2). So the drop-ins define the whole system, and they hold
# whether or not a future ROCKNIX brings its own back.
[ -d "$ES" ] || { echo "no $ES -- is EmulationStation installed?" >&2; exit 1; }
install -m 644 "$HERE/es_systems_m2hle.cfg"  "$ES/es_systems_m2hle.cfg"
install -m 644 "$HERE/es_features_m2hle.cfg" "$ES/es_features_m2hle.cfg"
echo "es_systems_m2hle.cfg / es_features_m2hle.cfg: Sega Model 2 installed as a drop-in"

# runemu.sh runs a standalone emulator's launcher from /usr/bin/start_<core>.sh
# and /usr is a read-only squashfs, so segamodel2's <command> goes through this
# wrapper instead. It re-derives its patched copy from the OS's own runemu.sh.
if [ -f "$HERE/m2hle-runemu.sh" ]; then
  install -m 755 "$HERE/m2hle-runemu.sh" /storage/.local/bin/m2hle-runemu.sh
fi

# An install from before the drop-ins may have left our block inside the managed
# files, and a whole forked copy of runemu.sh to go with it. The drop-in wins over
# both, and the next OS upgrade takes the edits away by itself, so say what is
# there rather than editing ROCKNIX's files to undo edits to ROCKNIX's files.
if [ -f "$ES/es_systems.cfg" ] && [ ! -L "$ES/es_systems.cfg" ] && grep -q 'm2hle-sa' "$ES/es_systems.cfg"; then
  echo "note: $ES/es_systems.cfg still carries the old in-place edit. The drop-in"
  echo "      takes precedence over it, and the next OS upgrade will replace the"
  echo "      file anyway; nothing to do."
fi
if [ -f /storage/.local/bin/runemu.sh ] && ! grep -qs 'local/bin/runemu.sh' "$ES"/es_systems*.cfg; then
  mv -f /storage/.local/bin/runemu.sh "/storage/.local/bin/runemu.sh.unused-$STAMP"
  echo "retired the forked /storage/.local/bin/runemu.sh (nothing refers to it now)"
fi

# Netplay settings copied from a PC: a netplay.cfg beside this script goes where
# the launcher's --net-config points. Private, since it holds a login. The one
# it replaces is kept, like the binary.
NETCFG_DIR=/storage/.config/m2hle2
if [ -f "$HERE/netplay.cfg" ]; then
  mkdir -p "$NETCFG_DIR" && chmod 700 "$NETCFG_DIR"
  if [ -f "$NETCFG_DIR/netplay.cfg" ] && ! cmp -s "$HERE/netplay.cfg" "$NETCFG_DIR/netplay.cfg"; then
    cp -a "$NETCFG_DIR/netplay.cfg" "$NETCFG_DIR/netplay.cfg.bak-$STAMP"
  fi
  install -m 600 "$HERE/netplay.cfg" "$NETCFG_DIR/netplay.cfg"
  echo "installed netplay.cfg -> $NETCFG_DIR/netplay.cfg"
fi

# "Update m2-hle" in the Sega Model 2 game list, beside the game: launching it
# runs m2hle-update.sh. (ES's Tools menu cannot hold it -- ROCKNIX's boot rsync
# deletes anything there; m2hle-update.sh says so at length.)
if [ -x /storage/.local/bin/m2hle-update.sh ]; then
  bash /storage/.local/bin/m2hle-update.sh --install-entry || echo "could not add the Update m2-hle entry" >&2
fi

echo
echo "restart EmulationStation for the Sega Model 2 tile to appear:"
echo "    systemctl restart \${UI_SERVICE##* }"
