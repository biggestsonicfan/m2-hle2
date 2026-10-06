# The Dreamcast port against KallistiOS's own examples (Pinboard #520)

Two things are here. First, what KallistiOS's examples do for assertions, stack traces,
threading, GL rendering and the PVR effects, held against `dreamcast/` (**A**, the
port on `idea-340-dreamcast` with Gems C and AOT). Second, **B**, a Dreamcast port of
the same board written from scratch the way those examples write things
(`dreamcast_kos/`). B renders through GLdc, uses KOS threads and the assert handler,
and shares only the board layer with A. The two are measured on the same disc image
and the same Flycast.

## 1. The examples

| Example | What it shows | The idiom |
|---|---|---|
| `basic/stacktrace` | a backtrace from anywhere | `arch_stk_trace(n)`; needs frames that save PR (`-fno-omit-frame-pointer`) |
| `basic/asserthnd` | replacing the assert handler | `assert_set_handler(fn)`; declared even under `NDEBUG` (`assert.h:131`) |
| `basic/watchdog` | a hang turns into a reset | `wdt_enable_watchdog(0, WDT_CLK_DIV_4096, WDT_RST_MANUAL)` + `wdt_pet()` in the frame loop |
| `basic/stackprotector` | stack smashing caught | `-fstack-protector-all` and a `__stack_chk_fail` that prints `arch_get_ret_addr()` |
| `basic/threading/*` | threads, semaphores, mutex/cond, sleeping | `thd_create`/`thd_join`, `kthread_attr_t { prio, label, stack_size }`, `thd_sleep(ms)` to wait, never a `thd_pass` spin |
| `pvr/pvrmark_strips_direct` | the fastest PVR submission | `pvr_dr_target`/`pvr_dr_commit`, strips, a 512 KB vertex buffer |
| `pvr/modifier_volume`, `cheap_shadow` | shadows without drawing twice | `pvr_mod_compile(..., PVR_MODIFIER_OTHER_POLY / INCLUDE_LAST_POLY, ...)` and `pvr_modifier_vol_t` triangles in the `*_MOD` lists |
| `pvr/bumpmap` | sprite contexts, KMG textures | `pvr_sprite_cxt_t` in OP and PT, `pvr_txr_load_kimg` |
| `pvr/texture_render` | render to texture | a `pvr_mem_malloc`'d 1024x512 RGB565 non-twiddled target |
| `pvr/fb_tex` | the framebuffer as a texture | `PVR_TXRFMT_X32_STRIDE` |
| `gldc/*` (nehe, quadmark, trimark, tristripmark) | OpenGL on the PVR | `glKosInit`, `glBegin`/`glEnd` or arrays, `glKosSwapBuffers`, VQ and twiddled textures |

## 2. A against them

A renders with the PVR API directly (`dc_pvr.h`: DR vertices into OP16/TR16, its own
`GEO3D_DC_SINK`), which is what `pvrmark_strips_direct` does and is the right choice for
speed. What differs from the examples:

**Debugging aids: none of the four.**
- No assert handler, no backtrace, no watchdog, no stack protector. `dbgio` goes to
  `null` (`main_dc.c:335`) unless `DC_STATS_DBGIO`, so a failed KOS `assert` prints
  nowhere and the player sees a frozen picture. A crash in the field leaves nothing.
  `assert_set_handler` costs nothing at run time and works with `NDEBUG`; a handler
  that draws the expression and `arch_stk_trace` onto the screen (the text layer is
  already there) would make every field report a stack.
