# WEB-PORT.md

The plan for **play.sonicthefighte.rs**: a UI-stripped WebAssembly build of this emulator that plays *Sonic the Fighters* over RPCN, deployed to GitHub Pages from this repository on every push to master.

Pair with [CLAUDE.md](CLAUDE.md) (invariants) and [PROPOSAL.md](PROPOSAL.md) (architecture). This file is the web target's equivalent of both: what was found, what was decided and why, and the order to build it in. Work happens on the `wasm` branch.

Status: **M1 done, M2 boots.** `arc-s` is merged (`dd6f3cb`) and the web frontend runs: a full Sonic-vs-Knuckles fight at 60 game fps in headless Edge, from a merged zip loaded by CRC, with keyboard input and sound in sync (confirmed by ear), on one thread. Not done: any network path (M3), the wizard past its first step (M4), the deploy (M5). Section 8 is what has been *measured*; everything else about browser behaviour is still a claim to be checked at the milestone that names it.

---

## 1. What the browser takes away

Three things native builds rely on do not exist in a browser tab. Each one decides a piece of the architecture.

| Native | Browser | Consequence |
|---|---|---|
| TCP/TLS to RPCN (`tls.h`, Schannel), UDP to the signaling helper and the peer (`net_socket.h`) | No sockets of any kind. WebSocket, WebTransport and WebRTC only. | **A gateway daemon has to exist**, on a host we control, or the web build cannot reach RPCN at all. §4. |
| A process with its own threads; headers are whatever we like | GitHub Pages is static files and **cannot set HTTP response headers**. Threads in wasm need `SharedArrayBuffer`, which needs `COOP`/`COEP` headers. | **Single-threaded first.** §3.3. |
| ROM zips on disk next to the exe; `m2hle_netplay.cfg`; `m2hle.log` | No filesystem. ROMs can never be hosted by us. | The player supplies one zip; it is read from memory and cached in the browser. Settings go to `localStorage`. §3.5, §3.6. |

Two things work *better* than expected, and both were checked in the source rather than assumed:

- **The net layer's whole contact with the OS is two headers and 11 call sites**, all in `rpcn_client.h`: `tls_connect` / `tls_send_all` / `tls_recv` / `tls_close`, `net_udp_open` / `net_udp_send` / `net_udp_recv`, `net_resolve_ipv4`, `net_local_ipv4_towards`. `tls.h` already has a non-Windows stub backend. Nothing above `rpcn_client.h` knows what a socket is.
- **Only `tls_connect` blocks.** Everything after it is a polled state machine (`rpcn_poll`: "Never blocks"). A WebSocket backend can return "connected" at once, queue sends until `onopen`, and surface a failed open as a closed stream — so the netcode runs on the browser's main thread with **no pthreads and no Asyncify**.

---

## 2. Base branch: `wasm` = netplay fixes + `arc-s`

`wasm` was cut from `fix/netplay-session-start-crash-and-twitch-reauth` (`a58426a`), not master, because the wizard is built on exactly what that branch fixes: Twitch token reuse (`c3ca0bf`), the step count in the frame check (`7d17843`), and texture RAM freed under the frame callback (`aa0d069`).

`origin/arc-s` is the other half. It already contains most of what "UI-stripped" and "small footprint" mean:

- `src/ui/game_frame.h` — the per-frame compose/scan/draw lifted out of `main.c`, shared by every frontend "so the two cannot drift". The web frontend is its third caller.
- `M2HLE_FRONTEND` in CMake, and a frontend (`src/main_sdl.c`) with **no ImGui, and so no Python/ply/dear_bindings** in the build.
- `game_render.h` compiles its GLSL for **GLES 3 — the same dialect as WebGL2** (GLSL ES 3.00). The `#version 410` shaders on master would not compile in a browser.
- The perf work, measured on an RK3566 (four Cortex-A55 cores, far below any desktop): `bp_check` early-out, hook address filter, `mem_fetch2`, region-lookup cache, forced-inline `i960_step_hot`, GPU tile compositor with incremental compose, mesh cache, incremental texture atlas (per-KB dirty flags instead of 4 M texels a frame), discard-free fill pipeline, colour-ramp fetch. That device holds 58–60 game fps; this is the evidence that one browser thread can carry emu + render (§3.3).

