/*
 * The page's half of the web build. Loaded BEFORE m2hle.js, because Emscripten
 * reads the global `Module` as it starts.
 *
 * What is here today is step 1 of the wizard (WEB-PORT.md section 5): hand the
 * emulator the player's own game file. The file is read in this page and passed
 * to the wasm heap; it is never sent anywhere.
 */
'use strict';

/* The words (m2hle-lang.js). rom.missing.* are m2hle-lang.js's own. */
m2hleLang.add({
  en: {
    'page.gl.lost': 'The browser took the graphics away (the WebGL context was lost). Reload the page to carry on.',
    'page.no.wasm': 'It does not support WebAssembly.',
    'page.zip.bad': 'That file could not be read as a zip. Choose your {id}.zip.',
    'page.zip.missing': 'This zip is missing {file}, which {title} needs.',
    'page.sfight.bad': 'That file could not be read as a zip. Choose your merged sfight.zip or schamp.zip, ' +
                       'or sfight.zip and schamp.zip together.',
    'page.sfight.schamp': 'This zip does not have the Sonic the Fighters program files ({file}). ' +
                          'It looks like a split "schamp" set: add sfight.zip too, or choose a merged set.',
    'page.sfight.split': 'This zip is missing {file}, which the game needs. It looks like a split ' +
                         '"sfight" set, which keeps those files in a separate schamp.zip: add schamp.zip too, ' +
                         'or choose a merged set.',
    'page.follow.bad': 'Cannot follow: {error}',
    'page.memory': 'Not enough memory to read that file.',
    'page.unreadable': 'That file could not be read.',
    'page.fetch': 'Could not fetch {what}.',
    'page.build': 'Build {version}',
    'page.fs.enter': 'Fullscreen',
    'page.fs.exit': 'Exit fullscreen',
    'page.pause': 'Pause',
    'page.resume': 'Resume',
    'page.pause.key': '{action} ({key})',
  },
  ja: {
    'page.gl.lost': 'ブラウザがグラフィックスを解放しました（WebGL コンテキストの消失）。続けるにはページを再読み込みしてください。',
    'page.no.wasm': 'WebAssembly に対応していません。',
    'page.zip.bad': 'このファイルは zip として読み込めませんでした。{id}.zip を選んでください。',
    'page.zip.missing': 'この zip には {title} に必要な {file} がありません。',
    'page.sfight.bad': 'このファイルは zip として読み込めませんでした。マージ済みの sfight.zip または schamp.zip、' +
                       'もしくは sfight.zip と schamp.zip を一緒に選んでください。',
    'page.sfight.schamp': 'この zip にはソニック・ザ・ファイターズのプログラムファイル（{file}）がありません。' +
                          'スプリットの「schamp」セットのようです。sfight.zip も追加するか、マージ済みのセットを選んでください。',
    'page.sfight.split': 'この zip にはゲームに必要な {file} がありません。スプリットの「sfight」セットのようです。' +
                         'これらのファイルは別の schamp.zip にあるので、schamp.zip も追加するか、マージ済みのセットを選んでください。',
    'page.follow.bad': 'フォローできません：{error}',
    'page.memory': 'メモリが足りないため、ファイルを読み込めません。',
    'page.unreadable': 'ファイルを読み込めませんでした。',
    'page.fetch': '{what} を取得できませんでした。',
    'page.build': 'ビルド {version}',
    'page.fs.enter': '全画面',
    'page.fs.exit': '全画面を終了',
    'page.pause': 'ポーズ',
    'page.resume': '再開',
    'page.pause.key': '{action}（{key}）',
  },
});

const $ = (id) => document.getElementById(id);

/* The build this page belongs to, stamped into index.html (web/stamp-site.cmake).
 * Every file the page loads by itself is asked for under it, so a cached copy from
 * a previous deploy is never paired with this one. */
const VERSION = document.documentElement.dataset.version || 'dev';
const versioned = (path) => path + '?v=' + encodeURIComponent(VERSION);
const steps = ['step-loading', 'step-rom', 'step-busy', 'step-fatal'];

function show(step) {
  for (const s of steps) $(s).hidden = s !== step;
  $('panel').hidden = false;
}

function fatal(text) {
  m2hleTools.add('fatal: ' + text, 'error');
  $('fatal-text').textContent = text;
  show('step-fatal');
}

