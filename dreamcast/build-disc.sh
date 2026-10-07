#!/bin/bash
# build-disc.sh -- the Dreamcast discs of m2-hle2, built offline (Pinboard #529).
#
#   dreamcast/build-disc.sh            ask for every option, Enter takes the default
#   dreamcast/build-disc.sh -y         take every default (and what DCB_* presets) unasked
#   dreamcast/build-disc.sh --last     the options of the last build, unasked
#   dreamcast/build-disc.sh --install  put "Build Dreamcast disc" on the desktop
#
# It does what an agent used to do by hand (dreamcast/README.md): check out
# the commit asked for into a clean worktree of its own, build the program
# with KallistiOS (make -C dreamcast), make the GDI and/or the CDI
# (mkdisc.sh), zip each for burning, boot each in Flycast for a screenshot and
# put them in the canary folder. Nothing it needs comes from the network but
# the optional `git fetch`.
#
# Every option is a DCB_<KEY> variable: preset one in the environment to
# change its default (DCB_FPS_CAP=0 dreamcast/build-disc.sh). The answers are
# saved in $WORK/last.conf, which --last reads back. When an option is added
# to the Makefile or mkdisc.sh, add its question here.
#
# What it keeps, in $DCB_WORK (~/build/dc-build): src/ (the worktree), out/dc
# (the objects, so a rebuild is incremental), assets/ (STF.AFS and
# TEXTURES.PAK, made once from the PS3 files; ~120 MB) and host/ (the
# det_digest that records TEXTURES.PAK). The discs go to the canary folder and
# are deleted here once they are there.
set -eu

DCB_WORK=${DCB_WORK:-$HOME/build/dc-build}
SELF=$(readlink -f "$0")
HERE=$(dirname "$SELF")
ASSUME=0
CANARY=$HOME/source/repos/ai/canary/canary.py
KOS_ENV=${KOS_ENV:-$HOME/build/tools/dc/kos/environ.sh}
FLYCAST_CORE=${FLYCAST_CORE:-$HOME/build/tools/dc/flycast/build-lr/flycast_libretro.so}

say()  { printf '\033[1;36m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m!!\033[0m %s\n' "$*" >&2; }
# wipe DIR: one of our own scratch folders, gone.
wipe() { [ ! -e "$1" ] || find "$1" -delete; }
die()  { printf '\033[1;31mxx\033[0m %s\n' "$*" >&2; exit 1; }

# ---- Questions ---------------------------------------------------------------
# ask KEY "question" default: sets DCB_KEY (a preset DCB_KEY is the default).
ask() {
    local key=$1 q=$2 def=$3 var="DCB_$1" ans
    def=${!var-$def}
    if [ "$ASSUME" = 1 ]; then printf -v "$var" '%s' "$def"; return; fi
    read -r -e -p "$q [$def]: " ans || true
    printf -v "$var" '%s' "${ans:-$def}"
}

# yes KEY "question" y|n: sets DCB_KEY to 1 or 0.
yes() {
    local key=$1 q=$2 def=$3 var="DCB_$1" ans
    case "${!var-}" in 1) def=y ;; 0) def=n ;; esac
    while :; do
        if [ "$ASSUME" = 1 ]; then ans=$def; else
            read -r -p "$q [$( [ "$def" = y ] && echo Y/n || echo y/N )]: " ans || true
            ans=${ans:-$def}
        fi
        case "$ans" in
            [yY]*) printf -v "$var" 1; return ;;
            [nN]*) printf -v "$var" 0; return ;;
        esac
        echo "  y or n"
    done
}

# pick KEY "question" default choice...: one of the choices.
pick() {
    local key=$1 q=$2 def=$3 var="DCB_$1"
    shift 3
    while :; do
        ask "$key" "$q ($(echo "$@" | tr ' ' /))" "$def"
        for c; do [ "${!var}" = "$c" ] && return; done
        [ "$ASSUME" = 1 ] && die "DCB_$key=${!var}: not one of $*"
        echo "  one of: $*"
    done
}

