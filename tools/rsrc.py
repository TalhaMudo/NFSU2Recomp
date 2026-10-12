#!/usr/bin/env python3
"""PE .rsrc dizinini gez, tum kaynaklari listele."""
import sys, struct
EXE = "Need for Speed Underground 2 - Clean Edition/Need for Speed Underground 2/SPEED2.EXE"
d = open(EXE,'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]
nsec = struct.unpack_from('<H', d, pe+6)[0]
optsz = struct.unpack_from('<H', d, pe+20)[0]
imgbase = struct.unpack_from('<I', d, pe+24+28)[0]
secs=[]
for i in range(nsec):
    o = pe+24+optsz+40*i
    nm = d[o:o+8].rstrip(b'\0').decode('latin1')
    vsz, va, rsz, ra = struct.unpack_from('<IIII', d, o+8)
    secs.append((nm, va, vsz, ra, rsz))
rs = [s for s in secs if s[0]=='.rsrc'][0]
_, rva, rvsz, rra, rrsz = rs
def at(off): return rra + off
def walk(off, depth, path):
    n_named, n_id = struct.unpack_from('<HH', d, at(off)+12)
    for i in range(n_named + n_id):
        e = at(off) + 16 + 8*i
        name, oto = struct.unpack_from('<II', d, e)
        if name & 0x80000000:
            p = at(name & 0x7FFFFFFF)
            ln = struct.unpack_from('<H', d, p)[0]
            label = d[p+2:p+2+2*ln].decode('utf-16-le')
        else:
            label = str(name)
        if oto & 0x80000000:
            walk(oto & 0x7FFFFFFF, depth+1, path+[label])
        else:
            dva, dsz, _, _ = struct.unpack_from('<IIII', d, at(oto))
            print("  %-40s VA=0x%08x boyut=%d" % ('/'.join(path+[label]), imgbase+dva, dsz))
walk(0, 0, [])
