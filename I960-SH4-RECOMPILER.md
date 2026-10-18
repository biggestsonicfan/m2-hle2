# The real cost of an i960 to SH-4 recompiler (Pinboard #491)

What goes into a recompiler, why the i960 and the SH-4 are an awkward pair for
one, and what the Dreamcast port has already learned by trying. The figures
come from [DREAMCAST-PORT.md](DREAMCAST-PORT.md) (#370, #386, #392, #394, #456,
#459) and [GEMS-COLLECTION.md](GEMS-COLLECTION.md). Everything there was
measured in Flycast, not on a Dreamcast.

## The short answer

- **A real dynamic recompiler is several thousand lines and months of work**,
  most of it not the translation. The translation is a few hundred lines: the
  #386 prototype, `src/core/i960_jit_sh4.h`, is 519 and correct. The rest is
  keeping guest registers in host ones across blocks, linking blocks to each
  other, a code cache that fits in 16 MB, the board's timing kept exact at
  every exit, and tests to prove it.
- **The two CPUs are a bad match.** The guest has more registers than the
  host. Its condition code has three bits where the SH-4 has one. The SH-4
  has small immediates, short branches, no integer divide, no
  count-leading-zeros, a single-precision FPU, and faults on unaligned
  access. On top of that, the i960 saves 16 registers on every `call`.
- **STF does not give a recompiler much to win.** Its straight runs of code
  average 1.9 instructions. #386's compiled code was 3x faster on pure
  register work and slower on the real game (61 ms a slice against 44).
- **The static route already took most of it.** The AOT (#394, #459) compiles
  the program ROM to C on the PC. Gems' C (#456) runs the hottest call trees
  natively. The i960 slice is ~15 ms of a ~36 ms frame now, and most of what
  is left is Gems C, hooks and the COP, not interpretation. A perfect
  recompiler would save a few ms a frame. The drawing side has more to give.

## What a recompiler is made of

An interpreter fetches, decodes and runs one guest instruction at a time and
pays for the decode and the dispatch every time. A recompiler does the decode
once and writes host code that does the same work. These are its parts, and
what each one has cost or would cost here.

1. **Front end: decode and block discovery.** Find where a run of guest code
   starts and ends: a branch, a call, an instruction that talks to the board,
   a hook's address. The interpreter's decoder and `i960_blocks.h` already do
   this. *Cost here: done.*
2. **Translation.** For each guest instruction, the host instructions that do
   the same thing, with the same edge cases: shift counts of 32 and over,
   divide by zero, NaN compares, overflow. The prototype covers the plain
   ALU, loads, stores, compares and branches, and calls C (`ibj_slow`) for
   the rest. *Cost here: done for the common ops; the long tail (FP, `emul`,
   `scanbit`, `modpc`, ...) can stay as calls.*
3. **Register allocation.** Decide which guest registers live in host
   registers, and when they go back to memory. This is most of the speed, and
   it is what the prototype leaves out: every op loads its sources from the
   cpu struct and stores its result back. *Cost here: the largest single
   piece of new work (see "Registers" below).*
4. **Emission.** Encode SH-4 instructions, place the constants the code needs
   in a literal pool it can reach, patch branches, respect delay slots. The
   prototype has an emitter (`ibj_t`, 64 constants a block, 2 KB of code a
   block at most). *Cost here: done, but it does not schedule for the SH-4's
   two pipes, which GCC does for the AOT's C.*
5. **Memory access.** Every guest load and store becomes a fast path (plain
   RAM, through the page table) and a slow path (MMIO, a page that is not in,
   an unaligned word). The fast path's length is paid on every access.
   *Cost here: done (`rd_page` / `wr_page` inline), ~6-8 SH-4 instructions
   before the access itself.*
6. **Exits, the dispatcher and block linking.** When a block ends, either go
   back to a loop that finds the next block or jump straight to it. Linking
   (chaining) is what makes short blocks cheap. The prototype always goes
   back to the run loop. *Cost here: not built. It needs a way to unlink a
   block when its target is flushed.*
7. **The code cache.** Where the code lives, when it is thrown away, and what
   happens when it fills. On the SH-4 every new block also needs the operand
   cache written back and the instruction cache invalidated, from uncached
   (P2) code. The prototype's 384 KB buffer flushed 224 times by frame 1152.
   *Cost here: needs a bigger buffer (no room) or a smarter policy.*
8. **Exactness: timing, interrupts and state.** Each compiled block must
   charge the cycles the interpreter would (`i960_cycle_cost`), end before
   the timers' horizon, and leave the cpu struct exactly as the interpreter
   would at every point the outside can look. The board is deterministic and
   its timers feed `rand`, so an off-by-one cycle changes the fight.
   *Cost here: solved per block for the block runner and the AOT; a
   recompiler that keeps registers in host registers has to solve it again
   at every exit.*
9. **Invalidation.** Code that is written to must be recompiled. Every
   program the port runs today executes from ROM, so this is cheap here: no
   self-modifying code to watch for.
10. **Testing.** Run the same code both ways and compare everything.
    `make JIT=1 JIT_TEST=400` (random blocks, registers, CC and memory) and
    `det_digest --cpu` (thousands of frames, byte-identical) already exist.
    *Cost here: the harness exists; every new optimisation must pass it.*

The prototype is items 1, 2, 4, 5 and 10. Items 3, 6 and 7 are what #386 said
a real one would need. Item 8 is what makes them hard.

## Why the i960 and the SH-4 are a hard pair

Most well-known recompilers translate a guest with few registers to a host
with more: the PS1's MIPS or the Saturn's SH-2 to x86-64 or ARM64. Here it
is the other way round, and almost every feature disagrees.

| | i960 KB (the board, 25 MHz) | SH-4 (Dreamcast, 200 MHz) |
|---|---|---|
| Integer registers | 32: g0-g15, r0-r15 | 16, of which r15 is the stack and r0 is the only index register |
| Operands | three (`addo g1, g2, g3`) | two, and the destination is a source (`add r1, r2`) |
| Calls | `call` saves r0-r15 (a new frame), `ret` brings them back | `jsr`; the callee saves what it uses |
| Condition | 3-bit CC: less, equal, greater (and unordered) | one T bit |
| Immediates | 5-bit literals, 12-bit offsets, 32-bit displacements | 8-bit `mov #imm`; anything else from a literal pool |
| Load / store offsets | any 32-bit displacement, base + index × scale | 4 bits × size (60 bytes for a word), or r0 + Rn |
| Branches | 24-bit displacement | `bt`/`bf` ±256 bytes, `bra` ±4 KB, beyond that `jmp @Rn` |
| Shifts | count ≥ 32 gives 0 (or all sign) | `shld`: count mod 32, sign means direction |
| Divide | `divo`, `divi`, `remo`, `modi`, `ediv` in hardware | none: `div1` one bit at a time, or the FPU |
| Bit search | `scanbit`, `spanbit` | no count-leading-zeros |
| Floating point | four 80-bit registers, or IEEE singles in integer registers | single precision fast; double only by switching FPSCR.PR |
| Unaligned access | allowed (slow) | address error |
| Byte order | little-endian | little-endian |
| Caches | | 8 KB I-cache, 16 KB operand cache, both direct-mapped |
| Memory | an 80 MB board | 16 MB in all |

Byte order is the one thing in the port's favour. The GameCube and PS3 ports
of STF (GEMS-COLLECTION.md) swap bytes on every access; the Dreamcast does not
have to.

### Registers

With 32 guest registers and roughly 8 usable host ones, the whole register
file cannot live in host registers. A real recompiler has to cache a few, the
ones the block uses most, and write them back at each exit or call out.
Every exit then needs to know which host register holds which guest register,
and every slow path (a COP store, a page fault, an interrupt check) has to
spill them and reload after. That bookkeeping is the work #386 did not do:
its code keeps nothing in registers, which is why it is correct and slow.

The 4-bit displacement shapes even the simplest version. `mov.l @(disp,Rn)`
reaches 15 words, so the prototype points r14 at the globals and r13 at the
locals, and each 16-word bank is one base register away. Anything beyond the
first 16 words of a base (the AC, the cycles, the FP registers) needs r0 or
a literal.

### Calls save 16 registers

An i960 `call` gives the callee a fresh set of local registers. The KB keeps
a few sets on chip; m2-hle2 keeps them on a host stack (`frame_stack`), so
every `call` and `ret` copies 64 bytes, plus the frame pointer bookkeeping.
STF is C compiled by gcc960, so it calls a great deal. A recompiler cannot
skip the copy unless it proves the frame is never looked at, and the
program does read its frames (pfp, sp, rip are in r0-r2). #459 measured
inlining `call` / `ret` in the AOT: a few percent of the slice, at a code
size the heap could not spare.

### The condition code

`cmpi`, `cmpo`, `cmpr` set one of three bits. Branches (`be`, `bge`, `bno`
...), `test*`, `concmp*`, `addc`/`subc` and `alterbit` read them. CLAUDE.md
lists the traps: `cmpobX` / `cmpibX` update the CC even when the branch is
not taken, `chkbit` sets 000 and not 101, and an unordered `cmpr` sets 000.
The SH-4 gets one answer per compare (`cmp/eq`, `cmp/gt`, `cmp/hi` ...), so a
full CC is two compares and some shifting. The cure is to work it out only
when something reads it: the AOT keeps it in a C local (`cc_`) and lets GCC
drop the dead ones; the prototype keeps it in r9. Either way the CC has to
be real at every exit, since an interrupt can land between `cmpo` and `bg`
(CLAUDE.md: `unpack_lod_data`'s bit-buffer refill).

### Constants, offsets and branches

i960 code is full of 32-bit addresses (MEMB displacements, `lda` of a
table), and the SH-4 can only load them from a literal pool, PC-relative and
at most 1020 bytes ahead. The emitter has to collect a block's constants and
place them where every load can reach. Branches beyond ±256 bytes need a
branch around a branch, and beyond 4 KB an address in a register. None of
this is slow, but it is fiddly, and it is why blocks have a size cap.

### Shifts, divides, bit searches

- `shlo`, `shro`, `shri` with a register count need a compare and a branch:
  the i960 gives 0 (or the sign) at 32 and over, the SH-4 wraps.
- A divide is dozens of SH-4 instructions: 32 `div1` steps, or a trip
  through the FPU in double precision. The i960 does it in one.
- `scanbit` (highest set bit) has no SH-4 instruction; it is a loop or a
  table.
- `emul` is `dmulu.l` and two `sts`; `mulo` is `mul.l` and `sts macl`. Fine.

These stay calls into the interpreter's code in practice. They are rare.

### Floating point

The KB has four 80-bit FP registers, and arithmetic on IEEE singles held in
integer registers. m2-hle2 holds the FP registers as `double`. The Dreamcast
port builds with `-m4-single`, where `double` is emulated in software, so
the FP paths stay `float`. A recompiler could emit real double-precision
SH-4 ops, but each switch of FPSCR.PR is an `lds` to FPSCR, which stalls.

Exactness is the harder part. A single-precision result computed in single
precision is the same as computing it in double and rounding, so `addr` on
singles is safe. But KOS runs the FPU with denormals flushed to zero
(FPSCR.DN = 1), and turning that off makes denormal operands trap.
DREAMCAST-PORT.md (#461) found the first divergence from MAME exactly there,
at frame +580 of the replay. `sh-elf gcc` also fuses `a*b+c` into `fmac`
unless told not to. A recompiler would inherit all of this.

### Memory: the board does not fit

The board's address space is 80 MB of ROM and RAM with MMIO in between:
the COP FIFO and GEO at `0x008xxxxx`, the timers that `rand` reads. On the
Dreamcast all of ROM is paged through a 1-1.5 MB cache. So
every load and store is a page-table lookup and a test, then the access. Two
ways to make it cheaper, neither good here:

- **The SH-4's MMU as "fastmem"**: map the guest's space onto the host's and
  let the TLB do the lookup. The UTLB has 64 entries with a software refill,
  and in Flycast the MMU costs a third of the frame (#394, part 12). The
  port turned the MMU off for that reason.
- **Unaligned words** fault on the SH-4, so the fast path must test the low
  bits (or the code must be known aligned) before every word access.

And the code cache competes with the board for the same 16 MB. The AOT at
`AOT_COVER=0.98` is 1.0 MB of SH-4; all of it (4.1-4.8 MB) does not boot.
The prototype's 384 KB buffer was already too small for one frame of STF.

### The caches

The SH-4 has an 8 KB instruction cache and a 16 KB operand cache, both
direct-mapped. Writing code means writing back the operand cache and
invalidating the I-cache lines, from code running uncached; on a 200 MHz CPU
that is not free per block. More code also means more I-cache misses: STF
runs most of its code once a frame, far more than 8 KB of it. In Flycast
there is a further cost that hardware does not have: Flycast recompiles its
own translation of any 4 KB page that is written, so the JIT's buffer has to
sit on pages of its own (`s_ibj_buf`, 4096-aligned).

### Timing has to stay exact

The board is deterministic: the same frames give the same hash on every
build, and the port is held to MAME frame by frame over the serial link
(#461). That holds only because each block charges the interpreter's cycles,
runs only if it ends before the timers' horizon, and stops after any access
that moved the attention word, the interrupt lines or the horizon. A hook's
address is never compiled, and neither are Gems' 47 trap sites. Every one of
these rules has to hold in a recompiler too, at the granularity of its
blocks, including linked ones: a chain of blocks still has to stop at the
horizon.

## What the port has built, and what it got

| | i960 work | status |
|---|---|---|
| Interpreter (`i960_exec_word`) | ~1.5-2 µs an instruction | the reference |
| Decoded blocks, replayed (#370, `i960_blocks.h`) | attract ~10% faster | on with AOT, 2 KB pool |
| JIT prototype (#386, `i960_jit_sh4.h`) | 3x on register-only code; real slice 61 ms against 44 | `make JIT=1`, off |
| AOT to C (#394, `i960_aot.h`, `tools/i960_aot.py`) | slices to frame 1500: 81.8 s to 54.6 s | on (`AOT_COVER=0.98`) |
| Handlers compiled, paged ROM inline (#459) | slice 6.6-7.3% shorter | on |
| Gems' C (#456) | slice 8.7 s to 5.9 s over 400 frames | on when present, private |

Why the prototype lost:

- **Blocks are short.** A run of block-able instructions is 1.9 long on
  average, half are runs of one, and a third of all steps are the idle loop
  (already skipped by a hook). Entering and leaving a block cost more than
  its ops.
- **Every exit went back to the run loop.** No chaining.
- **The buffer thrashed.** 224 flushes, 197k blocks compiled (7 s of
  compiling) by frame 1152.
- **The slow paths stayed calls.** 4.7M of them, the same C as before.

Why the AOT won where the JIT lost: no compile time at run time, no buffer to
flush, GCC's register allocation and scheduling within each 2^n-byte chunk,
and the CC kept in a local that GCC can drop. It is a static recompiler; it
just leaves the back end to GCC.

Where an attract fight frame goes now (#456's bench, with Gems): ~15 ms of
i960 and ~21 ms of drawing. #459 counted, per host frame, ~11.5k i960
instructions run compiled, 372 interpreted and 1285 in the block runner. The
i960 slice is mostly Gems C, hooks and the COP.

## So what would a real one cost, and buy?

**To build** (beyond what exists): a register cache with spill and reload at
every exit and slow path; block linking with unlinking on flush; a code
cache policy that fits in a few hundred KB beside the board; the timing
rules (horizon, attention, hooks, traps, interrupt floor) carried across
linked blocks; and enough of the long tail compiled that the slow calls stop
dominating. Each of these is about the size of #386 or larger, and each must
keep `det_digest --cpu` byte-identical over thousands of frames. An estimate,
not a measurement: 3,000-6,000 lines and several months of part-time work,
plus testing on real hardware, which Flycast's timing cannot stand in for.

**To gain**, for STF: the AOT already runs nearly all the i960's
instructions as compiled code. A JIT could beat it only by keeping registers
in host registers across whole functions, and by covering 100% of the code in
less memory than the AOT's 4 MB, since it compiles only what runs. The first
is worth a few ms of the ~15 ms slice at best; the second matters only if the
AOT's 2% uncovered tail becomes the bottleneck, and it is 372 instructions a
frame today. Neither reaches 30 fps on its own: the drawing is ~21 ms.

**Where a dynamic recompiler would pay**: a Model 2 game without an AOT map
or Gems C (every other game, until one is recorded), code that is not in ROM,
or a homebrew program loaded at run time. Even there, recording an AOT map
for the game is cheaper.

## Cheaper ways to most of it

1. **Function-level AOT.** Translate whole i960 functions to C functions,
   with r0-r15 as C locals and `call` as a C call, as Sega did for Gems' 94
   functions. GCC then keeps the hot registers in SH-4 registers. The frame
   copy stays unless the generator proves a function never reads its frame.
   This is the "real recompiler" worth building first: the same rules as
   today's AOT, no run-time compiler, no I-cache flushing.
2. **Profile-guided AOT with a smaller footprint.** Compile the hot chunks
   with `-O2` and the warm ones with `-Os`, so more of the map fits in the
   same megabyte.
3. **The hot state in the operand cache's RAM mode** (DREAMCAST-PORT.md,
   "Next optimization targets"): the cpu and bus structs in the 8 KB on-chip
   RAM, so the register file loads never miss.
4. **The drawing side**, which is the larger half of the frame: per-face
   `ftrv`, `fipr` and the store queues in the mesh decode and submit.

A dynamic i960 to SH-4 recompiler is possible. It is the most expensive item
on this list and, for STF, the one with the least left to win.
