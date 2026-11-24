#!/usr/bin/env python3
"""DIFFERENTIAL test for x86 semantics: the lifter against independent oracles.

The same instruction bytes are run in up to four places:
  1) rosetta : compiled as x86-64, runs under Rosetta 2 (Apple's translator)
  2) unicorn : run by Unicorn (QEMU's translator) in true 32-bit mode
  3) hw      : the Rosetta oracle program run natively on x86-64 hardware
               (--export here, run there, --hw here; see below)
  4) guest   : translated to C by RawLifter, runs on ARM64 -> our output
The output (6 GPRs + flags + 128 bytes of memory) is compared, guest against
every oracle and the oracles against each other.

None of 1) and 2) is a CPU: both are translators written independently of each
other and of the lifter, so agreement with both is strong evidence but not
proof.  3) is the real thing, for the 64-bit-safe subset.

Encodings that carry the same meaning in 64-bit mode are used for 1) and 3):
  - eax..esi only (no REX, no r8+)
  - memory is always N(%rdi) / [edi+N]
  - edi and esp are NOT USED as destinations
Templates marked m32=True are 32-bit-only encodings (one-byte inc/dec, pushad,
aaa..., 16-bit addressing); only unicorn can check those.

usage:
  fuzzisa.py [n] [seed]                 run (oracles: ORACLES=rosetta,unicorn)
  fuzzisa.py [n] [seed] --export DIR    also write DIR/oracle.c + DIR/README.txt
                                        for a run on x86-64 hardware
  fuzzisa.py [n] [seed] --hw FILE       also compare against the output of that
                                        run (same n and seed)
Every run writes <FUZZDIR>/coverage.json (which instruction was checked by
which oracle), read by tools/verify/isacoverage.py.
"""
import os, sys, random, struct, subprocess, hashlib, json
import os as _os; sys.path.insert(0, _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '..', 'lifter'))
from lift import RawLifter, Unsupported

SCRATCH = 0x00A00000          # address of the memory buffer in the guest
MEMSZ   = 128
BASE    = 0x00400000

R32 = ['%eax','%ecx','%edx','%ebx','%ebp','%esi']
R16 = ['%ax','%cx','%dx','%bx','%bp','%si']
R8  = ['%al','%cl','%dl','%bl','%ah','%ch']      # ah/ch: high byte forms
XMM = ['%xmm0','%xmm1','%xmm2','%xmm3']

# flag mask: CF PF AF ZF SF OF  (+DF kept separately)
F_CF,F_PF,F_AF,F_ZF,F_SF,F_OF = 1,4,0x10,0x40,0x80,0x800
F_ALL = F_CF|F_PF|F_AF|F_ZF|F_SF|F_OF

def M(rng, lo=0, hi=96, align=4):
    off = rng.randrange(lo, hi, align)
    return f"{off}(%rdi)"

class Tmpl:
    def __init__(self, name, gen, undef=0, skipmem=False, ptr=False, m32=False):
        self.name, self.gen, self.undef = name, gen, undef
        self.skipmem, self.ptr, self.m32 = skipmem, ptr, m32

T = []
def tmpl(name, undef=0, skipmem=False, ptr=False, m32=False):
    """skipmem: the result is affected by the precision difference between 80-bit
    x87 and double (fprem, fsin...) -- the memory result isn't compared.
    m32: a 32-bit-only encoding; assembled with .code32, not run on the 64-bit
    oracles.  Use %edi (not %rdi) for memory in these."""
    def d(f): T.append(Tmpl(name, f, undef, skipmem, ptr, m32)); return f
    return d

# ---------------- integer core ----------------
ARITH = ['add','sub','adc','sbb','and','or','xor','cmp']
for op in ARITH:
    @tmpl(f"{op} r32,r32")
    def _(rng, op=op): return f"{op}l {rng.choice(R32)}, {rng.choice(R32)}"
    @tmpl(f"{op} m32,r32")
    def _(rng, op=op): return f"{op}l {M(rng)}, {rng.choice(R32)}"
    @tmpl(f"{op} r32,m32")
    def _(rng, op=op): return f"{op}l {rng.choice(R32)}, {M(rng)}"
    @tmpl(f"{op} imm32,r32")
    def _(rng, op=op): return f"{op}l ${rng.getrandbits(32)}, {rng.choice(R32)}"
    @tmpl(f"{op} imm8,r32")
    def _(rng, op=op): return f"{op}l ${rng.randrange(-128,128)}, {rng.choice(R32)}"
    @tmpl(f"{op} r16,r16")
    def _(rng, op=op): return f"{op}w {rng.choice(R16)}, {rng.choice(R16)}"
    @tmpl(f"{op} r8,r8")
    def _(rng, op=op): return f"{op}b {rng.choice(R8)}, {rng.choice(R8)}"
    @tmpl(f"{op} m8,r8")
    def _(rng, op=op): return f"{op}b {M(rng,0,96,1)}, {rng.choice(R8)}"

@tmpl("test r32,r32")
def _(rng): return f"testl {rng.choice(R32)}, {rng.choice(R32)}"
@tmpl("test imm32,r32")
def _(rng): return f"testl ${rng.getrandbits(32)}, {rng.choice(R32)}"
@tmpl("inc/dec r32")
def _(rng): return f"{rng.choice(['incl','decl'])} {rng.choice(R32)}"
@tmpl("inc/dec m32")
def _(rng): return f"{rng.choice(['incl','decl'])} {M(rng)}"
@tmpl("neg/not r32")
def _(rng): return f"{rng.choice(['negl','notl'])} {rng.choice(R32)}"
@tmpl("neg/not r8")
def _(rng): return f"{rng.choice(['negb','notb'])} {rng.choice(R8)}"

# ---------------- shifts / rotates ----------------
for op in ['shl','shr','sar','rol','ror','rcl','rcr']:
    @tmpl(f"{op} imm,r32", undef=F_OF|F_AF)
    def _(rng, op=op): return f"{op}l ${rng.randrange(0,33)}, {rng.choice(R32)}"
    @tmpl(f"{op} cl,r32", undef=F_OF|F_AF)
    def _(rng, op=op): return f"{op}l %cl, {rng.choice(R32)}"
    @tmpl(f"{op} imm,r16", undef=F_OF|F_AF)
    def _(rng, op=op): return f"{op}w ${rng.randrange(0,17)}, {rng.choice(R16)}"
    @tmpl(f"{op} imm,r8", undef=F_OF|F_AF)
    def _(rng, op=op): return f"{op}b ${rng.randrange(0,9)}, {rng.choice(R8)}"
@tmpl("shld/shrd", undef=F_OF|F_AF)
def _(rng):
    op = rng.choice(['shldl','shrdl'])
    return f"{op} ${rng.randrange(0,32)}, {rng.choice(R32)}, {rng.choice(R32)}"

# ---------------- multiply / divide ----------------
@tmpl("imul r32,r32", undef=F_SF|F_ZF|F_AF|F_PF)
def _(rng): return f"imull {rng.choice(R32)}, {rng.choice(R32)}"
@tmpl("imul imm,r32,r32", undef=F_SF|F_ZF|F_AF|F_PF)
def _(rng): return f"imull ${rng.randrange(-2000,2000)}, {rng.choice(R32)}, {rng.choice(R32)}"
@tmpl("imul m32,r32", undef=F_SF|F_ZF|F_AF|F_PF)
def _(rng): return f"imull {M(rng)}, {rng.choice(R32)}"
@tmpl("mul/imul edx:eax", undef=F_ALL)
def _(rng):
    op = rng.choice(['mull','imull'])
    return f"{op} {rng.choice(['%ecx','%ebx','%esi'])}"
@tmpl("cdq/cwde/cbw")
def _(rng): return rng.choice(['cltd','cwtl','cbtw'])

# ---------------- moves / extension ----------------
@tmpl("movzx/movsx 8->32")
def _(rng):
    op = rng.choice(['movzbl','movsbl'])
    return f"{op} {M(rng,0,96,1)}, {rng.choice(R32)}"
@tmpl("movzx/movsx 16->32")
def _(rng):
    op = rng.choice(['movzwl','movswl'])
    return f"{op} {M(rng,0,96,2)}, {rng.choice(R32)}"
@tmpl("xchg r32,r32")
def _(rng):
    a,b = rng.sample(R32,2); return f"xchgl {a}, {b}"
