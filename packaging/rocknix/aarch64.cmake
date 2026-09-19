# Cross-compile the sdl3 frontend for the RK3566 (Anbernic RG ARC-S, ROCKNIX).
# Used by .github/workflows/canary.yml and by the docker recipe in README.md.
#
# The sysroot is the build container's own arm64 multiarch tree
# (dpkg --add-architecture arm64 + libsdl3-dev:arm64 libgles-dev:arm64), so
# nothing here names a path: CMAKE_LIBRARY_ARCHITECTURE is what sends
# find_package at /usr/lib/aarch64-linux-gnu.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
set(CMAKE_LIBRARY_ARCHITECTURE aarch64-linux-gnu)
# Four Cortex-A55 cores. The board's arithmetic stays host-independent whatever
# this says -- CMakeLists.txt passes -ffp-contract=off, which is what keeps an
# AArch64 build's RAM equal to an x86 build's (see its comment).
set(CMAKE_C_FLAGS_INIT "-mcpu=cortex-a55")
# cmake, ninja and the generated dear_bindings step are the host's.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
