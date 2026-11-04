/* reflect.c - the sky, the car lights and more detail in the reflections.
 * HD engine only (NFS_ENHANCED): the original engine stays the 2004 picture, so
 * there the hooks do nothing and the two overrides call the game's own
 * functions.  Every part is off unless asked for (launcher Settings > Picture,
 * Reflections).  The HD engine already draws the reflection targets larger
 * (enhance.c "REFLECTION RESOLUTION").
 *
 * Follows what Aero_'s "NFS HD Reflections" mod changes in Underground 2 (which
 * values and which calls); the code here is our own.
 *
 * The game draws three reflections into separate render targets:
 *   road reflection   view 4       320x240          (the wet road)
 *   rear-view mirror  view 3       256x256          (bumper and hood views)
 *   car cube map      views 16-21  128 per face     (the car's paint)
 *
 * NFSU2_REFLECT_SKY=1: the sky in the road reflection and the mirror.
 *   The sky (0x60f890: puts the sky model around the view's camera) is only
 *   drawn for the main view and, as its own small sky, in the car cube map; the
 *   road reflection and the mirror show black where the sky would be.  Here the
 *   road reflection (before its scenery, 0x5cab69) and the mirror (0x5cae71)
 *   draw it too, the way the main view does (sky, then 0x5cf430 draws the
 *   queued models).  In 0x60f890 both take the main view's path (0x60f9f6,
 *   edx = 1), the sky scaled to fit inside the view's far plane (10 km for the
 *   road reflection, 192 m for the mirror).  The road reflection draws the
 *   scenery mirrored in z (0x5d67b9 negates each model's z axis), so its sky's
 *   matrix (the newest one in the per-frame pool at 0x82da5c) is mirrored too.
 *
 * NFSU2_REFLECT_LIGHTS=1: the cars' lights in the road reflection.
 *   The game calls 0x6154a0 (the cars' light glows for a reflection, from each
 *   car's own light colours) for the road reflection inside the scenery
 *   shader's pass (0x5cac17), where it draws nothing, and draws the glows with
 *   the generic 0x494270 instead (0x5cacb5, a dimmer shader).  Here 0x6154a0 is
 *   skipped at the first place and drawn instead of 0x494270 for the road
 *   reflection, in the light glows' own pass: the same glows, white and bright.
 *
 * NFSU2_REFLECT_DETAIL=1: more detail in the reflections.
 *   The reflections draw the scenery's low-detail models (0x5d6666: flags
 *   0x1800 of the view = reflection, 0x20 = mirror) and the cars in the road
 *   reflection at their lowest level of detail (0x63167d, 0x6311fc: the model
 *   and wheel LOD); here they get the main view's.  The cube map's far plane
 *   (0x7fee8c, 64 m) moves to 10 km, so the car's paint reflects the city
 *   rather than the street around it.  It roughly doubles the draws into the
 *   reflections.                                                            */
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

void sub_006154a0_orig(CPU *c);
void sub_00494270_orig(CPU *c);

#if NFS_ENHANCED

uint32_t call_guest(CPU *c, uint32_t fn, int nargs, const uint32_t *args);   /* com.c */

/* hook sites (tools/gen_v2.sh HOOKS) */
#define H_ROAD_SCENERY  GA(0x005cab69u)   /* push 8: view 4's scenery list */
#define H_MIRROR_SCENERY GA(0x005cae71u)  /* push 1: view 3's scenery list */
#define H_SKY_SCALE     GA(0x0060f9f6u)   /* cmp edx,1: the sky's scale is in st0 */
#define H_SCENERY_LOD   GA(0x005d6666u)   /* test bh,0x18: ebx = the view's flags */
#define H_CAR_LOD       GA(0x0063167du)   /* push ecx: ecx/edx = model/wheel LOD */
#define H_CAR_LOD2      GA(0x006311fcu)   /* mov edx,[ebp+0xc]: eax/ecx = model/wheel LOD */
#define FN_SKY          GA(0x0060f890u)
#define FN_DRAW_QUEUED  GA(0x005cf430u)
#define CUBE_FAR        GA(0x007fee8cu)   /* float, 64.0 */
#define SKY_POOL        GA(0x0082da5cu)   /* next free 4x4 in the per-frame matrix pool */

static int g_init, g_sky, g_lights, g_detail;
static unsigned long g_n_sky[2], g_n_lod, g_n_car, g_n_lights;
static int g_lights_here;
static float g_cube_far0 = -1.f;      /* the game's cube map far plane (64 m) */

/* ABTEST=REFL (metal.m IN-RUN A/B): all three off in the measurement's off slices */
static int ab_on(void){ int metal_abt_off(const char *); return !metal_abt_off("REFL"); }
#define SKY    (g_sky && ab_on())
#define LIGHTS (g_lights && ab_on())
#define DETAIL (g_detail && ab_on())

