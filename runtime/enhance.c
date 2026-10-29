/* Graphics enhancements (HD build only).  See enhance.h, GRAPHICS_RESEARCH.md.
 *
 * 1) REFLECTION RESOLUTION
 *    The game draws three reflections into separate render targets:
 *      road reflection    320x240   (upscaled 6x horizontally at 1080p)
 *      rear-view mirror   256x256
 *      car cube map       128/face
 *    The targets are recognized by their CALL ADDRESS (our exe, SHA 8f6a9ae2...).
 *    The community mods' addresses are shifted in this exe; the ones here come
 *    from our own parsing:
 *      0x5BA0A7  CreateTexture(320,240,RT)   road reflection
 *      0x5BA11D  CreateTexture(320,240,RT)   road reflection blur
 *                                            (only at reflection level 3)
 *      0x5BA1DD  CreateTexture([7FEE80],[7FEE84],RT)  rear-view mirror
 *      0x5BA559  CreateCubeTexture([7FEE6C],RT)       car environment map
 *    The Z surfaces (0x5BA0E9, 0x5BA21C, 0x5BA5AA) stay untouched: Metal creates
 *    the Z texture at the size of the target it is first used with.
 *
 *    Env:    HDREFL=0       turn the reflection enhancement off (for A/B)
 *            HD_ROAD=0.5    road reflection: multiple of the back buffer.
 *                           2026-10-15 1.0 -> 0.5: ~2.5 W less while holding
 *                           120 fps in a 3024x1964 race (energy per frame -17%);
 *                           the image difference is next to nothing (with ray
 *                           tracing on this texture is only a blurry fallback).
 *                           Shrinking the cube/mirror brought no extra gain.
 *            HD_MIRROR=1024 mirror edge (pixels)
 *            HD_CUBE=512    cube face edge (pixels)
 *            HD_BLUR=0.5    road reflection blur: 1 = the game's own width,
 *                           0 = sharp (see below)
 *
 * 2) ROAD REFLECTION BLUR
 *    The game blurs the reflection in two passes with ps 19 (13->14->13):
 *    4 taps, offsets fixed in UV relative to 320 pixels.  When the texture grew
 *    to 1920 the taps landed 6 pixels apart and a vertical striped pattern
 *    appeared.  Solution: a mip chain on the reflection targets; in the blur pass
 *    read from level L and scale the tap offsets about the center by
 *    s = 2^L / scale.  This way the tap spacing is always ~1 texel:
 *      HD_BLUR=1 -> L = log2(scale), s = 1: the game's blur width
 *      HD_BLUR=0 -> L = 0, s = 1/scale: a fine blur at full resolution
 *
 * 3) MSAA (code in metal.m, "HD: MSAA for the screen")
 *    HD_MSAA=4 (default; 2/8 also work, 0/1 off).  Only SCREEN passes are
 *    multisampled; every pass is resolved into the drawable with
 *    StoreAndMultisampleResolve.  A multisampled twin of Z is kept and its depth
 *    is also RESOLVED into the single-sampled Z (HD_ZRESOLVE=0 turns it off):
 *    the game's depth-tested fullscreen quad (ps 17 / vs 11, to a 1920x1080
 *    target) must see the scene's depth.  Without the resolve, the number of
 *    draws doing a Z test on an offscreen target was measured at ~11 per frame.
 *
 * 4) SSR + reflection separation (metal.m "HD: SSR", vsemu.c ps_to_msl_ex2,
 *    camera: overrides.c sub_005c7ed0).  HD_SSR=0 off, HD_SSR_DEBUG=1.
 *    Details: GRAPHICS_RESEARCH.md §8.
 *
 * 5) RAY-TRACED REFLECTIONS (metal.m "RT kernel" + "STATIC SCENE CACHE",
 *    the vsk_N compute kernel from vsemu.c vs_to_msl).  macOS 13+.
 *    For road and car pixels a reflection ray is cast into the real scene
 *    geometry; a hit is shaded with the game's own material (texture, vertex
 *    color, headlight term, fog, alpha test).  Miss -> the game's own reflection.
 *      HD_RT=0         off (falls back to SSR)
 *      HD_RT_SCALE=1..4 ray resolution divisor; AUTOMATIC if not given (by width:
 *                      1080p 2, 2560/3024 3, 4K 4)
 *      HD_RT_CACHE=0   turn off the static scene cache (what's behind the camera is lost)
 *      HD_RT_MAXD=600  ray range (world units ~ meters)
 *      HD_RT_LOD=0.05  texture mip by distance
 *      HD_RT_FIX=0     turn off the vertex check (triangles with NaN / infinite /
 *                      farther-than-100-km vertices poison the acceleration
 *                      structure: §11)
 *    Diagnostics: HD_SSR_DEBUG=4 (reflection only), HD_RT_LOG=1/2,
 *    HD_RT_DUMPTAB=<frame>, HD_RT_STRESS=<N>, HD_RT_STAGE/NOTEX/PLAIN, HD_RTPROBE.
 *    Details: GRAPHICS_RESEARCH.md §9-§10.
 *
 * 6) GPU BUDGET (metal.m "MSAA: unnecessary store / resolve", "DEFER THE
 *    INTERMEDIATE PASS THAT SPLITS THE SCENE").  5 multisampled screen passes
 *    per frame were reduced to 1.
 *      HD_MSLAZY=0     lazy store/resolve off (old behavior)
 *      HD_REORDER=0    intermediate pass deferral off
 *    Measurement (both builds): GPUWIN=a,b  PASSPROF=N [PASSPROF_MODE=1]
 *    HD only: HD_COMP=0  HD_RT_AB=1.   Details: GRAPHICS_RESEARCH.md §11.   */