/* A browser may take the WebGL context away (a phone's GPU reset, memory pressure).
 * sokol cannot build its resources again, so the picture went black while the
 * board and the sound ran on (Pinboard #601). A follower reloads and joins again;
 * a player is told, since a reload ends the game. One reload a minute at most, so
 * a context lost at every start does not reload for ever. */
$('canvas').addEventListener('webglcontextlost', (e) => {
  e.preventDefault();
  m2hleTools.add('the browser took the graphics away (WebGL context lost)', 'error');
  if (new URLSearchParams(location.search).has('follow') && performance.now() > 60000) {
    location.reload();
    return;
  }
  $('fatal-reload').hidden = false;
  $('fatal-reload').onclick = () => location.reload();
  fatal(m2hleLang.t('page.gl.lost'));
});

/* What a failed load means, in words a player can act on. The emulator matches
 * files by checksum, so it knows exactly which ones the zips do not hold. A
 * clone set (vonj, sfight) can come split, its parent's files in their own zip
 * (von.zip, schamp.zip): the page then holds on to what it has and asks for the
 * parent (`held`, below). */
function explainMissing(missing) {
  const names = missing.split(' ').filter(Boolean);
  const parent = Module.UTF8ToString(Module._web_rom_parent());
  if (m2hleGame.id !== 'sfight' && names.length > 0 && parent && held.length > 0) {
    return m2hleLang.t(held.length > 1 ? 'rom.missing.more' : 'rom.missing.parent',
                       { file: names[0], parent: parent });
  }
  if (m2hleGame.id !== 'sfight') {
    return names.length === 0
      ? m2hleLang.t('page.zip.bad', { id: m2hleGame.id })
      : m2hleLang.t('page.zip.missing', { file: names[0], title: m2hleGame.title });
  }
  if (names.length === 0) return m2hleLang.t('page.sfight.bad');
  const first = names[0];
  if (/^epr-1900[12]\./.test(first)) return m2hleLang.t('page.sfight.schamp', { file: first });
  return m2hleLang.t('page.sfight.split', { file: first });
}

/* The zips the page can give the emulator at once (RL_MEM_ZIPS, rom_loader.h). */
const MAX_ZIPS = 4;
/* The zips of a load that missed files, kept for the next pick: one set split in
 * two zips can be picked one at a time. */
let held = [];

/* `zips`: the bytes of each zip, one set's files across them. */
function loadZip(zips) {
  show('step-busy');
  /* ?follow= names the leader's profile before the game loads (m2hle-follow.js), and
   * the arcade (stf-arcade.mjs, if it loaded) puts the shared board in localStorage. */
  followReady.then(() => window.stfArcade && window.stfArcade.ready)
    .then(() => loadZipNow(zips), (e) => romError(m2hleLang.t('page.follow.bad', { error: e.message })));
}

/* A malloc'd copy of `bytes` in the emulator's heap, or 0. */
function heapCopy(bytes) {
  const ptr = Module._malloc(bytes.length);
  if (ptr) Module.HEAPU8.set(bytes, ptr);   /* HEAPU8 read AFTER malloc: growth replaces the view */
  return ptr;
}

