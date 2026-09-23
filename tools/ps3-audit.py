#!/usr/bin/env python3
"""
ps3-audit.py -- hold two ends of a PS3 Sonic the Fighters session against each other.

Each end is a log of what it put on the wire and what it took off it:

  * RPCS3's own log, with the log channels sys_net_dump (and ideally Signaling
    and sys_net) at Trace in config.yml: every datagram the game sends or
    receives, and RPCS3's signaling steps.
  * m2hle's PS3 wire log (--net-ps3-wire FILE, or "wire" on the MCP bridge's
    netplay_connect): the same, from this emulator's side, plus every line the
    PS3 link logs (net/ps3_link.h).

Either side may be either kind, so two RPCS3 logs (a PS3 against a PS3) audit
the same way as an RPCS3 log against m2hle -- which is how a PS3's own habits
become the yardstick for ours.

What it reports:

  1. The two clocks, laid on one: every datagram one side sent is found in the
     other side's receive log by its bytes. The offset between the clocks and
     the one-way delay in each direction fall out of those pairs (assuming the
     path is symmetric, which on a LAN it is to well under a millisecond).
  2. Delivery, each way: what was sent, what arrived, what never did, and
     anything that arrived that the other side does not say it sent.
  3. The shape of each side's traffic: RUDP packet types and flags and the
     game's messages per channel, side by side. A difference here is a way we
     do not behave like a PS3.
  4. The lockstep: the input frames each side sent, gaps in them, the longest
     silences, and the delay changes.
  5. Reliable channel 1: segments sent more than once (retransmissions), and
     segments that arrived twice.
  6. One timeline of both sides' events -- signaling, room calls, RUDP opens and
     resets, SyncStart, the first and last inputs, and every line m2hle's link
     logged -- on the first side's clock.

usage: ps3-audit.py SIDE_A.log SIDE_B.log [--timeline N] [--peer IP]

The decoding follows net/rudp.h and net/ps3_link.h; see those for the formats.
"""
import argparse
import collections
import datetime
import re
import statistics
import sys

# ---------------------------------------------------------------------------
# Reading the logs


class Side:
    def __init__(self, path):
        self.path = path
        self.kind = None          # 'rpcs3' or 'm2hle'
        self.start = None         # datetime of the log's time 0 (1 s resolution)
        self.packets = []         # (t, 'TX'|'RX', ip, port, vport, bytes)
        self.events = []          # (t, text)
        self.name = path


RPCS3_LINE = re.compile(r'^·(.) (\d+):(\d+):(\d+\.\d+) (.*)$')
RPCS3_DUMP = re.compile(r'sys_net_dump: (sendto|recvfrom)\(([\d.]+):(\d+):(\d+)\): ?(.*)$')
HEXLINE = re.compile(r'^(?:[0-9A-F]{2} ?)+$')
RPCS3_EVENTS = [
    re.compile(r'Signaling: (Sending \w+ packet to [\d.:]+|SP \w+ from .*|Called sig2 CB: .*|Timeout disconnection.*)'),
    re.compile(r'sceNp2: (sceNpMatching2(?:CreateJoinRoom|JoinRoom|LeaveRoom|SearchRoom|SetRoomDataInternal|SetRoomMemberDataInternal)\()'),
    re.compile(r'rpcn: (Received notification .*|JoinRoomResult .*|Join notification .*|You are now logged in.*|Disconnected)'),
]


def read_rpcs3(side):
    side.kind = 'rpcs3'
    cur = None
    with open(side.path, encoding='utf-8', errors='replace') as f:
        for ln in f:
            ln = ln.rstrip('\r\n')
            if side.start is None and ln.startswith('Current Time: '):
                side.start = datetime.datetime.fromisoformat(ln[len('Current Time: '):].strip())
                continue
            m = RPCS3_LINE.match(ln)
            if not m:
                if cur is not None and ln.strip() and HEXLINE.match(ln.strip()):
                    cur[5].extend(int(x, 16) for x in ln.split())
                    continue
                if cur is not None:
                    side.packets.append(cur); cur = None
                continue
            if cur is not None:
                side.packets.append(cur); cur = None
            t = int(m.group(2)) * 3600 + int(m.group(3)) * 60 + float(m.group(4))
            rest = m.group(5)
            d = RPCS3_DUMP.search(rest)
            if d:
                vport = int(d.group(4))
                if vport == 0:
                    continue       # librudp's own loopback wakeups, not traffic
                cur = [t, 'TX' if d.group(1) == 'sendto' else 'RX', d.group(2), int(d.group(3)), vport,
                       [int(x, 16) for x in d.group(5).split()]]
                continue
            for rx in RPCS3_EVENTS:
                e = rx.search(rest)
                if e:
                    text = e.group(1) if e.groups() else e.group(0)
                    if 'sendto' not in text:
                        side.events.append((t, 'rpcs3: ' + text[:150]))
                    break
    if cur is not None:
        side.packets.append(cur)


