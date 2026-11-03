/* xtinput.c - NFSU-XtendedInput's controller handling, built into the runtime.
 *
 * Port of the GAME_UG2 path of xan1242's NFSU-XtendedInput (dllmain.cpp,
 * github.com/xan1242/NFSU-XtendedInput, 2023-01-29 build), MIT License,
 * Copyright (c) 2022 Lovro Pleše (full notice in runtime/xtinput_events.h).
 * With it the pad behaves as it does under Windows with that plugin installed:
 * RT and LT are separate analog throttle and brake, and every game event
 * (driving, menus, car select, debug camera) is bound in the plugin's own
 * NFSU_XtendedInput.ini, [Events] for the pad and [EventsKB] for the keyboard.
 *
 * How the plugin plugs in, and what this file does in its place:
 *   - FindScannerConfig (0x572120) chooses, per JoyEvent, the "scanner" that
 *     the event generator (0x57B0B0) calls each poll.  The plugin returns its
 *     own ScannerConfig for every event; so does the override below.  The
 *     configs live in guest memory and all point at one native entry
 *     (XT_SCANNER_THUNK, a dispatch_patch address) that runs the plugin's scanner
 *     for that event.  A scanner returns JoyEvent | value << 8, or 0 for "nothing".
 *   - RealDriver registers 0x407750 as the handler for JOY_EVENT_STEER (0x8F) and
 *     STEER_ANALOG (0x90); the plugin replaces the first with its own steering
 *     (left stick + keys, written as a float to RealDriver+0x2C8) and the second
 *     with a no-op.  Here a dispatch_patch on 0x407750 does both.
 *   - The plugin reads the pad once per frame; here it is read on the first
 *     scanner call of each poll.  Back sends 'Q' to the front end, as there.
 *   - The keyboard goes through the same scanners (key state from the window,
 *     [EventsKB] names mapped to DirectInput key codes), because the game's own
 *     keyboard scanners are no longer used.  The plugin's mouse and raw-input
 *     code is not needed: the runtime's DirectInput mouse is left alone.
 *   - The pad is no longer offered to the game as a DirectInput device (com.c),
 *     just as the plugin keeps DirectInput from seeing XInput pads.
 * Addresses are the North American v1.2 SPEED2.EXE's (the plugin's own); other
 * builds compile this file to stubs.
 * It is off unless switched on; the launcher's "XtendedInput layout" switch sets
 * NFSU2_XTINPUT.  Settings come from the game folder's scripts/NFSU_XtendedInput.ini
 * (a Windows install with the plugin already has one), otherwise from the plugin's
 * stock ini built in here (runtime/xtinput_default_ini.h).
 *   NFSU2_XTINPUT=1          on (otherwise the pad is the DirectInput one of gamepad.m)
 *   NFSU2_XTINPUT_INI=path   ini to use instead of <game>/scripts/NFSU_XtendedInput.ini
 *   NFSU2_XTINPUT_LOG=1      log every scanner result that isn't 0           */
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>

int  xtinput_active(void);
void xtinput_install(void);
void xtinput_report(void);

#if defined(NFS_EXE_NA)
#include "xtinput_events.h"
#include "xtinput_default_ini.h"

/* NA function names are pasted together so tools/addrmap.py (which maps the
 * European sub_XXXXXXXX names used elsewhere in the runtime) leaves them alone. */
#define NA_SUB(a) sub_##a
void NA_SUB(00572120)(CPU *c);          /* FindScannerConfig: override, tools/gen_v2.sh OVR_NA */
void NA_SUB(00572120_orig)(CPU *c);
void NA_SUB(00407750)(CPU *c);          /* RealDriver steer handler (lifted) */

void     dispatch_patch(uint32_t, void (*)(CPU *));
uint32_t call_guest(CPU *c, uint32_t fn, int nargs, const uint32_t *args);
uint32_t gheap_alloc(uint32_t n, int zero);
int      fileio_game_path(const char *win, char *out, size_t outsz);
void     metal_fill_keyboard(unsigned char *dst, unsigned n);
int      gamepad_xinput_state(uint16_t *buttons, uint8_t trig[2], int16_t sticks[4]);

#define NA_STEER_HANDLER  0x00407750u
#define NA_FESENDKEY      0x0055DBD0u   /* FESendKeystroke(key), cdecl */
#define NA_JOY2           0x00865768u   /* port 2's Joystick object */
#define NA_DEVICE_COUNT   0x00870764u
#define NA_JOYTYPE_P1     0x00864788u
#define NA_JOYTYPE_P2     0x00864789u
#define REALDRIVER_STEER  0x2C8u
#define XT_SCANNER_THUNK  0xDE5C0000u   /* not guest code: only the patch table knows it */

