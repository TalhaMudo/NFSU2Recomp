/* Phase 3 seed: the Win32 API subset needed for boot + tracing for the rest. */
#include <stdatomic.h>
#include "enhance.h"
#include "cpu.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <time.h>
#include <setjmp.h>
#include <pthread.h>
#include <unistd.h>

/* getenv() SCANS the environment array and takes a lock on every call.  On
 * per-draw paths (for example the "PDLIST" check in SetVertexShaderConstantF
 * was the FIRST term of an && chain) it has a measurable cost: in the profile
 * getenv+__findenv_locked ate 3% of the guest thread.
 * Environment variables don't change during a run -> resolve once, keep. */
#define ENVC(nm_) ({ static const char *_ev_; static int _ei_; \
                     if (!_ei_){ _ei_ = 1; _ev_ = getenv(nm_); } _ev_; })

jmp_buf g_exit_jmp;
int g_exit_code = 0;
int g_exit_armed = 0;
pthread_t g_guest_main_th;           /* set by tests/boot.c: the thread that owns g_exit_jmp */
/* ExitProcess / TerminateProcess.  Before: only longjmp'd out of the guest main
 * thread; the window's main loop kept running, so "Quit Game" left a frozen
 * window that had to be force-quit.  Now the main loop is told to quit too, and
 * a call from any other guest thread (whose stack g_exit_jmp does not belong to)
 * just parks that thread instead of jumping across threads. */
static void guest_exit_now(void){
    void metal_request_quit(void); metal_request_quit();
    if (g_exit_armed && pthread_equal(pthread_self(), g_guest_main_th)) longjmp(g_exit_jmp, 1);
    for (;;) pause();                /* the process ends when main() returns */
}

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

/* --- guest heap: see gheap.c (headers keep the size for HeapSize/ReAlloc) --- */
#define HEAP_BASE 0x10000000u
#define HEAP_MAX  0x2F000000u
#define HDR 16u
/* ---- HYBRID virtual clock -----------------------------------------------
 * Tried:
 *   (a) a fixed increment per call -> irregular time, broken animations
 *   (b) the real monotonic clock -> since the emulator is slow, the game sees
 *       a huge "elapsed time" every frame and enters a catch-up loop (20->1 fps)
 *   (c) only 16.67 ms per frame -> the frame tempo is right BUT code that waits
 *       on time without presenting frames (the video player) enters an
 *       INFINITE LOOP: measured, timeGetTime was called 231 million times.
 *   (d) CHOSEN: 16.67 ms per frame + advance slowly if the clock is queried
 *       many times and no frame arrives.  This keeps the tempo right and
 *       guarantees that loops waiting on time make progress.            */
static _Atomic uint64_t g_vclock_base  = 0;   /* virtual time at the start of the frame */
static _Atomic uint64_t g_vclock_creep = 0;   /* busy-wait compensation     */
static _Atomic uint64_t g_real_base    = 0;   /* real time of that frame    */
static _Atomic uint32_t g_vclock_polls = 0;

