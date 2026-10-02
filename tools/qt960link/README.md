# The QT960 <-> Model 2B link (Pinboard #312)

Two i960s run the same instruction with the same operands, and their answers are compared.
One is an Intel QT960 evaluation board (i960KB) running NINDY. The other is a Model 2B
running [m2-kernel](../../../m2-kernel), the serial monitor kernel. In the end the QT960 is
the real board on a real cable. For now both run in MAME, in the shared build of
claude_mame's fork. Each MAME has its own window, so you can watch the link come up.

```
tools/qt960link/run.sh                   # two windows on $DISPLAY (:1, the container's VNC)
HEADLESS=300 tools/qt960link/run.sh      # no windows; stop after 300 checks
```

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

`qtlink.bin` is built for the board as it is: it loads at 0x10100000 through NINDY's `mo`
or a download, and starts with `go 10100000`. On the PC end of the real cable, something
has to do what `qt960_link.lua` does in relay mode. It must carry each `>` line to the
Model 2B (a real board's serial port, or MAME's `m2k_serial.lua` over TCP) and type each
reply back as a `<` line. That script is not written yet. The reply timeout is generous
(`SPINS_BYTE`, a few seconds on the board), because the two ends need not run at the same
speed.
