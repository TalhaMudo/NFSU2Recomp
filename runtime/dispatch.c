/* Dolayli cagri dispatch: guest adres -> lift edilmis native fonksiyon.
 * O(1) dogrudan indeks tablosu (.text buyuklugunde uint32 dizisi).      */
#include "cpu.h"
uint64_t g_icall_hits = 0;
#include <stdio.h>
#include <stdlib.h>

typedef void (*guest_fn)(CPU*);
extern const uint32_t g_fn_addr[];
extern const guest_fn g_fn_ptr[];
extern const uint32_t g_fn_count;

#define TEXT_BASE 0x00401000u
#define TEXT_SIZE 0x00381af1u

static uint32_t *g_idx;

/* istatistik */
uint64_t g_stat_indirect_ok = 0, g_stat_indirect_miss = 0, g_stat_unsupported = 0;
#define MISS_MAX 64
static uint32_t g_miss[MISS_MAX], g_miss_from[MISS_MAX]; static int g_miss_n = 0;
extern uint32_t g_watch_prev_ip;
static uint32_t g_unsup[MISS_MAX]; static int g_unsup_n = 0;

extern void (*g_backtrace_fn)(struct CPU*,int);
void guest_backtrace(CPU*,int);

void dispatch_init(void){
    if (g_idx) return;
    g_backtrace_fn = (void(*)(struct CPU*,int))guest_backtrace;
    g_idx = calloc(TEXT_SIZE, sizeof(uint32_t));
    if (!g_idx){ fprintf(stderr,"dispatch tablosu ayrilamadi\n"); exit(1); }
    uint32_t dup = 0, oob = 0;
    for (uint32_t i = 0; i < g_fn_count; i++){
        uint32_t a = g_fn_addr[i];
        if (a - TEXT_BASE >= TEXT_SIZE){ oob++; continue; }
        if (g_idx[a - TEXT_BASE]) dup++;
        g_idx[a - TEXT_BASE] = i + 1;
    }
    fprintf(stderr, "dispatch: %u fonksiyon kayitli (%u yinelenen, %u aralik disi), tablo %.1f MB\n",
            g_fn_count, dup, oob, TEXT_SIZE*4.0/1048576.0);
}

int  win32_call(CPU*, uint32_t);
int  run_generated(CPU*, uint32_t);
void genexec_report(void);
int  com_call(CPU*, uint32_t);
int  g_guest_exited = 0;

/* --- calisma zamani yamalari: dolayli cagri hedeflerini degistir ---
 * Yeniden uretim gerektirmeden (yalnizca dolayli cagrilan fonksiyonlar). */
#define PATCH_MAX 24
static struct { uint32_t addr; guest_fn fn; } g_patch[PATCH_MAX];
static int g_patch_n = 0;
void dispatch_patch(uint32_t a, guest_fn f){
    if (a >= 0xE0000000u){        /* dispatch_indirect'teki sira buna dayaniyor */
        fprintf(stderr,"dispatch_patch: adres %08x metin araliginin disinda, yok sayildi\n", a);
        return; }
    if (g_patch_n < PATCH_MAX){ g_patch[g_patch_n].addr=a; g_patch[g_patch_n].fn=f; g_patch_n++; }
}

