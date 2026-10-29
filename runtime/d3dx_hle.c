/* The D3DX9 effect library (statically linked into the game) -- HLE work.
 *
 * Measurement (2026-09-23): in a race frame 48% of the guest thread is inside
 * ID3DXEffect::BeginPass (vtable 0x7b6fe8, slot 66 = sub_0066ace2).
 * Most of it isn't D3D calls but interpreting state expressions (preshaders)
 * on the CPU:
 *
 *   BeginPass 66ace2
 *     -> 6691b1  apply pass
 *        -> 669296  set a single state (type>>24 switch, 100-byte record)
 *           -> 664a2f -> 663a42  evaluate the state expression
 *                          -> 663275 / 662edc  gather inputs
 *                          -> 688e89  preshader VM (x87)
 *        -> 669f40 -> 668e0d -> 668cd0 -> 664a2f ...
 *
 * Stage 0: counter wrappers (D3DXSTAT, D3DXANALYZE, CPUWIN).
 * Stage 1: sub_00664a2f state value memo (on by default, D3DXMEMO=0 turns it
 *          off; D3DXVERIFY=<n> verification with memory comparison).
 * Stage 2: sub_00669296 state assignment memo (on by default, D3DXMEMO2=0
 *          turns it off; D3DXVERIFY2=<n>).  Device call recording is in com.c (dxrec.h).
 * Stage 3: parameter versions (the sub_0065ab24 hook) + sub_006691b1 pass
 *          memo.  Came out WRONG in verification -> OFF by default (D3DXMEMO3=1).
 * Details, measurements and progress log: D3DX_HLE.md.
 *
 * CAUTION: every sub_XXXX here, together with the ones in runtime/overrides.c,
 * must be in genall.py's OVERRIDE list (see D3DX_HLE.md).                */
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <mach/mach_time.h>
#include <string.h>
#include "dxrec.h"

typedef struct { const char *name; uint32_t addr; uint64_t n, ticks; } DxStat;

enum { DX_BEGINPASS, DX_PASSAPPLY, DX_STATEAPPLY, DX_664A2F, DX_EVALSTATE,
       DX_VM, DX_GATHER_A, DX_GATHER_B, DX_660A52, DX_668E0D,
       DX_BEGIN, DX_COMMIT, DX_ENDPASS, DX_END,
       DX_SETMATRIX, DX_SETVECTOR, DX_SETTEXTURE, DX_SETINTARRAY, DX_SETFLOAT,
       DX_SETVECTORARRAY, DX_N };

static DxStat g_dx[DX_N] = {
    [DX_BEGINPASS]      = {"BeginPass",        GA(0x0066ace2u)},
    [DX_PASSAPPLY]      = {"apply pass",      GA(0x006691b1u)},
    [DX_STATEAPPLY]     = {"set state",         GA(0x00669296u)},
    [DX_664A2F]         = {"sub_00664a2f",      GA(0x00664a2fu)},
    [DX_EVALSTATE]      = {"evaluate expr", GA(0x00663a42u)},
    [DX_VM]             = {"preshader VM",      GA(0x00688e89u)},
    [DX_GATHER_A]       = {"gather inputs (0)",   GA(0x00663275u)},
    [DX_GATHER_B]       = {"gather inputs (1,2)", GA(0x00662edcu)},
    [DX_660A52]         = {"sub_00660a52",      GA(0x00660a52u)},
    [DX_668E0D]         = {"sub_00668e0d",      GA(0x00668e0du)},
    [DX_BEGIN]          = {"Begin",             GA(0x00664792u)},
    [DX_COMMIT]         = {"CommitChanges",     GA(0x0066ac27u)},
    [DX_ENDPASS]        = {"EndPass",           GA(0x0065addbu)},
    [DX_END]            = {"End",               GA(0x0065acf5u)},
    [DX_SETMATRIX]      = {"SetMatrix",         GA(0x0066d13au)},
    [DX_SETVECTOR]      = {"SetVector",         GA(0x0066d303u)},
    [DX_SETTEXTURE]     = {"SetTexture",        GA(0x0066d7e9u)},
    [DX_SETINTARRAY]    = {"SetIntArray",       GA(0x0066e8a9u)},
    [DX_SETFLOAT]       = {"SetFloat",          GA(0x0066f501u)},
    [DX_SETVECTORARRAY] = {"SetVectorArray",    GA(0x0066ca38u)},
};

long metal_frames(void);
int g_dx_in_pass = 0;          /* BeginPass/CommitChanges depth (COMSTAT) */
static int  g_dx_on = -1;
static long g_dx_from = 0, g_dx_f0 = -1;   /* D3DXSTAT=<frame>: count from that frame on */
static inline int dx_on(void){
    if (g_dx_on < 0){
        const char *e = getenv("D3DXSTAT");
        g_dx_on = e != NULL; g_dx_from = e ? atol(e) : 0;
    }
    return g_dx_on;
}
/* Reset the counters at the start of the measurement window (only from BeginPass). */
static void dx_window(void){
    if (g_dx_f0 >= 0) return;
    long f = metal_frames();
    if (f < g_dx_from) return;
    for (int i = 0; i < DX_N; i++){ g_dx[i].n = 0; g_dx[i].ticks = 0; }
    g_dx_f0 = f;
}

