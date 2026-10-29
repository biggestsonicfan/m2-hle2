#!/usr/bin/env python3
"""gen-scsp-dsp.py -- compile known SCSP DSP microprograms to straight-line C.

A Model 2 sound driver loads one DSP program at boot and keeps it: STF and
Fighting Vipers load the same 84-step reverb (Hiro's driver). scsp_dsp_step
interprets it every sample, and a straight-line compile of the same program
runs about twice as fast, because every field of every step is a constant the
compiler can fold. This writes src/board/scsp_dsp_known.h: one function per
program listed in tools/scsp-dsp-programs.txt, and the table scsp_dsp_decode
matches MPRO against. Any other program runs on the interpreter as before.

Each step is emitted with the semantics of the interpreter's general body
(SCSP_DK_ANY in scsp.h), field by field, so the output is the same bits by
construction; tests/scsp_dsp_test.c holds every compiled program against the
reference step over random chip state.

Usage:
  python tools/gen-scsp-dsp.py                     regenerate the header
  python tools/gen-scsp-dsp.py --add NAME DUMP     add a program: DUMP is MPRO as
                                                   512 little-endian u16 words
                                                   (1 KB), as a debugger dumps it
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROGRAMS = os.path.join(ROOT, 'tools', 'scsp-dsp-programs.txt')
OUT = os.path.join(ROOT, 'src', 'board', 'scsp_dsp_known.h')


def read_programs():
    progs, cur = [], None
    for line in open(PROGRAMS):
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        if line.startswith('program '):
            cur = {'name': line.split()[1], 'words': []}
            progs.append(cur)
        else:
            cur['words'] += [int(w, 16) for w in line.split()]
    for p in progs:
        assert len(p['words']) == 512, p['name']
    return progs


def last_step(words):
    """scsp_dsp_start: one past the last step with any bit set."""
    for i in range(127, -1, -1):
        if any(words[i * 4:i * 4 + 4]):
            return i + 1
    return 0


def fields(p):
    """One MPRO step's fields, by name."""
    return dict(
        tra=(p[0] >> 8) & 0x7F, twt=(p[0] >> 7) & 1, twa=p[0] & 0x7F,
        xsel=(p[1] >> 15) & 1, ysel=(p[1] >> 13) & 3, ira=(p[1] >> 6) & 0x3F,
        iwt=(p[1] >> 5) & 1, iwa=p[1] & 0x1F,
        table=(p[2] >> 15) & 1, mwt=(p[2] >> 14) & 1, mrd=(p[2] >> 13) & 1,
        ewt=(p[2] >> 12) & 1, ewa=(p[2] >> 8) & 0xF, adrl=(p[2] >> 7) & 1,
        frcl=(p[2] >> 6) & 1, sh=(p[2] >> 4) & 3, yrl=(p[2] >> 3) & 1,
        negb=(p[2] >> 2) & 1, zero=(p[2] >> 1) & 1, bsel=p[2] & 1,
        nofl=(p[3] >> 15) & 1, cra=(p[3] >> 9) & 0x3F, masa=(p[3] >> 2) & 0x1F,
        adreb=(p[3] >> 1) & 1, nxadr=p[3] & 1)


class Program:
    """What emit_step has seen so far of one program, so later steps can read
    this sample's TEMP and MEMS from locals rather than from the chip.

    The chip state the program leaves is the same: every TEMP write is stored
    as it is made, and every MEMS written goes back at the end. Nothing else
    sees the chip mid-sample (the DSP runs between two samples, on the sound
    thread), and a step only ever reads TEMP at the same offset (+ DEC) it was
    written at within the sample, which is the same word."""

    def __init__(self, steps):
        self.temp = {}       # TEMP offset -> the local that holds this sample's write
        self.coefs, self.madrs, self.mixs, self.exts = set(), set(), set(), set()
        # MEMS is 24-bit signed unless a NOFL read put a raw word << 8 there
        self.mems_sext = any(f['nofl'] and f['mrd'] and st & 1 for st, f in enumerate(steps))
        self.mems_used = sorted({f['ira'] for f in steps if f['ira'] <= 0x1F} |
                                {f['iwa'] for f in steps if f['iwt']})
        self.mems_written = sorted({f['iwa'] for f in steps if f['iwt']})


