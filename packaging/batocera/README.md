# m2-hle on batocera.linux

The m2-hle libretro core (`m2hle_libretro.so`) as a batocera.linux buildroot
package, `libretro-m2hle`, for the Sega Model 2 system (`model2`) batocera
already has. It sits beside sm2-emu, which stays the system's default: on a
board with both, the core is picked from the system's emulator menu. It is a
libretro core rather than a standalone emulator, so RetroArch handles the pads,
hotkeys, savestates and rewind, and batocera-launch needs no generator of its own.

Batocera builds every package from source, so nothing here ships a binary: the
package downloads one m2-hle2 commit's GitHub tarball and the submodules the
core reads (sokol, miniz, stb, Lua, GGPO). The desktop's ImGui, the noclip
viewer and the Gems/Ages C are not downloaded.

## Files

| File | Becomes, in batocera.linux |
|---|---|
| `libretro-m2hle/Config.in` | `package/batocera/emulators/retroarch/libretro/libretro-m2hle/Config.in` |
| `libretro-m2hle/libretro-m2hle.mk.in` | `.../libretro-m2hle/libretro-m2hle.mk`, pinned to a commit |
| `libretro-m2hle/m2hle.libretro.core.yml` | `.../libretro-m2hle/m2hle.libretro.core.yml` (ES: system, extensions, features) |
| `add-to-batocera.sh` | puts the three in, and the four one-line edits below |
| `ci/` | a BR2_EXTERNAL holding only the package, for `.github/workflows/batocera.yml` |

`add-to-batocera.sh` also edits, one line each, beside the neighbours batocera
already has:

- `Config.in`: sources the package after libretro-flycast;
- `package/batocera/core/batocera-system/Config.in`: selects it with the arcade
  systems, after sm2-emu, on x86_64 (OpenGL 4.1 core) and on GLES 3 boards
  (OpenGL ES 3), the boards the canary's Linux cores already run on;
- `python-src/batocera-launch-libretro/pyproject.toml`: names the core for
  batocera-launch (`m2hle = 'batocera_launch_libretro:RACore'`). A core with no
  entry gets the bare `Core` class, not RetroArch's.

These are the steps of the wiki's [How to add an emulator](https://wiki.batocera.org/batocera.linux_buildroot_modifications#how_to_add_an_emulator)
for a libretro core: the package and its `.mk`, sourcing it, selecting it for
the boards, the ES features file (the `.core.yml`, which `emulator-info-package`
installs and `batocera-build-es-data` reads), and the launcher.

## Making the batocera.linux PR

```bash
git clone https://github.com/batocera-linux/batocera.linux.git
packaging/batocera/add-to-batocera.sh batocera.linux             # HEAD of this checkout
packaging/batocera/add-to-batocera.sh batocera.linux --rev <sha> # or another commit
```

`--rev` must be a commit GitHub has (pushed): the package downloads its
tarball. The `.mk` gets that commit, the version stamp the canary uses
(`rNNN-<sha>`, shown as the core's version in RetroArch) and the submodule
commits that revision records. Run it again with a newer `--rev` to move the
package on; only the package directory changes then. Don't edit the hashes in
the `.mk` by hand.

Batocera's own build checks hashes against a `.hash` file when one is present;
the package has none, as none of batocera's libretro packages has one.

## CI

`.github/workflows/batocera.yml` runs as part of the canary (`canary.yml`
calls it on every push and PR), once a week on its own (batocera moves too),
and by hand. It never builds all of batocera (hours, and tens of GB):

- **es-data**: adds the package to today's batocera.linux master (twice, to
  prove the script changes nothing the second time), runs batocera's own
  `batocera-build-es-data` over every emulator and core batocera has, and
  checks that the `model2` system offers `libretro/m2hle` for `.zip`.
- **buildroot**, x86_64 and aarch64: batocera's buildroot fork at the commit
  batocera.linux pins, a Bootlin glibc toolchain and Mesa (softpipe, so no
  LLVM), with `ci/` as a BR2_EXTERNAL named `BATOCERA` that holds only the
  package and batocera's `pkg-emulator-info.mk`. Building one package needs only
  its dependencies, but batocera's whole tree as an external pulls in every
  package's Config.in and its own defconfigs. It runs
  `make libretro-m2hle`, which downloads the tarballs as batocera would, and checks the
  core: the machine, only `retro_*` exported, the stamped `.info`. The canary
  publishes the two cores as `m2hle-libretro-batocera-x86_64.zip` and
  `m2hle-libretro-batocera-aarch64.zip` (the `.so` and its `.info`), beside its
  other cores. A failed batocera build doesn't hold the canary back; that zip
  keeps its previous build.

`ci/Config.in` defines the two batocera symbols the `.mk` reads
(`BR2_PACKAGE_BATOCERA_TARGET_X86_64_ANY`, `BR2_PACKAGE_BATOCERA_GLES3`). In
batocera they come from the board's defconfig.