#define DX_WRAP(idx, fn)                                                    \
    void fn##_orig(CPU *c);                                                 \
    void fn(CPU *c){                                                        \
        if (!dx_on()){ fn##_orig(c); return; }                              \
        if (idx == DX_BEGINPASS) dx_window();                               \
        int inp = (idx == DX_BEGINPASS || idx == DX_COMMIT);                \
        g_dx_in_pass += inp;                                                \
        uint64_t t0 = mach_absolute_time();                                 \
        fn##_orig(c);                                                       \
        g_dx_in_pass -= inp;                                                \
        g_dx[idx].ticks += mach_absolute_time() - t0;                       \
        g_dx[idx].n++;                                                      \
    }

DX_WRAP(DX_BEGINPASS,      sub_0066ace2)
DX_WRAP(DX_VM,             sub_00688e89)
DX_WRAP(DX_GATHER_A,       sub_00663275)
DX_WRAP(DX_GATHER_B,       sub_00662edc)
DX_WRAP(DX_660A52,         sub_00660a52)
DX_WRAP(DX_BEGIN,          sub_00664792)
DX_WRAP(DX_COMMIT,         sub_0066ac27)
DX_WRAP(DX_ENDPASS,        sub_0065addb)
DX_WRAP(DX_END,            sub_0065acf5)
DX_WRAP(DX_SETMATRIX,      sub_0066d13a)
DX_WRAP(DX_SETVECTOR,      sub_0066d303)
DX_WRAP(DX_SETINTARRAY,    sub_0066e8a9)
DX_WRAP(DX_SETFLOAT,       sub_0066f501)
DX_WRAP(DX_SETVECTORARRAY, sub_0066ca38)

/* ---- Analysis: do the inputs of expression evaluation really change? ----
 * D3DXANALYZE=<frame>: from that frame on, on every sub_00663a42 call compares a
 * hash of the state's input parameter data with the previous evaluation.
 *
 * sub_00663a42(this=ecx, a, b)  ret 8
 *   pass/param record : P = a*0x64 + [this+0x2c]
 *   state record      : R = b*0x64 + [P+4]
 *   input records     : [R+0x34] + i*32, i < word[R+0x4e]   (only if [R+0x3c] != 0)
 *     +0  dirty flag (if non-zero gather, then [+0] = [+4])
 *     +0xc -> {data pointer, definition}   definition: +0 type, +4 class,
 *                                          +0x10 elements, +0x14 rows, +0x18 columns
 *     +0x10 -> register definition: word +4 set (0/1/2), +6 index, +8 count */
typedef struct { uint32_t k0, k1; uint64_t h; uint32_t n; } AnSlot;
#define AN_CAP (1u<<16)
static AnSlot *g_an; static long g_an_from = -1;
static uint64_t an_calls, an_first, an_same, an_diff, an_noinput, an_dirty0, an_bytes;
static uint64_t an_hist_inputs[9];

static uint64_t fnv(const uint8_t *p, uint32_t n, uint64_t h){
    for (uint32_t i = 0; i < n; i++){ h ^= p[i]; h *= 0x100000001b3ull; }
    return h;
}
static void analyze_eval(CPU *c){
    if (g_an_from < 0){ const char *e = getenv("D3DXANALYZE"); g_an_from = e ? atol(e) : 1L<<40; }
    if (metal_frames() < g_an_from) return;
    if (!g_an) g_an = calloc(AN_CAP, sizeof *g_an);
    uint32_t th = c->ecx, a = MR32(c->esp+4), b = MR32(c->esp+8);
    uint32_t P = a*0x64u + MR32(th+0x2cu), R = b*0x64u + MR32(P+4u);
    an_calls++;
    uint64_t h = 0xcbf29ce484222325ull; unsigned nin = 0, alldirty0 = 1;
    if (MR32(R+0x3cu)){
        unsigned n = MR16(R+0x4eu); uint32_t base = MR32(R+0x34u);
        for (unsigned i = 0; i < n; i++){
            uint32_t rec = base + i*32u;
            if (!MR32(rec)) continue;             /* not dirty: D3DX won't gather */
            alldirty0 = 0; nin++;
            uint32_t pp = MR32(rec+0xcu), dp = MR32(pp), ds = MR32(pp+4u);
            uint32_t el = MR32(ds+0x10u); if (!el) el = 1;
            uint32_t rows = MR32(ds+0x14u); if (!rows) rows = 1;
            uint32_t bytes = el*rows*16u; if (bytes > 4096u) bytes = 4096u;
            if (dp){ h = fnv(g_mem + dp, bytes, h); an_bytes += bytes; }
            h = fnv((const uint8_t*)&dp, 4, h);
        }
    }
    an_hist_inputs[nin > 8 ? 8 : nin]++;
    if (alldirty0) an_dirty0++;
    if (!nin){ an_noinput++; }
    uint32_t hk = (th * 0x9E3779B1u) ^ (a * 0x85EBCA77u) ^ (b * 0xC2B2AE3Du);
    for (uint32_t j = 0; j < AN_CAP; j++){
        AnSlot *s = &g_an[(hk + j) & (AN_CAP-1)];
        if (s->n && (s->k0 != th || s->k1 != ((a<<16)|(b&0xffff)))) continue;
        if (!s->n){ s->k0 = th; s->k1 = (a<<16)|(b&0xffff); s->h = h; s->n = 1; an_first++;
            if (an_first <= 12){
                uint32_t o = MR32(R+0x38u), vt = MR32(o);
                fprintf(stderr, "[D3DXANALYZE] key this=%08x a=%u b=%u R=%08x obj=%08x vt=%08x vt[3]=%08x"
                        " [R+0x40]=%08x [R+0x44]=%08x P.data=%08x\n", th, a, b, R, o, vt, MR32(vt+12u),
                        MR32(R+0x40u), MR32(R+0x44u), MR32(P));
            }
            return; }
        if (s->h == h) an_same++; else an_diff++;
        s->h = h; s->n++;
        return;
    }
}
void sub_00663a42_orig(CPU *c);
void sub_00663a42(CPU *c){
    if (!dx_on()){ sub_00663a42_orig(c); return; }
    analyze_eval(c);
    uint64_t t0 = mach_absolute_time();
    sub_00663a42_orig(c);
    g_dx[DX_EVALSTATE].ticks += mach_absolute_time() - t0;
    g_dx[DX_EVALSTATE].n++;
}

/* ---- State value memo: sub_00664a2f(this=ecx, a, b)  ret 8 ----
 * Computes the state's value and writes it to P.data + [R+0x48].  For expression
 * states ([R+0x38] != 0, [R+0x30]&4 == 0) the result depends only on the data
 * of a single input (D3DXANALYZE: 95.8% the same).  If the input hash didn't
 * change, skipping the call should change nothing in memory -- D3DXVERIFY
 * measures this.
 *   D3DXVERIFY=<n> : on 1/n of the predicted hits run the original and compare
 *                    the effect object +-256 KB and .data before/after.
 *   D3DXMEMO=0     : turn the memo off (ON by default).
 * Extra guard: on a hit the target dword (P.data+[R+0x48]) and the first dword
 * of the value buffer ([R+8]) must be unchanged since the last real computation;
 * if the effect is recreated at the same address or someone writes to the
 * target, it is recomputed. */
/* Memo record: a FULL copy of the input is kept and compared with memcmp
 * (not a hash -- no collision risk; a byte-wise FNV hash took 8% in the
 * profile).  Inputs exceeding MEMO_BUF aren't memoized, always computed. */
#define MEMO_BUF 256u
typedef struct { uint32_t th, ab, eax, n, dst, dval, vval, len, valid; uint8_t buf[MEMO_BUF]; } MemoSlot;
#define MEMO_CAP 4096u
static MemoSlot *g_memo;
static int g_memo_on = -1, g_verify_n = -1;
static uint64_t m_calls, m_elig, m_big, m_hit, m_skip, m_guard, v_runs, v_clean, v_dirty;
static uint8_t *v_buf0, *v_buf1;
#define V_WIN   0x40000u
#define V_DATA0 0x007e8000u
#define V_DATA1 0x008bc000u

/* Snapshot writer: compares in place against the OLD snapshot in the record
 * slot's buffer, and only copies after the first difference.  Since the hit
 * rate is 90%+, most calls make no copy at all (copying first and then doing
 * memcmp took 9% in the profile). */
typedef struct { uint8_t *d; uint32_t len, cap; int eq; } SnapW;
/* Inline: XOR-OR comparison with 8-byte words.  With a constant n the compiler
 * unrolls it completely; the library memcmp call was 2.6% in the profile. */
static inline __attribute__((always_inline)) int sw_put(SnapW *w, const void *p, uint32_t n){
    if (w->len + n > w->cap) return 0;
    uint8_t *q = w->d + w->len;
    const uint8_t *s = (const uint8_t *)p;
    if (w->eq){
        uint64_t x = 0; uint32_t i = 0;
        for (; i + 8u <= n; i += 8u){ uint64_t a, b; memcpy(&a, q+i, 8); memcpy(&b, s+i, 8); x |= a ^ b; }
        for (; i + 4u <= n; i += 4u){ uint32_t a, b; memcpy(&a, q+i, 4); memcpy(&b, s+i, 4); x |= a ^ b; }
        for (; i < n; i++) x |= (uint64_t)(q[i] ^ s[i]);
        if (x){ w->eq = 0; memcpy(q, s, n); }
    } else memcpy(q, s, n);
    w->len += n;
    return 1;
}

/* The inputs of the evaluation; 0 if the buffer overflows. */
static inline __attribute__((always_inline)) int state_input_snap(SnapW *w, uint32_t th, uint32_t P, uint32_t R){
    uint32_t fix[6] = { th, R, MR32(R+0x38u), MR32(P), MR32(R+0x48u), MR32(R+8u) };
    if (!sw_put(w, fix, sizeof fix)) return 0;
    if (MR32(R+0x3cu)){
        unsigned n = MR16(R+0x4eu); uint32_t base = MR32(R+0x34u);
        for (unsigned i = 0; i < n; i++){
            uint32_t rec = base + i*32u;
            uint32_t pp = MR32(rec+0xcu), dp = MR32(pp), ds = MR32(pp+4u);
            uint32_t el = MR32(ds+0x10u); if (!el) el = 1;
            uint32_t rows = MR32(ds+0x14u); if (!rows) rows = 1;
            uint32_t bytes = dp ? el*rows*16u : 0u;
            uint32_t hdr[2] = { MR32(rec), dp };
            if (bytes > w->cap || !sw_put(w, hdr, sizeof hdr)) return 0;
            if (bytes && !sw_put(w, g_mem + dp, bytes)) return 0;
        }
    }
    return 1;
}

static void verify_diff(uint32_t lo, uint32_t n, const uint8_t *before,
                        uint32_t th, uint32_t P, uint32_t R){
    static int shown = 0;
    for (uint32_t i = 0; i < n && shown < 24; i++){
        if (before[i] == g_mem[lo+i]) continue;
        uint32_t a = lo + i, pd = MR32(P);
        fprintf(stderr, "[D3DXVERIFY] diff %08x  (this%+d  R%+d  P.data%+d)  %02x -> %02x\n",
                a, (int)(a-th), (int)(a-R), (int)(a-pd), before[i], g_mem[a]);
        shown++;
        i |= 3u;                       /* write the same dword once */
    }
}

static inline __attribute__((always_inline)) uint32_t rd32s(uint32_t a){ return (a && mem_is_committed(a) && mem_is_committed(a+3u)) ? MR32(a) : 0u; }

static int g_pctx_n;
static void pctx_note_inputs(uint32_t R);
void sub_00664a2f_orig(CPU *c);
void sub_00664a2f(CPU *c){
    if (g_memo_on < 0){
        const char *m = getenv("D3DXMEMO");
        g_memo_on = m ? atoi(m) != 0 : 1;
        const char *v = getenv("D3DXVERIFY"); g_verify_n = v ? atoi(v) : 0;
        if (g_memo_on || g_verify_n) g_memo = calloc(MEMO_CAP, sizeof *g_memo);
    }
    uint32_t th = c->ecx, a = MR32(c->esp+4u), b = MR32(c->esp+8u);
    uint32_t P = a*0x64u + MR32(th+0x2cu), R = b*0x64u + MR32(P+4u);
    if (g_pctx_n) pctx_note_inputs(R);     /* Stage 3: pass dependency */
    if (!g_memo){ sub_00664a2f_orig(c); return; }
    m_calls++;
    if ((MR8(R+0x30u) & 4) || !MR32(R+0x38u)){ sub_00664a2f_orig(c); return; }
    m_elig++;
    uint32_t ab = (a<<16) | (b & 0xffffu);
    uint32_t hk = (th * 0x9E3779B1u) ^ (ab * 0x85EBCA77u);
    MemoSlot *s = NULL;
    for (uint32_t j = 0; j < 64u; j++){
        MemoSlot *t = &g_memo[(hk + j) & (MEMO_CAP-1)];
        if (!t->n || (t->th == th && t->ab == ab)){ s = t; break; }
    }
    if (!s){ sub_00664a2f_orig(c); return; }
    int mine = s->n && s->th == th && s->ab == ab && s->valid;
    SnapW w = { s->buf, 0, MEMO_BUF, mine };
    if (!state_input_snap(&w, th, P, R)){
        m_big++; s->valid = 0; s->th = th; s->ab = ab; s->n++;
        sub_00664a2f_orig(c); return; }
    uint32_t dst = MR32(P) + MR32(R+0x48u), vb = MR32(R+8u);
    int same = mine && w.eq && w.len == s->len;
    int hit = same && s->dst == dst && rd32s(dst) == s->dval && rd32s(vb) == s->vval;
    if (same && !hit) m_guard++;
    if (hit){
        m_hit++;
        if (g_verify_n && (m_hit % (uint64_t)g_verify_n) == 0){
            /* window: around this, only mapped 16 KB pages */
            uint32_t lo = th & ~0x3fffu, hi = lo + 0x4000u, dn = V_DATA1 - V_DATA0;
            while (lo - 0x4000u >= th - V_WIN && mem_is_committed(lo - 0x4000u)) lo -= 0x4000u;
            while (hi + 0x4000u <= th + V_WIN && mem_is_committed(hi)) hi += 0x4000u;
            uint32_t n = hi - lo;
            if (!v_buf0){ v_buf0 = malloc(2u*V_WIN + 0x8000u); v_buf1 = malloc(dn); }
            memcpy(v_buf0, g_mem + lo, n); memcpy(v_buf1, g_mem + V_DATA0, dn);
            uint32_t esp0 = c->esp;
            sub_00664a2f_orig(c);
            v_runs++;
            /* this+0x114 (saved FPU CW) and +0x118 bit0 (CW saved):
             * 663a42 sets the FPU precision on the first evaluation inside
             * BeginPass, and the end of BeginPass restores it.  Since the skipped
             * evaluation never touches the CW, no restore is needed either -- a
             * harmless difference. */
            { uint32_t off = th + 0x114u - lo; memcpy(v_buf0 + off, g_mem + th + 0x114u, 8); }
            int d0 = memcmp(v_buf0, g_mem + lo, n) != 0;
            int d1 = memcmp(v_buf1, g_mem + V_DATA0, dn) != 0;
            if (d0 || d1 || c->eax != s->eax){
                v_dirty++;
                if (d0) verify_diff(lo, n, v_buf0, th, P, R);
                if (d1) verify_diff(V_DATA0, dn, v_buf1, th, P, R);
                if (c->eax != s->eax)
                    fprintf(stderr, "[D3DXVERIFY] eax diff %08x -> %08x\n", s->eax, c->eax);
            } else v_clean++;
            (void)esp0;
            s->eax = c->eax; s->dval = rd32s(dst); s->vval = rd32s(vb);
            return;
        }
        if (g_memo_on){
            m_skip++;
            POP32(c->eip); c->esp += 8u; c->eax = s->eax;
            return;
        }
    }
    s->th = th; s->ab = ab; s->valid = 0; s->n++;     /* the buffer is already the new snapshot */
    sub_00664a2f_orig(c);
    s->len = w.len; s->eax = c->eax; s->valid = 1;
    s->dst = dst; s->dval = rd32s(dst); s->vval = rd32s(vb);
}

/* ---- Stage 3: parameter versions ----
 * Parameter data only changes through Set*, and all the Set*s reach D3DX's
 * "parameter changed" point sub_0065ab24 (20 call sites, all vtable Set* slots
 * + CloneEffect + the parameter block path).  There a global counter is written
 * to the 64-byte lines of the changed data.  Since the effect pool shares data
 * between effects, the key is the data ADDRESS.
 * A pass's dependencies = the lines its states read; if none of them changed
 * since record time, the pass produces the same output.                  */
#define PV_CAP (1u<<16)
typedef struct { uint32_t line; uint32_t ep; } PvSlot;
static PvSlot  g_pv[PV_CAP];
static uint32_t g_pepoch = 1;
static int      g_pv_on = 0;         /* keep versions only while Stage 3 is on */
static uint64_t pv_bumps, pv_full;

static inline uint32_t pv_get(uint32_t line){
    uint32_t h = line * 0x9E3779B1u;
    for (uint32_t j = 0; j < 32u; j++){
        PvSlot *t = &g_pv[(h + j) & (PV_CAP-1)];
        if (t->line == line) return t->ep;
        if (!t->line) return 0;
    }
    return 0xFFFFFFFFu;                  /* table full: always "changed" */
}
static inline void pv_set(uint32_t line, uint32_t ep){
    uint32_t h = line * 0x9E3779B1u;
    for (uint32_t j = 0; j < 32u; j++){
        PvSlot *t = &g_pv[(h + j) & (PV_CAP-1)];
        if (t->line == line || !t->line){ t->line = line; t->ep = ep; return; }
    }
    pv_full++;
}
/* The data range of a parameter slot (upper bound estimate: 16 bytes per row,
 * matrix 4x16).  Definition: +0x10 elements, +0x14 rows. */
static inline void slot_range(uint32_t sl, uint32_t *dp, uint32_t *n){
    uint32_t d = MR32(sl), ds = MR32(sl+4u);
    uint32_t el = MR32(ds+0x10u); if (!el) el = 1;
    uint32_t rows = MR32(ds+0x14u); if (!rows) rows = 1;
    uint32_t b = el*rows*16u; if (b > 4096u) b = 4096u;
    *dp = d; *n = d ? b : 0u;
}

/* Native 66056c verification: the 65ab24 calls the original makes directly
 * (effect, slot) are recorded in order; nested calls aren't recorded. */
#define AB_LOG 64
static int      g_ab_log, g_ab_n;
static uint32_t g_ab_seq[AB_LOG][2];

void sub_0065ab24_orig(CPU *c);
void sub_0065ab24(CPU *c){
    uint32_t sl = MR32(c->esp+4u), dp, n;
    int lg = g_ab_log;
    if (lg){
        if (g_ab_n < AB_LOG){ g_ab_seq[g_ab_n][0] = c->ecx; g_ab_seq[g_ab_n][1] = sl; }
        g_ab_n++; g_ab_log = 0;
    }
    if (g_pv_on && sl && mem_is_committed(sl) && mem_is_committed(sl+7u)){
        slot_range(sl, &dp, &n);
        if (n){
            uint32_t ep = ++g_pepoch;
            for (uint32_t l = dp >> 6; l <= (dp + n - 1u) >> 6; l++) pv_set(l, ep);
            pv_bumps++;
        }
    }
    sub_0065ab24_orig(c);
    g_ab_log = lg;
}

/* ---- Native helpers (Option 1) ----
 * C that produces exactly the same result in place of the lifted x87/flag code.
 *   D3DXNATIVE=0     : turn off (on by default)
 *   D3DXNVERIFY=<n>  : on every nth call compute the native result, run the
 *                      original, compare (matrix: 64 bytes + eax;
 *                      pool: the 65ab24 call sequence + eax).           */
static int g_dxn_on = -1, g_dxn_v = 0;
static uint64_t mm_calls, mm_slow, mmv_runs, mmv_bad, pl_calls, plv_runs, plv_bad;
static inline int dxn_on(void){
    if (g_dxn_on < 0){
        const char *e = getenv("D3DXNATIVE"); g_dxn_on = e ? atoi(e) != 0 : 1;
        const char *v = getenv("D3DXNVERIFY"); g_dxn_v = v ? atoi(v) : 0;
    }
    return g_dxn_on;
}

/* D3DXMatrixMultiply(out, m1, m2)  stdcall, ret 12, eax = out.
 * Original (x87): for every output p_k = a_ik*b_kj (double, the product of two
 * floats is exact), t = (p2+p0) + (p1+p3), out = (float)t.  Since the row path
 * (m2 != out) reads the m1 row and the column path (m2 == out, m1 != out) reads
 * the m2 column before writing, the result is always the product of the
 * original inputs; for m1 == m2 == out, m2 is copied to the stack and the row
 * path is taken.  The only difference between the two paths is the operand
 * order in the product -- it only matters for NaN propagation.
 * Fast path: if all 32 inputs are finite, the products/sums can't produce NaN
 * or overflow (|a*b| < 2^256), and the x87_r2 checks are vacuous.  Otherwise,
 * and on partial overlap, the original runs.  FMA contraction must be OFF. */
#pragma clang fp contract(off)
static inline __attribute__((always_inline)) int mm_ovl(uint32_t a, uint32_t b){
    return a != b && a - b + 63u < 127u;   /* |a-b| < 64 */
}
static inline __attribute__((always_inline)) int mm_load(float *M, uint32_t ga){
    uint32_t u[16], bad = 0;
    memcpy(u, g_mem + ga, 64);
    for (int k = 0; k < 16; k++) bad |= ((u[k] & 0x7f800000u) == 0x7f800000u);
    memcpy(M, u, 64);
    return !bad;
}
static inline __attribute__((always_inline)) void mm_mul(float *R, const float *A, const float *B){
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++){
            double p0 = (double)A[i*4+0] * (double)B[0*4+j];
            double p1 = (double)A[i*4+1] * (double)B[1*4+j];
            double p2 = (double)A[i*4+2] * (double)B[2*4+j];
            double p3 = (double)A[i*4+3] * (double)B[3*4+j];
            double s1 = p2 + p0;
            double s2 = p1 + p3;
            R[i*4+j] = (float)(s1 + s2);
        }
}
void sub_0067193e_orig(CPU *c);
void sub_0067193e(CPU *c){
    if (!dxn_on()){ sub_0067193e_orig(c); return; }
    uint32_t sp = c->esp, out = MR32(sp+4u), m1 = MR32(sp+8u), m2 = MR32(sp+12u);
    float A[16], B[16], R[16];
    mm_calls++;
    if (mm_ovl(out, m1) || mm_ovl(out, m2) || !mm_load(A, m1) || !mm_load(B, m2)){
        mm_slow++; sub_0067193e_orig(c); return; }
    mm_mul(R, A, B);
    if (g_dxn_v && (mm_calls % (uint64_t)g_dxn_v) == 0){
        sub_0067193e_orig(c);
        mmv_runs++;
        if (memcmp(g_mem + out, R, 64) || c->eax != out){
            if (mmv_bad++ < 8)
                fprintf(stderr, "[D3DXNVERIFY] matrix diff out=%08x m1=%08x m2=%08x eax=%08x\n",
                        out, m1, m2, c->eax);
        }
        return;
    }
    memcpy(g_mem + out, R, 64);
    /* the original's ecx/edx at exit (meaningless to the caller but cheap) */
    if (m2 != out){ c->ecx = m1 + 0x40u; c->edx = m2; }
    else if (m1 == out){ c->ecx = m1 + 0x40u; c->edx = sp - 0x44u; }
    else { c->ecx = m1; c->edx = m2; }
    c->eax = out;
    POP32(c->eip); c->esp += 12u;
}
#pragma clang fp contract(on)

/* Parameter-changed notification, sub_0066056c(this=ecx, slot) ret 4.
 * If the parameter is shared in the effect pool, for every active effect in the
 * pool (byte +0x119 bit 0) 65ab24(effect, the slot in that effect), then itself.
 *   pobj = [[this+0xb0] + [slot+0x14]*4],  head = [pobj+0x40]  (a ring list,
 *   node: +0 effect, +4 slot, +0x10 next), pool = [this+0x38]:
 *   +8 effect count, +0xc single effect.
 * The original's prologue (pushing ebp/ebx/esi/edi) is done exactly: 65ab24
 * sees the same esp.  65ab24 only reads ecx and [esp+4]. */
