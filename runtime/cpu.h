/* NFSU2 native port - x86-32 CPU state and guest memory access (macOS ARM64) */
#ifndef NFSU2_CPU_H
#define NFSU2_CPU_H
#include <math.h>
#include <stdint.h>
#include <string.h>

/* Guest memory base. Guest 32-bit address -> host address in a single ARM64 instruction:
 *   ldr w0, [x_base, w_addr, uxtw]        (zero extra cost)              */
extern uint8_t *g_mem;
/* The whole 32-bit guest address space is reserved as PROT_NONE; the regions
 * in use are made RW with mem_commit. This way a bad guest access turns into a
 * diagnosable signal instead of crashing the host.                       */
#define GSZ 0x100000000ull

/* GA(0x...): an address in the game's executable, written as it is in the
 * reference (European v1.2) build.  Other builds compile the runtime with a
 * generated header (tools/addrmap.py header) that maps every GA literal and
 * every sub_XXXXXXXX name to that build; an unmappable address stops the build. */
#ifndef GA
#define GA(x) (x)
#endif

#define MR8(a)   (*(uint8_t  *)(g_mem + (uint32_t)(a)))
#define MR16(a)  (*(uint16_t *)(g_mem + (uint32_t)(a)))
#define MR32(a)  (*(uint32_t *)(g_mem + (uint32_t)(a)))
#define MR64(a)  (*(uint64_t *)(g_mem + (uint32_t)(a)))
#define MRF32(a) (*(float    *)(g_mem + (uint32_t)(a)))
#define MRF64(a) (*(double   *)(g_mem + (uint32_t)(a)))

enum { R_EAX=0,R_ECX,R_EDX,R_EBX,R_ESP,R_EBP,R_ESI,R_EDI };

typedef struct CPU {
    union {
        uint32_t r[8];
        struct { uint32_t eax,ecx,edx,ebx,esp,ebp,esi,edi; };
    };
    uint32_t eip;
    uint32_t fs_base, gs_base;   /* TEB / TLS base (flat model) */
    /* flags - each kept as 0/1 (clang DCE eliminates the unused ones) */
    uint32_t cf,pf,af,zf,sf,of,df;
    /* x87 stack */
    double   st[8];
    int32_t  top;
    uint16_t fcw, fsw;
    /* MMX (not shared with x87, kept separately) */
    uint64_t mm[8];
    /* SSE */
    uint32_t mxcsr;      /* SSE control/status: only stored and read back (ldmxcsr/stmxcsr) */
    union { uint8_t b[16]; float f[4]; double d[2]; uint32_t u32[4]; uint64_t u64[2]; } xmm[8];
} CPU;

/* ---- flag helpers ----------------------------------------------------- */
static inline uint32_t parity8(uint32_t v){ v&=0xff; v^=v>>4; v^=v>>2; v^=v>>1; return (~v)&1; }

/* ---- flag masks --------------------------------------------------------
 * Every arithmetic instruction computed all six flags EAGERLY (ZF/SF/PF/CF/OF/AF),
 * but the following branch usually reads only one.  Measured: the generated code
 * has 110k FLAGS_ADD + 130k FLAGS_SUB + 130k FLAGS_LOG, whereas OF is read
 * directly in only 22 places (CC_O/CC_NO), PF in 5.7k places.
 * The generator (tools/lift.py) now solves flag LIVENESS over the flow graph
 * and gives every call a mask of "flags read later".  Since the mask is a
 * compile-time constant, unused branches are removed entirely. */
#define F_CF  0x01u
#define F_PF  0x02u
#define F_AF  0x04u
#define F_ZF  0x08u
#define F_SF  0x10u
#define F_OF  0x20u
#define F_ALL 0x3Fu

