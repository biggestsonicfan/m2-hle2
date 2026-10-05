#!/usr/bin/env python3
"""dc-lockstep.py -- the Dreamcast build held against MAME, frame by frame, over
the Dreamcast's serial port (Pinboard #461).

The Dreamcast side is a `make -C dreamcast LINK=1` disc (dreamcast/dc_link.h):
it plays attract's Sonic vs Bean replay fight with the arcade profile, region
Japan, and at every game frame edge sends a record of both fighters over the
SCIF, then waits for this script's word before it runs the next frame. Flycast's
libretro core, patched with dreamcast/tools/flycast-scif.patch, carries the port
to the TCP socket FLYCAST_SCIF names; this script listens there, runs MAME's
tools/mame/match-replay.lua for the same fight (or reads a reference it took
before), and reports, as tools/match-replay.mjs does for the desktop build, the
first frame each part of the fighters parts from MAME's. At the first frame
something differs the Dreamcast is asked for the whole frame (its 'F'), so the
words that differ are named, not just the 0x100-byte block.

  tools/dc-lockstep.py --gdi <disc>/m2hle2.gdi --core <flycast_libretro.so>
      [--ref mame.bin] [--work DIR] [--frames 1300] [--show 6]

--ref names a reference already taken (match-replay.mjs --mame's mame.bin is
one); without it, MAME runs headless into --work first. --peer holds the
Dreamcast to a desktop build's run of the same fight as well: the prefix
match-replay.mjs --out leaves (DIR/here, its here.json and here.blocks.bin),
or a mame.bin-format file. MAME's differences that the desktop build shares are
the board's, not the port's. --listen PORT waits for
a Dreamcast started by hand instead of launching RetroArch (FLYCAST_SCIF must
then be 127.0.0.1:PORT in its environment).
"""
import argparse, os, socket, struct, subprocess, sys, signal, time, zlib

ROBS = (0x510D00, 0x514100)
ROB = 0x3400
FIGHT = 0x1F8                       # the fight state, sent raw
BLOCK = 0x100
EXTRA = ((0x90E800, 0x800), (0x90F600, 0x100))
EXTRA_BYTES = sum(l for _, l in EXTRA)
MREC = 8 + 2 * ROB + EXTRA_BYTES    # match-replay.lua's record
BLINK = (0x140, 0x158)              # the eye blink and gaze: rand(), never read back
HDR = 20
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def blocks(buf, lo, hi):
    return [zlib.crc32(buf[o:min(o + BLOCK, hi)]) & 0xFFFFFFFF for o in range(lo, hi, BLOCK)]


def digest(robs, extra):
    """What the Dreamcast sends for a frame (dc_link_frame), from a whole one."""
    fight = [robs[p * ROB:p * ROB + FIGHT] for p in range(2)]
    crcs = []
    for p in range(2):
        crcs += blocks(robs[p * ROB:(p + 1) * ROB], FIGHT, ROB)
    off = 0
    for _, l in EXTRA:
        crcs += blocks(extra[off:off + l], 0, l)
        off += l
    return fight, crcs


def block_name(i):
    nb = (ROB - FIGHT + BLOCK - 1) // BLOCK
    if i < 2 * nb:
        p, k = divmod(i, nb)
        lo = FIGHT + k * BLOCK
        return 'P%d+%x..%x' % (p + 1, lo, min(lo + BLOCK, ROB))
    i -= 2 * nb
    for a, l in EXTRA:
        n = (l + BLOCK - 1) // BLOCK
        if i < n:
            return 'bufferram %x' % (a + i * BLOCK)
        i -= n
    return '?'


def word_diffs(a, b, base, lo, hi, label, skip=None):
    out = []
    for w in range(lo, hi, 4):
        if skip and skip[0] <= w < skip[1]:
            continue
        x, y = struct.unpack_from('<I', a, base + w)[0], struct.unpack_from('<I', b, base + w)[0]
        if x != y:
            fx, fy = struct.unpack_from('<f', a, base + w)[0], struct.unpack_from('<f', b, base + w)[0]
            out.append('%s+%x %x/%x (%.7g/%.7g)' % (label, w, x, y, fx, fy))
    return out


