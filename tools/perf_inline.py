#!/usr/bin/env python3
"""perf_inline.py -- a perf profile of m2hle by INLINED function.

The board is header-only and built at -O3, so nearly everything is inlined into
a handful of real functions: `perf report` puts ~30% of a run on
emu_slice_body, which is the i960, its bus, the timers and the HLE hook check
all at once. This charges each sample to the innermost inlined function at its
address instead (addr2line -i), which is what a reader of the source means by
"function". See PERF-PROFILE.md.

    perf record -F 2000 --call-graph fp -o run.data -- <exe> ...
    tools/perf_inline.py run.data <exe> [--top N] [--chain] [--callers SYM]

  --chain        key each sample by its inline chain (innermost first, 4 deep)
                 instead of the innermost function alone
  --callers SYM  only samples whose leaf is SYM (a libc memcpy, say), charged
                 to the first frame of <exe> above it. With frame pointers a
                 leaf that sets up no frame hides its direct caller, so record
                 with --call-graph dwarf for this, or use
                 `perf report --inline -G -S SYM` on a dwarf recording.

Percentages are of every sample in the recording, all threads; filter a thread
with `perf record -t` or `perf script --comms` upstream if it matters.
"""
import argparse
import collections
import subprocess


def load_symbols(exe):
    syms = {}
    nm = subprocess.run(["nm", "--defined-only", exe], capture_output=True, text=True).stdout
    for line in nm.splitlines():
        p = line.split()
        if len(p) == 3:
            syms.setdefault(p[2], int(p[0], 16))
    return syms


def load_records(data):
    """Each sample's frames, leaf first, as 'sym+0xoff (dso)' strings."""
    out = subprocess.run(["perf", "script", "-i", data, "-F", "ip,sym,symoff,dso"],
                         capture_output=True, text=True).stdout
    recs, cur = [], []
    for line in out.split("\n"):
        line = line.strip()
        if not line:
            if cur:
                recs.append(cur)
                cur = []
            continue
        frame = line.split(None, 1)[1] if " " in line else line
        # The kernel's frames under a user-space-only event go. A dwarf unwind
        # adds a line per inlined level, a libc memcpy's included; they stay,
        # marked, for --callers to match on.
        if frame != "[unknown] ([unknown])":
            cur.append(frame)
    if cur:
        recs.append(cur)
    return recs


def frame_addr(frame, exe_name, syms):
    """File address of a frame in exe, or None (another DSO, unknown symbol)."""
    so, _, dso = frame.rpartition(" (")
    if dso.rstrip(")").split("/")[-1] != exe_name:
        return None
    sym, _, off = so.rpartition("+0x")
    if sym not in syms:
        return None
    return syms[sym] + int(off, 16)


def inline_chains(exe, addrs):
    """addr -> [(function, file:line), ...], innermost first."""
    if not addrs:
        return {}
    out = subprocess.run(["addr2line", "-a", "-i", "-f", "-e", exe] + [hex(a) for a in addrs],
                         capture_output=True, text=True).stdout.splitlines()
    chains, cur, lines = {}, None, []
    for line in out + ["0x0"]:
        if line.startswith("0x"):
            if cur is not None:
                chains[cur] = list(zip(lines[0::2], lines[1::2]))
            cur, lines = int(line, 16), []
        else:
            lines.append(line)
    return chains


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("data")
    ap.add_argument("exe")
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--chain", action="store_true")
    ap.add_argument("--callers")
    a = ap.parse_args()

    name = a.exe.split("/")[-1]
    syms = load_symbols(a.exe)
    recs = load_records(a.data)
    hits, other = collections.Counter(), collections.Counter()
    for r in recs:
        if a.callers:
            for i, fr in enumerate(r):
                addr = frame_addr(fr, name, syms)
                if addr is not None:
                    if any(f.startswith(a.callers + "+") for f in r[:i]):
                        hits[addr - 1] += 1  # a return address: the call is before it
                    break
            continue
        r = [f for f in r if not f.endswith("(inlined)")]
        if not r:
            continue
        addr = frame_addr(r[0], name, syms)
        if addr is None:
            other[r[0].rpartition(" (")[2].rstrip(")").split("/")[-1] or "?"] += 1
        else:
            hits[addr] += 1

    chains = inline_chains(a.exe, sorted(hits))
    agg = collections.Counter()
    for addr, n in hits.items():
        ch = chains.get(addr) or [("?", "?")]
        if a.chain or a.callers:
            key = " < ".join(f for f, _ in ch[:4 if a.chain else 3])
            if a.callers:
                key += "  @" + ch[0][1].split("/")[-1]
        else:
            key = ch[0][0]
        agg[key] += n

    total = len(recs)
    print(f"{total} samples")
    for key, n in agg.most_common(a.top):
        print(f"{100 * n / total:6.2f}%  {key}")
    if other and not a.callers:
        print("-- outside the executable:")
        for key, n in other.most_common(6):
            print(f"{100 * n / total:6.2f}%  {key}")


if __name__ == "__main__":
    main()
