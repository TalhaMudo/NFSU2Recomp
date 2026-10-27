/* Guest heap (HeapAlloc / GlobalAlloc / LocalAlloc / VirtualAlloc).
 *
 * It used to be a bump allocator whose HeapFree did nothing: real play with
 * the u2net mod + HD textures filled the 496 MB arena at ~1 MB/s and a worker
 * thread crashed after ~8 minutes on a failed allocation.  Now freed blocks
 * are reused:
 *   - every block keeps the old 16-byte header (+0 user size, +4 0xA110C8ED),
 *     so HeapSize and the diagnostic block walkers (com.c, overrides.c) work
 *     as before; +8 = block bytes incl. header, +12 = ~(block bytes);
 *   - small blocks (<= 4 KB) go to free lists by size class (16 bytes),
 *     larger ones to an address-sorted extent list that coalesces neighbours
 *     and gives the top of the arena back to the bump pointer;
 *   - a freed block first sits in a QUARANTINE FIFO (32 MB by default) before
 *     it can be reused: the game may still touch memory it just freed, which
 *     was harmless while nothing was reused;
 *   - a free of a block that isn't live (double free, bad pointer, corrupt
 *     header) is logged and ignored (the block just leaks).
 *
 * NFSU2_HEAP_QUAR_MB=<n>  quarantine size (0 = reuse at once)
 * NFSU2_HEAP_FREE=0       old behaviour (frees ignored), for A/B
 * HEAPPOISON=1            fill freed blocks with 0xFEEEFEEE and check the
 *                         pattern when they leave the quarantine
 * NFSU2_HEAPLOG=<s>       a status line every s seconds (default 60, 0 off)
 */
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

#define GH_BASE  0x10000000u
#define GH_MAX   0x2F000000u
#define HDR      16u
#define SIG_USED 0xA110C8EDu
#define SIG_QUAR 0xF4EEC0DEu      /* freed, in quarantine */
#define SIG_FREE 0xF4EEF4EEu      /* freed, in a free list / extent */
#define SMALL    4096u            /* block bytes incl. header */
#define NCLASS   (SMALL / 16u + 1u)
#define POISON   0xFEEEFEEEu

static pthread_mutex_t g_lk = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_top = GH_BASE, g_committed_top = GH_BASE;

/* small free lists: host-side stacks of block addresses */
typedef struct { uint32_t *a; uint32_t n, cap; } Stack;
static Stack g_cls[NCLASS];
/* large free extents, sorted by address */
static uint32_t *g_ext_a, *g_ext_s, g_ext_n, g_ext_cap;
/* quarantine ring */
static uint32_t *g_q; static uint32_t g_q_cap, g_q_head, g_q_n;
static uint64_t g_q_bytes, g_q_lim;

static int g_free_on = -1, g_poison;
static uint64_t st_alloc, st_free, st_reuse, st_fail, st_double, st_bad, st_corrupt,
                st_inplace, st_poison_hit, st_live, st_peak_live, st_peak_top,
                st_free_bytes;

static void cfg(void){
    if (g_free_on >= 0) return;
    const char *e = getenv("NFSU2_HEAP_FREE");
    g_free_on = !(e && *e == '0');
    e = getenv("NFSU2_HEAP_QUAR_MB");
    g_q_lim = (uint64_t)((e && *e) ? strtoul(e, 0, 0) : 32ul) << 20;
    e = getenv("HEAPPOISON");
    g_poison = (e && *e && *e != '0');
    g_q_cap = 1u << 18;
    g_q = malloc(sizeof *g_q * g_q_cap);
}

static void push(Stack *s, uint32_t v){
    if (s->n == s->cap){ s->cap = s->cap ? s->cap * 2u : 64u; s->a = realloc(s->a, sizeof *s->a * s->cap); }
    s->a[s->n++] = v;
}

static void set_hdr(uint32_t b, uint32_t user, uint32_t sig, uint32_t bytes){
    MR32(b) = user; MR32(b+4) = sig; MR32(b+8) = bytes; MR32(b+12) = ~bytes;
}

