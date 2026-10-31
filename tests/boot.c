/* Starting from the entry point: how far does it get? */
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
/* .app bundle support (runtime/bundle.m) */
int app_is_bundled(void); void app_set_defaults(void); void app_open_log(void);
const char *app_find_game(void); void app_no_game(void); int app_check_exe(const char *exe);
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
    if (getenv("WATCH_ADDR")) watch_dump("end of run / crash", 24);
    fprintf(stderr,"\n  CRT heap: _crtheap=0x%08x __active_heap=%u\n", MR32(GA(0x8ba4a4u)), MR32(GA(0x8ba4a8u)));
    sampler_report();
    com_report(); win32_report(); { void vclock_report(void); vclock_report(); }
    { void metal_budget_report(void); metal_budget_report(); }
    dispatch_report();
}

static void all_reports(void);
static void on_alarm(int s){ (void)s;
    fprintf(stderr,"\n*** TIMEOUT - the guest is still running (infinite loop or long work)\n");
    fprintf(stderr,"    import calls: %llu  indirect: %llu  traps: %llu\n",
      (unsigned long long)g_win32_calls,(unsigned long long)g_stat_indirect_ok,
      (unsigned long long)g_stat_unsupported);
    /* The direct answer to "where did it get stuck?": the call chain of the guest
     * thread running at that moment. */
    { extern void guest_backtrace(CPU*,int);
      extern CPU *g_all_cpus[]; extern uint32_t g_all_tids[]; extern int g_all_n;
      fprintf(stderr,"    --- call chains of ALL guest threads (%d) ---\n", g_all_n);
      for (int i = 0; i < g_all_n; i++){
          if (!g_all_cpus[i]) continue;
          fprintf(stderr,"    [thread %d  tid=%u  esp=0x%08x]\n",
                  i, g_all_tids[i], g_all_cpus[i]->esp);
          guest_backtrace(g_all_cpus[i], 26);
      } }
    all_reports();
    /* In the PGO measurement build the profile is written with atexit, and _exit
     * skips it.  We look the symbol up with dlsym: a weak declaration still gives
     * a link error on Mach-O if the symbol doesn't exist at all. */
    { int (*wr)(void) = (int(*)(void))dlsym(RTLD_DEFAULT, "__llvm_profile_write_file");
      if (wr) wr(); }
    _exit(2);
}
/* Sampler: periodically records the running guest instruction.
 * For finding infinite loops and hot spots.                             */
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
        /* Without WATCH=1 g_watch_prev_ip stays empty; in a COVERAGE=1 build function
         * entries update g_last_fn -> a function profile. */
        { extern uint32_t g_last_fn;
          uint32_t ip = g_watch_prev_ip ? g_watch_prev_ip : g_last_fn;
          if (g_samp_n < SAMP_MAX) g_samp[g_samp_n++] = ip; }
    }
    return 0;
}
void sampler_report(void){
    if (!g_samp_n) return;
    fprintf(stderr, "\n=== SAMPLER (%d samples) - hottest guest addresses ===\n", g_samp_n);
    /* simple counting: group identical addresses */
    for (int pass = 0; pass < 12; pass++){
        uint32_t best = 0; int bc = 0;
        for (int i = 0; i < g_samp_n; i++){
            if (!g_samp[i]) continue;
            int cnt = 0;
            for (int j = 0; j < g_samp_n; j++) if (g_samp[j] == g_samp[i]) cnt++;
            if (cnt > bc){ bc = cnt; best = g_samp[i]; }
        }
        if (!bc) break;
        fprintf(stderr, "   0x%08x  %5d samples  (%%%.1f)\n", best, bc, 100.0*bc/g_samp_n);
        for (int i = 0; i < g_samp_n; i++) if (g_samp[i] == best) g_samp[i] = 0;
    }
}

