/*
 * Gamepads, and the Controls panel that maps them.
 *
 * The browser's Gamepad API has no events for buttons, only a snapshot to poll,
 * so this polls it once per display frame and hands the emulator one bit per
 * GAME_INPUT_* action (main_web.c, web_pad_set). The emulator presses and
 * releases only what changed, so a pad and the keyboard can hold the same
 * direction without letting go of each other.
 *
 * The mapping is kept in this origin's localStorage under 'm2hle.pad' and applies
 * to every pad. The first pad plays player 1; a second plays player 2 unless the
 * panel says otherwise. Under netplay both reach the local player (netplay.h,
 * netplay_sample_local), which is what a player with two pads plugged in expects.
 *
 * Browsers hide pads from a page until a button is pressed on one: "no controller"
 * before that is not a fault.
 */
'use strict';

const m2hlePad = (() => {
  const $ = (id) => document.getElementById(id);
  const STORE = 'm2hle.pad';

  /* The rows of the panel. `act` is the GAME_INPUT_* index for player 1
   * (src/core/game_profile.h); player 2's is 10 further on. */
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
  ];
  const P2_OFFSET = 10;

  /* A binding is a button ({b: index}) or one direction of an axis ({a: index, d: ±1}).
   * The defaults are for the "standard" layout, which is what the browser reports for
   * most XInput, PlayStation and Switch pads. */
  const DEFAULTS = {
    up:    [{ b: 12 }, { a: 1, d: -1 }],
    down:  [{ b: 13 }, { a: 1, d: 1 }],
    left:  [{ b: 14 }, { a: 0, d: -1 }],
    right: [{ b: 15 }, { a: 0, d: 1 }],
    b1:    [{ b: 2 }],
    b2:    [{ b: 0 }],
    b3:    [{ b: 1 }],
    b4:    [{ b: 3 }],
    start: [{ b: 9 }],
    coin:  [{ b: 8 }],
  };
  const MAX_BINDS = 4;
  const PRESS = 0.5;          /* a button counts as held past this (analogue triggers) */
  const DEAD = 0.5;           /* an axis counts as held past this */
  const GRAB = 0.6;           /* ...and must move this far from rest to be captured */

  const clone = (o) => JSON.parse(JSON.stringify(o));
  let binds = clone(DEFAULTS);
  let secondIsP2 = true;
  let ready = false;          /* the emulator's exports exist */
  let listening = null;       /* {id, rest: {buttons, axes} per pad index} while capturing */
  let padMask = 0;            /* what the pads held at the last poll */
  let touchMask = 0;          /* what the touch buttons hold (m2hle-touch.js, setTouch) */
  let keyMask = 0;            /* what the keyboard holds (m2hle-keys.js, setKeys) */

  function load() {
    try {
      const s = JSON.parse(localStorage.getItem(STORE) || 'null');
      if (!s || s.v !== 1) return;
      for (const a of ACTIONS) {
        const list = s.binds && s.binds[a.id];
        if (Array.isArray(list)) binds[a.id] = list.filter(valid).slice(0, MAX_BINDS);
      }
      if (typeof s.secondIsP2 === 'boolean') secondIsP2 = s.secondIsP2;
    } catch (e) { /* private mode, or a value from somewhere else: keep the defaults */ }
  }

  function save() {
    try { localStorage.setItem(STORE, JSON.stringify({ v: 1, binds, secondIsP2 })); } catch (e) { /* private mode */ }
  }

  function valid(x) {
    return x && ((Number.isInteger(x.b) && x.b >= 0 && x.b < 64) ||
                 (Number.isInteger(x.a) && x.a >= 0 && x.a < 16 && (x.d === 1 || x.d === -1)));
  }

  const same = (x, y) => x.b === y.b && x.a === y.a && x.d === y.d;

  /* ---- Names ------------------------------------------------------------------ */

  const STD_BUTTONS = ['A / Cross', 'B / Circle', 'X / Square', 'Y / Triangle', 'Left bumper', 'Right bumper',
                       'Left trigger', 'Right trigger', 'Back / Select', 'Start', 'Left stick press',
                       'Right stick press', 'D-pad up', 'D-pad down', 'D-pad left', 'D-pad right', 'Home'];
  const STD_AXES = [['Left stick left', 'Left stick right'], ['Left stick up', 'Left stick down'],
                    ['Right stick left', 'Right stick right'], ['Right stick up', 'Right stick down']];

  function bindName(x, standard) {
    if (x.b !== undefined) return standard && STD_BUTTONS[x.b] ? STD_BUTTONS[x.b] : 'Button ' + x.b;
    if (standard && STD_AXES[x.a]) return STD_AXES[x.a][x.d > 0 ? 1 : 0];
    return 'Axis ' + x.a + (x.d > 0 ? ' +' : ' −');
  }

  /* ---- Polling ---------------------------------------------------------------- */

  function pads() {
    const out = [];
    const list = navigator.getGamepads ? navigator.getGamepads() : [];
    for (const p of list) if (p && p.connected) out.push(p);
    return out;
  }

  function held(pad, x) {
    if (x.b !== undefined) {
      const btn = pad.buttons[x.b];
      return !!btn && (btn.pressed || btn.value > PRESS);
    }
    const v = pad.axes[x.a];
    return v !== undefined && v * x.d > DEAD;
  }

  /* One bit per player-1 action (0..9) this pad holds. */
  function padActions(pad) {
    let m = 0;
    for (const a of ACTIONS) {
      for (const x of binds[a.id]) if (held(pad, x)) { m |= 1 << a.act; break; }
    }
    return m;
  }

  function poll() {
    requestAnimationFrame(poll);
    const list = pads();
    if (listening) { capture(list); return; }

    let mask = 0;
    list.forEach((pad, i) => {
      const m = padActions(pad);
      mask |= (i === 1 && secondIsP2) ? m << P2_OFFSET : m;
      if (i === 0) showHeld(m);
    });
    if (list.length === 0) showHeld(0);
    padMask = mask;
    /* Every frame, not only on a change: the emulator forgets what the pad held
     * whenever it lets go of the keyboard (focus lost, the drawer opened), and a
     * direction still held should come straight back. */
    send();
  }

  /* The touch buttons and the keyboard share this one channel to the emulator:
   * web_pad_set takes the whole mask, so two callers would let go of each other's
   * presses. */
  function send() {
    if (ready && !listening) Module._web_pad_set((padMask | touchMask | keyMask) >>> 0);
  }

  /* A key goes out now rather than at the next poll, for the same reason. */
  function setKeys(m) {
    keyMask = m >>> 0;
    send();
  }

  /* A touch press goes out now rather than at the next poll: a tap is short. */
  function setTouch(m) {
    touchMask = m >>> 0;
    send();
  }

  /* ---- The panel -------------------------------------------------------------- */

  function snapshot(list) {
    const rest = new Map();
    for (const p of list) {
      rest.set(p.index, { buttons: p.buttons.map((b) => b.pressed || b.value > PRESS), axes: p.axes.slice() });
    }
    return rest;
  }

  function listen(id) {
    if (listening && listening.id === id) { stopListening(); return; }
    listening = { id, rest: snapshot(pads()) };
    /* Nothing reaches the game while a press is being captured: binding Punch
     * should not throw one. */
    if (ready) Module._web_pad_set(0);
    render();
  }

  function stopListening() {
    listening = null;
    render();
  }

  /* The first thing pressed or pushed since listen() began. A pad plugged in
   * mid-capture gets its rest state from its first poll. */
  function capture(list) {
    for (const p of list) {
      let rest = listening.rest.get(p.index);
      if (!rest) { listening.rest.set(p.index, snapshot([p]).get(p.index)); continue; }
      let got = null;
      p.buttons.forEach((b, i) => {
        const down = b.pressed || b.value > PRESS;
        if (down && !rest.buttons[i] && !got) got = { b: i };
        rest.buttons[i] = down;
      });
      if (!got) {
        p.axes.forEach((v, i) => {
          if (!got && Math.abs(v) > GRAB && Math.abs(v - (rest.axes[i] || 0)) > GRAB) got = { a: i, d: v > 0 ? 1 : -1 };
        });
      }
      if (got) {
        const list2 = binds[listening.id];
        if (!list2.some((x) => same(x, got))) {
          if (list2.length >= MAX_BINDS) list2.shift();
          list2.push(got);
          save();
        }
        stopListening();
        return;
      }
    }
  }

  function render() {
    const rows = $('pad-rows');
    if (!rows) return;
    const list = pads();
    const standard = list.length === 0 || list[0].mapping === 'standard';
    rows.textContent = '';
    for (const a of ACTIONS) {
      const tr = document.createElement('li');
      tr.className = 'pad-row';
      tr.id = 'pad-row-' + a.id;
      const name = document.createElement('span');
      name.className = 'pad-name';
      name.textContent = a.label;
      tr.appendChild(name);

      const chips = document.createElement('span');
      chips.className = 'pad-chips';
      if (binds[a.id].length === 0) {
        const none = document.createElement('span');
        none.className = 'status';
        none.textContent = 'Not set';
        chips.appendChild(none);
      }
      binds[a.id].forEach((x, i) => {
        const chip = document.createElement('button');
        chip.className = 'pad-chip';
        chip.type = 'button';
        chip.title = 'Remove';
        chip.setAttribute('aria-label', 'Remove ' + bindName(x, standard) + ' from ' + a.label);
        chip.textContent = bindName(x, standard) + ' ×';
        chip.addEventListener('click', () => { binds[a.id].splice(i, 1); save(); render(); });
        chips.appendChild(chip);
      });
      tr.appendChild(chips);

      const add = document.createElement('button');
      add.className = 'tool pad-add';
      add.type = 'button';
      const on = listening && listening.id === a.id;
      add.textContent = on ? 'Press a button…' : 'Add';
      add.setAttribute('aria-pressed', String(!!on));
      add.addEventListener('click', () => listen(a.id));
      tr.appendChild(add);
      rows.appendChild(tr);
    }
    $('pad-p2').checked = secondIsP2;
    status(list);
  }

  function status(list) {
    const el = $('pad-status');
    if (!el) return;
    if (list.length === 0) {
      el.textContent = 'No controller found. Connect one and press any button on it.';
      return;
    }
    const names = list.map((p, i) => (i === 0 ? 'Player 1: ' : (i === 1 && secondIsP2 ? 'Player 2: ' : 'Also player 1: ')) +
                                    p.id.replace(/\s*\(.*?Vendor.*?\)\s*/i, ' ').trim());
    el.textContent = names.join(' · ') + (list[0].mapping === 'standard' ? '' : ' (unrecognised layout: buttons are shown by number)');
  }

  /* The rows light up while the first pad holds them: the quickest way to check a mapping. */
  let shownHeld = -1;
  function showHeld(m) {
    if (m === shownHeld || $('controls').hidden) return;
    shownHeld = m;
    for (const a of ACTIONS) {
      const row = $('pad-row-' + a.id);
      if (row) row.classList.toggle('pad-held', !!(m & (1 << a.act)));
    }
  }

  function toggle(open) {
    const panel = $('controls');
    if (open === undefined) open = panel.hidden;
    if (!open) stopListening();
    /* Play online opens in the same place: one at a time. */
    if (open && !$('online').hidden) $('np-close').click();
    panel.hidden = !open;
    $('btn-controls').setAttribute('aria-expanded', String(open));
    shownHeld = -1;
    if (open) render();
    else $('canvas').focus();
  }

  function init() {
    load();
    $('btn-controls').addEventListener('click', () => toggle());
    $('pad-close').addEventListener('click', () => toggle(false));
    $('btn-online').addEventListener('click', () => toggle(false));
    $('pad-reset').addEventListener('click', () => { binds = clone(DEFAULTS); secondIsP2 = true; save(); stopListening(); });
    $('pad-p2').addEventListener('change', (e) => { secondIsP2 = e.target.checked; save(); render(); });
    $('controls').addEventListener('keydown', (e) => {
      if (e.key === 'Escape') { if (listening) stopListening(); else toggle(false); }
    });
    window.addEventListener('gamepadconnected', (e) => {
      m2hleTools.add('gamepad: connected "' + e.gamepad.id + '" (' + (e.gamepad.mapping || 'no standard layout') + ')');
      if (!$('controls').hidden) render();
    });
    window.addEventListener('gamepaddisconnected', (e) => {
      m2hleTools.add('gamepad: disconnected "' + e.gamepad.id + '"');
      if (!$('controls').hidden) render();
    });
    requestAnimationFrame(poll);
  }

  /* Called from Module.onM2hleReady. */
  function onReady() { ready = true; }

  document.addEventListener('DOMContentLoaded', init);

  return { onReady, toggle, setTouch, setKeys, get binds() { return clone(binds); } };
})();
