#!/bin/bash
# Updates the device's m2hle from the rolling canary release: the standalone
# emulator (m2hle-sa), the RetroArch core (m2hle_libretro.so), or both.
#
#   m2hle-update.sh                  check, and install what is newer
#   m2hle-update.sh --force          reinstall the canary even if it is current
#   m2hle-update.sh --check [SECS]   check only; skipped if the last check is
#                                    younger than SECS. Exit 0 = update waiting,
#                                    1 = current, 2 = could not tell (offline...)
#   m2hle-update.sh --tool           what ES's Tools > "Update m2hle" runs: the
#                                    default, plus an on-screen result
#   --component sa|core              (with any of the above) only that one, and
#                                    install it even if it is not installed yet
#
# By default it updates whichever of the two is installed. The zip for each is
# found by searching the release's assets for this device's platform and CPU
# (uname), so the same script serves an arm64 and an x86_64 device.
#
# "Newer" is the zip's own sha256 (the release asset's digest), not the release
# notes: when a CI job fails, the notes name the new commit but the zip on the
# release is still the old one.
#
# The standalone emulator installs with the new zip's own install-es.sh, and the
# core with its install-rocknix.sh, and both install this script, so it updates
# itself.
set -uo pipefail

REPO=biggestsonicfan/m2-hle2
TAG=canary
API="https://api.github.com/repos/$REPO/releases/tags/$TAG"

DIR=/storage/.local/share/m2hle
STAGE="$DIR/update"
AVAILABLE="$DIR/UPDATE_AVAILABLE"     # present while an update is waiting
CHECKED="$STAGE/last-check"           # its mtime is when the canary was last asked
CORES=/tmp/cores                      # overlay over /storage/cores; write through it
ES=/storage/.emulationstation

MODE=install
MAX_AGE=0
ONLY=""
while [ $# -gt 0 ]; do
  case "$1" in
    --check) MODE=check
             if [[ "${2:-}" =~ ^[0-9]+$ ]]; then MAX_AGE=$2; shift; fi ;;
    --force) MODE=force ;;
    --tool)  MODE=tool ;;
    --component)
      case "${2:-}" in sa|core) ONLY=$2; shift ;;
        *) echo "--component takes sa or core" >&2; exit 2 ;; esac ;;
    *) echo "usage: $0 [--check [SECS] | --force | --tool] [--component sa|core]" >&2; exit 2 ;;
  esac
  shift
done

log() { [ "$MODE" = check ] || echo "$*"; }

notify() {  # tool mode only: a result that stays up until a button
  [ "$MODE" = tool ] && [ -x /usr/bin/sdl2notify ] && /usr/bin/sdl2notify --center "$1" 255 255 255 "${2:-8}" >/dev/null 2>&1
  return 0
}

fail() {  # before any component: nothing can be done
  log "m2hle update: $1"
  notify "m2hle update failed||$1"
  exit 2
}

# ---- This device's platform and CPU, as the release's asset names spell them.
case "$(uname -s)" in
  Linux)  OS=linux ;;
  Darwin) OS=macos ;;
  *)      OS=$(uname -s | tr 'A-Z' 'a-z') ;;
esac
[ "$(uname -o 2>/dev/null)" = Android ] && OS=android
case "$(uname -m)" in
  aarch64|arm64)  ARCH=arm64;  ARCH_ALT=aarch64; ELF_MACHINE=AArch64 ;;
  x86_64|amd64)   ARCH=x64;    ARCH_ALT=x86_64;  ELF_MACHINE=X86-64 ;;
  *)              ARCH=$(uname -m); ARCH_ALT=$ARCH; ELF_MACHINE="" ;;
esac

# Asset names to look for, best first. The first one on the release wins.
candidates() {
  case "$1" in
    sa)   echo "m2hle-rocknix-$ARCH.zip m2hle-rocknix-$ARCH_ALT.zip" ;;
    core) echo "m2hle-libretro-$OS-$ARCH.zip m2hle-libretro-$OS-$ARCH_ALT.zip"
          [ "$OS" = macos ] && echo "m2hle-libretro-macos-universal.zip" ;;
  esac
}

