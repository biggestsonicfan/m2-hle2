import struct, zlib, sys, os
p = sys.argv[1]; out = sys.argv[2]
f = open(p,'rb'); d = f.read()
magic, ver, comp, toclen, entsz, nent, bsz, flags = struct.unpack('>4sI4sIIIII', d[:32])
toc = d[32:32+entsz*nent]
ents = []
for i in range(nent):
    e = toc[i*entsz:(i+1)*entsz]
    idx = struct.unpack('>I', e[16:20])[0]
    size = int.from_bytes(e[20:25],'big'); off = int.from_bytes(e[25:30],'big')
    ents.append((idx,size,off))
bw = 2 if bsz<=65536 else 3 if bsz<=16777216 else 4
nb = (toclen - 32 - entsz*nent)//bw
bl = [int.from_bytes(d[32+entsz*nent+i*bw:32+entsz*nent+(i+1)*bw],'big') for i in range(nb)]
def read(e):
    idx,size,off = e; res=bytearray(); pos=off
    while len(res)<size:
        z = bl[idx]; idx+=1
        if z==0: z=bsz
        chunk = d[pos:pos+z]; pos+=z
        try: res += zlib.decompress(chunk)
        except Exception: res += chunk
    return bytes(res[:size])
names = read(ents[0]).decode().split('\n')
for n,e in zip(names, ents[1:]):
    n=n.strip().lstrip('/')
    if not n: continue
    fp=os.path.join(out,n); os.makedirs(os.path.dirname(fp),exist_ok=True)
    open(fp,'wb').write(read(e))
print(len(names))
