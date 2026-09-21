#!/usr/bin/env node
/*
 * web-netplay.mjs — two players on the web build, end to end, through the page.
 *
 *   node tools/web-netplay.mjs [--url http://localhost:8080/] [--gw ws://127.0.0.1:8787/gw]
 *        [--seconds 40] [--shot out.png] [--hide-a 10] [--browser path]
 *        [--a name:password] [--b name:password] [--ui-shots DIR] [--keep]
 *
 * Starts two headless browsers, each with a profile of its own (so each has its
 * own localStorage, i.e. its own stored sign-in), loads the game in both, and
 * then does what two people would do, by clicking the page's own controls:
 * create an account each (or sign in with --a / --b), A creates a match, B
 * finds it and presses Play, A presses Start, B accepts. Then both press keys
 * for --seconds while the script watches the netplay state of each board.
 *
 * It passes when both boards reach "playing", both keep advancing, and neither
 * latches a desync (the per-frame check values the two boards exchange). The
 * two players must be on the same build: this is the web-vs-web gate of
 * WEB-NETPLAY.md, N2.
 *
 * --ui-shots DIR saves a screenshot of the online panel at each step (sign-in,
 * lobby, a room waiting, a room with the opponent in it) for looking at.
 *
 * --hide-a N opens a second tab in browser A for N seconds, which hides the
 * game's tab: the board is then driven by the page's background worker, and the
 * run shows whether B had to wait (WEB-NETPLAY.md 5.3).
 *
 * Needs a gateway and an RPCN server to talk to: see web/gateway/README.md,
 * "Testing locally". The web server must serve --rom (tools/web-serve.mjs).
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
const base = opt('--url', 'http://localhost:8080/');
const gw = opt('--gw', 'ws://127.0.0.1:8787/gw');
const seconds = Number(opt('--seconds', '40'));
const shot = opt('--shot', null);
const hideA = Number(opt('--hide-a', '0'));
const uiShots = opt('--ui-shots', null);
const suffix = Math.random().toString(36).slice(2, 7);
const cred = (flag, who) => {
  const v = opt(flag, null);
  if (v) { const [npid, password] = v.split(':'); return { npid, password, create: false }; }
  return { npid: `web${who}${suffix}`, password: `pw-${suffix}-${who}`, email: `web${who}${suffix}@example.com`, create: true };
};
const players = { A: cred('--a', 'a'), B: cred('--b', 'b') };

const CANDIDATES = [
  opt('--browser', null), process.env.CHROME_PATH,
  'C:/Program Files/Google/Chrome/Application/chrome.exe',
  'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',
  'C:/Program Files/Microsoft/Edge/Application/msedge.exe',
  '/usr/bin/google-chrome', '/usr/bin/chromium', '/usr/bin/chromium-browser',
].filter(Boolean);
const browserPath = CANDIDATES.find((p) => fs.existsSync(p));
if (!browserPath) { console.error('no Chrome or Edge found; pass --browser'); process.exit(2); }

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const t0 = Date.now();
const stamp = () => ((Date.now() - t0) / 1000).toFixed(1).padStart(6);
const log = (who, ...a) => console.log(`${stamp()}s  ${who}`, ...a);

/* One fixed profile per player, reused and never deleted here (this script
 * deletes no directories). The stored sign-in is cleared at the start instead. */