#include "enhance.h"
#if NFS_ENHANCED
#include <stdio.h>
#include <stdlib.h>

#ifndef GA                       /* game address, see cpu.h */
#define GA(x) (x)
#endif
#define RET_ROAD       GA(0x005BA0A7u)
#define RET_ROAD_BLUR  GA(0x005BA11Du)
#define RET_MIRROR     GA(0x005BA1DDu)
#define RET_CUBE       GA(0x005BA559u)

static int    g_init, g_refl = 1;
static double g_road = 0.5, g_blur = 0.5;
static int    g_road_env;                 /* HD_ROAD given: no automatic choice */
static uint32_t g_mirror = 1024, g_cube = 512;
static uint32_t g_hits[4];
static uint32_t g_last_w[4], g_last_h[4];

static uint32_t clampu(double v){
    if (v < 16.0) return 16u;
    if (v > 8192.0) return 8192u;
    return (uint32_t)(v + 0.5);
}

static void enh_init(void){
    if (g_init) return;
    g_init = 1;
    const char *e;
    if ((e = getenv("HDREFL"))   && *e == '0') g_refl = 0;
    if ((e = getenv("HD_ROAD"))   && *e){ g_road = atof(e); g_road_env = 1; }
    if ((e = getenv("HD_MIRROR")) && *e) g_mirror = clampu(atof(e));
    if ((e = getenv("HD_CUBE"))   && *e) g_cube   = clampu(atof(e));
    if ((e = getenv("HD_BLUR"))   && *e) g_blur   = atof(e);
    if (g_blur < 0.0) g_blur = 0.0;
    if (g_blur > 1.0) g_blur = 1.0;
    if (g_road <= 0.0) g_road = 0.5;
    if (g_road_env) fprintf(stderr, "[HD] reflection: %s  road=%.2fx back buffer  blur=%.2f  mirror=%u  cube=%u\n",
                            g_refl ? "ON" : "OFF", g_road, g_blur, g_mirror, g_cube);
    else fprintf(stderr, "[HD] reflection: %s  road=%s back buffer  blur=%.2f  mirror=%u  cube=%u\n",
                 g_refl ? "ON" : "OFF", "automatic (1/3x with ray tracing on, 0.50x with it off)", g_blur, g_mirror, g_cube);
}

const char *enh_build_name(void){ return "HD"; }
float enh_blur(void){ enh_init(); return (float)g_blur; }

