/* visual.c - optional colour setting on the game side (launcher "Picture").
 * Off by default; off is exactly the game's own behaviour.
 *
 * COLOUR TINT (TINT=less|none|cool)
 *   The yellow-green cast of the PC version comes from the game's screen
 *   filter (the IDI_SCREENFILTER_FX effect, our ps 28 -> ps 27 -> ps 26 chain,
 *   see metal.m POST-PROCESS CHAIN).  Two of its steps add colour:
 *   - ps 27 adds a tint colour to every pixel that is not already yellow
 *     (weight 0.4 * saturate(1 - (max(R, G) - B))).  sub_005c6530 hands it
 *     to the effect with SetVector at 0x5c6766: the colour at 0x7ff15c..64
 *     times the strength at 0x7ff16c, .w = the filter's desaturation
 *     (0x7ff188).  Measured in a downtown race: (0.147, 0.172, 0.126), i.e.
 *     green + red over blue.
 *   - ps 26 replaces blue with a lookup in a 1x256 ramp the game refills every
 *     frame: R and G of each row follow the row, B is pulled down (64 -> 54,
 *     128 -> 115, 192 -> 182).  The ramp is the texture whose pointer the game
 *     keeps at 0x86f8ac (created at 0x5bbcae, CreateTexture(1, 256)).
 *   TINT=none makes both neutral: the tint colour becomes its own grey (same
 *   luma, so the filter's brightness lift stays) and the ramp's B follows its
 *   R.  A grey in the scene then comes out grey.  TINT=less does half of it.
 *   TINT=cool also lifts the ramp's blue by 9% (the blue gain RKBDI's "Piss
 *   Filter Removal" ends up with after the filter's three passes), for the
 *   cooler look of that mod.  Only copies change: a hook before the
 *   SetVector call rewrites the vector on the stack, com.c's UnlockRect
 *   rewrites the ramp on its way to the GPU.  The rest of the filter
 *   (contrast curve, desaturation, glow) stays, and the HUD and the menus
 *   never go through it.  ABTEST=TINT switches it off in the measurement's
 *   off slices (metal.m IN-RUN A/B): before/after frames from one run.
 *   RKBDI's mod replaces the effect with its own
 *   levels shader; it was read as a reference only, no code from it is used. */
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_tint = -1;                  /* 0 the game's, 50 less, 100 none, 109 cool */
static unsigned long g_tint_ramp, g_tint_vec;

static int tint_mode(void){
    if (g_tint < 0){
        const char *e = getenv("TINT");
        g_tint = !e ? 0 : !strcmp(e, "less") ? 50 : !strcmp(e, "none") ? 100 : !strcmp(e, "cool") ? 109 : 0;
    }
    int metal_abt_off(const char *);
    return g_tint && !metal_abt_off("TINT") ? g_tint : 0;
}

/* com.c UnlockRect: the data about to be uploaded for texture object tex.
 * Returns the data to upload instead (tmp, cap bytes), or src unchanged. */
const void *visual_tex_upload(uint32_t tex, const void *src, uint32_t w, uint32_t h,
                              uint32_t fmt, uint32_t pitch, void *tmp, uint32_t cap){
    int m = tint_mode();
    if (!m || w != 1u || h != 256u || fmt != 21u || pitch < 4u || pitch * h > cap) return src;
    if (!tex || !mem_is_committed(GA(0x0086f8acu) + 3u) || MR32(GA(0x0086f8acu)) != tex) return src;
    int pct = m > 100 ? 100 : m, gain = m > 100 ? m : 100;
    const uint8_t *s = (const uint8_t *)src; uint8_t *d = (uint8_t *)tmp;
    for (uint32_t y = 0; y < h; y++){
        const uint8_t *p = s + y * pitch; uint8_t *q = d + y * pitch;
        for (uint32_t k = 0; k < pitch; k++) q[k] = p[k];
        int b = p[0], r = p[2];          /* A8R8G8B8 in memory: B G R A */
        int t = (r * gain + 50) / 100; if (t > 255) t = 255;
        q[0] = (uint8_t)(b + ((t - b) * pct + (t > b ? 50 : -50)) / 100);
    }
    g_tint_ramp++;
    return tmp;
}

/* Called from guest_hook (debug.c) BEFORE the instruction at ip runs. */
int visual_hook(CPU *c, uint32_t ip){
    if (ip != GA(0x005c6766u)) return 0;        /* call [ecx+0x88]: SetVector(effect, handle, &v) */
    int m = tint_mode();
    uint32_t v = mem_is_committed(c->esp + 11u) ? MR32(c->esp + 8u) : 0u;
    if (m && v && mem_is_committed(v + 15u)){
        float k = (m > 100 ? 100 : m) / 100.f;
        float r = MRF32(v), g = MRF32(v + 4u), b = MRF32(v + 8u);
        float y = 0.3f * r + 0.59f * g + 0.11f * b;  /* the filter's own luma weights (ps 28) */
        MRF32(v) = r + (y - r) * k; MRF32(v + 4u) = g + (y - g) * k; MRF32(v + 8u) = b + (y - b) * k;
        g_tint_vec++;
    }
    return 1;
}

void visual_report(void){
    if (g_tint > 0) fprintf(stderr, "  colour tint      : %s, %lu tint colours and %lu ramp uploads rewritten\n",
                             g_tint == 50 ? "less" : g_tint == 100 ? "none" : "cool", g_tint_vec, g_tint_ramp);
}
