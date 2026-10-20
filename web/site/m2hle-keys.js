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
 * Kept in this origin's localStorage under 'm2hle.keys', or 'm2hle.keys.<game>'
 * for a game with a layout of its own (m2hleGame.keys: the racing games).
 *
 * This script loads BEFORE m2hle-tools.js, so its window capture listener runs
 * first. It has to: the tools listener stops every key aimed at a panel (so
 * typing in the online panel never reaches the game), and "press a key" in the
 * Controls panel is exactly such a key. Anything this script does not take, it
 * leaves for the listeners after it.
 */
'use strict';

/* The words (m2hle-lang.js). keys.act.* are the rows' names unless the game has
 * its own (m2hleGame.labels); m2hle-pad.js and m2hle-touch.js, which load after
 * this, name their rows with them too. */
m2hleLang.add({
  en: {
    'keys.act.up': 'Up', 'keys.act.down': 'Down', 'keys.act.left': 'Left', 'keys.act.right': 'Right',
    'keys.act.b1': 'Punch', 'keys.act.b2': 'Kick', 'keys.act.b3': 'Barrier', 'keys.act.b4': 'Button 4',
    'keys.act.start': 'Start', 'keys.act.coin': 'Coin', 'keys.act.pause': 'Pause',
    'keys.sub.macros': 'Macros: one press holds several buttons',
    'keys.sub.page': 'The page: offline only, as the button beside the menu',
    'keys.act.service': 'Service', 'keys.act.test': 'Test',
    'keys.act.gearn': 'Neutral', 'keys.act.gear1': '1st gear', 'keys.act.gear2': '2nd gear',
    'keys.act.gear3': '3rd gear', 'keys.act.gear4': '4th gear',
    'keys.sub.cab': 'The cabinet: player 1 only',
    'keys.none': 'Not set',
    'keys.remove': 'Remove',
    'keys.remove.from': 'Remove {key} from {action}',
    'keys.add': 'Add',
    'keys.press': 'Press a key…',
    'keys.who.p1': 'Player 1 {action}',
    'keys.who.p2': 'Player 2 {action}',
    'keys.clash': '{key} is on both {first} and {second}.',
    'keys.space': 'Space',
  },
  ja: {
    'keys.act.up': '上', 'keys.act.down': '下', 'keys.act.left': '左', 'keys.act.right': '右',
    'keys.act.b1': 'パンチ', 'keys.act.b2': 'キック', 'keys.act.b3': 'バリア', 'keys.act.b4': 'ボタン4',
    'keys.act.start': 'スタート', 'keys.act.coin': 'コイン', 'keys.act.pause': 'ポーズ',
    'keys.sub.macros': 'マクロ：1回押すと複数のボタンを同時に押します',
    'keys.sub.page': 'ページ：オフライン時のみ。メニュー横のボタンと同じです',
    'keys.act.service': 'サービス', 'keys.act.test': 'テスト',
    'keys.act.gearn': 'ニュートラル', 'keys.act.gear1': '1速', 'keys.act.gear2': '2速',
    'keys.act.gear3': '3速', 'keys.act.gear4': '4速',
    'keys.sub.cab': '筐体：プレイヤー1のみ',
    'keys.none': '未設定',
    'keys.remove': '削除',
    'keys.remove.from': '{action}から{key}を削除',
    'keys.add': '追加',
    'keys.press': 'キーを押してください…',
    'keys.who.p1': 'プレイヤー1の{action}',
    'keys.who.p2': 'プレイヤー2の{action}',
    'keys.clash': '{key}が{first}と{second}の両方に割り当てられています。',
    'keys.space': 'スペース',
  },
});