# ---- The source -------------------------------------------------------------
ask_source() {
    echo
    say "Source"
    ask REPO "The m2-hle2 repository" "$HOME/source/repos/ai/m2-hle2"
    ask REF "Branch or commit to build (\"here\": this checkout as it is)" origin/idea-340-dreamcast
    yes FETCH "git fetch first (needs the network)" y
}

# Checks out DCB_REF, detached, in $DCB_WORK/src, and sets SRC.
checkout_source() {
    if [ "$DCB_REF" = here ]; then
        SRC=$(readlink -f "$HERE/..")
        [ -z "$(git -C "$SRC" status --porcelain --untracked-files=no)" ] ||
            warn "$SRC has changes not committed: the build shows -dirty and its build number is HEAD's"
        return
    fi
    [ "$DCB_FETCH" = 1 ] && { git -C "$DCB_REPO" fetch -q origin || warn "git fetch failed: building what is here"; }
    local rev
    rev=$(git -C "$DCB_REPO" rev-parse --verify -q "$DCB_REF^{commit}") || die "no commit $DCB_REF in $DCB_REPO"
    SRC=$DCB_WORK/src
    if [ ! -e "$SRC/.git" ]; then
        mkdir -p "$DCB_WORK"
        git -C "$DCB_REPO" worktree add -q --detach "$SRC" "$rev"
    else
        git -C "$SRC" checkout -q --force --detach "$rev"
    fi
    say "Building $DCB_REF = $(git -C "$SRC" log -1 --format='%h %s' "$rev")"
}

# The checked-out tree's own copy of this script runs the rest, so a commit
# that changes the questions asks its own.
handover() {
    local theirs=$SRC/dreamcast/build-disc.sh
    [ "${DCB_HANDED:-0}" = 1 ] && return
    [ -f "$theirs" ] || { warn "$DCB_REF has no build-disc.sh: building it with this one"; return; }
    cmp -s "$theirs" "$SELF" && return
    [ "$SELF" = "$DCB_WORK/build-disc.sh" ] && cp "$theirs" "$DCB_WORK/build-disc.sh"
    export DCB_HANDED=1 DCB_SRC=$SRC
    export_answers
    exec bash "$theirs" $( [ "$ASSUME" = 1 ] && echo -y )
}

# ---- The options -------------------------------------------------------------
ask_release() {
    echo
    say "Release"
    ask RELEASE "Release name, on the PROF panel's VR line (empty: none)" "Alpha 0.3"
    pick DISCS "Discs" both gdi cdi both
    if [ "$DCB_DISCS" != cdi ]; then
        ask GDI_NAME "GDI disc's name (zip StF-<name>-<version>.zip)" Tracker
        ask GDI_CANARY "  its canary entry" "dreamcast/stf-$(echo "$DCB_GDI_NAME" | tr 'A-Z ' 'a-z-')"
    fi
    if [ "$DCB_DISCS" != gdi ]; then
        ask CDI_NAME "CDI disc's name" Lolo
        ask CDI_CANARY "  its canary entry" "dreamcast/stf-$(echo "$DCB_CDI_NAME" | tr 'A-Z ' 'a-z-')"
    fi
}

ask_program() {
    echo
    say "The program (make -C dreamcast)"
    yes AOT "AOT: the i960 code compiled to SH-4 ahead of time" y
    [ "$DCB_AOT" = 1 ] && ask AOT_COVER "  share of the map compiled (AOT_COVER)" 0.99
    yes GEMS "Gems C: Sega's C from Sonic Gems Collection for the trapped functions and the COP" y
    [ "$DCB_GEMS" = 1 ] && ask GEMS_DIR "  its folder" "$HOME/source/repos/ai/Sonic Gems Collection/m2hle"
    yes FRAME512 "FRAME512: a 512x384 frame in the middle of the 640x480 signal" y
    yes FILL "FILL: stretch the board over the whole frame (shape not kept)" n
    ask VIEW "VIEW x,y,w,h: show only that part of the board (empty: all of it)" ""
    yes FPS "FPS counter, top right" y
    pick PANEL "Stats panel: prof = profile panel, hidden until R shows it; full = stats always up" prof prof full none
    ask FPS_CAP "FPS_CAP: at most this many board frames a second (0: no cap)" 60
    yes HOST_MATH "HOST_MATH: the COP's sin, cos and square roots from the SH-4's own instructions" y
    yes STRIPS "STRIPS: meshes pre-walked into strips (STRIPS.PAK on the disc)" y
}

