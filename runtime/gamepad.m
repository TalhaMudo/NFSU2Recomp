/* Game controller support.  The game polls DirectInput joysticks (EnumDevices GAMECTRL, a
 * c_dfDIJoystick2 = DIJOYSTATE2 data format, 272 bytes); com.c presents the pad as such
 * a device and fills the state here.  The layout is the one Windows gives an Xbox 360
 * pad in DirectInput, which is what the game's own controller screen expects:
 *   X/Y   left stick     Rx/Ry  right stick     Z  LT (+) / RT (-) combined
 *   POV0  d-pad (centidegrees, -1 centred)
 *   buttons 0 A, 1 B, 2 X, 3 Y, 4 LB, 5 RB, 6 Back/View, 7 Start/Menu, 8 LS, 9 RS
 * Axes are -10000..10000 (the game sets that range with SetProperty).
 *
 * Pads come from two places:
 *   - Apple's GameController framework: Xbox, PlayStation, Switch and MFi pads.
 *   - IOKit HID, for the pads GameController does not present: DirectInput-mode pads
 *     (8BitDo D-input, Logitech "D" mode, generic USB pads; issue #53).  Their buttons
 *     and axes are mapped to the Xbox layout by runtime/padmap.c: the user's mapping,
 *     else the built-in table (SDL's macOS mappings), else a guess from the device's
 *     axes.  A GameController pad wins when both kinds are connected.
 *   NFSU2_PAD=0                off
 *   NFSU2_PAD_FAKE="X=0.5;B0=1;POV=9000"   synthetic pad (tests, no hardware)
 *   NFSU2_PAD_DEADZONE=0.10    radial stick dead zone (default 0.10)
 *   NFSU2_PAD_HID=0            no IOKit HID pads (GameController ones only)
 *   NFSU2_PAD_MAP=...          mappings for HID pads in SDL GameControllerDB form, tried
 *                              before the built-in ones: the lines themselves, or a file
 *                              (a path starting with / or ~).  Default: the game folder's
 *                              gamecontrollerdb.txt, if there is one.  A line without a
 *                              GUID ("a:b1,b:b2,...") applies to any HID pad.
 *   NFSU2_PAD_HIDSIM="B0=1@1000-1030;A5=1"  simulated HID pad (tests, no hardware;
 *                              hid_simulate)
 */
#import <Foundation/Foundation.h>
#import <GameController/GameController.h>
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hid/IOHIDUsageTables.h>
#include <math.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <os/lock.h>
#include "padmap.h"

int  gamepad_present(void);
void gamepad_fill(unsigned char *dst);
const char *gamepad_name(void);
void gamepad_install(void);
void gamepad_report(void);
int  fileio_game_path(const char *win, char *out, size_t outsz);   /* fileio.c */

#define DI_RANGE 10000.0f

static int   g_pad_off = 0;
static int   g_fake = 0;
static float g_fake_ax[8];
static int   g_fake_btn[16];
static int   g_fake_pov = -1;
static float g_dead = 0.10f;
static char  g_name[128] = "Xbox Controller";
static GCController *g_ctl = nil;
static int   g_di_hid = 0;                   /* DirectInput reads the HID pad (gamepad_present) */
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

/* The Xbox-layout state of a GameController pad (sticks up = positive). */
static void state_from_gc(GCExtendedGamepad *g, PadState *s){
    s->ax[PM_LX] = g.leftThumbstick.xAxis.value;   s->ax[PM_LY] = g.leftThumbstick.yAxis.value;
    s->ax[PM_RX] = g.rightThumbstick.xAxis.value;  s->ax[PM_RY] = g.rightThumbstick.yAxis.value;
    s->ax[PM_LT] = g.leftTrigger.value;            s->ax[PM_RT] = g.rightTrigger.value;
    GCControllerButtonInput *bs[PM_NBTN] = { g.buttonA, g.buttonB, g.buttonX, g.buttonY,
        g.leftShoulder, g.rightShoulder, g.buttonOptions, g.buttonMenu,
        g.leftThumbstickButton, g.rightThumbstickButton,
        g.dpad.up, g.dpad.down, g.dpad.left, g.dpad.right };
    s->btn = 0;
    for (int i = 0; i < PM_NBTN; i++) if (bs[i] && bs[i].pressed) s->btn |= (uint16_t)(1u << i);
}

/* XInput snapshot for gamepad_xinput_state.  The controller's value-changed handler
 * (or the HID pad's input callback) fills it on a queue of its own and the game thread
 * only copies it: GameController calls made from the game thread (+controllers in
 * particular) wait for the main thread, which is busy presenting frames, and stalled
 * the game and its audio. */
