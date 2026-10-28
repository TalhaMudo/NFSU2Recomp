/* Win32 layer - second round: registry, window, synchronization, threads */
#include <strings.h>
#include "cpu.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <sys/event.h>
#include <sched.h>

void dispatch_indirect(CPU*,uint32_t);
void mem_commit(uint32_t,uint32_t);
uint32_t win32_alloc(uint32_t n, int zero);
uint32_t res_find(uint32_t,uint32_t,int);
uint32_t res_data(uint32_t);
uint32_t res_size(uint32_t);

/* ---- object table (HANDLE -> type/state) ---- */
#define H_BASE 0x00A00000u
typedef enum { OB_NONE=0, OB_EVENT, OB_MUTEX, OB_THREAD, OB_FILE, OB_KEY, OB_SEM } ObKind;
typedef struct {
    ObKind kind; int signaled; int manual;
    pthread_mutex_t mtx; pthread_cond_t cv;
    uint32_t guest_arg, guest_entry, tid;
    uint32_t owner_tid; int rec;      /* MUTEX: ownership + recursion */
    pthread_t th; int started; void *pending;
    volatile int terminate;           /* THREAD: TerminateThread was called on it */
    char name[64];
} Obj;
#define OBJ_MAX 512
static Obj g_obj[OBJ_MAX];
static int g_obj_n = 1;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
typedef struct { uint64_t waits, sets; uint32_t waiter_tid, setter_tid; } WStat;
static WStat g_wstat[64];

/* ---- BIG GUEST LOCK ----------------------------------------------------
 * Guest code runs on only one thread at a time. This keeps the guest
 * memory/allocators shared by the lifted code safe and the instruction trace
 * consistent. The lock is released only in blocking calls (Sleep/Wait*).  */
static pthread_mutex_t g_guest = PTHREAD_MUTEX_INITIALIZER;
extern CPU *g_cur_cpu;
static _Thread_local CPU *tl_cpu = 0;
static _Thread_local void *tl_obj = 0;   /* this thread's object */
/* Wait semantics.  Waits used to give up early: events after 2 s even for INFINITE, multi-object
 * waits after 200 ms.  A game thread that waits for an event during a long load (the NIS
 * audio stream thread, for instance) then saw a bogus WAIT_TIMEOUT.  NFSU2_WAIT_LEGACY=1 restores
 * the old behaviour. */
static int wait_legacy(void){ static int v=-1; if (v<0){ const char *e=getenv("NFSU2_WAIT_LEGACY"); v = (e && *e && *e!='0'); } return v; }
static uint64_t wait_now_ms(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000u + (uint64_t)t.tv_nsec/1000000u; }
static int wait_thread_killed(void);

/* CPU state of all guest threads: to be able to dump the stack traces of ALL
 * of them at stall time (a single thread's can be misleading). */
#define MAXCPUS 32
CPU *g_all_cpus[MAXCPUS];
uint32_t g_all_tids[MAXCPUS];
int g_all_n = 0;
static void reg_cpu(CPU *c){
    for (int i=0;i<g_all_n;i++) if (g_all_cpus[i]==c) return;
    if (g_all_n < MAXCPUS){
        g_all_cpus[g_all_n] = c;
        g_all_tids[g_all_n] = c->fs_base ? MR32(c->fs_base+0x24) : 0;
        g_all_n++;
    }
}
/* --- measuring the HOLD TIME of the big lock (GIL) ---------------------
 * A guest thread doesn't release the GIL as long as it makes no Win32 call; a
 * long computation (example: decoding the audio stream) starves the other
 * threads.  The audio writes ~50 ms of silence every 30 s -- we measure
 * whether this is the culprit.  GILLOG=<ms> reports every hold above the
 * threshold. */
static uint64_t gil_now_us(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec*1000000ull + (uint64_t)ts.tv_nsec/1000ull;
}
static _Thread_local uint64_t tl_gil_t0 = 0;
uint64_t g_gil_max_us = 0, g_gil_long = 0;
static int gil_thr_us(void){
    static int v = -1;
    if (v < 0){ const char *e = getenv("GILLOG"); v = (e && *e) ? atoi(e)*1000 : 0; }
    return v;
}
static void gil_took(void){ tl_gil_t0 = gil_now_us(); }
static void gil_gave(void){
    if (!tl_gil_t0) return;
    uint64_t d = gil_now_us() - tl_gil_t0;
    tl_gil_t0 = 0;
    if (d > g_gil_max_us) g_gil_max_us = d;
    int thr = gil_thr_us();
    if (thr && d > (uint64_t)thr){
        g_gil_long++;
        static int shown = 0;
        if (shown < 80){ shown++;
            double audio_time(void);
            fprintf(stderr, "  [GIL] t=%.3f s  lock held %.1f ms  guest eip=%08x\n",
                    audio_time(), d/1000.0, tl_cpu ? tl_cpu->eip : 0u);
        }
    }
}
void guest_lock(CPU *c){ pthread_mutex_lock(&g_guest); tl_cpu=c; g_cur_cpu=c; reg_cpu(c); gil_took(); }
void guest_unlock(void){ gil_gave(); pthread_mutex_unlock(&g_guest); }
static void guest_yield_begin(void){ gil_gave(); pthread_mutex_unlock(&g_guest); }
static void guest_thread_die(void);
static void guest_yield_end(void){ pthread_mutex_lock(&g_guest); g_cur_cpu = tl_cpu; gil_took();
    if (tl_obj && ((Obj*)tl_obj)->terminate) guest_thread_die(); }
/* Sleep while releasing the big lock (win32.c: the game's frame wait loop). */
uint64_t g_sleep_late_ns, g_sleep_gil_ns, g_sleep_n;    /* diagnostics: sleep latency / lock wait */
/* On macOS nanosleep wakes up with a timer coalescing margin: in the game a
 * 3 ms request is on average 447 us late (separate measurement: median ~770 us,
 * QoS makes no difference).  A kevent timer with NOTE_CRITICAL isn't
 * coalesced: median ~45 us. */
static void sleep_precise_ns(long ns){
    static _Thread_local int kq = -1;
    if (kq < 0) kq = kqueue();
    if (kq >= 0){
        struct kevent ev, out;
        EV_SET(&ev, 1, EVFILT_TIMER, EV_ADD | EV_ONESHOT, NOTE_NSECONDS | NOTE_CRITICAL, ns, 0);
        if (kevent(kq, &ev, 1, &out, 1, NULL) == 1) return;
    }
    struct timespec ts = { (time_t)(ns / 1000000000L), ns % 1000000000L };
    nanosleep(&ts, 0);
}
void guest_sleep_ns(long ns){
    struct timespec a, b, d;
    guest_yield_begin();
    clock_gettime(CLOCK_MONOTONIC, &a); sleep_precise_ns(ns); clock_gettime(CLOCK_MONOTONIC, &b);
    guest_yield_end();
    clock_gettime(CLOCK_MONOTONIC, &d);
    int64_t sl = (int64_t)(b.tv_sec - a.tv_sec) * 1000000000LL + (b.tv_nsec - a.tv_nsec) - ns;
    int64_t gl = (int64_t)(d.tv_sec - b.tv_sec) * 1000000000LL + (d.tv_nsec - b.tv_nsec);
    g_sleep_late_ns += sl > 0 ? (uint64_t)sl : 0; g_sleep_gil_ns += (uint64_t)gl; g_sleep_n++;
}

/* --- frame rate locking (60 Hz) ---------------------------------------
 * The virtual clock advances EXACTLY 16.667 ms per frame, so the game speed =
 * (real fps / 60).  When the emulator did 117 fps the game ran at 195% speed.
 * Locking the frame to 60 Hz makes the speed independent of FPS.
 * The big lock is RELEASED during the sleep, otherwise the worker threads stop. */
/* TIMELOG=1: the virtual/real time ratio and frame count once a second.
 * For measuring the complaint "the game speeds up needlessly in cutscenes" --
 * if the ratio goes above 1.0 the game really is running fast. */
static void timelog_tick(void){
    static int on = -1;
    if (on < 0){ const char *e = getenv("TIMELOG"); on = (e && *e && *e != '0'); }
    if (!on) return;
    uint64_t host_us(void);
    static uint64_t lr = 0, lv = 0; static long lf = 0;
    long metal_frames(void);
    uint64_t r = gil_now_us(), v = host_us();
    long f = metal_frames();
    if (!lr){ lr = r; lv = v; lf = f; return; }
    if (r - lr < 1000000ull) return;
    fprintf(stderr, "  [time] real %.2f s, virtual %.2f s, ratio %.3f, %ld frames (%.1f fps)\n",
            (r-lr)/1e6, (v-lv)/1e6, (double)(v-lv)/(double)(r-lr),
            f-lf, (f-lf)*1e6/(double)(r-lr));
    lr = r; lv = v; lf = f;
}

