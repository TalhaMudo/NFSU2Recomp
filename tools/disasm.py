#!/usr/bin/env python3
import sys; sys.path.insert(0,'tools')
from lift import Lifter
from capstone.x86 import *
L=Lifter(sys.argv[1])
for spec in sys.argv[2:]:
    if '-' in spec: a,b=spec.split('-'); a=int(a,16); b=int(b,16)
    else: a=int(spec,16); b=a+0x60
    print(f"--- 0x{a:08x} .. 0x{b:08x} ---")
    va=a
    while va<b:
        off=va-L.TVA
        try: i=next(L.md.disasm(L.TDATA[off:off+16],va))
        except StopIteration: break
        mark=''
        if i.mnemonic=='call' and i.operands and i.operands[0].type==X86_OP_IMM:
            mark='   <-- call'
        print(f"  {i.address:08x}  {i.bytes.hex():<14s} {i.mnemonic:9s} {i.op_str}{mark}")
        va=i.address+i.size
    print()
