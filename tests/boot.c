/* Entry point'ten baslatma denemesi: nereye kadar gidiyor? */
#include "cpu.h"
#include "decls.h"
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <dlfcn.h>
#include <pthread/qos.h>
#include <stdlib.h>
#include <unistd.h>
#include <setjmp.h>
#include <pthread.h>
extern jmp_buf g_exit_jmp; extern int g_exit_code, g_exit_armed; extern int g_bt_on;
void dispatch_init(void); void dispatch_report(void);
void win32_init(void); void win32_report(void);
void com_report(void); void com_set_trace(int);
void fileio_set_root(const char*);
void fileio_set_write_root(const char*); const char *app_save_dir(void);
/* .app paketi destegi (runtime/bundle.m) */
int app_is_bundled(void); void app_set_defaults(void); void app_open_log(void);
const char *app_find_game(void); void app_no_game(void);
void guest_lock(CPU*); void guest_unlock(void);
void watch_arm(uint32_t); void watch_dump(const char*,int);
extern uint64_t g_hook_hits, g_gs_fail;
void mem_commit(uint32_t,uint32_t);
extern int g_guest_exited;
extern uint64_t g_stat_indirect_ok,g_stat_indirect_miss,g_stat_unsupported,g_win32_calls;

#define TEB_BASE   0x00C00000u
#define PEB_BASE   0x00C10000u
#define STACK_LO   0x7F000000u
#define STACK_HI   0x7FF00000u
static CPU cpu; static CPU *const c = &cpu;
void win32_set_trace_max(uint32_t);
extern void (*g_report_fn)(void);
void sampler_report(void);
static void all_reports(void){
    static int done; if(done) return; done=1;
    if (getenv("WATCH_ADDR")) watch_dump("calisma sonu / cokme", 24);
    fprintf(stderr,"\n  CRT heap: _crtheap=0x%08x __active_heap=%u\n", MR32(0x8ba4a4u), MR32(0x8ba4a8u));
    sampler_report();
    com_report(); win32_report(); { void vclock_report(void); vclock_report(); }
    { void metal_budget_report(void); metal_budget_report(); }
    dispatch_report();
}

static void all_reports(void);
static void on_alarm(int s){ (void)s;
    fprintf(stderr,"\n*** ZAMAN ASIMI - guest hala calisiyor (sonsuz dongu ya da uzun is)\n");
    fprintf(stderr,"    import cagrisi: %llu  dolayli: %llu  trap: %llu\n",
      (unsigned long long)g_win32_calls,(unsigned long long)g_stat_indirect_ok,
      (unsigned long long)g_stat_unsupported);
    /* "Nerede takildi?" sorusunun dogrudan cevabi: o anda calisan guest
     * thread'inin cagri zinciri. */
    { extern void guest_backtrace(CPU*,int);
      extern CPU *g_all_cpus[]; extern uint32_t g_all_tids[]; extern int g_all_n;
      fprintf(stderr,"    --- TUM guest thread'lerinin cagri zinciri (%d) ---\n", g_all_n);
      for (int i = 0; i < g_all_n; i++){
          if (!g_all_cpus[i]) continue;
          fprintf(stderr,"    [thread %d  tid=%u  esp=0x%08x]\n",
                  i, g_all_tids[i], g_all_cpus[i]->esp);
          guest_backtrace(g_all_cpus[i], 26);
      } }
    all_reports();
    /* PGO olcum yapisinda profil atexit ile yazilir, _exit onu atlar.
     * Sembolu dlsym ile ariyoruz: zayif bildirim Mach-O'da sembol hic
     * yoksa yine de link hatasi veriyor. */
    { int (*wr)(void) = (int(*)(void))dlsym(RTLD_DEFAULT, "__llvm_profile_write_file");
      if (wr) wr(); }
    _exit(2);
}
/* Ornekleyici: periyodik olarak calisan guest komutunu kaydeder.
 * Sonsuz donguleri ve sicak noktalari bulmak icin.                     */
