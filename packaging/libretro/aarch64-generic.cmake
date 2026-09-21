# Cross-compile the libretro core for any 64-bit ARM Linux RetroArch: ROCKNIX,
# Knulli, muOS, ArkOS and the rest. Used by .github/workflows/canary.yml
# (job libretro-linux-arm64).
#
# Unlike packaging/rocknix/aarch64.cmake this does not name a CPU: -mcpu=cortex-a55
# lets the compiler use ARMv8.2 instructions, which a Cortex-A53 or A35 handheld
# (RK3326, Allwinner H700, RK3566's older siblings) meets with SIGILL. Plain ARMv8-A runs on
# all of them; -mtune only orders the instructions for the common A55.
#
# The sysroot is the build container's own arm64 multiarch tree, as for the
# rocknix toolchain, so nothing here names a path.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
set(CMAKE_LIBRARY_ARCHITECTURE aarch64-linux-gnu)
# The board's arithmetic stays host-independent regardless: CMakeLists.txt adds
# -ffp-contract=off -fno-strict-aliasing for every GCC/Clang build.
set(CMAKE_C_FLAGS_INIT "-march=armv8-a -mtune=cortex-a55")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
