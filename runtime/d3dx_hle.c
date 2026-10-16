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
DX_WRAP(DX_BEGIN,          00664792)
DX_WRAP(DX_COMMIT,         0066ac27)
DX_WRAP(DX_ENDPASS,        0065addb)
DX_WRAP(DX_END,            0065acf5)
DX_WRAP(DX_SETMATRIX,      0066d13a)
DX_WRAP(DX_SETVECTOR,      0066d303)
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

/* Yerel 66056c dogrulamasi: orijinalin dogrudan yaptigi 65ab24 cagrilari
 * (efekt, yuva) sirasiyla kaydedilir; ic ice cagrilar kaydedilmez. */
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

/* ---- Yerel yardimcilar (Secenek 1) ----
 * Lift edilmis x87/bayrak kodunun yerine birebir ayni sonucu ureten C.
 *   D3DXNATIVE=0     : kapat (varsayilan acik)
 *   D3DXNVERIFY=<n>  : her n. cagrida yerel sonucu hesapla, orijinali
 *                      calistir, karsilastir (matris: 64 bayt + eax;
 *                      havuz: 65ab24 cagri dizisi + eax).               */
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
 * Orijinal (x87): her cikti icin p_k = a_ik*b_kj (double, iki float'in
 * carpimi tam), t = (p2+p0) + (p1+p3), out = (float)t.  Satir yolu (m2 !=
 * out) m1 satirini, sutun yolu (m2 == out, m1 != out) m2 sutununu yazmadan
 * once okudugu icin sonuc daima ozgun girdilerin carpimi; m1 == m2 == out
 * durumunda m2 yigina kopyalanip satir yoluna gidiliyor.  Iki yolun tek
 * farki carpimdaki islenen sirasi -- yalnizca NaN yayiliminda onemli.
 * Hizli yol: 32 girdinin hepsi sonluysa carpim/toplam NaN ya da tasma
 * uretemez (|a*b| < 2^256), x87_r2 denetimleri bos gecer.  Aksi halde ve
 * kismi ortusmede orijinal calisir.  FMA birlestirmesi KAPALI olmali.   */
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
                fprintf(stderr, "[D3DXNVERIFY] matris farki out=%08x m1=%08x m2=%08x eax=%08x\n",
                        out, m1, m2, c->eax);
        }
        return;
    }
    memcpy(g_mem + out, R, 64);
    /* orijinalin cikistaki ecx/edx'i (cagiran icin anlamsiz ama ucuz) */
    if (m2 != out){ c->ecx = m1 + 0x40u; c->edx = m2; }
    else if (m1 == out){ c->ecx = m1 + 0x40u; c->edx = sp - 0x44u; }
    else { c->ecx = m1; c->edx = m2; }
    c->eax = out;
    POP32(c->eip); c->esp += 12u;
}
#pragma clang fp contract(on)

/* Parametre degisti bildirimi, sub_0066056c(this=ecx, yuva) ret 4.
 * Parametre efekt havuzunda paylasiliyorsa havuzdaki her etkin efekt
 * (bayt +0x119 bit 0) icin 65ab24(efekt, o efektteki yuva), sonra kendisi.
 *   pobj = [[this+0xb0] + [yuva+0x14]*4],  bas = [pobj+0x40]  (halka liste,
 *   dugum: +0 efekt, +4 yuva, +0x10 sonraki), havuz = [this+0x38]:
 *   +8 efekt sayisi, +0xc tek efekt.
 * Orijinalin prologu (ebp/ebx/esi/edi itmeleri) aynen yapilir: 65ab24
 * ayni esp'yi gorur.  65ab24 yalnizca ecx ve [esp+4]'u okur. */
static inline __attribute__((always_inline)) uint32_t pl_call(CPU *c, uint32_t eff, uint32_t sl, uint32_t ret){
    c->ecx = eff; PUSH32(sl); PUSH32(ret); sub_0065ab24(c); return c->eax;
}
/* Kuru calisma (dogrulama): cagrilarin hepsi >= 0 donerse yapilacak dizi. */
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
            fprintf(stderr, "[D3DXNVERIFY] havuz farki this=%08x yuva=%08x plan %d cagri, orijinal %d (eax %08x)\n",
                    th, sl, pn, g_ab_n, c->eax);
        return;
    }
    uint32_t sebp = c->ebp, sebx = c->ebx, sesi = c->esi, sedi = c->edi, r;
    PUSH32(sebp); uint32_t fp = c->esp; PUSH32(sebx); PUSH32(sesi); PUSH32(sedi);
    c->ebp = fp; c->ebx = th; c->edx = sl;
    uint32_t pobj = MR32(MR32(th+0xb0u) + MR32(sl+0x14u)*4u), head = MR32(pobj+0x40u);
    c->edi = head;
    if (!head){ r = pl_call(c, th, sl, 0x6605feu); goto done; }
    {
    uint32_t pool = MR32(th+0x38u), cnt = MR32(pool+8u);
    if (cnt <= 1u){
        if (!cnt){ r = 0; goto done; }
        uint32_t e = MR32(pool+0xcu);
        if (e == th){ r = pl_call(c, th, sl, 0x6605feu); goto done; }
        if (e) for (uint32_t q = MR32(head+0x10u); q != head; q = MR32(q+0x10u))
            if (MR32(q) == e){ r = pl_call(c, e, MR32(q+4u), 0x6605feu); goto done; }
    }
    for (uint32_t q = MR32(head+0x10u); q != head; q = MR32(q+0x10u)){
        uint32_t ef = MR32(q);
        if (ef && (MR8(ef+0x119u) & 1u)){
            c->esi = q;
            r = pl_call(c, ef, MR32(q+4u), 0x6605ddu);
            if ((int32_t)r < 0) goto done;
        }
    }
    if (!(MR8(th+0x119u) & 1u)){ r = 0; goto done; }
    r = pl_call(c, th, MR32(fp+8u), 0x6605feu);
    }
