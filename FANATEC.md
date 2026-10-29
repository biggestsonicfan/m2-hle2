# Fanatec wheels: rev LEDs and display (Pinboard #1361)

A Fanatec wheel base drives Daytona USA and Sega Rally in m2-hle2 like any wheel,
and its rev LEDs and three-digit display show the game's meters.

- **Force** needs nothing of ours. On Windows the base is a DirectInput wheel
  (Fanatec's driver, `FAWForceFeedbackDrv64`), SDL lists it as a wheel, and
  `sdl_pads.h`'s haptics drive it (`--ffb-gain`). On Linux the out-of-tree
  `hid-fanatecff` module gives it the force.
- **Lights** (`src/ui/fanatec.h`, desktop and SDL3 front ends): while a game with a
  dashboard runs, the first Fanatec base found gets the rev LEDs (base, rim, and
  an RGB rim's colours), filled from the profile's `rpm_lo` and flashing at the
  limiter. Its display shows the gear (`N`, `1`..`4`), or the speed with
  `--fanatec speed`. `--fanatec off` leaves the wheel alone. The LEDs and the
  display go back to the wheel when the game has no dashboard, the window loses
  focus, or the run ends.
- **Found as a wheel** by its vendor ID (0EB7) and a PID below 0x1000
  (`fanatec_is_base`), as well as by SDL's wheel list. Every base PID known is
  below 0x1000 and every pedal PID known (183B, 6204-6206) above; SDL 3.4 lists
  ten (0001, 0004-0007, 0011, 0020 = CSL DD, GT DD Pro
  and ClubSport DD in PC mode, 0197, 038E, 0E03), and the rule adds the SDK's
  others (0002, 0021-002F, 0E04-0E30, 0F00-0FFF). A base stays the wheel when
  SDL has a gamepad mapping for it, and of two copies of one base the one that
  moves steers. The console prints every joystick that is not a gamepad, with
  its VID:PID and what it was taken for. A base still missed can be picked as
  the wheel in the Controls window (Pinboard #1416).
- **The lights' HID open** lists every 0EB7 collection with SDL's
  `SDL_HINT_HIDAPI_ENUMERATE_ONLY_CONTROLLERS` off for the call (on by default,
  it hides a collection that is not a joystick or gamepad), and when no base
  opens it logs once each device it saw, with its usage and path.
- **The force** is SDL's haptics on the wheel (DirectInput through Fanatec's
  driver; hid-fanatecff on Linux). A wheel that gets none prints why: no haptic
  device, the open's error, or no effect it takes.
- **Linux** needs the base's hidraw node to be writable, e.g.
  `/etc/udev/rules.d/60-fanatec.rules`:
  `KERNEL=="hidraw*", ATTRS{idVendor}=="0eb7", MODE="0666"` (or `TAG+="uaccess"`).
  Without it, m2hle says so once in the log and leaves the lights off. With
  `hid-fanatecff` loaded, our writes share the device with the module's.
- Not tried on a real wheel yet: no Fanatec hardware here. `tests/fanatec_test.c`
  checks the reports byte for byte against the SDK, and
  `tests/fanatec_host_test.c` runs `fanatec.h` on a pretend DD1.

## The game's dashboard (`game_profile_t.dash`)

A host read of the board's RAM once a drawn frame (`mem_peek*`: no bus counters,
no callbacks), so it changes nothing the board computes and needs no netplay
protocol bump. `get_status` reports it as `"dash"`.

| Game | In a race | Speed (km/h) | Gear | RPM |
|---|---|---|---|---|
| Daytona USA (`daytona`, not the clones) | car pointer `0x501220` in `0x500000..0x51FE00` (it is `0x505180`) | int32 at car+`0x18` | byte at car+`0x14C`, 0..3 (+1) | int16 at car+`0x154`, limiter 8500 |
| Sega Rally (`srallyc`) | mode byte `0x202098` == 3 | float at `0x500080` × 64.5 | word at `0x500078`, 1..4 | float at `0x50007C`, 8800 at the limiter |

Checked against the HUD on a run: Sega Rally's speed within 1 km/h (the HUD lags
while braking) and its gear on every shot; Daytona's speed on every shot.

## The protocol

