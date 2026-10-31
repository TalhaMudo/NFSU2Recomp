/* COM bridge: offers the guest COM objects with native implementations.
 * Guest object: [obj+0]=vtable.  vtable entry: 0xE0000000|(iface<<16)|method
 * All methods are stdcall, the first argument is 'this'.               */
#include <time.h>
#include <stdatomic.h>
#include "cpu.h"
#include "d3d9_tables.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <unistd.h>
#include <pthread.h>

/* getenv() SCANS the environment array and takes a lock on every call.  On
 * per-draw paths (for example the "PDLIST" check in SetVertexShaderConstantF
 * was the FIRST term of an && chain) it has a measurable cost: in the profile
 * getenv+__findenv_locked ate 3% of the guest thread.
 * Environment variables don't change during a run -> resolve once, keep. */
#define ENVC(nm_) ({ static const char *_ev_; static int _ei_; \
                     if (!_ei_){ _ei_ = 1; _ev_ = getenv(nm_); } _ev_; })


void mem_commit(uint32_t,uint32_t);
void guest_backtrace(CPU*,int);
void dispatch_indirect(CPU*,uint32_t);
static int g_verbose = -1;
static int VB(void){ if(g_verbose<0) g_verbose = ENVC("VERBOSE")?1:0; return g_verbose; }
/* Metal bridge (metal.m) - a no-op if absent */
int  metal_available(void);
void metal_set_clear_color(unsigned argb);
void metal_present(void);
long metal_frames(void);
long metal_tris(void);
long metal_frames_dropped(void);
long metal_tex_created(void);
long metal_tex_uploaded(void);
void metal_begin_frame(void);
#include "gfx.h"
#include "vsemu.h"
#include "enhance.h"

/* The COM dispatch compares the method NAME: a run has 144 MILLION com_calls
 * and each one did dozens of strcmps in an if-else chain (in the profile
 * strcmp+_platform_strcmp were 3.6% of the guest thread, com_call itself 9.3%).
 * The name is already known from the (interface, slot) pair; but instead of
 * rewriting the whole chain, make the comparison CHEAP: almost all matches are
 * eliminated at the first character, so don't make the call at all. */
#define NMEQ(nmv_, lit_) ((nmv_)[0] == (lit_)[0] && !strcmp((nmv_), (lit_)))
unsigned metal_create_texture(unsigned w, unsigned h, unsigned levels, unsigned d3dfmt);
unsigned metal_create_texture_ex(unsigned w, unsigned h, unsigned levels,
                                 unsigned d3dfmt, unsigned is_rt);
void     metal_set_render_target(unsigned texid);
void     metal_clear_target(unsigned argb);
void     metal_set_blend(unsigned key, float alpha_ref);
void     metal_upload_texture(unsigned id_, unsigned level, const void *data,
                              unsigned pitch, unsigned w, unsigned h, unsigned d3dfmt);
unsigned metal_level_pitch(unsigned w, unsigned level, unsigned d3dfmt);
unsigned metal_level_rows(unsigned h, unsigned level, unsigned d3dfmt);
void     metal_fill_keyboard(unsigned char *dst, unsigned n);
void     metal_fill_mouse(unsigned char *dst, unsigned n);
void     metal_get_cursor(int *x, int *y);
#define NEL(a) ((unsigned)(sizeof(a)/sizeof(*(a))))

#define COM_MAGIC 0xE0000000u
#define COM_BASE  0x30000000u
#define COM_MAX   0x38000000u
static uint32_t com_ptr = COM_BASE;
static uint32_t com_alloc(uint32_t n){
    n=(n+15)&~15u;
    if (com_ptr+n > COM_MAX) return 0;
    uint32_t p=com_ptr; com_ptr+=n; mem_commit(p,n); memset(g_mem+p,0,n); return p;
}
/* general allocator on the guest side (for Lock etc.) over 0x38000000..0x7DE00000
 * (1.1 GB of address space; macOS backs only the pages the game writes).  The game
 * Releases textures, vertex and index buffers (it never AddRefs them), and the
 * Release path hands their data back.  A freed piece first sits in a quarantine
 * FIFO for GBUF_QUAR presents, because the GPU path copies guest data a frame or so
 * after the draw and a released resource may still be bound.  Then (v2.2) it joins
 * an address-ordered set of free extents: neighbours merge, the top of the area goes
 * back to the bump pointer, and a request takes the first extent that fits from
 * size bins (exact bins up to 16 KB, 8 per power of two above) and splits the rest
 * off.  Before v2.2 a piece was reused only for a request of exactly its size, so
 * the top climbed ~30 MB per race while the live data stayed ~130 MB.
 * NFSU2_GBUF_MERGE=0: the old exact-size reuse (A/B). */
static uint64_t g_present;
#define GBUF_BASE 0x38000000u
#define GBUF_MAX  0x7DE00000u     /* 2 MB gap: gbuf_alloc commits a 1 MB margin */
#define GBUF_UNITS ((GBUF_MAX - GBUF_BASE) >> 6)
/* A released resource stays alive in Direct3D while it is still bound to the device, so the
 * data must outlive the Release by a good margin (NFSU2_GBUF_QUAR presents, default 1500 = ~15 s). */
static uint32_t gbuf_quar(void){ static int q=-1; if (q<0){ const char *e=getenv("NFSU2_GBUF_QUAR"); q = e ? atoi(e) : 1500; if (q<1) q=1; } return (uint32_t)q; }
#define GBUF_NB   4096            /* exact-size buckets (NFSU2_GBUF_MERGE=0) */
#define GBUF_NBIN 400             /* size bins of the free extents */
enum { GN_SPARE, GN_QUAR, GN_FREE, GN_BUCKET };
static uint32_t gbuf_ptr = GBUF_BASE, g_gb_committed = GBUF_BASE;
static uint32_t *g_gsz;                         /* (addr-BASE)>>6 -> size of the live block */
static uint32_t *g_gfx;                         /* first and last unit of a free extent -> node+1 */
static struct GFree { uint32_t addr, size, stamp; int next, prev; int state; } *g_gfree;
static int g_gfree_cap = 0, g_gfree_unused = -1;
static int g_gfree_head[GBUF_NB];                        /* exact-size buckets */
static int g_gbin[GBUF_NBIN]; static uint64_t g_gbinmap[(GBUF_NBIN + 63) / 64];
static int g_gq_head = -1, g_gq_tail = -1;               /* quarantine FIFO (stamp order) */
static int g_gb_merge = 1;
static pthread_mutex_t g_gb_lk = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_gb_freed = 0, g_gb_reused = 0, g_gb_split = 0, g_gb_bytes_freed = 0, g_gb_bad = 0;
static uint32_t g_gb_live = 0, g_gb_live_peak = 0, g_gb_top_peak = 0;
static uint32_t g_gb_quar_bytes = 0, g_gb_free_bytes = 0, g_gb_next = 0;
/* gbuf data never handed back (counted for the report): 0 shader/decl copies,
 * 1 sound buffers, 2 surface lock buffers, 3 other (DirectInput, index scratch) */
static uint64_t g_gb_kept[4];
uint32_t com_gbuf_used(void){ return gbuf_ptr - GBUF_BASE; }
void com_gbuf_fmt(char *s, size_t n){
    snprintf(s, n, "%u KB (top peak %u, live %u, peak live %u, free %u in %u extents, quarantine %u%s)",
             (gbuf_ptr - GBUF_BASE) >> 10, g_gb_top_peak >> 10, g_gb_live >> 10, g_gb_live_peak >> 10,
             g_gb_free_bytes >> 10, g_gb_next, g_gb_quar_bytes >> 10, g_gb_merge ? "" : ", MERGE OFF");
}
static int gnode_get(void){
    if (g_gfree_unused < 0){
        int o = g_gfree_cap; g_gfree_cap = o ? o * 2 : (1 << 16);
        g_gfree = realloc(g_gfree, (size_t)g_gfree_cap * sizeof *g_gfree);
        if (!g_gfree){ fprintf(stderr, "  [gbuf] node pool: out of host memory\n"); abort(); }
        for (int i = g_gfree_cap - 1; i >= o; i--){ g_gfree[i].state = GN_SPARE; g_gfree[i].next = g_gfree_unused; g_gfree_unused = i; }
    }
    int k = g_gfree_unused; g_gfree_unused = g_gfree[k].next;
    return k;
}
static void gnode_put(int k){ g_gfree[k].state = GN_SPARE; g_gfree[k].next = g_gfree_unused; g_gfree_unused = k; }
static void gbuf_init(void){
    if (g_gsz) return;
    const char *e = getenv("NFSU2_GBUF_MERGE");
    g_gb_merge = !(e && *e == '0');
    g_gsz = calloc(GBUF_UNITS, 4);
    if (g_gb_merge) g_gfx = calloc(GBUF_UNITS, 4);
    for (int i = 0; i < GBUF_NB; i++) g_gfree_head[i] = -1;
    for (int i = 0; i < GBUF_NBIN; i++) g_gbin[i] = -1;
}
/* size bin of an extent of u 64-byte units: exact below 256, then 8 per power of two */
static int gbin_of(uint32_t u){
    if (u < 256u) return (int)u;
    int m = 31 - __builtin_clz(u);
    return 256 + (m - 8) * 8 + (int)((u >> (m - 3)) & 7u);
}
static void gbin_ins(int k){
    int b = gbin_of(g_gfree[k].size >> 6);
    g_gfree[k].prev = -1; g_gfree[k].next = g_gbin[b];
    if (g_gbin[b] >= 0) g_gfree[g_gbin[b]].prev = k;
    g_gbin[b] = k; g_gbinmap[b >> 6] |= 1ull << (b & 63);
}
static void gbin_del(int k){
    int b = gbin_of(g_gfree[k].size >> 6);
    if (g_gfree[k].prev >= 0) g_gfree[g_gfree[k].prev].next = g_gfree[k].next; else g_gbin[b] = g_gfree[k].next;
    if (g_gfree[k].next >= 0) g_gfree[g_gfree[k].next].prev = g_gfree[k].prev;
    if (g_gbin[b] < 0) g_gbinmap[b >> 6] &= ~(1ull << (b & 63));
}
static void gfx_set(int k, uint32_t v){
    uint32_t u0 = (g_gfree[k].addr - GBUF_BASE) >> 6, u1 = u0 + (g_gfree[k].size >> 6) - 1u;
    g_gfx[u0] = v; g_gfx[u1] = v;
}
/* the free extent whose first or last unit is u (or -1) */
static int gfx_at(uint32_t u, uint32_t addr, int end){
    if (u >= GBUF_UNITS) return -1;
    int k = (int)g_gfx[u] - 1;
    if (k < 0 || k >= g_gfree_cap || g_gfree[k].state != GN_FREE) return -1;
    return (end ? g_gfree[k].addr + g_gfree[k].size : g_gfree[k].addr) == addr ? k : -1;
}
static void gext_unlink(int k){ gbin_del(k); gfx_set(k, 0); g_gb_next--; }
/* node k (addr/size set) leaves the quarantine: merge with free neighbours */
static void gext_release(int k){
    uint32_t a = g_gfree[k].addr, s = g_gfree[k].size;
    g_gb_free_bytes += s;
    if (a > GBUF_BASE){
        int j = gfx_at(((a - GBUF_BASE) >> 6) - 1u, a, 1);
        if (j >= 0){ gext_unlink(j); a = g_gfree[j].addr; s += g_gfree[j].size; gnode_put(j); }
    }
    { int j = gfx_at((a + s - GBUF_BASE) >> 6, a + s, 0);
      if (j >= 0){ gext_unlink(j); s += g_gfree[j].size; gnode_put(j); } }
    if (a + s == gbuf_ptr){ gbuf_ptr = a; g_gb_free_bytes -= s; gnode_put(k); return; }
    g_gfree[k].addr = a; g_gfree[k].size = s; g_gfree[k].state = GN_FREE;
    gfx_set(k, (uint32_t)k + 1u); gbin_ins(k); g_gb_next++;
}
/* a free extent of >= n bytes: first fit in n's bin, else the head of the next non-empty bin */
static uint32_t gext_take(uint32_t n){
    int b = gbin_of(n >> 6), k = -1;
    for (int j = g_gbin[b], guard = 0; j >= 0 && guard < 64; j = g_gfree[j].next, guard++)
        if (g_gfree[j].size >= n){ k = j; break; }
    if (k < 0){
        for (int w = (b + 1) >> 6; w < (GBUF_NBIN + 63) / 64 && k < 0; w++){
            uint64_t m = g_gbinmap[w];
            if (w == (b + 1) >> 6) m &= ~0ull << ((b + 1) & 63);
            if (m) k = g_gbin[w * 64 + __builtin_ctzll(m)];
        }
    }
    if (k < 0) return 0;
    uint32_t p = g_gfree[k].addr, rest = g_gfree[k].size - n;
    gext_unlink(k);
    if (rest){ g_gfree[k].addr = p + n; g_gfree[k].size = rest;
               gfx_set(k, (uint32_t)k + 1u); gbin_ins(k); g_gb_next++; g_gb_split++; }
    else gnode_put(k);
    g_gb_free_bytes -= n;
    return p;
}
static void gbuf_age(void){                     /* quarantine -> free extents / size buckets */
    while (g_gq_head >= 0 && (uint32_t)g_present - g_gfree[g_gq_head].stamp >= gbuf_quar()){
        int n = g_gq_head; g_gq_head = g_gfree[n].next; if (g_gq_head < 0) g_gq_tail = -1;
        g_gb_quar_bytes -= g_gfree[n].size;
        if (g_gb_merge){ gext_release(n); continue; }
        uint32_t bk = (g_gfree[n].size >> 6) % GBUF_NB;
        g_gfree[n].state = GN_BUCKET; g_gb_free_bytes += g_gfree[n].size;
        g_gfree[n].next = g_gfree_head[bk]; g_gfree_head[bk] = n;
    }
}
static void gbuf_free(uint32_t p){
    if (!p) return;
    pthread_mutex_lock(&g_gb_lk);
    gbuf_init();
    uint32_t n = (p >= GBUF_BASE && p < gbuf_ptr && !(p & 63u)) ? g_gsz[(p - GBUF_BASE) >> 6] : 0;
    if (!n){ g_gb_bad++; pthread_mutex_unlock(&g_gb_lk); return; }   /* bad pointer / double free */
    g_gsz[(p - GBUF_BASE) >> 6] = 0;
    int k = gnode_get();
    g_gfree[k].addr = p; g_gfree[k].size = n; g_gfree[k].stamp = (uint32_t)g_present;
    g_gfree[k].next = -1; g_gfree[k].state = GN_QUAR;
    if (g_gq_tail >= 0) g_gfree[g_gq_tail].next = k; else g_gq_head = k;
    g_gq_tail = k;
    g_gb_freed++; g_gb_bytes_freed += n; g_gb_live -= n; g_gb_quar_bytes += n;
    pthread_mutex_unlock(&g_gb_lk);
}
static uint32_t gbuf_alloc(uint32_t n){
    n=(n+63)&~63u;
    pthread_mutex_lock(&g_gb_lk);
    gbuf_init();
    gbuf_age();
    uint32_t p = 0;
    if (g_gb_merge) p = gext_take(n);
    else { uint32_t bk = (n >> 6) % GBUF_NB; int *pp = &g_gfree_head[bk];
      for (int guard = 0; *pp >= 0 && guard < 64; guard++, pp = &g_gfree[*pp].next)
          if (g_gfree[*pp].size == n){
              int k = *pp; p = g_gfree[k].addr;
              *pp = g_gfree[k].next; gnode_put(k); g_gb_free_bytes -= n;
              break; } }
    if (p){
        memset(g_mem + p, 0, n);                  /* fresh buffers were zero */
        g_gsz[(p - GBUF_BASE) >> 6] = n;
        g_gb_reused++; g_gb_live += n; if (g_gb_live > g_gb_live_peak) g_gb_live_peak = g_gb_live;
        pthread_mutex_unlock(&g_gb_lk);
        return p; }
    if (gbuf_ptr+n > GBUF_MAX || gbuf_ptr+n < gbuf_ptr){
        static int shown = 0;
        if (shown++ < 5) fprintf(stderr, "  [gbuf] OUT OF MEMORY: %u bytes, buffer heap %u KB (live %u KB, free %u KB, quarantine %u KB)\n",
                                 n, (gbuf_ptr - GBUF_BASE) / 1024u, g_gb_live / 1024u,
                                 g_gb_free_bytes / 1024u, g_gb_quar_bytes / 1024u);
        pthread_mutex_unlock(&g_gb_lk);
        return 0; }
    uint32_t hw = GBUF_BASE + g_gb_top_peak;
    p=gbuf_ptr; gbuf_ptr+=n;
    if (gbuf_ptr - GBUF_BASE > g_gb_top_peak) g_gb_top_peak = gbuf_ptr - GBUF_BASE;
    g_gsz[(p - GBUF_BASE) >> 6] = n;
    g_gb_live += n; if (g_gb_live > g_gb_live_peak) g_gb_live_peak = g_gb_live;
    /* The FULL size + a safety margin is committed per buffer.  Some of the game's
     * paths (high texture/LOD settings) write a little more than we computed;
     * since what follows wasn't committed, it crashed (measured: 0x38098000).
     * The margin removes this class of crash; since mmap is lazy no physical
     * memory is spent.  Below the old top peak the pages were written before (the
     * top went down): zero them like a reused piece. */
    if (p < hw) memset(g_mem + p, 0, hw - p < n ? hw - p : n);
    if (p + n + (1u<<20) > g_gb_committed){ mem_commit(p, n + (1u<<20)); g_gb_committed = p + n + (1u<<20); }
    pthread_mutex_unlock(&g_gb_lk);
    return p;
}

typedef struct { const char *name; const Method *m; unsigned n; } Iface;
static const Iface g_if[] = {
    {"?",              0,         0},
    {"IDirect3D9",       M_D3D9,    NEL(M_D3D9)},
    {"IDirect3DDevice9", M_DEVICE,  NEL(M_DEVICE)},
    {"IDirect3DTexture9",M_TEXTURE, NEL(M_TEXTURE)},
    {"IDirect3DVertexBuffer9", M_VBUF, NEL(M_VBUF)},
    {"IDirect3DIndexBuffer9",  M_IBUF, NEL(M_IBUF)},
    {"IDirect3DSurface9",      M_SURFACE, NEL(M_SURFACE)},
    {"IDirect3DSimple",        M_SIMPLE,  NEL(M_SIMPLE)},
    {"IDirect3DVertexShader9",   M_VSHADER,    NEL(M_VSHADER)},
    {"IDirect3DVertexDeclaration9", M_VDECL,   NEL(M_VDECL)},
    {"IDirect3DStateBlock9",     M_STATEBLOCK, NEL(M_STATEBLOCK)},
    {"IDirect3DSwapChain9",      M_SWAPCHAIN,  NEL(M_SWAPCHAIN)},
    {"IDirect3DQuery9",          M_QUERY,      NEL(M_QUERY)},
    {"IDirectInput8",            M_DI8,        NEL(M_DI8)},
    {"IDirectInputDevice8",      M_DIDEV,      NEL(M_DIDEV)},
    {"IDirectSound",             M_DS,         NEL(M_DS)},
    {"IDirectSoundBuffer",       M_DSBUF,      NEL(M_DSBUF)},
};
enum { IF_D3D9=1, IF_DEV, IF_TEX, IF_VB, IF_IB, IF_SURF, IF_SIMPLE,
       IF_VSHADER, IF_VDECL, IF_STATEBLOCK, IF_SWAPCHAIN, IF_QUERY,
       IF_DI8, IF_DIDEV, IF_DS, IF_DSBUF };

/* --- reference counts of textures, vertex and index buffers -----------------
 * Kept on the host, keyed by the guest object address (objects are never reused:
 * com_alloc only grows).  Created = 1, AddRef +1, Release -1; the buffer heap
 * data is handed back when the count reaches 0.  All COM calls run under the
 * guest lock.  NFSU2_GBUF_FREE0=1: never free (the old behaviour). */
#define RC_CAP (1u << 20)                       /* live objects; linear probing */
static uint32_t *g_rck; static int32_t *g_rcv; static uint32_t g_rc_n;
static uint64_t g_rc_kept, g_rc_kept_data, g_rc_last, g_rc_unknown;
static int gbuf_free_off(void){ return ENVC("NFSU2_GBUF_FREE0") != 0; }
static uint32_t rc_hash(uint32_t o){ return (o * 2654435761u) & (RC_CAP - 1u); }
static int32_t *rc_find(uint32_t o){
    if (!g_rck || !o) return 0;
    for (uint32_t i = rc_hash(o); g_rck[i]; i = (i + 1u) & (RC_CAP - 1u))
        if (g_rck[i] == o) return &g_rcv[i];
    return 0;
}
static void rc_set(uint32_t o, int32_t v){
    if (!g_rck){ g_rck = calloc(RC_CAP, 4); g_rcv = calloc(RC_CAP, 4); }
    if (!g_rck || !g_rcv || g_rc_n >= RC_CAP - RC_CAP / 8u) return;   /* full: never freed */
    uint32_t i = rc_hash(o);
    while (g_rck[i] && g_rck[i] != o) i = (i + 1u) & (RC_CAP - 1u);
    if (!g_rck[i]) g_rc_n++;
    g_rck[i] = o; g_rcv[i] = v;
}
static void rc_del(uint32_t o){                 /* backward-shift deletion */
    uint32_t i = rc_hash(o);
    while (g_rck[i] != o){ if (!g_rck[i]) return; i = (i + 1u) & (RC_CAP - 1u); }
    for (uint32_t j = i;;){
        g_rck[i] = 0; g_rcv[i] = 0;
        for (;;){
            j = (j + 1u) & (RC_CAP - 1u);
            if (!g_rck[j]){ g_rc_n--; return; }
            uint32_t h = rc_hash(g_rck[j]);
            /* entry j may move to i if its home slot is not in (i, j] */
            if ((i <= j) ? (h <= i || h > j) : (h <= i && h > j)) break;
        }
        g_rck[i] = g_rck[j]; g_rcv[i] = g_rcv[j]; i = j;
    }
}
/* 1 if this Release dropped the last reference */
static int rc_release(uint32_t o, uint32_t iface, uint32_t from){
    int32_t *rc = rc_find(o);
    if (!rc){ g_rc_unknown++;                   /* not counted (table full) or released again */
        if (ENVC("RCLOG") && g_rc_unknown <= 40)
            fprintf(stderr, "  [rc] Release of an uncounted object %08x (iface %u) from %08x\n", o, iface, from);
        return 0; }
    if (--*rc > 0){
        g_rc_kept++;
        /* would the old free-on-every-Release have taken live data away? */
        if (mem_is_committed(o + 67u)){
            uint32_t q0 = (iface == IF_TEX) ? 32u : 8u, q1 = (iface == IF_TEX) ? 64u : 12u;
            for (uint32_t q = q0; q < q1; q += 4u) if (MR32(o + q)){ g_rc_kept_data++; break; }
        }
        return 0;
    }
    rc_del(o); g_rc_last++;
    if (ENVC("RCLOG") && g_rc_last <= 4000)
        fprintf(stderr, "  [rc] last release %08x (iface %u) from %08x\n", o, iface, from);
    return 1;
}
static void rc_addref(uint32_t o){
    if (gbuf_free_off()) return;
    int32_t *rc = rc_find(o);
    if (rc) ++*rc;
}
/* Release of a texture / vertex buffer / index buffer: hand the buffer heap data
 * back when the last reference goes.  from = caller (RCLOG). */
static void obj_release(uint32_t o, uint32_t iface, uint32_t from){
    if (gbuf_free_off() || !o || !rc_release(o, iface, from)) return;
    if (iface == IF_VB || iface == IF_IB){
        if (mem_is_committed(o+11)){ gbuf_free(MR32(o+8)); MR32(o+8)=0; }
    } else if (iface == IF_TEX && mem_is_committed(o+67)){
        for (uint32_t q=32; q<64; q+=4){ gbuf_free(MR32(o+q)); MR32(o+q)=0; }
        if (mem_is_committed(o+143) && MR32(o+140)==0xC0BEC0BEu)
            for (uint32_t q=104; q<136; q+=4){ gbuf_free(MR32(o+q)); MR32(o+q)=0; }
    }
}
/* d3dx_hle.c skips the guest's texture AddRef/Release calls on its native
 * SetTexture path; it reports them here so the counts stay right. */
void com_tex_ref_skipped(uint32_t o, int addref){
    if (addref) rc_addref(o); else obj_release(o, IF_TEX, 0);
}

static uint32_t com_new(uint32_t iface, uint32_t extra){
    unsigned n = g_if[iface].n;
    uint32_t vt = com_alloc(n*4u);
    for (unsigned i=0;i<n;i++) MR32(vt+4u*i) = COM_MAGIC | (iface<<16) | i;
    uint32_t obj = com_alloc(4u+extra);
    /* ZERO the extra area.  Object fields (size, format, texture id, cube face
     * index...) are always read assuming "0 = not set up"; if com_alloc doesn't
     * give fresh memory, garbage values are read. */
    for (uint32_t q = 0; q < 4u + extra; q += 4u) MR32(obj + q) = 0u;
    MR32(obj) = vt;
    if ((iface == IF_TEX || iface == IF_VB || iface == IF_IB) && obj) rc_set(obj, 1);
    return obj;
}

#define PSCUBE 256   /* must be the SAME as in metal.m: cube ps variant */
uint64_t g_com_calls=0, g_com_unimpl=0;
/* per-method call counter: a work list for the render layer */
#define MSTAT_IF 20
#define MSTAT_M  128
static uint64_t g_mstat[MSTAT_IF][MSTAT_M];
static uint64_t g_present=0, g_draw=0;
static uint32_t g_d3d9=0, g_dev=0, g_cur_decl=0, g_cur_fvf=0, g_cur_tex=0;

/* ---------------- vertex shader (interpreted on the CPU) ----------------
 * NFSU2 does all its draws with DrawPrimitiveUP and the positions are RAW
 * (POSITION, not POSITIONT): to place them correctly on screen the vertex
 * shader must be run. See runtime/vsemu.c                              */
#define MAXVS 1024
static VSProg  *g_vsprog[MAXVS];
static unsigned g_vsprogn = 1;          /* 0 = "no shader" */
void mprot_arm(uint32_t);
/* CoreAudio output (runtime/audio.m) */
int      audio_register(uint32_t data, uint32_t bytes, uint32_t rate,
                        uint32_t chans, uint32_t bits);
void     audio_play(int id, int loop);
void     audio_stop(int id);
uint32_t audio_cursor(int id);
void     audio_set_volume(int id, long millibel);
void     audio_init(void);
void     audio_report(void);
void cov_reset(void);
static VSConst  g_vsk;                  /* c#/i#/b# constant file */
static unsigned g_cur_vs = 0;
static uint64_t g_vs_draws = 0, g_ff_draws = 0;
static uint64_t g_v_in = 0, g_v_out = 0, g_v_badw = 0;
static uint64_t g_tex_fail = 0;
static uint64_t g_dikey_calls=0, g_dikey_hits=0;
static uint64_t g_mat_ok=0, g_mat_bad=0;
static uint64_t g_di_enum=0, g_di_objs=0;
/* --- DirectInput action map -------------------------------------------
 * The game drives the menu with an "action map": more than one key is bound to
 * each action (example: arrow-up and W -> the same action).  Semantic format:
 *   dwSemantic = 0x81000400 | DIK_scancode
 * The game reads actions BUFFERED with GetDeviceData; if we don't produce
 * events nothing happens in the menu.                                  */
#define DI_KB_SEM_BASE 0x81000400u
/* --- vertex/index buffer draw path (3D models) ------------------
 * The game draws 3D geometry with DrawIndexedPrimitive; the UI with
 * DrawPrimitiveUP.  Since this path wasn't implemented, ALL 3D model geometry
 * (including the car in the menu) was silently dropped.            */
static uint32_t g_ss_vb = 0, g_ss_off = 0, g_ss_stride = 0;
static uint32_t g_ib_obj = 0;
static uint32_t g_idx_scratch = 0;
static uint64_t g_dip_draws = 0, g_dip_skip = 0, g_dip_tris = 0;
static uint32_t g_act_of_key[256];      /* DIK -> uAppData (0 = not bound) */
static uint32_t g_act_ofs[256];         /* DIK -> offset in the action array */
static int      g_act_count = 0;
static unsigned char g_kb_prev[256];
static uint32_t g_di_seq = 1;
static uint64_t g_di_events = 0;
static uint32_t host_us_ms(void){ uint64_t host_us(void); return (uint32_t)(host_us()/1000ull); }
static uint32_t *g_lk_tid=0; static unsigned *g_lk_cnt=0; static uint64_t *g_lk_byt=0;
static uint32_t *g_lock_obj=0; static unsigned *g_lock_cnt=0; static int g_lock_n=0;
static uint64_t g_snd_bytes=0, g_snd_nonzero=0, g_snd_zero=0, g_snd_play=0;
static uint32_t g_cur_rt = 0;
static uint32_t g_cur_rt_w = 0, g_cur_rt_h = 0;   /* its size as the game sees it (HD may enlarge it in Metal) */
/* D3DRENDERSTATETYPE values; the defaults are D3D9's initial state */
/* Resolutions to offer in the game's settings menu (desktop last). */
/* The game's OWN internal resolution table.  Measured experimentally:
 * g_RacingResolution in the registry is an index into this table; 6 and above
 * OVERFLOW the game's own array (garbage like 8126268x268571776 reached
 * CreateDevice).  Previously 12 modes were advertised here, so the settings menu
 * showed more than the 6 rows the game supports, and selecting an extra row led
 * to an array overflow -> crash.  Vanilla NFSU2 only supports 4:3/5:4; there is
 * no widescreen. */
/* Why we don't extend the table: ADVERTISING a 7th mode overflows the game's own
 * array and produces a garbage size (8126268x268571776 every time); the game
 * stores it and creates its render targets at that size too.  Patching the table
 * in guest memory was also tried; it wasn't found as a contiguous {w,h} array.
 * So the row count stays at 6, and only the LABELS and the real device size are
 * ours. */
/* --- Resolution rows in the settings menu ----------------------------
 * The game's internal table (g_RacingResolution 0..5, selected row [0x870d1c])
 * is fixed and all 4:3.  The text in the menu comes from READY-MADE strings in
 * .rdata (sub_0049b690: row -> "640x480".."1600x1200"; display only, the size
 * isn't read from this text).  The EnumAdapterModes list only decides which rows
 * CAN BE SELECTED: the game looks up the table value in the list.  Measured
 * (2026-09-27, the user's screen): when the list was replaced with the labels,
 * the arrows couldn't go to any row; previously, too, since the desktop mode was
 * reported as 1920x1080, the 1600x1200 row (the so-called "1080p") could never
 * be selected.
 *
 * Arrangement: list = the game's table (all 6 rows selectable), desktop mode
 * >= 1600x1200, the menu strings are rewritten in memory and the device is set up
 * at the row's REAL resolution (the aspect difference via Hor+, res_pick).
 * The string slots are 8/8/12/12/12/12 bytes: short labels for the first two rows.
 * NOWIDE=1: the game's original table and strings. */
static const uint32_t g_tbl[6][2] = {
    {640,480},{800,600},{1024,768},{1280,960},{1280,1024},{1600,1200},
};
static uint32_t g_modes[6][2] = {
    {1280,720},{1920,1080},{2560,1440},{0,0},{3840,2160},{1600,1200},
};
#define ROW_NATIVE 3
static const uint32_t g_lbl_addr[6] = {GA(0x0078fa1cu),GA(0x0078fa14u),GA(0x0078fa08u),GA(0x0078f9fcu),GA(0x0078f9f0u),GA(0x0078f9e4u)};
static const uint32_t g_lbl_cap[6]  = {8,8,12,12,12,12};
static void label_put(int row, const char *txt){
    uint32_t a = g_lbl_addr[row], n = (uint32_t)strlen(txt);
    if (n + 1 > g_lbl_cap[row] || !mem_is_committed(a) || !mem_is_committed(a + g_lbl_cap[row] - 1)) return;
    /* is the expected original string in place (let's not write into another EXE version) */
    uint8_t c0 = MR8(a);
    if (!(c0 >= '0' && c0 <= '9')) return;
    memset(g_mem + a, 0, g_lbl_cap[row]); memcpy(g_mem + a, txt, n);
}
static int g_cur_row = -1;             /* the game's active row (-1 = unknown) */
static int g_row4_on = 1;              /* is the 4K row selectable (see modes_refresh) */
static int modemap_on(void){ return !ENVC("NOWIDE"); }
/* The rows are generated from the VIEW's (window / fullscreen) aspect ratio:
 * "1080p" = 1080 lines at the view's aspect ratio.  This way the drawing surface
 * fills the window without bars, and the resolution only changes quality.  The
 * "Screen" row is the view's own pixel count (1:1, upper limit 9 MP); 1600x1200
 * deliberately stays 4:3 (the original look, bars at the sides). */
static void modes_refresh(void){
    unsigned vw = 0, vh = 0;
    void metal_view_size(unsigned*, unsigned*); metal_view_size(&vw, &vh);
    if (!vw || !vh){ vw = 2560; vh = 1600; }
    static const unsigned hts[6] = {720, 1080, 1440, 0, 2160, 0};
    for (int i = 0; i < 6; i++){
        if (!hts[i]) continue;
        unsigned hh = hts[i], ww = (unsigned)((double)hh * vw / vh + 0.5) & ~1u;
        g_modes[i][0] = ww; g_modes[i][1] = hh;
    }
    double dw = vw, dh = vh;
    const char *mp = ENVC("NFSMAXPIX"); double maxpix = (mp && *mp) ? atof(mp) : 9.0e6;
    if (maxpix > 1.0e5 && dw * dh > maxpix){ double k = sqrt(maxpix / (dw * dh)); dw *= k; dh *= k; }
    unsigned w = (unsigned)dw & ~1u, h = (unsigned)dh & ~1u;
    g_modes[ROW_NATIVE][0] = w; g_modes[ROW_NATIVE][1] = h;
    /* The 4K row only if the screen can show it (screen row >= 2160).
     * On a smaller screen (MacBook ~3K) 4K was being shown downscaled: ~1.6x the
     * pixels, no gain on screen, the laptop heats up (user request 2026-10-14).
     * The row can't be deleted (the table has 6 rows); it is reported to the game
     * as UNSELECTABLE: EnumAdapterModes doesn't give 1280x1024 in its 5th entry.
     * Read from the game code: sub_005be5b0 marks availability ([0x86e880+4*row])
     * by looking up the table value in the list, sub_0049b600 skips unavailable
     * rows with the arrows, sub_005be6a0 drops to the next available row below if
     * the saved row is unavailable (4K -> screen).  If this row is reached anyway,
     * the screen resolution and its label are used. */
    g_row4_on = 2160u <= h;
    if (!g_row4_on){ g_modes[4][0] = w; g_modes[4][1] = h; }
    if (!modemap_on()) return;
    static const char *fixed[6] = {"720p","1080p","1440p",0,"4K","1600x1200"};
    char nb[16];
    for (int i = 0; i < 6; i++){
        if (fixed[i] && (i != 4 || g_row4_on)) label_put(i, fixed[i]);
        else { snprintf(nb, sizeof nb, "%ux%u", w, h); label_put(i, nb); }
    }
}
static const uint32_t *mode_label(unsigned i){
    if (!modemap_on()) return g_tbl[i];
    if (i == ROW_NATIVE && !g_modes[i][0]) modes_refresh();
    return g_modes[i];
}
static int tbl_row(uint32_t w, uint32_t h){
    for (int i = 0; i < 6; i++) if (g_tbl[i][0] == w && g_tbl[i][1] == h) return i;
    return -1;
}

uint32_t g_rs[256] = {0};        /* also read by the inline no-effect check in d3dx_hle.c */
/* RSLOG=1: measure WHICH render states the game sets.  Instead of applying a
 * state we don't handle by blind guessing, first verify that it is used. */
static uint32_t g_rs_hits[256] = {0};
static uint32_t g_rs_vals[256][4];        /* the first 4 distinct values */
static uint8_t  g_rs_nval[256] = {0};

/* ---- D3D9 state blocks ----------------------------------------------
 * The game records state with BeginStateBlock/EndStateBlock, then captures and
 * restores it with Capture/Apply around effect passes (in the trace 367 records,
 * 418 Captures, 326 Applys).  These were returned as EMPTY objects, so Apply
 * restored nothing: blend mode, alpha test and texture stage operations leaked
 * over from the previous pass -> overly bright light streaks and wrongly dark
 * surfaces. */
#define MAXSB     512
#define MAXSBREC  128
typedef struct { uint32_t magic; uint32_t a[8]; int nargs; } SBRec;
typedef struct { SBRec r[MAXSBREC]; int n; int used; } SBlock;
static SBlock  g_sb[MAXSB];
static int     g_sbn = 1;          /* 0 = invalid */
int     g_sb_rec = 0;       /* the block being recorded, 0 = no recording (read by d3dx_hle.c) */
int     g_sb_replay = 0;
static uint64_t g_sb_applied = 0, g_sb_captured = 0, g_sb_dropped = 0, g_sb_full = 0;

/* Raw state shadows: Capture reads from these (the values the game GAVE). */
static uint32_t g_raw_vs = 0, g_raw_ps = 0, g_raw_decl = 0, g_raw_fvf = 0;
static uint32_t g_raw_tex[8] = {0};
/* The D3D DEFAULT of D3DTSS_TEXCOORDINDEX is the stage number, NOT 0.
 * When the table started at zero, stages 1 and 2 -- the headlight texture and
 * the falloff ramp -- were sampled with the road's own (repeating) diffuse UV;
 * the headlight cone was copied along the whole road and the ground blew out
 * bright from end to end.  The game never writes this state because the default
 * should already be right. */
uint32_t g_tss_raw[8][40] = {
    [0] = { [11] = 0u }, [1] = { [11] = 1u }, [2] = { [11] = 2u },
    [3] = { [11] = 3u }, [4] = { [11] = 4u }, [5] = { [11] = 5u },
    [6] = { [11] = 6u }, [7] = { [11] = 7u },
};
uint32_t g_smp_raw[16][20];

/* Is this method recorded into a state block? */
/* --- method ID ------------------------------------------------------------
 * The dispatch used to compare the method NAME.  NMEQ's first-letter filter
 * didn't help: almost all D3D9 device methods start with "Set"/"Get"/"Draw", so a
 * FULL strcmp still ran at every step of the chain (measured: 11% of the guest
 * thread was strcmp, 2335 samples of it from com_call).  The id is now resolved
 * ONCE per method (an (iface,slot) -> id cache), and the chain is pure integer
 * comparisons.  A run has 377 million com_calls. */