typedef struct { uint16_t buttons; uint8_t trig[2]; int16_t sticks[4]; int connected; } XSnap;
static os_unfair_lock    g_xs_lock = OS_UNFAIR_LOCK_INIT;
static XSnap             g_xs;
static GCController     *g_xs_ctl = nil;          /* main thread only */
static dispatch_queue_t  g_xs_queue;
static volatile int      g_gc_on;                 /* a GameController pad feeds g_xs */

static XSnap xs_from_state(const PadState *p){
    static const uint16_t xbit[PM_NBTN] = { 0x1000, 0x2000, 0x4000, 0x8000, 0x0100, 0x0200,
        0x0020, 0x0010, 0x0040, 0x0080, 0x0001, 0x0002, 0x0004, 0x0008 };
    #define S16(v) ((int16_t)lroundf(fmaxf(-1.f, fminf(1.f, (v))) * 32767.f))
    #define U8(v)  ((uint8_t)lroundf(fmaxf(0.f, fminf(1.f, (v))) * 255.f))
    XSnap s = { 0 };
    s.connected = 1;
    s.sticks[0] = S16(p->ax[PM_LX]);  s.sticks[1] = S16(p->ax[PM_LY]);
    s.sticks[2] = S16(p->ax[PM_RX]);  s.sticks[3] = S16(p->ax[PM_RY]);
    s.trig[0] = U8(p->ax[PM_LT]);     s.trig[1] = U8(p->ax[PM_RT]);
    for (int i = 0; i < PM_NBTN; i++) if (p->btn & (1u << i)) s.buttons |= xbit[i];
    #undef S16
    #undef U8
    return s;
}

static void xs_snapshot(GCExtendedGamepad *g){
    PadState p;
    state_from_gc(g, &p);
    XSnap s = xs_from_state(&p);
    os_unfair_lock_lock(&g_xs_lock); g_xs = s; os_unfair_lock_unlock(&g_xs_lock);
}

static void hid_publish(void);

static void xs_attach(GCController *c){              /* main thread */
    GCExtendedGamepad *g = c.extendedGamepad;
    if (!g || g_xs_ctl) return;
    g_xs_ctl = c;
    g_gc_on = 1;
    { const char *n = c.vendorName.UTF8String; if (n && *n) snprintf(g_name, sizeof g_name, "%s", n); }
    g_seen = 1;
    c.handlerQueue = g_xs_queue;
    g.valueChangedHandler = ^(GCExtendedGamepad *gp, GCControllerElement *el){ (void)el; xs_snapshot(gp); };
    dispatch_async(g_xs_queue, ^{ xs_snapshot(g); });
    fprintf(stderr, "[pad] reading %s (%s) for XtendedInput controls\n",
            c.vendorName.UTF8String ?: "controller", c.productCategory.UTF8String ?: "?");
}

static void xs_detach(GCController *c){              /* main thread */
    if (c != g_xs_ctl) return;
    c.extendedGamepad.valueChangedHandler = nil;
    g_xs_ctl = nil;
    g_gc_on = 0;
    dispatch_async(g_xs_queue, ^{                    /* after any snapshot still queued */
        os_unfair_lock_lock(&g_xs_lock); memset(&g_xs, 0, sizeof g_xs); os_unfair_lock_unlock(&g_xs_lock);
        hid_publish();                               /* a HID pad takes over, if there is one */
    });
    for (GCController *o in [GCController controllers]) if (o != c && o.extendedGamepad){ xs_attach(o); break; }
}

/* ---- IOKit HID pads -------------------------------------------------------------
 * An IOHIDManager on g_xs_queue only enumerates (Joystick / GamePad / MultiAxis
 * devices that GameController does not support); the pad in use is opened as a device
 * of its own, so no other device is opened or sends values here.  Its inputs are
 * numbered as SDL's IOKit driver numbers them (padmap.h). */
#define HID_MAXAX  32
#define HID_MAXBT  128
#define HID_MAXHAT 4
typedef struct { IOHIDElementRef el; uint32_t cookie, usage; long min, max; } HEl;
typedef struct {
    IOHIDDeviceRef mdev;                 /* the manager's device (enumeration, removal) */
    IOHIDDeviceRef dev;                  /* our own, opened, on g_xs_queue */
    int nax, nbt, nhat;
    HEl ax[HID_MAXAX], bt[HID_MAXBT], hat[HID_MAXHAT];
    float axv[HID_MAXAX];
    uint8_t axok[HID_MAXAX], btv[HID_MAXBT], hatv[HID_MAXHAT];
    PadMap map;
    char name[128];
} HPad;