/* XINPUT_GAMEPAD button bits and the plugin's codes for analog controls */
enum { XB_DPAD_UP = 0x0001, XB_DPAD_DOWN = 0x0002, XB_DPAD_LEFT = 0x0004, XB_DPAD_RIGHT = 0x0008,
       XB_START = 0x0010, XB_BACK = 0x0020, XB_LEFT_THUMB = 0x0040, XB_RIGHT_THUMB = 0x0080,
       XB_LEFT_SHOULDER = 0x0100, XB_RIGHT_SHOULDER = 0x0200,
       XB_A = 0x1000, XB_B = 0x2000, XB_X = 0x4000, XB_Y = 0x8000 };
enum { CD_LT = 0xABC0, CD_RT, CD_LS_X, CD_LS_Y, CD_RS_X, CD_RS_Y,
       CD_LS_UP = 0xABC7, CD_LS_DOWN, CD_LS_LEFT, CD_LS_RIGHT,
       CD_RS_UP, CD_RS_DOWN, CD_RS_LEFT, CD_RS_RIGHT, CD_DPAD = 0xDAD0 };

/* scanner kinds (the plugin's Scanner_* functions) */
enum { SC_NONE, SC_UPORDOWN, SC_DOWN, SC_DIGITALANALOG, SC_DIGITALDOWNANALOG,
       SC_ANALOG, SC_DRAGSTEER, SC_STEER, SC_TYPECHANGED };

typedef struct { uint16_t buttons; uint8_t lt, rt; int16_t lx, ly, rx, ry; int connected; } XState;

static int      g_on = -1;                       /* -1 not decided yet */
static char     g_ini_path[1024];
static XState   g_x[2];                          /* port 1: never connected */
static unsigned char g_kb[256];                  /* DirectInput key state */
static struct { uint8_t kind, dik; uint16_t mask; } g_cfg[MAX_JOY_EVENT];
static uint16_t g_ev_state[2][MAX_JOY_EVENT];    /* EventStates */
static uint8_t  g_ev_state_kb[2][MAX_JOY_EVENT]; /* EventStatesKB */
static uint32_t g_cfg_guest;                     /* guest ScannerConfigs[MAX_JOY_EVENT], 0x20 each */
static uint16_t g_steer_left_btn, g_steer_right_btn, g_steer_left_axis, g_steer_right_axis;
static uint8_t  g_steer_left_dik, g_steer_right_dik;
static uint16_t g_dz_ls, g_dz_rs, g_thr_shift, g_thr_stick, g_thr_trigger;
static uint16_t g_back_old;
static uint64_t g_last_poll_us, g_polls, g_scans, g_events, g_steers, g_finds;
static uint64_t g_poll_us_total, g_poll_us_max;          /* time spent reading the pad and keys */
static int      g_log;

/* ---- ini (GetPrivateProfileString semantics: first match, value trimmed) ---- */
static char *g_ini;

static int ini_get(const char *sect, const char *key, char *out, size_t n){
    out[0] = 0;
    if (!g_ini) return 0;
    int in_sect = 0;
    for (const char *p = g_ini; *p; ){
        const char *e = strchr(p, '\n'); size_t len = e ? (size_t)(e - p) : strlen(p);
        char line[512]; if (len >= sizeof line) len = sizeof line - 1;
        memcpy(line, p, len); line[len] = 0;
        p = e ? e + 1 : p + len;
        char *s = line; while (isspace((unsigned char)*s)) s++;
        char *t = s + strlen(s); while (t > s && isspace((unsigned char)t[-1])) *--t = 0;
        if (*s == '['){
            char *r = strchr(s, ']'); if (r) *r = 0;
            in_sect = !strcasecmp(s + 1, sect); continue; }
        if (!in_sect || *s == ';') continue;
        char *eq = strchr(s, '='); if (!eq) continue;
        char *k = eq; while (k > s && isspace((unsigned char)k[-1])) k--;
        if ((size_t)(k - s) != strlen(key) || strncasecmp(s, key, (size_t)(k - s))) continue;
        char *v = eq + 1; while (isspace((unsigned char)*v)) v++;
        snprintf(out, n, "%s", v);
        return 1;
    }
    return 0;
}
static float ini_float(const char *sect, const char *key, float def){
    char v[128]; return ini_get(sect, key, v, sizeof v) ? (float)atof(v) : def; }

