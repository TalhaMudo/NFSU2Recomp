/* Runtime wrappers for lifted functions.
 *
 * The D3DX effect parameter tree is parsed as a CYCLE (node 0 -> 6 -> 4..8 -> 6)
 * and one more than the node array capacity (8) is requested (index 8).
 * The root cause is still open (see PROJECT.md). There are two guards here:
 *   1) array bounds: return safely on an index beyond the capacity
 *   2) visited set: every node at most once per top-level walk
 * This way the walk terminates and the game can progress.                 */
#include "cpu.h"
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdatomic.h>
#include "enhance.h"

void guest_backtrace(CPU*,int);

uint64_t g_cycle_break = 0, g_oob_node = 0, g_depth_guard = 0;
static uint64_t guard_total = 0, guard_cap = 0;
static CPU *g_guard_cpu = 0;

#define HEAP_LO 0x10000000u
#define HEAP_HI 0x2F000000u

/* Find the heap block containing a guest address (header: +0 size, +4 signature) */
static int block_of(uint32_t addr, uint32_t *ubeg, uint32_t *uend){
    if (addr < HEAP_LO || addr >= HEAP_HI) return 0;
    for (uint32_t p = (addr & ~15u); p >= HEAP_LO; p -= 16u){
        if (!mem_is_committed(p+4)) return 0;
        if (MR32(p+4) == 0xA110C8EDu){
            uint32_t sz = MR32(p), beg = p + 16u, end = beg + sz;
            if (addr >= beg && addr < end){ *ubeg = beg; *uend = end; return 1; }
            return 0;
        }
        if (p < 16u) break;
    }
    return 0;
}

static void guard_hit(void){
    if (!guard_cap){ const char *e = getenv("GUARD_CAP");
                     guard_cap = e ? (uint64_t)atoll(e) : 2000000ull; }
    if (guard_total == 5000 && g_guard_cpu && getenv("GUARD_BT")){
        fprintf(stderr,"\n=== call chain at the 5000th guard hit ===\n");
        guest_backtrace(g_guard_cpu, 14);
    }
    if (++guard_total > guard_cap){
        fprintf(stderr,"\n*** GUARD CEILING (%llu) - the effect system can't progress.\n",
                (unsigned long long)guard_cap);
        { extern uint32_t g_watch_addr; extern void watch_dump(const char*,int);
          if (g_watch_addr) watch_dump("guard ceiling", 20); }
        { extern void (*g_report_fn)(void); if (g_report_fn) g_report_fn(); }
        _exit(6);
    }
}

/* --- shared: node array bounds + visited check ----------------------- */
typedef struct { uint64_t bits[8]; uint32_t tab; } VisSet;   /* 512 nodes */

/* The tree guard is NOW OFF BY DEFAULT.
 * The cyclic parse actually came from CPU emulation bugs (x87 rounding, SSE
 * width, adc/sbb carry...).  Once those were fixed the walk terminates by
 * itself; the guard is only enabled with GUARD_ON=1, and then it also cuts
 * valid nodes and breaks the tree.                                        */
static int g_guard_off = -1;
static int guard_off(void){
    if (g_guard_off < 0) g_guard_off = getenv("GUARD_ON") ? 0 : 1;
    return g_guard_off;
}

static int node_guard(CPU *c, uint32_t a1, int depth, VisSet *vs){
    if (guard_off()) return 0;
    uint32_t tab = c->ecx + 0x2cu;
    if (!mem_is_committed(tab)) return 0;
    uint32_t arr = MR32(tab), beg, end;
    if (block_of(arr, &beg, &end)){
        uint32_t cap = (end - arr) / 0x64u;
        if (a1 >= cap){ g_oob_node++; guard_hit(); return 1; }
    }
    if (depth == 0){                       /* top level: reset the set */
        for (int i=0;i<8;i++) vs->bits[i]=0;
        vs->tab = arr;
    }
    if (arr == vs->tab && a1 < 512u){
        uint64_t *w = &vs->bits[a1 >> 6]; uint64_t b = 1ull << (a1 & 63);
        if (*w & b){ g_cycle_break++; guard_hit(); return 1; }
        *w |= b;
    }
    return 0;
}

/* --- sub_0066aa3f(a1, a2) : the main tree walker, ret 8, HRESULT ----- */
void sub_0066aa3f_orig(CPU *c);
static _Thread_local int d_aa3f = 0;
static _Thread_local VisSet vs_aa3f;

