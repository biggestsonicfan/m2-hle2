# scsp.h against Mednafen's SCSP

`scsp_vs_mednafen` feeds the same register traffic into `src/board/scsp.h` and into
Mednafen's Saturn SCSP (`src/ss/scsp.inc`, the same YMF292), from the same sound RAM, and
compares them sample by sample.

scsp.h follows MAME, and `snd_replay` grades the board against MAME, so where MAME's chip is
wrong the two agree and nothing notices. Mednafen's chip was written separately, from Saturn
hardware tests. Where the two differ is where scsp.h is worth checking against a real board.
This tool finds those places. It does not decide which side is right.

## Build and run

```sh
tools/scsp_mednafen/build.sh               # fetches Mednafen 1.32.1 (sha256-checked), builds with gcc/g++
B=tools/scsp_mednafen/build/scsp_vs_mednafen
$B [seeds=40] [samples=8192] [probe]       # the table below
$B --show <probe> <seed> [samples] [wav]   # one draw: first differing samples and reads, and a WAV
$B --timers                                # when each timer fires, and how often, on each side
$B --bench [samples=441000]                # each chip alone, the same load: ns per sample
```

- **Build on Linux, WSL, MSYS2 or macOS.** The shim uses GCC builtins. The main CMake build doesn't include this tool, so an ordinary build never touches Mednafen.
- **What `--show` writes:** a WAV with scsp.h's left channel on the left and Mednafen's on the right.

**Licence.** Mednafen is GPL-2.0-or-later. None of it is in this repository.
- `fetch.sh` downloads the release tarball into `mednafen/`, which is git-ignored.
- `build.sh` compiles it into a local binary. Keep that binary local; don't distribute it.
- `mdfn_scsp.cpp` is this repository's own code: the handful of Mednafen helpers that `scsp.inc` needs, written from what they do, plus a C interface.

## Probes

Each probe changes one thing from a plain voice, then runs from N random draws. The plain voice is:
16-bit, normal loop, attack 31, no decay, TL 0, centred, no LFO or FM.

| probe | what varies |
|---|---|
| plain, pitch, pcm8, sbctl, noise, loop-modes | the sample path |
| attack, key-scaling, decay, release, loop-link, envelope | the EG, one field at a time, then all together |
| level-pan, pitch-lfo, amp-lfo, fm | levels, both LFOs, FM between two slots |
| many-voices | 8 slots, all of the above |
| dsp | STF's own reverb program (`scsp_dsp_known[0]`) with random COEF and MADRS, fed by ISEL/IMXL and mixed by EFSDL |
| dsp-random | random programs; most are silent |
| timers-irq | the three timers and their interrupt levels. SCIPD is compared without bits 9 and 10 (see below) |
| monitor | the slot monitor (0x408) on a playing slot |
| midi | the MIDI input buffer and its status word (0x404) |
| fuzz | random events of every kind, as `tests/scsp_fuzz.c` generates them |

The table's columns:
- **exact:** draws that matched in every sample, read and interrupt level.
- **1st diff:** the median sample of the first difference.
- **err med:** the median of RMS(difference) / RMS(scsp.h).
- **settled:** the same error, leaving out the first 256 samples (the attack) and anything after key-off.
- **level dB:** Mednafen's level relative to scsp.h.

## First run (2026-09-26, 100 draws of 8192 samples)

```
probe          exact 1st diff  err med  settled  set max level dB   reads bad  irq bad
plain          0/100        0   0.0357   0.0078   0.0078    -0.07           -     0/100
pitch          0/100        0   0.0358   0.0279   1.0656    -0.03           -     0/100
attack         0/100        0   0.3410   0.3299   0.9974   -14.41           -     0/100
key-scaling    0/100        0   0.1204   0.0677   1.1327    -1.18           -     0/100
decay          0/100        0   0.1177   0.0967   1.3021    +0.73           -     0/100
release        0/100        0   0.0331   0.0078   0.0078    -0.04           -     0/100
loop-link      0/100        0   0.9926   0.9797  25.7833   -22.55           -     0/100
envelope       0/100        0   0.1795   0.1847  12.0655    -4.50           -     0/100
level-pan     15/100        0   0.0597   0.0520   1.0537    +0.36           -     0/100
pcm8           0/100        0   0.0362   0.0078   0.0078    -0.07           -     0/100
loop-modes     0/100        0   0.0411   0.0277   1.3679    -0.13           -     0/100
noise          0/100        0   1.3954   1.3965   1.3965    +0.02           -     0/100
sbctl          0/100        0   0.0359   0.0078   0.0078    -0.07           -     0/100
pitch-lfo      0/100        0   1.3215   1.3453   1.4377    -0.26           -     0/100
amp-lfo        0/100        0   0.3064   0.3027  14.8751    +2.76           -     0/100
fm             0/100        0   1.0559   1.0584   1.2570    +0.21           -     0/100
many-voices    0/100        0   0.4593   0.5763   9.0015    -1.06           -     0/100
dsp            0/100        0   0.7958   0.7928   5.1061    -1.43           -     0/100
dsp-random    43/100        0   0.9978   0.9348 2949.5713    -0.11           -     0/100
timers-irq     0/100        -   0.0000   0.0000   0.0000    +0.00 85800/117100    98/100
monitor        0/100        0   0.5600   0.7641 319.2670    -4.13 35526/63100     0/100
midi           0/100        -   0.0000   0.0000   0.0000    +0.00 163900/163900     0/100
fuzz           0/100       56   2.8974   2.9304  16.7924    +9.25 10265/12789     0/100
```

