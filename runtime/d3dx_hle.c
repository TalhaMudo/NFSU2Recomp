/* D3DX9 efekt kutuphanesi (oyuna statik bagli) -- HLE calismasi.
 *
 * Olcum (2026-09-23): yaris karesinde konuk thread'in %48'i
 * ID3DXEffect::BeginPass (vtable 0x7b6fe8, yuva 66 = sub_0066ace2) icinde.
 * Bunun buyuk kismi D3D cagrisi degil, durum ifadelerinin (preshader)
 * CPU'da yorumlanmasi:
 *
 *   BeginPass 66ace2
 *     -> 6691b1  gecisi uygula
 *        -> 669296  tek durum ata (tip>>24 anahtari, 100 baytlik kayit)
 *           -> 664a2f -> 663a42  durum ifadesini degerlendir
 *                          -> 663275 / 662edc  girdileri topla
 *                          -> 688e89  preshader VM (x87)
 *        -> 669f40 -> 668e0d -> 668cd0 -> 664a2f ...
 *
 * Asama 0: sayac sarmalayicilari (D3DXSTAT, D3DXANALYZE, CPUWIN).
 * Asama 1: sub_00664a2f durum degeri notu (varsayilan acik, D3DXMEMO=0
 *          kapatir; D3DXVERIFY=<n> bellek karsilastirmali dogrulama).
 * Asama 2: sub_00669296 durum atama notu (varsayilan acik, D3DXMEMO2=0
 *          kapatir; D3DXVERIFY2=<n>).  Cihaz cagrisi kaydi com.c'de (dxrec.h).
 * Asama 3: parametre surumleri (sub_0065ab24 kancasi) + sub_006691b1 gecis
 *          notu.  Dogrulamada YANLIS cikti -> varsayilan KAPALI (D3DXMEMO3=1).
 * Ayrintilar, olcumler ve ilerleme gunlugu: D3DX_HLE.md.
 *
 * DIKKAT: buradaki her sub_XXXX, runtime/overrides.c'dekilerle birlikte
 * genall.py'nin OVERRIDE listesinde olmali (bkz. D3DX_HLE.md).          */
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
    [DX_BEGINPASS]      = {"BeginPass",        0x0066ace2u},
    [DX_PASSAPPLY]      = {"gecis uygula",      0x006691b1u},
    [DX_STATEAPPLY]     = {"durum ata",         0x00669296u},
    [DX_664A2F]         = {"sub_00664a2f",      0x00664a2fu},
    [DX_EVALSTATE]      = {"ifade degerlendir", 0x00663a42u},
    [DX_VM]             = {"preshader VM",      0x00688e89u},
    [DX_GATHER_A]       = {"girdi topla (0)",   0x00663275u},
    [DX_GATHER_B]       = {"girdi topla (1,2)", 0x00662edcu},
    [DX_660A52]         = {"sub_00660a52",      0x00660a52u},
    [DX_668E0D]         = {"sub_00668e0d",      0x00668e0du},
    [DX_BEGIN]          = {"Begin",             0x00664792u},
    [DX_COMMIT]         = {"CommitChanges",     0x0066ac27u},
    [DX_ENDPASS]        = {"EndPass",           0x0065addbu},
    [DX_END]            = {"End",               0x0065acf5u},
    [DX_SETMATRIX]      = {"SetMatrix",         0x0066d13au},
    [DX_SETVECTOR]      = {"SetVector",         0x0066d303u},
    [DX_SETTEXTURE]     = {"SetTexture",        0x0066d7e9u},
    [DX_SETINTARRAY]    = {"SetIntArray",       0x0066e8a9u},
    [DX_SETFLOAT]       = {"SetFloat",          0x0066f501u},
    [DX_SETVECTORARRAY] = {"SetVectorArray",    0x0066ca38u},
};

long metal_frames(void);
int g_dx_in_pass = 0;          /* BeginPass/CommitChanges derinligi (COMSTAT) */
static int  g_dx_on = -1;
static long g_dx_from = 0, g_dx_f0 = -1;   /* D3DXSTAT=<kare>: o kareden itibaren say */
static inline int dx_on(void){
    if (g_dx_on < 0){
        const char *e = getenv("D3DXSTAT");
        g_dx_on = e != NULL; g_dx_from = e ? atol(e) : 0;
    }
    return g_dx_on;
}
/* Sayaclari olcum penceresinin basinda sifirla (yalnizca BeginPass'ten). */
static void dx_window(void){
    if (g_dx_f0 >= 0) return;
    long f = metal_frames();
    if (f < g_dx_from) return;
    for (int i = 0; i < DX_N; i++){ g_dx[i].n = 0; g_dx[i].ticks = 0; }
    g_dx_f0 = f;
}