function loadZipNow(zips) {
  /* Let the "reading" text paint before the (synchronous) extraction runs. */
  requestAnimationFrame(() => setTimeout(() => {
    /* ?script= and ?pause= : a run whose inputs are keyed to game frames, so it is
     * the same run every time (main_web.c, "A scripted run"). For tests. */
    if (params.get('script')) {
      const text = params.get('script');
      const n = Module.lengthBytesUTF8(text) + 1, sp = Module._malloc(n);
      Module.stringToUTF8(text, sp, n);
      Module._web_script(sp);
      Module._free(sp);
    }
    if (params.get('pause')) Module._web_pause_at(Number(params.get('pause')) >>> 0);

    /* The first zip is web_rom_load's, the others go before it (web_rom_extra);
     * both take the buffers over. */
    const ptrs = zips.map(heapCopy);
    if (ptrs.includes(0)) {
      ptrs.forEach((p) => p && Module._free(p));
      romError(m2hleLang.t('page.memory'));
      return;
    }
    ptrs.slice(1).forEach((p, i) => Module._web_rom_extra(p, zips[i + 1].length));
    const ptr = ptrs[0];
    /* Keep this game's save in localStorage, or not: the arcade asks (Sega Rally asks
     * first; without the arcade, Sonic the Fighters' is kept, as before, and a game
     * that says keepSave in m2hle-games.js). */
    Module._web_backup_keep(window.stfArcade ? (window.stfArcade.keepSave() ? 1 : 0)
                                             : (m2hleGame.id === 'sfight' || m2hleGame.keepSave ? 1 : 0));
    const rc = Module._web_rom_load(ptr, zips[0].length);   /* takes ownership of ptr */
    held = rc !== 0 && zips.length < MAX_ZIPS ? zips : [];
    if (rc !== 0) {
      romError(explainMissing(Module.UTF8ToString(Module._web_rom_missing())));
      return;
    }
    $('panel').hidden = true;
    keepRunning();
    /* Backup RAM (the test menu's settings, rankings) is kept once a minute;
     * this keeps the last of it when the tab goes. */
    addEventListener('pagehide', () => Module._web_backup_flush());
    if (m2hleFollow.active) m2hleFollow.onGame();   /* no netplay: the board is the leader's */
    else { m2hleNetplay.onGame(); m2hleGgpo.onGame(); }
    m2hleReport.onGame();
    m2hleCheats.onGame();
    if (window.stfArcade) window.stfArcade.onGame(m2hleFollow.active ? 'follow' : 'solo');
    m2hleTouch.onGame();
    pauseOnGame();
    $('canvas').focus();
  }, 0));
}

/* The board runs on the wall clock, whether or not the browser is drawing. A
 * hidden tab gets no animation frames and a throttled window gets them late, so
 * a worker's timer -- which browsers do not throttle the way they throttle a
 * hidden page's own timers -- ticks several times a frame, and the emulator runs
 * the board from it whenever frames have gone quiet (web_background_tick, which
 * does nothing while they arrive). The GPU work stops with the frames, the sound
 * with the AudioContext (audioStart). Measure per browser: WEB-NETPLAY.md 5.3. */
let runWorker = null;
/* Settled once ?follow='s profile is in (at once without one). */
let followReady = Promise.resolve();

function keepRunning() {
  if (runWorker) return;
  const src = 'setInterval(() => postMessage(0), 8);';
  runWorker = new Worker(URL.createObjectURL(new Blob([src], { type: 'text/javascript' })));
  runWorker.onmessage = () => Module._web_background_tick();
  let wasHidden = false;
  document.addEventListener('visibilitychange', () => {
    if (document.hidden === wasHidden) return;
    wasHidden = document.hidden;
    m2hleTools.print(wasHidden ? 'page hidden: the game keeps running, without picture or sound'
                               : 'page visible again');
  });
}

function romError(text) {
  m2hleTools.add('game file refused: ' + text, 'warning');
  $('rom-error').textContent = text;
  $('rom-error').hidden = false;
  show('step-rom');
}

/* The picked or dropped files, after the zips a failed load left (`held`). */
function readFiles(files) {
  files = Array.from(files || []).slice(0, MAX_ZIPS);
  if (files.length === 0) return;
  $('rom-error').hidden = true;
  Promise.all(files.map((f) => f.arrayBuffer()))
    .then((bufs) => loadZip(held.concat(bufs.map((b) => new Uint8Array(b))).slice(-MAX_ZIPS)),
          () => romError(m2hleLang.t('page.unreadable')));
}

/* ---- Sound ------------------------------------------------------------------
 *
 * An AudioWorklet (m2hle-audio-worklet.js) plays the sound and holds the only
 * queue; this side makes the context, posts the chunks the emulator renders each
 * frame, and steers the resampling ratio from the fill the worklet reports. Why
 * a worklet and not the emulator's own audio callback: src/core/audio_out.h,
 * "The push model". If worklets are not available the emulator falls back to
 * that callback (a ScriptProcessorNode), which works and is later.
 *
 * ?audioms=N sets the worklet's queue in milliseconds, for finding where a given
 * machine starts to drop out. The default covers one dropped display frame.
 * ?audio=fallback forces the ScriptProcessor path, so it can be tested in a
 * browser that would never take it.
 */
const params = new URLSearchParams(location.search);
const audio = {
  mode: 'pending',              /* 'worklet' | 'fallback' */
  ctx: null,
  node: null,
  target: Math.round(44.1 * Math.max(10, Math.min(250, Number(params.get('audioms')) || 40))),
  fill: 0,
  fillAvg: 0,
  buffering: true,
  underruns: 0,
  resyncs: 0,
};

