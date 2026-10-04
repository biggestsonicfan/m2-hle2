#!/usr/bin/env python3
"""The i960 program ROM compiled ahead of time to C (src/core/i960_aot.h).

    i960_aot.py --map MAP --rom CODE.BIN [--hook ADDR ...] [--cover F] --out GEN.h

MAP is the code a game runs, as `det_digest --aot-map` writes it: a line per
instruction, `ip w1 w2 count jumped` (or `ip weight jumped`: the words come
from the ROM), and `hook ADDR` per hook of the profile. --cover F compiles only
the hottest instructions, as many as make up F of the counts (0.995: a third
of STF's code, 1.5 MB of SH-4 where all of it is 4.8, and the Dreamcast has
16 MB); the rest stays with the interpreter. CODE.BIN is the program ROM
as the i960 sees it from address 0 (the first 1 MB). The output holds ROM
words, so it is a build product and is never committed.

Each 2^shift bytes of code become one C function: a switch over the blocks
that start there, then the blocks. The common instructions (moves, ALU,
compares, branches, loads and stores in plain memory) are written out as C
with their operands decoded here, the way i960_exec.h runs them; the rest call
the interpreter's i960_exec_word out of line. Inlining the interpreter at every
instruction instead took a compiler over 6 GB and ten minutes.
"""
import argparse, os, re, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
EXEC_H = os.path.join(HERE, '..', 'src', 'board', 'i960_exec.h')

def cycle_tables(path):
    """g_i960_cyc / g_i960_cyc_reg, read out of i960_cycle_table_init."""
    src = open(path).read()
    body = src[src.index('i960_cycle_table_init(void)'):src.index('i960_cycle_cost(uint32_t')]
    ops = body[body.index('ops[] = {'):body.index('};', body.index('ops[] = {'))]
    reg = body[body.index('reg[] = {'):body.index('};', body.index('reg[] = {'))]
    cyc = [1] * 256
    for op, c in re.findall(r'\{\s*(0x[0-9A-Fa-f]+)\s*,\s*(\d+)\s*\}', ops):
        cyc[int(op, 16)] = int(c)
    creg = [1] * (40 * 16)
    for op, fn, c in re.findall(r'\{\s*(0x[0-9A-Fa-f]+)\s*,\s*(0x[0-9A-Fa-f]+)\s*,\s*(\d+)\s*\}', reg):
        creg[((int(op, 16) - 0x58) << 4) | int(fn, 16)] = int(c)
    def cost(w1):
        op = w1 >> 24
        if 0x58 <= op < 0x80:
            return creg[((op - 0x58) << 4) | ((w1 >> 7) & 0xF)]
        return cyc[op]
    return cost