#define DX_WRAP(idx, hex)                                                   \
    void sub_##hex##_orig(CPU *c);                                          \
    void sub_##hex(CPU *c){                                                 \
        if (!dx_on()){ sub_##hex##_orig(c); return; }                       \
        if (idx == DX_BEGINPASS) dx_window();                               \
        int inp = (idx == DX_BEGINPASS || idx == DX_COMMIT);                \
        g_dx_in_pass += inp;                                                \
        uint64_t t0 = mach_absolute_time();                                 \
        sub_##hex##_orig(c);                                                \
        g_dx_in_pass -= inp;                                                \
        g_dx[idx].ticks += mach_absolute_time() - t0;                       \
        g_dx[idx].n++;                                                      \
    }

DX_WRAP(DX_BEGINPASS,      0066ace2)
DX_WRAP(DX_VM,             00688e89)
DX_WRAP(DX_GATHER_A,       00663275)
DX_WRAP(DX_GATHER_B,       00662edc)
DX_WRAP(DX_660A52,         00660a52)
DX_WRAP(DX_668E0D,         00668e0d)
DX_WRAP(DX_BEGIN,          00664792)
DX_WRAP(DX_COMMIT,         0066ac27)
DX_WRAP(DX_ENDPASS,        0065addb)
DX_WRAP(DX_END,            0065acf5)
DX_WRAP(DX_SETMATRIX,      0066d13a)
DX_WRAP(DX_SETVECTOR,      0066d303)
DX_WRAP(DX_SETTEXTURE,     0066d7e9)
DX_WRAP(DX_SETINTARRAY,    0066e8a9)
DX_WRAP(DX_SETFLOAT,       0066f501)
DX_WRAP(DX_SETVECTORARRAY, 0066ca38)

