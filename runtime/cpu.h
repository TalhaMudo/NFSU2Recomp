/* NFSU2 native port - x86-32 CPU durumu ve guest bellek erisimi (macOS ARM64) */
#ifndef NFSU2_CPU_H
#define NFSU2_CPU_H
#include <math.h>
#include <stdint.h>
#include <string.h>

/* Guest bellek tabani. Guest 32-bit adres -> host adres tek ARM64 komutunda:
 *   ldr w0, [x_base, w_addr, uxtw]        (sifir ek maliyet)             */
extern uint8_t *g_mem;
/* Tum 32-bit guest adres alani PROT_NONE olarak rezerve edilir; kullanilan
 * bolgeler mem_commit ile RW yapilir. Boylece hatali guest erisimi host'u
 * cokertmek yerine teshis edilebilir bir sinyale donusur.                */
#define GSZ 0x100000000ull

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
    uint32_t fs_base, gs_base;   /* TEB / TLS tabani (duz model) */
    /* bayraklar - her biri 0/1 olarak tutulur (clang DCE kullanilmayanlari eler) */
    uint32_t cf,pf,af,zf,sf,of,df;
    /* x87 yigini */
    double   st[8];
    int32_t  top;
    uint16_t fcw, fsw;
    /* MMX (x87 ile ortak degil, ayri tutuluyor) */
    uint64_t mm[8];
    /* SSE */
    union { uint8_t b[16]; float f[4]; double d[2]; uint32_t u32[4]; uint64_t u64[2]; } xmm[8];
} CPU;

/* ---- bayrak yardimcilari ---------------------------------------------- */
static inline uint32_t parity8(uint32_t v){ v&=0xff; v^=v>>4; v^=v>>2; v^=v>>1; return (~v)&1; }

/* ---- bayrak maskeleri --------------------------------------------------
 * Her aritmetik komut alti bayragi PESINEN hesapliyordu (ZF/SF/PF/CF/OF/AF),
 * ama sonraki dal genelde yalnizca birini okuyor.  Olculdu: uretilen kodda
 * 110k FLAGS_ADD + 130k FLAGS_SUB + 130k FLAGS_LOG var, buna karsilik OF
 * dogrudan yalnizca 22 yerde (CC_O/CC_NO), PF 5.7k yerde okunuyor.
 * Ureteç (tools/lift.py) artik akis grafigi uzerinde bayrak CANLILIGI
 * cozup her cagriya "sonradan okunan bayraklar" maskesini veriyor.  Maske
 * derleme zamani sabiti oldugu icin kullanilmayan dallar tamamen silinir. */
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
/* ADC/SBB: tasima girisi 1 iken sonuc girdiye ESIT olabilir; bu durumda
 * (res<a) testi tasimayi kaciriyordu (ornek: sbb eax,eax  CF=1 -> CF=1). */
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
/* mantiksal: CF=OF=0 */
#define FLAGS_LOG_M(res,bits,M) do{ SETZSP_M(res,bits,M); \
    if((M)&F_CF) c->cf=0; if((M)&F_OF) c->of=0; if((M)&F_AF) c->af=0; }while(0)
#define FLAGS_LOG(res,bits) FLAGS_LOG_M(res,bits,F_ALL)

/* ---- yigin ------------------------------------------------------------ */
/* KRITIK: operand esp'ye bagli olabilir (ornek: push [esp+8], push esp).
 * x86 semantigi: PUSH once degeri okur, sonra esp'yi azaltir.            */
#define PUSH32(v) do{ uint32_t _pv = (uint32_t)(v); c->esp -= 4; MR32(c->esp) = _pv; }while(0)
/* POP: once esp artar, hedef adresi YENI esp ile hesaplanir (pop [esp+N]). */
#define POP32(d)  do{ uint32_t _qv = MR32(c->esp); c->esp += 4; (d) = _qv; }while(0)

/* ---- kosullar --------------------------------------------------------- */
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


/* ---- x87 FPU yigini ---------------------------------------------------
 * NOT: ARM64'te 80-bit extended float yok; double (64-bit) kullaniyoruz.
 * Bu, x86'nin 80-bit ara hassasiyetine gore kucuk sapma yaratabilir.     */
#define ST(n)    (c->st[(c->top + (n)) & 7])
/* DIKKAT: kaynak deger top DEGISMEDEN once hesaplanmali.  ST(i) top'a
 * gore tanimli oldugu icin "fld st(0)" onceki FPUSH'ta yeni acilan bos
 * yuvayi okuyordu -> kendini kopyalamak yerine cop yukluyordu.  Bu, CRT'nin
 * __ftol rutinini (fld st(0) ile basliyor) bozuyor, yani oyundaki HER
 * float->int donusumunu.  Belirti: kare-farki olcumu hep 1 birim cikiyordu. */
#define FPUSH(v) do{ double _fpv = (double)(v); \
    c->top = (c->top - 1) & 7; c->st[c->top] = _fpv; }while(0)
#define FPOP()   do{ c->top = (c->top + 1) & 7; }while(0)

/* x87 karsilastirma -> durum kelimesi C0/C2/C3 bitleri (8/10/14) */
#define FSW_C0 (1u<<8)
#define FSW_C1 (1u<<9)
#define FSW_C2 (1u<<10)
#define FSW_C3 (1u<<14)
/* fnstsw: gercek donanimda TOP alani (bit 11-13) de okunur. */
static inline uint16_t fsw_get(const struct CPU *c){
    return (uint16_t)((c->fsw & (uint16_t)~0x3800u) | (uint16_t)((c->top & 7u) << 11));
}
/* X87OPT: blok ici x87 yazmac tahsisinde c->top bolge sonunda yazilir;
 * fnstsw o andaki (statik) top'u acikca alir. */
