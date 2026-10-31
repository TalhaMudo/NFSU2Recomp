#!/usr/bin/env python3
"""Translate all 23k functions to C, split them into shards, generate the function table."""
import sys, os, struct, collections, time, multiprocessing as mp
sys.path.insert(0,'tools')
import pefile
from capstone import *
from capstone.x86 import *
from lift import Lifter, Unsupported, XPatchError
import xpatch

EXE = sys.argv[1]
NSHARD = int(sys.argv[2]) if len(sys.argv)>2 else 128
# GENOUT: output directory (default gen/all); COMMENTS=1: every x86 instruction is
# written as a comment (same code; for instruction-level profiling, see tools/lift_profile.py)
OUT = os.environ.get('GENOUT', 'gen/all')
os.makedirs(OUT, exist_ok=True)

# ---------- Phase 1: closed function set ----------
def discover_all():
    L=Lifter(EXE); pe=pefile.PE(EXE); IB=pe.OPTIONAL_HEADER.ImageBase
    EP=IB+pe.OPTIONAL_HEADER.AddressOfEntryPoint
    def rd32(a):
        for nm,va,vs,d in L.secs:
            if va<=a<va+len(d)-3: return struct.unpack_from('<I',d,a-va)[0]
    funcs={EP}
    for a in (int(x,16) for x in open(os.environ.get('RELOCS','out/relocs.txt'))):
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

# ---------- Phase 2: parallel generation ----------
_L=None
# INLINE=<n>: "leaf" functions that make no calls and have at most n x86
# instructions are put into every shard as static inline (always_inline) copies,
# direct calls are redirected to them.  The fixed cost per call (saving entry/exit
# registers, writing c-> fields to memory and reading them back) is as large as
# the work itself for leaf calls.  The original sub_X definitions stay as they are
# (fntab, indirect calls).
INLINE_MAX = int(os.environ.get('INLINE','0') or 0)
_INL = {}
import re as _re
_INL_CALL = _re.compile(r'\bsub_([0-9a-f]{8})\(c\)')
_INL_YASAK = ('dispatch_indirect', 'unsupported_insn', 'cpuid_shim', 'guest_hook', 'rdtsc_shim', 'cov_hit')
OVERRIDES = {int(x,16) for x in os.environ.get('OVERRIDE','').replace('0x','').split(',') if x.strip()}

# MEMARG=1: the guest memory base as a PARAMETER.  Every lifted function becomes
#   void sub_X_m(CPU *restrict c, uint8_t *restrict _mb)
# lifted code calls each other through the _m version, the g_mem macros go to _mb.
# The externally visible void sub_X(CPU*) stays as a thin wrapper (fntab, indirect
# calls, runtime -> lift calls, overrides don't change).
# Why: g_mem is a pointer read from memory and c is "escaped" because it is passed
# to calls; the compiler has to reread the c-> fields (and with
# -fno-strict-aliasing g_mem itself) after every write to guest memory.  Two
# separate noalias parameters definitely don't alias in LLVM (no escape analysis
# needed).
# For correctness this output must be compiled with -fno-strict-aliasing: lifted
# code accesses the same guest address with different types (MR32 write, MRF32 read).
MEMARG = os.environ.get('MEMARG','') == '1'
_MA_HDR = _re.compile(r'void sub_([0-9a-f]{8})(_orig)?\(CPU \*(?:restrict )?c\) ?\{')
def _memarg(code):
    h = _MA_HDR.search(code)
    if not h: return code
    name = f"sub_{h.group(1)}{h.group(2) or ''}"
    body = _INL_CALL.sub(lambda m: f"sub_{m.group(1)}_m(c, _mb)", code[h.end():])
    return (code[:h.start()] + f"void {name}_m(CPU *restrict c, uint8_t *restrict _mb) {{" + body
            + f"\nvoid {name}(CPU *c) {{ {name}_m(c, _gm()); }}\n")
_MA_PRO = ('static inline uint8_t *_gm(void){ return g_mem; }\n'
           '#define g_mem _mb\n')

def _init(inl=None):
    global _L, _INL
    if inl: _INL = inl
    hooks = [int(x,16) for x in os.environ.get('HOOKS','').replace('0x','').split(',') if x.strip()]
    watch = os.environ.get('WATCH','') == '1'
    cov   = os.environ.get('COVERAGE','') == '1'
    _L = Lifter(EXE, comments=os.environ.get('COMMENTS','') == '1', trap=True, hooks=hooks, watch=watch)
    _L.cov = cov
    _L.xp = xpatch.load(os.environ.get('XPATCH'))   # LOCAL PATCH: run-time switchable patches

