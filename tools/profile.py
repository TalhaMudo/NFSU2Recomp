#!/usr/bin/env python3
import sys, struct, collections, re
import pefile
from capstone import *
from capstone.x86 import *
pe=pefile.PE(sys.argv[1]); IB=pe.OPTIONAL_HEADER.ImageBase
EP=IB+pe.OPTIONAL_HEADER.AddressOfEntryPoint
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
rs=set(relocs)
md=Cs(CS_ARCH_X86,CS_MODE_32); md.detail=True
cov=bytearray(TSZ); seen=set(); q=collections.deque()
def seed(a):
    if in_text(a) and a not in seen: seen.add(a); q.append(a)
seed(EP)
for a in relocs:
    v=rd32(a)
    if v is not None and in_text(v): seed(v)
mn=collections.Counter(); grp=collections.Counter(); total=0
X87=set('fld fst fstp fadd faddp fsub fsubp fsubr fsubrp fmul fmulp fdiv fdivp fdivr fdivrp fcom fcomp fcompp fcomi fcomip fucom fucomp fucompp fucomi fucomip fchs fabs fsqrt fsin fcos fptan fpatan fyl2x fyl2xp1 f2xm1 fscale frndint fxch fildl fild fistp fist fisttp fnstsw fnstcw fldcw fldz fld1 fldpi fldl2e fldl2t fldlg2 fldln2 ffree finit fninit fclex fnclex fprem fprem1 fxam ftst fincstp fdecstp fcmovb fcmove fcmovbe fcmovu fcmovnb fcmovne fcmovnbe fcmovnu fisub fiadd fimul fidiv ficom ficomp fbld fbstp fsave fnsave frstor fstcw fstsw fxsave fxrstor'.split())
SSE_PFX=('movap','movup','movss','movsd','adds','subs','muls','divs','sqrts','cvt','ucomis','comis','shufp','unpck','andp','orp','xorp','maxs','mins','rcps','rsqrts','pshuf','punpck','paddd','psubd','pmullw','movdqa','movdqu','movq','movd','pand','por','pxor','psll','psrl','emms','maskmov','movnt','prefetch','ldmxcsr','stmxcsr','rsqrt','haddp','addsubp')
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
        m=i.mnemonic; mn[m]+=1; total+=1
        if m in X87 or (m.startswith('f') and m not in ('fs','fxch')): grp['x87']+=1
        elif any(m.startswith(p) for p in SSE_PFX): grp['SSE/MMX']+=1
        ops=i.operands; op=ops[0] if ops else None
        if m=='call':
            if op is not None and op.type==X86_OP_IMM: seed(op.imm)
            else: grp['DOLAYLI_CALL']+=1
            va=i.address+i.size; continue
        if m in ('jmp','ljmp'):
            if op is not None and op.type==X86_OP_IMM: va=op.imm; continue
            grp['DOLAYLI_JMP']+=1
            if op is not None and op.type==X86_OP_MEM and op.mem.scale==4 and op.mem.disp:
                t=op.mem.disp&0xffffffff; n=0
                while n<1024 and (t+4*n) in rs:
                    v=rd32(t+4*n)
                    if v is None or not in_text(v): break
                    seed(v); n+=1
            break
        if m.startswith('j'):
            if op is not None and op.type==X86_OP_IMM: seed(op.imm)
            va=i.address+i.size; continue
        if m in ('ret','retf','iret','iretd','hlt','ud2','int3'): break
        va=i.address+i.size
print(f"toplam cozulen komut: {total}\n")
print("=== EN SIK 25 KOMUT ===")
for m,c in mn.most_common(25): print(f"  {m:10s} {c:8d}  {100.0*c/total:5.2f}%")
print(f"\n=== KRITIK KATEGORILER ===")
for k in ('x87','SSE/MMX','DOLAYLI_CALL','DOLAYLI_JMP'):
    print(f"  {k:14s} {grp[k]:8d}  {100.0*grp[k]/total:5.2f}%")
print(f"\n  benzersiz komut cesidi (opcode): {len(mn)}")
risky=[m for m in mn if m in ('in','out','cpuid','rdtsc','cli','sti','lock','xchg','cmpxchg','cmpxchg8b','lgdt','lidt','sysenter','int','into','rdpmc','wrmsr','rdmsr','sgdt','sidt','smsw','arpl','bound')]
print(f"  ayricalikli/ozel komutlar: {sorted((m,mn[m]) for m in risky)}")
