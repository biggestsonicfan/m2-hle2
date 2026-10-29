# m2emulator's default controls (Pinboard #1347)

Our default keys follow m2emulator 1.1a's, which is what most Model 2 players
have in their fingers. This page is what m2emulator binds by default, read off
its `EMULATOR.EXE`, and where our defaults stand against it.

## How it was read

m2emulator keeps its control layouts in the program, not in `EMULATOR.INI`.
Each layout is a global object of a `CInputPorts` subclass (MSVC RTTI names
them `_model2_Inputs`, `_daytona_Inputs`, ...), filled by a constructor that
calls the list insert at `0x4519d0` once an entry. The entry is built on the
stack just before the call: a word (port in the low byte, bit mask in the
high), the default key as a dword, a type byte and a name pointer. A game's
descriptor (`0x10ed4b0` holds the table) carries the game's name at +0x04, the
board name at +0x10 and its layout object at +0x14. Keys are DirectInput scan
codes; `0x100` up are the first joystick (`0x100`..`0x103` left, right, up,
down; `0x110`, `0x120`, ... its buttons) and `0xff0000nn` its axis `nn`. The
port is the input byte at `0x5aa874 + port`. Ports 0..6 go to the board, port
7 is the emulator's own (render stats, tilemap toggles); 138 entries have no
name and no key (unused bits).

It was read twice:

