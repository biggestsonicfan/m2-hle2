#!/usr/bin/env node
/*
 * Probes a deployed gateway from outside, without signing in to anything:
 *
 *   node test/probe-live.mjs [wss://rpcn.sonicthefighte.rs/gw]
 *
 *   - /gw/health answers
 *   - an upgrade from a foreign Origin is refused
 *   - /gw/stream reaches RPCN: RPCN speaks first on a new connection? No -- it
 *     waits for a request, so this sends a malformed one and expects RPCN to
 *     answer or hang up (either proves the TLS upstream is up), not the gateway
 *     to report it unreachable
 *   - /gw/dgram: the round-trip echo, and a signaling keepalive for user 0,
 *     which RPCN's helper answers with the address it saw -- the droplet's
 *     public address and this session's gateway port
 */
import { WebSocket } from 'ws';

const base = (process.argv[2] || 'wss://rpcn.sonicthefighte.rs/gw').replace(/\/$/, '');
const ORIGIN = 'https://play.sonicthefighte.rs';
const TAG = [100, 127, 255, 254];
let failed = false;
const ok = (cond, what) => { console.log(`${cond ? 'ok  ' : 'FAIL'}  ${what}`); if (!cond) failed = true; };

function open(path, origin = ORIGIN) {
  return new Promise((resolve) => {
    const ws = new WebSocket(`${base}/${path}`, { origin });
    ws.binaryType = 'nodebuffer';
    const t = setTimeout(() => resolve({ ws, err: 'timeout' }), 8000);
    ws.once('open', () => { clearTimeout(t); resolve({ ws }); });
    ws.once('unexpected-response', (req, res) => { clearTimeout(t); resolve({ ws, status: res.statusCode }); });
    ws.once('error', (e) => { clearTimeout(t); resolve({ ws, err: e.message }); });
  });
}

const next = (ws, ms = 5000) => new Promise((resolve) => {
  const t = setTimeout(() => resolve(null), ms);
  ws.once('message', (d) => { clearTimeout(t); resolve(d); });
  ws.once('close', (code, reason) => { clearTimeout(t); resolve({ closed: code, reason: String(reason) }); });
});

const httpBase = base.replace(/^ws/, 'http');
const h = await fetch(`${httpBase}/health`).then((r) => r.json()).catch((e) => ({ err: e.message }));
ok(h.ok === true, `health: ${JSON.stringify(h)}`);

const bad = await open('stream', 'https://example.com');
ok(bad.status === 403, `foreign origin refused (${bad.status || bad.err})`);

const s = await open('stream');
ok(!s.err && !s.status, `stream opened (${s.err || s.status || 'ok'})`);
if (!s.err && !s.status) {
  /* A header claiming a 13-byte packet of an unknown command. */
  s.ws.send(Buffer.from([0, 0xFF, 0xFF, 13, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0]));
  const r = await next(s.ws, 6000);
  const unreachable = r && r.reason && /could not be reached|unexpected certificate/.test(r.reason);
  ok(!unreachable, `stream reached RPCN (${r === null ? 'no answer yet, connection held' : Buffer.isBuffer(r) ? r.length + ' bytes back' : 'closed: ' + r.reason})`);
  s.ws.close();
}

const d = await open('dgram');
ok(!d.err && !d.status, `dgram opened (${d.err || d.status || 'ok'})`);
if (!d.err && !d.status) {
  const ping = Buffer.alloc(14);
  ping.writeDoubleBE(performance.now(), 6);
  d.ws.send(ping);
  const echo = await next(d.ws);
  ok(Buffer.isBuffer(echo) && echo.length === 14, `echo (${Buffer.isBuffer(echo) ? (performance.now() - echo.readDoubleBE(6)).toFixed(0) + ' ms round trip' : 'none'})`);

  const frame = Buffer.alloc(6 + 13);
  Buffer.from(TAG).copy(frame, 0);
  frame.writeUInt16BE(3657, 4);
  frame[6] = 1;              /* keepalive, user 0, local_addr rewritten by the gateway */
  d.ws.send(frame);
  const r = await next(d.ws);
  if (Buffer.isBuffer(r) && r.length >= 6 + 9) {
    const from = [...r.subarray(0, 4)].join('.') + ':' + r.readUInt16BE(4);
    const seen = [...r.subarray(9, 13)].join('.') + ':' + r.readUInt16BE(13);
    ok(from === '100.127.255.254:3657', `signaling reply labelled ${from}`);
    ok(!seen.startsWith('127.') && !seen.startsWith('172.'), `RPCN saw us at ${seen}`);
  } else {
    ok(false, `signaling reply (${JSON.stringify(r)})`);
  }
  d.ws.close();
}
console.log(failed ? 'RESULT: FAIL' : 'RESULT: PASS');
process.exit(failed ? 1 : 0);