#define SAMP_MAX 4096
static uint32_t g_samp[SAMP_MAX]; static int g_samp_n = 0;
static void *sampler_main(void *unused){
    (void)unused;
    extern uint32_t g_watch_prev_ip;
    long us = 2000;
    { const char *e = getenv("SAMPLE_US"); if (e) us = atol(e); }
    for (;;){
        struct timespec ts = { us/1000000L, (us%1000000L)*1000L };
        nanosleep(&ts, 0);
        /* WATCH=1 yoksa g_watch_prev_ip bos kalir; COVERAGE=1 derlemesinde
         * fonksiyon girisleri g_last_fn'i gunceller -> fonksiyon profili. */
        { extern uint32_t g_last_fn;
          uint32_t ip = g_watch_prev_ip ? g_watch_prev_ip : g_last_fn;
          if (g_samp_n < SAMP_MAX) g_samp[g_samp_n++] = ip; }
    }
    return 0;
}
void sampler_report(void){
    if (!g_samp_n) return;
    fprintf(stderr, "\n=== ORNEKLEYICI (%d ornek) - en sicak guest adresleri ===\n", g_samp_n);
    /* basit sayim: ayni adresleri grupla */
    for (int pass = 0; pass < 12; pass++){
        uint32_t best = 0; int bc = 0;
        for (int i = 0; i < g_samp_n; i++){
            if (!g_samp[i]) continue;
            int cnt = 0;
            for (int j = 0; j < g_samp_n; j++) if (g_samp[j] == g_samp[i]) cnt++;
            if (cnt > bc){ bc = cnt; best = g_samp[i]; }
        }
        if (!bc) break;
        fprintf(stderr, "   0x%08x  %5d ornek  (%%%.1f)\n", best, bc, 100.0*bc/g_samp_n);
        for (int i = 0; i < g_samp_n; i++) if (g_samp[i] == best) g_samp[i] = 0;
    }
}

static void *guest_main_thread(void *unused){
    (void)unused;
    pthread_setname_np("konuk ana");
    mem_altstack_install();
    if (setjmp(g_exit_jmp)==0){
        g_exit_armed = 1;
        guest_lock(&cpu);
        PUSH32(0xDEADC0DEu);
        sub_0075bcc7(&cpu);
        guest_unlock();
        fprintf(stderr,"\n*** guest entry point DONDU (eip=0x%08x, eax=0x%08x)\n",cpu.eip,cpu.eax);
    } else {
        fprintf(stderr,"\n*** guest cikis kodu %d ile sonlandi\n", g_exit_code);
    }
    return 0;
}

