/* Win32 layer - second round: registry, window, synchronization, threads */
#include <strings.h>
#include "cpu.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <sys/event.h>
#include <pthread/qos.h>
#include <stdatomic.h>
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
    int counting;                     /* EVENT: set by the timer thread, counts ticks (see CATCH-UP) */
    int prio;                         /* THREAD: SetThreadPriority (see LOCK PRIORITY) */
    int closed; uint64_t closed_us;   /* CloseHandle / RegCloseKey (see FREEING HANDLES) */
    int dups;                         /* DuplicateHandle copies (same value) still open */
    _Atomic int busy;                 /* threads inside a Wait on it right now */
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
uint64_t g_set_merged = 0;      /* timer SetEvents lost: TICK_CAP wake-ups already pending (see CATCH-UP) */

/* ---- BIG GUEST LOCK ----------------------------------------------------
 * Guest code runs on only one thread at a time. This keeps the guest
 * memory/allocators shared by the lifted code safe and the instruction trace
 * consistent. The lock is released only in blocking calls (Sleep/Wait*).  */
static pthread_mutex_t g_guest = PTHREAD_MUTEX_INITIALIZER;
extern CPU *g_cur_cpu;
static _Thread_local CPU *tl_cpu = 0;
static _Thread_local void *tl_obj = 0;   /* this thread's object */
static _Thread_local int tl_timer_thread = 0;   /* the timeSetEvent thread (see CATCH-UP) */
static _Thread_local int tl_cs_depth = 0;       /* critical sections held (see REAL critical sections) */
/* Wait semantics.  Waits used to give up early: events after 2 s even for INFINITE, multi-object
 * waits after 200 ms.  A game thread that waits for an event during a long load (the NIS
 * audio stream thread, for instance) then saw a bogus WAIT_TIMEOUT.  NFSU2_WAIT_LEGACY=1 cuts
 * waits at 2 s / 200 ms again (A/B). */
static int wait_legacy(void){ static int v=-1; if (v<0){ const char *e=getenv("NFSU2_WAIT_LEGACY"); v = (e && *e && *e!='0'); } return v; }
static uint64_t wait_now_us(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000u + (uint64_t)t.tv_nsec/1000u; }
static int wait_thread_killed(void);
/* The deadline of a wait (monotonic clock, UINT64_MAX = INFINITE) and the next slice of it.
 * Short slices let a TerminateThread end a waiting thread; a condition variable may also
 * wake early, so every wait loops until its object is ready or the deadline has passed. */
static uint64_t wait_deadline(uint32_t ms, uint32_t legacy_cap_ms){
    if (wait_legacy() && (ms == 0xFFFFFFFFu || ms > legacy_cap_ms)) ms = legacy_cap_ms;
    return ms == 0xFFFFFFFFu ? UINT64_MAX : wait_now_us() + (uint64_t)ms * 1000u;
}
static uint64_t wait_slice(uint64_t deadline, uint64_t slice_us){
    uint64_t now = wait_now_us();
    return deadline <= now ? 0 : deadline - now < slice_us ? deadline - now : slice_us;
}
/* With o->mtx held.  A mutex is ready when it is free or already ours (recursion); a wait
 * on it takes ownership.  A semaphore gives one count per wait, an auto-reset event resets,
 * a thread stays signaled once it has ended. */
static int wait_ready(Obj *o, uint32_t tid){
    return o->signaled > 0 || (o->kind == OB_MUTEX && o->owner_tid == tid && o->rec > 0);
}
static void wait_take(Obj *o, uint32_t tid){
    if (o->kind == OB_MUTEX){ o->signaled = 0; o->owner_tid = tid; o->rec++; }
    else if (o->kind == OB_SEM) o->signaled--;
    else if (o->kind == OB_EVENT && !o->manual) o->signaled = o->counting && o->signaled > 1 ? o->signaled - 1 : 0;
}
/* THREADLOG=1: log thread creation, termination and stack recycling */
static int thread_log(void){ static int v=-1; if (v<0){ const char *e=getenv("THREADLOG"); v = (e && *e && *e!='0'); } return v; }

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
/* ---- LOCK PRIORITY ----------------------------------------------------------
 * The sound threads -- the ones the game itself raises to THREAD_PRIORITY_HIGHEST or
 * above (the sound service thread 2, the DirectSound mixer threads 15) -- and the
 * timeSetEvent thread take the big lock before the others.  On Windows they run in
 * parallel with the game; here they wait for the lock, and the macOS mutex is not
 * fair: the game thread gives it up and takes it straight back thousands of times a
 * second (every WaitForSingleObject), and a woken waiter can lose that race again and
 * again -- how long depends on the machine.  Measured here: a 656 ms hold while a race
 * loaded, 950 ms of the music muted.  Now (a) a thread re-taking the lock steps back
 * while a sound thread waits for it, and (b) the thread holding it hands it over at its
 * next Win32, COM or indirect guest call (outside critical sections).  A sound thread's
 * turn is short (it mixes, then sleeps or waits): the sound threads' longest wait during
 * a load went to 10-24 ms, frame times unchanged.  NFSU2_GILPRIO=0 turns it off. */