@tmpl("bswap r32")
def _(rng): return f"bswapl {rng.choice(R32)}"
@tmpl("lea")
def _(rng):
    # %rdi is NOT USED as a base: in the oracle it holds a 64-bit host pointer,
    # in the guest a 32-bit guest address -- the results legitimately differ.
    b = rng.choice(['%rax','%rcx','%rdx','%rbx'])
    i = rng.choice(['%rax','%rcx','%rdx','%rbx'])
    sc = rng.choice([1,2,4,8]); d = rng.randrange(-64,64)
    return f"leal {d}({b},{i},{sc}), {rng.choice(R32)}"

# ---------------- conditions ----------------
CC = ['o','no','b','ae','e','ne','be','a','s','ns','p','np','l','ge','le','g']
@tmpl("setcc")
def _(rng): return f"set{rng.choice(CC)} {rng.choice(R8[:4])}"
@tmpl("cmovcc")
def _(rng): return f"cmov{rng.choice(CC)}l {rng.choice(R32)}, {rng.choice(R32)}"

# ---------------- bit ----------------
@tmpl("bt/bts/btr/btc r32", undef=F_OF|F_SF|F_ZF|F_AF|F_PF)
def _(rng):
    op = rng.choice(['btl','btsl','btrl','btcl'])
    return f"{op} ${rng.randrange(0,32)}, {rng.choice(R32)}"
@tmpl("bsf/bsr", undef=F_OF|F_SF|F_AF|F_PF|F_CF)
def _(rng):
    op = rng.choice(['bsfl','bsrl'])
    return f"{op} {rng.choice(R32)}, {rng.choice(R32)}"

# ---------------- x87 ----------------
@tmpl("x87 arithmetic -> memory")
def _(rng):
    op = rng.choice(['fadds','fsubs','fmuls','fdivs','fsubrs','fdivrs'])
    a,b,d = 0, 8, 64
    return f"flds {a}(%rdi)\n\t{op} {b}(%rdi)\n\tfstps {d}(%rdi)"
@tmpl("x87 transcendental (80-bit difference)", skipmem=True)
def _(rng):
    op = rng.choice(['fsin','fcos','f2xm1','fptan'])
    return f"flds 0(%rdi)\n\t{op}\n\tfstps 64(%rdi)"
@tmpl("x87 unary -> memory")
def _(rng):
    # f2xm1 is only defined for |x|<=1; can't be compared with random input
    op = rng.choice(['fabs','fchs','fsqrt','frndint'])
    return f"flds 0(%rdi)\n\t{op}\n\tfstps 64(%rdi)"
@tmpl("x87 fistp RC")
def _(rng):
    cw = rng.choice([0x037f,0x077f,0x0b7f,0x0f7f])
    return (f"movw ${cw}, 100(%rdi)\n\tfldcw 100(%rdi)\n\t"
            f"flds 0(%rdi)\n\tfistpl 64(%rdi)\n\tmovw $0x037f, 100(%rdi)\n\tfldcw 100(%rdi)")
@tmpl("x87 fild -> fstp")
def _(rng):
    return "fildl 0(%rdi)\n\tfstps 64(%rdi)"
@tmpl("x87 fxch/fcom")
def _(rng):
    return ("flds 0(%rdi)\n\tflds 8(%rdi)\n\tfxch %st(1)\n\t"
            "fcomps 16(%rdi)\n\tfnstsw %ax\n\tandl $0x7f00, %eax\n\tfstps 64(%rdi)")
@tmpl("x87 fprem/fscale", skipmem=True)
def _(rng):
    op = rng.choice(['fprem','fprem1','fscale'])
    return f"flds 8(%rdi)\n\tflds 0(%rdi)\n\t{op}\n\tfstps 64(%rdi)\n\tfstp %st(0)"
@tmpl("x87 fld st(i) - stack copy")
def _(rng):
    # "fld %st(i)" must read the source BEFORE top changes.  Because this template
    # didn't exist, the FPUSH evaluation order bug went unnoticed.
    i = rng.randrange(0, 3)
    return ("flds 0(%rdi)\n\tflds 8(%rdi)\n\tflds 16(%rdi)\n\t"
            f"fld %st({i})\n\t"
            "fstps 64(%rdi)\n\tfstps 68(%rdi)\n\t"
            "fstps 72(%rdi)\n\tfstps 76(%rdi)")
@tmpl("x87 fst/fxch st(i) mixed")
def _(rng):
    i = rng.randrange(1, 3)
    op = rng.choice([f"fst %st({i})", f"fxch %st({i})", f"fld %st({i})"])
    return ("flds 0(%rdi)\n\tflds 8(%rdi)\n\tflds 16(%rdi)\n\t"
            f"{op}\n\t"
            "fstps 64(%rdi)\n\tfstps 68(%rdi)\n\tfstps 72(%rdi)")
@tmpl("x87 __ftol-like (fld st(0) + fistp qword)")
def _(rng):
    return ("flds 0(%rdi)\n\tfld %st(0)\n\t"
            "fsts 64(%rdi)\n\tfistpq 72(%rdi)\n\t"
            "fildq 72(%rdi)\n\tfsubp %st(1)\n\tfstps 68(%rdi)")
@tmpl("x87 double")
def _(rng):
    op = rng.choice(['faddl','fsubl','fmull','fdivl'])
    return f"fldl 0(%rdi)\n\t{op} 8(%rdi)\n\tfstpl 64(%rdi)"

# ---------------- SSE / SSE2 ----------------
@tmpl("sse packed float")
def _(rng):
    op = rng.choice(['addps','subps','mulps','divps','minps','maxps','andps','orps','xorps'])
    return f"movups 0(%rdi), %xmm0\n\tmovups 16(%rdi), %xmm1\n\t{op} %xmm1, %xmm0\n\tmovups %xmm0, 64(%rdi)"
@tmpl("sse scalar float")
def _(rng):
    op = rng.choice(['addss','subss','mulss','divss','minss','maxss','sqrtss'])
    return f"movups 0(%rdi), %xmm0\n\tmovups 16(%rdi), %xmm1\n\t{op} %xmm1, %xmm0\n\tmovups %xmm0, 64(%rdi)"
@tmpl("sse2 packed double")
def _(rng):
    op = rng.choice(['addpd','subpd','mulpd','divpd','minpd','maxpd'])
    return f"movupd 0(%rdi), %xmm0\n\tmovupd 16(%rdi), %xmm1\n\t{op} %xmm1, %xmm0\n\tmovupd %xmm0, 64(%rdi)"
@tmpl("sse shuffle/unpack")
def _(rng):
    op = rng.choice([f"shufps ${rng.getrandbits(8)}, %xmm1, %xmm0",
                     "unpcklps %xmm1, %xmm0", "unpckhps %xmm1, %xmm0",
                     "movlhps %xmm1, %xmm0", "movhlps %xmm1, %xmm0"])
    return f"movups 0(%rdi), %xmm0\n\tmovups 16(%rdi), %xmm1\n\t{op}\n\tmovups %xmm0, 64(%rdi)"
@tmpl("sse shufps m128 / cvtpi2ps")
def _(rng):
    op = rng.choice([f"shufps ${rng.getrandbits(8)}, 16(%rdi), %xmm0",
                     "cvtpi2ps 16(%rdi), %xmm0",
                     "movq 24(%rdi), %mm1\n\tcvtpi2ps %mm1, %xmm0\n\temms"])
    return f"movups 0(%rdi), %xmm0\n\t{op}\n\tmovups %xmm0, 64(%rdi)"
@tmpl("sse2 packed int")
def _(rng):
    op = rng.choice(['paddd','psubd','paddw','psubw','paddb','psubb','pand','por','pxor',
                     'pmullw','pminsw','pmaxsw','pavgb','pavgw','psadbw','pcmpeqd','pcmpgtd'])
    return f"movdqu 0(%rdi), %xmm0\n\tmovdqu 16(%rdi), %xmm1\n\t{op} %xmm1, %xmm0\n\tmovdqu %xmm0, 64(%rdi)"
@tmpl("sse cvt")
def _(rng):
    op = rng.choice(['cvtps2dq','cvttps2dq','cvtdq2ps','cvtpd2ps','cvtps2pd'])
    return f"movups 16(%rdi), %xmm1\n\tmovups 0(%rdi), %xmm0\n\t{op} %xmm0, %xmm1\n\tmovups %xmm1, 64(%rdi)"
@tmpl("sse cvt skaler")
def _(rng):
    op = rng.choice(['cvtsi2ssl','cvtsi2sdl'])
    return f"movups 16(%rdi), %xmm0\n\t{op} 0(%rdi), %xmm0\n\tmovups %xmm0, 64(%rdi)"
@tmpl("sse cvt -> int")
def _(rng):
    op = rng.choice(['cvttss2si','cvtss2si','cvttsd2si','cvtsd2si'])
    return f"movups 0(%rdi), %xmm0\n\t{op} %xmm0, %eax\n\tmovl %eax, 64(%rdi)"