class Link:
    def __init__(self, conn):
        self.c, self.buf = conn, b''

    def read(self, n):
        while len(self.buf) < n:
            d = self.c.recv(1 << 16)
            if not d:
                raise EOFError('the Dreamcast hung up')
            self.buf += d
        r, self.buf = self.buf[:n], self.buf[n:]
        return r

    def hello(self):
        """KallistiOS prints its banner on the port before the program runs:
        skip to the hello, and return what came before it."""
        text = b''
        while True:
            k = self.buf.find(b'M2LH')
            if k >= 0:
                text += self.buf[:k]
                self.buf = self.buf[k:]
                return text.decode(errors='replace')
            text += self.buf[:-3] if len(self.buf) > 3 else b''
            self.buf = self.buf[-3:]
            d = self.c.recv(1 << 16)
            if not d:
                raise EOFError('the Dreamcast hung up before its hello')
            self.buf += d

    def record(self):
        h = self.read(HDR)
        if h[:3] != b'M2L':
            raise ValueError('lost sync on the link: %r' % h)
        fc, stage, step, n, ln = struct.unpack('<IBBxxII', h[4:])
        return chr(h[3]), dict(fc=fc, stage=stage, step=step, n=n), self.read(ln)

    def say(self, c):
        self.c.sendall(c)


def load_ref(path):
    b = open(path, 'rb').read()
    out = []
    for o in range(0, len(b) - MREC + 1, MREC):
        fc, stage = struct.unpack_from('<IB', b, o)
        out.append(dict(fc=fc, stage=stage, robs=b[o + 8:o + 8 + 2 * ROB], extra=b[o + 8 + 2 * ROB:o + MREC]))
    return out


def load_capture(prefix):
    """A desktop build's run: match-replay.mjs's capture_dl prefix, or a file
    in match-replay.lua's format."""
    if os.path.isfile(prefix):
        return load_ref(prefix)
    import json
    meta = json.load(open(prefix + '.json'))
    b = open(prefix + '.blocks.bin', 'rb').read()
    per = 2 * ROB + EXTRA_BYTES
    return [dict(fc=m[2], stage=m[3], robs=b[i * per:i * per + 2 * ROB], extra=b[i * per + 2 * ROB:(i + 1) * per])
            for i, m in enumerate(meta['marks'])]