Read in Ghidra from Fanatec's driver package 474
(`FanatecCpl_EndorFanatecSdk64_VS2019.dll`, the SDK; `FAWForceFeedbackDrv64`, the
force). Addresses are the SDK's image (base 0x180000000) unless marked FFB.
**C** = seen in the binary, **I** = inferred. Byte strings are the bytes after the
report ID, unless they start `ff` (a 64-byte report whose first byte is 0xFF).
`src/core/fanatec_proto.h` builds the reports m2hle sends.

### 1. Framing

- **C** Every command is queued as a 64-byte buffer (`FSWinCmdUsbHid` slot 0
  FUN_1800467e0 -> `HidCommandSubmit` FUN_18003d280). A 1 ms `timeSetEvent` thread
  (FUN_18003d550, tick FUN_18003d7a0/FUN_18003d020) writes them with **WriteFile**
  (FUN_180046ac0), not `HidD_SetOutputReport`. The FFB driver has the same class
  (`FSWinCmdUsbHid::Write WriteFile`).
- **C** FUN_180046ac0:
  - `buf[0]==0xFF`: written as is, 64 bytes, to the **col03** handle (this+0x10).
  - otherwise: **8 bytes** to the main (col01) handle, and `buf[0]` is overwritten
    with the TLC mode byte (this+0x20). So a regular command is
    `[ID][7 data bytes]`, ID 0x00 on single-TLC bases and 0x01 on multi-TLC bases.
- **C** The mode comes from FUN_180041c80(pid) (the same test as the export
  `FSUtilHasWheelBaseMultipleTopLevelCollection2`). Multi-TLC (ID 1 + col03) for VID
  0x0EB7 PIDs 0x0004-0x0007, 0x0020-0x002F, 0x0E04-0x0E30, 0x0F10-0x0F1F,
  0x0F40-0x0FEF. Everything else is single-TLC (ID 0, no 0xFF reports).
- **C** The constructor (FUN_180045f00 -> FUN_180046820) uses `CreateFileW`
  (R/W, share 3). For multi-TLC, FUN_1800463e0 derives the col03 path from the col01
  path (`col01`->`col03`, `0000#`->`0002#`). The enumerator (FUN_180037cf0/
  FUN_1800407b0) skips the col02/col03 paths.
- **I** On Linux, hidraw has one node per USB interface, not per collection. Write
  both the 8-byte `[01]...` reports and the 64-byte `[ff]...` reports to that node,
  first byte = report ID (SDL_hid_write uses the same convention). On a single-TLC
  base, send `00` + 7 bytes.
- **C** Merge: when both the normal queue and the "FullForce" queue (slot 8
  FUN_180046800 -> FUN_18003d380) hold data, and FUN_180041b50 holds (`cmd[0]<2`
  and `cmd[1]&0xf` is 1 or 3, which is an FFB command, see section 5), the tick
  copies the 8-byte command into the 64-byte buffer at offset 0x22.
- **C** PID -> base type (FUN_180040410): 0x0001=10 (CSW V2), 0x0002=0xb (CSL
  Elite), 0x0004=0xd (CSW V2.5), 0x0005=0xc, 0x0006=0xe (DD1), 0x0007=0xf (DD2),
  0x0020=0x11, 0x0E03-04=0xc, 0x0E05-06=0x10, 0x0E07=0x12, 0x0E08-0E30=0xc,
  0x038E=9 (CSW V1), 0x0F0x=9, 0x0F1x=0xd, 0x0F2x-0F3x=0xb, 0x0F4x=0xc,
  0x0F5x=0xe, 0x0F6x=0xf, other 0x0Fxx=0x13, else 1.

### 2. Rev LEDs

Which object is created (factory FUN_1800326a0, handle types 6 and 8):

- **C** Type 8 is the **base** rev LEDs, `FSCmdLedRevsWheelBase` (FUN_1800260f0).
  It exists only when FUN_18003f090(base type) != 0, i.e. base types 0xb/0xc
  (9 LEDs).
- **C** Type 6 is the **rim** rev LEDs. If the RGB flag is clear, or
  FUN_180041d80 holds (rim 0x14, or the second argument is 2), the bitmask class
  `FSCmdLedRevsWheelRim` (FUN_180025d70) is used. Otherwise `FSCmdLedRevsRGB`
  (FUN_180025560), with multi-TLC = FUN_180041c80(pid).
  - **I** That the flag means "the rim reports RGB LEDs".