@tmpl("sse cmp/compare")
def _(rng):
    op = rng.choice([f"cmpps ${rng.randrange(0,8)}, %xmm1, %xmm0",
                     f"cmpss ${rng.randrange(0,8)}, %xmm1, %xmm0"])
    return f"movups 0(%rdi), %xmm0\n\tmovups 16(%rdi), %xmm1\n\t{op}\n\tmovups %xmm0, 64(%rdi)"
@tmpl("ucomiss/comisd")
def _(rng):
    op = rng.choice(['ucomiss','comiss','ucomisd','comisd'])
    return (f"movups 0(%rdi), %xmm0\n\tmovups 16(%rdi), %xmm1\n\t{op} %xmm1, %xmm0")


# ---------------- string instructions (esi = source pointer) ----------------
@tmpl("movs/stos/lods", ptr=True)
def _(rng):
    op = rng.choice(['movsb','movsw','movsl','stosb','stosw','stosl','lodsb','lodsw','lodsl'])
    n  = rng.randrange(1, 5)
    d  = rng.choice(['cld','std'])
    # with std the pointers move backwards; start from the middle of the buffer
    pre = f"{d}\n\tmovl ${n}, %ecx\n\t"
    rep = 'rep ' if op.startswith(('movs','stos')) and rng.random() < 0.7 else ''
    return pre + rep + op + "\n\tcld"
@tmpl("scas/cmps", ptr=True, undef=0)
def _(rng):
    op = rng.choice(['scasb','scasl','cmpsb','cmpsl'])
    n  = rng.randrange(1, 5)
    rep = rng.choice(['', 'repe ', 'repne '])
    return f"cld\n\tmovl ${n}, %ecx\n\t" + rep + op

# ---------------- division ----------------
@tmpl("div/idiv 32", undef=F_ALL)
def _(rng):
    op = rng.choice(['divl','idivl'])
    # the divisor must not be zero and the quotient must not overflow: small dividend, large divisor
    d = rng.randrange(1, 1000)
    return ("movl $0, %edx" + chr(10) + chr(9) + "andl $0xffff, %eax" + chr(10) + chr(9)
            + "movl $" + str(d) + ", %ecx" + chr(10) + chr(9) + op + " %ecx")
@tmpl("mul/imul 8-16", undef=F_ALL)
def _(rng):
    op = rng.choice(['mulb','imulb','mulw','imulw'])
    return f"{op} {rng.choice(['%cl','%bl']) if op.endswith('b') else rng.choice(['%cx','%bx'])}"

# ---------------- flag moves ----------------
@tmpl("pushf/popf/sahf/lahf")
def _(rng):
    return rng.choice(["lahf", "sahf", "pushfq\n\tpopq %rax\n\tandl $0x8d5, %eax",
                       "clc", "stc", "cmc"])
@tmpl("xadd/cmpxchg")
def _(rng):
    if rng.random() < 0.5:
        return f"xaddl {rng.choice(R32[1:])}, {rng.choice(R32[1:])}"
    return f"cmpxchgl {rng.choice(['%ecx','%edx','%ebx'])}, {rng.choice(['%ecx','%edx','%ebx'])}"
@tmpl("movzx/movsx register")
def _(rng):
    op = rng.choice(['movzbl','movsbl','movzwl','movswl'])
    src = rng.choice(R8) if op.endswith('bl') else rng.choice(R16)
    return f"{op} {src}, {rng.choice(R32)}"
@tmpl("bit m32", undef=F_OF|F_SF|F_ZF|F_AF|F_PF)
def _(rng):
    op = rng.choice(['btl','btsl','btrl','btcl'])
    return f"{op} ${rng.randrange(0,32)}, {M(rng)}"
@tmpl("xchg m32")
def _(rng): return f"xchgl {rng.choice(R32)}, {M(rng)}"
@tmpl("mov variants")
def _(rng):
    return rng.choice([f"movl {M(rng)}, {rng.choice(R32)}",
                       f"movl {rng.choice(R32)}, {M(rng)}",
                       f"movw {M(rng,0,96,2)}, {rng.choice(R16)}",
                       f"movb {M(rng,0,96,1)}, {rng.choice(R8)}",
                       f"movl ${rng.getrandbits(32)}, {M(rng)}"])

# ---------------- x87: register forms, constants, compares, integer operands ----------------
X87_3 = "flds 0(%rdi)\n\tflds 8(%rdi)\n\tflds 16(%rdi)\n\t"
@tmpl("x87 reg-reg arithmetic")
def _(rng):
    i = rng.randrange(1, 3)
    op = rng.choice(['faddp','fsubp','fsubrp','fmulp','fdivp','fdivrp'])
    if rng.random() < 0.5:
        return X87_3 + f"{op} %st, %st({i})\n\tfstps 64(%rdi)\n\tfstps 68(%rdi)"
    op = rng.choice(['fadd','fsub','fsubr','fmul','fdiv','fdivr'])
    form = rng.choice([f"%st({i}), %st", f"%st, %st({i})"])
    return X87_3 + f"{op} {form}\n\tfstps 64(%rdi)\n\tfstps 68(%rdi)\n\tfstps 72(%rdi)"
@tmpl("x87 constants")
def _(rng):
    op = rng.choice(['fldz','fld1','fldpi','fldl2e','fldl2t','fldlg2','fldln2'])
    return f"{op}\n\t" + rng.choice(["fstps 64(%rdi)", "fstpl 64(%rdi)"])
@tmpl("x87 compare -> status word")
def _(rng):
    op = rng.choice(["fcom %st(1)", "fcomp %st(1)", "fcompp", "fucom %st(1)", "fucomp %st(1)",
                     "fucompp", "ftst", "fcoms 16(%rdi)", "fcompl 16(%rdi)", "fxam"])
    return ("flds 8(%rdi)\n\tflds 0(%rdi)\n\t" + op +
            "\n\tfnstsw %ax\n\tandl $0x4500, %eax\n\tffree %st(0)\n\tffree %st(1)")
@tmpl("x87 compare -> eflags (fcomi)")
def _(rng):
    op = rng.choice(["fcomi %st(1), %st", "fucomi %st(1), %st", "fcomip %st(1), %st", "fucomip %st(1), %st"])
    return "flds 8(%rdi)\n\tflds 0(%rdi)\n\t" + op + "\n\tffree %st(0)\n\tffree %st(1)"
@tmpl("x87 fcmovcc")
def _(rng):
    cc = rng.choice(['b','e','be','u','nb','ne','nbe','nu'])
    return (f"cmpl {rng.choice(R32)}, {rng.choice(R32)}\n\tflds 8(%rdi)\n\tflds 0(%rdi)\n\t"
            f"fcmov{cc} %st(1), %st\n\tfstps 64(%rdi)\n\tfstps 68(%rdi)")
@tmpl("x87 integer operand")
def _(rng):
    op = rng.choice(['fiaddl','fisubl','fisubrl','fimull','fidivl','fidivrl',
                     'fiadds','fisubs','fimuls','fidivs'])
    # 32(%rdi) holds random bytes (not a float): a real integer operand
    return f"flds 0(%rdi)\n\t{op} 32(%rdi)\n\tfstps 64(%rdi)"
@tmpl("x87 ficom / fist / fild sizes")
def _(rng):
    return rng.choice([
        "flds 0(%rdi)\n\tficoml 32(%rdi)\n\tfnstsw %ax\n\tandl $0x4500, %eax\n\tfstps 64(%rdi)",
        "flds 0(%rdi)\n\tficomps 32(%rdi)\n\tfnstsw %ax\n\tandl $0x4500, %eax",
        "flds 0(%rdi)\n\tfistl 64(%rdi)\n\tfstps 68(%rdi)",
        "flds 0(%rdi)\n\tfistps 64(%rdi)",
        "flds 0(%rdi)\n\tfisttpl 64(%rdi)",
        "filds 32(%rdi)\n\tfstps 64(%rdi)",
        "fildq 32(%rdi)\n\tfstpl 64(%rdi)",
        "fildl 32(%rdi)\n\tfstpl 64(%rdi)"])
@tmpl("x87 control word / status")
def _(rng):
    return rng.choice([
        "fnstcw 64(%rdi)", "fwait\n\tfnstcw 64(%rdi)",
        "flds 0(%rdi)\n\tfnstsw %ax\n\tandl $0x3800, %eax\n\tfstps 64(%rdi)",
        "flds 0(%rdi)\n\tflds 8(%rdi)\n\tfincstp\n\tfnstsw %ax\n\tandl $0x3800, %eax\n\tfdecstp\n\tfstps 64(%rdi)\n\tfstps 68(%rdi)",
        "fnclex\n\tfnstsw %ax"])
