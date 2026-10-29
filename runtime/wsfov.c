/* wsfov.c - Hor+ widescreen inside the game's own projection setup.
 *
 * Contributed by Burak Cetin (iburakct).  Behaviour follows th0mk's
 * nfsu2-web runtime/widescreen.c (MIT), which mirrors ThirteenAG's
 * NFSUnderground2.WidescreenFix; the addresses below were re-verified by
 * disassembly in the European v1.2 SPEED2.EXE.  The North American v1.2 build
 * has the same instructions, 0x10 lower in the projection setup (0x5c7fee) and
 * at the same address for the HUD marker; GA() maps them (runtime/build_na.h),
 * tools/gen_v2.sh lifts the hooks at the NA addresses.
 *
 * Problem: com.c's aspect_apply made the picture wide by scaling clip-space x
 * of every perspective draw, AFTER the game had culled its objects with its
 * own 4:3 frustum.  Objects in the strips outside the 4:3 middle were skipped
 * and appeared late when the camera turned (donuts, 360s).
 *
 * Fix: scale P00 where the game computes it (sub_005c7ed0, the fstp at
 * 0x5c7ffe) for the views that fill the screen -- 1/2 (player) and 4/5 (their
 * road reflection) -- so the view*projection product AND the culling planes
 * the game builds from it are Hor+.  The mirror (3), shadow views (9, 10) and
 * cube map faces (16-21) keep the game's projection.  aspect_apply then
 * leaves perspective draws alone, and the HD composite gets k = 1 because the
 * recorded projection already contains the Hor+ factor.
 *
 * The game also places HUD markers over world positions from that product
 * (sub_004ebec0: x / view width * 640, the fstp at 0x4ec35c).  The HUD is
 * shown squeezed into the 4:3 middle (HUDFIX), so the marker x gets the
 * inverse of that squeeze.
 *
 * Only screens wider than 4:3 (aspfix < 1) take this path; narrower ones keep
 * the renderer path.  NFSU2_FOV=renderer restores the old behaviour.        */
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

float com_aspfix(void);

static int g_mode = -1;              /* 1 = game projection, 0 = renderer */
static unsigned long g_proj, g_proj_wide, g_mark, g_mark_moved;

static void ws_init(void){
    if (g_mode >= 0) return;
    const char *e = getenv("NFSU2_FOV");
    g_mode = !(e && !strcasecmp(e, "renderer"));
    fprintf(stderr, "ws: Hor+ in the %s\n", g_mode
            ? "game's projection setup (culling matches the screen)"
            : "renderer (NFSU2_FOV=renderer)");
}

/* Game-side Hor+ active right now?  (the device aspect can change with the
 * resolution menu, so this is asked per call) */
int ws_game_fov(void){
    ws_init();
    float k = com_aspfix();
    return g_mode && k > 0.05f && k < 1.f;
}

/* Called from guest_hook (debug.c) BEFORE the instruction at ip runs; the
 * lifter writes the x87 region back first, so ST(0) is the live value. */
int ws_hook(CPU *c, uint32_t ip){
    switch (ip){
    case GA(0x005c7ffeu): {                  /* fstp [esp+1c] = P00 */
        g_proj++;
        if (!ws_game_fov()) return 1;
        uint32_t view = mem_is_committed(c->ebp + 4u) ? MR32(c->ebp + 4u) : 0u;
        if (view == 1u || view == 2u || view == 4u || view == 5u){
            ST(0) *= (double)com_aspfix();
            g_proj_wide++;
        }
        return 1; }
    case GA(0x004ec35cu): {                  /* fstp [esp+24] = marker x (640 units) */
        g_mark++;
        if (!ws_game_fov()) return 1;
        ST(0) = 320.0 + (ST(0) - 320.0) / (double)com_aspfix();
        g_mark_moved++;
        return 1; }
    }
    return 0;
}

void ws_report(void){
    ws_init();
    fprintf(stderr, "  widescreen fov   : %s; projection setups %lu (Hor+ %lu), HUD markers %lu (moved %lu)\n",
            g_mode ? "game" : "renderer", g_proj, g_proj_wide, g_mark, g_mark_moved);
}