/* ---- names -> codes (NFSU_XtendedInput_XInputConfig.h / _VKHash.h) ---- */
static uint16_t xinput_bitmask(const char *n){
    static const struct { const char *n; uint16_t b; } t[] = {
        {"XINPUT_GAMEPAD_DPAD_UP", XB_DPAD_UP}, {"XINPUT_GAMEPAD_DPAD_DOWN", XB_DPAD_DOWN},
        {"XINPUT_GAMEPAD_DPAD_LEFT", XB_DPAD_LEFT}, {"XINPUT_GAMEPAD_DPAD_RIGHT", XB_DPAD_RIGHT},
        {"XINPUT_GAMEPAD_START", XB_START}, {"XINPUT_GAMEPAD_BACK", XB_BACK},
        {"XINPUT_GAMEPAD_LEFT_THUMB", XB_LEFT_THUMB}, {"XINPUT_GAMEPAD_RIGHT_THUMB", XB_RIGHT_THUMB},
        {"XINPUT_GAMEPAD_LB", XB_LEFT_SHOULDER}, {"XINPUT_GAMEPAD_LEFT_SHOULDER", XB_LEFT_SHOULDER},
        {"XINPUT_GAMEPAD_RB", XB_RIGHT_SHOULDER}, {"XINPUT_GAMEPAD_RIGHT_SHOULDER", XB_RIGHT_SHOULDER},
        {"XINPUT_GAMEPAD_A", XB_A}, {"XINPUT_GAMEPAD_B", XB_B},
        {"XINPUT_GAMEPAD_X", XB_X}, {"XINPUT_GAMEPAD_Y", XB_Y} };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) if (!strcmp(n, t[i].n)) return t[i].b;
    return 0;
}
static uint16_t xinput_configdef(const char *n){
    static const struct { const char *n; uint16_t d; } t[] = {
        {"XINPUT_GAMEPAD_LT", CD_LT}, {"XINPUT_GAMEPAD_RT", CD_RT},
        {"XINPUT_GAMEPAD_LS_X", CD_LS_X}, {"XINPUT_GAMEPAD_LS_Y", CD_LS_Y},
        {"XINPUT_GAMEPAD_RS_X", CD_RS_X}, {"XINPUT_GAMEPAD_RS_Y", CD_RS_Y},
        {"XINPUT_GAMEPAD_DPAD", CD_DPAD},
        {"XINPUT_GAMEPAD_LS_UP", CD_LS_UP}, {"XINPUT_GAMEPAD_LS_DOWN", CD_LS_DOWN},
        {"XINPUT_GAMEPAD_LS_LEFT", CD_LS_LEFT}, {"XINPUT_GAMEPAD_LS_RIGHT", CD_LS_RIGHT},
        {"XINPUT_GAMEPAD_RS_UP", CD_RS_UP}, {"XINPUT_GAMEPAD_RS_DOWN", CD_RS_DOWN},
        {"XINPUT_GAMEPAD_RS_LEFT", CD_RS_LEFT}, {"XINPUT_GAMEPAD_RS_RIGHT", CD_RS_RIGHT} };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) if (!strcmp(n, t[i].n)) return t[i].d;
    return 0;
}

/* Windows virtual-key name (or a single letter/digit) -> DirectInput key code, the
 * index of the runtime's keyboard state.  Left/right modifier variants both map to
 * the one key the window reports. */
static uint8_t vk_name_to_dik(const char *n){
    static const struct { const char *n; uint8_t d; } t[] = {
        {"VK_BACK",0x0E},{"VK_TAB",0x0F},{"VK_RETURN",0x1C},{"VK_ESCAPE",0x01},{"VK_SPACE",0x39},
        {"VK_SHIFT",0x2A},{"VK_LSHIFT",0x2A},{"VK_RSHIFT",0x36},
        {"VK_CONTROL",0x1D},{"VK_LCONTROL",0x1D},{"VK_RCONTROL",0x9D},
        {"VK_MENU",0x38},{"VK_LMENU",0x38},{"VK_RMENU",0xB8},
        {"VK_CAPITAL",0x3A},{"VK_PAUSE",0xC5},
        {"VK_PRIOR",0xC9},{"VK_NEXT",0xD1},{"VK_END",0xCF},{"VK_HOME",0xC7},
        {"VK_LEFT",0xCB},{"VK_UP",0xC8},{"VK_RIGHT",0xCD},{"VK_DOWN",0xD0},
        {"VK_INSERT",0xD2},{"VK_DELETE",0xD3},
        {"VK_NUMPAD0",0x52},{"VK_NUMPAD1",0x4F},{"VK_NUMPAD2",0x50},{"VK_NUMPAD3",0x51},
        {"VK_NUMPAD4",0x4B},{"VK_NUMPAD5",0x4C},{"VK_NUMPAD6",0x4D},{"VK_NUMPAD7",0x47},
        {"VK_NUMPAD8",0x48},{"VK_NUMPAD9",0x49},{"VK_MULTIPLY",0x37},{"VK_ADD",0x4E},
        {"VK_SUBTRACT",0x4A},{"VK_DECIMAL",0x53},{"VK_DIVIDE",0xB5},
        {"VK_F1",0x3B},{"VK_F2",0x3C},{"VK_F3",0x3D},{"VK_F4",0x3E},{"VK_F5",0x3F},{"VK_F6",0x40},
        {"VK_F7",0x41},{"VK_F8",0x42},{"VK_F9",0x43},{"VK_F10",0x44},{"VK_F11",0x57},{"VK_F12",0x58},
        {"VK_OEM_1",0x27},{"VK_OEM_PLUS",0x0D},{"VK_OEM_COMMA",0x33},{"VK_OEM_MINUS",0x0C},
        {"VK_OEM_PERIOD",0x34},{"VK_OEM_2",0x35},{"VK_OEM_3",0x29},{"VK_OEM_4",0x1A},
        {"VK_OEM_5",0x2B},{"VK_OEM_6",0x1B},{"VK_OEM_7",0x28} };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) if (!strcmp(n, t[i].n)) return t[i].d;
    if (n[0] && !n[1]){                                   /* the plugin's single-char fallback */
        static const char letters[] = "QWERTYUIOPASDFGHJKLZXCVBNM";
        static const uint8_t ldik[] = {0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,
            0x1E,0x1F,0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x2C,0x2D,0x2E,0x2F,0x30,0x31,0x32};
        int ch = toupper((unsigned char)n[0]);
        const char *q = ch ? strchr(letters, ch) : 0;
        if (q) return ldik[q - letters];
        if (ch >= '1' && ch <= '9') return (uint8_t)(0x02 + (ch - '1'));
        if (ch == '0') return 0x0B;
    }
    return 0;
}