static int on(const char *name){
    const char *e = getenv(name);
    return e && *e && *e != '0';
}

static void refl_init(void){
    if (g_init) return;
    g_init = 1;
    g_sky = on("NFSU2_REFLECT_SKY");
    g_lights = on("NFSU2_REFLECT_LIGHTS");
    g_detail = on("NFSU2_REFLECT_DETAIL");
    if (g_sky || g_lights || g_detail)
        fprintf(stderr, "reflections:%s%s%s\n",
                g_sky ? " sky" : "", g_lights ? " car-lights" : "", g_detail ? " more-detail" : "");
}

/* ---- hooks (debug.c guest_hook, before the instruction at ip) ------------- */
static int refl_view(uint32_t id){ return id == 3u || id == 4u || (id >= 16u && id <= 21u); }

int refl_hook(CPU *c, uint32_t ip){
    switch (ip){
    case H_ROAD_SCENERY: case H_MIRROR_SCENERY:
        refl_init();
        if (SKY && c->esi){
            uint32_t a[1] = { c->esi }, pool = MR32(SKY_POOL);
            call_guest(c, FN_SKY, 1, a);
            /* The road reflection draws the scenery mirrored in z (0x5d67b9 negates
             * each model's z axis); mirror the sky the same way, about the camera:
             * the sky is far enough for the plane not to matter. */
            if (ip == H_ROAD_SCENERY && MR32(SKY_POOL) == pool + 0x40u)
                MRF32(pool + 0x28u) = -MRF32(pool + 0x28u);
            call_guest(c, FN_DRAW_QUEUED, 0, a);
            g_n_sky[ip == H_MIRROR_SCENERY]++;
        }
        return 1;
    case H_SKY_SCALE:
        /* The road reflection takes the main views' path (edx 1-3 draw the sky).
         * The main views' sky (scale 1) fits inside their 10 km far plane; the
         * mirror's far plane is 192 m, so its sky is scaled to fit inside its own. */
        if (SKY && (c->edx == 3u || c->edx == 4u)){
            uint32_t v = MR32(c->ebp + 8u), cam = v ? MR32(v + 0x40u) : 0u;
            float far = cam ? MRF32(cam + 0xc0u) : 0.f, k = far / 10400.f;
            if (k > 0.f) ST(0) = (double)(k < 1.f ? k : 1.f);
            c->edx = 1u;
        }
        return 1;
    case H_SCENERY_LOD:
        refl_init();
        if (g_detail){
            if (g_cube_far0 < 0.f) g_cube_far0 = MRF32(CUBE_FAR);
            float want = DETAIL ? 10000.f : g_cube_far0;
            if (MRF32(CUBE_FAR) != want) MRF32(CUBE_FAR) = want;
        }
        if (DETAIL && (c->ebx & 0x1820u)){ c->ebx &= ~0x1820u; g_n_lod++; }
        return 1;
    case H_CAR_LOD: case H_CAR_LOD2:
        if (DETAIL){
            uint32_t v = MR32(c->ebp + 8u);
            if (v && refl_view(MR32(v + 4u))){
                if (ip == H_CAR_LOD){ c->ecx = 0; c->edx = 0; }
                else                { c->eax = 0; c->ecx = 0; }
                g_n_car++;
            }
        }
        return 1;
    }
    return 0;
}

/* ---- car lights in the road reflection (tools/gen_v2.sh OVR) -------------- */
void sub_006154a0(CPU *c){                  /* cdecl (eView *view) */
    refl_init();
    if (LIGHTS && !g_lights_here){ POP32(c->eip); return; }   /* drawn below instead */
    sub_006154a0_orig(c);
}

void sub_00494270(CPU *c){                  /* cdecl (eView *view, int, int pass) */
    refl_init();
    uint32_t v = MR32(c->esp + 4u);
    if (LIGHTS && v && MR32(v + 4u) == 4u && MR32(c->esp + 12u) == 1u){
        g_lights_here = 1;
        PUSH32(v); PUSH32(MR32(c->esp + 4u));
        sub_006154a0_orig(c);
        c->esp += 4u;
        g_lights_here = 0;
        g_n_lights++;
        POP32(c->eip);
        return;
    }
    sub_00494270_orig(c);
}

void refl_report(void){
    if (!g_init || !(g_sky || g_lights || g_detail)) return;
    fprintf(stderr, "  reflections      : sky road %lu / mirror %lu, scenery LOD raised %lu, car LOD raised %lu, car lights drawn %lu\n",
            g_n_sky[0], g_n_sky[1], g_n_lod, g_n_car, g_n_lights);
}

#else   /* original engine: the game's own reflections */
int  refl_hook(CPU *c, uint32_t ip){ (void)c; (void)ip; return 0; }
void sub_006154a0(CPU *c){ sub_006154a0_orig(c); }
void sub_00494270(CPU *c){ sub_00494270_orig(c); }
void refl_report(void){}
#endif