def _inlprobe(a):
    """leaf + small + not overridden -> (address, code) or (address, None)"""
    if a in OVERRIDES: return a, None
    _L.traps = 0
    try:
        code, tg = _L.lift(a)
    except Exception:
        return a, None
    m = _re.search(r'\((\d+) instructions,', code)
    if not m or int(m.group(1)) > INLINE_MAX: return a, None
    if _INL_CALL.search(code.split('{', 1)[1]) or any(y in code for y in _INL_YASAK): return a, None
    if _L.traps: return a, None
    return a, code

def _shard(args):
    idx, addrs = args
    L=_L; parts=[]; defined=[]; refs=set(); traps=0; trunc=0; kinds=collections.Counter(); used=set()
    for a in addrs:
        L.traps=0; L.truncated=0
        try:
            code,tg = L.lift(a)
        except XPatchError:
            raise
        except Exception as e:
            fb = f"void sub_{a:08x}(CPU *c){{ unsupported_insn(c,0x{a:x}u,\"lift-error\"); POP32(c->eip); }}"
            parts.append(_memarg(fb) if MEMARG else fb)
            defined.append(a); kinds[f"<lift:{type(e).__name__}>"]+=1; continue
        if a in OVERRIDES:
            code = code.replace(f"void sub_{a:08x}(CPU *restrict c) {{",
                                f"void sub_{a:08x}_orig(CPU *restrict c) {{")
        if MEMARG:
            code = _memarg(code)
        if _INL:
            hdr, body = code.split('{', 1)
            body = _INL_CALL.sub(lambda m: (used.add(int(m.group(1), 16)) or f"inl_{m.group(1)}(c)")
                                 if int(m.group(1), 16) in _INL else m.group(0), body)
            code = hdr + '{' + body
        parts.append(code); defined.append(a)
        refs |= {t for t in tg if L.in_text(t)}
        traps += L.traps; trunc += L.truncated
        kinds.update(L.trap_kinds); L.trap_kinds.clear()
    path=f"{OUT}/shard_{idx:04d}.c"
    with open(path,'w') as f:
        f.write('#include "cpu.h"\n#include "decls.h"\n#include <math.h>\n\n')
        if MEMARG: f.write(_MA_PRO + '\n')
        for x in sorted(used):
            f.write(_INL[x].replace(f"void sub_{x:08x}(CPU *restrict c) {{",
                    f"static inline __attribute__((always_inline)) void inl_{x:08x}(CPU *restrict c) {{") + "\n")
        f.write("\n".join(parts))
        f.write("\n")
    return idx, defined, sorted(refs), traps, trunc, dict(kinds), os.path.getsize(path), sorted(L.xp_hits)