_Atomic int             g_gil_prio_wait  = 0;   /* sound threads waiting for the lock (dispatch.c reads it) */
static _Atomic uint64_t g_gil_prio_taken = 0;   /* times one of them got it */
uint64_t g_gil_handoffs = 0, g_gil_deferred = 0;
uint64_t g_pw_n = 0, g_pw_us = 0, g_pw_max = 0;  /* sound threads' lock wait (written under the lock) */
static int sleep_old(void);
static int gil_prio_off(void){ static int v=-1; if (v<0){ const char *e=getenv("NFSU2_GILPRIO"); v = (e && *e=='0') || sleep_old(); } return v; }
static int gil_is_prio(void){ return tl_timer_thread || (tl_obj && ((Obj*)tl_obj)->prio >= 2); }
static void gil_acquire(void){
    if (!gil_prio_off()){
        if (gil_is_prio()){
            uint64_t t0 = gil_now_us();
            atomic_fetch_add(&g_gil_prio_wait, 1);
            pthread_mutex_lock(&g_guest);
            atomic_fetch_sub(&g_gil_prio_wait, 1);
            atomic_fetch_add(&g_gil_prio_taken, 1);
            uint64_t w = gil_now_us() - t0;
            g_pw_n++; g_pw_us += w; if (w > g_pw_max) g_pw_max = w;
            return;
        }
        if (atomic_load_explicit(&g_gil_prio_wait, memory_order_relaxed)){
            uint64_t t0 = gil_now_us(), g = atomic_load(&g_gil_prio_taken);
            g_gil_deferred++;
            while (atomic_load(&g_gil_prio_wait) && atomic_load(&g_gil_prio_taken) == g &&
                   gil_now_us() - t0 < 2000u)
                sched_yield();
        }
    }
    pthread_mutex_lock(&g_guest);
}
void guest_lock(CPU *c){ gil_acquire(); tl_cpu=c; g_cur_cpu=c; reg_cpu(c); gil_took(); }
void guest_unlock(void){ gil_gave(); pthread_mutex_unlock(&g_guest); }
static void guest_yield_begin(void){ gil_gave(); pthread_mutex_unlock(&g_guest); }
static void guest_thread_die(void);
static void guest_yield_end(void){ gil_acquire(); g_cur_cpu = tl_cpu; gil_took();
    if (tl_obj && ((Obj*)tl_obj)->terminate) guest_thread_die(); }
/* For runtime code that blocks on a guest thread outside Sleep/Wait* (metal_present
 * waiting for the screen): give the big lock up for the wait.  Returns 0, and does
 * nothing, on a thread that isn't a guest thread. */
int  guest_blocking_begin(void){ if (!tl_cpu) return 0; guest_yield_begin(); return 1; }
/* LOCK PRIORITY (b): called at every Win32, COM and indirect guest call by the thread holding the lock */
void guest_gil_check(void){
    if (!atomic_load_explicit(&g_gil_prio_wait, memory_order_relaxed)) return;
    if (gil_prio_off() || !tl_cpu || gil_is_prio() || tl_cs_depth) return;
    g_gil_handoffs++;
    guest_yield_begin();
    guest_yield_end();
}
void guest_blocking_end(void){ guest_yield_end(); }
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
/* ---- THE GAME'S OWN SLEEPS (Sleep/SleepEx, the timeSetEvent thread) ----------------
 * Two 100 Hz loops make the game's sound: the DirectSound mixer thread (0x7263c7:
 * timeBeginPeriod(1), then "next += 10 ms, mix, Sleep(next - now)") and the sound
 * service timer (0x6f5b4d, a one-shot timeSetEvent re-armed every 10 ms that wakes the
 * service thread).  Both mix per wake-up and catch up after a late one, but a long or
 * irregular wake-up empties the 0.5 s stream buffer.  They used to sleep with nanosleep,
 * which macOS coalesces (more so on QOS_CLASS_DEFAULT threads, which the game's worker
 * threads were): on a reporter's Mac (issue #12) both loops ran at ~32 Hz instead of
 * 100 with stalls -- the game wrote 130 KB/s of sound for 176 KB/s played, 28% of the
 * output was silence, the music cut out every few seconds.  Windows honours
 * timeBeginPeriod(1); a NOTE_CRITICAL kevent timer is not coalesced either (measured
 * here under full CPU load: nanosleep 0.9 ms late on average, max 25 ms; kevent
 * 0.03 ms, max 2 ms).  NFSU2_SLEEP_OLD=1 brings back nanosleep;
 * SLEEPSLACK=<us> (measurement) adds that much to every one of these sleeps, to
 * reproduce a system that wakes threads late. */
