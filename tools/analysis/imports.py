#!/usr/bin/env python3
"""Extract the IAT and count how many call sites each import has inside .text."""
import sys, struct, collections
import os as _os; sys.path.insert(0, _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '..', 'lifter'))
import pefile
from capstone import *
from capstone.x86 import *
from lift import Lifter

EXE=sys.argv[1]
pe=pefile.PE(EXE); IB=pe.OPTIONAL_HEADER.ImageBase
L=Lifter(EXE)
pe.parse_data_directories()
iat={}   # iat_va -> (dll, name)
for e in pe.DIRECTORY_ENTRY_IMPORT:
    dll=e.dll.decode('latin1')
    for imp in e.imports:
        nm = imp.name.decode('latin1') if imp.name else f"#{imp.ordinal}"
        iat[imp.address] = (dll, nm)
print(f"IAT entries: {len(iat)}  (address range 0x{min(iat):08x}-0x{max(iat):08x})")

# scan .text: call/jmp dword ptr [IAT]
md=L.md; md.detail=True
EPa=IB+pe.OPTIONAL_HEADER.AddressOfEntryPoint
def rd32(a):
    for nm,va,vs,d in L.secs:
        if va<=a<va+len(d)-3: return struct.unpack_from('<I',d,a-va)[0]
seeds={EPa}
for a in (int(x,16) for x in open('out/relocs.txt')):
    v=rd32(a)
    if v is not None and L.in_text(v): seeds.add(v)
cov=bytearray(L.TSZ); q=collections.deque(seeds); seen=set(seeds)
sites=collections.Counter(); indirect_other=0
while q:
    va=q.popleft()
    while True:
        if not L.in_text(va): break
        off=va-L.TVA
        if off>=L.TSZ or cov[off]: break
        try: i=next(md.disasm(L.TDATA[off:off+16],va))
        except StopIteration: break
        for k in range(i.size):
            if off+k<L.TSZ: cov[off+k]=1
        m=i.mnemonic; ops=i.operands; o=ops[0] if ops else None
        if m in ('call','jmp') and o is not None and o.type==X86_OP_MEM \
           and o.mem.base==0 and o.mem.index==0:
            t=o.mem.disp & 0xffffffff
            if t in iat: sites[t]+=1
            elif m=='call': indirect_other+=1
        if m=='call':
            if o is not None and o.type==X86_OP_IMM and L.in_text(o.imm) and o.imm not in seen:
                seen.add(o.imm); q.append(o.imm)
            va=i.address+i.size; continue
        if m=='jmp':
            if o is not None and o.type==X86_OP_IMM: va=o.imm; continue
            break
        if m.startswith('j'):
            if o is not None and o.type==X86_OP_IMM and o.imm not in seen:
                seen.add(o.imm); q.append(o.imm)
            va=i.address+i.size; continue
        if m in ('ret','retf','int3','hlt','ud2'): break
        va=i.address+i.size

by_dll=collections.Counter(); used=0
rows=[]
for a,(dll,nm) in iat.items():
    n=sites.get(a,0)
    if n: used+=1
    by_dll[dll]+=n
    rows.append((n,dll,nm,a))
rows.sort(reverse=True)
print(f"called imports: {used}/{len(iat)}   indirect calls outside the IAT: {indirect_other}\n")
print("=== CALL SITES PER DLL ===")
for d,n in by_dll.most_common(): print(f"  {d:14s} {n:5d}")
print(f"\n=== TOP 40 MOST CALLED IMPORTS (Stage 3 priority list) ===")
for n,dll,nm,a in rows[:40]:
    print(f"  {n:5d}  {dll:14s} {nm}")
print(f"\n=== {len(iat)-used} NEVER CALLED IMPORTS (a stub is enough) ===")
z=[f"{dll}!{nm}" for n,dll,nm,a in rows if n==0]
for i in range(0,min(len(z),60),4): print("   "+"  ".join(f"{x:26s}" for x in z[i:i+4]))
# generate the C table
with open('gen/all/imports.c','w') as f:
    f.write('#include "cpu.h"\n#include <stdint.h>\n')
    f.write('typedef struct { uint32_t iat; const char*dll; const char*name; } ImpEnt;\n')
    f.write(f'const uint32_t g_imp_count = {len(iat)};\nconst ImpEnt g_imp[] = {{\n')
    for a,(dll,nm) in sorted(iat.items()):
        f.write(f'  {{0x{a:08x}u,"{dll}","{nm}"}},\n')
    f.write('};\n')
print(f"\n-> gen/all/imports.c written ({len(iat)} entries)")
