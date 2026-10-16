/* Asama 3 tohumu: boot icin gerekli Win32 API alt kumesi + kalanlar icin izleme. */
#include <stdatomic.h>
#include "enhance.h"
#include "cpu.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <setjmp.h>

/* getenv() her cagrida ortam dizisini TARAR ve kilit alir.  Cizim basina
 * calisan yollarda (ornegin SetVertexShaderConstantF icindeki "PDLIST"
 * kontrolu &&-zincirinin ILK terimiydi) olculebilir maliyeti var:
 * profilde getenv+__findenv_locked konuk thread'in %3'unu yiyordu.
 * Ortam degiskenleri kosu boyunca degismez -> bir kez coz, sakla. */
#define ENVC(nm_) ({ static const char *_ev_; static int _ei_; \
                     if (!_ei_){ _ei_ = 1; _ev_ = getenv(nm_); } _ev_; })

jmp_buf g_exit_jmp;
int g_exit_code = 0;
int g_exit_armed = 0;

typedef struct { uint32_t iat; const char*dll; const char*name; int nargs; } ImpEnt;
extern const ImpEnt g_imp[]; extern const uint32_t g_imp_count;
void mem_commit(uint32_t,uint32_t);
void guest_backtrace(CPU*,int);
int win32_ext_call(CPU*, const char*, uint32_t*);
void guest_fair_yield(void);
int fileio_call(CPU*, const char*, uint32_t*);
void fileio_report(void);
uint64_t win32_ext_threads(void);
uint64_t win32_ext_threads_exited(void);
uint32_t com_direct3dcreate9(void);
uint32_t com_directinput8create(uint32_t);
uint32_t com_directsoundcreate(uint32_t);
void com_init(void);
int g_bt_on = 0;

#define IMP_MAGIC 0xF0000000u

/* --- guest heap: baslikli bump ayirici (HeapSize/ReAlloc icin boyut saklar) --- */
#define HEAP_BASE 0x10000000u
#define HEAP_MAX  0x2F000000u
#define HDR 16u
/* ---- HIBRIT sanal saat -------------------------------------------------
 * Denenenler:
 *   (a) cagri basina sabit artis -> zaman duzensiz, animasyonlar bozuk
 *   (b) gercek monotonik saat    -> emulator yavas oldugu icin oyun her
 *       karede devasa "gecen sure" gorup telafi dongusune giriyor (20->1 fps)
 *   (c) yalnizca kare basina 16.67 ms -> kare temposu dogru AMA kare
 *       sunmadan zamani bekleyen kod (video oynatici) SONSUZ DONGUYE
 *       giriyor: olcum, timeGetTime 231 milyon kez cagrildi.
 *   (d) SECILEN: kare basina 16.67 ms + saat cok kez sorgulanip hic kare
 *       gelmiyorsa yavasca ilerlet.  Boylece hem tempo dogru kalir hem de
 *       zaman bekleyen dongulerin ilerlemesi garanti olur.             */
static _Atomic uint64_t g_vclock_base  = 0;   /* kare basindaki sanal zaman */
static _Atomic uint64_t g_vclock_creep = 0;   /* mesgul-bekleme telafisi    */
static _Atomic uint64_t g_real_base    = 0;   /* o karenin gercek zamani    */
static _Atomic uint32_t g_vclock_polls = 0;