done:
    c->eax = r;
    c->edi = sedi; c->esi = sesi; c->ebx = sebx; c->ebp = sebp; c->esp = fp + 4u;
    POP32(c->eip); c->esp += 4u;
}

/* SetMatrix'in donusturme adimi, sub_0065cf44(tanim, hedef, kaynak) ret 12.
 * 4x4 matrisi DEVRIK yazar, parametre turune gore ([tanim+0]):
 *   3 float : hedef[4j+i] = kaynak[4i+j] (bit kopyasi)
 *   1 bool  : hedef[4j+i] = (kaynak[4i+j] == [0x7d7d3c]) ? 0 : 1   (NaN -> 1)
 *   2 int   : __ftol (0x75dd28) ile -- orijinalde kalir
 *   diger   : 0x80004005; orijinalde kalir.
 * Basari eax = 0.  Hedef/kaynak ortusurse orijinal (yerinde devrik farkli). */
static uint64_t sm_calls, sm_slow, smv_runs, smv_bad;
void sub_0065cf44_orig(CPU *c);
void sub_0065cf44(CPU *c){
    if (!dxn_on()){ sub_0065cf44_orig(c); return; }
    uint32_t sp = c->esp, t = MR32(MR32(sp+4u)), dst = MR32(sp+8u), src = MR32(sp+12u);
    sm_calls++;
    if ((t != 3u && t != 1u) || dst - src + 63u < 127u){ sm_slow++; sub_0065cf44_orig(c); return; }
    uint32_t S[16], D[16];
    memcpy(S, g_mem + src, 64);
    double z = (double)MRF32(0x7d7d3cu), last = 0;
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
            fprintf(stderr, "[D3DXNVERIFY] SetMatrix donusumu farki tur %u hedef=%08x kaynak=%08x eax=%08x\n",
                    t, dst, src, c->eax);
        return;
    }
    memcpy(g_mem + dst, D, 64);
    if (t == 3u){ c->ecx = src + 0x30u; }
    else { c->ecx = dst + 0x4cu; fcompare(c, z, last); }
    c->edx = 0; c->eax = 0;
    POP32(c->eip); c->esp += 12u;
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

/* ---- Asama 4a: yerel durum uygulayici, sub_00669296 turleri 1/2/3 ----
 * Lift edilmis kodun birebir karsiligi, D3DX'in kendi bellek yapilari
 * uzerinde (defter tutma degismez, desteklenmeyen her yol orijinale duser):
 *   P = a*0x64 + [this+0x2c], R = b*0x64 + [P+4], deger = [[R+0x48] + [P]]
 *   el == 0xffff ise el = word[R+0x54]
 *   R+0 != 0 (kirli): R+0 = R+4; 664a2f(this, a, b) degeri yeniler (<0: don)
 *   tur = [R+0x58]>>24, durum = [R+0x58]&0xffffff, cihaz = [this+8]:
 *     1 SetRenderState(durum, deger)            vtable +0xe4
 *     2 SetTextureStageState(el, durum, deger)  +0x10c
 *     3 [this+0x3c]==1 ise hicbir sey; degilse SetSamplerState(el, durum, deger) +0x114
 *   Tur 3 kirli + [this+0x3c]==4 + el==0xffff: sampler'i kullanan her
 *   shader icin dongu -> orijinal.  Cihaz cagrisi etkisizse (com.c dxn_skip)
 *   hic yapilmaz; degilse konuk dagitimi uzerinden com_call'a gider.
 *   D3DXNATIVE4=0     : kapat
 *   D3DXN4VERIFY=<n>  : uygun cagrilarin 1/n'inde yerel kuru calisir, bellek
 *                       + cagri listesi saklanir, orijinal kayitla calisir,
 *                       karsilastirilir. */
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
 *       (a0,a1,a2) 4 SetTexture(a0,a1) 5/6 Set{V,P}SConstantF(baslangic, veri,
 *       adet) 7/8 Set{Vertex,Pixel}Shader(a0) 9/10 ...ConstantI 11/12 ...ConstantB
 *       (9..12 etkisizlik denetimi yok: her zaman yapilir) */