void frame_pace(void){
    timelog_tick();
    /* DEFAULT: NO LIMITER.  The 60 Hz rationale above NO LONGER HOLDS --
     * since the virtual clock switched to VCLOCK=real, the game speed is
     * independent of the real fps (measured: virtual/real = 0.999 at both
     * 63.7 fps and 99 fps).  With the limiter on, 61% of the race frame was
     * nanosleep; with it off the race went 63.7 -> 79.4 fps and the menus to
     * 120 fps (the display refresh limit).
     * PACE60=1 brings back the old behavior.  Runaway fps is cut by vsync
     * anyway; the virtual clock's per-frame increment is floored at 4 ms (250 fps). */
    static int off = -1;
    if (off < 0){ const char *e = getenv("PACE60");
                  off = !(e && *e && *e != '0'); }
    if (off) return;
    static uint64_t deadline = 0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec*1000000ull + (uint64_t)ts.tv_nsec/1000ull;
    if (!deadline || now > deadline + 200000ull) deadline = now;   /* far behind: reset */
    deadline += 16667ull;
    if (now < deadline){
        uint64_t d = deadline - now;
        struct timespec sl = { (time_t)(d/1000000ull), (long)((d%1000000ull)*1000ull) };
        guest_yield_begin();
        nanosleep(&sl, 0);
        guest_yield_end();
    }
}

/* Fair GIL: guest code can also spin in non-blocking Win32 calls
 * (example: a wait loop spinning on EnterCriticalSection). We periodically
 * release the lock and give the other guest threads a turn.            */
/* ---- REAL critical sections -------------------------------------------
 * EnterCriticalSection/LeaveCriticalSection used to be no-op stubs.  The single
 * big lock (GIL) covered for them most of the time, BUT guest_fair_yield
 * releases the GIL every 1024 Win32 calls: a thread could lose the lock while
 * INSIDE a critical section and another thread could enter the same section.
 * The game's loader/audio/video queues get silently corrupted that way.
 *
 * The GIL MUST BE RELEASED while waiting for the lock, otherwise deadlock.  */
#define MAXCS 512
static struct { uint32_t key; pthread_mutex_t m; int used; } g_cs[MAXCS];
static pthread_mutex_t g_cs_tab = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local int tl_cs_depth = 0;
uint64_t g_cs_enter = 0, g_cs_contended = 0;

static int cs_disabled(void){
    static int v = -1;
    if (v < 0){ const char *e = getenv("NOCS"); v = (e && *e && *e != '0') ? 1 : 0; }
    return v;
}
static pthread_mutex_t *cs_for(uint32_t key){
    if (cs_disabled()) return 0;          /* old behavior: no-op stub */
    pthread_mutex_lock(&g_cs_tab);
    unsigned h = (key >> 2) % MAXCS;
    for (unsigned i = 0; i < MAXCS; i++){
        unsigned j = (h + i) % MAXCS;
        if (g_cs[j].used && g_cs[j].key == key){
            pthread_mutex_unlock(&g_cs_tab); return &g_cs[j].m;
        }
        if (!g_cs[j].used){
            pthread_mutexattr_t a;
            pthread_mutexattr_init(&a);
            pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
            pthread_mutex_init(&g_cs[j].m, &a);
            pthread_mutexattr_destroy(&a);
            g_cs[j].key = key; g_cs[j].used = 1;
            pthread_mutex_unlock(&g_cs_tab); return &g_cs[j].m;
        }
    }
    pthread_mutex_unlock(&g_cs_tab);
    return 0;                      /* table full: fall back to the old behavior */
}

uint32_t call_guest(CPU *c, uint32_t fn, int nargs, const uint32_t *args);
int metal_take_char(int remove);            /* metal.m: WM_CHAR queue */
static uint32_t g_wndproc = 0;
static uint32_t g_hwnd    = 0x00B00001u;
#define WMQ 8
static struct { uint32_t msg, wp, lp; } g_wmq[WMQ];
static int g_wmq_head = 0, g_wmq_tail = 0;
static int g_wm_activated = 0;
uint64_t g_wm_sent = 0, g_wm_dispatched = 0;

static void wm_push(uint32_t m, uint32_t wp, uint32_t lp){
    int nx = (g_wmq_tail + 1) % WMQ;
    if (nx == g_wmq_head) return;
    g_wmq[g_wmq_tail].msg = m; g_wmq[g_wmq_tail].wp = wp; g_wmq[g_wmq_tail].lp = lp;
    g_wmq_tail = nx;
}
static int wm_pop(uint32_t *m, uint32_t *wp, uint32_t *lp){
    if (g_wmq_head == g_wmq_tail) return 0;
    *m = g_wmq[g_wmq_head].msg; *wp = g_wmq[g_wmq_head].wp; *lp = g_wmq[g_wmq_head].lp;
    g_wmq_head = (g_wmq_head + 1) % WMQ;
    return 1;
}

static uint32_t cur_tid(CPU *c){
    return (c && c->fs_base && mem_is_committed(c->fs_base+0x24u))
           ? MR32(c->fs_base+0x24u) : 0u;
}

void guest_fair_yield(void){
    if (tl_cs_depth) return;          /* yielding inside a critical section */
    static _Thread_local uint32_t n = 0;
    if ((++n & 0x3FFu) != 0) return;          /* once every 1024 calls */
    guest_yield_begin();
    sched_yield();
    /* CAUTION: there used to be a nanosleep(50us) here every time.  On macOS a
     * sleep of 50 us really takes ~1-2 ms (scheduler quantum + timer
     * coalescing).  In the pre-race cutscene the guest calls a Win32 function
     * ~110 thousand times per frame; that meant ~110 sleeps = ~165 ms per frame.
     * Measured: 70% of the guest thread was in __semwait_signal, i.e. SLEEP,
     * not computation.  Frame time 235 ms (4.6 fps).  sched_yield already
     * provides fairness; we thin out the sleeps (only to guarantee the other
     * threads make progress). */
    if ((n & 0x7FFFu) == 0){
        struct timespec ts = {0, 50000L}; nanosleep(&ts, 0);
    }
    guest_yield_end();
}

static uint32_t obj_new(ObKind k){
    pthread_mutex_lock(&g_lock);
    int i = g_obj_n < OBJ_MAX ? g_obj_n++ : 0;
    pthread_mutex_unlock(&g_lock);
    if (!i) return 0;
    Obj *o=&g_obj[i]; memset(o,0,sizeof *o); o->kind=k;
    pthread_mutex_init(&o->mtx,0); pthread_cond_init(&o->cv,0);
    return H_BASE + (uint32_t)i;
}
/* for fileio.c to signal the overlapped completion event. */
void win32_signal_event(uint32_t h);

static Obj *obj_get(uint32_t h){
    if (h < H_BASE || h >= H_BASE+OBJ_MAX) return 0;
    uint32_t i = h - H_BASE;
    return g_obj[i].kind ? &g_obj[i] : 0;
}

/* ---- guest thread ---- */
#define GTHREAD_STACK 0x00100000u          /* 1 MB */
/* 16 stacks at 0x7E000000..0x7F000000, below the main stack (boot.c STACK_LO);
 * the game starts 8 threads.  (They were at 0x60000000, now the buffer heap's.) */
#define GTHREAD_TOP   0x7F000000u
static uint32_t g_tstack_next = 0x7E000000u;
uint64_t g_threads_started = 0, g_threads_exited = 0;

typedef struct { uint32_t entry, arg, teb; Obj *ob; } TArg;

void mem_altstack_install(void);
/* Guest threads need a BIG stack: in lifted code every guest call is a host C
 * call, and lifted frames are wide.  The main guest thread (tests/boot.c) got
 * 1 GB, but the worker threads the game opens with CreateThread got the macOS
 * default of 512 KB via `pthread_create(...,0,...)` -> a HOST STACK OVERFLOW in
 * deep recursion (SIGSEGV, an address outside guest space).  The reservation
 * is virtual address space; it is only committed as it gets used. */
/* Guest thread stacks: 1 MB slots in a small area (16 of them).  The game starts worker
 * threads at every race load, and TerminateThread used to do nothing, so they piled up and
 * the 16 slots ran out after ~9 races ("no room for another guest thread stack", exit 7).
 * Now slots are recycled when a thread ends, and TerminateThread really ends the thread
 * (at its next return from a wait/sleep, where it gets the guest lock back). */