enum {
    MID__none = 0,
    MID_SetRenderState,
    MID_SetTextureStageState,
    MID_SetSamplerState,
    MID_SetTexture,
    MID_SetVertexShader,
    MID_SetPixelShader,
    MID_SetVertexDeclaration,
    MID_SetFVF,
    MID_SetVertexShaderConstantF,
    MID_SetPixelShaderConstantF,
    MID_SetVertexShaderConstantI,
    MID_SetVertexShaderConstantB,
    MID_SetTransform,
    MID_SetMaterial,
    MID_SetLight,
    MID_LightEnable,
    MID_SetStreamSource,
    MID_SetIndices,
    MID_SetViewport,
    MID_SetScissorRect,
    MID_SetNPatchMode,
    MID_SetClipPlane,
    MID_GetSurfaceLevel,
    MID_GetCubeMapSurface,
    MID_LockRect,
    MID_UnlockRect,
    MID_AddDirtyRect,
    MID_QueryInterface,
    MID_AddRef,
    MID_Release,
    MID_GetAdapterCount,
    MID_GetAdapterModeCount,
    MID_EnumAdapterModes,
    MID_GetAdapterDisplayMode,
    MID_GetAdapterIdentifier,
    MID_GetDeviceCaps,
    MID_CreateDevice,
    MID_TestCooperativeLevel,
    MID_GetAvailableTextureMem,
    MID_GetDirect3D,
    MID_GetDisplayMode,
    MID_Clear,
    MID_BeginScene,
    MID_EndScene,
    MID_Present,
    MID_StretchRect,
    MID_SetRenderTarget,
    MID_Reset,
    MID_DrawIndexedPrimitive,
    MID_DrawPrimitiveUP,
    MID_CreateTexture,
    MID_CreateCubeTexture,
    MID_CreateVolumeTexture,
    MID_CreateVertexBuffer,
    MID_CreateIndexBuffer,
    MID_CreateRenderTarget,
    MID_CreateDepthStencilSurface,
    MID_CreateOffscreenPlainSurface,
    MID_GetBackBuffer,
    MID_GetRenderTarget,
    MID_GetDepthStencilSurface,
    MID_SetDepthStencilSurface,
    MID_CreateVertexDeclaration,
    MID_CreateVertexShader,
    MID_CreatePixelShader,
    MID_BeginStateBlock,
    MID_CreateStateBlock,
    MID_CreateQuery,
    MID_EndStateBlock,
    MID_CreateAdditionalSwapChain,
    MID_GetSwapChain,
    MID_GetNumberOfSwapChains,
    MID_GetTransform,
    MID_GetMaterial,
    MID_GetLight,
    MID_GetViewport,
    MID_GetScissorRect,
    MID_GetClipStatus,
    MID_GetCreationParameters,
    MID_GetRasterStatus,
    MID_GetGammaRamp,
    MID_GetClipPlane,
    MID_GetDevice,
    MID_Lock,
    MID_GetLevelCount,
    MID_GetType,
    MID_GetDesc,
    MID_GetLevelDesc,
    MID_CreateSoundBuffer,
    MID_GetCaps,
    MID_SetCooperativeLevel,
    MID_GetSpeakerConfig,
    MID_DuplicateSoundBuffer,
    MID_Unlock,
    MID_Play,
    MID_Stop,
    MID_SetVolume,
    MID_SetFrequency,
    MID_GetStatus,
    MID_GetCurrentPosition,
    MID_GetFormat,
    MID_EnumDevices,
    MID_EnumDevicesBySemantics,
    MID_GetDeviceStatus,
    MID_Initialize,
    MID_FindDevice,
    MID_EnumObjects,
    MID_GetObjectInfo,
    MID_GetDeviceInfo,
    MID_SetDataFormat,
    MID_Acquire,
    MID_Unacquire,
    MID_Poll,
    MID_GetDeviceState,
    MID_BuildActionMap,
    MID_SetActionMap,
    MID_GetDeviceData,
    MID_GetCapabilities,
    MID_GetProperty,
    MID_SetProperty,
    MID_SetEventNotification,
    MID_GetDataSize,
    MID_GetData,
    MID_Apply,
    MID_Capture,
    MID_GetFunction,
    MID_GetDeclaration,
    MID__count
};
static const char *const g_midname[MID__count] = {
    "",
    "SetRenderState",
    "SetTextureStageState",
    "SetSamplerState",
    "SetTexture",
    "SetVertexShader",
    "SetPixelShader",
    "SetVertexDeclaration",
    "SetFVF",
    "SetVertexShaderConstantF",
    "SetPixelShaderConstantF",
    "SetVertexShaderConstantI",
    "SetVertexShaderConstantB",
    "SetTransform",
    "SetMaterial",
    "SetLight",
    "LightEnable",
    "SetStreamSource",
    "SetIndices",
    "SetViewport",
    "SetScissorRect",
    "SetNPatchMode",
    "SetClipPlane",
    "GetSurfaceLevel",
    "GetCubeMapSurface",
    "LockRect",
    "UnlockRect",
    "AddDirtyRect",
    "QueryInterface",
    "AddRef",
    "Release",
    "GetAdapterCount",
    "GetAdapterModeCount",
    "EnumAdapterModes",
    "GetAdapterDisplayMode",
    "GetAdapterIdentifier",
    "GetDeviceCaps",
    "CreateDevice",
    "TestCooperativeLevel",
    "GetAvailableTextureMem",
    "GetDirect3D",
    "GetDisplayMode",
    "Clear",
    "BeginScene",
    "EndScene",
    "Present",
    "StretchRect",
    "SetRenderTarget",
    "Reset",
    "DrawIndexedPrimitive",
    "DrawPrimitiveUP",
    "CreateTexture",
    "CreateCubeTexture",
    "CreateVolumeTexture",
    "CreateVertexBuffer",
    "CreateIndexBuffer",
    "CreateRenderTarget",
    "CreateDepthStencilSurface",
    "CreateOffscreenPlainSurface",
    "GetBackBuffer",
    "GetRenderTarget",
    "GetDepthStencilSurface",
    "SetDepthStencilSurface",
    "CreateVertexDeclaration",
    "CreateVertexShader",
    "CreatePixelShader",
    "BeginStateBlock",
    "CreateStateBlock",
    "CreateQuery",
    "EndStateBlock",
    "CreateAdditionalSwapChain",
    "GetSwapChain",
    "GetNumberOfSwapChains",
    "GetTransform",
    "GetMaterial",
    "GetLight",
    "GetViewport",
    "GetScissorRect",
    "GetClipStatus",
    "GetCreationParameters",
    "GetRasterStatus",
    "GetGammaRamp",
    "GetClipPlane",
    "GetDevice",
    "Lock",
    "GetLevelCount",
    "GetType",
    "GetDesc",
    "GetLevelDesc",
    "CreateSoundBuffer",
    "GetCaps",
    "SetCooperativeLevel",
    "GetSpeakerConfig",
    "DuplicateSoundBuffer",
    "Unlock",
    "Play",
    "Stop",
    "SetVolume",
    "SetFrequency",
    "GetStatus",
    "GetCurrentPosition",
    "GetFormat",
    "EnumDevices",
    "EnumDevicesBySemantics",
    "GetDeviceStatus",
    "Initialize",
    "FindDevice",
    "EnumObjects",
    "GetObjectInfo",
    "GetDeviceInfo",
    "SetDataFormat",
    "Acquire",
    "Unacquire",
    "Poll",
    "GetDeviceState",
    "BuildActionMap",
    "SetActionMap",
    "GetDeviceData",
    "GetCapabilities",
    "GetProperty",
    "SetProperty",
    "SetEventNotification",
    "GetDataSize",
    "GetData",
    "Apply",
    "Capture",
    "GetFunction",
    "GetDeclaration",
};
static int mid_resolve(const char *nm){
    for (int i = 1; i < MID__count; i++)
        if (nm[0] == g_midname[i][0] && !strcmp(nm, g_midname[i])) return i;
    return -1;                       /* unknown: don't search again */
}
#define MIDIF 32
#define MIDM 160
static int16_t g_midc[MIDIF][MIDM];  /* 0 = not resolved yet */
#define MID(x_) (mid == MID_##x_)

static int sb_recordable(int mid){
    return MID(SetRenderState)       || MID(SetTextureStageState)
        || MID(SetSamplerState)      || MID(SetTexture)
        || MID(SetVertexShader)      || MID(SetPixelShader)
        || MID(SetVertexDeclaration) || MID(SetFVF)
        || MID(SetVertexShaderConstantF) || MID(SetPixelShaderConstantF)
        || MID(SetVertexShaderConstantI) || MID(SetVertexShaderConstantB)
        || MID(SetTransform)         || MID(SetMaterial)
        || MID(SetLight)             || MID(LightEnable)
        || MID(SetStreamSource)      || MID(SetIndices)
        || MID(SetViewport)          || MID(SetScissorRect)
        || MID(SetNPatchMode)        || MID(SetClipPlane);
}
static int g_rs_init = 0;
/* 19=SRCBLEND 20=DESTBLEND 22=CULLMODE 27=ALPHABLENDENABLE
 * 15=ALPHATESTENABLE 24=ALPHAREF 25=ALPHAFUNC 168=COLORWRITEENABLE 171=BLENDOP */
/* If g_rs[] didn't change, rs_apply() is pure waste: the game calls
 * SetRenderState 91.5 MILLION times in a run and the vast majority rewrite the
 * same value.  Since rs_apply derives entirely from g_rs, skipping it gives
 * exactly the same result.  The flag is STICKY: changes made while Metal isn't
 * ready yet are still applied when the device arrives. */
int g_rs_dirty = 1;
static uint64_t g_rs_skip = 0, g_rs_run = 0;
static void rs_apply(void){
    g_rs_run++;
    if (!g_rs_init){ g_rs_init=1;
        g_rs[19]=5; g_rs[20]=6; g_rs[27]=0; g_rs[168]=0xF; g_rs[171]=1;
        g_rs[22]=3;                            /* CULLMODE default: CCW */
        g_rs[15]=0; g_rs[24]=0; g_rs[25]=8;
        g_rs[7]=1; g_rs[14]=1; g_rs[23]=4;     /* Z defaults: on */
        g_rs[193]=0xFFFFFFFFu; }               /* BLENDFACTOR default: white */
    unsigned key = (g_rs[27]?1u:0u)
                 | ((g_rs[19] & 31u) << 1)
                 | ((g_rs[20] & 31u) << 6)
                 | ((g_rs[171] & 7u) << 11)
                 | ((g_rs[168] & 15u) << 14);
    /* alpha test: we only support the "greater/greater-equal" form */
    float aref = 0.f;
    if (g_rs[15] && (g_rs[25]==5 || g_rs[25]==6 || g_rs[25]==7))
        aref = (float)(g_rs[24] & 0xFFu) / 255.0f;
    metal_set_blend(key, aref);
    /* D3DRS_BLENDFACTOR (193): when SRCBLEND/DESTBLEND = BLENDFACTOR /
     * INVBLENDFACTOR, blending crossfades with this CONSTANT color.
     * In Metal this is encoder state (setBlendColor), not pipeline state. */
    { void metal_set_blendcolor(uint32_t argb);
      metal_set_blendcolor(g_rs[193]); }
    /* D3DRS_CULLMODE (22): 1=NONE 2=CW 3=CCW.  It was never applied; since Metal's
     * default is CullModeNone, the BACK FACES of everything were drawn too.  The
     * far side of a closed body z-fights with the near side -> constantly
     * flickering blotches on the car's doors/bumpers, building edges and the
     * skyline. */
    { void metal_set_cull(unsigned);
      metal_set_cull(g_rs[22] ? g_rs[22] : 3u); }
    /* D3DRS_FOGENABLE (28) + D3DRS_FOGCOLOR (34).  With a vertex shader present the
     * fog factor comes from oFog; the fixed-function stage blends the pixel toward
     * FOGCOLOR.  Measured: FOGENABLE is set 5.3M times, 6 of the 18 vertex shaders
     * write oFog. */
    { void metal_set_fog(unsigned, unsigned);
      metal_set_fog(g_rs[28], g_rs[34]); }
    /* Depth: 7=ZENABLE 14=ZWRITEENABLE 23=ZFUNC (default LESSEQUAL=4).
     * Since the UI draws with Z off, this must be carried per draw. */
    { void metal_set_depth(unsigned);
      unsigned zf = g_rs[23] ? (g_rs[23] & 7u) : 4u;
      unsigned dm = (g_rs[7] ? 1u : 0u) | (g_rs[14] ? 2u : 0u) | (zf << 2);
      metal_set_depth(dm); }
}
static uint64_t g_rt_set=0, g_rt_screen=0;
static uint64_t g_rt_tex=0;
static uint32_t g_bb_w = 1280, g_bb_h = 720;   /* back buffer size */
/* Cube textures share the IDirect3DTexture9 vtable; storing which object is a
 * cube inside the object is UNSAFE (2D objects are allocated smaller, reading
 * past the allocated area reads neighboring heap data).  A separate registry: */
/* The registry must be UNBOUNDED: the game recreates 2 cube textures on every
 * device Reset (resolution / fullscreen change, two Resets each).  The old
 * 64-entry table filled up at the 32nd change; the next cube was taken for "2D",
 * GetCubeMapSurface returned with the wrong argument count, the stack shifted by
 * 4 bytes and the game crashed (user log: sub_005b9920 after exactly 64 Resets,
 * address 0x80).  Since COM objects are never released (com_alloc is a bump
 * pointer), addresses aren't reused; a bitmap is safe. */
static uint8_t g_cube_bits[(COM_MAX - COM_BASE) / 16u / 8u];
static unsigned g_cube_n = 0;
static void cube_mark(uint32_t obj){
    if (obj < COM_BASE || obj >= COM_MAX) return;
    uint32_t k = (obj - COM_BASE) >> 4;
    g_cube_bits[k >> 3] |= (uint8_t)(1u << (k & 7u)); g_cube_n++;
}
static int is_cube_tex(uint32_t obj){
    if (obj < COM_BASE || obj >= COM_MAX || (obj & 15u)) return 0;
    uint32_t k = (obj - COM_BASE) >> 4;
    return (g_cube_bits[k >> 3] >> (k & 7u)) & 1u;
}
static uint64_t g_st_ok=0, g_st_null=0, g_st_zero=0, g_st_other=0;
static uint32_t g_tex_stage[4] = {0,0,0,0};
static float    g_psc[32][4];                    /* ps constants c0..c31 */
static uint32_t g_psc_max = 0;                   /* the highest index seen + 1 */
static uint32_t g_psc_drop = 0;                  /* writes above 32 (dropped) */
static int      g_cur_ps_id = 0;                 /* active ps id */
static int      g_psn = 0;                       /* number of registered ps */
static uint64_t g_stage_hist[4] = {0,0,0,0};
static float g_rmin[3] = { 1e30f, 1e30f, 1e30f };
static float g_rmax[3] = { -1e30f, -1e30f, -1e30f };
/* the last 8 SetVertexShaderConstantF calls (for diagnostics) */
static struct { uint32_t sr, pd, n; } g_sc[8];
static unsigned g_scn = 0;

/* D3DDECLTYPE -> read 4 floats into a v# register */
static void decl_fetch(uint32_t addr, unsigned type, float out[4]){
    out[0]=out[1]=out[2]=0.f; out[3]=1.f;
    switch (type){
    case 0: out[0]=MRF32(addr); break;                                   /* FLOAT1 */
    case 1: out[0]=MRF32(addr); out[1]=MRF32(addr+4); break;             /* FLOAT2 */
    case 2: out[0]=MRF32(addr); out[1]=MRF32(addr+4);
            out[2]=MRF32(addr+8); break;                                 /* FLOAT3 */
    case 3: out[0]=MRF32(addr); out[1]=MRF32(addr+4);
            out[2]=MRF32(addr+8); out[3]=MRF32(addr+12); break;          /* FLOAT4 */
    case 4: /* D3DCOLOR: B,G,R,A in memory -> (R,G,B,A) */
            out[0]=MR8(addr+2)/255.f; out[1]=MR8(addr+1)/255.f;
            out[2]=MR8(addr)/255.f;   out[3]=MR8(addr+3)/255.f; break;
    case 5: out[0]=MR8(addr); out[1]=MR8(addr+1);
            out[2]=MR8(addr+2); out[3]=MR8(addr+3); break;               /* UBYTE4 */
    case 8: out[0]=MR8(addr)/255.f; out[1]=MR8(addr+1)/255.f;
            out[2]=MR8(addr+2)/255.f; out[3]=MR8(addr+3)/255.f; break;   /* UBYTE4N */
    case 6: out[0]=(int16_t)MR16(addr); out[1]=(int16_t)MR16(addr+2); break;
    case 7: out[0]=(int16_t)MR16(addr);   out[1]=(int16_t)MR16(addr+2);
            out[2]=(int16_t)MR16(addr+4); out[3]=(int16_t)MR16(addr+6); break;
    case 9: out[0]=(int16_t)MR16(addr)/32767.f;
            out[1]=(int16_t)MR16(addr+2)/32767.f; break;
    case 10:out[0]=(int16_t)MR16(addr)/32767.f;   out[1]=(int16_t)MR16(addr+2)/32767.f;
            out[2]=(int16_t)MR16(addr+4)/32767.f; out[3]=(int16_t)MR16(addr+6)/32767.f; break;
    case 11:out[0]=MR16(addr)/65535.f; out[1]=MR16(addr+2)/65535.f; break;
    case 12:out[0]=MR16(addr)/65535.f;   out[1]=MR16(addr+2)/65535.f;
            out[2]=MR16(addr+4)/65535.f; out[3]=MR16(addr+6)/65535.f; break;
    default: break;
    }
}

/* --- DRAWDUMP=<frame#>: dump every draw in that frame in detail --- */
static uint32_t g_present_no = 0;
#define ZSURF_MAGIC 0x5A535246u   /* 'ZSRF': Z surface object, +52 = id */
static uint32_t g_cur_zid = 1;           /* the bound Z surface (0 = none) */
/* --- device loss / rebuild -----------------------------------------------
 * When the window mode changes (fullscreen <-> windowed) the drawing resolution
 * must change too; since the game's render targets are tied to the device size,
 * ONLY the game itself can do this with Reset.  As with alt-tab on Windows, we
 * report the device as "lost": Present returns D3DERR_DEVICELOST,
 * TestCooperativeLevel D3DERR_DEVICENOTRESET, the game releases its resources
 * and calls Reset, and res_pick picks the size for the new mode. */
static _Atomic int g_lost = 0;
static unsigned long g_tcl_n = 0, g_tcl_lost_n = 0, g_lost_presents = 0, g_resets = 0;
void com_request_reset(void){
    if (!g_dev) return;          /* no device yet: CreateDevice will see the new mode anyway */
    atomic_store(&g_lost, 1); }
static int g_wc_armed = 0, g_wc_after = 0;
static int      g_dd_want    = -1;
static int dd_on(void){
    static int init=0;
    if (!init){ init=1; const char *e=ENVC("DRAWDUMP"); g_dd_want = e&&*e ? atoi(e) : -1; }
    return g_dd_want >= 0 && (uint32_t)g_dd_want == g_present_no;
}
static void dd_decl(uint32_t d){
    uint32_t el = (d && mem_is_committed(d+4)) ? MR32(d+4) : 0;
    static const char *TY[]={"FLOAT1","FLOAT2","FLOAT3","FLOAT4","D3DCOLOR","UBYTE4",
        "SHORT2","SHORT4","UBYTE4N","SHORT2N","SHORT4N","USHORT2N","USHORT4N",
        "UDEC3","DEC3N","FLOAT16_2","FLOAT16_4","UNUSED"};
    static const char *US[]={"POSITION","BLENDWEIGHT","BLENDINDICES","NORMAL","PSIZE",
        "TEXCOORD","TANGENT","BINORMAL","TESSFACTOR","POSITIONT","COLOR","FOG",
        "DEPTH","SAMPLE"};
    if (!el){ fprintf(stderr,"      decl=0x%08x (no declaration)\n", d); return; }
    fprintf(stderr,"      decl=0x%08x:", d);
    for (uint32_t k=0;k<16;k++){
        uint32_t e=el+8u*k;
        if (!mem_is_committed(e+7)) break;
        uint32_t of=MR16(e+2), ty=MR8(e+4), us=MR8(e+6), ui=MR8(e+7);
        if (MR16(e)==0xFFu && ty==17u) break;
        fprintf(stderr," %u:%s/%s%u", of, ty<18?TY[ty]:"?", us<14?US[us]:"?", ui);
    }
    fprintf(stderr,"\n");
}
int g_com_trace = 0;
void com_set_trace(int v){ g_com_trace=v; }
void draw_stats_save(uint64_t*,uint64_t*);
/* During replay (state block Apply) the arguments come from the record, not the
 * guest stack. */
static const uint32_t *g_arg_ovr = 0;
static uint32_t A(CPU*c,int i){
    if (g_arg_ovr) return g_arg_ovr[i & 7];
    return MR32(c->esp + 4u*(uint32_t)i);
}

/* --- run a guest callback as stdcall --------------------
 * DirectInput's EnumDevices family works with callbacks.  If the callback is never
 * made the game finds NO input devices: the mouse cursor is read but menu actions
 * aren't bound to anything.
 * The CPU state is preserved; since it is stdcall the callee cleans up the arguments. */
uint32_t call_guest(CPU *c, uint32_t fn, int nargs, const uint32_t *args){
    if (!fn) return 0;
    CPU save = *c;
    for (int i = nargs - 1; i >= 0; i--) PUSH32(args[i]);
    PUSH32(0xDEADBEEFu);                    /* fake return address */
    dispatch_indirect(c, fn);
    uint32_t ret = c->eax;
    *c = save;                              /* go back to the state before the call */
    return ret;
}

/* GUID_SysKeyboard / GUID_SysMouse (DirectInput system devices) */
static void put_guid(uint32_t p, uint32_t d1, uint16_t d2, uint16_t d3,
                     const uint8_t d4[8]){
    MR32(p)=d1; MR16(p+4)=d2; MR16(p+6)=d3;
    for (int i=0;i<8;i++) MR8(p+8+i)=d4[i];
}

/* Fill a DIDEVICEINSTANCEA (580 bytes) and return its address.  kind 1 keyboard,
 * 2 mouse, 3 game pad (host controller, runtime/gamepad.m). */
int gamepad_present(void);
void gamepad_fill(unsigned char *dst);
const char *gamepad_name(void);
static uint32_t make_devinst(int kind){
    static uint32_t slots[4] = {0,0,0,0};
    uint32_t *slot = &slots[kind & 3];
    if (!*slot){ *slot = gbuf_alloc(600); g_gb_kept[3] += 640; }
    uint32_t p = *slot;
    memset(g_mem+p, 0, 600);
    MR32(p) = 580u;                                   /* dwSize */
    static const uint8_t tail[8] = {0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00};
    if (kind == 3){
        static const uint8_t ptail[8] = {0,0,'P','I','D','V','I','D'};
        put_guid(p+4,  0x6F1D2B70u, 0xD5A0, 0x11CF, tail);     /* guidInstance */
        put_guid(p+20, 0x028E045Eu, 0x0000, 0x0000, ptail);    /* guidProduct: VID 045E PID 028E */
        MR32(p+36) = 0x0215u;                         /* DI8DEVTYPE_GAMEPAD, STANDARD */
        snprintf((char*)(g_mem+p+40),  260, "%s", gamepad_name());
        snprintf((char*)(g_mem+p+300), 260, "%s", gamepad_name());
        return p;
    }
    int keyboard = (kind == 1);
    put_guid(p+4,  keyboard?0x6F1D2B61u:0x6F1D2B60u, 0xD5A0, 0x11CF, tail);
    put_guid(p+20, keyboard?0x6F1D2B61u:0x6F1D2B60u, 0xD5A0, 0x11CF, tail);
    /* dwDevType: subtype << 8 | type.  0x13=KEYBOARD, 0x12=MOUSE */
    MR32(p+36) = keyboard ? 0x0313u : 0x0212u;
    snprintf((char*)(g_mem+p+40),  260, "%s", keyboard ? "Keyboard" : "Mouse");
    snprintf((char*)(g_mem+p+300), 260, "%s", keyboard ? "Keyboard" : "Mouse");
    return p;
}

static void fill_caps(uint32_t p);


/* --- vertex processing: gather inputs per the declaration, run the shader --- */
typedef struct { uint16_t off; uint8_t type; int8_t reg; } DElem;

static int g_fixmat = -1;
static uint64_t g_t_draw_ns = 0, g_v_xform = 0;
static uint64_t g_t_vs_ns = 0;
static uint64_t g_v_per_vs[64] = {0};
/* Draw path timing statistics (exit report) only with DRAWTIME=1: two clock reads
 * per draw were ~0.8% of the guest thread in the real game. */
static inline uint64_t now_ns(void){
    static int on = -1;
    if (on < 0) on = getenv("DRAWTIME") != NULL;
    if (!on) return 0;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return (uint64_t)ts.tv_sec*1000000000ull + (uint64_t)ts.tv_nsec;
}
/* The GPU shader id corresponding to a (vertex shader, vertex declaration)
 * pair.  Since the same bytecode can be used with different declarations, the
 * key is both together. */
#define MAXGVS 256
static struct { uint32_t vs, decl; int gid, state; } g_gvs[MAXGVS];
static int g_gvsn = 0, g_gvs_next = 1;
static int g_gvs_nk[MAXGVS];
static int g_gpuvs = -1;
static uint64_t g_gpuvs_draws = 0, g_gpuvs_fail = 0, g_gpuvs_drop = 0;

static int gpuvs_id(const VSProg *prog, uint32_t vs, uint32_t decl,
                    const DElem *el, int nel){
    for (int i = 0; i < g_gvsn; i++)
        if (g_gvs[i].vs == vs && g_gvs[i].decl == decl)
            return g_gvs[i].state > 0 ? g_gvs[i].gid : -1;
    if (g_gvsn >= MAXGVS || g_gvs_next >= MAXGVS) return -1;
    int slot = g_gvsn++;
    g_gvs[slot].vs = vs; g_gvs[slot].decl = decl;
    g_gvs[slot].state = -1; g_gvs[slot].gid = 0;

    int dcl[16][2];
    for (int i = 0; i < 16; i++){ dcl[i][0] = -1; dcl[i][1] = 0; }
    for (int k = 0; k < nel; k++){
        int rg = el[k].reg;
        if (rg >= 0 && rg < 16){ dcl[rg][0] = (int)el[k].off; dcl[rg][1] = (int)el[k].type; }
    }
    int gid = g_gvs_next;
    /* GPUVSMASK: which translated shaders are allowed (bit0=vs_1).
     * For isolating an image bug per shader. */
    { static long mask = -1;
      if (mask < 0){ const char *em = ENVC("GPUVSMASK");
                     mask = (em && *em) ? strtol(em, 0, 0) : -1L; }
      if (mask >= 0 && !((mask >> (gid-1)) & 1L)){
          if (ENVC("GPUVSLOG"))
              fprintf(stderr, "  [gpuvs] shader=%u decl=%08x -> outside the mask, on the CPU\n", vs, decl);
          g_gvs_next++;         /* consume the id anyway so the mask bits don't shift */
          return -1;
      } }
    static char msl[160*1024];          /* HD: + capture kernel */
    int len = vs_to_msl(prog, gid, (const int (*)[2])dcl, msl, sizeof msl);
    if (len <= 0){ g_gpuvs_fail++; return -1; }
    void metal_register_vs(int, const char *);
    int  metal_vs_ready(int);
    metal_register_vs(gid, msl);
    if (!metal_vs_ready(gid)){ g_gpuvs_fail++; return -1; }
    g_gvs_next++;
    g_gvs[slot].gid = gid; g_gvs[slot].state = 1;
    g_gvs_nk[gid] = vs_kmax(prog);
#if NFS_ENHANCED
    /* HD ray tracing attempt: the offset/type of the position element, the constant
     * register of the position matrix and whether it is skinned (BLENDINDICES = usage 2). */
    { int pr = vs_input_reg(prog, 0, 0), po = -1, pty = -1, cw = -1;
      for (int k = 0; k < nel; k++) if (el[k].reg == pr){ po = (int)el[k].off; pty = (int)el[k].type; }
      int cx = vs_pos_creg(prog, &cw);
      float ramp[2] = { 0.005f, 0.5f };
      int lr = vs_light_creg(prog, ramp);
      void metal_hd_vs_info(int, int, int, int, int, int, int, float, float);
      metal_hd_vs_info(gid, po, pty, (cw == cx + 3) ? cx : -1,
                       vs_input_reg(prog, 2, 0) >= 0 || vs_input_reg(prog, 1, 0) >= 0,
                       vs_fog_creg(prog), lr, ramp[0], ramp[1]); }
#endif
    if (ENVC("GPUVSLOG"))
        fprintf(stderr, "  [gpuvs] shader=%u decl=%08x -> vs_%d (%u instructions, %d constants)%s\n",
                vs, decl, gid, vs_ninstr(prog), g_gvs_nk[gid],
                vs_writes_fog(prog) ? "  [WRITES oFog]" : "");
    /* GPUVSDUMP=<gid>: write the generated MSL and the source bytecode. */
    { const char *dv = ENVC("GPUVSDUMP");
      if (dv && *dv && atoi(dv) == gid){
          fprintf(stderr, "--- vs_%d source disassembly ---\n", gid);
          vs_disasm(prog, stderr);
          fprintf(stderr, "--- vs_%d generated MSL ---\n%s\n", gid, msl);
      } }
    return gid;
}

/* The game's front-end WorldViewProj matrix arrives broken (only rows 2 and 3
 * written).  FIXMAT: if it is broken, put in a reasonable matrix.  The decision
 * doesn't change during a draw, so it is made once OUTSIDE the vertex loop; both
 * the CPU and GPU paths use the same helper so they don't diverge. */
/* --- Widescreen (Hor+) ---------------------------------------------------
 * NFSU2 was designed for 4:3: ALL 6 rows of the internal resolution table are 4:3
 * and index 6+ overflows the array, so 1080p can't be selected from the menu.
 * When the device is made 1920x1080 the game produces the same 4:3 image and it
 * gets STRETCHED horizontally (measured: the 1080p frame is a pure stretch of the
 * 4:3 frame -- difference 2.03, vertical crop hypothesis 34.2).
 * Fix: in perspective draws scale clip-space x by game_aspect/device_aspect.
 * Proportions are fixed and MORE of the scene is visible horizontally (Hor+).
 * Telling them apart: oPos.w = dot(v, c3); in perspective c3.w=0, in an
 * orthographic (UI) matrix c3.w=1.  The UI isn't touched. */
static float g_aspfix = 1.f;
/* The Hor+ factor (for clip-space x); the HD composite pass uses it to bring the
 * camera into the same space as the geometry drawn on screen. */
float com_aspfix(void){ return g_aspfix; }
/* translation programs: the variants (HD reflection, post-process chain) are generated at draw time */
static VSProg *g_psprog[256];
/* POST-PROCESS CHAIN (metal.m): the MSL of the ps's tile memory read variant.
 * Length on success, otherwise 0. */
int com_ps_fb_msl(int base, int vid, int in_idx, int out_idx, char *msl, size_t cap){
    if (base <= 0 || base >= 256 || !g_psprog[base]) return 0;
    return ps_to_msl_fb(g_psprog[base], vid, msl, cap, in_idx, out_idx);
}
#if NFS_ENHANCED
/* Reflection separation variant: base ps (cube=1 for the cube variant), reflection
 * stage k, Metal id vid.  1 on success. */
int com_ps_refl_variant(int base, int cube, int k, int vid){
    if (base <= 0 || base >= 256 || !g_psprog[base]) return 0;
    static char msl[32768];
    if (ps_to_msl_ex2(g_psprog[base], vid, msl, sizeof msl, cube ? 1u : 0u, k) <= 0) return 0;
    void metal_register_ps(int, const char *);
    metal_register_ps(vid, msl);
    fprintf(stderr, "[HD] reflection variant: ps %d%s stage %d -> %d\n",
            base, cube ? " (cube)" : "", k, vid);
    return 1;
}
#endif
static uint32_t g_req_w = 0, g_req_h = 0;   /* the resolution the game thinks it has */
unsigned long g_asp_persp = 0, g_asp_ortho = 0;
/* The resolution the game asks for -> the resolution the device will REALLY draw at.
 * Three rules are gathered in one place: the NFSRES pin, the widescreen choice
 * from the menu, and Retina pixel density.  CreateDevice and Reset MUST use the
 * same rule: Reset used to pass the raw value through, so when the resolution was
 * changed in-game the correction was silently undone. */
static void res_pick(uint32_t gw, uint32_t gh, uint32_t *dw, uint32_t *dh){
    static int rdone = 0, pinned = 0;
    static uint32_t rw = 0, rh = 0;
    *dw = gw; *dh = gh;
    /* The cursor must be in the space the game THINKS it is in, not the drawing surface. */
    { void metal_set_game_size(unsigned,unsigned); metal_set_game_size(gw, gh); }
    if (!rdone){ rdone = 1; const char *e = ENVC("NFSRES");
        if (e && *e){ unsigned a = 0, b = 0;
            if (sscanf(e, "%ux%u", &a, &b) == 2 && a >= 640 && b >= 480
                && a <= 8192 && b <= 8192){ rw = a; rh = b; pinned = 1; } } }
    uint32_t aw = rw, ah = rh;
    int row = tbl_row(gw, gh);
    g_cur_row = row;
    if (modemap_on()) modes_refresh();                 /* screen row + menu strings */
    if (!pinned && row >= 0 && modemap_on()){
        aw = g_modes[row][0]; ah = g_modes[row][1];
        fprintf(stderr,"com: menu row %d -> %ux%u\n", row + 1, aw, ah);
    }
    else if (!pinned){
        uint32_t bw = aw ? aw : gw, bh = ah ? ah : gh;
        const char *nv = ENVC("NFSNATIVE");
        if (bw && bh && !(nv && nv[0] == '0')){
            void metal_native_size(unsigned,unsigned,unsigned*,unsigned*);
            unsigned nw = bw, nh = bh;
            metal_native_size(bw, bh, &nw, &nh);
            if (nw > bw || nh > bh){ aw = nw; ah = nh; } }
    }
    if (!aw || !ah){
        /* No override: device = what the game asked for.  Values left over from a
         * previous CreateDevice must not stay stale after a Reset (RT resizing and
         * GetDisplayMode look at them). */
        g_req_w = g_req_h = 0; g_aspfix = 1.f;
        { void metal_set_ui_aspect(float); metal_set_ui_aspect(1.f); }
        { void metal_set_uv_scale(unsigned, unsigned, unsigned, unsigned); metal_set_uv_scale(0, 0, 0, 0); }
        return;
    }
    g_req_w = gw; g_req_h = gh;
    { void metal_set_uv_scale(unsigned, unsigned, unsigned, unsigned);   /* see metal.m UV SCALE */
      metal_set_uv_scale(gw, gh, aw, ah); }
    /* The game keeps drawing for its own aspect ratio; we compensate for the
     * difference in clip space.  If the ratio is preserved exactly this factor stays 1.0. */
    if (gw && gh){
        float ga = (float)gw / (float)gh, da = (float)aw / (float)ah;
        g_aspfix = ga / da;
        const char *ov = ENVC("ASPFIX");
        if (ov && *ov) g_aspfix = (float)atof(ov);
    }
    *dw = aw; *dh = ah;
    { static int hf = -1;
      if (hf < 0){ const char *e = ENVC("HUDFIX"); hf = !(e && *e && *e=='0'); }
      void metal_set_ui_aspect(float); metal_set_ui_aspect(hf && g_aspfix < 1.f ? g_aspfix : 1.f); }
    fprintf(stderr,"com: device resolution %ux%u -> %ux%u (aspfix=%.4f)\n",
            gw, gh, aw, ah, (double)g_aspfix);
}
/* Copy ONLY the part of the constant file this shader reads.  The whole file
 * (4416 bytes) used to be copied per draw, just to scale a single row (16 bytes):
 * 1316 copies per frame in a race, 5.7 MB, 0.14 ms (measured).  Downstream never
 * reads outside [0, vs_kmax): the GPU path gives metal_draw_raw exactly vs_kmax
 * rows, and the CPU path vs_run doesn't exceed this bound either (with relative
 * addressing vs_kmax is 256 anyway). */
static int16_t g_vs_kmax[MAXVS];          /* 0 = not looked at yet */
static void vsk_copy_used(VSConst *dst, const VSConst *src, const VSProg *prog){
    unsigned vs = g_cur_vs;
    int n = (vs < MAXVS) ? g_vs_kmax[vs] : 0;
    if (n <= 0){ n = vs_kmax(prog); if (n < 1) n = 1; if (n > 256) n = 256;
                 if (vs < MAXVS) g_vs_kmax[vs] = (int16_t)n; }
    memcpy(dst->f, src->f, (size_t)n * sizeof dst->f[0]);
    memcpy(dst->i, src->i, sizeof dst->i);
    memcpy(dst->b, src->b, sizeof dst->b);
}
/* The constant register of the position matrix depends on the shader; resolve
 * it once and keep it (-2 = not looked at yet). */
static int8_t g_pos_cx[MAXVS], g_pos_cw[MAXVS]; static uint8_t g_pos_seen[MAXVS];
static unsigned long g_asp_np[MAXVS], g_asp_no[MAXVS]; static float g_asp_cw3[MAXVS];
/* Is this orthographic draw a full-width bar (clip x -1..1, part of the height)?
 * The cinematic letterbox bars are.  With HUDFIX they were narrowed to the centre
 * like the HUD, so in a cinematic the bars stopped at the old 4:3 edges and the
 * scene showed through at the sides.  Such bars are left at full width.  Only
 * small draws (<= 64 vertices, float3/float4 position) are looked at.
 * HUDWIDE=0: old behaviour. */