/* Dump the node array of the effect parameter tree (diagnostic).
 * Node: 0x64 bytes.  +0x04 array base, +0x18/+0x1a child node indices
 * (0xFFFF = none), +0x58 type tag.                                       */
static void dump_nodes(CPU *c){
    uint32_t tab = c->ecx + 0x2cu;
    if (!mem_is_committed(tab)) return;
    uint32_t arr = MR32(tab), beg, end;
    if (!block_of(arr, &beg, &end)){ fprintf(stderr,"  [tree] array 0x%08x outside any block\n", arr); return; }
    uint32_t cap = (end - arr) / 0x64u;
    fprintf(stderr,"  [tree] array=0x%08x capacity=%u (block %08x..%08x)\n", arr, cap, beg, end);
    /* raw dump: to understand the node structure */
    for (uint32_t i = 0; i < cap && i < 4; i++){
        uint32_t n = arr + i*0x64u;
        if (!mem_is_committed(n+0x63)) break;
        fprintf(stderr,"    node[%u] raw @%08x:\n", i, n);
        for (int rw = 0; rw < 0x64; rw += 16){
            fprintf(stderr,"      +%02x:", rw);
            for (int q = 0; q < 16 && rw+q < 0x64; q++)
                fprintf(stderr," %02x", MR8(n+rw+q));
            fprintf(stderr,"   ");
            for (int q = 0; q < 16 && rw+q < 0x64; q++){
                uint8_t ch = MR8(n+rw+q);
                fputc((ch>=32&&ch<127)?ch:'.', stderr);
            }
            fprintf(stderr,"\n");
        }
        /* where do +0 and +4 point? */
        for (int f = 0; f <= 4; f += 4){
            uint32_t ptr = MR32(n+f);
            if (ptr && mem_is_committed(ptr+31)){
                fprintf(stderr,"      [+%d]->%08x:", f, ptr);
                for (int q=0;q<24;q++) fprintf(stderr," %02x", MR8(ptr+q));
                fprintf(stderr,"   ");
                for (int q=0;q<24;q++){ uint8_t ch=MR8(ptr+q);
                    fputc((ch>=32&&ch<127)?ch:'.', stderr); }
                fprintf(stderr,"\n");
            }
        }
    }
    for (uint32_t i = 0; i < cap && i < 64; i++){
        uint32_t n = arr + i*0x64u;
        if (!mem_is_committed(n+0x5C)) break;
        uint32_t nameptr = MR32(n);
        char nm[40]; nm[0]=0;
        if (nameptr && mem_is_committed(nameptr)){
            for (int k=0;k<32;k++){ uint8_t ch=MR8(nameptr+k);
                if (!ch || ch<32 || ch>126){ nm[k]=0; break; } nm[k]=(char)ch; nm[k+1]=0; }
        }
        fprintf(stderr,"    [%2u] name=0x%08x '%s' base=0x%08x ch18=%5u ch1a=%5u type=0x%08x\n",
                i, nameptr, nm, MR32(n+4),
                MR16(n+0x18), MR16(n+0x1a), MR32(n+0x58));
    }
}

/* Recursion chain: (node index, array base) pairs.
 * Dumped when the depth threshold is exceeded, to see the cycle concretely. */
static _Thread_local uint32_t chain_idx[160], chain_arr[160];
static int chain_dumped = 0;

void sub_0066aa3f(CPU *c){
    uint32_t a1 = MR32(c->esp + 4u);
    g_guard_cpu = c;
    { static int dumped = 0;
      if (!dumped && getenv("TREEDUMP")){ dumped = 1; dump_nodes(c); } }
    if (getenv("WALKLOG")){
        static int nlog = 0;
        if (nlog < 60){ nlog++;
            uint32_t tabp = c->ecx + 0x2cu;
            uint32_t arr = mem_is_committed(tabp) ? MR32(tabp) : 0;
            uint32_t esi = arr + a1*0x64u;
            if (mem_is_committed(esi+0x35))
                fprintf(stderr,"    [walk] depth=%2d a1=%-5u node=%08x "
                        "ch18=%5u ch1a=%5u count=%u base=%08x\n",
                        d_aa3f, a1, esi, MR16(esi+0x18), MR16(esi+0x1a),
                        MR16(esi+0x34), MR32(esi+4));
            else
                fprintf(stderr,"    [walk] depth=%2d a1=%-5u node=%08x UNREACHABLE\n",
                        d_aa3f, a1, esi);
        }
    }
    if (d_aa3f < 160){ chain_idx[d_aa3f] = a1;
        chain_arr[d_aa3f] = mem_is_committed(c->ecx+0x2c) ? MR32(c->ecx+0x2c) : 0; }
    if (!chain_dumped && d_aa3f >= 40 && getenv("CHAIN")){
        chain_dumped = 1;
        fprintf(stderr,"  [chain] recursion depth %d, (index@array):\n", d_aa3f);
        for (int q = 0; q <= d_aa3f && q < 160; q++){
            fprintf(stderr," %u@%08x", chain_idx[q], chain_arr[q]);
            if ((q % 8) == 7) fprintf(stderr,"\n");
        }
        fprintf(stderr,"\n");
    }
    if (node_guard(c, a1, d_aa3f, &vs_aa3f)){
        POP32(c->eip); c->esp += 8u; c->eax = 0;   /* S_OK */
        return;
    }
    if (d_aa3f >= 128){ g_depth_guard++; guard_hit();
        POP32(c->eip); c->esp += 8u; c->eax = 0; return; }
    d_aa3f++;
    sub_0066aa3f_orig(c);
    d_aa3f--;
}

