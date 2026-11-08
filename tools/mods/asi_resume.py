#!/usr/bin/env python3
"""Find where an ASI plugin's code jumps back into the game ("resume points").

    asi_resume.py <out/ul/plugin.dll> <SPEED2.EXE> <out/ul/resume.txt>

A code cave in the plugin returns into the MIDDLE of a game function:

    push 0x513CDA ; retn              (lifted as a jump with PUSHRET=1)
    jmp  dword ptr [DoLinkLPToTrunk]  (a DWORD in the plugin's .data = 0x625401)

The lifted exe only has entry points at function starts, so every such address
must become an extra entry (genall.py EXTRA_FUNCS): lifted like a switch-table
target, it runs the rest of the game function from there.

Found statically: `push imm32` directly followed by `ret`, and `jmp [abs32]`
whose dword in the plugin image holds a game .text address.  Anything this
misses shows up at run time as "[UNRESOLVED INDIRECT CALL]" with the address.
"""
import sys, struct
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_REG_INVALID

dll, exe, out = sys.argv[1:4]
pd = pefile.PE(dll); pe = pefile.PE(exe)
DB = pd.OPTIONAL_HEADER.ImageBase; EB = pe.OPTIONAL_HEADER.ImageBase
etext = next(s for s in pe.sections if s.Name.rstrip(b'\0') == b'.text')
ELO = EB + etext.VirtualAddress; EHI = ELO + etext.Misc_VirtualSize
def in_exe(a): return ELO <= a < EHI
img = pd.get_memory_mapped_image()
def d32(a):
    o = a - DB
    return struct.unpack_from('<I', img, o)[0] if 0 <= o <= len(img) - 4 else None

dtext = next(s for s in pd.sections if s.Name.rstrip(b'\0') == b'.text')
md = Cs(CS_ARCH_X86, CS_MODE_32); md.detail = True; md.skipdata = True
code = dtext.get_data(); base = DB + dtext.VirtualAddress
found = {}
prev = None
for i in md.disasm(code, base):
    if i.mnemonic == 'ret' and not i.operands and prev is not None and prev.mnemonic == 'push' \
            and prev.operands and prev.operands[0].type == X86_OP_IMM and in_exe(prev.operands[0].imm & 0xffffffff):
        found.setdefault(prev.operands[0].imm & 0xffffffff, set()).add(f"push-ret@{prev.address:#x}")
    if i.mnemonic == 'jmp' and i.operands and i.operands[0].type == X86_OP_MEM:
        mm = i.operands[0].mem
        if mm.base == X86_REG_INVALID and mm.index == X86_REG_INVALID:
            v = d32(mm.disp & 0xffffffff)
            if v is not None and in_exe(v):
                found.setdefault(v, set()).add(f"jmp[{mm.disp & 0xffffffff:#x}]@{i.address:#x}")
    prev = i
with open(out, 'w') as f:
    for a in sorted(found):
        f.write(f"{a:08x}  # {' '.join(sorted(found[a]))}\n")
print(f"{len(found)} resume points -> {out}")