typedef struct { uint32_t vp, nv, stride; int p_off, p_type; } VtxView;
static unsigned long g_asp_wide = 0;
static float g_wide_ylo, g_wide_yhi;
static int ortho_full_width(const VSConst *vsk, int cx, const VtxView *vv){
    if (!vv || !vv->vp || !vv->nv || vv->nv > 64u || !vv->stride || vv->p_off < 0) return 0;
    if (vv->p_type != 2 && vv->p_type != 3) return 0;        /* FLOAT3 / FLOAT4 */
    const float *r = vsk->f[cx], *ry = vsk->f[cx + 1];
    float lo = 1e30f, hi = -1e30f, ylo = 1e30f, yhi = -1e30f;
    for (uint32_t i = 0; i < vv->nv; i++){
        uint32_t a = vv->vp + i * vv->stride + (uint32_t)vv->p_off;
        if (!mem_is_committed(a + 11)) return 0;
        float x = MRF32(a), y = MRF32(a + 4), z = MRF32(a + 8);
        float w = (vv->p_type == 3) ? MRF32(a + 12) : 1.f;
        float cxv = x * r[0] + y * r[1] + z * r[2] + w * r[3];
        float cyv = x * ry[0] + y * ry[1] + z * ry[2] + w * ry[3];
        if (cxv < lo) lo = cxv; if (cxv > hi) hi = cxv;
        if (cyv < ylo) ylo = cyv; if (cyv > yhi) yhi = cyv;
    }
    g_wide_ylo = ylo; g_wide_yhi = yhi;
    /* Bars only: full width but part of the height and on screen (measured: the
     * cinematic bars are y 0.75..1 and -1..-0.75).  A full-screen picture (splash,
     * loading screen, y -1..1 or larger) keeps its 4:3 shape. */
    return lo <= -0.98f && hi >= 0.98f && (yhi - ylo) < 1.9f && yhi > -1.f && ylo < 1.f;
}
static int hudpos_on(void){ static int v = -1; if (v < 0){ const char *e = ENVC("HUDPOS"); v = (e && *e == '1'); } return v; }
static const VSConst *aspect_apply(const VSConst *vsk, const VSProg *prog, const VtxView *vv){
    if (g_aspfix == 1.f || !prog) return vsk;
    unsigned vs = g_cur_vs;
    if (vs >= MAXVS) return vsk;
    if (!g_pos_seen[vs]){
        int w = -1; int x = vs_pos_creg(prog, &w);
        g_pos_cx[vs] = (int8_t)((x >= 0 && x <= 120) ? x : -1);
        g_pos_cw[vs] = (int8_t)((w >= 0 && w <= 123) ? w : -1);
        g_pos_seen[vs] = 1;
    }
    int cx = g_pos_cx[vs], cw = g_pos_cw[vs];
    if (cx < 0 || cw < 0){ g_asp_ortho++; return vsk; }
    /* Perspective?  oPos.w = dot(v, c[cw]).  In an orthographic/screen matrix this
     * row is (0,0,0,1): w is a constant 1.  In perspective the xyz part becomes the
     * view axis and is NOT ZERO (the w component is the camera distance --
     * measured: 5.79 / 222.0 / -86.5, so the "w==0" test was wrong). */
    g_asp_cw3[vs] = vsk->f[cw][3];
    { float ax = fabsf(vsk->f[cw][0]) + fabsf(vsk->f[cw][1]) + fabsf(vsk->f[cw][2]);
      if (ax < 1e-6f){ g_asp_ortho++; g_asp_no[vs]++;
        /* Orthographic draws into a SMALL render target (the rear-view mirror, 256x256:
         * its frame shade and shape mask) are not UI: squeezing them narrowed the mask
         * and left strips of the mirror's scene at its sides.  Only the screen and
         * screen-sized targets get the HUD treatment. */
        if (g_cur_rt && g_cur_rt_w && g_cur_rt_h &&
            (g_cur_rt_w * 2u < g_bb_w || g_cur_rt_h * 2u < g_bb_h)) return vsk;
        /* HUDFIX=1: also scale the UI about the center, so circles stay circular.
         * CAUTION: fullscreen post-process quads are orthographic too; if they get
         * shrunk, bars appear at the edges. */
        { static int hf = -1;
          if (hf < 0){ const char *e = ENVC("HUDFIX"); hf = !(e && *e && *e=='0'); }
          static int hw = -1;
          if (hw < 0){ const char *e = ENVC("HUDWIDE"); hw = !(e && *e && *e=='0'); }
          if (hf && hw && ortho_full_width(vsk, cx, vv)){
              g_asp_wide++;
              { const char *al = ENVC("ASPLOG"); static int nw = 0;
                static uint32_t seen[256]; static int ns = 0; int dup = 0;
                uint32_t key = (uint32_t)g_cur_ps_id * 2654435761u ^ g_tex_stage[0] * 40503u
                             ^ (uint32_t)(int)(g_wide_ylo * 100.f) * 977u ^ (uint32_t)(int)(g_wide_yhi * 100.f);
                for (int q = 0; q < ns; q++) if (seen[q] == key) dup = 1;
                if (!dup && ns < 256) seen[ns++] = key;
                if (al && !dup && (long)g_present_no >= atol(al) && nw < 200){ nw++;
                      fprintf(stderr,"  [asp-wide] frame=%ld vs=%u ps=%d tex0=%u nv=%u y=%.3f..%.3f\n",
                              (long)g_present_no, vs, g_cur_ps_id, g_tex_stage[0], vv->nv,
                              (double)g_wide_ylo, (double)g_wide_yhi); } }
              return vsk; }
          /* HUDPOS=1 (see metal.m HUD ANCHORING): hand the draw's 4:3 clip-space
           * rectangle to the renderer, which moves the HUD elements to the screen
           * edges per element.  Only while the game world is loaded (TheWorld). */
          if (hf && hudpos_on() && vv && vv->vp && vv->nv && vv->nv <= 4096u && vv->stride && vv->p_off >= 0
              && (vv->p_type == 2 || vv->p_type == 3)
              && mem_is_committed(GA(0x00890080u) + 3u) && MR32(GA(0x00890080u))){
              const float *r = vsk->f[cx], *ry = vsk->f[cx + 1];
              float lo = 1e30f, hi = -1e30f, ylo = 1e30f, yhi = -1e30f; int ok = 1;
              for (uint32_t i = 0; i < vv->nv; i++){
                  uint32_t a = vv->vp + i * vv->stride + (uint32_t)vv->p_off;
                  if (!mem_is_committed(a + 11)){ ok = 0; break; }
                  float x = MRF32(a), y = MRF32(a + 4), z = MRF32(a + 8);
                  float w = (vv->p_type == 3) ? MRF32(a + 12) : 1.f;
                  float cxv = x * r[0] + y * r[1] + z * r[2] + w * r[3];
                  float cyv = x * ry[0] + y * ry[1] + z * ry[2] + w * ry[3];
                  if (cxv < lo) lo = cxv; if (cxv > hi) hi = cxv;
                  if (cyv < ylo) ylo = cyv; if (cyv > yhi) yhi = cyv; }
              if (ok){ void metal_set_next_hud(float, float, float, float);
                       metal_set_next_hud(lo, hi, ylo, yhi); }
          }
          if (hf){ static VSConst ht; vsk_copy_used(&ht, vsk, prog);
                   for (int k = 0; k < 4; k++) ht.f[cx][k] *= g_aspfix;
                   return &ht; } }
        if (ENVC("ASPLOG")){ static int no = 0;
            if (no < 6){ no++;
                fprintf(stderr,"  [asp-orto] vs=%u cx=%d cw=%d  c%d=(%.4f %.4f %.4f %.4f)"
                        "  c%d=(%.4f %.4f %.4f %.4f)\n", vs, cx, cw,
                        cx, vsk->f[cx][0],vsk->f[cx][1],vsk->f[cx][2],vsk->f[cx][3],
                        cw, vsk->f[cw][0],vsk->f[cw][1],vsk->f[cw][2],vsk->f[cw][3]); } }
        return vsk; } }
    g_asp_persp++; g_asp_np[vs]++;
    { int ws_game_fov(void); if (ws_game_fov()) return vsk; }   /* wsfov.c */
    static VSConst tmp;
    vsk_copy_used(&tmp, vsk, prog);
    for (int k = 0; k < 4; k++) tmp.f[cx][k] *= g_aspfix;
    if (ENVC("ASPLOG")){ static int n = 0;
        if (n < 6){ n++;
            fprintf(stderr,"  [asp] vs=%u c%d=(%.4f %.4f %.4f %.4f) -> (%.4f %.4f %.4f %.4f)"
                    "  c%d=(%.3f %.3f %.3f %.3f)\n", g_cur_vs, cx,
                    vsk->f[cx][0],vsk->f[cx][1],vsk->f[cx][2],vsk->f[cx][3],
                    tmp.f[cx][0],tmp.f[cx][1],tmp.f[cx][2],tmp.f[cx][3],
                    cw, vsk->f[cw][0],vsk->f[cw][1],vsk->f[cw][2],vsk->f[cw][3]); } }
    return &tmp;
}

static const VSConst *fixmat_apply(const VSConst *vsk){
    if (!(vsk->f[3][3] == 0.f && vsk->f[3][0] == 0.f &&
          vsk->f[0][0] == 0.f && vsk->f[1][1] == 0.f)) return vsk;
    if (g_fixmat < 0){ const char *e6 = ENVC("FIXMAT");
                       g_fixmat = (e6 && *e6 && *e6 != '0') ? 1 : 0; }
    if (!g_fixmat) return vsk;
    static VSConst tmp;
    static int mode = -1;
    if (mode < 0){ const char *e4 = ENVC("FIXMAT_MODE"); mode = e4 ? atoi(e4) : 0; }
    memcpy(&tmp, vsk, sizeof tmp);
    if (mode == 4){
        static float t4 = -1e30f;
        if (t4 == -1e30f){ const char *e9 = ENVC("FIXMAT_YOFF");
                           t4 = e9 ? (float)atof(e9) : 0.f; }
        memset(tmp.f, 0, sizeof tmp.f);
        tmp.f[0][0]=1.f; tmp.f[1][1]=1.f; tmp.f[1][3]=t4;
        tmp.f[2][2]=1.f; tmp.f[3][3]=1.f;
        return &tmp;
    }
    if (mode == 3){
        /* The raw positions are already in clip space; the matrix the game wants to
         * write is the identity. */
        memset(tmp.f, 0, sizeof tmp.f);
        tmp.f[0][0]=1.f; tmp.f[1][1]=1.f; tmp.f[2][2]=1.f; tmp.f[3][3]=1.f;
        return &tmp;
    }
    if (mode == 1 || mode == 2){
        float ys = (mode == 1) ? -1.f : 1.f;
        float yb = (mode == 1) ?  1.f : -1.f;
        tmp.f[0][0]=0.75f; tmp.f[0][1]=0; tmp.f[0][2]=0; tmp.f[0][3]=0;
        tmp.f[1][0]=0; tmp.f[1][1]=ys; tmp.f[1][2]=0; tmp.f[1][3]=yb;
        tmp.f[2][0]=0; tmp.f[2][1]=0; tmp.f[2][2]=1.f; tmp.f[2][3]=0;
        tmp.f[3][0]=0; tmp.f[3][1]=0; tmp.f[3][2]=0; tmp.f[3][3]=1.f;
        return &tmp;
    }
    tmp.f[0][0]=0.75f; tmp.f[0][1]=0; tmp.f[0][2]=0; tmp.f[0][3]=0;
    { static float yoff = -1e30f, ysc = 1.f;
      if (yoff == -1e30f){
          const char *e2 = ENVC("FIXMAT_YOFF");
          const char *e3 = ENVC("FIXMAT_YSCALE");
          yoff = e2 ? (float)atof(e2) : 0.f;
          ysc  = e3 ? (float)atof(e3) : 1.f; }
      tmp.f[1][0]=0; tmp.f[1][1]=ysc; tmp.f[1][2]=0; tmp.f[1][3]=-yoff; }
    tmp.f[2][0]=0; tmp.f[2][1]=0; tmp.f[2][2]=1.f; tmp.f[2][3]=0;
    tmp.f[3][0]=0; tmp.f[3][1]=0; tmp.f[3][2]=0; tmp.f[3][3]=1.f;
    return &tmp;
}

/* Per stage "which coordinate set + projective divide".
 * D3DTSS_TEXCOORDINDEX = 11, D3DTSS_TEXTURETRANSFORMFLAGS = 24.
 * Encoding: index + 16*mode  (mode 0 = no divide, 1 = /z, 2 = /w).
 * In the trace, stages 1 and 2 use COORDINATE 0 in 422 thousand calls; we were
 * giving stage N coordinate N. */
/* Carry the game's D3DSAMP_* state over to the Metal sampler.
 * D3DSAMP: 1=ADDRESSU 2=ADDRESSV 5=MAGFILTER 6=MINFILTER 7=MIPFILTER
 *          9=MAXMIPLEVEL 10=MAXANISOTROPY
 * Previously all of it was ignored and everything was drawn linear+mip-linear. */
static void smp_push(void){
    void metal_set_samplers(const unsigned k[4]);
    unsigned k[4];
    for (int st = 0; st < 4; st++){
        unsigned mn = g_smp_raw[st][6], mg = g_smp_raw[st][5], mp = g_smp_raw[st][7];
        unsigned au = g_smp_raw[st][1], av = g_smp_raw[st][2], an = g_smp_raw[st][10];
        if (mn < 1u || mn > 3u) mn = 2u;          /* invalid/unset -> LINEAR */
        if (mg < 1u || mg > 3u) mg = 2u;
        if (mp > 2u) mp = 0u;
        if (au < 1u || au > 5u) au = 1u;          /* -> WRAP */
        if (av < 1u || av > 5u) av = 1u;
        if (an < 1u) an = 1u; if (an > 16u) an = 16u;
        unsigned al = 0; while ((1u << al) < an) al++;
        k[st] = (mn & 3u) | ((mg & 3u) << 2) | ((mp & 3u) << 4)
              | ((au & 7u) << 6) | ((av & 7u) << 9) | ((al & 7u) << 12);
    }
    metal_set_samplers(k);
}

static void texproj_push(void){
    void metal_set_texproj(const float[4]);
    float sel[4];
    for (int st = 0; st < 4; st++){
        unsigned ci  = g_tss_raw[st][11] & 0xFFFFu;
        unsigned ttf = g_tss_raw[st][24];
        if (ci > 3u) ci = (unsigned)st;
        int pm = 0;
        if (ttf & 0x100u){                 /* D3DTTFF_PROJECTED */
            /* NFSU2 writes this with a COUNT FIELD of 0 (measured: all 28 calls in the trace
             * are exactly "D3DTTFF_PROJECTED").  The vertex shader produces a
             * 4-component coordinate (oT1.xyz = dot(v0,c4..c6), oT1.w = r0.x), so a
             * count of 0 -> divide by W.
             * Without the divide the headlight texture spreads over the surface
             * instead of forming a cone: the "headlight streaks" the user saw and the
             * striped reflections on the ground (stage 3 = reflected_sampler) were this. */
            unsigned cnt = ttf & 7u;
            pm = (cnt == 3u) ? 1 : 2;      /* 3 -> xy/z, the others -> xy/w */
        }
        sel[st] = (float)(ci + 16u*(unsigned)pm);
    }
    metal_set_texproj(sel);
}

/* dip_indexed -> draw_up: this draw's vertices are to be read by index from the
 * mirrors in the pool (when n > 0).  If draw_up can't take the GPU path it returns
 * 0 and the caller falls back to the old (index expansion) path. */
static struct { uint32_t n, vbo, ibo, isz; int32_t base; uint64_t gh; } g_ix;
static uint64_t g_dip_ix = 0, g_dip_ixfb[6];
static int draw_up(uint32_t pt, uint32_t nv, uint32_t vp, uint32_t stride){
    uint64_t _t0 = now_ns();
    static TVert tv[4096];
    const VSConst *vsk = &g_vsk;
    if (g_fixmat < 0){ const char *e6 = ENVC("FIXMAT");
        g_fixmat = (e6 && *e6 && *e6 != '0') ? 1 : 0; }
    if (g_gpuvs < 0){ const char *e8 = ENVC("GPUVS");
        g_gpuvs = (e8 && *e8 && *e8 != '0') ? 1 : 0; }
    const VSProg *prog = (g_cur_vs && g_cur_vs < MAXVS) ? g_vsprog[g_cur_vs] : 0;
    vsk = fixmat_apply(vsk);

    /* Declaration parsing ONCE per (declaration, shader).  It used to rebuild from
     * scratch on every draw, reading up to 32 elements from guest memory and testing
     * them with mem_is_committed (~1300 times per frame).  Safe: declaration objects
     * are copied when created and never change, and com_alloc doesn't reuse
     * addresses. */
    static struct { uint32_t decl; const VSProg *prog; int nel, p_off, p_type, c_off, c_type,
                    t_off, t_type, ok; DElem el[16]; } dcc[64];
    uint32_t d = g_cur_decl;
    unsigned dci = (unsigned)(((uintptr_t)prog >> 4) ^ (d >> 3) ^ (d >> 11)) & 63u;
    int dlog = ENVC("DECLLOG") != NULL;
    if (dlog || !dcc[dci].ok || dcc[dci].decl != d || dcc[dci].prog != prog){
    DElem el[16]; int nel = 0;
    int p_off = -1, p_type = 2, c_off = -1, c_type = 4, t_off = -1, t_type = 1;
    uint32_t ep = (d && mem_is_committed(d+8)) ? MR32(d+4) : 0;
    if (ep){
        for (int k = 0; k < 32 && nel < 16; k++){
            uint32_t e = ep + 8u*k;
            if (!mem_is_committed(e+7)) break;
            unsigned st=MR16(e), of=MR16(e+2), ty=MR8(e+4), us=MR8(e+6), ui=MR8(e+7);
            if (st == 0xFFu && ty == 17u) break;
            if (st != 0) continue;                       /* stream 0 only */
            /* DECLLOG=<ps>: print only the declarations drawing with that pixel shader,
             * once, with ALL their elements. */
            { const char *dl = ENVC("DECLLOG");
              if (dl && atoi(dl) == g_cur_ps_id){
                static uint32_t seen[32]; static int sn = 0; int nw = 1;
                for (int q=0;q<sn;q++) if (seen[q]==d) nw = 0;
                if (nw && k == 0 && sn < 32){ seen[sn++] = d;
                    fprintf(stderr,"  [decl] ps=%d decl=0x%08x stride=%u vs=%u\n",
                            g_cur_ps_id, d, stride, g_cur_vs);
                    for (int q2 = 0; q2 < 24; q2++){
                        uint32_t e2 = ep + 8u*q2;
                        if (!mem_is_committed(e2+7)) break;
                        unsigned s2=MR16(e2), o2=MR16(e2+2), t2=MR8(e2+4),
                                 u2=MR8(e2+6), i2=MR8(e2+7);
                        if (s2 == 0xFFu && t2 == 17u) break;
                        int rg2 = prog ? vs_input_reg(prog,(int)u2,(int)i2) : -2;
                        fprintf(stderr,"       stream=%u offset=%2u type=%u usage=%u/%u -> v%d\n",
                                s2,o2,t2,u2,i2,rg2);
                    }
                }
              } }
            if (us == 0 && ui == 0){ p_off = (int)of; p_type = (int)ty; }
            else if (us == 10 && ui == 0){ c_off = (int)of; c_type = (int)ty; }
            else if (us == 5 && ui == 0){ t_off = (int)of; t_type = (int)ty; }
            if (prog){
                int rg = vs_input_reg(prog, (int)us, (int)ui);
                if (rg >= 0){ el[nel].off=(uint16_t)of; el[nel].type=(uint8_t)ty;
                              el[nel].reg=(int8_t)rg; nel++; }
            }
        }
    }
    /* without a declaration, NFSU2's standard layout: pos@0 color@12 uv@16 */
    if (p_off < 0){ p_off = 0;  p_type = 2; }
    if (c_off < 0){ c_off = 12; c_type = 4; }
    if (t_off < 0){ t_off = 16; t_type = 1; }
    dcc[dci].decl = d; dcc[dci].prog = prog; dcc[dci].nel = nel;
    dcc[dci].p_off = p_off; dcc[dci].p_type = p_type; dcc[dci].c_off = c_off;
    dcc[dci].c_type = c_type; dcc[dci].t_off = t_off; dcc[dci].t_type = t_type;
    memcpy(dcc[dci].el, el, sizeof el); dcc[dci].ok = 1;
    }
    const DElem *el = dcc[dci].el; int nel = dcc[dci].nel;
    int p_off = dcc[dci].p_off, p_type = dcc[dci].p_type, c_off = dcc[dci].c_off,
        c_type = dcc[dci].c_type, t_off = dcc[dci].t_off, t_type = dcc[dci].t_type;
    { VtxView vv = { vp, nv, stride, p_off, p_type };
      vsk = aspect_apply(vsk, prog, &vv); }

    /* ---------------- GPU vertex shader (GPUVS) ----------------
     * Measured: ~45% of the run time was in the vs_run interpreter on the CPU,
     * ceiling ~3.5 M vertices/s.  The game's own bytecode is translated to MSL and
     * run on the GPU; vertices are sent RAW, and the read layout is taken from the
     * D3D declaration and embedded in the shader.
     * An untranslatable shader (flow control etc.) doesn't silently run wrong:
     * vs_to_msl returns 0 and we fall back to the proven CPU path. */
    if (prog && nel && stride && stride <= 256u && g_gpuvs){
        int gid = gpuvs_id(prog, g_cur_vs, g_cur_decl, el, nel);
        if (gid > 0){
            int  metal_draw_raw(const void*, unsigned, unsigned, unsigned,
                                unsigned, int, const float*, unsigned);
            void metal_set_stage_tex(unsigned,unsigned,unsigned);
            void metal_set_ps(int, const float *);
            metal_set_stage_tex(g_tex_stage[1], g_tex_stage[2], g_tex_stage[3]);
            metal_set_ps(g_cur_ps_id, &g_psc[0][0]);
            texproj_push(); smp_push();
            if (g_ix.n){
                int metal_draw_ix(unsigned, unsigned, unsigned, unsigned, unsigned, int,
                                  unsigned, int, const float *, unsigned, unsigned long long);
                if (metal_draw_ix(g_ix.vbo, stride, g_ix.ibo, g_ix.isz, g_ix.n, g_ix.base,
                                  g_cur_tex, gid, &vsk->f[0][0], (unsigned)g_gvs_nk[gid], g_ix.gh)){
                    g_vs_draws++; g_gpuvs_draws++; g_v_xform += nv;
                    g_t_draw_ns += now_ns() - _t0;
                    return 1;
                }
                return 0;
            }
            /* Is it a quad that fully overwrites the target?  (see metal.m PASS LOAD)
             * Only candidates that don't read the target: blending off, 4 channels
             * written, no alpha test and no Z test, at most 6 vertices.  The vertices
             * are transformed on the CPU; a few quads of the post-process chain per
             * frame. */
            { void metal_set_next_cov_clip(const float (*)[4], unsigned, unsigned);
              if (nv <= 6u && (pt == 4u || pt == 5u || pt == 6u) && !g_rs[27] &&
                  (g_rs[168] & 15u) == 15u && !g_rs[15] && (!g_rs[7] || (g_rs[23] & 15u) == 8u)){
                  float cp[6][4], cuv[6][16];
                  for (uint32_t i = 0; i < nv; i++){
                      float v[16][4];
                      for (int r2 = 0; r2 < 16; r2++){ v[r2][0]=v[r2][1]=v[r2][2]=0.f; v[r2][3]=1.f; }
                      for (int k = 0; k < nel; k++)
                          decl_fetch(vp + i*stride + el[k].off, el[k].type, v[el[k].reg]);
                      VSOut so;
                      vs_run(prog, vsk, (const float (*)[4])v, &so);
                      memcpy(cp[i], so.pos, sizeof cp[i]);
                      memcpy(cuv[i], so.tex, sizeof cuv[i]);          /* oT0..oT3 */
                  }
                  { void metal_set_next_cov_uv(const float (*)[16], unsigned);
                    metal_set_next_cov_uv((const float (*)[16])cuv, nv); }
                  metal_set_next_cov_clip((const float (*)[4])cp, nv, pt);
              } }
            if (metal_draw_raw(g_mem + vp, nv, stride, pt, g_cur_tex, gid,
                               &vsk->f[0][0], (unsigned)g_gvs_nk[gid])){
                g_vs_draws++; g_gpuvs_draws++; g_v_xform += nv;
                g_t_draw_ns += now_ns() - _t0;
                return 1;
            }
            /* Couldn't be queued (arena overflow): let the CPU path continue --
             * silently dropping it destroyed geometry. */
            g_gpuvs_drop++;
        }
    }
    if (g_ix.n) return 0;           /* an indexed source can't be given to the CPU path */

    for (uint32_t i = 0; i < nv; i++){
        uint32_t o = vp + i*stride;
        TVert *out = &tv[i];
        if (prog && nel){
            /* DIAGNOSTIC (FIXMAT=1): the game's WVP matrix arrives as diag(0,0,1,1) --
             * the X/Y scale is zero.  While looking for the root cause, put in a
             * reasonable orthographic matrix to see the rest of the pipeline. */
            /* Is the matrix healthy per draw?  If some draws arrive with the right matrix,
             * the front end uses two different spaces. */
            { int degenerate = (vsk->f[3][3] == 0.f && vsk->f[3][0] == 0.f &&
                                vsk->f[0][0] == 0.f && vsk->f[1][1] == 0.f);
              if (degenerate){ g_mat_bad++;
                  static uint32_t seen[8]; static int ns=0;
                  int f4=0; for(int q=0;q<ns;q++) if(seen[q]==g_cur_vs) f4=1;
                  if(!f4 && ns<8 && ENVC("MATLOG")){ seen[ns++]=g_cur_vs;
                      fprintf(stderr,"  [matrix-BROKEN] shader=%u texture=%u last upload sr=%u n=%u pd=%08x\n",
                              g_cur_vs, g_cur_tex,
                              g_scn?g_sc[(g_scn-1)&7].sr:0,
                              g_scn?g_sc[(g_scn-1)&7].n:0,
                              g_scn?g_sc[(g_scn-1)&7].pd:0); } }
              else {
                  g_mat_ok++;
                  static uint32_t seenok[8]; static int nsok=0;
                  int f5=0; for(int q=0;q<nsok;q++) if(seenok[q]==g_cur_vs) f5=1;
                  if(!f5 && nsok<8 && ENVC("MATLOG")){ seenok[nsok++]=g_cur_vs;
                      fprintf(stderr,"  [matrix-VALID] shader=%u texture=%u pd=%08x\n",
                              g_cur_vs, g_cur_tex, g_scn?g_sc[(g_scn-1)&7].pd:0); }
                  static int shown = 0;
                  if (0){ shown++;
                      fprintf(stderr,"  [matrix-REAL] draw %llu:\n",
                              (unsigned long long)g_vs_draws);
                      for (int rw=0; rw<4; rw++)
                          fprintf(stderr,"      %9.4f %9.4f %9.4f %9.4f\n",
                                  vsk->f[rw][0], vsk->f[rw][1],
                                  vsk->f[rw][2], vsk->f[rw][3]);
                  }
              } }
            float v[16][4];
            for (int r2 = 0; r2 < 16; r2++){ v[r2][0]=v[r2][1]=v[r2][2]=0.f; v[r2][3]=1.f; }
            for (int k = 0; k < nel; k++)
                decl_fetch(o + el[k].off, el[k].type, v[el[k].reg]);
            VSOut so;
            /* NOVS=1: MEASUREMENT only.  Skips the vertex shader interpreter to see whether
             * the bottleneck really is here. */
            static int novs = -1;
            if (novs < 0){ const char *e7 = ENVC("NOVS"); novs = (e7 && *e7 && *e7!='0'); }
            if (novs){ memset(&so, 0, sizeof so);
                       so.pos[0]=v[0][0]; so.pos[1]=v[0][1];
                       so.pos[2]=v[0][2]; so.pos[3]=1.f;
                       so.col[0][0]=so.col[0][1]=so.col[0][2]=so.col[0][3]=1.f; }
            else { uint64_t _v0 = now_ns();
                   vs_run(prog, vsk, (const float (*)[4])v, &so);
                   g_t_vs_ns += now_ns() - _v0; }
            /* D3D9: vs_1_1 - vs_2_x color outputs are clamped to [0,1] (same as the GPU path,
             * see vsemu.c vs_to_msl) */
            if (vs_colsat_on() && vs_version_major(prog) < 3)
                for (int c2 = 0; c2 < 2; c2++) for (int k2 = 0; k2 < 4; k2++){
                    float x = so.col[c2][k2]; so.col[c2][k2] = !(x > 0.f) ? 0.f : (x > 1.f ? 1.f : x); }
            out->x=so.pos[0]; out->y=so.pos[1]; out->z=so.pos[2]; out->w=so.pos[3];
            out->r=so.col[0][0]; out->g=so.col[0][1];
            out->b=so.col[0][2]; out->a=so.col[0][3];
            out->r1=so.col[1][0]; out->g1=so.col[1][1];
            out->b1=so.col[1][2]; out->a1=so.col[1][3];
            /* On the GPU path the vertex shader does the selection; the CPU path must apply
             * the SAME permutation, otherwise the two paths diverge. */
            for (int ts = 0; ts < 4; ts++){
                unsigned ci = g_tss_raw[ts][11] & 0xFFFFu;
                if (ci > 3u) ci = (unsigned)ts;
                for (int tc = 0; tc < 4; tc++) out->t[ts][tc] = so.tex[ci][tc];
            }
        } else {
            float pos[4], col[4], uv[4];
            decl_fetch(o + (unsigned)p_off, (unsigned)p_type, pos);
            decl_fetch(o + (unsigned)c_off, (unsigned)c_type, col);
            decl_fetch(o + (unsigned)t_off, (unsigned)t_type, uv);
            out->x=pos[0]; out->y=pos[1]; out->z=pos[2];
            out->w=(p_type==3)?pos[3]:1.f;
            out->r=col[0]; out->g=col[1]; out->b=col[2]; out->a=col[3];
            out->r1=0.f; out->g1=0.f; out->b1=0.f; out->a1=0.f;
            for (int ts = 0; ts < 4; ts++){
                out->t[ts][0]=uv[0]; out->t[ts][1]=uv[1];
                out->t[ts][2]=0.f;   out->t[ts][3]=1.f;
            }
        }
    }
    if (prog && nel){
        g_vs_draws++;
        /* The RAW position range of shaded draws: shows which space the UI is in and what
         * the matrix should do. */
        for (uint32_t i3 = 0; i3 < nv; i3++){
            uint32_t o3 = vp + i3*stride;
            if (!mem_is_committed(o3 + (unsigned)p_off + 11)) break;
            float rx = MRF32(o3 + (unsigned)p_off);
            float ry = MRF32(o3 + (unsigned)p_off + 4);
            float rz = MRF32(o3 + (unsigned)p_off + 8);
            if (rx < g_rmin[0]) g_rmin[0] = rx;  if (rx > g_rmax[0]) g_rmax[0] = rx;
            if (ry < g_rmin[1]) g_rmin[1] = ry;  if (ry > g_rmax[1]) g_rmax[1] = ry;
            if (rz < g_rmin[2]) g_rmin[2] = rz;  if (rz > g_rmax[2]) g_rmax[2] = rz;
        }
    } else g_ff_draws++;
    /* Are the transformed vertices on screen?  Go from clip space to NDC and count. */
    { for (uint32_t i2 = 0; i2 < nv; i2++){
        float w2 = tv[i2].w;
        if (w2 <= 0.f){ g_v_badw++; continue; }
        float nx = tv[i2].x / w2, ny = tv[i2].y / w2;
        if (nx >= -1.f && nx <= 1.f && ny >= -1.f && ny <= 1.f) g_v_in++;
        else g_v_out++;
      } }
    { void metal_set_stage_tex(unsigned,unsigned,unsigned);
      void metal_set_ps(int, const float *);
      metal_set_stage_tex(g_tex_stage[1], g_tex_stage[2], g_tex_stage[3]);
      metal_set_ps(g_cur_ps_id, &g_psc[0][0]);
      texproj_push(); smp_push();
      if (ENVC("PSC0")){ static int n5=0;
          if (n5<6 && g_tex_stage[1]){ n5++;
              fprintf(stderr,"  [ps] ps=%d c0=(%.3f %.3f %.3f %.3f) tex=%u/%u/%u/%u\n",
                      g_cur_ps_id,
                      (double)g_psc[0][0],(double)g_psc[0][1],
                      (double)g_psc[0][2],(double)g_psc[0][3],
                      g_tex_stage[0], g_tex_stage[1],
                      g_tex_stage[2], g_tex_stage[3]); } } }
    { int nstage=0; for(int q=0;q<4;q++) if (g_tex_stage[q]) nstage++;
      if (nstage>0 && nstage<=4) g_stage_hist[nstage-1]++;
      static int shown=0;
      if (ENVC("STAGEDUMP") && nstage>=2 && shown<8){ shown++;
          fprintf(stderr,"  [stage] draw: tex0=%u tex1=%u tex2=%u tex3=%u  verts=%u\n",
                  g_tex_stage[0],g_tex_stage[1],g_tex_stage[2],g_tex_stage[3], nv); } }
    g_t_draw_ns += now_ns() - _t0; g_v_xform += nv;
    if (prog && g_cur_vs < 64u) g_v_per_vs[g_cur_vs] += nv;
    if (dd_on()){
        static int nd = 0;
        if (nd++ < 6){
            fprintf(stderr,"   >>> draw type=%u verts=%u stride=%u tex=%u shader=%u elements=%d\n",
                    pt, nv, stride, g_cur_tex, g_cur_vs, nel);
            if (prog) vs_disasm(prog, stderr);
            for (int k=0;k<nel;k++)
                fprintf(stderr,"      element: offset=%u type=%u -> v%d\n",
                        el[k].off, el[k].type, el[k].reg);
            for (uint32_t i=0;i<nv && i<3;i++){
                uint32_t o = vp + i*stride;
                if (prog && nel){
                    float v[16][4];
                    for (int r2=0;r2<16;r2++){ v[r2][0]=v[r2][1]=v[r2][2]=0.f; v[r2][3]=1.f; }
                    for (int k=0;k<nel;k++) decl_fetch(o+el[k].off, el[k].type, v[el[k].reg]);
                    for (int k=0;k<nel;k++)
                        fprintf(stderr,"        v%d = %g %g %g %g\n", el[k].reg,
                                v[el[k].reg][0],v[el[k].reg][1],v[el[k].reg][2],v[el[k].reg][3]);
                }
                fprintf(stderr,"        -> pos=(%g %g %g %g) color=(%.3g %.3g %.3g %.3g) uv=(%g %g)\n",
                        tv[i].x,tv[i].y,tv[i].z,tv[i].w,
                        tv[i].r,tv[i].g,tv[i].b,tv[i].a, tv[i].t[0][0],tv[i].t[0][1]);
            }
            fprintf(stderr,"      last constant uploads (newest to oldest):\n");
            for (int q=1; q<=4 && q<=(int)g_scn; q++){
                unsigned ix = (g_scn - q) & 7;
                fprintf(stderr,"        sr=%-3u n=%-3u pd=%08x", g_sc[ix].sr, g_sc[ix].n, g_sc[ix].pd);
                if (g_sc[ix].pd && mem_is_committed(g_sc[ix].pd+63))
                    fprintf(stderr,"  first4=[%.4g %.4g %.4g %.4g][%.4g %.4g %.4g %.4g]",
                        MRF32(g_sc[ix].pd), MRF32(g_sc[ix].pd+4),
                        MRF32(g_sc[ix].pd+8), MRF32(g_sc[ix].pd+12),
                        MRF32(g_sc[ix].pd+16), MRF32(g_sc[ix].pd+20),
                        MRF32(g_sc[ix].pd+24), MRF32(g_sc[ix].pd+28));
                fprintf(stderr,"\n");
            }
            /* the c0..c7 in use */
            for (int q=0;q<8;q++)
                fprintf(stderr,"        c%d = %g %g %g %g\n", q,
                        g_vsk.f[q][0],g_vsk.f[q][1],g_vsk.f[q][2],g_vsk.f[q][3]);
        }
    }
    metal_draw_tv(tv, nv, pt, g_cur_tex);
    return 1;
}

/* DrawIndexedPrimitive fast path: static VBs/IBs are mirrored in the pool and drawn
 * indexed (see metal.m PERSISTENT VERTEX / INDEX POOL).  0 = couldn't be applied,
 * the caller falls back to the index expansion path. */
static int dip_indexed(uint32_t pt, uint32_t basev, uint32_t starti, uint32_t ntri, uint32_t isz){
    int metal_mirror(uint32_t, uint32_t, const void *, uint32_t, unsigned, uint32_t *, uint32_t *);
    const uint8_t *metal_pool_cpu(void);
    static int off = -1;             /* NOIX=1: off (A/B measurement, safety) */
    if (off < 0){ const char *e = ENVC("NOIX"); off = (e && *e && *e != '0'); }
    if (off) return 0;
    if (pt != 4u || !g_gpuvs){ g_dip_ixfb[0]++; return 0; }         /* strip/fan: the old path */
    uint32_t vb = g_ss_vb, ib = g_ib_obj, stride = g_ss_stride, so = g_ss_off;
    if (!mem_is_committed(vb+63) || !mem_is_committed(ib+63)){ g_dip_ixfb[1]++; return 0; }
    uint32_t vsz = MR32(vb+4), vba = MR32(vb+8), isb = MR32(ib+4), iba = MR32(ib+8);
    /* if it was locked in the last 4 frames its content may be changing: instead of
     * re-mirroring every frame, the old path (dynamic buffers always go there) */
    if ((uint32_t)g_present - MR32(vb+52) < 4u || (uint32_t)g_present - MR32(ib+52) < 4u){
        g_dip_ixfb[2]++; return 0; }
    uint32_t ni = ntri * 3u;
    if ((stride & 3u) || (so & 3u) || (int32_t)basev < 0 || !vba || !iba || !vsz || !isb ||
        (uint64_t)(starti + ni) * isz > isb ||
        !mem_is_committed(vba + vsz - 1u) || !mem_is_committed(iba + isb - 1u)){
        g_dip_ixfb[3]++; return 0; }
    /* The Metal index offset must be a multiple of 4; for an odd start with 16-bit
     * indices a second mirror of the IB shifted by 2 bytes is used (key ib+2:
     * com_alloc is 16-aligned, it doesn't collide with another object).
     * Measured: without this 19% of DIP draws fell back to the old path. */
    uint32_t sh = ((starti * isz) & 3u) ? 2u : 0u;
    if (sh && isz != 2u){ g_dip_ixfb[3]++; return 0; }
    uint32_t voff, ioff, imax;
    if (!metal_mirror(vb, MR32(vb+48), g_mem + vba, vsz, 0, &voff, NULL) ||
        !metal_mirror(ib + sh, MR32(ib+48), g_mem + iba + sh, isb - sh, isz, &ioff, &imax)){
        g_dip_ixfb[4]++; return 0; }
    /* Bound: if the largest index of the whole IB fits this VB, no scan; if it doesn't
     * (one IB serves several VBs), the draw's range. */
    if ((uint64_t)so + ((uint64_t)basev + imax + 1u) * stride > vsz){
        const uint8_t *P = metal_pool_cpu() + ioff + (size_t)starti * isz - sh;
        uint32_t mx = 0;
        if (isz == 2u){ const uint16_t *x = (const uint16_t *)P; for (uint32_t k = 0; k < ni; k++) if (x[k] > mx) mx = x[k]; }
        else          { const uint32_t *x = (const uint32_t *)P; for (uint32_t k = 0; k < ni; k++) if (x[k] > mx) mx = x[k]; }
        if ((uint64_t)so + ((uint64_t)basev + mx + 1u) * stride > vsz){ g_dip_ixfb[5]++; return 0; }
    }
    /* geometry id (HD ray tracing cache): same mirror + same range = same geometry;
     * the id changes when the generation changes */
    uint64_t gh = 1469598103934665603ull;
    uint32_t kv[9] = { vb, MR32(vb+48), ib, MR32(ib+48), starti, ni, basev, stride, so };
    for (int q = 0; q < 9; q++){ gh = (gh ^ kv[q]) * 1099511628211ull; gh ^= gh >> 29; }
    g_ix.n = ni; g_ix.vbo = voff + so; g_ix.ibo = ioff + starti * isz - sh; g_ix.isz = isz;
    g_ix.base = (int32_t)basev; g_ix.gh = gh | 1ull;
    int ok = draw_up(4u, ni, 0u, stride);
    g_ix.n = 0;
    if (ok) g_dip_ix++;
    return ok;
}

