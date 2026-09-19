/*
 * The object viewer, from the page: window.m2hleObjview.
 *
 * The emulator draws one model on its own, offscreen, from a camera you place,
 * and hands back the PNG (src/ui/objview.h). This is the browser's half of
 * that: the same JSON command vocabulary the desktop build's TCP bridge
 * carries, marshalled across to the wasm exports, and the answers turned back
 * into objects.
 *
 * The difference from the desktop is that nothing here may block: the thread
 * asking is the thread drawing. So a request is armed and the answer collected
 * on a later animation frame, and every call below is async for that reason.
 * A shot batch still renders inside ONE emulator frame -- eight angles is eight
 * passes over geometry decoded once -- so `await shot({count: 8})` costs a
 * frame or two of wall time, not eight.
 *
 * Two ways in:
 *   - a console, or an automation harness over CDP:
 *       await m2hleObjview.waitReady();
 *       const r = await m2hleObjview.shot({ model: 3544, six: true });
 *       r.shots[0].url        // a blob: URL, or .bytes for the raw PNG
 *   - ?objview=N in the address bar, which shows model N on the canvas in
 *     place of the game. Drag to orbit, wheel to zoom, Escape to go back. The
 *     board keeps running behind it.
 */
'use strict';

const m2hleObjview = (() => {
  const raf = () => new Promise((r) => requestAnimationFrame(r));

  const live = () => typeof Module !== 'undefined' && !!Module._web_objview;

  /* A JS string into the heap and back out again. ccall is not among the
   * runtime methods this build exports, so the marshalling is by hand. */
  function call(json) {
    const s = JSON.stringify(json);
    const n = Module.lengthBytesUTF8(s) + 1;
    const p = Module._malloc(n);
    try {
      Module.stringToUTF8(s, p, n);
      return JSON.parse(Module.UTF8ToString(Module._web_objview(p)));
    } finally {
      Module._free(p);
    }
  }

  /* Raw: one command, one reply, no waiting. Everything else is built on it. */
  function cmd(obj) {
    if (!live()) throw new Error('the emulator is not ready yet');
    return call(obj);
  }

  function status() { return cmd({ cmd: 'objview_status' }); }

  /* Wait until the frame callback has served one pass, so what comes back is
   * this request's result and not the previous one's. `serial` counts passes. */
  async function settle(before, timeoutMs = 4000) {
    const deadline = performance.now() + timeoutMs;
    for (;;) {
      await raf();
      const st = status();
      if (st.serial !== before) return st;
      if (performance.now() > deadline) return { ...st, timedOut: true };
    }
  }

  /*
   * Select the object, place it in 3D space and aim the camera, then wait for
   * one render pass so the reply carries this object's triangle count, bounds
   * and auto-fit distance. See MCP_GUIDE.md for every field; the common ones
   * are { model, capture, use_capture_matrix, yaw, pitch, dist, fov, autofit,
   * fit_margin, width, height, wireframe, textured, cull }.
   *
   * ok:false means the object did not draw, and last_error says why.
   */
  async function set(opts = {}) {
    const before = status().serial;
    cmd({ cmd: 'objview_set', ...opts });
    return settle(before);
  }

  /* How many triangles each model-table entry decodes to. Most of the table is
   * empty in any game, and an empty entry looks like a broken one in a picture. */
  function list(first = 0, count = 64, nonemptyOnly = true) {
    return cmd({ cmd: 'objview_list', first, count, nonempty_only: nonemptyOnly ? 1 : 0 });
  }

  /*
   * Render the object from one or more angles and collect the PNGs.
   *
   *   { six: true }                  the six camera stations: +Z +X -Z -X +Y -Y
   *   { count: N, yaw_step: D }      a turntable; with no step, N spread over a turn
   *   neither                        one shot at the current yaw and pitch
   *
   * Any setting `set` takes may be passed here too, so one call can select the
   * object, place it and shoot it. Each shot comes back with `coverage` (how
   * much of the image is not background) and `box` (where the object drew), so
   * an off-screen or hair-thin result is visible without looking at the image.
   *
   * There is no filesystem here: the bytes live in the emulator's heap until
   * the next batch, and are copied out into a Blob before this returns.
   */
  async function shot(opts = {}) {
    const armed = cmd({ cmd: 'objview_shot', ...opts });
    if (!armed.ok) return armed;

    const timeoutMs = opts.timeout_ms ?? 30000;
    const deadline = performance.now() + timeoutMs;
    let res;
    for (;;) {
      await raf();
      res = JSON.parse(Module.UTF8ToString(Module._web_objview_poll()));
      if (!res.pending) break;
      if (performance.now() > deadline) {
        return { ok: false, error: 'the frame callback did not take the shot in time' };
      }
    }
    if (!res.ok) return res;

    for (const s of res.shots) {
      const ptr = Module._web_objview_png(s.index);
      const len = Module._web_objview_png_len(s.index);
      if (!ptr || !len) { s.bytes = null; continue; }
      /* subarray is a view into a heap that grows and moves; copy it. */
      s.bytes = new Uint8Array(Module.HEAPU8.subarray(ptr, ptr + len));
      s.blob = new Blob([s.bytes], { type: 'image/png' });
      s.url = URL.createObjectURL(s.blob);
    }
    return res;
  }

  /*
   * Wait until the game has built the 3D state the viewer needs.
   *
   * The model table is ROM and readable the moment the zip loads, but what the
   * object is MADE of is not: the texture sheets are filled by the game's own
   * decompressor and the face palette by its colour setup, both during boot. A
   * model decoded before then has the right shape with no texels and no
   * colours, which reads as an artifact and is not one. In STF that lands as
   * attract mode starts.
   */
  async function waitReady(timeoutMs = 60000) {
    const deadline = performance.now() + timeoutMs;
    for (;;) {
      if (live()) {
        const st = status();
        if (st.ready) return st;
        if (performance.now() > deadline) return { ...st, timedOut: true };
      } else if (performance.now() > deadline) {
        return { ok: false, ready: false, timedOut: true, error: 'the emulator never started' };
      }
      await raf();
    }
  }

  /* Show the viewer on the canvas in place of the game, or go back to the game.
   * The board keeps running either way. */
  function show(on = true) {
    if (!live()) return false;
    Module._web_objview_show(on ? 1 : 0);
    return !!Module._web_objview_showing();
  }
  const showing = () => live() && !!Module._web_objview_showing();

  /* ---- Orbiting with the mouse, while the viewer is on the canvas ---------- */

  let drag = null;
  function onPointerDown(e) {
    if (!showing()) return;
    drag = { x: e.clientX, y: e.clientY, yaw: status().yaw, pitch: status().pitch };
    e.target.setPointerCapture?.(e.pointerId);
    e.preventDefault();
  }
  function onPointerMove(e) {
    if (!drag) return;
    /* A canvas width's drag is half a turn, which is about right by hand. */
    const w = Math.max(1, e.target.clientWidth || 800);
    const yaw = drag.yaw + ((e.clientX - drag.x) / w) * 360;
    let pitch = drag.pitch - ((e.clientY - drag.y) / w) * 360;
    pitch = Math.max(-89, Math.min(89, pitch));
    cmd({ cmd: 'objview_set', yaw, pitch });
    e.preventDefault();
  }
  function onPointerUp() { drag = null; }
  function onWheel(e) {
    if (!showing()) return;
    const st = status();
    /* Zooming means leaving the auto-framing, which would otherwise put the
     * distance back where it was on the very next pass. */
    cmd({ cmd: 'objview_set', autofit: 0, dist: st.dist * (e.deltaY > 0 ? 1.1 : 1 / 1.1) });
    e.preventDefault();
  }
  function onKey(e) {
    if (e.key === 'Escape' && showing()) { show(false); e.stopImmediatePropagation(); }
  }

  function attach() {
    const c = document.getElementById('canvas');
    if (!c) return;
    c.addEventListener('pointerdown', onPointerDown);
    c.addEventListener('pointermove', onPointerMove);
    c.addEventListener('pointerup', onPointerUp);
    c.addEventListener('pointercancel', onPointerUp);
    c.addEventListener('wheel', onWheel, { passive: false });
    /* Before sokol_app's own window listener, which swallows keys. */
    window.addEventListener('keydown', onKey, true);
  }

  /* Called from Module.onM2hleReady, once the exports and the GL context exist. */
  async function onReady() {
    attach();
    const asked = new URLSearchParams(location.search).get('objview');
    if (asked === null) return;
    await waitReady();
    const model = Number(asked);
    if (Number.isFinite(model) && asked !== '') await set({ model, active: 1 });
    show(true);
  }

  return { cmd, status, set, list, shot, waitReady, show, showing, onReady };
})();
