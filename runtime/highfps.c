/* High-FPS fixes (v2.1): the frame-rate parts of ThirteenAG's NFSU2 Widescreen Fix,
 * re-implemented for the recompiled game.
 *
 * The port runs races at the display rate (120 Hz) while the game was made for 60 or
 * less.  Nothing of the fix's code is used here: its source was read for what it
 * changes, then each spot was found and checked by disassembling the European EXE.
 *
 *   tick       xpatch hf_tick     0x57a094  PrepareRealTimestep adds at least one 1/60 s
 *                                           tick per frame: at 120 fps tick-driven game
 *                                           time ran twice as fast.  jne -> jmp: a frame
 *                                           can have 0 ticks, the remainder carries over.
 *   particles  hook 0x5d2c27              the particle update gets a fixed 1/60 s step
 *                                           (push 0x3c888889) every frame: smoke and sparks
 *                                           ran twice as fast.  The step becomes the frame's
 *                                           real time [0x865194] (clamped to 0..0.05 s).
 *   nos        hook 0x61ad90              UpdateCarStreakingFlares records the car
 *                                           positions the NOS trail is drawn through once
 *                                           per frame (it returns when [0x8a44ec] equals
 *                                           the frame counter [0x870818]): at 120 fps the
 *                                           trail was half as long.  It now records every
 *                                           1/60 s * CustomNOSTrailLength; on the other
 *                                           frames the hook sets [0x8a44ec] so it returns.
 *   intro60    xpatch hf_intro60  0x435fa4  NIS scenes (race intros) set the frame target to
 *                                           1/30 s; 60.0 instead (HighFPSCutscenes).  Not
 *                                           more: above 60 the race intro camera goes wrong.
 *   borders    hooks 0x5cbe54, 0x5c804e   the intro letterbox: the bar height the game
 *                                           draws (sub_5cbdd0, [esp+0xc], from the player
 *                                           camera +0xc8) becomes 0, and the player view's
 *                                           projection (sub_5c7ed0, [esp+0x28] / [esp+0x38])
 *                                           is zoomed by 1 / (1 - 2 * height) so the picture
 *                                           between the bars fills the screen
 *                                           (DisableCutsceneBorders).
 *
 * Settings: $NFSU2_SCRIPTS/NFSUnderground2.WidescreenFix.ini (any section):
 * HighFPSCutscenes, DisableCutsceneBorders, FixNOSTrailLength, CustomNOSTrailLength.
 * Without the ini: tick, particles and nos on, intros at 30 fps, borders kept.
 *
 *   NFSU2_HIGHFPS=0                          all off
 *   NFSU2_HIGHFPS_OFF=tick,particles,nos,intro60,borders   single fixes off
 *   NFSU2_HIGHFPS_SET="Sec.Key=v;..."        replaces ini values (tests, A/B) */
#include "cpu.h"
#include "xpatch.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#if defined(NFS_EXE_NA) || !defined(XP_HF_TICK)
void highfps_install(void){}
int  highfps_hook(CPU *c, uint32_t ip){ (void)c; (void)ip; return 0; }
void highfps_report(void){}
#else

extern unsigned char g_xpatch[];

/* ---- European v1.2 addresses ---- */
#define A_REAL_STEP    0x00865194u   /* the frame's real time step, seconds          */
#define A_FRAME        0x00870818u   /* frame counter                                */
#define A_NOS_LAST     0x008a44ecu   /* frame UpdateCarStreakingFlares last ran      */
#define HOOK_PARTICLE  0x005d2c27u   /* call 0x492e10, [esp] = 1/60 step             */
#define HOOK_NOS       0x0061ad90u   /* UpdateCarStreakingFlares: mov eax, [0x870818] */
#define HOOK_BARS      0x005cbe54u   /* fld [esp+0xc]: letterbox bar height          */
#define HOOK_PROJ      0x005c804eu   /* fld [esp+0x38]: view scales ready            */
#define BAR_FULL       0.125f        /* bar height when the letterbox is in          */

static struct { char sec[16], key[32], val[32]; } g_ini[48];
static int g_nini;