function audioFallback(why) {
  m2hleTools.printErr('[WARN] audio: no worklet (' + why + '); using the ScriptProcessor path');
  if (audio.ctx) { audio.ctx.close().catch(() => {}); audio.ctx = null; }
  audio.mode = 'fallback';
  Module._web_audio_use_fallback();
}

function audioStart() {
  if (params.get('audio') === 'fallback') { audioFallback('asked for by ?audio=fallback'); return; }
  const Ctx = window.AudioContext || window.webkitAudioContext;
  if (!Ctx) { audioFallback('no WebAudio'); return; }
  try {
    audio.ctx = new Ctx({ sampleRate: 44100, latencyHint: 'interactive' });
  } catch (e) {
    try { audio.ctx = new Ctx({ latencyHint: 'interactive' }); } catch (e2) { audioFallback(String(e2)); return; }
  }
  const ctx = audio.ctx;
  if (!ctx.audioWorklet) { audioFallback('insecure context or old browser'); return; }

  /* A browser keeps a new context suspended until the visitor does something.
   * Not {once: true}: a context can be interrupted again later (a phone call,
   * another tab taking the device), and these are cheap. */
  const resume = () => { if (audio.ctx && audio.ctx.state !== 'running') audio.ctx.resume().catch(() => {}); };
  for (const type of ['pointerdown', 'keydown', 'touchend']) document.addEventListener(type, resume, true);
  /* Hidden, the board keeps running (keepRunning below) but nobody is listening:
   * the sound stops with the picture, and audioPush drops what the board makes
   * meanwhile, so it comes back with what is happening now. */
  document.addEventListener('visibilitychange', () => {
    if (!document.hidden) resume();
    else if (audio.ctx && audio.ctx.state === 'running') audio.ctx.suspend().catch(() => {});
  });

  ctx.audioWorklet.addModule(versioned('m2hle-audio-worklet.js')).then(() => {
    /* audio.target is in board frames (44.1 kHz); the worklet counts the context's. */
    const target = Math.round(audio.target * ctx.sampleRate / 44100);
    audio.node = new AudioWorkletNode(ctx, 'm2hle-out', {
      numberOfInputs: 0,
      numberOfOutputs: 1,
      outputChannelCount: [2],
      processorOptions: { target },
    });
    audio.fillAvg = target;
    audio.node.port.onmessage = (e) => {
      const m = e.data;
      audio.fill = m.fill;
      audio.buffering = m.buffering;
      audio.underruns = m.underruns;
      audio.resyncs = m.resyncs;
      if (m.buffering) { audio.fillAvg = target; Module._web_audio_set_nudge(0); return; }
      /* The reported fill is caught at a random point of its sawtooth (up a chunk,
       * down 128 frames at a time), so steer by an average of it. At most 1%:
       * the two clocks differ by far less, and more would be heard as pitch. */
      audio.fillAvg += (m.fill - audio.fillAvg) * 0.1;
      const err = Math.max(-1, Math.min(1, (audio.fillAvg - target) / target));
      Module._web_audio_set_nudge(0.01 * err);
    };
    audio.node.connect(ctx.destination);
    audio.mode = 'worklet';
    Module._web_audio_use_worklet(ctx.sampleRate);
    m2hleTools.print('audio: worklet, ' + ctx.sampleRate + ' Hz, queue ' + (audio.target / 44.1).toFixed(0) +
                     ' ms, base latency ' + (ctx.baseLatency * 1000).toFixed(0) + ' ms');
  }).catch((e) => audioFallback(String(e)));
}

/* Called by the emulator once per display frame with what the board produced. */
function audioPush(ptr, frames) {
  /* A suspended context runs no worklet, and messages to it would pile up in the
   * port without bound. Drop the audio instead: when sound starts it starts with
   * what is happening now, not with everything since the page loaded. */
  if (audio.mode !== 'worklet' || audio.ctx.state !== 'running') return;
  const chunk = new Float32Array(Module.HEAPF32.subarray(ptr >> 2, (ptr >> 2) + frames * 2));
  audio.node.port.postMessage(chunk, [chunk.buffer]);
}