@tmpl("x87 two-operand transcendental (80-bit difference)", skipmem=True)
def _(rng):
    op = rng.choice(['fyl2x', 'fpatan', 'fyl2xp1'])
    return f"flds 8(%rdi)\n\tflds 0(%rdi)\n\tfabs\n\t{op}\n\tfstps 64(%rdi)"
@tmpl("x87 fsincos (80-bit difference)", skipmem=True)
def _(rng): return "flds 0(%rdi)\n\tfsincos\n\tfstps 64(%rdi)\n\tfstps 68(%rdi)"

# ---------------- SSE moves and the rest of SSE/SSE2 ----------------
@tmpl("sse moves")
def _(rng):
    return rng.choice([
        "movaps 0(%rdi), %xmm0\n\tmovaps %xmm0, %xmm1\n\tmovaps %xmm1, 64(%rdi)",
        "movss 4(%rdi), %xmm0\n\tmovss %xmm0, 64(%rdi)",
        "movups 0(%rdi), %xmm0\n\tmovss 20(%rdi), %xmm0\n\tmovups %xmm0, 64(%rdi)",
        "movups 0(%rdi), %xmm0\n\tmovups 16(%rdi), %xmm1\n\tmovss %xmm1, %xmm0\n\tmovups %xmm0, 64(%rdi)",
        "movups 0(%rdi), %xmm0\n\tmovlps 16(%rdi), %xmm0\n\tmovups %xmm0, 64(%rdi)",
        "movups 0(%rdi), %xmm0\n\tmovhps 16(%rdi), %xmm0\n\tmovups %xmm0, 64(%rdi)",
        "movups 0(%rdi), %xmm0\n\tmovlps %xmm0, 64(%rdi)\n\tmovhps %xmm0, 72(%rdi)",
        "movups 0(%rdi), %xmm0\n\tmovntps %xmm0, 64(%rdi)",
        "movups 0(%rdi), %xmm0\n\tmovmskps %xmm0, %eax",
        "movd 0(%rdi), %xmm0\n\tmovd %xmm0, %eax\n\tmovups %xmm0, 64(%rdi)",
        f"movd {rng.choice(R32)}, %xmm0\n\tmovups %xmm0, 64(%rdi)",
        f"movd {rng.choice(R32)}, %mm0\n\tmovq %mm0, 64(%rdi)\n\temms",
        "movq 0(%rdi), %mm0\n\tmovd %mm0, %eax\n\tmovntq %mm0, 64(%rdi)\n\temms"])
@tmpl("sse2 moves")
def _(rng):
    return rng.choice([
        "movdqa 0(%rdi), %xmm0\n\tmovdqa %xmm0, %xmm1\n\tmovdqa %xmm1, 64(%rdi)",
        "movapd 0(%rdi), %xmm0\n\tmovapd %xmm0, 64(%rdi)",
        "movupd 0(%rdi), %xmm0\n\tmovlpd 16(%rdi), %xmm0\n\tmovupd %xmm0, 64(%rdi)",
        "movupd 0(%rdi), %xmm0\n\tmovhpd 16(%rdi), %xmm0\n\tmovupd %xmm0, 64(%rdi)",
        "movsd 8(%rdi), %xmm0\n\tmovsd %xmm0, 64(%rdi)",
        "movq 0(%rdi), %xmm0\n\tmovdqu %xmm0, 64(%rdi)",
        "movdqu 0(%rdi), %xmm0\n\tmovq %xmm0, 64(%rdi)",
        "movq 0(%rdi), %mm0\n\tmovq2dq %mm0, %xmm0\n\tmovdqu %xmm0, 64(%rdi)\n\temms",
        "movdqu 0(%rdi), %xmm0\n\tmovdq2q %xmm0, %mm0\n\tmovq %mm0, 64(%rdi)\n\temms",
        f"movdqu 0(%rdi), %xmm0\n\tpshufd ${rng.getrandbits(8)}, %xmm0, %xmm1\n\tmovdqu %xmm1, 64(%rdi)",
        f"movdqu 0(%rdi), %xmm0\n\tpshuflw ${rng.getrandbits(8)}, %xmm0, %xmm1\n\tmovdqu %xmm1, 64(%rdi)",
        f"movdqu 0(%rdi), %xmm0\n\tpshufhw ${rng.getrandbits(8)}, %xmm0, %xmm1\n\tmovdqu %xmm1, 64(%rdi)",
        "movdqu 0(%rdi), %xmm0\n\tpmovmskb %xmm0, %eax",
        "movups 0(%rdi), %xmm0\n\tcvtdq2pd %xmm0, %xmm1\n\tmovupd %xmm1, 64(%rdi)"])
@tmpl("sse more packed/scalar float")
def _(rng):
    op = rng.choice(['andnps','sqrtps','unpcklpd','unpckhpd','andpd','andnpd','orpd','xorpd',
                     'addsd','subsd','mulsd','divsd','minsd','maxsd','sqrtsd','sqrtpd',
                     f"cmppd ${rng.randrange(0,8)},", f"cmpsd ${rng.randrange(0,8)},",
                     f"shufpd ${rng.randrange(0,4)},"])
    return f"movups 0(%rdi), %xmm0\n\tmovups 16(%rdi), %xmm1\n\t{op} %xmm1, %xmm0\n\tmovups %xmm0, 64(%rdi)"
PINT = ['paddb','paddw','paddd','psubb','psubw','psubd','paddsb','paddsw','psubsb','psubsw',
        'paddusb','paddusw','psubusb','psubusw','pand','pandn','por','pxor',
        'pcmpeqb','pcmpeqw','pcmpeqd','pcmpgtb','pcmpgtw','pcmpgtd',
        'pmullw','pmulhw','pmaddwd','punpcklbw','punpcklwd','punpckldq',
        'punpckhbw','punpckhwd','punpckhdq','packuswb','packsswb','packssdw']
@tmpl("mmx packed int (all)")
def _(rng):
    op = rng.choice(PINT + ['pmulhuw','pavgb','pavgw','pminub','pmaxub','pminsw','pmaxsw','psadbw',
                            'paddq','psubq','pmuludq'])
    return (f"movq 0(%rdi), %mm0\n\tmovq 8(%rdi), %mm1\n\t{op} %mm1, %mm0\n\t"
            "movq %mm0, 64(%rdi)\n\temms")
@tmpl("mmx packed int, memory operand")
def _(rng):
    return (f"movq 0(%rdi), %mm0\n\t{rng.choice(PINT)} 8(%rdi), %mm0\n\t"
            "movq %mm0, 64(%rdi)\n\temms")
@tmpl("sse2 packed int (all)")
def _(rng):
    op = rng.choice(PINT + ['paddq','psubq','pmuludq','pmulhuw','pminub','pmaxub',
                            'punpcklqdq','punpckhqdq'])
    return f"movdqu 0(%rdi), %xmm0\n\tmovdqu 16(%rdi), %xmm1\n\t{op} %xmm1, %xmm0\n\tmovdqu %xmm0, 64(%rdi)"
@tmpl("sse2 shift imm/reg")
def _(rng):
    op = rng.choice(['psllw','pslld','psllq','psrlw','psrld','psrlq','psraw','psrad'])
    if rng.random() < 0.4:
        return f"movdqu 0(%rdi), %xmm0\n\t{op} ${rng.randrange(0,70)}, %xmm0\n\tmovdqu %xmm0, 64(%rdi)"
    if rng.random() < 0.3:
        return (f"movdqu 0(%rdi), %xmm0\n\t{rng.choice(['pslldq','psrldq'])} ${rng.randrange(0,18)}, %xmm0\n\t"
                "movdqu %xmm0, 64(%rdi)")
    return f"movdqu 0(%rdi), %xmm0\n\tmovdqu 16(%rdi), %xmm1\n\t{op} %xmm1, %xmm0\n\tmovdqu %xmm0, 64(%rdi)"
@tmpl("pextrw/pinsrw")
def _(rng):
    return rng.choice([
        f"movq 0(%rdi), %mm0\n\tpextrw ${rng.randrange(0,4)}, %mm0, %eax\n\temms",
        f"movq 0(%rdi), %mm0\n\tpinsrw ${rng.randrange(0,4)}, {rng.choice(R32)}, %mm0\n\tmovq %mm0, 64(%rdi)\n\temms",
        f"movdqu 0(%rdi), %xmm0\n\tpextrw ${rng.randrange(0,8)}, %xmm0, %eax",
        f"movdqu 0(%rdi), %xmm0\n\tpinsrw ${rng.randrange(0,8)}, {rng.choice(R32)}, %xmm0\n\tmovdqu %xmm0, 64(%rdi)"])