- A hang (the pager's poll below, a COP FIFO wait) never resets. `wdt_pet` once a
  frame is the example's whole cost.

**Threading.**
- The FPS cap waits by spinning `thd_pass()` (`main_dc.c:468`). With the sound thread
  the only other thread, that spin is the CPU the cap was meant to give back.
  `thd_sleep` (ms) for most of the wait, then the spin, is the example's way.
- The sound thread is `thd_create(1, ds_thread, NULL)` (`dc_sound.h:534`): the default
  priority and no label. `kthread_attr_t { .prio, .label = "sound" }` makes its share
  explicit and names it in `thd_pslist`.
- The pager polls the GD-ROM syscalls in a tight loop (`pg_read_cmd`, `dc_pager.h:118`,
  up to 1e8 spins) and never yields. The sound thread starves for the length of a
  read, which is when a page fault is already costing a frame.

**PVR.**
- Tile layers are written to VRAM a `uint32` at a time (`dp_tiles_convert`,
  `dc_pvr.h:758`). The examples build the texture in RAM and use `pvr_txr_load` /
  `sq_cpy` (store queues), which write 32 bytes at a time. A already uses
  `pvr_txr_load` for the ROM textures.
- The layers are single-buffered (`dc_pvr.h:536-540`: bg and fg 512x512x2, text
  1024x512x2). Rewriting a texture the PVR may still be reading is a tear on hardware
  that Flycast does not show.
- The empty front layer is still drawn as a quad, and the 768 KB vertex buffer is
  more than the frames use (the strips example runs on 512 KB).
- No modifier volumes. STF's shadows are drawn as flattened geometry
  (`rob_kage_disp_test`), which the board does too, so that is faithful. A modifier
  volume (`cheap_shadow`) would be cheaper on the PVR, but it would not be the board.
- `-lGL` is linked but A never calls GL (`Makefile` 162, 166, 172). It costs nothing
  but link time, because the linker drops it.

## 3. B: the port written from the examples

`dreamcast_kos/main_kos.c` (one file, ~900 lines) on GLdc 1.1.1 from kos-ports:

- Every face goes into GLdc's TR list as `GL_TRIANGLES` arrays, batched by texture,
  autosort off, in the board's order (a radix sort on `geo3d`'s key). Faces come from
  `geo3d`'s DC sink, as in A.
- ROM textures are cut to 8bpp `GL_COLOR_INDEX8_EXT` against two shared 16-entry
  palettes (opaque, and texel 15 transparent). Tile layers and text are ARGB1555
  textures. Text is `bfont_draw_str`.
- `assert_set_handler` prints the expression and `arch_stk_trace` (debug builds),
  the frame cap sleeps with `thd_sleep`, and the sound and pager are A's.

What B does not have, by design or for lack of time: the offset colour (specular), the
scissor windows, `STRIPS.PAK`'s pre-built strips, mip levels, tiles over 512 texels
(drawn untextured) and full-width text. B's texture cuts are counted inside its decode
time.

### What GLdc does that the examples do not tell you

These cost the most time, and each one fails quietly:

1. **GLdc grows its vertex lists at run time** (`containers/aligned_vector.c`): 256
   vertices at a time, by `memalign` of the new size, a copy and a `free`. In a release
   build the `assert(vector->data)` is compiled out, so when the heap cannot take the
   new block the next write is through NULL. B's first long run went black right after
   the FBI screen this way. Reserve the list once (`initial_tr_capacity`) and keep every
   frame inside it. B budgets each frame from the nearest face back and drops the
   farthest faces past the reserve (`K_TR_CAP`, counted in `lost`).
2. **`GL_RGB5_A1` is not ARGB1555 in GLdc.** `_cleanInternalFormat` maps it (and
   `GL_RGBA`) to twiddled **ARGB4444**: half the colour bits, and a conversion on
   every upload through a temporary buffer the size of the whole texture. For a
   512x512 layer that is 512 KB allocated and freed per upload, on a heap with about
   570 KB left. Ask for `GL_ARGB1555_KOS` and pass `GL_BGRA` /
   `GL_UNSIGNED_SHORT_1_5_5_5_REV`: no conversion, no buffer.
