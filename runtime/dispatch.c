#include <stdatomic.h>
/* Indirect call dispatch: guest address -> lifted native function.
 * O(1) direct index table (a uint32 array the size of .text).           */
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

/* statistics */
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
    if (!g_idx){ fprintf(stderr,"could not allocate the dispatch table\n"); exit(1); }
    uint32_t dup = 0, oob = 0;
    for (uint32_t i = 0; i < g_fn_count; i++){
        uint32_t a = g_fn_addr[i];
        if (a - TEXT_BASE >= TEXT_SIZE){ oob++; continue; }
        if (g_idx[a - TEXT_BASE]) dup++;
        g_idx[a - TEXT_BASE] = i + 1;
    }
    fprintf(stderr, "dispatch: %u functions registered (%u duplicates, %u out of range), table %.1f MB\n",
            g_fn_count, dup, oob, TEXT_SIZE*4.0/1048576.0);
}

/* --- more lifted modules: ASI plugins (runtime/asi.c) ---
 * A plugin's code is lifted into its own table (genall FNTAB_PREFIX) at its own
 * address range; calls into it arrive here like any other indirect call. */
typedef struct { uint32_t lo, size, n; const uint32_t *addr; const guest_fn *ptr; uint32_t *idx; } DMod;
static DMod g_dmod[4]; static int g_ndmod;
void dispatch_add_module(uint32_t lo, uint32_t size, const uint32_t *addr, const guest_fn *ptr, uint32_t n){
    if (g_ndmod == 4) return;
    DMod *m = &g_dmod[g_ndmod];
    m->lo = lo; m->size = size; m->n = n; m->addr = addr; m->ptr = ptr;
    m->idx = calloc(size, sizeof(uint32_t));
    if (!m->idx){ fprintf(stderr,"could not allocate a module dispatch table\n"); exit(1); }
    for (uint32_t i = 0; i < n; i++)
        if (addr[i] - lo < size) m->idx[addr[i] - lo] = i + 1;
    g_ndmod++;
    fprintf(stderr, "dispatch: module 0x%08x..0x%08x, %u functions\n", lo, lo + size, n);
}
/* guest function start at or below a, in a module; 0 if none */
static uint32_t dmod_nearest(uint32_t a){
    for (int k = 0; k < g_ndmod; k++){
        DMod *m = &g_dmod[k];
        if (a - m->lo >= m->size) continue;
        uint32_t lo = 0, hi = m->n, best = 0;
        while (lo < hi){ uint32_t md = (lo+hi)/2; if (m->addr[md] <= a){ best = m->addr[md]; lo = md+1; } else hi = md; }
        return best;
    }
    return 0;
}

int  win32_call(CPU*, uint32_t);
int  run_generated(CPU*, uint32_t);
void genexec_report(void);
int  com_call(CPU*, uint32_t);
int  g_guest_exited = 0;

/* --- run-time patches: change indirect call targets ---
 * Without regeneration (only for indirectly called functions). */
#define PATCH_MAX 24
static struct { uint32_t addr; guest_fn fn; } g_patch[PATCH_MAX];
static int g_patch_n = 0;
void dispatch_patch(uint32_t a, guest_fn f){
    if (a >= 0xE0000000u){        /* the order in dispatch_indirect relies on this */
        fprintf(stderr,"dispatch_patch: address %08x outside the text range, ignored\n", a);
        return; }
    if (g_patch_n < PATCH_MAX){ g_patch[g_patch_n].addr=a; g_patch[g_patch_n].fn=f; g_patch_n++; }
}

