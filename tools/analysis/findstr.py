#!/usr/bin/env python3
"""Search for a string in the image and find references to its address (reloc assisted)."""
import sys, struct
import os as _os; sys.path.insert(0, _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '..', 'lifter'))
import pefile
from capstone import *
from capstone.x86 import *
from lift import Lifter
EXE=sys.argv[1]; NEEDLE=sys.argv[2].encode('latin1')
L=Lifter(EXE)
hits=[]
for nm,va,vs,d in L.secs:
    off=0
    while True:
        k=d.find(NEEDLE,off)
        if k<0: break
        hits.append((va+k,nm)); off=k+1
print(f"'{sys.argv[2]}' found: {len(hits)} places")
for a,nm in hits: print(f"  0x{a:08x}  ({nm})")
if not hits: sys.exit()

# does anything at reloc locations point to these addresses?
def rd32(a):
    for nm,va,vs,d in L.secs:
        if va<=a<va+len(d)-3: return struct.unpack_from('<I',d,a-va)[0]
relocs=[int(x,16) for x in open('out/relocs.txt')]
targets={a for a,_ in hits}
print("\n=== reloc locations pointing to these addresses ===")
refs=[]
for r in relocs:
    v=rd32(r)
    if v in targets:
        refs.append((r,v))
        loc=next((nm for nm,va,vs,d in L.secs if va<=r<va+vs),'?')
        print(f"  0x{r:08x} ({loc}) -> 0x{v:08x}")
# dump the surroundings of the references inside .text
print("\n=== surroundings of the references inside .text ===")
for r,v in refs:
    if L.in_text(r):
        start=max(L.TVA, r-40)
        va=start
        print(f"--- 0x{r:08x} surroundings ---")
        while va < r+24:
            off=va-L.TVA
            try: i=next(L.md.disasm(L.TDATA[off:off+16],va))
            except StopIteration: break
            mark = '  <== STRING' if (i.address <= r < i.address+i.size) else ''
            print(f"  {i.address:08x}  {i.mnemonic:9s} {i.op_str}{mark}")
            va=i.address+i.size
