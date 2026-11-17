#!/usr/bin/env python3
"""Reloc-assisted recursive descent: measuring real code coverage."""
import sys, struct, collections
import pefile
from capstone import *
from capstone.x86 import *

pe = pefile.PE(sys.argv[1]); IB = pe.OPTIONAL_HEADER.ImageBase
EP = IB + pe.OPTIONAL_HEADER.AddressOfEntryPoint
secs=[]
for s in pe.sections:
    nm=s.Name.rstrip(b'\x00').decode('latin1').strip() or '<none>'
    secs.append((nm, IB+s.VirtualAddress, s.Misc_VirtualSize, s.get_data()))
TVA,TSZ,TDATA = next((va,vs,d) for nm,va,vs,d in secs if nm=='.text')
TEND=TVA+TSZ
def in_text(a): return TVA<=a<TEND
def rd32(a):
    for nm,va,vs,d in secs:
        if va<=a<va+len(d)-3: return struct.unpack_from('<I',d,a-va)[0]
    return None

relocs=[int(x,16) for x in open('out/relocs.txt')]
relocset=set(relocs)
# values at reloc locations = absolute addresses
targets=[v for a in relocs if (v:=rd32(a)) is not None]
code_ptrs=sorted({v for v in targets if in_text(v)})
print(f".text targets from relocs: {len(code_ptrs)} unique addresses")

md=Cs(CS_ARCH_X86,CS_MODE_32); md.detail=True
covered=bytearray(TSZ); funcs=set(); ind_call=0; ind_jmp=0; badi=0
seen=set(); q=collections.deque()
def seed(a,f=False):
    if in_text(a) and a not in seen:
        seen.add(a); q.append(a)
    if f and in_text(a): funcs.add(a)

seed(EP,True)
for v in code_ptrs: seed(v,True)

def run():
    global ind_call, ind_jmp, badi
    while q:
        va=q.popleft()
        while True:
            if not in_text(va): break
            off=va-TVA
            if off>=TSZ or covered[off]: break
            try: insn=next(md.disasm(TDATA[off:off+16],va))
            except StopIteration: badi+=1; break
            for i in range(insn.size):
                if off+i<TSZ: covered[off+i]=1
            m=insn.mnemonic; ops=insn.operands; op=ops[0] if ops else None
            if m=='call':
                if op is not None and op.type==X86_OP_IMM: seed(op.imm,True)
                else: ind_call+=1
                va=insn.address+insn.size; continue
            if m in ('jmp','ljmp'):
                if op is not None and op.type==X86_OP_IMM:
                    va=op.imm; continue
                ind_jmp+=1
                if op is not None and op.type==X86_OP_MEM and op.mem.scale==4 and op.mem.disp:
                    tbl=op.mem.disp&0xffffffff; n=0
                    while n<1024 and (tbl+4*n) in relocset:
                        t=rd32(tbl+4*n)
                        if t is None or not in_text(t): break
                        seed(t); n+=1
                break
            if m.startswith('j'):
                if op is not None and op.type==X86_OP_IMM: seed(op.imm)
                va=insn.address+insn.size; continue
            if m in ('ret','retf','iret','iretd','hlt','ud2','int3'): break
            va=insn.address+insn.size
run()

cov=sum(covered)
print(f"\n=== PASS 1 (reloc assisted) ===")
print(f"  coverage: {cov}/{TSZ} = {100.0*cov/TSZ:.2f}%   functions: {len(funcs)}")

# --- PASS 2: is there embedded DATA in the gaps? (jump table / string / alignment padding) ---
gaps=[];i=0
while i<TSZ:
    if not covered[i]:
        j=i
        while j<TSZ and not covered[j]: j+=1
        gaps.append((TVA+i,j-i)); i=j
    else: i+=1

pad=0; jt_bytes=0; jt_n=0; strb=0; unknown=[]
for va,sz in gaps:
    blk=TDATA[va-TVA:va-TVA+sz]
    # alignment padding: repeated CC/90/00
    if all(b in (0xCC,0x90,0x00) for b in blk): pad+=sz; continue
    # jump table inside .text: a run of consecutive relocated DWORDs
    if sz>=8 and va%4==0 and all((va+k) in relocset for k in range(0,min(sz,16),4)):
        jt_bytes+=sz; jt_n+=1; continue
    if sum(1 for b in blk if 32<=b<127 or b in (0,9,10,13))/max(len(blk),1) > 0.92:
        strb+=sz; continue
    unknown.append((sz,va))

print(f"\n=== BREAKDOWN OF THE {TSZ-cov} UNCOVERED BYTES ===")
print(f"  alignment padding (CC/90/00)  : {pad:8d} bytes")
print(f"  jump tables in .text (reloc'd): {jt_bytes:8d} bytes ({jt_n} tables)")
print(f"  embedded string/data          : {strb:8d} bytes")
unk=sum(s for s,_ in unknown)
print(f"  UNCLASSIFIABLE                : {unk:8d} bytes ({len(unknown)} blocks)")
eff=cov+pad+jt_bytes+strb
print(f"\n  ==> EXPLAINED TOTAL: {eff}/{TSZ} = {100.0*eff/TSZ:.2f}%")
print(f"  ==> real uncertainty: {100.0*unk/TSZ:.2f}%")
unknown.sort(reverse=True)
print("\n  largest 12 unclassified blocks:")
for sz,va in unknown[:12]: print(f"    0x{va:08x}  {sz} bytes")
print(f"\n  INDIRECT calls: {ind_call}   INDIRECT jumps: {ind_jmp}   invalid instructions: {badi}")