- **rea** ([morluto/rea](https://github.com/morluto/rea), 6.7.0, over MCP):
  the 20 constructors from the callers of `0x4519d0`, decompiled and parsed;
  the games from the writers of the descriptor table.
- **Ghidra 12.1** (`tools/m2emu-controls/M2emuControls.java`): the same
  constructors from the raw disassembly, with the stack modelled byte by byte
  (some entries store only the port byte, keeping the last entry's mask), and
  the games from the stores of a layout object at descriptor +0x14.

Both agree: 20 layouts, 406 named entries (port, mask, key, type, name) and
138 unnamed ones (port, mask), no mismatch. 67 games use 18 of the layouts
(`_daytonat_Inputs` and `_vf2_Inputs` are built but no game picks them).
`README.TXT` says the same of the cabinet keys: "service (f1), test (f2)".

To redo it (the EXE is not in the tree; the script refuses another build of it):

```
python3 tools/m2emu-controls/m2emu_controls.py --exe <m2emulator>/EMULATOR.EXE \
    --ghidra ~/build/tools/ghidra --work /tmp/<task>/ghidra > out.md
```

## The layouts in short

- **Fighters, shooters, the board's default** (`_model2_Inputs`; VF2, FV,
  STF, DOA, Dynamite Cop, HOTD, VCop 2, ...): P1 arrows, buttons Z X C V,
  Start 1, Coin 5. P2 is the first joystick only (no keyboard keys), Start 2,
  Coin 6. Service F1 (IN0 0x08), Test F2 (IN0 0x04).
- **Virtual-On** (`_von_Inputs`): the left lever on the arrows, trigger Z,
  button X; the right lever on the joystick.
- **Driving** (`_daytona_Inputs`, `_srally_Inputs`, `_stcc_Inputs`, ...):
  steering, accelerator and brake on joystick axes, with the arrows as their
  digital stand-ins (Up accelerates, Down brakes, Left / Right steer). Gears
  A S D F (G neutral), the view buttons (VR) Z X C V (Sega Rally's one VR on
  Z), Start 1, Coins 5 / 6, Service F1, Test F2.
- **Behind Enemy Lines** is the one layout with Service on IN0 0x04 and Test
  on 0x08; we do not run it.

## Parity

| Input | m2emulator | m2-hle2 before | m2-hle2 now |
|---|---|---|---|
| P1 directions, B1-B4 | arrows, Z X C V | same | same |
| P1 Start, Coin | 1, 5 | same | same |
| P2 directions, B1-B4 | joystick only | I J K L, Del End PgDn Home | unchanged (keyboard P2 kept) |
| P2 Start, Coin | 2, 6 | same | same |
| Service (IN0 0x08) | F1 | F2 | **F1** (and 9, MAME's) |
| Test (IN0 0x04) | F2 | F3 | **F2** |
| Driving: accel / brake | Up / Down (and axes) | P1 B1 / B2 = Z / X | **Up / Down** |
| Driving: VR 1-4 | Z X C V | C V and P2 B3 / B4 | **Z X C V** |
| Driving: gears 1-4, N | A S D F, G | no gear inputs | **A S D F, G** |

Driving games are the profiles with a wheel accelerator and brake
(`input_keys_driving`: Daytona USA and Sega Rally). On them the arrows' Up
and Down move to the pedals, and the profiles' own P1 Up / Down bits sit on
W and Q.

On the web the driving games carry these keys in `m2hle-games.js`
(`DRIVING_KEYS`) and keep their remapping under their own storage key
(`m2hle.keys.<id>`), so a fighter's custom keys and a driving game's do not
overwrite each other. SDL3 has the same driving map; F1 stays its lobby key
there, so its service button is 9 alone.

Three profiles called IN0 0x04 SERVICE: Sonic the Fighters, Virtual-On and
the m2snake homebrew. It is the test switch (MAME's `PORT_SERVICE_NO_TOGGLE`,
m2emulator's Test); they now read TEST 0x04 and SERVICE 0x08 like every
other profile. That changes which bit a netplay peer's SERVICE / TEST sends,
so `NETPLAY_PROTO_REV` went to 35 (37 once merged over master's 36).

With #1137's gear buttons merged, the driving map has them on A S D F G
(desktop, SDL3 and the web), and the web's Service / Test rows are on F1
(and 9) / F2 like the desktop's. #1353 took rev 36 first, so the IN0 change
is rev 37 on master.

## Every layout

Generated by `m2emu_controls.py` (emulator-only entries and unnamed bits left
out; "joy" is the first joystick).

### _model2_Inputs (29 games)

Games: Dead or Alive (Model 2A), Dead or Alive (Model 2B), Dynamite Cop (Model 2A), Dynamite Cop (Model 2B), Dynamite Cop (Model 2C), Dynamite Deka 2 (Model 2A), Dynamite Deka 2 (Model 2B), Fighting Vipers, Gunblade NY, House of the Dead, Last Bronx (Export, Rev A), Last Bronx (Jap, Rev A), Last Bronx (USA, Rev A), Model2 Board Roms, Pilot Kids (Model 2A), Pilot Kids (Model 2B), Sonic Championship, Sonic The Fighters, Virtua Cop 2, Virtua Fighter 2, Virtua Fighter 2 (Rev A), Virtua Fighter 2 (Rev B), Virtua Fighter 2.1, Virtua Striker, Virtua Striker (Rev A), Zero Gunner (Jap, Model 2A), Zero Gunner (Jap, Model 2B), Zero Gunner (Model 2A), Zero Gunner (Model 2B)

| Input | Port | Mask | Key |
|---|---|---|---|
| Player 1 Up | 1 | 0x20 | Up |
| Player 1 Down | 1 | 0x10 | Down |
| Player 1 Left | 1 | 0x80 | Left |
| Player 1 Right | 1 | 0x40 | Right |
| Player 1 Button 1 | 1 | 0x01 | Z |
| Player 1 Button 2 | 1 | 0x02 | X |
| Player 1 Button 3 | 1 | 0x04 | C |
| Player 1 Button 4 | 1 | 0x08 | V |
| Player 1 Start | 0 | 0x10 | 1 |
| Player 1 Coin | 0 | 0x01 | 5 |
| Player 2 Up | 2 | 0x20 | joy Up |
| Player 2 Down | 2 | 0x10 | joy Down |
| Player 2 Left | 2 | 0x80 | joy Left |
| Player 2 Right | 2 | 0x40 | joy Right |
| Player 2 Button 1 | 2 | 0x01 | joy button 1 |
| Player 2 Button 2 | 2 | 0x02 | joy button 2 |
| Player 2 Button 3 | 2 | 0x04 | joy button 3 |
| Player 2 Button 4 | 2 | 0x08 | joy button 4 |
| Player 2 Start | 0 | 0x20 | 2 |
| Player 2 Coin | 0 | 0x02 | 6 |
| Service | 0 | 0x08 | F1 |
| Test | 0 | 0x04 | F2 |

### _model2a_Inputs (2 games)

Games: Virtua Cop, Virtua Cop (Rev A)

| Input | Port | Mask | Key |
|---|---|---|---|
| Player 1 Up | 1 | 0x20 | Up |
| Player 1 Down | 1 | 0x10 | Down |
| Player 1 Left | 1 | 0x80 | Left |
| Player 1 Right | 1 | 0x40 | Right |
| Player 1 Button 1 | 1 | 0x01 | Z |
| Player 1 Button 2 | 1 | 0x02 | X |
| Player 1 Button 3 | 1 | 0x04 | C |
| Player 1 Button 4 | 1 | 0x08 | V |
| Player 1 Start | 5 | 0x10 | 1 |
| Player 1 Coin | 5 | 0x01 | 5 |
| Player 2 Up | 2 | 0x20 | joy Up |
| Player 2 Down | 2 | 0x10 | joy Down |
| Player 2 Left | 2 | 0x80 | joy Left |
| Player 2 Right | 2 | 0x40 | joy Right |
| Player 2 Button 1 | 2 | 0x01 | joy button 1 |
| Player 2 Button 2 | 2 | 0x02 | joy button 2 |
| Player 2 Button 3 | 2 | 0x04 | joy button 3 |
| Player 2 Button 4 | 2 | 0x08 | joy button 4 |
| Player 2 Start | 5 | 0x20 | 2 |
| Player 2 Coin | 5 | 0x02 | 6 |
| Service | 5 | 0x08 | F1 |
| Test | 5 | 0x04 | F2 |

### _von_Inputs (2 games)

Games: Virtual On Cybertroopers (Japan), Virtual On Cybertroopers (US)

| Input | Port | Mask | Key |
|---|---|---|---|
| Left Stick Up | 1 | 0x20 | Up |
| Left Stick Down | 1 | 0x10 | Down |
| Left Stick Left | 1 | 0x80 | Left |
| Left Stick Right | 1 | 0x40 | Right |
| Left Stick Trigger | 1 | 0x01 | Z |
| Left Stick Button | 1 | 0x02 | X |
| Player 1 Start | 0 | 0x10 | 1 |
| Player 1 Coin | 0 | 0x01 | 5 |
| Right Stick Up | 2 | 0x20 | joy Up |
| Right Stick Down | 2 | 0x10 | joy Down |
| Right Stick Left | 2 | 0x80 | joy Left |
| Right Stick Right | 2 | 0x40 | joy Right |
| Right Stick Trigger | 2 | 0x01 | joy button 1 |
| Right Stick Button | 2 | 0x02 | joy button 2 |
| Service | 0 | 0x08 | F1 |
| Test | 0 | 0x04 | F2 |

### _bel_Inputs (1 games)

Games: Behind Enemy Lines

| Input | Port | Mask | Key |
|---|---|---|---|
| Player 1 Up | 1 | 0x20 | Up |
| Player 1 Down | 1 | 0x10 | Down |
| Player 1 Left | 1 | 0x80 | Left |
| Player 1 Right | 1 | 0x40 | Right |
| Player 1 Trigger | 1 | 0x01 | Z |
| Player 1 Missile | 1 | 0x02 | X |
| Player 1 Button 3 | 1 | 0x04 | C |
| Player 1 Button 4 | 1 | 0x08 | V |
| Player 1 Start | 0 | 0x10 | 1 |
| Player 1 Coin | 0 | 0x01 | 5 |
| Player 2 Up | 2 | 0x20 | joy Up |
| Player 2 Down | 2 | 0x10 | joy Down |
| Player 2 Left | 2 | 0x80 | joy Left |
| Player 2 Right | 2 | 0x40 | joy Right |
| Player 2 Trigger | 2 | 0x01 | joy button 1 |
| Player 2 Missile | 2 | 0x02 | joy button 2 |
| Player 2 Button 3 | 2 | 0x04 | joy button 3 |
| Player 2 Button 4 | 2 | 0x08 | joy button 4 |
| Player 2 Start | 0 | 0x20 | 2 |
| Player 2 Coin | 0 | 0x02 | 6 |
| Service | 0 | 0x04 | F1 |
| Test | 0 | 0x08 | F2 |

### _stcc_Inputs (9 games)

Games: Indianapolis 500 (Rev A, Deluxe), Indianapolis 500 (Rev A, Twin, Newer rev), Indianapolis 500 (Rev A, Twin, Older rev), Over Rev, Over Rev (Model 2B), Sega Touring Car Championship, Sega Touring Car Championship (Rev A), Sega Touring Car Championship (Rev B), Super GT 24h

| Input | Port | Mask | Key |
|---|---|---|---|
| Up | 2 | 0x20 | Up |
| Down | 2 | 0x10 | Down |
| Left | 2 | 0x80 | Left |
| Right | 2 | 0x40 | Right |
| Steering | 0 | 0x08 | joy axis 0 |
| Accelerate | 1 | 0x08 | joy axis 1 |
| Brake | 2 | 0x08 | joy axis 3 |
| Button 1 | 1 | 0x01 | Z |
| Button 2 | 1 | 0x20 | X |
| Button 3 | 1 | 0x10 | C |
| Button 4 | 1 | 0x02 | V |
| Start | 0 | 0x40 | 1 |
| Coin | 0 | 0x01 | 5 |
| Service | 0 | 0x08 | F1 |
| Test | 0 | 0x04 | F2 |

### _tops_Inputs (3 games)

Games: Top Skater, Top Skater (Jap), Top Skater (USA)

| Input | Port | Mask | Key |
|---|---|---|---|
| Up | 2 | 0x20 | Up |
| Down | 2 | 0x10 | Down |
| Left | 2 | 0x80 | Left |
| Right | 2 | 0x40 | Right |
| Curving | 0 | 0x08 | joy axis 0 |
| Slide | 1 | 0x08 | joy axis 1 |
| Jump Front | 0 | 0x20 | Z |
| Jump Tail | 1 | 0x01 | X |
| Select Left | 0 | 0x80 | C |
| Select Right | 0 | 0x10 | V |
| Start | 0 | 0x40 | 1 |
| Coin | 0 | 0x01 | 5 |
| Service | 0 | 0x08 | F1 |
| Test | 0 | 0x04 | F2 |

### _skig_Inputs (1 games)

Games: Sega Ski Super G

| Input | Port | Mask | Key |
|---|---|---|---|
| Swing Left | 2 | 0x80 | Up |
| Swing Right | 2 | 0x40 | Down |
| Incl Left | 2 | 0x10 | Left |
| Incl Right | 2 | 0x20 | Right |
| Inclining | 0 | 0x08 | joy axis 0 |
| Swing | 1 | 0x08 | joy axis 1 |
| Zoom Out | 1 | 0x01 | Z |
| Zoom In | 0 | 0x20 | X |
| Select 1 | 0 | 0x40 | 1 |
| Select 2 | 0 | 0x80 | 2 |
| Select 3 | 0 | 0x10 | 3 |
| Coin | 0 | 0x01 | 5 |
| Service | 0 | 0x08 | F1 |
| Test | 0 | 0x04 | F2 |

### _wave_Inputs (1 games)

Games: Wave Runner

| Input | Port | Mask | Key |
|---|---|---|---|
| Roll Left | 2 | 0x20 | Q |
| Roll Right | 2 | 0x10 | W |
| Handle Left | 2 | 0x40 | Left |
| Handle Right | 2 | 0x80 | Right |
| Throttle | 2 | 0x01 | Up |
| Handle | 0 | 0x08 | joy axis 0 |
| Roll | 1 | 0x08 | joy axis 1 |
| Throttle | 2 | 0x08 | joy axis 2 |
| View | 1 | 0x01 | Z |
| Start | 0 | 0x40 | 1 |
| Coin | 0 | 0x01 | 5 |
| Service | 0 | 0x08 | F1 |
| Test | 0 | 0x04 | F2 |

### _wski_Inputs (1 games)

Games: Sega Water Ski

| Input | Port | Mask | Key |
|---|---|---|---|
| Select Up | 1 | 0x02 | Up |
| Select Down | 0 | 0x40 | Down |
| Slide Left | 2 | 0x80 | Left |
| Slide Right | 2 | 0x40 | Right |
| Slide | 0 | 0x80 | joy axis 0 |
| Set | 1 | 0x01 | Z |
| Start | 0 | 0x40 | 1 |
| Coin | 0 | 0x01 | 5 |
| Service | 0 | 0x08 | F1 |
| Test | 0 | 0x04 | F2 |

### _dtank_Inputs (1 games)

Games: Desert Tank

| Input | Port | Mask | Key |
|---|---|---|---|
| Steer Left | 2 | 0x80 | Left |
| Steer Right | 2 | 0x40 | Right |
| Turret Up | 2 | 0x10 | Q |
| Turret Down | 2 | 0x20 | A |
| Accel + | 2 | 0x08 | Up |
| Accel - | 2 | 0x04 | Down |
| Steering | 0 | 0x02 | joy axis 0 |
| Turret | 1 | 0x02 | joy axis 1 |
| Accelerator | 2 | 0x02 | joy axis 2 |
| Machine Gun | 6 | 0x10 | E |
| Cannon | 6 | 0x20 | R |
| VR1 (Blue) | 5 | 0x20 | 2 |
| VR2 (Green) | 5 | 0x40 | 3 |
| VR3 (Red) | 5 | 0x80 | 4 |
| Start | 5 | 0x10 | 1 |
| Coin | 5 | 0x01 | 5 |
| Coin 2 | 5 | 0x02 | 6 |
| Service | 5 | 0x08 | F1 |
| Test | 5 | 0x04 | F2 |

### _daytona_Inputs (8 games)

Games: Daytona USA, Daytona USA '93 Edition, Daytona USA (Saturn Ads), Daytona USA Special Edition, Daytona USA Turbo, Daytona USA Turbo (Rev A), Daytona USA: GTX2004, Daytona USA: To The Maxx

| Input | Port | Mask | Key |
|---|---|---|---|
| Up | 2 | 0x20 | Up |
| Down | 2 | 0x10 | Down |
| Left | 2 | 0x80 | Left |
| Right | 2 | 0x40 | Right |
| Steering | 0 | 0x08 | joy axis 0 |
| Accelerate | 1 | 0x08 | joy axis 1 |
| Brake | 2 | 0x08 | joy axis 3 |
| Shift 1 | 4 | 0x01 | A |
| Shift 2 | 4 | 0x02 | S |
| Shift 3 | 4 | 0x04 | D |
| Shift 4 | 4 | 0x08 | F |
| Shift Neutral | 4 | 0x10 | G |
| VR 1 | 5 | 0x20 | Z |
| VR 2 | 5 | 0x40 | X |
| VR 3 | 5 | 0x80 | C |
| VR 4 | 6 | 0x01 | V |
| Start | 5 | 0x10 | 1 |
| Coin 1 | 5 | 0x01 | 5 |
| Coin 2 | 5 | 0x02 | 6 |
| Service | 5 | 0x08 | F1 |
| Test | 5 | 0x04 | F2 |

### _daytonat_Inputs (0 games)

Games: none

| Input | Port | Mask | Key |
|---|---|---|---|
| Up | 2 | 0x20 | Up |
| Down | 2 | 0x10 | Down |
| Left | 2 | 0x80 | Left |
| Right | 2 | 0x40 | Right |
| Steering | 0 | 0x08 | joy axis 0 |
| Accelerate | 1 | 0x08 | joy axis 1 |
| Brake | 2 | 0x08 | joy axis 3 |
| Shift 1 | 4 | 0x01 | A |
| Shift 2 | 4 | 0x02 | S |
| Shift 3 | 4 | 0x04 | D |
| Shift 4 | 4 | 0x08 | F |
| VR 1 | 5 | 0x20 | Z |
| VR 2 | 5 | 0x40 | X |
| VR 3 | 5 | 0x80 | C |
| VR 4 | 6 | 0x01 | V |
| Test 2 | 6 | 0x08 | P |
| Start | 5 | 0x10 | 1 |
| Coin 1 | 5 | 0x01 | 5 |
| Coin 2 | 5 | 0x02 | 6 |
| Service | 5 | 0x08 | F1 |
| Test | 5 | 0x04 | F2 |

### _skytargt_Inputs (1 games)

Games: Sky Target

| Input | Port | Mask | Key |
|---|---|---|---|
| Up | 2 | 0x20 | Up |
| Down | 2 | 0x10 | Down |
| Left | 2 | 0x80 | Left |
| Right | 2 | 0x40 | Right |
| Horizontal | 0 | 0x08 | joy axis 0 |
| Vertical | 1 | 0x08 | joy axis 1 |
| Machine gun | 1 | 0x10 | Z |
| Missile | 1 | 0x20 | X |
| View Change | 0 | 0x20 | C |
| Start | 0 | 0x40 | 1 |
| Coin 1 | 0 | 0x01 | 5 |
| Coin 2 | 0 | 0x02 | 6 |
| Service | 0 | 0x08 | F1 |
| Test | 0 | 0x04 | F2 |

### _manxtt_Inputs (2 games)

Games: ManxTT Superbike (Rev C, Twin), ManxTT Superbike (Rev D, Deluxe)

| Input | Port | Mask | Key |
|---|---|---|---|
| Up | 2 | 0x20 | Up |
| Down | 2 | 0x10 | Down |
| Left | 2 | 0x80 | Left |
| Right | 2 | 0x40 | Right |
| Steering | 0 | 0x40 | joy axis 0 |
| Accelerate | 1 | 0x40 | joy axis 1 |
| Brake | 2 | 0x40 | joy axis 3 |
| Shift Up | 1 | 0x10 | A |
| Shift Down | 1 | 0x20 | S |
| Start | 0 | 0x40 | 1 |
| Coin 1 | 0 | 0x01 | 5 |
| Coin 2 | 0 | 0x02 | 6 |
| Service | 0 | 0x08 | F1 |
| Test | 0 | 0x04 | F2 |

### _srally_Inputs (3 games)

Games: Sega Rally Championship, Sega Rally Championship (Rev B), Sega Rally Pro Drivin'

| Input | Port | Mask | Key |
|---|---|---|---|
| Up | 2 | 0x20 | Up |
| Down | 2 | 0x10 | Down |
| Left | 2 | 0x80 | Left |
| Right | 2 | 0x40 | Right |
| Steering | 0 | 0x40 | joy axis 0 |
| Accelerate | 1 | 0x40 | joy axis 1 |
| Brake | 2 | 0x40 | joy axis 3 |
| Shift 1 | 1 | 0x01 | A |
| Shift 2 | 1 | 0x02 | S |
| Shift 3 | 1 | 0x04 | D |
| Shift 4 | 1 | 0x08 | F |
| Shift Neutral | 1 | 0x10 | G |
| VR | 0 | 0x20 | Z |
| Start | 0 | 0x40 | 1 |
| Coin 1 | 0 | 0x01 | 5 |
| Coin 2 | 0 | 0x02 | 6 |
| Service | 0 | 0x08 | F1 |
| Test | 0 | 0x04 | F2 |

### _vf2_Inputs (0 games)

Games: none

| Input | Port | Mask | Key |
|---|---|---|---|
| Player 1 Up | 6 | 0x20 | Up |
| Player 1 Down | 6 | 0x10 | Down |
| Player 1 Left | 6 | 0x80 | Left |
| Player 1 Right | 6 | 0x40 | Right |
| Player 1 Button 1 | 6 | 0x01 | Z |
| Player 1 Button 2 | 6 | 0x02 | X |
| Player 1 Button 3 | 6 | 0x04 | C |
| Player 1 Button 4 | 6 | 0x08 | V |
| Player 1 Start | 5 | 0x10 | 1 |
| Player 1 Coin | 5 | 0x01 | 5 |
| Player 2 Up | 2 | 0x20 | joy Up |
| Player 2 Down | 2 | 0x10 | joy Down |
| Player 2 Left | 2 | 0x80 | joy Left |
| Player 2 Right | 2 | 0x40 | joy Right |
| Player 2 Button 1 | 2 | 0x01 | joy button 1 |
| Player 2 Button 2 | 2 | 0x02 | joy button 2 |
| Player 2 Button 3 | 2 | 0x04 | joy button 3 |
| Player 2 Button 4 | 2 | 0x08 | joy button 4 |
| Player 2 Start | 5 | 0x20 | 2 |
| Player 2 Coin | 5 | 0x02 | 6 |
| Service | 5 | 0x08 | F1 |
| Test | 5 | 0x04 | F2 |

### _motoraid_Inputs (1 games)

Games: Motor Raid

| Input | Port | Mask | Key |
|---|---|---|---|
| Up | 2 | 0x20 | Up |
| Down | 2 | 0x10 | Down |
| Left | 2 | 0x80 | Left |
| Right | 2 | 0x40 | Right |
| Bank | 0 | 0x08 | joy axis 0 |
| Accelerate | 1 | 0x08 | joy axis 1 |
| Brake | 2 | 0x08 | joy axis 3 |
| Kick | 1 | 0x10 | Z |
| Punch | 1 | 0x20 | X |
| Player 1 Start | 0 | 0x40 | 1 |
| Coin 1 | 0 | 0x01 | 5 |
| Coin 2 | 0 | 0x02 | 6 |
| Service | 0 | 0x08 | F1 |
| Test | 0 | 0x04 | F2 |

### _rchase2_Inputs (1 games)

Games: Rail Chase 2

| Input | Port | Mask | Key |
|---|---|---|---|
| Player 1 Up | 3 | 0x80 | Up |
| Player 1 Down | 3 | 0x40 | Down |
| Player 1 Left | 3 | 0x20 | Left |
| Player 1 Right | 3 | 0x10 | Right |
| Player 1 Button 1 | 1 | 0x01 | Z |
| Player 1 Button 2 | 1 | 0x02 | X |
| Player 1 Button 3 | 1 | 0x04 | C |
| Player 1 Button 4 | 1 | 0x08 | V |
| Player 1 Start | 0 | 0x10 | 1 |
| Player 1 Coin | 0 | 0x01 | 5 |
| Player 2 Up | 2 | 0x20 | joy Up |
| Player 2 Down | 2 | 0x10 | joy Down |
| Player 2 Left | 2 | 0x80 | joy Left |
| Player 2 Right | 2 | 0x40 | joy Right |
| Player 2 Button 1 | 2 | 0x01 | joy button 1 |
| Player 2 Button 2 | 2 | 0x02 | joy button 2 |
| Player 2 Button 3 | 2 | 0x04 | joy button 3 |
| Player 2 Button 4 | 2 | 0x08 | joy button 4 |
| Player 2 Start | 0 | 0x20 | 2 |
| Player 2 Coin | 0 | 0x02 | 6 |
| Service | 0 | 0x08 | F1 |
| Test | 0 | 0x04 | F2 |

### _vcop2_Inputs (0 games)

Games: none

| Input | Port | Mask | Key |
|---|---|---|---|
| Player 1 Start | 5 | 0x10 | 1 |
| Player 1 Coin | 0 | 0x01 | 5 |
| Player 2 Start | 5 | 0x20 | 2 |
| Player 2 Coin | 0 | 0x02 | 6 |
| Service | 0 | 0x08 | F1 |
| Test | 0 | 0x04 | F2 |

### _dynabb_Inputs (1 games)

Games: Dynamite Baseball 97 (Rev A)

| Input | Port | Mask | Key |
|---|---|---|---|
| Player 1 Up | 1 | 0x20 | Up |
| Player 1 Down | 1 | 0x10 | Down |
| Player 1 Left | 1 | 0x80 | Left |
| Player 1 Right | 1 | 0x40 | Right |
| Player 1 Bat | 0 | 0x40 | joy axis 0 |
| Player 1 Button 1 | 1 | 0x01 | Z |
| Player 1 Button 2 | 1 | 0x02 | X |
| Player 1 Bat + | 1 | 0x04 | C |
| Player 1 Bat - | 1 | 0x08 | V |
| Player 1 Start | 0 | 0x10 | 1 |
| Player 1 Coin | 0 | 0x01 | 5 |
| Player 2 Up | 2 | 0x20 | joy Up |
| Player 2 Down | 2 | 0x10 | joy Down |
| Player 2 Left | 2 | 0x80 | joy Left |
| Player 2 Right | 2 | 0x40 | joy Right |
| Player 2 Bat | 1 | 0x40 | joy axis 0 |
| Player 2 Button 1 | 2 | 0x01 | joy button 1 |
| Player 2 Button 2 | 2 | 0x02 | joy button 2 |
| Player 2 Button 3 | 2 | 0x04 | joy button 3 |
| Player 2 Button 4 | 2 | 0x08 | joy button 4 |
| Player 2 Start | 0 | 0x20 | 2 |
| Player 2 Coin | 0 | 0x02 | 6 |
| Service | 0 | 0x08 | F1 |
| Test | 0 | 0x04 | F2 |