static int sleep_old(void){ static int v=-1; if (v<0){ const char *e=getenv("NFSU2_SLEEP_OLD"); v = (e && *e && *e!='0'); } return v; }
/* CATCH-UP (timer thread): a timer re-armed for <= 1 ms fires again at once, and a
 * SetEvent from a timer callback counts (an auto-reset event keeps up to TICK_CAP
 * pending wake-ups) -- a late wake-up or a wait for the big lock then delays the
 * game's sound ticks instead of dropping them.  Windows gets this for free: its timer
 * and the sound service thread run in parallel.  NFSU2_CATCHUP=0 turns it off. */
#define TICK_CAP 16
static int timer_catchup_off(void){ static int v=-1; if (v<0){ const char *e=getenv("NFSU2_CATCHUP"); v = (e && *e=='0') || sleep_old(); } return v; }
static long sleep_slack_ns(void){ static long v=-1; if (v<0){ const char *e=getenv("SLEEPSLACK"); v = e ? atol(e)*1000L : 0; } return v; }
static void game_sleep_ns(long ns){
    ns += sleep_slack_ns();
    if (!sleep_old()){ sleep_precise_ns(ns); return; }
    struct timespec ts = { (time_t)(ns / 1000000000L), ns % 1000000000L };
    nanosleep(&ts, 0);
}
/* SLEEP DEBT (Sleep/SleepEx): a thread that woke up late gets its next sleeps shortened
 * by the lateness -- skipped while the debt is larger than the request -- so its sleeps
 * average what it asked for.  The game's 100 Hz mixer loop catches up after a late
 * wake-up by asking for Sleep(1) until it is back on schedule; on a system that wakes
 * threads late every one of those 1 ms sleeps was late again and the loop never caught
 * up.  With precise sleeps the debt stays at tens of microseconds.  NFSU2_CATCHUP=0
 * turns it off. */
