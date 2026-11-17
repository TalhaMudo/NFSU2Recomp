#!/usr/bin/env python3
"""Classify uncovered blocks: data, or unreachable code?"""
import sys, struct, collections
import pefile
from capstone import *
from capstone.x86 import *

pe = pefile.PE(sys.argv[1])
IB = pe.OPTIONAL_HEADER.ImageBase
secs=[]
for s in pe.sections:
    raw = s.Name.rstrip(b'\x00')
    nm = raw.decode('latin1').strip() or '<UNNAMED>'
    secs.append((nm, IB+s.VirtualAddress, s.Misc_VirtualSize, s.get_data(), s.Characteristics))

print("=== UNNAMED SECTION IDENTITY ===")
for nm,va,vs,data,ch in secs:
    if nm=='<UNNAMED>':
        print(f"VA 0x{va:08x} size {vs}  char 0x{ch:08x} (WRITE={bool(ch&0x80000000)}, EXEC={bool(ch&0x20000000)}, READ={bool(ch&0x40000000)})")
        print("first 64 bytes:", ' '.join(f'{b:02x}' for b in data[:64]))
        # extract readable strings
        cur=b''; found=[]
        for c in data[:60000]:
            if 32<=c<127: cur+=bytes([c])
            else:
                if len(cur)>=6: found.append(cur.decode('latin1'))
                cur=b''
        print(f"strings in the first 60KB; examples: {len(found)} ")
        for s2 in found[:25]: print("   ", s2)
        # entropi
        import math
        cnt=collections.Counter(data[:100000]); n=sum(cnt.values())
        ent=-sum((c/n)*math.log2(c/n) for c in cnt.values())
        print(f"entropy (first 100KB): {ent:.2f} / 8.0  -> {'COMPRESSED/ENCRYPTED' if ent>7.5 else 'normal code/data'}")