/* For tools/web-smoke.mjs and the ?debug readout. Queue in milliseconds. */
function audioStats() {
  if (audio.mode === 'worklet') {
    const ctx = audio.ctx;
    return {
      mode: 'worklet', state: ctx.state, rate: ctx.sampleRate,
      queueMs: audio.fill * 1000 / ctx.sampleRate, targetMs: audio.target / 44.1,
      buffering: audio.buffering, underruns: audio.underruns, resyncs: audio.resyncs,
      baseMs: (ctx.baseLatency || 0) * 1000, outputMs: (ctx.outputLatency || 0) * 1000,
    };
  }
  if (audio.mode === 'fallback') {
    const ctx = Module._saudio_context;
    return {
      mode: 'fallback', state: ctx ? ctx.state : 'none', rate: ctx ? ctx.sampleRate : 0,
      queueMs: Module._web_audio_queued() / 44.1, targetMs: 2048 / 44.1, buffering: false,
      underruns: Module._web_audio_underruns(), resyncs: Module._web_audio_resyncs(),
      baseMs: ctx ? (ctx.baseLatency || 0) * 1000 : 0, outputMs: ctx ? (ctx.outputLatency || 0) * 1000 : 0,
    };
  }
  return { mode: audio.mode };
}

/* ?debug: the numbers that decide how late the sound is, on THIS machine. The
 * device's own latency is the one part no test of ours can see. */
function debugStart() {
  if (!params.has('debug')) return;
  const box = document.createElement('pre');
  box.className = 'debug';
  document.body.appendChild(box);
  let last = 0, lastT = performance.now();
  setInterval(() => {
    if (!Module._web_frames) return;
    const now = performance.now(), frames = Module._web_frames();
    const fps = (frames - last) * 1000 / (now - lastT);
    last = frames; lastT = now;
    const a = audioStats();
    const lines = ['game ' + fps.toFixed(1) + ' fps', 'audio ' + a.mode + (a.state ? ' (' + a.state + ')' : '')];
    if (a.rate) {
      lines.push('queue    ' + a.queueMs.toFixed(0) + ' ms  (target ' + a.targetMs.toFixed(0) + ')' + (a.buffering ? '  buffering' : ''));
      lines.push('base     ' + a.baseMs.toFixed(0) + ' ms');
      lines.push('device   ' + a.outputMs.toFixed(0) + ' ms');
      lines.push('total   ~' + (a.queueMs + a.baseMs + a.outputMs).toFixed(0) + ' ms');
      /* The worklet counts dropouts; the fallback's callback counts samples it had to hold. */
      lines.push((a.mode === 'fallback' ? 'held samples ' : 'dropouts ') + a.underruns + '   resyncs ' + a.resyncs);
    }
    /* The sound BOARD: voices sounding, and whether the game's command bytes all
     * arrived. If the music is missing, this line is the evidence. */
    if (Module._web_sound_status) {
      const b = JSON.parse(Module.UTF8ToString(Module._web_sound_status()));
      let voices = 0;
      for (let v = b.active >>> 0; v; v &= v - 1) voices++;
      lines.push('board    ' + voices + ' voices   midi ' + b.midi_writes + ' sent, ' + b.midi_drops +
                 ' lost, peak ' + b.midi_hi + '/31, ' + b.midi_drains + ' drains');
    }
    box.textContent = lines.join('\n');
  }, 500);
}

/* "Build r123-abc1234": a deployed build names its commit, so it links to it. */
function buildLine() {
  const el = $('build');
  const sha = /-([0-9a-f]{7,40})$/.exec(VERSION);
  if (!sha) { el.textContent = m2hleLang.t('page.build', { version: VERSION }); return; }
  /* The link goes where the language puts the version. */
  const [before, after] = m2hleLang.t('page.build').split('{version}');
  el.textContent = before;
  const a = document.createElement('a');
  a.href = 'https://github.com/biggestsonicfan/m2-hle2/commit/' + sha[1];
  a.target = '_blank';
  a.rel = 'noopener noreferrer';
  a.textContent = VERSION;
  el.append(a, after || '');
}

/* ---- Emscripten ------------------------------------------------------------ */