/* ---- Analiz: ifade degerlendirmesinin girdileri gercekten degisiyor mu? ----
 * D3DXANALYZE=<kare>: o kareden itibaren her sub_00663a42 cagrisinda durumun
 * girdi parametre verisinin ozetini onceki degerlendirmeyle karsilastirir.
 *
 * sub_00663a42(this=ecx, a, b)  ret 8
 *   pass/param kaydi : P = a*0x64 + [this+0x2c]
 *   durum kaydi      : R = b*0x64 + [P+4]
 *   girdi kayitlari  : [R+0x34] + i*32, i < word[R+0x4e]   (yalnizca [R+0x3c] != 0)
 *     +0  kirli bayragi (0 degilse topla, sonra [+0] = [+4])
 *     +0xc -> {veri isaretcisi, tanim}   tanim: +0 tur, +4 sinif, +0x10 eleman,
 *                                              +0x14 satir, +0x18 sutun
 *     +0x10 -> yazmac tanimi: word +4 kume (0/1/2), +6 indeks, +8 adet      */
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
            if (!MR32(rec)) continue;             /* kirli degil: D3DX toplamayacak */
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
                fprintf(stderr, "[D3DXANALYZE] anahtar this=%08x a=%u b=%u R=%08x obj=%08x vt=%08x vt[3]=%08x"
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

/* ---- Durum degeri notu (memo): sub_00664a2f(this=ecx, a, b)  ret 8 ----
 * Durumun degerini hesaplayip P.data + [R+0x48]'e yazar.  Ifade durumlarinda
 * ([R+0x38] != 0, [R+0x30]&4 == 0) sonuc yalnizca tek girdinin verisine bagli
 * (D3DXANALYZE: %95.8 ayni).  Girdi ozeti degismediyse cagriyi atlamak
 * bellekte hicbir seyi degistirmemeli -- D3DXVERIFY bunu olcer.
 *   D3DXVERIFY=<n> : tahmini isabetlerin 1/n'inde orijinali calistir, efekt
 *                    nesnesi +-256 KB ve .data'yi once/sonra karsilastir.
 *   D3DXMEMO=0     : notu kapat (varsayilan ACIK).
 * Ek koruma: isabette hedef dword (P.data+[R+0x48]) ve deger tamponunun ilk
 * dword'u ([R+8]) son gercek hesaptan beri degismemis olmali; efekt ayni
 * adreste yeniden yaratilirsa ya da biri hedefe yazarsa yeniden hesaplanir. */
/* Not kaydi: girdinin TAM kopyasi tutulur ve memcmp ile karsilastirilir
 * (ozet degil -- carpisma riski yok; bayt bayt FNV ozeti profilde %8
 * tutuyordu).  MEMO_BUF'u asan girdiler notlanmaz, her zaman hesaplanir. */
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

/* Anlik goruntu yazici: kayit yuvasinin tamponundaki ESKI goruntuyle yerinde
 * karsilastirir, yalnizca ilk farktan sonra kopyalar.  Isabet orani %90+
 * oldugu icin cogu cagrida hic kopya yok (once kopyalayip sonra memcmp
 * etmek profilde %9 tutuyordu). */
typedef struct { uint8_t *d; uint32_t len, cap; int eq; } SnapW;
/* Satir ici: 8 baytlik sozcuklerle XOR-OR karsilastirma.  Sabit n'de
 * derleyici tamamen acar; kutuphane memcmp cagrisi profilde %2.6 idi. */
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

/* Degerlendirmenin girdileri; tampon tasarsa 0. */
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
        fprintf(stderr, "[D3DXVERIFY] fark %08x  (this%+d  R%+d  P.data%+d)  %02x -> %02x\n",
                a, (int)(a-th), (int)(a-R), (int)(a-pd), before[i], g_mem[a]);
        shown++;
        i |= 3u;                       /* ayni dword'u bir kez yaz */
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
    if (g_pctx_n) pctx_note_inputs(R);     /* Asama 3: gecis bagimliligi */
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
            /* pencere: this etrafinda, yalnizca haritalanmis 16 KB sayfalar */
            uint32_t lo = th & ~0x3fffu, hi = lo + 0x4000u, dn = V_DATA1 - V_DATA0;
            while (lo - 0x4000u >= th - V_WIN && mem_is_committed(lo - 0x4000u)) lo -= 0x4000u;
            while (hi + 0x4000u <= th + V_WIN && mem_is_committed(hi)) hi += 0x4000u;
            uint32_t n = hi - lo;
            if (!v_buf0){ v_buf0 = malloc(2u*V_WIN + 0x8000u); v_buf1 = malloc(dn); }
            memcpy(v_buf0, g_mem + lo, n); memcpy(v_buf1, g_mem + V_DATA0, dn);
            uint32_t esp0 = c->esp;
            sub_00664a2f_orig(c);
            v_runs++;
            /* this+0x114 (kayitli FPU CW) ve +0x118 bit0 (CW kaydedildi):
             * 663a42 BeginPass icindeki ilk degerlendirmede FPU hassasiyetini
             * ayarlar, BeginPass sonu geri yukler.  Atlanan degerlendirme CW'ye
             * hic dokunmadigi icin geri yukleme de gerekmez -- zararsiz fark. */
            { uint32_t off = th + 0x114u - lo; memcpy(v_buf0 + off, g_mem + th + 0x114u, 8); }
            int d0 = memcmp(v_buf0, g_mem + lo, n) != 0;
            int d1 = memcmp(v_buf1, g_mem + V_DATA0, dn) != 0;
            if (d0 || d1 || c->eax != s->eax){
                v_dirty++;
                if (d0) verify_diff(lo, n, v_buf0, th, P, R);
                if (d1) verify_diff(V_DATA0, dn, v_buf1, th, P, R);
                if (c->eax != s->eax)
                    fprintf(stderr, "[D3DXVERIFY] eax farki %08x -> %08x\n", s->eax, c->eax);
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
    s->th = th; s->ab = ab; s->valid = 0; s->n++;     /* tampon zaten yeni goruntu */
    sub_00664a2f_orig(c);
    s->len = w.len; s->eax = c->eax; s->valid = 1;
    s->dst = dst; s->dval = rd32s(dst); s->vval = rd32s(vb);
}

/* ---- Asama 3: parametre surumleri ----
 * Parametre verisi yalnizca Set* ile degisir ve Set*'larin hepsi D3DX'in
 * "parametre degisti" noktasi sub_0065ab24'e ulasir (20 cagri yeri, hepsi
 * vtable Set* yuvalari + CloneEffect + parametre bloku yolu).  Orada
 * degisen verinin 64 baytlik satirlarina global bir sayac yazilir.  Efekt
 * havuzu veriyi efektler arasinda paylastigi icin anahtar veri ADRESI.
 * Bir gecisin bagimliliklari = durumlarinin okudugu satirlar; hicbiri kayit
 * anindan beri degismediyse gecis ayni ciktiyi uretir.                    */
#define PV_CAP (1u<<16)
typedef struct { uint32_t line; uint32_t ep; } PvSlot;
static PvSlot  g_pv[PV_CAP];
static uint32_t g_pepoch = 1;
static int      g_pv_on = 0;         /* yalnizca Asama 3 acikken surum tut */
static uint64_t pv_bumps, pv_full;

static inline uint32_t pv_get(uint32_t line){
    uint32_t h = line * 0x9E3779B1u;
    for (uint32_t j = 0; j < 32u; j++){
        PvSlot *t = &g_pv[(h + j) & (PV_CAP-1)];
        if (t->line == line) return t->ep;
        if (!t->line) return 0;
    }
    return 0xFFFFFFFFu;                  /* tablo dolu: her zaman "degismis" */
}
static inline void pv_set(uint32_t line, uint32_t ep){
    uint32_t h = line * 0x9E3779B1u;
    for (uint32_t j = 0; j < 32u; j++){
        PvSlot *t = &g_pv[(h + j) & (PV_CAP-1)];
        if (t->line == line || !t->line){ t->line = line; t->ep = ep; return; }
    }
    pv_full++;
}
/* Parametre yuvasinin veri araligi (ust sinir tahmini: satir basina 16 bayt,
 * matris 4x16).  Tanim: +0x10 eleman, +0x14 satir. */
static inline void slot_range(uint32_t sl, uint32_t *dp, uint32_t *n){
    uint32_t d = MR32(sl), ds = MR32(sl+4u);
    uint32_t el = MR32(ds+0x10u); if (!el) el = 1;
    uint32_t rows = MR32(ds+0x14u); if (!rows) rows = 1;
    uint32_t b = el*rows*16u; if (b > 4096u) b = 4096u;
    *dp = d; *n = d ? b : 0u;
}

void sub_0065ab24_orig(CPU *c);
void sub_0065ab24(CPU *c){
    uint32_t sl = MR32(c->esp+4u), dp, n;
    if (g_pv_on && sl && mem_is_committed(sl) && mem_is_committed(sl+7u)){
        slot_range(sl, &dp, &n);
        if (n){
            uint32_t ep = ++g_pepoch;
            for (uint32_t l = dp >> 6; l <= (dp + n - 1u) >> 6; l++) pv_set(l, ep);
            pv_bumps++;
        }
    }
    sub_0065ab24_orig(c);
}

/* Kaydi alinan gecisin baglami: durum sarmalayicisi bagimlilik bildirir. */
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
/* Bir durumun okudugu parametre verisi: ifade girdileri + deger tamponu.
 * Hem durum atama (669296) hem deger alma (664a2f) sarmalayicisindan
 * cagrilir; sampler nesnelerinin alt durumlari (tur 14/15, 668e0d/668cd0)
 * 669296'dan gecmiyor ama degerlerini 664a2f ile aliyor. */
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

/* ---- Asama 2: durum atama notu, sub_00669296(this=ecx, a, b, elem) ret 0xc ----
 * Bir durumun cihaza uygulanmasi.  Girdilerin anlik goruntusu ayniysa ve
 * son gercek calismada yaptigi D3D cagrilarinin HEPSI simdi etkisizse
 * (com.c dxrec_redundant) fonksiyon atlanir.
 *   D3DXMEMO2=0     : kapat (varsayilan acik)
 *   D3DXVERIFY2=<n> : tahmini isabetlerin 1/n'inde orijinali kayitla calistir;
 *                     bellek penceresi + "her cagri etkisizdi" denetlenir. */
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

void sub_00669296_orig(CPU *c);
void sub_00669296(CPU *c){
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
    /* Tur 9 (doku tablosundan dolayli okuma) ve 14/15 (ifadeyle diziden nesne
     * secip ic ice uygulama) anlik goruntunun disinda girdi okuyor:
     * D3DXVERIFY2 bu turlerde cihaz/bellek farki buldu.  Yalnizca dogrulanmis
     * 1..8 notlanir; gerisi her zaman orijinal kodla calisir. */
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
    /* yuvayi hemen sahiplen: ic ice bir cagri ayni bos yuvayi almasin */
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
                    fprintf(stderr, "[D3DXVERIFY2] tur %u (kod %08x) R=%08x: bellek=%d cihaz=%d eax=%d  cagri %u (kayit ok=%d)\n",
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
            dxrec_append(&s->rec);        /* acik gecis kaydi atlanan cagrilari da bilsin */
            POP32(c->eip); c->esp += 12u; c->eax = s->eax;
            goto out;
        }
    }
    s->valid = 0;                         /* tampon zaten yeni goruntu */
    dxrec_begin(&s->rec, DXREC_STATE);
    sub_00669296_orig(c);
    dxrec_end();
    s->eax = c->eax; s->n++; s->type = type; s->len = w.len; s->valid = 1;
    }
