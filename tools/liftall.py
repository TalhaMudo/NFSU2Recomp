#!/usr/bin/env python3
"""Tum fonksiyonlari lift etmeyi dene; eksik komutlari sikliga gore raporla."""
import sys, struct, collections, time
sys.path.insert(0,'tools')
import pefile
from capstone import *
from capstone.x86 import *
from lift import Lifter, Unsupported

exe=sys.argv[1]; L=Lifter(exe)
pe=pefile.PE(exe); IB=pe.OPTIONAL_HEADER.ImageBase
EP=IB+pe.OPTIONAL_HEADER.AddressOfEntryPoint
def rd32(a):
    for nm,va,vs,d in L.secs:
        if va<=a<va+len(d)-3: return struct.unpack_from('<I',d,a-va)[0]
relocs=[int(x,16) for x in open('out/relocs.txt')]
funcs={EP}
for a in relocs:
    v=rd32(a)
    if v is not None and L.in_text(v): funcs.add(v)
# dogrudan cagri hedeflerini de ekle
md=L.md; cov=bytearray(L.TSZ); q=collections.deque(funcs); seen=set(funcs)
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
        m=i.mnemonic; o=i.operands[0] if i.operands else None
        if m=='call':
            if o is not None and o.type==X86_OP_IMM and L.in_text(o.imm):
                if o.imm not in seen: seen.add(o.imm); funcs.add(o.imm); q.append(o.imm)
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

funcs=sorted(funcs)
print(f"denenecek fonksiyon: {len(funcs)}")
ok=0; bad=0; missing=collections.Counter(); bad_fn=collections.Counter(); t0=time.time()
tot_ins=0; ok_ins=0
for n,f in enumerate(funcs):
    if n%3000==0 and n: print(f"  ...{n}/{len(funcs)}  ({time.time()-t0:.0f}s)",file=sys.stderr)
    try:
        blocks,_=L.discover(f, maxins=3000)
    except Exception: bad+=1; continue
    ni=sum(len(b) for b in blocks.values()); tot_ins+=ni
    fail=None
    for ba in blocks:
        for i in blocks[ba]:
            m=i.mnemonic
            if m=='jmp' or (m.startswith('j') and m[1:] in {'o','no','b','ae','e','ne','be','a','s','ns','p','np','l','ge','le','g','nae','nb','c','nc','z','nz','na','nbe','pe','po','nge','nl','ng','nle'}):
                continue
            try: L.insn(i,set())
            except Unsupported as e:
                missing[str(e)]+=1
                if fail is None: fail=str(e)
            except Exception as e:
                missing[f"<hata:{type(e).__name__}>"]+=1
                if fail is None: fail=str(e)
    if fail is None: ok+=1; ok_ins+=ni
    else: bad+=1; bad_fn[fail]+=1
print(f"\n=== SONUC ({time.time()-t0:.0f}s) ===")
print(f"  TAM lift edilen fonksiyon : {ok}/{len(funcs)} = {100.0*ok/len(funcs):.1f}%")
print(f"  komut bazinda kapsam      : {ok_ins}/{tot_ins} = {100.0*ok_ins/max(tot_ins,1):.1f}%")
print(f"\n=== EKSIK KOMUTLAR (siklik) — sonraki is listesi ===")
for m,cnt in missing.most_common(30):
    print(f"  {m:14s} {cnt:7d}   (bu yuzden takilan fonksiyon: {bad_fn.get(m,0)})")
print(f"\n  farkli eksik komut cesidi: {len(missing)}")
tot_missing=sum(missing.values())
print(f"  toplam eksik komut ornegi: {tot_missing}  = kodun %{100.0*tot_missing/max(tot_ins,1):.2f}'si")
