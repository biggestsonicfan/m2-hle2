# Logitech G25 without Logitech's software (Pinboard #1370)

G Hub does not support the G25. Without Logitech Gaming Software 5.10.127 (or the
older Profiler), Windows sees it as a Driving Force EX. It turns 200 degrees, both
pedals share one axis, there is no clutch or shifter, and there is no force,
because no DirectInput effect driver is installed. m2-hle2 now does the work that
software does, using SDL's hidapi (`src/ui/lg4ff.h`, desktop and SDL3 front ends).
The G27, the G29 (Pinboard #1419), the Driving Force GT and the Driving Force Pro
use the same commands and get the same treatment.

- **Mode switch.** A wheel still in compatibility mode is sent the command that
  turns it into its own model, once. It drops off the bus and comes back under its
  own PID (a G25 is `C299`, a G29 `C24F`). A G29 is switched from either position
  of its PS3/PS4 switch. SDL lists it as a wheel and `sdl_pads.h` plays it like
  any other: by default the wheel, accelerator and brake are axes `0,1,2`
  (`--wheel-axes`), and the gear lever is buttons, set with `--wheel-buttons`.
- **Range.** `--g25-range N` sets the turning range in degrees (default 270, about
  what the cabinets' wheels turn; 40..900; `0` leaves the wheel as it is). This
  replaces the range set in LGS, if it is installed. The DFP takes no range
  command from us. The Controls window's Wheel tab sets it too, per game
  (Rotation; `wheel.range` in the controls file), and sends it at once.
- **Force.** Only when SDL has no haptic for the wheel, which means Windows without
  Logitech's driver. The drive board's force (`ffb.h`) goes to the wheel this way:
  - the push becomes its constant force;
  - the centring uses the wheel's own spring;
  - the uncentring (Daytona's grass) is a reversed spring in slot 3: it pushes
    the wheel on the way it is turned;
  - the clutch (Daytona standing, slow) uses the damper slot. It used to be the
    friction slot, which buzzes on a G25 held against it (Pinboard #1447).

  A report is sent only when something changes. Where SDL has the haptic (Linux's
  `hid-lg4ff`, or Windows with LGS installed), `sdl_pads.h` keeps the force, and we
  only switch the mode and set the range. `--ffb-gain` scales the force either way.
- **Letting go.** No push, no spring, no damper when the window loses focus or the
  run ends. `--g25 off` leaves every Logitech wheel alone.
- **Linux.** The kernel's `hid-lg4ff` already switches the mode and gives the force.
  To set the range from here, the wheel's hidraw node has to be writable, e.g.
  `/etc/udev/rules.d/60-logitech-wheel.rules`:
  `KERNEL=="hidraw*", ATTRS{idVendor}=="046d", ATTRS{idProduct}=="c29[4-9a-b]|c24f|c260", MODE="0666"`.
  The kernel does not switch a G29 in its PS4 mode; with the rule, m2hle does.
  Without that rule, m2hle says so once in the log, and the Wheel tab says so
  under Rotation. It then tries the range through the kernel driver's
  `/sys/bus/hid/drivers/logitech/<device>/range`, which is root's unless a rule
  opens it as well (the one Oversteer installs does):
  `ACTION=="add", SUBSYSTEM=="hid", DRIVERS=="logitech", RUN+="/bin/sh -c 'cd %S%p; chmod 666 range || true'"`.
  See the next section for why 900 degrees comes back without the hidraw rule.
- **Not tried on a real wheel yet:** there is no Logitech wheel here.
  `tests/lg4ff_test.c` checks every report byte for byte. `tests/lg4ff_host_test.c`
  runs `lg4ff.h` on a pretend G25 that starts in compatibility mode, then on a
  pretend G29 in its PS4 mode. The direction
  of the push (above `0x80` = right) follows SDL's lg4ff haptics; it is not seen in
  the binary.

## Linux: 900 degrees, a wheel that steers by itself, buttons (Pinboard #1439)

A G29 owner on Ubuntu 26.10 found the wheel stuck at 900 degrees, the car steering
with the wheel straight, and the wheel's buttons impossible to set in the Controls
window. No G29 is here; what was found:

- **900 degrees.** Once the hidraw node can be read, SDL 3.4's own HIDAPI `lg4ff`
  driver takes the wheel (it is on by default off Windows). On the wheel's first
  report it sends a range of `SDL_HIDAPI_LG4FF_RANGE` from SDL's environment, 900
  when unset, and turns autocentre off. That came after ours. `lg4ff_sdl_env` now
  puts our range there before `SDL_Init` (both front ends), and the Wheel tab
  updates it. Without the hidraw rule the kernel's `hid-lg4ff` keeps the wheel, at
  its own default of 900, until the sysfs `range` is written (above).
- **Steering by itself.** Not reproduced. A wheel whose straight is not the middle
  of its axis would do it; the Wheel tab's **Centre** button records where the
  wheel reads held straight (`wheel.center`), and each side of that is spread over
  its half of the turn (`wheel_host_steer_centred`). Under the axes, "The game
  reads" shows the board's wheel value against its straight (`0x80` for Sega
  Rally), so an owner can tell whether the offset comes from the wheel or from
  the game. It also warns when a pad or wheel button holds Left or Right: with
  the host's wheel exactly straight, those still walk the board's wheel.
- **Buttons.** Not reproduced either: the per-button action combo opens here with
  and without a (virtual) G29. The tab now lists the actions, each with a **Set**
  button: click it, then press the wheel's button (one that was already held does
  not count). Right-click clears the action's buttons. The per-button combos, for
  combinations, are under "Each button's actions".

## The G25's own layout (Pinboard #1446)

The built-in wheel layout (axes `0,1,2`, paddles for the lever) is wrong for a
G25: SDL lists its axes as wheel `0`, clutch `1`, accelerator `2`, brake `3`
(`SDL_hidapi_lg4ff.c`, and the same on Windows without it). While the built-in
layout is untouched (no controls file changed the axes or buttons, no
`--wheel-axes` / `--wheel-buttons`), a connected G25 (`046D:C299`) gets its own,
the owner's (`g_wheel_presets` in `sdl_pads.h`):

| G25 | Plays |
|---|---|
| axes 0, 2, 3 | wheel, accelerator, brake |
| 6 / 7 (the wheel's two buttons) | Start / Coin |
| 3 / 0 (shifter, black) | Service / Test |
| 4 / 5 (paddles) | lever up / down |
| 12-15 (H-gate 1-4) | Gear 1-4 (the lever stays in a gear until the next, io_5649.h) |
| 11, 8, 9, 10 (shifter, red row) | the view buttons VR1-VR4 (Daytona); 11 is Sega Rally's VIEW |

Daytona's VR3 and VR4 are player 2's B3 / B4 (`daytona.h`), so a wheel or pad
button may now press P2's actions (`p2:b3` in the controls file); the Controls
window lists them only where they are view buttons (`quirks.wheel.view`), and
names every view button "view N". The wheel's d-pad (its first hat) was never
read: it is four more buttons, `wheel.32`-`wheel.35` (up, right, down, left),
"D-pad up" etc. in the Wheel tab, unbound by default. Checked on a virtual G25
(`tests/pads_test.c`); no G25 here.

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
- **I** Conditions are not wheel commands in the DLL (it works them out on the
  host). From new-lg4ff instead: `lg4ff_damper` is slot 4, `81 0c k s k s clip` to
  start it, `8c` to change it, `83` to stop it; `lg4ff_uncenter` is slot 3's spring,
  `41 0b d1 d2 kk ss clip` with both signs set (`4c`, `43`). The friction slot
  (`0e`) is not used: it sticks and slips, a buzz, when the wheel is held.
- **C** Also seen and not used here:
  - G27 rev LEDs `f8 12 <mask>` (masks 1, 3, 7, f, 1f);
  - `fa fe` and `f3` (`FUN_180017a90`, `FUN_180017a20`);
  - `HidD_SetFeature {04,04}` on close.

## The G29 (Pinboard #1419)

LGS 5.10 is older than the G29. These facts come from **LGS 9.04.49 x64**, the last
LGS (`LGS_9.04.49_x64_Logitech.exe` from download01.logi.com, sha256
`6db9f644…d3e0`), read with Ghidra 12.1.4 and objdump. G Hub drives the G29 too;
with G Hub, SDL has the haptic and we only set the range, as with LGS for a G25.

| File | What it is |
|---|---|
| `Drivers/LGJoyHid/WinUSB/LGJoyWinUsb.inf` | binds the G29 in compat mode (`C294` rev `1350`..`1355`) and in its PS4 mode (`C260`) to WinUSB, so LCore.exe can switch it |
| `Drivers/LGJoyHid/LGJoyHid.inf` | `C24F` (`G29Inst`): the HID minidriver and the force CLSID, as for a G25 |
| `Drivers/LGJoyHid/LGJoyHidFilter64.sys` | the newer `wmfilter.sys`: device type `0x3F` for `C24F` |
| `Drivers/VirtualBus/LGJoyXlCore64.sys` | the range |
| `LCore.exe` | the service: the G29's mode switch |

- **C** Which wheel (LCore.exe `0x140238dd0`): VID `046D`, PID `C294`, `C298` or
  `C29A`, `rev & 0xff00 == 0x1300` and `rev & 0xff >= 0x50` is a G29 in its PS3
  mode. The filter's DFGT test (`FUN_1400073c0`) is the same with `< 0x50`: a
  `C294` with rev `1350` is a G29, not a DFGT. LGS 5.10 took every `13xx` for a DFGT,
  and so did #1370's first `lg4ff_identify`.
- **I** Linux's `hid-lg4ff` also takes rev `89xx` for a G29 (its second ident). Not
  seen in LGS; `lg4ff_identify` takes it too.
- **C** The switch, PS3 mode (LCore.exe `0x140239030`, called from `0x140238ba6`
  for each HID device): `00 f8 09 05 01`, written to the HID device. There is no
  `f8 0a` before it, and no fifth "detach" byte (Linux sends `f8 0a`, then
  `f8 09 05 01 01`). The filter's table has `f8 0a` / `f8 09 05 01` pairs for the
  G29 too (`0x1400095b0`, `0x1400095e8`), but no code reads them. Over WinUSB
  (`0x1402391e0`, for `C294`) it is the same bytes in a `SET_REPORT` (`21 09 00 02`).
  `lg4ff_mode_switch` sends `f8 09 05 01` as an 8-byte report with ID 0.
- **C** The switch, PS4 mode (LCore.exe `0x140239100`, for `C260`): the 6 bytes
  `30 f8 09 05 01 01` written to the wheel's pipe, i.e. the same command as report
  `0x30`, with the detach byte. Linux does not do this. `lg4ff_mode_switch` writes
  the same 6 bytes through hidapi; **I** that this reaches the wheel through the
  HID stack (Windows: an output report `0x30` in its descriptor) is not seen here.
- **C** `C24F` is device type `0x3F` (`LGJoyHidFilter64.sys`, `FUN_1400031a4`): a 16-bit wheel axis (`0xffff`), 8-bit pedals, and the
  `OperatingRange` (default 200, at most 900) like the G25's.
- **C** Range (`LGJoyXlCore64.sys`): for types `0x3F` and `0x40` (the G920) it
  sends `00 f8 81 <lo> <hi>`, the degrees clamped to 40..900, as a 17-byte write;
  for the G25 family the same bytes as an 8-byte one. We send 8 bytes;
  hidapi on Windows pads a write to the device's output report length.
- **I** Force: LGS 9's `LGJoyFrc64.dll` is a 35 KB stub that hands the effects to
  the service, so the G29's force reports are not in it. The G29 plays the same
  `11 08`, `fe 0d`, `81 0e` reports as the G27 in Linux's `hid-lg4ff` and
  new-lg4ff, and gets the same here.
- **C** Also seen, not used: `C261` (G920 in its Xbox mode) gets `0f 00 00 01 42`
  (`0x140239350`).