void dispatch_indirect(CPU *c, uint32_t addr){
    /* The CHEAP range test first.  COM vtable methods (0xE...) and Win32
     * import bridges (0xF...) are far above the guest text range, so they
     * NEVER collide with the patch table below -- patches are always inside
     * .text (dispatch_patch is only called with guest function addresses).
     * Previously every call first scanned the g_patch[] array linearly; a run
     * has 377 million COM calls alone. */
    if (addr >= 0xE0000000u){
        if (addr < 0xF0000000u){ com_call(c, addr); return; }
        if (win32_call(c, addr - 0xF0000000u) < 0) g_guest_exited = 1;
        return;
    }
    for (int i = 0; i < g_patch_n; i++)
        if (g_patch[i].addr == addr){ g_stat_indirect_ok++; g_patch[i].fn(c); return; }
    /* EFXSTAT: inventory of D3DX effect vtable calls (d3dx_hle.c) */
    { extern int g_efx_stat; extern void efx_stat_note(CPU *c, uint32_t addr);
      if (g_efx_stat && addr - 0x650000u < 0x40000u) efx_stat_note(c, addr); }
    uint32_t off = addr - TEXT_BASE;
    /* win32_ext.c LOCK PRIORITY: a waiting sound thread also gets the big lock here, so a
     * long stretch of game code without Win32 or COM calls (a load) doesn't keep it out */
    { extern _Atomic int g_gil_prio_wait;
      if (__builtin_expect(atomic_load_explicit(&g_gil_prio_wait, memory_order_relaxed) != 0, 0)){
          void guest_gil_check(void); guest_gil_check(); } }
    if (off < TEXT_SIZE){
        uint32_t k = g_idx[off];
        if (k){
            /* ICALL=<address>: count the INDIRECT calls made to that function and
             * dump the call chain for the first few.  Needed because virtual
             * calls don't show up in the static graph. */
            static uint32_t want = 0xFFFFFFFFu;
            if (want == 0xFFFFFFFFu){ const char *e = getenv("ICALL");
                                      want = e ? (uint32_t)strtoul(e,0,0) : 0; }
            if (want && addr == want){
                extern uint64_t g_icall_hits; g_icall_hits++;
                { static uint32_t seen[256]; static int ns=0;
                  uint32_t h2 = MR32(c->esp+8);
                  int f3=0; for(int q=0;q<ns;q++) if(seen[q]==h2) f3=1;
                  if(!f3 && ns<256){ seen[ns++]=h2;
                    fprintf(stderr,"  [binding] hash=0x%08X %s\n", h2,
                            h2==0x3EE7363Fu ? "  <<< WORLDVIEWPROJECTION" :
                           (h2==0x04191C37u ? "  <<< TEXTUREOFFSET" : "")); } }
                static int shown = 0;
                if (shown < 3){ shown++;
                    fprintf(stderr,"  [icall] 0x%08x called (arg1=%08x arg2=%08x arg3=%08x)\n",
                            addr, MR32(c->esp+4), MR32(c->esp+8), MR32(c->esp+12));
                    guest_backtrace(c, 10);
                }
            }
            g_stat_indirect_ok++; g_fn_ptr[k-1](c); return; }
    }
    for (int k = 0; k < g_ndmod; k++){
        uint32_t o2 = addr - g_dmod[k].lo;
        if (o2 < g_dmod[k].size){
            uint32_t j = g_dmod[k].idx[o2];
            if (j){ g_stat_indirect_ok++; g_dmod[k].ptr[j-1](c); return; }
            break;
        }
    }
    /* No lifted counterpart.  The game may have generated code at this address
     * AT RUN TIME (NFSU2's material pipeline does this heavily).
     * Try it with a small x86 interpreter; on an unknown instruction it returns
     * safely (restores esp) and falls through to the facility below. */
    { static int gx = -1;
      if (gx < 0){ const char *e = getenv("GENEXEC"); gx = (e && *e=='0') ? 0 : 1; }
      if (gx && mem_is_committed(addr) && run_generated(c, addr)){
          g_stat_indirect_ok++; return;
      } }

    g_stat_indirect_miss++;
    /* Caller: the return address is ALREADY ON THE STACK (before POP32).
     * g_watch_prev_ip used to be used, but it is only filled when instruction
     * tracing is on, so the report always showed 0x00000000. */
    { uint32_t ret = MR32(c->esp);
      if (g_miss_n < MISS_MAX){ g_miss_from[g_miss_n]=ret; g_miss[g_miss_n++]=addr; }
      static int shown = 0;
      if (shown < 5){ shown++;
          fprintf(stderr, "\n  [UNRESOLVED INDIRECT CALL] target=0x%08x  return=0x%08x\n"
                  "      eax=%08x ecx=%08x edx=%08x ebx=%08x esi=%08x edi=%08x\n"
                  "      arg1=%08x arg2=%08x arg3=%08x\n",
                  addr, ret, c->eax, c->ecx, c->edx, c->ebx, c->esi, c->edi,
                  MR32(c->esp+4), MR32(c->esp+8), MR32(c->esp+12));
          /* The bytes at the target: did the game generate CODE at run time (a thunk),
           * or is the pointer broken?  The quickest way to tell. */
          if (mem_is_committed(addr+15)){
              fprintf(stderr, "      96 bytes at the target:\n       ");
              for (int q = 0; q < 96; q++){
                  if (q && !(q & 15)) fprintf(stderr, "\n       ");
                  fprintf(stderr, " %02x", MR8(addr+q));
              }
              fprintf(stderr, "\n");
          } else fprintf(stderr, "      target NOT COMMITTED (not data, a garbage pointer)\n");
          guest_backtrace(c, 12);
      } }
    POP32(c->eip);            /* clear the return address, swallow the call */
}

void unsupported_insn(CPU *c, uint32_t addr, const char *mn){
    (void)c; (void)mn;
    g_stat_unsupported++;
    if (g_unsup_n < MISS_MAX) g_unsup[g_unsup_n++] = addr;
}