int enh_rt_size(uint32_t ret, uint32_t bb_w, uint32_t bb_h,
                uint32_t *w, uint32_t *h){
    enh_init();
    if (!g_refl) return ENH_NONE;
    int k, kind;
    uint32_t nw, nh;
    switch (ret){
    case RET_ROAD: case RET_ROAD_BLUR:
        /* If it isn't the expected size (another version/patch), don't touch it. */
        if (*w != 320u || *h != 240u) return ENH_NONE;
        k  = (ret == RET_ROAD) ? 0 : 1; kind = ENH_ROAD;
        /* ROAD REFLECTION SCALE: with ray tracing on, the game's road reflection is only
         * visible where the ray doesn't hit anything (sky, out of range); where it
         * hits, the composite subtracts it exactly with R0 from the G-buffer and
         * puts the RT result in.  1/3 is enough there (1008x633 at 3024, 3x the
         * original's 320x240).  Measured (Free Run, capacity without presenting,
         * deterministic): 5.94 -> 5.72 ms/frame.  With ray tracing off (SSR) the
         * reflection is visible on the whole road: 0.5.  HD_ROAD=<ratio> by hand. */
        { int metal_hd_rt_enabled(void);
          double rs = g_road_env ? g_road : (metal_hd_rt_enabled() ? 1.0 / 3.0 : 0.5);
          nw = clampu(bb_w * rs); nh = clampu(bb_h * rs); }
        break;
    case RET_MIRROR:
        if (*w != 256u || *h != 256u) return ENH_NONE;
        k = 2; kind = ENH_MIRROR; nw = nh = g_mirror;
        break;
    case RET_CUBE:
        if (*w != 128u) return ENH_NONE;
        k = 3; kind = ENH_CUBE; nw = nh = g_cube;
        break;
    default:
        return ENH_NONE;
    }
    if (g_hits[k] < 3 || g_last_w[k] != nw || g_last_h[k] != nh){
        static const char *nm[4] = { "road reflection", "road blur",
                                     "rear-view mirror", "cube map" };
        fprintf(stderr, "[HD] %s %ux%u -> %ux%u\n", nm[k], *w, *h, nw, nh);
    }
    g_hits[k]++; g_last_w[k] = nw; g_last_h[k] = nh;
    *w = nw; *h = nh;
    return kind;
}

/* Hand the main view's (id 1) matrices to Metal as frame data. */
void enh_view_setup(uint32_t id, const float *m, uint32_t cam){
    (void)cam;
    /* VIEWLOG=1: the view matrix (3x3 + position) of every view id, once after the
     * 4000th call per id.  For reading the axes of the cube face cameras. */
    { static int vl = -1; static unsigned cnt[32]; static unsigned char done[32];
      if (vl < 0) vl = getenv("VIEWLOG") ? 1 : 0;
      if (vl && id < 32u && !done[id] && ++cnt[id] > 4000u){ done[id] = 1;
          fprintf(stderr, "[view %u] right=(%.3f %.3f %.3f) y=(%.3f %.3f %.3f) fwd=(%.3f %.3f %.3f) P00=%.3f P11=%.3f\n",
                  id, m[0], m[4], m[8], m[1], m[5], m[9], m[2], m[6], m[10], m[16], m[21]); } }
    /* The cube map face cameras (views 16..21) are aligned NOT to the world axes
     * but to some other frame (measured).  To convert the cube sampling vector to
     * world space, the forward directions of the face cameras: cube +x = view 17,
     * +y = view 18, +z = view 21 (measured: right/back/up on the car; verified
     * with the HD_SSR_DEBUG=3 normal agreement test). */
    static float cube_b[9] = { 1,0,0, 0,1,0, 0,0,1 };
    if (id == 17u || id == 18u || id == 21u){
        int a = (id == 17u) ? 0 : (id == 18u) ? 1 : 2;
        cube_b[a*3] = m[2]; cube_b[a*3+1] = m[6]; cube_b[a*3+2] = m[10];
        return;
    }
    if (id != 1u) return;
    void metal_hd_set_camera(const float *m48, float k, const float *cube9);
    float com_aspfix(void);
    int ws_game_fov(void);   /* wsfov.c */
    metal_hd_set_camera(m, ws_game_fov() ? 1.f : com_aspfix(), cube_b);
}

void enh_report(void){
    if (!g_init) return;
    fprintf(stderr, "[HD] reflection targets: road=%u blur=%u mirror=%u cube=%u"
            " (last: road %ux%u, mirror %u, cube %u)\n",
            g_hits[0], g_hits[1], g_hits[2], g_hits[3],
            g_last_w[0], g_last_h[0], g_last_w[2], g_last_w[3]);
}
#endif /* NFS_ENHANCED */