static inline __attribute__((always_inline)) uint32_t pl_call(CPU *c, uint32_t eff, uint32_t sl, uint32_t ret){
    c->ecx = eff; PUSH32(sl); PUSH32(ret); sub_0065ab24(c); return c->eax;
}
/* Dry run (verification): the sequence to execute if all calls return >= 0. */
static int pl_plan(uint32_t th, uint32_t sl, uint32_t seq[][2]){
    int n = 0;
    #define PL_ADD(e, s) do{ if (n < AB_LOG){ seq[n][0] = (e); seq[n][1] = (s); } n++; }while(0)
    uint32_t pobj = MR32(MR32(th+0xb0u) + MR32(sl+0x14u)*4u), head = MR32(pobj+0x40u);
    if (!head){ PL_ADD(th, sl); return n; }
    uint32_t pool = MR32(th+0x38u), cnt = MR32(pool+8u);
    if (cnt <= 1u){
        if (!cnt) return 0;
        uint32_t e = MR32(pool+0xcu);
        if (e == th){ PL_ADD(th, sl); return n; }
        if (e) for (uint32_t q = MR32(head+0x10u); q != head; q = MR32(q+0x10u))
            if (MR32(q) == e){ PL_ADD(e, MR32(q+4u)); return n; }
    }
    for (uint32_t q = MR32(head+0x10u); q != head; q = MR32(q+0x10u)){
        uint32_t ef = MR32(q);
        if (ef && (MR8(ef+0x119u) & 1u)) PL_ADD(ef, MR32(q+4u));
    }
    if (MR8(th+0x119u) & 1u) PL_ADD(th, sl);
    #undef PL_ADD
    return n;
}
void sub_0066056c_orig(CPU *c);
void sub_0066056c(CPU *c){
    if (!dxn_on()){ sub_0066056c_orig(c); return; }
    uint32_t th = c->ecx, sl = MR32(c->esp+4u);
    pl_calls++;
    if (g_dxn_v && (pl_calls % (uint64_t)g_dxn_v) == 0){
        static uint32_t plan[AB_LOG][2];
        int pn = pl_plan(th, sl, plan);
        int lg = g_ab_log; g_ab_log = 1; g_ab_n = 0;
        sub_0066056c_orig(c);
        g_ab_log = lg;
        plv_runs++;
        int bad = pn != g_ab_n;
        for (int i = 0; !bad && i < pn && i < AB_LOG; i++)
            bad = plan[i][0] != g_ab_seq[i][0] || plan[i][1] != g_ab_seq[i][1];
        if (!pn && c->eax) bad = 1;
        if (bad && plv_bad++ < 8)
            fprintf(stderr, "[D3DXNVERIFY] pool diff this=%08x slot=%08x plan %d calls, original %d (eax %08x)\n",
                    th, sl, pn, g_ab_n, c->eax);
        return;
    }
    uint32_t sebp = c->ebp, sebx = c->ebx, sesi = c->esi, sedi = c->edi, r;
    PUSH32(sebp); uint32_t fp = c->esp; PUSH32(sebx); PUSH32(sesi); PUSH32(sedi);
    c->ebp = fp; c->ebx = th; c->edx = sl;
    uint32_t pobj = MR32(MR32(th+0xb0u) + MR32(sl+0x14u)*4u), head = MR32(pobj+0x40u);
    c->edi = head;
    if (!head){ r = pl_call(c, th, sl, GA(0x6605feu)); goto done; }
    {
    uint32_t pool = MR32(th+0x38u), cnt = MR32(pool+8u);
    if (cnt <= 1u){
        if (!cnt){ r = 0; goto done; }
        uint32_t e = MR32(pool+0xcu);
        if (e == th){ r = pl_call(c, th, sl, GA(0x6605feu)); goto done; }
        if (e) for (uint32_t q = MR32(head+0x10u); q != head; q = MR32(q+0x10u))
            if (MR32(q) == e){ r = pl_call(c, e, MR32(q+4u), GA(0x6605feu)); goto done; }
    }
    for (uint32_t q = MR32(head+0x10u); q != head; q = MR32(q+0x10u)){
        uint32_t ef = MR32(q);
        if (ef && (MR8(ef+0x119u) & 1u)){
            c->esi = q;
            r = pl_call(c, ef, MR32(q+4u), GA(0x6605ddu));
            if ((int32_t)r < 0) goto done;
        }
    }
    if (!(MR8(th+0x119u) & 1u)){ r = 0; goto done; }
    r = pl_call(c, th, MR32(fp+8u), GA(0x6605feu));
    }
done:
    c->eax = r;
    c->edi = sedi; c->esi = sesi; c->ebx = sebx; c->ebp = sebp; c->esp = fp + 4u;
    POP32(c->eip); c->esp += 4u;
}

/* SetMatrix's conversion step, sub_0065cf44(def, dest, src) ret 12.
 * Writes the 4x4 matrix TRANSPOSED, by parameter type ([def+0]):
 *   3 float : dest[4j+i] = src[4i+j] (bit copy)
 *   1 bool  : dest[4j+i] = (src[4i+j] == [0x7d7d3c]) ? 0 : 1   (NaN -> 1)
 *   2 int   : with __ftol (0x75dd28) -- stays in the original
 *   other   : 0x80004005; stays in the original.
 * Success eax = 0.  If dest/src overlap, the original (an in-place transpose differs). */
static uint64_t sm_calls, sm_slow, smv_runs, smv_bad;
void sub_0065cf44_orig(CPU *c);
void sub_0065cf44(CPU *c){
    if (!dxn_on()){ sub_0065cf44_orig(c); return; }
    uint32_t sp = c->esp, t = MR32(MR32(sp+4u)), dst = MR32(sp+8u), src = MR32(sp+12u);
    sm_calls++;
    if ((t != 3u && t != 1u) || dst - src + 63u < 127u){ sm_slow++; sub_0065cf44_orig(c); return; }
    uint32_t S[16], D[16];
    memcpy(S, g_mem + src, 64);
    double z = (double)MRF32(GA(0x7d7d3cu)), last = 0;
    if (t == 3u){
        for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) D[4*j+i] = S[4*i+j];
    } else {
        for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++){
            float v; memcpy(&v, &S[4*i+j], 4); last = (double)v;
            D[4*j+i] = (last == z) ? 0u : 1u;
        }
    }
    if (g_dxn_v && (sm_calls % (uint64_t)g_dxn_v) == 0){
        sub_0065cf44_orig(c);
        smv_runs++;
        if ((memcmp(g_mem + dst, D, 64) || c->eax) && smv_bad++ < 8)
            fprintf(stderr, "[D3DXNVERIFY] SetMatrix conversion diff type %u dest=%08x src=%08x eax=%08x\n",
                    t, dst, src, c->eax);
        return;
    }
    memcpy(g_mem + dst, D, 64);
    if (t == 3u){ c->ecx = src + 0x30u; }
    else { c->ecx = dst + 0x4cu; fcompare(c, z, last); }
    c->edx = 0; c->eax = 0;
    POP32(c->eip); c->esp += 12u;
}

/* Context of the pass being recorded: the state wrapper reports dependencies. */
#define PS_DEPS 48
typedef struct PassCtx { int elig; uint32_t tmask, ndep; uint32_t dep[PS_DEPS]; } PassCtx;
static PassCtx *g_pctx[8];
static int      g_pctx_n;
static inline void pctx_dep_range(uint32_t dp, uint32_t n){
    if (!g_pctx_n || !n) return;
    PassCtx *x = g_pctx[g_pctx_n-1];
    for (uint32_t l = dp >> 6; l <= (dp + n - 1u) >> 6; l++){
        uint32_t k = 0;
        for (; k < x->ndep; k++) if (x->dep[k] == l) break;
        if (k < x->ndep) continue;
        if (x->ndep >= PS_DEPS){ x->elig = 0; return; }
        x->dep[x->ndep++] = l;
    }
}
/* The parameter data a state reads: expression inputs + value buffer.
 * Called from both the state assignment (669296) and value retrieval (664a2f)
 * wrappers; the sub-states of sampler objects (type 14/15, 668e0d/668cd0)
 * don't go through 669296 but get their values with 664a2f. */
static void pctx_note_inputs(uint32_t R){
    if (MR32(R+0x38u) && !(MR8(R+0x30u) & 4) && MR32(R+0x3cu)){
        unsigned n = MR16(R+0x4eu); uint32_t base = MR32(R+0x34u);
        for (unsigned i = 0; i < n; i++){
            uint32_t dp, sz; slot_range(MR32(base + i*32u + 0xcu), &dp, &sz);
            pctx_dep_range(dp, sz);
        }
    }
    { uint32_t vb = MR32(R+8u); if (vb) pctx_dep_range(vb, 16u); }
}
static void pctx_note_state(uint32_t th, uint32_t P, uint32_t R, uint32_t type){
    if (!g_pctx_n) return;
    (void)th; (void)P;
    g_pctx[g_pctx_n-1]->tmask |= 1u << (type & 31u);
    if (type == 0 || type > 15){ g_pctx[g_pctx_n-1]->elig = 0; return; }
    pctx_note_inputs(R);
}

/* ---- Stage 2: state assignment memo, sub_00669296(this=ecx, a, b, elem) ret 0xc ----
 * Applying a state to the device.  If the input snapshot is the same and ALL
 * the D3D calls it made in its last real run would now have no effect
 * (com.c dxrec_redundant), the function is skipped.
 *   D3DXMEMO2=0     : turn off (on by default)
 *   D3DXVERIFY2=<n> : on 1/n of the predicted hits run the original with
 *                     recording; the memory window + "every call had no
 *                     effect" are checked. */
#define M2_BUF 640u
typedef struct {
    uint32_t th, ab, elem, eax, n, len, type, valid;
    uint8_t  buf[M2_BUF];
    DxRec    rec;
    DxCall   rcall[8];
    uint8_t  rdata[256];
} M2Slot;
#define M2_CAP 8192u
static M2Slot *g_m2;
static int g_m2_on = -1, g_v2_n = 0;
static uint64_t m2_calls, m2_excl, m2_same, m2_hit, m2_skip, m2_norec, m2_devchg, m2_big;
static uint64_t v2_runs, v2_clean, v2_mem, v2_dev, v2_eax;
static uint64_t m2_type_calls[32], m2_type_hit[32], v2_type_bad[32];

static inline __attribute__((always_inline)) int snap_bytes(SnapW *w, uint32_t ga, uint32_t n){
    int ok = ga && mem_is_committed(ga) && mem_is_committed(ga + n - 1u);
    uint32_t tag = ok ? ga : 0u;
    if (!sw_put(w, &tag, 4)) return 0;
    return ok ? sw_put(w, g_mem + ga, n) : 1;
}
static inline __attribute__((always_inline)) int state_apply_snap(SnapW *w, uint32_t th, uint32_t a, uint32_t b, uint32_t el,
                            uint32_t P, uint32_t R){
    uint32_t fix[11] = { th, a, b, el, MR32(th+0x3cu), MR32(th+0x40u), MR32(th+0x44u),
                         MR32(th+0x118u) & ~1u, MR32(th+0xb0u), MR32(P), MR32(P+4u) };
    if (!sw_put(w, fix, sizeof fix)) return 0;
    if (!sw_put(w, g_mem + R, 0x64u)) return 0;
    if (!snap_bytes(w, MR32(P) + MR32(R+0x48u), 16u)) return 0;
    if (!snap_bytes(w, MR32(R+8u), 16u)) return 0;
    { uint32_t ot = MR32(th+0xb0u), ix = MR32(R+0x1cu);
      uint32_t oa = (ix < 0x10000u) ? ot + ix*4u : 0u;
      if (!snap_bytes(w, oa, 4u)) return 0; }
    if (MR32(R+0x38u) && !(MR8(R+0x30u) & 4))
        if (!state_input_snap(w, th, P, R)) return 0;
    return 1;
}

/* ---- Stage 4a: native state applier, sub_00669296 types 1/2/3 ----
 * The exact equivalent of the lifted code, on D3DX's own memory structures
 * (the bookkeeping doesn't change, every unsupported path falls back to the
 * original):
 *   P = a*0x64 + [this+0x2c], R = b*0x64 + [P+4], value = [[R+0x48] + [P]]
 *   if el == 0xffff then el = word[R+0x54]
 *   R+0 != 0 (dirty): R+0 = R+4; 664a2f(this, a, b) refreshes the value (<0: return)
 *   type = [R+0x58]>>24, state = [R+0x58]&0xffffff, device = [this+8]:
 *     1 SetRenderState(state, value)            vtable +0xe4
 *     2 SetTextureStageState(el, state, value)  +0x10c
 *     3 nothing if [this+0x3c]==1; otherwise SetSamplerState(el, state, value) +0x114
 *   Type 3 dirty + [this+0x3c]==4 + el==0xffff: a loop over every shader using
 *   the sampler -> original.  If the device call has no effect (com.c dxn_skip)
 *   it isn't made at all; otherwise it goes to com_call through guest dispatch.
 *   D3DXNATIVE4=0     : turn off
 *   D3DXN4VERIFY=<n>  : on 1/n of eligible calls the native code does a dry run,
 *                       memory + the call list are saved, the original runs with
 *                       recording, and they are compared. */
int dxn_skip(int kind, uint32_t a0, uint32_t a1, uint32_t a2);
void sub_00664a2f(CPU *c);
void sub_00669296_orig(CPU *c);
void dispatch_indirect(CPU*, uint32_t);
static int g_n4_on = -1, g_n4_v = 0;
static uint64_t n4_calls, n4_dirty, n4_issued, n4_skipped;
#define NDRY 64
typedef struct { uint64_t runs, bad, norec; } VStat;
#define VEXT 40
static void nat_verify(CPU *c, int (*nat)(CPU*), void (*orig)(CPU*), uint32_t th,
                       const uint32_t *ext, int next, VStat *vs, const char *tag, uint32_t info);
static int n4_dry, n4_dryn; static uint32_t n4_dryk[NDRY][4];
/* kind: 1 SetRenderState(a0,a1) 2 SetTextureStageState 3 SetSamplerState
 *       (a0,a1,a2) 4 SetTexture(a0,a1) 5/6 Set{V,P}SConstantF(start, data,
 *       count) 7/8 Set{Vertex,Pixel}Shader(a0) 9/10 ...ConstantI 11/12 ...ConstantB
 *       (9..12 have no no-effect check: always made) */
static const int n4_nargs[13] = { 0, 2, 3, 3, 2, 3, 3, 1, 1, 3, 3, 3, 3 };
/* The inline equivalent of com.c dxrec_call_redundant for types 1..3 (same rule);
 * -1 while record/replay/trace is on: the slow path (dxn_skip) decides. */
extern uint32_t g_rs[256], g_tss_raw[8][40], g_smp_raw[16][20];
extern int g_rs_dirty, g_sb_rec, g_sb_replay, g_dxrec_depth, g_fxtr;
static inline int n4_red_fast(int kind, uint32_t a0, uint32_t a1, uint32_t a2){
    if (g_sb_rec | g_sb_replay | g_dxrec_depth | g_fxtr) return -1;
    switch (kind){
    case 1: return a0 >= 256u || (g_rs[a0] == a1 && !g_rs_dirty);
    case 2: return !(a0 < 8u && a1 < 40u) || g_tss_raw[a0][a1] == a2;
    case 3: return !(a0 < 16u && a1 < 20u) || g_smp_raw[a0][a1] == a2;
    }
    return -1;
}
static inline uint32_t n4_dev(CPU *c, uint32_t dev, int kind, uint32_t off, uint32_t ret,
                              uint32_t a0, uint32_t a1, uint32_t a2){
    if (n4_dry){
        if (n4_dryn < NDRY){ n4_dryk[n4_dryn][0] = (uint32_t)kind; n4_dryk[n4_dryn][1] = a0;
                             n4_dryk[n4_dryn][2] = a1; n4_dryk[n4_dryn][3] = a2; }
        n4_dryn++; return 0;
    }
    int f = n4_red_fast(kind, a0, a1, a2);
    if (f > 0 || (f < 0 && dxn_skip(kind, a0, a1, a2))){ n4_skipped++; return 0; }
    n4_issued++;
    uint32_t fn = MR32(MR32(dev) + off);
    int na = n4_nargs[kind];
    if (na >= 3) PUSH32(a2);
    if (na >= 2) PUSH32(a1);
    PUSH32(a0); PUSH32(dev); PUSH32(ret);
    dispatch_indirect(c, fn);
    return c->eax;
}
/* A per-state memo for dirty expression states (Stage 1's logic, small slots
 * indexed directly by R): if the input parameters' data, the target dword and
 * the first dword of the value buffer haven't changed since the last real
 * 664a2f, the evaluation would write the same result -> skip.  At most 2
 * inputs / 64 bytes; larger ones are always evaluated. */
