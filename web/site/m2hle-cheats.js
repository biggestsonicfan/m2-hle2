/* Cheats: the game's script, typed into the drawer's Cheats tab (core/m2lua.h,
 * Pinboard #958).
 *
 * M2emulator ran SCRIPTS\<set>.lua; here the script is the tab's scratchpad,
 * kept in this browser per ROM set (localStorage m2hle.cheats.<set>), with
 * whether it is on (.on) and its options' values (.opts). Run hands the text
 * to the board (web_cheats_set), which runs it at each vblank; Turn off takes
 * it away, patches and all. The next visit runs it again if it was on.
 *
 * A session (RPCN, GGPO, a replay, a follow) keeps the script but the board
 * does not run it: both ends must run the same ROM. It runs again, from its
 * start, when the session ends; this page then puts its options back.
 *
 * What PostDraw draws (text) is painted on a canvas over the picture, placed
 * where the board last put the game. The script's own words (errors, option
 * names) go in as text, never as HTML. */
'use strict';

m2hleLang.add({
  en: {
    'cheats.none': 'No game yet.',
    'cheats.empty': 'No script. Type one, or press Example, then Run.',
    'cheats.off': 'Off. Run starts it.',
    'cheats.running': 'Running: {frames} frames.',
    'cheats.held': 'Waiting: an online match, a replay or a follow has the board.',
    'cheats.error': 'Error: {error}',
  },
  ja: {
    'cheats.none': 'まだゲームがありません。',
    'cheats.empty': 'スクリプトがありません。入力するか「例」を押して、「実行」を押してください。',
    'cheats.off': '停止中です。「実行」で始まります。',
    'cheats.running': '実行中：{frames} フレーム',
    'cheats.held': '待機中：オンライン対戦、リプレイ、フォローが基板を使っています。',
    'cheats.error': 'エラー：{error}',
  },
});

