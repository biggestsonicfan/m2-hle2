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


def emit_step(st, p):
    tra = (p[0] >> 8) & 0x7F; twt = (p[0] >> 7) & 1; twa = p[0] & 0x7F
    xsel = (p[1] >> 15) & 1; ysel = (p[1] >> 13) & 3; ira = (p[1] >> 6) & 0x3F
    iwt = (p[1] >> 5) & 1; iwa = p[1] & 0x1F
    table = (p[2] >> 15) & 1; mwt = (p[2] >> 14) & 1; mrd = (p[2] >> 13) & 1
    ewt = (p[2] >> 12) & 1; ewa = (p[2] >> 8) & 0xF; adrl = (p[2] >> 7) & 1
    frcl = (p[2] >> 6) & 1; sh = (p[2] >> 4) & 3; yrl = (p[2] >> 3) & 1
    negb = (p[2] >> 2) & 1; zero = (p[2] >> 1) & 1; bsel = p[2] & 1
    nofl = (p[3] >> 15) & 1; cra = (p[3] >> 9) & 0x3F; masa = (p[3] >> 2) & 0x1F
    adreb = (p[3] >> 1) & 1; nxadr = p[3] & 1
    # MAME: the delay memory is only touched on odd steps
    mrd = mrd and (st & 1)
    mwt = mwt and (st & 1)

    s = []
    if ira <= 0x1F:
        s.append('inputs = scsp_sext(mems[%d], 24);' % ira)
    elif ira <= 0x2F:
        s.append('inputs = scsp_sext((int32_t)((uint32_t)d->mixs[%d] << 4), 24);' % (ira - 0x20))
    else:
        s.append('inputs = scsp_sext(d->exts[%d] * 256, 24);' % (ira - 0x30))
    if iwt:
        if ira == iwa:
            s.append('inputs = memval;')
        s.append('mems[%d] = memval;' % iwa)
    s.append('tr = scsp_sext(temp[(%du + dec) & 0x7F], 24);' % tra)
    if zero:
        s.append('b = 0;')
    else:
        base = 'acc' if bsel else 'tr'
        s.append('b = %s;' % ('(int32_t)(0u - (uint32_t)%s)' % base if negb else base))
    s.append('x = %s;' % ('inputs' if xsel else 'tr'))
    ysrc = ['frc', 'd->coef[%d]' % cra, 'yreg', 'yreg'][ysel]
    ysh = [0, 3, 11, 4][ysel]
    ymask = 0x0FFF if ysel == 3 else 0x1FFF
    s.append('y = scsp_sext((%s >> %d) & 0x%X, 13);' % (ysrc, ysh, ymask))
    if yrl:
        s.append('yreg = inputs;')
    dbl = 1 if sh in (1, 2) else 0
    if sh <= 1:
        s.append('v = (int64_t)((uint64_t)(int64_t)acc << %d); '
                 'shifted = (int32_t)(v > 0x7FFFFF ? 0x7FFFFF : v < -0x800000 ? -0x800000 : v);' % dbl)
    else:
        s.append('v = (int64_t)((uint64_t)(int64_t)acc << %d); '
                 'shifted = scsp_sext((int32_t)(uint32_t)v, 24);' % dbl)
    s.append('acc = (int32_t)(((int64_t)x * (int64_t)y) >> 12) + b;')
    if twt:
        s.append('temp[(%du + dec) & 0x7F] = shifted;' % twa)
    if frcl:
        s.append('frc = (shifted >> %d) & 0x%X;' % ((0, 0x0FFF) if sh == 3 else (11, 0x1FFF)))
    if mrd or mwt:
        a = 'd->madrs[%d]' % masa
        if not table:
            a += ' + dec'
        if adreb:
            a += ' + (adrs & 0x0FFF)'
        if nxadr:
            a += ' + 1u'
        s.append('addr = ((((uint32_t)%s) & %s) + rbp) << 1;' % (a, '0xFFFFu' if table else 'rbl_mask'))
        if mrd:
            s.append('memval = %s;' % ('scsp_ram_w(s, addr) << 8' if nofl else 'scsp_dsp_unpack(scsp_ram_w(s, addr))'))
        if mwt:
            s.append('scsp_ram_ww(s, addr, %s);' % ('(uint16_t)(shifted >> 8)' if nofl else 'scsp_dsp_pack(shifted)'))
    if adrl:
        s.append('adrs = %s;' % ('(uint32_t)((shifted >> 12) & 0xFFF)' if sh == 3 else '(uint32_t)(inputs >> 16)'))
    if ewt:
        s.append('efreg[%d] = (int16_t)(efreg[%d] + (shifted >> 8));' % (ewa, ewa))
    return '    /* %3d */ { %s }' % (st, ' '.join(s))


def emit_program(prog):
    words = prog['words']
    last = last_step(words)
    run = last
    for st in range(last):
        if ((words[st * 4 + 1] >> 6) & 0x3F) > 0x31:   # a bad IRA ends the program there
            run = st
            break
    name = 'scsp_dsp_prog_' + prog['name']
    out = ['static void %s(scsp_t *s) {' % name,
           '    scsp_dsp_t *d = &s->dsp;',
           '    int32_t *mems = d->mems, *temp = d->temp;',
           '    int16_t efreg[16] = {0};',
           '    const uint32_t dec = d->dec, rbl_mask = d->rbl - 1, rbp = d->rbp << 12;',
           '    int32_t acc = 0, memval = 0, frc = 0, yreg = 0, inputs, tr, b, x, y, shifted;',
           '    uint32_t adrs = 0, addr;',
           '    int64_t v;',
           '    (void)frc; (void)yreg; (void)adrs; (void)addr; (void)memval; (void)rbl_mask; (void)rbp;']
    for st in range(run):
        out.append(emit_step(st, words[st * 4:st * 4 + 4]))
    out.append('    memcpy(d->efreg, efreg, sizeof d->efreg);')
    if run < last:
        out.append('    /* the program ends on a bad IRA at step %d: no DEC, MIXS kept */' % run)
    else:
        out.append('    d->dec--;')
        out.append('    memset(d->mixs, 0, sizeof d->mixs);')
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
 * word for word; anything else runs on the interpreter.
 */
#ifndef SCSP_DSP_KNOWN_H
#define SCSP_DSP_KNOWN_H

%s

typedef struct {
    int      last_step;
    void   (*run)(scsp_t *s);
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