static uint64_t real_us(void){
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

/* Virtual clock.
 * BETWEEN frames it advances exactly 16.667 ms: animation speed stays
 * independent of the emulator's speed.  WITHIN a frame it flows smoothly,
 * proportional to real time -- because the game measures the frame time by
 * sampling the clock TWICE within a frame and taking the difference
 * (0x005812e5).  The old clock that jumped in one step made it say "the frame
 * took zero", so the front-end tick [0x8651a8] never increased and all
 * animations/timed transitions froze. */
/* Measure the ratio of virtual time to real time: clock changes must not
 * speed the game up (it had previously run at 2.01x). */
static _Atomic uint64_t g_vt_last = 0, g_rt_start = 0, g_vt_start = 0;
void vclock_report(void){
    uint64_t v0 = atomic_load(&g_vt_start), r0 = atomic_load(&g_rt_start);
    uint64_t v1 = atomic_load(&g_vt_last),  r1 = real_us();
    if (!r0 || r1 <= r0) return;
    fprintf(stderr, "  virtual clock     : %.3f x real time (%.1f s virtual / %.1f s real)\n",
            (double)(v1 - v0) / (double)(r1 - r0),
            (double)(v1 - v0)/1e6, (double)(r1 - r0)/1e6);
}
void vclock_tick(void){
    uint64_t cr = atomic_exchange(&g_vclock_creep, 0ull);
    uint64_t now = real_us();
    /* Advance per frame.
     *   VCLOCK=fixed : exactly 16.667 ms per frame (old behavior).  When the
     *                  emulator drops below 60 fps the GAME GOES SLOW MOTION --
     *                  and since the music is also played from the game's clock,
     *                  its pitch drops.  The user's "the music slows down when
     *                  fps drops" was exactly this (measured: at 50 fps
     *                  virtual/real = 0.885, i.e. ~2 semitones flat).
     *   VCLOCK=real  : the REAL elapsed time (default).  The game and the music
     *                  both flow in real time.  To keep it out of the catch-up
     *                  loop, the per-frame increment is capped at 50 ms. */
    /*   VCLOCK=<us>  : exactly <us> every frame (MEASUREMENT: with VCLOCK=8333 +
     *                  NOVSYNC=1 the game runs in 120 fps time but the limiter
     *                  never waits; fps = GPU/CPU capacity, the route is the
     *                  same frame by frame). */
    { static _Atomic int mode = -1; static uint64_t fstep = 16667ull;
      int m = atomic_load(&mode);
      if (m < 0){ const char *e = ENVC("VCLOCK");
                  m = (e && *e == 'f') ? 0 : 1;
                  if (e && *e >= '1' && *e <= '9'){ fstep = strtoull(e, 0, 10); if (fstep < 1000ull) fstep = 1000ull; m = 0; }
                  atomic_store(&mode, m); }
      uint64_t rb = atomic_load(&g_real_base);
      uint64_t step = fstep;
      if (m && rb && now > rb){
          step = now - rb;
          if (step < 4000ull)  step = 4000ull;    /* above 250 fps: clamp */
          if (step > 50000ull) step = 50000ull;   /* huge dt -> catch-up loop */
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
    /* The ceiling on WITHIN-frame advance.  16 ms was TOO TIGHT: code waiting for
     * more than 16 ms of GAME time within a frame (the pre-race cutscene) hit the
     * ceiling and could only advance through the safety valve below (after
     * 200 ms) -> every frame necessarily took ~235 ms (4.6 fps, measured).
     * Removing the ceiling entirely doesn't work either: the game sees the time
     * really elapsed and enters the catch-up loop.  A three-frame budget is a
     * good middle ground. */
    { static _Atomic uint64_t maxd = 0;
      uint64_t m = atomic_load(&maxd);
      if (!m){ const char *e = ENVC("VCLOCK_MAXD");
               m = (e && *e) ? (uint64_t)strtoull(e,0,0) : 50000ull;
               if (m < 16000ull) m = 16000ull;
               atomic_store(&maxd, m); }
      if (d > m) d = m; }

    /* Safety valve against loops that spin on the clock without producing frames.
     *
     * CAUTION: the criterion used to be the POLL COUNT (>2000 queries per frame ->
     * 400 us every 64 queries, 2 ms above 20000...).  More than 20 thousand clock
     * queries in a busy race frame is NORMAL; this added ~100 ms of fake time
     * per frame.  Measured: 2.04 s virtual in 1.01 s real (ratio 2.01) --
     * cutscenes and races ran twice as fast.  Now the criterion is REAL TIME: it
     * only kicks in if the frame took longer than 200 ms (i.e. really stalled)
     * and adds at most 100 ms per frame. */
    uint32_t n = atomic_fetch_add(&g_vclock_polls, 1);
    uint64_t stalled = (now > rb) ? (now - rb) : 0ull;      /* unclamped */
    /* Safety valve: threshold 200 ms -> 60 ms, step 2 ms -> 4 ms.  Since the
     * ceiling was raised, this path now only runs on REAL stalls. */
    if (stalled > 60000ull && (n & 15u) == 15u){
        if (atomic_load(&g_vclock_creep) < 200000ull)
            atomic_fetch_add(&g_vclock_creep, 4000ull);
    }
    return base + d + atomic_load(&g_vclock_creep);
}
uint32_t host_ms(void){ return (uint32_t)(host_us() / 1000ull) + 1000u; }
uint64_t host_qpc(void){ return host_us(); }

/* the guest heap itself lives in gheap.c (free lists + quarantine) */
uint32_t gheap_alloc(uint32_t n, int zero);
int      gheap_free(uint32_t u, const char *who);
uint32_t gheap_realloc(uint32_t o, uint32_t n, int zero, int in_place_only);
uint32_t gheap_size(uint32_t u);
uint32_t gheap_top_used(void);
uint64_t gheap_failed(void);
void     gheap_report(void);
#define galloc gheap_alloc
#define gsize  gheap_size
uint32_t win32_block_size(uint32_t u){ return gsize(u); }
uint32_t win32_heap_used(void){ return gheap_top_used(); }

/* --- tracing --- */
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

static uint32_t ARG(CPU*c,int i){ return MR32(c->esp + 4u*(uint32_t)i); }  /* after the ret address is POPped */

/* QPCWHO=1: who (thread, return address, one frame up) calls
 * QueryPerformanceCounter.  ~21 thousand calls per frame were seen at 120 fps
 * (~2 thousand at 60 fps with PACE60): a time-based wait loop.  The call is
 * under the GIL, the table is lock-free. */
typedef struct { uint32_t tid, ra, ra2; uint64_t n; } QpcWho;
static QpcWho g_qw[128];
static void qpc_report(void){
    fprintf(stderr, "\n=== QPCWHO: QueryPerformanceCounter callers ===\n");
    for (int k = 0; k < 128; k++){
        int b = -1;
        for (int i = 0; i < 128; i++) if (g_qw[i].n && (b < 0 || g_qw[i].n > g_qw[b].n)) b = i;
        if (b < 0 || k >= 24) break;
        fprintf(stderr, "  %12llu  thread %5u  return 0x%08x  parent 0x%08x\n",
                (unsigned long long)g_qw[b].n, g_qw[b].tid, g_qw[b].ra, g_qw[b].ra2);
        g_qw[b].n = 0;
    }
}
/* ---- The game's frame wait loop: sleep instead of spinning ----
 * sub_0057eae0 (0x57eb5e..0x57eb8e), if the frame time dt is below the target:
 *     start = GetTicks(); target = (X - dt) * k;
 *     do { elapsed = Diff(start, GetTicks()); } while (elapsed < target);
 * GetTicks = sub_0043bdf0 = QPC >> [0x8284e4]; Diff = sub_0043be20 =
 * (now - start) * 2^S * [0x828028] seconds.  The loop only looks at the clock;
 * at 120 fps ~19 thousand QPCs per frame and ALL of the game thread's idle time
 * (8.2 ms CPU per frame, 98%) burned here: every ms won in D3DX went here, and
 * power didn't change.
 * On a QPC coming from the loop (return 0x43bdfe, its caller 0x57eb79) the
 * remaining time is computed with the game's own formula and we sleep for
 * (remaining - margin); the final margin is still waited out by spinning.  Since
 * the exit condition and the clock are the same, the game's timing doesn't
 * change, only the CPU goes idle.  The sleep uses kevent NOTE_CRITICAL
 * (guest_sleep_ns): nanosleep woke up 447 us late on average, kevent 41 us
 * (98% <= 200 us).
 * SPINSLEEP=<margin us> (default 200), SPINSLEEP=0 turns it off.
 * Stack: after POP32 [esp+0xc] = 0x57eb79, E = esp+0x14 (0057eae0's local
 * base), target float [E+0xc] = [esp+0x20], start = EBX. */
static int g_spin_margin = -1;
static uint64_t g_spin_n, g_spin_us, g_spin_late[7];   /* late wake-up: <=25,50,100,200,400,800,>800 us */
static void spin_report(void){
    fprintf(stderr, "\n=== SPINSLEEP: the frame wait loop slept %llu times, total %.2f s (margin %d us) ===\n",
            (unsigned long long)g_spin_n, g_spin_us / 1e6, g_spin_margin);
    fprintf(stderr, "  late wake-up: <=25us %llu  <=50 %llu  <=100 %llu  <=200 %llu  <=400 %llu  <=800 %llu  >800 %llu\n",
            (unsigned long long)g_spin_late[0], (unsigned long long)g_spin_late[1], (unsigned long long)g_spin_late[2],
            (unsigned long long)g_spin_late[3], (unsigned long long)g_spin_late[4], (unsigned long long)g_spin_late[5],
            (unsigned long long)g_spin_late[6]);
    extern uint64_t g_sleep_late_ns, g_sleep_gil_ns, g_sleep_n;
    if (g_sleep_n) fprintf(stderr, "  mean: nanosleep latency %.0f us, lock wait on wake-up %.0f us\n",
                           g_sleep_late_ns / 1e3 / g_sleep_n, g_sleep_gil_ns / 1e3 / g_sleep_n);
}
uint64_t spin_slept_us(void){ return g_spin_us; }   /* perflog: frame limiter sleep */
static void qpc_spin_sleep(CPU *c){
    if (MR32(c->esp + 0xcu) != GA(0x57eb79u)) return;
    if (g_spin_margin < 0){ const char *e = getenv("SPINSLEEP"); g_spin_margin = e ? atoi(e) : 200;
                         if (g_spin_margin && ENVC("QPCWHO")) atexit(spin_report); }
    if (!g_spin_margin) return;
    /* the game's units: F = [0x828028] per QPC tick (measured 0.001 = ms,
     * k = [0x784550] = 1000), X = [0x865558] = 1/120 s frame target */
    uint32_t S = MR32(GA(0x8284e4u)) & 31u;
    float F = MRF32(GA(0x828028u)), frame_target = MRF32(c->esp + 0x20u);
    if (!(F > 0.0f) || !(frame_target > 0.0f)) return;
    uint64_t now = host_qpc();
    int32_t d = (int32_t)((uint32_t)(now >> S) - c->ebx); if (d < 0) d = 0;
    double rem_time = (double)frame_target - (double)d * (double)(1u << S) * (double)F;
    double us = rem_time / (double)F - (double)g_spin_margin;      /* QPC tick = us (QPF 1 MHz) */
    if (us < 50.0 || us > 100000.0) return;                  /* short: spin; >0.1 s: unexpected */
    void guest_sleep_ns(long ns);
    guest_sleep_ns((long)(us * 1000.0));
    g_spin_n++; g_spin_us += (uint64_t)us;
    { double overshoot = (double)(host_qpc() - now) - us;          /* us slept beyond what was asked */
      int b = overshoot <= 25 ? 0 : overshoot <= 50 ? 1 : overshoot <= 100 ? 2 : overshoot <= 200 ? 3 : overshoot <= 400 ? 4 : overshoot <= 800 ? 5 : 6;
      g_spin_late[b]++; }
}
static void qpc_who(CPU *c){
    static int init = 0; static uint64_t tot = 0;
    if (!init){ init = 1; atexit(qpc_report); }
    if ((++tot & 0xFFFFFu) == 0){ long metal_frames(void);
        fprintf(stderr, "  [qpc] %llu M calls, frame %ld\n", (unsigned long long)(tot >> 20), metal_frames()); }
    uint32_t tid = (c->fs_base && mem_is_committed(c->fs_base+0x24u)) ? MR32(c->fs_base+0x24u) : 0u;
    uint32_t ra = c->eip;                       /* after POP32: the return address */
    /* sub_0043bdf0 (the game's time wrapper): sub esp,8; push arg; call.
     * After POP32 [esp]=arg, [esp+4..0xb]=locals, [esp+0xc]=its caller. */
    uint32_t ra2 = ra == GA(0x43bdfeu) ? MR32(c->esp+0xcu)
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
    guest_fair_yield();          /* give the other guest threads a turn */
    POP32(c->eip);                      /* take the return address */
    int handled = 1;
    const char *n = e->name;
    uint32_t r = 0;

    /* QueryPerformanceCounter is the most frequent call (the frame limiter); to
     * skip the ~20 strcmps of the name chain, the import index is cached on the
     * first match. */
    static uint32_t qpc_idx = 0xFFFFFFFFu;
    if (idx == qpc_idx || (qpc_idx == 0xFFFFFFFFu && !strcmp(n,"QueryPerformanceCounter") && (qpc_idx = idx, 1))){
        if (ENVC("QPCWHO")) qpc_who(c);
        if (c->eip == GA(0x43bdfeu)) qpc_spin_sleep(c);
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
              fprintf(stderr,"      [alloc] HeapAlloc(%u) -> 0x%08x   calling guest instruction: 0x%08x\n",
                      sz, r, g_watch_prev_ip);
              guest_backtrace(c, 12); } } }
        /* WATCHALLOC=<size>: set a watchpoint at +16 of the first allocation of that
         * size.  Since the constant block is written only once, this is the only
         * way to catch the write. */
        /* WATCHAT=<address>: set a watchpoint on the allocation returning exactly that address */
        { const char *wa = ENVC("WATCHAT");
          if (wa && r){ static int armed2 = 0;
            uint32_t want3 = (uint32_t)strtoul(wa,0,0);
            if (!armed2 && r == want3){ armed2 = 1;
              void watch_arm(uint32_t);
              const char *wo = ENVC("WATCHOFF");
              uint32_t off3 = wo ? (uint32_t)strtoul(wo,0,0) : 0u;
              fprintf(stderr,"      [alloc] target block 0x%08x (%u bytes) allocated, watching +%u\n",
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
              fprintf(stderr,"      [alloc] %u-byte block 0x%08x allocated; watchpoint at +16\n", sz, r);
              watch_arm(r); } } }
        static int shown=0;
        if (ENVC("VERBOSE") && shown<2 && sz>0x100000u){ shown++;
            fprintf(stderr,"      [heap] HeapAlloc(size=0x%08x) SUSPICIOUS. ebp=0x%08x esp=0x%08x\n", sz, c->ebp, c->esp);
            fprintf(stderr,"        stack (from esp):");
            for(int k=0;k<14;k++) fprintf(stderr," %s%08x", k?"":"", MR32(c->esp+4u*k));
            fprintf(stderr,"\n        around ebp    :");
            for(int k=-4;k<=6;k++) fprintf(stderr," %s%08x", k==0?">":"", MR32(c->ebp+4u*(uint32_t)k));
            fprintf(stderr,"\n        esi=%08x edi=%08x ebx=%08x\n", c->esi,c->edi,c->ebx); }
    }
    else if (!strcmp(n,"HeapReAlloc")){
        /* (heap, flags, mem, bytes); flags 8 = ZERO_MEMORY, 0x10 = REALLOC_IN_PLACE_ONLY */
        r = gheap_realloc(ARG(c,2), ARG(c,3), ARG(c,1)&8u, ARG(c,1)&0x10u); }
    else if (!strcmp(n,"HeapFree")){ gheap_free(ARG(c,2), "HeapFree"); r = 1; }
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
    else if (!strcmp(n,"GlobalFree")||!strcmp(n,"LocalFree")){ gheap_free(ARG(c,0), n); r = 0; }
    else if (!strcmp(n,"GlobalLock")||!strcmp(n,"LocalLock")) r = ARG(c,0);
    else if (!strcmp(n,"GlobalUnlock"))           r = 1;
    else if (!strcmp(n,"VirtualFree")){          /* (addr, size, type): only MEM_RELEASE frees */
        if (ARG(c,2) & 0x8000u) gheap_free(ARG(c,0), n);
        r = 1; }
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
    /* Critical sections are NOT HANDLED here; they are implemented in win32_ext.c
     * with REAL recursive locks (see cs_for).  If left as a no-op stub, the GIL's
     * fair sharing breaks mutual exclusion. */
    else if (!strcmp(n,"GetCurrentThreadId")){
        /* the real thread id from the TEB (fs:[0x24]); every thread must be unique */
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
        fprintf(stderr,"\n*** guest ExitProcess(%u) - terminating\n",ARG(c,0));
        guest_exit_now(); }
    else if (!strcmp(n,"LoadLibraryA")||!strcmp(n,"LoadLibraryW")){
        r = 0x20000000u;
        { static int sh=0; uint32_t p2=ARG(c,0);
          if (ENVC("VERBOSE") && sh<12){ sh++; fprintf(stderr,"      [dll] LoadLibrary(ptr=0x%08x) \"%s\"\n", p2, p2?(char*)(g_mem+p2):"(NULL)"); } } }
    else if (!strcmp(n,"GetProcAddress")){
        uint32_t pname = ARG(c,1); r = 0;
        if (pname > 0xffff){                       /* by name (not ordinal) */
            const char *want = (const char*)(g_mem + pname);
            for (uint32_t i=0;i<g_imp_count;i++)
                if (!strcmp(g_imp[i].name, want)){ r = IMP_MAGIC + i; break; }
            { static int sh=0;
              if (ENVC("VERBOSE") && !r && sh<12){ sh++; fprintf(stderr,"      [dll] GetProcAddress(\"%s\") NOT FOUND\n", want); } }
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
        fprintf(stderr,"\n*** GUEST MESSAGEBOX ***\n  title: %s\n  text : %s\n***\n",
                cap?(char*)(g_mem+cap):"(none)", txt?(char*)(g_mem+txt):"(none)");
        r = 1; }
    else if (!strcmp(n,"Direct3DCreate9")) r = com_direct3dcreate9();
    else if (!strcmp(n,"DirectInput8Create")) r = com_directinput8create(ARG(c,3));
    else if (!strcmp(n,"DirectSoundCreate") || (!strcmp(n,"#1") && !strcasecmp(e->dll,"DSOUND.dll")))
        r = com_directsoundcreate(ARG(c,1));
    else if (!strcmp(n,"SetConsoleCtrlHandler")) r = 1;
    else if (!strcmp(n,"RtlUnwind"))             r = 0;
    else if (!strcmp(n,"TerminateProcess")){ g_exit_code=(int)ARG(c,1);
        fprintf(stderr,"\n*** guest TerminateProcess(%u) - terminating\n",ARG(c,1));
        guest_exit_now(); }
    else if (fileio_call(c, n, &r)) { /* file I/O */ }
    else if (win32_ext_call(c, n, &r)) { /* second-round layer */ }
    else { handled = 0; g_win32_unimpl++; stat_[idx].unimpl++;
           if (getenv("FILEOPLOG"))
               fprintf(stderr, "  [stub] %s (return 0x%08x)\n", n, MR32(c->esp));
           if (unimpl_first==0xffffffffu){ unimpl_first=idx; unimpl_name=n; } }

    if (g_bt_on && (!strcmp(n,"GetModuleFileNameA")||!strcmp(n,"ExitProcess")||!strcmp(n,"RaiseException")))
        guest_backtrace(c, 14);
    if (trace_n < trace_max)
        fprintf(stderr, "  [%4u] %-14s %-32s (%d arg) %s-> 0x%08x\n",
                ++trace_n, e->dll, n, e->nargs, handled?"":"[STUB] ", r);
    else trace_n++;

    c->eax = r;
    c->esp += 4u * (uint32_t)e->nargs;      /* stdcall: the callee cleans up the arguments */
    return 1;
}

void win32_init(void){
    stat_ = calloc(g_imp_count, sizeof *stat_);
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    tick0 = (uint64_t)ts.tv_sec*1000 + ts.tv_nsec/1000000;
    mem_commit(HEAP_BASE, 0x10000);
    g_cmdline = galloc(64,1);
    strcpy((char*)(g_mem+g_cmdline), "SPEED2.EXE");
    /* fill the IAT with magic addresses */
    for (uint32_t i=0;i<g_imp_count;i++) MR32(g_imp[i].iat) = IMP_MAGIC + i;
    com_init();
    /* addresses to be read later for diagnosing the CRT heap mode */
    fprintf(stderr,"win32: %u imports bound to the IAT, heap 0x%08x\n", g_imp_count, HEAP_BASE);
}

void win32_report(void){
    fprintf(stderr,"\n=== WIN32 KATMANI ===\n");
    fprintf(stderr,"  total import calls    : %llu\n",(unsigned long long)g_win32_calls);
    fprintf(stderr,"  not implemented       : %llu\n",(unsigned long long)g_win32_unimpl);
    fprintf(stderr,"  heap usage            : %u KB   failed allocations: %llu\n",
            win32_heap_used()/1024u, (unsigned long long)gheap_failed());
    gheap_report();
    /* the most called unimplemented ones */
    fprintf(stderr,"  guest threads : %llu started, %llu terminated\n",
            (unsigned long long)win32_ext_threads(),(unsigned long long)win32_ext_threads_exited());
    fileio_report();
    { extern void res_report(void); res_report(); }
    { extern void overrides_report(void); overrides_report(); }
    enh_report();
    { extern void d3dx_hle_report(void); d3dx_hle_report(); }
    fprintf(stderr,"  --- NOT IMPLEMENTED (called, stub returned) ---\n");
    for (uint32_t i=0;i<g_imp_count;i++)
        if (stat_[i].unimpl)
            fprintf(stderr,"    %-14s %-32s %u times\n", g_imp[i].dll, g_imp[i].name, stat_[i].unimpl);
    fprintf(stderr,"  --- 15 MOST CALLED ---\n");
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
            /* we don't know whether they were handled; just show the frequently called ones */
        }
    for (uint32_t i=0;i<g_imp_count;i++)
        if (stat_[i].calls>1 && shown<15){ fprintf(stderr,"    %-30s %u times\n", g_imp[i].name, stat_[i].calls); shown++; }
}
