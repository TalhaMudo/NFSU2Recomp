/* Game controller support (Xbox / PlayStation / MFi pads) through Apple's GameController
 * framework.  The game polls DirectInput joysticks (EnumDevices GAMECTRL, a
 * c_dfDIJoystick2 = DIJOYSTATE2 data format, 272 bytes); com.c presents the pad as such
 * a device and fills the state here.  The layout is the one Windows gives an Xbox 360
 * pad in DirectInput, which is what the game's own controller screen expects:
 *   X/Y   left stick     Rx/Ry  right stick     Z  LT (+) / RT (-) combined
 *   POV0  d-pad (centidegrees, -1 centred)
 *   buttons 0 A, 1 B, 2 X, 3 Y, 4 LB, 5 RB, 6 Back/View, 7 Start/Menu, 8 LS, 9 RS
 * Axes are -10000..10000 (the game sets that range with SetProperty).
 *   NFSU2_PAD=0                off
 *   NFSU2_PAD_FAKE="X=0.5;B0=1;POV=9000"   synthetic pad (tests, no hardware)
 *   NFSU2_PAD_DEADZONE=0.10    radial stick dead zone (default 0.10)
 */
#import <Foundation/Foundation.h>
#import <GameController/GameController.h>
#include <math.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

int  gamepad_present(void);
void gamepad_fill(unsigned char *dst);
const char *gamepad_name(void);
void gamepad_install(void);
void gamepad_report(void);

#define DI_RANGE 10000.0f

static int   g_pad_off = 0;
static int   g_fake = 0;
static float g_fake_ax[8];
static int   g_fake_btn[16];
static int   g_fake_pov = -1;
static float g_dead = 0.10f;
static char  g_name[128] = "Xbox Controller";
static GCController *g_ctl = nil;
static unsigned long long g_polls = 0, g_pressed = 0;
static int   g_seen = 0;

static void parse_fake(const char *s){
    char *buf = strdup(s), *save = NULL;
    for (char *t = strtok_r(buf, ";", &save); t; t = strtok_r(NULL, ";", &save)){
        char *eq = strchr(t, '='); if (!eq) continue; *eq = 0;
        float v = (float)atof(eq + 1);
        static const char *ax[8] = {"X","Y","Z","RX","RY","RZ","S0","S1"};
        for (int i = 0; i < 8; i++) if (!strcmp(t, ax[i])) g_fake_ax[i] = v;
        if (t[0] == 'B' && t[1]){ int n = atoi(t + 1); if (n >= 0 && n < 16) g_fake_btn[n] = v != 0; }
        if (!strcmp(t, "POV")) g_fake_pov = (int)v;
    }
    free(buf);
}

static GCController *pick(void){
    for (GCController *c in [GCController controllers])
        if (c.extendedGamepad) return c;
    return nil;
}

void gamepad_install(void){
    const char *e = getenv("NFSU2_PAD");
    if (e && !strcmp(e, "0")){ g_pad_off = 1; return; }
    if ((e = getenv("NFSU2_PAD_DEADZONE"))) g_dead = (float)atof(e);
    if ((e = getenv("NFSU2_PAD_FAKE"))){ g_fake = 1; parse_fake(e); snprintf(g_name, sizeof g_name, "Fake Pad"); }
    @autoreleasepool {
        if ([GCController respondsToSelector:@selector(setShouldMonitorBackgroundEvents:)])
            GCController.shouldMonitorBackgroundEvents = YES;
        [[NSNotificationCenter defaultCenter]
            addObserverForName:GCControllerDidConnectNotification object:nil queue:nil
            usingBlock:^(NSNotification *n){
                GCController *c = n.object;
                fprintf(stderr, "[pad] connected: %s\n", c.vendorName.UTF8String ?: "controller");
            }];
        [[NSNotificationCenter defaultCenter]
            addObserverForName:GCControllerDidDisconnectNotification object:nil queue:nil
            usingBlock:^(NSNotification *n){
                fprintf(stderr, "[pad] disconnected\n");
            }];
    }
}

/* Is a pad available?  Controllers are discovered while the main run loop runs, which is
 * long before the game enumerates devices; a short wait covers a slow start. */
