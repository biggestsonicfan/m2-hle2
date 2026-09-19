/*
 * The page's half of the web build. Loaded BEFORE m2hle.js, because Emscripten
 * reads the global `Module` as it starts.
 *
 * What is here today is step 1 of the wizard (WEB-PORT.md section 5): hand the
 * emulator the player's own game file. The file is read in this page and passed
 * to the wasm heap; it is never sent anywhere.
 */
'use strict';

const $ = (id) => document.getElementById(id);
const steps = ['step-loading', 'step-rom', 'step-busy', 'step-fatal'];

function show(step) {
  for (const s of steps) $(s).hidden = s !== step;
  $('panel').hidden = false;
}

function fatal(text) {
  $('fatal-text').textContent = text;
  show('step-fatal');
}

/* What a failed load means, in words a player can act on. The emulator matches
 * files by checksum, so it knows exactly which ones the zip does not hold. */
function explainMissing(missing) {
  const names = missing.split(' ').filter(Boolean);
  if (names.length === 0) {
    return 'That file could not be read as a zip. Choose your merged sfight.zip or schamp.zip.';
  }
  const first = names[0];
  if (/^epr-1900[12]\./.test(first)) {
    return 'This zip does not have the Sonic the Fighters program files (' + first + '). ' +
           'It looks like a split "schamp" set. You need a merged set: one zip that holds ' +
           'both sfight and schamp.';
  }
  return 'This zip is missing ' + first + ', which the game needs. It looks like a split ' +
         '"sfight" set, which keeps those files in a separate schamp.zip. You need a merged ' +
         'set: one zip that holds both.';
}

function loadZip(bytes) {
  show('step-busy');
  /* Let the "reading" text paint before the (synchronous) extraction runs. */
  requestAnimationFrame(() => setTimeout(() => {
    const ptr = Module._malloc(bytes.length);
    if (!ptr) { romError('Not enough memory to read that file.'); return; }
    Module.HEAPU8.set(bytes, ptr);           /* HEAPU8 read AFTER malloc: growth replaces the view */
    const rc = Module._web_rom_load(ptr, bytes.length);   /* takes ownership of ptr */
    if (rc !== 0) {
      romError(explainMissing(Module.UTF8ToString(Module._web_rom_missing())));
      return;
    }
    $('panel').hidden = true;
    $('keys').hidden = false;
    $('canvas').focus();
  }, 0));
}

function romError(text) {
  $('rom-error').textContent = text;
  $('rom-error').hidden = false;
  show('step-rom');
}

function readFile(file) {
  if (!file) return;
  $('rom-error').hidden = true;
  file.arrayBuffer().then((buf) => loadZip(new Uint8Array(buf)),
                          () => romError('That file could not be read.'));
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
  console.warn('audio: no worklet (' + why + '); using the ScriptProcessor path');
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
  document.addEventListener('visibilitychange', () => { if (!document.hidden) resume(); });

  ctx.audioWorklet.addModule('m2hle-audio-worklet.js').then(() => {
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
    console.log('audio: worklet, ' + ctx.sampleRate + ' Hz, queue ' + (audio.target / 44.1).toFixed(0) +
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
    box.textContent = lines.join('\n');
  }, 500);
}

/* ---- Emscripten ------------------------------------------------------------ */

var Module = {
  canvas: $('canvas'),
  print: (t) => console.log(t),
  printErr: (t) => console.warn(t),
  onAbort: (what) => fatal(String(what)),

  /* The emulator's side of "Sound", above. */
  m2hleAudioStart: audioStart,
  m2hleAudioPush: audioPush,
  m2hleAudioStats: audioStats,

  /* Called from main_web.c's init(), once the exports can be used. */
  onM2hleReady() {
    show('step-rom');
    debugStart();
    /* Development only: ?rom=<path> loads a zip from THIS site, so a headless
     * browser can boot the game with no file dialog. A production site hosts no
     * ROMs, so there is nothing for it to find; other origins are refused. */
    const dev = new URLSearchParams(location.search).get('rom');
    if (dev) {
      const url = new URL(dev, location.href);
      if (url.origin === location.origin) {
        fetch(url).then((r) => r.ok ? r.arrayBuffer() : Promise.reject(new Error(r.status)))
                  .then((buf) => loadZip(new Uint8Array(buf)))
                  .catch((e) => romError('Could not fetch ' + url.pathname + ' (' + e.message + ').'));
      }
    }
  },
};

if (typeof WebAssembly !== 'object') {
  fatal('It does not support WebAssembly.');
}

/* ---- The file picker and the drop zone ------------------------------------- */

$('file').addEventListener('change', (e) => readFile(e.target.files[0]));

const drop = $('drop');
for (const type of ['dragenter', 'dragover']) {
  window.addEventListener(type, (e) => { e.preventDefault(); drop.classList.add('over'); });
}
for (const type of ['dragleave', 'drop']) {
  window.addEventListener(type, (e) => { e.preventDefault(); drop.classList.remove('over'); });
}
window.addEventListener('drop', (e) => {
  if ($('step-rom').hidden) return;
  readFile(e.dataTransfer && e.dataTransfer.files[0]);
});