installed() {
  case "$1" in
    sa)   [ -x "$DIR/m2hle" ] ;;
    core) [ -f "$CORES/m2hle_libretro.so" ] ;;
  esac
}

label() { case "$1" in sa) echo "m2hle" ;; core) echo "RetroArch core" ;; esac; }

# Where each component keeps the digest and VERSION.txt of the zip it came from.
state_dir() { case "$1" in sa) echo "$DIR" ;; core) echo "$DIR/libretro" ;; esac; }

if [ -n "$ONLY" ]; then
  COMPONENTS=("$ONLY")
else
  COMPONENTS=()
  for c in sa core; do installed "$c" && COMPONENTS+=("$c"); done
  if [ ${#COMPONENTS[@]} -eq 0 ]; then
    fail "neither m2hle nor its RetroArch core is installed (--component sa|core installs one)"
  fi
fi

mkdir -p "$STAGE"
# One run at a time: the launcher's background check and the Tools entry share
# the staging directory.
exec 9>"$STAGE/lock"
if ! flock -n 9; then
  log "another m2hle update is running"
  exit 2
fi

if [ "$MODE" = check ] && [ "$MAX_AGE" -gt 0 ] && [ -f "$CHECKED" ] \
   && [ $(( $(date +%s) - $(stat -c %Y "$CHECKED") )) -lt "$MAX_AGE" ]; then
  [ -f "$AVAILABLE" ] && exit 0 || exit 1
fi

log "This device: $OS-$ARCH"
log "Asking GitHub for the $TAG release..."
JSON=$(curl -fsSL --max-time 20 -H 'Accept: application/vnd.github+json' "$API") \
  || fail "could not reach GitHub (is Wi-Fi on?)"
touch "$CHECKED"

# ---- One component: check, and unless checking, download, test and install.
# Returns 0 = updated (or, checking, waiting), 1 = current, 2 = failed.
# Sets NEW_VERSION, and ERR on failure.
update_one() {
  local c=$1 name st asset="" url digest updated zip new
  name=$(label "$c")
  st=$(state_dir "$c")
  ERR=""
  NEW_VERSION=""

  for n in $(candidates "$c"); do
    if jq -e --arg n "$n" 'any(.assets[]; .name == $n)' >/dev/null <<<"$JSON"; then asset=$n; break; fi
  done
  if [ -z "$asset" ]; then
    ERR="the $TAG release has no $name for $OS-$ARCH (looked for: $(candidates "$c" | xargs))"
    return 2
  fi

  read -r url digest updated < <(jq -r --arg n "$asset" \
    '.assets[] | select(.name == $n) | "\(.browser_download_url) \(.digest // "-") \(.updated_at)"' <<<"$JSON")
  digest="${digest#sha256:}"
  [[ "$digest" =~ ^[0-9a-f]{64}$ ]] || { ERR="GitHub gave no sha256 for $asset"; return 2; }

  log "$name: $(cat "$st/VERSION.txt" 2>/dev/null || echo "version unknown") installed; $asset built $updated"
  if [ "$MODE" != force ] && installed "$c" && [ "$digest" = "$(cat "$st/INSTALLED_SHA256" 2>/dev/null)" ]; then
    NEW_VERSION=$(cat "$st/VERSION.txt" 2>/dev/null)
    return 1
  fi

  zip="$STAGE/$asset"
  if [ "$(sha256sum "$zip" 2>/dev/null | cut -d' ' -f1)" != "$digest" ]; then
    log "Downloading $asset..."
    curl -fsSL --max-time 300 -o "$zip.part" "$url" || { rm -f "$zip.part"; ERR="download failed"; return 2; }
    mv -f "$zip.part" "$zip"
  fi
  [ "$(sha256sum "$zip" | cut -d' ' -f1)" = "$digest" ] || { rm -f "$zip"; ERR="the download's sha256 does not match GitHub's"; return 2; }

  # The zips are flat, so emptying the directory is one level of files.
  new="$STAGE/$c"
  mkdir -p "$new"
  rm -f "$new"/*
  unzip -o -q "$zip" -d "$new" || { ERR="could not unpack $asset"; return 2; }
  NEW_VERSION=$(cat "$new/VERSION.txt" 2>/dev/null || echo "canary $updated")

  local req
  case "$c" in
    sa)   req="m2hle install-es.sh start_m2hle.sh VERSION.txt" ;;
    core) req="m2hle_libretro.so m2hle_libretro.info" ;;
  esac
  for f in $req; do [ -f "$new/$f" ] || { ERR="$asset has no $f"; return 2; }; done

  # Installed by hand (from an unpacked download) leaves no digest behind: the
  # same binary, or for the emulator the same VERSION.txt, means it is this one.
  if [ "$MODE" != force ] && installed "$c"; then
    local same=0
    case "$c" in
      sa)   cmp -s "$new/m2hle" "$DIR/m2hle" && same=1
            [ "$NEW_VERSION" = "$(cat "$st/VERSION.txt" 2>/dev/null)" ] && same=1 ;;
      core) cmp -s "$new/m2hle_libretro.so" "$CORES/m2hle_libretro.so" && same=1 ;;
    esac
    if [ "$same" = 1 ]; then
      mkdir -p "$st"
      echo "$digest" > "$st/INSTALLED_SHA256"
      install -m 644 "$new/VERSION.txt" "$st/VERSION.txt" 2>/dev/null
      return 1
    fi
  fi

  [ "$MODE" = check ] && return 0

  # The new build has to load on this device before it replaces the one that
  # does.
  case "$c" in
    sa)
      # A bad argument prints usage and exits 2; a missing library is 127, a
      # wrong architecture 126, a crash 128+signal.
      chmod +x "$new/m2hle"
      "$new/m2hle" --update-selftest >/dev/null 2>&1
      local rc=$?
      [ "$rc" -eq 2 ] || { ERR="the new m2hle does not start here (exit $rc); nothing was changed"; return 2; }
      ;;
    core)
      # A core can't be run, so: the right CPU, every library it needs is
      # here, and it is a libretro core at all.
      if [ -n "$ELF_MACHINE" ] && command -v readelf >/dev/null; then
        readelf -h "$new/m2hle_libretro.so" 2>/dev/null | grep -qi "Machine:.*$ELF_MACHINE" \
          || { ERR="the new core is not built for $ARCH; nothing was changed"; return 2; }
        readelf --dyn-syms -W "$new/m2hle_libretro.so" 2>/dev/null | grep -q ' retro_run$' \
          || { ERR="the new core has no retro_run; nothing was changed"; return 2; }
      fi
      if command -v ldd >/dev/null && ldd "$new/m2hle_libretro.so" 2>&1 | grep -q 'not found'; then
        ERR="the new core needs a library this device lacks: $(ldd "$new/m2hle_libretro.so" 2>&1 | grep 'not found' | awk '{print $1}' | xargs); nothing was changed"
        return 2
      fi
      # Replacing the file under a running RetroArch is safe on Linux, but
      # the game would keep the old one until RetroArch restarts; say so.
      if pgrep -x retroarch >/dev/null; then
        ERR="RetroArch is running; close it and update again"
        return 2
      fi
      ;;
  esac

  log "Installing $name $NEW_VERSION..."
  case "$c" in
    sa)
      bash "$new/install-es.sh" || { ERR="install-es.sh failed; the previous binary is kept as $DIR/m2hle.bak-*"; return 2; }
      ;;
    core)
      if [ -f "$new/install-rocknix.sh" ]; then
        bash "$new/install-rocknix.sh" || { ERR="install-rocknix.sh failed; the previous core is kept as /storage/cores/m2hle_libretro.so.bak-*"; return 2; }
      else
        # A zip without ROCKNIX's installer: just the core and its info.
        local stamp; stamp=$(date +%Y%m%d-%H%M%S)
        [ -f "$CORES/m2hle_libretro.so" ] && cp -a "$CORES/m2hle_libretro.so" "/storage/cores/m2hle_libretro.so.bak-$stamp"
        install -m 755 "$new/m2hle_libretro.so" "$CORES/m2hle_libretro.so" \
          && install -m 644 "$new/m2hle_libretro.info" "$CORES/m2hle_libretro.info" \
          || { ERR="could not write $CORES"; return 2; }
      fi
      # A stale write to the overlay's upper directory shows the old core
      # through /tmp/cores; make sure the one RetroArch loads is the new one.
      cmp -s "$new/m2hle_libretro.so" "$CORES/m2hle_libretro.so" \
        || { ERR="$CORES still shows the old core (remount /tmp/cores or reboot)"; return 2; }
      # RetroArch shows the version and extensions from its cache of the .info.
      [ -f /storage/cores/core_info.cache ] && mv -f /storage/cores/core_info.cache /storage/cores/core_info.cache.old
      ;;
  esac

  mkdir -p "$st"
  [ -f "$new/VERSION.txt" ] && install -m 644 "$new/VERSION.txt" "$st/VERSION.txt"
  echo "$digest" > "$st/INSTALLED_SHA256"
  return 0
}

es_before=$(cat "$ES/es_systems.cfg" "$ES/es_features.cfg" 2>/dev/null | md5sum)
UPDATED=() WAITING=() FAILED=() CURRENT=()
for c in "${COMPONENTS[@]}"; do
  update_one "$c"
  case $? in
    0) if [ "$MODE" = check ]; then WAITING+=("$(label "$c") $NEW_VERSION")
       else UPDATED+=("$(label "$c") $NEW_VERSION"); log "Updated $(label "$c"): $NEW_VERSION"; fi ;;
    1) CURRENT+=("$(label "$c") $NEW_VERSION"); log "$(label "$c") is up to date." ;;
    *) FAILED+=("$(label "$c"): $ERR"); log "m2hle update: $(label "$c"): $ERR" ;;
  esac
done
es_after=$(cat "$ES/es_systems.cfg" "$ES/es_features.cfg" 2>/dev/null | md5sum)

join() { local IFS=,; echo "$*" | sed 's/,/, /g'; }

if [ "$MODE" = check ]; then
  if [ ${#WAITING[@]} -gt 0 ]; then join "${WAITING[@]}" > "$AVAILABLE"; exit 0; fi
  [ ${#FAILED[@]} -gt 0 ] && exit 2
  rm -f "$AVAILABLE"
  exit 1
fi

# Whatever did not fail is now current.
[ ${#FAILED[@]} -eq 0 ] && rm -f "$AVAILABLE"

if [ ${#FAILED[@]} -gt 0 ]; then
  msg="$(join "${FAILED[@]}")"
  [ ${#UPDATED[@]} -gt 0 ] && msg="updated $(join "${UPDATED[@]}"); $msg"
  notify "m2hle update failed||$msg"
  RC=2
elif [ ${#UPDATED[@]} -gt 0 ]; then
  RC=0
else
  notify "m2hle is up to date||$(join "${CURRENT[@]}")" 4
  exit 1
fi

if [ ${#UPDATED[@]} -gt 0 ]; then
  if [ "$es_before" != "$es_after" ]; then
    log "The ES configuration changed; restarting ES."
    [ "$RC" = 0 ] && notify "m2hle updated||$(join "${UPDATED[@]}") - restarting ES for its new options" 6
    if [ "$MODE" = tool ]; then
      set +u; . /etc/profile; set -u   # for UI_SERVICE
      systemctl restart ${UI_SERVICE:-essway.service}
    fi
  else
    [ "$RC" = 0 ] && notify "m2hle updated||$(join "${UPDATED[@]}")" 6
  fi
fi
exit $RC