/* ---- SetupScannerConfig / InitConfig ---- */
static void setup_config(void){
    g_dz_ls       = (uint16_t)(ini_float("Deadzone", "DeadzonePercentLS", 0.24f) * 32767.f);
    g_dz_rs       = (uint16_t)(ini_float("Deadzone", "DeadzonePercentRS", 0.24f) * 32767.f);
    g_thr_shift   = (uint16_t)(ini_float("Deadzone", "DeadzonePercent_Shifting", 0.62f) * 32767.f);
    g_thr_stick   = (uint16_t)(ini_float("Deadzone", "DeadzonePercent_AnalogStickDigital", 0.50f) * 32767.f);
    g_thr_trigger = (uint16_t)(ini_float("Deadzone", "DeadzonePercent_AnalogTriggerDigital", 0.12f) * 255.f);
    char v[128];
    for (unsigned i = 0; i < MAX_JOY_EVENT; i++){
        ini_get("EventsKB", JoyEventNames[i], v, sizeof v);
        g_cfg[i].dik = vk_name_to_dik(v);
        ini_get("Events", JoyEventNames[i], v, sizeof v);
        uint16_t cd = xinput_configdef(v);
        if (!cd){
            g_cfg[i].kind = bIsEventDigitalDownOnly(i) ? SC_DOWN : SC_UPORDOWN;
            g_cfg[i].mask = xinput_bitmask(v);
        } else {
            g_cfg[i].mask = cd;
            g_cfg[i].kind = SC_NONE;
            if (bIsEventAnalog(i)) g_cfg[i].kind = SC_ANALOG;
            else if (cd != CD_DPAD)
                g_cfg[i].kind = bIsEventDigitalDownOnly(i) ? SC_DIGITALDOWNANALOG : SC_DIGITALANALOG;
            if (i == JOY_EVENT_DRAG_RACE_CHANGE_LANE_LEFT_ANALOG || i == JOY_EVENT_DRAG_RACE_CHANGE_LANE_RIGHT_ANALOG)
                g_cfg[i].kind = SC_DRAGSTEER;
        }
        if (i == JOY_EVENT_STEER || i == JOY_EVENT_STEER_ANALOG) g_cfg[i].kind = SC_STEER;
    }
    g_cfg[JOY_EVENT_TYPE_CHANGED].kind = SC_TYPECHANGED;
    g_cfg[JOY_EVENT_LIVE_TYPE_CHANGED].kind = SC_TYPECHANGED;

    ini_get("Events", "SteerLeftButton", v, sizeof v);  g_steer_left_btn  = xinput_bitmask(v);
    ini_get("Events", "SteerRightButton", v, sizeof v); g_steer_right_btn = xinput_bitmask(v);
    if (!ini_get("Events", "SteerLeftAxis", v, sizeof v))  snprintf(v, sizeof v, "XINPUT_GAMEPAD_LS_LEFT");
    g_steer_left_axis = xinput_configdef(v);
    if (!ini_get("Events", "SteerRightAxis", v, sizeof v)) snprintf(v, sizeof v, "XINPUT_GAMEPAD_LS_RIGHT");
    g_steer_right_axis = xinput_configdef(v);
    if (!ini_get("EventsKB", "KeyboardSteerLeft", v, sizeof v))  snprintf(v, sizeof v, "VK_LEFT");
    g_steer_left_dik = vk_name_to_dik(v);
    if (!ini_get("EventsKB", "KeyboardSteerRight", v, sizeof v)) snprintf(v, sizeof v, "VK_RIGHT");
    g_steer_right_dik = vk_name_to_dik(v);
}

