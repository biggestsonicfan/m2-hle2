#!/usr/bin/env python3
"""Name the samples of an in-process host profile (src/core/host_prof.h).

The emulator writes the report itself, split by thread, zone and dynamic
symbol. Its own code has no dynamic symbols, and nearly all of it is inlined
into a few functions, so this resolves the raw PCs with `addr2line -i` against
an unstripped copy of the same build (built with -g) and prints, per zone:

  leaf       the innermost inlined function the sample was in
  inclusive  every function on the inline chain (and the out-of-line function
             around it), counted once per sample

    python3 tools/hostprof.py REPORT [--sym MODULE=PATH ...] [--zone Z] [--top N]

MODULE is a basename from the report's maps (m2hle, m2hle_libretro.so). By
default the report's own executable path is used when it exists here. PCs in
modules with no local file stay "module+offset". The ARM binary needs
aarch64-linux-gnu-addr2line (binutils-aarch64-linux-gnu), which is picked
from the ELF header.
"""
import argparse
import collections
import os
import struct
import subprocess
import sys


def parse_report(path):
    head, maps, raw, raw_lr = {}, [], [], []
    section = None
    with open(path) as f:
        for line in f:
            line = line.rstrip("\n")
            if line.startswith("## "):
                section = line[3:].split()[0]
                continue
            if not line or line.startswith("#"):
                continue
            if section is None:
                k, _, v = line.partition(" ")
                head[k] = v
            elif section == "maps":
                p = line.split()
                if len(p) >= 6:
                    lo, hi = (int(x, 16) for x in p[0].split("-"))
                    maps.append((lo, hi, int(p[2], 16), p[5]))
            elif section in ("raw", "raw-lr"):
                p = line.split()
                if len(p) == 4 and p[0].lstrip("-").isdigit():
                    (raw if section == "raw" else raw_lr).append((int(p[0]), p[1], int(p[2], 16), int(p[3])))
    return head, maps, raw, raw_lr


def elf_info(path):
    """(machine, [(p_offset, p_vaddr, p_filesz)] of PT_LOAD) for a 64-bit LE ELF."""
    with open(path, "rb") as f:
        eh = f.read(64)
        if eh[:4] != b"\x7fELF" or eh[4] != 2:
            return None, []
        machine = struct.unpack_from("<H", eh, 18)[0]
        phoff, = struct.unpack_from("<Q", eh, 32)
        phentsize, phnum = struct.unpack_from("<HH", eh, 54)
        loads = []
        for i in range(phnum):
            f.seek(phoff + i * phentsize)
            ph = f.read(56)
            p_type, _, p_offset, p_vaddr, _, p_filesz = struct.unpack_from("<IIQQQQ", ph)
            if p_type == 1:
                loads.append((p_offset, p_vaddr, p_filesz))
        return machine, loads


