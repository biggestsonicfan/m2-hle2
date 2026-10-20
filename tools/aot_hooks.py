#!/usr/bin/env python3
"""aot_hooks.py HEADER... : the hook addresses of the given profile headers, as
i960_aot.py --hook arguments, one line. An entry is `{ ADDR, fn, "name" }` in a
hook table; ADDR is a hex literal or a macro, which is looked up as a
`#define NAME 0x...` in the headers and everything under src/. A macro that
cannot be found is an error: a hook the compiled code does not know turns the
whole AOT off at run time (i960_aot.h aot_check_profile), and quietly.
"""
import glob, os, re, sys

ENTRY = re.compile(r'\{\s*([A-Za-z_][A-Za-z0-9_]*|0x[0-9A-Fa-f]+)u?\s*,\s*[A-Za-z0-9_]*hook[A-Za-z0-9_]*\s*,')
DEFINE = re.compile(r'^\s*#\s*define\s+([A-Za-z_][A-Za-z0-9_]*)\s+(0x[0-9A-Fa-f]+)u?\b', re.M)

def main(argv):
    headers = argv[1:]
    if not headers: sys.exit(__doc__)
    src = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'src'))
    defs = {}
    for p in glob.glob(os.path.join(src, '**', '*.h'), recursive=True) + headers:
        for m in DEFINE.finditer(open(p, encoding='utf-8', errors='replace').read()):
            defs.setdefault(m.group(1), int(m.group(2), 16))
    addrs = set()
    for h in headers:
        for m in ENTRY.finditer(open(h, encoding='utf-8', errors='replace').read()):
            tok = m.group(1)
            if tok.startswith('0x'):
                addrs.add(int(tok, 16))
            elif tok in defs:
                addrs.add(defs[tok])
            else:
                sys.exit(f'aot_hooks: {h}: no #define for {tok}')
    print(' '.join('--hook 0x%X' % a for a in sorted(addrs)))

if __name__ == '__main__':
    main(sys.argv)
