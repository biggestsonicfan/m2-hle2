# The QT960 <-> Model 2B link (Pinboard #312)

Two i960s run the same instruction with the same operands, and their answers are compared.
One is an Intel QT960 evaluation board (i960KB) running NINDY. The other is a Model 2B
running [m2-kernel](../../../m2-kernel), the serial monitor kernel. In the end the QT960 is
the real board on a real cable. For now both run in MAME, in the shared build of
claude_mame's fork. Each MAME has its own window, so you can watch the link come up.

```
tools/qt960link/run.sh                   # two windows on $DISPLAY (:1, the container's VNC)
HEADLESS=300 tools/qt960link/run.sh      # no windows; stop after 300 checks
HOST=1 tools/qt960link/run.sh            # the real board's path: qtlink_host.py on the serial line
```

## Run it yourself

In the dev container, from a terminal inside the VNC desktop (`:1`, port 5901, or noVNC
on 6080):

1. Check out the branch (until PR #175 is merged it is not on master):
   `git fetch origin && git switch feat/qt960-link`, or use the existing worktree
   `~/source/repos/ai/.trees/m2-hle2/feat-qt960-link`.
2. Run `tools/qt960link/run.sh`. It needs, and finds by itself:
   - the shared MAME, `~/build/mame-bin/mame-shared/shared` (see "What the MAME needs");
   - m2-kernel beside this repo, `~/source/repos/ai/m2-kernel` (its `roms/m2kernel`);
   - `qt960.zip` in `$ROMS_DIR` (`~/build/mameroms`);
   - the i960-elf toolchain in `/opt/i960/bin`, to build `qtlink.bin`;
   - `xdotool`, to get past the warning screens (else press Shift in each window).
3. Two windows open: the Model 2B on the left, the QT960 on the right. Within a few
   seconds both warning screens go. NINDY prints its banner, then the right window fills
   with `mo` lines for about half a minute while qtlink is typed in. After `go 10100000`
   the check lines start scrolling (`... OK`), and the Model 2B's overlay counts QTTESTS
   up with QTBAD staying 0.
4. Close either window, or press Ctrl+C in the terminal, to stop both.

From an ssh shell instead, `DISPLAY=:1 tools/qt960link/run.sh` puts the windows on the
VNC desktop just the same. `HEADLESS=20 tools/qt960link/run.sh` needs no display and ends
with `qt960_link: 20 checks, 0 differences` (about 15 s). Logs and the built `qtlink.bin`
go in `$WORK`; the Model 2B's MAME log is `$WORK/m2kernel.log`.

## Options

`MAME`, `M2K` (an m2-kernel checkout), `ROMS_DIR`, `M2K_PORT` (7960) and `WORK`
(`/tmp/qt960link`) override the defaults. Close either window, or press Ctrl+C, to stop both.

Both drivers are marked not working, so MAME opens each window on its "known problems"
warning, and nothing runs until a key is pressed there (`-skip_gameinfo` does not skip it).
`run.sh` presses Shift in each window a few times with `xdotool`, which also puts the windows
side by side. It is Shift because the QT960 has a terminal keyboard, and a space would be
typed to NINDY. Without `xdotool`, press a key in each window yourself.

## What happens

1. The left window is the Model 2B booting m2-kernel. Its serial port reaches TCP through
   m2-kernel's own bridge, `m2k_serial.lua`.
2. The right window is the QT960. NINDY runs its self-test and prints its banner and the
   `=>` prompt. `qt960_link.lua` acts as the terminal at the far end of the QT960's cable.
   It types `qtlink.bin` (built by `qtlink/build.sh` with the i960-elf toolchain) into SRAM
   one word at a time with `mo 10100000 <count>`, then types `go 10100000`. Loading takes
   about half a minute.
3. qtlink pings the kernel (`MK01`), reads its STATUS, and uploads the stubs in `stubs.s`
   into the kernel's RAM at `0x230000`. It reads them back to check them, puts a counter
   overlay on the Model 2B's screen, and starts checking. Each check runs one stub on the
   QT960 with a plain `call` and on the Model 2B with the kernel's CALL, then prints both
   answers:

   ```
   00034 EXTRACT   E2E8BB73 0000000A 0000000D 00001A2E 00001A2E  OK
   ```

   The columns are the test number, the instruction, the operands g0, g1 and g2, the
   QT960's answer, the Model 2B's answer, and OK or DIFF. The Model 2B's overlay counts the
   tests (QTTESTS), the mismatches (QTBAD) and the last answer (QTLAST).

The QT960 has one serial port, and NINDY's terminal uses it, so the link travels over it as
text. A kernel command frame (`A5 cmd len payload sum`, m2-kernel's `src/m2k_proto.h`) goes
out as the line `>A5....\r`. The reply (`5A ...`) comes back typed in as `<5A....\r`.
Everything else on the line is ordinary terminal output. `qt960_link.lua` reaches the
82510 UART through read and write taps on its registers, so the driver is unchanged.

## What the MAME needs

The fork's `shared` branch has everything. It needs three fixes beyond the qt960 driver
(`qt960-driver` branch):

- **BURST regions** (`0d6e6139eea`). The driver mapped its memory without MAME's
  `BURST` flag, so `ldl`/`stl`/`ldq`/`stq` hit one address over and over. The
  `STL+LD`, `ST+LDL` and `ST+LDQ` stubs check that this is fixed.
- **`modtc`** (`f97a7b69ccd`). NINDY's `go` sets the trace controls before it calls the
  program. Without it, the CPU stops on "Unhandled 65.4".
- **`modify` and `extract`** (`b61b6f88f60`). The i960 core was missing both.

## NINDY 3.01 facts this needed

- `mo addr count` reads the count as **decimal**. Each word's prompt is `addr : old : `.
  NINDY reads the line while it prints (its ^S/^C check), so anything typed before the
  second colon is lost. The loader waits for ` : <hex> : `.
- The `=>` printed before the `Version 3.01` banner belongs to the self-test. Wait for the
  one that follows the banner.
- **A program cannot return to NINDY in MAME.** NINDY takes a program back through a
  breakpoint trace fault: its exit stub is `fmark; syncf; .word 0xfeedface`, and the fault
  handler at 0x5150 recognises it. MAME's i960 raises no trace faults. After a plain `ret`,
  NINDY's "program running" flag stays set, its command loop returns out of the monitor,
  and the CPU ends up at 0x314. So qtlink loops forever when it stops (`crt0.s`). Reset the
  board to get NINDY back.

## On the real board

No flash upload is needed. qtlink runs from the QT960's SRAM at 0x10100000, and the PC
types it in over the serial cable through NINDY's `mo`, the same way `qt960_link.lua` does
in MAME. It is gone at power-off, so the PC loads it again each time. That is about a
minute at NINDY's 9600 baud (1652 words). NINDY's `df` could put a program in the board's
flash, but qtlink is linked for SRAM, and the PC has to stay on the cable to relay the
Model 2B's frames anyway. Flash would gain nothing.

`qtlink_host.py` is the program for that PC. It waits for NINDY's `=>`, loads qtlink with
`mo`, types `go 10100000`, and relays: each `>A5...` line goes to the Model 2B as bytes, and
each reply is typed back as `<5A...`.

1. Build the image: `tools/qt960link/qtlink/build.sh` (i960-elf toolchain), or take `qtlink.bin` from
   `$WORK` after any `run.sh`.
2. Cable the QT960's serial port to the PC (9600 8N1, no flow control). For a COM port the
   script needs pyserial (`pip install pyserial`).
3. Start the Model 2B end. For now that is MAME, from the dev container or any machine with
   the shared MAME: `M2K_PORT=7960 mame m2kernel -autoboot_script m2k_serial.lua` (see
   `run.sh`). A real Model 2B running m2-kernel would be a second serial port instead.
4. Run the host, then power on or reset the QT960 (or press Enter at its prompt):

   ```
   python3 tools/qt960link/qtlink_host.py --board COM3 --m2k tcp:127.0.0.1:7960 --bin qtlink.bin
   python3 tools/qt960link/qtlink_host.py --board /dev/ttyUSB0 --m2k /dev/ttyUSB1
   ```

   The board's output scrolls by as it would on a terminal, then the check lines.
   `--checks N` stops after N checks (exit status 1 on any DIFF), `--quiet` prints only the
   count, the DIFF lines and the summary, `--no-load` skips the `mo` when qtlink is already
   in SRAM. qtlink never returns to NINDY, so reset the board to stop it.

The same program drives the emulated QT960. `qt960_wire.lua` puts the QT960's serial port
on TCP (`QTWIRE_PORT`, 7961) and does nothing else, so the board in MAME sees what a real one
on a cable would. `HOST=1 tools/qt960link/run.sh` runs the link that way (and
`HOST=1 HEADLESS=30` with no windows: 30 checks, 0 differences). Nothing on that path is
specific to MAME, so it is the test of the host program before it meets the board.

Not yet checked on hardware: the real board's memory map (the driver's SRAM is 2 MB at
0x10000000) and the 82510's registers. qtlink polls LSR at 0x20000014 and reads and writes
data at 0x20000000, as NINDY leaves the chip, bank 0. If either differs, the `mo` load still
works (it is NINDY's), and qtlink is what needs changing. The reply timeout (`SPINS_BYTE`,
a few seconds on the board) is generous, because the two ends need not run at the same speed.