/* --- sub_0065ae35(a1, a2, a3) : helper walker, ret 0xc --------------- */
void sub_0065ae35_orig(CPU *c);
static _Thread_local int d_ae35 = 0;
static _Thread_local VisSet vs_ae35;

void sub_0065ae35(CPU *c){
    uint32_t a1 = MR32(c->esp + 4u);
    g_guard_cpu = c;
    if (node_guard(c, a1, d_ae35, &vs_ae35)){
        POP32(c->eip); c->esp += 12u; c->eax = 0;
        return;
    }
    if (d_ae35 >= 128){ g_depth_guard++; guard_hit();
        POP32(c->eip); c->esp += 12u; c->eax = 0; return; }
    d_ae35++;
    sub_0065ae35_orig(c);
    d_ae35--;
}

/* --- sub_006e2be0(list, b, c): walks the list and frees every item ---
 * The first argument can arrive as NULL (a case that doesn't happen on
 * Windows). The original code crashes because it doesn't check for null; we
 * return safely.                                                         */
void sub_006e2be0(CPU *c);                     /* the generated original */
uint64_t g_null_list = 0, g_e2be0_calls = 0;
static void wrap_6e2be0(CPU *c){
    uint32_t a1 = MR32(c->esp + 4u);
    g_e2be0_calls++;
    if (a1 == 0 || !mem_is_committed(a1 + 4u)){
        g_null_list++;
        POP32(c->eip);                          /* ret (cdecl: the caller cleans up) */
        return;
    }
    sub_006e2be0(c);
}


/* --- sub_006623ea: copies an effect parameter value into the shader constant block ---
 * __thiscall, 7 stack arguments.  Stack layout (caller 0x00669710):
 *   [esp+04] a1  -> *(a1)=source data, *(a1+4)=type descriptor
 *   [esp+08] a2  = destination (inside the constant block)
 *   [esp+0c] a3  = D3DXPARAMETER_TYPE (1=BOOL 2=INT 3=FLOAT)
 *   [esp+10] a4  = row count
 *   [esp+14] a5  = column count
 * Inputs/outputs are dumped to see why the WVP matrix was written incompletely. */
void sub_006623ea_orig(CPU *c);
uint64_t g_copy_calls = 0;