out:
    if (t0){ g_dx[DX_STATEAPPLY].ticks += mach_absolute_time() - t0; g_dx[DX_STATEAPPLY].n++; }
}

/* ---- Asama 3: gecis notu, sub_006691b1(this=ecx, gecis, eleman) ret 8 ----
 * Isabet: parmak izi ayni (efekt alanlari + gecis kaydi), kayit anindan beri
 * bagimli parametre satirlarinin hicbiri degismemis, kaydedilen cihaz
 * cagrilarinin hepsi simdi etkisiz.  Uygunluk: gecisteki her durum tur 1..8,
 * ic ice uygulama yok, 6691b1'in ozel yollari (P&8, [this+0x3c]==4 && P&1)
 * kapali.
 *   D3DXMEMO3=1     : ac (varsayilan KAPALI -- bkz. D3DX_HLE.md Asama 3 sonucu)
 *   D3DXVERIFY3=<n> : isabetlerin 1/n'inde orijinali calistir, bellek + cihaz */
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

/* Parmak izi: efekt alanlari, gecis kaydi ve teknik kaydinin +0x8c..+0xab
 * araligi.  Bayrak 8 acikken 6691b1 her gecis 66404c/6641a7'yi cagiriyor:
 * gecisin shader sabit yazmac sayisini ([P+0x1c..]) teknigin o ana kadarki
 * en buyugu ([teknik+0xa4..0xaa]) ile karsilastirip yalnizca BUYUMEDE
 * kayit hazirligi yapiyor.  Isinmadan sonra cagrisiz; buyume olursa bu
 * alanlar degisir ve gecis yeniden kaydedilir. */