/* ---- UpdateControllerState / ReadXInput_Extra, once per poll ---- */
static uint64_t now_us(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static void poll_input(CPU *c){
    uint64_t t = now_us();
    if (t - g_last_poll_us < 2000u) return;      /* one read per event-generation pass */
    g_last_poll_us = t;
    g_polls++;
    XState *x = &g_x[0];
    uint8_t trig[2]; int16_t s[4];
    x->connected = gamepad_xinput_state(&x->buttons, trig, s);
    x->lt = trig[0]; x->rt = trig[1];
    x->lx = s[0]; x->ly = s[1]; x->rx = s[2]; x->ry = s[3];
    if (x->connected){
        /* zero a stick only when both of its axes are inside the dead zone */
        if (x->lx < g_dz_ls && x->lx > -g_dz_ls && x->ly < g_dz_ls && x->ly > -g_dz_ls) x->lx = x->ly = 0;
        if (x->rx < g_dz_rs && x->rx > -g_dz_rs && x->ry < g_dz_rs && x->ry > -g_dz_rs) x->rx = x->ry = 0;
        if ((x->buttons & XB_BACK) != g_back_old){
            if (x->buttons & XB_BACK){                  /* once, on press */
                uint32_t a = 'Q';
                call_guest(c, NA_FESENDKEY, 1, &a);
            }
            g_back_old = x->buttons & XB_BACK;
        }
    }
    metal_fill_keyboard(g_kb, 256);
    { uint64_t d = now_us() - t; g_poll_us_total += d; if (d > g_poll_us_max) g_poll_us_max = d; }
    if (g_polls == 1)
        fprintf(stderr, "[xt] first poll: pad %s, device count %u, joystick types P1 %u P2 %u\n",
                x->connected ? "connected" : "not connected", MR32(NA_DEVICE_COUNT),
                MR8(NA_JOYTYPE_P1), MR8(NA_JOYTYPE_P2));
    /* The plugin marks player 1's port as present (keyboard or pad) and player 2's
     * as absent every frame; the game ignores the events of a port whose type is 0xFF (sub_00571a60).
     * The device count it also writes is left to the game: here its DirectInput
     * devices exist and it counts them itself. */
    MR8(NA_JOYTYPE_P1) = 1;
    MR8(NA_JOYTYPE_P2) = 0xFF;
}

/* ---- scanners ---- */
#define EV_PRESSED(ev) ((int)(ev) + (0xFF << 8))      /* param is always 0xFF */

static int ev_always_reports(unsigned ev){             /* "throttle/brake is very time sensitive" */
    return ev == JOY_EVENT_THROTTLE || ev == JOY_EVENT_BRAKE ||
           ev == JOY_EVENT_THROTTLE_ANALOG || ev == JOY_EVENT_THROTTLE_ANALOG_ALTERNATE ||
           ev == JOY_EVENT_BRAKE_ANALOG || ev == JOY_EVENT_BRAKE_ANALOG_ALTERNATE;
}

static int key_down(uint8_t dik){ return dik && (g_kb[dik] & 0x80); }

/* Scanner_DigitalUpOrDown_XInput */
static int pad_up_or_down(unsigned ev, int ci){
    uint16_t on = g_x[ci].buttons & g_cfg[ev].mask;
    if (ev_always_reports(ev) || on != g_ev_state[ci][ev]){
        g_ev_state[ci][ev] = on;
        return on ? EV_PRESSED(ev) : (int)ev;
    }
    return 0;
}
/* Scanner_DigitalUpOrDown_AsyncKB (the plugin's default KeyboardReadingMode 0) */
static int kb_up_or_down(unsigned ev){
    uint8_t on = (uint8_t)key_down(g_cfg[ev].dik);
    if (ev_always_reports(ev) || on != g_ev_state_kb[0][ev]){
        g_ev_state_kb[0][ev] = on;
        return on ? EV_PRESSED(ev) : (int)ev;
    }
    return 0;
}
/* Scanner_DigitalDown_XInput / _AsyncKB */
static int pad_down(unsigned ev, int ci){
    uint16_t on = g_x[ci].buttons & g_cfg[ev].mask;
    if (on != g_ev_state[ci][ev]){ g_ev_state[ci][ev] = on; if (on) return (int)ev; }
    return 0;
}
static int kb_down(unsigned ev){
    uint8_t on = (uint8_t)key_down(g_cfg[ev].dik);
    if (on != g_ev_state_kb[0][ev]){ g_ev_state_kb[0][ev] = on; if (on) return (int)ev; }
    return 0;
}

/* A trigger or stick direction used as a button; -1 if the binding isn't one. */
static int analog_as_button(unsigned ev, int ci){
    const XState *x = &g_x[ci];
    int thr = g_thr_stick;
    if (ev == JOY_EVENT_SHIFTUP || ev == JOY_EVENT_SHIFTDOWN ||
        ev == JOY_EVENT_SHIFTUP_ALTERNATE || ev == JOY_EVENT_SHIFTDOWN_ALTERNATE) thr = g_thr_shift;
    switch (g_cfg[ev].mask){
    case CD_LT:       return x->lt > g_thr_trigger;
    case CD_RT:       return x->rt > g_thr_trigger;
    case CD_LS_UP:    return x->ly >  thr;
    case CD_LS_DOWN:  return x->ly < -thr;
    case CD_LS_LEFT:  return x->lx < -thr;
    case CD_LS_RIGHT: return x->lx >  thr;
    case CD_RS_UP:    return x->ry >  thr;
    case CD_RS_DOWN:  return x->ry < -thr;
    case CD_RS_LEFT:  return x->rx < -thr;
    case CD_RS_RIGHT: return x->rx >  thr;
    default:          return -1;
    }
}
/* Scanner_DigitalAnalog_XInput */
static int pad_digital_analog(unsigned ev, int ci){
    int on = analog_as_button(ev, ci);
    if (on < 0 || on == g_ev_state[ci][ev]) return 0;
    g_ev_state[ci][ev] = (uint16_t)on;
    return on ? EV_PRESSED(ev) : (int)ev;
}
/* Scanner_DigitalDownAnalog_XInput.  The plugin never clears the state in its LT
 * case, so an LT-bound press-only event fired once per session; it is cleared here
 * like the other cases (the stock ini binds no press-only event to LT). */
static int pad_digital_down_analog(unsigned ev, int ci){
    int on = analog_as_button(ev, ci);
    if (on < 0 || on == g_ev_state[ci][ev]) return 0;
    g_ev_state[ci][ev] = (uint16_t)on;
    return on ? (int)ev : 0;
}

static uint8_t stick_byte(int16_t v){                  /* (char)(v/32767*127) + 0x80 */
    return (uint8_t)((int8_t)((float)v / 32767.f * 127.f) + 0x80);
}
/* Scanner_Analog */
static int pad_analog(unsigned ev, int ci){
    const XState *x = &g_x[ci];
    uint8_t axis = 0;
    switch (g_cfg[ev].mask){
    case CD_LT:   axis = x->lt; break;
    case CD_RT:   axis = x->rt; break;
    case CD_LS_X: axis = stick_byte(x->lx); break;
    case CD_RS_X: axis = stick_byte(x->rx); break;
    case CD_LS_Y: axis = stick_byte(x->ly); break;
    case CD_RS_Y: axis = stick_byte(x->ry); break;
    default: break;
    }
    if (ev == JOY_EVENT_DEBUG_CAMERA_INOUT || ev == JOY_EVENT_CARSEL_ORBIT_UPDOWN ||
        ev == JOY_EVENT_HYDRAULIC_PRESSURIZE_ANALOG_UD || ev == JOY_EVENT_HYDRAULIC_BOUNCE_ANALOG_UD)
        axis = (uint8_t)-axis;                         /* "invert some broken axis" */
    if (axis != g_ev_state[ci][ev] ||
        ev == JOY_EVENT_THROTTLE_ANALOG || ev == JOY_EVENT_THROTTLE_ANALOG_ALTERNATE ||
        ev == JOY_EVENT_BRAKE_ANALOG || ev == JOY_EVENT_BRAKE_ANALOG_ALTERNATE){
        g_ev_state[ci][ev] = axis;
        return (int)ev + (axis << 8);
    }
    return 0;
}
/* Scanner_Analog_DragSteer (only reachable if the ini binds the *_ANALOG lane events) */
static int pad_drag_steer(unsigned ev, int ci){
    const XState *x = &g_x[ci];
    int16_t v;
    switch (g_cfg[ev].mask){
    case CD_LS_X: v = x->lx; break;  case CD_RS_X: v = x->rx; break;
    case CD_LS_Y: v = x->ly; break;  case CD_RS_Y: v = x->ry; break;
    default: return 0;
    }
    uint8_t axis = (uint8_t)(int)((float)v / 32767.f * 127.f);
    int8_t signed_axis = (int8_t)axis;
    if (axis != g_ev_state[ci][ev]){
        g_ev_state[ci][ev] = axis;
        if (ev == JOY_EVENT_DRAG_RACE_CHANGE_LANE_LEFT_ANALOG && signed_axis < 0) return (int)ev + (axis << 8);
        if (ev == JOY_EVENT_DRAG_RACE_CHANGE_LANE_RIGHT_ANALOG && signed_axis > 0) return (int)ev + (axis << 8);
    }
    return 0;
}

/* Native entry of every ScannerConfig: cdecl int (EventNode, unk1, unk2, ScannerConfig*, Joystick*) */
static void xt_scanner(CPU *c){
    uint32_t cfg = MR32(c->esp + 16u), joy = MR32(c->esp + 20u);
    unsigned ev = (cfg - g_cfg_guest) / 0x20u;
    int ci = (joy == NA_JOY2) ? 1 : 0, r = 0;
    poll_input(c);
    g_scans++;
    if (cfg >= g_cfg_guest && ev < MAX_JOY_EVENT){
        switch (g_cfg[ev].kind){
        case SC_UPORDOWN:          r = pad_up_or_down(ev, ci) | kb_up_or_down(ev); break;
        case SC_DOWN:              r = pad_down(ev, ci) | kb_down(ev); break;
        case SC_DIGITALANALOG:     r = pad_digital_analog(ev, ci) | kb_up_or_down(ev); break;
        case SC_DIGITALDOWNANALOG: r = pad_digital_down_analog(ev, ci) | kb_down(ev); break;
        case SC_ANALOG:            r = pad_analog(ev, ci); break;
        case SC_DRAGSTEER:         r = pad_drag_steer(ev, ci); break;
        case SC_STEER:             r = (int)ev + (1 << 8); break;   /* poll the steer handler every time */
        default:                   r = 0; break;
        }
    }
    if (r) g_events++;
    if (g_log && r && ev != JOY_EVENT_STEER && ev != JOY_EVENT_STEER_ANALOG && !ev_always_reports(ev))
        fprintf(stderr, "[xt] port %d %s value %u\n", ci, JoyEventNames[ev], (unsigned)((r >> 8) & 0xFF));
    c->eax = (uint32_t)r;
    POP32(c->eip);                                    /* cdecl */
}

/* GetAnalogStickValue for the steering binds */
static float stick_value(int ci, uint16_t bind){
    const XState *x = &g_x[ci];
    switch (bind){
    case CD_LT:       return x->lt / 255.f;
    case CD_RT:       return x->rt / 255.f;
    case CD_LS_X:     return x->lx / 32767.f;
    case CD_RS_X:     return x->rx / 32767.f;
    case CD_LS_Y:     return x->ly / 32767.f;
    case CD_RS_Y:     return x->ry / 32767.f;
    case CD_LS_UP:    return x->ly > 0 ?  x->ly / 32767.f : 0.f;
    case CD_LS_DOWN:  return x->ly < 0 ? -x->ly / 32767.f : 0.f;
    case CD_LS_LEFT:  return x->lx < 0 ? -x->lx / 32767.f : 0.f;
    case CD_LS_RIGHT: return x->lx > 0 ?  x->lx / 32767.f : 0.f;
    case CD_RS_UP:    return x->ry > 0 ?  x->ry / 32767.f : 0.f;
    case CD_RS_DOWN:  return x->ry < 0 ? -x->ry / 32767.f : 0.f;
    case CD_RS_LEFT:  return x->rx < 0 ? -x->rx / 32767.f : 0.f;
    case CD_RS_RIGHT: return x->rx > 0 ?  x->rx / 32767.f : 0.f;
    default:          return 0.f;
    }
}

/* RealDriver_JoyHandler_Steer (STEER) and DummyFunc (STEER_ANALOG):
 * cdecl void (JoystickPort, JoyEvent, Value, RealDriver*) */
static void xt_steer(CPU *c){
    uint32_t port = MR32(c->esp + 4u), ev = MR32(c->esp + 8u), rd = MR32(c->esp + 16u);
    if (ev != JOY_EVENT_STEER && ev != JOY_EVENT_STEER_ANALOG){ NA_SUB(00407750)(c); return; }
    if (ev == JOY_EVENT_STEER && rd){
        int ci = port == 1 ? 1 : 0;
        poll_input(c);
        g_steers++;
        float v = stick_value(ci, g_steer_left_axis) - stick_value(ci, g_steer_right_axis);
        if (g_x[ci].buttons & g_steer_left_btn)  v += 1.f;
        if (g_x[ci].buttons & g_steer_right_btn) v -= 1.f;
        if (key_down(g_steer_left_dik))  v += 1.f;
        if (key_down(g_steer_right_dik)) v -= 1.f;
        if (v > 1.f) v = 1.f;
        if (v < -1.f) v = -1.f;
        MRF32(rd + REALDRIVER_STEER) = v;              /* positive = left */
    }
    POP32(c->eip);
}

/* FindScannerConfig(JoyEvent, config type, ...) -> ScannerConfig*, cdecl.  The
 * plugin's FindScannerConfig_Custom: one config per event whatever the device type. */
void NA_SUB(00572120)(CPU *c){
    if (!xtinput_active()){ NA_SUB(00572120_orig)(c); return; }
    uint32_t ev = MR32(c->esp + 4u);
    g_finds++;
    if (!g_cfg_guest){
        g_cfg_guest = gheap_alloc(MAX_JOY_EVENT * 0x20u, 1);
        for (unsigned i = 0; g_cfg_guest && i < MAX_JOY_EVENT; i++){
            uint32_t p = g_cfg_guest + i * 0x20u;
            MR32(p + 0x00) = 0xFFu;                    /* JoyConfigType */
            MR32(p + 0x08) = i;                        /* JoyEvent */
            MR32(p + 0x0C) = g_cfg[i].kind == SC_NONE ? 0u : XT_SCANNER_THUNK;
            MR16(p + 0x10) = g_cfg[i].mask;            /* BitmaskStuff */
            MR32(p + 0x18) = 0xFFu;                    /* param */
        }
    }
    c->eax = (g_cfg_guest && ev < MAX_JOY_EVENT) ? g_cfg_guest + ev * 0x20u : 0u;
    POP32(c->eip);
}

int xtinput_active(void){ return g_on > 0; }

void xtinput_install(void){
    const char *e = getenv("NFSU2_XTINPUT");
    g_on = 0;
    if (!e || !atoi(e)) return;                       /* off unless asked for (the launcher's switch) */
    g_log = (e = getenv("NFSU2_XTINPUT_LOG")) && strcmp(e, "0");
    if ((e = getenv("NFSU2_XTINPUT_INI"))) snprintf(g_ini_path, sizeof g_ini_path, "%s", e);
    else if (!fileio_game_path("scripts\\NFSU_XtendedInput.ini", g_ini_path, sizeof g_ini_path))
        g_ini_path[0] = 0;
    if (g_ini_path[0]){
        FILE *f = fopen(g_ini_path, "rb");
        long n = -1;
        if (f && !fseek(f, 0, SEEK_END) && (n = ftell(f)) >= 0 && !fseek(f, 0, SEEK_SET)){
            g_ini = calloc(1, (size_t)n + 1);
            if (g_ini && fread(g_ini, 1, (size_t)n, f) != (size_t)n){ free(g_ini); g_ini = 0; }
        }
        if (f) fclose(f);
        if (!g_ini){ fprintf(stderr, "[xt] cannot read %s, using the plugin's stock settings\n", g_ini_path);
                     g_ini_path[0] = 0; }
    }
    if (!g_ini) g_ini = strdup(xtinput_default_ini);
    if (!g_ini) return;
    for (char *p = g_ini; *p; p++) if (*p == '\r') *p = ' ';
    setup_config();
    dispatch_patch(XT_SCANNER_THUNK, xt_scanner);
    dispatch_patch(NA_STEER_HANDLER, xt_steer);
    g_on = 1;
    unsigned bound = 0, keys = 0;
    for (unsigned i = 0; i < MAX_JOY_EVENT; i++){ bound += g_cfg[i].mask != 0; keys += g_cfg[i].dik != 0; }
    fprintf(stderr, "[xt] XtendedInput controls from %s: %u pad and %u keyboard bindings, "
            "dead zone LS %.2f\n", g_ini_path[0] ? g_ini_path : "the plugin's stock settings",
            bound, keys, g_dz_ls / 32767.f);
}

void xtinput_report(void){
    if (!xtinput_active()) return;
    fprintf(stderr, "  xtinput          : %llu config lookups, %llu polls, %llu scans, %llu events, %llu steer updates\n",
            (unsigned long long)g_finds, (unsigned long long)g_polls, (unsigned long long)g_scans,
            (unsigned long long)g_events, (unsigned long long)g_steers);
    fprintf(stderr, "  xtinput reads    : %.1f us average, %llu us longest\n",
            g_polls ? (double)g_poll_us_total / (double)g_polls : 0.0, (unsigned long long)g_poll_us_max);
}

#else   /* other executables: the plugin's addresses are North American v1.2 ones */
int  xtinput_active(void){ return 0; }
void xtinput_install(void){}
void xtinput_report(void){}
#endif