typedef struct { uint32_t R, th, dst, dval, vb, vval, eax, n, base; uint32_t rec[2], pp[2], dp[2], nb[2]; uint8_t d[96]; } N4Memo;
#define N4M_CAP 2048u
static N4Memo *g_n4m;
static int g_n4m_on = -1;
static uint64_t n4m_hit, n4m_miss, n4m_big, n4m_why[4];
void sub_00664a2f_orig(CPU *c);
/* 1: inputs gathered (rec, pp, dp, nb filled); 0: not memoizable */
static inline int n4m_inputs(uint32_t R, uint32_t *n, uint32_t *rec, uint32_t *pp, uint32_t *dp, uint32_t *nb){
    if (!MR32(R+0x38u) || (MR8(R+0x30u) & 4)){ n4m_why[0]++; return 0; }
    if (!MR32(R+0x3cu)){ n4m_why[1]++; return 0; }
    uint32_t k = MR16(R+0x4eu), base = MR32(R+0x34u), tot = 0;
    if (k == 0 || k > 2u){ n4m_why[2]++; return 0; }
    for (uint32_t i = 0; i < k; i++){
        uint32_t r = base + i*32u, p = MR32(r + 0xcu), d = MR32(p), ds = MR32(p+4u);
        uint32_t el = MR32(ds+0x10u); if (!el) el = 1;
        uint32_t rows = MR32(ds+0x14u); if (!rows) rows = 1;
        uint32_t b = d ? el*rows*16u : 0u;
        tot += b; if (tot > 96u){ n4m_why[3]++; return 0; }
        rec[i] = r; pp[i] = p; dp[i] = d; nb[i] = b;
    }
    *n = k; return 1;
}
static inline uint32_t n4_eval(CPU *c, uint32_t th, uint32_t a, uint32_t b, uint32_t R, uint32_t P, uint32_t ret){
    if (g_n4m_on < 0){ const char *e = getenv("D3DXN4MEMO"); g_n4m_on = e ? atoi(e) != 0 : 1;
                       if (g_n4m_on) g_n4m = calloc(N4M_CAP, sizeof *g_n4m); }
    N4Memo *s = NULL;
    uint32_t dst = MR32(R+0x48u) + MR32(P), vb = MR32(R+8u);
    if (g_n4m){
        s = &g_n4m[((R >> 2) * 0x9E3779B1u) >> 21];
        /* hit: same state, the input records point to the same parameter,
         * the data and target/value buffer are as in the last real evaluation */
        if (s->R == R && s->th == th && s->dst == dst && s->vb == vb && s->n
            && MR16(R+0x4eu) == s->n && MR32(R+0x34u) == s->base
            && MR32(s->rec[0] + 0xcu) == s->pp[0] && MR32(s->pp[0]) == s->dp[0]
            && (s->n < 2 || (MR32(s->rec[1] + 0xcu) == s->pp[1] && MR32(s->pp[1]) == s->dp[1]))
            && rd32s(dst) == s->dval && rd32s(vb) == s->vval
            && !memcmp(s->d, g_mem + s->dp[0], s->nb[0])
            && (s->n < 2 || !memcmp(s->d + s->nb[0], g_mem + s->dp[1], s->nb[1]))){
            n4m_hit++;
            return s->eax;
        }
    }
    uint32_t n = 0, rec[2] = {0,0}, pp[2] = {0,0}, dp[2] = {0,0}, nb[2] = {0,0};
    int ok = g_n4m && n4m_inputs(R, &n, rec, pp, dp, nb);
    if (g_n4m){ if (ok) n4m_miss++; else n4m_big++; }
    c->ecx = th; PUSH32(b); PUSH32(a); PUSH32(ret);
    if (g_n4m) sub_00664a2f_orig(c); else sub_00664a2f(c);
    uint32_t r = c->eax;
    if (ok){
        s->R = R; s->th = th; s->dst = dst; s->vb = vb; s->n = n; s->eax = r; s->base = MR32(R+0x34u);
        for (uint32_t i = 0; i < 2; i++){ s->rec[i] = rec[i]; s->pp[i] = pp[i]; s->dp[i] = dp[i]; s->nb[i] = nb[i]; }
        memcpy(s->d, g_mem + dp[0], nb[0]); if (n > 1) memcpy(s->d + nb[0], g_mem + dp[1], nb[1]);
        s->dval = rd32s(dst); s->vval = rd32s(vb);
    } else if (s) s->R = 0;
    return r;
}
/* ---- Stage 4c: native sampler applier, sub_00668cd0(this, P, Rstart, Rend) ret 12 ----
 * For every sampler binding state R in [Rstart, Rend):
 *   vp = [R+0x48] + [P]; record = w[vp+2] + w[vp] - 1, w[R+0x56] consecutive
 *   records, stage = w[R+0x54] (+1 per record).  The states of each record (P2):
 *     type 3: if [R2] is dirty 664a2f(this, record no, k), then [R2] = [R2+4];
 *             SetSamplerState(stage, [R2+0x58]&0xffffff, [[R2+0x48]+[P2]])
 *     type 9: o = [[this+0xb0] + [R2+0x1c]*4], i = [[R2+8]],
 *             if [o+0x40] exists i = [[o+0x40]+8][i]; SetTexture(stage, [[this+0x18] + i*24 + 4])
 *   On a <0 result stop and return; 0 at the end.  A record with another type -> original. */
static uint64_t n6_calls;
static VStat n6vs;
static int g_n6_on = -1;
static int n6_samp(CPU *c, uint32_t th, uint32_t P, uint32_t Rb, uint32_t Re, uint32_t *res){
    uint32_t base = MR32(th+0x2cu);
    for (uint32_t R = Rb; R < Re; R += 0x64u){
        uint32_t vp = MR32(R+0x48u) + MR32(P);
        uint32_t P2 = (MR16(vp+2u) + MR16(vp))*0x64u + base - 0x64u, end = MR16(R+0x56u)*0x64u + P2;
        for (; P2 < end; P2 += 0x64u){
            uint32_t R2 = MR32(P2+4u);
            for (uint32_t k = 0; k < MR16(P2+0x36u); k++, R2 += 0x64u){
                uint32_t t = MR8(R2+0x5bu);
                if (t != 3u && t != 9u) return 0;
            }
        }
    }
    n6_calls++;
    uint32_t dev = MR32(th+8u), r;
    for (uint32_t R = Rb; R < Re; R += 0x64u){
        uint32_t vp = MR32(R+0x48u) + MR32(P), stage = MR16(R+0x54u);
        uint32_t P2 = (MR16(vp+2u) + MR16(vp))*0x64u + base - 0x64u, end = MR16(R+0x56u)*0x64u + P2;
        for (; P2 < end; P2 += 0x64u, stage++){
            uint32_t R2 = MR32(P2+4u);
            for (uint32_t k = 0; k < MR16(P2+0x36u); k++, R2 += 0x64u){
                if (MR8(R2+0x5bu) == 3u){
                    if (MR32(R2)){
                        uint32_t rec = (uint32_t)((int32_t)(P2 - MR32(th+0x2cu)) / 0x64) & 0xffffu;
                        r = n4_eval(c, th, rec, k, R2, P2, GA(0x668d51u));
                        if ((int32_t)r < 0){ *res = r; return 1; }
                        MR32(R2) = MR32(R2+4u);
                    }
                    r = n4_dev(c, dev, 3, 0x114u, GA(0x668d7du), stage, MR32(R2+0x58u) & 0xffffffu,
                               MR32(MR32(R2+0x48u) + MR32(P2)));
                } else {
                    uint32_t o = MR32(MR32(th+0xb0u) + MR32(R2+0x1cu)*4u), o40 = MR32(o+0x40u);
                    uint32_t ix = MR32(MR32(R2+8u));
                    if (o40) ix = MR32(MR32(o40+8u) + ix*4u);
                    r = n4_dev(c, dev, 4, 0x104u, GA(0x668dc4u), stage, MR32(MR32(th+0x18u) + ix*24u + 4u), 0);
                }
                if ((int32_t)r < 0){ *res = r; return 1; }
            }
        }
    }
    *res = 0;
    return 1;
}
static int n6_apply(CPU *c){
    uint32_t sp = c->esp, res;
    if (!n6_samp(c, c->ecx, MR32(sp+4u), MR32(sp+8u), MR32(sp+12u), &res)) return 0;
    c->eax = res; POP32(c->eip); c->esp += 12u;
    return 1;
}
void sub_00668cd0_orig(CPU *c);
void sub_00668cd0(CPU *c){
    if (g_n6_on < 0){ const char *e = getenv("D3DXNATIVE6"); g_n6_on = e ? atoi(e) != 0 : 1; }
    if (g_n6_on && !g_pctx_n){
        if (g_n4_v){
            static uint64_t vk;
            if ((++vk % (uint64_t)g_n4_v) == 0){
                uint32_t ext[VEXT], ne = 0, sp = c->esp, th = c->ecx, P = MR32(sp+4u), base = MR32(th+0x2cu);
                ext[ne++] = P;
                for (uint32_t R = MR32(sp+8u); R < MR32(sp+12u) && ne < VEXT; R += 0x64u){
                    ext[ne++] = R;
                    uint32_t vp = MR32(R+0x48u) + MR32(P);
                    uint32_t P2 = (MR16(vp+2u) + MR16(vp))*0x64u + base - 0x64u, end = MR16(R+0x56u)*0x64u + P2;
                    for (; P2 < end && ne + 1 < VEXT; P2 += 0x64u){
                        ext[ne++] = P2;
                        for (uint32_t k = 0, R2 = MR32(P2+4u); k < MR16(P2+0x36u) && ne < VEXT; k++, R2 += 0x64u) ext[ne++] = R2;
                    }
                }
                nat_verify(c, n6_apply, sub_00668cd0_orig, th, ext, (int)ne, &n6vs, "D3DXN6VERIFY", P);
                return;
            }
        }
        if (n6_apply(c)) return;
    }
    sub_00668cd0_orig(c);
}

/* ---- Stage 4d: native shader applier, sub_00668e0d(this, P) ret 4 ----
 * P = shader record; R0 = [P+4] the shader object state, vs = [P+8] bit 1.
 *   If R0 is dirty: 669296(this, record, 0, 0xffff) -> original.
 *   Set{Vertex,Pixel}Shader(from the object table; o+0x40 indirect index)
 *   P&4: 663bc1(this, record, 1); if P&0x10, upload word[P+0x50/56/5c] F/I/B
 *        constants (if 4c==4e / 52==54 / 58==5a); skip one state.
 *   float constant states [.., w[P+0x4e]): if dirty R=R+4, and if R+0x30&0x10
 *        a raw copy ([R+0x5c], [R+0x60] bytes) otherwise 6623ea; then upload the
 *        block (no upload if the loop is empty).  If w[P+0x4e]==w[P+0x36] return 0.
 *   int [.., w[P+0x54]) -> 6627a5 + ConstantI; bool [.., w[P+0x5a]) ->
 *        662bab + ConstantB; the remaining [.., w[P+0x60]) samplers -> 668cd0.
 * The [ebp-8] local slot is passed by address to the filler functions; it is
 * imitated with the same update rule.  In dry verification, if a guest subcall
 * is needed the native path is rejected (the original runs). */