void sub_006623ea(CPU *c){
    uint32_t a1 = MR32(c->esp + 4u),  dst = MR32(c->esp + 8u);
    uint32_t ty = MR32(c->esp + 12u), rows = MR32(c->esp + 16u);
    uint32_t cols = MR32(c->esp + 20u);
    uint32_t src = (a1 && mem_is_committed(a1+7)) ? MR32(a1) : 0;
    uint32_t td  = (a1 && mem_is_committed(a1+7)) ? MR32(a1+4) : 0;
    uint32_t cls = (td && mem_is_committed(td+7)) ? MR32(td+4) : 0xFFFFFFFFu;
    g_copy_calls++;
    /* The FE effect's constant block is 80 bytes; only track copies that write to it */
    static int n = 0;
    int is80 = 0;
    { uint32_t b1, e1;
      if (block_of(dst, &b1, &e1) && (e1 - b1) == 80u) is80 = 1; }
    int show = 0;   /* to be decided AFTER the copy */
    (void)show;
    /* MPROTSRC=1: WHO writes to the FE effect's WVP value area?
     * Once the copy source is known we make the page read-only and catch it. */
    if (is80 && src && getenv("MPROTSRC")){
        static int armed3 = 0;
        uint32_t b2, e2;
        if (!armed3 && block_of(dst, &b2, &e2) && dst == b2){
            armed3 = 1;
            void mprot_arm(uint32_t);
            fprintf(stderr,"  [copy] WVP source %08x -- page protected\n", src);
            mprot_arm(src);
        }
    }
    sub_006623ea_orig(c);
    /* Only dump copies that write a MEANINGFUL value into the 80-byte FE block */
    if (getenv("COPYLOG") && is80 && n < 14 && dst && mem_is_committed(dst+63)){
        int meaningful = 0;
        for (int q=0;q<16;q++){
            float fv = MRF32(dst + 4*q);
            if (fv != 0.f && fabsf(fv) > 1e-6f && fabsf(fv) < 1e6f) meaningful = 1;
        }
        if (meaningful){ n++;
            uint32_t bb, ee; block_of(dst, &bb, &ee);
            fprintf(stderr,"  [copy %llu] a2=%u class=%d source=%08x dest=%08x"
                    " (block %08x offset %d)\n",
                    (unsigned long long)g_copy_calls, ty, (int)cls, src, dst,
                    bb, (int)(dst - bb));
            if (src && mem_is_committed(src+63)){
                fprintf(stderr,"      source:");
                for (int r2=0;r2<4;r2++)
                    fprintf(stderr," [%.4g %.4g %.4g %.4g]", MRF32(src+r2*16),
                            MRF32(src+r2*16+4), MRF32(src+r2*16+8), MRF32(src+r2*16+12));
                fprintf(stderr,"\n");
            }
            fprintf(stderr,"      dest  :");
            for (int r2=0;r2<4;r2++)
                fprintf(stderr," [%.4g %.4g %.4g %.4g]", MRF32(dst+r2*16),
                        MRF32(dst+r2*16+4), MRF32(dst+r2*16+8), MRF32(dst+r2*16+12));
            fprintf(stderr,"\n");
            guest_backtrace(c, 8);
        }
    }
}

/* --- Game region: match the region to the data's cars ---------------------
 * The territory decides which cars exist (sub_005022c0): territory 0 hides the
 * European-only cars listed at 0x7f7c48 (CORSA, PEUGOT106), any other territory
 * hides the North American-only ones at 0x7f7c50 (CIVIC, RSX).  A race that
 * picks a car whose folder is missing hangs on "Loading".
 *   European v1.2:      [0x7fa73c] = 6, a constant in .data (sub_005712b0 -> the
 *                       player's language, sub_00571390 -> the country)
 *   North American v1.2: the same variable is in .bss, so 0 (-> "EN", "US")
 * Data with only one region's cars (US discs: CIVIC + RSX; European release:
 * CORSA + PEUGOT106) gets that region; otherwise the executable's own region
 * stays.  NFSU2_REGION=na|eu forces it. */
#define GAME_REGION_ADDR GA(0x7fa73cu)
#define GAME_REGION_NA   0u
#define GAME_REGION_EU   6u
static void region_install(void){
    int fileio_game_file_exists(const char *win);
    uint32_t cur = MR32(GAME_REGION_ADDR);
    if (cur != GAME_REGION_EU && cur != GAME_REGION_NA){
        fprintf(stderr, "region: unexpected territory %u, left unchanged\n", cur);
        return;
    }
    const char *e = getenv("NFSU2_REGION");
    uint32_t want = cur;
    const char *why = "the executable's own";
    if (e && *e){
        if (!strcasecmp(e, "na") || !strcasecmp(e, "us")) want = GAME_REGION_NA;
        else if (!strcasecmp(e, "eu")) want = GAME_REGION_EU;
        else { fprintf(stderr, "region: NFSU2_REGION=%s not understood (na|eu)\n", e); return; }
        why = "forced by NFSU2_REGION";
    } else {
        int eu_cars = fileio_game_file_exists("CARS\\CORSA\\GEOMETRY.BIN")
                   && fileio_game_file_exists("CARS\\PEUGOT106\\GEOMETRY.BIN");
        int na_cars = fileio_game_file_exists("CARS\\CIVIC\\GEOMETRY.BIN")
                   && fileio_game_file_exists("CARS\\RSX\\GEOMETRY.BIN");
        if (na_cars && !eu_cars){ want = GAME_REGION_NA; why = "North American data"; }
        else if (eu_cars && !na_cars){ want = GAME_REGION_EU; why = "European data"; }
        else if (eu_cars && na_cars) why = "data with both regions' cars, the executable's own";
    }
    MR32(GAME_REGION_ADDR) = want;
    fprintf(stderr, "region: %s (territory %u -> %u, %s)\n",
            want == GAME_REGION_NA ? "North American" : "European", cur, want, why);
}

