#!/bin/bash

# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026-present ROCKNIX (https://github.com/ROCKNIX)

. /etc/profile
set_kill set "m2hle"

M2HLE="${M2HLE:-/storage/.local/share/m2hle/m2hle}"
CONFIG_DIR="/storage/.config/m2hle"
mkdir -p "${CONFIG_DIR}"

GAME="${1##*/}"
PLATFORM="${2##*/}"

CORES=$(get_setting "cores" "${PLATFORM}" "${GAME}")
if [ "${CORES}" = "little" ]; then
  EMUPERF="${SLOW_CORES}"
elif [ "${CORES}" = "big" ]; then
  EMUPERF="${FAST_CORES}"
else
  unset EMUPERF
fi

OPTIONS=()

# The game runs at 60 Hz either way; this only sets how often a frame is drawn.
RENDER_FPS=$(get_setting render_fps "${PLATFORM}" "${GAME}")
[[ "${RENDER_FPS}" =~ ^(30|45|60)$ ]] || RENDER_FPS=30
OPTIONS+=(--render-fps "${RENDER_FPS}")

RENDER_SCALE=$(get_setting render_scale "${PLATFORM}" "${GAME}")
[[ "${RENDER_SCALE}" =~ ^[1-2]$ ]] && OPTIONS+=(--render-scale "${RENDER_SCALE}")

# Screen size: 1 shows the board's 496x384 pixel for pixel, centred; anything else fills the screen.
SCREEN_SIZE=$(get_setting screen_size "${PLATFORM}" "${GAME}")
[ "${SCREEN_SIZE}" = "1" ] && OPTIONS+=(--display-scale 1)

# Buttons. ROCKNIX's rg_arc_joypad mapping gives A = SDL a (south), B = b (east),
# C = rightstick (r3), X = y (north), Y = x (west), Z = leftstick (l3).
# Sonic the Fighters: button 1 Punch, 2 Kick, 3 Barrier (MAME schamp inputs).
#   A = Punch, B = Kick, C = Block; Y, which m2hle's default gives button 3, is free.
# "button macros" on puts combos on the top row, each above the button it starts
# from, as the Gems Collection and HD ports allow: X = P+K, Y = K+B, Z = P+K+B.
MACROS=$(get_setting button_macros "${PLATFORM}" "${GAME}")
if [ "${MACROS}" = "on" ]; then
  OPTIONS+=(--pad-map "south=b1,east=b2,r3=b3,north=b1+b2,west=b2+b3,l3=b1+b2+b3")
else
  OPTIONS+=(--pad-map "south=b1,east=b2,r3=b3,west=none")
fi

# Audio: the sound board (68000 + SCSP) runs and plays unless turned off; off is
# silent and leaves the emu thread less to do (cooler). Its own key: sm2-emu's
# "sound board" setting (segamodel2.sound_board) stays sm2-emu's.
AUDIO=$(get_setting audio "${PLATFORM}" "${GAME}")
[ "${AUDIO}" = "off" ] || OPTIONS+=(--sound)

# Status line in the top-right corner (fps, temperature, time, battery): on unless turned off.
OSD=$(get_setting status_overlay "${PLATFORM}" "${GAME}")
[ "${OSD}" = "off" ] || OPTIONS+=(--osd)

# Online play: sign in to RPCN with the stored netplay settings and open the
# lobby (L1+R1 brings it back during a game). The settings are the PC's
# %APPDATA%\m2hle2\netplay.cfg copied to the path below; README.md says how.
ONLINE=$(get_setting online_play "${PLATFORM}" "${GAME}")
[ "${ONLINE}" = "on" ] && OPTIONS+=(--netplay --net-config /storage/.config/m2hle2/netplay.cfg)

# The RK3566's critical trip powers the unit off at ~95 C: quit first.
OPTIONS+=(--max-temp 90)

# Update check: in the background, at most every 6 hours, never holding up the
# game (m2hle-update.sh has the details). The result is shown after the game.
UPDATE_CHECK=$(get_setting update_check "${PLATFORM}" "${GAME}")
M2HLE_UPDATE=/storage/.local/bin/m2hle-update.sh
if [ "${UPDATE_CHECK}" != "off" ] && [ -x "${M2HLE_UPDATE}" ]; then
  "${M2HLE_UPDATE}" --check 21600 >/dev/null 2>&1 </dev/null &
fi

sway_fullscreen m2hle pidof &

cd "${CONFIG_DIR}"
echo "Command: ${M2HLE} ${OPTIONS[*]} ${1}" >>/var/log/exec.log 2>&1
${EMUPERF} "${M2HLE}" "${OPTIONS[@]}" "${1}" >>/var/log/exec.log 2>&1 ||:

if [ "${UPDATE_CHECK}" != "off" ] && [ -f /storage/.local/share/m2hle/UPDATE_AVAILABLE ]; then
  /usr/bin/sdl2notify --center "m2hle update available||$(cat /storage/.local/share/m2hle/UPDATE_AVAILABLE) - launch Update m2-hle" 255 255 255 4 >/dev/null 2>&1 ||:
fi
