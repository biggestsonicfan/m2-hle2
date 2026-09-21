#!/usr/bin/env node
/*
 * gateway.mjs -- lets the browser build reach RPCN (WEB-NETPLAY.md, section 4).
 *
 * A browser tab can open neither TCP nor UDP. RPCN is TLS over TCP plus a UDP
 * signaling helper, and a match is UDP between the two players. So each browser
 * player opens two WebSockets here:
 *
 *   /gw/stream  one TLS connection to RPCN, bytes relayed both ways. The
 *               upstream is fixed by this file's config; the client cannot name
 *               a host. An open WebSocket-to-TCP proxy is abused within days.
 *   /gw/dgram   one UDP socket on the public address, and one virtual address
 *               from a private pool. Each WebSocket message is one datagram,
 *               framed [ip: 4][port: u16 BE][payload]; outbound the address is
 *               the destination, inbound the source.
 *
 * The virtual address exists because RPCN, seeing two players on one public
 * address (every browser player is on ours), gives each the other's LOCAL
 * address with port 3658. The gateway writes the virtual address into each
 * signaling keepalive, so that local address is one it can route: browser to
 * browser never leaves this process. Browser to desktop goes out of the
 * session's real UDP socket, and the desktop client needs no change.
 *
 * NOTHING HERE LOGS A PAYLOAD BYTE. The stream is RPCN's protocol in the clear,
 * login tokens included; the operator of this gateway is the operator of RPCN,
 * which is why it runs on that host and nowhere else.
 *
 *   node gateway.mjs --config /etc/m2hle-gateway.json
 */
import dgram from 'node:dgram';
import dns from 'node:dns';
import fs from 'node:fs';
import http from 'node:http';
import net from 'node:net';
import tls from 'node:tls';
import { pathToFileURL } from 'node:url';
import { WebSocketServer, WebSocket } from 'ws';

import {
  AddressPool, Bucket, P2P_PORT, SIGNALING_PORT, SIGNALING_TAG,
  ipv4, ipv4Text, normalizeFingerprint, parseCidr, rewriteSignaling, route,
} from './rules.mjs';

export const DEFAULTS = {
  listen: { host: '127.0.0.1', port: 8787 },
  /* Take the client address from X-Forwarded-For when the connection comes from
   * loopback, i.e. from the Caddy in front. */
  trustProxy: true,
  origins: ['https://play.sonicthefighte.rs', 'https://biggestsonicfan.github.io'],
  rpcn: { host: '127.0.0.1', port: 31313, tls: true, fingerprint: '' },
  signaling: { host: '127.0.0.1', port: SIGNALING_PORT },
  udp: { bind: '0.0.0.0', portMin: 40000, portMax: 40999 },
  pool: '100.64.0.0/16',
  /* Development only: let datagrams reach RFC 1918 / loopback addresses, for a
   * desktop client on the same machine or LAN as a test gateway. */
  allowPrivateDestinations: false,
  limits: {
    streamsPerIp: 6,          /* a session, a sign-up and a Twitch flow can overlap */
    dgramsPerIp: 4,
    maxSessions: 2000,
    datagramsPerSec: 240,     /* ~4x what lockstep sends at 60 Hz with resends */
    bytesPerSec: 65536,
    maxPayload: 1200,
    streamIdleSec: 900,
    preConnectBytes: 65536,   /* queued while the upstream connects */
  },
};

function merge(base, over) {
  const out = { ...base };
  for (const [k, v] of Object.entries(over || {})) {
    out[k] = v && typeof v === 'object' && !Array.isArray(v) ? merge(base[k] || {}, v) : v;
  }
  return out;
}

const stamp = () => new Date().toISOString();
const defaultLog = (...a) => console.log(stamp(), ...a);

/*
 * Starts a gateway. Resolves to { port, close(), stats() } once it is listening.
 * `log` is replaceable so tests stay quiet.
 */