M2_HEAD = re.compile(r'^# m2hle ps3 wire log; start (\S+)')
M2_PKT = re.compile(r'^([\d.]+) (TX|RX) ([\d.]+):(\d+):(\d+)(.*)$')
M2_EV = re.compile(r'^([\d.]+) EV (.*)$')


def read_m2hle(side):
    side.kind = 'm2hle'
    with open(side.path, encoding='utf-8', errors='replace') as f:
        for ln in f:
            ln = ln.rstrip('\r\n')
            m = M2_HEAD.match(ln)
            if m:
                side.start = datetime.datetime.fromisoformat(m.group(1))
                continue
            m = M2_PKT.match(ln)
            if m:
                vport = int(m.group(5))
                if vport == 0:
                    side.events.append((float(m.group(1)), 'm2hle: %s vport-0 datagram (%s)'
                                        % (m.group(2), signaling_name(m.group(6)))))
                    continue
                side.packets.append([float(m.group(1)), m.group(2), m.group(3), int(m.group(4)), vport,
                                     [int(x, 16) for x in m.group(6).split()]])
                continue
            m = M2_EV.match(ln)
            if m:
                side.events.append((float(m.group(1)), 'm2hle: ' + m.group(2)))


SIG_CMDS = ['PING', 'PONG', 'CONNECT', 'CONNECT_ACK', 'CONFIRM', 'FINISHED', 'FINISHED_ACK', 'INFO']


def signaling_name(hexstr):
    b = hexstr.split()
    if len(b) == 75 and b[2] == '01':
        c = int(b[3 + 24], 16)
        return 'signaling ' + (SIG_CMDS[c] if c < len(SIG_CMDS) else str(c))
    if len(b) >= 3 and b[2] == '00':
        return 'RPCN pong'
    return '%d bytes' % len(b)


def read_side(path):
    side = Side(path)
    with open(path, encoding='utf-8', errors='replace') as f:
        head = f.read(4096)
    if head.lstrip('﻿').startswith('# m2hle ps3 wire log'):
        read_m2hle(side)
    else:
        read_rpcs3(side)
    return side

# ---------------------------------------------------------------------------
# Decoding (net/rudp.h, net/ps3_link.h)


TYPES = {0: 'DATA', 1: 'KEEPALIVE', 2: 'SYN', 3: 'RST'}
MSG = {0: 'SyncIo', 1: 'SyncIoTcp', 2: 'UpdSetting', 3: 'SyncStart', 4: 'RespSyncStart'}


def subpackets(b):
    i = 0
    while len(b) - i > 1:
        h = b[i] << 8 | b[i + 1]
        T, L = h >> 14, h & 0x7FF
        if T == 0 or L < 2 * T or i + L > len(b):
            yield None
            return
        vw = [b[i + 2 + 2 * k] << 8 | b[i + 3 + 2 * k] for k in range(T - 1)]
        yield (vw[0] if vw else None, b[i + 2 * T:i + L])
        i += L


def rudp(r):
    if len(r) < 4:
        return None
    typ, fl = r[0] >> 6, r[0] & 0x3F
    d = {'type': TYPES[typ], 'flags': fl, 'seq': r[2] << 8 | r[3]}
    o = 4
    if fl & 0x20:
        d['ack'] = r[o] << 8 | r[o + 1]; o += 2
    if fl & 0x04:
        o += 2
    if typ == 2:
        d['syn_flags'] = r[o] << 8 | r[o + 1]
        d['id'] = int.from_bytes(bytes(r[o + 6:o + 10]), 'big')
        o += 12
    elif typ == 3:
        d['reason'] = r[o] << 8 | r[o + 1]
        o += 6
    if fl & 0x10:
        while o < len(r):
            t = r[o]; o += 1
            if t & 0x7F:
                o += 1 + r[o]
            if t & 0x80:
                break
    d['payload'] = r[o:]
    return d