static IOHIDManagerRef   g_hidm;
static CFMutableArrayRef g_hid_devs;     /* usable devices in arrival order; queue only */
static HPad             *g_hp;           /* the pad in use; queue only */
static char             *g_padmap_user;  /* NFSU2_PAD_MAP / gamecontrollerdb.txt */
static char              g_padmap_from[160];
static PadState          g_hid_st;       /* g_xs_lock */
static int               g_hid_on;       /* g_xs_lock */
static char              g_hid_name[128];/* g_xs_lock */

static long hid_num(IOHIDDeviceRef d, CFStringRef key){
    CFTypeRef v = IOHIDDeviceGetProperty(d, key);
    long n = 0;
    if (v && CFGetTypeID(v) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)v, kCFNumberLongType, &n);
    return n;
}

static void hid_str(IOHIDDeviceRef d, CFStringRef key, char *out, size_t n){
    CFTypeRef v = IOHIDDeviceGetProperty(d, key);
    out[0] = 0;
    if (v && CFGetTypeID(v) == CFStringGetTypeID())
        CFStringGetCString((CFStringRef)v, out, (CFIndex)n, kCFStringEncodingUTF8);
}

/* SDL's element lists: axes, buttons and hats, each sorted by usage (ties in arrival
 * order), duplicates by cookie skipped.  Mapping lines index into these. */
static void hid_add_el(HPad *h, IOHIDElementRef e){
    IOHIDElementType t = IOHIDElementGetType(e);
    if (t == kIOHIDElementTypeCollection){
        CFArrayRef ch = IOHIDElementGetChildren(e);
        if (ch) for (CFIndex i = 0; i < CFArrayGetCount(ch); i++)
            hid_add_el(h, (IOHIDElementRef)CFArrayGetValueAtIndex(ch, i));
        return;
    }
    if (t != kIOHIDElementTypeInput_Misc && t != kIOHIDElementTypeInput_Button &&
        t != kIOHIDElementTypeInput_Axis) return;
    uint32_t pg = IOHIDElementGetUsagePage(e), u = IOHIDElementGetUsage(e);
    HEl *list = NULL; int *n = NULL, cap = 0;
    #define AXES    { list = h->ax;  n = &h->nax;  cap = HID_MAXAX; }
    #define BUTTONS { list = h->bt;  n = &h->nbt;  cap = HID_MAXBT; }
    if (pg == kHIDPage_GenericDesktop){
        if (u >= kHIDUsage_GD_X && u <= kHIDUsage_GD_Wheel) AXES
        else if (u == kHIDUsage_GD_Hatswitch){ list = h->hat; n = &h->nhat; cap = HID_MAXHAT; }
        else if ((u >= kHIDUsage_GD_DPadUp && u <= kHIDUsage_GD_DPadLeft) || u == kHIDUsage_GD_Start ||
                 u == kHIDUsage_GD_Select || u == kHIDUsage_GD_SystemMainMenu) BUTTONS
    } else if (pg == kHIDPage_Simulation){
        if (u == kHIDUsage_Sim_Rudder || u == kHIDUsage_Sim_Throttle ||
            u == kHIDUsage_Sim_Accelerator || u == kHIDUsage_Sim_Brake) AXES
    } else if (pg == kHIDPage_Button || pg == kHIDPage_Consumer) BUTTONS
    #undef AXES
    #undef BUTTONS
    if (!list) return;
    uint32_t ck = (uint32_t)IOHIDElementGetCookie(e);
    for (int i = 0; i < *n; i++) if (list[i].cookie == ck) return;
    if (*n >= cap) return;
    int at = *n;
    while (at > 0 && list[at-1].usage > u) at--;
    memmove(list + at + 1, list + at, (size_t)(*n - at) * sizeof *list);
    list[at] = (HEl){ e, ck, u, (long)IOHIDElementGetLogicalMin(e), (long)IOHIDElementGetLogicalMax(e) };
    (*n)++;
}