static const int n4_nargs[13] = { 0, 2, 3, 3, 2, 3, 3, 1, 1, 3, 3, 3, 3 };
/* Tur 1..3 icin com.c dxrec_call_redundant'in satir ici esi (ayni kural);
 * kayit/tekrar/iz acikken -1: yavas yol (dxn_skip) karar verir. */
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
/* Kirli ifade durumu icin durum basina not (Asama 1'in mantigi, R ile
 * dogrudan indeksli kucuk yuvalar): girdi parametrelerinin verisi, hedef
 * dword ve deger tamponunun ilk dword'u son gercek 664a2f'den beri
 * degismediyse degerlendirme ayni sonucu yazardi -> atla.  En fazla 2
 * girdi / 64 bayt; daha buyukler her zaman degerlendirilir. */
typedef struct { uint32_t R, th, dst, dval, vb, vval, eax, n, base; uint32_t rec[2], pp[2], dp[2], nb[2]; uint8_t d[96]; } N4Memo;
#define N4M_CAP 2048u
static N4Memo *g_n4m;
static int g_n4m_on = -1;
static uint64_t n4m_hit, n4m_miss, n4m_big, n4m_why[4];
void sub_00664a2f_orig(CPU *c);
/* 1: girdiler toplandi (rec, pp, dp, nb doldu); 0: notlanamaz */
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
        /* isabet: ayni durum, girdi kayitlari ayni parametreyi gosteriyor,
         * veri ve hedef/deger tamponu son gercek degerlendirmedeki gibi */
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
/* ---- Asama 4c: yerel sampler uygulayici, sub_00668cd0(this, P, Rbas, Rson) ret 12 ----
 * [Rbas, Rson) arasindaki her sampler baglama durumu R icin:
 *   vp = [R+0x48] + [P]; kayit = w[vp+2] + w[vp] - 1, ardisik w[R+0x56] kayit,
 *   asama = w[R+0x54] (kayit basina +1).  Her kaydin (P2) durumlari:
 *     tur 3: [R2] kirliyse 664a2f(this, kayit no, k), sonra [R2] = [R2+4];
 *            SetSamplerState(asama, [R2+0x58]&0xffffff, [[R2+0x48]+[P2]])
 *     tur 9: o = [[this+0xb0] + [R2+0x1c]*4], i = [[R2+8]],
 *            [o+0x40] varsa i = [[o+0x40]+8][i]; SetTexture(asama, [[this+0x18] + i*24 + 4])
 *   <0 sonucta dur ve don; sonunda 0.  Baska tur iceren kayit -> orijinal. */
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
                        r = n4_eval(c, th, rec, k, R2, P2, 0x668d51u);
                        if ((int32_t)r < 0){ *res = r; return 1; }
                        MR32(R2) = MR32(R2+4u);
                    }
                    r = n4_dev(c, dev, 3, 0x114u, 0x668d7du, stage, MR32(R2+0x58u) & 0xffffffu,
                               MR32(MR32(R2+0x48u) + MR32(P2)));
                } else {
                    uint32_t o = MR32(MR32(th+0xb0u) + MR32(R2+0x1cu)*4u), o40 = MR32(o+0x40u);
                    uint32_t ix = MR32(MR32(R2+8u));
                    if (o40) ix = MR32(MR32(o40+8u) + ix*4u);
                    r = n4_dev(c, dev, 4, 0x104u, 0x668dc4u, stage, MR32(MR32(th+0x18u) + ix*24u + 4u), 0);
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

/* ---- Asama 4d: yerel shader uygulayici, sub_00668e0d(this, P) ret 4 ----
 * P = shader kaydi; R0 = [P+4] shader nesnesi durumu, vs = [P+8] bit 1.
 *   R0 kirliyse: 669296(this, kayit, 0, 0xffff) -> orijinal.
 *   Set{Vertex,Pixel}Shader(nesne tablosundan; o+0x40 dolayli indeks)
 *   P&4: 663bc1(this, kayit, 1); P&0x10 ise word[P+0x50/56/5c] adet F/I/B
 *        sabitini (4c==4e / 52==54 / 58==5a ise) yukle; bir durum atla.
 *   float sabit durumlari [.., w[P+0x4e]): kirliyse R=R+4 ve R+0x30&0x10 ise
 *        ham kopya ([R+0x5c], [R+0x60] bayt) yoksa 6623ea; sonra blok yukle
 *        (dongu bossa yukleme de yok).  w[P+0x4e]==w[P+0x36] ise 0 don.
 *   int [.., w[P+0x54]) -> 6627a5 + ConstantI; bool [.., w[P+0x5a]) ->
 *        662bab + ConstantB; kalan [.., w[P+0x60]) sampler'lar -> 668cd0.
 * [ebp-8] yerel yuvasi doldurucu fonksiyonlara adres olarak gidiyor; ayni
 * guncelleme kuraliyla taklit edilir.  Kuru dogrulamada konuk alt cagrisi
 * gerekiyorsa yerel yol reddedilir (orijinal calisir). */
void sub_00663bc1(CPU*); void sub_006623ea(CPU*); void sub_006627a5(CPU*); void sub_00662bab(CPU*);
void sub_00668e0d_orig(CPU *c);
static uint64_t n7_calls;
static VStat n7vs;
static int g_n7_on = -1;
/* doldurucu: fn(this, R+8, hedef, w[R+0x56], [R+0x3c], [[R+0x34]+0x10], &yuva, [o+0x4c]) */
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
    r = vs ? n4_dev(c, dev, 7, 0x170u, 0x668e79u, sh, 0, 0) : n4_dev(c, dev, 8, 0x1acu, 0x668ea9u, sh, 0, 0);
    if ((int32_t)r < 0){ *res = r; return 1; }
    uint32_t R = R0 + 0x64u; slot8 = R;
    if (MR8(P+8u) & 4u){
        uint32_t rec = (uint32_t)((int32_t)(P - base) / 0x64) & 0xffffu;
        c->ecx = th; PUSH32(1u); PUSH32(rec); PUSH32(0x668ed9u); sub_00663bc1(c);
        r = c->eax; if ((int32_t)r < 0){ *res = r; return 1; }
        if (MR8(P+8u) & 0x10u){
            uint32_t n;
            if ((n = MR16(P+0x50u)) && MR16(P+0x4cu) == MR16(P+0x4eu))
                n4_dev(c, dev, vs ? 5 : 6, vs ? 0x178u : 0x1b4u, vs ? 0x668f1eu : 0x668f26u, 0, MR32(P+0x40u)+MR32(P), n);
            if ((n = MR16(P+0x56u)) && MR16(P+0x52u) == MR16(P+0x54u))
                n4_dev(c, dev, vs ? 9 : 10, vs ? 0x180u : 0x1bcu, vs ? 0x668f59u : 0x668f61u, 0, MR32(P+0x44u)+MR32(P), n);
            if ((n = MR16(P+0x5cu)) && MR16(P+0x58u) == MR16(P+0x5au))
                n4_dev(c, dev, vs ? 11 : 12, vs ? 0x188u : 0x1c4u, vs ? 0x668f94u : 0x668f9cu, 0, MR32(P+0x48u)+MR32(P), n);
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
            } else n7_fill(c, sub_006623ea, 0x669017u, th, P, R, &slot8);
        }
        r = n4_dev(c, dev, vs ? 5 : 6, vs ? 0x178u : 0x1b4u, vs ? 0x669045u : 0x66904du,
                   0, MR32(P+0x40u)+MR32(P), MR16(P+0x50u));
        if ((int32_t)r < 0){ *res = r; return 1; }
    }
    if (MR16(P+0x4eu) == MR16(P+0x36u)){ *res = 0; return 1; }
    uint32_t endi = MR16(P+0x54u)*0x64u + MR32(P+4u);
    if (R < endi){
        for (; R < endi; R += 0x64u)
            if (MR32(R)){ MR32(R) = MR32(R+4u); n7_fill(c, sub_006627a5, 0x6690b3u, th, P, R, &slot8); }
        r = n4_dev(c, dev, vs ? 9 : 10, vs ? 0x180u : 0x1bcu, vs ? 0x6690deu : 0x6690e6u,
                   0, MR32(P+0x44u)+MR32(P), MR16(P+0x56u));
        if ((int32_t)r < 0){ *res = r; return 1; }
    }
    uint32_t endb = MR16(P+0x5au)*0x64u + MR32(P+4u);
    if (R < endb){
        for (; R < endb; R += 0x64u)
            if (MR32(R)){ MR32(R) = MR32(R+4u); n7_fill(c, sub_00662bab, 0x66913bu, th, P, R, &slot8); }
        r = n4_dev(c, dev, vs ? 11 : 12, vs ? 0x188u : 0x1c4u, vs ? 0x669166u : 0x66916eu,
                   0, MR32(P+0x48u)+MR32(P), MR16(P+0x5cu));
        if ((int32_t)r < 0){ *res = r; return 1; }
    }
    if (R >= ends){ *res = 0; return 1; }
    if (!n6_samp(c, th, P, R, ends, &r)){
        c->ecx = th; PUSH32(ends); PUSH32(R); PUSH32(P); PUSH32(0x66918eu); sub_00668cd0_orig(c);
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

/* ---- Asama 4e: yerel SetTexture yolu ----
 * sub_0065ccd8(this, doku, i, o40) ret 12: nesne tablosu yuvasi
 *   y = [this+0x18] + (o40 ? [[o40+8] + i*4] : i)*24;  [y+4] == doku ise 0.
 *   doku->AddRef, eski->Release, [y+4] = doku; o40 varsa havuz halkasi
 *   ([o40+0x10] .. o40, dugum: +0 efekt, +8 indeks tablosu) her efektin
 *   yuvasina ayni: AddRef(yeni), Release(eski), yaz.  0 doner.
 * com.c'de doku AddRef/Release'in hicbir yan etkisi yok (1/0 doner):
 *   hedef bir com.c dokusuysa cagri atlanir.
 * sub_0066d7e9 SetTexture(this, h, doku) ret 12: h < 0 ise kayit = ~h
 *   (metin tutamaci -> orijinal); dizi parametresi + eleman yoksa ya da tur
 *   5..9 degilse 0x8876086c; [this+0x3c]==3 -> orijinal (666a78 yolu);
 *   yuva ayniysa 0; degilse 65ccd8 sonra 66056c(this, kayit).
 *   D3DXNATIVE8=0 kapatir. */
void sub_0065ccd8_orig(CPU *c);
extern int g_fxtr; int fxtr_now(void);
static int g_n8_on = -1;
static uint64_t n8_tex, n8_set, n8_refskip;
static inline int n8_init(void){
    if (g_n8_on < 0){ const char *e = getenv("D3DXNATIVE8"); g_n8_on = e ? atoi(e) != 0 : 1; }
    return g_n8_on;
}
static inline void n8_ref(CPU *c, uint32_t obj, uint32_t off, uint32_t ret){
    uint32_t fn = MR32(MR32(obj) + off);
    if ((fn & 0xF0000000u) == 0xE0000000u && ((fn >> 16) & 0x1fffu) == 3u && !(g_fxtr && fxtr_now())){
        n8_refskip++; return; }
    PUSH32(obj); PUSH32(ret); dispatch_indirect(c, fn);
}
static void n8_store(CPU *c, uint32_t th, uint32_t tex, uint32_t ix, uint32_t o40){
    n8_tex++;
    uint32_t y = MR32(th+0x18u) + (o40 ? MR32(MR32(o40+8u) + ix*4u) : ix)*24u;
    if (MR32(y+4u) == tex) return;
    if (tex) n8_ref(c, tex, 4u, 0x65cd10u);
    uint32_t old = MR32(y+4u);
    if (old){ n8_ref(c, old, 8u, 0x65cd1du); MR32(y+4u) = 0; }
    MR32(y+4u) = tex;
    if (!o40) return;
    for (uint32_t nd = MR32(o40+0x10u); nd != o40; nd = MR32(nd+0x10u)){
        uint32_t ef = MR32(nd);
        if (!ef) continue;
        uint32_t y2 = MR32(ef+0x18u) + MR32(MR32(nd+8u) + ix*4u)*24u, nw = MR32(y+4u);
        if (nw) n8_ref(c, nw, 4u, 0x65cd52u);
        uint32_t o2 = MR32(y2+4u);
        if (o2){ n8_ref(c, o2, 8u, 0x65cd5fu); MR32(y2+4u) = 0; }
        MR32(y2+4u) = MR32(y+4u);
    }
}
void sub_0065ccd8(CPU *c){
    if (!n8_init()){ sub_0065ccd8_orig(c); return; }
    uint32_t sp = c->esp, ret = MR32(sp), th = c->ecx;
    PUSH32(c->ebp); PUSH32(c->ebx); PUSH32(c->esi); PUSH32(c->edi);  /* orijinal prolog: alt cagrilar ayni esp'yi gorsun */
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
    PUSH32(c->ebp); PUSH32(c->ebx); PUSH32(c->esi); PUSH32(c->edi);   /* 65ccd8'in prologu */
    n8_store(c, th, tex, ix, o40);
    c->esp += 16u;
    c->ecx = th; PUSH32(rec); PUSH32(0x66d8afu); sub_0066056c(c);
    r = c->eax;
    c->esp += 16u;
done:
    c->eax = r; c->esp = sp + 16u; c->eip = ret;
    }
out:
    if (t0){ g_dx[DX_SETTEXTURE].ticks += mach_absolute_time() - t0; g_dx[DX_SETTEXTURE].n++; }
}

