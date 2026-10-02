#!/bin/sh
# run.sh — the QT960 <-> Model 2B link (Pinboard #312), both boards in MAME, side by side.
#
#   tools/qt960link/run.sh            # windows on $DISPLAY (:1, the container's VNC)
#   HEADLESS=300 tools/qt960link/run.sh   # no windows; stop after 300 checks
#   HOST=1 tools/qt960link/run.sh     # load and relay with qtlink_host.py, as on the real board
#
# Left: a Model 2B running m2-kernel, its serial port on TCP through m2-kernel's own
# bridge (m2k_serial.lua). Right: a QT960 (i960KB) at NINDY's prompt; qt960_link.lua types
# qtlink into it with "mo", starts it with "go", and carries its frames to the bridge.
# Close either window (or Ctrl+C) to stop both. The script presses a key in each window to
# get past MAME's "this system doesn't work" warning (xdotool; without it, press one yourself).
#
# MAME  the shared MAME (fork branch "shared"; needs its qt960 driver with BURST
#       regions and the i960 core's modtc, modify and extract)
# M2K   an m2-kernel checkout (roms/m2kernel: the kernel's EPROMs and m2k_serial.lua)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
MAME=${MAME:-$HOME/build/mame-bin/mame-shared/shared}
M2K=${M2K:-$(cd "$HERE/../../.." && pwd)/m2-kernel}
[ -d "$M2K" ] || M2K=$HOME/source/repos/ai/m2-kernel
ROMS=${ROMS_DIR:-$HOME/build/mameroms}
PORT=${M2K_PORT:-7960}
WORK=${WORK:-${TMPDIR:-/tmp}/qt960link}
export DISPLAY=${DISPLAY:-:1}

[ -x "$MAME" ] || { echo "run.sh: no MAME at $MAME (set MAME)"; exit 1; }
[ -f "$M2K/roms/m2kernel/m2k_serial.lua" ] || { echo "run.sh: no m2-kernel at $M2K (set M2K)"; exit 1; }
mkdir -p "$WORK/k" "$WORK/q"
OUT="$WORK" sh "$HERE/qtlink/build.sh" > /dev/null

if [ -n "$HEADLESS" ]; then
    VIDEO="-video none -sound none -seconds_to_run 100000"
    export QTLINK_EXIT=$HEADLESS QTLINK_ECHO=1
else
    VIDEO="-window -nomaximize -sound none"
fi

# m2-kernel's bridge listens; the QT960's side connects (and retries until it can)
cd "$WORK/k"
M2K_PORT=$PORT "$MAME" m2kernel \
    -rompath "$M2K/roms;$ROMS" -skip_gameinfo $VIDEO \
    -cfg_directory . -nvram_directory . -snapshot_directory . \
    -autoboot_script "$M2K/roms/m2kernel/m2k_serial.lua" > "$WORK/m2kernel.log" 2>&1 &
KPID=$!
trap 'kill $KPID 2>/dev/null; sleep 1 2>/dev/null; kill -9 $KPID 2>/dev/null; true' EXIT INT TERM

# Both drivers are MACHINE_NOT_WORKING, so each MAME opens on a warning that waits for a
# key (-skip_gameinfo does not skip it), and nothing runs until it goes. Put the windows side
# by side (the window manager places them itself) and press a key in each.
if [ -z "$HEADLESS" ]; then
    if command -v xdotool > /dev/null; then
        (
            # A window appears before its warning does, and a busy machine takes seconds to get
            # there, so press more than once. MAME reads keys once a frame and misses a bare tap,
            # so hold it. Shift: a space would reach NINDY through the QT960's terminal.
            k=$(timeout 60 xdotool search --sync --name '\[m2kernel\]' | head -1)
            q=$(timeout 60 xdotool search --sync --name '\[qt960\]' | head -1)
            for try in 1 2 3 4 5 6; do
                sleep 2
                for w in "$k 200" "$q 760"; do
                    set -- $w
                    [ $# -eq 2 ] || continue
                    xdotool windowmove "$1" "$2" 100
                    xdotool windowactivate --sync "$1"
                    sleep 0.3
                    xdotool keydown shift
                    sleep 0.2
                    xdotool keyup shift
                done
            done
        ) > /dev/null 2>&1 &
    else
        echo "run.sh: no xdotool: press a key in each MAME window to get past its warning"
    fi
fi

cd "$WORK/q"
if [ -n "$HOST" ]; then
    # The real board's path: the QT960's serial port on TCP (qt960_wire.lua), and
    # qtlink_host.py on it, as it would be on a COM port.
    QTWIRE_PORT=$((PORT + 1)) "$MAME" qt960 -rompath "$ROMS" -skip_gameinfo $VIDEO \
        -cfg_directory . -nvram_directory . -snapshot_directory . \
        -autoboot_script "$HERE/qt960_wire.lua" > "$WORK/qt960.log" 2>&1 &
    QPID=$!
    trap 'kill $KPID $QPID 2>/dev/null; sleep 1 2>/dev/null; kill -9 $KPID $QPID 2>/dev/null; true' EXIT INT TERM
    python3 "$HERE/qtlink_host.py" --board tcp:127.0.0.1:$((PORT + 1)) \
        --m2k tcp:127.0.0.1:$PORT --bin "$WORK/qtlink.bin" ${HEADLESS:+--checks $HEADLESS --quiet}
    exit
fi
QTLINK_BIN="$WORK/qtlink.bin" QTLINK_M2K=127.0.0.1:$PORT \
    "$MAME" qt960 -rompath "$ROMS" -skip_gameinfo $VIDEO \
    -cfg_directory . -nvram_directory . -snapshot_directory . \
    -autoboot_script "$HERE/qt960_link.lua"