void dispatch_indirect(CPU *c, uint32_t addr){
    /* Once UCUZ aralik testi.  COM vtable metotlari (0xE...) ve Win32
     * import kopruleri (0xF...) konuk metin araliginin cok ustunde, yani
     * asagidaki yama tablosuyla ASLA cakismaz -- yamalar hep .text
     * icindedir (dispatch_patch yalnizca konuk fonksiyon adresiyle
     * cagriliyor).  Eskiden her cagri once g_patch[] dizisini dogrusal
     * tariyordu; kosuda tek basina 377 milyon COM cagrisi var. */
    if (addr >= 0xE0000000u){
        if (addr < 0xF0000000u){ com_call(c, addr); return; }
        if (win32_call(c, addr - 0xF0000000u) < 0) g_guest_exited = 1;
        return;
    }
    for (int i = 0; i < g_patch_n; i++)
        if (g_patch[i].addr == addr){ g_stat_indirect_ok++; g_patch[i].fn(c); return; }
    /* EFXSTAT: D3DX efekt vtable cagrilarinin envanteri (d3dx_hle.c) */
    { extern int g_efx_stat; extern void efx_stat_note(CPU *c, uint32_t addr);
      if (g_efx_stat && addr - 0x650000u < 0x40000u) efx_stat_note(c, addr); }
    uint32_t off = addr - TEXT_BASE;
    if (off < TEXT_SIZE){
        uint32_t k = g_idx[off];
        if (k){
            /* ICALL=<adres>: o fonksiyona yapilan DOLAYLI cagrilari say ve
             * ilk birkacinda cagri zincirini dok.  Sanal cagrilar statik
             * grafla gorunmedigi icin gerekli. */
            static uint32_t want = 0xFFFFFFFFu;
            if (want == 0xFFFFFFFFu){ const char *e = getenv("ICALL");
                                      want = e ? (uint32_t)strtoul(e,0,0) : 0; }
            if (want && addr == want){
                extern uint64_t g_icall_hits; g_icall_hits++;
                { static uint32_t seen[256]; static int ns=0;
                  uint32_t h2 = MR32(c->esp+8);
                  int f3=0; for(int q=0;q<ns;q++) if(seen[q]==h2) f3=1;
                  if(!f3 && ns<256){ seen[ns++]=h2;
                    fprintf(stderr,"  [baglama] hash=0x%08X %s\n", h2,
                            h2==0x3EE7363Fu ? "  <<< WORLDVIEWPROJECTION" :
                           (h2==0x04191C37u ? "  <<< TEXTUREOFFSET" : "")); } }
                static int shown = 0;
                if (shown < 3){ shown++;
                    fprintf(stderr,"  [icall] 0x%08x cagrildi (arg1=%08x arg2=%08x arg3=%08x)\n",
                            addr, MR32(c->esp+4), MR32(c->esp+8), MR32(c->esp+12));
                    guest_backtrace(c, 10);
                }
            }
            g_stat_indirect_ok++; g_fn_ptr[k-1](c); return; }
    }
    /* Lift edilmis karsiligi yok.  Oyun bu adrese CALISMA ANINDA kod
     * uretmis olabilir (NFSU2'nin malzeme boru hatti bunu yogun yapar).
     * Kucuk bir x86 yorumlayicisiyla dene; bilinmeyen komutta guvenle
     * geri doner (esp'yi eski haline getirir) ve asagidaki tesise duser. */
    { static int gx = -1;
      if (gx < 0){ const char *e = getenv("GENEXEC"); gx = (e && *e=='0') ? 0 : 1; }
      if (gx && mem_is_committed(addr) && run_generated(c, addr)){
          g_stat_indirect_ok++; return;
      } }

    g_stat_indirect_miss++;
    /* Cagiran: donus adresi ZATEN YIGINDA (POP32'den once).  Eskiden
     * g_watch_prev_ip kullaniliyordu ama o yalnizca komut izleme aciksa
     * dolar, bu yuzden rapor hep 0x00000000 gosteriyordu. */
    { uint32_t ret = MR32(c->esp);
      if (g_miss_n < MISS_MAX){ g_miss_from[g_miss_n]=ret; g_miss[g_miss_n++]=addr; }
      static int shown = 0;
      if (shown < 5){ shown++;
          fprintf(stderr, "\n  [COZULEMEYEN DOLAYLI CAGRI] hedef=0x%08x  donus=0x%08x\n"
                  "      eax=%08x ecx=%08x edx=%08x ebx=%08x esi=%08x edi=%08x\n"
                  "      arg1=%08x arg2=%08x arg3=%08x\n",
                  addr, ret, c->eax, c->ecx, c->edx, c->ebx, c->esi, c->edi,
                  MR32(c->esp+4), MR32(c->esp+8), MR32(c->esp+12));
          /* Hedefteki baytlar: oyun calisma aninda KOD mu uretti (thunk),
           * yoksa isaretci mi bozuk?  Ayirt etmenin en hizli yolu. */
          if (mem_is_committed(addr+15)){
              fprintf(stderr, "      hedefteki 96 bayt:\n       ");
              for (int q = 0; q < 96; q++){
                  if (q && !(q & 15)) fprintf(stderr, "\n       ");
                  fprintf(stderr, " %02x", MR8(addr+q));
              }
              fprintf(stderr, "\n");
          } else fprintf(stderr, "      hedef commit EDILMEMIS (veri degil, cop isaretci)\n");
          guest_backtrace(c, 12);
      } }
    POP32(c->eip);            /* donus adresini temizle, cagriyi yut */
}