@tmpl("mxcsr")
def _(rng): return "stmxcsr 64(%rdi)\n\tandl $0xffc0, 64(%rdi)"

# ---------------- more integer forms ----------------
@tmpl("setcc m8 / all conditions")
def _(rng): return f"set{rng.choice(CC)} {rng.randrange(64,96)}(%rdi)"
@tmpl("loope/loopne")
def _(rng):
    return (f"movl ${rng.randrange(1,5)}, %ecx\n1:\tsubl $1, %eax\n\tcmpl %ebx, %edx\n\t"
            f"{rng.choice(['loope','loopne'])} 1b")
@tmpl("string ops, 16-bit", ptr=True)
def _(rng):
    op = rng.choice(['movsw','stosw','lodsw','scasw','cmpsw','stosb','movsb','scasb'])
    rep = rng.choice(['', 'rep ']) if op[:4] in ('movs','stos') else rng.choice(['', 'repe ', 'repne '])
    return f"cld\n\tmovl ${rng.randrange(1,5)}, %ecx\n\t{rep}{op}"
@tmpl("adc/sbb chains")
def _(rng):
    return (f"addl {rng.choice(R32)}, %eax\n\tadcl {rng.choice(R32)}, %edx\n\t"
            f"subl {rng.choice(R32)}, %ecx\n\tsbbl {rng.choice(R32)}, %ebx")
@tmpl("imul/mul/div m32", undef=F_ALL)
def _(rng):
    return rng.choice([f"mull {M(rng)}", f"imull {M(rng)}",
                       f"movl $0, %edx\n\torl $1, 8(%rdi)\n\tdivl 8(%rdi)",
                       "cltd\n\torl $0x10000, 8(%rdi)\n\tandl $0x7fffffff, 8(%rdi)\n\tidivl 8(%rdi)"])
@tmpl("shifts on memory", undef=F_OF|F_AF)
def _(rng):
    op = rng.choice(['shll','shrl','sarl','roll','rorl'])
    return rng.choice([f"{op} ${rng.randrange(0,33)}, {M(rng)}", f"{op} %cl, {M(rng)}", f"{op} {M(rng)}"])
@tmpl("neg/not/inc/dec m8/m16/m32")
def _(rng):
    op = rng.choice(['neg','not','inc','dec']); sz = rng.choice(['b','w','l'])
    return f"{op}{sz} {M(rng)}"

# ---------------- control flow inside the case ----------------
@tmpl("jcc short")
def _(rng):
    return (f"cmpl {rng.choice(R32)}, {rng.choice(R32)}\n\tj{rng.choice(CC)} 1f\n\t"
            f"notl {rng.choice(R32)}\n1:")
@tmpl("jcc after test/sub")
def _(rng):
    op = rng.choice([f"testl {rng.choice(R32)}, {rng.choice(R32)}",
                     f"subl {rng.choice(R32)}, {rng.choice(R32)}",
                     f"andl ${rng.getrandbits(32)}, {rng.choice(R32)}"])
    return f"{op}\n\tj{rng.choice(CC)} 1f\n\taddl $0x1111, %edx\n1:"
@tmpl("loop")
def _(rng):
    return f"movl ${rng.randrange(1,6)}, %ecx\n1:\taddl %ebx, %eax\n\tloop 1b"
@tmpl("bt/bts r32,r32", undef=F_OF|F_SF|F_ZF|F_AF|F_PF)
def _(rng):
    op = rng.choice(['btl','btsl','btrl','btcl'])
    return f"{op} {rng.choice(R32)}, {rng.choice(R32)}"
@tmpl("xchg eax,r32 (one byte) / nop")
def _(rng):
    return rng.choice([f"xchgl %eax, {rng.choice(R32[1:])}", "nop"])
@tmpl("movd/movq mmx")
def _(rng):
    op = rng.choice(['paddb','paddw','paddd','psubb','psubw','psubd','pand','por','pxor',
                     'pcmpeqb','pcmpeqw','pcmpeqd','pmullw','punpcklbw','punpcklwd',
                     'punpckldq','punpckhbw','punpckhwd','punpckhdq','packuswb','packsswb'])
    return (f"movq 0(%rdi), %mm0\n\tmovq 8(%rdi), %mm1\n\t{op} %mm1, %mm0\n\t"
            "movq %mm0, 64(%rdi)\n\temms")
@tmpl("mmx shift imm/reg")
def _(rng):
    op = rng.choice(['psllw','pslld','psllq','psrlw','psrld','psrlq','psraw','psrad'])
    if rng.random() < 0.5:
        return (f"movq 0(%rdi), %mm0\n\t{op} ${rng.randrange(0,70)}, %mm0\n\t"
                "movq %mm0, 64(%rdi)\n\temms")
    # count in a register: the WHOLE 64-bit count decides (>= width clears the register)
    return (f"movq 0(%rdi), %mm0\n\tmovq 16(%rdi), %mm1\n\t{op} %mm1, %mm0\n\t"
            "movq %mm0, 64(%rdi)\n\temms")

# ---------------- 32-bit-only encodings (unicorn oracle only) ----------------
E32 = ['%eax','%ecx','%edx','%ebx','%ebp','%esi']
@tmpl("inc/dec r32, one-byte 40-4F", m32=True)
def _(rng): return f"{rng.choice(['incl','decl'])} {rng.choice(E32)}"
@tmpl("pushad -> pop", m32=True)
def _(rng):
    return "pushal\n\tpopl %eax\n\tpopl %ecx\n\tpopl %edx\n\tpopl %ebx\n\taddl $16, %esp"
@tmpl("pushad / popad", m32=True)
def _(rng):
    a, b = rng.sample([0, 4, 8, 16, 20, 24, 28], 2)       # 12 is the saved esp: popad skips it
    return f"pushal\n\tnotl {a}(%esp)\n\tnegl {b}(%esp)\n\tpopal"
@tmpl("push/pop r32, imm, m32", m32=True)
def _(rng):
    return rng.choice([
        f"pushl {rng.choice(E32)}\n\tpopl {rng.choice(E32)}",
        f"pushl ${rng.getrandbits(32)}\n\tpopl {rng.choice(E32)}",
        f"pushl ${rng.randrange(-128,128)}\n\tpopl {rng.choice(E32)}",
        f"pushl {rng.randrange(0,60,4)}(%edi)\n\tpopl {rng.randrange(64,96,4)}(%edi)",
        f"pushw {rng.choice(['%ax','%cx','%dx','%bx'])}\n\tpopw {rng.choice(['%ax','%cx','%dx','%bx'])}"])
@tmpl("pushfd/popfd", m32=True)
def _(rng):
    return rng.choice(["pushfl\n\tpopl %eax\n\tandl $0x8d5, %eax",
                       "pushl %eax\n\tandl $0x8d5, (%esp)\n\torl $0x202, (%esp)\n\tpopfl"])
@tmpl("enter/leave", m32=True)
def _(rng):
    return rng.choice([f"pushl %ebp\n\tmovl %esp, %ebp\n\tsubl ${rng.randrange(0,64,4)}, %esp\n\t"
                       "movl %ebp, %eax\n\tleave",
                       f"enter ${rng.randrange(0,64,4)}, $0\n\tmovl %esp, %ecx\n\tleave"])
@tmpl("aaa/aas", undef=F_OF|F_SF|F_ZF|F_PF, m32=True)
def _(rng): return rng.choice(['aaa','aas'])
@tmpl("daa/das", undef=F_OF, m32=True)
def _(rng): return rng.choice(['daa','das'])
@tmpl("aam/aad", undef=F_OF|F_AF|F_CF, m32=True)
def _(rng): return rng.choice(['aam','aad', f"aam ${rng.randrange(1,256)}", f"aad ${rng.randrange(0,256)}"])
@tmpl("lea, 16-bit addressing", m32=True)
def _(rng):
    m = rng.choice(['(%bx,%si)','(%bx,%di)','(%bp,%si)','(%bp,%di)','(%si)','(%di)','(%bx)'])
    return f"leal {rng.randrange(-200,200)}{m}, {rng.choice(E32)}"
@tmpl("jecxz", m32=True)
def _(rng): return "jecxz 1f\n\tnotl %eax\n1:"
@tmpl("xlat", m32=True)
def _(rng): return "movl %edi, %ebx\n\txlat"
@tmpl("lea with edi base", m32=True)
def _(rng):
    i = rng.choice(['%eax','%ecx','%edx','%ebx'])
    return f"leal {rng.randrange(-64,64)}(%edi,{i},{rng.choice([1,2,4,8])}), {rng.choice(E32)}"