/* --- Car shadow height ------------------------------------------------------
 * The cars' shadow is laid 4 cm above the road (float [0x7a66bc] = 0.04, added to
 * the ground height at 0x6171cb / 0x622645).  That puts the shadow's edge into the
 * bottom of each tyre: the lower rim is darkened.  1.5 cm keeps it off the road
 * surface and out of the tyres.  The lifted code reads the float each time, so
 * one write at start is enough.  SHADOWZ=<cm> picks another height (4 = the
 * original game). */
static void shadow_install(void){
    uint32_t a = GA(0x7a66bcu);
    if (!mem_is_committed(a + 3u) || MR32(a) != 0x3d23d70au){          /* 0.04f */
        fprintf(stderr, "shadow: unexpected value at the shadow height, left unchanged\n");
        return;
    }
    float cm = 1.5f;
    const char *e = getenv("SHADOWZ");
    if (e && *e){ cm = (float)atof(e); if (!(cm >= 0.f && cm <= 20.f)) cm = 4.f; }
    if (cm == 4.f) return;
    MRF32(a) = cm / 100.f;
    fprintf(stderr, "shadow: cars' shadows %.1f cm above the road (game: 4)\n", (double)cm);
}

/* --- Sponsor screens: a Continue button for the mouse ---------------------------
 * The front end's screen table has a row per screen: package name, constructor,
 * 0, the help bar's buttons, ...  After a career race the reward screen offers
 * Continue + Back (0x404), but "Select a sponsor!" (UI_SponsorPopup.fng) and the
 * sponsor list (UI_Sponsorship_new.fng) offer only Back (0x400).  With the mouse
 * there was no way forward: Back led to the reward screen and its Continue back
 * here.  Both get Continue too.  The rows are found by their package name, so the
 * same code serves both executables. */
static uint32_t image_find_bytes(const void *pat, uint32_t n, uint32_t from, uint32_t align){
    extern uint32_t g_image_base, g_image_end;
    uint32_t a = from ? from : g_image_base;
    for (; a + n <= g_image_end; a += align)
        if (!memcmp(g_mem + a, pat, n)) return a;
    return 0;
}
static void sponsor_continue_install(void){
    extern uint32_t g_image_base, g_image_end;
    static const char *const pkg[] = { "UI_SponsorPopup.fng", "UI_Sponsorship_new.fng" };
    int done = 0;
    for (int k = 0; k < 2; k++){
        uint32_t s = image_find_bytes(pkg[k], (uint32_t)strlen(pkg[k]) + 1u, 0, 1);
        if (!s) continue;
        /* the row: the name's address, then a constructor inside the image */
        for (uint32_t r = image_find_bytes(&s, 4, 0, 4); r; r = image_find_bytes(&s, 4, r + 4u, 4)){
            uint32_t ctor = MR32(r + 4u), mask = MR32(r + 12u);
            if (ctor > g_image_base && ctor < g_image_end && MR32(r + 8u) == 0u && MR32(r + 20u) == 0x80u){
                if (mask == 0x400u){ MR32(r + 12u) = 0x404u; done++; }
                break; }
        }
    }
    if (done) fprintf(stderr, "sponsor screens: Continue added to %d of 2\n", done);
    else fprintf(stderr, "sponsor screens: rows not found, left unchanged\n");
}

/* --- BOOT MOVIES: skippable logos and notice ----------------------------------
 * The screens at start play a movie each and go on when it has ended: their
 * message handler gets 0xc3960eb9 from the player, sets [0x8383a8] and moves to
 * the next screen (0x4c4f50).  The EA logo, the blank screen and the THX logo
 * (handler 0x4c53a0) answer nothing else; the notice (PSA, 0x4c53d0) takes
 * Accept/Start (0x406415e3 / 0xb5af2461) only once a profile exists; the opening
 * movie (0x4c5330) takes them but not Escape, Space or B.  BOOTMOVIES:
 *   skippable (default)  Accept or Start, or a new press of Enter, Escape, Space or
 *                        a controller's A, B or Menu (seen on the screen's per-frame
 *                        0xc98356ba), reaches the handler as "movie ended"
 *   off                  they aren't opened (fileio.c); the game goes on as for a missing file
 *   original             as the game has them
 * Story and tutorial movies are other screens and never touched. */