static _Thread_local int64_t tl_sleep_debt_ns = 0;
uint64_t g_sleep_skipped = 0;
static void game_sleep_debt_ns(long ns){
    if (timer_catchup_off()){ game_sleep_ns(ns); return; }
    if (tl_sleep_debt_ns >= ns){ tl_sleep_debt_ns -= ns; g_sleep_skipped++; sched_yield(); return; }
    long eff = ns - (long)tl_sleep_debt_ns;
    uint64_t t0 = wait_now_us();
    game_sleep_ns(eff);
    int64_t late = (int64_t)(wait_now_us() - t0) * 1000 - eff;
    tl_sleep_debt_ns = late < 0 ? 0 : late > 200000000LL ? 200000000LL : late;
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
uint64_t g_cs_enter = 0, g_cs_contended = 0;

static int cs_disabled(void){
    static int v = -1;
    if (v < 0){ const char *e = getenv("NOCS"); v = (e && *e && *e != '0') ? 1 : 0; }
    return v;
}
/* used: 0 empty, 1 live, 2 deleted (DeleteCriticalSection; the probe chain goes on
 * past it and a new section can take it).  Deleted sections used to stay: +2 per race
 * round trip, and a full table means no locking at all.  NFSU2_HANDLEFREE=0: old. */
static uint64_t g_cs_deleted = 0;
static pthread_mutex_t *cs_for(uint32_t key){
    if (cs_disabled()) return 0;          /* old behavior: no-op stub */
    pthread_mutex_lock(&g_cs_tab);
    unsigned h = (key >> 2) % MAXCS;
    int tomb = -1;
    for (unsigned i = 0; i < MAXCS; i++){
        unsigned j = (h + i) % MAXCS;
        if (g_cs[j].used == 1 && g_cs[j].key == key){
            pthread_mutex_unlock(&g_cs_tab); return &g_cs[j].m;
        }
        if (g_cs[j].used == 2){ if (tomb < 0) tomb = (int)j; continue; }
        if (!g_cs[j].used){ if (tomb < 0) tomb = (int)j; break; }
    }
    if (tomb >= 0){                /* a new section: the first deleted or empty slot */
        unsigned j = (unsigned)tomb;
        pthread_mutexattr_t a;
        pthread_mutexattr_init(&a);
        pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&g_cs[j].m, &a);
        pthread_mutexattr_destroy(&a);
        g_cs[j].key = key; g_cs[j].used = 1;
        pthread_mutex_unlock(&g_cs_tab); return &g_cs[j].m;
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

/* GILHOG=<hold ms>:<every ms> (measurement): every <every> ms the game thread keeps the
 * lock for <hold> ms, giving it up and taking it straight back every 50 us -- the
 * unfair re-take race of LOCK PRIORITY, made reproducible. */
static void gil_hog(void){
    static int h = -1, per = 0; static uint64_t next = 0;
    if (h < 0){ const char *e = getenv("GILHOG"); h = 0;
                if (e && *e){ h = atoi(e); const char *q = strchr(e, ':'); per = q ? atoi(q+1) : 100; } }
    if (!h || tl_obj || tl_timer_thread || !tl_cpu) return;
    uint64_t now = gil_now_us();
    if (now < next) return;
    next = now + (uint64_t)per * 1000u;
    uint64_t end = now + (uint64_t)h * 1000u;
    while (gil_now_us() < end){
        uint64_t t = gil_now_us(); while (gil_now_us() - t < 50u) ;
        guest_yield_begin(); guest_yield_end();
    }
}
void guest_fair_yield(void){
    gil_hog();
    guest_gil_check();
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

/* ---- FREEING HANDLES ------------------------------------------------------------
 * CloseHandle did nothing for events, mutexes, semaphores and threads (RegCloseKey for
 * registry keys), and a slot of the 511 was never reused: +4 per race round trip.
 * Now a closed object's slot is reused once nobody waits on it, a thread has ended
 * and 2 s have passed (a late SetEvent on a stale handle then finds nothing to break).
 * NFSU2_HANDLEFREE=0: the old behaviour. */
static int handlefree_off(void){ static int v=-1; if (v<0){ const char *e=getenv("NFSU2_HANDLEFREE"); v = e && *e=='0'; } return v; }
static uint64_t g_obj_reused = 0;
static uint32_t obj_new(ObKind k){
    pthread_mutex_lock(&g_lock);
    int i = 0;
    if (!handlefree_off()){
        uint64_t now = wait_now_us();
        for (int q = 1; q < g_obj_n; q++){
            Obj *c2 = &g_obj[q];
            if (!c2->kind || !c2->closed || atomic_load(&c2->busy) || now - c2->closed_us < 2000000u) continue;
            if (c2->kind == OB_THREAD && (!c2->started || c2->signaled <= 0)) continue;   /* still running */
            pthread_mutex_destroy(&c2->mtx); pthread_cond_destroy(&c2->cv);
            if (q < 64) memset(&g_wstat[q], 0, sizeof g_wstat[q]);
            i = q; g_obj_reused++; break;
        }
    }
    if (!i) i = g_obj_n < OBJ_MAX ? g_obj_n++ : 0;
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
static void obj_close(uint32_t h){                /* see FREEING HANDLES */
    Obj *o = obj_get(h);
    if (!o || handlefree_off()) return;
    pthread_mutex_lock(&g_lock);
    if (o->dups > 0) o->dups--;
    else if (!o->closed){ o->closed = 1; o->closed_us = wait_now_us(); }
    pthread_mutex_unlock(&g_lock);
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
    { static int sh = 0; if (thread_log() && sh++ < 200)
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
    /* The game's worker threads (the sound mixer and service threads, the loaders) at the
     * guest main thread's QoS.  pthread_create does NOT inherit it: they ran at
     * QOS_CLASS_DEFAULT, where macOS coalesces timers more and may park the thread on an
     * efficiency core -- the 100 Hz sound loops (see THE GAME'S OWN SLEEPS) depend on
     * waking on time.  Only one guest thread runs at a time (big lock), so this doesn't
     * take more cores.  NFSU2_SLEEP_OLD=1 leaves the default. */
    if (!sleep_old()) pthread_attr_set_qos_class_np(&at, QOS_CLASS_USER_INTERACTIVE, 0);
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
    cpu.top = 0; cpu.fcw = 0x037f; cpu.mxcsr = 0x1F80u;
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
    uint32_t delay_ms;        /* the delay the game asked for */
} Timer;

static Timer    g_tm[MAXTIMER];
static uint32_t g_timer_next_id = 1;
static int      g_timer_running = 0;
uint64_t        g_timer_ticks = 0;

/* Timers used GAME time (from when the virtual clock was 16.67 ms a frame).  The only
 * timer is the sound service (0x6f5b4d), which now reads real time (win32.c SOUND
 * PACE CLOCK), so the timers are due in real time too -- otherwise they would wait
 * for the virtual clock, which stands still in a long frame.  NFSU2_PACE=0: game time. */
uint64_t host_us(void);
int pace_on(void);
static uint64_t now_us(void){ return pace_on() ? wait_now_us() : host_us(); }

/* ---- TIMER STATS -----------------------------------------------------------
 * The game's sound service is clocked by a one-shot timeSetEvent that re-arms itself
 * (0x6f5b4d, 100 Hz): every tick SetEvents the sound service thread, which runs the
 * sound services once and waits again.  Ticks that come while the service thread hasn't
 * run yet merge (auto-reset event; see CATCH-UP).  Issue #12 log: 32 ticks a second,
 * the game wrote 130 KB/s for 176 KB/s played, 28% of the output silent.  Logged every
 * 30 s and at the end: tick rate, how late the timer thread woke, how long it and the
 * sound threads waited for the big lock (LOCK PRIORITY), catch-up and lost ticks. */
static uint64_t g_ts_n, g_ts_gil_us, g_ts_gil_max, g_ts_cb_us, g_ts_cb_max;
static uint64_t g_ts_late_us, g_ts_late_max, g_ts_late_n, g_ts_t0, g_ts_tlast;
static uint64_t g_ts_last_ticks, g_ts_last_merged, g_ts_catchup, g_ts_last_catchup;
static void timer_stat_print(const char *tag, uint64_t now){
    double el = (double)(now - g_ts_tlast) / 1e6;
    if (el <= 0) el = 1e-6;
    fprintf(stderr, "  [timer] %s %.0f s: %.1f ticks/s, wake late %.2f ms avg (max %.1f), lock wait %.2f ms avg (max %.1f), "
                    "callback %.3f ms avg (max %.1f), %.1f catch-up ticks/s, %.1f lost SetEvents/s\n",
            tag, (double)(now - g_ts_t0) / 1e6,
            (double)(g_timer_ticks - g_ts_last_ticks) / el,
            g_ts_late_n ? (double)g_ts_late_us / g_ts_late_n / 1000.0 : 0.0, g_ts_late_max / 1000.0,
            g_ts_n ? (double)g_ts_gil_us / g_ts_n / 1000.0 : 0.0, g_ts_gil_max / 1000.0,
            g_ts_n ? (double)g_ts_cb_us / g_ts_n / 1000.0 : 0.0, g_ts_cb_max / 1000.0,
            (double)(g_ts_catchup - g_ts_last_catchup) / el,
            (double)(g_set_merged - g_ts_last_merged) / el);
    g_ts_last_catchup = g_ts_catchup;
    if (!gil_prio_off()){ static uint64_t lh, ld;
      fprintf(stderr, "  [timer] %s %.0f s: sound threads' lock wait %.2f ms avg (max %.1f), %.1f hand-overs/s, %.1f step-backs/s\n",
              tag, (double)(now - g_ts_t0) / 1e6, g_pw_n ? (double)g_pw_us / g_pw_n / 1000.0 : 0.0, g_pw_max / 1000.0,
              (double)(g_gil_handoffs - lh) / el, (double)(g_gil_deferred - ld) / el);
      lh = g_gil_handoffs; ld = g_gil_deferred; g_pw_n = g_pw_us = g_pw_max = 0; }
    g_ts_n = g_ts_gil_us = g_ts_gil_max = g_ts_cb_us = g_ts_cb_max = 0;
    g_ts_late_us = g_ts_late_max = g_ts_late_n = 0;
    g_ts_last_ticks = g_timer_ticks; g_ts_last_merged = g_set_merged; g_ts_tlast = now;
}
static void timer_stat_fire(uint64_t gil, uint64_t cb){
    g_ts_n++; g_ts_gil_us += gil; g_ts_cb_us += cb;
    if (gil > g_ts_gil_max) g_ts_gil_max = gil;
    if (cb  > g_ts_cb_max)  g_ts_cb_max  = cb;
}
static void timer_stat_wake(uint64_t late){
    g_ts_late_n++; g_ts_late_us += late; if (late > g_ts_late_max) g_ts_late_max = late;
    uint64_t now = wait_now_us();
    if (!g_ts_t0){ g_ts_t0 = g_ts_tlast = now; return; }
    if (now - g_ts_tlast >= 30000000ull) timer_stat_print("at", now);
}
void timer_report(void){
    if (g_ts_t0) timer_stat_print("end,", wait_now_us());
    extern uint64_t g_sleep_skipped;
    fprintf(stderr, "  [timer] game sleeps skipped to repay late wake-ups (SLEEP DEBT): %llu\n", (unsigned long long)g_sleep_skipped);
    /* fixed-size tables that only grow (long-session check) */
    { int ncs = 0, nlive = 0;
      for (int i = 0; i < MAXCS; i++) if (g_cs[i].used == 1) ncs++;
      for (int i = 1; i < g_obj_n; i++) if (g_obj[i].kind && !g_obj[i].closed) nlive++;
      fprintf(stderr, "  win32 tables      : objects %d open, %d slots of %d used (%llu reused); critical sections %d of %d (%llu deleted)%s\n",
              nlive, g_obj_n - 1, OBJ_MAX - 1, (unsigned long long)g_obj_reused, ncs, MAXCS,
              (unsigned long long)g_cs_deleted, handlefree_off() ? "  [NFSU2_HANDLEFREE=0]" : ""); }
}

static void *timer_thread_main(void *p){
    (void)p;
    mem_altstack_install();
    tl_timer_thread = 1;
    CPU cpu; memset(&cpu,0,sizeof cpu);
    uint32_t stack_lo, stack_hi, teb;
    stack_lo = tstack_get();
    if (!stack_lo){
        fprintf(stderr, "  [thread] no room for another guest thread stack\n"); exit(7); }
    stack_hi = stack_lo + GTHREAD_STACK;
    teb = 0x00C60000u;
    mem_commit(stack_lo, GTHREAD_STACK);
    mem_commit(teb, 0x2000);
    cpu.top = 0; cpu.fcw = 0x037f; cpu.mxcsr = 0x1F80u;
    cpu.fs_base = teb; cpu.gs_base = teb;
    MR32(teb+0x00)=0xFFFFFFFFu; MR32(teb+0x04)=stack_hi; MR32(teb+0x08)=stack_lo;
    MR32(teb+0x18)=teb; MR32(teb+0x24)=999u;
    MR32(teb+0x2C)=teb+0x1000u; MR32(teb+0x30)=0x00C10000u;

    for (;;){
        /* Sleep until the earliest timer is due, at most 1 ms (the game's clock may jump
         * at a frame); see THE GAME'S OWN SLEEPS for why not nanosleep. */
        long want = 1000000L;
        { uint64_t nw = now_us(), due = 0;
          pthread_mutex_lock(&g_lock);
          for (int i = 0; i < MAXTIMER; i++)
              if (g_tm[i].due_us && (!due || g_tm[i].due_us < due)) due = g_tm[i].due_us;
          pthread_mutex_unlock(&g_lock);
          if (due && !sleep_old()){
              uint64_t left = due > nw ? due - nw : 0u;
              if (left < 1000u) want = left < 50u ? 50000L : (long)left * 1000L;
          } }
        uint64_t ws0 = wait_now_us();
        game_sleep_ns(want);
        { uint64_t ws = wait_now_us() - ws0, w = (uint64_t)want / 1000u; timer_stat_wake(ws > w ? ws - w : 0u); }
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
            uint64_t tl0 = wait_now_us();
            guest_lock(&cpu);
            uint64_t tl1 = wait_now_us();
            for (int burst = 0;; burst++){
                uint32_t id0 = g_timer_next_id;
                dispatch_indirect(&cpu, proc);
                g_timer_ticks++;
                /* CATCH-UP: the callback re-armed itself for <= 1 ms, i.e. it is behind its
                 * own schedule (0x6f5b4d: delay = due - now - 1, at least 1).  Fire it again
                 * right away, still holding the lock, instead of sleeping and waiting for the
                 * lock once per missed tick. */
                if (burst + 1 >= TICK_CAP || timer_catchup_off()) break;
                int again = -1;
                pthread_mutex_lock(&g_lock);
                for (int k = 0; k < MAXTIMER; k++)
                    if (g_tm[k].due_us && !g_tm[k].period_us && g_tm[k].proc == proc &&
                        g_tm[k].id >= id0 && g_tm[k].delay_ms <= 1u){ again = k; break; }
                if (again >= 0){ user = g_tm[again].user; id = g_tm[again].id; g_tm[again].due_us = 0; }
                pthread_mutex_unlock(&g_lock);
                if (again < 0) break;
                g_ts_catchup++;
                cpu.esp = stack_hi - 0x1000u;
                { CPU *c=&cpu;
                  PUSH32(0u); PUSH32(0u); PUSH32(user); PUSH32(0u); PUSH32(id);
                  PUSH32(0xDEAD7EADu); }
            }
            guest_unlock();
            timer_stat_fire(tl1 - tl0, wait_now_us() - tl1);
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
        /* HKLM\HARDWARE\DESCRIPTION\System\CentralProcessor\0 "~MHz".  The movie
         * player's timer (sub_006f625d) reads it; without it the timer calibrates
         * RDTSC against QueryPerformanceCounter, waiting for two measurements that
         * agree.  Our QPC is the virtual clock and RDTSC is real time, so they never
         * agree and the first movie hung for ever.  3000 matches rdtsc_shim (3 GHz). */
        else if (!strcasecmp(vn,"~MHz")){ dval=3000; have_d=1; }

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
    else if (!strcmp(n,"RegCloseKey")){ obj_close(A(c,0)); r=ERROR_SUCCESS; }
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
        if(o){ pthread_mutex_lock(&o->mtx);
               if (o->kind == OB_EVENT && !o->manual && tl_timer_thread && !timer_catchup_off()){
                   /* see CATCH-UP: a timer tick is not lost when its waiter hasn't run yet */
                   o->counting = 1;
                   if (o->signaled < TICK_CAP) o->signaled++; else g_set_merged++;
               } else {
                   if (o->signaled < 1) o->signaled = 1;
               }
               pthread_cond_broadcast(&o->cv); pthread_mutex_unlock(&o->mtx);} r=1; }
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
          static int sh=0, evlog=-1;
          if (evlog < 0){ const char *e=getenv("EVENTLOG"); evlog = e && *e && *e!='0'; }
          if (evlog && ++sh % 400 == 0){        /* EVENTLOG=1: it filled 18 MB of a 2-hour log */
            fprintf(stderr,"  [event] handle  type  signal  wait  SetEvent  waiting-tid\n");
            for (int i=0;i<64;i++) if (g_wstat[i].waits || g_wstat[i].sets)
                fprintf(stderr, "         h%-4d %3d %6d %8llu %9llu  tid=%u\n", i,
                        g_obj[i].kind, g_obj[i].signaled,
                        (unsigned long long)g_wstat[i].waits,
                        (unsigned long long)g_wstat[i].sets,
                        g_wstat[i].waiter_tid);
          } }
        if(!o){ r=0; }
        else {
            /* MUTEX: ownership + RECURSION (wait_ready).  The same thread must be able to take
             * the same mutex again; otherwise the game's CRT wrapper that takes nested locks
             * deadlocks itself (this is how the video player froze). */
            uint32_t me = cur_tid(c);
            uint64_t deadline = wait_deadline(ms, o->kind == OB_MUTEX && ms == 0xFFFFFFFFu ? ms : 2000u), us;
            atomic_fetch_add(&o->busy, 1);            /* see FREEING HANDLES */
            guest_yield_begin();
            pthread_mutex_lock(&o->mtx);
            while (!wait_ready(o, me) && !wait_thread_killed() && (us = wait_slice(deadline, 50000u))){
                struct timespec ts = { (time_t)(us / 1000000u), (long)(us % 1000000u) * 1000L };
                pthread_cond_timedwait_relative_np(&o->cv, &o->mtx, &ts);
            }
            r = wait_ready(o, me) ? 0u : 0x102u;        /* WAIT_OBJECT_0 / WAIT_TIMEOUT */
            if (!r) wait_take(o, me);
            pthread_mutex_unlock(&o->mtx);
            atomic_fetch_sub(&o->busy, 1);
            guest_yield_end();
        } }
    /* --- thread --- */
    else if (!strcmp(n,"CreateThread")){
        uint32_t entry=A(c,2), arg=A(c,3), ptid=A(c,5);
        { static int sc=0; if (thread_log() && sc++ < 200) fprintf(stderr, "  [thread] CreateThread entry=0x%08x arg=0x%08x flags=0x%x\n", entry, arg, A(c,4)); }
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
        { static int sh = 0; if (thread_log() && sh++ < 200)
            fprintf(stderr, "  [thread] TerminateThread tid=%u%s\n", to ? to->tid : 0u, to == tl_obj ? " (self)" : ""); }
        if (to && to->kind == OB_THREAD){
            if (to == (Obj*)tl_obj) guest_thread_die();
            to->terminate = 1;
        }
        r=1; }
    else if (!strcmp(n,"GetExitCodeThread")){ uint32_t p=A(c,1); if(p) MR32(p)=0; r=1; }
    else if (!strcmp(n,"SetThreadPriority")){
        /* recorded for LOCK PRIORITY; 0xFFFFFFFE = GetCurrentThread() */
        uint32_t h = A(c,0); int p = (int)A(c,1);
        Obj *to = h == 0xFFFFFFFEu ? (Obj*)tl_obj : obj_get(h);
        if (to && to->kind == OB_THREAD) to->prio = p;
        { static int sh = 0; if (sh++ < 16)
            fprintf(stderr, "  [thread] SetThreadPriority tid=%u priority=%d (from tid=%u, caller 0x%08x)\n",
                    to ? to->tid : 0u, p, cur_tid(c), MR32(c->esp - 4u)); }
        r=1; }
    else if (!strcmp(n,"SetThreadAffinityMask")||!strcmp(n,"SetProcessAffinityMask")) r=1;
    else if (!strcmp(n,"GetProcessAffinityMask")){
        uint32_t a=A(c,1),b=A(c,2); if(a) MR32(a)=0xFF; if(b) MR32(b)=0xFF; r=1; }
    else if (!strcmp(n,"SetPriorityClass")) r=1;
    else if (!strcmp(n,"GetPriorityClass")) r=0x20;
    /* --- handles / misc --- */
    else if (!strcmp(n,"Sleep") || !strcmp(n,"SleepEx")){
        /* see THE GAME'S OWN SLEEPS; Sleep(0): just yield (200 us) */
        uint32_t ms=A(c,0);
        guest_yield_begin();
        if (ms) game_sleep_debt_ns((long)ms * 1000000L);   /* see SLEEP DEBT */
        else    game_sleep_ns(200000L);
        guest_yield_end();
        r=0; }
    else if (!strcmp(n,"WaitForMultipleObjects")){
        uint32_t cnt=A(c,0), parr=A(c,1), waitall=A(c,2), ms=A(c,3);
        /* Unknown handles are skipped, as before.  Wait-all checks and takes every object
         * under all of their locks, taken in address order: a partial take could hand an
         * auto-reset event or a semaphore count to two waiters. */
        Obj *objs[64], *locks[64];
        uint32_t idx[64], nobj = 0, nl = 0, me = cur_tid(c);
        if (!cnt || cnt > 64u || !parr || !mem_is_committed(parr) || !mem_is_committed(parr + 4u*cnt - 1u)){
            r = 0xFFFFFFFFu;                                         /* WAIT_FAILED */
        } else {
            for (uint32_t i = 0; i < cnt; i++){
                Obj *o = obj_get(MR32(parr + 4u*i));
                if (!o) continue;
                objs[nobj] = o; idx[nobj++] = i;
                uint32_t j = 0;
                while (j < nl && locks[j] < o) j++;
                if (j < nl && locks[j] == o) continue;                /* the same object twice */
                memmove(locks + j + 1u, locks + j, (nl - j) * sizeof *locks);
                locks[j] = o; nl++;
            }
            uint64_t deadline = wait_deadline(ms, 200u), us;
            for (uint32_t i = 0; i < nl; i++) atomic_fetch_add(&locks[i]->busy, 1);   /* see FREEING HANDLES */
            guest_yield_begin();
            for (;;){
                uint32_t got = 0x102u;
                if (waitall && nl){
                    for (uint32_t i = 0; i < nl; i++) pthread_mutex_lock(&locks[i]->mtx);
                    uint32_t ready = 0;
                    for (uint32_t i = 0; i < nl; i++) ready += wait_ready(locks[i], me) != 0;
                    if (ready == nl){ for (uint32_t i = 0; i < nl; i++) wait_take(locks[i], me); got = 0; }
                    for (uint32_t i = nl; i > 0; i--) pthread_mutex_unlock(&locks[i-1u]->mtx);
                } else if (!waitall){
                    for (uint32_t k = 0; k < nobj && got == 0x102u; k++){
                        pthread_mutex_lock(&objs[k]->mtx);
                        if (wait_ready(objs[k], me)){ wait_take(objs[k], me); got = idx[k]; }
                        pthread_mutex_unlock(&objs[k]->mtx);
                    }
                }
                r = got;
                if (got != 0x102u || wait_thread_killed() || !(us = wait_slice(deadline, 1000u))) break;
                sleep_precise_ns((long)us * 1000L);       /* see THE GAME'S OWN SLEEPS */
            }
            for (uint32_t i = 0; i < nl; i++) atomic_fetch_sub(&locks[i]->busy, 1);
            guest_yield_end();
        } }
    else if (!strcmp(n,"SwitchToThread")){ guest_yield_begin(); sched_yield(); guest_yield_end(); r=1; }
    else if (!strcmp(n,"CloseHandle")){ obj_close(A(c,0)); r=1; }
    else if (!strcmp(n,"DuplicateHandle")){ uint32_t p=A(c,3); if(p) MR32(p)=A(c,1);
        { Obj *o = obj_get(A(c,1)); if (o){ pthread_mutex_lock(&g_lock); o->dups++; pthread_mutex_unlock(&g_lock); } }
        r=1; }
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
    else if (!strcmp(n,"timeGetTime")){ uint32_t game_ms_at(uint32_t); r = game_ms_at(c->eip); }   /* see win32.c SOUND PACE CLOCK */
    else if (!strcmp(n,"timeSetEvent")){
        /* (uDelay, uResolution, lpTimeProc, dwUser, fuEvent) */
        uint32_t delay=A(c,0), proc=A(c,2), user=A(c,3), flags=A(c,4);
        uint32_t id=0;
        if (proc && !getenv("NOTIMER")){
            pthread_mutex_lock(&g_lock);
            if (!g_timer_running){
                g_timer_running = 1;
                pthread_t th2; pthread_attr_t ta; pthread_attr_init(&ta);
                /* the game asks for 1 ms timers (timeBeginPeriod(1)): highest QoS, like the guest main thread */
                if (!sleep_old()) pthread_attr_set_qos_class_np(&ta, QOS_CLASS_USER_INTERACTIVE, 0);
                if (pthread_create(&th2,&ta,timer_thread_main,0)==0) pthread_detach(th2);
                else g_timer_running = 0;
                pthread_attr_destroy(&ta);
            }
            for (int i=0;i<MAXTIMER;i++){
                if (g_tm[i].due_us) continue;
                uint32_t us = delay ? delay*1000u : 1000u;
                g_tm[i].proc=proc; g_tm[i].user=user;
                g_tm[i].id = id = g_timer_next_id++;
                g_tm[i].period_us = (flags & 1u) ? us : 0u;
                g_tm[i].delay_ms = delay;
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
    else if (!strcmp(n,"DeleteCriticalSection")){
        if (!cs_disabled() && !handlefree_off()){
            uint32_t key = A(c,0);
            pthread_mutex_lock(&g_cs_tab);
            unsigned h = (key >> 2) % MAXCS;
            for (unsigned i = 0; i < MAXCS; i++){
                unsigned j = (h + i) % MAXCS;
                if (!g_cs[j].used) break;
                if (g_cs[j].used == 1 && g_cs[j].key == key){
                    /* only if nobody holds it (destroy says EBUSY otherwise) */
                    if (pthread_mutex_destroy(&g_cs[j].m) == 0){ g_cs[j].used = 2; g_cs_deleted++; }
                    break;
                }
            }
            pthread_mutex_unlock(&g_cs_tab);
        }
        r=1; }
    else return 0;                       /* this layer isn't interested */
    *out = r;
    return 1;
}
uint64_t win32_ext_threads(void){ return g_threads_started; }
uint64_t win32_ext_threads_exited(void){ return g_threads_exited; }