### What the differences are

Each of these was traced to its code on both sides unless marked **measured only**.
"MAME" means scsp.h, which follows MAME on purpose.

1. **Full-scale level: Mednafen is exactly 127/128 of scsp.h** (−0.07 dB).
   - Once the attack is over, the plain, pcm8, sbctl and release probes differ only by this gain. After scaling, the residual is 1.5e-5.
   - **Measured only:** the ratio is exact, but I haven't traced which stage (EG, TL, SDL or MVOL) produces it.
2. **The attack is a different curve.**
   - MAME: the level climbs *linearly* from 0x17F to 0x3FF, over the time in its AR table, and AR 31 is immediate.
   - Mednafen: the attenuation falls *exponentially* from silence (`level += ~level >> shift`), so AR 31 takes about 10 samples.
   - Mednafen: when rate + key scaling reaches 32 or more, the attack never runs at all. Its source says hardware does this ("stuck in EG attack phase").
   - This one difference drives the attack (−14 dB) and loop-link (−23 dB) probes, most of envelope, and the monitor's EG field.
3. **Key rate scaling uses different formulas.**
   - MAME: `2·KRS + OCT + FNS bit 9`, added to `2·rate` (in half steps).
   - Mednafen: `clamp(KRS + OCT, 0, 15)`, added to the 5-bit rate and capped at 31. Octave has twice the effect, FNS has none, and there is a clamp.
4. **Rate 0 with key scaling on:** MAME still moves the envelope. Mednafen never clocks it (`& (bool)ERateNoScale`). scsp.h's own comment already says "hardware: 0 = never".
5. **Timers after they fire.** See `--timers`: 255 − reload samples to the first fire on both sides, give or take one.
   - scsp.h (MAME: "an acknowledged timer that has expired pends again") stops the timer, and every SCIRE sets the bit again until the timer is reloaded. The table's "period 1" is that.
   - Mednafen lets the counter run on: 0xFF wraps to 0x00, and the timer fires again every 256 × 2^prescale samples.
   - Reload 0xFF fires at once in Mednafen. In scsp.h (MAME) it leaves the timer as it was.
   - Mednafen clocks the prescaler off a global sample counter. scsp.h counts from the write, which CLAUDE.md says MAME's measurements need.
   - **The drivers reload inside the handler,** so the difference only shows if a handler is late or skipped.
6. **SCIPD bit 10, the one-sample interrupt:** Mednafen sets it every sample. scsp.h never does. A driver that enables it gets an interrupt only from Mednafen.
7. **MIDI status (0x404) bit 11, output empty:** set in Mednafen, always 0 in scsp.h. The data byte matches.
8. **The slot monitor (0x408)** is timed differently.
   - Mednafen latches CA/SGC/EG for the selected slot as it produces each sample, so a read straight after an MSLC write sees the old data.
   - scsp.h works the value out at the read, then resets MSLC to slot 0 (MAME).
   - The probe re-selects the slot before every read and reads one sample later. What remains is mostly item 2 seen through the EG field.
9. **Measured only, not traced yet:**
   - **Noise:** the same level, but an uncorrelated sequence (a different LFSR or seed).
   - **Both LFOs and FM:** differ enough to be uncorrelated.
   - **STF's reverb through the DSP:** 79% median error. The fly's music runs through this program.
   - **A few pitch settings:** up to 107% error; most pitches match to 3%.
   - **Decay:** about +0.7 dB.
   - **Level and pan:** a small spread.
   - **Slot word 5 bit 15:** Mednafen's EG bypass. The probes keep it clear, and scsp.h doesn't implement it.

### Speed (`--bench`, 2026-09-26)

Each chip was timed alone on the same load: the best of 5 runs of 10 s of audio, built with gcc 13 at `-O2`,
on the dev container's 8 cores (with a stream and a trainer running beside it).

```
load                  scsp.h ns/smp       mednafen    ratio
idle                           22.8         1395.6   61.26x
8 voices                      100.7         1427.8   14.18x
32 voices                     344.7         1598.5    4.64x
32 voices + reverb            507.7         1762.3    3.47x
```

- **With `-O3 -march=native`** the ratios were 84x, 20x, 6.2x and 4.2x.
- **Why Mednafen is slower:** it runs every slot's full pipeline every sample, whether the slot is keyed or not, and interprets the DSP (its dynarec is compiled out here).
- **Why scsp.h is faster:** it skips idle slots, and it runs STF's reverb as compiled C (`scsp_dsp_known.h`).
- **STF holds 5-16 voices,** where scsp.h is roughly 14-20x faster.

So swapping in Mednafen's chip would cost speed, not save it. Where hardware sides with Mednafen, the thing to port is the behaviour, into scsp.h.

**Not changed in scsp.h.** MAME is the declared oracle, and `snd_replay` grades against it.
Any of these would change the board's output against MAME, and a change to the sound board's
interrupt timing is a `NETPLAY_PROTO_REV` bump. Each item is a question to settle on a real
board, or against a hardware-tested reference, before scsp.h moves.