async function launch(who) {
  const port = 9300 + Math.floor(Math.random() * 600);
  const profile = path.join(os.tmpdir(), `m2hle-web-netplay-${who}`);
  fs.mkdirSync(profile, { recursive: true });
  const child = spawn(browserPath, [
    '--headless=new', '--no-first-run', '--disable-extensions', '--mute-audio',
    `--user-data-dir=${profile}`, `--remote-debugging-port=${port}`, '--window-size=1200,800',
    '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist', '--autoplay-policy=no-user-gesture-required',
    ...(process.env.CI ? ['--no-sandbox'] : []),
    'about:blank',
  ], { stdio: ['ignore', 'ignore', 'pipe'] });
  let err = '', exited = null;
  child.stderr.on('data', (d) => { err = (err + d).slice(-4000); });
  child.on('exit', (code, signal) => { exited = signal || code; });

  let target = null;
  for (let i = 0; i < 300 && !target && exited === null; i++) {
    try {
      const list = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
      target = list.find((t) => t.type === 'page');
    } catch { /* not up yet */ }
    if (!target) await sleep(100);
  }
  if (!target) {
    throw new Error(`${who}: ${exited !== null ? `the browser exited (${exited})` : 'the browser never opened its debugging port (30 s)'}` +
                    (err.trim() ? `\n${err.trim().split('\n').slice(-15).join('\n')}` : ''));
  }

  const ws = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((res, rej) => { ws.onopen = res; ws.onerror = rej; });
  let nextId = 1;
  const pending = new Map();
  const b = { who, child, port, ws, errors: 0 };
  b.send = (method, params = {}) => new Promise((resolve) => {
    const id = nextId++;
    pending.set(id, resolve);
    ws.send(JSON.stringify({ id, method, params }));
  });
  ws.onmessage = (ev) => {
    const msg = JSON.parse(ev.data);
    if (msg.id && pending.has(msg.id)) { pending.get(msg.id)(msg.result || msg.error); pending.delete(msg.id); return; }
    if (msg.method === 'Runtime.consoleAPICalled') {
      const text = msg.params.args.map((a) => a.value ?? a.description ?? '').join(' ');
      if (/netplay|gateway|\[ERR|\[WARN\] net/i.test(text)) log(who, text);
    } else if (msg.method === 'Runtime.exceptionThrown') {
      b.errors++;
      const d = msg.params.exceptionDetails;
      log(who, 'EXCEPTION:', d.exception?.description || d.text);
    }
  };
  b.eval = async (expression) => {
    const r = await b.send('Runtime.evaluate', { expression, returnByValue: true, awaitPromise: true });
    if (r?.exceptionDetails) throw new Error(`${who}: ${r.exceptionDetails.exception?.description || r.exceptionDetails.text}`);
    return r?.result?.value;
  };
  b.status = () => b.eval('m2hleNetplay.status()');
  b.key = async (key, down) => {
    const code = /^[0-9]$/.test(key) ? `Digit${key}` : /^[a-z]$/i.test(key) ? `Key${key.toUpperCase()}` : key;
    const vk = key.length === 1 ? key.toUpperCase().charCodeAt(0) : ({ ArrowLeft: 37, ArrowUp: 38, ArrowRight: 39, ArrowDown: 40 })[key] || 0;
    await b.send('Input.dispatchKeyEvent', { type: down ? 'keyDown' : 'keyUp', key, code, windowsVirtualKeyCode: vk, nativeVirtualKeyCode: vk });
  };
  await b.send('Runtime.enable');
  await b.send('Page.enable');
  return b;
}

async function until(b, what, test, ms = 30000) {
  const end = Date.now() + ms;
  let last;
  while (Date.now() < end) {
    last = await b.status().catch(() => null);
    if (last && test(last)) return last;
    await sleep(250);
  }
  throw new Error(`${b.who}: timed out waiting for ${what} (state ${last && last.state}, error "${last && last.error}")`);
}

async function uiShot(b, name) {
  if (!uiShots) return;
  await sleep(400);   /* the panel redraws four times a second */
  fs.mkdirSync(uiShots, { recursive: true });
  const r = await b.send('Page.captureScreenshot', { format: 'png' });
  const file = path.join(uiShots, `${name}.png`);
  if (r?.data) { fs.writeFileSync(file, Buffer.from(r.data, 'base64')); log(b.who, 'ui ->', file); }
}

const click = (b, id) => b.eval(`document.getElementById(${JSON.stringify(id)}).click()`);
const fill = (b, id, value) => b.eval(`document.getElementById(${JSON.stringify(id)}).value = ${JSON.stringify(value)}`);

let failed = false;
const browsers = [];
try {
  for (const who of ['A', 'B']) browsers.push(await launch(who));
  const [A, B] = browsers;

  const page = new URL(base);
  page.searchParams.set('rom', '/dev-rom.zip');
  page.searchParams.set('gw', gw);
  for (const b of browsers) {
    /* A clean slate: forget whatever a previous run stored in this profile. */
    await b.send('Page.navigate', { url: page.href });
    await sleep(500);
    await b.eval("localStorage.removeItem('m2hle_netplay.cfg'); true");
    await b.send('Page.navigate', { url: page.href });
  }
  for (const b of browsers) {
    const end = Date.now() + 60000;
    while (Date.now() < end && !(await b.eval("typeof Module !== 'undefined' && Module._web_state ? Module._web_state() : 0").catch(() => 0))) await sleep(250);
    log(b.who, 'game running; frames', await b.eval('Module._web_frames()'));
  }

  /* ---- Sign in, through the panel ---- */
  for (const b of browsers) {
    const p = players[b.who];
    await click(b, 'btn-online');
    if (b.who === 'A') await uiShot(b, '1-signin');
    if (p.create) {
      await click(b, 'np-tab-create');
      await fill(b, 'np-create-name', p.npid);
      await fill(b, 'np-create-password', p.password);
      await fill(b, 'np-create-email', p.email);
      await b.eval("document.getElementById('np-form-create').requestSubmit()");
      log(b.who, `creating account ${p.npid}`);
    } else {
      await fill(b, 'np-signin-name', p.npid);
      await fill(b, 'np-signin-password', p.password);
      await b.eval("document.getElementById('np-form-signin').requestSubmit()");
      log(b.who, `signing in as ${p.npid}`);
    }
  }
  for (const b of browsers) {
    const s = await until(b, 'online', (s) => s.state === 'online' || s.state === 'failed' || (s.account && s.account.state === 3));
    if (s.state !== 'online') throw new Error(`${b.who}: sign-in failed: ${s.error || s.account.error}`);
    log(b.who, `online as ${s.npid}`);
  }

  /* ---- A hosts, B finds it and joins ---- */
  await uiShot(A, '2-lobby-empty');
  await click(A, 'np-create-match');
  const hosted = await until(A, 'a room', (s) => s.state === 'in a room' && s.room.id !== '0');
  log('A', `hosting room ${hosted.room.id} (flags 0x${hosted.room.flags.toString(16)})`);

  const seen = await until(B, 'A\'s room in the list', (s) => (s.rooms || []).some((r) => r.id === hosted.room.id), 30000);
  const row = seen.rooms.find((r) => r.id === hosted.room.id);
  log('B', `sees ${row.owner}'s room: ${row.why || 'joinable'} (web=${row.web}, delay ${row.delay})`);
  if (row.why) throw new Error(`B refuses A's room: ${row.why}`);
  await uiShot(A, '3-room-waiting');
  await uiShot(B, '4-lobby-with-room');
  await B.eval(`[...document.querySelectorAll('#np-rooms .np-room')].find((li) => li.querySelector('.np-who').textContent === ${JSON.stringify(row.owner)}).querySelector('button').click()`);

  await until(A, 'B to be heard', (s) => s.room.peer_heard, 30000);
  await until(B, 'A to be heard', (s) => s.room.peer_heard, 30000);
  log('A', 'peer heard:', (await A.status()).room.peer);
  log('B', 'peer heard:', (await B.status()).room.peer);
  await uiShot(A, '5-room-joined');

  /* ---- Start: A presses Start, B accepts ---- */
  await click(A, 'np-start');
  await until(B, 'A\'s challenge', (s) => s.room.peer_ready || s.state !== 'in a room', 10000);
  await uiShot(B, '6-room-challenged');
  await click(B, 'np-start');
  for (const b of browsers) await until(b, 'playing', (s) => s.state === 'playing', 30000);
  log('both', 'playing');

  /* ---- Play: both mash keys; watch both boards ---- */
  const KEYS = ['ArrowLeft', 'ArrowRight', 'ArrowUp', 'ArrowDown', 'z', 'x', 'c', '5', '1'];
  let hidden = null;
  let last = { A: null, B: null };
  for (let s = 1; s <= seconds; s++) {
    for (const b of browsers) {
      const k = KEYS[Math.floor(Math.random() * KEYS.length)];
      await b.send('Runtime.evaluate', { expression: "document.getElementById('canvas').focus()" });
      await b.key(k, true);
      setTimeout(() => b.key(k, false), 150);
    }
    if (s === 3) {
      /* The panel folds away when the match starts; opening it again mid-match
       * (to end the match or leave) must keep it open. It used to shut itself
       * a quarter of a second later, on every status poll. */
      await click(B, 'btn-online');
      await sleep(1000);
      const panel = await B.eval("({ open: !document.getElementById('online').hidden, " +
        "stop: !document.getElementById('np-stop').hidden, leave: !document.getElementById('np-leave').hidden })");
      log('B', `panel opened mid-match: open=${panel.open} end-match=${panel.stop} leave=${panel.leave}`);
      if (!panel.open || !panel.stop || !panel.leave) { failed = true; log('B', 'FAIL: the panel did not stay open with its controls'); }
      await uiShot(B, '7-room-playing');
      await click(B, 'np-close');
    }
    if (hideA && s === 5) {
      hidden = await A.send('Target.createTarget', { url: 'about:blank', background: false });
      log('A', `game tab hidden for ${hideA} s:`, await A.eval('document.hidden').catch(() => '?'));
    }
    if (hidden && s === 5 + hideA) {
      await A.send('Target.closeTarget', { targetId: hidden.targetId });
      hidden = null;
      log('A', 'game tab visible again');
    }
    await sleep(1000);
    const line = [];
    for (const b of browsers) {
      const st = await b.status();
      const prev = last[b.who];
      line.push(`${b.who}: ${st.state} frame=${st.frame} (+${prev ? st.frame - prev.frame : 0}) stalls=${st.stalls}` +
                `${st.desync !== null ? ' DESYNC@' + st.desync : ''}`);
      if (st.desync !== null) failed = true;
      if (st.state !== 'playing') failed = true;
      last[b.who] = st;
    }
    console.log(`${stamp()}s  ${line.join('   ')}`);
  }
  const rtts = [];
  for (const b of browsers) rtts.push(`${b.who} rtt ${Math.round((await b.eval('m2hleNetplay.rtt()')) || 0)} ms`);
  console.log(`${stamp()}s  ${rtts.join(', ')}`);

  if (shot) {
    for (const b of browsers) {
      const r = await b.send('Page.captureScreenshot', { format: 'png' });
      const file = shot.replace(/\.png$/, `-${b.who}.png`);
      if (r?.data) { fs.writeFileSync(file, Buffer.from(r.data, 'base64')); log(b.who, 'screenshot ->', file); }
    }
  }
  const need = Math.min(600, seconds * 30);
  const moved = browsers.every((b) => last[b.who] && last[b.who].frame > need);
  if (!moved) { failed = true; console.log(`FAIL: a board did not get past frame ${need}`); }
  if (browsers.some((b) => b.errors)) failed = true;
} catch (e) {
  failed = true;
  console.log(`${stamp()}s  FAIL: ${e.message}`);
} finally {
  if (!args.includes('--keep')) for (const b of browsers) b.child.kill();
}
console.log(failed ? 'RESULT: FAIL' : 'RESULT: PASS');
process.exit(failed ? 1 : 0);