/* ---- D3DX HLE Stage 2: call recording and no-effect check (dxrec.h) ----
 * D3DX's state assignment (sub_00669296) produces ~36 thousand device calls per
 * frame and 96% of them rewrite the value already on the device.  Here, whether
 * the values a handler would write if a call were made NOW are the same as the
 * current state is checked by repeating the handler's own logic.
 * Only the 8 methods below are supported; any other device call invalidates the
 * record (no skipping is done).                                          */
#include "dxrec.h"
#define DXREC_DEPTH 8
static DxRec *g_dxrec[DXREC_DEPTH];
int    g_dxrec_depth = 0;
const char *dxrec_mid_name(int mid){ return (mid > 0 && mid < MID__count) ? g_midname[mid] : "?"; }
int dxrec_sb_recording(void){ return g_sb_rec != 0 || g_sb_replay != 0; }
void dxrec_begin(DxRec *r, int kind){
    int in_state = 0, same = 0;
    for (int i = 0; i < g_dxrec_depth && i < DXREC_DEPTH; i++){
        if (g_dxrec[i]->kind == DXREC_STATE) in_state = 1;
        if (g_dxrec[i]->kind == kind) same = 1;
    }
    if (in_state || (same && kind == DXREC_PASS))
        for (int i = 0; i < g_dxrec_depth && i < DXREC_DEPTH; i++) g_dxrec[i]->ok = 0;
    r->kind = (uint8_t)kind; r->ok = 1; r->ncall = 0; r->ndata = 0; r->allred = 1;
    if (g_dxrec_depth < DXREC_DEPTH) g_dxrec[g_dxrec_depth] = r; else r->ok = 0;
    g_dxrec_depth++;
}
void dxrec_append(const DxRec *src){
    int lim = g_dxrec_depth < DXREC_DEPTH ? g_dxrec_depth : DXREC_DEPTH;
    for (int i = 0; i < lim; i++){
        DxRec *r = g_dxrec[i];
        if (!r->ok) continue;
        if (!src->ok || r->ncall + src->ncall > r->maxcall
            || r->ndata + src->ndata > r->maxdata){ r->ok = 0; continue; }
        for (int k = 0; k < src->ncall; k++){
            DxCall *d = &r->call[r->ncall++];
            *d = src->call[k];
            d->doff = (uint16_t)(src->call[k].doff + r->ndata);
        }
        memcpy(r->data + r->ndata, src->data, src->ndata);
        r->ndata = (uint16_t)(r->ndata + src->ndata);
    }
}
void dxrec_end(void){ if (g_dxrec_depth > 0) g_dxrec_depth--; }

/* The number of bytes to copy/compare for constant methods with a data pointer
 * (the same as the handler's bounds). */
static uint32_t dxrec_datalen(int mid, const uint32_t *a){
    if (MID(SetVertexShaderConstantF)){ uint32_t n = a[2]; if (a[0] >= 256u) return 0;
        if (a[0] + n > 256u) n = 256u - a[0]; return n*16u; }
    if (MID(SetPixelShaderConstantF)){ uint32_t n = a[2]; if (a[0] >= 32u) return 0;
        if (a[0] + n > 32u) n = 32u - a[0]; return n*16u; }
    return 0;
}
/* a[]: the arguments after this; d: constant data (dlen bytes). */
static int dxrec_call_redundant(int mid, const uint32_t *a, const uint8_t *d, uint32_t dlen){
    if (MID(SetRenderState))
        return a[0] >= 256u || (g_rs[a[0]] == a[1] && !g_rs_dirty);
    if (MID(SetSamplerState))
        return !(a[0] < 16u && a[1] < 20u) || g_smp_raw[a[0]][a[1]] == a[2];
    if (MID(SetTextureStageState))
        return !(a[0] < 8u && a[1] < 40u) || g_tss_raw[a[0]][a[1]] == a[2];
    if (MID(SetTexture)){
        uint32_t stg = a[0], to = a[1];
        if (stg >= 8u) return 1;
        uint32_t der = (to && mem_is_committed(to+28)) ? MR32(to+28) : 0;
        if (g_raw_tex[stg] != to) return 0;
        if (stg < 4u && g_tex_stage[stg] != der) return 0;
        if (stg == 0 && g_cur_tex != der) return 0;
        return 1;
    }
    if (MID(SetVertexShader)){
        uint32_t h = a[0];
        unsigned der = (h && mem_is_committed(h+15)) ? MR32(h+12) : 0;
        if (der >= MAXVS) der = 0;
        return g_raw_vs == h && g_cur_vs == der;
    }
    if (MID(SetPixelShader)){
        uint32_t h = a[0];
        int der = (h && mem_is_committed(h+15)) ? (int)MR32(h+12) : 0;
        return g_raw_ps == h && g_cur_ps_id == der;
    }
    if (MID(SetVertexShaderConstantF))
        return dlen == 0 || !memcmp(g_vsk.f[a[0]], d, dlen);
    if (MID(SetPixelShaderConstantF))
        return dlen == 0 || !memcmp(g_psc[a[0]], d, dlen);
    return 0;
}
/* The D3DX native path (d3dx_hle.c, Stage 4): would this device call have no
 * effect if made NOW?  If so the caller doesn't make it at all (saving the guest
 * dispatch + com_call).  NO during state block recording/replay, with an open
 * dxrec record or in an FXTRACE frame: the call must really be made so it gets
 * into the record/trace.
 * kind: 1 SetRenderState(a0, a1), 2 SetTextureStageState(a0, a1, a2),
 *       3 SetSamplerState(a0, a1, a2), 4 SetTexture(a0, a1),
 *       5/6 Set{Vertex,Pixel}ShaderConstantF(a0, a1=data, a2), 7/8 Set{Vertex,Pixel}Shader(a0). */
int fxtr_now(void); extern int g_fxtr;
int dxn_skip(int kind, uint32_t a0, uint32_t a1, uint32_t a2){
    if (g_sb_rec || g_sb_replay || g_dxrec_depth) return 0;
    if (g_fxtr && fxtr_now()) return 0;
    uint32_t a[3] = { a0, a1, a2 };
    int mid = kind == 1 ? MID_SetRenderState : kind == 2 ? MID_SetTextureStageState
            : kind == 3 ? MID_SetSamplerState : kind == 4 ? MID_SetTexture
            : kind == 5 ? MID_SetVertexShaderConstantF : kind == 6 ? MID_SetPixelShaderConstantF
            : kind == 7 ? MID_SetVertexShader : kind == 8 ? MID_SetPixelShader : 0;
    if (!mid) return 0;
    if (kind == 5 || kind == 6){                 /* a1: guest data pointer */
        uint32_t dlen = dxrec_datalen(mid, a);
        if (!dlen) return 0;
        if (!a1 || !mem_is_committed(a1) || !mem_is_committed(a1 + dlen - 1u)) return 0;
        return dxrec_call_redundant(mid, a, g_mem + a1, dlen);
    }
    return dxrec_call_redundant(mid, a, NULL, 0);
}
static int dxrec_supported(int mid){
    return MID(SetRenderState) || MID(SetSamplerState) || MID(SetTextureStageState)
        || MID(SetTexture) || MID(SetVertexShader) || MID(SetPixelShader)
        || MID(SetVertexShaderConstantF) || MID(SetPixelShaderConstantF);
}
/* From com_call, BEFORE the handler: append to every open record. */
static void dxrec_note(CPU *c, int iface, int mid, int nargs){
    /* Counter calls that are no-ops for us (texture AddRef/Release) don't break the record. */
    if ((MID(AddRef) || MID(Release)) && iface != IF_STATEBLOCK
        && iface != IF_DS && iface != IF_DSBUF) return;
    int okc = iface == IF_DEV && dxrec_supported(mid) && !dxrec_sb_recording()
              && nargs >= 2;
    uint32_t a[3] = { 0, 0, 0 };
    for (int i = 0; i < 3 && i + 1 < nargs; i++) a[i] = A(c, i+1);
    uint32_t dlen = okc ? dxrec_datalen(mid, a) : 0;
    const uint8_t *dp = NULL;
    if (dlen){
        uint32_t pd = a[1];
        if (!pd || !mem_is_committed(pd) || !mem_is_committed(pd+dlen-1u)) okc = 0;
        else dp = g_mem + pd;
    }
    int red = okc && dxrec_call_redundant(mid, a, dp, dlen);
    int lim = g_dxrec_depth < DXREC_DEPTH ? g_dxrec_depth : DXREC_DEPTH;
    for (int i = 0; i < lim; i++){
        DxRec *r = g_dxrec[i];
        if (!r->ok) continue;
        if (!okc || r->ncall >= r->maxcall || r->ndata + dlen > r->maxdata){ r->ok = 0; continue; }
        DxCall *k = &r->call[r->ncall++];
        k->mid = (int16_t)mid; k->a[0] = a[0]; k->a[1] = a[1]; k->a[2] = a[2];
        k->doff = r->ndata; k->dlen = (uint16_t)dlen; k->red = (uint8_t)red;
        if (dlen){ memcpy(r->data + r->ndata, dp, dlen); r->ndata += dlen; }
        if (!red) r->allred = 0;
    }
}
int dxrec_redundant(const DxRec *r){
    if (!r->ok || dxrec_sb_recording()) return 0;
    for (int i = 0; i < r->ncall; i++){
        const DxCall *k = &r->call[i];
        if (!dxrec_call_redundant(k->mid, k->a, r->data + k->doff, k->dlen)) return 0;
    }
    return 1;
}

/* COMSTAT=<frame>: IDirect3DDevice9 method counts, whether they come from INSIDE
 * or OUTSIDE D3DX BeginPass / CommitChanges, and the REDUNDANT ratio (same as the
 * last value on the device).  For D3DX_HLE.md Stage 2; must be used together with
 * D3DXSTAT (that wrapper keeps the BeginPass depth). */
extern int g_dx_in_pass;
static int g_cs_on = -1; static long g_cs_from = 0, g_cs_f0 = -1;
static uint64_t cs_n[MID__count][2], cs_red[MID__count][2];
typedef struct { uint32_t k0, k1; uint64_t v; } CsSlot;
#define CS_CAP (1u<<15)
static CsSlot *cs_tab;
static void comstat(CPU *c, int mid, int nargs){
    long metal_frames(void);
    long f = metal_frames();
    if (f < g_cs_from || mid <= 0) return;
    if (g_cs_f0 < 0) g_cs_f0 = f;
    if (!cs_tab) cs_tab = calloc(CS_CAP, sizeof *cs_tab);
    int in = g_dx_in_pass > 0;
    cs_n[mid][in]++;
    const char *nm = g_midname[mid];
    if (nm[0] != 'S' || nm[1] != 'e' || nm[2] != 't') return;
    int nkey = (MID(SetSamplerState) || MID(SetTextureStageState)) ? 2 : (nargs >= 3 ? 1 : 0);
    uint32_t k1 = 0; for (int i = 0; i < nkey; i++) k1 = k1*0x10001u + A(c, 1+i);
    uint64_t v = 0xcbf29ce484222325ull;
    for (int i = 1 + nkey; i < nargs; i++){ v ^= A(c,i); v *= 0x100000001b3ull; }
    uint32_t dp = 0, db = 0;
    if (MID(SetVertexShaderConstantF) || MID(SetPixelShaderConstantF)
        || MID(SetVertexShaderConstantI)){ dp = A(c,2); db = A(c,3)*16u; }
    else if (MID(SetTransform) || MID(SetMaterial)){ dp = A(c, nargs-1); db = 64u; }
    if (dp && db <= 4096u && mem_is_committed(dp) && mem_is_committed(dp+db-1u))
        for (uint32_t i = 0; i < db; i++){ v ^= MR8(dp+i); v *= 0x100000001b3ull; }
    uint32_t h = ((uint32_t)mid * 0x9E3779B1u) ^ (k1 * 0x85EBCA77u);
    for (uint32_t j = 0; j < CS_CAP; j++){
        CsSlot *t = &cs_tab[(h + j) & (CS_CAP-1)];
        if (t->k0 == 0){ t->k0 = (uint32_t)mid; t->k1 = k1; t->v = v; return; }
        if (t->k0 == (uint32_t)mid && t->k1 == k1){
            if (t->v == v) cs_red[mid][in]++;
            t->v = v; return; }
    }
}
void comstat_report(void){
    if (g_cs_f0 < 0) return;
    long metal_frames(void);
    double fr = (double)(metal_frames() - g_cs_f0); if (fr < 1) fr = 1;
    fprintf(stderr, "\n=== COMSTAT (%.0f frames): IDirect3DDevice9 calls/frame ===\n", fr);
    fprintf(stderr, "  %-28s %10s %10s %9s %10s %9s\n", "method", "BeginPass", "redundant",
            "", "outside", "redundant");
    for (int pass = 0; pass < 40; pass++){
        int best = -1; uint64_t bv = 0;
        for (int i = 1; i < MID__count; i++){
            uint64_t t = cs_n[i][0] + cs_n[i][1];
            if (t > bv){ bv = t; best = i; } }
        if (best < 0 || bv < (uint64_t)fr) break;
        int i = best;
        fprintf(stderr, "  %-28s %10.1f %9.1f%% %9s %10.1f %8.1f%%\n", g_midname[i],
            cs_n[i][1]/fr, cs_n[i][1] ? 100.0*cs_red[i][1]/cs_n[i][1] : 0.0, "",
            cs_n[i][0]/fr, cs_n[i][0] ? 100.0*cs_red[i][0]/cs_n[i][0] : 0.0);
        cs_n[i][0] = cs_n[i][1] = 0;
    }
}

/* FXTRACE=<frame>[,<frame>...]: prints every device call (arguments,
 * constant/matrix data) and the effect method markers from d3dx_hle.c in order in
 * those frames.  A behavioral reference so the native ID3DXEffect produces the
 * same device state as D3DX (D3DX_HLE.md, Stage 4). */
int g_fxtr = 0;
static uint32_t g_fxtr_fr[8]; static int g_fxtr_nf;
__attribute__((constructor)) static void fxtr_init(void){
    const char *e = getenv("FXTRACE");
    for (; e && *e && g_fxtr_nf < 8; ){
        g_fxtr_fr[g_fxtr_nf++] = (uint32_t)strtoul(e, (char**)&e, 10);
        if (*e == ',') e++; else break;
    }
    g_fxtr = g_fxtr_nf > 0;
}
int fxtr_now(void){
    for (int i = 0; i < g_fxtr_nf; i++) if (g_fxtr_fr[i] == g_present_no) return 1;
    return 0;
}
static void fxtr_floats(uint32_t p, uint32_t n){
    for (uint32_t i = 0; i < n; i++){
        float f; uint32_t u = (p && mem_is_committed(p+4u*i)) ? MR32(p+4u*i) : 0u;
        memcpy(&f, &u, 4); fprintf(stderr, "%s%g", (i&3) ? "," : (i ? " | " : " "), f);
    }
}
static void fxtr_dev(CPU *c, int iface, int mid, int nargs, const char *nm){
    if (iface != IF_DEV){
        if (iface == IF_STATEBLOCK || ((MID(AddRef) || MID(Release)) && iface == IF_TEX))
            fprintf(stderr, "D  [%d]%s %08x\n", iface, nm, A(c,0));
        return;
    }
    fprintf(stderr, "D  %s", nm);
    for (int i = 1; i < nargs && i < 7; i++) fprintf(stderr, " %x", A(c,i));
    if (MID(SetVertexShaderConstantF) || MID(SetPixelShaderConstantF)) fxtr_floats(A(c,2), A(c,3)*4u);
    else if (MID(SetTransform)) fxtr_floats(A(c,2), 16u);
    fprintf(stderr, "\n");
}

/* RBTEST=<Present#> (test): what the magazine cover capture does (0x5c3150), done
 * by hand -- the middle of the screen StretchRect'ed into a 512x512 lockable target
 * and read back; written to RBTEST_OUT (PPM).  It runs at the frame's first copy
 * from the screen (where the game's capture sits: after the scene, before the
 * post-process and the menus), or at Present if the frame has none. */
static void rbtest_run(const char *where){
    void metal_blit_rect(unsigned,unsigned,float,float,float,float);
    int metal_rt_readback(unsigned,void*,unsigned,unsigned,unsigned);
    unsigned tid = metal_create_texture_ex(512, 512, 1, 21u, 1);
    metal_blit_rect(0, tid, 0.25f, 0.25f, 0.75f, 0.75f);
    uint8_t *px = calloc(512u*512u, 4);
    int ok = px ? metal_rt_readback(tid, px, 2048u, 512u, 512u) : 0;
    unsigned long nz = 0, sum = 0;
    for (unsigned q = 0; px && q < 512u*512u*4u; q++){ if (px[q]) nz++; sum += px[q]; }
    fprintf(stderr, "  [rbtest] Present %u (%s): readback %d, %lu nonzero bytes, mean %.1f\n",
            g_present_no, where, ok, nz, px ? (double)sum/(512.0*512.0*4.0) : 0.0);
    const char *op = ENVC("RBTEST_OUT");
    FILE *f = (op && px) ? fopen(op, "wb") : NULL;
    if (f){ fprintf(f, "P6\n512 512\n255\n");
        for (unsigned q = 0; q < 512u*512u; q++){ uint8_t rgb[3] = { px[q*4+2], px[q*4+1], px[q*4] }; fwrite(rgb, 1, 3, f); }
        fclose(f); }
    free(px);
}
static int g_rbtest_done = 0;

/* TUNING GRAPH DRAG: while the ECU / turbo graph is up (overrides.c), moving the
 * mouse up or down with the left button held turns the wheel for the game, one
 * notch per 8 units of the 640x480 menu: the dot of the picked column follows the
 * pointer.  TUNEDRAG=0 turns it off.  ms: DIMOUSESTATE(2), wheel at +8, button 0 at +12. */