#### 2a. Bitmask rev LEDs (`FSCmdLedRevsBase`, vftable 0x1801019c0)

- **C** Base, 9 LEDs (header from FUN_1800260f0, report built by FUN_1800244f0):
  `f8 13 <m0> <m1>`. Bit j of m0 = LED j (0-7), bit 0 of m1 = LED 8. All off =
  `f8 13 00 00`.
- **C** Rim, 9 LEDs (FUN_180025d70): `f8 09 08 <hi> <lo>`, with v = hi<<8|lo
  (9 bits). Bit k = LED (8-k), so LED 0 is the MSB (bit reversal via FUN_180042b90).
- **C** Rev-stripe rims (rim types 7, 8, 0xf; FUN_180041eb0): before the first
  bitmask send, the translator is turned on with `f8 09 07 01`
  (RevLedTranslatorEnable FUN_180025130 sends `f8 09 07 <p>`).
- **C** Stripe colour (FUN_1800242a0, sent when the colour is dirty): translator off
  (`f8 09 07 00`), then `f8 09 08 <hi> <lo>` with a 9-bit **BGR333** value: B = bits
  0-2, R = bits 3-5, G = bits 6-8. The SDK only uses the full values:

  | colour | hi lo | colour | hi lo |
  |---|---|---|---|
  | red | `00 38` | cyan | `01 c7` |
  | blue | `00 07` | magenta | `00 3f` |
  | green | `01 c0` | yellow | `01 f8` |
  | white | `01 ff` | off | `00 00` |

  `ItemSetColor` (slot 0x30, FUN_180024c10) works only on stripe rims, index 0.
- **C** Rim white LED (slot 0x40, FUN_1800252a0; sent first by SubmitToDevice
  FUN_180025300, and by FUN_180025c30 for RGB): `f8 09 02 <0|1>`.
- **C** Rim-to-base mirror (slot 0x38, FUN_180025210 -> FUN_180025460):
  `f8 14 00` = mirror on (the base copies the rim), `f8 14 ff` = mirror off (the host
  drives the base).

#### 2b. RGB rev LEDs (`FSCmdLedRevsRGB`, vftable 0x1801020a8), 9 LEDs

- **C** The SDK's default colours (FUN_1800259e0): LEDs 0-2 yellow, 3-5 red, 6-8 blue.
- **C** Percent to channel (FUN_180040650): clamp(0..100)/100 * max, with R max 31,
  G max 63, B max 31 (RGB565 levels).
- **C** **Multi-TLC** (`FSCmdLedRGB`, FUN_180026260):
  - Colour (FUN_180026880), only when something changed: 64-byte
    `ff 01 00`, then LED i as a big-endian u16 at offset 3+2i:
    `b<<11 | g<<5 | r` (blue in the high bits, red in the low bits).
  - Byte 21 = 1 when the save flag is set (FUN_180026630).
    - **I** The save flag is `FSLedColorsSave`, which stores the colours in the
      wheel.
  - Brightness (FUN_180026670): `ff 01 03`. Bytes 3..11 are the per-LED level,
    bytes 15/16/17 are three extra brightness slots (indexes 101-103,
    FUN_180026b20), and byte 18 = 1 to save (FUN_180026620).
  - Level (FUN_18003edc0): 0 -> 0, else max(1, ceil(pct/13)), so 0..7.
- **C** **Single-TLC** (`FSCmdLedRGBSingleTLC`, FUN_180027220; send
  FUN_180028070 -> FUN_180027710):
  - `f8 09 0a <b0 b1 b2 b3>`: 27 on/off bits, LSB-first across b0..b3. Bit 3i = R≠0,
    3i+1 = G≠0, 3i+2 = B≠0 (FUN_180027f30). So 1-bit colour, 8 colours.
  - Then `f8 09 0c <level 0..7> <1 if save>`.

### 3. Three-digit 7-segment display (`FSCmdLedSevenSegmentDisplayThreeDigits`)

vftable 0x180102a70, constructor FUN_180028c50. It is created (handle type 5) when
FUN_180041e30(rim, x) holds: rim type in {0xa, 0xd, 0x14, 0x15, 0x16, 0x17}, or
x != 0. **C**

- **C** State: 24 segment cells = 3 digits × 8. Digit d uses cells 8d..8d+7. Digit 0
  is the hundreds digit in SetNumber.
  - **I** Digit 0 is the leftmost.
