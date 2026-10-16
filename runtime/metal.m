/* D3D9 -> Metal koprusu (1. asama: pencere + Clear/Present)
 *
 * macOS'ta pencere ANA thread'de olmali. Guest zaten ayri bir thread'de
 * calistigi icin uygun: ana thread NSApplication dongusunu surer, guest
 * thread'i cizim komutlarini kuyruga yazar.                              */
#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>

/* getenv() her cagrida ortam dizisini TARAR ve kilit alir.  Cizim basina
 * calisan yollarda (ornegin SetVertexShaderConstantF icindeki "PDLIST"
 * kontrolu &&-zincirinin ILK terimiydi) olculebilir maliyeti var:
 * profilde getenv+__findenv_locked konuk thread'in %3'unu yiyordu.
 * Ortam degiskenleri kosu boyunca degismez -> bir kez coz, sakla. */
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
static long             g_skip_empty;   /* icerigi olmayan kare: sunma */
static id<MTLTexture>   g_capture;   /* kare yakalama icin okunabilir kopya */

/* --- kare dokumu: DUMPFRAME=100,300 ile secilen kareleri PNG yazar --------
 * Ekranda ne oldugunu tahmin etmek yerine dogrudan gorebilmek icin.        */
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
static int dump_wanted(long f){
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
        snprintf(path, sizeof path, "%s/nfsu2_kare_%04ld.png", g_dump_dir, frame);
        CFStringRef sp = CFStringCreateWithCString(NULL, path, kCFStringEncodingUTF8);
        CFURLRef url = CFURLCreateWithFileSystemPath(NULL, sp, kCFURLPOSIXPathStyle, false);
        CGImageDestinationRef dst =
            CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL);
        if (dst){ CGImageDestinationAddImage(dst, img, NULL);
                  CGImageDestinationFinalize(dst); CFRelease(dst); }
        CFRelease(url); CFRelease(sp); CGImageRelease(img); CGContextRelease(cg);
        fprintf(stderr, "metal: kare %ld yazildi -> %s\n", frame, path);
    }
    CGColorSpaceRelease(cs); free(buf);
}
static id<MTLDevice>    g_dev;
static id<MTLCommandQueue> g_queue;

/* Guest thread'in yazdigi, ana thread'in okudugu kare durumu */
static _Atomic float    g_clear_r = 0.f, g_clear_g = 0.f, g_clear_b = 0.f;
static _Atomic int      g_frame_pending = 0;
/* Kim kimi bekliyor?  Kare suresi = konuk CPU + ana thread'in alma
 * gecikmesi + vsync.  Ucunu ayirmadan hangisini optimize edecegimizi
 * bilemeyiz. */
static _Atomic unsigned long long g_wait_present_ns = 0;  /* konuk: onceki kare alinsin diye */
static _Atomic unsigned long long g_wait_drawable_ns = 0; /* ana: nextDrawable (vsync) */
static _Atomic unsigned long long g_wait_frame_ns = 0;    /* ana: kare hazir olsun diye */

static _Atomic long     g_frames_drawn  = 0;
static _Atomic long     g_tris_drawn    = 0;
static _Atomic long     g_tex_created   = 0, g_tex_uploaded = 0;
static _Atomic int      g_should_quit   = 0;

/* --- fare durumu: on yuz fareyle geziliyor (DIMOUSESTATE2) --------------
 * DirectInput goreli hareket ister; mutlak konumu da GetCursorPos icin
 * saklıyoruz. */
static _Atomic int g_mouse_x = 640, g_mouse_y = 360;
/* --- sunum geometrisi --------------------------------------------------
 * Cizim yuzeyi PIKSEL, pencere icerigi NOKTA cinsindendir; Retina'da bu
 * ikisi 2 kat farklidir.  Ikisini birbirine esit saymak iki ayri hataya
 * yol aciyordu: (1) 1280x960'lik yuzey 2560x1836 piksellik alana
 * buyutulup her piksel yeniden orneklendigi icin goruntu bulaniklasiyor,
 * (2) fare noktalari piksel sanildigi icin imlec kayiyordu. */
static _Atomic int g_draw_w = 1280, g_draw_h = 720;   /* cizim  - piksel */
static _Atomic int g_cont_w = 1280, g_cont_h = 720;   /* icerik - nokta  */
/* Oyunun SANDIGI cozunurluk.  Retina olceginde cizim yuzeyinden kucuk
 * (oyun 1280x960 sanirken yuzey 2448x1836); imlec bu uzayda olmali. */
static _Atomic int g_game_w = 1280, g_game_h = 720;
/* --- tam ekran ----------------------------------------------------------
 * Tam ekranda cizim yuzeyi EKRANIN piksel boyutuna (ve oranina) cikar;
 * oyunun 4:3 istegi kirpma uzayinda Hor+ ile duzeltilir (res_pick/aspfix).
 * Gecis sirasinda boyut ancak oyunun Reset'iyle degisebildigi icin
 * com_request_reset() cihaz kaybi uretir.  g_fs_pw/ph: tam ekran icerik
 * alani (piksel), girildiginde AppKit'ten olculur. */
static _Atomic int g_fs = 0, g_fs_pw = 0, g_fs_ph = 0;
/* Arayuz (ortografik) HUDFIX ile merkeze gore aspfix kadar daraltiliyor;
 * fare de o alana eslenmeli.  x10000 tamsayi. */
static _Atomic int g_ui_asp = 10000;
void metal_set_ui_aspect(float a){
    if (!(a > 0.05f && a <= 1.0f)) a = 1.0f;
    atomic_store(&g_ui_asp, (int)lroundf(a * 10000.f));
}
void com_request_reset(void);
static _Atomic int g_mouse_dx = 0, g_mouse_dy = 0, g_mouse_dz = 0;
static _Atomic unsigned g_mouse_btn = 0;

void metal_mouse_add(int dx, int dy){                 /* sentetik hareket (test) */
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
    /* MOUSEAT=<x>,<y>[@<kare>,<x>,<y>]: imleci oyun uzayinda sabitle (test) */
    { const char *e = ENVC("MOUSEAT");
      if (e && *e){ int ax = 0, ay = 0, f2 = 0, bx = 0, by = 0;
          int n2 = sscanf(e, "%d,%d@%d,%d,%d", &ax, &ay, &f2, &bx, &by);
          if (n2 == 5 && atomic_load(&g_frames_drawn) >= f2){ ax = bx; ay = by; }
          if (x) *x = ax; if (y) *y = ay; return; } }
    if (x) *x = atomic_load(&g_mouse_x);
    if (y) *y = atomic_load(&g_mouse_y);
}
/* Oyunun sandigi pencere boyutu (GetClientRect/GetWindowRect).  Oyun
 * GetCursorPos'u bu dikdortgene gore kendi arayuz uzayina olcekliyor;
 * sabit 1280x720 donulurken 1280x960'ta imlec dikeyde 1.33 kat asagi
 * kayiyordu (olculdu: MOUSEAT). */
void metal_game_size(unsigned *w, unsigned *h){
    if (w) *w = (unsigned)atomic_load(&g_game_w);
    if (h) *h = (unsigned)atomic_load(&g_game_h);
}
void metal_set_game_size(unsigned w, unsigned h){
    if (!w || !h || w > 8192u || h > 8192u) return;
    atomic_store(&g_game_w, (int)w); atomic_store(&g_game_h, (int)h);
}
/* Goruntunun pencerede kapladigi alan (nokta).  Icerige EN-BOY KORUNARAK
 * oturtuldugu icin (kCAGravityResizeAspect) tam ekranda kenarlarda siyah
 * bant olusur.  0 donerse geometri henuz bilinmiyor. */
static int shown_rect(double *ox, double *oy, double *vw, double *vh){
    double dw = atomic_load(&g_draw_w), dh = atomic_load(&g_draw_h);
    double cw = atomic_load(&g_cont_w), ch = atomic_load(&g_cont_h);
    if (dw < 1.0 || dh < 1.0 || cw < 1.0 || ch < 1.0) return 0;
    double k = fmin(cw / dw, ch / dh);           /* nokta / cizim pikseli */
    *vw = dw * k; *vh = dh * k;
    *ox = (cw - *vw) * 0.5; *oy = (ch - *vh) * 0.5;
    return 1;
}
/* Pencere noktasi -> OYUNUN cozunurluk uzayi (cizim yuzeyi degil: Retina
 * olceginde oyun 1280x960 sanirken yuzey 2448x1836).  Bandi hesaba katar;
 * Y ters cevrilir: AppKit alt-sol, oyun ust-sol. */
static void mouse_from_point(double px, double py, int *gx, int *gy){
    double ox, oy, vw, vh;
    double lw = atomic_load(&g_game_w), lh = atomic_load(&g_game_h);
    double ch = atomic_load(&g_cont_h);
    if (!shown_rect(&ox, &oy, &vw, &vh) || lw < 1.0 || lh < 1.0){
        *gx = (int)px; *gy = (int)py; return; }
    { double a = atomic_load(&g_ui_asp) / 10000.0;   /* arayuz ortada, daraltilmis */
      ox += vw * (1.0 - a) * 0.5; vw *= a; }
    double x = (px - ox) / vw * lw;
    double y = (ch - py - oy) / vh * lh;
    if (x < 0.0) x = 0.0; else if (x > lw - 1.0) x = lw - 1.0;
    if (y < 0.0) y = 0.0; else if (y > lh - 1.0) y = lh - 1.0;
    *gx = (int)x; *gy = (int)y;
}
/* DirectInput goreli hareketi.  Eskiden pencere NOKTASI = oyun pikseli
 * oldugu icin nokta deltasi oldugu gibi gidiyordu.  Pencere artik oyunun
 * cozunurlugunden bagimsiz boyutlandigi icin ayni hissi koruyacak sekilde
 * olcekliyoruz: goruntunun bir ucundan otekine gitmek yine oyun genisligi
 * kadar hareket uretir.  (Olceksiz halde NFSNATIVE=0'da pencere yarim
 * boyutta oldugu icin imlec yarim hizda kaliyordu.) */
static void mouse_delta_scale(double *sx, double *sy){
    double ox, oy, vw, vh;
    double lw = atomic_load(&g_game_w), lh = atomic_load(&g_game_h);
    *sx = *sy = 1.0;
    if (!shown_rect(&ox, &oy, &vw, &vh) || lw < 1.0 || lh < 1.0) return;
    vw *= atomic_load(&g_ui_asp) / 10000.0;
    *sx = lw / vw; *sy = lh / vh;
}

/* --- klavye durumu: DirectInput'un okuyacagi 256 baytlik tablo --- */
static _Atomic unsigned char g_keys[256];

/* macOS sanal tus kodu -> DirectInput DIK_* tarama kodu */
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
/* Guest tarafi: DirectInput klavye durumunu doldur */
void metal_fill_keyboard(unsigned char *dst, unsigned n){
    if (n > 256) n = 256;
    for (unsigned i = 0; i < n; i++)
        dst[i] = atomic_load(&g_keys[i]) ? 0x80 : 0x00;
}

/* Dokular pencere acilmadan ONCE yaratiliyor.  Bu yuzden "kullanilabilir"
 * olcutu cihazdir, katman degil; yoksa erken dokular sessizce kayboluyordu. */
int  metal_available(void){ return g_dev != nil; }
int  metal_window_ready(void){ return g_layer != nil; }

long metal_frames(void){ return atomic_load(&g_frames_drawn); }
long metal_tris(void){ return atomic_load(&g_tris_drawn); }
long metal_tex_created(void){ return atomic_load(&g_tex_created); }
long metal_tex_uploaded(void){ return atomic_load(&g_tex_uploaded); }
int  metal_quit_requested(void){ return atomic_load(&g_should_quit); }

/* ---------------- cizim komut kuyrugu ----------------
 * Guest thread cizimleri buraya biriktirir; ana thread Present'te encode eder. */
#include "gfx.h"
typedef TVert MVert;   /* kirpma uzayi + iki renk + 4x4 doku koordinati */
/* target: 0 = ekran (drawable), degilse g_tex[target] offscreen hedefi.
 * is_clear=1 olan giris cizim degil, hedefi temizleme komutudur. */
/* blend: paketli karisim durumu (bkz. metal_blend_key)
 *   bit0      : alfa karisimi acik
 *   bit1-5    : kaynak carpani (D3DBLEND)
 *   bit6-10   : hedef carpani
 *   bit11-13  : karisim islemi (D3DBLENDOP)
 *   bit14-17  : renk yazma maskesi                                       */
/* vsid > 0 : kose gölgelendiricisi GPU'da kosuyor.  Bu durumda koseler
 * g_vbuf'ta degil HAM olarak g_rvbuf'ta (rawoff), sabitler ise g_kbuf'ta
 * (koff) durur; cizim vertexStart:0 ile tampon ofsetinden yapilir. */
/* ps_2_0 32 float sabit kullanabilir.  Dosya 8'de kesiliyordu ve daha
 * kotusu vsemu.c yuksek indisi 'idx & 7' ile KATLIYORDU: c8 sabitini
 * okuyan bir gölgelendirici c0'i goruyordu.  Post-process (bloom esigi,
 * bulanik dokunus agirliklari) tam da o araligi kullaniyor. */
#define PSC_N 32
typedef struct { unsigned first, count, tex, target, clear_argb, is_clear;
                 unsigned blend; float alpha_ref; unsigned depth;
                 unsigned tex1, tex2, tex3; int ps;
                 /* ps sabitleri karenin havuzunda (g_pscb[b][psci]); c0 ayrica
                  * (HD isin izleme malzemesi yalnizca onu okuyor) */
                 unsigned psci; float c0[4];
                 int vsid; unsigned rawoff, koff; float proj[4];
                 unsigned smp[4];
                 unsigned is_blit, blit_src; unsigned bcol; unsigned cull;
                 float fogv[4];
                 unsigned clr_flags; float clr_z; unsigned ksz;
                 /* indisli cizim (bkz. KALICI KOSE / INDIS HAVUZU): ix_isz 2/4 ise
                  * rawoff havuzdaki 0. kosenin ofseti, ix_ib ilk indisin ofseti;
                  * count indis sayisi.  gh: geometri kimligi (0 = icerikten ozet). */
                 unsigned ix_isz, ix_ib; int ix_base; unsigned long long gh; } MDraw;

#define MAXV  (256*1024)
#define MAXD  (16*1024)
#define MAXRV (16u*1024u*1024u)      /* ham kose arenasi (bayt) */
#define MAXKB ( 4u*1024u*1024u)      /* kose sabit arenasi (bayt) */
static MVert  *g_vbuf[2];
static MDraw  *g_dbuf[2];
static unsigned g_vn[2], g_dn[2];
static uint8_t *g_rvbuf[2];  static unsigned g_rvn[2];
static uint8_t *g_kbuf[2];   static unsigned g_kn[2];
static id<MTLBuffer> g_mtlrvb, g_mtlkb;
static unsigned g_ov_rv = 0, g_ov_kb = 0;   /* arena tasmasi sayaci */
/* --- kare butcesi olcumu: GPU gercekte ne kadar mesgul? --------------
 * Simdiye kadar yalnizca CPU tarafi olculuyordu; "neye donanim harciyoruz"
 * sorusunu GPU suresini bilmeden cevaplamak mumkun degil. */
static _Atomic unsigned long long g_gpu_ns = 0, g_gpu_n = 0, g_gpu_max_ns = 0;
static _Atomic unsigned long long g_enc_ns = 0;         /* kodlama (CPU) */
/* GPUWIN=a,b: ayni olcumler yalnizca [a,b) kareleri icin (menuler ortalamayi
 * sulandirmasin; ozellik basina GPU bedelini yaris penceresinde olcmek icin) */
static _Atomic unsigned long long g_gpuw_ns = 0, g_gpuw_n = 0, g_encw_ns = 0, g_encw_n = 0;
static long g_gpuw_a = -1, g_gpuw_b = -1;
static int gpuw_in(long f){
    if (g_gpuw_a == -1){ const char *e = ENVC("GPUWIN"); long a = 0, b = 0;
        if (e && sscanf(e, "%ld,%ld", &a, &b) == 2 && b > a){ g_gpuw_b = b; g_gpuw_a = a; } else g_gpuw_a = -2; }
    return g_gpuw_a >= 0 && f >= g_gpuw_a && f < g_gpuw_b;
}
/* ---- ADIM ADIM OLCUM (HD_RT_AB=1) ---------------------------------------
 * GPUWIN penceresini 100 karelik dilimlere boler; her dilimde isin izleme
 * boru hattinin bir kismi kapatilir ve dilim basina GPU suresi raporlanir.
 * Hava durumu (yagmur) rastgele: ayri kosularla karsilastirmak yerine AYNI
 * kosuda, ayni sahnede adimlarin bedelini verir.  Dilimin ilk 30 karesi
 * atilir (kuyruktaki onceki kipin kareleri). */
#define AB_N 7
static const char *g_ab_name[AB_N] = {
    "0 tam (isin izleme + birlestirme)", "1 izleme var, birlestirme yok", "2 duz izleme (golgeleme yok), birlestirme yok",
    "3 yakalama + yapi kurulumu", "4 yalniz yakalama", "5 isin izleme yok, SSR birlestirme", "6 isin izleme yok, birlestirme yok" };
static int g_ab_on = -1, g_ab_mode = -1;      /* g_ab_mode: bu karenin kipi (-1 = normal) */
static _Atomic unsigned long long g_ab_ns[AB_N], g_ab_cnt[AB_N];
static int ab_frame(long f, int *count_it){
    if (g_ab_on < 0){ const char *e = ENVC("HD_RT_AB"); g_ab_on = (e && *e == '1') ? 1 : 0; }
    *count_it = 0;
    if (!g_ab_on || !gpuw_in(f)) return -1;
    long r = f - g_gpuw_a;
    *count_it = (r % 100) >= 30;
    return (int)((r / 100) % AB_N);
}
/* ---- GECIS PROFILI -------------------------------------------------------
 * PASSPROF=N: GPUWIN penceresindeki her N. karede, her gecisin (render /
 * hesaplama / blit / ivme yapisi) GPU suresini Metal sayac ornekleyicisiyle
 * olc.  "GPU suresi neye gidiyor" sorusunu ozellik ac-kapa farkindan degil
 * DOGRUDAN cevaplar.  Render gecislerinde kose ve parca asamasi ayri gelir
 * (Apple GPU'lari karo tabanli: once tum kose isi, sonra parca isi).
 * Zaman damgalari GPU saatinde; iki noktali sampleTimestamps ile ns'ye. */
