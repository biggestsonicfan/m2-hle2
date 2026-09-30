# The SCSP: what it does, and how this emulator runs it

The Model 2 sound board is a 68000, 512 KB of sound RAM, the sample ROMs and a
Yamaha YMF292 "SCSP" (the Saturn's sound chip). The 68000 runs the game's own
driver (`epr-19021.31` for STF, a disassembly is in
`schamp_SoundDriver_disasm/`), and the SCSP makes the sound. This page is the
chip as `src/board/scsp.h` models it, what STF's driver uses of it (measured),
and why the emulator runs it the way it does (Pinboard #166, "SCSP HLE").

## What the chip is

Four blocks share sound RAM with the 68000.

**32 slots (voices).** Each slot plays one sample out of sound RAM:

- *Source*: 8- or 16-bit PCM from a start address SA (`SSCTL` 0), or the noise
  generator (1), or silence (2, 3). Sample bits can be inverted (`SBCTL`).
- *Pitch*: an octave and a 10-bit fraction make a 20.12 fixed-point step per
  output sample; the pitch LFO (`PLFOS`/`PLFOWS`) multiplies it. Reads are
  linearly interpolated between the two samples around the position (MAME's
  choice).
- *Loop*: none, forward, reverse or alternating, between LSA and LEA
  (`LPCTL`). A voice with no loop ends at the later of the two; that clears
  its KYONB bit.
- *FM*: a slot can add the average of two entries of the "sound stack" (the
  last 64 slot outputs) to its read position (`MDL`, `MDXSL`, `MDYSL`).
- *Envelope*: attack, decay 1, decay 2, release (`AR`, `D1R`, `D2R`, `RR`,
  key-rate scaling `KRS`, decay level `DL`), then the amplitude LFO and the
  total level `TL`.
- *Out*: three sends from the one sample. The direct mix (`DISDL`, `DIPAN`),
  the DSP's input bus MIXS[`ISEL`] (`IMXL`), and the sound stack.

Key-on is two-step: a slot's KYONB bit says what it wants, and a write with
KYONEX set commits every slot's KYONB at once.

**The DSP.** A 128-step microprogram (MPRO) runs once per sample over 16 MIXS
inputs, two EXTS inputs, 128 TEMP registers and a delay line in sound RAM
(RBP, RBL; 16-bit floats in MAME's format), and writes 16 EFREG outputs.
Slots 0-15's `EFSDL`/`EFPAN` put EFREG n in the mix. It is a programmable
effects processor; games load a reverb.

**The mixer.** Direct sends plus EFREG, clamped (18- or 16-bit DAC), times the
master volume `MVOL`.

**The control block.** Three timers (A, B, C) counting in samples scaled by a
prescaler, an interrupt controller that raises one of seven 68000 levels per
source (`SCILV0-2`, `SCIEB`, `SCIPD`, `SCIRE`), a MIDI input FIFO (the i960's
commands arrive here, off the i8251 serial line) and a MIDI output, the slot
monitor (0x408: `MSLC` selects a slot, the chip latches its play position CA,
envelope state SGC and level EG once a sample), and a DMA engine between sound
RAM and the registers.

## What STF's driver uses of it

Measured over 90 s of attract and 200 s of a scripted one-player game
(slot registers sampled at every sample boundary, 2026-09-30):

| | attract | game |
|---|---|---|
| voices sounding, mean / max | 16.4 / 32 | 18.3 / 32 |
| 8-bit samples | 100% | 100% |
| forward loop / no loop | 76% / 24% | 67% / 33% |
| reverse, alternating loops, FM, noise, SDIR | none | none |
| pitch LFO / amplitude LFO | none | 0.3% / none |
| sending to the DSP (`IMXL` ≠ 0) | none | 0.1% |
| slots 0-15 mixing EFREG back in | 10% | 9% |

- **Every voice streams.** The driver keys a voice with LSA 0 and LEA 0x1FFF
  (key-on at `0x601F0A`: `move.l #$1FFF,4(a5,d4.w)`): an 8 KB window in sound RAM,
  played as a forward loop, whose half the chip is not playing the driver
  refills from the sample ROMs. To know which half that is, it writes the slot
  number to MSLC, waits ten `ror.l`, and tests CA bit 0 in 0x409. That poll is
  most of what the driver does: MSLC is written 0.9 times a sample, the monitor
  read once every five.
- **The reverb is per instrument.** The DSP send byte (slot + 0x15, ISEL/IMXL)
  comes from the instrument definition (`move.b $140D(a6),$15(a5,d4.w)`,
  InsData + 0x0D), and almost no instrument sets it. The DSP still runs its 84
  steps every sample: it is the biggest single cost on the sound board (~36%),
  and in attract its input is silence.
- **Timers are the tempo.** Timer B and C are reloaded inside their interrupt
  handlers (~850 times a second each), timer A ~85 times; the time the handler
  takes before the reload is part of every period (CLAUDE.md, "Timing").
- **The delay line** is 64 KB at 0x70000-0x7FFFF, a page of its own.

## How the emulator runs it: the chip's own time

The chip used to run in strict lockstep: after every 256 clocks of the 68000,
one call made one sample of all 32 slots, the DSP and the mix. That is exact,
and it is the most expensive way to be exact.

What the driver can observe is narrow. It reads the monitor, a slot's KYONB,
the sound stack and the DSP's registers, and it shares sound RAM with the
slots (which read it) and the DSP (which reads and writes it). Everything else
the chip does is the output. So `scsp.h` now makes its samples late
("The chip's own time"):

- The 68000 loop only says a sample boundary passed (`scsp_tick`).
- Each slot keeps its own place in the samples owed, and is run on alone,
  a stretch at a time in a tight loop (`scsp_slot_run_pcm`), when something
  looks at it: its KYONB, the monitor if it is the slot MSLC named at the last
  boundary, or a 68000 write inside the RAM it may read. The driver's refill
  writes run one slot on; its monitor poll runs one slot on; neither makes the
  chip catch up.
- A full sync (`scsp_sync`: every slot up to now, then the DSP and the mix
  sample by sample) happens at the end of each run and before any register
  write that changes what the chip makes. Timer, interrupt, MIDI and MSLC
  writes do not sync.
- Slot contributions to each owed sample are integer sums, so the order the
  slots arrive in does not matter: the output is bit for bit the lockstep
  chip's.
- FM, the noise generator, or a voice reading the DSP's delay line make the
  chip `coupled`, and every boundary is then a full sync: the lockstep chip.

In attract that is one full sync per ~90 samples and 0.7 single-slot catch-ups
per sample. The sound board is **~10% faster** (60 s of attract audio, best of
8 interleaved: 1.52 s → 1.37 s on x86; the voices went from 1.42 G to 0.97 G
instructions), with the same output and RAM, every sample.

This is not the high-level sound engine that was rejected before (CLAUDE.md,
"Do not go back to a key-on event mixer"): nothing is guessed. Every value the
driver can read is made exactly when it reads it.

### Why it is exact, and how that is checked

A slot run alone is only right if nothing it reads changes under it. The RAM a
voice may read before its registers next change is bounded in
`scsp_slot_range`, and everything not provably bounded claims all of RAM. Two
MAME behaviours make that less obvious than it looks: a forward loop shorter
than one pitch step lands past its end again and creeps on through RAM, and
the alternating loop reads a position that stepped below 0 as past LEA and
reflects it far beyond the loop.

- `tests/scsp_lazy_test.c` (ctest) runs random voices and DSP programs twice
  with different garbage everywhere outside the RAM they claim. It found both
  traps above; ten planted range bugs each fail it.
- `scsp_fuzz` holds the chip against the lockstep one under random register
  traffic, with scenarios that leave the chip alone for long stretches.
- `m68k_fuzz` (random code on the whole board) at 1, 37 and 735 samples a run,
  `det_digest --sound` over 100 s of attract and a 12,000-frame scripted game
  (sound thread on and off), and `snd_bench`'s hashes: all identical to master.
  The fuzz also matches between GCC and emcc (wasm).

### What is left

The DSP is now the largest single cost, and in STF it mostly reverberates
silence. An exact shortcut would have to prove the delay line and TEMP are at a
fixed point under zero input before skipping a step; that is the next lever,
not done here.