export async function startGateway(userConfig = {}, log = defaultLog) {
  const cfg = merge(DEFAULTS, userConfig);
  const pool = new AddressPool(parseCidr(cfg.pool));
  const udpBind = cfg.udp.bind;
  const selfIp = udpBind && udpBind !== '0.0.0.0' ? ipv4(udpBind) : undefined;
  const pin = normalizeFingerprint(cfg.rpcn.fingerprint);
  if (cfg.rpcn.tls && !pin) log('WARNING: rpcn.fingerprint is not set; the RPCN certificate is not checked');

  const streams = new Set();
  const dgrams = new Map();          /* vip -> session */
  const perIp = { stream: new Map(), dgram: new Map() };
  let nextId = 1;
  let portCursor = cfg.udp.portMin;
  const totals = { streams: 0, dgrams: 0, refused: 0 };

  /* The signaling helper, resolved once. A name is allowed in the config. */
  const signalingAddr = await new Promise((resolve, reject) => {
    if (net.isIPv4(cfg.signaling.host)) { resolve(cfg.signaling.host); return; }
    dns.lookup(cfg.signaling.host, { family: 4 }, (err, addr) => err ? reject(err) : resolve(addr));
  });

  const count = (map, ip, d) => {
    const n = (map.get(ip) || 0) + d;
    if (n <= 0) map.delete(ip); else map.set(ip, n);
    return n;
  };

  function clientIp(req) {
    const peer = (req.socket.remoteAddress || '').replace(/^::ffff:/, '');
    if (cfg.trustProxy && (peer === '127.0.0.1' || peer === '::1')) {
      const fwd = String(req.headers['x-forwarded-for'] || '').split(',')[0].trim();
      if (fwd) return fwd.replace(/^::ffff:/, '');
    }
    return peer;
  }

  function originAllowed(req) {
    const origin = req.headers.origin;
    return !!origin && cfg.origins.includes(origin);
  }

  /* ---- /gw/stream --------------------------------------------------------- */

  function openStream(ws, ip) {
    const id = nextId++;
    const s = { id, ip, ws, up: null, open: false, pending: [], pendingBytes: 0, rx: 0, tx: 0, last: Date.now() };
    streams.add(s);
    totals.streams++;
    log(`stream ${id} open from ${ip}`);

    let closed = false;
    const finish = (code, reason) => {
      if (closed) return;
      closed = true;
      streams.delete(s);
      count(perIp.stream, ip, -1);
      clearInterval(s.timer);
      if (s.up) s.up.destroy();
      if (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING) ws.close(code, reason);
      log(`stream ${id} closed (${reason || code}); ${s.tx} bytes up, ${s.rx} down`);
    };

    const onUp = () => {
      s.open = true;
      for (const b of s.pending) s.up.write(b);
      s.pending = [];
      s.pendingBytes = 0;
    };
    if (cfg.rpcn.tls) {
      s.up = tls.connect({ host: cfg.rpcn.host, port: cfg.rpcn.port, rejectUnauthorized: false, servername: undefined });
      s.up.on('secureConnect', () => {
        const got = normalizeFingerprint(s.up.getPeerCertificate().fingerprint256);
        if (pin && got !== pin) { finish(1011, 'RPCN presented an unexpected certificate'); return; }
        onUp();
      });
    } else {
      s.up = net.connect({ host: cfg.rpcn.host, port: cfg.rpcn.port }, onUp);
    }
    s.up.setNoDelay(true);
    s.up.on('data', (b) => {
      s.rx += b.length;
      s.last = Date.now();
      if (ws.readyState !== WebSocket.OPEN) return;
      ws.send(b);
      /* A browser that stops reading must not make this process buffer RPCN's
       * whole output. Pause the upstream until the socket drains. */
      if (ws.bufferedAmount > 1 << 20) s.up.pause();
    });
    s.up.on('error', (e) => finish(1011, s.open ? 'the RPCN connection failed' : 'the RPCN server could not be reached'));
    s.up.on('close', () => finish(1000, 'RPCN closed the connection'));

    ws.on('message', (data, isBinary) => {
      if (!isBinary) { finish(1003, 'binary messages only'); return; }
      s.tx += data.length;
      s.last = Date.now();
      if (s.open) { s.up.write(data); return; }
      s.pendingBytes += data.length;
      if (s.pendingBytes > cfg.limits.preConnectBytes) { finish(1009, 'too much sent before RPCN answered'); return; }
      s.pending.push(Buffer.from(data));
    });
    ws.on('close', () => finish(1000, 'the browser closed the connection'));
    ws.on('error', () => finish(1011, 'websocket error'));

    s.timer = setInterval(() => {
      if (s.up && s.up.isPaused() && ws.bufferedAmount < 1 << 18) s.up.resume();
      if (Date.now() - s.last > cfg.limits.streamIdleSec * 1000) finish(1000, 'idle');
    }, 1000);
  }

  /* ---- /gw/dgram ---------------------------------------------------------- */

  function bindUdp() {
    return new Promise((resolve) => {
      const span = cfg.udp.portMax - cfg.udp.portMin + 1;
      let tries = 0;
      const attempt = () => {
        if (tries++ >= span) { resolve(null); return; }
        const port = portCursor;
        portCursor = portCursor >= cfg.udp.portMax ? cfg.udp.portMin : portCursor + 1;
        const sock = dgram.createSocket('udp4');
        sock.once('error', () => { sock.close(); attempt(); });
        sock.bind(port, udpBind, () => { sock.removeAllListeners('error'); resolve({ sock, port }); });
      };
      attempt();
    });
  }

  function frame(ip, port, payload) {
    const out = Buffer.allocUnsafe(6 + payload.length);
    out.writeUInt32BE(ip >>> 0, 0);
    out.writeUInt16BE(port, 4);
    payload.copy(out, 6);
    return out;
  }

  async function openDgram(ws, ip) {
    const vip = pool.take();
    const bound = vip === null ? null : await bindUdp();
    if (!bound) {
      if (vip !== null) pool.release(vip);
      count(perIp.dgram, ip, -1);
      ws.close(1013, 'the gateway is full');
      return;
    }
    const id = nextId++;
    const s = {
      id, ip, ws, vip, sock: bound.sock, port: bound.port,
      out: new Bucket(cfg.limits.datagramsPerSec), outBytes: new Bucket(cfg.limits.bytesPerSec),
      in: new Bucket(cfg.limits.datagramsPerSec * 2), sent: 0, recv: 0, dropped: 0,
    };
    dgrams.set(vip, s);
    totals.dgrams++;
    log(`dgram ${id} open from ${ip}: udp ${udpBind}:${s.port}, virtual ${ipv4Text(vip)}`);

    let closed = false;
    const finish = (code, reason) => {
      if (closed) return;
      closed = true;
      dgrams.delete(vip);
      pool.release(vip);
      count(perIp.dgram, ip, -1);
      try { s.sock.close(); } catch { /* already */ }
      if (ws.readyState === WebSocket.OPEN) ws.close(code, reason);
      log(`dgram ${id} closed (${reason || code}); ${s.sent} out, ${s.recv} in, ${s.dropped} dropped`);
    };

    const deliver = (srcIp, srcPort, payload) => {
      if (ws.readyState !== WebSocket.OPEN || ws.bufferedAmount > 1 << 18) { s.dropped++; return; }
      ws.send(frame(srcIp, srcPort, payload));
    };
    s.deliver = deliver;

    s.sock.on('message', (msg, rinfo) => {
      if (!s.in.take()) { s.dropped++; return; }
      s.recv++;
      if (rinfo.address === signalingAddr && rinfo.port === cfg.signaling.port) {
        deliver(SIGNALING_TAG, SIGNALING_PORT, msg);
      } else if (net.isIPv4(rinfo.address)) {
        deliver(ipv4(rinfo.address), rinfo.port, msg);
      }
    });
    s.sock.on('error', () => finish(1011, 'udp socket error'));

    ws.on('message', (data, isBinary) => {
      if (!isBinary || data.length < 6) return;
      const payload = data.subarray(6);
      if (payload.length > cfg.limits.maxPayload) { s.dropped++; return; }
      if (!s.out.take() || !s.outBytes.take(data.length)) { s.dropped++; return; }
      const dst = data.readUInt32BE(0) >>> 0;
      const port = data.readUInt16BE(4);
      const r = route(dst, port, { pool: pool.cidr, self: selfIp, allowPrivate: cfg.allowPrivateDestinations });
      switch (r.kind) {
        case 'ping':
          ws.send(data);
          break;
        case 'signaling': {
          const pkt = rewriteSignaling(payload, vip);
          if (!pkt) { s.dropped++; break; }
          s.sent++;
          s.sock.send(pkt, cfg.signaling.port, signalingAddr);
          break;
        }
        case 'pool': {
          const peer = dgrams.get(r.vip);
          if (!peer || peer === s) { s.dropped++; break; }
          s.sent++;
          peer.deliver(vip, P2P_PORT, Buffer.from(payload));
          break;
        }
        case 'udp':
          s.sent++;
          s.sock.send(payload, port, ipv4Text(dst));
          break;
        default:
          s.dropped++;
          totals.refused++;
      }
    });
    ws.on('close', () => finish(1000, 'the browser closed the connection'));
    ws.on('error', () => finish(1011, 'websocket error'));
  }

  /* ---- HTTP + upgrade ----------------------------------------------------- */

  const server = http.createServer((req, res) => {
    if (req.url === '/gw/health') {
      res.writeHead(200, { 'content-type': 'application/json', 'cache-control': 'no-store', 'access-control-allow-origin': '*' });
      res.end(JSON.stringify({ ok: true, streams: streams.size, dgrams: dgrams.size }));
      return;
    }
    res.writeHead(404, { 'content-type': 'text/plain' });
    res.end('not found\n');
  });

  const wss = new WebSocketServer({ noServer: true, maxPayload: 1 << 20, perMessageDeflate: false });

  server.on('upgrade', (req, socket, head) => {
    const path = (req.url || '').split('?')[0];
    const kind = path === '/gw/stream' ? 'stream' : path === '/gw/dgram' ? 'dgram' : null;
    const ip = clientIp(req);
    const refuse = (status, why) => {
      totals.refused++;
      log(`refused ${kind || path} from ${ip}: ${why}`);
      socket.end(`HTTP/1.1 ${status}\r\nConnection: close\r\nContent-Length: 0\r\n\r\n`);
    };
    if (!kind) { refuse('404 Not Found', 'unknown path'); return; }
    if (!originAllowed(req)) { refuse('403 Forbidden', `origin ${req.headers.origin || '(none)'}`); return; }
    if (streams.size + dgrams.size >= cfg.limits.maxSessions) { refuse('503 Service Unavailable', 'full'); return; }
    const limit = kind === 'stream' ? cfg.limits.streamsPerIp : cfg.limits.dgramsPerIp;
    if ((perIp[kind].get(ip) || 0) >= limit) { refuse('429 Too Many Requests', 'per-address limit'); return; }
    count(perIp[kind], ip, +1);
    wss.handleUpgrade(req, socket, head, (ws) => {
      if (kind === 'stream') openStream(ws, ip);
      else openDgram(ws, ip);
    });
  });

  /* Dead browsers (a laptop lid closed) do not close their sockets. */
  const heartbeat = setInterval(() => {
    for (const ws of wss.clients) {
      if (ws.isAlive === false) { ws.terminate(); continue; }
      ws.isAlive = false;
      ws.ping();
    }
  }, 30000);
  wss.on('connection', (ws) => { ws.isAlive = true; ws.on('pong', () => { ws.isAlive = true; }); });

  await new Promise((resolve) => server.listen(cfg.listen.port, cfg.listen.host, resolve));
  const port = server.address().port;
  log(`gateway on ${cfg.listen.host}:${port} -> RPCN ${cfg.rpcn.host}:${cfg.rpcn.port}` +
      `${cfg.rpcn.tls ? '' : ' (plain TCP)'}, signaling ${signalingAddr}:${cfg.signaling.port}, ` +
      `udp ${udpBind}:${cfg.udp.portMin}-${cfg.udp.portMax}, pool ${cfg.pool}`);

  return {
    port,
    stats: () => ({ streams: streams.size, dgrams: dgrams.size, ...totals }),
    close: () => new Promise((resolve) => {
      clearInterval(heartbeat);
      for (const ws of wss.clients) ws.terminate();
      server.close(() => resolve());
    }),
  };
}

/* ---- Command line ----------------------------------------------------------- */

if (import.meta.url === `file://${process.argv[1]}` || import.meta.url.endsWith(encodeURI(process.argv[1].replace(/\\/g, '/')))) {
  const i = process.argv.indexOf('--config');
  const file = i >= 0 ? process.argv[i + 1] : null;
  const config = file ? JSON.parse(fs.readFileSync(file, 'utf8')) : {};
  startGateway(config).catch((e) => { console.error(stamp(), 'gateway failed to start:', e.message); process.exit(1); });
}
