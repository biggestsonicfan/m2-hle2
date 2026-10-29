#!/usr/bin/env python3
"""tgp-ops.py - each Model 2A TGP command's argument and reply counts, measured.

A Model 2A game talks to its TGP in calls into the program it uploaded: a header
word (op << 23) | (op << 8) | op, then the op's arguments; the program pushes
the replies back. The TGP core (src/board/tgp.h) runs that program, and
det_digest's --cop log records every word the program took from its FIFO with
the PC that took it (tag 0x22000000|PC) and every reply the i960 read (tag
0x30000000). This splits the log into commands and prints, per op, how many
were sent, the argument and reply counts seen, and the first frame (Pinboard
#1393):

    det_digest $ROMS_DIR/vf2.zip --profile vf2 --region japan --frames 3000 \\
        --cop 1:3000:/dev/shm/tgp.log --out /dev/shm/digest.txt
    python3 tools/tgp-ops.py /dev/shm/tgp.log --names commands.csv

A command starts where the program's main loop takes a word. The loop is found
by itself: the PCs at which every word read is a header, of more than one op
(VF2 2.1: 0x35; Sega Rally: 0x77, and 0x70 for its first word, which
tools/grade-tgp.py also counts). --loop-pc overrides it.

--names takes a CSV with `opcode` and `name` columns, and optionally `ps2_args`
(vf2-decomp's decomp/tgp/commands.csv has all three, from Sega's PS2 port).
With ps2_args, an op whose measured count differs from it, or varies, is
flagged, and the exit status is 1.

A count written `a,bxN` means it varied: N commands had b. The reply count of
an op that answers by its data (VF2's 0x4a, Fn_osage) varies by design.
"""

import argparse
import collections
import csv
import sys


def is_header(val):
    op = (val >> 23) & 0x7F
    return val == (op << 23) | (op << 8) | op


def read_log(path):
    with open(path) as f:
        for line in f:
            parts = line.split()
            if len(parts) != 3:
                continue
            yield int(parts[0]), int(parts[1], 16), int(parts[2], 16)


def find_loop(path):
    reads = collections.Counter()
    headers = collections.Counter()
    ops = collections.defaultdict(set)
    for _, tag, val in read_log(path):
        if tag >> 24 == 0x22:
            pc = tag & 0xFFFF
            reads[pc] += 1
            if is_header(val):
                headers[pc] += 1
                ops[pc].add((val >> 23) & 0x7F)
    # The loop reads only headers, of many ops. A handler whose argument is
    # 0.0 every time also reads only header-shaped words, but all of op 0x00
    # (Sega Rally's 0x15e, op 0x03's argument).
    return sorted(pc for pc, n in reads.items()
                  if headers[pc] == n and len(ops[pc]) > 1)


def measure(path, loops):
    ops = {}
    cur = None
    args = replies = 0

    def close():
        if cur is not None:
            o = ops[cur]
            o["n"] += 1
            o["args"][args] += 1
            o["replies"][replies] += 1

    for frame, tag, val in read_log(path):
        if tag >> 24 == 0x22:
            if tag & 0xFFFF in loops:
                close()
                cur = (val >> 23) & 0x7F
                args = replies = 0
                if cur not in ops:
                    ops[cur] = {"n": 0, "args": collections.Counter(),
                                "replies": collections.Counter(), "first": frame}
            else:
                args += 1
        elif tag == 0x30000000 and cur is not None:
            replies += 1
    close()
    return ops


def fmt(counter):
    if len(counter) == 1:
        return str(next(iter(counter)))
    return ",".join(f"{k}x{v}" for k, v in sorted(counter.items()))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("log", help="det_digest --cop log of a Model 2A game")
    ap.add_argument("--loop-pc", type=lambda s: int(s, 0), action="append",
                    help="the main loop's FIFO-read PC (repeatable); found if omitted")
    ap.add_argument("--names", help="CSV with opcode,name[,ps2_args] columns")
    a = ap.parse_args()

    loops = a.loop_pc or find_loop(a.log)
    if not loops:
        sys.exit("no PC reads only header words: is this a Model 2A --cop log?")
    print("# loop PC " + " ".join(f"0x{pc:x}" for pc in loops))

    names = {}
    if a.names:
        with open(a.names, newline="") as f:
            for row in csv.DictReader(f):
                names[int(row["opcode"], 16)] = row

    ops = measure(a.log, set(loops))
    bad = 0
    print("op\tcommands\targs\treplies\tfirst_frame\tname\tcheck")
    for op in sorted(ops):
        o = ops[op]
        row = names.get(op, {})
        check = ""
        want = row.get("ps2_args", "")
        if want != "":
            if len(o["args"]) != 1 or next(iter(o["args"])) != int(want):
                check = f"MISMATCH ps2 {want}"
                bad += 1
            else:
                check = "ok"
        print(f"0x{op:02x}\t{o['n']}\t{fmt(o['args'])}\t{fmt(o['replies'])}\t"
              f"{o['first']}\t{row.get('name', '')}\t{check}")
    print(f"# {len(ops)} ops, {sum(o['n'] for o in ops.values())} commands"
          + (f", {bad} differ from ps2_args" if bad else ""))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