void cpuid_shim(CPU *c){
    /* a generic Pentium III-like: SSE yes, 3DNow no */
    switch (c->eax){
    case 0: c->eax=1; c->ebx=0x756e6547; c->edx=0x49656e69; c->ecx=0x6c65746e; break;
    case 1: c->eax=0x000006d8; c->ebx=0; c->ecx=0; c->edx=0x0383fbff; break;
    default: c->eax=c->ebx=c->ecx=c->edx=0; break;
    }
}
/* RDTSC: a real time stamp counter at a nominal 3.0 GHz.  It used to add a fixed
 * 3000 per call, so anything that times rdtsc against the clock measured an
 * absurdly slow CPU: the game's first-run hardware detection (sub_005c0ae0: TSC
 * over 50 ms -> MHz, then sub_005cde50 scores CPU MHz + GPU) read ~0 MHz and put
 * EVERY graphics setting at its minimum (world detail 0 -> visible pop-in, no
 * crowds, reflections, shadows, fog...).  The other users are profiling counters
 * and the audio library's CPU calibration; nothing drives game time with it (that
 * is QueryPerformanceCounter, our virtual clock).  RDTSC=fixed: old behaviour. */
#include <mach/mach_time.h>
uint64_t rdtsc_shim(void){
    static int mode = -1;
    if (mode < 0){ const char *e = getenv("RDTSC"); mode = (e && !strcmp(e, "fixed")) ? 0 : 1; }
    if (!mode){ static uint64_t t = 0; return (t += 3000); }
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    uint64_t ticks = mach_absolute_time();
    /* ns * 3: split to keep ticks * numer from overflowing on long runs */
    return (ticks / tb.denom) * tb.numer * 3u + (ticks % tb.denom) * tb.numer * 3u / tb.denom;
}

void dispatch_report(void){
    fprintf(stderr,"\n=== runtime ===\n");
    fprintf(stderr,"  indirect calls resolved  : %llu\n", (unsigned long long)g_stat_indirect_ok);
    fprintf(stderr,"  indirect calls unresolved: %llu\n", (unsigned long long)g_stat_indirect_miss);
    fprintf(stderr,"  unsupported instructions : %llu\n", (unsigned long long)g_stat_unsupported);
    genexec_report();
    if (g_miss_n){ fprintf(stderr,"  unresolved indirect calls (target <- calling instruction):\n");
        int shown=0;
        for(int i=0;i<g_miss_n && shown<12;i++){
            int dup=0; for(int j=0;j<i;j++) if(g_miss_from[j]==g_miss_from[i]) dup=1;
            if(dup) continue;
            fprintf(stderr,"    0x%08x  <-  0x%08x\n", g_miss[i], g_miss_from[i]); shown++; }
    }
    if (g_unsup_n){ fprintf(stderr,"  first trap addresses     :"); for(int i=0;i<g_unsup_n && i<10;i++) fprintf(stderr," 0x%08x",g_unsup[i]); fprintf(stderr,"\n"); }
}

/* --- guest stack tracer: resolves return addresses in the .text range --- */
static uint32_t nearest_fn(uint32_t a){
    uint32_t lo=0, hi=g_fn_count, best=0;
    while(lo<hi){ uint32_t m=(lo+hi)/2;
        if(g_fn_addr[m]<=a){ best=g_fn_addr[m]; lo=m+1; } else hi=m; }
    return best;
}
void guest_backtrace(CPU *c, int maxn){
    fprintf(stderr,"    guest stack trace (esp=0x%08x):\n", c->esp);
    int shown=0;
    for (uint32_t k=0; k<2048 && shown<maxn; k++){
        uint32_t a = c->esp + 4u*k;
        if (!mem_is_committed(a)) continue;      /* skip unmapped pages */
        uint32_t v = MR32(a);
        uint32_t fm = dmod_nearest(v);
        if (fm){
            fprintf(stderr,"      [%2d] +0x%04x  return 0x%08x   (plugin sub_%08x + 0x%x)\n",
                    shown, k*4, v, fm, v-fm);
            shown++; continue;
        }
        if (v - TEXT_BASE < TEXT_SIZE){
            uint32_t f = nearest_fn(v);
            fprintf(stderr,"      [%2d] +0x%04x  return 0x%08x   (sub_%08x + 0x%x)\n",
                    shown, k*4, v, f, v-f);
            shown++;
        }
    }
}

/* Resolve the generated guest function from a faulting host PC: g_fn_ptr maps
 * host addresses to guest addresses; the nearest function start below the PC
 * is searched for. */
uint32_t guest_fn_from_host(const void *pc){
    uintptr_t p = (uintptr_t)pc, best = 0; uint32_t bestg = 0;
    for (uint32_t i = 0; i < g_fn_count; i++){
        uintptr_t f = (uintptr_t)g_fn_ptr[i];
        if (f <= p && f > best){ best = f; bestg = g_fn_addr[i]; }
    }
    return bestg;
}
