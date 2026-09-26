#!/usr/bin/env python3
"""
av-nv12-check.py — hold the A/V tap's GPU NV12 (--av-format nv12) against
swscale's conversion of the BGRA tap.

    python tools/av-nv12-check.py                       # card, then attract
    python tools/av-nv12-check.py --only card --size 1920x1080

Two checks, each on two headless emulators side by side, one per format:

  card     --av-test-card: eight full bars over a grey ramp. The BGRA tap must
           be the card exactly (that pins the BGRA path's orientation), and the
           NV12 tap is held against ffmpeg's BT.709 limited-range conversion of
           the same card. Pure red must come out Y 63, Cb 102, Cr 240.
  attract  the game itself, a board frame both runs rendered (every packet
           names its board frame, and the board is deterministic from boot).

Reported per plane: the largest difference, and how many samples are off by
more than 1. Chroma is compared against swscale twice, with its default
filter and with `area` (a 2x2 box, which is what the GPU does), because the
two only disagree at colour edges. The pass criterion is the issue's: Y, Cb
and Cr within +-1 of the box conversion, and nothing upside down.
"""
import argparse, os, socket, struct, subprocess, sys, threading, time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
BARS = [(255, 255, 255), (255, 255, 0), (0, 255, 255), (0, 255, 0),
        (255, 0, 255), (255, 0, 0), (0, 0, 255), (0, 0, 0)]


def card_rgb(w, h):
    """The card as av_capture.h draws it: top row first, RGB."""
    img = np.zeros((h, w, 3), np.uint8)
    x = np.arange(w)
    bar = np.array(BARS, np.uint8)[x * 8 // w]
    top = h * 2 // 3
    img[:top] = bar[None, :, :]
    ramp = (x * 255 // (w - 1)).astype(np.uint8)
    img[top:] = ramp[None, :, None]
    return img


WINDOW = False   # --window: stream from a window (the GL builds have no headless path)


def launch(exe, rom, port, fmt, size, card):
    args = [exe, "--rom", rom, "--profile", "sfight", "--region", "japan", "--run",
            "--av-mute", "--av-port", str(port), "--av-size", size, "--av-format", fmt]
    if not WINDOW:
        args += ["--headless", "--no-tray"]
    if card:
        args.append("--av-test-card")
    return subprocess.Popen(args, cwd=os.path.dirname(rom),
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


class Tap:
    def __init__(self, port):
        deadline = time.time() + 30
        while True:
            try:
                self.s = socket.create_connection(("127.0.0.1", port), timeout=15)
                break
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.25)
        self.s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8 << 20)
        self.s.settimeout(30)
        h = self.rd(32)
        if h[0:4] != b"M2AV":
            sys.exit("not an M2AV stream")
        self.w, self.h = struct.unpack_from("<HH", h, 8)
        self.pixfmt = h[12:16].decode()

    def rd(self, n):
        buf = bytearray(n)
        view = memoryview(buf)
        got = 0
        while got < n:
            k = self.s.recv_into(view[got:], n - got)
            if not k:
                raise EOFError("the emulator closed the stream")
            got += k
        return bytes(buf)

    def frames(self):
        """Yield (board frame, payload) for every video packet."""
        while True:
            ph = self.rd(24)
            kind, size, frame = ph[0], struct.unpack_from("<I", ph, 4)[0], struct.unpack_from("<Q", ph, 8)[0]
            body = self.rd(size)
            if kind == ord("V"):
                yield frame, body

    def close(self):
        self.s.close()


def split_nv12(buf, w, h):
    y = np.frombuffer(buf, np.uint8, w * h).reshape(h, w)
    c = np.frombuffer(buf, np.uint8, w * h // 2, w * h).reshape(h // 2, w // 2, 2)
    return y, c[:, :, 0], c[:, :, 1]


def swscale_nv12(bgra, w, h, flags):
    vf = "scale=flags=%s:out_color_matrix=bt709:out_range=limited" % flags
    out = subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "bgra",
         "-s", "%dx%d" % (w, h), "-i", "pipe:0", "-vf", vf, "-pix_fmt", "nv12",
         "-f", "rawvideo", "pipe:1"],
        input=bgra, capture_output=True, check=True).stdout
    return split_nv12(out, w, h)


def diff(name, got, ref):
    d = np.abs(got.astype(np.int16) - ref.astype(np.int16))
    over = int((d > 1).sum())
    print("    %-3s max %3d   >1: %7d of %d (%.4f%%)" % (name, d.max(), over, d.size, 100.0 * over / d.size))
    return int(d.max()), over