static void *guest_main_thread(void *unused){
    (void)unused;
    pthread_setname_np("guest main");
    { extern pthread_t g_guest_main_th; g_guest_main_th = pthread_self(); }
    mem_altstack_install();
    if (setjmp(g_exit_jmp)==0){
        g_exit_armed = 1;
        guest_lock(&cpu);
        PUSH32(0xDEADC0DEu);
        sub_0075bcc7(&cpu);
        guest_unlock();
        fprintf(stderr,"\n*** guest entry point RETURNED (eip=0x%08x, eax=0x%08x)\n",cpu.eip,cpu.eax);
    } else {
        fprintf(stderr,"\n*** guest terminated with exit code %d\n", g_exit_code);
    }
    return 0;
}

int main(int argc,char**argv){
    /* System shortcut guard (metal.m ks_engel): the same binary, without ever
     * starting the game, waits for the game process to end. */
    if (getenv("NFSU_KISAYOL_BEKCI")){ extern int metal_kisayol_bekci(void); return metal_kisayol_bekci(); }
    /* When running from a bundle: run.sh's settings, a log file and the location
     * of the game data.  None of these kick in when running from the terminal. */
    const char *exe = (argc > 1 && strncmp(argv[1], "-psn_", 5)) ? argv[1] : NULL;
    if (app_is_bundled()){
        app_open_log(); app_set_defaults();
        if (!exe || access(exe, R_OK) != 0){
            exe = app_find_game();
            if (!exe){ app_no_game(); return 1; }
        }
    }
    if (!exe) exe = "SPEED2.EXE";
    if (!app_check_exe(exe)) return 1;

    mem_init(); mem_fault_handler_install();
    if(mem_load_pe(exe)) return 1;
    mem_commit(TEB_BASE,0x20000);
    mem_commit(STACK_LO, STACK_HI-STACK_LO);
    fileio_set_root(exe);
    /* Saves are written outside the game/app folder (so they aren't lost on update). */
    { const char *sd = app_save_dir(); if (sd) fileio_set_write_root(sd); }
    dispatch_init(); win32_init();
    { extern void overrides_install(void); overrides_install(); }
    { extern void highfps_install(void); highfps_install(); }   /* runtime/highfps.c */
    { extern void gamepad_install(void); gamepad_install(); }   /* runtime/gamepad.m */
    if (getenv("BT")) g_bt_on = 1;
    g_report_fn = all_reports;
    com_set_trace(getenv("COMTRACE")?1:0);
    if (getenv("TRACE_MAX")) win32_set_trace_max((uint32_t)atoi(getenv("TRACE_MAX")));
    else if (getenv("VERBOSE")) win32_set_trace_max(400u);
    { const char*w = getenv("WATCH_ADDR"); if (w) watch_arm((uint32_t)strtoul(w,0,0)); }

    /* TEB (fs:) */
    MR32(TEB_BASE+0x00)=0xFFFFFFFFu;      /* end of the SEH chain */
    MR32(TEB_BASE+0x04)=STACK_HI;         /* StackBase          */
    MR32(TEB_BASE+0x08)=STACK_LO;         /* StackLimit         */
    MR32(TEB_BASE+0x18)=TEB_BASE;         /* Self               */
    MR32(TEB_BASE+0x20)=100;              /* ProcessId          */
    MR32(TEB_BASE+0x24)=1;                /* ThreadId           */
    MR32(TEB_BASE+0x2C)=TEB_BASE+0x1000;  /* TLS array          */
    MR32(TEB_BASE+0x30)=PEB_BASE;         /* PEB                */
    mem_commit(PEB_BASE,0x1000);
    MR32(PEB_BASE+0x08)=0x00400000u;      /* ImageBaseAddress   */
    MR32(PEB_BASE+0xA4)=5; MR32(PEB_BASE+0xA8)=1;

    { extern CPU *g_cur_cpu; g_cur_cpu = &cpu; }
    memset(&cpu,0,sizeof cpu);
    cpu.esp = STACK_HI - 0x1000u;
    cpu.top = 0; cpu.fcw = 0x037f; cpu.mxcsr = 0x1F80u;
    cpu.fs_base = TEB_BASE;
    cpu.gs_base = TEB_BASE;

    /* CPUBENCH=1: measure the speed the guest thread really runs at.
     * The same loop takes 2.00 s from the shell; if it takes much longer inside
     * the process, the work has been put on an EFFICIENCY core. */
    if (getenv("CPUBENCH")){
        struct timespec a,b; volatile uint64_t x=1;
        clock_gettime(CLOCK_MONOTONIC,&a);
        for (uint64_t i=0;i<300000000ull;i++) x = x*6364136223846793005ull + 1442695040888963407ull;
        clock_gettime(CLOCK_MONOTONIC,&b);
        fprintf(stderr,"  [cpubench] 3e8 iter: %.2f s (0.60 s expected from the shell)\n",
                (b.tv_sec-a.tv_sec)+(b.tv_nsec-a.tv_nsec)/1e9);
    }
    fprintf(stderr,"\n=== RUNNING THE ENTRY POINT: 0x0075bcc7 ===\n");
    fprintf(stderr,"    TEB=0x%08x  stack=0x%08x..0x%08x  esp=0x%08x\n\n",
            TEB_BASE,STACK_LO,STACK_HI,cpu.esp);
    { unsigned a = getenv("ALARM") ? (unsigned)atoi(getenv("ALARM")) : 20u;
      if (a) { signal(SIGALRM,on_alarm); alarm(a); } }
    {   /* Run the guest on a thread with a BIG stack: lifted code carries the guest
         * call chain on the host C stack.                                    */
        pthread_attr_t at; pthread_attr_init(&at);
        pthread_attr_setstacksize(&at, 1024ull*1024ull*1024ull);   /* 1 GB */
        /* Put the guest thread explicitly at the HIGHEST QoS: all of the frame
         * production happens here.  The default QoS is inherited, and it dropped
         * to the background when the window wasn't in front -> efficiency core. */
        pthread_attr_set_qos_class_np(&at, QOS_CLASS_USER_INTERACTIVE, 0);
        pthread_t th;
        if (getenv("SAMPLE")){ extern int g_prof_on; g_prof_on = 1;
            pthread_t sth; pthread_create(&sth,0,sampler_main,0); pthread_detach(sth); }
        /* Set up the Metal device BEFORE the guest starts: the game creates its first
         * textures before the window opens; without a device they silently vanish. */
        if (getenv("WINDOW")){ extern void metal_early_init(void); metal_early_init(); }
        if (pthread_create(&th,&at,guest_main_thread,0)!=0){
            fprintf(stderr,"could not create the guest main thread\n"); return 1; }
        if (getenv("WINDOW")){
            /* macOS: the window must be on the MAIN thread; the guest is on a separate thread anyway */
            extern void metal_run_main_loop(void);
            metal_run_main_loop();
        } else {
            pthread_join(th,0);
        }
    }
    alarm(0);
    fprintf(stderr,"\n  hook hits: %llu   /GS failures: %llu\n",
            (unsigned long long)g_hook_hits,(unsigned long long)g_gs_fail);
    if (getenv("WATCH_ADDR")) watch_dump("end of run", 20);
    /* MEMDUMP=0xADDR[,0xADDR...] (diagnostic): guest dwords at exit */
    { const char *md = getenv("MEMDUMP");
      for (const char *q = md; q && *q; ){
          uint32_t a = (uint32_t)strtoul(q, NULL, 0);
          if (a && mem_is_committed(a + 3))
              fprintf(stderr, "  [memdump] 0x%08x = 0x%08x (%d, %.6g f)\n", a, MR32(a), (int)MR32(a),
                      (double)*(float *)(g_mem + a));
          q = strchr(q, ','); if (q) q++; } }
    all_reports();
    return 0;
}
