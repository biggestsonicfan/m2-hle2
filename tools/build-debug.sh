#!/usr/bin/env bash
# A Linux m2hle to debug with: symbols, frame pointers, and by default ASan +
# UBSan. The fly's dashboard runs it in place of the release canary (stf-fly's
# emulator build switch), so its crashes and faults turn up with a stack.
#
#   tools/build-debug.sh [--flavor asan|symbols|debug] [--commit REV]
#                        [--repo DIR] [--build DIR] [--install DIR]
#
#   asan     RelWithDebInfo + address,undefined (default). About 3x slower
#            than release; ASan stops the process at the first bad access and
#            says where, UBSan logs each fault once and carries on.
#   symbols  RelWithDebInfo, no sanitizers: release speed, for perf and gdb.
#   debug    -O0 -g: slowest, every variable visible in gdb.
#
# --commit REV builds that commit instead of this checkout, in a worktree of
# its own under $M2HLE_DEBUG_HOME (default ~/.cache/m2hle-debug). Give it the
# canary's commit (VERSION.txt beside the release exe) and the debug build runs
# the same code as the release it stands in for. The flags go in through
# CMAKE_C_FLAGS as well as M2HLE_SANITIZE, so a commit from before that option
# builds the same way.
#
# --repo DIR is the m2-hle2 checkout to work from, when this script is not run
# out of one (the fly's dashboard takes it from master with git show).
#
# --install DIR copies the binary into DIR with a VERSION.txt naming the commit
# and flavour, which is how the fly's kit gets bin/linux-debug/m2hle.
#
# None of the flavours changes the board's arithmetic (-ffp-contract=off and
# -fno-strict-aliasing hold at every optimisation level), so a debug build
# has to compute the frames a release build of the same commit does;
# tools/ab-builds.mjs is the check. A build of another commit than the
# canary's is another matter: netplay against people on the canary can desync.
set -euo pipefail

here=$(cd "$(dirname "$0")/.." && pwd)
flavor=asan
commit=""
build=""
install=""
while [ $# -gt 0 ]; do
    case "$1" in
        --flavor)  flavor=$2; shift 2 ;;
        --commit)  commit=$2; shift 2 ;;
        --repo)    here=$(cd "$2" && pwd); shift 2 ;;
        --build)   build=$2; shift 2 ;;
        --install) install=$2; shift 2 ;;
        -h|--help) sed -n '2,32p' "$0"; exit 0 ;;
        *) echo "build-debug: unknown argument $1" >&2; exit 2 ;;
    esac
done
case "$flavor" in
    asan)    type=RelWithDebInfo; san="address,undefined" ;;
    symbols) type=RelWithDebInfo; san="" ;;
    debug)   type=Debug;          san="" ;;
    *) echo "build-debug: --flavor is asan, symbols or debug, not $flavor" >&2; exit 2 ;;
esac

src=$here
home=${M2HLE_DEBUG_HOME:-$HOME/.cache/m2hle-debug}
if [ -n "$commit" ]; then
    # The main checkout's object store and submodule clones, so nothing is
    # fetched that is already on disk. noclip only feeds tools/ and is big.
    common=$(cd "$here" && cd "$(git rev-parse --git-common-dir)" && pwd)
    src=$home/src
    if [ ! -e "$src/.git" ]; then
        mkdir -p "$home"
        # -f: the cache is usually on a container's own disk, and a container
        # rebuilt from its image leaves the old registration behind.
        git -C "$here" worktree add -f --detach "$src" "$commit"
    else
        git -C "$src" fetch -q origin 2>/dev/null || true
        git -C "$src" checkout -q --detach "$commit"
    fi
    for m in imgui dear_bindings sokol miniz ImGuiFileDialog imgui_club stb; do
        ref=()
        [ -d "$common/modules/vendor/$m" ] && ref=(--reference "$common/modules/vendor/$m")
        git -C "$src" submodule update -q --init "${ref[@]}" "vendor/$m"
    done
fi
build=${build:-$home/build-$flavor}

# dear_bindings needs ply. The system Python refuses pip (PEP 668), so a venv
# in the build tree carries it when the interpreter on PATH has none.
py=$(command -v python3)
if ! "$py" -c "import ply" 2>/dev/null; then
    if [ ! -x "$build/.venv/bin/python" ]; then
        mkdir -p "$build"
        python3 -m venv "$build/.venv"
        "$build/.venv/bin/python" -m pip install -q ply==3.11
    fi
    py=$build/.venv/bin/python
fi

flags="-fno-omit-frame-pointer"
ldflags=""
if [ -n "$san" ]; then
    flags="$flags -fsanitize=$san"
    ldflags="-fsanitize=$san"
fi
sha=$(git -C "$src" rev-parse HEAD)
cmake -S "$src" -B "$build" -DCMAKE_BUILD_TYPE=$type -DM2HLE_SANITIZE="$san" \
      -DCMAKE_C_FLAGS="$flags" -DCMAKE_CXX_FLAGS="$flags" \
      -DCMAKE_EXE_LINKER_FLAGS="$ldflags" \
      -DM2HLE_BUILD_TESTS=OFF -DPython3_EXECUTABLE="$py" \
      -DM2HLE_VERSION="dev-${sha:0:7}" >/dev/null
# Niced: the machine that builds this is usually streaming and training too.
nice -n 10 cmake --build "$build" --target m2hle -j "$(nproc)"

exe=$(find "$build" -maxdepth 2 -type f -name m2hle -perm -u+x | head -1)
[ -n "$exe" ] || { echo "build-debug: no m2hle under $build" >&2; exit 1; }
echo "built $exe ($flavor, ${sha:0:7})"

if [ -n "$install" ]; then
    mkdir -p "$install"
    # Beside a running copy: write a new file and rename it over the old one,
    # so an emulator already running from it keeps its (unlinked) image.
    cp "$exe" "$install/m2hle.new"
    mv -f "$install/m2hle.new" "$install/m2hle"
    dirty=$(git -C "$src" status --porcelain --untracked-files=no | head -1)
    printf 'dev-%s%s (%s) %s\n' "${sha:0:7}" "${dirty:+-dirty}" "$sha" "$flavor" > "$install/VERSION.txt"
    echo "installed $install/m2hle"
fi
