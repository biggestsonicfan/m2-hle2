/*
 * The page's tools drawer: the emulator's log, and a check for what is making the
 * game feel slow. Loaded before m2hle-page.js, which wires it into `Module`.
 *
 * CONSOLE. Every line m2-hle prints goes through Module.print / printErr, and the
 * emulator already says what each one is: log.h prefixes [INFO] [WARN] [ERR ]
 * [DBG ], and sokol prefixes [sg][error] and the like. Those become the three
 * filters -- General, Warnings, Errors -- and what Copy puts on the clipboard is
 * what the filters show, under a header that says which build, browser and GPU it
 * came from, because that is the first thing anyone reading a pasted log asks.
 *
 * LAG CHECK. "Lag" is one word for several different problems that need opposite
 * advice, so this measures instead of guessing blind: the emulator keeps running
 * totals of where each frame's time goes (main_web.c, g_web_perf), this reads
 * them twice a few seconds apart, and the findings are reasoned from the
 * difference. It can tell apart: the CPU not keeping up with the emulation; the
 * browser drawing too few frames (the GPU, a software renderer, a throttled or
 * 30 Hz display); single long frames; sound dropping out or a slow audio device;
 * and waiting on the other player. When the evidence points at the GPU it can
 * act on it, by drawing the game at a lower resolution (web_set_render_scale).
 * When it finds nothing it says so, with the numbers, and says what is left.
 */
'use strict';