- **C** Report (FUN_180029850): `f8 09 01 02 <d0> <d1> <d2>`. In digit byte d, bit i
  = cell 8d+i (the packing loop uses pow(2,i), FUN_180091aa0).
- **C** Bit order, from the digit font (slot 0x38, FUN_1800292b0) and the char font
  (slot 0x50, FUN_180029ef0):

  | bit | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
  |---|---|---|---|---|---|---|---|---|
  | segment | a (top) | b (top-right) | c (bottom-right) | d (bottom) | e (bottom-left) | f (top-left) | g (middle) | decimal point |

- **C** Digits (switch in FUN_1800292b0; checked by running the decompiled C):
  `0=3f 1=06 2=5b 3=4f 4=66 5=6d 6=7d 7=07 8=7f 9=6f`. The DP is the 4th argument
  (`== 1` sets bit 7).
- **C** Characters (FUN_180029ef0, a `wcsncmp` chain; values from running the
  decompiled function). Unknown characters blank the digit (slot 0x40).
  - Letters: `A=77 B=7c C=58 D=5e E=79 F=71 G=7d H=76 I=06 J=06 K=75 L=38 M=37 N=54 O=5c P=73 Q=67 R=50 S=6d T=78 U=3e V=18 W=7e X=76 Y=6e Z=5b`
  - Digits: `0=3f 1=06 2=5b 3=4f 4=66 5=6d 6=7d 7=07 8=7f 9=6f`
  - Symbols: `+=70 _=08 .=80 ,=10 ;=04 :=41 /=52 \=64 ^=03 (=21 )=0c [=39 ]=0f !=09 ?=53 #=46 *=02 &=62 %=0d ==48 >=62 <=14`
  - `-` and space are not in the table (blank).
  - `+` is tested twice; the second test is dead code.
  - **I** Some glyphs look odd (J=I, `>`=`&`, `-` missing). They are exactly what
    the binary does.
- **C** Methods:

  | slot | function | what it does |
  |---|---|---|
  | 0x08 / 0x10 | FUN_180029e30 / FUN_180029d60 | cell on / off (-1 = all) |
  | 0x18 | FUN_18002afa0 -> FUN_180029ae0 | submit (sends only when a cell changed) |
  | 0x28 | | digit count, 3 (`FSLedDigitCountGet`) |
  | 0x30 | FUN_1800295f0 | set the raw segments a..g, dp of one digit |
  | 0x38 | | set a digit 0-9 + dp |
  | 0x40 | FUN_180029090 | blank a digit |
  | 0x48 | FUN_18002ade0 | `FSLedSetNumber` |
  | 0x50 | | set a char |
  | 0x58 | FUN_18002ad70 | set 3 wchar_t |

- **C** FSLedSetNumber (export FUN_1800508d0 -> slot 0x48, FUN_18002ade0): clamps n
  to 0..999 and always shows three digits with leading zeros: `n/100`,
  `(n%100)/10`, `n%10`, no DP. 5 shows "005".
- **C** Display-mode enable (FUN_18002aeb0; disassembly at 0x18002aefc):
  `f8 09 01 18 <02 = on | 01 = off> 00 00`.
  - The constructor sends "on" (only if the object's 7-seg flag is set).
  - Reset (slot 0x20, FUN_180029030; `FSItmDisplayReset` / release) clears the
    cells, submits, then sends "off".
- **C** When the "display on" state goes to off, submit (FUN_180029ae0) sends
  `f8 09 01 02 00 00 00`.

### 4. Handing control back, and conflicts

- **C** Destroying the bitmask rev object (FUN_180023fc0/FUN_180023e10) sends
  `f8 14 00` (mirror back on). If the translator was enabled, it also sends
  `f8 09 07 00`.
- **C** Destroying the 7-seg object sends `f8 09 01 18 01` (display handed back
  to the wheel).
- **C** ITM (`FSCmdITM`, vftable 0x180101008) destructor FUN_180021240 sends
  `ff 05 02 ..` (ITM off).
- **I** A clean exit for a C reimplementation:
  1. Blank the digits and send `f8 09 01 18 01`.
  2. Send `f8 14 00`.
  3. Send `f8 09 07 00` on stripe rims.
  4. Turn all rev bits off (`f8 13 00 00` / `f8 09 08 00 00`).
- **C** ITM (multi-TLC only, factory type 0xb, 64-byte reports):

  | report | function | what it does |
  |---|---|---|
  | `ff 05 02 <0/1>` | FUN_1800214e0 | ITM enable |
  | `ff 02 02 <page 0..6>` | FUN_1800215a0 | page set |
  | `ff 05 04 <p> <max(1,n)>` | FUN_180021660 | |
  | `ff 05 05 01` | FUN_180021450 | display reset |
  | `ff 05 01 ...` / `ff 05 03 ...` | FUN_1800217c0 / FUN_180022cb0 | |

  - **I** The `ff 05 04` report is unit set; `ff 05 01` / `ff 05 03` are parameter
    values.
- **C** Conflicts with DirectInput FFB:
  - There is no lock between processes. The game's FFB (via FAWForceFeedbackDrv,
    `WriteFile` on the same col01 handle) and the SDK's LED writes share one output
    endpoint.
  - Inside one process, the SDK queues everything through one 1 ms writer and
    may merge FFB commands into a pending 0xFF report (section 1).
  - **I** Two writers each sending at 1 kHz may starve each other. Keep LED updates
    change-only, as the SDK does (dirty flags everywhere).