def message(p):
    if len(p) < 6:
        return None
    m = {'msg': MSG.get(p[0], 'msg%d' % p[0]), 'len': len(p)}
    if p[0] in (0, 1) and len(p) >= 9:
        n = p[7] << 8 | p[8]
        pk = p[9:9 + n]
        if len(pk) >= 6:
            m['frame'] = int.from_bytes(bytes(pk[0:4]), 'big')
            m['side'] = pk[4] >> 5
            m['gen'] = pk[4] & 0x1F
            m['b5'] = pk[5]
    elif p[0] == 3 and len(p) >= 12:
        m['gen'], m['side'] = p[6], p[7]
        m['sender'] = p[10] << 8 | p[11]
    elif p[0] == 4 and len(p) >= 8:
        m['target'] = p[6] << 8 | p[7]
    return m


def decode(side):
    """Per packet: a list of (channel, rudp dict, message dict or None)."""
    for pk in side.packets:
        segs = []
        for sp in subpackets(pk[5]):
            if sp is None:
                segs.append((None, None, None))
                break
            ch, r = sp
            h = rudp(r)
            m = message(h['payload']) if h and h['type'] == 'DATA' and h['payload'] else None
            segs.append((ch, h, m))
        pk.append(segs)

# ---------------------------------------------------------------------------
# Matching one side's sends to the other side's receives


def match(tx_side, rx_side, peer_of_rx, coarse):
    """Pair tx_side's TX packets with rx_side's RX packets of identical bytes.
    `coarse` is rx_clock - tx_clock to within a few seconds. Returns the pairs
    as (tx_time, rx_time) and the unmatched lists."""
    rx = collections.defaultdict(collections.deque)
    for pk in rx_side.packets:
        if pk[1] == 'RX' and (peer_of_rx is None or pk[2] == peer_of_rx):
            rx[bytes(pk[5])].append(pk)
    pairs, lost = [], []
    used = set()
    for pk in (p for p in tx_side.packets if p[1] == 'TX'):
        q = rx.get(bytes(pk[5]))
        found = None
        while q:
            cand = q[0]
            dt = cand[0] - (pk[0] + coarse)
            if dt < -2.0:            # older than this send could explain: never matched
                q.popleft(); continue
            if dt > 5.0:             # not yet: this one was lost
                break
            found = q.popleft()
            break
        if found is None:
            lost.append(pk)
        else:
            used.add(id(found))
            pairs.append((pk[0], found[0], pk))
    extra = [p for p in rx_side.packets if p[1] == 'RX' and id(p) not in used
             and (peer_of_rx is None or p[2] == peer_of_rx)]
    return pairs, lost, extra


def coarse_offset(a, b):
    """b's clock minus a's, from datagrams whose bytes occur once on each side."""
    def uniq(side, d):
        c = collections.Counter(bytes(p[5]) for p in side.packets if p[1] == d)
        return {bytes(p[5]): p[0] for p in side.packets if p[1] == d and c[bytes(p[5])] == 1}
    diffs = []
    ta, rb = uniq(a, 'TX'), uniq(b, 'RX')
    diffs += [rb[k] - ta[k] for k in ta.keys() & rb.keys()]
    tb, ra = uniq(b, 'TX'), uniq(a, 'RX')
    diffs += [tb[k] - ra[k] for k in tb.keys() & ra.keys()]
    if diffs:
        return statistics.median(diffs)
    if a.start and b.start:
        return (a.start - b.start).total_seconds()
    return 0.0

# ---------------------------------------------------------------------------
# Reports


def pct(v, q):
    if not v:
        return float('nan')
    v = sorted(v)
    return v[min(len(v) - 1, int(q * (len(v) - 1) + 0.5))]


