#!/bin/bash
# Installs the m2-hle libretro core into ROCKNIX's RetroArch and lists it as a
# Sega Model 2 emulator in EmulationStation. Run it as root from the unpacked
# m2hle-libretro-linux-arm64.zip, over ssh:
#
#   ./install-rocknix.sh                  install; pick it per game in ES
#   ./install-rocknix.sh --make-default   ...and make it Model 2's emulator
#
# Idempotent: running it again only refreshes the core and its info file.
#
# It also installs m2hle-update.sh and the "Update m2-hle" entry in the Sega
# Model 2 game list, which keep the core current from the canary release from
# then on (m2hle-update.sh runs this script from each new zip).
# /storage/.local/share/m2hle/libretro/ holds the VERSION.txt of the zip
# installed last.
set -euo pipefail
STAMP=$(date +%Y%m%d-%H%M%S)
HERE=$(cd "$(dirname "$0")" && pwd)
ES=/storage/.emulationstation
SYSCFG=/storage/.config/system/configs/system.cfg
RA_CFG=/storage/.config/retroarch/config/m2-hle

MAKE_DEFAULT=0
for arg in "$@"; do
  case "$arg" in
    --make-default) MAKE_DEFAULT=1 ;;
    *) echo "usage: $0 [--make-default]" >&2; exit 2 ;;
  esac
done
for f in m2hle_libretro.so m2hle_libretro.info; do
  [ -f "$HERE/$f" ] || { echo "no $f beside this script" >&2; exit 1; }
done

# 1. The core. /tmp/cores is an overlay whose writable layer is /storage/cores,
#    so what is installed through it survives a reboot and an update.
if [ -f /tmp/cores/m2hle_libretro.so ] && ! cmp -s "$HERE/m2hle_libretro.so" /tmp/cores/m2hle_libretro.so; then
  cp -a /tmp/cores/m2hle_libretro.so "/storage/cores/m2hle_libretro.so.bak-$STAMP"
  echo "kept the previous core as /storage/cores/m2hle_libretro.so.bak-$STAMP"
fi
install -m 755 "$HERE/m2hle_libretro.so" /tmp/cores/m2hle_libretro.so
install -m 644 "$HERE/m2hle_libretro.info" /tmp/cores/m2hle_libretro.info
echo "installed the core -> /storage/cores/m2hle_libretro.so"
# RetroArch lists a core's name and version from its cache of the .info files.
[ -f /storage/cores/core_info.cache ] && mv -f /storage/cores/core_info.cache /storage/cores/core_info.cache.old

# 1b. Which canary this is, the updater (it updates this core and the
#     standalone m2hle from the canary release) and its game-list entry.
STATE=/storage/.local/share/m2hle/libretro
mkdir -p "$STATE"
if [ -f "$HERE/VERSION.txt" ]; then
  install -m 644 "$HERE/VERSION.txt" "$STATE/VERSION.txt"
  echo "version: $(cat "$HERE/VERSION.txt")"
fi
if [ -f "$HERE/m2hle-update.sh" ]; then
  mkdir -p /storage/.local/bin
  install -m 755 "$HERE/m2hle-update.sh" /storage/.local/bin/m2hle-update.sh
fi

# 2. The Sega Model 2 system, which is what lists this core as an emulator.
#
#    es_systems_m2hle.cfg is a drop-in: ES merges every es_systems_*.cfg beside
#    es_systems.cfg, and unlike es_systems.cfg itself nothing renames it on an OS
#    upgrade (/usr/share/post-update symlinks the stock copy over that one every
#    time). It already names retroarch / m2hle as the default emulator, so with it
#    in place there is nothing to add. It arrives with either installer, since a
#    device may have only the core.
#
#    This used to edit es_systems.cfg in place, and ROCKNIX 7.0.2 is where that
#    stopped working twice over: the edit is undone by the next upgrade, and 7.0.2
#    removed the segamodel2 system altogether, so there was no longer a block to
#    edit -- under set -e the failing awk took the rest of this script with it.
if [ -f "$ES/es_systems_m2hle.cfg" ]; then
  echo "es_systems_m2hle.cfg: Sega Model 2 already installed"
elif [ -f "$HERE/es_systems_m2hle.cfg" ]; then
  install -m 644 "$HERE/es_systems_m2hle.cfg" "$ES/es_systems_m2hle.cfg"
  [ -f "$HERE/m2hle-runemu.sh" ] && install -m 755 "$HERE/m2hle-runemu.sh" /storage/.local/bin/m2hle-runemu.sh
  echo "es_systems_m2hle.cfg: Sega Model 2 installed (restart EmulationStation)"
  echo "  the standalone emulator is offered too; m2hle-update.sh --component sa installs it"