static int hid_set(HPad *h, IOHIDElementRef e, long v){   /* 1: one of the pad's inputs */
    uint32_t ck = (uint32_t)IOHIDElementGetCookie(e);
    for (int i = 0; i < h->nax; i++) if (h->ax[i].cookie == ck){
        h->axv[i] = padmap_axis(v, &h->ax[i].min, &h->ax[i].max); h->axok[i] = 1; return 1; }
    for (int i = 0; i < h->nbt; i++) if (h->bt[i].cookie == ck){ h->btv[i] = v != 0; return 1; }
    for (int i = 0; i < h->nhat; i++) if (h->hat[i].cookie == ck){
        h->hatv[i] = padmap_hat(v, h->hat[i].min, h->hat[i].max); return 1; }
    return 0;
}

/* The HID pad's state for DirectInput, and for XtendedInput unless a GameController
 * pad feeds that.  Queue only. */
static void hid_publish(void){
    PadState s;
    memset(&s, 0, sizeof s);
    if (g_hp){
        PadRaw r = { g_hp->nax, g_hp->nbt, g_hp->nhat, g_hp->axv, g_hp->axok, g_hp->btv, g_hp->hatv };
        padmap_eval(&g_hp->map, &r, &s);
    }
    XSnap x = xs_from_state(&s);
    os_unfair_lock_lock(&g_xs_lock);
    g_hid_st = s;
    g_hid_on = g_hp != NULL;
    if (g_hp) snprintf(g_hid_name, sizeof g_hid_name, "%s", g_hp->name);
    if (!g_gc_on){ if (g_hp) g_xs = x; else memset(&g_xs, 0, sizeof g_xs); }
    os_unfair_lock_unlock(&g_xs_lock);
}

static void hid_value(void *ctx, IOReturn res, void *sender, IOHIDValueRef v){
    (void)ctx; (void)res; (void)sender;
    if (!g_hp) return;
    IOHIDElementRef e = IOHIDValueGetElement(v);
    if (e && IOHIDElementGetDevice(e) == g_hp->dev && hid_set(g_hp, e, IOHIDValueGetIntegerValue(v)))
        hid_publish();
}

static const char *const k_src[] = { "guessed from its axes", "built-in", "NFSU2_PAD_MAP (any pad)", "NFSU2_PAD_MAP" };

/* The pad's mapping: the user's, the built-in table's, or a guess from its axes. */
static void hid_map(HPad *h, PadId id, const char *transport, const char *note){
    char map[2048], dbname[128];
    int src = padmap_find(g_padmap_user, id, map, sizeof map, dbname, sizeof dbname);
    if (src == PM_SRC_NONE){
        uint32_t us[HID_MAXAX];
        for (int i = 0; i < h->nax; i++) us[i] = h->ax[i].usage;
        padmap_guess(us, h->nax, h->nbt, h->nhat, id.vid, map, sizeof map);
    }
    padmap_parse(map, &h->map);
    if (!h->name[0]) snprintf(h->name, sizeof h->name, "%s", dbname[0] ? dbname : "Game Controller");
    fprintf(stderr, "[pad] HID controller: %s (%04x:%04x v%04x %s; %d axes, %d buttons, %d hats)%s\n",
            h->name, id.vid, id.pid, id.ver, transport[0] ? transport : "?", h->nax, h->nbt, h->nhat, note);
    fprintf(stderr, "[pad]   mapping %s%s%s%s%s: %s\n", k_src[src],
            src >= PM_SRC_USER_ANY ? " from " : "", src >= PM_SRC_USER_ANY ? g_padmap_from : "",
            dbname[0] ? ", listed as " : "", dbname, map);
}

