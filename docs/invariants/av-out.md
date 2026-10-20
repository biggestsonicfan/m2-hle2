# Raw A/V out (`--av-port`, host-side — `core/av_stream.h`, `ui/av_capture.h`)

Part of m2-hle2's load-bearing invariants, indexed in [CLAUDE.md](../../CLAUDE.md): facts reverse-engineered or debugged into the emulator, in no datasheet. Do not re-derive them.

One local client gets BGRA frames and 16-bit stereo samples on one socket, every packet stamped
with the board's own 44.1 kHz counter (`g_sound.out_total`, latched per game frame in
`g_frame_clock`). Four of these were measured, not reasoned out, and three of them fail quietly.

- **The audio has to be tapped at the producer, in `sound_out_push`.** The host ring below it
  has exactly one reader (`audio_out.h`), and a host with no audio device drains nothing — so a
  second reader of that ring only ever sees what the first already counted into `out_dropped`.
  `sound_set_tap` is that tap and `out_total` is the clock that goes with it; `sound_reset`
  leaves both alone, so a board reset does not move an A/V client's clock backwards.
- **A headless D3D11 device never Presents, so nothing submits the command buffer, and a
  staging `CopyResource` is still unfinished when the non-blocking `Map` reaches it two frames
  later.** `av__cap_issue` calls `ID3D11DeviceContext_Flush` after the copy for that reason
  (`Flush` does not wait). *Measured:* 30% of frames survived the map without it and 100% with
  it — 16 fps became 55. A windowed run gets the same flush free from Present, which is exactly
  why this does not show up until there is no window.
- **`Sleep(1)` is ~15.6 ms in a process that has neither a window nor an audio device**, because
  nothing in it has asked Windows for a finer timer. Polling the frame clock at 15.6 ms against
  a 16.7 ms frame caught 37 of 60. The headless loop uses a
  `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` waitable timer instead — a real millisecond, no
  `timeBeginPeriod`, nothing past kernel32.
- **Copies still in flight when a client goes must not reach the next one.** The first video
  packet of a session is what sets where the audio starts, so one leaked frame tagged with an
  old board frame drags the audio read head back by however long the emulator sat between
  clients — and the whole next session is then spent lapping through stale audio. `av_capture`
  drops its in-flight copies when `av_stream_session()` changes, and `av__serve` clamps the
  origin to what the ring still holds. *Symptom:* a second client got 0.6 fps and a lapped
  audio ring where the first had got 13.
- **The rings are allocated once and never freed** (`av_stream_shutdown` says so). The tap runs
  on the emu thread and clearing the function pointer does not retire a call already inside it.
  Same shape as the texram crash under "Memory Bus", same answer.
- **On Linux, stream with `--headless --av-port`, not from a window.** Capture mode is Win32-only,
  so `--kiosk` on Linux is an ordinary ImGui window, and on Xvfb every swap copies the whole back
  buffer out of the GPU into the X server after waiting for the GPU to finish. In the fly's
  docker container (Mesa d3d12 over WSL, the GPU shared with the stream's CUDA encoder) the frame
  callback took ~6 ms and the swap ~19 ms: the board ran at 60 and the tap got 33-39 frames, all
  counted as `dropped_missed` (a 16x16 window still paid ~10 ms). Headless is a surfaceless EGL
  context (`headless_gpu_init` in `main.c`, libEGL by `dlopen`): nothing is presented, no X server
  is needed, and the same bench held 60/60. `GALLIUM_DRIVER=d3d12` picks the GPU as it does for a
  window; with none, Mesa gives llvmpipe, where the fill shader is 98% of the process.
  *Symptom:* the stream's "board 17-26 fps in, ~700 gapped" while `get_status` said 60.