### 5. Force feedback (FAWForceFeedbackDrv64)

- **C** It is a user-mode DirectInput effect driver (`CFAWForceFeedback`). Effects
  are computed **in software** (`FSThreadFFBCommand::PrivateCalculateAndSend`,
  `CurrentForceValuesSWGet`). The result goes to the wheel as the same 8-byte
  reports (`[ID] b1 b2 ...`), through `FSWinCmdUsbHid::Write`.
- **C** b1 = `slot<<4 | op`, where op 1 = set/play and op 3 = stop. b2 = effect.
- **C** Constant, 0x08 (FFB FUN_180022dc0, always slot 0, magnitude clamped to
  ±10000):
  - 16-bit: `01 08 <lo> <hi> 00 00 01`, with u16 = 0x8000 at 0, else
    |m|*32767.5/10000 + 32767.5, mirrored as 65535-v for m<0 (FFB
    FUN_180026b10). This is offset binary, little-endian.
  - 8-bit: `01 08 <v> 00 00 00 00`, with v = 0x80 if |m|<40, else
    |m|*126/9931 + 128.5 (negated for m<0) (FFB FUN_180026ab0).
  - 16-bit is used when firmware > 0x297 and registry
    `HKLM\SOFTWARE\Fanatec Endor AG\FanatecWheel\FFB8BitMode` is 0 (FFB
    FUN_1800230b0).
  - Stop = `03 08`.
- **C** Spring, 0x0b (FFB FUN_180022f90): `s1 0b <d-> <d+> <k> <sgn> <sat>`.
  - d± = pos*128/10000+128, clamped to 255 (FFB FUN_180026a80).
  - k = (k1*15/10000)<<4 | (k2*15/10000) (FFB FUN_180026bc0).
  - sgn = coefficient sign flags (FFB FUN_180026ce0).
  - sat = sat*255/10000, with 0 -> 0xff (FFB FUN_180026ca0).
  - Stop = `s3 0b`.
- **C** Damper, 0x0c (FFB FUN_180022eb0): `s1 0c <k+> <sign+> <k-> <sign-> <sat>`.
  - k = coef*15/10000, or coef*255/10000 in "8-bit" mode (FFB FUN_180026a40).
  - Stop = `s3 0c`.
- **C** Reset (FFB FUN_180024230, TryResetEffects): for slot 0..15, sends
  `s3 08`, `s3 0b`, `s3 0c`.
- **C** Periodic, ramp and the other effects are not sent to the hardware. They
  are summed in software into the slot-0 constant force (FUN_1800233f0,
  "lMagnitudeTotal").
- **I** Raw HID FFB from the host is feasible: it is these plain output reports
  and nothing else. No `HidD_SetFeature` handshake was seen in the FFB path. Send
  the constant force at a steady rate (the driver repeats while playing) and
  `03 08` on exit.
- **I** Linux: mainline has no Fanatec FF driver, and `hid-pidff` does not apply.
  The out-of-tree `hid-fanatecff` module provides the FF API and LED/display
  sysfs. Without it, a host can send the reports above over hidraw (needs a udev
  rule for 0EB7). With it loaded, hidraw writes still work but race the module's
  own writes. Not checked against a device.