static uint32_t g_tfree[64]; static int g_tfree_n = 0;
static _Thread_local uint32_t tl_stack_lo = 0;
static uint32_t tstack_get(void){
    uint32_t lo = 0;
    pthread_mutex_lock(&g_lock);
    if (g_tfree_n > 0) lo = g_tfree[--g_tfree_n];
    else if (g_tstack_next + GTHREAD_STACK <= GTHREAD_TOP){ lo = g_tstack_next; g_tstack_next += GTHREAD_STACK; }
    pthread_mutex_unlock(&g_lock);
    return lo;
}
static void tstack_put(uint32_t lo){
    if (!lo) return;
    pthread_mutex_lock(&g_lock);
    if (g_tfree_n < 64) g_tfree[g_tfree_n++] = lo;
    pthread_mutex_unlock(&g_lock);
}
static int wait_thread_killed(void){ return tl_obj && ((Obj*)tl_obj)->terminate; }
static void guest_thread_die(void){            /* called WITH the guest lock held */
    Obj *o = (Obj*)tl_obj;
    if (o){ pthread_mutex_lock(&o->mtx); o->signaled=1;
            pthread_cond_broadcast(&o->cv); pthread_mutex_unlock(&o->mtx); }
    { static int sh = 0; if (sh++ < 200)
        fprintf(stderr, "  [thread] tid=%u ends, stack slot %08x recycled\n", o ? o->tid : 0u, tl_stack_lo); }
    tstack_put(tl_stack_lo); tl_stack_lo = 0;
    __sync_fetch_and_add(&g_threads_exited, 1);
    guest_unlock();
    pthread_exit(0);
}

static int guest_pthread_create(pthread_t *th, void *(*fn)(void*), void *arg){
    pthread_attr_t at;
    if (pthread_attr_init(&at) != 0) return pthread_create(th, 0, fn, arg);
    pthread_attr_setstacksize(&at, 256ull*1024ull*1024ull);
    int rc = pthread_create(th, &at, fn, arg);
    pthread_attr_destroy(&at);
    if (rc != 0) rc = pthread_create(th, 0, fn, arg);   /* last resort */
    return rc;
}

static void *guest_thread_main(void *p){
    mem_altstack_install();
    TArg *t = (TArg*)p;
    CPU cpu; memset(&cpu,0,sizeof cpu);
    uint32_t stack_lo, stack_hi;
    stack_lo = tstack_get();
    if (!stack_lo){
        fprintf(stderr, "  [thread] no room for another guest thread stack\n"); exit(7); }
    stack_hi = stack_lo + GTHREAD_STACK; tl_stack_lo = stack_lo;
    mem_commit(stack_lo, GTHREAD_STACK);
    memset(g_mem + stack_lo, 0, GTHREAD_STACK);     /* a recycled slot starts clean */
    cpu.esp = stack_hi - 0x1000u;
    cpu.top = 0; cpu.fcw = 0x037f;
    cpu.fs_base = t->teb; cpu.gs_base = t->teb;
    /* TEB */
    MR32(t->teb+0x00)=0xFFFFFFFFu; MR32(t->teb+0x04)=stack_hi; MR32(t->teb+0x08)=stack_lo;
    MR32(t->teb+0x18)=t->teb; MR32(t->teb+0x24)=t->ob?t->ob->tid:0;
    MR32(t->teb+0x2C)=t->teb+0x1000u; MR32(t->teb+0x30)=0x00C10000u;
    /* stdcall: arg, then the return address */
    { CPU *c=&cpu; PUSH32(t->arg); PUSH32(0xDEAD7EADu); }
    __sync_fetch_and_add(&g_threads_started, 1);
    { static int sh=0; if (getenv("VERBOSE") && sh<40){ sh++;
        fprintf(stderr,"      [thread] STARTED entry=0x%08x arg=0x%08x teb=0x%08x tid=%u\n",
                t->entry, t->arg, t->teb, t->ob?t->ob->tid:0); } }
    tl_obj = t->ob;
    /* diagnostics: so it is recognizable in thread lists (perflog thread.csv, sample) */
    { char nm[32]; snprintf(nm, sizeof nm, "guest %u @%06x", t->ob ? t->ob->tid : 0u, t->entry & 0xFFFFFFu);
      pthread_setname_np(nm); }
    guest_lock(&cpu);
    dispatch_indirect(&cpu, t->entry);
    free(t);
    guest_thread_die();                              /* returned normally */
    return 0;
}

void win32_signal_event(uint32_t h){
    Obj *o = obj_get(h);
    if (!o || o->kind != OB_EVENT) return;
    pthread_mutex_lock(&o->mtx);
    o->signaled = 1;
    pthread_cond_broadcast(&o->cv);
    pthread_mutex_unlock(&o->mtx);
}

/* ---- WINMM multimedia timer -------------------------------------------
 * The NFSU2 front end drives its update loop from a timeSetEvent callback; if
 * left as a stub the menu animations don't advance and input isn't processed.
 * The game re-arms the one-shot timer inside the callback, so opening a thread
 * per timer doesn't work: a SINGLE timer thread processes all entries in turn. */
#define MAXTIMER 32
typedef struct {
    uint32_t proc, user, id;
    uint64_t due_us;          /* 0 = empty */
    uint32_t period_us;       /* 0 = one-shot */
} Timer;

static Timer    g_tm[MAXTIMER];
static uint32_t g_timer_next_id = 1;
static int      g_timer_running = 0;
uint64_t        g_timer_ticks = 0;

/* Timers must also use GAME time; otherwise when the emulator is slow the
 * callbacks run far too often relative to the game's own time. */
uint64_t host_us(void);
static uint64_t now_us(void){ return host_us(); }

static void *timer_thread_main(void *p){
    (void)p;
    mem_altstack_install();
    CPU cpu; memset(&cpu,0,sizeof cpu);
    uint32_t stack_lo, stack_hi, teb;
    stack_lo = tstack_get();
    if (!stack_lo){
        fprintf(stderr, "  [thread] no room for another guest thread stack\n"); exit(7); }
    stack_hi = stack_lo + GTHREAD_STACK;
    teb = 0x00C60000u;
    mem_commit(stack_lo, GTHREAD_STACK);
    mem_commit(teb, 0x2000);
    cpu.top = 0; cpu.fcw = 0x037f;
    cpu.fs_base = teb; cpu.gs_base = teb;
    MR32(teb+0x00)=0xFFFFFFFFu; MR32(teb+0x04)=stack_hi; MR32(teb+0x08)=stack_lo;
    MR32(teb+0x18)=teb; MR32(teb+0x24)=999u;
    MR32(teb+0x2C)=teb+0x1000u; MR32(teb+0x30)=0x00C10000u;

    for (;;){
        struct timespec ts = {0, 1000000L};      /* 1 ms */
        nanosleep(&ts, 0);
        uint64_t now = now_us();
        for (int i = 0; i < MAXTIMER; i++){
            uint32_t proc, user, id;
            pthread_mutex_lock(&g_lock);
            if (!g_tm[i].due_us || g_tm[i].due_us > now){ pthread_mutex_unlock(&g_lock); continue; }
            proc = g_tm[i].proc; user = g_tm[i].user; id = g_tm[i].id;
            if (g_tm[i].period_us) g_tm[i].due_us = now + g_tm[i].period_us;
            else                   g_tm[i].due_us = 0;      /* a one-shot ends */
            pthread_mutex_unlock(&g_lock);

            cpu.esp = stack_hi - 0x1000u;
            { CPU *c=&cpu;
              /* stdcall: dw2, dw1, dwUser, uMsg, uTimerID  (right to left) */
              PUSH32(0u); PUSH32(0u); PUSH32(user); PUSH32(0u); PUSH32(id);
              PUSH32(0xDEAD7EADu); }
            guest_lock(&cpu);
            dispatch_indirect(&cpu, proc);
            guest_unlock();
            g_timer_ticks++;
        }
    }
    return 0;
}

/* ---- registry: in memory, "not found" default ---- */
/* If the game doesn't find a value it uses its own default - which is what we want. */
#define ERROR_SUCCESS         0u
#define ERROR_FILE_NOT_FOUND  2u

/* ---- exported API ---- */
static uint32_t A(CPU *c, int i){ return MR32(c->esp + 4u*(uint32_t)i); }

/* --- simple persistent registry ----------------------------------------
 * The settings the game writes (graphics detail, resolution, sound) used to be
 * thrown away.  Here they are kept and written to the ~/.nfsu2_registry file. */
#define MAXREG 128
static struct { char name[64]; int is_str; uint32_t dw; char sv[128]; } g_reg[MAXREG];
static int g_regn = 0;
static int g_reg_loaded = 0;

