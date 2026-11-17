#!/usr/bin/env python3
import sys,struct,collections
import os as _os; sys.path.insert(0, _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '..', 'lifter'))
import pefile
from capstone import *
from capstone.x86 import *
from lift import Lifter, Unsupported
L=Lifter(sys.argv[1]); pe=pefile.PE(sys.argv[1]); IB=pe.OPTIONAL_HEADER.ImageBase
def rd32(a):
    for nm,va,vs,d in L.secs:
        if va<=a<va+len(d)-3: return struct.unpack_from('<I',d,a-va)[0]
funcs={IB+pe.OPTIONAL_HEADER.AddressOfEntryPoint}
for a in (int(x,16) for x in open('out/relocs.txt')):
    v=rd32(a)
    if v is not None and L.in_text(v): funcs.add(v)
res=[]
for f in sorted(funcs):
    try: blocks,calls=L.discover(f,maxins=60)
    except Exception: continue
    ins=[i for ba in sorted(blocks) for i in blocks[ba]]
    if not (6<=len(ins)<=22): continue
    if calls: continue
    mn=[i.mnemonic for i in ins]
    if not any(x.startswith('f') for x in mn): continue
    if any(x=='call' for x in mn): continue
    # no global memory access, stack only
    # allow global CONSTANT reads (rdata float constants), don't allow writes
    if any(o.type==X86_OP_MEM and o.mem.base==0 and o.mem.index==0 and i.mnemonic in ('mov','stos') for i in ins for o in i.operands[:1]): continue
    try:
        for i in ins:
            if i.mnemonic=='jmp' or (i.mnemonic.startswith('j') and len(i.mnemonic)<=3): continue
            L.insn(i,set())
    except Unsupported: continue
    if mn[-1] not in ('ret','retf'): continue
    res.append((len(ins),f,ins))
res.sort()
print(f"suitable x87 functions: {len(res)}\n")
for n,f,ins in res[:8]:
    print(f"--- 0x{f:08x} ({n} instructions) ---")
    for i in ins: print(f"   {i.address:08x}  {i.mnemonic:10s} {i.op_str}")
    print()