#define MSG_MOVIE_ENDED 0xc3960eb9u
#define MSG_ACCEPT      0x406415e3u
#define MSG_START       0xb5af2461u
#define MSG_TICK        0xc98356bau
int bootmovies_mode(void){                 /* 0 original, 1 skippable, 2 off */
    static int m = -1;
    if (m < 0){ const char *e = getenv("BOOTMOVIES");
        m = !e || !*e ? 1 : !strcasecmp(e, "off") ? 2 : !strcasecmp(e, "original") ? 0 : 1; }
    return m;
}
static int skip_key_fresh(void){
    void metal_fill_keyboard(unsigned char *, unsigned); void gamepad_fill(unsigned char *);
    unsigned char k[256], p[272];
    metal_fill_keyboard(k, 256); gamepad_fill(p);
    int down = ((k[0x1C] | k[0x9C] | k[0x01] | k[0x39]) & 0x80) || ((p[0x30] | p[0x31] | p[0x37]) & 0x80);
    static int was = 1;                    /* a key held since before is no new press */
    int fresh = down && !was; was = down;
    return fresh;
}
static void boot_movie_message(CPU *c, int own_skip){
    if (bootmovies_mode() != 1) return;
    uint32_t msg = MR32(c->esp + 4u);
    int pad = !own_skip && (msg == MSG_ACCEPT || msg == MSG_START);   /* own_skip: the screen takes them itself */
    if (pad) (void)skip_key_fresh();       /* the same press must not skip the next movie too */
    if (pad || (msg == MSG_TICK && skip_key_fresh())){
        MR32(c->esp + 4u) = MSG_MOVIE_ENDED;
        static int n = 0;
        if (n++ < 8) fprintf(stderr, "  [movie] boot movie skipped\n");
    }
}
void sub_004c53a0(CPU *c); void sub_004c53d0(CPU *c); void sub_004c5330(CPU *c);
static void wrap_logo_screen(CPU *c){    boot_movie_message(c, 0); sub_004c53a0(c); }
static void wrap_psa_screen(CPU *c){     boot_movie_message(c, 0); sub_004c53d0(c); }
static void wrap_opening_screen(CPU *c){ boot_movie_message(c, 1); sub_004c5330(c); }

/* --- Tuning graphs: drag a dot with the mouse ------------------------------------
 * On the ECU and turbo graphs (UI_PerformanceTuning_Graph.fng, message handler
 * 0x5654e0) the mouse picks a column, but only the wheel or the keys move its dot.
 * The handler is only watched here: its per-frame message says the screen is up,
 * and com.c then gives an up/down drag with the left button as wheel turns
 * (TUNING GRAPH DRAG). */
static _Atomic long g_graph_tick = -100;
int tuning_graph_active(void){
    long metal_frames(void);
    return metal_frames() - atomic_load(&g_graph_tick) <= 3;
}
void sub_005654e0(CPU *c);
static void wrap_tuning_graph(CPU *c){
    long metal_frames(void);
    if (MR32(c->esp + 4u) == MSG_TICK) atomic_store(&g_graph_tick, metal_frames());
    sub_005654e0(c);
}

void overrides_install(void){
    void dispatch_patch(uint32_t, void(*)(CPU*));
    dispatch_patch(GA(0x005654e0u), wrap_tuning_graph);
    dispatch_patch(GA(0x006e2be0u), wrap_6e2be0);
    dispatch_patch(GA(0x004c53a0u), wrap_logo_screen);
    dispatch_patch(GA(0x004c53d0u), wrap_psa_screen);
    dispatch_patch(GA(0x004c5330u), wrap_opening_screen);
    region_install();
    shadow_install();
    sponsor_continue_install();
}

void overrides_report(void){
    if (g_e2be0_calls)
        fprintf(stderr,"  sub_006e2be0: %llu calls, %llu NULL lists skipped\n",
                (unsigned long long)g_e2be0_calls,(unsigned long long)g_null_list);
    if (g_cycle_break || g_oob_node || g_depth_guard)
        fprintf(stderr,"  tree guard: cycle %llu, out of bounds %llu, depth %llu\n",
                (unsigned long long)g_cycle_break,
                (unsigned long long)g_oob_node,
                (unsigned long long)g_depth_guard);
}