static void hid_use(IOHIDDeviceRef md){
    IOHIDDeviceRef d = IOHIDDeviceCreate(kCFAllocatorDefault, IOHIDDeviceGetService(md));
    if (!d) return;
    HPad *h = calloc(1, sizeof *h);
    h->mdev = (IOHIDDeviceRef)CFRetain(md);
    h->dev = d;
    hid_str(md, CFSTR(kIOHIDProductKey), h->name, sizeof h->name);
    CFArrayRef els = IOHIDDeviceCopyMatchingElements(d, NULL, kIOHIDOptionsTypeNone);
    if (els){
        for (CFIndex i = 0; i < CFArrayGetCount(els); i++)
            hid_add_el(h, (IOHIDElementRef)CFArrayGetValueAtIndex(els, i));
        CFRelease(els);
    }
    char transport[64];
    hid_str(md, CFSTR(kIOHIDTransportKey), transport, sizeof transport);
    PadId id = { (uint16_t)(strncmp(transport, "Bluetooth", 9) ? 3 : 5),
                 (uint16_t)hid_num(md, CFSTR(kIOHIDVendorIDKey)),
                 (uint16_t)hid_num(md, CFSTR(kIOHIDProductIDKey)),
                 (uint16_t)hid_num(md, CFSTR(kIOHIDVersionNumberKey)) };

    IOHIDDeviceSetDispatchQueue(d, g_xs_queue);
    IOHIDDeviceSetCancelHandler(d, ^{ CFRelease(d); });
    IOHIDDeviceRegisterInputValueCallback(d, hid_value, NULL);
    IOHIDDeviceActivate(d);
    IOReturn r = IOHIDDeviceOpen(d, kIOHIDOptionsTypeNone);
    /* what the device has already reported (a value never reported has no time stamp) */
    for (int k = 0; k < 3; k++){
        HEl *l = k == 0 ? h->ax : k == 1 ? h->bt : h->hat;
        int n = k == 0 ? h->nax : k == 1 ? h->nbt : h->nhat;
        for (int i = 0; i < n; i++){
            IOHIDValueRef v = NULL;
            if (IOHIDDeviceGetValueWithOptions(d, l[i].el, &v, kIOHIDDeviceGetValueWithoutUpdate) == kIOReturnSuccess
                && v && IOHIDValueGetTimeStamp(v))
                hid_set(h, l[i].el, IOHIDValueGetIntegerValue(v));
        }
    }
    hid_map(h, id, transport, r == kIOReturnSuccess ? "" : " -- cannot open it");
    g_hp = h;
    hid_publish();
}

/* NFSU2_PAD_HIDSIM="B0=1;A5=1;H0=1": no IOKit, a simulated 8BitDo Ultimate 2C in
 * D-input mode (2dc8:301b; axes X Y Z Rz Accelerator Brake, 16 buttons, a hat) whose
 * raw inputs, in SDL numbering, are as given -- a test of the mapping and of the HID
 * pad's way into DirectInput / XtendedInput without the hardware.  "B0=1@1000-1030"
 * holds an input only from the 1000th to the 1030th DirectInput poll of the pad
 * (about one per frame), so that the game sees it pressed and released. */
typedef struct { char k; int n; float v; long from, to; } SimEv;
static SimEv g_simev[16];
static int   g_nsimev;
static long  g_sim_polls;                              /* game thread */

static void sim_set(HPad *h, char k, int n, float v){
    if (k == 'A' && n >= 0 && n < h->nax) h->axv[n] = v;
    if (k == 'B' && n >= 0 && n < h->nbt) h->btv[n] = v != 0;
    if (k == 'H' && n >= 0 && n < h->nhat) h->hatv[n] = (uint8_t)v;
}

static void sim_poll(void){                            /* game thread, one DirectInput poll */
    long p = ++g_sim_polls;
    for (int i = 0; i < g_nsimev; i++){
        SimEv e = g_simev[i];
        if (p != e.from && p != e.to) continue;
        float v = p == e.from ? e.v : (e.k == 'A' && e.n >= 4 ? -1.f : 0.f);
        dispatch_async(g_xs_queue, ^{ if (g_hp){ sim_set(g_hp, e.k, e.n, v); hid_publish(); } });
    }
}

static void hid_simulate(const char *spec){
    static const uint32_t us[6] = { kHIDUsage_GD_X, kHIDUsage_GD_Y, kHIDUsage_GD_Z, kHIDUsage_GD_Rz,
                                    kHIDUsage_Sim_Accelerator, kHIDUsage_Sim_Brake };
    HPad *h = calloc(1, sizeof *h);
    h->nax = 6; h->nbt = 16; h->nhat = 1;
    for (int i = 0; i < 6; i++){ h->ax[i].usage = us[i]; h->axok[i] = 1; h->axv[i] = i >= 4 ? -1.f : 0.f; }
    snprintf(h->name, sizeof h->name, "8BitDo Ultimate 2C Wireless Controller (simulated)");
    char *buf = strdup(spec), *save = NULL;
    for (char *t = strtok_r(buf, ";", &save); t; t = strtok_r(NULL, ";", &save)){
        char *eq = strchr(t, '='), *at = strchr(t, '@');
        if (!eq || !t[1]) continue;
        SimEv e = { t[0], atoi(t + 1), (float)atof(eq + 1), 0, 0 };
        if (at && g_nsimev < 16){
            e.from = atol(at + 1);
            char *dash = strchr(at, '-');
            e.to = dash ? atol(dash + 1) : e.from + 15;
            g_simev[g_nsimev++] = e;
        } else sim_set(h, e.k, e.n, e.v);
    }
    free(buf);
    PadId id = { 3, 0x2dc8, 0x301b, 0x0001 };
    dispatch_async(g_xs_queue, ^{ hid_map(h, id, "simulated", ""); g_hp = h; hid_publish(); });
}