void sub_00663bc1(CPU*); void sub_006623ea(CPU*); void sub_006627a5(CPU*); void sub_00662bab(CPU*);
void sub_00668e0d_orig(CPU *c);
static uint64_t n7_calls;
static VStat n7vs;
static int g_n7_on = -1;
/* filler: fn(this, R+8, dest, w[R+0x56], [R+0x3c], [[R+0x34]+0x10], &slot, [o+0x4c]) */
static inline void n7_fill(CPU *c, void (*fn)(CPU*), uint32_t ret, uint32_t th, uint32_t P, uint32_t R, uint32_t *slot8){
    uint32_t o = MR32(MR32(th+0xb0u) + MR32(R+0x1cu)*4u);
    PUSH32(*slot8); uint32_t sa = c->esp;
    PUSH32(MR32(o+0x4cu)); PUSH32(sa); PUSH32(MR32(MR32(R+0x34u)+0x10u)); PUSH32(MR32(R+0x3cu));
    PUSH32(MR16(R+0x56u)); PUSH32(MR32(R+0x48u) + MR32(P)); PUSH32(R+8u); PUSH32(ret);
    c->ecx = th; fn(c);
    *slot8 = MR32(sa); c->esp = sa + 4u;
}
static int n7_shader(CPU *c, uint32_t th, uint32_t P, uint32_t *res){
    uint32_t R0 = MR32(P+4u), vs = (MR32(P+8u) & 2u) != 0, base = MR32(th+0x2cu);
    if (MR32(R0)) return 0;
    uint32_t endf = MR16(P+0x4eu)*0x64u + MR32(P+4u), ends = MR16(P+0x60u)*0x64u + MR32(P+4u);
    if (n4_dry){
        if (MR8(P+8u) & 4u) return 0;
        for (uint32_t R = R0 + 0x64u; R < endf; R += 0x64u)
            if (MR32(R) && !(MR8(R+0x30u) & 0x10u)) return 0;
        if (MR16(P+0x4eu) != MR16(P+0x36u) && endf < MR16(P+0x5au)*0x64u + MR32(P+4u)) return 0;
    }
    n7_calls++;
    uint32_t dev = MR32(th+8u), r, slot8;
    uint32_t o = MR32(MR32(th+0xb0u) + MR32(R0+0x1cu)*4u), o40 = MR32(o+0x40u), ix = MR32(MR32(R0+8u));
    if (o40) ix = MR32(MR32(o40+8u) + ix*4u);
    uint32_t sh = MR32(MR32(th+0x18u) + ix*24u + 4u);
    r = vs ? n4_dev(c, dev, 7, 0x170u, GA(0x668e79u), sh, 0, 0) : n4_dev(c, dev, 8, 0x1acu, GA(0x668ea9u), sh, 0, 0);
    if ((int32_t)r < 0){ *res = r; return 1; }
    uint32_t R = R0 + 0x64u; slot8 = R;
    if (MR8(P+8u) & 4u){
        uint32_t rec = (uint32_t)((int32_t)(P - base) / 0x64) & 0xffffu;
        c->ecx = th; PUSH32(1u); PUSH32(rec); PUSH32(GA(0x668ed9u)); sub_00663bc1(c);
        r = c->eax; if ((int32_t)r < 0){ *res = r; return 1; }
        if (MR8(P+8u) & 0x10u){
            uint32_t n;
            if ((n = MR16(P+0x50u)) && MR16(P+0x4cu) == MR16(P+0x4eu))
                n4_dev(c, dev, vs ? 5 : 6, vs ? 0x178u : 0x1b4u, vs ? GA(0x668f1eu) : GA(0x668f26u), 0, MR32(P+0x40u)+MR32(P), n);
            if ((n = MR16(P+0x56u)) && MR16(P+0x52u) == MR16(P+0x54u))
                n4_dev(c, dev, vs ? 9 : 10, vs ? 0x180u : 0x1bcu, vs ? GA(0x668f59u) : GA(0x668f61u), 0, MR32(P+0x44u)+MR32(P), n);
            if ((n = MR16(P+0x5cu)) && MR16(P+0x58u) == MR16(P+0x5au))
                n4_dev(c, dev, vs ? 11 : 12, vs ? 0x188u : 0x1c4u, vs ? GA(0x668f94u) : GA(0x668f9cu), 0, MR32(P+0x48u)+MR32(P), n);
        }
        R += 0x64u; slot8 = R;
    }
    if (R < endf){
        for (; R < endf; R += 0x64u, slot8 = R){
            if (!MR32(R)) continue;
            MR32(R) = MR32(R+4u);
            if (MR8(R+0x30u) & 0x10u){
                uint32_t dst = MR32(R+0x48u) + MR32(P), src = MR32(R+0x5cu), nb = MR32(R+0x60u);
                if (dst + nb <= src || src + nb <= dst) memcpy(g_mem + dst, g_mem + src, nb);
                else { for (uint32_t k = 0; k + 4u <= nb; k += 4u) MR32(dst+k) = MR32(src+k);   /* rep movsd */
                       for (uint32_t k = nb & ~3u; k < nb; k++) MR8(dst+k) = MR8(src+k); }
            } else n7_fill(c, sub_006623ea, GA(0x669017u), th, P, R, &slot8);
        }
        r = n4_dev(c, dev, vs ? 5 : 6, vs ? 0x178u : 0x1b4u, vs ? GA(0x669045u) : GA(0x66904du),
                   0, MR32(P+0x40u)+MR32(P), MR16(P+0x50u));
        if ((int32_t)r < 0){ *res = r; return 1; }
    }
    if (MR16(P+0x4eu) == MR16(P+0x36u)){ *res = 0; return 1; }
    uint32_t endi = MR16(P+0x54u)*0x64u + MR32(P+4u);
    if (R < endi){
        for (; R < endi; R += 0x64u)
            if (MR32(R)){ MR32(R) = MR32(R+4u); n7_fill(c, sub_006627a5, GA(0x6690b3u), th, P, R, &slot8); }
        r = n4_dev(c, dev, vs ? 9 : 10, vs ? 0x180u : 0x1bcu, vs ? GA(0x6690deu) : GA(0x6690e6u),
                   0, MR32(P+0x44u)+MR32(P), MR16(P+0x56u));
        if ((int32_t)r < 0){ *res = r; return 1; }
    }
    uint32_t endb = MR16(P+0x5au)*0x64u + MR32(P+4u);
    if (R < endb){
        for (; R < endb; R += 0x64u)
            if (MR32(R)){ MR32(R) = MR32(R+4u); n7_fill(c, sub_00662bab, GA(0x66913bu), th, P, R, &slot8); }
        r = n4_dev(c, dev, vs ? 11 : 12, vs ? 0x188u : 0x1c4u, vs ? GA(0x669166u) : GA(0x66916eu),
                   0, MR32(P+0x48u)+MR32(P), MR16(P+0x5cu));
        if ((int32_t)r < 0){ *res = r; return 1; }
    }
    if (R >= ends){ *res = 0; return 1; }
    if (!n6_samp(c, th, P, R, ends, &r)){
        c->ecx = th; PUSH32(ends); PUSH32(R); PUSH32(P); PUSH32(GA(0x66918eu)); sub_00668cd0_orig(c);
        r = c->eax;
    }
    *res = r;
    return 1;
}
static int n7_apply(CPU *c){
    uint32_t res;
    if (!n7_shader(c, c->ecx, MR32(c->esp+4u), &res)) return 0;
    c->eax = res; POP32(c->eip); c->esp += 4u;
    return 1;
}
void sub_00668e0d(CPU *c){
    if (g_n7_on < 0){ const char *e = getenv("D3DXNATIVE7"); g_n7_on = e ? atoi(e) != 0 : 1; }
    uint64_t t0 = dx_on() ? mach_absolute_time() : 0;
    if (g_n7_on && !g_pctx_n){
        static uint64_t vk;
        if (g_n4_v && (++vk % (uint64_t)g_n4_v) == 0){
            uint32_t th = c->ecx, P = MR32(c->esp+4u), ext[VEXT], ne = 0;
            ext[ne++] = P;
            uint32_t ends = MR16(P+0x60u)*0x64u + MR32(P+4u);
            for (uint32_t R = MR32(P+4u); R < ends && ne < VEXT - 8; R += 0x64u) ext[ne++] = R;
            uint32_t blk = MR32(P+0x40u) + MR32(P), nb = MR16(P+0x50u)*16u;
            for (uint32_t k = 0; k < nb && ne < VEXT; k += 0x64u) ext[ne++] = blk + k;
            nat_verify(c, n7_apply, sub_00668e0d_orig, th, ext, (int)ne, &n7vs, "D3DXN7VERIFY", P);
            goto out;
        }
        if (n7_apply(c)) goto out;
    }
    sub_00668e0d_orig(c);
out:
    if (t0){ g_dx[DX_668E0D].ticks += mach_absolute_time() - t0; g_dx[DX_668E0D].n++; }
}

/* ---- Stage 4e: native SetTexture path ----
 * sub_0065ccd8(this, tex, i, o40) ret 12: object table slot
 *   y = [this+0x18] + (o40 ? [[o40+8] + i*4] : i)*24;  0 if [y+4] == tex.
 *   tex->AddRef, old->Release, [y+4] = tex; if o40 exists, the pool ring
 *   ([o40+0x10] .. o40, node: +0 effect, +8 index table) does the same for every
 *   effect's slot: AddRef(new), Release(old), write.  Returns 0.
 * A com.c texture's AddRef/Release only change its reference count, so the call
 *   is skipped and the count is updated directly (com_tex_ref_skipped).
 * sub_0066d7e9 SetTexture(this, h, tex) ret 12: if h < 0, record = ~h
 *   (string handle -> original); if an array parameter without an element, or the
 *   type isn't 5..9, 0x8876086c; [this+0x3c]==3 -> original (the 666a78 path);
 *   0 if the slot is the same; otherwise 65ccd8 then 66056c(this, record).
 *   D3DXNATIVE8=0 turns it off. */
void sub_0065ccd8_orig(CPU *c);
extern int g_fxtr; int fxtr_now(void);
static int g_n8_on = -1;
static uint64_t n8_tex, n8_set, n8_refskip;
static inline int n8_init(void){
    if (g_n8_on < 0){ const char *e = getenv("D3DXNATIVE8"); g_n8_on = e ? atoi(e) != 0 : 1; }
    return g_n8_on;
}
void com_tex_ref_skipped(uint32_t obj, int addref);   /* com.c: keep the reference count */
static inline void n8_ref(CPU *c, uint32_t obj, uint32_t off, uint32_t ret){
    uint32_t fn = MR32(MR32(obj) + off);
    if ((fn & 0xF0000000u) == 0xE0000000u && ((fn >> 16) & 0x1fffu) == 3u && !(g_fxtr && fxtr_now())){
        n8_refskip++; com_tex_ref_skipped(obj, off == 4u); return; }
    PUSH32(obj); PUSH32(ret); dispatch_indirect(c, fn);
}
static void n8_store(CPU *c, uint32_t th, uint32_t tex, uint32_t ix, uint32_t o40){
    n8_tex++;
    uint32_t y = MR32(th+0x18u) + (o40 ? MR32(MR32(o40+8u) + ix*4u) : ix)*24u;
    if (MR32(y+4u) == tex) return;
    if (tex) n8_ref(c, tex, 4u, GA(0x65cd10u));
    uint32_t old = MR32(y+4u);
    if (old){ n8_ref(c, old, 8u, GA(0x65cd1du)); MR32(y+4u) = 0; }
    MR32(y+4u) = tex;
    if (!o40) return;
    for (uint32_t nd = MR32(o40+0x10u); nd != o40; nd = MR32(nd+0x10u)){
        uint32_t ef = MR32(nd);
        if (!ef) continue;
        uint32_t y2 = MR32(ef+0x18u) + MR32(MR32(nd+8u) + ix*4u)*24u, nw = MR32(y+4u);
        if (nw) n8_ref(c, nw, 4u, GA(0x65cd52u));
        uint32_t o2 = MR32(y2+4u);
        if (o2){ n8_ref(c, o2, 8u, GA(0x65cd5fu)); MR32(y2+4u) = 0; }
        MR32(y2+4u) = MR32(y+4u);
    }
}
void sub_0065ccd8(CPU *c){
    if (!n8_init()){ sub_0065ccd8_orig(c); return; }
    uint32_t sp = c->esp, ret = MR32(sp), th = c->ecx;
    PUSH32(c->ebp); PUSH32(c->ebx); PUSH32(c->esi); PUSH32(c->edi);  /* original prologue: so the subcalls see the same esp */
    n8_store(c, th, MR32(sp+4u), MR32(sp+8u), MR32(sp+12u));
    c->esp = sp + 16u; c->eip = ret; c->eax = 0;
}
void sub_0066056c(CPU *c);
void sub_0066d7e9_orig(CPU *c);
void sub_0066d7e9(CPU *c){
    uint64_t t0 = dx_on() ? mach_absolute_time() : 0;
    uint32_t sp = c->esp, th = MR32(sp+4u), h = MR32(sp+8u), tex = MR32(sp+12u);
    if (!n8_init() || (int32_t)h >= 0 || MR32(th+0x3cu) == 3u){ sub_0066d7e9_orig(c); goto out; }
    {
    n8_set++;
    uint32_t ret = MR32(sp), rec = ~h, d = MR32(rec+4u), r;
    if ((MR32(d+0x10u) && MR32(rec+8u) == 0xffffffffu) || (int32_t)MR32(d) < 5 || (int32_t)MR32(d) > 9){
        r = 0x8876086cu; goto done;
    }
    uint32_t o40 = MR32(MR32(MR32(th+0xb0u) + MR32(rec+0x14u)*4u) + 0x40u), ix = MR32(MR32(rec));
    uint32_t e = o40 ? MR32(MR32(o40+8u) + ix*4u) : ix;
    if (tex == MR32(MR32(th+0x18u) + e*24u + 4u)){ r = 0; goto done; }
    PUSH32(c->ebp); PUSH32(c->ebx); PUSH32(c->esi); PUSH32(c->edi);
    PUSH32(c->ebp); PUSH32(c->ebx); PUSH32(c->esi); PUSH32(c->edi);   /* 65ccd8's prologue */
    n8_store(c, th, tex, ix, o40);
    c->esp += 16u;
    c->ecx = th; PUSH32(rec); PUSH32(GA(0x66d8afu)); sub_0066056c(c);
    r = c->eax;
    c->esp += 16u;
done:
    c->eax = r; c->esp = sp + 16u; c->eip = ret;
    }
out:
    if (t0){ g_dx[DX_SETTEXTURE].ticks += mach_absolute_time() - t0; g_dx[DX_SETTEXTURE].n++; }
}

/* Core: if it returns 1 the state was applied natively, *res = 669296's return
 * value.  Doesn't touch the stack (except push/pop for 664a2f and device calls). */
void sub_006691b1(CPU *c); void sub_00668e0d(CPU*);
static uint64_t n4_obj;
static inline int n4_core(CPU *c, uint32_t th, uint32_t a, uint32_t b, uint32_t eld, uint32_t *res){
    uint32_t P = a*0x64u + MR32(th+0x2cu), R = b*0x64u + MR32(P+4u);
    uint32_t code = MR32(R+0x58u), type = code >> 24, el = eld & 0xffffu;
    if (el == 0xffffu) el = MR16(R+0x54u);
    int dirty = MR32(R) != 0;
    if (type == 14u || type == 15u){
        /* object assignment (14 shader, 15 sampler): dirty + array selector -> original */
        if (dirty && MR32(R+0x38u)) return 0;
        n4_obj++;
        if (dirty) MR32(R) = MR32(R+4u);
        uint32_t vp = MR32(R+0x48u) + MR32(P), cnt = MR16(vp), r = 0;
        if (!cnt){ *res = 0; return 1; }
        uint32_t rec = MR16(vp+2u) + cnt - 1u, mode = MR32(th+0x3cu);
        if (type == 14u){
            if (mode == 4u){
                uint32_t P2 = rec*0x64u + MR32(th+0x2cu);
                c->ecx = th;
                if (MR8(P2+8u) & 1u){ PUSH32(P2); PUSH32(GA(0x669f73u)); sub_00668e0d(c); }
                else { PUSH32(0xffffu); PUSH32(rec); PUSH32(GA(0x669f85u)); sub_006691b1(c); }
                *res = 0; return 1;
            }
            c->ecx = th; PUSH32(0xffffu); PUSH32(rec); PUSH32(GA(0x669fa6u)); sub_006691b1(c);
            *res = c->eax; return 1;
        }
        if (mode == 4u){
            if (!n6_samp(c, th, P, R, R + 0x64u, &r)){
                c->ecx = th; PUSH32(R + 0x64u); PUSH32(R); PUSH32(P); PUSH32(GA(0x669fe5u)); sub_00668cd0_orig(c);
                r = c->eax;
            }
            *res = r; return 1;
        }
        c->ecx = th; PUSH32((eld & 0xffff0000u) | el); PUSH32(rec); PUSH32(GA(0x6693c1u)); sub_006691b1(c);
        *res = c->eax; return 1;
    }
    if (type - 1u > 2u) return 0;
    if (dirty && type == 3u && MR32(th+0x3cu) == 4u && el == 0xffffu) return 0;
    n4_calls++;
    uint32_t vp = MR32(R+0x48u) + MR32(P), r = 0;
    if (dirty){
        n4_dirty++;
        MR32(R) = MR32(R+4u);
        r = n4_eval(c, th, a, b, R, P, type == 3u ? GA(0x669533u) : GA(0x669863u));
        if ((int32_t)r < 0){ *res = r; return 1; }
    }
    uint32_t dev = MR32(th+8u), v = MR32(vp), st = code & 0xffffffu;
    if (type == 1u)      r = n4_dev(c, dev, 1, 0xe4u,  GA(0x66989eu), st, v, 0);
    else if (type == 2u) r = n4_dev(c, dev, 2, 0x10cu, GA(0x6698bfu), el, st, v);
    else if (MR32(th+0x3cu) != 1u) r = n4_dev(c, dev, 3, 0x114u, GA(0x6698eau), el, st, v);
    *res = r;
    return 1;
}
static int n4_apply(CPU *c){
    uint32_t th = c->ecx, sp = c->esp, res;
    uint32_t a = MR16(sp+4u), b = MR16(sp+8u), el = MR16(sp+12u);
    if (!n4_core(c, th, a, b, MR32(sp+12u), &res)) return 0;
    if (el == 0xffffu){                                  /* writes to the original argument slot */
        uint32_t P = a*0x64u + MR32(th+0x2cu), R = b*0x64u + MR32(P+4u);
        MR16(sp+12u) = MR16(R+0x54u);
    }
    c->eax = res; POP32(c->eip); c->esp += 12u;
    return 1;
}
/* General verification: the native code does a dry run (n4_dev RECORDS the device
 * calls, doesn't make them), memory and the call list are saved; memory is
 * rolled back, the original runs with dxrec recording; the memory window
 * (this +-256 KB, .data, extra records), the call sequence and eax/esp are
 * compared.  Only paths that reach the device solely through n4_dev can be
 * verified (guest subcalls can't be dry-run). */