#include <mach/mach_time.h>
#define PP_MAXP 160
#define PP_MAXC 64
static id<MTLCounterSet> g_pp_set;
static int g_pp_every = -1;
static int g_pp_mode = 3;    /* PASSPROF_MODE: 1 = yalniz render gecisleri, 2 = yalniz hesaplama/blit/ivme, 3 = hepsi */
static pthread_mutex_t g_pp_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { char name[48]; double v, f; unsigned long long n; } g_pp_cat[PP_MAXC];
static int g_pp_ncat; static unsigned long long g_pp_frames;
static double g_pp_cb_s;                      /* ayni karelerin komut tamponu GPU suresi (sn) */
static MTLTimestamp g_pp_c0, g_pp_g0;
static id<MTLCounterSampleBuffer> g_pp_sb;    /* bu kare (yalniz kodlama thread'i) */
static int g_pp_np; static uint8_t g_pp_pc[PP_MAXP], g_pp_pk[PP_MAXP];
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
            if (!g_pp_every) fprintf(stderr, "[PASSPROF] bu GPU'da sayac ornekleme yok\n");
            else [g_dev sampleTimestamps:&g_pp_c0 gpuTimestamp:&g_pp_g0];
        }
    }
    if (g_pp_every <= 0 || !gpuw_in(fno) || fno % g_pp_every) return;
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
    { static int once = 0;            /* ilk profilli karenin gecis SIRASI */
      if (!once){ once = 1; fprintf(stderr, "  --- gecis sirasi (ilk profilli kare) ---\n");
          for (int k = 0; k < np; k++) fprintf(stderr, "  [sira %2d] %s\n", k, g_pp_cat[g_pp_pc[k]].name); } }
    uint8_t *pc = malloc((size_t)np * 2); memcpy(pc, g_pp_pc, (size_t)np); memcpy(pc + np, g_pp_pk, (size_t)np);
    [cb addCompletedHandler:^(id<MTLCommandBuffer> b2){
        NSData *dt = [sb resolveCounterRange:NSMakeRange(0, (NSUInteger)np * 4)];
        if (dt && dt.length >= (NSUInteger)np * 4 * sizeof(MTLCounterResultTimestamp)){
            const MTLCounterResultTimestamp *t = (const MTLCounterResultTimestamp *)dt.bytes;
            pthread_mutex_lock(&g_pp_lock);
            for (int k = 0; k < np; k++){
                uint64_t a = t[k*4].timestamp, b = t[k*4+1].timestamp, c = t[k*4+2].timestamp, d2 = t[k*4+3].timestamp;
                int ci = pc[k];
                if (a != MTLCounterErrorValue && b != MTLCounterErrorValue && b >= a) g_pp_cat[ci].v += (double)(b - a);
                if (!pc[np + k] && c != MTLCounterErrorValue && d2 != MTLCounterErrorValue && d2 >= c) g_pp_cat[ci].f += (double)(d2 - c);
                g_pp_cat[ci].n++;
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
    double sc = (g1 > g_pp_g0) ? ((double)(c1 - g_pp_c0) * tb.numer / tb.denom) / (double)(g1 - g_pp_g0) : 1.0;   /* ns / GPU tiki */
    double tot = 0; int ord[PP_MAXC];
    for (int i = 0; i < g_pp_ncat; i++){ ord[i] = i; tot += g_pp_cat[i].v + g_pp_cat[i].f; }
    /* Apple GPU'sunda sayac ornekleri dogrudan ns geliyor (sampleTimestamps ise mach tiki):
     * gecis toplami komut tamponu suresini asiyorsa olcek 1'dir. */
    if (tot * sc > g_pp_cb_s * 1e9 * 1.5) sc = 1.0;
    for (int i = 0; i < g_pp_ncat; i++) for (int j = i + 1; j < g_pp_ncat; j++)
        if (g_pp_cat[ord[j]].v + g_pp_cat[ord[j]].f > g_pp_cat[ord[i]].v + g_pp_cat[ord[i]].f){ int t = ord[i]; ord[i] = ord[j]; ord[j] = t; }
    double fr = (double)g_pp_frames;
    fprintf(stderr, "  --- gecis profili: %llu kare, gecis toplami %.3f ms/kare, komut tamponu %.3f ms/kare (olcek %.4f ns/tik) ---\n",
            g_pp_frames, tot * sc / fr / 1e6, g_pp_cb_s / fr * 1e3, sc);
    fprintf(stderr, "  %-44s %8s %8s %8s %7s\n", "gecis", "ms/kare", "kose", "parca", "adet/k");
    for (int q = 0; q < g_pp_ncat; q++){ int i = ord[q];
        fprintf(stderr, "  %-44s %8.3f %8.3f %8.3f %7.2f\n", g_pp_cat[i].name,
                (g_pp_cat[i].v + g_pp_cat[i].f) * sc / fr / 1e6, g_pp_cat[i].v * sc / fr / 1e6,
                g_pp_cat[i].f * sc / fr / 1e6, (double)g_pp_cat[i].n / fr); }
}
static _Atomic unsigned long long g_pass_n = 0, g_draw_n = 0;
/* Render hedefi = doku kimligi | ((yuz+1) << 16).  Kup dokuda oyun ayni
 * dokunun ALTI YUZUNE ayri ayri ciziyor; yuz olmadan altisi ust uste
 * ayni yuzeye gidiyordu. */
#define TGT_ID(t)   ((t) & 0xFFFFu)
#define TGT_FACE(t) (((t) >> 16) & 7u)          /* 0 = yuz yok */
static unsigned g_cur_target = 0;          /* guest'in etkin render hedefi */
static unsigned g_cur_blend  = 0;
static unsigned g_cur_bcol   = 0xFFFFFFFFu;   /* D3DRS_BLENDFACTOR (ARGB) */
static unsigned g_cur_cull   = 3u;            /* D3DRS_CULLMODE: 1=NONE 2=CW 3=CCW */
static unsigned g_cur_depth  = 1u << 8; /* bit0=test, bit1=yazma, bit2-4=islev,
                                          * bit8+ = bagli Z yuzeyi (DEPTH_ZID) */
static unsigned g_cur_dmode  = 0;
static unsigned g_cur_zid    = 1;       /* 1 = cihazin ortuk Z yuzeyi */
static unsigned g_cur_tex1 = 0, g_cur_tex2 = 0, g_cur_tex3 = 0;
static int      g_cur_ps = 0;              /* etkin piksel gölgelendirici */
static float    g_cur_psc[PSC_N][4];       /* ps sabitleri c0..c(PSC_N-1) */
/* Kare basina ps sabiti havuzu.  Eskiden her cizim kaydi 32 sabitin tamamini
 * (512 bayt) tasiyordu: kopya + sifirlama + kodlamada bir kopya ve karsilastirma
 * daha, cizim basina ~2 KB.  Olculdu: cizimlerin %92'si bir oncekiyle AYNI
 * sabitleri kullaniyor (oyun en fazla c3 yaziyor).  Artik sabitler yalnizca
 * degistiginde havuza eklenir, kayit sira numarasini tasir. */
static float    (*g_pscb[2])[PSC_N][4];
static unsigned g_pscn[2], g_psc_last[2], g_psc_lastver[2];
static unsigned g_psc_ver = 1;              /* g_cur_psc her degistiginde artar */
static unsigned psc_put(int b){              /* g_qlock altinda */
    if (g_pscn[b] && g_psc_lastver[b] == g_psc_ver) return g_psc_last[b];
    if (g_pscn[b] >= MAXD) return g_psc_last[b];
    memcpy(g_pscb[b][g_pscn[b]], g_cur_psc, sizeof g_cur_psc);
    g_psc_last[b] = g_pscn[b]++; g_psc_lastver[b] = g_psc_ver;
    return g_psc_last[b];
}
/* Asama basina projeksiyon kipi: 0 = yok, 3 = xy/z, 4 = xy/w.
 * D3DTSS_TEXTURETRANSFORMFLAGS | D3DTTFF_PROJECTED. */
static float    g_cur_proj[4];
static unsigned g_cur_smp[4] = {0,0,0,0};
/* Oyunun D3DSAMP_* durumu.  Daha once TEK bir global ornekleyici vardi
 * (linear/linear/mip-linear, anizotropi yok).  Oyun aslinda
 * MINFILTER=ANISOTROPIC + MAXANISOTROPY=16 istiyor; izotropik trilinear
 * verince ucgen KENARLARINDA turev tahmini siciriyor, bir seviye kaba mip
 * seciliyor ve her dortgenin kenarlarinda + kosegeninde 1 piksellik cizgi
 * olusuyor (harflerin uzerinde en belirgin).                            */
void metal_set_samplers(const unsigned k[4]){
    for (int i = 0; i < 4; i++) g_cur_smp[i] = k[i];
}
#if NFS_ENHANCED
#define MAXPS 2560          /* + yansima varyantlari: ps + PSREFL*(1+asama) */
#else
#define MAXPS 512
#endif
#define PSCUBE 256          /* kup varyanti: psid + PSCUBE */
#define PSREFL 512          /* (HD) yansima ayristirma varyanti */
static id<MTLFunction> g_psfn[MAXPS];      /* ceviriden derlenmis ps */
static id<MTLTexture>        g_depth_tex;  /* Z yuzeyi YOKKEN kullanilan karalama */
/* --- Z yuzeyleri ------------------------------------------------------
 * Eskiden tek bir Z dokusu vardi ve hedefin boyutu her degistiginde
 * YENIDEN yaratiliyordu.  Oyun bir karede 320x240 (isik/parlama), 6 x
 * 128x128 (yansima kupu), ekran ve 640x480 (bulaniklik) hedefleri arasinda
 * gidip geliyor; ekrana her donuste Z yeni (tanimsiz) bir dokudan
 * yukleniyordu -> sehir isiklari ve bazi arabalar duvarin arkasindan
 * gorunuyordu.  Artik D3D'deki gibi: her Z yuzeyinin kendi dokusu var,
 * SetDepthStencilSurface hangisinin bagli oldugunu, Clear(ZBUFFER) ne
 * zaman temizlenecegini belirliyor; icerik kareler arasi korunuyor. */
#define MAXZ 64
#define DEPTH_ZID(d) ((d) >> 8)
static id<MTLTexture> g_ztex[MAXZ];
static unsigned char  g_zpend[MAXZ];        /* bir sonraki geciste temizle */
static float          g_zpend_v[MAXZ];
static _Atomic unsigned long long g_z_mismatch = 0, g_z_null = 0;
static _Atomic unsigned char g_zforget[MAXZ];  /* guest yeniden yaratti: dokuyu at */
void metal_z_forget(unsigned zid){ if (zid < MAXZ) atomic_store(&g_zforget[zid], 1); }
static id<MTLDepthStencilState> g_dss[64];
static float    g_cur_aref   = 0.f;
/* D3DRS_FOGENABLE(28) + D3DRS_FOGCOLOR(34).  Sis HIC uygulanmiyordu: kose
 * gölgelendiricisinin oFog cikisi MSL'de "dummy"ye gidiyordu.  Oyunun 18
 * kose gölgelendiricisinden 6'si oFog yaziyor ve FOGENABLE 5.3M kez
 * kuruluyor -> uzak geometri hic solmuyordu. (r,g,b,acik) */
static float    g_cur_fog[4] = {0.f,0.f,0.f,0.f};
static int      g_cur = 0;                 /* guest'in yazdigi tampon */
static int      g_ready = -1;              /* ana thread'in cizecegi tampon */
static pthread_mutex_t g_qlock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_qcond = PTHREAD_COND_INITIALIZER;
/* g_qcond: ana thread -> konuk ("kareni aldim").
 * g_rcond: konuk -> ana thread ("kare hazir").  Eskiden ana thread
 * 4 ms'lik nanosleep ile YOKLUYORDU; kare suresi histogrami tam 5 ms
 * adimlarla kuantalanmisti (15.5/20.5/25.5/30.5 ms, aralari BOS).
 * Konuk kareyi bitirdikten sonra ana thread onu alana kadar 0-5 ms
 * bosa bekliyordu.  Artik uyandirma aninda. */
static pthread_cond_t  g_rcond = PTHREAD_COND_INITIALIZER;
static _Atomic long    g_frames_dropped = 0;
/* Ana thread'in su an KODLADIGI tampon (-1 = yok).  metal_present zaman
 * asimina ugrasa bile bu tampona takas YAPAMAZ: ana thread g_dbuf[rb]'yi
 * kilitsiz geziyor; konuk ayni tampona yeni karenin cizimlerini yazarsa
 * kare yarim kalir ya da yeni cizimler eski kose verisiyle cizilir. */
static int g_enc_rb = -1;
static _Atomic unsigned long g_cb_err = 0;         /* GPU'da hatayla biten kare */
static _Atomic unsigned long g_enc_late = 0;       /* 60 ms dolarken kodlama suruyordu */
static long g_enc_late_f[8];                       /* ilk birkacinin kare numarasi */

static id<MTLRenderPipelineState> g_pso;
static id<MTLLibrary>             g_lib;
static MTLVertexDescriptor       *g_vdesc;
static id<MTLBuffer>              g_mtlvb;
/* --- kare basina tampon halkasi ----------------------------------------
 * g_mtlvb / g_mtlrvb / g_mtlkb eskiden TEK'ti: her karenin kose ve sabit
 * verisi ayni paylasimli tampona kopyalaniyordu.  [cb commit] GPU'yu
 * BEKLEMEZ; sonraki karenin memcpy'si GPU onceki karenin kose asamasini
 * calistirmadan gelirse, o karenin cizimleri (first/rawoff/koff ESKI
 * yerlesime gore) YENI karenin baytlarini okur.  Karenin sonunda cizilen
 * ekran-uzayi isler (son islem + HUD) ilk kurban: ekrani kaplayan dev
 * ucgenler, ustlerinde font atlasi; dunya saglam (kullanici fotograflari,
 * 2026-10-13).  Olculdu: yarista 9000 karenin 6472'sinde tampon, onceki
 * kare GPU'da bitmeden eziliyordu; 1046'sinda yerlesim de kaymisti.
 * Artik her kare GPU'su BITMIS bir takim alir; tamamlanma isleyicisi geri
 * verir.  Dogruluk takim SAYISINA degil semafora bagli: hepsi ucustaysa
 * fb_acquire GPU'yu bekler.  Uc takim olcume dayaniyor: yarista 8000 kare,
 * iki kosu, 0 bekleme (2+ kare ayni anda ucusta: gorunur pencerede 6/9000,
 * saydam test penceresinde 2292/7600 -- ucuncu takim bunu karsiliyor).
 * Tek istisna makine uykudayken (kapak kapali, DarkWake): GPU durdugu icin
 * 7600 karede 3138 bekleme goruldu; o kosularin zamanlamasi gecersiz. */
#define NFB 3
static id<MTLBuffer>         g_fb_vb[NFB], g_fb_rvb[NFB], g_fb_kb[NFB];
static _Atomic int           g_fb_busy[NFB];
static dispatch_semaphore_t  g_fb_sem;
static _Atomic unsigned long g_fb_overlap = 0, g_fb_wait = 0;
static _Atomic unsigned long long g_fb_wait_ns = 0;
static int fb_acquire(void){
    if (dispatch_semaphore_wait(g_fb_sem, DISPATCH_TIME_NOW) != 0){
        struct timespec w0, w1; clock_gettime(CLOCK_MONOTONIC, &w0);
        atomic_fetch_add(&g_fb_wait, 1ul);          /* hepsi GPU'da: biri bitene kadar bekle */
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
    if (any) atomic_fetch_add(&g_fb_overlap, 1ul);  /* eskiden bu karede tampon ezilirdi */
    return got - 1;                                 /* semafor en az bir bos takim garanti eder */
}
static void fb_release(int i){
    atomic_store(&g_fb_busy[i], 0);
    dispatch_semaphore_signal(g_fb_sem);
}
/* Takim tamponlari sabit MAXV/MAXRV/MAXKB boyunda DEGIL, karenin ihtiyacina
 * gore buyur.  Paylasimli tampon GPU'da bir kez kullanildi mi TAMAMI ayak
 * izine yazilir (olculdu: 3 x 28 MB tamponun yalnizca 64 KB'i doluyken
 * +85 MB); sabit boyda uc takim 147 MB tutuyordu, oysa CPU kose yolu yarista
 * kare basina ~400 kose (45 KB) tasiyor.  Takim fb_acquire'dan BOS geldigi
 * icin eski tampon hicbir komut tamponunda degil; degistirmek guvenli. */
#define FB_STEP (256u * 1024u)
static _Atomic unsigned long g_fb_grow = 0;
static id<MTLBuffer> fb_fit(id<MTLBuffer> b, NSUInteger need, NSUInteger maxlen){
    if (b && b.length >= need) return b;
    NSUInteger cap = need + need / 4;               /* pay: her karede yeniden yaratma */
    cap = (cap + FB_STEP - 1) / FB_STEP * FB_STEP;
    if (cap < FB_STEP) cap = FB_STEP;
    if (cap > maxlen) cap = maxlen;
    id<MTLBuffer> nb = [g_dev newBufferWithLength:cap options:MTLResourceStorageModeShared];
    if (!nb) return b;                              /* fb_put eskisine sigdigi kadar yazar */
    if (b) atomic_fetch_add(&g_fb_grow, 1ul);
    return nb;
}
static void fb_put(id<MTLBuffer> b, const void *src, NSUInteger n){
    if (b && n) memcpy([b contents], src, n < b.length ? n : b.length);
}
/* --- KALICI KOSE / INDIS HAVUZU -------------------------------------------
 * Oyunun duragan (kilitlenmeyen) kose ve indis tamponlari GPU'da BIR KEZ
 * yansitilir; indisli cizimler oradan drawIndexedPrimitives ile cizilir.
 * Eskiden her karede indisler konuk tarafinda acilip (ara dizi), arenaya,
 * sonra Metal tamponuna kopyalaniyordu: 3 x 2.7 MB/kare, ~1 ms CPU; koseler
 * x2.1 cogaldigi icin GPU kose asamasi da (4K HD'de gecis suresinin ~%19'u)
 * iki kat calisiyordu.  Olculdu (2026-10-14): kaynak VB'lerin %99.6'si 120+
 * karedir kilitlenmemis.
 * Havuz TEK tampon: HD yakalama cekirdegi tum kayitlari tek baglamayla
 * okuyabilsin.  Yansilar yalnizca konuk thread'inde kurulur/dusurulur; ana
 * thread karenin teslim anindaki havuz nesnesini baglar (buyume eskisini
 * kopyalayip yenisine gecer, ofsetler degismez).  Bosalan bloklar ucustaki
 * kareler bitsin diye PF_DELAY kare sonra yeniden kullanilir. */
#define MIR_N     4096u             /* yansi tablosu (acik adresleme, 2^n) */
#define MIR_TOMB  1u                /* silinmis yuva (konuk adresi asla 1 olmaz) */
#define PF_N      4096
#define PF_DELAY  8u                /* kare */
#define MIR_IDLE  900u              /* bu kadar kare cizilmeyen yansi duser */
#define POOL_MAX  (768u << 20)
static id<MTLBuffer> g_pool;        /* guncel havuz (degisimi g_qlock altinda) */
static id<MTLBuffer> g_pool_fr[2];  /* karenin teslim anindaki havuz */
static uint32_t g_pool_top = 0;
static struct { uint32_t key, gen, off, size, imax, used; } g_mir[MIR_N];
static struct { uint32_t off, size, at; } g_pf[PF_N];
static int g_pfn = 0;
static uint32_t g_gframe = 0;       /* konuk kare sayaci (metal_present) */
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
static void mir_sweep(void){                 /* uzun suredir cizilmeyen yansilari dusur */
    for (unsigned i = 0; i < MIR_N; i++){
        if (g_mir[i].key <= MIR_TOMB || g_gframe - g_mir[i].used < MIR_IDLE) continue;
        pool_free(g_mir[i].off, g_mir[i].size);
        g_mir[i].key = MIR_TOMB; g_mir_drop++; g_mir_live--;
    }
}
/* Konuk: guest nesnesinin (key) bu uretiminin (gen) havuzdaki ofseti; yoksa
 * ya da eskiyse simdi kurulur.  isz > 0 ise indis tamponudur ve *imax tum
 * tampondaki en buyuk indis olur.  0 = kurulamadi (cagiran eski yola duser). */
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
/* Konuk: havuzdaki yansinin CPU gorunumu (indis araligi taramasi icin). */
const uint8_t *metal_pool_cpu(void){ return g_pool ? (const uint8_t *)[g_pool contents] : NULL; }
/* Ana thread: cizimin j. kosesinin ham baytlari (HD isin izleme CPU okumalari). */
static const uint8_t *dr_vtx(int rb, const MDraw *dr, unsigned j, unsigned stride){
    if (!dr->ix_isz) return g_rvbuf[rb] + dr->rawoff + (size_t)j * stride;
    const uint8_t *P = (const uint8_t *)[g_pool_fr[rb] contents];
    uint32_t ix = dr->ix_isz == 4u ? ((const uint32_t *)(P + dr->ix_ib))[j]
                                   : ((const uint16_t *)(P + dr->ix_ib))[j];
    return P + dr->rawoff + (size_t)((int64_t)ix + dr->ix_base) * stride;
}
static id<MTLBuffer>              g_blitvb;   /* StretchRect icin tam ekran dortgen */
static id<MTLSamplerState>        g_samp;

/* --- doku tablosu --- */
#define MAXTEX 16384   /* uzun oturum + cok Reset: her Reset RT'leri yeniden yaratir */
static uint8_t g_tex_cube[MAXTEX];          /* 1 = MTLTextureTypeCube */
static unsigned g_tex_lvlmask[MAXTEX];
static id<MTLTexture> g_tex[MAXTEX];
static unsigned       g_texn = 1;            /* 0 = "doku yok" (beyaz) */
/* --- Reset'te render hedeflerini birakmak --------------------------------
 * D3D9'da Reset'ten once oyun TUM D3DPOOL_DEFAULT kaynaklarini (render
 * hedefleri dahil) birakmak zorunda, yoksa Reset basarisiz olur.  Biz
 * Release'te Metal dokusunu hic birakmiyorduk: her cozunurluk degisimi
 * ~6 render hedefi sizdiriyordu (4K'da degisim basina >100 MB).  Reset
 * aninda o ana kadar yaratilmis render hedefleri olu kabul edilir ve
 * ekrandaki kare(ler) bitince ana thread'de birakilir. */
static uint8_t g_tex_isrt[MAXTEX];
static _Atomic unsigned g_rtfree_upto = 0;    /* bu kimlikten KUCUK RT'ler olu */
static unsigned         g_rtfree_done = 1;    /* buraya kadar birakildi */
static _Atomic long     g_rtfree_at = 0;      /* bu kareden sonra birak */
static unsigned long    g_rt_freed = 0;
static uint8_t          g_tex_freed[MAXTEX];      /* tani: birakildi */
static unsigned short   g_freed_w[MAXTEX], g_freed_h[MAXTEX];
static unsigned long    g_freed_use_smp = 0, g_freed_use_tgt = 0;
static void freed_use(unsigned id_, int as_tgt){
    if (id_ >= MAXTEX || !g_tex_freed[id_]) return;
    if (as_tgt) g_freed_use_tgt++; else g_freed_use_smp++;
    static int shown = 0;
    if (shown < 12){ shown++;
        fprintf(stderr, "metal: UYARI birakilmis RT %s olarak kullanildi: id=%u %ux%u%s\n",
                as_tgt ? "HEDEF" : "doku", id_, g_freed_w[id_], g_freed_h[id_],
                g_tex_cube[id_] ? " (kup)" : ""); }
}
static id<MTLTexture> g_white;

/* D3DFMT -> MTLPixelFormat; blok boyutu ve blok basina bayt dondurur */
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

unsigned metal_create_texture_ex(unsigned w, unsigned h, unsigned levels,
                                 unsigned d3dfmt, unsigned is_rt);
/* Yeni doku icerigi TANIMSIZDIR: TUM mip seviyeleri ve (kupte) TUM yuzler
 * seffaf siyahla temizlenir.  Eskiden yalniz 2B'nin 0. seviyesi siliniyordu,
 * kup hic silinmiyordu.  Ana menude araba cevre haritasi (256/128 kup RT)
 * HIC cizilmiyor ama govde onu ornekliyor (ps 265, asama 0): Reset
 * hedefleri birakinca Metal o bellegi yeni dokulara verebiliyor ve kup
 * eski dokunun copunu tasiyordu (kasitli copla dogrulandi: govdede gurultu). */
static void tex_zero_all(id<MTLTexture> t, unsigned w, unsigned h, unsigned levels,
                         unsigned faces, unsigned blk, unsigned bpb){
    if (ENVC("NOTEXZERO")) { if (faces == 1) levels = 1; else return; }  /* tani: eski davranis */
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
    if (!g_dev || g_texn >= MAXTEX) return 0;
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
    /* Yeni doku icerigi tanimsizdir; seffaf siyahla temizle ki yuklenmemis
     * dokular ekranda cop olarak gorunmesin (ornek: render target'lar). */
    tex_zero_all(t, w, h, levels, 1, blk, bpb);
    unsigned id_ = g_texn++;
    g_tex[id_] = t;
    g_tex_isrt[id_] = is_rt ? 1 : 0;
    atomic_fetch_add(&g_tex_created, 1); if (g_perflog) perflog_event(PL_TEX, 0, 0);
    return id_;
}
/* Guest (Reset): simdiye kadarki render hedefleri olu. */
void metal_reset_rts(void){
    if (ENVC("NORTFREE")) return;        /* tani: birakmayi kapat */
    atomic_store(&g_rtfree_upto, g_texn);
    atomic_store(&g_rtfree_at, atomic_load(&g_frames_drawn) + 3);
}
/* Ana thread, kare sonunda: zamani geldiyse olu RT'leri birak. */
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
/* HD: yol yansimasi hedefleri (bkz. enhance.c "YOL YANSIMASI BULANIKLIGI").
 * Mip zinciriyle yaratiliyorlar; bir bulaniklik cizimi bu dokulardan birini
 * orneklemeden once, son cizimden beri kirlenmisse mip'leri uretilir. */
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

/* HD: ekran icin MSAA (HD_MSAA=4 varsayilan, 0/1 kapali).
 * Yalnizca EKRAN gecisleri cok ornekli: oyunun sahnesi ve arayuzu dogrudan
 * arka tampona ciziliyor.  Render hedefleri (yansima, son-islem, kup)
 * tek ornekli kalir.  Her ekran gecisi StoreAndMultisampleResolve ile
 * kapanir; boylece StretchRect'in arka tampondan okudugu drawable her an
 * cozulmus goruntuyu tasir.  Z icin her yuzeyin cok ornekli bir IKIZI
 * tutulur (icerikleri paylasilmaz; bkz. g_hd_zshare sayaci). */
static int g_hd_msn = -1;
static id<MTLTexture> g_hd_mscol, g_hd_msdepth_scratch;
static id<MTLTexture> g_hd_msz[64];
/* --- BELLEKSIZ MSAA EKLERI -------------------------------------------------
 * Cok ornekli renk / G-tamponu / derinlik eskiden hep Private'ti: 4K'da
 * ~930 MB (renk 133 + G 664 + Z 133).  Oysa olculen her karede TEK bir cok
 * ornekli ekran gecisi var ve renk/G ornekleri hic bellege yazilmiyor,
 * yalnizca tek ornekliye cozuluyor (hd_ms_end).  Apple GPU'sunda boyle bir
 * ek MTLStorageModeMemoryless olabilir: karo bellekte yasar, hic bellek
 * ayrilmaz.  Kosul: ek YUKLENMEZ (Clear/DontCare) ve SAKLANMAZ.  Gecis
 * acilirken hd_ms_memless_ok ileriye bakarak karar verir; tutmayacaksa
 * (bolunmus gecis, ekrana kopya) gecis Private ikizlerle acilir -- onlar
 * artik yalnizca gerektiginde yaratilir.  Kapanista ongoru tutmadiysa
 * sayilir ve belleksiz kip kalici olarak kapanir.  HD_MSMEMLESS=0 kapatir. */
static id<MTLTexture> g_hd_mscol_ml, g_hd_msz_ml;
static int g_hd_ml_off = 0;                    /* ongoru tutmadi: kalici kapali */
static _Atomic unsigned long long g_hd_ml_pass, g_hd_ml_miss;
/* MS derinligin saklanmasi kapanista kararlastirilir (hd_ms_end).  Bir Z'nin
 * son cok ornekli gecisi saklamadiysa ornekleri kayiptir: temizlemeden
 * yuklemek isteyen sonraki gecis temizlenerek acilir ve sayilir. */
static uint8_t g_hd_msz_seen[64], g_hd_msz_kept[64];
static _Atomic unsigned long long g_hd_mszst, g_hd_mszskip, g_hd_mszlost;
/* acik cok ornekli ekran gecisi (yalnizca ana thread): dz = MS derinlik bagli
 * ve saklamasi kapanista secilecek; zclr = derinlik temizlenerek acildi;
 * res = tek ornekli ikize cozum bagli */
static struct { int on, ml, dz, zclr, res; unsigned zid; } g_hd_msp;
/* ekran kopyasini cok ornekli dokuya cizen boru hatti, log2(ornek) basina:
 * tanim acilista saklanir, sayi degisince (COZUNURLUGE GORE MSAA) kurulur */
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
static uint8_t g_hd_zms_used[64];             /* bu karede ekranda kullanildi */
static uint8_t g_hd_zpend_ms[64];             /* ikizin bekleyen Clear(Z)'si */
static float   g_hd_zpend_msv[64];
static _Atomic unsigned long long g_hd_mspass, g_hd_msblit, g_hd_zshare, g_hd_zrev, g_hd_zres;
static uint8_t g_hd_zsdirty[64];              /* tek ornekli ikize ekran disinda Z yazildi */
static int hd_msaa_n(void){
    if (g_hd_msn < 0){
        const char *e = ENVC("HD_MSAA");
        int n = (e && *e) ? atoi(e) : 4;
        if (n != 2 && n != 4 && n != 8) n = 1;
        if (n > 1 && g_dev && ![g_dev supportsTextureSampleCount:(NSUInteger)n]) n = 1;
        g_hd_msn = n;
        fprintf(stderr, "[HD] MSAA: %s\n", n > 1 ? (n == 8 ? "8x" : n == 4 ? "4x" : "2x") : "KAPALI");
    }
    return g_hd_msn;
}
/* --- COZUNURLUGE GORE MSAA -------------------------------------------------
 * 1440 satir ve ustunde 2x, altinda 4x (kullanici karari 2026-10-14: 1440p ve
 * Mac'in kendi ekran cozunurlugunde).  Bu piksel yogunlugunda 4x'in kenar
 * kazanci kucuk; bedeli cok ornekli sahne gecisinin karo bellegi ve cozumu
 * (4K'da bu gecis GPU suresinin ~%44'u; ne kadari ornek sayisindan, olculmedi).
 * Her kare ekran boyutuna bakilir;
 * sayi degisince MS dokular (orneksayisi denetimi) ve boru hatlari (anahtarda
 * log2(ornek)) kendiliginden yenilenir.  HD_MSAA verilirse sayi sabittir;
 * HD_MSLAZY=0'da da (oradaki tekil boru hatlari acilista kuruluyor). */
static int hd_mslazy(void);
static int hd_msaa_l2(void){ int n = hd_msaa_n(); return n >= 8 ? 3 : n >= 4 ? 2 : n >= 2 ? 1 : 0; }
static void hd_msaa_frame(NSUInteger w, NSUInteger h){
    static int aut = -1;
    if (aut < 0){ const char *e = ENVC("HD_MSAA"); aut = !(e && *e); }
    if (!aut || hd_msaa_n() <= 1 || !hd_mslazy()) return;
    int want = (h >= 1400u) ? 2 : 4;
    if (want == g_hd_msn || ![g_dev supportsTextureSampleCount:(NSUInteger)want]) return;
    g_hd_msn = want;
    fprintf(stderr, "[HD] MSAA: %dx (cizim %lux%lu; 1440 satir ve ustu 2x)\n",
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
                                   NSUInteger w, NSUInteger h){   /* bkz. BELLEKSIZ MSAA EKLERI */
    if (old && old.width == w && old.height == h && old.sampleCount == (NSUInteger)hd_msaa_n()) return old;
    MTLTextureDescriptor *td = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:pf width:w height:h mipmapped:NO];
    td.textureType = MTLTextureType2DMultisample;
    td.sampleCount = (NSUInteger)hd_msaa_n();
    td.usage = MTLTextureUsageRenderTarget;
    td.storageMode = MTLStorageModeMemoryless;
    return [g_dev newTextureWithDescriptor:td];
}
static int hd_ml_hw(void){                     /* belleksiz doku destegi + HD_MSMEMLESS */
    static int v = -1;
    if (v < 0){ const char *e = ENVC("HD_MSMEMLESS");
        v = !(e && *e == '0') && g_dev && [g_dev supportsFamily:MTLGPUFamilyApple1]; }
    return v;
}

/* HD: ana gorunumun kamerasi, KARE basina (guest yazar, ana thread okur).
 * 48 float: gorunum (16), projeksiyon (16), carpim (16); D3D satir vektoru
 * duzeni (v' = v * M).  Kaynak: overrides.c sub_005c7ed0. */
static float g_hd_cam[2][48];
static float g_hd_cam_k[2];                 /* Hor+ kirpma x carpani */
static float g_hd_cube[2][9];               /* kup +x,+y,+z eksenlerinin dunya yonleri */
static int   g_hd_cam_ok[2];
void metal_hd_set_camera(const float *m48, float k, const float *cube9){
    pthread_mutex_lock(&g_qlock);
    memcpy(g_hd_cam[g_cur], m48, sizeof g_hd_cam[0]);
    memcpy(g_hd_cube[g_cur], cube9, sizeof g_hd_cube[0]);
    g_hd_cam_k[g_cur] = k;
    g_hd_cam_ok[g_cur] = 1;
    pthread_mutex_unlock(&g_qlock);
}

/* ===== HD: SSR (ekran uzayi yansima) =====================================
 * Yansiyan yuzeyler (yol: yol yansimasi dokusunu ornekleyen ps 5/6) ana
 * sahne gecisinde G-tamponuna kendi yansima agirligini B ve orijinal
 * yansimayi R0 yazar (vsemu.c ps_to_msl_ex2).  Sahne biter bitmez (ekrandan
 * ilk StretchRect) birlestirme gecisi derinlik tamponunda isin yurutur ve
 * isabet varsa renk + B*(R_yeni - R0) yazar.  Isabet yoksa R0 kalir
 * (oyunun duzlemsel yansimasi: ekran disi icerik icin dogru yedek).
 * Ileride R_yeni isin izlemeden gelecek; G-tamponu ve birlestirme ayni.
 * Ortam: HD_SSR=0 kapali, HD_SSR_STEPS=48, HD_SSR_MAXD=120 (metre). */
static int g_hd_ssr = -1;
#define HD_NG 3   /* G-tamponu: uc hedef, bkz. G-TAMPONU PAKETLEME */
/* --- G-TAMPONU PAKETLEME ---------------------------------------------------
 * Eski duzen (HD_GPACK=0): B+sinif RGBA16F, R0 RGBA8, yon RGBA16F = 20 bayt.
 * Paketli (varsayilan) 12 bayt, yine uc hedef ve hepsi MSAA'da cozulebilir:
 *   G0 RGBA8Unorm   R0.rgb + sinif/4.  Sinif 8 bitte kaldigi icin kenar
 *                   ortalamalari eskisi gibi ayiklanir (GB_ROAD/GB_CAR).
 *   G1 RGB10A2Unorm B/4: 0..4 araligi, adim 0.004.  Yolda B = doku alfasi x
 *                   kose alfasi <= 1 (GRAFIK_ARASTIRMA.md 8); araba boyasininki
 *                   olculmedi.  4'u asarsa kirpilir: yansima zayiflar, bozulmaz.
 *   G2 RGB10A2Unorm birim yon, kod = 512 + 511*d (512 = tam sifir).  Dogrusal
 *                   kodlama cozumde ortalamayi korur: kenarda sifirla
 *                   ortalanan yon kisalir ama ayni yonu gosterir (sekizyuzlu
 *                   kodlama bunu bozardi).
 * Olculdu (2026-10-14, oyunsuz GPU sinamasi: ayni rastgele ornekler iki duzene
 * 2x ve 4x yazilip donanimla cozuldu, bu makrolarla okundu): sinif kararlari
 * birebir ayni; B*(R_yeni - R0) farki eski duzene gore en cok 1 LSB (medyan
 * 0.3); yon farki tekduze pikselde en cok 0.105 derece.
 * Okuyucular ayni indislerle baglanir (G0 2, G1 3, G2 4); kod cozumu GB_*
 * makrolarinda (hd_gb_src), yazan vsemu.c ps_to_msl_ex2.  Kazanc: cok
 * ornekli karo bellegi renk + G ornek basina 24 -> 16 bayt; her karedeki
 * cozum yazimi, birlestirme okumalari ve tek ornekli G dokulari %40 az
 * (3024x1964'te 119 -> 71 MB). */
static int hd_gpack(void){
    static int v = -1;
    if (v < 0){ const char *e = ENVC("HD_GPACK"); v = (e && *e == '0') ? 0 : 1;
        fprintf(stderr, "[HD] G-tamponu: %s\n", v ? "paketli (12 bayt/ornek)" : "eski (20 bayt/ornek)"); }
    return v;
}
static MTLPixelFormat hd_gfmt(int gi){
    static const MTLPixelFormat old[HD_NG] = { MTLPixelFormatRGBA16Float, MTLPixelFormatRGBA8Unorm,
                                               MTLPixelFormatRGBA16Float };
    static const MTLPixelFormat pk[HD_NG]  = { MTLPixelFormatRGBA8Unorm, MTLPixelFormatRGB10A2Unorm,
                                               MTLPixelFormatRGB10A2Unorm };
    return hd_gpack() ? pk[gi] : old[gi];
}
/* G-tamponunu okuyan her gölgelendiricinin basina */
static NSString *kGBDec = @
"#if GB_PACK\n"
"#define GB_CLS(g0)    ((g0).a * 4.0)\n"
"#define GB_R0(g0, g1) ((g0).rgb)\n"
"#define GB_B(g0, g1)  ((g1).rgb * 4.0)\n"
"#define GB_DIR(g2)    (((g2).rgb * 1023.0 - 512.0) * (1.0 / 511.0))\n"
/* 8 bitlik sinifin yuvarlamasi (<= 0.008) sinirdaki ortalamalari icine almasin */
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
        fprintf(stderr, "[HD] SSR: %s\n", g_hd_ssr ? "ACIK" : "KAPALI"); }
    return g_hd_ssr;
}
/* Cizim aninda (konuk thread): ekrana cizen ve yol yansimasi dokusunu
 * ornekleyen ps icin ayristirma varyantina gec. */
int metal_tex_is_cube(unsigned id_);
static int hd_refl_ps(int ps, unsigned target, const unsigned tx[4]){
    if (ps <= 0 || ps >= PSREFL || target || !hd_ssr_on()) return ps;
    int k = -1;
    for (int q = 0; q < 4; q++) if (hd_is_road(tx[q])){ k = q; break; }
    /* araba boyasi: 0. asamada kup harita (ps zaten kup varyanti) */
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
    return g_psfn[vid] ? vid : ps;
}
/* 4x4 tersi, satir-oncelikli.  Basarisizsa 0. */
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
/* prm = (A, Bc, guc, azami mesafe), sz = (W, H, adim, kalinlik) */
"static float lin_z(float d, constant SSRP &S) { return S.prm.y / (d - S.prm.x); }\n"
"fragment float4 hd_ssr_frag(FSO in [[stage_in]],\n"
"    texture2d<float> scene [[texture(0)]], depth2d<float> dep [[texture(1)]],\n"
"    texture2d<float> g1 [[texture(2)]], texture2d<float> g2 [[texture(3)]],\n"
"    texture2d<float> g3 [[texture(4)]],\n"
"    constant SSRP &S [[buffer(0)]]) {\n"
"  uint2 px = uint2(in.pos.xy);\n"
"  float4 c = scene.read(px);\n"
"  float4 gb = g1.read(px);\n"
"  int dbg = int(S.cam.w) - 1;                             /* 0 normal, 1 isabet, 2 yon */\n"
"  float cls = GB_CLS(gb);\n"
"  bool road = GB_ROAD(cls);\n"
"  bool car  = GB_CAR(cls);                                /* kenar ortalamalarini ele */\n"
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
/* N = normalize(R - V): yuzeyden hafifce ayril, kendine carpma */
"    float3 N = normalize(Rd - V);\n"
"    if (dbg == 3) {\n"                 /* N, derinlikten gelen geometrik normalle uyusuyor mu */
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
/* ===== HD: ISIN IZLEME SURE OLCUMU (HD_RTPROBE=1, DUMPFRAME karesinde) =====
 * Soru: bu makinede sahneyi her karede isin izleme yapisina koyup yansima
 * isini atmak NE KADAR SURER?  Bir kez calisir, rapor basar, iki PNG yazar.
 *   1) Yakalama (CPU): ana sahnenin cizimleri dunya uzayina.  Kose
 *      gölgelendiricisi CALISTIRILMAZ: oPos = p * WVP oldugundan
 *      dunya = p * (WVP * (VP')^-1); WVP cizimin c[cx..cx+3] sabitleri.
 *   2) Kurulum (GPU): tum sahne tek ilkel ivme yapisi; varsayilan, hizli
 *      kurulum ve yeniden uydurma (refit) ayri ayri.
 *   3) Izleme (GPU): birincil isin (tum pikseller; dogrulama goruntusu) ve
 *      yansima isini (yalnizca G-tamponunda yansiyan pikseller).          */
#define HD_MAXVSI 512
static struct { int16_t po, pty, cx, fogr, litr; uint8_t skin, ok; float ramp[2]; } g_hd_vsi[HD_MAXVSI];
void metal_hd_vs_info(int gid, int po, int pty, int cx, int skinned, int fogr, int litr, float ramp0, float ramp1){
    if (gid <= 0 || gid >= HD_MAXVSI) return;
    g_hd_vsi[gid].po = (int16_t)po; g_hd_vsi[gid].pty = (int16_t)pty;
    g_hd_vsi[gid].cx = (int16_t)cx; g_hd_vsi[gid].skin = skinned ? 1 : 0;
    g_hd_vsi[gid].fogr = (int16_t)fogr;
    g_hd_vsi[gid].litr = (int16_t)litr; g_hd_vsi[gid].ramp[0] = ramp0; g_hd_vsi[gid].ramp[1] = ramp1;
    g_hd_vsi[gid].ok = 1;
}
static NSString *kRTSrc = @
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"using namespace raytracing;\n"
"struct RTP { float4x4 ivp; float4 cam; float4 cb0; float4 cb1; float4 cb2; float4 sz; };\n"
/* sz = (W, H, kip, -) ; kip 0 = birincil, 1 = yansima */
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
/* mesafe + ucgen kimliginden hafif ton: geometri okunsun */
"  float sh = 1.0 / (1.0 + 0.02 * h.distance);\n"
"  float hsh = float((h.primitive_id * 2654435761u) >> 24) / 255.0;\n"
"  outp.write(float4(sh * (0.6 + 0.4 * hsh), sh * (0.7 + 0.3 * h.triangle_barycentric_coord.x), sh, 1.0), tid);\n"
"}\n";
static double hd_now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
                            return t.tv_sec * 1e3 + t.tv_nsec * 1e-6; }
static void hd_rt_probe(int rb, id<MTLTexture> ref, long frame){
    static int done = 0; if (done) return; done = 1;
    fprintf(stderr, "\n[RT] ===== isin izleme sure olcumu (kare %ld) =====\n", frame);
    if (!g_dev.supportsRaytracing){ fprintf(stderr, "[RT] cihaz isin izlemeyi desteklemiyor\n"); return; }
    if (!g_hd_cam_ok[rb]){ fprintf(stderr, "[RT] kamera yok\n"); return; }
    float vp[16], ivp[16], iv[16];
    memcpy(vp, &g_hd_cam[rb][32], sizeof vp);
    for (int r4 = 0; r4 < 4; r4++) vp[r4*4] *= g_hd_cam_k[rb];
    if (!hd_inv4(vp, ivp) || !hd_inv4(&g_hd_cam[rb][0], iv)) return;

    /* --- 1) yakalama --- */
    double t0 = hd_now();
    size_t cap = 1u << 20, nv = 0;
    float *V = (float *)malloc(cap * 12);
    unsigned nd = 0, sk_vs = 0, sk_skin = 0, sk_ortho = 0, sk_nz = 0;
    for (unsigned i = 0; i < g_dn[rb]; i++){
        MDraw *dr = &g_dbuf[rb][i];
        if (dr->is_blit && !dr->blit_src) break;            /* sahne sonu */
        if (dr->target || dr->is_clear || dr->is_blit || dr->count < 3) continue;
        if (dr->vsid <= 0 || dr->vsid >= HD_MAXVSI || !g_hd_vsi[dr->vsid].ok ||
            g_hd_vsi[dr->vsid].po < 0 || g_hd_vsi[dr->vsid].cx < 0 ||
            (g_hd_vsi[dr->vsid].pty != 2 && g_hd_vsi[dr->vsid].pty != 3)){ sk_vs++;
            { static uint8_t seen[HD_MAXVSI];
              if (dr->vsid > 0 && dr->vsid < HD_MAXVSI && !seen[dr->vsid]){ seen[dr->vsid] = 1;
                  fprintf(stderr, "[RT]   atlanan vs_%d: bilgi=%d konum ofseti=%d turu=%d matris yazmaci=%d (ps %d, %u kose)\n",
                          dr->vsid, g_hd_vsi[dr->vsid].ok, g_hd_vsi[dr->vsid].po, g_hd_vsi[dr->vsid].pty,
                          g_hd_vsi[dr->vsid].cx, dr->ps, dr->count); } }
            continue; }
        if (g_hd_vsi[dr->vsid].skin){ sk_skin++; continue; }
        if (!(dr->depth & 2u)){ sk_nz++; continue; }        /* Z yazmayan: isik, parcacik */
        const uint8_t *kb = g_kbuf[rb] + dr->koff;
        unsigned stride; memcpy(&stride, kb, 4);
        const float *k = (const float *)(kb + 16);
        int cx = g_hd_vsi[dr->vsid].cx;
        if ((unsigned)(cx + 4) * 16u > dr->ksz - 16u || !stride){ sk_vs++; continue; }
        const float *c3 = k + (cx + 3) * 4;
        if (c3[0] == 0.f && c3[1] == 0.f && c3[2] == 0.f){ sk_ortho++; continue; }
        /* M[r][c] = c[cx+c][r];  W = M * ivp  (cift duyarlik) */
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
    fprintf(stderr, "[RT] yakalama: %u cizim, %zu ucgen, CPU %.2f ms  (atlanan: vs %u, kemikli %u, ortografik %u, Z yazmayan %u)\n",
            nd, ntri, t_cap, sk_vs, sk_skin, sk_ortho, sk_nz);
    if (ntri < 16){ free(V); return; }
    { float mn[3] = {1e30f,1e30f,1e30f}, mx[3] = {-1e30f,-1e30f,-1e30f};
      for (size_t q = 0; q < nv; q++) for (int a = 0; a < 3; a++){ float v = V[q*3+a];
          if (v < mn[a]) mn[a] = v; if (v > mx[a]) mx[a] = v; }
      fprintf(stderr, "[RT] dunya kutusu: x[%.0f..%.0f] y[%.0f..%.0f] z[%.0f..%.0f]  kamera (%.1f %.1f %.1f)\n",
              mn[0],mx[0],mn[1],mx[1],mn[2],mx[2], iv[12],iv[13],iv[14]); }

    @autoreleasepool {
    id<MTLBuffer> vb = [g_dev newBufferWithBytes:V length:nv * 12 options:MTLResourceStorageModeShared];
    free(V);
    /* --- 2) kurulum --- */
    id<MTLAccelerationStructure> acc = nil;
    static const char *un[3] = { "varsayilan kurulum", "hizli kurulum (PreferFastBuild)", "yeniden uydurma (refit)" };
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
            if (u == 2 && it == 0) continue;               /* ilk tur kurulum */
            if (it == 0) continue;                         /* isinma */
            if (ms < best) best = ms; sum += ms;
        }
        fprintf(stderr, "[RT] %-34s: en iyi %.2f ms, ort %.2f ms  (yapi %.1f MB)\n", un[u], best, sum / (N - 1),
                sz.accelerationStructureSize / 1048576.0);
        if (u == 0) acc = a2;
    }
    /* --- 3) izleme --- */
    NSError *e = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:hd_gb_src(kRTSrc) options:nil error:&e];
    if (!lib){ fprintf(stderr, "[RT] cekirdek derlenemedi: %s\n", [[e localizedDescription] UTF8String]); return; }
    id<MTLComputePipelineState> cps = [g_dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"hd_rt_probe"] error:&e];
    if (!cps){ fprintf(stderr, "[RT] hesaplama boru hatti kurulamadi: %s\n", [[e localizedDescription] UTF8String]); return; }
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
    static const char *mn2[2] = { "birincil isin (tum pikseller)", "yansima isini (yol + araba)" };
    for (int mode = 0; mode < 2; mode++){
        if (mode == 1 && !have_g){ fprintf(stderr, "[RT] yansima kipi atlandi: G-tamponu yok (HD_SSR acik olmali)\n"); break; }
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
        /* isin sayisi: alfa > 0.75 olan pikseller iz surdu */
        size_t rays = 0, hits = 0;
        { uint8_t *px = (uint8_t *)malloc(Wd * Hd * 4);
          [outp getBytes:px bytesPerRow:Wd * 4 fromRegion:MTLRegionMake2D(0, 0, Wd, Hd) mipmapLevel:0];
          for (size_t q = 0; q < Wd * Hd; q++){
              if (mode == 0){ rays++; if (px[q*4+3] > 200) hits++; }
              else if (px[q*4+3] > 200){ rays++; if (px[q*4+2] > 80 || px[q*4+1] > 8) hits++; } }
          free(px); }
        fprintf(stderr, "[RT] %-34s: en iyi %.2f ms, ort %.2f ms  (%zu isin, %.0f%% isabet, %.0f M isin/sn)\n",
                mn2[mode], best, sum / (N - 1), rays, rays ? 100.0 * hits / rays : 0.0,
                best > 0 ? rays / best / 1000.0 : 0.0);
        dump_texture(outp, 99001 + mode);
    }
    fprintf(stderr, "[RT] ===== son =====\n\n");
    }
}

