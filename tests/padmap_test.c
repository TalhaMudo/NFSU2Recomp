/* Checks runtime/padmap.c, the mappings of IOKit HID game pads (issue #53).
 *   clang -Iruntime -o out/padmap_test tests/padmap_test.c runtime/padmap.c && out/padmap_test
 * The raw inputs below are what runtime/gamepad.m builds from a device: axes, buttons
 * and hats in SDL's IOKit order. */
#include "padmap.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_fail = 0, g_n = 0;
#define CHECK(c) do { g_n++; if (!(c)){ g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define NEAR(a, b) (fabsf((a) - (b)) < 1e-3f)

typedef struct { float ax[8]; uint8_t ok[8], bt[32], hat[2]; int nax, nbt, nhat; } Dev;

static PadState run(const char *map, const Dev *d){
    PadMap m; PadState s;
    padmap_parse(map, &m);
    PadRaw r = { d->nax, d->nbt, d->nhat, d->ax, d->ok, d->bt, d->hat };
    padmap_eval(&m, &r, &s);
    return s;
}

/* 8BitDo Ultimate 2C in D-input mode: X Y Z Rz Accelerator Brake, 15+ buttons, a hat */
static Dev ultimate2c(void){
    Dev d; memset(&d, 0, sizeof d);
    d.nax = 6; d.nbt = 16; d.nhat = 1;
    for (int i = 0; i < 6; i++) d.ok[i] = 1;
    d.ax[4] = d.ax[5] = -1.f;                 /* triggers at rest: logical minimum */
    return d;
}

