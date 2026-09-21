/*
 * The gateway end to end, against stand-ins: a plain-TCP "RPCN" that echoes, a
 * UDP "signaling helper" that records what it gets and answers, and a UDP
 * "desktop peer". Everything on loopback, so the private-destination rule is
 * relaxed for the peer (allowPrivateDestinations), exactly as a local test
 * gateway would be configured.
 */
import assert from 'node:assert/strict';
import dgram from 'node:dgram';
import net from 'node:net';
import { after, before, test } from 'node:test';
import { WebSocket } from 'ws';

import { startGateway } from '../gateway.mjs';
import { SIGNALING_TAG, ipv4, ipv4Text } from '../rules.mjs';

const ORIGIN = 'https://play.sonicthefighte.rs';
let gw, rpcn, signaling, sigPort, lastSignaling;

const udp = () => new Promise((resolve) => {
  const s = dgram.createSocket('udp4');
  s.bind(0, '127.0.0.1', () => resolve(s));
});

before(async () => {
  rpcn = net.createServer((c) => c.on('data', (b) => c.write(Buffer.concat([Buffer.from('echo:'), b]))));
  await new Promise((r) => rpcn.listen(0, '127.0.0.1', r));
  signaling = await udp();
  sigPort = signaling.address().port;
  signaling.on('message', (msg, rinfo) => {
    lastSignaling = { msg, rinfo };
    signaling.send(Buffer.from('sig-reply'), rinfo.port, rinfo.address);
  });
  gw = await startGateway({
    listen: { host: '127.0.0.1', port: 0 },
    origins: [ORIGIN],
    rpcn: { host: '127.0.0.1', port: rpcn.address().port, tls: false },
    signaling: { host: '127.0.0.1', port: sigPort },
    udp: { bind: '127.0.0.1', portMin: 41000, portMax: 41010 },
    allowPrivateDestinations: true,
    limits: { dgramsPerIp: 3 },
  }, () => {});
});

after(async () => {
  await gw.close();
  rpcn.close();
  signaling.close();
});

function connect(path, origin = ORIGIN) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(`ws://127.0.0.1:${gw.port}${path}`, { origin });
    ws.binaryType = 'nodebuffer';
    ws.inbox = [];
    ws.waiters = [];
    ws.on('message', (d) => {
      const w = ws.waiters.shift();
      if (w) w(d); else ws.inbox.push(d);
    });
    ws.once('open', () => resolve(ws));
    ws.once('unexpected-response', (req, res) => reject(new Error(`HTTP ${res.statusCode}`)));
    ws.once('error', reject);
  });
}

const next = (ws, ms = 2000) => new Promise((resolve, reject) => {
  if (ws.inbox.length) { resolve(ws.inbox.shift()); return; }
  const t = setTimeout(() => reject(new Error('timed out waiting for a message')), ms);
  ws.waiters.push((d) => { clearTimeout(t); resolve(d); });
});

const frame = (ip, port, payload) => {
  const b = Buffer.alloc(6 + payload.length);
  b.writeUInt32BE(ip >>> 0, 0);
  b.writeUInt16BE(port, 4);
  Buffer.from(payload).copy(b, 6);
  return b;
};
const unframe = (b) => ({ ip: b.readUInt32BE(0) >>> 0, port: b.readUInt16BE(4), payload: b.subarray(6) });

test('stream: bytes sent before the upstream connects are delivered, in order', async () => {
  const ws = await connect('/gw/stream');
  ws.send(Buffer.from('hello'));
  const got = await next(ws);
  assert.equal(got.toString(), 'echo:hello');
  ws.close();
});

test('an origin that is not the site is refused', async () => {
  await assert.rejects(connect('/gw/stream', 'https://evil.example'), /403/);
  await assert.rejects(connect('/gw/dgram', 'https://evil.example'), /403/);
});

test('dgram: ping comes straight back', async () => {
  const ws = await connect('/gw/dgram');
  ws.send(frame(0, 0, 'ping1234'));
  const r = unframe(await next(ws));
  assert.deepEqual([r.ip, r.port, r.payload.toString()], [0, 0, 'ping1234']);
  ws.close();
});

test('dgram: signaling gets the virtual address and the reply comes back tagged', async () => {
  const ws = await connect('/gw/dgram');
  const pkt = Buffer.from([1, 9, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]);
  ws.send(frame(SIGNALING_TAG, 3657, pkt));
  const r = unframe(await next(ws));
  assert.equal(r.ip, SIGNALING_TAG);
  assert.equal(r.port, 3657);
  assert.equal(r.payload.toString(), 'sig-reply');
  const vip = lastSignaling.msg.readUInt32BE(9);
  assert.equal(ipv4Text(vip).startsWith('100.64.'), true, `virtual address ${ipv4Text(vip)}`);
  assert.equal(lastSignaling.rinfo.address, '127.0.0.1');
  assert.ok(lastSignaling.rinfo.port >= 41000 && lastSignaling.rinfo.port <= 41010);
  ws.close();
});

async function virtualAddressOf(ws) {
  ws.send(frame(SIGNALING_TAG, 3657, Buffer.from([1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0])));
  await next(ws);
  return lastSignaling.msg.readUInt32BE(9) >>> 0;
}

test('dgram: two browser players reach each other through the pool, as <vip>:3658', async () => {
  const a = await connect('/gw/dgram');
  const b = await connect('/gw/dgram');
  const va = await virtualAddressOf(a);
  const vb = await virtualAddressOf(b);
  assert.notEqual(va, vb);

  a.send(frame(vb, 3658, 'a->b'));
  let r = unframe(await next(b));
  assert.deepEqual([r.ip, r.port, r.payload.toString()], [va, 3658, 'a->b']);

  b.send(frame(va, 3658, 'b->a'));
  r = unframe(await next(a));
  assert.deepEqual([r.ip, r.port, r.payload.toString()], [vb, 3658, 'b->a']);

  /* To itself: dropped, not looped back. */
  a.send(frame(va, 3658, 'self'));
  a.send(frame(0, 0, 'after'));
  r = unframe(await next(a));
  assert.equal(r.payload.toString(), 'after');
  a.close();
  b.close();
});

test('dgram: a desktop peer is reached over real UDP and answers back', async () => {
  const peer = await udp();
  const ws = await connect('/gw/dgram');
  const seen = new Promise((resolve, reject) => {
    const t = setTimeout(() => reject(new Error('the peer never heard from the gateway')), 2000);
    peer.once('message', (msg, rinfo) => { clearTimeout(t); resolve({ msg, rinfo }); });
  });
  ws.send(frame(ipv4('127.0.0.1'), peer.address().port, 'to-native'));
  const { msg, rinfo } = await seen;
  assert.equal(msg.toString(), 'to-native');
  peer.send(Buffer.from('from-native'), rinfo.port, rinfo.address);
  const r = unframe(await next(ws));
  assert.deepEqual([ipv4Text(r.ip), r.port, r.payload.toString()], ['127.0.0.1', peer.address().port, 'from-native']);
  ws.close();
  peer.close();
});

test('dgram: per-address limit', async () => {
  const socks = [await connect('/gw/dgram'), await connect('/gw/dgram'), await connect('/gw/dgram')];
  await assert.rejects(connect('/gw/dgram'), /429/);
  for (const s of socks) s.close();
});
