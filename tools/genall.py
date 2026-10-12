#!/usr/bin/env python3
"""23k fonksiyonun tamamini C'ye cevir, shard'lara bol, fonksiyon tablosunu uret."""
import sys, os, struct, collections, time, multiprocessing as mp
sys.path.insert(0,'tools')
import pefile
from capstone import *
from capstone.x86 import *
from lift import Lifter, Unsupported

EXE = sys.argv[1]
NSHARD = int(sys.argv[2]) if len(sys.argv)>2 else 128
OUT = 'gen/all'
os.makedirs(OUT, exist_ok=True)

# ---------- Faz 1: kapali fonksiyon kumesi ----------
def discover_all():
    L=Lifter(EXE); pe=pefile.PE(EXE); IB=pe.OPTIONAL_HEADER.ImageBase
    EP=IB+pe.OPTIONAL_HEADER.AddressOfEntryPoint
    def rd32(a):
        for nm,va,vs,d in L.secs:
            if va<=a<va+len(d)-3: return struct.unpack_from('<I',d,a-va)[0]
    funcs={EP}
    for a in (int(x,16) for x in open('out/relocs.txt')):
        v=rd32(a)
        if v is not None and L.in_text(v): funcs.add(v)
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
                if o is not None and o.type==X86_OP_IMM and L.in_text(o.imm) and o.imm not in seen:
                    seen.add(o.imm); funcs.add(o.imm); q.append(o.imm)
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
    return sorted(funcs)

# ---------- Faz 2: paralel uretim ----------
_L=None
OVERRIDES = {int(x,16) for x in os.environ.get('OVERRIDE','').replace('0x','').split(',') if x.strip()}

def _init():
    global _L
    hooks = [int(x,16) for x in os.environ.get('HOOKS','').replace('0x','').split(',') if x.strip()]
    watch = os.environ.get('WATCH','') == '1'
    cov   = os.environ.get('COVERAGE','') == '1'
    _L = Lifter(EXE, comments=False, trap=True, hooks=hooks, watch=watch)
    _L.cov = cov

def _shard(args):
    idx, addrs = args
    L=_L; parts=[]; defined=[]; refs=set(); traps=0; trunc=0; kinds=collections.Counter()
    for a in addrs:
        L.traps=0; L.truncated=0
        try:
            code,tg = L.lift(a)
        except Exception as e:
            parts.append(f"void sub_{a:08x}(CPU *c){{ unsupported_insn(c,0x{a:x}u,\"lift-hatasi\"); POP32(c->eip); }}")
            defined.append(a); kinds[f"<lift:{type(e).__name__}>"]+=1; continue
        if a in OVERRIDES:
            code = code.replace(f"void sub_{a:08x}(CPU *restrict c) {{",
                                f"void sub_{a:08x}_orig(CPU *restrict c) {{")
        parts.append(code); defined.append(a)
        refs |= {t for t in tg if L.in_text(t)}
        traps += L.traps; trunc += L.truncated
        kinds.update(L.trap_kinds); L.trap_kinds.clear()
    path=f"{OUT}/shard_{idx:04d}.c"
    with open(path,'w') as f:
        f.write('#include "cpu.h"\n#include "decls.h"\n#include <math.h>\n\n')
        f.write("\n".join(parts))
        f.write("\n")
    return idx, defined, sorted(refs), traps, trunc, dict(kinds), os.path.getsize(path)

if __name__=='__main__':
    t0=time.time()
    print("Faz 1: fonksiyon kumesi kesfediliyor...")
    funcs = discover_all()
    print(f"  {len(funcs)} fonksiyon  ({time.time()-t0:.0f}s)")

    chunks=[(i, funcs[i::NSHARD]) for i in range(NSHARD)]
    hk=os.environ.get('HOOKS',''); wt=os.environ.get('WATCH','')
    if OVERRIDES: print(f"  [override: {[hex(a) for a in sorted(OVERRIDES)]}]")
    print(f"\nFaz 2: {NSHARD} shard, {mp.cpu_count()} cekirdek ile uretiliyor..."
          + (f"  [kanca: {hk}]" if hk else "")
          + ("  [KAPSAMA olcumu]" if os.environ.get('COVERAGE','')=='1' else "") + ("  [WATCHPOINT enstrumantasyonu]" if wt=='1' else ""))
    t1=time.time()
    defined=set(); refs=set(); traps=0; trunc=0; kinds=collections.Counter(); bytes_=0
    with mp.Pool(mp.cpu_count(), initializer=_init) as pool:
        for n,(idx,d,r,tp,tr,kk,sz) in enumerate(pool.imap_unordered(_shard, chunks)):
            defined |= set(d); refs |= set(r); traps+=tp; trunc+=tr
            kinds.update(kk); bytes_+=sz
            if (n+1)%16==0: print(f"  {n+1}/{NSHARD} shard  ({time.time()-t1:.0f}s)")
    print(f"  uretim bitti ({time.time()-t1:.0f}s)")

    missing = sorted(refs - defined)
    print(f"\n  tanimli: {len(defined)}   referans verilen: {len(refs)}   EKSIK: {len(missing)}")
    if missing:
        with open(f"{OUT}/stubs.c",'w') as f:
            f.write('#include "cpu.h"\n#include "decls.h"\n\n')
            for a in missing:
                f.write(f"void sub_{a:08x}(CPU *c){{ unsupported_insn(c,0x{a:x}u,\"stub\"); POP32(c->eip); }}\n")
        print(f"  -> stubs.c ({len(missing)} stub)")

    allf = sorted(defined | set(missing))
    with open(f"{OUT}/decls.h",'w') as f:
        f.write("#ifndef DECLS_H\n#define DECLS_H\n#include \"cpu.h\"\n")
        f.write("void dispatch_indirect(CPU*,uint32_t);\nvoid unsupported_insn(CPU*,uint32_t,const char*);\n")
        f.write("void cpuid_shim(CPU*);\nuint64_t rdtsc_shim(void);\n")
        for a in allf: f.write(f"void sub_{a:08x}(CPU*);\n")
        for a in sorted(OVERRIDES): f.write(f"void sub_{a:08x}_orig(CPU*);\n")
        f.write("#endif\n")
    with open(f"{OUT}/fntab.c",'w') as f:
        f.write('#include "cpu.h"\n#include "decls.h"\ntypedef void(*guest_fn)(CPU*);\n')
        f.write(f"const uint32_t g_fn_count = {len(allf)};\n")
        f.write("const uint32_t g_fn_addr[] = {\n")
        for i in range(0,len(allf),8): f.write("  "+",".join(f"0x{a:08x}u" for a in allf[i:i+8])+",\n")
        f.write("};\nconst guest_fn g_fn_ptr[] = {\n")
        for i in range(0,len(allf),4): f.write("  "+",".join(f"sub_{a:08x}" for a in allf[i:i+4])+",\n")
        f.write("};\n")
    print(f"\n=== URETIM OZETI ({time.time()-t0:.0f}s) ===")
    print(f"  fonksiyon        : {len(allf)}")
    print(f"  C kaynak boyutu  : {bytes_/1048576:.1f} MB ({NSHARD} shard)")
    print(f"  trap (desteklenmeyen komut) : {traps}")
    print(f"  kesilmis dallanma hedefi    : {trunc}")
    print("\n  en sik trap nedenleri:")
    for k,v in collections.Counter(kinds).most_common(12): print(f"    {k:16s} {v}")