int main(void){
    char map[2048], name[128];

    /* --- the issue's pad: found in the built-in table ------------------------- */
    PadId u2c = { 3, 0x2dc8, 0x301b, 0x0001 };
    CHECK(padmap_find(NULL, u2c, map, sizeof map, name, sizeof name) == PM_SRC_BUILTIN);
    CHECK(!strcmp(name, "8BitDo Ultimate 2C Wireless"));
    {   /* a different firmware version or Bluetooth still finds it */
        PadId v = u2c; v.ver = 0x0110; v.bus = 5;
        char m2[2048];
        CHECK(padmap_find(NULL, v, m2, sizeof m2, NULL, 0) == PM_SRC_BUILTIN && !strcmp(m2, map));
    }
    Dev d = ultimate2c();
    PadState s = run(map, &d);
    CHECK(s.btn == 0 && NEAR(s.ax[PM_LT], 0) && NEAR(s.ax[PM_RT], 0) && NEAR(s.ax[PM_LX], 0));
    d.bt[0] = 1; s = run(map, &d); CHECK(s.btn == 1u << PM_A);  d.bt[0] = 0;
    d.bt[1] = 1; s = run(map, &d); CHECK(s.btn == 1u << PM_B);  d.bt[1] = 0;
    d.bt[3] = 1; s = run(map, &d); CHECK(s.btn == 1u << PM_X);  d.bt[3] = 0;
    d.bt[4] = 1; s = run(map, &d); CHECK(s.btn == 1u << PM_Y);  d.bt[4] = 0;
    d.bt[6] = d.bt[7] = 1; s = run(map, &d); CHECK(s.btn == (1u << PM_LB | 1u << PM_RB)); d.bt[6] = d.bt[7] = 0;
    d.bt[10] = 1; s = run(map, &d); CHECK(s.btn == 1u << PM_BACK);  d.bt[10] = 0;
    d.bt[11] = 1; s = run(map, &d); CHECK(s.btn == 1u << PM_START); d.bt[11] = 0;
    d.bt[13] = d.bt[14] = 1; s = run(map, &d); CHECK(s.btn == (1u << PM_LS | 1u << PM_RS)); d.bt[13] = d.bt[14] = 0;
    d.bt[2] = d.bt[5] = d.bt[12] = 1; s = run(map, &d); CHECK(s.btn == 0);   /* paddles, guide: unused */
    d.bt[2] = d.bt[5] = d.bt[12] = 0;
    d.ax[5] = 1.f; s = run(map, &d); CHECK(NEAR(s.ax[PM_LT], 1) && NEAR(s.ax[PM_RT], 0));   /* Brake = LT */
    d.ax[5] = -1.f; d.ax[4] = 0.f; s = run(map, &d); CHECK(NEAR(s.ax[PM_RT], 0.5f));         /* Accelerator = RT */
    d.ax[4] = -1.f;
    d.ax[1] = -1.f; s = run(map, &d); CHECK(NEAR(s.ax[PM_LY], 1));        /* HID up = minimum -> up */
    d.ax[1] = 0.f; d.ax[0] = 0.5f; s = run(map, &d); CHECK(NEAR(s.ax[PM_LX], 0.5f));
    d.ax[0] = 0.f; d.ax[2] = -1.f; d.ax[3] = 1.f; s = run(map, &d);
    CHECK(NEAR(s.ax[PM_RX], -1) && NEAR(s.ax[PM_RY], -1));                 /* right stick on Z / Rz */
    d.ax[2] = d.ax[3] = 0.f;
    d.hat[0] = 1|2; s = run(map, &d); CHECK(s.btn == (1u << PM_UP | 1u << PM_RIGHT));
    d.hat[0] = 4; s = run(map, &d); CHECK(s.btn == 1u << PM_DOWN);
    d.hat[0] = 0;
    { Dev n = ultimate2c(); n.ok[0] = n.ok[1] = 0; n.ax[0] = n.ax[1] = -1.f;   /* not reported yet */
      s = run(map, &n); CHECK(NEAR(s.ax[PM_LX], 0) && NEAR(s.ax[PM_LY], 0)); }

    /* --- the user's mappings come first --------------------------------------- */
    const char *user =
        "# comment\n"
        "03000000c82d00001b30000000000000,Mine,a:b1,b:b0,platform:Windows,\n"   /* not for macOS */
        "03000000c82d00001b30000000000000,Mine,a:b2,b:b0,platform:Mac OS X,\r\n";
    CHECK(padmap_find(user, u2c, map, sizeof map, name, sizeof name) == PM_SRC_USER);
    CHECK(!strcmp(name, "Mine") && !strncmp(map, "a:b2,b:b0", 9));
    CHECK(padmap_find("a:b5,b:b6\n", u2c, map, sizeof map, NULL, 0) == PM_SRC_USER_ANY && !strcmp(map, "a:b5,b:b6"));
    CHECK(padmap_find("*,Any,x:b9,", u2c, map, sizeof map, name, sizeof name) == PM_SRC_USER_ANY
          && !strcmp(name, "Any") && !strcmp(map, "x:b9,"));
    /* HIDAPI / GameController GUIDs number the inputs differently: not used */
    CHECK(padmap_find("03000000c82d00001b30000001006800,H,a:b7,", u2c, map, sizeof map, NULL, 0) == PM_SRC_BUILTIN);

    /* --- an unknown device: a mapping from its axes ---------------------------- */
    PadId unk = { 3, 0x1234, 0x5678, 1 };
    CHECK(padmap_find(NULL, unk, map, sizeof map, NULL, 0) == PM_SRC_NONE);
    {   /* Logitech-style DirectInput pad: X Y Z Rz, 12 buttons, a hat */
        uint32_t us[4] = { 0x30, 0x31, 0x32, 0x35 };
        padmap_guess(us, 4, 12, 1, unk.vid, map, sizeof map);
        Dev g; memset(&g, 0, sizeof g); g.nax = 4; g.nbt = 12; g.nhat = 1;
        for (int i = 0; i < 4; i++) g.ok[i] = 1;
        g.bt[1] = 1; s = run(map, &g); CHECK(s.btn == 1u << PM_A); g.bt[1] = 0;
        g.bt[0] = 1; s = run(map, &g); CHECK(s.btn == 1u << PM_X); g.bt[0] = 0;
        g.bt[6] = 1; s = run(map, &g); CHECK(NEAR(s.ax[PM_LT], 1)); g.bt[6] = 0;
        g.bt[9] = 1; s = run(map, &g); CHECK(s.btn == 1u << PM_START); g.bt[9] = 0;
        g.ax[3] = -1.f; s = run(map, &g); CHECK(NEAR(s.ax[PM_RY], 1));
    }
    {   /* an 8BitDo the table does not list: its D-input order, analog triggers */
        uint32_t us[6] = { 0x30, 0x31, 0x32, 0x35, 0xC4, 0xC5 };
        padmap_guess(us, 6, 15, 1, 0x2dc8, map, sizeof map);
        Dev g = ultimate2c();
        g.bt[0] = 1; s = run(map, &g); CHECK(s.btn == 1u << PM_A); g.bt[0] = 0;
        g.bt[4] = 1; s = run(map, &g); CHECK(s.btn == 1u << PM_Y); g.bt[4] = 0;
        g.ax[5] = 1.f; s = run(map, &g); CHECK(NEAR(s.ax[PM_LT], 1) && NEAR(s.ax[PM_RT], 0));
    }

    /* --- half and inverted axes, axis buttons, half outputs -------------------- */
    {
        Dev g; memset(&g, 0, sizeof g); g.nax = 4; g.nbt = 4; g.nhat = 1;
        for (int i = 0; i < 4; i++) g.ok[i] = 1;
        const char *m = "lefttrigger:+a3,righttrigger:-a3,lefty:a1~,dpdown:+a2,dpup:-a2,"
                        "+leftx:h0.2,-leftx:h0.8,";
        g.ax[3] = 0.6f;  s = run(m, &g); CHECK(NEAR(s.ax[PM_LT], 0.6f) && NEAR(s.ax[PM_RT], 0));
        g.ax[3] = -0.4f; s = run(m, &g); CHECK(NEAR(s.ax[PM_LT], 0) && NEAR(s.ax[PM_RT], 0.4f));
        g.ax[3] = 0.f;
        g.ax[1] = 1.f;   s = run(m, &g); CHECK(NEAR(s.ax[PM_LY], 1));       /* ~: up = maximum */
        g.ax[1] = 0.f;
        g.ax[2] = 0.9f;  s = run(m, &g); CHECK(s.btn == 1u << PM_DOWN);
        g.ax[2] = -0.9f; s = run(m, &g); CHECK(s.btn == 1u << PM_UP);
        g.ax[2] = 0.2f;  s = run(m, &g); CHECK(s.btn == 0);
        g.ax[2] = 0.f;
        g.hat[0] = 2; s = run(m, &g); CHECK(NEAR(s.ax[PM_LX], 1));
        g.hat[0] = 8; s = run(m, &g); CHECK(NEAR(s.ax[PM_LX], -1));
    }
    {   PadMap m; CHECK(padmap_parse("a:b1,guide:b12,bogus,lefttrigger:x3,dpup:h0.0,b:b300,x:b2~,y:b3", &m) == 2); }

    /* --- HID value conversions ----------------------------------------------- */
    { long lo = 0, hi = 255;
      CHECK(NEAR(padmap_axis(0, &lo, &hi), -1) && NEAR(padmap_axis(255, &lo, &hi), 1));
      CHECK(fabsf(padmap_axis(128, &lo, &hi)) < 0.01f);
      CHECK(NEAR(padmap_axis(511, &lo, &hi), 1) && hi == 511);              /* range widens */
      lo = hi = 5; CHECK(padmap_axis(5, &lo, &hi) == 0.f); }
    CHECK(padmap_hat(0, 0, 7) == 1 && padmap_hat(2, 0, 7) == 2 && padmap_hat(5, 0, 7) == 12);
    CHECK(padmap_hat(8, 0, 7) == 0 && padmap_hat(15, 0, 7) == 0);           /* null state */
    CHECK(padmap_hat(1, 0, 3) == 2 && padmap_hat(3, 0, 3) == 8);           /* 4-way hat */
    CHECK(padmap_hat(1, 1, 8) == 1 && padmap_hat(0, 1, 8) == 0);           /* 1-based */
    CHECK(padmap_hat(3, 0, 5) == 0);                                       /* odd range */

    printf("padmap: %d/%d checks passed\n", g_n - g_fail, g_n);
    return g_fail != 0;
}