int main(int argc,char**argv){
    /* Paketten calisirken: run.sh'in ayarlari, bir log dosyasi ve oyun
     * verisinin yeri.  Terminalden calistirmada hicbiri devreye girmez. */
    const char *exe = (argc > 1 && strncmp(argv[1], "-psn_", 5)) ? argv[1] : NULL;
    if (app_is_bundled()){
        app_open_log(); app_set_defaults();
        if (!exe || access(exe, R_OK) != 0){
            exe = app_find_game();
            if (!exe){ app_no_game(); return 1; }
        }
    }
    if (!exe) exe = "SPEED2.EXE";

    mem_init(); mem_fault_handler_install();
    if(mem_load_pe(exe)) return 1;
    mem_commit(TEB_BASE,0x20000);
    mem_commit(STACK_LO, STACK_HI-STACK_LO);
    fileio_set_root(exe);
    /* Veri paketin icine gomuluyse kayitlar disariya yazilir. */
    { const char *sd = app_save_dir(); if (sd) fileio_set_write_root(sd); }
    dispatch_init(); win32_init();
    { extern void overrides_install(void); overrides_install(); }
    if (getenv("BT")) g_bt_on = 1;
    g_report_fn = all_reports;
    com_set_trace(getenv("COMTRACE")?1:0);
    if (getenv("TRACE_MAX")) win32_set_trace_max((uint32_t)atoi(getenv("TRACE_MAX")));
    else if (getenv("VERBOSE")) win32_set_trace_max(400u);
    { const char*w = getenv("WATCH_ADDR"); if (w) watch_arm((uint32_t)strtoul(w,0,0)); }

    /* TEB (fs:) */
    MR32(TEB_BASE+0x00)=0xFFFFFFFFu;      /* SEH zinciri sonu   */
    MR32(TEB_BASE+0x04)=STACK_HI;         /* StackBase          */
    MR32(TEB_BASE+0x08)=STACK_LO;         /* StackLimit         */
    MR32(TEB_BASE+0x18)=TEB_BASE;         /* Self               */
    MR32(TEB_BASE+0x20)=100;              /* ProcessId          */
    MR32(TEB_BASE+0x24)=1;                /* ThreadId           */
    MR32(TEB_BASE+0x2C)=TEB_BASE+0x1000;  /* TLS dizisi         */
    MR32(TEB_BASE+0x30)=PEB_BASE;         /* PEB                */
    mem_commit(PEB_BASE,0x1000);
    MR32(PEB_BASE+0x08)=0x00400000u;      /* ImageBaseAddress   */
    MR32(PEB_BASE+0xA4)=5; MR32(PEB_BASE+0xA8)=1;

    { extern CPU *g_cur_cpu; g_cur_cpu = &cpu; }
    memset(&cpu,0,sizeof cpu);
    cpu.esp = STACK_HI - 0x1000u;
    cpu.top = 0; cpu.fcw = 0x037f;
    cpu.fs_base = TEB_BASE;
    cpu.gs_base = TEB_BASE;

    /* CPUBENCH=1: konuk thread'in gercekten hangi hizda kostugunu olc.
     * Ayni dongu kabuktan 2.00 sn suruyor; surec icinde cok daha uzun
     * suruyorsa is VERIMLILIK cekirdegine atilmis demektir. */
    if (getenv("CPUBENCH")){
        struct timespec a,b; volatile uint64_t x=1;
        clock_gettime(CLOCK_MONOTONIC,&a);
        for (uint64_t i=0;i<300000000ull;i++) x = x*6364136223846793005ull + 1442695040888963407ull;
        clock_gettime(CLOCK_MONOTONIC,&b);
        fprintf(stderr,"  [cpubench] 3e8 iter: %.2f sn (kabukta 0.60 sn beklenir)\n",
                (b.tv_sec-a.tv_sec)+(b.tv_nsec-a.tv_nsec)/1e9);
    }
    fprintf(stderr,"\n=== ENTRY POINT CALISTIRILIYOR: 0x0075bcc7 ===\n");
    fprintf(stderr,"    TEB=0x%08x  yigin=0x%08x..0x%08x  esp=0x%08x\n\n",
            TEB_BASE,STACK_LO,STACK_HI,cpu.esp);
    { unsigned a = getenv("ALARM") ? (unsigned)atoi(getenv("ALARM")) : 20u;
      if (a) { signal(SIGALRM,on_alarm); alarm(a); } }
    {   /* Guest'i BUYUK yigina sahip bir thread'de calistir: lift edilmis kod
         * guest cagri zincirini host C yigini uzerinde tasir.                */
        pthread_attr_t at; pthread_attr_init(&at);
        pthread_attr_setstacksize(&at, 1024ull*1024ull*1024ull);   /* 1 GB */
        /* Konuk thread'i acikca EN YUKSEK QoS'a koy: kare uretiminin
         * tamami burada.  Varsayilan QoS mirasla geliyor ve pencere on
         * planda degilken arka plana dusuyordu -> verimlilik cekirdegi. */
        pthread_attr_set_qos_class_np(&at, QOS_CLASS_USER_INTERACTIVE, 0);
        pthread_t th;
        if (getenv("SAMPLE")){ extern int g_prof_on; g_prof_on = 1;
            pthread_t sth; pthread_create(&sth,0,sampler_main,0); pthread_detach(sth); }
        /* Metal cihazini guest baslamadan ONCE kur: oyun ilk dokularini
         * pencere acilmadan yaratiyor; cihaz yoksa sessizce kayboluyorlar. */
        if (getenv("WINDOW")){ extern void metal_early_init(void); metal_early_init(); }
        if (pthread_create(&th,&at,guest_main_thread,0)!=0){
            fprintf(stderr,"guest ana thread olusturulamadi\n"); return 1; }
        if (getenv("WINDOW")){
            /* macOS: pencere ANA thread'de olmali; guest zaten ayri thread'de */
            extern void metal_run_main_loop(void);
            metal_run_main_loop();
        } else {
            pthread_join(th,0);
        }
    }
    alarm(0);
    fprintf(stderr,"\n  kanca isabeti: %llu   /GS hatasi: %llu\n",
            (unsigned long long)g_hook_hits,(unsigned long long)g_gs_fail);
    if (getenv("WATCH_ADDR")) watch_dump("calisma sonu", 20);
    all_reports();
    return 0;
}