static void hid_drop(void){                            /* the pad in use is gone */
    HPad *h = g_hp;
    if (!h) return;
    g_hp = NULL;
    IOHIDDeviceClose(h->dev, kIOHIDOptionsTypeNone);
    IOHIDDeviceCancel(h->dev);                         /* the cancel handler releases it */
    CFRelease(h->mdev);
    free(h);
}

static void hid_choose(void){                          /* a game pad before a joystick */
    CFIndex n = CFArrayGetCount(g_hid_devs), best = -1;
    for (CFIndex i = 0; i < n && best < 0; i++)
        if (hid_num((IOHIDDeviceRef)CFArrayGetValueAtIndex(g_hid_devs, i), CFSTR(kIOHIDPrimaryUsageKey)) == kHIDUsage_GD_GamePad)
            best = i;
    if (best < 0 && n) best = 0;
    if (best >= 0) hid_use((IOHIDDeviceRef)CFArrayGetValueAtIndex(g_hid_devs, best));
}

static void hid_added(void *ctx, IOReturn res, void *sender, IOHIDDeviceRef d){
    (void)ctx; (void)res; (void)sender;
    char name[128];
    hid_str(d, CFSTR(kIOHIDProductKey), name, sizeof name);
    long vid = hid_num(d, CFSTR(kIOHIDVendorIDKey)), pid = hid_num(d, CFSTR(kIOHIDProductIDKey));
    @autoreleasepool {
        if (@available(macOS 11.0, *)) {
            if ([GCController supportsHIDDevice:d]){
                fprintf(stderr, "[pad] %s (%04lx:%04lx): through GameController\n", name[0] ? name : "HID device", vid, pid);
                return;
            }
        }
    }
    CFArrayAppendValue(g_hid_devs, d);
    fprintf(stderr, "[pad] HID connected: %s (%04lx:%04lx)\n", name[0] ? name : "game controller", vid, pid);
    if (!g_hp) hid_choose();
}

static void hid_removed(void *ctx, IOReturn res, void *sender, IOHIDDeviceRef d){
    (void)ctx; (void)res; (void)sender;
    CFIndex i = CFArrayGetFirstIndexOfValue(g_hid_devs, CFRangeMake(0, CFArrayGetCount(g_hid_devs)), d);
    if (i < 0) return;
    if (g_hp && g_hp->mdev == d){
        fprintf(stderr, "[pad] HID disconnected: %s\n", g_hp->name);
        hid_drop();
    }
    CFArrayRemoveValueAtIndex(g_hid_devs, i);
    if (!g_hp){ hid_choose(); if (!g_hp) hid_publish(); }
}

static char *read_text(const char *path){
    FILE *f = fopen(path, "rb");
    char *s = NULL;
    long n = -1;
    if (f && !fseek(f, 0, SEEK_END) && (n = ftell(f)) >= 0 && n < (8 << 20) && !fseek(f, 0, SEEK_SET)){
        s = calloc(1, (size_t)n + 1);
        if (s && fread(s, 1, (size_t)n, f) != (size_t)n){ free(s); s = NULL; }
    }
    if (f) fclose(f);
    return s;
}

static void hid_load_maps(void){
    const char *e = getenv("NFSU2_PAD_MAP");
    char path[1200] = "";
    if (e && *e){
        if (*e == '/' || *e == '~'){
            const char *home = getenv("HOME");
            if (*e == '~' && home) snprintf(path, sizeof path, "%s%s", home, e + 1);
            else snprintf(path, sizeof path, "%s", e);
        } else {
            g_padmap_user = strdup(e);
            snprintf(g_padmap_from, sizeof g_padmap_from, "the environment");
        }
    } else if (!fileio_game_path("gamecontrollerdb.txt", path, sizeof path))
        path[0] = 0;
    if (path[0]){
        g_padmap_user = read_text(path);
        snprintf(g_padmap_from, sizeof g_padmap_from, "%s", path);
        if (!g_padmap_user) fprintf(stderr, "[pad] cannot read %s\n", path);
        else fprintf(stderr, "[pad] controller mappings from %s\n", path);
    }
}

