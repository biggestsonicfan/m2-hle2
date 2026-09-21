/*
 * Touch buttons: a d-pad and the arcade buttons, drawn over the game canvas.
 *
 * Each press is one bit per GAME_INPUT_* action, the same as a gamepad's, and goes
 * to the emulator through m2hle-pad.js (setTouch), which owns the one channel there
 * is (web_pad_set). They play player 1, which is also the local player under
 * netplay (netplay.h, netplay_sample_local).
 *
 * The d-pad is one control with eight directions, read from where the finger is
 * relative to its centre, and it keeps the finger that landed on it wherever that
 * finger goes. A finger that lands on a button slides: it holds whichever buttons
 * it is over, so rolling a thumb from Punch to Kick works as it does on a stick.
 *
 * Everything the player can change is kept in this origin's localStorage under
 * 'm2hle.touch': whether to show them, their size, opacity and outline, and where
 * each one sits, with its own size and whether it is shown at all. Placement is
 * kept per orientation (a phone held upright has its free space below the
 * picture, one held sideways has it at the sides), as fractions of the canvas, so
 * a layout survives a different screen.
 *
 * ?touch=on / ?touch=off overrides "show them" for one visit, for testing.
 */
'use strict';

const m2hleTouch = (() => {
  const $ = (id) => document.getElementById(id);
  const STORE = 'm2hle.touch';

  /* `act` is the GAME_INPUT_* index for player 1 (src/core/game_profile.h, and
   * m2hle-pad.js's ACTIONS), `acts` a macro's: one button that holds several.
   * `base` is the diameter in CSS pixels at 100%. */
  const CONTROLS = [
    { id: 'dpad',  label: 'D-pad',    base: 150 },
    { id: 'b1',    label: 'Punch',    act: 4, base: 68 },
    { id: 'b2',    label: 'Kick',     act: 5, base: 68 },
    { id: 'b3',    label: 'Barrier',  act: 6, base: 68 },
    { id: 'b4',    label: 'Button 4', act: 7, base: 68, text: '4' },
    { id: 'start', label: 'Start',    act: 8, base: 50 },
    { id: 'coin',  label: 'Coin',     act: 9, base: 50 },
    { id: 'pk',    label: 'P + K',     acts: [4, 5],    base: 56, text: 'P+K' },
    { id: 'pb',    label: 'P + B',     acts: [4, 6],    base: 56, text: 'P+B' },
    { id: 'kb',    label: 'K + B',     acts: [5, 6],    base: 56, text: 'K+B' },
    { id: 'pkb',   label: 'P + K + B', acts: [4, 5, 6], base: 56, text: 'PKB' },
  ];
  for (const c of CONTROLS) c.mask = (c.acts || (c.act === undefined ? [] : [c.act])).reduce((m, i) => m | (1 << i), 0);
  const UP = 1 << 0, DOWN = 1 << 1, LEFT = 1 << 2, RIGHT = 1 << 3;
  /* 45-degree sectors from atan2 with y pointing down: right, down-right, down, ... */
  const SECTORS = [RIGHT, DOWN | RIGHT, DOWN, DOWN | LEFT, LEFT, UP | LEFT, UP, UP | RIGHT];
  const DEAD = 0.2;           /* of the d-pad's radius: a thumb resting in the middle holds nothing */
  const SLOP = 1.1;           /* a button answers a little outside its circle */

  /* Where each control sits: its centre as a fraction of the canvas, a size of its
   * own (1 = the global size) and whether it is shown. Sideways, the picture fills
   * the height and leaves bars at the sides; upright, it sits in the middle with
   * room below. STF plays on three buttons, so Button 4 starts hidden, and so
   * do the macros: the editor shows hidden buttons, and "Show" puts one back.
   * P+K takes Button 4's place, which STF does not use. */
  const DEFAULTS = {
    landscape: {
      dpad:  { x: 0.13, y: 0.66, s: 1, on: true },
      b1:    { x: 0.82, y: 0.80, s: 1, on: true },
      b2:    { x: 0.93, y: 0.64, s: 1, on: true },
      b3:    { x: 0.82, y: 0.48, s: 1, on: true },
      b4:    { x: 0.93, y: 0.32, s: 1, on: false },
      start: { x: 0.94, y: 0.10, s: 1, on: true },
      coin:  { x: 0.06, y: 0.10, s: 1, on: true },
      pk:    { x: 0.93, y: 0.32, s: 1, on: false },
      pb:    { x: 0.71, y: 0.80, s: 1, on: false },
      kb:    { x: 0.71, y: 0.60, s: 1, on: false },
      pkb:   { x: 0.82, y: 0.16, s: 1, on: false },
    },
    portrait: {
      dpad:  { x: 0.25, y: 0.86, s: 1, on: true },
      b1:    { x: 0.66, y: 0.92, s: 1, on: true },
      b2:    { x: 0.86, y: 0.84, s: 1, on: true },
      b3:    { x: 0.66, y: 0.78, s: 1, on: true },
      b4:    { x: 0.86, y: 0.745, s: 1, on: false },
      start: { x: 0.85, y: 0.12, s: 1, on: true },
      coin:  { x: 0.15, y: 0.12, s: 1, on: true },
      pk:    { x: 0.86, y: 0.745, s: 1, on: false },
      pb:    { x: 0.49, y: 0.93, s: 1, on: false },
      kb:    { x: 0.49, y: 0.78, s: 1, on: false },
      pkb:   { x: 0.86, y: 0.64, s: 1, on: false },
    },
  };
  const APPEARANCE = { size: 1, opacity: 0.5, outline: true };

  const clone = (o) => JSON.parse(JSON.stringify(o));
  const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));

  let cfg = { show: 'auto', vibrate: true, ...APPEARANCE, layouts: clone(DEFAULTS) };
  const override = new URLSearchParams(location.search).get('touch');
  let touchSeen = matchMedia('(pointer: coarse)').matches;
  let gameOn = false;
  let editing = false;
  let selected = null;        /* the control id the editor's second row acts on */
  let drag = null;            /* {id, pid, ox, oy} while a control is being moved */
  const els = {};             /* control id -> its element */
  const ptrs = new Map();     /* pointerId -> {dpad: bool, x, y} in canvas coordinates */
  let area = { left: 0, top: 0, width: 1, height: 1 };
  let mask = 0;

  /* ---- Settings --------------------------------------------------------------- */

  function load() {
    try {
      const s = JSON.parse(localStorage.getItem(STORE) || 'null');
      if (!s || s.v !== 1) return;
      if (['auto', 'on', 'off'].includes(s.show)) cfg.show = s.show;
      if (typeof s.vibrate === 'boolean') cfg.vibrate = s.vibrate;
      if (typeof s.outline === 'boolean') cfg.outline = s.outline;
      if (Number.isFinite(s.size)) cfg.size = clamp(s.size, 0.5, 2);
      if (Number.isFinite(s.opacity)) cfg.opacity = clamp(s.opacity, 0.1, 1);
      for (const o of Object.keys(DEFAULTS)) {
        for (const c of CONTROLS) {
          const p = s.layouts && s.layouts[o] && s.layouts[o][c.id];
          if (!p) continue;
          const d = cfg.layouts[o][c.id];
          if (Number.isFinite(p.x)) d.x = clamp(p.x, 0, 1);
          if (Number.isFinite(p.y)) d.y = clamp(p.y, 0, 1);
          if (Number.isFinite(p.s)) d.s = clamp(p.s, 0.5, 2);
          if (typeof p.on === 'boolean') d.on = p.on;
        }
      }
    } catch (e) { /* private mode, or a value from somewhere else: keep the defaults */ }
  }

  function save() {
    try { localStorage.setItem(STORE, JSON.stringify({ v: 1, ...cfg })); } catch (e) { /* private mode */ }
  }

  /* ---- Layout ----------------------------------------------------------------- */

  const orient = () => (area.width >= area.height ? 'landscape' : 'portrait');
  const layout = () => cfg.layouts[orient()];
  const diameter = (c) => c.base * cfg.size * layout()[c.id].s;

  /* The control's centre in canvas pixels, kept whole on the canvas. */
  function centre(c) {
    const p = layout()[c.id], r = diameter(c) / 2;
    const fit = (v, len) => (len <= 2 * r ? len / 2 : clamp(v * len, r, len - r));
    return { x: fit(p.x, area.width), y: fit(p.y, area.height), r };
  }

  function visible() {
    const show = override === 'on' || override === 'off' ? override : cfg.show;
    return editing || (gameOn && (show === 'on' || (show === 'auto' && touchSeen)));
  }

  function build() {
    const root = $('touch');
    for (const c of CONTROLS) {
      const el = document.createElement('div');
      el.className = 'tb' + (c.id === 'dpad' ? ' dpad' : '');
      el.dataset.id = c.id;
      if (c.id === 'dpad') {
        for (const d of ['up', 'right', 'down', 'left']) {
          const a = document.createElement('span');
          a.className = 'arr arr-' + d;
          el.appendChild(a);
        }
      } else {
        el.textContent = c.text || c.label;
      }
      root.appendChild(el);
      els[c.id] = el;
    }
  }

  /* Follow the canvas: it shrinks when the tools drawer opens, and turns with the phone. */
  function track() {
    const r = $('canvas').getBoundingClientRect();
    const was = orient();
    area = { left: r.left, top: r.top, width: Math.max(1, r.width), height: Math.max(1, r.height) };
    const root = $('touch');
    root.style.left = r.left + 'px';
    root.style.top = r.top + 'px';
    root.style.width = r.width + 'px';
    root.style.height = r.height + 'px';
    if (was !== orient()) release();
    render();
  }

  function render() {
    const on = visible();
    const root = $('touch');
    root.hidden = !on;
    document.body.classList.toggle('touch-on', on);
    if (!on) return;
    root.classList.toggle('outline', cfg.outline);
    root.classList.toggle('editing', editing);
    root.style.setProperty('--touch-opacity', String(cfg.opacity));
    for (const c of CONTROLS) {
      const el = els[c.id], p = layout()[c.id], { x, y, r } = centre(c);
      el.hidden = !p.on && !editing;
      el.classList.toggle('off', !p.on);
      el.classList.toggle('sel', editing && selected === c.id);
      el.style.width = el.style.height = 2 * r + 'px';
      el.style.left = x - r + 'px';
      el.style.top = y - r + 'px';
      el.style.fontSize = Math.max(10, Math.round(r * (c.text ? 0.7 : 0.36))) + 'px';
    }
    if (editing) renderEditor();
  }

  /* ---- Playing ---------------------------------------------------------------- */

  function local(e) { return { x: e.clientX - area.left, y: e.clientY - area.top }; }

  function dpadBits(pt) {
    const c = CONTROLS[0], { x, y, r } = centre(c);
    const dx = pt.x - x, dy = pt.y - y;
    if (Math.hypot(dx, dy) < DEAD * r) return 0;
    const s = Math.round(Math.atan2(dy, dx) / (Math.PI / 4));
    return SECTORS[(s + 8) % 8];
  }

  function buttonBits(pt) {
    let m = 0;
    for (const c of CONTROLS) {
      if (c.id === 'dpad' || !layout()[c.id].on) continue;
      const { x, y, r } = centre(c);
      if (Math.hypot(pt.x - x, pt.y - y) <= r * SLOP) m |= c.mask;
    }
    return m;
  }

  function update() {
    let m = 0;
    for (const p of ptrs.values()) m |= p.dpad ? dpadBits(p) : buttonBits(p);
    if (m === mask) return;
    const pressed = m & ~mask;
    mask = m;
    /* A buzz for each button that goes down, and for the d-pad changing direction:
     * the only way to feel a press on glass. */
    if (pressed && cfg.vibrate && navigator.vibrate) navigator.vibrate(10);
    for (const c of CONTROLS) {
      if (c.id !== 'dpad') els[c.id].classList.toggle('down', (m & c.mask) === c.mask);
    }
    const arr = els.dpad.children;
    arr[0].classList.toggle('down', !!(m & UP));
    arr[1].classList.toggle('down', !!(m & RIGHT));
    arr[2].classList.toggle('down', !!(m & DOWN));
    arr[3].classList.toggle('down', !!(m & LEFT));
    m2hlePad.setTouch(m);
  }

  function release() {
    ptrs.clear();
    mask = -1;                /* force the update below to send */
    update();
  }

  function onDown(e) {
    const el = e.target.closest('.tb');
    if (!el) return;
    e.preventDefault();       /* no focus change, text selection or emulated mouse */
    if (e.pointerType === 'touch') touchSeen = true;
    try { el.setPointerCapture(e.pointerId); } catch (err) { /* already gone */ }
    const pt = local(e);
    if (editing) {
      const c = CONTROLS.find((k) => k.id === el.dataset.id), { x, y } = centre(c);
      selected = c.id;
      drag = { id: c.id, pid: e.pointerId, ox: pt.x - x, oy: pt.y - y };
      render();
      return;
    }
    ptrs.set(e.pointerId, { dpad: el.dataset.id === 'dpad', ...pt });
    update();
  }

  function onMove(e) {
    if (drag && drag.pid === e.pointerId) {
      const pt = local(e), p = layout()[drag.id];
      p.x = clamp((pt.x - drag.ox) / area.width, 0, 1);
      p.y = clamp((pt.y - drag.oy) / area.height, 0, 1);
      render();
      return;
    }
    const p = ptrs.get(e.pointerId);
    if (!p) return;
    Object.assign(p, local(e));
    update();
  }

  function onUp(e) {
    if (drag && drag.pid === e.pointerId) {
      /* Store where it was drawn, not where the finger let go past an edge. */
      const c = CONTROLS.find((k) => k.id === drag.id), { x, y } = centre(c), p = layout()[drag.id];
      p.x = x / area.width;
      p.y = y / area.height;
      drag = null;
      save();
      render();
      return;
    }
    if (ptrs.delete(e.pointerId)) update();
  }

  /* ---- The editor ------------------------------------------------------------- */

  function renderEditor() {
    const c = CONTROLS.find((k) => k.id === selected);
    $('te-orient').textContent = orient() === 'landscape' ? 'This is the layout for holding it sideways.'
                                                          : 'This is the layout for holding it upright.';
    $('te-size').value = Math.round(cfg.size * 100);
    $('te-opacity').value = Math.round(cfg.opacity * 100);
    $('te-outline').checked = cfg.outline;
    $('te-sel').textContent = c ? c.label : 'Tap a button';
    $('te-one').disabled = $('te-hide').disabled = !c;
    $('te-one').value = c ? Math.round(layout()[c.id].s * 100) : 100;
    $('te-hide').textContent = c && !layout()[c.id].on ? 'Show' : 'Hide';
  }

  function edit(on) {
    if (on === editing) return;
    editing = on;
    release();
    drag = null;
    selected = null;
    if (on && !$('controls').hidden) m2hlePad.toggle(false);
    $('touch-edit').hidden = !on;
    if (!on) { save(); $('canvas').focus(); }
    render();
  }

  function init() {
    load();
    build();

    const root = $('touch');
    root.addEventListener('pointerdown', onDown);
    root.addEventListener('pointermove', onMove);
    root.addEventListener('pointerup', onUp);
    root.addEventListener('pointercancel', onUp);
    root.addEventListener('lostpointercapture', onUp);
    root.addEventListener('contextmenu', (e) => e.preventDefault());

    /* "On touch screens" also means a laptop's touch screen, from its first touch. */
    window.addEventListener('pointerdown', (e) => {
      if (e.pointerType === 'touch' && !touchSeen) { touchSeen = true; render(); }
    }, true);
    /* A backgrounded tab never hears the fingers lift. */
    document.addEventListener('visibilitychange', () => { if (document.hidden) release(); });
    window.addEventListener('blur', release);

    new ResizeObserver(track).observe($('canvas'));
    window.addEventListener('resize', track);
    track();

    /* The Controls panel's section. */
    $('touch-show').value = cfg.show;
    $('touch-show').addEventListener('change', (e) => { cfg.show = e.target.value; save(); render(); });
    if (navigator.vibrate) {
      $('touch-vibrate-row').hidden = false;
      $('touch-vibrate').checked = cfg.vibrate;
      $('touch-vibrate').addEventListener('change', (e) => { cfg.vibrate = e.target.checked; save(); });
    }
    $('touch-customize').addEventListener('click', () => edit(true));
    /* On a phone this is what the panel is for; a controller comes second. */
    if (touchSeen) $('controls').insertBefore($('touch-section'), $('pad-section'));

    /* The editor. Settings apply as the sliders move and are saved when it closes. */
    $('te-size').addEventListener('input', (e) => { cfg.size = e.target.value / 100; render(); });
    $('te-opacity').addEventListener('input', (e) => { cfg.opacity = e.target.value / 100; render(); });
    $('te-outline').addEventListener('change', (e) => { cfg.outline = e.target.checked; render(); });
    $('te-one').addEventListener('input', (e) => {
      if (selected) { layout()[selected].s = e.target.value / 100; render(); }
    });
    $('te-hide').addEventListener('click', () => {
      if (selected) { layout()[selected].on = !layout()[selected].on; render(); }
    });
    $('te-reset').addEventListener('click', () => {
      cfg.layouts[orient()] = clone(DEFAULTS[orient()]);
      selected = null;
      render();
    });
    $('te-move').addEventListener('click', () => $('touch-edit').classList.toggle('bottom'));
    $('te-done').addEventListener('click', () => edit(false));
    $('touch-edit').addEventListener('keydown', (e) => { if (e.key === 'Escape') edit(false); });
  }

  /* Called by m2hle-page.js once a game is running: before that there is nothing to press. */
  function onGame() {
    gameOn = true;
    render();
  }

  document.addEventListener('DOMContentLoaded', init);

  /* Where a control is drawn, in page coordinates (null if it is not on screen).
   * tools/web-smoke.mjs --taps aims with it. */
  function where(id) {
    const c = CONTROLS.find((k) => k.id === id);
    if (!c || $('touch').hidden || els[id].hidden) return null;
    const { x, y, r } = centre(c);
    return { x: area.left + x, y: area.top + y, r };
  }

  return { onGame, edit, where, get mask() { return mask; }, get settings() { return clone(cfg); } };
})();
