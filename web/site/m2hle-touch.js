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
 * A twin-stick game (Virtual-On, m2hle-games.js `twin`) gets two d-pads, one per
 * lever, each with its shot and dash beside it, and none of the fighting games'
 * buttons. They press what m2hle-pad.js's twin rows press: the left lever is
 * player 1's directions, B1 and B2, the right lever player 2's (actions 10..15).
 *
 * Everything the player can change is kept in this origin's localStorage under
 * 'm2hle.touch': whether to show them, their size, opacity and outline, and where
 * each one sits, with its own size and whether it is shown at all. Placement is
 * kept per orientation (a phone held upright has its free space below the
 * picture, one held sideways has it at the sides), as fractions of the canvas, so
 * a layout survives a different screen. A twin-stick game's placement is its own
 * ('m2hle.touch.vonj', as the pad's bindings are); the look and "show them" are
 * shared.
 *
 * ?touch=on / ?touch=off overrides "show them" for one visit, for testing.
 */
'use strict';

/* The words (m2hle-lang.js). The buttons' names are m2hle-keys.js's keys.act.*. */
m2hleLang.add({
  en: {
    'touch.dpad': 'D-pad',
    'touch.lever.left': 'Left lever',
    'touch.lever.right': 'Right lever',
    'touch.shot': 'Shot',
    'touch.dash': 'Dash',
    'touch.landscape': 'This is the layout for holding it sideways.',
    'touch.portrait': 'This is the layout for holding it upright.',
    'touch.pick': 'Tap a button',
    'touch.show': 'Show',
    'touch.hide': 'Hide',
  },
  ja: {
    'touch.dpad': '十字キー',
    'touch.lever.left': '左レバー',
    'touch.lever.right': '右レバー',
    'touch.shot': 'ショット',
    'touch.dash': 'ダッシュ',
    'touch.landscape': 'これは横向きで持つときの配置です。',
    'touch.portrait': 'これは縦向きで持つときの配置です。',
    'touch.pick': 'ボタンをタップしてください',
    'touch.show': '表示',
    'touch.hide': '非表示',
  },
});

/* Where each control sits: its centre as a fraction of the canvas, a size of its
 * own (1 = the global size) and whether it is shown. Sideways, the picture fills
 * the height and leaves bars at the sides; upright, it sits in the middle with
 * room below. STF plays on three buttons, so Button 4 starts hidden, and so
 * do the macros: the editor shows hidden buttons, and "Show" puts one back.
 * P+K takes Button 4's place, which STF does not use. */
const M2HLE_TOUCH_LAYOUTS = {
  solo: {
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
  },
  /* Twin sticks: a lever under each thumb, its shot above it on the outside (the
   * lever's trigger, under the index finger on a cabinet) and its dash on the
   * inside (the thumb button), mirrored left and right. Sideways, Start sits
   * beside Coin: the page's buttons take the top right corner. */
  twin: {
    landscape: {
      dpad:  { x: 0.12, y: 0.70, s: 1, on: true },
      rpad:  { x: 0.88, y: 0.70, s: 1, on: true },
      b1:    { x: 0.06, y: 0.34, s: 1, on: true },
      b2:    { x: 0.19, y: 0.40, s: 1, on: true },
      rshot: { x: 0.94, y: 0.34, s: 1, on: true },
      rdash: { x: 0.81, y: 0.40, s: 1, on: true },
      start: { x: 0.17, y: 0.10, s: 1, on: true },
      coin:  { x: 0.06, y: 0.10, s: 1, on: true },
    },
    portrait: {
      dpad:  { x: 0.23, y: 0.87, s: 1, on: true },
      rpad:  { x: 0.77, y: 0.87, s: 1, on: true },
      b1:    { x: 0.10, y: 0.71, s: 1, on: true },
      b2:    { x: 0.30, y: 0.71, s: 1, on: true },
      rshot: { x: 0.90, y: 0.71, s: 1, on: true },
      rdash: { x: 0.70, y: 0.71, s: 1, on: true },
      start: { x: 0.85, y: 0.12, s: 1, on: true },
      coin:  { x: 0.15, y: 0.12, s: 1, on: true },
    },
  },
};

const m2hleTouch = (() => {
  const $ = (id) => document.getElementById(id);
  const TWIN = !!m2hleGame.twin;
  const STORE = 'm2hle.touch';
  const LAYOUT_STORE = TWIN ? STORE + '.' + m2hleGame.id : STORE;

  /* `act` is the GAME_INPUT_* index for player 1 (src/core/game_profile.h, and
   * m2hle-pad.js's ACTIONS), `acts` a macro's: one button that holds several.
   * A d-pad's `dirs` are the actions of its up, down, left and right.
   * `base` is the diameter in CSS pixels at 100%. */
  const SOLO_CONTROLS = [
    { id: 'dpad',  label: m2hleLang.t('touch.dpad'), dirs: [0, 1, 2, 3], base: 150 },
    { id: 'b1',    act: 4, base: 68 },
    { id: 'b2',    act: 5, base: 68 },
    { id: 'b3',    act: 6, base: 68 },
    { id: 'b4',    act: 7, base: 68, text: '4' },
    { id: 'start', act: 8, base: 50 },
    { id: 'coin',  act: 9, base: 50 },
    { id: 'pk',    label: 'P + K',     acts: [4, 5],    base: 56, text: 'P+K' },
    { id: 'pb',    label: 'P + B',     acts: [4, 6],    base: 56, text: 'P+B' },
    { id: 'kb',    label: 'K + B',     acts: [5, 6],    base: 56, text: 'K+B' },
    { id: 'pkb',   label: 'P + K + B', acts: [4, 5, 6], base: 56, text: 'PKB' },
  ];
  /* A twin-stick game: m2hle-pad.js's twin rows, the right lever being player 2's
   * port. `face` is the word on the button; its name is the game's (`labels`). */
  const TWIN_CONTROLS = [
    { id: 'dpad',  label: m2hleLang.t('touch.lever.left'), dirs: [0, 1, 2, 3], base: 140 },
    { id: 'rpad',  label: m2hleLang.t('touch.lever.right'), dirs: [10, 11, 12, 13], base: 140 },
    { id: 'b1',    act: 4,  base: 64, face: 'touch.shot' },
    { id: 'b2',    act: 5,  base: 64, face: 'touch.dash' },
    { id: 'rshot', act: 14, base: 64, face: 'touch.shot' },
    { id: 'rdash', act: 15, base: 64, face: 'touch.dash' },
    { id: 'start', act: 8,  base: 50 },
    { id: 'coin',  act: 9,  base: 50 },
  ];
  const CONTROLS = TWIN ? TWIN_CONTROLS : SOLO_CONTROLS;
  for (const c of CONTROLS) prepare(c);

  /* A control's name, face and the actions it holds. */
  function prepare(c) {
    if (!c.label) c.label = (TWIN && m2hleGame.labels[c.id]) || m2hleLang.t('keys.act.' + c.id);
    if (c.face) c.face = m2hleLang.t(c.face);
    c.mask = (c.acts || c.dirs || (c.act === undefined ? [] : [c.act])).reduce((m, i) => m | (1 << i), 0);
  }
  const UP = 1 << 0, DOWN = 1 << 1, LEFT = 1 << 2, RIGHT = 1 << 3;
  /* 45-degree sectors from atan2 with y pointing down: right, down-right, down, ... */
  const SECTORS = [RIGHT, DOWN | RIGHT, DOWN, DOWN | LEFT, LEFT, UP | LEFT, UP, UP | RIGHT];
  const DEAD = 0.2;           /* of the d-pad's radius: a thumb resting in the middle holds nothing */
  const SLOP = 1.1;           /* a button answers a little outside its circle */

  const DEFAULTS = TWIN ? M2HLE_TOUCH_LAYOUTS.twin : M2HLE_TOUCH_LAYOUTS.solo;
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
  const ptrs = new Map();     /* pointerId -> {stick: its d-pad or null, x, y} in canvas coordinates */
  let area = { left: 0, top: 0, width: 1, height: 1 };
  let mask = 0;

  /* ---- Settings --------------------------------------------------------------- */

  const stored = (key) => {
    const s = JSON.parse(localStorage.getItem(key) || 'null');
    return s && s.v === 1 ? s : null;
  };

  function load() {
    try {
      const s = stored(STORE);
      if (s) loadLook(s);
      const l = TWIN ? stored(LAYOUT_STORE) : s;
      if (l) loadLayouts(l.layouts);
    } catch (e) { /* private mode, or a value from somewhere else: keep the defaults */ }
  }

  function loadLook(s) {
    if (['auto', 'on', 'off'].includes(s.show)) cfg.show = s.show;
    if (typeof s.vibrate === 'boolean') cfg.vibrate = s.vibrate;
    if (typeof s.outline === 'boolean') cfg.outline = s.outline;
    if (Number.isFinite(s.size)) cfg.size = clamp(s.size, 0.5, 2);
    if (Number.isFinite(s.opacity)) cfg.opacity = clamp(s.opacity, 0.1, 1);
  }

  function loadLayouts(layouts) {
    for (const o of Object.keys(DEFAULTS)) {
      for (const c of CONTROLS) {
        const p = layouts && layouts[o] && layouts[o][c.id];
        if (!p) continue;
        const d = cfg.layouts[o][c.id];
        if (Number.isFinite(p.x)) d.x = clamp(p.x, 0, 1);
        if (Number.isFinite(p.y)) d.y = clamp(p.y, 0, 1);
        if (Number.isFinite(p.s)) d.s = clamp(p.s, 0.5, 2);
        if (typeof p.on === 'boolean') d.on = p.on;
      }
    }
  }

  /* A twin-stick game writes its layout to its own key, and the look to the
   * shared one beside the fighting games' layout, which it leaves as it was. */
  function save() {
    try {
      if (!TWIN) { localStorage.setItem(STORE, JSON.stringify({ v: 1, ...cfg })); return; }
      const { layouts, ...look } = cfg, old = stored(STORE);
      localStorage.setItem(STORE, JSON.stringify({ v: 1, ...look, layouts: old ? old.layouts : undefined }));
      localStorage.setItem(LAYOUT_STORE, JSON.stringify({ v: 1, layouts }));
    } catch (e) { /* private mode */ }
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
      el.className = 'tb' + (c.dirs ? ' dpad' : '');
      el.dataset.id = c.id;
      if (c.dirs) {
        for (const d of ['up', 'right', 'down', 'left']) {
          const a = document.createElement('span');
          a.className = 'arr arr-' + d;
          el.appendChild(a);
        }
      } else {
        el.textContent = c.text || c.face || c.label;
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

  /* The d-pad's actions for where the finger is: UP..RIGHT name its dirs[0..3]. */
  function dpadBits(c, pt) {
    const { x, y, r } = centre(c);
    const dx = pt.x - x, dy = pt.y - y;
    if (Math.hypot(dx, dy) < DEAD * r) return 0;
    const s = SECTORS[(Math.round(Math.atan2(dy, dx) / (Math.PI / 4)) + 8) % 8];
    return c.dirs.reduce((m, a, i) => (s & (1 << i) ? m | (1 << a) : m), 0);
  }

  function buttonBits(pt) {
    let m = 0;
    for (const c of CONTROLS) {
      if (c.dirs || !layout()[c.id].on) continue;
      const { x, y, r } = centre(c);
      if (Math.hypot(pt.x - x, pt.y - y) <= r * SLOP) m |= c.mask;
    }
    return m;
  }

  function update() {
    let m = 0;
    for (const p of ptrs.values()) m |= p.stick ? dpadBits(p.stick, p) : buttonBits(p);
    if (m === mask) return;
    const pressed = m & ~mask;
    mask = m;
    /* A buzz for each button that goes down, and for the d-pad changing direction:
     * the only way to feel a press on glass. */
    if (pressed && cfg.vibrate && navigator.vibrate) navigator.vibrate(10);
    for (const c of CONTROLS) {
      if (!c.dirs) { els[c.id].classList.toggle('down', (m & c.mask) === c.mask); continue; }
      /* The arrows are built up, right, down, left. */
      const arr = els[c.id].children;
      arr[0].classList.toggle('down', !!(m & (1 << c.dirs[0])));
      arr[1].classList.toggle('down', !!(m & (1 << c.dirs[3])));
      arr[2].classList.toggle('down', !!(m & (1 << c.dirs[1])));
      arr[3].classList.toggle('down', !!(m & (1 << c.dirs[2])));
    }
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
    const pt = local(e), c = CONTROLS.find((k) => k.id === el.dataset.id);
    if (editing) {
      const { x, y } = centre(c);
      selected = c.id;
      drag = { id: c.id, pid: e.pointerId, ox: pt.x - x, oy: pt.y - y };
      render();
      return;
    }
    ptrs.set(e.pointerId, { stick: c.dirs ? c : null, ...pt });
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
    $('te-orient').textContent = m2hleLang.t('touch.' + orient());
    $('te-size').value = Math.round(cfg.size * 100);
    $('te-opacity').value = Math.round(cfg.opacity * 100);
    $('te-outline').checked = cfg.outline;
    $('te-sel').textContent = c ? c.label : m2hleLang.t('touch.pick');
    $('te-one').disabled = $('te-hide').disabled = !c;
    $('te-one').value = c ? Math.round(layout()[c.id].s * 100) : 100;
    $('te-hide').textContent = m2hleLang.t(c && !layout()[c.id].on ? 'touch.show' : 'touch.hide');
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