static const char *reg_path(void){
    static char pth[512];
    const char *h = getenv("HOME");
    snprintf(pth, sizeof pth, "%s/.nfsu2_registry", h ? h : ".");
    return pth;
}
static void reg_save(void);
static void reg_load(void){
    if (g_reg_loaded) return;
    g_reg_loaded = 1;
    FILE *f = fopen(reg_path(), "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f) && g_regn < MAXREG){
        char nm[64], val[128]; int isstr = 0; unsigned dw = 0;
        if (sscanf(line, "d %63[^=]=%u", nm, &dw) == 2){
            snprintf(g_reg[g_regn].name, 64, "%s", nm);
            g_reg[g_regn].is_str = 0; g_reg[g_regn].dw = dw; g_regn++;
        } else if (sscanf(line, "s %63[^=]=%127[^\n]", nm, val) == 2){
            snprintf(g_reg[g_regn].name, 64, "%s", nm);
            snprintf(g_reg[g_regn].sv, 128, "%s", val);
            g_reg[g_regn].is_str = 1; g_regn++;
        }
        (void)isstr;
    }
    fclose(f);
    /* One-time repair.  Before 2026-10-25 the game's first-run hardware detection
     * measured a 0 MHz CPU (EFLAGS ID bit, see eflags_get in cpu.h) and saved every
     * graphics option at its minimum, then NotFirstTime=1, so it never detected
     * again: no crowds, short draw distance (pop-in), no reflections.  If the saved
     * settings carry exactly that signature, drop NotFirstTime and the g_* values
     * so the next launch detects again.  Done once (PortSettingsRepaired). */
    int fixed = 0, zero = 0;
    for (int i = 0; i < g_regn; i++){
        if (!strcasecmp(g_reg[i].name, "PortSettingsRepaired")) fixed = 1;
        if (!g_reg[i].is_str && g_reg[i].dw == 0 &&
            (!strcasecmp(g_reg[i].name, "g_PerformanceLevel") ||
             !strcasecmp(g_reg[i].name, "g_WorldLodLevel") ||
             !strcasecmp(g_reg[i].name, "g_CarLodLevel") ||
             !strcasecmp(g_reg[i].name, "g_CrowdEnable"))) zero++;
    }
    if (!fixed && g_regn < MAXREG){
        if (zero == 4){
            int k = 0;
            for (int i = 0; i < g_regn; i++){
                if (!strncmp(g_reg[i].name, "g_", 2) ||
                    !strcasecmp(g_reg[i].name, "NotFirstTime")) continue;
                if (k != i) g_reg[k] = g_reg[i];
                k++;
            }
            g_regn = k;
            fprintf(stderr, "registry: minimum settings from the old hardware detection"
                            " found; detecting again\n");
        }
        snprintf(g_reg[g_regn].name, 64, "PortSettingsRepaired");
        g_reg[g_regn].is_str = 0; g_reg[g_regn].dw = 1; g_regn++;
        reg_save();
    }
}
static void reg_save(void){
    FILE *f = fopen(reg_path(), "w");
    if (!f) return;
    for (int i = 0; i < g_regn; i++){
        if (g_reg[i].is_str) fprintf(f, "s %s=%s\n", g_reg[i].name, g_reg[i].sv);
        else                 fprintf(f, "d %s=%u\n", g_reg[i].name, g_reg[i].dw);
    }
    fclose(f);
}
static int reg_find(const char *nm){
    reg_load();
    for (int i = 0; i < g_regn; i++)
        if (!strcasecmp(g_reg[i].name, nm)) return i;
    return -1;
}
static void reg_put_dword(const char *nm, uint32_t v){
    int i = reg_find(nm);
    if (i < 0){ if (g_regn >= MAXREG) return; i = g_regn++;
                snprintf(g_reg[i].name, 64, "%s", nm); }
    g_reg[i].is_str = 0; g_reg[i].dw = v; reg_save();
}
static void reg_put_string(const char *nm, const char *v){
    int i = reg_find(nm);
    if (i < 0){ if (g_regn >= MAXREG) return; i = g_regn++;
                snprintf(g_reg[i].name, 64, "%s", nm); }
    g_reg[i].is_str = 1; snprintf(g_reg[i].sv, 128, "%s", v); reg_save();
}

