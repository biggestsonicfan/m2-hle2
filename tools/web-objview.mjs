#!/usr/bin/env node
/*
 * web-objview.mjs — the object viewer, against the WEB build, from a shell.
 *
 *   node tools/web-objview.mjs --url http://localhost:8080/?rom=/dev-rom.zip \
 *        --out shots --model 3544 --six
 *
 *   node tools/web-objview.mjs --url ... --out shots --model 3545 --count 8 --pitch0 15
 *   node tools/web-objview.mjs --url ... --list 3540:24
 *   node tools/web-objview.mjs --url ... --out shots --opts '{"capture":7,"use_capture_matrix":true}'
 *
 * The desktop build has a TCP bridge an MCP server talks to; a browser has no
 * such thing, so this is the equivalent: Chrome or Edge over the DevTools
 * protocol, driving the page's own window.m2hleObjview and writing the PNGs it
 * hands back. Same command vocabulary, same reply fields, same PNGs — the only
 * difference is that they arrive base64 over a debugging socket instead of
 * being written by the emulator, because in a browser there is nowhere to
 * write.
 *
 * It waits for the game to have BUILT its 3D state, not merely for frames to go
 * by: the texture sheets and the face palette are filled by the game's own boot
 * code, and a model decoded before then has the right shape with no texels,
 * which reads as an artifact and is not one. In STF that lands as attract mode
 * starts, about 12 seconds in.
 *
 * Serve the site and a ROM first:
 *   node tools/web-serve.mjs --rom <merged.zip>
 *
 * Options
 *   --url URL          the page, usually with ?rom=/dev-rom.zip
 *   --out DIR          where the PNGs go (default: objview-shots)
 *   --name STEM        file stem (default: the model or capture index)
 *   --model N          a model-table index
 *   --capture N        one of the models the board drew this frame
 *   --six              the six camera stations: +Z +X -Z -X +Y -Y
 *   --count N          a turntable of N angles over a full turn
 *   --yaw D --pitch D  a single angle (or the sweep's start with --count)
 *   --width N --height N
 *   --wireframe        overlay the decoder's edges
 *   --no-texture       flat face colour only
 *   --cull 0|1|2       backface culling
 *   --list FIRST:COUNT list triangle counts instead of shooting
 *   --opts JSON        anything else, merged last (every objview_set field)
 *   --wait S           how long to wait for the game's 3D state (default 90)
 *   --at-frame N       and then until the game reaches frame N. Face colours come
 *                      from palette RAM, which the game fills per scene: a model
 *                      whose scene has not been reached draws black even once the
 *                      textures are in. STF's attract has the fighters by ~900.
 *   --show             leave the viewer on the canvas and screenshot the page
 *   --browser PATH     Chrome or Edge, if it is not where this looks
 *   --headful          run with a window, to watch it
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
const has = (name) => args.includes(name);

const url = opt('--url', 'http://localhost:8080/?rom=/dev-rom.zip');
const outDir = opt('--out', 'objview-shots');
const waitS = Number(opt('--wait', '90'));
const listSpec = opt('--list', null);
const atFrame = Number(opt('--at-frame', '0'));

/* Everything the viewer understands, built from the flags and then overlaid
 * with --opts so an unusual field needs no flag of its own. */
const shotOpts = {};
if (opt('--model', null) !== null) shotOpts.model = Number(opt('--model'));
if (opt('--capture', null) !== null) shotOpts.capture = Number(opt('--capture'));
if (has('--six')) shotOpts.six = true;
if (opt('--count', null) !== null) shotOpts.count = Number(opt('--count'));
if (opt('--yaw', null) !== null) shotOpts.yaw0 = Number(opt('--yaw'));
if (opt('--pitch', null) !== null) shotOpts.pitch0 = Number(opt('--pitch'));
if (opt('--width', null) !== null) shotOpts.width = Number(opt('--width'));
if (opt('--height', null) !== null) shotOpts.height = Number(opt('--height'));
if (has('--wireframe')) shotOpts.wireframe = true;
if (has('--no-texture')) shotOpts.textured = false;
if (opt('--cull', null) !== null) shotOpts.cull = Number(opt('--cull'));
if (opt('--opts', null) !== null) Object.assign(shotOpts, JSON.parse(opt('--opts')));

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
/* One fixed profile directory, reused and never deleted here: a fresh one per
 * run would need a recursive delete, and these scripts do not delete trees. */
