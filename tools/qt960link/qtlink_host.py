#!/usr/bin/env python3
"""qtlink_host.py — the PC on the end of a QT960's serial cable (Pinboard #316).

What qt960_link.lua does inside MAME, for a real board: it types qtlink.bin into the
QT960's SRAM through NINDY's "mo" command, starts it with "go", and then relays. qtlink
writes each command frame for the Model 2B as a text line ">A5....\\r"; this sends the
bytes to the Model 2B (m2-kernel) and types the kernel's reply back as "<5A....\\r".

  python3 qtlink_host.py --board COM3 --m2k tcp:127.0.0.1:7960        # Windows
  python3 qtlink_host.py --board /dev/ttyUSB0 --m2k /dev/ttyUSB1      # both real boards
  python3 qtlink_host.py --board tcp:127.0.0.1:7961 --m2k tcp:127.0.0.1:7960   # both in MAME

An end is "tcp:HOST:PORT" (a MAME bridge: qt960_wire.lua for the QT960, m2k_serial.lua for
the Model 2B) or a serial device, which needs pyserial (pip install pyserial).

Power the QT960 on (or reset it) after this starts, or press Enter at its prompt yourself
first: it waits for NINDY's "=>". qtlink never returns to NINDY; reset the board to stop it.
"""
import argparse
import os
import re
import socket
import sys
import time

BASE = 0x10100000


class Tcp:
    def __init__(self, hostport):
        host, port = hostport.rsplit(":", 1)
        deadline = time.time() + 60
        while True:
            try:
                self.s = socket.create_connection((host, int(port)), timeout=5)
                break
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.5)
        self.s.settimeout(0.02)

    def read(self):
        try:
            got = self.s.recv(4096)
        except socket.timeout:
            return b""
        if not got:
            raise EOFError("the other end hung up")
        return got

    def write(self, b):
        self.s.sendall(b)


class Serial:
    def __init__(self, dev, baud):
        try:
            import serial
        except ImportError:
            sys.exit("qtlink_host: a serial port needs pyserial (pip install pyserial)")
        self.p = serial.Serial(dev, baud, bytesize=8, parity="N", stopbits=1, timeout=0.02)

    def read(self):
        return self.p.read(4096)

    def write(self, b):
        self.p.write(b)


def open_end(spec, baud):
    return Tcp(spec[4:]) if spec.startswith("tcp:") else Serial(spec, baud)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--board", required=True, help="the QT960: COMn, /dev/ttyX or tcp:HOST:PORT")
    ap.add_argument("--m2k", default="tcp:127.0.0.1:7960", help="the Model 2B (m2-kernel)")
    ap.add_argument("--baud", type=int, default=9600, help="the QT960's line (NINDY: 9600)")
    ap.add_argument("--m2k-baud", type=int, default=9600, help="the Model 2B's line, if serial")
    ap.add_argument("--bin", default=os.path.join(here, "qtlink.bin"), help="qtlink/build.sh's image")
    ap.add_argument("--checks", type=int, default=0, help="stop after N checks (0: run on)")
    ap.add_argument("--no-load", action="store_true", help="qtlink is already in SRAM: only 'go'")
    ap.add_argument("--quiet", action="store_true", help="don't echo the board's output")
    a = ap.parse_args()

    img = open(a.bin, "rb").read()
    img += b"\0" * (-len(img) % 4)
    words = [int.from_bytes(img[i:i + 4], "little") for i in range(0, len(img), 4)]
    print(f"qtlink_host: {a.bin}: {len(words)} words", file=sys.stderr)

    board = open_end(a.board, a.baud)
    m2k = None
    show = (lambda b: None) if a.quiet else (lambda b: (sys.stdout.write(b.decode("latin-1")), sys.stdout.flush()))

    # ---- load: answer each prompt NINDY gives, one line at a time ---------------------
    tail = ""

    def wait_for(pattern, timeout, poke=None):
        nonlocal tail
        rx = re.compile(pattern)
        deadline = time.time() + timeout
        nudge = time.time() + 3
        while time.time() < deadline:
            got = board.read()
            if got:
                show(got)
                tail = (tail + got.decode("latin-1"))[-256:]
                if rx.search(tail):
                    tail = ""
                    return
            elif poke and time.time() > nudge:
                board.write(poke)
                nudge = time.time() + 3
        sys.exit(f"\nqtlink_host: no {pattern!r} from the board in {timeout} s")

    def send(s):
        board.write(s.encode() + b"\r")

    # NINDY prints "=>" after its self-test, then resets, prints its banner and "=>" again.
    # So take a prompt only once the board has been quiet after it for a second. A board
    # already sitting at the prompt prints nothing, so press Enter until one comes.
    wait_for(r"=>\s*$", 600, poke=b"\r")
    while True:
        quiet = time.time() + 1.5
        late = b""
        while time.time() < quiet and not late:
            late = board.read()
        if not late:
            break
        show(late)
        tail = late.decode("latin-1")
        if not re.search(r"=>\s*$", tail):
            wait_for(r"=>\s*$", 600, poke=b"\r")
    if not a.no_load:
        t0 = time.time()
        send(f"mo {BASE:08x} {len(words)}")              # the count is decimal
        for i, w in enumerate(words):
            # "addr : old : ": NINDY reads the line while it prints, so wait for the
            # second colon before typing the word.
            wait_for(r" : [0-9A-Fa-f]+ : $", 10)
            send(f"{w:08x}")
            if a.quiet and i % 256 == 0:
                print(f"qtlink_host: {i}/{len(words)} words", file=sys.stderr)
        wait_for(r"=>\s*$", 10)
        print(f"\nqtlink_host: loaded {len(words)} words in {time.time() - t0:.0f} s", file=sys.stderr)
    m2k = open_end(a.m2k, a.m2k_baud)
    print(f"qtlink_host: Model 2B at {a.m2k}; go {BASE:08x}", file=sys.stderr)
    send(f"go {BASE:08x}")

    # ---- relay ------------------------------------------------------------------------
    line, rx, checks, diffs = b"", b"", 0, 0
    while True:
        for c in board.read():
            if c in (10, 13):
                s = line.decode("latin-1")
                line = b""
                m = re.search(r">(A5[0-9A-Fa-f]+)$", s)   # a frame may end a line of text
                if m and len(m.group(1)) % 2 == 0:
                    m2k.write(bytes.fromhex(m.group(1)))
                elif s:
                    if not a.quiet or s.endswith("  DIFF"):
                        print(s)
                    if re.match(r"^\s*\d+ \S+ .*  (OK|DIFF)$", s):
                        checks += 1
                        diffs += s.endswith("  DIFF")
                    if a.checks and checks >= a.checks:
                        print(f"qtlink_host: {checks} checks, {diffs} differences")
                        return 1 if diffs else 0
            else:
                line += bytes([c])
        rx += m2k.read()
        while len(rx) >= 4:                                # 5A status len payload sum
            if rx[0] != 0x5A:
                rx = rx[1:]
                continue
            n = 4 + rx[2]
            if len(rx) < n:
                break
            send("<" + rx[:n].hex().upper())
            rx = rx[n:]


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