static void tunedrag(unsigned char *ms){
    int tuning_graph_active(void); void metal_get_cursor(int *, int *);
    static int on = -1, held = 0, acc = 0, lasty = 0;
    if (on < 0){ const char *e = ENVC("TUNEDRAG"); on = !(e && *e == '0'); }
    if (!on || !tuning_graph_active() || !(ms[12] & 0x80)){ held = 0; return; }
    int x, y; metal_get_cursor(&x, &y); (void)x;
    int y480 = g_bb_h ? (int)((long)y * 480 / (long)g_bb_h) : y;
    if (!held){ held = 1; lasty = y480; acc = 0; return; }
    acc += lasty - y480; lasty = y480;               /* up is positive, like the wheel */
    int notches = acc / 8; acc -= notches * 8;
    if (notches){ int32_t dz; memcpy(&dz, ms + 8, 4); dz += notches * 120; memcpy(ms + 8, &dz, 4); }
}
int com_call(CPU *c, uint32_t magic){
    uint32_t iface=(magic>>16)&0x1FFFu, m=magic&0xFFFFu;
    g_com_calls++;
    if (!g_sb_replay) POP32(c->eip);
    if (iface==0 || iface>=NEL(g_if) || m>=g_if[iface].n){
        fprintf(stderr,"  [COM] invalid interface/method %u:%u\n", iface, m);
        c->eax=0x80004001u; return 1;
    }
    if (iface < MSTAT_IF && m < MSTAT_M) g_mstat[iface][m]++;
    const Method *M=&g_if[iface].m[m];
    int nargs=M->nargs; uint32_t r=0; int impl=1;
    const char *nm=M->name;
    int mid;
    if (iface < MIDIF && m < MIDM){
        mid = g_midc[iface][m];
        if (!mid) mid = g_midc[iface][m] = (int16_t)mid_resolve(nm);
    } else mid = mid_resolve(nm);

    if (g_dxrec_depth) dxrec_note(c, (int)iface, mid, nargs);
    if (g_fxtr && fxtr_now()) fxtr_dev(c, (int)iface, mid, nargs, nm);
    /* ZSEQ=<frame>: the target / Z surface / Clear / Z state sequence in that frame (diagnostic) */
    if (iface==IF_DEV && ENVC("ZSEQ") && g_present_no == (uint32_t)atoi(ENVC("ZSEQ"))){
        static unsigned nd = 0;
        if (nm[0]=='D' && nm[1]=='r'){ nd++; }
        else if (MID(SetRenderTarget) || MID(SetDepthStencilSurface) || MID(Clear)
                 || (MID(SetRenderState) && (A(c,1)==7||A(c,1)==14||A(c,1)==23))
                 || MID(BeginScene) || MID(EndScene) || MID(StretchRect)){
            if (nd){ fprintf(stderr, "  [zseq]   .. %u draws\n", nd); nd = 0; }
            uint32_t s2 = A(c,2);
            fprintf(stderr, "  [zseq] %s %x %x %x %x  (rt=%u", nm, A(c,1), A(c,2), A(c,3), A(c,4), g_cur_rt);
            if ((MID(SetRenderTarget)||MID(SetDepthStencilSurface)) && s2 && mem_is_committed(s2+47))
                fprintf(stderr, " surface tid=%u %ux%u type=%u", MR32(s2+28), MR32(s2+4), MR32(s2+8), MR32(s2+40));
            if (MID(SetDepthStencilSurface) && A(c,1) && mem_is_committed(A(c,1)+47))
                fprintf(stderr, " zsurface %ux%u type=%u", MR32(A(c,1)+4), MR32(A(c,1)+8), MR32(A(c,1)+40));
            fprintf(stderr, ")\n");
        }
    }
    if (g_cs_on && iface==IF_DEV){
        if (g_cs_on < 0){ const char *e = getenv("COMSTAT"); g_cs_on = e != NULL; g_cs_from = e ? atol(e) : 0; }
        if (g_cs_on) comstat(c, mid, nargs);
    }

    /* --- CUBE TEXTURE: the same vtable slot, ONE MORE argument ---------------
     * IDirect3DCubeTexture9 methods are in the same slots as their 2D counterparts
     * but carry a FaceType parameter first.  The function bodies already told them
     * apart; but the STACK CLEANUP (nargs) hadn't been fixed, so every call leaked
     * 4 bytes.  With reflections on, when the game called GetCubeMapSurface for the
     * 6 cube faces the stack shifted by exactly 24 bytes: the caller's epilogue
     * popped from the wrong place, esi and the return address got corrupted and the
     * game crashed on the first frame (reproduced exactly with
     * g_CarEnvironmentMapEnable=1). */
    /* Look at the CHEAP method id first: is_cube_tex() reads guest MEMORY and used to
     * run on EVERY texture call -- in a run IDirect3DTexture9 AddRef+Release alone
     * are 96.8 million calls. */
    if (iface==IF_TEX &&
        (MID(GetSurfaceLevel) || MID(GetCubeMapSurface) ||
         MID(LockRect) || MID(UnlockRect) ||
         MID(AddDirtyRect)) && is_cube_tex(A(c,0)))
        nargs++;

    /* --- state block RECORDING -------------------------------------------
     * In D3D9 the device state DOESN'T CHANGE during recording; the call is only
     * written to the block. */
    if (g_sb_rec && iface==IF_DEV && !g_sb_replay && sb_recordable(mid)){
        SBlock *B = &g_sb[g_sb_rec];
        if (B->n < MAXSBREC){
            SBRec *R = &B->r[B->n++];
            R->magic = magic; R->nargs = nargs;
            for (int i2 = 0; i2 < 8; i2++) R->a[i2] = (i2 < nargs) ? A(c,i2) : 0u;
        } else g_sb_dropped++;
        c->eax = 0;
        c->esp += 4u*(uint32_t)nargs;
        return 1;
    }

    /* --- common: IUnknown --- */
    if      (MID(QueryInterface)){
        /* In DirectSound8 CreateSoundBuffer returns an IDirectSoundBuffer; the game does
         * QueryInterface to get IDirectSoundBuffer8.  When E_NOINTERFACE is returned
         * those sounds may never be used (measured: 4 buffers are created but none is
         * written to).  We return the same object - the vtable slots are shared anyway. */
        uint32_t ppv = (nargs >= 3) ? A(c,2) : 0u;
        if ((iface==IF_DS || iface==IF_DSBUF) && ppv){
            mem_commit(ppv,4); MR32(ppv) = A(c,0); r = 0;
        } else r=0x80004002u;
    }
    else if (MID(AddRef)){
        if (iface==IF_TEX || iface==IF_VB || iface==IF_IB) rc_addref(A(c,0));
        r=1; }
    else if (MID(Release)){
        /* The game silences a sound buffer with Release, NOT Stop (measured: Stop=0,
         * Release=7 in a 3-minute session).  Without stopping it, the menu music
         * looped stale content during the race and clashed with the race sound. */
        if (iface==IF_STATEBLOCK){
            /* Give the block slot back: since the game keeps creating and releasing blocks
             * throughout the session, without a pool it runs out. */
            uint32_t self1=A(c,0);
            int bi = (self1 && mem_is_committed(self1+11)) ? (int)MR32(self1+8) : 0;
            if (bi > 0 && bi < MAXSB){ g_sb[bi].used = 0; g_sb[bi].n = 0;
                                       if (g_sb_rec == bi) g_sb_rec = 0;
                                       MR32(self1+8) = 0; }
        }
        /* The data goes back only when the LAST reference is released (rc_release). */
        if (iface==IF_TEX || iface==IF_VB || iface==IF_IB) obj_release(A(c,0), iface, c->eip);
        if (iface==IF_DSBUF){
            uint32_t self0=A(c,0);
            if (self0 && mem_is_committed(self0+31) && MR32(self0+28)){
                void audio_stop(int);
                audio_stop((int)MR32(self0+28));
                MR32(self0+12)=0;
            }
        }
        r=0; }
    /* --- IDirect3D9 --- */
    else if (iface==IF_D3D9){
        if      (MID(GetAdapterCount))     r=1;
        /* The resolution list was a SINGLE row (1280x720), so the resolution couldn't be
         * changed in the game's settings menu.  We give a real list; the selected mode
         * goes to Metal via CreateDevice/Reset. */
        else if (MID(GetAdapterModeCount)) r=(uint32_t)NEL(g_modes);
        else if (MID(EnumAdapterModes)){
            uint32_t mode=A(c,3), pm=A(c,4);
            if (mode >= NEL(g_modes)) { r=0x8876086Cu; }
            else { uint32_t fmt=A(c,2); if(!fmt) fmt=22u;   /* mirror the requested format */
                   if (mode == 0 && modemap_on()) modes_refresh();   /* menu strings */
                   const uint32_t *ml = g_tbl[mode];   /* selectability: the game's table */
                   if (mode == 4 && modemap_on() && !g_row4_on)
                       ml = g_tbl[ROW_NATIVE];         /* 4K can't be selected: see modes_refresh */
                   if(pm){ mem_commit(pm,16);
                           MR32(pm)   = ml[0];
                           MR32(pm+4) = ml[1];
                           MR32(pm+8) = 60;
                           MR32(pm+12)= fmt; }
                   r=0; } }
        else if (MID(GetAdapterDisplayMode)){
            uint32_t pm=A(c,2);
            if(pm){ mem_commit(pm,16);
                    /* Desktop mode: the game eliminates rows larger than this
                     * (which is why 1600x1200 could never be selected). */
                    const uint32_t *nl = mode_label(ROW_NATIVE);
                    MR32(pm)   = nl[0] > 1600u ? nl[0] : 1600u;
                    MR32(pm+4) = nl[1] > 1200u ? nl[1] : 1200u;
                    MR32(pm+8)=60; MR32(pm+12)=22; }
            r=0; }
        else if (MID(GetAdapterIdentifier)){
            /* D3DADAPTER_IDENTIFIER9 is 1100 bytes: Driver[512], Description[512],
             * DeviceName[32], DriverVersion@1056, VendorId@1064, DeviceId@1068,
             * SubSysId@1072, Revision@1076, DeviceIdentifier@1080, WHQLLevel@1096.
             * Only the two strings used to be written; VendorId / DeviceId kept
             * whatever was in memory, the game's first-run hardware detection saw an
             * unknown card and put EVERY graphics setting at its minimum (world detail
             * 0: visible world pop-in, no crowds, reflections, shadows, fog, motion
             * blur...).  Report a top card of the game's era (GeForce 6800 Ultra) and
             * a driver of the time.  ADAPTERID=<vendor>:<device> (hex) overrides. */
            uint32_t p=A(c,3);
            if(p){ mem_commit(p,1100); memset(g_mem+p,0,1100);
                   strcpy((char*)(g_mem+p),"nv4_disp.dll");
                   strcpy((char*)(g_mem+p+512),"NVIDIA GeForce 6800 Ultra (Apple Silicon / Metal)");
                   strcpy((char*)(g_mem+p+1024),"\\\\.\\DISPLAY1");
                   uint32_t ven = 0x10DEu, dev = 0x0040u;
                   { const char *e = ENVC("ADAPTERID");
                     if (e && *e){ ven = (uint32_t)strtoul(e, NULL, 16);
                                   const char *q = strchr(e, ':'); if (q) dev = (uint32_t)strtoul(q + 1, NULL, 16); } }
                   MR32(p+1056) = (10u << 16) | 7164u;       /* 6.14.10.7164 (ForceWare 71.64) */
                   MR32(p+1060) = (6u << 16) | 14u;
                   MR32(p+1064) = ven; MR32(p+1068) = dev;
                   MR32(p+1072) = 0; MR32(p+1076) = 0xA1u;
                   MR32(p+1096) = 1u; }
            r=0; }
        else if (MID(GetDeviceCaps)){ uint32_t pc=A(c,3); if(pc) fill_caps(pc); r=0; }
        else if (MID(CreateDevice)){
            if(!g_dev) g_dev=com_new(IF_DEV,256);
            /* (this, Adapter, DeviceType, hFocusWindow, BehaviorFlags,
             *  pPresentationParameters, ppReturnedDeviceInterface)
             * pPresentationParameters WAS NEVER READ: whatever resolution the game
             * asked for, we stayed at 1280x720.  This is why changing the resolution
             * from the settings menu had no effect -- the game recreates the device
             * with the new size, and we gave back the same device and ignored the size. */
            uint32_t ppp=A(c,5);
            if (ppp && mem_is_committed(ppp+7)){
                uint32_t w4=MR32(ppp), h4=MR32(ppp+4);
                uint32_t win4 = mem_is_committed(ppp+35) ? MR32(ppp+32) : 1u;
                fprintf(stderr,"com: CreateDevice request %ux%u (%s)\n",
                        w4, h4, win4 ? "windowed" : "fullscreen");
                /* D3DPRESENT_PARAMETERS: SwapEffect@24, EnableAutoDepthStencil@36,
                 * AutoDepthStencilFormat@40, Flags@44 (2 = DISCARD_DEPTHSTENCIL).
                 * Measured (2026-10-18): swap 1 (DISCARD), Z format 75 (D24S8),
                 * flags 0 -> the implicit Z MUST BE PRESERVED across frames. */
                if (mem_is_committed(ppp+55))
                    fprintf(stderr,"com: presentation parameters: swap %u, implicit Z %u (format %u), flags 0x%x, interval 0x%x\n",
                            MR32(ppp+24), MR32(ppp+36), MR32(ppp+40), MR32(ppp+44), MR32(ppp+52));
                if (w4 > 8192u || h4 > 8192u){      /* the game's array overflow */
                    fprintf(stderr,"com: invalid size ignored, keeping %ux%u\n",
                            g_bb_w, g_bb_h);
                    w4 = h4 = 0;
                }
                /* NFSRES=<width>x<height>: override the resolution the game asks for.
                 * The game's own internal table has 6 rows, all 4:3; 1080p can't be
                 * selected from the menu (index 6+ overflows the array). */
                { uint32_t dw = w4, dh = h4;
                  res_pick(w4, h4, &dw, &dh);
                  w4 = dw; h4 = dh; }
                if (w4 && h4){
                    g_bb_w = w4; g_bb_h = h4;
                    void metal_want_size(unsigned,unsigned);
                    metal_want_size(w4,h4);              /* if the window hasn't opened */
                    if (metal_available()){ void metal_resize(unsigned,unsigned);
                                            metal_resize(w4,h4); }
                }
            }
            /* RESSCAN=1: find the game's OWN resolution table in guest memory.  The table
             * isn't stored flat in the EXE, it is built at run time; patching it is
             * the only clean way to carry 1080p everywhere in the game (aspect
             * ratio, render targets, viewport). */
            if (ENVC("RESSCAN")){
                static const uint32_t want[12] = {640,480,800,600,1024,768,
                                                  1280,960,1280,1024,1600,1200};
                struct { uint32_t lo, hi; } rng[] = {
                    {0x00400000u,0x00940000u},          /* image */
                    {0x10000000u,0x18000000u},          /* heap  */
                };
                int found = 0;
                for (unsigned q = 0; q < sizeof rng/sizeof *rng && found < 8; q++)
                    for (uint32_t a = rng[q].lo; a + 48u < rng[q].hi && found < 8; a += 4u){
                        if (!mem_is_committed(a) || !mem_is_committed(a+47u)) continue;
                        int ok = 1;
                        for (int k = 0; k < 12; k++)
                            if (MR32(a + 4u*(uint32_t)k) != want[k]){ ok = 0; break; }
                        if (ok){ fprintf(stderr,"com: [resscan] table 0x%08x\n", a); found++; }
                    }
                /* If the full array isn't found, report only the 1600x1200 pairs */
                if (!found){
                    int n2 = 0;
                    for (unsigned q = 0; q < sizeof rng/sizeof *rng && n2 < 12; q++)
                        for (uint32_t a = rng[q].lo; a + 8u < rng[q].hi && n2 < 12; a += 4u){
                            if (!mem_is_committed(a) || !mem_is_committed(a+7u)) continue;
                            if (MR32(a)==1600u && MR32(a+4u)==1200u){
                                fprintf(stderr,"com: [resscan] 1600x1200 @ 0x%08x  "
                                        "previous: %u %u %u %u\n", a,
                                        mem_is_committed(a-16u)?MR32(a-16u):0,
                                        mem_is_committed(a-12u)?MR32(a-12u):0,
                                        mem_is_committed(a-8u)?MR32(a-8u):0,
                                        mem_is_committed(a-4u)?MR32(a-4u):0);
                                n2++; }
                        }
                    if (!n2) fprintf(stderr,"com: [resscan] table not found\n");
                }
            }
            uint32_t pp=A(c,6); if(pp) MR32(pp)=g_dev;
            fprintf(stderr,"com: IDirect3DDevice9 created 0x%08x (%ux%u)\n",
                    g_dev, g_bb_w, g_bb_h);
            r=0; }
        else r=0;                       /* Check* -> D3D_OK */
    }
    /* --- IDirect3DDevice9 --- */
    else if (iface==IF_DEV){if (MID(SetRenderState)){
            uint32_t st=A(c,1), val=A(c,2);
            if (st < 256u){
                if (g_rs[st] != val){ g_rs[st] = val; g_rs_dirty = 1;
                    int seen=0; for(int k=0;k<g_rs_nval[st];k++) if(g_rs_vals[st][k]==val) seen=1;
                    if(!seen && g_rs_nval[st]<4) g_rs_vals[st][g_rs_nval[st]++]=val;
                }
                g_rs_hits[st]++;
            }
            if (g_rs_dirty){ if (metal_available()){ rs_apply(); g_rs_dirty = 0; } }
            else g_rs_skip++;
            r=0; }
        else if (MID(SetSamplerState)){
            uint32_t sm=A(c,1), ty=A(c,2), val=A(c,3);
            if (sm < 16u && ty < 20u) g_smp_raw[sm][ty] = val;
            if (ENVC("SMPLOG") && sm < 4u && ty < 20u){
                static uint32_t seen[4][20];
                if (seen[sm][ty] != val + 1u){ seen[sm][ty] = val + 1u;
                    const char *tn = ty==1?"ADDRESSU":ty==2?"ADDRESSV":ty==3?"ADDRESSW":
                                     ty==5?"MAGFILTER":ty==6?"MINFILTER":ty==7?"MIPFILTER":
                                     ty==8?"MIPMAPLODBIAS":ty==9?"MAXMIPLEVEL":
                                     ty==10?"MAXANISOTROPY":ty==11?"SRGB":"?";
                    fprintf(stderr,"  [smp] stage%u %s(%u) = %u\n", sm, tn, ty, val); } }
            r=0; }
        else if (MID(SetTextureStageState)){
            uint32_t stg=A(c,1), ty=A(c,2), val=A(c,3);
            if (stg < 8u && ty < 40u) g_tss_raw[stg][ty] = val;
            r=0; }
        else if (MID(SetTexture)){
            /* CUBELOG=1: measure whether the cube texture really gets bound.
             * There is NO cube support on the Metal side (a flat 2D is created);
             * first verify that this path is used. */
            if (ENVC("CUBELOG") && A(c,2) && is_cube_tex(A(c,2))){
                static unsigned n2 = 0;
                if (n2 < 8){ n2++;
                    fprintf(stderr,"  [cube] SetTexture stage=%u tex=0x%08x vs=%u\n",
                            A(c,1), A(c,2), g_cur_vs); }
            }
            uint32_t stg=A(c,1), to=A(c,2);
            if (stg < 8u) g_raw_tex[stg] = to;
            /* Track all stages: car paint/reflections may be in the upper stages; for
             * measurement only for now. */
            if (stg < 4u)
                g_tex_stage[stg] = (to && mem_is_committed(to+28)) ? MR32(to+28) : 0;
            if (stg==0){
                g_cur_tex = (to && mem_is_committed(to+28)) ? MR32(to+28) : 0;
                if (!to) g_st_null++;
                else if (!g_cur_tex){ g_st_zero++;
                    static int sd=0;
                    if (sd<4 && mem_is_committed(to+31)){ sd++;
                        uint32_t vt = MR32(to);
                        uint32_t mg = (vt && mem_is_committed(vt)) ? MR32(vt) : 0;
                        fprintf(stderr,"  [SetTexture] object=%08x vtable=%08x first-entry=%08x"
                                " -> interface=%u  fields: +4=%u +8=%u +14=%u(fmt) +18=%u +1c=%u\n",
                                to, vt, mg, (mg>>16)&0x1FFFu,
                                MR32(to+4), MR32(to+8), MR32(to+0x14),
                                MR32(to+0x18), MR32(to+0x1c));
                    }
                } else g_st_ok++;
            } else g_st_other++;
            r=0; }
        else if (!strncmp(nm,"Draw",4)){
            g_draw++;
            { static const char *seen[8]; static int ns=0;
              int f=0; for(int q=0;q<ns;q++) if(seen[q]==nm) f=1;   /* names from the g_if table: a pointer is enough */
              if(!f && ns<8){ seen[ns++]=nm;
                  fprintf(stderr,"  [draw-name] \"%s\"\n", nm); } }
            if (MID(DrawIndexedPrimitive)){
                /* (this, Type, BaseVertexIndex, MinIndex, NumVertices,
                 *  StartIndex, PrimitiveCount) */
                uint32_t pt=A(c,1), basev=A(c,2), starti=A(c,5), nprim=A(c,6);
                r=0;
                if (metal_available() && g_ss_vb && g_ib_obj && g_ss_stride &&
                    nprim && nprim < 200000u){
                    uint32_t vbase = MR32(g_ss_vb+8) + g_ss_off;
                    uint32_t ibase = MR32(g_ib_obj+8);
                    uint32_t ifmt  = MR32(g_ib_obj+12);
                    uint32_t isz   = (ifmt==102u) ? 4u : 2u;   /* INDEX32 : INDEX16 */
                    uint32_t ntri  = (pt==4u||pt==5u||pt==6u) ? nprim : 0u;
                    if (!ntri || !vbase || !ibase){ g_dip_skip++; }
                    else if (dip_indexed(pt, basev, starti, ntri, isz)){
                        g_dip_draws++; g_dip_tris += ntri; }
                    else {
                        if (!g_idx_scratch){ g_idx_scratch = gbuf_alloc(4096u*64u); g_gb_kept[3] += 4096u*64u; }
                        uint32_t maxv = 4095u / 3u * 3u;       /* draw_up limit */
                        uint32_t done = 0;
                        while (done < ntri && g_ss_stride <= 64u){
                            uint32_t chunk = ntri - done;
                            if (chunk*3u > maxv) chunk = maxv/3u;
                            uint32_t nv = chunk*3u, ok = 1;
                            for (uint32_t t=0; t<chunk && ok; t++){
                                uint32_t g = done + t, i0,i1,i2;
                                if (pt==4u){ i0=g*3u; i1=g*3u+1u; i2=g*3u+2u; }
                                else if (pt==5u){ i0=g; i1=g+1u; i2=g+2u;
                                                  if (g&1u){ uint32_t tmp=i1; i1=i2; i2=tmp; } }
                                else { i0=0u; i1=g+1u; i2=g+2u; }
                                uint32_t idx[3] = { i0, i1, i2 };
                                for (int k=0;k<3;k++){
                                    uint32_t ia = ibase + (starti + idx[k])*isz;
                                    if (!mem_is_committed(ia+isz-1u)){ ok=0; break; }
                                    uint32_t vi = (isz==2u) ? MR16(ia) : MR32(ia);
                                    uint32_t src = vbase + (basev + vi)*g_ss_stride;
                                    uint32_t dst = g_idx_scratch + (t*3u+(uint32_t)k)*g_ss_stride;
                                    if (!mem_is_committed(src + g_ss_stride - 1u)){ ok=0; break; }
                                    memcpy(g_mem+dst, g_mem+src, g_ss_stride);
                                }
                            }
                            if (!ok){ g_dip_skip++; break; }
                            draw_up(4u, nv, g_idx_scratch, g_ss_stride);
                            g_dip_draws++; g_dip_tris += chunk;
                            done += chunk;
                        }
                    }
                }
            }
            else if (MID(DrawPrimitiveUP)){
                /* (this, PrimitiveType, PrimitiveCount, pVertexData, VertexStride) */
                static uint64_t ptype[8], stride_hist[64];
                static int sh = 0;
                uint32_t pt=A(c,1), stride=A(c,4);
                if (pt<8) ptype[pt]++;
                if (stride < 64*4 && (stride&3)==0) stride_hist[stride/4]++;
                draw_stats_save(ptype, stride_hist);
                if (metal_available()){
                    uint32_t nprim=A(c,2), vp=A(c,3);
                    uint32_t nv = (pt==4) ? nprim*3u : nprim+2u;
                    if (vp && stride && nv >= 3 && nv < 4096u &&
                        mem_is_committed(vp) && mem_is_committed(vp + nv*stride - 1))
                        draw_up(pt, nv, vp, stride);
                }
                if (dd_on()){
                    static uint32_t last_decl = 0xFFFFFFFFu; static int nd = 0;
                    uint32_t nprim=A(c,2), vp=A(c,3);
                    uint32_t nv = (pt==4) ? nprim*3u : nprim+2u;
                    if (g_cur_decl != last_decl){ last_decl = g_cur_decl; dd_decl(g_cur_decl); }
                    if (nd++ < 40){
                        fprintf(stderr,"   #%-3d type=%u verts=%-4u stride=%u tex=%u\n",
                                nd, pt, nv, stride, g_cur_tex);
                        for (uint32_t v=0; v<nv && v<4; v++){
                            uint32_t o = vp + v*stride;
                            if (!mem_is_committed(o+31)) break;
                            fprintf(stderr,"        [%u] ", v);
                            for (int w2=0; w2<8; w2++)
                                fprintf(stderr,"%s%.4g", w2?" ":"", (double)MRF32(o+4*w2));
                            fprintf(stderr,"  | ham:");
                            for (int w2=0; w2<8; w2++) fprintf(stderr," %08x", MR32(o+4*w2));
                            fprintf(stderr,"\n");
                        }
                    }
                }
                if (VB() && sh < 6){
                    sh++;
                    uint32_t d  = g_cur_decl;
                    uint32_t el = (d && mem_is_committed(d+4)) ? MR32(d+4) : 0;
                    fprintf(stderr,"  [draw] type=%u count=%u stride=%u decl=0x%08x fvf=0x%08x elements=0x%08x\n",
                            pt, A(c,2), stride, d, g_cur_fvf, el);
                    /* dump the first vertices' data */
                    { uint32_t vp = A(c,3), nprim = A(c,2);
                      uint32_t nv = (pt==6||pt==5) ? nprim+2u : nprim*3u;
                      if (nv > 6) nv = 6;
                      for (uint32_t v=0; v<nv; v++){
                        uint32_t o = vp + v*stride;
                        if (!mem_is_committed(o+24)) break;
                        fprintf(stderr,"        v%u: pos=(%.2f, %.2f, %.2f) color=0x%08x uv=(%.3f, %.3f)\n",
                                v, MRF32(o), MRF32(o+4), MRF32(o+8), MR32(o+12),
                                MRF32(o+16), MRF32(o+20));
                      } }
                    for (uint32_t k = 0; el && k < 16; k++){
                        uint32_t e = el + 8u*k;
                        if (!mem_is_committed(e+7)) break;
                        uint32_t st=MR16(e), of=MR16(e+2), ty=MR8(e+4), us=MR8(e+6);
                        if (st == 0xFFu && ty == 17u) break;
                        fprintf(stderr,"        offset=%-3u type=%-2u usage=%u\n", of, ty, us);
                    }
                }
            }
            r=0; }
        else if (MID(SetVertexShader)){
            uint32_t h=A(c,1); g_raw_vs = h;
            g_cur_vs = (h && mem_is_committed(h+15)) ? MR32(h+12) : 0;
            if (g_cur_vs >= MAXVS) g_cur_vs = 0;
            r=0; }
        else if (MID(SetPixelShader)){
            uint32_t sh=A(c,1); g_raw_ps = sh;
            g_cur_ps_id = (sh && mem_is_committed(sh+15)) ? (int)MR32(sh+12) : 0;
            r=0; }
        else if (MID(SetVertexShaderConstantF)){
            /* (this, StartRegister, pConstantData, Vector4fCount) */
            uint32_t sr=A(c,1), pd=A(c,2), n=A(c,3);
            static int sh=0;
            g_sc[g_scn & 7].sr = sr; g_sc[g_scn & 7].pd = pd;
            g_sc[g_scn & 7].n = n; g_scn++;
            for (uint32_t k=0; k<n && sr+k<256; k++){
                uint32_t o=pd+16u*k;
                if (!mem_is_committed(o+15)) break;
                memcpy(g_vsk.f[sr+k], g_mem+o, 16);
            }
            /* VSKDUMP: dump the SURROUNDINGS of pd.  If the matrix is shifted by 2 vectors,
             * the right rows should be at pd-32. */
            /* WATCHVSK: who writes to this buffer?  Right after the upload make the page
             * read-only; the writer of the next frame gets caught. */
            /* PDLIST: list the distinct pd values and whether the matrix is broken.  For
             * comparing good/broken blocks. */
            if (ENVC("PDLIST") && sr==0 && n>=4 && pd && mem_is_committed(pd+63)){
                static uint32_t seen[32]; static int ns=0;
                int f=0; for (int q=0;q<ns;q++) if (seen[q]==pd) f=1;
                if (!f && ns<32){ seen[ns++]=pd;
                    float m00=MRF32(pd), m11=MRF32(pd+20), m33=MRF32(pd+60);
                    int bad = (m00==0.f && m11==0.f && m33==0.f);
                    fprintf(stderr,"  [pd] %08x n=%u %s  m00=%g m11=%g m33=%g\n",
                            pd, n, bad?"BROKEN":"good  ", m00, m11, m33);
                }
            }
            /* OWNERSCAN: find the structure on the heap that points to pd, look for a name in it. */
            if (ENVC("OWNERSCAN") && sr==0 && n==5 && pd){
                static int os_=0, oseen=0;
                oseen++;
                if (oseen > 400 && !os_){ os_=1;
                    fprintf(stderr,"  [owner] structures pointing to pd=%08x:\n", pd);
                    int found = 0;
                    for (uint64_t A2 = 0x10000000ull; A2 < 0x30000000ull && found < 12; A2 += 4){
                        uint32_t a3 = (uint32_t)A2;
                        if ((a3 & 0x3FFFu) == 0 && !mem_is_committed(a3)) { A2 += 0x3FFCull; continue; }
                        if (!mem_is_committed(a3+3)) continue;
                        if (MR32(a3) != pd) continue;
                        found++;
                        fprintf(stderr,"    structure field %08x:\n", a3);
                        uint32_t base = (a3 > 0x80u) ? a3 - 0x80u : 0x10000u;
                        for (uint32_t q = base; q <= a3 + 0x80u; q += 4u){
                            if (!mem_is_committed(q+3)) continue;
                            uint32_t P = MR32(q);
                            if (P < 0x10000u || !mem_is_committed(P+48u)) continue;
                            const char *t = (const char*)(g_mem+P);
                            int ok = 1, L = 0;
                            while (L < 44 && t[L]){
                                unsigned char ch = (unsigned char)t[L];
                                if (ch < 0x20u || ch > 0x7Eu){ ok = 0; break; }
                                L++; }
                            if (ok && L >= 3 && L < 44 && t[L] == 0)
                                fprintf(stderr,"        %08x (pd%+d) -> \"%s\"\n",
                                        q, (int)q - (int)a3, t);
                        }
                    }
                    if (!found) fprintf(stderr,"    (no pointer to it found)\n");
                }
            }
            /* PARAMNAME: scan the pointers on the stack and find the NAME of the D3DX effect
             * parameter.  To learn which parameter is never set. */
            if (ENVC("PARAMNAME") && sr==0 && n==5 && pd){
                static int pn=0, pseen=0;
                pseen++;
                if (pseen > 400 && pn < 1){ pn=1;
                    fprintf(stderr,"  [param] stack scan for pd=%08x:\n", pd);
                    for (uint32_t off=0; off<0x120u; off+=4u){
                        uint32_t sp2 = c->esp + off;
                        if (!mem_is_committed(sp2+3)) continue;
                        uint32_t V = MR32(sp2);
                        if (V < 0x10000u || !mem_is_committed(V+0x67u)) continue;
                        for (uint32_t q=0; q<0x64u; q+=4u){
                            uint32_t P = MR32(V+q);
                            if (P < 0x10000u || !mem_is_committed(P+40u)) continue;
                            const char *t = (const char*)(g_mem+P);
                            int ok=1, L=0;
                            while (L<40 && t[L]){
                                unsigned char ch=(unsigned char)t[L];
                                if (ch<0x20u || ch>0x7Eu){ ok=0; break; }
                                L++; }
                            if (ok && L>=4 && L<40 && t[L]==0)
                                fprintf(stderr,"      stack+%03x -> structure %08x +%02x -> \"%s\"\n",
                                        off, V, q, t);
                        }
                    }
                }
            }
            /* VSKBT: the guest caller chain that uploads the matrix */
            if (ENVC("VSKBT") && sr==0 && n==5 && pd){
                static int bt=0, bseen=0;
                bseen++;
                if (bseen > atoi(ENVC("VSKBT")) && bt<2){ bt++;
                    fprintf(stderr,"  [vsk-caller] upload #%d pd=%08x\n", bseen, pd);
                    guest_backtrace(c, 14); }
            }
            if (ENVC("WATCHVSK") && sr==0 && n==5 && pd){
                static int wv=0, wseen=0;
                wseen++;
                if (wseen > atoi(ENVC("WATCHVSK")) && !wv){ wv=1; mprot_arm(pd); }
            }
            if (ENVC("VSKDUMP") && sr==0 && n==5 && pd){
                static int vd=0, vseen=0;
                vseen++;
                if (vseen>500 && vd<3){ vd++;
                    fprintf(stderr,"  [vsk] pd=%08x n=%u surroundings:\n", pd, n);
                    for (int q=-4; q<8; q++){
                        uint32_t o2 = pd + (uint32_t)(16*q);
                        if (!mem_is_committed(o2+15)) { fprintf(stderr,"      %+3d: (none)\n", q); continue; }
                        fprintf(stderr,"      %+3d (%08x): %14.7g %14.7g %14.7g %14.7g\n", q, o2,
                                MRF32(o2), MRF32(o2+4), MRF32(o2+8), MRF32(o2+12));
                    }
                }
            }
            if (ENVC("CONSTDUMP") && sr==0 && n==5 && pd && mem_is_committed(pd+79)){
                static int cd=0;
                if (cd<2){ cd++;
                    for (uint32_t bp2=(pd & ~15u); bp2>=0x10000000u; bp2-=16u){
                        if (!mem_is_committed(bp2+4)) break;
                        if (MR32(bp2+4)==0xA110C8EDu){
                            fprintf(stderr,"  [block] pd=%08x  block start=%08x size=%u"
                                    "  -> pd offset=%d\n",
                                    pd, bp2+16u, MR32(bp2), (int)(pd-(bp2+16u)));
                            break; } }
                    fprintf(stderr,"  [constants] pd=%08x n=%u  (including 2 vec4 before the block)\n", pd, n);
                    for (int rw=-2; rw<(int)n && rw<11; rw++){
                        uint32_t a2 = pd + (uint32_t)(rw*16);
                        if (!mem_is_committed(a2+15)) continue;
                        fprintf(stderr,"      c%-3d %11.5g %11.5g %11.5g %11.5g\n", rw,
                                MRF32(a2), MRF32(a2+4), MRF32(a2+8), MRF32(a2+12));
                    }
                }
            }
            /* is the matrix healthy? for c0..c3: a last row like (0,0,0,1) or at least one
             * element of reasonable magnitude */
            if (sr==0 && n>=4 && pd && mem_is_committed(pd+63)){
                static uint64_t mat_good=0, mat_bad=0, mat_zero=0; static int mat_shown=0;
                int nz=0, n_big=0;
                for (int q=0;q<16;q++){
                    float fv = MRF32(pd+4u*q);
                    if (fv != 0.f) nz++;
                    if (fabsf(fv) > 1e6f) n_big++;
                }
                if (nz==0) mat_zero++;
                else if (n_big){ mat_bad++;
                    if (ENVC("WATCHALLOC") && !g_wc_armed){
                        g_wc_armed = 1;
                        void watch_dump(const char*, int);
                        void watch_arm(uint32_t);
                        watch_dump("constant block writer", 30);
                        watch_arm(0);
                    }
                    if (0){
                        if (++g_wc_after == 40){
                            void watch_dump(const char*, int);
                            void watch_arm(uint32_t);
                            watch_dump("constant block writer", 24);
                            watch_arm(0);              /* turn the watch off */
                            g_wc_armed = 0;
                        }
                    }
                    /* c->eip = right after the guest instruction that called SetVertexShaderConstantF
                     * (POP32'd at com_call entry) */
                    static uint32_t caller[8]; static int ncall=0;
                    int seen=0;
                    for (int q=0;q<ncall;q++) if (caller[q]==c->eip) seen=1;
                    if (!seen && ncall<8){ caller[ncall++]=c->eip;
                        fprintf(stderr,"  [matrix-BROKEN] pd=%08x  caller return=0x%08x  n=%u\n",
                                pd, c->eip, n);
                        /* The caller uses an ebp-based frame and ebp is live:
                         * [ebp-8] = the parameter descriptor (see 0x00669d87). */
                        /* which heap block is pd in? (header: +0 size, +4 0xA110C8ED) */
                        for (uint32_t bp = (pd & ~15u); bp >= 0x10000000u; bp -= 16u){
                            if (!mem_is_committed(bp+4)) break;
                            if (MR32(bp+4) == 0xA110C8EDu){
                                uint32_t bsz = MR32(bp);
                                fprintf(stderr,"      pd block: start=%08x size=%u  pd offset=%d\n",
                                        bp+16u, bsz, (int)(pd - (bp+16u)));
                                fprintf(stderr,"      first 64 bytes of the block:");
                                for (int q=0;q<64;q++){ if(q%32==0) fprintf(stderr,"\n        ");
                                    fprintf(stderr,"%02x ", MR8(bp+16u+q)); }
                                fprintf(stderr,"\n        ascii: ");
                                for (int q=0;q<64;q++){ uint8_t ch=MR8(bp+16u+q);
                                    fputc((ch>=32&&ch<127)?ch:'.', stderr); }
                                fprintf(stderr,"\n");
                                break;
                            }
                        }
                        /* Set a watchpoint to catch the next write: the block is refilled every frame,
                         * so setting it now is enough. */
                        if (ENVC("WATCHCONST")){
                            void watch_arm(uint32_t);
                            watch_arm(pd + 16u);
                            g_wc_armed = 1;
                        }
                        /* Search memory for the semantic hash (h=h*33+c): do the game side and the
                         * effect side hold the same value? */
                        /* Find the "WorldViewProj" string and the structures pointing to it: to see the
                         * type field of the parameter descriptor directly. */
                        if (ENVC("FINDPARAM")){
                            static const char *needle = "WorldViewProj";
                            size_t nl = strlen(needle);
                            uint32_t found[8]; int nf = 0;
                            for (uint32_t a = 0x10000000u; a < 0x10400000u && nf < 8; a += 4){
                                if (!mem_is_committed(a + 32)) { a += 0x3FFCu; continue; }
                                if (!memcmp(g_mem + a, needle, nl) && MR8(a+nl) == 0)
                                    found[nf++] = a;
                            }
                            fprintf(stderr,"      'WorldViewProj' found: in %d places\n", nf);
                            for (int k = 0; k < nf; k++){
                                fprintf(stderr,"        @%08x\n", found[k]);
                                /* search for dwords pointing to this address */
                                int np = 0;
                                for (uint32_t a = 0x10000000u; a < 0x10400000u && np < 6; a += 4){
                                    if (!mem_is_committed(a + 4)) { a += 0x3FFCu; continue; }
                                    if (MR32(a) != found[k]) continue;
                                    np++;
                                    /* which field of a structure is the pointer in? try assuming +8 */
                                    for (int off = 0; off <= 0x10; off += 4){
                                        uint32_t base = a - (uint32_t)off;
                                        if (!mem_is_committed(base + 0x5F)) continue;
                                        fprintf(stderr,"          pointer@%08x (structure start %08x, field +%02x)"
                                                "  +54=%u +56=%u +04=%u +00=%u\n",
                                                a, base, off, MR16(base+0x54), MR16(base+0x56),
                                                MR32(base+4), MR32(base));
                                    }
                                }
                            }
                        }
                        uint32_t desc = mem_is_committed(c->ebp) ? MR32(c->ebp - 8u) : 0;
                        fprintf(stderr,"      ebp=%08x descriptor=%08x\n", c->ebp, desc);
                        if (desc && mem_is_committed(desc+0x5F)){
                            for (int rw2=0; rw2<0x60; rw2+=16){
                                fprintf(stderr,"        +%02x:", rw2);
                                for (int q=0;q<16;q++) fprintf(stderr," %02x", MR8(desc+rw2+q));
                                fprintf(stderr,"  ");
                                for (int q=0;q<16;q++){ uint8_t ch=MR8(desc+rw2+q);
                                    fputc((ch>=32&&ch<127)?ch:'.', stderr); }
                                fprintf(stderr,"\n");
                            }
                            fprintf(stderr,"        reg=%u say=%u bayrak30=%02x\n",
                                    MR16(desc+0x54), MR16(desc+0x56), MR8(desc+0x30));
                            for (int f=0; f<0x60; f+=4){
                                uint32_t v = MR32(desc+f);
                                if (v > 0x10000000u && v < 0x30000000u && mem_is_committed(v+15)){
                                    fprintf(stderr,"        [+%02x]->%08x (pd diff %+d):", f, v, (int)(v - pd));
                                    for (int q=0;q<16;q++) fprintf(stderr," %02x", MR8(v+q));
                                    fprintf(stderr,"  ");
                                    for (int q=0;q<16;q++){ uint8_t ch=MR8(v+q);
                                        fputc((ch>=32&&ch<127)?ch:'.', stderr); }
                                    fprintf(stderr,"\n");
                                }
                            }
                        }
                        for (uint32_t rw=0; rw<n && rw<13; rw++){
                            if (!mem_is_committed(pd+rw*16+15)) break;
                            fprintf(stderr,"      c%-2u %12.4g %12.4g %12.4g %12.4g\n", rw,
                                MRF32(pd+rw*16), MRF32(pd+rw*16+4),
                                MRF32(pd+rw*16+8), MRF32(pd+rw*16+12));
                        }
                        /* BEFORE the buffer: the game sometimes gives a pointer shifted backwards; the
                         * real data may be there */
                        if (pd >= 64 && mem_is_committed(pd-64)){
                            fprintf(stderr,"      --- 4 vec4 before pd ---\n");
                            for (int rw=-4; rw<0; rw++)
                                fprintf(stderr,"      c%-2d %12.4g %12.4g %12.4g %12.4g\n", rw,
                                    MRF32(pd+rw*16), MRF32(pd+rw*16+4),
                                    MRF32(pd+rw*16+8), MRF32(pd+rw*16+12));
                        }
                    }
                    static int bt=0;
                    if (ENVC("MATBT") && bt<2){ bt++;
                        fprintf(stderr,"  [matrix-BROKEN] pd=%08x, call stack:\n", pd);
                        guest_backtrace(c, 14); } }
                else { mat_good++;
                       if (mat_shown<3){ mat_shown++;
                         fprintf(stderr,"  [matrix-GOOD] pd=%08x\n", pd);
                         for (int rw=0;rw<4;rw++)
                           fprintf(stderr,"      %9.4f %9.4f %9.4f %9.4f\n",
                             MRF32(pd+rw*16),MRF32(pd+rw*16+4),
                             MRF32(pd+rw*16+8),MRF32(pd+rw*16+12)); } }
                if (((mat_good+mat_bad+mat_zero) % 20000)==0)
                    fprintf(stderr,"  [matrix] good=%llu broken=%llu zero=%llu\n",
                            (unsigned long long)mat_good,(unsigned long long)mat_bad,
                            (unsigned long long)mat_zero);
            }
            if (ENVC("VSCDUMP") && sh<6 && sr==0 && n>=4 && pd && mem_is_committed(pd+63)){
                sh++;
                fprintf(stderr,"  [vsconst] esp=%08x pd=%08x sr=%u n=%u  stack:",
                        c->esp, pd, sr, n);
                for (int q=0;q<6;q++) fprintf(stderr," %08x", MR32(c->esp+4u*q));
                fprintf(stderr,"\n            raw@pd:");
                for (int q=0;q<16;q++){ if(q%8==0) fprintf(stderr,"\n              ");
                    fprintf(stderr," %08x", MR32(pd+4u*q)); }
                fprintf(stderr,"\n");
            }
            if (VB() && sh<8 && sr==0 && n>=4 && pd && mem_is_committed(pd+63)){
                sh++;
                fprintf(stderr,"  [vsconst] c0..c3 (StartRegister=%u, %u vectors):\n", sr, n);
                for (uint32_t rw=0; rw<4; rw++)
                    fprintf(stderr,"     [%8.3f %8.3f %8.3f %8.3f]\n",
                            MRF32(pd+rw*16), MRF32(pd+rw*16+4),
                            MRF32(pd+rw*16+8), MRF32(pd+rw*16+12));
            }
            r=0; }
        else if (MID(SetStreamSource)){
            /* (this, StreamNumber, pStreamData, OffsetInBytes, Stride) */
            if (A(c,1)==0u){ g_ss_vb=A(c,2); g_ss_off=A(c,3); g_ss_stride=A(c,4); }
            r=0; }
        else if (MID(SetIndices)){ g_ib_obj=A(c,1); r=0; }
        else if (MID(SetPixelShaderConstantF)){
            /* (this, StartRegister, pConstantData, Vector4fCount) */
            uint32_t sr2=A(c,1), pd2=A(c,2), n2=A(c,3);
            if (sr2+n2 > g_psc_max) g_psc_max = sr2+n2;
            if (sr2+n2 > 32u) g_psc_drop++;
            for (uint32_t q4=0; q4<n2 && sr2+q4<32u; q4++){
                uint32_t off = pd2 + 16u*q4;
                if (!pd2 || !mem_is_committed(off+15)) break;
                for (int c4=0;c4<4;c4++) g_psc[sr2+q4][c4] = MRF32(off+4u*(uint32_t)c4);
            }
            r=0; }
        
        else if      (MID(TestCooperativeLevel)){
            g_tcl_n++;
            /* Device loss (fullscreen transition): as in D3D9 the game releases its
             * D3DPOOL_DEFAULT resources and calls Reset. */
            r = atomic_load(&g_lost) ? 0x88760869u /* D3DERR_DEVICENOTRESET */ : 0u;
            if (r && g_tcl_lost_n++ < 4) fprintf(stderr,"com: TestCooperativeLevel -> DEVICENOTRESET\n"); }
        else if (MID(GetAvailableTextureMem)) r=256u*1024u*1024u;
        else if (MID(GetDeviceCaps)){ uint32_t pc=A(c,1); if(pc) fill_caps(pc); r=0; }
        else if (MID(GetDirect3D)){ uint32_t p=A(c,1); if(p) MR32(p)=g_d3d9; r=0; }
        else if (MID(GetDisplayMode)){
            /* Must be a VALID size; when a fixed 1280x720 was returned the game's settings
             * screen couldn't find the selected row in the list.  For the same reason,
             * if the device is larger than the game thinks (Retina scale, NFSRES) we
             * report the size the game ITSELF chose, not the real size (e.g.
             * 2448x1836): the real size isn't in that table. */
            uint32_t pm=A(c,2);
            uint32_t dmw = g_req_w ? g_req_w : g_bb_w;
            uint32_t dmh = g_req_h ? g_req_h : g_bb_h;

            if(pm){mem_commit(pm,16);MR32(pm)=dmw;MR32(pm+4)=dmh;
                   MR32(pm+8)=60;MR32(pm+12)=22;} r=0; }
        else if (MID(Clear)){
            /* Clear(this,Count,pRects,Flags,Color,Z,Stencil) -> Color = arg4 */
            /* Flags: 1=TARGET 2=ZBUFFER 4=STENCIL; Z = arg5 (float) */
            if (metal_available()){
                void metal_clear_target_ex(unsigned, unsigned, float);
                uint32_t zb = A(c,5); float zf; memcpy(&zf, &zb, 4);
                metal_set_clear_color(A(c,4)); metal_clear_target_ex(A(c,4), A(c,3), zf); }
            r=0; }
        else if (MID(BeginScene)){ if (metal_available()) metal_begin_frame(); r=0; }
        else if (MID(EndScene)) r=0;
        else if (MID(Present)){ g_present_no++;
            /* STALL=<present>:<ms> (test): freeze the game thread once, as a slow disk or a
             * long frame would -- e.g. to check the sound stale-data guard (audio.m). */
            { static long sa = -2; static unsigned sms = 0;
              if (sa == -2){ const char *e = ENVC("STALL"); sa = -1;
                  if (e && *e){ sa = atol(e); const char *q = strchr(e, ':'); sms = q ? (unsigned)atoi(q + 1) : 1000u; } }
              if (sa >= 0 && (long)g_present_no == sa){ fprintf(stderr, "STALL: present %ld, %u ms\n", sa, sms); usleep(sms * 1000u); } }
            { extern void guest_cpu_mark(void); guest_cpu_mark(); }
            /* FETICK: the front-end tick [0x8651a8] should increase at 60 Hz, but the game's
             * frame-delta measurement always gives 1/1800 s, so the tick never
             * increases -> animations and timed screen transitions freeze.
             * Since our virtual clock advances exactly 1/60 s per frame, +1 tick per
             * frame gives the right speed. */
            if (ENVC("FETICK") && mem_is_committed(GA(0x008651a8u)))
                MR32(GA(0x008651a8u)) = MR32(GA(0x008651a8u)) + 1u;
            /* FIXFRAMETIME: the game's TARGET FRAME TIME [0x865558] is never set (its
             * setters sub_00576cc0/ce0 aren't called) -> the frame delta dt stays at
             * 1/1800, the front-end tick [0x8651a8] never increases, animations and
             * timed screen transitions freeze.
             * The game's own default [0x7fb710] = 1/120. */
            /* LIMFREE=<frame> (MEASUREMENT only, with VCLOCK=8333 + NOVSYNC=1): after that
             * frame the target frame time ~0 -> the game's 120 fps limiter doesn't
             * wait, fps = GPU/CPU capacity; game time is fixed per frame by the
             * virtual clock.  DON'T USE in menus: front-end timing breaks, AUTOSEQ drifts. */
            { const char *lf = ENVC("LIMFREE");
              if (lf && g_present_no >= (unsigned)atoi(lf) && g_present_no >= 2u && mem_is_committed(GA(0x00865558u)))
                  MRF32(GA(0x00865558u)) = 0.0002f; }
            { const char *ff = ENVC("FIXFRAMETIME");
              static int done_ft = 0;
              if (ff && *ff && ff[0] != '0' && !done_ft && g_present_no >= 2u){
                  done_ft = 1;
                  float v = (float)atof(ff);
                  if (!(v > 0.f)) v = mem_is_committed(GA(0x007fb710u))
                                      ? MRF32(GA(0x007fb710u)) : (1.f/120.f);
                  if (mem_is_committed(GA(0x00865558u))){
                      MRF32(GA(0x00865558u)) = v;
                      fprintf(stderr,"  [frame-time] [0x865558] set to %g\n",
                              (double)v);
                  }
              } }
            /* DATADIFF: which global variables change between two frames?  A frozen
             * counter/clock shows up directly. */
            { const char *dd2 = ENVC("DATADIFF");
              static uint8_t *snap = 0; static int taken = 0;
              const uint32_t DLO = 0x007E0000u, DHI = 0x008A0000u;
              if (dd2 && *dd2){
                  uint32_t at = (uint32_t)atoi(dd2);
                  if (!taken && g_present_no == at){
                      snap = malloc(DHI - DLO);
                      if (snap){ memcpy(snap, g_mem + DLO, DHI - DLO); taken = 1;
                                 fprintf(stderr,"  [diff] frame %u: snapshot taken\n", at); }
                  } else if (taken == 1 && g_present_no == at + 600u){
                      taken = 2; int n = 0;
                      fprintf(stderr,"  [diff] changed between frames %u and %u:\n", at, at+600u);
                      for (uint32_t o = 0; o + 4 <= DHI - DLO; o += 4){
                          uint32_t oldv, newv;
                          memcpy(&oldv, snap + o, 4);
                          memcpy(&newv, g_mem + DLO + o, 4);
                          if (oldv == newv) continue;
                          if (++n > 40) break;
                          float fo, fn2; memcpy(&fo,&oldv,4); memcpy(&fn2,&newv,4);
                          fprintf(stderr,"     0x%08x: %10u -> %-10u  (%g -> %g)\n",
                                  DLO + o, oldv, newv, (double)fo, (double)fn2);
                      }
                      fprintf(stderr,"  [diff] total changed dwords (first 40 shown)\n");
                  }
              } }
            /* FORCESWITCH: we got stuck on the splash.  We trigger the transition by hand and
             * measure whether the next screen (profile / main menu) loads.
             * DIAGNOSTIC purpose. */
            { const char *fs2 = ENVC("FORCESWITCH");
              static int fired = 0;
              extern uint32_t g_fe_mgr;
              if (fs2 && *fs2 && !fired && g_present_no >= 2500u && g_fe_mgr){
                  fired = 1;
                  uint32_t from = GA(0x0078ea60u);              /* UG_LS_Splash.fng */
                  uint32_t to   = (atoi(fs2) == 2) ? GA(0x0078f530u)   /* UI_Main.fng */
                                                   : GA(0x0078ea4cu);  /* MC_Background.fng */
                  fprintf(stderr,"  [force] SwitchFEngPackages(manager=%08x, \"%s\" -> \"%s\")\n",
                          g_fe_mgr, (const char*)(g_mem+from), (const char*)(g_mem+to));
                  { CPU save = *c;
                    if (atoi(fs2) == 3){
                        /* sub_004c4f50(current_package_name, 0): advances the boot flow; finds the target
                         * in its own list and transitions by LOADING it. */
                        PUSH32(0u); PUSH32(from);
                        PUSH32(0xDEADBEEFu);
                        dispatch_indirect(c, GA(0x004c4f50u));
                    } else {
                        PUSH32(to); PUSH32(from);
                        PUSH32(0xDEADBEEFu);
                        c->ecx = g_fe_mgr;
                        dispatch_indirect(c, GA(0x0051cbb0u));
                    }
                    *c = save; }
                  fprintf(stderr,"  [force] call returned\n");
              } }
            /* BOOTFLOW: walk the boot flow list (0x836488).
             * Shows directly what is being waited for on the splash. */
            if (ENVC("BOOTFLOW") && (g_present_no % 900u) == 5u){
                uint32_t head = mem_is_committed(GA(0x00836488u)) ? MR32(GA(0x00836488u)) : 0;
                { /* Front-end clock chain:
                   *   [0x865194] = frame delta (dt, seconds)
                   *   [0x865140] = accumulator remainder
                   *   [0x8651a8] = front-end tick (rounded from dt to 60 Hz)
                   * Animations and timed screen transitions depend on this tick. */
                  fprintf(stderr,"  [fe-clock] frame %u: dt(865194)=%g  remainder(865140)=%g"
                          "  TICK(8651a8)=%d  frame(865148)=%d  target(865558)=%g\n", g_present_no,
                          mem_is_committed(GA(0x00865194u))?(double)MRF32(GA(0x00865194u)):-1.0,
                          mem_is_committed(GA(0x00865140u))?(double)MRF32(GA(0x00865140u)):-1.0,
                          mem_is_committed(GA(0x008651a8u))?(int)MR32(GA(0x008651a8u)):-1,
                          mem_is_committed(GA(0x00865148u))?(int)MR32(GA(0x00865148u)):-1,
                          mem_is_committed(GA(0x00865558u))?(double)MRF32(GA(0x00865558u)):-1.0);
                  { uint64_t host_us(void);
                    uint64_t t1 = host_us(); volatile int sp=0;
                    for (int q=0;q<200000;q++) sp+=q;
                    uint64_t t2 = host_us();
                    { /* __ftol (sub_0075bed4) correctness: call it with ST(0)=value and compare the
                       * result.  The only suspect primitive in the frame-delta chain. */
                      static int ft_done = 0;
                      if (!ft_done){ ft_done = 1;
                        const double tv[5] = { 90.5, 15.5, 1.5, 0.5, 30000.5 };
                        for (int q = 0; q < 5; q++){
                            CPU sv = *c;
                            c->top = (c->top - 1) & 7; c->st[c->top] = tv[q];
                            PUSH32(0xDEADBEEFu);
                            dispatch_indirect(c, GA(0x0075bed4u));
                            fprintf(stderr,"           __ftol(%g) = %d %s\n",
                                    tv[q], (int)c->eax,
                                    ((int)c->eax == (int)tv[q]) ? "" : "  <-- WRONG");
                            *c = sv;
                        } } }
                    fprintf(stderr,"           MID esi(86513c)=%d  remainder after int2(865144)=%g\n",
                            mem_is_committed(GA(0x0086513cu))?(int)MR32(GA(0x0086513cu)):-1,
                            mem_is_committed(GA(0x00865144u))?(double)MRF32(GA(0x00865144u)):-1.0);
                    { /* sub_0057eae0 writes the measured frame time every frame to
                       * 0x8647a8 + (frame%100)*16 + 4. */
                      uint32_t fr = mem_is_committed(GA(0x00865148u))?MR32(GA(0x00865148u)):0;
                      fprintf(stderr,"           MEASURED frame time (last 4 frames):");
                      for (int q=0;q<4;q++){
                          uint32_t idx = (fr - (uint32_t)q) % 100u;
                          uint32_t a4 = GA(0x008647a8u) + idx*16u + 4u;
                          fprintf(stderr,"  %g", mem_is_committed(a4)?(double)MRF32(a4):-1.0);
                      }
                      fprintf(stderr,"\n"); }
                    fprintf(stderr,"           ms/tik(828028)=%g  kaydirma(8284e4)=%u\n",
                            mem_is_committed(GA(0x00828028u))?(double)MRF32(GA(0x00828028u)):-1.0,
                            mem_is_committed(GA(0x008284e4u))?MR32(GA(0x008284e4u)):0);
                    fprintf(stderr,"           QPC bolen(8284e4)=%u  olcek(78435c)=%g"
                            "  host_us within frame: %llu -> %llu (diff %llu us)\n",
                            mem_is_committed(GA(0x008284e4u))?MR32(GA(0x008284e4u)):0,
                            mem_is_committed(GA(0x0078435cu))?(double)MRF32(GA(0x0078435cu)):0.0,
                            (unsigned long long)t1,(unsigned long long)t2,
                            (unsigned long long)(t2-t1)); }
                  fprintf(stderr,"           girdi(7fb714)=%g  mod(7fb700)=%u  kalan2(865144)=%g\n",
                          mem_is_committed(GA(0x007fb714u))?(double)MRF32(GA(0x007fb714u)):-1.0,
                          mem_is_committed(GA(0x007fb700u))?MR32(GA(0x007fb700u)):0,
                          mem_is_committed(GA(0x00865144u))?(double)MRF32(GA(0x00865144u)):-1.0); }
                { /* Game clock: sub_0057a1b0 => [0x865134] * constant.
                   * If this counter doesn't increase, front-end animations stay frozen. */
                  fprintf(stderr,"  [gameclock] tick(865134)=%d  (865138)=%08x  sec(7fb700)=%u"
                          "  scale=%g/%g  fe_t(864720)=%g\n",
                          mem_is_committed(GA(0x00865134u))?(int)MR32(GA(0x00865134u)):-1,
                          mem_is_committed(GA(0x00865138u))?MR32(GA(0x00865138u)):0,
                          mem_is_committed(GA(0x007fb700u))?MR32(GA(0x007fb700u)):0,
                          mem_is_committed(GA(0x007851c0u))?(double)MRF32(GA(0x007851c0u)):0.0,
                          mem_is_committed(GA(0x007875bcu))?(double)MRF32(GA(0x007875bcu)):0.0,
                          mem_is_committed(GA(0x00864720u))?(double)MRF32(GA(0x00864720u)):0.0); }
                { uint64_t host_us(void); uint32_t host_ms(void);
                  fprintf(stderr,"  [clock] frame %u: host_us=%llu  host_ms=%u\n",
                          g_present_no, (unsigned long long)host_us(), host_ms()); }
                fprintf(stderr,"  [boot] frame %u  list=%08x  flag(83648c)=%u  (836498)=%08x\n",
                        g_present_no, head,
                        mem_is_committed(GA(0x0083648cu)) ? MR8(GA(0x0083648cu)) : 999u,
                        mem_is_committed(GA(0x00836498u)) ? MR32(GA(0x00836498u)) : 0);
                if (head && mem_is_committed(head+0x40u)){
                    for (uint32_t q = 0; q < 0x40u; q += 4u){
                        uint32_t V = MR32(head+q);
                        const char *t = 0;
                        if (V > 0x10000u && mem_is_committed(V+40u)){
                            const char *z = (const char*)(g_mem+V);
                            int ok=1, L=0;
                            while (L<38 && z[L]){ unsigned char ch=(unsigned char)z[L];
                                if (ch<0x20u||ch>0x7Eu){ ok=0; break; } L++; }
                            if (ok && L>=3 && z[L]==0) t = z;
                        }
                        fprintf(stderr,"      +%02x = %08x%s%s\n", q, V,
                                t ? "  -> " : "", t ? t : "");
                    }
                }
            }
            /* FLOWLOG: read the game's own flow tracker.
             * 0x86549c and 0x864f88 hold the NAME of the step currently running. */
            if (ENVC("FLOWLOG") && (g_present_no % 500u) == 1u){
                static char last[160] = {0};
                char cur[160]; cur[0] = 0;
                uint32_t gl[2] = { GA(0x0086549cu), GA(0x00864f88u) };
                for (int q = 0; q < 2; q++){
                    const char *t = "(?)";
                    if (mem_is_committed(gl[q]+3)){
                        uint32_t P = MR32(gl[q]);
                        if (P > 0x10000u && mem_is_committed(P+2)) t = (const char*)(g_mem+P);
                    }
                    strncat(cur, t, 70);
                    strncat(cur, " | ", 4);
                }
                if (strcmp(cur, last)){
                    snprintf(last, sizeof last, "%s", cur);
                    fprintf(stderr,"  [flow] frame %u: %s  flag(865498)=%u (8654a4)=%u\n",
                            g_present_no, cur,
                            mem_is_committed(GA(0x00865498u))?MR8(GA(0x00865498u)):999u,
                            mem_is_committed(GA(0x008654a4u))?MR32(GA(0x008654a4u)):999u);
                }
            }
            /* PRESENTBT: extract the structure of the main loop. */
            { const char *pb = ENVC("PRESENTBT");
              static int nb = 0;
              if (pb && nb < 2 && g_present_no >= (uint32_t)atoi(pb)){ nb++;
                  fprintf(stderr,"  [main-loop] Present #%u:\n", g_present_no);
                  guest_backtrace(c, 26); } }
            /* To measure the steady state: at the given frame reset coverage and start the
             * sampler.  This way you see "what is spinning now", not "everything since
             * startup". */
            { static int done = 0; const char *ra2 = ENVC("COVRESET_AT");
              uint32_t at = ra2 ? (uint32_t)atoi(ra2) : 0;
              if (!done && at && g_present_no >= at){ done = 1;
                  cov_reset();
                  fprintf(stderr,"  [measure] frame %u: coverage reset, sampler started\n",
                          g_present_no); }
              }
            g_present++;
            { void vclock_tick(void); vclock_tick(); }
            /* Since game speed = real fps / 60, throttle the frame to 60 Hz
             * (DEFAULT: NO limiter; locked to 60 Hz with PACE60=1). */
            { void frame_pace(void); frame_pace(); }
            /* COVRESET=<Present#>: reset coverage from that frame on so the report shows ONLY
             * the code running in the steady state. */
            { const char *cr = ENVC("COVRESET");
              if (cr && g_present_no == (uint32_t)atoi(cr)){
                  void cov_reset(void); cov_reset();
                  fprintf(stderr,"  coverage reset (Present %u)\n", g_present_no);
              } }
            { const char *rt = ENVC("RBTEST");
              if (rt && metal_available() && !g_rbtest_done && g_present_no == (uint32_t)atoi(rt)){
                  g_rbtest_done = 1; rbtest_run("at Present"); } }
            if (metal_available()) metal_present();
            { extern int g_perflog; void perflog_guest_frame(void);
              if (g_perflog) perflog_guest_frame(); }
            { void mprot_rearm(void); mprot_rearm(); }
            /* The game's own slot pools: if one runs out a red name is printed on screen.
             * List 0x828538 (tail) -> +4 (previous) ... 0x828534. */
            if (ENVC("POOLS") && (g_present_no % 150) == 20){
                fprintf(stderr,"  --- slot pools (list head 0x828530=%u) ---\n",
                        mem_is_committed(GA(0x828530u)) ? MR32(GA(0x828530u)) : 0xFFFFFFFFu);
                /* directly known pools: EventHandler (0x883e38), Event (0x883e34?) */
                { uint32_t known[] = {GA(0x883e38u), GA(0x883e34u)};
                  for (int q=0;q<2;q++){
                    if (!mem_is_committed(known[q]+4)) continue;
                    uint32_t po = MR32(known[q]);
                    if (!po || !mem_is_committed(po+0x2f)) continue;
                    uint32_t np2 = MR32(po+0x0c);
                    char nm3[48]; nm3[0]=0;
                    if (np2 && mem_is_committed(np2))
                        for (int k2=0;k2<40;k2++){ uint8_t ch=MR8(np2+k2);
                            if (ch<32||ch>126){ nm3[k2]=0; break; } nm3[k2]=(char)ch; nm3[k2+1]=0; }
                    fprintf(stderr,"    [0x%08x] %-24s capacity=%u free=%u element=%u"
                            "  (+08=%u +10=%u +18=%u +1c=%u)\n",
                            known[q], nm3, MR32(po+0x24), MR32(po+0x2c), MR32(po+0x28),
                            MR32(po+8), MR32(po+0x10), MR32(po+0x18), MR32(po+0x1c));
                  } }
                uint32_t n2 = mem_is_committed(GA(0x828538u)) ? MR32(GA(0x828538u)) : 0;
                int guard2 = 0;
                while (n2 && n2 != GA(0x828534u) && guard2++ < 400){
                    if (!mem_is_committed(n2+0x2f)) break;
                    uint32_t nameptr = MR32(n2+0x0c);
                    uint32_t cap = MR32(n2+0x24), esz = MR32(n2+0x28), fr = MR32(n2+0x2c);
                    char nm2[48]; nm2[0]=0;
                    if (nameptr && mem_is_committed(nameptr))
                        for (int q=0;q<40;q++){ uint8_t ch=MR8(nameptr+q);
                            if (ch<32||ch>126){ nm2[q]=0; break; } nm2[q]=(char)ch; nm2[q+1]=0; }
                    if (cap && cap < 100000u)
                        fprintf(stderr,"    %-28s capacity=%-6u free=%-6u element=%u%s\n",
                                nm2, cap, fr, esz, (fr==0)?"   <<< TUKENDI":"");
                    n2 = MR32(n2+4);
                }
            }
            if (g_present_no == 100){
                if (ENVC("HASHSCAN")){
                    static const uint32_t want[] = {0x3EE7363Fu, 0x04191C37u};
                    static const char *nmz[] = {"WORLDVIEWPROJECTION","TEXTUREOFFSET"};
                    for (int hz=0; hz<2; hz++){
                        int cnt=0; uint32_t first[6]; int nf2=0;
                        for (uint32_t a=0x10000000u; a<0x10800000u; a+=4){
                            if (!mem_is_committed(a+4)){ a += 0x3FFCu; continue; }
                            if (MR32(a)==want[hz]){ cnt++; if(nf2<6) first[nf2++]=a; }
                        }
                        fprintf(stderr,"  [hash] %-20s 0x%08X -> in %d places:",
                                nmz[hz], want[hz], cnt);
                        for (int q=0;q<nf2;q++) fprintf(stderr," %08x", first[q]);
                        fprintf(stderr,"\n");
                    }
                }
            }
            /* RESETAT=<frame>: produce a device loss at that frame (test) */
            { const char *ra = ENVC("RESETAT");   /* several frames separated by commas */
              int hitr = 0;
              for (const char *q = ra; q && *q; ){ if ((uint32_t)atoi(q) == g_present_no) hitr = 1;
                  while (*q && *q != ',') q++; if (*q) q++; }
              if (hitr){
                  fprintf(stderr,"com: RESETAT: device loss at frame %u\n", g_present_no);
                  com_request_reset(); } }
            r=0;
            if (atomic_load(&g_lost)){ r = 0x88760868u;   /* D3DERR_DEVICELOST */
                if (g_lost_presents++ < 4) fprintf(stderr,"com: Present -> DEVICELOST\n"); }
            }
        else if (MID(StretchRect)){
            /* (this, pSrcSurface, pSrcRect, pDstSurface, pDstRect, Filter)
             * HAD NEVER BEEN IMPLEMENTED (measured: 2261 calls in a session).  The game
             * copies the scene from the back buffer into a texture with this; the
             * post-process (bleach bypass / motion blur / DOF) reads from that texture.
             * Without the copy the source stayed empty and the composite produced
             * BLACK -- this was why the main menu went completely dark at high detail.
             * Surface layout: +28 = Metal texture id, +40 = origin
             * (1 = back buffer, 2 = texture surface, 4 = active target). */
            uint32_t ss=A(c,1), ds=A(c,3);
            uint32_t stex = (ss && mem_is_committed(ss+31)) ? MR32(ss+28) : 0u;
            uint32_t dtex = (ds && mem_is_committed(ds+31)) ? MR32(ds+28) : 0u;
            if (ENVC("BLITLOG")){
                static int nb2=0;
                if (nb2 < 8){ nb2++;
                    uint32_t so=(ss&&mem_is_committed(ss+43))?MR32(ss+40):0;
                    uint32_t doo=(ds&&mem_is_committed(ds+43))?MR32(ds+40):0;
                    fprintf(stderr,"  [blit] source tex=%u origin=%u (%ux%u) -> dest tex=%u origin=%u (%ux%u)\n",
                            stex, so, ss?MR32(ss+4):0, ss?MR32(ss+8):0,
                            dtex, doo, ds?MR32(ds+4):0, ds?MR32(ds+8):0); }
            }
            { const char *rt = ENVC("RBTEST");
              if (rt && !stex && metal_available() && !g_rbtest_done && g_present_no + 1u == (uint32_t)atoi(rt)){
                  g_rbtest_done = 1; rbtest_run("first screen copy"); } }
            if (metal_available() && (stex || dtex) && stex != dtex){
                void metal_blit(unsigned,unsigned);
                void metal_blit_rect(unsigned,unsigned,float,float,float,float);
                /* A source RECT (left, top, right, bottom): only that part is copied.
                 * The magazine / DVD cover photo is cut from the middle of the screen
                 * (0x5c3150); it used to be squeezed from the whole screen. */
                uint32_t sr = A(c,2);
                uint32_t sw = (ss && mem_is_committed(ss+11)) ? MR32(ss+4) : 0u;
                uint32_t sh = (ss && mem_is_committed(ss+11)) ? MR32(ss+8) : 0u;
                if (!stex){ sw = g_bb_w; sh = g_bb_h; }   /* the back buffer */
                if (sr && mem_is_committed(sr+15) && sw && sh){
                    int32_t l = (int32_t)MR32(sr), t = (int32_t)MR32(sr+4);
                    int32_t rr = (int32_t)MR32(sr+8), b = (int32_t)MR32(sr+12);
                    int whole = l <= 0 && t <= 0 && rr >= (int32_t)sw && b >= (int32_t)sh;
                    if (rr > l && b > t && !whole)
                        metal_blit_rect(stex, dtex, (float)l/(float)sw, (float)t/(float)sh,
                                        (float)rr/(float)sw, (float)b/(float)sh);
                    else metal_blit(stex, dtex);
                } else metal_blit(stex, dtex);
            }
            r=0; }
        else if (MID(SetRenderTarget)){
            if (ENVC("CUBELOG")){
                static unsigned n3 = 0;
                uint32_t sf2 = A(c,2);
                if (n3 < 8 && sf2 && mem_is_committed(sf2+43) && MR32(sf2+40)==2u){
                    n3++;
                    fprintf(stderr,"  [cube] SetRenderTarget CUBE FACE 0x%08x\n", sf2);
                }
            }
            /* (this, RenderTargetIndex, pRenderTarget) */
            uint32_t idx=A(c,1), surf=A(c,2);
            if (idx==0 && metal_available()){
                uint32_t tid = (surf && mem_is_committed(surf+35)) ? MR32(surf+28) : 0;
                { static uint64_t org[8]; static int shown=0;
                  uint32_t o5 = (surf && mem_is_committed(surf+43)) ? MR32(surf+40) : 7;
                  if (o5 < 8) org[o5]++;
                  if (ENVC("RTLOG") && !shown && (org[0]+org[1]+org[2]+org[3]+org[4]+org[7]) % 400 == 0){
                    fprintf(stderr,"  [RT] origin counter: none=%llu backbuf=%llu surfaceLvl=%llu"
                            " created=%llu getRT=%llu unknown=%llu\n",
                            (unsigned long long)org[0],(unsigned long long)org[1],
                            (unsigned long long)org[2],(unsigned long long)org[3],
                            (unsigned long long)org[4],(unsigned long long)org[7]); } }
                uint32_t face = (surf && mem_is_committed(surf+47)) ? MR32(surf+44) : 0u;
                g_cur_rt = tid;
                g_cur_rt_w = (tid && mem_is_committed(surf+11)) ? MR32(surf+4) : 0u;
                g_cur_rt_h = (tid && mem_is_committed(surf+11)) ? MR32(surf+8) : 0u;
                metal_set_render_target(tid | ((face & 7u) << 16));
                if (tid) g_rt_set++; else g_rt_screen++;
            }
            r=0; }
        else if (MID(SetTransform)){
            static int nt=0;
            uint32_t ty=A(c,1), mp=A(c,2);
            /* XFORMAT=<frame>: ALL SetTransform calls in that frame only. */
            { static long xa = -2; if (xa == -2){ const char *e = ENVC("XFORMAT"); xa = e ? atol(e) : -1; }
              if (xa >= 0 && (long)g_present_no == xa && mp && mem_is_committed(mp+63)){
                const char *tn = ty==2?"VIEW":(ty==3?"PROJECTION":(ty==256?"WORLD":"?"));
                fprintf(stderr,"  [xf@%ld] type=%u(%s) caller=0x%08x\n", xa, ty, tn, c->eip);
                for (int rw=0; rw<4; rw++)
                    fprintf(stderr,"      %10.4f %10.4f %10.4f %10.4f\n",
                            MRF32(mp+rw*16), MRF32(mp+rw*16+4),
                            MRF32(mp+rw*16+8), MRF32(mp+rw*16+12)); } }
            if (ENVC("XFORM") && nt<9 && mp && mem_is_committed(mp+63)){ nt++;
                const char *tn = ty==2?"VIEW":(ty==3?"PROJECTION":(ty==256?"WORLD":"?"));
                fprintf(stderr,"  [SetTransform] tip=%u(%s)\n", ty, tn);
                for (int rw=0; rw<4; rw++)
                    fprintf(stderr,"      %10.4f %10.4f %10.4f %10.4f\n",
                            MRF32(mp+rw*16), MRF32(mp+rw*16+4),
                            MRF32(mp+rw*16+8), MRF32(mp+rw*16+12));
            }
            r=0; }
        else if (MID(SetViewport)){
            static int nv2=0;
            uint32_t vp2=A(c,1);
            if (ENVC("XFORM") && nv2<6 && vp2 && mem_is_committed(vp2+23)){ nv2++;
                fprintf(stderr,"  [SetViewport] x=%u y=%u g=%u y=%u minZ=%.3f maxZ=%.3f\n",
                        MR32(vp2), MR32(vp2+4), MR32(vp2+8), MR32(vp2+12),
                        MRF32(vp2+16), MRF32(vp2+20));
            }
            r=0; }
        else if (MID(SetVertexShaderConstantI)){
            uint32_t sr=A(c,1), pd=A(c,2), n=A(c,3);
            for (uint32_t k=0; k<n && sr+k<16; k++){
                uint32_t o=pd+16u*k;
                if (!mem_is_committed(o+15)) break;
                for (int j2=0;j2<4;j2++) g_vsk.i[sr+k][j2]=(int32_t)MR32(o+4u*j2);
            }
            r=0; }
        else if (MID(SetVertexShaderConstantB)){
            uint32_t sr=A(c,1), pd=A(c,2), n=A(c,3);
            for (uint32_t k=0; k<n && sr+k<16; k++){
                uint32_t o=pd+4u*k;
                if (!mem_is_committed(o+3)) break;
                g_vsk.b[sr+k]=(int32_t)MR32(o);
            }
            r=0; }
        else if (MID(SetVertexDeclaration)){ g_cur_decl=A(c,1); g_raw_decl=g_cur_decl; r=0; }
        else if (MID(Reset)){
            /* (this, D3DPRESENT_PARAMETERS*)
             * The game resets the device when it changes resolution/graphics detail.
             * This method HAD NEVER been implemented; changing settings crashed. */
            uint32_t pp3=A(c,1);
            if (pp3 && mem_is_committed(pp3+7)){
                uint32_t w3=MR32(pp3), h3=MR32(pp3+4);
                fprintf(stderr,"  [D3D] Reset: %ux%u\n", w3, h3);
                if (w3 > 8192u || h3 > 8192u) w3 = h3 = 0;   /* garbage size */
                /* The SAME rule as CreateDevice must be applied: Reset used to pass the raw value
                 * through, so when the resolution was changed in-game the image got
                 * blurry again. */
                if (w3 && h3){ uint32_t d3w = w3, d3h = h3;
                    res_pick(w3, h3, &d3w, &d3h); w3 = d3w; h3 = d3h; }
                if (w3 && h3){ g_bb_w = w3; g_bb_h = h3; }
                if (w3 && h3){
                    void metal_want_size(unsigned,unsigned); metal_want_size(w3,h3);
                    if (metal_available()){ void metal_resize(unsigned,unsigned);
                                            metal_resize(w3,h3); } }
            }
            g_resets++;
            /* The game must have released all render targets before Reset (the D3D9 rule);
             * release them on the Metal side too, otherwise every resolution change
             * leaked all of them. */
            if (metal_available()){ void metal_reset_rts(void); metal_reset_rts(); }
            if (atomic_exchange(&g_lost, 0))
                fprintf(stderr,"com: device loss closed by Reset (TCL %lu calls)\n", g_tcl_n);
            /* After Reset the state returns to defaults.  D3D9: target 0 returns to the back
             * buffer, the Z surface to the IMPLICIT Z (EnableAutoDepthStencil), and the
             * game relies on this and never sets them again.  Previously the last
             * target/Z stayed as they were on the Metal side.  The fullscreen transition
             * catches Reset MID-FRAME through the device-loss path: at that moment a draw
             * to texture 7 without Z (NULL) was in progress (measured).  Afterwards the
             * main scene was drawn without a Z test -> the car inside-out, the ground
             * reflection black.  Old behavior: NORTRESET=1 (3/3 broken). */
            if (g_cur_zid != 1u)
                fprintf(stderr, "com: Reset: bound Z surface %u -> implicit Z\n", g_cur_zid);
            /* D3D9 Reset returns ALL device state (render/sampler/stage states) to the
             * defaults and the game relies on this: it pulls its own state shadow back
             * to the defaults too and never rewrites the states it thinks are "already
             * like that".  We kept the old values (if Reset hit mid-frame the state of
             * the current pass leaked).  No visible difference in the measured runs; it
             * is required by the D3D9 rule. */
            if (!ENVC("NORSRESET")){
                memset(g_rs, 0, sizeof g_rs); g_rs_init = 0; g_rs_dirty = 1;
                memset(g_smp_raw, 0, sizeof g_smp_raw);
                memset(g_tss_raw, 0, sizeof g_tss_raw);
                for (int q7 = 0; q7 < 8; q7++) g_tss_raw[q7][11] = (uint32_t)q7;
                if (metal_available()){ rs_apply(); g_rs_dirty = 0; }
            }
            if (g_cur_rt)
                fprintf(stderr, "com: Reset: bound render target texture %u -> back buffer\n", g_cur_rt);
            if (!ENVC("NORTRESET")){
            g_cur_zid = 1;
            if (metal_available()){ void metal_set_zsurf(unsigned); metal_set_zsurf(1);
                                    metal_set_render_target(0); } }
            g_cur_tex = 0; g_cur_rt = 0; g_cur_rt_w = g_cur_rt_h = 0; g_cur_ps_id = 0;
            for (int q6=0;q6<4;q6++) g_tex_stage[q6]=0;
            r=0; }
        else if (MID(SetFVF)){ g_cur_fvf=A(c,1); g_raw_fvf=g_cur_fvf; r=0; }
        else if (MID(CreateTexture)){
            /* (this,W,H,Levels,Usage,Format,Pool,ppTex,pShared) */
            uint32_t w=A(c,1), h=A(c,2), lev=A(c,3), usage=A(c,4), fmt=A(c,5), pool=A(c,6);
            /* With NFSRES active the game creates its post-process targets at the resolution
             * it THINKS it has (1280x1024).  The scene is drawn at 1920x1080, then copied
             * into that small target and scaled back up: both a resolution loss and a
             * scaled blit.  Pulling the fullscreen targets to the device size makes the
             * chain 1:1 from start to end. */
            if ((usage & 1u) && g_req_w && w == g_req_w && h == g_req_h){
                static int sr4 = 0;
                if (sr4 < 3){ sr4++;
                    fprintf(stderr,"  [RT-tex] %ux%u -> %ux%u (NFSRES)\n", w,h,g_bb_w,g_bb_h); }
                w = g_bb_w; h = g_bb_h;
            }
            uint32_t pp=A(c,7); uint32_t t=com_new(IF_TEX,64);
            MR32(t+4)=w; MR32(t+8)=h; MR32(t+20)=fmt; MR32(t+24)=lev?lev:1u;
            /* HD build: reflection targets grow in Metal, the game keeps seeing the old size
             * (not compiled into the original build). */
            uint32_t mw = w, mh = h, mlev = lev ? lev : 1u;
            int ekind = (usage & 1u) ? enh_rt_size(c->eip, g_bb_w, g_bb_h, &mw, &mh) : ENH_NONE;
            /* Road reflection: a full mip chain for the blur pass (see enhance.c). */
            if (ekind == ENH_ROAD){ mlev = 1u; for (uint32_t q = mw > mh ? mw : mh; q > 1u; q >>= 1) mlev++; }
            if (metal_available()){
                /* D3DUSAGE_RENDERTARGET = 1 */
                uint32_t tid = metal_create_texture_ex(mw,mh,mlev,fmt,(usage & 1u)?1u:0u);
                MR32(t+28)=tid;
                if (ekind == ENH_ROAD && tid){ void metal_hd_mark_road(unsigned,unsigned,unsigned);
                                               metal_hd_mark_road(tid, w, h); }
                if ((usage & 1u) && ENVC("RTIDLOG"))
                    fprintf(stderr,"  [RT-id] %ux%u (metal %ux%u) id=%u caller=0x%08x\n",
                            w, h, mw, mh, tid, c->eip);
                if (usage & 1u){ g_rt_tex++;
                    static int sr3=0;
                    if (sr3<6){ sr3++;
                        fprintf(stderr,"  [RT-tex] %ux%u fmt=%u levels=%u -> id=%u\n",
                                w,h,fmt,lev?lev:1u,tid); } }
                if (!tid){ g_tex_fail++;
                    static uint32_t bad[16]; static int nb=0; int seen=0;
                    for (int q=0;q<nb;q++) if (bad[q]==fmt) seen=1;
                    if (!seen && nb<16){ bad[nb++]=fmt;
                        fprintf(stderr,"  [tex] UNSUPPORTED format %u (0x%x) %ux%u usage=0x%x\n",
                                fmt, fmt, w, h, usage); }
                }
            }
            { static uint64_t fmt_hist[64]; static int sh=0;
              /* D3DFMT: 21=A8R8G8B8 22=X8R8G8B8 23=R5G6B5 25=A1R5G5B5 26=A4R4G4B4
                 827611204='DXT1' 861165636='DXT3' 894720068='DXT5' */
              const char *fn2 = "?";
              switch(fmt){ case 21: fn2="A8R8G8B8"; break; case 22: fn2="X8R8G8B8"; break;
                           case 23: fn2="R5G6B5"; break; case 25: fn2="A1R5G5B5"; break;
                           case 26: fn2="A4R4G4B4"; break; case 28: fn2="A8"; break;
                           case 0x31545844: fn2="DXT1"; break; case 0x33545844: fn2="DXT3"; break;
                           case 0x35545844: fn2="DXT5"; break; case 41: fn2="L8"; break; }
              if (fmt<64) fmt_hist[fmt]++;
              if (VB() && sh<12){ sh++;
                fprintf(stderr,"  [tex] %ux%u lev=%u usage=0x%x fmt=%u(%s) pool=%u\n",
                        w,h,lev,usage,fmt,fn2,pool); } }
            if(pp) MR32(pp)=t; r=0; }
        else if (MID(CreateCubeTexture)){
            /* (this, EdgeLength, Levels, Usage, Format, Pool, ppCubeTexture, pShared)
             * An empty COM object used to be returned: no size/format, no Metal texture
             * -> garbage values were read in the reflection path. */
            uint32_t edge=A(c,1), lev=A(c,2), usage=A(c,3), fmt=A(c,4), pp=A(c,6);
            uint32_t t2=com_new(IF_TEX,160);        /* wide enough for face buffers */
            MR32(t2+140)=0xC0BEC0BEu;                /* cube marker (Release frees faces) */
            if (!edge || edge > 4096u) edge = 256u;
            MR32(t2+4)=edge; MR32(t2+8)=edge; MR32(t2+20)=fmt?fmt:22u;
            MR32(t2+24)=lev?lev:1u;
            uint32_t medge = edge, medge2 = edge;
            if (usage & 1u) enh_rt_size(c->eip, g_bb_w, g_bb_h, &medge, &medge2);
            if (metal_available()){
                unsigned metal_create_texture_cube(unsigned,unsigned,unsigned,unsigned);
                /* A cube texture is ALWAYS a render target: the game draws the environment map to
                 * the six faces and then samples it. */
                MR32(t2+28)= getenv("NOCUBE")
                    ? metal_create_texture_ex(medge,medge,lev?lev:1u,fmt?fmt:22u,1u)
                    : metal_create_texture_cube(medge,lev?lev:1u,fmt?fmt:22u,1u);
            }
            if (ENVC("CUBELOG")) fprintf(stderr,"  [cube] CreateCubeTexture edge=%u levels=%u usage=0x%x format=%u pool=%u -> metal %u\n",
                                          edge, lev, usage, fmt, A(c,5), MR32(t2+28));
            cube_mark(t2);                          /* CUBE TEXTURE registry */
            if(pp) MR32(pp)=t2; r=0; }
        else if (MID(GetCubeMapSurface)){
            /* (this, FaceType, Level, ppCubeMapSurface) */
            uint32_t self2=A(c,0), pp=A(c,3); uint32_t sf=com_new(IF_SURF,64);
            MR32(sf+40)=2;
            /* +44: FACE index + 1 (0 = not a cube face).  SetRenderTarget packs it into the
             * target field; otherwise the six faces are drawn on top of each other into
             * the same texture. */
            { uint32_t face = A(c,1); MR32(sf+44) = (face < 6u) ? face + 1u : 1u; }
            if (self2 && mem_is_committed(self2+31)){
                MR32(sf+4)=MR32(self2+4); MR32(sf+8)=MR32(self2+8);
                MR32(sf+20)=MR32(self2+20); MR32(sf+28)=MR32(self2+28);
            }
            if(pp) MR32(pp)=sf; r=0; }
        else if (MID(CreateVolumeTexture)){ uint32_t pp=A(c,8); if(pp) MR32(pp)=com_new(IF_TEX,64); r=0; }
        else if (MID(CreateVertexBuffer)){
            uint32_t pp=A(c,5); uint32_t b=com_new(IF_VB,64);
            MR32(b+4)=A(c,1); MR32(b+8)=gbuf_alloc(A(c,1)?A(c,1):64);
            if(pp) MR32(pp)=b; r=0; }
        else if (MID(CreateIndexBuffer)){
            uint32_t pp=A(c,5); uint32_t b=com_new(IF_IB,64);
            MR32(b+4)=A(c,1); MR32(b+8)=gbuf_alloc(A(c,1)?A(c,1):64);
            MR32(b+12)=A(c,3);              /* Format: 101=16bit, 102=32bit */
            if(pp) MR32(pp)=b; r=0; }
        else if (MID(CreateRenderTarget)){
            /* (this,W,H,Format,MultiSample,MSQuality,Lockable,ppSurface,pShared) */
            uint32_t w2=A(c,1), h2=A(c,2), f2=A(c,3), pp=A(c,7);
            uint32_t sf=com_new(IF_SURF,64);
            MR32(sf+4)=w2; MR32(sf+8)=h2; MR32(sf+20)=f2; MR32(sf+40)=3;
            if (metal_available()) MR32(sf+28)=metal_create_texture_ex(w2,h2,1,f2,1);
            if(pp) MR32(pp)=sf; r=0; }
        else if (MID(CreateDepthStencilSurface)){
            uint32_t pp=A(c,7); uint32_t sf=com_new(IF_SURF,64);
            MR32(sf+4)=A(c,1); MR32(sf+8)=A(c,2); MR32(sf+20)=A(c,3); MR32(sf+40)=5;
            /* Z surface id (metal.m g_ztex): 1 implicit, >=2 created. */
            /* On Reset the game recreates its surfaces; when the table fills up wrap around
             * (the old ones have been released by then), have the texture reset. */
            static uint32_t next_zid = 2;
            if (next_zid >= 64u) next_zid = 2;
            uint32_t zid = next_zid++;
            MR32(sf+52) = zid; MR32(sf+56) = ZSURF_MAGIC;
            if (metal_available()){ void metal_z_forget(unsigned); metal_z_forget(zid); }
            if(pp) MR32(pp)=sf; r=0; }
        else if (MID(CreateOffscreenPlainSurface)){ uint32_t pp=A(c,5); if(pp) MR32(pp)=com_new(IF_SURF,64); r=0; }
        else if (MID(GetBackBuffer)){ uint32_t pp=A(c,4); uint32_t sf=com_new(IF_SURF,64);
            MR32(sf+4)=g_bb_w; MR32(sf+8)=g_bb_h; MR32(sf+20)=22u;
            MR32(sf+40)=1; if(pp) MR32(pp)=sf; r=0; }
        else if (MID(GetRenderTarget)||MID(GetDepthStencilSurface)){
            /* These surfaces must also carry the real back buffer size; otherwise GetDesc
             * returns 0. */
            uint32_t pp=A(c, nargs-1); uint32_t sf=com_new(IF_SURF,64);
            MR32(sf+40)=4; MR32(sf+28)=g_cur_rt;   /* return the active target */
            MR32(sf+4)=g_bb_w; MR32(sf+8)=g_bb_h; MR32(sf+20)=22u;
            if (MID(GetDepthStencilSurface)){
                /* return the active Z surface (the game binds it back later with
                 * SetDepthStencilSurface) */
                MR32(sf+28)=0; MR32(sf+20)=80u;
                MR32(sf+52)=g_cur_zid; MR32(sf+56)=ZSURF_MAGIC;
            }
            if(pp) MR32(pp)=sf; r=0; }
        else if (MID(SetDepthStencilSurface)){
            /* (this, pNewZStencil).  NULL -> Z test/write disabled. */
            uint32_t sf=A(c,1), zid=0;
            if (sf){
                zid = 1;   /* unrecognized surface: implicit */
                if (mem_is_committed(sf+59) && MR32(sf+56)==ZSURF_MAGIC) zid = MR32(sf+52);
            }
            g_cur_zid = zid;
            if (metal_available()){ void metal_set_zsurf(unsigned); metal_set_zsurf(zid); }
            r=0; }
        else if (MID(CreateVertexDeclaration)){
            uint32_t el=A(c,1), pp=A(c,2);
            { static int sh=0;
              if (VB() && sh<16 && el){ sh++;
                static const char *TY[]={"FLOAT1","FLOAT2","FLOAT3","FLOAT4","D3DCOLOR",
                    "UBYTE4","SHORT2","SHORT4","UBYTE4N","SHORT2N","SHORT4N","USHORT2N",
                    "USHORT4N","UDEC3","DEC3N","FLOAT16_2","FLOAT16_4","UNUSED"};
                static const char *US[]={"POSITION","BLENDWEIGHT","BLENDINDICES","NORMAL",
                    "PSIZE","TEXCOORD","TANGENT","BINORMAL","TESSFACTOR","POSITIONT",
                    "COLOR","FOG","DEPTH","SAMPLE"};
                fprintf(stderr,"  [vdecl] declaration:\n");
                for (uint32_t k=0;k<32;k++){
                    uint32_t e=el+8u*k;
                    if (!mem_is_committed(e+7)) break;
                    uint32_t st=MR16(e), of=MR16(e+2);
                    uint32_t ty=MR8(e+4), me=MR8(e+5), us=MR8(e+6), ui=MR8(e+7);
                    if (st==0xFFu && ty==17u){ fprintf(stderr,"     [%u] END\n",k); break; }
                    fprintf(stderr,"     [%u] stream=%u offset=%-3u type=%-8s usage=%-10s idx=%u (method=%u)\n",
                            k, st, of, ty<18?TY[ty]:"?", us<14?US[us]:"?", ui, me);
                }
              } }
            uint32_t h=com_new(IF_VDECL,32);
            uint32_t n=0;
            if (el){
                for(; n<64; n++){
                    uint32_t e=el+8u*n;
                    if (!mem_is_committed(e+7)) break;
                    if (MR16(e)==0xFFu && MR8(e+4)==17u){ n++; break; }
                }
            }
            uint32_t bytes = 8u*n;
            uint32_t copy = bytes ? gbuf_alloc(bytes) : 0;
            g_gb_kept[0] += (bytes + 63u) & ~63u;
            if (copy && el) memcpy(g_mem+copy, g_mem+el, bytes);
            MR32(h+4)=copy; MR32(h+8)=bytes;
            if(pp) MR32(pp)=h; r=0; }
        else if (MID(CreateVertexShader)||MID(CreatePixelShader)){
            uint32_t fn=A(c,1), pp=A(c,2);
            uint32_t h=com_new(IF_VSHADER,32);
            /* scan the shader token stream: up to the 0x0000FFFF END token */
            uint32_t sz=0;
            if (fn){ uint32_t k=0;
                for (; k<16384; k++){
                    if (!mem_is_committed(fn+4u*k)) break;
                    uint32_t t=MR32(fn+4u*k); if(t==0x0000FFFFu){ k++; break; }
                }
                sz = 4u*k; }
            uint32_t copy = sz ? gbuf_alloc(sz) : 0;
            g_gb_kept[0] += (sz + 63u) & ~63u;
            if (copy && fn) memcpy(g_mem+copy, g_mem+fn, sz);
            MR32(h+4)=copy; MR32(h+8)=sz; MR32(h+12)=0;
            /* PSDUMP: pixel shaders use the same token format too; compile and disassemble
             * them to see WHAT they do. */
            if (MID(CreatePixelShader) && copy && sz >= 8 && ENVC("PSDUMP")){
                static int pn = 0;
                if (pn < atoi(ENVC("PSDUMP"))){
                    VSProg *pp2 = vs_compile((const uint32_t *)(g_mem+copy), sz/4u);
                    if (pp2 && vs_ninstr(pp2) < 5u){ pp2 = 0; }   /* skip simple UI */
                    if (pp2){ pn++;
                        fprintf(stderr,"  --- PIXEL SHADER #%d (%u instructions) ---\n",
                                pn, vs_ninstr(pp2));
                        vs_disasm(pp2, stderr);
                        { static char msl[16384];
                          int len = ps_to_msl(pp2, pn, msl, sizeof msl);
                          if (len > 0) fprintf(stderr, "--- URETILEN MSL ---\n%s\n", msl);
                          else fprintf(stderr, "--- MSL could not be generated ---\n"); }
                    }
                }
            }
            /* Translate the pixel shader to MSL and register it with Metal:
             * the game's own shader runs EXACTLY on the GPU. */
            if (MID(CreatePixelShader) && copy && sz >= 8){
                VSProg *pp3 = vs_compile((const uint32_t *)(g_mem+copy), sz/4u);
                if (pp3){
                    static char msl[16384];
                    int id = ++g_psn;
                    void metal_register_ps(int, const char *);
                    if (ps_to_msl(pp3, id, msl, sizeof msl) > 0){
                        { const char *pm = ENVC("PSMSL");   /* PSMSL=<id>[,<id>...] */
                          for (const char *q = pm; q && *q; ){ while (*q == ',') q++;
                              if (atoi(q) == id){ fprintf(stderr, "--- MSL ps %d ---\n", id); vs_disasm(pp3, stderr);
                                                  fprintf(stderr, "%s\n", msl); break; }
                              while (*q && *q != ',') q++; } }
                        metal_register_ps(id, msl);
                        MR32(h+12) = (uint32_t)id;
                    }
                    /* CUBE VARIANT (id + PSCUBE): in D3D9 ps_1_x doesn't declare the texture TYPE, the
                     * type comes from the binding.  The car environment map binds a cube
                     * texture to stage 0; if a shader expecting texture2d is bound to the
                     * same slot, Metal rejects the draw (validateCommonDrawErrors).
                     * The variant is selected at draw time based on the bound texture. */
                    if (id < PSCUBE &&
                        ps_to_msl_ex(pp3, id + PSCUBE, msl, sizeof msl, 1u) > 0){
                        /* PSMSL=<id>: write the MSL of that id (including the cube variant) */
                        if (ENVC("PSMSL") && atoi(ENVC("PSMSL")) == id + PSCUBE)
                            fprintf(stderr, "--- MSL ps %d ---\n%s\n", id + PSCUBE, msl);
                        metal_register_ps(id + PSCUBE, msl);
                    }
                    /* the program is kept: the HD reflection separation and post-process chain variants
                     * are generated at draw time, when needed */
                    if (id < PSCUBE && !g_psprog[id]) g_psprog[id] = pp3;
                    else vs_free(pp3);
                }
            }
            if (MID(CreateVertexShader) && copy && sz >= 8 && g_vsprogn < MAXVS){
                VSProg *vp2 = vs_compile((const uint32_t *)(g_mem+copy), sz/4u);
                if (vp2){ g_vsprog[g_vsprogn] = vp2; MR32(h+12) = g_vsprogn++;
                    if (ENVC("VSDUMP")){
                        static int vd2 = 0;
                        if (vd2 < atoi(ENVC("VSDUMP")) && vs_ninstr(vp2) >= 10u){ vd2++;
                            static int dcl[16][2];
                            for (int q7=0;q7<16;q7++){ dcl[q7][0]=-1; dcl[q7][1]=0; }
                            dcl[0][0]=0;  dcl[0][1]=2;    /* pos  FLOAT3 @0  */
                            dcl[1][0]=12; dcl[1][1]=4;    /* color D3DCOLOR  */
                            dcl[2][0]=16; dcl[2][1]=1;    /* uv   FLOAT2     */
                            static char msl2[24576];
                            int L2 = vs_to_msl(vp2, (int)g_vsprogn, dcl, msl2, sizeof msl2);
                            fprintf(stderr,"--- VERTEX SHADER #%u (%u instructions) MSL ---\n%s\n",
                                    g_vsprogn, vs_ninstr(vp2), L2>0?msl2:"(could not be generated)");
                        }
                    }
                }
            }
            if(pp) MR32(pp)=h; r=0; }
        else if (MID(BeginStateBlock)){
            /* D3D9: a nested Begin IS INVALID (the second returns D3DERR_INVALIDCALL).  The game
             * does this (845 and 846 back to back in the trace); counting the second as a
             * new block made the outer block get lost. */
            if (g_sb_rec) r=0x8876086Cu;          /* D3DERR_INVALIDCALL */
            else {
                int slot = 0;
                for (int q = 1; q < g_sbn; q++) if (!g_sb[q].used){ slot = q; break; }
                if (!slot && g_sbn < MAXSB) slot = g_sbn++;
                if (slot){ g_sb_rec = slot; g_sb[slot].n = 0; g_sb[slot].used = 1; }
                else { g_sb_rec = 0; g_sb_full++; }
                r=0;
            } }
        else if (MID(CreateStateBlock)){
            /* Captures a predefined set (D3DSBT_ALL etc.).  Never used in the trace; we leave
             * an empty block. */
            uint32_t pp=A(c,2);
            if(pp){ uint32_t o=com_new(IF_STATEBLOCK,32); MR32(o+8)=0; MR32(pp)=o; }
            r=0; }
        else if (MID(CreateQuery)){ uint32_t pp=A(c,2); if(pp) MR32(pp)=com_new(IF_QUERY,32); r=0; }
        else if (MID(EndStateBlock)){
            uint32_t pp=A(c,1);
            uint32_t o=com_new(IF_STATEBLOCK,32);
            MR32(o+8) = (uint32_t)g_sb_rec;         /* block index */
            g_sb_rec = 0;
            if(pp) MR32(pp)=o;
            r=0; }
        else if (MID(CreateAdditionalSwapChain)){ uint32_t pp=A(c,2); if(pp) MR32(pp)=com_new(IF_SWAPCHAIN,32); r=0; }
        else if (MID(GetSwapChain)){ uint32_t pp=A(c,2); if(pp) MR32(pp)=com_new(IF_SWAPCHAIN,32); r=0; }
        else if (MID(GetNumberOfSwapChains)) r=1;
        else if (!strncmp(nm,"Get",3) && nargs>=2){
            /* fill the output parameter with a safe default */
            uint32_t p=A(c,nargs-1); uint32_t sz=4;
            if      (MID(GetTransform))           sz=64;
            else if (MID(GetMaterial))            sz=68;
            else if (MID(GetLight))               sz=104;
            else if (MID(GetViewport))            sz=24;
            else if (MID(GetScissorRect))         sz=16;
            else if (MID(GetClipStatus))          sz=8;
            else if (MID(GetCreationParameters))  sz=16;
            else if (MID(GetRasterStatus))        sz=8;
            else if (MID(GetGammaRamp))           sz=1536;
            else if (MID(GetClipPlane))           sz=16;
            else if (!strncmp(nm,"GetVertexShaderConstant",23)
                  || !strncmp(nm,"GetPixelShaderConstant",22)) sz=16*4;
            if (p && p < 0xE0000000u){ mem_commit(p,sz); memset(g_mem+p,0,sz); }
            if (MID(GetViewport) && p){ MR32(p+8)=1280; MR32(p+12)=720; MRF32(p+20)=1.0f; }
            r=0; }
        else r=0;                        /* the remaining Set* -> D3D_OK */
    }
    /* --- resources --- */
    else if (iface==IF_TEX || iface==IF_VB || iface==IF_IB || iface==IF_SURF){
        uint32_t self=A(c,0);
        if      (MID(GetDevice)){ uint32_t p=A(c,1); if(p) MR32(p)=g_dev; r=0; }
        else if (MID(Lock)){
            uint32_t off=A(c,1), sz=A(c,2), pp=A(c,3);
            /* PERSISTENT POOL: the content may change -> let the mirror generation advance and
             * the buffer count as "volatile" for a few frames (see dip_indexed). */
            if (self && (iface==IF_VB || iface==IF_IB)){
                MR32(self+48) = MR32(self+48) + 1u; MR32(self+52) = (uint32_t)g_present; }
            uint32_t base = self?MR32(self+8):0;
            if(!base){ base=gbuf_alloc(sz?sz:65536); if(self) MR32(self+8)=base; }
            if(pp) MR32(pp)=base+off; r=0; }
        else if (MID(LockRect)){
            /* 2D texture : (Level, pLockedRect, pRect, Flags)
             * CUBE texture : (FaceType, Level, pLockedRect, pRect, Flags)
             * Surface    : (pLockedRect, pRect, Flags)
             * If the cube signature is treated like 2D, Level is read as pLockedRect;
             * the game's real structure is NOT FILLED and it crashes with a garbage
             * pointer (measured: guest address 0x400). */
            int is_cube = (iface==IF_TEX && is_cube_tex(self));
            uint32_t face  = is_cube ? A(c,1) : 0u;
            uint32_t level = is_cube ? A(c,2) : ((iface==IF_TEX) ? A(c,1) : 0u);
            uint32_t plr   = is_cube ? A(c,3) : ((iface==IF_TEX) ? A(c,2) : A(c,1));
            uint32_t w = self?MR32(self+4):256, h = self?MR32(self+8):256;
            uint32_t fmt = self?MR32(self+20):21u;
            if(!w) w=256; if(!h) h=256;
            if(w>8192) w=8192; if(h>8192) h=8192;
            if(level>7) level=7;
            uint32_t pitch = metal_available() ? metal_level_pitch(w,level,fmt) : 0;
            uint32_t rows  = metal_available() ? metal_level_rows(h,level,fmt)  : 0;
            if(!pitch){ uint32_t lw=w>>level; if(!lw)lw=1; pitch=lw*4u; }
            if(!rows){  uint32_t lh=h>>level; if(!lh)lh=1; rows=lh; }
            uint32_t slot = self ? (is_cube ? (self + 104u + 4u*(face & 7u))
                                            : (self + 32u + 4u*level)) : 0;
            uint32_t buf = slot ? MR32(slot) : 0;
            if(!buf){ buf = gbuf_alloc(pitch*rows + 64u); if(slot) MR32(slot)=buf;
                     if (iface==IF_SURF) g_gb_kept[2] += (pitch*rows + 127u) & ~63u; }
            if(plr){ mem_commit(plr,8); MR32(plr)=pitch; MR32(plr+4)=buf; }
            /* A lockable render target (CreateRenderTarget, origin 3): the game reads
             * what was drawn into it -- the magazine / DVD cover photo.  Bring the
             * pixels back from the GPU (metal.m, READING BACK A LOCKABLE RENDER TARGET);
             * the buffer used to stay empty and the covers were saved black. */
            if (iface==IF_SURF && self && buf && mem_is_committed(self+43) && MR32(self+40)==3u &&
                MR32(self+28) && metal_available()){
                int metal_rt_readback(unsigned,void*,unsigned,unsigned,unsigned);
                metal_rt_readback(MR32(self+28), g_mem+buf, pitch, w, h);
            }
            r=0; }
        else if (MID(UnlockRect)){
            /* 2D: (Level)  |  CUBE: (FaceType, Level)  |  Surface: () */
            int is_cube2 = (iface==IF_TEX && is_cube_tex(self));
            uint32_t face2 = is_cube2 ? A(c,1) : 0u;
            uint32_t level = is_cube2 ? A(c,2) : ((iface==IF_TEX) ? A(c,1) : 0u);
            if(level>7) level=7;
            uint32_t w = self?MR32(self+4):0, h = self?MR32(self+8):0;
            uint32_t fmt = self?MR32(self+20):21u, mid = self?MR32(self+28):0;
            uint32_t buf = self ? (is_cube2 ? MR32(self + 104u + 4u*(face2 & 7u))
                                            : MR32(self + 32u + 4u*level)) : 0;
            /* Upload to the FACE on a cube texture.  The face info used to be read and thrown
             * away: all six faces were written to slice 0, face 0 carried the last uploaded
             * face, and 1-5 stayed black.  Measured (menu, frame 1650, reflection cube
             * texture 11): face brightnesses 43.8/0/0/0/0/0; in the Windows reference the
             * car roof reflects the ceiling light on the +Y face. */
            if (metal_available() && mid && buf && w && h){
                void metal_upload_texture_face(unsigned,unsigned,unsigned,const void*,unsigned,unsigned,unsigned,unsigned);
                if (is_cube2)
                    metal_upload_texture_face(mid, level, (face2 < 6u) ? face2 : 0u, g_mem+buf,
                                              metal_level_pitch(w,level,fmt), w, h, fmt);
                else
                    metal_upload_texture(mid, level, g_mem+buf,
                                         metal_level_pitch(w,level,fmt), w, h, fmt);
            }
            if (ENVC("LOCKLOG") && w==640 && h==480){
                static int nl=0;
                if (nl<3){ nl++;
                    fprintf(stderr,"  [lock] %ux%u fmt=%u lev=%u mid=%u pitch=%u  first 32 bytes:\n    ",
                            w,h,fmt,level,mid, metal_level_pitch(w,level,fmt));
                    for (int q=0;q<32;q++) fprintf(stderr,"%02x ", MR8(buf+q));
                    fprintf(stderr,"\n    second row: ");
                    uint32_t pt = metal_level_pitch(w,level,fmt);
                    for (int q=0;q<16;q++) fprintf(stderr,"%02x ", MR8(buf+pt+q));
                    fprintf(stderr,"\n");
                }
            }
            r=0; }
        else if (MID(GetLevelCount)) r=(self&&MR32(self+24))?MR32(self+24):1u;
        else if (MID(GetType)) r=(iface==IF_TEX)?3u:(iface==IF_SURF?1u:(iface==IF_VB?4u:5u));
        else if (MID(GetSurfaceLevel)||MID(GetCubeMapSurface)){
            /* The surface must carry the Metal id of the texture it belongs to; otherwise
             * SetRenderTarget can't know which texture to draw into.
             * A CUBE texture uses the same vtable slot but its signature differs:
             *   2D  : GetSurfaceLevel(Level, ppSurface)
             *   CUBE: GetCubeMapSurface(FaceType, Level, ppSurface)
             * If treated like 2D the pointer is written to the WRONG argument. */
            uint32_t self0=A(c,0);
            int cube3 = is_cube_tex(self0);
            uint32_t pp = cube3 ? A(c,3) : A(c,2);
            uint32_t sf=com_new(IF_SURF,64);
            MR32(sf+40)=2;
            /* +44: CUBE FACE index + 1.  Since a cube texture shares the same vtable as 2D,
             * GetCubeMapSurface can fall into this branch (under the name
             * GetSurfaceLevel); if the face isn't written, SetRenderTarget draws all six
             * faces into slice 0 (same meaning as BRANCH1). */
            if (cube3){ uint32_t face = A(c,1); MR32(sf+44) = (face < 6u) ? face + 1u : 1u; }
            /* In D3D9 a texture's surfaces share the texture's reference count, so this
             * call AddRefs the texture.  The game balances it by releasing the texture
             * (the car environment map: 6 faces, then 6 Releases of the cube). */
            if (iface == IF_TEX) rc_addref(self0);
            uint32_t self=A(c,0);
            if (self && mem_is_committed(self+31)){
                MR32(sf+4)=MR32(self+4); MR32(sf+8)=MR32(self+8);
                MR32(sf+20)=MR32(self+20); MR32(sf+28)=MR32(self+28);
            }
            if(pp) MR32(pp)=sf; r=0; }
        else if (MID(GetDesc)||MID(GetLevelDesc)){
            /* D3DSURFACE_DESC layout:
             *   0 Format, 4 Type, 8 Usage, 12 Pool,
             *  16 MultiSampleType, 20 MultiSampleQuality,
             *  24 Width, 28 Height
             * Width/height used to be written at 16/20 (i.e. into the MultiSample
             * fields) -> the game read the size as 0, computed a garbage pointer at
             * high detail and crashed. */
            uint32_t p=A(c,nargs-1);
            if(p){ mem_commit(p,32); memset(g_mem+p,0,32);
                uint32_t fw = self?MR32(self+4):256, fh = self?MR32(self+8):256;
                uint32_t ff = self?MR32(self+20):0;
                if (!fw) fw = 640; if (!fh) fh = 480;
                if (!ff) ff = 22;                    /* D3DFMT_X8R8G8B8 */
                MR32(p)    = ff;                     /* Format          */
                MR32(p+4)  = (iface==IF_TEX) ? 3u : 1u;  /* Type: TEXTURE/SURFACE */
                MR32(p+8)  = 0;                      /* Usage           */
                MR32(p+12) = 0;                      /* Pool: DEFAULT   */
                MR32(p+16) = 0;                      /* MultiSampleType */
                MR32(p+20) = 0;                      /* MS Quality      */
                MR32(p+24) = fw;                     /* Width           */
                MR32(p+28) = fh;                     /* Height          */
            }
            r=0; }
        else r=0;
    }
    /* --- DirectSound --- */
    else if (iface==IF_DS){
        if      (MID(CreateSoundBuffer)){
            uint32_t desc=A(c,1), pp=A(c,2);
            uint32_t b=com_new(IF_DSBUF,64);
            /* DSBUFFERDESC: dwSize@0 dwFlags@4 dwBufferBytes@8 dwReserved@12
             *               lpwfxFormat@16 guid3DAlgorithm@20
             * CAUTION: the size was read from +12 (dwReserved) -> it always came as 0
             * and fell back to 65536.  The game feeds the stream buffer based on the size
             * IT asked for; when our cursor lapped much faster the game pushed the song
             * fast and wrote silence where it couldn't keep up -- "the music runs fast
             * and cuts out now and then". */
            uint32_t bytes = (desc && mem_is_committed(desc+11)) ? MR32(desc+8) : 0;
            if (ENVC("SNDBUF"))
                fprintf(stderr, "  [sound] CreateSoundBuffer requested=%u bytes "
                        "(flags=%08x, old-wrong-field=%u)\n",
                        bytes, desc?MR32(desc+4):0, desc?MR32(desc+12):0);
            /* DSLOG=1: which guest subsystem created this buffer?  Since the SFX buffers
             * (#1-#4) were never played, finding their owner is a must. */
            if (ENVC("DSLOG")){
                fprintf(stderr, "  [sound] --- CreateSoundBuffer #%d call chain ---\n",
                        (int)g_snd_play);
                guest_backtrace(c, 10);
            }
            if(!bytes || bytes>0x400000u) bytes=65536u;
            MR32(b+4)=bytes; MR32(b+8)=gbuf_alloc(bytes); MR32(b+12)=0; g_gb_kept[1] += (bytes + 63u) & ~63u;
            /* Bytes/second from WAVEFORMATEX: the play cursor must advance REALISTICALLY.
             * The video player drives its frames from the audio clock; if the cursor is
             * fake, playback never ends. */
            { uint32_t bps = 176400u;                /* default 44.1k 16-bit stereo */
              uint32_t pfmt = (desc && mem_is_committed(desc+19)) ? MR32(desc+16) : 0;
              if (pfmt && mem_is_committed(pfmt+15)){
                  uint32_t v = MR32(pfmt+8);         /* nAvgBytesPerSec */
                  if (v >= 8000u && v <= 1000000u) bps = v;
              }
              MR32(b+20)=bps; MR32(b+24)=0;
              /* Register with the CoreAudio mixer.  The primary buffer
               * (DSBCAPS_PRIMARYBUFFER = 0x1) isn't played. */
              uint32_t flags = (desc && mem_is_committed(desc+7)) ? MR32(desc+4) : 0;
              uint32_t rate = 44100u, chans = 2u, bits = 16u;
              if (pfmt && mem_is_committed(pfmt+15)){
                  uint32_t ch = MR16(pfmt+2), sr = MR32(pfmt+4), bt = MR16(pfmt+14);
                  if (ch>=1u && ch<=2u) chans = ch;
                  if (sr>=4000u && sr<=192000u) rate = sr;
                  if (bt==8u || bt==16u) bits = bt;
              }
              MR32(b+28)=0;
              if (!(flags & 0x1u)){
                  audio_init();
                  MR32(b+28) = (uint32_t)audio_register(MR32(b+8), bytes, rate, chans, bits);
              } }
            if(pp) MR32(pp)=b; r=0; }
        else if (MID(GetCaps)){
            /* DSCAPS: dwSize@0 dwFlags@4 dwMinSecondarySampleRate@8
             *         dwMaxSecondarySampleRate@12 dwPrimaryBuffers@16
             *         dwMaxHwMixingAllBuffers@20 ...
             * The flags used to be only 0x200 (SECONDARYSTEREO) and 100000 was written
             * to dwPrimaryBuffers.  The game learns the sound capabilities from here. */
            uint32_t p=A(c,1);
            if(p){ uint32_t sz=MR32(p); if(!sz||sz>256u) sz=0x64u; mem_commit(p,sz);
                   memset(g_mem+p,0,sz); MR32(p)=sz;
                   MR32(p+4)=0x00000F1Fu;   /* primary mono/stereo/8/16 +
                                             * secondary mono/stereo/8/16 +
                                             * CONTINUOUSRATE */
                   MR32(p+8)=100u; MR32(p+12)=100000u;
                   MR32(p+16)=1u;           /* dwPrimaryBuffers */
                   MR32(p+20)=64; MR32(p+24)=64;
                   MR32(p+28)=64; MR32(p+32)=64;
                   MR32(p+36)=0x7FFFFFFFu;  /* dwTotalHwMemBytes  */
                   MR32(p+40)=0x7FFFFFFFu;  /* dwFreeHwMemBytes   */
                   MR32(p+44)=0x7FFFFFFFu;
                   MR32(p+48)=64; MR32(p+52)=64; MR32(p+56)=64; }
            r=0; }
        else if (MID(SetCooperativeLevel)) r=0;
        else if (MID(GetSpeakerConfig)){ uint32_t p=A(c,1); if(p){mem_commit(p,4);MR32(p)=0x00000006u;} r=0; }
        else if (MID(DuplicateSoundBuffer)){
            /* (this, pOriginal, ppDuplicate) - the duplicate shares the same PCM data.  It
             * used to return an EMPTY COM object: no size, no data, no sound in the mixer.
             * Since the game plays simultaneous effects (engine, menu clicks) this way,
             * ALL of them were silent. */
            uint32_t src=A(c,1), pp=A(c,2);
            uint32_t b=com_new(IF_DSBUF,64);
            if (src && mem_is_committed(src+31)){
                MR32(b+4)=MR32(src+4);          /* size    */
                MR32(b+8)=MR32(src+8);          /* veri    */
                MR32(b+20)=MR32(src+20);        /* bytes/s */
                MR32(b+12)=0; MR32(b+16)=0; MR32(b+24)=0;
                int audio_duplicate(int);
                MR32(b+28)=(uint32_t)audio_duplicate((int)MR32(src+28));
            }
            if(pp) MR32(pp)=b; r=0; }
        else r=0;
    }
    else if (iface==IF_DSBUF){
        uint32_t self=A(c,0);
        uint32_t sz = self?MR32(self+4):65536u, buf = self?MR32(self+8):0;
        if      (MID(Lock)){
            /* Which buffer object is being locked?  SFX events lock buffers but aren't
             * heard; they may be writing to an unregistered (primary or unreported)
             * buffer. */
            /* Which THREAD is locking?  The stream (music) is on a separate thread; SFX
             * events come from the MAIN thread.  Telling the two apart is the most direct
             * way to show whether SFX data is really written. */
            { uint32_t tid = (c->fs_base && mem_is_committed(c->fs_base+0x24u))
                             ? MR32(c->fs_base+0x24u) : 0u;
              static uint32_t tl[8]; static unsigned tc[8]; static uint64_t tb[8];
              int ti=-1; for(int q=0;q<8;q++) if(tl[q]==tid+1u) ti=q;
              if(ti<0) for(int q=0;q<8;q++) if(!tl[q]){ tl[q]=tid+1u; ti=q; break; }
              if(ti>=0){ tc[ti]++; tb[ti]+=A(c,2); }
              g_lk_tid=tl; g_lk_cnt=tc; g_lk_byt=tb; }
            { static uint32_t obj[16]; static unsigned cnt[16]; static int nobj=0;
              static int reg_atexit=0;
              int fi=-1; for(int q=0;q<nobj;q++) if(obj[q]==self) fi=q;
              if(fi<0 && nobj<16){ fi=nobj++; obj[fi]=self; cnt[fi]=0; }
              if(fi>=0) cnt[fi]++;
              (void)reg_atexit;
              g_lock_obj = obj; g_lock_cnt = cnt; g_lock_n = nobj; }
            if (ENVC("DSLOG")){ static int nl3=0;
                if (nl3 < 2){ nl3++;
                    fprintf(stderr,"  [sound] --- Lock call chain (buffer %u) ---\n",
                            self?MR32(self+28):0);
                    guest_backtrace(c, 12); } }
            /* (this, dwOffset, dwBytes, ppv1, pcb1, ppv2, pcb2, dwFlags)
             *
             * A DirectSound buffer is CIRCULAR: if the requested range goes past the end,
             * TWO regions are returned (up to the end + the continuation from the start).
             * A single flat region used to be given; the part to be written at the start
             * of the buffer was never written and every wrap made a silent gap. */
            uint32_t off=A(c,1), want=A(c,2), pp1=A(c,3), pn1=A(c,4),
                     pp2=A(c,5), pn2=A(c,6), fl=A(c,7);
            if(!buf){ buf=gbuf_alloc(sz?sz:65536u); if(self) MR32(self+8)=buf; g_gb_kept[1] += ((sz?sz:65536u) + 63u) & ~63u; }
            if(!sz) sz = 65536u;
            if (fl & 0x2u){ off = 0; want = sz; }        /* ENTIREBUFFER */
            off %= sz;
            if (!want || want > sz) want = sz;
            uint32_t avail = sz - off;
            uint32_t b1 = (want <= avail) ? want : avail;
            uint32_t b2 = want - b1;
            if (ENVC("SNDLOG")){
                static int nl=0;
                uint32_t sid2 = (self && mem_is_committed(self+31)) ? MR32(self+28) : 0;
                if (nl<40 && sid2){ nl++;
                    uint32_t rp=0, rw=0;
                    void audio_cursors(int, uint32_t*, uint32_t*);
                    audio_cursors((int)sid2, &rp, &rw);
                    fprintf(stderr,"  [snd] lock ofs=%6u size=%6u  |  play=%6u write=%6u"
                            "  buffer=%u  ahead=%d\n", off, want, rp, rw, sz,
                            (int)((off + sz - rp) % sz));
                }
            }
            if(pp1){ mem_commit(pp1,4); MR32(pp1)=buf+off; }
            if(pn1){ mem_commit(pn1,4); MR32(pn1)=b1; }
            if(pp2){ mem_commit(pp2,4); MR32(pp2)= b2 ? buf : 0u; }
            if(pn2){ mem_commit(pn2,4); MR32(pn2)= b2; }
            r=0; }
        else if (MID(Unlock)){
            /* VOICELOG=1: the game's OWN sound channel table.
             * sub_00725378 walks the table at address [0x8b466c], stride 0x90, count
             * [0x8b460c] (int16); it tests byte +0x26 as "in use".  The only way to
             * measure directly whether SFX events open channels. */
            if (ENVC("VOICELOG")){
                static unsigned nu=0; static int peak=0;
                if ((nu++ % 120u) == 0u){
                    uint32_t tb = mem_is_committed(GA(0x8b466cu)) ? MR32(GA(0x8b466cu)) : 0u;
                    int cnt = mem_is_committed(GA(0x8b460cu)) ? (int)(int16_t)MR16(GA(0x8b460cu)) : 0;
                    int act = 0;
                    if (tb && cnt > 0 && cnt < 256)
                        for (int q=0;q<cnt;q++){
                            uint32_t e = tb + (uint32_t)q*0x90u;
                            if (mem_is_committed(e+0x26u) && MR8(e+0x26u)) act++;
                        }
                    if (act > peak) peak = act;
                    /* Changes ACROSS the table: even if my interpretation (+0x26) is wrong, this
                     * shows whether any field gets written. */
                    unsigned nz = 0, ent_nz = 0;
                    if (tb && cnt > 0 && cnt < 256)
                        for (int q=0;q<cnt;q++){
                            uint32_t e = tb + (uint32_t)q*0x90u; unsigned any=0;
                            if (!mem_is_committed(e+0x8fu)) continue;
                            for (unsigned k=0;k<0x90u;k++) if (MR8(e+k)){ nz++; any=1; }
                            if (any) ent_nz++;
                        }
                    /* Volume globals: sub_0072d3ec scales the sound as
                     * ((uint16)[0x8b44c6] << 8) * param. */
                    fprintf(stderr,"  [volume] [0x8b44c6]=%u  [0x8b44c4]=%u  [0x8b44c8]=%u"
                            "  [0x8b44ca]=%u  [0x8b44cc]=%u\n",
                            mem_is_committed(GA(0x8b44c6u))?MR16(GA(0x8b44c6u)):0xFFFFu,
                            mem_is_committed(GA(0x8b44c4u))?MR16(GA(0x8b44c4u)):0xFFFFu,
                            mem_is_committed(GA(0x8b44c8u))?MR16(GA(0x8b44c8u)):0xFFFFu,
                            mem_is_committed(GA(0x8b44cau))?MR16(GA(0x8b44cau)):0xFFFFu,
                            mem_is_committed(GA(0x8b44ccu))?MR16(GA(0x8b44ccu)):0xFFFFu);
                    fprintf(stderr,"  [channel] table=0x%08x count=%d active=%d (peak=%d)"
                            "  non-zero entries=%u bytes=%u\n",
                            tb, cnt, act, peak, ent_nz, nz);
                }
            }
            /* Is the game writing real PCM to this buffer?  If so the sound engine (and so
             * the front-end logic) IS RUNNING. */
            uint32_t p1=A(c,1), n1=A(c,2);
            if (p1 && n1 && n1 < 1u<<22 && mem_is_committed(p1+ (n1>16?15:0))){
                uint32_t nz=0, lim = n1 < 4096u ? n1 : 4096u;
                for (uint32_t q=0;q<lim;q++) if (MR8(p1+q)) nz++;
                if (nz){ g_snd_bytes += n1; g_snd_nonzero++; }
                else      g_snd_zero++;
                { uint32_t sid4 = (self && mem_is_committed(self+31)) ? MR32(self+28) : 0;
                  if (sid4){ void audio_note_write_stat(int, uint32_t, int);
                             audio_note_write_stat((int)sid4, n1, nz ? 1 : 0); } }
                /* SNDZERO: report regions written ENTIRELY AS ZERO, together with the calling
                 * guest address.  Measured: the game WRITES ~50-95 ms of silence exactly
                 * every 30 s (the buffer isn't starved, the data is empty). */
                if (ENVC("SNDZERO")){
                    double audio_time(void);
                    double _t = audio_time();
                    /* The buffer clears at startup aren't interesting; the real event happens AFTER
                     * the first 20 s, while the 30 s stream block is refreshed.  We look at
                     * the whole region (not the first 4 KB): the second half of the write
                     * may be empty. */
                    if (_t > 20.0 && n1 >= 1024u){
                        uint32_t nz2 = 0, lim2 = n1 < 65536u ? n1 : 65536u;
                        for (uint32_t q = 0; q < lim2; q++) if (MR8(p1+q)) nz2++;
                        if (nz2 * 4u < lim2){          /* more than 75% zero */
                            static int nzl = 0;
                            if (nzl < 120){ nzl++;
                                fprintf(stderr, "  [zero-write] t=%.3f s  ofs=%u size=%u "
                                        "filled=%.1f%%  caller=guest:%08x\n",
                                        _t, p1 - (self?MR32(self+8):0), n1,
                                        100.0*nz2/lim2, c->eip);
                            }
                        }
                    }
                }
                /* Tell the mixer the write position (for the servo).
                 * CAUTION: the lock may have WRAPPED -- the second region (pv2,cb2)
                 * continues from the START of the buffer.  Looking only at the first
                 * region made us think the write cursor was at the end of the buffer on a
                 * wrap and fed the servo wrongly. */
                { uint32_t sid3 = (self && mem_is_committed(self+31)) ? MR32(self+28) : 0;
                  uint32_t base3 = self ? MR32(self+8) : 0;
                  uint32_t n2u   = A(c,4);
                  if (sid3 && base3 && p1 >= base3){
                      void audio_note_write(int, uint32_t);
                      void audio_note_region(int, uint32_t, uint32_t);   /* see audio.m STALE DATA GUARD */
                      audio_note_region((int)sid3, p1 - base3, n1);
                      if (n2u && n2u < 1u<<22) audio_note_region((int)sid3, 0u, n2u);
                      if (n2u && n2u < 1u<<22)
                          audio_note_write((int)sid3, n2u);          /* continue from the start */
                      else
                          audio_note_write((int)sid3, (p1 - base3) + n1);
                  } }
            }
            r=0; }
        else if (MID(Play)){
            if(self){ MR32(self+12)=1; uint64_t host_us(void);
                      MR32(self+24)=(uint32_t)(host_us()/1000ull);
                      /* (this, Reserved, Priority, Flags); DSBPLAY_LOOPING=1 */
                      if (MR32(self+28)) audio_play((int)MR32(self+28), (A(c,3) & 1u) ? 1 : 0); }
            g_snd_play++;
            if (ENVC("DSLOG")){
                fprintf(stderr, "  [sound] --- Play call chain (buffer %u) ---\n",
                        self?MR32(self+28):0);
                guest_backtrace(c, 10);
            }
            r=0; }
        else if (MID(Stop)){
            if(self){ MR32(self+12)=0; MR32(self+24)=0;
                      if (MR32(self+28)) audio_stop((int)MR32(self+28)); }
            r=0; }
        else if (MID(SetVolume)){
            if(self && MR32(self+28)) audio_set_volume((int)MR32(self+28), (long)(int32_t)A(c,1));
            r=0; }
        else if (MID(SetFrequency)){
            if(self && MR32(self+28)){ void audio_set_freq(int, uint32_t);
                                       audio_set_freq((int)MR32(self+28), A(c,1)); }
            r=0; }
        else if (MID(GetStatus)){
            uint32_t p=A(c,1); if(p){mem_commit(p,4); MR32(p)= (self&&MR32(self+12))?1u:0u;} r=0; }
        else if (MID(GetCurrentPosition)){
            int break_gcp = 0;
            /* The play cursor advances with ELAPSED TIME (not a fake fixed step): the video
             * player drives its frames from this clock. */
            uint32_t p1=A(c,1), p2=A(c,2);
            uint64_t host_us(void);
            uint32_t bps = (self && mem_is_committed(self+27)) ? MR32(self+20) : 176400u;
            if (!bps) bps = 176400u;
            uint32_t playing = self ? MR32(self+12) : 0;
            uint32_t t0 = self ? MR32(self+24) : 0;
            uint64_t now = host_us();
            if (playing && !t0){ t0 = (uint32_t)(now/1000ull); if(self) MR32(self+24)=t0; }
            uint32_t pos = 0;
            /* If registered with the mixer use the REAL play cursor: the game's streaming
             * logic chooses where it writes based on it; if the estimated clock and the
             * real playback diverge, the sound breaks. */
            { uint32_t sid = (self && mem_is_committed(self+31)) ? MR32(self+28) : 0;
              if (sid && playing){
                  uint32_t rp=0, rw=0;
                  void audio_cursors(int, uint32_t*, uint32_t*);
                  audio_cursors((int)sid, &rp, &rw);
                  if (p1){ mem_commit(p1,4); MR32(p1)=rp; }
                  if (p2){ mem_commit(p2,4); MR32(p2)=rw; }
                  if (self) MR32(self+16)=rp;
                  r=0; break_gcp = 1;
              } }
            if (!break_gcp && playing && t0){
                uint64_t ms = (now/1000ull) - (uint64_t)t0;
                uint64_t byt = ms * (uint64_t)bps / 1000ull;
                pos = sz ? (uint32_t)(byt % (uint64_t)sz) : 0;
            } else if (self) pos = MR32(self+16);
            if (!break_gcp){
                if(self) MR32(self+16)=pos;
                if(p1){mem_commit(p1,4); MR32(p1)=pos;}
                if(p2){mem_commit(p2,4); MR32(p2)=sz?((pos+2048u)%sz):0;}
            }
            r=0; }
        else if (MID(GetFormat)){
            uint32_t p=A(c,1), avail=A(c,2), pw=A(c,3);
            if(p && avail>=18u){ mem_commit(p,18); memset(g_mem+p,0,18);
                MR16(p)=1; MR16(p+2)=2; MR32(p+4)=44100; MR32(p+8)=44100*4;
                MR16(p+12)=4; MR16(p+14)=16; }
            if(pw){mem_commit(pw,4); MR32(pw)=18;}
            r=0; }
        else if (MID(GetCaps)){
            uint32_t p=A(c,1);
            if(p){ uint32_t s2=MR32(p); if(!s2||s2>256u) s2=16u; mem_commit(p,s2);
                   memset(g_mem+p,0,s2); MR32(p)=s2; MR32(p+8)=sz; }
            r=0; }
        else if (!strncmp(nm,"Get",3) && nargs>=2){
            uint32_t p=A(c,nargs-1);
            if(p && p<0xE0000000u){ mem_commit(p,8); memset(g_mem+p,0,8); }
            r=0; }
        else r=0;
    }
    /* --- DirectInput8 --- */
    else if (iface==IF_DI8){
        if      (MID(CreateDevice)){
            /* (rguid, lplpDirectInputDevice, pUnkOuter) */
            uint32_t g=A(c,1), pp=A(c,2); uint32_t d=com_new(IF_DIDEV,128);
            uint32_t kind = 0;
            if (g && mem_is_committed(g+15)){
                uint32_t d1 = MR32(g);
                if (d1 == 0x6F1D2B61u) kind = 1;        /* GUID_SysKeyboard */
                else if (d1 == 0x6F1D2B60u) kind = 2;   /* GUID_SysMouse    */
                else if (d1 == 0x6F1D2B70u) kind = 3;   /* host game pad    */
                fprintf(stderr,"  [DI] CreateDevice GUID %08x-%04x-%04x -> %s\n",
                        d1, MR16(g+4), MR16(g+6),
                        kind==1?"KEYBOARD":(kind==2?"MOUSE":(kind==3?"GAMEPAD":"unknown")));
            }
            MR32(d+8) = kind;
            if(pp) MR32(pp)=d; r=0; }
        else if (MID(EnumDevices)){
            /* (dwDevType, lpCallback, pvRef, dwFlags)
             * REALLY call the callback: report a keyboard and a mouse. */
            uint32_t devtype=A(c,1), cb=A(c,2), ref=A(c,3);
            if (cb && devtype == 4u){                 /* DI8DEVCLASS_GAMECTRL: the host pad */
                if (gamepad_present()){
                    uint32_t args[2] = { make_devinst(3), ref };
                    call_guest(c, cb, 2, args);
                    fprintf(stderr,"  [DI] game pad offered to the game: %s\n", gamepad_name());
                }
                g_di_enum++;
            }
            else if (cb && ENVC("DI_ENUM")){
                for (int k=0;k<2;k++){
                    int is_kb = (k==0);
                    uint32_t t = is_kb ? 0x13u : 0x12u;
                    /* 0 = DI8DEVCLASS_ALL -> all */
                    if (devtype && (devtype & 0xFFu) != t && devtype > 4u) continue;
                    uint32_t args[2] = { make_devinst(is_kb ? 1 : 2), ref };
                    if (call_guest(c, cb, 2, args) == 0) break;   /* DIENUM_STOP */
                }
                g_di_enum++;
            }
            r=0; }
        else if (MID(EnumDevicesBySemantics)){
            /* (ptszUserName, lpdiActionFormat, lpCallback, pvRef, dwFlags)
             * callback: (lpddi, lpdid, dwFlags, dwRemaining, pvRef) */
            uint32_t cb=A(c,3), ref=A(c,4);
            if (cb && !ENVC("DI_NO_SEM")){
                for (int k=0;k<2;k++){
                    int is_kb = (k==0);
                    uint32_t dev = com_new(IF_DIDEV,128);
                    MR32(dev+8) = is_kb ? 1u : 2u;     /* device type marker */
                    uint32_t args[5] = { make_devinst(is_kb ? 1 : 2), dev,
                                         0x00000001u,          /* DIEDBS_MAPPEDPRI1 */
                                         (uint32_t)(1-k), ref };
                    if (call_guest(c, cb, 5, args) == 0) break;
                }
                g_di_enum++;
            }
            r=0; }
        else if (MID(GetDeviceStatus)) r=0;
        else if (MID(Initialize))      r=0;
        else if (MID(FindDevice))      r=0x80070002u;
        else r=0;
    }
    else if (iface==IF_DIDEV){
        uint32_t self=A(c,0);
        uint32_t dkind = (self && mem_is_committed(self+11)) ? MR32(self+8) : 0;
        /* DITRACE=1: the call chain per device.  The game sets up the keyboard device but
         * never reads it; let's see where the chain breaks. */
        int di_tr = 0;
        { static int n2 = 0;
          if (ENVC("DITRACE") && n2 < 140){ n2++; di_tr = 1;
              fprintf(stderr,"  [DI%u/%08x] %-24s", dkind, self, nm); } }
        if (MID(EnumObjects)){
            /* (lpCallback, pvRef, dwFlags) -- callback:
             * BOOL(LPCDIDEVICEOBJECTINSTANCE, LPVOID)
             * If it isn't called the game sees the device as "objectless" and abandons it. */
            uint32_t cb=A(c,1), ref=A(c,2), flags=A(c,3);
            if (cb && dkind == 3u){                 /* game pad: 8 axes, 10 buttons, 1 POV */
                static const uint32_t axg[8] = {0xA36D02E0,0xA36D02E1,0xA36D02E2,0xA36D02F4,
                                                0xA36D02F5,0xA36D02E3,0xA36D02E4,0xA36D02E4};
                uint32_t oi = gbuf_alloc(340); g_gb_kept[3] += 384;
                for (int k = 0; k < 19; k++){
                    uint32_t type, ofs, g1;
                    if (k < 8){        type = 0x02u | ((uint32_t)k << 8);        ofs = 4u*k;  g1 = axg[k]; }
                    else if (k == 8){  type = 0x10u;                             ofs = 0x20;  g1 = 0xA36D02F2; }
                    else {             type = 0x04u | ((uint32_t)(k-9) << 8);    ofs = 0x30u + (uint32_t)(k-9); g1 = 0xA36D02F0; }
                    if (!(flags == 0u || (flags & type & 0xFFu))) continue;   /* DIDFT_ALL / filter */
                    memset(g_mem+oi, 0, 340);
                    MR32(oi) = 316u;
                    put_guid(oi+4, g1, 0xC9F3, 0x11CF, (const uint8_t[8]){0xBF,0xC7,0x44,0x45,0x53,0x54,0,0});
                    MR32(oi+20) = ofs;  MR32(oi+24) = type;  MR32(oi+28) = 0;
                    snprintf((char*)(g_mem+oi+32), 260, k < 8 ? "Axis %d" : (k == 8 ? "POV" : "Button %d"),
                             k < 8 ? k : k - 9);
                    uint32_t args[2] = { oi, ref };
                    if (call_guest(c, cb, 2, args) == 0) break;
                }
                g_di_objs++;
            }
            else if (cb && dkind == 1u){                 /* keyboard */
                uint32_t oi = gbuf_alloc(340); g_gb_kept[3] += 384;
                for (uint32_t dik = 1; dik < 0xEEu; dik++){
                    if (!(flags & 0x0Cu) && flags != 0u) break;   /* DIDFT_BUTTON/ALL */
                    memset(g_mem+oi, 0, 340);
                    MR32(oi) = 316u;                              /* dwSize */
                    put_guid(oi+4, 0x55728220u, 0xD33C, 0x11CF,
                             (const uint8_t[8]){0xBF,0xC7,0x44,0x45,0x53,0x54,0,0});
                    MR32(oi+20) = dik;                            /* dwOfs */
                    MR32(oi+24) = 0x04u | (dik << 8);             /* PSHBUTTON|inst */
                    MR32(oi+28) = 0;
                    snprintf((char*)(g_mem+oi+32), 260, "Key %u", dik);
                    uint32_t args[2] = { oi, ref };
                    if (call_guest(c, cb, 2, args) == 0) break;
                }
                g_di_objs++;
            }
            r=0; }
        else if (MID(GetObjectInfo)){
            /* (pdidoi, dwObj, dwHow) */
            uint32_t oi=A(c,1), obj=A(c,2);
            if (oi && mem_is_committed(oi+3)){
                uint32_t sz = MR32(oi); if (!sz || sz > 400u) sz = 316u;
                mem_commit(oi, sz); memset(g_mem+oi, 0, sz); MR32(oi)=sz;
                put_guid(oi+4, 0x55728220u, 0xD33C, 0x11CF,
                         (const uint8_t[8]){0xBF,0xC7,0x44,0x45,0x53,0x54,0,0});
                uint32_t dik = (obj >> 8) & 0xFFu; if (!dik) dik = obj & 0xFFu;
                MR32(oi+20) = dik;
                MR32(oi+24) = 0x04u | (dik << 8);
                snprintf((char*)(g_mem+oi+32), 260, "Key %u", dik);
            }
            r=0; }
        else if (MID(GetDeviceInfo)){
            uint32_t p=A(c,1);
            if (p && mem_is_committed(p+3)){
                uint32_t sz=MR32(p); if(!sz||sz>700u) sz=580u;
                uint32_t src = make_devinst(dkind ? (int)dkind : 1);
                mem_commit(p, sz);
                memcpy(g_mem+p, g_mem+src, sz < 580u ? sz : 580u);
                MR32(p)=sz;
            }
            r=0; }
        else
        if      (MID(SetDataFormat)){ if(self) MR32(self+4)=A(c,1); r=0; }
        else if (MID(SetCooperativeLevel)) r=0;
        else if (MID(Acquire)||MID(Unacquire)||MID(Poll)) r=0;
        else if (MID(GetDeviceState)){
            uint32_t cb=A(c,1), p=A(c,2);
            { static uint32_t seen_cb[8]; static int ns=0;
              int f2=0; for (int q=0;q<ns;q++) if (seen_cb[q]==cb) f2=1;
              if (!f2 && ns<8){ seen_cb[ns++]=cb;
                fprintf(stderr,"  [input] GetDeviceState size=%u (dataFormat=0x%08x device=%u)\n",
                        cb, self?MR32(self+4):0,
                        (self && mem_is_committed(self+11)) ? MR32(self+8) : 0); } }
            if(p && cb && cb<4096u){
                mem_commit(p,cb); memset(g_mem+p,0,cb);
                /* a 256-byte request = the DirectInput keyboard table */
                if (cb == 272u && dkind == 3u){             /* DIJOYSTATE2: host game pad */
                    gamepad_fill(g_mem+p);
                    g_dikey_calls++;
                }
                else if (cb == 20u && metal_available()){        /* DIMOUSESTATE2 */
                    metal_fill_mouse(g_mem+p, cb);
                    tunedrag(g_mem+p);
                    g_dikey_calls++;
                    for (uint32_t q=12;q<20;q++)
                        if (MR8(p+q)){ g_dikey_hits++; break; }
                }
                else if (cb == 16u && metal_available()){   /* DIMOUSESTATE */
                    unsigned char tmpm[20];
                    metal_fill_mouse(tmpm, 20);
                    tunedrag(tmpm);
                    memcpy(g_mem+p, tmpm, 16);
                    g_dikey_calls++;
                }
                else if (cb == 256u && metal_available()){
                    metal_fill_keyboard(g_mem+p, 256u);
                    g_dikey_calls++;
                    for (uint32_t q=0;q<256;q++)
                        if (MR8(p+q)){ g_dikey_hits++;
                            if (g_dikey_hits <= 3)
                                fprintf(stderr,"  [input] key 0x%02x pressed (call %llu)\n",
                                        q, (unsigned long long)g_dikey_calls);
                            break; }
                }
            }
            r=0; }
        else if (MID(BuildActionMap)||MID(SetActionMap)){
            /* DIACTIONFORMATA: +4 dwActionSize +12 dwNumActions +16 rgoAction
             * DIACTIONA (40): +0 uAppData +4 dwSemantic +32 dwObjID +36 dwHow */
            uint32_t af=A(c,1);
            int is_build = MID(BuildActionMap);
            if (!is_build){
                memset(g_act_of_key, 0, sizeof g_act_of_key);
                memset(g_act_ofs, 0, sizeof g_act_ofs);
                g_act_count = 0;
                if (ENVC("DIMAP")) fprintf(stderr,"  [map] --- new active map ---\n");
            }
            if (af && mem_is_committed(af+71)){
                uint32_t nact=MR32(af+12), rg=MR32(af+16), asz=MR32(af+4);
                if (!asz) asz=40u;
                { static int sl=0; if (sl<4){ sl++;
                    fprintf(stderr,"  [DI] %s: %u actions, dataSize=%u actionSize=%u buffer=%u device=%u\n",
                            nm, nact, MR32(af+8), asz, MR32(af+40),
                            (self && mem_is_committed(self+11)) ? MR32(self+8) : 0); } }
                if (nact && nact < 1024u && rg){
                    for (uint32_t k=0;k<nact;k++){
                        uint32_t a2=rg+k*asz;
                        if (!mem_is_committed(a2+39)) break;
                        uint32_t sem=MR32(a2+4), app=MR32(a2);
                        if ((sem & 0xFFFFFF00u) != DI_KB_SEM_BASE) continue;
                        uint32_t dik = sem & 0xFFu;
                        if (!dik) continue;
                        if (is_build){
                            /* DirectInput fills these fields:
                             * dwObjID = DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(dik) */
                            MR32(a2+32) = 0x04u | (dik << 8);
                            MR32(a2+36) = 0x20u;             /* DIAH_DEFAULT */
                            /* guidInstance = keyboard */
                            put_guid(a2+16, 0x6F1D2B61u, 0xD5A0, 0x11CF,
                                     (const uint8_t[8]){0xBF,0xC7,0x44,0x45,0x53,0x54,0,0});
                        } else {
                            g_act_of_key[dik] = app;
                            g_act_ofs[dik]    = k * 4u;      /* data offset */
                            g_act_count++;
                            if (ENVC("DIMAP"))
                                fprintf(stderr,"  [map] dik=0x%02x -> action 0x%x (ofs %u)\n",
                                        dik, app, k*4u);
                        }
                    }
                }
            }
            r=0; }
        else if (MID(GetDeviceData)){
            /* (cbObjectData, rgdod, pdwInOut, dwFlags)
             * DIDEVICEOBJECTDATA: +0 dwOfs +4 dwData +8 dwTimeStamp
             *                     +12 dwSequence +16 uAppData          */
            uint32_t cb=A(c,1), rg=A(c,2), pcnt=A(c,3), flags=A(c,4);
            uint32_t want = (pcnt && mem_is_committed(pcnt+3)) ? MR32(pcnt) : 0;
            uint32_t n = 0;
            { static int sg=0;
              if (ENVC("DIDATA") && sg<4){ sg++;
                fprintf(stderr,"  [DIdata] cb=%u requested=%u flags=0x%x action=%d\n",
                        cb, want, flags, g_act_count); } }
            if (g_act_count && metal_available() && cb >= 16u && cb <= 32u){
                unsigned char cur[256];
                metal_fill_keyboard(cur, 256);
                for (uint32_t k = 1; k < 256u && n < want; k++){
                    if (!g_act_of_key[k]) continue;
                    unsigned char now = cur[k] ? 1 : 0, was = g_kb_prev[k];
                    if (now == was) continue;
                    if (rg && !(flags & 1u)){          /* consume unless DIGDD_PEEK */
                        uint32_t e = rg + n*cb;
                        mem_commit(e, cb); memset(g_mem+e, 0, cb);
                        MR32(e)    = g_act_ofs[k];
                        MR32(e+4)  = now ? 0x80u : 0u;
                        MR32(e+8)  = (uint32_t)(host_us_ms());
                        MR32(e+12) = g_di_seq++;
                        if (cb >= 20u) MR32(e+16) = g_act_of_key[k];
                        { static int se=0;
                          if (ENVC("DIDATA") && se<6){ se++;
                            fprintf(stderr,"  [DIdata] event: dik=%u ofs=%u data=0x%x app=0x%x\n",
                                    k, g_act_ofs[k], now?0x80u:0u, g_act_of_key[k]); } }
                    }
                    if (!(flags & 1u)) g_kb_prev[k] = now;
                    n++; g_di_events++;
                }
            }
            if(pcnt) MR32(pcnt)=n; r=0; }
        else if (MID(GetCapabilities)){
            uint32_t p=A(c,1);
            if(p){ uint32_t sz=MR32(p); if(!sz||sz>256u) sz=44u; mem_commit(p,sz);
                   memset(g_mem+p,0,sz); MR32(p)=sz;
                   if (dkind == 3u){
                       MR32(p+4)=0x00000001u;  MR32(p+8)=0x00000215u;   /* attached, GAMEPAD */
                       MR32(p+12)=8; MR32(p+16)=10; MR32(p+20)=1;       /* axes, buttons, POVs */
                   } else {
                       MR32(p+4)=0x00000010u;   /* DIDC_ATTACHED */
                       MR32(p+8)=0x00000013u;   /* DI8DEVTYPE_KEYBOARD */
                       MR32(p+12)=256; } }      /* dwAxes/dwButtons approximate */
            r=0; }
        else if (MID(GetProperty)){
            uint32_t p=A(c,2);
            if(p){ uint32_t sz=MR32(p); if(sz && sz<=256u){ mem_commit(p,sz);
                   memset(g_mem+p+16,0,sz-16>0?sz-16:0); } }
            r=0; }
        else if (MID(SetProperty)) r=0;
        else if (MID(EnumObjects)) r=0;
        else if (MID(GetObjectInfo)||MID(GetDeviceInfo)){
            uint32_t p=A(c,nargs-1);
            if(p){ uint32_t sz=MR32(p); if(!sz||sz>512u) sz=32u; mem_commit(p,sz);
                   memset(g_mem+p,0,sz); MR32(p)=sz; }
            r=0; }
        else if (MID(SetEventNotification)) r=0;
        else if (!strncmp(nm,"Get",3) && nargs>=2){
            uint32_t p=A(c,nargs-1);
            if(p && p<0xE0000000u){ mem_commit(p,32); memset(g_mem+p,0,32); }
            r=0; }
        else r=0;
        if (di_tr) fprintf(stderr, " -> 0x%08x\n", r);
    }
    else if (iface==IF_QUERY){
        if      (MID(GetDevice)){ uint32_t p=A(c,1); if(p) MR32(p)=g_dev; r=0; }
        else if (MID(GetDataSize)) r=4;
        else if (MID(GetData)){ uint32_t p=A(c,1); if(p){mem_commit(p,4);MR32(p)=0;} r=0; }
        else r=0;
    }
    else if (iface==IF_SWAPCHAIN){
        if      (MID(GetDevice)){ uint32_t p=A(c,1); if(p) MR32(p)=g_dev; r=0; }
        else if (MID(GetBackBuffer)){ uint32_t pp=A(c,3); if(pp) MR32(pp)=com_new(IF_SURF,64); r=0; }
        else if (MID(Present)){ g_present++; r=0; }
        else if (!strncmp(nm,"Get",3) && nargs>=2){ uint32_t p=A(c,nargs-1);
            if(p && p<0xE0000000u){ mem_commit(p,64); memset(g_mem+p,0,64);} r=0; }
        else r=0;
    }
    else if (iface==IF_VSHADER || iface==IF_VDECL || iface==IF_STATEBLOCK || iface==IF_SIMPLE){
        if (iface==IF_STATEBLOCK && MID(Apply)){
            uint32_t self5=A(c,0);
            int bi = (self5 && mem_is_committed(self5+11)) ? (int)MR32(self5+8) : 0;
            if (bi > 0 && bi < MAXSB && g_sb[bi].used){
                SBlock *B = &g_sb[bi];
                const uint32_t *save_ovr = g_arg_ovr;
                int save_rep = g_sb_replay, save_rec = g_sb_rec;
                g_sb_replay = 1; g_sb_rec = 0;
                for (int k = 0; k < B->n; k++){
                    g_arg_ovr = B->r[k].a;
                    com_call(c, B->r[k].magic);
                }
                g_arg_ovr = save_ovr; g_sb_replay = save_rep; g_sb_rec = save_rec;
                g_sb_applied++;
            }
            r=0; }
        else if (iface==IF_STATEBLOCK && MID(Capture)){
            /* Rewrite the CURRENT value of every state in the block.  This is how the game
             * builds its save/restore idiom: record once, Capture before changing, then
             * Apply. */
            uint32_t self6=A(c,0);
            int bi = (self6 && mem_is_committed(self6+11)) ? (int)MR32(self6+8) : 0;
            if (bi > 0 && bi < MAXSB && g_sb[bi].used){
                SBlock *B = &g_sb[bi];
                for (int k = 0; k < B->n; k++){
                    SBRec *R = &B->r[k];
                    uint32_t mm = R->magic & 0xFFFFu;
                    uint32_t ifc = (R->magic>>16) & 0x1FFFu;
                    if (ifc >= NEL(g_if) || mm >= g_if[ifc].n) continue;
                    /* a cached numeric id instead of the method name: D3DX Begin captures 5 blocks per
                     * effect, the name comparison was a hot spot */
                    int md;
                    if (ifc < MIDIF && mm < MIDM){
                        md = g_midc[ifc][mm];
                        if (!md) md = g_midc[ifc][mm] = (int16_t)mid_resolve(g_if[ifc].m[mm].name);
                    } else md = mid_resolve(g_if[ifc].m[mm].name);
                    if      (md == MID_SetRenderState){
                        if (R->a[1] < 256u) R->a[2] = g_rs[R->a[1]]; }
                    else if (md == MID_SetTextureStageState){
                        if (R->a[1] < 8u && R->a[2] < 40u) R->a[3] = g_tss_raw[R->a[1]][R->a[2]]; }
                    else if (md == MID_SetSamplerState){
                        if (R->a[1] < 16u && R->a[2] < 20u) R->a[3] = g_smp_raw[R->a[1]][R->a[2]]; }
                    else if (md == MID_SetTexture){
                        if (R->a[1] < 8u) R->a[2] = g_raw_tex[R->a[1]]; }
                    else if (md == MID_SetVertexShader)      R->a[1] = g_raw_vs;
                    else if (md == MID_SetPixelShader)       R->a[1] = g_raw_ps;
                    else if (md == MID_SetVertexDeclaration) R->a[1] = g_raw_decl;
                    else if (md == MID_SetFVF)               R->a[1] = g_cur_fvf;
                }
                g_sb_captured++;
            }
            r=0; }
        else if (MID(GetDevice)){ uint32_t p=A(c,1); if(p) MR32(p)=g_dev; r=0; }
        else if (MID(GetFunction)||MID(GetDeclaration)){
            uint32_t self2=A(c,0), pdata=A(c,1), psize=A(c,2);
            uint32_t src = self2?MR32(self2+4):0, sz = self2?MR32(self2+8):0;
            if (pdata && src && sz){ mem_commit(pdata,sz); memcpy(g_mem+pdata, g_mem+src, sz); }
            if (psize){ mem_commit(psize,4); MR32(psize)=sz; }
            r=0; }
        else r=0;
    }
    else r=0;

    if (g_com_trace && !MID(Present) && strncmp(nm,"Draw",4)
        && !MID(SetRenderState) && !MID(SetTextureStageState))
        fprintf(stderr,"  [COM] %s::%-28s (%d arg) -> 0x%08x%s\n",
                g_if[iface].name, nm, nargs, r, impl?"":" [MISSING]");
    if(!impl) g_com_unimpl++;
    c->eax=r;
    if (!g_sb_replay) c->esp += 4u*(uint32_t)nargs;
    return 1;
}

