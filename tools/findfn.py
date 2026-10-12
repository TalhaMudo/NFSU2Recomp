#!/usr/bin/env python3
"""Lifter'i dogrulamak icin kucuk, yaprak (leaf), tamsayi-only fonksiyonlar bul."""
import sys,struct,collections
import pefile
from capstone import *
from capstone.x86 import *
pe=pefile.PE(sys.argv[1]); IB=pe.OPTIONAL_HEADER.ImageBase
secs=[]
for s in pe.sections:
    nm=s.Name.rstrip(b'\x00').decode('latin1').strip() or '<none>'
    secs.append((nm,IB+s.VirtualAddress,s.Misc_VirtualSize,s.get_data()))
TVA,TSZ,TDATA=next((va,vs,d) for nm,va,vs,d in secs if nm=='.text'); TEND=TVA+TSZ
def in_text(a): return TVA<=a<TEND
def rd32(a):
    for nm,va,vs,d in secs:
        if va<=a<va+len(d)-3: return struct.unpack_from('<I',d,a-va)[0]
relocs=[int(x,16) for x in open('out/relocs.txt')]
# fonksiyon adaylari: call hedefleri
md=Cs(CS_ARCH_X86,CS_MODE_32); md.detail=True
EP=IB+pe.OPTIONAL_HEADER.AddressOfEntryPoint
cov=bytearray(TSZ); seen=set(); q=collections.deque(); calls=collections.Counter(); funcs=set()
def seed(a,f=False):
    if in_text(a) and a not in seen: seen.add(a); q.append(a)
    if f and in_text(a): funcs.add(a)
seed(EP,True)
for a in relocs:
    v=rd32(a)
    if v is not None and in_text(v): seed(v,True)
while q:
    va=q.popleft()
    while True:
        if not in_text(va): break
        off=va-TVA
        if off>=TSZ or cov[off]: break
        try: i=next(md.disasm(TDATA[off:off+16],va))
        except StopIteration: break
        for k in range(i.size):
            if off+k<TSZ: cov[off+k]=1
        m=i.mnemonic; ops=i.operands; op=ops[0] if ops else None
        if m=='call':
            if op is not None and op.type==X86_OP_IMM: seed(op.imm,True); calls[op.imm]+=1
            va=i.address+i.size; continue
        if m in ('jmp','ljmp'):
            if op is not None and op.type==X86_OP_IMM: va=op.imm; continue
            break
        if m.startswith('j'):
            if op is not None and op.type==X86_OP_IMM: seed(op.imm)
            va=i.address+i.size; continue
        if m in ('ret','retf','int3','hlt','ud2'): break
        va=i.address+i.size
print(f"toplam fonksiyon adayi: {len(funcs)}, dogrudan cagrilan: {len(calls)}")
# her fonksiyonu lineer tara, yaprak+tamsayi olanlari sec
INT_OK=set('mov push pop add sub cmp test lea xor and or inc dec ret shl shr sar neg not imul mul div idiv movzx movsx setne sete xchg nop leave cdq sbb adc cmovne cmove'.split())
cands=[]
for f in sorted(funcs):
    off=f-TVA; ins=[]; ok=True; n=0
    va=f
    while n<80:
        o=va-TVA
        if o>=TSZ: ok=False;break
        try: i=next(md.disasm(TDATA[o:o+16],va))
        except StopIteration: ok=False;break
        m=i.mnemonic; ins.append(i); n+=1
        if m=='ret': break
        if m=='call': ok=False;break
        if m.startswith('j') and m!='jmp':
            t=i.operands[0]
            if t.type==X86_OP_IMM and not (f<=t.imm<va+i.size+40): ok=False;break
        elif m=='jmp': ok=False;break
        elif m not in INT_OK: ok=False;break
        va=i.address+i.size
    else: ok=False
    if ok and 4<=len(ins)<=25 and ins[-1].mnemonic=='ret':
        # global bellege dokunmuyor mu? (sadece stack/register)
        touches_mem = any(o.type==X86_OP_MEM and o.mem.base not in (X86_REG_ESP,X86_REG_EBP) for i2 in ins for o in i2.operands)
        cands.append((len(ins), f, touches_mem, calls[f]))
cands.sort(key=lambda x:(x[2], -x[3], x[0]))
print(f"\nuygun aday: {len(cands)}\n")
print("=== EN IYI 8 ADAY (yaprak, tamsayi, global bellek yok) ===")
for n,f,tm,cc in cands[:8]:
    print(f"\n--- 0x{f:08x}  ({n} komut, {cc} kez cagriliyor, globalbellek={tm}) ---")
    va=f
    for _ in range(n):
        o=va-TVA; i=next(md.disasm(TDATA[o:o+16],va))
        print(f"   {i.address:08x}  {i.bytes.hex():<16s} {i.mnemonic} {i.op_str}")
        va=i.address+i.size