def emit_inputs(f, prog):
    s = []
    ira = f['ira']
    if ira <= 0x1F:
        s.append('inputs = %s;' % ('scsp_sext(m%d, 24)' % ira if prog.mems_sext else 'm%d' % ira))
    elif ira <= 0x2F:
        prog.mixs.add(ira - 0x20)
        s.append('inputs = mx%d;' % (ira - 0x20))
    else:
        prog.exts.add(ira - 0x30)
        s.append('inputs = ex%d;' % (ira - 0x30))
    if f['iwt']:
        if ira == f['iwa']:
            s.append('inputs = memval;')
        s.append('m%d = memval;' % f['iwa'])
    return s


def emit_mem(st, f, prog):
    a = 'ma%d' % f['masa'] if not f['table'] else 'mt%d' % f['masa']
    prog.madrs.add((f['masa'], bool(f['table'])))
    if f['adreb']:
        a += ' + (adrs & 0x0FFF)'
    if f['nxadr']:
        a += ' + 1u'
    s = ['addr = (((%s) & %s) + rbp) << 1;' % (a, '0xFFFFu' if f['table'] else 'rbl_mask')]
    if f['mrd']:
        s.append('memval = %s;' % ('scsp_dsp_ring_rd(ram, addr) << 8' if f['nofl']
                                   else 'scsp_dsp_unpack(scsp_dsp_ring_rd(ram, addr))'))
    if f['mwt']:
        s.append('scsp_dsp_ring_wr(ram, addr, %s);' % ('(uint16_t)(shifted >> 8)' if f['nofl']
                                                       else 'scsp_dsp_pack(shifted)'))
    return s


def emit_operands(f, prog):
    """tr, b, x and y for one step."""
    # TEMP holds only the shifter's 24-bit signed output, so it is read as it is
    s = ['tr = %s;' % prog.temp.get(f['tra'], 'temp[(%du + dec) & 0x7F]' % f['tra'])]
    if f['zero']:
        s.append('b = 0;')
    else:
        base = 'acc' if f['bsel'] else 'tr'
        s.append('b = %s;' % ('(int32_t)(0u - (uint32_t)%s)' % base if f['negb'] else base))
    s.append('x = %s;' % ('inputs' if f['xsel'] else 'tr'))
    if f['ysel'] == 1:
        prog.coefs.add(f['cra'])      # a 16-bit COEF >> 3 is already the 13-bit signed Y
        s.append('y = c%d;' % f['cra'])
    else:
        ysrc = ['frc', None, 'yreg', 'yreg'][f['ysel']]
        ysh = [0, 3, 11, 4][f['ysel']]
        ymask = 0x0FFF if f['ysel'] == 3 else 0x1FFF
        s.append('y = scsp_sext((%s >> %d) & 0x%X, 13);' % (ysrc, ysh, ymask))
    if f['yrl']:
        s.append('yreg = inputs;')
    return s


def emit_shift(sh):
    """The shifter: SHFT 0 and 1 saturate, 2 and 3 wrap at 24 bits."""
    if sh == 0:
        return 'shifted = acc > 0x7FFFFF ? 0x7FFFFF : acc < -0x800000 ? -0x800000 : acc;'
    if sh == 1:
        return ('v = (int64_t)((uint64_t)(int64_t)acc << 1); '
                'shifted = (int32_t)(v > 0x7FFFFF ? 0x7FFFFF : v < -0x800000 ? -0x800000 : v);')
    return 'shifted = scsp_sext((int32_t)((uint32_t)acc << %d), 24);' % (sh == 2)


