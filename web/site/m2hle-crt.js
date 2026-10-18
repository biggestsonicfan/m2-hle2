/*
 * m2hle-crt.js -- the Picture tab's monitor adjustments: the pots on an arcade
 * monitor's chassis (src/ui/crt_pots.h). Each of red, green and blue has a
 * drive (gain) and a cutoff (bias); H and V each have a position and a size.
 * They act on the game's picture whatever filter is on.
 *
 * Stored, per browser: localStorage m2hle.crtPots, the pots off centre.
 */
'use strict';

/* The words (m2hle-lang.js): crt.group.* the headings, crt.<label> the pots. */
m2hleLang.add({
  en: {
    'crt.group.gain': 'Drive (gain)', 'crt.group.bias': 'Cutoff (bias)', 'crt.group.geometry': 'Geometry',
    'crt.red': 'Red', 'crt.green': 'Green', 'crt.blue': 'Blue',
    'crt.hPos': 'H position', 'crt.vPos': 'V position', 'crt.hSize': 'H size', 'crt.vSize': 'V size',
    'crt.aria': '{group}: {pot}',
  },
  ja: {
    'crt.group.gain': 'ドライブ（ゲイン）', 'crt.group.bias': 'カットオフ（バイアス）', 'crt.group.geometry': '画面位置・サイズ',
    'crt.red': '赤', 'crt.green': '緑', 'crt.blue': '青',
    'crt.hPos': '水平位置', 'crt.vPos': '垂直位置', 'crt.hSize': '水平サイズ', 'crt.vSize': '垂直サイズ',
    'crt.aria': '{group}：{pot}',
  },
});

const m2hleCrt = (() => {
  const $ = (id) => document.getElementById(id);
  const KEY = 'm2hle.crtPots';

  /* In CRT_POT_* order; `id` is the index the emulator knows them by. */
  const POTS = [
    { key: 'gainR', group: 'gain', label: 'red', lo: 0, hi: 2, centre: 1, step: 0.01 },
    { key: 'gainG', label: 'green', lo: 0, hi: 2, centre: 1, step: 0.01 },
    { key: 'gainB', label: 'blue', lo: 0, hi: 2, centre: 1, step: 0.01 },
    { key: 'biasR', group: 'bias', label: 'red', lo: -0.5, hi: 0.5, centre: 0, step: 0.005 },
    { key: 'biasG', label: 'green', lo: -0.5, hi: 0.5, centre: 0, step: 0.005 },
    { key: 'biasB', label: 'blue', lo: -0.5, hi: 0.5, centre: 0, step: 0.005 },
    { key: 'hPos', group: 'geometry', label: 'hPos', lo: -0.25, hi: 0.25, centre: 0, step: 0.0025 },
    { key: 'vPos', label: 'vPos', lo: -0.25, hi: 0.25, centre: 0, step: 0.0025 },
    { key: 'hSize', label: 'hSize', lo: 0.5, hi: 1.5, centre: 1, step: 0.005 },
    { key: 'vSize', label: 'vSize', lo: 0.5, hi: 1.5, centre: 1, step: 0.005 },
  ];

  let ready = false;
  const inputs = [];

  function saved() {
    try { return JSON.parse(localStorage.getItem(KEY) || '{}') || {}; } catch (e) { return {}; }
  }

  function save() {
    const off = {};
    POTS.forEach((p, i) => { const v = Number(inputs[i].value); if (v !== p.centre) off[p.key] = v; });
    try { localStorage.setItem(KEY, JSON.stringify(off)); } catch (e) { /* private mode */ }
  }

  /* Gains and sizes as a percentage, the bias and positions as a signed one. */
  function fmt(p, v) {
    const pc = Math.round(v * 100);
    return p.centre === 0 ? (pc > 0 ? '+' : '') + pc + '%' : pc + '%';
  }

  function push(i) {
    if (ready && inputs[i]) Module._web_crt_pot_set(i, Number(inputs[i].value));
  }

  function build() {
    const box = $('crt-pots');
    const mine = saved();
    let group = '';
    POTS.forEach((p, i) => {
      if (p.group) {
        group = m2hleLang.t('crt.group.' + p.group);
        const h = document.createElement('p');
        h.className = 'crt-group';
        h.textContent = group;
        box.appendChild(h);
      }
      const row = document.createElement('label');
      row.className = 'shader-param';
      const name = document.createElement('span');
      name.textContent = m2hleLang.t('crt.' + p.label);
      const input = document.createElement('input');
      input.type = 'range';
      input.min = String(p.lo);
      input.max = String(p.hi);
      input.step = String(p.step);
      const v = Number(mine[p.key]);
      input.value = String(Number.isFinite(v) ? v : p.centre);
      input.setAttribute('aria-label', m2hleLang.t('crt.aria', { group, pot: name.textContent }));
      const out = document.createElement('output');
      out.textContent = fmt(p, Number(input.value));
      input.addEventListener('input', () => {
        out.textContent = fmt(p, Number(input.value));
        push(i);
        save();
      });
      /* A double click puts one pot back at its centre. */
      input.addEventListener('dblclick', () => {
        input.value = String(p.centre);
        input.dispatchEvent(new Event('input'));
      });
      inputs.push(input);
      row.append(name, input, out);
      box.appendChild(row);
    });
  }

  function reset() {
    POTS.forEach((p, i) => {
      inputs[i].value = String(p.centre);
      inputs[i].dispatchEvent(new Event('input'));
    });
  }

  function init() {
    if (inputs.length) return;
    build();
    $('crt-reset').addEventListener('click', reset);
    POTS.forEach((p, i) => push(i));
  }

  /* Called from Module.onM2hleReady: the exports exist now. That can come
   * before DOMContentLoaded, so whichever is second sends the pots. */
  function onReady() {
    ready = true;
    POTS.forEach((p, i) => push(i));
  }

  /* For scripts and tests: set one pot by key, as the slider would. */
  function set(key, value) {
    const i = POTS.findIndex((p) => p.key === key);
    if (i < 0 || !inputs[i]) return false;
    inputs[i].value = String(value);
    inputs[i].dispatchEvent(new Event('input'));
    return true;
  }

  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', init);
  else init();

  return { onReady, set, reset };
})();