static uint32_t g_ds = 0;
/* DirectSoundCreate(lpGuid, ppDS, pUnkOuter) */
uint32_t com_directsoundcreate(uint32_t ppDS){
    if (!g_ds){ g_ds = com_new(IF_DS, 64);
        fprintf(stderr,"com: IDirectSound object 0x%08x\n", g_ds); }
    if (ppDS) MR32(ppDS) = g_ds;
    return 0;
}

static uint32_t g_di8 = 0;
/* DirectInput8Create(hinst, dwVersion, riidltf, ppvOut, punkOuter) */
uint32_t com_directinput8create(uint32_t ppvOut){
    if (!g_di8){ g_di8 = com_new(IF_DI8, 64);
        fprintf(stderr,"com: IDirectInput8 object 0x%08x\n", g_di8); }
    if (ppvOut) MR32(ppvOut) = g_di8;
    return 0;                       /* DI_OK */
}

uint32_t com_direct3dcreate9(void){
    if(!g_d3d9){ g_d3d9=com_new(IF_D3D9,64);
        fprintf(stderr,"com: IDirect3D9 object 0x%08x\n", g_d3d9); }
    return g_d3d9;
}
void com_init(void){ mem_commit(COM_BASE,0x10000);
    /* GBUF_SKIP_MB=<n>: start the buffer heap n MB higher (test of the top of the area) */
    { const char *e = getenv("GBUF_SKIP_MB"); if (e && *e) gbuf_ptr += (uint32_t)atoi(e) << 20; }
    { extern char **environ; (void)environ; }
}
static uint64_t *g_ptype = 0, *g_stride = 0;
void draw_stats_save(uint64_t *a, uint64_t *b){ g_ptype=a; g_stride=b; }

