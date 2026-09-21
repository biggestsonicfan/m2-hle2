/*
 * rules.mjs -- the gateway's decisions about addresses, kept apart from its
 * sockets so they can be tested without opening any (test/rules.test.mjs).
 *
 * Addresses are IPv4 as a uint32 in host order (1.2.3.4 = 0x01020304). The
 * datagram frame carries them in network order, which is the same four bytes
 * read big-endian.
 */

/* The address the web client sends RPCN signaling to, and the one signaling
 * replies are relabelled as coming from. A browser cannot resolve names, and
 * rpcn_connect wants the signaling address the moment the stream is opened, so
 * the client uses this constant and the gateway maps it to the real helper. It
 * sits in shared address space (100.64.0.0/10) but outside the virtual pool, so
 * it can never be a player. Must match WEB_SIGNALING_TAG in src/net/web_socket.h. */
export const SIGNALING_TAG = ipv4('100.127.255.254');
export const SIGNALING_PORT = 3657;

/* RPCN hands two players who share a public address each other's LOCAL address
 * with this port hard-coded (room_manager.rs, cmd_misc.rs). Every browser player
 * shares the gateway's address, so browser-to-browser traffic always arrives
 * addressed to <virtual address>:3658. */
export const P2P_PORT = 3658;

export function ipv4(text) {
  const parts = String(text).split('.');
  if (parts.length !== 4) throw new Error(`not an IPv4 address: ${text}`);
  let v = 0;
  for (const p of parts) {
    const n = Number(p);
    if (!/^\d{1,3}$/.test(p) || n > 255) throw new Error(`not an IPv4 address: ${text}`);
    v = v * 256 + n;
  }
  return v >>> 0;
}

export function ipv4Text(v) {
  return [v >>> 24, (v >>> 16) & 255, (v >>> 8) & 255, v & 255].join('.');
}

export function parseCidr(text) {
  const [addr, bitsText] = String(text).split('/');
  const bits = Number(bitsText);
  if (!(bits >= 1 && bits <= 30)) throw new Error(`pool must be a CIDR block of /1../30: ${text}`);
  const mask = bits === 0 ? 0 : (0xFFFFFFFF << (32 - bits)) >>> 0;
  const base = (ipv4(addr) & mask) >>> 0;
  return { base, mask, bits, size: 2 ** (32 - bits) };
}

export function inCidr(ip, cidr) {
  return ((ip & cidr.mask) >>> 0) === cidr.base;
}

/* Everything that is not a public unicast address. A relay that sends to any of
 * these is an SSRF hole into the droplet's own network, or a way to reach
 * services on loopback. */
const NOT_PUBLIC = [
  '0.0.0.0/8', '10.0.0.0/8', '100.64.0.0/10', '127.0.0.0/8', '169.254.0.0/16',
  '172.16.0.0/12', '192.0.0.0/24', '192.0.2.0/24', '192.88.99.0/24', '192.168.0.0/16',
  '198.18.0.0/15', '198.51.100.0/24', '203.0.113.0/24', '224.0.0.0/4', '240.0.0.0/4',
].map((c) => {
  const [addr, bits] = c.split('/');
  const mask = (0xFFFFFFFF << (32 - Number(bits))) >>> 0;
  return { base: (ipv4(addr) & mask) >>> 0, mask };
});

export function isPublicUnicast(ip) {
  return !NOT_PUBLIC.some((c) => ((ip & c.mask) >>> 0) === c.base);
}

/*
 * Where an outgoing datagram goes. `dst` / `port` are what the client framed;
 * the answer is one of
 *   { kind: 'ping' }                       echo it straight back (RTT probe)
 *   { kind: 'signaling' }                  rewrite local_addr, send to RPCN's helper
 *   { kind: 'pool', vip }                  deliver to the session holding vip
 *   { kind: 'udp' }                        send it out of the session's socket
 *   { kind: 'refuse', why }
 */
export function route(dst, port, { pool, self, allowPrivate = false }) {
  if (dst === 0 && port === 0) return { kind: 'ping' };
  if (dst === SIGNALING_TAG) {
    return port === SIGNALING_PORT ? { kind: 'signaling' } : { kind: 'refuse', why: 'signaling port' };
  }
  if (inCidr(dst, pool)) {
    return port === P2P_PORT ? { kind: 'pool', vip: dst } : { kind: 'refuse', why: 'pool port' };
  }
  if (port < 1024) return { kind: 'refuse', why: 'port below 1024' };
  if (allowPrivate) return { kind: 'udp' };   /* a development gateway; see gateway.mjs */
  if (self !== undefined && dst === self) return { kind: 'refuse', why: 'the gateway itself' };
  if (!isPublicUnicast(dst)) return { kind: 'refuse', why: 'not a public address' };
  return { kind: 'udp' };
}

/* A signaling keepalive is exactly [1][user_id: u64 LE][local_addr: 4]
 * (udp_server.rs rejects anything else). Returns a rewritten copy carrying the
 * session's virtual address, or null if this is not one. */
export function rewriteSignaling(payload, vip) {
  if (payload.length !== 13 || payload[0] !== 1) return null;
  const out = Buffer.from(payload);
  out.writeUInt32BE(vip >>> 0, 9);
  return out;
}

/* Hands out virtual addresses from the pool: never the network or broadcast
 * address, never one still in use, lowest free first after a moving cursor so
 * an address just released is not immediately reissued. */
export class AddressPool {
  constructor(cidr) {
    this.cidr = typeof cidr === 'string' ? parseCidr(cidr) : cidr;
    this.used = new Set();
    this.cursor = 1;
  }

  take() {
    const n = this.cidr.size - 2;
    for (let i = 0; i < n; i++) {
      const off = 1 + ((this.cursor - 1 + i) % n);
      const ip = (this.cidr.base + off) >>> 0;
      if (ip === SIGNALING_TAG || this.used.has(ip)) continue;
      this.used.add(ip);
      this.cursor = off + 1;
      return ip;
    }
    return null;
  }

  release(ip) { this.used.delete(ip); }
}

/* A token bucket: `rate` per second, bursting to `rate`. */
export class Bucket {
  constructor(rate, now = Date.now()) {
    this.rate = rate;
    this.tokens = rate;
    this.at = now;
  }

  take(n = 1, now = Date.now()) {
    this.tokens = Math.min(this.rate, this.tokens + ((now - this.at) / 1000) * this.rate);
    this.at = now;
    if (this.tokens < n) return false;
    this.tokens -= n;
    return true;
  }
}

/* "AB:CD:..." / "abcd..." -> "ABCD..." (64 hex digits) or '' */
export function normalizeFingerprint(text) {
  const hex = String(text || '').replace(/[^0-9a-fA-F]/g, '').toUpperCase();
  return hex.length === 64 ? hex : '';
}