/* ---- large extents ---- */
static uint32_t ext_find(uint32_t a){            /* first index with addr >= a */
    uint32_t lo = 0, hi = g_ext_n;
    while (lo < hi){ uint32_t m = (lo + hi) / 2u; if (g_ext_a[m] < a) lo = m + 1u; else hi = m; }
    return lo;
}
static void ext_del(uint32_t i){
    memmove(g_ext_a + i, g_ext_a + i + 1, (g_ext_n - i - 1u) * 4u);
    memmove(g_ext_s + i, g_ext_s + i + 1, (g_ext_n - i - 1u) * 4u);
    g_ext_n--;
}
static void ext_add(uint32_t a, uint32_t s){
    uint32_t i = ext_find(a);
    if (i > 0 && g_ext_a[i-1] + g_ext_s[i-1] == a){          /* merge with the one below */
        i--; a = g_ext_a[i]; s += g_ext_s[i]; ext_del(i);
    }
    if (i < g_ext_n && a + s == g_ext_a[i]){                 /* and the one above */
        s += g_ext_s[i]; ext_del(i);
    }
    if (a + s == g_top){ g_top = a; return; }                /* back to the wilderness */
    if (g_ext_n == g_ext_cap){
        g_ext_cap = g_ext_cap ? g_ext_cap * 2u : 256u;
        g_ext_a = realloc(g_ext_a, 4u * g_ext_cap); g_ext_s = realloc(g_ext_s, 4u * g_ext_cap);
    }
    memmove(g_ext_a + i + 1, g_ext_a + i, (g_ext_n - i) * 4u);
    memmove(g_ext_s + i + 1, g_ext_s + i, (g_ext_n - i) * 4u);
    g_ext_a[i] = a; g_ext_s[i] = s; g_ext_n++;
    set_hdr(a, 0, SIG_FREE, s);
}

/* a block leaves the quarantine (or skips it) and becomes reusable */
static void release(uint32_t b){
    uint32_t bytes = MR32(b+8);
    if (g_poison){
        uint32_t n = MR32(b), bad = 0xFFFFFFFFu;
        for (uint32_t o = 0; o + 4u <= n; o += 4u)
            if (MR32(b + HDR + o) != POISON){ bad = o; break; }
        if (bad != 0xFFFFFFFFu && st_poison_hit++ < 20)
            fprintf(stderr, "  [heap] written after free: block 0x%08x (%u bytes) at +%u = 0x%08x\n",
                    b + HDR, n, bad, MR32(b + HDR + bad));
    }
    st_free_bytes += bytes;
    if (bytes <= SMALL){ set_hdr(b, 0, SIG_FREE, bytes); push(&g_cls[bytes / 16u], b); }
    else { uint32_t t = g_top; ext_add(b, bytes);
           if (g_top != t) st_free_bytes -= t - g_top; }
}

static void quarantine(uint32_t b){
    uint32_t bytes = MR32(b+8);
    MR32(b+4) = SIG_QUAR;
    if (g_poison){ uint32_t n = MR32(b);
                   for (uint32_t o = 0; o + 4u <= n; o += 4u) MR32(b + HDR + o) = POISON; }
    if (!g_q_lim){ release(b); return; }
    g_q[(g_q_head + g_q_n) & (g_q_cap - 1u)] = b; g_q_n++; g_q_bytes += bytes;
    while (g_q_n && (g_q_bytes > g_q_lim || g_q_n == g_q_cap)){
        uint32_t o = g_q[g_q_head]; g_q_head = (g_q_head + 1u) & (g_q_cap - 1u); g_q_n--;
        g_q_bytes -= MR32(o+8);
        release(o);
    }
}

/* the extent with the smallest size >= need (or 0) */
static int take_extent(uint32_t need, uint32_t *out){
    uint32_t best = 0xFFFFFFFFu, bs = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < g_ext_n; i++)
        if (g_ext_s[i] >= need && g_ext_s[i] < bs){ bs = g_ext_s[i]; best = i; if (bs == need) break; }
    if (best == 0xFFFFFFFFu) return 0;
    uint32_t a = g_ext_a[best], rest = bs - need;
    *out = a;
    if (rest > SMALL){ g_ext_a[best] = a + need; g_ext_s[best] = rest; set_hdr(a + need, 0, SIG_FREE, rest); }
    else { ext_del(best);
           if (rest){ set_hdr(a + need, 0, SIG_FREE, rest); push(&g_cls[rest / 16u], a + need); } }
    st_free_bytes -= need;
    return 1;
}

