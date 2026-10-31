/* D3D9 -> Metal bridge (stage 1: window + Clear/Present)
 *
 * On macOS the window must be on the MAIN thread. Since the guest already runs
 * on a separate thread this fits: the main thread drives the NSApplication loop,
 * the guest thread writes draw commands into a queue.                    */
#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/QuartzCore.h>

/* getenv() SCANS the environment array and takes a lock on every call.  On
 * per-draw paths (for example the "PDLIST" check in SetVertexShaderConstantF
 * was the FIRST term of an && chain) it has a measurable cost: in the profile
 * getenv+__findenv_locked ate 3% of the guest thread.
 * Environment variables don't change during a run -> resolve once, keep. */
#define ENVC(nm_) ({ static const char *_ev_; static int _ei_; \
                     if (!_ei_){ _ei_ = 1; _ev_ = getenv(nm_); } _ev_; })

#import <Metal/Metal.h>
#include "enhance.h"
#include "perflog.h"
#include <stdatomic.h>
#include <math.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <stdlib.h>

static NSWindow        *g_win;
#import <ImageIO/ImageIO.h>
#import <CoreServices/CoreServices.h>

static CAMetalLayer    *g_layer;
static long             g_skip_empty;   /* a frame with no content: don't present */
static id<MTLTexture>   g_capture;   /* readable copy for frame capture */

/* --- frame dump: DUMPFRAME=100,300 writes the selected frames as PNG --------
 * To see directly what is on screen instead of guessing.                   */
static int   g_dump_want[16];
static int   g_dump_n;
static char  g_dump_dir[512];

static void dump_init(void){
    const char *e = ENVC("DUMPFRAME");
    if (!e || !*e) return;
    const char *d = ENVC("DUMPDIR");
    snprintf(g_dump_dir, sizeof g_dump_dir, "%s", d && *d ? d : "/tmp");
    for (const char *p = e; *p && g_dump_n < 16; ){
        while (*p == ',' || *p == ' ') p++;
        if (!*p) break;
        g_dump_want[g_dump_n++] = atoi(p);
        while (*p && *p != ',') p++;
    }
}
/* CAPTURE SHORTCUT: with the game in front, Cmd+Shift+D -> the next frame is
 * written as PNG + DRAWLIST + DRAWSUM (to the PERFLOG directory, otherwise
 * ~/Library/Logs/NFSU2/).  For scenes the user reaches but automated tests
 * can't produce (e.g. the end of a career race). */
static _Atomic int g_dump_req;
static long g_dump_req_f = -1;
static int dump_wanted(long f){
    if (f == g_dump_req_f) return 1;
    for (int i = 0; i < g_dump_n; i++) if (g_dump_want[i] == (int)f) return 1;
    return 0;
}
static void dump_texture(id<MTLTexture> t, long frame){
    NSUInteger w = t.width, h = t.height, stride = w * 4;
    void *buf = malloc(stride * h);
    if (!buf) return;
    [t getBytes:buf bytesPerRow:stride fromRegion:MTLRegionMake2D(0,0,w,h) mipmapLevel:0];
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    /* Metal BGRA8Unorm -> CG: 32-bit little endian + premultiplied first */
    CGContextRef cg = CGBitmapContextCreate(buf, w, h, 8, stride, cs,
        kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
    if (cg){
        CGImageRef img = CGBitmapContextCreateImage(cg);
        char path[640];
        snprintf(path, sizeof path, "%s/nfsu2_frame_%04ld.png", g_dump_dir, frame);
        CFStringRef sp = CFStringCreateWithCString(NULL, path, kCFStringEncodingUTF8);
        CFURLRef url = CFURLCreateWithFileSystemPath(NULL, sp, kCFURLPOSIXPathStyle, false);
        CGImageDestinationRef dst =
            CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL);
        if (dst){ CGImageDestinationAddImage(dst, img, NULL);
                  CGImageDestinationFinalize(dst); CFRelease(dst); }
        CFRelease(url); CFRelease(sp); CGImageRelease(img); CGContextRelease(cg);
        fprintf(stderr, "metal: frame %ld written -> %s\n", frame, path);
    }
    CGColorSpaceRelease(cs); free(buf);
}
static id<MTLDevice>    g_dev;
static id<MTLCommandQueue> g_queue;

/* Frame state written by the guest thread and read by the main thread */
static _Atomic float    g_clear_r = 0.f, g_clear_g = 0.f, g_clear_b = 0.f;
static _Atomic int      g_frame_pending = 0;
/* Who waits for whom?  Frame time = guest CPU + the main thread's pickup
 * latency + vsync.  Without separating the three we can't know which one to
 * optimize. */
static _Atomic unsigned long long g_wait_present_ns = 0;  /* guest: for the previous frame to be picked up */
static _Atomic unsigned long long g_wait_drawable_ns = 0; /* main: nextDrawable (vsync) */
static _Atomic unsigned long long g_wait_frame_ns = 0;    /* main: for the frame to be ready */

static _Atomic long     g_frames_drawn  = 0;
static _Atomic long     g_tris_drawn    = 0;
static _Atomic long     g_tex_created   = 0, g_tex_uploaded = 0;
static _Atomic int      g_should_quit   = 0;

/* --- mouse state: the front end is navigated with the mouse (DIMOUSESTATE2) --------------
 * DirectInput wants relative motion; we also keep the absolute position for
 * GetCursorPos. */
static _Atomic int g_mouse_x = 640, g_mouse_y = 360;
/* --- presentation geometry ---------------------------------------------
 * The drawing surface is in PIXELS, the window content in POINTS; on Retina the
 * two differ by 2x.  Treating them as equal led to two separate bugs: (1) the
 * 1280x960 surface was upscaled into a 2560x1836-pixel area and every pixel was
 * resampled, so the image got blurry, (2) mouse points were taken for pixels, so
 * the cursor drifted. */
static _Atomic int g_draw_w = 1280, g_draw_h = 720;   /* drawing - pixels */
static _Atomic int g_cont_w = 1280, g_cont_h = 720;   /* content - points */
/* The resolution the game THINKS it has.  At Retina scale it is smaller than the
 * drawing surface (the game thinks 1280x960 while the surface is 2448x1836); the
 * cursor must be in this space. */
static _Atomic int g_game_w = 1280, g_game_h = 720;
/* --- fullscreen ----------------------------------------------------------
 * In fullscreen the drawing surface goes to the SCREEN's pixel size (and aspect
 * ratio); the game's 4:3 request is corrected in clip space with Hor+
 * (res_pick/aspfix).  Since during the transition the size can only change
 * through the game's Reset, com_request_reset() produces a device loss.
 * g_fs_pw/ph: the fullscreen content area (pixels), measured from AppKit on entry. */
static _Atomic int g_fs = 0, g_fs_pw = 0, g_fs_ph = 0;
/* The UI (orthographic) is narrowed about the center by aspfix with HUDFIX;
 * the mouse must be mapped to that area too.  x10000 integer. */
static _Atomic int g_ui_asp = 10000;
void metal_set_ui_aspect(float a){
    if (!(a > 0.05f && a <= 1.0f)) a = 1.0f;
    atomic_store(&g_ui_asp, (int)lroundf(a * 10000.f));
}
void com_request_reset(void);
static _Atomic int g_mouse_dx = 0, g_mouse_dy = 0, g_mouse_dz = 0;
static _Atomic unsigned g_mouse_btn = 0;

void metal_mouse_add(int dx, int dy){                 /* synthetic motion (test) */
    atomic_fetch_add(&g_mouse_dx, dx); atomic_fetch_add(&g_mouse_dy, dy); }
void metal_fill_mouse(unsigned char *dst, unsigned n){
    if (n < 20) return;
    int dx = atomic_exchange(&g_mouse_dx, 0);
    int dy = atomic_exchange(&g_mouse_dy, 0);
    int dz = atomic_exchange(&g_mouse_dz, 0);
    unsigned b = atomic_load(&g_mouse_btn);
    memcpy(dst + 0, &dx, 4);
    memcpy(dst + 4, &dy, 4);
    memcpy(dst + 8, &dz, 4);
    for (int i = 0; i < 8; i++) dst[12 + i] = (b >> i) & 1u ? 0x80 : 0x00;
}
void metal_get_cursor(int *x, int *y){
    /* MOUSEAT=<x>,<y>[@<frame>,<x>,<y>]: pin the cursor in game space (test) */
    { const char *e = ENVC("MOUSEAT");
      if (e && *e){ int ax = 0, ay = 0, f2 = 0, bx = 0, by = 0;
          int n2 = sscanf(e, "%d,%d@%d,%d,%d", &ax, &ay, &f2, &bx, &by);
          if (n2 == 5 && atomic_load(&g_frames_drawn) >= f2){ ax = bx; ay = by; }
          if (x) *x = ax; if (y) *y = ay; return; } }
    if (x) *x = atomic_load(&g_mouse_x);
    if (y) *y = atomic_load(&g_mouse_y);
}
/* The window size the game thinks it has (GetClientRect/GetWindowRect).  The
 * game scales GetCursorPos into its own UI space relative to this rectangle;
 * while a fixed 1280x720 was returned, at 1280x960 the cursor drifted down
 * vertically by 1.33x (measured: MOUSEAT). */
void metal_game_size(unsigned *w, unsigned *h){
    if (w) *w = (unsigned)atomic_load(&g_game_w);
    if (h) *h = (unsigned)atomic_load(&g_game_h);
}
void metal_set_game_size(unsigned w, unsigned h){
    if (!w || !h || w > 8192u || h > 8192u) return;
    atomic_store(&g_game_w, (int)w); atomic_store(&g_game_h, (int)h);
}
/* The area the image covers in the window (points).  Since it is fitted to the
 * content KEEPING THE ASPECT RATIO (kCAGravityResizeAspect), black bars appear
 * at the edges in fullscreen.  If it returns 0 the geometry isn't known yet. */
static int shown_rect(double *ox, double *oy, double *vw, double *vh){
    double dw = atomic_load(&g_draw_w), dh = atomic_load(&g_draw_h);
    double cw = atomic_load(&g_cont_w), ch = atomic_load(&g_cont_h);
    if (dw < 1.0 || dh < 1.0 || cw < 1.0 || ch < 1.0) return 0;
    double k = fmin(cw / dw, ch / dh);           /* points / drawing pixel */
    *vw = dw * k; *vh = dh * k;
    *ox = (cw - *vw) * 0.5; *oy = (ch - *vh) * 0.5;
    return 1;
}
/* Window point -> the GAME's resolution space (not the drawing surface: at Retina
 * scale the game thinks 1280x960 while the surface is 2448x1836).  Accounts for
 * the bars; Y is flipped: AppKit bottom-left, game top-left. */
static void mouse_from_point(double px, double py, int *gx, int *gy){
    double ox, oy, vw, vh;
    double lw = atomic_load(&g_game_w), lh = atomic_load(&g_game_h);
    double ch = atomic_load(&g_cont_h);
    if (!shown_rect(&ox, &oy, &vw, &vh) || lw < 1.0 || lh < 1.0){
        *gx = (int)px; *gy = (int)py; return; }
    { double a = atomic_load(&g_ui_asp) / 10000.0;   /* UI in the middle, narrowed */
      ox += vw * (1.0 - a) * 0.5; vw *= a; }
    double x = (px - ox) / vw * lw;
    double y = (ch - py - oy) / vh * lh;
    if (x < 0.0) x = 0.0; else if (x > lw - 1.0) x = lw - 1.0;
    if (y < 0.0) y = 0.0; else if (y > lh - 1.0) y = lh - 1.0;
    *gx = (int)x; *gy = (int)y;
}
/* DirectInput relative motion.  Previously window POINT = game pixel, so the
 * point delta went through as is.  Since the window is now sized independently of
 * the game's resolution, we scale it to keep the same feel: going from one edge
 * of the image to the other still produces motion equal to the game width.
 * (Unscaled, with NFSNATIVE=0 the window was half size so the cursor moved at
 * half speed.) */
static void mouse_delta_scale(double *sx, double *sy){
    double ox, oy, vw, vh;
    double lw = atomic_load(&g_game_w), lh = atomic_load(&g_game_h);
    *sx = *sy = 1.0;
    if (!shown_rect(&ox, &oy, &vw, &vh) || lw < 1.0 || lh < 1.0) return;
    vw *= atomic_load(&g_ui_asp) / 10000.0;
    *sx = lw / vw; *sy = lh / vh;
}

/* --- keyboard state: the 256-byte table DirectInput will read --- */
static _Atomic unsigned char g_keys[256];

/* macOS virtual key code -> DirectInput DIK_* scan code */
static unsigned char mac_to_dik(unsigned short kc){
    switch (kc){
    case 0x00: return 0x1E; /* A */   case 0x01: return 0x1F; /* S */
    case 0x02: return 0x20; /* D */   case 0x03: return 0x21; /* F */
    case 0x04: return 0x23; /* H */   case 0x05: return 0x22; /* G */
    case 0x06: return 0x2C; /* Z */   case 0x07: return 0x2D; /* X */
    case 0x08: return 0x2E; /* C */   case 0x09: return 0x2F; /* V */
    case 0x0B: return 0x30; /* B */   case 0x0C: return 0x10; /* Q */
    case 0x0D: return 0x11; /* W */   case 0x0E: return 0x12; /* E */
    case 0x0F: return 0x13; /* R */   case 0x10: return 0x15; /* Y */
    case 0x11: return 0x14; /* T */   case 0x1F: return 0x18; /* O */
    case 0x20: return 0x16; /* U */   case 0x22: return 0x17; /* I */
    case 0x23: return 0x19; /* P */   case 0x25: return 0x26; /* L */
    case 0x26: return 0x24; /* J */   case 0x28: return 0x25; /* K */
    case 0x2D: return 0x31; /* N */   case 0x2E: return 0x32; /* M */
    case 0x12: return 0x02; case 0x13: return 0x03; case 0x14: return 0x04;
    case 0x15: return 0x05; case 0x17: return 0x06; case 0x16: return 0x07;
    case 0x1A: return 0x08; case 0x1C: return 0x09; case 0x19: return 0x0A;
    case 0x1D: return 0x0B;                        /* 1..0 */
    case 0x24: return 0x1C; /* Return  -> DIK_RETURN */
    case 0x30: return 0x0F; /* Tab     */
    case 0x31: return 0x39; /* Space   */
    case 0x33: return 0x0E; /* Delete  -> BACK */
    case 0x35: return 0x01; /* Escape  */
    case 0x7B: return 0xCB; /* Left    */
    case 0x7C: return 0xCD; /* Right   */
    case 0x7D: return 0xD0; /* Down    */
    case 0x7E: return 0xC8; /* Up      */
    case 0x38: return 0x2A; /* Shift   */
    case 0x3B: return 0x1D; /* Control */
    default:   return 0;
    }
}
/* --- typed characters -> WM_CHAR to the window procedure ------------------
 * The game takes text input (career profile name etc.) NOT from DirectInput but
 * from the WM_CHAR branch of the window procedure (0x005ccd70 -> 0x0055dbe0 ->
 * text box 0x00551280: 0x0D accept, 0x1B cancel, 0x08 delete, the rest is
 * appended).  On Windows TranslateMessage produces this; we produce it directly
 * from the character macOS decoded (keyboard layout, Shift included).
 * A single-producer (main thread) / single-consumer (PeekMessage) ring. */
#define CHQ 128u
static unsigned char g_chq[CHQ];
static _Atomic unsigned g_chq_w, g_chq_r;
static void char_push(unsigned char ch){
    unsigned w = atomic_load(&g_chq_w), r = atomic_load(&g_chq_r);
    if (w - r >= CHQ) return;                 /* full: drop */
    g_chq[w % CHQ] = ch;
    atomic_store(&g_chq_w, w + 1);
}
/* remove=0: just peek (PM_NOREMOVE).  -1 if empty. */
int metal_take_char(int remove){
    unsigned r = atomic_load(&g_chq_r);
    if (r == atomic_load(&g_chq_w)) return -1;
    int ch = g_chq[r % CHQ];
    if (remove) atomic_store(&g_chq_r, r + 1);
    return ch;
}
/* The character(s) a key press produces.  The game uses single-byte
 * Windows-1252: U+00A0..U+00FF are the same values; letters specific to Turkish
 * aren't in 1252 and are folded to their nearest ASCII equivalent. */
static void char_from_event(NSEvent *ev){
    switch ([ev keyCode]){
    case 0x24: case 0x4C: char_push(0x0D); return;   /* Return / Enter */
    case 0x33: char_push(0x08); return;              /* Delete -> Backspace */
    case 0x35: char_push(0x1B); return;              /* Escape */
    case 0x30: char_push(0x09); return;              /* Tab */
    default: break;
    }
    if ([ev modifierFlags] & NSEventModifierFlagControl) return;
    NSString *s = [ev characters];
    for (NSUInteger i = 0; i < [s length]; i++){
        unichar u = [s characterAtIndex:i];
        switch (u){
        case 0x0131: u = 'i'; break;  case 0x0130: u = 'I'; break;   /* ı İ */
        case 0x015F: u = 's'; break;  case 0x015E: u = 'S'; break;   /* ş Ş */
        case 0x011F: u = 'g'; break;  case 0x011E: u = 'G'; break;   /* ğ Ğ */
        default: break;
        }
        if ((u >= 0x20 && u < 0x7F) || (u >= 0xA0 && u <= 0xFF))
            char_push((unsigned char)u);
    }
}
/* AUTOTEXT="<frame>:<text>": "types" the text from that frame on, one letter
 * every 6 frames (diagnostic/test).  \n -> Enter (0x0D), \b -> Backspace. */
static void autotext_tick(long f){
    static int init = 0; static long start = -1; static char txt[128]; static int n = 0, pos = 0;
    if (!init){ init = 1;
        const char *e = getenv("AUTOTEXT");
        const char *c = e ? strchr(e, ':') : 0;
        if (c){ start = strtol(e, 0, 0);
            for (const char *q = c + 1; *q && n < (int)sizeof txt - 1; q++){
                if (q[0] == '\\' && q[1] == 'n'){ txt[n++] = 0x0D; q++; }
                else if (q[0] == '\\' && q[1] == 'b'){ txt[n++] = 0x08; q++; }
                else txt[n++] = *q;
            } }
    }
    if (start < 0 || pos >= n || f < start) return;
    if ((f - start) / 6 >= pos){ char_push((unsigned char)txt[pos]); pos++; }
}

/* Guest side: fill the DirectInput keyboard state */
void metal_fill_keyboard(unsigned char *dst, unsigned n){
    if (n > 256) n = 256;
    for (unsigned i = 0; i < n; i++)
        dst[i] = atomic_load(&g_keys[i]) ? 0x80 : 0x00;
}

/* Textures are created BEFORE the window opens.  So the "usable" criterion is the
 * device, not the layer; otherwise early textures silently vanished. */
int  metal_available(void){ return g_dev != nil; }
int  metal_window_ready(void){ return g_layer != nil; }

long metal_frames(void){ return atomic_load(&g_frames_drawn); }
long metal_tris(void){ return atomic_load(&g_tris_drawn); }
long metal_tex_created(void){ return atomic_load(&g_tex_created); }
long metal_tex_uploaded(void){ return atomic_load(&g_tex_uploaded); }
int  metal_quit_requested(void){ return atomic_load(&g_should_quit); }
/* The guest ended (ExitProcess / TerminateProcess, e.g. "Quit Game"): the main
 * loop must close the window too, otherwise the app stays open with a frozen
 * window and has to be force-quit. */
void metal_request_quit(void){ atomic_store(&g_should_quit, 1); }

/* ---------------- draw command queue ----------------
 * The guest thread accumulates draws here; the main thread encodes them at Present. */
#include "gfx.h"
typedef TVert MVert;   /* clip space + two colors + 4x4 texture coordinates */
/* target: 0 = screen (drawable), otherwise the offscreen target g_tex[target].
 * An entry with is_clear=1 isn't a draw but a command to clear the target. */
/* blend: packed blend state (see metal_blend_key)
 *   bit0      : alpha blending on
 *   bit1-5    : source factor (D3DBLEND)
 *   bit6-10   : destination factor
 *   bit11-13  : blend operation (D3DBLENDOP)
 *   bit14-17  : color write mask                                         */
/* vsid > 0 : the vertex shader runs on the GPU.  In this case the vertices are
 * RAW in g_rvbuf (rawoff), not in g_vbuf, and the constants are in g_kbuf (koff);
 * the draw is made with vertexStart:0 from the buffer offset. */
/* ps_2_0 can use 32 float constants.  The file was cut at 8, and worse, vsemu.c
 * FOLDED a high index with 'idx & 7': a shader reading constant c8 saw c0.  The
 * post-process (bloom threshold, blur tap weights) uses exactly that range. */
#define PSC_N 32
typedef struct { unsigned first, count, tex, target, clear_argb, is_clear;
                 unsigned blend; float alpha_ref; unsigned depth;
                 unsigned tex1, tex2, tex3; int ps;
                 /* the ps constants are in the frame's pool (g_pscb[b][psci]); c0 separately
                  * (the HD ray tracing material only reads that) */
                 unsigned psci; float c0[4];
                 int vsid; unsigned rawoff, koff; float proj[4];
                 unsigned smp[4];
                 unsigned is_blit, blit_src; unsigned bcol; unsigned cull;
                 /* StretchRect with a source rectangle: the part of the source to copy (uv) */
                 unsigned blit_rect; float blit_uv[4];
                 float fogv[4];
                 unsigned clr_flags; float clr_z; unsigned ksz;
                 /* indexed draw (see PERSISTENT VERTEX / INDEX POOL): if ix_isz is 2/4,
                  * rawoff is the offset of vertex 0 in the pool, ix_ib the offset of the
                  * first index; count is the index count.  gh: geometry id (0 = hash
                  * from the content). */
                 unsigned ix_isz, ix_ib; int ix_base; unsigned long long gh;
                 /* could the draw cover the target completely (see PASS LOAD):
                  * if covok, cov = the post-clip NDC rectangle x0,x1,y0,y1 */
                 unsigned covok; float cov[4];
                 /* stage 0 UV at the rectangle's edges: u(x0) u(x1) v(y0) v(y1)
                  * (if the vertex data is affine and consistent; cuvok) */
                 unsigned cuvok; float cuv[4];
                 /* UV SCALE: the corrected oT sets (also in K.pad1) and the difference (VS buffer 3) */
                 unsigned uvbm; float uvb[2];
                 /* HUD ANCHORING: the 4:3 clip rectangle of a HUD draw (com.c) and the
                  * x shift planned for it (VS buffer 3 .z) */
                 unsigned hudok; float hud[4]; float xsh;
                 /* ORDERED TEXTURE UPLOADS: into tex's up_level / up_face, from the frame's
                  * upload area (rawoff), up_w x up_h, rows up_pitch bytes */
                 unsigned is_up, up_level, up_face, up_w, up_h, up_pitch; } MDraw;
/* the coverage rectangle of the next draw (com.c -> metal_set_next_cov_clip) */
static int g_nc_ok = 0; static float g_nc[4];
static int g_ncuv_ok = 0; static float g_ncuv[4];
static float g_ncuv_in[6][16]; static unsigned g_ncuv_n = 0; static int g_ncuv_valid = 0;
/* see UV SCALE: the coordinate set correction of the next draw */
static unsigned g_nuvb_mask = 0; static float g_nuvb[2];

#define MAXV  (256*1024)
#define MAXD  (16*1024)
#define MAXRV (16u*1024u*1024u)      /* raw vertex arena (bytes) */
#define MAXKB ( 4u*1024u*1024u)      /* vertex constant arena (bytes) */
static MVert  *g_vbuf[2];
static MDraw  *g_dbuf[2];
static unsigned g_vn[2], g_dn[2];
static uint8_t *g_rvbuf[2];  static unsigned g_rvn[2];
static uint8_t *g_kbuf[2];   static unsigned g_kn[2];
static id<MTLBuffer> g_mtlrvb, g_mtlkb;
/* ---- ORDERED TEXTURE UPLOADS ----------------------------------------------------
 * The game rewrites some textures every frame (measured: the post-process color
 * tables #8 / #9, 5467 uploads in 5467 frames).  The upload went to the texture
 * IMMEDIATELY with replaceRegion: while the GPU was still drawing the previous
 * frame, the next frame's table (or a half-written table) could be seen; even
 * encoding the same draw twice in the same frame gave different results (found in
 * the post-process chain verification).  In D3D9 every draw sees the content as of
 * the moment it was submitted.  The second and later uploads now go into the draw
 * list as ORDERED records; the data is copied into the frame's upload area, and
 * during encoding it becomes a blit (buffer to texture) ordered with the draws.
 * The first upload (loading screen, a texture not used yet) is immediate as
 * before.  The pending uploads of a skipped frame are applied immediately before
 * the buffer is reused.
 * TEXORD=0 is the old behavior. */
#define MAXUP (8u*1024u*1024u)
static uint8_t *g_upbuf[2]; static unsigned g_upn[2];
static _Atomic unsigned long long g_up_ord = 0, g_up_ovf = 0;
static int texord_on(void){
    static int on = -1; if (on < 0){ const char *e = ENVC("TEXORD"); on = (e && *e == '0') ? 0 : 1; }
    return on;
}
static unsigned g_ov_rv = 0, g_ov_kb = 0;   /* arena overflow counter */
/* --- frame budget measurement: how busy is the GPU really? --------------
 * Until now only the CPU side was measured; the question "what are we spending
 * the hardware on" can't be answered without knowing the GPU time. */
static _Atomic unsigned long long g_gpu_ns = 0, g_gpu_n = 0, g_gpu_max_ns = 0;
static _Atomic unsigned long long g_enc_ns = 0;         /* encoding (CPU) */
/* GPUWIN=a,b: the same measurements only for frames [a,b) (so the menus don't
 * dilute the mean; for measuring the GPU cost per feature in the race window) */
static _Atomic unsigned long long g_gpuw_ns = 0, g_gpuw_n = 0, g_encw_ns = 0, g_encw_n = 0;
static long g_gpuw_a = -1, g_gpuw_b = -1;
/* --- GPU ENERGY and MEMORY TRAFFIC (IOReport, no sudo needed) ---------------
 * GPU time (ms/frame) doesn't measure the amount of work because of frequency
 * scaling: when work drops macOS lowers the GPU clock, and the same frame can take
 * LONGER (measured 2026-10-18: 5.3 -> 5.8/7.6 ms on an improvement that cut memory
 * traffic).  We use two measures, both from IOReport and neither needing admin
 * rights:
 *  - "Energy Model / GPU Energy" (nJ): powermetrics's GPU power.  The modelled
 *    energy of the GPU core; does NOT INCLUDE DRAM.
 *  - "AMC Stats / Perf Counters": bytes per agent at the memory controller.
 *    GFX RD/WR = the GPU's reads / writes to the memory system (including the
 *    system cache), GFX DCS RD/WR = the part of that going to DRAM.  A measure of
 *    WORK independent of DVFS: whatever the GPU clock, the bytes a frame carries
 *    are the same.
 * Since it is a private library, it is opened at run time with dlopen. */
#include <dlfcn.h>
#include <mach/mach_time.h>
/*  - "GPU Stats / GPU Performance States" (GPUPH): time per P-state.
 *    Multiplied by the frequency table (pmgr voltage-states9) it gives BUSY GPU
 *    CYCLES: a measure of work independent of DVFS; divided by the highest clock
 *    it gives "how many ms at that clock" = whether we can hold 120 fps (the
 *    8.33 ms limit). */
typedef struct { double j, rd, wr, drd, dwr, gcy, gact, gtot; } IorS;   /* J, bytes, cycles, ticks; since the first call */
static double g_ior_fmax;                     /* MHz, the GPU's highest P-state (0: no table) */
static int ior_read(IorS *o){                 /* 1 = valid */
    static int st = 0;
    static void *sub; static CFMutableDictionaryRef subch; static CFDictionaryRef s0;
    static CFDictionaryRef (*cs)(void *, CFMutableDictionaryRef, CFTypeRef);
    static CFDictionaryRef (*cd)(CFDictionaryRef, CFDictionaryRef, CFTypeRef);
    static void (*it)(CFDictionaryRef, int (^)(CFDictionaryRef));
    static CFStringRef (*gn)(CFDictionaryRef), (*gu)(CFDictionaryRef);
    static int64_t (*gv)(CFDictionaryRef, int32_t);
    static int32_t (*sct)(CFDictionaryRef); static CFStringRef (*snm)(CFDictionaryRef, int32_t);
    static int64_t (*srs)(CFDictionaryRef, int32_t);
    static double fq[16]; static int nfq; static double tick_mhz = 24.0;
    if (st == 0){
        st = -1;
        void *h = dlopen("/usr/lib/libIOReport.dylib", RTLD_LAZY);
        if (!h) return 0;
        CFMutableDictionaryRef (*cc)(CFStringRef, CFStringRef, uint64_t, uint64_t, uint64_t) =
            (CFMutableDictionaryRef (*)(CFStringRef, CFStringRef, uint64_t, uint64_t, uint64_t))dlsym(h, "IOReportCopyChannelsInGroup");
        void (*mg)(CFMutableDictionaryRef, CFDictionaryRef, CFTypeRef) =
            (void (*)(CFMutableDictionaryRef, CFDictionaryRef, CFTypeRef))dlsym(h, "IOReportMergeChannels");
        void *(*csub)(void *, CFMutableDictionaryRef, CFMutableDictionaryRef *, uint64_t, CFTypeRef) =
            (void *(*)(void *, CFMutableDictionaryRef, CFMutableDictionaryRef *, uint64_t, CFTypeRef))dlsym(h, "IOReportCreateSubscription");
        cs = (CFDictionaryRef (*)(void *, CFMutableDictionaryRef, CFTypeRef))dlsym(h, "IOReportCreateSamples");
        cd = (CFDictionaryRef (*)(CFDictionaryRef, CFDictionaryRef, CFTypeRef))dlsym(h, "IOReportCreateSamplesDelta");
        it = (void (*)(CFDictionaryRef, int (^)(CFDictionaryRef)))dlsym(h, "IOReportIterate");
        gn = (CFStringRef (*)(CFDictionaryRef))dlsym(h, "IOReportChannelGetChannelName");
        gu = (CFStringRef (*)(CFDictionaryRef))dlsym(h, "IOReportChannelGetUnitLabel");
        gv = (int64_t (*)(CFDictionaryRef, int32_t))dlsym(h, "IOReportSimpleGetIntegerValue");
        if (!cc || !csub || !cs || !cd || !it || !gn || !gu || !gv) return 0;
        CFMutableDictionaryRef ch = cc(CFSTR("Energy Model"), NULL, 0, 0, 0);
        if (!ch) return 0;
        CFMutableDictionaryRef ch2 = cc(CFSTR("AMC Stats"), CFSTR("Perf Counters"), 0, 0, 0);
        if (ch2 && mg) mg(ch, ch2, NULL);
        sct = (int32_t (*)(CFDictionaryRef))dlsym(h, "IOReportStateGetCount");
        snm = (CFStringRef (*)(CFDictionaryRef, int32_t))dlsym(h, "IOReportStateGetNameForIndex");
        srs = (int64_t (*)(CFDictionaryRef, int32_t))dlsym(h, "IOReportStateGetResidency");
        {   /* GPU P-state frequencies: pmgr "voltage-states9" = (Hz, mV) pairs, 0 Hz = OFF */
            unsigned (*gms)(unsigned, CFDictionaryRef) = (unsigned (*)(unsigned, CFDictionaryRef))dlsym(RTLD_DEFAULT, "IOServiceGetMatchingService");
            CFMutableDictionaryRef (*nmm)(const char *) = (CFMutableDictionaryRef (*)(const char *))dlsym(RTLD_DEFAULT, "IOServiceNameMatching");
            CFTypeRef (*rcp)(unsigned, CFStringRef, CFAllocatorRef, uint32_t) = (CFTypeRef (*)(unsigned, CFStringRef, CFAllocatorRef, uint32_t))dlsym(RTLD_DEFAULT, "IORegistryEntryCreateCFProperty");
            int (*orl)(unsigned) = (int (*)(unsigned))dlsym(RTLD_DEFAULT, "IOObjectRelease");
            unsigned sv = (gms && nmm) ? gms(0, nmm("pmgr")) : 0;
            CFTypeRef vs = (sv && rcp) ? rcp(sv, CFSTR("voltage-states9"), kCFAllocatorDefault, 0) : NULL;
            if (vs && CFGetTypeID(vs) == CFDataGetTypeID()){
                const uint32_t *w = (const uint32_t *)CFDataGetBytePtr((CFDataRef)vs);
                long nw = CFDataGetLength((CFDataRef)vs) / 4;
                for (long q = 0; q + 1 < nw && nfq < 16; q += 2) if (w[q]) fq[nfq++] = w[q] / 1e6;
            }
            if (vs) CFRelease(vs);
            if (sv && orl) orl(sv);
            for (int q = 0; q < nfq; q++) if (fq[q] > g_ior_fmax) g_ior_fmax = fq[q];
            mach_timebase_info_data_t tb; mach_timebase_info(&tb);   /* state time in mach ticks */
            if (tb.numer) tick_mhz = 1e3 * tb.denom / tb.numer;
        }
        CFMutableDictionaryRef ch3 = (sct && snm && srs) ? cc(CFSTR("GPU Stats"), CFSTR("GPU Performance States"), 0, 0, 0) : NULL;
        if (ch3 && mg) mg(ch, ch3, NULL);
        sub = csub(NULL, ch, &subch, 0, NULL);
        if (!sub || !subch) return 0;
        s0 = cs(sub, subch, NULL);
        if (!s0) return 0;
        st = 1;
    }
    if (st < 0) return 0;
    CFDictionaryRef s1 = cs(sub, subch, NULL); if (!s1) return 0;
    CFDictionaryRef dd = cd(s0, s1, NULL); CFRelease(s1); if (!dd) return 0;
    __block IorS r = { -1, 0, 0, 0, 0, 0, 0, 0 };
    it(dd, ^int(CFDictionaryRef smp){
        CFStringRef n = gn(smp); if (!n) return 0;
        char nm[40]; if (!CFStringGetCString(n, nm, sizeof nm, kCFStringEncodingUTF8)) return 0;
        if (!strcmp(nm, "GPU Energy")){
            CFStringRef u = gu(smp); double k = 1e-9;                     /* nJ */
            if (u && CFStringCompare(u, CFSTR("uJ"), 0) == kCFCompareEqualTo) k = 1e-6;
            if (u && CFStringCompare(u, CFSTR("mJ"), 0) == kCFCompareEqualTo) k = 1e-3;
            r.j = (double)gv(smp, 0) * k; }
        else if (!strcmp(nm, "GFX RD"))     r.rd  = (double)gv(smp, 0);
        else if (!strcmp(nm, "GFX WR"))     r.wr  = (double)gv(smp, 0);
        else if (!strcmp(nm, "GFX DCS RD")) r.drd = (double)gv(smp, 0);
        else if (!strcmp(nm, "GFX DCS WR")) r.dwr = (double)gv(smp, 0);
        else if (!strcmp(nm, "GPUPH") && sct){
            int32_t k = sct(smp);
            for (int32_t q = 0; q < k; q++){
                double t = (double)srs(smp, q); r.gtot += t;
                char sn[16] = ""; CFStringRef x = snm(smp, q);
                if (!x || !CFStringGetCString(x, sn, sizeof sn, kCFStringEncodingUTF8) || sn[0] != 'P') continue;
                int pi = atoi(sn + 1); r.gact += t;
                if (pi >= 1 && pi <= nfq) r.gcy += t / tick_mhz * fq[pi - 1];   /* ticks / (ticks/us) * MHz = cycles */
            } }
        return 0; });
    CFRelease(dd);
    *o = r;
    return r.j >= 0;
}
static IorS g_gS0, g_gS1; static int g_gS0ok = 0, g_gS1ok = 0; static double g_gT0, g_gT1;
static double gpuE_now_s(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static int gpuw_in(long f){
    if (g_gpuw_a == -1){ const char *e = ENVC("GPUWIN"); long a = 0, b = 0;
        if (e && sscanf(e, "%ld,%ld", &a, &b) == 2 && b > a){ g_gpuw_b = b; g_gpuw_a = a; } else g_gpuw_a = -2; }
    return g_gpuw_a >= 0 && f >= g_gpuw_a && f < g_gpuw_b;
}
/* ---- IN-RUN A/B (ABTEST=NAME[,N[,SKIP]]) ---------------------------------
 * The GPU energy of separate runs varies a lot (measured 2026-10-18: 30.8 / 34.9
 * / 40.0 mJ/frame with the same settings -- DVFS state and the rest of the system).
 * So measure a feature's cost within the SAME run: the GPUWIN window is split into
 * N-frame slices (default 120), feature NAME is ON in even slices and OFF in odd
 * ones.  The first SKIP frames of a slice (default 8: the previous mode's work in
 * the queue) are dropped; in the rest the GPU energy, GPU memory traffic (two
 * IOReport samples) and GPU time are accumulated per mode.  The report gives the
 * difference of neighboring slice pairs as mean +- standard error: slowly changing
 * DVFS / background load weighs equally on both modes.  A feature asks for its own
 * switch with abt_off("NAME"). */
#define ABT_MAXB 1024
static const char *g_abt_name = NULL;
static int g_abt_init = 0, g_abt_n = 120, g_abt_skip = 8;
static int g_abt_cur_off = 0;                 /* this frame (encoding thread): feature off */
static IorS g_abt_d[ABT_MAXB];                /* the measured part of the slice (difference) */
static uint8_t g_abt_ok[ABT_MAXB];
static IorS g_abt_s0; static int g_abt_s0ok = 0; static long g_abt_blk = -1;
static _Atomic unsigned long long g_abt_ns[2], g_abt_cnt[2];
static int abt_off(const char *name){
    return g_abt_cur_off && g_abt_name && !strcmp(g_abt_name, name);
}
/* called at the start of encoding: mode (-1 = outside A/B, 0 = on, 1 = off);
 * *cnt = whether the frame's GPU time counts */
static int abt_frame(long f, int *cnt){
    *cnt = 0; g_abt_cur_off = 0;
    if (!g_abt_init){ g_abt_init = 1;
        const char *e = ENVC("ABTEST");
        if (e && *e){ static char nm[32]; int n = 0, s = -1;
            sscanf(e, "%31[^,],%d,%d", nm, &n, &s);
            g_abt_name = nm; if (n >= 16) g_abt_n = n; if (s >= 0 && s < g_abt_n / 2) g_abt_skip = s;
            fprintf(stderr, "ABTEST: %s, %d-frame slices (first %d dropped), even slices ON\n",
                    g_abt_name, g_abt_n, g_abt_skip); } }
    if (!g_abt_name || g_gpuw_a < 0) return -1;
    long r = f - g_gpuw_a;
    if (r < 0 || f > g_gpuw_b) return -1;
    long b = r / g_abt_n, k = r % g_abt_n;
    /* the measured part of the previous slice is done (including the window's last frame) */
    if (k == 0 && b > 0 && g_abt_blk == b - 1 && b - 1 < ABT_MAXB){
        IorS x; if (g_abt_s0ok && ior_read(&x)){
            IorS *d = &g_abt_d[b - 1];
            d->j = x.j - g_abt_s0.j; d->rd = x.rd - g_abt_s0.rd; d->wr = x.wr - g_abt_s0.wr;
            d->drd = x.drd - g_abt_s0.drd; d->dwr = x.dwr - g_abt_s0.dwr;
            d->gcy = x.gcy - g_abt_s0.gcy; d->gact = x.gact - g_abt_s0.gact; d->gtot = x.gtot - g_abt_s0.gtot; g_abt_ok[b - 1] = 1; }
        g_abt_blk = -1; }
    if (f >= g_gpuw_b || (b + 1) * g_abt_n > g_gpuw_b - g_gpuw_a) return -1;   /* half a slice: outside A/B */
    if (k == g_abt_skip){ g_abt_s0ok = ior_read(&g_abt_s0); g_abt_blk = b; }
    g_abt_cur_off = (int)(b & 1);
    *cnt = k >= g_abt_skip;
    return g_abt_cur_off;
}
/* measure q (0 energy mJ, 1 GPU read MB, 2 GPU write MB, 3 DRAM read MB, 4 DRAM write MB,
 * 5 busy GPU Mcycles) per frame */
static double abt_val(long b, int q){
    double fr = (double)(g_abt_n - g_abt_skip); const IorS *d = &g_abt_d[b];
    double v = q == 0 ? d->j * 1e3 : q == 1 ? d->rd / 1e6 : q == 2 ? d->wr / 1e6 : q == 3 ? d->drd / 1e6
             : q == 4 ? d->dwr / 1e6 : d->gcy / 1e6;
    return v / fr;
}
static void abt_report(void){
    if (!g_abt_name || g_gpuw_a < 0) return;
    static const char *qn[6] = { "GPU energy     mJ/frame", "GPU read       MB/frame", "GPU write      MB/frame",
                                 "DRAM read      MB/frame", "DRAM write     MB/frame", "GPU busy Mcycles/frame" };
    long nb = (g_gpuw_b - g_gpuw_a) / g_abt_n; if (nb > ABT_MAXB) nb = ABT_MAXB;
    fprintf(stderr, "ABTEST %s (%ld slices x %d frames, neighboring pairs ON-OFF):\n", g_abt_name, nb, g_abt_n);
    for (int q = 0; q < 6; q++){
        double sa[2] = {0, 0}, sd = 0, sdd = 0; int na[2] = {0, 0}, np = 0;
        for (long b = 0; b < nb; b++) if (g_abt_ok[b]){ sa[b & 1] += abt_val(b, q); na[b & 1]++; }
        for (long b = 0; b + 1 < nb; b += 2) if (g_abt_ok[b] && g_abt_ok[b + 1]){
            double d = abt_val(b, q) - abt_val(b + 1, q); sd += d; sdd += d * d; np++; }
        if (!na[0] || !na[1] || np < 2) continue;
        double m = sd / np, v = (sdd - np * m * m) / (np - 1), se = sqrt(v > 0 ? v / np : 0), base = sa[1] / na[1];
        fprintf(stderr, "ABTEST %s: %s  ON %9.3f  OFF %9.3f  diff %+8.3f +- %6.3f (%%%+.1f, %d pairs)\n",
                g_abt_name, qn[q], sa[0] / na[0], base, m, se, base > 0 ? m / base * 100 : 0, np);
    }
    unsigned long long c0 = atomic_load(&g_abt_cnt[0]), c1 = atomic_load(&g_abt_cnt[1]);
    if (c0 && c1)
        fprintf(stderr, "ABTEST %s: GPU time ON %.3f ms/frame, OFF %.3f ms/frame (DVFS: not a measure of work)\n",
                g_abt_name, (double)atomic_load(&g_abt_ns[0]) / c0 / 1e6, (double)atomic_load(&g_abt_ns[1]) / c1 / 1e6);
}
/* ---- STEP BY STEP MEASUREMENT (HD_RT_AB=1) ---------------------------------------
 * Splits the GPUWIN window into 100-frame slices; in each slice part of the ray
 * tracing pipeline is turned off and the GPU time per slice is reported.
 * The weather (rain) is random: instead of comparing separate runs, it gives the
 * cost of the steps in the SAME run, in the same scene.  The first 30 frames of a
 * slice are dropped (the previous mode's frames in the queue). */
#define AB_N 7
static const char *g_ab_name[AB_N] = {
    "0 full (ray tracing + composite)", "1 tracing on, no composite", "2 plain tracing (no shading), no composite",
    "3 capture + structure build", "4 capture only", "5 no ray tracing, SSR composite", "6 no ray tracing, no composite" };
static int g_ab_on = -1, g_ab_mode = -1;      /* g_ab_mode: this frame's mode (-1 = normal) */
static _Atomic unsigned long long g_ab_ns[AB_N], g_ab_cnt[AB_N];
static int ab_frame(long f, int *count_it){
    if (g_ab_on < 0){ const char *e = ENVC("HD_RT_AB"); g_ab_on = (e && *e == '1') ? 1 : 0; }
    *count_it = 0;
    if (!g_ab_on || !gpuw_in(f)) return -1;
    long r = f - g_gpuw_a;
    *count_it = (r % 100) >= 30;
    return (int)((r / 100) % AB_N);
}
/* ---- PASS PROFILE -------------------------------------------------------
 * PASSPROF=N: on every Nth frame in the GPUWIN window, measure the GPU time of
 * every pass (render / compute / blit / acceleration structure) with the Metal
 * counter sampler.  Answers "where does GPU time go" DIRECTLY instead of from
 * feature on/off differences.  For render passes the vertex and fragment stages
 * come separately (Apple GPUs are tile-based: all vertex work first, then the
 * fragment work).  Timestamps are in the GPU clock; converted to ns with two-point
 * sampleTimestamps. */
#include <mach/mach_time.h>
#define PP_MAXP 160
#define PP_MAXC 64
static id<MTLCounterSet> g_pp_set;
static int g_pp_every = -1;
static int g_pp_mode = 3;    /* PASSPROF_MODE: 1 = render passes only, 2 = compute/blit/acceleration only, 3 = all */
static pthread_mutex_t g_pp_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { char name[48]; double v, f; unsigned long long n; } g_pp_cat[PP_MAXC];
static int g_pp_ncat; static unsigned long long g_pp_frames;
static double g_pp_cb_s;                      /* the command buffer GPU time (s) of the same frames */
static MTLTimestamp g_pp_c0, g_pp_g0;
static id<MTLCounterSampleBuffer> g_pp_sb;    /* this frame (encoding thread only) */
static int g_pp_np; static uint8_t g_pp_pc[PP_MAXP], g_pp_pk[PP_MAXP];
static long g_pp_fno;                         /* the number of the profiled frame */
static FILE *g_pp_csv;                        /* PASSPROF_CSV=<path>: frame,pass,vertex_ms,fragment_ms */
static int pp_cat(const char *name){
    for (int i = 0; i < g_pp_ncat; i++) if (!strcmp(g_pp_cat[i].name, name)) return i;
    if (g_pp_ncat >= PP_MAXC) return PP_MAXC - 1;
    snprintf(g_pp_cat[g_pp_ncat].name, sizeof g_pp_cat[0].name, "%s", name);
    return g_pp_ncat++;
}
static void pp_begin(long fno){
    g_pp_sb = nil; g_pp_np = 0;
    if (g_pp_every < 0){
        const char *e = ENVC("PASSPROF"); g_pp_every = e ? atoi(e) : 0;
        { const char *m = ENVC("PASSPROF_MODE"); if (m && atoi(m) > 0) g_pp_mode = atoi(m); }
        if (g_pp_every > 0){
            if (![g_dev supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary]) g_pp_every = 0;
            for (id<MTLCounterSet> cs in g_dev.counterSets)
                if ([cs.name isEqualToString:MTLCommonCounterSetTimestamp]) g_pp_set = cs;
            if (!g_pp_set) g_pp_every = 0;
            if (!g_pp_every) fprintf(stderr, "[PASSPROF] no counter sampling on this GPU\n");
            else [g_dev sampleTimestamps:&g_pp_c0 gpuTimestamp:&g_pp_g0];
        }
    }
    if (g_pp_every <= 0 || !gpuw_in(fno) || fno % g_pp_every) return;
    g_pp_fno = fno;
    { static int o = 0; if (!o){ o = 1; const char *cp = ENVC("PASSPROF_CSV");
        if (cp && *cp){ g_pp_csv = fopen(cp, "w"); if (g_pp_csv) fprintf(g_pp_csv, "frame,pass,vertex_ms,fragment_ms\n"); } } }
    MTLCounterSampleBufferDescriptor *sd = [MTLCounterSampleBufferDescriptor new];
    sd.counterSet = g_pp_set; sd.storageMode = MTLStorageModeShared; sd.sampleCount = PP_MAXP * 4;
    NSError *er = nil;
    g_pp_sb = [g_dev newCounterSampleBufferWithDescriptor:sd error:&er];
}
static int pp_slot(const char *name, int kind){
    if (!g_pp_sb || g_pp_np >= PP_MAXP) return -1;
    if (!(g_pp_mode & (kind ? 2 : 1))) return -1;
    pthread_mutex_lock(&g_pp_lock); int c = pp_cat(name); pthread_mutex_unlock(&g_pp_lock);
    g_pp_pc[g_pp_np] = (uint8_t)c; g_pp_pk[g_pp_np] = (uint8_t)kind;
    return g_pp_np++;
}
static void pp_rp(MTLRenderPassDescriptor *rp, const char *name){
    int k = pp_slot(name, 0); if (k < 0) return;
    MTLRenderPassSampleBufferAttachmentDescriptor *a = rp.sampleBufferAttachments[0];
    a.sampleBuffer = g_pp_sb;
    a.startOfVertexSampleIndex = (NSUInteger)k * 4;     a.endOfVertexSampleIndex = (NSUInteger)k * 4 + 1;
    a.startOfFragmentSampleIndex = (NSUInteger)k * 4 + 2; a.endOfFragmentSampleIndex = (NSUInteger)k * 4 + 3;
}
static id<MTLBlitCommandEncoder> pp_blit(id<MTLCommandBuffer> cb, const char *name){
    int k = pp_slot(name, 1); if (k < 0) return [cb blitCommandEncoder];
    MTLBlitPassDescriptor *d = [MTLBlitPassDescriptor blitPassDescriptor];
    d.sampleBufferAttachments[0].sampleBuffer = g_pp_sb;
    d.sampleBufferAttachments[0].startOfEncoderSampleIndex = (NSUInteger)k * 4;
    d.sampleBufferAttachments[0].endOfEncoderSampleIndex = (NSUInteger)k * 4 + 1;
    return [cb blitCommandEncoderWithDescriptor:d];
}
static id<MTLComputeCommandEncoder> pp_compute(id<MTLCommandBuffer> cb, const char *name){
    int k = pp_slot(name, 1); if (k < 0) return [cb computeCommandEncoder];
    MTLComputePassDescriptor *d = [MTLComputePassDescriptor computePassDescriptor];
    d.sampleBufferAttachments[0].sampleBuffer = g_pp_sb;
    d.sampleBufferAttachments[0].startOfEncoderSampleIndex = (NSUInteger)k * 4;
    d.sampleBufferAttachments[0].endOfEncoderSampleIndex = (NSUInteger)k * 4 + 1;
    return [cb computeCommandEncoderWithDescriptor:d];
}
static id<MTLAccelerationStructureCommandEncoder> pp_accel(id<MTLCommandBuffer> cb, const char *name){
    if (@available(macOS 13.0, *)){
        int k = pp_slot(name, 1);
        if (k >= 0){
            MTLAccelerationStructurePassDescriptor *d = [MTLAccelerationStructurePassDescriptor accelerationStructurePassDescriptor];
            d.sampleBufferAttachments[0].sampleBuffer = g_pp_sb;
            d.sampleBufferAttachments[0].startOfEncoderSampleIndex = (NSUInteger)k * 4;
            d.sampleBufferAttachments[0].endOfEncoderSampleIndex = (NSUInteger)k * 4 + 1;
            return [cb accelerationStructureCommandEncoderWithDescriptor:d];
        }
    }
    return [cb accelerationStructureCommandEncoder];
}
static void pp_end(id<MTLCommandBuffer> cb){
    if (!g_pp_sb || !g_pp_np) { g_pp_sb = nil; return; }
    id<MTLCounterSampleBuffer> sb = g_pp_sb; int np = g_pp_np;
    { static int once = 0;            /* the pass ORDER of the first profiled frame */
      if (!once){ once = 1; fprintf(stderr, "  --- pass order (first profiled frame) ---\n");
          for (int k = 0; k < np; k++) fprintf(stderr, "  [order %2d] %s\n", k, g_pp_cat[g_pp_pc[k]].name); } }
    uint8_t *pc = malloc((size_t)np * 2); memcpy(pc, g_pp_pc, (size_t)np); memcpy(pc + np, g_pp_pk, (size_t)np);
    long pfno = g_pp_fno;
    [cb addCompletedHandler:^(id<MTLCommandBuffer> b2){
        NSData *dt = [sb resolveCounterRange:NSMakeRange(0, (NSUInteger)np * 4)];
        if (dt && dt.length >= (NSUInteger)np * 4 * sizeof(MTLCounterResultTimestamp)){
            const MTLCounterResultTimestamp *t = (const MTLCounterResultTimestamp *)dt.bytes;
            pthread_mutex_lock(&g_pp_lock);
            for (int k = 0; k < np; k++){
                uint64_t a = t[k*4].timestamp, b = t[k*4+1].timestamp, c = t[k*4+2].timestamp, d2 = t[k*4+3].timestamp;
                int ci = pc[k];
                double kv = 0, kf = 0;
                if (a != MTLCounterErrorValue && b != MTLCounterErrorValue && b >= a){ kv = (double)(b - a); g_pp_cat[ci].v += kv; }
                if (!pc[np + k] && c != MTLCounterErrorValue && d2 != MTLCounterErrorValue && d2 >= c){ kf = (double)(d2 - c); g_pp_cat[ci].f += kf; }
                g_pp_cat[ci].n++;
                if (g_pp_csv) fprintf(g_pp_csv, "%ld,%s,%.4f,%.4f\n", pfno, g_pp_cat[ci].name, kv / 1e6, kf / 1e6);   /* ~ns (scale 1.0) */
            }
            g_pp_frames++; g_pp_cb_s += b2.GPUEndTime - b2.GPUStartTime;
            pthread_mutex_unlock(&g_pp_lock);
        }
        free(pc);
    }];
    g_pp_sb = nil;
}
static void pp_report(void){
    if (g_pp_every <= 0 || !g_pp_frames) return;
    MTLTimestamp c1, g1; [g_dev sampleTimestamps:&c1 gpuTimestamp:&g1];
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    double sc = (g1 > g_pp_g0) ? ((double)(c1 - g_pp_c0) * tb.numer / tb.denom) / (double)(g1 - g_pp_g0) : 1.0;   /* ns / GPU tick */
    double tot = 0; int ord[PP_MAXC];
    for (int i = 0; i < g_pp_ncat; i++){ ord[i] = i; tot += g_pp_cat[i].v + g_pp_cat[i].f; }
    /* On the Apple GPU counter samples come directly in ns (sampleTimestamps however
     * are mach ticks): if the pass total exceeds the command buffer time, the scale is 1. */
    if (tot * sc > g_pp_cb_s * 1e9 * 1.5) sc = 1.0;
    for (int i = 0; i < g_pp_ncat; i++) for (int j = i + 1; j < g_pp_ncat; j++)
        if (g_pp_cat[ord[j]].v + g_pp_cat[ord[j]].f > g_pp_cat[ord[i]].v + g_pp_cat[ord[i]].f){ int t = ord[i]; ord[i] = ord[j]; ord[j] = t; }
    double fr = (double)g_pp_frames;
    fprintf(stderr, "  --- pass profile: %llu frames, pass total %.3f ms/frame, command buffer %.3f ms/frame (scale %.4f ns/tick) ---\n",
            g_pp_frames, tot * sc / fr / 1e6, g_pp_cb_s / fr * 1e3, sc);
    fprintf(stderr, "  %-44s %8s %8s %8s %7s\n", "pass", "ms/frame", "vertex", "fragment", "adet/k");
    for (int q = 0; q < g_pp_ncat; q++){ int i = ord[q];
        fprintf(stderr, "  %-44s %8.3f %8.3f %8.3f %7.2f\n", g_pp_cat[i].name,
                (g_pp_cat[i].v + g_pp_cat[i].f) * sc / fr / 1e6, g_pp_cat[i].v * sc / fr / 1e6,
                g_pp_cat[i].f * sc / fr / 1e6, (double)g_pp_cat[i].n / fr); }
}
static _Atomic unsigned long long g_pass_n = 0, g_draw_n = 0;
/* Render target = texture id | ((face+1) << 16).  For a cube texture the game
 * draws to the SIX FACES of the same texture separately; without the face all six
 * went to the same surface on top of each other. */
#define TGT_ID(t)   ((t) & 0xFFFFu)
#define TGT_FACE(t) (((t) >> 16) & 7u)          /* 0 = no face */
static unsigned g_cur_target = 0;          /* the guest's active render target */
static unsigned g_cur_blend  = 0;
static unsigned g_cur_bcol   = 0xFFFFFFFFu;   /* D3DRS_BLENDFACTOR (ARGB) */
static unsigned g_cur_cull   = 3u;            /* D3DRS_CULLMODE: 1=NONE 2=CW 3=CCW */
static unsigned g_cur_depth  = 1u << 8; /* bit0=test, bit1=write, bit2-4=function,
                                          * bit8+ = the bound Z surface (DEPTH_ZID) */
static unsigned g_cur_dmode  = 0;
static unsigned g_cur_zid    = 1;       /* 1 = the device's implicit Z surface */
static unsigned g_cur_tex1 = 0, g_cur_tex2 = 0, g_cur_tex3 = 0;
static int      g_cur_ps = 0;              /* active pixel shader */
static float    g_cur_psc[PSC_N][4];       /* ps constants c0..c(PSC_N-1) */
/* A per-frame ps constant pool.  Every draw record used to carry all 32
 * constants (512 bytes): a copy + zeroing + another copy and compare in encoding,
 * ~2 KB per draw.  Measured: 92% of draws use the SAME constants as the previous
 * one (the game writes at most c3).  Now constants are only added to the pool when
 * they change, and the record carries the sequence number. */
static float    (*g_pscb[2])[PSC_N][4];
static unsigned g_pscn[2], g_psc_last[2], g_psc_lastver[2];
static unsigned g_psc_ver = 1;              /* increments every time g_cur_psc changes */
static unsigned psc_put(int b){              /* under g_qlock */
    if (g_pscn[b] && g_psc_lastver[b] == g_psc_ver) return g_psc_last[b];
    if (g_pscn[b] >= MAXD) return g_psc_last[b];
    memcpy(g_pscb[b][g_pscn[b]], g_cur_psc, sizeof g_cur_psc);
    g_psc_last[b] = g_pscn[b]++; g_psc_lastver[b] = g_psc_ver;
    return g_psc_last[b];
}
/* Projection mode per stage: 0 = none, 3 = xy/z, 4 = xy/w.
 * D3DTSS_TEXTURETRANSFORMFLAGS | D3DTTFF_PROJECTED. */
static float    g_cur_proj[4];
static unsigned g_cur_smp[4] = {0,0,0,0};
/* The game's D3DSAMP_* state.  Previously there was a SINGLE global sampler
 * (linear/linear/mip-linear, no anisotropy).  The game actually asks for
 * MINFILTER=ANISOTROPIC + MAXANISOTROPY=16; with isotropic trilinear the derivative
 * estimate jumps at triangle EDGES, a mip one level too coarse is selected, and a
 * 1-pixel line forms along the edges + diagonal of every quad (most visible over
 * letters).                                                             */
void metal_set_samplers(const unsigned k[4]){
    for (int i = 0; i < 4; i++) g_cur_smp[i] = k[i];
}
#if NFS_ENHANCED
#define MAXPS 2560          /* + reflection variants: ps + PSREFL*(1+stage) */
#else
#define MAXPS 512
#endif
#define PSCUBE 256          /* cube variant: psid + PSCUBE */
#define PSREFL 512          /* (HD) reflection separation variant */
static id<MTLFunction> g_psfn[MAXPS];      /* ps compiled from the translation (with alpha test, kAT=1) */
static id<MTLFunction> g_psfn_nat[MAXPS];  /* the same ps, without alpha test (kAT=0; see ALPHA TEST VARIANT) */
/* ASYNC PIXEL SHADER COMPILES.  The game creates all its pixel shaders while it
 * boots; each was compiled from MSL source on the spot, one after the other.  With
 * a cold Metal cache (the first start of a new build) that is ~8 s of black screen
 * in the HD build (more variants per shader), ~1 s in the normal one.  Now each
 * compile is started in the background (up to 8 at a time: the Metal compiler runs
 * them in parallel) and a draw waits only for the shader it actually needs.
 * PSASYNC=0: compile on the spot, as before. */
/* g_ps_pend: 0 done/none, 1 queued (not started), 2 compiling.  A draw that needs a
 * shader still queued compiles it itself right away (jumps the queue): the first
 * frames need a handful of the ~280 shaders the game creates at boot. */
static _Atomic unsigned char g_ps_pend[MAXPS];
static pthread_mutex_t g_ps_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_ps_cv = PTHREAD_COND_INITIALIZER;
static void (^g_ps_job[MAXPS])(void);
static void ps_run(int psid){
    unsigned char want = 1;
    if (!atomic_compare_exchange_strong(&g_ps_pend[psid], &want, 2)) return;   /* someone else has it */
    void (^job)(void) = g_ps_job[psid];
    if (job) job();
}
static void ps_wait(int psid){
    if (psid <= 0 || psid >= MAXPS || !atomic_load(&g_ps_pend[psid])) return;
    ps_run(psid);
    pthread_mutex_lock(&g_ps_mx);
    while (atomic_load(&g_ps_pend[psid])) pthread_cond_wait(&g_ps_cv, &g_ps_mx);
    pthread_mutex_unlock(&g_ps_mx);
}
static id<MTLFunction> g_fmain_at, g_fmain_nat;   /* fixed-function fragment shader */
/* ---- ALPHA TEST VARIANT ----------------------------------------------------
 * Every translated fragment shader did the alpha test itself:
 * "if (res.a < P.aref) discard_fragment()".  In a draw with the test off (aref 0)
 * no fragment is discarded, but because the shader HAS a discard the Apple GPU
 * doesn't treat the draw as "opaque": hidden surface removal (HSR) is disabled and
 * invisible fragments get shaded too.  Every ps is specialized twice with a
 * function constant (kAT); a draw with aref 0 is drawn with the test-free variant.
 * A difference can only arise for a fragment producing res.a < 0: in D3D9 that
 * fragment is written with the alpha test off (clamped to 0 in UNORM), so the
 * test-free variant is more faithful to D3D9.  ALFAVAR=0 is the old behavior;
 * ALFAVAR=2 diagnostic: a fragment that would be discarded in the test-free variant
 * is magenta (0 magenta = exactly the same); ALFAVAR=3 tests the diagnostic itself:
 * alpha-tested draws are also drawn with the diagnostic variant, trees / wire
 * fences MUST be magenta. */
static int alfavar_mode(void){
    /* In HD the ALFAVAR=2 counter first found a 12.6 M fragment difference: negative
     * vertex color alpha was arriving at the fixed-function shader.  D3D9 clamps the
     * color input to [0,1] (see COLOR CLAMPING); with clamping, 0 in both builds. */
    static int m = -1; if (m < 0){ const char *e = ENVC("ALFAVAR"); m = (e && *e) ? atoi(e) : 1; }
    return m;
}
static id<MTLBuffer> g_atdbg;                 /* ALFAVAR>=2: per-ps counter of fragments to discard (frag. buffer 6; 0 = fmain) */
/* ---- COLOR CLAMPING ----------------------------------------------------------
 * In D3D9, ps_1_1 - ps_2_0 (and the fixed-function pipeline) see the vertex color
 * clamped to [0,1] (v0/v1: "8 bit unsigned, (0, +1)"; "saturated before use by the
 * shader").  Our vertex shader passed oD0/oD1 through without clamping (measured
 * 2026-10-19: 1.18 billion fragments with colors outside [0,1] in race + menu,
 * mostly car paint ps 265).  Clamping is done in two places: per vertex (vsemu.c
 * vs_to_msl and the com.c CPU path; the hardware color interpolators carried
 * [0,1]) and per fragment (kCS; for floating-point overflows and missed paths).
 * In the Windows reference the menu frames didn't change (difference 0.1 - 0.5,
 * the same as before).  RENKSAT=0 is the old behavior. */
static int renksat_on(void){
    static int on = -1; if (on < 0){ const char *e = ENVC("RENKSAT"); on = (e && *e == '0') ? 0 : 1; }
    return on;
}
static id<MTLFunction> fn_spec(id<MTLLibrary> lib, NSString *name, int at){
    MTLFunctionConstantValues *cv = [MTLFunctionConstantValues new];
    bool dbg = (at == 2), cs = renksat_on() != 0;
    [cv setConstantValue:&at type:MTLDataTypeInt atIndex:0];
    [cv setConstantValue:&dbg type:MTLDataTypeBool atIndex:1];
    [cv setConstantValue:&cs type:MTLDataTypeBool atIndex:2];
    NSError *e = nil;
    id<MTLFunction> f = [lib newFunctionWithName:name constantValues:cv error:&e];
    if (!f) fprintf(stderr, "metal: %s could not be specialized (kAT=%d): %s\n", [name UTF8String], at,
                    e ? [[e localizedDescription] UTF8String] : "?");
    return f;
}
static id<MTLTexture>        g_depth_tex;  /* scratch used when there is NO Z surface */
/* --- Z surfaces ------------------------------------------------------
 * There used to be a single Z texture, RECREATED every time the target size
 * changed.  Within one frame the game goes back and forth between 320x240
 * (light/glow), 6 x 128x128 (reflection cube), screen and 640x480 (blur) targets;
 * on every return to the screen Z was loaded from a new (undefined) texture ->
 * city lights and some cars were visible through walls.  Now it is like D3D: every
 * Z surface has its own texture, SetDepthStencilSurface decides which one is
 * bound and Clear(ZBUFFER) when it is cleared; the content is preserved across
 * frames. */
#define MAXZ 64
#define DEPTH_ZID(d) ((d) >> 8)
static id<MTLTexture> g_ztex[MAXZ];
static unsigned char  g_zpend[MAXZ];        /* clear in the next pass */
static float          g_zpend_v[MAXZ];
static _Atomic unsigned long long g_z_mismatch = 0, g_z_null = 0;
static _Atomic unsigned char g_zforget[MAXZ];  /* the guest recreated it: drop the texture */
void metal_z_forget(unsigned zid){ if (zid < MAXZ) atomic_store(&g_zforget[zid], 1); }
static id<MTLDepthStencilState> g_dss[64];
static float    g_cur_aref   = 0.f;
/* D3DRS_FOGENABLE(28) + D3DRS_FOGCOLOR(34).  Fog was NEVER applied: the vertex
 * shader's oFog output went to "dummy" in MSL.  6 of the game's 18 vertex shaders
 * write oFog and FOGENABLE is set 5.3M times -> distant geometry never faded.
 * (r,g,b,on) */
static float    g_cur_fog[4] = {0.f,0.f,0.f,0.f};
static int      g_cur = 0;                 /* the buffer the guest writes */
static int      g_ready = -1;              /* the buffer the main thread will draw */
static pthread_mutex_t g_qlock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_qcond = PTHREAD_COND_INITIALIZER;
/* g_qcond: main thread -> guest ("I took your frame").
 * g_rcond: guest -> main thread ("frame ready").  The main thread used to POLL
 * with a 4 ms nanosleep; the frame time histogram was quantized in exactly 5 ms
 * steps (15.5/20.5/25.5/30.5 ms, EMPTY in between).  After the guest finished a
 * frame it waited idly 0-5 ms until the main thread picked it up.  Now the wake-up
 * is immediate. */
static pthread_cond_t  g_rcond = PTHREAD_COND_INITIALIZER;
static _Atomic long    g_frames_dropped = 0;
/* The buffer the main thread is ENCODING right now (-1 = none).  Even if
 * metal_present times out it CAN'T swap into this buffer: the main thread walks
 * g_dbuf[rb] without a lock; if the guest wrote the new frame's draws into the
 * same buffer, the frame would be left half done or the new draws drawn with old
 * vertex data. */
static int g_enc_rb = -1;
/* OFF-SCREEN FLUSH (see READING BACK A LOCKABLE RENDER TARGET): buffer b is drawn
 * into g_flush_tid's texture instead of the screen and not presented. */
static int g_noshow[2], g_noshow_next;
static unsigned g_flush_tid;
static long g_flush_req, g_flush_done;     /* under g_qlock; done is signalled on g_qcond */
static _Atomic unsigned long g_cb_err = 0;         /* frames that ended with an error on the GPU */
static _Atomic unsigned long g_enc_late = 0;       /* encoding was still going when the 60 ms ran out */
static long g_enc_late_f[8];                       /* frame numbers of the first few */

static id<MTLRenderPipelineState> g_pso;
static id<MTLLibrary>             g_lib;
static MTLVertexDescriptor       *g_vdesc;
static id<MTLBuffer>              g_mtlvb;
/* --- per-frame buffer ring ----------------------------------------------
 * g_mtlvb / g_mtlrvb / g_mtlkb used to be SINGLE: every frame's vertex and constant
 * data was copied into the same shared buffer.  [cb commit] DOESN'T WAIT for the
 * GPU; if the next frame's memcpy came before the GPU ran the previous frame's
 * vertex stage, that frame's draws (first/rawoff/koff relative to the OLD layout)
 * read the NEW frame's bytes.  The screen-space work drawn at the end of the frame
 * (post-process + HUD) is the first victim: giant triangles covering the screen,
 * the font atlas on top; the world intact (user photos, 2026-10-13).  Measured: in
 * a race the buffer was overwritten before the previous frame finished on the GPU
 * in 6472 of 9000 frames; in 1046 the layout had shifted too.
 * Now every frame takes a set whose GPU work is FINISHED; the completion handler
 * gives it back.  Correctness depends on the semaphore, not the NUMBER of sets: if
 * all are in flight, fb_acquire waits for the GPU.  Three sets are based on
 * measurement: 8000 frames in a race, two runs, 0 waits (2+ frames in flight at
 * once: 6/9000 in a visible window, 2292/7600 in a transparent test window -- the
 * third set covers that).  The only exception is while the machine sleeps (lid
 * closed, DarkWake): since the GPU stops, 3138 waits were seen in 7600 frames; the
 * timing of those runs is invalid. */
#define NFB 3
static id<MTLBuffer>         g_fb_vb[NFB], g_fb_rvb[NFB], g_fb_kb[NFB], g_fb_up[NFB];
static _Atomic int           g_fb_busy[NFB];
static dispatch_semaphore_t  g_fb_sem;
static _Atomic unsigned long g_fb_overlap = 0, g_fb_wait = 0;
static _Atomic unsigned long long g_fb_wait_ns = 0;
static int fb_acquire(void){
    if (dispatch_semaphore_wait(g_fb_sem, DISPATCH_TIME_NOW) != 0){
        struct timespec w0, w1; clock_gettime(CLOCK_MONOTONIC, &w0);
        atomic_fetch_add(&g_fb_wait, 1ul);          /* all on the GPU: wait until one finishes */
        dispatch_semaphore_wait(g_fb_sem, DISPATCH_TIME_FOREVER);
        clock_gettime(CLOCK_MONOTONIC, &w1);
        atomic_fetch_add(&g_fb_wait_ns, (unsigned long long)(w1.tv_sec - w0.tv_sec) * 1000000000ull
                         + (unsigned long long)w1.tv_nsec - (unsigned long long)w0.tv_nsec);
    }
    int got = 0, any = 0;
    for (int i = 0; i < NFB; i++){
        if (!got){ int z = 0;
            if (atomic_compare_exchange_strong(&g_fb_busy[i], &z, 1)){ got = i + 1; continue; } }
        if (atomic_load(&g_fb_busy[i])) any = 1;
    }
    if (any) atomic_fetch_add(&g_fb_overlap, 1ul);  /* the buffer used to be overwritten in this frame */
    return got - 1;                                 /* the semaphore guarantees at least one free set */
}
static void fb_release(int i){
    atomic_store(&g_fb_busy[i], 0);
    dispatch_semaphore_signal(g_fb_sem);
}
/* The set buffers are NOT a fixed MAXV/MAXRV/MAXKB size; they grow with the
 * frame's needs.  Once a shared buffer has been used on the GPU, ALL of it counts
 * toward the footprint (measured: +85 MB while only 64 KB of 3 x 28 MB buffers was
 * filled); three fixed-size sets held 147 MB, whereas the CPU vertex path carries
 * ~400 vertices (45 KB) per frame in a race.  Since the set comes FREE from
 * fb_acquire, the old buffer isn't in any command buffer; replacing it is safe. */
#define FB_STEP (256u * 1024u)
static _Atomic unsigned long g_fb_grow = 0;
static id<MTLBuffer> fb_fit(id<MTLBuffer> b, NSUInteger need, NSUInteger maxlen){
    if (b && b.length >= need) return b;
    NSUInteger cap = need + need / 4;               /* margin: don't recreate every frame */
    cap = (cap + FB_STEP - 1) / FB_STEP * FB_STEP;
    if (cap < FB_STEP) cap = FB_STEP;
    if (cap > maxlen) cap = maxlen;
    id<MTLBuffer> nb = [g_dev newBufferWithLength:cap options:MTLResourceStorageModeShared];
    if (!nb) return b;                              /* fb_put writes as much as fits into the old one */
    if (b) atomic_fetch_add(&g_fb_grow, 1ul);
    return nb;
}
static void fb_put(id<MTLBuffer> b, const void *src, NSUInteger n){
    if (b && n) memcpy([b contents], src, n < b.length ? n : b.length);
}
/* --- PERSISTENT VERTEX / INDEX POOL -------------------------------------------
 * The game's static (never locked) vertex and index buffers are mirrored on the
 * GPU ONCE; indexed draws are drawn from there with drawIndexedPrimitives.
 * Previously, every frame the indices were expanded on the guest side (an
 * intermediate array), copied into the arena, then into the Metal buffer:
 * 3 x 2.7 MB/frame, ~1 ms of CPU; since vertices multiplied x2.1, the GPU vertex
 * stage (~19% of pass time in 4K HD) also ran twice.  Measured (2026-10-14): 99.6%
 * of source VBs haven't been locked for 120+ frames.
 * The pool is a SINGLE buffer: so the HD capture kernel can read all records with a
 * single binding.  Mirrors are only built/dropped on the guest thread; the main
 * thread binds the pool object as of the frame's submission (growing copies the
 * old one and switches to the new, offsets don't change).  Freed blocks are reused
 * PF_DELAY frames later so frames in flight can finish. */
#define MIR_N     4096u             /* mirror table (open addressing, 2^n) */
#define MIR_TOMB  1u                /* deleted slot (a guest address is never 1) */
#define PF_N      4096
#define PF_DELAY  8u                /* frame */
#define MIR_IDLE  900u              /* a mirror not drawn for this many frames is dropped */
#define POOL_MAX  (768u << 20)
static id<MTLBuffer> g_pool;        /* current pool (changed under g_qlock) */
static id<MTLBuffer> g_pool_fr[2];  /* the pool as of the frame's submission */
static uint32_t g_pool_top = 0;
static struct { uint32_t key, gen, off, size, imax, used; } g_mir[MIR_N];
static struct { uint32_t off, size, at; } g_pf[PF_N];
static int g_pfn = 0;
static uint32_t g_gframe = 0;       /* guest frame counter (metal_present) */
static unsigned long g_mir_build = 0, g_mir_drop = 0, g_mir_live = 0, g_mir_full = 0;
static unsigned long long g_mir_bytes = 0;
static int pool_reserve(uint32_t top){
    NSUInteger cap = g_pool ? g_pool.length : 0;
    if (top <= cap) return 1;
    NSUInteger nc = cap ? cap : (4u << 20);
    while (nc < top) nc += nc / 2;
    if (nc > POOL_MAX) return 0;
    id<MTLBuffer> nb = [g_dev newBufferWithLength:nc options:MTLResourceStorageModeShared];
    if (!nb) return 0;
    if (g_pool && g_pool_top) memcpy([nb contents], [g_pool contents], g_pool_top);
    pthread_mutex_lock(&g_qlock); g_pool = nb; pthread_mutex_unlock(&g_qlock);
    return 1;
}
static int pool_alloc(uint32_t want, uint32_t *off, uint32_t *size){
    want = (want + 255u) & ~255u;
    for (int i = 0; i < g_pfn; i++){
        if (g_pf[i].size < want || g_gframe - g_pf[i].at < PF_DELAY) continue;
        *off = g_pf[i].off;
        if (g_pf[i].size - want >= 4096u){ *size = want; g_pf[i].off += want; g_pf[i].size -= want; }
        else { *size = g_pf[i].size; g_pf[i] = g_pf[--g_pfn]; }
        return 1;
    }
    if (!pool_reserve(g_pool_top + want)) return 0;
    *off = g_pool_top; *size = want; g_pool_top += want;
    return 1;
}
static void pool_free(uint32_t off, uint32_t size){
    if (!size) return;
    if (g_pfn < PF_N){ g_pf[g_pfn].off = off; g_pf[g_pfn].size = size; g_pf[g_pfn].at = g_gframe; g_pfn++; }
}
static void mir_sweep(void){                 /* drop mirrors not drawn for a long time */
    for (unsigned i = 0; i < MIR_N; i++){
        if (g_mir[i].key <= MIR_TOMB || g_gframe - g_mir[i].used < MIR_IDLE) continue;
        pool_free(g_mir[i].off, g_mir[i].size);
        g_mir[i].key = MIR_TOMB; g_mir_drop++; g_mir_live--;
    }
}
/* Guest: the offset in the pool of this generation (gen) of the guest object (key);
 * built now if missing or stale.  If isz > 0 it is an index buffer and *imax
 * becomes the largest index in the whole buffer.  0 = couldn't be built (the caller
 * falls back to the old path). */
int metal_mirror(uint32_t key, uint32_t gen, const void *src, uint32_t size, unsigned isz,
                 uint32_t *off, uint32_t *imax){
    if (!g_dev || key <= MIR_TOMB || !size || size > (64u << 20)) return 0;
    { static uint32_t last = 0; if (g_gframe - last >= 256u){ last = g_gframe; mir_sweep(); } }
    unsigned h = (key * 2654435761u) & (MIR_N - 1u), slot = MIR_N, q;
    for (q = 0; q < 64u; q++){
        unsigned i = (h + q) & (MIR_N - 1u);
        if (g_mir[i].key == key){ slot = i; break; }
        if (g_mir[i].key <= MIR_TOMB && slot == MIR_N) slot = i;
        if (!g_mir[i].key) break;
    }
    if (slot == MIR_N){ g_mir_full++; return 0; }
    if (g_mir[slot].key == key && g_mir[slot].gen == gen && g_mir[slot].size >= size){
        g_mir[slot].used = g_gframe;
        *off = g_mir[slot].off; if (imax) *imax = g_mir[slot].imax;
        return 1;
    }
    if (g_mir[slot].key == key){ pool_free(g_mir[slot].off, g_mir[slot].size); g_mir[slot].key = MIR_TOMB; g_mir_live--; }
    uint32_t o, sz;
    if (!pool_alloc(size, &o, &sz)) return 0;
    uint8_t *dst = (uint8_t *)[g_pool contents] + o;
    memcpy(dst, src, size);
    uint32_t mx = 0;
    if (isz == 2u){ const uint16_t *ix = (const uint16_t *)dst; for (uint32_t k = 0; k < size / 2u; k++) if (ix[k] > mx) mx = ix[k]; }
    else if (isz == 4u){ const uint32_t *ix = (const uint32_t *)dst; for (uint32_t k = 0; k < size / 4u; k++) if (ix[k] > mx) mx = ix[k]; }
    g_mir[slot].key = key; g_mir[slot].gen = gen; g_mir[slot].off = o; g_mir[slot].size = sz;
    g_mir[slot].imax = mx; g_mir[slot].used = g_gframe;
    g_mir_build++; g_mir_live++; g_mir_bytes += size;
    *off = o; if (imax) *imax = mx;
    return 1;
}
/* Guest: the CPU view of the mirror in the pool (for scanning the index range). */
const uint8_t *metal_pool_cpu(void){ return g_pool ? (const uint8_t *)[g_pool contents] : NULL; }
/* Main thread: the raw bytes of the draw's j-th vertex (HD ray tracing CPU reads). */
static const uint8_t *dr_vtx(int rb, const MDraw *dr, unsigned j, unsigned stride){
    if (!dr->ix_isz) return g_rvbuf[rb] + dr->rawoff + (size_t)j * stride;
    const uint8_t *P = (const uint8_t *)[g_pool_fr[rb] contents];
    uint32_t ix = dr->ix_isz == 4u ? ((const uint32_t *)(P + dr->ix_ib))[j]
                                   : ((const uint16_t *)(P + dr->ix_ib))[j];
    return P + dr->rawoff + (size_t)((int64_t)ix + dr->ix_base) * stride;
}
static id<MTLBuffer>              g_blitvb;   /* fullscreen quad for StretchRect */
static id<MTLSamplerState>        g_samp;

/* --- texture table --- */
#define MAXTEX 65536   /* ids are 16 bits (TGT_ID).  Ids are never reused and a race creates ~3000
                        * textures: 16384 ran out after ~30 min and every later texture was white. */
static uint8_t g_tex_cube[MAXTEX];          /* 1 = MTLTextureTypeCube */
static unsigned g_tex_lvlmask[MAXTEX];
static id<MTLTexture> g_tex[MAXTEX];
static unsigned       g_texn = 1;            /* 0 = "no texture" (white) */
/* --- releasing render targets on Reset --------------------------------
 * In D3D9 the game must release ALL D3DPOOL_DEFAULT resources (render targets
 * included) before Reset, otherwise Reset fails.  We never released the Metal
 * texture on Release: every resolution change leaked ~6 render targets (>100 MB
 * per change at 4K).  At Reset time, the render targets created so far are
 * considered dead and are released on the main thread once the frame(s) on
 * screen are done. */
static uint8_t g_tex_isrt[MAXTEX];
static _Atomic unsigned g_rtfree_upto = 0;    /* RTs with ids SMALLER than this are dead */
static unsigned         g_rtfree_done = 1;    /* released up to here */
static _Atomic long     g_rtfree_at = 0;      /* release after this frame */
static unsigned long    g_rt_freed = 0;
static uint8_t          g_tex_freed[MAXTEX];      /* diagnostic: released */
static unsigned short   g_freed_w[MAXTEX], g_freed_h[MAXTEX];
static unsigned long    g_freed_use_smp = 0, g_freed_use_tgt = 0;
static void freed_use(unsigned id_, int as_tgt){
    if (id_ >= MAXTEX || !g_tex_freed[id_]) return;
    if (as_tgt) g_freed_use_tgt++; else g_freed_use_smp++;
    static int shown = 0;
    if (shown < 12){ shown++;
        fprintf(stderr, "metal: WARNING released RT used as %s: id=%u %ux%u%s\n",
                as_tgt ? "TARGET" : "texture", id_, g_freed_w[id_], g_freed_h[id_],
                g_tex_cube[id_] ? " (cube)" : ""); }
}
static id<MTLTexture> g_white;

/* D3DFMT -> MTLPixelFormat; returns the block size and bytes per block */
static MTLPixelFormat d3dfmt_to_mtl(unsigned f, unsigned *blk, unsigned *bpb){
    switch (f){
    case 21: case 22: *blk=1; *bpb=4; return MTLPixelFormatBGRA8Unorm;   /* A8R8G8B8 */
    case 28: *blk=1; *bpb=1; return MTLPixelFormatA8Unorm;               /* A8 */
    case 41: *blk=1; *bpb=1; return MTLPixelFormatR8Unorm;               /* L8 */
    case 0x31545844: *blk=4; *bpb=8;  return MTLPixelFormatBC1_RGBA;     /* DXT1 */
    case 0x33545844: *blk=4; *bpb=16; return MTLPixelFormatBC2_RGBA;     /* DXT3 */
    case 0x35545844: *blk=4; *bpb=16; return MTLPixelFormatBC3_RGBA;     /* DXT5 */
    default: *blk=0; *bpb=0; return MTLPixelFormatInvalid;
    }
}

/* Ids are never reused (the COM objects have no refcount): when the table is full every
 * later texture is "no texture" = white.  Say so once instead of failing silently. */
static unsigned long g_tex_full_hits;
static unsigned g_texmax = MAXTEX;       /* NFSU2_TEXMAX=<n>: test the table-full path with a small table */
static unsigned tex_limit(void){
    static int init;
    if (!init){ init = 1; const char *e = getenv("NFSU2_TEXMAX");
                if (e && atoi(e) > 1 && (unsigned)atoi(e) < MAXTEX) g_texmax = (unsigned)atoi(e); }
    return g_texmax;
}
static void tex_table_full(void){
    if (!g_tex_full_hits++)
        fprintf(stderr, "metal: WARNING texture table full (%u ids): new textures will be white\n", g_texmax);
}
unsigned metal_create_texture_ex(unsigned w, unsigned h, unsigned levels,
                                 unsigned d3dfmt, unsigned is_rt);
/* New texture content is UNDEFINED: ALL mip levels and (for cubes) ALL faces are
 * cleared to transparent black.  Previously only level 0 of 2D was cleared, cubes
 * never.  In the main menu the car environment map (256/128 cube RT) is NEVER drawn
 * but the body samples it (ps 265, stage 0): when Reset releases the targets, Metal
 * can give that memory to new textures and the cube carried the old texture's
 * garbage (verified with deliberate garbage: noise on the body). */
static void tex_zero_all(id<MTLTexture> t, unsigned w, unsigned h, unsigned levels,
                         unsigned faces, unsigned blk, unsigned bpb){
    if (ENVC("NOTEXZERO")) { if (faces == 1) levels = 1; else return; }  /* diagnostic: old behavior */
    for (unsigned lv = 0; lv < levels; lv++){
        unsigned lw = w >> lv, lh = h >> lv;
        if (!lw) lw = 1; if (!lh) lh = 1;
        size_t row, rows;
        if (!blk || blk == 1){ row = (size_t)lw * (bpb ? bpb : 4); rows = lh; }
        else { row = (size_t)((lw + blk - 1) / blk) * bpb; rows = (lh + blk - 1) / blk; }
        void *zero = calloc(1, row * rows);
        if (!zero) return;
        for (unsigned f = 0; f < faces; f++)
            [t replaceRegion:MTLRegionMake2D(0,0,lw,lh) mipmapLevel:lv slice:f
                   withBytes:zero bytesPerRow:row bytesPerImage:0];
        free(zero);
    }
}
unsigned metal_create_texture(unsigned w, unsigned h, unsigned levels, unsigned d3dfmt){
    return metal_create_texture_ex(w, h, levels, d3dfmt, 0);
}
unsigned metal_create_texture_ex(unsigned w, unsigned h, unsigned levels,
                                 unsigned d3dfmt, unsigned is_rt){
    if (!g_dev) return 0;
    if (g_texn >= tex_limit()){ tex_table_full(); return 0; }
    unsigned blk, bpb;
    MTLPixelFormat pf = d3dfmt_to_mtl(d3dfmt, &blk, &bpb);
    if (pf == MTLPixelFormatInvalid) return 0;
    if (!levels) levels = 1;
    MTLTextureDescriptor *td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf
                              width:w height:h mipmapped:(levels > 1)];
    td.mipmapLevelCount = levels;
    td.usage = MTLTextureUsageShaderRead | (is_rt ? MTLTextureUsageRenderTarget : 0);
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> t = [g_dev newTextureWithDescriptor:td];
    if (!t) return 0;
    /* New texture content is undefined; clear to transparent black so textures that
     * weren't uploaded don't show up as garbage on screen (example: render targets). */
    tex_zero_all(t, w, h, levels, 1, blk, bpb);
    unsigned id_ = g_texn++;
    g_tex[id_] = t;
    g_tex_isrt[id_] = is_rt ? 1 : 0;
    atomic_fetch_add(&g_tex_created, 1); if (g_perflog) perflog_event(PL_TEX, 0, 0);
    return id_;
}
/* Guest (Reset): the render targets so far are dead. */
void metal_reset_rts(void){
    if (ENVC("NORTFREE")) return;        /* diagnostic: turn off releasing */
    atomic_store(&g_rtfree_upto, g_texn);
    atomic_store(&g_rtfree_at, atomic_load(&g_frames_drawn) + 3);
}
/* Main thread, at the end of a frame: release dead RTs if it is time. */
static void rt_free_pending(void){
    unsigned upto = atomic_load(&g_rtfree_upto);
    if (upto <= g_rtfree_done || atomic_load(&g_frames_drawn) < atomic_load(&g_rtfree_at)) return;
    for (unsigned i = g_rtfree_done; i < upto && i < MAXTEX; i++)
        if (g_tex_isrt[i] && g_tex[i]){
            g_freed_w[i] = (unsigned short)g_tex[i].width; g_freed_h[i] = (unsigned short)g_tex[i].height;
            g_tex[i] = nil; g_tex_isrt[i] = 0; g_tex_freed[i] = 1; g_rt_freed++; }
    g_rtfree_done = upto;
}

#if NFS_ENHANCED
/* HD: road reflection targets (see enhance.c "ROAD REFLECTION BLUR").
 * They are created with a mip chain; before a blur draw samples one of these
 * textures, its mips are generated if it got dirty since the last draw. */
static uint8_t  g_hd_road[MAXTEX], g_hd_dirty[MAXTEX];
static uint16_t g_hd_ow[MAXTEX],   g_hd_oh[MAXTEX];
static _Atomic unsigned long long g_hd_mipgen, g_hd_blurdraw;
void metal_hd_mark_road(unsigned id_, unsigned ow, unsigned oh){
    if (!id_ || id_ >= MAXTEX) return;
    g_hd_ow[id_] = (uint16_t)ow; g_hd_oh[id_] = (uint16_t)oh;
    g_hd_dirty[id_] = 1;
    g_hd_road[id_] = 1;
}
static int hd_is_road(unsigned t){ return t && t < MAXTEX && g_hd_road[t] && g_tex[t]; }

/* HD: MSAA for the screen (HD_MSAA=4 default, 0/1 off).
 * Only SCREEN passes are multisampled: the game's scene and UI are drawn directly
 * to the back buffer.  Render targets (reflection, post-process, cube) stay
 * single-sampled.  Every screen pass closes with StoreAndMultisampleResolve; so
 * the drawable StretchRect reads from the back buffer always carries the resolved
 * image.  For Z a multisampled TWIN of every surface is kept (their contents
 * aren't shared; see the g_hd_zshare counter). */
static int g_hd_msn = -1;
static id<MTLTexture> g_hd_mscol, g_hd_msdepth_scratch;
static id<MTLTexture> g_hd_msz[64];
/* --- MEMORYLESS MSAA ATTACHMENTS ---------------------------------------------
 * Multisampled color / G-buffer / depth used to always be Private: ~930 MB at 4K
 * (color 133 + G 664 + Z 133).  Yet every measured frame has a SINGLE multisampled
 * screen pass and the color/G samples are never written to memory, only resolved
 * into single-sampled (hd_ms_end).  On the Apple GPU such an attachment can be
 * MTLStorageModeMemoryless: it lives in tile memory and no memory is allocated.
 * Condition: the attachment is NOT LOADED (Clear/DontCare) and NOT STORED.  When the
 * pass opens, hd_ms_memless_ok decides by looking ahead; if it won't hold (a split
 * pass, a copy to the screen) the pass opens with the Private twins -- which are now
 * only created when needed.  If the prediction failed at close it is counted and
 * memoryless mode is turned off permanently.  HD_MSMEMLESS=0 turns it off. */
static id<MTLTexture> g_hd_mscol_ml, g_hd_msz_ml;
static int g_hd_ml_off = 0;                    /* the prediction failed: off permanently */
static _Atomic unsigned long long g_hd_ml_pass, g_hd_ml_miss;
/* Storing the MS depth is decided at close (hd_ms_end).  If a Z's last
 * multisampled pass didn't store, its samples are lost: a following pass that wants
 * to load without clearing is opened with a clear and counted. */
static uint8_t g_hd_msz_seen[64], g_hd_msz_kept[64];
static _Atomic unsigned long long g_hd_mszst, g_hd_mszskip, g_hd_mszlost;
/* the open multisampled screen pass (main thread only): dz = MS depth bound, its
 * store to be chosen at close; zclr = depth was opened with a clear;
 * res = a resolve to the single-sampled twin is bound */
static struct { int on, ml, dz, zclr, res; unsigned zid; } g_hd_msp;
/* the pipeline drawing the screen copy into the multisampled texture, per
 * log2(samples): the descriptor is kept at startup and built when the count changes
 * (MSAA BY RESOLUTION) */
static MTLRenderPipelineDescriptor *g_pso_ms_desc;
static id<MTLRenderPipelineState>   g_pso_msn[4];
static int hd_msaa_l2(void);
static id<MTLRenderPipelineState> hd_pso_ms(void){
    int l = hd_msaa_l2();
    if (!l || !g_pso_ms_desc) return nil;
    if (!g_pso_msn[l]){ NSError *e = nil;
        g_pso_ms_desc.rasterSampleCount = (NSUInteger)1u << l;
        g_pso_msn[l] = [g_dev newRenderPipelineStateWithDescriptor:g_pso_ms_desc error:&e]; }
    return g_pso_msn[l];
}
static uint8_t g_hd_zms_used[64];             /* used on screen in this frame */
static uint8_t g_hd_zpend_ms[64];             /* the twin's pending Clear(Z) */
static float   g_hd_zpend_msv[64];
static _Atomic unsigned long long g_hd_mspass, g_hd_msblit, g_hd_zshare, g_hd_zrev, g_hd_zres;
static uint8_t g_hd_zsdirty[64];              /* Z was written to the single-sampled twin outside the screen */
static int hd_msaa_n(void){
    if (g_hd_msn < 0){
        const char *e = ENVC("HD_MSAA");
        int n = (e && *e) ? atoi(e) : 4;
        if (n != 2 && n != 4 && n != 8) n = 1;
        if (n > 1 && g_dev && ![g_dev supportsTextureSampleCount:(NSUInteger)n]) n = 1;
        g_hd_msn = n;
        fprintf(stderr, "[HD] MSAA: %s\n", n > 1 ? (n == 8 ? "8x" : n == 4 ? "4x" : "2x") : "OFF");
    }
    return g_hd_msn;
}
/* --- MSAA BY RESOLUTION ----------------------------------------------------
 * 2x at 1440 lines and above, 4x below (user decision 2026-10-14: at 1440p and at
 * the Mac's own screen resolution).  At this pixel density 4x's edge gain is small;
 * its cost is the tile memory and resolve of the multisampled scene pass (at 4K
 * this pass is ~44% of GPU time; how much of it is due to the sample count wasn't
 * measured).  The screen size is checked every frame; when the count changes the
 * MS textures (sample count check) and pipelines (log2(samples) in the key) renew
 * themselves.  If HD_MSAA is given the count is fixed; also with HD_MSLAZY=0
 * (the single pipelines there are built at startup). */
static int hd_mslazy(void);
static int hd_msaa_l2(void){ int n = hd_msaa_n(); return n >= 8 ? 3 : n >= 4 ? 2 : n >= 2 ? 1 : 0; }
static void hd_msaa_frame(NSUInteger w, NSUInteger h){
    static int aut = -1;
    if (aut < 0){ const char *e = ENVC("HD_MSAA"); aut = !(e && *e); }
    if (!aut || hd_msaa_n() <= 1 || !hd_mslazy()) return;
    int want = (h >= 1400u) ? 2 : 4;
    if (want == g_hd_msn || ![g_dev supportsTextureSampleCount:(NSUInteger)want]) return;
    g_hd_msn = want;
    fprintf(stderr, "[HD] MSAA: %dx (drawing %lux%lu; 2x at 1440 lines and above)\n",
            want, (unsigned long)w, (unsigned long)h);
}
static id<MTLTexture> hd_ms_tex(id<MTLTexture> old, MTLPixelFormat pf,
                                NSUInteger w, NSUInteger h){
    if (old && old.width == w && old.height == h && old.sampleCount == (NSUInteger)hd_msaa_n()) return old;
    MTLTextureDescriptor *td = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:pf width:w height:h mipmapped:NO];
    td.textureType = MTLTextureType2DMultisample;
    td.sampleCount = (NSUInteger)hd_msaa_n();
    td.usage = MTLTextureUsageRenderTarget;
    td.storageMode = MTLStorageModePrivate;
    return [g_dev newTextureWithDescriptor:td];
}
static id<MTLTexture> hd_ms_tex_ml(id<MTLTexture> old, MTLPixelFormat pf,
                                   NSUInteger w, NSUInteger h){   /* see MEMORYLESS MSAA ATTACHMENTS */
    if (old && old.width == w && old.height == h && old.sampleCount == (NSUInteger)hd_msaa_n()) return old;
    MTLTextureDescriptor *td = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:pf width:w height:h mipmapped:NO];
    td.textureType = MTLTextureType2DMultisample;
    td.sampleCount = (NSUInteger)hd_msaa_n();
    td.usage = MTLTextureUsageRenderTarget;
    td.storageMode = MTLStorageModeMemoryless;
    return [g_dev newTextureWithDescriptor:td];
}
static int hd_ml_hw(void){                     /* memoryless texture support + HD_MSMEMLESS */
    static int v = -1;
    if (v < 0){ const char *e = ENVC("HD_MSMEMLESS");
        v = !(e && *e == '0') && g_dev && [g_dev supportsFamily:MTLGPUFamilyApple1]; }
    return v;
}

/* HD: the main view's camera, PER FRAME (the guest writes, the main thread reads).
 * 48 floats: view (16), projection (16), product (16); D3D row-vector layout
 * (v' = v * M).  Source: overrides.c sub_005c7ed0. */
static float g_hd_cam[2][48];
static float g_hd_cam_k[2];                 /* Hor+ clip x factor */
static float g_hd_cube[2][9];               /* world directions of the cube +x,+y,+z axes */
static int   g_hd_cam_ok[2];
void metal_hd_set_camera(const float *m48, float k, const float *cube9){
    pthread_mutex_lock(&g_qlock);
    memcpy(g_hd_cam[g_cur], m48, sizeof g_hd_cam[0]);
    memcpy(g_hd_cube[g_cur], cube9, sizeof g_hd_cube[0]);
    g_hd_cam_k[g_cur] = k;
    g_hd_cam_ok[g_cur] = 1;
    pthread_mutex_unlock(&g_qlock);
}

/* ===== HD: SSR (screen-space reflections) ===================================
 * Reflective surfaces (road: ps 5/6 sampling the road reflection texture) write
 * their own reflection weight B and the original reflection R0 to the G-buffer in
 * the main scene pass (vsemu.c ps_to_msl_ex2).  As soon as the scene ends (the
 * first StretchRect from the screen) the composite pass marches a ray through the
 * depth buffer and, on a hit, writes color + B*(R_new - R0).  Without a hit R0
 * stays (the game's planar reflection: the correct fallback for offscreen content).
 * Later R_new will come from ray tracing; the G-buffer and composite are the same.
 * Env: HD_SSR=0 off, HD_SSR_STEPS=48, HD_SSR_MAXD=120 (meters). */
static int g_hd_ssr = -1;
#define HD_NG 3   /* G-buffer: three targets, see G-BUFFER PACKING */
/* --- G-BUFFER PACKING -------------------------------------------------------
 * Old layout (HD_GPACK=0): B+class RGBA16F, R0 RGBA8, direction RGBA16F = 20 bytes.
 * Packed (default) 12 bytes, still three targets and all resolvable in MSAA:
 *   G0 RGBA8Unorm   R0.rgb + class/4.  Since the class stays in 8 bits, edge
 *                   averages are filtered out as before (GB_ROAD/GB_CAR).
 *   G1 RGB10A2Unorm B/4: range 0..4, step 0.004.  On the road B = texture alpha x
 *                   vertex alpha <= 1 (GRAPHICS_RESEARCH.md 8); car paint's wasn't
 *                   measured.  Above 4 it is clamped: the reflection weakens, it
 *                   doesn't break.
 *   G2 RGB10A2Unorm unit direction, code = 512 + 511*d (512 = exactly zero).  The
 *                   linear encoding preserves the average on resolve: a direction
 *                   averaged with zero at an edge gets shorter but points the same
 *                   way (octahedral encoding would break this).
 * Measured (2026-10-14, a GPU test without the game: the same random samples were
 * written to both layouts at 2x and 4x, resolved by the hardware and read with these
 * macros): class decisions exactly the same; the B*(R_new - R0) difference from the
 * old layout at most 1 LSB (median 0.3); the direction difference at most 0.105
 * degrees on a uniform pixel.
 * Readers bind with the same indices (G0 2, G1 3, G2 4); decoding is in the GB_*
 * macros (hd_gb_src), the writer is vsemu.c ps_to_msl_ex2.  Gain: multisampled tile
 * memory color + G per sample 24 -> 16 bytes; the resolve writes every frame, the
 * composite reads and the single-sampled G textures are 40% smaller (119 -> 71 MB
 * at 3024x1964). */
static int hd_gpack(void){
    static int v = -1;
    if (v < 0){ const char *e = ENVC("HD_GPACK"); v = (e && *e == '0') ? 0 : 1;
        fprintf(stderr, "[HD] G-buffer: %s\n", v ? "packed (12 bytes/sample)" : "old (20 bytes/sample)"); }
    return v;
}
static MTLPixelFormat hd_gfmt(int gi){
    static const MTLPixelFormat old[HD_NG] = { MTLPixelFormatRGBA16Float, MTLPixelFormatRGBA8Unorm,
                                               MTLPixelFormatRGBA16Float };
    static const MTLPixelFormat pk[HD_NG]  = { MTLPixelFormatRGBA8Unorm, MTLPixelFormatRGB10A2Unorm,
                                               MTLPixelFormatRGB10A2Unorm };
    return hd_gpack() ? pk[gi] : old[gi];
}
/* at the start of every shader that reads the G-buffer */
static NSString *kGBDec = @
"#if GB_PACK\n"
"#define GB_CLS(g0)    ((g0).a * 4.0)\n"
"#define GB_R0(g0, g1) ((g0).rgb)\n"
"#define GB_B(g0, g1)  ((g1).rgb * 4.0)\n"
"#define GB_DIR(g2)    (((g2).rgb * 1023.0 - 512.0) * (1.0 / 511.0))\n"
/* so the rounding of the 8-bit class (<= 0.008) doesn't let averages at the boundary in */
"#define GB_ROAD(c)    ((c) > 0.51 && (c) < 1.49)\n"
"#define GB_CAR(c)     ((c) > 1.76 && (c) < 2.24)\n"
"#else\n"
"#define GB_CLS(g0)    ((g0).a)\n"
"#define GB_R0(g0, g1) ((g1).rgb)\n"
"#define GB_B(g0, g1)  ((g0).rgb)\n"
"#define GB_DIR(g2)    ((g2).xyz)\n"
"#define GB_ROAD(c)    ((c) > 0.5 && (c) < 1.5)\n"
"#define GB_CAR(c)     ((c) > 1.75 && (c) < 2.25)\n"
"#endif\n";
static NSString *hd_gb_src(NSString *src){
    return [[(hd_gpack() ? @"#define GB_PACK 1\n" : @"#define GB_PACK 0\n")
             stringByAppendingString:kGBDec] stringByAppendingString:src];
}
static id<MTLTexture> g_hd_g[HD_NG], g_hd_gms[HD_NG], g_hd_gms_ml[HD_NG], g_hd_scenecopy;
static id<MTLRenderPipelineState> g_hd_ssr_pso, g_hd_ssr_pso_ms;
static uint8_t g_hd_vtried[MAXPS];
static _Atomic unsigned long long g_hd_ssr_n, g_hd_refl_draws;
static int hd_ssr_on(void){
    if (g_hd_ssr < 0){ const char *e = ENVC("HD_SSR");
        g_hd_ssr = (e && *e == '0') ? 0 : 1;
        fprintf(stderr, "[HD] SSR: %s\n", g_hd_ssr ? "ON" : "OFF"); }
    return g_hd_ssr;
}
/* At draw time (guest thread): for a ps drawing to the screen and sampling the road
 * reflection texture, switch to the separation variant. */
int metal_tex_is_cube(unsigned id_);
static int hd_refl_ps(int ps, unsigned target, const unsigned tx[4]){
    if (ps <= 0 || ps >= PSREFL || target || !hd_ssr_on()) return ps;
    int k = -1;
    for (int q = 0; q < 4; q++) if (hd_is_road(tx[q])){ k = q; break; }
    /* car paint: the cube map at stage 0 (the ps is already the cube variant) */
    { static int car = -1;
      if (car < 0){ const char *e = ENVC("HD_SSR_CAR"); car = (e && *e == '0') ? 0 : 1; }
      if (k < 0 && car && ps >= PSCUBE && metal_tex_is_cube(tx[0])) k = 0; }
    if (k < 0) return ps;
    int vid = ps + PSREFL * (1 + k);
    if (vid >= MAXPS) return ps;
    if (!g_psfn[vid] && !g_hd_vtried[vid]){
        g_hd_vtried[vid] = 1;
        int com_ps_refl_variant(int, int, int, int);
        com_ps_refl_variant(ps % PSCUBE, ps >= PSCUBE, k, vid);
    }
    ps_wait(vid);
    return g_psfn[vid] ? vid : ps;
}
/* 4x4 inverse, row-major.  0 on failure. */
static int hd_inv4(const float *m, float *o){
    float inv[16], det;
    inv[0] = m[5]*m[10]*m[15]-m[5]*m[11]*m[14]-m[9]*m[6]*m[15]+m[9]*m[7]*m[14]+m[13]*m[6]*m[11]-m[13]*m[7]*m[10];
    inv[4] = -m[4]*m[10]*m[15]+m[4]*m[11]*m[14]+m[8]*m[6]*m[15]-m[8]*m[7]*m[14]-m[12]*m[6]*m[11]+m[12]*m[7]*m[10];
    inv[8] = m[4]*m[9]*m[15]-m[4]*m[11]*m[13]-m[8]*m[5]*m[15]+m[8]*m[7]*m[13]+m[12]*m[5]*m[11]-m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14]+m[4]*m[10]*m[13]+m[8]*m[5]*m[14]-m[8]*m[6]*m[13]-m[12]*m[5]*m[10]+m[12]*m[6]*m[9];
    inv[1] = -m[1]*m[10]*m[15]+m[1]*m[11]*m[14]+m[9]*m[2]*m[15]-m[9]*m[3]*m[14]-m[13]*m[2]*m[11]+m[13]*m[3]*m[10];
    inv[5] = m[0]*m[10]*m[15]-m[0]*m[11]*m[14]-m[8]*m[2]*m[15]+m[8]*m[3]*m[14]+m[12]*m[2]*m[11]-m[12]*m[3]*m[10];
    inv[9] = -m[0]*m[9]*m[15]+m[0]*m[11]*m[13]+m[8]*m[1]*m[15]-m[8]*m[3]*m[13]-m[12]*m[1]*m[11]+m[12]*m[3]*m[9];
    inv[13] = m[0]*m[9]*m[14]-m[0]*m[10]*m[13]-m[8]*m[1]*m[14]+m[8]*m[2]*m[13]+m[12]*m[1]*m[10]-m[12]*m[2]*m[9];
    inv[2] = m[1]*m[6]*m[15]-m[1]*m[7]*m[14]-m[5]*m[2]*m[15]+m[5]*m[3]*m[14]+m[13]*m[2]*m[7]-m[13]*m[3]*m[6];
    inv[6] = -m[0]*m[6]*m[15]+m[0]*m[7]*m[14]+m[4]*m[2]*m[15]-m[4]*m[3]*m[14]-m[12]*m[2]*m[7]+m[12]*m[3]*m[6];
    inv[10] = m[0]*m[5]*m[15]-m[0]*m[7]*m[13]-m[4]*m[1]*m[15]+m[4]*m[3]*m[13]+m[12]*m[1]*m[7]-m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14]+m[0]*m[6]*m[13]+m[4]*m[1]*m[14]-m[4]*m[2]*m[13]-m[12]*m[1]*m[6]+m[12]*m[2]*m[5];
    inv[3] = -m[1]*m[6]*m[11]+m[1]*m[7]*m[10]+m[5]*m[2]*m[11]-m[5]*m[3]*m[10]-m[9]*m[2]*m[7]+m[9]*m[3]*m[6];
    inv[7] = m[0]*m[6]*m[11]-m[0]*m[7]*m[10]-m[4]*m[2]*m[11]+m[4]*m[3]*m[10]+m[8]*m[2]*m[7]-m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11]+m[0]*m[7]*m[9]+m[4]*m[1]*m[11]-m[4]*m[3]*m[9]-m[8]*m[1]*m[7]+m[8]*m[3]*m[5];
    inv[15] = m[0]*m[5]*m[10]-m[0]*m[6]*m[9]-m[4]*m[1]*m[10]+m[4]*m[2]*m[9]+m[8]*m[1]*m[6]-m[8]*m[2]*m[5];
    det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (fabsf(det) < 1e-20f) return 0;
    det = 1.0f / det;
    for (int i = 0; i < 16; i++) o[i] = inv[i] * det;
    return 1;
}
static id<MTLTexture> hd_tex(id<MTLTexture> old, MTLPixelFormat pf, NSUInteger w, NSUInteger h,
                            NSUInteger samples){
    if (old && old.width == w && old.height == h && old.sampleCount == samples) return old;
    MTLTextureDescriptor *td = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:pf width:w height:h mipmapped:NO];
    if (samples > 1){ td.textureType = MTLTextureType2DMultisample; td.sampleCount = samples;
                      td.usage = MTLTextureUsageRenderTarget; }
    else td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModePrivate;
    return [g_dev newTextureWithDescriptor:td];
}
static NSString *kSSRSrc = @
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct FSO { float4 pos [[position]]; };\n"
"vertex FSO hd_fs_vert(uint vid [[vertex_id]]) {\n"
"  float2 p = float2(float((vid << 1) & 2), float(vid & 2));\n"
"  FSO o; o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0); return o; }\n"
"struct SSRP { float4x4 vp; float4x4 ivp; float4 cam; float4 prm; float4 sz; float4 cam2;\n"
"              float4 cb0; float4 cb1; float4 cb2; };\n"
/* prm = (A, Bc, strength, max distance), sz = (W, H, step, thickness) */
"static float lin_z(float d, constant SSRP &S) { return S.prm.y / (d - S.prm.x); }\n"
"fragment float4 hd_ssr_frag(FSO in [[stage_in]],\n"
"    texture2d<float> scene [[texture(0)]], depth2d<float> dep [[texture(1)]],\n"
"    texture2d<float> g1 [[texture(2)]], texture2d<float> g2 [[texture(3)]],\n"
"    texture2d<float> g3 [[texture(4)]],\n"
"    constant SSRP &S [[buffer(0)]]) {\n"
"  uint2 px = uint2(in.pos.xy);\n"
"  float4 c = scene.read(px);\n"
"  float4 gb = g1.read(px);\n"
"  int dbg = int(S.cam.w) - 1;                             /* 0 normal, 1 hit, 2 direction */\n"
"  float cls = GB_CLS(gb);\n"
"  bool road = GB_ROAD(cls);\n"
"  bool car  = GB_CAR(cls);                                /* filter out edge averages */\n"
"  if (!road && !car) return dbg > 0 ? float4(c.rgb * 0.25, c.a) : c;\n"
"  float d = dep.read(px);\n"
"  if (d >= 1.0) return c;\n"
"  float2 uv = (float2(px) + 0.5) / S.sz.xy;\n"
"  float4 wp = S.ivp * float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, d, 1.0);\n"
"  float3 P = wp.xyz / wp.w;\n"
"  float3 V = normalize(P - S.cam.xyz);\n"
"  float3 Rd; float maxd = S.prm.w; float lod = 0.0; float dt = 0.25;\n"
"  if (road) Rd = reflect(V, float3(0.0, 0.0, 1.0));\n"
"  else {\n"
"    float3 gd = GB_DIR(g3.read(px));\n"
"    if (dot(gd, gd) < 1e-6) return c;\n"
"    Rd = normalize(gd.x * S.cb0.xyz + gd.y * S.cb1.xyz + gd.z * S.cb2.xyz);\n"
"    if (dbg == 2) return float4(Rd * 0.5 + 0.5, 1.0);\n"
/* N = normalize(R - V): step slightly off the surface, self-intersection */
"    float3 N = normalize(Rd - V);\n"
"    if (dbg == 3) {\n"                 /* does N agree with the geometric normal from depth */
"      float2 u1 = (float2(px) + float2(1.5, 0.5)) / S.sz.xy, u2 = (float2(px) + float2(0.5, 1.5)) / S.sz.xy;\n"
"      float4 w1 = S.ivp * float4(u1.x * 2.0 - 1.0, 1.0 - u1.y * 2.0, dep.read(px + uint2(1, 0)), 1.0);\n"
"      float4 w2 = S.ivp * float4(u2.x * 2.0 - 1.0, 1.0 - u2.y * 2.0, dep.read(px + uint2(0, 1)), 1.0);\n"
"      float3 Ng = normalize(cross(w1.xyz / w1.w - P, w2.xyz / w2.w - P));\n"
"      if (dot(Ng, V) > 0.0) Ng = -Ng;\n"
"      float a = dot(N, Ng);\n"
"      return float4(a < 0.3 ? 1.0 : 0.0, a > 0.7 ? 1.0 : 0.0, (a >= 0.3 && a <= 0.7) ? 1.0 : 0.0, 1.0);\n"
"    }\n"
"    if (dot(N, V) > -0.02) return dbg > 0 ? float4(0.0, 0.0, 0.3, 1.0) : c;\n"
"    P += N * 0.04;\n"
"    maxd = min(maxd, 60.0); lod = S.cam2.x; dt = 0.08;\n"
"  }\n"
"  if (road && Rd.z <= 0.0) return c;\n"
"  constexpr sampler ls(filter::linear, mip_filter::linear, address::clamp_to_edge);\n"
"  int steps = int(S.sz.z);\n"
"  float t = 0.0, tprev = 0.0;\n"
"  float2 huv = float2(-1.0); float conf = 0.0;\n"
"  for (int i = 0; i < steps; i++) {\n"
"    tprev = t; t += dt; dt *= 1.12;\n"
"    if (t > maxd) break;\n"
"    float4 q = S.vp * float4(P + Rd * t, 1.0);\n"
"    if (q.w <= 0.6) break;\n"
"    float2 suv = float2(q.x / q.w * 0.5 + 0.5, 0.5 - q.y / q.w * 0.5);\n"
"    if (any(suv < 0.0) || any(suv > 1.0)) break;\n"
"    float zs = lin_z(dep.read(uint2(suv * S.sz.xy)), S);\n"
"    float diff = q.w - zs;\n"
"    if (diff > 0.02) {\n"
"      if (diff < max(S.sz.w, dt * 1.5)) {\n"
"        float a = tprev, b = t;\n"
"        for (int j = 0; j < 6; j++) {\n"
"          float m = 0.5 * (a + b);\n"
"          float4 qm = S.vp * float4(P + Rd * m, 1.0);\n"
"          float2 mu = float2(qm.x / qm.w * 0.5 + 0.5, 0.5 - qm.y / qm.w * 0.5);\n"
"          float zm = lin_z(dep.read(uint2(clamp(mu, 0.0, 0.9999) * S.sz.xy)), S);\n"
"          if (qm.w - zm > 0.0) b = m; else a = m;\n"
"          suv = mu; }\n"
"        float2 e = min(suv, 1.0 - suv);\n"
"        conf = smoothstep(0.0, 0.08, min(e.x, e.y)) * (1.0 - smoothstep(0.6, 1.0, t / maxd));\n"
"        huv = suv;\n"
"      }\n"
"      break;\n"
"    }\n"
"  }\n"
"  if (dbg == 1) return float4(road ? 0.3 + 0.7 * saturate(length(GB_B(gb, g2.read(px)))) : 0.0, conf,\n"
"                              conf > 0.0 ? (road ? 0.0 : 1.0) : 0.6, 1.0);\n"
"  if (conf <= 0.0) return c;\n"
"  float4 gv = g2.read(px);\n"
"  float3 R0 = GB_R0(gb, gv);\n"
"  float3 Rn = mix(R0, scene.sample(ls, huv, level(lod)).rgb, conf * S.prm.z);\n"
"  return float4(c.rgb + GB_B(gb, gv) * (Rn - R0), c.a);\n"
"}\n";
/* ===== HD: RAY TRACING TIME MEASUREMENT (HD_RTPROBE=1, at the DUMPFRAME frame) =====
 * Question: on this machine, how long does it TAKE to put the scene into a ray
 * tracing structure every frame and cast reflection rays?  Runs once, prints a
 * report, writes two PNGs.
 *   1) Capture (CPU): the main scene's draws into world space.  The vertex shader
 *      is NOT RUN: since oPos = p * WVP, world = p * (WVP * (VP')^-1); WVP is the
 *      draw's c[cx..cx+3] constants.
 *   2) Build (GPU): the whole scene as a single primitive acceleration structure;
 *      default, fast build and refit separately.
 *   3) Tracing (GPU): primary rays (all pixels; a verification image) and
 *      reflection rays (only pixels reflecting in the G-buffer).          */
#define HD_MAXVSI 512
static struct { int16_t po, pty, cx, fogr, litr; uint8_t skin, ok; float ramp[2]; } g_hd_vsi[HD_MAXVSI];
void metal_hd_vs_info(int gid, int po, int pty, int cx, int skinned, int fogr, int litr, float ramp0, float ramp1){
    if (gid <= 0 || gid >= HD_MAXVSI) return;
    g_hd_vsi[gid].po = (int16_t)po; g_hd_vsi[gid].pty = (int16_t)pty;
    g_hd_vsi[gid].cx = (int16_t)cx; g_hd_vsi[gid].skin = skinned ? 1 : 0;
    g_hd_vsi[gid].fogr = (int16_t)fogr;
    g_hd_vsi[gid].litr = (int16_t)litr; g_hd_vsi[gid].ramp[0] = ramp0; g_hd_vsi[gid].ramp[1] = ramp1;
    g_hd_vsi[gid].ok = 1;
    if (ENVC("HD_VSINFO")) fprintf(stderr, "[HD] vs_%d: position ofs %d type %d, matrix c%d, skinned %d, fog c%d, light c%d\n",
                                   gid, po, pty, cx, skinned, fogr, litr);
}
static NSString *kRTSrc = @
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"using namespace raytracing;\n"
"struct RTP { float4x4 ivp; float4 cam; float4 cb0; float4 cb1; float4 cb2; float4 sz; };\n"
/* sz = (W, H, mode, -) ; mode 0 = primary, 1 = reflection */
"kernel void hd_rt_probe(uint2 tid [[thread_position_in_grid]],\n"
"    texture2d<float, access::write> outp [[texture(0)]], depth2d<float> dep [[texture(1)]],\n"
"    texture2d<float> g1 [[texture(2)]], texture2d<float> g3 [[texture(3)]],\n"
"    primitive_acceleration_structure acc [[buffer(0)]], constant RTP &S [[buffer(1)]]) {\n"
"  if (tid.x >= uint(S.sz.x) || tid.y >= uint(S.sz.y)) return;\n"
"  float2 uv = (float2(tid) + 0.5) / S.sz.xy;\n"
"  float2 nd = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);\n"
"  ray r; r.min_distance = 0.02; r.max_distance = 3000.0;\n"
"  float tag = 0.0;\n"
"  if (S.sz.z < 0.5) {\n"
"    float4 w = S.ivp * float4(nd, 0.5, 1.0);\n"
"    r.origin = S.cam.xyz; r.direction = normalize(w.xyz / w.w - S.cam.xyz);\n"
"  } else {\n"
"    float4 gb = g1.read(tid); float cls = GB_CLS(gb);\n"
"    bool road = GB_ROAD(cls), car = GB_CAR(cls);\n"
"    float d = dep.read(tid);\n"
"    if ((!road && !car) || d >= 1.0) { outp.write(float4(0.0), tid); return; }\n"
"    float4 w = S.ivp * float4(nd, d, 1.0);\n"
"    float3 P = w.xyz / w.w; float3 V = normalize(P - S.cam.xyz); float3 N = float3(0.0, 0.0, 1.0);\n"
"    float3 Rd;\n"
"    if (road) Rd = reflect(V, N);\n"
"    else { float3 gd = GB_DIR(g3.read(tid));\n"
"           Rd = normalize(gd.x * S.cb0.xyz + gd.y * S.cb1.xyz + gd.z * S.cb2.xyz); N = normalize(Rd - V); }\n"
"    r.origin = P + N * 0.05; r.direction = Rd; tag = road ? 0.5 : 1.0;\n"
"  }\n"
"  intersector<triangle_data> it;\n"
"  it.assume_geometry_type(geometry_type::triangle);\n"
"  it.force_opacity(forced_opacity::opaque);\n"
"  intersection_result<triangle_data> h = it.intersect(r, acc);\n"
"  if (h.type == intersection_type::none) { outp.write(float4(0.0, 0.0, tag > 0.0 ? 0.25 : 0.0, tag > 0.0 ? 1.0 : 0.5), tid); return; }\n"
/* a light tint from distance + triangle id: so the geometry is readable */
"  float sh = 1.0 / (1.0 + 0.02 * h.distance);\n"
"  float hsh = float((h.primitive_id * 2654435761u) >> 24) / 255.0;\n"
"  outp.write(float4(sh * (0.6 + 0.4 * hsh), sh * (0.7 + 0.3 * h.triangle_barycentric_coord.x), sh, 1.0), tid);\n"
"}\n";
static double hd_now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
                            return t.tv_sec * 1e3 + t.tv_nsec * 1e-6; }
static void hd_rt_probe(int rb, id<MTLTexture> ref, long frame){
    static int done = 0; if (done) return; done = 1;
    fprintf(stderr, "\n[RT] ===== ray tracing time measurement (frame %ld) =====\n", frame);
    if (!g_dev.supportsRaytracing){ fprintf(stderr, "[RT] the device doesn't support ray tracing\n"); return; }
    if (!g_hd_cam_ok[rb]){ fprintf(stderr, "[RT] no camera\n"); return; }
    float vp[16], ivp[16], iv[16];
    memcpy(vp, &g_hd_cam[rb][32], sizeof vp);
    for (int r4 = 0; r4 < 4; r4++) vp[r4*4] *= g_hd_cam_k[rb];
    if (!hd_inv4(vp, ivp) || !hd_inv4(&g_hd_cam[rb][0], iv)) return;

    /* --- 1) capture --- */
    double t0 = hd_now();
    size_t cap = 1u << 20, nv = 0;
    float *V = (float *)malloc(cap * 12);
    unsigned nd = 0, sk_vs = 0, sk_skin = 0, sk_ortho = 0, sk_nz = 0;
    for (unsigned i = 0; i < g_dn[rb]; i++){
        MDraw *dr = &g_dbuf[rb][i];
        if (dr->is_blit && !dr->blit_src) break;            /* end of the scene */
        if (dr->target || dr->is_clear || dr->is_blit || dr->count < 3) continue;
        if (dr->vsid <= 0 || dr->vsid >= HD_MAXVSI || !g_hd_vsi[dr->vsid].ok ||
            g_hd_vsi[dr->vsid].po < 0 || g_hd_vsi[dr->vsid].cx < 0 ||
            (g_hd_vsi[dr->vsid].pty != 2 && g_hd_vsi[dr->vsid].pty != 3)){ sk_vs++;
            { static uint8_t seen[HD_MAXVSI];
              if (dr->vsid > 0 && dr->vsid < HD_MAXVSI && !seen[dr->vsid]){ seen[dr->vsid] = 1;
                  fprintf(stderr, "[RT]   skipped vs_%d: info=%d position offset=%d type=%d matrix register=%d (ps %d, %u verts)\n",
                          dr->vsid, g_hd_vsi[dr->vsid].ok, g_hd_vsi[dr->vsid].po, g_hd_vsi[dr->vsid].pty,
                          g_hd_vsi[dr->vsid].cx, dr->ps, dr->count); } }
            continue; }
        if (g_hd_vsi[dr->vsid].skin){ sk_skin++; continue; }
        if (!(dr->depth & 2u)){ sk_nz++; continue; }        /* doesn't write Z: lights, particles */
        const uint8_t *kb = g_kbuf[rb] + dr->koff;
        unsigned stride; memcpy(&stride, kb, 4);
        const float *k = (const float *)(kb + 16);
        int cx = g_hd_vsi[dr->vsid].cx;
        if ((unsigned)(cx + 4) * 16u > dr->ksz - 16u || !stride){ sk_vs++; continue; }
        const float *c3 = k + (cx + 3) * 4;
        if (c3[0] == 0.f && c3[1] == 0.f && c3[2] == 0.f){ sk_ortho++; continue; }
        /* M[r][c] = c[cx+c][r];  W = M * ivp  (double precision) */
        double W[16];
        for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++){
            double a = 0; for (int q = 0; q < 4; q++) a += (double)k[(cx + q) * 4 + r] * (double)ivp[q * 4 + c];
            W[r * 4 + c] = a; }
        if (nv + dr->count > cap){ while (nv + dr->count > cap) cap *= 2; V = (float *)realloc(V, cap * 12); }
        unsigned po = (unsigned)g_hd_vsi[dr->vsid].po;
        for (unsigned j = 0; j < dr->count; j++){
            float p[3]; memcpy(p, dr_vtx(rb, dr, j, stride) + po, 12);
            double x = p[0]*W[0] + p[1]*W[4] + p[2]*W[8]  + W[12];
            double y = p[0]*W[1] + p[1]*W[5] + p[2]*W[9]  + W[13];
            double z = p[0]*W[2] + p[1]*W[6] + p[2]*W[10] + W[14];
            double w = p[0]*W[3] + p[1]*W[7] + p[2]*W[11] + W[15];
            float *o = V + (nv + j) * 3;
            o[0] = (float)(x / w); o[1] = (float)(y / w); o[2] = (float)(z / w);
        }
        nv += dr->count - dr->count % 3; nd++;
    }
    double t_cap = hd_now() - t0;
    size_t ntri = nv / 3;
    fprintf(stderr, "[RT] capture: %u draws, %zu triangles, CPU %.2f ms  (skipped: vs %u, skinned %u, orthographic %u, no Z write %u)\n",
            nd, ntri, t_cap, sk_vs, sk_skin, sk_ortho, sk_nz);
    if (ntri < 16){ free(V); return; }
    { float mn[3] = {1e30f,1e30f,1e30f}, mx[3] = {-1e30f,-1e30f,-1e30f};
      for (size_t q = 0; q < nv; q++) for (int a = 0; a < 3; a++){ float v = V[q*3+a];
          if (v < mn[a]) mn[a] = v; if (v > mx[a]) mx[a] = v; }
      fprintf(stderr, "[RT] world box: x[%.0f..%.0f] y[%.0f..%.0f] z[%.0f..%.0f]  camera (%.1f %.1f %.1f)\n",
              mn[0],mx[0],mn[1],mx[1],mn[2],mx[2], iv[12],iv[13],iv[14]); }

    @autoreleasepool {
    id<MTLBuffer> vb = [g_dev newBufferWithBytes:V length:nv * 12 options:MTLResourceStorageModeShared];
    free(V);
    /* --- 2) build --- */
    id<MTLAccelerationStructure> acc = nil;
    static const char *un[3] = { "default build", "fast build (PreferFastBuild)", "refit" };
    for (int u = 0; u < 3; u++){
        MTLAccelerationStructureTriangleGeometryDescriptor *gd =
            [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
        gd.vertexBuffer = vb; gd.vertexStride = 12; gd.triangleCount = ntri; gd.opaque = YES;
        MTLPrimitiveAccelerationStructureDescriptor *ad = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
        ad.geometryDescriptors = @[gd];
        ad.usage = (u == 1) ? MTLAccelerationStructureUsagePreferFastBuild
                 : (u == 2) ? MTLAccelerationStructureUsageRefit : MTLAccelerationStructureUsageNone;
        MTLAccelerationStructureSizes sz = [g_dev accelerationStructureSizesWithDescriptor:ad];
        id<MTLAccelerationStructure> a2 = [g_dev newAccelerationStructureWithSize:sz.accelerationStructureSize];
        NSUInteger ss = sz.buildScratchBufferSize > sz.refitScratchBufferSize ? sz.buildScratchBufferSize : sz.refitScratchBufferSize;
        id<MTLBuffer> scr = [g_dev newBufferWithLength:ss ? ss : 16 options:MTLResourceStorageModePrivate];
        double best = 1e30, sum = 0; int N = 6;
        for (int it = 0; it < N; it++){
            id<MTLCommandBuffer> cb = [g_queue commandBuffer];
            id<MTLAccelerationStructureCommandEncoder> ae = [cb accelerationStructureCommandEncoder];
            if (u == 2 && it > 0)
                [ae refitAccelerationStructure:a2 descriptor:ad destination:a2 scratchBuffer:scr scratchBufferOffset:0];
            else
                [ae buildAccelerationStructure:a2 descriptor:ad scratchBuffer:scr scratchBufferOffset:0];
            [ae endEncoding];
            [cb commit]; [cb waitUntilCompleted];
            double ms = (cb.GPUEndTime - cb.GPUStartTime) * 1e3;
            if (u == 2 && it == 0) continue;               /* first round build */
            if (it == 0) continue;                         /* warm-up */
            if (ms < best) best = ms; sum += ms;
        }
        fprintf(stderr, "[RT] %-34s: best %.2f ms, mean %.2f ms  (structure %.1f MB)\n", un[u], best, sum / (N - 1),
                sz.accelerationStructureSize / 1048576.0);
        if (u == 0) acc = a2;
    }
    /* --- 3) tracing --- */
    NSError *e = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:hd_gb_src(kRTSrc) options:nil error:&e];
    if (!lib){ fprintf(stderr, "[RT] kernel could not be compiled: %s\n", [[e localizedDescription] UTF8String]); return; }
    id<MTLComputePipelineState> cps = [g_dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"hd_rt_probe"] error:&e];
    if (!cps){ fprintf(stderr, "[RT] compute pipeline could not be built: %s\n", [[e localizedDescription] UTF8String]); return; }
    NSUInteger Wd = ref.width, Hd = ref.height;
    MTLTextureDescriptor *od = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                     width:Wd height:Hd mipmapped:NO];
    od.usage = MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead; od.storageMode = MTLStorageModeShared;
    id<MTLTexture> outp = [g_dev newTextureWithDescriptor:od];
    id<MTLTexture> zt = g_ztex[1];
    int have_g = g_hd_g[0] && g_hd_g[2] && zt && zt.width == Wd && zt.height == Hd && g_hd_g[0].width == Wd;
    struct { float ivp[16], cam[4], cb[3][4], sz[4]; } P;
    memcpy(P.ivp, ivp, sizeof ivp);
    P.cam[0] = iv[12]; P.cam[1] = iv[13]; P.cam[2] = iv[14]; P.cam[3] = 1.f;
    for (int a3 = 0; a3 < 3; a3++){ memcpy(P.cb[a3], &g_hd_cube[rb][a3*3], 12); P.cb[a3][3] = 0.f; }
    P.sz[0] = (float)Wd; P.sz[1] = (float)Hd; P.sz[3] = 0.f;
    static const char *mn2[2] = { "primary rays (all pixels)", "reflection rays (road + car)" };
    for (int mode = 0; mode < 2; mode++){
        if (mode == 1 && !have_g){ fprintf(stderr, "[RT] reflection mode skipped: no G-buffer (HD_SSR must be on)\n"); break; }
        P.sz[2] = (float)mode;
        double best = 1e30, sum = 0; int N = 8;
        for (int it = 0; it < N; it++){
            id<MTLCommandBuffer> cb = [g_queue commandBuffer];
            id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
            [ce setComputePipelineState:cps];
            [ce setTexture:outp atIndex:0];
            [ce setTexture:(have_g ? zt : nil) atIndex:1];
            [ce setTexture:(have_g ? g_hd_g[0] : nil) atIndex:2];
            [ce setTexture:(have_g ? g_hd_g[2] : nil) atIndex:3];
            [ce setAccelerationStructure:acc atBufferIndex:0];
            [ce setBytes:&P length:sizeof P atIndex:1];
            NSUInteger tw = cps.threadExecutionWidth, th = cps.maxTotalThreadsPerThreadgroup / tw;
            [ce dispatchThreadgroups:MTLSizeMake((Wd + tw - 1) / tw, (Hd + th - 1) / th, 1)
               threadsPerThreadgroup:MTLSizeMake(tw, th, 1)];
            [ce endEncoding];
            [cb commit]; [cb waitUntilCompleted];
            double ms = (cb.GPUEndTime - cb.GPUStartTime) * 1e3;
            if (it == 0) continue;
            if (ms < best) best = ms; sum += ms;
        }
        /* ray count: pixels with alpha > 0.75 traced */
        size_t rays = 0, hits = 0;
        { uint8_t *px = (uint8_t *)malloc(Wd * Hd * 4);
          [outp getBytes:px bytesPerRow:Wd * 4 fromRegion:MTLRegionMake2D(0, 0, Wd, Hd) mipmapLevel:0];
          for (size_t q = 0; q < Wd * Hd; q++){
              if (mode == 0){ rays++; if (px[q*4+3] > 200) hits++; }
              else if (px[q*4+3] > 200){ rays++; if (px[q*4+2] > 80 || px[q*4+1] > 8) hits++; } }
          free(px); }
        fprintf(stderr, "[RT] %-34s: best %.2f ms, mean %.2f ms  (%zu rays, %.0f%% hits, %.0f M rays/s)\n",
                mn2[mode], best, sum / (N - 1), rays, rays ? 100.0 * hits / rays : 0.0,
                best > 0 ? rays / best / 1000.0 : 0.0);
        dump_texture(outp, 99001 + mode);
    }
    fprintf(stderr, "[RT] ===== end =====\n\n");
    }
}

/* ===== HD: RAY-TRACED REFLECTIONS (HD_RT=0 turns it off -> falls back to SSR) ==========
 * Every frame, as soon as the main scene ends (in place of the SSR composite):
 *   1) CAPTURE (GPU): for every draw of the scene the game's OWN vertex shader
 *      runs once more as a compute kernel (vsemu.c vsk_N) and the world position +
 *      color + texture coordinates + fog are written to a buffer.  No draws are
 *      skipped: road, skinned models, all of them.
 *   2) ACCELERATION STRUCTURE: rebuilt every frame with all triangles (measured:
 *      1.7 ms for ~19 thousand triangles).
 *   3) REFLECTION: a ray for road and car pixels; on a hit the math of the game's
 *      pixel shader (ps 2/10: 2*texture*color; ps 4/6: with the headlight term;
 *      car: body texture), the alpha test and fog are applied.
 *      Without a hit the game's own reflection (R0) stays.
 * Material textures are sampled through a bindless table (argument buffer). */
#define RT_MAXV   (600u * 1024u)     /* instant capture: vertices */
/* The static store has two parts: BASE + NEW (see STATIC STORE: BASE + NEW). */
#define RT_SMAXV  (480u * 1024u)     /* static store: vertices */
#define RT_RMAXV  (96u * 1024u)      /* new region: promotions since the last merge (32 thousand triangles) */
#define RT_SMAXE  8192u              /* static material */
#define RT_MAXREC 8192u
#define RT_MAXTEX 512u
typedef struct { uint32_t rawoff, koff, first, mat, ib; int32_t base; uint32_t isz, pad; } HDRecC;
typedef struct { uint32_t tex0, tex1, tex2, flags; float c0[4]; float fogc[4]; float prm[4]; float lw[16]; } HDMatC;
static int g_rt = -1;
static id<MTLComputePipelineState> g_hd_vsk[HD_MAXVSI];
static id<MTLBuffer> g_rt_pos, g_rt_attr, g_rt_scratch, g_rt_item[3], g_rt_rec[3], g_rt_mat[3], g_rt_arg[3];
/* --- INSTANT STRUCTURE: IN-PLACE UPDATE (refit) -------------------------------
 * The structure of moving geometry was rebuilt from scratch every frame.  If the
 * same draws came in the same order and with the same triangle counts as in the
 * previous frame, the triangle layout is the same; only the positions change.
 * Then the tree isn't rebuilt, its boxes are fitted to the new positions (refit, a
 * small fraction of a build).  Since motion loosens the tree over time, it is still
 * rebuilt from scratch every RT_REFIT_MAX frames.  HD_RT_REFIT=0 old behavior. */
#define RT_REFIT_MAX 20u
/* POSITION CHECK: the signature (VS, triangle count, texture, geometry id, place) is
 * the same, but if other COPIES of the same model (palm, pole, the same car model)
 * were drawn in a different order, triangle i is somewhere else in this frame: refit
 * keeps the tree and the boxes cover half the scene.  Measured (Free Run, under an
 * overpass): the ray kernel at full resolution 1.5 -> 15-21 ms, 1.6 ms with
 * HD_RT_REFIT=0.  Now each draw's world position (translation from the VS matrix) is
 * compared with the last full build; if any moved more than RT_REFIT_MOVE units
 * (~a car length) it is rebuilt from scratch: a copy that swapped places at once, a
 * fast car every ~10 frames (Free Run: Mcycles 10 units 9.84, 1 unit 9.79, without
 * refit 10.09 -- the refit gain ~3%, the threshold difference within measurement
 * noise).  HD_RT_REFIT_MOVE=<units>. */
#define RT_REFIT_MOVE 4.f
static float g_rt_dp[MAXD][3]; static uint8_t g_rt_dpok[MAXD];      /* this frame's draws: translation */
static float g_rt_bpos[RT_MAXREC][3]; static uint8_t g_rt_bpok[RT_MAXREC]; static unsigned g_rt_bn;  /* the last full build */
static _Atomic unsigned long long g_rt_nmoved;  /* same signature, position shifted: rebuild from scratch */
static int g_hd_dump_now;                       /* (below) this frame was selected with DUMPFRAME */
static uint64_t g_rt_dsig;                      /* the signature of the last built/refitted layout */
static uint32_t g_rt_dsince = 0xFFFFFFFFu;      /* frames since the last full build */
static _Atomic unsigned long long g_rt_nbuild, g_rt_nrefit;
static int hd_rt_refit_on(void){
    static int v = -1;
    if (v < 0){ const char *e = ENVC("HD_RT_REFIT"); v = (e && *e == '0') ? 0 : 1; }
    return v;
}
static MTLAccelerationStructureUsage hd_rt_dyn_usage(void){
    return MTLAccelerationStructureUsagePreferFastBuild | (hd_rt_refit_on() ? MTLAccelerationStructureUsageRefit : 0);
}
/* Static parts: fast build + refit (the evicted ones drop out with refit).  HD_RT_SQ=1:
 * a quality build (build ~2x more expensive; the ray pass was measured, no difference). */
static MTLAccelerationStructureUsage hd_rt_part_usage(void){
    static int q = -1;
    if (q < 0){ const char *e = ENVC("HD_RT_SQ"); q = (e && *e == '1') ? 1 : 0; }
    return (q ? MTLAccelerationStructureUsageNone : MTLAccelerationStructureUsagePreferFastBuild)
           | MTLAccelerationStructureUsageRefit;
}
/* ---- STATIC STORE: SPATIAL GROUPS + NEW -----------------------------------
 * History: the static store was a single sub-structure and was rebuilt FROM SCRATCH
 * at quality every 30 frames (~10 ms in a long session, a frame missed every 30
 * frames).  8 equal time-ordered slots removed the spike, but the slots overlapped
 * in the same region (ray kernel +48%; frames over 12 ms in a user session
 * 1.5% -> 5.1%).  BASE + NEW brought the kernel back, but the base (~130 thousand
 * triangles) was built in one go at every merge (every ~10 s in free driving):
 * GPU +5 ms in that frame (measured, 12-13 ms frames at all 15 merges).  Now:
 *  - The store has TWO HALVES (ping-pong).  In the active half: GROUPS [0, new.lo) + NEW.
 *  - GROUPS: the live entries are split by their centers into at most RT_NG spatial
 *    groups (the largest group at the triangle-weighted median of its longest
 *    axis), target ~RT_GTRI triangles.  Groups are disjoint in space: a ray mostly
 *    walks a single group's tree, with no overlap as with the time-ordered slots.
 *  - MERGE (when the new region fills up): the live ones are laid out group by group
 *    into the other half on the GPU (hd_rt_gather), the new groups' structures are
 *    built ONE PER FRAME (~16 thousand triangles, fast mode, overlapped with the
 *    scene), and when all are done the switch happens in a single frame: groups +
 *    an empty new region.  During a merge promotion and eviction wait (the geometry
 *    stays in the instant capture), the old groups stay in the top-level structure;
 *    the old half isn't touched until the next merge (frames in flight may read it;
 *    >= 4 frames between merges).
 *  - An evicted entry's triangles are collapsed onto its first vertex and its group
 *    is refitted (at most one group per frame, once every RT_BREFIT frames per
 *    group); the material slot is freed once that part's refit / build is encoded.
 *  - Store full: at a merge the OLDEST (birth frame) entries are dropped.
 * Top-level structure = instant + groups + new; the kernel knows the part of a hit
 * from the instance table (IB: instance -> absolute base in the store). */
#define RT_BREFIT 15u
#define RT_NG     12u                    /* maximum number of groups */
#define RT_GTRI   16384u                 /* target group size, triangles (HD_RT_GTRI) */
#define RT_NINST  (2u + RT_NG)           /* instant + groups + new */
static unsigned rt_gtri(void);
typedef struct {
    id<MTLAccelerationStructure> as; id<MTLBuffer> scratch;
    uint32_t lo, built;                  /* store vertex index (absolute); vertex count in the structure */
    uint32_t built_frame, refit_frame;
    uint8_t  dirty, refit;
} RTPart;
static RTPart g_rt_recent;
static RTPart g_rt_grp[RT_NG], g_rt_ngrp[RT_NG];          /* active groups; those built during a merge */
static unsigned g_rt_ng, g_rt_nng, g_rt_mnext;
static uint32_t g_rt_half, g_rt_scur, g_rt_sdead;          /* active half; used (absolute), dead */
static int g_rt_mstate;                                     /* 1: a merge is in progress */
static uint32_t g_rt_mgen, g_rt_mend, g_rt_lastsw;
static id<MTLBuffer> g_rt_gscr_b, g_rt_gscr_r; static NSUInteger g_rt_gscr_len;   /* group build / refit scratch */
static id<MTLBuffer> g_rt_spos, g_rt_sattr, g_rt_tscratch, g_rt_inst[3];
static id<MTLComputePipelineState> g_rt_colk, g_rt_gatk;   /* collapsing an evicted entry; merge copy */
static uint32_t g_rt_ib[RT_NINST]; static unsigned g_rt_nib;   /* instance -> store base (instant: ~0) */
static id<MTLAccelerationStructure> g_rt_tl[RT_NINST]; static unsigned g_rt_tln;   /* those in the top-level structure (useResource) */
static _Atomic unsigned long long g_rt_srefits, g_rt_sdrop, g_rt_merges;
static id<MTLAccelerationStructure> g_rt_as, g_rt_tlas;
static int g_rt_cache = 1, g_rt_only_static;
static id<MTLRenderPipelineState> g_rt_pso, g_rt_pso_ms;
static id<MTLComputePipelineState> g_rt_kern, g_rt_kern2;   /* 2: the RT_V2 variable */
static id<MTLTexture> g_rt_out;
static id<MTLArgumentEncoder> g_rt_argenc;
static unsigned g_rt_ring;
static id<MTLTexture> g_rt_texs[RT_MAXTEX];
static uint32_t g_rt_tstamp[MAXTEX]; static uint16_t g_rt_tslot[MAXTEX]; static uint32_t g_rt_stamp;
static _Atomic unsigned long long g_rt_frames, g_rt_tris, g_rt_draws, g_rt_skipk, g_rt_over;
static int hd_rt_on(void){
    if (g_rt < 0){ const char *e = ENVC("HD_RT");
        g_rt = (e && *e == '0') ? 0 : 1;
        if (g_rt && g_dev && !g_dev.supportsRaytracing) g_rt = 0;
        if (@available(macOS 13.0, *)) {} else g_rt = 0;      /* acceleration structure in the render pass */
        fprintf(stderr, "[HD] ray-traced reflections: %s\n", g_rt ? "ON" : "OFF"); }
    return g_rt;
}
int metal_hd_rt_enabled(void){ return g_dev ? hd_rt_on() : 0; }   /* enhance.c: road reflection scale */
static id<MTLComputePipelineState> g_rt_fixk;   /* vertex check (HD_RT_FIX=0 turns it off) */
static id<MTLBuffer> g_rt_fixcnt;               /* [0] NaN/infinite triangles, [1] triangles too far away (total) */
static NSString *kRTFSrc = @
"#include <metal_stdlib>\n"
"#ifndef RT_V2\n"
"#define RT_V2 0\n"
"#endif\n"
"using namespace metal;\n"
"using namespace raytracing;\n"
"struct FSO { float4 pos [[position]]; };\n"
"vertex FSO hd_fs_vert(uint vid [[vertex_id]]) {\n"
"  float2 p = float2(float((vid << 1) & 2), float(vid & 2));\n"
"  FSO o; o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0); return o; }\n"
"struct SSRP { float4x4 vp; float4x4 ivp; float4 cam; float4 prm; float4 sz; float4 cam2;\n"
"              float4 cb0; float4 cb1; float4 cb2; float4 fogp; float4 rt2; };\n"
"struct HDMat { uint tex0; uint tex1; uint tex2; uint flags; float4 c0; float4 fogc; float4 prm;\n"
"               float4 l0; float4 l1; float4 l2; float4 l3; };\n"
"struct HDAttr { float4 col0; float4 uv1; float4 uv2; float2 uv0; float fog; float pad; };\n"
"struct HDTex { array<texture2d<float>, 512> t; };\n"
"static float2 hd_proj(float4 q, uint mode) {\n"
"  float d = mode == 2u ? q.w : (mode == 1u ? q.z : 1.0);\n"
"  return q.xy / (abs(d) < 1e-6 ? 1e-6 : d); }\n"
/* Rays are cast in a COMPUTE kernel.  The same code in a fragment shader was
 * measured: GPU ~14 ms/frame (the game got GPU-bound, ~74 fps); in the kernel ~2 ms. */
"kernel void hd_rt_kern(uint2 tid0 [[thread_position_in_grid]],\n"
"    texture2d<float, access::write> outp [[texture(0)]], depth2d<float> dep [[texture(1)]],\n"
"    texture2d<float> g1 [[texture(2)]], texture2d<float> g2 [[texture(3)]], texture2d<float> g3 [[texture(4)]],\n"
"    constant SSRP &S [[buffer(0)]], instance_acceleration_structure acc [[buffer(1)]],\n"
"    device const HDMat *MAT [[buffer(3)]],\n"
"    device const HDAttr *ATT [[buffer(4)]], constant HDTex &TX [[buffer(6)]],\n"
"    device const HDAttr *SATT [[buffer(7)]], constant uint *IB [[buffer(8)]]) {\n"
/* S.cb2.w = 0 full; 1/2 = that frame's half of the checkerboard (see HALF RAYS) */
"  uint par = uint(S.cb2.w + 0.5);\n"
"  uint2 tid = par == 0u ? tid0 : uint2(tid0.x * 2u + ((tid0.y + par) & 1u), tid0.y);\n"
/* S.cb0.w = scale (1 or 2): output (W/scale, H/scale); every output pixel represents
 * the (tid * scale) pixel of the full-resolution G-buffer. */
"  uint sc = uint(max(S.cb0.w, 1.0));\n"
"  uint2 px = tid * sc;\n"
"  if (px.x >= uint(S.sz.x) || px.y >= uint(S.sz.y)) return;\n"
"  float4 gb = g1.read(px); float cls = GB_CLS(gb);\n"
"  bool road = GB_ROAD(cls);\n"
"  bool car  = GB_CAR(cls);\n"
"  float d = dep.read(px);\n"
"  if ((!road && !car) || d >= 1.0) { outp.write(float4(0.0), tid); return; }\n"
/* WEIGHT THRESHOLD (see hd_composite): the composite changes the pixel by
 * B*(R_new - R0); if the game's reflection weight B is below the threshold the ray
 * is invisible */
"  if (S.rt2.z > 0.0) { float3 bw = GB_B(gb, g2.read(px));\n"
"    if (max(bw.r, max(bw.g, bw.b)) < S.rt2.z) { outp.write(float4(0.0), tid); return; } }\n"
"  float2 uv = (float2(px) + 0.5) / S.sz.xy;\n"
"  float4 wp = S.ivp * float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, d, 1.0);\n"
"  float3 P = wp.xyz / wp.w;\n"
/* DISTANCE LIMIT (see hd_composite): no ray on far surfaces, the game's own
 * reflection (R0) stays; a soft transition before the limit */
"  float rfade = 1.0;\n"
"  if (S.rt2.x > 0.0) { float dc = length(P - S.cam.xyz);\n"
"    if (dc >= S.rt2.x) { outp.write(float4(0.0), tid); return; }\n"
"    rfade = 1.0 - smoothstep(S.rt2.x * S.rt2.y, S.rt2.x, dc); }\n"
"  float3 V = normalize(P - S.cam.xyz);\n"
"  float3 Rd, N = float3(0.0, 0.0, 1.0);\n"
"  if (road) { Rd = reflect(V, N); if (Rd.z <= 0.0) { outp.write(float4(0.0), tid); return; } }\n"
"  else {\n"
"    float3 gd = GB_DIR(g3.read(px));\n"
"    if (dot(gd, gd) < 1e-6) { outp.write(float4(0.0), tid); return; }\n"
"    Rd = normalize(gd.x * S.cb0.xyz + gd.y * S.cb1.xyz + gd.z * S.cb2.xyz);\n"
"    N = normalize(Rd - V);\n"
"    if (dot(N, V) > -0.02) { outp.write(float4(0.0), tid); return; }\n"
"  }\n"
"  constexpr sampler rs(filter::linear, mip_filter::linear, address::repeat);\n"
"  constexpr sampler cs(filter::linear, address::clamp_to_edge);\n"
"  float maxd = S.cam2.y;\n"
"  float3 org = P + N * 0.05;\n"
"  float tot = 0.0, conf = 0.0; float3 rc = float3(0.0);\n"
"  intersector<triangle_data, instancing> isec;\n"
"  isec.assume_geometry_type(geometry_type::triangle);\n"
"  isec.force_opacity(forced_opacity::opaque);\n"
/* RT_V2 (see hd_rt_init): instance transforms are identity (instant/base/new are
 * always identical) -> skips converting the trace ray into instance space */
"#if RT_V2\n"
"  isec.assume_identity_transforms(true);\n"
"#endif\n"
"  for (int k = 0; k < 5; k++) {\n"
"    ray r; r.origin = org; r.direction = Rd; r.min_distance = 0.01; r.max_distance = maxd - tot;\n"
"    intersection_result<triangle_data, instancing> h = isec.intersect(r, acc);\n"
"    if (h.type == intersection_type::none) break;\n"
/* instance table (see STATIC STORE SLOTS): ~0 = instant capture, otherwise the
 * slot's first vertex in the store; the triangle number is within the slot */
"    uint ib = IB[h.instance_id];\n"
"    bool st = ib != 0xFFFFFFFFu;\n"
"    uint g0 = (st ? ib : 0u) + h.primitive_id * 3u;\n"
"    tot += h.distance;\n"
"    if (S.cam2.w > 1.5) { rc = float3(1.0 / (1.0 + 0.02 * tot)); conf = 1.0; break; }\n"
"    float2 b = h.triangle_barycentric_coord; float w0 = 1.0 - b.x - b.y;\n"
"    HDAttr a0, a1, a2;\n"
"    if (st) { a0 = SATT[g0]; a1 = SATT[g0 + 1u]; a2 = SATT[g0 + 2u]; }\n"
"    else    { a0 = ATT[g0];  a1 = ATT[g0 + 1u];  a2 = ATT[g0 + 2u]; }\n"
"    HDMat m = MAT[uint(a0.pad + 0.5)];\n"
"    float4 col = a0.col0 * w0 + a1.col0 * b.x + a2.col0 * b.y;\n"
"    float4 uv1 = a0.uv1 * w0 + a1.uv1 * b.x + a2.uv1 * b.y;\n"
"    float2 uv0 = a0.uv0 * w0 + a1.uv0 * b.x + a2.uv0 * b.y;\n"
"    float fog = a0.fog * w0 + a1.fog * b.x + a2.fog * b.y;\n"
/* static store: the recorded fog is relative to the camera at promotion time;
 * recompute it with the game's formula
 * (vs_4: fog = clamp((end - depth) * scale, low, 1)) */
"    if ((m.flags & 0x10000u) != 0u) {\n"
"      float3 hp = org + Rd * h.distance;\n"
"      float zv = (S.vp * float4(hp, 1.0)).w;\n"
"      fog = S.fogp.w > 0.5 ? clamp((S.fogp.x - zv) * S.fogp.y, S.fogp.z, 1.0) : 1.0;\n"
"    }\n"
"    bool mcar = (m.flags & 2u) != 0u;\n"
"    float lod = clamp(log2(1.0 + tot * S.cam2.z), 0.0, 5.0);\n"
"    float4 tc = S.cam2.w > 0.5 ? float4(1.0) : TX.t[mcar ? m.tex1 : m.tex0].sample(rs, mcar ? uv1.xy : uv0, level(lod));\n"
/* alpha test (trees, fences): the ray continues through the surface */
"    if ((m.flags & 1u) != 0u && tc.a * col.a < m.prm.x) { org += Rd * (h.distance + 0.02); continue; }\n"
"    float3 lit = col.rgb;\n"
/* The static copy is stored as an UNLIT BASE.  If the part is drawn lit by
 * headlights in this frame (0x20000), the headlight term is built at the hit point
 * with the draw's CURRENT light matrix (world -> light, m.l0..l3):
 * oT1 = p.L, oT2.x = scale*q.w + offset. */
"    if ((m.flags & 0x20000u) != 0u && S.cam2.w < 0.5) {\n"
"      float3 hp2 = org + Rd * h.distance;\n"
"      float4 q1 = hp2.x * m.l0 + hp2.y * m.l1 + hp2.z * m.l2 + m.l3;\n"
"      float4 q2 = float4(q1.w * m.prm.y + m.prm.z, 0.0, 1.0, 1.0);\n"
"      float3 l1 = TX.t[m.tex1].sample(cs, hd_proj(q1, (m.flags >> 8) & 3u), level(0.0)).rgb;\n"
"      float3 l2 = TX.t[m.tex2].sample(cs, hd_proj(q2, (m.flags >> 10) & 3u), level(0.0)).rgb;\n"
"      lit = l1 * l2 * m.c0.rgb + col.rgb;\n"
"    }\n"
"    if ((m.flags & 4u) != 0u && S.cam2.w < 0.5) {\n"   /* ps 4/6: headlight texture x falloff ramp */
"      float4 uv2 = a0.uv2 * w0 + a1.uv2 * b.x + a2.uv2 * b.y;\n"
"      float3 l1 = TX.t[m.tex1].sample(cs, hd_proj(uv1, (m.flags >> 8) & 3u), level(0.0)).rgb;\n"
"      float3 l2 = TX.t[m.tex2].sample(cs, hd_proj(uv2, (m.flags >> 10) & 3u), level(0.0)).rgb;\n"
"      lit = l1 * l2 * m.c0.rgb + col.rgb;\n"
"    }\n"
"    float3 rgb = saturate(2.0 * tc.rgb * lit);\n"
"    rgb = mix(m.fogc.rgb, rgb, mix(1.0, saturate(fog), m.fogc.a));\n"
"    rc = rgb; conf = 1.0 - smoothstep(0.85, 1.0, tot / maxd);\n"
"    break;\n"
"  }\n"
"  outp.write(float4(rc, conf * rfade), tid);\n"
"}\n"
/* VERTEX CHECK: if a captured triangle has a NaN / infinite / absurdly far
 * vertex, degenerate the triangle.  A SINGLE NaN triangle poisons the
 * acceleration structure's boxes at the root: rays can't cull any node, tracing
 * gets ~15x slower (measured: at 3024x1964 plain tracing 58 ms instead of 4 ms,
 * in ~8% of runs; which draw produces it depends on randomly chosen content). */
"struct HDFix { uint ntri; float lim; uint first; uint slot; };\n"
"kernel void hd_rt_fix(uint tid [[thread_position_in_grid]],\n"
"    device packed_float3 *P [[buffer(0)]], device atomic_uint *CNT [[buffer(1)]],\n"
"    constant HDFix &F [[buffer(2)]]) {\n"
"  if (tid >= F.ntri) return;\n"
"  uint v = F.first + tid * 3u;\n"
/* F.lim < 0: DIAGNOSTIC (HD_RT_POISON): instead of checking, move one vertex of the first triangle to -F.lim */
"  if (F.lim < 0.0) { if (tid == 0u) P[v] = float3(-F.lim, P[v].y, P[v].z); return; }\n"
"  float3 a = P[v], b = P[v + 1u], c = P[v + 2u];\n"
"  float3 m = max(max(abs(a), abs(b)), abs(c));\n"
"  bool nan = any(isnan(a)) || any(isnan(b)) || any(isnan(c)) || any(isinf(a)) || any(isinf(b)) || any(isinf(c));\n"
"  bool far = !nan && max(m.x, max(m.y, m.z)) > F.lim;\n"
"  if (nan || far) {\n"
"    P[v] = float3(0.0); P[v + 1u] = float3(0.0); P[v + 2u] = float3(0.0);\n"
"    atomic_fetch_add_explicit(&CNT[nan ? 0 : 1], 1u, memory_order_relaxed);\n"
"    atomic_fetch_add_explicit(&CNT[2u + F.slot * 2u + (nan ? 0u : 1u)], 1u, memory_order_relaxed);\n"
"  }\n"
"}\n"
/* EVICTION: collapse the entry's triangles onto its own first vertex -- area 0 (rays
 * don't hit), the slot's boxes stay in place; then the slot is refitted. */
/* MERGE COPY: one workgroup per chunk (source, destination, count <= 256);
 * position (12 bytes) + attributes (64 bytes) into the other half */
"struct HDGat { uint src; uint dst; uint n; };\n"
"kernel void hd_rt_gather(uint tid [[thread_position_in_grid]], device packed_float3 *P [[buffer(0)]],\n"
"    device float4 *A [[buffer(1)]], device const HDGat *G [[buffer(2)]], constant uint &NG [[buffer(3)]]) {\n"
"  uint c = tid >> 8u, i = tid & 255u;\n"
"  if (c >= NG || i >= G[c].n) return;\n"
"  uint s = G[c].src + i, d = G[c].dst + i;\n"
"  P[d] = P[s];\n"
"  for (uint k = 0u; k < 4u; k++) A[d * 4u + k] = A[s * 4u + k];\n"
"}\n"
"struct HDCol { uint first; uint n; };\n"
"kernel void hd_rt_collapse(uint tid [[thread_position_in_grid]], device packed_float3 *P [[buffer(0)]],\n"
"    constant HDCol *R [[buffer(1)]], constant uint &NR [[buffer(2)]]) {\n"
"  if (tid >= NR) return;\n"
"  uint f = R[tid].first; float3 p = P[f];\n"
"  for (uint i = 1u; i < R[tid].n; i++) P[f + i] = p;\n"
"}\n"
/* composite: color + B * (R_new - R0) */
"static float4 rtc(uint2 px, float4 c, depth2d<float> dep, texture2d<float> g1,\n"
"                  texture2d<float> g2, texture2d<float> rt, constant SSRP &S) {\n"
"  float4 gb = g1.read(px);\n"
"  int dbg = int(S.cam.w) - 1;\n"
"  float cls = GB_CLS(gb);\n"
"  bool road = GB_ROAD(cls);\n"
"  bool car  = GB_CAR(cls);\n"
"  if (!road && !car) return dbg > 0 ? float4(c.rgb * 0.25, c.a) : c;\n"
"  float4 r;\n"
"  uint sc = uint(max(S.cb0.w, 1.0));\n"
"  if (sc <= 1u) r = rt.read(px);\n"
"  else {\n"                 /* depth- and class-aware bilinear upscaling */
"    float2 f = float2(px) / float(sc); uint2 b0 = uint2(floor(f)); float2 fr = f - floor(f);\n"
"    uint2 lim = uint2(rt.get_width() - 1, rt.get_height() - 1), fl = uint2(S.sz.xy) - 1u;\n"
"    float zc = S.prm.y / (dep.read(px) - S.prm.x);\n"
"    float4 acc = float4(0.0); float ws = 0.0;\n"
"    for (uint j = 0u; j < 4u; j++) {\n"
"      uint2 o = uint2(j & 1u, j >> 1u); uint2 q = min(b0 + o, lim); uint2 qf = min(q * sc, fl);\n"
"      float zq = S.prm.y / (dep.read(qf) - S.prm.x);\n"
"      float same = abs(GB_CLS(g1.read(qf)) - cls) < 0.3 ? 1.0 : 0.0;\n"
"      float w = (o.x == 1u ? fr.x : 1.0 - fr.x) * (o.y == 1u ? fr.y : 1.0 - fr.y);\n"
"      w = (w + 0.02) * same / (1.0 + 60.0 * abs(zq - zc) / max(zc, 0.5));\n"
"      acc += rt.read(q) * w; ws += w; }\n"
"    r = ws > 0.0 ? acc / ws : float4(0.0);\n"
"  }\n"
"  if (dbg == 4) return float4(r.a > 0.0 ? r.rgb : float3(0.0, 0.0, 0.35), 1.0);\n"
"  if (dbg == 1) return float4(road ? 0.5 : 0.0, r.a, r.a > 0.0 ? (road ? 0.0 : 1.0) : 0.6, 1.0);\n"
"  if (r.a <= 0.0) return c;\n"
"  float4 gv = g2.read(px);\n"
"  float3 R0 = GB_R0(gb, gv);\n"
"  float3 Rn = mix(R0, r.rgb, r.a * S.prm.z);\n"
"  return float4(c.rgb + GB_B(gb, gv) * (Rn - R0), c.a);\n"
"}\n"
"fragment float4 hd_rtc_frag(FSO in [[stage_in]],\n"
"    texture2d<float> scene [[texture(0)]], depth2d<float> dep [[texture(1)]],\n"
"    texture2d<float> g1 [[texture(2)]],\n"
"    texture2d<float> g2 [[texture(3)]], texture2d<float> rt [[texture(5)]],\n"
"    constant SSRP &S [[buffer(0)]]) {\n"
"  uint2 px = uint2(in.pos.xy);\n"
"  return rtc(px, scene.read(px), dep, g1, g2, rt, S);\n"
"}\n"
/* copy-free: the pixel's own color from tile memory (programmable blending) */
"fragment float4 hd_rtc_frag_fb(FSO in [[stage_in]], float4 c [[color(0)]],\n"
"    depth2d<float> dep [[texture(1)]], texture2d<float> g1 [[texture(2)]],\n"
"    texture2d<float> g2 [[texture(3)]], texture2d<float> rt [[texture(5)]],\n"
"    constant SSRP &S [[buffer(0)]]) {\n"
"  return rtc(uint2(in.pos.xy), c, dep, g1, g2, rt, S);\n"
"}\n";
/* --- COPY-FREE COMPOSITE ----------------------------------------------------
 * The ray tracing composite reads only its OWN color at every pixel.  Previously
 * the whole screen was first copied out (0.34 ms GPU at 3024x1964, ~46 MB of
 * traffic), then the whole screen was rewritten.  On Apple GPUs the pass can
 * load the screen and read the pixel's color from tile memory: no copy needed.
 * SSR (samples other pixels) and the single MSAA mode stay on the old path.
 * HD_RT_FB=0 old. */
static id<MTLRenderPipelineState> g_rt_pso_fb;
static int hd_rt_fb_on(void){
    static int v = -1;
    if (v < 0){ const char *e = ENVC("HD_RT_FB"); v = !(e && *e == '0') && [g_dev supportsFamily:MTLGPUFamilyApple1]; }
    return v;
}
static void hd_rt_init(void){
    if (!hd_rt_on() || g_rt_pso) return;
    NSError *e = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:hd_gb_src(kRTFSrc) options:nil error:&e];
    if (!lib){ fprintf(stderr, "[HD] RT shader could not be compiled: %s\n", [[e localizedDescription] UTF8String]); g_rt = 0; return; }
    id<MTLFunction> ff = [lib newFunctionWithName:@"hd_rtc_frag"];
    id<MTLFunction> kf = [lib newFunctionWithName:@"hd_rt_kern"];
    g_rt_kern = [g_dev newComputePipelineStateWithFunction:kf error:&e];
    /* Kernel VARIANT (RT_V2, diagnostic): same source + assume_identity_transforms.
     * Measured 2026-10-20 (HD_RTPROBE2, same data, 7 frames): no difference (+-2%).
     * Compiled with HD_RT_V2=1; ABTEST=RTV2 compares in the same run. */
    { const char *ev = ENVC("HD_RT_V2");
      if (ev && *ev == '1'){
          NSError *e2 = nil;
          id<MTLLibrary> l2 = [g_dev newLibraryWithSource:[@"#define RT_V2 1\n" stringByAppendingString:hd_gb_src(kRTFSrc)] options:nil error:&e2];
          id<MTLFunction> k2 = l2 ? [l2 newFunctionWithName:@"hd_rt_kern"] : nil;
          if (k2) g_rt_kern2 = [g_dev newComputePipelineStateWithFunction:k2 error:&e2];
          if (!g_rt_kern2) fprintf(stderr, "[HD] RT kernel variant could not be built: %s\n", e2 ? [[e2 localizedDescription] UTF8String] : "?"); } }
    { id<MTLFunction> xf = [lib newFunctionWithName:@"hd_rt_fix"];
      if (xf) g_rt_fixk = [g_dev newComputePipelineStateWithFunction:xf error:&e];
      g_rt_fixcnt = [g_dev newBufferWithLength:(2 + 2 * HD_MAXVSI) * 4 options:MTLResourceStorageModeShared];
      memset([g_rt_fixcnt contents], 0, (2 + 2 * HD_MAXVSI) * 4);
      { const char *ev = ENVC("HD_RT_FIX"); if (ev && *ev == '0') g_rt_fixk = nil; } }
    if (!g_rt_kern){ fprintf(stderr, "[HD] RT kernel could not be built: %s\n", [[e localizedDescription] UTF8String]); g_rt = 0; return; }
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"hd_fs_vert"];
    pd.fragmentFunction = ff;
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    g_rt_pso = [g_dev newRenderPipelineStateWithDescriptor:pd error:&e];
    if (hd_rt_fb_on()){ MTLRenderPipelineDescriptor *pf = [pd copy];
        pf.fragmentFunction = [lib newFunctionWithName:@"hd_rtc_frag_fb"];
        NSError *ef = nil; g_rt_pso_fb = [g_dev newRenderPipelineStateWithDescriptor:pf error:&ef];
        if (!g_rt_pso_fb) fprintf(stderr, "[HD] copy-free composite could not be built: %s\n", [[ef localizedDescription] UTF8String]); }
    if (hd_msaa_n() > 1){ pd.rasterSampleCount = (NSUInteger)hd_msaa_n();
                          g_rt_pso_ms = [g_dev newRenderPipelineStateWithDescriptor:pd error:&e]; }
    if (!g_rt_pso){ fprintf(stderr, "[HD] RT pipeline could not be built: %s\n", [[e localizedDescription] UTF8String]); g_rt = 0; return; }
    g_rt_argenc = [kf newArgumentEncoderWithBufferIndex:6];
    g_rt_pos   = [g_dev newBufferWithLength:RT_MAXV * 12 options:MTLResourceStorageModePrivate];
    g_rt_attr  = [g_dev newBufferWithLength:RT_MAXV * 64 options:MTLResourceStorageModePrivate];
    g_rt_spos  = [g_dev newBufferWithLength:2u * RT_SMAXV * 12 options:MTLResourceStorageModePrivate];   /* two halves */
    g_rt_sattr = [g_dev newBufferWithLength:2u * RT_SMAXV * 64 options:MTLResourceStorageModePrivate];
    { id<MTLFunction> cf = [lib newFunctionWithName:@"hd_rt_collapse"];
      if (cf) g_rt_colk = [g_dev newComputePipelineStateWithFunction:cf error:&e];
      id<MTLFunction> gf = [lib newFunctionWithName:@"hd_rt_gather"];
      if (gf) g_rt_gatk = [g_dev newComputePipelineStateWithFunction:gf error:&e];
      if (!g_rt_colk || !g_rt_gatk){ fprintf(stderr, "[HD] RT store kernels could not be built\n"); g_rt = 0; return; } }
    for (int i = 0; i < 3; i++){
        g_rt_item[i] = [g_dev newBufferWithLength:RT_MAXV * 4 options:MTLResourceStorageModeShared];
        g_rt_rec[i]  = [g_dev newBufferWithLength:RT_MAXREC * sizeof(HDRecC) options:MTLResourceStorageModeShared];
        g_rt_mat[i]  = [g_dev newBufferWithLength:(RT_SMAXE + RT_MAXREC) * sizeof(HDMatC) options:MTLResourceStorageModeShared];
        g_rt_arg[i]  = [g_dev newBufferWithLength:g_rt_argenc.encodedLength options:MTLResourceStorageModeShared];
        g_rt_inst[i] = [g_dev newBufferWithLength:RT_NINST * sizeof(MTLAccelerationStructureInstanceDescriptor)
                                          options:MTLResourceStorageModeShared];
    }
    /* sub-structures and scratch buffers are allocated once for the largest size;
     * each has its own scratch so builds in the same encoder can run
     * concurrently (a shared scratch would race) */
    for (int k = 0; k < 2; k++){                  /* 0 instant, 1 new (groups at full size on merge) */
        MTLAccelerationStructureTriangleGeometryDescriptor *gd =
            [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
        gd.vertexBuffer = k ? g_rt_spos : g_rt_pos; gd.vertexStride = 12;
        gd.triangleCount = (k == 0 ? RT_MAXV : RT_RMAXV) / 3; gd.opaque = YES;
        MTLPrimitiveAccelerationStructureDescriptor *ad = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
        ad.geometryDescriptors = @[gd];
        ad.usage = k ? hd_rt_part_usage() : hd_rt_dyn_usage();
        MTLAccelerationStructureSizes sz = [g_dev accelerationStructureSizesWithDescriptor:ad];
        id<MTLAccelerationStructure> a = [g_dev newAccelerationStructureWithSize:sz.accelerationStructureSize];
        NSUInteger scs = sz.buildScratchBufferSize > sz.refitScratchBufferSize ? sz.buildScratchBufferSize : sz.refitScratchBufferSize;
        id<MTLBuffer> sc = [g_dev newBufferWithLength:scs options:MTLResourceStorageModePrivate];
        if (k == 0){ g_rt_as = a; g_rt_scratch = sc; }
        else { g_rt_recent.as = a; g_rt_recent.scratch = sc; }
    }
    for (int i = 0; i < 3; i++){
        MTLAccelerationStructureInstanceDescriptor *id2 = (MTLAccelerationStructureInstanceDescriptor *)[g_rt_inst[i] contents];
        memset(id2, 0, RT_NINST * sizeof *id2);
        for (unsigned k = 0; k < RT_NINST; k++){
            id2[k].transformationMatrix.columns[0] = MTLPackedFloat3Make(1, 0, 0);
            id2[k].transformationMatrix.columns[1] = MTLPackedFloat3Make(0, 1, 0);
            id2[k].transformationMatrix.columns[2] = MTLPackedFloat3Make(0, 0, 1);
            id2[k].transformationMatrix.columns[3] = MTLPackedFloat3Make(0, 0, 0);
            id2[k].options = MTLAccelerationStructureInstanceOptionOpaque;
            id2[k].mask = 0xFF; id2[k].accelerationStructureIndex = k; } }
    { MTLInstanceAccelerationStructureDescriptor *td = [MTLInstanceAccelerationStructureDescriptor descriptor];
      td.instancedAccelerationStructures = @[g_rt_as, g_rt_recent.as];
      td.instanceCount = RT_NINST; td.instanceDescriptorBuffer = g_rt_inst[0];
      MTLAccelerationStructureSizes sz = [g_dev accelerationStructureSizesWithDescriptor:td];
      g_rt_tlas = [g_dev newAccelerationStructureWithSize:sz.accelerationStructureSize];
      g_rt_tscratch = [g_dev newBufferWithLength:sz.buildScratchBufferSize ? sz.buildScratchBufferSize : 16
                                         options:MTLResourceStorageModePrivate]; }
    { const char *e2 = ENVC("HD_RT_CACHE"); g_rt_cache = (e2 && *e2 == '0') ? 0 : 1; }
    fprintf(stderr, "[HD] RT ready: instant at most %u triangles, static store 2 x %u triangles (at most %u spatial groups x ~%u triangles + new region %u, %s build), cache %s\n",
            RT_MAXV / 3, RT_SMAXV / 3, RT_NG, rt_gtri(), RT_RMAXV / 3,
            (hd_rt_part_usage() & MTLAccelerationStructureUsagePreferFastBuild) ? "fast" : "quality",
            g_rt_cache ? "ON" : "OFF");
}
static unsigned hd_rt_slot(unsigned tex, unsigned *nt){
    if (!tex || tex >= MAXTEX || !g_tex[tex] || g_tex_cube[tex] ||
        g_tex[tex].textureType != MTLTextureType2D) return 0;
    if (g_rt_tstamp[tex] == g_rt_stamp) return g_rt_tslot[tex];
    if (*nt >= RT_MAXTEX) return 0;
    g_rt_tstamp[tex] = g_rt_stamp; g_rt_tslot[tex] = (uint16_t)*nt;
    g_rt_texs[*nt] = g_tex[tex];
    return (*nt)++;
}
/* ---- STATIC SCENE CACHE --------------------------------------------
 * The game doesn't draw what the camera doesn't see; the scene behind the car
 * would be missing from the reflection.  Also, capturing the whole scene and
 * building the structure every frame pushed the GPU over the frame limit
 * (measured: 15.6 ms/frame).
 * Draw = (hash of the raw vertex data, world matrix W).  If the same draw is
 * seen in the same place for RT_PROMOTE frames it is STATIC: its captured
 * vertices are copied to the static store on the GPU (no readback to the CPU)
 * and the static sub-structure is rebuilt at most once every RT_SREBUILD
 * frames.  Static entries leave the instant capture; the instant sub-structure
 * keeps the cars + newly visible things.
 * Eviction: if it is inside the view but not drawn (moved/removed) or if it is
 * very far away.  Cars (cube-mapped ps) and skinned models never enter. */
#define RT_PROMOTE  12u
#define RT_SREBUILD 30u
#define RT_TABN     16384u
typedef struct { uint64_t rk, gh; float W[12]; float c[3], r; uint32_t first, n, seen, born, matidx;
                 uint32_t nfirst, mgen;   /* merge: place in the other half, merge id */
                 uint16_t sight; uint8_t state; uint8_t pad; uint8_t vol; } RTEnt;
/* state: 0 empty, 1 candidate, 2 static (waiting for the structure), 3 static, 9 deleted.  pad: copied to the store.
 * gh: geometry hash only (table key); rk: gh + texture + VS.  vol: volatility (below). */
static RTEnt g_rt_tab[RT_TABN];
static HDMatC g_rt_smatc[RT_SMAXE]; static uint32_t g_rt_smtex[RT_SMAXE][3];
static uint32_t g_rt_smn, g_rt_frame_no, g_rt_tomb;
static _Atomic unsigned long long g_rt_promo, g_rt_evict, g_rt_reset, g_rt_sbuilds, g_rt_dynv, g_rt_statv;
static int g_rt_need_clear = 1;          /* the new buffer's content is undefined */
static uint32_t g_rt_mfree[RT_SMAXE], g_rt_mfree_n;
static unsigned g_rt_ev_far, g_rt_ev_frus, g_rt_ev_cand;
static _Atomic unsigned long long g_rt_evsib;
static _Atomic unsigned long long g_rt_compact, g_rt_rehash;
#define RT_HIDX(gh) ((unsigned)((gh) * 0x9E3779B97F4A7C15ull >> 50) & (RT_TABN - 1))
/* Deleted entries (9) lengthen the chains at the end; re-lay the live ones in place. */
static void rt_rehash(void){
    static RTEnt tmp[RT_TABN];
    unsigned n = 0;
    for (unsigned q = 0; q < RT_TABN; q++) if (g_rt_tab[q].state && g_rt_tab[q].state != 9) tmp[n++] = g_rt_tab[q];
    memset(g_rt_tab, 0, sizeof g_rt_tab); g_rt_tomb = 0;
    for (unsigned q = 0; q < n; q++){
        unsigned i = RT_HIDX(tmp[q].gh);
        while (g_rt_tab[i].state) i = (i + 1) & (RT_TABN - 1);
        g_rt_tab[i] = tmp[q];
    }
    atomic_fetch_add(&g_rt_rehash, 1ull);
}
/* Evict an entry.  If static, its triangles go into the collapse list (collapsed
 * onto the first vertex on the GPU), its part (group or new) is marked for refit;
 * the material slot is freed once that part's refit / build is encoded.  If a
 * merge is in progress or the list is full the eviction is deferred (returns 0). */
#define RT_MAXCOL 512u                   /* collapse list via setBytes (<= 4 KB) */
static uint32_t g_rt_zr[RT_MAXCOL][2]; static unsigned g_rt_nzr;
#define RT_PREC 0xFEu                    /* pending material part: new region */
#define RT_PSW  0xFFu                    /* pending material part: the merge's switch */
static uint32_t g_rt_mpq[RT_SMAXE]; static uint8_t g_rt_mpqp[RT_SMAXE]; static unsigned g_rt_mpq_n;
static void rt_mpend_add(uint32_t mat, unsigned part){
    if (g_rt_mpq_n < RT_SMAXE){ g_rt_mpq[g_rt_mpq_n] = mat; g_rt_mpqp[g_rt_mpq_n] = (uint8_t)part; g_rt_mpq_n++; }
}
/* free the pending materials of parts with done[part] = 1 */
static void rt_mpend_release(const uint8_t *done){
    unsigned w = 0;
    for (unsigned q = 0; q < g_rt_mpq_n; q++){
        if (done[g_rt_mpqp[q]]){ if (g_rt_mfree_n < RT_SMAXE) g_rt_mfree[g_rt_mfree_n++] = g_rt_mpq[q]; }
        else { g_rt_mpq[w] = g_rt_mpq[q]; g_rt_mpqp[w] = g_rt_mpqp[q]; w++; }
    }
    g_rt_mpq_n = w;
}
static int rt_grp_of(uint32_t first){
    for (unsigned k = 0; k < g_rt_ng; k++)
        if (first >= g_rt_grp[k].lo && first < g_rt_grp[k].lo + g_rt_grp[k].built) return (int)k;
    return -1;
}
static int rt_evict(RTEnt *e){
    if (e->state == 2 || e->state == 3){
        if (g_rt_mstate) return 0;                     /* merge in progress: after the switch */
        if (e->pad){
            if (g_rt_nzr >= RT_MAXCOL) return 0;
            g_rt_zr[g_rt_nzr][0] = e->first; g_rt_zr[g_rt_nzr][1] = e->n; g_rt_nzr++;
        }
        g_rt_sdead += e->n;
        int k = e->pad ? rt_grp_of(e->first) : -1;
        if (k >= 0){ g_rt_grp[k].refit = 1; rt_mpend_add(e->matidx, (unsigned)k); }
        else { if (e->pad && e->first >= g_rt_recent.lo && e->first - g_rt_recent.lo < g_rt_recent.built) g_rt_recent.refit = 1;
               rt_mpend_add(e->matidx, RT_PREC); }
        atomic_fetch_add(&g_rt_evict, 1ull);
    }
    e->state = 9; g_rt_tomb++;
    return 1;
}
/* Live store entries, in store order */
static uint32_t g_rt_lv[RT_TABN]; static unsigned g_rt_nlv;
static int rt_lv_cmp(const void *a, const void *b){
    uint32_t x = g_rt_tab[*(const uint32_t *)a].first, y = g_rt_tab[*(const uint32_t *)b].first;
    return x < y ? -1 : x > y;
}
static void rt_live_sorted(void){
    g_rt_nlv = 0;
    for (unsigned q = 0; q < RT_TABN; q++){
        RTEnt *e = &g_rt_tab[q];
        if (e->state != 2 && e->state != 3) continue;
        if (!e->pad){                     /* never copied: back to candidate */
            e->state = 1; e->sight = 0;
            if (g_rt_mfree_n < RT_SMAXE) g_rt_mfree[g_rt_mfree_n++] = e->matidx;
            continue; }
        g_rt_lv[g_rt_nlv++] = q;
    }
    qsort(g_rt_lv, g_rt_nlv, sizeof g_rt_lv[0], rt_lv_cmp);
}
/* SPATIAL GROUPING: entries ix[0,n) into at most RT_NG groups; gs[g]..gs[g+1].
 * The largest (by triangles) group is split at the triangle-weighted median of
 * its centers along the longest axis; stops below the target or at RT_NG. */
static int g_rt_sax;
static int rt_ax_cmp(const void *a, const void *b){
    float x = g_rt_tab[*(const uint32_t *)a].c[g_rt_sax], y = g_rt_tab[*(const uint32_t *)b].c[g_rt_sax];
    return x < y ? -1 : x > y;
}
static int rt_born_cmp(const void *a, const void *b){
    uint32_t x = g_rt_tab[*(const uint32_t *)a].born, y = g_rt_tab[*(const uint32_t *)b].born;
    return x < y ? -1 : x > y;
}
static unsigned rt_partition(uint32_t *ix, unsigned n, unsigned gtri, unsigned *gs){
    unsigned ng = 1; gs[0] = 0; gs[1] = n;
    while (ng < RT_NG){
        int best = -1; uint64_t bt = 0;
        for (unsigned g = 0; g < ng; g++){
            if (gs[g + 1] - gs[g] < 2u) continue;
            uint64_t t = 0; for (unsigned q = gs[g]; q < gs[g + 1]; q++) t += g_rt_tab[ix[q]].n / 3u;
            if (t > gtri && t > bt){ bt = t; best = (int)g; }
        }
        if (best < 0) break;
        unsigned a = gs[best], b = gs[best + 1];
        float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
        for (unsigned q = a; q < b; q++){ const float *c = g_rt_tab[ix[q]].c;
            for (int d = 0; d < 3; d++){ if (c[d] < mn[d]) mn[d] = c[d]; if (c[d] > mx[d]) mx[d] = c[d]; } }
        float ex = mx[0] - mn[0], ey = mx[1] - mn[1], ez = mx[2] - mn[2];
        g_rt_sax = (ex >= ey && ex >= ez) ? 0 : (ey >= ez) ? 1 : 2;
        qsort(ix + a, b - a, sizeof ix[0], rt_ax_cmp);
        uint64_t acc = 0; unsigned m = a;
        while (m < b - 1u){ acc += g_rt_tab[ix[m]].n / 3u; m++; if (acc * 2u >= bt) break; }
        if (m <= a) m = a + 1u;
        memmove(gs + best + 2, gs + best + 1, (size_t)(ng - (unsigned)best) * sizeof gs[0]);
        gs[best + 1] = m; ng++;
    }
    return ng;
}
static MTLPrimitiveAccelerationStructureDescriptor *rt_part_desc(uint32_t lo, uint32_t nv){
    MTLAccelerationStructureTriangleGeometryDescriptor *gd = [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
    gd.vertexBuffer = g_rt_spos; gd.vertexBufferOffset = (NSUInteger)lo * 12; gd.vertexStride = 12;
    gd.triangleCount = nv / 3; gd.opaque = YES;
    MTLPrimitiveAccelerationStructureDescriptor *ad = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
    ad.geometryDescriptors = @[gd]; ad.usage = hd_rt_part_usage();
    return ad;
}
static unsigned rt_gtri(void){
    static unsigned v;
    if (!v){ const char *e = ENVC("HD_RT_GTRI"); v = e ? (unsigned)atoi(e) : RT_GTRI; if (v < 2048u) v = 2048u; }
    return v;
}
static uint32_t g_rt_gix[RT_TABN];
static void rt_merge_switch(uint32_t frame);
/* START A MERGE (at the start of the frame, before classification).  force_drop:
 * drop the oldest quarter (stress test). */
static void rt_merge_begin(id<MTLCommandBuffer> cb, uint32_t frame, int force_drop){
    static int lg = -1; if (lg < 0) lg = ENVC("HD_RT_LOG") != NULL;
    uint32_t hb = g_rt_half * RT_SMAXV, u0 = g_rt_scur - hb, d0 = g_rt_sdead, r0 = g_rt_scur - g_rt_recent.lo;
    rt_live_sorted();
    unsigned n = g_rt_nlv; uint64_t tot = 0;
    memcpy(g_rt_gix, g_rt_lv, (size_t)n * sizeof g_rt_gix[0]);
    for (unsigned q = 0; q < n; q++) tot += g_rt_tab[g_rt_gix[q]].n;
    /* the new half must fit groups + new region + margin; if not, the oldest go */
    uint64_t cap = (uint64_t)RT_SMAXV - RT_RMAXV - 30000u;
    int dropped = 0;
    if (force_drop || tot > cap){
        uint64_t want = force_drop ? tot - tot / 4u : cap * 3u / 4u;
        qsort(g_rt_gix, n, sizeof g_rt_gix[0], rt_born_cmp);
        unsigned sk = 0;
        while (sk < n && tot > want){
            RTEnt *e = &g_rt_tab[g_rt_gix[sk]];
            tot -= e->n; rt_mpend_add(e->matidx, RT_PSW);   /* visible in the old groups until the switch */
            e->state = 9; g_rt_tomb++; sk++;
        }
        memmove(g_rt_gix, g_rt_gix + sk, (size_t)(n - sk) * sizeof g_rt_gix[0]); n -= sk;
        dropped = 1; atomic_fetch_add(&g_rt_sdrop, 1ull);
    }
    unsigned gt = rt_gtri();
    if (tot / 3u > (uint64_t)gt * RT_NG) gt = (unsigned)(tot / 3u / RT_NG) + 1u;
    unsigned gs[RT_NG + 1];
    unsigned ng = n ? rt_partition(g_rt_gix, n, gt, gs) : 0;
    uint32_t cur = (1u - g_rt_half) * RT_SMAXV;
    unsigned nch = 0;
    for (unsigned q = 0; q < n; q++) nch += (g_rt_tab[g_rt_gix[q]].n + 255u) / 256u;
    id<MTLBuffer> chb = nch ? [g_dev newBufferWithLength:(NSUInteger)nch * 12 options:MTLResourceStorageModeShared] : nil;
    uint32_t *ch = chb ? (uint32_t *)[chb contents] : NULL; unsigned c = 0;
    uint32_t gen = ++g_rt_mgen;
    NSUInteger maxscr = 0;
    for (unsigned g = 0; g < ng; g++){
        RTPart *pt = &g_rt_ngrp[g];
        pt->scratch = nil; pt->lo = cur; pt->refit = 0; pt->dirty = 1;
        for (unsigned q = gs[g]; q < gs[g + 1]; q++){
            RTEnt *e = &g_rt_tab[g_rt_gix[q]];
            e->nfirst = cur; e->mgen = gen;
            for (uint32_t o = 0; o < e->n; o += 256u){
                ch[c * 3] = e->first + o; ch[c * 3 + 1] = cur + o; ch[c * 3 + 2] = e->n - o < 256u ? e->n - o : 256u; c++; }
            cur += e->n;
        }
        pt->built = cur - pt->lo;
        MTLAccelerationStructureSizes sz = [g_dev accelerationStructureSizesWithDescriptor:rt_part_desc(pt->lo, pt->built)];
        pt->as = [g_dev newAccelerationStructureWithSize:sz.accelerationStructureSize];
        NSUInteger s2 = sz.buildScratchBufferSize > sz.refitScratchBufferSize ? sz.buildScratchBufferSize : sz.refitScratchBufferSize;
        if (s2 > maxscr) maxscr = s2;
    }
    if (maxscr > g_rt_gscr_len){
        g_rt_gscr_b = [g_dev newBufferWithLength:maxscr options:MTLResourceStorageModePrivate];
        g_rt_gscr_r = [g_dev newBufferWithLength:maxscr options:MTLResourceStorageModePrivate];
        g_rt_gscr_len = maxscr;
    }
    if (c){
        id<MTLComputeCommandEncoder> ce = pp_compute(cb, "RT: merge copy (compute)");
        [ce setComputePipelineState:g_rt_gatk];
        [ce setBuffer:g_rt_spos offset:0 atIndex:0];
        [ce setBuffer:g_rt_sattr offset:0 atIndex:1];
        [ce setBuffer:chb offset:0 atIndex:2];
        uint32_t nc = c; [ce setBytes:&nc length:4 atIndex:3];
        [ce dispatchThreadgroups:MTLSizeMake(c, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
        [ce endEncoding];
    }
    if (lg) fprintf(stderr, "[RT] frame %ld: merge, store %u verts (dead %u, new %u)%s -> %u groups, %u verts\n",
                    atomic_load(&g_frames_drawn), u0, d0, r0, dropped ? ", oldest dropped" : "", ng,
                    cur - (1u - g_rt_half) * RT_SMAXV);
    g_rt_nng = ng; g_rt_mnext = 0; g_rt_mend = cur; g_rt_mstate = 1;
    atomic_fetch_add(&g_rt_merges, 1ull);
    if (!ng) rt_merge_switch(frame);                  /* empty store: nothing to build */
}
/* SWITCH: all the new groups are built (encoded this frame or earlier). */
static void rt_merge_switch(uint32_t frame){
    for (unsigned q = 0; q < RT_TABN; q++){
        RTEnt *e = &g_rt_tab[q];
        if (e->state != 2 && e->state != 3) continue;
        if (e->pad && e->mgen == g_rt_mgen){ e->first = e->nfirst; e->state = 3; }
        else { rt_mpend_add(e->matidx, RT_PSW); e->state = 9; g_rt_tomb++; }   /* did not enter the merge (shouldn't happen) */
    }
    for (unsigned k = 0; k < g_rt_ng; k++) g_rt_grp[k].as = nil;
    for (unsigned k = 0; k < g_rt_nng; k++){
        g_rt_grp[k] = g_rt_ngrp[k]; g_rt_ngrp[k].as = nil;
        g_rt_grp[k].dirty = 0; g_rt_grp[k].refit = 0; g_rt_grp[k].built_frame = g_rt_grp[k].refit_frame = frame;
    }
    g_rt_ng = g_rt_nng; g_rt_nng = 0;
    g_rt_half ^= 1u; g_rt_scur = g_rt_mend; g_rt_sdead = 0; g_rt_nzr = 0;
    g_rt_recent.lo = g_rt_scur; g_rt_recent.built = 0; g_rt_recent.dirty = 0; g_rt_recent.refit = 0;
    { uint8_t all[256]; memset(all, 1, sizeof all); rt_mpend_release(all); }   /* the old structures left the top-level structure */
    g_rt_mstate = 0; g_rt_lastsw = frame;
}
/* Promotion: make room for n vertices in the new region (absolute index; ~0 = no room / merge in progress). */
static int g_rt_want_merge;               /* there was no room: merge at the start of the next frame */
static uint32_t rt_alloc(unsigned n){
    if (g_rt_mstate) return 0xFFFFFFFFu;
    uint32_t end = g_rt_half * RT_SMAXV + RT_SMAXV;
    if (g_rt_scur + n > end || g_rt_scur + n - g_rt_recent.lo > RT_RMAXV){ g_rt_want_merge = 1; return 0xFFFFFFFFu; }
    uint32_t f = g_rt_scur; g_rt_scur += n; g_rt_recent.dirty = 1;
    return f;
}
static uint32_t rt_depot_used(void){ return g_rt_scur - g_rt_half * RT_SMAXV; }
static uint32_t rt_depot_dead(void){ return g_rt_sdead; }
static uint32_t g_rt_slit[RT_SMAXE];      /* static material: drawn lit by headlights in this frame */
static inline void rt_mat_of(const MDraw *dr, HDMatC *m){
    int base = dr->ps % PSREFL, cube = base >= PSCUBE, b = base % PSCUBE;
    memset(m, 0, sizeof *m);
    m->tex0 = dr->tex; m->tex1 = dr->tex1; m->tex2 = dr->tex2;        /* texture IDS */
    m->flags = (dr->alpha_ref > 0.f ? 1u : 0u) | (cube ? 2u : 0u) | ((b == 4 || b == 6) ? 4u : 0u)
             | ((((unsigned)(int)dr->proj[1] >> 4) & 3u) << 8) | ((((unsigned)(int)dr->proj[2] >> 4) & 3u) << 10);
    memcpy(m->c0, dr->c0, 16); memcpy(m->fogc, dr->fogv, 16);
    m->prm[0] = dr->alpha_ref;
}
/* Refresh a static entry's material from the current draw (so fog color etc.
 * stay current).  The static copy is stored as an UNLIT BASE: the headlight
 * (ps 4/6) moves with the player's car; if it were baked per vertex the light
 * pool would stay on the road after the car left (measured: after the rivals
 * pulled away the start line was bright in the reflection).  The game draws
 * nearly all of the near scene with the lit VS (255 draws at the start of a
 * race); recapturing all of them every frame would make the cache pointless
 * (GPU 7.1 -> 10.9 ms).  Instead the draw's light matrix (VS constants
 * c[K..K+3], object -> light) is converted to world space and written into the
 * material; the kernel builds the headlight term at the hit point.
 * If the part wasn't drawn lit in that frame (or wasn't drawn at all) there is
 * no term. */
static void rt_smat_update(const RTEnt *e, const MDraw *dr, const float *k, uint32_t frame){
    HDMatC m; rt_mat_of(dr, &m);
    int lit = (m.flags & 4u) != 0u, K = g_hd_vsi[dr->vsid].litr;
    m.flags = (m.flags & ~4u) | 0x10000u;                       /* 0x10000: fog is recomputed */
    if (lit && K >= 0 && (unsigned)(K + 4) * 16u <= dr->ksz - 16u){
        /* world = object*A + t  ->  object = (world - t)*A^-1;  q = object*LW,  LW[r][c] = c[K+c][r] */
        const float *W = e->W;
        double a00 = W[0], a01 = W[1], a02 = W[2], a10 = W[3], a11 = W[4], a12 = W[5], a20 = W[6], a21 = W[7], a22 = W[8];
        double det = a00*(a11*a22 - a12*a21) - a01*(a10*a22 - a12*a20) + a02*(a10*a21 - a11*a20);
        if (fabs(det) > 1e-12){
            double id = 1.0 / det, ai[3][3] = {
                { (a11*a22 - a12*a21)*id, (a02*a21 - a01*a22)*id, (a01*a12 - a02*a11)*id },
                { (a12*a20 - a10*a22)*id, (a00*a22 - a02*a20)*id, (a02*a10 - a00*a12)*id },
                { (a10*a21 - a11*a20)*id, (a01*a20 - a00*a21)*id, (a00*a11 - a01*a10)*id } };
            for (int c = 0; c < 4; c++){
                double l[3];
                for (int j = 0; j < 3; j++){
                    l[j] = 0; for (int r = 0; r < 3; r++) l[j] += ai[j][r] * (double)k[(K + c) * 4 + r];
                    m.lw[j * 4 + c] = (float)l[j]; }
                m.lw[12 + c] = (float)((double)k[(K + c) * 4 + 3] - (double)W[9]*l[0] - (double)W[10]*l[1] - (double)W[11]*l[2]);
            }
            m.prm[1] = g_hd_vsi[dr->vsid].ramp[0]; m.prm[2] = g_hd_vsi[dr->vsid].ramp[1];
            g_rt_slit[e->matidx] = frame;
        } else m.flags &= ~0xF00u;
    } else m.flags &= ~0xF00u;
    g_rt_smatc[e->matidx] = m;
}
static uint64_t rt_rawhash(const uint8_t *p, unsigned bytes, unsigned n, unsigned stride){
    uint64_t h = 1469598103934665603ull ^ ((uint64_t)n << 32) ^ stride;
    unsigned seg = bytes < 288u ? bytes : 96u;
    unsigned off[3] = { 0, bytes < 288u ? 0 : (bytes / 2u) & ~3u, bytes < 288u ? 0 : bytes - 96u };
    for (int s3 = 0; s3 < (bytes < 288u ? 1 : 3); s3++){
        const uint8_t *q = p + off[s3];
        unsigned k = 0;
        for (; k + 8 <= seg; k += 8){ uint64_t v; memcpy(&v, q + k, 8); h = (h ^ v) * 1099511628211ull; h ^= h >> 29; }
        for (; k < seg; k++){ h = (h ^ q[k]) * 1099511628211ull; }
    }
    return h ? h : 1;
}
static int rt_same_t(const float *a, const float *b){
    return fabsf(a[9] - b[9]) + fabsf(a[10] - b[10]) + fabsf(a[11] - b[11]) <= 0.03f;
}
static int rt_same_r(const float *a, const float *b){
    float dr = 0; for (int q = 0; q < 9; q++) dr += fabsf(a[q] - b[q]);
    return dr <= 0.02f;
}
/* If the same geometry (gh) is drawn at the same SPOT but with another texture/VS
 * or another ORIENTATION, it is a SIBLING:
 *   - a road piece lit by headlights <-> unlit, animated texture (different rk);
 *   - tree/light billboards turning to face the camera, rotating objects
 *     (different orientation).
 * If both stay in the store they overlap: the ray hits either at random, and
 * alpha-tested stacks make the ray get recast many times (measured: 12 copies of
 * one tree; GPU 8 -> 22 ms).  Rule:
 *   - an unseen (3+ frames) STATIC sibling is evicted;
 *   - an unseen CANDIDATE sibling is reused in place (so an object that rotates
 *     every frame doesn't fill the table with new entries);
 *   - every change raises the VOLATILITY (vol), the promotion threshold is
 *     RT_PROMOTE << vol: something that keeps changing stays in the instant
 *     capture and doesn't churn the store.
 * Siblings drawn together in the same frame (mirrored paired parts) survive. */
static RTEnt *rt_find(uint64_t rk, uint64_t gh, const float *W, uint32_t frame){
    unsigned i = RT_HIDX(gh), tomb = RT_TABN, vol = 0;
    RTEnt *found = 0, *empty = 0, *reuse = 0;
    for (unsigned probe = 0; probe < 512; probe++, i = (i + 1) & (RT_TABN - 1)){
        RTEnt *e = &g_rt_tab[i];
        if (e->state == 0){ empty = e; break; }
        if (e->state == 9){ if (tomb == RT_TABN) tomb = i; continue; }
        if (e->gh != gh || !rt_same_t(e->W, W)) continue;
        if (e->rk == rk && rt_same_r(e->W, W)){ found = e; continue; }
        if (e->vol > vol) vol = e->vol;
        if (e->state == 1){ if (frame - e->seen >= 1u && !reuse) reuse = e; continue; }
        if (frame - e->seen >= 3u){
            unsigned v = e->vol + 1u; if (v > 6u) v = 6u;
            if (rt_evict(e)){ if (v > vol) vol = v; atomic_fetch_add(&g_rt_evsib, 1ull); }
        }
    }
    if (!found){
        if (reuse){ found = reuse; if (vol < 6u) vol++; }
        else if (tomb != RT_TABN){ found = &g_rt_tab[tomb]; g_rt_tomb--; }
        else if (empty) found = empty;
        else return 0;
        memset(found, 0, sizeof *found);
        found->rk = rk; found->gh = gh; memcpy(found->W, W, sizeof found->W);
        found->state = 1; found->born = frame; found->seen = frame - 1u;
    }
    if (vol > found->vol) found->vol = (uint8_t)vol;
    return found;
}
/* Capture the scene's draws [0, upto), build the sub-structures and the top-level
 * structure.  Returns: total triangles (0 = no ray tracing this frame).  fogp: the
 * game's fog parameters for static hits (end, scale, lower limit, present). */
static unsigned hd_rt_frame(id<MTLCommandBuffer> cb, int rb, unsigned upto, const float *iv,
                            const float *vp, const float *ivp, float ipx, float ipy,
                            unsigned *ntex, float *fogp){
    if (!g_rt_pso) return 0;
    unsigned ring = (g_rt_ring = (g_rt_ring + 1) % 3);
    uint32_t frame = ++g_rt_frame_no;
    static unsigned cnt[HD_MAXVSI], off[HD_MAXVSI], fill[HD_MAXVSI];
    static uint8_t  act[MAXD];            /* 0 skip, 1 instant capture */
    static RTEnt   *ent[MAXD];
    memset(cnt, 0, sizeof cnt);
    unsigned nrec = 0, total = 0, npromo = 0;
    fogp[0] = fogp[1] = fogp[2] = fogp[3] = 0.f;
    g_rt_nzr = 0;
    if (g_rt_cache){
        /* HD_RT_STRESS=N: every N frames rehash + compacting merge, and every 4N
         * also drop the oldest quarter (tests the rare paths) */
        static int stress = -1; if (stress < 0){ const char *ev = ENVC("HD_RT_STRESS"); stress = ev ? atoi(ev) : 0; }
        if (stress > 0 && frame % (uint32_t)stress == 0u){
            if (!g_rt_mstate && frame - g_rt_lastsw >= 4u){
                rt_rehash(); rt_merge_begin(cb, frame, frame % (4u * (uint32_t)stress) == 0u); }
        }
        /* MERGE: the new region is about to fill or the store is full (see STATIC STORE: SPATIAL GROUPS) */
        else if (!g_rt_mstate && frame - g_rt_lastsw >= 4u &&
                 (g_rt_want_merge || g_rt_scur - g_rt_recent.lo > RT_RMAXV - 30000u || rt_depot_used() > RT_SMAXV - 60000u)){
            g_rt_want_merge = 0; rt_merge_begin(cb, frame, 0); }
        if (g_rt_tomb > RT_TABN / 4u) rt_rehash();
    }
    #define RT_OK(dr) (!(dr)->target && !(dr)->is_clear && !(dr)->is_blit && (dr)->count >= 3 && \
        (dr)->vsid > 0 && (dr)->vsid < HD_MAXVSI && ((dr)->depth & 2u))
    /* --- pass 1: classify --- */
    for (unsigned i = 0; i < upto && i < MAXD; i++){
        MDraw *dr = &g_dbuf[rb][i];
        act[i] = 0; ent[i] = 0;
        if (!RT_OK(dr)) continue;
        if (!g_hd_vsk[dr->vsid]){ atomic_fetch_add(&g_rt_skipk, 1ull); continue; }
        const uint8_t *kb = g_kbuf[rb] + dr->koff;
        const float *k = (const float *)(kb + 16);
        unsigned stride; memcpy(&stride, kb, 4);
        int cx = g_hd_vsi[dr->vsid].ok ? g_hd_vsi[dr->vsid].cx : -1;
        int have_m = cx >= 0 && (unsigned)(cx + 4) * 16u <= dr->ksz - 16u;
        if (have_m){
            const float *c3 = k + (cx + 3) * 4;
            if (c3[0] == 0.f && c3[1] == 0.f && c3[2] == 0.f) continue;       /* orthographic (screen) */
        }
        g_rt_dpok[i] = (uint8_t)have_m;                  /* see POSITION CHECK: W's translation row */
        if (have_m){
            double vx = (double)k[cx * 4 + 3] * (double)ipx, vy = (double)k[(cx + 1) * 4 + 3] * (double)ipy,
                   vz = (double)k[(cx + 3) * 4 + 3];
            for (int c = 0; c < 3; c++)
                g_rt_dp[i][c] = (float)(vx * iv[c] + vy * iv[4 + c] + vz * iv[8 + c] + (double)iv[12 + c]);
        }
        { int fr = g_hd_vsi[dr->vsid].fogr;                                   /* the game's fog constants */
          if (fr >= 0 && fogp[3] == 0.f && (unsigned)(fr + 1) * 16u <= dr->ksz - 16u){
              fogp[0] = k[fr*4]; fogp[1] = k[fr*4+1]; fogp[2] = k[fr*4+2]; fogp[3] = 1.f; } }
        unsigned n = dr->count - dr->count % 3u;
        int base = dr->ps % PSREFL, cube = base >= PSCUBE;
        RTEnt *e = 0;
        if (g_rt_cache && have_m && !cube && !g_hd_vsi[dr->vsid].skin && stride && n <= 60000u){
            /* W = M * P'^-1 * V^-1,  M[r][c] = c[cx+c][r].  The SAME path as the capture
             * kernel: view = (x/P00', y/P11, w), the homogeneous column EXACTLY (0,0,0,1).
             * It used to be M * (VP')^-1: the homogeneous component comes out of the
             * (z - Q*w)/(-Q*near) cancellation with ~1e-4 error, and multiplied by the
             * camera position (hundreds of units) W's translation wobbled 0.03-0.1
             * units relative to the camera.
             * Result: the same object counted as a separate entry at every camera
             * angle and piled up in the store (measured: 878 of 1771 entries were
             * copies; alpha-tested copy stacks made the ray get recast 5 times and
             * pushed the GPU from 8 ms to 22 ms). */
            float W[12];
            for (int r = 0; r < 4; r++){
                double vx = (double)k[cx * 4 + r] * (double)ipx, vy = (double)k[(cx + 1) * 4 + r] * (double)ipy,
                       vz = (double)k[(cx + 3) * 4 + r];
                for (int c = 0; c < 3; c++)
                    W[r * 3 + c] = (float)(vx * iv[c] + vy * iv[4 + c] + vz * iv[8 + c] + (r == 3 ? (double)iv[12 + c] : 0.0));
            }
            /* for indexed draws the id (VB/IB + generation + range) was computed on the guest side */
            uint64_t gh = dr->gh ? dr->gh : rt_rawhash(g_rvbuf[rb] + dr->rawoff, n * stride, n, stride);
            /* rk has NO VS id: the lit and unlit draws of the same part
             * (different VS, same positions) are one entry */
            uint64_t rk = (gh ^ ((uint64_t)dr->tex << 12)) * 0xD6E8FEB86659FD93ull;
            e = rt_find(rk ? rk : 1, gh, W, frame);
            if (e){
                /* promotion needs CONSECUTIVE sightings: a rotating object periodically comes
                 * back to the same angle, a total count made it "static" at every angle */
                if (e->seen != frame){
                    if (e->state == 1 && frame - e->seen > 2u) e->sight = 0;
                    e->seen = frame; if (e->sight < 60000) e->sight++; }
                if (e->state == 3 && e->n == n){
                    rt_smat_update(e, dr, k, frame);
                    ent[i] = e; continue;                                      /* in the static structure: no capture */
                }
                if (e->state == 1 && e->sight >= (RT_PROMOTE << e->vol) && npromo < 400u &&
                    (g_rt_mfree_n || g_rt_smn < RT_SMAXE - 64u)){
                    npromo++;
                    /* PROMOTION: bounding sphere from the raw positions */
                    unsigned po = (unsigned)(g_hd_vsi[dr->vsid].po < 0 ? 0 : g_hd_vsi[dr->vsid].po);
                    float mn[3] = {1e30f,1e30f,1e30f}, mx[3] = {-1e30f,-1e30f,-1e30f};
                    for (unsigned j = 0; j < n; j++){ float pv[3]; memcpy(pv, dr_vtx(rb, dr, j, stride) + po, 12);
                        for (int a3 = 0; a3 < 3; a3++){ if (pv[a3] < mn[a3]) mn[a3] = pv[a3]; if (pv[a3] > mx[a3]) mx[a3] = pv[a3]; } }
                    float lc[3] = { 0.5f*(mn[0]+mx[0]), 0.5f*(mn[1]+mx[1]), 0.5f*(mn[2]+mx[2]) };
                    float hx = 0.5f*(mx[0]-mn[0]), hy = 0.5f*(mx[1]-mn[1]), hz = 0.5f*(mx[2]-mn[2]);
                    for (int c = 0; c < 3; c++) e->c[c] = lc[0]*W[c] + lc[1]*W[3+c] + lc[2]*W[6+c] + W[9+c];
                    float sc2 = 0; for (int q = 0; q < 9; q++) sc2 += W[q]*W[q];
                    e->r = sqrtf(hx*hx + hy*hy + hz*hz) * sqrtf(sc2 / 3.f) + 0.1f;
                    float ddx = e->c[0] - iv[12], ddy = e->c[1] - iv[13], ddz = e->c[2] - iv[14];
                    if (sqrtf(ddx*ddx + ddy*ddy + ddz*ddz) > 800.f + e->r){
                        /* outside the ray range: don't put in the store (it would be evicted as "far" the moment it isn't seen) */
                        e->sight = 0;
                    } else {
                    uint32_t f0 = rt_alloc(n);              /* see STATIC STORE: BASE + NEW */
                    if (f0 != 0xFFFFFFFFu){
                        e->state = 2; e->first = f0; e->n = n; e->pad = 0;
                        e->matidx = g_rt_mfree_n ? g_rt_mfree[--g_rt_mfree_n] : g_rt_smn++;
                        atomic_fetch_add(&g_rt_promo, 1ull);
                    }
                    }
                }
            }
        }
        if (total + n > RT_MAXV || nrec >= RT_MAXREC){ atomic_fetch_add(&g_rt_over, 1ull); continue; }
        act[i] = 1; ent[i] = e;
        cnt[dr->vsid] += n; total += n; nrec++;
    }
    { unsigned a = 0; for (int g = 0; g < HD_MAXVSI; g++){ off[g] = a; a += cnt[g]; } }
    memcpy(fill, off, sizeof fill);
    uint32_t *item = (uint32_t *)[g_rt_item[ring] contents];
    HDRecC *rec = (HDRecC *)[g_rt_rec[ring] contents];
    HDMatC *mat = (HDMatC *)[g_rt_mat[ring] contents];
    unsigned nt = 1, ri = 0;
    g_rt_stamp++; g_rt_texs[0] = g_white;
    /* GPU copies of the promoted ones: (instant first vertex, static first vertex, count) */
    static uint32_t cp[512][3]; unsigned ncp = 0;
    uint64_t dsig = 1469598103934665603ull;         /* signature of the instant triangle layout (see refit) */
    static float cpos[RT_MAXREC][3]; static uint8_t cpok[RT_MAXREC];   /* translation in record order */
    /* --- pass 2: records + materials --- */
    for (unsigned i = 0; i < upto && i < MAXD; i++){
        if (!act[i]) continue;
        MDraw *dr = &g_dbuf[rb][i];
        unsigned n = dr->count - dr->count % 3u, f = fill[dr->vsid];
        { uint64_t w[4] = { (uint64_t)dr->vsid << 32 | n, (uint64_t)(uint32_t)dr->ps << 32 | dr->tex, dr->gh, f };
          for (int q = 0; q < 4; q++){ dsig ^= w[q]; dsig *= 1099511628211ull; } }
        fill[dr->vsid] += n;
        int base = dr->ps % PSREFL, cube = base >= PSCUBE, b = base % PSCUBE;
        HDMatC m; rt_mat_of(dr, &m);
        RTEnt *e = ent[i];
        uint32_t midx = RT_SMAXE + ri;
        if (e && e->state == 2 && e->first + n <= 2u * RT_SMAXV && e->seen == frame && e->born != 0xFFFFFFFFu && ncp < 512 &&
            e->pad == 0){
            /* promoted this frame: static material + GPU copy */
            e->pad = 1; midx = e->matidx;
            rt_smat_update(e, dr, (const float *)(g_kbuf[rb] + dr->koff + 16), frame);
            cp[ncp][0] = f; cp[ncp][1] = e->first; cp[ncp][2] = n; ncp++;
        }
        HDMatC *dm = &mat[RT_SMAXE + ri];
        *dm = m;
        dm->tex0 = hd_rt_slot(m.tex0, &nt); dm->tex1 = hd_rt_slot(m.tex1, &nt); dm->tex2 = hd_rt_slot(m.tex2, &nt);
        rec[ri].rawoff = dr->rawoff; rec[ri].koff = dr->koff; rec[ri].first = f; rec[ri].mat = midx;
        rec[ri].ib = dr->ix_ib; rec[ri].base = dr->ix_base; rec[ri].isz = dr->ix_isz; rec[ri].pad = 0;
        for (unsigned q = 0; q < n; q++) item[f + q] = ri;
        cpok[ri] = g_rt_dpok[i]; if (cpok[ri]) memcpy(cpos[ri], g_rt_dp[i], 12);
        if (ENVC("HD_RT_DLOG") && g_hd_dump_now)        /* diagnostic: layout of the instant capture */
            fprintf(stderr, "[DLOG] frame %ld record %u: vs %d ps %d n %u tex %u gh %016llx (%s) f %u ofs %u position %s %.1f %.1f %.1f\n",
                    (long)atomic_load(&g_frames_drawn), ri, dr->vsid, dr->ps, n, dr->tex, (unsigned long long)dr->gh,
                    dr->gh ? "indexed" : "ham", f, dr->rawoff, cpok[ri] ? "yes" : "no", cpos[ri][0], cpos[ri][1], cpos[ri][2]);
        ri++;
    }
    #undef RT_OK
    /* static materials: texture id -> this frame's slot */
    for (unsigned q = 0; q < g_rt_smn; q++){
        HDMatC *sm = &mat[q]; *sm = g_rt_smatc[q];
        if (g_rt_slit[q] == frame) sm->flags |= 0x20000u;
        sm->tex0 = hd_rt_slot(g_rt_smatc[q].tex0, &nt); sm->tex1 = hd_rt_slot(g_rt_smatc[q].tex1, &nt);
        sm->tex2 = hd_rt_slot(g_rt_smatc[q].tex2, &nt);
    }
    /* --- eviction scan ---
     * The game also culls things inside the view (doesn't draw small objects far
     * away, section visibility).  When the "in the view but not drawn" rule was
     * applied up to 200 units, poles etc. kept getting evicted and promoted again
     * (measured: 300+ evictions in 300 frames while the camera moved).  Now:
     *  - FAR: farther than 900 units and not seen for 30 frames (ray range 600);
     *  - NEAR: right in front of the camera (< 50 units, less by size), in the
     *    middle of the view and not drawn for 60 frames = really removed
     *    (a knocked-over obstacle, a closed menu scene);
     *  - SIBLING: inside rt_find (same geometry drawn with another texture/VS). */
    if (g_rt_cache){
        static int lg2 = -1; if (lg2 < 0){ const char *ev = ENVC("HD_RT_LOG"); lg2 = ev ? atoi(ev) : 0; }
        for (unsigned q = 0; q < RT_TABN; q++){
            RTEnt *e = &g_rt_tab[q];
            if (e->state == 0 || e->state == 9) continue;
            uint32_t age = frame - e->seen;
            if (e->state == 1){ if (age > 30u + 90u * e->vol){ e->state = 9; g_rt_tomb++; g_rt_ev_cand++; } continue; }
            if (age < 30u) continue;
            float dx = e->c[0] - iv[12], dy = e->c[1] - iv[13], dz = e->c[2] - iv[14];
            float dist = sqrtf(dx*dx + dy*dy + dz*dz);
            int kill = dist > 900.f + e->r ? 1 : 0;
            if (!kill && age >= 60u){
                float qx = e->c[0]*vp[0] + e->c[1]*vp[4] + e->c[2]*vp[8]  + vp[12];
                float qy = e->c[0]*vp[1] + e->c[1]*vp[5] + e->c[2]*vp[9]  + vp[13];
                float qw = e->c[0]*vp[3] + e->c[1]*vp[7] + e->c[2]*vp[11] + vp[15];
                float lim = 60.f * e->r < 50.f ? 60.f * e->r : 50.f;
                if (qw > e->r + 3.f && qw < lim && fabsf(qx) < 0.6f * qw && fabsf(qy) < 0.6f * qw) kill = 2;
            }
            if (!kill) continue;
            uint32_t tex0 = g_rt_smatc[e->matidx].tex0, fl = g_rt_smatc[e->matidx].flags;
            if (!rt_evict(e)) break;
            if (kill == 1) g_rt_ev_far++; else g_rt_ev_frus++;
            if (lg2 >= 2 && (frame % 300u) < 100u)
                fprintf(stderr, "[RTEV] frame %u %s: gh %016llx n %u distance %.0f r %.1f center (%.0f %.0f %.0f) seen %u life %u age %u mat(tex %u flags %x)\n",
                        frame, kill == 1 ? "far" : "yakin", (unsigned long long)e->gh, e->n, dist, e->r,
                        e->c[0], e->c[1], e->c[2], e->sight, frame - e->born, age, tex0, fl);
        }
    }
    { static int lg3 = -1; if (lg3 < 0){ const char *ev = ENVC("HD_RT_DUMPTAB"); lg3 = ev ? atoi(ev) : 0; }
      if (lg3 > 0 && frame == (uint32_t)lg3){
          for (unsigned q = 0; q < RT_TABN; q++){ RTEnt *e = &g_rt_tab[q];
              if (e->state != 2 && e->state != 3) continue;
              float dw = 0; for (int a = 0; a < 12; a++) dw += fabsf(e->W[a] - ((a == 0 || a == 4 || a == 8) ? 1.f : 0.f));
              fprintf(stderr, "[RTTAB] %016llx %016llx n %u c %.2f %.2f %.2f r %.2f age %u seen %u vol %u identity %d t %.2f %.2f %.2f tex %u %u flags %x aref %.3f\n",
                      (unsigned long long)e->gh, (unsigned long long)e->rk, e->n, e->c[0], e->c[1], e->c[2], e->r, frame - e->seen, e->sight, e->vol,
                      dw < 1e-3f, e->W[9], e->W[10], e->W[11], g_rt_smatc[e->matidx].tex0, g_rt_smatc[e->matidx].tex1,
                      g_rt_smatc[e->matidx].flags, g_rt_smatc[e->matidx].prm[0]); }
          fprintf(stderr, "[RTTAB] camera %.2f %.2f %.2f\n", iv[12], iv[13], iv[14]);
      } }
    { static int lg = -1; if (lg < 0) lg = ENVC("HD_RT_LOG") ? 1 : 0;
      static unsigned long long lp, le, lsib;
      if (lg && frame % 300u == 0u){
          unsigned n1 = 0, n2 = 0, n3 = 0;
          for (unsigned q = 0; q < RT_TABN; q++){ unsigned st = g_rt_tab[q].state; n1 += st == 1; n2 += st == 2; n3 += st == 3; }
          unsigned long long pr = atomic_load(&g_rt_promo), ev = atomic_load(&g_rt_evict);
          fprintf(stderr, "[RTLOG] frame %u: instant %u tris/%u draws, store %u verts (%u dead), candidates %u pending %u static %u | "
                  "in 300 frames promoted %llu evicted %llu (far %u, near %u, sibling %llu, candidate %u) | compactions %llu resets %llu hashes %llu\n",
                  frame, total / 3, nrec, rt_depot_used(), rt_depot_dead(), n1, n2, n3, pr - lp, ev - le,
                  g_rt_ev_far, g_rt_ev_frus, atomic_load(&g_rt_evsib) - lsib, g_rt_ev_cand, atomic_load(&g_rt_compact), atomic_load(&g_rt_reset), atomic_load(&g_rt_rehash));
          lp = pr; le = ev; lsib = atomic_load(&g_rt_evsib); g_rt_ev_far = g_rt_ev_frus = g_rt_ev_cand = 0;
          if (g_rt_fixcnt){ const uint32_t *fc = (const uint32_t *)[g_rt_fixcnt contents];
              char ln[512]; int o = 0;
              for (int g = 1; g < HD_MAXVSI && o < 470; g++) if (fc[2 + g * 2] || fc[3 + g * 2])
                  o += snprintf(ln + o, sizeof ln - (size_t)o, " vs_%d:%u/%u", g, fc[2 + g * 2], fc[3 + g * 2]);
              fprintf(stderr, "[RTLOG]   vertex check (total): NaN/infinite %u tris, too far %u tris | VS (NaN/far):%s\n", fc[0], fc[1], o ? ln : " -"); }
          { char ln[512]; int o = 0;      /* vertices captured instantly per VS (which shaders enter the structure) */
            for (int g = 1; g < HD_MAXVSI && o < 480; g++) if (cnt[g]) o += snprintf(ln + o, sizeof ln - (size_t)o, " vs_%d:%u", g, cnt[g]);
            fprintf(stderr, "[RTLOG]   instant capture (VS:verts):%s\n", o ? ln : " -"); }
      } }
    /* --- GPU: capture --- */
    if (total){
        id<MTLComputeCommandEncoder> ce = pp_compute(cb, "RT: vertex capture (compute)");
        [ce setBuffer:g_mtlrvb offset:0 atIndex:1];
        [ce setBuffer:g_mtlkb offset:0 atIndex:2];
        [ce setBuffer:g_rt_item[ring] offset:0 atIndex:3];
        [ce setBuffer:g_rt_rec[ring] offset:0 atIndex:4];
        [ce setBuffer:g_rt_pos offset:0 atIndex:5];
        [ce setBuffer:g_rt_attr offset:0 atIndex:6];
        [ce setBuffer:(g_pool_fr[rb] ? g_pool_fr[rb] : g_mtlrvb) offset:0 atIndex:8];   /* indexed records */
        for (int g = 1; g < HD_MAXVSI; g++){
            if (!cnt[g]) continue;
            struct { float invV[16]; float prm[4]; uint32_t first, count, p0, p1; } C;
            memcpy(C.invV, iv, sizeof C.invV);
            C.prm[0] = ipx; C.prm[1] = ipy; C.prm[2] = C.prm[3] = 0.f;
            C.first = off[g]; C.count = cnt[g]; C.p0 = C.p1 = 0;
            [ce setComputePipelineState:g_hd_vsk[g]];
            [ce setBytes:&C length:sizeof C atIndex:7];
            NSUInteger tw = g_hd_vsk[g].maxTotalThreadsPerThreadgroup;
            [ce dispatchThreadgroups:MTLSizeMake((cnt[g] + tw - 1) / tw, 1, 1)
               threadsPerThreadgroup:MTLSizeMake(tw, 1, 1)];
        }
        if (g_rt_fixk && g_rt_fixcnt){
            [ce setComputePipelineState:g_rt_fixk];
            [ce setBuffer:g_rt_pos offset:0 atIndex:0];
            [ce setBuffer:g_rt_fixcnt offset:0 atIndex:1];
            NSUInteger tw = g_rt_fixk.maxTotalThreadsPerThreadgroup;
            for (int g = 1; g < HD_MAXVSI; g++){            /* per VS: so the counters tell which shader it is */
                if (!cnt[g]) continue;
                static float poison = -1.f;      /* HD_RT_POISON=<value>: causality test (no check done) */
                if (poison < 0.f){ const char *ev = ENVC("HD_RT_POISON"); poison = ev ? (float)atof(ev) : 0.f; }
                struct { uint32_t ntri; float lim; uint32_t first, slot; } F = { cnt[g] / 3u, poison > 0.f ? -poison : 100000.f, off[g], (uint32_t)g };
                if (poison > 0.f && g != 3) continue;        /* only the first triangle of one VS (world) */
                [ce setBytes:&F length:sizeof F atIndex:2];
                [ce dispatchThreadgroups:MTLSizeMake((cnt[g] / 3u + tw - 1) / tw, 1, 1)
                   threadsPerThreadgroup:MTLSizeMake(tw, 1, 1)];
            }
        }
        [ce endEncoding];
    }
    static int stage = -1;
    if (stage < 0){ const char *e = ENVC("HD_RT_STAGE"); stage = e ? atoi(e) : 3; }
    int st = (g_ab_mode == 3) ? 2 : (g_ab_mode == 4) ? 1 : stage;      /* HD_RT_AB slice */
    if (st == 1) return 0;
    /* --- GPU: copy into the static store (this frame's promotions) --- */
    if (ncp || g_rt_need_clear){
        id<MTLBlitCommandEncoder> be = pp_blit(cb, "RT: copy into the static store");
        if (g_rt_need_clear){ g_rt_need_clear = 0;
            [be fillBuffer:g_rt_spos range:NSMakeRange(0, 2u * RT_SMAXV * 12) value:0]; }
        for (unsigned q = 0; q < ncp; q++){
            [be copyFromBuffer:g_rt_pos sourceOffset:(NSUInteger)cp[q][0] * 12 toBuffer:g_rt_spos
             destinationOffset:(NSUInteger)cp[q][1] * 12 size:(NSUInteger)cp[q][2] * 12];
            [be copyFromBuffer:g_rt_attr sourceOffset:(NSUInteger)cp[q][0] * 64 toBuffer:g_rt_sattr
             destinationOffset:(NSUInteger)cp[q][1] * 64 size:(NSUInteger)cp[q][2] * 64];
        }
        [be endEncoding];
    }
    /* --- GPU: collapse the evicted ones onto their first vertices (see STATIC STORE SLOTS) --- */
    if (g_rt_nzr){
        id<MTLComputeCommandEncoder> ce = pp_compute(cb, "RT: collapse evicted");
        [ce setComputePipelineState:g_rt_colk];
        [ce setBuffer:g_rt_spos offset:0 atIndex:0];
        [ce setBytes:g_rt_zr length:(NSUInteger)g_rt_nzr * 8 atIndex:1];
        uint32_t nr = g_rt_nzr; [ce setBytes:&nr length:4 atIndex:2];
        NSUInteger tw = g_rt_colk.threadExecutionWidth;
        [ce dispatchThreadgroups:MTLSizeMake((g_rt_nzr + tw - 1) / tw, 1, 1) threadsPerThreadgroup:MTLSizeMake(tw, 1, 1)];
        [ce endEncoding];
    }
    /* --- GPU: sub-structures in ONE encoder: instant + base + new (each with its own
     * scratch; the builds run concurrently, k79 asbench) --- */
    int have_dyn = total >= 3, dyn_refit = 0;
    if (have_dyn){
        dsig ^= (uint64_t)total; dsig *= 1099511628211ull;
        dyn_refit = hd_rt_refit_on() && dsig == g_rt_dsig && g_rt_dsince < RT_REFIT_MAX;
        if (dyn_refit && ri == g_rt_bn){                /* see POSITION CHECK */
            static float mv2 = -1.f;
            if (mv2 < 0.f){ const char *ev = ENVC("HD_RT_REFIT_MOVE"); float m = ev ? (float)atof(ev) : RT_REFIT_MOVE; mv2 = m * m; }
            for (unsigned q = 0; q < ri; q++){
                if (!cpok[q] || !g_rt_bpok[q]) continue;
                float dx = cpos[q][0] - g_rt_bpos[q][0], dy = cpos[q][1] - g_rt_bpos[q][1], dz = cpos[q][2] - g_rt_bpos[q][2];
                if (dx*dx + dy*dy + dz*dz > mv2){ dyn_refit = 0; atomic_fetch_add(&g_rt_nmoved, 1ull); break; }
            }
        } else if (dyn_refit) dyn_refit = 0;
    }
    /* groups: during a merge one new group build per frame; on eviction at most one
     * group refit per frame (once every RT_BREFIT frames per group).
     * new: a build every RT_SREBUILD frames, refit in the same frame on eviction */
    uint32_t rn = g_rt_scur - g_rt_recent.lo;
    int mb = (g_rt_mstate && g_rt_mnext < g_rt_nng) ? (int)g_rt_mnext++ : -1;
    int gr = -1;
    if (!g_rt_mstate && g_rt_ng){ static unsigned rr;
        for (unsigned t = 0; t < g_rt_ng; t++){ unsigned k = (rr + t) % g_rt_ng;
            if (g_rt_grp[k].refit && g_rt_grp[k].built >= 3 && frame - g_rt_grp[k].refit_frame >= RT_BREFIT){ gr = (int)k; rr = k + 1u; break; } } }
    int ract = (g_rt_recent.dirty && rn >= 3 && (!g_rt_recent.built || frame - g_rt_recent.built_frame >= RT_SREBUILD)) ? 1
             : (g_rt_recent.refit && g_rt_recent.built >= 3) ? 2 : 0;
    if (have_dyn || mb >= 0 || gr >= 0 || ract){
        id<MTLAccelerationStructureCommandEncoder> ae = pp_accel(cb,
            mb >= 0 ? "RT: sub-structures (with group build)" : ract == 1 ? "RT: sub-structures (with new build)"
            : (gr >= 0 || ract) ? "RT: sub-structures (with refit)" : dyn_refit ? "RT: instant structure (refit)" : "RT: instant structure build");
        if (have_dyn){
            MTLAccelerationStructureTriangleGeometryDescriptor *gd =
                [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
            gd.vertexBuffer = g_rt_pos; gd.vertexStride = 12; gd.triangleCount = total / 3; gd.opaque = YES;
            MTLPrimitiveAccelerationStructureDescriptor *ad = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
            ad.geometryDescriptors = @[gd]; ad.usage = hd_rt_dyn_usage();
            if (dyn_refit){
                [ae refitAccelerationStructure:g_rt_as descriptor:ad destination:g_rt_as
                                 scratchBuffer:g_rt_scratch scratchBufferOffset:0];
                g_rt_dsince++; atomic_fetch_add(&g_rt_nrefit, 1ull);
            } else {
                [ae buildAccelerationStructure:g_rt_as descriptor:ad scratchBuffer:g_rt_scratch scratchBufferOffset:0];
                g_rt_dsince = 0; atomic_fetch_add(&g_rt_nbuild, 1ull);
                memcpy(g_rt_bpos, cpos, (size_t)ri * sizeof cpos[0]); memcpy(g_rt_bpok, cpok, ri); g_rt_bn = ri;
            }
            g_rt_dsig = dsig;
        }
        if (mb >= 0){                                   /* merge: the next new group */
            RTPart *pt = &g_rt_ngrp[mb];
            [ae buildAccelerationStructure:pt->as descriptor:rt_part_desc(pt->lo, pt->built)
                             scratchBuffer:g_rt_gscr_b scratchBufferOffset:0];
            pt->built_frame = frame; atomic_fetch_add(&g_rt_sbuilds, 1ull);
        }
        if (gr >= 0){
            RTPart *pt = &g_rt_grp[gr];
            [ae refitAccelerationStructure:pt->as descriptor:rt_part_desc(pt->lo, pt->built) destination:pt->as
                             scratchBuffer:g_rt_gscr_r scratchBufferOffset:0];
            pt->refit = 0; pt->refit_frame = frame; atomic_fetch_add(&g_rt_srefits, 1ull);
        }
        if (ract){
            RTPart *pt = &g_rt_recent;
            uint32_t nv = ract == 1 ? rn : pt->built;
            if (ract == 1){
                [ae buildAccelerationStructure:pt->as descriptor:rt_part_desc(pt->lo, nv) scratchBuffer:pt->scratch scratchBufferOffset:0];
                pt->built = nv; pt->built_frame = pt->refit_frame = frame; pt->dirty = 0; pt->refit = 0;
                atomic_fetch_add(&g_rt_sbuilds, 1ull);
            } else {
                [ae refitAccelerationStructure:pt->as descriptor:rt_part_desc(pt->lo, nv) destination:pt->as
                                 scratchBuffer:pt->scratch scratchBufferOffset:0];
                pt->refit = 0; pt->refit_frame = frame; atomic_fetch_add(&g_rt_srefits, 1ull);
            }
        }
        [ae endEncoding];
    }
    if (!have_dyn) g_rt_dsince = 0xFFFFFFFFu;
    /* the pending promotions in the built new region are now in the structure: remove from capture */
    if (ract == 1) for (unsigned q = 0; q < RT_TABN; q++){
        RTEnt *e = &g_rt_tab[q];
        if (e->state != 2 || !e->pad) continue;
        if (e->first >= g_rt_recent.lo && e->first - g_rt_recent.lo < g_rt_recent.built) e->state = 3;
    }
    /* the evicted ones' triangles dropped out of the structure with refit / build: free their materials
     * (the new region every frame: either refitted this frame or never in the structure) */
    { uint8_t done[256]; memset(done, 0, sizeof done); done[RT_PREC] = 1; if (gr >= 0) done[gr] = 1;
      rt_mpend_release(done); }
    /* the merge's last group was encoded: switch (this frame's top-level structure with the new groups) */
    if (g_rt_mstate && g_rt_mnext >= g_rt_nng) rt_merge_switch(frame);
    /* --- GPU: top-level structure = instant + groups + new; IB: instance -> store base --- */
    unsigned sbuilt = 0;
    MTLAccelerationStructureInstanceDescriptor *id2 = (MTLAccelerationStructureInstanceDescriptor *)[g_rt_inst[ring] contents];
    NSMutableArray *ias = [NSMutableArray arrayWithCapacity:RT_NINST];
    g_rt_nib = 0; g_rt_tln = 0;
    static int only = -1;                 /* diagnostic: HD_RT_ONLY=1 instant only, 2 groups only, 3 new only */
    if (only < 0){ const char *ev = ENVC("HD_RT_ONLY"); only = ev ? atoi(ev) : 0; }
    if (have_dyn && (!only || only == 1)){ id2[g_rt_nib].accelerationStructureIndex = (uint32_t)ias.count; [ias addObject:g_rt_as];
                   g_rt_tl[g_rt_tln++] = g_rt_as; g_rt_ib[g_rt_nib++] = 0xFFFFFFFFu; }
    for (unsigned k = 0; k <= g_rt_ng; k++){
        RTPart *pt = k < g_rt_ng ? &g_rt_grp[k] : &g_rt_recent;
        if (pt->built < 3 || !pt->as) continue;
        if (only && only != (k < g_rt_ng ? 2 : 3)) continue;
        id2[g_rt_nib].accelerationStructureIndex = (uint32_t)ias.count; [ias addObject:pt->as];
        g_rt_tl[g_rt_tln++] = pt->as; g_rt_ib[g_rt_nib++] = pt->lo; sbuilt += pt->built;
    }
    g_rt_only_static = !have_dyn;
    if (!g_rt_nib) return 0;
    {   id<MTLAccelerationStructureCommandEncoder> ae = pp_accel(cb, "RT: top-level structure");
        MTLInstanceAccelerationStructureDescriptor *td = [MTLInstanceAccelerationStructureDescriptor descriptor];
        td.instancedAccelerationStructures = ias;
        td.instanceCount = g_rt_nib; td.instanceDescriptorBuffer = g_rt_inst[ring];
        [ae buildAccelerationStructure:g_rt_tlas descriptor:td scratchBuffer:g_rt_tscratch scratchBufferOffset:0];
        [ae endEncoding]; }
    if (st == 2) return 0;
    /* texture table */
    [g_rt_argenc setArgumentBuffer:g_rt_arg[ring] offset:0];
    for (unsigned q = 0; q < nt; q++) [g_rt_argenc setTexture:g_rt_texs[q] atIndex:q];
    *ntex = nt;
    atomic_fetch_add(&g_rt_frames, 1ull);
    atomic_fetch_add(&g_rt_tris, (unsigned long long)((total + sbuilt) / 3));
    atomic_fetch_add(&g_rt_draws, (unsigned long long)ri);
    atomic_fetch_add(&g_rt_dynv, (unsigned long long)total);
    atomic_fetch_add(&g_rt_statv, (unsigned long long)sbuilt);
    return (total + sbuilt) / 3;
}
static int g_hd_dump_now;       /* this frame was selected with DUMPFRAME (diagnostic) */
static int g_rt_kforce;         /* HD_RTPROBE2: 1 old kernel, 2 RT_V2 (comparison with the same data) */
static void hd_rt_trace(id<MTLCommandBuffer> cb, const void *par, size_t parsz, id<MTLTexture> zt,
                        NSUInteger W, NSUInteger H, unsigned nt){
    id<MTLComputeCommandEncoder> ce = pp_compute(cb, "RT: ray kernel (compute)");
    [ce setComputePipelineState:g_rt_kforce == 1 ? g_rt_kern : (g_rt_kforce == 2 && g_rt_kern2) ? g_rt_kern2
                                : (g_rt_kern2 && !abt_off("RTV2")) ? g_rt_kern2 : g_rt_kern];
    [ce setTexture:g_rt_out atIndex:0];
    [ce setTexture:zt atIndex:1];
    [ce setTexture:g_hd_g[0] atIndex:2];
    [ce setTexture:g_hd_g[1] atIndex:3];
    [ce setTexture:g_hd_g[2] atIndex:4];
    [ce setBytes:par length:parsz atIndex:0];
    [ce setAccelerationStructure:g_rt_tlas atBufferIndex:1];
    [ce useResource:g_rt_as usage:MTLResourceUsageRead];
    for (unsigned k = 0; k < g_rt_tln; k++) if (g_rt_tl[k] != g_rt_as) [ce useResource:g_rt_tl[k] usage:MTLResourceUsageRead];
    { uint32_t ib1 = 0xFFFFFFFFu;        /* see STATIC STORE SLOTS: instance -> store base */
      if (g_rt_nib) [ce setBytes:g_rt_ib length:sizeof(uint32_t) * g_rt_nib atIndex:8];
      else [ce setBytes:&ib1 length:sizeof ib1 atIndex:8]; }
    [ce setBuffer:g_rt_mat[g_rt_ring] offset:0 atIndex:3];
    [ce setBuffer:g_rt_attr offset:0 atIndex:4];
    [ce setBuffer:g_rt_arg[g_rt_ring] offset:0 atIndex:6];
    [ce setBuffer:g_rt_sattr offset:0 atIndex:7];
    [ce useResources:g_rt_texs count:nt usage:MTLResourceUsageRead];
    NSUInteger tw = g_rt_kern.threadExecutionWidth, th = g_rt_kern.maxTotalThreadsPerThreadgroup / tw;
    [ce dispatchThreadgroups:MTLSizeMake((W + tw - 1) / tw, (H + th - 1) / th, 1)
       threadsPerThreadgroup:MTLSizeMake(tw, th, 1)];
    [ce endEncoding];
}
/* --- RAY TRACING AT THE START OF THE FRAME ---------------------------------------------
 * Capture + acceleration structures were encoded where the scene ends (right
 * before the composite).  Since the ray kernel waits for the structures, the GPU
 * latency of the build was on the frame's critical path: Metal's software builder
 * takes ~0.8 ms even for a small structure and doesn't fill the GPU (measured,
 * k79 asbench: 8 builds in one encoder ~2.5x a single build).  The captured
 * vertex data and the camera are known at the start of the frame; when the same
 * work is encoded BEFORE the first render pass the GPU overlaps it with the
 * reflection / cube / scene passes (k79 ovl: a fast build of 8.7 thousand
 * triangles 0.94 ms alone, +0.1 ms together with 9 ms of render work).  The end
 * of the scene (first copy from the screen) and the reflection draw are scanned
 * at the start of the frame; if the early path couldn't run the composite builds
 * by itself as before.
 * HD_RT_EARLY=0 old; ABTEST=RTEARLY old path in the off slice. */
static struct { int ok; unsigned tris, nt; float fogp[4]; } g_rt_early;
static int hd_rt_early_on(void){
    static int v = -1;
    if (v < 0){ const char *e = ENVC("HD_RT_EARLY"); v = (e && *e == '0') ? 0 : 1; }
    return v && !abt_off("RTEARLY");
}
/* The frame's camera: view-projection (with Hor+), its inverse, the view inverse and
 * the projection's 1/P00, 1/P11.  0 = no camera or singular. */
static int hd_cam_mats(int rb, float *vp, float *ivp, float *iv, float *ipx, float *ipy){
    if (!g_hd_cam_ok[rb]) return 0;
    memcpy(vp, &g_hd_cam[rb][32], 16 * sizeof(float));
    for (int r4 = 0; r4 < 4; r4++) vp[r4*4] *= g_hd_cam_k[rb];   /* Hor+ */
    if (!hd_inv4(vp, ivp) || !hd_inv4(&g_hd_cam[rb][0], iv)) return 0;
    float p00 = g_hd_cam[rb][16] * g_hd_cam_k[rb], p11 = g_hd_cam[rb][16 + 5];
    if (p00 == 0.f || p11 == 0.f) return 0;
    *ipx = 1.f / p00; *ipy = 1.f / p11;
    return 1;
}
/* ---- MID-SCENE COPY (motion blur) -------------------------------
 * When the game drives fast, in most frames (race 6000-8000: 1237 of 2000 frames)
 * it first draws the far scene and copies the screen to #786, blurs it (ps19) and
 * blends it onto the screen (ps22), THEN draws the cars and the near scene
 * (writing Z).  HD counted the first copy from the screen as the end of the scene:
 * the composite (RT/SSR) ran at the copy, after the copy the screen was single
 * sampled -> in those frames the cars had no MSAA, no G-buffer (no RT reflection)
 * and were outside the reflection capture; flickering since it changed from frame
 * to frame.  Now: if after a copy from the screen there is a draw writing Z to the
 * screen in the same frame (before the next screen copy), that copy is MID-SCENE:
 * the multisampled color / G / Z are stored + resolved (the copy reads the resolved
 * color), the scene continues with the same attachments (Load), the composite at
 * the real end of the scene.  HD_MBFIX=0 old behavior. */
static uint8_t g_hd_brk[MAXD];                   /* this frame's mid-scene copies */
static _Atomic unsigned long long g_hd_brk_n;
static void hd_break_plan(int rb){
    static int on = -1; if (on < 0){ const char *e = ENVC("HD_MBFIX"); on = !(e && *e == '0'); }
    if (rb < 0) return;
    unsigned n = g_dn[rb] < MAXD ? g_dn[rb] : MAXD, b = ~0u;
    memset(g_hd_brk, 0, n);
    for (unsigned i = 0; i < n; i++){
        const MDraw *dr = &g_dbuf[rb][i];
        if (dr->is_blit && !dr->blit_src){ b = dr->target ? i : ~0u; continue; }
        if (dr->is_blit) continue;
        if (on && !abt_off("MBFIX") && b != ~0u && !dr->target && !dr->is_clear && (dr->depth & 2u) && dr->count && !g_hd_brk[b]){
            g_hd_brk[b] = 1; atomic_fetch_add(&g_hd_brk_n, 1ull); }
    }
}
/* The record that will trigger the composite: the first copy from the screen (same
 * condition as in the encoding loop).  *refl: will the reflection variant be drawn
 * into the G-buffered screen pass before it (a prediction of hd_refl_drawn). */
static unsigned hd_scene_end_scan(int rb, int *refl){
    *refl = 0;
    for (unsigned i = 0; i < g_dn[rb] && i < MAXD; i++){
        const MDraw *dr = &g_dbuf[rb][i];
        if (dr->is_blit){
            if (!dr->blit_src && dr->target && TGT_ID(dr->target) < MAXTEX && g_tex[TGT_ID(dr->target)] && !g_hd_brk[i]) return i;
            continue; }
        if (!dr->target && !dr->is_clear && dr->ps >= PSREFL) *refl = 1;
    }
    return ~0u;
}
/* diagnostic (HD_MBLOG=1): is there a 3D draw writing Z to the screen AFTER the first
 * copy from the screen (motion blur copy in the middle of the scene) */
static void hd_mb_log(int rb, long fno){
    static int on = -1; if (on < 0) on = ENVC("HD_MBLOG") ? 1 : 0;
    if (!on || rb < 0) return;
    static unsigned long long nf, nm; static int shown;
    unsigned n = g_dn[rb], B = ~0u, after = 0, a777 = 0, firstk = 0;
    for (unsigned i = 0; i < n && i < MAXD; i++){
        const MDraw *dr = &g_dbuf[rb][i];
        if (B == ~0u){ if (dr->is_blit && !dr->blit_src) B = i; continue; }
        if (!dr->target && !dr->is_clear && !dr->is_blit && (dr->depth & 2u) && dr->count){
            if (!after) firstk = i; after++; if (dr->ps >= PSREFL) a777++; }
    }
    nf++; if (after) nm++;
    static long lastf = -10;
    if (after && fno > 5800 && fno - lastf > 30 && shown < 40){ shown++;
        fprintf(stderr, "[MB] frame %ld: screen copy %u, then %u Z-writing screen draws (first %u, reflective %u), total %u\n",
                fno, B, after, firstk, a777, n); }
    if (after) lastf = fno;
    if ((nf % 2000ull) == 0ull) fprintf(stderr, "[MB] %llu mid-scene copies in %llu frames\n", nf, nm);
}
static void hd_rt_early(id<MTLCommandBuffer> cb, int rb){
    g_rt_early.ok = 0;
    if (!hd_rt_early_on() || !hd_rt_on() || !g_hd_g[0] || g_ab_mode == 5 || g_ab_mode == 6) return;
    int refl = 0; unsigned se = hd_scene_end_scan(rb, &refl);
    float vp[16], ivp[16], iv[16], ipx, ipy;
    if (se == ~0u || !refl || !hd_cam_mats(rb, vp, ivp, iv, &ipx, &ipy)) return;
    hd_rt_init();
    if (!g_rt_pso) return;
    g_rt_early.tris = hd_rt_frame(cb, rb, se, iv, vp, ivp, ipx, ipy, &g_rt_early.nt, g_rt_early.fogp);
    g_rt_early.ok = 1;
}
/* End-of-scene composite: ray tracing (if present) or SSR.  i = the draw index
 * where the scene ends; scr = the resolved screen texture (drawable). */
/* ---- MSAA: unnecessary store / resolve ------------------------------------
 * Measured (PASSPROF, 1080p race frame): screen passes are the biggest item of
 * GPU time; there are 5 multisampled screen passes per frame and EACH stored and
 * resolved color + 3 G-buffers + Z at 4x:
 *     0  Clear only (moves straight on to reflection targets)
 *     13 scene (part 1)     14 = off-screen intermediate pass
 *     15 scene (part 2)     -> composite -> post-process chain
 *     39 post-process quad  40 UI
 * Three fixes:
 *  1) A screen pass with only a Clear isn't opened (the next screen pass clears on
 *     first use anyway).
 *  2) The store action is chosen when the pass CLOSES (MTLStoreActionUnknown):
 *     if another multisampled screen pass follows, STORE only; if a copy reading
 *     the screen (composite / post-process) or the end of the frame follows,
 *     RESOLVE only.  The G-buffer is resolved only right before the composite.
 *  3) Screen passes AFTER the first copy from the screen (post-process quad, UI)
 *     are single sampled: since the composite and post-process write the
 *     already-resolved image identically to all samples, MSAA has no information
 *     there.
 * Z untouched (the twin sync logic is separate).  HD_MSLAZY=0 old behavior. */
static int hd_mslazy(void){
    static int v = -1; if (v < 0){ const char *e = ENVC("HD_MSLAZY"); v = (e && *e == '0') ? 0 : 1; }
    return v;
}
/* When the pass OPENS: will a resolved image be needed after this pass?
 * Metal rule: if a resolve target is bound the store action must include a
 * resolve, so the resolve decision must be made at open time (store at close).
 * Returns: bit0 color must be resolved, bit1 a reading screen copy (G must be resolved too). */
static unsigned hd_ms_predict(int rb, unsigned i){
    unsigned n = g_dn[rb], zid = DEPTH_ZID(g_dbuf[rb][i].depth), j = i + 1;
    if (!hd_mslazy()) return 3u;
    for (; j < n; j++){ const MDraw *x = &g_dbuf[rb][j];                 /* end of the pass */
        if (x->is_blit || x->target || DEPTH_ZID(x->depth) != zid) break;
        if (x->is_clear && (x->clr_flags & 2u) && zid) break; }
    for (; j < n; j++){ const MDraw *x = &g_dbuf[rb][j];
        if (x->is_blit){
            if (!x->blit_src) return 3u;                                  /* the screen is read */
            if (!x->target) return 1u;                                    /* copy into the screen: cautious */
            continue; }
        if (!x->target) return 0u;                                        /* another multisampled screen pass */
    }
    return 1u;                                                            /* end of frame: present */
}
/* Can this multisampled screen pass be opened with MEMORYLESS attachments (the
 * caller checks the load)?  If after the pass ends, before the screen is read
 * (composite / post-process copy), there is no other screen pass or copy into the
 * screen in the same frame, no sample needs to be stored.  The body detection is
 * the same as hd_ms_predict; the encoding loop also closes the pass at the same
 * places. */
static int hd_ms_memless_ok(int rb, unsigned i){
    unsigned n = g_dn[rb], zid = DEPTH_ZID(g_dbuf[rb][i].depth), j = i + 1;
    for (; j < n; j++){ const MDraw *x = &g_dbuf[rb][j];                 /* end of the pass */
        if (x->is_blit || x->target || DEPTH_ZID(x->depth) != zid) break;
        if (x->is_clear && (x->clr_flags & 2u) && zid) break; }
    for (; j < n; j++){ const MDraw *x = &g_dbuf[rb][j];
        if (x->is_blit){
            if (!x->blit_src) return !g_hd_brk[j];                        /* the screen is read: single sampled afterwards (unless mid-scene) */
            if (!x->target) return 0;                                     /* copy into the screen: stored into MS color */
            continue; }
        if (!x->target) return 0;                                         /* another screen pass: loads */
    }
    return 1;                                                             /* end of frame: resolve only */
}
static int hd_ms_memless(void){ return hd_ml_hw() && !g_hd_ml_off && hd_mslazy(); }
static int hd_mszstore_all(void){              /* HD_MSZSTORE=1: MS depth always stored (old) */
    static int v = -1; if (v < 0){ const char *e = ENVC("HD_MSZSTORE"); v = (e && *e == '1'); }
    return v;
}
/* When the pass CLOSES: must the samples be stored?  Returns: if a resolve was
 * needed although none was bound at open time (prediction missed), bit0 color /
 * bit1 G: the caller encodes a separate resolve pass with hd_ms_fallback (its
 * counter is in the report; should be 0). */
static _Atomic unsigned long long g_hd_msfb;
static int hd_ms_end(id<MTLRenderCommandEncoder> enc, int rb, unsigned k, int c_ms, int c_gb, unsigned att){
    if (!c_ms || !hd_mslazy()){ g_hd_msp.on = 0; return 0; }
    unsigned n = g_dn[rb], k0 = k; int store = 0, res = 0, reader = 0, pend = 0;
    for (; k < n; k++){ const MDraw *x = &g_dbuf[rb][k];
        if (x->is_blit){
            if (!x->blit_src){ res = 1; reader = 1; if (g_hd_brk[k]) store = 1; break; }
            if (!x->target){ res = 1; store = 1; break; }
            continue; }
        if (!x->target){ store = 1; break; }
    }
    if (k >= n) res = 1;
    if (g_hd_msp.on && g_hd_msp.ml){
        /* Memoryless pass: the actions were fixed at open time (resolve only or
         * DontCare; set*StoreAction can only be called on a pass opened with
         * Unknown).  Here only whether the prediction held is checked: if not,
         * the samples are lost in this frame (the next pass opens cleared), and
         * memoryless mode is turned off permanently. */
        if (store || !(att & 1u) || (reader && c_gb && !(att & 2u))){
            atomic_fetch_add(&g_hd_ml_miss, 1ull); g_hd_ml_off = 1; }
        g_hd_msp.on = 0;
        return 0;
    }
    if (att & 1u) [enc setColorStoreAction:store ? MTLStoreActionStoreAndMultisampleResolve
                                                 : MTLStoreActionMultisampleResolve atIndex:0];
    else { [enc setColorStoreAction:(store || res) ? MTLStoreActionStore : MTLStoreActionDontCare atIndex:0];
           if (res) pend |= 1; }
    if (c_gb){
        MTLStoreAction g;
        if (att & 2u) g = store ? MTLStoreActionStoreAndMultisampleResolve : MTLStoreActionMultisampleResolve;
        else { g = (store || reader) ? MTLStoreActionStore : MTLStoreActionDontCare; if (reader) pend |= 2; }
        for (int gi = 0; gi < HD_NG; gi++) [enc setColorStoreAction:g atIndex:(NSUInteger)(1 + gi)];
    }
    /* MS DEPTH: used to be an unconditional Store -- 133 MB of memory writes per
     * frame at 4K (~12 GB/s at ~95 fps).  These samples are only loaded by a
     * later multisampled screen pass opened with the same Z; there is no such
     * pass in the measured frames and the game clears Z every frame.  Now: store
     * if it is reused in the same frame; don't store if it is cleared or the
     * screen was read / the frame ended -- but if the pass was opened without
     * clearing (loading), the game relies on persistence, so store. */
    if (g_hd_msp.on && g_hd_msp.dz){
        unsigned zid = g_hd_msp.zid; int sd = -1;
        for (unsigned q = k0; q < n && sd < 0; q++){ const MDraw *x = &g_dbuf[rb][q];
            if (x->is_blit){ if (!x->blit_src && !g_hd_brk[q]) break; continue; }   /* the screen was read: single sampled afterwards */
            if (DEPTH_ZID(x->depth) != zid) continue;
            if (x->is_clear && (x->clr_flags & 2u)) sd = 0;               /* cleared before reuse */
            else if (!x->target) sd = 1;                                  /* a new MS pass with the same Z loads */
        }
        if (sd < 0) sd = !g_hd_msp.zclr;
        if (hd_mszstore_all()) sd = 1;
        [enc setDepthStoreAction:g_hd_msp.res ? (sd ? MTLStoreActionStoreAndMultisampleResolve
                                                    : MTLStoreActionMultisampleResolve)
                                              : (sd ? MTLStoreActionStore : MTLStoreActionDontCare)];
        if (zid < 64u) g_hd_msz_kept[zid] = (uint8_t)sd;
        atomic_fetch_add(sd ? &g_hd_mszst : &g_hd_mszskip, 1ull);
    }
    g_hd_msp.on = 0;
    return pend;
}
static void hd_ms_fallback(id<MTLCommandBuffer> cb, id<MTLTexture> scr, int pend){
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    if (!g_hd_mscol) return;
    rp.colorAttachments[0].texture = g_hd_mscol; rp.colorAttachments[0].resolveTexture = scr;
    rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    rp.colorAttachments[0].storeAction = MTLStoreActionStoreAndMultisampleResolve;
    if (pend & 2) for (int gi = 0; gi < HD_NG; gi++){
        if (!g_hd_gms[gi] || !g_hd_g[gi]) continue;
        rp.colorAttachments[1 + gi].texture = g_hd_gms[gi]; rp.colorAttachments[1 + gi].resolveTexture = g_hd_g[gi];
        rp.colorAttachments[1 + gi].loadAction = MTLLoadActionLoad;
        rp.colorAttachments[1 + gi].storeAction = MTLStoreActionStoreAndMultisampleResolve; }
    [[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
    atomic_fetch_add(&g_hd_msfb, 1ull);
}
static void hd_composite(id<MTLCommandBuffer> cb, int rb, unsigned i, id<MTLTexture> scr,
                         unsigned zid, int refl_drawn){
    id<MTLTexture> zt = (zid && zid < MAXZ) ? g_ztex[zid] : nil;
    int ms = hd_msaa_n() > 1 && g_hd_mscol && !hd_mslazy();     /* in lazy mode single sampled afterwards */
    id<MTLRenderPipelineState> sp = ms ? g_hd_ssr_pso_ms : g_hd_ssr_pso;
    float ivp[16], vp[16], iv[16];
    if (!refl_drawn || !g_hd_cam_ok[rb] || !zt || !sp || !g_hd_g[0] ||
        zt.width != scr.width || zt.height != scr.height) return;
    memcpy(vp, &g_hd_cam[rb][32], sizeof vp);
    for (int r4 = 0; r4 < 4; r4++) vp[r4*4] *= g_hd_cam_k[rb];   /* Hor+ */
    if (!hd_inv4(vp, ivp) || !hd_inv4(&g_hd_cam[rb][0], iv)) return;

    struct { float vp[16], ivp[16], cam[4], prm[4], sz[4], cam2[4], cb[3][4], fogp[4], rt2[4]; } sp2;
    memset(sp2.fogp, 0, sizeof sp2.fogp); memset(sp2.rt2, 0, sizeof sp2.rt2);
    static float cl = -1.f, st = -1.f, md = -1.f, rmd = -1.f, rlk = -1.f;
    if (cl < 0.f){ const char *e = ENVC("HD_SSR_CARLOD"); cl = e ? (float)atof(e) : 1.0f;
                   e = ENVC("HD_SSR_STEPS"); st = e ? (float)atof(e) : 48.f;
                   e = ENVC("HD_SSR_MAXD"); md = e ? (float)atof(e) : 120.f;
                   e = ENVC("HD_RT_MAXD"); rmd = e ? (float)atof(e) : 600.f;
                   e = ENVC("HD_RT_LOD"); rlk = e ? (float)atof(e) : 0.05f; }
    memcpy(sp2.vp, vp, sizeof vp); memcpy(sp2.ivp, ivp, sizeof ivp);
    sp2.cam[0] = iv[12]; sp2.cam[1] = iv[13]; sp2.cam[2] = iv[14];
    sp2.cam[3] = ENVC("HD_SSR_DEBUG") ? 1.f + (float)atoi(ENVC("HD_SSR_DEBUG")) : 1.f;
    sp2.prm[0] = g_hd_cam[rb][16 + 10];       /* P[2][2] */
    sp2.prm[1] = g_hd_cam[rb][16 + 14];       /* P[3][2] */
    sp2.prm[2] = 1.f; sp2.prm[3] = md;
    sp2.sz[0] = (float)scr.width; sp2.sz[1] = (float)scr.height; sp2.sz[2] = st; sp2.sz[3] = 0.6f;
    sp2.cam2[0] = cl; sp2.cam2[1] = rmd; sp2.cam2[2] = rlk;
    /* --- RAY DISTANCE LIMIT ---------------------------------------------------
     * Measured (2026-10-21, Quick Race capacity mode): ~1.3 / 2.0 ms of the ray
     * kernel comes from the far scene's pixels (road near the horizon: grazing,
     * long rays).  At that distance the reflection is a few pixels and foggy; no
     * ray is cast on surfaces farther than HD_RT_FAR units, the game's own
     * reflection (R0) stays; in the last 30% of the limit the ray result blends
     * softly into R0 (no visible edge).  OFF BY DEFAULT: measured (capacity mode)
     * only -0.1 ms at 60 / 120 / 200 units -- the cost isn't on the far road, the
     * rays are spread over the whole road.  HD_RT_FAR=<units> turns it on.
     * HD_RT_BMIN=<threshold>: no ray on pixels where the game's reflection weight
     * B is below the threshold (change <= B).  Measured: -0.8% at 0.02, -3.3%
     * Mcycles at 0.06 (0.06 cuts visible reflection) -- B is large on most road
     * pixels; off by default. */
    { static float rfar = -1.f;
      if (rfar < 0.f){ const char *e = ENVC("HD_RT_FAR"); rfar = e ? (float)atof(e) : 0.f; if (rfar < 0.f) rfar = 0.f;
          fprintf(stderr, "[HD] ray distance limit: %s (%.0f units)\n", rfar > 0.f ? "ON" : "OFF", (double)rfar); }
      sp2.rt2[0] = abt_off("RTFAR") ? 0.f : rfar; sp2.rt2[1] = 0.7f; }
    { static float bmin = -1.f;
      if (bmin < 0.f){ const char *e = ENVC("HD_RT_BMIN"); bmin = e ? (float)atof(e) : 0.f; if (bmin < 0.f) bmin = 0.f; }
      sp2.rt2[2] = abt_off("RTBMIN") ? 0.f : bmin; }
    sp2.cam2[3] = ENVC("HD_RT_PLAIN") ? 2.f : ENVC("HD_RT_NOTEX") ? 1.f : 0.f;     /* diagnostic */
    if (g_ab_mode == 2) sp2.cam2[3] = 2.f;
    for (int a3 = 0; a3 < 3; a3++){ memcpy(sp2.cb[a3], &g_hd_cube[rb][a3*3], 12); sp2.cb[a3][3] = 0.f; }
    static int rsc_env = -1;  /* HD_RT_SCALE=1..4: ray buffer divisor (1 = full resolution) */
    /* AUTOMATIC if not given: the ray count shouldn't grow with resolution (2 at
     * 1080p, 3 at 2560 and 3024 width, 4 at 4K -> always a ray buffer ~1000 pixels
     * wide).  Measured (3024x1964, dry weather): at scale 2 tracing + shading ~6 ms. */
    if (rsc_env < 0){ const char *e = ENVC("HD_RT_SCALE"); int v = e ? atoi(e) : 0; rsc_env = (v >= 1 && v <= 4) ? v : 0; }
    int rsc = rsc_env;
    if (!rsc){ int a = (int)((scr.width + 480) / 960); rsc = a < 2 ? 2 : a > 4 ? 4 : a; }
    sp2.cb[0][3] = (float)rsc;

    /* ray tracing: capture + build the structure + cast rays (SSR on failure) */
    unsigned rt_tris = 0, rt_nt = 0;
    if (hd_rt_on() && g_ab_mode != 5 && g_ab_mode != 6){
        hd_rt_init();
        float p00 = g_hd_cam[rb][16] * g_hd_cam_k[rb], p11 = g_hd_cam[rb][16 + 5];
        if (g_rt_early.ok){              /* see RAY TRACING AT THE START OF THE FRAME */
            g_rt_early.ok = 0; rt_tris = g_rt_early.tris; rt_nt = g_rt_early.nt;
            memcpy(sp2.fogp, g_rt_early.fogp, sizeof sp2.fogp); sp2.cb[1][3] = (float)g_rt_only_static; }
        else if (g_rt_pso && p00 != 0.f && p11 != 0.f)
            { rt_tris = hd_rt_frame(cb, rb, i, iv, vp, ivp, 1.f / p00, 1.f / p11, &rt_nt, sp2.fogp); sp2.cb[1][3] = (float)g_rt_only_static; }
        if (rt_tris){
            NSUInteger ow = (scr.width + rsc - 1) / rsc, oh = (scr.height + rsc - 1) / rsc;
            if (!g_rt_out || g_rt_out.width != ow || g_rt_out.height != oh){
                MTLTextureDescriptor *od = [MTLTextureDescriptor
                    texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                    width:ow height:oh mipmapped:NO];
                od.usage = MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead;
                od.storageMode = MTLStorageModePrivate;
                g_rt_out = [g_dev newTextureWithDescriptor:od];
            }
            /* HD_RTPROBE2 (at the DUMPFRAME frame): the SAME kernel + the SAME data, timed
             * alone OUTSIDE the frame.  For comparing with the in-frame cost. */
            if (g_hd_dump_now && ENVC("HD_RTPROBE2")){
                static int once = 0;
                if (!once || ENVC("HD_RTPROBE2_HER")){ once = 1;
                    id<MTLCommandBuffer> pb = [g_queue commandBuffer]; unsigned nt2 = 0;
                    unsigned t2 = hd_rt_frame(pb, rb, i, iv, vp, ivp, 1.f / p00, 1.f / p11, &nt2, sp2.fogp);
                    [pb commit]; [pb waitUntilCompleted];
                    fprintf(stderr, "[RT2] capture+build (separate buffer): %.2f ms, %u tris\n",
                            (pb.GPUEndTime - pb.GPUStartTime) * 1e3, t2);
                    double best[3] = { 1e30, 1e30, 1e30 }, sum[3] = { 0, 0, 0 };
                    for (int it = 0; it < 19; it++){       /* kernel variants alternating, same data */
                        int kv = g_rt_kern2 ? 1 + (it & 1) : 1;
                        g_rt_kforce = kv;
                        id<MTLCommandBuffer> tb = [g_queue commandBuffer];
                        hd_rt_trace(tb, &sp2, sizeof sp2, zt, ow, oh, nt2);
                        [tb commit]; [tb waitUntilCompleted];
                        double msx = (tb.GPUEndTime - tb.GPUStartTime) * 1e3;
                        if (it < 2) continue;
                        if (msx < best[kv]) best[kv] = msx; sum[kv] += msx;
                    }
                    g_rt_kforce = 0;
                    fprintf(stderr, "[RT2] frame %ld ray kernel (outside the frame, alone): old best %.3f mean %.3f ms | RT_V2 best %.3f mean %.3f ms\n",
                            (long)atomic_load(&g_frames_drawn), best[1], sum[1] / (g_rt_kern2 ? 9 : 17), best[2], g_rt_kern2 ? sum[2] / 8 : 0.0);
                    { rt_tris = hd_rt_frame(cb, rb, i, iv, vp, ivp, 1.f / p00, 1.f / p11, &rt_nt, sp2.fogp); sp2.cb[1][3] = (float)g_rt_only_static; }
                }
            }
            /* --- HALF RAYS ----------------------------------------------------
             * The ray kernel (~2.5 ms at 3024x1964) is the biggest item of the
             * reflection system.  Every frame only one half of a checkerboard is
             * traced; the other half is the previous frame's result left in the
             * ray texture (8 ms at 120 fps).  The composite's 4-point upscale mixes
             * the two anyway.  If the texture was just created that frame is traced
             * fully.  HD_RT_HALF=0 old. */
            static int half = -1; static unsigned hpar; static id<MTLTexture> hseen;
            if (half < 0){ const char *e = ENVC("HD_RT_HALF"); half = (e && *e == '0') ? 0 : 1; }
            NSUInteger tw2 = ow;
            if (half && hseen == g_rt_out){ sp2.cb[2][3] = (float)(1u + (hpar++ & 1u)); tw2 = (ow + 1) / 2; }
            hseen = g_rt_out;
            hd_rt_trace(cb, &sp2, sizeof sp2, zt, tw2, oh, rt_nt);
            sp2.cb[2][3] = 0.f;
            sp = ms ? g_rt_pso_ms : g_rt_pso;
        }
    }
    /* HD_COMP=0 (diagnostic): skip the scene copy + composite pass.  Together with
     * HD_RT_STAGE, to measure the GPU cost of the steps one by one (no reflection in the image). */
    { static int comp = -1; if (comp < 0){ const char *e = ENVC("HD_COMP"); comp = (e && *e == '0') ? 0 : 1; }
      if (!comp) return;
      if (g_ab_mode == 1 || g_ab_mode == 2 || g_ab_mode == 3 || g_ab_mode == 4 || g_ab_mode == 6) return; }
    int fb = rt_tris && !ms && g_rt_pso_fb;        /* see COPY-FREE COMPOSITE */
    if (fb){
        MTLRenderPassDescriptor *sr = [MTLRenderPassDescriptor renderPassDescriptor];
        sr.colorAttachments[0].texture = scr;
        sr.colorAttachments[0].loadAction = MTLLoadActionLoad;
        sr.colorAttachments[0].storeAction = MTLStoreActionStore;
        pp_rp(sr, "composite: RT (copy-free)");
        id<MTLRenderCommandEncoder> se = [cb renderCommandEncoderWithDescriptor:sr];
        [se setRenderPipelineState:g_rt_pso_fb];
        [se setFragmentTexture:zt atIndex:1];
        [se setFragmentTexture:g_hd_g[0] atIndex:2];
        [se setFragmentTexture:g_hd_g[1] atIndex:3];
        [se setFragmentTexture:g_hd_g[2] atIndex:4];
        [se setFragmentTexture:g_rt_out atIndex:5];
        [se setFragmentBytes:&sp2 length:sizeof sp2 atIndex:0];
        [se drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [se endEncoding];
        atomic_fetch_add(&g_hd_ssr_n, 1ull);
        return;
    }
    if (!g_hd_scenecopy || g_hd_scenecopy.width != scr.width || g_hd_scenecopy.height != scr.height){
        MTLTextureDescriptor *cd = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:scr.pixelFormat
            width:scr.width height:scr.height mipmapped:YES];
        cd.mipmapLevelCount = 4;     /* SSR car: light soft reflection */
        cd.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        cd.storageMode = MTLStorageModePrivate;
        g_hd_scenecopy = [g_dev newTextureWithDescriptor:cd];
    }
    id<MTLBlitCommandEncoder> sc = pp_blit(cb, "composite: scene copy");
    [sc copyFromTexture:scr sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
             sourceSize:MTLSizeMake(scr.width, scr.height, 1)
              toTexture:g_hd_scenecopy destinationSlice:0 destinationLevel:0
      destinationOrigin:MTLOriginMake(0,0,0)];
    if (!rt_tris) [sc generateMipmapsForTexture:g_hd_scenecopy];     /* only SSR reads it */
    [sc endEncoding];
    MTLRenderPassDescriptor *sr = [MTLRenderPassDescriptor renderPassDescriptor];
    if (ms){ sr.colorAttachments[0].texture = g_hd_mscol;
             sr.colorAttachments[0].resolveTexture = scr;
             sr.colorAttachments[0].storeAction = MTLStoreActionStoreAndMultisampleResolve; }
    else { sr.colorAttachments[0].texture = scr;
           sr.colorAttachments[0].storeAction = MTLStoreActionStore; }
    sr.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    pp_rp(sr, rt_tris ? "composite: RT" : "composite: SSR");
    id<MTLRenderCommandEncoder> se = [cb renderCommandEncoderWithDescriptor:sr];
    [se setRenderPipelineState:sp];
    [se setFragmentTexture:g_hd_scenecopy atIndex:0];
    [se setFragmentTexture:zt atIndex:1];
    [se setFragmentTexture:g_hd_g[0] atIndex:2];
    [se setFragmentTexture:g_hd_g[1] atIndex:3];
    [se setFragmentTexture:g_hd_g[2] atIndex:4];
    if (rt_tris) [se setFragmentTexture:g_rt_out atIndex:5];
    [se setFragmentBytes:&sp2 length:sizeof sp2 atIndex:0];
    [se drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [se endEncoding];
    atomic_fetch_add(&g_hd_ssr_n, 1ull);
}
#endif
/* --- PASS MERGING (both builds) ------------------------------------------
 * At 3024x1964 in the normal build the screen was drawn in 5 separate passes per
 * frame (PASSPROF content dump, 2026-10-15): an empty pass with only a Clear, the
 * main scene, a SINGLE-draw second scene pass because the light glare target came
 * in between, the post-process quad and a separate UI pass because only the Z
 * binding changed.  Every separate pass reads and writes full-screen color (+ Z)
 * (~24 MB each).  Three changes, none of them changes a pixel:
 *  1) pass_reorder: the intermediate pass splitting the scene is deferred (below,
 *     with its rules).
 *  2) pass_clear_only: a screen pass with only a Clear isn't opened; the screen is
 *     cleared with the same color on its first real use anyway, the Z clear waits
 *     in g_zpend.
 *  3) A draw that neither tests nor writes Z doesn't open a new pass just because
 *     the Z surface is different.
 * Previously 1 and 2 were HD only.  PASSMERGE=0 turns off all three. */
static int pass_merge_on(void){
    static int v = -1;
    if (v < 0){ const char *e = ENVC("PASSMERGE"); v = (e && *e == '0') ? 0 : 1; }
    return v;
}
/* ---- DEFER THE INTERMEDIATE PASS THAT SPLITS THE SCENE -----------------------------------
 * Measured (PASSPROF content dump, race frame): main scene 674 draws in a single
 * screen pass; then a 23-draw off-screen pass (light glares: TESTS the scene's Z,
 * doesn't write Z), then a SINGLE draw to the screen, then the post-process.
 * Because of that single draw the scene opened a second screen pass: 4x color +
 * 3 G-buffers were stored and loaded back (~200 MB write + ~200 MB read).
 * Pattern:  ... screen | R = off-screen run (single target T) | S = screen run ...
 * If R doesn't write / clear Z and S doesn't write Z, doesn't sample T, uses the
 * same Z surface, the two runs swap places: S writes to the screen's color (R
 * can't read it), R writes only to T (S doesn't read it), neither changes Z
 * -> the result is identical pixel for pixel, but the scene is a SINGLE pass.
 * HD_REORDER=0 turns it off. */
static _Atomic unsigned long long g_hd_reord;
static void pass_reorder(int rb){
    static int on = -1; if (on < 0){ const char *e = ENVC("HD_REORDER"); on = (e && *e == '0') ? 0 : 1; }
    if (!on || !pass_merge_on()) return;
    unsigned n = g_dn[rb]; MDraw *D = g_dbuf[rb];
    static MDraw tmp[64];
    for (unsigned a = 1; a + 1 < n; a++){
        const MDraw *p = &D[a - 1];
        if (p->target || p->is_blit || p->is_clear) continue;                 /* previous: screen draw */
        if (!D[a].target || D[a].is_blit || D[a].is_clear) continue;          /* a: start of the off-screen run */
        unsigned T = D[a].target, tid = TGT_ID(T), zs = DEPTH_ZID(p->depth), b = a, c;
        int ok = 1;
        for (; b < n; b++){ const MDraw *x = &D[b];
            if (x->target != T || x->is_blit) break;
            if (x->is_clear || (x->depth & 2u)){ ok = 0; break; } }
        if (!ok || b >= n || b - a > 512u) continue;
        for (c = b; c < n && c - b < 64u; c++){ const MDraw *x = &D[c];
            if (x->target || x->is_blit || x->is_clear) break;
            if (DEPTH_ZID(x->depth) != zs || (x->depth & 2u) ||
                x->tex == tid || x->tex1 == tid || x->tex2 == tid || x->tex3 == tid){ ok = 0; break; } }
        if (!ok || c == b) continue;
        if (c < n && !D[c].target && !D[c].is_blit && !D[c].is_clear) continue;   /* S exceeded 64: no gain */
        unsigned ns = c - b, nr = b - a;
        memcpy(tmp, &D[b], (size_t)ns * sizeof(MDraw));
        memmove(&D[a + ns], &D[a], (size_t)nr * sizeof(MDraw));
        memcpy(&D[a], tmp, (size_t)ns * sizeof(MDraw));
        atomic_fetch_add(&g_hd_reord, 1ull);
        a = c - 1;
    }
}
/* 1 if the draw at i would open a pass with only a Clear on the screen: the first
 * real work after it is on another target, and the screen gets drawn to again
 * before it is read. */
static int pass_clear_only(int rb, unsigned i){
    unsigned n = g_dn[rb], j = i;
    const MDraw *d0 = &g_dbuf[rb][i];
#if NFS_ENHANCED
    if (!hd_mslazy()) return 0;
#endif
    if (!pass_merge_on() || d0->target || !d0->is_clear || d0->is_blit) return 0;
    while (j < n && g_dbuf[rb][j].is_clear && !g_dbuf[rb][j].target && !g_dbuf[rb][j].is_blit) j++;
    if (j >= n) return 0;
    if (!g_dbuf[rb][j].target) return 0;                          /* the pass continues with a real draw (or a screen blit) */
    for (unsigned k = j; k < n; k++){ const MDraw *x = &g_dbuf[rb][k];
        if (x->is_blit){ if (!x->blit_src || !x->target) return 0; continue; }
        if (!x->target) return 1;
    }
    return 0;
}

/* A REAL cube texture.  The game draws the car environment map into the 6
 * FACES separately (measured: SetRenderTarget with 6 different cube surfaces),
 * then binds the texture to stage 0 and samples the reflection.  It used to
 * create a plain 2D texture, i.e. all six faces were written on top of each
 * other into the same surface. */
unsigned metal_create_texture_cube(unsigned edge, unsigned levels,
                                   unsigned d3dfmt, unsigned is_rt){
    if (!g_dev) return 0;
    if (g_texn >= tex_limit()){ tex_table_full(); return 0; }
    unsigned blk, bpb;
    MTLPixelFormat pf = d3dfmt_to_mtl(d3dfmt, &blk, &bpb);
    if (pf == MTLPixelFormatInvalid) return 0;
    if (!levels) levels = 1;
    MTLTextureDescriptor *td =
        [MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:pf
                              size:edge mipmapped:(levels > 1)];
    td.mipmapLevelCount = levels;
    td.usage = MTLTextureUsageShaderRead | (is_rt ? MTLTextureUsageRenderTarget : 0);
    /* NOT Private: the game also writes to the cube faces with LockRect/UnlockRect
     * and replaceRegion is invalid on a Private texture (measured:
     * _validateReplaceRegion assert, crash at frame 9). */
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> t = [g_dev newTextureWithDescriptor:td];
    if (!t) return 0;
    tex_zero_all(t, edge, edge, levels, 6, blk, bpb);
    unsigned id_ = g_texn++;
    g_tex[id_] = t;
    g_tex_cube[id_] = 1;
    g_tex_isrt[id_] = is_rt ? 1 : 0;
    atomic_fetch_add(&g_tex_created, 1); if (g_perflog) perflog_event(PL_TEX, 0, 0);
    return id_;
}
int metal_tex_is_cube(unsigned id_){
    return (id_ && id_ < MAXTEX) ? g_tex_cube[id_] : 0;
}

static unsigned g_texup_n[MAXTEX];            /* TEXUPSAY=1 (diagnostic): upload count per texture */
void metal_upload_texture_face(unsigned id_, unsigned level, unsigned face,
                               const void *data, unsigned pitch,
                               unsigned w, unsigned h, unsigned d3dfmt){
    if (!id_ || id_ >= MAXTEX || !g_tex[id_] || !data) return;
    g_texup_n[id_]++;
    unsigned blk, bpb;
    if (d3dfmt_to_mtl(d3dfmt, &blk, &bpb) == MTLPixelFormatInvalid) return;
    unsigned lw = w >> level, lh = h >> level;
    if (!lw) lw = 1; if (!lh) lh = 1;
    MTLRegion rg = MTLRegionMake2D(0, 0, lw, lh);
    struct timespec _u0; if (g_perflog) clock_gettime(CLOCK_MONOTONIC, &_u0);
    /* see ORDERED TEXTURE UPLOADS: second and later uploads go into the draw list */
    int queued = 0;
    if (texord_on() && g_texup_n[id_] >= 2 && g_upbuf[0]){
        unsigned rows = (blk == 1) ? lh : (lh + blk - 1) / blk, sz = pitch * rows;
        if (g_tex_cube[id_] && face > 5u) face = 0u;
        pthread_mutex_lock(&g_qlock);
        int b = g_cur;
        unsigned off = (g_upn[b] + 255u) & ~255u;
        if (off + sz <= MAXUP && g_dn[b] < MAXD){
            memcpy(g_upbuf[b] + off, data, sz); g_upn[b] = off + sz;
            MDraw *D = &g_dbuf[b][g_dn[b]];
            memset(D, 0, sizeof *D);
            D->is_up = 1; D->tex = id_; D->target = 0xFFFFFFFFu;     /* so it matches no target */
            D->up_level = level; D->up_face = g_tex_cube[id_] ? face : 0u;
            D->up_w = lw; D->up_h = lh; D->up_pitch = pitch; D->rawoff = off;
            g_dn[b]++; queued = 1;
            atomic_fetch_add(&g_up_ord, 1ull);
        } else atomic_fetch_add(&g_up_ovf, 1ull);
        pthread_mutex_unlock(&g_qlock);
    }
    if (queued) { }
    else if (g_tex_cube[id_]){
        if (face > 5u) face = 0u;
        [g_tex[id_] replaceRegion:rg mipmapLevel:level slice:face
                        withBytes:data bytesPerRow:pitch bytesPerImage:0];
    } else {
        [g_tex[id_] replaceRegion:rg mipmapLevel:level withBytes:data bytesPerRow:pitch];
    }
    if (g_perflog){ struct timespec _u1; clock_gettime(CLOCK_MONOTONIC, &_u1);
        perflog_event(PL_TEXUP, (uint64_t)(_u1.tv_sec - _u0.tv_sec) * 1000000000ull + (uint64_t)_u1.tv_nsec - (uint64_t)_u0.tv_nsec,
                      (uint64_t)pitch * (blk == 1 ? lh : (lh + blk - 1) / blk)); }
    if (level < 16u) g_tex_lvlmask[id_] |= 1u << level;
    atomic_fetch_add(&g_tex_uploaded, 1);
}
void metal_upload_texture(unsigned id_, unsigned level, const void *data,
                          unsigned pitch, unsigned w, unsigned h, unsigned d3dfmt){
    metal_upload_texture_face(id_, level, 0u, data, pitch, w, h, d3dfmt);
}
/* Compare the declared mip count with the levels ACTUALLY uploaded.
 * An unuploaded level = undefined memory; when that level gets selected at
 * triangle edges it shows up on screen as 1-pixel boxes/diagonals. */
void metal_miplevel_report(void){
    unsigned bad = 0, tot = 0;
    for (unsigned i = 1; i < MAXTEX && i < g_texn; i++){
        if (!g_tex[i]) continue;
        unsigned lv = (unsigned)g_tex[i].mipmapLevelCount;
        if (lv <= 1) continue;
        /* render target: the levels are generated by the GPU (HD road reflection, generateMipmaps),
         * not uploaded -- if counted, every HD session reported a false "MISSING" */
        if (g_tex[i].usage & MTLTextureUsageRenderTarget) continue;
        tot++;
        unsigned want = (lv >= 16u) ? 0xFFFFu : ((1u << lv) - 1u);
        if ((g_tex_lvlmask[i] & want) != want){
            if (bad < 6)
                fprintf(stderr,"    texture %u: %u levels declared, uploaded mask 0x%04x\n",
                        i, lv, g_tex_lvlmask[i]);
            bad++;
        }
    }
    fprintf(stderr,"  mip levels        : %u multi-level textures, %u MISSING\n", tot, bad);
}
unsigned metal_level_pitch(unsigned w, unsigned level, unsigned d3dfmt){
    unsigned blk, bpb;
    if (d3dfmt_to_mtl(d3dfmt, &blk, &bpb) == MTLPixelFormatInvalid) return 0;
    unsigned lw = w >> level; if (!lw) lw = 1;
    if (blk == 1) return lw * bpb;
    return ((lw + blk - 1) / blk) * bpb;
}
unsigned metal_level_rows(unsigned h, unsigned level, unsigned d3dfmt){
    unsigned blk, bpb;
    if (d3dfmt_to_mtl(d3dfmt, &blk, &bpb) == MTLPixelFormatInvalid) return 0;
    unsigned lh = h >> level; if (!lh) lh = 1;
    if (blk == 1) return lh;
    return (lh + blk - 1) / blk;
}

/* D3DCOLOR (0xAARRGGBB, in memory B,G,R,A) -> uchar4 */
/* prim: 4=TRIANGLELIST 5=TRIANGLESTRIP 6=TRIANGLEFAN */
static NSString *kPrelude = @
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct VOut { float4 pos [[position]]; float4 col0; float4 col1;\n"
"              float4 uv0; float4 uv1; float4 uv2; float4 uv3;\n"
"              float fog; };\n"
"struct PSPar { float aref; float usec; float pad0; float pad1;\n"
"               float4 proj; float4 c[32]; float4 fog; };\n"
/* alpha test mode (see ALPHA TEST VARIANT): 1 test, 0 none, 2 diagnostic (+ counter) */
"constant int kAT [[function_constant(0)]];\n"
"constant bool kATD [[function_constant(1)]];\n"
/* color input clamping (see COLOR CLAMPING) */
"constant bool kCS [[function_constant(2)]];\n"
#if NFS_ENHANCED
/* HD: color + G-buffer (c1..c3 = G0..G2; content: see G-BUFFER PACKING) */
"struct PSOut { float4 c0 [[color(0)]]; float4 c1 [[color(1)]]; float4 c2 [[color(2)]];\n"
"               float4 c3 [[color(3)]]; };\n"
/* ray tracing capture (vsemu.c vsk_N and metal.m HDRecC/HDMatC must match) */
"struct HDRec { uint rawoff; uint koff; uint first; uint mat; uint ib; int base; uint isz; uint pad; };\n"
"struct HDAttr { float4 col0; float4 uv1; float4 uv2; float2 uv0; float fog; float pad; };\n"
"struct HDCap { float4x4 invV; float4 prm; uint first; uint count; uint p0; uint p1; };\n"
#endif
;

static NSString *kShaderSrc = @
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct VIn  { float4 pos [[attribute(0)]]; float4 col0 [[attribute(1)]];\n"
"              float4 col1 [[attribute(2)]];\n"
"              float4 uv0 [[attribute(3)]]; float4 uv1 [[attribute(4)]];\n"
"              float4 uv2 [[attribute(5)]]; float4 uv3 [[attribute(6)]]; };\n"
"struct VOut { float4 pos [[position]]; float4 col0; float4 col1;\n"
"              float4 uv0; float4 uv1; float4 uv2; float4 uv3;\n"
"              float fog; };\n"
"struct PSPar { float aref; float usec; float pad0; float pad1;\n"
"               float4 proj; float4 c[32]; float4 fog; };\n"
"constant int kAT [[function_constant(0)]];\n"
"constant bool kATD [[function_constant(1)]];\n"
"constant bool kCS [[function_constant(2)]];\n"
"vertex VOut vmain(VIn v [[stage_in]]) {\n"
"  VOut o;\n"
"  o.pos = v.pos; o.col0 = v.col0; o.col1 = v.col1;\n"
"  o.uv0 = v.uv0; o.uv1 = v.uv1; o.uv2 = v.uv2; o.uv3 = v.uv3;\n"
"  o.fog = 1.0;\n"
"  return o;\n"
"}\n"
"fragment float4 fmain(VOut i [[stage_in]], texture2d<float> t [[texture(0)]],\n"
"                      texture2d<float> t1 [[texture(1)]],\n"
"                      texture2d<float> t2 [[texture(2)]],\n"
"                      texture2d<float> t3 [[texture(3)]],\n"
"                      sampler sm [[sampler(0)]],\n"
"                      constant PSPar &P [[buffer(0)]],\n"
"                      device atomic_uint *atdbg [[buffer(6), function_constant(kATD)]]) {\n"
"  float4 c = (kCS ? saturate(i.col0) : i.col0) * t.sample(sm, i.uv0.xy);\n"
"  if (kAT == 1 && c.a < P.aref) discard_fragment();\n"
"  bool at_kill = (kAT == 2 && c.a < P.aref);\n"
"  c.rgb = mix(P.fog.rgb, c.rgb, mix(1.0, saturate(i.fog), P.fog.a));\n"
"  if (kATD) { if (at_kill) { atomic_fetch_add_explicit(&atdbg[0], 1u, memory_order_relaxed);"
" c = float4(1.0, 0.0, 1.0, 1.0); }\n"
"    if (any(i.col0 < 0.0) || any(i.col0 > 1.0) || any(i.col1 < 0.0) || any(i.col1 > 1.0))"
" atomic_fetch_add_explicit(&atdbg[4096], 1u, memory_order_relaxed); }\n"
"  return c;\n"
"}\n";

void metal_draw_tv(const TVert *src, unsigned nverts, unsigned prim, unsigned tex){
    g_nc_ok = 0;
    if (!g_vbuf[0] || nverts < 3) return;
    unsigned tris = (prim == 4) ? nverts/3 : nverts - 2;
    if (!tris) return;
    pthread_mutex_lock(&g_qlock);
    int b = g_cur;
    if (g_vn[b] + tris*3 <= MAXV && g_dn[b] < MAXD){
        MVert *dst = g_vbuf[b] + g_vn[b];
        for (unsigned t = 0; t < tris; t++){
            unsigned idx[3];
            if (prim == 6){ idx[0]=0;     idx[1]=t+1; idx[2]=t+2; }        /* FAN   */
            else if (prim == 5){                                          /* STRIP */
                if (t & 1){ idx[0]=t+1; idx[1]=t;   idx[2]=t+2; }
                else      { idx[0]=t;   idx[1]=t+1; idx[2]=t+2; }
            }
            else { idx[0]=t*3; idx[1]=t*3+1; idx[2]=t*3+2; }              /* LIST  */
            for (int k = 0; k < 3; k++) *dst++ = src[idx[k]];
        }
        /* CAREFUL: the draw buffer is reused ACROSS FRAMES.
         * If a new field is added and not assigned here the previous frame's
         * value stays.  is_blit was such a field: a slot that was a blit once
         * counted as a blit forever and replaced the real draw with a stale
         * texture (the screen turned into a mosaic over time).  Zero all of it
         * before recording. */
        memset(&g_dbuf[b][g_dn[b]], 0, sizeof g_dbuf[b][g_dn[b]]);
        g_dbuf[b][g_dn[b]].first  = g_vn[b];
        g_dbuf[b][g_dn[b]].count  = tris*3;
        g_dbuf[b][g_dn[b]].tex    = tex;
        g_dbuf[b][g_dn[b]].target = g_cur_target;
        g_dbuf[b][g_dn[b]].is_clear = 0;
        g_dbuf[b][g_dn[b]].blend = g_cur_blend;
        g_dbuf[b][g_dn[b]].bcol  = g_cur_bcol;
        g_dbuf[b][g_dn[b]].cull  = g_cur_cull;
        g_dbuf[b][g_dn[b]].depth = g_cur_depth;
        g_dbuf[b][g_dn[b]].tex1 = g_cur_tex1;
        g_dbuf[b][g_dn[b]].tex2 = g_cur_tex2;
        g_dbuf[b][g_dn[b]].tex3 = g_cur_tex3;
        g_dbuf[b][g_dn[b]].ps   = (g_cur_ps > 0 && g_cur_ps < PSCUBE &&
                                   metal_tex_is_cube(tex))
                                    ? g_cur_ps + PSCUBE : g_cur_ps;
#if NFS_ENHANCED
        { unsigned tx4[4] = { tex, g_cur_tex1, g_cur_tex2, g_cur_tex3 };
          g_dbuf[b][g_dn[b]].ps = hd_refl_ps(g_dbuf[b][g_dn[b]].ps, g_cur_target, tx4); }
#endif
        g_dbuf[b][g_dn[b]].psci = psc_put(b);
        memcpy(g_dbuf[b][g_dn[b]].c0, g_cur_psc[0], sizeof g_cur_psc[0]);
        g_dbuf[b][g_dn[b]].alpha_ref = g_cur_aref;
        memcpy(g_dbuf[b][g_dn[b]].fogv, g_cur_fog, sizeof g_cur_fog);
        memcpy(g_dbuf[b][g_dn[b]].proj, g_cur_proj, sizeof g_cur_proj);
        g_dbuf[b][g_dn[b]].vsid = 0;
        g_dbuf[b][g_dn[b]].rawoff = 0;
        g_dbuf[b][g_dn[b]].koff = 0;
        memcpy(g_dbuf[b][g_dn[b]].smp, g_cur_smp, sizeof g_cur_smp);
        g_dn[b]++;
        g_vn[b] += tris*3;
    }
    pthread_mutex_unlock(&g_qlock);
}

/* --- guest side API (callable from any thread) --- */
void metal_set_clear_color(unsigned argb){
    atomic_store(&g_clear_r, ((argb >> 16) & 0xFF) / 255.0f);
    atomic_store(&g_clear_g, ((argb >>  8) & 0xFF) / 255.0f);
    atomic_store(&g_clear_b, ((argb      ) & 0xFF) / 255.0f);
}
void metal_begin_frame(void){ }

/* Guest: blend state of the following draws. */
void metal_set_blend(unsigned key, float alpha_ref){
    g_cur_blend = key; g_cur_aref = alpha_ref;
}
void metal_set_blendcolor(unsigned argb){ g_cur_bcol = argb; }
void metal_set_fog(unsigned enable, unsigned argb){
    static int off = -1;
    if (off < 0){ const char *e = ENVC("NOFOG"); off = (e && *e && *e!='0'); }
    if (off) enable = 0;
    pthread_mutex_lock(&g_qlock);
    g_cur_fog[0] = (float)((argb>>16)&0xFFu)/255.0f;
    g_cur_fog[1] = (float)((argb>> 8)&0xFFu)/255.0f;
    g_cur_fog[2] = (float)( argb      &0xFFu)/255.0f;
    g_cur_fog[3] = enable ? 1.0f : 0.0f;
    pthread_mutex_unlock(&g_qlock);
}
void metal_set_cull(unsigned m){ g_cur_cull = (m >= 1u && m <= 3u) ? m : 3u; }
/* Guest: depth test/write state.  3D models were drawn in submission order
 * without a Z-buffer -> polygons that should be behind the body were drawn on
 * top, black blotches appeared. */
void metal_set_depth(unsigned mode){ g_cur_dmode = mode & 63u; g_cur_depth = g_cur_dmode | (g_cur_zid << 8); }
/* Guest: SetDepthStencilSurface.  0 = no Z surface (in D3D the Z test/write has
 * no effect), 1 = implicit surface, >=2 created with CreateDepthStencilSurface. */
void metal_set_zsurf(unsigned zid){
    if (zid >= MAXZ) zid = 0;
    g_cur_zid = zid; g_cur_depth = g_cur_dmode | (g_cur_zid << 8);
}
/* Device Reset: the game calls it when it changes resolution/detail.
 * Since it wasn't implemented, changing settings crashed. */
unsigned g_want_w = 0, g_want_h = 0;
/* Metal may not be up yet when the device is created; we store the requested
 * size and use it when the window opens. */
void metal_want_size(unsigned w, unsigned h){
    if (!w || !h || w > 8192u || h > 8192u) return;
    g_want_w = w; g_want_h = h;
}
/* --- Retina: draw at the real pixel density ---------------------------
 * The game's largest mode is 1280x960.  On a 2x screen that surface was shown
 * scaled up 2x, i.e. every pixel was resampled and got blurry (the difference
 * is obvious next to the Windows output).
 * Fix: draw the surface with as many pixels as the screen has from the start.
 * THE RATIO IS KEPT EXACTLY (an exact multiple of the reduced ratio is chosen),
 * so the clip-space correction (aspfix) doesn't kick in -- pure sharpness.
 * Since it is called from the guest thread it does NOT TOUCH NSScreen (only safe
 * on the main thread); it reads the value the main thread measured and stored. */
static unsigned u_gcd(unsigned a, unsigned b){
    while (b){ unsigned t = a % b; a = b; b = t; } return a;
}
/* The window style in ONE place: if the usable area calculation doesn't use the
 * same style as the window itself the title bar margin doesn't match. */
#define NFSU_WIN_STYLE (NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | \
                        NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable)
/* The screen's usable CONTENT area (points) and scale.  Measured with AppKit on
 * the main thread and stored.  A guess was NOT ENOUGH: the title bar was assumed
 * to be 28 points, on a notched screen it was 32; the 3-point difference clipped
 * the window and brought back the resampling (blur). */
/* FBONLY: the frame is drawn into a readable middle texture (the game copies from
 * the back buffer), then one full-screen pass writes it to a render-target-only
 * drawable.  framebufferOnly drawables can be compressed and shown directly; with
 * readable drawables WindowServer had extra work every frame. */
static int fbonly_on(void){
    static int v = -1;
    if (v < 0){ const char *e = getenv("FBONLY"); v = (e && *e && *e != '0') ? 1 : 0; }
    return v;
}
static id<MTLTexture> g_fb_mid;
static id<MTLRenderPipelineState> g_fb_copy_pso;
static id<MTLTexture> fbonly_mid(id<MTLTexture> drw){
    if (!g_fb_mid || g_fb_mid.width != drw.width || g_fb_mid.height != drw.height ||
        g_fb_mid.pixelFormat != drw.pixelFormat){
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:drw.pixelFormat
            width:drw.width height:drw.height mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModePrivate;
        g_fb_mid = [g_dev newTextureWithDescriptor:td];
    }
    return g_fb_mid;
}
static void fbonly_copy(id<MTLCommandBuffer> cb, id<MTLTexture> src, id<MTLTexture> dst){
    if (!g_fb_copy_pso){
        NSString *src_ = @"#include <metal_stdlib>\nusing namespace metal;\n"
            "struct V { float4 p [[position]]; };\n"
            "vertex V fbc_v(uint i [[vertex_id]]) { float2 q = float2(float((i << 1) & 2), float(i & 2));"
            " V o; o.p = float4(q * 2.0 - 1.0, 0.0, 1.0); return o; }\n"
            "fragment float4 fbc_f(V in [[stage_in]], texture2d<float> t [[texture(0)]]) {"
            " return t.read(uint2(in.p.xy)); }\n";
        NSError *e = nil;
        id<MTLLibrary> lib = [g_dev newLibraryWithSource:src_ options:nil error:&e];
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [lib newFunctionWithName:@"fbc_v"];
        pd.fragmentFunction = [lib newFunctionWithName:@"fbc_f"];
        pd.colorAttachments[0].pixelFormat = dst.pixelFormat;
        g_fb_copy_pso = [g_dev newRenderPipelineStateWithDescriptor:pd error:&e];
        if (!g_fb_copy_pso){ fprintf(stderr, "metal: FBONLY copy pipeline failed\n"); return; }
    }
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = dst;
    rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
    [en setRenderPipelineState:g_fb_copy_pso];
    [en setFragmentTexture:src atIndex:0];
    [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [en endEncoding];
}
static _Atomic int g_scr_cw = 0, g_scr_ch = 0, g_scr_s100 = 0;
static void scr_cache(void){
    @autoreleasepool {
        NSScreen *sc = [NSScreen mainScreen];
        if (!sc) return;
        NSRect c = [NSWindow contentRectForFrameRect:[sc visibleFrame]
                                           styleMask:NFSU_WIN_STYLE];
        atomic_store(&g_scr_cw, (int)c.size.width);
        atomic_store(&g_scr_ch, (int)c.size.height);
        atomic_store(&g_scr_s100, (int)lround([sc backingScaleFactor] * 100.0));
    }
}
void metal_native_size(unsigned gw, unsigned gh, unsigned *ow, unsigned *oh){
    if (!ow || !oh) return;
    *ow = gw; *oh = gh;
    if (!gw || !gh) return;
    if (atomic_load(&g_fs)){
        double fw = atomic_load(&g_fs_pw), fh = atomic_load(&g_fs_ph);
        if (fw >= 640.0 && fh >= 480.0){
            double maxpix = 6.0e6;
            { const char *e = ENVC("NFSMAXPIX"); if (e && *e) maxpix = atof(e); }
            if (maxpix > 1.0e5 && fw * fh > maxpix){
                double k = sqrt(maxpix / (fw * fh)); fw = floor(fw * k); fh = floor(fh * k); }
            *ow = ((unsigned)fw) & ~1u; *oh = ((unsigned)fh) & ~1u;
            return;
        }
    }
    double s = 1.0, aw, ah;
    int cw = atomic_load(&g_scr_cw), ch = atomic_load(&g_scr_ch);
    int s100 = atomic_load(&g_scr_s100);
    if (cw > 0 && ch > 0 && s100 > 0){
        /* exactly the same limit win_apply_size will apply */
        aw = cw; ah = ch; s = s100 / 100.0;
    } else {
        /* Without the cache, a CoreGraphics estimate (thread-safe). */
        CGDirectDisplayID d = CGMainDisplayID();
        CGRect b = CGDisplayBounds(d);                /* in POINTS */
        if (b.size.width < 320.0 || b.size.height < 240.0) return;
        CGDisplayModeRef m = CGDisplayCopyDisplayMode(d);
        if (m){ size_t pw = CGDisplayModeGetPixelWidth(m);
                if (pw) s = (double)pw / b.size.width;
                CGDisplayModeRelease(m); }
        /* menu bar / notch (<=32) + title bar (<=32) */
        aw = b.size.width; ah = b.size.height - 64.0;
    }
    if (s < 1.0) s = 1.0;
    if (aw < 320.0 || ah < 240.0) return;
    unsigned g = u_gcd(gw, gh); if (!g) return;
    unsigned ax = gw / g, ay = gh / g;                /* reduced ratio */
    double u = fmin(aw * s / (double)ax, ah * s / (double)ay);
    /* GPU load upper limit: ~6 MP is more than enough for a game this old.
     * Unlimited upscaling on a 5K/6K screen eats the frame budget. */
    double maxpix = 6.0e6;
    { const char *e = ENVC("NFSMAXPIX"); if (e && *e) maxpix = atof(e); }
    if (maxpix > 1.0e5){
        double up = sqrt(maxpix / ((double)ax * (double)ay));
        if (up < u) u = up;
    }
    unsigned units = (unsigned)u;
    if (!units) return;
    unsigned w = ax * units, h = ay * units;
    if (w <= gw || h <= gh || w > 8192u || h > 8192u) return;   /* no gain */
    *ow = w; *oh = h;
}
/* --- sampler cache ------------------------------------------- */
#define MAXSMP 64
static struct { unsigned key; id<MTLSamplerState> st; } g_smpc[MAXSMP];
static int g_smpc_n = 0;
unsigned long g_smp_overflow = 0;
static id<MTLSamplerState> samp_for(unsigned k){
    for (int i = 0; i < g_smpc_n; i++) if (g_smpc[i].key == k) return g_smpc[i].st;
    unsigned mn=k&3u, mg=(k>>2)&3u, mp=(k>>4)&3u;
    unsigned au=(k>>6)&7u, av=(k>>9)&7u, al=(k>>12)&7u;
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    /* D3DTEXF: 1=POINT 2=LINEAR 3=ANISOTROPIC */
    sd.minFilter = (mn==1u) ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
    sd.magFilter = (mg==1u) ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
    /* MIPLIN: make the D3DTEXF_POINT mip trilinear.  POINT mip picks the level
     * HARD; at a triangle edge the derivative quad spills outside, so the LOD
     * changes by a hair and in a 2-pixel band right along the edge it jumps to
     * the next mip -> a visible line around every quad's frame and on the
     * diagonal two triangles share. */
    static int mlin = -1;
    if (mlin < 0){ const char *e = ENVC("MIPLIN"); mlin = (e && *e && *e!='0'); }
    sd.mipFilter = ENVC("MIPOFF") ? MTLSamplerMipFilterNotMipmapped
                 : (mp==0u) ? MTLSamplerMipFilterNotMipmapped
                 : (mp==1u && !mlin) ? MTLSamplerMipFilterNearest
                            : MTLSamplerMipFilterLinear;
    NSUInteger an = (NSUInteger)1u << al;
    if (an < 1) an = 1; if (an > 16) an = 16;
    /* LODMAX=<n>: limit mip selection to level n.  To measure HOW MUCH the LOD of
     * the edge line jumps. */
    { static float lmx = -1.f;
      if (lmx < 0.f){ const char *e = ENVC("LODMAX"); lmx = e ? (float)atof(e) : 1e9f; }
      if (lmx < 1e8f) sd.lodMaxClamp = lmx; }
    { static int noan = -1;
      if (noan < 0){ const char *e = ENVC("NOANISO"); noan = (e && *e && *e!='0'); }
      sd.maxAnisotropy = (!noan && (mn==3u || mg==3u)) ? an : 1; }
    /* D3DTADDRESS: 1=WRAP 2=MIRROR 3=CLAMP 4=BORDER 5=MIRRORONCE */
    MTLSamplerAddressMode am[6] = { MTLSamplerAddressModeRepeat,
        MTLSamplerAddressModeRepeat, MTLSamplerAddressModeMirrorRepeat,
        MTLSamplerAddressModeClampToEdge, MTLSamplerAddressModeClampToZero,
        MTLSamplerAddressModeMirrorClampToEdge };
    sd.sAddressMode = am[au < 6u ? au : 1u];
    sd.tAddressMode = am[av < 6u ? av : 1u];
    id<MTLSamplerState> st = [g_dev newSamplerStateWithDescriptor:sd];
    if (!st) return g_samp;
    if (g_smpc_n < MAXSMP){ g_smpc[g_smpc_n].key = k; g_smpc[g_smpc_n].st = st; g_smpc_n++; }
    else { g_smp_overflow++;      /* cache full: a NEW object for every draw */
           static int warned = 0;
           if (!warned){ warned = 1;
               fprintf(stderr,"metal: ORNEKLEYICI ONBELLEGI DOLDU (%d) - "
                       "a new object is created for every draw\n", MAXSMP); } }
    return st;
}

/* Must be called on the main thread.  Sizes the window so it shows the w x h
 * PIXEL drawing surface 1:1: content = pixels / screen scale.
 * The content used to be set directly to w x h POINTS; on a 2x Retina that meant
 * scaling a 1280-pixel surface up to a 2560-pixel area -- the image was blurry.
 * Now 1 game pixel = 1 screen pixel. */
/* The window size is INDEPENDENT of the resolution: it is sized once on first
 * launch from the screen's usable area, after that only the user changes it.
 * When the resolution changes only the drawing surface (quality) changes; the
 * layer scales it to the window keeping the aspect ratio.  (User request
 * 2026-09-27: "fixed window size, the game should shrink as resolution" -- the
 * same on Windows.)  Since the settings menu rows are generated from the window's
 * ratio there are normally no bars (see com.c modes_refresh). */
static _Atomic int g_win_sized = 0;
static void win_apply_size(unsigned w, unsigned h){
    if (!g_layer) return;
    CGFloat s = g_win ? [g_win backingScaleFactor]
                      : [[NSScreen mainScreen] backingScaleFactor];
    if (s < 1.0) s = 1.0;
    g_layer.contentsScale = s;          /* if left alone it stays 1.0 */
    g_layer.drawableSize  = CGSizeMake((CGFloat)w, (CGFloat)h);
    atomic_store(&g_draw_w, (int)w); atomic_store(&g_draw_h, (int)h);
    if (!g_win) return;
    int fs = atomic_load(&g_fs) || ([g_win styleMask] & NSWindowStyleMaskFullScreen);
    if (!fs && !g_win_sized){
        NSScreen *sc = [g_win screen] ?: [NSScreen mainScreen];
        NSRect vis = [sc visibleFrame];
        NSRect cr  = [g_win contentRectForFrameRect:
                        NSMakeRect(0, 0, vis.size.width, vis.size.height)];
        [g_win setContentSize:NSMakeSize(floor(cr.size.width), floor(cr.size.height))];
        g_win_sized = 1;
    }
    NSSize c = [[g_win contentView] bounds].size;
    atomic_store(&g_cont_w, (int)c.width); atomic_store(&g_cont_h, (int)c.height);
    int exact = fabs(c.width * s - (CGFloat)w) < 1.0 && fabs(c.height * s - (CGFloat)h) < 1.0;
    fprintf(stderr, "metal: %s: drawing %ux%u pixels, content %.0fx%.0f points (scale %.2g) -> %s\n",
            fs ? "fullscreen" : "window", w, h, (double)c.width, (double)c.height, (double)s,
            exact ? "1:1" : "scaled to the window");
}
/* The VIEW (pixels) the settings menu rows are based on: in fullscreen the
 * fullscreen content, in a window the window's current content; if the window
 * isn't sized yet, the size it will get on first launch.  Called from the guest
 * thread, doesn't touch AppKit. */
void metal_view_size(unsigned *ow, unsigned *oh){
    double sc = atomic_load(&g_scr_s100) / 100.0; if (sc < 1.0) sc = 1.0;
    double w = 0, h = 0;
    if (atomic_load(&g_fs) && atomic_load(&g_fs_pw) > 0){
        w = atomic_load(&g_fs_pw); h = atomic_load(&g_fs_ph);
    } else if (g_win_sized){
        w = atomic_load(&g_cont_w) * sc; h = atomic_load(&g_cont_h) * sc;
    } else {
        w = atomic_load(&g_scr_cw) * sc; h = atomic_load(&g_scr_ch) * sc;
    }
    if (w < 320.0 || h < 240.0){ *ow = *oh = 0; return; }
    *ow = ((unsigned)lround(w)) & ~1u; *oh = ((unsigned)lround(h)) & ~1u;
}
void metal_resize(unsigned w, unsigned h){
    if (!w || !h || w > 8192u || h > 8192u) return;
    dispatch_async(dispatch_get_main_queue(), ^{ win_apply_size(w, h); });
}
void metal_set_stage_tex(unsigned t1, unsigned t2, unsigned t3){
    g_cur_tex1 = t1; g_cur_tex2 = t2; g_cur_tex3 = t3;
}
/* The guest's active pixel shader and constants. */
void metal_set_ps(int psid, const float *c32){
    g_cur_ps = psid;
    if (c32 && memcmp(g_cur_psc, c32, sizeof g_cur_psc)){
        memcpy(g_cur_psc, c32, sizeof g_cur_psc); g_psc_ver++; }
}
void metal_set_texproj(const float p4[4]){ memcpy(g_cur_proj, p4, sizeof g_cur_proj); }
/* --- GPU vertex shaders ---------------------------------------
 * Measured: ~45% of the run time was in the vs_run interpreter on the CPU.
 * The game's own bytecode is translated to MSL and compiled here. */
#define MAXVSF 512
static id<MTLFunction> g_vsfn[MAXVSF];
void metal_register_vs(int vsid, const char *msl){
    if (vsid <= 0 || vsid >= MAXVSF || !msl || !g_dev || g_vsfn[vsid]) return;
    @autoreleasepool {
        NSString *src = [kPrelude stringByAppendingString:
                         [NSString stringWithUTF8String:msl]];
        NSError *err = nil;
        struct timespec _c0; if (g_perflog) clock_gettime(CLOCK_MONOTONIC, &_c0);
        id<MTLLibrary> lib = [g_dev newLibraryWithSource:src options:nil error:&err];
        if (g_perflog){ struct timespec _c1; clock_gettime(CLOCK_MONOTONIC, &_c1);
            perflog_event(PL_VS, (uint64_t)(_c1.tv_sec - _c0.tv_sec) * 1000000000ull + (uint64_t)_c1.tv_nsec - (uint64_t)_c0.tv_nsec, 0); }
        if (!lib){
            static int shown = 0;
            if (shown < 5){ shown++;
                fprintf(stderr, "vertex shader %d could not be compiled: %s\n", vsid,
                        err ? [[err localizedDescription] UTF8String] : "?"); }
            return;
        }
        char nm[32]; snprintf(nm, sizeof nm, "vs_%d", vsid);
        g_vsfn[vsid] = [lib newFunctionWithName:[NSString stringWithUTF8String:nm]];
        if (!g_vsfn[vsid])
            fprintf(stderr, "vertex shader %d: function not found\n", vsid);
#if NFS_ENHANCED
        if (vsid < HD_MAXVSI && hd_rt_on()){
            snprintf(nm, sizeof nm, "vsk_%d", vsid);
            id<MTLFunction> kf = [lib newFunctionWithName:[NSString stringWithUTF8String:nm]];
            NSError *ke = nil;
            if (kf) g_hd_vsk[vsid] = [g_dev newComputePipelineStateWithFunction:kf error:&ke];
            if (!g_hd_vsk[vsid]) fprintf(stderr, "[HD] capture kernel vs_%d could not be built: %s\n", vsid,
                                         ke ? [[ke localizedDescription] UTF8String] : "no function");
        }
#endif
    }
}
int metal_vs_ready(int vsid){
    return (vsid > 0 && vsid < MAXVSF && g_vsfn[vsid] != nil) ? 1 : 0;
}

/* Queue the raw (untransformed) vertices + constants; the GPU does the vertex
 * processing.  kdata = c0..c[nk-1], nk = vs_kmax. */
/* Under g_qlock: place the constant block (reuse it if repeated) and fill the
 * draw record.  The caller sets up the vertex source (rawoff / index fields,
 * count) and increments g_dn.  NULL = the constant arena is full. */
/* --- FULL-SCREEN COVERAGE ------------------------------------------------------
 * Does a draw write EVERY pixel of the target (D3D9 pixel center rule) without
 * blending, all 4 channels, without alpha test / texkill / Z test?  Coverage is
 * found in com.c by transforming the vertices on the CPU (only draws with <= 6
 * vertices that satisfy these conditions).  User: SCREEN REDIRECTION.
 * Measured and REJECTED (2026-10-18, PASS LOAD): DontCare instead of Load for the
 * color of passes starting with such a draw, a scratch depth instead of the real
 * Z for passes not using Z.  It was correct by the poison test, but in an
 * in-run A/B the GPU energy was +0.6 +- 0.8 mJ/frame, DRAM traffic
 * +1.6 +- 1.8 / +1.2 +- 1.4 MB/frame: no effect.  The Apple GPU already skips
 * loading a fully covered tile with hidden surface removal, and doesn't move an
 * untouched Z either. */
static uint8_t g_ps_kill[MAXPS];             /* ps contains texkill (discard) */
static uint8_t g_ps_fbok[MAXPS];             /* stage 0 in a single place with TCRD(0) (see POST-PROCESS CHAIN) */
/* com.c: per-vertex oT0..oT3 of the next quad (BEFORE metal_set_next_cov_clip) */
void metal_set_next_cov_uv(const float (*uv)[16], unsigned n){
    g_ncuv_valid = 0;
    if (!uv || n > 6) return;
    memcpy(g_ncuv_in, uv, n * sizeof uv[0]); g_ncuv_n = n; g_ncuv_valid = 1;
}
/* ---- UV SCALE -------------------------------------------------------------
 * With NFSRES / Retina the game draws assuming 1280x960, the full-screen targets
 * (#2/#3/#4, screen) are 3024x1836.  Post-process quads carry the mapping
 * u = (x + 0.5) / 1280 per the D3D9 half-texel rule (measured: u 0.0003906 ..
 * 1.0003906, v 0.0005208 .. 1.0005208).  On the real target this shifts the pixel
 * center (0.68, 0.46) texels from the texel center: every full-resolution pass
 * shifted the image and softened it with bilinear filtering (the main image goes
 * through 4 passes).  We set up the mapping the game intends at the real size:
 * the vertex shader adds the difference (0.5/Wg - 0.5/Wo) to this set, i.e.
 * u = (x + 0.5) / Wo.  Only when: the quad covers the target fully, the target is
 * scaled (game size != real), the UVs at the set's corners follow exactly this
 * pattern.  UVFIX=0 turns it off. */
static unsigned g_uvg_w, g_uvg_h, g_uva_w, g_uva_h;     /* what the game assumes / the real back buffer */
void metal_set_uv_scale(unsigned gw, unsigned gh, unsigned aw, unsigned ah){
    g_uvg_w = gw; g_uvg_h = gh; g_uva_w = aw; g_uva_h = ah;
}
static int uvfix_on(void){
    static int on = -1; if (on < 0){ const char *e = ENVC("UVFIX"); on = (e && *e == '0') ? 0 : 1; }
    return on;
}
static _Atomic unsigned long long g_uvfix_n = 0;
void metal_set_next_cov_clip(const float (*p)[4], unsigned n, unsigned prim){
    g_nc_ok = 0; g_ncuv_ok = 0;
    if (!p || n < 3 || n > 6) return;
    unsigned tri[2][3];
    if (prim == 4u && n == 6){ unsigned a[2][3] = {{0,1,2},{3,4,5}}; memcpy(tri, a, sizeof tri); }
    else if (prim == 5u && n == 4){ unsigned a[2][3] = {{0,1,2},{1,2,3}}; memcpy(tri, a, sizeof tri); }
    else if (prim == 6u && n == 4){ unsigned a[2][3] = {{0,1,2},{0,2,3}}; memcpy(tri, a, sizeof tri); }
    else return;                               /* only a two-triangle quad */
    float x[6], y[6], x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
    for (unsigned i = 0; i < n; i++){
        float w = p[i][3];
        if (!(w > 0.f)) return;
        float z = p[i][2] / w;
        if (!(z >= 0.f && z <= 1.f)) return;   /* near/far plane clipping */
        x[i] = p[i][0] / w; y[i] = p[i][1] / w;
        if (x[i] < x0) x0 = x[i]; if (x[i] > x1) x1 = x[i];
        if (y[i] < y0) y0 = y[i]; if (y[i] > y1) y1 = y[i];
    }
    if (!(x1 > x0 && y1 > y0)) return;
    /* every vertex must be a corner of the rectangle; the two triangles must cover the whole rectangle:
     * three distinct vertices each, 4 corners combined, the two shared vertices opposite (diagonal) */
    unsigned cid[6];
    for (unsigned i = 0; i < n; i++){
        int ex = (x[i] == x0) ? 0 : (x[i] == x1) ? 1 : -1;
        int ey = (y[i] == y0) ? 0 : (y[i] == y1) ? 1 : -1;
        if (ex < 0 || ey < 0) return;
        cid[i] = (unsigned)(ex | (ey << 1));
    }
    unsigned m[2] = {0, 0};
    for (int t = 0; t < 2; t++){
        for (int k = 0; k < 3; k++) m[t] |= 1u << cid[tri[t][k]];
        if (__builtin_popcount(m[t]) != 3) return;
    }
    if ((m[0] | m[1]) != 15u) return;
    unsigned sh = m[0] & m[1];
    if (sh != 0x9u && sh != 0x6u) return;      /* the shared edge must be the diagonal */
    g_nc[0] = x0; g_nc[1] = x1; g_nc[2] = y0; g_nc[3] = y1; g_nc_ok = 1;
    /* UV: vertices on the rectangle's x0/x1 edges must carry the same u, those on
     * y0/y1 the same v (axis-aligned affine mapping); if not, that set is ignored.
     * cuv: stage 0's set (diagnostic / composite); uvb: UV SCALE */
    g_nuvb_mask = 0;
    if (g_ncuv_valid && g_ncuv_n == n){
        int s0 = ((int)g_cur_proj[0]) & 3;
        int full = x0 > -1.f - 1e-6f && x0 < -1.f + 1e-6f && x1 > 1.f - 1e-6f && x1 < 1.f + 1e-6f &&
                   y0 > -1.f - 1e-6f && y0 < -1.f + 1e-6f && y1 > 1.f - 1e-6f && y1 < 1.f + 1e-6f;
        /* is the target scaled: a texture at screen or back buffer size, game size differs */
        int scaled = 0;
        if (uvfix_on() && full && g_uvg_w && g_uva_w && (g_uvg_w != g_uva_w || g_uvg_h != g_uva_h)){
            unsigned t = TGT_ID(g_cur_target);
            if (!g_cur_target) scaled = 1;
            else if (t < MAXTEX && g_tex[t] && g_tex[t].width == g_uva_w && g_tex[t].height == g_uva_h) scaled = 1;
        }
        for (int st = 0; st < 4; st++){
            float u[2] = {0, 0}, v[2] = {0, 0}; int su[2] = {0, 0}, sv[2] = {0, 0}, ok = 1;
            for (unsigned i = 0; i < n && ok; i++){
                int ex = (int)(cid[i] & 1u), ey = (int)(cid[i] >> 1);
                float uu = g_ncuv_in[i][st*4], vv = g_ncuv_in[i][st*4 + 1];
                if (!(g_ncuv_in[i][st*4 + 3] == 1.f)) ok = 0;                /* w != 1: projection */
                if (!su[ex]){ u[ex] = uu; su[ex] = 1; } else if (u[ex] != uu) ok = 0;
                if (!sv[ey]){ v[ey] = vv; sv[ey] = 1; } else if (v[ey] != vv) ok = 0;
            }
            if (!(ok && su[0] && su[1] && sv[0] && sv[1])) continue;
            if (st == s0){ g_ncuv[0] = u[0]; g_ncuv[1] = u[1]; g_ncuv[2] = v[0]; g_ncuv[3] = v[1]; g_ncuv_ok = 1; }
            if (scaled){
                /* the game's half-texel pattern: u(x=-1) = 0.5/Wg, u(x=+1) = 1 + 0.5/Wg,
                 * v(y=+1) = 0.5/Hg, v(y=-1) = 1 + 0.5/Hg */
                float hu = 0.5f / (float)g_uvg_w, hv = 0.5f / (float)g_uvg_h, e = 1e-6f;
                if (fabsf(u[0] - hu) < e && fabsf(u[1] - (1.f + hu)) < e &&
                    fabsf(v[1] - hv) < e && fabsf(v[0] - (1.f + hv)) < e)
                    g_nuvb_mask |= 1u << st;
            }
        }
        if (g_nuvb_mask){
            g_nuvb[0] = 0.5f / (float)g_uva_w - 0.5f / (float)g_uvg_w;
            g_nuvb[1] = 0.5f / (float)g_uva_h - 0.5f / (float)g_uvg_h; }
    }
    g_ncuv_valid = 0;
}
static int halfpx_on(void){                  /* HALFPX=0: old (unshifted) viewport */
    static int hp = -1;
    if (hp < 0){ const char *e = ENVC("HALFPX"); hp = (e && *e == '0') ? 0 : 1; }
    return hp;
}
/* does dr write every pixel of a W x H target (without reading the target's previous content)? */
static int draw_covers(const MDraw *dr, NSUInteger W, NSUInteger H){
    if (!halfpx_on()) return 0;                 /* the rule depends on the half-pixel shift */
    if (dr->is_clear || dr->is_blit || !dr->covok || !dr->count) return 0;
    if (dr->blend & 1u) return 0;                          /* blending reads the target */
    if (((dr->blend >> 14) & 0xFu) != 0xFu) return 0;      /* incomplete write mask */
    if (dr->alpha_ref > 0.f) return 0;                     /* alpha test: discard */
    if (dr->depth & 1u) return 0;                          /* Z test: a losing fragment */
    if (dr->ps > 0 && dr->ps < MAXPS && g_ps_kill[dr->ps]) return 0;   /* texkill */
    /* Covered if the D3D9 pixel center i (integer) is in [X0, X1).  The same rule
     * holds with Metal's half-pixel-shifted viewport (see HALFPX). */
    float X0 = (dr->cov[0] + 1.f) * 0.5f * (float)W, X1 = (dr->cov[1] + 1.f) * 0.5f * (float)W;
    float Y0 = (1.f - dr->cov[3]) * 0.5f * (float)H, Y1 = (1.f - dr->cov[2]) * 0.5f * (float)H;
    return X0 <= 0.f && Y0 <= 0.f && X1 > (float)W - 1.f + 1e-3f && Y1 > (float)H - 1.f + 1e-3f;
}
/* ---- SCREEN REDIRECTION ---------------------------------------------------
 * Race frame: the scene is drawn to the screen, then copied to a SAME SIZE target
 * (#2) (the post-process reads it), and at the very end the screen is rewritten
 * by a full-screen quad.  The copy at 3024x1836 is 22 MB read + 22 MB write,
 * 0.59 ms/frame (PASSPROF, 2026-10-18).  If the scene is drawn straight into that
 * target the copy is unnecessary: same draws, same size, same format, same Z ->
 * same content.
 * Conditions (from the frame's records, before encoding):
 *  - B = the FIRST blit reading the screen; its target T is a 2D render target
 *    texture of the same size and format as the screen;
 *  - within [0,B) T is neither sampled nor a target; no blit touches the screen
 *    or T; there is at least one record to the screen;
 *  - the first draw touching the screen after B overwrites the screen COMPLETELY
 *    (PASS LOAD coverage rule); no screen blit or color clear in between.
 * Encoding: screen records in the range [0,B) are drawn into T (screen semantics
 * kept: the first pass clears with the game's clear color), B is skipped, the
 * screen starts as if used for the first time after B.  SCRREDIR=0 turns it off;
 * SCRREDIR=2 poison test: the first screen pass after B is cleared with magenta
 * (if the coverage is wrong it leaks into the final image).  In HD the screen is
 * multisampled / G-buffered: not tried there, off. */
static int scrredir_mode(void){
    static int m = -1; if (m < 0){ const char *e = ENVC("SCRREDIR"); m = (e && *e) ? atoi(e) : 1; }
    return m;
}
static _Atomic unsigned long long g_scr_redir = 0;
static long scr_redirect_plan(int rb, id<MTLTexture> scr, unsigned *outT){
#if NFS_ENHANCED
    (void)rb; (void)scr; (void)outT; return -1;
#else
    if (!scrredir_mode() || abt_off("SCRREDIR") || !scr || rb < 0) return -1;
    unsigned n = g_dn[rb], B, k; const MDraw *D = g_dbuf[rb];
    for (B = 0; B < n; B++) if (D[B].is_blit && !D[B].blit_src) break;
    if (B >= n || D[B].blit_rect) return -1;
    unsigned T = TGT_ID(D[B].target);
    if (!D[B].target || TGT_FACE(D[B].target) || !T || T >= MAXTEX) return -1;
    id<MTLTexture> tt = g_tex[T];
    if (!tt || tt.width != scr.width || tt.height != scr.height || tt.pixelFormat != scr.pixelFormat ||
        tt.textureType != MTLTextureType2D || !(tt.usage & MTLTextureUsageRenderTarget)) return -1;
    int any = 0;
    for (k = 0; k < B; k++){ const MDraw *x = &D[k];
        if (x->is_blit){ if (!x->target || TGT_ID(x->target) == T || x->blit_src == T) return -1; continue; }
        if (x->target && TGT_ID(x->target) == T) return -1;
        if (!x->is_clear && (x->tex == T || x->tex1 == T || x->tex2 == T || x->tex3 == T)) return -1;
        if (!x->target) any = 1; }
    if (!any) return -1;
    for (k = B + 1; k < n; k++){ const MDraw *x = &D[k];
        if (x->is_blit){ if (!x->blit_src || !x->target) return -1; continue; }
        if (x->target) continue;
        if (x->is_clear){ if (x->clr_flags & 1u) return -1; continue; }   /* only a Z clear passes */
        if (!x->count) continue;
        if (!draw_covers(x, scr.width, scr.height)) return -1;
        *outT = T; return (long)B;
    }
    return -1;
#endif
}

/* ---- HUD ANCHORING (HUDPOS=1) ---------------------------------------------
 * The game lays its HUD out for 640x480; on a wider screen HUDFIX squeezes it
 * into the centered 4:3 area (round gauges stay round).  With HUDPOS the
 * elements on the left go to the left edge and the ones on the right to the
 * right edge, still unstretched.  com.c hands over each HUD draw's 4:3 clip
 * rectangle (only while the world is loaded); before a frame is encoded the
 * draws are joined into elements (below) and each element is moved as one
 * piece, so a text, a gauge, a panel or the mirror never splits. */
static int g_nhud_ok = 0; static float g_nhud[4];
void metal_set_next_hud(float x0, float x1, float y0, float y1){
    g_nhud[0] = x0; g_nhud[1] = x1; g_nhud[2] = y0; g_nhud[3] = y1; g_nhud_ok = 1;
}
static _Atomic unsigned long long g_hud_moved = 0, g_hud_kept = 0;
/* Elements are the connected pieces of the frame's HUD: draws whose rectangles
 * overlap or touch (a song box with its frame pieces and text, a gauge with its
 * needle and digits, the mirror with its frame, a 9-piece panel with its
 * contents) form one element, found with a union-find over the frame's HUD
 * draws.  An element crossing the middle stays; one on the left goes to the
 * left edge, one on the right to the right edge.  If elements chain across
 * the middle the result is the old centered layout, never a split element. */
#define HUD_MAXN 1536
static int hud_find(int *par, int x){ while (par[x] != x){ par[x] = par[par[x]]; x = par[x]; } return x; }
static void hud_anchor_plan(int rb){
    unsigned n = g_dn[rb]; MDraw *D = g_dbuf[rb];
    float asp = atomic_load(&g_ui_asp) / 10000.0f;
    for (unsigned i = 0; i < n; i++) D[i].xsh = 0.f;
    if (!(asp < 0.999f)) return;
    float sh = 1.f - asp;                       /* the 4:3 edge -> the screen edge */
    static unsigned idx[HUD_MAXN]; static int par[HUD_MAXN]; static float bb[HUD_MAXN][4];
    int m = 0;
    for (unsigned i = 0; i < n; i++){
        const MDraw *d = &D[i];
        if (!d->hudok || d->target || d->is_clear || d->is_blit || d->is_up || d->vsid <= 0) continue;
        if (m == HUD_MAXN) return;              /* unusual frame: leave it centered */
        idx[m] = i; par[m] = m; m++;
    }
    if (!m) return;
    const float tol = 0.02f;                    /* touching = within 1/100 of the 4:3 width */
    for (int a = 0; a < m; a++){
        const float *ra = D[idx[a]].hud;
        for (int b = a + 1; b < m; b++){
            const float *rb2 = D[idx[b]].hud;
            if (ra[0] <= rb2[1] + tol && rb2[0] <= ra[1] + tol &&
                ra[2] <= rb2[3] + tol && rb2[2] <= ra[3] + tol){
                int x = hud_find(par, a), y = hud_find(par, b);
                if (x != y) par[y] = x; }
        }
    }
    for (int a = 0; a < m; a++){ bb[a][0] = bb[a][2] = 1e30f; bb[a][1] = bb[a][3] = -1e30f; }
    for (int a = 0; a < m; a++){
        int r = hud_find(par, a); const float *q = D[idx[a]].hud;
        if (q[0] < bb[r][0]) bb[r][0] = q[0]; if (q[1] > bb[r][1]) bb[r][1] = q[1];
        if (q[2] < bb[r][2]) bb[r][2] = q[2]; if (q[3] > bb[r][3]) bb[r][3] = q[3];
    }
    unsigned long long moved = 0, kept = 0;
    for (int a = 0; a < m; a++){
        int r = hud_find(par, a);
        float x0 = bb[r][0], x1 = bb[r][1], c = (x0 + x1) * 0.5f, sx = 0.f;
        if (!(x0 < -0.15f && x1 > 0.15f)){      /* not crossing the middle */
            if (c < -0.25f) sx = -sh; else if (c > 0.25f) sx = sh;
        }
        D[idx[a]].xsh = sx;
        if (sx != 0.f) moved++; else kept++;
    }
    atomic_fetch_add(&g_hud_moved, moved); atomic_fetch_add(&g_hud_kept, kept);
}

static MDraw *raw_rec(int b, unsigned stride, unsigned tex, int vsid,
                      const float *kdata, unsigned nk, unsigned uvbm, const float *uvb){
    unsigned ksz = 16u + nk * 16u;
    /* If the constant block repeats, don't upload again: the game draws the parts
     * of the same object one after another, the block is mostly the same. */
    static unsigned last_koff[2], last_ksz[2]; static int last_vs[2];
    unsigned koff;
    uint8_t hdr[16]; memset(hdr, 0, sizeof hdr); memcpy(hdr, &stride, 4);
    /* TEXCOORDINDEX per stage (2 bits each) -> the vertex shader does the selection
     * itself; on the pixel side the coordinate stays a pure interpolant. */
    { uint32_t ts = 0;
      for (int q = 0; q < 4; q++)
          ts |= ((uint32_t)((int)g_cur_proj[q]) & 3u) << (8*q);
      memcpy(hdr + 4, &ts, 4); }
    /* UV SCALE: pad1 = set mask; the difference itself is in the draw record (VS buffer 3) */
    if (uvbm) memcpy(hdr + 8, &uvbm, 4);
    if (last_ksz[b] == ksz && last_vs[b] == vsid && g_kn[b] &&
        !memcmp(g_kbuf[b] + last_koff[b], hdr, 16) &&
        !memcmp(g_kbuf[b] + last_koff[b] + 16, kdata, nk * 16u)){
        koff = last_koff[b];
    } else {
        koff = (g_kn[b] + 255u) & ~255u;
        if (koff + ksz > MAXKB){ g_ov_kb++; return NULL; }
        memcpy(g_kbuf[b] + koff, hdr, 16);
        memcpy(g_kbuf[b] + koff + 16, kdata, nk * 16u);
        g_kn[b] = koff + ksz;
        last_koff[b] = koff; last_ksz[b] = ksz; last_vs[b] = vsid;
    }
    MDraw *D = &g_dbuf[b][g_dn[b]];
    memset(D, 0, sizeof *D);          /* reused slot: see metal_draw_tv */
    D->tex = tex; D->target = g_cur_target;
    memcpy(D->smp, g_cur_smp, sizeof D->smp);
    D->blend = g_cur_blend; D->depth = g_cur_depth; D->alpha_ref = g_cur_aref;
    D->bcol  = g_cur_bcol; D->cull = g_cur_cull;
    memcpy(D->fogv, g_cur_fog, sizeof D->fogv);
    D->tex1 = g_cur_tex1; D->tex2 = g_cur_tex2; D->tex3 = g_cur_tex3;
    D->ps = (g_cur_ps > 0 && g_cur_ps < PSCUBE && metal_tex_is_cube(tex))
              ? g_cur_ps + PSCUBE : g_cur_ps;
#if NFS_ENHANCED
    { unsigned tx4[4] = { tex, g_cur_tex1, g_cur_tex2, g_cur_tex3 };
      D->ps = hd_refl_ps(D->ps, g_cur_target, tx4); }
#endif
    D->psci = psc_put(b); memcpy(D->c0, g_cur_psc[0], sizeof D->c0);
    memcpy(D->proj, g_cur_proj, sizeof D->proj);
    D->vsid = vsid; D->koff = koff; D->ksz = ksz;
    if (uvbm && uvb){ D->uvbm = uvbm; D->uvb[0] = uvb[0]; D->uvb[1] = uvb[1]; }
    if (g_nhud_ok){ D->hudok = 1; memcpy(D->hud, g_nhud, sizeof D->hud); g_nhud_ok = 0; }
    return D;
}
int metal_draw_raw(const void *src, unsigned nverts, unsigned stride,
                   unsigned prim, unsigned tex, int vsid,
                   const float *kdata, unsigned nk){
    int covok = g_nc_ok; float cov[4]; memcpy(cov, g_nc, sizeof cov); g_nc_ok = 0;
    int cuvok = covok && g_ncuv_ok; float cuv[4]; memcpy(cuv, g_ncuv, sizeof cuv); g_ncuv_ok = 0;
    unsigned uvbm = covok ? g_nuvb_mask : 0u; float uvb[2] = { g_nuvb[0], g_nuvb[1] }; g_nuvb_mask = 0;
    if (!g_rvbuf[0] || nverts < 3 || !stride || stride > 256u) return 0;
    unsigned tris = (prim == 4) ? nverts/3 : nverts - 2;
    if (!tris) return 0;
    const uint8_t *S = (const uint8_t *)src;
    unsigned need = tris * 3u * stride;

    pthread_mutex_lock(&g_qlock);
    int b = g_cur;
    unsigned roff = (g_rvn[b] + 255u) & ~255u;
    if (roff + need > MAXRV){ g_ov_rv++; pthread_mutex_unlock(&g_qlock); return 0; }
    if (g_dn[b] >= MAXD){ pthread_mutex_unlock(&g_qlock); return 0; }
    MDraw *D = raw_rec(b, stride, tex, vsid, kdata, nk, uvbm, uvb);
    if (!D){ pthread_mutex_unlock(&g_qlock); return 0; }
    if (uvbm) atomic_fetch_add(&g_uvfix_n, 1ull);

    uint8_t *dst = g_rvbuf[b] + roff;
    /* TRIANGLE LIST: the source is already contiguous, a single block copy is enough.
     * It used to call memcpy PER VERTEX (measured: ~1250 draws x ~90 vertices per
     * frame = 112 thousand small memcpys/frame; _platform_memmove 4.6% of the guest
     * thread in the profile).  For strips and fans gathering is required since
     * vertices are shared. */
    if (prim == 4){
        memcpy(dst, S, need);
    } else
    for (unsigned t = 0; t < tris; t++){
        unsigned idx[3];
        if (prim == 6){ idx[0]=0; idx[1]=t+1; idx[2]=t+2; }               /* FAN   */
        else if (prim == 5){                                              /* STRIP */
            if (t & 1){ idx[0]=t+1; idx[1]=t; idx[2]=t+2; }
            else      { idx[0]=t;   idx[1]=t+1; idx[2]=t+2; }
        }
        else { idx[0]=t*3; idx[1]=t*3+1; idx[2]=t*3+2; }                  /* LIST  */
        for (int k = 0; k < 3; k++){
            memcpy(dst, S + (size_t)idx[k]*stride, stride);
            dst += stride;
        }
    }
    g_rvn[b] = roff + need;
    D->first = 0; D->count = tris*3; D->rawoff = roff;
    D->covok = (unsigned)covok; memcpy(D->cov, cov, sizeof cov);
    D->cuvok = (unsigned)cuvok; memcpy(D->cuv, cuv, sizeof cuv);
    g_dn[b]++;
    pthread_mutex_unlock(&g_qlock);
    return 1;
}
/* Indexed triangle list from slices in the pool (see PERSISTENT VERTEX / INDEX
 * POOL).  vbo: pool offset of vertex 0, ibo: offset of the first index. */
int metal_draw_ix(unsigned vbo, unsigned stride, unsigned ibo, unsigned isz, unsigned nidx,
                  int base, unsigned tex, int vsid, const float *kdata, unsigned nk,
                  unsigned long long gh){
    g_nc_ok = 0; g_nuvb_mask = 0;
    if (!g_pool || nidx < 3 || !stride || stride > 256u || (isz != 2u && isz != 4u)) return 0;
    pthread_mutex_lock(&g_qlock);
    int b = g_cur;
    if (g_dn[b] >= MAXD){ pthread_mutex_unlock(&g_qlock); return 0; }
    MDraw *D = raw_rec(b, stride, tex, vsid, kdata, nk, 0u, NULL);
    if (!D){ pthread_mutex_unlock(&g_qlock); return 0; }
    D->count = nidx - nidx % 3u; D->rawoff = vbo;
    D->ix_isz = isz; D->ix_ib = ibo; D->ix_base = base; D->gh = gh ? gh : 1ull;
    g_dn[b]++;
    pthread_mutex_unlock(&g_qlock);
    return 1;
}
void metal_raw_overflow(unsigned *rv, unsigned *kb){
    if (rv) *rv = g_ov_rv;
    if (kb) *kb = g_ov_kb;
}

/* Compile and store the MSL source from the translator (once per id). */
/* CAREFUL: the parameter name can't be "id" - it shadows ObjC's id type and
 * the expression "id<MTLLibrary>" can't be parsed. */
void metal_register_ps(int psid, const char *msl){
    { const char *pd = ENVC("PSDUMP");          /* supports a comma-separated list */
      if (pd){ char want[16]; int n = snprintf(want, sizeof want, "%d", psid);
        for (const char *q = pd; q && *q; ){
            while (*q==',' || *q==' ') q++;
            if (!*q) break;
            if (!strncmp(q, want, (size_t)n) && (q[n]==0 || q[n]==',' || q[n]==' ')){
                fprintf(stderr, "=== ps %d MSL ===\n%s\n=== end ===\n", psid, msl);
                break; }
            while (*q && *q != ',') q++; } } }
    if (psid > 0 && psid < MAXPS && msl)
        g_ps_kill[psid] = strstr(msl, "< 0.0)) discard_fragment()") ? 1 : 0;   /* texkill (vsemu.c) */
    /* POST-PROCESS CHAIN: is stage 0 sampled in only one place, with the interpolant coordinate (TCRD(0))?
     * ("SMP0(" twice: the #define line + a single call) */
    if (psid > 0 && psid < PSCUBE && msl){
        int n = 0; for (const char *q = msl; (q = strstr(q, "SMP0(")) != NULL; q++) n++;
        g_ps_fbok[psid] = (n == 2 && strstr(msl, "SMP0(TCRD(0), TPICK(0).xyz)")) ? 1 : 0; }
    /* PSISO=<psid>:<expr> : output one of the shader's INTERMEDIATE TERMS directly.
     * Instead of guessing which input blows up we see it on screen (t[0] diffuse,
     * t[1] headlight texture, t[2] ramp, v[0] vertex color, P.c[0] constant). */
    char *iso = 0;
    { const char *pe = ENVC("PSISO");
      if (pe && *pe){ const char *cl = strchr(pe, ':');
        /* PSISO=*:  -> every shader outputs a FIXED color derived from its own id;
         * so we can read off the screen which ps drew which surface. */
        char autoex[96];
        if (cl && pe[0] == '*' && cl == pe + 1){
            /* Alpha must stay ORIGINAL: if we fix it the blending breaks and the
             * frame falls apart completely. */
            snprintf(autoex, sizeof autoex,
                     "%.4f, %.4f, %.4f, res.a",
                     (double)((psid * 97) % 251) / 250.0,
                     (double)((psid * 53) % 251) / 250.0,
                     (double)((psid * 29) % 251) / 250.0);
            cl = pe + 1;
        } else autoex[0] = 0;
        if (cl && (autoex[0] || atoi(pe) == psid)){
            const char *ex = autoex[0] ? autoex : cl + 1;
            size_t need = strlen(msl) + strlen(ex) + 128;
            iso = (char *)malloc(need);
            if (iso){
                const char *rp = strstr(msl, "return res;");
                const char *dc = strstr(msl, "if (res.a < P.aref) discard_fragment();");
                if (rp){
                    size_t head = (size_t)(rp - msl);
                    memcpy(iso, msl, head); iso[head] = 0;
                    (void)dc;   /* alpha test kept: so the silhouette stays correct */
                    snprintf(iso + head, need - head, "return float4(%s);\n}\n", ex);
                    msl = iso;
                    fprintf(stderr, "  [psiso] ps %d -> %s\n", psid, ex);
                } else { free(iso); iso = 0; }
            }
        } } }
    if (psid <= 0 || psid >= MAXPS || !msl || !g_dev || g_psfn[psid] || atomic_load(&g_ps_pend[psid])){ free(iso); return; }
    static int async = -1;
    if (async < 0){ const char *e = ENVC("PSASYNC"); async = !(e && *e == '0'); }
    static dispatch_queue_t q; static dispatch_semaphore_t slots;
    if (async && !q){
        q = dispatch_queue_create("nfsu2.ps-compile",
                dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_CONCURRENT, QOS_CLASS_USER_INITIATED, 0));
        slots = dispatch_semaphore_create(8);
    }
    NSString *src;
    @autoreleasepool {
        src = [kPrelude stringByAppendingString:[NSString stringWithUTF8String:msl]];
    }
    free(iso);
    void (^job)(void) = ^{ @autoreleasepool {
        NSError *err = nil;
        struct timespec _c0; if (g_perflog) clock_gettime(CLOCK_MONOTONIC, &_c0);
        id<MTLLibrary> lib = [g_dev newLibraryWithSource:src options:nil error:&err];
        if (g_perflog){ struct timespec _c1; clock_gettime(CLOCK_MONOTONIC, &_c1);
            perflog_event(PL_PS, (uint64_t)(_c1.tv_sec - _c0.tv_sec) * 1000000000ull + (uint64_t)_c1.tv_nsec - (uint64_t)_c0.tv_nsec, 0); }
        if (!lib){
            static _Atomic int shown = 0;
            if (atomic_fetch_add(&shown, 1) < 3)
                fprintf(stderr, "ps%d could not be compiled: %s\n", psid,
                        err ? [[err localizedDescription] UTF8String] : "?");
        } else {
            char nm[32]; snprintf(nm, sizeof nm, "ps_%d", psid);
            NSString *fnm = [NSString stringWithUTF8String:nm];
            id<MTLFunction> f = fn_spec(lib, fnm, 1), fnat = nil;
            if (!f) fprintf(stderr, "ps%d: function not found\n", psid);
            if (alfavar_mode()) fnat = fn_spec(lib, fnm, alfavar_mode() >= 2 ? 2 : 0);
            pthread_mutex_lock(&g_ps_mx);
            g_psfn[psid] = f; g_psfn_nat[psid] = fnat;
            pthread_mutex_unlock(&g_ps_mx);
        }
        pthread_mutex_lock(&g_ps_mx);
        atomic_store(&g_ps_pend[psid], 0);
        g_ps_job[psid] = nil;
        pthread_cond_broadcast(&g_ps_cv);
        pthread_mutex_unlock(&g_ps_mx);
    } };
    g_ps_job[psid] = job;
    atomic_store(&g_ps_pend[psid], 1);
    if (!async){ ps_run(psid); return; }
    dispatch_async(q, ^{
        if (atomic_load(&g_ps_pend[psid]) != 1) return;     /* a draw took it already */
        dispatch_semaphore_wait(slots, DISPATCH_TIME_FOREVER);
        ps_run(psid);
        dispatch_semaphore_signal(slots);
    });
}

/* D3DCMPFUNC (1..8) -> Metal compare function */
static MTLCompareFunction cmp_of(unsigned f){
    switch (f){
    case 1: return MTLCompareFunctionNever;
    case 2: return MTLCompareFunctionLess;
    case 3: return MTLCompareFunctionEqual;
    case 4: return MTLCompareFunctionLessEqual;
    case 5: return MTLCompareFunctionGreater;
    case 6: return MTLCompareFunctionNotEqual;
    case 7: return MTLCompareFunctionGreaterEqual;
    default: return MTLCompareFunctionAlways;
    }
}
static id<MTLDepthStencilState> dss_for(unsigned mode){
    unsigned k = mode & 63u;
    if (g_dss[k]) return g_dss[k];
    MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
    if (k & 1u) dd.depthCompareFunction = cmp_of((k >> 2) & 7u);
    else        dd.depthCompareFunction = MTLCompareFunctionAlways;
    dd.depthWriteEnabled = (k & 2u) ? YES : NO;
    g_dss[k] = [g_dev newDepthStencilStateWithDescriptor:dd];
    return g_dss[k];
}
/* Guest: send the following draws to this target (0 = screen). */
void metal_set_render_target(unsigned texid){
    pthread_mutex_lock(&g_qlock);
    g_cur_target = texid;
    pthread_mutex_unlock(&g_qlock);
}
/* Guest: clear the active target (applied at the right place in draw order). */
/* StretchRect: the game copies the scene from the BACK BUFFER into a texture and
 * the post-process (bleach bypass, motion blur, DOF) reads from that texture.  It
 * was never implemented (measured: 2261 calls in a session) -> the source texture
 * stayed empty, the composite produced black.  We put the copy into the draw list
 * as an ORDERED entry; at present time it runs as a full-screen quad (scaling
 * and format conversion come for free).        */
/* The last copy the guest asked for (READING BACK A LOCKABLE RENDER TARGET redoes
 * a copy from the screen out of the flushed frame's final image). */
static unsigned g_lb_src, g_lb_dst; static float g_lb_uv[4];
static id<MTLRenderPipelineState> g_pso_copy;
void metal_blit(unsigned src_tex, unsigned dst_tex){
    pthread_mutex_lock(&g_qlock);
    g_lb_src = src_tex; g_lb_dst = dst_tex;
    g_lb_uv[0] = 0.f; g_lb_uv[1] = 0.f; g_lb_uv[2] = 1.f; g_lb_uv[3] = 1.f;
    int b = g_cur;
    if (g_dn[b] < MAXD){
        MDraw *D = &g_dbuf[b][g_dn[b]];
        memset(D, 0, sizeof *D);
        D->target = dst_tex; D->blit_src = src_tex; D->is_blit = 1;
        g_dn[b]++;
    }
    pthread_mutex_unlock(&g_qlock);
}
/* StretchRect with a source rectangle (u0,v0)-(u1,v1) in 0..1 of the source.  The
 * magazine and DVD covers are cut from the middle of the screen this way. */
void metal_blit_rect(unsigned src_tex, unsigned dst_tex, float u0, float v0, float u1, float v1){
    pthread_mutex_lock(&g_qlock);
    g_lb_src = src_tex; g_lb_dst = dst_tex;
    g_lb_uv[0] = u0; g_lb_uv[1] = v0; g_lb_uv[2] = u1; g_lb_uv[3] = v1;
    int b = g_cur;
    if (g_dn[b] < MAXD){
        MDraw *D = &g_dbuf[b][g_dn[b]];
        memset(D, 0, sizeof *D);
        D->target = dst_tex; D->blit_src = src_tex; D->is_blit = 1;
        D->blit_rect = 1; D->blit_uv[0] = u0; D->blit_uv[1] = v0; D->blit_uv[2] = u1; D->blit_uv[3] = v1;
        g_dn[b]++;
    }
    pthread_mutex_unlock(&g_qlock);
}

void metal_clear_target_ex(unsigned argb, unsigned flags, float z);
void metal_clear_target(unsigned argb){ metal_clear_target_ex(argb, 1u, 1.f); }
void metal_clear_target_ex(unsigned argb, unsigned flags, float z){
    pthread_mutex_lock(&g_qlock);
    int b = g_cur;
    if (g_dn[b] < MAXD){
        memset(&g_dbuf[b][g_dn[b]], 0, sizeof g_dbuf[b][g_dn[b]]);
        g_dbuf[b][g_dn[b]].first = 0; g_dbuf[b][g_dn[b]].count = 0;
        g_dbuf[b][g_dn[b]].tex = 0;   g_dbuf[b][g_dn[b]].target = g_cur_target;
        { static unsigned ov = 0xFFFFFFFFu;      /* CLEARCOL=0xAARRGGBB: diagnostic */
          if (ov == 0xFFFFFFFFu){ const char *e=ENVC("CLEARCOL");
                                  ov = e ? (unsigned)strtoul(e,0,0) : 0u; }
          if (ov) argb = ov; }
        g_dbuf[b][g_dn[b]].clear_argb = argb;
        g_dbuf[b][g_dn[b]].is_clear = 1;
        g_dbuf[b][g_dn[b]].clr_flags = flags;
        g_dbuf[b][g_dn[b]].clr_z = z;
        g_dbuf[b][g_dn[b]].depth = g_cur_depth;
        g_dn[b]++;
    }
    pthread_mutex_unlock(&g_qlock);
}

void metal_present(void){
    if (!g_layer || !g_vbuf[0]) return;
    pthread_mutex_lock(&g_qlock);
    /* If the previous frame hasn't been put on screen yet, WAIT.  If we don't, the
     * guest writes over the next frame and the screen is blank at intervals
     * (the "goes black and comes back" bug the user saw).
     * The wait lasts until the main thread gets a drawable, i.e. up to vsync: 13 ms
     * of a 16.7 ms frame in the menus.  It used to hold the big guest lock, so the
     * game's other threads hardly ran; the music stream thread wrote 20-50% of what
     * played and the menu music came in fragments (fine when minimised: no frames).
     * The lock is given up for the wait and taken back after g_qlock is released,
     * so the two are never waited for in opposite orders. */
    int yielded = 0;
    if (g_ready >= 0){
        pthread_mutex_unlock(&g_qlock);
        int guest_blocking_begin(void);
        yielded = guest_blocking_begin();
        pthread_mutex_lock(&g_qlock);
    }
    int dropped = 0;
    if (g_ready >= 0){
        struct timespec ts, _w0; clock_gettime(CLOCK_MONOTONIC,&_w0);
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 60L * 1000000L;             /* at most 60 ms */
        if (ts.tv_nsec >= 1000000000L){ ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        int late = 0;
        while (g_ready >= 0){
            /* The time ran out but the main thread is still ENCODING the previous frame:
             * don't give up, wait.  The swap would point g_cur at the buffer it is
             * reading (see g_enc_rb).  The signal comes as soon as encoding ends. */
            if (late && g_enc_rb >= 0){ pthread_cond_wait(&g_qcond, &g_qlock); continue; }
            if (pthread_cond_timedwait(&g_qcond, &g_qlock, &ts) != 0){
                if (g_enc_rb >= 0){
                    if (!late){ late = 1;
                        unsigned long n = atomic_fetch_add(&g_enc_late, 1ul);
                        if (n < 8) g_enc_late_f[n] = atomic_load(&g_frames_drawn) + 1; }
                    continue;
                }
                atomic_fetch_add(&g_frames_dropped, 1);
                dropped = 1;
                break;                             /* let's not hang */
            }
        }
        { struct timespec _w1; clock_gettime(CLOCK_MONOTONIC,&_w1);
          atomic_fetch_add(&g_wait_present_ns,
            (unsigned long long)(_w1.tv_sec-_w0.tv_sec)*1000000000ull
            + (unsigned long long)_w1.tv_nsec - (unsigned long long)_w0.tv_nsec); }
    }
    g_pool_fr[g_cur] = g_pool;       /* this frame's indexed draws are in this pool */
    g_gframe++;
    g_noshow[g_cur] = g_noshow_next; g_noshow_next = 0;
    g_ready = g_cur;                 /* let the main thread draw this buffer */
    g_cur ^= 1;                      /* let the guest keep writing into the other one */
    /* see ORDERED TEXTURE UPLOADS: the uploads of a skipped (never encoded) frame must not be lost */
    if (dropped && g_upbuf[g_cur]){
        for (unsigned q = 0; q < g_dn[g_cur]; q++){ const MDraw *u = &g_dbuf[g_cur][q];
            if (!u->is_up || !u->tex || u->tex >= MAXTEX || !g_tex[u->tex]) continue;
            MTLRegion rg = MTLRegionMake2D(0, 0, u->up_w, u->up_h);
            if (g_tex_cube[u->tex])
                [g_tex[u->tex] replaceRegion:rg mipmapLevel:u->up_level slice:u->up_face
                                   withBytes:g_upbuf[g_cur] + u->rawoff bytesPerRow:u->up_pitch bytesPerImage:0];
            else
                [g_tex[u->tex] replaceRegion:rg mipmapLevel:u->up_level
                                   withBytes:g_upbuf[g_cur] + u->rawoff bytesPerRow:u->up_pitch]; } }
    g_upn[g_cur] = 0;
    g_vn[g_cur] = 0; g_dn[g_cur] = 0;
    g_rvn[g_cur] = 0; g_kn[g_cur] = 0; g_pscn[g_cur] = 0;
#if NFS_ENHANCED
    g_hd_cam_ok[g_cur] = 0;
#endif
    atomic_store(&g_frame_pending, 1);
    pthread_cond_signal(&g_rcond);      /* let the main thread take it right away */
    pthread_mutex_unlock(&g_qlock);
    if (yielded){ void guest_blocking_end(void); guest_blocking_end(); }
}

/* ---- READING BACK A LOCKABLE RENDER TARGET -----------------------------------
 * The magazine and DVD covers: the game makes a lockable render target
 * (CreateRenderTarget, Lockable=1), StretchRects the middle of the screen into it
 * and LockRects it to read the photo (0x5c2ee0, 0x5c3150), then saves the pixels.
 * Our draws only reach the GPU when the frame is presented, so the lock used to
 * hand back an empty buffer and every cover was saved black.  Here the frame so far
 * is handed to the main thread as an OFF-SCREEN frame (drawn into g_flush_tid, not
 * presented), we wait until the GPU has run it and copy the target's pixels.  The
 * rest of the frame starts by copying that screen back, so what is shown is whole.
 * Returns 1 when dst was filled. */
int metal_rt_readback(unsigned mid, void *dst, unsigned pitch, unsigned w, unsigned h){
    if (!g_dev || !g_layer || !g_vbuf[0] || !mid || mid >= MAXTEX || !g_tex[mid] || !dst) return 0;
    if (g_tex[mid].pixelFormat != MTLPixelFormatBGRA8Unorm || g_tex[mid].storageMode != MTLStorageModeShared) return 0;
    if (!g_flush_tid) g_flush_tid = metal_create_texture_ex(16, 16, 1, 21u, 1);
    if (!g_flush_tid) return 0;
    pthread_mutex_lock(&g_qlock);
    int pending = g_dn[g_cur] > 0;
    long my = 0;
    if (pending){ g_noshow_next = 1; my = ++g_flush_req; }
    /* was this target just filled from the screen?  (then copy it again below) */
    int from_screen = (g_lb_dst == mid && g_lb_src == 0u);
    float uv[4]; memcpy(uv, g_lb_uv, sizeof uv);
    pthread_mutex_unlock(&g_qlock);
    if (pending){
        metal_present();
        int guest_blocking_begin(void); void guest_blocking_end(void);
        int yielded = guest_blocking_begin();
        pthread_mutex_lock(&g_qlock);
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += 2;
        while (g_flush_done < my)
            if (pthread_cond_timedwait(&g_qcond, &g_qlock, &ts) != 0) break;
        int ok = g_flush_done >= my;
        pthread_mutex_unlock(&g_qlock);
        if (yielded) guest_blocking_end();
        metal_blit(g_flush_tid, 0);                 /* the screen so far, back on the screen */
        if (!ok){ fprintf(stderr, "metal: readback of texture %u timed out\n", mid); return 0; }
        /* A copy from the screen: in the HD build the screen is only complete at
         * the end of the frame (multisampled / composited passes), the copy made in
         * the middle of the flushed frame read an unfinished screen.  Cut it again
         * from the flushed frame's final image. */
        id<MTLTexture> fs = (g_flush_tid < MAXTEX) ? g_tex[g_flush_tid] : nil;
        if (from_screen && fs && g_pso_copy && g_blitvb){
            MVert bq[6]; memcpy(bq, [g_blitvb contents], sizeof bq);
            for (int q = 0; q < 6; q++){
                float u = bq[q].t[0][0], v = bq[q].t[0][1];
                bq[q].t[0][0] = uv[0] + u * (uv[2] - uv[0]);
                bq[q].t[0][1] = uv[1] + v * (uv[3] - uv[1]); }
            id<MTLCommandBuffer> cbc = [g_queue commandBuffer];
            MTLRenderPassDescriptor *bp = [MTLRenderPassDescriptor renderPassDescriptor];
            bp.colorAttachments[0].texture = g_tex[mid];
            bp.colorAttachments[0].loadAction = MTLLoadActionClear;    /* opaque: alpha stays 1 */
            bp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
            bp.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> be = [cbc renderCommandEncoderWithDescriptor:bp];
            [be setRenderPipelineState:g_pso_copy];
            [be setVertexBytes:bq length:sizeof bq atIndex:0];
            [be setFragmentSamplerState:g_samp atIndex:0];
            [be setFragmentTexture:fs atIndex:0];
            { struct { float aref, usec, p0, p1; float proj[4]; float c[PSC_N][4]; float fogv[4]; } bf;
              memset(&bf, 0, sizeof bf);
              [be setFragmentBytes:&bf length:sizeof bf atIndex:0]; }
            [be drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
            [be endEncoding];
            [cbc commit]; [cbc waitUntilCompleted];
        }
    } else {
        /* nothing new this frame: let the GPU finish what was already sent */
        id<MTLCommandBuffer> cbw = [g_queue commandBuffer]; [cbw commit]; [cbw waitUntilCompleted];
    }
    id<MTLTexture> t = g_tex[mid];
    NSUInteger cw = t.width < w ? t.width : w, ch = t.height < h ? t.height : h;
    [t getBytes:dst bytesPerRow:pitch fromRegion:MTLRegionMake2D(0, 0, cw, ch) mipmapLevel:0];
    static int nlog = 0;
    if (nlog < 8){ nlog++;
        fprintf(stderr, "metal: lockable render target %u (%lux%lu) read back for the game%s\n", mid,
                (unsigned long)cw, (unsigned long)ch, pending ? " (frame flushed off screen)" : ""); }
    return 1;
}

long metal_frames_dropped(void){ return atomic_load(&g_frames_dropped); }
unsigned long long metal_wait_present_ns(void){ return atomic_load(&g_wait_present_ns); }
int metal_thermal_state(void){ return (int)[[NSProcessInfo processInfo] thermalState]; }

/* PERFLOG screenshot: a full-size copy of the drawable; when done, every k-th
 * pixel is read on a background queue and a ~480-pixel-wide JPEG is written.  No
 * work on the game threads; a new one isn't taken before the previous image is
 * written.
 * The copy goes into a LINEAR MTLBuffer: it used to go into a shared TEXTURE, the
 * GPU stores that losslessly compressed and every row getBytes forced the driver
 * to decompress blocks on the CPU (sample: ~0.08 cores, it polluted the measurement). */
static id<MTLBuffer> g_pl_buf;
static _Atomic int g_pl_busy = 0;
static void pl_thumb(id<MTLCommandBuffer> cb, id<MTLTexture> src, long fno){
    if (atomic_load(&g_pl_busy)) return;
    if (src.pixelFormat != MTLPixelFormatBGRA8Unorm && src.pixelFormat != MTLPixelFormatBGRA8Unorm_sRGB) return;
    NSUInteger W = src.width, H = src.height, bpr = W * 4, need = bpr * H;
    if (!g_pl_buf || g_pl_buf.length < need)
        g_pl_buf = [g_dev newBufferWithLength:need options:MTLResourceStorageModeShared];
    if (!g_pl_buf) return;
    atomic_store(&g_pl_busy, 1);
    id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
    [bl copyFromTexture:src sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
             sourceSize:MTLSizeMake(W, H, 1) toBuffer:g_pl_buf destinationOffset:0
 destinationBytesPerRow:bpr destinationBytesPerImage:need];
    [bl endEncoding];
    id<MTLBuffer> buf = g_pl_buf;
    double tms = perflog_now_us() / 1e3;
    [cb addCompletedHandler:^(id<MTLCommandBuffer> b){
        (void)b;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
            NSUInteger k = (W + 479) / 480, w = W / k, h = H / k;
            const uint8_t *base = (const uint8_t *)[buf contents];
            uint8_t *out = malloc(w * h * 4);
            if (out){
                for (NSUInteger y = 0; y < h; y++){
                    const uint8_t *row = base + (y * k) * bpr;
                    for (NSUInteger x = 0; x < w; x++) memcpy(out + (y * w + x) * 4, row + x * k * 4, 4);
                }
                CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
                CGContextRef cg = CGBitmapContextCreate(out, w, h, 8, w * 4, cs,
                    kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
                if (cg){
                    CGImageRef img = CGBitmapContextCreateImage(cg);
                    char path[1100];
                    snprintf(path, sizeof path, "%s/screens/%09.0f_%ld.jpg", g_pl_dir, tms, fno);
                    CFStringRef sp = CFStringCreateWithCString(NULL, path, kCFStringEncodingUTF8);
                    CFURLRef url = CFURLCreateWithFileSystemPath(NULL, sp, kCFURLPOSIXPathStyle, false);
                    CGImageDestinationRef dst = CGImageDestinationCreateWithURL(url, CFSTR("public.jpeg"), 1, NULL);
                    if (dst){ CGImageDestinationAddImage(dst, img, NULL); CGImageDestinationFinalize(dst); CFRelease(dst); }
                    CFRelease(url); CFRelease(sp); CGImageRelease(img); CGContextRelease(cg);
                }
                CGColorSpaceRelease(cs);
            }
            free(out);
            atomic_store(&g_pl_busy, 0);
        });
    }];
}

/* Called from the main thread, BEFORE the guest starts.
 * The game creates its first textures before the window opens; if the device
 * wasn't ready at that moment the textures silently got lost. */
void metal_early_init(void){
    if (g_dev) return;
    /* Measure the screen geometry BEFORE the guest thread starts, on the main thread:
     * the guest picks the drawing size from it in CreateDevice (boot.c calls this
     * function before pthread_create). */
    [NSApplication sharedApplication];
    scr_cache();
    /* App Nap / timer coalescing OFF.  When the game window isn't in the
     * foreground macOS drops the process to background QoS and the work moves
     * to the EFFICIENCY cores -- measured: same binary, same scene, 36 ms/frame
     * instead of 10 ms (3.4x).  clang ran at full speed from the same shell, so
     * it wasn't the machine but the APP being throttled. */
    { static id act; if (!act)
        act = [[NSProcessInfo processInfo] beginActivityWithOptions:
                 NSActivityUserInitiated | NSActivityLatencyCritical
                 reason:@"game frame production"];
      (void)act; }
    dump_init();
    g_dev = MTLCreateSystemDefaultDevice();
    if (!g_dev){ fprintf(stderr, "metal: device not found\n"); return; }
    g_queue = [g_dev newCommandQueue];
    @autoreleasepool {
        g_vbuf[0] = malloc(sizeof(MVert)*MAXV); g_vbuf[1] = malloc(sizeof(MVert)*MAXV);
        g_dbuf[0] = malloc(sizeof(MDraw)*MAXD); g_dbuf[1] = malloc(sizeof(MDraw)*MAXD);
        g_pscb[0] = malloc(sizeof *g_pscb[0] * MAXD); g_pscb[1] = malloc(sizeof *g_pscb[1] * MAXD);
        for (int i = 0; i < NFB; i++){           /* see "per-frame buffer ring" */
            g_fb_vb[i]  = fb_fit(nil, 0, sizeof(MVert)*MAXV);
            g_fb_rvb[i] = fb_fit(nil, 0, MAXRV);
            g_fb_kb[i]  = fb_fit(nil, 0, MAXKB);
        }
        g_fb_sem = dispatch_semaphore_create(NFB);
        g_mtlvb = g_fb_vb[0]; g_mtlrvb = g_fb_rvb[0]; g_mtlkb = g_fb_kb[0];
        /* A fixed full-screen quad for StretchRect copies. */
        g_blitvb = [g_dev newBufferWithLength:sizeof(MVert)*6
                                      options:MTLResourceStorageModeShared];
        if (g_blitvb){
            MVert q[6]; memset(q, 0, sizeof q);
            const float px[6] = {-1, 1, 1,-1, 1,-1}, py[6] = { 1, 1,-1, 1,-1,-1};
            const float tu[6] = { 0, 1, 1, 0, 1, 0}, tv3[6] = { 0, 0, 1, 0, 1, 1};
            for (int i2 = 0; i2 < 6; i2++){
                q[i2].x = px[i2]; q[i2].y = py[i2]; q[i2].z = 0.f; q[i2].w = 1.f;
                q[i2].r = q[i2].g = q[i2].b = q[i2].a = 1.f;
                for (int k2 = 0; k2 < 4; k2++){
                    q[i2].t[k2][0] = tu[i2]; q[i2].t[k2][1] = tv3[i2];
                    q[i2].t[k2][2] = 0.f;    q[i2].t[k2][3] = 1.f;
                }
            }
            memcpy([g_blitvb contents], q, sizeof q);
        }
        g_rvbuf[0] = malloc(MAXRV); g_rvbuf[1] = malloc(MAXRV);
        g_upbuf[0] = malloc(MAXUP); g_upbuf[1] = malloc(MAXUP);
        g_kbuf[0]  = malloc(MAXKB); g_kbuf[1]  = malloc(MAXKB);
        NSError *err = nil;
        id<MTLLibrary> lib = [g_dev newLibraryWithSource:kShaderSrc options:nil error:&err];
        g_lib = lib;
        if (!lib){
            fprintf(stderr,"metal: shader could not be compiled: %s\n",
                    [[err localizedDescription] UTF8String]);
        } else {
            MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
            vd.attributes[0].format = MTLVertexFormatFloat4;
            vd.attributes[0].offset = 0;  vd.attributes[0].bufferIndex = 0;
            vd.attributes[1].format = MTLVertexFormatFloat4;
            vd.attributes[1].offset = 16; vd.attributes[1].bufferIndex = 0;
            vd.attributes[2].format = MTLVertexFormatFloat4;   /* oD1 */
            vd.attributes[2].offset = 32; vd.attributes[2].bufferIndex = 0;
            vd.attributes[3].format = MTLVertexFormatFloat4;   /* oT0 */
            vd.attributes[3].offset = 48; vd.attributes[3].bufferIndex = 0;
            vd.attributes[4].format = MTLVertexFormatFloat4;   /* oT1 */
            vd.attributes[4].offset = 64; vd.attributes[4].bufferIndex = 0;
            vd.attributes[5].format = MTLVertexFormatFloat4;   /* oT2 */
            vd.attributes[5].offset = 80; vd.attributes[5].bufferIndex = 0;
            vd.attributes[6].format = MTLVertexFormatFloat4;   /* oT3 */
            vd.attributes[6].offset = 96; vd.attributes[6].bufferIndex = 0;
            vd.layouts[0].stride = sizeof(MVert);
            vd.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;
            g_vdesc = vd;

            if (alfavar_mode() >= 2){
                g_atdbg = [g_dev newBufferWithLength:8192 * 4 options:MTLResourceStorageModeShared];
                memset([g_atdbg contents], 0, 8192 * 4); }
            g_fmain_at  = fn_spec(lib, @"fmain", 1);
            g_fmain_nat = fn_spec(lib, @"fmain", alfavar_mode() >= 2 ? 2 : 0);
            MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
            pd.vertexFunction   = [lib newFunctionWithName:@"vmain"];
            pd.fragmentFunction = g_fmain_at;
            pd.vertexDescriptor = vd;
            pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
            pd.colorAttachments[0].blendingEnabled = YES;
            pd.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
            pd.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
            pd.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
            pd.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
            NSError *e3 = nil;
            g_pso = [g_dev newRenderPipelineStateWithDescriptor:pd error:&e3];
            if (!g_pso) fprintf(stderr,"metal: pipeline could not be built: %s\n",
                                [[e3 localizedDescription] UTF8String]);
            /* a plain copy (no blending, RGB only) for READING BACK A LOCKABLE RENDER TARGET */
            pd.colorAttachments[0].blendingEnabled = NO;
            pd.colorAttachments[0].writeMask = MTLColorWriteMaskRed | MTLColorWriteMaskGreen | MTLColorWriteMaskBlue;
            g_pso_copy = [g_dev newRenderPipelineStateWithDescriptor:pd error:nil];
            pd.colorAttachments[0].blendingEnabled = YES;
            pd.colorAttachments[0].writeMask = MTLColorWriteMaskAll;
#if NFS_ENHANCED
            if (hd_msaa_n() > 1){
                g_pso_ms_desc = [pd copy];
                (void)hd_pso_ms();                  /* ahead of time for the sample count at startup */
            }
            if (hd_ssr_on()){
                NSError *e4 = nil;
                id<MTLLibrary> sl = [g_dev newLibraryWithSource:hd_gb_src(kSSRSrc) options:nil error:&e4];
                if (!sl) fprintf(stderr, "[HD] SSR shader could not be compiled: %s\n",
                                 [[e4 localizedDescription] UTF8String]);
                else {
                    MTLRenderPipelineDescriptor *sd2 = [MTLRenderPipelineDescriptor new];
                    sd2.vertexFunction = [sl newFunctionWithName:@"hd_fs_vert"];
                    sd2.fragmentFunction = [sl newFunctionWithName:@"hd_ssr_frag"];
                    sd2.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
                    g_hd_ssr_pso = [g_dev newRenderPipelineStateWithDescriptor:sd2 error:&e4];
                    if (hd_msaa_n() > 1){
                        sd2.rasterSampleCount = (NSUInteger)hd_msaa_n();
                        g_hd_ssr_pso_ms = [g_dev newRenderPipelineStateWithDescriptor:sd2 error:&e4];
                    }
                    if (!g_hd_ssr_pso) fprintf(stderr, "[HD] SSR pipeline could not be built: %s\n",
                                               [[e4 localizedDescription] UTF8String]);
                }
            }
#endif
        }
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
        sd.minFilter = MTLSamplerMinMagFilterLinear;
        sd.magFilter = MTLSamplerMinMagFilterLinear;
        /* MIPOFF=1: turn off mip selection (diagnostic).  To tell whether a coarse mip
         * is picked and a 1-pixel line appears when the derivative estimate jumps
         * at triangle edges. */
        sd.mipFilter = ENVC("MIPOFF") ? MTLSamplerMipFilterNotMipmapped
                                        : MTLSamplerMipFilterLinear;
        sd.sAddressMode = MTLSamplerAddressModeRepeat;
        sd.tAddressMode = MTLSamplerAddressModeRepeat;
        g_samp = [g_dev newSamplerStateWithDescriptor:sd];

        MTLTextureDescriptor *wd =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                  width:1 height:1 mipmapped:NO];
        wd.usage = MTLTextureUsageShaderRead; wd.storageMode = MTLStorageModeShared;
        g_white = [g_dev newTextureWithDescriptor:wd];
        unsigned px = 0xFFFFFFFFu;
        [g_white replaceRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0
                     withBytes:&px bytesPerRow:4];
    }
    fprintf(stderr, "metal: device ready (%s), draw path %s\n",
            [[g_dev name] UTF8String], g_pso ? "HAZIR" : "NONE");
}

/* --- pipeline cache by blend state -------------------------
 * Since D3D9's fixed-function blend settings are baked into the pipeline in
 * Metal, every different combination needs a separate pipeline.        */
#if NFS_ENHANCED
#define MAXPSO 2048         /* + MSAA and G-buffer bits */
#else
#define MAXPSO 1024         /* variants with / without alpha test */
#endif
static struct { uint64_t key; id<MTLRenderPipelineState> pso; } g_psocache[MAXPSO];
static unsigned g_pson = 0;

static MTLBlendFactor d3d_blend(unsigned f, int is_alpha){
    (void)is_alpha;
    switch (f){
    case 1:  return MTLBlendFactorZero;
    case 2:  return MTLBlendFactorOne;
    case 3:  return MTLBlendFactorSourceColor;
    case 4:  return MTLBlendFactorOneMinusSourceColor;
    case 5:  return MTLBlendFactorSourceAlpha;
    case 6:  return MTLBlendFactorOneMinusSourceAlpha;
    case 7:  return MTLBlendFactorDestinationAlpha;
    case 8:  return MTLBlendFactorOneMinusDestinationAlpha;
    case 9:  return MTLBlendFactorDestinationColor;
    case 10: return MTLBlendFactorOneMinusDestinationColor;
    case 11: return MTLBlendFactorSourceAlphaSaturated;
    /* 12/13 = BOTHSRCALPHA / BOTHINVSRCALPHA (legacy, SRCBLEND only) */
    case 12: return MTLBlendFactorSourceAlpha;
    case 13: return MTLBlendFactorOneMinusSourceAlpha;
    /* 14/15 = BLENDFACTOR / INVBLENDFACTOR.  They were MISSING and both fell back
     * to One: the post-process composite (SRC=14, DEST=15) did "scene + scene"
     * instead of a crossfade, and in a race the ground was blown-out white from
     * end to end. */
    case 14: return MTLBlendFactorBlendColor;
    case 15: return MTLBlendFactorOneMinusBlendColor;
    default: return MTLBlendFactorOne;
    }
}
static MTLBlendOperation d3d_blendop(unsigned op){
    switch (op){
    case 2: return MTLBlendOperationSubtract;
    case 3: return MTLBlendOperationReverseSubtract;
    case 4: return MTLBlendOperationMin;
    case 5: return MTLBlendOperationMax;
    default: return MTLBlendOperationAdd;
    }
}

static id<MTLRenderPipelineState> pso_for(uint64_t key){
    for (unsigned i = 0; i < g_pson; i++)
        if (g_psocache[i].key == key) return g_psocache[i].pso;
    if (!g_lib || g_pson >= MAXPSO) return g_pso;

    /* key: blend(0..19) | ps<<20 | vs<<32 | (HD) msaa<<48 | G<<49 | log2(samples)<<50
     *        | no alpha test<<52 */
    int psid = (int)((key >> 20) & 0xFFFu);
    int vsid = (int)((key >> 32) & 0xFFFu);
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    /* The GPU vertex shader reads its own vertex data from a buffer:
     * NO vertexDescriptor is given (otherwise Metal expects stage_in). */
    pd.vertexFunction   = (vsid > 0 && vsid < MAXVSF && g_vsfn[vsid])
                          ? g_vsfn[vsid]
                          : [g_lib newFunctionWithName:@"vmain"];
    int nat = (int)((key >> 52) & 1u);         /* see ALPHA TEST VARIANT */
    ps_wait(psid);
    pd.fragmentFunction = (psid > 0 && psid < MAXPS && g_psfn[psid])
                          ? ((nat && g_psfn_nat[psid]) ? g_psfn_nat[psid] : g_psfn[psid])
                          : ((nat && g_fmain_nat) ? g_fmain_nat : g_fmain_at);
    pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    pd.vertexDescriptor = (vsid > 0 && vsid < MAXVSF && g_vsfn[vsid]) ? nil : g_vdesc;
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    unsigned en   = key & 1u;
    unsigned sf   = (key >> 1) & 31u;
    unsigned df   = (key >> 6) & 31u;
    unsigned op   = (key >> 11) & 7u;
    unsigned mask = (key >> 14) & 15u;
    pd.colorAttachments[0].blendingEnabled = en ? YES : NO;
    pd.colorAttachments[0].sourceRGBBlendFactor        = d3d_blend(sf, 0);
    pd.colorAttachments[0].destinationRGBBlendFactor   = d3d_blend(df, 0);
    pd.colorAttachments[0].sourceAlphaBlendFactor      = d3d_blend(sf, 1);
    pd.colorAttachments[0].destinationAlphaBlendFactor = d3d_blend(df, 1);
    pd.colorAttachments[0].rgbBlendOperation   = d3d_blendop(op);
    pd.colorAttachments[0].alphaBlendOperation = d3d_blendop(op);
    MTLColorWriteMask cw = 0;
    if (mask & 1) cw |= MTLColorWriteMaskRed;
    if (mask & 2) cw |= MTLColorWriteMaskGreen;
    if (mask & 4) cw |= MTLColorWriteMaskBlue;
    if (mask & 8) cw |= MTLColorWriteMaskAlpha;
    pd.colorAttachments[0].writeMask = cw;
#if NFS_ENHANCED
    if ((key >> 48) & 1u) pd.rasterSampleCount = ((key >> 50) & 3u) ? (NSUInteger)1u << ((key >> 50) & 3u)
                                                                    : (NSUInteger)hd_msaa_n();
    if ((key >> 49) & 1u){
        /* G-buffer: blended draws and the fixed-function shader (ps 0, single output)
         * don't write it -- the data of the surface below stays. */
        MTLColorWriteMask gm = (en || psid == 0) ? MTLColorWriteMaskNone : MTLColorWriteMaskAll;
        for (int gi = 0; gi < HD_NG; gi++){
            pd.colorAttachments[1 + gi].pixelFormat = hd_gfmt(gi);
            pd.colorAttachments[1 + gi].writeMask = gm; }
    }
#endif

    NSError *e2 = nil;
    struct timespec _o0; if (g_perflog) clock_gettime(CLOCK_MONOTONIC, &_o0);
    id<MTLRenderPipelineState> ps = [g_dev newRenderPipelineStateWithDescriptor:pd error:&e2];
    if (g_perflog){ struct timespec _o1; clock_gettime(CLOCK_MONOTONIC, &_o1);
        perflog_event(PL_PSO, (uint64_t)(_o1.tv_sec - _o0.tv_sec) * 1000000000ull + (uint64_t)_o1.tv_nsec - (uint64_t)_o0.tv_nsec, 0); }
    if (!ps) return g_pso;
    g_psocache[g_pson].key = key;
    g_psocache[g_pson].pso = ps;
    g_pson++;
    return ps;
}

/* ---- POST-PROCESS CHAIN ------------------------------------------------------
 * In the race / 3D menu frame the post-process applies a color table in three
 * full-screen passes: #3 = ps28(#2), #2 = ps27(#3), #3 = ps26(#2).  Every pass
 * read its input from a texture at full resolution and wrote its output to
 * memory; measured (ABTEST=SKIPPS, 2026-10-19): the three passes are ~16% of
 * the GPU energy.  Since a step's input is the previous step's output at the
 * SAME pixel, the chain is moved into a SINGLE pass with both targets bound; a
 * step reads its input from tile memory ([[color(n)]]; in draw order on Apple
 * GPUs).  Since intermediate results are written to and read from the same 8-bit
 * target, the quantization is the same too.
 * Conditions (per record): a single quad covering the target fully (FULL-SCREEN
 * COVERAGE), no Z / blending / alpha test; the stage 0 texture is the other chain
 * target, same size / format; UV 1:1 on the texel center (after UV SCALE); the ps
 * samples stage 0 in one place with TCRD(0); other stages don't read the chain
 * targets.  Consecutive records alternate between the two targets.
 * FBZINCIR=0 turns it off; FBZINCIR=2 verification: the chain is drawn on the old
 * path, the merged path is drawn into scratch textures with the same input and the
 * two are compared pixel by pixel on the GPU (report: "differing pixels", should
 * be 0; measured 2026-10-19: 0 in 5454 chains).  FBZCTRL=1 runs the helper with
 * the normal pipeline, FBZCTRL=2 runs it twice and compares it with itself
 * (determinism; before ORDERED TEXTURE UPLOADS a 1-3/255 difference showed up here).
 * Gain (ABTEST=FBZINCIR, two runs): DRAM -23 MB/frame, GPU energy within noise. */
static int fbz_mode(void){
    static int m = -1; if (m < 0){ const char *e = ENVC("FBZINCIR"); m = (e && *e) ? atoi(e) : 1; }
    return m;
}
static _Atomic int g_fb_st[MAXPS][2];             /* 0 none, 1 compiling, 2 ready, 3 failed */
static id<MTLFunction> g_fb_fn[MAXPS][2];          /* [ps][output attachment] */
static _Atomic unsigned long long g_fbz_n = 0, g_fbz_rec = 0, g_fbz_val = 0;
int com_ps_fb_msl(int base, int vid, int in_idx, int out_idx, char *msl, size_t cap);
static int fb_fn_ready(int ps, int out){
    if (ps <= 0 || ps >= PSCUBE || !g_ps_fbok[ps] || !g_dev) return 0;
    int st = atomic_load(&g_fb_st[ps][out]);
    if (st == 2) return 1;
    if (st != 0) return 0;
    atomic_store(&g_fb_st[ps][out], 1);
    static char msl[32768];                        /* encoding thread only */
    int vid = 30000 + ps * 2 + out;
    if (com_ps_fb_msl(ps, vid, 1 - out, out, msl, sizeof msl) <= 0){ atomic_store(&g_fb_st[ps][out], 3); return 0; }
    NSString *src = [kPrelude stringByAppendingString:[NSString stringWithUTF8String:msl]];
    NSString *fnm = [NSString stringWithFormat:@"ps_%d", vid];
    int psv = ps, outv = out;
    /* no hitch in the frame: compile in the background, old path until it is ready */
    [g_dev newLibraryWithSource:src options:nil completionHandler:^(id<MTLLibrary> lib, NSError *err){
        id<MTLFunction> f = lib ? fn_spec(lib, fnm, 0) : nil;
        if (!f) fprintf(stderr, "metal: post-process chain variant could not be compiled (ps %d): %s\n", psv,
                        err ? [[err localizedDescription] UTF8String] : "?");
        g_fb_fn[psv][outv] = f;
        atomic_store(&g_fb_st[psv][outv], f ? 2 : 3); }];
    return 0;
}
#define FB_MAXPSO 32
static struct { int ps, out, vs; id<MTLRenderPipelineState> pso; } g_fbpso[FB_MAXPSO];
static int g_fbpso_n = 0;
static id<MTLRenderPipelineState> fb_pso(int ps, int out, int vs){
    for (int i = 0; i < g_fbpso_n; i++)
        if (g_fbpso[i].ps == ps && g_fbpso[i].out == out && g_fbpso[i].vs == vs) return g_fbpso[i].pso;
    if (g_fbpso_n >= FB_MAXPSO || vs <= 0 || vs >= MAXVSF || !g_vsfn[vs] || !g_fb_fn[ps][out]) return nil;
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = g_vsfn[vs];
    pd.fragmentFunction = g_fb_fn[ps][out];
    for (int a = 0; a < 2; a++){
        pd.colorAttachments[a].pixelFormat = MTLPixelFormatBGRA8Unorm;
        pd.colorAttachments[a].blendingEnabled = NO;
        pd.colorAttachments[a].writeMask = (a == out) ? MTLColorWriteMaskAll : MTLColorWriteMaskNone; }
    NSError *e = nil;
    id<MTLRenderPipelineState> p = [g_dev newRenderPipelineStateWithDescriptor:pd error:&e];
    if (!p){ fprintf(stderr, "metal: chain pipeline could not be built: %s\n", e ? [[e localizedDescription] UTF8String] : "?");
             return nil; }
    g_fbpso[g_fbpso_n].ps = ps; g_fbpso[g_fbpso_n].out = out; g_fbpso[g_fbpso_n].vs = vs;
    g_fbpso[g_fbpso_n].pso = p; g_fbpso_n++;
    return p;
}
/* can the record join the chain (see conditions) */
static int fb_rec_ok(const MDraw *x){
    if (x->is_clear || x->is_blit || x->vsid <= 0 || x->ix_isz || x->count != 6u) return 0;
    if (x->ps <= 0 || x->ps >= PSCUBE || !g_ps_fbok[x->ps]) return 0;
    if (!x->target || TGT_FACE(x->target) || (x->depth & 3u)) return 0;
    unsigned t = TGT_ID(x->target), s = x->tex;
    if (t >= MAXTEX || !s || s >= MAXTEX || s == t || !g_tex[t] || !g_tex[s]) return 0;
    id<MTLTexture> T = g_tex[t], S = g_tex[s];
    if (T.textureType != MTLTextureType2D || S.textureType != MTLTextureType2D) return 0;
    if (T.width != S.width || T.height != S.height || T.mipmapLevelCount != 1 || S.mipmapLevelCount != 1) return 0;
    if (T.pixelFormat != MTLPixelFormatBGRA8Unorm || S.pixelFormat != MTLPixelFormatBGRA8Unorm) return 0;
    if (!draw_covers(x, T.width, T.height)) return 0;
    if (x->tex1 == t || x->tex1 == s || x->tex2 == t || x->tex2 == s || x->tex3 == t || x->tex3 == s) return 0;
    /* stage 0 coordinate 1:1 on the texel center (no projection) */
    int sel = (int)x->proj[0];
    if ((sel >> 4) != 0 || !x->cuvok) return 0;
    sel &= 3;
    int fx = (int)((x->uvbm >> sel) & 1u);
    float du = fx ? x->uvb[0] : 0.f, dv = fx ? x->uvb[1] : 0.f;
    float W = (float)T.width, H = (float)T.height, eu = 1e-3f / W, ev = 1e-3f / H;
    float u0 = x->cuv[0] + du, u1 = x->cuv[1] + du, vb = x->cuv[2] + dv, vt = x->cuv[3] + dv;
    if (fabsf(u0 - 0.5f / W) > eu || fabsf(u1 - (1.f + 0.5f / W)) > eu) return 0;
    if (fabsf(vt - 0.5f / H) > ev || fabsf(vb - (1.f + 0.5f / H)) > ev) return 0;
    return 1;
}
/* the frame's chains: role[i] 1 = chain start, 2 = continuation.  Number of chains. */
static int fb_plan(int rb, uint8_t *role, unsigned n){
    memset(role, 0, n);
    if (!fbz_mode() || abt_off("FBZINCIR") || rb < 0) return 0;
    const MDraw *D = g_dbuf[rb]; int nch = 0;
    for (unsigned i = 0; i + 1 < n; ){
        if (!fb_rec_ok(&D[i])){ i++; continue; }
        unsigned A = TGT_ID(D[i].target), j = i + 1;
        while (j < n && fb_rec_ok(&D[j]) && D[j].tex == TGT_ID(D[j - 1].target) &&
               TGT_ID(D[j].target) == D[j - 1].tex) j++;
        if (j - i >= 2){
            int ready = 1;
            for (unsigned k = i; k < j; k++){
                int out = (TGT_ID(D[k].target) == A) ? 0 : 1;
                if (!fb_fn_ready(D[k].ps, out) || !fb_pso(D[k].ps, out, D[k].vsid)) ready = 0; }
            if (ready){ role[i] = 1; for (unsigned k = i + 1; k < j; k++) role[k] = 2; nch++; }
        }
        i = j;
    }
    return nch;
}
/* chain [s, e]: attachment 0 = the first record's target (TA), attachment 1 = the first record's input (TB).
 * State binding is the same as the rules in the main loop (sampler, constants, textures,
 * vertex buffers, UV SCALE); stage 0 comes from tile memory. */
static id<MTLRenderPipelineState> pso_for(uint64_t key);
static int alfavar_mode(void);
/* FBZINCIR=2 + FBZCTRL=1 (diagnostic): the same helper, but every record in its OWN
 * pass and with the NORMAL pipeline (stage 0 from the texture).  Must match the main
 * loop exactly; if not, the difference is in the helper's state binding, not in the
 * tile memory read. */
static void fb_encode_ctrl(id<MTLCommandBuffer> cb, int rb, unsigned s, unsigned e,
                           id<MTLTexture> TA, id<MTLTexture> TB){
    const MDraw *D = g_dbuf[rb]; unsigned A = TGT_ID(D[s].target);
    for (unsigned k = s; k <= e; k++){
        const MDraw *dr = &D[k];
        int out = (TGT_ID(dr->target) == A) ? 0 : 1;
        id<MTLTexture> T = out ? TB : TA, S = out ? TA : TB;
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = T;
        rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        if (g_atdbg) [enc setFragmentBuffer:g_atdbg offset:0 atIndex:6];
        if (halfpx_on()){ MTLViewport vp = { 0.5, 0.5, (double)T.width, (double)T.height, 0.0, 1.0 };
                          [enc setViewport:vp]; }
        [enc setVertexBuffer:g_mtlvb offset:0 atIndex:0];
        [enc setFragmentSamplerState:g_samp atIndex:0];
        { static int cw = -1;
          if (cw < 0){ const char *ev = ENVC("CULLWIND"); cw = (ev && *ev && *ev != '0') ? 1 : 0; }
          [enc setFrontFacingWinding:cw ? MTLWindingCounterClockwise : MTLWindingClockwise]; }
        [enc setDepthStencilState:dss_for(0)];
        uint64_t k2 = (uint64_t)(dr->blend & 0xFFFFFu) | ((uint64_t)(dr->ps & 0xFFF) << 20) | ((uint64_t)(dr->vsid & 0xFFF) << 32);
        if (alfavar_mode() && !(dr->alpha_ref > 0.f)) k2 |= 1ull << 52;
        [enc setRenderPipelineState:pso_for(k2)];
        [enc setCullMode:(dr->cull == 1u) ? MTLCullModeNone : (dr->cull == 2u) ? MTLCullModeFront : MTLCullModeBack];
        for (int si = 0; si < 4; si++) [enc setFragmentSamplerState:samp_for(dr->smp[si]) atIndex:si];
        { struct { float aref, usec, p0, p1; float proj[4]; float c[PSC_N][4]; float fogv[4]; } fp;
          fp.aref = dr->alpha_ref; fp.usec = 0.f; fp.p0 = fp.p1 = 0.f;
          memcpy(fp.proj, dr->proj, sizeof fp.proj);
          memcpy(fp.c, g_pscb[rb][dr->psci], sizeof fp.c);
          memcpy(fp.fogv, dr->fogv, sizeof fp.fogv);
          [enc setFragmentBytes:&fp length:sizeof fp atIndex:0]; }
        { unsigned ts[4] = { 0, dr->tex1, dr->tex2, dr->tex3 };
          [enc setFragmentTexture:S atIndex:0];
          for (int q = 1; q < 4; q++){
              unsigned tt = ts[q];
              [enc setFragmentTexture:(tt && tt < MAXTEX && g_tex[tt]) ? g_tex[tt] : g_white atIndex:(NSUInteger)q]; } }
        { float u4[4] = { dr->uvbm ? dr->uvb[0] : 0.f, dr->uvbm ? dr->uvb[1] : 0.f, dr->xsh, 0 };
          [enc setVertexBytes:u4 length:sizeof u4 atIndex:3]; }
        [enc setVertexBuffer:g_mtlrvb offset:dr->rawoff atIndex:1];
        [enc setVertexBuffer:g_mtlkb  offset:dr->koff   atIndex:2];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:dr->count];
        [enc endEncoding];
    }
}
static void fb_encode(id<MTLCommandBuffer> cb, int rb, unsigned s, unsigned e,
                      id<MTLTexture> TA, id<MTLTexture> TB){
    const MDraw *D = g_dbuf[rb]; unsigned A = TGT_ID(D[s].target);
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = TA;
    rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;     /* the first record overwrites completely */
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[1].texture = TB;
    rp.colorAttachments[1].loadAction = MTLLoadActionLoad;         /* the chain's input */
    rp.colorAttachments[1].storeAction = MTLStoreActionStore;
    pp_rp(rp, "post-process chain (merged)");
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    atomic_fetch_add(&g_pass_n, 1ull);
    if (g_atdbg) [enc setFragmentBuffer:g_atdbg offset:0 atIndex:6];
    if (halfpx_on()){ MTLViewport vp = { 0.5, 0.5, (double)TA.width, (double)TA.height, 0.0, 1.0 };
                      [enc setViewport:vp]; }
    [enc setVertexBuffer:g_mtlvb offset:0 atIndex:0];
    [enc setFragmentSamplerState:g_samp atIndex:0];
    { static int cw = -1;
      if (cw < 0){ const char *ev = ENVC("CULLWIND"); cw = (ev && *ev && *ev != '0') ? 1 : 0; }
      [enc setFrontFacingWinding:cw ? MTLWindingCounterClockwise : MTLWindingClockwise]; }
    [enc setDepthStencilState:dss_for(0)];
    for (unsigned k = s; k <= e; k++){
        const MDraw *dr = &D[k];
        int out = (TGT_ID(dr->target) == A) ? 0 : 1;
        [enc setRenderPipelineState:fb_pso(dr->ps, out, dr->vsid)];
        [enc setCullMode:(dr->cull == 1u) ? MTLCullModeNone : (dr->cull == 2u) ? MTLCullModeFront : MTLCullModeBack];
        for (int si = 0; si < 4; si++) [enc setFragmentSamplerState:samp_for(dr->smp[si]) atIndex:si];
        { struct { float aref, usec, p0, p1; float proj[4]; float c[PSC_N][4]; float fogv[4]; } fp;
          fp.aref = dr->alpha_ref; fp.usec = 0.f; fp.p0 = fp.p1 = 0.f;
          memcpy(fp.proj, dr->proj, sizeof fp.proj);
          memcpy(fp.c, g_pscb[rb][dr->psci], sizeof fp.c);
          memcpy(fp.fogv, dr->fogv, sizeof fp.fogv);
          [enc setFragmentBytes:&fp length:sizeof fp atIndex:0]; }
        { unsigned ts[4] = { 0, dr->tex1, dr->tex2, dr->tex3 };     /* stage 0: tile memory */
          for (int q = 0; q < 4; q++){
              unsigned tt = ts[q];
              [enc setFragmentTexture:(tt && tt < MAXTEX && g_tex[tt]) ? g_tex[tt] : g_white atIndex:(NSUInteger)q]; } }
        { float u4[4] = { dr->uvbm ? dr->uvb[0] : 0.f, dr->uvbm ? dr->uvb[1] : 0.f, dr->xsh, 0 };
          [enc setVertexBytes:u4 length:sizeof u4 atIndex:3]; }
        [enc setVertexBuffer:g_mtlrvb offset:dr->rawoff atIndex:1];
        [enc setVertexBuffer:g_mtlkb  offset:dr->koff   atIndex:2];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:dr->count];
        atomic_fetch_add(&g_draw_n, 1ull);
    }
    [enc endEncoding];
}
/* FBZINCIR=2: compare the two textures on the GPU, add the differing pixel count to g_fbv_cnt */
static id<MTLComputePipelineState> g_fbv_cmp;
static id<MTLBuffer> g_fbv_cnt;
static id<MTLTexture> g_fbv_t[5];                  /* input copy, merged A, merged B, (diagnostic) second A/B */
static id<MTLTexture> fbv_tex(int k, id<MTLTexture> like){
    if (!g_fbv_t[k] || g_fbv_t[k].width != like.width || g_fbv_t[k].height != like.height){
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                    width:like.width height:like.height mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModePrivate;
        g_fbv_t[k] = [g_dev newTextureWithDescriptor:td]; }
    return g_fbv_t[k];
}
static void fbv_compare(id<MTLCommandBuffer> cb, id<MTLTexture> a, id<MTLTexture> b, int slot){
    if (!g_fbv_cmp){
        NSString *src = @"#include <metal_stdlib>\nusing namespace metal;\n"
            "kernel void fbz_cmp(texture2d<float, access::read> a [[texture(0)]],\n"
            "                    texture2d<float, access::read> b [[texture(1)]],\n"
            "                    device atomic_uint *n [[buffer(0)]], uint2 g [[thread_position_in_grid]]){\n"
            "  if (g.x >= a.get_width() || g.y >= a.get_height()) return;\n"
            "  float4 d = abs(a.read(g) - b.read(g)) * 255.0;\n"
            "  uint m = uint(round(max(max(d.x, d.y), max(d.z, d.w))));\n"
            "  if (m == 0u) return;\n"
            "  atomic_fetch_add_explicit(&n[0], 1u, memory_order_relaxed);\n"
            "  atomic_fetch_max_explicit(&n[1], m, memory_order_relaxed);\n"
            "  if (m > 1u) atomic_fetch_add_explicit(&n[2], 1u, memory_order_relaxed);\n"
            "}\n";
        NSError *er = nil;
        id<MTLLibrary> lib = [g_dev newLibraryWithSource:src options:nil error:&er];
        id<MTLFunction> f = [lib newFunctionWithName:@"fbz_cmp"];
        g_fbv_cmp = f ? [g_dev newComputePipelineStateWithFunction:f error:&er] : nil;
        g_fbv_cnt = [g_dev newBufferWithLength:32 options:MTLResourceStorageModeShared];
        memset([g_fbv_cnt contents], 0, 32);
        if (!g_fbv_cmp) return;
    }
    id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
    [ce setComputePipelineState:g_fbv_cmp];
    [ce setTexture:a atIndex:0]; [ce setTexture:b atIndex:1];
    [ce setBuffer:g_fbv_cnt offset:(NSUInteger)slot * 16u atIndex:0];
    [ce dispatchThreads:MTLSizeMake(a.width, a.height, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
    [ce endEncoding];
}
/* DUMPTEX=<id>: draw the texture onto a full-screen quad and write it as a PNG. */
static void dump_one_texture(unsigned id_, long frame){
    if (!g_dev || !g_pso || id_ >= MAXTEX || !g_tex[id_]) return;
    NSUInteger w = g_tex[id_].width, h = g_tex[id_].height;
    if (!w || !h) return;
    /* BGRA8 texture: read the RAW bytes with a blit.  The draw path below uses g_pso
     * and that has ALPHA BLENDING on: every texture with alpha below 1 was dumped
     * darkened (example: a bright-area extraction target writing alpha=0 looked
     * completely black).  This path writes RGB as is and prints alpha statistics
     * separately. */
    if (g_tex[id_].pixelFormat == MTLPixelFormatBGRA8Unorm && !ENVC("DUMPOLD")){
        NSUInteger slice = 0; long tg = 90000 + (long)id_;
        if (g_tex[id_].textureType == MTLTextureTypeCube){
            const char *fe = ENVC("DUMPFACE"); slice = fe ? (NSUInteger)atoi(fe) : 0; if (slice > 5) slice = 0;
            tg = 96000 + (long)id_*10 + (long)slice; }
        MTLTextureDescriptor *sd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                         width:w height:h mipmapped:NO];
        sd.storageMode = MTLStorageModeShared; sd.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> shd = [g_dev newTextureWithDescriptor:sd];
        if (shd){
            id<MTLCommandBuffer> cbb = [g_queue commandBuffer];
            id<MTLBlitCommandEncoder> bl = [cbb blitCommandEncoder];
            [bl copyFromTexture:g_tex[id_] sourceSlice:slice sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
                     sourceSize:MTLSizeMake(w,h,1) toTexture:shd destinationSlice:0 destinationLevel:0
              destinationOrigin:MTLOriginMake(0,0,0)];
            [bl endEncoding]; [cbb commit]; [cbb waitUntilCompleted];
            uint8_t *px = malloc(w*h*4);
            if (px){ [shd getBytes:px bytesPerRow:w*4 fromRegion:MTLRegionMake2D(0,0,w,h) mipmapLevel:0];
                double sa = 0; unsigned amin = 255, amax = 0;
                for (NSUInteger q = 0; q < w*h; q++){ unsigned a = px[q*4+3]; sa += a; if (a < amin) amin = a; if (a > amax) amax = a; }
                fprintf(stderr, "metal: texture %u (%lux%lu) RAW dump, alpha mean %.1f min %u max %u\n", id_,
                        (unsigned long)w, (unsigned long)h, sa/(double)(w*h), amin, amax);
                /* DUMPALPHA=1: also write the alpha channel as a grey picture (frame 95000+id) */
                if (ENVC("DUMPALPHA")){
                    for (NSUInteger q = 0; q < w*h; q++){ uint8_t a = px[q*4+3]; px[q*4] = px[q*4+1] = px[q*4+2] = a; px[q*4+3] = 255; }
                    MTLTextureDescriptor *ad = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                     width:w height:h mipmapped:NO];
                    ad.storageMode = MTLStorageModeShared;
                    id<MTLTexture> at = [g_dev newTextureWithDescriptor:ad];
                    if (at){ [at replaceRegion:MTLRegionMake2D(0,0,w,h) mipmapLevel:0 withBytes:px bytesPerRow:w*4];
                             dump_texture(at, 95000 + (long)id_); } }
                free(px); }
            dump_texture(shd, tg);
            return;
        }
    }
    MTLTextureDescriptor *td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                              width:w height:h mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> off = [g_dev newTextureWithDescriptor:td];
    if (!off) return;
    MVert q[6]; memset(q, 0, sizeof q);
    { const float px[6] = {-1, 1, 1,-1, 1,-1}, py[6] = { 1, 1,-1, 1,-1,-1};
      const float tu[6] = { 0, 1, 1, 0, 1, 0}, tv2[6] = { 0, 0, 1, 0, 1, 1};
      for (int i = 0; i < 6; i++){
          q[i].x = px[i]; q[i].y = py[i]; q[i].z = 0.f; q[i].w = 1.f;
          q[i].r = q[i].g = q[i].b = q[i].a = 1.f;
          for (int k = 0; k < 4; k++){
              q[i].t[k][0] = tu[i]; q[i].t[k][1] = tv2[i];
              q[i].t[k][2] = 0.f;   q[i].t[k][3] = 1.f;
          }
      } }
    memcpy([g_mtlvb contents], q, sizeof q);
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = off;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0,0,0,1);
    id<MTLCommandBuffer> cb = [g_queue commandBuffer];
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    [enc setRenderPipelineState:g_pso];
    [enc setVertexBuffer:g_mtlvb offset:0 atIndex:0];
    [enc setFragmentSamplerState:g_samp atIndex:0];
    /* DUMPMIP=<level>: show only that mip level (with a texture view).
     * To inspect the uploaded mip content directly. */
    id<MTLTexture> srct = g_tex[id_];
    long tag = 90000 + (long)id_;
    { const char *ml = ENVC("DUMPMIP");
      if (ml){ NSUInteger lv = (NSUInteger)atoi(ml);
        if (lv < g_tex[id_].mipmapLevelCount){
            id<MTLTexture> vw = [g_tex[id_] newTextureViewWithPixelFormat:g_tex[id_].pixelFormat
                                    textureType:MTLTextureType2D
                                         levels:NSMakeRange(lv,1)
                                         slices:NSMakeRange(0,1)];
            if (vw){ srct = vw; tag = 95000 + (long)id_*10 + (long)lv; }
        } else fprintf(stderr,"metal: texture %u level %lu missing (total %lu)\n",
                       id_,(unsigned long)lv,(unsigned long)g_tex[id_].mipmapLevelCount); } }
    /* Cube texture: g_pso samples 2D; Metal refuses to bind a cube as 2D.
     * Show the first face (or DUMPFACE=<0..5>) with a 2D view. */
    if (srct.textureType == MTLTextureTypeCube){
        const char *fe = ENVC("DUMPFACE"); NSUInteger fc = fe ? (NSUInteger)atoi(fe) : 0;
        if (fc > 5) fc = 0;
        id<MTLTexture> vw = [srct newTextureViewWithPixelFormat:srct.pixelFormat
                                textureType:MTLTextureType2D
                                     levels:NSMakeRange(0,1)
                                     slices:NSMakeRange(fc,1)];
        if (vw){ srct = vw; tag = 96000 + (long)id_*10 + (long)fc; }
    }
    [enc setFragmentTexture:srct atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
    [enc endEncoding];
    [cb commit]; [cb waitUntilCompleted];
    fprintf(stderr, "metal: texture %u (%lux%lu, %lu levels) dump\n", id_,
            (unsigned long)w, (unsigned long)h,
            (unsigned long)g_tex[id_].mipmapLevelCount);
    dump_texture(off, tag);
}

/* The game draws its own cursor; if the macOS cursor is also visible there are
 * two cursors on screen.  Hide the system cursor while in the game area, bring it
 * back on leaving. */
static int g_cur_hidden = 0;
static void cursor_hide_update(int inside){
    if (inside && !g_cur_hidden){ [NSCursor hide];   g_cur_hidden = 1; }
    else if (!inside && g_cur_hidden){ [NSCursor unhide]; g_cur_hidden = 0; }
}
/* --- ProMotion: display refresh rate -------------------------------------
 * macOS ProMotion picks the display rate based on the app's presentation pace;
 * our presents are tied to display sync.  2026-10-18: in the first minutes after
 * waking from sleep on low battery and being plugged in, fullscreen measured
 * exactly 60 fps and menus in a window exactly 30 fps (GPU ~6 ms per frame, not
 * load); after a few minutes it went back to 120 on its own, and the control run
 * without the switch was 120 too.  So it is a transient system state; the
 * preference below could NOT BE TESTED under a condition that would tell them
 * apart.
 * DLINK=2: no preference, measurement only -- writes the REAL refresh rate every
 * 5 s (tools/session.sh turns it on).  DLINK=1: the same, plus prefers the maximum
 * rate (CADisplayLink, macOS 14+); first candidate if the state repeats. */
static _Atomic unsigned long g_dl_ticks = 0;
@interface NfsuTick : NSObject @end
@implementation NfsuTick { double t0; unsigned long n0; }
- (void)tick:(id)link {
    (void)link;
    unsigned long n = atomic_fetch_add(&g_dl_ticks, 1ul) + 1ul;
    double t = CACurrentMediaTime();
    if (t0 == 0.0){ t0 = t; n0 = n; return; }
    if (t - t0 >= 5.0){
        fprintf(stderr, "[display] %.1f Hz (frame %ld)\n", (double)(n - n0) / (t - t0),
                (long)atomic_load(&g_frames_drawn));
        t0 = t; n0 = n; }
}
@end
static NfsuTick *g_dltarget;
static id g_dlink;
static void dlink_start(NSView *v){
    const char *e = ENVC("DLINK");
    int mode = (e && *e) ? atoi(e) : 0;
    if (mode <= 0) return;
    if (@available(macOS 14.0, *)){
        g_dltarget = [NfsuTick new];
        CADisplayLink *dl = [v displayLinkWithTarget:g_dltarget selector:@selector(tick:)];
        if (mode == 1){
            NSScreen *sc = [[v window] screen] ?: [NSScreen mainScreen];
            float mx = (float)[sc maximumFramesPerSecond];
            if (mx < 60.f) mx = 60.f;
            dl.preferredFrameRateRange = CAFrameRateRangeMake(mx, mx, mx);
        }
        [dl addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
        g_dlink = dl;
        fprintf(stderr, "metal: display link (CADisplayLink): %s\n",
                mode == 1 ? "prefer maximum rate" : "measurement only");
    } else fprintf(stderr, "metal: DLINK requires macOS 14\n");
}
@interface NfsuView : NSView @end
@implementation NfsuView
- (BOOL)wantsUpdateLayer { return YES; }
- (CALayer *)makeBackingLayer {
    CAMetalLayer *l = [CAMetalLayer layer];
    l.device = g_dev;
    l.pixelFormat = MTLPixelFormatBGRA8Unorm;
    l.framebufferOnly = NO;   /* must be readable for frame dumps (see FBONLY) */
    l.opaque = YES;
    return l;
}
@end

/* The fullscreen preference is remembered across runs.  NFSFULL=1/0 overrides it
 * (measurement runs use NFSFULL=0, independent of the user's preference). */
static void fs_pref_save(int on){
    const char *e = ENVC("NFSFULL");
    if (e && *e) return;                 /* a forced mode must not change the preference */
    [[NSUserDefaults standardUserDefaults] setBool:on ? YES : NO forKey:@"NFSUFullScreen"];
}
static int fs_pref_load(void){
    const char *e = ENVC("NFSFULL");
    if (e && *e) return e[0] != '0';
    return [[NSUserDefaults standardUserDefaults] boolForKey:@"NFSUFullScreen"] ? 1 : 0;
}

/* ---- SYSTEM SHORTCUT BLOCK ---------------------------------------------
 * While the game is in front, macOS's Space / Mission Control shortcuts are off.
 * 2026-10-19 session: pressing a game key with Ctrl together with an arrow key
 * (Ctrl+Left/Right) switched the desktop; the game left fullscreen game mode for
 * 1.6 s, the frame froze in nextDrawable for 0.54 s.  The shortcut state is kept
 * in the window server, isn't written to the preferences file and does NOT COME
 * BACK BY ITSELF when the process dies (tested with SIGKILL).  So on the first
 * disable a small watchdog process is spawned from the same binary
 * (NFSU_KISAYOL_BEKCI, the first thing tests/boot.c main does): it learns the
 * disabled ids through a pipe, and when the pipe closes -- however the game ends --
 * it re-enables them.  When the game goes to the background the shortcuts are
 * re-enabled right away; Cmd+Tab, Spotlight and screenshot shortcuts are untouched.
 * NFSKISAYOL=0: don't touch at all; =2 (test): disable even while the window is
 * in the background.  In test runs with NOFOCUS it doesn't kick in unless =2. */
#include <spawn.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <mach-o/dyld.h>
extern char **environ;
typedef int  (*ks_set_fn)(unsigned, bool);
typedef bool (*ks_is_fn)(unsigned);
static ks_set_fn g_ks_set;
static ks_is_fn  g_ks_is;
/* Mission Control 32/34, application windows 33/35, show desktop 36/37,
 * previous/next input source 60/61 (Ctrl+Space), Space left/right/up/down
 * 79-86, switch to Desktop 1-16 118-133 (Ctrl+1..).  The ids are the same as
 * com.apple.symbolichotkeys. */
static const unsigned short g_ks_ids[] = { 32, 33, 34, 35, 36, 37, 60, 61,
    79, 80, 81, 82, 83, 84, 85, 86, 118, 119, 120, 121, 122, 123, 124, 125,
    126, 127, 128, 129, 130, 131, 132, 133 };
#define KS_N ((int)(sizeof g_ks_ids / sizeof g_ks_ids[0]))
static unsigned char g_ks_off[KS_N];     /* the ones we disabled */
static int g_ks_on = 0, g_ks_fd = -1;

static int ks_api(void){
    static int done = 0;
    if (!done){ done = 1;
        void *h = dlopen("/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight", RTLD_LAZY);
        g_ks_set = h ? (ks_set_fn)dlsym(h, "SLSSetSymbolicHotKeyEnabled") : NULL;
        g_ks_is  = h ? (ks_is_fn)dlsym(h, "SLSIsSymbolicHotKeyEnabled") : NULL;
        if (!g_ks_set || !g_ks_is) fprintf(stderr, "app: no system shortcut API, block off\n"); }
    return g_ks_set && g_ks_is;
}
/* 0 = don't touch, 1 = disable while the game is in front, 2 = (test) disable in the background too */
static int ks_wanted(void){
    static int w = -1;
    if (w < 0){ const char *e = ENVC("NFSKISAYOL"), *nf = ENVC("NOFOCUS");
        if (e && e[0] == '2') w = 2;
        else w = !(e && e[0] == '0') && !(nf && nf[0] && nf[0] != '0'); }
    return w;
}
/* Watchdog: "d <id>" (disabled) / "e <id>" (re-enabled) lines from stdin;
 * EOF = the game ended -> enable those still disabled. */
int metal_kisayol_bekci(void){
    signal(SIGINT, SIG_IGN); signal(SIGHUP, SIG_IGN);
    signal(SIGTERM, SIG_IGN); signal(SIGQUIT, SIG_IGN);
    unsigned char off[256] = {0};
    char line[32];
    while (fgets(line, sizeof line, stdin)){
        unsigned id = (unsigned)strtoul(line + 1, NULL, 10);
        if (id < 256) off[id] = line[0] == 'd';
    }
    int n = 0;
    if (ks_api()) for (unsigned id = 0; id < 256; id++) if (off[id]){ g_ks_set(id, true); n++; }
    if (n) fprintf(stderr, "shortcut watchdog: the game ended unexpectedly, %d system shortcuts re-enabled\n", n);
    return 0;
}
static void ks_bekci_start(void){
    char path[4096]; uint32_t n = sizeof path;
    if (_NSGetExecutablePath(path, &n) != 0) return;
    int p[2]; if (pipe(p) != 0) return;
    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, p[0], 0);
    posix_spawn_file_actions_addinherit_np(&fa, 2);   /* the watchdog's note goes to the game's log */
    posix_spawnattr_t at; posix_spawnattr_init(&at);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_CLOEXEC_DEFAULT);
    char *av[] = { path, NULL };
    char *ev[] = { "NFSU_KISAYOL_BEKCI=1", NULL };
    pid_t pid;
    int r = posix_spawn(&pid, path, &fa, &at, av, ev);
    posix_spawn_file_actions_destroy(&fa); posix_spawnattr_destroy(&at);
    close(p[0]);
    if (r != 0){ close(p[1]);
        fprintf(stderr, "app: shortcut watchdog could not be started (%d), block off\n", r); return; }
    fcntl(p[1], F_SETFD, FD_CLOEXEC);
    fcntl(p[1], F_SETNOSIGPIPE, 1);     /* if the watchdog dies, writing must not kill the game */
    g_ks_fd = p[1];
}
static void ks_note(char op, unsigned id){
    char b[16]; int n = snprintf(b, sizeof b, "%c %u\n", op, id);
    if (g_ks_fd >= 0) (void)!write(g_ks_fd, b, (size_t)n);
}
static void ks_engel(int on){
    if (on == g_ks_on) return;
    g_ks_on = on;
    if (!ks_api()) return;
    int n = 0;
    if (on){
        if (g_ks_fd < 0) ks_bekci_start();
        if (g_ks_fd < 0) return;        /* no disabling without a watchdog: on a crash they would stay disabled */
        for (int i = 0; i < KS_N; i++){
            unsigned id = g_ks_ids[i];
            if (!g_ks_is(id)) continue;  /* the user already disabled it */
            ks_note('d', id);            /* watchdog first: if we crash in between it re-enables */
            if (g_ks_set(id, false) == 0){ g_ks_off[i] = 1; n++; }
            else ks_note('e', id);
        }
    } else {
        for (int i = 0; i < KS_N; i++) if (g_ks_off[i]){
            g_ks_set(g_ks_ids[i], true); g_ks_off[i] = 0; ks_note('e', g_ks_ids[i]); n++; }
    }
    fprintf(stderr, "app: system shortcuts %s (%d)\n", on ? "disabled (game in front)" : "re-enabled", n);
}

@interface NfsuDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate> @end
@implementation NfsuDelegate
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)s { (void)s; return YES; }
- (BOOL)windowShouldClose:(NSWindow *)w { (void)w;
    fprintf(stderr, "app: window closed -> exit\n");
    atomic_store(&g_should_quit, 1); return YES; }
/* When the user enlarges the window or goes fullscreen the content size changes;
 * if the mouse mapping doesn't know, the cursor drifts. */
/* On entering fullscreen measure the content area, save the preference and make
 * the game Reset to the new size (see g_fs). */
- (void)windowDidEnterFullScreen:(NSNotification *)n {
    NSWindow *w = [n object];
    NSSize c = [[w contentView] bounds].size;
    CGFloat s = [w backingScaleFactor]; if (s < 1.0) s = 1.0;
    atomic_store(&g_fs_pw, (int)lround(c.width * s));
    atomic_store(&g_fs_ph, (int)lround(c.height * s));
    atomic_store(&g_cont_w, (int)c.width); atomic_store(&g_cont_h, (int)c.height);
    atomic_store(&g_fs, 1);
    fs_pref_save(1);
    fprintf(stderr, "app: fullscreen %.0fx%.0f points = %dx%d pixels -> rebuilding the device\n",
            (double)c.width, (double)c.height, atomic_load(&g_fs_pw), atomic_load(&g_fs_ph));
    com_request_reset(); }
- (void)windowDidExitFullScreen:(NSNotification *)n { (void)n;
    atomic_store(&g_fs, 0);
    fs_pref_save(0);
    fprintf(stderr, "app: back to window mode -> rebuilding the device\n");
    com_request_reset(); }
- (void)windowDidResignKey:(NSNotification *)n { (void)n; cursor_hide_update(0); }
/* The user resized the window by hand: the drawing resolution is chosen again for
 * the new ratio (with the game's own Reset, see com_request_reset). */
- (void)windowDidEndLiveResize:(NSNotification *)n {
    NSSize c = [[[n object] contentView] bounds].size;
    atomic_store(&g_cont_w, (int)c.width); atomic_store(&g_cont_h, (int)c.height);
    if (!atomic_load(&g_fs)) com_request_reset(); }
- (void)windowDidResize:(NSNotification *)n {
    NSSize c = [[[n object] contentView] bounds].size;
    atomic_store(&g_cont_w, (int)c.width);
    atomic_store(&g_cont_h, (int)c.height); }
/* Cmd-Q: DON'T terminate directly.  Cutting the guest thread off in the middle
 * leads to a shutdown race (see sub_0050d510).  We tell the draw loop to "stop"
 * and return CANCEL; when the loop exits main() writes the report and ends.
 *
 * LATER was tried and is UNUSABLE: while waiting for the reply AppKit runs its
 * OWN run loop, our draw loop never returns -> shutdown hangs.  Cancel's only
 * side effect is a warning on logout; the app exits by itself right after anyway. */
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)s {
    (void)s; fprintf(stderr, "app: quit request (Cmd-Q / Dock)\n");
    atomic_store(&g_should_quit, 1); return NSTerminateCancel; }
@end

#if NFS_ENHANCED
/* ---- SEPARATE G PASS ----------------------------------------------------------
 * Measured (2026-10-21, Quick Race capacity mode, 8.40 ms/frame at the highest clock):
 * MSAA 2x costs 0.9 ms without the G-buffer, ~3 ms with it.  G (3 targets, 12 bytes
 * per sample) was carried in tile memory as 2 samples together with color + Z in
 * the multisampled scene pass and resolved; in the motion blur split (see MID-SCENE
 * COPY) it was also stored and loaded back.  On top of that the reflection variant
 * evaluates the game's shader THREE TIMES (for R0, B) and that was in the
 * multisampled pass too.  Now:
 *  - The scene pass has no G; reflecting draws are drawn there with the game's OWN
 *    shader (identical to the variant's c0).
 *  - When the scene ends (right before the composite) a SINGLE-SAMPLED G pass
 *    redraws only the opaque reflection variant draws: target the G textures
 *    (clean), Z = the scene's resolved Z twin (Sample0), <= test without writing Z.
 *    Every Z-writing surface in front of them culls them -> class stays 0 (the same
 *    result as the old non-reflecting opaque draw zeroing G).  The quarter-pixel
 *    offset between the single-sample center and Sample0 is covered by a
 *    slope-based Z bias.
 *  - Blended draws don't write G, as before (not redrawn).
 * HD_GSEP=0 old; ABTEST=GSEP compares in the same run. */
static int hd_gsep(void){
    static int v = -1; if (v < 0){ const char *e = ENVC("HD_GSEP"); v = (e && *e == '0') ? 0 : 1; }
    return v && hd_msaa_n() > 1 && !abt_off("GSEP");
}
static id<MTLTexture> g_hd_gp_c0;                 /* color 0 of the G pass: not written (memoryless) */
static _Atomic unsigned long long g_hd_gp_n, g_hd_gp_draws;
static void hd_gpass(id<MTLCommandBuffer> cb, int rb, unsigned upto, unsigned zid){
    id<MTLTexture> zt = (zid && zid < MAXZ) ? g_ztex[zid] : nil;
    if (rb < 0 || !zt || !g_hd_g[0] || zt.width != g_hd_g[0].width || zt.height != g_hd_g[0].height) return;
    NSUInteger W = zt.width, H = zt.height;
    if (!g_hd_gp_c0 || g_hd_gp_c0.width != W || g_hd_gp_c0.height != H){
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                         width:W height:H mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget;
        td.storageMode = hd_ml_hw() ? MTLStorageModeMemoryless : MTLStorageModePrivate;
        g_hd_gp_c0 = [g_dev newTextureWithDescriptor:td];
    }
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = g_hd_gp_c0;
    rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    rp.colorAttachments[0].storeAction = MTLStoreActionDontCare;
    for (int gi = 0; gi < HD_NG; gi++){
        MTLRenderPassColorAttachmentDescriptor *ga = rp.colorAttachments[1 + gi];
        ga.texture = g_hd_g[gi];
        ga.loadAction = MTLLoadActionClear;
        ga.storeAction = MTLStoreActionStore;
        ga.clearColor = (gi == 2 && hd_gpack())           /* packed: zero direction = 512 */
            ? MTLClearColorMake(512.0 / 1023.0, 512.0 / 1023.0, 512.0 / 1023.0, 0)
            : MTLClearColorMake(0, 0, 0, 0);
    }
    rp.depthAttachment.texture = zt;
    rp.depthAttachment.loadAction = MTLLoadActionLoad;
    rp.depthAttachment.storeAction = MTLStoreActionStore;
    pp_rp(rp, "G pass (single sampled)");
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
    if (halfpx_on()){ MTLViewport vp = { 0.5, 0.5, (double)W, (double)H, 0.0, 1.0 }; [e setViewport:vp]; }
    [e setVertexBuffer:g_mtlvb offset:0 atIndex:0];
    { float z4[4] = {0, 0, 0, 0}; [e setVertexBytes:z4 length:sizeof z4 atIndex:3]; }
    { static int cw = -1;
      if (cw < 0){ const char *ev = ENVC("CULLWIND"); cw = (ev && *ev && *ev != '0') ? 1 : 0; }
      [e setFrontFacingWinding:cw ? MTLWindingCounterClockwise : MTLWindingClockwise]; }
    if (g_atdbg) [e setFragmentBuffer:g_atdbg offset:0 atIndex:6];
    /* Z: <= test, no write (see dss_for: bit0 test, bit1 write, bit2-4 D3DCMP) */
    [e setDepthStencilState:dss_for(1u | (4u << 2))];
    [e setDepthBias:0.f slopeScale:-1.0f clamp:0.f];
    unsigned nd = 0;
    const MDraw *D = g_dbuf[rb];
    for (unsigned j = 0; j < upto && j < g_dn[rb]; j++){
        const MDraw *dr = &D[j];
        if (dr->is_blit || dr->is_clear || !dr->count || dr->target || dr->ps < PSREFL || (dr->blend & 1u)) continue;
        if (!(dr->depth & 1u) && !(dr->depth & 2u)) continue;   /* draw without Z: its place in the scene is unknown */
        uint64_t k2 = (uint64_t)(dr->blend & 0xFFFFFu & ~(15u << 14))   /* color 0 isn't written */
                    | ((uint64_t)(dr->ps & 0xFFF) << 20)
                    | ((uint64_t)(dr->vsid & 0xFFF) << 32)
                    | (1ull << 49);
        if ((alfavar_mode() && !(dr->alpha_ref > 0.f) && !abt_off("ALFAVAR")) || alfavar_mode() == 3) k2 |= 1ull << 52;
        [e setRenderPipelineState:pso_for(k2)];
        [e setCullMode:(dr->cull == 1u) ? MTLCullModeNone : (dr->cull == 2u) ? MTLCullModeFront : MTLCullModeBack];
        [e setBlendColorRed:(float)((dr->bcol>>16)&0xFFu)/255.f green:(float)((dr->bcol>>8)&0xFFu)/255.f
                       blue:(float)(dr->bcol&0xFFu)/255.f alpha:(float)((dr->bcol>>24)&0xFFu)/255.f];
        for (int si = 0; si < 4; si++) [e setFragmentSamplerState:samp_for(dr->smp[si]) atIndex:(NSUInteger)si];
        { struct { float aref, usec, p0, p1; float proj[4]; float c[PSC_N][4]; float fogv[4]; } fp;
          fp.aref = dr->alpha_ref; fp.usec = 0.f; fp.p0 = fp.p1 = 0.f;
          memcpy(fp.proj, dr->proj, sizeof fp.proj);
          memcpy(fp.fogv, dr->fogv, sizeof fp.fogv);
          memcpy(fp.c, g_pscb[rb][dr->psci], sizeof fp.c);
          [e setFragmentBytes:&fp length:sizeof fp atIndex:0]; }
        { unsigned ts[4] = { dr->tex, dr->tex1, dr->tex2, dr->tex3 };
          for (int q4 = 0; q4 < 4; q4++){ unsigned tt = ts[q4];
              [e setFragmentTexture:(tt && tt < MAXTEX && g_tex[tt]) ? g_tex[tt] : g_white atIndex:(NSUInteger)q4]; } }
        if (dr->vsid > 0 && dr->ix_isz){
            id<MTLBuffer> pl = g_pool_fr[rb];
            [e setVertexBuffer:pl offset:dr->rawoff atIndex:1];
            [e setVertexBuffer:g_mtlkb offset:dr->koff atIndex:2];
            [e drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:dr->count
                           indexType:(dr->ix_isz == 4u) ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16
                         indexBuffer:pl indexBufferOffset:dr->ix_ib instanceCount:1 baseVertex:dr->ix_base baseInstance:0];
        } else if (dr->vsid > 0){
            float u4[4] = { dr->uvbm ? dr->uvb[0] : 0.f, dr->uvbm ? dr->uvb[1] : 0.f, dr->xsh, 0 };
            [e setVertexBytes:u4 length:sizeof u4 atIndex:3];
            [e setVertexBuffer:g_mtlrvb offset:dr->rawoff atIndex:1];
            [e setVertexBuffer:g_mtlkb offset:dr->koff atIndex:2];
            [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:dr->count];
        } else
            [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:dr->first vertexCount:dr->count];
        nd++;
    }
    [e endEncoding];
    atomic_fetch_add(&g_hd_gp_n, 1ull); atomic_fetch_add(&g_hd_gp_draws, nd);
}
#endif
/* Must be called on the main thread.  Opens the window and runs the NSApp loop;
 * doesn't return (until the app quits).                                */
void metal_run_main_loop(void){
    @autoreleasepool {
        metal_early_init();
        if (!g_dev) return;

        [NSApplication sharedApplication];
        /* NOFOCUS=1 (test runs): no Dock icon, doesn't steal focus; the window opens
         * in the background.  The user's keyboard doesn't go to the test window. */
        [NSApp setActivationPolicy:(ENVC("NOFOCUS") && ENVC("NOFOCUS")[0] != '0')
                                   ? NSApplicationActivationPolicyAccessory
                                   : NSApplicationActivationPolicyRegular];
        NfsuDelegate *del = [NfsuDelegate new];
        [NSApp setDelegate:del];
        /* We don't call [NSApp run] (the event loop is driven by hand below).
         * finishLaunching is normally run's first job: it registers the AppleEvent
         * handlers.  Without it the app never responds to "Quit" (Dock > Quit,
         * logout, osascript quit) -- the process hangs. */
        [NSApp finishLaunching];

        /* Menu bar.  Since the event loop is driven by hand below AppKit doesn't set
         * it up by itself: in an app without a menu Cmd-Q, Cmd-H, Cmd-M don't work
         * and the window looks "frozen". */
        {   NSMenu *bar = [NSMenu new];
            NSMenuItem *appItem = [NSMenuItem new];
            [bar addItem:appItem];
            NSMenu *appMenu = [NSMenu new];
            [appMenu addItemWithTitle:@"NFSU2 Hakkinda"
                               action:@selector(orderFrontStandardAboutPanel:)
                        keyEquivalent:@""];
            [appMenu addItem:[NSMenuItem separatorItem]];
            [appMenu addItemWithTitle:@"Gizle" action:@selector(hide:) keyEquivalent:@"h"];
            [appMenu addItemWithTitle:@"Kucult" action:@selector(performMiniaturize:)
                        keyEquivalent:@"m"];
            [appMenu addItemWithTitle:@"Tam Ekran"
                               action:@selector(toggleFullScreen:) keyEquivalent:@"f"];
            /* macOS's standard shortcut Ctrl+Cmd+F (without a menu item it wasn't bound to
             * anything); it sits hidden in the menu. */
            { NSMenuItem *fs2 = [appMenu addItemWithTitle:@"Tam Ekran (Ctrl+Cmd+F)"
                                   action:@selector(toggleFullScreen:) keyEquivalent:@"f"];
              [fs2 setKeyEquivalentModifierMask:NSEventModifierFlagControl | NSEventModifierFlagCommand];
              if (@available(macOS 13.0, *)){
                  [fs2 setHidden:YES]; [fs2 setAllowsKeyEquivalentWhenHidden:YES]; } }
            [appMenu addItem:[NSMenuItem separatorItem]];
            [appMenu addItemWithTitle:@"NFSU2'den Cik" action:@selector(terminate:)
                        keyEquivalent:@"q"];
            [appItem setSubmenu:appMenu];
            [NSApp setMainMenu:bar];
        }

        /* The window size was FIXED at 1280x720: even if the game requested another
         * resolution with CreateDevice (g_RacingResolution in the registry) the
         * window didn't change.  Since the device can be created BEFORE Metal, we
         * read the requested size from here. */
        unsigned rw = g_want_w ? g_want_w : 1280u, rh = g_want_h ? g_want_h : 720u;
        /* If the game hasn't created its device yet the placeholder is Retina-scaled
         * too; otherwise the window first opens at half size (640x360 points) and
         * grows when CreateDevice comes.  Since nearly all of the game's modes are
         * 4:3 the placeholder has the 1280x960 ratio: in the most common case the
         * window doesn't jump at all at CreateDevice. */
        if (!g_want_w){ const char *nv = ENVC("NFSNATIVE");
            if (!(nv && nv[0] == '0')){ rw = 1280u; rh = 960u;
                                         metal_native_size(1280u, 960u, &rw, &rh); } }
        NSRect frame = NSMakeRect(0, 0, (CGFloat)rw, (CGFloat)rh);
        g_win = [[NSWindow alloc] initWithContentRect:frame
                    styleMask:NFSU_WIN_STYLE
                      backing:NSBackingStoreBuffered defer:NO];
        [g_win setTitle:NFS_ENHANCED ? @"Need for Speed Underground 2 HD - native ARM64"
                                     : @"Need for Speed Underground 2 - native ARM64"];
        [g_win setDelegate:del];

        NfsuView *view = [[NfsuView alloc] initWithFrame:frame];
        [view setWantsLayer:YES];
        [g_win setContentView:view];
        g_layer = (CAMetalLayer *)view.layer;
        dlink_start(view);
        /* Needed so StretchRect can read from the back buffer.  FBONLY=1 (test):
         * render-target-only drawables (compressible, eligible for direct display). */
        g_layer.framebufferOnly = fbonly_on() ? YES : NO;
        g_layer.opaque = YES;
        /* NOVSYNC=1: turn off vsync.  For MEASUREMENT only -- with it off the image
         * tears, 1:1 fidelity breaks. */
        if (ENVC("NOVSYNC")) g_layer.displaySyncEnabled = NO;

        /* The green button goes fullscreen.  The drawing surface stays at the size the
         * game requested; it is scaled up KEEPING the aspect ratio, otherwise a 5:4
         * image is stretched on a wide screen. */
        [g_win setCollectionBehavior:[g_win collectionBehavior] |
                NSWindowCollectionBehaviorFullScreenPrimary];
        g_layer.contentsGravity = kCAGravityResizeAspect;
        g_win.backgroundColor = [NSColor blackColor];

        /* Match the drawing surface to the window 1:1 (see win_apply_size). */
        win_apply_size(rw, rh);
        [g_win center];
        [g_win setAcceptsMouseMovedEvents:YES];
        /* NOFOCUS=1: open in the background, DON'T take focus from the app the user is
         * working in (for automated test/measurement runs). */
        if (ENVC("NOFOCUS") && ENVC("NOFOCUS")[0] != '0'){
            /* NOFOCUS=2: the window must NEVER be visible (fully transparent, doesn't swallow
             * the mouse).  Since it stays on screen as far as the window server is
             * concerned the drawable flow and DUMPFRAME work the same; with =1 the
             * window was in the background but visible. */
            if (ENVC("NOFOCUS")[0] == '2'){ [g_win setAlphaValue:0.0];
                                            [g_win setIgnoresMouseEvents:YES];
                                            [g_win setHasShadow:NO]; }
            [g_win orderBack:nil];
        }
        else { [g_win makeKeyAndOrderFront:nil];
               [NSApp activateIgnoringOtherApps:YES]; }
        if (fs_pref_load()) [g_win toggleFullScreen:nil];

        /* draw resources were set up in metal_early_init() */
#ifndef NFS_VERSION
#define NFS_VERSION 0
#endif
        fprintf(stderr, "version: v%d (%s)\n", NFS_VERSION, NFS_ENHANCED ? "HD" : "normal");
        fprintf(stderr, "metal: window opened (%s), draw path %s\n",
                [[g_dev name] UTF8String], g_pso ? "HAZIR" : "NONE");

        /* QUITFRAMES=<n>: exit CLEANLY after n frames are presented.  The ALARM path
         * _exits from the signal handler; that path skips atexit (and the PGO
         * profile write). */
        long quitat = 0;
        { const char *e = ENVC("QUITFRAMES"); if (e && *e) quitat = atol(e); }

        /* Draw loop: present the queued frame at 60 Hz */
        while (!atomic_load(&g_should_quit)){
            if (quitat && atomic_load(&g_frames_drawn) >= quitat){
                atomic_store(&g_should_quit, 1); break; }
            rt_free_pending();
            /* FSFAKE=<frame>[,...]: simulate the fullscreen transition from the GAME's point of
             * view without touching the window (view = screen size, device lost);
             * real fullscreen isn't reliable in the background (test). */
            { static long ff[8]; static int fn = -1, fi = 0;
              if (fn < 0){ fn = 0; const char *e = ENVC("FSFAKE");
                  for (const char *p = e; p && *p && fn < 8; ){
                      ff[fn++] = atol(p); while (*p && *p != ',') p++; if (*p) p++; } }
              if (fi < fn && atomic_load(&g_frames_drawn) >= ff[fi]){
                  fi++;
                  int on = !atomic_load(&g_fs);
                  if (on){ NSScreen *sc = [NSScreen mainScreen]; CGFloat k = [sc backingScaleFactor];
                           NSRect fr = [sc frame]; CGFloat top = 0;
                           if (@available(macOS 12.0, *)) top = [sc safeAreaInsets].top;
                           atomic_store(&g_fs_pw, (int)lround(fr.size.width * k));
                           atomic_store(&g_fs_ph, (int)lround((fr.size.height - top) * k)); }
                  atomic_store(&g_fs, on);
                  fprintf(stderr, "app: FSFAKE frame %ld -> %s (%dx%d)\n", atomic_load(&g_frames_drawn),
                          on ? "fullscreen" : "window", atomic_load(&g_fs_pw), atomic_load(&g_fs_ph));
                  com_request_reset(); } }
            /* FSSIM=<frame>:<k|m|c>[,...]: queue SYNTHETIC input at that frame (test):
             * k = fn+F, m = Ctrl+Cmd+F, c = click on the green button.
             * The events go through the event loop below like real input. */
            { static long sf[8]; static char sm[8]; static int sn = -1, si = 0;
              if (sn < 0){ sn = 0; const char *e = ENVC("FSSIM");
                  for (const char *p = e; p && *p && sn < 8; ){
                      sf[sn] = atol(p); while (*p && *p != ':' && *p != ',') p++;
                      sm[sn] = (*p == ':') ? p[1] : 'k'; sn++;
                      while (*p && *p != ',') p++; if (*p) p++; } }
              if (si < sn && atomic_load(&g_frames_drawn) >= sf[si]){
                  char md = sm[si++]; NSInteger wn = [g_win windowNumber];
                  fprintf(stderr, "app: FSSIM frame %ld mode %c\n", atomic_load(&g_frames_drawn), md);
                  if (md == 'k' || md == 'm'){
                      NSEventModifierFlags fl = md == 'k' ? NSEventModifierFlagFunction
                          : (NSEventModifierFlagControl | NSEventModifierFlagCommand);
                      for (int ud = 0; ud < 2; ud++){
                          NSEvent *ke = [NSEvent keyEventWithType:ud ? NSEventTypeKeyUp : NSEventTypeKeyDown
                              location:NSZeroPoint modifierFlags:fl timestamp:0 windowNumber:wn
                              context:nil characters:@"f" charactersIgnoringModifiers:@"f"
                              isARepeat:NO keyCode:0x03];
                          [NSApp postEvent:ke atStart:NO]; }
                  } else {
                      NSButton *zb = [g_win standardWindowButton:NSWindowZoomButton];
                      NSPoint cp = zb ? [zb convertPoint:NSMakePoint(NSMidX([zb bounds]), NSMidY([zb bounds])) toView:nil]
                                      : NSZeroPoint;
                      for (int ud = 0; ud < 2; ud++){
                          NSEvent *me = [NSEvent mouseEventWithType:ud ? NSEventTypeLeftMouseUp : NSEventTypeLeftMouseDown
                              location:cp modifierFlags:0 timestamp:0 windowNumber:wn context:nil
                              eventNumber:0 clickCount:1 pressure:ud ? 0.f : 1.f];
                          [NSApp postEvent:me atStart:NO]; }
                  } } }
            /* FSTOGGLE=<frame>[,<frame>...]: fullscreen toggle at those frames (test) */
            { static long tg[8]; static int tn = -1, ti = 0;
              if (tn < 0){ tn = 0; const char *e = ENVC("FSTOGGLE");
                  for (const char *p = e; p && *p && tn < 8; ){
                      tg[tn++] = atol(p); while (*p && *p != ',') p++; if (*p) p++; } }
              if (ti < tn && atomic_load(&g_frames_drawn) >= tg[ti]){
                  ti++; fprintf(stderr, "app: FSTOGGLE frame %ld\n", atomic_load(&g_frames_drawn));
                  [g_win toggleFullScreen:nil]; } }
            /* System shortcuts are off while the game is in front (see ks_engel) */
            { int kw = ks_wanted(); if (kw) ks_engel(kw == 2 || [NSApp isActive]); }
            /* BGFPS=<n>: while another app is in front (Spotlight, Cmd-Tab...) present
             * at most n frames a second. */
            { static int bg = -2; if (bg == -2){ const char *e = ENVC("BGFPS"); bg = e ? atoi(e) : 0; }
              if (bg > 0 && ![NSApp isActive]) usleep((useconds_t)(1000000 / bg)); }
            /* FPSCAP=<n> (test): at most n frames a second, always */
            { static int cap = -2; if (cap == -2){ const char *e = ENVC("FPSCAP"); cap = e ? atoi(e) : 0; }
              if (cap > 0) usleep((useconds_t)(1000000 / cap)); }
            @autoreleasepool {
                NSEvent *ev;
                while ((ev = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                untilDate:nil
                                                   inMode:NSDefaultRunLoopMode
                                                  dequeue:YES])){
                    NSEventType et = [ev type];
                    if (et == NSEventTypeKeyDown || et == NSEventTypeKeyUp){
                        /* Cmd shortcuts must go to the menu, NOT TO THE GAME:
                         * otherwise Cmd-Q doesn't work.  The game itself doesn't
                         * use Cmd, no conflict. */
                        /* macOS's fullscreen shortcut fn/Globe+F: since it doesn't carry
                         * Command it went to the game as "F". */
                        if ([ev keyCode] == 0x03 &&
                            ([ev modifierFlags] & NSEventModifierFlagFunction)){
                            if (et == NSEventTypeKeyDown && ![ev isARepeat])
                                [g_win toggleFullScreen:nil];
                            continue; }
                        if ([ev modifierFlags] & NSEventModifierFlagCommand){
                            /* see CAPTURE SHORTCUT */
                            if ([ev keyCode] == 0x02 && ([ev modifierFlags] & NSEventModifierFlagShift)){
                                if (et == NSEventTypeKeyDown && ![ev isARepeat]) atomic_store(&g_dump_req, 1);
                                continue; }
                            [NSApp sendEvent:ev]; continue; }
                        unsigned char dik = mac_to_dik([ev keyCode]);
                        if (dik) atomic_store(&g_keys[dik], et == NSEventTypeKeyDown ? 1 : 0);
                        /* While held, repeats are written too (like Windows). */
                        if (et == NSEventTypeKeyDown) char_from_event(ev);
                        continue;                    /* goes to the game, not the system */
                    }
                    /* If the mouse event is OUTSIDE the game area (title bar:
                     * close/minimize/fullscreen buttons, dragging the window,
                     * resizing from the edge) it must go to AppKit.  All
                     * clicks used to be swallowed by the game; the green button
                     * didn't work. */
                    int m_ev = (et == NSEventTypeMouseMoved || et == NSEventTypeLeftMouseDragged ||
                                et == NSEventTypeRightMouseDragged || et == NSEventTypeLeftMouseDown ||
                                et == NSEventTypeLeftMouseUp || et == NSEventTypeRightMouseDown ||
                                et == NSEventTypeRightMouseUp);
                    if (m_ev){
                        int inside = [ev window] == g_win &&
                            NSPointInRect([ev locationInWindow], [[g_win contentView] frame]);
                        if (et == NSEventTypeLeftMouseUp)  atomic_fetch_and(&g_mouse_btn, ~1u);
                        if (et == NSEventTypeRightMouseUp) atomic_fetch_and(&g_mouse_btn, ~2u);
                        if (!inside){
                            if (et == NSEventTypeMouseMoved) cursor_hide_update(0);
                            [NSApp sendEvent:ev]; continue; }
                    }
                    if (et == NSEventTypeMouseMoved || et == NSEventTypeLeftMouseDragged ||
                        et == NSEventTypeRightMouseDragged){
                        NSPoint pt = [ev locationInWindow];
                        cursor_hide_update([ev window] == g_win &&
                            NSPointInRect(pt, [[g_win contentView] frame]) && [g_win isKeyWindow]);
                        int nx, ny; mouse_from_point(pt.x, pt.y, &nx, &ny);
                        double msx, msy; mouse_delta_scale(&msx, &msy);
                        atomic_fetch_add(&g_mouse_dx, (int)([ev deltaX] * msx));
                        atomic_fetch_add(&g_mouse_dy, (int)([ev deltaY] * msy));
                        atomic_store(&g_mouse_x, nx);
                        atomic_store(&g_mouse_y, ny);
                        continue;
                    }
                    if (et == NSEventTypeLeftMouseDown)  { atomic_fetch_or(&g_mouse_btn, 1u); continue; }
                    if (et == NSEventTypeLeftMouseUp)    { atomic_fetch_and(&g_mouse_btn, ~1u); continue; }
                    if (et == NSEventTypeRightMouseDown) { atomic_fetch_or(&g_mouse_btn, 2u); continue; }
                    if (et == NSEventTypeRightMouseUp)   { atomic_fetch_and(&g_mouse_btn, ~2u); continue; }
                    if (et == NSEventTypeScrollWheel){
                        atomic_fetch_add(&g_mouse_dz, (int)([ev deltaY] * 120.0));
                        continue;
                    }
                    if (et == NSEventTypeFlagsChanged){
                        NSEventModifierFlags f = [ev modifierFlags];
                        atomic_store(&g_keys[0x2A], (f & NSEventModifierFlagShift)   ? 1 : 0);
                        atomic_store(&g_keys[0x1D], (f & NSEventModifierFlagControl) ? 1 : 0);
                        atomic_store(&g_keys[0x38], (f & NSEventModifierFlagOption)  ? 1 : 0);
                        continue;
                    }
                    [NSApp sendEvent:ev];
                }

                /* Also process run-loop sources outside the event queue.
                 * AppleEvents (Dock > Quit, the "quit" command, logout) come
                 * from here; if only nextEventMatching... is called the app
                 * never responds to Quit. */
                [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                         beforeDate:[NSDate distantPast]];

                /* AUTOKEY=<vk>[,<vk>...]: periodic synthetic key presses to measure whether
                 * the game reacts to input. */
                /* AUTOCLICK=1: periodic synthetic left click + small mouse move.
                 * To measure whether input reaches the game. */
                if (ENVC("AUTOCLICK")){
                    long f = atomic_load(&g_frames_drawn);
                    long ph = f % 120;
                    if (ph == 0){ atomic_fetch_add(&g_mouse_dx, 3);
                                  atomic_fetch_add(&g_mouse_dy, 2); }
                    if (ph == 60) atomic_fetch_or(&g_mouse_btn, 1u);
                    if (ph == 70) atomic_fetch_and(&g_mouse_btn, ~1u);
                }
                /* AUTOSEQ="<frame>:<vk>,<frame>:<vk>,...": press the key once at the given
                 * frame (for 10 frames).  AUTOKEY presses continuously; a scriptable
                 * sequence was needed to walk the menus and reach a specific scene. */
                /* AUTOSEQ="<frame>:<vk>[:<len>],...": press the key from the frame for <len>
                 * frames (default 8).  The length was needed: without driving the car
                 * we can't see the headlights hit the surface. */
                autotext_tick(atomic_load(&g_frames_drawn));
                { enum { SQMAX = 64 };
                  static int sqinit = 0; static long sqf[SQMAX], sqd[SQMAX];
                  static unsigned char sqk[SQMAX]; static int sqn = 0;
                  if (!sqinit){ sqinit = 1;
                    const char *e = ENVC("AUTOSEQ");
                    const char *q = e;
                    for (; q && *q && sqn < SQMAX; ){
                        while (*q==',' || *q==' ') q++;
                        if (!*q) break;
                        sqf[sqn] = strtol(q, 0, 0);
                        const char *c2 = strchr(q, ':');
                        if (!c2) break;
                        sqk[sqn] = (unsigned char)strtoul(c2+1, 0, 0);
                        const char *c3 = strchr(c2+1, ':');
                        const char *comma = strchr(q, ',');
                        sqd[sqn] = (c3 && (!comma || c3 < comma)) ? strtol(c3+1, 0, 0) : 8;
                        if (sqd[sqn] < 1) sqd[sqn] = 8;
                        sqn++;
                        while (*q && *q != ',') q++;
                    }
                    /* if the limit is exceeded it must not be cut off SILENTLY (in one test the
                     * trailing Enters were lost and the game was thought to have "hung") */
                    while (q && (*q == ',' || *q == ' ')) q++;
                    if (q && *q) fprintf(stderr, "app: WARNING AUTOSEQ cut off at %d entries, the rest ignored\n", SQMAX);
                  }
                  if (sqn){
                    long f = atomic_load(&g_frames_drawn);
                    for (int q = 0; q < sqn; q++){
                        int on = (f >= sqf[q] && f < sqf[q] + sqd[q]);
                        if (on || f == sqf[q] + sqd[q])
                            atomic_store(&g_keys[sqk[q]], on ? 1 : 0);
                    }
                  }
                }
                { static int akinit = 0; static unsigned char akk[64]; static int akn = 0;
                  if (!akinit){ akinit = 1;
                    const char *e = ENVC("AUTOKEY");
                    for (const char *q = e; q && *q && akn < 64; ){
                        while (*q==',' || *q==' ') q++;
                        if (!*q) break;
                        akk[akn++] = (unsigned char)strtoul(q, 0, 0);
                        while (*q && *q != ',') q++;
                    }
                  }
                  /* AUTOKEYSTOP=<frame>: stop pressing after this frame.
                   * Since AUTOKEY pressed Enter continuously the pre-race CUTSCENE
                   * was skipped; this was needed to see it. */
                  { static long akstop = -2;
                    if (akstop == -2){ const char *e = ENVC("AUTOKEYSTOP");
                        akstop = (e && *e) ? strtol(e, 0, 0) : -1; }
                    if (akn){
                      long f = atomic_load(&g_frames_drawn);
                      if (akstop >= 0 && f > akstop){
                          for (int q = 0; q < akn; q++) atomic_store(&g_keys[akk[q]], 0);
                      } else {
                          int slot = (int)((f / 30) % akn);
                          int on = (f % 30) < 10;
                          for (int q = 0; q < akn; q++)
                              atomic_store(&g_keys[akk[q]], (on && q == slot) ? 1 : 0);
                      }
                    } }
                }
                int have_new;
                int noshow = 0;
                pthread_mutex_lock(&g_qlock); have_new = (g_ready >= 0);
                if (have_new) noshow = g_noshow[g_ready];
                pthread_mutex_unlock(&g_qlock);
                if (!have_new && atomic_load(&g_frame_pending)) g_skip_empty++;
                if (have_new && atomic_exchange(&g_frame_pending, 0)){
                    struct timespec _d0,_d1; clock_gettime(CLOCK_MONOTONIC,&_d0);
                    /* NODRAW=1 (MEASUREMENT only): 3 rotating private textures instead of the
                     * screen, no present -> no compositor pace (120 Hz), frame rate = GPU/CPU
                     * capacity.  displaySyncEnabled=NO has no effect in an invisible window. */
                    id<CAMetalDrawable> d = nil; id<MTLTexture> dtex = nil;
                    { static int nd = -1; if (nd < 0) nd = ENVC("NODRAW") ? 1 : 0;
                      if (nd){ static id<MTLTexture> ot[3]; static unsigned oi;
                          CGSize ds = g_layer.drawableSize; unsigned k3 = (oi++) % 3u;
                          if (!ot[k3] || ot[k3].width != (NSUInteger)ds.width || ot[k3].height != (NSUInteger)ds.height){
                              MTLTextureDescriptor *od = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:g_layer.pixelFormat
                                  width:(NSUInteger)ds.width height:(NSUInteger)ds.height mipmapped:NO];
                              od.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; od.storageMode = MTLStorageModePrivate;
                              ot[k3] = [g_dev newTextureWithDescriptor:od]; }
                          dtex = ot[k3]; }
                      else if (noshow && g_flush_tid && g_flush_tid < MAXTEX){
                          /* an off-screen flush: the screen goes into a texture the next frame copies back */
                          CGSize ds = g_layer.drawableSize;
                          id<MTLTexture> ft = g_tex[g_flush_tid];
                          if (!ft || ft.width != (NSUInteger)ds.width || ft.height != (NSUInteger)ds.height ||
                              ft.pixelFormat != g_layer.pixelFormat){
                              MTLTextureDescriptor *od = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:g_layer.pixelFormat
                                  width:(NSUInteger)ds.width height:(NSUInteger)ds.height mipmapped:NO];
                              od.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; od.storageMode = MTLStorageModePrivate;
                              ft = [g_dev newTextureWithDescriptor:od]; g_tex[g_flush_tid] = ft; }
                          dtex = ft; }
                      else { d = [g_layer nextDrawable]; dtex = d.texture;
                             if (d && fbonly_on()) dtex = fbonly_mid(d.texture); } }
                    clock_gettime(CLOCK_MONOTONIC,&_d1);
                    atomic_fetch_add(&g_wait_drawable_ns,
                      (unsigned long long)(_d1.tv_sec-_d0.tv_sec)*1000000000ull
                      + (unsigned long long)_d1.tv_nsec - (unsigned long long)_d0.tv_nsec);
                    if (dtex){
                        struct timespec _e0; clock_gettime(CLOCK_MONOTONIC,&_e0);
                        id<MTLCommandBuffer> cb = [g_queue commandBuffer];
                        pp_begin(atomic_load(&g_frames_drawn) + 1);
                        int ab_count = 0;
                        g_ab_mode = ab_frame(atomic_load(&g_frames_drawn) + 1, &ab_count);
                        int ab_mode = g_ab_mode;
                        int abt_cnt = 0, abt_mode = abt_frame(atomic_load(&g_frames_drawn) + 1, &abt_cnt);
                        /* Don't overwrite a buffer the GPU may still be reading:
                         * take an empty set for this frame (wait without holding the lock). */
                        int fbi = fb_acquire();
                        pthread_mutex_lock(&g_qlock);
                        int rb = g_ready;
                        g_enc_rb = rb;
                        /* Release the lock HERE.  It used to be held for the whole frame
                         * encoding (0.4-2 ms), and then through the vertex copy too;
                         * since metal_draw_raw asks for the same lock per draw, the
                         * guest thread blocked on every draw in that window -- the
                         * whole point of double buffering was exactly this overlap.
                         * Safe: from the moment g_enc_rb is set, rb belongs only to
                         * the main thread; the guest always writes into the other
                         * one (g_cur) and metal_present doesn't swap while g_enc_rb
                         * >= 0 (even on timeout). */
                        pthread_mutex_unlock(&g_qlock);
                        if (rb >= 0){
                            g_fb_vb[fbi]  = fb_fit(g_fb_vb[fbi],  sizeof(MVert)*(NSUInteger)g_vn[rb], sizeof(MVert)*MAXV);
                            g_fb_rvb[fbi] = fb_fit(g_fb_rvb[fbi], g_rvn[rb], MAXRV);
                            g_fb_kb[fbi]  = fb_fit(g_fb_kb[fbi],  g_kn[rb],  MAXKB);
                            if (g_upn[rb]){ g_fb_up[fbi] = fb_fit(g_fb_up[fbi], g_upn[rb], MAXUP);
                                            fb_put(g_fb_up[fbi], g_upbuf[rb], g_upn[rb]); }
                        }
                        g_mtlvb = g_fb_vb[fbi]; g_mtlrvb = g_fb_rvb[fbi]; g_mtlkb = g_fb_kb[fbi];
                        if (rb >= 0){
                            fb_put(g_mtlvb,  g_vbuf[rb],  sizeof(MVert)*(NSUInteger)g_vn[rb]);
                            fb_put(g_mtlrvb, g_rvbuf[rb], g_rvn[rb]);
                            fb_put(g_mtlkb,  g_kbuf[rb],  g_kn[rb]);
                        }

#if NFS_ENHANCED
                        hd_msaa_frame(dtex.width, dtex.height);   /* see MSAA BY RESOLUTION */
#endif
                        /* Draws are sorted by target; a new render pass is opened
                         * when the target changes. */
                        id<MTLRenderCommandEncoder> enc = nil;
                        unsigned cur_t = 0xFFFFFFFFu;
                        unsigned cur_z = 0xFFFFFFFFu;   /* the pass's Z surface */
                        /* --- encoder state cache ---------------------
                         * ~1350 draws per frame; on each of them 4 samplers, the
                         * 532-byte constant buffer and 4 textures were bound
                         * UNCONDITIONALLY.  Rebinding what didn't change isn't free in Metal. */
                        id<MTLRenderPipelineState> c_pso = nil;
                        unsigned c_smp[4] = {~0u,~0u,~0u,~0u};
                        unsigned c_tex[4] = {~0u,~0u,~0u,~0u};
                        unsigned c_bcol = ~0u;
                        unsigned c_cull = ~0u;
                        float c_uvb[3] = {0.f, 0.f, 0.f};    /* UV difference (.xy) + HUD shift (.z) bound in VS buffer 3 */
                        unsigned c_depth = ~0u;
                        int c_zon = 0;                  /* the pass has a real Z surface */
                        int c_ms = 0;                   /* (HD) the pass is multisampled */
                        int c_gb = 0;                   /* (HD) the pass carries the G-buffer */
                        int c_gbx = 0;                  /* (HD) scene pass, G deferred to a separate pass (see SEPARATE G PASS) */
                        int hd_post = 0;                /* (HD) the first copy from the screen is past: post-process stage */
                        unsigned c_att = 0;             /* (HD) resolve target bound to the pass: bit0 color, bit1 G */
                        (void)hd_post; (void)c_att;
                        if (rb >= 0) pass_reorder(rb);  /* defer the intermediate pass splitting the scene (see PASS MERGING) */
                        /* see SCREEN REDIRECTION: screen records up to scr_b are drawn into scr_T */
                        unsigned scr_T = 0; int scr_after = 0;
                        if (rb >= 0) hud_anchor_plan(rb);
                        long scr_b = rb >= 0 ? scr_redirect_plan(rb, dtex, &scr_T) : -1;
                        /* see POST-PROCESS CHAIN: fbrole[i] 1 = chain start, 2 = continuation */
                        static uint8_t fbrole[MAXD];
                        int nfb = rb >= 0 ? fb_plan(rb, fbrole, g_dn[rb]) : 0;
                        int fbv_pend = 0; unsigned fbv_s = 0, fbv_e = 0;   /* FBZINCIR=2: verification pending */
#if NFS_ENHANCED
                        int hd_scene_open = 1;          /* main scene up to the first copy from the screen */
                        int hd_gb_first = 1;            /* G-buffer for the first time this frame */
                        int hd_refl_drawn = 0;          /* the reflection variant was drawn this frame */
                        unsigned hd_scene_zid = 0;      /* the main scene's Z surface */
#endif
#if NFS_ENHANCED
                        memset(g_hd_zms_used, 0, sizeof g_hd_zms_used);
                        g_rt_early.ok = 0;
                        hd_break_plan(rb);
                        hd_mb_log(rb, atomic_load(&g_frames_drawn) + 1);
                        if (rb >= 0) hd_rt_early(cb, rb);   /* see RAY TRACING AT THE START OF THE FRAME */
#endif
                        struct { float aref, usec, p0, p1; float proj[4]; float c[PSC_N][4]; float fogv[4]; } c_fp;
                        int c_fp_valid = 0;
                        unsigned c_psci = ~0u;          /* c_fp's ps constant pool sequence */
                        unsigned char touched[MAXTEX + 1];   /* bit (texture*8 + face) */
                        memset(touched, 0, sizeof touched);
                        int screen_touched = 0;

                        long fno_pre = atomic_load(&g_frames_drawn) + 1;
                        if (ENVC("FRAMELOG") && rb >= 0 && (fno_pre % 10) == 0)
                            fprintf(stderr,"  [frame %3ld] draws=%u verts=%u\n",
                                    fno_pre, g_dn[rb], g_vn[rb]);
                        { static long yk = -2; if (yk == -2){ const char *e = ENVC("YAKALAKARE"); yk = e ? atol(e) : -1; }
                          if (fno_pre == yk) atomic_store(&g_dump_req, 1); }   /* test of the shortcut */
                        if (atomic_exchange(&g_dump_req, 0)){
                            g_dump_req_f = fno_pre;
                            if (!g_dump_n){ const char *pl = getenv("PERFLOG"), *h = getenv("HOME");
                                if (pl && *pl) snprintf(g_dump_dir, sizeof g_dump_dir, "%s", pl);
                                else snprintf(g_dump_dir, sizeof g_dump_dir, "%s/Library/Logs/NFSU2", h ? h : "/tmp"); }
                            fprintf(stderr, "capture: frame %ld (Cmd+Shift+D) -> %s\n", fno_pre, g_dump_dir);
                        }
                        int  dumpnow = (g_dump_n || g_dump_req_f == fno_pre) && dump_wanted(fno_pre);
                        /* GLYPHDUMP: screen box + UV of every quad.
                         * Goal: decode the TEXT the game writes and read its state. */
                        if (ENVC("GLYPHDUMP") && dumpnow && rb >= 0){
                            fprintf(stderr,"  [glyph] frame %ld, %u draws\n", fno_pre, g_dn[rb]);
                            for (unsigned i = 0; i < g_dn[rb]; i++){
                                MDraw *dr = &g_dbuf[rb][i];
                                if (dr->count < 3) continue;
                                MVert *v0 = &g_vbuf[rb][dr->first];
                                float xmin=1e30f,xmax=-1e30f,ymin=1e30f,ymax=-1e30f;
                                float umin=1e30f,vmin=1e30f,umax=-1e30f,vmax=-1e30f;
                                for (unsigned q=0;q<dr->count;q++){
                                    float w2 = v0[q].w != 0.f ? v0[q].w : 1.f;
                                    float X=v0[q].x/w2, Y=v0[q].y/w2;
                                    if(X<xmin)xmin=X; if(X>xmax)xmax=X;
                                    if(Y<ymin)ymin=Y; if(Y>ymax)ymax=Y;
                                    if(v0[q].t[0][0]<umin)umin=v0[q].t[0][0];
                                    if(v0[q].t[0][0]>umax)umax=v0[q].t[0][0];
                                    if(v0[q].t[0][1]<vmin)vmin=v0[q].t[0][1];
                                    if(v0[q].t[0][1]>vmax)vmax=v0[q].t[0][1];
                                }
                                fprintf(stderr,"    G %u %.5f %.5f %.5f %.5f %.6f %.6f %.6f %.6f %.3f %.3f %.3f\n",
                                        dr->tex, xmin,xmax,ymin,ymax, umin,umax,vmin,vmax,
                                        v0[0].r, v0[0].g, v0[0].b);
                            }
                        }
                        if (dumpnow && rb >= 0){
                            fprintf(stderr,"  [frame %ld] %u draws, %u verts\n",
                                    fno_pre, g_dn[rb], g_vn[rb]);
                            { unsigned hist[16]; memset(hist,0,sizeof hist);
                              unsigned other=0;
                              for (unsigned q=0;q<g_dn[rb];q++){
                                  unsigned tg=g_dbuf[rb][q].target;
                                  if (tg<16) hist[tg]++; else other++; }
                              fprintf(stderr,"    target distribution:");
                              for (unsigned q=0;q<16;q++) if (hist[q])
                                  fprintf(stderr," %s=%u", q?"":"screen", hist[q]),
                                  (q? fprintf(stderr,"(tex%u)",q):0);
                              if (other) fprintf(stderr," diger=%u", other);
                              fprintf(stderr,"\n"); }
                            for (unsigned i = 0; i < g_dn[rb] && i < 200; i++){
                                MDraw *dr = &g_dbuf[rb][i];
                                fprintf(stderr,"    #%u target=%u tex=%u verts=%u blend=%05x ps=%d smp=%04x",
                                        i, dr->target, dr->tex, dr->count, dr->blend, dr->ps, dr->smp[0]);
                                if (dr->count >= 3){
                                    MVert *v0 = &g_vbuf[rb][dr->first];
                                    /* the quad's bounds + color: to understand why it isn't visible */
                                    float xmin=1e30f,xmax=-1e30f,ymin=1e30f,ymax=-1e30f;
                                    for (unsigned q=0;q<dr->count;q++){
                                        float w2 = v0[q].w != 0.f ? v0[q].w : 1.f;
                                        float X = v0[q].x/w2, Y = v0[q].y/w2;
                                        if(X<xmin)xmin=X; if(X>xmax)xmax=X;
                                        if(Y<ymin)ymin=Y; if(Y>ymax)ymax=Y;
                                    }
                                    fprintf(stderr,"  x[%.3g..%.3g] y[%.3g..%.3g]"
                                            " color=(%.2g %.2g %.2g a=%.2g) uv=(%.3g,%.3g)",
                                            xmin,xmax,ymin,ymax,
                                            v0[0].r,v0[0].g,v0[0].b,v0[0].a, v0[0].t[0][0], v0[0].t[0][1]);
                                }
                                fprintf(stderr,"\n");
                            }
                        }
                        /* SLIVER=1: find triangles that cut across the screen end to end
                         * but have near-zero area.  The 1-pixel diagonal lines on
                         * screen come from these; instead of guessing which draw
                         * call produces them we measure. */
                        /* DRAWV=<n>: print ALL vertices of draw number n.
                         * Whether there is a seam on the edge two triangles share can
                         * only be told by seeing the raw values. */
                        /* DRAWSUM: aggregate the frame's draws by the (target,texture,ps,blend)
                         * key.  Show the structure at a glance instead of thousands
                         * of lines. */
                        if (rb >= 0 && (ENVC("DRAWSUM") || g_dump_req_f == fno_pre) && dump_wanted(fno_pre)){
                            typedef struct { unsigned tg,tx,t1,t2,bl,sm; int ps;
                                             unsigned nd,nv; float x0,x1,y0,y1; } Grp;
                            static Grp gp[192]; int gn = 0;
                            for (unsigned i = 0; i < g_dn[rb]; i++){
                                MDraw *dr = &g_dbuf[rb][i];
                                int k = -1;
                                for (int q=0;q<gn;q++)
                                    if (gp[q].tg==dr->target && gp[q].tx==dr->tex &&
                                        gp[q].t1==dr->tex1 && gp[q].t2==dr->tex2 &&
                                        gp[q].bl==dr->blend && gp[q].ps==dr->ps &&
                                        gp[q].sm==dr->smp[0]) { k=q; break; }
                                if (k < 0){ if (gn >= 192) continue; k = gn++;
                                    gp[k].tg=dr->target; gp[k].tx=dr->tex; gp[k].bl=dr->blend;
                                    gp[k].t1=dr->tex1; gp[k].t2=dr->tex2;
                                    gp[k].ps=dr->ps; gp[k].sm=dr->smp[0];
                                    gp[k].nd=gp[k].nv=0;
                                    gp[k].x0=gp[k].y0=1e30f; gp[k].x1=gp[k].y1=-1e30f; }
                                gp[k].nd++; gp[k].nv += dr->count;
                                if (dr->vsid <= 0 && dr->count >= 3){
                                    MVert *v0 = &g_vbuf[rb][dr->first];
                                    for (unsigned q=0;q<dr->count;q++){
                                        float w2 = v0[q].w != 0.f ? v0[q].w : 1.f;
                                        float X=v0[q].x/w2, Y=v0[q].y/w2;
                                        if(X<gp[k].x0)gp[k].x0=X; if(X>gp[k].x1)gp[k].x1=X;
                                        if(Y<gp[k].y0)gp[k].y0=Y; if(Y>gp[k].y1)gp[k].y1=Y;
                                    }
                                }
                            }
                            fprintf(stderr,"  [summary] frame %ld: %u draws, %d groups\n",
                                    fno_pre, g_dn[rb], gn);
                            for (int pass=0; pass<gn && pass<192; pass++){
                                int best=-1; unsigned bv=0;
                                for (int q=0;q<gn;q++) if (gp[q].nv>bv){ bv=gp[q].nv; best=q; }
                                if (best<0) break;
                                Grp *g = &gp[best];
                                { unsigned tw=0,th=0,dw=0,dh=0;
                                  if (g->tx && g->tx<MAXTEX && g_tex[g->tx]){
                                      tw=(unsigned)g_tex[g->tx].width; th=(unsigned)g_tex[g->tx].height; }
                                  if (g->tg && g->tg<MAXTEX && g_tex[g->tg]){
                                      dw=(unsigned)g_tex[g->tg].width; dh=(unsigned)g_tex[g->tg].height; }
                                  unsigned aw=0,ah=0;
                                  if (g->t1 && g->t1<MAXTEX && g_tex[g->t1]){
                                      aw=(unsigned)g_tex[g->t1].width; ah=(unsigned)g_tex[g->t1].height; }
                                  fprintf(stderr,"    target=%u(%ux%u) tex=%u(%ux%u) tex1=%u(%ux%u) tex2=%u"
                                        " ps=%d blend=%05x smp=%04x  %u draws %u verts\n",
                                        g->tg,dw,dh,g->tx,tw,th,g->t1,aw,ah,g->t2,
                                        g->ps,g->bl,g->sm,g->nd,g->nv); }
                                g->nv = 0;
                            }
                        }
                        /* DRAWLIST=1 (with DUMPFRAME): the full state of every draw +
                         * constant hashes; to find the first differing draw between two runs. */
                        if (rb >= 0 && (ENVC("DRAWLIST") || g_dump_req_f == fno_pre) && dump_wanted(fno_pre)){
                            for (unsigned di = 0; di < g_dn[rb]; di++){
                                MDraw *dr = &g_dbuf[rb][di];
                                uint64_t hp = 1469598103934665603ull, hk = 1469598103934665603ull;
                                const uint8_t *pp = (const uint8_t *)g_pscb[rb][dr->psci];
                                for (size_t q = 0; q < sizeof g_pscb[rb][0]; q++){ hp ^= pp[q]; hp *= 1099511628211ull; }
                                if (dr->vsid > 0 && g_kbuf[rb]){
                                    const uint8_t *kp = (const uint8_t *)g_kbuf[rb] + dr->koff;
                                    for (unsigned q = 0; q < dr->ksz && dr->koff + q < MAXKB; q++){ hk ^= kp[q]; hk *= 1099511628211ull; }
                                }
                                if (dr->covok)
                                    fprintf(stderr,"  [dl] %4u coverage NDC x %.7f..%.7f y %.7f..%.7f  uv%s u %.7f..%.7f v %.7f..%.7f\n",
                                            di, dr->cov[0], dr->cov[1], dr->cov[2], dr->cov[3], dr->cuvok ? "" : "(none)",
                                            dr->cuv[0], dr->cuv[1], dr->cuv[2], dr->cuv[3]);
                                fprintf(stderr,"  [dl] %4u t=%x z=%x cull=%u bl=%05x ps=%d vs=%d tx=%u,%u,%u,%u n=%u clr=%u blit=%u bs=%u cov=%u psc=%016llx vk=%016llx\n",
                                        di, dr->target, dr->depth, dr->cull, dr->blend, dr->ps, dr->vsid,
                                        dr->tex, dr->tex1, dr->tex2, dr->tex3, dr->count, dr->is_clear, dr->is_blit,
                                        dr->blit_src, dr->covok,
                                        (unsigned long long)hp, (unsigned long long)hk);
                            }
                        }
                        if (rb >= 0 && ENVC("DRAWV") && dump_wanted(fno_pre)){
                            unsigned di = (unsigned)atoi(ENVC("DRAWV"));
                            if (di < g_dn[rb]){
                                MDraw *dr = &g_dbuf[rb][di];
                                fprintf(stderr,"  [vert] draw#%u vsid=%d verts=%u tex=%u\n",
                                        di, dr->vsid, dr->count, dr->tex);
                                if (dr->vsid <= 0){
                                    MVert *v0 = &g_vbuf[rb][dr->first];
                                    for (unsigned q = 0; q < dr->count && q < 12; q++)
                                        fprintf(stderr,"    v%u  pos=(%.9g, %.9g, %.9g, %.9g)"
                                                "  uv0=(%.9g, %.9g)\n", q,
                                                v0[q].x, v0[q].y, v0[q].z, v0[q].w,
                                                v0[q].t[0][0], v0[q].t[0][1]);
                                }
                            }
                        }
#if NFS_ENHANCED
                        if (rb >= 0 && ENVC("HD_RTPROBE") && dump_wanted(fno_pre))
                            hd_rt_probe(rb, dtex, fno_pre);
                        g_hd_dump_now = dump_wanted(fno_pre);
                        /* CAMLOG=1 (with DUMPFRAME): the frame's camera + the vertex constants of the
                         * first few draws drawing world geometry. */
                        if (rb >= 0 && ENVC("CAMLOG") && dump_wanted(fno_pre)){
                            fprintf(stderr, "  [camera] frame %ld valid=%d\n", fno_pre, g_hd_cam_ok[rb]);
                            static const char *mn[3] = { "view", "projection", "product" };
                            for (int m = 0; m < 3; m++){
                                fprintf(stderr, "    %s\n", mn[m]);
                                for (int r = 0; r < 4; r++)
                                    fprintf(stderr, "      %11.5f %11.5f %11.5f %11.5f\n",
                                            g_hd_cam[rb][m*16+r*4], g_hd_cam[rb][m*16+r*4+1],
                                            g_hd_cam[rb][m*16+r*4+2], g_hd_cam[rb][m*16+r*4+3]); }
                            int shown = 0;
                            for (unsigned di = 0; di < g_dn[rb] && shown < 4; di++){
                                MDraw *dr = &g_dbuf[rb][di];
                                if (dr->target || dr->vsid <= 0 || dr->is_clear || dr->is_blit || dr->count < 30) continue;
                                const float *k = (const float *)(g_kbuf[rb] + dr->koff + 16);
                                unsigned nk = (dr->ksz - 16u) / 16u; if (nk > 16) nk = 16;
                                fprintf(stderr, "    draw#%u ps=%d vs=%d verts=%u constants c0..c%u:\n",
                                        di, dr->ps, dr->vsid, dr->count, nk ? nk-1 : 0);
                                for (unsigned q = 0; q < nk; q++)
                                    fprintf(stderr, "      c%-2u %11.5f %11.5f %11.5f %11.5f\n", q,
                                            k[q*4], k[q*4+1], k[q*4+2], k[q*4+3]);
                                shown++;
                            }
                        }
#endif
                        /* DRAWVPS=<ps>: dump the vertices of ALL draws using that shader, together with
                         * the four texture coordinate sets. */
                        if (rb >= 0 && ENVC("DRAWVPS") && dump_wanted(fno_pre)){
                            int want = atoi(ENVC("DRAWVPS"));
                            for (unsigned di = 0; di < g_dn[rb]; di++){
                                MDraw *dr = &g_dbuf[rb][di];
                                if (dr->ps != want) continue;
                                fprintf(stderr,"  [vert-ps] draw#%u ps=%d vsid=%d verts=%u target=%u tex=%u,%u,%u,%u\n",
                                        di, dr->ps, dr->vsid, dr->count, dr->target,
                                        dr->tex, dr->tex1, dr->tex2, dr->tex3);
                                if (dr->vsid > 0) continue;
                                MVert *v0 = &g_vbuf[rb][dr->first];
                                for (unsigned q = 0; q < dr->count && q < 6; q++)
                                    fprintf(stderr,"    v%u pos=(%.5g,%.5g,%.5g,%.5g) t0=(%.6g,%.6g) t1=(%.6g,%.6g)"
                                            " t2=(%.6g,%.6g) t3=(%.6g,%.6g)\n", q,
                                            v0[q].x, v0[q].y, v0[q].z, v0[q].w,
                                            v0[q].t[0][0], v0[q].t[0][1], v0[q].t[1][0], v0[q].t[1][1],
                                            v0[q].t[2][0], v0[q].t[2][1], v0[q].t[3][0], v0[q].t[3][1]);
                            }
                        }
                        if (rb >= 0 && ENVC("PSHIST") && dump_wanted(fno_pre)){
                            /* Per-ps draw/triangle count within the frame.  The DRAWSUM
                             * group table fills up at 192 and the LAST passes
                             * weren't listed; this histogram covers all of them. */
                            struct { int ps; unsigned nd, nv, bl; } h[128]; int hn = 0;
                            for (unsigned i = 0; i < g_dn[rb]; i++){
                                MDraw *dr = &g_dbuf[rb][i];
                                int k = -1;
                                for (int q = 0; q < hn; q++) if (h[q].ps == dr->ps){ k = q; break; }
                                if (k < 0){ if (hn >= 128) continue; k = hn++;
                                            h[k].ps = dr->ps; h[k].nd = h[k].nv = 0;
                                            h[k].bl = dr->blend; }
                                h[k].nd++; h[k].nv += dr->count;
                            }
                            fprintf(stderr, "  [pshist] frame %ld: %u draws, %d distinct ps\n",
                                    fno_pre, g_dn[rb], hn);
                            for (int q = 0; q < hn; q++)
                                fprintf(stderr, "     ps=%-4d %5u draws %7u verts  blend=%05x\n",
                                        h[q].ps, h[q].nd, h[q].nv, h[q].bl);
                        }
                        if (rb >= 0 && ENVC("SLIVER") && dump_wanted(fno_pre)){
                            int nrep = 0;
                            for (unsigned i = 0; i < g_dn[rb] && nrep < 24; i++){
                                MDraw *dr = &g_dbuf[rb][i];
                                if (dr->vsid > 0 || dr->count < 3) continue;
                                MVert *v0 = &g_vbuf[rb][dr->first];
                                for (unsigned t = 0; t + 2 < dr->count && nrep < 24; t += 3){
                                    float X[3], Y[3];
                                    for (int q = 0; q < 3; q++){
                                        float w2 = v0[t+q].w != 0.f ? v0[t+q].w : 1.f;
                                        X[q] = v0[t+q].x / w2; Y[q] = v0[t+q].y / w2;
                                    }
                                    float ar = 0.5f * fabsf((X[1]-X[0])*(Y[2]-Y[0]) -
                                                            (X[2]-X[0])*(Y[1]-Y[0]));
                                    float sp = 0.f;
                                    for (int q = 0; q < 3; q++){
                                        int r2 = (q+1)%3;
                                        float dx = X[r2]-X[q], dy = Y[r2]-Y[q];
                                        float L = sqrtf(dx*dx+dy*dy); if (L > sp) sp = L;
                                    }
                                    if (sp > 0.5f && ar < 2e-4f){
                                        nrep++;
                                        fprintf(stderr,"  [sliver] draw#%u tri%u tex=%u"
                                                " blend=%05x ps=%d verts=%u  length=%.3f area=%.2e\n"
                                                "      (%.4f,%.4f) (%.4f,%.4f) (%.4f,%.4f)"
                                                "  color=(%.2f %.2f %.2f a=%.2f)\n",
                                                i, t/3, dr->tex, dr->blend, dr->ps, dr->count,
                                                sp, ar, X[0],Y[0], X[1],Y[1], X[2],Y[2],
                                                v0[t].r, v0[t].g, v0[t].b, v0[t].a);
                                    }
                                }
                            }
                            if (!nrep) fprintf(stderr,"  [sliver] frame %ld: none found\n", fno_pre);
                        }
                        if (rb >= 0 && g_pso){
                            for (unsigned i = 0; i <= g_dn[rb]; i++){
                                /* FBZINCIR=2: the chain drawn on the old path is done -> draw the merged path
                                 * into scratch textures with the same input and compare */
                                if (fbv_pend && i > fbv_e){
#if NFS_ENHANCED
                                    if (enc){ int pe = hd_ms_end(enc, rb, i, c_ms, c_gb, c_att);
                                              [enc endEncoding]; enc = nil;
                                              if (pe) hd_ms_fallback(cb, dtex, pe); }
#endif
                                    if (enc){ [enc endEncoding]; enc = nil; }
                                    cur_t = 0xFFFFFFFFu; cur_z = 0xFFFFFFFFu;
                                    const MDraw *d0 = &g_dbuf[rb][fbv_s];
                                    id<MTLTexture> TA = g_tex[TGT_ID(d0->target)], TB = g_tex[d0->tex];
                                    id<MTLTexture> FA = fbv_tex(1, TA), FB = fbv_tex(2, TB), IN = fbv_tex(0, TB);
                                    id<MTLBlitCommandEncoder> bc = [cb blitCommandEncoder];
                                    [bc copyFromTexture:IN toTexture:FB]; [bc endEncoding];
                                    const char *fc = ENVC("FBZCTRL");
                                    if (fc && *fc == '2'){
                                        /* diagnostic: run the control path twice, compare it with itself */
                                        id<MTLTexture> FA2 = fbv_tex(3, TA), FB2 = fbv_tex(4, TB);
                                        id<MTLBlitCommandEncoder> b2 = [cb blitCommandEncoder];
                                        [b2 copyFromTexture:IN toTexture:FB2]; [b2 endEncoding];
                                        fb_encode_ctrl(cb, rb, fbv_s, fbv_e, FA, FB);
                                        fb_encode_ctrl(cb, rb, fbv_s, fbv_e, FA2, FB2);
                                        fbv_compare(cb, FA, FA2, 0); fbv_compare(cb, FB, FB2, 1);
                                    } else {
                                        if (fc) fb_encode_ctrl(cb, rb, fbv_s, fbv_e, FA, FB);
                                        else    fb_encode(cb, rb, fbv_s, fbv_e, FA, FB);
                                        fbv_compare(cb, FA, TA, 0); fbv_compare(cb, FB, TB, 1);
                                    }
                                    atomic_fetch_add(&g_fbz_val, 1ull); fbv_pend = 0;
                                }
                                if (i == g_dn[rb]) break;
                                MDraw *dr = &g_dbuf[rb][i];
                                /* see ORDERED TEXTURE UPLOADS: buffer -> texture copy in order with the draws */
                                if (dr->is_up){
#if NFS_ENHANCED
                                    if (enc){ int pe = hd_ms_end(enc, rb, i, c_ms, c_gb, c_att);
                                              [enc endEncoding]; enc = nil;
                                              if (pe) hd_ms_fallback(cb, dtex, pe); }
#endif
                                    if (enc){ [enc endEncoding]; enc = nil; }
                                    cur_t = 0xFFFFFFFFu; cur_z = 0xFFFFFFFFu;
                                    if (dr->tex && dr->tex < MAXTEX && g_tex[dr->tex] && g_fb_up[fbi] &&
                                        dr->rawoff + dr->up_pitch <= g_fb_up[fbi].length){
                                        id<MTLBlitCommandEncoder> bc = pp_blit(cb, "texture upload (ordered)");
                                        [bc copyFromBuffer:g_fb_up[fbi] sourceOffset:dr->rawoff
                                            sourceBytesPerRow:dr->up_pitch sourceBytesPerImage:0
                                                   sourceSize:MTLSizeMake(dr->up_w, dr->up_h, 1)
                                                    toTexture:g_tex[dr->tex] destinationSlice:dr->up_face
                                             destinationLevel:dr->up_level destinationOrigin:MTLOriginMake(0, 0, 0)];
                                        [bc endEncoding];
                                    }
                                    continue;
                                }
                                if (nfb && fbrole[i] == 1){
                                    unsigned e = i; while (e + 1 < g_dn[rb] && fbrole[e + 1] == 2) e++;
#if NFS_ENHANCED
                                    if (enc){ int pe = hd_ms_end(enc, rb, i, c_ms, c_gb, c_att);
                                              [enc endEncoding]; enc = nil;
                                              if (pe) hd_ms_fallback(cb, dtex, pe); }
#endif
                                    if (enc){ [enc endEncoding]; enc = nil; }
                                    cur_t = 0xFFFFFFFFu; cur_z = 0xFFFFFFFFu;
                                    id<MTLTexture> TA = g_tex[TGT_ID(dr->target)], TB = g_tex[dr->tex];
                                    if (fbz_mode() == 2){
                                        /* save the input; this time the chain is drawn on the old path */
                                        id<MTLBlitCommandEncoder> bc = [cb blitCommandEncoder];
                                        [bc copyFromTexture:TB toTexture:fbv_tex(0, TB)]; [bc endEncoding];
                                        fbv_pend = 1; fbv_s = i; fbv_e = e;
                                    } else {
                                        fb_encode(cb, rb, i, e, TA, TB);
                                        { unsigned ta = TGT_ID(dr->target) * 8u, tb = dr->tex * 8u;
                                          touched[ta >> 3] |= 1u << (ta & 7); touched[tb >> 3] |= 1u << (tb & 7); }
                                        atomic_fetch_add(&g_fbz_n, 1ull);
                                        atomic_fetch_add(&g_fbz_rec, (unsigned long long)(e - i + 1));
                                        i = e; continue;
                                    }
                                }
                                /* SKIPDRAW=<n>[,<n>...] or <a>-<b>: skip these draw indices
                                 * (to see an intermediate target's content in the dump frame). */
                                { static int skinit = 0; static int sklo[16], skhi[16]; static int skn = 0;
                                  if (!skinit){ skinit = 1;
                                    for (const char *q = ENVC("SKIPDRAW"); q && *q && skn < 16; ){
                                        while (*q == ',') q++;
                                        if (!*q) break;
                                        sklo[skn] = atoi(q); skhi[skn] = sklo[skn];
                                        while (*q && *q != ',' && *q != '-') q++;
                                        if (*q == '-'){ q++; skhi[skn] = atoi(q); }
                                        skn++;
                                        while (*q && *q != ',') q++; } }
                                  int skip = 0;
                                  for (int q = 0; q < skn; q++) if ((int)i >= sklo[q] && (int)i <= skhi[q]) skip = 1;
                                  if (skip) continue; }
                                /* SKIPPS=<n>[,<n>...]: skip ALL draws using this pixel shader.
                                 * The most direct way to isolate a pass's contribution.
                                 * With ABTEST=SKIPPS it skips only in the OFF slices: those
                                 * draws' GPU energy / memory cost is measured in the same
                                 * run (the image breaks). */
                                { static int spinit = 0; static int sps[16]; static int spn = 0;
                                  if (!spinit){ spinit = 1;
                                    const char *e = ENVC("SKIPPS");
                                    for (const char *q = e; q && *q && spn < 16; ){
                                        while (*q==',' || *q==' ') q++;
                                        if (!*q) break;
                                        sps[spn++] = atoi(q);
                                        while (*q && *q != ',') q++; } }
                                  int skip = 0;
                                  for (int q = 0; q < spn; q++) if (sps[q] == dr->ps) skip = 1;
                                  if (skip && g_abt_name && !strcmp(g_abt_name, "SKIPPS") && !abt_off("SKIPPS")) skip = 0;
                                  if (skip) continue; }
                                if (dr->is_blit && (long)i == scr_b){
                                    /* SCREEN REDIRECTION: the scene is already in T; the screen starts empty from here */
                                    if (enc){ [enc endEncoding]; enc = nil; cur_t = 0xFFFFFFFFu; cur_z = 0xFFFFFFFFu; }
                                    { unsigned tk_ = scr_T * 8u; touched[tk_ >> 3] |= 1u << (tk_ & 7); }
                                    screen_touched = 0; scr_after = 1;
                                    atomic_fetch_add(&g_scr_redir, 1ull);
                                    continue;
                                }
                                if (dr->is_blit){
#if NFS_ENHANCED
                                    /* valid or not: hd_ms_end may have seen this copy and not stored the samples */
                                    if (!dr->blit_src && hd_mslazy() && !g_hd_brk[i]) hd_post = 1;
#endif
                                    /* Resolve the source/destination texture; screen = drawable */
                                    id<MTLTexture> st2 = dr->blit_src
                                        ? ((dr->blit_src < MAXTEX) ? g_tex[dr->blit_src] : nil)
                                        : dtex;
                                    id<MTLTexture> dt2 = dr->target
                                        ? ((TGT_ID(dr->target) < MAXTEX)
                                             ? g_tex[TGT_ID(dr->target)] : nil)
                                        : dtex;
                                    if (st2 && dt2 && st2 != dt2){
                                        if (ENVC("BLITLOG")){
                                            static unsigned seen[32]; static int sn = 0;
                                            unsigned key = (dr->blit_src<<16) ^ dr->target;
                                            int nw = 1;
                                            for (int q=0;q<sn;q++) if (seen[q]==key) nw = 0;
                                            if (nw && sn < 32){ seen[sn++] = key;
                                                fprintf(stderr,"  [blit] tex %u (%lux%lu) -> tex %u (%lux%lu)  scale %.2fx\n",
                                                    dr->blit_src,(unsigned long)st2.width,(unsigned long)st2.height,
                                                    dr->target,(unsigned long)dt2.width,(unsigned long)dt2.height,
                                                    (double)st2.width/(double)(dt2.width?dt2.width:1)); }
                                        }
#if NFS_ENHANCED
                                        if (enc){ int pe = hd_ms_end(enc, rb, i, c_ms, c_gb, c_att);
                                                  [enc endEncoding]; enc = nil; cur_t = 0xFFFFFFFFu; cur_z = 0xFFFFFFFFu;
                                                  if (pe) hd_ms_fallback(cb, dtex, pe); }
#endif
                                        if (enc){ [enc endEncoding]; enc = nil; cur_t = 0xFFFFFFFFu; cur_z = 0xFFFFFFFFu; }
#if NFS_ENHANCED
                                        /* SSR composite: the main scene is copied from the screen for the FIRST time
                                         * (the post-process starts).  Scene + G-buffer + Z are ready. */
                                        if (st2 == dtex && hd_scene_open && !g_hd_brk[i]){
                                            hd_scene_open = 0;
                                            if (hd_gsep() && hd_refl_drawn && hd_ssr_on()) hd_gpass(cb, rb, i, hd_scene_zid);
                                            hd_composite(cb, rb, i, dtex, hd_scene_zid, hd_refl_drawn);
                                        }
                                        /* MSAA: the screen content lives in a multisampled texture.
                                         * A copy written straight to the drawable would be
                                         * OVERWRITTEN by the next screen pass's resolve; draw the
                                         * copy into the multisampled texture as a quad, resolve. */
                                        if (dt2 == dtex && hd_msaa_n() > 1 && hd_pso_ms() && !hd_post){
                                            g_hd_mscol = hd_ms_tex(g_hd_mscol, dtex.pixelFormat,
                                                                   dtex.width, dtex.height);
                                            MTLRenderPassDescriptor *mp =
                                                [MTLRenderPassDescriptor renderPassDescriptor];
                                            mp.colorAttachments[0].texture = g_hd_mscol;
                                            mp.colorAttachments[0].resolveTexture = dtex;
                                            mp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
                                            mp.colorAttachments[0].storeAction = MTLStoreActionStoreAndMultisampleResolve;
                                            pp_rp(mp, "blit -> screen (MSAA quad)");
                                            id<MTLRenderCommandEncoder> me =
                                                [cb renderCommandEncoderWithDescriptor:mp];
                                            [me setRenderPipelineState:hd_pso_ms()];
                                            [me setVertexBuffer:g_blitvb offset:0 atIndex:0];
                                            [me setFragmentSamplerState:g_samp atIndex:0];
                                            [me setFragmentTexture:st2 atIndex:0];
                                            { struct { float aref, usec, p0, p1; float proj[4]; float c[PSC_N][4]; float fogv[4]; } bf;
                                              memset(&bf, 0, sizeof bf);
                                              [me setFragmentBytes:&bf length:sizeof bf atIndex:0]; }
                                            [me drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
                                            [me endEncoding];
                                            screen_touched = 1;
                                            atomic_fetch_add(&g_hd_msblit, 1ull);
                                            continue;
                                        }
#endif
                                        /* If 1:1 and the same format use a REAL copy.
                                         * SAMPLING the drawable with a full-screen quad gave
                                         * stale content at tile (32x32) boundaries: a
                                         * rectangular mosaic in the sky and on glass
                                         * facades.  (Measured: block edges at exact
                                         * multiples of 32/64 pixels.)  copyFromTexture is
                                         * both correct and cheaper. */
                                        if (st2.width == dt2.width && st2.height == dt2.height &&
                                            st2.pixelFormat == dt2.pixelFormat && !dr->blit_rect &&
                                            !ENVC("BLITQUAD")){
                                            id<MTLBlitCommandEncoder> bc = pp_blit(cb, "blit (same-size copy)");
                                            [bc copyFromTexture:st2 sourceSlice:0 sourceLevel:0
                                                   sourceOrigin:MTLOriginMake(0,0,0)
                                                     sourceSize:MTLSizeMake(st2.width, st2.height, 1)
                                                      toTexture:dt2 destinationSlice:0
                                               destinationLevel:0
                                              destinationOrigin:MTLOriginMake(0,0,0)];
                                            [bc endEncoding];
                                            if (dr->target){ unsigned tk_=TGT_ID(dr->target)*8u+TGT_FACE(dr->target);
                                                     touched[tk_>>3] |= 1u<<(tk_&7); }
                                            else screen_touched = 1;
                                            continue;
                                        }
                                        /* Scaled blit: sampling the drawable DIRECTLY gives
                                         * stale content at tile boundaries
                                         * (32/64 px mosaic).  First take it into an
                                         * intermediate texture with a real copy, then
                                         * sample from that with scaling. */
                                        if (st2 == dtex){
                                            static id<MTLTexture> scratch;
                                            if (!scratch || scratch.width != st2.width ||
                                                scratch.height != st2.height ||
                                                scratch.pixelFormat != st2.pixelFormat){
                                                MTLTextureDescriptor *sd2 =
                                                  [MTLTextureDescriptor
                                                    texture2DDescriptorWithPixelFormat:st2.pixelFormat
                                                    width:st2.width height:st2.height mipmapped:NO];
                                                sd2.usage = MTLTextureUsageShaderRead |
                                                            MTLTextureUsageRenderTarget;
                                                sd2.storageMode = MTLStorageModePrivate;
                                                scratch = [g_dev newTextureWithDescriptor:sd2];
                                            }
                                            if (scratch){
                                                id<MTLBlitCommandEncoder> bc2 = pp_blit(cb, "blit (scratch copy)");
                                                [bc2 copyFromTexture:st2 sourceSlice:0 sourceLevel:0
                                                        sourceOrigin:MTLOriginMake(0,0,0)
                                                          sourceSize:MTLSizeMake(st2.width,st2.height,1)
                                                           toTexture:scratch destinationSlice:0
                                                    destinationLevel:0
                                                   destinationOrigin:MTLOriginMake(0,0,0)];
                                                [bc2 endEncoding];
                                                st2 = scratch;
                                            }
                                        }
                                        MTLRenderPassDescriptor *bp =
                                            [MTLRenderPassDescriptor renderPassDescriptor];
                                        bp.colorAttachments[0].texture = dt2;
                                        bp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
                                        bp.colorAttachments[0].storeAction = MTLStoreActionStore;
                                        { char pn[48]; snprintf(pn, sizeof pn, "blit quad -> %lux%lu", (unsigned long)dt2.width, (unsigned long)dt2.height);
                                          pp_rp(bp, pn); }
                                        id<MTLRenderCommandEncoder> be =
                                            [cb renderCommandEncoderWithDescriptor:bp];
                                        [be setRenderPipelineState:g_pso];
                                        if (dr->blit_rect){
                                            /* the source rectangle only: the full-screen quad with its uv remapped */
                                            MVert bq[6]; memcpy(bq, [g_blitvb contents], sizeof bq);
                                            for (int q = 0; q < 6; q++){
                                                float u = bq[q].t[0][0], v = bq[q].t[0][1];
                                                bq[q].t[0][0] = dr->blit_uv[0] + u * (dr->blit_uv[2] - dr->blit_uv[0]);
                                                bq[q].t[0][1] = dr->blit_uv[1] + v * (dr->blit_uv[3] - dr->blit_uv[1]); }
                                            [be setVertexBytes:bq length:sizeof bq atIndex:0];
                                        } else
                                        [be setVertexBuffer:g_blitvb offset:0 atIndex:0];
                                        [be setFragmentSamplerState:g_samp atIndex:0];
                                        [be setFragmentTexture:st2 atIndex:0];
                                        { struct { float aref, usec, p0, p1; float proj[4]; float c[PSC_N][4]; float fogv[4]; } bf;
                                          memset(&bf, 0, sizeof bf);
                                          [be setFragmentBytes:&bf length:sizeof bf atIndex:0]; }
                                        [be drawPrimitives:MTLPrimitiveTypeTriangle
                                               vertexStart:0 vertexCount:6];
                                        [be endEncoding];
                                        if (dr->target){ unsigned tk_=TGT_ID(dr->target)*8u+TGT_FACE(dr->target);
                                                     touched[tk_>>3] |= 1u<<(tk_&7); }
                                        else screen_touched = 1;
                                    }
                                    continue;
                                }
                                /* Clear(ZBUFFER): clear that Z surface at the start of the next pass.
                                 * If a pass with the same surface is open, close it so
                                 * the clear order is kept. */
                                if (dr->is_clear && (dr->clr_flags & 2u)){
                                    unsigned zc = DEPTH_ZID(dr->depth);
                                    if (zc && zc < MAXZ){
                                        g_zpend[zc] = 1;
                                        g_zpend_v[zc] = dr->clr_z < 0.f ? 0.f : dr->clr_z > 1.f ? 1.f : dr->clr_z;
#if NFS_ENHANCED
                                        /* the multisampled twin consumes its own clear separately */
                                        if (zc < 64u){ g_hd_zpend_ms[zc] = 1; g_hd_zpend_msv[zc] = g_zpend_v[zc]; }
#endif
#if NFS_ENHANCED
                                        if (enc && cur_z == zc){ int pe = hd_ms_end(enc, rb, i, c_ms, c_gb, c_att);
                                                  [enc endEncoding]; enc = nil; cur_t = 0xFFFFFFFFu;
                                                  if (pe) hd_ms_fallback(cb, dtex, pe); }
#endif
                                        if (enc && cur_z == zc){ [enc endEncoding]; enc = nil; cur_t = 0xFFFFFFFFu; }
                                    }
                                }
#if NFS_ENHANCED
                                /* HD blur draw: from road target to road target.
                                 * If the source's mips aren't current close the pass,
                                 * generate, reopen with the same target (Load). */
                                int hd_blur = !dr->is_clear && hd_is_road(dr->tex) &&
                                              hd_is_road(TGT_ID(dr->target));
                                if (hd_blur && g_hd_dirty[dr->tex]){
                                    if (enc){ int pe = hd_ms_end(enc, rb, i, c_ms, c_gb, c_att);
                                              [enc endEncoding]; enc = nil; cur_t = 0xFFFFFFFFu;
                                              if (pe) hd_ms_fallback(cb, dtex, pe); }
                                    id<MTLBlitCommandEncoder> mb = pp_blit(cb, "road reflection mip generation");
                                    [mb generateMipmapsForTexture:g_tex[dr->tex]];
                                    [mb endEncoding];
                                    g_hd_dirty[dr->tex] = 0;
                                    atomic_fetch_add(&g_hd_mipgen, 1ull);
                                }
#endif
                                /* see PASS MERGING: a draw not using Z doesn't split a pass because of the Z surface */
                                int zsplit = DEPTH_ZID(dr->depth) != cur_z &&
                                             !(enc && pass_merge_on() && !dr->is_clear && !(dr->depth & 3u));
                                /* a screen pass with only a Clear: don't open (the Z clear was recorded above) */
                                if (!enc || dr->target != cur_t || zsplit)
                                    if (!hd_post && !screen_touched && pass_clear_only(rb, i)) continue;
                                if (dr->target != cur_t || zsplit){
#if NFS_ENHANCED
                                    if (enc){ int pe = hd_ms_end(enc, rb, i, c_ms, c_gb, c_att);
                                              [enc endEncoding]; enc = nil;
                                              if (pe) hd_ms_fallback(cb, dtex, pe); }
#endif
                                    if (enc){ [enc endEncoding]; enc = nil; }
#if NFS_ENHANCED
                                    if (hd_is_road(TGT_ID(dr->target))) g_hd_dirty[TGT_ID(dr->target)] = 1;
#endif
                                    id<MTLTexture> tgt = dtex;
                                    if (dr->target) freed_use(TGT_ID(dr->target), 1);
                                    if (dr->target && TGT_ID(dr->target) < MAXTEX &&
                                        g_tex[TGT_ID(dr->target)])
                                        tgt = g_tex[TGT_ID(dr->target)];
                                    if (!dr->target && (long)i < scr_b) tgt = g_tex[scr_T];   /* EKRAN YONLENDIRME */
                                    unsigned tk0 = TGT_ID(dr->target)*8u + TGT_FACE(dr->target);
                                    int first_use = dr->target
                                        ? !(touched[tk0>>3] & (1u<<(tk0&7)))
                                        : !screen_touched;
                                    MTLRenderPassDescriptor *rp =
                                        [MTLRenderPassDescriptor renderPassDescriptor];
                                    rp.colorAttachments[0].texture = tgt;
                                    if (TGT_FACE(dr->target))
                                        rp.colorAttachments[0].slice = TGT_FACE(dr->target) - 1u;
                                    /* In D3D9 off-screen targets PERSIST across frames; only the game's explicit
                                     * Clear clears them.  Each target used to be cleared on its
                                     * first use in a frame: the front's glare trail (RT4 is
                                     * multiplied by the color tint every frame and added on top)
                                     * lost its feedback, the purple-blue halo never built up.
                                     * Measured (menu, frame 1650): RT4 = 0.314*RT5 (no
                                     * accumulation); bright areas +12 in blue in the Windows reference.
                                     * The screen (drawable) is new and undefined every frame: cleared.
                                     * OFFCLEAR=1 old behavior. */
                                    { static int oc = -1; if (oc < 0) oc = ENVC("OFFCLEAR") ? 1 : 0;
                                      int color_clear = dr->is_clear && (dr->clr_flags & 1u);
                                      int do_clear = first_use && (!dr->target || color_clear || oc);
                                      rp.colorAttachments[0].loadAction = do_clear ? MTLLoadActionClear : MTLLoadActionLoad;
                                      if (dr->target && color_clear && !oc)
                                          rp.colorAttachments[0].clearColor = MTLClearColorMake(
                                              ((dr->clear_argb >> 16) & 0xFFu) / 255.0, ((dr->clear_argb >> 8) & 0xFFu) / 255.0,
                                              (dr->clear_argb & 0xFFu) / 255.0, ((dr->clear_argb >> 24) & 0xFFu) / 255.0);
                                      else
                                          rp.colorAttachments[0].clearColor = MTLClearColorMake(
                                              atomic_load(&g_clear_r), atomic_load(&g_clear_g),
                                              atomic_load(&g_clear_b), 1.0);
                                      /* SCRREDIR=2: the first screen pass after the redirection is magenta (coverage test) */
                                      if (!dr->target && scr_after == 1){ scr_after = 2;
                                          if (scrredir_mode() == 2 && do_clear) rp.colorAttachments[0].clearColor = MTLClearColorMake(1, 0, 1, 1); } }
                                    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                                    c_ms = 0;
#if NFS_ENHANCED
                                    int c_ml = 0;                   /* see MEMORYLESS MSAA ATTACHMENTS */
                                    g_hd_msp.on = 0; g_hd_msp.ml = 0; g_hd_msp.dz = 0;
                                    if (!dr->target && hd_msaa_n() > 1 && !hd_post){
                                        unsigned zq = DEPTH_ZID(dr->depth);
                                        /* no attachment will be loaded (color first use, G first time,
                                         * depth will be cleared) and no sample will be stored */
                                        c_ml = hd_ms_memless() && first_use &&
                                               (hd_gb_first || hd_gsep() || !(hd_scene_open && hd_ssr_on())) &&
                                               !(zq && zq < 64u && g_hd_msz_seen[zq] && g_hd_msz_kept[zq] &&
                                                 !g_hd_zpend_ms[zq]) &&
                                               hd_ms_memless_ok(rb, i);
                                        id<MTLTexture> msc;
                                        if (c_ml) msc = g_hd_mscol_ml = hd_ms_tex_ml(g_hd_mscol_ml, tgt.pixelFormat,
                                                                                     tgt.width, tgt.height);
                                        else      msc = g_hd_mscol = hd_ms_tex(g_hd_mscol, tgt.pixelFormat,
                                                                               tgt.width, tgt.height);
                                        if (!msc) c_ml = 0;
                                        if (msc){
                                            c_ms = 1;
                                            c_att = hd_ms_predict(rb, i);
                                            if (c_ml) atomic_fetch_add(&g_hd_ml_pass, 1ull);
                                            rp.colorAttachments[0].texture = msc;
                                            if (c_att & 1u) rp.colorAttachments[0].resolveTexture = tgt;
                                            rp.colorAttachments[0].storeAction =
                                                  c_ml ? ((c_att & 1u) ? MTLStoreActionMultisampleResolve   /* memoryless: definite */
                                                                       : MTLStoreActionDontCare)
                                                : hd_mslazy() ? MTLStoreActionUnknown             /* at close: hd_ms_end */
                                                : MTLStoreActionStoreAndMultisampleResolve;
                                            atomic_fetch_add(&g_hd_mspass, 1ull);
                                        }
                                    }
                                    c_gb = 0;
                                    c_gbx = !dr->target && hd_scene_open && hd_ssr_on() && c_ms && hd_gsep();
                                    if (!dr->target && hd_scene_open && hd_ssr_on() && !c_gbx){
                                        NSUInteger ns = c_ms ? (NSUInteger)hd_msaa_n() : 1u;
                                        for (int gi = 0; gi < HD_NG; gi++){
                                            g_hd_g[gi] = hd_tex(g_hd_g[gi], hd_gfmt(gi), tgt.width, tgt.height, 1);
                                            if (ns > 1 && c_ml) g_hd_gms_ml[gi] = hd_ms_tex_ml(g_hd_gms_ml[gi], hd_gfmt(gi), tgt.width, tgt.height);
                                            else if (ns > 1) g_hd_gms[gi] = hd_tex(g_hd_gms[gi], hd_gfmt(gi), tgt.width, tgt.height, ns);
                                        }
                                        for (int gi = 0; gi < HD_NG; gi++){
                                            MTLRenderPassColorAttachmentDescriptor *ga = rp.colorAttachments[1 + gi];
                                            ga.texture = (ns > 1) ? (c_ml ? g_hd_gms_ml[gi] : g_hd_gms[gi]) : g_hd_g[gi];
                                            if (ns > 1){ if (c_att & 2u) ga.resolveTexture = g_hd_g[gi];
                                                         ga.storeAction = c_ml ? ((c_att & 2u) ? MTLStoreActionMultisampleResolve
                                                                                                : MTLStoreActionDontCare)
                                                                       : hd_mslazy() ? MTLStoreActionUnknown
                                                                       : MTLStoreActionStoreAndMultisampleResolve; }
                                            else ga.storeAction = MTLStoreActionStore;
                                            ga.loadAction = hd_gb_first ? MTLLoadActionClear : MTLLoadActionLoad;
                                            ga.clearColor = (gi == 2 && hd_gpack())     /* packed: zero direction = 512 */
                                                ? MTLClearColorMake(512.0 / 1023.0, 512.0 / 1023.0, 512.0 / 1023.0, 0)
                                                : MTLClearColorMake(0, 0, 0, 0);
                                        }
                                        hd_gb_first = 0;
                                        hd_scene_zid = DEPTH_ZID(dr->depth);
                                        c_gb = 1;
                                    }
                                    if (c_gbx){                     /* see SEPARATE G PASS: G textures single sampled */
                                        for (int gi = 0; gi < HD_NG; gi++)
                                            g_hd_g[gi] = hd_tex(g_hd_g[gi], hd_gfmt(gi), tgt.width, tgt.height, 1);
                                        hd_gb_first = 0;
                                        hd_scene_zid = DEPTH_ZID(dr->depth);
                                    }
#endif
                                    /* Z: the bound Z surface's own texture (see g_ztex).
                                     * If there is no surface or its size doesn't match the
                                     * target a scratch texture is bound and the Z test is
                                     * turned off (pipelines always expect Depth32Float). */
                                    unsigned zid = DEPTH_ZID(dr->depth);
                                    id<MTLTexture> ztex = nil;
                                    int zclear = 0; float zval = 1.f;
                                    if (zid && zid < MAXZ){
                                        if (atomic_exchange(&g_zforget[zid], 0)){ g_ztex[zid] = nil;
#if NFS_ENHANCED
                                            if (zid < 64u){ g_hd_msz[zid] = nil; g_hd_msz_seen[zid] = 0; g_hd_msz_kept[zid] = 0; }
#endif
                                        }
                                        id<MTLTexture> zt = g_ztex[zid];
                                        if (!zt || zt.width != tgt.width || zt.height != tgt.height){
                                            /* the implicit surface (1) is sized with the screen; created
                                             * surfaces with the target they are first used with.  Later
                                             * use with a different-sized target: scratch. */
                                            if (!zt || zid == 1u){
                                                MTLTextureDescriptor *zd =
                                                    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
                                                        MTLPixelFormatDepth32Float
                                                        width:tgt.width height:tgt.height mipmapped:NO];
                                                zd.usage = MTLTextureUsageRenderTarget
#if NFS_ENHANCED
                                                         | MTLTextureUsageShaderRead   /* SSR reads depth */
#endif
                                                         ;
                                                zd.storageMode = MTLStorageModePrivate;
                                                g_ztex[zid] = zt = [g_dev newTextureWithDescriptor:zd];
                                                zclear = 1;
                                            } else { zt = nil; atomic_fetch_add(&g_z_mismatch, 1ull); }
                                        }
                                        ztex = zt;
                                        if (ztex && g_zpend[zid] && !c_ms){ zclear = 1; zval = g_zpend_v[zid]; g_zpend[zid] = 0; }
                                    }
                                    if (!ztex){
                                        if (!zid) atomic_fetch_add(&g_z_null, 1ull);
                                        if (!g_depth_tex ||
                                            g_depth_tex.width  != tgt.width ||
                                            g_depth_tex.height != tgt.height){
                                            MTLTextureDescriptor *zd =
                                                [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
                                                    MTLPixelFormatDepth32Float
                                                    width:tgt.width height:tgt.height mipmapped:NO];
                                            zd.usage = MTLTextureUsageRenderTarget;
                                            zd.storageMode = MTLStorageModePrivate;
                                            g_depth_tex = [g_dev newTextureWithDescriptor:zd];
                                        }
                                    }
#if NFS_ENHANCED
                                    id<MTLTexture> zres = nil;     /* depth resolve target */
                                    if (c_ms){
                                        if (ztex && zid < 64u){
                                            zres = ztex;
                                            /* wants to load without clearing but the last pass didn't store */
                                            int lost = g_hd_msz_seen[zid] && !g_hd_msz_kept[zid] && !g_hd_zpend_ms[zid];
                                            if (c_ml){
                                                g_hd_msz_ml = hd_ms_tex_ml(g_hd_msz_ml, MTLPixelFormatDepth32Float,
                                                                           tgt.width, tgt.height);
                                                ztex = g_hd_msz_ml;
                                                zclear = 1;                     /* memoryless: always cleared */
                                            } else {
                                                id<MTLTexture> old = g_hd_msz[zid];
                                                g_hd_msz[zid] = hd_ms_tex(old, MTLPixelFormatDepth32Float,
                                                                          tgt.width, tgt.height);
                                                zclear = (g_hd_msz[zid] != old);       /* new: undefined */
                                                ztex = g_hd_msz[zid];
                                            }
                                            zval = 1.f;
                                            if (g_hd_zpend_ms[zid]){ zclear = 1; zval = g_hd_zpend_msv[zid]; g_hd_zpend_ms[zid] = 0; }
                                            if (lost){ zclear = 1; atomic_fetch_add(&g_hd_mszlost, 1ull); }
                                            g_hd_msz_seen[zid] = 1;
                                            g_hd_zms_used[zid] = 1;
                                            g_hd_msp.dz = 1; g_hd_msp.zid = zid; g_hd_msp.zclr = zclear;
                                        } else {
                                            /* screen pass without depth: always Clear + DontCare */
                                            g_hd_msdepth_scratch = hd_ml_hw()
                                                ? hd_ms_tex_ml(g_hd_msdepth_scratch, MTLPixelFormatDepth32Float, tgt.width, tgt.height)
                                                : hd_ms_tex(g_hd_msdepth_scratch, MTLPixelFormatDepth32Float, tgt.width, tgt.height);
                                        }
                                    }
#endif
                                    if (ztex){
                                        rp.depthAttachment.texture = ztex;
                                        rp.depthAttachment.loadAction = zclear ? MTLLoadActionClear : MTLLoadActionLoad;
                                        rp.depthAttachment.storeAction = MTLStoreActionStore;
                                        rp.depthAttachment.clearDepth = zval;
#if NFS_ENHANCED
                                        /* RESOLVE the MSAA depth into the single-sampled twin: passes drawing into a
                                         * single-sampled target with the same Z after the screen
                                         * (light glares etc.) see the scene's depth.  Since the
                                         * twin now reflects the MS state the pending clear is
                                         * invalid. */
                                        if (zres && zres.width == ztex.width && zres.height == ztex.height &&
                                            !(ENVC("HD_ZRESOLVE") && ENVC("HD_ZRESOLVE")[0] == '0')){
                                            rp.depthAttachment.resolveTexture = zres;
                                            rp.depthAttachment.storeAction = MTLStoreActionStoreAndMultisampleResolve;
                                            rp.depthAttachment.depthResolveFilter = MTLMultisampleDepthResolveFilterSample0;
                                            g_zpend[zid] = 0;
                                            g_hd_zsdirty[zid] = 0;
                                            atomic_fetch_add(&g_hd_zres, 1ull);
                                        }
                                        /* storing MS depth: definite at open time for memoryless (never
                                         * stored), chosen at close for Private (hd_ms_end) */
                                        if (c_ms && g_hd_msp.dz && c_ml){
                                            rp.depthAttachment.storeAction = rp.depthAttachment.resolveTexture
                                                ? MTLStoreActionMultisampleResolve : MTLStoreActionDontCare;
                                            g_hd_msz_kept[g_hd_msp.zid] = 0;
                                            atomic_fetch_add(&g_hd_mszskip, 1ull);
                                            g_hd_msp.dz = 0;
                                        } else if (c_ms && g_hd_msp.dz && hd_mslazy()){
                                            g_hd_msp.res = rp.depthAttachment.resolveTexture != nil;
                                            rp.depthAttachment.storeAction = MTLStoreActionUnknown;
                                        } else g_hd_msp.dz = 0;
#endif
                                    } else if (g_depth_tex){
                                        rp.depthAttachment.texture = g_depth_tex;
#if NFS_ENHANCED
                                        if (c_ms) rp.depthAttachment.texture = g_hd_msdepth_scratch;
#endif
                                        rp.depthAttachment.loadAction = MTLLoadActionClear;
                                        rp.depthAttachment.storeAction = MTLStoreActionDontCare;
                                        rp.depthAttachment.clearDepth = 1.0;
                                    }
                                    cur_z = zid;
                                    c_zon = ztex != nil;
#if NFS_ENHANCED
                                    g_hd_msp.on = c_ms; g_hd_msp.ml = c_ml;
                                    if (!c_ms) g_hd_msp.dz = 0;
#endif
                                    if (g_pp_sb){ char pn[48];
                                        if (!dr->target) snprintf(pn, sizeof pn, "EKRAN%s%s%s", tgt != dtex ? " (redirected)" : "", c_ms ? " +MSAA" : "",
#if NFS_ENHANCED
                                                                  c_gb ? " +G-buffer" : "");
#else
                                                                  "");
#endif
                                        else snprintf(pn, sizeof pn, "%s #%u %lux%lu", TGT_FACE(dr->target) ? "cube face" : "target",
                                                      TGT_ID(dr->target), (unsigned long)tgt.width, (unsigned long)tgt.height);
                                        pp_rp(rp, pn);
                                        { static long ff = -1;        /* pass content in the first profiled frame */
                                          if (ff < 0) ff = fno_pre;
                                          if (ff == fno_pre){
                                              unsigned nd = 0, nz = 0, nzt = 0, na = 0, zz = DEPTH_ZID(dr->depth);
                                              for (unsigned q = i; q < g_dn[rb]; q++){ MDraw *x = &g_dbuf[rb][q];
                                                  if (x->is_blit || x->target != dr->target || DEPTH_ZID(x->depth) != zz) break;
                                                  if (x->is_clear){ if (q > i && (x->clr_flags & 2u)) break; continue; }
                                                  nd++; if (x->depth & 2u) nz++; if (x->depth & 1u) nzt++; if (x->alpha_ref > 0.f) na++; }
                                              fprintf(stderr, "  [content] draw#%u %s: zid %u, %u draws, Z writing %u, Z testing %u, alpha tested %u, first ps %d tex %u\n",
                                                      i, pn, zz, nd, nz, nzt, na, dr->ps, dr->tex); } } }
                                    enc = [cb renderCommandEncoderWithDescriptor:rp];
                                    atomic_fetch_add(&g_pass_n, 1ull);
                                    if (g_atdbg) [enc setFragmentBuffer:g_atdbg offset:0 atIndex:6];   /* ALFAVAR diagnostic */
                                    /* The D3D9 pixel center is at an INTEGER, in Metal at +0.5.
                                     * The game shifts full-screen quads by -0.5; in unshifted
                                     * Metal the last row/column was never written.  On the
                                     * quarter-resolution glare target this was a 4-pixel
                                     * darkening at the bottom and right edges after upscaling
                                     * (not on Windows).  Shifting the viewport by half a pixel
                                     * gives the D3D9 coverage and interpolation rule exactly.
                                     * HALFPX=0 old behavior. */
                                    { int hp = halfpx_on();
                                      if (hp){ MTLViewport vp = { 0.5, 0.5, (double)tgt.width, (double)tgt.height, 0.0, 1.0 };
                                               [enc setViewport:vp]; } }
                                    [enc setVertexBuffer:g_mtlvb offset:0 atIndex:0];
                                    [enc setFragmentSamplerState:g_samp atIndex:0];
                                    { float z4[4] = {0, 0, 0, 0};          /* UV SCALE: VS buffer 3 */
                                      [enc setVertexBytes:z4 length:sizeof z4 atIndex:3];
                                      c_uvb[0] = c_uvb[1] = c_uvb[2] = 0.f; }
                                    /* new encoder = all state reset */
                                    c_pso = nil; c_bcol = ~0u; c_depth = ~0u; c_fp_valid = 0;
                                    c_cull = ~0u;
                                    /* In D3D the front face is CLOCKWISE.  CULLWIND=1 can flip it
                                     * (needed if our vertex path flips Y). */
                                    { static int cw = -1;
                                      if (cw < 0){ const char *e = ENVC("CULLWIND");
                                                   cw = (e && *e && *e != '0') ? 1 : 0; }
                                      [enc setFrontFacingWinding:cw ? MTLWindingCounterClockwise
                                                                    : MTLWindingClockwise]; }
                                    for (int q4=0;q4<4;q4++){ c_smp[q4]=~0u; c_tex[q4]=~0u; }
                                    if (dr->target){ unsigned tk_=TGT_ID(dr->target)*8u+TGT_FACE(dr->target);
                                                     touched[tk_>>3] |= 1u<<(tk_&7); }
                                    else screen_touched = 1;
                                    cur_t = dr->target;
                                }
                                if (dr->is_clear || !dr->count) continue;
                                /* PSINFO=<n>: print the target/texture/blend of the first draws
                                 * using this ps. */
                                { static int pi = -2; static int shown = 0;
                                  if (pi == -2){ const char *e=ENVC("PSINFO"); pi = e?atoi(e):-1; }
                                  if (pi >= 0 && dr->ps == pi && shown < 24){ shown++;
                                    fprintf(stderr,"  [psinfo] ps=%d target=%u tex=%u(%lux%lu) tex1=%u"
                                            " blend=%05x bcol=%08x verts=%u  c0=(%.3f %.3f %.3f %.3f)\n",
                                            dr->ps, dr->target, dr->tex,
                                            (dr->tex && dr->tex<MAXTEX && g_tex[dr->tex])?(unsigned long)g_tex[dr->tex].width:0ul,
                                            (dr->tex && dr->tex<MAXTEX && g_tex[dr->tex])?(unsigned long)g_tex[dr->tex].height:0ul,
                                            dr->tex1, dr->blend, dr->bcol, dr->count,
                                            (double)dr->c0[0],(double)dr->c0[1],
                                            (double)dr->c0[2],(double)dr->c0[3]); } }
#if NFS_ENHANCED
                                if (dr->ps >= PSREFL && (c_gb || c_gbx)){ hd_refl_drawn = 1; atomic_fetch_add(&g_hd_refl_draws, 1ull); }
                                /* The same Z surface was used on the screen this frame (with its multisampled twin)
                                 * and now a Z TEST is done on a single-sampled target:
                                 * since the twins don't share content this draw can't see
                                 * the scene's depth.  Count it for measurement. */
                                /* Reverse direction: a single-sampled pass WROTE Z, then the screen (MS twin)
                                 * tests with that depth -- the twin can't see it. */
                                if (!c_ms && c_zon && cur_z < 64u && (dr->depth & 2u)) g_hd_zsdirty[cur_z] = 1;
                                if (c_ms && c_zon && cur_z < 64u && (dr->depth & 1u) && g_hd_zsdirty[cur_z])
                                    atomic_fetch_add(&g_hd_zrev, 1ull);
                                if (!c_ms && c_zon && cur_z < 64u && g_hd_zms_used[cur_z] && (dr->depth & 1u)){
                                    atomic_fetch_add(&g_hd_zshare, 1ull);
                                    if (ENVC("ZSHARELOG")){
                                        static unsigned zt[16], zz[16], zn[16], zps[16]; static int zk = 0;
                                        int q, f = -1;
                                        for (q = 0; q < zk; q++) if (zt[q] == dr->target && zz[q] == cur_z && zps[q] == (unsigned)dr->ps) f = q;
                                        if (f < 0 && zk < 16){ f = zk++; zt[f] = dr->target; zz[f] = cur_z; zps[f] = (unsigned)dr->ps; zn[f] = 0;
                                            id<MTLTexture> tt = (TGT_ID(dr->target) < MAXTEX) ? g_tex[TGT_ID(dr->target)] : nil;
                                            fprintf(stderr, "  [zshare] target=%u (%lux%lu) z=%u ps=%d vs=%d blend=%05x verts=%u\n",
                                                    dr->target, (unsigned long)tt.width, (unsigned long)tt.height,
                                                    cur_z, dr->ps, dr->vsid, dr->blend, dr->count); }
                                        if (f >= 0) zn[f]++;
                                    }
                                }
#endif
                                { int psk = dr->ps;
#if NFS_ENHANCED
                                  /* see SEPARATE G PASS: the game's own shader in the G-less scene */
                                  if (c_gbx && psk >= PSREFL) psk %= PSREFL;
#endif
                                  uint64_t k2 = (uint64_t)(dr->blend & 0xFFFFFu)
                                               | ((uint64_t)(psk & 0xFFF) << 20)
                                               | ((uint64_t)(dr->vsid & 0xFFF) << 32)
                                               | ((uint64_t)(c_ms & 1) << 48)
                                               | ((uint64_t)(c_gb & 1) << 49);
                                  /* see ALPHA TEST VARIANT: discard-free variant if there is no test */
                                  if ((alfavar_mode() && !(dr->alpha_ref > 0.f) && !abt_off("ALFAVAR")) ||
                                      alfavar_mode() == 3)          /* diagnostic: test the diagnostic itself */
                                      k2 |= 1ull << 52;
#if NFS_ENHANCED
                                  if (c_ms) k2 |= (uint64_t)hd_msaa_l2() << 50;   /* see MSAA BY RESOLUTION */
#endif
                                  id<MTLRenderPipelineState> ps2 = pso_for(k2);
                                  if (ps2 != c_pso){ c_pso = ps2;
                                      [enc setRenderPipelineState:ps2]; } }
                                /* BLENDFACTOR: state of the encoder, not the pipeline;
                                 * reset on every target change. */
                                if (dr->cull != c_cull){ c_cull = dr->cull;
                                    [enc setCullMode:(dr->cull == 1u) ? MTLCullModeNone
                                                   : (dr->cull == 2u) ? MTLCullModeFront
                                                                      : MTLCullModeBack]; }
                                if (dr->bcol != c_bcol){ c_bcol = dr->bcol;
                                    [enc setBlendColorRed:(float)((dr->bcol>>16)&0xFFu)/255.f
                                                    green:(float)((dr->bcol>>8)&0xFFu)/255.f
                                                     blue:(float)(dr->bcol&0xFFu)/255.f
                                                    alpha:(float)((dr->bcol>>24)&0xFFu)/255.f]; }
                                for (int si = 0; si < 4; si++){
                                    unsigned sk = dr->smp[si];
#if NFS_ENHANCED
                                    /* mip filter LINEAR (bits 4-5 = 2): a level() read
                                     * is ignored on a sampler without mips. */
                                    if (hd_blur) sk = (sk & ~0x30u) | 0x20u;
#endif
                                    if (sk != c_smp[si]){ c_smp[si] = sk;
                                        [enc setFragmentSamplerState:samp_for(sk) atIndex:si]; } }
                                { unsigned dm = c_zon ? (dr->depth & 63u) : 0u;
                                  if (dm != c_depth){ c_depth = dm;
                                      [enc setDepthStencilState:dss_for(dm)]; } }
                                { struct { float aref, usec, p0, p1; float proj[4]; float c[PSC_N][4]; float fogv[4]; } fp;
                                  fp.aref = dr->alpha_ref;
                                  fp.usec = 0.f; fp.p0 = fp.p1 = 0.f;
#if NFS_ENHANCED
                                  if (hd_blur){
                                      /* scale = Metal texture / the size the game assumes; so the sample
                                       * spacing stays ~1 texel at level L. */
                                      id<MTLTexture> st = g_tex[dr->tex];
                                      float sx = g_hd_ow[dr->tex] ? (float)st.width  / g_hd_ow[dr->tex] : 1.f;
                                      float sy = g_hd_oh[dr->tex] ? (float)st.height / g_hd_oh[dr->tex] : 1.f;
                                      float sc = sx > sy ? sx : sy;
                                      if (sc > 1.f){
                                          float L = enh_blur() * log2f(sc);
                                          fp.p0 = exp2f(L) / sc;     /* offset scale (0..1] */
                                          fp.p1 = L;                 /* mip level */
                                      }
                                      atomic_fetch_add(&g_hd_blurdraw, 1ull);
                                  }
#endif
                                  memcpy(fp.proj, dr->proj, sizeof fp.proj);
                                  memcpy(fp.fogv, dr->fogv, sizeof fp.fogv);
                                  /* same pool sequence + same small fields: no need to build and compare
                                   * the 548-byte block */
                                  if (!(c_fp_valid && dr->psci == c_psci &&
                                        !memcmp(&fp, &c_fp, offsetof(__typeof__(fp), c)) &&
                                        !memcmp(fp.fogv, c_fp.fogv, sizeof fp.fogv))){
                                      memcpy(fp.c, g_pscb[rb][dr->psci], sizeof fp.c);
                                      if (!c_fp_valid || memcmp(&fp, &c_fp, sizeof fp)){
                                          memcpy(&c_fp, &fp, sizeof fp); c_fp_valid = 1;
                                          [enc setFragmentBytes:&fp length:sizeof fp atIndex:0]; }
                                      c_psci = dr->psci; } }
                                { unsigned ts[4] = { dr->tex, dr->tex1, dr->tex2, dr->tex3 };
                                  for (int q4=0;q4<4;q4++){
                                      if (ts[q4] == c_tex[q4]) continue;
                                      c_tex[q4] = ts[q4];
                                      unsigned tt = ts[q4];
                                      freed_use(tt, 0);
                                      id<MTLTexture> txn = (tt && tt < MAXTEX && g_tex[tt])
                                                           ? g_tex[tt] : g_white;
                                      [enc setFragmentTexture:txn atIndex:(NSUInteger)q4];
                                  } }
                                atomic_fetch_add(&g_draw_n, 1ull);
                                if (dr->vsid > 0 && dr->xsh != c_uvb[2]){   /* HUD ANCHORING */
                                    float u4[4] = { dr->uvbm ? dr->uvb[0] : c_uvb[0], dr->uvbm ? dr->uvb[1] : c_uvb[1], dr->xsh, 0 };
                                    [enc setVertexBytes:u4 length:sizeof u4 atIndex:3];
                                    c_uvb[0] = u4[0]; c_uvb[1] = u4[1]; c_uvb[2] = dr->xsh; }
                                if (dr->vsid > 0 && dr->ix_isz){
                                    id<MTLBuffer> pl = g_pool_fr[rb];
                                    [enc setVertexBuffer:pl offset:dr->rawoff atIndex:1];
                                    [enc setVertexBuffer:g_mtlkb  offset:dr->koff   atIndex:2];
                                    [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:dr->count
                                                     indexType:(dr->ix_isz == 4u) ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16
                                                   indexBuffer:pl indexBufferOffset:dr->ix_ib
                                                 instanceCount:1 baseVertex:dr->ix_base baseInstance:0];
                                } else if (dr->vsid > 0){
                                    if (dr->uvbm && (dr->uvb[0] != c_uvb[0] || dr->uvb[1] != c_uvb[1])){
                                        float u4[4] = { dr->uvb[0], dr->uvb[1], c_uvb[2], 0 };
                                        [enc setVertexBytes:u4 length:sizeof u4 atIndex:3];
                                        c_uvb[0] = dr->uvb[0]; c_uvb[1] = dr->uvb[1]; }
                                    [enc setVertexBuffer:g_mtlrvb offset:dr->rawoff atIndex:1];
                                    [enc setVertexBuffer:g_mtlkb  offset:dr->koff   atIndex:2];
                                    [enc drawPrimitives:MTLPrimitiveTypeTriangle
                                            vertexStart:0 vertexCount:dr->count];
                                } else {
                                    [enc drawPrimitives:MTLPrimitiveTypeTriangle
                                            vertexStart:dr->first vertexCount:dr->count];
                                }
                            }
                            { long tt = (long)(g_vn[rb]/3);
                              for (unsigned i = 0; i < g_dn[rb]; i++)
                                  if (g_dbuf[rb][i].vsid > 0) tt += (long)(g_dbuf[rb][i].count/3);
                              atomic_fetch_add(&g_tris_drawn, tt); }
                        }
#if NFS_ENHANCED
                        if (enc){ int pe = rb >= 0 ? hd_ms_end(enc, rb, g_dn[rb], c_ms, c_gb, c_att) : 0;
                                  [enc endEncoding]; enc = nil;
                                  if (pe) hd_ms_fallback(cb, dtex, pe); }
#endif
                        if (enc) [enc endEncoding];
                        /* if nothing was ever drawn to the screen, at least clear it */
                        if (!screen_touched){
                            MTLRenderPassDescriptor *rp =
                                [MTLRenderPassDescriptor renderPassDescriptor];
                            rp.colorAttachments[0].texture = dtex;
                            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
                            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                            rp.colorAttachments[0].clearColor = MTLClearColorMake(
                                atomic_load(&g_clear_r), atomic_load(&g_clear_g),
                                atomic_load(&g_clear_b), 1.0);
                            id<MTLRenderCommandEncoder> e2 =
                                [cb renderCommandEncoderWithDescriptor:rp];
                            [e2 endEncoding];
                        }
                        /* PERFLOG counters must be taken BEFORE the buffer is GIVEN BACK to the game:
                         * afterwards the game thread zeroes and refills the same buffer.
                         * It used to be read after present; when the drawable wait got
                         * longer the log showed "draws 0" (there were actually draws). */
                        unsigned pl_nd = rb >= 0 ? g_dn[rb] : 0u, pl_nv = rb >= 0 ? g_vn[rb] : 0u, pl_nt = 0;
                        if (g_perflog && rb >= 0){
                            /* number of distinct textures (+ targets) bound in the frame: the kernel cost of
                             * command buffer submission grows with the resource count */
                            static uint32_t pl_st[MAXTEX]; static uint32_t pl_gen = 0;
                            pl_gen++;
                            for (unsigned q = 0; q < pl_nd; q++){
                                const MDraw *x = &g_dbuf[rb][q];
                                unsigned ids[5] = { x->tex, x->tex1, x->tex2, x->tex3, TGT_ID(x->target) };
                                for (int w = 0; w < 5; w++){ unsigned id_ = ids[w];
                                    if (id_ && id_ < MAXTEX && pl_st[id_] != pl_gen){ pl_st[id_] = pl_gen; pl_nt++; } }
                            }
                        }
                        pthread_mutex_lock(&g_qlock);
                        g_ready = -1; g_enc_rb = -1;
                        pthread_cond_signal(&g_qcond);
                        pthread_mutex_unlock(&g_qlock);
                        /* TESTFILL: fill with a known color to verify the capture path.
                         * If the PNG comes out purple the capture is sound. */
                        if (ENVC("TESTFILL")){
                            MTLRenderPassDescriptor *rp =
                                [MTLRenderPassDescriptor renderPassDescriptor];
                            rp.colorAttachments[0].texture = dtex;
                            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
                            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                            rp.colorAttachments[0].clearColor =
                                MTLClearColorMake(1.0, 0.0, 1.0, 1.0);
                            id<MTLRenderCommandEncoder> e3 =
                                [cb renderCommandEncoderWithDescriptor:rp];
                            [e3 endEncoding];
                        }
                        if (g_perflog && atomic_exchange(&g_pl_thumb_req, 0)) pl_thumb(cb, dtex, fno_pre);
                        /* Take a readable copy BEFORE present: reading back from a presented
                         * drawable's texture isn't reliable. */
                        if (dumpnow){
                            /* recreate if the size changed (fullscreen transition) */
                            if (g_capture && (g_capture.width != dtex.width ||
                                              g_capture.height != dtex.height)) g_capture = nil;
                            if (!g_capture){
                                MTLTextureDescriptor *td =
                                    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
                                        MTLPixelFormatBGRA8Unorm
                                        width:dtex.width height:dtex.height
                                        mipmapped:NO];
                                td.usage = MTLTextureUsageShaderRead;
                                td.storageMode = MTLStorageModeShared;
                                g_capture = [g_dev newTextureWithDescriptor:td];
                            }
                            if (g_capture){
                                id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
                                [bl copyFromTexture:dtex sourceSlice:0 sourceLevel:0
                                       sourceOrigin:MTLOriginMake(0,0,0)
                                         sourceSize:MTLSizeMake(dtex.width,
                                                                dtex.height,1)
                                          toTexture:g_capture destinationSlice:0
                                   destinationLevel:0
                                  destinationOrigin:MTLOriginMake(0,0,0)];
                                [bl endEncoding];
                            }
                        }
                        { struct timespec _e1; clock_gettime(CLOCK_MONOTONIC,&_e1);
                          unsigned long long dt =
                              (unsigned long long)(_e1.tv_sec-_e0.tv_sec)*1000000000ull
                            + (unsigned long long)_e1.tv_nsec - (unsigned long long)_e0.tv_nsec;
                          atomic_fetch_add(&g_enc_ns, dt);
                          if (gpuw_in(fno_pre)){ atomic_fetch_add(&g_encw_ns, dt); atomic_fetch_add(&g_encw_n, 1ull); } }
                        int in_win = gpuw_in(fno_pre);
                        /* GPU energy at the start/end of the GPUWIN window (see GPU ENERGY) */
                        if (g_gpuw_a >= 0 && fno_pre == g_gpuw_a && !g_gS0ok){ g_gS0ok = ior_read(&g_gS0); g_gT0 = gpuE_now_s(); }
                        if (g_gpuw_a >= 0 && fno_pre == g_gpuw_b && g_gS0ok && !g_gS1ok){ g_gS1ok = ior_read(&g_gS1); g_gT1 = gpuE_now_s(); }
                        pp_end(cb);
                        [cb addCompletedHandler:^(id<MTLCommandBuffer> b2){
                            /* a frame ending with an error: GPU times come back 0, it corrupts the measurements too */
                            if (b2.status == MTLCommandBufferStatusError){
                                unsigned long ne = atomic_fetch_add(&g_cb_err, 1ul);
                                if (ne < 5) fprintf(stderr, "metal: COMMAND BUFFER ERROR: %s\n",
                                                    [[b2.error localizedDescription] UTF8String]); }
                            fb_release(fbi);        /* GPU done: the set can be reused */
                            double g = (b2.GPUEndTime - b2.GPUStartTime) * 1e9;
                            if (g > 0.0 && g < 1e9){
                                unsigned long long ns = (unsigned long long)g;
                                if (in_win){ atomic_fetch_add(&g_gpuw_ns, ns); atomic_fetch_add(&g_gpuw_n, 1ull); }
                                if (ab_mode >= 0 && ab_count){ atomic_fetch_add(&g_ab_ns[ab_mode], ns); atomic_fetch_add(&g_ab_cnt[ab_mode], 1ull); }
                                if (abt_mode >= 0 && abt_cnt){ atomic_fetch_add(&g_abt_ns[abt_mode], ns); atomic_fetch_add(&g_abt_cnt[abt_mode], 1ull); }
                                atomic_fetch_add(&g_gpu_ns, ns);
                                atomic_fetch_add(&g_gpu_n, 1ull);
                                if (g_perflog) perflog_gpu(fno_pre, g);
                                unsigned long long mx = atomic_load(&g_gpu_max_ns);
                                while (ns > mx &&
                                       !atomic_compare_exchange_weak(&g_gpu_max_ns, &mx, ns)) {}
                            } }];
                        if (d && fbonly_on() && dtex != d.texture) fbonly_copy(cb, dtex, d.texture);
                        if (d) [cb presentDrawable:d];
                        [cb commit];
                        if (noshow){
                            /* the guest waits in metal_rt_readback until this is on the GPU */
                            [cb waitUntilCompleted];
                            pthread_mutex_lock(&g_qlock);
                            g_flush_done = g_flush_req;
                            pthread_cond_broadcast(&g_qcond);
                            pthread_mutex_unlock(&g_qlock);
                        }
                        long fno = atomic_fetch_add(&g_frames_drawn, 1) + 1;
                        if (g_perflog){ struct timespec _p1; clock_gettime(CLOCK_MONOTONIC, &_p1);
                            perflog_render_frame(fno, pl_nt,
                                (uint64_t)(_d1.tv_sec - _d0.tv_sec) * 1000000000ull + (uint64_t)_d1.tv_nsec - (uint64_t)_d0.tv_nsec,
                                (uint64_t)(_p1.tv_sec - _e0.tv_sec) * 1000000000ull + (uint64_t)_p1.tv_nsec - (uint64_t)_e0.tv_nsec,
                                pl_nd, pl_nv); }
                        /* FPSCSV=<path>: time + workload per frame.  To see WHERE the stutter
                         * is without guessing. */
                        { static FILE *fp = NULL; static int init = 0;
                          static uint64_t prev = 0;
                          if (!init){ init = 1; const char *e = ENVC("FPSCSV");
                              if (e && *e){ fp = fopen(e, "w");
                                  if (fp) fprintf(fp, "frame,ms,draws,verts,tris\n"); } }
                          if (fp){ struct timespec _ts;
                              clock_gettime(CLOCK_MONOTONIC, &_ts);
                              uint64_t now = (uint64_t)_ts.tv_sec*1000000ull
                                           + (uint64_t)_ts.tv_nsec/1000ull;
                              if (prev) fprintf(fp, "%ld,%.2f,%u,%u,%u\n", fno,
                                    (double)(now - prev)/1000.0,
                                    pl_nd, pl_nv, pl_nv/3u);
                              prev = now;
                              if ((fno % 240) == 0) fflush(fp); } }
                        if (dumpnow){
                            [cb waitUntilCompleted];
                            if (g_capture) dump_texture(g_capture, fno_pre);
                            { const char *dt = ENVC("DUMPTEX");   /* comma-separated list */
                              for (const char *q = dt; q && *q; ){
                                  while (*q==',' || *q==' ') q++;
                                  if (!*q) break;
                                  unsigned tn = (unsigned)strtoul(q, 0, 0);
                                  dump_one_texture(tn, fno_pre*1000 + tn);
                                  while (*q && *q != ',') q++; } }
                        }
                        (void)fno;
                    }
                }
                /* SLEEP until the frame is ready (not polling).  The 4 ms upper limit
                 * is only for draining the NSEvent queue: when a frame comes
                 * cond_signal wakes it up immediately. */
                { struct timespec ts, _f0, _f1; clock_gettime(CLOCK_MONOTONIC,&_f0);
                  clock_gettime(CLOCK_REALTIME, &ts);
                  ts.tv_nsec += 4L * 1000 * 1000;
                  if (ts.tv_nsec >= 1000000000L){ ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
                  pthread_mutex_lock(&g_qlock);
                  if (g_ready < 0) pthread_cond_timedwait(&g_rcond, &g_qlock, &ts);
                  pthread_mutex_unlock(&g_qlock);
                  clock_gettime(CLOCK_MONOTONIC,&_f1);
                  atomic_fetch_add(&g_wait_frame_ns,
                    (unsigned long long)(_f1.tv_sec-_f0.tv_sec)*1000000000ull
                    + (unsigned long long)_f1.tv_nsec - (unsigned long long)_f0.tv_nsec); }
            }
        }
        /* Re-enable the shortcuts; when the pipe closes the watchdog exits too. */
        ks_engel(0);
        if (g_ks_fd >= 0){ close(g_ks_fd); g_ks_fd = -1; }
        fprintf(stderr, "metal: empty frames skipped = %ld\n", g_skip_empty);
        fprintf(stderr, "metal: buffer ring: in %lu frames the previous frame was still on the GPU "
                "(a single buffer would have been overwritten), waited for an empty set %lu times (total %.1f ms)\n",
                atomic_load(&g_fb_overlap), atomic_load(&g_fb_wait),
                (double)atomic_load(&g_fb_wait_ns) / 1e6);
        { NSUInteger v = 0, r = 0, k = 0;
          for (int i = 0; i < NFB; i++){ v += g_fb_vb[i].length; r += g_fb_rvb[i].length; k += g_fb_kb[i].length; }
          fprintf(stderr, "metal: buffer ring size (sum over %d sets): verts %.1f MB + raw verts %.1f MB + constants %.1f MB, grew %lu times\n",
                  NFB, v / 1048576.0, r / 1048576.0, k / 1048576.0, atomic_load(&g_fb_grow)); }
        fprintf(stderr, "metal: vertex/index pool: %.1f MB (used %.1f MB), %lu slices live, %lu built (%.1f MB copied), %lu dropped, table full %lu\n",
                g_pool ? g_pool.length / 1048576.0 : 0.0, g_pool_top / 1048576.0, g_mir_live, g_mir_build,
                g_mir_bytes / 1048576.0, g_mir_drop, g_mir_full);
        fprintf(stderr, "metal: command buffer error: %lu frames\n", atomic_load(&g_cb_err));
        { unsigned long n = atomic_load(&g_enc_late);
          fprintf(stderr, "metal: encoding exceeded 60 ms (used to swap into the buffer being encoded): %lu times", n);
          for (unsigned long q = 0; q < n && q < 8; q++) fprintf(stderr, "%s%ld", q ? "," : "  kareler: ", g_enc_late_f[q]);
          fprintf(stderr, "\n"); }
        fprintf(stderr, "metal: render targets released after Reset: %lu (used afterwards: texture %lu, target %lu)\n",
                g_rt_freed, g_freed_use_smp, g_freed_use_tgt);
        if (atomic_load(&g_hud_moved) || atomic_load(&g_hud_kept))
            fprintf(stderr, "metal: HUD anchoring: %llu draws moved to the edges, %llu kept\n",
                    atomic_load(&g_hud_moved), atomic_load(&g_hud_kept));
        fprintf(stderr, "metal: Z surface: %llu passes without Z, %llu passes size mismatch (scratch)\n",
                (unsigned long long)g_z_null, (unsigned long long)g_z_mismatch);
        { unsigned long long tb = 0; unsigned live = 0;
          for (unsigned i = 1; i < g_texn && i < MAXTEX; i++) if (g_tex[i]){ live++; tb += g_tex[i].allocatedSize; }
          fprintf(stderr, "metal: texture ids used %u of %u (%u still alive, %.0f MB), table full %lu times\n",
                  g_texn, (unsigned)MAXTEX, live, tb / 1048576.0, g_tex_full_hits); }
        fprintf(stderr, "metal: window closed (%ld frames drawn)\n",
                atomic_load(&g_frames_drawn));
    }
}

/* Frame budget report: GPU busy time, CPU encoding time, pass/draw count.
 * "What are we spending hardware on" can only be answered by seeing these three
 * together. */
void metal_budget_report(void){
#if NFS_ENHANCED
    fprintf(stderr, "[HD] road blur: %llu draws, %llu mip generations\n",
            atomic_load(&g_hd_blurdraw), atomic_load(&g_hd_mipgen));
    { unsigned long long f = atomic_load(&g_rt_frames);
      fprintf(stderr, "[HD] ray tracing: %llu frames, per frame mean %.0f tris / %.0f instant draws; skipped without kernel %llu, overflow %llu\n",
              f, f ? (double)atomic_load(&g_rt_tris) / f : 0.0, f ? (double)atomic_load(&g_rt_draws) / f : 0.0,
              atomic_load(&g_rt_skipk), atomic_load(&g_rt_over));
      fprintf(stderr, "[HD] RT cache: per frame mean instant %.0f tris + static %.0f tris; promoted %llu, evicted %llu, resets %llu, static builds %llu\n",
              f ? (double)atomic_load(&g_rt_dynv) / 3 / f : 0.0, f ? (double)atomic_load(&g_rt_statv) / 3 / f : 0.0,
              atomic_load(&g_rt_promo), atomic_load(&g_rt_evict), atomic_load(&g_rt_reset), atomic_load(&g_rt_sbuilds));
      fprintf(stderr, "[HD] RT instant structure: %llu full builds (%llu due to position shift), %llu in-place updates (refit %s)\n",
              atomic_load(&g_rt_nbuild), atomic_load(&g_rt_nmoved), atomic_load(&g_rt_nrefit), hd_rt_refit_on() ? "ON" : "OFF");
      { uint32_t gv = 0; for (unsigned k = 0; k < g_rt_ng; k++) gv += g_rt_grp[k].built;
        fprintf(stderr, "[HD] RT cache: merges %llu (oldest dropped %llu), part builds %llu, refits %llu, "
                "rehashes %llu, store %u verts (%u dead; %u groups %u verts, new %u)\n",
                atomic_load(&g_rt_merges), atomic_load(&g_rt_sdrop), atomic_load(&g_rt_sbuilds),
                atomic_load(&g_rt_srefits), atomic_load(&g_rt_rehash), rt_depot_used(), rt_depot_dead(),
                g_rt_ng, gv, g_rt_scur - g_rt_recent.lo); }
      if (g_rt_fixcnt){ const uint32_t *fc = (const uint32_t *)[g_rt_fixcnt contents];
          fprintf(stderr, "[HD] RT vertex check: %s; neutralized tris: NaN/infinite %u, too far %u (per frame mean %.1f)\n",
                  g_rt_fixk ? "ON" : "OFF", fc[0], fc[1], f ? (double)(fc[0] + fc[1]) / (double)f : 0.0); } }
    fprintf(stderr, "[HD] SSR: %llu composites, %llu reflection draws\n",
            atomic_load(&g_hd_ssr_n), atomic_load(&g_hd_refl_draws));
    fprintf(stderr, "[HD] MSAA lazy store: %s, mispredicted (extra resolve pass) %llu; deferred intermediate passes %llu\n",
            hd_mslazy() ? "ON" : "OFF", atomic_load(&g_hd_msfb), atomic_load(&g_hd_reord));
    fprintf(stderr, "[HD] MSAA %dx: %llu screen passes, %llu blits to screen, %llu Z resolves,"
            " Z sharing: %llu screen->target draws, %llu target->screen draws (invisible)\n",
            g_hd_msn, atomic_load(&g_hd_mspass), atomic_load(&g_hd_msblit),
            atomic_load(&g_hd_zres), atomic_load(&g_hd_zshare), atomic_load(&g_hd_zrev));
    fprintf(stderr, "[HD] MSAA attachments: %llu passes MEMORYLESS, mispredicted %llu%s | MS depth: %llu stored, "
            "%llu not stored, %llu times a load without clear was requested but the samples were lost (should be 0)\n",
            atomic_load(&g_hd_ml_pass), atomic_load(&g_hd_ml_miss), g_hd_ml_off ? " (memoryless mode OFF)" : "",
            atomic_load(&g_hd_mszst), atomic_load(&g_hd_mszskip), atomic_load(&g_hd_mszlost));
    fprintf(stderr, "[HD] mid-scene copy (motion blur): %llu frames -- scene continued multisampled\n",
            atomic_load(&g_hd_brk_n));
    { unsigned long long gn = atomic_load(&g_hd_gp_n);
      fprintf(stderr, "[HD] separate G pass: %s, %llu passes, %.1f reflecting draws per pass\n",
              hd_gsep() ? "ON" : "OFF", gn, gn ? (double)atomic_load(&g_hd_gp_draws) / gn : 0.0); }
#endif
    unsigned long long n  = atomic_load(&g_gpu_n);
    unsigned long long gs = atomic_load(&g_gpu_ns);
    unsigned long long mx = atomic_load(&g_gpu_max_ns);
    unsigned long long es = atomic_load(&g_enc_ns);
    long fr = atomic_load(&g_frames_drawn);
    if (!fr) return;
    fprintf(stderr, "  --- frame budget ---\n");
    if (n) fprintf(stderr, "  GPU busy          : mean %.2f ms/frame, max %.2f ms (%llu frames measured)\n",
                   (double)gs/(double)n/1e6, (double)mx/1e6, n);
    fprintf(stderr, "  CPU encoding      : mean %.2f ms/frame (writing the draw list to Metal)\n",
            (double)es/(double)fr/1e6);
    pp_report();
    if (g_ab_on > 0){
        fprintf(stderr, "  --- step by step measurement (HD_RT_AB) ---\n");
        for (int m = 0; m < AB_N; m++){ unsigned long long c = atomic_load(&g_ab_cnt[m]);
            if (c) fprintf(stderr, "  [AB] %-48s GPU %7.2f ms/frame (%llu frames)\n", g_ab_name[m], (double)atomic_load(&g_ab_ns[m]) / (double)c / 1e6, c); } }
    { unsigned long long wn = atomic_load(&g_gpuw_n), en = atomic_load(&g_encw_n);
      if (wn && en) fprintf(stderr, "  WINDOW %ld-%ld    : GPU %.3f ms/frame, CPU encoding %.3f ms/frame (%llu frames)\n",
              g_gpuw_a, g_gpuw_b, (double)atomic_load(&g_gpuw_ns)/(double)wn/1e6,
              (double)atomic_load(&g_encw_ns)/(double)en/1e6, wn);
      if (g_gS0ok && g_gS1ok && g_gT1 > g_gT0){
          double nf = (double)(g_gpuw_b - g_gpuw_a);
          fprintf(stderr, "  WINDOW %ld-%ld    : GPU ENERGY %.3f mJ/frame, mean %.3f W (IOReport, system wide)\n",
                  g_gpuw_a, g_gpuw_b, (g_gS1.j - g_gS0.j) * 1e3 / nf, (g_gS1.j - g_gS0.j) / (g_gT1 - g_gT0));
          fprintf(stderr, "  WINDOW %ld-%ld    : GPU MEMORY read %.1f write %.1f MB/frame, DRAM read %.1f write %.1f MB/frame (AMC)\n",
                  g_gpuw_a, g_gpuw_b, (g_gS1.rd - g_gS0.rd) / 1e6 / nf, (g_gS1.wr - g_gS0.wr) / 1e6 / nf,
                  (g_gS1.drd - g_gS0.drd) / 1e6 / nf, (g_gS1.dwr - g_gS0.dwr) / 1e6 / nf);
          double ga = g_gS1.gact - g_gS0.gact, gt = g_gS1.gtot - g_gS0.gtot, gc = g_gS1.gcy - g_gS0.gcy;
          if (gt > 0 && gc > 0 && g_ior_fmax > 0)
              fprintf(stderr, "  WINDOW %ld-%ld    : GPU OCCUPANCY %%%.1f, mean clock %.0f MHz, busy %.2f Mcycles/frame = %.2f ms/frame @%.0f MHz (highest clock; 120 fps limit 8.33)\n",
                      g_gpuw_a, g_gpuw_b, 100.0 * ga / gt, ga > 0 ? gc / (ga / gt * (g_gT1 - g_gT0) * 1e6) : 0.0,
                      gc / 1e6 / nf, gc / g_ior_fmax / 1e3 / nf, g_ior_fmax); } }
    abt_report();
    fprintf(stderr, "  guest WAIT        : %.2f ms/frame (for the previous frame to reach the screen)\n"
                    "  main thread wait  : %.2f ms/frame frame-ready, %.2f ms/frame nextDrawable (vsync)\n",
            (double)atomic_load(&g_wait_present_ns)/(double)fr/1e6,
            (double)atomic_load(&g_wait_frame_ns)/(double)fr/1e6,
            (double)atomic_load(&g_wait_drawable_ns)/(double)fr/1e6);
    fprintf(stderr, "  render passes     : %.1f/frame,  draw calls: %.0f/frame\n",
            (double)atomic_load(&g_pass_n)/(double)fr,
            (double)atomic_load(&g_draw_n)/(double)fr);
    fprintf(stderr, "  screen redirection: %.2f/frame copies removed (see SCREEN REDIRECTION)\n",
            (double)atomic_load(&g_scr_redir)/(double)fr);
    fprintf(stderr, "  UV scale          : %.2f/frame full-screen quad half-texel corrections (see UV SCALE)\n",
            (double)atomic_load(&g_uvfix_n)/(double)fr);
    if (ENVC("TEXUPSAY")){
        fprintf(stderr, "  texture uploads (most):");
        for (int k = 0; k < 12; k++){ int best = -1;
            for (int q = 1; q < MAXTEX; q++) if (g_texup_n[q] && (best < 0 || g_texup_n[q] > g_texup_n[best])) best = q;
            if (best < 0) break;
            fprintf(stderr, " #%d:%u", best, g_texup_n[best]); g_texup_n[best] = 0; }
        fprintf(stderr, "\n"); }
    fprintf(stderr, "  ordered uploads   : %.2f/frame texture uploads ordered with draws (overflow %llu; see ORDERED TEXTURE UPLOADS)\n",
            (double)atomic_load(&g_up_ord)/(double)fr, (unsigned long long)atomic_load(&g_up_ovf));
    fprintf(stderr, "  post-process chain: %.2f/frame merged passes (%.2f/frame records)\n",
            (double)atomic_load(&g_fbz_n)/(double)fr, (double)atomic_load(&g_fbz_rec)/(double)fr);
    if (atomic_load(&g_fbz_val) && g_fbv_cnt){
        const volatile uint32_t *c = (const volatile uint32_t *)[g_fbv_cnt contents];
        fprintf(stderr, "  chain verification: %llu chains; A (final output): differing pixels %u, largest %u/255, >1: %u;"
                        " B (intermediate): differing %u, largest %u/255, >1: %u\n",
                (unsigned long long)atomic_load(&g_fbz_val), c[0], c[1], c[2], c[4], c[5], c[6]); }
    if (g_atdbg){
        const volatile uint32_t *c = (const volatile uint32_t *)[g_atdbg contents];
        unsigned long long tot = 0; for (int q = 0; q < MAXPS; q++) tot += c[q];
        fprintf(stderr, "  alpha test check  : %llu fragments would differ in the test-free variant (ALFAVAR=%d; should be 0 at 2)\n",
                tot, alfavar_mode());
        for (int k = 0; k < 12 && tot; k++){           /* the ps with the most counts */
            int best = -1; for (int q = 0; q < MAXPS; q++) if (c[q] && (best < 0 || c[q] > c[best])) best = q;
            if (best < 0) break;
            fprintf(stderr, "    ps %4d%s: %u\n", best, best == 0 ? " (fmain)" : "", c[best]);
            ((volatile uint32_t *)c)[best] = 0; }
        /* fragments arriving with an out-of-[0,1] vertex color (in the diagnostic variant; D3D9 clamps it) */
        unsigned long long tc = 0; for (int q = 0; q < MAXPS; q++) tc += c[4096 + q];
        fprintf(stderr, "  vertex color check: %llu fragments arrived with out-of-[0,1] color\n", tc);
        for (int k = 0; k < 12 && tc; k++){
            int best = -1; for (int q = 0; q < MAXPS; q++) if (c[4096 + q] && (best < 0 || c[4096 + q] > c[4096 + best])) best = q;
            if (best < 0) break;
            fprintf(stderr, "    ps %4d%s: %u\n", best, best == 0 ? " (fmain)" : "", c[4096 + best]);
            ((volatile uint32_t *)c)[4096 + best] = 0; } }
}