const m2hleTools = (() => {
  const $ = (id) => document.getElementById(id);
  const t0 = performance.now();
  let ready = false;            /* the emulator's exports exist, and its GL context */

  /* ---- Keys typed into the drawer stay in the drawer ---------------------------
   * sokol_app listens for keys on `window`, in the capture phase, and swallows
   * them -- so without this Ctrl+C in the log would reach the game and not the
   * clipboard. This listener is registered first (this script runs before
   * m2hle.js), so it runs first, and can keep the event from sokol entirely. */
  for (const type of ['keydown', 'keyup', 'keypress']) {
    window.addEventListener(type, (e) => {
      if (e.target instanceof Element && e.target.closest('#drawer, #online, #controls, input, textarea, select')) {
        e.stopImmediatePropagation();
      }
    }, true);
  }

  /* ---- The log ------------------------------------------------------------------ */

  const MAX_LINES = 3000, TRIM_CHUNK = 500;
  /* A line repeated within this long of its last appearance is folded into that
   * line as a count. The emulator warns every time it meets something it does not
   * handle, and when that is once a frame the log is four lines repeated two
   * hundred times -- unreadable, and slow to draw. Found the first time this
   * console was opened on a fight: an unimplemented COP command, 67 times. */
  const FOLD_MS = 5000;
  const recent = new Map();     /* text -> its line, while it can still be folded into */
  const events = { general: 0, warning: 0, error: 0 };   /* every occurrence, folded or not */
  const dirty = new Set();      /* drawn lines whose count changed */
  let redrawAll = false;
  const lines = [];             /* { t, level, text, n, row } */
  const counts = { general: 0, warning: 0, error: 0 };
  const shown = { general: true, warning: true, error: true };
  let rendered = 0;             /* lines[] index the DOM is drawn up to */
  let dropped = 0;

  function classify(text) {
    if (/\[ERR \]|\]\[(error|panic)\]|^(Uncaught|RuntimeError|Aborted|abort\()/.test(text)) return 'error';
    if (/\[WARN\]|\]\[warning\]/.test(text)) return 'warning';
    return 'general';
  }

  function add(text, level) {
    text = String(text);
    level = level || classify(text);
    events[level]++;
    const now = performance.now();
    const seen = recent.get(text);
    if (seen && now - seen.last < FOLD_MS) {
      seen.n++;
      seen.last = now;
      if (seen.row) dirty.add(seen);
      scheduleRender();
      return;
    }
    const line = { t: (now - t0) / 1000, level, text, n: 1, last: now, row: null };
    lines.push(line);
    recent.set(text, line);
    if (recent.size > 256) {
      for (const [k, v] of recent) if (now - v.last >= FOLD_MS) recent.delete(k);
      if (recent.size > 256) recent.clear();
    }
    counts[level]++;
    /* Trimmed in chunks, and the list redrawn whole when it is: cheaper than keeping
     * the DOM and the array in step one line at a time during a flood of warnings. */
    if (lines.length > MAX_LINES + TRIM_CHUNK) {
      for (const gone of lines.splice(0, TRIM_CHUNK)) counts[gone.level]--;
      dropped += TRIM_CHUNK;
      redrawAll = true;
    }
    scheduleRender();
  }

  /* The emulator's two streams. Mirrored to the browser's own console by level, so
   * devtools and tools/web-smoke.mjs see exactly what this shows. */
  function print(text) { add(text); console.log(text); }
  function printErr(text) {
    const level = classify(text);
    add(text, level);
    (level === 'error' ? console.error : level === 'warning' ? console.warn : console.log)(text);
  }

  window.addEventListener('error', (e) => add('Uncaught ' + (e.message || e.error), 'error'));
  window.addEventListener('unhandledrejection', (e) => add('Uncaught (in promise) ' + (e.reason && e.reason.message || e.reason), 'error'));

  const stamp = (l) => l.t.toFixed(1).padStart(7) + 's  ';
  const shownText = (l) => stamp(l) + l.text + (l.n > 1 ? '    (x' + l.n + ')' : '');
  let renderQueued = false;
  function scheduleRender() {
    if (renderQueued) return;
    renderQueued = true;
    requestAnimationFrame(() => { renderQueued = false; render(); });
  }

  function render() {
    for (const k of ['general', 'warning', 'error']) {
      const el = $('count-' + k);
      if (el) el.textContent = counts[k];
    }
    const badge = $('console-badge');
    if (badge) {
      badge.textContent = counts.error || '';
      badge.hidden = counts.error === 0;
    }
    const log = $('log');
    if (!log || $('drawer').hidden || $('pane-console').hidden) return;   /* drawn on open */
    const stick = log.scrollHeight - log.scrollTop - log.clientHeight < 24;
    if (redrawAll) { log.textContent = ''; rendered = 0; redrawAll = false; for (const l of lines) l.row = null; }
    for (const l of dirty) if (l.row) l.row.textContent = shownText(l);
    dirty.clear();
    const frag = document.createDocumentFragment();
    for (; rendered < lines.length; rendered++) {
      const l = lines[rendered];
      if (!shown[l.level]) continue;
      const row = document.createElement('div');
      row.className = 'ln ln-' + l.level;
      row.textContent = shownText(l);
      l.row = row;
      frag.appendChild(row);
    }
    log.appendChild(frag);
    $('log-empty').hidden = log.childElementCount > 0;
    if (stick) log.scrollTop = log.scrollHeight;
  }

  function rerender() {
    const log = $('log');
    redrawAll = true;
    render();
    log.scrollTop = log.scrollHeight;
  }

  function systemInfo() {
    const g = gpuInfo();
    const c = $('canvas');
    return [
      'Build      ' + (document.documentElement.dataset.version || 'dev'),
      'When       ' + new Date().toISOString(),
      'Browser    ' + navigator.userAgent,
      'GPU        ' + (g.renderer || 'unknown') + (g.software ? '   (SOFTWARE RENDERER)' : ''),
      'Canvas     ' + c.width + 'x' + c.height + ' at devicePixelRatio ' + window.devicePixelRatio,
      'Cores      ' + (navigator.hardwareConcurrency || '?') + (navigator.deviceMemory ? ', ~' + navigator.deviceMemory + ' GB memory' : ''),
    ];
  }

  function consoleText() {
    const which = ['general', 'warning', 'error'].filter((k) => shown[k]);
    const out = ['Sonic the Fighters (m2-hle web) -- console', ...systemInfo(),
                 'Showing    ' + which.join(', ') + (dropped ? '   (' + dropped + ' older lines were discarded)' : ''), ''];
    for (const l of lines) if (shown[l.level]) out.push(shownText(l));
    return out.join('\n');
  }

  async function copy(text, button) {
    let ok = false;
    try {
      await navigator.clipboard.writeText(text);
      ok = true;
    } catch (e) {
      /* No clipboard permission (or not a secure context): the old way. */
      const ta = document.createElement('textarea');
      ta.value = text;
      ta.style.position = 'fixed';
      ta.style.opacity = '0';
      document.body.appendChild(ta);
      ta.select();
      try { ok = document.execCommand('copy'); } catch (e2) { ok = false; }
      ta.remove();
    }
    const was = button.textContent;
    button.textContent = ok ? 'Copied' : 'Copy failed';
    setTimeout(() => { button.textContent = was; }, 1500);
  }

  /* ---- The GPU ------------------------------------------------------------------- */

  let gpuCache = null;
  function gpuInfo() {
    if (gpuCache) return gpuCache;
    /* Only once sokol has made its context: asking the canvas for one before then
     * would CREATE it, with the wrong attributes, and sokol would inherit it. */
    if (!ready) return {};
    const gl = $('canvas').getContext('webgl2');
    if (!gl) return {};
    const dbg = gl.getExtension('WEBGL_debug_renderer_info');
    const renderer = String(dbg ? gl.getParameter(dbg.UNMASKED_RENDERER_WEBGL) : gl.getParameter(gl.RENDERER));
    gpuCache = {
      gl, renderer,
      software: /swiftshader|llvmpipe|softpipe|software|basic render/i.test(renderer),
      timer: gl.getExtension('EXT_disjoint_timer_query_webgl2'),
    };
    return gpuCache;
  }

  /* GPU time per frame, where the browser allows it (Chrome on desktop, mostly):
   * a timer query around the frame's GL work, asked for by web_perf_gpu_timing.
   * Results come back some frames later; a "disjoint" reading is thrown away. */
  const gpuTimer = { active: null, pending: [], ns: 0, n: 0 };
  function frameBegin() {
    const g = gpuInfo();
    if (!g.timer || gpuTimer.active || gpuTimer.pending.length > 8) return;
    gpuTimer.active = g.gl.createQuery();
    g.gl.beginQuery(g.timer.TIME_ELAPSED_EXT, gpuTimer.active);
  }
  function frameEnd() {
    const g = gpuInfo();
    if (!g.timer) return;
    if (gpuTimer.active) {
      g.gl.endQuery(g.timer.TIME_ELAPSED_EXT);
      gpuTimer.pending.push(gpuTimer.active);
      gpuTimer.active = null;
    }
    while (gpuTimer.pending.length && g.gl.getQueryParameter(gpuTimer.pending[0], g.gl.QUERY_RESULT_AVAILABLE)) {
      const q = gpuTimer.pending.shift();
      if (!g.gl.getParameter(g.timer.GPU_DISJOINT_EXT)) {
        gpuTimer.ns += g.gl.getQueryParameter(q, g.gl.QUERY_RESULT);
        gpuTimer.n++;
      }
      g.gl.deleteQuery(q);
    }
  }

  /* ---- The lag check --------------------------------------------------------------- */

  const readPerf = (reset) => JSON.parse(Module.UTF8ToString(Module._web_perf(reset ? 1 : 0)));
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  const f1 = (x) => x.toFixed(1);

  async function measure(seconds) {
    let hidden = document.hidden;
    const onVis = () => { if (document.hidden) hidden = true; };
    document.addEventListener('visibilitychange', onVis);
    gpuTimer.ns = 0; gpuTimer.n = 0;
    Module._web_perf_gpu_timing(1);
    readPerf(true);                                   /* clear the maxima */
    const a = readPerf(false), audioA = Module.m2hleAudioStats(), eventsA = { ...events };
    await sleep(seconds * 1000);
    const b = readPerf(true), audioB = Module.m2hleAudioStats();
    Module._web_perf_gpu_timing(0);
    document.removeEventListener('visibilitychange', onVis);

    const T = Math.max(0.001, (b.now_us - a.now_us) / 1e6);
    const d = (k) => b[k] - a[k];
    const g = gpuInfo();
    return {
      seconds: T, hidden,
      drawRate: d('callbacks') / T,                   /* frames the browser let us draw, per second */
      /* Of those, the ones actually drawn: on a display faster than the board, a
       * page short of time draws only the board's own frames rather than
       * interpolating between them (main_web.c, web_afford_picture). */
      pictureRate: (d('callbacks') - d('pictures_skipped')) / T,
      skipped: d('pictures_skipped'),
      speed: d('slices') / T,                         /* emulated frames per second: 60 is full speed */
      sliceMs: d('slices') ? d('slice_us') / d('slices') / 1000 : 0,
      sliceMaxMs: b.slice_us_max / 1000,
      renderMs: d('callbacks') ? d('render_us') / d('callbacks') / 1000 : 0,
      renderMaxMs: b.render_us_max / 1000,
      busy: (d('slice_us') + d('render_us')) / (T * 1e6),   /* share of the main thread we used */
      longFrames: d('long_callbacks'), worstGapMs: b.gap_us_max / 1000,
      forgivenMs: d('forgiven_us') / 1000,
      gpuMs: gpuTimer.n ? gpuTimer.ns / gpuTimer.n / 1e6 : null,
      canvasW: b.canvas_w, canvasH: b.canvas_h, renderScale: b.render_scale,
      renderer: g.renderer || 'unknown', software: !!g.software,
      audio: audioB, dropouts: (audioB.underruns || 0) - (audioA.underruns || 0),
      netplay: b.netplay, stalls: d('netplay_stalls'), netDelay: b.netplay_delay,
      complaints: (events.warning - eventsA.warning) + (events.error - eventsA.error),
    };
  }

  /* What the numbers mean. Each finding: how bad, a plain title, the evidence, what
   * to try, and sometimes something this page can do about it right now. */
  function reason(m) {
    const out = [];
    const find = (level, title, evidence, tips, action) => out.push({ level, title, evidence, tips: tips || [], action });
    const megapixels = m.canvasW * m.canvasH / 1e6;
    const lowerRes = m.renderScale === 0 || m.renderScale > 2
      ? { label: 'Draw the game at lower resolution', run: () => setRenderScale(2) } : null;

    if (m.hidden) {
      find('info', 'This tab was in the background during the check',
           'Browsers all but stop a tab nobody is looking at, so these numbers describe that, not the game.',
           ['Run the check again with the game in front, while playing.']);
      return out;                 /* nothing measured then means anything */
    }

    const slow = m.speed < 57;
    const cpuBound = m.busy > 0.7 || m.sliceMs + m.renderMs > 13;
    if (slow && cpuBound) {
      find('bad', 'This computer is not keeping up with the emulation',
           'The game ran at ' + f1(m.speed) + ' of 60 frames a second. Emulating one frame took ' + f1(m.sliceMs) +
           ' ms on average (worst ' + f1(m.sliceMaxMs) + ') and preparing the picture ' + f1(m.renderMs) +
           ' ms; together they have to fit in 16.7.',
           ['Close other tabs and programs, especially anything else using the CPU.',
            'On a laptop, plug in the charger and turn off battery saver: both slow the CPU down.',
            'Chrome and Edge run this kind of code fastest.']);
    } else if (cpuBound) {
      find('warn', 'This computer is only just keeping up',
           'The game is at full speed (' + f1(m.speed) + '), but emulating a frame takes ' + f1(m.sliceMs) +
           ' ms and the picture ' + f1(m.renderMs) + ' ms of the 16.7 available, so anything else the computer does will cost frames.',
           ['Close other tabs and programs.', 'On a laptop, plug in the charger and turn off battery saver.']);
    } else if (slow) {
      find('bad', 'The browser is not giving the game enough turns to run',
           'The game ran at ' + f1(m.speed) + ' of 60 frames a second, but the emulator itself was only busy ' +
           Math.round(m.busy * 100) + '% of the time. The page was called ' + f1(m.drawRate) +
           ' times a second, and the game can catch up at most three frames per call.',
           ['Something outside the emulator is holding the page up: see the findings below.']);
    }

    /* Everything from here to the sound is also what a slow CPU looks like. When the
     * CPU is already the answer these are its symptoms, not more causes, and listing
     * them sends people after the wrong thing. */
    let drawExplained = false;    /* too few frames drawn, and a finding already says why */
    if (m.drawRate < 50 && !cpuBound) {
      drawExplained = true;
      const near30 = m.drawRate > 27 && m.drawRate < 33;
      if (m.software) {
        find('bad', 'The browser is drawing in software: graphics acceleration is off',
             'It reports its renderer as "' + m.renderer + '", and drew ' + f1(m.drawRate) + ' frames a second.',
             ['Turn on "Use graphics acceleration when available" in the browser\'s settings (System), then restart it.',
              'If it is already on, the graphics driver may be blocked or out of date: update it.'], lowerRes);
      } else if (m.gpuMs !== null && m.gpuMs > 12) {
        find('bad', 'The graphics card is the bottleneck',
             'Each frame took the GPU ' + f1(m.gpuMs) + ' ms, at ' + m.canvasW + 'x' + m.canvasH + ' (' + f1(megapixels) +
             ' million pixels). It has 16.7 ms for 60 frames a second.',
             ['Make the browser window smaller, or use the button below.'], lowerRes);
      } else if (near30) {
        find(slow ? 'bad' : 'warn', 'The picture is being drawn at 30 frames a second',
             'The browser called the page ' + f1(m.drawRate) + ' times a second. ' +
             (slow ? '' : 'The game itself still ran at full speed (' + f1(m.speed) + '), so it plays correctly but looks choppy. ') +
             'Exactly half of 60 usually means something is rationing frames rather than struggling.',
             ['Turn off battery saver / low power mode, and plug in the charger.',
              'Check the display is set to 60 Hz or more, not 30.',
              'If the window is large or the screen is 4K, the GPU may be settling on every other frame: try the button below.'],
             lowerRes);
      } else {
        find(slow ? 'bad' : 'warn', 'The browser is drawing too few frames',
             'It drew ' + f1(m.drawRate) + ' frames a second while the emulator needed only ' + f1(m.sliceMs + m.renderMs) +
             ' ms of each one' + (m.gpuMs === null ? '; this browser does not let the page time the GPU, so this is an inference' : '') +
             '. Canvas: ' + m.canvasW + 'x' + m.canvasH + ' (' + f1(megapixels) + ' million pixels), renderer "' + m.renderer + '".',
             ['Make the window smaller, or use the button below: if that fixes it, it was the GPU.',
              'Close other tabs showing video or animation.'], lowerRes);
      }
    } else if (m.software) {
      find('warn', 'The browser is drawing in software',
           'Renderer: "' + m.renderer + '". It is keeping up right now, but it is using the CPU to do the GPU\'s job.',
           ['Turn on graphics acceleration in the browser\'s settings and restart it.']);
    }

    if (!cpuBound && !drawExplained && (m.longFrames >= Math.max(2, m.seconds * 0.5) || m.worstGapMs > 70)) {
      const ours = m.sliceMaxMs > 20 || m.renderMaxMs > 20;
      find('warn', 'Stutter: some frames arrived late',
           m.longFrames + ' frames in ' + f1(m.seconds) + ' s came more than 25 ms after the one before (worst gap ' +
           f1(m.worstGapMs) + ' ms). ' + (ours
             ? 'The emulator had slow frames of its own (worst emulation ' + f1(m.sliceMaxMs) + ' ms, worst picture ' +
               f1(m.renderMaxMs) + ' ms), which is normal while the game loads a new scene and not in the middle of a fight.'
             : 'The emulator\'s own work never took longer than ' + f1(Math.max(m.sliceMaxMs, m.renderMaxMs)) +
               ' ms, so the pauses came from outside it: the browser, other tabs, or the system.'),
           ours ? ['If it happens mid-fight, run the check again then and copy the report.']
                : ['Close other tabs and programs.', 'Browser extensions that touch every page can do this: try a private window.']);
    }

    if (m.dropouts > 0) {
      find('warn', 'The sound dropped out ' + m.dropouts + ' time' + (m.dropouts === 1 ? '' : 's'),
           'The audio queue ran empty (it holds about ' + Math.round(m.audio.targetMs || 0) + ' ms). The sound is made a frame at a time, so this follows from ' +
           (cpuBound ? 'the slow frames above' : 'late frames') + ' rather than being a problem of its own.',
           [cpuBound ? 'It will stop when the game runs at full speed.'
                     : 'Fix the late frames first. If the sound still breaks up, add ?audioms=80 to the address for a deeper queue (sound will be slightly later).']);
    }
    const deviceMs = (m.audio.baseMs || 0) + (m.audio.outputMs || 0);
    if (deviceMs > 90) {
      find('warn', 'Your audio device adds ' + Math.round(deviceMs) + ' ms of delay',
           'That is the browser\'s own report for the output device, on top of the ' + Math.round(m.audio.queueMs || 0) +
           ' ms this page queues. Bluetooth headphones and speakers are the usual cause.',
           ['Use wired headphones or the computer\'s speakers if sound behind the picture bothers you.']);
    }

    if (m.complaints / m.seconds > 10) {
      const worst = lines.filter((l) => l.level !== 'general').sort((x, y) => y.n - x.n)[0];
      find('warn', 'The emulator is complaining ' + Math.round(m.complaints / m.seconds) + ' times a second',
           'It logged ' + m.complaints + ' warnings or errors in ' + f1(m.seconds) + ' s' +
           (worst ? ', most often: "' + worst.text + '"' : '') + '. Each one costs a little time; more to the ' +
           'point, it means the game is doing something the emulator does not handle yet, and what follows may be drawn or played wrong.',
           ['Open the Console tab, press Copy, and send it with a note of what was on screen.']);
    }

    if (m.netplay === 'playing' && m.stalls > 0) {
      find('warn', 'Waiting for the other player',
           'The game paused ' + m.stalls + ' time' + (m.stalls === 1 ? '' : 's') + ' in ' + f1(m.seconds) +
           ' s because the other player\'s input had not arrived (frame delay ' + m.netDelay + ').',
           ['A higher frame delay absorbs a slower connection, at the cost of slightly later controls.',
            'Wi-Fi on either side is the usual cause: a cable helps more than anything else.']);
    }

    if (!out.some((f) => f.level === 'bad' || f.level === 'warn')) {
      find('ok', 'Nothing here is running slowly',
           'The game ran at ' + f1(m.speed) + ' of 60 frames a second and the browser drew ' + f1(m.drawRate) +
           '. Emulation took ' + f1(m.sliceMs) + ' ms a frame and the picture ' + f1(m.renderMs) + ' ms, of 16.7 available' +
           (m.gpuMs !== null ? '; the GPU took ' + f1(m.gpuMs) + ' ms' : '') + '. ' + m.longFrames + ' late frames; sound queue ' +
           Math.round(m.audio.queueMs || 0) + ' ms.',
           ['If the controls still feel late, the delay is outside this page: a TV not in game mode, a wireless keyboard or controller, or the display itself.',
            'If it only happens sometimes, run this check again while it is happening.']);
    }
    return out;
  }

  function reportText(m, findings) {
    const out = ['Sonic the Fighters (m2-hle web) -- lag check', ...systemInfo(), ''];
    for (const f of findings) {
      out.push('[' + f.level.toUpperCase() + '] ' + f.title, '    ' + f.evidence);
      for (const t of f.tips) out.push('    - ' + t);
      out.push('');
    }
    out.push('Measurements over ' + f1(m.seconds) + ' s:',
      '  game speed        ' + f1(m.speed) + ' / 60 fps',
      '  drawn             ' + f1(m.pictureRate) + ' fps' +
                              (m.skipped ? ' (of ' + f1(m.drawRate) + ' the display asked for; the rest would have been interpolated between board frames)' : ''),
      '  emulation         ' + f1(m.sliceMs) + ' ms avg, ' + f1(m.sliceMaxMs) + ' worst',
      '  picture (CPU)     ' + f1(m.renderMs) + ' ms avg, ' + f1(m.renderMaxMs) + ' worst',
      '  GPU               ' + (m.gpuMs === null ? 'not measurable in this browser' : f1(m.gpuMs) + ' ms avg'),
      '  main thread busy  ' + Math.round(m.busy * 100) + '%',
      '  late frames       ' + m.longFrames + ' (worst gap ' + f1(m.worstGapMs) + ' ms), time dropped ' + f1(m.forgivenMs) + ' ms',
      '  canvas            ' + m.canvasW + 'x' + m.canvasH + ', render scale ' + (m.renderScale || 'full'),
      '  audio             ' + (m.audio.mode || '?') + ', queue ' + Math.round(m.audio.queueMs || 0) + ' ms, device ' +
                              Math.round((m.audio.baseMs || 0) + (m.audio.outputMs || 0)) + ' ms, dropouts ' + m.dropouts,
      '  netplay           ' + m.netplay + (m.netplay === 'playing' ? ', stalls ' + m.stalls : ''),
      '  warnings/errors   ' + m.complaints + ' during the check');
    const lately = lines.filter((l) => l.level !== 'general').slice(-15);
    if (lately.length) {
      out.push('', 'Recent warnings and errors:');
      for (const l of lately) out.push('  ' + shownText(l));
    }
    return out.join('\n');
  }

  let lastReport = '';
  async function diagnose(seconds) {
    seconds = seconds || 5;
    const status = $('lag-status'), list = $('lag-findings'), start = $('lag-start');
    if (!ready || Module._web_state() !== 1) {
      status.textContent = 'Start the game first: there is nothing to measure until it is running.';
      return null;
    }
    start.disabled = true;
    list.textContent = '';
    $('lag-copy').hidden = true;
    /* Hand the keyboard back: the check is only worth anything while playing. */
    $('canvas').focus();
    const pending = measure(seconds);
    for (let s = seconds; s > 0; s--) {
      status.textContent = 'Measuring. Keep playing... ' + s;
      await sleep(1000);
    }
    const m = await pending;
    const findings = reason(m);
    status.textContent = 'Checked ' + f1(m.seconds) + ' seconds of play.';
    for (const f of findings) {
      const card = document.createElement('article');
      card.className = 'finding finding-' + f.level;
      const h = document.createElement('h3'); h.textContent = f.title; card.appendChild(h);
      const p = document.createElement('p'); p.textContent = f.evidence; card.appendChild(p);
      if (f.tips.length) {
        const ul = document.createElement('ul');
        for (const t of f.tips) { const li = document.createElement('li'); li.textContent = t; ul.appendChild(li); }
        card.appendChild(ul);
      }
      if (f.action) {
        const b = document.createElement('button');
        b.className = 'tool';
        b.textContent = f.action.label;
        b.addEventListener('click', () => { f.action.run(); b.textContent = 'Done. Run the check again to compare.'; b.disabled = true; });
        card.appendChild(b);
      }
      list.appendChild(card);
    }
    lastReport = reportText(m, findings);
    add('lag check: ' + findings.map((f) => '[' + f.level + '] ' + f.title).join('; '));
    $('lag-copy').hidden = false;
    start.disabled = false;
    start.textContent = 'Check again';
    return { measurements: m, findings, text: lastReport };
  }

  /* ---- Picture resolution ---------------------------------------------------------- */

  function setRenderScale(n) {
    Module._web_set_render_scale(n);
    try { localStorage.setItem('m2hle.renderScale', String(n)); } catch (e) { /* private mode */ }
    const sel = $('render-scale');
    if (sel) sel.value = String(n);
  }

  /* ---- The drawer ---------------------------------------------------------------------- */

  function openDrawer(tab) {
    const drawer = $('drawer');
    const same = !drawer.hidden && !$('pane-' + tab).hidden;
    if (same) { closeDrawer(); return; }
    drawer.hidden = false;
    for (const t of ['lag', 'console']) {
      $('pane-' + t).hidden = t !== tab;
      $('tab-' + t).setAttribute('aria-selected', String(t === tab));
      $('btn-' + t).setAttribute('aria-expanded', String(t === tab));
    }
    if (tab === 'console') rerender();
    /* Let go of anything held: the key-up will land in here, not in the game. */
    if (ready && Module._web_release_keys) Module._web_release_keys();
  }

  function closeDrawer() {
    $('drawer').hidden = true;
    for (const t of ['lag', 'console']) $('btn-' + t).setAttribute('aria-expanded', 'false');
    $('canvas').focus();
  }

  function init() {
    $('btn-lag').addEventListener('click', () => openDrawer('lag'));
    $('btn-console').addEventListener('click', () => openDrawer('console'));
    $('tab-lag').addEventListener('click', () => openDrawer('lag'));
    $('tab-console').addEventListener('click', () => openDrawer('console'));
    $('drawer-close').addEventListener('click', closeDrawer);
    $('lag-start').addEventListener('click', () => diagnose(5));
    $('lag-copy').addEventListener('click', (e) => copy(lastReport, e.currentTarget));
    $('log-copy').addEventListener('click', (e) => copy(consoleText(), e.currentTarget));
    $('log-clear').addEventListener('click', () => {
      lines.length = 0; rendered = 0; dropped = 0;
      recent.clear(); dirty.clear();
      counts.general = counts.warning = counts.error = 0;
      rerender();
    });
    for (const k of ['general', 'warning', 'error']) {
      $('filter-' + k).addEventListener('click', (e) => {
        shown[k] = !shown[k];
        e.currentTarget.setAttribute('aria-pressed', String(shown[k]));
        rerender();
      });
    }
    $('render-scale').addEventListener('change', (e) => setRenderScale(Number(e.target.value)));
    /* sokol_app re-measures the canvas when the WINDOW resizes, and only then. The
     * drawer changes the canvas's size without that, and the old
     * drawing buffer gets stretched into the new box: a squashed picture. Tell it. */
    if (window.ResizeObserver) {
      new ResizeObserver(() => window.dispatchEvent(new Event('resize'))).observe($('canvas'));
    }
    render();
  }

  /* Called from Module.onM2hleReady: the exports and the GL context exist now. */
  function onReady() {
    ready = true;
    let stored = 0;
    try { stored = Number(localStorage.getItem('m2hle.renderScale')) || 0; } catch (e) { /* private mode */ }
    const asked = new URLSearchParams(location.search).get('scale');
    const scale = asked !== null ? Number(asked) || 0 : stored;
    if (scale) Module._web_set_render_scale(scale);
    $('render-scale').value = String(scale);
    const g = gpuInfo();
    add('web: renderer "' + (g.renderer || 'unknown') + '"' + (g.software ? ' (software)' : '') +
        ', canvas ' + $('canvas').width + 'x' + $('canvas').height + ', devicePixelRatio ' + window.devicePixelRatio);
  }

  document.addEventListener('DOMContentLoaded', init);

  return { add, print, printErr, frameBegin, frameEnd, diagnose, reason, onReady, consoleText, openDrawer };
})();
