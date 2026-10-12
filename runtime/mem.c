/* Guest bellek: 4 GB rezerve (PROT_NONE), kullanilan bolgeler commit edilir. */
#include <errno.h>
#include <pthread.h>
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdlib.h>
void (*g_backtrace_fn)(struct CPU*,int) = 0;   /* dispatch.c kurar */
CPU *g_cur_cpu = 0;

uint8_t *g_mem = 0;
uint32_t g_image_base = 0x400000u, g_res_dir_va = 0, g_res_dir_size = 0;

/* izleme durumu: her derlemede tanimli olmali (debug.c opsiyonel) */
uint32_t g_itrace[ITRACE_SIZE];
uint64_t g_itrace_n = 0;
uint32_t g_watch_addr = 0, g_watch_val = 0, g_watch_prev_ip = 0;
void (*g_itrace_dump_fn)(int) = 0;      /* debug.c kurar */
void (*g_report_fn)(void) = 0;          /* boot.c kurar */
__attribute__((weak)) void watch_report_change(uint32_t ip, uint32_t o, uint32_t n){
    (void)ip;(void)o;(void)n;
}
static uint64_t g_committed = 0;
/* commit edilen 16 KB sayfalarin bitmap'i (4 GB / 16 KB = 262144 bit = 32 KB) */
uint8_t g_cbits[1u<<15] = {0};
/* Govde artik cpu.h'ta satir ici (uc komut).  Ayri bir cagri olarak
 * profilde konuk thread'in %1.45'iydi -- is degil, cagri masrafi. */


void mprot_arm(uint32_t);
void mem_init(void){
    if (g_mem) return;
    g_mem = mmap(0, GSZ, PROT_NONE, MAP_PRIVATE|MAP_ANON|MAP_NORESERVE, -1, 0);
    if (g_mem == MAP_FAILED){ perror("mmap 4GB"); exit(1); }
}

void mem_commit(uint32_t addr, uint32_t size){
    uint64_t lo = addr & ~0x3FFFull;                    /* 16 KB sayfa */
    uint64_t hi = ((uint64_t)addr + size + 0x3FFF) & ~0x3FFFull;
    if (hi > 0x100000000ull) hi = 0x100000000ull;
    if (mprotect(g_mem + lo, hi - lo, PROT_READ|PROT_WRITE) != 0){
        fprintf(stderr,"mem_commit 0x%08x+%u basarisiz\n", addr, size); exit(1);
    }
    for (uint64_t a2 = lo; a2 < hi; a2 += 0x4000ull){
        uint32_t pg = (uint32_t)(a2 >> 14);
        g_cbits[pg>>3] |= (uint8_t)(1u << (pg&7));
    }
    g_committed += hi - lo;
    /* WATCHADDR=<adres>: o adres commit edilir edilmez watchpoint kur.
     * Boylece ilk yazan bile yakalanir. */
    { static int wa_done = 0;
      const char *wa = getenv("WATCHADDR");
      if (wa && !wa_done){
          uint32_t t = (uint32_t)strtoul(wa, 0, 0);
          if ((uint64_t)t >= lo && (uint64_t)t < hi){ wa_done = 1; mprot_arm(t); }
      } }
}
uint64_t mem_committed(void){ return g_committed; }

/* Guest'in commit edilmemis bolgeye erisimini teshis et */
static volatile int g_in_handler = 0;
/* --- yazma yakalama: bir guest sayfasini salt-okunur yapip yazani bul ---
 * Watchpoint enstrumantasyonu olmadan "bu bellege kim yaziyor?" sorusunu
 * cevaplar: yazma SIGSEGV uretir, hatali host PC'den uretilmis fonksiyon
 * (dolayisiyla guest fonksiyonu) cozulur.                                */
uint8_t *g_mprot_page = 0;
static size_t g_mprot_sz = 0;
static int g_mprot_hits = 0;
static uint32_t g_mprot_target = 0;      /* ilgilendigimiz tam adres */
static volatile int g_mprot_found = 0;   /* hedefe yazan bulundu */
static int      g_mprot_persist = 0;     /* her karede yeniden kur */
/* dispatch.c saglar; tek basina linklenen test kosumlarinda zayif varsayilan */
__attribute__((weak)) uint32_t guest_fn_from_host(const void *pc){ (void)pc; return 0; }

/* Korunan sayfa 16 KB ve cok yogun: ilk yabanci yazma korumayi dusuruyor.
 * Ayri bir thread surekli yeniden kurar, boylece hedefe yazan yakalanir. */