/* ===== HD: ISIN IZLEMELI YANSIMA (HD_RT=0 kapatir -> SSR'a duser) ==========
 * Her karede, ana sahne biter bitmez (SSR birlestirmesinin yerinde):
 *   1) YAKALAMA (GPU): sahnenin her cizimi icin oyunun KENDI kose
 *      gölgelendiricisi bir hesaplama cekirdegi olarak bir kez daha calisir
 *      (vsemu.c vsk_N) ve dunya konumu + renk + doku koordinatlari + sis
 *      tampona yazilir.  Atlanan cizim yok: yol, kemikli model, hepsi.
 *   2) IVME YAPISI: tum ucgenlerle her karede yeniden kurulur (olculdu:
 *      ~19 bin ucgende 1.7 ms).
 *   3) YANSIMA: yol ve araba pikselleri icin isin; isabette oyunun piksel
 *      gölgelendiricisinin matematigi (ps 2/10: 2*doku*renk; ps 4/6: far
 *      isigi terimiyle; araba: govde dokusu), alfa testi ve sis uygulanir.
 *      Isabet yoksa oyunun kendi yansimasi (R0) kalir.
 * Malzeme dokulari bagimsiz tablo (arguman tamponu) ile ornekleniyor.   */
#define RT_MAXV   (600u * 1024u)     /* anlik yakalama: kose */
#define RT_SMAXV  (480u * 1024u)     /* sabit depo: kose */
#define RT_SMAXE  8192u              /* sabit malzeme */
#define RT_MAXREC 8192u
#define RT_MAXTEX 512u
typedef struct { uint32_t rawoff, koff, first, mat, ib; int32_t base; uint32_t isz, pad; } HDRecC;
typedef struct { uint32_t tex0, tex1, tex2, flags; float c0[4]; float fogc[4]; float prm[4]; float lw[16]; } HDMatC;
static int g_rt = -1;
static id<MTLComputePipelineState> g_hd_vsk[HD_MAXVSI];
static id<MTLBuffer> g_rt_pos, g_rt_attr, g_rt_scratch, g_rt_item[3], g_rt_rec[3], g_rt_mat[3], g_rt_arg[3];
/* --- ANLIK YAPI: YERINDE GUNCELLEME (refit) -------------------------------
 * Hareketli geometrinin yapisi her karede bastan kuruluyordu.  Bir onceki
 * karedekiyle ayni cizimler ayni sirada ve ayni ucgen sayisiyla geldiyse
 * ucgen duzeni aynidir; yalnizca konumlar degisir.  O zaman agac yeniden
 * kurulmaz, kutulari yeni konumlara uydurulur (refit, kurulumun kucuk bir
 * kesri).  Hareket agaci zamanla gevsettigi icin her RT_REFIT_MAX karede
 * bir yine bastan kurulur.  HD_RT_REFIT=0 eski davranis. */
#define RT_REFIT_MAX 20u
static uint64_t g_rt_dsig;                      /* son kurulan/uydurulan duzenin imzasi */
static uint32_t g_rt_dsince = 0xFFFFFFFFu;      /* son tam kurulumdan beri kare */
static _Atomic unsigned long long g_rt_nbuild, g_rt_nrefit;
static int hd_rt_refit_on(void){
    static int v = -1;
    if (v < 0){ const char *e = ENVC("HD_RT_REFIT"); v = (e && *e == '0') ? 0 : 1; }
    return v;
}
static MTLAccelerationStructureUsage hd_rt_dyn_usage(void){
    return MTLAccelerationStructureUsagePreferFastBuild | (hd_rt_refit_on() ? MTLAccelerationStructureUsageRefit : 0);
}
static id<MTLBuffer> g_rt_sposb[2], g_rt_sattrb[2], g_rt_sscratch, g_rt_tscratch, g_rt_inst;
static unsigned g_rt_sbuf;           /* etkin sabit tampon cifti (sikistirmada degisir) */
#define g_rt_spos  g_rt_sposb[g_rt_sbuf]
#define g_rt_sattr g_rt_sattrb[g_rt_sbuf]
static id<MTLAccelerationStructure> g_rt_as, g_rt_sas, g_rt_tlas;
static int g_rt_cache = 1, g_rt_only_static;
static id<MTLRenderPipelineState> g_rt_pso, g_rt_pso_ms;
static id<MTLComputePipelineState> g_rt_kern;
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
        if (@available(macOS 13.0, *)) {} else g_rt = 0;      /* render gecisinde ivme yapisi */
        fprintf(stderr, "[HD] isin izlemeli yansima: %s\n", g_rt ? "ACIK" : "KAPALI"); }
    return g_rt;
}
static id<MTLComputePipelineState> g_rt_fixk;   /* kose denetimi (HD_RT_FIX=0 kapatir) */
static id<MTLBuffer> g_rt_fixcnt;               /* [0] NaN/sonsuz ucgen, [1] cok uzak ucgen (toplam) */
static NSString *kRTFSrc = @
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"using namespace raytracing;\n"
"struct FSO { float4 pos [[position]]; };\n"
"vertex FSO hd_fs_vert(uint vid [[vertex_id]]) {\n"
"  float2 p = float2(float((vid << 1) & 2), float(vid & 2));\n"
"  FSO o; o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0); return o; }\n"
"struct SSRP { float4x4 vp; float4x4 ivp; float4 cam; float4 prm; float4 sz; float4 cam2;\n"
"              float4 cb0; float4 cb1; float4 cb2; float4 fogp; };\n"
"struct HDMat { uint tex0; uint tex1; uint tex2; uint flags; float4 c0; float4 fogc; float4 prm;\n"
"               float4 l0; float4 l1; float4 l2; float4 l3; };\n"
"struct HDAttr { float4 col0; float4 uv1; float4 uv2; float2 uv0; float fog; float pad; };\n"
"struct HDTex { array<texture2d<float>, 512> t; };\n"
"static float2 hd_proj(float4 q, uint mode) {\n"
"  float d = mode == 2u ? q.w : (mode == 1u ? q.z : 1.0);\n"
"  return q.xy / (abs(d) < 1e-6 ? 1e-6 : d); }\n"
/* Isinlar HESAPLAMA cekirdeginde atilir.  Ayni kod parca gölgelendiricisinde
 * olculdu: GPU ~14 ms/kare (oyun GPU'ya takildi, ~74 fps); cekirdekte ~2 ms. */
"kernel void hd_rt_kern(uint2 tid0 [[thread_position_in_grid]],\n"
"    texture2d<float, access::write> outp [[texture(0)]], depth2d<float> dep [[texture(1)]],\n"
"    texture2d<float> g1 [[texture(2)]], texture2d<float> g3 [[texture(4)]],\n"
"    constant SSRP &S [[buffer(0)]], instance_acceleration_structure acc [[buffer(1)]],\n"
"    device const HDMat *MAT [[buffer(3)]],\n"
"    device const HDAttr *ATT [[buffer(4)]], constant HDTex &TX [[buffer(6)]],\n"
"    device const HDAttr *SATT [[buffer(7)]]) {\n"
/* S.cb2.w = 0 tam; 1/2 = satranc tahtasinin o karedeki yarisi (bkz. YARIM ISIN) */
"  uint par = uint(S.cb2.w + 0.5);\n"
"  uint2 tid = par == 0u ? tid0 : uint2(tid0.x * 2u + ((tid0.y + par) & 1u), tid0.y);\n"
/* S.cb0.w = olcek (1 ya da 2): cikti (W/olcek, H/olcek); her cikti pikseli tam
 * cozunurluklu G-tamponunun (tid * olcek) pikselini temsil eder. */
"  uint sc = uint(max(S.cb0.w, 1.0));\n"
"  uint2 px = tid * sc;\n"
"  if (px.x >= uint(S.sz.x) || px.y >= uint(S.sz.y)) return;\n"
"  float4 gb = g1.read(px); float cls = GB_CLS(gb);\n"
"  bool road = GB_ROAD(cls);\n"
"  bool car  = GB_CAR(cls);\n"
"  float d = dep.read(px);\n"
"  if ((!road && !car) || d >= 1.0) { outp.write(float4(0.0), tid); return; }\n"
"  float2 uv = (float2(px) + 0.5) / S.sz.xy;\n"
"  float4 wp = S.ivp * float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, d, 1.0);\n"
"  float3 P = wp.xyz / wp.w;\n"
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
"  for (int k = 0; k < 5; k++) {\n"
"    ray r; r.origin = org; r.direction = Rd; r.min_distance = 0.01; r.max_distance = maxd - tot;\n"
"    intersection_result<triangle_data, instancing> h = isec.intersect(r, acc);\n"
"    if (h.type == intersection_type::none) break;\n"
"    uint g0 = h.primitive_id * 3u;\n"
/* ornek 0 = anlik yakalama (maske 1), ornek 1 = sabit depo (maske 2; S.cb1.w: yalnizca sabit var) */
"    bool st = h.instance_id == 1u || S.cb1.w > 0.5;\n"
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
/* sabit depo: kayitli sis terfi anindaki kameraya gore; oyunun formuluyle
 * (vs_4: sis = clamp((bitis - derinlik) * olcek, alt, 1)) yeniden hesapla */
"    if ((m.flags & 0x10000u) != 0u) {\n"
"      float3 hp = org + Rd * h.distance;\n"
"      float zv = (S.vp * float4(hp, 1.0)).w;\n"
"      fog = S.fogp.w > 0.5 ? clamp((S.fogp.x - zv) * S.fogp.y, S.fogp.z, 1.0) : 1.0;\n"
"    }\n"
"    bool mcar = (m.flags & 2u) != 0u;\n"
"    float lod = clamp(log2(1.0 + tot * S.cam2.z), 0.0, 5.0);\n"
"    float4 tc = S.cam2.w > 0.5 ? float4(1.0) : TX.t[mcar ? m.tex1 : m.tex0].sample(rs, mcar ? uv1.xy : uv0, level(lod));\n"
/* alfa testi (agac, cit): isin yuzeyin icinden devam eder */
"    if ((m.flags & 1u) != 0u && tc.a * col.a < m.prm.x) { org += Rd * (h.distance + 0.02); continue; }\n"
"    float3 lit = col.rgb;\n"
/* Sabit kopya ISIKSIZ TABAN saklanir.  Parca bu karede far isigi alarak
 * ciziliyorsa (0x20000) far terimi isabet noktasinda, cizimin O ANKI isik
 * matrisiyle (dunya -> isik, m.l0..l3) kurulur: oT1 = p.L, oT2.x = olcek*q.w + kayma. */
"    if ((m.flags & 0x20000u) != 0u && S.cam2.w < 0.5) {\n"
"      float3 hp2 = org + Rd * h.distance;\n"
"      float4 q1 = hp2.x * m.l0 + hp2.y * m.l1 + hp2.z * m.l2 + m.l3;\n"
"      float4 q2 = float4(q1.w * m.prm.y + m.prm.z, 0.0, 1.0, 1.0);\n"
"      float3 l1 = TX.t[m.tex1].sample(cs, hd_proj(q1, (m.flags >> 8) & 3u), level(0.0)).rgb;\n"
"      float3 l2 = TX.t[m.tex2].sample(cs, hd_proj(q2, (m.flags >> 10) & 3u), level(0.0)).rgb;\n"
"      lit = l1 * l2 * m.c0.rgb + col.rgb;\n"
"    }\n"
"    if ((m.flags & 4u) != 0u && S.cam2.w < 0.5) {\n"   /* ps 4/6: far isigi dokusu x sonum rampasi */
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
"  outp.write(float4(rc, conf), tid);\n"
"}\n"
/* KOSE DENETIMI: yakalanan ucgenlerde NaN / sonsuz / akil disi uzak kose
 * varsa ucgeni dejenere et.  TEK bir NaN'li ucgen ivme yapisinin kutularini
 * kokten zehirler: isinlar hicbir dugumu eleyemez, izleme ~15 kat yavaslar
 * (olculdu: 3024x1964'te duz izleme 4 ms yerine 58 ms, kosularin ~%8'inde;
 * hangi cizimin urettigi rastgele secilen iceriğe bagli). */