@tmpl("string ops with edi/esi as values", ptr=True, m32=True)
def _(rng):
    op = rng.choice(['movsb','movsl','stosl','lodsl','scasb','cmpsl'])
    return f"cld\n\tmovl ${rng.randrange(1,4)}, %ecx\n\t{rng.choice(['','rep '])}{op}"

def assemble(asm_list, workdir, m32=None):
    """Assemble all test instructions in a single file, return the byte arrays.

    A separator byte is NOT USED: an instruction's own bytes can merge with the
    separator (example: `andb %cl,%ah` -> 20 CC).  Instead every test gets a global
    symbol and the boundaries are read from the symbol table."""
    src = ['.text']
    for i, a in enumerate(asm_list):
        src.append('.globl _c%d' % i)
        src.append('_c%d:' % i)
        if m32 and m32[i]:
            src.append('\t.code32')
            src.append('\t' + a)
            src.append('\t.code64')
        else:
            src.append('\t' + a)
    src.append('.globl _cend')
    src.append('_cend:')
    src.append('\tnop')
    sfile = os.path.join(workdir, 'fuzz.s')
    ofile = os.path.join(workdir, 'fuzz.o')
    open(sfile, 'w').write('\n'.join(src) + '\n')
    subprocess.run(['clang','-arch','x86_64','-c','-o',ofile,sfile], check=True)

    nm = subprocess.run(['nm','-n',ofile], capture_output=True, text=True, check=True).stdout
    addr = {}
    for line in nm.splitlines():
        f = line.split()
        if len(f) >= 3 and f[2].startswith('_c'):
            try: addr[f[2]] = int(f[0], 16)
            except ValueError: pass

    raw = subprocess.run(['otool','-t',ofile], capture_output=True, text=True, check=True).stdout
    byts = bytearray()
    for line in raw.splitlines():
        parts = line.split()
        if len(parts) < 2: continue
        if not all(len(x) == 2 for x in parts[1:]): continue
        try: int(parts[0], 16)
        except ValueError: continue
        for x in parts[1:]: byts.append(int(x, 16))

    out = []
    for i in range(len(asm_list)):
        a0 = addr.get('_c%d' % i)
        a1 = addr.get('_c%d' % (i+1), addr.get('_cend'))
        if a0 is None or a1 is None or a1 <= a0:
            raise SystemExit('assemble: no boundary found for %d' % i)
        out.append(bytes(byts[a0:a1]))
    return out

ORACLE_C = """
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <setjmp.h>
#include <signal.h>
static sigjmp_buf g_jb;
static volatile int g_infault;
static void onfault(int sig){ (void)sig; if (g_infault) siglongjmp(g_jb, 1); _exit(9); }
/* 16-byte aligned so that mem (offset 32) is too: movaps/movdqa need it */
typedef struct { uint32_t r[6]; uint32_t fl; uint32_t pad; uint8_t mem[@MEMSZ@]; } __attribute__((aligned(16))) St;
extern const unsigned char CODE[];
extern const unsigned CODE_OFF[];
extern const unsigned CODE_LEN[];
extern const unsigned NCASE;
extern const unsigned char INIT[];
#define INITSZ (32 + @MEMSZ@)   /* packed size of one initial state */
static void *page;
static void run_one(St *s, unsigned long long rsiv){
  __asm__ volatile(
    "pushq %%rbx\\n\\tpushq %%rbp\\n\\tpushq %%r12\\n\\tpushq %%r13\\n\\t"
    "movq %0, %%r12\\n\\tmovq %1, %%r13\\n\\t"
    "leaq 32(%%r12), %%rdi\\n\\t"
    "movl 0(%%r12), %%eax\\n\\tmovl 4(%%r12), %%ecx\\n\\t"
    "movl 8(%%r12), %%edx\\n\\tmovl 12(%%r12), %%ebx\\n\\t"
    "movl 16(%%r12), %%ebp\\n\\tmovl 20(%%r12), %%esi\\n\\t"
    "fninit\\n\\t"                    /* every case starts with a clean FPU */
    "xorps %%xmm0, %%xmm0\\n\\txorps %%xmm1, %%xmm1\\n\\t"
    "xorps %%xmm2, %%xmm2\\n\\txorps %%xmm3, %%xmm3\\n\\t"
    "movl 24(%%r12), %%r11d\\n\\tpushq %%r11\\n\\tpopfq\\n\\t"
    "callq *%%r13\\n\\t"
    "pushfq\\n\\tpopq %%r11\\n\\t"
    "movl %%eax, 0(%%r12)\\n\\tmovl %%ecx, 4(%%r12)\\n\\t"
    "movl %%edx, 8(%%r12)\\n\\tmovl %%ebx, 12(%%r12)\\n\\t"
    "movl %%ebp, 16(%%r12)\\n\\tmovl %%esi, 20(%%r12)\\n\\t"
    "movl %%r11d, 24(%%r12)\\n\\t"
    "popq %%r13\\n\\tpopq %%r12\\n\\tpopq %%rbp\\n\\tpopq %%rbx\\n\\t"
    :: "r"(s), "r"(page), "r"(rsiv) : "rax","rcx","rdx","rsi","rdi","r11","memory","cc",
       "xmm0","xmm1","xmm2","xmm3","st","st(1)","st(2)","st(3)");
}
int main(void){
  setvbuf(stdout, 0, _IOLBF, 0);
  /* Some random cases can fault on the real CPU (division, access);
   * skip that case and continue, don't lose the whole run. */
  signal(SIGSEGV, onfault); signal(SIGBUS, onfault);
  signal(SIGFPE, onfault);  signal(SIGILL, onfault); signal(SIGTRAP, onfault);
  page = mmap(0, 1<<20, PROT_READ|PROT_WRITE|PROT_EXEC, MAP_PRIVATE|MAP_ANON, -1, 0);
  for (unsigned i = 0; i < NCASE; i++){
    if (!CODE_LEN[i]) continue;          /* 32-bit-only encoding: not for this oracle */
    unsigned char *p = (unsigned char*)page;
    memcpy(p, CODE + CODE_OFF[i], CODE_LEN[i]);
    p[CODE_LEN[i]] = 0xC3;
    St s; memcpy(&s, INIT + i*INITSZ, INITSZ);
    /* if pad != 0 esi is used as a POINTER (string instructions) */
    unsigned long long rsiv = s.pad ? (unsigned long long)(unsigned long)(s.mem + 64)
                                    : (unsigned long long)s.r[5];
    g_infault = 1;
    if (sigsetjmp(g_jb, 1) != 0){ g_infault = 0; continue; }   /* skip the case */
    run_one(&s, rsiv);
    g_infault = 0;
    printf("%u", i);
    for (int k = 0; k < 6; k++) printf(" %08x", s.r[k]);
    printf(" %08x ", s.fl & 0x8D5u);
    for (int k = 0; k < @MEMSZ@; k++) printf("%02x", s.mem[k]);
    printf("\\n");
  }
  return 0;
}
"""

GUEST_HDR = """
#include "cpu.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
void mem_commit(uint32_t,uint32_t);
void dispatch_indirect(CPU*c,uint32_t a){(void)c;(void)a;}
void unsupported_insn(CPU*c,uint32_t a,const char*m){(void)c;(void)a;(void)m;}
void guest_hook(CPU*c,uint32_t a){(void)c;(void)a;}
void cpuid_shim(CPU*c){(void)c;} uint64_t rdtsc_shim(void){return 0;}
"""

def gen_oracle(cases, chunks, workdir, build=True):
    blob = bytearray(); offs = []; lens = []
    for cs, c in zip(cases, chunks):
        if cs['m32']: c = b''
        offs.append(len(blob)); lens.append(len(c)); blob += c
    init = bytearray()
    for cs in cases:
        init += struct.pack('<6III', *cs['regs'], cs['flags'],
                            1 if cs['ptr'] else 0) + cs['mem']
    o = [ORACLE_C.replace('@MEMSZ@', str(MEMSZ))]
    o.append('const unsigned NCASE = %d;' % len(cases))
    o.append('const unsigned char CODE[] = {' + ','.join(str(b) for b in blob) + '};')
    o.append('const unsigned CODE_OFF[] = {' + ','.join(map(str, offs)) + '};')
    o.append('const unsigned CODE_LEN[] = {' + ','.join(map(str, lens)) + '};')
    o.append('const unsigned char INIT[] = {' + ','.join(str(b) for b in init) + '};')
    src = os.path.join(workdir, 'oracle.c')
    open(src, 'w').write('\n'.join(o))
    if not build: return src
    exe = os.path.join(workdir, 'oracle')
    subprocess.run(['clang','-arch','x86_64','-O0','-o',exe,src], check=True)
    return exe