ask_disc() {
    echo
    say "The disc (mkdisc.sh)"
    ask PS3 "The PS3 release's folder (stf_rom/, sound/)" "$HOME/source/repos/ai/StF - PS3"
    yes SOUND "Sound: STF.AFS from the PS3's sound bank" y
    yes MODELS "MODELS.PAK: meshes laid out scene by scene" y
    yes TEXPAK "TEXTURES.PAK: textures already cut for the PVR" y
    yes REDO_ASSETS "  make STF.AFS / TEXTURES.PAK again (else the cached ones in $DCB_WORK/assets)" n
    if [ "$DCB_DISCS" != gdi ]; then
        yes DUMMY "CDI: the 80-minute dummy, so the game sits on the disc's outer edge (~810 MB image)" y
        yes FAST "CDI: write it fast, without EDC/ECC (emulators only, not for burning)" n
    fi
    yes ZIP "A zip of each disc, for burning" y
}

ask_after() {
    echo
    say "Afterwards"
    yes TEST "Boot each disc in Flycast (no window) and take a screenshot" y
    [ "$DCB_TEST" = 1 ] && ask TEST_SECS "  seconds to run it" 45
    yes CANARY "Put them in the canary folder (replaces the entry's last build)" y
    ask JOBS "make -j" 2
}

# Options only the tests and the benches use.
ask_dev() {
    echo
    yes DEV "Developer options (JIT, LINK, OPTAB, HASH_FRAME, AOT map...)" n
    [ "$DCB_DEV" = 1 ] || return 0
    yes OPTAB "OPTAB: dispatch through a handler table" n
    yes JIT "JIT: the SH-4 block JIT" n
    [ "$DCB_JIT" = 1 ] && { yes JIT_ON "  JIT_ON: on at boot" y; yes JIT_TEST "  JIT_TEST: its self-test" n; }
    ask HASH_FRAME "HASH_FRAME: show the board's hash at this frame (0: none)" 0
    yes LINK "LINK: lockstep against MAME over the serial port" n
    [ "$DCB_LINK" = 1 ] && { yes LINK_GEMS "  LINK_GEMS: Gems kept on in it" n; ask SINCOS "  SINCOS.BIN for the disc (tools/mksincos.py)" ""; }
    [ "$DCB_AOT" = 1 ] && ask AOT_MAP "AOT_MAP" sfight.aotmap
    ask IB_POOL "IB_POOL: the block runner's pool (empty: the Makefile's)" ""
    ask EXTRA "EXTRA: more compiler flags" ""
}

ALL_KEYS="REPO REF FETCH RELEASE DISCS GDI_NAME GDI_CANARY CDI_NAME CDI_CANARY AOT AOT_COVER GEMS GEMS_DIR
FRAME512 FILL VIEW FPS PANEL FPS_CAP HOST_MATH STRIPS PS3 SOUND MODELS TEXPAK REDO_ASSETS DUMMY FAST ZIP
TEST TEST_SECS CANARY JOBS DEV OPTAB JIT JIT_ON JIT_TEST HASH_FRAME LINK LINK_GEMS SINCOS AOT_MAP IB_POOL EXTRA"

export_answers() { local k; for k in $ALL_KEYS; do v="DCB_$k"; [ -n "${!v+x}" ] && export "$v"; done; return 0; }

save_answers() {
    local k v
    for k in $ALL_KEYS; do v="DCB_$k"; [ -n "${!v+x}" ] && printf 'DCB_%s=%q\n' "$k" "${!v}"; done > "$DCB_WORK/last.conf"
    return 0
}

# ---- Make's command line --------------------------------------------------------
HUD_ARG() {
    case "$DCB_PANEL" in
        prof) echo prof ;;
        full) echo "" ;;
        none) [ "$DCB_FPS" = 1 ] && echo min || echo none ;;
    esac
}