static inline uint16_t fsw_get_top(const struct CPU *c, uint32_t top){
    return (uint16_t)((c->fsw & (uint16_t)~0x3800u) | (uint16_t)((top & 7u) << 11));
}
static inline void fcompare(struct CPU *c, double a, double b){
    c->fsw &= (uint16_t)~(FSW_C0|FSW_C2|FSW_C3);
    if (a != a || b != b)      c->fsw |= FSW_C0|FSW_C2|FSW_C3;  /* sirasiz (NaN) */
    else if (a > b)            ;                                 /* hepsi 0 */
    else if (a < b)            c->fsw |= FSW_C0;
    else                       c->fsw |= FSW_C3;
}
/* fcomi / fucomi: dogrudan EFLAGS yazar */
#define FCOMI(a,b) do{ double _x=(a),_y=(b); \
    if(_x!=_x||_y!=_y){ c->zf=1;c->pf=1;c->cf=1; } \
    else { c->pf=0; c->zf=(_x==_y); c->cf=(_x<_y); } c->of=c->sf=c->af=0; }while(0)

/* ---- EFLAGS birlestirme/ayirma (pushfd/popfd/sahf) -------------------- */
static inline uint32_t eflags_get(const struct CPU *c){
    return 0x00000202u | (c->cf&1u) | ((c->pf&1u)<<2) | ((c->af&1u)<<4)
         | ((c->zf&1u)<<6) | ((c->sf&1u)<<7) | ((c->df&1u)<<10) | ((c->of&1u)<<11);
}
static inline void eflags_set(struct CPU *c, uint32_t f){
    c->cf=f&1u; c->pf=(f>>2)&1u; c->af=(f>>4)&1u;
    c->zf=(f>>6)&1u; c->sf=(f>>7)&1u; c->df=(f>>10)&1u; c->of=(f>>11)&1u;
}

/* ---- string komutlari ------------------------------------------------- */
#define DFSTEP(n) (c->df ? -(int32_t)(n) : (int32_t)(n))

/* ---- watchpoint: guest bellek adresini izle -------------------------- */
extern uint32_t g_watch_addr, g_watch_val, g_watch_prev_ip;
void watch_report_change(uint32_t culprit_ip, uint32_t oldv, uint32_t newv);

/* Komut seviyesinde yurutme izi: son 64K guest komut adresi.
 * Cokme/hata aninda "buraya nasil geldik" sorusunu kesin cevaplar.        */
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

/* ---- fonksiyon kapsama olcumu (COVERAGE=1 ile uretilirse) ------------
 * Amac: "hangi kod HIC calismiyor?" sorusunu cevaplamak.  Belirti
 * kovalamak yerine calismayan alt sistemi dogrudan gostermek icin.     */
extern uint32_t *g_cov;   /* fonksiyon basina CAGRI SAYISI */
extern uint32_t g_last_fn;          /* ornekleyici icin: en son girilen fonksiyon */
extern int      g_prof_on;          /* SAMPLE / COVERAGE aciksa 1 */
/* DIKKAT: burada g_last_fn KOSULSUZ yaziliyordu.  Bu, HER guest fonksiyon
 * girisinde bir global magaza demek (saniyede milyonlarca).  Yalnizca
 * olcum acikken yaz. */
static inline void cov_hit(uint32_t va){
    if (__builtin_expect(g_prof_on, 0)){
        g_last_fn = va;
        if (g_cov && va >= 0x00401000u && va < 0x00401000u + (3676913u))
            g_cov[va - 0x00401000u]++;
    }
}
void cov_report(const char *path);

/* x87 yuvarlama: sonuc, kontrol kelimesinin RC alanina (bit 10-11) uyar.
 * 00=en yakina (cift), 01=asagi, 10=yukari, 11=sifira dogru (kesme).
 * CRT'nin __ftol rutini RC'yi 11 yapip fistp cagirir; bunu dikkate almazsak
 * her float->int donusumu kesir >= 0.5 oldugunda bir fazla cikar.        */
static inline double x87_round(uint16_t fcw, double v){
    switch ((fcw >> 10) & 3u){
    case 0:  return nearbyint(v);      /* host modu: en yakina, cift */
    case 1:  return floor(v);
    case 2:  return ceil(v);
    default: return trunc(v);
    }
}

/* ---- gecersiz islem sonuclari ve aralik disi donusumler --------------
 * x86'da 0/0, sqrt(-x) gibi gecersiz islemler "real indefinite" uretir:
 * isareti NEGATIF olan sessiz NaN.  C'nin sqrt/0.0 sonucu pozitif NaN
 * oldugu icin ayrica duzeltmek gerekir.  Girdilerden biri NaN ise o
 * yayilir.                                                              */
#define X87_INDEF (-__builtin_nan(""))
static inline double x87_r2(double a, double b, double r){
    if (r == r) return r;
    if (a != a) return a;
    if (b != b) return b;
    return X87_INDEF;
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
static inline double sse_r2d(double a, double b, double r){ return x87_r2(a,b,r); }
static inline double sse_r1d(double a, double r){ return x87_r1(a,r); }

/* Aralik disi / NaN -> "integer indefinite" (en negatif deger). */
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

/* ---- 80-bit uzatilmis kayan nokta (x87 tbyte) <-> double ------------- */
static inline void f80_store(uint8_t *p, double v){
    /* isaret, 15-bit us, 64-bit acik mantis */
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
extern uint8_t g_cbits[1u<<15];   /* commit edilen 16 KB sayfalarin bitmap'i */
static inline int mem_is_committed(uint32_t addr){
    uint32_t pg = (addr) >> 14;
    return (g_cbits[pg>>3] >> (pg&7)) & 1;
}

#endif
