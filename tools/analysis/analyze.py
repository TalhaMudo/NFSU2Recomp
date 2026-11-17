#!/usr/bin/env python3
"""SPEED2.EXE static analysis: sections, unnamed section identity, recursive-descent code coverage."""
import sys, collections
import pefile
from capstone import *
from capstone.x86 import *

EXE = sys.argv[1]
pe = pefile.PE(EXE, fast_load=False)
IB = pe.OPTIONAL_HEADER.ImageBase
EP = IB + pe.OPTIONAL_HEADER.AddressOfEntryPoint

print(f"ImageBase 0x{IB:08x}   EntryPoint 0x{EP:08x}")
print(f"Subsystem {pe.OPTIONAL_HEADER.Subsystem}  DllChar 0x{pe.OPTIONAL_HEADER.DllCharacteristics:04x}")
print("\n=== SECTIONS ===")
secs = []
for s in pe.sections:
    nm = s.Name.rstrip(b'\x00').decode('latin1') or '(unnamed)'
    va = IB + s.VirtualAddress
    secs.append((nm, va, s.Misc_VirtualSize, s.get_data()))
    print(f"  {nm:12s} VA 0x{va:08x}  vsize {s.Misc_VirtualSize:8d}  rawsize {s.SizeOfRawData:8d}  char 0x{s.Characteristics:08x}")

# is there a .reloc?
print("\n.reloc present:", any(n=='.reloc' for n,_,_,_ in secs))

# first bytes of the unnamed section
for nm,va,vs,data in secs:
    if nm == '(unnamed)':
        print(f"\n=== UNNAMED SECTION 0x{va:08x} first 96 bytes ===")
        print(' '.join(f'{b:02x}' for b in data[:96]))
        printable = bytes(c if 32<=c<127 else 46 for c in data[:400])
        print("ASCII:", printable.decode('latin1'))

TEXT = next(x for x in secs if x[0]=='.text')
tname, TVA, TSZ, TDATA = TEXT
TEND = TVA + TSZ
print(f"\n.text: 0x{TVA:08x} - 0x{TEND:08x}  ({TSZ} bytes)")

md = Cs(CS_ARCH_X86, CS_MODE_32)
md.detail = True

def rd(va, n):
    for nm,sva,vs,data in secs:
        if sva <= va < sva+len(data):
            off = va - sva
            return data[off:off+n]
    return b''

covered = bytearray(TSZ)          # 1 = decoded as code
funcs = set()
indirect_calls = 0
indirect_jmps  = 0
bad_insn = 0
jumptable_hits = 0

def in_text(va): return TVA <= va < TEND

# --- seeds ---
seeds = collections.deque()
seen  = set()
def seed(va, isfunc=False):
    if in_text(va) and va not in seen:
        seen.add(va); seeds.append(va)
        if isfunc: funcs.add(va)

seed(EP, True)

# 4-byte values in .data/.rdata pointing into .text (function pointers, vtables)
import struct
ptr_seeds = 0
for nm,sva,vs,data in secs:
    if nm in ('.data', '.rdata', '(unnamed)'):
        for off in range(0, len(data)-4, 4):
            v = struct.unpack_from('<I', data, off)[0]
            if in_text(v):
                b = rd(v, 3)
                # typical x86 function prologues
                if b[:3] in (b'\x55\x8b\xec', b'\x53\x8b\xdc', b'\x56\x8b\xf1') or b[:1] in (b'\x55', b'\x53', b'\x56', b'\x57', b'\x83', b'\x81', b'\x8b', b'\x6a', b'\xa1', b'\xb8'):
                    if v not in seen:
                        ptr_seeds += 1
                    seed(v, True)
print(f"found {ptr_seeds} function-pointer seeds in the data sections")

# --- recursive descent ---
while seeds:
    va = seeds.popleft()
    while True:
        if not in_text(va): break
        off = va - TVA
        if off >= TSZ or covered[off]: break
        code = TDATA[off:off+16]
        if not code: break
        try:
            insn = next(md.disasm(code, va))
        except StopIteration:
            bad_insn += 1; break
        for i in range(insn.size):
            if off+i < TSZ: covered[off+i] = 1
        g = insn.group
        m = insn.mnemonic
        op = insn.operands[0] if insn.operands else None
        if m == 'call':
            if op is not None and op.type == X86_OP_IMM:
                seed(op.imm, True)
            else:
                indirect_calls += 1
            va = insn.address + insn.size
            continue
        if m in ('jmp','ljmp'):
            if op is not None and op.type == X86_OP_IMM:
                va = op.imm; continue
            else:
                indirect_jmps += 1
                # jump table heuristic:  jmp dword ptr [reg*4 + TABLE]
                if op is not None and op.type == X86_OP_MEM and op.mem.index != 0 and op.mem.scale == 4 and op.mem.disp:
                    tbl = op.mem.disp & 0xffffffff
                    n = 0
                    while n < 512:
                        raw = rd(tbl + 4*n, 4)
                        if len(raw) < 4: break
                        tgt = struct.unpack('<I', raw)[0]
                        if not in_text(tgt): break
                        seed(tgt); n += 1
                    if n: jumptable_hits += 1
                break
        if m.startswith('j'):   # conditional branch
            if op is not None and op.type == X86_OP_IMM: seed(op.imm)
            va = insn.address + insn.size; continue
        if m in ('ret','retf','iret','iretd','hlt','ud2'):
            break
        if m == 'int3':
            break
        va = insn.address + insn.size

cov = sum(covered)
print("\n=== COVERAGE (recursive descent) ===")
print(f"  decoded code      : {cov:9d} / {TSZ} bytes  = {100.0*cov/TSZ:.2f} %")
print(f"  functions found   : {len(funcs)}")
print(f"  INDIRECT calls (call reg/[mem]) : {indirect_calls}")
print(f"  INDIRECT jumps (jmp reg/[mem])  : {indirect_jmps}")
print(f"  decoded jump tables             : {jumptable_hits}")
print(f"  invalid instructions            : {bad_insn}")

# largest uncovered gaps
gaps=[]; i=0
while i < TSZ:
    if not covered[i]:
        j=i
        while j<TSZ and not covered[j]: j+=1
        gaps.append((j-i, TVA+i)); i=j
    else: i+=1
gaps.sort(reverse=True)
print(f"\n  total number of gaps: {len(gaps)}")
print("  largest 10 uncovered blocks:")
for sz,va in gaps[:10]:
    print(f"    0x{va:08x}  {sz} bytes")
