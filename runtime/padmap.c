/* Controller mappings for IOKit HID game pads; see padmap.h. */
#include "padmap.h"
#include <stdio.h>
#include <string.h>
#include "padmap_db.h"            /* k_padmap_db[], tools/packaging/padmap_db.py */

static int hexv(int c){
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* An SDL GUID of an IOKit device: bus, CRC, vendor, product, version, driver byte 0
 * (16 bytes, little-endian fields).  Other drivers number the inputs differently. */
static int parse_guid(const char *s, size_t len, PadId *id){
    uint8_t b[16];
    if (len != 32) return 0;
    for (int i = 0; i < 16; i++){
        int h = hexv(s[2*i]), l = hexv(s[2*i + 1]);
        if (h < 0 || l < 0) return 0;
        b[i] = (uint8_t)(h << 4 | l);
    }
    if (b[14]) return 0;
    id->bus = (uint16_t)(b[0] | b[1] << 8);   id->vid = (uint16_t)(b[4] | b[5] << 8);
    id->pid = (uint16_t)(b[8] | b[9] << 8);   id->ver = (uint16_t)(b[12] | b[13] << 8);
    return 1;
}

static const char *find_in(const char *s, size_t len, const char *what){
    size_t n = strlen(what);
    for (size_t i = 0; i + n <= len; i++) if (!memcmp(s + i, what, n)) return s + i;
    return NULL;
}

typedef struct { int score; const char *map, *name; size_t maplen, namelen; } Pick;

/* One line of a mapping list.  `any` collects a line for every device (user text). */
static void consider(const char *s, size_t len, PadId id, Pick *any, Pick *best){
    while (len && (*s == ' ' || *s == '\t')){ s++; len--; }
    while (len && (s[len-1] == '\r' || s[len-1] == ' ' || s[len-1] == '\t')) len--;
    if (!len || *s == '#') return;
    { const char *p = find_in(s, len, "platform:");
      if (p && (size_t)(s + len - p) >= 9 && !find_in(p, (size_t)(s + len - p), "platform:Mac OS X")) return; }
    const char *c1 = memchr(s, ',', len);
    if (!c1) return;
    size_t f1 = (size_t)(c1 - s);
    if (memchr(s, ':', f1)){                        /* no GUID and no name: the mapping alone */
        if (any && !any->map){ any->map = s; any->maplen = len; any->name = ""; any->namelen = 0; }
        return;
    }
    const char *c2 = memchr(c1 + 1, ',', len - f1 - 1);
    if (!c2) return;
    const char *map = c2 + 1;
    size_t maplen = len - (size_t)(map - s);
    if (f1 == 1 && *s == '*'){
        if (any && !any->map){ any->map = map; any->maplen = maplen; any->name = c1 + 1;
                               any->namelen = (size_t)(c2 - c1 - 1); }
        return;
    }
    PadId e;
    if (!parse_guid(s, f1, &e) || e.vid != id.vid || e.pid != id.pid) return;
    int score = 1 + (e.ver == id.ver ? 2 : 0) + (e.bus == id.bus ? 1 : 0);
    if (score > best->score){
        best->score = score; best->map = map; best->maplen = maplen;
        best->name = c1 + 1; best->namelen = (size_t)(c2 - c1 - 1);
    }
}

static void copy_n(char *out, size_t outsz, const char *s, size_t n){
    if (!outsz) return;
    if (n >= outsz) n = outsz - 1;
    memcpy(out, s, n); out[n] = 0;
}

int padmap_find(const char *user, PadId id, char *out, size_t outsz, char *name, size_t namesz){
    Pick any = {0}, ub = {0}, bb = {0}, *w = NULL;
    int src = PM_SRC_NONE;
    if (user)
        for (const char *s = user; *s; ){
            const char *e = strchr(s, '\n');
            size_t n = e ? (size_t)(e - s) : strlen(s);
            consider(s, n, id, &any, &ub);
            s += n + (e ? 1 : 0);
        }
    if (ub.map){ w = &ub; src = PM_SRC_USER; }
    else if (any.map){ w = &any; src = PM_SRC_USER_ANY; }
    else {
        for (size_t i = 0; i < sizeof k_padmap_db / sizeof k_padmap_db[0]; i++)
            consider(k_padmap_db[i], strlen(k_padmap_db[i]), id, NULL, &bb);
        if (bb.map){ w = &bb; src = PM_SRC_BUILTIN; }
    }
    if (out && outsz) out[0] = 0;
    if (name && namesz) name[0] = 0;
    if (w){ copy_n(out, outsz, w->map, w->maplen); copy_n(name, namesz, w->name, w->namelen); }
    return src;
}

/* --- parsing ------------------------------------------------------------- */

static const char *const k_out[PM_NBTN + PM_NAX] = {
    "a", "b", "x", "y", "leftshoulder", "rightshoulder", "back", "start", "leftstick",
    "rightstick", "dpup", "dpdown", "dpleft", "dpright",
    "leftx", "lefty", "rightx", "righty", "lefttrigger", "righttrigger" };

static int out_code(const char *k, size_t n, int8_t *half){
    *half = 0;
    if (n && (*k == '+' || *k == '-')){ *half = *k == '+' ? 1 : -1; k++; n--; }
    for (int i = 0; i < PM_NBTN + PM_NAX; i++)
        if (strlen(k_out[i]) == n && !memcmp(k_out[i], k, n)) return i;
    return -1;                                       /* guide, misc1, paddle1, ...: not used */
}

static int in_parse(const char *v, size_t n, PadBind *b){
    size_t i = 1;
    long num = 0, mask = 0;
    if (n && (*v == '+' || *v == '-')){ b->ihalf = *v == '+' ? 1 : -1; v++; n--; }
    if (n < 2 || (v[0] != 'a' && v[0] != 'b' && v[0] != 'h')) return 0;
    if (v[1] < '0' || v[1] > '9') return 0;
    while (i < n && v[i] >= '0' && v[i] <= '9' && num < 1000) num = num * 10 + (v[i++] - '0');
    if (num > 255) return 0;
    b->kind = v[0]; b->idx = (uint8_t)num;
    if (b->kind == 'h'){
        if (i >= n || v[i] != '.') return 0;
        i++;
        while (i < n && v[i] >= '0' && v[i] <= '9' && mask < 1000) mask = mask * 10 + (v[i++] - '0');
        if (mask <= 0 || mask > 15) return 0;
        b->mask = (uint8_t)mask;
    }
    if (b->kind == 'a' && i < n && v[i] == '~'){ b->inv = 1; i++; }
    if (b->kind != 'a' && b->ihalf) return 0;
    return i == n;
}

int padmap_parse(const char *s, PadMap *m){
    m->n = 0;
    while (s && *s && *s != '\n' && *s != '\r'){
        const char *e = s;
        while (*e && *e != ',' && *e != '\n' && *e != '\r') e++;
        const char *colon = memchr(s, ':', (size_t)(e - s));
        if (colon && m->n < (int)(sizeof m->b / sizeof m->b[0])){
            PadBind b;
            memset(&b, 0, sizeof b);
            int o = out_code(s, (size_t)(colon - s), &b.ohalf);
            if (o >= 0 && in_parse(colon + 1, (size_t)(e - colon - 1), &b)){
                b.out = (int8_t)o;
                m->b[m->n++] = b;
            }
        }
        s = *e == ',' ? e + 1 : e;
    }
    return m->n;
}

/* --- evaluation (SDL's gamepad mapping rules) ------------------------------ */

void padmap_eval(const PadMap *m, const PadRaw *r, PadState *s){
    float acc[PM_NAX] = {0};
    uint16_t btn = 0;
    for (int i = 0; i < m->n; i++){
        const PadBind *b = &m->b[i];
        int digital = 1, pressed = 0;
        float t = 0.f;                          /* position in the input's range, 0..1 */
        if (b->kind == 'b'){
            if (b->idx >= r->nbtn) continue;
            pressed = r->btn[b->idx] != 0;
        } else if (b->kind == 'h'){
            if (b->idx >= r->nhat) continue;
            pressed = (r->hat[b->idx] & b->mask) != 0;
        } else {
            if (b->idx >= r->nax || !r->axok[b->idx]) continue;
            float v = r->ax[b->idx], lo = b->ihalf ? 0.f : -1.f, hi = b->ihalf < 0 ? -1.f : 1.f;
            if (b->inv){ float x = lo; lo = hi; hi = x; }
            if (v < (lo < hi ? lo : hi) || v > (lo < hi ? hi : lo)) continue;   /* the other half */
            t = (v - lo) / (hi - lo);
            digital = 0;
        }
        if (b->out < PM_NBTN){
            if (digital ? pressed : t >= 0.5f) btn |= (uint16_t)(1u << b->out);
            continue;
        }
        int o = b->out - PM_NBTN;
        float olo = -1.f, ohi = 1.f;
        if (o == PM_LT || o == PM_RT){ olo = 0.f; ohi = 1.f; }
        else if (b->ohalf){ olo = 0.f; ohi = (float)b->ohalf; }
        if (digital){ if (pressed) acc[o] += ohi; }
        else acc[o] += olo + t * (ohi - olo);
    }
    for (int o = 0; o < PM_NAX; o++){
        float lo = (o == PM_LT || o == PM_RT) ? 0.f : -1.f;
        if (acc[o] < lo) acc[o] = lo;
        if (acc[o] > 1.f) acc[o] = 1.f;
    }
    s->ax[PM_LX] = acc[PM_LX];  s->ax[PM_LY] = -acc[PM_LY];    /* SDL: down = positive */
    s->ax[PM_RX] = acc[PM_RX];  s->ax[PM_RY] = -acc[PM_RY];
    s->ax[PM_LT] = acc[PM_LT];  s->ax[PM_RT] = acc[PM_RT];
    s->btn = btn;
}

/* --- a mapping for an unknown device --------------------------------------- */

static void add(char *out, size_t sz, const char *fmt, int a, int b){
    size_t n = strlen(out);
    if (n < sz) snprintf(out + n, sz - n, fmt, a, b);
}

/* Sticks on X/Y and Z/Rz (or Rx/Ry), analog triggers on Brake/Accelerator (or the
 * remaining pair), the hat as the d-pad, and the button order of the device family:
 * 8BitDo's D-input order, else the usual DirectInput one (1 X, 2 A, 3 B, 4 Y, 5 LB,
 * 6 RB, 7 LT, 8 RT, 9 Back, 10 Start, 11 LS, 12 RS). */
const char *padmap_guess(const uint32_t *u, int nax, int nbtn, int nhat, uint16_t vid,
                         char *out, size_t sz){
    int ix = -1, iy = -1, iz = -1, irx = -1, iry = -1, irz = -1, iacc = -1, ibrk = -1;
    for (int i = nax - 1; i >= 0; i--)
        switch (u[i]){
        case 0x30: ix = i; break;   case 0x31: iy = i; break;   case 0x32: iz = i; break;
        case 0x33: irx = i; break;  case 0x34: iry = i; break;  case 0x35: irz = i; break;
        case 0xC4: iacc = i; break; case 0xC5: ibrk = i; break; default: break;
        }
    int sx = -1, sy = -1, lt = -1, rt = -1;
    if (iz >= 0 && irz >= 0){ sx = iz; sy = irz; if (irx >= 0 && iry >= 0){ lt = irx; rt = iry; } }
    else if (irx >= 0 && iry >= 0){ sx = irx; sy = iry; if (iz >= 0 && irz >= 0){ lt = iz; rt = irz; } }
    if (ibrk >= 0 && iacc >= 0){ lt = ibrk; rt = iacc; }

    static const int k8bitdo[12] = { 0, 1, 3, 4, 6, 7, 10, 11, 13, 14, 8, 9 };
    static const int kdinput[12] = { 1, 2, 0, 3, 4, 5, 8, 9, 10, 11, 6, 7 };
    const int *bt = vid == 0x2dc8 ? k8bitdo : kdinput;     /* [PM_A..PM_RS, LT, RT] */
    if (sz) out[0] = 0;
    for (int i = 0; i < 10; i++)
        if (bt[i] < nbtn){
            size_t n = strlen(out);
            if (n < sz) snprintf(out + n, sz - n, "%s:b%d,", k_out[i], bt[i]);
        }
    if (ix >= 0) add(out, sz, "leftx:a%d,", ix, 0);
    if (iy >= 0) add(out, sz, "lefty:a%d,", iy, 0);
    if (sx >= 0) add(out, sz, "rightx:a%d,righty:a%d,", sx, sy);
    if (lt >= 0) add(out, sz, "lefttrigger:a%d,righttrigger:a%d,", lt, rt);
    else if (bt[11] < nbtn) add(out, sz, "lefttrigger:b%d,righttrigger:b%d,", bt[10], bt[11]);
    if (nhat > 0) add(out, sz, "dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8,", 0, 0);
    return out;
}

/* --- HID values ---------------------------------------------------------- */

float padmap_axis(long v, long *min, long *max){
    if (v < *min) *min = v;
    if (v > *max) *max = v;
    if (*max <= *min) return 0.f;
    return (float)(v - *min) * 2.f / (float)(*max - *min) - 1.f;
}

uint8_t padmap_hat(long v, long min, long max){
    static const uint8_t dir[8] = { 1, 1|2, 2, 2|4, 4, 4|8, 8, 8|1 };
    long range = max - min + 1, p = v - min;
    if (range == 4) p *= 2;
    else if (range != 8) return 0;
    return (p >= 0 && p < 8) ? dir[p] : 0;
}