static void hid_install(void){
    hid_load_maps();
    const char *sim = getenv("NFSU2_PAD_HIDSIM");
    if (sim){ hid_simulate(sim); return; }
    g_hid_devs = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
    g_hidm = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if (!g_hidm || !g_hid_devs) return;
    NSArray *match = @[
        @{ @kIOHIDPrimaryUsagePageKey: @(kHIDPage_GenericDesktop), @kIOHIDPrimaryUsageKey: @(kHIDUsage_GD_GamePad) },
        @{ @kIOHIDPrimaryUsagePageKey: @(kHIDPage_GenericDesktop), @kIOHIDPrimaryUsageKey: @(kHIDUsage_GD_Joystick) },
        @{ @kIOHIDPrimaryUsagePageKey: @(kHIDPage_GenericDesktop), @kIOHIDPrimaryUsageKey: @(kHIDUsage_GD_MultiAxisController) } ];
    IOHIDManagerSetDeviceMatchingMultiple(g_hidm, (__bridge CFArrayRef)match);
    IOHIDManagerRegisterDeviceMatchingCallback(g_hidm, hid_added, NULL);
    IOHIDManagerRegisterDeviceRemovalCallback(g_hidm, hid_removed, NULL);
    IOHIDManagerSetDispatchQueue(g_hidm, g_xs_queue);
    IOHIDManagerActivate(g_hidm);
}

void gamepad_install(void){
    const char *e = getenv("NFSU2_PAD");
    if (e && !strcmp(e, "0")){ g_pad_off = 1; return; }
    if ((e = getenv("NFSU2_PAD_DEADZONE"))) g_dead = (float)atof(e);
    if ((e = getenv("NFSU2_PAD_FAKE"))){ g_fake = 1; parse_fake(e); snprintf(g_name, sizeof g_name, "Fake Pad"); }
    g_xs_queue = dispatch_queue_create("nfsu2.pad", DISPATCH_QUEUE_SERIAL);
    @autoreleasepool {
        if (@available(macOS 11.3, *))
            GCController.shouldMonitorBackgroundEvents = YES;
        [[NSNotificationCenter defaultCenter]
            addObserverForName:GCControllerDidConnectNotification object:nil queue:nil
            usingBlock:^(NSNotification *n){
                GCController *c = n.object;
                fprintf(stderr, "[pad] connected: %s\n", c.vendorName.UTF8String ?: "controller");
                xs_attach(c);
            }];
        [[NSNotificationCenter defaultCenter]
            addObserverForName:GCControllerDidDisconnectNotification object:nil queue:nil
            usingBlock:^(NSNotification *n){
                fprintf(stderr, "[pad] disconnected\n");
                xs_detach(n.object);
            }];
        dispatch_async(dispatch_get_main_queue(), ^{   /* pads connected before the start */
            GCController *c = pick(); if (c) xs_attach(c); });
    }
    if (!g_fake && !((e = getenv("NFSU2_PAD_HID")) && !strcmp(e, "0"))) hid_install();
}

static int hid_present(void){
    os_unfair_lock_lock(&g_xs_lock);
    int on = g_hid_on;
    if (on) snprintf(g_name, sizeof g_name, "%s", g_hid_name);
    os_unfair_lock_unlock(&g_xs_lock);
    return on;
}

/* Is a pad available?  Controllers are discovered while the main run loop runs, which is
 * long before the game enumerates devices; a short wait covers a slow start.  A
 * GameController pad is preferred to a HID one. */
