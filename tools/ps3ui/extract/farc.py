import struct, sys, os, zlib
def entries(d):
    m = d[:4]; hs = struct.unpack('>I', d[4:8])[0]
    p = 8
    if m == b'FArc':
        p += 4  # alignment
        while p < 8+hs:
            e = d.index(b'\0', p); n = d[p:e].decode(); p = e+1
            off, sz = struct.unpack('>II', d[p:p+8]); p += 8
            yield n, d[off:off+sz]
    elif m == b'FArC':
        p += 4
        while p < 8+hs:
            e = d.index(b'\0', p); n = d[p:e].decode(); p = e+1
            off, csz, sz = struct.unpack('>III', d[p:p+12]); p += 12
            yield n, zlib.decompress(d[off:off+csz], 31)
    else: raise Exception(m)
if __name__ == '__main__':
    for fn in sys.argv[1:]:
        d = open(fn,'rb').read(); od = fn + '.d'; os.makedirs(od, exist_ok=True)
        for n, b in entries(d):
            open(os.path.join(od, n), 'wb').write(b); print(fn, n, len(b), b[:4])