"struct HDFix { uint ntri; float lim; uint first; uint slot; };\n"
"kernel void hd_rt_fix(uint tid [[thread_position_in_grid]],\n"
"    device packed_float3 *P [[buffer(0)]], device atomic_uint *CNT [[buffer(1)]],\n"
"    constant HDFix &F [[buffer(2)]]) {\n"
"  if (tid >= F.ntri) return;\n"
"  uint v = F.first + tid * 3u;\n"
/* F.lim < 0: TANI (HD_RT_POISON): denetim yerine ilk ucgenin bir kosesini -F.lim'e tasi */
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
/* birlestirme: renk + B * (R_yeni - R0) */
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
"  else {\n"                 /* derinlige ve sinifa duyarli cift dogrusal buyutme */
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
/* kopyasiz: pikselin kendi rengi karo belleginden (programlanabilir karistirma) */
"fragment float4 hd_rtc_frag_fb(FSO in [[stage_in]], float4 c [[color(0)]],\n"
"    depth2d<float> dep [[texture(1)]], texture2d<float> g1 [[texture(2)]],\n"
"    texture2d<float> g2 [[texture(3)]], texture2d<float> rt [[texture(5)]],\n"
"    constant SSRP &S [[buffer(0)]]) {\n"
"  return rtc(uint2(in.pos.xy), c, dep, g1, g2, rt, S);\n"
"}\n";
/* --- KOPYASIZ BIRLESTIRME ----------------------------------------------------
 * Isin izleme birlestirmesi her pikselde yalnizca KENDI rengini okur.  Eskiden
 * once tum ekran bir kopyaya aktariliyor (3024x1964'te 0.34 ms GPU, ~46 MB
 * trafik), sonra tum ekran bastan yaziliyordu.  Apple GPU'larinda gecis ekrani
 * yukleyip pikselin rengini karo belleginden okuyabilir: kopya gerekmez.  SSR
 * (baska piksellerden ornekler) ve tekil MSAA kipi eski yolda.  HD_RT_FB=0 eski. */
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
    if (!lib){ fprintf(stderr, "[HD] RT gölgelendiricisi derlenemedi: %s\n", [[e localizedDescription] UTF8String]); g_rt = 0; return; }
    id<MTLFunction> ff = [lib newFunctionWithName:@"hd_rtc_frag"];
    id<MTLFunction> kf = [lib newFunctionWithName:@"hd_rt_kern"];
    g_rt_kern = [g_dev newComputePipelineStateWithFunction:kf error:&e];
    { id<MTLFunction> xf = [lib newFunctionWithName:@"hd_rt_fix"];
      if (xf) g_rt_fixk = [g_dev newComputePipelineStateWithFunction:xf error:&e];
      g_rt_fixcnt = [g_dev newBufferWithLength:(2 + 2 * HD_MAXVSI) * 4 options:MTLResourceStorageModeShared];
      memset([g_rt_fixcnt contents], 0, (2 + 2 * HD_MAXVSI) * 4);
      { const char *ev = ENVC("HD_RT_FIX"); if (ev && *ev == '0') g_rt_fixk = nil; } }
    if (!g_rt_kern){ fprintf(stderr, "[HD] RT cekirdegi kurulamadi: %s\n", [[e localizedDescription] UTF8String]); g_rt = 0; return; }
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"hd_fs_vert"];
    pd.fragmentFunction = ff;
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    g_rt_pso = [g_dev newRenderPipelineStateWithDescriptor:pd error:&e];
    if (hd_rt_fb_on()){ MTLRenderPipelineDescriptor *pf = [pd copy];
        pf.fragmentFunction = [lib newFunctionWithName:@"hd_rtc_frag_fb"];
        NSError *ef = nil; g_rt_pso_fb = [g_dev newRenderPipelineStateWithDescriptor:pf error:&ef];
        if (!g_rt_pso_fb) fprintf(stderr, "[HD] kopyasiz birlestirme kurulamadi: %s\n", [[ef localizedDescription] UTF8String]); }
    if (hd_msaa_n() > 1){ pd.rasterSampleCount = (NSUInteger)hd_msaa_n();
                          g_rt_pso_ms = [g_dev newRenderPipelineStateWithDescriptor:pd error:&e]; }
    if (!g_rt_pso){ fprintf(stderr, "[HD] RT boru hatti kurulamadi: %s\n", [[e localizedDescription] UTF8String]); g_rt = 0; return; }
    g_rt_argenc = [kf newArgumentEncoderWithBufferIndex:6];
    g_rt_pos   = [g_dev newBufferWithLength:RT_MAXV * 12 options:MTLResourceStorageModePrivate];
    g_rt_attr  = [g_dev newBufferWithLength:RT_MAXV * 64 options:MTLResourceStorageModePrivate];
    for (int i = 0; i < 2; i++){
        g_rt_sposb[i]  = [g_dev newBufferWithLength:RT_SMAXV * 12 options:MTLResourceStorageModePrivate];
        g_rt_sattrb[i] = [g_dev newBufferWithLength:RT_SMAXV * 64 options:MTLResourceStorageModePrivate];
    }
    for (int i = 0; i < 3; i++){
        g_rt_item[i] = [g_dev newBufferWithLength:RT_MAXV * 4 options:MTLResourceStorageModeShared];
        g_rt_rec[i]  = [g_dev newBufferWithLength:RT_MAXREC * sizeof(HDRecC) options:MTLResourceStorageModeShared];
        g_rt_mat[i]  = [g_dev newBufferWithLength:(RT_SMAXE + RT_MAXREC) * sizeof(HDMatC) options:MTLResourceStorageModeShared];
        g_rt_arg[i]  = [g_dev newBufferWithLength:g_rt_argenc.encodedLength options:MTLResourceStorageModeShared];
    }
    /* alt yapilar ve karalama tamponlari en buyuk boyuta gore bir kez ayrilir */
    for (int k = 0; k < 2; k++){
        MTLAccelerationStructureTriangleGeometryDescriptor *gd =
            [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
        gd.vertexBuffer = k ? g_rt_spos : g_rt_pos; gd.vertexStride = 12;
        gd.triangleCount = (k ? RT_SMAXV : RT_MAXV) / 3; gd.opaque = YES;
        MTLPrimitiveAccelerationStructureDescriptor *ad = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
        ad.geometryDescriptors = @[gd];
        ad.usage = k ? MTLAccelerationStructureUsageNone : hd_rt_dyn_usage();
        MTLAccelerationStructureSizes sz = [g_dev accelerationStructureSizesWithDescriptor:ad];
        id<MTLAccelerationStructure> a = [g_dev newAccelerationStructureWithSize:sz.accelerationStructureSize];
        NSUInteger scs = sz.buildScratchBufferSize > sz.refitScratchBufferSize ? sz.buildScratchBufferSize : sz.refitScratchBufferSize;
        id<MTLBuffer> sc = [g_dev newBufferWithLength:scs options:MTLResourceStorageModePrivate];
        if (k){ g_rt_sas = a; g_rt_sscratch = sc; } else { g_rt_as = a; g_rt_scratch = sc; }
    }
    g_rt_inst = [g_dev newBufferWithLength:2 * sizeof(MTLAccelerationStructureInstanceDescriptor)
                                   options:MTLResourceStorageModeShared];
    { MTLAccelerationStructureInstanceDescriptor *id2 = (MTLAccelerationStructureInstanceDescriptor *)[g_rt_inst contents];
      memset(id2, 0, 2 * sizeof *id2);
      for (int k = 0; k < 2; k++){
          id2[k].transformationMatrix.columns[0] = MTLPackedFloat3Make(1, 0, 0);
          id2[k].transformationMatrix.columns[1] = MTLPackedFloat3Make(0, 1, 0);
          id2[k].transformationMatrix.columns[2] = MTLPackedFloat3Make(0, 0, 1);
          id2[k].transformationMatrix.columns[3] = MTLPackedFloat3Make(0, 0, 0);
          id2[k].options = MTLAccelerationStructureInstanceOptionOpaque;
          id2[k].mask = 0xFF; id2[k].accelerationStructureIndex = (uint32_t)k; } }
    { MTLInstanceAccelerationStructureDescriptor *td = [MTLInstanceAccelerationStructureDescriptor descriptor];
      td.instancedAccelerationStructures = @[g_rt_as, g_rt_sas];
      td.instanceCount = 2; td.instanceDescriptorBuffer = g_rt_inst;
      MTLAccelerationStructureSizes sz = [g_dev accelerationStructureSizesWithDescriptor:td];
      g_rt_tlas = [g_dev newAccelerationStructureWithSize:sz.accelerationStructureSize];
      g_rt_tscratch = [g_dev newBufferWithLength:sz.buildScratchBufferSize options:MTLResourceStorageModePrivate]; }
    { const char *e2 = ENVC("HD_RT_CACHE"); g_rt_cache = (e2 && *e2 == '0') ? 0 : 1; }
    fprintf(stderr, "[HD] RT hazir: anlik en fazla %u ucgen, sabit depo en fazla %u ucgen, onbellek %s\n",
            RT_MAXV / 3, RT_SMAXV / 3, g_rt_cache ? "ACIK" : "KAPALI");
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
/* ---- SABIT SAHNE ONBELLEGI --------------------------------------------
 * Oyun kameranin gormedigini cizmiyor; arabanin arkasindaki sahne yansimada
 * eksik kalirdi.  Ayrica her karede tum sahneyi yakalayip yapiyi kurmak
 * GPU'yu kare sinirinin ustune itiyordu (olculdu: 15.6 ms/kare).
 * Cizim = (ham kose verisinin ozeti, dunya matrisi W).  Ayni cizim
 * RT_PROMOTE kare ayni yerde gorulurse SABIT sayilir: yakalanan koseleri
 * GPU'da sabit depoya kopyalanir (CPU'ya geri okuma yok) ve sabit alt yapi
 * en fazla RT_SREBUILD karede bir yeniden kurulur.  Sabit girdiler anlik
 * yakalamadan cikar; anlik alt yapi arabalar + yeni gorunenlerle kalir.
 * Atma: gorus alaninin icinde oldugu halde cizilmiyorsa (tasinmis/kalkmis)
 * ya da cok uzaksa.  Arabalar (kup haritali ps) ve kemikliler hic girmez. */
#define RT_PROMOTE  12u
#define RT_SREBUILD 30u
#define RT_TABN     16384u
typedef struct { uint64_t rk, gh; float W[12]; float c[3], r; uint32_t first, n, seen, born, matidx;
                 uint16_t sight; uint8_t state; uint8_t pad; uint8_t vol; } RTEnt;
/* state: 0 bos, 1 aday, 2 sabit (yapi bekliyor), 3 sabit, 9 silinmis.  pad: depoya kopyalandi.
 * gh: yalniz geometri ozeti (tablo anahtari); rk: gh + doku + VS.  vol: oynaklik (asagida). */
static RTEnt g_rt_tab[RT_TABN];
static HDMatC g_rt_smatc[RT_SMAXE]; static uint32_t g_rt_smtex[RT_SMAXE][3];
static uint32_t g_rt_scur, g_rt_sdead, g_rt_smn, g_rt_sbuilt, g_rt_frame_no, g_rt_last_sbuild, g_rt_tomb;
static int g_rt_sdirty;
static _Atomic unsigned long long g_rt_promo, g_rt_evict, g_rt_reset, g_rt_sbuilds, g_rt_dynv, g_rt_statv;
static int g_rt_need_clear = 1;          /* yeni tamponun icerigi tanimsiz */
static uint32_t g_rt_mfree[RT_SMAXE], g_rt_mfree_n;
static uint32_t g_rt_mpend[RT_SMAXE], g_rt_mpend_n;   /* atilanlarin yuvalari: yapi yeniden kurulunca serbest */
static unsigned g_rt_ev_far, g_rt_ev_frus, g_rt_ev_cand;
static _Atomic unsigned long long g_rt_evsib;
static _Atomic unsigned long long g_rt_compact, g_rt_rehash;
#define RT_HIDX(gh) ((unsigned)((gh) * 0x9E3779B97F4A7C15ull >> 50) & (RT_TABN - 1))
/* Silinmis girdiler (9) sonda zincirlerini uzatir; canlilari yerinde yeniden diz. */
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
/* Bir girdiyi at.  Sabitse depodaki araligi sifirlanir (dejenere ucgen),
 * malzeme yuvasi yapi yeniden kurulunca serbest kalir.  Sifirlama listesi
 * doluysa atma ertelenir (0 doner). */
static uint32_t g_rt_zr[1024][2]; static unsigned g_rt_nzr;
static int rt_evict(RTEnt *e){
    if (e->state == 2 || e->state == 3){
        if (e->pad){
            if (g_rt_nzr >= 1024) return 0;
            g_rt_zr[g_rt_nzr][0] = e->first; g_rt_zr[g_rt_nzr][1] = e->n; g_rt_nzr++;
        }
        g_rt_sdead += e->n; g_rt_sdirty = 1;
        if (g_rt_mpend_n < RT_SMAXE) g_rt_mpend[g_rt_mpend_n++] = e->matidx;
        atomic_fetch_add(&g_rt_evict, 1ull);
    }
    e->state = 9; g_rt_tomb++;
    return 1;
}
/* SIKISTIRMA: olu araliklar deponun %40'ini gectiginde canli girdiler diger
 * tampon ciftine art arda kopyalanir (GPU'da).  Tum depoyu sifirlamak
 * biriktirilen kamera-arkasi geometriyi de siliyordu.  Onceki karelerin
 * komut tamponlari eski cifti okumaya devam eder (ping-pong). */
static void rt_compact(id<MTLCommandBuffer> cb){
    unsigned src = g_rt_sbuf, dst = 1u - g_rt_sbuf;
    uint32_t cur = 0;
    id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
    for (unsigned q = 0; q < RT_TABN; q++){
        RTEnt *e = &g_rt_tab[q];
        if (e->state != 2 && e->state != 3) continue;
        if (!e->pad){ e->state = 1; g_rt_mfree[g_rt_mfree_n++] = e->matidx; continue; }   /* henuz kopyalanmamisti */
        [be copyFromBuffer:g_rt_sposb[src] sourceOffset:(NSUInteger)e->first * 12 toBuffer:g_rt_sposb[dst]
         destinationOffset:(NSUInteger)cur * 12 size:(NSUInteger)e->n * 12];
        [be copyFromBuffer:g_rt_sattrb[src] sourceOffset:(NSUInteger)e->first * 64 toBuffer:g_rt_sattrb[dst]
         destinationOffset:(NSUInteger)cur * 64 size:(NSUInteger)e->n * 64];
        e->first = cur; cur += e->n; e->state = 2;              /* yapi yeniden kurulana dek anlik yakalamada */
    }
    /* kalan kisim eski bir sikistirmanin artigi: kopyasi geciken terfiler cop ucgen gostermesin */
    if (cur < RT_SMAXV)
        [be fillBuffer:g_rt_sposb[dst] range:NSMakeRange((NSUInteger)cur * 12, (NSUInteger)(RT_SMAXV - cur) * 12) value:0];
    [be endEncoding];
    g_rt_sbuf = dst; g_rt_scur = cur; g_rt_sdead = 0; g_rt_sbuilt = 0; g_rt_sdirty = 1;
    atomic_fetch_add(&g_rt_compact, 1ull);
}
static void rt_cache_reset(void){
    g_rt_need_clear = 1;                 /* eski geometri dejenere olsun */
    g_rt_mfree_n = g_rt_mpend_n = 0;
    memset(g_rt_tab, 0, sizeof g_rt_tab);
    g_rt_scur = g_rt_sdead = g_rt_smn = g_rt_sbuilt = g_rt_tomb = 0; g_rt_sdirty = 0;
    atomic_fetch_add(&g_rt_reset, 1ull);
}
static uint32_t g_rt_slit[RT_SMAXE];      /* sabit malzeme: bu karede far isigi alarak cizildi */
static inline void rt_mat_of(const MDraw *dr, HDMatC *m){
    int base = dr->ps % PSREFL, cube = base >= PSCUBE, b = base % PSCUBE;
    memset(m, 0, sizeof *m);
    m->tex0 = dr->tex; m->tex1 = dr->tex1; m->tex2 = dr->tex2;        /* doku KIMLIKLERI */
    m->flags = (dr->alpha_ref > 0.f ? 1u : 0u) | (cube ? 2u : 0u) | ((b == 4 || b == 6) ? 4u : 0u)
             | ((((unsigned)(int)dr->proj[1] >> 4) & 3u) << 8) | ((((unsigned)(int)dr->proj[2] >> 4) & 3u) << 10);
    memcpy(m->c0, dr->c0, 16); memcpy(m->fogc, dr->fogv, 16);
    m->prm[0] = dr->alpha_ref;
}
/* Sabit girdinin malzemesini o anki cizimden yenile (sis rengi vb. guncel
 * kalsin).  Sabit kopya ISIKSIZ TABAN olarak saklanir: far isigi (ps 4/6)
 * oyuncunun arabasiyla gezer; kose basina dondurulursa isik havuzu araba
 * gittikten sonra da yolda kalir (olculdu: rakipler kalktiktan sonra start
 * cizgisi yansimada parlak).  Oyun yakin sahnenin neredeyse tamamini isikli
 * VS ile ciziyor (yaris basinda 255 cizim), hepsini her kare yeniden
 * yakalamak onbellegi anlamsizlastirir (GPU 7.1 -> 10.9 ms).  Bunun yerine
 * cizimin isik matrisi (VS sabitleri c[K..K+3], nesne -> isik) dunya uzayina
 * cevrilip malzemeye yazilir; cekirdek far terimini isabet noktasinda kurar.
 * Parca o karede isikli cizilmediyse (ya da hic cizilmediyse) terim yoktur. */
static void rt_smat_update(const RTEnt *e, const MDraw *dr, const float *k, uint32_t frame){
    HDMatC m; rt_mat_of(dr, &m);
    int lit = (m.flags & 4u) != 0u, K = g_hd_vsi[dr->vsid].litr;
    m.flags = (m.flags & ~4u) | 0x10000u;                       /* 0x10000: sis yeniden hesaplanir */
    if (lit && K >= 0 && (unsigned)(K + 4) * 16u <= dr->ksz - 16u){
        /* dunya = nesne*A + t  ->  nesne = (dunya - t)*A^-1;  q = nesne*LW,  LW[r][c] = c[K+c][r] */
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
/* Ayni geometri (gh) ayni NOKTADA ama baska doku/VS ya da baska YONELIMLE
 * ciziliyorsa KARDES'tir:
 *   - far isigi alan yol parcasi <-> almayan, animasyonlu doku (rk farkli);
 *   - kameraya donen agac/isik panolari, donen nesneler (yonelim farkli).
 * Ikisi birden depoda kalirsa ust uste biner: isin rastgele birine carpar,
 * alfa testli yiginlar isini defalarca yeniden attirir (olculdu: bir agacin
 * 12 kopyasi; GPU 8 -> 22 ms).  Kural:
 *   - gorulmeyen (3+ kare) SABIT kardes atilir;
 *   - gorulmeyen ADAY kardes yerinde yeniden kullanilir (her karede donen
 *     nesne tabloyu yeni girdilerle doldurmasin);
 *   - her degisim OYNAKLIGI (vol) artirir, terfi esigi RT_PROMOTE << vol:
 *     surekli degisen sey anlik yakalamada kalir, depoyu calkalamaz.
 * Ayni karede birlikte cizilen kardesler (aynalanmis es parcalar) yasar. */
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
/* Sahnenin [0, upto) cizimlerini yakala, alt yapilari ve ust yapiyi kur.
 * Donus: toplam ucgen (0 = bu karede isin izleme yok).  fogp: sabit
 * isabetler icin oyunun sis parametreleri (bitis, olcek, alt sinir, var). */
static unsigned hd_rt_frame(id<MTLCommandBuffer> cb, int rb, unsigned upto, const float *iv,
                            const float *vp, const float *ivp, float ipx, float ipy,
                            unsigned *ntex, float *fogp){
    if (!g_rt_pso) return 0;
    unsigned ring = (g_rt_ring = (g_rt_ring + 1) % 3);
    uint32_t frame = ++g_rt_frame_no;
    static unsigned cnt[HD_MAXVSI], off[HD_MAXVSI], fill[HD_MAXVSI];
    static uint8_t  act[MAXD];            /* 0 atla, 1 anlik yakala */
    static RTEnt   *ent[MAXD];
    memset(cnt, 0, sizeof cnt);
    unsigned nrec = 0, total = 0, npromo = 0;
    fogp[0] = fogp[1] = fogp[2] = fogp[3] = 0.f;
    g_rt_nzr = 0;
    if (g_rt_cache){
        /* HD_RT_STRESS=N: her N karede sikistirma + yeniden ozetleme (ender yollarin testi) */
        static int stress = -1; if (stress < 0){ const char *ev = ENVC("HD_RT_STRESS"); stress = ev ? atoi(ev) : 0; }
        if (stress > 0 && frame % (uint32_t)stress == 0u){ rt_rehash(); if (g_rt_scur) rt_compact(cb); }
        if (g_rt_tomb > RT_TABN / 4u) rt_rehash();
        if ((g_rt_scur > 30000u && g_rt_sdead * 5u > g_rt_scur * 2u) ||
            (g_rt_scur > RT_SMAXV - 30000u && g_rt_sdead > 0u)) rt_compact(cb);
        if (g_rt_scur > RT_SMAXV - 30000u && g_rt_sdead == 0u && frame % 600u == 0u) rt_cache_reset();  /* dolu ve olu yok: en eskiyi birakmanin yolu */
    }
    #define RT_OK(dr) (!(dr)->target && !(dr)->is_clear && !(dr)->is_blit && (dr)->count >= 3 && \
        (dr)->vsid > 0 && (dr)->vsid < HD_MAXVSI && ((dr)->depth & 2u))
    /* --- 1. gecis: siniflandir --- */
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
            if (c3[0] == 0.f && c3[1] == 0.f && c3[2] == 0.f) continue;       /* ortografik (ekran) */
        }
        { int fr = g_hd_vsi[dr->vsid].fogr;                                   /* oyunun sis sabitleri */
          if (fr >= 0 && fogp[3] == 0.f && (unsigned)(fr + 1) * 16u <= dr->ksz - 16u){
              fogp[0] = k[fr*4]; fogp[1] = k[fr*4+1]; fogp[2] = k[fr*4+2]; fogp[3] = 1.f; } }
        unsigned n = dr->count - dr->count % 3u;
        int base = dr->ps % PSREFL, cube = base >= PSCUBE;
        RTEnt *e = 0;
        if (g_rt_cache && have_m && !cube && !g_hd_vsi[dr->vsid].skin && stride && n <= 60000u){
            /* W = M * P'^-1 * V^-1,  M[r][c] = c[cx+c][r].  Yakalama cekirdegiyle AYNI
             * yol: gorus = (x/P00', y/P11, w), homojen sutun TAM (0,0,0,1).
             * Eskiden M * (VP')^-1 idi: homojen bilesen (z - Q*w)/(-Q*near)
             * iptalinden ~1e-4 hatayla cikiyor, kamera konumuyla (yuzlerce birim)
             * carpilinca W'nin otelemesi kameraya gore 0.03-0.1 birim oynuyordu.
             * Sonuc: ayni nesne her kamera acisinda ayri girdi sayilip depoda
             * ust uste yigiliyordu (olculdu: 1771 girdinin 878'i kopya; alfa
             * testli kopya yiginlari isini 5 kez yeniden attirip GPU'yu
             * 8 ms'den 22 ms'ye cikardi). */
            float W[12];
            for (int r = 0; r < 4; r++){
                double vx = (double)k[cx * 4 + r] * (double)ipx, vy = (double)k[(cx + 1) * 4 + r] * (double)ipy,
                       vz = (double)k[(cx + 3) * 4 + r];
                for (int c = 0; c < 3; c++)
                    W[r * 3 + c] = (float)(vx * iv[c] + vy * iv[4 + c] + vz * iv[8 + c] + (r == 3 ? (double)iv[12 + c] : 0.0));
            }
            /* indisli cizimde kimlik (VB/IB + uretim + aralik) konuk tarafinda hesaplandi */
            uint64_t gh = dr->gh ? dr->gh : rt_rawhash(g_rvbuf[rb] + dr->rawoff, n * stride, n, stride);
            /* rk'de VS kimligi YOK: ayni parcanin far isigi alan ve almayan cizimi
             * (farkli VS, ayni konumlar) tek girdidir */
            uint64_t rk = (gh ^ ((uint64_t)dr->tex << 12)) * 0xD6E8FEB86659FD93ull;
            e = rt_find(rk ? rk : 1, gh, W, frame);
            if (e){
                /* terfi ART ARDA gorulme ister: donen nesne ayni aciya periyodik olarak
                 * geri gelir, toplam sayim onu her acida ayri "sabit" yapiyordu */
                if (e->seen != frame){
                    if (e->state == 1 && frame - e->seen > 2u) e->sight = 0;
                    e->seen = frame; if (e->sight < 60000) e->sight++; }
                if (e->state == 3 && e->n == n){
                    rt_smat_update(e, dr, k, frame);
                    ent[i] = e; continue;                                      /* sabit yapida: yakalama yok */
                }
                if (e->state == 1 && e->sight >= (RT_PROMOTE << e->vol) && npromo < 400u &&
                    g_rt_scur + n <= RT_SMAXV - 30000u && (g_rt_mfree_n || g_rt_smn < RT_SMAXE - 64u)){
                    npromo++;
                    /* TERFI: sinir kuresi ham konumlardan */
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
                        /* isin menzilinin disinda: depoya alma (gorulmedigi an "uzak" diye atilirdi) */
                        e->sight = 0;
                    } else {
                    e->state = 2; e->first = g_rt_scur; e->n = n; e->pad = 0;
                    e->matidx = g_rt_mfree_n ? g_rt_mfree[--g_rt_mfree_n] : g_rt_smn++;
                    g_rt_scur += n; g_rt_sdirty = 1;
                    atomic_fetch_add(&g_rt_promo, 1ull);
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
    /* terfi edenlerin GPU kopyalari: (anlik ilk kose, sabit ilk kose, adet) */
    static uint32_t cp[512][3]; unsigned ncp = 0;
    uint64_t dsig = 1469598103934665603ull;         /* anlik ucgen duzeninin imzasi (bkz. refit) */
    /* --- 2. gecis: kayitlar + malzemeler --- */
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
        if (e && e->state == 2 && e->first + n <= RT_SMAXV && e->seen == frame && e->born != 0xFFFFFFFFu && ncp < 512 &&
            e->pad == 0){
            /* bu karede terfi etti: sabit malzeme + GPU kopyasi */
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
        ri++;
    }
    #undef RT_OK
    /* sabit malzemeler: doku kimligi -> bu karenin yuvasi */
    for (unsigned q = 0; q < g_rt_smn; q++){
        HDMatC *sm = &mat[q]; *sm = g_rt_smatc[q];
        if (g_rt_slit[q] == frame) sm->flags |= 0x20000u;
        sm->tex0 = hd_rt_slot(g_rt_smatc[q].tex0, &nt); sm->tex1 = hd_rt_slot(g_rt_smatc[q].tex1, &nt);
        sm->tex2 = hd_rt_slot(g_rt_smatc[q].tex2, &nt);
    }
    /* --- atma taramasi ---
     * Oyun gorus alaninin icindekileri de ayikliyor (kucuk nesneyi uzakta
     * cizmiyor, bolum gorunurlugu).  "Gorus alaninda ama cizilmiyor" kurali
     * 200 birime kadar uygulaninca direkler vb. surekli atilip yeniden terfi
     * ediyordu (olculdu: kamera gezerken 300 karede 300+ atma).  Simdi:
     *  - UZAK: 900 birimden uzak ve 30 karedir gorulmuyor (isin menzili 600);
     *  - YAKIN: kameranin hemen onunde (< 50 birim, boyutuna gore daha az),
     *    gorusun ortasinda ve 60 karedir cizilmiyor = gercekten kalkmis
     *    (devrilen engel, kapanan menu sahnesi);
     *  - KARDES: rt_find icinde (ayni geometri baska doku/VS ile cizildi). */
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
                fprintf(stderr, "[RTEV] kare %u %s: gh %016llx n %u uzaklik %.0f r %.1f merkez (%.0f %.0f %.0f) gorulme %u omur %u yas %u mat(tex %u bayrak %x)\n",
                        frame, kill == 1 ? "uzak" : "yakin", (unsigned long long)e->gh, e->n, dist, e->r,
                        e->c[0], e->c[1], e->c[2], e->sight, frame - e->born, age, tex0, fl);
        }
    }
    { static int lg3 = -1; if (lg3 < 0){ const char *ev = ENVC("HD_RT_DUMPTAB"); lg3 = ev ? atoi(ev) : 0; }
      if (lg3 > 0 && frame == (uint32_t)lg3){
          for (unsigned q = 0; q < RT_TABN; q++){ RTEnt *e = &g_rt_tab[q];
              if (e->state != 2 && e->state != 3) continue;
              float dw = 0; for (int a = 0; a < 12; a++) dw += fabsf(e->W[a] - ((a == 0 || a == 4 || a == 8) ? 1.f : 0.f));
              fprintf(stderr, "[RTTAB] %016llx %016llx n %u c %.2f %.2f %.2f r %.2f yas %u gor %u vol %u birim %d t %.2f %.2f %.2f tex %u %u bayrak %x aref %.3f\n",
                      (unsigned long long)e->gh, (unsigned long long)e->rk, e->n, e->c[0], e->c[1], e->c[2], e->r, frame - e->seen, e->sight, e->vol,
                      dw < 1e-3f, e->W[9], e->W[10], e->W[11], g_rt_smatc[e->matidx].tex0, g_rt_smatc[e->matidx].tex1,
                      g_rt_smatc[e->matidx].flags, g_rt_smatc[e->matidx].prm[0]); }
          fprintf(stderr, "[RTTAB] kamera %.2f %.2f %.2f\n", iv[12], iv[13], iv[14]);
      } }
    { static int lg = -1; if (lg < 0) lg = ENVC("HD_RT_LOG") ? 1 : 0;
      static unsigned long long lp, le, lsib;
      if (lg && frame % 300u == 0u){
          unsigned n1 = 0, n2 = 0, n3 = 0;
          for (unsigned q = 0; q < RT_TABN; q++){ unsigned st = g_rt_tab[q].state; n1 += st == 1; n2 += st == 2; n3 += st == 3; }
          unsigned long long pr = atomic_load(&g_rt_promo), ev = atomic_load(&g_rt_evict);
          fprintf(stderr, "[RTLOG] kare %u: anlik %u ucgen/%u cizim, depo %u kose (%u olu), aday %u bekleyen %u sabit %u | "
                  "300 karede terfi %llu atma %llu (uzak %u, yakin %u, kardes %llu, aday %u) | sikistirma %llu sifirlama %llu ozet %llu\n",
                  frame, total / 3, nrec, g_rt_scur, g_rt_sdead, n1, n2, n3, pr - lp, ev - le,
                  g_rt_ev_far, g_rt_ev_frus, atomic_load(&g_rt_evsib) - lsib, g_rt_ev_cand, atomic_load(&g_rt_compact), atomic_load(&g_rt_reset), atomic_load(&g_rt_rehash));
          lp = pr; le = ev; lsib = atomic_load(&g_rt_evsib); g_rt_ev_far = g_rt_ev_frus = g_rt_ev_cand = 0;
          if (g_rt_fixcnt){ const uint32_t *fc = (const uint32_t *)[g_rt_fixcnt contents];
              char ln[512]; int o = 0;
              for (int g = 1; g < HD_MAXVSI && o < 470; g++) if (fc[2 + g * 2] || fc[3 + g * 2])
                  o += snprintf(ln + o, sizeof ln - (size_t)o, " vs_%d:%u/%u", g, fc[2 + g * 2], fc[3 + g * 2]);
              fprintf(stderr, "[RTLOG]   kose denetimi (toplam): NaN/sonsuz %u ucgen, cok uzak %u ucgen | VS (NaN/uzak):%s\n", fc[0], fc[1], o ? ln : " -"); }
          { char ln[512]; int o = 0;      /* VS basina anlik yakalanan kose (hangi golgelendirici yapiya giriyor) */
            for (int g = 1; g < HD_MAXVSI && o < 480; g++) if (cnt[g]) o += snprintf(ln + o, sizeof ln - (size_t)o, " vs_%d:%u", g, cnt[g]);
            fprintf(stderr, "[RTLOG]   anlik yakalama (VS:kose):%s\n", o ? ln : " -"); }
      } }
    /* --- GPU: yakalama --- */
    if (total){
        id<MTLComputeCommandEncoder> ce = pp_compute(cb, "RT: kose yakalama (hesaplama)");
        [ce setBuffer:g_mtlrvb offset:0 atIndex:1];
        [ce setBuffer:g_mtlkb offset:0 atIndex:2];
        [ce setBuffer:g_rt_item[ring] offset:0 atIndex:3];
        [ce setBuffer:g_rt_rec[ring] offset:0 atIndex:4];
        [ce setBuffer:g_rt_pos offset:0 atIndex:5];
        [ce setBuffer:g_rt_attr offset:0 atIndex:6];
        [ce setBuffer:(g_pool_fr[rb] ? g_pool_fr[rb] : g_mtlrvb) offset:0 atIndex:8];   /* indisli kayitlar */
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
            for (int g = 1; g < HD_MAXVSI; g++){            /* VS basina: sayaclar hangi golgelendirici oldugunu soylesin */
                if (!cnt[g]) continue;
                static float poison = -1.f;      /* HD_RT_POISON=<deger>: nedensellik testi (denetim yapilmaz) */
                if (poison < 0.f){ const char *ev = ENVC("HD_RT_POISON"); poison = ev ? (float)atof(ev) : 0.f; }
                struct { uint32_t ntri; float lim; uint32_t first, slot; } F = { cnt[g] / 3u, poison > 0.f ? -poison : 100000.f, off[g], (uint32_t)g };
                if (poison > 0.f && g != 3) continue;        /* yalniz bir VS'nin (dunya) ilk ucgeni */
                [ce setBytes:&F length:sizeof F atIndex:2];
                [ce dispatchThreadgroups:MTLSizeMake((cnt[g] / 3u + tw - 1) / tw, 1, 1)
                   threadsPerThreadgroup:MTLSizeMake(tw, 1, 1)];
            }
        }
        [ce endEncoding];
    }
    static int stage = -1;
    if (stage < 0){ const char *e = ENVC("HD_RT_STAGE"); stage = e ? atoi(e) : 3; }
    int st = (g_ab_mode == 3) ? 2 : (g_ab_mode == 4) ? 1 : stage;      /* HD_RT_AB dilimi */
    if (st == 1) return 0;
    /* --- GPU: sabit depoya kopya + atilanlari sifirla (dejenere ucgen) --- */
    if (ncp || g_rt_nzr || g_rt_need_clear){
        id<MTLBlitCommandEncoder> be = pp_blit(cb, "RT: sabit depoya kopya");
        if (g_rt_need_clear){ g_rt_need_clear = 0;
            [be fillBuffer:g_rt_sposb[0] range:NSMakeRange(0, RT_SMAXV * 12) value:0];
            [be fillBuffer:g_rt_sposb[1] range:NSMakeRange(0, RT_SMAXV * 12) value:0]; }
        for (unsigned q = 0; q < ncp; q++){
            [be copyFromBuffer:g_rt_pos sourceOffset:(NSUInteger)cp[q][0] * 12 toBuffer:g_rt_spos
             destinationOffset:(NSUInteger)cp[q][1] * 12 size:(NSUInteger)cp[q][2] * 12];
            [be copyFromBuffer:g_rt_attr sourceOffset:(NSUInteger)cp[q][0] * 64 toBuffer:g_rt_sattr
             destinationOffset:(NSUInteger)cp[q][1] * 64 size:(NSUInteger)cp[q][2] * 64];
        }
        for (unsigned q = 0; q < g_rt_nzr; q++)
            [be fillBuffer:g_rt_spos range:NSMakeRange((NSUInteger)g_rt_zr[q][0] * 12, (NSUInteger)g_rt_zr[q][1] * 12) value:0];
        [be endEncoding];
    }
    /* --- GPU: alt yapilar + ust yapi (profilde ayri kalemler) --- */
    id<MTLAccelerationStructureCommandEncoder> ae = nil;
    int have_dyn = total >= 3;
    if (have_dyn){
        dsig ^= (uint64_t)total; dsig *= 1099511628211ull;
        int refit = hd_rt_refit_on() && dsig == g_rt_dsig && g_rt_dsince < RT_REFIT_MAX;
        ae = pp_accel(cb, refit ? "RT: anlik yapi (refit)" : "RT: anlik yapi kurulumu");
        MTLAccelerationStructureTriangleGeometryDescriptor *gd =
            [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
        gd.vertexBuffer = g_rt_pos; gd.vertexStride = 12; gd.triangleCount = total / 3; gd.opaque = YES;
        MTLPrimitiveAccelerationStructureDescriptor *ad = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
        ad.geometryDescriptors = @[gd]; ad.usage = hd_rt_dyn_usage();
        if (refit){
            [ae refitAccelerationStructure:g_rt_as descriptor:ad destination:g_rt_as
                             scratchBuffer:g_rt_scratch scratchBufferOffset:0];
            g_rt_dsince++; atomic_fetch_add(&g_rt_nrefit, 1ull);
        } else {
            [ae buildAccelerationStructure:g_rt_as descriptor:ad scratchBuffer:g_rt_scratch scratchBufferOffset:0];
            g_rt_dsince = 0; atomic_fetch_add(&g_rt_nbuild, 1ull);
        }
        g_rt_dsig = dsig;
        [ae endEncoding]; ae = nil;
    } else g_rt_dsince = 0xFFFFFFFFu;
    if (g_rt_sdirty && g_rt_scur >= 3 && (g_rt_sbuilt == 0 || frame - g_rt_last_sbuild >= RT_SREBUILD)){
        ae = pp_accel(cb, "RT: sabit yapi kurulumu");
        MTLAccelerationStructureTriangleGeometryDescriptor *gd =
            [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
        gd.vertexBuffer = g_rt_spos; gd.vertexStride = 12; gd.triangleCount = g_rt_scur / 3; gd.opaque = YES;
        MTLPrimitiveAccelerationStructureDescriptor *ad = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
        ad.geometryDescriptors = @[gd]; ad.usage = MTLAccelerationStructureUsageNone;
        [ae buildAccelerationStructure:g_rt_sas descriptor:ad scratchBuffer:g_rt_sscratch scratchBufferOffset:0];
        g_rt_sbuilt = g_rt_scur; g_rt_last_sbuild = frame; g_rt_sdirty = 0;
        for (unsigned q = 0; q < RT_TABN; q++) if (g_rt_tab[q].state == 2 && g_rt_tab[q].pad) g_rt_tab[q].state = 3;
        for (unsigned q = 0; q < g_rt_mpend_n && g_rt_mfree_n < RT_SMAXE; q++) g_rt_mfree[g_rt_mfree_n++] = g_rt_mpend[q];
        g_rt_mpend_n = 0;
        atomic_fetch_add(&g_rt_sbuilds, 1ull);
        [ae endEncoding]; ae = nil;
    }
    int have_st = g_rt_sbuilt >= 3;
    if (!have_dyn && !have_st) return 0;
    ae = pp_accel(cb, "RT: ust yapi");
    { MTLAccelerationStructureInstanceDescriptor *id2 = (MTLAccelerationStructureInstanceDescriptor *)[g_rt_inst contents];
      /* ornek 0 her zaman var olan yapi; kimlik (accelerationStructureIndex) hangi depo oldugunu soyler */
      id2[0].accelerationStructureIndex = have_dyn ? 0u : 1u;
      id2[1].accelerationStructureIndex = 1u;
      MTLInstanceAccelerationStructureDescriptor *td = [MTLInstanceAccelerationStructureDescriptor descriptor];
      td.instancedAccelerationStructures = @[g_rt_as, g_rt_sas];
      td.instanceCount = (have_dyn && have_st) ? 2 : 1; td.instanceDescriptorBuffer = g_rt_inst;
      [ae buildAccelerationStructure:g_rt_tlas descriptor:td scratchBuffer:g_rt_tscratch scratchBufferOffset:0];
      g_rt_only_static = !have_dyn; }
    [ae endEncoding];
    if (st == 2) return 0;
    /* doku tablosu */
    [g_rt_argenc setArgumentBuffer:g_rt_arg[ring] offset:0];
    for (unsigned q = 0; q < nt; q++) [g_rt_argenc setTexture:g_rt_texs[q] atIndex:q];
    *ntex = nt;
    atomic_fetch_add(&g_rt_frames, 1ull);
    atomic_fetch_add(&g_rt_tris, (unsigned long long)((total + g_rt_sbuilt) / 3));
    atomic_fetch_add(&g_rt_draws, (unsigned long long)ri);
    atomic_fetch_add(&g_rt_dynv, (unsigned long long)total);
    atomic_fetch_add(&g_rt_statv, (unsigned long long)g_rt_sbuilt);
    return (total + g_rt_sbuilt) / 3;
}
static int g_hd_dump_now;       /* bu kare DUMPFRAME ile secildi (tani) */
static void hd_rt_trace(id<MTLCommandBuffer> cb, const void *par, size_t parsz, id<MTLTexture> zt,
                        NSUInteger W, NSUInteger H, unsigned nt){
    id<MTLComputeCommandEncoder> ce = pp_compute(cb, "RT: isin cekirdegi (hesaplama)");
    [ce setComputePipelineState:g_rt_kern];
    [ce setTexture:g_rt_out atIndex:0];
    [ce setTexture:zt atIndex:1];
    [ce setTexture:g_hd_g[0] atIndex:2];
    [ce setTexture:g_hd_g[2] atIndex:4];
    [ce setBytes:par length:parsz atIndex:0];
    [ce setAccelerationStructure:g_rt_tlas atBufferIndex:1];
    [ce useResource:g_rt_as usage:MTLResourceUsageRead];
    [ce useResource:g_rt_sas usage:MTLResourceUsageRead];
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
/* Sahne sonu birlestirmesi: isin izleme (varsa) ya da SSR.  i = sahnenin
 * bittigi cizim indeksi; scr = cozulmus ekran dokusu (drawable). */
/* ---- MSAA: gereksiz saklama / cozme ------------------------------------
 * Olculdu (PASSPROF, 1080p yaris karesi): ekran gecisleri GPU suresinin en
 * buyuk kalemi; karede 5 cok ornekli ekran gecisi var ve HER biri renk +
 * 3 G-tamponu + Z'yi 4x olarak saklayip cozuyordu:
 *     0  yalniz Clear (hemen yansima hedeflerine geciliyor)
 *     13 sahne (1. parca)   14 = ekran disi ara gecis
 *     15 sahne (2. parca)   -> birlestirme -> son islem zinciri
 *     39 son islem dortgeni 40 arayuz
 * Uc duzeltme:
 *  1) Yalniz Clear iceren ekran gecisi acilmaz (sonraki ekran gecisi zaten
 *     ilk kullanimda temizler).
 *  2) Saklama eylemi gecis KAPANIRKEN secilir (MTLStoreActionUnknown):
 *     sirada baska cok ornekli ekran gecisi varsa yalniz SAKLA; ekrani
 *     okuyan bir kopya (birlestirme / son islem) ya da kare sonu varsa
 *     yalniz COZ.  G-tamponu yalniz birlestirmeden hemen once cozulur.
 *  3) Ekrandan ilk kopyadan SONRAKI ekran gecisleri (son islem dortgeni,
 *     arayuz) tek ornekli: birlestirme ve son islem zaten cozulmus goruntuyu
 *     tum orneklere ayni yazdigi icin orada MSAA'nin bilgisi yok.
 * Z'ye dokunulmadi (ikiz esitleme mantigi ayri).  HD_MSLAZY=0 eski davranis. */
static int hd_mslazy(void){
    static int v = -1; if (v < 0){ const char *e = ENVC("HD_MSLAZY"); v = (e && *e == '0') ? 0 : 1; }
    return v;
}
/* Gecis ACILIRKEN: bu gecisten sonra cozulmus goruntu gerekecek mi?
 * Metal kurali: cozme hedefi bagliysa saklama eylemi cozme iceren bir eylem
 * olmak zorunda, yani cozme karari acilista verilmeli (saklama kapanista).
 * Donus: bit0 renk cozulmeli, bit1 okuyucu bir ekran kopyasi (G de cozulmeli). */
static unsigned hd_ms_predict(int rb, unsigned i){
    unsigned n = g_dn[rb], zid = DEPTH_ZID(g_dbuf[rb][i].depth), j = i + 1;
    if (!hd_mslazy()) return 3u;
    for (; j < n; j++){ const MDraw *x = &g_dbuf[rb][j];                 /* gecisin sonu */
        if (x->is_blit || x->target || DEPTH_ZID(x->depth) != zid) break;
        if (x->is_clear && (x->clr_flags & 2u) && zid) break; }
    for (; j < n; j++){ const MDraw *x = &g_dbuf[rb][j];
        if (x->is_blit){
            if (!x->blit_src) return 3u;                                  /* ekran okunuyor */
            if (!x->target) return 1u;                                    /* ekrana kopya: temkinli */
            continue; }
        if (!x->target) return 0u;                                        /* baska bir cok ornekli ekran gecisi */
    }
    return 1u;                                                            /* kare sonu: sunum */
}
/* Bu cok ornekli ekran gecisi BELLEKSIZ eklerle acilabilir mi (yuklemeyi
 * cagiran denetler)?  Gecis bittikten sonra, ekran okunmadan (birlestirme /
 * son islem kopyasi) once ayni karede baska bir ekran gecisi ya da ekrana
 * kopya yoksa hicbir ornegin saklanmasi gerekmez.  Govde tespiti
 * hd_ms_predict ile ayni; kodlama dongusu de gecisi ayni yerlerde kapatir. */
static int hd_ms_memless_ok(int rb, unsigned i){
    unsigned n = g_dn[rb], zid = DEPTH_ZID(g_dbuf[rb][i].depth), j = i + 1;
    for (; j < n; j++){ const MDraw *x = &g_dbuf[rb][j];                 /* gecisin sonu */
        if (x->is_blit || x->target || DEPTH_ZID(x->depth) != zid) break;
        if (x->is_clear && (x->clr_flags & 2u) && zid) break; }
    for (; j < n; j++){ const MDraw *x = &g_dbuf[rb][j];
        if (x->is_blit){
            if (!x->blit_src) return 1;                                   /* ekran okunuyor: sonrasi tek ornekli */
            if (!x->target) return 0;                                     /* ekrana kopya: MS renge saklanir */
            continue; }
        if (!x->target) return 0;                                         /* baska bir ekran gecisi: yukler */
    }
    return 1;                                                             /* kare sonu: yalnizca cozum */
}
static int hd_ms_memless(void){ return hd_ml_hw() && !g_hd_ml_off && hd_mslazy(); }
static int hd_mszstore_all(void){              /* HD_MSZSTORE=1: MS derinlik hep saklanir (eski) */
    static int v = -1; if (v < 0){ const char *e = ENVC("HD_MSZSTORE"); v = (e && *e == '1'); }
    return v;
}
/* Gecis KAPANIRKEN: ornekler saklanmali mi?  Donus: acilista cozme baglanmadigi
 * halde cozum gerektiyse (tahmin tutmadi) bit0 renk / bit1 G: cagiran
 * hd_ms_fallback ile ayri bir cozme gecisi kodlar (sayaci raporda; 0 olmali). */
static _Atomic unsigned long long g_hd_msfb;
static int hd_ms_end(id<MTLRenderCommandEncoder> enc, int rb, unsigned k, int c_ms, int c_gb, unsigned att){
    if (!c_ms || !hd_mslazy()){ g_hd_msp.on = 0; return 0; }
    unsigned n = g_dn[rb], k0 = k; int store = 0, res = 0, reader = 0, pend = 0;
    for (; k < n; k++){ const MDraw *x = &g_dbuf[rb][k];
        if (x->is_blit){
            if (!x->blit_src){ res = 1; reader = 1; break; }
            if (!x->target){ res = 1; store = 1; break; }
            continue; }
        if (!x->target){ store = 1; break; }
    }
    if (k >= n) res = 1;
    if (g_hd_msp.on && g_hd_msp.ml){
        /* Belleksiz gecis: eylemler acilista kesinlesti (yalnizca cozum ya da
         * DontCare; set*StoreAction yalnizca Unknown ile acilan gecise
         * cagrilabilir).  Burada yalnizca ongorunun tuttugu denetlenir:
         * tutmadiysa bu karede ornekler kaybolur (sonraki gecis temizlenmis
         * acar), belleksiz kip kalici olarak kapanir. */
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
    /* MS DERINLIK: eskiden kosulsuz Store -- 4K'da kare basina 133 MB bellek
     * yazimi (~95 fps'te ~12 GB/s).  Bu ornekleri yalnizca ayni Z ile acilan
     * sonraki bir cok ornekli ekran gecisi yukler; olculen karelerde boyle
     * gecis yok ve oyun Z'yi her kare temizliyor.  Simdi: ayni karede yeniden
     * kullanim varsa sakla; temizleniyorsa ya da ekran okundu / kare bitti
     * ise saklama -- ancak gecis temizlemeden (yukleyerek) acildiysa oyun
     * kaliciliga guveniyor demektir, sakla. */
    if (g_hd_msp.on && g_hd_msp.dz){
        unsigned zid = g_hd_msp.zid; int sd = -1;
        for (unsigned q = k0; q < n && sd < 0; q++){ const MDraw *x = &g_dbuf[rb][q];
            if (x->is_blit){ if (!x->blit_src) break; continue; }        /* ekran okundu: sonrasi tek ornekli */
            if (DEPTH_ZID(x->depth) != zid) continue;
            if (x->is_clear && (x->clr_flags & 2u)) sd = 0;               /* yeniden kullanimdan once temizleniyor */
            else if (!x->target) sd = 1;                                  /* ayni Z ile yeni MS gecisi yukler */
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
    int ms = hd_msaa_n() > 1 && g_hd_mscol && !hd_mslazy();     /* tembel kipte sonrasi tek ornekli */
    id<MTLRenderPipelineState> sp = ms ? g_hd_ssr_pso_ms : g_hd_ssr_pso;
    float ivp[16], vp[16], iv[16];
    if (!refl_drawn || !g_hd_cam_ok[rb] || !zt || !sp || !g_hd_g[0] ||
        zt.width != scr.width || zt.height != scr.height) return;
    memcpy(vp, &g_hd_cam[rb][32], sizeof vp);
    for (int r4 = 0; r4 < 4; r4++) vp[r4*4] *= g_hd_cam_k[rb];   /* Hor+ */
    if (!hd_inv4(vp, ivp) || !hd_inv4(&g_hd_cam[rb][0], iv)) return;

    struct { float vp[16], ivp[16], cam[4], prm[4], sz[4], cam2[4], cb[3][4], fogp[4]; } sp2;
    memset(sp2.fogp, 0, sizeof sp2.fogp);
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
    sp2.cam2[3] = ENVC("HD_RT_PLAIN") ? 2.f : ENVC("HD_RT_NOTEX") ? 1.f : 0.f;     /* tani */
    if (g_ab_mode == 2) sp2.cam2[3] = 2.f;
    for (int a3 = 0; a3 < 3; a3++){ memcpy(sp2.cb[a3], &g_hd_cube[rb][a3*3], 12); sp2.cb[a3][3] = 0.f; }
    static int rsc_env = -1;  /* HD_RT_SCALE=1..4: isin tamponu boleni (1 = tam cozunurluk) */
    /* Verilmediyse OTOMATIK: isin sayisi cozunurlukle buyumesin (1080p'de 2, 2560 ve
     * 3024 genislikte 3, 4K'da 4 -> hep ~1000 piksel genisliginde isin tamponu).
     * Olculdu (3024x1964, kuru hava): olcek 2'de izleme + golgeleme ~6 ms. */
    if (rsc_env < 0){ const char *e = ENVC("HD_RT_SCALE"); int v = e ? atoi(e) : 0; rsc_env = (v >= 1 && v <= 4) ? v : 0; }
    int rsc = rsc_env;
    if (!rsc){ int a = (int)((scr.width + 480) / 960); rsc = a < 2 ? 2 : a > 4 ? 4 : a; }
    sp2.cb[0][3] = (float)rsc;

    /* isin izleme: yakala + yapiyi kur + isinlari at (basarisizsa SSR) */
    unsigned rt_tris = 0, rt_nt = 0;
    if (hd_rt_on() && g_ab_mode != 5 && g_ab_mode != 6){
        hd_rt_init();
        float p00 = g_hd_cam[rb][16] * g_hd_cam_k[rb], p11 = g_hd_cam[rb][16 + 5];
        if (g_rt_pso && p00 != 0.f && p11 != 0.f)
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
            /* HD_RTPROBE2 (DUMPFRAME karesinde): AYNI cekirdek + AYNI veri, kare
             * DISINDA tek basina zamanlanir.  Kare ici maliyetle karsilastirmak icin. */
            if (g_hd_dump_now && ENVC("HD_RTPROBE2")){
                static int once = 0;
                if (!once){ once = 1;
                    id<MTLCommandBuffer> pb = [g_queue commandBuffer]; unsigned nt2 = 0;
                    unsigned t2 = hd_rt_frame(pb, rb, i, iv, vp, ivp, 1.f / p00, 1.f / p11, &nt2, sp2.fogp);
                    [pb commit]; [pb waitUntilCompleted];
                    fprintf(stderr, "[RT2] yakalama+kurulum (ayri tampon): %.2f ms, %u ucgen\n",
                            (pb.GPUEndTime - pb.GPUStartTime) * 1e3, t2);
                    double best = 1e30, sum = 0;
                    for (int it = 0; it < 9; it++){
                        id<MTLCommandBuffer> tb = [g_queue commandBuffer];
                        hd_rt_trace(tb, &sp2, sizeof sp2, zt, ow, oh, nt2);
                        [tb commit]; [tb waitUntilCompleted];
                        double msx = (tb.GPUEndTime - tb.GPUStartTime) * 1e3;
                        if (it == 0) continue;
                        if (msx < best) best = msx; sum += msx;
                    }
                    fprintf(stderr, "[RT2] isin cekirdegi (kare disi, tek basina): en iyi %.2f ms, ort %.2f ms\n", best, sum / 8);
                    { rt_tris = hd_rt_frame(cb, rb, i, iv, vp, ivp, 1.f / p00, 1.f / p11, &rt_nt, sp2.fogp); sp2.cb[1][3] = (float)g_rt_only_static; }
                }
            }
            /* --- YARIM ISIN ----------------------------------------------------
             * Isin cekirdegi (3024x1964'te ~2.5 ms) yansima sisteminin en buyuk
             * kalemi.  Her karede satranc tahtasinin yalnizca bir yarisi izlenir;
             * obur yari isin dokusunda kalan onceki karenin sonucudur (120 fps'te
             * 8 ms).  Birlestirmenin 4 noktali buyutmesi ikisini zaten karistirir.
             * Doku yeni yaratildiysa o kare tam izlenir.  HD_RT_HALF=0 eski. */
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
    /* HD_COMP=0 (tani): sahne kopyasi + birlestirme gecisini atla.  HD_RT_STAGE ile
     * birlikte adimlarin GPU bedelini tek tek olcmek icin (goruntude yansima olmaz). */
    { static int comp = -1; if (comp < 0){ const char *e = ENVC("HD_COMP"); comp = (e && *e == '0') ? 0 : 1; }
      if (!comp) return;
      if (g_ab_mode == 1 || g_ab_mode == 2 || g_ab_mode == 3 || g_ab_mode == 4 || g_ab_mode == 6) return; }
    int fb = rt_tris && !ms && g_rt_pso_fb;        /* bkz. KOPYASIZ BIRLESTIRME */
    if (fb){
        MTLRenderPassDescriptor *sr = [MTLRenderPassDescriptor renderPassDescriptor];
        sr.colorAttachments[0].texture = scr;
        sr.colorAttachments[0].loadAction = MTLLoadActionLoad;
        sr.colorAttachments[0].storeAction = MTLStoreActionStore;
        pp_rp(sr, "birlestirme: RT (kopyasiz)");
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
        cd.mipmapLevelCount = 4;     /* SSR araba: hafif yumusak yansima */
        cd.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        cd.storageMode = MTLStorageModePrivate;
        g_hd_scenecopy = [g_dev newTextureWithDescriptor:cd];
    }
    id<MTLBlitCommandEncoder> sc = pp_blit(cb, "birlestirme: sahne kopyasi");
    [sc copyFromTexture:scr sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
             sourceSize:MTLSizeMake(scr.width, scr.height, 1)
              toTexture:g_hd_scenecopy destinationSlice:0 destinationLevel:0
      destinationOrigin:MTLOriginMake(0,0,0)];
    if (!rt_tris) [sc generateMipmapsForTexture:g_hd_scenecopy];     /* yalnizca SSR okur */
    [sc endEncoding];
    MTLRenderPassDescriptor *sr = [MTLRenderPassDescriptor renderPassDescriptor];
    if (ms){ sr.colorAttachments[0].texture = g_hd_mscol;
             sr.colorAttachments[0].resolveTexture = scr;
             sr.colorAttachments[0].storeAction = MTLStoreActionStoreAndMultisampleResolve; }
    else { sr.colorAttachments[0].texture = scr;
           sr.colorAttachments[0].storeAction = MTLStoreActionStore; }
    sr.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    pp_rp(sr, rt_tris ? "birlestirme: RT" : "birlestirme: SSR");
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
/* --- GECIS BIRLESTIRME (iki build) ------------------------------------------
 * 3024x1964'te normal surumde ekran karede 5 ayri gecisle ciziliyordu (PASSPROF
 * icerik dokumu, 2026-10-15): yalniz Clear iceren bos gecis, ana sahne, isik
 * parlamasi hedefi araya girdigi icin TEK cizimlik ikinci sahne gecisi, son
 * islem dortgeni ve yalniz Z baglamasi degistigi icin ayri arayuz gecisi.  Her
 * ayri gecis tam ekran renk (+ Z) okuyup yaziyor (~24 MB her biri).  Uc
 * duzenleme, ucu de pikseli degistirmez:
 *  1) pass_reorder: sahneyi bolen ara gecis ertelenir (asagida, kurallariyla).
 *  2) pass_clear_only: yalniz Clear iceren ekran gecisi acilmaz; ekran ilk
 *     gercek kullaniminda zaten ayni renkle temizleniyor, Z temizligi
 *     g_zpend ile bekliyor.
 *  3) Z'yi ne test eden ne yazan cizim, yalniz Z yuzeyi farkli diye yeni
 *     gecis acmaz.
 * Onceden 1 ve 2 yalniz HD'deydi.  PASSMERGE=0 ucunu de kapatir. */
static int pass_merge_on(void){
    static int v = -1;
    if (v < 0){ const char *e = ENVC("PASSMERGE"); v = (e && *e == '0') ? 0 : 1; }
    return v;
}
/* ---- SAHNEYI BOLEN ARA GECISI ERTELE -----------------------------------
 * Olculdu (PASSPROF icerik dokumu, yaris karesi): ana sahne 674 cizim tek
 * ekran gecisi; ardindan 23 cizimlik bir ekran disi gecis (isik parlamalari:
 * sahnenin Z'sini TEST eder, Z yazmaz), sonra ekrana TEK cizim, sonra son
 * islem.  O tek cizim yuzunden sahne ikinci bir ekran gecisi aciyor: 4x renk
 * + 3 G-tamponu saklanip geri yukleniyordu (~200 MB yazma + ~200 MB okuma).
 * Kalip:  ... ekran | R = ekran disi kosu (tek hedef T) | S = ekran kosusu ...
 * R Z yazmiyor / temizlemiyorsa ve S Z yazmiyor, T'yi orneklemiyor, ayni Z
 * yuzeyini kullaniyorsa iki kosu yer degistirir: S ekranin rengine yazar
 * (R okuyamaz), R yalniz T'ye yazar (S okumuyor), ikisi de Z'yi degistirmez
 * -> sonuc piksel piksel ayni, ama sahne TEK gecis.  HD_REORDER=0 kapatir. */
static _Atomic unsigned long long g_hd_reord;
static void pass_reorder(int rb){
    static int on = -1; if (on < 0){ const char *e = ENVC("HD_REORDER"); on = (e && *e == '0') ? 0 : 1; }
    if (!on || !pass_merge_on()) return;
    unsigned n = g_dn[rb]; MDraw *D = g_dbuf[rb];
    static MDraw tmp[64];
    for (unsigned a = 1; a + 1 < n; a++){
        const MDraw *p = &D[a - 1];
        if (p->target || p->is_blit || p->is_clear) continue;                 /* onceki: ekran cizimi */
        if (!D[a].target || D[a].is_blit || D[a].is_clear) continue;          /* a: ekran disi kosunun basi */
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
        if (c < n && !D[c].target && !D[c].is_blit && !D[c].is_clear) continue;   /* S 64'u asti: kazanc yok */
        unsigned ns = c - b, nr = b - a;
        memcpy(tmp, &D[b], (size_t)ns * sizeof(MDraw));
        memmove(&D[a + ns], &D[a], (size_t)nr * sizeof(MDraw));
        memcpy(&D[a], tmp, (size_t)ns * sizeof(MDraw));
        atomic_fetch_add(&g_hd_reord, 1ull);
        a = c - 1;
    }
}
/* i'deki cizim ekranda yalniz Clear olan bir gecis acacaksa 1: ardindan gelen
 * ilk gercek is baska hedefte, ve ekran okunmadan once ekrana yine cizilecek. */
static int pass_clear_only(int rb, unsigned i){
    unsigned n = g_dn[rb], j = i;
    const MDraw *d0 = &g_dbuf[rb][i];
#if NFS_ENHANCED
    if (!hd_mslazy()) return 0;
#endif
    if (!pass_merge_on() || d0->target || !d0->is_clear || d0->is_blit) return 0;
    while (j < n && g_dbuf[rb][j].is_clear && !g_dbuf[rb][j].target && !g_dbuf[rb][j].is_blit) j++;
    if (j >= n) return 0;
    if (!g_dbuf[rb][j].target) return 0;                          /* gecis gercek cizimle (ya da ekran blitiyle) suruyor */
    for (unsigned k = j; k < n; k++){ const MDraw *x = &g_dbuf[rb][k];
        if (x->is_blit){ if (!x->blit_src || !x->target) return 0; continue; }
        if (!x->target) return 1;
    }
    return 0;
}

/* GERCEK kup doku.  Oyun araba cevre haritasini 6 YUZE ayri ayri
 * ciziyor (olculdu: SetRenderTarget 6 farkli kup yuzeyiyle), sonra
 * dokuyu asama 0'a baglayip yansimayi orneklyor.  Eskiden duz bir 2B
 * doku yaratiliyordu, yani alti yuz ust uste ayni yuzeye yaziliyordu. */
unsigned metal_create_texture_cube(unsigned edge, unsigned levels,
                                   unsigned d3dfmt, unsigned is_rt){
    if (!g_dev || g_texn >= MAXTEX) return 0;
    unsigned blk, bpb;
    MTLPixelFormat pf = d3dfmt_to_mtl(d3dfmt, &blk, &bpb);
    if (pf == MTLPixelFormatInvalid) return 0;
    if (!levels) levels = 1;
    MTLTextureDescriptor *td =
        [MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:pf
                              size:edge mipmapped:(levels > 1)];
    td.mipmapLevelCount = levels;
    td.usage = MTLTextureUsageShaderRead | (is_rt ? MTLTextureUsageRenderTarget : 0);
    /* Private DEGIL: oyun kup yuzlerine LockRect/UnlockRect ile de
     * yaziyor ve replaceRegion Private dokuda gecersiz (olculdu:
     * _validateReplaceRegion assert'i, 9. karede cokme). */
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

void metal_upload_texture_face(unsigned id_, unsigned level, unsigned face,
                               const void *data, unsigned pitch,
                               unsigned w, unsigned h, unsigned d3dfmt){
    if (!id_ || id_ >= MAXTEX || !g_tex[id_] || !data) return;
    unsigned blk, bpb;
    if (d3dfmt_to_mtl(d3dfmt, &blk, &bpb) == MTLPixelFormatInvalid) return;
    unsigned lw = w >> level, lh = h >> level;
    if (!lw) lw = 1; if (!lh) lh = 1;
    MTLRegion rg = MTLRegionMake2D(0, 0, lw, lh);
    struct timespec _u0; if (g_perflog) clock_gettime(CLOCK_MONOTONIC, &_u0);
    if (g_tex_cube[id_]){
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
/* Bildirilen mip sayisi ile GERCEKTEN yuklenen seviyeleri karsilastir.
 * Yuklenmemis seviye = tanimsiz bellek; ucgen kenarlarinda o seviye
 * secilince ekranda 1 piksellik kutu/kosegen olarak gorunuyor. */
void metal_miplevel_report(void){
    unsigned bad = 0, tot = 0;
    for (unsigned i = 1; i < MAXTEX && i < g_texn; i++){
        if (!g_tex[i]) continue;
        unsigned lv = (unsigned)g_tex[i].mipmapLevelCount;
        if (lv <= 1) continue;
        tot++;
        unsigned want = (lv >= 16u) ? 0xFFFFu : ((1u << lv) - 1u);
        if ((g_tex_lvlmask[i] & want) != want){
            if (bad < 6)
                fprintf(stderr,"    doku %u: %u seviye bildirildi, yuklenen maske 0x%04x\n",
                        i, lv, g_tex_lvlmask[i]);
            bad++;
        }
    }
    fprintf(stderr,"  mip seviyeleri    : %u cok-seviyeli doku, %u tanesi EKSIK\n", tot, bad);
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

/* D3DCOLOR (0xAARRGGBB, bellekte B,G,R,A) -> uchar4 */
/* prim: 4=TRIANGLELIST 5=TRIANGLESTRIP 6=TRIANGLEFAN */
static NSString *kPrelude = @
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct VOut { float4 pos [[position]]; float4 col0; float4 col1;\n"
"              float4 uv0; float4 uv1; float4 uv2; float4 uv3;\n"
"              float fog; };\n"
"struct PSPar { float aref; float usec; float pad0; float pad1;\n"
"               float4 proj; float4 c[32]; float4 fog; };\n"
#if NFS_ENHANCED
/* HD: renk + G-tamponu (c1..c3 = G0..G2; icerik: bkz. G-TAMPONU PAKETLEME) */
"struct PSOut { float4 c0 [[color(0)]]; float4 c1 [[color(1)]]; float4 c2 [[color(2)]];\n"
"               float4 c3 [[color(3)]]; };\n"
/* isin izleme yakalamasi (vsemu.c vsk_N ile metal.m HDRecC/HDMatC ayni olmali) */
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
"                      constant PSPar &P [[buffer(0)]]) {\n"
"  float4 c = i.col0 * t.sample(sm, i.uv0.xy);\n"
"  if (c.a < P.aref) discard_fragment();\n"
"  c.rgb = mix(P.fog.rgb, c.rgb, mix(1.0, saturate(i.fog), P.fog.a));\n"
"  return c;\n"
"}\n";

void metal_draw_tv(const TVert *src, unsigned nverts, unsigned prim, unsigned tex){
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
        /* DIKKAT: cizim tamponu KARELER ARASI yeniden kullaniliyor.
         * Yeni bir alan eklenip burada atanmazsa onceki karenin degeri
         * kalir.  is_blit boyle bir alandi: bir kez blit olan yuva
         * sonsuza kadar blit sayilip gercek cizimi bayat dokuyla
         * degistiriyordu (ekran zamanla mozaige donuyordu).  Kayittan
         * once tamamini sifirla. */
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

/* --- guest tarafi API (herhangi bir thread'den cagrilabilir) --- */
void metal_set_clear_color(unsigned argb){
    atomic_store(&g_clear_r, ((argb >> 16) & 0xFF) / 255.0f);
    atomic_store(&g_clear_g, ((argb >>  8) & 0xFF) / 255.0f);
    atomic_store(&g_clear_b, ((argb      ) & 0xFF) / 255.0f);
}
void metal_begin_frame(void){ }

/* Guest: sonraki cizimlerin karisim durumu. */
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
/* Guest: derinlik testi/yazma durumu.  3B modeller Z-tamponu olmadan
 * gonderim sirasina gore ciziliyordu -> gövdenin arkasinda kalmasi
 * gereken poligonlar ustune biniyor, siyah lekeler olusuyordu. */
void metal_set_depth(unsigned mode){ g_cur_dmode = mode & 63u; g_cur_depth = g_cur_dmode | (g_cur_zid << 8); }
/* Guest: SetDepthStencilSurface.  0 = Z yuzeyi yok (D3D'de Z testi/yazmasi
 * etkisiz), 1 = ortuk yuzey, >=2 CreateDepthStencilSurface ile yaratilan. */
void metal_set_zsurf(unsigned zid){
    if (zid >= MAXZ) zid = 0;
    g_cur_zid = zid; g_cur_depth = g_cur_dmode | (g_cur_zid << 8);
}
/* Cihaz Reset'i: oyun cozunurluk/detay degistirince cagirir.
 * Uygulanmadigi icin ayar degistirmek cokuyordu. */
unsigned g_want_w = 0, g_want_h = 0;
/* Cihaz yaratilirken Metal henuz ayakta olmayabilir; istenen boyutu
 * saklayip pencere acilirken kullaniyoruz. */
void metal_want_size(unsigned w, unsigned h){
    if (!w || !h || w > 8192u || h > 8192u) return;
    g_want_w = w; g_want_h = h;
}
/* --- Retina: gercek piksel yogunlugunda ciz ---------------------------
 * Oyunun en buyuk kipi 1280x960.  2x bir ekranda o yuzey iki kat
 * buyutulerek gosteriliyordu, yani her piksel yeniden orneklenip
 * bulaniklasiyordu (Windows ciktisiyla yan yana konunca fark bariz).
 * Cozum: yuzeyi bastan ekranin sahip oldugu kadar pikselle cizmek.
 * ORAN BIREBIR KORUNUR (indirgenmis oranin tam kati secilir), boylece
 * kirpma uzayi duzeltmesi (aspfix) devreye girmez -- saf keskinlik.
 * Konuk thread'den cagrildigi icin NSScreen'e DOKUNMAZ (yalnizca ana
 * thread'de guvenli); ana thread'in olcup sakladigi degeri okur. */
static unsigned u_gcd(unsigned a, unsigned b){
    while (b){ unsigned t = a % b; a = b; b = t; } return a;
}
/* Pencere stili TEK yerde: kullanilabilir alan hesabi pencerenin kendisiyle
 * ayni stili kullanmazsa baslik cubugu payi tutmaz. */
#define NFSU_WIN_STYLE (NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | \
                        NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable)
/* Ekranin kullanilabilir ICERIK alani (nokta) ve olcegi.  Ana thread'de
 * AppKit ile olculup saklanir.  Tahmin YETMEDI: baslik cubugu 28 nokta
 * sanilmisti, centikli ekranda 32 cikti; 3 noktalik fark pencereyi
 * kirpip yeniden orneklemeyi (bulanikligi) geri getirdi. */
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
        /* win_apply_size'in uygulayacagi sinirin birebir aynisi */
        aw = cw; ah = ch; s = s100 / 100.0;
    } else {
        /* Onbellek yoksa CoreGraphics ile tahmin (thread-guvenli). */
        CGDirectDisplayID d = CGMainDisplayID();
        CGRect b = CGDisplayBounds(d);                /* NOKTA cinsinden */
        if (b.size.width < 320.0 || b.size.height < 240.0) return;
        CGDisplayModeRef m = CGDisplayCopyDisplayMode(d);
        if (m){ size_t pw = CGDisplayModeGetPixelWidth(m);
                if (pw) s = (double)pw / b.size.width;
                CGDisplayModeRelease(m); }
        /* menu cubugu / centik (<=32) + baslik cubugu (<=32) */
        aw = b.size.width; ah = b.size.height - 64.0;
    }
    if (s < 1.0) s = 1.0;
    if (aw < 320.0 || ah < 240.0) return;
    unsigned g = u_gcd(gw, gh); if (!g) return;
    unsigned ax = gw / g, ay = gh / g;                /* indirgenmis oran */
    double u = fmin(aw * s / (double)ax, ah * s / (double)ay);
    /* GPU yuku ust siniri: bu yasin bir oyunu icin ~6 MP fazlasiyla yeter.
     * 5K/6K bir ekranda sinirsiz buyutmek kare butcesini yer. */
    double maxpix = 6.0e6;
    { const char *e = ENVC("NFSMAXPIX"); if (e && *e) maxpix = atof(e); }
    if (maxpix > 1.0e5){
        double up = sqrt(maxpix / ((double)ax * (double)ay));
        if (up < u) u = up;
    }
    unsigned units = (unsigned)u;
    if (!units) return;
    unsigned w = ax * units, h = ay * units;
    if (w <= gw || h <= gh || w > 8192u || h > 8192u) return;   /* kazanc yok */
    *ow = w; *oh = h;
}
/* --- ornekleyici onbellegi ------------------------------------------- */
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
    /* MIPLIN: D3DTEXF_POINT mip'i uc seviyeli (trilinear) yap.  POINT mip
     * seviyeyi SERT seciyor; ucgen kenarinda turev dortgeni disari tastigi
     * icin LOD kil payi degisiyor ve tam kenar boyunca 2 piksellik bir
     * bantta bir ust mip'e atliyor -> her dortgenin cercevesinde ve iki
     * ucgenin paylastigi kosegende gorunur cizgi. */
    static int mlin = -1;
    if (mlin < 0){ const char *e = ENVC("MIPLIN"); mlin = (e && *e && *e!='0'); }
    sd.mipFilter = ENVC("MIPOFF") ? MTLSamplerMipFilterNotMipmapped
                 : (mp==0u) ? MTLSamplerMipFilterNotMipmapped
                 : (mp==1u && !mlin) ? MTLSamplerMipFilterNearest
                            : MTLSamplerMipFilterLinear;
    NSUInteger an = (NSUInteger)1u << al;
    if (an < 1) an = 1; if (an > 16) an = 16;
    /* LODMAX=<n>: mip secimini n. seviyeyle sinirla.  Kenar cizgisinin
     * LOD'un NE KADAR sicradigini olcmek icin. */
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
    else { g_smp_overflow++;      /* onbellek doldu: her cizimde YENI nesne */
           static int warned = 0;
           if (!warned){ warned = 1;
               fprintf(stderr,"metal: ORNEKLEYICI ONBELLEGI DOLDU (%d) - "
                       "her cizimde yeni nesne uretiliyor\n", MAXSMP); } }
    return st;
}

/* Ana thread'de cagrilmali.  Pencereyi, w x h PIKSELlik cizim yuzeyini
 * BIREBIR gosterecek sekilde boyutlar: icerik = piksel / ekran olcegi.
 * Eskiden icerik dogrudan w x h NOKTA yapiliyordu; 2x Retina'da bu, 1280
 * piksellik yuzeyin 2560 piksellik alana buyutulmesi demekti -- goruntu
 * bulanikti.  Simdi 1 oyun pikseli = 1 ekran pikseli. */
/* Pencere boyutu cozunurlukten BAGIMSIZ: ilk acilista bir kez ekranin
 * kullanilabilir alanina gore boyutlanir, sonra yalnizca kullanici
 * degistirir.  Cozunurluk degisince yalnizca cizim yuzeyi (kalite)
 * degisir; katman onu pencereye en-boy koruyarak olcekler.  (Kullanici
 * istegi 2026-09-27: "pencere sabit boyut, oyun ufalmali cozunurluk
 * olarak" -- Windows'ta da boyle.)  Ayar menusu satirlari pencerenin
 * oranindan uretildigi icin normalde bant olusmaz (bkz. com.c
 * modes_refresh). */
static _Atomic int g_win_sized = 0;
static void win_apply_size(unsigned w, unsigned h){
    if (!g_layer) return;
    CGFloat s = g_win ? [g_win backingScaleFactor]
                      : [[NSScreen mainScreen] backingScaleFactor];
    if (s < 1.0) s = 1.0;
    g_layer.contentsScale = s;          /* birakilirsa 1.0 kaliyor */
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
    fprintf(stderr, "metal: %s: cizim %ux%u piksel, icerik %.0fx%.0f nokta (olcek %.2g) -> %s\n",
            fs ? "tam ekran" : "pencere", w, h, (double)c.width, (double)c.height, (double)s,
            exact ? "1:1" : "pencereye olcekleniyor");
}
/* Ayar menusu satirlarinin dayandigi GORUNUM (piksel): tam ekranda tam
 * ekran icerigi, penceredeyse pencerenin su anki icerigi; pencere henuz
 * boyutlanmadiysa ilk acilista alacagi boyut.  Guest thread'den cagrilir,
 * AppKit'e dokunmaz. */
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
/* Guest'in etkin piksel gölgelendiricisi ve sabitleri. */
void metal_set_ps(int psid, const float *c32){
    g_cur_ps = psid;
    if (c32 && memcmp(g_cur_psc, c32, sizeof g_cur_psc)){
        memcpy(g_cur_psc, c32, sizeof g_cur_psc); g_psc_ver++; }
}
void metal_set_texproj(const float p4[4]){ memcpy(g_cur_proj, p4, sizeof g_cur_proj); }
/* --- GPU kose gölgelendiricileri ---------------------------------------
 * Olculdu: calisma suresinin ~%45'i CPU'daki vs_run yorumlayicisindaydi.
 * Oyunun kendi bytecode'u MSL'ye cevrilip burada derlenir. */
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
                fprintf(stderr, "kose gölgelendirici %d derlenemedi: %s\n", vsid,
                        err ? [[err localizedDescription] UTF8String] : "?"); }
            return;
        }
        char nm[32]; snprintf(nm, sizeof nm, "vs_%d", vsid);
        g_vsfn[vsid] = [lib newFunctionWithName:[NSString stringWithUTF8String:nm]];
        if (!g_vsfn[vsid])
            fprintf(stderr, "kose gölgelendirici %d: fonksiyon bulunamadi\n", vsid);
#if NFS_ENHANCED
        if (vsid < HD_MAXVSI && hd_rt_on()){
            snprintf(nm, sizeof nm, "vsk_%d", vsid);
            id<MTLFunction> kf = [lib newFunctionWithName:[NSString stringWithUTF8String:nm]];
            NSError *ke = nil;
            if (kf) g_hd_vsk[vsid] = [g_dev newComputePipelineStateWithFunction:kf error:&ke];
            if (!g_hd_vsk[vsid]) fprintf(stderr, "[HD] yakalama cekirdegi vs_%d kurulamadi: %s\n", vsid,
                                         ke ? [[ke localizedDescription] UTF8String] : "fonksiyon yok");
        }
#endif
    }
}
int metal_vs_ready(int vsid){
    return (vsid > 0 && vsid < MAXVSF && g_vsfn[vsid] != nil) ? 1 : 0;
}

/* Ham koseleri (donusturulmemis) + sabitleri kuyruga koy; kose islemini
 * GPU yapar.  kdata = c0..c[nk-1], nk = vs_kmax. */
/* g_qlock altinda: sabit blogunu yerlestir (tekrarliysa yeniden kullan) ve
 * cizim kaydini doldur.  Kose kaynagini (rawoff / indis alanlari, count)
 * cagiran kurar ve g_dn'yi arttirir.  NULL = sabit arenasi doldu. */
static MDraw *raw_rec(int b, unsigned stride, unsigned tex, int vsid,
                      const float *kdata, unsigned nk){
    unsigned ksz = 16u + nk * 16u;
    /* Sabit blogu tekrarlaniyorsa yeniden yukleme: oyun ayni nesnenin
     * parcalarini pes pese ciziyor, blok cogunlukla ayni. */
    static unsigned last_koff[2], last_ksz[2]; static int last_vs[2];
    unsigned koff;
    uint8_t hdr[16]; memset(hdr, 0, sizeof hdr); memcpy(hdr, &stride, 4);
    /* Asama basina TEXCOORDINDEX (2'ser bit) -> kose golgelendiricisi secimi
     * kendisi yapsin; piksel tarafinda koordinat saf interpolant kalir. */
    { uint32_t ts = 0;
      for (int q = 0; q < 4; q++)
          ts |= ((uint32_t)((int)g_cur_proj[q]) & 3u) << (8*q);
      memcpy(hdr + 4, &ts, 4); }
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
    memset(D, 0, sizeof *D);          /* yeniden kullanilan yuva: bkz. metal_draw_tv */
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
    return D;
}
int metal_draw_raw(const void *src, unsigned nverts, unsigned stride,
                   unsigned prim, unsigned tex, int vsid,
                   const float *kdata, unsigned nk){
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
    MDraw *D = raw_rec(b, stride, tex, vsid, kdata, nk);
    if (!D){ pthread_mutex_unlock(&g_qlock); return 0; }

    uint8_t *dst = g_rvbuf[b] + roff;
    /* UCGEN LISTESI: kaynak zaten bitisik, tek blok kopya yeter.
     * Eskiden KOSE BASINA memcpy cagriliyordu (olculdu: kare basina
     * ~1250 cizim x ~90 kose = 112 bin kucuk memcpy/kare; profilde
     * _platform_memmove konuk thread'in %4.6'si).  Serit ve yelpazede
     * koseler paylasildigi icin toplama sart. */
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
    g_dn[b]++;
    pthread_mutex_unlock(&g_qlock);
    return 1;
}
/* Havuzdaki yansilardan indisli ucgen listesi (bkz. KALICI KOSE / INDIS
 * HAVUZU).  vbo: 0. kosenin havuz ofseti, ibo: ilk indisin ofseti. */
int metal_draw_ix(unsigned vbo, unsigned stride, unsigned ibo, unsigned isz, unsigned nidx,
                  int base, unsigned tex, int vsid, const float *kdata, unsigned nk,
                  unsigned long long gh){
    if (!g_pool || nidx < 3 || !stride || stride > 256u || (isz != 2u && isz != 4u)) return 0;
    pthread_mutex_lock(&g_qlock);
    int b = g_cur;
    if (g_dn[b] >= MAXD){ pthread_mutex_unlock(&g_qlock); return 0; }
    MDraw *D = raw_rec(b, stride, tex, vsid, kdata, nk);
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

/* Ceviriden gelen MSL kaynagini derleyip sakla (id basina bir kez). */
/* DIKKAT: parametre adi "id" olamaz - ObjC'nin id turunu golgeler ve
 * "id<MTLLibrary>" ifadesi ayristirilamaz. */
void metal_register_ps(int psid, const char *msl){
    { const char *pd = ENVC("PSDUMP");          /* virgullu liste destekli */
      if (pd){ char want[16]; int n = snprintf(want, sizeof want, "%d", psid);
        for (const char *q = pd; q && *q; ){
            while (*q==',' || *q==' ') q++;
            if (!*q) break;
            if (!strncmp(q, want, (size_t)n) && (q[n]==0 || q[n]==',' || q[n]==' ')){
                fprintf(stderr, "=== ps %d MSL ===\n%s\n=== son ===\n", psid, msl);
                break; }
            while (*q && *q != ',') q++; } } }
    /* PSISO=<psid>:<ifade> : gölgelendiricinin ARA TERIMLERINDEN birini
     * dogrudan cikisa ver.  Hangi girdinin patladigini tahmin etmek yerine
     * ekranda goruruz (t[0] diffuse, t[1] far dokusu, t[2] rampa, v[0]
     * kose rengi, P.c[0] sabit). */
    char *iso = 0;
    { const char *pe = ENVC("PSISO");
      if (pe && *pe){ const char *cl = strchr(pe, ':');
        /* PSISO=*:  -> her gölgelendirici kendi kimliginden turetilmis
         * SABIT bir renk versin; hangi yuzeyi hangi ps'in cizdigini
         * ekrandan okuyabilelim. */
        char autoex[96];
        if (cl && pe[0] == '*' && cl == pe + 1){
            /* Alfa ORIJINAL kalmali: sabitlersek karisim bozulur ve
             * kare tamamen dagilir. */
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
                    (void)dc;   /* alfa testi korunur: siluet dogru kalsin */
                    snprintf(iso + head, need - head, "return float4(%s);\n}\n", ex);
                    msl = iso;
                    fprintf(stderr, "  [psiso] ps %d -> %s\n", psid, ex);
                } else { free(iso); iso = 0; }
            }
        } } }
    if (psid <= 0 || psid >= MAXPS || !msl || !g_dev || g_psfn[psid]){ free(iso); return; }
    @autoreleasepool {
        NSString *src = [kPrelude stringByAppendingString:
                         [NSString stringWithUTF8String:msl]];
        NSError *err = nil;
        struct timespec _c0; if (g_perflog) clock_gettime(CLOCK_MONOTONIC, &_c0);
        id<MTLLibrary> lib = [g_dev newLibraryWithSource:src options:nil error:&err];
        if (g_perflog){ struct timespec _c1; clock_gettime(CLOCK_MONOTONIC, &_c1);
            perflog_event(PL_PS, (uint64_t)(_c1.tv_sec - _c0.tv_sec) * 1000000000ull + (uint64_t)_c1.tv_nsec - (uint64_t)_c0.tv_nsec, 0); }
        if (!lib){
            static int shown = 0;
            if (shown < 3){ shown++;
                fprintf(stderr, "ps%d derlenemedi: %s\n", psid,
                        err ? [[err localizedDescription] UTF8String] : "?"); }
            return;
        }
        char nm[32]; snprintf(nm, sizeof nm, "ps_%d", psid);
        g_psfn[psid] = [lib newFunctionWithName:[NSString stringWithUTF8String:nm]];
        if (!g_psfn[psid]) fprintf(stderr, "ps%d: fonksiyon bulunamadi\n", psid);
    }
    free(iso);
}

/* D3DCMPFUNC (1..8) -> Metal karsilastirma islevi */
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
/* Guest: sonraki cizimler bu hedefe gitsin (0 = ekran). */
void metal_set_render_target(unsigned texid){
    pthread_mutex_lock(&g_qlock);
    g_cur_target = texid;
    pthread_mutex_unlock(&g_qlock);
}
/* Guest: etkin hedefi temizle (cizim sirasinda dogru yerde uygulanir). */
/* StretchRect: oyun sahneyi ARKA TAMPONDAN bir dokuya kopyaliyor ve
 * son-islem (bleach bypass, motion blur, DOF) o dokudan okuyor.  Hic
 * uygulanmamisti (olculdu: oturumda 2261 cagri) -> kaynak doku bos
 * kaliyor, birlestirme siyah uretiyordu.  Kopyayi cizim listesine
 * SIRALI bir giris olarak koyuyoruz; sunum sirasinda tam ekran dortgen
 * olarak yurutulur (olcekleme ve bicim donusumu bedava gelir).        */
void metal_blit(unsigned src_tex, unsigned dst_tex){
    pthread_mutex_lock(&g_qlock);
    int b = g_cur;
    if (g_dn[b] < MAXD){
        MDraw *D = &g_dbuf[b][g_dn[b]];
        memset(D, 0, sizeof *D);
        D->target = dst_tex; D->blit_src = src_tex; D->is_blit = 1;
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
        { static unsigned ov = 0xFFFFFFFFu;      /* CLEARCOL=0xAARRGGBB: tani */
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
    /* Onceki kare henuz ekrana konmadiysa BEKLE.  Beklemezsek guest bir
     * sonraki karenin uzerine yaziyor ve ekran araliklarla bos kaliyor
     * (kullanicinin gordugu "siyah olup geliyor" hatasi).               */
    if (g_ready >= 0){
        struct timespec ts, _w0; clock_gettime(CLOCK_MONOTONIC,&_w0);
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 60L * 1000000L;             /* en fazla 60 ms */
        if (ts.tv_nsec >= 1000000000L){ ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        int late = 0;
        while (g_ready >= 0){
            /* Sure doldu ama ana thread onceki kareyi hala KODLUYOR: birakma,
             * bekle.  Takas g_cur'u onun okudugu tampona cevirirdi (bkz.
             * g_enc_rb).  Kodlama biter bitmez sinyal gelir. */
            if (late && g_enc_rb >= 0){ pthread_cond_wait(&g_qcond, &g_qlock); continue; }
            if (pthread_cond_timedwait(&g_qcond, &g_qlock, &ts) != 0){
                if (g_enc_rb >= 0){
                    if (!late){ late = 1;
                        unsigned long n = atomic_fetch_add(&g_enc_late, 1ul);
                        if (n < 8) g_enc_late_f[n] = atomic_load(&g_frames_drawn) + 1; }
                    continue;
                }
                atomic_fetch_add(&g_frames_dropped, 1);
                break;                             /* takilmayalim */
            }
        }
        { struct timespec _w1; clock_gettime(CLOCK_MONOTONIC,&_w1);
          atomic_fetch_add(&g_wait_present_ns,
            (unsigned long long)(_w1.tv_sec-_w0.tv_sec)*1000000000ull
            + (unsigned long long)_w1.tv_nsec - (unsigned long long)_w0.tv_nsec); }
    }
    g_pool_fr[g_cur] = g_pool;       /* bu karenin indisli cizimleri bu havuzda */
    g_gframe++;
    g_ready = g_cur;                 /* ana thread bu tamponu cizsin */
    g_cur ^= 1;                      /* guest digerine yazmaya devam etsin */
    g_vn[g_cur] = 0; g_dn[g_cur] = 0;
    g_rvn[g_cur] = 0; g_kn[g_cur] = 0; g_pscn[g_cur] = 0;
#if NFS_ENHANCED
    g_hd_cam_ok[g_cur] = 0;
#endif
    atomic_store(&g_frame_pending, 1);
    pthread_cond_signal(&g_rcond);      /* ana thread hemen alsin */
    pthread_mutex_unlock(&g_qlock);
}

long metal_frames_dropped(void){ return atomic_load(&g_frames_dropped); }
unsigned long long metal_wait_present_ns(void){ return atomic_load(&g_wait_present_ns); }
int metal_thermal_state(void){ return (int)[[NSProcessInfo processInfo] thermalState]; }

/* PERFLOG ekran goruntusu: drawable'in tam boy kopyasi (GPU blit, paylasimli
 * doku); tamamlaninca arka plan kuyrugunda her k. piksel okunup ~480 piksel
 * genisliginde JPEG yazilir.  Oyun thread'lerinde is yok; onceki goruntu
 * yazilmadan yenisi alinmaz. */
static id<MTLTexture> g_pl_tex;
static _Atomic int g_pl_busy = 0;
static void pl_thumb(id<MTLCommandBuffer> cb, id<MTLTexture> src, long fno){
    if (atomic_load(&g_pl_busy)) return;
    NSUInteger W = src.width, H = src.height;
    if (g_pl_tex && (g_pl_tex.width != W || g_pl_tex.height != H)) g_pl_tex = nil;
    if (!g_pl_tex){
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:src.pixelFormat
                                       width:W height:H mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead; td.storageMode = MTLStorageModeShared;
        g_pl_tex = [g_dev newTextureWithDescriptor:td];
        if (!g_pl_tex) return;
    }
    atomic_store(&g_pl_busy, 1);
    id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
    [bl copyFromTexture:src sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
             sourceSize:MTLSizeMake(W, H, 1) toTexture:g_pl_tex destinationSlice:0
       destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
    [bl endEncoding];
    id<MTLTexture> tex = g_pl_tex;
    double tms = perflog_now_us() / 1e3;
    [cb addCompletedHandler:^(id<MTLCommandBuffer> b){
        (void)b;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
            NSUInteger k = (W + 479) / 480, w = W / k, h = H / k;
            uint8_t *row = malloc(W * 4), *out = malloc(w * h * 4);
            if (row && out){
                for (NSUInteger y = 0; y < h; y++){
                    [tex getBytes:row bytesPerRow:W * 4 fromRegion:MTLRegionMake2D(0, y * k, W, 1) mipmapLevel:0];
                    for (NSUInteger x = 0; x < w; x++) memcpy(out + (y * w + x) * 4, row + x * k * 4, 4);
                }
                CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
                CGContextRef cg = CGBitmapContextCreate(out, w, h, 8, w * 4, cs,
                    kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
                if (cg){
                    CGImageRef img = CGBitmapContextCreateImage(cg);
                    char path[1100];
                    snprintf(path, sizeof path, "%s/ekran/%09.0f_%ld.jpg", g_pl_dir, tms, fno);
                    CFStringRef sp = CFStringCreateWithCString(NULL, path, kCFStringEncodingUTF8);
                    CFURLRef url = CFURLCreateWithFileSystemPath(NULL, sp, kCFURLPOSIXPathStyle, false);
                    CGImageDestinationRef dst = CGImageDestinationCreateWithURL(url, CFSTR("public.jpeg"), 1, NULL);
                    if (dst){ CGImageDestinationAddImage(dst, img, NULL); CGImageDestinationFinalize(dst); CFRelease(dst); }
                    CFRelease(url); CFRelease(sp); CGImageRelease(img); CGContextRelease(cg);
                }
                CGColorSpaceRelease(cs);
            }
            free(row); free(out);
            atomic_store(&g_pl_busy, 0);
        });
    }];
}

/* Ana thread'den, guest baslamadan ONCE cagrilir.
 * Oyun ilk dokularini pencere acilmadan yaratiyor; cihaz o an hazir
 * degilse dokular sessizce kayboluyordu. */
void metal_early_init(void){
    if (g_dev) return;
    /* Ekran geometrisini konuk thread baslamadan ONCE, ana thread'de olc:
     * konuk CreateDevice'ta cizim boyutunu buna gore seciyor (boot.c bu
     * fonksiyonu pthread_create'ten once cagiriyor). */
    [NSApplication sharedApplication];
    scr_cache();
    /* App Nap / zamanlayici birlestirme KAPALI.  Oyun penceresi on
     * planda degilken macOS sureci arka plan QoS'una dusuruyor ve is
     * VERIMLILIK cekirdeklerine kayiyor -- olculdu: ayni ikili, ayni
     * sahne, 10 ms yerine 36 ms/kare (3.4x).  clang ayni kabuktan tam
     * hizda kosuyordu, yani makine degil UYGULAMA kisilyordu. */
    { static id act; if (!act)
        act = [[NSProcessInfo processInfo] beginActivityWithOptions:
                 NSActivityUserInitiated | NSActivityLatencyCritical
                 reason:@"oyun karesi uretimi"];
      (void)act; }
    dump_init();
    g_dev = MTLCreateSystemDefaultDevice();
    if (!g_dev){ fprintf(stderr, "metal: cihaz bulunamadi\n"); return; }
    g_queue = [g_dev newCommandQueue];
    @autoreleasepool {
        g_vbuf[0] = malloc(sizeof(MVert)*MAXV); g_vbuf[1] = malloc(sizeof(MVert)*MAXV);
        g_dbuf[0] = malloc(sizeof(MDraw)*MAXD); g_dbuf[1] = malloc(sizeof(MDraw)*MAXD);
        g_pscb[0] = malloc(sizeof *g_pscb[0] * MAXD); g_pscb[1] = malloc(sizeof *g_pscb[1] * MAXD);
        for (int i = 0; i < NFB; i++){           /* bkz. "kare basina tampon halkasi" */
            g_fb_vb[i]  = fb_fit(nil, 0, sizeof(MVert)*MAXV);
            g_fb_rvb[i] = fb_fit(nil, 0, MAXRV);
            g_fb_kb[i]  = fb_fit(nil, 0, MAXKB);
        }
        g_fb_sem = dispatch_semaphore_create(NFB);
        g_mtlvb = g_fb_vb[0]; g_mtlrvb = g_fb_rvb[0]; g_mtlkb = g_fb_kb[0];
        /* StretchRect kopyalari icin sabit tam ekran dortgen. */
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
        g_kbuf[0]  = malloc(MAXKB); g_kbuf[1]  = malloc(MAXKB);
        NSError *err = nil;
        id<MTLLibrary> lib = [g_dev newLibraryWithSource:kShaderSrc options:nil error:&err];
        g_lib = lib;
        if (!lib){
            fprintf(stderr,"metal: shader derlenemedi: %s\n",
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

            MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
            pd.vertexFunction   = [lib newFunctionWithName:@"vmain"];
            pd.fragmentFunction = [lib newFunctionWithName:@"fmain"];
            pd.vertexDescriptor = vd;
            pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
            pd.colorAttachments[0].blendingEnabled = YES;
            pd.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
            pd.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
            pd.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
            pd.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
            NSError *e3 = nil;
            g_pso = [g_dev newRenderPipelineStateWithDescriptor:pd error:&e3];
            if (!g_pso) fprintf(stderr,"metal: pipeline kurulamadi: %s\n",
                                [[e3 localizedDescription] UTF8String]);
#if NFS_ENHANCED
            if (hd_msaa_n() > 1){
                g_pso_ms_desc = [pd copy];
                (void)hd_pso_ms();                  /* acilistaki ornek sayisi icin simdiden */
            }
            if (hd_ssr_on()){
                NSError *e4 = nil;
                id<MTLLibrary> sl = [g_dev newLibraryWithSource:hd_gb_src(kSSRSrc) options:nil error:&e4];
                if (!sl) fprintf(stderr, "[HD] SSR gölgelendiricisi derlenemedi: %s\n",
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
                    if (!g_hd_ssr_pso) fprintf(stderr, "[HD] SSR boru hatti kurulamadi: %s\n",
                                               [[e4 localizedDescription] UTF8String]);
                }
            }
#endif
        }
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
        sd.minFilter = MTLSamplerMinMagFilterLinear;
        sd.magFilter = MTLSamplerMinMagFilterLinear;
        /* MIPOFF=1: mip secimini kapat (tani).  Ucgen kenarlarinda turev
         * tahmini sicradiginda kaba mip secilip 1 piksellik cizgi olusuyor
         * mu, bunu ayirt etmek icin. */
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
    fprintf(stderr, "metal: cihaz hazir (%s), cizim yolu %s\n",
            [[g_dev name] UTF8String], g_pso ? "HAZIR" : "YOK");
}

/* --- karisim durumuna gore pipeline onbellegi -------------------------
 * D3D9'un sabit fonksiyon karisim ayarlari Metal'de pipeline'a gomulu
 * oldugu icin her farkli kombinasyon icin ayri pipeline gerekir.        */
#if NFS_ENHANCED
#define MAXPSO 2048         /* + MSAA ve G-tamponu bitleri */
#else
#define MAXPSO 512
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
    /* 12/13 = BOTHSRCALPHA / BOTHINVSRCALPHA (eski, yalniz SRCBLEND'de) */
    case 12: return MTLBlendFactorSourceAlpha;
    case 13: return MTLBlendFactorOneMinusSourceAlpha;
    /* 14/15 = BLENDFACTOR / INVBLENDFACTOR.  EKSIKTI ve ikisi de One'a
     * dusuyordu: son-islem birlestirmesi (SRC=14, DEST=15) capraz gecis
     * yerine "sahne + sahne" yapiyor, yarista zemin bastan basa patlak
     * beyaz oluyordu. */
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

    /* anahtar: karisim(0..19) | ps<<20 | vs<<32 | (HD) msaa<<48 | G<<49 | log2(ornek)<<50 */
    int psid = (int)((key >> 20) & 0xFFFu);
    int vsid = (int)((key >> 32) & 0xFFFu);
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    /* GPU kose gölgelendiricisi kendi kose verisini tampondan okur:
     * vertexDescriptor VERILMEZ (yoksa Metal stage_in bekler). */
    pd.vertexFunction   = (vsid > 0 && vsid < MAXVSF && g_vsfn[vsid])
                          ? g_vsfn[vsid]
                          : [g_lib newFunctionWithName:@"vmain"];
    pd.fragmentFunction = (psid > 0 && psid < MAXPS && g_psfn[psid])
                          ? g_psfn[psid]
                          : [g_lib newFunctionWithName:@"fmain"];
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
        /* G-tamponu: karisimli cizimler ve sabit islevli gölgelendirici
         * (ps 0, tek cikisli) yazmaz -- altindaki yuzeyin verisi kalir. */
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

/* DUMPTEX=<id>: dokuyu tam ekran bir dortgene cizip PNG olarak yaz. */
static void dump_one_texture(unsigned id_, long frame){
    if (!g_dev || !g_pso || id_ >= MAXTEX || !g_tex[id_]) return;
    NSUInteger w = g_tex[id_].width, h = g_tex[id_].height;
    if (!w || !h) return;
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
    /* DUMPMIP=<seviye>: yalnizca o mip seviyesini goster (doku gorunumu ile).
     * Yuklenen mip icerigini dogrudan denetlemek icin. */
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
        } else fprintf(stderr,"metal: doku %u seviye %lu yok (toplam %lu)\n",
                       id_,(unsigned long)lv,(unsigned long)g_tex[id_].mipmapLevelCount); } }
    [enc setFragmentTexture:srct atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
    [enc endEncoding];
    [cb commit]; [cb waitUntilCompleted];
    fprintf(stderr, "metal: doku %u (%lux%lu, %lu seviye) dokumu\n", id_,
            (unsigned long)w, (unsigned long)h,
            (unsigned long)g_tex[id_].mipmapLevelCount);
    dump_texture(off, tag);
}

/* Oyun kendi imlecini ciziyor; macOS imleci de gorunurse ekranda iki imlec
 * oluyor.  Oyun alanindayken sistemin imlecini gizle, cikinca geri getir. */
static int g_cur_hidden = 0;
static void cursor_hide_update(int inside){
    if (inside && !g_cur_hidden){ [NSCursor hide];   g_cur_hidden = 1; }
    else if (!inside && g_cur_hidden){ [NSCursor unhide]; g_cur_hidden = 0; }
}
@interface NfsuView : NSView @end
@implementation NfsuView
- (BOOL)wantsUpdateLayer { return YES; }
- (CALayer *)makeBackingLayer {
    CAMetalLayer *l = [CAMetalLayer layer];
    l.device = g_dev;
    l.pixelFormat = MTLPixelFormatBGRA8Unorm;
    l.framebufferOnly = NO;   /* kare dokumu icin okunabilir olmali */
    return l;
}
@end

/* Tam ekran tercihi kosular arasi hatirlanir.  NFSFULL=1/0 onu gecersiz
 * kilar (olcum koşulari NFSFULL=0 ile kullanicinin tercihinden bagimsiz). */
static void fs_pref_save(int on){
    const char *e = ENVC("NFSFULL");
    if (e && *e) return;                 /* zorlanmis kip tercihi degistirmesin */
    [[NSUserDefaults standardUserDefaults] setBool:on ? YES : NO forKey:@"NFSUFullScreen"];
}
static int fs_pref_load(void){
    const char *e = ENVC("NFSFULL");
    if (e && *e) return e[0] != '0';
    return [[NSUserDefaults standardUserDefaults] boolForKey:@"NFSUFullScreen"] ? 1 : 0;
}
@interface NfsuDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate> @end
@implementation NfsuDelegate
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)s { (void)s; return YES; }
- (BOOL)windowShouldClose:(NSWindow *)w { (void)w;
    fprintf(stderr, "app: pencere kapatildi -> cikis\n");
    atomic_store(&g_should_quit, 1); return YES; }
/* Kullanici pencereyi buyuttugunde ya da tam ekrana gectiginde icerik
 * boyutu degisir; fare eslemesi bunu bilmezse imlec kayar. */
/* Tam ekrana girince icerik alanini olc, tercihi sakla ve oyunu yeni
 * boyuta Reset ettir (bkz. g_fs). */
- (void)windowDidEnterFullScreen:(NSNotification *)n {
    NSWindow *w = [n object];
    NSSize c = [[w contentView] bounds].size;
    CGFloat s = [w backingScaleFactor]; if (s < 1.0) s = 1.0;
    atomic_store(&g_fs_pw, (int)lround(c.width * s));
    atomic_store(&g_fs_ph, (int)lround(c.height * s));
    atomic_store(&g_cont_w, (int)c.width); atomic_store(&g_cont_h, (int)c.height);
    atomic_store(&g_fs, 1);
    fs_pref_save(1);
    fprintf(stderr, "app: tam ekran %.0fx%.0f nokta = %dx%d piksel -> cihaz yeniden kuruluyor\n",
            (double)c.width, (double)c.height, atomic_load(&g_fs_pw), atomic_load(&g_fs_ph));
    com_request_reset(); }
- (void)windowDidExitFullScreen:(NSNotification *)n { (void)n;
    atomic_store(&g_fs, 0);
    fs_pref_save(0);
    fprintf(stderr, "app: pencere kipine donuldu -> cihaz yeniden kuruluyor\n");
    com_request_reset(); }
- (void)windowDidResignKey:(NSNotification *)n { (void)n; cursor_hide_update(0); }
/* Kullanici pencereyi elle boyutlandirdi: cizim cozunurlugu yeni oranla
 * yeniden secilsin (oyunun kendi Reset'i ile, bkz. com_request_reset). */
- (void)windowDidEndLiveResize:(NSNotification *)n {
    NSSize c = [[[n object] contentView] bounds].size;
    atomic_store(&g_cont_w, (int)c.width); atomic_store(&g_cont_h, (int)c.height);
    if (!atomic_load(&g_fs)) com_request_reset(); }
- (void)windowDidResize:(NSNotification *)n {
    NSSize c = [[[n object] contentView] bounds].size;
    atomic_store(&g_cont_w, (int)c.width);
    atomic_store(&g_cont_h, (int)c.height); }
/* Cmd-Q: dogrudan terminate ETME.  Konuk thread'i ortasinda kesmek
 * kapanis yarisina yol aciyor (bkz. sub_0050d510).  Cizim dongusune "dur"
 * deyip CANCEL donuyoruz; dongu cikinca main() raporu yazip sonlaniyor.
 *
 * LATER denendi ve KULLANILAMAZ: AppKit onayi beklerken KENDI run
 * loop'unu suruyor, bizim cizim dongumuz hic geri donmuyor -> kapanis
 * asiliyor.  Cancel'in tek yan etkisi oturum kapatmada bir uyari; uygulama
 * zaten hemen ardindan kendi kendine cikiyor. */
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)s {
    (void)s; fprintf(stderr, "app: cikis istegi (Cmd-Q / Dock)\n");
    atomic_store(&g_should_quit, 1); return NSTerminateCancel; }
@end

/* Ana thread'de cagrilmali. Pencereyi acar ve NSApp dongusunu surer;
 * geri donmez (uygulama kapanana kadar).                                */
void metal_run_main_loop(void){
    @autoreleasepool {
        metal_early_init();
        if (!g_dev) return;

        [NSApplication sharedApplication];
        /* NOFOCUS=1 (test kosulari): Dock simgesi yok, odak calmaz; pencere
         * arkada acilir.  Kullanicinin klavyesi test penceresine gitmez. */
        [NSApp setActivationPolicy:(ENVC("NOFOCUS") && ENVC("NOFOCUS")[0] != '0')
                                   ? NSApplicationActivationPolicyAccessory
                                   : NSApplicationActivationPolicyRegular];
        NfsuDelegate *del = [NfsuDelegate new];
        [NSApp setDelegate:del];
        /* [NSApp run] cagirmiyoruz (olay dongusu asagida elle suruluyor).
         * finishLaunching normalde run'in ilk isi: AppleEvent isleyicilerini
         * kaydeder.  Onsuz uygulama "Cik" komutuna (Dock > Cik, oturum
         * kapatma, osascript quit) hic yanit vermiyor -- surec asili kaliyor. */
        [NSApp finishLaunching];

        /* Menu cubugu.  Olay dongusu asagida elle surdugu icin AppKit
         * bunu kendiliginden kurmuyor: menusuz bir uygulamada Cmd-Q,
         * Cmd-H, Cmd-M calismaz ve pencere "takilmis" gibi gorunur. */
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
            /* macOS'un standart kisayolu Ctrl+Cmd+F (menu esi olmadan
             * hicbir yere baglanmiyordu); menude gizli durur. */
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

        /* Pencere boyutu SABIT 1280x720 idi: oyun CreateDevice ile baska
         * bir cozunurluk istese bile (kayit defterindeki g_RacingResolution)
         * pencere degismiyordu.  Cihaz Metal'den ONCE yaratilabildigi icin
         * istenen boyutu buradan okuyoruz. */
        unsigned rw = g_want_w ? g_want_w : 1280u, rh = g_want_h ? g_want_h : 720u;
        /* Oyun cihazini henuz yaratmadiysa yer tutucu da Retina olcekli
         * olsun; yoksa pencere once yarim boyutta (640x360 nokta) acilip
         * CreateDevice gelince buyuyor.  Oyunun kiplerinin neredeyse
         * tamami 4:3 oldugu icin yer tutucu 1280x960 oraninda: en yaygin
         * durumda pencere CreateDevice'ta hic ziplamiyor. */
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
        /* StretchRect arka tampondan okuyabilsin diye gerekli. */
        g_layer.framebufferOnly = NO;
        /* NOVSYNC=1: vsync'i kapat.  Yalnizca OLCUM icin -- kapaliyken
         * goruntu yirtilir, 1:1 sadakat bozulur. */
        if (ENVC("NOVSYNC")) g_layer.displaySyncEnabled = NO;

        /* Yesil dugme tam ekrana gecsin.  Cizim yuzeyi oyunun istedigi
         * boyutta kaliyor; en-boy oranini KORUYARAK buyutulsun, yoksa
         * 5:4 goruntu genis ekranda yayiliyor. */
        [g_win setCollectionBehavior:[g_win collectionBehavior] |
                NSWindowCollectionBehaviorFullScreenPrimary];
        g_layer.contentsGravity = kCAGravityResizeAspect;
        g_win.backgroundColor = [NSColor blackColor];

        /* Cizim yuzeyi ile pencereyi birebir eslestir (bkz. win_apply_size). */
        win_apply_size(rw, rh);
        [g_win center];
        [g_win setAcceptsMouseMovedEvents:YES];
        /* NOFOCUS=1: arka planda ac, kullanicinin calistigi uygulamadan
         * odagi ALMA (otomatik test/olcum kosulari icin). */
        if (ENVC("NOFOCUS") && ENVC("NOFOCUS")[0] != '0'){
            /* NOFOCUS=2: pencere HIC gorunmesin (tam saydam, fareyi yutmaz).
             * Pencere sunucusu acisindan ekranda kaldigi icin drawable akisi
             * ve DUMPFRAME aynen calisir; =1'de pencere arkada ama gorunurdu. */
            if (ENVC("NOFOCUS")[0] == '2'){ [g_win setAlphaValue:0.0];
                                            [g_win setIgnoresMouseEvents:YES];
                                            [g_win setHasShadow:NO]; }
            [g_win orderBack:nil];
        }
        else { [g_win makeKeyAndOrderFront:nil];
               [NSApp activateIgnoringOtherApps:YES]; }
        if (fs_pref_load()) [g_win toggleFullScreen:nil];

        /* cizim kaynaklari metal_early_init() icinde kuruldu */
        fprintf(stderr, "metal: pencere acildi (%s), cizim yolu %s\n",
                [[g_dev name] UTF8String], g_pso ? "HAZIR" : "YOK");

        /* QUITFRAMES=<n>: n kare sunulunca TEMIZ cik.  ALARM yolu
         * sinyal isleyicisinden _exit ediyor; o yol atexit'i (ve PGO
         * profil yazimini) atliyor. */
        long quitat = 0;
        { const char *e = ENVC("QUITFRAMES"); if (e && *e) quitat = atol(e); }

        /* Cizim dongusu: 60 Hz'de kuyruktaki kareyi sun */
        while (!atomic_load(&g_should_quit)){
            if (quitat && atomic_load(&g_frames_drawn) >= quitat){
                atomic_store(&g_should_quit, 1); break; }
            rt_free_pending();
            /* FSFAKE=<kare>[,...]: pencereye dokunmadan tam ekran gecisini
             * OYUNUN gozunden taklit et (gorunum = ekran boyutu, cihaz kaybi);
             * arka planda gercek tam ekran guvenilir degil (test). */
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
                  fprintf(stderr, "app: FSFAKE kare %ld -> %s (%dx%d)\n", atomic_load(&g_frames_drawn),
                          on ? "tam ekran" : "pencere", atomic_load(&g_fs_pw), atomic_load(&g_fs_ph));
                  com_request_reset(); } }
            /* FSSIM=<kare>:<k|m|c>[,...]: o karede SENTETIK girdi kuyruga
             * koy (test): k = fn+F, m = Ctrl+Cmd+F, c = yesil dugmeye tik.
             * Olaylar gercek girdi gibi asagidaki olay dongusunden gecer. */
            { static long sf[8]; static char sm[8]; static int sn = -1, si = 0;
              if (sn < 0){ sn = 0; const char *e = ENVC("FSSIM");
                  for (const char *p = e; p && *p && sn < 8; ){
                      sf[sn] = atol(p); while (*p && *p != ':' && *p != ',') p++;
                      sm[sn] = (*p == ':') ? p[1] : 'k'; sn++;
                      while (*p && *p != ',') p++; if (*p) p++; } }
              if (si < sn && atomic_load(&g_frames_drawn) >= sf[si]){
                  char md = sm[si++]; NSInteger wn = [g_win windowNumber];
                  fprintf(stderr, "app: FSSIM kare %ld kip %c\n", atomic_load(&g_frames_drawn), md);
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
            /* FSTOGGLE=<kare>[,<kare>...]: o karelerde tam ekran gecisi (test) */
            { static long tg[8]; static int tn = -1, ti = 0;
              if (tn < 0){ tn = 0; const char *e = ENVC("FSTOGGLE");
                  for (const char *p = e; p && *p && tn < 8; ){
                      tg[tn++] = atol(p); while (*p && *p != ',') p++; if (*p) p++; } }
              if (ti < tn && atomic_load(&g_frames_drawn) >= tg[ti]){
                  ti++; fprintf(stderr, "app: FSTOGGLE kare %ld\n", atomic_load(&g_frames_drawn));
                  [g_win toggleFullScreen:nil]; } }
            @autoreleasepool {
                NSEvent *ev;
                while ((ev = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                untilDate:nil
                                                   inMode:NSDefaultRunLoopMode
                                                  dequeue:YES])){
                    NSEventType et = [ev type];
                    if (et == NSEventTypeKeyDown || et == NSEventTypeKeyUp){
                        /* Cmd'li kisayollar OYUNA DEGIL menuye gitmeli:
                         * aksi halde Cmd-Q calismaz.  Oyunun kendisi Cmd
                         * kullanmiyor, catisma yok. */
                        /* macOS'un tam ekran kisayolu fn/Globe+F: Command
                         * tasimadigi icin oyuna "F" olarak gidiyordu. */
                        if ([ev keyCode] == 0x03 &&
                            ([ev modifierFlags] & NSEventModifierFlagFunction)){
                            if (et == NSEventTypeKeyDown && ![ev isARepeat])
                                [g_win toggleFullScreen:nil];
                            continue; }
                        if ([ev modifierFlags] & NSEventModifierFlagCommand){
                            [NSApp sendEvent:ev]; continue; }
                        unsigned char dik = mac_to_dik([ev keyCode]);
                        if (dik) atomic_store(&g_keys[dik], et == NSEventTypeKeyDown ? 1 : 0);
                        continue;                    /* oyuna gitsin, sisteme degil */
                    }
                    /* Fare olayi oyun alaninin DISINDAysa (baslik cubugu:
                     * kapat/kucult/tam ekran dugmeleri, pencereyi surukleme,
                     * kenardan boyutlandirma) AppKit'e gitmeli.  Eskiden
                     * tum tiklar oyuna yutuluyordu; yesil dugme calismiyordu. */
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

                /* Olay kuyrugu disindaki run-loop kaynaklarini da isle.
                 * AppleEvent'ler (Dock > Cik, "quit" komutu, oturum
                 * kapatma) buradan gelir; yalnizca nextEventMatching...
                 * cagirilirsa uygulama Cik komutuna hic yanit vermiyor. */
                [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                         beforeDate:[NSDate distantPast]];

                /* AUTOKEY=<dik>[,<dik>...]: oyunun girise tepki verip
                 * vermedigini olcmek icin donemsel sentetik tus basisi. */
                /* AUTOCLICK=1: donemsel sentetik sol tik + kucuk fare hareketi.
                 * Girisin oyuna ulasip ulasmadigini olcmek icin. */
                if (ENVC("AUTOCLICK")){
                    long f = atomic_load(&g_frames_drawn);
                    long ph = f % 120;
                    if (ph == 0){ atomic_fetch_add(&g_mouse_dx, 3);
                                  atomic_fetch_add(&g_mouse_dy, 2); }
                    if (ph == 60) atomic_fetch_or(&g_mouse_btn, 1u);
                    if (ph == 70) atomic_fetch_and(&g_mouse_btn, ~1u);
                }
                /* AUTOSEQ="<kare>:<dik>,<kare>:<dik>,...": belirtilen karede
                 * bir kez tus bas (10 kare boyunca).  AUTOKEY surekli
                 * basiyor; menuleri gezip belirli bir sahneye ulasmak icin
                 * betiklenebilir bir diziye ihtiyac vardi. */
                /* AUTOSEQ="<kare>:<dik>[:<sure>],...": kareden itibaren
                 * <sure> kare boyunca tusa bas (varsayilan 8).  Sure
                 * gerekiyordu: arabayi surmeden farlarin yuzeye vurusunu
                 * goremiyoruz. */
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
                    /* sinir asilirsa SESSIZCE kesilmesin (bir testte sondaki
                     * Enter'lar kayboldu, oyun "takildi" sanildi) */
                    while (q && (*q == ',' || *q == ' ')) q++;
                    if (q && *q) fprintf(stderr, "app: UYARI AUTOSEQ %d girdide kesildi, kalanlar yok sayildi\n", SQMAX);
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
                  /* AUTOKEYSTOP=<kare>: bu kareden sonra basmayi birak.
                   * AUTOKEY surekli Enter'a bastigi icin yaris oncesi ARA
                   * SAHNE atlaniyordu; onu gorebilmek icin gerekliydi. */
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
                pthread_mutex_lock(&g_qlock); have_new = (g_ready >= 0);
                pthread_mutex_unlock(&g_qlock);
                if (!have_new && atomic_load(&g_frame_pending)) g_skip_empty++;
                if (have_new && atomic_exchange(&g_frame_pending, 0)){
                    struct timespec _d0,_d1; clock_gettime(CLOCK_MONOTONIC,&_d0);
                    id<CAMetalDrawable> d = [g_layer nextDrawable];
                    clock_gettime(CLOCK_MONOTONIC,&_d1);
                    atomic_fetch_add(&g_wait_drawable_ns,
                      (unsigned long long)(_d1.tv_sec-_d0.tv_sec)*1000000000ull
                      + (unsigned long long)_d1.tv_nsec - (unsigned long long)_d0.tv_nsec);
                    if (d){
                        struct timespec _e0; clock_gettime(CLOCK_MONOTONIC,&_e0);
                        id<MTLCommandBuffer> cb = [g_queue commandBuffer];
                        pp_begin(atomic_load(&g_frames_drawn) + 1);
                        int ab_count = 0;
                        g_ab_mode = ab_frame(atomic_load(&g_frames_drawn) + 1, &ab_count);
                        int ab_mode = g_ab_mode;
                        /* GPU'nun hala okuyor olabilecegi tamponun ustune yazma:
                         * bu kare icin bos bir takim al (kilidi tutmadan bekle). */
                        int fbi = fb_acquire();
                        pthread_mutex_lock(&g_qlock);
                        int rb = g_ready;
                        g_enc_rb = rb;
                        /* Kilidi BURADA birak.  Eskiden tum kare kodlamasi
                         * boyunca (0.4-2 ms), sonra da kose kopyasi boyunca
                         * tutuluyordu; metal_draw_raw cizim basina ayni kilidi
                         * istedigi icin konuk thread o pencerede her cizimde
                         * blokluyordu -- cift tamponlamanin amaci tam da bu
                         * ortusmeydi.  Guvenli: g_enc_rb kuruldugu andan
                         * itibaren rb yalnizca ana thread'in; konuk hep
                         * digerine (g_cur) yaziyor ve metal_present g_enc_rb
                         * >= 0 iken (zaman asiminda bile) takas yapmiyor. */
                        pthread_mutex_unlock(&g_qlock);
                        if (rb >= 0){
                            g_fb_vb[fbi]  = fb_fit(g_fb_vb[fbi],  sizeof(MVert)*(NSUInteger)g_vn[rb], sizeof(MVert)*MAXV);
                            g_fb_rvb[fbi] = fb_fit(g_fb_rvb[fbi], g_rvn[rb], MAXRV);
                            g_fb_kb[fbi]  = fb_fit(g_fb_kb[fbi],  g_kn[rb],  MAXKB);
                        }
                        g_mtlvb = g_fb_vb[fbi]; g_mtlrvb = g_fb_rvb[fbi]; g_mtlkb = g_fb_kb[fbi];
                        if (rb >= 0){
                            fb_put(g_mtlvb,  g_vbuf[rb],  sizeof(MVert)*(NSUInteger)g_vn[rb]);
                            fb_put(g_mtlrvb, g_rvbuf[rb], g_rvn[rb]);
                            fb_put(g_mtlkb,  g_kbuf[rb],  g_kn[rb]);
                        }

#if NFS_ENHANCED
                        hd_msaa_frame(d.texture.width, d.texture.height);   /* bkz. COZUNURLUGE GORE MSAA */
#endif
                        /* Cizimler hedeflerine gore siraya dizili; hedef
                         * degistikce yeni bir render gecisi acilir. */
                        id<MTLRenderCommandEncoder> enc = nil;
                        unsigned cur_t = 0xFFFFFFFFu;
                        unsigned cur_z = 0xFFFFFFFFu;   /* gecisin Z yuzeyi */
                        /* --- kodlayici durumu onbellegi ---------------------
                         * Kare basina ~1350 cizim var; her birinde 4 ornekleyici,
                         * 532 baytlik sabit tamponu ve 4 doku KOSULSUZ baglaniyordu.
                         * Degismeyeni yeniden baglamak Metal'de bedava degil. */
                        id<MTLRenderPipelineState> c_pso = nil;
                        unsigned c_smp[4] = {~0u,~0u,~0u,~0u};
                        unsigned c_tex[4] = {~0u,~0u,~0u,~0u};
                        unsigned c_bcol = ~0u;
                        unsigned c_cull = ~0u;
                        unsigned c_depth = ~0u;
                        int c_zon = 0;                  /* gecisin gercek Z yuzeyi var */
                        int c_ms = 0;                   /* (HD) gecis cok ornekli */
                        int c_gb = 0;                   /* (HD) gecis G-tamponu tasiyor */
                        int hd_post = 0;                /* (HD) ekrandan ilk kopya gecti: son islem asamasi */
                        unsigned c_att = 0;             /* (HD) gecise cozme hedefi baglandi: bit0 renk, bit1 G */
                        (void)hd_post; (void)c_att;
                        if (rb >= 0) pass_reorder(rb);  /* sahneyi bolen ara gecisi ertele (bkz. GECIS BIRLESTIRME) */
#if NFS_ENHANCED
                        int hd_scene_open = 1;          /* ekrandan ilk kopyaya kadar ana sahne */
                        int hd_gb_first = 1;            /* G-tamponu bu karede ilk kez */
                        int hd_refl_drawn = 0;          /* bu karede yansima varyanti cizildi */
                        unsigned hd_scene_zid = 0;      /* ana sahnenin Z yuzeyi */
#endif
#if NFS_ENHANCED
                        memset(g_hd_zms_used, 0, sizeof g_hd_zms_used);
#endif
                        struct { float aref, usec, p0, p1; float proj[4]; float c[PSC_N][4]; float fogv[4]; } c_fp;
                        int c_fp_valid = 0;
                        unsigned c_psci = ~0u;          /* c_fp'nin ps sabiti havuz sirasi */
                        unsigned char touched[MAXTEX + 1];   /* (doku*8 + yuz) bit'i */
                        memset(touched, 0, sizeof touched);
                        int screen_touched = 0;

                        long fno_pre = atomic_load(&g_frames_drawn) + 1;
                        if (ENVC("FRAMELOG") && rb >= 0 && (fno_pre % 10) == 0)
                            fprintf(stderr,"  [kare %3ld] cizim=%u kose=%u\n",
                                    fno_pre, g_dn[rb], g_vn[rb]);
                        int  dumpnow = g_dump_n && dump_wanted(fno_pre);
                        /* GLYPHDUMP: her dortgenin ekran kutusu + UV'si.
                         * Amac: oyunun yazdigi METNI cozup durumunu okumak. */
                        if (ENVC("GLYPHDUMP") && dumpnow && rb >= 0){
                            fprintf(stderr,"  [glif] kare %ld, %u cizim\n", fno_pre, g_dn[rb]);
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
                            fprintf(stderr,"  [kare %ld] %u cizim, %u kose\n",
                                    fno_pre, g_dn[rb], g_vn[rb]);
                            { unsigned hist[16]; memset(hist,0,sizeof hist);
                              unsigned other=0;
                              for (unsigned q=0;q<g_dn[rb];q++){
                                  unsigned tg=g_dbuf[rb][q].target;
                                  if (tg<16) hist[tg]++; else other++; }
                              fprintf(stderr,"    hedef dagilimi:");
                              for (unsigned q=0;q<16;q++) if (hist[q])
                                  fprintf(stderr," %s=%u", q?"":"ekran", hist[q]),
                                  (q? fprintf(stderr,"(doku%u)",q):0);
                              if (other) fprintf(stderr," diger=%u", other);
                              fprintf(stderr,"\n"); }
                            for (unsigned i = 0; i < g_dn[rb] && i < 200; i++){
                                MDraw *dr = &g_dbuf[rb][i];
                                fprintf(stderr,"    #%u hedef=%u doku=%u kose=%u karisim=%05x ps=%d smp=%04x",
                                        i, dr->target, dr->tex, dr->count, dr->blend, dr->ps, dr->smp[0]);
                                if (dr->count >= 3){
                                    MVert *v0 = &g_vbuf[rb][dr->first];
                                    /* dortgenin sinirlari + renk: neden gorunmedigini anlamak icin */
                                    float xmin=1e30f,xmax=-1e30f,ymin=1e30f,ymax=-1e30f;
                                    for (unsigned q=0;q<dr->count;q++){
                                        float w2 = v0[q].w != 0.f ? v0[q].w : 1.f;
                                        float X = v0[q].x/w2, Y = v0[q].y/w2;
                                        if(X<xmin)xmin=X; if(X>xmax)xmax=X;
                                        if(Y<ymin)ymin=Y; if(Y>ymax)ymax=Y;
                                    }
                                    fprintf(stderr,"  x[%.3g..%.3g] y[%.3g..%.3g]"
                                            " renk=(%.2g %.2g %.2g a=%.2g) uv=(%.3g,%.3g)",
                                            xmin,xmax,ymin,ymax,
                                            v0[0].r,v0[0].g,v0[0].b,v0[0].a, v0[0].t[0][0], v0[0].t[0][1]);
                                }
                                fprintf(stderr,"\n");
                            }
                        }
                        /* SLIVER=1: ekrani boydan boya kesen ama alani
                         * sifira yakin ucgenleri bul.  Ekranda 1 piksellik
                         * kosegen cizgiler bunlardan geliyor; hangi cizim
                         * cagrisinin urettigini tahmin etmek yerine olcuyoruz. */
                        /* DRAWV=<n>: n numarali cizimin TUM koselerini bas.
                         * Iki ucgenin paylastigi kenarda dikis olup olmadigini
                         * ancak ham degerleri gorerek anlayabiliriz. */
                        /* DRAWSUM: karenin cizimlerini (hedef,doku,ps,karisim)
                         * anahtarina gore topla.  Binlerce satir yerine yapiyi
                         * bir bakista gostersin. */
                        if (rb >= 0 && ENVC("DRAWSUM") && dump_wanted(fno_pre)){
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
                            fprintf(stderr,"  [ozet] kare %ld: %u cizim, %d grup\n",
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
                                  fprintf(stderr,"    hedef=%u(%ux%u) doku=%u(%ux%u) doku1=%u(%ux%u) doku2=%u"
                                        " ps=%d karisim=%05x smp=%04x  %u cizim %u kose\n",
                                        g->tg,dw,dh,g->tx,tw,th,g->t1,aw,ah,g->t2,
                                        g->ps,g->bl,g->sm,g->nd,g->nv); }
                                g->nv = 0;
                            }
                        }
                        /* DRAWLIST=1 (DUMPFRAME ile): her cizimin tam durumu +
                         * sabit ozetleri; iki kosunun ilk farkli cizimini bulmak icin. */
                        if (rb >= 0 && ENVC("DRAWLIST") && dump_wanted(fno_pre)){
                            for (unsigned di = 0; di < g_dn[rb]; di++){
                                MDraw *dr = &g_dbuf[rb][di];
                                uint64_t hp = 1469598103934665603ull, hk = 1469598103934665603ull;
                                const uint8_t *pp = (const uint8_t *)g_pscb[rb][dr->psci];
                                for (size_t q = 0; q < sizeof g_pscb[rb][0]; q++){ hp ^= pp[q]; hp *= 1099511628211ull; }
                                if (dr->vsid > 0 && g_kbuf[rb]){
                                    const uint8_t *kp = (const uint8_t *)g_kbuf[rb] + dr->koff;
                                    for (unsigned q = 0; q < dr->ksz && dr->koff + q < MAXKB; q++){ hk ^= kp[q]; hk *= 1099511628211ull; }
                                }
                                fprintf(stderr,"  [dl] %4u t=%x z=%x cull=%u bl=%05x ps=%d vs=%d tx=%u,%u,%u,%u n=%u clr=%u blit=%u psc=%016llx vk=%016llx\n",
                                        di, dr->target, dr->depth, dr->cull, dr->blend, dr->ps, dr->vsid,
                                        dr->tex, dr->tex1, dr->tex2, dr->tex3, dr->count, dr->is_clear, dr->is_blit,
                                        (unsigned long long)hp, (unsigned long long)hk);
                            }
                        }
                        if (rb >= 0 && ENVC("DRAWV") && dump_wanted(fno_pre)){
                            unsigned di = (unsigned)atoi(ENVC("DRAWV"));
                            if (di < g_dn[rb]){
                                MDraw *dr = &g_dbuf[rb][di];
                                fprintf(stderr,"  [kose] cizim#%u vsid=%d kose=%u doku=%u\n",
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
                            hd_rt_probe(rb, d.texture, fno_pre);
                        g_hd_dump_now = dump_wanted(fno_pre);
                        /* CAMLOG=1 (DUMPFRAME ile): karenin kamerasi + dunya
                         * geometrisi cizen ilk birkac cizimin kose sabitleri. */
                        if (rb >= 0 && ENVC("CAMLOG") && dump_wanted(fno_pre)){
                            fprintf(stderr, "  [kamera] kare %ld gecerli=%d\n", fno_pre, g_hd_cam_ok[rb]);
                            static const char *mn[3] = { "gorunum", "projeksiyon", "carpim" };
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
                                fprintf(stderr, "    cizim#%u ps=%d vs=%d kose=%u sabitler c0..c%u:\n",
                                        di, dr->ps, dr->vsid, dr->count, nk ? nk-1 : 0);
                                for (unsigned q = 0; q < nk; q++)
                                    fprintf(stderr, "      c%-2u %11.5f %11.5f %11.5f %11.5f\n", q,
                                            k[q*4], k[q*4+1], k[q*4+2], k[q*4+3]);
                                shown++;
                            }
                        }
#endif
                        /* DRAWVPS=<ps>: o gölgelendiriciyi kullanan TUM cizimlerin
                         * koselerini, dort doku koordinati kumesiyle birlikte dok. */
                        if (rb >= 0 && ENVC("DRAWVPS") && dump_wanted(fno_pre)){
                            int want = atoi(ENVC("DRAWVPS"));
                            for (unsigned di = 0; di < g_dn[rb]; di++){
                                MDraw *dr = &g_dbuf[rb][di];
                                if (dr->ps != want) continue;
                                fprintf(stderr,"  [kose-ps] cizim#%u ps=%d vsid=%d kose=%u hedef=%u doku=%u,%u,%u,%u\n",
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
                            /* Kare icinde ps basina cizim/ucgen sayimi.  DRAWSUM
                             * grup tablosu 192'de doluyor ve SON gecisler
                             * listelenmiyordu; bu histogram hepsini kapsar. */
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
                            fprintf(stderr, "  [pshist] kare %ld: %u cizim, %d farkli ps\n",
                                    fno_pre, g_dn[rb], hn);
                            for (int q = 0; q < hn; q++)
                                fprintf(stderr, "     ps=%-4d %5u cizim %7u kose  karisim=%05x\n",
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
                                        fprintf(stderr,"  [sliver] cizim#%u ucgen%u doku=%u"
                                                " karisim=%05x ps=%d kose=%u  uzunluk=%.3f alan=%.2e\n"
                                                "      (%.4f,%.4f) (%.4f,%.4f) (%.4f,%.4f)"
                                                "  renk=(%.2f %.2f %.2f a=%.2f)\n",
                                                i, t/3, dr->tex, dr->blend, dr->ps, dr->count,
                                                sp, ar, X[0],Y[0], X[1],Y[1], X[2],Y[2],
                                                v0[t].r, v0[t].g, v0[t].b, v0[t].a);
                                    }
                                }
                            }
                            if (!nrep) fprintf(stderr,"  [sliver] kare %ld: hic bulunamadi\n", fno_pre);
                        }
                        if (rb >= 0 && g_pso){
                            for (unsigned i = 0; i < g_dn[rb]; i++){
                                MDraw *dr = &g_dbuf[rb][i];
                                { static int sk = -2;
                                  if (sk == -2){ const char *e=ENVC("SKIPDRAW"); sk = e?atoi(e):-1; }
                                  if (sk >= 0 && (int)i == sk) continue; }
                                /* SKIPPS=<n>[,<n>...]: bu piksel gölgelendiricisini
                                 * kullanan TUM cizimleri atla.  Bir gecisin katkisini
                                 * ayirmanin en dogrudan yolu. */
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
                                  if (skip) continue; }
                                if (dr->is_blit){
#if NFS_ENHANCED
                                    /* gecerli olsun olmasin: hd_ms_end bu kopyayi gorup ornekleri saklamamis olabilir */
                                    if (!dr->blit_src && hd_mslazy()) hd_post = 1;
#endif
                                    /* Kaynak/hedef dokuyu coz; ekran = drawable */
                                    id<MTLTexture> st2 = dr->blit_src
                                        ? ((dr->blit_src < MAXTEX) ? g_tex[dr->blit_src] : nil)
                                        : d.texture;
                                    id<MTLTexture> dt2 = dr->target
                                        ? ((TGT_ID(dr->target) < MAXTEX)
                                             ? g_tex[TGT_ID(dr->target)] : nil)
                                        : d.texture;
                                    if (st2 && dt2 && st2 != dt2){
                                        if (ENVC("BLITLOG")){
                                            static unsigned seen[32]; static int sn = 0;
                                            unsigned key = (dr->blit_src<<16) ^ dr->target;
                                            int nw = 1;
                                            for (int q=0;q<sn;q++) if (seen[q]==key) nw = 0;
                                            if (nw && sn < 32){ seen[sn++] = key;
                                                fprintf(stderr,"  [blit] doku %u (%lux%lu) -> doku %u (%lux%lu)  olcek %.2fx\n",
                                                    dr->blit_src,(unsigned long)st2.width,(unsigned long)st2.height,
                                                    dr->target,(unsigned long)dt2.width,(unsigned long)dt2.height,
                                                    (double)st2.width/(double)(dt2.width?dt2.width:1)); }
                                        }
#if NFS_ENHANCED
                                        if (enc){ int pe = hd_ms_end(enc, rb, i, c_ms, c_gb, c_att);
                                                  [enc endEncoding]; enc = nil; cur_t = 0xFFFFFFFFu; cur_z = 0xFFFFFFFFu;
                                                  if (pe) hd_ms_fallback(cb, d.texture, pe); }
#endif
                                        if (enc){ [enc endEncoding]; enc = nil; cur_t = 0xFFFFFFFFu; cur_z = 0xFFFFFFFFu; }
#if NFS_ENHANCED
                                        /* SSR birlestirme: ana sahne ekrandan ILK kez kopyalaniyor
                                         * (son islem basliyor).  Sahne + G-tamponu + Z hazir. */
                                        if (st2 == d.texture && hd_scene_open){
                                            hd_scene_open = 0;
                                            hd_composite(cb, rb, i, d.texture, hd_scene_zid, hd_refl_drawn);
                                        }
                                        /* MSAA: ekran icerigi cok ornekli dokuda yasiyor.
                                         * Drawable'a dogrudan yazilan kopya bir sonraki
                                         * ekran gecisinin cozumuyle EZILIRDI; kopyayi
                                         * cok ornekli dokuya dortgen olarak ciz, coz. */
                                        if (dt2 == d.texture && hd_msaa_n() > 1 && hd_pso_ms() && !hd_post){
                                            g_hd_mscol = hd_ms_tex(g_hd_mscol, d.texture.pixelFormat,
                                                                   d.texture.width, d.texture.height);
                                            MTLRenderPassDescriptor *mp =
                                                [MTLRenderPassDescriptor renderPassDescriptor];
                                            mp.colorAttachments[0].texture = g_hd_mscol;
                                            mp.colorAttachments[0].resolveTexture = d.texture;
                                            mp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
                                            mp.colorAttachments[0].storeAction = MTLStoreActionStoreAndMultisampleResolve;
                                            pp_rp(mp, "blit -> ekran (MSAA dortgen)");
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
                                        /* 1:1 ve ayni bicimdeyse GERCEK kopya kullan.
                                         * Tam ekran dortgenle drawable'dan ORNEKLEMEK
                                         * karo (32x32) sinirlarinda bayat icerik
                                         * veriyordu: gokyuzunde ve cam cephelerde
                                         * dikdortgen mozaik.  (Olculdu: blok kenarlari
                                         * tam 32/64 piksel katlarinda.)  copyFromTexture
                                         * hem dogru hem daha ucuz. */
                                        if (st2.width == dt2.width && st2.height == dt2.height &&
                                            st2.pixelFormat == dt2.pixelFormat &&
                                            !ENVC("BLITQUAD")){
                                            id<MTLBlitCommandEncoder> bc = pp_blit(cb, "blit (ayni boyut kopya)");
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
                                        /* Olcekli blit: drawable'dan DOGRUDAN
                                         * ornekleme karo sinirlarinda bayat
                                         * icerik veriyor (32/64 px mozaik).
                                         * Once gercek kopya ile ara dokuya al,
                                         * sonra ondan olcekleyerek ornekle. */
                                        if (st2 == d.texture){
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
                                                id<MTLBlitCommandEncoder> bc2 = pp_blit(cb, "blit (karalama kopyasi)");
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
                                        { char pn[48]; snprintf(pn, sizeof pn, "blit dortgen -> %lux%lu", (unsigned long)dt2.width, (unsigned long)dt2.height);
                                          pp_rp(bp, pn); }
                                        id<MTLRenderCommandEncoder> be =
                                            [cb renderCommandEncoderWithDescriptor:bp];
                                        [be setRenderPipelineState:g_pso];
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
                                /* Clear(ZBUFFER): o Z yuzeyi bir sonraki gecis
                                 * basinda temizlensin.  Ayni yuzeyle acik gecis
                                 * varsa kapat ki temizleme sirasi korunsun. */
                                if (dr->is_clear && (dr->clr_flags & 2u)){
                                    unsigned zc = DEPTH_ZID(dr->depth);
                                    if (zc && zc < MAXZ){
                                        g_zpend[zc] = 1;
                                        g_zpend_v[zc] = dr->clr_z < 0.f ? 0.f : dr->clr_z > 1.f ? 1.f : dr->clr_z;
#if NFS_ENHANCED
                                        /* cok ornekli ikiz kendi temizligini ayri tuketir */
                                        if (zc < 64u){ g_hd_zpend_ms[zc] = 1; g_hd_zpend_msv[zc] = g_zpend_v[zc]; }
#endif
#if NFS_ENHANCED
                                        if (enc && cur_z == zc){ int pe = hd_ms_end(enc, rb, i, c_ms, c_gb, c_att);
                                                  [enc endEncoding]; enc = nil; cur_t = 0xFFFFFFFFu;
                                                  if (pe) hd_ms_fallback(cb, d.texture, pe); }
#endif
                                        if (enc && cur_z == zc){ [enc endEncoding]; enc = nil; cur_t = 0xFFFFFFFFu; }
                                    }
                                }
#if NFS_ENHANCED
                                /* HD bulaniklik cizimi: yol hedefinden yol hedefine.
                                 * Kaynagin mip'leri guncel degilse gecisi kapat,
                                 * uret, ayni hedefle (Load) yeniden ac. */
                                int hd_blur = !dr->is_clear && hd_is_road(dr->tex) &&
                                              hd_is_road(TGT_ID(dr->target));
                                if (hd_blur && g_hd_dirty[dr->tex]){
                                    if (enc){ int pe = hd_ms_end(enc, rb, i, c_ms, c_gb, c_att);
                                              [enc endEncoding]; enc = nil; cur_t = 0xFFFFFFFFu;
                                              if (pe) hd_ms_fallback(cb, d.texture, pe); }
                                    id<MTLBlitCommandEncoder> mb = pp_blit(cb, "yol yansimasi mip uretimi");
                                    [mb generateMipmapsForTexture:g_tex[dr->tex]];
                                    [mb endEncoding];
                                    g_hd_dirty[dr->tex] = 0;
                                    atomic_fetch_add(&g_hd_mipgen, 1ull);
                                }
#endif
                                /* bkz. GECIS BIRLESTIRME: Z kullanmayan cizim Z yuzeyi yuzunden gecis bolmez */
                                int zsplit = DEPTH_ZID(dr->depth) != cur_z &&
                                             !(enc && pass_merge_on() && !dr->is_clear && !(dr->depth & 3u));
                                /* yalniz Clear iceren ekran gecisi: acma (Z temizligi yukarida kaydedildi) */
                                if (!enc || dr->target != cur_t || zsplit)
                                    if (!hd_post && !screen_touched && pass_clear_only(rb, i)) continue;
                                if (dr->target != cur_t || zsplit){
#if NFS_ENHANCED
                                    if (enc){ int pe = hd_ms_end(enc, rb, i, c_ms, c_gb, c_att);
                                              [enc endEncoding]; enc = nil;
                                              if (pe) hd_ms_fallback(cb, d.texture, pe); }
#endif
                                    if (enc){ [enc endEncoding]; enc = nil; }
#if NFS_ENHANCED
                                    if (hd_is_road(TGT_ID(dr->target))) g_hd_dirty[TGT_ID(dr->target)] = 1;
#endif
                                    id<MTLTexture> tgt = d.texture;
                                    if (dr->target) freed_use(TGT_ID(dr->target), 1);
                                    if (dr->target && TGT_ID(dr->target) < MAXTEX &&
                                        g_tex[TGT_ID(dr->target)])
                                        tgt = g_tex[TGT_ID(dr->target)];
                                    unsigned tk0 = TGT_ID(dr->target)*8u + TGT_FACE(dr->target);
                                    int first_use = dr->target
                                        ? !(touched[tk0>>3] & (1u<<(tk0&7)))
                                        : !screen_touched;
                                    MTLRenderPassDescriptor *rp =
                                        [MTLRenderPassDescriptor renderPassDescriptor];
                                    rp.colorAttachments[0].texture = tgt;
                                    if (TGT_FACE(dr->target))
                                        rp.colorAttachments[0].slice = TGT_FACE(dr->target) - 1u;
                                    rp.colorAttachments[0].loadAction =
                                        first_use ? MTLLoadActionClear : MTLLoadActionLoad;
                                    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                                    rp.colorAttachments[0].clearColor = MTLClearColorMake(
                                        atomic_load(&g_clear_r), atomic_load(&g_clear_g),
                                        atomic_load(&g_clear_b), 1.0);
                                    c_ms = 0;
#if NFS_ENHANCED
                                    int c_ml = 0;                   /* bkz. BELLEKSIZ MSAA EKLERI */
                                    g_hd_msp.on = 0; g_hd_msp.ml = 0; g_hd_msp.dz = 0;
                                    if (!dr->target && hd_msaa_n() > 1 && !hd_post){
                                        unsigned zq = DEPTH_ZID(dr->depth);
                                        /* hicbir ek yuklenmeyecek (renk ilk kullanim, G ilk kez,
                                         * derinlik temizlenecek) ve hicbir ornek saklanmayacak */
                                        c_ml = hd_ms_memless() && first_use &&
                                               (hd_gb_first || !(hd_scene_open && hd_ssr_on())) &&
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
                                                  c_ml ? ((c_att & 1u) ? MTLStoreActionMultisampleResolve   /* belleksiz: kesin */
                                                                       : MTLStoreActionDontCare)
                                                : hd_mslazy() ? MTLStoreActionUnknown             /* kapanirken: hd_ms_end */
                                                : MTLStoreActionStoreAndMultisampleResolve;
                                            atomic_fetch_add(&g_hd_mspass, 1ull);
                                        }
                                    }
                                    c_gb = 0;
                                    if (!dr->target && hd_scene_open && hd_ssr_on()){
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
                                            ga.clearColor = (gi == 2 && hd_gpack())     /* paketli: sifir yon = 512 */
                                                ? MTLClearColorMake(512.0 / 1023.0, 512.0 / 1023.0, 512.0 / 1023.0, 0)
                                                : MTLClearColorMake(0, 0, 0, 0);
                                        }
                                        hd_gb_first = 0;
                                        hd_scene_zid = DEPTH_ZID(dr->depth);
                                        c_gb = 1;
                                    }
#endif
                                    /* Z: bagli Z yuzeyinin kendi dokusu (bkz. g_ztex).
                                     * Yuzey yoksa ya da boyutu hedefe uymuyorsa
                                     * karalama dokusu baglanir ve Z testi kapatilir
                                     * (boru hatlari hep Depth32Float bekliyor). */
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
                                            /* ortuk yuzey (1) ekranla boyutlanir; yaratilan
                                             * yuzeyler ilk kullanildiklari hedefle.  Sonradan
                                             * farkli boyutlu hedefle kullanim: karalama. */
                                            if (!zt || zid == 1u){
                                                MTLTextureDescriptor *zd =
                                                    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
                                                        MTLPixelFormatDepth32Float
                                                        width:tgt.width height:tgt.height mipmapped:NO];
                                                zd.usage = MTLTextureUsageRenderTarget
#if NFS_ENHANCED
                                                         | MTLTextureUsageShaderRead   /* SSR derinligi okur */
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
                                    id<MTLTexture> zres = nil;     /* derinlik cozumu hedefi */
                                    if (c_ms){
                                        if (ztex && zid < 64u){
                                            zres = ztex;
                                            /* temizlemeden yuklemek istiyor ama son gecis saklamadi */
                                            int lost = g_hd_msz_seen[zid] && !g_hd_msz_kept[zid] && !g_hd_zpend_ms[zid];
                                            if (c_ml){
                                                g_hd_msz_ml = hd_ms_tex_ml(g_hd_msz_ml, MTLPixelFormatDepth32Float,
                                                                           tgt.width, tgt.height);
                                                ztex = g_hd_msz_ml;
                                                zclear = 1;                     /* belleksiz: hep temizlenir */
                                            } else {
                                                id<MTLTexture> old = g_hd_msz[zid];
                                                g_hd_msz[zid] = hd_ms_tex(old, MTLPixelFormatDepth32Float,
                                                                          tgt.width, tgt.height);
                                                zclear = (g_hd_msz[zid] != old);       /* yeni: tanimsiz */
                                                ztex = g_hd_msz[zid];
                                            }
                                            zval = 1.f;
                                            if (g_hd_zpend_ms[zid]){ zclear = 1; zval = g_hd_zpend_msv[zid]; g_hd_zpend_ms[zid] = 0; }
                                            if (lost){ zclear = 1; atomic_fetch_add(&g_hd_mszlost, 1ull); }
                                            g_hd_msz_seen[zid] = 1;
                                            g_hd_zms_used[zid] = 1;
                                            g_hd_msp.dz = 1; g_hd_msp.zid = zid; g_hd_msp.zclr = zclear;
                                        } else {
                                            /* derinliksiz ekran gecisi: hep Clear + DontCare */
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
                                        /* MSAA derinligini tek ornekli ikize COZ: ekrandan sonra
                                         * ayni Z ile tek ornekli hedefe cizen gecisler (isik
                                         * parlamalari vb.) sahnenin derinligini gorsun.  Ikiz
                                         * artik MS durumunu yansittigi icin bekleyen temizligi
                                         * gecersiz. */
                                        if (zres && zres.width == ztex.width && zres.height == ztex.height &&
                                            !(ENVC("HD_ZRESOLVE") && ENVC("HD_ZRESOLVE")[0] == '0')){
                                            rp.depthAttachment.resolveTexture = zres;
                                            rp.depthAttachment.storeAction = MTLStoreActionStoreAndMultisampleResolve;
                                            rp.depthAttachment.depthResolveFilter = MTLMultisampleDepthResolveFilterSample0;
                                            g_zpend[zid] = 0;
                                            g_hd_zsdirty[zid] = 0;
                                            atomic_fetch_add(&g_hd_zres, 1ull);
                                        }
                                        /* MS derinligin saklamasi: belleksizde acilista kesin (hic
                                         * saklanmaz), Private'ta kapanista secilir (hd_ms_end) */
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
                                        if (!dr->target) snprintf(pn, sizeof pn, "EKRAN%s%s", c_ms ? " +MSAA" : "",
#if NFS_ENHANCED
                                                                  c_gb ? " +G-tampon" : "");
#else
                                                                  "");
#endif
                                        else snprintf(pn, sizeof pn, "%s #%u %lux%lu", TGT_FACE(dr->target) ? "kup yuzu" : "hedef",
                                                      TGT_ID(dr->target), (unsigned long)tgt.width, (unsigned long)tgt.height);
                                        pp_rp(rp, pn);
                                        { static long ff = -1;        /* ilk profilli karede gecis icerigi */
                                          if (ff < 0) ff = fno_pre;
                                          if (ff == fno_pre){
                                              unsigned nd = 0, nz = 0, nzt = 0, na = 0, zz = DEPTH_ZID(dr->depth);
                                              for (unsigned q = i; q < g_dn[rb]; q++){ MDraw *x = &g_dbuf[rb][q];
                                                  if (x->is_blit || x->target != dr->target || DEPTH_ZID(x->depth) != zz) break;
                                                  if (x->is_clear){ if (q > i && (x->clr_flags & 2u)) break; continue; }
                                                  nd++; if (x->depth & 2u) nz++; if (x->depth & 1u) nzt++; if (x->alpha_ref > 0.f) na++; }
                                              fprintf(stderr, "  [icerik] cizim#%u %s: zid %u, %u cizim, Z yazan %u, Z test eden %u, alfa testli %u, ilk ps %d doku %u\n",
                                                      i, pn, zz, nd, nz, nzt, na, dr->ps, dr->tex); } } }
                                    enc = [cb renderCommandEncoderWithDescriptor:rp];
                                    atomic_fetch_add(&g_pass_n, 1ull);
                                    [enc setVertexBuffer:g_mtlvb offset:0 atIndex:0];
                                    [enc setFragmentSamplerState:g_samp atIndex:0];
                                    /* yeni kodlayici = tum durum sifirlandi */
                                    c_pso = nil; c_bcol = ~0u; c_depth = ~0u; c_fp_valid = 0;
                                    c_cull = ~0u;
                                    /* D3D'de on yuz SAAT YONUNDEDIR.  CULLWIND=1
                                     * ile ters cevrilebilir (kose yolumuz Y'yi
                                     * cevirirse gerekir). */
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
                                /* PSINFO=<n>: bu ps'i kullanan ilk cizimlerin
                                 * hedefini/dokusunu/karisimini bas. */
                                { static int pi = -2; static int shown = 0;
                                  if (pi == -2){ const char *e=ENVC("PSINFO"); pi = e?atoi(e):-1; }
                                  if (pi >= 0 && dr->ps == pi && shown < 6){ shown++;
                                    fprintf(stderr,"  [psinfo] ps=%d hedef=%u doku=%u(%lux%lu) doku1=%u"
                                            " karisim=%05x kose=%u  c0=(%.3f %.3f %.3f %.3f)\n",
                                            dr->ps, dr->target, dr->tex,
                                            (dr->tex && dr->tex<MAXTEX && g_tex[dr->tex])?(unsigned long)g_tex[dr->tex].width:0ul,
                                            (dr->tex && dr->tex<MAXTEX && g_tex[dr->tex])?(unsigned long)g_tex[dr->tex].height:0ul,
                                            dr->tex1, dr->blend, dr->count,
                                            (double)dr->c0[0],(double)dr->c0[1],
                                            (double)dr->c0[2],(double)dr->c0[3]); } }
#if NFS_ENHANCED
                                if (dr->ps >= PSREFL && c_gb){ hd_refl_drawn = 1; atomic_fetch_add(&g_hd_refl_draws, 1ull); }
                                /* Ayni Z yuzeyi bu karede ekranda (cok ornekli ikiziyle)
                                 * kullanildi ve simdi tek ornekli bir hedefte Z TESTI
                                 * yapiliyor: ikizler icerik paylasmadigi icin bu cizim
                                 * sahnenin derinligini goremez.  Olcum icin say. */
                                /* Ters yon: tek ornekli gecis Z YAZDI, sonra ekran (MS ikiz)
                                 * o derinlikle test ediyor -- ikiz bunu goremez. */
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
                                            fprintf(stderr, "  [zpay] hedef=%u (%lux%lu) z=%u ps=%d vs=%d karisim=%05x kose=%u\n",
                                                    dr->target, (unsigned long)tt.width, (unsigned long)tt.height,
                                                    cur_z, dr->ps, dr->vsid, dr->blend, dr->count); }
                                        if (f >= 0) zn[f]++;
                                    }
                                }
#endif
                                { uint64_t k2 = (uint64_t)(dr->blend & 0xFFFFFu)
                                               | ((uint64_t)(dr->ps & 0xFFF) << 20)
                                               | ((uint64_t)(dr->vsid & 0xFFF) << 32)
                                               | ((uint64_t)(c_ms & 1) << 48)
                                               | ((uint64_t)(c_gb & 1) << 49);
#if NFS_ENHANCED
                                  if (c_ms) k2 |= (uint64_t)hd_msaa_l2() << 50;   /* bkz. COZUNURLUGE GORE MSAA */
#endif
                                  id<MTLRenderPipelineState> ps2 = pso_for(k2);
                                  if (ps2 != c_pso){ c_pso = ps2;
                                      [enc setRenderPipelineState:ps2]; } }
                                /* BLENDFACTOR: boru hattinin degil kodlayicinin
                                 * durumu; her hedef degisiminde sifirlanir. */
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
                                    /* mip filtresi DOGRUSAL (bit 4-5 = 2): level() okumasi
                                     * mip'siz ornekleyicide yok sayilir. */
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
                                      /* olcek = Metal dokusu / oyunun sandigi boyut; ornek
                                       * araligi L seviyesinde ~1 teksel kalsin. */
                                      id<MTLTexture> st = g_tex[dr->tex];
                                      float sx = g_hd_ow[dr->tex] ? (float)st.width  / g_hd_ow[dr->tex] : 1.f;
                                      float sy = g_hd_oh[dr->tex] ? (float)st.height / g_hd_oh[dr->tex] : 1.f;
                                      float sc = sx > sy ? sx : sy;
                                      if (sc > 1.f){
                                          float L = enh_blur() * log2f(sc);
                                          fp.p0 = exp2f(L) / sc;     /* ofset olcegi (0..1] */
                                          fp.p1 = L;                 /* mip seviyesi */
                                      }
                                      atomic_fetch_add(&g_hd_blurdraw, 1ull);
                                  }
#endif
                                  memcpy(fp.proj, dr->proj, sizeof fp.proj);
                                  memcpy(fp.fogv, dr->fogv, sizeof fp.fogv);
                                  /* ayni havuz sirasi + ayni kucuk alanlar: 548 baytlik
                                   * blogu kurup karsilastirmaya gerek yok */
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
                                if (dr->vsid > 0 && dr->ix_isz){
                                    id<MTLBuffer> pl = g_pool_fr[rb];
                                    [enc setVertexBuffer:pl offset:dr->rawoff atIndex:1];
                                    [enc setVertexBuffer:g_mtlkb  offset:dr->koff   atIndex:2];
                                    [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:dr->count
                                                     indexType:(dr->ix_isz == 4u) ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16
                                                   indexBuffer:pl indexBufferOffset:dr->ix_ib
                                                 instanceCount:1 baseVertex:dr->ix_base baseInstance:0];
                                } else if (dr->vsid > 0){
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
                                  if (pe) hd_ms_fallback(cb, d.texture, pe); }
#endif
                        if (enc) [enc endEncoding];
                        /* ekrana hic cizilmediyse en azindan temizle */
                        if (!screen_touched){
                            MTLRenderPassDescriptor *rp =
                                [MTLRenderPassDescriptor renderPassDescriptor];
                            rp.colorAttachments[0].texture = d.texture;
                            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
                            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                            rp.colorAttachments[0].clearColor = MTLClearColorMake(
                                atomic_load(&g_clear_r), atomic_load(&g_clear_g),
                                atomic_load(&g_clear_b), 1.0);
                            id<MTLRenderCommandEncoder> e2 =
                                [cb renderCommandEncoderWithDescriptor:rp];
                            [e2 endEncoding];
                        }
                        pthread_mutex_lock(&g_qlock);
                        g_ready = -1; g_enc_rb = -1;
                        pthread_cond_signal(&g_qcond);
                        pthread_mutex_unlock(&g_qlock);
                        /* TESTFILL: yakalama yolunu dogrulamak icin bilinen
                         * renkle doldur.  PNG mor cikarsa yakalama saglam. */
                        if (ENVC("TESTFILL")){
                            MTLRenderPassDescriptor *rp =
                                [MTLRenderPassDescriptor renderPassDescriptor];
                            rp.colorAttachments[0].texture = d.texture;
                            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
                            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                            rp.colorAttachments[0].clearColor =
                                MTLClearColorMake(1.0, 0.0, 1.0, 1.0);
                            id<MTLRenderCommandEncoder> e3 =
                                [cb renderCommandEncoderWithDescriptor:rp];
                            [e3 endEncoding];
                        }
                        if (g_perflog && atomic_exchange(&g_pl_thumb_req, 0)) pl_thumb(cb, d.texture, fno_pre);
                        /* Sunumdan ONCE okunabilir bir kopya al: sunulmus bir
                         * drawable'in dokusundan geri okuma guvenilir degil. */
                        if (dumpnow){
                            /* boyut degistiyse (tam ekran gecisi) yeniden yarat */
                            if (g_capture && (g_capture.width != d.texture.width ||
                                              g_capture.height != d.texture.height)) g_capture = nil;
                            if (!g_capture){
                                MTLTextureDescriptor *td =
                                    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
                                        MTLPixelFormatBGRA8Unorm
                                        width:d.texture.width height:d.texture.height
                                        mipmapped:NO];
                                td.usage = MTLTextureUsageShaderRead;
                                td.storageMode = MTLStorageModeShared;
                                g_capture = [g_dev newTextureWithDescriptor:td];
                            }
                            if (g_capture){
                                id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
                                [bl copyFromTexture:d.texture sourceSlice:0 sourceLevel:0
                                       sourceOrigin:MTLOriginMake(0,0,0)
                                         sourceSize:MTLSizeMake(d.texture.width,
                                                                d.texture.height,1)
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
                        pp_end(cb);
                        [cb addCompletedHandler:^(id<MTLCommandBuffer> b2){
                            /* hatayla biten kare: GPU sureleri 0 gelir, olcumleri de bozar */
                            if (b2.status == MTLCommandBufferStatusError){
                                unsigned long ne = atomic_fetch_add(&g_cb_err, 1ul);
                                if (ne < 5) fprintf(stderr, "metal: KOMUT TAMPONU HATASI: %s\n",
                                                    [[b2.error localizedDescription] UTF8String]); }
                            fb_release(fbi);        /* GPU bitti: takim yeniden kullanilabilir */
                            double g = (b2.GPUEndTime - b2.GPUStartTime) * 1e9;
                            if (g > 0.0 && g < 1e9){
                                unsigned long long ns = (unsigned long long)g;
                                if (in_win){ atomic_fetch_add(&g_gpuw_ns, ns); atomic_fetch_add(&g_gpuw_n, 1ull); }
                                if (ab_mode >= 0 && ab_count){ atomic_fetch_add(&g_ab_ns[ab_mode], ns); atomic_fetch_add(&g_ab_cnt[ab_mode], 1ull); }
                                atomic_fetch_add(&g_gpu_ns, ns);
                                atomic_fetch_add(&g_gpu_n, 1ull);
                                if (g_perflog) perflog_gpu(fno_pre, g);
                                unsigned long long mx = atomic_load(&g_gpu_max_ns);
                                while (ns > mx &&
                                       !atomic_compare_exchange_weak(&g_gpu_max_ns, &mx, ns)) {}
                            } }];
                        [cb presentDrawable:d];
                        [cb commit];
                        long fno = atomic_fetch_add(&g_frames_drawn, 1) + 1;
                        if (g_perflog){ struct timespec _p1; clock_gettime(CLOCK_MONOTONIC, &_p1);
                            /* karede baglanan farkli doku (+ hedef) sayisi: komut tamponu
                             * gonderiminin cekirdek maliyeti kaynak sayisiyla buyur */
                            static uint32_t pl_st[MAXTEX]; static uint32_t pl_gen = 0;
                            unsigned pl_nt = 0; pl_gen++;
                            for (unsigned q = 0; rb >= 0 && q < g_dn[rb]; q++){
                                const MDraw *x = &g_dbuf[rb][q];
                                unsigned ids[5] = { x->tex, x->tex1, x->tex2, x->tex3, TGT_ID(x->target) };
                                for (int w = 0; w < 5; w++){ unsigned id_ = ids[w];
                                    if (id_ && id_ < MAXTEX && pl_st[id_] != pl_gen){ pl_st[id_] = pl_gen; pl_nt++; } }
                            }
                            perflog_render_frame(fno, pl_nt,
                                (uint64_t)(_d1.tv_sec - _d0.tv_sec) * 1000000000ull + (uint64_t)_d1.tv_nsec - (uint64_t)_d0.tv_nsec,
                                (uint64_t)(_p1.tv_sec - _e0.tv_sec) * 1000000000ull + (uint64_t)_p1.tv_nsec - (uint64_t)_e0.tv_nsec,
                                rb >= 0 ? g_dn[rb] : 0u, rb >= 0 ? g_vn[rb] : 0u); }
                        /* FPSCSV=<yol>: kare basina sure + is yuku.  Kasmanin
                         * NEREDE oldugunu tahmin etmeden gormek icin. */
                        { static FILE *fp = NULL; static int init = 0;
                          static uint64_t prev = 0;
                          if (!init){ init = 1; const char *e = ENVC("FPSCSV");
                              if (e && *e){ fp = fopen(e, "w");
                                  if (fp) fprintf(fp, "kare,ms,cizim,kose,ucgen\n"); } }
                          if (fp){ struct timespec _ts;
                              clock_gettime(CLOCK_MONOTONIC, &_ts);
                              uint64_t now = (uint64_t)_ts.tv_sec*1000000ull
                                           + (uint64_t)_ts.tv_nsec/1000ull;
                              if (prev) fprintf(fp, "%ld,%.2f,%u,%u,%u\n", fno,
                                    (double)(now - prev)/1000.0,
                                    rb>=0?g_dn[rb]:0u, rb>=0?g_vn[rb]:0u,
                                    rb>=0?(g_vn[rb]/3u):0u);
                              prev = now;
                              if ((fno % 240) == 0) fflush(fp); } }
                        if (dumpnow){
                            [cb waitUntilCompleted];
                            if (g_capture) dump_texture(g_capture, fno_pre);
                            { const char *dt = ENVC("DUMPTEX");   /* virgullu liste */
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
                /* Kare hazir olana kadar UYU (yoklama degil).  4 ms'lik
                 * ust sinir yalnizca NSEvent kuyrugunu bosaltmak icin:
                 * kare gelince cond_signal aninda uyandiriyor. */
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
        fprintf(stderr, "metal: bos kare atlandi = %ld\n", g_skip_empty);
        fprintf(stderr, "metal: tampon halkasi: %lu karede onceki kare hala GPU'daydi "
                "(tek tamponla ezilirdi), %lu kez bos takim beklendi (toplam %.1f ms)\n",
                atomic_load(&g_fb_overlap), atomic_load(&g_fb_wait),
                (double)atomic_load(&g_fb_wait_ns) / 1e6);
        { NSUInteger v = 0, r = 0, k = 0;
          for (int i = 0; i < NFB; i++){ v += g_fb_vb[i].length; r += g_fb_rvb[i].length; k += g_fb_kb[i].length; }
          fprintf(stderr, "metal: tampon halkasi boyu (%d takim toplami): kose %.1f MB + ham kose %.1f MB + sabit %.1f MB, %lu kez buyudu\n",
                  NFB, v / 1048576.0, r / 1048576.0, k / 1048576.0, atomic_load(&g_fb_grow)); }
        fprintf(stderr, "metal: kose/indis havuzu: %.1f MB (dolu %.1f MB), %lu yansi canli, %lu kuruldu (%.1f MB kopya), %lu dustu, tablo dolu %lu\n",
                g_pool ? g_pool.length / 1048576.0 : 0.0, g_pool_top / 1048576.0, g_mir_live, g_mir_build,
                g_mir_bytes / 1048576.0, g_mir_drop, g_mir_full);
        fprintf(stderr, "metal: komut tamponu hatasi: %lu kare\n", atomic_load(&g_cb_err));
        { unsigned long n = atomic_load(&g_enc_late);
          fprintf(stderr, "metal: kodlama 60 ms'yi asti (eskiden kodlanan tampona takas olurdu): %lu kez", n);
          for (unsigned long q = 0; q < n && q < 8; q++) fprintf(stderr, "%s%ld", q ? "," : "  kareler: ", g_enc_late_f[q]);
          fprintf(stderr, "\n"); }
        fprintf(stderr, "metal: Reset sonrasi birakilan render hedefi: %lu (sonradan kullanilan: doku %lu, hedef %lu)\n",
                g_rt_freed, g_freed_use_smp, g_freed_use_tgt);
        fprintf(stderr, "metal: Z yuzeyi: %llu gecis Z'siz, %llu gecis boyut uyumsuz (karalama)\n",
                (unsigned long long)g_z_null, (unsigned long long)g_z_mismatch);
        fprintf(stderr, "metal: pencere kapandi (%ld kare cizildi)\n",
                atomic_load(&g_frames_drawn));
    }
}

/* Kare butcesi raporu: GPU mesgul suresi, CPU kodlama suresi, gecis/cizim
 * sayisi.  "Neye donanim harciyoruz" sorusu ancak bu ucu birlikte gorulunce
 * cevaplanabilir. */
void metal_budget_report(void){
#if NFS_ENHANCED
    fprintf(stderr, "[HD] yol bulanikligi: %llu cizim, %llu mip uretimi\n",
            atomic_load(&g_hd_blurdraw), atomic_load(&g_hd_mipgen));
    { unsigned long long f = atomic_load(&g_rt_frames);
      fprintf(stderr, "[HD] isin izleme: %llu kare, kare basina ort %.0f ucgen / %.0f anlik cizim; cekirdeksiz atlanan %llu, tasma %llu\n",
              f, f ? (double)atomic_load(&g_rt_tris) / f : 0.0, f ? (double)atomic_load(&g_rt_draws) / f : 0.0,
              atomic_load(&g_rt_skipk), atomic_load(&g_rt_over));
      fprintf(stderr, "[HD] RT onbellek: kare basina ort anlik %.0f ucgen + sabit %.0f ucgen; terfi %llu, atma %llu, sifirlama %llu, sabit kurulum %llu\n",
              f ? (double)atomic_load(&g_rt_dynv) / 3 / f : 0.0, f ? (double)atomic_load(&g_rt_statv) / 3 / f : 0.0,
              atomic_load(&g_rt_promo), atomic_load(&g_rt_evict), atomic_load(&g_rt_reset), atomic_load(&g_rt_sbuilds));
      fprintf(stderr, "[HD] RT anlik yapi: %llu tam kurulum, %llu yerinde guncelleme (refit %s)\n",
              atomic_load(&g_rt_nbuild), atomic_load(&g_rt_nrefit), hd_rt_refit_on() ? "ACIK" : "KAPALI");
      fprintf(stderr, "[HD] RT onbellek: sikistirma %llu, yeniden ozetleme %llu, depo %u kose (%u olu)\n",
              atomic_load(&g_rt_compact), atomic_load(&g_rt_rehash), g_rt_scur, g_rt_sdead);
      if (g_rt_fixcnt){ const uint32_t *fc = (const uint32_t *)[g_rt_fixcnt contents];
          fprintf(stderr, "[HD] RT kose denetimi: %s; etkisizlestirilen ucgen: NaN/sonsuz %u, cok uzak %u (kare basina ort %.1f)\n",
                  g_rt_fixk ? "ACIK" : "KAPALI", fc[0], fc[1], f ? (double)(fc[0] + fc[1]) / (double)f : 0.0); } }
    fprintf(stderr, "[HD] SSR: %llu birlestirme, %llu yansima cizimi\n",
            atomic_load(&g_hd_ssr_n), atomic_load(&g_hd_refl_draws));
    fprintf(stderr, "[HD] MSAA tembel saklama: %s, tahmin tutmayan (ek cozme gecisi) %llu; ertelenen ara gecis %llu\n",
            hd_mslazy() ? "ACIK" : "KAPALI", atomic_load(&g_hd_msfb), atomic_load(&g_hd_reord));
    fprintf(stderr, "[HD] MSAA %dx: %llu ekran gecisi, %llu ekrana blit, %llu Z cozumu,"
            " Z paylasim: %llu ekran->hedef cizimi, %llu hedef->ekran cizimi (gorunmez)\n",
            g_hd_msn, atomic_load(&g_hd_mspass), atomic_load(&g_hd_msblit),
            atomic_load(&g_hd_zres), atomic_load(&g_hd_zshare), atomic_load(&g_hd_zrev));
    fprintf(stderr, "[HD] MSAA ekleri: %llu gecis BELLEKSIZ, ongoru tutmayan %llu%s | MS derinlik: %llu saklandi, "
            "%llu saklanmadi, %llu kez temizlemeden yukleme istendi ama ornekler kayipti (0 olmali)\n",
            atomic_load(&g_hd_ml_pass), atomic_load(&g_hd_ml_miss), g_hd_ml_off ? " (belleksiz kip KAPANDI)" : "",
            atomic_load(&g_hd_mszst), atomic_load(&g_hd_mszskip), atomic_load(&g_hd_mszlost));
#endif
    unsigned long long n  = atomic_load(&g_gpu_n);
    unsigned long long gs = atomic_load(&g_gpu_ns);
    unsigned long long mx = atomic_load(&g_gpu_max_ns);
    unsigned long long es = atomic_load(&g_enc_ns);
    long fr = atomic_load(&g_frames_drawn);
    if (!fr) return;
    fprintf(stderr, "  --- kare butcesi ---\n");
    if (n) fprintf(stderr, "  GPU mesgul        : ort %.2f ms/kare, en fazla %.2f ms (%llu kare olculdu)\n",
                   (double)gs/(double)n/1e6, (double)mx/1e6, n);
    fprintf(stderr, "  CPU kodlama       : ort %.2f ms/kare (cizim listesini Metal'e yazmak)\n",
            (double)es/(double)fr/1e6);
    pp_report();
    if (g_ab_on > 0){
        fprintf(stderr, "  --- adim adim olcum (HD_RT_AB) ---\n");
        for (int m = 0; m < AB_N; m++){ unsigned long long c = atomic_load(&g_ab_cnt[m]);
            if (c) fprintf(stderr, "  [AB] %-48s GPU %7.2f ms/kare (%llu kare)\n", g_ab_name[m], (double)atomic_load(&g_ab_ns[m]) / (double)c / 1e6, c); } }
    { unsigned long long wn = atomic_load(&g_gpuw_n), en = atomic_load(&g_encw_n);
      if (wn && en) fprintf(stderr, "  PENCERE %ld-%ld   : GPU %.3f ms/kare, CPU kodlama %.3f ms/kare (%llu kare)\n",
              g_gpuw_a, g_gpuw_b, (double)atomic_load(&g_gpuw_ns)/(double)wn/1e6,
              (double)atomic_load(&g_encw_ns)/(double)en/1e6, wn); }
    fprintf(stderr, "  konuk BEKLEME     : %.2f ms/kare (onceki kare ekrana konsun diye)\n"
                    "  ana thread bekleme: %.2f ms/kare kare-hazir, %.2f ms/kare nextDrawable (vsync)\n",
            (double)atomic_load(&g_wait_present_ns)/(double)fr/1e6,
            (double)atomic_load(&g_wait_frame_ns)/(double)fr/1e6,
            (double)atomic_load(&g_wait_drawable_ns)/(double)fr/1e6);
    fprintf(stderr, "  render gecisi     : %.1f/kare,  cizim cagrisi: %.0f/kare\n",
            (double)atomic_load(&g_pass_n)/(double)fr,
            (double)atomic_load(&g_draw_n)/(double)fr);
}