def sext(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v

# REG opcodes the interpreter runs on the CPU alone. Anything else (synmov to
# the IAC, modpc, intdis/inten, flushreg, syncf, modtc, ldtime, unknown) is
# synced first and may stop the run.
REG_PURE = {
    0x580, 0x581, 0x582, 0x583, 0x584, 0x586, 0x587, 0x588, 0x589, 0x58a, 0x58b, 0x58c,
    0x58d, 0x58e, 0x58f, 0x590, 0x591, 0x592, 0x593, 0x598, 0x59a, 0x59b, 0x59c, 0x59d,
    0x59e, 0x5a0, 0x5a1, 0x5a2, 0x5a3, 0x5a4, 0x5a5, 0x5a6, 0x5a7, 0x5ac, 0x5ad, 0x5ae,
    0x5b0, 0x5b2, 0x5cc, 0x5dc, 0x5ec, 0x5fc, 0x640, 0x641, 0x645, 0x650, 0x651, 0x670,
    0x671, 0x674, 0x675, 0x6c0, 0x6c1, 0x6c2, 0x6c3, 0x6c9, 0x684, 0x685, 0x688, 0x701,
    0x708, 0x70b, 0x741, 0x748, 0x749, 0x74b, 0x78b, 0x78c, 0x78d, 0x78f,
}
LOADS = {0x80: 1, 0x88: 2, 0x90: 4, 0xC0: 1, 0xC8: 2}
STORES = {0x82: 1, 0x8A: 2, 0x92: 4, 0xC2: 1, 0xCA: 2}
LOADN = {0x98: 2, 0xA0: 3, 0xB0: 4}
STOREN = {0x9A: 2, 0xA2: 3, 0xB2: 4}

def ilen(w1):
    op = w1 >> 24
    if 0x80 <= op < 0xD0 and ((w1 >> 10) & 0xF) in (5, 0xC, 0xD, 0xE, 0xF):
        return 8
    return 4

def classify(ip, w1):
    """(kind, targets): kind is pure, br (targets: taken, and fall-through if
    conditional), call, ind (target known only at run time), ld/st/ldn/stn
    (size), slow."""
    op = w1 >> 24
    if op < 0x20:
        disp = sext(w1 & 0x00FFFFFC, 24)
        t = (ip + disp) & 0xFFFFFFFF
        if op in (0x08, 0x0B): return 'br', [t]
        if op == 0x09:         return 'call', [t]
        if op == 0x0A:         return 'ind', []
        if 0x10 <= op <= 0x17: return 'br', [t, ip + 4]
        return 'slow', []
    if op < 0x40:
        disp = sext(w1 & 0x1FFC, 13)
        t = (ip + disp) & 0xFFFFFFFF
        if 0x20 <= op <= 0x27: return 'pure', []
        if op == 0x38:         return 'pure', []
        if op == 0x3F:         return 'br', [t]
        if op >= 0x30:         return 'br', [t, ip + 4]
        return 'slow', []
    if 0x58 <= op < 0x80:
        opc = ((w1 >> 20) & 0xFF0) | ((w1 >> 7) & 0xF)
        return ('pure' if opc in REG_PURE else 'slow'), []
    if 0x80 <= op < 0xD0:
        if op == 0x8C: return 'pure', []
        if op in LOADS:  return 'ld', [LOADS[op]]
        if op in STORES: return 'st', [STORES[op]]
        if op in LOADN:  return 'ldn', [LOADN[op] * 4]
        if op in STOREN: return 'stn', [STOREN[op] * 4]
        if op in (0x84, 0x85, 0x86): return 'ind', []
        return 'slow', []
    return 'slow', []

def R(i):
    """Register i (0-15 r, 16-31 g) as the array reg_read indexes."""
    return f'AR({(i + 16) & 31})'

def S(i, m):
    return f'{i}u' if m else R(i)

def cond(mask, cc):
    return f'({cc} & {mask}u)' if mask else f'({cc} == 0u)'

def mem_ea_c(ip, w1, w2):
    """mem_ea as a C expression, or None for a mode the generator leaves to the
    interpreter."""
    mode, ab, x = (w1 >> 10) & 0xF, (w1 >> 14) & 0x1F, w1 & 0x1F
    sc = [1, 2, 4, 8, 16][(w1 >> 7) & 7] if ((w1 >> 7) & 7) <= 4 else 1
    if not mode & 4:
        off = w1 & 0xFFF
        return f'({R(ab)} + 0x{off:X}u)' if mode & 8 else f'0x{off:X}u'
    idx = f'{R(x)} * {sc}u'
    return {4: f'{R(ab)}', 5: f'0x{(ip + 8 + w2) & 0xFFFFFFFF:X}u', 7: f'({R(ab)} + {idx})',
            0xC: f'0x{w2:X}u', 0xD: f'(0x{w2:X}u + {R(ab)})', 0xE: f'(0x{w2:X}u + {idx})',
            0xF: f'(0x{w2:X}u + {R(ab)} + {idx})'}.get(mode)

# REG ops written out: opcode -> C of the new dst value (s1, s2, d: operands).
REG_C = {
    0x581: '{s1} & {s2}', 0x587: '{s1} | {s2}', 0x586: '{s1} ^ {s2}', 0x58a: '~{s1}',
    0x590: '{s2} + {s1}', 0x592: '{s2} - {s1}', 0x591: '{s2} + {s1}', 0x593: '{s2} - {s1}',
    0x598: '({s1} < 32u ? {s2} >> {s1} : 0u)',
    0x59b: '({s1} < 32u ? (uint32_t)((int32_t){s2} >> {s1}) : ((int32_t){s2} < 0 ? 0xFFFFFFFFu : 0u))',
    0x59c: '({s1} < 32u ? {s2} << {s1} : 0u)', 0x59e: '({s1} < 32u ? {s2} << {s1} : 0u)',
    0x580: '{s2} ^ (1u << ({s1} & 31u))', 0x583: '{s2} | (1u << ({s1} & 31u))',
    0x584: '{s1} & ~{s2}', 0x582: '~{s1} & {s2}', 0x588: '~({s1} | {s2})', 0x589: '~({s1} ^ {s2})',
    0x58b: '{s2} | ~{s1}', 0x58c: '{s2} & ~(1u << ({s1} & 31u))', 0x58d: '{s1} | ~{s2}',
    0x58e: '~({s1} & {s2})', 0x701: '{s2} * {s1}', 0x741: '{s2} * {s1}', 0x5cc: '{s1}',
}

def fsrc(i, m):
    """A real operand as i960_exec.h's FP_SRC reads it: a register's bits as a
    single, or (m set) fp0-fp3 and the literals 0.0 and 1.0."""
    if not m: return f'i960_single_to_double({R(i)})'
    return {16: '0.0', 22: '1.0'}.get(i, f'cpu->fp_regs[{i & 3}]')

def fdst(d, m, v):
    return f'cpu->fp_regs[{d & 3}] = {v};' if m else f'{R(d)} = i960_double_to_single({v});'

# The real ops as i960_exec.h runs them: (a_, b_) = (src1, src2), dst value.
FP_C = {
    0x78f: 'i960_nan_result(a_ + b_, a_, b_)', 0x78d: 'i960_nan_result(b_ - a_, b_, a_)',
    0x78c: 'i960_nan_result(a_ * b_, a_, b_)',
    0x78b: '(a_ != 0.0 ? i960_nan_result(b_ / a_, b_, a_) : 0.0)',
}

def direct(ip, w1, w2, kind):
    """C for a pure, ld, st, ldn or stn instruction, or None."""
    op = w1 >> 24
    if 0x58 <= op < 0x80:
        opc = ((w1 >> 20) & 0xFF0) | ((w1 >> 7) & 0xF)
        s1i, s2i, d = w1 & 0x1F, (w1 >> 14) & 0x1F, (w1 >> 19) & 0x1F
        s1, s2 = S(s1i, (w1 >> 11) & 1), S(s2i, (w1 >> 12) & 1)
        if opc in REG_C:
            return f'{R(d)} = {REG_C[opc].format(s1=s1, s2=s2)};'
        if opc in (0x5dc, 0x5ec, 0x5fc):
            n = {0x5dc: 2, 0x5ec: 3, 0x5fc: 4}[opc]
            if d + n > 32 or (not (w1 >> 11) & 1 and s1i + n > 32): return None
            src = [s1 if (w1 >> 11) & 1 else R(s1i + k) for k in range(n)]
            return ('{ ' + ' '.join(f'uint32_t v{k}_ = {src[k]};' for k in range(n)) + ' '
                    + ' '.join(f'{R(d + k)} = v{k}_;' for k in range(n)) + ' }')
        if opc in (0x5a0, 0x5a1):
            f = 'AOT_CC_O' if opc == 0x5a0 else 'AOT_CC_I'
            t = '' if opc == 0x5a0 else '(int32_t)'
            return f'ACC({f}({t}{s1}, {t}{s2}));'
        if opc in (0x5a4, 0x5a5, 0x5a6, 0x5a7):
            f = 'AOT_CC_O' if opc in (0x5a4, 0x5a6) else 'AOT_CC_I'
            t = '' if opc in (0x5a4, 0x5a6) else '(int32_t)'
            pm = '+' if opc in (0x5a4, 0x5a5) else '-'
            return f'{{ uint32_t b_ = {s2}; ACC({f}({t}{s1}, {t}b_)); {R(d)} = b_ {pm} 1u; }}'
        if opc in (0x5a2, 0x5a3):
            t = '' if opc == 0x5a2 else '(int32_t)'
            return f'if (!(AGETCC & CC_L)) ACC({t}{s1} <= {t}{s2} ? CC_E : CC_G);'
        if opc == 0x5ae:
            return f'ACC(({s2} & (1u << ({s1} & 31u))) ? CC_E : CC_NO);'
        m1, m2, m3 = (w1 >> 11) & 1, (w1 >> 12) & 1, (w1 >> 13) & 1
        if opc in FP_C:
            return (f'{{ double a_ = {fsrc(s1i, m1)}, b_ = {fsrc(s2i, m2)}; '
                    + fdst(d, m3, FP_C[opc]) + ' }')
        if opc in (0x684, 0x685):
            return (f'{{ double a_ = {fsrc(s1i, m1)}, b_ = {fsrc(s2i, m2)}; '
                    'ACC(a_ < b_ ? CC_L : a_ == b_ ? CC_E : CC_G); }')
        if opc == 0x6c9:
            return '{ double a_ = ' + fsrc(s1i, m1) + '; ' + fdst(d, m3, 'a_') + ' }'
        if opc in (0x6c0, 0x6c1):
            return f'{R(d)} = i960_real_to_int32(i960_round_ac(cpu, {fsrc(s1i, m1)}));'
        if opc in (0x6c2, 0x6c3):
            return f'{R(d)} = i960_real_to_int32({fsrc(s1i, m1)});'
        if opc in (0x674, 0x675):
            i = f'(int32_t)i960_real_to_int32(cpu->fp_regs[{s1i & 3}])' if m1 else f'(int32_t){R(s1i)}'
            return '{ double a_ = (double)' + i + '; ' + fdst(d, m3, 'a_') + ' }'
        if opc == 0x641:
            return (f'{{ uint32_t v_ = {s1}; if (v_) {{ {R(d)} = 31u - (uint32_t)__builtin_clz(v_); ACC(CC_E); }}'
                    f' else {{ {R(d)} = 0xFFFFFFFFu; ACC(CC_NO); }} }}')
        if opc == 0x5b0:
            return (f'{{ uint64_t r_ = (uint64_t){s2} + (uint64_t){s1} + ((AGETCC & 2u) ? 1u : 0u); '
                    f'{R(d)} = (uint32_t)r_; ACC((r_ >> 32) ? CC_E : CC_NO); }}')
        if opc == 0x58f:
            return (f'{R(d)} = (AGETCC & CC_E) ? ({s2} | (1u << ({s1} & 31u)))'
                    f' : ({s2} & ~(1u << ({s1} & 31u)));')
        return None
    if 0x20 <= op <= 0x27:
        return f'{R((w1 >> 19) & 0x1F)} = {cond(op & 7, "AGETCC")} ? 1u : 0u;'
    if op == 0x38:
        return ';'
    if 0x80 <= op < 0xD0:
        ea = mem_ea_c(ip, w1, w2)
        if ea is None: return None
        d = (w1 >> 19) & 0x1F
        if op == 0x8C: return f'{R(d)} = {ea};'
        sz = {0x80: 1, 0x88: 2, 0x90: 4, 0xC0: 1, 0xC8: 2, 0x82: 1, 0x8A: 2, 0x92: 4, 0xC2: 1, 0xCA: 2}
        # inline only the RAM (AOT_RAM); the helpers take the other pages
        rd = {0x80: 'AOT_RL8(0)', 0x88: 'AOT_RL16(0)', 0x90: 'AOT_RL32(0)',
              0xC0: '(uint32_t)(int32_t)(int8_t)AOT_RL8(0)', 0xC8: '(uint32_t)(int32_t)(int16_t)AOT_RL16(0)'}
        wr = {0x82: 'AOT_RS8', 0x8A: 'AOT_RS16', 0x92: 'AOT_RS32', 0xC2: 'AOT_RS8', 0xCA: 'AOT_RS16'}
        # a load off the ROMs (AOT_ROMD): the same, through q_
        if op in rd: return (ea, f'AOT_RAM({sz[op]}u)', f'{R(d)} = {rd[op]};',
                             f'AOT_ROMD({sz[op]}u)', f'{R(d)} = {rd[op].replace("AOT_RL", "AOT_OL")};')
        if op in wr:
            last = f'g_last_store_ip = 0x{ip:X}u; ' if op in (0x82, 0x8A, 0x92) else ''
            return ea, f'AOT_RAM({sz[op]}u)', f'{last}{wr[op]}(0x{ip:X}u, 0, {R(d)});'
        if op in LOADN:
            n = LOADN[op]
            if d + n > 32: return None
            return (ea, f'AOT_RAM({4 * n}u)', ' '.join(f'{R(d + k)} = AOT_RL32({4 * k}u);' for k in range(n)),
                    f'AOT_ROMD({4 * n}u)', ' '.join(f'{R(d + k)} = AOT_OL32({4 * k}u);' for k in range(n)))
        if op in STOREN:
            n = STOREN[op]
            if d + n > 32: return None
            return ea, f'AOT_RAM({4 * n}u)', (f'g_last_store_ip = 0x{ip:X}u; '
                        + ' '.join(f'AOT_RS32(0x{ip:X}u, {4 * k}u, {R(d + k)});' for k in range(n)))
    return None

def branch_c(ip, w1):
    """(C condition or None for always, taken target, link C or '') of a br."""
    op = w1 >> 24
    if op < 0x20:
        t = (ip + sext(w1 & 0x00FFFFFC, 24)) & 0xFFFFFFFF
        if op == 0x08: return None, t, ''
        if op == 0x0B: return None, t, f'{R(16 + 14)} = 0x{ip + 4:X}u;'
        return cond(op & 7, 'AGETCC'), t, ''
    t = (ip + sext(w1 & 0x1FFC, 13)) & 0xFFFFFFFF
    if op == 0x3F: return None, t, ''
    s1i, s2i = (w1 >> 19) & 0x1F, (w1 >> 14) & 0x1F
    s1, s2 = S(s1i, (w1 >> 13) & 1), R(s2i)
    if op in (0x30, 0x37):
        bit = f'({s2} & (1u << ({s1} & 31u)))'
        c = f'!{bit}' if op == 0x30 else bit
        return f'AOT_CCB({c})', t, ''
    f = 'AOT_CMPB_O' if op < 0x38 else 'AOT_CMPB_I'
    return f'{f}({s1}, {s2}, {op & 7}u)', t, ''

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--map', required=True)
    ap.add_argument('--rom', required=True, help='program ROM from i960 address 0')
    ap.add_argument('--hook', action='append', default=[], help='an address never to compile')
    ap.add_argument('--shift', type=int, default=12)
    ap.add_argument('--limit', type=lambda s: int(s, 0), default=0x100000)
    ap.add_argument('--cover', type=float, default=1.0, help='compile the hottest F of the counts')
    ap.add_argument('--out', required=True)
    a = ap.parse_args()
    cost = cycle_tables(EXEC_H)

    rom = open(a.rom, 'rb').read()[:a.limit]
    def word(addr):
        return struct.unpack_from('<I', rom, addr)[0] if addr + 4 <= len(rom) else 0

    ips, jumped, hooks = set(), set(), set(int(h, 0) for h in a.hook)
    hits = {}
    bad = 0
    for line in open(a.map):
        f = line.split()
        if not f or f[0].startswith('#'): continue
        if f[0] == 'hook':
            hooks.add(int(f[1], 16)); continue
        ip = int(f[0], 16)
        if ip >= a.limit or ip & 3: continue
        if len(f) >= 5:
            if int(f[1], 16) != word(ip): bad += 1
            hits[ip] = hits.get(ip, 0) + int(f[3])
            jumped_flag = f[4]
        else:
            hits[ip] = hits.get(ip, 0) + int(f[1])
            jumped_flag = f[2]
        ips.add(ip)
        if jumped_flag != '0': jumped.add(ip)
    if bad:
        sys.exit(f'i960_aot: {bad} words of the map differ from {a.rom}: not the ROM it was made on')
    ips -= hooks
    if a.cover < 1.0:
        # the hottest first, until they make up the share asked for
        total, run, keep = sum(hits[ip] for ip in ips), 0, set()
        for ip in sorted(ips, key=lambda x: (-hits[x], x)):
            if run >= a.cover * total: break
            keep.add(ip); run += hits[ip]
        ips = keep

    info = {}
    for ip in ips:
        w1 = word(ip)
        info[ip] = (w1, word(ip + 4), ilen(w1)) + classify(ip, w1)
    # Leaders: where control arrives other than from the instruction before.
    lead = set(ip for ip in jumped if ip in ips)
    for ip, (w1, w2, n, kind, tg) in info.items():
        if kind in ('br', 'call'):
            for t in tg: lead.add(t)
        if kind in ('call', 'ind', 'slow', 'br'):
            lead.add(ip + n)
    for ip, (w1, w2, n, kind, tg) in info.items():
        if ip + n in ips and (ip + n) >> a.shift != ip >> a.shift: lead.add(ip + n)
    for h in hooks: lead.add(h + 4)
    for ip in ips:
        # first of a run: nothing compiled falls into it
        pass
    prev_of = {}
    for ip, (w1, w2, n, kind, tg) in info.items():
        prev_of[ip + n] = ip
    for ip in ips:
        if ip not in prev_of: lead.add(ip)
    lead &= ips
    # Where a block has to start: after a transfer or a slow op, after a gap,
    # at a chunk's edge, after a hook. Any other leader (a branch's target, an
    # address the interpreter arrived at) is entered mid-block, by a stub that
    # checks the room left to the block's end: the code that falls into it
    # pays no check.
    split = set()
    for ip, (w1, w2, n, kind, tg) in info.items():
        if kind in ('call', 'ind', 'slow', 'br'): split.add(ip + n)
        if ip + n in ips and (ip + n) >> a.shift != ip >> a.shift: split.add(ip + n)
        if ip not in prev_of: split.add(ip)
    for h in hooks: split.add(h + 4)
    split &= ips

    chunks = {}
    for ip in sorted(ips):
        chunks.setdefault(ip >> a.shift, []).append(ip)

    out = []
    w = out.append
    w('/* Generated by tools/i960_aot.py from the program ROM: do not commit. */')
    w(f'#define AOT_SHIFT {a.shift}')
    nchunks = (a.limit >> a.shift)
    w(f'#define AOT_NCHUNKS {nchunks}u')
    hl = sorted(hooks)
    w(f'#define AOT_NHOOKS {len(hl)}u')
    w('static const uint32_t s_aot_hooks[] = {' + ', '.join('0x%08Xu' % h for h in hl) + '};')
    # The ROM it was compiled from: an FNV of the whole image, and 256 words to
    # sample a slice at a time.
    h = 2166136261
    for i in range(0, len(rom) // 4 * 4, 4):
        h = ((h ^ struct.unpack_from('<I', rom, i)[0]) * 16777619) & 0xFFFFFFFF
    w(f'#define AOT_ROM_BYTES {len(rom) // 4 * 4}u')
    w(f'#define AOT_FNV 0x{h:08X}u')
    srt = sorted(ips)
    samp = [srt[(i * len(srt)) // 256] for i in range(256)]
    w('static const uint32_t s_aot_samp[256][2] = {' + ', '.join('{0x%Xu,0x%08Xu}' % (s, word(s)) for s in samp) + '};')
    # Leaders, a bit each: the run loop asks only at one.
    bits = bytearray(a.limit // 32)
    for ip in lead: bits[ip >> 5] |= 1 << ((ip >> 2) & 7)
    w('static const uint8_t s_aot_lead[%d] = {' % len(bits))
    for i in range(0, len(bits), 64):
        w(','.join(str(b) for b in bits[i:i + 64]) + ',')
    w('};')

    nblocks = ndirect = 0
    for c, cips in sorted(chunks.items()):
        body, K = [], []
        def op(x, w1, w2, k2, nx):
            assert nx < 1024 and k2 < (1 << 22)
            K.append(f'{{0x{x:X}u,0x{w1:08X}u,0x{w2:08X}u,{(k2 << 10) | nx}u}}')
            return len(K) - 1
        b_ = body.append
        b_(f'static int aot_c{c:04x}(i960_cpu_t *cpu, memory_bus_t *bus, aot_state_t *s) {{')
        b_('    uint32_t ip_ = s->ip; uint8_t *p_; (void)p_; AOT_REGS;')
        b_('dispatch: __attribute__((unused));')
        b_('    switch (ip_) {')
        for ip in cips:
            if ip in lead: b_(f'    case 0x{ip:X}u: goto L{ip:x};')
        b_('    default: AOT_OUT(ip_, AOT_STOP);')
        b_('    }')
        def jump(t):
            if t in lead and t >> a.shift == c: return f'goto L{t:x};'
            if t in lead: return f'AOT_OUT(0x{t:X}u, AOT_GO);'
            return f'AOT_OUT(0x{t:X}u, AOT_STOP);'
        redispatch = f'if ((ip_ >> AOT_SHIFT) == 0x{c:X}u) goto dispatch; AOT_OUT(ip_, AOT_GO);'
        i = 0
        while i < len(cips):
            ip = cips[i]
            if ip not in lead: i += 1; continue
            # the block: on until a leader, a gap, or a transfer
            blk = [ip]
            while True:
                w1, w2, n, kind, tg = info[blk[-1]]
                nx = blk[-1] + n
                if kind in ('br', 'call', 'ind') or nx not in ips or nx in split: break
                blk.append(nx)
            costs = [cost(info[x][0]) for x in blk]
            cb, nb = sum(costs), len(blk)
            nblocks += 1
            b_(f'L{ip:x}: AOT_LEAD(0x{ip:X}u, {nb}, {cb});')
            stubs = []
            for j, x in enumerate(blk):
                w1, w2, n, kind, tg = info[x]
                if j and x in lead:
                    b_(f'M{x:x}:')
                    stubs.append(f'L{x:x}: AOT_LEAD(0x{x:X}u, {nb - j}, {sum(costs[j:])}); goto M{x:x};')
                k2, nx_left = sum(costs[j + 1:]), nb - j - 1
                dc = direct(x, w1, w2, kind) if kind in ('pure', 'ld', 'st', 'ldn', 'stn') else None
                if kind == 'br':
                    c_, t_, link = branch_c(x, w1)
                    ndirect += 1
                    if link: b_(f'    {link}')
                    if c_ is None:
                        b_(f'    {jump(t_)}')
                    else:
                        b_(f'    if ({c_}) {jump(t_)}')
                        b_(f'    {jump(tg[1])}')
                    continue
                if kind == 'pure' and dc is not None:
                    ndirect += 1
                    b_(f'    {dc}')
                    continue
                k = op(x, w1, w2, k2, nx_left)
                if dc is not None:
                    ndirect += 1
                    ea, test, code = dc[:3]
                    slow = 'AOT_IO' if kind in ('ld', 'st') else 'AOT_IOSTN' if kind == 'stn' else 'AOT_LDN'
                    if len(dc) > 3:
                        b_(f'    {{ uint32_t ea_ = {ea}; const uint8_t *q_; if (M2_LIKELY({test})) {{ {code} }}'
                           f' else if ({dc[3]}) {{ {dc[4]} }} else {slow}({k}); }}')
                    else:
                        b_(f'    {{ uint32_t ea_ = {ea}; if (M2_LIKELY({test})) {{ {code} }} else {slow}({k}); }}')
                elif kind in ('ld', 'st', 'ldn', 'stn'):
                    b_(f'    AOT_SLOW({k});')
                elif kind == 'slow':
                    b_(f'    AOT_SLOW({k});')
                    b_(f'    if (cpu->sfr.ip != 0x{x + n:X}u) {{ s->rn += {nx_left}; s->rc += {k2}; ip_ = cpu->sfr.ip; {redispatch} }}')
                elif kind == 'call':
                    b_(f'    AOT_CALL({k});')
                elif w1 >> 24 == 0x0A:
                    b_(f'    AOT_RET({k});')
                else:
                    b_(f'    AOT_X({k});')
                if kind == 'call':
                    b_(f'    {jump(tg[0])}')
                elif kind == 'ind':
                    b_(f'    ip_ = cpu->sfr.ip; {redispatch}')
            last = blk[-1]
            if info[last][3] not in ('br', 'call', 'ind'):
                b_(f'    {jump(last + info[last][2])}')
            for t in stubs: b_(t)
            i = cips.index(blk[-1]) + 1
        b_('}')
        if K:
            w(f'static const aot_op_t K_c{c:04x}[] = {{' + ','.join(K) + '};')
            w(f'#define K K_c{c:04x}')
        out.extend(body)
        if K: w('#undef K')
    w('static const aot_chunk_fn s_aot_chunk[AOT_NCHUNKS] = {')
    for c in sorted(chunks):
        w(f'    [0x{c:X}] = aot_c{c:04x},')
    w('};')
    open(a.out, 'w').write('\n'.join(out) + '\n')
    print(f'i960_aot: {len(ips)} instructions ({ndirect} as C), {len(lead)} leaders, {nblocks} blocks, '
          f'{len(chunks)} chunks, {len(hooks)} hooks', file=sys.stderr)

if __name__ == '__main__':
    main()