int win32_ext_call(CPU *c, const char *n, uint32_t *out){
    uint32_t r = 0;
    /* --- Registry --- */
    if (!strcmp(n,"RegOpenKeyA")){ uint32_t p=A(c,2); if(p) MR32(p)=obj_new(OB_KEY); r=ERROR_SUCCESS; }
    else if (!strcmp(n,"RegOpenKeyExA")){
        if (getenv("REGLOG")){ uint32_t sp=A(c,1);
            fprintf(stderr,"  [registry] OpenKeyEx(\"%s\")\n",
                    (sp && mem_is_committed(sp)) ? (const char*)(g_mem+sp) : "(null)"); }
        uint32_t p=A(c,4); if(p) MR32(p)=obj_new(OB_KEY); r=ERROR_SUCCESS; }
    else if (!strcmp(n,"RegCreateKeyA")){ uint32_t p=A(c,2); if(p) MR32(p)=obj_new(OB_KEY); r=ERROR_SUCCESS; }
    else if (!strcmp(n,"RegCreateKeyExA")){ uint32_t p=A(c,7); if(p) MR32(p)=obj_new(OB_KEY); r=ERROR_SUCCESS; }
    else if (!strcmp(n,"RegQueryValueExA")){
        /* An empty registry sends the game down the "first install" path ON EVERY
         * LAUNCH (NotFirstTime isn't found), and without install information the
         * front-end flow doesn't progress.  We return a reasonable
         * installed-game profile. */
        uint32_t nameptr=A(c,1), ptype=A(c,3), pdata=A(c,4), pcb=A(c,5);
        const char *vn = (nameptr && mem_is_committed(nameptr))
                         ? (const char*)(g_mem+nameptr) : "";
        if (getenv("REGLOG"))
            fprintf(stderr,"  [registry] QueryValue(\"%s\")\n", vn);

        const char *sval = 0; uint32_t dval = 0; int have_d = 0;
        { int ri = reg_find(vn);            /* look at the SAVED value first */
          if (ri >= 0){
              if (g_reg[ri].is_str) sval = g_reg[ri].sv;
              else { dval = g_reg[ri].dw; have_d = 1; }
          } }
        if (sval || have_d) { /* the saved value will be used */ }
        /* NotFirstTime is NOT faked any more: it used to read 1 on an empty
         * registry (from the days when writes were dropped and every launch was
         * a "first launch"), so the game never ran its first-run hardware
         * detection and kept every graphics setting at 0.  Writes persist now: the
         * first launch detects, writes NotFirstTime=1 itself, and later launches
         * read it back.  REGFIRST=1 restores the fake. */
        else if (!strcasecmp(vn,"NotFirstTime") && getenv("REGFIRST")){ dval=1; have_d=1; }
        else if (!strcasecmp(vn,"Language"))      sval="English";
        else if (!strcasecmp(vn,"Install Dir"))   sval="C:\\NFSU2";
        else if (!strcasecmp(vn,"CD Drive"))      sval="C:\\NFSU2";
        else if (!strcasecmp(vn,"StreamingInstall")){ dval=0; have_d=1; }
        else if (!strcasecmp(vn,"VERSION"))       sval="1.2.0.0";
        else if (!strcasecmp(vn,"SIZE")){ dval=0; have_d=1; }

        if (sval){
            uint32_t need = (uint32_t)strlen(sval) + 1u;
            if (ptype){ mem_commit(ptype,4); MR32(ptype)=1u; }      /* REG_SZ */
            if (!pdata){ if(pcb){ mem_commit(pcb,4); MR32(pcb)=need; } r=ERROR_SUCCESS; }
            else {
                uint32_t cap = pcb ? MR32(pcb) : need;
                if (cap < need){ if(pcb) MR32(pcb)=need; r=234u; } /* ERROR_MORE_DATA */
                else { mem_commit(pdata,need); memcpy(g_mem+pdata, sval, need);
                       if(pcb) MR32(pcb)=need; r=ERROR_SUCCESS; }
            }
        } else if (have_d){
            if (ptype){ mem_commit(ptype,4); MR32(ptype)=4u; }      /* REG_DWORD */
            if (!pdata){ if(pcb){ mem_commit(pcb,4); MR32(pcb)=4u; } r=ERROR_SUCCESS; }
            else {
                uint32_t cap = pcb ? MR32(pcb) : 4u;
                if (cap < 4u){ if(pcb) MR32(pcb)=4u; r=234u; }
                else { mem_commit(pdata,4); MR32(pdata)=dval;
                       if(pcb) MR32(pcb)=4u; r=ERROR_SUCCESS; }
            }
        } else {
            if(pcb){ mem_commit(pcb,4); MR32(pcb)=0; }
            r=ERROR_FILE_NOT_FOUND;
        }
    }
    else if (!strcmp(n,"RegQueryValueA")){ r=ERROR_FILE_NOT_FOUND; }
    else if (!strcmp(n,"RegSetValueExA")||!strcmp(n,"RegSetValueA")){
        /* Writes used to be SILENTLY DROPPED: the graphics/sound settings the game
         * saved were reset on every launch.  Now they are kept and written to
         * disk. */
        uint32_t nameptr=A(c,1), type=A(c,3), pdata=A(c,4), cb=A(c,5);
        const char *vn = (nameptr && mem_is_committed(nameptr))
                         ? (const char*)(g_mem+nameptr) : "";
        if (*vn){
            if (type == 4u && pdata && mem_is_committed(pdata+3))
                reg_put_dword(vn, MR32(pdata));
            else if (type == 1u && pdata && mem_is_committed(pdata))
                reg_put_string(vn, (const char*)(g_mem+pdata));
            if (getenv("REGLOG"))
                fprintf(stderr,"  [registry] SetValue(\"%s\") type=%u size=%u\n", vn, type, cb);
        }
        r=ERROR_SUCCESS; }
    else if (!strcmp(n,"RegCloseKey")) r=ERROR_SUCCESS;
    else if (!strcmp(n,"RegDeleteKeyA")||!strcmp(n,"RegDeleteValueA")) r=ERROR_SUCCESS;
    else if (!strcmp(n,"RegEnumKeyExA")||!strcmp(n,"RegEnumValueA")) r=259u; /* ERROR_NO_MORE_ITEMS */
    /* --- window / USER32 --- */
    /* The window message path.  DirectInput devices are only Acquired when the
     * application gets FOCUS; the game learns this from the
     * WM_ACTIVATEAPP/WM_SETFOCUS messages.  Since PeekMessage always returned 0
     * and DispatchMessage never called the window procedure, the game never said
     * "I'm active" -> the keyboard was never Acquired.                  */

    else if (!strcmp(n,"RegisterClassExA")||!strcmp(n,"RegisterClassA")){
        /* WNDCLASSEXA: +8 lpfnWndProc | WNDCLASSA: +4 lpfnWndProc */
        uint32_t wc=A(c,0);
        if (wc && mem_is_committed(wc+11)){
            uint32_t off = !strcmp(n,"RegisterClassExA") ? 8u : 4u;
            uint32_t wp = MR32(wc+off);
            if (wp) { g_wndproc = wp;
                fprintf(stderr,"  [window] class registered, procedure=0x%08x\n", wp); }
        }
        r=0xC001u; }
    else if (!strcmp(n,"UnregisterClassA")) r=1;
    else if (!strcmp(n,"CreateWindowExA")){
        r = g_hwnd;
        if (!g_wm_activated){ g_wm_activated = 1;
            wm_push(0x0018u, 1u, 0u);   /* WM_SHOWWINDOW  */
            wm_push(0x001Cu, 1u, 0u);   /* WM_ACTIVATEAPP  active */
            wm_push(0x0006u, 1u, 0u);   /* WM_ACTIVATE     WA_ACTIVE */
            wm_push(0x0007u, 0u, 0u);   /* WM_SETFOCUS     */
            wm_push(0x000Fu, 0u, 0u);   /* WM_PAINT        */
        } }
    else if (!strcmp(n,"DestroyWindow")) r=1;
    else if (!strcmp(n,"GetDesktopWindow")) r=0x00B00000u;
    else if (!strcmp(n,"GetActiveWindow")||!strcmp(n,"GetForegroundWindow")) r=0x00B00001u;
    else if (!strcmp(n,"LoadIconA")||!strcmp(n,"LoadCursorA")) r=0x00B10001u;
    else if (!strcmp(n,"SetCursor")) r=0x00B10001u;
    else if (!strcmp(n,"ShowCursor")){ static int cnt=0; r=(uint32_t)(A(c,0)?++cnt:--cnt); }
    else if (!strcmp(n,"GetCursorPos")){ uint32_t p=A(c,0);
        if(p){ int mx=0,my=0; void metal_get_cursor(int*,int*); metal_get_cursor(&mx,&my);
               mem_commit(p,8); MR32(p)=(uint32_t)mx; MR32(p+4)=(uint32_t)my; } r=1; }
    else if (!strcmp(n,"SetCursorPos")) r=1;
    else if (!strcmp(n,"GetSystemMetrics")){
        unsigned gw = 1280, gh = 720; void metal_game_size(unsigned*,unsigned*); metal_game_size(&gw,&gh);
        switch(A(c,0)){ case 0: r=gw; break; case 1: r=gh; break;
                        case 4: r=19; break; case 5: r=1; break; case 6: r=1; break;
                        case 32: r=4; case 33: r=4; break; default: r=0; } }
    else if (!strcmp(n,"AdjustWindowRect")){ r=1; }
    else if (!strcmp(n,"GetClientRect")||!strcmp(n,"GetWindowRect")){
        /* The game's real resolution: cursor mapping follows it (see
         * metal_game_size). */
        unsigned gw = 1280, gh = 720; void metal_game_size(unsigned*,unsigned*); metal_game_size(&gw,&gh);
        uint32_t p=A(c,1); if(p){mem_commit(p,16);MR32(p)=0;MR32(p+4)=0;MR32(p+8)=gw;MR32(p+12)=gh;} r=1; }
    else if (!strcmp(n,"ShowWindow")||!strcmp(n,"UpdateWindow")||!strcmp(n,"SetFocus")
           ||!strcmp(n,"SetForegroundWindow")||!strcmp(n,"SetWindowPos")
           ||!strcmp(n,"InvalidateRect")||!strcmp(n,"SetWindowTextA")) r=1;
    else if (!strcmp(n,"SetWindowLongA")||!strcmp(n,"GetWindowLongA")) r=0;
    else if (!strcmp(n,"PeekMessageA")||!strcmp(n,"GetMessageA")){
        /* MSG: +0 hwnd +4 message +8 wParam +12 lParam +16 time +20 pt */
        uint32_t pm=A(c,0), m=0, wp=0, lp=0;
        int remove = !strcmp(n,"GetMessageA") ? 1 : (A(c,4) & 1u);
        if (g_wmq_head != g_wmq_tail){
            if (remove) wm_pop(&m,&wp,&lp);
            else { m=g_wmq[g_wmq_head].msg; wp=g_wmq[g_wmq_head].wp; lp=g_wmq[g_wmq_head].lp; }
            if (pm){ mem_commit(pm,28);
                MR32(pm)=g_hwnd; MR32(pm+4)=m; MR32(pm+8)=wp; MR32(pm+12)=lp;
                MR32(pm+16)=0; MR32(pm+20)=0; MR32(pm+24)=0; }
            g_wm_sent++;
            r=1;
        } else if ((wp = (uint32_t)metal_take_char(remove)) != 0xFFFFFFFFu){
            /* A typed character (see metal.m char_push).  The WM_CHAR that Windows's
             * TranslateMessage would produce; lParam: repeat=1. */
            if (pm){ mem_commit(pm,28);
                MR32(pm)=g_hwnd; MR32(pm+4)=0x0102u; MR32(pm+8)=wp; MR32(pm+12)=1u;
                MR32(pm+16)=0; MR32(pm+20)=0; MR32(pm+24)=0; }
            g_wm_sent++;
            r=1;
        } else {
            if (pm){ mem_commit(pm,28); memset(g_mem+pm,0,28); MR32(pm)=g_hwnd; }
            r = !strcmp(n,"GetMessageA") ? 1u : 0u;   /* GetMessage 0 = WM_QUIT */
        }
    }
    else if (!strcmp(n,"DispatchMessageA")){
        uint32_t pm=A(c,0);
        if (g_wndproc && pm && mem_is_committed(pm+15)){
            uint32_t args[4] = { MR32(pm), MR32(pm+4), MR32(pm+8), MR32(pm+12) };
            if (args[1]){ g_wm_dispatched++; r = call_guest(c, g_wndproc, 4, args); }
        }
    }
    else if (!strcmp(n,"TranslateMessage")
           ||!strcmp(n,"DefWindowProcA")||!strcmp(n,"PostMessageA")
           ||!strcmp(n,"SendMessageA")||!strcmp(n,"PostQuitMessage")) r=0;
    else if (!strcmp(n,"ReleaseCapture")||!strcmp(n,"SetCapture")) r=1;
    else if (!strcmp(n,"IsIconic")) r=0;
    else if (!strcmp(n,"GetKeyboardState")){ uint32_t p=A(c,0); if(p){mem_commit(p,256);memset(g_mem+p,0,256);} r=1; }
    else if (!strcmp(n,"GetAsyncKeyState")) r=0;
    /* --- synchronization --- */
    else if (!strcmp(n,"CreateEventA")){
        uint32_t h=obj_new(OB_EVENT); Obj*o=obj_get(h);
        if(o){ o->manual=(int)A(c,1); o->signaled=(int)A(c,2); } r=h; }
    else if (!strcmp(n,"SetEvent")){ Obj*o=obj_get(A(c,0));
        { uint32_t h=A(c,0);
          if (h>=H_BASE && h<H_BASE+64){ g_wstat[h-H_BASE].sets++;
            g_wstat[h-H_BASE].setter_tid =
                (c->fs_base && mem_is_committed(c->fs_base+0x24u))
                ? MR32(c->fs_base+0x24u) : 0u; } }
        if(o){ pthread_mutex_lock(&o->mtx); o->signaled=1; pthread_cond_broadcast(&o->cv); pthread_mutex_unlock(&o->mtx);} r=1; }
    else if (!strcmp(n,"ResetEvent")){ Obj*o=obj_get(A(c,0));
        if(o){ pthread_mutex_lock(&o->mtx); o->signaled=0; pthread_mutex_unlock(&o->mtx);} r=1; }
    else if (!strcmp(n,"CreateMutexA")){
        uint32_t h=obj_new(OB_MUTEX); Obj*o=obj_get(h);
        if(o){ if (A(c,1)){ o->signaled=0; o->owner_tid=cur_tid(c); o->rec=1; }
               else       { o->signaled=1; o->owner_tid=0; o->rec=0; } }
        r=h; }
    else if (!strcmp(n,"ReleaseMutex")){
        Obj*o=obj_get(A(c,0)); uint32_t me=cur_tid(c);
        r=0;
        if(o){ pthread_mutex_lock(&o->mtx);
               if (o->kind == OB_MUTEX){
                   if (o->owner_tid == me && o->rec > 0){
                       if (--o->rec == 0){ o->owner_tid = 0; o->signaled = 1;
                                           pthread_cond_broadcast(&o->cv); }
                       r=1;
                   } else r=0;            /* we aren't the owner */
               } else { o->signaled=1; pthread_cond_broadcast(&o->cv); r=1; }
               pthread_mutex_unlock(&o->mtx); }
    }
    else if (!strcmp(n,"CreateSemaphoreA")){ uint32_t h=obj_new(OB_SEM); Obj*o=obj_get(h); if(o) o->signaled=(int)A(c,1); r=h; }
    else if (!strcmp(n,"ReleaseSemaphore")){ Obj*o=obj_get(A(c,0));
        if(o){ pthread_mutex_lock(&o->mtx); o->signaled+=(int)A(c,1); pthread_cond_broadcast(&o->cv); pthread_mutex_unlock(&o->mtx);} r=1; }
    else if (!strcmp(n,"WaitForSingleObject")){
        Obj*o=obj_get(A(c,0)); uint32_t ms=A(c,1);
        { uint32_t h=A(c,0);
          uint32_t tid = (c->fs_base && mem_is_committed(c->fs_base+0x24u))
                         ? MR32(c->fs_base+0x24u) : 0u;
          if (h>=H_BASE && h<H_BASE+64){ g_wstat[h-H_BASE].waits++;
                                         g_wstat[h-H_BASE].waiter_tid = tid; }
          static int sh=0;
          if (++sh % 400 == 0){
            fprintf(stderr,"  [event] handle  type  signal  wait  SetEvent  waiting-tid\n");
            for (int i=0;i<64;i++) if (g_wstat[i].waits || g_wstat[i].sets)
                fprintf(stderr, "         h%-4d %3d %6d %8llu %9llu  tid=%u\n", i,
                        g_obj[i].kind, g_obj[i].signaled,
                        (unsigned long long)g_wstat[i].waits,
                        (unsigned long long)g_wstat[i].sets,
                        g_wstat[i].waiter_tid);
          } }
        if(!o){ r=0; }
        else if (o->kind == OB_MUTEX){
            /* MUTEX: ownership + RECURSION.  The same thread must be able to take the same
             * mutex again; otherwise the game's CRT wrapper that takes nested locks
             * deadlocks itself (this is how the video player froze). */
            uint32_t me = cur_tid(c);
            guest_yield_begin();
            pthread_mutex_lock(&o->mtx);
            if (o->owner_tid == me && o->rec > 0){
                o->rec++; r = 0;                       /* WAIT_OBJECT_0 */
            } else {
                struct timespec ts;
                int timed_out = 0;
                while (!o->signaled && !timed_out){
                    if (!ms){ timed_out = 1; break; }
                    clock_gettime(CLOCK_REALTIME,&ts);
                    uint32_t wait = (ms==0xFFFFFFFFu)? 2000u : (ms>2000u?2000u:ms);
                    ts.tv_sec += wait/1000; ts.tv_nsec += (long)(wait%1000)*1000000L;
                    if(ts.tv_nsec>=1000000000L){ts.tv_sec++;ts.tv_nsec-=1000000000L;}
                    if (pthread_cond_timedwait(&o->cv,&o->mtx,&ts) != 0 &&
                        ms != 0xFFFFFFFFu) timed_out = 1;
                    if (ms != 0xFFFFFFFFu) break;
                }
                if (o->signaled){ o->signaled = 0; o->owner_tid = me; o->rec = 1; r = 0; }
                else r = 0x102u;                        /* WAIT_TIMEOUT */
            }
            pthread_mutex_unlock(&o->mtx);
            guest_yield_end();
        }
        else {
            guest_yield_begin();
            pthread_mutex_lock(&o->mtx);
            if(!o->signaled && ms){
                uint64_t t0 = wait_now_ms();
                for (;;){
                    struct timespec ts; clock_gettime(CLOCK_REALTIME,&ts);
                    uint32_t wait = 2000u;
                    if (!wait_legacy()){
                        wait = 50u;                               /* short slices: see terminate */
                        if (ms != 0xFFFFFFFFu){
                            uint64_t el = wait_now_ms() - t0;
                            if (el >= ms) break;
                            if (ms - el < wait) wait = (uint32_t)(ms - el);
                        }
                    } else if (ms != 0xFFFFFFFFu && ms < wait) wait = ms;
                    ts.tv_sec += wait/1000; ts.tv_nsec += (long)(wait%1000)*1000000L;
                    if(ts.tv_nsec>=1000000000L){ts.tv_sec++;ts.tv_nsec-=1000000000L;}
                    pthread_cond_timedwait(&o->cv,&o->mtx,&ts);
                    if (o->signaled || wait_legacy() || wait_thread_killed()) break;
                }
            }
            r = o->signaled ? 0u : 0x102u;              /* WAIT_OBJECT_0 / WAIT_TIMEOUT */
            if(o->signaled && !o->manual) o->signaled=0;
            pthread_mutex_unlock(&o->mtx);
            guest_yield_end();
        } }
    /* --- thread --- */
    else if (!strcmp(n,"CreateThread")){
        uint32_t entry=A(c,2), arg=A(c,3), ptid=A(c,5);
        { static int sc=0; if (sc++ < 200) fprintf(stderr, "  [thread] CreateThread entry=0x%08x arg=0x%08x flags=0x%x\n", entry, arg, A(c,4)); }
        { static int sh=0; if (getenv("VERBOSE") && sh<40){ sh++;
            fprintf(stderr,"      [thread] CreateThread(attr=0x%08x stack=0x%x entry=0x%08x arg=0x%08x flags=0x%x ptid=0x%08x)\n",
                    A(c,0),A(c,1),entry,arg,A(c,4),ptid); } }
        uint32_t h=obj_new(OB_THREAD); Obj*o=obj_get(h);
        static uint32_t next_tid=100;
        if(o){
            o->tid = ++next_tid;
            if(ptid) MR32(ptid)=o->tid;
            uint32_t teb;
            pthread_mutex_lock(&g_lock);
            static uint32_t teb_next = 0x00C20000u;
            teb = teb_next; teb_next += 0x2000u;
            pthread_mutex_unlock(&g_lock);
            mem_commit(teb, 0x2000);
            TArg *t = calloc(1,sizeof *t);
            t->entry=entry; t->arg=arg; t->teb=teb; t->ob=o;
            o->pending = t;
            if (A(c,4) & 0x4u){                 /* CREATE_SUSPENDED */
                o->started = 0;                 /* ResumeThread will wait */
            } else {
                if (guest_pthread_create(&o->th,guest_thread_main,t)==0){
                    o->started=1; o->pending=0; pthread_detach(o->th);
                } else free(t);
            }
        }
        r=h; }
    else if (!strcmp(n,"GetCurrentThread")) r=0xFFFFFFFEu;
    else if (!strcmp(n,"ResumeThread")){
        Obj*o=obj_get(A(c,0));
        if (o && !o->started && o->pending){
            if (guest_pthread_create(&o->th,guest_thread_main,o->pending)==0){
                o->started=1; o->pending=0; pthread_detach(o->th); }
        }
        r=1; }
    else if (!strcmp(n,"SuspendThread")) r=0;
    else if (!strcmp(n,"ExitThread")){
        /* REALLY terminate the guest thread. Otherwise execution falls from
         * _endthreadex into the _threadstartex prologue and the thread restarts itself. */
        guest_thread_die();
        r=1; }
    else if (!strcmp(n,"TerminateThread")){
        Obj *to = obj_get(A(c,0));
        { static int sh = 0; if (sh++ < 200)
            fprintf(stderr, "  [thread] TerminateThread tid=%u%s\n", to ? to->tid : 0u, to == tl_obj ? " (self)" : ""); }
        if (to && to->kind == OB_THREAD){
            if (to == (Obj*)tl_obj) guest_thread_die();
            to->terminate = 1;
        }
        r=1; }
    else if (!strcmp(n,"GetExitCodeThread")){ uint32_t p=A(c,1); if(p) MR32(p)=0; r=1; }
    else if (!strcmp(n,"SetThreadAffinityMask")||!strcmp(n,"SetProcessAffinityMask")) r=1;
    else if (!strcmp(n,"GetProcessAffinityMask")){
        uint32_t a=A(c,1),b=A(c,2); if(a) MR32(a)=0xFF; if(b) MR32(b)=0xFF; r=1; }
    else if (!strcmp(n,"SetPriorityClass")) r=1;
    else if (!strcmp(n,"GetPriorityClass")) r=0x20;
    /* --- handles / misc --- */
    else if (!strcmp(n,"Sleep")){
        uint32_t ms=A(c,0);
        guest_yield_begin();
        { struct timespec ts={ (time_t)(ms/1000u), (long)((ms%1000u)*1000000u) };
          if(!ms){ ts.tv_nsec = 200000L; }        /* Sleep(0): just yield */
          nanosleep(&ts,0); }
        guest_yield_end();
        r=0; }
    else if (!strcmp(n,"SleepEx")){
        uint32_t ms=A(c,0);
        guest_yield_begin();
        { struct timespec ts={ (time_t)(ms/1000u), (long)((ms%1000u)*1000000u) };
          if(!ms) ts.tv_nsec = 200000L;
          nanosleep(&ts,0); }
        guest_yield_end();
        r=0; }
    else if (!strcmp(n,"WaitForMultipleObjects")){
        uint32_t cnt=A(c,0), parr=A(c,1), waitall=A(c,2), ms=A(c,3);
        guest_yield_begin();
        uint32_t got=0x102u;
        uint64_t t0 = wait_now_ms();
        for (int spin=0; ; spin++){
            if (waitall && !wait_legacy()){
                uint32_t nsig=0, nobj=0;
                for (uint32_t i=0;i<cnt && i<64;i++){
                    Obj*o=obj_get(MR32(parr+4u*i)); if(!o) continue; nobj++;
                    pthread_mutex_lock(&o->mtx); if (o->signaled) nsig++; pthread_mutex_unlock(&o->mtx); }
                if (nobj && nsig==nobj){
                    for (uint32_t i=0;i<cnt && i<64;i++){
                        Obj*o=obj_get(MR32(parr+4u*i)); if(!o) continue;
                        pthread_mutex_lock(&o->mtx); if(!o->manual) o->signaled=0; pthread_mutex_unlock(&o->mtx); }
                    got=0; }
            } else
            for (uint32_t i=0;i<cnt && i<64;i++){
                Obj*o=obj_get(MR32(parr+4u*i));
                if(o){ pthread_mutex_lock(&o->mtx);
                       int sig=o->signaled;
                       if(sig && !o->manual) o->signaled=0;
                       pthread_mutex_unlock(&o->mtx);
                       if(sig){ got=i; break; } }
            }
            if (got!=0x102u || ms==0) break;
            if (wait_legacy()){ if (spin>=199) break; }
            else { if (ms!=0xFFFFFFFFu && wait_now_ms()-t0 >= ms) break; if (wait_thread_killed()) break; }
            struct timespec ts={0,1000000L}; nanosleep(&ts,0);
        }
        guest_yield_end();
        r=got; }
    else if (!strcmp(n,"SwitchToThread")){ guest_yield_begin(); sched_yield(); guest_yield_end(); r=1; }
    else if (!strcmp(n,"CloseHandle")) r=1;
    else if (!strcmp(n,"DuplicateHandle")){ uint32_t p=A(c,3); if(p) MR32(p)=A(c,1); r=1; }
    else if (!strcmp(n,"SetErrorMode")) r=0;
    else if (!strcmp(n,"GetLogicalDrives")) r=0x0000000Cu;             /* C: and D: */
    else if (!strcmp(n,"GetDriveTypeA")){
#if NFS_EXE_NA
        /* The North American v1.2 no-CD executable still looks for a CD-ROM
         * drive (sub_005bf450: GetDriveTypeA == DRIVE_CDROM; the disc contents
         * check is patched out) and otherwise shows "Please insert Disc 2"
         * forever.  D: is that drive; paths on it resolve to the game folder. */
        uint32_t p=A(c,0); char d = p ? (char)MR8(p) : 'C';
        r = (d=='D' || d=='d') ? 5u : 3u;                                  /* DRIVE_CDROM : DRIVE_FIXED */
#else
        r=3;                                                           /* DRIVE_FIXED */
#endif
    }
    else if (!strcmp(n,"GetCurrentDirectoryA")){
        uint32_t p=A(c,1); const char*s="C:\\NFSU2"; uint32_t l=(uint32_t)strlen(s);
        if(p){ mem_commit(p,l+1); memcpy(g_mem+p,s,l); MR8(p+l)=0; } r=l; }
    else if (!strcmp(n,"SetCurrentDirectoryA")) r=1;
    else if (!strcmp(n,"GetLongPathNameA")||!strcmp(n,"GetShortPathNameA")){
        uint32_t src=A(c,0), dst=A(c,1), sz=A(c,2);
        uint32_t l = src? (uint32_t)strlen((char*)(g_mem+src)) : 0;
        if(dst && sz>l){ memcpy(g_mem+dst, g_mem+src, l+1); }
        r=l; }
    /* The timestamps of save files (the WIN32_FIND_DATA FILETIMEs).  When a profile
     * is found the game feeds them into the CRT's localtime; they used to be
     * unimplemented -> garbage -> localtime NULL -> crash at guest address 0x14
     * (tm_year).  They were never called until a profile had been saved. */
    else if (!strcmp(n,"FileTimeToLocalFileTime")||!strcmp(n,"LocalFileTimeToFileTime")){
        uint32_t s=A(c,0), d=A(c,1); r=0;
        if (s && d && mem_is_committed(s+7)){
            uint64_t ft = (uint64_t)MR32(s) | ((uint64_t)MR32(s+4) << 32);
            time_t t = time(0); struct tm lt; localtime_r(&t, &lt);
            int64_t off = (int64_t)lt.tm_gmtoff * 10000000ll;
            ft = !strcmp(n,"FileTimeToLocalFileTime") ? ft + (uint64_t)off : ft - (uint64_t)off;
            mem_commit(d,8); MR32(d)=(uint32_t)ft; MR32(d+4)=(uint32_t)(ft>>32); r=1; } }
    else if (!strcmp(n,"FileTimeToSystemTime")){
        /* SYSTEMTIME: wYear wMonth wDayOfWeek wDay wHour wMinute wSecond wMs */
        uint32_t s=A(c,0), d=A(c,1); r=0;
        if (s && d && mem_is_committed(s+7)){
            uint64_t ft = (uint64_t)MR32(s) | ((uint64_t)MR32(s+4) << 32);
            uint64_t sec = ft / 10000000ull;
            if (sec >= 11644473600ull){
                time_t t = (time_t)(sec - 11644473600ull); struct tm g;
                if (gmtime_r(&t, &g)){
                    mem_commit(d,16);
                    MR16(d)    = (uint16_t)(g.tm_year + 1900);
                    MR16(d+2)  = (uint16_t)(g.tm_mon + 1);
                    MR16(d+4)  = (uint16_t)g.tm_wday;
                    MR16(d+6)  = (uint16_t)g.tm_mday;
                    MR16(d+8)  = (uint16_t)g.tm_hour;
                    MR16(d+10) = (uint16_t)g.tm_min;
                    MR16(d+12) = (uint16_t)g.tm_sec;
                    MR16(d+14) = (uint16_t)((ft / 10000ull) % 1000ull);
                    r=1; } } } }
    else if (!strcmp(n,"GetTimeZoneInformation")){
        /* TIME_ZONE_INFORMATION (172 bytes): +0 Bias (minutes, UTC = local + Bias).
         * The buffer used not to be filled (garbage). */
        uint32_t p=A(c,0);
        if (p){ mem_commit(p,172); memset(g_mem+p,0,172);
            time_t t = time(0); struct tm lt; localtime_r(&t, &lt);
            MR32(p) = (uint32_t)(int32_t)(-(lt.tm_gmtoff / 60)); }
        r=0; }                                        /* TIME_ZONE_ID_UNKNOWN */
    else if (!strcmp(n,"GetFullPathNameA")){
        /* (lpFileName, nBufferLength, lpBuffer, lpFilePart)
         * The career save builds the profile path with this
         * ("C:\NFSU2\NFS Underground 2").  It used to be unimplemented: it
         * returned 0 and left the buffer empty, the game tried to write to a
         * garbage path and said "Unable to save <name>".  The current
         * directory is always C:\NFSU2. */
        uint32_t src=A(c,0), sz=A(c,1), dst=A(c,2), fpart=A(c,3);
        const char *in = (src && mem_is_committed(src)) ? (const char*)(g_mem+src) : "";
        char full[1024]; size_t k = 0;
        if (in[0] && in[1] == ':')                    /* "X:..." */
            { full[k++] = in[0]; full[k++] = ':'; in += 2; }
        else { memcpy(full, "C:", 2); k = 2; }
        if (*in != '\\' && *in != '/'){               /* relative: the current directory */
            memcpy(full + k, "\\NFSU2", 6); k += 6; }
        /* component by component: skip ".", ".." goes one up */
        while (*in && k < sizeof full - 2){
            while (*in == '\\' || *in == '/') in++;
            if (!*in) break;
            const char *e2 = in; while (*e2 && *e2 != '\\' && *e2 != '/') e2++;
            size_t L = (size_t)(e2 - in);
            if (L == 1 && in[0] == '.') { in = e2; continue; }
            if (L == 2 && in[0] == '.' && in[1] == '.'){
                while (k > 2 && full[k-1] != '\\') k--;
                if (k > 2) k--;
                in = e2; continue; }
            if (k + 1 + L >= sizeof full - 1) break;
            full[k++] = '\\'; memcpy(full + k, in, L); k += L; in = e2;
        }
        if (k == 2) full[k++] = '\\';                 /* only "C:\" */
        /* the trailing separator is kept ("dir\" -> "C:\...\dir\") */
        { size_t il = src && mem_is_committed(src) ? strlen((const char*)(g_mem+src)) : 0;
          const char *s0 = (const char*)(g_mem+src);
          if (il && (s0[il-1]=='\\' || s0[il-1]=='/') && full[k-1] != '\\') full[k++] = '\\'; }
        full[k] = 0;
        if (dst && sz > k){
            mem_commit(dst, (uint32_t)k + 1); memcpy(g_mem+dst, full, k + 1);
            if (fpart){ mem_commit(fpart, 4);
                char *ls = strrchr(full, '\\');
                MR32(fpart) = (ls && ls[1]) ? dst + (uint32_t)(ls + 1 - full) : 0; }
            r = (uint32_t)k;
        } else r = (uint32_t)k + 1;                   /* the required size (including NUL) */
    }
    else if (!strcmp(n,"GetSystemDirectoryA")||!strcmp(n,"GetWindowsDirectoryA")){
        uint32_t p=A(c,0); const char*s="C:\\Windows"; uint32_t l=(uint32_t)strlen(s);
        if(p){ mem_commit(p,l+1); memcpy(g_mem+p,s,l); MR8(p+l)=0; } r=l; }
    else if (!strcmp(n,"GetSystemTimeAsFileTime")){
        uint32_t p=A(c,0); if(p){ mem_commit(p,8);
            static uint64_t ft = 130000000000000000ull; ft += 100000;
            MR32(p)=(uint32_t)ft; MR32(p+4)=(uint32_t)(ft>>32); } r=0; }
    else if (!strcmp(n,"GetLocalTime")||!strcmp(n,"GetSystemTime")){
        uint32_t p=A(c,0); if(p){ mem_commit(p,16); memset(g_mem+p,0,16);
            MR16(p)=2026; MR16(p+2)=8; MR16(p+6)=24; MR16(p+8)=12; } r=0; }
    /* --- copy protection / anti-debug: return failure --- */
    else if (!strcmp(n,"CreateToolhelp32Snapshot")) r=0xFFFFFFFFu;
    else if (!strcmp(n,"Process32First")||!strcmp(n,"Process32Next")) r=0;
    else if (!strcmp(n,"IsDebuggerPresent")) r=0;
    /* --- resources --- */
    else if (!strcmp(n,"FindResourceA")) r = res_find(A(c,1), A(c,2), 0);
    else if (!strcmp(n,"FindResourceW")) r = res_find(A(c,1), A(c,2), 1);
    else if (!strcmp(n,"LoadResource"))  r = A(c,1);            /* carry the HRSRC */
    else if (!strcmp(n,"LockResource"))  r = res_data(A(c,0));
    else if (!strcmp(n,"SizeofResource"))r = res_size(A(c,1));
    else if (!strcmp(n,"FreeResource"))  r = 1;
    else if (!strcmp(n,"GetSystemInfo")){
        uint32_t p=A(c,0); if(p){ mem_commit(p,36); memset(g_mem+p,0,36);
            MR32(p+4)=4096; MR32(p+20)=10; MR32(p+32)=0x000006d8; } r=0; }
    else if (!strcmp(n,"GlobalMemoryStatus")){
        uint32_t p=A(c,0); if(p){ mem_commit(p,32); memset(g_mem+p,0,32);
            MR32(p)=32; MR32(p+4)=50; MR32(p+8)=0x40000000u; MR32(p+12)=0x20000000u; } r=0; }
    else if (!strcmp(n,"IsBadReadPtr")||!strcmp(n,"IsBadWritePtr")||!strcmp(n,"IsBadCodePtr")) r=0;
    else if (!strcmp(n,"OutputDebugStringA")){ uint32_t p=A(c,0);
        if(p) fprintf(stderr,"[dbg] %s", (char*)(g_mem+p)); r=0; }
    else if (!strcmp(n,"timeBeginPeriod")||!strcmp(n,"timeEndPeriod")) r=0;
    else if (!strcmp(n,"timeGetTime")){ uint32_t host_ms(void); r = host_ms(); }
    else if (!strcmp(n,"timeSetEvent")){
        /* (uDelay, uResolution, lpTimeProc, dwUser, fuEvent) */
        uint32_t delay=A(c,0), proc=A(c,2), user=A(c,3), flags=A(c,4);
        uint32_t id=0;
        if (proc && !getenv("NOTIMER")){
            pthread_mutex_lock(&g_lock);
            if (!g_timer_running){
                g_timer_running = 1;
                pthread_t th2;
                if (pthread_create(&th2,0,timer_thread_main,0)==0) pthread_detach(th2);
                else g_timer_running = 0;
            }
            for (int i=0;i<MAXTIMER;i++){
                if (g_tm[i].due_us) continue;
                uint32_t us = delay ? delay*1000u : 1000u;
                g_tm[i].proc=proc; g_tm[i].user=user;
                g_tm[i].id = id = g_timer_next_id++;
                g_tm[i].period_us = (flags & 1u) ? us : 0u;
                g_tm[i].due_us = now_us() + us;
                break;
            }
            pthread_mutex_unlock(&g_lock);
            { static int sh2=0; if (sh2<3){ sh2++;
                fprintf(stderr,"  [timer] timeSetEvent %u ms, %s, callback 0x%08x\n",
                        delay, (flags&1u)?"periodic":"one-shot", proc); } }
        }
        r=id; }
    else if (!strcmp(n,"timeKillEvent")){
        uint32_t id=A(c,0);
        pthread_mutex_lock(&g_lock);
        for (int i=0;i<MAXTIMER;i++)
            if (g_tm[i].due_us && g_tm[i].id==id){ g_tm[i].due_us=0; break; }
        pthread_mutex_unlock(&g_lock);
        r=0; }
    else if (!strcmp(n,"timeGetDevCaps")){
        uint32_t p=A(c,0);
        if(p){ mem_commit(p,8); MR32(p)=1u; MR32(p+4)=1000000u; }
        r=0; }
    else if (!strcmp(n,"timeBeginPeriod")||!strcmp(n,"timeEndPeriod")) r=0;
    else if (!strcmp(n,"joyGetNumDevs")) r=0;
    /* --- REAL critical sections -----------------------------------------
     * If left as a no-op stub, the GIL's fair sharing (it releases the lock every
     * 1024 Win32 calls) breaks mutual exclusion: a thread can lose the lock while
     * inside a critical section.                                        */
    else if (!strcmp(n,"InitializeCriticalSection")||
             !strcmp(n,"InitializeCriticalSectionAndSpinCount")){
        (void)cs_for(A(c,0)); r=1; }
    else if (!strcmp(n,"EnterCriticalSection")){
        pthread_mutex_t *m = cs_for(A(c,0));
        if (m){
            g_cs_enter++;
            if (pthread_mutex_trylock(m) != 0){
                g_cs_contended++;
                guest_yield_begin();          /* release the GIL while waiting */
                pthread_mutex_lock(m);
                guest_yield_end();
            }
            tl_cs_depth++;
        }
        r=1; }
    else if (!strcmp(n,"TryEnterCriticalSection")){
        pthread_mutex_t *m = cs_for(A(c,0));
        r=1;
        if (m){ if (pthread_mutex_trylock(m)==0) tl_cs_depth++; else r=0; } }
    else if (!strcmp(n,"LeaveCriticalSection")){
        pthread_mutex_t *m = cs_for(A(c,0));
        if (m && tl_cs_depth > 0){ tl_cs_depth--; pthread_mutex_unlock(m); }
        r=1; }
    else if (!strcmp(n,"DeleteCriticalSection")) r=1;
    else return 0;                       /* this layer isn't interested */
    *out = r;
    return 1;
}
uint64_t win32_ext_threads(void){ return g_threads_started; }
uint64_t win32_ext_threads_exited(void){ return g_threads_exited; }
