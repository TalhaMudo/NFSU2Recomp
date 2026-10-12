#!/usr/bin/env python3
"""SPEED2.EXE'den verilen adres araligini sokumle."""
import sys, struct
sys.path.insert(0,'tools')
import capstone

EXE = "Need for Speed Underground 2 - Clean Edition/Need for Speed Underground 2/SPEED2.EXE"
d = open(EXE,'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]
nsec = struct.unpack_from('<H', d, pe+6)[0]
opt  = struct.unpack_from('<H', d, pe+20)[0]
imgbase = struct.unpack_from('<I', d, pe+24+28)[0]
secs=[]
for i in range(nsec):
    o = pe+24+opt+40*i
    name = d[o:o+8].rstrip(b'\0').decode('latin1')
    vsz, va, rsz, ra = struct.unpack_from('<IIII', d, o+8)
    secs.append((name, imgbase+va, vsz, ra, rsz))

def read(va, n):
    for name, sva, vsz, ra, rsz in secs:
        if sva <= va < sva+vsz:
            off = ra + (va - sva)
            return d[off:off+n]
    return b''

start = int(sys.argv[1], 16)
count = int(sys.argv[2]) if len(sys.argv)>2 else 60
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
md.detail = False
buf = read(start, count*8)
n=0
for i in md.disasm(buf, start):
    print("  %08x  %-24s %s" % (i.address, i.mnemonic, i.op_str))
    n+=1
    if n>=count: break