# ---------------- second oracle: Unicorn (QEMU's translator), 32-bit mode ----------------
UC_CODE, UC_STACK_LO, UC_ESP = BASE, 0x00AF0000, 0x00AFFFFC   # esp = the guest's at entry
UC_PRE = bytes.fromhex('dbe3' '0f57c0' '0f57c9' '0f57d2' '0f57db')   # fninit; xorps xmm0..3

def run_unicorn(cases, chunks):
    """Returns {case index as str: [eax, ecx, edx, ebx, ebp, esi, flags, memhex]} in
    the format of the other oracles, or None if unicorn isn't installed.
    Addresses are the guest's own (edi = SCRATCH, the same stack pointer), so
    unlike the 64-bit oracles every register is comparable."""
    try:
        from unicorn import Uc, UcError, UC_ARCH_X86, UC_MODE_32
        from unicorn import x86_const as X
    except ImportError:
        return None
    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    uc.mem_map(0, 0x10000)                       # 16-bit addressing forms (lea only)
    uc.mem_map(UC_CODE, 0x10000)
    uc.mem_map(SCRATCH & ~0xfff, 0x10000)
    uc.mem_map(UC_STACK_LO, 0x10000)
    uc.reg_write(X.UC_X86_REG_CR0, (uc.reg_read(X.UC_X86_REG_CR0) | 0x2) & ~0xC)   # MP, no EM/TS
    uc.reg_write(X.UC_X86_REG_CR4, uc.reg_read(X.UC_X86_REG_CR4) | 0x600)          # OSFXSR, OSXMMEXCPT
    GP = [X.UC_X86_REG_EAX, X.UC_X86_REG_ECX, X.UC_X86_REG_EDX,
          X.UC_X86_REG_EBX, X.UC_X86_REG_EBP, X.UC_X86_REG_ESI]
    out = {}
    for i, (cs, ch) in enumerate(zip(cases, chunks)):
        code = UC_PRE + bytes(ch)
        # the previous case's translated blocks live at the same address: drop them,
        # or stale code runs (seen as "the previous instruction executed again")
        uc.ctl_remove_cache(UC_CODE, UC_CODE + 0x10000)
        uc.mem_write(UC_CODE, code)
        uc.mem_write(UC_STACK_LO, bytes(0x10000))
        uc.mem_write(SCRATCH, cs['mem'])
        for r, v in zip(GP, cs['regs']): uc.reg_write(r, v)
        if cs['ptr']: uc.reg_write(X.UC_X86_REG_ESI, SCRATCH + 64)
        uc.reg_write(X.UC_X86_REG_EDI, SCRATCH)
        uc.reg_write(X.UC_X86_REG_ESP, UC_ESP)
        uc.reg_write(X.UC_X86_REG_EFLAGS, cs['flags'])
        uc.reg_write(X.UC_X86_REG_MXCSR, 0x1f80)   # the power-on value; unicorn starts at 0
        try:
            uc.emu_start(UC_CODE, UC_CODE + len(code), timeout=2_000_000, count=100000)
        except UcError:
            continue                              # fault (division, access): skip, like the others
        if uc.reg_read(X.UC_X86_REG_EIP) != UC_CODE + len(code): continue
        regs = ['%08x' % uc.reg_read(r) for r in GP]
        fl = '%08x' % (uc.reg_read(X.UC_X86_REG_EFLAGS) & 0x8D5)
        out[str(i)] = regs + [fl, bytes(uc.mem_read(SCRATCH, MEMSZ)).hex()]
    return out

EXPORT_README = """Run the fuzzer's oracle on real x86-64 hardware
===============================================

oracle.c is self-contained C (inline assembly, no dependencies).  On any
x86-64 Linux machine (a cloud VM is fine: the instructions run on the CPU):

    cc -O0 -mno-red-zone -o oracle oracle.c
    ./oracle > hw.txt
    grep -m1 'model name' /proc/cpuinfo >> hw.cpu.txt

Bring hw.txt back and compare (same n and seed as the export):

    tools/verify/fuzzisa.py @N@ @SEED@ --hw hw.txt

Generated with: n=@N@ seed=@SEED@ (@NC@ cases for the 64-bit oracle).
"""

def gen_guest(cases, chunks, workdir):
    bodies = []; names = []; skipped = []
    for i, c in enumerate(chunks):
        L = RawLifter(bytes(c) + b'\xc3', base=BASE, comments=False)
        try:
            code, _ = L.lift(BASE)
        except Unsupported as e:
            names.append(None); skipped.append((i, str(e))); continue
        except Exception as e:
            names.append(None); skipped.append((i, 'ERROR: ' + str(e))); continue
        bodies.append(code.replace('sub_%08x' % BASE, 't%d' % i))
        names.append('t%d' % i)
    # one small function per case: a single main() with every case inlined made
    # clang's compile time grow much faster than the number of cases
    o = [GUEST_HDR, '\n'.join(bodies), 'static CPU cpu; static CPU *const c = &cpu;']
    calls = []
    for i, cs in enumerate(cases):
        if i >= len(names) or names[i] is None: continue
        regs = cs['regs']; f = cs['flags']
        calls.append('  k%d();' % i)
        o.append('static __attribute__((noinline)) void k%d(void){ unsigned char m[%d];' % (i, MEMSZ))
        o.append('   memset(&cpu,0,sizeof cpu); cpu.esp=0x00B00000u; cpu.top=0; cpu.fcw=0x037f;')
        for k, rn in enumerate(['eax','ecx','edx','ebx','ebp','esi']):
            o.append('   cpu.%s=0x%08xu;' % (rn, regs[k]))
        if cs['ptr']:
            o.append('   cpu.esi=0x%08xu;' % (SCRATCH + 64))
        o.append('   cpu.edi=0x%08xu;' % SCRATCH)
        o.append('   cpu.cf=%d; cpu.pf=%d; cpu.af=%d;' % ((f>>0)&1, (f>>2)&1, (f>>4)&1))
        o.append('   cpu.zf=%d; cpu.sf=%d; cpu.of=%d; cpu.df=%d;' % ((f>>6)&1,(f>>7)&1,(f>>11)&1,(f>>10)&1))
        o.append('   static const unsigned char mi[%d]={%s};' % (MEMSZ, ','.join(str(b) for b in cs['mem'])))
        o.append('   memcpy(g_mem+0x%08xu, mi, %d);' % (SCRATCH, MEMSZ))
        o.append('   PUSH32(0xDEADC0DEu);')
        o.append('   %s(&cpu);' % names[i])
        o.append('   printf("%d");' % i)
        o.append('   printf(" %08x %08x %08x %08x %08x %08x",'
                 ' cpu.eax,cpu.ecx,cpu.edx,cpu.ebx,cpu.ebp,cpu.esi);')
        o.append('   { uint32_t fl = (cpu.cf?1u:0)|(cpu.pf?4u:0)|(cpu.af?0x10u:0)'
                 '|(cpu.zf?0x40u:0)|(cpu.sf?0x80u:0)|(cpu.of?0x800u:0);')
        o.append('     printf(" %08x ", fl); }')
        o.append('   memcpy(m, g_mem+0x%08xu, %d);' % (SCRATCH, MEMSZ))
        o.append('   for(int k=0;k<%d;k++) printf("%%02x", m[k]);' % MEMSZ)
        o.append('   printf("\\n");')
        o.append('  }')
    o.append('int main(void){ mem_init(); mem_commit(0x0u,0x01000000u); mem_fault_handler_install();')
    o += calls
    o.append('  return 0; }')
    src = os.path.join(workdir, 'guest.c')
    open(src, 'w').write('\n'.join(o))
    exe = os.path.join(workdir, 'guest')
    subprocess.run(['clang','-O1','-Wno-unused-label','-Wno-parentheses-equality','-Iruntime','-o',exe,src,
                    'runtime/mem.c','-lm'], check=True)
    return exe, skipped