def resolve(binary, addrs):
    """{vaddr: [innermost function, ..., outermost]} via addr2line -i."""
    machine, _ = elf_info(binary)
    tool = "aarch64-linux-gnu-addr2line" if machine == 183 else "addr2line"
    out = {}
    addrs = sorted(set(addrs))
    for i in range(0, len(addrs), 4000):
        chunk = addrs[i:i + 4000]
        r = subprocess.run([tool, "-f", "-i", "-C", "-a", "-e", binary] + ["%x" % a for a in chunk],
                           capture_output=True, text=True, check=True)
        cur = None
        lines = r.stdout.splitlines()
        j = 0
        while j < len(lines):
            ln = lines[j]
            if ln.startswith("0x"):
                cur = int(ln, 16)
                out[cur] = []
                j += 1
                continue
            fn = ln
            loc = lines[j + 1] if j + 1 < len(lines) else ""
            j += 2
            if cur is not None:
                base = os.path.basename(loc.split(" ")[0]) if loc else ""
                out[cur].append(fn if fn != "??" else "?? (%s)" % base)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("report")
    ap.add_argument("--sym", action="append", default=[], help="MODULE=PATH of an unstripped binary")
    ap.add_argument("--zone", help="only this zone")
    ap.add_argument("--thread", type=int, help="only this tid")
    ap.add_argument("--top", type=int, default=25)
    a = ap.parse_args()

    head, maps, raw, raw_lr = parse_report(a.report)
    syms = {}
    exe = head.get("exe", "")
    if exe and os.path.exists(exe):
        syms[os.path.basename(exe)] = exe
    for s in a.sym:
        k, _, v = s.partition("=")
        syms[k] = v

    def locate(pc):
        for lo, hi, off, path in maps:
            if lo <= pc < hi:
                return os.path.basename(path), pc - lo + off
        return "[unknown]", pc

    # file offset -> vaddr per module with a local binary
    loads = {m: elf_info(p)[1] for m, p in syms.items()}

    def vaddr(mod, off):
        for p_off, p_va, sz in loads.get(mod, []):
            if p_off <= off < p_off + sz:
                return off - p_off + p_va
        return None

    rows = [r for r in raw if (not a.zone or r[1] == a.zone) and (a.thread is None or r[0] == a.thread)]
    total = sum(r[3] for r in rows) or 1
    want = collections.defaultdict(set)
    placed = []
    for tid, zone, pc, n in rows:
        mod, off = locate(pc)
        va = vaddr(mod, off) if mod in syms else None
        if va is not None:
            want[mod].add(va)
        placed.append((tid, zone, mod, off, va, n))
    chains = {m: resolve(syms[m], v) for m, v in want.items()}

    leaf = collections.defaultdict(collections.Counter)
    incl = collections.defaultdict(collections.Counter)
    zone_tot = collections.Counter()
    for tid, zone, mod, off, va, n in placed:
        zone_tot[zone] += n
        if va is not None and va in chains.get(mod, {}):
            ch = chains[mod][va] or ["??"]
        else:
            ch = ["%s+0x%x" % (mod, off & ~0xFFF) if mod not in syms else "%s+0x%x" % (mod, off)]
            if mod not in syms:
                ch = [mod]
        leaf[zone][ch[0]] += n
        for fn in set(ch):
            incl[zone][fn] += n

    print("%s: %s samples, %s s CPU over %s s wall, %s Hz" %
          (a.report, head.get("samples"), head.get("cpu_s"), head.get("wall_s"), head.get("hz")))
    for zone, zt in zone_tot.most_common():
        print("\n== %s: %d samples, %.1f%% of those shown" % (zone, zt, 100.0 * zt / total))
        print("   leaf%s inclusive" % (" " * 52))
        L = leaf[zone].most_common(a.top)
        I = incl[zone].most_common(a.top)
        for i in range(max(len(L), len(I))):
            l = "%5.1f%% %-50s" % (100.0 * L[i][1] / total, L[i][0][:50]) if i < len(L) else " " * 57
            r = "%5.1f%% %s" % (100.0 * I[i][1] / total, I[i][0][:60]) if i < len(I) else ""
            print("   %s  %s" % (l, r))

    # Who called into modules we cannot name (the GL driver, libc): their LR.
    ext = collections.Counter()
    for tid, zone, lr, n in raw_lr:
        if a.zone and zone != a.zone:
            continue
        mod, off = locate(lr)
        if mod in syms:
            va = vaddr(mod, off)
            if va is not None:
                ext[(mod, va)] += n
    if ext:
        ch = {}
        by_mod = collections.defaultdict(set)
        for m, va in ext:
            by_mod[m].add(va)
        for m, v in by_mod.items():
            for va, c in resolve(syms[m], v).items():
                ch[(m, va)] = c
        agg = collections.Counter()
        for k, n in ext.items():
            c = ch.get(k) or ["??"]
            agg[" < ".join(c[:3])] += n
        print("\n== link register inside our code (the caller of a leaf; for driver/libc samples)")
        for k, n in agg.most_common(a.top):
            print("   %5.1f%% %s" % (100.0 * n / total, k[:110]))


if __name__ == "__main__":
    sys.exit(main())