if __name__=='__main__':
    t0=time.time()
    print("Phase 1: discovering the function set...")
    funcs = discover_all()
    print(f"  {len(funcs)} functions  ({time.time()-t0:.0f}s)")

    chunks=[(i, funcs[i::NSHARD]) for i in range(NSHARD)]
    hk=os.environ.get('HOOKS',''); wt=os.environ.get('WATCH','')
    if OVERRIDES: print(f"  [override: {[hex(a) for a in sorted(OVERRIDES)]}]")
    print(f"\nFaz 2: {NSHARD} shard, {mp.cpu_count()} cores..."
          + (f"  [hook: {hk}]" if hk else "")
          + ("  [COVERAGE measurement]" if os.environ.get('COVERAGE','')=='1' else "") + ("  [WATCHPOINT instrumentation]" if wt=='1' else ""))
    t1=time.time()
    inl = {}
    if INLINE_MAX:
        with mp.Pool(mp.cpu_count(), initializer=_init) as pool:
            for a, code in pool.imap_unordered(_inlprobe, funcs, chunksize=64):
                if code: inl[a] = code
        print(f"  [inline: {len(inl)} leaf functions <= {INLINE_MAX} instructions]  ({time.time()-t1:.0f}s)")
    defined=set(); refs=set(); traps=0; trunc=0; kinds=collections.Counter(); bytes_=0
    with mp.Pool(mp.cpu_count(), initializer=_init, initargs=(inl,)) as pool:
        xhits=set()
        for n,(idx,d,r,tp,tr,kk,sz,xh) in enumerate(pool.imap_unordered(_shard, chunks)):
            defined |= set(d); refs |= set(r); traps+=tp; trunc+=tr; xhits |= set(xh)
            kinds.update(kk); bytes_+=sz
            if (n+1)%16==0: print(f"  {n+1}/{NSHARD} shard  ({time.time()-t1:.0f}s)")
    print(f"  generation done ({time.time()-t1:.0f}s)")

    # LIFT_MISSING=1: call targets in .text that discovery missed (its linear scan
    # can stop on bytes an earlier, misaligned scan already covered) are lifted
    # too, in extra shards, instead of becoming "stub" traps.  Repeated until no
    # new target appears.  Off by default so the European output stays as it is.
    if os.environ.get('LIFT_MISSING') == '1':
        _init(inl); extra = NSHARD
        while True:
            more = sorted(a for a in refs - defined if _L.in_text(a))
            if not more: break
            idx, d, r, tp, tr, kk, sz, xh = _shard((extra, more)); xhits |= set(xh)
            print(f"  [lift-missing] shard_{extra:04d}: {', '.join(hex(a) for a in more)}")
            defined |= set(d); refs |= set(r); traps += tp; trunc += tr; kinds.update(kk); bytes_ += sz
            extra += 1
    # LOCAL PATCH: xpatch header + a check that every site was found in some function
    spec = xpatch.load(os.environ.get('XPATCH'))
    with open(f"{OUT}/xpatch.h",'w') as f:
        f.write(spec.header() if spec else "#pragma once\n#define XP_COUNT 0\n#define XPV_COUNT 0\n"
                "static const char *const xp_names[] = { 0 };\nstatic const char *const xpv_names[] = { 0 };\n")
    if spec:
        hit_names = {n for n, a in xhits}
        lost = [n for n in spec.names if n not in hit_names]
        print(f"  [xpatch: {len(spec.names)} switches, {len(xhits)} sites lifted]")
        for n, a in sorted(xhits, key=lambda t: t[1]): print(f"    {a:#x}  {n}")
        if lost: raise SystemExit(f"xpatch: no site found for {lost}")
    missing = sorted(refs - defined)
    print(f"\n  defined: {len(defined)}   referenced: {len(refs)}   MISSING: {len(missing)}")
    if missing or (MEMARG and OVERRIDES):
        with open(f"{OUT}/stubs.c",'w') as f:
            f.write('#include "cpu.h"\n#include "decls.h"\n\n')
            for a in missing:
                f.write(f"void sub_{a:08x}(CPU *c){{ unsupported_insn(c,0x{a:x}u,\"stub\"); POP32(c->eip); }}\n")
            if MEMARG:   # lifted code calls everyone through _m: a bridge for the missing ones and overrides
                for a in sorted(set(missing) | OVERRIDES):
                    f.write(f"void sub_{a:08x}_m(CPU *restrict c, uint8_t *restrict _mb){{ (void)_mb; sub_{a:08x}(c); }}\n")
        print(f"  -> stubs.c ({len(missing)} stub)")

    allf = sorted(defined | set(missing))
    with open(f"{OUT}/decls.h",'w') as f:
        f.write("#ifndef DECLS_H\n#define DECLS_H\n#include \"cpu.h\"\n")
        f.write("void dispatch_indirect(CPU*,uint32_t);\nvoid unsupported_insn(CPU*,uint32_t,const char*);\n")
        f.write("void cpuid_shim(CPU*);\nuint64_t rdtsc_shim(void);\n")
        f.write("extern unsigned char g_xpatch[];\nextern unsigned int g_xpval[];\n")   # LOCAL PATCH: xpatch
        for a in allf: f.write(f"void sub_{a:08x}(CPU*);\n")
        for a in sorted(OVERRIDES): f.write(f"void sub_{a:08x}_orig(CPU*);\n")
        if MEMARG:
            for a in allf: f.write(f"void sub_{a:08x}_m(CPU *restrict, uint8_t *restrict);\n")
            for a in sorted(OVERRIDES): f.write(f"void sub_{a:08x}_orig_m(CPU *restrict, uint8_t *restrict);\n")
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
    print(f"  functions        : {len(allf)}")
    print(f"  C source size    : {bytes_/1048576:.1f} MB ({NSHARD} shard)")
    print(f"  traps (unsupported instruction) : {traps}")
    print(f"  truncated branch targets        : {trunc}")
    print("\n  most common trap reasons:")
    for k,v in collections.Counter(kinds).most_common(12): print(f"    {k:16s} {v}")