static void *mprot_rearm_thread(void *a0){
    (void)a0;
    for (;;){
        /* g_mprot_page kurulu olsa bile yeniden koru: araya giren bir
         * mem_commit korumayi sessizce kaldirmis olabilir. */
        if (g_mprot_target && !g_mprot_found){
            size_t ps = (size_t)getpagesize();
            uint8_t *pg = (uint8_t*)((uintptr_t)(g_mem + g_mprot_target) & ~(uintptr_t)(ps-1));
            if (mprotect(pg, ps, PROT_READ) == 0){ g_mprot_page = pg; g_mprot_sz = ps; }
        }
        struct timespec ts = {0, 50 * 1000};    /* 50 us */
        nanosleep(&ts, 0);
    }
    return 0;
}

void mprot_arm(uint32_t guest_addr){
    /* Apple Silicon'da sayfa 16 KB; hizalama sistem degerinden alinmali. */
    size_t ps = (size_t)getpagesize();
    uint8_t *pg = (uint8_t*)((uintptr_t)(g_mem + guest_addr) & ~(uintptr_t)(ps - 1));
    if (mprotect(pg, ps, PROT_READ) != 0){
        fprintf(stderr,"mprot: kurulamadi (%s)\n", strerror(errno)); return;
    }
    g_mprot_page = pg; g_mprot_sz = ps; g_mprot_target = guest_addr;
    g_mprot_persist = getenv("MPROT_PERSIST") ? 1 : 0;
    if (getenv("MPROT_HARD")){
        static pthread_t th; static int started = 0;
        if (!started){ started = 1; pthread_create(&th, 0, mprot_rearm_thread, 0); }
    }
    fprintf(stderr,"mprot: guest 0x%08x sayfasi salt-okunur (yazani yakalamak icin)\n",
            guest_addr);
}

/* Kare basinda korumayi yeniden kur (tek seferlik yakalama yetmiyorsa). */
void mprot_rearm(void){
    if (!g_mprot_persist || g_mprot_page || !g_mprot_target) return;
    size_t ps = (size_t)getpagesize();
    uint8_t *pg = (uint8_t*)((uintptr_t)(g_mem + g_mprot_target) & ~(uintptr_t)(ps - 1));
    if (mprotect(pg, ps, PROT_READ) == 0){ g_mprot_page = pg; g_mprot_sz = ps; }
}

static void segv(int sig, siginfo_t *si, void *uc){
    (void)sig;
    /* korunan sayfaya yazma mi? */
    if (g_mprot_page){
        uint8_t *fa = (uint8_t*)si->si_addr;
        if (fa >= g_mprot_page && fa < g_mprot_page + g_mprot_sz){
            void *pc = 0;
#if defined(__aarch64__)
            ucontext_t *u = (ucontext_t*)uc;
            if (u && u->uc_mcontext) pc = (void*)(uintptr_t)u->uc_mcontext->__ss.__pc;
#endif
            uint32_t gfn = pc ? guest_fn_from_host(pc) : 0;
            uint32_t ga = (uint32_t)(fa - g_mem);
            int interesting = !g_mprot_target ||
                (ga >= g_mprot_target && ga < g_mprot_target + 64u);
            if (getenv("MPROT_ANY")) interesting = 1;   /* mekanizma testi */
            if (interesting && g_mprot_hits < 12){
                g_mprot_hits++;
                fprintf(stderr,"mprot: HEDEFE YAZMA guest 0x%08x  guest fonksiyon=sub_%08x\n",
                        ga, gfn);
                if (g_mprot_hits >= 6) g_mprot_found = 1;
            }
            mprotect(g_mprot_page, g_mprot_sz, PROT_READ|PROT_WRITE);
            g_mprot_page = 0;           /* tek seferlik */
            return;                      /* komut yeniden calissin */
        }
    }
    if (g_in_handler){ fprintf(stderr,"\n*** isleyici icinde ikinci hata - cikiliyor\n"); _exit(4); }
    g_in_handler = 1;
    /* Hataya neden olan GUEST FONKSIYONU: host PC'den cozulur.  Yigin izi
     * yalnizca cagiranlari verir; hata tam olarak nerede oldu, bu soyler. */
    { void *fpc = 0;
#if defined(__aarch64__)
      ucontext_t *u2 = (ucontext_t*)uc;
      if (u2 && u2->uc_mcontext) fpc = (void*)(uintptr_t)u2->uc_mcontext->__ss.__pc;
#endif
      uint32_t ffn = fpc ? guest_fn_from_host(fpc) : 0;
      if (ffn) fprintf(stderr,"\n*** HATA GUEST FONKSIYONU: sub_%08x\n", ffn);
    }
    uint8_t *p = (uint8_t*)si->si_addr;
    if (p >= g_mem && p < g_mem + GSZ)
        fprintf(stderr,"\n*** GUEST BELLEK HATASI: guest adres 0x%08llx (haritalanmamis)\n",
                (unsigned long long)(p - g_mem));
    else
        fprintf(stderr,"\n*** HOST SEGV: %p (guest alani disi - muhtemelen HOST YIGIN TASMASI:"
                       " guest kodunda sonsuz ozyineleme)\n", p);
    { extern uint32_t g_watch_prev_ip; extern uint64_t g_itrace_n;
      if (g_itrace_n) fprintf(stderr,"    HATAYA NEDEN OLAN GUEST KOMUTU: 0x%08x\n", g_watch_prev_ip); }
    if (g_cur_cpu){
        CPU *c = (CPU*)g_cur_cpu;
        fprintf(stderr,"    eax=%08x ecx=%08x edx=%08x ebx=%08x\n", c->eax,c->ecx,c->edx,c->ebx);
        fprintf(stderr,"    esp=%08x ebp=%08x esi=%08x edi=%08x  df=%u\n", c->esp,c->ebp,c->esi,c->edi,c->df);
        fprintf(stderr,"    fs_base=%08x tid=%u   [ebp+8]=%08x [ebp+4]=%08x\n",
                c->fs_base,
                (c->fs_base && mem_is_committed(c->fs_base+0x24u)) ? MR32(c->fs_base+0x24u) : 0u,
                mem_is_committed(c->ebp+8u) ? MR32(c->ebp+8u) : 0xDEADu,
                mem_is_committed(c->ebp+4u) ? MR32(c->ebp+4u) : 0xDEADu);
    }
    if (g_cur_cpu && g_backtrace_fn) g_backtrace_fn(g_cur_cpu, 12);
    if (g_itrace_dump_fn && getenv("ITRACE")) g_itrace_dump_fn(atoi(getenv("ITRACE")));
    if (g_report_fn) g_report_fn();
    _exit(3);
}
/* sigaltstack THREAD BASINA kurulur: her guest thread'i kendi sinyal
 * yiginini kurmali, yoksa yigin tasmasinda isleyici de coker.          */
