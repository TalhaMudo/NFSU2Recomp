/* reflect.c - sharper reflections, and the sky, the car lights and more detail
 * in them.  Both engines; every part is off unless asked for (the launcher's
 * Reflections card sets the variables below).
 *
 * Follows what Aero_'s "NFS HD Reflections" mod changes in Underground 2 (which
 * values and which calls); the code here is our own.
 *
 * The game draws three reflections into separate render targets:
 *   road reflection   view 4       320x240          (the wet road)
 *   rear-view mirror  view 3       256x256          (bumper and hood views)
 *   car cube map      views 16-21  128 per face     (the car's paint)
 *
 * NFSU2_REFLECTIONS=sharp|sharpest (original engine)
 *   The targets grow in Metal while the game keeps its own sizes, the same way
 *   as in the HD engine (enhance.c, "REFLECTION RESOLUTION"): the renderer always
 *   draws to the whole target and samples in UV space, so nothing in the game
 *   has to know.  The road reflection's blur pass reads a mip level of the
 *   larger texture (metal.m "ROAD REFLECTION BLUR", enhance.c "ROAD REFLECTION
 *   BLUR") so its taps stay one texel apart.
 *     sharp     road 1/3 of the screen, mirror 512, cube 256 per face
 *     sharpest  road 1/2 of the screen, mirror 1024, cube 512 per face
 *   The HD engine always uses its own sizes, which are at least "sharpest".
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
#include "enhance.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

uint32_t call_guest(CPU *c, uint32_t fn, int nargs, const uint32_t *args);   /* com.c */

/* render target CreateTexture call sites (same as enhance.c) */
#define RET_ROAD        GA(0x005BA0A7u)
#define RET_ROAD_BLUR   GA(0x005BA11Du)
#define RET_MIRROR      GA(0x005BA1DDu)
#define RET_CUBE        GA(0x005BA559u)
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

static int g_init, g_size, g_sky, g_lights, g_detail;
static unsigned long g_n_sky[2], g_n_lod, g_n_car, g_n_lights;
static int g_lights_here;

static int on(const char *name){
    const char *e = getenv(name);
    return e && *e && *e != '0';
}

static void refl_init(void){
    if (g_init) return;
    g_init = 1;
    const char *e = getenv("NFSU2_REFLECTIONS");
    if (e && !strcasecmp(e, "sharp")) g_size = 1;
    else if (e && !strcasecmp(e, "sharpest")) g_size = 2;
    g_sky = on("NFSU2_REFLECT_SKY");
    g_lights = on("NFSU2_REFLECT_LIGHTS");
    g_detail = on("NFSU2_REFLECT_DETAIL");
    if (g_size || g_sky || g_lights || g_detail)
        fprintf(stderr, "reflections: size %s%s%s%s\n",
                NFS_ENHANCED ? "HD engine's" : g_size == 2 ? "sharpest" : g_size ? "sharp" : "original",
                g_sky ? ", sky" : "", g_lights ? ", car lights" : "", g_detail ? ", more detail" : "");
}

/* ---- render target sizes (original engine; see enhance.h) ---------------- */
int refl_rt_size(uint32_t ret, uint32_t bb_w, uint32_t bb_h, uint32_t *w, uint32_t *h){
    refl_init();
    if (!g_size) return ENH_NONE;
    uint32_t nw, nh; int kind, k;
    switch (ret){
    case RET_ROAD: case RET_ROAD_BLUR: {
        if (*w != 320u || *h != 240u) return ENH_NONE;    /* another version: leave it */
        uint32_t d = g_size == 2 ? 2u : 3u;
        nw = bb_w / d; nh = bb_h / d;
        if (nw < 320u) nw = 320u;
        if (nh < 240u) nh = 240u;
        kind = ENH_ROAD; k = ret == RET_ROAD ? 0 : 1;
        break; }
    case RET_MIRROR:
        if (*w != 256u || *h != 256u) return ENH_NONE;
        nw = nh = g_size == 2 ? 1024u : 512u; kind = ENH_MIRROR; k = 2;
        break;
    case RET_CUBE:
        if (*w != 128u) return ENH_NONE;
        nw = nh = g_size == 2 ? 512u : 256u; kind = ENH_CUBE; k = 3;
        break;
    default:
        return ENH_NONE;
    }
    static uint32_t lw[4], lh[4];
    if (lw[k] != nw || lh[k] != nh){
        static const char *nm[4] = { "road reflection", "road blur", "rear-view mirror", "cube map" };
        fprintf(stderr, "reflections: %s %ux%u -> %ux%u\n", nm[k], *w, *h, nw, nh);
        lw[k] = nw; lh[k] = nh;
    }
    *w = nw; *h = nh;
    return kind;
}

/* Road blur width for the larger target: 1 = the game's, 0 = sharp (enhance.c). */
float refl_blur(void){ refl_init(); return g_size ? 0.5f : 1.f; }
/* Whether road reflection targets can be larger than the game's (vsemu.c). */
int refl_road_grows(void){ refl_init(); return g_size != 0; }

/* ---- hooks (debug.c guest_hook, before the instruction at ip) ------------- */
static int refl_view(uint32_t id){ return id == 3u || id == 4u || (id >= 16u && id <= 21u); }

int refl_hook(CPU *c, uint32_t ip){
    switch (ip){
    case H_ROAD_SCENERY: case H_MIRROR_SCENERY:
        refl_init();
        if (g_sky && c->esi){
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
        if (g_sky && (c->edx == 3u || c->edx == 4u)){
            uint32_t v = MR32(c->ebp + 8u), cam = v ? MR32(v + 0x40u) : 0u;
            float far = cam ? MRF32(cam + 0xc0u) : 0.f, k = far / 10400.f;
            if (k > 0.f) ST(0) = (double)(k < 1.f ? k : 1.f);
            c->edx = 1u;
        }
        return 1;
    case H_SCENERY_LOD:
        refl_init();
        if (g_detail && (c->ebx & 0x1820u)){
            static int far_set;
            if (!far_set){ far_set = 1; MRF32(CUBE_FAR) = 10000.f; }
            c->ebx &= ~0x1820u; g_n_lod++;
        }
        return 1;
    case H_CAR_LOD: case H_CAR_LOD2:
        if (g_detail){
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
void sub_006154a0_orig(CPU *c);
void sub_006154a0(CPU *c){                  /* cdecl (eView *view) */
    refl_init();
    if (g_lights && !g_lights_here){ POP32(c->eip); return; }   /* drawn below instead */
    sub_006154a0_orig(c);
}

void sub_00494270_orig(CPU *c);
void sub_00494270(CPU *c){                  /* cdecl (eView *view, int, int pass) */
    refl_init();
    uint32_t v = MR32(c->esp + 4u);
    if (g_lights && v && MR32(v + 4u) == 4u && MR32(c->esp + 12u) == 1u){
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
    if (!g_init || !(g_size || g_sky || g_lights || g_detail)) return;
    fprintf(stderr, "  reflections      : sky road %lu / mirror %lu, scenery LOD raised %lu, car LOD raised %lu, car lights drawn %lu\n",
            g_n_sky[0], g_n_sky[1], g_n_lod, g_n_car, g_n_lights);
}
