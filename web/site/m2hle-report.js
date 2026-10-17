/* Screenshot and Report an issue (Pinboard #653).
 *
 * Screenshot saves the game's picture as the player sees it (filter and menus
 * included) as a PNG. Report an issue freezes the board on the frame it was opened
 * on and gathers what a developer needs to see that frame again: the picture, a
 * savestate of the board drawn into it, the console and this machine's details,
 * zipped as one file the player attaches to a GitHub issue. Dropping that zip (or
 * its state.sta) on a running game loads the state back.
 *
 * Offline only, like Pause: in a room or a match the board is one of two, and a
 * state of one of them would not reproduce anything (main_web.c,
 * web_report_allowed). The menu items go away there. */
'use strict';

const m2hleReport = (() => {
  /* The panel's words. What goes to the developers (report.txt, the issue's
   * facts, the console) stays English; the issue's last line asks the player. */
  m2hleLang.add({
    en: {
      'report.error': 'The picture could not be taken. Is a game running offline?',
      'report.frame': '{game}, frame {frame}',
      'report.state.with': 'with a savestate of this frame',
      'report.state.without': 'without a savestate ({why})',
      'report.state.none': 'no state was saved',
      'report.attach': 'Attach the downloaded {file} here (drag it into this box).',
    },
    ja: {
      'report.error': '画面を取得できませんでした。ゲームをオフラインで遊んでいますか？',
      'report.frame': '{game}、フレーム {frame}',
      'report.state.with': 'このフレームのセーブステート付き',
      'report.state.without': 'セーブステートなし（{why}）',
      'report.state.none': 'セーブステートを保存できませんでした',
      'report.attach': 'ダウンロードした {file} をここに添付してください（この欄にドラッグします）。',
    },
  });
  const t = m2hleLang.t;
  const $ = (id) => document.getElementById(id);
  const ISSUES = 'https://github.com/biggestsonicfan/m2-hle2/issues/new';
  let M = null;
  let open = false;
  let held = false;      /* the panel froze the board, and lets it go on close */
  const PANELS = [['online', 'np-close'], ['controls', 'pad-close'], ['ggpo', 'gg-close']];
  let report = null;     /* { png, state, info, when } of the frame the panel opened on */

  /* ---- A store-only zip ------------------------------------------------------ */

  const CRC = (() => {
    const t = new Uint32Array(256);
    for (let n = 0; n < 256; n++) {
      let c = n;
      for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
      t[n] = c >>> 0;
    }
    return t;
  })();

  function crc32(bytes) {
    let c = 0xFFFFFFFF;
    for (let i = 0; i < bytes.length; i++) c = CRC[(c ^ bytes[i]) & 0xFF] ^ (c >>> 8);
    return (c ^ 0xFFFFFFFF) >>> 0;
  }

  /* The local header (30 bytes) or the central one (46), name after it. */
  function zipHeader(central, f, offset) {
    const h = new DataView(new ArrayBuffer(central ? 46 : 30));
    h.setUint32(0, central ? 0x02014b50 : 0x04034b50, true);
    const at = central ? 2 : 0;           /* the central header has "version made by" first */
    if (central) h.setUint16(4, 20, true);
    h.setUint16(4 + at, 20, true);        /* version needed */
    h.setUint16(6 + at, 0x0800, true);    /* UTF-8 names; method 0, stored */
    h.setUint32(14 + at, f.crc, true);
    h.setUint32(18 + at, f.bytes.length, true);
    h.setUint32(22 + at, f.bytes.length, true);
    h.setUint16(26 + at, f.name.length, true);
    if (central) h.setUint32(42, offset, true);
    return new Uint8Array(h.buffer);
  }

  function zip(files) {
    const parts = [], dir = [];
    let offset = 0;
    for (const f of files) {
      const name = new TextEncoder().encode(f.name);
      const e = { name, bytes: f.bytes, crc: crc32(f.bytes) };
      dir.push(zipHeader(true, e, offset), name);
      parts.push(zipHeader(false, e, 0), name, f.bytes);
      offset += 30 + name.length + f.bytes.length;
    }
    const dirLen = dir.reduce((n, p) => n + p.length, 0);
    const end = new DataView(new ArrayBuffer(22));
    end.setUint32(0, 0x06054b50, true);
    end.setUint16(8, files.length, true);
    end.setUint16(10, files.length, true);
    end.setUint32(12, dirLen, true);
    end.setUint32(16, offset, true);
    return new Blob([...parts, ...dir, new Uint8Array(end.buffer)], { type: 'application/zip' });
  }

  /* One stored entry out of a zip, by name: enough for our own reports. */
  function unzipEntry(bytes, want) {
    const v = new DataView(bytes.buffer, bytes.byteOffset, bytes.length);
    for (let at = 0; at + 30 <= bytes.length && v.getUint32(at, true) === 0x04034b50;) {
      const method = v.getUint16(at + 8, true), size = v.getUint32(at + 18, true);
      const nameLen = v.getUint16(at + 26, true), extra = v.getUint16(at + 28, true);
      const name = new TextDecoder().decode(bytes.subarray(at + 30, at + 30 + nameLen));
      const data = at + 30 + nameLen + extra;
      if (name === want || name.endsWith('/' + want)) return method === 0 ? bytes.subarray(data, data + size) : null;
      at = data + size;
    }
    return null;
  }

  /* ---- The board's side ---------------------------------------------------- */

  const allowed = () => !!(M && M._web_report_allowed());
  const info = () => JSON.parse(M.UTF8ToString(M._web_report_info()));

  function copyOut(ptr, len) {
    return ptr && len > 0 ? M.HEAPU8.slice(ptr, ptr + len) : null;
  }

  /* The next picture drawn, read back in the frame callback (and the board saved
   * with it). Resolves null when it could not be taken. */
  function grab(withState) {
    if (!allowed() || !M._web_shot_arm(withState ? 1 : 0)) return Promise.resolve(null);
    const deadline = performance.now() + 3000;
    return new Promise((done) => {
      const wait = () => {
        if (M._web_shot_pending() && performance.now() < deadline) { requestAnimationFrame(wait); return; }
        done(collect(withState));
      };
      requestAnimationFrame(wait);
    });
  }

  function collect(withState) {
    const w = M._web_shot_w(), h = M._web_shot_h();
    const rgba = copyOut(M._web_shot_data(), w * h * 4);
    M._web_shot_free();
    let state = null, stateError = '';
    if (withState) {
      state = copyOut(M._web_state_data(), M._web_state_len());
      stateError = state ? '' : M.UTF8ToString(M._web_state_error());   /* '': none, no reason */
      M._web_state_free();
    }
    return rgba ? { w, h, rgba, state, stateError, info: info(), when: new Date() } : null;
  }

  function toPng(shot) {
    const c = document.createElement('canvas');
    c.width = shot.w;
    c.height = shot.h;
    c.getContext('2d').putImageData(new ImageData(new Uint8ClampedArray(shot.rgba.buffer), shot.w, shot.h), 0, 0);
    return new Promise((done) => c.toBlob(done, 'image/png'));
  }

  function stamp(shot) {
    const d = shot.when, two = (n) => String(n).padStart(2, '0');
    return (shot.info.profile || 'm2hle') + '-' + d.getFullYear() + two(d.getMonth() + 1) + two(d.getDate()) +
           '-' + two(d.getHours()) + two(d.getMinutes()) + two(d.getSeconds()) + '-f' + shot.info.frame;
  }

  function download(blob, name) {
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = name;
    document.body.appendChild(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(a.href), 10000);
  }

  /* ---- Screenshot ------------------------------------------------------------ */

  /* A room can start while a picture is being encoded or zipped: the copy the page
   * pulled before it is dropped here, before anything is saved, and the panel with it. */
  function dropIfOnline() {
    if (allowed()) return false;
    if (open) toggle(false);
    report = null;
    return true;
  }

  async function screenshot() {
    const shot = await grab(false);
    if (!shot) { m2hleTools.printErr('screenshot: none taken'); return; }
    const png = await toPng(shot);
    if (dropIfOnline()) { m2hleTools.printErr('screenshot: dropped, a room started'); return; }
    download(png, stamp(shot) + '.png');
    m2hleTools.print('screenshot: ' + shot.w + 'x' + shot.h + ' at frame ' + shot.info.frame);
  }

  /* ---- Report an issue ------------------------------------------------------- */

  function reportText() {
    const r = report, desc = $('rp-text').value.trim();
    return [
      desc || '(no description)', '',
      '---',
      'Game       ' + r.info.game + ' (profile ' + r.info.profile + ')',
      'Frame      ' + r.info.frame,
      'Page       ' + location.href,
      ...m2hleTools.systemInfo(),
      'State      ' + (r.state ? r.state.length + ' bytes (state.sta)' : 'none: ' + (r.stateError || 'no state was saved')),
    ].join('\n');
  }

  async function reportZip() {
    if (dropIfOnline() || !report) return;   /* a room started: no state leaves the page */
    const r = report, enc = new TextEncoder();
    const png = new Uint8Array(await r.png.arrayBuffer());
    if (dropIfOnline()) return;
    const files = [
      { name: 'report.txt', bytes: enc.encode(reportText() + '\n') },
      { name: 'screenshot.png', bytes: png },
      { name: 'console.txt', bytes: enc.encode(m2hleTools.consoleText() + '\n') },
    ];
    if (r.state) files.push({ name: 'state.sta', bytes: r.state });
    download(zip(files), stamp(r) + '-report.zip');
    $('rp-saved').hidden = false;
  }

  /* The issue's body rides in the URL, which GitHub cuts off at about 8 KB. */
  function openIssue() {
    if (dropIfOnline() || !report) return;
    const desc = $('rp-text').value.trim();
    const title = '[web] ' + (desc.split('\n')[0].slice(0, 80) || report.info.game + ': ');
    let body = reportText();
    if (body.length > 5000) body = body.slice(0, 5000) + '\n...';
    body += '\n\n' + t('report.attach', { file: stamp(report) + '-report.zip' });
    window.open(ISSUES + '?title=' + encodeURIComponent(title) + '&body=' + encodeURIComponent(body),
                '_blank', 'noopener');
  }

  function showReport() {
    const r = report;
    $('rp-busy').hidden = true;
    $('rp-ready').hidden = !r;
    $('rp-error').hidden = !!r;
    if (!r) { $('rp-error').textContent = t('report.error'); return; }
    $('rp-img').src = URL.createObjectURL(r.png);
    $('rp-frame').textContent = t('report.frame', { game: r.info.game, frame: r.info.frame });
    $('rp-state').textContent = r.state ? t('report.state.with')
      : t('report.state.without', { why: r.stateError || t('report.state.none') });
    $('rp-saved').hidden = true;
  }

  async function begin() {
    $('rp-busy').hidden = false;
    $('rp-ready').hidden = true;
    $('rp-error').hidden = true;
    const shot = await grab(true);
    if (!open) return;   /* closed before the frame came */
    held = !!shot && M._web_paused() !== 1 && M._web_report_hold(1) === 1;
    const png = shot && await toPng(shot);
    if (dropIfOnline() || !open) return;
    report = shot ? { ...shot, png } : null;
    showReport();
  }

  function toggle(want) {
    open = want === undefined ? !open : want;
    $('report').hidden = !open;
    $('btn-report').setAttribute('aria-expanded', String(open));
    if (open) {
      for (const [panel, close] of PANELS) if (!$(panel).hidden) $(close).click();
      begin();
      return;
    }
    if (held) M._web_report_hold(0);
    held = false;
    report = null;   /* the picture and the state go with the panel */
    if ($('rp-img').src) URL.revokeObjectURL($('rp-img').src);
    $('canvas').focus();
  }

  /* ---- A state back ---------------------------------------------------------- */

  /* A .sta, or a report's zip with one inside. True if the file was one of ours. */
  async function loadFile(file) {
    if (!file || !M || !/\.(sta|zip)$/i.test(file.name)) return false;
    if (!allowed()) { m2hleTools.printErr('state not loaded: not while playing online'); return true; }
    const bytes = new Uint8Array(await file.arrayBuffer());
    const sta = /\.sta$/i.test(file.name) ? bytes : unzipEntry(bytes, 'state.sta');
    if (!sta) {
      if (unzipEntry(bytes, 'report.txt')) m2hleTools.printErr('state not loaded (' + file.name + '): no stored state.sta in it');
      return false;
    }
    const ptr = M._malloc(sta.length);
    M.HEAPU8.set(sta, ptr);
    const ok = M._web_state_load(ptr, sta.length);   /* takes ptr */
    if (ok) m2hleTools.print('state loaded: ' + file.name);
    else m2hleTools.printErr('state not loaded (' + file.name + '): ' + M.UTF8ToString(M._web_state_error()));
    return true;
  }

  /* ---- Wiring ---------------------------------------------------------------- */

  function render() {
    const ok = allowed();
    $('btn-shot').hidden = !ok;
    $('btn-report').hidden = !ok;
    if (!ok) dropIfOnline();   /* a room started under it */
  }

  function wire() {
    $('btn-shot').addEventListener('click', screenshot);
    $('btn-report').addEventListener('click', () => toggle());
    $('rp-close').addEventListener('click', () => toggle(false));
    $('rp-zip').addEventListener('click', reportZip);
    $('rp-issue').addEventListener('click', openIssue);
    $('rp-copy').addEventListener('click', (e) => { if (!dropIfOnline() && report) m2hleTools.copy(reportText(), e.currentTarget); });
    for (const id of ['btn-online', 'btn-ggpo', 'btn-controls']) $(id).addEventListener('click', () => toggle(false));
  }

  function onReady(module) { M = module; }

  /* The game has loaded: the menu items come and go with netplay from here on. */
  function onGame() {
    if (!M || !M._web_shot_arm || onGame.done) return;
    onGame.done = true;
    wire();
    setInterval(render, 250);
    render();
  }

  return { onReady, onGame, loadFile, toggle, grab, zip, unzipEntry };
})();