make_args() {
    MAKE_ARGS=(OUT="$OUT" VENDOR="$VENDOR" -j"$DCB_JOBS"
        FRAME512="$DCB_FRAME512" FILL="$DCB_FILL" VIEW="$DCB_VIEW" FPS_CAP="$DCB_FPS_CAP"
        HUD="$(HUD_ARG)" FPS="$DCB_FPS" HOST_MATH="$DCB_HOST_MATH" STRIPS="$DCB_STRIPS"
        RELEASE="$DCB_RELEASE")
    if [ "$DCB_AOT" = 1 ]; then MAKE_ARGS+=(AOT="$ROMS/rom_code1.bin" AOT_COVER="$DCB_AOT_COVER"); else MAKE_ARGS+=(AOT=); fi
    if [ "$DCB_GEMS" = 1 ]; then MAKE_ARGS+=(GEMS="$DCB_GEMS_DIR"); else MAKE_ARGS+=(GEMS=); fi
    [ "${DCB_DEV:-0}" = 1 ] || return 0
    MAKE_ARGS+=(OPTAB="$DCB_OPTAB" JIT="$DCB_JIT" HASH_FRAME="$DCB_HASH_FRAME" LINK="$DCB_LINK" EXTRA="$DCB_EXTRA")
    [ "$DCB_JIT" = 1 ] && MAKE_ARGS+=(JIT_ON="$DCB_JIT_ON" JIT_TEST="$DCB_JIT_TEST")
    [ "$DCB_LINK" = 1 ] && MAKE_ARGS+=(LINK_GEMS="$DCB_LINK_GEMS")
    [ -n "${DCB_AOT_MAP:-}" ] && MAKE_ARGS+=(AOT_MAP="$DCB_AOT_MAP")
    [ -n "$DCB_IB_POOL" ] && MAKE_ARGS+=(IB_POOL="$DCB_IB_POOL")
    return 0
}

# ---- The steps -------------------------------------------------------------------
check_tools() {
    [ -f "$KOS_ENV" ] || die "no KallistiOS at $KOS_ENV (KOS_ENV=)"
    [ -f "$ROMS/rom_code1.bin" ] || die "no $ROMS/rom_code1.bin (the PS3 release's stf_rom)"
    [ -f "$VENDOR/sokol/sokol_gfx.h" ] || die "no $VENDOR/sokol: VENDOR= a checkout's vendor/ with its submodules"
    if [ "$DCB_GEMS" = 1 ] && [ ! -f "$DCB_GEMS_DIR/gems_impl.h" ]; then die "no $DCB_GEMS_DIR/gems_impl.h (Gems C)"; fi
    if [ "$DCB_DISCS" != gdi ] && [ ! -f "$HOME/build/tools/dc/lazyboot/tools/boots3" ]; then
        say "Fetching Lazyboot's files for the CDI (once)"; sh "$SRC/dreamcast/tools/get-lazyboot.sh"
    fi
    if [ "$DCB_DISCS" != gdi ] && [ "${DCB_FAST:-0}" = 0 ] && ! command -v cdi4dc >/dev/null &&
       [ ! -x "$HOME/build/tools/dc/img4dc/cdi4dc/cdi4dc" ]; then
        say "Building cdi4dc (once)"; sh "$SRC/dreamcast/tools/build-cdi4dc.sh"
    fi
    return 0
}

# A det_digest for the host, from this tree, to record TEXTURES.PAK.
host_det_digest() {
    local h=$DCB_WORK/host
    mkdir -p "$h"
    printf '#define MINIZ_EXPORT\n#define MINIZ_NO_EXPORT\n' > "$h/miniz_export.h"
    say "Compiling det_digest for the host (~15 s)"
    (cd "$SRC" && cc -O2 -std=gnu11 -fno-strict-aliasing -ffp-contract=off -w -DM2HLE_DEV_TOOLS=0 \
        -DM2HLE_VERSION='"dc-build"' -I"$h" -Isrc -Isrc/board -Isrc/core -Isrc/net -Isrc/ui -Isrc/profiles \
        -I"$VENDOR/stb" -I"$VENDOR/miniz" -I"$VENDOR/sokol" -I"$VENDOR/sokol/util" \
        tests/det_digest.c "$VENDOR"/miniz/miniz*.c -o "$h/det_digest" -lm -lpthread -ldl)
}

