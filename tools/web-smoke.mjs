#!/usr/bin/env node
/*
 * web-smoke.mjs — run the web build in a real (headless) browser and report.
 *
 *   node tools/web-smoke.mjs --url http://localhost:8080/?rom=/dev-rom.zip
 *        [--seconds 30] [--shot out.png] [--shot-at 10,20] [--browser path/to/chrome-or-edge]
 *        [--size 992x768] [--expect-frames N] [--keys "5@12,1@14"] [--gesture-audio]
 *        [--expect-log TEXT] [--fail-on-log REGEX]
 *
 * Drives Chrome or Edge over the DevTools protocol in REAL time. Headless
 * "virtual time" is useless here: the game is paced by the wall clock, and
 * virtual time skips ahead the moment the page looks idle.
 *
 * Reports every console line, any uncaught exception, and once a second the
 * emulator's own state (web_state / web_frames, exported by main_web.c), so a run
 * shows whether the board is advancing and how fast. Screenshots are taken at the
 * times asked for and at the end.
 *
 * --keys presses keys at given seconds: "5@12" is key '5' (coin) at t=12 s.
 *
 * --gesture-audio leaves the browser's autoplay policy alone, so WebAudio stays
 * suspended until the first --keys press, as it does for a real visitor. By then
 * the board has filled its output ring with old audio; the run shows whether the
 * queue comes straight back to its target (a resync) or plays out stale.
 *
 * Without a ROM the page stops at "add your game", which is still a test worth
 * having: WebGL2 came up, every shader compiled, and nothing threw. That is the
 * run CI does before it deploys (.github/workflows/pages.yml), with
 *   --expect-log TEXT     fail unless some console line contains TEXT
 *   --fail-on-log REGEX   fail if any console line matches (default: sokol_gfx
 *                         errors and the emulator's own [ERR ] lines)
 * With --expect-frames the exit code says whether the game got that far.
 *
 * No dependencies: Node 22+ has WebSocket and fetch built in.
 */
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

const args = process.argv.slice(2);
const opt = (name, fallback) => {
  const i = args.indexOf(name);
  return i >= 0 && i + 1 < args.length ? args[i + 1] : fallback;
};
const url = opt('--url', 'http://localhost:8080/');
const seconds = Number(opt('--seconds', '30'));
const shot = opt('--shot', null);
const shotAt = (opt('--shot-at', '') || '').split(',').filter(Boolean).map(Number);
const [width, height] = opt('--size', '992x768').split('x').map(Number);
const expectFrames = Number(opt('--expect-frames', '0'));
const expectLog = opt('--expect-log', null);
const failOnLog = new RegExp(opt('--fail-on-log', String.raw`\[sg\]\[(error|panic)\]|\[ERR \]`));
let sawExpected = false;
const keys = (opt('--keys', '') || '').split(',').filter(Boolean).map((k) => {
  const [key, at] = k.split('@');
  return { key, at: Number(at), done: false };
});

const CANDIDATES = [
  opt('--browser', null),
  process.env.CHROME_PATH,
  'C:/Program Files/Google/Chrome/Application/chrome.exe',
  'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',
  'C:/Program Files/Microsoft/Edge/Application/msedge.exe',
  '/usr/bin/google-chrome',
  '/usr/bin/chromium',
  '/usr/bin/chromium-browser',
  '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
].filter(Boolean);
const browser = CANDIDATES.find((p) => fs.existsSync(p));
if (!browser) { console.error('no Chrome or Edge found; pass --browser'); process.exit(2); }

const port = 9300 + Math.floor(Math.random() * 500);
/* One fixed profile directory, reused by every run and never deleted here: a
 * fresh one per run would need a recursive delete to clean up, and this script
 * does not delete directories. Remove it by hand if you want it gone. */
const profile = path.join(os.tmpdir(), 'm2hle-web-smoke-profile');
fs.mkdirSync(profile, { recursive: true });
const child = spawn(browser, [
  '--headless=new', '--no-first-run', '--disable-extensions', '--mute-audio',
  `--user-data-dir=${profile}`, `--remote-debugging-port=${port}`,
  `--window-size=${width},${height}`,
  /* A machine with no usable GPU (CI, a remote session) still gets WebGL2. */
  '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist',
  /* CI runners do not allow the unprivileged user namespaces Chrome's sandbox
   * wants. The only page this browser ever opens is our own build. */
  ...(process.env.CI ? ['--no-sandbox'] : []),
  /* The AudioContext starts suspended until a gesture; nobody is here to click. */
  ...(args.includes('--gesture-audio') ? [] : ['--autoplay-policy=no-user-gesture-required']),
  'about:blank',
], { stdio: 'ignore' });

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function pageSocketUrl() {
  for (let i = 0; i < 100; i++) {
    try {
      const list = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
      const page = list.find((t) => t.type === 'page');
      if (page) return page.webSocketDebuggerUrl;
    } catch { /* not up yet */ }
    await sleep(100);
  }
  throw new Error('the browser never opened its debugging port');
}