**`arc-s` predates netplay entirely** — no `src/net`, and `main_sdl.c` has no audio either. So the web frontend is not a port of `main_sdl.c`; it is a new host that takes `arc-s`'s stripped structure and master's netplay + `audio_out.h`.

### The merge is small textually and not small semantically

This merge is done (`dd6f3cb`); what follows is why it needed more than resolving markers, kept because the same trap waits for the next long-lived branch. `git merge origin/arc-s` into `wasm` gave **5 conflict hunks in 3 files**:

| File | Hunks | Nature |
|---|---|---|
| `src/board/memory.h` | 1 | Mechanical. Both sides added a function at the same spot (`dl_tap`/`dl_cop_tap` vs `mem__note_change`). Keep both. |
| `CMakeLists.txt` | 2 | Mechanical. Submodule probe list (keep `imgui_club` on the sokol path), and `arc-s` moved the miniz block above the frontend split (drop the second copy, keep the `imgui_club` block). |
| `src/ui/game_render.h` | 2 | **Semantic.** See below. |

The semantic conflict is one master commit, **`8391e6b` "pick mip levels from the board's per-polygon texlod"** (+127/−30 across `geo3d.h` and `game_render.h`). It widened `lbpl` from `vec3` to `vec4`, added `ez`, and added `g_geo3d_mode` / `g_geo3d_lod` / `g_geo3d_emit_texlod` and polygon-RAM meshes (`model_idx < 0`). Everything `arc-s` added *after* its merge-base has never seen it (`grep` count in `arc-s`: `g_geo3d_mode` 0, `g_geo3d_lod` 0, `texlod` 0 in `geo3d.h`):

- `geo3d_decode_model_cached` — a second copy of the decode path. `geo3d.h` **auto-merges with no conflict and is still wrong**: the cached path would emit faces without texlod.
- The optimised fill shader, `game_render_fill_fs_ref_glsl`, the discard-free `fill_opaque` variant, and the GLES build of each.

What the port has to get right:

- `geo_mode` / `geo_lod` are **per object, not per model**. They cannot go in the mesh cache; cache the per-face input and apply mode/lod at replay, the way the cache already replays transform, lighting and live-palette colour.
- **Polygon-RAM meshes must bypass the cache** — the mesh lives in RAM the game rewrites — the same way texture-RAM UV streams already take the full decoder.
- A `vec4`/`vec3` mismatch between stages is a link error on GL and **silent on nothing**: D3D11 (HLSL) is a separate source and must get the same change. D3D11 is what the live native builds run.

---

## 3. Architecture

### 3.1 A third frontend: `M2HLE_FRONTEND=web`

`src/main_web.c`, built only under Emscripten. `sokol_app` (canvas, WebGL2 context, keyboard, `requestAnimationFrame`) + `sokol_gfx` (GLES3) + `sokol_audio`, plus `game_frame.h`, `audio_out.h`, `input.h`, `netplay.h`. **No ImGui, no ImGuiFileDialog, no `mem_edit.cpp`, no MCP bridge, no kiosk, no SDL.** `sokol_app` over SDL3 because it is already in the tree and its Emscripten backend is a fraction of the size of SDL3's port (to be confirmed with a size report at M2); the one thing SDL3 would have given for free is gamepads, which is ~40 lines of `navigator.getGamepads()` polled per frame into `g_input.held` (M6).

Only the `sfight` profile is registered in this build. `fvipers` and `m2snake` are left out, not hidden: a smaller binary, and no way to load a set that cannot be played online.

### 3.2 The wizard and lobby are HTML, not ImGui

The page draws the wizard and the lobby as DOM over the canvas and talks to the emulator through a small exported C API. This is a decision, and the reasons are all about the brief ("simple, easy to understand"):

- **The Twitch link must be a real `<a target="_blank" rel="noopener noreferrer">`.** The activation URL arrives from the server *after* the click, asynchronously. `window.open` called later from a `requestAnimationFrame` callback is outside the user gesture and gets eaten by popup blockers (Safari reliably, Chrome after ~5 s). A link the player clicks never is. The CLAUDE.md rule still holds and is enforced in C before the URL ever reaches the page: literal `https://` prefix or it is not shown.
- Text entry, copy/paste of the device code, screen readers, password managers and phone keyboards all work in DOM and none work in an ImGui canvas.
- It removes ImGui + cimgui + the Python codegen from the web build outright — the single largest footprint cut available.

The API already has the right shape, because `netplay_window.h` was written to it: *"Draws entirely from a snapshot and never touches netplay state directly: every button posts a command."* The web build exports exactly that:

```c
int         web_netplay_post(int cmd, const char *json_cfg);   /* -> netplay_post   */
const char *web_netplay_status_json(void);                     /* <- netplay_get_status */
int         web_rom_load(const uint8_t *zip, size_t len);      /* -> §3.5           */
const char *web_rom_report_json(void);                         /* which files, which missing */
```

`netplay_status_t` is large (two room tables and a 64×160 log ring), so the JSON is built from it on demand, a few times a second, not per frame.

### 3.3 Threading: one slice function, single-threaded first

`emu_thread_run_loop`'s RUNNING branch becomes `emu_run_slice(ctx)` — one netplay pump, one slice, no sleeping — and the native thread loop calls it, **behaviour-preserving**. The web frontend calls the same function from `frame_cb`, paced by a time accumulator (not by `rAF` count: displays run at 60, 120, 144 Hz; the board runs at `EMU_SLICES_PER_SEC`).

Why single-threaded first:

- **It deploys to plain GitHub Pages with no tricks.** Threads need `COOP: same-origin` + `COEP: require-corp`, which Pages cannot send. The two workarounds both have a cost: `coi-serviceworker` (a service worker that re-serves every response with the headers) forces a reload on first visit and does not work where service workers are off, e.g. Firefox private windows; and a Cloudflare Transform Rule only works if `play` is proxied through Cloudflare (the apex is — `104.21.31.12` — but proxying a Pages custom domain complicates GitHub's certificate issuance).
- **The budget is there.** See §2: the RK3566 holds 60 fps.
- The emu mutex disappears on the web (no second thread), which also removes the main-thread spin-wait Emscripten turns `pthread_mutex_lock` into.

The cost, stated plainly: **`requestAnimationFrame` stops in a hidden tab**, so a player who tabs away stalls their opponent, and m2-hle2 drops a session that stalls for 15 s. Mitigation: on `visibilitychange`, drive slices from a timer instead (tabs playing audio — this one is — are exempt from Chrome's background timer throttling). If measurement at M2 says one thread is not enough, `emu_run_slice` is already the seam: `-pthread` + `coi-serviceworker` is a build flag and a file, not a redesign.

### 3.4 Network seams

| Seam | Native | Web backend |
|---|---|---|
| `tls.h` | Schannel over TCP | **A `wss://` byte stream to the gateway.** The browser does the TLS and validates the gateway's certificate against the public CA set — this *is* `tls.h`'s VALIDATED mode. No crypto library in the wasm. Certificate pinning does not exist here (the browser gives no access to the peer certificate); the fingerprint field is ignored and hidden. |
| `net_socket.h` UDP | `sendto` / `recvfrom` | **Framed datagrams on a second WebSocket**: `[ip:4][port:2][payload]` each way. `net_udp_open` opens it; `net_udp_recv` pops a ring the `onmessage` handler fills. |
| `net_resolve_ipv4`, `net_local_ipv4_towards` | `getaddrinfo`, routing table | Answered by the gateway in its hello: the signaling address to target, and **this session's virtual local address** (§4.2). |
| `net_now_ms` | `GetTickCount64` | `emscripten_get_now()`. |
| `netplay_open_url` | `ShellExecute` | No-op returning false; the page shows the link (§3.2). |
| `netplay_settings_load/save` | `m2hle_netplay.cfg` | `localStorage`, same keys. |

WebSocket is TCP, so a lost packet stalls everything behind it (head-of-line blocking) where UDP would simply have lost one datagram the lockstep's redundant re-sends already cover. That is the known price of v1. The seam is the datagram API, so WebTransport datagrams (no HOL blocking, but no Safari at the time of writing — check again at M3) or a WebRTC data channel can replace the transport later without touching `lockstep.h`.

### 3.5 ROMs: one zip, matched by CRC, never on our server

The site ships the emulator and nothing else. The player picks (or drops) **one zip**; a merged `sfight` or `schamp` set is the only thing that holds all 23 files, because `sfight` is a MAME *clone* of `schamp`: a split `sfight.zip` lacks the parent's shared data and a split `schamp.zip` lacks `sfight`'s program ROMs. The emulator always runs the `sfight` program (`epr-19001.15` / `epr-19002.16`, identity CRC `72E66A1D`) — both peers must, or the lockstep diverges on the first frame — so a merged set is accepted under either name.

- **Match by CRC32 from the zip's central directory, not by file name.** The directory already stores each entry's CRC, so matching costs no decompression, and it is indifferent to the name of the zip, to subfolders (merged sets put clone files in `sfight/`), and to renamed files. `zip_extract` today matches by base name with `MZ_ZIP_FLAG_IGNORE_PATH`, which in a merged set returns whichever same-named entry comes first.
- **Strict on the web.** Native logs a CRC mismatch at WARN and carries on, because hacked sets are a use case there. Online, a different byte is a desync; the web loader refuses, and says which files are missing in words a player can act on ("this looks like a split set — you need the *merged* one").
- **Read from memory** (`mz_zip_reader_init_mem`) straight out of the picked file's buffer, extracting each ROM directly into its region. Going through Emscripten's MEMFS would hold the 18 MB zip twice.
- **Cache the zip in OPFS/IndexedDB** after a good load, so the second visit is one click. Offer "forget my game file".

### 3.6 Footprint

`arc-s` is the CPU/GPU half. The wasm-specific half:

- Drop ImGui, cimgui, ImGuiFileDialog, imgui_club, MCP bridge, kiosk, debug windows, the two unused profiles (§3.1, §3.2).
- `-Oz`/`-O3` to be chosen by measurement (an interpreter core usually wants `-O3`; the rest `-Os`), `-flto`, `--closure 1`, `-sFILESYSTEM=0` (nothing uses a file once §3.5 and §3.4 land — `log.h` writes to the console), `-sMALLOC=emmalloc`, no exceptions/RTTI (no C++ left in the build at all).
- **Memory:** the ROM regions are 82.5 MB as loaded today (`main_data` alone is 32 MB, 15 MB of which is the same 1 MB image copied 15 times by `rom_region_copy`). Start with a fixed `INITIAL_MEMORY` sized from a measured peak rather than `ALLOW_MEMORY_GROWTH` (growth detaches JS views of the heap). Turning the `main_data` mirrors into region aliases is a board-layer change and is **deliberately not in the first pass**: measure, then decide, and if done it goes through `grade-models` and `match-replay` like any board change.
- `mem_init` clears in place on reset (`mem_region_fresh`) — already true, and it matters more here: no allocator churn at every session start.

---

## 4. The gateway

One small daemon on the RPCN host (`rpcn.sonicthefighte.rs`, `143.198.49.181`). It lives in this repo under `web/gateway/` but is **not** deployed by the Pages workflow — GitHub Pages cannot run it, and it needs a real certificate for `wss://` (Caddy or nginx + Let's Encrypt in front is the simple way). Node is the default choice because `tools/` is already Node; nothing about the design depends on it.

### 4.1 Two channels per player

- **Stream:** `wss://…/rpcn` → one TLS connection to RPCN `:31313`. Bytes in, bytes out. The upstream is **fixed in the gateway's config**; the client cannot name a host. An open WebSocket-to-TCP proxy is an abuse vector within hours of being found.
- **Datagram:** `wss://…/udp` → one UDP socket per session, from a fixed, firewalled port range. Frames are `[ip:4][port:2][payload]`.

Because the gateway terminates the browser's TLS, it sees the login token in the clear. The gateway operator is the RPCN operator, so no new party learns anything — but it is why the gateway must run on a host we own and never on a third-party tunnel.

### 4.2 Three things that are silently wrong if done the obvious way

- **The gateway must reach RPCN by its public address, never `localhost`.** RPCN records a player's address from what it sees. Over loopback every browser player is `127.0.0.1`, and a native opponent is told to punch at `127.0.0.1`. *(RPCN's exact rule — TCP peer address vs UDP source — was not verified from its source here; verify at M3 against the real server before trusting this paragraph.)*
- **Two browser players share the gateway's public IPv4, which is the case CLAUDE.md already documents:** RPCN hands each the other's *local* address with port 3658 hardcoded. So the gateway gives every session a **virtual local address** from a private pool, reports it as `local_ip` in the signaling keepalive, and routes any datagram addressed to `pool-address:3658` internally, session to session, with no UDP involved. Browser-vs-browser then never leaves the gateway; browser-vs-native goes out the session's real UDP socket, and the native client needs no change at all.
- **A relay that forwards to any address is a reflector and an SSRF hole.** Allow: the RPCN signaling address, the virtual pool, and public unicast. Refuse: loopback, RFC 1918, link-local, multicast, ports below 1024. Cap datagrams per second and sessions per source IP. Check `Origin: https://play.sonicthefighte.rs` on upgrade.

Also: every browser player reaches RPCN from one IP, so any per-IP limit RPCN applies (sign-up rate, bans) applies to all of them at once.

---

## 5. The wizard and the lobby

Three steps, one visible at a time, each with one obvious button. Everything the native window exposes that a player does not need — server, port, certificate fingerprint, e-mail token, classic account, YAMP's room list, the log — is gone or behind an "Advanced" disclosure (frame delay, private-match password).

1. **Add your game.** Drop zone + file button. On success: "Sonic the Fighters — ready", and the game boots into attract behind the panel. On failure: which files are missing and what kind of set that means (§3.5).
2. **Sign in with Twitch.** One button. Then the code, large, with a Copy button, and an **Open Twitch** link (§3.2): "Check the code on Twitch matches this one, then approve." Then "Waiting for Twitch…" with Cancel. Then "Signed in as *name*". A returning player with a good token skips this step — that is `netplay_twitch_reuse`, already built. The states map one-to-one onto `rpcn_twitch_state_t`; `TwitchAuthPending` and `TwitchAuthSlowDown` are not errors and must not end the flow, and **the flow must stay on one connection** — so the gateway must not recycle the upstream mid-flow.
3. **Find a match.** A list of open matches ("*name* is waiting — **Play**") and one big **Create a match**. In a room: "Waiting for an opponent…" → "*name* joined — **Start match**" → or, when the other side pressed first, "*name* is ready — **Accept**" (`peer_ready`, the challenge latch). One line sets expectations: "Both games restart together when the match begins" (the cold-boot barrier). While playing: a small connection indicator from `stalls`, and a plain message if `desync_frame` latches.

Rooms whose `flagAttr` fails `netplay_room_reject_reason` are shown greyed with the reason, never hidden: "nobody is online" and "someone is online on an incompatible version" are different answers.

---

## 6. Deploy

`.github/workflows/pages.yml`, separate from `canary.yml`:

- **Build job** on push to `master` and `wasm`, and on PRs: `mymindstorm/setup-emsdk` pinned to an exact version (a toolchain bump can change float codegen — see M2's gate), submodules `vendor/sokol vendor/miniz` only (no Python step — nothing generates cimgui), `emcmake cmake -DM2HLE_FRONTEND=web`, assemble `site/` = `index.html` + JS/CSS + `m2hle.js` + `m2hle.wasm` + `CNAME`.
- **Deploy job** on `master` only (plus `workflow_dispatch`): `actions/upload-pages-artifact` → `actions/deploy-pages`. No `gh-pages` branch.
- The workflow is **not added until M2 produces something that builds.** A pipeline that fails on every push teaches everyone to ignore it.

Only the repository owner can do these, and the site does not exist until they are done:

- Repo **Settings → Pages → Source: GitHub Actions**; custom domain `play.sonicthefighte.rs`; Enforce HTTPS. (Pages on a private repository needs a paid plan — `gh` is not installed on the dev machine, so visibility was not checked.)
- **DNS:** `play.sonicthefighte.rs` does not resolve today (NXDOMAIN). Add `CNAME play → biggestsonicfan.github.io` at Cloudflare, **DNS-only (grey cloud)** at least until GitHub has issued its certificate.
- To preview from `wasm` before merging, the `github-pages` environment's deployment-branch rule has to allow that branch.
- The gateway host, its firewall range, and its certificate (§4).

---

## 7. Milestones

Each ends on something measured. Native builds for verification go in a **fresh build directory, never `build_vs22`** — live processes run out of it.

- **M0 — done.** Branch, analysis, this document.
- **M1 — done (`dd6f3cb`).** Gate as run: MSVC build of every target, `ctest` 6/6, D3D11 launched (HLSL compiles, runs), and `arc_bench --draw-digest` identical with and without the mesh cache over 4,531 frames / 324,749 cache hits. Not run: `--verify-atlas`, `grade-models`, `match-replay` (they need the MCP bridge on a machine whose default port is not in use), and the GL shaders on a native GPU. *Original scope:* Resolve §2's five hunks; port `8391e6b` into the cached decoder and every fill-shader variant, GLSL and HLSL. *Gate:* MSVC build + `ctest`; `arc_bench --draw-digest` identical with and without the mesh cache; `--verify-atlas`; `node tools/grade-models.mjs`; `node tools/match-replay.mjs`.
- **M2 — boots; gates partly open.** Done: `M2HLE_FRONTEND=web`, `emu_slice_body` / `emu_slice_finish` shared with the native thread (native re-checked: 59.9 fps headless, `ctest` 6/6), `main_web.c`, the in-memory CRC-matched loader, audio init, keyboard, and step 1 of the page. Open: Chrome/Firefox/Safari on real GPUs (headless Edge on SwiftShader, plus one person's browser — which is where the audio lag was found and fixed, section 8), a frame-time number from a modest laptop, and the determinism gate below — see section 8 for where it stands. *Original scope:* emsdk pinned; `M2HLE_FRONTEND=web`; `emu_run_slice` extracted (native histogram unchanged); `main_web.c`; in-memory CRC-matched ROM load; audio; keyboard. *Gates:* attract runs in Chrome, Firefox and Safari; frame-time budget logged on a modest laptop; and **the determinism gate: the wasm build's per-frame check values (`netplay_frame_check`) equal the native build's over the attract replay fight.** This is what cross-play with native clients rests on. The reasons to expect a pass were checked: sin/cos come from the COP data ROM tables, √ / ÷ / atan2 are the firmware ports, wasm has no FMA contraction, and the float→int `0x80000000` case was made explicit for the ARM build. The reasons to measure anyway: musl's libm is not MSVC's, and wasm does not define NaN payload bits.
- **M3 — it reaches RPCN.** Web backends for `tls.h` / `net_socket.h`; the gateway; settings in `localStorage`. *Gate:* a web client and a native client, two accounts, play a session to the end with `desync_frame` clear (the two-client method is already worked out; one side becomes a browser tab). Then web-vs-web through the virtual pool.
- **M4 — wizard and lobby.** §5, against the exported API. *Gate:* someone who has never seen RPCN gets from a blank tab to a match without being told anything.
- **M5 — deploy.** §6.
- **M6 — after it works.** Gamepad; OPFS cache; hidden-tab timer; touch controls; the footprint pass with numbers (wasm size, heap peak); WebTransport/WebRTC if WebSocket latency proves to be the complaint; threads only if M2's budget says so.

---

## 8. Measured so far

Toolchain: Emscripten **6.0.9**, installed beside the repo (`../emsdk`, not on `PATH`). Pin this exact version in `pages.yml`.

**Build.** `emcmake` finds no generator on a stock Windows box; Visual Studio ships a Ninja that works:

```
emcmake cmake -S . -B build_web -G Ninja -DCMAKE_MAKE_PROGRAM="<VS>/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe" -DM2HLE_FRONTEND=web -DCMAKE_BUILD_TYPE=Release
cmake --build build_web          # -> build_web/site/ is the whole website
node tools/web-serve.mjs --rom path/to/merged.zip      # http://localhost:8080/?rom=/dev-rom.zip
node tools/web-smoke.mjs --url "http://localhost:8080/?rom=/dev-rom.zip" --seconds 30 --shot out.png --keys "5@8,1@11"
```

`web-smoke.mjs` drives headless Chrome/Edge over the DevTools protocol in real time (headless "virtual time" skips ahead when the page looks idle, and screenshots a game that has not loaded yet). Without a ROM it still proves WebGL2 came up and every shader compiled, which is the part CI can run.

**Size.** `m2hle.wasm` 410 KB, `m2hle.js` 146 KB (unminified, `-O2`, no LTO, no closure), page 9 KB. Before any footprint pass, and before the network code has a backend.

**Speed** (wasm under Node 24, this machine, while ~10 native emulators were also running on it): one 60 Hz slice of STF **with the sound board = 1.53 ms average, 5.5 ms worst**; the renderer's CPU side ~0.8 ms excluding tile compose (the bench's dummy backend composes tiles on the CPU; WebGL2 uses the GPU compositor). In headless Edge on *software* GL the game holds 60-61 fps. One thread is enough; section 3.3 stands.

**Audio latency — settled by ear, 2026-09-19.** The first listen said "audible, behind the picture"; the second, after shrinking the queue, "still behind, by less"; the third, on the worklet, "sounds and looks good". What each round found, because the first two fixes were real and still not enough:

1. *The queue was the desktop's.* `audio_out.h` held 8,192 frames (186 ms, sized for a bursty emu thread and a live stream). And a browser keeps WebAudio suspended until the first click or key while the board fills its 16,384-frame ring: measured sitting at 16,383 frames (371 ms) of stale audio, which a 1% rate nudge takes ~19 s to work off. Now the host chooses (`audio_out_config_t`), and a ring past three times its target is resynced in one faded skip. Desktop defaults untouched.
2. *The callback was the rest.* sokol_audio's browser backend is a `ScriptProcessorNode`: main-thread, and it **double-buffers, so an N-frame node costs ~2N before the device** (1024 frames = ~46 ms, not 23), on top of a queue that must cover a whole callback. That left ~110 ms plus the device, against a picture ~25 ms late.
3. *So the web build pushes to an `AudioWorklet`* (`web/site/m2hle-audio-worklet.js`; `audio_out_drain` renders a chunk per display frame, the page posts it). The worklet takes 128 frames at a time and holds the only queue: a 40 ms jitter cushion, sized for one dropped display frame (nothing arrives for ~33 ms, then two chunks). It buffers to its target before playing, decays instead of clicking on an underrun, resyncs when stale, and reports its fill so the page can steer the resampler (1% at most). No SharedArrayBuffer needed: chunks go by `postMessage` with the buffer transferred. While the context is suspended nothing is posted — messages would pile up in the port without bound — so sound starts with what is happening now.

Measured in headless Edge: **queue 27-50 ms around a 40 ms target, 0 dropouts, 0 resyncs** over attract, character select and a fight; suspended-until-first-key starts clean. The fallback (no worklet: old browser, insecure context; force it with `?audio=fallback`) is the ScriptProcessor path at 512 frames / 2,048 queued, ~46 ms held. `?debug` shows queue, `baseLatency` and `outputLatency` live — the device's share is the one part no test of ours can see. `?audioms=N` moves the cushion.

**It compiles unchanged.** The whole board layer, the renderer's CPU side and `src/net/` built for wasm with no source changes — `tests/arc_bench.c` to wasm was the first thing tried and it ran. Everything web-specific is in the new files plus four small seams: `emu_ctx_init`, the memory zip source in `rom_loader.h`, no log file under Emscripten, one profile under `M2HLE_WEB`.

**GLSL ES 3.00 has no `ldexp`.** Master's texlod fix (`8391e6b`) used it in `fast_log2`, so that shader could never have compiled on GLES 3 or WebGL2 — nothing had tried, because `arc-s` branched before it. The GLSL fills now read the exponent and mantissa with `floatBitsToInt`, which is what `model2rd.ipp` does to `f2u(z)`, is exact, and exists in both dialects. HLSL unchanged.

**Determinism — the gate is NOT passed, and the failure is small and specific.**

- wasm vs wasm, two runs: i960 instruction histograms over game frames 900-4350 **byte-identical**. The wasm build is deterministic against itself, and WebAssembly defines float results bit-for-bit across machines (NaN payloads excepted), so **web-vs-web netplay stands on the same footing as native-vs-native.**
- wasm vs native x86-64 (gcc 15, glibc, `-O2 -ffp-contract=off`): the same histograms differ at **21 of 24,525 addresses, by 1 to 7 executions — 31 instructions in 134,143,483.** One function around `0x2E04C`-`0x2E0C8` took a branch the other way once; a few two-instruction fragments at `0x1846C`, `0x18508`, `0x18FF8`, `0x1928C`, `0x19344` ran once or seven times more. It did not snowball over 3,450 frames. The sound board is not the cause (identical result with it off) and it is not a host libm call: the only inexact libm calls left in simulation code are six `powf` in SCSP table init (`scsp.h`), and the i960 cannot read those back.
- So a float comparison lands differently between clang/wasm and gcc/x86-64 somewhere in the COP or i960 FP path. **Not yet known: whether MSVC agrees with either** — and MSVC is the native build people actually run. Next step: a per-frame state digest in `arc_bench` (registers + work RAM), find the first differing frame, then diff that frame's COP conversation (`g_dl.cop` already records one). Until then, **web-vs-native cross-play is unproven; web-vs-web is not affected.**

**Still duplicated, on purpose for now:** `tests/arc_bench.c` carries its own copy of the slice body; it should call `emu_slice_body` so the bench measures what ships. `web_install_board` in `main_web.c` repeats `main.c`'s post-load sequence and reset hook; a shared header would hold all three hosts to one copy.