def shape(side):
    c = collections.Counter()
    for pk in side.packets:
        if pk[1] != 'TX':
            continue
        for ch, h, m in pk[6]:
            if h is None:
                c[('bad sub-packet',)] += 1
                continue
            key = ('ch%s' % ch, h['type'], 'f%02x' % h['flags'])
            if m:
                key += (m['msg'], m['len'])
            elif h['type'] == 'DATA':
                key += ('(empty: ack)',)
            if h['type'] == 'SYN':
                key += ('syn_flags %04x' % h['syn_flags'],)
            c[key] += 1
    return c


def lockstep(side):
    """The input frames this side sent, per generation run (a new run starts
    when the frame number goes back to 0)."""
    runs = []
    for pk in side.packets:
        if pk[1] != 'TX':
            continue
        for ch, h, m in pk[6]:
            if m and m['msg'] == 'SyncIo' and 'frame' in m:
                if not runs or m['frame'] < runs[-1]['last'] - 30:
                    runs.append({'t0': pk[0], 'first': m['frame'], 'last': m['frame'], 'times': [],
                                 'frames': [], 'delays': [], 'side': m['side'], 'gen': m['gen']})
                r = runs[-1]
                r['last'] = max(r['last'], m['frame'])
                r['times'].append(pk[0])
                r['frames'].append(m['frame'])
            if m and m['msg'] == 'SyncIoTcp' and 'b5' in m and m['b5'] & 0x80 and runs:
                runs[-1]['delays'].append((m['frame'], (m['b5'] >> 3) & 0xF))
    return runs