/* --- sub_0050d510: the game's DEBUG printf --------------------------------
 * In the retail build it was emptied to "xor eax,eax; ret", so the engine's
 * whole internal log goes in the bin: front-end package transitions, the
 * message queue, "Joy Event" input dispatch...  We catch it here and print it.
 * cdecl, variadic: format = [esp+4], arguments from [esp+8].            */
uint32_t g_fe_mgr = 0;   /* front-end package manager (captured) */
void sub_0050d510_orig(CPU *c);

static const char *gstr(uint32_t p){
    if (!p || !mem_is_committed(p)) return "(null)";
    return (const char *)(g_mem + p);
}
void sub_0050d510(CPU *c){
    static int on = -1;
    if (on < 0){ const char *e = getenv("GAMELOG"); on = (e && *e && *e != '0'); }
    if (on){
        uint32_t fp = MR32(c->esp + 4u);
        if (fp && mem_is_committed(fp)){
            const char *f = (const char *)(g_mem + fp);
            uint32_t ap = c->esp + 8u;          /* next varargs slot */
            char out[1024]; size_t o = 0;
            for (const char *q = f; *q && o < sizeof out - 8; q++){
                if (*q != '%'){ out[o++] = *q; continue; }
                q++;
                if (*q == '%'){ out[o++] = '%'; continue; }
                /* skip the flag/width/precision fields */
                while (*q && (strchr("-+ #0", *q) || (*q >= '0' && *q <= '9') ||
                              *q == '.' || *q == 'l' || *q == 'h')) q++;
                char cv = *q;
                if (!cv) break;
                char tmp[256];
                if (cv == 's'){
                    uint32_t sp = mem_is_committed(ap+3) ? MR32(ap) : 0; ap += 4;
                    snprintf(tmp, sizeof tmp, "%s", gstr(sp));
                } else if (cv == 'c'){
                    uint32_t v = mem_is_committed(ap+3) ? MR32(ap) : 0; ap += 4;
                    snprintf(tmp, sizeof tmp, "%c", (int)(v & 0xFFu));
                } else if (cv == 'f' || cv == 'g' || cv == 'e'){
                    uint64_t lo = mem_is_committed(ap+7) ? ((uint64_t)MR32(ap) |
                                  ((uint64_t)MR32(ap+4) << 32)) : 0; ap += 8;
                    double dv; memcpy(&dv, &lo, 8);
                    snprintf(tmp, sizeof tmp, "%g", dv);
                } else {
                    uint32_t v = mem_is_committed(ap+3) ? MR32(ap) : 0; ap += 4;
                    if (cv == 'x') snprintf(tmp, sizeof tmp, "%x", v);
                    else if (cv == 'X') snprintf(tmp, sizeof tmp, "%X", v);
                    else if (cv == 'u') snprintf(tmp, sizeof tmp, "%u", v);
                    else if (cv == 'p') snprintf(tmp, sizeof tmp, "%08x", v);
                    else snprintf(tmp, sizeof tmp, "%d", (int)v);
                }
                for (const char *t2 = tmp; *t2 && o < sizeof out - 2; t2++) out[o++] = *t2;
            }
            out[o] = 0;
            fprintf(stderr, "  [game] %s%s", out,
                    (o && out[o-1] == '\n') ? "" : "\n");
            /* Inside SwitchFEngPackages ebx = the front-end package manager
             * ("this").  printf is called exactly at that point, so we catch the
             * pointer here for free; needed to force the transition. */
            if (!strncmp(out, "SwitchFEngPackages", 18)) g_fe_mgr = c->ebx;
            /* WHO triggers the screen transition?  The call chain of a successful
             * transition shows what is missing on the splash screen. */
            { const char *bt = getenv("GAMELOG_BT");
              static int nbt = 0;
              if (bt && *bt && nbt < 4 && !strncmp(out, bt, strlen(bt))){
                  nbt++; guest_backtrace(c, 16); } }
        }
    }
    sub_0050d510_orig(c);
}