void unsupported_insn(CPU *c, uint32_t addr, const char *mn){
    (void)c; (void)mn;
    g_stat_unsupported++;
    if (g_unsup_n < MISS_MAX) g_unsup[g_unsup_n++] = addr;
}

void cpuid_shim(CPU *c){
    /* jenerik Pentium III benzeri: SSE evet, 3DNow hayir */
    switch (c->eax){
    case 0: c->eax=1; c->ebx=0x756e6547; c->edx=0x49656e69; c->ecx=0x6c65746e; break;
    case 1: c->eax=0x000006d8; c->ebx=0; c->ecx=0; c->edx=0x0383fbff; break;
    default: c->eax=c->ebx=c->ecx=c->edx=0; break;
    }
}
uint64_t rdtsc_shim(void){
    static uint64_t t = 0; return (t += 3000);
}

void dispatch_report(void){
    fprintf(stderr,"\n=== calisma zamani ===\n");
    fprintf(stderr,"  dolayli cagri  cozuldu : %llu\n", (unsigned long long)g_stat_indirect_ok);
    fprintf(stderr,"  dolayli cagri  cozulemedi: %llu\n", (unsigned long long)g_stat_indirect_miss);
    fprintf(stderr,"  desteklenmeyen komut     : %llu\n", (unsigned long long)g_stat_unsupported);
    genexec_report();
    if (g_miss_n){ fprintf(stderr,"  cozulemeyen dolayli cagrilar (hedef <- cagiran komut):\n");
        int shown=0;
        for(int i=0;i<g_miss_n && shown<12;i++){
            int dup=0; for(int j=0;j<i;j++) if(g_miss_from[j]==g_miss_from[i]) dup=1;
            if(dup) continue;
            fprintf(stderr,"    0x%08x  <-  0x%08x\n", g_miss[i], g_miss_from[i]); shown++; }
    }
    if (g_unsup_n){ fprintf(stderr,"  ilk trap adresleri      :"); for(int i=0;i<g_unsup_n && i<10;i++) fprintf(stderr," 0x%08x",g_unsup[i]); fprintf(stderr,"\n"); }
}

/* --- guest yigin izleyici: .text araligindaki donus adreslerini cozer --- */
static uint32_t nearest_fn(uint32_t a){
    uint32_t lo=0, hi=g_fn_count, best=0;
    while(lo<hi){ uint32_t m=(lo+hi)/2;
        if(g_fn_addr[m]<=a){ best=g_fn_addr[m]; lo=m+1; } else hi=m; }
    return best;
}
void guest_backtrace(CPU *c, int maxn){
    fprintf(stderr,"    guest yigin izi (esp=0x%08x):\n", c->esp);
    int shown=0;
    for (uint32_t k=0; k<2048 && shown<maxn; k++){
        uint32_t a = c->esp + 4u*k;
        if (!mem_is_committed(a)) continue;      /* haritalanmamis sayfayi atla */
        uint32_t v = MR32(a);
        if (v - TEXT_BASE < TEXT_SIZE){
            uint32_t f = nearest_fn(v);
            fprintf(stderr,"      [%2d] +0x%04x  donus 0x%08x   (sub_%08x + 0x%x)\n",
                    shown, k*4, v, f, v-f);
            shown++;
        }
    }
}

/* Hatali host PC'den uretilmis guest fonksiyonunu coz: g_fn_ptr host
 * adreslerini guest adresleriyle eslestirir; PC'nin altindaki en yakin
 * fonksiyon basi aranir. */
uint32_t guest_fn_from_host(const void *pc){
    uintptr_t p = (uintptr_t)pc, best = 0; uint32_t bestg = 0;
    for (uint32_t i = 0; i < g_fn_count; i++){
        uintptr_t f = (uintptr_t)g_fn_ptr[i];
        if (f <= p && f > best){ best = f; bestg = g_fn_addr[i]; }
    }
    return bestg;
}