def compare(nv12, bgra, w, h):
    got = split_nv12(nv12, w, h)
    ok = True
    for flags in ("area", "bicubic"):
        print("  vs swscale %s:" % flags)
        ref = swscale_nv12(bgra, w, h, flags)
        for name, g, r in zip(("Y", "Cb", "Cr"), got, ref):
            m, _ = diff(name, g, r)
            if flags == "area" and m > 1:
                ok = False
    return ok, got


def grab(exe, rom, size, card, want, span, base_port):
    """Start both runs, return {fmt: (tap dims, {frame: payload})} over [want, want+span]."""
    procs, out = [], {}
    try:
        for i, fmt in enumerate(("bgra", "nv12")):
            procs.append(launch(exe, rom, base_port + i, fmt, size, card))
        # Both at once: a run with no client is not captured, and the other
        # would be past the window by the time the first was read.
        taps = {fmt: Tap(base_port + i) for i, fmt in enumerate(("bgra", "nv12"))}

        def read(fmt):
            tap, got = taps[fmt], {}
            for frame, body in tap.frames():
                if frame >= want:
                    got[frame] = body
                if frame >= want + span:
                    break
            out[fmt] = ((tap.w, tap.h, tap.pixfmt), got)
            tap.close()

        threads = [threading.Thread(target=read, args=(f,)) for f in taps]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        if len(out) != 2:
            raise RuntimeError("a tap stopped before the window was read")
    finally:
        for p in procs:
            p.terminate()
        for p in procs:
            try:
                p.wait(10)
            except subprocess.TimeoutExpired:
                p.kill()
    return out


def check_card(args):
    print("card (%s):" % args.size)
    out = grab(args.exe, args.rom, args.size, True, 1, 30, args.port)
    (bw, bh, bfmt), bf = out["bgra"]
    (nw, nh, nfmt), nf = out["nv12"]
    if bfmt != "BGRA" or nfmt != "NV12" or (bw, bh) != (nw, nh):
        print("  FAIL: streams say %s %dx%d and %s %dx%d" % (bfmt, bw, bh, nfmt, nw, nh))
        return False
    w, h = nw, nh
    ref = card_rgb(w, h)
    bgra = np.frombuffer(next(iter(bf.values())), np.uint8).reshape(h, w, 4)
    exact = np.array_equal(bgra[:, :, 2::-1], ref)
    print("  BGRA tap is the card exactly: %s" % ("yes" if exact else "NO"))
    ok, (y, cb, cr) = compare(next(iter(nf.values())), bgra.tobytes(), w, h)
    # Middle of the red bar (bar 5), well inside it on both axes.
    ry, rx = h // 3, (5 * w // 8 + 6 * w // 8) // 2
    red = (int(y[ry, rx]), int(cb[ry // 2, rx // 2]), int(cr[ry // 2, rx // 2]))
    print("  pure red: Y %d Cb %d Cr %d (want 63 102 240)" % red)
    # Upside down would put the ramp (grey, C 128) where the bars are.
    flipped = int(y[0, 0]) != 235 or int(y[h - 1, 0]) != 16
    print("  orientation: top-left Y %d (white, 235), bottom-left Y %d (black, 16)%s"
          % (y[0, 0], y[h - 1, 0], "  UPSIDE DOWN?" if flipped else ""))
    good = exact and ok and red == (63, 102, 240) and not flipped
    print("  card: %s" % ("PASS" if good else "FAIL"))
    return good


def check_attract(args):
    print("attract (%s), from board frame %d:" % (args.size, args.frame))
    out = grab(args.exe, args.rom, args.size, False, args.frame, 60, args.port + 2)
    (w, h, _), bf = out["bgra"]
    _, nf = out["nv12"]
    common = sorted(set(bf) & set(nf))
    if not common:
        print("  FAIL: the two runs share no board frame in the window")
        return False
    frame = common[len(common) // 2]
    print("  board frame %d (%d frames in common)" % (frame, len(common)))
    ok, _ = compare(nf[frame], bf[frame], w, h)
    print("  attract: %s" % ("PASS" if ok else "FAIL"))
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("--exe", default=os.path.join(REPO, "build", "Release", "m2hle.exe"))
    ap.add_argument("--rom", default=os.path.join(REPO, "test_m2snake", "sfight.zip"))
    ap.add_argument("--size", default="1396x1080")
    ap.add_argument("--frame", type=int, default=1800, help="attract: first board frame to take")
    ap.add_argument("--port", type=int, default=7280, help="first of four ports")
    ap.add_argument("--only", choices=("card", "attract"))
    ap.add_argument("--window", action="store_true",
                    help="run the emulators windowed (GL builds; e.g. under xvfb-run on Linux)")
    args = ap.parse_args()
    global WINDOW
    WINDOW = args.window
    args.rom = os.path.abspath(args.rom)

    ok = True
    if args.only in (None, "card"):
        ok &= check_card(args)
    if args.only in (None, "attract"):
        ok &= check_attract(args)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