static void trim(char *s){
    char *p = s; while (*p && isspace((unsigned char)*p)) p++;
    memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s); while (n && isspace((unsigned char)s[n-1])) s[--n] = 0;
}
static void ini_put(const char *sec, const char *key, const char *val){
    int i = 0;
    while (i < g_nini && (strcasecmp(g_ini[i].sec, sec) || strcasecmp(g_ini[i].key, key))) i++;
    if (i == 48) return;
    if (i == g_nini) g_nini++;
    snprintf(g_ini[i].sec, sizeof g_ini[i].sec, "%s", sec);
    snprintf(g_ini[i].key, sizeof g_ini[i].key, "%s", key);
    snprintf(g_ini[i].val, sizeof g_ini[i].val, "%s", val);
}
static int ini_load(const char *path){
    FILE *f = fopen(path, "r"); if (!f) return 0;
    char line[256], sec[16] = "";
    while (fgets(line, sizeof line, f)){
        char *c = strchr(line, ';'); if (c) *c = 0;
        trim(line);
        if (line[0] == '['){
            char *e = strchr(line, ']'); if (e) *e = 0;
            snprintf(sec, sizeof sec, "%s", line + 1); continue; }
        char *eq = strchr(line, '='); if (!eq) continue;
        *eq = 0; trim(line); trim(eq + 1);
        ini_put(sec, line, eq + 1);
    }
    fclose(f); return 1;
}
static void ini_override(const char *spec){
    char buf[512]; snprintf(buf, sizeof buf, "%s", spec);
    for (char *it, *p = strtok_r(buf, ";", &it); p; p = strtok_r(NULL, ";", &it)){
        char *dot = strchr(p, '.'), *eq = strchr(p, '=');
        if (!dot || !eq || dot > eq) continue;
        *dot = 0; *eq = 0; trim(p); trim(dot + 1); trim(eq + 1);
        ini_put(p, dot + 1, eq + 1);
        fprintf(stderr, "  [highfps] override %s.%s = %s\n", p, dot + 1, eq + 1);
    }
}
/* the fix's versions keep the same keys in different sections: look in all of them */
static const char *ini_any(const char *key){
    for (int i = 0; i < g_nini; i++)
        if (!strcasecmp(g_ini[i].key, key)) return g_ini[i].val;
    return NULL;
}
static int   ini_int(const char *k, int d){ const char *v = ini_any(k); return v ? atoi(v) : d; }
static float ini_flt(const char *k, float d){ const char *v = ini_any(k); return v ? (float)atof(v) : d; }

static int g_particles, g_nos, g_borders;
static float g_nos_interval = 1.0f / 60.0f, g_nos_acc;
static uint32_t g_nos_frame = 0xffffffffu;
static uint64_t g_n_particle, g_n_nos_run, g_n_nos_skip, g_n_bars, g_n_zoom;
static char g_on_list[120];

static void on(const char *name){
    size_t n = strlen(g_on_list);
    snprintf(g_on_list + n, sizeof g_on_list - n, "%s%s", n ? " " : "", name);
}
static int off_listed(const char *list, const char *name){
    if (!list) return 0;
    size_t n = strlen(name);
    for (const char *p = list; (p = strstr(p, name)); p += n)
        if ((p == list || p[-1] == ',') && (p[n] == 0 || p[n] == ',')) return 1;
    return 0;
}