static uint64_t real_us(void){
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

/* Sanal saat.
 * Kareler ARASINDA tam 16.667 ms ilerler: animasyon hizi emulatorun
 * hizindan bagimsiz kalir.  Kare ICINDE ise gercek zamanla orantili,
 * duzgun akar -- cunku oyun kare suresini saati kare icinde IKI KEZ
 * ornekleyip farkini alarak olcuyor (0x005812e5).  Tek hamlede atlayan
 * eski saat ona "kare sifir surdu" dedirtiyor, bu yuzden on yuz tiki
 * [0x8651a8] hic artmiyor ve tum animasyonlar/zamanli gecisler donuyordu. */
/* Sanal zamanin gercek zamana oranini olc: saat degisiklikleri oyunu
 * hizlandirmasin (daha once 2.01 kati kosmustu). */
static _Atomic uint64_t g_vt_last = 0, g_rt_start = 0, g_vt_start = 0;
void vclock_report(void){
    uint64_t v0 = atomic_load(&g_vt_start), r0 = atomic_load(&g_rt_start);
    uint64_t v1 = atomic_load(&g_vt_last),  r1 = real_us();
    if (!r0 || r1 <= r0) return;
    fprintf(stderr, "  sanal saat        : %.3f x gercek zaman (%.1f sn sanal / %.1f sn gercek)\n",
            (double)(v1 - v0) / (double)(r1 - r0),
            (double)(v1 - v0)/1e6, (double)(r1 - r0)/1e6);
}
void vclock_tick(void){
    uint64_t cr = atomic_exchange(&g_vclock_creep, 0ull);
    uint64_t now = real_us();
    /* Kare basina ilerleme.
     *   VCLOCK=fixed : her kare tam 16.667 ms (eski davranis).  Emulator
     *                  60 fps'in altina dusunce OYUN AGIR CEKIM olur --
     *                  ve muzik de oyunun saatinden calindigi icin
     *                  perdesi duser.  Kullanicinin duydugu "fps dusunce
     *                  muzik yavasliyor" tam olarak buydu (olculdu: 50 fps'te
     *                  sanal/gercek = 0.885, yani ~2 yarim ses pes).
     *   VCLOCK=real  : GERCEK gecen sure (varsayilan).  Oyun da muzik de
     *                  gercek zamanda akar.  Telafi dongusune girmemesi
     *                  icin kare basina artis 50 ms ile sinirli. */
    { static _Atomic int mode = -1;
      int m = atomic_load(&mode);
      if (m < 0){ const char *e = ENVC("VCLOCK");
                  m = (e && *e == 'f') ? 0 : 1; atomic_store(&mode, m); }
      uint64_t rb = atomic_load(&g_real_base);
      uint64_t step = 16667ull;
      if (m && rb && now > rb){
          step = now - rb;
          if (step < 4000ull)  step = 4000ull;    /* 250 fps ustu: kirp */
          if (step > 50000ull) step = 50000ull;   /* devasa dt -> telafi dongusu */
      }
      atomic_fetch_add(&g_vclock_base, step + cr); }
    atomic_store(&g_real_base, now);
    atomic_store(&g_vclock_polls, 0);
    { uint64_t v = atomic_load(&g_vclock_base);
      if (!atomic_load(&g_rt_start)){ atomic_store(&g_rt_start, real_us());
                                      atomic_store(&g_vt_start, v); }
      atomic_store(&g_vt_last, v); }
}

uint64_t host_us(void){
    uint64_t base = atomic_load(&g_vclock_base);
    uint64_t rb   = atomic_load(&g_real_base);
    uint64_t now  = real_us();
    uint64_t d    = (now > rb) ? (now - rb) : 0ull;
    /* Kare ICI ilerlemenin tavani.  16 ms COK DARDI: bir karede 16 ms'den
     * fazla OYUN zamani bekleyen kod (yaris oncesi ara sahne) tavana
     * carpiyor ve ancak asagidaki emniyet valfiyle (200 ms sonra) ilerleyebiliyordu
     * -> her kare zorunlu olarak ~235 ms surdu (4.6 fps, olculdu).
     * Tavani tamamen kaldirmak da olmaz: oyun gercekten gecen sureyi gorup
     * telafi dongusune giriyor.  Uc kare butcesi iyi bir orta yol. */
    { static _Atomic uint64_t maxd = 0;
      uint64_t m = atomic_load(&maxd);
      if (!m){ const char *e = ENVC("VCLOCK_MAXD");
               m = (e && *e) ? (uint64_t)strtoull(e,0,0) : 50000ull;
               if (m < 16000ull) m = 16000ull;
               atomic_store(&maxd, m); }
      if (d > m) d = m; }

    /* Kare uretmeden saati donup bekleyen dongulere karsi emniyet valfi.
     *
     * DIKKAT: eskiden olcut ANKET SAYISIYDI (kare basina >2000 sorgu ->
     * her 64 sorguda 400 us, >20000'de 2 ms...).  Yogun bir yaris
     * karesinde 20 binden fazla saat sorgusu NORMALDIR; bu, kare basina
     * ~100 ms sahte zaman ekliyordu.  Olculdu: gercek 1.01 sn'de sanal
     * 2.04 sn (oran 2.01) -- ara sahneler ve yaris iki kat hizli
     * kosuyordu.  Artik olcut GERCEK ZAMAN: yalnizca kare 200 ms'den
     * uzun surduyse (yani gercekten takildiysa) devreye girer ve kare
     * basina en fazla 100 ms ekler. */
    uint32_t n = atomic_fetch_add(&g_vclock_polls, 1);
    uint64_t stalled = (now > rb) ? (now - rb) : 0ull;      /* kirpilmamis */
    /* Emniyet valfi: esik 200 ms -> 60 ms, adim 2 ms -> 4 ms.  Tavan
     * yukseldigi icin bu yol artik yalnizca GERCEK takilmalarda calisir. */
    if (stalled > 60000ull && (n & 15u) == 15u){
        if (atomic_load(&g_vclock_creep) < 200000ull)
            atomic_fetch_add(&g_vclock_creep, 4000ull);
    }
    return base + d + atomic_load(&g_vclock_creep);
}
uint32_t host_ms(void){ return (uint32_t)(host_us() / 1000ull) + 1000u; }
uint64_t host_qpc(void){ return host_us(); }

static uint32_t heap_ptr = HEAP_BASE;
static uint64_t g_alloc_fail = 0;
static uint32_t galloc(uint32_t n, int zero){
    uint32_t need = (n + HDR + 15u) & ~15u;
    if (heap_ptr + need > HEAP_MAX || need < n){ g_alloc_fail++; return 0; }
    uint32_t p = heap_ptr; heap_ptr += need;
    mem_commit(p, need);
    MR32(p) = n;                       /* kullanici boyutu */
    MR32(p+4) = 0xA110C8EDu;           /* imza             */
    uint32_t u = p + HDR;
    if (zero) memset(g_mem + u, 0, n);
    return u;
}
static uint32_t gsize(uint32_t u){
    if (!u || u < HEAP_BASE + HDR || u >= HEAP_MAX) return 0;
    if (MR32(u - HDR + 4) != 0xA110C8EDu) return 0;
    return MR32(u - HDR);
}
uint32_t win32_block_size(uint32_t u){ return gsize(u); }
uint32_t win32_heap_used(void){ return heap_ptr - HEAP_BASE; }

/* --- izleme --- */
typedef struct { uint32_t calls; uint32_t unimpl; } ImpStat;
static ImpStat *stat_;
static uint32_t trace_n = 0, trace_max = 0;
void win32_set_trace_max(uint32_t v){ trace_max = v; }
static uint32_t unimpl_first = 0xffffffffu;
static const char *unimpl_name = 0;
uint64_t g_win32_calls = 0, g_win32_unimpl = 0;

static uint32_t tls_slot[128]; static int tls_next = 0;
static uint32_t g_cmdline = 0;
static uint64_t tick0;

static uint32_t ARG(CPU*c,int i){ return MR32(c->esp + 4u*(uint32_t)i); }  /* ret adresi POP edildikten sonra */

/* QPCWHO=1: QueryPerformanceCounter'i kim (thread, donus adresi, bir ust
 * cerceve) cagiriyor.  120 fps'te kare basina ~21 bin cagri goruldu (60 fps
 * PACE60'ta ~2 bin): zamana bagli bir bekleme dongusu.  Cagri GIL altinda,
 * tablo kilitsiz. */
typedef struct { uint32_t tid, ra, ra2; uint64_t n; } QpcWho;
static QpcWho g_qw[128];
static void qpc_report(void){
    fprintf(stderr, "\n=== QPCWHO: QueryPerformanceCounter cagiranlar ===\n");
    for (int k = 0; k < 128; k++){
        int b = -1;
        for (int i = 0; i < 128; i++) if (g_qw[i].n && (b < 0 || g_qw[i].n > g_qw[b].n)) b = i;
        if (b < 0 || k >= 24) break;
        fprintf(stderr, "  %12llu  thread %5u  donus 0x%08x  ust 0x%08x\n",
                (unsigned long long)g_qw[b].n, g_qw[b].tid, g_qw[b].ra, g_qw[b].ra2);
        g_qw[b].n = 0;
    }
}
/* ---- Oyunun kare bekleme dongusu: donme yerine uyku ----
 * sub_0057eae0 (0x57eb5e..0x57eb8e), kare suresi dt hedefin altindaysa:
 *     start = GetTicks(); hedef = (X - dt) * k;
 *     do { gecen = Fark(start, GetTicks()); } while (gecen < hedef);
 * GetTicks = sub_0043bdf0 = QPC >> [0x8284e4]; Fark = sub_0043be20 =
 * (now - start) * 2^S * [0x828028] saniye.  Dongu yalnizca saate bakiyor;
 * 120 fps'te kare basina ~19 bin QPC ve oyun thread'inin TUM bos zamani
 * (kare basina 8.2 ms CPU, %98) burada yaniyordu: D3DX'te kazanilan her
 * ms buraya gidiyor, guc degismiyordu.
 * Donguden gelen QPC'de (donus 0x43bdfe, onu cagiran 0x57eb79) kalan
 * sure oyunun kendi formuluyle hesaplanir ve (kalan - pay) kadar uyunur;
 * son pay yine donerek beklenir.  Cikis kosulu ve saat ayni oldugu icin
 * oyunun zamanlamasi degismez, yalnizca CPU bos kalir.  Uyku kevent
 * NOTE_CRITICAL ile (guest_sleep_ns): nanosleep ortalama 447 us gec
 * uyaniyordu, kevent 41 us (%98'i <= 200 us).
 * SPINSLEEP=<pay us> (varsayilan 200), SPINSLEEP=0 kapatir.
 * Yigin: POP32 sonrasi [esp+0xc] = 0x57eb79, E = esp+0x14 (0057eae0'nun
 * yerel taban), hedef float [E+0xc] = [esp+0x20], start = EBX. */
static int g_spin_pay = -1;
static uint64_t g_spin_n, g_spin_us, g_spin_gec[7];   /* gec uyanma: <=25,50,100,200,400,800,>800 us */
static void spin_report(void){
    fprintf(stderr, "\n=== SPINSLEEP: kare bekleme dongusu %llu kez uyudu, toplam %.2f sn (pay %d us) ===\n",
            (unsigned long long)g_spin_n, g_spin_us / 1e6, g_spin_pay);
    fprintf(stderr, "  gec uyanma: <=25us %llu  <=50 %llu  <=100 %llu  <=200 %llu  <=400 %llu  <=800 %llu  >800 %llu\n",
            (unsigned long long)g_spin_gec[0], (unsigned long long)g_spin_gec[1], (unsigned long long)g_spin_gec[2],
            (unsigned long long)g_spin_gec[3], (unsigned long long)g_spin_gec[4], (unsigned long long)g_spin_gec[5],
            (unsigned long long)g_spin_gec[6]);
    extern uint64_t g_sleep_late_ns, g_sleep_gil_ns, g_sleep_n;
    if (g_sleep_n) fprintf(stderr, "  ortalama: nanosleep gecikmesi %.0f us, uyanista kilit bekleme %.0f us\n",
                           g_sleep_late_ns / 1e3 / g_sleep_n, g_sleep_gil_ns / 1e3 / g_sleep_n);
}
uint64_t spin_slept_us(void){ return g_spin_us; }   /* perflog: kare sinirlayici uykusu */
static void qpc_spin_sleep(CPU *c){
    if (MR32(c->esp + 0xcu) != 0x57eb79u) return;
    if (g_spin_pay < 0){ const char *e = getenv("SPINSLEEP"); g_spin_pay = e ? atoi(e) : 200;
                         if (g_spin_pay && ENVC("QPCWHO")) atexit(spin_report); }
    if (!g_spin_pay) return;
    /* oyunun birimi: F = [0x828028] QPC tiki basina (olculen 0.001 = ms,
     * k = [0x784550] = 1000), X = [0x865558] = 1/120 sn kare hedefi */
    uint32_t S = MR32(0x8284e4u) & 31u;
    float F = MRF32(0x828028u), hedef = MRF32(c->esp + 0x20u);
    if (!(F > 0.0f) || !(hedef > 0.0f)) return;
    uint64_t now = host_qpc();
    int32_t d = (int32_t)((uint32_t)(now >> S) - c->ebx); if (d < 0) d = 0;
    double kalan = (double)hedef - (double)d * (double)(1u << S) * (double)F;
    double us = kalan / (double)F - (double)g_spin_pay;      /* QPC tiki = us (QPF 1 MHz) */
    if (us < 50.0 || us > 100000.0) return;                  /* kisa: don; >0.1 sn: beklenmedik */
    void guest_sleep_ns(long ns);
    guest_sleep_ns((long)(us * 1000.0));
    g_spin_n++; g_spin_us += (uint64_t)us;
    { double gec = (double)(host_qpc() - now) - us;          /* istenenden fazla uyunan us */
      int b = gec <= 25 ? 0 : gec <= 50 ? 1 : gec <= 100 ? 2 : gec <= 200 ? 3 : gec <= 400 ? 4 : gec <= 800 ? 5 : 6;
      g_spin_gec[b]++; }
}
static void qpc_who(CPU *c){
    static int init = 0; static uint64_t tot = 0;
    if (!init){ init = 1; atexit(qpc_report); }
    if ((++tot & 0xFFFFFu) == 0){ long metal_frames(void);
        fprintf(stderr, "  [qpc] %llu M cagri, kare %ld\n", (unsigned long long)(tot >> 20), metal_frames()); }
    uint32_t tid = (c->fs_base && mem_is_committed(c->fs_base+0x24u)) ? MR32(c->fs_base+0x24u) : 0u;
    uint32_t ra = c->eip;                       /* POP32 sonrasi: donus adresi */
    /* sub_0043bdf0 (oyunun zaman sarmalayicisi): sub esp,8; push arg; call.
     * POP32 sonrasi [esp]=arg, [esp+4..0xb]=yerel, [esp+0xc]=onu cagiran. */
    uint32_t ra2 = ra == 0x43bdfeu ? MR32(c->esp+0xcu)
                 : (c->ebp && mem_is_committed(c->ebp+4u)) ? MR32(c->ebp+4u) : 0u;
    uint32_t h = (tid * 2654435761u ^ ra * 40503u ^ ra2) & 127u;
    for (int i = 0; i < 128; i++, h = (h + 1) & 127u){
        QpcWho *w = &g_qw[h];
        if (!w->n){ w->tid = tid; w->ra = ra; w->ra2 = ra2; w->n = 1; return; }
        if (w->tid == tid && w->ra == ra && w->ra2 == ra2){ w->n++; return; }
    }
}

int win32_call(CPU *c, uint32_t idx){
    if (idx >= g_imp_count) return 0;
    const ImpEnt *e = &g_imp[idx];
    stat_[idx].calls++; g_win32_calls++;
    guest_fair_yield();          /* diger guest thread'lerine sira ver */
    POP32(c->eip);                      /* donus adresini al */
    int handled = 1;
    const char *n = e->name;
    uint32_t r = 0;

    /* QueryPerformanceCounter en sik cagri (kare sinirlayici); ad zincirinin
     * ~20 strcmp'ini atlamak icin import indisi ilk eslesmede onbellege alinir. */
    static uint32_t qpc_idx = 0xFFFFFFFFu;
    if (idx == qpc_idx || (qpc_idx == 0xFFFFFFFFu && !strcmp(n,"QueryPerformanceCounter") && (qpc_idx = idx, 1))){
        if (ENVC("QPCWHO")) qpc_who(c);
        if (c->eip == 0x43bdfeu) qpc_spin_sleep(c);
        uint64_t v = host_qpc(); uint32_t p=ARG(c,0);
        if(p){ MR32(p)=(uint32_t)v; MR32(p+4)=(uint32_t)(v>>32); } r=1; }
    else if (!strcmp(n,"GetVersion"))            r = 0x0A280105u;      /* 5.1.2600 */
    else if (!strcmp(n,"GetModuleHandleA"))      r = 0x00400000u;
    else if (!strcmp(n,"GetProcessHeap"))        r = 0x00000001u;
    else if (!strcmp(n,"HeapCreate"))            r = 0x00000002u;
    else if (!strcmp(n,"HeapDestroy"))           r = 1;
    else if (!strcmp(n,"HeapAlloc")){
        uint32_t sz=ARG(c,2); r = galloc(sz, ARG(c,1) & 8u);
        { const char *w = ENVC("TRACE_ALLOC");
          if (w){ uint32_t want = (uint32_t)strtoul(w,0,0);
            if (sz == want){ extern uint32_t g_watch_prev_ip;
              fprintf(stderr,"      [alloc] HeapAlloc(%u) -> 0x%08x   cagiran guest komut: 0x%08x\n",
                      sz, r, g_watch_prev_ip);
              guest_backtrace(c, 12); } } }
        /* WATCHALLOC=<boyut>: o boyuttaki ilk ayirmanin +16'sina watchpoint kur.
         * Sabit blogu yalnizca bir kez yazildigi icin yazmayi ancak boyle
         * yakalayabiliyoruz. */
        /* WATCHAT=<adres>: tam o adresi donduren ayirmada watchpoint kur */
        { const char *wa = ENVC("WATCHAT");
          if (wa && r){ static int armed2 = 0;
            uint32_t want3 = (uint32_t)strtoul(wa,0,0);
            if (!armed2 && r == want3){ armed2 = 1;
              void watch_arm(uint32_t);
              const char *wo = ENVC("WATCHOFF");
              uint32_t off3 = wo ? (uint32_t)strtoul(wo,0,0) : 0u;
              fprintf(stderr,"      [alloc] hedef blok 0x%08x (%u bayt) ayrildi, izlenen +%u\n",
                      r, sz, off3);
              watch_arm(r + off3);
              if (ENVC("MPROT")){ void mprot_arm(uint32_t); mprot_arm(r + off3); }
            } } }
        { const char *w2 = ENVC("WATCHALLOC");
          if (w2 && r){ static int armed = 0; static int seen = 0;
            uint32_t want2 = (uint32_t)strtoul(w2,0,0);
            const char *ws = ENVC("WATCHALLOC_N");
            int wantn = ws ? atoi(ws) : 1;
            if (!armed && sz == want2 && ++seen == wantn){ armed = 1;
              void watch_arm(uint32_t);
              fprintf(stderr,"      [alloc] %u baytlik blok 0x%08x ayrildi; +16'ya watchpoint\n", sz, r);
              watch_arm(r); } } }
        static int shown=0;
        if (ENVC("VERBOSE") && shown<2 && sz>0x100000u){ shown++;
            fprintf(stderr,"      [heap] HeapAlloc(size=0x%08x) SUPHELI. ebp=0x%08x esp=0x%08x\n", sz, c->ebp, c->esp);
            fprintf(stderr,"        yigin (esp'den):");
            for(int k=0;k<14;k++) fprintf(stderr," %s%08x", k?"":"", MR32(c->esp+4u*k));
            fprintf(stderr,"\n        ebp cevresi   :");
            for(int k=-4;k<=6;k++) fprintf(stderr," %s%08x", k==0?">":"", MR32(c->ebp+4u*(uint32_t)k));
            fprintf(stderr,"\n        esi=%08x edi=%08x ebx=%08x\n", c->esi,c->edi,c->ebx); }
    }
    else if (!strcmp(n,"HeapReAlloc")){
        uint32_t o=ARG(c,2), ns=ARG(c,3), os=gsize(o);
        uint32_t p=galloc(ns, ARG(c,1)&8u);
        if(p&&o){ uint32_t cp = os && os<ns ? os : ns; memcpy(g_mem+p, g_mem+o, cp); }
        r=p; }
    else if (!strcmp(n,"HeapFree"))              r = 1;
    else if (!strcmp(n,"HeapSize")){
        uint32_t lp=ARG(c,2); r = gsize(lp);
        static int shown=0;
        if (ENVC("VERBOSE") && shown<8){ shown++;
            fprintf(stderr,"      [heap] HeapSize(h=0x%08x,f=0x%08x,p=0x%08x) hdr.size=0x%08x hdr.sig=0x%08x -> 0x%08x\n",
                    ARG(c,0),ARG(c,1),lp,
                    (lp>=HEAP_BASE+HDR&&lp<HEAP_MAX)?MR32(lp-HDR):0xDEAD0000u,
                    (lp>=HEAP_BASE+HDR&&lp<HEAP_MAX)?MR32(lp-HDR+4):0xDEAD0000u, r); }
    }
    else if (!strcmp(n,"VirtualAlloc"))          r = galloc(ARG(c,1), 1);
    else if (!strcmp(n,"GlobalAlloc"))            r = galloc(ARG(c,1), ARG(c,0)&0x40u);
    else if (!strcmp(n,"LocalAlloc"))             r = galloc(ARG(c,1), ARG(c,0)&0x40u);
    else if (!strcmp(n,"GlobalFree")||!strcmp(n,"LocalFree")) r = 0;
    else if (!strcmp(n,"GlobalLock")||!strcmp(n,"LocalLock")) r = ARG(c,0);
    else if (!strcmp(n,"GlobalUnlock"))           r = 1;
    else if (!strcmp(n,"VirtualFree"))           r = 1;
    else if (!strcmp(n,"GetCommandLineA"))       r = g_cmdline;
    else if (!strcmp(n,"GetEnvironmentStrings")||!strcmp(n,"GetEnvironmentStringsW")){
        uint32_t p=galloc(8,1); r=p; }
    else if (!strcmp(n,"FreeEnvironmentStringsA")||!strcmp(n,"FreeEnvironmentStringsW")) r=1;
    else if (!strcmp(n,"GetStartupInfoA")){ uint32_t p=ARG(c,0); if(p){ mem_commit(p,68); memset(g_mem+p,0,68); MR32(p)=68; } r=0; }
    else if (!strcmp(n,"GetStdHandle"))          r = 0x10u + (ARG(c,0) & 3u);
    else if (!strcmp(n,"GetFileType"))           r = 2;   /* FILE_TYPE_CHAR */
    else if (!strcmp(n,"SetHandleCount"))        r = ARG(c,0);
    else if (!strcmp(n,"GetVersionExA")||!strcmp(n,"GetVersionExW")){
        uint32_t p=ARG(c,0);
        if(p){ MR32(p+4)=5; MR32(p+8)=1; MR32(p+12)=2600; MR32(p+16)=2; }
        r = 1; }
    else if (!strcmp(n,"GetACP"))                r = 1252;
    else if (!strcmp(n,"GetOEMCP"))              r = 437;
    else if (!strcmp(n,"GetCPInfo")){ uint32_t p=ARG(c,1); if(p){ mem_commit(p,20); memset(g_mem+p,0,20); MR32(p)=1; } r=1; }
    else if (!strcmp(n,"IsValidCodePage"))       r = 1;
    else if (!strcmp(n,"TlsAlloc"))              r = (tls_next<128)? (uint32_t)tls_next++ : 0xffffffffu;
    else if (!strcmp(n,"TlsSetValue")){
        uint32_t i=ARG(c,0), v=ARG(c,1);
        uint32_t arr = (c->fs_base && mem_is_committed(c->fs_base+0x2cu)) ? MR32(c->fs_base+0x2cu) : 0;
        if (arr && i<256u && mem_is_committed(arr+4u*i)){ MR32(arr+4u*i)=v; r=1; }
        else { if(i<128) tls_slot[i]=v; r=1; } }
    else if (!strcmp(n,"TlsGetValue")){
        uint32_t i=ARG(c,0);
        uint32_t arr = (c->fs_base && mem_is_committed(c->fs_base+0x2cu)) ? MR32(c->fs_base+0x2cu) : 0;
        if (arr && i<256u && mem_is_committed(arr+4u*i)) r = MR32(arr+4u*i);
        else r = (i<128)?tls_slot[i]:0; }
    else if (!strcmp(n,"TlsFree"))               r = 1;
    /* Kritik bolumler burada ELE ALINMAZ; win32_ext.c'de GERCEK
     * ozyinelemeli kilitlerle uygulaniyor (bkz. cs_for).  Etkisiz stub
     * birakilirsa GIL'in adil paylasimi karsilikli dislamayi bozuyor. */
    else if (!strcmp(n,"GetCurrentThreadId")){
        /* TEB'den gercek thread kimligi (fs:[0x24]); her thread benzersiz olmali */
        r = (c->fs_base && mem_is_committed(c->fs_base+0x24u)) ? MR32(c->fs_base+0x24u) : 1u;
        if (!r) r = 1u; }
    else if (!strcmp(n,"GetCurrentProcessId"))   r = 100;
    else if (!strcmp(n,"GetCurrentProcess"))     r = 0xffffffffu;
    else if (!strcmp(n,"GetLastError"))          r = 0;
    else if (!strcmp(n,"SetLastError"))          r = 0;
    else if (!strcmp(n,"GetTickCount")) r = host_ms();
    else if (!strcmp(n,"QueryPerformanceFrequency")){ uint32_t p=ARG(c,0); if(p){MR32(p)=1000000;MR32(p+4)=0;} r=1; }
    else if (!strcmp(n,"SetUnhandledExceptionFilter")) r = 0;
    else if (!strcmp(n,"UnhandledExceptionFilter"))    r = 0;
    else if (!strcmp(n,"IsProcessorFeaturePresent"))   r = 1;
    else if (!strcmp(n,"GetModuleFileNameA")){ uint32_t p=ARG(c,1),sz=ARG(c,2);
        const char*s="C:\\\\NFSU2\\\\SPEED2.EXE"; uint32_t l=(uint32_t)strlen(s);
        if(p&&sz){ if(l>=sz) l=sz-1; memcpy(g_mem+p,s,l); MR8(p+l)=0; } r=l; }
    else if (!strcmp(n,"ExitProcess")){ g_exit_code=(int)ARG(c,0);
        fprintf(stderr,"\n*** guest ExitProcess(%u) - sonlandiriliyor\n",ARG(c,0));
        if(g_exit_armed) longjmp(g_exit_jmp,1); return -1; }
    else if (!strcmp(n,"LoadLibraryA")||!strcmp(n,"LoadLibraryW")){
        r = 0x20000000u;
        { static int sh=0; uint32_t p2=ARG(c,0);
          if (ENVC("VERBOSE") && sh<12){ sh++; fprintf(stderr,"      [dll] LoadLibrary(ptr=0x%08x) \"%s\"\n", p2, p2?(char*)(g_mem+p2):"(NULL)"); } } }
    else if (!strcmp(n,"GetProcAddress")){
        uint32_t pname = ARG(c,1); r = 0;
        if (pname > 0xffff){                       /* isimle (ordinal degil) */
            const char *want = (const char*)(g_mem + pname);
            for (uint32_t i=0;i<g_imp_count;i++)
                if (!strcmp(g_imp[i].name, want)){ r = IMP_MAGIC + i; break; }
            { static int sh=0;
              if (ENVC("VERBOSE") && !r && sh<12){ sh++; fprintf(stderr,"      [dll] GetProcAddress(\"%s\") BULUNAMADI\n", want); } }
        } }
    else if (!strcmp(n,"FreeLibrary"))           r = 1;
    else if (!strcmp(n,"InterlockedIncrement")){ uint32_t p=ARG(c,0); r=++MR32(p); }
    else if (!strcmp(n,"InterlockedDecrement")){ uint32_t p=ARG(c,0); r=--MR32(p); }
    else if (!strcmp(n,"InterlockedExchange")){ uint32_t p=ARG(c,0); r=MR32(p); MR32(p)=ARG(c,1); }
    else if (!strcmp(n,"MultiByteToWideChar")){ uint32_t src=ARG(c,2); int sl=(int)ARG(c,3);
        uint32_t dst=ARG(c,4); int dl=(int)ARG(c,5);
        if(sl<0) sl=(int)strlen((char*)(g_mem+src))+1;
        if(dl==0){ r=(uint32_t)sl; } else { int k=0; for(;k<sl&&k<dl;k++) MR16(dst+2u*k)=MR8(src+k); r=(uint32_t)k; } }
    else if (!strcmp(n,"WideCharToMultiByte")){ uint32_t src=ARG(c,2); int sl=(int)ARG(c,3);
        uint32_t dst=ARG(c,4); int dl=(int)ARG(c,5);
        if(sl<0){ sl=0; while(MR16(src+2u*sl)) sl++; sl++; }
        if(dl==0){ r=(uint32_t)sl; } else { int k=0; for(;k<sl&&k<dl;k++) MR8(dst+k)=(uint8_t)MR16(src+2u*k); r=(uint32_t)k; } }
    else if (!strcmp(n,"GetStringTypeW")||!strcmp(n,"GetStringTypeA")){
        /* A: (LCID,dwInfoType,src,cch,out)  W: (dwInfoType,src,cch,out) */
        int aform = (n[13]=='A');
        uint32_t info = ARG(c, aform?1:0);
        uint32_t src  = ARG(c, aform?2:1);
        int      cch  = (int)ARG(c, aform?3:2);
        uint32_t out  = ARG(c, aform?4:3);
        int wide = !aform;
        if (cch < 0){ cch = 0; if(wide){ while(MR16(src+2u*cch)) cch++; } else { while(MR8(src+cch)) cch++; } cch++; }
        if (info == 1 && out){                       /* CT_CTYPE1 */
            for (int k=0;k<cch;k++){
                uint32_t ch = wide ? MR16(src+2u*k) : MR8(src+k);
                uint16_t f = 0x0200;                 /* C1_DEFINED */
                if (ch<128){
                    if (ch>='A'&&ch<='Z') f |= 0x0001|0x0100;
                    if (ch>='a'&&ch<='z') f |= 0x0002|0x0100;
                    if (ch>='0'&&ch<='9') f |= 0x0004|0x0080;
                    if ((ch>='a'&&ch<='f')||(ch>='A'&&ch<='F')) f |= 0x0080;
                    if (ch==' ') f |= 0x0008|0x0040;
                    if (ch=='\t'||ch=='\n'||ch=='\r'||ch==0x0b||ch==0x0c) f |= 0x0008;
                    if (ch<0x20||ch==0x7f) f |= 0x0020;
                    if (ch>0x20&&ch<0x7f&&!(f&(0x0100|0x0004))) f |= 0x0010;
                } else f |= 0x0100;
                MR16(out+2u*k) = f;
            }
        }
        r = 1; }
    else if (!strcmp(n,"LCMapStringW")||!strcmp(n,"LCMapStringA")){
        int wide = (n[11]=='W');
        uint32_t flags = ARG(c,1), src = ARG(c,2); int cch = (int)ARG(c,3);
        uint32_t dst = ARG(c,4); int cchd = (int)ARG(c,5);
        if (cch < 0){ cch = 0; if(wide){ while(MR16(src+2u*cch)) cch++; } else { while(MR8(src+cch)) cch++; } cch++; }
        if (cchd == 0){ r = (uint32_t)cch; }
        else {
            int k=0;
            for (;k<cch && k<cchd;k++){
                uint32_t ch = wide ? MR16(src+2u*k) : MR8(src+k);
                if ((flags & 0x00000100u) && ch<128 && ch>='A' && ch<='Z') ch += 32;   /* LOWERCASE */
                if ((flags & 0x00000200u) && ch<128 && ch>='a' && ch<='z') ch -= 32;   /* UPPERCASE */
                if (wide) MR16(dst+2u*k)=(uint16_t)ch; else MR8(dst+k)=(uint8_t)ch;
            }
            r = (uint32_t)k;
        } }
    else if (!strcmp(n,"GetLocaleInfoA")||!strcmp(n,"GetLocaleInfoW")){
        int wide = (n[13]=='W');
        uint32_t lct = ARG(c,1) & 0xffffu, p = ARG(c,2); int cch = (int)ARG(c,3);
        const char *v = 0; char num[16];
        switch (lct){
        case 0x0001: v="0409"; break;                 /* ILANGUAGE            */
        case 0x0002: v="English"; break;              /* SLANGUAGE            */
        case 0x1001: v="English"; break;              /* SENGLANGUAGE         */
        case 0x1002: v="United States"; break;        /* SENGCOUNTRY          */
        case 0x0003: v="ENU"; break;                  /* SABBREVLANGNAME      */
        case 0x0005: v="US"; break;                   /* SABBREVCTRYNAME      */
        case 0x000E: v="."; break;                    /* SDECIMAL             */
        case 0x000F: v=","; break;                    /* STHOUSAND            */
        case 0x0014: v="2"; break;                    /* IDIGITS              */
        case 0x000B: snprintf(num,sizeof num,"%d",437);  v=num; break; /* IDEFAULTCODEPAGE */
        case 0x1004: snprintf(num,sizeof num,"%d",1252); v=num; break; /* IDEFAULTANSICODEPAGE */
        case 0x0019: v="$"; break;                    /* SCURRENCY            */
        case 0x001D: v="/"; break;                    /* SDATE                */
        case 0x001E: v=":"; break;                    /* STIME                */
        default:     v="";  break;
        }
        uint32_t len = (uint32_t)strlen(v) + 1;
        if (cch == 0) r = wide ? len : len;
        else if (p){ uint32_t k=0; for(;k<len-1 && k<(uint32_t)cch-1;k++){ if(wide) MR16(p+2u*k)=v[k]; else MR8(p+k)=v[k]; }
                     if(wide) MR16(p+2u*k)=0; else MR8(p+k)=0; r=k+1; }
        else r = 0; }
    else if (!strcmp(n,"MessageBoxA")){
        uint32_t txt=ARG(c,1), cap=ARG(c,2);
        fprintf(stderr,"\n*** GUEST MESSAGEBOX ***\n  baslik: %s\n  metin : %s\n***\n",
                cap?(char*)(g_mem+cap):"(yok)", txt?(char*)(g_mem+txt):"(yok)");
        r = 1; }
    else if (!strcmp(n,"Direct3DCreate9")) r = com_direct3dcreate9();
    else if (!strcmp(n,"DirectInput8Create")) r = com_directinput8create(ARG(c,3));
    else if (!strcmp(n,"DirectSoundCreate") || (!strcmp(n,"#1") && !strcmp(e->dll,"DSOUND.dll")))
        r = com_directsoundcreate(ARG(c,1));
    else if (!strcmp(n,"SetConsoleCtrlHandler")) r = 1;
    else if (!strcmp(n,"RtlUnwind"))             r = 0;
    else if (!strcmp(n,"TerminateProcess")){ g_exit_code=(int)ARG(c,1);
        fprintf(stderr,"\n*** guest TerminateProcess(%u) - sonlandiriliyor\n",ARG(c,1));
        if(g_exit_armed) longjmp(g_exit_jmp,1); return -1; }
    else if (fileio_call(c, n, &r)) { /* dosya G/C */ }
    else if (win32_ext_call(c, n, &r)) { /* 2. tur katman */ }
    else { handled = 0; g_win32_unimpl++; stat_[idx].unimpl++;
           if (unimpl_first==0xffffffffu){ unimpl_first=idx; unimpl_name=n; } }

    if (g_bt_on && (!strcmp(n,"GetModuleFileNameA")||!strcmp(n,"ExitProcess")||!strcmp(n,"RaiseException")))
        guest_backtrace(c, 14);
    if (trace_n < trace_max)
        fprintf(stderr, "  [%4u] %-14s %-32s (%d arg) %s-> 0x%08x\n",
                ++trace_n, e->dll, n, e->nargs, handled?"":"[STUB] ", r);
    else trace_n++;

    c->eax = r;
    c->esp += 4u * (uint32_t)e->nargs;      /* stdcall: callee argumanlari temizler */
    return 1;
}

void win32_init(void){
    stat_ = calloc(g_imp_count, sizeof *stat_);
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    tick0 = (uint64_t)ts.tv_sec*1000 + ts.tv_nsec/1000000;
    mem_commit(HEAP_BASE, 0x10000);
    g_cmdline = galloc(64,1);
    strcpy((char*)(g_mem+g_cmdline), "SPEED2.EXE");
    /* IAT'i sihirli adreslerle doldur */
    for (uint32_t i=0;i<g_imp_count;i++) MR32(g_imp[i].iat) = IMP_MAGIC + i;
    com_init();
    /* CRT heap modu tanilamasi icin sonradan okunacak adresler */
    fprintf(stderr,"win32: %u import IAT'e baglandi, heap 0x%08x\n", g_imp_count, HEAP_BASE);
}

void win32_report(void){
    fprintf(stderr,"\n=== WIN32 KATMANI ===\n");
    fprintf(stderr,"  toplam import cagrisi : %llu\n",(unsigned long long)g_win32_calls);
    fprintf(stderr,"  implemente edilmemis  : %llu\n",(unsigned long long)g_win32_unimpl);
    fprintf(stderr,"  heap kullanimi        : %u KB   basarisiz ayirma: %llu\n",
            win32_heap_used()/1024u, (unsigned long long)g_alloc_fail);
    /* en cok cagrilan implemente edilmemisler */
    fprintf(stderr,"  guest thread : %llu baslatildi, %llu sonlandi\n",
            (unsigned long long)win32_ext_threads(),(unsigned long long)win32_ext_threads_exited());
    fileio_report();
    { extern void res_report(void); res_report(); }
    { extern void overrides_report(void); overrides_report(); }
    enh_report();
    { extern void d3dx_hle_report(void); d3dx_hle_report(); }
    fprintf(stderr,"  --- IMPLEMENTE EDILMEMIS (cagrildi, stub dondu) ---\n");
    for (uint32_t i=0;i<g_imp_count;i++)
        if (stat_[i].unimpl)
            fprintf(stderr,"    %-14s %-32s %u kez\n", g_imp[i].dll, g_imp[i].name, stat_[i].unimpl);
    fprintf(stderr,"  --- EN COK CAGRILAN 15 ---\n");
    for (int k=0;k<15;k++){
        uint32_t best=0, bi=0;
        for (uint32_t i=0;i<g_imp_count;i++) if (stat_[i].calls>best){ best=stat_[i].calls; bi=i; }
        if(!best) break;
        fprintf(stderr,"    %-14s %-32s %u\n", g_imp[bi].dll, g_imp[bi].name, best);
        stat_[bi].calls = 0;
    }
    int shown=0;
    for (uint32_t i=0;i<g_imp_count && shown<15;i++)
        if (stat_[i].calls){
            /* handled olup olmadigini bilmiyoruz; sadece sik cagrilanlari goster */
        }
    for (uint32_t i=0;i<g_imp_count;i++)
        if (stat_[i].calls>1 && shown<15){ fprintf(stderr,"    %-30s %u kez\n", g_imp[i].name, stat_[i].calls); shown++; }
}