def emit_step(st, p, prog):
    f = fields(p)
    # MAME: the delay memory is only touched on odd steps
    f['mrd'] = f['mrd'] and (st & 1)
    f['mwt'] = f['mwt'] and (st & 1)

    s = emit_inputs(f, prog) + emit_operands(f, prog)
    sh = f['sh']
    s.append(emit_shift(sh))
    s.append('acc = (int32_t)(((int64_t)x * (int64_t)y) >> 12) + b;')
    if f['twt']:
        s.append('const int32_t t%d = shifted; temp[(%du + dec) & 0x7F] = t%d;' % (st, f['twa'], st))
        prog.temp[f['twa']] = 't%d' % st
    if f['frcl']:
        s.append('frc = (shifted >> %d) & 0x%X;' % ((0, 0x0FFF) if sh == 3 else (11, 0x1FFF)))
    if f['mrd'] or f['mwt']:
        s += emit_mem(st, f, prog)
    if f['adrl']:
        s.append('adrs = %s;' % ('(uint32_t)((shifted >> 12) & 0xFFF)' if sh == 3 else '(uint32_t)(inputs >> 16)'))
    if f['ewt']:
        s.append('e%d += shifted >> 8;' % f['ewa'])
    return '    /* %3d */ %s' % (st, ' '.join(s))


def program_run(words):
    """(last_step, steps run): a bad IRA ends the program on that step."""
    last = last_step(words)
    for st in range(last):
        if ((words[st * 4 + 1] >> 6) & 0x3F) > 0x31:
            return last, st
    return last, last


def emit_prologue(name, words, run, prog):
    # The widest delay-memory address the program forms: rbp + rbl_mask for ring
    # steps, rbp + 0xFFFF for TABLE steps. One bounds check per sample covers all
    # of them; a ring past the end of RAM goes to the interpreter (return 0).
    span = '0xFFFFu' if any((words[st * 4 + 2] >> 15) & 1 and (words[st * 4 + 2] >> 13) & 3
                            for st in range(run)) else 'rbl_mask'
    out = ['static int %s(scsp_t *s) {' % name,
           '    scsp_dsp_t *d = &s->dsp;',
           '    const uint32_t dec = d->dec, rbl_mask = d->rbl - 1, rbp = d->rbp << 12;',
           '    if (((rbp + %s) << 1) + 1 >= s->ram_size) return 0;' % span,
           '    int32_t *temp = d->temp;',
           '    uint8_t *ram = s->ram;',
           '    int32_t acc = 0, memval = 0, frc = 0, yreg = 0, inputs, tr, b, x, y, shifted;',
           '    uint32_t adrs = 0, addr;',
           '    int64_t v;',
           '    (void)frc; (void)yreg; (void)adrs; (void)addr; (void)memval; (void)v; (void)rbl_mask; (void)ram;']
    # everything a step reads from the chip but no step writes: loaded once, up front
    out += ['    const int32_t c%d = d->coef[%d] >> 3;' % (c, c) for c in sorted(prog.coefs)]
    for m, table in sorted(prog.madrs):
        out.append('    const uint32_t %s%d = d->madrs[%d]%s;' % ('mt' if table else 'ma', m, m,
                                                                 '' if table else ' + dec'))
    out += ['    const int32_t mx%d = scsp_sext((int32_t)((uint32_t)d->mixs[%d] << 4), 24);' % (i, i)
            for i in sorted(prog.mixs)]
    out += ['    const int32_t ex%d = scsp_sext(d->exts[%d] * 256, 24);' % (i, i) for i in sorted(prog.exts)]
    if prog.mems_used:
        out.append('    int32_t %s;' % ', '.join('m%d = d->mems[%d]' % (i, i) for i in prog.mems_used))
    out.append('    int32_t %s;' % ', '.join('e%d = 0' % i for i in range(16)))
    return out