static void heaplog(void);

uint32_t gheap_alloc(uint32_t n, int zero){
    uint32_t need = (n + HDR + 15u) & ~15u, b = 0;
    if (need < n) { st_fail++; return 0; }
    pthread_mutex_lock(&g_lk);
    cfg();
    if (need <= SMALL && g_cls[need / 16u].n){
        b = g_cls[need / 16u].a[--g_cls[need / 16u].n]; st_free_bytes -= need; st_reuse++;
    } else if (g_ext_n && take_extent(need, &b)){
        st_reuse++;
    } else if (g_top + need <= GH_MAX && g_top + need > g_top){
        b = g_top; g_top += need;
        if (g_top > g_committed_top){
            uint32_t to = (g_top + 0xFFFFu) & ~0xFFFFu;
            if (to > GH_MAX) to = GH_MAX;
            mem_commit(g_committed_top, to - g_committed_top); g_committed_top = to;
        }
        if (g_top > st_peak_top) st_peak_top = g_top;
    } else { st_fail++; pthread_mutex_unlock(&g_lk);
             fprintf(stderr, "  [heap] OUT OF MEMORY: %u bytes (live %llu KB, top 0x%08x)\n",
                     n, (unsigned long long)(st_live >> 10), g_top);
             return 0; }
    set_hdr(b, n, SIG_USED, need);
    st_alloc++; st_live += need; if (st_live > st_peak_live) st_peak_live = st_live;
    if (zero) memset(g_mem + b + HDR, 0, n);
    if ((st_alloc & 4095u) == 0) heaplog();
    pthread_mutex_unlock(&g_lk);
    return b + HDR;
}

static int live_block(uint32_t u){
    if (u < GH_BASE + HDR || u >= g_top || (u & 15u)) return 0;
    return MR32(u - HDR + 4) == SIG_USED;
}

uint32_t gheap_size(uint32_t u){
    if (!u || u < GH_BASE + HDR || u >= GH_MAX || !mem_is_committed(u - HDR + 4)) return 0;
    if (MR32(u - HDR + 4) != SIG_USED) return 0;
    return MR32(u - HDR);
}

/* 1 = freed (or ignored as a no-op), 0 = not a block of ours */
int gheap_free(uint32_t u, const char *who){
    if (!u) return 1;
    pthread_mutex_lock(&g_lk);
    cfg();
    if (!live_block(u)){
        uint32_t sig = (u >= GH_BASE + HDR && u < g_top && !(u & 15u)) ? MR32(u - HDR + 4) : 0;
        int dbl = (sig == SIG_QUAR || sig == SIG_FREE);
        uint64_t k = dbl ? st_double++ : st_bad++;
        if (k < 10) fprintf(stderr, "  [heap] %s(0x%08x): %s, ignored\n", who, u,
                            dbl ? "double free" : "not a heap block");
        pthread_mutex_unlock(&g_lk);
        return 0;
    }
    uint32_t b = u - HDR, bytes = MR32(b+8);
    if (bytes != ~MR32(b+12) || bytes < MR32(b) + HDR || (bytes & 15u) || bytes > g_top - b){
        if (st_corrupt++ < 10) fprintf(stderr, "  [heap] %s(0x%08x): corrupt header (size %u, block %u), leaked\n",
                                       who, u, MR32(b), bytes);
        pthread_mutex_unlock(&g_lk);
        return 1;
    }
    st_free++; st_live -= bytes;
    if (g_free_on) quarantine(b); else MR32(b+4) = SIG_USED;   /* A/B: leak as before */
    pthread_mutex_unlock(&g_lk);
    return 1;
}

/* HeapReAlloc: in place when the block is big enough or ends at the top of
 * the arena; otherwise new block + copy + free of the old one. */