def main():
    argv = list(sys.argv[1:]); export = hwfile = None
    if '--export' in argv:
        k = argv.index('--export'); export = argv[k+1]; del argv[k:k+2]
    if '--hw' in argv:
        k = argv.index('--hw'); hwfile = argv[k+1]; del argv[k:k+2]
    n    = int(argv[0]) if len(argv) > 0 else 2000
    seed = int(argv[1]) if len(argv) > 1 else 12345
    want = [x for x in os.environ.get('ORACLES', 'rosetta,unicorn').split(',') if x]
    rng = random.Random(seed)
    workdir = os.environ.get('FUZZDIR', '/tmp/fuzzisa')
    os.makedirs(workdir, exist_ok=True)

    cases = []; asm = []
    for i in range(n):
        t = T[i % len(T)] if i < len(T) else rng.choice(T)
        a = t.gen(rng)
        asm.append(a)
        regs = [rng.getrandbits(32) for _ in range(6)]
        for k in range(6):
            if rng.random() < 0.35:
                regs[k] = rng.choice([0,1,2,0xFFFFFFFF,0x80000000,0x7FFFFFFF,
                                      rng.randrange(0,64), rng.randrange(0,4)])
        mem = bytearray(rng.getrandbits(8) for _ in range(MEMSZ))
        for off in range(0, 64, 4):
            if rng.random() < 0.7:
                v = rng.choice([0.0,1.0,-1.0,0.5,2.0,3.7,-3.7,1e-3,1e6,
                                rng.uniform(-100,100)])
                struct.pack_into('<f', mem, off, v)
        flags = 0x202 | (rng.getrandbits(12) & 0x8D5)
        cases.append(dict(regs=regs, mem=bytes(mem), flags=flags, tmpl=t.name,
                          undef=t.undef, skipmem=t.skipmem, ptr=t.ptr, asm=a, m32=t.m32))

    chunks = assemble(asm, workdir, [c['m32'] for c in cases])
    with open(os.path.join(workdir,'cases.txt'),'w') as fh:
        for i,(a,ch) in enumerate(zip(asm,chunks)):
            fh.write('%d\t%s\t%s\n' % (i, ch.hex(), a.replace(chr(10),' ; ').replace(chr(9),'')))
    gexe, skipped = gen_guest(cases, chunks, workdir)
    rg = subprocess.run([gexe], capture_output=True, text=True)
    if rg.returncode: print('  WARNING: guest exit code %d  %s' % (rg.returncode, rg.stderr[:200]))
    def parse(lines):
        d = {}
        for l in lines:
            f = l.split()
            if len(f) == 9: d[f[0]] = f[1:]
        return d
    gm = parse(rg.stdout.splitlines())

    # ---- oracles: name -> (results, same addresses as the guest?)
    oracles = {}
    if 'rosetta' in want:
        oexe = gen_oracle(cases, chunks, workdir)
        ro = subprocess.run([oexe], capture_output=True, text=True)
        if ro.returncode: print('  WARNING: rosetta oracle exit code %d' % ro.returncode)
        oracles['rosetta'] = (parse(ro.stdout.splitlines()), False)
    if 'unicorn' in want:
        um = run_unicorn(cases, chunks)
        if um is None: print('  note: unicorn not installed (pip install "unicorn>=2,<3"); oracle skipped')
        else: oracles['unicorn'] = (um, True)
    if hwfile:
        oracles['hw'] = (parse(open(hwfile).read().splitlines()), False)
    if export:
        os.makedirs(export, exist_ok=True)
        gen_oracle(cases, chunks, export, build=False)
        open(os.path.join(export, 'README.txt'), 'w').write(
            EXPORT_README.replace('@N@', str(n)).replace('@SEED@', str(seed))
                         .replace('@NC@', str(sum(1 for c in cases if not c['m32']))))
        print('  exported: %s/oracle.c (+ README.txt)' % export)

    def compare(i, ref, got, same_addr, names=('real', 'ours')):
        """differences between two result rows of case i (ref is the oracle)"""
        undef = cases[i]['undef']; diffs = []
        for j, nm in enumerate(['eax','ecx','edx','ebx','ebp','esi']):
            if nm == 'esi' and cases[i]['ptr'] and not same_addr: continue   # base address differs
            if ref[j] != got[j]: diffs.append('%s: %s %s != %s %s' % (nm, names[0], ref[j], names[1], got[j]))
        rf = int(ref[6],16) & ~undef; gf = int(got[6],16) & ~undef
        if rf != gf: diffs.append('flags: %s %03x != %s %03x' % (names[0], rf, names[1], gf))
        if ref[7] != got[7] and not cases[i]['skipmem']:
            for b in range(MEMSZ):
                a1, b1 = ref[7][b*2:b*2+2], got[7][b*2:b*2+2]
                if a1 != b1:
                    diffs.append('mem[%d]: %s %s != %s %s' % (b, names[0], a1, names[1], b1)); break
        return diffs

    def inp(i):
        return 'input: ' + ' '.join('%s=%08x' % (nm, v) for nm, v in
                 zip(['eax','ecx','edx','ebx','ebp','esi'], cases[i]['regs'])) \
               + ' flags=%03x' % (cases[i]['flags'] & 0x8d5)

    # ---- guest against every oracle
    bad = {}; checked = 0; per = {nm: [0, 0] for nm in oracles}     # name -> [checked, mismatched]
    status = {}                                                     # case -> {oracle: 'ok'|'diff'}
    for i in range(n):
        k = str(i)
        if k not in gm: continue
        any_oracle = False
        for nm, (res, same) in oracles.items():
            if k not in res: continue
            any_oracle = True; per[nm][0] += 1
            d = compare(i, res[k], gm[k], same)
            status.setdefault(i, {})[nm] = 'diff' if d else 'ok'
            if d:
                per[nm][1] += 1
                bad.setdefault(cases[i]['tmpl'], []).append((i, cases[i]['asm'], [inp(i), 'oracle: ' + nm] + d))
        checked += any_oracle

    # ---- oracles against each other (who is right when the lifter differs from one?)
    onames = list(oracles); odis = {}
    for x in range(len(onames)):
        for y in range(x + 1, len(onames)):
            a, b2 = onames[x], onames[y]; cnt = 0; ex = {}
            for i in range(n):
                k = str(i)
                if k in oracles[a][0] and k in oracles[b2][0]:
                    cnt += 1
                    d = compare(i, oracles[a][0][k], oracles[b2][0][k],
                                oracles[a][1] and oracles[b2][1], names=(a, b2))
                    if d: ex.setdefault(cases[i]['tmpl'], []).append((i, cases[i]['asm'], [inp(i)] + d))
            odis[(a, b2)] = (cnt, ex)

    # ---- coverage record for tools/verify/isacoverage.py
    try:
        from capstone import Cs, CS_ARCH_X86, CS_MODE_32
        md = Cs(CS_ARCH_X86, CS_MODE_32)
        cov = []
        unl = {i: e for i, e in skipped}
        for i, (cs, ch) in enumerate(zip(cases, chunks)):
            cov.append(dict(tmpl=cs['tmpl'], m32=cs['m32'], skipmem=cs['skipmem'],
                            mn=sorted({x.mnemonic for x in md.disasm(bytes(ch), BASE)}),
                            lifted=i not in unl, err=unl.get(i), st=status.get(i, {})))
        json.dump(dict(n=n, seed=seed, oracles=onames, cases=cov),
                  open(os.path.join(workdir, 'coverage.json'), 'w'))
    except ImportError:
        pass

    print('\n  checked: %d/%d   not liftable: %d' % (checked, n, len(skipped)))
    for nm in onames:
        print('    %-8s %5d cases compared, %d mismatches' % (nm, per[nm][0], per[nm][1]))
    for (a, b2), (cnt, ex) in odis.items():
        nd = sum(len(v) for v in ex.values())
        print('    %s vs %s: %d common cases, %d disagreements' % (a, b2, cnt, nd)
              + ('' if not nd else '  (oracles disagree with each other)'))
        for tn, lst in sorted(ex.items(), key=lambda x: -len(x[1])):
            print('      --- %s  (%d cases) ---' % (tn, len(lst)))
            for i, asm_, d in lst[:1]:
                print('        #%d  %r' % (i, asm_))
                for x in d[:3]: print('            ' + x)
    if skipped:
        seen = set()
        for i, e in skipped:
            if e in seen: continue
            seen.add(e); print('    no lift: %-28s example: %r' % (e, cases[i]['asm']))
    # If no case was checked DON'T SAY "matched": when the lifter crashed from the
    # start (0/n checked) this line came out green and hid the breakage.
    if not checked:
        print('  \033[31mNO CASE WAS CHECKED -- result invalid\033[0m'); return 1
    if not bad:
        print('  \033[32mALL RESULTS MATCHED\033[0m'); return 0
    print('\n  \033[31m%d mismatches, in %d templates\033[0m'
          % (sum(len(v) for v in bad.values()), len(bad)))
    for tn, lst in sorted(bad.items(), key=lambda x: -len(x[1])):
        print('\n  --- %s  (%d cases) ---' % (tn, len(lst)))
        for i, a, d in lst[:2]:
            print('    #%d  %r' % (i, a))
            for x in d[:5]: print('        ' + x)
    return 1

if __name__ == '__main__':
    sys.exit(main())