def emit_program(prog):
    words = prog['words']
    last, run = program_run(words)
    name = 'scsp_dsp_prog_' + prog['name']
    steps = [fields(words[st * 4:st * 4 + 4]) for st in range(run)]
    state = Program(steps)
    body = [emit_step(st, words[st * 4:st * 4 + 4], state) for st in range(run)]
    out = emit_prologue(name, words, run, state) + body
    if state.mems_written:
        out.append('    %s' % ' '.join('d->mems[%d] = m%d;' % (i, i) for i in state.mems_written))
    # EFREG adds wrap at 16 bits whether they wrap step by step or once here
    out.append('    %s' % ' '.join('d->efreg[%d] = (int16_t)e%d;' % (i, i) for i in range(16)))
    if run < last:
        out.append('    /* the program ends on a bad IRA at step %d: no DEC, MIXS kept */' % run)
    else:
        out.append('    d->dec--;')
        out.append('    memset(d->mixs, 0, sizeof d->mixs);')
    out.append('    return 1;')
    out.append('}')
    return name, last, '\n'.join(out)


def generate():
    progs = read_programs()
    body, table = [], []
    for p in progs:
        name, last, code = emit_program(p)
        body.append(code)
        words = ',\n'.join('        ' + ', '.join('0x%04X' % w for w in p['words'][i:i + 16])
                           for i in range(0, 512, 16))
        table.append('    { %d, %s, {\n%s } },' % (last, name, words))
    text = '''/*
 * scsp_dsp_known.h -- GENERATED by tools/gen-scsp-dsp.py from
 * tools/scsp-dsp-programs.txt. Do not edit; regenerate.
 *
 * DSP microprograms compiled to straight-line C, each with the interpreter's
 * semantics (scsp.h, scsp_dsp_step) and its fields folded to constants.
 * scsp_dsp_decode picks one when MPRO and the program length match an entry
 * word for word; anything else runs on the interpreter. MEMS, the inputs and
 * the coefficients and tap addresses are read once into locals, and a TEMP
 * word written earlier in the sample is read back from its local.
 */
#ifndef SCSP_DSP_KNOWN_H
#define SCSP_DSP_KNOWN_H

/*
 * Delay-memory access. A program's ring (RBP, RBL) is checked against the end
 * of RAM once per sample, and a ring past it runs on the interpreter instead;
 * within it the accesses are the plain big-endian word reads and writes
 * scsp_ram_w/ww make, without their per-access bounds test. The largest
 * address a ring can reach (RBP 0x3F << 12, plus 0xFFFF, doubled) is below the
 * 1 MB mask, so the mask is never needed.
 */
static inline uint16_t scsp_dsp_ring_rd(const uint8_t *ram, uint32_t a) {
    return (uint16_t)((ram[a] << 8) | ram[a + 1]);
}
static inline void scsp_dsp_ring_wr(uint8_t *ram, uint32_t a, uint16_t v) {
    ram[a] = (uint8_t)(v >> 8); ram[a + 1] = (uint8_t)v;
}

%s

typedef struct {
    int      last_step;
    int    (*run)(scsp_t *s);     /* 0: the ring is past the end of RAM, interpret */
    uint16_t mpro[128 * 4];
} scsp_dsp_known_t;

static const scsp_dsp_known_t scsp_dsp_known[] = {
%s
};

#endif /* SCSP_DSP_KNOWN_H */
''' % ('\n\n'.join(body), '\n'.join(table))
    with open(OUT, 'w', newline='\r\n') as f:
        f.write(text)
    print('wrote %s: %d program(s)' % (os.path.relpath(OUT, ROOT), len(progs)))


def add(name, dump):
    data = open(dump, 'rb').read()
    assert len(data) == 1024, 'MPRO is 512 words (1 KB)'
    words = [data[i] | data[i + 1] << 8 for i in range(0, 1024, 2)]
    for p in (read_programs() if os.path.exists(PROGRAMS) else []):
        if p['words'] == words:
            print('already listed as', p['name'])
            return
    with open(PROGRAMS, 'a', newline='\r\n') as f:
        f.write('\nprogram %s   # %d steps\n' % (name, last_step(words)))
        for st in range(128):
            f.write(' '.join('%04X' % w for w in words[st * 4:st * 4 + 4]) + '\n')


if __name__ == '__main__':
    if len(sys.argv) == 4 and sys.argv[1] == '--add':
        add(sys.argv[2], sys.argv[3])
    generate()