/* A block, not a function: its names stay out of the page's other scripts. */
let m2hleCheats;
{
  const $ = (id) => document.getElementById(id);
  const tr = m2hleLang.t;
  const POLL_MS = 500;
  const BOARD_W = 496, BOARD_H = 384;

  const EXAMPLE = [
    '-- M2emulator\'s script shape, in Lua 5.5: & | ~ << >> // work beside AND/OR/XOR/SHL/SHR.',
    'Frames = 0',
    '',
    'function Init() end',
    '',
    'function Frame()',
    '  Frames = Frames + 1',
    'end',
    '',
    'function PostDraw()',
    '  if Options.counter.value ~= 0 then',
    '    local word = I960_ReadDWord(0x00000000) & 0xFFFFFFFF',
    '    Video_DrawText(8, 8, string.format("frame %d  ROM[0] %08X", Frames, word), 0xFFFF00)',
    '  end',
    'end',
    '',
    'Options = {',
    '  counter = { name = "Frame counter", values = { "Off", "On" } },',
    '}',
    '',
  ].join('\n');

  let M = null;
  let set = '';           /* the game's ROM set, the storage key */
  let status = null;      /* web_cheats_status, last read */
  let drawGen = -1;
  let shown = false;      /* the overlay canvas has something on it */

  const key = (what) => 'm2hle.cheats.' + set + (what ? '.' + what : '');
  const load = (what, dflt) => { try { const v = localStorage.getItem(key(what)); return v === null ? dflt : v; } catch (e) { return dflt; } };
  const save = (what, v) => { try { localStorage.setItem(key(what), v); } catch (e) { /* private mode: this visit only */ } };
  const savedOpts = () => { try { return JSON.parse(load('opts', '{}')) || {}; } catch (e) { return {}; } };

  function readStatus() {
    status = JSON.parse(M.UTF8ToString(M._web_cheats_status()));
    return status;
  }

  /* A string for one call into the board. */
  function withString(text, fn) {
    const n = M.lengthBytesUTF8(text) + 1, p = M._malloc(n);
    M.stringToUTF8(text, p, n);
    try { return fn(p); } finally { M._free(p); }
  }

  /* The text to the board: "" takes the script away. */
  const hand = (text) => withString(text, (p) => M._web_cheats_set(p));

  /* The options the player set, back on a script that has just started (a Run,
   * a game load, or the end of a session). */
  function restoreOptions() {
    if (!status || !status.running) return;
    const want = savedOpts();
    for (const o of status.options) {
      const v = want[o.key];
      if (Number.isInteger(v) && v !== o.value && v >= 0 && v < o.values.length) setOption(o.key, v);
    }
  }

  const setOption = (k, v) => withString(k, (p) => M._web_cheats_option(p, v));

  function run() {
    if (!M || !set) return;
    const text = $('cheats-text').value;
    save('', text);
    save('on', text.trim() ? '1' : '0');
    hand(text);
    readStatus();
    restoreOptions();
    render();
  }

  function off() {
    if (!M || !set) return;
    save('on', '0');
    hand('');
    render();
  }

  /* ---- The tab ----------------------------------------------------------------- */

  function statusLine(s) {
    if (!set) return tr('cheats.none');
    if (s.error) return tr('cheats.error', { error: s.error });
    if (!s.loaded) return $('cheats-text').value.trim() ? tr('cheats.off') : tr('cheats.empty');
    return s.running ? tr('cheats.running', { frames: s.frames }) : tr('cheats.held');
  }

  function renderOptions(s) {
    const box = $('cheats-options');
    const opts = s.loaded && s.running ? s.options : [];
    $('cheats-opt-head').hidden = opts.length === 0;
    const sig = JSON.stringify(opts.map((o) => [o.key, o.name, o.values, o.value]));
    if (box.dataset.sig === sig) return;
    box.dataset.sig = sig;
    box.replaceChildren(...opts.map((o) => {
      const label = document.createElement('label');
      const name = document.createElement('span');
      name.textContent = o.name;
      const pick = document.createElement('select');
      o.values.forEach((v, i) => pick.add(new Option(v, String(i), false, i === o.value)));
      pick.addEventListener('change', () => {
        const v = Number(pick.value);
        const all = savedOpts();
        all[o.key] = v;
        save('opts', JSON.stringify(all));
        setOption(o.key, v);
        render();
      });
      label.append(name, pick);
      return label;
    }));
  }

  function render() {
    const s = M && set ? readStatus() : { loaded: false, options: [] };
    $('cheats-status').textContent = statusLine(s);
    renderOptions(s);
  }

  /* ---- PostDraw's picture -------------------------------------------------------- */

  function clearDraw(cv) {
    if (!shown) return;
    cv.getContext('2d').clearRect(0, 0, cv.width, cv.height);
    cv.hidden = true;
    shown = false;
  }

  function placeDraw(cv, d) {
    const box = $('canvas').getBoundingClientRect();
    const sx = box.width / d.canvas[0], sy = box.height / d.canvas[1];
    const [x, y, w, h] = d.rect;
    const css = { left: box.left + x * sx, top: box.top + y * sy, width: w * sx, height: h * sy };
    for (const k in css) cv.style[k] = css[k] + 'px';
    const pw = Math.max(1, Math.round(css.width * devicePixelRatio));
    const ph = Math.max(1, Math.round(css.height * devicePixelRatio));
    if (cv.width !== pw || cv.height !== ph) { cv.width = pw; cv.height = ph; }
  }

  const argb = (c) => `rgba(${(c >>> 16) & 255},${(c >>> 8) & 255},${c & 255},${((c >>> 24) & 255) / 255})`;

  function paint(cv, d) {
    placeDraw(cv, d);
    const g = cv.getContext('2d');
    g.setTransform(cv.width / BOARD_W, 0, 0, cv.height / BOARD_H, 0, 0);
    g.clearRect(0, 0, BOARD_W, BOARD_H);
    g.textBaseline = 'top';
    for (const it of d.items) {
      g.font = `${it.b ? 'bold ' : ''}${it.s}px "PS3 Mono", monospace`;
      g.fillStyle = argb(it.c);
      g.fillText(it.t, it.x, it.y);
    }
    cv.hidden = false;
    shown = true;
  }

  function drawLoop() {
    requestAnimationFrame(drawLoop);
    const cv = $('cheats-draw');
    if (!M || !status || !status.loaded) { clearDraw(cv); return; }
    const d = JSON.parse(M.UTF8ToString(M._web_cheats_draw()));
    if (!d.items.length || !d.rect[2]) { clearDraw(cv); drawGen = d.gen; return; }
    if (d.gen === drawGen && shown) return;
    drawGen = d.gen;
    paint(cv, d);
  }

  /* ---- Keys for Input_IsKeyPressed ---------------------------------------------- */

  /* KeyboardEvent.code to the DirectInput code M2emulator's scripts test. */
  const DIK = (() => {
    const m = { Escape: 1, Minus: 12, Equal: 13, Backspace: 14, Tab: 15, BracketLeft: 26, BracketRight: 27,
      Enter: 28, ControlLeft: 29, Semicolon: 39, Quote: 40, Backquote: 41, ShiftLeft: 42, Backslash: 43,
      Comma: 51, Period: 52, Slash: 53, ShiftRight: 54, NumpadMultiply: 55, AltLeft: 56, Space: 57,
      CapsLock: 58, NumLock: 69, ScrollLock: 70, NumpadSubtract: 74, NumpadAdd: 78, NumpadDecimal: 83,
      F11: 87, F12: 88, NumpadEnter: 156, ControlRight: 157, NumpadDivide: 181, AltRight: 184, Home: 199,
      ArrowUp: 200, PageUp: 201, ArrowLeft: 203, ArrowRight: 205, End: 207, ArrowDown: 208, PageDown: 209,
      Insert: 210, Delete: 211 };
    '1234567890'.split('').forEach((c, i) => { m['Digit' + c] = 2 + i; });
    'QWERTYUIOP'.split('').forEach((c, i) => { m['Key' + c] = 16 + i; });
    'ASDFGHJKL'.split('').forEach((c, i) => { m['Key' + c] = 30 + i; });
    'ZXCVBNM'.split('').forEach((c, i) => { m['Key' + c] = 44 + i; });
    for (let i = 1; i <= 10; i++) m['F' + i] = 58 + i;
    [[7, 71], [8, 72], [9, 73], [4, 75], [5, 76], [6, 77], [1, 79], [2, 80], [3, 81], [0, 82]]
      .forEach(([n, k]) => { m['Numpad' + n] = k; });
    return m;
  })();
  const PANELS = '#drawer, #online, #controls, #touch-edit, #menu, input, textarea, select';

  function onKey(e) {
    if (!M || e.repeat) return;
    if (e.target instanceof Element && e.target.closest(PANELS)) return;
    const k = DIK[e.code];
    if (k) M._web_cheats_key(k, e.type === 'keydown' ? 1 : 0);
  }

  /* ---- The page's calls ------------------------------------------------------------ */

  function onReady(module) {
    M = module;
    $('cheats-run').addEventListener('click', run);
    $('cheats-off').addEventListener('click', off);
    $('cheats-example').addEventListener('click', () => {
      const t = $('cheats-text');
      if (!t.value.trim()) t.value = EXAMPLE;
      if (set) save('', t.value);
      render();
    });
    $('cheats-text').addEventListener('input', () => { if (set) save('', $('cheats-text').value); });
    for (const type of ['keydown', 'keyup']) window.addEventListener(type, onKey, true);
    addEventListener('blur', () => { for (const k of Object.values(DIK)) M._web_cheats_key(k, 0); });
    let wasRunning = false;
    setInterval(() => {
      if (!set) return;
      readStatus();
      if (status.running && !wasRunning) restoreOptions();   /* back from a session */
      wasRunning = status.loaded && status.running;
      if (!$('drawer').hidden && !$('pane-cheats').hidden) render();
    }, POLL_MS);
    requestAnimationFrame(drawLoop);
    render();
  }

  /* A game has loaded (the board dropped the last one's script): this set's
   * scratchpad, run if it was on. */
  function onGame() {
    if (!M) return;
    set = M.UTF8ToString(M._web_cheats_romset());
    const text = load('', '');
    $('cheats-text').value = text;
    if (text.trim() && load('on', '0') === '1') {
      hand(text);
      readStatus();
      restoreOptions();
    }
    render();
  }

  m2hleCheats = { onReady, onGame };
}