#define SETZSP_M(res,bits,M) do{ \
    if((M)&F_ZF) c->zf = ((uint##bits##_t)(res)==0); \
    if((M)&F_SF) c->sf = (((uint##bits##_t)(res))>>((bits)-1))&1; \
    if((M)&F_PF) c->pf = parity8((uint32_t)(res)); }while(0)
#define SETZSP(res,bits) SETZSP_M(res,bits,F_ALL)

/* ADD */
#define FLAGS_ADD_M(a,b,res,bits,M) do{ SETZSP_M(res,bits,M); \
    if((M)&F_CF) c->cf = ((uint##bits##_t)(res) < (uint##bits##_t)(a)); \
    if((M)&F_AF) c->af = ((((a)^(b)^(res))>>4)&1); \
    if((M)&F_OF) c->of = ((~((a)^(b)) & ((a)^(res)))>>((bits)-1))&1; }while(0)
#define FLAGS_ADD(a,b,res,bits) FLAGS_ADD_M(a,b,res,bits,F_ALL)
/* SUB / CMP */
#define FLAGS_SUB_M(a,b,res,bits,M) do{ SETZSP_M(res,bits,M); \
    if((M)&F_CF) c->cf = ((uint##bits##_t)(a) < (uint##bits##_t)(b)); \
    if((M)&F_AF) c->af = ((((a)^(b)^(res))>>4)&1); \
    if((M)&F_OF) c->of = ((((a)^(b)) & ((a)^(res)))>>((bits)-1))&1; }while(0)
#define FLAGS_SUB(a,b,res,bits) FLAGS_SUB_M(a,b,res,bits,F_ALL)
/* ADC/SBB: with a carry-in of 1 the result can EQUAL the input; in that case
 * the (res<a) test missed the carry (example: sbb eax,eax  CF=1 -> CF=1). */
#define FLAGS_ADC_M(a,b,cin,res,bits,M) do{ SETZSP_M(res,bits,M); \
    if((M)&F_CF) c->cf = (cin) ? ((uint##bits##_t)(res) <= (uint##bits##_t)(a)) \
                               : ((uint##bits##_t)(res) <  (uint##bits##_t)(a)); \
    if((M)&F_AF) c->af = ((((a)^(b)^(res))>>4)&1); \
    if((M)&F_OF) c->of = ((~((a)^(b)) & ((a)^(res)))>>((bits)-1))&1; }while(0)
#define FLAGS_ADC(a,b,cin,res,bits) FLAGS_ADC_M(a,b,cin,res,bits,F_ALL)
#define FLAGS_SBB_M(a,b,cin,res,bits,M) do{ SETZSP_M(res,bits,M); \
    if((M)&F_CF) c->cf = (cin) ? ((uint##bits##_t)(a) <= (uint##bits##_t)(b)) \
                               : ((uint##bits##_t)(a) <  (uint##bits##_t)(b)); \
    if((M)&F_AF) c->af = ((((a)^(b)^(res))>>4)&1); \
    if((M)&F_OF) c->of = ((((a)^(b)) & ((a)^(res)))>>((bits)-1))&1; }while(0)
#define FLAGS_SBB(a,b,cin,res,bits) FLAGS_SBB_M(a,b,cin,res,bits,F_ALL)
/* logical: CF=OF=0 */
#define FLAGS_LOG_M(res,bits,M) do{ SETZSP_M(res,bits,M); \
    if((M)&F_CF) c->cf=0; if((M)&F_OF) c->of=0; if((M)&F_AF) c->af=0; }while(0)
#define FLAGS_LOG(res,bits) FLAGS_LOG_M(res,bits,F_ALL)

/* ---- stack ------------------------------------------------------------ */
/* CRITICAL: the operand may depend on esp (example: push [esp+8], push esp).
 * x86 semantics: PUSH first reads the value, then decrements esp.        */
#define PUSH32(v) do{ uint32_t _pv = (uint32_t)(v); c->esp -= 4; MR32(c->esp) = _pv; }while(0)
/* POP: esp is incremented first, the destination address is computed with the NEW esp (pop [esp+N]). */
#define POP32(d)  do{ uint32_t _qv = MR32(c->esp); c->esp += 4; (d) = _qv; }while(0)

/* ---- conditions ------------------------------------------------------- */
#define CC_O   (c->of)
#define CC_NO  (!c->of)
#define CC_B   (c->cf)
#define CC_AE  (!c->cf)
#define CC_E   (c->zf)
#define CC_NE  (!c->zf)
#define CC_BE  (c->cf||c->zf)
#define CC_A   (!c->cf&&!c->zf)
#define CC_S   (c->sf)
#define CC_NS  (!c->sf)
#define CC_P   (c->pf)
#define CC_NP  (!c->pf)
#define CC_L   (c->sf!=c->of)
#define CC_GE  (c->sf==c->of)
#define CC_LE  (c->zf||(c->sf!=c->of))
#define CC_G   (!c->zf&&(c->sf==c->of))


/* ---- x87 FPU stack ----------------------------------------------------
 * NOTE: ARM64 has no 80-bit extended float; we use double (64-bit).
 * This can create small deviations from x86's 80-bit intermediate precision. */
#define ST(n)    (c->st[(c->top + (n)) & 7])
/* CAUTION: the source value must be computed BEFORE top changes.  Since ST(i)
 * is defined relative to top, "fld st(0)" read the empty slot just opened by
 * the preceding FPUSH -> it loaded garbage instead of copying itself.  This
 * broke the CRT's __ftol routine (which starts with fld st(0)), i.e. EVERY
 * float->int conversion in the game.  Symptom: the frame-delta measurement
 * always came out as 1 unit. */
#define FPUSH(v) do{ double _fpv = (double)(v); \
    c->top = (c->top - 1) & 7; c->st[c->top] = _fpv; }while(0)
#define FPOP()   do{ c->top = (c->top + 1) & 7; }while(0)

/* x87 compare -> status word C0/C2/C3 bits (8/10/14) */
#define FSW_C0 (1u<<8)
#define FSW_C1 (1u<<9)
#define FSW_C2 (1u<<10)
#define FSW_C3 (1u<<14)
/* FXAM: the class of v in C3/C2/C0, its sign in C1.  The x87 registers carry no
 * tags here, so "empty" (C3+C0) is never answered; a value counts as denormal
 * by the double's range. */
static inline uint16_t x87_class_bits(double v){
    uint16_t k;
    switch (fpclassify(v)){
    case FP_NAN:       k = FSW_C0;          break;
    case FP_INFINITE:  k = FSW_C2 | FSW_C0; break;
    case FP_ZERO:      k = FSW_C3;          break;
    case FP_SUBNORMAL: k = FSW_C3 | FSW_C2; break;
    default:           k = FSW_C2;          break;
    }
    return signbit(v) ? (uint16_t)(k | FSW_C1) : k;
}
#define X87_EXAMINE(v) (c->fsw = (uint16_t)((c->fsw & (uint16_t)~(FSW_C0|FSW_C1|FSW_C2|FSW_C3)) | x87_class_bits(v)))
/* fnstsw: on real hardware the TOP field (bits 11-13) is read too. */
static inline uint16_t fsw_get(const struct CPU *c){
    return (uint16_t)((c->fsw & (uint16_t)~0x3800u) | (uint16_t)((c->top & 7u) << 11));
}
/* X87OPT: with intra-block x87 register allocation c->top is written at the
 * end of the region; fnstsw takes the (static) top of that moment explicitly. */
static inline uint16_t fsw_get_top(const struct CPU *c, uint32_t top){
    return (uint16_t)((c->fsw & (uint16_t)~0x3800u) | (uint16_t)((top & 7u) << 11));
}
static inline void fcompare(struct CPU *c, double a, double b){
    c->fsw &= (uint16_t)~(FSW_C0|FSW_C2|FSW_C3);
    if (a != a || b != b)      c->fsw |= FSW_C0|FSW_C2|FSW_C3;  /* unordered (NaN) */
    else if (a > b)            ;                                 /* all 0 */
    else if (a < b)            c->fsw |= FSW_C0;
    else                       c->fsw |= FSW_C3;
}
/* FCMPBL: branchless fcompare.  The game code's comparisons are data dependent;
 * the branchy version (NaN, >, <) was often mispredicted.  Same bits: unordered
 * -> C0|C2|C3, a<b -> C0, a==b -> C3, a>b -> none. */
static inline void fcompare_bl(struct CPU *c, double a, double b){
    uint32_t un = (uint32_t)__builtin_isunordered(a, b);
    uint32_t lt = (uint32_t)(a < b), eq = (uint32_t)(a == b);
    c->fsw = (uint16_t)((c->fsw & (uint16_t)~(FSW_C0|FSW_C2|FSW_C3))
                        | ((lt | un) << 8) | (un << 10) | ((eq | un) << 14));
}
/* fcomi / fucomi: write EFLAGS directly */
#define FCOMI(a,b) do{ double _x=(a),_y=(b); \
    if(_x!=_x||_y!=_y){ c->zf=1;c->pf=1;c->cf=1; } \
    else { c->pf=0; c->zf=(_x==_y); c->cf=(_x<_y); } c->of=c->sf=c->af=0; }while(0)

/* ---- EFLAGS packing/unpacking (pushfd/popfd/sahf) --------------------- */
/* AC (bit 18) and ID (bit 21) are not kept in CPU: they only matter to the
 * "does this CPU have CPUID" test (pushfd, flip bit 21, popfd, pushfd, compare).
 * Dropping them made every such test fail, so the game's first-run hardware
 * detection measured 0 MHz and set every graphics option to its minimum. */
extern uint32_t g_eflags_hi;
#define EFLAGS_HI_MASK 0x00240000u
static inline uint32_t eflags_get(const struct CPU *c){
    return 0x00000202u | g_eflags_hi | (c->cf&1u) | ((c->pf&1u)<<2) | ((c->af&1u)<<4)
         | ((c->zf&1u)<<6) | ((c->sf&1u)<<7) | ((c->df&1u)<<10) | ((c->of&1u)<<11);
}
static inline void eflags_set(struct CPU *c, uint32_t f){
    c->cf=f&1u; c->pf=(f>>2)&1u; c->af=(f>>4)&1u;
    c->zf=(f>>6)&1u; c->sf=(f>>7)&1u; c->df=(f>>10)&1u; c->of=(f>>11)&1u;
    g_eflags_hi = f & EFLAGS_HI_MASK;
}

/* ---- string instructions ---------------------------------------------- */
#define DFSTEP(n) (c->df ? -(int32_t)(n) : (int32_t)(n))

/* ---- watchpoint: watch a guest memory address ------------------------- */
extern uint32_t g_watch_addr, g_watch_val, g_watch_prev_ip;
void watch_report_change(uint32_t culprit_ip, uint32_t oldv, uint32_t newv);

/* Instruction-level execution trace: the last 64K guest instruction addresses.
 * Answers "how did we get here" exactly at crash/fault time.              */
#define ITRACE_BITS 16
#define ITRACE_SIZE (1u << ITRACE_BITS)
#define ITRACE_MASK (ITRACE_SIZE - 1u)
extern uint32_t g_itrace[ITRACE_SIZE];
extern uint64_t g_itrace_n;

static inline void watch_check(uint32_t ip){
    g_itrace[(uint32_t)g_itrace_n & ITRACE_MASK] = ip;
    g_itrace_n++;
    if (__builtin_expect(g_watch_addr != 0u, 0)){
        uint32_t v = *(uint32_t*)(g_mem + g_watch_addr);
        if (__builtin_expect(v != g_watch_val, 0)){
            watch_report_change(g_watch_prev_ip, g_watch_val, v);
            g_watch_val = v;
        }
    }
    g_watch_prev_ip = ip;
}
void itrace_dump(int n);

/* ---- function coverage measurement (if generated with COVERAGE=1) -----
 * Goal: answer "which code NEVER runs?".  To show the subsystem that doesn't
 * run directly instead of chasing symptoms.                              */
extern uint32_t *g_cov;   /* CALL COUNT per function */
extern uint32_t g_last_fn;          /* for the sampler: the most recently entered function */
extern int      g_prof_on;          /* 1 if SAMPLE / COVERAGE is on */
/* CAUTION: g_last_fn used to be written UNCONDITIONALLY here.  That means a
 * global store on EVERY guest function entry (millions per second).  Only
 * write it while measuring. */
static inline void cov_hit(uint32_t va){
    if (__builtin_expect(g_prof_on, 0)){
        g_last_fn = va;
        if (g_cov && va >= 0x00401000u && va < 0x00401000u + (3676913u))
            g_cov[va - 0x00401000u]++;
    }
}
void cov_report(const char *path);

/* x87 rounding: the result honors the RC field of the control word (bits 10-11).
 * 00=nearest (even), 01=down, 10=up, 11=toward zero (truncate).
 * The CRT's __ftol routine sets RC to 11 and calls fistp; if we ignore this,
 * every float->int conversion comes out one too high when the fraction >= 0.5. */
static inline double x87_round(uint16_t fcw, double v){
    switch ((fcw >> 10) & 3u){
    case 0:  return nearbyint(v);      /* host mode: nearest, even */
    case 1:  return floor(v);
    case 2:  return ceil(v);
    default: return trunc(v);
    }
}

/* ---- invalid operation results and out-of-range conversions -----------
 * On x86 invalid operations such as 0/0 and sqrt(-x) produce "real indefinite":
 * a quiet NaN with a NEGATIVE sign.  Since C's sqrt/0.0 result is a positive
 * NaN, it must be fixed separately.  If one of the inputs is a NaN, that one
 * propagates.                                                            */
#define X87_INDEF (-__builtin_nan(""))
/* A NaN operand of an x87 operation (Intel SDM vol. 1, table 4-7): the result is
 * that NaN made quiet.  With two NaNs, a quiet one beats a signaling one, and of
 * two of the same kind the larger significand wins (a, the destination, on a tie).
 * ARM simply takes the first operand, so a two-NaN result differed from Windows.
 * Out of line: the NaN path is cold and x87_r2 is inlined at every operation. */
static inline double x87_quiet(double v){
    uint64_t u; memcpy(&u, &v, 8); u |= 1ull << 51; memcpy(&v, &u, 8); return v;
}
static __attribute__((noinline, cold, unused)) double x87_nan2(double a, double b){
    if (a != a && b != b){
        uint64_t ua, ub; memcpy(&ua, &a, 8); memcpy(&ub, &b, 8);
        unsigned qa = (unsigned)(ua >> 51) & 1u, qb = (unsigned)(ub >> 51) & 1u;
        if (qa != qb) return qa ? a : b;
        uint64_t sa = ua & 0x0007FFFFFFFFFFFFull, sb = ub & 0x0007FFFFFFFFFFFFull;
        return x87_quiet(sb > sa ? b : a);
    }
    if (a != a) return x87_quiet(a);
    if (b != b) return x87_quiet(b);
    return X87_INDEF;
}
static inline double x87_r2(double a, double b, double r){
    if (__builtin_expect(r == r, 1)) return r;
    return x87_nan2(a, b);
}
static inline double x87_r1(double a, double r){
    if (r == r) return r;
    if (a != a) return a;
    return X87_INDEF;
}
static inline float sse_r2f(float a, float b, float r){
    if (r == r) return r;
    if (a != a) return a;
    if (b != b) return b;
    return (float)X87_INDEF;
}
static inline float sse_r1f(float a, float r){
    if (r == r) return r;
    if (a != a) return a;
    return (float)X87_INDEF;
}
/* SSE: the first operand's NaN wins whatever the second is (made quiet) */
static inline double sse_r2d(double a, double b, double r){
    if (r == r) return r;
    if (a != a) return x87_quiet(a);
    if (b != b) return x87_quiet(b);
    return X87_INDEF;
}
static inline double sse_r1d(double a, double r){ return x87_r1(a,r); }

/* Out of range / NaN -> "integer indefinite" (the most negative value). */
static inline uint32_t x86_cvt_i32(double v){
    if (!(v >= -2147483648.0 && v <= 2147483647.0)) return 0x80000000u;
    return (uint32_t)(int32_t)v;
}
static inline uint16_t x86_cvt_i16(double v){
    if (!(v >= -32768.0 && v <= 32767.0)) return 0x8000u;
    return (uint16_t)(int16_t)v;
}
static inline uint64_t x86_cvt_i64(double v){
    if (!(v >= -9223372036854775808.0 && v < 9223372036854775808.0))
        return 0x8000000000000000ull;
    return (uint64_t)(int64_t)v;
}

/* ---- 80-bit extended floating point (x87 tbyte) <-> double ------------ */
static inline void f80_store(uint8_t *p, double v){
    /* sign, 15-bit exponent, 64-bit explicit mantissa */
    uint64_t bits; memcpy(&bits, &v, 8);
    uint32_t sign = (uint32_t)(bits >> 63);
    int32_t  exp  = (int32_t)((bits >> 52) & 0x7FF);
    uint64_t man  = bits & 0xFFFFFFFFFFFFFull;
    uint16_t e80; uint64_t m80;
    if (exp == 0x7FF){ e80 = 0x7FFF; m80 = man ? (0xC000000000000000ull | (man << 11)) : 0x8000000000000000ull; }
    else if (exp == 0){
        if (!man){ e80 = 0; m80 = 0; }
        else { int sh = 0; while (!(man & 0x10000000000000ull)) { man <<= 1; sh++; }
               e80 = (uint16_t)(16383 - 1022 - sh); m80 = (man << 11) | 0x8000000000000000ull; }
    } else { e80 = (uint16_t)(exp - 1023 + 16383); m80 = 0x8000000000000000ull | (man << 11); }
    memcpy(p, &m80, 8);
    uint16_t se = (uint16_t)((sign << 15) | e80);
    memcpy(p + 8, &se, 2);
}
static inline double f80_load(const uint8_t *p){
    uint64_t m80; uint16_t se;
    memcpy(&m80, p, 8); memcpy(&se, p + 8, 2);
    uint32_t sign = (uint32_t)(se >> 15);
    int32_t  e80  = (int32_t)(se & 0x7FFF);
    if (e80 == 0 && m80 == 0) return sign ? -0.0 : 0.0;
    if (e80 == 0x7FFF){
        double inf = sign ? -1.0/0.0 : 1.0/0.0;
        return (m80 << 1) ? (0.0/0.0) : inf;
    }
    int32_t exp = e80 - 16383 + 1023;
    uint64_t man = (m80 & 0x7FFFFFFFFFFFFFFFull) >> 11;
    if (exp <= 0)   return sign ? -0.0 : 0.0;
    if (exp >= 0x7FF){ double inf = sign ? -1.0/0.0 : 1.0/0.0; return inf; }
    uint64_t bits = ((uint64_t)sign << 63) | ((uint64_t)(uint32_t)exp << 52) | man;
    double d; memcpy(&d, &bits, 8); return d;
}
#define WCHK(ip) watch_check(ip)
void guest_hook(CPU *c, uint32_t ip);

void mem_init(void);
void mem_commit(uint32_t addr, uint32_t size);
int  mem_load_pe(const char *path);
void mem_fault_handler_install(void);
void mem_altstack_install(void);
extern uint8_t g_cbits[1u<<15];   /* bitmap of committed 16 KB pages */
static inline int mem_is_committed(uint32_t addr){
    uint32_t pg = (addr) >> 14;
    return (g_cbits[pg>>3] >> (pg&7)) & 1;
}

#endif