static void nat_verify(CPU *c, int (*nat)(CPU*), void (*orig)(CPU*), uint32_t th,
                       const uint32_t *ext, int next, VStat *vs, const char *tag, uint32_t info){
    uint32_t lo = th & ~0x3fffu, hi = lo + 0x4000u, dn = V_DATA1 - V_DATA0;
    while (lo - 0x4000u >= th - V_WIN && mem_is_committed(lo - 0x4000u)) lo -= 0x4000u;
    while (hi + 0x4000u <= th + V_WIN && mem_is_committed(hi)) hi += 0x4000u;
    uint32_t n = hi - lo;
    static uint8_t *pre0, *pre1, *nat0, *nat1; static uint8_t preX[VEXT][0x64], natX[VEXT][0x64];
    if (!pre0){ pre0 = malloc(2u*V_WIN + 0x8000u); nat0 = malloc(2u*V_WIN + 0x8000u);
                pre1 = malloc(dn); nat1 = malloc(dn); }
    if (next > VEXT) next = VEXT;
    memcpy(pre0, g_mem + lo, n); memcpy(pre1, g_mem + V_DATA0, dn);
    for (int i = 0; i < next; i++) memcpy(preX[i], g_mem + ext[i], 0x64);
    CPU c0 = *c;
    n4_dry = 1; n4_dryn = 0;
    int ok = nat(c);
    n4_dry = 0;
    uint32_t neax = c->eax, nesp = c->esp;
    if (!ok){ *c = c0; orig(c); return; }
    memcpy(nat0, g_mem + lo, n); memcpy(nat1, g_mem + V_DATA0, dn);
    for (int i = 0; i < next; i++) memcpy(natX[i], g_mem + ext[i], 0x64);
    memcpy(g_mem + lo, pre0, n); memcpy(g_mem + V_DATA0, pre1, dn);
    for (int i = next - 1; i >= 0; i--) memcpy(g_mem + ext[i], preX[i], 0x64);
    *c = c0;
    DxRec vr; DxCall vc[NDRY]; uint8_t vd[256];
    DXREC_INIT(&vr, vc, vd);
    dxrec_begin(&vr, DXREC_STATE);
    orig(c);
    dxrec_end();
    if (!vr.ok){ vs->norec++; return; }     /* state block recording etc.: can't be compared */
    vs->runs++;
    /* this+0x114..0x11b (saved FPU CW + flag): 663a42 saves it inside BeginPass and
     * restores it at the end -- excluded, as in the Stage 1 verification. */
    { uint32_t off = th + 0x114u - lo; memcpy(nat0 + off, g_mem + th + 0x114u, 8); }
    int dm = memcmp(nat0, g_mem + lo, n) || memcmp(nat1, g_mem + V_DATA0, dn);
    for (int i = 0; !dm && i < next; i++) dm = memcmp(natX[i], g_mem + ext[i], 0x64) != 0;
    int dc = vr.ncall != n4_dryn;
    static const char *kn[13] = { "", "SetRenderState", "SetTextureStageState", "SetSamplerState", "SetTexture",
        "SetVertexShaderConstantF", "SetPixelShaderConstantF", "SetVertexShader", "SetPixelShader",
        "SetVertexShaderConstantI", "SetPixelShaderConstantI", "SetVertexShaderConstantB", "SetPixelShaderConstantB" };
    for (int i = 0; !dc && i < vr.ncall && i < NDRY; i++){
        const DxCall *k = &vr.call[i];
        int nk = (int)n4_dryk[i][0];
        int na = (nk >= 1 && nk <= 12) ? n4_nargs[nk] : 0;
        dc = na == 0 || strcmp(dxrec_mid_name(k->mid), kn[nk]) || k->a[0] != n4_dryk[i][1]
          || (na >= 2 && k->a[1] != n4_dryk[i][2]) || (na >= 3 && k->a[2] != n4_dryk[i][3]);
    }
    int de = c->eax != neax || c->esp != nesp;
    if (dm || dc || de){
        if (vs->bad++ < 12)
            fprintf(stderr, "[%s] diff (%08x): memory=%d calls=%d (native %d, original %d) eax=%d (%08x/%08x)\n",
                    tag, info, dm, dc, n4_dryn, vr.ncall, de, neax, c->eax);
        if (dm && vs->bad <= 4){ uint32_t z = ext && next ? ext[0] : 0; verify_diff(lo, n, nat0, th, z, z); }
    }
}
static VStat n4vs;
static void n4_verify(CPU *c){
    uint32_t th = c->ecx, sp = c->esp;
    uint32_t a = MR16(sp+4u), b = MR16(sp+8u);
    uint32_t P = a*0x64u + MR32(th+0x2cu), R = b*0x64u + MR32(P+4u);
    uint32_t ext[2] = { R, P };
    nat_verify(c, n4_apply, sub_00669296_orig, th, ext, 2, &n4vs, "D3DXN4VERIFY", MR32(R+0x58u));
}

void sub_00669296_orig(CPU *c);
void sub_00669296(CPU *c){
    if (g_n4_on < 0){
        const char *e = getenv("D3DXNATIVE4"); g_n4_on = e ? atoi(e) != 0 : 1;
        const char *v = getenv("D3DXN4VERIFY"); g_n4_v = v ? atoi(v) : 0;
    }
    if (g_n4_on && !g_pctx_n){
        uint64_t t0 = dx_on() ? mach_absolute_time() : 0;
        static uint64_t vk;
        if (g_n4_v && (++vk % (uint64_t)g_n4_v) == 0){
            uint32_t ty = MR32(MR16(c->esp+8u)*0x64u + MR32(MR16(c->esp+4u)*0x64u + MR32(c->ecx+0x2cu) + 4u) + 0x58u) >> 24;
            if (ty - 1u <= 2u){ n4_verify(c); goto n4out; }
        }
        if (n4_apply(c)){
n4out:
            if (t0){ g_dx[DX_STATEAPPLY].ticks += mach_absolute_time() - t0; g_dx[DX_STATEAPPLY].n++; }
            return;
        }
    }
    if (g_m2_on < 0){
        const char *m = getenv("D3DXMEMO2"); g_m2_on = m ? atoi(m) != 0 : 1;
        const char *v = getenv("D3DXVERIFY2"); g_v2_n = v ? atoi(v) : 0;
        if (g_m2_on || g_v2_n) g_m2 = calloc(M2_CAP, sizeof *g_m2);
    }
    uint64_t t0 = dx_on() ? mach_absolute_time() : 0;
    if (!g_m2){
        if (g_pctx_n){ uint32_t th = c->ecx, a = MR16(c->esp+4u), b = MR16(c->esp+8u);
            uint32_t P = a*0x64u + MR32(th+0x2cu), R = b*0x64u + MR32(P+4u);
            pctx_note_state(th, P, R, (MR32(R+0x58u) >> 24) & 31u); }
        sub_00669296_orig(c); goto out; }
    {
    uint32_t th = c->ecx, a = MR16(c->esp+4u), b = MR16(c->esp+8u), el = MR16(c->esp+12u);
    uint32_t P = a*0x64u + MR32(th+0x2cu), R = b*0x64u + MR32(P+4u);
    uint32_t type = (MR32(R+0x58u) >> 24) & 31u;
    pctx_note_state(th, P, R, type);
    m2_calls++; m2_type_calls[type]++;
    /* Type 9 (indirect read from the texture table) and 14/15 (selecting an object
     * from an array with an expression and applying it nested) read inputs outside
     * the snapshot: D3DXVERIFY2 found device/memory differences for these types.
     * Only the verified 1..8 are memoized; the rest always run the original code. */
    if (type == 0 || type > 8){ m2_excl++; sub_00669296_orig(c); goto out; }
    uint32_t ab = (a<<16) | b;
    uint32_t hk = (th * 0x9E3779B1u) ^ (ab * 0x85EBCA77u) ^ (el * 0xC2B2AE3Du);
    M2Slot *s = NULL;
    for (uint32_t j = 0; j < 64u; j++){
        M2Slot *t = &g_m2[(hk + j) & (M2_CAP-1)];
        if (!t->n || (t->th == th && t->ab == ab && t->elem == el)){ s = t; break; }
    }
    if (!s){ sub_00669296_orig(c); goto out; }
    if (!s->rec.call) DXREC_INIT(&s->rec, s->rcall, s->rdata);
    int mine = s->n && s->th == th && s->ab == ab && s->elem == el && s->valid;
    SnapW w = { s->buf, 0, M2_BUF, mine };
    /* claim the slot right away: so a nested call doesn't take the same empty slot */
    s->th = th; s->ab = ab; s->elem = el; if (!s->n) s->n = 1;
    if (!state_apply_snap(&w, th, a, b, el, P, R)){
        m2_big++; s->valid = 0; sub_00669296_orig(c); goto out; }
    int same = mine && w.eq && w.len == s->len;
    int hit = 0;
    if (same){
        m2_same++;
        if (!s->rec.ok) m2_norec++;
        else if (!dxrec_redundant(&s->rec)) m2_devchg++;
        else hit = 1;
    }
    if (hit){
        m2_hit++; m2_type_hit[type]++;
        if (g_v2_n && (m2_hit % (uint64_t)g_v2_n) == 0){
            uint32_t lo = th & ~0x3fffu, hi = lo + 0x4000u, dn = V_DATA1 - V_DATA0;
            while (lo - 0x4000u >= th - V_WIN && mem_is_committed(lo - 0x4000u)) lo -= 0x4000u;
            while (hi + 0x4000u <= th + V_WIN && mem_is_committed(hi)) hi += 0x4000u;
            uint32_t n = hi - lo;
            if (!v_buf0){ v_buf0 = malloc(2u*V_WIN + 0x8000u); v_buf1 = malloc(dn); }
            memcpy(v_buf0, g_mem + lo, n); memcpy(v_buf1, g_mem + V_DATA0, dn);
            DxRec vr; DxCall vc[64]; uint8_t vd[1024];
            DXREC_INIT(&vr, vc, vd);
            dxrec_begin(&vr, DXREC_STATE);
            sub_00669296_orig(c);
            dxrec_end();
            v2_runs++;
            { uint32_t off = th + 0x114u - lo; memcpy(v_buf0 + off, g_mem + th + 0x114u, 8); }
            int dm = memcmp(v_buf0, g_mem + lo, n) || memcmp(v_buf1, g_mem + V_DATA0, dn);
            int dd = !vr.allred, de = c->eax != s->eax;
            if (dm || dd || de){
                v2_type_bad[type]++;
                if (dm) v2_mem++;
                if (dd) v2_dev++;
                if (de) v2_eax++;
                static int shown = 0;
                if (shown++ < 12)
                    fprintf(stderr, "[D3DXVERIFY2] type %u (code %08x) R=%08x: memory=%d device=%d eax=%d  calls %u (record ok=%d)\n",
                            type, MR32(R+0x58u), R, dm, dd, de, vr.ncall, vr.ok);
                if (dm && shown <= 4){
                    if (memcmp(v_buf0, g_mem + lo, n)) verify_diff(lo, n, v_buf0, th, P, R);
                }
            } else v2_clean++;
            s->eax = c->eax;
            goto out;
        }
        if (g_m2_on){
            m2_skip++;
            dxrec_append(&s->rec);        /* so the open pass record also knows the skipped calls */
            POP32(c->eip); c->esp += 12u; c->eax = s->eax;
            goto out;
        }
    }
    s->valid = 0;                         /* the buffer is already the new snapshot */
    dxrec_begin(&s->rec, DXREC_STATE);
    sub_00669296_orig(c);
    dxrec_end();
    s->eax = c->eax; s->n++; s->type = type; s->len = w.len; s->valid = 1;
    }
out:
    if (t0){ g_dx[DX_STATEAPPLY].ticks += mach_absolute_time() - t0; g_dx[DX_STATEAPPLY].n++; }
}

/* ---- Stage 3: pass memo, sub_006691b1(this=ecx, pass, element) ret 8 ----
 * Hit: the fingerprint is the same (effect fields + pass record), none of the
 * dependent parameter lines changed since record time, and all the recorded
 * device calls currently have no effect.  Eligibility: every state in the pass
 * is type 1..8, no nested application, 6691b1's special paths (P&8,
 * [this+0x3c]==4 && P&1) off.
 *   D3DXMEMO3=1     : on (OFF by default -- see D3DX_HLE.md Stage 3 result)
 *   D3DXVERIFY3=<n> : on 1/n of the hits run the original, memory + device */
#define M3_BUF 192u
typedef struct {
    uint32_t th, ab, eax, n, len, valid, ep0;
    uint8_t  buf[M3_BUF];
    PassCtx  ctx;
    DxRec    rec;
    DxCall   rcall[64];
    uint8_t  rdata[1024];
} M3Slot;
#define M3_CAP 2048u
static M3Slot *g_m3;
static int g_m3_on = -1, g_v3_n = 0;
static uint64_t m3_calls, m3_fp, m3_dep, m3_dev, m3_inel, m3_hit, m3_skip, m3_rec, m3_recok;
static uint64_t v3_runs, v3_clean, v3_mem, v3_dev, v3_eax, m3_why[8]; static uint32_t m3_why_tm;

/* Fingerprint: the effect fields, the pass record and the +0x8c..+0xab range of
 * the technique record.  With flag 8 on, 6691b1 calls 66404c/6641a7 on every
 * pass: it compares the pass's shader constant register count ([P+0x1c..]) with
 * the technique's largest so far ([technique+0xa4..0xaa]) and only prepares a
 * record when it GROWS.  No calls after warm-up; if it grows these fields
 * change and the pass is re-recorded. */
static inline int pass_fp(SnapW *w, uint32_t th, uint32_t P){
    uint32_t fix[5] = { th, MR32(th+0x2cu), MR32(th+0x3cu), MR32(th+0x40u), MR32(th+0x118u) & ~1u };
    uint32_t tech = MR16(th+0x42u)*0xb4u + MR32(th+0xa4u);
    return sw_put(w, fix, sizeof fix) && sw_put(w, g_mem + P, 0x64u)
        && sw_put(w, g_mem + tech + 0x8cu, 0x20u);
}

void sub_006691b1_orig(CPU *c);
/* ---- Stage 4b: native pass loop, sub_006691b1(this, a, el) ret 8 ----
 *   P = a*0x64 + [this+0x2c]
 *   [this+0x118]&8 : 66404c(P+0x1c, word[P+0x18]!=0xffff), 6641a7(P+0x24, word[P+0x1a]!=0xffff); <0 return
 *   P&8            : word[P+0x18]==0xffff ? 667d61(a) : 666e07(a); <0 return
 *   [this+0x3c]==4 : if P&1 return the result of 668e0d(P); otherwise for every
 *                    state 669296(a, b, el), return if <0
 *   other          : 669296 for every state; return if <0 and [this+0x3c]!=1
 *   0 at the end.  Type 1/2/3 states go directly through n4_core, the others
 *   through a guest call.  The original's prologue (ebp/ebx/esi/edi) is set up
 *   exactly.  D3DXNATIVE5=0 turns it off.  With D3DXN4VERIFY on, states go
 *   through the 669296 wrapper (verified there). */
