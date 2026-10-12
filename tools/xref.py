#!/usr/bin/env python3
"""Bir adrese call/jmp eden tum yerleri bul (lineer tarama)."""
import sys
sys.path.insert(0,'tools')
from lift import Lifter
from capstone.x86 import *
L=Lifter(sys.argv[1])
targets=[int(t,16) for t in sys.argv[2:]]
hits={t:[] for t in targets}
off=0
while off < L.TSZ-16:
    try: i=next(L.md.disasm(L.TDATA[off:off+16], L.TVA+off))
    except StopIteration: off+=1; continue
    if i.operands and i.operands[0].type==X86_OP_IMM:
        t=i.operands[0].imm
        if t in hits and i.mnemonic in ('call','jmp') or (t in hits and i.mnemonic.startswith('j')):
            hits[t].append((i.address,i.mnemonic))
    off+=1
for t in targets:
    print(f"\n=== 0x{t:08x} referanslari: {len(hits[t])} ===")
    for a,m in hits[t][:30]: print(f"  0x{a:08x}  {m}")
