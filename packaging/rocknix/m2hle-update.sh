#!/bin/bash
# Updates the device's m2hle from the rolling canary release.
#
#   m2hle-update.sh                  check, and install if the canary is newer
#   m2hle-update.sh --force          reinstall the canary even if it is current
#   m2hle-update.sh --check [SECS]   check only; skipped if the last check is
#                                    younger than SECS. Exit 0 = update waiting,
#                                    1 = current, 2 = could not tell (offline...)
#   m2hle-update.sh --tool           what ES's Tools > "Update m2hle" runs: the
#                                    default, plus an on-screen result
#
# "Newer" is the ARM zip's own sha256 (the release asset's digest), not the
# release notes: when the rocknix CI job fails, the notes name the new commit
# but the zip on the release is still the old one.
#
# The install is the new zip's own install-es.sh, so this script updates itself.
set -uo pipefail

REPO=biggestsonicfan/m2-hle2
TAG=canary
ASSET=m2hle-rocknix-arm64.zip
API="https://api.github.com/repos/$REPO/releases/tags/$TAG"

DIR=/storage/.local/share/m2hle
STAGE="$DIR/update"
DIGEST_FILE="$DIR/INSTALLED_SHA256"   # digest of the zip that was installed
AVAILABLE="$DIR/UPDATE_AVAILABLE"     # present while an update is waiting
CHECKED="$STAGE/last-check"           # its mtime is when the canary was last asked

MODE=install
MAX_AGE=0
case "${1:-}" in
  --check) MODE=check; MAX_AGE="${2:-0}" ;;
  --force) MODE=force ;;
  --tool)  MODE=tool ;;
  "") ;;
  *) echo "usage: $0 [--check [SECS] | --force | --tool]" >&2; exit 2 ;;
esac

log() { [ "$MODE" = check ] || echo "$*"; }

notify() {  # tool mode only: a result that stays up until a button
  [ "$MODE" = tool ] && [ -x /usr/bin/sdl2notify ] && /usr/bin/sdl2notify --center "$1" 255 255 255 "${2:-8}" >/dev/null 2>&1
  return 0
}

fail() {
  log "m2hle update: $1"
  notify "m2hle update failed||$1"
  exit 2
}

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

installed_version() { cat "$DIR/VERSION.txt" 2>/dev/null || echo "unknown"; }

log "Installed: $(installed_version)"
log "Asking GitHub for the $TAG release..."
JSON=$(curl -fsSL --max-time 20 -H 'Accept: application/vnd.github+json' "$API") \
  || fail "could not reach GitHub (is Wi-Fi on?)"
touch "$CHECKED"

read -r URL DIGEST UPDATED < <(jq -r --arg n "$ASSET" \
  '.assets[] | select(.name == $n) | "\(.browser_download_url) \(.digest // "") \(.updated_at)"' <<<"$JSON")
[ -n "${URL:-}" ] || fail "the $TAG release has no $ASSET"
DIGEST="${DIGEST#sha256:}"
[[ "$DIGEST" =~ ^[0-9a-f]{64}$ ]] || fail "GitHub gave no sha256 for $ASSET"

if [ "$MODE" != force ] && [ "$DIGEST" = "$(cat "$DIGEST_FILE" 2>/dev/null)" ]; then
  rm -f "$AVAILABLE"
  log "Up to date (canary built $UPDATED)."
  notify "m2hle is up to date||$(installed_version)" 4
  exit 1
fi

ZIP="$STAGE/$ASSET"
if [ "$(sha256sum "$ZIP" 2>/dev/null | cut -d' ' -f1)" != "$DIGEST" ]; then
  log "Downloading $ASSET (canary built $UPDATED)..."
  curl -fsSL --max-time 300 -o "$ZIP.part" "$URL" || { rm -f "$ZIP.part"; fail "download failed"; }
  mv -f "$ZIP.part" "$ZIP"
fi
[ "$(sha256sum "$ZIP" | cut -d' ' -f1)" = "$DIGEST" ] || { rm -f "$ZIP"; fail "the download's sha256 does not match GitHub's"; }

NEW="$STAGE/unpacked"
mkdir -p "$NEW"
unzip -o -q "$ZIP" -d "$NEW" || fail "could not unpack $ASSET"
for f in m2hle install-es.sh start_m2hle.sh VERSION.txt; do
  [ -f "$NEW/$f" ] || fail "$ASSET has no $f"
done
NEW_VERSION=$(cat "$NEW/VERSION.txt")

# A zip installed by hand (install-es.sh straight from an unpacked download)
# leaves no digest behind: same VERSION.txt means it is this one.
if [ "$MODE" != force ] && [ "$NEW_VERSION" = "$(installed_version)" ]; then
  echo "$DIGEST" > "$DIGEST_FILE"
  rm -f "$AVAILABLE"
  log "Up to date ($NEW_VERSION)."
  notify "m2hle is up to date||$NEW_VERSION" 4
  exit 1
fi

if [ "$MODE" = check ]; then
  echo "$NEW_VERSION" > "$AVAILABLE"
  exit 0
fi

# The binary has to start on this device before it replaces the one that does.
# A bad argument prints usage and exits 2; a missing library is 127, a wrong
# architecture 126, a crash 128+signal.
chmod +x "$NEW/m2hle"
"$NEW/m2hle" --update-selftest >/dev/null 2>&1
rc=$?
[ "$rc" -eq 2 ] || fail "the new m2hle does not start here (exit $rc); nothing was changed"

log "Installing $NEW_VERSION..."
ES=/storage/.emulationstation
es_before=$(cat "$ES/es_systems.cfg" "$ES/es_features.cfg" 2>/dev/null | md5sum)
bash "$NEW/install-es.sh" || fail "install-es.sh failed; the previous binary is kept as $DIR/m2hle.bak-*"
es_after=$(cat "$ES/es_systems.cfg" "$ES/es_features.cfg" 2>/dev/null | md5sum)

# install-es.sh records VERSION.txt too, but only since the updater exists.
install -m 644 "$NEW/VERSION.txt" "$DIR/VERSION.txt"
echo "$DIGEST" > "$DIGEST_FILE"
rm -f "$AVAILABLE"
log "Updated: $NEW_VERSION"

if [ "$es_before" != "$es_after" ]; then
  log "The ES configuration changed; restarting ES."
  notify "m2hle updated||$NEW_VERSION - restarting ES for its new options" 6
  if [ "$MODE" = tool ]; then
    set +u; . /etc/profile; set -u   # for UI_SERVICE
    systemctl restart ${UI_SERVICE:-essway.service}
  fi
else
  notify "m2hle updated||$NEW_VERSION" 6
fi
exit 0