void sub_0066404c(CPU*); void sub_006641a7(CPU*); void sub_00667d61(CPU*);
void sub_00666e07(CPU*); void sub_00668e0d(CPU*);
static int g_n5_on = -1;
static uint64_t n5_calls;
static inline uint32_t n5_state(CPU *c, uint32_t th, uint32_t aw, uint32_t b, uint32_t el, uint32_t ret){
    uint32_t res;
    if (!g_n4_v && n4_core(c, th, aw & 0xffffu, b, el, &res)) return res;
    c->ecx = th; PUSH32(el); PUSH32(b); PUSH32(aw); PUSH32(ret);
    sub_00669296(c);
    return c->eax;
}
static void n5_pass(CPU *c){
    uint32_t th = c->ecx, sp = c->esp, ret = MR32(sp), aw = MR32(sp+4u), el = MR32(sp+8u);
    uint32_t a = aw & 0xffffu, P = a*0x64u + MR32(th+0x2cu), r = 0;
    uint32_t sebp = c->ebp, sebx = c->ebx, sesi = c->esi, sedi = c->edi;
    PUSH32(sebp); uint32_t fp = c->esp; PUSH32(sebx); PUSH32(sesi); PUSH32(sedi);
    c->ebp = fp; c->ebx = a; c->esi = P; c->edi = th;
    n5_calls++;
    if (MR8(th+0x118u) & 8u){
        c->ecx = th; PUSH32(P+0x1cu); PUSH32(MR16(P+0x18u) != 0xffffu); PUSH32(GA(0x6691e5u));
        sub_0066404c(c); r = c->eax; if ((int32_t)r < 0) goto done;
        c->ecx = th; PUSH32(P+0x24u); PUSH32(MR16(P+0x1au) != 0xffffu); PUSH32(GA(0x669204u));
        sub_006641a7(c); r = c->eax; if ((int32_t)r < 0) goto done;
    }
    if (MR8(P+8u) & 8u){
        c->ecx = th; PUSH32(a);
        if (MR16(P+0x18u) == 0xffffu){ PUSH32(GA(0x669241u)); sub_00667d61(c); }
        else { PUSH32(GA(0x669222u)); sub_00666e07(c); }
        r = c->eax; if ((int32_t)r < 0) goto done;
    }
    if (MR32(th+0x3cu) == 4u){
        if (MR8(P+8u) & 1u){
            c->ecx = th; PUSH32(P); PUSH32(GA(0x66923au)); sub_00668e0d(c);
            r = c->eax; goto done;
        }
        for (uint32_t b = 0; b < MR16(P+0x36u); b++){
            r = n5_state(c, th, aw, b, el, GA(0x669259u));
            if ((int32_t)r < 0) goto done;
        }
    } else {
        for (uint32_t b = 0; b < MR16(P+0x36u); b++){
            r = n5_state(c, th, aw, b, el, GA(0x66927cu));
            if ((int32_t)r < 0 && MR32(th+0x3cu) != 1u) goto done;
        }
    }
    r = 0;
done:
    c->eax = r;
    c->edi = sedi; c->esi = sesi; c->ebx = sebx; c->ebp = sebp;
    c->esp = sp + 12u; c->eip = ret;
}

void sub_006691b1(CPU *c){
    if (g_n5_on < 0){ const char *e = getenv("D3DXNATIVE5"); g_n5_on = e ? atoi(e) != 0 : 1; }
    if (g_n5_on && g_n4_on > 0 && !g_m3_on && !g_pctx_n){
        uint64_t t0 = dx_on() ? mach_absolute_time() : 0;
        n5_pass(c);
        if (t0){ g_dx[DX_PASSAPPLY].ticks += mach_absolute_time() - t0; g_dx[DX_PASSAPPLY].n++; }
        return;
    }
    if (g_m3_on < 0){
        /* OFF BY DEFAULT: D3DXVERIFY3 showed that skipping at the pass level is wrong
         * (textures come from the object table, not from parameter data; 78% of
         * the predicted hits were actually different).
         * D3DXMEMO3=1 for research only. */
        const char *m = getenv("D3DXMEMO3"); g_m3_on = m ? atoi(m) != 0 : 0;
        const char *v = getenv("D3DXVERIFY3"); g_v3_n = v ? atoi(v) : 0;
        if (g_m3_on || g_v3_n){ g_m3 = calloc(M3_CAP, sizeof *g_m3); g_pv_on = 1; }
    }
    uint64_t t0 = dx_on() ? mach_absolute_time() : 0;
    if (!g_m3){ sub_006691b1_orig(c); goto out3; }
    {
    uint32_t th = c->ecx, a = MR16(c->esp+4u), el = MR16(c->esp+8u);
    uint32_t P = a*0x64u + MR32(th+0x2cu);
    m3_calls++;
    uint32_t ab = (a<<16) | el;
    uint32_t hk = (th * 0x9E3779B1u) ^ (ab * 0x85EBCA77u);
    M3Slot *s = NULL;
    for (uint32_t j = 0; j < 32u; j++){
        M3Slot *t = &g_m3[(hk + j) & (M3_CAP-1)];
        if (!t->n || (t->th == th && t->ab == ab)){ s = t; break; }
    }
    if (!s){ sub_006691b1_orig(c); goto out3; }
    if (!s->rec.call) DXREC_INIT(&s->rec, s->rcall, s->rdata);
    int mine = s->n && s->th == th && s->ab == ab;
    SnapW w = { s->buf, 0, M3_BUF, mine && s->len };
    s->th = th; s->ab = ab; if (!s->n) s->n = 1;
    if (!pass_fp(&w, th, P)){ s->valid = 0; sub_006691b1_orig(c); goto out3; }
    int hit = 0;
    if (mine && s->valid){
        if (!(w.eq && w.len == s->len)) m3_fp++;
        else {
            uint32_t k = 0;
            for (; k < s->ctx.ndep; k++) if (pv_get(s->ctx.dep[k]) > s->ep0) break;
            if (k < s->ctx.ndep) m3_dep++;
            else if (!dxrec_redundant(&s->rec)) m3_dev++;
            else hit = 1;
        }
    } else if (mine) m3_inel++;
    if (hit){
        m3_hit++;
        if (g_v3_n && (m3_hit % (uint64_t)g_v3_n) == 0){
            uint32_t lo = th & ~0x3fffu, hi = lo + 0x4000u, dn = V_DATA1 - V_DATA0;
            while (lo - 0x4000u >= th - V_WIN && mem_is_committed(lo - 0x4000u)) lo -= 0x4000u;
            while (hi + 0x4000u <= th + V_WIN && mem_is_committed(hi)) hi += 0x4000u;
            uint32_t n = hi - lo;
            if (!v_buf0){ v_buf0 = malloc(2u*V_WIN + 0x8000u); v_buf1 = malloc(dn); }
            memcpy(v_buf0, g_mem + lo, n); memcpy(v_buf1, g_mem + V_DATA0, dn);
            static DxRec vr; static DxCall vc[256]; static uint8_t vd[4096];
            DXREC_INIT(&vr, vc, vd);
            dxrec_begin(&vr, DXREC_PASS);
            sub_006691b1_orig(c);
            dxrec_end();
            v3_runs++;
            { uint32_t off = th + 0x114u - lo; memcpy(v_buf0 + off, g_mem + th + 0x114u, 8); }
            int dm = memcmp(v_buf0, g_mem + lo, n) || memcmp(v_buf1, g_mem + V_DATA0, dn);
            int dd = !vr.allred, de = c->eax != s->eax;
            if (dm || dd || de){
                if (dm) v3_mem++;
                if (dd) v3_dev++;
                if (de) v3_eax++;
                static int shown = 0;
                if (shown++ < 12){
                    fprintf(stderr, "[D3DXVERIFY3] this=%08x pass=%u: memory=%d device=%d eax=%d calls %u (record %u) dependencies %u\n",
                            th, a, dm, dd, de, vr.ncall, s->rec.ncall, s->ctx.ndep);
                    for (int q = 0; q < vr.ncall; q++) if (!vr.call[q].red){
                        const DxCall *k = &vr.call[q];
                        uint32_t old = 0xFFFFFFFFu;
                        for (int z = 0; z < s->rec.ncall; z++)
                            if (s->rec.call[z].mid == k->mid && s->rec.call[z].a[0] == k->a[0]
                                && (k->mid != s->rec.call[z].mid || s->rec.call[z].a[1] == k->a[1] || 1)){ old = s->rec.call[z].a[2]; if (s->rec.call[z].a[1] != k->a[1]) old = s->rec.call[z].a[1]; break; }
                        fprintf(stderr, "      effective call: %s(%u, %u, %u)  in record: %08x\n",
                                dxrec_mid_name(k->mid), k->a[0], k->a[1], k->a[2], old);
                    }
                    if (dm && shown <= 3 && memcmp(v_buf0, g_mem + lo, n)) verify_diff(lo, n, v_buf0, th, P, P);
                }
            } else v3_clean++;
            goto out3;
        }
        if (g_m3_on){
            m3_skip++;
            dxrec_append(&s->rec);
            POP32(c->eip); c->esp += 8u; c->eax = s->eax;
            goto out3;
        }
    }
    /* record: gather dependencies and device calls */
    m3_rec++;
    s->valid = 0; s->len = w.len;
    s->ep0 = g_pepoch;
    s->ctx.elig = !(MR32(P+8u) & 8u) && !(MR32(th+0x3cu) == 4u && (MR32(P+8u) & 1u));
    s->ctx.ndep = 0; s->ctx.tmask = 0;
    if (g_pctx_n < 8) g_pctx[g_pctx_n] = &s->ctx; else s->ctx.elig = 0;
    g_pctx_n++;
    dxrec_begin(&s->rec, DXREC_PASS);
    sub_006691b1_orig(c);
    dxrec_end();
    g_pctx_n--;
    s->eax = c->eax;
    /* if a parameter changed during recording (it shouldn't), don't trust it */
    s->valid = s->rec.ok && s->ctx.elig && s->ep0 == g_pepoch;
    if (s->valid) m3_recok++;
    else {
        if (!s->rec.ok) m3_why[0]++;
        if (!s->ctx.elig) m3_why[1]++;
        if (s->ep0 != g_pepoch) m3_why[2]++;
        if (MR32(th+0x118u) & 8u) m3_why[3]++;
        if (MR32(P+8u) & 8u) m3_why[4]++;
        if (MR32(th+0x3cu) == 4u && (MR32(P+8u) & 1u)) m3_why[5]++;
        if (s->rec.ncall >= s->rec.maxcall) m3_why[6]++;
        m3_why_tm |= s->ctx.tmask;
    }
    }
out3:
    if (t0){ g_dx[DX_PASSAPPLY].ticks += mach_absolute_time() - t0; g_dx[DX_PASSAPPLY].n++; }
}

/* ---- Guest thread CPU time: a measurement independent of GPU waits ----
 * CPUWIN=<start>[,<end>]: the thread CPU clock is read at Present (guest
 * thread); the report prints the CPU ms per frame of frames [start, end).
 * Since frame time is max(CPU, GPU), this is the only reliable CPU measure when
 * the GPU is shared with another application.                              */
#include <time.h>
static long cw_from = -2, cw_to = 0, cw_f0 = -1, cw_f1 = -1;
static double cw_t0, cw_t1;
/* per-thread instruction/cycle counters (libsystem_kernel thread_selfcounts):
 * much more stable than time; the efficiency measure of the code the translator
 * generates */
#include <dlfcn.h>
static uint64_t cw_i0[2], cw_i1[2];
static int (*cw_tsc)(int, void *, size_t);
static void cw_counts(uint64_t *v){
    static int init = 0;
    if (!init){ init = 1; cw_tsc = (int (*)(int, void *, size_t))dlsym(RTLD_DEFAULT, "thread_selfcounts"); }
    v[0] = v[1] = 0;
    if (cw_tsc) cw_tsc(1, v, 2 * sizeof(uint64_t));
}
void guest_cpu_mark(void){
    if (cw_from == -2){
        const char *e = getenv("CPUWIN"); cw_from = -1;
        if (e){ cw_from = atol(e); const char *k = strchr(e, ','); cw_to = k ? atol(k+1) : 1L<<40; }
    }
    if (cw_from < 0) return;
    long f = metal_frames();
    if (f < cw_from || (cw_f1 >= 0 && f > cw_to)) return;
    struct timespec ts; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    double t = ts.tv_sec*1e3 + ts.tv_nsec/1e6;
    if (cw_f0 < 0){ cw_f0 = f; cw_t0 = t; cw_counts(cw_i0); }
    if (f <= cw_to){ cw_f1 = f; cw_t1 = t; cw_counts(cw_i1); }
}

/* ---- EFXSTAT=<frame>: inventory of the ID3DXEffect vtable (0x7b6fe8, 82 methods) ----
 * Every effect method call coming from dispatch_indirect: calls/frame per method
 * (from the game / from inside D3DX), distinct effect objects, Begin flags,
 * distinct (effect, pass) pairs.  Diagnostics only.                     */