const m2hleKeys = (() => {
  const $ = (id) => document.getElementById(id);
  const STORE = m2hleGame.keys ? 'm2hle.keys.' + m2hleGame.id : 'm2hle.keys';
  const MAX_BINDS = 3;

  /* The rows of the panel; `act` is the player-1 GAME_INPUT_* index (`acts` for a
   * macro), player 2's is 10 further on (src/core/game_profile.h, as in m2hle-pad.js). */
  const ACTIONS = [
    { id: 'up',    act: 0 },
    { id: 'down',  act: 1 },
    { id: 'left',  act: 2 },
    { id: 'right', act: 3 },
    { id: 'b1',    act: 4 },
    { id: 'b2',    act: 5 },
    { id: 'b3',    act: 6 },
    { id: 'b4',    act: 7 },
    { id: 'start', act: 8 },
    { id: 'coin',  act: 9 },
    /* Macros: one binding that holds several buttons at once, as the Gems
     * Collection and HD ports offer. Unbound until the player binds them. */
    { id: 'pk',    label: 'P + K',     acts: [4, 5],    macro: true },
    { id: 'pb',    label: 'P + B',     acts: [4, 6],    macro: true },
    { id: 'kb',    label: 'K + B',     acts: [5, 6],    macro: true },
    { id: 'pkb',   label: 'P + K + B', acts: [4, 5, 6], macro: true },
    /* The cabinet's own switches, player 1's tab only: the board's Service and
     * Test, and a racing game's gear buttons (m2hleGame.gears), one per gate of
     * the lever (src/board/io_5649.h). Unbound until the player binds them. */
    { id: 'service', act: 20, cab: true },
    { id: 'test',    act: 21, cab: true },
    { id: 'gearn',   act: 22, cab: true, gears: true },
    { id: 'gear1',   act: 23, cab: true, gears: true },
    { id: 'gear2',   act: 24, cab: true, gears: true },
    { id: 'gear3',   act: 25, cab: true, gears: true },
    { id: 'gear4',   act: 26, cab: true, gears: true },
    /* The page's Pause (m2hle-page.js), not a game button: it holds nothing. */
    { id: 'pause', acts: [],        page: true },
  ].filter((a) => !a.gears || m2hleGame.gears);
  for (const a of ACTIONS) a.mask = (a.acts || [a.act]).reduce((m, i) => m | (1 << i), 0);
  for (const a of ACTIONS) a.label = m2hleGame.labels[a.id] || a.label || m2hleLang.t('keys.act.' + a.id);
  const P2_OFFSET = 10;

  /* The desktop build's keys (src/board/input.h), which are MAME's and
   * m2emulator's, and a game's own over them (m2hleGame.keys). */
  const DEFAULTS = {
    p1: { up: ['ArrowUp'], down: ['ArrowDown'], left: ['ArrowLeft'], right: ['ArrowRight'],
          b1: ['KeyZ'], b2: ['KeyX'], b3: ['KeyC'], b4: ['KeyV'], start: ['Digit1'], coin: ['Digit5'],
          pk: [], pb: [], kb: [], pkb: [], service: ['F1', 'Digit9'], test: ['F2'],
          gearn: [], gear1: [], gear2: [], gear3: [], gear4: [], pause: ['KeyP', 'Pause'] },
    p2: { up: ['KeyI'], down: ['KeyK'], left: ['KeyJ'], right: ['KeyL'],
          b1: ['Delete'], b2: ['End'], b3: ['PageDown'], b4: ['Home'], start: ['Digit2'], coin: ['Digit6'],
          pk: [], pb: [], kb: [], pkb: [], service: [], test: [],
          gearn: [], gear1: [], gear2: [], gear3: [], gear4: [], pause: [] },
  };
  for (const p of ['p1', 'p2']) Object.assign(DEFAULTS[p], (m2hleGame.keys || {})[p]);

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
      /* Settings from before Pause was a row: a key the player gave the game keeps
       * that job rather than also pausing (P on Punch, say). */
      for (const code of Object.values(binds).flatMap((b) => b.pause)) if (bitsOf(code)) unbindPause(code);
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
    if (code === 'Space') return m2hleLang.t('keys.space');
    return code.replace(/(Left|Right)$/, ' ($1)').replace(/([a-z])([A-Z])/g, '$1 $2');
  }

  /* ---- The game's keys -------------------------------------------------------- */

  /* Which action bits one key code drives, both players. */
  function bitsOf(code) {
    let m = 0;
    for (const p of ['p1', 'p2']) {
      const shift = p === 'p2' ? P2_OFFSET : 0;
      for (const a of ACTIONS) if (!(a.cab && shift) && binds[p][a.id].includes(code)) m |= a.mask << shift;
    }
    return m >>> 0;
  }

  const isPause = (code) => binds.p1.pause.includes(code) || binds.p2.pause.includes(code);

  /* A key is the game's or Pause's, not both: binding it to one takes it off the
   * other, for both players, so a key never pauses and punches at once. */
  function unbindPause(code) {
    for (const p of ['p1', 'p2']) binds[p].pause = binds[p].pause.filter((c) => c !== code);
  }
  function unbindGame(code) {
    for (const p of ['p1', 'p2']) for (const a of ACTIONS) if (!a.page) binds[p][a.id] = binds[p][a.id].filter((c) => c !== code);
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
  const PANELS = '#drawer, #online, #controls, #touch-edit, #menu, input, textarea, select';
  const inPanel = (e) => e.target instanceof Element && e.target.closest(PANELS);

  function onKey(e) {
    if (listening && e.type === 'keydown') {
      e.preventDefault();
      e.stopImmediatePropagation();
      if (e.code === 'Escape' || !e.code) { stopListening(); return; }
      if (listening.id === 'pause') unbindGame(e.code);
      else unbindPause(e.code);
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
    if (isPause(e.code)) {
      e.preventDefault();
      if (!e.repeat) window.dispatchEvent(new Event('m2hle-pause'));
      return;
    }
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
      if (a.cab && player !== 'p1') continue;
      if (a.macro && a === ACTIONS.find((x) => x.macro)) {
        const sub = document.createElement('li');
        sub.className = 'pad-sub';
        sub.textContent = m2hleLang.t('keys.sub.macros');
        rows.appendChild(sub);
      }
      if (a.cab && a === ACTIONS.find((x) => x.cab)) {
        const sub = document.createElement('li');
        sub.className = 'pad-sub';
        sub.textContent = m2hleLang.t('keys.sub.cab');
        rows.appendChild(sub);
      }
      if (a.page) {
        const sub = document.createElement('li');
        sub.className = 'pad-sub';
        sub.textContent = m2hleLang.t('keys.sub.page');
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
        none.textContent = m2hleLang.t('keys.none');
        chips.appendChild(none);
      }
      list.forEach((code, i) => {
        const chip = document.createElement('button');
        chip.className = 'pad-chip';
        chip.type = 'button';
        chip.title = m2hleLang.t('keys.remove');
        chip.setAttribute('aria-label', m2hleLang.t('keys.remove.from', { key: keyName(code), action: a.label }));
        chip.textContent = keyName(code) + ' ×';
        chip.addEventListener('click', () => { list.splice(i, 1); save(); release(); render(); });
        chips.appendChild(chip);
      });
      tr.appendChild(chips);

      const add = document.createElement('button');
      add.className = 'tool pad-add';
      add.type = 'button';
      const on = listening && listening.player === player && listening.id === a.id;
      add.textContent = m2hleLang.t(on ? 'keys.press' : 'keys.add');
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
          const who = m2hleLang.t('keys.who.' + p, { action: a.label });
          if (seen.has(c)) out.push(m2hleLang.t('keys.clash', { key: keyName(c), first: seen.get(c), second: who }));
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
     * m2hlePad.toggle, and its tabs hide this section; this hears about both by watching. */
    const hiddenWatch = new MutationObserver(() => {
      if ($('controls').hidden || $('key-section').hidden) listening = null;
      render();
    });
    for (const id of ['controls', 'key-section']) hiddenWatch.observe($(id), { attributes: true, attributeFilter: ['hidden'] });
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