void mem_altstack_install(void){
    size_t sz = (size_t)SIGSTKSZ * 8;
    void *p = malloc(sz);
    if (!p) return;
    stack_t ss; ss.ss_sp = p; ss.ss_size = sz; ss.ss_flags = 0;
    sigaltstack(&ss, 0);
}
void mem_fault_handler_install(void){
    mem_altstack_install();
    struct sigaction sa; memset(&sa,0,sizeof sa);
    sa.sa_sigaction = segv; sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV,&sa,0); sigaction(SIGBUS,&sa,0);
}

int mem_load_pe(const char *path){
    FILE *f = fopen(path, "rb");
    if(!f){ perror(path); return -1; }
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    uint8_t *buf = malloc(sz);
    if (fread(buf,1,sz,f)!=(size_t)sz){ fclose(f); free(buf); return -1; }
    fclose(f);
    uint32_t e_lfanew = *(uint32_t*)(buf+0x3c);
    uint8_t *nt = buf + e_lfanew;
    if (*(uint32_t*)nt != 0x00004550u){ fprintf(stderr,"PE imzasi yok\n"); free(buf); return -1; }
    uint16_t nsec  = *(uint16_t*)(nt+6);
    uint16_t optsz = *(uint16_t*)(nt+20);
    uint32_t base  = *(uint32_t*)(nt+24+28);
    uint32_t imgsz = *(uint32_t*)(nt+24+56);
    /* DataDirectory[2] = RESOURCE */
    { /* PE32: DataDirectory opsiyonel basligin 96. baytinda; RESOURCE = indeks 2 */
      uint32_t dd = 24 + 96 + 2*8;
      uint32_t rva = *(uint32_t*)(nt + dd);
      uint32_t sz  = *(uint32_t*)(nt + dd + 4);
      g_image_base = base; g_res_dir_va = rva ? base + rva : 0; g_res_dir_size = sz; }
    uint8_t *sh    = nt + 24 + optsz;
    mem_init();
    mem_commit(base, imgsz);                 /* tum image araligi */
    for (int i=0;i<nsec;i++){
        uint8_t *s = sh + i*40;
        char nm[9]={0}; memcpy(nm,s,8);
        uint32_t vsize=*(uint32_t*)(s+8), va=*(uint32_t*)(s+12);
        uint32_t rsize=*(uint32_t*)(s+16), roff=*(uint32_t*)(s+20);
        uint32_t n = rsize < vsize ? rsize : vsize;
        memcpy(g_mem + base + va, buf + roff, n);
        fprintf(stderr,"  yuklendi %-9s -> 0x%08x  %u bayt\n", nm[0]?nm:"<isimsiz>", base+va, n);
    }
    free(buf);
    fprintf(stderr,"  image 0x%08x .. 0x%08x   kaynak dizini 0x%08x (%u bayt)\n",
            base, base+imgsz, g_res_dir_va, g_res_dir_size);
    return 0;
}