#define EFX_NM 82
int g_efx_stat;
static long efx_from, efx_f0 = -1;
static uint32_t efx_vt[EFX_NM];
static uint64_t efx_n[EFX_NM][2], efx_hstr[EFX_NM];
static uint32_t efx_this[1024]; static int efx_nthis;
static uint64_t efx_pair[4096]; static int efx_npair;
static uint32_t efx_bflag[8], efx_bflag_n[8]; static int efx_nbf;
extern int g_fxtr; int fxtr_now(void);
__attribute__((constructor)) static void efx_init(void){
    const char *e = getenv("EFXSTAT");
    g_efx_stat = e != NULL || getenv("FXTRACE") != NULL;
    efx_from = e ? atol(e) : (1L << 40);
}
/* FXTRACE: effect method marker (in the same stream as com.c's device call trace) */
static void efx_trace(CPU *c, int k){
    static const char *nm[EFX_NM] = { [26]="SetInt", [28]="SetIntArray", [30]="SetFloat",
        [34]="SetVector", [36]="SetVectorArray", [38]="SetMatrix", [40]="SetMatrixArray",
        [52]="SetTexture", [60]="SetTechnique", [65]="Begin", [66]="BeginPass",
        [67]="CommitChanges", [68]="EndPass", [69]="End" };
    uint32_t sp = c->esp, th = MR32(sp+4u), a1 = MR32(sp+8u), a2 = MR32(sp+12u), a3 = MR32(sp+16u);
    fprintf(stderr, "E %s[%d] %08x %08x", nm[k] ? nm[k] : "?", k, th, a1);
    uint32_t nf = 0, p = a2;
    switch (k){
    case 26: case 52: case 60: case 65: fprintf(stderr, " %x", a2); break;
    case 30: { float f; memcpy(&f, &a2, 4); fprintf(stderr, " %g", f); } break;
    case 28: fprintf(stderr, " n=%u:", a3); for (uint32_t i = 0; i < a3 && i < 64; i++) fprintf(stderr, " %d", (int)MR32(a2+4u*i)); break;
    case 34: nf = 4; break;
    case 36: nf = 4u*a3; fprintf(stderr, " n=%u", a3); break;
    case 38: nf = 16; break;
    case 40: nf = 16u*a3; fprintf(stderr, " n=%u", a3); break;
    }
    for (uint32_t i = 0; i < nf && i < 256; i++){
        float f; uint32_t u = MR32(p+4u*i); memcpy(&f, &u, 4);
        fprintf(stderr, "%s%g", (i&3) ? "," : " ", f);
    }
    fprintf(stderr, "\n");
}
static void efx_set_add(uint32_t *s, int *n, int cap, uint32_t v){
    for (int i = 0; i < *n; i++) if (s[i] == v) return;
    if (*n < cap) s[(*n)++] = v;
}
void efx_stat_note(CPU *c, uint32_t addr){
    if (!efx_vt[0]) for (int i = 0; i < EFX_NM; i++) efx_vt[i] = MR32(GA(0x7b6fe8u) + 4u*i);
    int k = -1;
    for (int i = 0; i < EFX_NM; i++) if (efx_vt[i] == addr){ k = i; break; }
    if (k < 0) return;
    if (g_fxtr && fxtr_now()) efx_trace(c, k);
    if (metal_frames() < efx_from) return;
    if (efx_f0 < 0) efx_f0 = metal_frames();
    uint32_t ret = MR32(c->esp), th = MR32(c->esp+4u);
    efx_n[k][ret - 0x650000u < 0x40000u]++;
    efx_set_add(efx_this, &efx_nthis, 1024, th);
    /* is the handle a string?  (the first argument in slots 8..64 is a D3DXHANDLE) */
    if (k >= 8 && k <= 64){
        uint32_t h = MR32(c->esp+8u);
        int str = h >= 0x10000u && mem_is_committed(h) && mem_is_committed(h+7u);
        for (int i = 0; str && i < 8; i++){
            uint8_t ch = MR8(h+i); if (!ch){ str = i > 0; break; }
            if (!((ch>='A'&&ch<='Z')||(ch>='a'&&ch<='z')||(ch>='0'&&ch<='9')||ch=='_')) str = 0;
        }
        efx_hstr[k] += str;
    }
    if (k == 65){                                    /* Begin(this, pPasses, flags) */
        uint32_t f = MR32(c->esp+12u); int j = 0;
        for (; j < efx_nbf; j++) if (efx_bflag[j] == f) break;
        if (j == efx_nbf && efx_nbf < 8) efx_bflag[efx_nbf++] = f;
        if (j < 8) efx_bflag_n[j]++;
    }
    if (k == 66){                                    /* BeginPass(this, pass) */
        uint64_t p = ((uint64_t)th << 32) | MR32(c->esp+8u);
        int j = 0; for (; j < efx_npair; j++) if (efx_pair[j] == p) break;
        if (j == efx_npair && efx_npair < 4096) efx_pair[efx_npair++] = p;
    }
}
/* D3DXCreateEffectFromResourceA(device, module, resource, defines, include,
 * flags, pool, &effect, &errors)  stdcall, 9 arguments. */
void sub_00640c12_orig(CPU *c);
void sub_00640c12(CPU *c){
    uint32_t sp = c->esp, res = MR32(sp+12u), pool = MR32(sp+28u), pout = MR32(sp+32u);
    sub_00640c12_orig(c);
    if (g_efx_stat){
        char nm[64] = "#";
        if (res >= 0x10000u){ int i = 0; for (; i < 63 && MR8(res+i); i++) nm[i] = (char)MR8(res+i); nm[i] = 0; }
        else snprintf(nm, sizeof nm, "#%u", res);
        uint32_t ef = pout ? MR32(pout) : 0u;
        fprintf(stderr, "[EFXSTAT] create %-28s pool=%08x effect=%08x result=%08x frame=%ld"
                " param %u@%08x technique %u@%08x object@%08x\n",
                nm, pool, ef, c->eax, metal_frames(), ef ? MR32(ef+0x98u) : 0u, ef ? MR32(ef+0x9cu) : 0u,
                ef ? MR32(ef+0xa0u) : 0u, ef ? MR32(ef+0xa4u) : 0u, ef ? MR32(ef+0x18u) : 0u);
    }
}

static void efx_report(void){
    if (!g_efx_stat || efx_f0 < 0) return;
    long fr = metal_frames() - efx_f0; if (fr < 1) fr = 1;
    fprintf(stderr, "\n=== EFXSTAT: ID3DXEffect calls, %ld frames (from frame %ld) ===\n", fr, efx_f0);
    fprintf(stderr, "  slot  address    game/frame  D3DX-inner/frame   total   string-handle\n");
    for (int i = 0; i < EFX_NM; i++) if (efx_n[i][0] || efx_n[i][1])
        fprintf(stderr, "  %3d   %08x  %10.1f  %10.1f  %8llu  %8llu\n", i, efx_vt[i],
                (double)efx_n[i][0]/fr, (double)efx_n[i][1]/fr,
                (unsigned long long)(efx_n[i][0] + efx_n[i][1]), (unsigned long long)efx_hstr[i]);
    fprintf(stderr, "  distinct effect objects: %d | distinct (effect, pass): %d\n", efx_nthis, efx_npair);
    fprintf(stderr, "  Begin flags:");
    for (int j = 0; j < efx_nbf; j++) fprintf(stderr, " 0x%x x%u", efx_bflag[j], efx_bflag_n[j]);
    fprintf(stderr, "\n");
}

void d3dx_hle_report(void){
    efx_report();
    { extern void comstat_report(void); comstat_report(); }
    if (cw_f1 > cw_f0 && cw_f0 >= 0)
        fprintf(stderr, "\n=== CPUWIN: guest thread CPU %.3f ms/frame (%ld..%ld, %ld frames) ===\n",
                (cw_t1 - cw_t0) / (double)(cw_f1 - cw_f0), cw_f0, cw_f1, cw_f1 - cw_f0);
        if (cw_i1[0] > cw_i0[0]){
            double fr = (double)(cw_f1 - cw_f0), ki = (double)(cw_i1[0] - cw_i0[0]), kc = (double)(cw_i1[1] - cw_i0[1]);
            fprintf(stderr, "=== CPUWIN: guest thread %.3f M instr/frame, %.3f M cycles/frame, IPC %.2f ===\n",
                    ki / fr / 1e6, kc / fr / 1e6, ki / kc);
        }
    if (m3_calls){
        fprintf(stderr, "  --- pass memo (sub_006691b1, Stage 3) ---\n");
        fprintf(stderr, "  calls %llu | hits %llu (%.1f%%) | skipped %llu | records %llu (eligible %llu)\n"
                        "  misses: fingerprint %llu, parameter changed %llu, device changed %llu, not eligible %llu\n"
                        "  versions: %llu parameter changes, table full %llu\n",
            (unsigned long long)m3_calls, (unsigned long long)m3_hit, 100.0*m3_hit/m3_calls,
            (unsigned long long)m3_skip, (unsigned long long)m3_rec, (unsigned long long)m3_recok,
            (unsigned long long)m3_fp, (unsigned long long)m3_dep, (unsigned long long)m3_dev,
            (unsigned long long)m3_inel, (unsigned long long)pv_bumps, (unsigned long long)pv_full);
        fprintf(stderr, "  ineligible records: rec %llu, type/dependency %llu, parameter %llu, flag8 %llu, P&8 %llu, 3c==4 %llu, call overflow %llu, types seen %08x\n",
            (unsigned long long)m3_why[0], (unsigned long long)m3_why[1], (unsigned long long)m3_why[2],
            (unsigned long long)m3_why[3], (unsigned long long)m3_why[4], (unsigned long long)m3_why[5],
            (unsigned long long)m3_why[6], m3_why_tm);
        if (v3_runs) fprintf(stderr, "  VERIFY3: %llu runs, clean %llu | memory %llu, device %llu, eax %llu\n",
            (unsigned long long)v3_runs, (unsigned long long)v3_clean, (unsigned long long)v3_mem,
            (unsigned long long)v3_dev, (unsigned long long)v3_eax);
    }
    if (n4_calls || n4vs.runs){
        fprintf(stderr, "  --- native state applier (669296 types 1/2/3, Stage 4a) ---\n"
                        "  calls %llu | dirty (664a2f) %llu | device calls made %llu, skipped (no effect) %llu\n",
            (unsigned long long)n4_calls, (unsigned long long)n4_dirty,
            (unsigned long long)n4_issued, (unsigned long long)n4_skipped);
        fprintf(stderr, "  native pass loop (6691b1, Stage 4b): %llu\n", (unsigned long long)n5_calls);
        fprintf(stderr, "  expression memo: hits %llu, misses %llu, not memoizable %llu (not an expression %llu, no inputs %llu, inputs>2 %llu, >96 bytes %llu)\n",
            (unsigned long long)n4m_hit, (unsigned long long)n4m_miss, (unsigned long long)n4m_big,
            (unsigned long long)n4m_why[0], (unsigned long long)n4m_why[1], (unsigned long long)n4m_why[2], (unsigned long long)n4m_why[3]);
        if (n4vs.runs) fprintf(stderr, "  VERIFY-4: %llu runs, DIFFERENT %llu (record invalid, not compared: %llu)\n",
            (unsigned long long)n4vs.runs, (unsigned long long)n4vs.bad, (unsigned long long)n4vs.norec);
        if (n8_set || n8_tex) fprintf(stderr, "  native SetTexture: %llu | texture slot (65ccd8): %llu | skipped texture AddRef/Release: %llu\n",
            (unsigned long long)n8_set, (unsigned long long)n8_tex, (unsigned long long)n8_refskip);
        if (n7vs.runs || n7_calls) fprintf(stderr, "  native shader applier (668e0d): %llu | VERIFY-7: %llu runs, DIFFERENT %llu (not compared %llu) | object state (type 14/15): %llu\n",
            (unsigned long long)n7_calls, (unsigned long long)n7vs.runs, (unsigned long long)n7vs.bad, (unsigned long long)n7vs.norec, (unsigned long long)n4_obj);
        if (n6vs.runs || n6_calls) fprintf(stderr, "  native sampler applier (668cd0): %llu | VERIFY-6: %llu runs, DIFFERENT %llu (not compared %llu)\n",
            (unsigned long long)n6_calls, (unsigned long long)n6vs.runs, (unsigned long long)n6vs.bad, (unsigned long long)n6vs.norec);
    }
    if (mm_calls || pl_calls || sm_calls){
        fprintf(stderr, "  --- native helpers ---\n  matrix multiply %llu (slow path %llu) | pool notification %llu"
                        " | SetMatrix conversion %llu (slow path %llu)\n",
            (unsigned long long)mm_calls, (unsigned long long)mm_slow, (unsigned long long)pl_calls,
            (unsigned long long)sm_calls, (unsigned long long)sm_slow);
        if (mmv_runs || plv_runs || smv_runs)
            fprintf(stderr, "  VERIFY-N: matrix %llu runs, DIFFERENT %llu | pool %llu runs, DIFFERENT %llu"
                            " | conversion %llu runs, DIFFERENT %llu\n",
                (unsigned long long)mmv_runs, (unsigned long long)mmv_bad,
                (unsigned long long)plv_runs, (unsigned long long)plv_bad,
                (unsigned long long)smv_runs, (unsigned long long)smv_bad);
    }
    if (m2_calls){
        fprintf(stderr, "  --- state assignment memo (sub_00669296) ---\n");
        fprintf(stderr, "  calls %llu | same input %llu (%.1f%%) | hits %llu (%.1f%%) | skipped %llu\n"
                        "  record invalid %llu | device changed %llu | buffer overflow %llu | out of type %llu\n",
            (unsigned long long)m2_calls, (unsigned long long)m2_same, 100.0*m2_same/m2_calls,
            (unsigned long long)m2_hit, 100.0*m2_hit/m2_calls, (unsigned long long)m2_skip,
            (unsigned long long)m2_norec, (unsigned long long)m2_devchg, (unsigned long long)m2_big,
            (unsigned long long)m2_excl);
        fprintf(stderr, "  type: calls/hit%%");
        for (int i = 0; i < 32; i++) if (m2_type_calls[i])
            fprintf(stderr, "  %d:%llu/%.0f%%", i, (unsigned long long)m2_type_calls[i],
                    100.0*m2_type_hit[i]/m2_type_calls[i]);
        fprintf(stderr, "\n");
        if (v2_runs){
            fprintf(stderr, "  VERIFY2: %llu runs, clean %llu | memory %llu, device %llu, eax %llu\n",
                (unsigned long long)v2_runs, (unsigned long long)v2_clean, (unsigned long long)v2_mem,
                (unsigned long long)v2_dev, (unsigned long long)v2_eax);
            fprintf(stderr, "  faulty types:");
            for (int i = 0; i < 32; i++) if (v2_type_bad[i]) fprintf(stderr, " %d:%llu", i, (unsigned long long)v2_type_bad[i]);
            fprintf(stderr, "\n");
        }
    }
    if (m_calls){
        fprintf(stderr, "  --- state value memo (sub_00664a2f) ---\n");
        if (m_big) fprintf(stderr, "  input over %u bytes (not memoized): %llu\n", MEMO_BUF, (unsigned long long)m_big);
        fprintf(stderr, "  calls %llu | eligible %llu | hits %llu (%.1f%%) | skipped %llu | guard aborts %llu\n",
            (unsigned long long)m_calls, (unsigned long long)m_elig, (unsigned long long)m_hit,
            m_elig ? 100.0*m_hit/m_elig : 0.0, (unsigned long long)m_skip, (unsigned long long)m_guard);
        if (v_runs) fprintf(stderr, "  VERIFY: %llu runs, clean %llu, DIFFERENT %llu\n",
            (unsigned long long)v_runs, (unsigned long long)v_clean, (unsigned long long)v_dirty);
    }
    if (!dx_on()) return;
    long fr = metal_frames() - (g_dx_f0 < 0 ? 0 : g_dx_f0); if (fr < 1) fr = 1;
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    fprintf(stderr, "\n=== D3DX effect (D3DXSTAT) — %ld frames, from frame %ld ===\n", fr, g_dx_f0);
    fprintf(stderr, "  %-20s %10s %12s %10s %10s\n",
            "function", "calls/frame", "inclusive", "ms/frame", "us/call");
    for (int i = 0; i < DX_N; i++){
        DxStat *s = &g_dx[i];
        if (!s->n) continue;
        double ns = (double)s->ticks * tb.numer / tb.denom;
        fprintf(stderr, "  %-20s %10.1f   sub_%08x %10.3f %10.3f\n", s->name,
                (double)s->n / fr, s->addr, ns / 1e6 / fr, ns / 1e3 / s->n);
    }
    if (an_calls){
        fprintf(stderr, "  --- D3DXANALYZE: expression evaluation inputs ---\n");
        fprintf(stderr, "  calls %llu | first %llu | input SAME %llu (%.1f%%) | DIFFERENT %llu (%.1f%%)\n",
            (unsigned long long)an_calls, (unsigned long long)an_first,
            (unsigned long long)an_same, 100.0*an_same/an_calls,
            (unsigned long long)an_diff, 100.0*an_diff/an_calls);
        fprintf(stderr, "  without dirty inputs %llu (%.1f%%), mean bytes hashed %.0f\n",
            (unsigned long long)an_dirty0, 100.0*an_dirty0/an_calls, (double)an_bytes/an_calls);
        fprintf(stderr, "  dirty input count distribution:");
        for (int i = 0; i < 9; i++) fprintf(stderr, " %d:%llu", i, (unsigned long long)an_hist_inputs[i]);
        fprintf(stderr, "\n");
    }
}