void com_report(void){
    fprintf(stderr,"\n=== COM / D3D9 KATMANI ===\n");
    { void ws_report(void); ws_report(); }   /* wsfov.c */
    { void gamepad_report(void); gamepad_report(); }   /* gamepad.m */
    { void xpatch_report(void); xpatch_report(); }
    if (g_aspfix != 1.f)
        { fprintf(stderr,"  widescreen       : aspfix %.4f, %lu perspective / %lu orthographic draws\n",
                  g_aspfix, g_asp_persp, g_asp_ortho);
          for (unsigned v2 = 0; v2 < MAXVS; v2++)
              if (g_asp_np[v2] || g_asp_no[v2])
                  fprintf(stderr,"     vs=%-3u cx=%-3d cw=%-3d  persp %8lu / orto %8lu  c[cw].w=%.3f\n",
                          v2, g_pos_cx[v2], g_pos_cw[v2], g_asp_np[v2], g_asp_no[v2], g_asp_cw3[v2]); }
    if (ENVC("RSLOG")){
        /* the states rs_apply() really uses */
        static const unsigned handled[] = {7,14,15,19,20,22,23,24,25,27,168,171,193};
        fprintf(stderr,"  --- SetRenderState usage (UNHANDLED ones starred) ---\n");
        for (unsigned i=0;i<256;i++){
            if (!g_rs_hits[i]) continue;
            int h=0; for (unsigned k=0;k<sizeof handled/sizeof*handled;k++) if(handled[k]==i) h=1;
            fprintf(stderr,"   %c st=%-3u %8u times  values:", h?' ':'*', i, g_rs_hits[i]);
            for (int k=0;k<g_rs_nval[i];k++){
                float f; memcpy(&f,&g_rs_vals[i][k],4);
                fprintf(stderr," 0x%x(%g)", g_rs_vals[i][k], f);
            }
            fprintf(stderr,"%s\n", g_rs_nval[i]>=4?" ...":"");
        }
    }
    fprintf(stderr,"  SetRenderState    : %llu applied, %llu SKIPPED (%%%.1f were redundant)\n",
            (unsigned long long)g_rs_run, (unsigned long long)g_rs_skip,
            100.0*(double)g_rs_skip/(double)((g_rs_skip+g_rs_run) ? (g_rs_skip+g_rs_run) : 1));
    fprintf(stderr,"  total COM calls    : %llu\n",(unsigned long long)g_com_calls);
    fprintf(stderr,"  Present (frames)   : %llu\n",(unsigned long long)g_present);
    if (metal_available())
        fprintf(stderr,"  Metal presentation : %ld frames, %ld triangles\n"
                       "  Metal textures     : %ld created, %ld uploads\n",
                metal_frames(), metal_tris(),
                metal_tex_created(), metal_tex_uploaded());
    fprintf(stderr,"  Draw* calls        : %llu\n",(unsigned long long)g_draw);
    fprintf(stderr,"  ps constant file   : highest used c%u, %u writes above 32\n",
            g_psc_max ? g_psc_max-1u : 0u, g_psc_drop);
    fprintf(stderr,"  guest COM heap     : %u KB\n",(com_ptr-COM_BASE)/1024u);
    fprintf(stderr,"  guest buffer heap  : %u KB\n",(gbuf_ptr-GBUF_BASE)/1024u);
    if (g_ptype){
        static const char *PT[] = {"?","POINTLIST","LINELIST","LINESTRIP",
                                   "TRIANGLELIST","TRIANGLESTRIP","TRIANGLEFAN","?"};
        fprintf(stderr,"  --- DrawPrimitiveUP: primitive types ---\n");
        for (int i=0;i<8;i++) if (g_ptype[i])
            fprintf(stderr,"    %-16s %llu\n", PT[i], (unsigned long long)g_ptype[i]);
        fprintf(stderr,"  --- vertex stride sizes ---\n");
        for (int i=0;i<64;i++) if (g_stride[i])
            fprintf(stderr,"    %3d bytes  %llu\n", i*4, (unsigned long long)g_stride[i]);
    }
    { char gb[192]; com_gbuf_fmt(gb, sizeof gb);
      fprintf(stderr,"  buffer heap       : top %s; %llu freed (%llu KB), %llu reused (%llu split), %llu bad frees\n",
              gb, (unsigned long long)g_gb_freed, (unsigned long long)(g_gb_bytes_freed/1024u),
              (unsigned long long)g_gb_reused, (unsigned long long)g_gb_split, (unsigned long long)g_gb_bad);
      fprintf(stderr,"  buffer heap kept  : never freed: shader/decl copies %llu KB, sound buffers %llu KB, "
                     "surface lock buffers %llu KB, other %llu KB\n",
              (unsigned long long)(g_gb_kept[0]>>10), (unsigned long long)(g_gb_kept[1]>>10),
              (unsigned long long)(g_gb_kept[2]>>10), (unsigned long long)(g_gb_kept[3]>>10));
      fprintf(stderr,"  refcounts         : %u live objects; %llu last releases (data freed), %llu releases kept "
                     "(still referenced, %llu with data), %llu uncounted\n",
              g_rc_n, (unsigned long long)g_rc_last, (unsigned long long)g_rc_kept,
              (unsigned long long)g_rc_kept_data, (unsigned long long)g_rc_unknown); }
    fprintf(stderr,"  --- DirectInput / DirectSound calls ---\n");
    { int any=0;
      for (unsigned i=1;i<NEL(g_if) && i<MSTAT_IF;i++){
        if (i!=IF_DI8 && i!=IF_DIDEV && i!=IF_DS && i!=IF_DSBUF && i!=IF_QUERY) continue;
        for (unsigned j=0;j<g_if[i].n && j<MSTAT_M;j++)
            if (g_mstat[i][j]){ any=1;
                fprintf(stderr,"    %-26s %-26s %llu\n",
                        g_if[i].name, g_if[i].m[j].name,
                        (unsigned long long)g_mstat[i][j]); }
      }
      if(!any) fprintf(stderr,"    (no calls at all)\n");
    }
    fprintf(stderr,"  vertex shader     : %llu draws with shader, %llu without, %u compiled\n",
            (unsigned long long)g_vs_draws, (unsigned long long)g_ff_draws, g_vsprogn-1);
    fprintf(stderr,"  state blocks      : %d slots, %llu Apply, %llu Capture, "
            "%llu record overflows, %llu slots not found\n",
            g_sbn-1, (unsigned long long)g_sb_applied,
            (unsigned long long)g_sb_captured, (unsigned long long)g_sb_dropped,
            (unsigned long long)g_sb_full);
    fprintf(stderr,"  GPU vertex shader: %llu draws on the GPU, %llu translations rejected,"
            " %d/%d (shader,decl) pairs registered\n",
            (unsigned long long)g_gpuvs_draws, (unsigned long long)g_gpuvs_fail,
            g_gvs_next-1, g_gvsn);
    fprintf(stderr,"  indexed draws      : %llu from the pool, old path: strip/fan %llu, object %llu, new lock %llu, "
            "alignment/bound %llu, mirror %llu, out of index %llu\n",
            (unsigned long long)g_dip_ix, (unsigned long long)g_dip_ixfb[0], (unsigned long long)g_dip_ixfb[1],
            (unsigned long long)g_dip_ixfb[2], (unsigned long long)g_dip_ixfb[3], (unsigned long long)g_dip_ixfb[4],
            (unsigned long long)g_dip_ixfb[5]);
    { unsigned ovrv=0, ovkb=0; void metal_raw_overflow(unsigned*,unsigned*);
      metal_raw_overflow(&ovrv,&ovkb);
      if (ovrv || ovkb || g_gpuvs_drop)
          fprintf(stderr,"  GPU vertex arena overflow : %u verts, %u constants, %llu draws went back to the CPU\n",
                  ovrv, ovkb, (unsigned long long)g_gpuvs_drop); }
    if (metal_available())
        fprintf(stderr,"  skipped frames    : %ld\n", metal_frames_dropped());
    { extern uint64_t g_icall_hits;
      if (ENVC("ICALL"))
          fprintf(stderr,"  ICALL hits        : %llu\n", (unsigned long long)g_icall_hits); }
    { const char *cp = ENVC("COVREPORT");
      if (cp){ void cov_report(const char*); cov_report(cp); } }
    { extern uint64_t g_timer_ticks;
      fprintf(stderr,"  timer ticks       : %llu\n", (unsigned long long)g_timer_ticks); }
    fprintf(stderr,"  draw matrix       : %llu valid, %llu broken\n",
            (unsigned long long)g_mat_ok,(unsigned long long)g_mat_bad);
    fprintf(stderr,"  DI action map     : %d keys bound, %llu events produced\n",
            g_act_count, (unsigned long long)g_di_events);
    fprintf(stderr,"  DI device enum    : %llu enumerations, %llu object enumerations\n",
            (unsigned long long)g_di_enum, (unsigned long long)g_di_objs);
    for (int q2 = 0; g_lk_tid && q2 < 8; q2++){
        if (!g_lk_tid[q2]) continue;
        fprintf(stderr,"  [lock] tid=%u  locks=%u  total=%.1f KB\n",
                g_lk_tid[q2]-1u, g_lk_cnt[q2], g_lk_byt[q2]/1024.0);
    }
    for (int q2 = 0; q2 < g_lock_n; q2++){
        uint32_t o2 = g_lock_obj[q2];
        fprintf(stderr,"  [lock] object 0x%08x  sound-id=%u  size=%u  locks=%u\n",
                o2, (o2 && mem_is_committed(o2+31)) ? MR32(o2+28) : 0u,
                (o2 && mem_is_committed(o2+7)) ? MR32(o2+4) : 0u, g_lock_cnt[q2]);
    }
    fprintf(stderr,"  sound buffers     : %llu non-empty writes, %llu empty writes, %.1f KB, %llu Play\n",
            (unsigned long long)g_snd_nonzero,(unsigned long long)g_snd_zero,
            g_snd_bytes/1024.0,(unsigned long long)g_snd_play);
    audio_report();
    fprintf(stderr,"  keyboard reads    : %llu calls, a key pressed in %llu of them\n",
            (unsigned long long)g_dikey_calls,(unsigned long long)g_dikey_hits);
    fprintf(stderr,"  RT textures created: %llu\n",(unsigned long long)g_rt_tex);
    { void metal_miplevel_report(void); if (metal_available()) metal_miplevel_report(); }
    fprintf(stderr,"  render target     : to a texture %llu, to the screen %llu\n",
            (unsigned long long)g_rt_set,(unsigned long long)g_rt_screen);
    fprintf(stderr,"  SetTexture        : valid %llu, NULL %llu, id=0 %llu, stage!=0 %llu\n",
            (unsigned long long)g_st_ok,(unsigned long long)g_st_null,
            (unsigned long long)g_st_zero,(unsigned long long)g_st_other);
    fprintf(stderr,"  texture creation  : %llu failed\n", (unsigned long long)g_tex_fail);
    fprintf(stderr,"  of which in vs_run: %.2f s\n", g_t_vs_ns/1e9);
    fprintf(stderr,"  --- VERTICES PER SHADER ---\n");
    for (int q=0;q<8;q++){
        uint64_t best=0; int bi=-1;
        for (int w=0;w<64;w++) if (g_v_per_vs[w]>best){ best=g_v_per_vs[w]; bi=w; }
        if (bi<0) break;
        fprintf(stderr,"     vs%-3d %12llu verts (%.1f%%)  %u instructions\n", bi,
                (unsigned long long)best,
                g_v_xform ? 100.0*(double)best/(double)g_v_xform : 0.0,
                (bi>0 && (unsigned)bi<g_vsprogn && g_vsprog[bi]) ? vs_ninstr(g_vsprog[bi]) : 0u);
        g_v_per_vs[bi]=0;
    }
    fprintf(stderr,"  draw path time    : %.2f s (with DRAWTIME=1), %llu vertices transformed (%.1f M verts/s)\n",
            g_t_draw_ns/1e9, (unsigned long long)g_v_xform,
            g_t_draw_ns ? (double)g_v_xform/(g_t_draw_ns/1000.0) : 0.0);
    fprintf(stderr,"  bound texture stages: 1: %llu, 2: %llu, 3: %llu, 4: %llu draws\n",
            (unsigned long long)g_stage_hist[0],(unsigned long long)g_stage_hist[1],
            (unsigned long long)g_stage_hist[2],(unsigned long long)g_stage_hist[3]);
    fprintf(stderr,"  indexed draws    : %llu draws, %llu triangles, %llu skipped\n",
            (unsigned long long)g_dip_draws,(unsigned long long)g_dip_tris,
            (unsigned long long)g_dip_skip);
    fprintf(stderr,"  raw position range: x[%.4g..%.4g] y[%.4g..%.4g] z[%.4g..%.4g]\n",
            g_rmin[0],g_rmax[0],g_rmin[1],g_rmax[1],g_rmin[2],g_rmax[2]);
    fprintf(stderr,"  vertex position   : on screen %llu, off %llu, w<=0 %llu\n",
            (unsigned long long)g_v_in, (unsigned long long)g_v_out,
            (unsigned long long)g_v_badw);
    for (unsigned q=1; q<g_vsprogn && q<6; q++)
        fprintf(stderr,"     vs%u: version %u.%u, %u instructions, %u unknown\n", q,
                vs_version(g_vsprog[q])>>8, vs_version(g_vsprog[q])&0xFF,
                vs_ninstr(g_vsprog[q]), vs_unknown(g_vsprog[q]));
    fprintf(stderr,"  --- MOST CALLED D3D9/COM METHODS ---\n");
    for (int k=0;k<120;k++){
        uint64_t best=0; int bi=0,bm=0;
        for (unsigned i=1;i<NEL(g_if) && i<MSTAT_IF;i++)
            for (unsigned j=0;j<g_if[i].n && j<MSTAT_M;j++)
                if (g_mstat[i][j]>best){ best=g_mstat[i][j]; bi=(int)i; bm=(int)j; }
        if (!best) break;
        fprintf(stderr,"    %-26s %-28s %llu\n",
                g_if[bi].name, g_if[bi].m[bm].name, (unsigned long long)best);
        g_mstat[bi][bm]=0;
    }
}

