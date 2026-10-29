/*
 * m2hle-cardboard.js -- side-by-side 3D for a phone in a Cardboard viewer
 * (Pinboard #1436; the emulator's half is src/ui/stereo.h).
 *
 * The menu's Cardboard 3D turns it on: the page goes fullscreen and landscape,
 * the bar and touch buttons go away, and the emulator draws the board's frame
 * twice, once per eye (main_web.c, web_stereo_set). The board runs once, as
 * ever, so a session, a replay or a room sees nothing of it.
 *
 * Its own menu is drawn twice too, one copy over each eye's picture, so it can
 * be read through the lenses. A pad's touchpad button or L3 + R3 opens it, and
 * so do a tap on the screen (the viewer's own button taps it), Escape and Q.
 * The d-pad or left stick picks a row and moves its slider, Cross presses,
 * Circle closes. It pauses the board while open, when the board may pause.
 *
 * ?cardboard=1 in the address turns 3D on at load; fullscreen waits for a tap.
 *
 * Stored, per browser: localStorage m2hle.cardboard, the settings.
 */
'use strict';

m2hleLang.add({
  en: {
    'cb.title': '3D settings',
    'cb.depth': 'Depth',
    'cb.focus': 'Screen distance',
    'cb.spacing': 'Lens spacing',
    'cb.size': 'Picture size',
    'cb.swap': 'Swap left and right',
    'cb.on': 'On',
    'cb.off': 'Off',
    'cb.resume': 'Resume',
    'cb.leave': 'Leave 3D',
    'cb.hint': 'D-pad: choose and adjust · ✕: select · ○: close',
    'cb.open': 'Touchpad or L3 + R3: 3D settings',
  },
  ja: {
    'cb.title': '3D設定',
    'cb.depth': '奥行き',
    'cb.focus': 'スクリーンの距離',
    'cb.spacing': 'レンズの間隔',
    'cb.size': '画面サイズ',
    'cb.swap': '左右を入れ替える',
    'cb.on': 'オン',
    'cb.off': 'オフ',
    'cb.resume': '再開',
    'cb.leave': '3Dを終了',
    'cb.hint': '方向キー：選択・調整　✕：決定　○：閉じる',
    'cb.open': 'タッチパッドまたはL3＋R3：3D設定',
  },
});