3. **`glTexSubImage2D` with a conversion wipes the rest of the texture.** It zeroes a
   whole-texture buffer, converts the rectangle into it and copies the whole buffer
   over the texture. Only the no-conversion path (a non-twiddled format given in its
   own layout) updates a rectangle. B allocates the layers and the text once with
   `glTexImage2D(..., NULL)` and refills them in halves through that path, which lets
   one 256 KB staging buffer do every upload.
4. A TR vertex is 64 bytes in GLdc's list and a draw's header takes one more slot.
   The near-Z clip can add a vertex per triangle. B reserves 10240 (640 KB of heap): A keeps its vertices in VRAM only, GLdc keeps a RAM copy of every one.

## 4. The A/B

Same disc image (`sfight`, Gems C and AOT where a build has them), Flycast in RetroArch
under Xvfb, an 11-minute run of attract with a screenshot every 30 s. The bench is
frames 3500-3900 of attract, wall time. The hash is the board's state at frame 1500.

| | A (`dreamcast/`, PVR DR) | B (`dreamcast_kos/`, GLdc) |
|---|---|---|
| board hash, frame 1500 | `634d853f` | `634d853f` |
| frames 3500-3900 | **14035 ms** (28.5 fps) | **39427 ms** (10.1 fps) |
| of which the board (slice) | 5228 ms | 5298 ms |
| of which drawing | 8791 ms | 34124 ms |
| B's drawing, by part | | tiles 13555, submit 13490, decode + texture cuts 6699, sort 336 ms |
| most faces in a frame | | 3595 |
| faces dropped at the vertex reserve | none | ~13 a frame (54059 by frame 4246) |
| pager cache | 1280 KB | 1024 KB |
| heap left in a fight | 576 KB | 184 KB |

Both ran attract into the fight without a fault. The identical hash says the board
is the same under both, AOT and Gems C included, so every difference is the host
side.

**What the A/B says.**
- The examples' GL path is easier to write and much slower to run here. B's draw is
  3.9 times A's. Half of the gap is GLdc's submission: every vertex is written into
  GLdc's list in RAM, then transformed and copied again into the PVR at swap. A writes
  each vertex once, straight into the PVR (DR). The other half is B's tile layers:
  a full redraw and full upload when anything changes, where A converts only the dirty
  lines.
- GLdc costs RAM that A spends on the pager cache: the RAM copy of the vertex list
  (640 KB reserved) and the staging for the uploads. B's cache is 256 KB smaller and its
  heap in a fight about 400 KB tighter, with faces dropped all the same.
- B's geometry, draw order and tile layers agree with A's shots. Its textures do not
  all agree: some large surfaces draw flat where A textures them (the ranking stage's
  ice, Robotnik's block on the title). The 512-texel cut limit is the likely cause and
  was not chased. B also lacks the specular and the scissor windows (section 3).

**Worth taking into A from the examples** (cheap, and none of it touches the board):
1. `assert_set_handler` with a backtrace (`arch_stk_trace`) onto the screen, and the
   watchdog petted once a frame.
2. `thd_sleep` for the bulk of the FPS cap's wait.
3. A labelled, prioritised sound thread (`kthread_attr_t`), and a `thd_pass()` in the
   pager's GD-ROM poll so a read does not starve it.
4. Tile layers staged in RAM and sent with `sq_cpy` / `pvr_txr_load` instead of
   32-bit VRAM writes, double-buffered so the PVR never reads a half-written layer.
5. Drop `-lGL` from the link lines.

Not worth taking: GLdc itself (above), and modifier-volume shadows, because the board
draws its shadows as geometry and A matches it.

Reproduce: `make -C dreamcast_kos OUT=<dir> AOT=<rom_code1.bin> HASH_FRAME=1500`, then
`dreamcast/mkdisc.sh <dir> <roms> <dir>/disc <STF.AFS>` (with `TEXPAK` set, as for A). The HUD prints the hash on row 6 and the bench on rows 3-4.