static void fill_caps(uint32_t p){
    mem_commit(p,0x150); memset(g_mem+p,0,0x150);
    MR32(p+0x00)=1; MR32(p+0x08)=0x00020000; MR32(p+0x0C)=0x00000002;
    MR32(p+0x10)=0x00000020; MR32(p+0x14)=0x80000001; MR32(p+0x18)=1;
    MR32(p+0x1C)=0x001BBEF0; MR32(p+0x20)=0x0000033F; MR32(p+0x24)=0x0F730191;
    MR32(p+0x28)=0x000000FF; MR32(p+0x2C)=0x00003FFF; MR32(p+0x30)=0x00003FFF;
    MR32(p+0x34)=0x000000FF; MR32(p+0x38)=0x0008FFFF; MR32(p+0x3C)=0x0021EDA5;
    MR32(p+0x40)=0x07030700; MR32(p+0x44)=0x07030700; MR32(p+0x48)=0x07030700;
    MR32(p+0x4C)=0x0000003F; MR32(p+0x50)=0x0000003F; MR32(p+0x54)=0x0000001F;
    MR32(p+0x58)=4096; MR32(p+0x5C)=4096; MR32(p+0x60)=256; MR32(p+0x64)=8192;
    MR32(p+0x68)=4096; MR32(p+0x6C)=16;
    MRF32(p+0x70)=1.0e10f; MRF32(p+0x74)=-32768.0f; MRF32(p+0x78)=-32768.0f;
    MRF32(p+0x7C)=32768.0f; MRF32(p+0x80)=32768.0f; MRF32(p+0x84)=0.0f;
    MR32(p+0x88)=0x000001FF; MR32(p+0x8C)=0x00100008; MR32(p+0x90)=0x03FFFFFF;
    MR32(p+0x94)=8; MR32(p+0x98)=8; MR32(p+0x9C)=0x0000013B;
    MR32(p+0xA0)=8; MR32(p+0xA4)=6; MR32(p+0xA8)=4; MR32(p+0xAC)=8;
    MRF32(p+0xB0)=256.0f; MR32(p+0xB4)=1048575; MR32(p+0xB8)=1048575;
    MR32(p+0xBC)=16; MR32(p+0xC0)=508;
    { /* The shader version can be set with an environment variable (SM=0 -> fixed function) */
      const char *sm = ENVC("D3D_SM");
      uint32_t vs=0xFFFE0200u, ps=0xFFFF0200u;
      if (sm){ int v=atoi(sm);
        if (v==0){ vs=0; ps=0; }
        else if (v==11){ vs=0xFFFE0101u; ps=0xFFFF0101u; }
        else if (v==14){ vs=0xFFFE0101u; ps=0xFFFF0104u; }
      }
      MR32(p+0xC4)=vs; MR32(p+0xC8)=256; MR32(p+0xCC)=ps; }
    MRF32(p+0xD0)=8.0f; MR32(p+0xD4)=0x00000051;
}