const m2hleCardboard = (() => {
  const $ = (id) => document.getElementById(id);
  const t = (k) => m2hleLang.t(k);
  const KEY = 'm2hle.cardboard';

  /* The sliders, in stereo.h's units. Focus moves in log steps: it is a multiple. */
  const ROWS = [
    { id: 'depth', lo: 0, hi: 0.06, step: 0.002, def: 0.02, fmt: (v) => Math.round(v * 1000) },
    { id: 'focus', lo: 0.25, hi: 4, step: 1.1, def: 1, log: true, fmt: (v) => v.toFixed(2) + '×' },
    { id: 'spacing', lo: 0.3, hi: 0.7, step: 0.005, def: 0.5, fmt: (v) => Math.round(v * 100) + '%' },
    { id: 'size', lo: 0.5, hi: 1, step: 0.01, def: 1, fmt: (v) => Math.round(v * 100) + '%' },
    { id: 'swap', toggle: true },
    { id: 'resume', button: true },
    { id: 'leave', button: true },
  ];

  const set = { depth: 0.02, focus: 1, spacing: 0.5, size: 1, swap: false };
  let ready = false, on = false, open = false, sel = 0;
  let weFull = false, wePaused = false, wake = null;
  const copies = [];   /* the menu, once per eye */

  function load() {
    try {
      const s = JSON.parse(localStorage.getItem(KEY) || 'null') || {};
      for (const r of ROWS) if (!r.button && typeof s[r.id] === typeof set[r.id]) set[r.id] = s[r.id];
    } catch (e) { /* private mode */ }
  }
  function save() {
    try { localStorage.setItem(KEY, JSON.stringify(set)); } catch (e) { /* private mode */ }
  }

  function push() {
    if (ready) Module._web_stereo_set(on ? 1 : 0, set.depth, set.focus, set.spacing, set.size, set.swap ? 1 : 0);
  }

  /* ---- The menu, twice ------------------------------------------------------ */

  function build() {
    for (let eye = 0; eye < 2; eye++) {
      const box = document.createElement('div');
      box.className = 'cb-menu ps-win';
      box.hidden = true;
      box.setAttribute('role', eye ? 'presentation' : 'dialog');
      if (eye) box.setAttribute('aria-hidden', 'true');
      const h = document.createElement('p');
      h.className = 'ps-title cb-title';
      h.textContent = t('cb.title');
      box.appendChild(h);
      const rows = ROWS.map((r, i) => {
        const row = document.createElement('div');
        row.className = 'cb-row' + (r.button ? ' cb-button' : '');
        const name = document.createElement('span');
        name.className = 'cb-name';
        name.textContent = t('cb.' + r.id);
        row.appendChild(name);
        let fill = null, val = null;
        if (!r.button) {
          if (!r.toggle) {
            const bar = document.createElement('span');
            bar.className = 'cb-bar';
            fill = document.createElement('span');
            fill.className = 'cb-fill';
            bar.appendChild(fill);
            row.appendChild(bar);
            bar.addEventListener('pointerdown', (e) => {
              const b = bar.getBoundingClientRect();
              setFrac(r, (e.clientX - b.left) / b.width);
              e.stopPropagation();
            });
          }
          val = document.createElement('span');
          val.className = 'cb-val';
          row.appendChild(val);
        }
        row.addEventListener('click', (e) => { sel = i; if (r.toggle || r.button) press(); else render(); e.stopPropagation(); });
        box.appendChild(row);
        return { row, fill, val };
      });
      const hint = document.createElement('p');
      hint.className = 'cb-hint';
      hint.textContent = t('cb.hint');
      box.appendChild(hint);
      box.addEventListener('pointerdown', (e) => e.stopPropagation());
      document.body.appendChild(box);
      copies.push({ box, rows });
    }
    const tip = document.createElement('p');
    tip.id = 'cb-tip';
    tip.className = 'cb-tip';
    tip.hidden = true;
    tip.textContent = t('cb.open');
    document.body.appendChild(tip);
  }

  const frac = (r, v) => r.log ? Math.log(v / r.lo) / Math.log(r.hi / r.lo) : (v - r.lo) / (r.hi - r.lo);

  function setFrac(r, f) {
    f = Math.min(1, Math.max(0, f));
    const v = r.log ? r.lo * Math.pow(r.hi / r.lo, f) : r.lo + f * (r.hi - r.lo);
    set[r.id] = Math.round(v / (r.log ? 0.01 : r.step)) * (r.log ? 0.01 : r.step);
    changed();
  }

  function nudge(dir) {
    const r = ROWS[sel];
    if (r.button) return;
    if (r.toggle) { set[r.id] = !set[r.id]; changed(); return; }
    let v = r.log ? set[r.id] * Math.pow(r.step, dir) : set[r.id] + dir * r.step;
    v = Math.min(r.hi, Math.max(r.lo, v));
    set[r.id] = r.log ? v : Math.round(v / r.step) * r.step;
    changed();
  }

  function changed() { push(); save(); render(); }

  function press() {
    const r = ROWS[sel];
    if (r.toggle) nudge(1);
    else if (r.id === 'resume') show(false);
    else if (r.id === 'leave') leave();
  }

  /* Each copy centred on its eye's picture (stereo.h: the centres are at
   * W/2 * (1 -+ spacing)), sized to fit inside it. */
  function place() {
    const c = $('canvas').getBoundingClientRect();
    const half = c.width / 2;
    const w = Math.min(half * set.size * 0.86, 420);
    copies.forEach((m, eye) => {
      const cx = c.left + c.width * 0.5 * (1 + (eye ? 1 : -1) * set.spacing);
      m.box.style.width = w + 'px';
      m.box.style.fontSize = Math.max(9, Math.min(16, w / 24)) + 'px';
      m.box.style.left = (cx - w / 2) + 'px';
      m.box.style.top = (c.top + c.height / 2) + 'px';
    });
    const tip = $('cb-tip');
    if (tip) tip.style.left = (c.left + c.width * 0.5 * (1 - set.spacing)) + 'px';
  }

  function render() {
    copies.forEach((m) => {
      m.box.hidden = !open;
      ROWS.forEach((r, i) => {
        const x = m.rows[i];
        x.row.classList.toggle('sel', i === sel);
        if (x.fill) x.fill.style.width = (frac(r, set[r.id]) * 100) + '%';
        if (x.val) x.val.textContent = r.toggle ? t(set[r.id] ? 'cb.on' : 'cb.off') : r.fmt(set[r.id]);
      });
    });
    if (open) place();
  }

  function show(o) {
    if (o === open || (o && !on)) return;
    open = o;
    if (open) {
      sel = 0;
      if (ready && Module._web_pause_allowed() && !Module._web_paused()) {
        wePaused = !!Module._web_set_paused(1);
      }
    } else if (wePaused) {
      wePaused = false;
      if (ready && Module._web_paused()) Module._web_set_paused(0);
    }
    render();
  }

  /* ---- On and off ------------------------------------------------------------ */

  const fsElement = () => document.fullscreenElement || document.webkitFullscreenElement;

  function goFull() {
    const de = document.documentElement;
    const req = de.requestFullscreen || de.webkitRequestFullscreen;
    if (!fsElement() && req) {
      const p = req.call(de, { navigationUI: 'hide' });
      weFull = true;
      if (p && p.then) {
        p.then(lockLandscape, (e) => { weFull = false; m2hleTools.add('fullscreen refused: ' + e.message, 'warning'); });
      }
    } else lockLandscape();
    if (navigator.wakeLock && !wake) {
      navigator.wakeLock.request('screen').then((w) => { wake = w; w.addEventListener('release', () => { wake = null; }); }, () => {});
    }
  }

  function lockLandscape() {
    const o = screen.orientation;
    if (o && o.lock) o.lock('landscape').catch(() => { /* a desktop, or a browser that will not */ });
  }

  function enter(full) {
    on = true;
    document.body.classList.add('cardboard');
    push();
    if (full) goFull();
    const tip = $('cb-tip');
    tip.hidden = false;
    clearTimeout(enter.tip);
    enter.tip = setTimeout(() => { tip.hidden = true; }, 6000);
    place();
    window.dispatchEvent(new Event('resize'));
  }

  function leave() {
    show(false);
    on = false;
    document.body.classList.remove('cardboard');
    $('cb-tip').hidden = true;
    push();
    if (screen.orientation && screen.orientation.unlock) { try { screen.orientation.unlock(); } catch (e) { /* none */ } }
    if (weFull && fsElement()) {
      const ex = document.exitFullscreen || document.webkitExitFullscreen;
      if (ex) ex.call(document);
    }
    weFull = false;
    if (wake) { wake.release(); wake = null; }
    window.dispatchEvent(new Event('resize'));
  }

  /* ---- The pad ---------------------------------------------------------------- */

  const REPEAT_FIRST = 320, REPEAT_EVERY = 70;
  let prev = 0, heldSince = 0, lastRepeat = 0;

  /* What the first pads hold, as bits: 1 up, 2 down, 4 left, 8 right, 16 Cross,
   * 32 Circle, 64 open (the touchpad, or L3 + R3 together). */
  function padBits(list) {
    let b = 0;
    for (const p of list) if (p.mapping === 'standard') b |= onePadBits(p);
    return b;
  }

  const padBtn = (p, i) => !!p.buttons[i] && (p.buttons[i].pressed || p.buttons[i].value > 0.5);

  /* One standard pad's bits. */
  function onePadBits(p) {
    let b = 0;
    const ax = p.axes, btn = padBtn;
    if (btn(p, 12) || ax[1] < -0.6) b |= 1;
    if (btn(p, 13) || ax[1] > 0.6) b |= 2;
    if (btn(p, 14) || ax[0] < -0.6) b |= 4;
    if (btn(p, 15) || ax[0] > 0.6) b |= 8;
    if (btn(p, 0)) b |= 16;
    if (btn(p, 1)) b |= 32;
    if (btn(p, 17) || (btn(p, 10) && btn(p, 11))) b |= 64;
    return b;
  }

  function claim(list) {
    if (!on) { prev = 0; return false; }
    const b = padBits(list), down = b & ~prev, now = performance.now();
    prev = b;
    if (down & 64) { show(!open); return true; }
    if (!open) return false;
    if (down & 32) { show(false); return true; }
    if (down & 16) press();
    let dir = down & 15;
    if (b & 15 && !dir) {
      if (now - heldSince > REPEAT_FIRST && now - lastRepeat > REPEAT_EVERY) { dir = b & 15; lastRepeat = now; }
    } else if (dir) { heldSince = now; lastRepeat = now; }
    if (dir & 1) { sel = (sel + ROWS.length - 1) % ROWS.length; render(); }
    if (dir & 2) { sel = (sel + 1) % ROWS.length; render(); }
    if (dir & 4) nudge(-1);
    if (dir & 8) nudge(1);
    return true;
  }

  /* ---- Keys and taps ---------------------------------------------------------- */

  function key(e) {
    if (!on) return;
    const k = e.key;
    if (k === 'Escape' || k === 'q' || k === 'Q') { show(!open); }
    else if (!open) return;
    else if (k === 'ArrowUp') { sel = (sel + ROWS.length - 1) % ROWS.length; render(); }
    else if (k === 'ArrowDown') { sel = (sel + 1) % ROWS.length; render(); }
    else if (k === 'ArrowLeft') nudge(-1);
    else if (k === 'ArrowRight') nudge(1);
    else if (k === 'Enter' || k === ' ') press();
    else return;
    e.preventDefault();
    e.stopImmediatePropagation();
  }

  function init() {
    load();
    build();
    const btn = $('btn-cardboard');
    btn.addEventListener('click', () => (on ? leave() : enter(true)));
    document.addEventListener('keydown', key, true);
    /* A tap on the picture (the viewer's button) opens the menu, and the first
     * one after ?cardboard=1 goes fullscreen too. */
    $('canvas').addEventListener('pointerdown', (e) => {
      if (!on) return;
      if (!fsElement() && !weFull) goFull();
      else show(!open);
      e.preventDefault();
    });
    window.addEventListener('resize', () => { if (open) place(); });
    m2hlePad.setClaim(claim);
  }

  function onReady() {
    ready = true;
    if (Module._web_stereo_set === undefined) { $('btn-cardboard').hidden = true; return; }
    $('btn-cardboard').hidden = false;
    if (new URLSearchParams(location.search).get('cardboard') === '1') enter(false);
    else push();
  }

  document.addEventListener('DOMContentLoaded', init);

  return { onReady, enter, leave, get on() { return on; } };
})();
