# CLAUDE.md

Seeds a fresh Claude session with the hard-won facts that took trial-and-error to discover — things that cannot be re-derived from the i960 manual or general emulator-design knowledge.

Pair with [PROPOSAL.md](PROPOSAL.md) §8 for the full subsystem gotcha catalogue and §3 for the board-vs-game layering. This file is for the *invariants that must never be re-derived*; the proposal is for the broader context.

---

## Project Overview

A general **Sega Model 2 arcade emulator**, written in C11 with Dear ImGui (via cimgui) and Sokol for cross-platform graphics. The first target is *Sonic The Fighters* (STF), because the bulk of the prior reverse-engineering work happened there — but the architecture is built for the **full Model 2 catalogue** from day one. Generalising across games strengthens every subsystem: most "STF bugs" turn out to be board-level i960 / COP / tile bugs that affect every Model 2 game equally.

**Reference prior project** — the original STF-only implementation lives at `c:\Users\bigge\source\repos\stf-hle\` (the directory containing this file). It is **not deleted**: treat it as a working reference for register-window logic, COP math, the polygon decoder, HLE hook patterns, and the memory-region table. Read freely from it; do not import code wholesale — the new project's layering (board vs game profile, §3 below) means files will need restructuring as they're brought over.

See [PROPOSAL.md](PROPOSAL.md) for the architecture, module map, build commands, and bootstrap checklist.

---

## Board vs. Game Layering (critical)

Code lives in one of two layers. Get this distinction wrong and you'll re-implement board-level fixes in per-game files.

- **Board layer** — anything shared by every Model 2 ROM set: i960 CPU core, memory bus, COP math, tile renderer, 3D polygon decoder, sound block, threading. Lives in shared `.h` modules.
- **Game-profile layer** — anything specific to one ROM set: HLE hook table (addresses), input map, ROM file list + CRC32s, optional quirks struct. Lives in a `game_profile_t` entry resolved from the loaded ROM CRC32s.

**Default to the board layer.** If a bug surfaces in STF, your first hypothesis should be "this is board-level and another game is also affected" — not "this is STF-specific." Only move a fix to the per-game layer when you have positive evidence (e.g. another game's ROM relies on the opposite behaviour).

---

## Implementation Invariants (do not re-derive)

These are facts reverse-engineered or debugged into the original implementation. Not in any datasheet. Treat as load-bearing.

### i960 CPU (board-level — every Model 2 game)

- **`chkbit` sets `CC_NO` (0x0) when the tested bit is 0**, not `CC_NE` (0x5). `bno` depends on this; getting it wrong inverts all `chkbit`+`bno` / `chkbit`+`bo` branch logic.
  - *Symptom that surfaced this in STF:* Espio's tongue drew every frame in the Rocket Metal attract cutscene because `bno rd_te_pass` never branched. Since this is i960-core behaviour, the same bug would surface in any Model 2 game that uses `chkbit`+`bno` — which is most of them.
- **`cmpobX` / `cmpibX` always update the condition code**, even when the branch is not taken. Downstream `bg` / `bl` / `be` read those CCs.
- **MEM mode 0x5 is IP-relative**: `effective = IP + 8 + disp`, where `+8` is relative to the *start* of the 2-word MEM instruction, not the next.
- **FP-from-GPR is bit-reinterpret, not int→float convert** — `memcpy` semantics.
- **`call` / `ret` frame layout**: align SP to 64 bytes (`(sp + 63) & ~63`), zero the new locals, save `pfp` / `sp` / `rip`, sync `g15` (frame pointer) every call. `ret` restores all locals.
- **Register-pair (`reg_quad`) ops are big-endian** even though the CPU is little-endian overall.

### Coprocessor (COP) — board-level math, every Model 2 game

- **Rotation is accumulated by post-multiply**, not rebuilt from stored angles. `g_sharc.rot[3][3]` is column-major (SHARC convention): `rot[col][row]`. Each ang command post-multiplies the running matrix:
  - `ang_y` (0x04800909 → PM 0x201BF): `new_col0 = c·col0 + s·col2`, `new_col2 = −s·col0 + c·col2`
  - `ang_x` (0x04000808 → PM 0x201AA): `new_col1 = c·col1 − s·col2`, `new_col2 = s·col1 + c·col2`
  - `ang_z` (0x05000A0A → PM 0x201D4): `new_col0 = c·col0 − s·col1`, `new_col1 = s·col0 + c·col1`
  - Verified by reading SHARC firmware dispatch table at DM[0x30000] in `C:\temp\sharc_bone.asm`.
  - **Previous versions of this doc had ang_x and ang_z PM addresses and formulas swapped — now corrected.**
  - The old "M = Ry_LH × Rx × Rz rebuild" was only correct for a single clean ang sequence from identity.
- **Z-negation in matrix storage** (HLE convention): `matrix[r][2] = −rot[2][r]` for rows 0 and 1; `matrix[2][2] = rot[2][2]`. The 0x14802929/0x35006A6A handlers use `−iz` for rows 0/1 and `+iz` for row 2. This asymmetric negation together with the negated col2 storage produces output identical to the SHARC's raw column-major multiply.
- **Angles are 16-bit signed fixed-point**, `0x10000 = 360°`. Only the low 16 bits are meaningful.
- **`0x07800F0F` returns world translation T[0..2], NOT rotation entries**: both STF (PM 0x02043D) and FV (PM 0x020402) use `DM(I7, 0x09)` (hex `0x00006E7E48000000`) — a post-modify-by-9 instruction whose immediate is encoded in bits[31:27] of the lower instruction word. This advances I7 from slot[0] to slot[9]=T[0]; the LCNTR=3 loop then outputs T[0], T[1], T[2]. The i960 stores these to `g7+0x1F4` for collision/IK. The STF annotation correctly named this `read_world_pos`. Previous CLAUDE.md entry was wrong ("slot[1..3]=rotation").
- **`0x06800D0D` zeros T[0..2] (world translation), NOT rotation entries**: same `DM(I7, 0x09)` post-modify-by-9 positions I7 at slot[9]; three zero-writes hit T[0..2]. Both STF (PM 0x02042A) and FV (PM 0x0203EF) are identical. Previous CLAUDE.md entry was wrong ("slot[1..3]=rotation").
- **`0x07000E0E` is a no-op in the HLE**: firmware PM 0x020433 writes 3 FIFO args to bone slot rotation entries `slot[1..3]`, but in STF the args are i960 addresses and the subsequent identity reset and ang commands overwrite those entries. The HLE ignores the command's args.
- **`0x1A803535` mirrors to `tgp_bone`**: the geo3d scanner reads `tgp_bone[player*16+slot]` when it encounters `0x1B803737` in the capture stream. For attract-mode characters (which use `calc_unit_mat` + `0x1A803535`, not the IK chain `0x35806B6B`), `tgp_bone` must be kept in sync with `rot_cache` by copying on every `0x1A803535` save.
- **Command `0x2F005E5E` is scalar-then-vector**: arg0 = scalar, args 1–3 = vector → returns `(s*x, s*y, s*z)`.
- *Note:* command opcodes documented here are the ones confirmed in STF. Other games may use additional opcodes — log unknown commands at WARN and extend the dispatch table.

### 3D Polygon Decoder (board-level — confirmed against two games)

Index-array decoder. Confirmed **J=1.0 on 4402/4405 STF models** AND originally reverse-engineered from Daytona at **J=1.0 on 2377 Daytona models**. The cross-game validation is what makes this load-bearing: the algorithm is the Model 2 board's polygon format, not an STF quirk.

STF reference dataset: `C:\m2\3d\new\stf-poly` — 4405 OBJ files, 5-digit zero-padded filenames (e.g. `00001.obj`). Treat as ground truth for STF.

**iFlag = `vp[25] & 0x03`:**

| iFlag | Meaning |
|-------|---------|
| 0 | Sentinel — previous group ended; start fresh strip |
| 1 | Carry far edge of previous face (`Index[-4]`, `Index[-2]`) |
| 2 | Plain new quad group |
| 3 | Anchor new strip off previous corner (`f1==1` → `Index[-1]`, else `Index[-2]`; `anchor_b = Index[-3]`) |

**Face loop:** `i < n_idx - 8`, always 2 groups behind tail.
**Face type:** `f1 == 2` → triangle; otherwise quad with **A-B-D-C** winding.
**Vertex convention:** `(x, y, -z)` — Z is negated on read.

**Model-table mesh pointer is encoded, not a raw offset**: actual ROM offset = `(ptr * 4) - 0x02000010 + 0x10` for STF. The `* 4` is hardware; the base may shift per game — verify before trusting on a new ROMset.

**Connectivity bruteforce:** simple rule `(vp[25] & 3) != 2` gives J≈0.708; the empirically correct `connect_when` mask for STF is **`0x45B4`**, found by brute force against the STF reference OBJs. Do NOT replace with a fan-mode or bitmask heuristic (that approach peaked at J≈0.71 and was abandoned). For a new game, re-run the bruteforce against that game's reference renderings before assuming the same mask.

### Memory Bus (board-level)

- **Region table is linear-scanned in declaration order.** TILE (`0x01000000`) MUST appear before H_SYNC (`0x01040000`) or H_SYNC reads route to the TILE handler.
- **IO region initializes to `0xFF`, not `0x00`** (hardware idle state).
- **`GEO_CAPTURE_SIZE` ≥ 32768.** Smaller sizes wrap mid-frame and produce partial 3D snapshots / flicker.

### Tile Renderer (board-level)

- **16-bit byteswap on pixel bytes**: indices `[0,1,2,3]` are read as `[1,0,3,2]` (XOR low bit of byte index). Within each swapped word, high nibble = left pixel, low nibble = right.
- **Tilemap entry (7-bit fields)**: bit15=priority, bit14=h_flip, bits[13:7]=pal_bank (7-bit, 0–127), bits[6:0]=char (7-bit). Full tile index = `entry & 0x3FFF` (= `(pal_bank<<7)|char`). Palette LUT index = `pal_bank * 16 + color_idx` (stride=16 entries = 32 bytes per bank). Verified: CG87 palette written to pal+0x660 = bank 51×32; tile entry pal_bank=(0x9980>>7)&0x7F=51; pal+51×32=0x660 ✓.
- **Color index 0 is transparent on foreground layers only**; background layers fully opaque (pass `NULL` for `alpha_out`).

### HLE Hooks (game-specific addresses, board-level patterns)

These addresses are STF-specific. The **patterns** repeat across the catalogue — every Model 2 game ships some form of COP self-test and frame-loop entry; the addresses change per ROMset.

- **`CoProcessorErr` at `0x74E4` (STF)** must be bypassed. Return via `locals.rip` (saved return address from the call frame), NOT normal IP advance. Without this, STF hangs at the Sega logo. Every Model 2 game will have an equivalent — find by symptom (hang on boot, COP self-test loop).
- **Timer IRQ flag at `0x50008C` (STF)** — write `0x01` to unblock the polled wait loop.
- **Frame pacing** is driven by `variable_diff_calc` (~`0x11A04` in STF) setting a volatile `g_frame_done`. The emu thread runs up to `EMU_STEPS_PER_SLICE` (500,000) instructions per slice — sized to always reach the frame boundary.

### Threading

- **Unlock the emu mutex BEFORE sleeping.** Sleeping inside the critical section freezes the UI.
- **Double-buffered CPU snapshot** (`cpu_snapshot` + `cpu_prev_snapshot`); UI always reads the current snapshot.
- **Sleep granularity**: Windows `Sleep()` ≈ 1 ms; POSIX `usleep()` ≈ 1 µs.

---

## STF Disassembly Reference

Authoritative IDA disassembly: `C:\m2\ida72\asm-check\`.

| File | Description |
|------|-------------|
| `stf_prog.asm` | Full ~477K-line IDA disassembly — primary reference for STF ROM addresses, struct layouts, function names |
| `decomp/` | ~1057 per-function `.S` files named after game subsystems |
| `process-win.bat` | ROM rebuild chain: `m2asm.py` → `gcc960` → `objcopy` (intel960) → `stfbin2rom.py` → `epr-19001.15` / `epr-19002.16` |

**ROM identity:** CRC32 `72E66A1D`, MD5 `2A3E32834FC727391C0AFCB18121245E`.
**`stfbin2rom.py --ctools`** strips the 44-byte (`0x2C`) b.out header `gcc960` prepends.

For other Model 2 games, see **MAME** (`src/mame/sega/model2.cpp`) as a cross-reference for memory map, COP opcodes, and ROM region layouts. Don't copy code; do cross-check addresses.

---

## Build

CMake ≥ 3.20 is not on `PATH` on the dev machine; use the VS-bundled binary:

```
C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
```

```
cmake -S <repo> -B <repo>/build
cmake --build <repo>/build --config Release --target ALL_BUILD -j 16 --
```

Output: `build\Release\demo.exe`. No automated tests — validation is interactive through the GUI. The active game profile is resolved by matching ROM CRC32s; STF (sfight + schamp) loads by default if present in the working directory.

---

## Conventions

- All modules except `demo.c` and `miniz.c` are **header-only `.h` files**. This is intentional — do not split into `.c`/`.h` pairs.
- Default new code to the **board layer**; only move to a `game_profile_t` quirk when there's positive evidence of game-specific behaviour.
- Memory addresses and sizes use `uint32_t`. Sign-extension is handled per-instruction.
- Platform threading is abstracted in `emu_thread.h`: `emu_lock()` / `emu_unlock()` wrap `CRITICAL_SECTION` on Windows and `pthread_mutex_t` on POSIX.
- Logging: `log_msg(severity, fmt, ...)` from `log.h` — `LOG_INFO`, `LOG_WARN`, `LOG_ERROR`. **Log unknown COP commands and unhandled MMIO at WARN** so new-game support work surfaces them automatically.
