#!/usr/bin/env python3
"""perf_inline.py -- a perf profile of m2hle by INLINED function.

The board is header-only and built at -O3, so nearly everything is inlined into
a handful of real functions: `perf report` puts ~30% of a run on
emu_slice_body, which is the i960, its bus, the timers and the HLE hook check
all at once. This charges each sample to the innermost inlined function at its
address instead (addr2line -i), which is what a reader of the source means by
"function". See PERF-PROFILE.md.

    perf record -F 2000 --call-graph fp -o run.data -- <exe> ...
    python3 tools/perf_inline.py run.data <exe> [--top N] [--chain] [--callers SYM]
                                 [--comm NAME]

  --chain        key each sample by its inline chain (innermost first, 4 deep)
                 instead of the innermost function alone
  --callers SYM  only samples whose leaf is SYM (a libc memcpy, say), charged
                 to the first frame of <exe> above it. With frame pointers a
                 leaf that sets up no frame hides its direct caller, so record
                 with --call-graph dwarf for this, or use
                 `perf report --inline -G -S SYM` on a dwarf recording.
  --comm NAME    only one thread's samples, by its name (m2-sound is the
                 sound board's; the main thread has the executable's name);
                 percentages are then of that thread

Percentages are of every sample's period (cycles) in the recording, all
threads, unless --comm. Before #1713 they were of sample counts, which put the
sound thread at about twice its share under `perf record -F`.
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


def load_records(data, comm=None):
    """Each sample's (period, frames), frames leaf first as 'sym+0xoff (dso)'.

    The period is what the sample stands for (cycles, for the default event).
    Under `perf record -F` it varies by thread: a thread that sleeps between
    bursts (the sound thread) gets samples worth about half a busy thread's, so
    counting samples would overweight it about 2x. Everything is weighted by it.
    """
    cmd = ["perf", "script", "-i", data, "-F", "period,ip,sym,symoff,dso"]
    if comm:
        cmd += ["--comms", comm]
    out = subprocess.run(cmd, capture_output=True, text=True).stdout
    recs, period, cur = [], 0, []
    for raw in out.split("\n"):
        line = raw.strip()
        if not line:
            if cur:
                recs.append((period, cur))
                cur = []
            continue
        if not raw.startswith("\t"):  # the sample's header: its period
            if cur:
                recs.append((period, cur))
                cur = []
            head = line.split(None, 2)
            period = int(head[0])
            if len(head) == 3:  # recorded without a call graph: the frame is here
                recs.append((period, [head[2]]))
            continue
        frame = line.split(None, 1)[1] if " " in line else line
        # The kernel's frames under a user-space-only event go. A dwarf unwind
        # adds a line per inlined level, a libc memcpy's included; they stay,
        # marked, for --callers to match on.
        if frame != "[unknown] ([unknown])":
            cur.append(frame)
    if cur:
        recs.append((period, cur))
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


def caller_hit(r, name, syms, callers):
    """The call site in r's first frame inside the executable, if a frame
    above it is callers; None otherwise."""
    for i, fr in enumerate(r):
        addr = frame_addr(fr, name, syms)
        if addr is not None:
            if any(f.startswith(callers + "+") for f in r[:i]):
                return addr - 1  # a return address: the call is before it
            return None
    return None


def count_hits(recs, name, syms, callers):
    """Weight (period) per address in the executable, and per object outside it."""
    hits, other = collections.Counter(), collections.Counter()
    for w, r in recs:
        if callers:
            addr = caller_hit(r, name, syms, callers)
            if addr is not None:
                hits[addr] += w
            continue
        r = [f for f in r if not f.endswith("(inlined)")]
        if not r:
            continue
        addr = frame_addr(r[0], name, syms)
        if addr is None:
            other[r[0].rpartition(" (")[2].rstrip(")").split("/")[-1] or "?"] += w
        else:
            hits[addr] += w
    return hits, other


def chain_key(ch, a):
    """The line an address's inline chain is summed under."""
    if not (a.chain or a.callers):
        return ch[0][0]
    key = " < ".join(f for f, _ in ch[:4 if a.chain else 3])
    if a.callers:
        key += "  @" + ch[0][1].split("/")[-1]
    return key


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("data")
    ap.add_argument("exe")
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--chain", action="store_true")
    ap.add_argument("--callers")
    ap.add_argument("--comm")
    a = ap.parse_args()

    name = a.exe.split("/")[-1]
    syms = load_symbols(a.exe)
    recs = load_records(a.data, a.comm)
    hits, other = count_hits(recs, name, syms, a.callers)

    chains = inline_chains(a.exe, sorted(hits))
    agg = collections.Counter()
    for addr, n in hits.items():
        agg[chain_key(chains.get(addr) or [("?", "?")], a)] += n

    total = sum(w for w, _ in recs) or 1
    print(f"{len(recs)} samples, weighted by period")
    for key, n in agg.most_common(a.top):
        print(f"{100 * n / total:6.2f}%  {key}")
    if other and not a.callers:
        print("-- outside the executable:")
        for key, n in other.most_common(6):
            print(f"{100 * n / total:6.2f}%  {key}")


if __name__ == "__main__":
    main()
