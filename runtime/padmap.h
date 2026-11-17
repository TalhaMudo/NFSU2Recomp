/* Controller mappings for game pads read through IOKit HID (runtime/gamepad.m):
 * pads that Apple's GameController framework does not present, such as DirectInput-
 * mode pads (8BitDo D-input, Logitech "D" mode, generic USB pads).  Plain C, no
 * framework, so tests/padmap_test.c can check it.
 *
 * A device's inputs are numbered the way SDL's IOKit joystick driver numbers them
 * (axes, buttons and hats each sorted by HID usage), so SDL GameControllerDB lines
 * describe them: "GUID,name,a:b0,b:b1,...,leftx:a0,lefty:a1,dpup:h0.1,...".  The
 * result is the Xbox layout that the rest of the runtime works in. */
#ifndef PADMAP_H
#define PADMAP_H
#include <stddef.h>
#include <stdint.h>

enum { PM_A, PM_B, PM_X, PM_Y, PM_LB, PM_RB, PM_BACK, PM_START, PM_LS, PM_RS,
       PM_UP, PM_DOWN, PM_LEFT, PM_RIGHT, PM_NBTN };
enum { PM_LX, PM_LY, PM_RX, PM_RY, PM_LT, PM_RT, PM_NAX };

/* Xbox-layout pad state: sticks -1..1 with up = positive (GameController's sense),
 * triggers 0..1, buttons as 1 << PM_*. */
typedef struct { float ax[PM_NAX]; uint16_t btn; } PadState;

/* A device's current inputs, in SDL's numbering. */
typedef struct {
    int nax, nbtn, nhat;
    const float   *ax;      /* -1..1 (logical minimum .. maximum) */
    const uint8_t *axok;    /* 0: no value seen yet, the binding is left neutral */
    const uint8_t *btn;     /* 0 / 1 */
    const uint8_t *hat;     /* 1 up, 2 right, 4 down, 8 left (diagonals: two bits) */
} PadRaw;

typedef struct {
    int8_t  out;            /* 0..PM_NBTN-1 button, PM_NBTN + PM_* axis */
    int8_t  ohalf;          /* output half axis: +1 / -1, 0 whole */
    char    kind;           /* 'a' axis, 'b' button, 'h' hat */
    uint8_t idx, mask;      /* input number; hat direction bits */
    int8_t  ihalf;          /* input half axis: +1 / -1, 0 whole */
    uint8_t inv;            /* input axis inverted (~) */
} PadBind;
typedef struct { PadBind b[64]; int n; } PadMap;

typedef struct { uint16_t bus, vid, pid, ver; } PadId;   /* bus: 3 USB, 5 Bluetooth */

enum { PM_SRC_NONE, PM_SRC_BUILTIN, PM_SRC_USER_ANY, PM_SRC_USER };

/* The mapping for a device: from `user` (text of SDL lines, may be NULL) first, then
 * the built-in table (runtime/padmap_db.h).  A user line whose first field is "*", or
 * that has no GUID at all, applies to any device.  Copies the mapping fields to
 * out and the line's name to name; returns PM_SRC_*. */
int  padmap_find(const char *user, PadId id, char *out, size_t outsz, char *name, size_t namesz);

/* A mapping for a device no table knows, from its axis usages (SDL order) and its
 * button and hat counts.  Returns out. */
const char *padmap_guess(const uint32_t *axusage, int nax, int nbtn, int nhat, uint16_t vid,
                         char *out, size_t outsz);

int  padmap_parse(const char *mapping, PadMap *m);        /* number of bindings */
void padmap_eval(const PadMap *m, const PadRaw *r, PadState *s);

/* HID value conversions as SDL's IOKit driver does them.  padmap_axis widens
 * [*min, *max] when the device reports outside its logical range. */
float   padmap_axis(long v, long *min, long *max);
uint8_t padmap_hat(long v, long min, long max);
#endif
