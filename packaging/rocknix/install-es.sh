#!/bin/bash
# Adds m2hle as a second emulator for segamodel2 in the device's local ES config.
# Run it from the unpacked release (it takes the binary and the launcher from
# its own directory); the old cross-build flow's /tmp copies still work.
# Idempotent: re-running only refreshes the binary and the launcher.
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

mkdir -p /storage/.local/share/m2hle
# Keep the binary that is being replaced: a canary is a development build, and
# stepping back to the one that worked should not need the network.
[ -f /storage/.local/share/m2hle/m2hle ] && cp -a /storage/.local/share/m2hle/m2hle "/storage/.local/share/m2hle/m2hle.bak-$STAMP"
install -m 755 "$BIN" /storage/.local/share/m2hle/m2hle
install -m 755 "$LAUNCHER" /storage/.local/bin/start_m2hle.sh
echo "installed $(basename "$BIN") -> /storage/.local/share/m2hle/m2hle"

if grep -q 'm2hle-sa' "$ES/es_systems.cfg"; then
  echo "es_systems.cfg: m2hle already listed"
else
  cp "$ES/es_systems.cfg" "$ES/es_systems.cfg.bak-$STAMP"
  # Insert after the </emulator> that closes sm2-emu inside the segamodel2 system.
  awk '
    /<name>segamodel2<\/name>/ { insys = 1 }
    insys && /<emulator name="sm2-emu">/ { inemu = 1 }
    { print }
    insys && inemu && /<\/emulator>/ {
      print "\t\t\t<emulator name=\"m2hle\">"
      print "\t\t\t\t<cores>"
      print "\t\t\t\t\t<core>m2hle-sa</core>"
      print "\t\t\t\t</cores>"
      print "\t\t\t</emulator>"
      inemu = 0; insys = 0; done = 1
    }
    END { if (!done) exit 1 }
  ' "$ES/es_systems.cfg.bak-$STAMP" > "$ES/es_systems.cfg.new"
  mv "$ES/es_systems.cfg.new" "$ES/es_systems.cfg"
  echo "es_systems.cfg: added m2hle (backup es_systems.cfg.bak-$STAMP)"
fi

if grep -q '<emulator name="m2hle"' "$ES/es_features.cfg"; then
  echo "es_features.cfg: m2hle already listed"
else
  cp "$ES/es_features.cfg" "$ES/es_features.cfg.bak-$STAMP"
  awk '
    { print }
    /<emulator name="sm2-emu">/ { inemu = 1 }
    inemu && /<\/emulator>/ {
      print "  <emulator name=\"m2hle\">"
      print "   <cores>"
      print "    <core name=\"m2hle-sa\">"
      print "      <features>"
      print "        <feature name=\"status overlay\">"
      print "          <choice name=\"on\" value=\"on\" />"
      print "          <choice name=\"off\" value=\"off\" />"
      print "        </feature>"
      print "        <feature name=\"render fps\">"
      print "          <choice name=\"30 (cooler)\" value=\"30\" />"
      print "          <choice name=\"45\" value=\"45\" />"
      print "          <choice name=\"60\" value=\"60\" />"
      print "        </feature>"
      print "        <feature name=\"render scale\">"
      print "          <choice name=\"1x (496x384)\" value=\"1\" />"
      print "          <choice name=\"2x (992x768)\" value=\"2\" />"
      print "        </feature>"
      print "        <feature name=\"screen size\">"
      print "          <choice name=\"fit screen\" value=\"fit\" />"
      print "          <choice name=\"1x (496x384)\" value=\"1\" />"
      print "        </feature>"
      print "      </features>"
      print "    </core>"
      print "   </cores>"
      print "  </emulator>"
      inemu = 0; done = 1
    }
    END { if (!done) exit 1 }
  ' "$ES/es_features.cfg.bak-$STAMP" > "$ES/es_features.cfg.new"
  mv "$ES/es_features.cfg.new" "$ES/es_features.cfg"
  echo "es_features.cfg: added m2hle (backup es_features.cfg.bak-$STAMP)"
fi

# Installs from before a feature existed: add it at the end of the m2hle core's
# feature list.  add_feature "<name>" "<label>=<value>" ...
add_feature() {
  local name="$1"; shift
  if awk -v n="<feature name=\"$name\">" '/<core name="m2hle-sa">/ { c = 1 } c && /<\/core>/ { exit } c && index($0, n) { found = 1; exit } END { exit !found }' "$ES/es_features.cfg"; then
    return
  fi
  local tag="${name// /_}"
  cp "$ES/es_features.cfg" "$ES/es_features.cfg.bak-$STAMP-$tag"
  local choices=""
  for c in "$@"; do choices+="          <choice name=\"${c%%=*}\" value=\"${c#*=}\" />"$'\n'; done
  awk -v feat="        <feature name=\"$name\">" -v choices="$choices" '
    /<core name="m2hle-sa">/ { c = 1 }
    c && /<\/features>/ { print feat; printf "%s", choices; print "        </feature>"; c = 0 }
    { print }
  ' "$ES/es_features.cfg.bak-$STAMP-$tag" > "$ES/es_features.cfg.new"
  mv "$ES/es_features.cfg.new" "$ES/es_features.cfg"
  echo "es_features.cfg: added the $name feature to m2hle (backup es_features.cfg.bak-$STAMP-$tag)"
}
add_feature "status overlay" "on=on" "off=off"
add_feature "screen size" "fit screen=fit" "1x (496x384)=1"
add_feature "audio" "on=on" "off (cooler)=off"
add_feature "button macros" "off=off" "on (X Y Z)=on"

grep -n -A12 '<name>segamodel2</name>' "$ES/es_systems.cfg" | grep -E 'emulator|core'