/* Cekirdek: 1 dondururse durum yerelde uygulandi, *res = 669296'nin donus
 * degeri.  Yigina dokunmaz (664a2f ve cihaz cagrilari icin it/cek haric). */
void sub_006691b1(CPU *c); void sub_00668e0d(CPU*);
static uint64_t n4_obj;
static inline int n4_core(CPU *c, uint32_t th, uint32_t a, uint32_t b, uint32_t eld, uint32_t *res){
    uint32_t P = a*0x64u + MR32(th+0x2cu), R = b*0x64u + MR32(P+4u);
    uint32_t code = MR32(R+0x58u), type = code >> 24, el = eld & 0xffffu;
    if (el == 0xffffu) el = MR16(R+0x54u);
    int dirty = MR32(R) != 0;
    if (type == 14u || type == 15u){
        /* nesne atama (14 shader, 15 sampler): kirli + dizi secici -> orijinal */
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
                if (MR8(P2+8u) & 1u){ PUSH32(P2); PUSH32(0x669f73u); sub_00668e0d(c); }
                else { PUSH32(0xffffu); PUSH32(rec); PUSH32(0x669f85u); sub_006691b1(c); }
                *res = 0; return 1;
            }
            c->ecx = th; PUSH32(0xffffu); PUSH32(rec); PUSH32(0x669fa6u); sub_006691b1(c);
            *res = c->eax; return 1;
        }
        if (mode == 4u){
            if (!n6_samp(c, th, P, R, R + 0x64u, &r)){
                c->ecx = th; PUSH32(R + 0x64u); PUSH32(R); PUSH32(P); PUSH32(0x669fe5u); sub_00668cd0_orig(c);
                r = c->eax;
            }
            *res = r; return 1;
        }
        c->ecx = th; PUSH32((eld & 0xffff0000u) | el); PUSH32(rec); PUSH32(0x6693c1u); sub_006691b1(c);
        *res = c->eax; return 1;
    }
    if (type - 1u > 2u) return 0;
    if (dirty && type == 3u && MR32(th+0x3cu) == 4u && el == 0xffffu) return 0;
    n4_calls++;
    uint32_t vp = MR32(R+0x48u) + MR32(P), r = 0;
    if (dirty){
        n4_dirty++;
        MR32(R) = MR32(R+4u);
        r = n4_eval(c, th, a, b, R, P, type == 3u ? 0x669533u : 0x669863u);
        if ((int32_t)r < 0){ *res = r; return 1; }
    }
    uint32_t dev = MR32(th+8u), v = MR32(vp), st = code & 0xffffffu;
    if (type == 1u)      r = n4_dev(c, dev, 1, 0xe4u,  0x66989eu, st, v, 0);
    else if (type == 2u) r = n4_dev(c, dev, 2, 0x10cu, 0x6698bfu, el, st, v);
    else if (MR32(th+0x3cu) != 1u) r = n4_dev(c, dev, 3, 0x114u, 0x6698eau, el, st, v);
    *res = r;
    return 1;
}
static int n4_apply(CPU *c){
    uint32_t th = c->ecx, sp = c->esp, res;
    uint32_t a = MR16(sp+4u), b = MR16(sp+8u), el = MR16(sp+12u);
    if (!n4_core(c, th, a, b, MR32(sp+12u), &res)) return 0;
    if (el == 0xffffu){                                  /* orijinal arguman yuvasina yazar */
        uint32_t P = a*0x64u + MR32(th+0x2cu), R = b*0x64u + MR32(P+4u);
        MR16(sp+12u) = MR16(R+0x54u);
    }
    c->eax = res; POP32(c->eip); c->esp += 12u;
    return 1;
}
/* Genel dogrulama: yerel kuru calisir (n4_dev cihaz cagrilarini KAYDEDER,
 * yapmaz), bellek ve cagri listesi saklanir; bellek geri alinir, orijinal
 * dxrec kaydiyla calisir; bellek penceresi (this +-256 KB, .data, ek
 * kayitlar), cagri dizisi, eax/esp karsilastirilir.  Yalnizca cihaza
 * yalniz n4_dev uzerinden giden yollar dogrulanabilir (konuk alt
 * cagrilari kuru calistirilamaz). */
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
    if (!vr.ok){ vs->norec++; return; }     /* durum blogu kaydi vb.: karsilastirilamaz */
    vs->runs++;
    /* this+0x114..0x11b (kayitli FPU CW + bayrak): 663a42 BeginPass icinde
     * kaydeder, sonunda geri yukler -- Asama 1 dogrulamasindaki gibi haric. */
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
            fprintf(stderr, "[%s] fark (%08x): bellek=%d cagri=%d (yerel %d, orijinal %d) eax=%d (%08x/%08x)\n",
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
/* ---- Asama 4b: yerel gecis dongusu, sub_006691b1(this, a, el) ret 8 ----
 *   P = a*0x64 + [this+0x2c]
 *   [this+0x118]&8 : 66404c(P+0x1c, word[P+0x18]!=0xffff), 6641a7(P+0x24, word[P+0x1a]!=0xffff); <0 don
 *   P&8            : word[P+0x18]==0xffff ? 667d61(a) : 666e07(a); <0 don
 *   [this+0x3c]==4 : P&1 ise 668e0d(P)'nin sonucunu don; degilse her durum
 *                    icin 669296(a, b, el), <0 ise don
 *   diger          : her durum icin 669296; <0 ve [this+0x3c]!=1 ise don
 *   sonunda 0.  Tur 1/2/3 durumlar dogrudan n4_core ile, digerleri konuk
 *   cagrisiyla.  Orijinalin prologu (ebp/ebx/esi/edi) aynen kurulur.
 *   D3DXNATIVE5=0 kapatir.  D3DXN4VERIFY acikken durumlar 669296
 *   sarmalayicisindan gecer (orada dogrulanir). */
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
        c->ecx = th; PUSH32(P+0x1cu); PUSH32(MR16(P+0x18u) != 0xffffu); PUSH32(0x6691e5u);
        sub_0066404c(c); r = c->eax; if ((int32_t)r < 0) goto done;
        c->ecx = th; PUSH32(P+0x24u); PUSH32(MR16(P+0x1au) != 0xffffu); PUSH32(0x669204u);
        sub_006641a7(c); r = c->eax; if ((int32_t)r < 0) goto done;
    }
    if (MR8(P+8u) & 8u){
        c->ecx = th; PUSH32(a);
        if (MR16(P+0x18u) == 0xffffu){ PUSH32(0x669241u); sub_00667d61(c); }
        else { PUSH32(0x669222u); sub_00666e07(c); }
        r = c->eax; if ((int32_t)r < 0) goto done;
    }
    if (MR32(th+0x3cu) == 4u){
        if (MR8(P+8u) & 1u){
            c->ecx = th; PUSH32(P); PUSH32(0x66923au); sub_00668e0d(c);
            r = c->eax; goto done;
        }
        for (uint32_t b = 0; b < MR16(P+0x36u); b++){
            r = n5_state(c, th, aw, b, el, 0x669259u);
            if ((int32_t)r < 0) goto done;
        }
    } else {
        for (uint32_t b = 0; b < MR16(P+0x36u); b++){
            r = n5_state(c, th, aw, b, el, 0x66927cu);
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
/* thread'e ozel komut/cevrim sayaci (libsystem_kernel thread_selfcounts):
 * zamandan cok daha kararli; ceviricinin urettigi kodun verimlilik olcusu */
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

/* ---- EFXSTAT=<kare>: ID3DXEffect vtable (0x7b6fe8, 82 metot) envanteri ----
 * dispatch_indirect'ten gelen her efekt metodu cagrisi: metot basina
 * cagri/kare (oyundan / D3DX icinden), farkli efekt nesnesi, Begin
 * bayraklari, farkli (efekt, gecis) ciftleri.  Yalnizca tani icin.      */
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
/* FXTRACE: efekt metodu isareti (com.c'deki cihaz cagrisi iziyle ayni akista) */
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
    if (!efx_vt[0]) for (int i = 0; i < EFX_NM; i++) efx_vt[i] = MR32(0x7b6fe8u + 4u*i);
    int k = -1;
    for (int i = 0; i < EFX_NM; i++) if (efx_vt[i] == addr){ k = i; break; }
    if (k < 0) return;
    if (g_fxtr && fxtr_now()) efx_trace(c, k);
    if (metal_frames() < efx_from) return;
    if (efx_f0 < 0) efx_f0 = metal_frames();
    uint32_t ret = MR32(c->esp), th = MR32(c->esp+4u);
    efx_n[k][ret - 0x650000u < 0x40000u]++;
    efx_set_add(efx_this, &efx_nthis, 1024, th);
    /* tutamac metin mi?  (yuva 8..64'te ilk arguman D3DXHANDLE) */
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
/* D3DXCreateEffectFromResourceA(cihaz, modul, kaynak, tanimlar, include,
 * bayraklar, havuz, &efekt, &hatalar)  stdcall, 9 arguman. */
void sub_00640c12_orig(CPU *c);
void sub_00640c12(CPU *c){
    uint32_t sp = c->esp, res = MR32(sp+12u), pool = MR32(sp+28u), pout = MR32(sp+32u);
    sub_00640c12_orig(c);
    if (g_efx_stat){
        char nm[64] = "#";
        if (res >= 0x10000u){ int i = 0; for (; i < 63 && MR8(res+i); i++) nm[i] = (char)MR8(res+i); nm[i] = 0; }
        else snprintf(nm, sizeof nm, "#%u", res);
        uint32_t ef = pout ? MR32(pout) : 0u;
        fprintf(stderr, "[EFXSTAT] olustur %-28s havuz=%08x efekt=%08x sonuc=%08x kare=%ld"
                " param %u@%08x teknik %u@%08x nesne@%08x\n",
                nm, pool, ef, c->eax, metal_frames(), ef ? MR32(ef+0x98u) : 0u, ef ? MR32(ef+0x9cu) : 0u,
                ef ? MR32(ef+0xa0u) : 0u, ef ? MR32(ef+0xa4u) : 0u, ef ? MR32(ef+0x18u) : 0u);
    }
}

static void efx_report(void){
    if (!g_efx_stat || efx_f0 < 0) return;
    long fr = metal_frames() - efx_f0; if (fr < 1) fr = 1;
    fprintf(stderr, "\n=== EFXSTAT: ID3DXEffect cagrilari, %ld kare (%ld. kareden) ===\n", fr, efx_f0);
    fprintf(stderr, "  yuva  adres      oyun/kare  D3DX-ici/kare   toplam   metin-tutamac\n");
    for (int i = 0; i < EFX_NM; i++) if (efx_n[i][0] || efx_n[i][1])
        fprintf(stderr, "  %3d   %08x  %10.1f  %10.1f  %8llu  %8llu\n", i, efx_vt[i],
                (double)efx_n[i][0]/fr, (double)efx_n[i][1]/fr,
                (unsigned long long)(efx_n[i][0] + efx_n[i][1]), (unsigned long long)efx_hstr[i]);
    fprintf(stderr, "  farkli efekt nesnesi: %d | farkli (efekt, gecis): %d\n", efx_nthis, efx_npair);
    fprintf(stderr, "  Begin bayraklari:");
    for (int j = 0; j < efx_nbf; j++) fprintf(stderr, " 0x%x x%u", efx_bflag[j], efx_bflag_n[j]);
    fprintf(stderr, "\n");
}

void d3dx_hle_report(void){
    efx_report();
    { extern void comstat_report(void); comstat_report(); }
    if (cw_f1 > cw_f0 && cw_f0 >= 0)
        fprintf(stderr, "\n=== CPUWIN: konuk thread CPU %.3f ms/kare (%ld..%ld, %ld kare) ===\n",
                (cw_t1 - cw_t0) / (double)(cw_f1 - cw_f0), cw_f0, cw_f1, cw_f1 - cw_f0);
        if (cw_i1[0] > cw_i0[0]){
            double fr = (double)(cw_f1 - cw_f0), ki = (double)(cw_i1[0] - cw_i0[0]), kc = (double)(cw_i1[1] - cw_i0[1]);
            fprintf(stderr, "=== CPUWIN: konuk thread %.3f M komut/kare, %.3f M cevrim/kare, IPC %.2f ===\n",
                    ki / fr / 1e6, kc / fr / 1e6, ki / kc);
        }
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
    if (n4_calls || n4vs.runs){
        fprintf(stderr, "  --- yerel durum uygulayici (669296 tur 1/2/3, Asama 4a) ---\n"
                        "  cagri %llu | kirli (664a2f) %llu | cihaz cagrisi yapilan %llu, atlanan (etkisiz) %llu\n",
            (unsigned long long)n4_calls, (unsigned long long)n4_dirty,
            (unsigned long long)n4_issued, (unsigned long long)n4_skipped);
        fprintf(stderr, "  yerel gecis dongusu (6691b1, Asama 4b): %llu\n", (unsigned long long)n5_calls);
        fprintf(stderr, "  ifade notu: isabet %llu, kacan %llu, notlanamaz %llu (ifade degil %llu, girdisiz %llu, girdi>2 %llu, >96 bayt %llu)\n",
            (unsigned long long)n4m_hit, (unsigned long long)n4m_miss, (unsigned long long)n4m_big,
            (unsigned long long)n4m_why[0], (unsigned long long)n4m_why[1], (unsigned long long)n4m_why[2], (unsigned long long)n4m_why[3]);
        if (n4vs.runs) fprintf(stderr, "  DOGRULAMA-4: %llu kosu, FARKLI %llu (kayit gecersiz, karsilastirilmadi: %llu)\n",
            (unsigned long long)n4vs.runs, (unsigned long long)n4vs.bad, (unsigned long long)n4vs.norec);
        if (n8_set || n8_tex) fprintf(stderr, "  yerel SetTexture: %llu | doku yuvasi (65ccd8): %llu | atlanan doku AddRef/Release: %llu\n",
            (unsigned long long)n8_set, (unsigned long long)n8_tex, (unsigned long long)n8_refskip);
        if (n7vs.runs || n7_calls) fprintf(stderr, "  yerel shader uygulayici (668e0d): %llu | DOGRULAMA-7: %llu kosu, FARKLI %llu (karsilastirilmadi %llu) | nesne durumu (tur 14/15): %llu\n",
            (unsigned long long)n7_calls, (unsigned long long)n7vs.runs, (unsigned long long)n7vs.bad, (unsigned long long)n7vs.norec, (unsigned long long)n4_obj);
        if (n6vs.runs || n6_calls) fprintf(stderr, "  yerel sampler uygulayici (668cd0): %llu | DOGRULAMA-6: %llu kosu, FARKLI %llu (karsilastirilmadi %llu)\n",
            (unsigned long long)n6_calls, (unsigned long long)n6vs.runs, (unsigned long long)n6vs.bad, (unsigned long long)n6vs.norec);
    }
    if (mm_calls || pl_calls || sm_calls){
        fprintf(stderr, "  --- yerel yardimcilar ---\n  matris carpimi %llu (yavas yol %llu) | havuz bildirimi %llu"
                        " | SetMatrix donusumu %llu (yavas yol %llu)\n",
            (unsigned long long)mm_calls, (unsigned long long)mm_slow, (unsigned long long)pl_calls,
            (unsigned long long)sm_calls, (unsigned long long)sm_slow);
        if (mmv_runs || plv_runs || smv_runs)
            fprintf(stderr, "  DOGRULAMA-N: matris %llu kosu, FARKLI %llu | havuz %llu kosu, FARKLI %llu"
                            " | donusum %llu kosu, FARKLI %llu\n",
                (unsigned long long)mmv_runs, (unsigned long long)mmv_bad,
                (unsigned long long)plv_runs, (unsigned long long)plv_bad,
                (unsigned long long)smv_runs, (unsigned long long)smv_bad);
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