var Module = {
  canvas: $('canvas'),
  /* ?args=--fill-ref,--no-mesh-cache reaches main_web.c as argv: the switches that
   * turn one render optimisation off each, for finding which one drew it wrong. */
  arguments: (params.get('args') || '').split(',').filter((a) => /^--[a-z-]+$/.test(a))
    .concat(m2hleGame.args),   /* the game, and Sega Rally's wheel (m2hle-game.js) */
  locateFile: (path) => versioned(path),      /* m2hle.wasm */
  /* Everything the emulator prints, into the Console (and on to devtools). */
  print: m2hleTools.print,
  printErr: m2hleTools.printErr,
  /* A GPU timer query around a frame's GL work, while a lag check is measuring. */
  m2hleFrameBegin: m2hleTools.frameBegin,
  m2hleFrameEnd: m2hleTools.frameEnd,
  onAbort: (what) => fatal(String(what)),

  /* The emulator's side of "Sound", above. */
  m2hleAudioStart: audioStart,
  m2hleAudioPush: audioPush,
  m2hleAudioStats: audioStats,

  /* Called from main_web.c's init(), once the exports can be used. */
  onM2hleReady() {
    followReady = m2hleFollow.prepare(Module);
    if (m2hleFollow.refused) m2hleTools.add('?follow= names another site: not followed', 'warning');
    show('step-rom');
    buildLine();
    m2hleTools.onReady();
    m2hleObjview.onReady();
    m2hleShader.onReady();
    m2hleCrt.onReady();
    m2hleReplay.onReady(Module);
    m2hleCheats.onReady(Module);
    m2hleReport.onReady(Module);
    m2hleNetplay.onReady(Module);
    m2hleGgpo.onReady(Module);
    m2hlePad.onReady();
    /* A light-gun game reloads on the right button (main_web.c, web_gun_event):
     * no context menu over the canvas then. */
    const canvas = document.getElementById('canvas');
    if (canvas) canvas.addEventListener('contextmenu', (e) => { if (Module._web_lightgun_on()) e.preventDefault(); });
    debugStart();
    /* Development only: ?rom=<path> loads a zip from THIS site, so a headless
     * browser can boot the game with no file dialog; a split set gives one per
     * zip (?rom=vonj.zip&rom=von.zip). A production site hosts no ROMs, so there
     * is nothing for it to find; other origins are refused. */
    const urls = new URLSearchParams(location.search).getAll('rom')
      .map((dev) => new URL(dev, location.href)).filter((url) => url.origin === location.origin);
    if (urls.length > 0) {
      Promise.all(urls.map((url) => fetch(url)
        .then((r) => r.ok ? r.arrayBuffer() : Promise.reject(new Error(url.pathname + ': ' + r.status)))))
        .then((bufs) => loadZip(bufs.map((b) => new Uint8Array(b))))
        .catch((e) => romError(m2hleLang.t('page.fetch', { what: e.message })));
    }
  },
};

if (typeof WebAssembly !== 'object') {
  fatal(m2hleLang.t('page.no.wasm'));
}

/* ---- The file picker and the drop zone ------------------------------------- */

$('file').addEventListener('change', (e) => { readFiles(e.target.files); e.target.value = ''; });

const drop = $('drop');
for (const type of ['dragenter', 'dragover']) {
  window.addEventListener(type, (e) => { e.preventDefault(); drop.classList.add('over'); });
}
for (const type of ['dragleave', 'drop']) {
  window.addEventListener(type, (e) => { e.preventDefault(); drop.classList.remove('over'); });
}
window.addEventListener('drop', (e) => {
  const files = e.dataTransfer ? e.dataTransfer.files : [];
  if (!$('step-rom').hidden) readFiles(files);
  else if ($('panel').hidden) m2hleReport.loadFile(files[0]);   /* a savestate, or a report's zip (m2hle-report.js) */
});

/* ---- The menu ----------------------------------------------------------------
 *
 * The bar holds the source link, Pause and this menu, and the menu holds
 * everything else. Its items keep the ids and the handlers they had in the bar, so
 * the panels (m2hle-netplay.js, m2hle-pad.js, m2hle-tools.js) and the test scripts
 * press them by id whether or not the menu is open. */

/* Off the bar and back to the game: the bar fades out again (m2hle.css uses
 * :focus-visible for that) and the keys go where a player expects them. */
function leaveBar() {
  const canvas = $('canvas');
  if (document.activeElement && document.activeElement.closest('.bar')) {
    if ($('panel').hidden) canvas.focus();
    else document.activeElement.blur();
  }
}

function menuShow(open) {
  const menu = $('menu');
  if (open === undefined) open = menu.hidden;
  menu.hidden = !open;
  $('btn-menu').setAttribute('aria-expanded', String(open));
  if (!open) leaveBar();
}