/* --- Force rain (FORCERAIN) --------------------------------------------------
 * The game picks the weather randomly at race start (measured: rain in ~1 of
 * 8 runs); debugging a rain-specific problem needs reproducible rain.
 * Rain::Update = sub_0061f130 (__thiscall, ecx = Rain* at eView+0x64):
 *   Rain+0x5094 target precipitation (0..1), +0x1f0 current intensity (drifts
 *   to the target), +0x509c cloud intensity.  The target is computed every
 *   frame by sub_0061e390 (if the "Rain" zone object 0x8a1f28 is active); if
 *   it isn't active the old target stays.
 * FORCERAIN=1 sets the target to 1.0, FORCERAIN=2 sets it to 0 (always dry).
 * FORCERAIN=3 an OVERCAST sky without rain (+0x509c and the target +0x5098 = 0;
 * 1.0 = clear sky), FORCERAIN=4 overcast sky + rain.
 * Without the variable the behavior is EXACTLY the game's.  In both builds. */
static int force_rain(void){
    static int fr = -1;
    if (fr < 0){ const char *e = getenv("FORCERAIN"); fr = e ? atoi(e) : 0; }
    return fr;
}
void sub_0061e390_orig(CPU *c);
void sub_0061e390(CPU *c){
    sub_0061e390_orig(c);
    int fr = force_rain();
    if (fr == 1 || fr == 4) ST(0) = 1.0; else if (fr == 2 || fr == 3) ST(0) = 0.0;
}
void sub_0061f130_orig(CPU *c);
void sub_0061f130(CPU *c){
    uint32_t rain = c->ecx;
    int fr = force_rain();
    if (fr && rain && mem_is_committed(rain + 0x50a3u))
        MRF32(rain + 0x5094u) = (fr == 1 || fr == 4) ? 1.0f : 0.0f;
    sub_0061f130_orig(c);
    if (fr >= 3 && rain && mem_is_committed(rain + 0x50a3u)){
        MRF32(rain + 0x5098u) = 0.0f; MRF32(rain + 0x509cu) = 0.0f; }
}

/* --- Force the sky transition (FORCETOD) -------------------------------------
 * sub_0060fab0 = the time-of-day / sky state machine (every frame while the
 * game state is 6).  A 300 s cycle over the world counter 0x7fb718 (s):
 * when (int)counter % 300 == 60 and state 0x8a1c8c == 0 it switches to state 2
 * and the sky transition (skymorph.efx: sunset -> night) starts.  Since test
 * runs last ~60 s they only occasionally hit this transition (measured: ~1 in
 * 8 runs), and the HD build got very slow in that case.
 * FORCETOD=1 triggers the transition at the first opportunity;
 * FORCETOD=<frame count> after that many calls.  Without it the behavior is
 * EXACTLY the game's.  In both builds. */
void sub_0060fab0_orig(CPU *c);
void sub_0060fab0(CPU *c){
    static int ft = -1, done = 0; static long calls = 0;
    if (ft < 0){ const char *e = getenv("FORCETOD"); ft = e ? atoi(e) : 0; }
    if (ft > 0 && !done && MR32(GA(0x8654a4u)) == 6u && MR32(GA(0x8a1c78u)) != 0u && ++calls >= ft &&
        MR32(GA(0x8a1c8cu)) == 0u){
        static int st = -1;
        if (st < 0){ const char *e = getenv("FORCETOD_STATE"); st = e ? atoi(e) : 2; if (st < 1 || st > 4) st = 2; }
        MR32(GA(0x8a1c8cu)) = (uint32_t)st; done = 1;
        fprintf(stderr, "[FORCETOD] sky state forced to %d (call %ld)\n", st, calls);
    }
    sub_0060fab0_orig(c);
}

/* --- sub_005c7ed0: per-view camera/projection setup ------------------------
 * __thiscall(ecx = matrix structure, [esp+4] = eView*), ret 0x10.
 *   eView+0x04 = view id (1 main, 3 rear-view mirror, 4 road reflection)
 *   eView+0x40 = Camera*  (CameraParams: +0xbc NearZ, +0xc0 FarZ, +0xc4 FOV)
 *   ecx+0x00 view matrix, +0x40 projection, +0x80 the product of the two
 * The game only gives SetTransform identity matrices; the camera can only be
 * read from here.  Behavior DOESN'T CHANGE: only the address is captured and
 * (in the HD build) the main view's matrices are recorded into the frame.  In
 * the original build enh_view_setup is an empty inline function.          */
void sub_005c7ed0_orig(CPU *c);
void sub_005c7ed0(CPU *c){
    uint32_t plat = c->ecx, view = MR32(c->esp + 4u);
    sub_005c7ed0_orig(c);
    if (view && plat && mem_is_committed(view + 0x43u) && mem_is_committed(plat + 0xbfu))
        enh_view_setup(MR32(view + 4u), (const float *)(g_mem + plat),
                       MR32(view + 0x40u));
}