def run_mame(a, ref):
    os.makedirs(a.work, exist_ok=True)
    nv, cfg = os.path.join(a.work, 'nvram'), os.path.join(a.work, 'cfg')
    env = dict(os.environ, MR_OUT=ref, MR_FRAMES=str(a.frames), MR_ROB='%x' % ROB,
               MR_EXTRA=','.join('%x:%x' % e for e in EXTRA))
    return subprocess.Popen([a.mame, 'sfight', '-rompath', a.rompath, '-nvram_directory', nv, '-cfg_directory', cfg,
                             '-snapshot_directory', a.work, '-nodrc', '-video', 'none', '-sound', 'none', '-nothrottle',
                             '-skip_gameinfo', '-seconds_to_run', '299',
                             '-autoboot_script', os.path.join(REPO, 'tools', 'mame', 'match-replay.lua')],
                            cwd=a.work, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            start_new_session=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--gdi'), ap.add_argument('--core')
    ap.add_argument('--ref'), ap.add_argument('--work', default='/dev/shm/dc-lockstep')
    ap.add_argument('--frames', type=int, default=1300), ap.add_argument('--show', type=int, default=6)
    ap.add_argument('--peer', help="a desktop build's run of the fight (match-replay.mjs --out DIR: DIR/here)")
    ap.add_argument('--listen', type=int, default=0)
    ap.add_argument('--mame', default=os.path.expanduser('~/build/mame-bin/mame-shared/shared'))
    ap.add_argument('--rompath', default=os.environ.get('ROMS_DIR', os.path.expanduser('~/build/mameroms')))
    ap.add_argument('--retroarch-config', help='a RetroArch config (audio off, Flycast HLE BIOS)')
    ap.add_argument('--timeout', type=float, default=1800)
    a = ap.parse_args()
    deadline = time.time() + a.timeout
    os.makedirs(a.work, exist_ok=True)
    procs = []

    def stop(*_):
        for p in procs:
            try:
                os.killpg(p.pid, signal.SIGTERM)
            except OSError:
                pass
    signal.signal(signal.SIGTERM, lambda *_: (stop(), sys.exit(1)))

    try:
        ref = a.ref
        mame = None
        if not ref:
            ref = os.path.join(a.work, 'mame.bin')
            mame = run_mame(a, ref)
            procs.append(mame)
            print('MAME: playing the replay into %s' % ref, flush=True)

        srv = socket.socket()
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(('127.0.0.1', a.listen))
        srv.listen(1)
        port = srv.getsockname()[1]
        if not a.listen:
            if not (a.gdi and a.core):
                ap.error('--gdi and --core, or --listen PORT')
            cmd = ['retroarch']
            if a.retroarch_config:
                cmd += ['--config', a.retroarch_config]
            cmd += ['-L', a.core, a.gdi]
            ra_log = open(os.path.join(a.work, 'retroarch.log'), 'w')
            procs.append(subprocess.Popen(['xvfb-run', '-a', '-s', '-screen 0 800x600x24'] + cmd, cwd=a.work,
                                          env=dict(os.environ, FLYCAST_SCIF='127.0.0.1:%d' % port),
                                          stdout=ra_log, stderr=subprocess.STDOUT, start_new_session=True))
        print('waiting for the Dreamcast on 127.0.0.1:%d' % port, flush=True)
        srv.settimeout(max(1, deadline - time.time()))
        conn, _ = srv.accept()
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        conn.settimeout(max(1, deadline - time.time()))
        link = Link(conn)
        console = link.hello().strip()
        if console:
            print('console: ' + console.replace('\n', '\n console: '), flush=True)
        kind, _, build = link.record()
        if kind != 'H':
            raise ValueError('expected the hello, got %r' % kind)
        print('Dreamcast: %s' % build.decode(errors='replace'), flush=True)
        link.say(b'G')

        mref = None
        d0 = None                       # the Dreamcast's record index at the stage load
        m0 = None
        peer = load_capture(a.peer) if a.peer else None
        p0 = next((i for i, f in enumerate(peer) if f['stage'] == 1), None) if peer else None
        if peer and p0 is None:
            raise ValueError('the desktop run never loaded the replay stage')
        first = {}                      # what -> first frame it differs from MAME
        pfirst = {}                     # the same against the desktop build
        shown = 0
        n = 0
        t0 = time.time()
        while True:
            kind, h, body = link.record()
            if kind != 'R':
                raise ValueError('expected a frame record, got %r' % kind)
            if d0 is None and h['stage'] == 1:
                d0 = h['n']
            if d0 is None:
                link.say(b'G')
                continue
            if mref is None:
                if mame:
                    mame.wait(timeout=max(1, deadline - time.time()))
                mref = load_ref(ref)
                m0 = next((i for i, f in enumerate(mref) if f['stage'] == 1), None)
                if m0 is None:
                    raise ValueError('MAME never loaded the replay stage (%d records in %s)' % (len(mref), ref))
                print('aligned at the stage load: MAME frame_counter %d, Dreamcast %d' % (mref[m0]['fc'], h['fc']), flush=True)
            i = h['n'] - d0
            if m0 + i >= len(mref) or i >= a.frames:
                break
            m = mref[m0 + i]
            fight, crcs = digest(m['robs'], m['extra'])
            dfight = [body[p * FIGHT:(p + 1) * FIGHT] for p in range(2)]
            dcrcs = struct.unpack_from('<%dI' % len(crcs), body, 2 * FIGHT)
            fd = []
            for p in range(2):
                fd += word_diffs(fight[p], dfight[p], 0, 0, FIGHT, 'P%d' % (p + 1), BLINK)
            ev = lambda fs: ' '.join('%d/%d' % struct.unpack_from('<H2xH', f, 0x1A8) for f in fs)
            bad = [k for k, (x, y) in enumerate(zip(crcs, dcrcs)) if x != y]
            nb = (ROB - FIGHT + BLOCK - 1) // BLOCK
            rig = [k for k in bad if k % nb < 1 + (0x400 - FIGHT - 1) // BLOCK and k < 2 * nb]
            rest = [k for k in bad if k < 2 * nb and k not in rig]
            buf = [k for k in bad if k >= 2 * nb]
            pnews = []
            pf = None
            if peer and p0 + i < len(peer):
                pf = peer[p0 + i]
                pfight, pcrcs = digest(pf['robs'], pf['extra'])
                pbad = [k for k, (x, y) in enumerate(zip(pcrcs, dcrcs)) if x != y]
                if pfight != dfight and 'fight state' not in pfirst:
                    pfirst['fight state'] = i
                    pnews.append('fight state')
                if pbad and 'the rest' not in pfirst:
                    pfirst['the rest'] = i
                    pnews.append('the rest (%s)' % ', '.join(block_name(k) for k in pbad[:6]))
            news = []
            for what, hit in (('motion/energy', ev(fight) != ev(dfight)), ('fight state', bool(fd)),
                              ('rig', bool(rig)), ('rest of the work structure', bool(rest)), ('bufferram', bool(buf))):
                if hit and what not in first:
                    first[what] = i
                    news.append(what)
            if pnews:
                print('+%d: the Dreamcast first differs from the desktop build in %s' % (i, '; '.join(pnews)), flush=True)
            if (news or pnews) and shown < a.show:
                shown += 1
                link.say(b'F')
                kind, hf, full = link.record()
                if kind != 'F':
                    raise ValueError('expected the whole frame, got %r' % kind)
                if pnews:
                    pw = []
                    for p in range(2):
                        pw += word_diffs(pf['robs'], full, p * ROB, 0, ROB, 'P%d' % (p + 1), None)
                    mo = 0
                    for ea, el in EXTRA:
                        pw += word_diffs(pf['extra'][mo:mo + el], full[2 * ROB + mo:2 * ROB + mo + el], 0, 0, el, 'buf %x' % ea)
                        mo += el
                    for w in pw[:24]:
                        print('  desktop/Dreamcast ' + w)
                if news:
                    print('+%d (frame_counter MAME %d, Dreamcast %d): first difference in %s' % (i, m['fc'], h['fc'], ', '.join(news)))
                    print('  motion/energy MAME %s, Dreamcast %s' % (ev(fight), ev(dfight)))
                    words = []
                    for p in range(2):
                        words += word_diffs(m['robs'], full, p * ROB, 0, ROB, 'P%d' % (p + 1), None)
                    words = [w for w in words if not any(w.startswith('P%d+%x ' % (p + 1, o)) for p in range(2) for o in range(*BLINK, 4))]
                    mo = 0
                    for ea, el in EXTRA:
                        words += word_diffs(m['extra'][mo:mo + el], full[2 * ROB + mo:2 * ROB + mo + el], 0, 0, el, 'buf %x' % ea)
                        mo += el
                    for w in words[:24]:
                        print('  ' + w)
                    if len(words) > 24:
                        print('  ... %d words' % len(words))
                    sys.stdout.flush()
            n = i + 1
            if n % 120 == 0:
                print('%d frames (%.0f s)' % (n, time.time() - t0), flush=True)
            link.say(b'G')
        link.say(b'Q')
        print('\n%d frames from the stage load compared' % n)
        for what in ('motion/energy', 'fight state', 'rig', 'rest of the work structure', 'bufferram'):
            print('  %-28s %s' % (what, 'first differs at +%d' % first[what] if what in first else 'identical'))
        if peer:
            print('against the desktop build (%s):' % a.peer)
            for what in ('fight state', 'the rest'):
                print('  %-28s %s' % (what, 'first differs at +%d' % pfirst[what] if what in pfirst else 'identical'))
            return 0 if not pfirst else 1
        return 0 if not first else 1
    finally:
        stop()


if __name__ == '__main__':
    sys.exit(main())