static inline int pass_fp(SnapW *w, uint32_t th, uint32_t P){
    uint32_t fix[5] = { th, MR32(th+0x2cu), MR32(th+0x3cu), MR32(th+0x40u), MR32(th+0x118u) & ~1u };
    uint32_t tech = MR16(th+0x42u)*0xb4u + MR32(th+0xa4u);
    return sw_put(w, fix, sizeof fix) && sw_put(w, g_mem + P, 0x64u)
        && sw_put(w, g_mem + tech + 0x8cu, 0x20u);
}

void sub_006691b1_orig(CPU *c);
void sub_006691b1(CPU *c){
    if (g_m3_on < 0){
        /* VARSAYILAN KAPALI: D3DXVERIFY3 gecis duzeyinde atlamanin yanlis
         * oldugunu gosterdi (dokular nesne tablosundan geliyor, parametre
         * verisinden degil; tahmini isabetlerin %78'i gercekte farkliydi).
         * Yalnizca arastirma icin D3DXMEMO3=1. */
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
                    fprintf(stderr, "[D3DXVERIFY3] this=%08x gecis=%u: bellek=%d cihaz=%d eax=%d cagri %u (kayit %u) bagimlilik %u\n",
                            th, a, dm, dd, de, vr.ncall, s->rec.ncall, s->ctx.ndep);
                    for (int q = 0; q < vr.ncall; q++) if (!vr.call[q].red){
                        const DxCall *k = &vr.call[q];
                        uint32_t old = 0xFFFFFFFFu;
                        for (int z = 0; z < s->rec.ncall; z++)
                            if (s->rec.call[z].mid == k->mid && s->rec.call[z].a[0] == k->a[0]
                                && (k->mid != s->rec.call[z].mid || s->rec.call[z].a[1] == k->a[1] || 1)){ old = s->rec.call[z].a[2]; if (s->rec.call[z].a[1] != k->a[1]) old = s->rec.call[z].a[1]; break; }
                        fprintf(stderr, "      etkili cagri: %s(%u, %u, %u)  kayittaki: %08x\n",
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
    /* kayit: bagimliliklari ve cihaz cagrilarini topla */
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
    /* kayit sirasinda bir parametre degistiyse (olmamali) guvenme */
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

/* ---- Konuk thread CPU suresi: GPU beklemesinden bagimsiz olcum ----
 * CPUWIN=<bas>[,<son>]: Present'te (konuk thread) thread CPU saati okunur;
 * raporda [bas, son) karelerinin kare basina CPU ms'i basilir.  Kare suresi
 * max(CPU, GPU) oldugu icin GPU baska bir uygulamayla paylasildiginda tek
 * guvenilir CPU olcusu budur.                                              */
#include <time.h>
static long cw_from = -2, cw_to = 0, cw_f0 = -1, cw_f1 = -1;
static double cw_t0, cw_t1;
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
    if (cw_f0 < 0){ cw_f0 = f; cw_t0 = t; }
    if (f <= cw_to){ cw_f1 = f; cw_t1 = t; }
}

void d3dx_hle_report(void){
    { extern void comstat_report(void); comstat_report(); }
    if (cw_f1 > cw_f0 && cw_f0 >= 0)
        fprintf(stderr, "\n=== CPUWIN: konuk thread CPU %.3f ms/kare (%ld..%ld, %ld kare) ===\n",
                (cw_t1 - cw_t0) / (double)(cw_f1 - cw_f0), cw_f0, cw_f1, cw_f1 - cw_f0);
    if (m3_calls){
        fprintf(stderr, "  --- gecis notu (sub_006691b1, Asama 3) ---\n");
        fprintf(stderr, "  cagri %llu | isabet %llu (%.1f%%) | atlanan %llu | kayit %llu (uygun %llu)\n"
                        "  kacan: parmak izi %llu, parametre degisti %llu, cihaz degisti %llu, uygun degil %llu\n"
                        "  surum: %llu parametre degisimi, tablo dolu %llu\n",
            (unsigned long long)m3_calls, (unsigned long long)m3_hit, 100.0*m3_hit/m3_calls,
            (unsigned long long)m3_skip, (unsigned long long)m3_rec, (unsigned long long)m3_recok,
            (unsigned long long)m3_fp, (unsigned long long)m3_dep, (unsigned long long)m3_dev,
            (unsigned long long)m3_inel, (unsigned long long)pv_bumps, (unsigned long long)pv_full);
        fprintf(stderr, "  uygunsuz kayit: rec %llu, tur/bagimlilik %llu, parametre %llu, bayrak8 %llu, P&8 %llu, 3c==4 %llu, cagri tasti %llu, gorulen turler %08x\n",
            (unsigned long long)m3_why[0], (unsigned long long)m3_why[1], (unsigned long long)m3_why[2],
            (unsigned long long)m3_why[3], (unsigned long long)m3_why[4], (unsigned long long)m3_why[5],
            (unsigned long long)m3_why[6], m3_why_tm);
        if (v3_runs) fprintf(stderr, "  DOGRULAMA3: %llu kosu, temiz %llu | bellek %llu, cihaz %llu, eax %llu\n",
            (unsigned long long)v3_runs, (unsigned long long)v3_clean, (unsigned long long)v3_mem,
            (unsigned long long)v3_dev, (unsigned long long)v3_eax);
    }
    if (m2_calls){
        fprintf(stderr, "  --- durum atama notu (sub_00669296) ---\n");
        fprintf(stderr, "  cagri %llu | girdi ayni %llu (%.1f%%) | isabet %llu (%.1f%%) | atlanan %llu\n"
                        "  kayit gecersiz %llu | cihaz degismis %llu | tampon tasti %llu | tur disi %llu\n",
            (unsigned long long)m2_calls, (unsigned long long)m2_same, 100.0*m2_same/m2_calls,
            (unsigned long long)m2_hit, 100.0*m2_hit/m2_calls, (unsigned long long)m2_skip,
            (unsigned long long)m2_norec, (unsigned long long)m2_devchg, (unsigned long long)m2_big,
            (unsigned long long)m2_excl);
        fprintf(stderr, "  tur: cagri/isabet%%");
        for (int i = 0; i < 32; i++) if (m2_type_calls[i])
            fprintf(stderr, "  %d:%llu/%.0f%%", i, (unsigned long long)m2_type_calls[i],
                    100.0*m2_type_hit[i]/m2_type_calls[i]);
        fprintf(stderr, "\n");
        if (v2_runs){
            fprintf(stderr, "  DOGRULAMA2: %llu kosu, temiz %llu | bellek %llu, cihaz %llu, eax %llu\n",
                (unsigned long long)v2_runs, (unsigned long long)v2_clean, (unsigned long long)v2_mem,
                (unsigned long long)v2_dev, (unsigned long long)v2_eax);
            fprintf(stderr, "  hatali turler:");
            for (int i = 0; i < 32; i++) if (v2_type_bad[i]) fprintf(stderr, " %d:%llu", i, (unsigned long long)v2_type_bad[i]);
            fprintf(stderr, "\n");
        }
    }
    if (m_calls){
        fprintf(stderr, "  --- durum degeri notu (sub_00664a2f) ---\n");
        if (m_big) fprintf(stderr, "  girdisi %u bayti asan (notlanmadi): %llu\n", MEMO_BUF, (unsigned long long)m_big);
        fprintf(stderr, "  cagri %llu | uygun %llu | isabet %llu (%.1f%%) | atlanan %llu | koruma iptali %llu\n",
            (unsigned long long)m_calls, (unsigned long long)m_elig, (unsigned long long)m_hit,
            m_elig ? 100.0*m_hit/m_elig : 0.0, (unsigned long long)m_skip, (unsigned long long)m_guard);
        if (v_runs) fprintf(stderr, "  DOGRULAMA: %llu kosu, temiz %llu, FARKLI %llu\n",
            (unsigned long long)v_runs, (unsigned long long)v_clean, (unsigned long long)v_dirty);
    }
    if (!dx_on()) return;
    long fr = metal_frames() - (g_dx_f0 < 0 ? 0 : g_dx_f0); if (fr < 1) fr = 1;
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    fprintf(stderr, "\n=== D3DX efekt (D3DXSTAT) — %ld kare, %ld. kareden ===\n", fr, g_dx_f0);
    fprintf(stderr, "  %-20s %10s %12s %10s %10s\n",
            "fonksiyon", "cagri/kare", "kapsayici", "ms/kare", "us/cagri");
    for (int i = 0; i < DX_N; i++){
        DxStat *s = &g_dx[i];
        if (!s->n) continue;
        double ns = (double)s->ticks * tb.numer / tb.denom;
        fprintf(stderr, "  %-20s %10.1f   sub_%08x %10.3f %10.3f\n", s->name,
                (double)s->n / fr, s->addr, ns / 1e6 / fr, ns / 1e3 / s->n);
    }
    if (an_calls){
        fprintf(stderr, "  --- D3DXANALYZE: ifade degerlendirme girdileri ---\n");
        fprintf(stderr, "  cagri %llu | ilk %llu | girdi AYNI %llu (%.1f%%) | FARKLI %llu (%.1f%%)\n",
            (unsigned long long)an_calls, (unsigned long long)an_first,
            (unsigned long long)an_same, 100.0*an_same/an_calls,
            (unsigned long long)an_diff, 100.0*an_diff/an_calls);
        fprintf(stderr, "  kirli girdisi olmayan %llu (%.1f%%), ort ozetlenen bayt %.0f\n",
            (unsigned long long)an_dirty0, 100.0*an_dirty0/an_calls, (double)an_bytes/an_calls);
        fprintf(stderr, "  kirli girdi sayisi dagilimi:");
        for (int i = 0; i < 9; i++) fprintf(stderr, " %d:%llu", i, (unsigned long long)an_hist_inputs[i]);
        fprintf(stderr, "\n");
    }
}

