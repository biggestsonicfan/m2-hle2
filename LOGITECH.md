# Logitech G25 without Logitech's software (Pinboard #1370)

G Hub does not support the G25. Without Logitech Gaming Software 5.10.127 (or the
older Profiler), Windows sees it as a Driving Force EX. It turns 200 degrees, both
pedals share one axis, there is no clutch or shifter, and there is no force,
because no DirectInput effect driver is installed. m2-hle2 now does the work that
software does, using SDL's hidapi (`src/ui/lg4ff.h`, desktop and SDL3 front ends).
The G27, the Driving Force GT and the Driving Force Pro use the same commands and
get the same treatment.

- **Mode switch.** A wheel still in compatibility mode is sent the command that
  turns it into its own model, once. It drops off the bus and comes back under its
  own PID (a G25 is `C299`). SDL lists it as a wheel and `sdl_pads.h` plays it like
  any other: by default the wheel, accelerator and brake are axes `0,1,2`
  (`--wheel-axes`), and the gear lever is buttons, set with `--wheel-buttons`.
- **Range.** `--g25-range N` sets the turning range in degrees (default 270, about
  what the cabinets' wheels turn; 40..900; `0` leaves the wheel as it is). This
  replaces the range set in LGS, if it is installed. The DFP takes no range
  command from us.
- **Force.** Only when SDL has no haptic for the wheel, which means Windows without
  Logitech's driver. The drive board's force (`ffb.h`) goes to the wheel this way:
  - the push becomes its constant force, with the shake added on top as a
    60 ms square wave;
  - the centring uses the wheel's own spring;
  - the friction uses its friction slot.

  A report is sent only when something changes. Where SDL has the haptic (Linux's
  `hid-lg4ff`, or Windows with LGS installed), `sdl_pads.h` keeps the force, and we
  only switch the mode and set the range. `--ffb-gain` scales the force either way.
- **Letting go.** No push, no spring, no friction when the window loses focus or the
  run ends. `--g25 off` leaves every Logitech wheel alone.
- **Linux.** The kernel's `hid-lg4ff` already switches the mode and gives the force.
  To set the range from here, the wheel's hidraw node has to be writable, e.g.
  `/etc/udev/rules.d/60-logitech-wheel.rules`:
  `KERNEL=="hidraw*", ATTRS{idVendor}=="046d", ATTRS{idProduct}=="c29[4-9a-b]", MODE="0666"`.
  Without that rule, m2hle says so once in the log and does nothing more.
- **Not tried on a real wheel yet:** there is no Logitech wheel here.
  `tests/lg4ff_test.c` checks every report byte for byte. `tests/lg4ff_host_test.c`
  runs `lg4ff.h` on a pretend G25 that starts in compatibility mode. The direction
  of the push (above `0x80` = right) follows SDL's lg4ff haptics; it is not seen in
  the binary.

## Where it was read

Taken from LGS 5.10.127 x64 (`lgs510_x64.exe` from download01.logi.com, sha256
`e07a278e…ca76`, the same file the Chocolatey package uses), in
`Common64/Logitech/WMDrivers/`, with Ghidra 12.1.4 and rea 6.7.0 (`rea function`
with the ghidra provider). **C** marks what is in the binary; **I** marks what is
inferred, or taken from Linux / SDL's `hid-lg4ff` port (new-lg4ff) because LGS does
it differently.

| File | What it is |
|---|---|
| `wmjoyhid.inf` | G25Inst: the `WmHidLo` HID minidriver plus the `WmJoyFrc` force CLSID. Axes 0 wheel, 1 combined, 2 accelerator, 5 brake, 6 clutch; 19 buttons |
| `wmfilter.sys` | a filter below the HID class driver: identifies the wheel, sends the mode switch, reads `OperatingRange` / `CombinePedals` / `ModeSwitchDelay` from the registry |
| `wmxlcore.sys` | the range and the per-device input translation |
| `wmjoyfrc.dll` | the DirectInput effect driver: the force reports, written from user mode |

### Transport

- **C** Every command is an 8-byte output report: report ID `00`, then 7 bytes.
  `wmfilter.sys` sends its commands as an `IRP_MJ_WRITE` of length 8 to the HID
  collection below it, from a system thread (`FUN_00011954` → `FUN_00011a9c` →
  `FUN_00010a18`). That is a plain `WriteFile`.
- **C** `wmjoyfrc.dll` writes with `WriteFileEx` on the HID handle (`FUN_18000b0f0`,
  vtable slot `0x38`). It sets byte 0 to the device's report ID, waits at least
  8 ms between writes, and gives up after 20 waits of 50 ms. Reports queue in
  32 slots (`FUN_18000b2b0` pushes, `FUN_18000b310` pops).
- **I** SDL's own lg4ff joystick driver is off on Windows; its comment blames
  hid.dll padding the reports. Its writes are 7 bytes starting `f8`, with no
  report-ID byte, and hidapi on Windows reads byte 0 as the ID. Ours lead with `00`.

### Which wheel (`wmfilter.sys`)

- **C** Device types: `C298` DFP `0x33`, `C299` G25 `0x38`, `C29A` DFGT `0x39`,
  `C29B` G27 `0x3A`. `C294` is `0x35` / `0x36` / `0x37` for revisions `20xx` /
  `21xx` / `22xx`, otherwise `0x2B`. For the 0x38-0x3A wheels the wheel axis is 14
  bits (`0x3fff`) and the pedals 8 bits.
- **C** The revision (bcdDevice) says what a compat wheel really is:
  - `C294` or `C298` with `rev & 0xff00 == 0x1200` is a G25;
  - `rev & 0xfff0 == 0x1230` on `C294`, `C298` or `C299` is a G27 (`FUN_00011918`);
  - `13xx` is a DFGT;
  - `1xxx` on `C294` is a DFP.

  `lg4ff_identify` tests for the G27 first. A `C294` with revision `2xxx` is a real
  Driving Force EX and is left alone.
- **C** The switch is sent `ModeSwitchDelay` ms (default 100) after the device starts
  (`FUN_00012080`). The commands:

  | Wheel | Command(s) |
  |---|---|
  | G25 | `f8 10` |
  | G27 | `f8 0a`, then `f8 09 04 01` |
  | DFGT | `f8 0a`, then `f8 09 03 01` |
  | DFP | `f8 01` |

  They match SDL's and Linux's tables.

### Range (`wmxlcore.sys`)

- **C** `FUN_00012b84`: for device types `0x33` and `0x38`-`0x3A` it sends
  `f8 81 <deg lo> <deg hi> 00 00 00`, the degrees clamped to [min, 900], through the
  lower interface's callback at `+0x18`. `wmfilter.sys` reads `OperatingRange`
  (default 200) for it.
- **I** The DFP gets the range in two other commands (`f8 02`/`f8 03` and `81 0b`,
  per SDL). We don't send those; `lg4ff_has_range` is false for the DFP.

### Force (`wmjoyfrc.dll`)

- **C** Constant force, `FUN_18000c640` (the effect loop):
  - the summed force is clamped to -127..127 and then to the device's min/max
    (vtable `+0xd8`);
  - the report is `11 08 <0x80 + force> <0x80 - second axis> 00 00 00`;
  - a force of exactly `0x80` is sent as `13 00 ...` (slot 1 stopped).

  `lg4ff_constant` sends `11 08 f 80`, or `13 00` for no force.
- **C** The wheel's centring spring, `FUN_180012aa0` / `FUN_180014f20`:
  - the spring is `fe 0d k k s 00 00`, then a 64-byte `f4`, or `f5` when the
    device's flags say so;
  - `FUN_1800121a0` is the older wheels' `fe 03` form, with the k values 5 and 6
    swapped;
  - k and s are read from a per-device min/mid/max table and interpolated
    (`FUN_1800118c0`; 10000 is mid, 15000 max).
- **I** That per-device table is not in the DLL. `lg4ff_autocenter` uses Linux's
  curve instead (halved for every wheel but the MOMO, as Linux and SDL do):
  `f5` (off), then `fe 0d a a b`, then `14` (on).
- **I** Friction is not a wheel command in the DLL (it works friction out on the
  host). `lg4ff_friction` uses new-lg4ff's slot 4: `81 0e k k ff 00` to start it,
  `8c` to change it, `83` to stop it.
- **C** Also seen and not used here:
  - G27 rev LEDs `f8 12 <mask>` (masks 1, 3, 7, f, 1f);
  - `fa fe` and `f3` (`FUN_180017a90`, `FUN_180017a20`);
  - `HidD_SetFeature {04,04}` on close.