void highfps_install(void){
    const char *e = getenv("NFSU2_HIGHFPS");
    if (e && !strcmp(e, "0")){ fprintf(stderr, "  [highfps] off (NFSU2_HIGHFPS=0)\n"); return; }
    char path[1024] = "";
    const char *dir = getenv("NFSU2_SCRIPTS");
    if (dir && *dir){
        snprintf(path, sizeof path, "%s/NFSUnderground2.WidescreenFix.ini", dir);
        if (!ini_load(path)) path[0] = 0;
    }
    const char *set = getenv("NFSU2_HIGHFPS_SET");
    if (set && *set) ini_override(set);
    const char *offl = getenv("NFSU2_HIGHFPS_OFF");

    /* the code at the sites must be the expected one */
    if (MR8(0x0057a094u) != 0x75 || MR32(0x00435fa4u) != 0x41f00000u ||
        MR32(HOOK_PARTICLE - 4u) != 0x835860u || MR32(HOOK_PARTICLE - 9u) != 0x3c888889u ||
        MR8(HOOK_NOS) != 0xa1 || MR32(HOOK_NOS + 1u) != A_FRAME || MR32(HOOK_NOS + 7u) != A_NOS_LAST ||
        MR32(HOOK_BARS) != 0x0c2444d9u || MR32(HOOK_PROJ) != 0x382444d9u){
        fprintf(stderr, "  [highfps] unexpected code at the patch sites: off\n");
        return;
    }
    if (!off_listed(offl, "tick")){ g_xpatch[XP_HF_TICK] = 1; on("tick"); }
    if (!off_listed(offl, "particles")){ g_particles = 1; on("particles"); }
    if (!off_listed(offl, "nos") && ini_int("FixNOSTrailLength", 1)){
        float len = ini_flt("CustomNOSTrailLength", 1.0f);
        if (!(len > 0.1f)) len = 0.1f;
        if (len > 10.f) len = 10.f;
        g_nos_interval = len / 60.0f;
        g_nos = 1; on("nos");
    }
    if (!off_listed(offl, "intro60") && ini_int("HighFPSCutscenes", 0)){
        g_xpatch[XP_HF_INTRO60] = 1; on("intro60");
    }
    if (!off_listed(offl, "borders") && ini_int("DisableCutsceneBorders", 0)){ g_borders = 1; on("borders"); }
    fprintf(stderr, "  [highfps] %s: %s\n", path[0] ? path : "no WidescreenFix ini",
            g_on_list[0] ? g_on_list : "nothing enabled");
}

/* the letterbox height the projection is zoomed for: the whole bar as soon as the scene
 * slides the bars in, so the framing doesn't zoom along with the slide */
static float bar_height(float h){
    if (!(h > 0.f)) return 0.f;
    if (h < BAR_FULL) h = BAR_FULL;
    return h > 0.45f ? 0.45f : h;
}

int highfps_hook(CPU *c, uint32_t ip){
    if (ip == HOOK_PARTICLE){
        if (g_particles){
            float dt = MRF32(A_REAL_STEP);
            if (!(dt > 0.f)) dt = 0.f;
            if (dt > 0.05f) dt = 0.05f;
            MRF32(c->esp) = dt;
            g_n_particle++;
        }
        return 1;
    }
    if (ip == HOOK_NOS){
        if (g_nos){
            uint32_t f = MR32(A_FRAME);
            if (f != g_nos_frame){            /* first call this frame (views, mirror) */
                g_nos_frame = f;
                float dt = MRF32(A_REAL_STEP);
                if (dt > 0.f && dt < 0.25f) g_nos_acc += dt;
                if (g_nos_acc >= g_nos_interval){
                    g_nos_acc = fmodf(g_nos_acc, g_nos_interval);
                    g_n_nos_run++;            /* the function records this frame */
                } else {
                    MR32(A_NOS_LAST) = f;     /* the function sees "already done" */
                    g_n_nos_skip++;
                }
            }
        }
        return 1;
    }
    if (ip == HOOK_BARS){
        if (g_borders && MRF32(c->esp + 0xcu) > 0.f){
            MRF32(c->esp + 0xcu) = 0.f;
            g_n_bars++;
        }
        return 1;
    }
    if (ip == HOOK_PROJ){
        /* ebp = view (id at +4, 1 = the player's), edi = its camera */
        if (g_borders && MR32(c->ebp + 4u) == 1u){
            float h = bar_height(MRF32(c->edi + 0xc8u));
            if (h > 0.f){
                float zoom = 1.f / (1.f - 2.f * h);
                MRF32(c->esp + 0x38u) *= zoom;
                MRF32(c->esp + 0x28u) *= zoom;
                g_n_zoom++;
            }
        }
        return 1;
    }
    return 0;
}

void highfps_report(void){
    if (!g_on_list[0]) return;
    fprintf(stderr, "  [highfps] on: %s; particle steps %llu, NOS trail records %llu (skipped %llu), "
            "letterbox removed %llu frames, zoomed %llu views\n", g_on_list,
            (unsigned long long)g_n_particle, (unsigned long long)g_n_nos_run,
            (unsigned long long)g_n_nos_skip, (unsigned long long)g_n_bars,
            (unsigned long long)g_n_zoom);
}
#endif
