# Threading

Part of m2-hle2's load-bearing invariants, indexed in [CLAUDE.md](../../CLAUDE.md): facts reverse-engineered or debugged into the emulator, in no datasheet. Do not re-derive them.

- **The renderer reads the GEO state published with its list, never the live copy** (`g_geo_rs`, `geodl_raster_for`). Texture RAM, polygon RAM and the material slots are applied on the emu thread at each publish and copied beside that list's snapshot. STF lays each frame's replaced texture points (eyes, mouths) from `0x805000` on, a different set each frame, so a render still drawing list N from live RAM took list N+1's points.
- **The sound board runs on its own thread, and anything that touches it calls `sound_settle()` first** (`sound.h`, "The sound thread"). The frame's samples are handed over at the frame edge (`sound_advance`) and run while the next slice's i960 does. The i960's side (the UART callbacks, `sound_uart_make_room`), a reset, a capture and every bridge command wait for the run first, so the board does the same work in the same order: `det_digest` (thread on and off, and the single-threaded wasm build) and `ab-builds --sound` against master are identical, every sound sample included. Two things are new: the samples appear up to a frame later, so the frame clock is `g_sound.out_due`, not `out_total`, and a host that drains the ring on its own thread reads to `out_pub` (libretro); and a new reader of `g_sound` has to settle first or it reads a run half done. `M2HLE_SOUND_THREAD=0` (or the libretro option) turns it off.
  - *Measured* unthrottled on x86 (STF attract): 855 fps to ~1000. A third of frames still wait (`sound_wait_us` in get_status's "emu"): a slice that starts with a sound byte queued has to know TxRDY, which needs the 68000's clock to the cycle. A host with other work between slices (a draw, a pacing sleep) covers that.
- **Unlock the emu mutex BEFORE sleeping.** Sleeping inside the critical section freezes the UI.
- **Double-buffered CPU snapshot** (`cpu_snapshot` + `cpu_prev_snapshot`); UI always reads the current snapshot.
- **Sleep granularity**: POSIX `usleep()` ≈ 1 µs, but Windows `Sleep()` is **~15.6 ms**, not 1 ms,
  unless something in the process has asked for a finer timer. A window or an audio device
  usually has; a `--headless` run has not, and a 1 ms poll there really waits 15.6 ms (measured
  — see the A/V section below, where it cost a third of the frames). Since Windows 10 2004 another
  program raising the system timer no longer lends it to a process that did not ask, so the 60 Hz
  throttle (`emu_sleep_us`) waits on a per-thread `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` timer too,
  falling back to `Sleep` only where one cannot be made. *Measured with `Sleep`:* `Sleep(14)` took
  15.5 ms, the catch-up clamp threw the deadline away, and a headless stream ran at ~52 board fps
  with a gap every few frames.
- **The netplay pump runs outside the mutex** (`emu_netplay_pump`, `emu_thread.h`) and runs in the
  STOPPED branch too. A TLS connect blocks for seconds, so pumping it under the mutex freezes the
  UI; pumping it only while RUNNING means a player who connects before pressing Run never logs in.