# README.md's fight script: start, pick, confirm, then from frame 900 a new
# input every 30 frames, the next of r1 l2 d3 u1 r2 13 l 4 r, nothing between.
fight_script() {
    local s="520:s,530:,640:1,650:,700:1,710:" f k items=(r1 l2 d3 u1 r2 13 l 4 r)
    for ((f = 900; f < 9000; f += 30)); do
        k=$(( (f / 30) % 18 ))
        if ((k % 2 == 0)); then s+=",$f:${items[k / 2]}"; else s+=",$f:"; fi
    done
    echo "$s"
}

make_texpak() {
    local a=$DCB_WORK/assets d=$DCB_WORK/host/det_digest
    host_det_digest
    say "Recording TEXTURES.PAK: attract, then a fight (~2 minutes)"
    rm -f "$a/TEXTURES.PAK.new"
    (cd "$DCB_WORK/host" && "$d" "$ROMS" --profile sfight_console --frames 6000 \
            --tex-pack "0:6000:$a/TEXTURES.PAK.new" --out /dev/null &&
        "$d" "$ROMS" --profile sfight_console --frames 9000 --script "$(fight_script)" \
            --tex-pack "0:9000:$a/TEXTURES.PAK.new:+100000" --out /dev/null) > "$a/texpak.log" 2>&1 ||
        { tail "$a/texpak.log"; die "TEXTURES.PAK: det_digest failed"; }
    mv "$a/TEXTURES.PAK.new" "$a/TEXTURES.PAK"
    grep 'tex-pack:' "$a/texpak.log" | tail -1
}

make_sound() {
    local a=$DCB_WORK/assets
    [ -d "$DCB_PS3/sound" ] || die "no $DCB_PS3/sound for STF.AFS"
    say "Making STF.AFS from the PS3's sound bank (~2 minutes)"
    python3 "$SRC/dreamcast/tools/mksound.py" "$DCB_PS3/sound" "$a/STF.AFS.new" > "$a/sound.log" 2>&1 ||
        { tail "$a/sound.log"; die "STF.AFS: mksound.py failed"; }
    mv "$a/STF.AFS.new" "$a/STF.AFS"
}

assets() {
    mkdir -p "$DCB_WORK/assets"
    if [ "$DCB_SOUND" = 1 ] && { [ "$DCB_REDO_ASSETS" = 1 ] || [ ! -f "$DCB_WORK/assets/STF.AFS" ]; }; then make_sound; fi
    if [ "$DCB_TEXPAK" = 1 ] && { [ "$DCB_REDO_ASSETS" = 1 ] || [ ! -f "$DCB_WORK/assets/TEXTURES.PAK" ]; }; then make_texpak; fi
    return 0
}

build_program() {
    make_args
    say "make -C dreamcast ${MAKE_ARGS[*]}"
    # shellcheck disable=SC1090
    if ! (set +u; . "$KOS_ENV" && make -C "$SRC/dreamcast" "${MAKE_ARGS[@]}") > "$OUT/build.log" 2>&1; then
        grep -E 'error|Error' "$OUT/build.log" | head -20; die "the build failed: $OUT/build.log"
    fi
    tail -2 "$OUT/build.log"
}

# mkdisc KIND DIR: the disc into DIR; prints the image's path.
mkdisc() {
    local kind=$1 dir=$2 afs=""
    wipe "$dir"; mkdir -p "$dir"
    [ "$DCB_SOUND" = 1 ] && afs=$DCB_WORK/assets/STF.AFS
    # shellcheck disable=SC1090
    (set +u; . "$KOS_ENV"
     export TEXPAK=""; [ "$DCB_TEXPAK" = 1 ] && TEXPAK=$DCB_WORK/assets/TEXTURES.PAK
     [ "$DCB_MODELS" = 1 ] || export NOPAK=1
     [ "$DCB_STRIPS" = 1 ] || export NOSTRIPS=1
     [ -n "${DCB_SINCOS:-}" ] && export SINCOS=$DCB_SINCOS
     if [ "$kind" = cdi ]; then
         export CDI=1; [ "$DCB_DUMMY" = 1 ] || export DUMMY=0; [ "$DCB_FAST" = 1 ] && export FAST=1
     fi
     [ -n "$TEXPAK" ] || unset TEXPAK
     sh "$SRC/dreamcast/mkdisc.sh" "$OUT" "$ROMS" "$dir" $afs) > "$dir.log" 2>&1 ||
        { tail "$dir.log" >&2; die "mkdisc.sh ($kind) failed: $dir.log"; }
    tail -1 "$dir.log"
}