def retransmits(side):
    seen = collections.Counter()
    for pk in side.packets:
        if pk[1] != 'TX':
            continue
        for ch, h, m in pk[6]:
            if h and ch == 1 and h['type'] == 'DATA' and h['payload']:
                seen[h['seq']] += 1
    return sum(1 for v in seen.values() if v > 1), sum(v - 1 for v in seen.values() if v > 1)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('a')
    ap.add_argument('b')
    ap.add_argument('--timeline', type=int, default=80, help='timeline lines to print (0 = none, -1 = all)')
    args = ap.parse_args()

    A, B = read_side(args.a), read_side(args.b)
    for s in (A, B):
        decode(s)
        s.name = '%s (%s)' % (s.path.replace('\\', '/').split('/')[-1], s.kind)
    print('A: %s  %d datagrams, %d events, start %s' % (A.name, len(A.packets), len(A.events), A.start))
    print('B: %s  %d datagrams, %d events, start %s' % (B.name, len(B.packets), len(B.events), B.start))
    if not A.packets or not B.packets:
        print('\nnothing to hold together: a side has no game datagrams (sys_net_dump at Trace? --net-ps3-wire?)')
        return 1

    # 1. Clocks
    coarse = coarse_offset(A, B)
    ab, lost_ab, extra_b = match(A, B, None, coarse)
    ba, lost_ba, extra_a = match(B, A, None, -coarse)
    d_ab = [rx - tx for tx, rx, _ in ab]          # latency + offset
    d_ba = [rx - tx for tx, rx, _ in ba]          # latency - offset
    if d_ab and d_ba:
        mab, mba = statistics.median(d_ab), statistics.median(d_ba)
        off = (mab - mba) / 2
    else:
        off = coarse
    # Only the stretch both logs cover: an RPCS3 log can hold a whole evening of
    # sessions against other peers, and those are not this pairing's to judge.
    ta = [p[0] for p in A.packets if p[1] == 'TX' or p[1] == 'RX']
    tb = [p[0] - off for p in B.packets]
    lo, hi = max(min(ta), min(tb)) - 2.0, min(max(ta), max(tb)) + 2.0
    A.packets = [p for p in A.packets if lo <= p[0] <= hi]
    B.packets = [p for p in B.packets if lo <= p[0] - off <= hi]
    A.events = [e for e in A.events if lo - 30 <= e[0] <= hi + 30]
    B.events = [e for e in B.events if lo - 30 <= e[0] - off <= hi + 30]
    ab, lost_ab, extra_b = match(A, B, None, off)
    ba, lost_ba, extra_a = match(B, A, None, -off)
    d_ab = [rx - tx for tx, rx, _ in ab]
    d_ba = [rx - tx for tx, rx, _ in ba]
    print("(audited: %.1f s of overlap, A's clock %.1f .. %.1f)" % (hi - lo, lo, hi))
    lat_ab = [d - off for d in d_ab]
    lat_ba = [d + off for d in d_ba]
    print('\n== 1. Clocks')
    print('B clock - A clock = %+.4f s  (from %d + %d matched datagrams)' % (off, len(ab), len(ba)))
    if A.start and B.start:
        print('  (the logs\' own start stamps say %+.0f s; the difference is the two machines\' clocks)'
              % (A.start - B.start).total_seconds())
    for name, lat in (('A -> B', lat_ab), ('B -> A', lat_ba)):
        if lat:
            print('  %s one-way: median %.1f ms, p95 %.1f ms, max %.1f ms'
                  % (name, 1000 * statistics.median(lat), 1000 * pct(lat, 0.95), 1000 * max(lat)))

    # 2. Delivery
    print('\n== 2. Delivery')
    for name, sent, pairs, lost, extra in (('A -> B', A, ab, lost_ab, extra_b), ('B -> A', B, ba, lost_ba, extra_a)):
        tx = sum(1 for p in sent.packets if p[1] == 'TX')
        print('  %s: %d sent, %d arrived, %d never arrived, %d arrived unexplained'
              % (name, tx, len(pairs), len(lost), len(extra)))
        for p in lost[:5]:
            segs = ', '.join('ch%s %s%s' % (ch, h['type'] if h else '?', (' ' + m['msg']) if m else '')
                             for ch, h, m in p[6])
            print('      lost at %.3f: %s' % (p[0], segs))

    # 3. Shape
    print('\n== 3. What each side sends (channel, RUDP type, flags, message, bytes)')
    sa, sb = shape(A), shape(B)
    keys = sorted(set(sa) | set(sb), key=lambda k: tuple(str(x) for x in k))
    print('  %-70s %8s %8s' % ('', 'A', 'B'))
    for k in keys:
        mark = '' if (sa[k] > 0) == (sb[k] > 0) else '   <-- only one side'
        print('  %-70s %8d %8d%s' % (' '.join(str(x) for x in k), sa[k], sb[k], mark))

    # 4. Lockstep
    print('\n== 4. Lockstep: input frames sent')
    for name, side in (('A', A), ('B', B)):
        for r in lockstep(side):
            frames = sorted(set(r['frames']))
            missing = (r['last'] - r['first'] + 1) - len(frames)
            gaps = [b - a for a, b in zip(r['times'], r['times'][1:])]
            print('  %s side %d gen %d: frames %d..%d over %.1f s, %d missing, longest silence %.0f ms, '
                  'delay changes %s' % (name, r['side'], r['gen'], r['first'], r['last'],
                                         r['times'][-1] - r['times'][0], missing,
                                         1000 * max(gaps) if gaps else 0,
                                         ' '.join('f%d:%d' % x for x in r['delays']) or 'none'))

    # 5. Reliable channel
    print('\n== 5. Reliable channel 1')
    for name, side in (('A', A), ('B', B)):
        n, extra = retransmits(side)
        print('  %s: %d segments sent more than once (%d extra copies)' % (name, n, extra))

    # 6. Timeline, on A's clock
    if args.timeline:
        ev = [(t, 'A', e) for t, e in A.events] + [(t - off, 'B', e) for t, e in B.events]
        for side, tag, sh in ((A, 'A', 0.0), (B, 'B', off)):
            for pk in side.packets:
                for ch, h, m in pk[6]:
                    if h and h['type'] in ('SYN', 'RST'):
                        ev.append((pk[0] - sh, tag, '%s ch%s %s%s' % (pk[1], ch, h['type'],
                                   ' ACK' if h['flags'] & 0x20 else '')))
                    if m and m['msg'] in ('SyncStart', 'RespSyncStart', 'UpdSetting'):
                        ev.append((pk[0] - sh, tag, '%s %s %s' % (pk[1], m['msg'],
                                   ' '.join('%s=%s' % (k, v) for k, v in m.items() if k not in ('msg', 'len')))))
        ev.sort()
        print('\n== 6. Timeline (A\'s clock, seconds)')
        shown = ev if args.timeline < 0 else ev[:args.timeline]
        for t, tag, e in shown:
            print('  %10.3f %s %s' % (t, tag, e))
        if len(shown) < len(ev):
            print('  ... %d more (--timeline -1 for all)' % (len(ev) - len(shown)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