const profile = path.join(os.tmpdir(), 'm2hle-web-objview-profile');
fs.mkdirSync(profile, { recursive: true });
const child = spawn(browser, [
  ...(has('--headful') ? [] : ['--headless=new']),
  '--no-first-run', '--disable-extensions', '--mute-audio',
  `--user-data-dir=${profile}`, `--remote-debugging-port=${port}`,
  '--window-size=1000,800',
  '--autoplay-policy=no-user-gesture-required',
  /* A machine with no usable GPU (CI, a remote session) still gets WebGL2. */
  '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist',
  ...(process.env.CI ? ['--no-sandbox'] : []),
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
      if (/\[ERR \]|\[sg\]\[(error|panic)\]/.test(text)) {
        failed = true;
        console.log(`${stamp()}s  ${text}`);
      }
    } else if (msg.method === 'Runtime.exceptionThrown') {
      failed = true;
      const d = msg.params.exceptionDetails;
      console.log(`${stamp()}s  EXCEPTION: ${d.exception?.description || d.text}`);
    }
  };

  const evaluate = async (expression, awaitPromise = true) => {
    const r = await send('Runtime.evaluate', { expression, returnByValue: true, awaitPromise });
    if (r?.exceptionDetails) {
      failed = true;
      throw new Error(r.exceptionDetails.exception?.description || r.exceptionDetails.text);
    }
    return r?.result?.value;
  };

  await send('Runtime.enable');
  await send('Page.enable');
  await send('Page.navigate', { url });

  /* The module only exists once m2hle.js has run. */
  console.log(`${stamp()}s  waiting for the page`);
  for (let i = 0; ; i++) {
    if (await evaluate('typeof m2hleObjview !== "undefined"', false)) break;
    if (i > 300) throw new Error('the page never defined m2hleObjview (is this the web build?)');
    await sleep(100);
  }

  console.log(`${stamp()}s  waiting for the game's 3D state (up to ${waitS}s)`);
  const ready = await evaluate(`m2hleObjview.waitReady(${waitS * 1000})`);
  console.log(`${stamp()}s  ready=${ready.ready} frames=${ready.frames} ` +
              `tex=${ready.tex_pct}% pal=${ready.pal_pct}% models=${ready.models} captures=${ready.captures}`);
  if (!ready.ready) {
    console.error('the game never finished building its 3D state; is a ROM loaded?');
    failed = true;
  }

  /* Face colours come out of palette RAM, which the game fills per scene: a
   * model whose scene has not been reached draws black even once its texels
   * are in. Waiting for a frame number is how you get past that. */
  if (ready.ready && atFrame > 0) {
    console.log(`${stamp()}s  waiting for game frame ${atFrame}`);
    for (let i = 0; ; i++) {
      const f = await evaluate('Module._web_frames()', false);
      if (f >= atFrame) { console.log(`${stamp()}s  at frame ${f}`); break; }
      if (i > waitS * 10) { console.log(`${stamp()}s  still at frame ${f}; going ahead`); break; }
      await sleep(100);
    }
  }

  if (!ready.ready) {
    /* reported above; nothing to shoot */
  } else if (listSpec) {
    const [first, count] = listSpec.split(':').map(Number);
    const r = await evaluate(`m2hleObjview.list(${first || 0}, ${count || 64})`);
    if (!r.ok) { console.error(`list failed: ${r.error}`); failed = true; }
    else {
      console.log(`${stamp()}s  ${r.listed} non-empty of ${r.count} from ${r.first} ` +
                  `(table holds ${r.table_count})`);
      for (const m of r.models) console.log(`        model ${m.model}: ${m.tris} tris`);
    }
  } else {
    const r = await evaluate(
      `(async () => {
         const res = await m2hleObjview.shot(${JSON.stringify(shotOpts)});
         if (res.shots) {
           for (const s of res.shots) {
             /* Bytes do not survive returnByValue; base64 does. */
             s.b64 = s.bytes ? btoa(String.fromCharCode(...s.bytes)) : null;
             delete s.bytes; delete s.blob;
           }
         }
         return res;
       })()`);
    if (!r.ok) {
      console.error(`shot failed: ${r.error || r.last_error}`);
      failed = true;
    } else {
      fs.mkdirSync(outDir, { recursive: true });
      const stem = opt('--name', null)
                   ?? (shotOpts.capture !== undefined ? `capture-${shotOpts.capture}` : `model-${r.drawn_model}`);
      console.log(`${stamp()}s  model ${r.drawn_model}: ${r.tris} tris, ${r.lines} lines, ` +
                  `${r.count} shot(s) at ${r.width}x${r.height}`);
      r.shots.forEach((s, i) => {
        const file = path.join(outDir, r.count > 1 ? `${stem}-${String(i).padStart(3, '0')}.png` : `${stem}.png`);
        if (s.b64) fs.writeFileSync(file, Buffer.from(s.b64, 'base64'));
        console.log(`        yaw ${String(s.yaw).padStart(6)}  pitch ${String(s.pitch).padStart(5)}  ` +
                    `coverage ${s.coverage.toFixed(4)}  box ${JSON.stringify(s.box)}  -> ${file}`);
      });
      if (r.shots.every((s) => s.coverage < 0.0005)) {
        console.log('        every shot is background: the model decoded to nothing, or it is off screen');
      }
    }
  }

  if (has('--show')) {
    await evaluate('m2hleObjview.show(true)', false);
    await sleep(500);
    fs.mkdirSync(outDir, { recursive: true });
    const r = await send('Page.captureScreenshot', { format: 'png' });
    const file = path.join(outDir, 'page.png');
    if (r?.data) { fs.writeFileSync(file, Buffer.from(r.data, 'base64')); console.log(`${stamp()}s  page -> ${file}`); }
  }

  ws.close();
} catch (e) {
  console.error(`web-objview: ${e.message}`);
  failed = true;
} finally {
  child.kill();
}
process.exit(failed ? 1 : 0);