version() { echo "$DCB_RELEASE" | grep -oE '[0-9][0-9.]*[a-z]?$' || echo "r$BUILD"; }

zip_disc() {
    local dir=$1 name=$2 z
    z="StF-$(echo "$name" | tr ' ' -)-$(version).zip"
    say "Zipping $z"
    (cd "$dir" && rm -f "$z" && zip -q -1 "$z" $(ls | grep -v '\.zip$'))
}

# test_disc IMAGE: Flycast's libretro core on a private Xvfb, a screenshot at the end.
test_disc() {
    local img=$1 r port pid
    r=$(dirname "$img")/../run-$(basename "$(dirname "$img")")
    wipe "$r"; mkdir -p "$r/sys" "$r/snap"
    port=$(python3 -c 'import socket;s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);s.bind(("127.0.0.1",0));print(s.getsockname()[1])')
    printf '%s\n' "system_directory = \"$r/sys\"" "savefile_directory = \"$r\"" "savestate_directory = \"$r\"" \
        "screenshot_directory = \"$r/snap\"" "core_options_path = \"$r/opts.cfg\"" 'config_save_on_exit = "false"' \
        'video_driver = "gl"' 'audio_driver = "null"' 'menu_driver = "rgui"' 'pause_nonactive = "false"' \
        'network_cmd_enable = "true"' "network_cmd_port = \"$port\"" > "$r/retroarch.cfg"
    echo 'reicast_hle_bios = "enabled"' > "$r/opts.cfg"
    say "Booting $(basename "$img") in Flycast for $DCB_TEST_SECS s"
    (cd "$r" && exec xvfb-run -a -s "-screen 0 1024x768x24" retroarch --config "$r/retroarch.cfg" \
        -L "$FLYCAST_CORE" "$img" > "$r/ra.log" 2>&1) &
    pid=$!
    sleep "$DCB_TEST_SECS"
    echo -n SCREENSHOT | python3 -c "import socket,sys;socket.socket(socket.AF_INET,socket.SOCK_DGRAM).sendto(sys.stdin.buffer.read(),('127.0.0.1',$port))"
    sleep 3
    pkill -f "retroarch --config $r/" || true; wait "$pid" 2>/dev/null || true
    SHOT=$(ls "$r"/snap/*.png 2>/dev/null | head -1)
    [ -n "$SHOT" ] || { warn "no screenshot from Flycast ($r/ra.log)"; return 0; }
    cp "$SHOT" "$OUT/$(basename "$(dirname "$img")").png"; SHOT="$OUT/$(basename "$(dirname "$img")").png"
    wipe "$r"
    say "Screenshot: $SHOT"
}

describe() {
    local kind=$1 d="STF $kind"
    [ -n "$DCB_RELEASE" ] && d+=" $DCB_RELEASE"
    d+=" at $GIT_DESC $BUILD: HUD=$(HUD_ARG) FPS=$DCB_FPS FRAME512=$DCB_FRAME512 FILL=$DCB_FILL"
    [ -n "$DCB_VIEW" ] && d+=" VIEW=$DCB_VIEW"
    d+=" FPS_CAP=$DCB_FPS_CAP"
    [ "$DCB_AOT" = 1 ] && d+=", AOT $DCB_AOT_COVER"; [ "$DCB_GEMS" = 1 ] && d+=", Gems C"
    [ "$DCB_PANEL" = prof ] && d+=", PROF panel hidden at boot - R shows/hides it"
    [ "$DCB_STRIPS" = 1 ] && d+=", STRIPS.PAK"; [ "$DCB_TEXPAK" = 1 ] && d+=", TEXTURES.PAK"
    [ "$DCB_SOUND" = 1 ] && d+=", STF.AFS sound"
    d+=". Made by dreamcast/build-disc.sh"
    [ "$DCB_TEST" = 1 ] && d+=", booted in Flycast"
    echo "$d"
}

# one_disc gdi|cdi NAME CANARY_ENTRY
one_disc() {
    local kind=$1 name=$2 entry=$3 dir img content
    dir=$OUT/$kind-$(echo "$name" | tr 'A-Z ' 'a-z-')
    img=$(mkdisc "$kind" "$dir")
    say "Disc: $img ($(du -sh "$dir" | cut -f1))"
    [ "$DCB_ZIP" = 1 ] && zip_disc "$dir" "$name"
    SHOT=""
    [ "$DCB_TEST" = 1 ] && test_disc "$img"
    if [ "$DCB_CANARY" = 1 ]; then
        content=$(basename "$img")
        python3 "$CANARY" add "$entry" --from "$dir"/* --emu flycast --content "$content" --src "$SRC" \
            --title "STF DC $(echo "$kind" | tr a-z A-Z) ($name)${DCB_RELEASE:+ $DCB_RELEASE}" \
            --about "$(describe "$(echo "$kind" | tr a-z A-Z)")" | tail -2
        wipe "$dir"
    fi
    local where=$dir; [ "$DCB_CANARY" = 1 ] && where="canary $entry"
    RESULTS+=("$kind: $where${SHOT:+ (screenshot $SHOT)}")
}

install_launcher() {
    mkdir -p "$DCB_WORK" "$HOME/Desktop"
    cp "$SELF" "$DCB_WORK/build-disc.sh"
    cat > "$HOME/Desktop/build-dreamcast-disc.desktop" <<EOF
[Desktop Entry]
Type=Application
Version=1.0
Name=Build Dreamcast disc
Comment=m2-hle2's Dreamcast GDI/CDI, built offline: asks for every option (dreamcast/build-disc.sh)
Exec=xfce4-terminal --hold --title "Build Dreamcast disc" -x bash "$DCB_WORK/build-disc.sh"
Icon=media-optical
Terminal=false
Categories=Development;
EOF
    chmod +x "$HOME/Desktop/build-dreamcast-disc.desktop"
    say "Desktop launcher: ~/Desktop/build-dreamcast-disc.desktop (runs $DCB_WORK/build-disc.sh)"
}

main() {
    case "${1:-}" in
        --install) install_launcher; exit 0 ;;
        -y) ASSUME=1 ;;
        --last) [ -f "$DCB_WORK/last.conf" ] || die "no $DCB_WORK/last.conf yet"
                [ "${DCB_HANDED:-0}" = 1 ] || . "$DCB_WORK/last.conf"; ASSUME=1 ;;
        -h|--help) sed -n '2,25p' "$SELF"; exit 0 ;;
        "") ;;
        *) die "unknown option $1 (--help)" ;;
    esac
    say "m2-hle2 Dreamcast disc builder -- Enter takes the [default]"
    if [ "${DCB_HANDED:-0}" = 1 ]; then SRC=$DCB_SRC; else ask_source; checkout_source; handover; fi
    ask_release; ask_program; ask_disc; ask_after; ask_dev
    mkdir -p "$DCB_WORK"; save_answers
    OUT=$DCB_WORK/out; mkdir -p "$OUT/dc"
    VENDOR=$DCB_REPO/vendor
    # AOT= takes no path with spaces: the ROM folder through a link.
    ln -sfn "$DCB_PS3/stf_rom" "$DCB_WORK/stf_rom"; ROMS=$DCB_WORK/stf_rom
    GIT_DESC=$(git -C "$SRC" describe --always --abbrev=7 --dirty)
    BUILD=r$(git -C "$SRC" rev-list --count HEAD)
    check_tools; assets
    OUT=$DCB_WORK/out/dc; build_program
    RESULTS=()
    [ "$DCB_DISCS" != cdi ] && one_disc gdi "$DCB_GDI_NAME" "$DCB_GDI_CANARY"
    [ "$DCB_DISCS" != gdi ] && one_disc cdi "$DCB_CDI_NAME" "$DCB_CDI_CANARY"
    echo; say "Done: $GIT_DESC $BUILD${DCB_RELEASE:+, $DCB_RELEASE}"
    printf '  %s\n' "${RESULTS[@]}"
    echo "  The same build again: $SELF --last"
}

main "$@"