uint32_t gheap_realloc(uint32_t o, uint32_t n, int zero, int in_place_only){
    if (!o) return in_place_only ? 0 : gheap_alloc(n, zero);
    pthread_mutex_lock(&g_lk);
    if (live_block(o)){
        uint32_t b = o - HDR, bytes = MR32(b+8), on = MR32(b);
        uint32_t need = (n + HDR + 15u) & ~15u;
        int ok = need >= n && need <= bytes;
        if (!ok && need >= n && b + bytes == g_top && b + need <= GH_MAX && b + need > b){
            g_top = b + need;
            if (g_top > g_committed_top){
                uint32_t to = (g_top + 0xFFFFu) & ~0xFFFFu; if (to > GH_MAX) to = GH_MAX;
                mem_commit(g_committed_top, to - g_committed_top); g_committed_top = to; }
            if (g_top > st_peak_top) st_peak_top = g_top;
            st_live += need - bytes; if (st_live > st_peak_live) st_peak_live = st_live;
            set_hdr(b, n, SIG_USED, need); ok = 1;
        }
        if (ok){
            MR32(b) = n;
            if (zero && n > on) memset(g_mem + o + on, 0, n - on);
            st_inplace++;
            pthread_mutex_unlock(&g_lk);
            return o;
        }
        pthread_mutex_unlock(&g_lk);
        if (in_place_only) return 0;
        uint32_t p = gheap_alloc(n, zero);
        if (p){ memcpy(g_mem + p, g_mem + o, on < n ? on : n); gheap_free(o, "HeapReAlloc"); }
        return p;
    }
    pthread_mutex_unlock(&g_lk);
    /* not one of our blocks: old behaviour (copy n bytes, keep the old one) */
    if (in_place_only) return 0;
    uint32_t p = gheap_alloc(n, zero);
    if (p && n && mem_is_committed(o) && mem_is_committed(o + n - 1u)) memcpy(g_mem + p, g_mem + o, n);
    return p;
}

uint32_t gheap_top_used(void){ return g_top - GH_BASE; }
uint64_t gheap_failed(void){ return st_fail; }

static void line(const char *tag){
    extern uint32_t com_gbuf_used(void);
    fprintf(stderr, "  [heap]%s live %llu KB (peak %llu), arena top %u KB (peak %llu), "
            "free %llu KB (%u extents), quarantine %llu KB; %llu allocs, %llu frees, %llu reused, "
            "%llu realloc in place; %llu double / %llu bad / %llu corrupt frees, %llu failed%s; "
            "buffer heap %u KB\n", tag,
            (unsigned long long)(st_live >> 10), (unsigned long long)(st_peak_live >> 10),
            (g_top - GH_BASE) >> 10, (unsigned long long)((st_peak_top ? st_peak_top - GH_BASE : 0) >> 10),
            (unsigned long long)(st_free_bytes >> 10), g_ext_n, (unsigned long long)(g_q_bytes >> 10),
            (unsigned long long)st_alloc, (unsigned long long)st_free, (unsigned long long)st_reuse,
            (unsigned long long)st_inplace, (unsigned long long)st_double, (unsigned long long)st_bad,
            (unsigned long long)st_corrupt, (unsigned long long)st_fail,
            g_free_on == 0 ? " (FREE OFF)" : "", com_gbuf_used() >> 10);
    if (g_poison) fprintf(stderr, "  [heap] poison: %llu blocks written after free\n",
                          (unsigned long long)st_poison_hit);
}

static void heaplog(void){
    static int every = -1; static time_t t0, last;
    if (every < 0){ const char *e = getenv("NFSU2_HEAPLOG"); every = (e && *e) ? atoi(e) : 60;
                    t0 = last = time(0); }
    if (every <= 0) return;
    time_t now = time(0);
    if (now - last < every) return;
    last = now;
    char tag[32]; snprintf(tag, sizeof tag, " t=%lds", (long)(now - t0));
    line(tag);
}

void gheap_report(void){
    pthread_mutex_lock(&g_lk); cfg(); line(""); pthread_mutex_unlock(&g_lk);
}
