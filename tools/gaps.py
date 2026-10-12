#!/usr/bin/env python3
"""Kapsanmayan bloklari siniflandir: veri mi, ulasilamayan kod mu?"""
import sys, struct, collections
import pefile
from capstone import *
from capstone.x86 import *

pe = pefile.PE(sys.argv[1])
IB = pe.OPTIONAL_HEADER.ImageBase
secs=[]
for s in pe.sections:
    raw = s.Name.rstrip(b'\x00')
    nm = raw.decode('latin1').strip() or '<ISIMSIZ>'
    secs.append((nm, IB+s.VirtualAddress, s.Misc_VirtualSize, s.get_data(), s.Characteristics))

print("=== ISIMSIZ BOLUM KIMLIGI ===")
for nm,va,vs,data,ch in secs:
    if nm=='<ISIMSIZ>':
        print(f"VA 0x{va:08x} boyut {vs}  char 0x{ch:08x} (WRITE={bool(ch&0x80000000)}, EXEC={bool(ch&0x20000000)}, READ={bool(ch&0x40000000)})")
        print("ilk 64 bayt:", ' '.join(f'{b:02x}' for b in data[:64]))
        # okunabilir stringleri cikar
        cur=b''; found=[]
        for c in data[:60000]:
            if 32<=c<127: cur+=bytes([c])
            else:
                if len(cur)>=6: found.append(cur.decode('latin1'))
                cur=b''
        print(f"ilk 60KB'de {len(found)} string; ornekler:")
        for s2 in found[:25]: print("   ", s2)
        # entropi
        import math
        cnt=collections.Counter(data[:100000]); n=sum(cnt.values())
        ent=-sum((c/n)*math.log2(c/n) for c in cnt.values())
        print(f"entropi (ilk 100KB): {ent:.2f} / 8.0  -> {'SIKISTIRILMIS/SIFRELI' if ent>7.5 else 'normal kod/veri'}")
