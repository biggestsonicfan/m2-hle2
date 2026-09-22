/*
 * The keyboard, and the Keyboard section of the Controls panel that maps it.
 *
 * The page reads the keys itself and the emulator never sees them: each key
 * down or up updates one bit per GAME_INPUT_* action, and that mask goes to the
 * emulator with the gamepads' and the touch buttons' through the one channel
 * they share (m2hlePad.setKeys -> web_pad_set). main_web.c no longer maps sokol
 * key codes, so the defaults here are the whole keyboard layout of the web build.
 *
 * A binding is a KeyboardEvent.code, the PHYSICAL key: Z stays in the bottom-left
 * corner on an AZERTY or Dvorak keyboard, as it does in MAME. Names are shown
 * through the browser's layout map where it has one, so that key reads "W" on
 * AZERTY.
 *
 * Kept in this origin's localStorage under 'm2hle.keys'.
 *
 * This script loads BEFORE m2hle-tools.js, so its window capture listener runs
 * first. It has to: the tools listener stops every key aimed at a panel (so
 * typing in the online panel never reaches the game), and "press a key" in the
 * Controls panel is exactly such a key. Anything this script does not take, it
 * leaves for the listeners after it.
 */
'use strict';

const m2hleKeys = (() => {
  const $ = (id) => document.getElementById(id);
  const STORE = 'm2hle.keys';
  const MAX_BINDS = 3;

  /* The rows of the panel; `act` is the player-1 GAME_INPUT_* index (`acts` for a
   * macro), player 2's is 10 further on (src/core/game_profile.h, as in m2hle-pad.js). */
  const ACTIONS = [
    { id: 'up',    label: 'Up',       act: 0 },
    { id: 'down',  label: 'Down',     act: 1 },
    { id: 'left',  label: 'Left',     act: 2 },
    { id: 'right', label: 'Right',    act: 3 },
    { id: 'b1',    label: 'Punch',    act: 4 },
    { id: 'b2',    label: 'Kick',     act: 5 },
    { id: 'b3',    label: 'Barrier',  act: 6 },
    { id: 'b4',    label: 'Button 4', act: 7 },
    { id: 'start', label: 'Start',    act: 8 },
    { id: 'coin',  label: 'Coin',     act: 9 },
    /* Macros: one binding that holds several buttons at once, as the Gems
     * Collection and HD ports offer. Unbound until the player binds them. */
    { id: 'pk',    label: 'P + K',     acts: [4, 5],    macro: true },
    { id: 'pb',    label: 'P + B',     acts: [4, 6],    macro: true },
    { id: 'kb',    label: 'K + B',     acts: [5, 6],    macro: true },
    { id: 'pkb',   label: 'P + K + B', acts: [4, 5, 6], macro: true },
  ];
  for (const a of ACTIONS) a.mask = (a.acts || [a.act]).reduce((m, i) => m | (1 << i), 0);
  const P2_OFFSET = 10;

  /* The desktop build's keys (src/board/input.h), which are MAME's. */
  const DEFAULTS = {
    p1: { up: ['ArrowUp'], down: ['ArrowDown'], left: ['ArrowLeft'], right: ['ArrowRight'],
          b1: ['KeyZ'], b2: ['KeyX'], b3: ['KeyC'], b4: ['KeyV'], start: ['Digit1'], coin: ['Digit5'],
          pk: [], pb: [], kb: [], pkb: [] },
    p2: { up: ['KeyI'], down: ['KeyK'], left: ['KeyJ'], right: ['KeyL'],
          b1: ['Delete'], b2: ['End'], b3: ['PageDown'], b4: ['Home'], start: ['Digit2'], coin: ['Digit6'],
          pk: [], pb: [], kb: [], pkb: [] },
  };

  const clone = (o) => JSON.parse(JSON.stringify(o));
  let binds = clone(DEFAULTS);
  let player = 'p1';           /* the player the panel is showing */
  let listening = null;        /* {player, id} while waiting for a key */
  let mask = 0;                /* actions the keys hold now */
  const down = new Set();      /* codes held, so a key bound twice lets go cleanly */
  let layout = null;           /* navigator.keyboard's layout map, where there is one */

  function load() {
    try {
      const s = JSON.parse(localStorage.getItem(STORE) || 'null');
      if (!s || s.v !== 1 || !s.binds) return;
      for (const p of ['p1', 'p2']) {
        for (const a of ACTIONS) {
          const list = s.binds[p] && s.binds[p][a.id];
          if (Array.isArray(list)) binds[p][a.id] = list.filter((c) => typeof c === 'string' && c.length < 32).slice(0, MAX_BINDS);
        }
      }
    } catch (e) { /* private mode, or a value from somewhere else: keep the defaults */ }
  }

  function save() {
    try { localStorage.setItem(STORE, JSON.stringify({ v: 1, binds })); } catch (e) { /* private mode */ }
  }

  /* ---- Names ------------------------------------------------------------------ */

  const ARROWS = { ArrowUp: '↑', ArrowDown: '↓', ArrowLeft: '←', ArrowRight: '→' };
  function keyName(code) {
    if (ARROWS[code]) return ARROWS[code];
    const mapped = layout && layout.get(code);
    if (mapped && mapped.trim()) return mapped.length === 1 ? mapped.toUpperCase() : mapped;
    let m;
    if ((m = /^Key([A-Z])$/.exec(code))) return m[1];
    if ((m = /^Digit(\d)$/.exec(code))) return m[1];
    if ((m = /^Numpad(.+)$/.exec(code))) return 'Num ' + m[1];
    if (code === 'Space') return 'Space';
    return code.replace(/(Left|Right)$/, ' ($1)').replace(/([a-z])([A-Z])/g, '$1 $2');
  }

  /* ---- The game's keys -------------------------------------------------------- */

  /* Which action bits one key code drives, both players. */
  function bitsOf(code) {
    let m = 0;
    for (const p of ['p1', 'p2']) {
      const shift = p === 'p2' ? P2_OFFSET : 0;
      for (const a of ACTIONS) if (binds[p][a.id].includes(code)) m |= a.mask << shift;
    }
    return m >>> 0;
  }

  function recompute() {
    let m = 0;
    for (const c of down) m |= bitsOf(c);
    if (m !== mask) {
      mask = m >>> 0;
      if (typeof m2hlePad === 'object') m2hlePad.setKeys(mask);
    }
  }

  /* Let go of everything: focus has gone somewhere the key-ups will not come from. */
  function release() {
    if (!down.size && !mask) return;
    down.clear();
    recompute();
  }

  /* Keys aimed at the page's own panels are theirs (and m2hle-tools.js keeps them
   * from the emulator); so are shortcuts with Ctrl, Alt or the system key. */
  const PANELS = '#drawer, #online, #controls, #touch-edit, input, textarea, select';
  const inPanel = (e) => e.target instanceof Element && e.target.closest(PANELS);

  function onKey(e) {
    if (listening && e.type === 'keydown') {
      e.preventDefault();
      e.stopImmediatePropagation();
      if (e.code === 'Escape' || !e.code) { stopListening(); return; }
      const list = binds[listening.player][listening.id];
      if (!list.includes(e.code)) {
        if (list.length >= MAX_BINDS) list.shift();
        list.push(e.code);
        save();
      }
      stopListening();
      return;
    }
    if (listening) { e.preventDefault(); e.stopImmediatePropagation(); return; }   /* the key-up of the captured key */

    if (e.type === 'keyup') {
      /* Let go wherever the key-up lands: a key held into a panel must not stick. */
      if (down.delete(e.code)) recompute();
      return;
    }
    if (inPanel(e) || e.ctrlKey || e.metaKey || e.altKey) return;
    if (!bitsOf(e.code)) return;
    /* A game key: the game has it and the page does not (arrows would scroll it). */
    e.preventDefault();
    if (e.repeat) return;
    down.add(e.code);
    recompute();
  }

  /* ---- The panel -------------------------------------------------------------- */

  function listen(id) {
    if (listening && listening.player === player && listening.id === id) { stopListening(); return; }
    release();
    listening = { player, id };
    render();
  }

  function stopListening() {
    listening = null;
    render();
  }

  function render() {
    const rows = $('key-rows');
    if (!rows || $('controls').hidden) return;
    for (const p of ['p1', 'p2']) {
      const tab = $('key-tab-' + p);
      tab.setAttribute('aria-selected', String(p === player));
    }
    rows.textContent = '';
    for (const a of ACTIONS) {
      if (a.macro && a === ACTIONS.find((x) => x.macro)) {
        const sub = document.createElement('li');
        sub.className = 'pad-sub';
        sub.textContent = 'Macros: one press holds several buttons';
        rows.appendChild(sub);
      }
      const tr = document.createElement('li');
      tr.className = 'pad-row';
      tr.id = 'key-row-' + a.id;
      const name = document.createElement('span');
      name.className = 'pad-name';
      name.textContent = a.label;
      tr.appendChild(name);

      const chips = document.createElement('span');
      chips.className = 'pad-chips';
      const list = binds[player][a.id];
      if (list.length === 0) {
        const none = document.createElement('span');
        none.className = 'status';
        none.textContent = 'Not set';
        chips.appendChild(none);
      }
      list.forEach((code, i) => {
        const chip = document.createElement('button');
        chip.className = 'pad-chip';
        chip.type = 'button';
        chip.title = 'Remove';
        chip.setAttribute('aria-label', 'Remove ' + keyName(code) + ' from ' + a.label);
        chip.textContent = keyName(code) + ' ×';
        chip.addEventListener('click', () => { list.splice(i, 1); save(); release(); render(); });
        chips.appendChild(chip);
      });
      tr.appendChild(chips);

      const add = document.createElement('button');
      add.className = 'tool pad-add';
      add.type = 'button';
      const on = listening && listening.player === player && listening.id === a.id;
      add.textContent = on ? 'Press a key…' : 'Add';
      add.setAttribute('aria-pressed', String(!!on));
      add.addEventListener('click', () => listen(a.id));
      tr.appendChild(add);
      rows.appendChild(tr);
    }
    const clash = clashes();
    $('key-clash').hidden = !clash;
    $('key-clash').textContent = clash;
  }

  /* One key on two actions is allowed (it presses both), but it is usually a
   * mistake, so say so. */
  function clashes() {
    const seen = new Map();
    const out = [];
    for (const p of ['p1', 'p2']) {
      for (const a of ACTIONS) {
        for (const c of binds[p][a.id]) {
          const who = (p === 'p1' ? 'Player 1 ' : 'Player 2 ') + a.label;
          if (seen.has(c)) out.push(keyName(c) + ' is on both ' + seen.get(c) + ' and ' + who + '.');
          else seen.set(c, who);
        }
      }
    }
    return out.join(' ');
  }

  function init() {
    load();
    for (const p of ['p1', 'p2']) {
      $('key-tab-' + p).addEventListener('click', () => { player = p; if (listening) listening = null; render(); });
    }
    $('key-reset').addEventListener('click', () => { binds = clone(DEFAULTS); save(); release(); stopListening(); });
    /* The panel's Close button, the Controls button and Escape all go through
     * m2hlePad.toggle; this hears about it by watching the panel. */
    new MutationObserver(() => { if ($('controls').hidden) listening = null; render(); })
      .observe($('controls'), { attributes: true, attributeFilter: ['hidden'] });
    if (navigator.keyboard && navigator.keyboard.getLayoutMap) {
      navigator.keyboard.getLayoutMap().then((m) => { layout = m; render(); }).catch(() => {});
    }
    render();
  }

  for (const type of ['keydown', 'keyup']) window.addEventListener(type, onKey, true);
  window.addEventListener('blur', release);
  document.addEventListener('visibilitychange', () => { if (document.hidden) release(); });
  document.addEventListener('DOMContentLoaded', init);

  return { release, get mask() { return mask; }, get binds() { return clone(binds); }, keyName };
})();