let failed = false;
try {
  const ws = new WebSocket(await pageSocketUrl());
  await new Promise((res, rej) => { ws.onopen = res; ws.onerror = rej; });

  let nextId = 1;
  const pending = new Map();
  const send = (method, params = {}) => new Promise((resolve) => {
    const id = nextId++;
    pending.set(id, resolve);
    ws.send(JSON.stringify({ id, method, params }));
  });
  const t0 = Date.now();
  const stamp = () => ((Date.now() - t0) / 1000).toFixed(1).padStart(5);

  ws.onmessage = (ev) => {
    const msg = JSON.parse(ev.data);
    if (msg.id && pending.has(msg.id)) { pending.get(msg.id)(msg.result || msg.error); pending.delete(msg.id); return; }
    if (msg.method === 'Runtime.consoleAPICalled') {
      const text = msg.params.args.map((a) => a.value ?? a.description ?? '').join(' ');
      console.log(`${stamp()}s  console.${msg.params.type}: ${text}`);
      if (expectLog && text.includes(expectLog)) sawExpected = true;
      if (failOnLog.test(text)) { failed = true; console.log(`${stamp()}s  ^ FAIL: matches --fail-on-log`); }
    } else if (msg.method === 'Runtime.exceptionThrown') {
      failed = true;
      const d = msg.params.exceptionDetails;
      console.log(`${stamp()}s  EXCEPTION: ${d.exception?.description || d.text}`);
    }
  };

  await send('Runtime.enable');
  await send('Page.enable');
  await send('Page.navigate', { url });

  const evaluate = async (expression) => {
    const r = await send('Runtime.evaluate', { expression, returnByValue: true });
    return r?.result?.value;
  };
  const screenshot = async (file) => {
    const r = await send('Page.captureScreenshot', { format: 'png' });
    if (r?.data) { fs.writeFileSync(file, Buffer.from(r.data, 'base64')); console.log(`${stamp()}s  screenshot -> ${file}`); }
  };
  const press = async (key) => {
    const code = /^[0-9]$/.test(key) ? `Digit${key}` : /^[a-z]$/i.test(key) ? `Key${key.toUpperCase()}` : key;
    const vk = key.length === 1 ? key.toUpperCase().charCodeAt(0) : 0;
    const base = { key, code, windowsVirtualKeyCode: vk, nativeVirtualKeyCode: vk };
    await send('Input.dispatchKeyEvent', { type: 'keyDown', ...base });
    await sleep(120);
    await send('Input.dispatchKeyEvent', { type: 'keyUp', ...base });
    console.log(`${stamp()}s  key ${key}`);
  };

  let lastFrames = 0, frames = 0;
  for (let s = 1; s <= seconds; s++) {
    await sleep(1000);
    const st = await evaluate(
      "(typeof Module !== 'undefined' && Module._web_state) ? [Module._web_state(), Module._web_frames(), " +
      "Module.m2hleAudioStats ? Module.m2hleAudioStats() : null] : null");
    if (st) {
      frames = st[1];
      /* The queue is sampled at an arbitrary phase of its sawtooth (up a chunk, down
       * a callback), so read the column as a level, not a number to the frame. */
      const a = st[2];
      const audio = !a ? '' : !a.rate ? `  audio ${a.mode}`
        : `  audio ${a.mode}/${a.state} queue=${a.queueMs.toFixed(0)}ms (target ${a.targetMs.toFixed(0)})` +
          `${a.buffering ? ' BUFFERING' : ''} ${a.mode === 'fallback' ? 'held-samples' : 'dropouts'}=${a.underruns} resyncs=${a.resyncs}`;
      console.log(`${stamp()}s  state=${st[0]} frames=${frames} (+${frames - lastFrames}/s)${audio}`);
      lastFrames = frames;
    }
    for (const k of keys) if (!k.done && s >= k.at) { k.done = true; await press(k.key); }
    if (shot && shotAt.includes(s)) await screenshot(shot.replace(/\.png$/, `-${s}s.png`));
  }
  if (shot) await screenshot(shot);

  if (expectLog && !sawExpected) {
    console.log(`FAIL: no console line contained "${expectLog}"`);
    failed = true;
  }
  if (expectFrames && frames < expectFrames) {
    console.log(`FAIL: ${frames} game frames, expected at least ${expectFrames}`);
    failed = true;
  }
  ws.close();
} catch (e) {
  console.error(String(e));
  failed = true;
} finally {
  child.kill();                      /* only the browser this script started */
}
process.exit(failed ? 1 : 0);