else
  echo "warning: no es_systems_m2hle.cfg here or in $ES, so nothing lists this core" >&2
  echo "         as a Sega Model 2 emulator. Install the standalone zip as well." >&2
fi

# 2b. The core in ES's feature list, so the game's options offer RetroArch's
#     netplay for it. Only netplay: rewind and autosave need savestates, which
#     this core does not have.
#
#     This one does still edit es_features.cfg, and so is undone by the next OS
#     upgrade -- re-running this script puts it back. It is not a drop-in because
#     the entry has to go *inside* ROCKNIX's <emulator name="retroarch">, and a
#     drop-in that named that emulator again might replace its whole core list
#     rather than add to it, which would cost every other RetroArch core its
#     options. Losing netplay from one core's menu is the smaller risk; the core
#     carries its own RPCN lobby in its core options either way.
if [ -L "$ES/es_features.cfg" ] && [ ! -w "$(readlink -f "$ES/es_features.cfg")" ]; then
  echo "es_features.cfg is the OS's read-only copy; skipping the netplay feature"
elif grep -q '<core name="m2hle"' "$ES/es_features.cfg" 2>/dev/null; then
  echo "es_features.cfg: m2hle already listed"
else
  cp "$ES/es_features.cfg" "$ES/es_features.cfg.bak-$STAMP"
  if awk '
      /<emulator name="retroarch"/ { ra = 1 }
      { print }
      ra && /<cores>/ { print "      <core name=\"m2hle\" features=\"netplay\" />"; ra = 0; done = 1 }
      END { if (!done) exit 1 }
    ' "$ES/es_features.cfg.bak-$STAMP" > "$ES/es_features.cfg.new"; then
    mv "$ES/es_features.cfg.new" "$ES/es_features.cfg"
    echo "es_features.cfg: added m2hle to the RetroArch cores (backup es_features.cfg.bak-$STAMP)"
  else
    rm -f "$ES/es_features.cfg.new"
    echo "es_features.cfg: no <emulator name=\"retroarch\"> to add the core to; skipped"
  fi
fi

# 2c. "Update m2-hle" in the Sega Model 2 game list, beside the game. Not a
#     Tools entry: ROCKNIX's boot rsync deletes anything in
#     /storage/.config/modules. After 2, because it reads the system's <path>
#     and checks .sh is among its extensions.
if [ -x /storage/.local/bin/m2hle-update.sh ]; then
  bash /storage/.local/bin/m2hle-update.sh --install-entry || echo "could not add the Update m2-hle entry" >&2
fi

# 3. ROCKNIX's RetroArch keeps saves in the ROM folder, which is usually
#    shared on the network. The core keeps its RPCN login with its saves, so
#    for this core only, saves go to RetroArch's own (private) saves folder.
mkdir -p "$RA_CFG"
if [ ! -f "$RA_CFG/m2-hle.cfg" ]; then
  printf 'savefiles_in_content_dir = "false"\n' > "$RA_CFG/m2-hle.cfg"
  chmod 600 "$RA_CFG/m2-hle.cfg"
  echo "RetroArch: this core's saves go to /storage/.config/retroarch/saves"
fi

# 4. Optionally, the default for Model 2. EmulationStation writes system.cfg
#    back when it exits, so it is stopped around the edit; and ROCKNIX restores
#    system.cfg from system.cfg.backup after an unclean shutdown, so the backup
#    is refreshed too, or the first crash quietly puts the old emulator back.
if [ "$MAKE_DEFAULT" = 1 ]; then
  es_was_up=0
  if systemctl is-active --quiet essway.service; then es_was_up=1; systemctl stop essway.service; sleep 2; fi
  cp -a "$SYSCFG" "$SYSCFG.bak-$STAMP"
  for kv in "segamodel2.emulator=retroarch" "segamodel2.core=m2hle"; do
    k=${kv%%=*}
    if grep -q "^${k//./\\.}=" "$SYSCFG"; then sed -i "s|^${k//./\\.}=.*|$kv|" "$SYSCFG"; else echo "$kv" >> "$SYSCFG"; fi
  done
  /usr/bin/chksysconfig backup
  sync
  echo "system.cfg: Model 2 now uses RetroArch / m2hle (backup system.cfg.bak-$STAMP)"
  [ "$es_was_up" = 1 ] && systemctl start essway.service
else
  sync
  echo "Pick it per game in EmulationStation (the game's options > emulator), or"
  echo "run again with --make-default. Restart EmulationStation to see the new entry."
fi
[ -x /storage/.local/bin/m2hle-update.sh ] && echo "Updates: the Update m2-hle entry in the Sega Model 2 game list, or m2hle-update.sh over ssh."
exit 0