$('btn-menu').addEventListener('click', () => menuShow());
/* An item does its own thing (a handler of its own, registered by the panel it
 * belongs to); the menu just closes behind it. */
$('menu').addEventListener('click', (e) => { if (e.target.closest('button')) menuShow(false); });
document.addEventListener('pointerdown', (e) => {
  if (!$('menu').hidden && !e.target.closest('.bar')) menuShow(false);
}, true);
document.addEventListener('keydown', (e) => {
  if (e.key === 'Escape' && !$('menu').hidden) { menuShow(false); $('btn-menu').focus(); }
});

/* ---- Fullscreen --------------------------------------------------------------
 *
 * The whole page, not the canvas alone: the touch buttons, the Controls panel and
 * this bar are all outside the canvas and have to come along. iPhone Safari only
 * fullscreens video, so there the button is never shown. */
const fsElement = () => document.fullscreenElement || document.webkitFullscreenElement;
const fsRequest = document.documentElement.requestFullscreen || document.documentElement.webkitRequestFullscreen;
const fsExit = document.exitFullscreen || document.webkitExitFullscreen;
if (fsRequest && (document.fullscreenEnabled || document.webkitFullscreenEnabled)) {
  const btn = $('btn-fullscreen');
  btn.hidden = false;
  btn.addEventListener('click', () => {
    const p = fsElement() ? fsExit.call(document) : fsRequest.call(document.documentElement, { navigationUI: 'hide' });
    if (p && p.catch) p.catch((e) => m2hleTools.add('fullscreen refused: ' + e.message, 'warning'));
  });
  const label = () => {
    btn.textContent = m2hleLang.t(fsElement() ? 'page.fs.exit' : 'page.fs.enter');
    btn.setAttribute('aria-pressed', String(!!fsElement()));
    /* Going either way leaves this button focused, and a focused button in the
     * bar is one the keyboard could be driving: hand the keys back to the game. */
    leaveBar();
  };
  document.addEventListener('fullscreenchange', label);
  document.addEventListener('webkitfullscreenchange', label);
}

/* ---- Pause -------------------------------------------------------------------
 *
 * Offline play only: the board stands still (main_web.c, web_set_paused) and the
 * sound with it, until Pause is pressed again. In a Console game it opens the
 * PS3's own pause menu (the shell's SELECT) instead, and Pause again resumes. In a room the other boards run on,
 * so the button goes away there, and a pause still on when a room starts is let
 * go by the emulator itself. P does the same, unless the player has bound P to
 * the game; so does the keyboard's Pause key. */
function pauseRender() {
  const btn = $('btn-pause');
  const how = Module._web_paused();   /* 1 the page's freeze, 2 the Console's own pause menu */
  const on = how !== 0;
  btn.hidden = !Module._web_pause_allowed();
  btn.setAttribute('aria-pressed', String(on));
  const key = m2hleKeys.binds.p1.pause[0];
  const action = m2hleLang.t(on ? 'page.resume' : 'page.pause');
  btn.title = key ? m2hleLang.t('page.pause.key', { action, key: m2hleKeys.keyName(key) }) : action;
  btn.setAttribute('aria-label', action);
  $('paused').hidden = how !== 1;   /* the PS3's menu says so itself */
}

function pauseToggle() {
  if (!Module._web_pause_allowed()) return;
  const on = !!Module._web_set_paused(Module._web_paused() ? 0 : 1);   /* the Console's menu opens a frame later */
  m2hleTools.print(on ? 'paused' : 'resumed');
  pauseRender();
}

function pauseOnGame() {
  if (pauseOnGame.done) return;
  pauseOnGame.done = true;
  $('btn-pause').addEventListener('click', () => { pauseToggle(); leaveBar(); });
  /* The keys and pad buttons bound to Pause in the Controls panel (P and the Pause
   * key unless the player changes them; m2hle-keys.js, m2hle-pad.js). */
  window.addEventListener('m2hle-pause', pauseToggle);
  /* The PS3 pauses by itself when a player's pad goes (TaskPause_WaitTrigger), so a
   * pad that drops out mid-game stops the board here too. */
  window.addEventListener('gamepaddisconnected', () => {
    if (Module._web_pause_allowed() && !Module._web_paused()) pauseToggle();
  });
  setInterval(pauseRender, 250);   /* a room can start, or end, at any time */
  pauseRender();
}