int gamepad_present(void){
    if (g_pad_off) return 0;
    if (g_fake) return 1;
    @autoreleasepool {
        for (int i = 0; i < 20 && !(g_ctl = pick()); i++) usleep(10000);
        g_ctl = pick();
        if (g_ctl){
            const char *n = g_ctl.vendorName.UTF8String;
            if (n && *n) snprintf(g_name, sizeof g_name, "%s", n);
            if (!g_seen){ g_seen = 1; fprintf(stderr, "[pad] using: %s (%s)\n", g_name,
                          g_ctl.productCategory.UTF8String ?: "?"); }
        }
    }
    return g_ctl != nil;
}

const char *gamepad_name(void){ return g_name; }

static int ax10k(float v){
    if (v > 1.f) v = 1.f; if (v < -1.f) v = -1.f;
    return (int)(v * DI_RANGE);
}

/* radial dead zone with rescale, so the stick leaves 0 smoothly */
static void stick(float x, float y, int *ox, int *oy){
    float m = sqrtf(x*x + y*y);
    if (m <= g_dead){ *ox = *oy = 0; return; }
    float k = (m - g_dead) / (1.f - g_dead); if (k > 1.f) k = 1.f;
    *ox = ax10k(x / m * k);
    *oy = ax10k(-(y / m) * k);                 /* DirectInput: forward = negative Y */
}

void gamepad_fill(unsigned char *dst){
    /* DIJOYSTATE2 = 272 bytes: lX lY lZ lRx lRy lRz, slider[2], POV[4], buttons[128], ... */
    int32_t *a = (int32_t *)dst;
    memset(dst, 0, 272);
    uint32_t *pov = (uint32_t *)(dst + 0x20);
    for (int i = 0; i < 4; i++) pov[i] = 0xFFFFFFFFu;
    unsigned char *btn = dst + 0x30;
    g_polls++;
    if (g_pad_off) return;
    if (g_fake){
        for (int i = 0; i < 8; i++) a[i] = ax10k(g_fake_ax[i]);
        for (int i = 0; i < 16; i++) btn[i] = g_fake_btn[i] ? 0x80 : 0;
        if (g_fake_pov >= 0) pov[0] = (uint32_t)g_fake_pov;
        return;
    }
    GCController *c = g_ctl;
    GCExtendedGamepad *g = c.extendedGamepad;
    if (!g) return;
    int lx, ly, rx, ry;
    stick(g.leftThumbstick.xAxis.value,  g.leftThumbstick.yAxis.value,  &lx, &ly);
    stick(g.rightThumbstick.xAxis.value, g.rightThumbstick.yAxis.value, &rx, &ry);
    a[0] = lx; a[1] = ly; a[3] = rx; a[4] = ry;
    a[2] = ax10k(g.leftTrigger.value - g.rightTrigger.value);   /* LT +, RT - */
    int up = g.dpad.up.pressed, dn = g.dpad.down.pressed, lf = g.dpad.left.pressed, rt = g.dpad.right.pressed;
    if (up || dn || lf || rt){
        int h = (up ? 1 : 0) - (dn ? 1 : 0), w = (rt ? 1 : 0) - (lf ? 1 : 0);
        static const int deg[3][3] = {   /* [down/none/up][left/none/right] */
            {22500, 18000, 13500}, {27000, -1, 9000}, {31500, 0, 4500} };
        int d = deg[h + 1][w + 1];
        if (d >= 0) pov[0] = (uint32_t)d;
    }
    GCControllerButtonInput *bs[10] = { g.buttonA, g.buttonB, g.buttonX, g.buttonY,
        g.leftShoulder, g.rightShoulder, g.buttonOptions, g.buttonMenu,
        g.leftThumbstickButton, g.rightThumbstickButton };
    for (int i = 0; i < 10; i++) if (bs[i] && bs[i].pressed){ btn[i] = 0x80; g_pressed++; }
}

void gamepad_report(void){
    if (g_pad_off) return;
    fprintf(stderr, "[pad] %s: %s, %llu state polls, %llu button-presses seen\n",
            g_fake ? "fake" : "GameController", g_ctl || g_fake ? g_name : "no controller",
            g_polls, g_pressed);
}