int gamepad_present(void){
    if (g_pad_off) return 0;
    if (g_fake) return 1;
    @autoreleasepool {
        for (int i = 0; i < 20 && !(g_ctl = pick()) && !g_hid_on; i++) usleep(10000);
        g_ctl = pick();
        if (g_ctl){
            g_di_hid = 0;
            const char *n = g_ctl.vendorName.UTF8String;
            if (n && *n) snprintf(g_name, sizeof g_name, "%s", n);
            if (!g_seen){ g_seen = 1; fprintf(stderr, "[pad] using: %s (%s)\n", g_name,
                          g_ctl.productCategory.UTF8String ?: "?"); }
            return 1;
        }
    }
    g_di_hid = hid_present();
    if (g_di_hid && !g_seen){ g_seen = 1; fprintf(stderr, "[pad] using: %s (HID)\n", g_name); }
    return g_di_hid;
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
    PadState p;
    GCExtendedGamepad *g = g_ctl.extendedGamepad;
    if (g) state_from_gc(g, &p);
    else if (g_di_hid){
        if (g_nsimev) sim_poll();
        os_unfair_lock_lock(&g_xs_lock);
        int on = g_hid_on; p = g_hid_st;
        os_unfair_lock_unlock(&g_xs_lock);
        if (!on) return;
    }
    else return;
    int lx, ly, rx, ry;
    stick(p.ax[PM_LX], p.ax[PM_LY], &lx, &ly);
    stick(p.ax[PM_RX], p.ax[PM_RY], &rx, &ry);
    a[0] = lx; a[1] = ly; a[3] = rx; a[4] = ry;
    a[2] = ax10k(p.ax[PM_LT] - p.ax[PM_RT]);   /* LT +, RT - */
    int up = !!(p.btn & (1u << PM_UP)), dn = !!(p.btn & (1u << PM_DOWN)),
        lf = !!(p.btn & (1u << PM_LEFT)), rt = !!(p.btn & (1u << PM_RIGHT));
    if (up || dn || lf || rt){
        int h = (up ? 1 : 0) - (dn ? 1 : 0), w = (rt ? 1 : 0) - (lf ? 1 : 0);
        static const int deg[3][3] = {   /* [down/none/up][left/none/right] */
            {22500, 18000, 13500}, {27000, -1, 9000}, {31500, 0, 4500} };
        int d = deg[h + 1][w + 1];
        if (d >= 0) pov[0] = (uint32_t)d;
    }
    for (int i = 0; i < 10; i++) if (p.btn & (1u << i)){ btn[i] = 0x80; g_pressed++; }
}

/* XInput view of the pad for runtime/xtinput.c (XINPUT_GAMEPAD: wButtons bits, triggers
 * 0..255, sticks -32768..32767 with up = positive).  No dead zone: the caller applies its
 * own.  Returns 0 (and a neutral state) when no pad is connected.  Only copies the
 * snapshot above, so it never waits on GameController.
 * NFSU2_PAD_FAKE maps X/Y -> left stick, RX/RY -> right stick (DirectInput sense,
 * forward = negative), Z -> LT, RZ -> RT, B0..B9 -> A B X Y LB RB Back Start LS RS,
 * POV -> d-pad. */
int gamepad_xinput_state(uint16_t *buttons, uint8_t trig[2], int16_t sticks[4]){
    *buttons = 0; trig[0] = trig[1] = 0;
    for (int i = 0; i < 4; i++) sticks[i] = 0;
    if (g_pad_off) return 0;
    static const uint16_t xbit[10] = { 0x1000, 0x2000, 0x4000, 0x8000, 0x0100, 0x0200,
                                       0x0020, 0x0010, 0x0040, 0x0080 };
    #define S16(v) ((int16_t)lroundf(fmaxf(-1.f, fminf(1.f, (v))) * 32767.f))
    #define U8(v)  ((uint8_t)lroundf(fmaxf(0.f, fminf(1.f, (v))) * 255.f))
    if (g_fake){
        sticks[0] = S16(g_fake_ax[0]);  sticks[1] = S16(-g_fake_ax[1]);
        sticks[2] = S16(g_fake_ax[3]);  sticks[3] = S16(-g_fake_ax[4]);
        trig[0] = U8(fabsf(g_fake_ax[2]));  trig[1] = U8(fabsf(g_fake_ax[5]));
        for (int i = 0; i < 10; i++) if (g_fake_btn[i]) *buttons |= xbit[i];
        switch (g_fake_pov){                     /* d-pad: up 1, down 2, left 4, right 8 */
        case 0: *buttons |= 1; break;      case 4500: *buttons |= 1|8; break;
        case 9000: *buttons |= 8; break;   case 13500: *buttons |= 2|8; break;
        case 18000: *buttons |= 2; break;  case 22500: *buttons |= 2|4; break;
        case 27000: *buttons |= 4; break;  case 31500: *buttons |= 1|4; break;
        default: break; }
        return 1;
    }
    #undef S16
    #undef U8
    os_unfair_lock_lock(&g_xs_lock); XSnap s = g_xs; os_unfair_lock_unlock(&g_xs_lock);
    g_polls++;
    *buttons = s.buttons; trig[0] = s.trig[0]; trig[1] = s.trig[1];
    for (int i = 0; i < 4; i++) sticks[i] = s.sticks[i];
    return s.connected;
}

void gamepad_report(void){
    if (g_pad_off) return;
    fprintf(stderr, "[pad] %s: %s, %llu state polls, %llu button-presses seen\n",
            g_fake ? "fake" : g_di_hid ? "HID" : "GameController",
            g_ctl || g_fake || g_seen ? g_name : "no controller",
            g_polls, g_pressed);
}
