/* COM koprusu: guest'e native implementasyonlu COM nesneleri sunar.
 * Guest nesne: [obj+0]=vtable.  vtable girisi: 0xE0000000|(iface<<16)|metot
 * Tum metotlar stdcall, ilk arguman 'this'.                            */
#include <time.h>
#include <stdatomic.h>
#include "cpu.h"
#include "d3d9_tables.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* getenv() her cagrida ortam dizisini TARAR ve kilit alir.  Cizim basina
 * calisan yollarda (ornegin SetVertexShaderConstantF icindeki "PDLIST"
 * kontrolu &&-zincirinin ILK terimiydi) olculebilir maliyeti var:
 * profilde getenv+__findenv_locked konuk thread'in %3'unu yiyordu.
 * Ortam degiskenleri kosu boyunca degismez -> bir kez coz, sakla. */
#define ENVC(nm_) ({ static const char *_ev_; static int _ei_; \
                     if (!_ei_){ _ei_ = 1; _ev_ = getenv(nm_); } _ev_; })


void mem_commit(uint32_t,uint32_t);
void guest_backtrace(CPU*,int);
void dispatch_indirect(CPU*,uint32_t);
static int g_verbose = -1;
static int VB(void){ if(g_verbose<0) g_verbose = ENVC("VERBOSE")?1:0; return g_verbose; }
/* Metal koprusu (metal.m) - yoksa etkisiz */
int  metal_available(void);
void metal_set_clear_color(unsigned argb);
void metal_present(void);
long metal_frames(void);
long metal_tris(void);
long metal_frames_dropped(void);
long metal_tex_created(void);
long metal_tex_uploaded(void);
void metal_begin_frame(void);
#include "gfx.h"
#include "vsemu.h"
#include "enhance.h"

/* COM dagitimi metot ADINI karsilastiriyor: kosuda 144 MILYON com_call var
 * ve her biri if-else zincirinde onlarca strcmp yapiyordu (profilde
 * strcmp+_platform_strcmp konuk thread'in %3.6'si, com_call kendisi %9.3).
 * Isim zaten (arayuz, yuva) ikilisiyle belli; ama zinciri butunuyle
 * yeniden yazmak yerine karsilastirmayi UCUZLAT: eslesmelerin neredeyse
 * tamami ilk karakterde elenir, o yuzden cagriyi hic yapma. */
#define NMEQ(nmv_, lit_) ((nmv_)[0] == (lit_)[0] && !strcmp((nmv_), (lit_)))
unsigned metal_create_texture(unsigned w, unsigned h, unsigned levels, unsigned d3dfmt);
unsigned metal_create_texture_ex(unsigned w, unsigned h, unsigned levels,
                                 unsigned d3dfmt, unsigned is_rt);
void     metal_set_render_target(unsigned texid);
void     metal_clear_target(unsigned argb);
void     metal_set_blend(unsigned key, float alpha_ref);
void     metal_upload_texture(unsigned id_, unsigned level, const void *data,
                              unsigned pitch, unsigned w, unsigned h, unsigned d3dfmt);
unsigned metal_level_pitch(unsigned w, unsigned level, unsigned d3dfmt);
unsigned metal_level_rows(unsigned h, unsigned level, unsigned d3dfmt);
void     metal_fill_keyboard(unsigned char *dst, unsigned n);
void     metal_fill_mouse(unsigned char *dst, unsigned n);
void     metal_get_cursor(int *x, int *y);
#define NEL(a) ((unsigned)(sizeof(a)/sizeof(*(a))))

#define COM_MAGIC 0xE0000000u
#define COM_BASE  0x30000000u
#define COM_MAX   0x38000000u
static uint32_t com_ptr = COM_BASE;
static uint32_t com_alloc(uint32_t n){
    n=(n+15)&~15u;
    if (com_ptr+n > COM_MAX) return 0;
    uint32_t p=com_ptr; com_ptr+=n; mem_commit(p,n); memset(g_mem+p,0,n); return p;
}
/* guest tarafi genel ayirici (Lock vb. icin) */
#define GBUF_BASE 0x38000000u
#define GBUF_MAX  0x60000000u
static uint32_t gbuf_ptr = GBUF_BASE;
static uint32_t gbuf_alloc(uint32_t n){
    n=(n+63)&~63u;
    if (gbuf_ptr+n > GBUF_MAX) return 0;
    uint32_t p=gbuf_ptr; gbuf_ptr+=n;
    /* Tampon basina TAM boyut + emniyet payi commit edilir.  Oyunun
     * bazi yollari (yuksek doku/LOD ayarlari) hesapladigimizdan biraz
     * fazla yaziyor; sonrasi commit edilmemis oldugu icin cokuyordu
     * (olculdu: 0x38098000).  Pay ile bu sinif cokme kalkiyor; mmap
     * tembel oldugu icin fiziksel bellek harcanmaz. */
    mem_commit(p, n + (1u<<20));
    return p;
}

typedef struct { const char *name; const Method *m; unsigned n; } Iface;
static const Iface g_if[] = {
    {"?",              0,         0},
    {"IDirect3D9",       M_D3D9,    NEL(M_D3D9)},
    {"IDirect3DDevice9", M_DEVICE,  NEL(M_DEVICE)},
    {"IDirect3DTexture9",M_TEXTURE, NEL(M_TEXTURE)},
    {"IDirect3DVertexBuffer9", M_VBUF, NEL(M_VBUF)},
    {"IDirect3DIndexBuffer9",  M_IBUF, NEL(M_IBUF)},
    {"IDirect3DSurface9",      M_SURFACE, NEL(M_SURFACE)},
    {"IDirect3DSimple",        M_SIMPLE,  NEL(M_SIMPLE)},
    {"IDirect3DVertexShader9",   M_VSHADER,    NEL(M_VSHADER)},
    {"IDirect3DVertexDeclaration9", M_VDECL,   NEL(M_VDECL)},
    {"IDirect3DStateBlock9",     M_STATEBLOCK, NEL(M_STATEBLOCK)},
    {"IDirect3DSwapChain9",      M_SWAPCHAIN,  NEL(M_SWAPCHAIN)},
    {"IDirect3DQuery9",          M_QUERY,      NEL(M_QUERY)},
    {"IDirectInput8",            M_DI8,        NEL(M_DI8)},
    {"IDirectInputDevice8",      M_DIDEV,      NEL(M_DIDEV)},
    {"IDirectSound",             M_DS,         NEL(M_DS)},
    {"IDirectSoundBuffer",       M_DSBUF,      NEL(M_DSBUF)},
};
enum { IF_D3D9=1, IF_DEV, IF_TEX, IF_VB, IF_IB, IF_SURF, IF_SIMPLE,
       IF_VSHADER, IF_VDECL, IF_STATEBLOCK, IF_SWAPCHAIN, IF_QUERY,
       IF_DI8, IF_DIDEV, IF_DS, IF_DSBUF };

static uint32_t com_new(uint32_t iface, uint32_t extra){
    unsigned n = g_if[iface].n;
    uint32_t vt = com_alloc(n*4u);
    for (unsigned i=0;i<n;i++) MR32(vt+4u*i) = COM_MAGIC | (iface<<16) | i;
    uint32_t obj = com_alloc(4u+extra);
    /* Fazladan alani SIFIRLA.  Nesne alanlari (boyut, format, doku
     * kimligi, kup yuz indeksi...) hep "0 = kurulmadi" varsayimiyla
     * okunuyor; com_alloc taze bellek vermezse cop deger okunur. */
    for (uint32_t q = 0; q < 4u + extra; q += 4u) MR32(obj + q) = 0u;
    MR32(obj) = vt;
    return obj;
}

#define PSCUBE 256   /* metal.m ile AYNI olmali: kup ps varyanti */
uint64_t g_com_calls=0, g_com_unimpl=0;
/* metot bazinda cagri sayaci: render katmani icin is listesi */
#define MSTAT_IF 20
#define MSTAT_M  128
static uint64_t g_mstat[MSTAT_IF][MSTAT_M];
static uint64_t g_present=0, g_draw=0;
static uint32_t g_d3d9=0, g_dev=0, g_cur_decl=0, g_cur_fvf=0, g_cur_tex=0;

/* ---------------- kose shader'i (CPU'da yorumlanir) ----------------
 * NFSU2 tum cizimlerini DrawPrimitiveUP ile yapiyor ve konumlar HAM
 * (POSITION, POSITIONT degil): ekrana dogru yerlestirmek icin kose
 * shader'ini calistirmak sart. Bkz. runtime/vsemu.c                   */
#define MAXVS 1024
static VSProg  *g_vsprog[MAXVS];
static unsigned g_vsprogn = 1;          /* 0 = "shader yok" */
void mprot_arm(uint32_t);
/* CoreAudio cikisi (runtime/audio.m) */
int      audio_register(uint32_t data, uint32_t bytes, uint32_t rate,
                        uint32_t chans, uint32_t bits);
void     audio_play(int id, int loop);
void     audio_stop(int id);
uint32_t audio_cursor(int id);
void     audio_set_volume(int id, long millibel);
void     audio_init(void);
void     audio_report(void);
void cov_reset(void);
static VSConst  g_vsk;                  /* c#/i#/b# sabit dosyasi */
static unsigned g_cur_vs = 0;
static uint64_t g_vs_draws = 0, g_ff_draws = 0;
static uint64_t g_v_in = 0, g_v_out = 0, g_v_badw = 0;
static uint64_t g_tex_fail = 0;
static uint64_t g_dikey_calls=0, g_dikey_hits=0;
static uint64_t g_mat_ok=0, g_mat_bad=0;
static uint64_t g_di_enum=0, g_di_objs=0;
/* --- DirectInput eylem haritasi ---------------------------------------
 * Oyun menuyu "action map" ile suruyor: her eyleme birden fazla tus
 * baglaniyor (ornek: ok-yukari ve W -> ayni eylem).  Semantik bicimi:
 *   dwSemantic = 0x81000400 | DIK_taramakodu
 * Oyun eylemleri GetDeviceData ile TAMPONLANMIS olarak okuyor; biz olay
 * uretmezsek menude hicbir sey olmuyor.                                */
#define DI_KB_SEM_BASE 0x81000400u
/* --- vertex/index buffer cizim yolu (3B modeller) ------------------
 * Oyun 3B geometriyi DrawIndexedPrimitive ile ciziyor; arayuz ise
 * DrawPrimitiveUP ile.  Bu yol uygulanmadigi icin TUM 3B model
 * geometrisi (menudeki araba dahil) sessizce dusuyordu.            */
static uint32_t g_ss_vb = 0, g_ss_off = 0, g_ss_stride = 0;
static uint32_t g_ib_obj = 0;
static uint32_t g_idx_scratch = 0;
static uint64_t g_dip_draws = 0, g_dip_skip = 0, g_dip_tris = 0;
static uint32_t g_act_of_key[256];      /* DIK -> uAppData (0 = bagli degil) */
static uint32_t g_act_ofs[256];         /* DIK -> eylem dizisindeki ofset */
static int      g_act_count = 0;
static unsigned char g_kb_prev[256];
static uint32_t g_di_seq = 1;
static uint64_t g_di_events = 0;
static uint32_t host_us_ms(void){ uint64_t host_us(void); return (uint32_t)(host_us()/1000ull); }
static uint32_t *g_lk_tid=0; static unsigned *g_lk_cnt=0; static uint64_t *g_lk_byt=0;
static uint32_t *g_lock_obj=0; static unsigned *g_lock_cnt=0; static int g_lock_n=0;
static uint64_t g_snd_bytes=0, g_snd_nonzero=0, g_snd_zero=0, g_snd_play=0;
static uint32_t g_cur_rt = 0;
/* D3DRENDERSTATETYPE degerleri; varsayilanlar D3D9'un baslangic durumu */
/* Oyunun ayar menusunde sunulacak cozunurlukler (masaustu en sonda). */
/* Oyunun KENDI ic cozunurluk tablosu.  Deneysel olarak olculdu: kayit
 * defterindeki g_RacingResolution bu tabloya indeks; 6 ve uzeri oyunun
 * kendi dizisini TASIRIYOR (CreateDevice'a 8126268x268571776 gibi cop
 * geliyordu).  Daha once burada 12 mod ilan ediliyordu, yani ayar menusu
 * oyunun destekledigi 6 satirdan fazlasini gosteriyor ve fazladan satiri
 * secmek dizi tasmasina -> cokmeye yol aciyordu.  Vanilya NFSU2 yalnizca
 * 4:3/5:4 destekler; genis ekran yoktur. */
/* Neden tabloyu genisletmiyoruz: 7. bir mod ILAN etmek oyunun kendi
 * dizisini tasiriyor ve cop boyut (her seferinde 8126268x268571776)
 * uretiyor; oyun onu saklayip render hedeflerini de o boyutla yaratiyor.
 * Tabloyu konuk bellekte yamalamak da denendi, bitisik {w,h} dizisi olarak
 * bulunamadi.  Bu yuzden satir sayisi 6'da kaliyor, yalnizca ETIKETLER ve
 * gercek cihaz boyutu bizim. */
/* --- Ayar menusundeki cozunurluk satirlari ---------------------------
 * Oyunun ic tablosu (g_RacingResolution 0..5, secili satir [0x870d1c])
 * sabit ve hepsi 4:3.  Menudeki yazi .rdata'daki HAZIR metinlerden geliyor
 * (sub_0049b690: satir -> "640x480".."1600x1200"; yalnizca gosterim, boyut
 * bu metinden okunmuyor).  EnumAdapterModes listesi ise yalnizca hangi
 * satirlarin SECILEBILECEGINI belirliyor: oyun tablo degerini listede
 * ariyor.  Olculdu (2026-09-27, kullanicinin ekrani): liste etiketlerle
 * degistirilince oklar hicbir satira gidemedi; eskiden de masaustu modu
 * 1920x1080 bildirildigi icin 1600x1200 satiri (sozde "1080p") hic
 * secilemiyordu.
 *
 * Duzen: liste = oyunun tablosu (6 satirin hepsi secilebilir), masaustu
 * modu >= 1600x1200, menu metinleri bellekte yeniden yaziliyor ve cihaz
 * satirin GERCEK cozunurlugunde kuruluyor (oran farki Hor+, res_pick).
 * Metin yuvalari 8/8/12/12/12/12 bayt: ilk iki satira kisa etiket.
 * NOWIDE=1: oyunun orijinal tablosu ve metinleri. */
static const uint32_t g_tbl[6][2] = {
    {640,480},{800,600},{1024,768},{1280,960},{1280,1024},{1600,1200},
};
static uint32_t g_modes[6][2] = {
    {1280,720},{1920,1080},{2560,1440},{0,0},{3840,2160},{1600,1200},
};
#define ROW_NATIVE 3
static const uint32_t g_lbl_addr[6] = {0x0078fa1cu,0x0078fa14u,0x0078fa08u,0x0078f9fcu,0x0078f9f0u,0x0078f9e4u};
static const uint32_t g_lbl_cap[6]  = {8,8,12,12,12,12};
static void label_put(int row, const char *txt){
    uint32_t a = g_lbl_addr[row], n = (uint32_t)strlen(txt);
    if (n + 1 > g_lbl_cap[row] || !mem_is_committed(a) || !mem_is_committed(a + g_lbl_cap[row] - 1)) return;
    /* beklenen orijinal metin yerinde mi (baska bir EXE surumune yazmayalim) */
    uint8_t c0 = MR8(a);
    if (!(c0 >= '0' && c0 <= '9')) return;
    memset(g_mem + a, 0, g_lbl_cap[row]); memcpy(g_mem + a, txt, n);
}
static int g_cur_row = -1;             /* oyunun etkin satiri (-1 = bilinmiyor) */
static int modemap_on(void){ return !ENVC("NOWIDE"); }
/* Satirlar GORUNUMUN (pencere / tam ekran) oranindan uretilir: "1080p" =
 * gorunum oraninda 1080 satir.  Boylece cizim yuzeyi pencereyi bantsiz
 * doldurur, cozunurluk yalnizca kaliteyi degistirir.  "Ekran" satiri
 * gorunumun kendi piksel sayisi (1:1, 9 MP ust sinir); 1600x1200 bilerek
 * 4:3 kalir (orijinal gorunum, kenarlarda bant). */
static void modes_refresh(void){
    unsigned vw = 0, vh = 0;
    void metal_view_size(unsigned*, unsigned*); metal_view_size(&vw, &vh);
    if (!vw || !vh){ vw = 2560; vh = 1600; }
    static const unsigned hts[6] = {720, 1080, 1440, 0, 2160, 0};
    for (int i = 0; i < 6; i++){
        if (!hts[i]) continue;
        unsigned hh = hts[i], ww = (unsigned)((double)hh * vw / vh + 0.5) & ~1u;
        g_modes[i][0] = ww; g_modes[i][1] = hh;
    }
    double dw = vw, dh = vh;
    const char *mp = ENVC("NFSMAXPIX"); double maxpix = (mp && *mp) ? atof(mp) : 9.0e6;
    if (maxpix > 1.0e5 && dw * dh > maxpix){ double k = sqrt(maxpix / (dw * dh)); dw *= k; dh *= k; }
    unsigned w = (unsigned)dw & ~1u, h = (unsigned)dh & ~1u;
    g_modes[ROW_NATIVE][0] = w; g_modes[ROW_NATIVE][1] = h;
    if (!modemap_on()) return;
    static const char *fixed[6] = {"720p","1080p","1440p",0,"4K","1600x1200"};
    char nb[16];
    for (int i = 0; i < 6; i++){
        if (fixed[i]) label_put(i, fixed[i]);
        else { snprintf(nb, sizeof nb, "%ux%u", w, h); label_put(i, nb); }
    }
}
static const uint32_t *mode_label(unsigned i){
    if (!modemap_on()) return g_tbl[i];
    if (i == ROW_NATIVE && !g_modes[i][0]) modes_refresh();
    return g_modes[i];
}
static int tbl_row(uint32_t w, uint32_t h){
    for (int i = 0; i < 6; i++) if (g_tbl[i][0] == w && g_tbl[i][1] == h) return i;
    return -1;
}

static uint32_t g_rs[256] = {0};
/* RSLOG=1: oyunun HANGI render state'leri kurdugunu olc.  Ele almadigimiz
 * bir state'i kor tahminle uygulamak yerine once kullanildigini dogrula. */
static uint32_t g_rs_hits[256] = {0};
static uint32_t g_rs_vals[256][4];        /* ilk 4 farkli deger */
static uint8_t  g_rs_nval[256] = {0};

/* ---- D3D9 durum bloklari (state block) ------------------------------
 * Oyun BeginStateBlock/EndStateBlock ile durum kaydediyor, sonra efekt
 * gecisleri etrafinda Capture/Apply ile kaydedip geri yukluyor (izde 367
 * kayit, 418 Capture, 326 Apply).  Bunlar BOS nesne olarak donuyordu, yani
 * Apply hicbir seyi geri yuklemiyordu: karisim kipi, alfa testi, doku asama
 * islemleri bir onceki gecisten sizip kaliyordu -> asiri parlak isik
 * cizgileri ve yanlis karanlik yuzeyler. */
#define MAXSB     512
#define MAXSBREC  128
typedef struct { uint32_t magic; uint32_t a[8]; int nargs; } SBRec;
typedef struct { SBRec r[MAXSBREC]; int n; int used; } SBlock;
static SBlock  g_sb[MAXSB];
static int     g_sbn = 1;          /* 0 = gecersiz */
static int     g_sb_rec = 0;       /* kayittaki blok, 0 = kayit yok */
static int     g_sb_replay = 0;
static uint64_t g_sb_applied = 0, g_sb_captured = 0, g_sb_dropped = 0, g_sb_full = 0;

/* Ham durum golgeleri: Capture bunlardan okur (oyunun VERDIGI degerler). */
static uint32_t g_raw_vs = 0, g_raw_ps = 0, g_raw_decl = 0, g_raw_fvf = 0;
static uint32_t g_raw_tex[8] = {0};
/* D3DTSS_TEXCOORDINDEX'in D3D VARSAYILANI asama numarasidir, 0 DEGIL.
 * Tablo sifirla baslayinca asama 1 ve 2 -- far isigi dokusu ve sonum
 * rampasi -- yolun kendi (tekrarlanan) diffuse UV'siyle orneklendi; far
 * konisi tum yol boyunca kopyalandi ve zemin bastan basa patlak parladi.
 * Oyun bu durumu hic yazmiyor cunku varsayilan zaten dogru olmali. */
static uint32_t g_tss_raw[8][40] = {
    [0] = { [11] = 0u }, [1] = { [11] = 1u }, [2] = { [11] = 2u },
    [3] = { [11] = 3u }, [4] = { [11] = 4u }, [5] = { [11] = 5u },
    [6] = { [11] = 6u }, [7] = { [11] = 7u },
};
static uint32_t g_smp_raw[16][20];

/* Bu metot bir state block'a kaydedilir mi? */
/* --- metot KIMLIGI ------------------------------------------------------
 * Dagitim eskiden metot ADINI karsilastiriyordu.  NMEQ'in ilk-harf
 * filtresi ise yaramadi: D3D9 cihaz metotlarinin neredeyse hepsi
 * "Set"/"Get"/"Draw" ile basliyor, yani her zincir adiminda yine TAM
 * strcmp calisiyordu (olculdu: konuk thread'in %11'i strcmp, bunun
 * 2335 ornegi com_call'dan).  Kimlik artik metot basina BIR KEZ
 * cozuluyor ((iface,slot) -> id onbellegi), zincir tamamen tamsayi
 * karsilastirmasi.  Kosuda 377 milyon com_call var. */
enum {
    MID__none = 0,
    MID_SetRenderState,
    MID_SetTextureStageState,
    MID_SetSamplerState,
    MID_SetTexture,
    MID_SetVertexShader,
    MID_SetPixelShader,
    MID_SetVertexDeclaration,
    MID_SetFVF,
    MID_SetVertexShaderConstantF,
    MID_SetPixelShaderConstantF,
    MID_SetVertexShaderConstantI,
    MID_SetVertexShaderConstantB,
    MID_SetTransform,
    MID_SetMaterial,
    MID_SetLight,
    MID_LightEnable,
    MID_SetStreamSource,
    MID_SetIndices,
    MID_SetViewport,
    MID_SetScissorRect,
    MID_SetNPatchMode,
    MID_SetClipPlane,
    MID_GetSurfaceLevel,
    MID_GetCubeMapSurface,
    MID_LockRect,
    MID_UnlockRect,
    MID_AddDirtyRect,
    MID_QueryInterface,
    MID_AddRef,
    MID_Release,
    MID_GetAdapterCount,
    MID_GetAdapterModeCount,
    MID_EnumAdapterModes,
    MID_GetAdapterDisplayMode,
    MID_GetAdapterIdentifier,
    MID_GetDeviceCaps,
    MID_CreateDevice,
    MID_TestCooperativeLevel,
    MID_GetAvailableTextureMem,
    MID_GetDirect3D,
    MID_GetDisplayMode,
    MID_Clear,
    MID_BeginScene,
    MID_EndScene,
    MID_Present,
    MID_StretchRect,
    MID_SetRenderTarget,
    MID_Reset,
    MID_DrawIndexedPrimitive,
    MID_DrawPrimitiveUP,
    MID_CreateTexture,
    MID_CreateCubeTexture,
    MID_CreateVolumeTexture,
    MID_CreateVertexBuffer,
    MID_CreateIndexBuffer,
    MID_CreateRenderTarget,
    MID_CreateDepthStencilSurface,
    MID_CreateOffscreenPlainSurface,
    MID_GetBackBuffer,
    MID_GetRenderTarget,
    MID_GetDepthStencilSurface,
    MID_SetDepthStencilSurface,
    MID_CreateVertexDeclaration,
    MID_CreateVertexShader,
    MID_CreatePixelShader,
    MID_BeginStateBlock,
    MID_CreateStateBlock,
    MID_CreateQuery,
    MID_EndStateBlock,
    MID_CreateAdditionalSwapChain,
    MID_GetSwapChain,
    MID_GetNumberOfSwapChains,
    MID_GetTransform,
    MID_GetMaterial,
    MID_GetLight,
    MID_GetViewport,
    MID_GetScissorRect,
    MID_GetClipStatus,
    MID_GetCreationParameters,
    MID_GetRasterStatus,
    MID_GetGammaRamp,
    MID_GetClipPlane,
    MID_GetDevice,
    MID_Lock,
    MID_GetLevelCount,
    MID_GetType,
    MID_GetDesc,
    MID_GetLevelDesc,
    MID_CreateSoundBuffer,
    MID_GetCaps,
    MID_SetCooperativeLevel,
    MID_GetSpeakerConfig,
    MID_DuplicateSoundBuffer,
    MID_Unlock,
    MID_Play,
    MID_Stop,
    MID_SetVolume,
    MID_SetFrequency,
    MID_GetStatus,
    MID_GetCurrentPosition,
    MID_GetFormat,
    MID_EnumDevices,
    MID_EnumDevicesBySemantics,
    MID_GetDeviceStatus,
    MID_Initialize,
    MID_FindDevice,
    MID_EnumObjects,
    MID_GetObjectInfo,
    MID_GetDeviceInfo,
    MID_SetDataFormat,
    MID_Acquire,
    MID_Unacquire,
    MID_Poll,
    MID_GetDeviceState,
    MID_BuildActionMap,
    MID_SetActionMap,
    MID_GetDeviceData,
    MID_GetCapabilities,
    MID_GetProperty,
    MID_SetProperty,
    MID_SetEventNotification,
    MID_GetDataSize,
    MID_GetData,
    MID_Apply,
    MID_Capture,
    MID_GetFunction,
    MID_GetDeclaration,
    MID__count
};
static const char *const g_midname[MID__count] = {
    "",
    "SetRenderState",
    "SetTextureStageState",
    "SetSamplerState",
    "SetTexture",
    "SetVertexShader",
    "SetPixelShader",
    "SetVertexDeclaration",
    "SetFVF",
    "SetVertexShaderConstantF",
    "SetPixelShaderConstantF",
    "SetVertexShaderConstantI",
    "SetVertexShaderConstantB",
    "SetTransform",
    "SetMaterial",
    "SetLight",
    "LightEnable",
    "SetStreamSource",
    "SetIndices",
    "SetViewport",
    "SetScissorRect",
    "SetNPatchMode",
    "SetClipPlane",
    "GetSurfaceLevel",
    "GetCubeMapSurface",
    "LockRect",
    "UnlockRect",
    "AddDirtyRect",
    "QueryInterface",
    "AddRef",
    "Release",
    "GetAdapterCount",
    "GetAdapterModeCount",
    "EnumAdapterModes",
    "GetAdapterDisplayMode",
    "GetAdapterIdentifier",
    "GetDeviceCaps",
    "CreateDevice",
    "TestCooperativeLevel",
    "GetAvailableTextureMem",
    "GetDirect3D",
    "GetDisplayMode",
    "Clear",
    "BeginScene",
    "EndScene",
    "Present",
    "StretchRect",
    "SetRenderTarget",
    "Reset",
    "DrawIndexedPrimitive",
    "DrawPrimitiveUP",
    "CreateTexture",
    "CreateCubeTexture",
    "CreateVolumeTexture",
    "CreateVertexBuffer",
    "CreateIndexBuffer",
    "CreateRenderTarget",
    "CreateDepthStencilSurface",
    "CreateOffscreenPlainSurface",
    "GetBackBuffer",
    "GetRenderTarget",
    "GetDepthStencilSurface",
    "SetDepthStencilSurface",
    "CreateVertexDeclaration",
    "CreateVertexShader",
    "CreatePixelShader",
    "BeginStateBlock",
    "CreateStateBlock",
    "CreateQuery",
    "EndStateBlock",
    "CreateAdditionalSwapChain",
    "GetSwapChain",
    "GetNumberOfSwapChains",
    "GetTransform",
    "GetMaterial",
    "GetLight",
    "GetViewport",
    "GetScissorRect",
    "GetClipStatus",
    "GetCreationParameters",
    "GetRasterStatus",
    "GetGammaRamp",
    "GetClipPlane",
    "GetDevice",
    "Lock",
    "GetLevelCount",
    "GetType",
    "GetDesc",
    "GetLevelDesc",
    "CreateSoundBuffer",
    "GetCaps",
    "SetCooperativeLevel",
    "GetSpeakerConfig",
    "DuplicateSoundBuffer",
    "Unlock",
    "Play",
    "Stop",
    "SetVolume",
    "SetFrequency",
    "GetStatus",
    "GetCurrentPosition",
    "GetFormat",
    "EnumDevices",
    "EnumDevicesBySemantics",
    "GetDeviceStatus",
    "Initialize",
    "FindDevice",
    "EnumObjects",
    "GetObjectInfo",
    "GetDeviceInfo",
    "SetDataFormat",
    "Acquire",
    "Unacquire",
    "Poll",
    "GetDeviceState",
    "BuildActionMap",
    "SetActionMap",
    "GetDeviceData",
    "GetCapabilities",
    "GetProperty",
    "SetProperty",
    "SetEventNotification",
    "GetDataSize",
    "GetData",
    "Apply",
    "Capture",
    "GetFunction",
    "GetDeclaration",
};
static int mid_resolve(const char *nm){
    for (int i = 1; i < MID__count; i++)
        if (nm[0] == g_midname[i][0] && !strcmp(nm, g_midname[i])) return i;
    return -1;                       /* bilinmiyor: bir daha arama */
}
#define MIDIF 32
#define MIDM 160
static int16_t g_midc[MIDIF][MIDM];  /* 0 = henuz cozulmedi */
#define MID(x_) (mid == MID_##x_)

static int sb_recordable(int mid){
    return MID(SetRenderState)       || MID(SetTextureStageState)
        || MID(SetSamplerState)      || MID(SetTexture)
        || MID(SetVertexShader)      || MID(SetPixelShader)
        || MID(SetVertexDeclaration) || MID(SetFVF)
        || MID(SetVertexShaderConstantF) || MID(SetPixelShaderConstantF)
        || MID(SetVertexShaderConstantI) || MID(SetVertexShaderConstantB)
        || MID(SetTransform)         || MID(SetMaterial)
        || MID(SetLight)             || MID(LightEnable)
        || MID(SetStreamSource)      || MID(SetIndices)
        || MID(SetViewport)          || MID(SetScissorRect)
        || MID(SetNPatchMode)        || MID(SetClipPlane);
}
static int g_rs_init = 0;
/* 19=SRCBLEND 20=DESTBLEND 22=CULLMODE 27=ALPHABLENDENABLE
 * 15=ALPHATESTENABLE 24=ALPHAREF 25=ALPHAFUNC 168=COLORWRITEENABLE 171=BLENDOP */
/* g_rs[] degismediyse rs_apply() saf israf: oyun kosuda SetRenderState'i
 * 91.5 MILYON kez cagiriyor ve buyuk cogunlugu ayni degeri yeniden
 * yaziyor.  rs_apply tamamen g_rs'ten turedigi icin atlamak birebir
 * ayni sonucu verir.  Bayrak KALICI: metal henuz hazir degilken yapilan
 * degisiklikler cihaz gelince yine uygulanir. */
static int g_rs_dirty = 1;
static uint64_t g_rs_skip = 0, g_rs_run = 0;
static void rs_apply(void){
    g_rs_run++;
    if (!g_rs_init){ g_rs_init=1;
        g_rs[19]=5; g_rs[20]=6; g_rs[27]=0; g_rs[168]=0xF; g_rs[171]=1;
        g_rs[22]=3;                            /* CULLMODE varsayilani: CCW */
        g_rs[15]=0; g_rs[24]=0; g_rs[25]=8;
        g_rs[7]=1; g_rs[14]=1; g_rs[23]=4;     /* Z varsayilanlari: acik */
        g_rs[193]=0xFFFFFFFFu; }               /* BLENDFACTOR varsayilani: beyaz */
    unsigned key = (g_rs[27]?1u:0u)
                 | ((g_rs[19] & 31u) << 1)
                 | ((g_rs[20] & 31u) << 6)
                 | ((g_rs[171] & 7u) << 11)
                 | ((g_rs[168] & 15u) << 14);
    /* alfa testi: yalnizca "buyuk/buyuk-esit" bicimini destekliyoruz */
    float aref = 0.f;
    if (g_rs[15] && (g_rs[25]==5 || g_rs[25]==6 || g_rs[25]==7))
        aref = (float)(g_rs[24] & 0xFFu) / 255.0f;
    metal_set_blend(key, aref);
    /* D3DRS_BLENDFACTOR (193): SRCBLEND/DESTBLEND = BLENDFACTOR /
     * INVBLENDFACTOR oldugunda karisim bu SABIT renkle capraz gecis yapar.
     * Metal'de bu kodlayici durumu (setBlendColor), boru hatti degil. */
    { void metal_set_blendcolor(uint32_t argb);
      metal_set_blendcolor(g_rs[193]); }
    /* D3DRS_CULLMODE (22): 1=NONE 2=CW 3=CCW.  Hic uygulanmiyordu; Metal'in
     * varsayilani CullModeNone oldugu icin her seyin ARKA YUZLERI de
     * ciziliyordu.  Kapali bir gövdenin uzak tarafi yakin tarafla derinlik
     * yarisina giriyor -> arabanin kapisinda/tamponunda, bina kenarlarinda
     * ve gokyuzu sinirinda surekli titreyen lekeler. */
    { void metal_set_cull(unsigned);
      metal_set_cull(g_rs[22] ? g_rs[22] : 3u); }
    /* D3DRS_FOGENABLE (28) + D3DRS_FOGCOLOR (34).  Kose gölgelendiricisi
     * varken sis carpani oFog'dan gelir; sabit islevli asama pikseli
     * FOGCOLOR'a dogru karistirir.  Olculdu: FOGENABLE 5.3M kez kuruluyor,
     * 18 kose gölgelendiricisinden 6'si oFog yaziyor. */
    { void metal_set_fog(unsigned, unsigned);
      metal_set_fog(g_rs[28], g_rs[34]); }
    /* Derinlik: 7=ZENABLE 14=ZWRITEENABLE 23=ZFUNC (varsayilan LESSEQUAL=4).
     * Arayuz Z'yi kapali cizdigi icin bunu cizim basina tasimak sart. */
    { void metal_set_depth(unsigned);
      unsigned zf = g_rs[23] ? (g_rs[23] & 7u) : 4u;
      unsigned dm = (g_rs[7] ? 1u : 0u) | (g_rs[14] ? 2u : 0u) | (zf << 2);
      metal_set_depth(dm); }
}
static uint64_t g_rt_set=0, g_rt_screen=0;
static uint64_t g_rt_tex=0;
static uint32_t g_bb_w = 1280, g_bb_h = 720;   /* geri tampon boyutu */
/* Kup dokular IDirect3DTexture9 vtable'ini paylasiyor; hangi nesnenin kup
 * oldugunu nesne icinde saklamak GUVENSIZ (2B nesneler daha kucuk ayrilir,
 * ayrilmis alanin disini okumak komsu obek verisini okur).  Ayri kayit: */
/* Kayit SINIRSIZ olmali: oyun her cihaz Reset'inde (cozunurluk / tam ekran
 * degisimi, ikiser Reset) 2 kup dokuyu yeniden yaratiyor.  Eskiden 64
 * girislik tablo 32. degisimde doluyordu; sonraki kup "2B" sanilip
 * GetCubeMapSurface yanlis arguman sayisiyla donuyor, yigin 4 bayt kayiyor
 * ve oyun cokuyordu (kullanici logu: tam 64 Reset'ten sonra sub_005b9920,
 * adres 0x80).  COM nesneleri hic birakilmadigi (com_alloc artan isaretci)
 * icin adres yeniden kullanilmaz; bit haritasi guvenli. */
static uint8_t g_cube_bits[(COM_MAX - COM_BASE) / 16u / 8u];
static unsigned g_cube_n = 0;
static void cube_mark(uint32_t obj){
    if (obj < COM_BASE || obj >= COM_MAX) return;
    uint32_t k = (obj - COM_BASE) >> 4;
    g_cube_bits[k >> 3] |= (uint8_t)(1u << (k & 7u)); g_cube_n++;
}
static int is_cube_tex(uint32_t obj){
    if (obj < COM_BASE || obj >= COM_MAX || (obj & 15u)) return 0;
    uint32_t k = (obj - COM_BASE) >> 4;
    return (g_cube_bits[k >> 3] >> (k & 7u)) & 1u;
}
static uint64_t g_st_ok=0, g_st_null=0, g_st_zero=0, g_st_other=0;
static uint32_t g_tex_stage[4] = {0,0,0,0};
static float    g_psc[32][4];                    /* ps sabitleri c0..c31 */
static uint32_t g_psc_max = 0;                   /* goruldugu en yuksek indis+1 */
static uint32_t g_psc_drop = 0;                  /* 32 ustu (dusurulen) yazma */
static int      g_cur_ps_id = 0;                 /* etkin ps kimligi */
static int      g_psn = 0;                       /* kayitli ps sayisi */
static uint64_t g_stage_hist[4] = {0,0,0,0};
static float g_rmin[3] = { 1e30f, 1e30f, 1e30f };
static float g_rmax[3] = { -1e30f, -1e30f, -1e30f };
/* son 8 SetVertexShaderConstantF cagrisi (tani icin) */
static struct { uint32_t sr, pd, n; } g_sc[8];
static unsigned g_scn = 0;

/* D3DDECLTYPE -> v# kaydina 4 float oku */
static void decl_fetch(uint32_t addr, unsigned type, float out[4]){
    out[0]=out[1]=out[2]=0.f; out[3]=1.f;
    switch (type){
    case 0: out[0]=MRF32(addr); break;                                   /* FLOAT1 */
    case 1: out[0]=MRF32(addr); out[1]=MRF32(addr+4); break;             /* FLOAT2 */
    case 2: out[0]=MRF32(addr); out[1]=MRF32(addr+4);
            out[2]=MRF32(addr+8); break;                                 /* FLOAT3 */
    case 3: out[0]=MRF32(addr); out[1]=MRF32(addr+4);
            out[2]=MRF32(addr+8); out[3]=MRF32(addr+12); break;          /* FLOAT4 */
    case 4: /* D3DCOLOR: bellekte B,G,R,A -> (R,G,B,A) */
            out[0]=MR8(addr+2)/255.f; out[1]=MR8(addr+1)/255.f;
            out[2]=MR8(addr)/255.f;   out[3]=MR8(addr+3)/255.f; break;
    case 5: out[0]=MR8(addr); out[1]=MR8(addr+1);
            out[2]=MR8(addr+2); out[3]=MR8(addr+3); break;               /* UBYTE4 */
    case 8: out[0]=MR8(addr)/255.f; out[1]=MR8(addr+1)/255.f;
            out[2]=MR8(addr+2)/255.f; out[3]=MR8(addr+3)/255.f; break;   /* UBYTE4N */
    case 6: out[0]=(int16_t)MR16(addr); out[1]=(int16_t)MR16(addr+2); break;
    case 7: out[0]=(int16_t)MR16(addr);   out[1]=(int16_t)MR16(addr+2);
            out[2]=(int16_t)MR16(addr+4); out[3]=(int16_t)MR16(addr+6); break;
    case 9: out[0]=(int16_t)MR16(addr)/32767.f;
            out[1]=(int16_t)MR16(addr+2)/32767.f; break;
    case 10:out[0]=(int16_t)MR16(addr)/32767.f;   out[1]=(int16_t)MR16(addr+2)/32767.f;
            out[2]=(int16_t)MR16(addr+4)/32767.f; out[3]=(int16_t)MR16(addr+6)/32767.f; break;
    case 11:out[0]=MR16(addr)/65535.f; out[1]=MR16(addr+2)/65535.f; break;
    case 12:out[0]=MR16(addr)/65535.f;   out[1]=MR16(addr+2)/65535.f;
            out[2]=MR16(addr+4)/65535.f; out[3]=MR16(addr+6)/65535.f; break;
    default: break;
    }
}

/* --- DRAWDUMP=<kare#>: o karedeki tum cizimleri ayrintili dok --- */
static uint32_t g_present_no = 0;
#define ZSURF_MAGIC 0x5A535246u   /* 'ZSRF': Z yuzeyi nesnesi, +52 = kimlik */
static uint32_t g_cur_zid = 1;           /* bagli Z yuzeyi (0 = yok) */
/* --- cihaz kaybi / yeniden kurma -----------------------------------------
 * Pencere kipi degisince (tam ekran <-> pencere) cizim cozunurlugu de
 * degismeli; oyunun render hedefleri cihaz boyutuna bagli oldugu icin bunu
 * ANCAK oyunun kendisi Reset ile yapabilir.  Windows'ta alt-tab'da oldugu
 * gibi cihazi "kayip" bildiriyoruz: Present D3DERR_DEVICELOST,
 * TestCooperativeLevel D3DERR_DEVICENOTRESET doner, oyun kaynaklarini
 * birakip Reset cagirir, res_pick yeni kipe gore boyut secer. */
static _Atomic int g_lost = 0;
static unsigned long g_tcl_n = 0, g_tcl_lost_n = 0, g_lost_presents = 0, g_resets = 0;
void com_request_reset(void){
    if (!g_dev) return;          /* cihaz henuz yok: CreateDevice zaten yeni kipi gorecek */
    atomic_store(&g_lost, 1); }
static int g_wc_armed = 0, g_wc_after = 0;
static int      g_dd_want    = -1;
static int dd_on(void){
    static int init=0;
    if (!init){ init=1; const char *e=ENVC("DRAWDUMP"); g_dd_want = e&&*e ? atoi(e) : -1; }
    return g_dd_want >= 0 && (uint32_t)g_dd_want == g_present_no;
}
static void dd_decl(uint32_t d){
    uint32_t el = (d && mem_is_committed(d+4)) ? MR32(d+4) : 0;
    static const char *TY[]={"FLOAT1","FLOAT2","FLOAT3","FLOAT4","D3DCOLOR","UBYTE4",
        "SHORT2","SHORT4","UBYTE4N","SHORT2N","SHORT4N","USHORT2N","USHORT4N",
        "UDEC3","DEC3N","FLOAT16_2","FLOAT16_4","UNUSED"};
    static const char *US[]={"POSITION","BLENDWEIGHT","BLENDINDICES","NORMAL","PSIZE",
        "TEXCOORD","TANGENT","BINORMAL","TESSFACTOR","POSITIONT","COLOR","FOG",
        "DEPTH","SAMPLE"};
    if (!el){ fprintf(stderr,"      decl=0x%08x (bildirim yok)\n", d); return; }
    fprintf(stderr,"      decl=0x%08x:", d);
    for (uint32_t k=0;k<16;k++){
        uint32_t e=el+8u*k;
        if (!mem_is_committed(e+7)) break;
        uint32_t of=MR16(e+2), ty=MR8(e+4), us=MR8(e+6), ui=MR8(e+7);
        if (MR16(e)==0xFFu && ty==17u) break;
        fprintf(stderr," %u:%s/%s%u", of, ty<18?TY[ty]:"?", us<14?US[us]:"?", ui);
    }
    fprintf(stderr,"\n");
}
int g_com_trace = 0;
void com_set_trace(int v){ g_com_trace=v; }
void draw_stats_save(uint64_t*,uint64_t*);
/* Yeniden oynatma (state block Apply) sirasinda argumanlar guest yiginindan
 * degil kayittan gelir. */
static const uint32_t *g_arg_ovr = 0;
static uint32_t A(CPU*c,int i){
    if (g_arg_ovr) return g_arg_ovr[i & 7];
    return MR32(c->esp + 4u*(uint32_t)i);
}

/* --- guest geri cagrisini stdcall olarak calistir --------------------
 * DirectInput'un EnumDevices ailesi geri cagirma ile calisiyor.  Geri
 * cagirma hic yapilmazsa oyun HIC giris cihazi bulamiyor: fare imleci
 * okunur ama menu eylemleri hicbir seye baglanmaz.
 * CPU durumu korunur; stdcall oldugu icin callee argumanlari temizler. */
uint32_t call_guest(CPU *c, uint32_t fn, int nargs, const uint32_t *args){
    if (!fn) return 0;
    CPU save = *c;
    for (int i = nargs - 1; i >= 0; i--) PUSH32(args[i]);
    PUSH32(0xDEADBEEFu);                    /* sahte donus adresi */
    dispatch_indirect(c, fn);
    uint32_t ret = c->eax;
    *c = save;                              /* cagri oncesi duruma don */
    return ret;
}

/* GUID_SysKeyboard / GUID_SysMouse (DirectInput sistem cihazlari) */
static void put_guid(uint32_t p, uint32_t d1, uint16_t d2, uint16_t d3,
                     const uint8_t d4[8]){
    MR32(p)=d1; MR16(p+4)=d2; MR16(p+6)=d3;
    for (int i=0;i<8;i++) MR8(p+8+i)=d4[i];
}

/* DIDEVICEINSTANCEA doldur (580 bayt) ve adresini dondur. */
static uint32_t make_devinst(int keyboard){
    static uint32_t kb = 0, ms = 0;
    uint32_t *slot = keyboard ? &kb : &ms;
    if (*slot) return *slot;
    uint32_t p = gbuf_alloc(600);
    memset(g_mem+p, 0, 600);
    MR32(p) = 580u;                                   /* dwSize */
    static const uint8_t tail[8] = {0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00};
    put_guid(p+4,  keyboard?0x6F1D2B61u:0x6F1D2B60u, 0xD5A0, 0x11CF, tail);
    put_guid(p+20, keyboard?0x6F1D2B61u:0x6F1D2B60u, 0xD5A0, 0x11CF, tail);
    /* dwDevType: alt tur << 8 | tur.  0x13=KEYBOARD, 0x12=MOUSE */
    MR32(p+36) = keyboard ? 0x0313u : 0x0212u;
    snprintf((char*)(g_mem+p+40),  260, "%s", keyboard ? "Keyboard" : "Mouse");
    snprintf((char*)(g_mem+p+300), 260, "%s", keyboard ? "Keyboard" : "Mouse");
    *slot = p;
    return p;
}

static void fill_caps(uint32_t p);


/* --- kose isleme: bildirime gore girisleri topla, shader'i calistir --- */
typedef struct { uint16_t off; uint8_t type; int8_t reg; } DElem;

static int g_fixmat = -1;
static uint64_t g_t_draw_ns = 0, g_v_xform = 0;
static uint64_t g_t_vs_ns = 0;
static uint64_t g_v_per_vs[64] = {0};
static inline uint64_t now_ns(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return (uint64_t)ts.tv_sec*1000000000ull + (uint64_t)ts.tv_nsec;
}
/* (kose gölgelendirici, kose bildirimi) ciftine karsilik gelen GPU
 * gölgelendirici kimligi.  Ayni bytecode farkli bildirimlerle
 * kullanilabildigi icin anahtar ikisi birlikte. */
#define MAXGVS 256
static struct { uint32_t vs, decl; int gid, state; } g_gvs[MAXGVS];
static int g_gvsn = 0, g_gvs_next = 1;
static int g_gvs_nk[MAXGVS];
static int g_gpuvs = -1;
static uint64_t g_gpuvs_draws = 0, g_gpuvs_fail = 0, g_gpuvs_drop = 0;

static int gpuvs_id(const VSProg *prog, uint32_t vs, uint32_t decl,
                    const DElem *el, int nel){
    for (int i = 0; i < g_gvsn; i++)
        if (g_gvs[i].vs == vs && g_gvs[i].decl == decl)
            return g_gvs[i].state > 0 ? g_gvs[i].gid : -1;
    if (g_gvsn >= MAXGVS || g_gvs_next >= MAXGVS) return -1;
    int slot = g_gvsn++;
    g_gvs[slot].vs = vs; g_gvs[slot].decl = decl;
    g_gvs[slot].state = -1; g_gvs[slot].gid = 0;

    int dcl[16][2];
    for (int i = 0; i < 16; i++){ dcl[i][0] = -1; dcl[i][1] = 0; }
    for (int k = 0; k < nel; k++){
        int rg = el[k].reg;
        if (rg >= 0 && rg < 16){ dcl[rg][0] = (int)el[k].off; dcl[rg][1] = (int)el[k].type; }
    }
    int gid = g_gvs_next;
    /* GPUVSMASK: hangi cevrilmis gölgelendiricilere izin verilecek (bit0=vs_1).
     * Bir goruntu hatasini gölgelendirici bazinda ayiklamak icin. */
    { static long mask = -1;
      if (mask < 0){ const char *em = ENVC("GPUVSMASK");
                     mask = (em && *em) ? strtol(em, 0, 0) : -1L; }
      if (mask >= 0 && !((mask >> (gid-1)) & 1L)){
          if (ENVC("GPUVSLOG"))
              fprintf(stderr, "  [gpuvs] shader=%u bildirim=%08x -> maske disi, CPU'da\n", vs, decl);
          g_gvs_next++;         /* kimlik yine de tuket ki maske bitleri kaymasin */
          return -1;
      } }
    static char msl[160*1024];          /* HD: + yakalama cekirdegi */
    int len = vs_to_msl(prog, gid, (const int (*)[2])dcl, msl, sizeof msl);
    if (len <= 0){ g_gpuvs_fail++; return -1; }
    void metal_register_vs(int, const char *);
    int  metal_vs_ready(int);
    metal_register_vs(gid, msl);
    if (!metal_vs_ready(gid)){ g_gpuvs_fail++; return -1; }
    g_gvs_next++;
    g_gvs[slot].gid = gid; g_gvs[slot].state = 1;
    g_gvs_nk[gid] = vs_kmax(prog);
#if NFS_ENHANCED
    /* HD isin izleme denemesi: konum ogesinin ofseti/turu, konum matrisinin
     * sabit yazmaci ve kemikli olup olmadigi (BLENDINDICES = kullanim 2). */
    { int pr = vs_input_reg(prog, 0, 0), po = -1, pty = -1, cw = -1;
      for (int k = 0; k < nel; k++) if (el[k].reg == pr){ po = (int)el[k].off; pty = (int)el[k].type; }
      int cx = vs_pos_creg(prog, &cw);
      float ramp[2] = { 0.005f, 0.5f };
      int lr = vs_light_creg(prog, ramp);
      void metal_hd_vs_info(int, int, int, int, int, int, int, float, float);
      metal_hd_vs_info(gid, po, pty, (cw == cx + 3) ? cx : -1,
                       vs_input_reg(prog, 2, 0) >= 0 || vs_input_reg(prog, 1, 0) >= 0,
                       vs_fog_creg(prog), lr, ramp[0], ramp[1]); }
#endif
    if (ENVC("GPUVSLOG"))
        fprintf(stderr, "  [gpuvs] shader=%u bildirim=%08x -> vs_%d (%u komut, %d sabit)%s\n",
                vs, decl, gid, vs_ninstr(prog), g_gvs_nk[gid],
                vs_writes_fog(prog) ? "  [oFog YAZIYOR]" : "");
    /* GPUVSDUMP=<gid>: uretilen MSL'yi ve kaynak bytecode'u yaz. */
    { const char *dv = ENVC("GPUVSDUMP");
      if (dv && *dv && atoi(dv) == gid){
          fprintf(stderr, "--- vs_%d kaynak sokum ---\n", gid);
          vs_disasm(prog, stderr);
          fprintf(stderr, "--- vs_%d uretilen MSL ---\n%s\n", gid, msl);
      } }
    return gid;
}

/* Oyunun on yuz WorldViewProj matrisi bozuk geliyor (yalnizca 2. ve 3.
 * satirlari yazili).  FIXMAT: bozuksa makul bir matris koy.  Karar cizim
 * boyunca degismez, bu yuzden kose dongusunun DISINDA bir kez verilir;
 * hem CPU hem GPU yolu ayni yardimciyi kullanir ki ikisi ayrismasin. */
/* --- Genis ekran (Hor+) -------------------------------------------------
 * NFSU2 4:3 tasarlanmis: ic cozunurluk tablosunun 6 satirinin TAMAMI 4:3 ve
 * indeks 6+ diziyi tasiriyor, yani menuden 1080p secilemiyor.  Cihazi
 * 1920x1080 yapinca oyun ayni 4:3 goruntuyu uretip yatayda GERILIYOR
 * (olculdu: 1080p kare, 4:3 karenin saf gerdirilmisi -- fark 2.03, dikey
 * kirpma varsayimi 34.2).
 * Duzeltme: perspektif cizimlerde kirpma uzayi x'ini oyun_orani/cihaz_orani
 * ile olcekle.  Oranlar duzelir ve yatayda DAHA COK sahne gorunur (Hor+).
 * Ayirt etme: oPos.w = dot(v, c3); perspektifte c3.w=0, ortografik
 * (arayuz) matriste c3.w=1.  Arayuze dokunulmaz. */
static float g_aspfix = 1.f;
/* Hor+ carpani (kirpma uzayi x'i icin); HD birlestirme gecisi kamerayi
 * ekranda cizilen geometriyle ayni uzaya almak icin kullanir. */
float com_aspfix(void){ return g_aspfix; }
#if NFS_ENHANCED
static VSProg *g_psprog[256];
/* Yansima ayristirma varyanti: taban ps (kup varyanti icin cube=1), yansima
 * asamasi k, Metal kimligi vid.  Basarida 1. */
int com_ps_refl_variant(int base, int cube, int k, int vid){
    if (base <= 0 || base >= 256 || !g_psprog[base]) return 0;
    static char msl[32768];
    if (ps_to_msl_ex2(g_psprog[base], vid, msl, sizeof msl, cube ? 1u : 0u, k) <= 0) return 0;
    void metal_register_ps(int, const char *);
    metal_register_ps(vid, msl);
    fprintf(stderr, "[HD] yansima varyanti: ps %d%s asama %d -> %d\n",
            base, cube ? " (kup)" : "", k, vid);
    return 1;
}
#endif
static uint32_t g_req_w = 0, g_req_h = 0;   /* oyunun sandigi cozunurluk */
unsigned long g_asp_persp = 0, g_asp_ortho = 0;
/* Oyunun istedigi cozunurluk -> cihazin GERCEKTEN cizecegi cozunurluk.
 * Uc kural tek yerde toplanir: NFSRES pini, menuden genis ekran secimi ve
 * Retina piksel yogunlugu.  CreateDevice ile Reset ayni kurali kullanmak
 * ZORUNDA: eskiden Reset ham degeri geciriyordu, yani oyun icinden
 * cozunurluk degistirince duzeltme sessizce geri aliniyordu. */
static void res_pick(uint32_t gw, uint32_t gh, uint32_t *dw, uint32_t *dh){
    static int rdone = 0, pinned = 0;
    static uint32_t rw = 0, rh = 0;
    *dw = gw; *dh = gh;
    /* Imlec oyunun SANDIGI uzayda olmali, cizim yuzeyinde degil. */
    { void metal_set_game_size(unsigned,unsigned); metal_set_game_size(gw, gh); }
    if (!rdone){ rdone = 1; const char *e = ENVC("NFSRES");
        if (e && *e){ unsigned a = 0, b = 0;
            if (sscanf(e, "%ux%u", &a, &b) == 2 && a >= 640 && b >= 480
                && a <= 8192 && b <= 8192){ rw = a; rh = b; pinned = 1; } } }
    uint32_t aw = rw, ah = rh;
    int row = tbl_row(gw, gh);
    g_cur_row = row;
    if (modemap_on()) modes_refresh();                 /* ekran satiri + menu metinleri */
    if (!pinned && row >= 0 && modemap_on()){
        aw = g_modes[row][0]; ah = g_modes[row][1];
        fprintf(stderr,"com: menu satiri %d -> %ux%u\n", row + 1, aw, ah);
    }
    else if (!pinned){
        uint32_t bw = aw ? aw : gw, bh = ah ? ah : gh;
        const char *nv = ENVC("NFSNATIVE");
        if (bw && bh && !(nv && nv[0] == '0')){
            void metal_native_size(unsigned,unsigned,unsigned*,unsigned*);
            unsigned nw = bw, nh = bh;
            metal_native_size(bw, bh, &nw, &nh);
            if (nw > bw || nh > bh){ aw = nw; ah = nh; } }
    }
    if (!aw || !ah){
        /* Gecersiz kilma yok: cihaz = oyunun istegi.  Onceki bir
         * CreateDevice'tan kalan degerler Reset'ten sonra bayat kalmasin
         * (RT yeniden boyutlama ve GetDisplayMode bunlara bakiyor). */
        g_req_w = g_req_h = 0; g_aspfix = 1.f;
        { void metal_set_ui_aspect(float); metal_set_ui_aspect(1.f); }
        return;
    }
    g_req_w = gw; g_req_h = gh;
    /* Oyun kendi orani icin cizmeye devam eder; farki kirpma uzayinda
     * telafi ediyoruz.  Oran birebir korunuyorsa bu carpan 1.0 kalir. */
    if (gw && gh){
        float ga = (float)gw / (float)gh, da = (float)aw / (float)ah;
        g_aspfix = ga / da;
        const char *ov = ENVC("ASPFIX");
        if (ov && *ov) g_aspfix = (float)atof(ov);
    }
    *dw = aw; *dh = ah;
    { static int hf = -1;
      if (hf < 0){ const char *e = ENVC("HUDFIX"); hf = !(e && *e && *e=='0'); }
      void metal_set_ui_aspect(float); metal_set_ui_aspect(hf && g_aspfix < 1.f ? g_aspfix : 1.f); }
    fprintf(stderr,"com: cihaz cozunurlugu %ux%u -> %ux%u (aspfix=%.4f)\n",
            gw, gh, aw, ah, (double)g_aspfix);
}
/* Konum matrisinin sabit yazmaci golgelendiriciye gore degisiyor; bir kez
 * cozup sakla (-2 = henuz bakilmadi). */
static int8_t g_pos_cx[MAXVS], g_pos_cw[MAXVS]; static uint8_t g_pos_seen[MAXVS];
static unsigned long g_asp_np[MAXVS], g_asp_no[MAXVS]; static float g_asp_cw3[MAXVS];
static const VSConst *aspect_apply(const VSConst *vsk, const VSProg *prog){
    if (g_aspfix == 1.f || !prog) return vsk;
    unsigned vs = g_cur_vs;
    if (vs >= MAXVS) return vsk;
    if (!g_pos_seen[vs]){
        int w = -1; int x = vs_pos_creg(prog, &w);
        g_pos_cx[vs] = (int8_t)((x >= 0 && x <= 120) ? x : -1);
        g_pos_cw[vs] = (int8_t)((w >= 0 && w <= 123) ? w : -1);
        g_pos_seen[vs] = 1;
    }
    int cx = g_pos_cx[vs], cw = g_pos_cw[vs];
    if (cx < 0 || cw < 0){ g_asp_ortho++; return vsk; }
    /* Perspektif mi?  oPos.w = dot(v, c[cw]).  Ortografik/ekran matrisinde
     * bu satir (0,0,0,1): w sabit 1.  Perspektifte ise xyz kismi gorus
     * ekseni olur ve SIFIR DEGILDIR (w bileseni kamera uzakligidir --
     * olculdu: 5.79 / 222.0 / -86.5, yani "w==0" testi yanlisti). */
    g_asp_cw3[vs] = vsk->f[cw][3];
    { float ax = fabsf(vsk->f[cw][0]) + fabsf(vsk->f[cw][1]) + fabsf(vsk->f[cw][2]);
      if (ax < 1e-6f){ g_asp_ortho++; g_asp_no[vs]++;
        /* HUDFIX=1: arayuzu de merkeze gore olcekle, boylece daireler
         * dairesel kalir.  DIKKAT: tam ekran son-islem dortgenleri de
         * ortografik; onlar kucululurse kenarlarda bant olusur. */
        { static int hf = -1;
          if (hf < 0){ const char *e = ENVC("HUDFIX"); hf = !(e && *e && *e=='0'); }
          if (hf){ static VSConst ht; memcpy(&ht, vsk, sizeof ht);
                   for (int k = 0; k < 4; k++) ht.f[cx][k] *= g_aspfix;
                   return &ht; } }
        if (ENVC("ASPLOG")){ static int no = 0;
            if (no < 6){ no++;
                fprintf(stderr,"  [asp-orto] vs=%u cx=%d cw=%d  c%d=(%.4f %.4f %.4f %.4f)"
                        "  c%d=(%.4f %.4f %.4f %.4f)\n", vs, cx, cw,
                        cx, vsk->f[cx][0],vsk->f[cx][1],vsk->f[cx][2],vsk->f[cx][3],
                        cw, vsk->f[cw][0],vsk->f[cw][1],vsk->f[cw][2],vsk->f[cw][3]); } }
        return vsk; } }
    g_asp_persp++; g_asp_np[vs]++;
    static VSConst tmp;
    memcpy(&tmp, vsk, sizeof tmp);
    for (int k = 0; k < 4; k++) tmp.f[cx][k] *= g_aspfix;
    if (ENVC("ASPLOG")){ static int n = 0;
        if (n < 6){ n++;
            fprintf(stderr,"  [asp] vs=%u c%d=(%.4f %.4f %.4f %.4f) -> (%.4f %.4f %.4f %.4f)"
                    "  c%d=(%.3f %.3f %.3f %.3f)\n", g_cur_vs, cx,
                    vsk->f[cx][0],vsk->f[cx][1],vsk->f[cx][2],vsk->f[cx][3],
                    tmp.f[cx][0],tmp.f[cx][1],tmp.f[cx][2],tmp.f[cx][3],
                    cw, vsk->f[cw][0],vsk->f[cw][1],vsk->f[cw][2],vsk->f[cw][3]); } }
    return &tmp;
}

static const VSConst *fixmat_apply(const VSConst *vsk){
    if (!(vsk->f[3][3] == 0.f && vsk->f[3][0] == 0.f &&
          vsk->f[0][0] == 0.f && vsk->f[1][1] == 0.f)) return vsk;
    if (g_fixmat < 0){ const char *e6 = ENVC("FIXMAT");
                       g_fixmat = (e6 && *e6 && *e6 != '0') ? 1 : 0; }
    if (!g_fixmat) return vsk;
    static VSConst tmp;
    static int mode = -1;
    if (mode < 0){ const char *e4 = ENVC("FIXMAT_MODE"); mode = e4 ? atoi(e4) : 0; }
    memcpy(&tmp, vsk, sizeof tmp);
    if (mode == 4){
        static float t4 = -1e30f;
        if (t4 == -1e30f){ const char *e9 = ENVC("FIXMAT_YOFF");
                           t4 = e9 ? (float)atof(e9) : 0.f; }
        memset(tmp.f, 0, sizeof tmp.f);
        tmp.f[0][0]=1.f; tmp.f[1][1]=1.f; tmp.f[1][3]=t4;
        tmp.f[2][2]=1.f; tmp.f[3][3]=1.f;
        return &tmp;
    }
    if (mode == 3){
        /* Ham konumlar zaten kirpma uzayinda; oyunun yazmak istedigi
         * matris birim matris. */
        memset(tmp.f, 0, sizeof tmp.f);
        tmp.f[0][0]=1.f; tmp.f[1][1]=1.f; tmp.f[2][2]=1.f; tmp.f[3][3]=1.f;
        return &tmp;
    }
    if (mode == 1 || mode == 2){
        float ys = (mode == 1) ? -1.f : 1.f;
        float yb = (mode == 1) ?  1.f : -1.f;
        tmp.f[0][0]=0.75f; tmp.f[0][1]=0; tmp.f[0][2]=0; tmp.f[0][3]=0;
        tmp.f[1][0]=0; tmp.f[1][1]=ys; tmp.f[1][2]=0; tmp.f[1][3]=yb;
        tmp.f[2][0]=0; tmp.f[2][1]=0; tmp.f[2][2]=1.f; tmp.f[2][3]=0;
        tmp.f[3][0]=0; tmp.f[3][1]=0; tmp.f[3][2]=0; tmp.f[3][3]=1.f;
        return &tmp;
    }
    tmp.f[0][0]=0.75f; tmp.f[0][1]=0; tmp.f[0][2]=0; tmp.f[0][3]=0;
    { static float yoff = -1e30f, ysc = 1.f;
      if (yoff == -1e30f){
          const char *e2 = ENVC("FIXMAT_YOFF");
          const char *e3 = ENVC("FIXMAT_YSCALE");
          yoff = e2 ? (float)atof(e2) : 0.f;
          ysc  = e3 ? (float)atof(e3) : 1.f; }
      tmp.f[1][0]=0; tmp.f[1][1]=ysc; tmp.f[1][2]=0; tmp.f[1][3]=-yoff; }
    tmp.f[2][0]=0; tmp.f[2][1]=0; tmp.f[2][2]=1.f; tmp.f[2][3]=0;
    tmp.f[3][0]=0; tmp.f[3][1]=0; tmp.f[3][2]=0; tmp.f[3][3]=1.f;
    return &tmp;
}

/* Asama basina "hangi koordinat kumesi + projeksiyon bolmesi".
 * D3DTSS_TEXCOORDINDEX = 11, D3DTSS_TEXTURETRANSFORMFLAGS = 24.
 * Kodlama: indeks + 16*kip  (kip 0 = bolme yok, 1 = /z, 2 = /w).
 * Izde 422 bin cagride asama 1 ve 2 KOORDINAT 0'i kullaniyor; biz asama
 * N'e koordinat N veriyorduk. */
/* Oyunun D3DSAMP_* durumunu Metal ornekleyicisine tasi.
 * D3DSAMP: 1=ADDRESSU 2=ADDRESSV 5=MAGFILTER 6=MINFILTER 7=MIPFILTER
 *          9=MAXMIPLEVEL 10=MAXANISOTROPY
 * Daha once tamami yok sayiliyor, her sey linear+mip-linear ciziliyordu. */
static void smp_push(void){
    void metal_set_samplers(const unsigned k[4]);
    unsigned k[4];
    for (int st = 0; st < 4; st++){
        unsigned mn = g_smp_raw[st][6], mg = g_smp_raw[st][5], mp = g_smp_raw[st][7];
        unsigned au = g_smp_raw[st][1], av = g_smp_raw[st][2], an = g_smp_raw[st][10];
        if (mn < 1u || mn > 3u) mn = 2u;          /* gecersiz/kurulmamis -> LINEAR */
        if (mg < 1u || mg > 3u) mg = 2u;
        if (mp > 2u) mp = 0u;
        if (au < 1u || au > 5u) au = 1u;          /* -> WRAP */
        if (av < 1u || av > 5u) av = 1u;
        if (an < 1u) an = 1u; if (an > 16u) an = 16u;
        unsigned al = 0; while ((1u << al) < an) al++;
        k[st] = (mn & 3u) | ((mg & 3u) << 2) | ((mp & 3u) << 4)
              | ((au & 7u) << 6) | ((av & 7u) << 9) | ((al & 7u) << 12);
    }
    metal_set_samplers(k);
}

static void texproj_push(void){
    void metal_set_texproj(const float[4]);
    float sel[4];
    for (int st = 0; st < 4; st++){
        unsigned ci  = g_tss_raw[st][11] & 0xFFFFu;
        unsigned ttf = g_tss_raw[st][24];
        if (ci > 3u) ci = (unsigned)st;
        int pm = 0;
        if (ttf & 0x100u){                 /* D3DTTFF_PROJECTED */
            /* NFSU2 bunu SAYI ALANI 0 ile yaziyor (olculdu: izde 28 cagrinin
             * hepsi tam "D3DTTFF_PROJECTED").  Kose gölgelendiricisi
             * koordinati 4 bilesenli uretiyor (oT1.xyz = dot(v0,c4..c6),
             * oT1.w = r0.x), o yuzden sayi 0 -> W ile bol.
             * Bolme yapilmayinca far dokusu koni olusturmak yerine yuzeye
             * yayiliyor: kullanicinin gordugu "far cizgileri" ve yerdeki
             * cizgili yansimalar (asama 3 = reflected_sampler) bu. */
            unsigned cnt = ttf & 7u;
            pm = (cnt == 3u) ? 1 : 2;      /* 3 -> xy/z, digerleri -> xy/w */
        }
        sel[st] = (float)(ci + 16u*(unsigned)pm);
    }
    metal_set_texproj(sel);
}

static void draw_up(uint32_t pt, uint32_t nv, uint32_t vp, uint32_t stride){
    uint64_t _t0 = now_ns();
    static TVert tv[4096];
    const VSConst *vsk = &g_vsk;
    if (g_fixmat < 0){ const char *e6 = ENVC("FIXMAT");
        g_fixmat = (e6 && *e6 && *e6 != '0') ? 1 : 0; }
    if (g_gpuvs < 0){ const char *e8 = ENVC("GPUVS");
        g_gpuvs = (e8 && *e8 && *e8 != '0') ? 1 : 0; }
    const VSProg *prog = (g_cur_vs && g_cur_vs < MAXVS) ? g_vsprog[g_cur_vs] : 0;
    vsk = fixmat_apply(vsk);
    vsk = aspect_apply(vsk, prog);

    DElem el[16]; int nel = 0;
    int p_off = -1, p_type = 2, c_off = -1, c_type = 4, t_off = -1, t_type = 1;
    uint32_t d = g_cur_decl;
    uint32_t ep = (d && mem_is_committed(d+8)) ? MR32(d+4) : 0;
    if (ep){
        for (int k = 0; k < 32 && nel < 16; k++){
            uint32_t e = ep + 8u*k;
            if (!mem_is_committed(e+7)) break;
            unsigned st=MR16(e), of=MR16(e+2), ty=MR8(e+4), us=MR8(e+6), ui=MR8(e+7);
            if (st == 0xFFu && ty == 17u) break;
            if (st != 0) continue;                       /* yalnizca akis 0 */
            /* DECLLOG=<ps>: yalnizca o piksel gölgelendiricisiyle cizen
             * bildirimleri, bir kez, TUM elemanlariyla bas. */
            { const char *dl = ENVC("DECLLOG");
              if (dl && atoi(dl) == g_cur_ps_id){
                static uint32_t seen[32]; static int sn = 0; int nw = 1;
                for (int q=0;q<sn;q++) if (seen[q]==d) nw = 0;
                if (nw && k == 0 && sn < 32){ seen[sn++] = d;
                    fprintf(stderr,"  [decl] ps=%d bildirim=0x%08x adim=%u vs=%u\n",
                            g_cur_ps_id, d, stride, g_cur_vs);
                    for (int q2 = 0; q2 < 24; q2++){
                        uint32_t e2 = ep + 8u*q2;
                        if (!mem_is_committed(e2+7)) break;
                        unsigned s2=MR16(e2), o2=MR16(e2+2), t2=MR8(e2+4),
                                 u2=MR8(e2+6), i2=MR8(e2+7);
                        if (s2 == 0xFFu && t2 == 17u) break;
                        int rg2 = prog ? vs_input_reg(prog,(int)u2,(int)i2) : -2;
                        fprintf(stderr,"       akis=%u ofset=%2u tur=%u kullanim=%u/%u -> v%d\n",
                                s2,o2,t2,u2,i2,rg2);
                    }
                }
              } }
            if (us == 0 && ui == 0){ p_off = (int)of; p_type = (int)ty; }
            else if (us == 10 && ui == 0){ c_off = (int)of; c_type = (int)ty; }
            else if (us == 5 && ui == 0){ t_off = (int)of; t_type = (int)ty; }
            if (prog){
                int rg = vs_input_reg(prog, (int)us, (int)ui);
                if (rg >= 0){ el[nel].off=(uint16_t)of; el[nel].type=(uint8_t)ty;
                              el[nel].reg=(int8_t)rg; nel++; }
            }
        }
    }
    /* bildirim yoksa NFSU2'nin standart duzeni: pos@0 renk@12 uv@16 */
    if (p_off < 0){ p_off = 0;  p_type = 2; }
    if (c_off < 0){ c_off = 12; c_type = 4; }
    if (t_off < 0){ t_off = 16; t_type = 1; }

    /* ---------------- GPU kose gölgelendiricisi (GPUVS) ----------------
     * Olculdu: calisma suresinin ~%45'i CPU'daki vs_run yorumlayicisinda,
     * tavan ~3.5 M kose/sn.  Oyunun kendi bytecode'u MSL'ye cevrilip
     * GPU'da kosturulur; koseler HAM gonderilir, okuma yerlesimi D3D
     * bildiriminden alinip gölgelendiriciye gomulur.
     * Cevrilemeyen gölgelendirici (akis denetimi vb.) sessizce yanlis
     * calismaz: vs_to_msl 0 doner ve kanitlanmis CPU yoluna dusulur. */
    if (prog && nel && stride && stride <= 256u && g_gpuvs){
        int gid = gpuvs_id(prog, g_cur_vs, g_cur_decl, el, nel);
        if (gid > 0){
            int  metal_draw_raw(const void*, unsigned, unsigned, unsigned,
                                unsigned, int, const float*, unsigned);
            void metal_set_stage_tex(unsigned,unsigned,unsigned);
            void metal_set_ps(int, const float *);
            metal_set_stage_tex(g_tex_stage[1], g_tex_stage[2], g_tex_stage[3]);
            metal_set_ps(g_cur_ps_id, &g_psc[0][0]);
            texproj_push(); smp_push();
            if (metal_draw_raw(g_mem + vp, nv, stride, pt, g_cur_tex, gid,
                               &vsk->f[0][0], (unsigned)g_gvs_nk[gid])){
                g_vs_draws++; g_gpuvs_draws++; g_v_xform += nv;
                g_t_draw_ns += now_ns() - _t0;
                return;
            }
            /* Kuyruga alinamadi (arena tasmasi): CPU yolu devam etsin --
             * sessizce dusurmek geometriyi yok ediyordu. */
            g_gpuvs_drop++;
        }
    }

    for (uint32_t i = 0; i < nv; i++){
        uint32_t o = vp + i*stride;
        TVert *out = &tv[i];
        if (prog && nel){
            /* TANI (FIXMAT=1): oyunun WVP matrisi diag(0,0,1,1) geliyor --
             * X/Y olcegi sifir.  Kok neden aranirken boru hattinin geri
             * kalanini gormek icin makul bir ortografik matris koy. */
            /* Cizim basina matris saglikli mi?  Bazi cizimler dogru
             * matrisle geliyorsa on yuz iki farkli uzay kullaniyor demektir. */
            { int degenerate = (vsk->f[3][3] == 0.f && vsk->f[3][0] == 0.f &&
                                vsk->f[0][0] == 0.f && vsk->f[1][1] == 0.f);
              if (degenerate){ g_mat_bad++;
                  static uint32_t seen[8]; static int ns=0;
                  int f4=0; for(int q=0;q<ns;q++) if(seen[q]==g_cur_vs) f4=1;
                  if(!f4 && ns<8 && ENVC("MATLOG")){ seen[ns++]=g_cur_vs;
                      fprintf(stderr,"  [matris-BOZUK] shader=%u doku=%u son yukleme sr=%u n=%u pd=%08x\n",
                              g_cur_vs, g_cur_tex,
                              g_scn?g_sc[(g_scn-1)&7].sr:0,
                              g_scn?g_sc[(g_scn-1)&7].n:0,
                              g_scn?g_sc[(g_scn-1)&7].pd:0); } }
              else {
                  g_mat_ok++;
                  static uint32_t seenok[8]; static int nsok=0;
                  int f5=0; for(int q=0;q<nsok;q++) if(seenok[q]==g_cur_vs) f5=1;
                  if(!f5 && nsok<8 && ENVC("MATLOG")){ seenok[nsok++]=g_cur_vs;
                      fprintf(stderr,"  [matris-GECERLI] shader=%u doku=%u pd=%08x\n",
                              g_cur_vs, g_cur_tex, g_scn?g_sc[(g_scn-1)&7].pd:0); }
                  static int shown = 0;
                  if (0){ shown++;
                      fprintf(stderr,"  [matris-GERCEK] cizim %llu:\n",
                              (unsigned long long)g_vs_draws);
                      for (int rw=0; rw<4; rw++)
                          fprintf(stderr,"      %9.4f %9.4f %9.4f %9.4f\n",
                                  vsk->f[rw][0], vsk->f[rw][1],
                                  vsk->f[rw][2], vsk->f[rw][3]);
                  }
              } }
            float v[16][4];
            for (int r2 = 0; r2 < 16; r2++){ v[r2][0]=v[r2][1]=v[r2][2]=0.f; v[r2][3]=1.f; }
            for (int k = 0; k < nel; k++)
                decl_fetch(o + el[k].off, el[k].type, v[el[k].reg]);
            VSOut so;
            /* NOVS=1: yalnizca OLCUM.  Vertex shader yorumlayicisini atlayip
             * darbogazin gercekten burasi olup olmadigini gorur. */
            static int novs = -1;
            if (novs < 0){ const char *e7 = ENVC("NOVS"); novs = (e7 && *e7 && *e7!='0'); }
            if (novs){ memset(&so, 0, sizeof so);
                       so.pos[0]=v[0][0]; so.pos[1]=v[0][1];
                       so.pos[2]=v[0][2]; so.pos[3]=1.f;
                       so.col[0][0]=so.col[0][1]=so.col[0][2]=so.col[0][3]=1.f; }
            else { uint64_t _v0 = now_ns();
                   vs_run(prog, vsk, (const float (*)[4])v, &so);
                   g_t_vs_ns += now_ns() - _v0; }
            out->x=so.pos[0]; out->y=so.pos[1]; out->z=so.pos[2]; out->w=so.pos[3];
            out->r=so.col[0][0]; out->g=so.col[0][1];
            out->b=so.col[0][2]; out->a=so.col[0][3];
            out->r1=so.col[1][0]; out->g1=so.col[1][1];
            out->b1=so.col[1][2]; out->a1=so.col[1][3];
            /* GPU yolunda secimi kose golgelendiricisi yapiyor; CPU yolu da
             * AYNI permutasyonu uygulamali, yoksa iki yol ayrisir. */
            for (int ts = 0; ts < 4; ts++){
                unsigned ci = g_tss_raw[ts][11] & 0xFFFFu;
                if (ci > 3u) ci = (unsigned)ts;
                for (int tc = 0; tc < 4; tc++) out->t[ts][tc] = so.tex[ci][tc];
            }
        } else {
            float pos[4], col[4], uv[4];
            decl_fetch(o + (unsigned)p_off, (unsigned)p_type, pos);
            decl_fetch(o + (unsigned)c_off, (unsigned)c_type, col);
            decl_fetch(o + (unsigned)t_off, (unsigned)t_type, uv);
            out->x=pos[0]; out->y=pos[1]; out->z=pos[2];
            out->w=(p_type==3)?pos[3]:1.f;
            out->r=col[0]; out->g=col[1]; out->b=col[2]; out->a=col[3];
            out->r1=0.f; out->g1=0.f; out->b1=0.f; out->a1=0.f;
            for (int ts = 0; ts < 4; ts++){
                out->t[ts][0]=uv[0]; out->t[ts][1]=uv[1];
                out->t[ts][2]=0.f;   out->t[ts][3]=1.f;
            }
        }
    }
    if (prog && nel){
        g_vs_draws++;
        /* Shader'li cizimlerin HAM konum araligi: UI'nin hangi uzayda
         * oldugunu ve matrisin ne yapmasi gerektigini gosterir. */
        for (uint32_t i3 = 0; i3 < nv; i3++){
            uint32_t o3 = vp + i3*stride;
            if (!mem_is_committed(o3 + (unsigned)p_off + 11)) break;
            float rx = MRF32(o3 + (unsigned)p_off);
            float ry = MRF32(o3 + (unsigned)p_off + 4);
            float rz = MRF32(o3 + (unsigned)p_off + 8);
            if (rx < g_rmin[0]) g_rmin[0] = rx;  if (rx > g_rmax[0]) g_rmax[0] = rx;
            if (ry < g_rmin[1]) g_rmin[1] = ry;  if (ry > g_rmax[1]) g_rmax[1] = ry;
            if (rz < g_rmin[2]) g_rmin[2] = rz;  if (rz > g_rmax[2]) g_rmax[2] = rz;
        }
    } else g_ff_draws++;
    /* Donusturulmus koseler ekranda mi?  Kirpma uzayindan NDC'ye gecip say. */
    { for (uint32_t i2 = 0; i2 < nv; i2++){
        float w2 = tv[i2].w;
        if (w2 <= 0.f){ g_v_badw++; continue; }
        float nx = tv[i2].x / w2, ny = tv[i2].y / w2;
        if (nx >= -1.f && nx <= 1.f && ny >= -1.f && ny <= 1.f) g_v_in++;
        else g_v_out++;
      } }
    { void metal_set_stage_tex(unsigned,unsigned,unsigned);
      void metal_set_ps(int, const float *);
      metal_set_stage_tex(g_tex_stage[1], g_tex_stage[2], g_tex_stage[3]);
      metal_set_ps(g_cur_ps_id, &g_psc[0][0]);
      texproj_push(); smp_push();
      if (ENVC("PSC0")){ static int n5=0;
          if (n5<6 && g_tex_stage[1]){ n5++;
              fprintf(stderr,"  [ps] ps=%d c0=(%.3f %.3f %.3f %.3f) doku=%u/%u/%u/%u\n",
                      g_cur_ps_id,
                      (double)g_psc[0][0],(double)g_psc[0][1],
                      (double)g_psc[0][2],(double)g_psc[0][3],
                      g_tex_stage[0], g_tex_stage[1],
                      g_tex_stage[2], g_tex_stage[3]); } } }
    { int nstage=0; for(int q=0;q<4;q++) if (g_tex_stage[q]) nstage++;
      if (nstage>0 && nstage<=4) g_stage_hist[nstage-1]++;
      static int shown=0;
      if (ENVC("STAGEDUMP") && nstage>=2 && shown<8){ shown++;
          fprintf(stderr,"  [asama] cizim: doku0=%u doku1=%u doku2=%u doku3=%u  kose=%u\n",
                  g_tex_stage[0],g_tex_stage[1],g_tex_stage[2],g_tex_stage[3], nv); } }
    g_t_draw_ns += now_ns() - _t0; g_v_xform += nv;
    if (prog && g_cur_vs < 64u) g_v_per_vs[g_cur_vs] += nv;
    if (dd_on()){
        static int nd = 0;
        if (nd++ < 6){
            fprintf(stderr,"   >>> cizim tip=%u kose=%u adim=%u doku=%u shader=%u eleman=%d\n",
                    pt, nv, stride, g_cur_tex, g_cur_vs, nel);
            if (prog) vs_disasm(prog, stderr);
            for (int k=0;k<nel;k++)
                fprintf(stderr,"      eleman: ofset=%u tip=%u -> v%d\n",
                        el[k].off, el[k].type, el[k].reg);
            for (uint32_t i=0;i<nv && i<3;i++){
                uint32_t o = vp + i*stride;
                if (prog && nel){
                    float v[16][4];
                    for (int r2=0;r2<16;r2++){ v[r2][0]=v[r2][1]=v[r2][2]=0.f; v[r2][3]=1.f; }
                    for (int k=0;k<nel;k++) decl_fetch(o+el[k].off, el[k].type, v[el[k].reg]);
                    for (int k=0;k<nel;k++)
                        fprintf(stderr,"        v%d = %g %g %g %g\n", el[k].reg,
                                v[el[k].reg][0],v[el[k].reg][1],v[el[k].reg][2],v[el[k].reg][3]);
                }
                fprintf(stderr,"        -> pos=(%g %g %g %g) renk=(%.3g %.3g %.3g %.3g) uv=(%g %g)\n",
                        tv[i].x,tv[i].y,tv[i].z,tv[i].w,
                        tv[i].r,tv[i].g,tv[i].b,tv[i].a, tv[i].t[0][0],tv[i].t[0][1]);
            }
            fprintf(stderr,"      son sabit yuklemeleri (yeniden eskiye):\n");
            for (int q=1; q<=4 && q<=(int)g_scn; q++){
                unsigned ix = (g_scn - q) & 7;
                fprintf(stderr,"        sr=%-3u n=%-3u pd=%08x", g_sc[ix].sr, g_sc[ix].n, g_sc[ix].pd);
                if (g_sc[ix].pd && mem_is_committed(g_sc[ix].pd+63))
                    fprintf(stderr,"  ilk4=[%.4g %.4g %.4g %.4g][%.4g %.4g %.4g %.4g]",
                        MRF32(g_sc[ix].pd), MRF32(g_sc[ix].pd+4),
                        MRF32(g_sc[ix].pd+8), MRF32(g_sc[ix].pd+12),
                        MRF32(g_sc[ix].pd+16), MRF32(g_sc[ix].pd+20),
                        MRF32(g_sc[ix].pd+24), MRF32(g_sc[ix].pd+28));
                fprintf(stderr,"\n");
            }
            /* kullanilan c0..c7 */
            for (int q=0;q<8;q++)
                fprintf(stderr,"        c%d = %g %g %g %g\n", q,
                        g_vsk.f[q][0],g_vsk.f[q][1],g_vsk.f[q][2],g_vsk.f[q][3]);
        }
    }
    metal_draw_tv(tv, nv, pt, g_cur_tex);
}

/* ---- D3DX HLE Asama 2: cagri kaydi ve etkisizlik denetimi (dxrec.h) ----
 * D3DX'in durum atamasi (sub_00669296) kare basina ~36 bin cihaz cagrisi
 * uretiyor ve %96'si cihazdaki degeri yeniden yaziyor.  Burada bir
 * cagrinin SIMDI yapilsa isleyicinin yazacagi degerlerin mevcut durumla
 * ayni olup olmadigi, isleyicinin kendi mantigi tekrarlanarak denetlenir.
 * Yalnizca asagidaki 8 metot desteklenir; baska her cihaz cagrisi kaydi
 * gecersiz kilar (atlama yapilmaz).                                      */
#include "dxrec.h"
#define DXREC_DEPTH 8
static DxRec *g_dxrec[DXREC_DEPTH];
static int    g_dxrec_depth = 0;
const char *dxrec_mid_name(int mid){ return (mid > 0 && mid < MID__count) ? g_midname[mid] : "?"; }
int dxrec_sb_recording(void){ return g_sb_rec != 0 || g_sb_replay != 0; }
void dxrec_begin(DxRec *r, int kind){
    int in_state = 0, same = 0;
    for (int i = 0; i < g_dxrec_depth && i < DXREC_DEPTH; i++){
        if (g_dxrec[i]->kind == DXREC_STATE) in_state = 1;
        if (g_dxrec[i]->kind == kind) same = 1;
    }
    if (in_state || (same && kind == DXREC_PASS))
        for (int i = 0; i < g_dxrec_depth && i < DXREC_DEPTH; i++) g_dxrec[i]->ok = 0;
    r->kind = (uint8_t)kind; r->ok = 1; r->ncall = 0; r->ndata = 0; r->allred = 1;
    if (g_dxrec_depth < DXREC_DEPTH) g_dxrec[g_dxrec_depth] = r; else r->ok = 0;
    g_dxrec_depth++;
}
void dxrec_append(const DxRec *src){
    int lim = g_dxrec_depth < DXREC_DEPTH ? g_dxrec_depth : DXREC_DEPTH;
    for (int i = 0; i < lim; i++){
        DxRec *r = g_dxrec[i];
        if (!r->ok) continue;
        if (!src->ok || r->ncall + src->ncall > r->maxcall
            || r->ndata + src->ndata > r->maxdata){ r->ok = 0; continue; }
        for (int k = 0; k < src->ncall; k++){
            DxCall *d = &r->call[r->ncall++];
            *d = src->call[k];
            d->doff = (uint16_t)(src->call[k].doff + r->ndata);
        }
        memcpy(r->data + r->ndata, src->data, src->ndata);
        r->ndata = (uint16_t)(r->ndata + src->ndata);
    }
}
void dxrec_end(void){ if (g_dxrec_depth > 0) g_dxrec_depth--; }

/* Veri isaretcisi olan sabit metotlari icin kopyalanacak/karsilastirilacak
 * bayt sayisi (isleyicinin sinirlariyla ayni). */
static uint32_t dxrec_datalen(int mid, const uint32_t *a){
    if (MID(SetVertexShaderConstantF)){ uint32_t n = a[2]; if (a[0] >= 256u) return 0;
        if (a[0] + n > 256u) n = 256u - a[0]; return n*16u; }
    if (MID(SetPixelShaderConstantF)){ uint32_t n = a[2]; if (a[0] >= 32u) return 0;
        if (a[0] + n > 32u) n = 32u - a[0]; return n*16u; }
    return 0;
}
/* a[]: this'ten sonraki argumanlar; d: sabit verisi (dlen bayt). */
static int dxrec_call_redundant(int mid, const uint32_t *a, const uint8_t *d, uint32_t dlen){
    if (MID(SetRenderState))
        return a[0] >= 256u || (g_rs[a[0]] == a[1] && !g_rs_dirty);
    if (MID(SetSamplerState))
        return !(a[0] < 16u && a[1] < 20u) || g_smp_raw[a[0]][a[1]] == a[2];
    if (MID(SetTextureStageState))
        return !(a[0] < 8u && a[1] < 40u) || g_tss_raw[a[0]][a[1]] == a[2];
    if (MID(SetTexture)){
        uint32_t stg = a[0], to = a[1];
        if (stg >= 8u) return 1;
        uint32_t der = (to && mem_is_committed(to+28)) ? MR32(to+28) : 0;
        if (g_raw_tex[stg] != to) return 0;
        if (stg < 4u && g_tex_stage[stg] != der) return 0;
        if (stg == 0 && g_cur_tex != der) return 0;
        return 1;
    }
    if (MID(SetVertexShader)){
        uint32_t h = a[0];
        unsigned der = (h && mem_is_committed(h+15)) ? MR32(h+12) : 0;
        if (der >= MAXVS) der = 0;
        return g_raw_vs == h && g_cur_vs == der;
    }
    if (MID(SetPixelShader)){
        uint32_t h = a[0];
        int der = (h && mem_is_committed(h+15)) ? (int)MR32(h+12) : 0;
        return g_raw_ps == h && g_cur_ps_id == der;
    }
    if (MID(SetVertexShaderConstantF))
        return dlen == 0 || !memcmp(g_vsk.f[a[0]], d, dlen);
    if (MID(SetPixelShaderConstantF))
        return dlen == 0 || !memcmp(g_psc[a[0]], d, dlen);
    return 0;
}
static int dxrec_supported(int mid){
    return MID(SetRenderState) || MID(SetSamplerState) || MID(SetTextureStageState)
        || MID(SetTexture) || MID(SetVertexShader) || MID(SetPixelShader)
        || MID(SetVertexShaderConstantF) || MID(SetPixelShaderConstantF);
}
/* com_call'dan, isleyiciden ONCE: acik her kayda ekle. */
static void dxrec_note(CPU *c, int iface, int mid, int nargs){
    /* Bizde etkisiz olan sayac cagrilari (doku AddRef/Release) kaydi bozmaz. */
    if ((MID(AddRef) || MID(Release)) && iface != IF_STATEBLOCK
        && iface != IF_DS && iface != IF_DSBUF) return;
    int okc = iface == IF_DEV && dxrec_supported(mid) && !dxrec_sb_recording()
              && nargs >= 2;
    uint32_t a[3] = { 0, 0, 0 };
    for (int i = 0; i < 3 && i + 1 < nargs; i++) a[i] = A(c, i+1);
    uint32_t dlen = okc ? dxrec_datalen(mid, a) : 0;
    const uint8_t *dp = NULL;
    if (dlen){
        uint32_t pd = a[1];
        if (!pd || !mem_is_committed(pd) || !mem_is_committed(pd+dlen-1u)) okc = 0;
        else dp = g_mem + pd;
    }
    int red = okc && dxrec_call_redundant(mid, a, dp, dlen);
    int lim = g_dxrec_depth < DXREC_DEPTH ? g_dxrec_depth : DXREC_DEPTH;
    for (int i = 0; i < lim; i++){
        DxRec *r = g_dxrec[i];
        if (!r->ok) continue;
        if (!okc || r->ncall >= r->maxcall || r->ndata + dlen > r->maxdata){ r->ok = 0; continue; }
        DxCall *k = &r->call[r->ncall++];
        k->mid = (int16_t)mid; k->a[0] = a[0]; k->a[1] = a[1]; k->a[2] = a[2];
        k->doff = r->ndata; k->dlen = (uint16_t)dlen; k->red = (uint8_t)red;
        if (dlen){ memcpy(r->data + r->ndata, dp, dlen); r->ndata += dlen; }
        if (!red) r->allred = 0;
    }
}
int dxrec_redundant(const DxRec *r){
    if (!r->ok || dxrec_sb_recording()) return 0;
    for (int i = 0; i < r->ncall; i++){
        const DxCall *k = &r->call[i];
        if (!dxrec_call_redundant(k->mid, k->a, r->data + k->doff, k->dlen)) return 0;
    }
    return 1;
}

/* COMSTAT=<kare>: IDirect3DDevice9 metot sayilari, D3DX BeginPass /
 * CommitChanges ICINDEN mi DISINDAN mi geldigi ve GEREKSIZ (cihazdaki son
 * degerle ayni) orani.  D3DX_HLE.md Asama 2 icin; D3DXSTAT ile birlikte
 * kullanilmali (BeginPass derinligini o sarmalayici tutuyor). */
extern int g_dx_in_pass;
static int g_cs_on = -1; static long g_cs_from = 0, g_cs_f0 = -1;
static uint64_t cs_n[MID__count][2], cs_red[MID__count][2];
typedef struct { uint32_t k0, k1; uint64_t v; } CsSlot;
#define CS_CAP (1u<<15)
static CsSlot *cs_tab;
static void comstat(CPU *c, int mid, int nargs){
    long metal_frames(void);
    long f = metal_frames();
    if (f < g_cs_from || mid <= 0) return;
    if (g_cs_f0 < 0) g_cs_f0 = f;
    if (!cs_tab) cs_tab = calloc(CS_CAP, sizeof *cs_tab);
    int in = g_dx_in_pass > 0;
    cs_n[mid][in]++;
    const char *nm = g_midname[mid];
    if (nm[0] != 'S' || nm[1] != 'e' || nm[2] != 't') return;
    int nkey = (MID(SetSamplerState) || MID(SetTextureStageState)) ? 2 : (nargs >= 3 ? 1 : 0);
    uint32_t k1 = 0; for (int i = 0; i < nkey; i++) k1 = k1*0x10001u + A(c, 1+i);
    uint64_t v = 0xcbf29ce484222325ull;
    for (int i = 1 + nkey; i < nargs; i++){ v ^= A(c,i); v *= 0x100000001b3ull; }
    uint32_t dp = 0, db = 0;
    if (MID(SetVertexShaderConstantF) || MID(SetPixelShaderConstantF)
        || MID(SetVertexShaderConstantI)){ dp = A(c,2); db = A(c,3)*16u; }
    else if (MID(SetTransform) || MID(SetMaterial)){ dp = A(c, nargs-1); db = 64u; }
    if (dp && db <= 4096u && mem_is_committed(dp) && mem_is_committed(dp+db-1u))
        for (uint32_t i = 0; i < db; i++){ v ^= MR8(dp+i); v *= 0x100000001b3ull; }
    uint32_t h = ((uint32_t)mid * 0x9E3779B1u) ^ (k1 * 0x85EBCA77u);
    for (uint32_t j = 0; j < CS_CAP; j++){
        CsSlot *t = &cs_tab[(h + j) & (CS_CAP-1)];
        if (t->k0 == 0){ t->k0 = (uint32_t)mid; t->k1 = k1; t->v = v; return; }
        if (t->k0 == (uint32_t)mid && t->k1 == k1){
            if (t->v == v) cs_red[mid][in]++;
            t->v = v; return; }
    }
}
void comstat_report(void){
    if (g_cs_f0 < 0) return;
    long metal_frames(void);
    double fr = (double)(metal_frames() - g_cs_f0); if (fr < 1) fr = 1;
    fprintf(stderr, "\n=== COMSTAT (%.0f kare): IDirect3DDevice9 cagri/kare ===\n", fr);
    fprintf(stderr, "  %-28s %10s %10s %9s %10s %9s\n", "metot", "BeginPass", "gereksiz",
            "", "disarida", "gereksiz");
    for (int pass = 0; pass < 40; pass++){
        int best = -1; uint64_t bv = 0;
        for (int i = 1; i < MID__count; i++){
            uint64_t t = cs_n[i][0] + cs_n[i][1];
            if (t > bv){ bv = t; best = i; } }
        if (best < 0 || bv < (uint64_t)fr) break;
        int i = best;
        fprintf(stderr, "  %-28s %10.1f %9.1f%% %9s %10.1f %8.1f%%\n", g_midname[i],
            cs_n[i][1]/fr, cs_n[i][1] ? 100.0*cs_red[i][1]/cs_n[i][1] : 0.0, "",
            cs_n[i][0]/fr, cs_n[i][0] ? 100.0*cs_red[i][0]/cs_n[i][0] : 0.0);
        cs_n[i][0] = cs_n[i][1] = 0;
    }
}

int com_call(CPU *c, uint32_t magic){
    uint32_t iface=(magic>>16)&0x1FFFu, m=magic&0xFFFFu;
    g_com_calls++;
    if (!g_sb_replay) POP32(c->eip);
    if (iface==0 || iface>=NEL(g_if) || m>=g_if[iface].n){
        fprintf(stderr,"  [COM] gecersiz arayuz/metot %u:%u\n", iface, m);
        c->eax=0x80004001u; return 1;
    }
    if (iface < MSTAT_IF && m < MSTAT_M) g_mstat[iface][m]++;
    const Method *M=&g_if[iface].m[m];
    int nargs=M->nargs; uint32_t r=0; int impl=1;
    const char *nm=M->name;
    int mid;
    if (iface < MIDIF && m < MIDM){
        mid = g_midc[iface][m];
        if (!mid) mid = g_midc[iface][m] = (int16_t)mid_resolve(nm);
    } else mid = mid_resolve(nm);

    if (g_dxrec_depth) dxrec_note(c, (int)iface, mid, nargs);
    /* ZSEQ=<kare>: o karede hedef / Z yuzeyi / Clear / Z durumu sirasi (tani) */
    if (iface==IF_DEV && ENVC("ZSEQ") && g_present_no == (uint32_t)atoi(ENVC("ZSEQ"))){
        static unsigned nd = 0;
        if (nm[0]=='D' && nm[1]=='r'){ nd++; }
        else if (MID(SetRenderTarget) || MID(SetDepthStencilSurface) || MID(Clear)
                 || (MID(SetRenderState) && (A(c,1)==7||A(c,1)==14||A(c,1)==23))
                 || MID(BeginScene) || MID(EndScene) || MID(StretchRect)){
            if (nd){ fprintf(stderr, "  [zseq]   .. %u cizim\n", nd); nd = 0; }
            uint32_t s2 = A(c,2);
            fprintf(stderr, "  [zseq] %s %x %x %x %x  (rt=%u", nm, A(c,1), A(c,2), A(c,3), A(c,4), g_cur_rt);
            if ((MID(SetRenderTarget)||MID(SetDepthStencilSurface)) && s2 && mem_is_committed(s2+47))
                fprintf(stderr, " yuzey tid=%u %ux%u tur=%u", MR32(s2+28), MR32(s2+4), MR32(s2+8), MR32(s2+40));
            if (MID(SetDepthStencilSurface) && A(c,1) && mem_is_committed(A(c,1)+47))
                fprintf(stderr, " zyuzey %ux%u tur=%u", MR32(A(c,1)+4), MR32(A(c,1)+8), MR32(A(c,1)+40));
            fprintf(stderr, ")\n");
        }
    }
    if (g_cs_on && iface==IF_DEV){
        if (g_cs_on < 0){ const char *e = getenv("COMSTAT"); g_cs_on = e != NULL; g_cs_from = e ? atol(e) : 0; }
        if (g_cs_on) comstat(c, mid, nargs);
    }

    /* --- KUP DOKU: ayni vtable yuvasi, BIR FAZLA arguman ---------------
     * IDirect3DCubeTexture9 metotlari 2B karsiliklariyla ayni yuvalarda
     * ama basta bir FaceType parametresi tasiyor.  Islev govdeleri bunu
     * zaten ayirt ediyordu; ama YIGIN TEMIZLIGI (nargs) duzeltilmemisti,
     * yani her cagri 4 bayt sizdiriyordu.  Yansima acikken oyun 6 kup
     * yuzu icin GetCubeMapSurface cagirinca tam 24 bayt kayiyor: cagiran
     * fonksiyonun epilogu yanlis yerden pop yapiyor, esi ve donus adresi
     * bozuluyor ve oyun ilk karede cokuyordu (g_CarEnvironmentMapEnable=1
     * ile birebir yeniden uretildi). */
    /* Once UCUZ olan metot kimligine bak: is_cube_tex() konuk BELLEGI
     * okuyor ve eskiden HER doku cagrisinda calisiyordu -- kosuda
     * IDirect3DTexture9 AddRef+Release tek basina 96.8 milyon cagri. */
    if (iface==IF_TEX &&
        (MID(GetSurfaceLevel) || MID(GetCubeMapSurface) ||
         MID(LockRect) || MID(UnlockRect) ||
         MID(AddDirtyRect)) && is_cube_tex(A(c,0)))
        nargs++;

    /* --- durum blogu KAYDI ---------------------------------------------
     * D3D9'da kayit sirasinda cihaz durumu DEGISMEZ; cagri yalnizca
     * bloga yazilir. */
    if (g_sb_rec && iface==IF_DEV && !g_sb_replay && sb_recordable(mid)){
        SBlock *B = &g_sb[g_sb_rec];
        if (B->n < MAXSBREC){
            SBRec *R = &B->r[B->n++];
            R->magic = magic; R->nargs = nargs;
            for (int i2 = 0; i2 < 8; i2++) R->a[i2] = (i2 < nargs) ? A(c,i2) : 0u;
        } else g_sb_dropped++;
        c->eax = 0;
        c->esp += 4u*(uint32_t)nargs;
        return 1;
    }

    /* --- ortak: IUnknown --- */
    if      (MID(QueryInterface)){
        /* DirectSound8'de CreateSoundBuffer IDirectSoundBuffer dondurur;
         * IDirectSoundBuffer8'i almak icin oyun QueryInterface yapar.
         * E_NOINTERFACE donunce o sesler hic kullanilmiyor olabilir
         * (olculdu: 4 tampon yaratiliyor ama hicbirine yazilmiyor).
         * Ayni nesneyi donduruyoruz - vtable yuvalari zaten ortak. */
        uint32_t ppv = (nargs >= 3) ? A(c,2) : 0u;
        if ((iface==IF_DS || iface==IF_DSBUF) && ppv){
            mem_commit(ppv,4); MR32(ppv) = A(c,0); r = 0;
        } else r=0x80004002u;
    }
    else if (MID(AddRef))  r=1;
    else if (MID(Release)){
        /* Oyun ses tamponunu Stop ile DEGIL Release ile susturuyor
         * (olculdu: 3 dakikalik oturumda Stop=0, Release=7).  Durdurmayinca
         * menu muzigi yaris boyunca bayat icerigi donguye alip yaris
         * sesiyle cakisiyordu. */
        if (iface==IF_STATEBLOCK){
            /* Blok yuvasini geri ver: oyun oturum boyunca surekli blok
             * yaratip biraktigi icin havuz yoksa tukenir. */
            uint32_t self1=A(c,0);
            int bi = (self1 && mem_is_committed(self1+11)) ? (int)MR32(self1+8) : 0;
            if (bi > 0 && bi < MAXSB){ g_sb[bi].used = 0; g_sb[bi].n = 0;
                                       if (g_sb_rec == bi) g_sb_rec = 0;
                                       MR32(self1+8) = 0; }
        }
        if (iface==IF_DSBUF){
            uint32_t self0=A(c,0);
            if (self0 && mem_is_committed(self0+31) && MR32(self0+28)){
                void audio_stop(int);
                audio_stop((int)MR32(self0+28));
                MR32(self0+12)=0;
            }
        }
        r=0; }
    /* --- IDirect3D9 --- */
    else if (iface==IF_D3D9){
        if      (MID(GetAdapterCount))     r=1;
        /* Cozunurluk listesi TEK satirdi (1280x720), bu yuzden oyunun
         * ayar menusunde cozunurluk degistirilemiyordu.  Gercek bir liste
         * veriyoruz; secilen mod CreateDevice/Reset ile Metal'e gecer. */
        else if (MID(GetAdapterModeCount)) r=(uint32_t)NEL(g_modes);
        else if (MID(EnumAdapterModes)){
            uint32_t mode=A(c,3), pm=A(c,4);
            if (mode >= NEL(g_modes)) { r=0x8876086Cu; }
            else { uint32_t fmt=A(c,2); if(!fmt) fmt=22u;   /* istenen bicimi yansit */
                   if (mode == 0 && modemap_on()) modes_refresh();   /* menu metinleri */
                   const uint32_t *ml = g_tbl[mode];   /* secilebilirlik: oyunun tablosu */
                   if(pm){ mem_commit(pm,16);
                           MR32(pm)   = ml[0];
                           MR32(pm+4) = ml[1];
                           MR32(pm+8) = 60;
                           MR32(pm+12)= fmt; }
                   r=0; } }
        else if (MID(GetAdapterDisplayMode)){
            uint32_t pm=A(c,2);
            if(pm){ mem_commit(pm,16);
                    /* Masaustu modu: oyun bundan buyuk satirlari eliyor
                     * (1600x1200 bu yuzden hic secilemiyordu). */
                    const uint32_t *nl = mode_label(ROW_NATIVE);
                    MR32(pm)   = nl[0] > 1600u ? nl[0] : 1600u;
                    MR32(pm+4) = nl[1] > 1200u ? nl[1] : 1200u;
                    MR32(pm+8)=60; MR32(pm+12)=22; }
            r=0; }
        else if (MID(GetAdapterIdentifier)){
            uint32_t p=A(c,3);
            if(p){ mem_commit(p,1024); memset(g_mem+p,0,1024);
                   strcpy((char*)(g_mem+p),"NFSU2 Native Renderer");
                   strcpy((char*)(g_mem+p+512),"Apple Silicon / Metal"); }
            r=0; }
        else if (MID(GetDeviceCaps)){ uint32_t pc=A(c,3); if(pc) fill_caps(pc); r=0; }
        else if (MID(CreateDevice)){
            if(!g_dev) g_dev=com_new(IF_DEV,256);
            /* (this, Adapter, DeviceType, hFocusWindow, BehaviorFlags,
             *  pPresentationParameters, ppReturnedDeviceInterface)
             * pPresentationParameters HIC OKUNMUYORDU: oyun hangi
             * cozunurlugu isterse istesin biz 1280x720'de kaliyorduk.
             * Ayar menusunden cozunurluk degistirmenin etkisiz kalmasinin
             * sebebi buydu -- oyun cihazi yeni boyutla yeniden yaratiyor,
             * biz ayni cihazi geri verip boyutu yok sayiyorduk. */
            uint32_t ppp=A(c,5);
            if (ppp && mem_is_committed(ppp+7)){
                uint32_t w4=MR32(ppp), h4=MR32(ppp+4);
                uint32_t win4 = mem_is_committed(ppp+35) ? MR32(ppp+32) : 1u;
                fprintf(stderr,"com: CreateDevice istegi %ux%u (%s)\n",
                        w4, h4, win4 ? "pencereli" : "tam ekran");
                if (w4 > 8192u || h4 > 8192u){      /* oyunun dizi tasmasi */
                    fprintf(stderr,"com: gecersiz boyut yok sayildi, %ux%u korunuyor\n",
                            g_bb_w, g_bb_h);
                    w4 = h4 = 0;
                }
                /* NFSRES=<gen>x<yuk>: oyunun istedigi cozunurlugu gecersiz kil.
                 * Oyunun kendi ic tablosu 6 satir ve tamami 4:3; menuden
                 * 1080p secilemiyor (indeks 6+ diziyi tasiriyor). */
                { uint32_t dw = w4, dh = h4;
                  res_pick(w4, h4, &dw, &dh);
                  w4 = dw; h4 = dh; }
                if (w4 && h4){
                    g_bb_w = w4; g_bb_h = h4;
                    void metal_want_size(unsigned,unsigned);
                    metal_want_size(w4,h4);              /* pencere acilmadiysa */
                    if (metal_available()){ void metal_resize(unsigned,unsigned);
                                            metal_resize(w4,h4); }
                }
            }
            /* RESSCAN=1: oyunun KENDI cozunurluk tablosunu konuk bellekte bul.
             * Tablo EXE'de duz durmuyor, calisma aninda kuruluyor; onu
             * yamalamak 1080p'yi oyunun her yerine (en-boy orani, render
             * hedefleri, viewport) tasimanin tek temiz yolu. */
            if (ENVC("RESSCAN")){
                static const uint32_t want[12] = {640,480,800,600,1024,768,
                                                  1280,960,1280,1024,1600,1200};
                struct { uint32_t lo, hi; } rng[] = {
                    {0x00400000u,0x00940000u},          /* image */
                    {0x10000000u,0x18000000u},          /* heap  */
                };
                int found = 0;
                for (unsigned q = 0; q < sizeof rng/sizeof *rng && found < 8; q++)
                    for (uint32_t a = rng[q].lo; a + 48u < rng[q].hi && found < 8; a += 4u){
                        if (!mem_is_committed(a) || !mem_is_committed(a+47u)) continue;
                        int ok = 1;
                        for (int k = 0; k < 12; k++)
                            if (MR32(a + 4u*(uint32_t)k) != want[k]){ ok = 0; break; }
                        if (ok){ fprintf(stderr,"com: [resscan] tablo 0x%08x\n", a); found++; }
                    }
                /* Tam dizi bulunamazsa yalnizca 1600x1200 ikililerini bildir */
                if (!found){
                    int n2 = 0;
                    for (unsigned q = 0; q < sizeof rng/sizeof *rng && n2 < 12; q++)
                        for (uint32_t a = rng[q].lo; a + 8u < rng[q].hi && n2 < 12; a += 4u){
                            if (!mem_is_committed(a) || !mem_is_committed(a+7u)) continue;
                            if (MR32(a)==1600u && MR32(a+4u)==1200u){
                                fprintf(stderr,"com: [resscan] 1600x1200 @ 0x%08x  "
                                        "onceki: %u %u %u %u\n", a,
                                        mem_is_committed(a-16u)?MR32(a-16u):0,
                                        mem_is_committed(a-12u)?MR32(a-12u):0,
                                        mem_is_committed(a-8u)?MR32(a-8u):0,
                                        mem_is_committed(a-4u)?MR32(a-4u):0);
                                n2++; }
                        }
                    if (!n2) fprintf(stderr,"com: [resscan] tablo bulunamadi\n");
                }
            }
            uint32_t pp=A(c,6); if(pp) MR32(pp)=g_dev;
            fprintf(stderr,"com: IDirect3DDevice9 olusturuldu 0x%08x (%ux%u)\n",
                    g_dev, g_bb_w, g_bb_h);
            r=0; }
        else r=0;                       /* Check* -> D3D_OK */
    }
    /* --- IDirect3DDevice9 --- */
    else if (iface==IF_DEV){if (MID(SetRenderState)){
            uint32_t st=A(c,1), val=A(c,2);
            if (st < 256u){
                if (g_rs[st] != val){ g_rs[st] = val; g_rs_dirty = 1;
                    int seen=0; for(int k=0;k<g_rs_nval[st];k++) if(g_rs_vals[st][k]==val) seen=1;
                    if(!seen && g_rs_nval[st]<4) g_rs_vals[st][g_rs_nval[st]++]=val;
                }
                g_rs_hits[st]++;
            }
            if (g_rs_dirty){ if (metal_available()){ rs_apply(); g_rs_dirty = 0; } }
            else g_rs_skip++;
            r=0; }
        else if (MID(SetSamplerState)){
            uint32_t sm=A(c,1), ty=A(c,2), val=A(c,3);
            if (sm < 16u && ty < 20u) g_smp_raw[sm][ty] = val;
            if (ENVC("SMPLOG") && sm < 4u && ty < 20u){
                static uint32_t seen[4][20];
                if (seen[sm][ty] != val + 1u){ seen[sm][ty] = val + 1u;
                    const char *tn = ty==1?"ADDRESSU":ty==2?"ADDRESSV":ty==3?"ADDRESSW":
                                     ty==5?"MAGFILTER":ty==6?"MINFILTER":ty==7?"MIPFILTER":
                                     ty==8?"MIPMAPLODBIAS":ty==9?"MAXMIPLEVEL":
                                     ty==10?"MAXANISOTROPY":ty==11?"SRGB":"?";
                    fprintf(stderr,"  [smp] asama%u %s(%u) = %u\n", sm, tn, ty, val); } }
            r=0; }
        else if (MID(SetTextureStageState)){
            uint32_t stg=A(c,1), ty=A(c,2), val=A(c,3);
            if (stg < 8u && ty < 40u) g_tss_raw[stg][ty] = val;
            r=0; }
        else if (MID(SetTexture)){
            /* CUBELOG=1: kup dokunun gercekten baglanip baglanmadigini olc.
             * Metal tarafinda kup destegi YOK (duz 2B yaratiliyor); once
             * bu yolun kullanildigini dogrula. */
            if (ENVC("CUBELOG") && A(c,2) && is_cube_tex(A(c,2))){
                static unsigned n2 = 0;
                if (n2 < 8){ n2++;
                    fprintf(stderr,"  [kup] SetTexture asama=%u doku=0x%08x vs=%u\n",
                            A(c,1), A(c,2), g_cur_vs); }
            }
            uint32_t stg=A(c,1), to=A(c,2);
            if (stg < 8u) g_raw_tex[stg] = to;
            /* Tum asamalari izle: araba boyasi/yansimasi ust asamalarda
             * olabilir; simdilik yalnizca olcum icin. */
            if (stg < 4u)
                g_tex_stage[stg] = (to && mem_is_committed(to+28)) ? MR32(to+28) : 0;
            if (stg==0){
                g_cur_tex = (to && mem_is_committed(to+28)) ? MR32(to+28) : 0;
                if (!to) g_st_null++;
                else if (!g_cur_tex){ g_st_zero++;
                    static int sd=0;
                    if (sd<4 && mem_is_committed(to+31)){ sd++;
                        uint32_t vt = MR32(to);
                        uint32_t mg = (vt && mem_is_committed(vt)) ? MR32(vt) : 0;
                        fprintf(stderr,"  [SetTexture] nesne=%08x vtable=%08x ilk-giris=%08x"
                                " -> arayuz=%u  alanlar: +4=%u +8=%u +14=%u(fmt) +18=%u +1c=%u\n",
                                to, vt, mg, (mg>>16)&0x1FFFu,
                                MR32(to+4), MR32(to+8), MR32(to+0x14),
                                MR32(to+0x18), MR32(to+0x1c));
                    }
                } else g_st_ok++;
            } else g_st_other++;
            r=0; }
        else if (!strncmp(nm,"Draw",4)){
            g_draw++;
            { static const char *seen[8]; static int ns=0;
              int f=0; for(int q=0;q<ns;q++) if(!strcmp(seen[q],nm)) f=1;
              if(!f && ns<8){ seen[ns++]=nm;
                  fprintf(stderr,"  [cizim-adi] \"%s\"\n", nm); } }
            if (MID(DrawIndexedPrimitive)){
                /* (this, Type, BaseVertexIndex, MinIndex, NumVertices,
                 *  StartIndex, PrimitiveCount) */
                uint32_t pt=A(c,1), basev=A(c,2), starti=A(c,5), nprim=A(c,6);
                r=0;
                if (metal_available() && g_ss_vb && g_ib_obj && g_ss_stride &&
                    nprim && nprim < 200000u){
                    uint32_t vbase = MR32(g_ss_vb+8) + g_ss_off;
                    uint32_t ibase = MR32(g_ib_obj+8);
                    uint32_t ifmt  = MR32(g_ib_obj+12);
                    uint32_t isz   = (ifmt==102u) ? 4u : 2u;   /* INDEX32 : INDEX16 */
                    uint32_t ntri  = (pt==4u||pt==5u||pt==6u) ? nprim : 0u;
                    if (!ntri || !vbase || !ibase){ g_dip_skip++; }
                    else {
                        if (!g_idx_scratch) g_idx_scratch = gbuf_alloc(4096u*64u);
                        uint32_t maxv = 4095u / 3u * 3u;       /* draw_up siniri */
                        uint32_t done = 0;
                        while (done < ntri && g_ss_stride <= 64u){
                            uint32_t chunk = ntri - done;
                            if (chunk*3u > maxv) chunk = maxv/3u;
                            uint32_t nv = chunk*3u, ok = 1;
                            for (uint32_t t=0; t<chunk && ok; t++){
                                uint32_t g = done + t, i0,i1,i2;
                                if (pt==4u){ i0=g*3u; i1=g*3u+1u; i2=g*3u+2u; }
                                else if (pt==5u){ i0=g; i1=g+1u; i2=g+2u;
                                                  if (g&1u){ uint32_t tmp=i1; i1=i2; i2=tmp; } }
                                else { i0=0u; i1=g+1u; i2=g+2u; }
                                uint32_t idx[3] = { i0, i1, i2 };
                                for (int k=0;k<3;k++){
                                    uint32_t ia = ibase + (starti + idx[k])*isz;
                                    if (!mem_is_committed(ia+isz-1u)){ ok=0; break; }
                                    uint32_t vi = (isz==2u) ? MR16(ia) : MR32(ia);
                                    uint32_t src = vbase + (basev + vi)*g_ss_stride;
                                    uint32_t dst = g_idx_scratch + (t*3u+(uint32_t)k)*g_ss_stride;
                                    if (!mem_is_committed(src + g_ss_stride - 1u)){ ok=0; break; }
                                    memcpy(g_mem+dst, g_mem+src, g_ss_stride);
                                }
                            }
                            if (!ok){ g_dip_skip++; break; }
                            draw_up(4u, nv, g_idx_scratch, g_ss_stride);
                            g_dip_draws++; g_dip_tris += chunk;
                            done += chunk;
                        }
                    }
                }
            }
            else if (MID(DrawPrimitiveUP)){
                /* (this, PrimitiveType, PrimitiveCount, pVertexData, VertexStride) */
                static uint64_t ptype[8], stride_hist[64];
                static int sh = 0;
                uint32_t pt=A(c,1), stride=A(c,4);
                if (pt<8) ptype[pt]++;
                if (stride < 64*4 && (stride&3)==0) stride_hist[stride/4]++;
                draw_stats_save(ptype, stride_hist);
                if (metal_available()){
                    uint32_t nprim=A(c,2), vp=A(c,3);
                    uint32_t nv = (pt==4) ? nprim*3u : nprim+2u;
                    if (vp && stride && nv >= 3 && nv < 4096u &&
                        mem_is_committed(vp) && mem_is_committed(vp + nv*stride - 1))
                        draw_up(pt, nv, vp, stride);
                }
                if (dd_on()){
                    static uint32_t last_decl = 0xFFFFFFFFu; static int nd = 0;
                    uint32_t nprim=A(c,2), vp=A(c,3);
                    uint32_t nv = (pt==4) ? nprim*3u : nprim+2u;
                    if (g_cur_decl != last_decl){ last_decl = g_cur_decl; dd_decl(g_cur_decl); }
                    if (nd++ < 40){
                        fprintf(stderr,"   #%-3d tip=%u kose=%-4u adim=%u doku=%u\n",
                                nd, pt, nv, stride, g_cur_tex);
                        for (uint32_t v=0; v<nv && v<4; v++){
                            uint32_t o = vp + v*stride;
                            if (!mem_is_committed(o+31)) break;
                            fprintf(stderr,"        [%u] ", v);
                            for (int w2=0; w2<8; w2++)
                                fprintf(stderr,"%s%.4g", w2?" ":"", (double)MRF32(o+4*w2));
                            fprintf(stderr,"  | ham:");
                            for (int w2=0; w2<8; w2++) fprintf(stderr," %08x", MR32(o+4*w2));
                            fprintf(stderr,"\n");
                        }
                    }
                }
                if (VB() && sh < 6){
                    sh++;
                    uint32_t d  = g_cur_decl;
                    uint32_t el = (d && mem_is_committed(d+4)) ? MR32(d+4) : 0;
                    fprintf(stderr,"  [draw] tip=%u sayi=%u adim=%u decl=0x%08x fvf=0x%08x eleman=0x%08x\n",
                            pt, A(c,2), stride, d, g_cur_fvf, el);
                    /* ilk kose verilerini dok */
                    { uint32_t vp = A(c,3), nprim = A(c,2);
                      uint32_t nv = (pt==6||pt==5) ? nprim+2u : nprim*3u;
                      if (nv > 6) nv = 6;
                      for (uint32_t v=0; v<nv; v++){
                        uint32_t o = vp + v*stride;
                        if (!mem_is_committed(o+24)) break;
                        fprintf(stderr,"        v%u: pos=(%.2f, %.2f, %.2f) renk=0x%08x uv=(%.3f, %.3f)\n",
                                v, MRF32(o), MRF32(o+4), MRF32(o+8), MR32(o+12),
                                MRF32(o+16), MRF32(o+20));
                      } }
                    for (uint32_t k = 0; el && k < 16; k++){
                        uint32_t e = el + 8u*k;
                        if (!mem_is_committed(e+7)) break;
                        uint32_t st=MR16(e), of=MR16(e+2), ty=MR8(e+4), us=MR8(e+6);
                        if (st == 0xFFu && ty == 17u) break;
                        fprintf(stderr,"        ofset=%-3u tip=%-2u kullanim=%u\n", of, ty, us);
                    }
                }
            }
            r=0; }
        else if (MID(SetVertexShader)){
            uint32_t h=A(c,1); g_raw_vs = h;
            g_cur_vs = (h && mem_is_committed(h+15)) ? MR32(h+12) : 0;
            if (g_cur_vs >= MAXVS) g_cur_vs = 0;
            r=0; }
        else if (MID(SetPixelShader)){
            uint32_t sh=A(c,1); g_raw_ps = sh;
            g_cur_ps_id = (sh && mem_is_committed(sh+15)) ? (int)MR32(sh+12) : 0;
            r=0; }
        else if (MID(SetVertexShaderConstantF)){
            /* (this, StartRegister, pConstantData, Vector4fCount) */
            uint32_t sr=A(c,1), pd=A(c,2), n=A(c,3);
            static int sh=0;
            g_sc[g_scn & 7].sr = sr; g_sc[g_scn & 7].pd = pd;
            g_sc[g_scn & 7].n = n; g_scn++;
            for (uint32_t k=0; k<n && sr+k<256; k++){
                uint32_t o=pd+16u*k;
                if (!mem_is_committed(o+15)) break;
                memcpy(g_vsk.f[sr+k], g_mem+o, 16);
            }
            /* VSKDUMP: pd'nin CEVRESINI dok.  Matris 2 vektor kaymissa
             * dogru satirlar pd-32'de duruyor olmali. */
            /* WATCHVSK: bu tampona kim yaziyor?  Yuklemeden hemen sonra
             * sayfayi salt-okunur yap; sonraki karenin yazani yakalanir. */
            /* PDLIST: farkli pd degerlerini ve matrisin bozuk olup
             * olmadigini listele.  Iyi/bozuk blok karsilastirmasi icin. */
            if (ENVC("PDLIST") && sr==0 && n>=4 && pd && mem_is_committed(pd+63)){
                static uint32_t seen[32]; static int ns=0;
                int f=0; for (int q=0;q<ns;q++) if (seen[q]==pd) f=1;
                if (!f && ns<32){ seen[ns++]=pd;
                    float m00=MRF32(pd), m11=MRF32(pd+20), m33=MRF32(pd+60);
                    int bad = (m00==0.f && m11==0.f && m33==0.f);
                    fprintf(stderr,"  [pd] %08x n=%u %s  m00=%g m11=%g m33=%g\n",
                            pd, n, bad?"BOZUK":"iyi  ", m00, m11, m33);
                }
            }
            /* OWNERSCAN: obekte pd'yi isaret eden yapiyi bul, icinde ad ara. */
            if (ENVC("OWNERSCAN") && sr==0 && n==5 && pd){
                static int os_=0, oseen=0;
                oseen++;
                if (oseen > 400 && !os_){ os_=1;
                    fprintf(stderr,"  [sahip] pd=%08x'i isaret eden yapilar:\n", pd);
                    int found = 0;
                    for (uint64_t A2 = 0x10000000ull; A2 < 0x30000000ull && found < 12; A2 += 4){
                        uint32_t a3 = (uint32_t)A2;
                        if ((a3 & 0x3FFFu) == 0 && !mem_is_committed(a3)) { A2 += 0x3FFCull; continue; }
                        if (!mem_is_committed(a3+3)) continue;
                        if (MR32(a3) != pd) continue;
                        found++;
                        fprintf(stderr,"    yapi alani %08x:\n", a3);
                        uint32_t base = (a3 > 0x80u) ? a3 - 0x80u : 0x10000u;
                        for (uint32_t q = base; q <= a3 + 0x80u; q += 4u){
                            if (!mem_is_committed(q+3)) continue;
                            uint32_t P = MR32(q);
                            if (P < 0x10000u || !mem_is_committed(P+48u)) continue;
                            const char *t = (const char*)(g_mem+P);
                            int ok = 1, L = 0;
                            while (L < 44 && t[L]){
                                unsigned char ch = (unsigned char)t[L];
                                if (ch < 0x20u || ch > 0x7Eu){ ok = 0; break; }
                                L++; }
                            if (ok && L >= 3 && L < 44 && t[L] == 0)
                                fprintf(stderr,"        %08x (pd%+d) -> \"%s\"\n",
                                        q, (int)q - (int)a3, t);
                        }
                    }
                    if (!found) fprintf(stderr,"    (isaret eden bulunamadi)\n");
                }
            }
            /* PARAMNAME: yigindaki isaretcileri tarayip D3DX efekt
             * parametresinin ADINI bul.  Hangi parametrenin hic
             * ayarlanmadigini ogrenmek icin. */
            if (ENVC("PARAMNAME") && sr==0 && n==5 && pd){
                static int pn=0, pseen=0;
                pseen++;
                if (pseen > 400 && pn < 1){ pn=1;
                    fprintf(stderr,"  [param] pd=%08x icin yigin taramasi:\n", pd);
                    for (uint32_t off=0; off<0x120u; off+=4u){
                        uint32_t sp2 = c->esp + off;
                        if (!mem_is_committed(sp2+3)) continue;
                        uint32_t V = MR32(sp2);
                        if (V < 0x10000u || !mem_is_committed(V+0x67u)) continue;
                        for (uint32_t q=0; q<0x64u; q+=4u){
                            uint32_t P = MR32(V+q);
                            if (P < 0x10000u || !mem_is_committed(P+40u)) continue;
                            const char *t = (const char*)(g_mem+P);
                            int ok=1, L=0;
                            while (L<40 && t[L]){
                                unsigned char ch=(unsigned char)t[L];
                                if (ch<0x20u || ch>0x7Eu){ ok=0; break; }
                                L++; }
                            if (ok && L>=4 && L<40 && t[L]==0)
                                fprintf(stderr,"      yigin+%03x -> yapi %08x +%02x -> \"%s\"\n",
                                        off, V, q, t);
                        }
                    }
                }
            }
            /* VSKBT: matrisi yukleyen guest cagirici zinciri */
            if (ENVC("VSKBT") && sr==0 && n==5 && pd){
                static int bt=0, bseen=0;
                bseen++;
                if (bseen > atoi(ENVC("VSKBT")) && bt<2){ bt++;
                    fprintf(stderr,"  [vsk-cagirici] yukleme #%d pd=%08x\n", bseen, pd);
                    guest_backtrace(c, 14); }
            }
            if (ENVC("WATCHVSK") && sr==0 && n==5 && pd){
                static int wv=0, wseen=0;
                wseen++;
                if (wseen > atoi(ENVC("WATCHVSK")) && !wv){ wv=1; mprot_arm(pd); }
            }
            if (ENVC("VSKDUMP") && sr==0 && n==5 && pd){
                static int vd=0, vseen=0;
                vseen++;
                if (vseen>500 && vd<3){ vd++;
                    fprintf(stderr,"  [vsk] pd=%08x n=%u cevresi:\n", pd, n);
                    for (int q=-4; q<8; q++){
                        uint32_t o2 = pd + (uint32_t)(16*q);
                        if (!mem_is_committed(o2+15)) { fprintf(stderr,"      %+3d: (yok)\n", q); continue; }
                        fprintf(stderr,"      %+3d (%08x): %14.7g %14.7g %14.7g %14.7g\n", q, o2,
                                MRF32(o2), MRF32(o2+4), MRF32(o2+8), MRF32(o2+12));
                    }
                }
            }
            if (ENVC("CONSTDUMP") && sr==0 && n==5 && pd && mem_is_committed(pd+79)){
                static int cd=0;
                if (cd<2){ cd++;
                    for (uint32_t bp2=(pd & ~15u); bp2>=0x10000000u; bp2-=16u){
                        if (!mem_is_committed(bp2+4)) break;
                        if (MR32(bp2+4)==0xA110C8EDu){
                            fprintf(stderr,"  [blok] pd=%08x  blok basi=%08x boyut=%u"
                                    "  -> pd ofseti=%d\n",
                                    pd, bp2+16u, MR32(bp2), (int)(pd-(bp2+16u)));
                            break; } }
                    fprintf(stderr,"  [sabitler] pd=%08x n=%u  (blok oncesi 2 vec4 dahil)\n", pd, n);
                    for (int rw=-2; rw<(int)n && rw<11; rw++){
                        uint32_t a2 = pd + (uint32_t)(rw*16);
                        if (!mem_is_committed(a2+15)) continue;
                        fprintf(stderr,"      c%-3d %11.5g %11.5g %11.5g %11.5g\n", rw,
                                MRF32(a2), MRF32(a2+4), MRF32(a2+8), MRF32(a2+12));
                    }
                }
            }
            /* matris saglikli mi? c0..c3 icin: son satir (0,0,0,1) benzeri
             * ya da en az bir eleman makul buyuklukte */
            if (sr==0 && n>=4 && pd && mem_is_committed(pd+63)){
                static uint64_t iyi=0, kotu=0, sifir=0; static int gos=0;
                int nz=0, buyuk=0;
                for (int q=0;q<16;q++){
                    float fv = MRF32(pd+4u*q);
                    if (fv != 0.f) nz++;
                    if (fabsf(fv) > 1e6f) buyuk++;
                }
                if (nz==0) sifir++;
                else if (buyuk){ kotu++;
                    if (ENVC("WATCHALLOC") && !g_wc_armed){
                        g_wc_armed = 1;
                        void watch_dump(const char*, int);
                        void watch_arm(uint32_t);
                        watch_dump("sabit blogu yazicisi", 30);
                        watch_arm(0);
                    }
                    if (0){
                        if (++g_wc_after == 40){
                            void watch_dump(const char*, int);
                            void watch_arm(uint32_t);
                            watch_dump("sabit blogu yazicisi", 24);
                            watch_arm(0);              /* izlemeyi kapat */
                            g_wc_armed = 0;
                        }
                    }
                    /* c->eip = SetVertexShaderConstantF'i cagiran guest komutunun
                     * hemen sonrasi (com_call girisinde POP32 edildi) */
                    static uint32_t caller[8]; static int ncall=0;
                    int seen=0;
                    for (int q=0;q<ncall;q++) if (caller[q]==c->eip) seen=1;
                    if (!seen && ncall<8){ caller[ncall++]=c->eip;
                        fprintf(stderr,"  [matris-BOZUK] pd=%08x  cagiran donus=0x%08x  n=%u\n",
                                pd, c->eip, n);
                        /* Cagiran ebp-tabanli cerceve kullaniyor ve ebp canli:
                         * [ebp-8] = parametre tanimlayicisi (bkz. 0x00669d87). */
                        /* pd hangi heap blogunda? (baslik: +0 boyut, +4 0xA110C8ED) */
                        for (uint32_t bp = (pd & ~15u); bp >= 0x10000000u; bp -= 16u){
                            if (!mem_is_committed(bp+4)) break;
                            if (MR32(bp+4) == 0xA110C8EDu){
                                uint32_t bsz = MR32(bp);
                                fprintf(stderr,"      pd blogu: bas=%08x boyut=%u  pd ofseti=%d\n",
                                        bp+16u, bsz, (int)(pd - (bp+16u)));
                                fprintf(stderr,"      blok basi 64 bayt:");
                                for (int q=0;q<64;q++){ if(q%32==0) fprintf(stderr,"\n        ");
                                    fprintf(stderr,"%02x ", MR8(bp+16u+q)); }
                                fprintf(stderr,"\n        ascii: ");
                                for (int q=0;q<64;q++){ uint8_t ch=MR8(bp+16u+q);
                                    fputc((ch>=32&&ch<127)?ch:'.', stderr); }
                                fprintf(stderr,"\n");
                                break;
                            }
                        }
                        /* Bir sonraki yazmayi yakalamak icin watchpoint kur:
                         * blok her karede yeniden dolduruluyor, bu yuzden
                         * simdi kurmak yeterli. */
                        if (ENVC("WATCHCONST")){
                            void watch_arm(uint32_t);
                            watch_arm(pd + 16u);
                            g_wc_armed = 1;
                        }
                        /* Semantik hash'i (h=h*33+c) bellekte ara: oyun tarafi
                         * ve efekt tarafi ayni degeri tutuyor mu? */
                        /* "WorldViewProj" string'ini ve ona isaret eden yapilari bul:
                         * parametre tanimlayicisinin tur alanini dogrudan gormek icin. */
                        if (ENVC("FINDPARAM")){
                            static const char *needle = "WorldViewProj";
                            size_t nl = strlen(needle);
                            uint32_t found[8]; int nf = 0;
                            for (uint32_t a = 0x10000000u; a < 0x10400000u && nf < 8; a += 4){
                                if (!mem_is_committed(a + 32)) { a += 0x3FFCu; continue; }
                                if (!memcmp(g_mem + a, needle, nl) && MR8(a+nl) == 0)
                                    found[nf++] = a;
                            }
                            fprintf(stderr,"      'WorldViewProj' bulundu: %d yerde\n", nf);
                            for (int k = 0; k < nf; k++){
                                fprintf(stderr,"        @%08x\n", found[k]);
                                /* bu adrese isaret eden dword'leri ara */
                                int np = 0;
                                for (uint32_t a = 0x10000000u; a < 0x10400000u && np < 6; a += 4){
                                    if (!mem_is_committed(a + 4)) { a += 0x3FFCu; continue; }
                                    if (MR32(a) != found[k]) continue;
                                    np++;
                                    /* isaretci bir yapinin hangi alaninda? +8 varsayimiyla dene */
                                    for (int off = 0; off <= 0x10; off += 4){
                                        uint32_t base = a - (uint32_t)off;
                                        if (!mem_is_committed(base + 0x5F)) continue;
                                        fprintf(stderr,"          isaretci@%08x (yapi basi %08x, alan +%02x)"
                                                "  +54=%u +56=%u +04=%u +00=%u\n",
                                                a, base, off, MR16(base+0x54), MR16(base+0x56),
                                                MR32(base+4), MR32(base));
                                    }
                                }
                            }
                        }
                        uint32_t desc = mem_is_committed(c->ebp) ? MR32(c->ebp - 8u) : 0;
                        fprintf(stderr,"      ebp=%08x tanimlayici=%08x\n", c->ebp, desc);
                        if (desc && mem_is_committed(desc+0x5F)){
                            for (int rw2=0; rw2<0x60; rw2+=16){
                                fprintf(stderr,"        +%02x:", rw2);
                                for (int q=0;q<16;q++) fprintf(stderr," %02x", MR8(desc+rw2+q));
                                fprintf(stderr,"  ");
                                for (int q=0;q<16;q++){ uint8_t ch=MR8(desc+rw2+q);
                                    fputc((ch>=32&&ch<127)?ch:'.', stderr); }
                                fprintf(stderr,"\n");
                            }
                            fprintf(stderr,"        reg=%u say=%u bayrak30=%02x\n",
                                    MR16(desc+0x54), MR16(desc+0x56), MR8(desc+0x30));
                            for (int f=0; f<0x60; f+=4){
                                uint32_t v = MR32(desc+f);
                                if (v > 0x10000000u && v < 0x30000000u && mem_is_committed(v+15)){
                                    fprintf(stderr,"        [+%02x]->%08x (pd fark %+d):", f, v, (int)(v - pd));
                                    for (int q=0;q<16;q++) fprintf(stderr," %02x", MR8(v+q));
                                    fprintf(stderr,"  ");
                                    for (int q=0;q<16;q++){ uint8_t ch=MR8(v+q);
                                        fputc((ch>=32&&ch<127)?ch:'.', stderr); }
                                    fprintf(stderr,"\n");
                                }
                            }
                        }
                        for (uint32_t rw=0; rw<n && rw<13; rw++){
                            if (!mem_is_committed(pd+rw*16+15)) break;
                            fprintf(stderr,"      c%-2u %12.4g %12.4g %12.4g %12.4g\n", rw,
                                MRF32(pd+rw*16), MRF32(pd+rw*16+4),
                                MRF32(pd+rw*16+8), MRF32(pd+rw*16+12));
                        }
                        /* tamponun ONCESI: oyun bazen geriye kaydirilmis
                         * isaretci veriyor; gercek veri orada olabilir */
                        if (pd >= 64 && mem_is_committed(pd-64)){
                            fprintf(stderr,"      --- pd'den once 4 vec4 ---\n");
                            for (int rw=-4; rw<0; rw++)
                                fprintf(stderr,"      c%-2d %12.4g %12.4g %12.4g %12.4g\n", rw,
                                    MRF32(pd+rw*16), MRF32(pd+rw*16+4),
                                    MRF32(pd+rw*16+8), MRF32(pd+rw*16+12));
                        }
                    }
                    static int bt=0;
                    if (ENVC("MATBT") && bt<2){ bt++;
                        fprintf(stderr,"  [matris-BOZUK] pd=%08x, cagri yigini:\n", pd);
                        guest_backtrace(c, 14); } }
                else { iyi++;
                       if (gos<3){ gos++;
                         fprintf(stderr,"  [matris-IYI] pd=%08x\n", pd);
                         for (int rw=0;rw<4;rw++)
                           fprintf(stderr,"      %9.4f %9.4f %9.4f %9.4f\n",
                             MRF32(pd+rw*16),MRF32(pd+rw*16+4),
                             MRF32(pd+rw*16+8),MRF32(pd+rw*16+12)); } }
                if (((iyi+kotu+sifir) % 20000)==0)
                    fprintf(stderr,"  [matris] iyi=%llu bozuk=%llu sifir=%llu\n",
                            (unsigned long long)iyi,(unsigned long long)kotu,
                            (unsigned long long)sifir);
            }
            if (ENVC("VSCDUMP") && sh<6 && sr==0 && n>=4 && pd && mem_is_committed(pd+63)){
                sh++;
                fprintf(stderr,"  [vsconst] esp=%08x pd=%08x sr=%u n=%u  yigin:",
                        c->esp, pd, sr, n);
                for (int q=0;q<6;q++) fprintf(stderr," %08x", MR32(c->esp+4u*q));
                fprintf(stderr,"\n            ham@pd:");
                for (int q=0;q<16;q++){ if(q%8==0) fprintf(stderr,"\n              ");
                    fprintf(stderr," %08x", MR32(pd+4u*q)); }
                fprintf(stderr,"\n");
            }
            if (VB() && sh<8 && sr==0 && n>=4 && pd && mem_is_committed(pd+63)){
                sh++;
                fprintf(stderr,"  [vsconst] c0..c3 (StartRegister=%u, %u vektor):\n", sr, n);
                for (uint32_t rw=0; rw<4; rw++)
                    fprintf(stderr,"     [%8.3f %8.3f %8.3f %8.3f]\n",
                            MRF32(pd+rw*16), MRF32(pd+rw*16+4),
                            MRF32(pd+rw*16+8), MRF32(pd+rw*16+12));
            }
            r=0; }
        else if (MID(SetStreamSource)){
            /* (this, StreamNumber, pStreamData, OffsetInBytes, Stride) */
            if (A(c,1)==0u){ g_ss_vb=A(c,2); g_ss_off=A(c,3); g_ss_stride=A(c,4); }
            r=0; }
        else if (MID(SetIndices)){ g_ib_obj=A(c,1); r=0; }
        else if (MID(SetPixelShaderConstantF)){
            /* (this, StartRegister, pConstantData, Vector4fCount) */
            uint32_t sr2=A(c,1), pd2=A(c,2), n2=A(c,3);
            if (sr2+n2 > g_psc_max) g_psc_max = sr2+n2;
            if (sr2+n2 > 32u) g_psc_drop++;
            for (uint32_t q4=0; q4<n2 && sr2+q4<32u; q4++){
                uint32_t off = pd2 + 16u*q4;
                if (!pd2 || !mem_is_committed(off+15)) break;
                for (int c4=0;c4<4;c4++) g_psc[sr2+q4][c4] = MRF32(off+4u*(uint32_t)c4);
            }
            r=0; }
        
        else if      (MID(TestCooperativeLevel)){
            g_tcl_n++;
            /* Cihaz kaybi (tam ekran gecisi): oyun D3D9'daki gibi
             * D3DPOOL_DEFAULT kaynaklarini birakip Reset cagiriyor. */
            r = atomic_load(&g_lost) ? 0x88760869u /* D3DERR_DEVICENOTRESET */ : 0u;
            if (r && g_tcl_lost_n++ < 4) fprintf(stderr,"com: TestCooperativeLevel -> DEVICENOTRESET\n"); }
        else if (MID(GetAvailableTextureMem)) r=256u*1024u*1024u;
        else if (MID(GetDeviceCaps)){ uint32_t pc=A(c,1); if(pc) fill_caps(pc); r=0; }
        else if (MID(GetDirect3D)){ uint32_t p=A(c,1); if(p) MR32(p)=g_d3d9; r=0; }
        else if (MID(GetDisplayMode)){
            /* GECERLI boyut olmali; sabit 1280x720 donunce oyunun ayar
             * ekrani listedeki secili satiri bulamiyordu.  Ayni sebeple
             * cihaz oyunun sandigindan buyukse (Retina olcegi, NFSRES)
             * gercek boyutu (or. 2448x1836) degil oyunun KENDI sectigi
             * boyutu bildiriyoruz: gercek boyut o tabloda yok. */
            uint32_t pm=A(c,2);
            uint32_t dmw = g_req_w ? g_req_w : g_bb_w;
            uint32_t dmh = g_req_h ? g_req_h : g_bb_h;

            if(pm){mem_commit(pm,16);MR32(pm)=dmw;MR32(pm+4)=dmh;
                   MR32(pm+8)=60;MR32(pm+12)=22;} r=0; }
        else if (MID(Clear)){
            /* Clear(this,Count,pRects,Flags,Color,Z,Stencil) -> Color = arg4 */
            /* Flags: 1=TARGET 2=ZBUFFER 4=STENCIL; Z = arg5 (float) */
            if (metal_available()){
                void metal_clear_target_ex(unsigned, unsigned, float);
                uint32_t zb = A(c,5); float zf; memcpy(&zf, &zb, 4);
                metal_set_clear_color(A(c,4)); metal_clear_target_ex(A(c,4), A(c,3), zf); }
            r=0; }
        else if (MID(BeginScene)){ if (metal_available()) metal_begin_frame(); r=0; }
        else if (MID(EndScene)) r=0;
        else if (MID(Present)){ g_present_no++;
            { extern void guest_cpu_mark(void); guest_cpu_mark(); }
            /* FETICK: on yuz tiki [0x8651a8] 60 Hz'de artmali ama oyunun
             * kare-farki olcumu hep 1/1800 s veriyor, bu yuzden tik hic
             * artmiyor -> animasyonlar ve zamanli ekran gecisleri donuyor.
             * Sanal saatimiz kare basina tam 1/60 s ilerlettigi icin kare
             * basina +1 tik dogru hizi verir. */
            if (ENVC("FETICK") && mem_is_committed(0x008651a8u))
                MR32(0x008651a8u) = MR32(0x008651a8u) + 1u;
            /* FIXFRAMETIME: oyunun HEDEF KARE SURESI [0x865558] hic
             * kurulmuyor (kuran sub_00576cc0/ce0 cagrilmiyor) -> kare
             * farki dt hep 1/1800 kaliyor, on yuz tiki [0x8651a8] hic
             * artmiyor, animasyonlar ve zamanli ekran gecisleri donuyor.
             * Oyunun kendi varsayilani [0x7fb710] = 1/120. */
            { const char *ff = ENVC("FIXFRAMETIME");
              static int done_ft = 0;
              if (ff && *ff && ff[0] != '0' && !done_ft && g_present_no >= 2u){
                  done_ft = 1;
                  float v = (float)atof(ff);
                  if (!(v > 0.f)) v = mem_is_committed(0x007fb710u)
                                      ? MRF32(0x007fb710u) : (1.f/120.f);
                  if (mem_is_committed(0x00865558u)){
                      MRF32(0x00865558u) = v;
                      fprintf(stderr,"  [kare-suresi] [0x865558] = %g olarak kuruldu\n",
                              (double)v);
                  }
              } }
            /* DATADIFF: iki kare arasinda hangi global degiskenler
             * degisiyor?  Donmus bir sayac/zaman varsa dogrudan gorunur. */
            { const char *dd2 = ENVC("DATADIFF");
              static uint8_t *snap = 0; static int taken = 0;
              const uint32_t DLO = 0x007E0000u, DHI = 0x008A0000u;
              if (dd2 && *dd2){
                  uint32_t at = (uint32_t)atoi(dd2);
                  if (!taken && g_present_no == at){
                      snap = malloc(DHI - DLO);
                      if (snap){ memcpy(snap, g_mem + DLO, DHI - DLO); taken = 1;
                                 fprintf(stderr,"  [fark] kare %u: anlik goruntu alindi\n", at); }
                  } else if (taken == 1 && g_present_no == at + 600u){
                      taken = 2; int n = 0;
                      fprintf(stderr,"  [fark] kare %u ile %u arasinda degisenler:\n", at, at+600u);
                      for (uint32_t o = 0; o + 4 <= DHI - DLO; o += 4){
                          uint32_t oldv, newv;
                          memcpy(&oldv, snap + o, 4);
                          memcpy(&newv, g_mem + DLO + o, 4);
                          if (oldv == newv) continue;
                          if (++n > 40) break;
                          float fo, fn2; memcpy(&fo,&oldv,4); memcpy(&fn2,&newv,4);
                          fprintf(stderr,"     0x%08x: %10u -> %-10u  (%g -> %g)\n",
                                  DLO + o, oldv, newv, (double)fo, (double)fn2);
                      }
                      fprintf(stderr,"  [fark] toplam degisen dword (ilk 40 gosterildi)\n");
                  }
              } }
            /* FORCESWITCH: splash'te takildik.  Gecisi elle tetikleyip
             * bir sonraki ekranin (profil / ana menu) yuklenip
             * yuklenmedigini olcuyoruz.  TANI amacli. */
            { const char *fs2 = ENVC("FORCESWITCH");
              static int fired = 0;
              extern uint32_t g_fe_mgr;
              if (fs2 && *fs2 && !fired && g_present_no >= 2500u && g_fe_mgr){
                  fired = 1;
                  uint32_t from = 0x0078ea60u;              /* UG_LS_Splash.fng */
                  uint32_t to   = (atoi(fs2) == 2) ? 0x0078f530u   /* UI_Main.fng */
                                                   : 0x0078ea4cu;  /* MC_Background.fng */
                  fprintf(stderr,"  [zorla] SwitchFEngPackages(yonetici=%08x, \"%s\" -> \"%s\")\n",
                          g_fe_mgr, (const char*)(g_mem+from), (const char*)(g_mem+to));
                  { CPU save = *c;
                    if (atoi(fs2) == 3){
                        /* sub_004c4f50(mevcut_paket_adi, 0): onyukleme
                         * akisini ilerletir; hedefi kendi listesinden bulur
                         * ve YUKLEYEREK gecis yapar. */
                        PUSH32(0u); PUSH32(from);
                        PUSH32(0xDEADBEEFu);
                        dispatch_indirect(c, 0x004c4f50u);
                    } else {
                        PUSH32(to); PUSH32(from);
                        PUSH32(0xDEADBEEFu);
                        c->ecx = g_fe_mgr;
                        dispatch_indirect(c, 0x0051cbb0u);
                    }
                    *c = save; }
                  fprintf(stderr,"  [zorla] cagri dondu\n");
              } }
            /* BOOTFLOW: onyukleme akisi listesini (0x836488) yuru.
             * Splash'te neyin beklendigini dogrudan gosterir. */
            if (ENVC("BOOTFLOW") && (g_present_no % 900u) == 5u){
                uint32_t head = mem_is_committed(0x00836488u) ? MR32(0x00836488u) : 0;
                { /* On yuz saati zinciri:
                   *   [0x865194] = kare farki (dt, saniye)
                   *   [0x865140] = biriktirici kalan
                   *   [0x8651a8] = on yuz tiki (dt'den 60 Hz'e yuvarlanir)
                   * Animasyonlar ve zamanli ekran gecisleri bu tike bagli. */
                  fprintf(stderr,"  [fe-saat] kare %u: dt(865194)=%g  kalan(865140)=%g"
                          "  TIK(8651a8)=%d  kare(865148)=%d  hedef(865558)=%g\n", g_present_no,
                          mem_is_committed(0x00865194u)?(double)MRF32(0x00865194u):-1.0,
                          mem_is_committed(0x00865140u)?(double)MRF32(0x00865140u):-1.0,
                          mem_is_committed(0x008651a8u)?(int)MR32(0x008651a8u):-1,
                          mem_is_committed(0x00865148u)?(int)MR32(0x00865148u):-1,
                          mem_is_committed(0x00865558u)?(double)MRF32(0x00865558u):-1.0);
                  { uint64_t host_us(void);
                    uint64_t t1 = host_us(); volatile int sp=0;
                    for (int q=0;q<200000;q++) sp+=q;
                    uint64_t t2 = host_us();
                    { /* __ftol (sub_0075bed4) dogrulugu: ST(0)=deger ile
                       * cagirip sonucu karsilastir.  Kare-farki zincirindeki
                       * tek supheli ilkel bu. */
                      static int ft_done = 0;
                      if (!ft_done){ ft_done = 1;
                        const double tv[5] = { 90.5, 15.5, 1.5, 0.5, 30000.5 };
                        for (int q = 0; q < 5; q++){
                            CPU sv = *c;
                            c->top = (c->top - 1) & 7; c->st[c->top] = tv[q];
                            PUSH32(0xDEADBEEFu);
                            dispatch_indirect(c, 0x0075bed4u);
                            fprintf(stderr,"           __ftol(%g) = %d %s\n",
                                    tv[q], (int)c->eax,
                                    ((int)c->eax == (int)tv[q]) ? "" : "  <-- YANLIS");
                            *c = sv;
                        } } }
                    fprintf(stderr,"           ARA esi(86513c)=%d  int2 sonrasi kalan(865144)=%g\n",
                            mem_is_committed(0x0086513cu)?(int)MR32(0x0086513cu):-1,
                            mem_is_committed(0x00865144u)?(double)MRF32(0x00865144u):-1.0);
                    { /* sub_0057eae0 her karede olculen kare suresini
                       * 0x8647a8 + (kare%100)*16 + 4 adresine yaziyor. */
                      uint32_t fr = mem_is_committed(0x00865148u)?MR32(0x00865148u):0;
                      fprintf(stderr,"           OLCULEN kare suresi (son 4 kare):");
                      for (int q=0;q<4;q++){
                          uint32_t idx = (fr - (uint32_t)q) % 100u;
                          uint32_t a4 = 0x008647a8u + idx*16u + 4u;
                          fprintf(stderr,"  %g", mem_is_committed(a4)?(double)MRF32(a4):-1.0);
                      }
                      fprintf(stderr,"\n"); }
                    fprintf(stderr,"           ms/tik(828028)=%g  kaydirma(8284e4)=%u\n",
                            mem_is_committed(0x00828028u)?(double)MRF32(0x00828028u):-1.0,
                            mem_is_committed(0x008284e4u)?MR32(0x008284e4u):0);
                    fprintf(stderr,"           QPC bolen(8284e4)=%u  olcek(78435c)=%g"
                            "  host_us kare-ici: %llu -> %llu (fark %llu us)\n",
                            mem_is_committed(0x008284e4u)?MR32(0x008284e4u):0,
                            mem_is_committed(0x0078435cu)?(double)MRF32(0x0078435cu):0.0,
                            (unsigned long long)t1,(unsigned long long)t2,
                            (unsigned long long)(t2-t1)); }
                  fprintf(stderr,"           girdi(7fb714)=%g  mod(7fb700)=%u  kalan2(865144)=%g\n",
                          mem_is_committed(0x007fb714u)?(double)MRF32(0x007fb714u):-1.0,
                          mem_is_committed(0x007fb700u)?MR32(0x007fb700u):0,
                          mem_is_committed(0x00865144u)?(double)MRF32(0x00865144u):-1.0); }
                { /* Oyun saati: sub_0057a1b0 => [0x865134] * sabit.
                   * Bu sayac artmiyorsa on yuz animasyonlari donuk kalir. */
                  fprintf(stderr,"  [oyunsaati] tik(865134)=%d  (865138)=%08x  sec(7fb700)=%u"
                          "  olcek=%g/%g  fe_t(864720)=%g\n",
                          mem_is_committed(0x00865134u)?(int)MR32(0x00865134u):-1,
                          mem_is_committed(0x00865138u)?MR32(0x00865138u):0,
                          mem_is_committed(0x007fb700u)?MR32(0x007fb700u):0,
                          mem_is_committed(0x007851c0u)?(double)MRF32(0x007851c0u):0.0,
                          mem_is_committed(0x007875bcu)?(double)MRF32(0x007875bcu):0.0,
                          mem_is_committed(0x00864720u)?(double)MRF32(0x00864720u):0.0); }
                { uint64_t host_us(void); uint32_t host_ms(void);
                  fprintf(stderr,"  [saat] kare %u: host_us=%llu  host_ms=%u\n",
                          g_present_no, (unsigned long long)host_us(), host_ms()); }
                fprintf(stderr,"  [onyukleme] kare %u  liste=%08x  bayrak(83648c)=%u  (836498)=%08x\n",
                        g_present_no, head,
                        mem_is_committed(0x0083648cu) ? MR8(0x0083648cu) : 999u,
                        mem_is_committed(0x00836498u) ? MR32(0x00836498u) : 0);
                if (head && mem_is_committed(head+0x40u)){
                    for (uint32_t q = 0; q < 0x40u; q += 4u){
                        uint32_t V = MR32(head+q);
                        const char *t = 0;
                        if (V > 0x10000u && mem_is_committed(V+40u)){
                            const char *z = (const char*)(g_mem+V);
                            int ok=1, L=0;
                            while (L<38 && z[L]){ unsigned char ch=(unsigned char)z[L];
                                if (ch<0x20u||ch>0x7Eu){ ok=0; break; } L++; }
                            if (ok && L>=3 && z[L]==0) t = z;
                        }
                        fprintf(stderr,"      +%02x = %08x%s%s\n", q, V,
                                t ? "  -> " : "", t ? t : "");
                    }
                }
            }
            /* FLOWLOG: oyunun kendi akis izleyicisini oku.
             * 0x86549c ve 0x864f88 o an calisan adimin ADINI tutuyor. */
            if (ENVC("FLOWLOG") && (g_present_no % 500u) == 1u){
                static char last[160] = {0};
                char cur[160]; cur[0] = 0;
                uint32_t gl[2] = { 0x0086549cu, 0x00864f88u };
                for (int q = 0; q < 2; q++){
                    const char *t = "(?)";
                    if (mem_is_committed(gl[q]+3)){
                        uint32_t P = MR32(gl[q]);
                        if (P > 0x10000u && mem_is_committed(P+2)) t = (const char*)(g_mem+P);
                    }
                    strncat(cur, t, 70);
                    strncat(cur, " | ", 4);
                }
                if (strcmp(cur, last)){
                    snprintf(last, sizeof last, "%s", cur);
                    fprintf(stderr,"  [akis] kare %u: %s  bayrak(865498)=%u (8654a4)=%u\n",
                            g_present_no, cur,
                            mem_is_committed(0x00865498u)?MR8(0x00865498u):999u,
                            mem_is_committed(0x008654a4u)?MR32(0x008654a4u):999u);
                }
            }
            /* PRESENTBT: ana dongunun yapisini cikar. */
            { const char *pb = ENVC("PRESENTBT");
              static int nb = 0;
              if (pb && nb < 2 && g_present_no >= (uint32_t)atoi(pb)){ nb++;
                  fprintf(stderr,"  [ana-dongu] Present #%u:\n", g_present_no);
                  guest_backtrace(c, 26); } }
            /* Kararli durumu olcmek icin: belirtilen karede kapsamayi
             * sifirla ve ornekleyiciyi baslat.  Boylece "acilisla birlikte
             * her sey" degil, "simdi ne donuyor" gorunur. */
            { static int done = 0; const char *ra2 = ENVC("COVRESET_AT");
              uint32_t at = ra2 ? (uint32_t)atoi(ra2) : 0;
              if (!done && at && g_present_no >= at){ done = 1;
                  cov_reset();
                  fprintf(stderr,"  [olcum] kare %u: kapsama sifirlandi, ornekleyici basladi\n",
                          g_present_no); }
              }
            g_present++;
            { void vclock_tick(void); vclock_tick(); }
            /* Oyun hizi = gercek fps / 60 oldugu icin kareyi 60 Hz'e
             * frenle (VARSAYILAN: fren YOK; PACE60=1 ile 60 Hz'e sabitlenir). */
            { void frame_pace(void); frame_pace(); }
            /* COVRESET=<Present#>: o kareden itibaren kapsamayi sifirla ki
             * rapor YALNIZCA kararli durumda calisan kodu gostersin. */
            { const char *cr = ENVC("COVRESET");
              if (cr && g_present_no == (uint32_t)atoi(cr)){
                  void cov_reset(void); cov_reset();
                  fprintf(stderr,"  kapsama sifirlandi (Present %u)\n", g_present_no);
              } }
            if (metal_available()) metal_present();
            { void mprot_rearm(void); mprot_rearm(); }
            /* Oyunun kendi slot havuzlari: tukenen varsa ekrana kirmizi ad
             * basiliyor.  Liste 0x828538 (kuyruk) -> +4 (onceki) ... 0x828534. */
            if (ENVC("POOLS") && (g_present_no % 150) == 20){
                fprintf(stderr,"  --- slot havuzlari (liste basi 0x828530=%u) ---\n",
                        mem_is_committed(0x828530u) ? MR32(0x828530u) : 0xFFFFFFFFu);
                /* dogrudan bilinen havuzlar: EventHandler (0x883e38), Event (0x883e34?) */
                { uint32_t known[] = {0x883e38u, 0x883e34u};
                  for (int q=0;q<2;q++){
                    if (!mem_is_committed(known[q]+4)) continue;
                    uint32_t po = MR32(known[q]);
                    if (!po || !mem_is_committed(po+0x2f)) continue;
                    uint32_t np2 = MR32(po+0x0c);
                    char nm3[48]; nm3[0]=0;
                    if (np2 && mem_is_committed(np2))
                        for (int k2=0;k2<40;k2++){ uint8_t ch=MR8(np2+k2);
                            if (ch<32||ch>126){ nm3[k2]=0; break; } nm3[k2]=(char)ch; nm3[k2+1]=0; }
                    fprintf(stderr,"    [0x%08x] %-24s kapasite=%u bos=%u eleman=%u"
                            "  (+08=%u +10=%u +18=%u +1c=%u)\n",
                            known[q], nm3, MR32(po+0x24), MR32(po+0x2c), MR32(po+0x28),
                            MR32(po+8), MR32(po+0x10), MR32(po+0x18), MR32(po+0x1c));
                  } }
                uint32_t n2 = mem_is_committed(0x828538u) ? MR32(0x828538u) : 0;
                int guard2 = 0;
                while (n2 && n2 != 0x828534u && guard2++ < 400){
                    if (!mem_is_committed(n2+0x2f)) break;
                    uint32_t nameptr = MR32(n2+0x0c);
                    uint32_t cap = MR32(n2+0x24), esz = MR32(n2+0x28), fr = MR32(n2+0x2c);
                    char nm2[48]; nm2[0]=0;
                    if (nameptr && mem_is_committed(nameptr))
                        for (int q=0;q<40;q++){ uint8_t ch=MR8(nameptr+q);
                            if (ch<32||ch>126){ nm2[q]=0; break; } nm2[q]=(char)ch; nm2[q+1]=0; }
                    if (cap && cap < 100000u)
                        fprintf(stderr,"    %-28s kapasite=%-6u bos=%-6u eleman=%u%s\n",
                                nm2, cap, fr, esz, (fr==0)?"   <<< TUKENDI":"");
                    n2 = MR32(n2+4);
                }
            }
            if (g_present_no == 100){
                if (ENVC("HASHSCAN")){
                    static const uint32_t want[] = {0x3EE7363Fu, 0x04191C37u};
                    static const char *nmz[] = {"WORLDVIEWPROJECTION","TEXTUREOFFSET"};
                    for (int hz=0; hz<2; hz++){
                        int cnt=0; uint32_t first[6]; int nf2=0;
                        for (uint32_t a=0x10000000u; a<0x10800000u; a+=4){
                            if (!mem_is_committed(a+4)){ a += 0x3FFCu; continue; }
                            if (MR32(a)==want[hz]){ cnt++; if(nf2<6) first[nf2++]=a; }
                        }
                        fprintf(stderr,"  [hash] %-20s 0x%08X -> %d yerde:",
                                nmz[hz], want[hz], cnt);
                        for (int q=0;q<nf2;q++) fprintf(stderr," %08x", first[q]);
                        fprintf(stderr,"\n");
                    }
                }
            }
            /* RESETAT=<kare>: o karede cihaz kaybi uret (test) */
            { const char *ra = ENVC("RESETAT");   /* virgulle birden cok kare */
              int hitr = 0;
              for (const char *q = ra; q && *q; ){ if ((uint32_t)atoi(q) == g_present_no) hitr = 1;
                  while (*q && *q != ',') q++; if (*q) q++; }
              if (hitr){
                  fprintf(stderr,"com: RESETAT: kare %u'de cihaz kaybi\n", g_present_no);
                  com_request_reset(); } }
            r=0;
            if (atomic_load(&g_lost)){ r = 0x88760868u;   /* D3DERR_DEVICELOST */
                if (g_lost_presents++ < 4) fprintf(stderr,"com: Present -> DEVICELOST\n"); }
            }
        else if (MID(StretchRect)){
            /* (this, pSrcSurface, pSrcRect, pDstSurface, pDstRect, Filter)
             * HIC UYGULANMAMISTI (olculdu: oturumda 2261 cagri).  Oyun
             * sahneyi arka tampondan bir dokuya bununla kopyaliyor; son
             * islem (bleach bypass / motion blur / DOF) o dokudan okuyor.
             * Kopya yapilmayinca kaynak bos kaliyor ve birlestirme SIYAH
             * uretiyordu -- yuksek detayda ana menunun tamamen kararmasinin
             * sebebi buydu.
             * Yuzey yerlesimi: +28 = Metal doku kimligi, +40 = koken
             * (1 = arka tampon, 2 = doku yuzeyi, 4 = etkin hedef). */
            uint32_t ss=A(c,1), ds=A(c,3);
            uint32_t stex = (ss && mem_is_committed(ss+31)) ? MR32(ss+28) : 0u;
            uint32_t dtex = (ds && mem_is_committed(ds+31)) ? MR32(ds+28) : 0u;
            if (ENVC("BLITLOG")){
                static int nb2=0;
                if (nb2 < 8){ nb2++;
                    uint32_t so=(ss&&mem_is_committed(ss+43))?MR32(ss+40):0;
                    uint32_t doo=(ds&&mem_is_committed(ds+43))?MR32(ds+40):0;
                    fprintf(stderr,"  [blit] kaynak doku=%u koken=%u (%ux%u) -> hedef doku=%u koken=%u (%ux%u)\n",
                            stex, so, ss?MR32(ss+4):0, ss?MR32(ss+8):0,
                            dtex, doo, ds?MR32(ds+4):0, ds?MR32(ds+8):0); }
            }
            if (metal_available() && (stex || dtex) && stex != dtex){
                void metal_blit(unsigned,unsigned);
                metal_blit(stex, dtex);
            }
            r=0; }
        else if (MID(SetRenderTarget)){
            if (ENVC("CUBELOG")){
                static unsigned n3 = 0;
                uint32_t sf2 = A(c,2);
                if (n3 < 8 && sf2 && mem_is_committed(sf2+43) && MR32(sf2+40)==2u){
                    n3++;
                    fprintf(stderr,"  [kup] SetRenderTarget KUP YUZEYI 0x%08x\n", sf2);
                }
            }
            /* (this, RenderTargetIndex, pRenderTarget) */
            uint32_t idx=A(c,1), surf=A(c,2);
            if (idx==0 && metal_available()){
                uint32_t tid = (surf && mem_is_committed(surf+35)) ? MR32(surf+28) : 0;
                { static uint64_t org[8]; static int shown=0;
                  uint32_t o5 = (surf && mem_is_committed(surf+43)) ? MR32(surf+40) : 7;
                  if (o5 < 8) org[o5]++;
                  if (ENVC("RTLOG") && !shown && (org[0]+org[1]+org[2]+org[3]+org[4]+org[7]) % 400 == 0){
                    fprintf(stderr,"  [RT] koken sayaci: yok=%llu backbuf=%llu yuzeyLvl=%llu"
                            " olusturulan=%llu getRT=%llu bilinmeyen=%llu\n",
                            (unsigned long long)org[0],(unsigned long long)org[1],
                            (unsigned long long)org[2],(unsigned long long)org[3],
                            (unsigned long long)org[4],(unsigned long long)org[7]); } }
                uint32_t face = (surf && mem_is_committed(surf+47)) ? MR32(surf+44) : 0u;
                g_cur_rt = tid;
                metal_set_render_target(tid | ((face & 7u) << 16));
                if (tid) g_rt_set++; else g_rt_screen++;
            }
            r=0; }
        else if (MID(SetTransform)){
            static int nt=0;
            uint32_t ty=A(c,1), mp=A(c,2);
            /* XFORMAT=<kare>: yalnizca o karedeki TUM SetTransform cagrilari. */
            { static long xa = -2; if (xa == -2){ const char *e = ENVC("XFORMAT"); xa = e ? atol(e) : -1; }
              if (xa >= 0 && (long)g_present_no == xa && mp && mem_is_committed(mp+63)){
                const char *tn = ty==2?"VIEW":(ty==3?"PROJECTION":(ty==256?"WORLD":"?"));
                fprintf(stderr,"  [xf@%ld] tip=%u(%s) cagiran=0x%08x\n", xa, ty, tn, c->eip);
                for (int rw=0; rw<4; rw++)
                    fprintf(stderr,"      %10.4f %10.4f %10.4f %10.4f\n",
                            MRF32(mp+rw*16), MRF32(mp+rw*16+4),
                            MRF32(mp+rw*16+8), MRF32(mp+rw*16+12)); } }
            if (ENVC("XFORM") && nt<9 && mp && mem_is_committed(mp+63)){ nt++;
                const char *tn = ty==2?"VIEW":(ty==3?"PROJECTION":(ty==256?"WORLD":"?"));
                fprintf(stderr,"  [SetTransform] tip=%u(%s)\n", ty, tn);
                for (int rw=0; rw<4; rw++)
                    fprintf(stderr,"      %10.4f %10.4f %10.4f %10.4f\n",
                            MRF32(mp+rw*16), MRF32(mp+rw*16+4),
                            MRF32(mp+rw*16+8), MRF32(mp+rw*16+12));
            }
            r=0; }
        else if (MID(SetViewport)){
            static int nv2=0;
            uint32_t vp2=A(c,1);
            if (ENVC("XFORM") && nv2<6 && vp2 && mem_is_committed(vp2+23)){ nv2++;
                fprintf(stderr,"  [SetViewport] x=%u y=%u g=%u y=%u minZ=%.3f maxZ=%.3f\n",
                        MR32(vp2), MR32(vp2+4), MR32(vp2+8), MR32(vp2+12),
                        MRF32(vp2+16), MRF32(vp2+20));
            }
            r=0; }
        else if (MID(SetVertexShaderConstantI)){
            uint32_t sr=A(c,1), pd=A(c,2), n=A(c,3);
            for (uint32_t k=0; k<n && sr+k<16; k++){
                uint32_t o=pd+16u*k;
                if (!mem_is_committed(o+15)) break;
                for (int j2=0;j2<4;j2++) g_vsk.i[sr+k][j2]=(int32_t)MR32(o+4u*j2);
            }
            r=0; }
        else if (MID(SetVertexShaderConstantB)){
            uint32_t sr=A(c,1), pd=A(c,2), n=A(c,3);
            for (uint32_t k=0; k<n && sr+k<16; k++){
                uint32_t o=pd+4u*k;
                if (!mem_is_committed(o+3)) break;
                g_vsk.b[sr+k]=(int32_t)MR32(o);
            }
            r=0; }
        else if (MID(SetVertexDeclaration)){ g_cur_decl=A(c,1); g_raw_decl=g_cur_decl; r=0; }
        else if (MID(Reset)){
            /* (this, D3DPRESENT_PARAMETERS*)
             * Oyun cozunurluk/grafik detayi degistirince cihazi sifirlar.
             * Bu metot HIC uygulanmamisti; ayar degistirmek cokuyordu. */
            uint32_t pp3=A(c,1);
            if (pp3 && mem_is_committed(pp3+7)){
                uint32_t w3=MR32(pp3), h3=MR32(pp3+4);
                fprintf(stderr,"  [D3D] Reset: %ux%u\n", w3, h3);
                if (w3 > 8192u || h3 > 8192u) w3 = h3 = 0;   /* cop boyut */
                /* CreateDevice ile AYNI kural uygulanmali: eskiden Reset
                 * ham degeri geciriyordu, yani oyun icinden cozunurluk
                 * degistirince goruntu yeniden bulaniklasiyordu. */
                if (w3 && h3){ uint32_t d3w = w3, d3h = h3;
                    res_pick(w3, h3, &d3w, &d3h); w3 = d3w; h3 = d3h; }
                if (w3 && h3){ g_bb_w = w3; g_bb_h = h3; }
                if (w3 && h3){
                    void metal_want_size(unsigned,unsigned); metal_want_size(w3,h3);
                    if (metal_available()){ void metal_resize(unsigned,unsigned);
                                            metal_resize(w3,h3); } }
            }
            g_resets++;
            /* Oyun Reset'ten once butun render hedeflerini birakmis olmali
             * (D3D9 kurali); Metal tarafinda da birak, yoksa her cozunurluk
             * degisimi hepsini sizdiriyordu. */
            if (metal_available()){ void metal_reset_rts(void); metal_reset_rts(); }
            if (atomic_exchange(&g_lost, 0))
                fprintf(stderr,"com: cihaz kaybi Reset ile kapandi (TCL %lu cagri)\n", g_tcl_n);
            /* Reset sonrasi durum varsayilana doner.  D3D9: hedef 0 geri
             * tampona, Z yuzeyi ORTUK Z'ye (EnableAutoDepthStencil) doner ve
             * oyun buna guvenip ikisini bir daha kurmuyor.  Eskiden Metal
             * tarafinda son hedef/Z oldugu gibi kaliyordu.  Tam ekran gecisi
             * cihaz kaybi yolundan Reset'i KARE ORTASINDA yakaliyor: o an
             * doku 7'ye Z'siz (NULL) cizim yapiliyordu (olculdu).  Sonrasinda
             * ana sahne Z testi olmadan ciziliyordu -> araba ici disina donuk,
             * zemin yansimasi kara.  Eski davranis: NORTRESET=1 (3/3 bozuk). */
            if (g_cur_zid != 1u)
                fprintf(stderr, "com: Reset: bagli Z yuzeyi %u -> ortuk Z\n", g_cur_zid);
            /* D3D9 Reset TUM cihaz durumunu (render/sampler/asama durumlari)
             * varsayilana dondurur ve oyun buna guveniyor: kendi durum
             * golgesini de varsayilana cekip "zaten oyle" sandigi durumlari
             * bir daha yazmiyor.  Biz eski degerleri tutuyorduk (Reset
             * kare ortasina denk gelirse o anki gecisin durumu sizardi).
             * Olculen kosularda gorunur fark yok; D3D9 kurali geregi. */
            if (!ENVC("NORSRESET")){
                memset(g_rs, 0, sizeof g_rs); g_rs_init = 0; g_rs_dirty = 1;
                memset(g_smp_raw, 0, sizeof g_smp_raw);
                memset(g_tss_raw, 0, sizeof g_tss_raw);
                for (int q7 = 0; q7 < 8; q7++) g_tss_raw[q7][11] = (uint32_t)q7;
                if (metal_available()){ rs_apply(); g_rs_dirty = 0; }
            }
            if (g_cur_rt)
                fprintf(stderr, "com: Reset: bagli render hedefi doku %u -> geri tampon\n", g_cur_rt);
            if (!ENVC("NORTRESET")){
            g_cur_zid = 1;
            if (metal_available()){ void metal_set_zsurf(unsigned); metal_set_zsurf(1);
                                    metal_set_render_target(0); } }
            g_cur_tex = 0; g_cur_rt = 0; g_cur_ps_id = 0;
            for (int q6=0;q6<4;q6++) g_tex_stage[q6]=0;
            r=0; }
        else if (MID(SetFVF)){ g_cur_fvf=A(c,1); g_raw_fvf=g_cur_fvf; r=0; }
        else if (MID(CreateTexture)){
            /* (this,W,H,Levels,Usage,Format,Pool,ppTex,pShared) */
            uint32_t w=A(c,1), h=A(c,2), lev=A(c,3), usage=A(c,4), fmt=A(c,5), pool=A(c,6);
            /* NFSRES etkinken oyun son-islem hedeflerini KENDI sandigi
             * cozunurlukte yaratiyor (1280x1024).  Sahne 1920x1080'de
             * ciziliyor, sonra o kucuk hedefe kopyalanip geri buyutuluyor:
             * hem cozunurluk kaybi hem de olcekli blit.  Tam ekran
             * hedefleri cihaz boyutuna cekince zincir bastan sona 1:1. */
            if ((usage & 1u) && g_req_w && w == g_req_w && h == g_req_h){
                static int sr4 = 0;
                if (sr4 < 3){ sr4++;
                    fprintf(stderr,"  [RT-doku] %ux%u -> %ux%u (NFSRES)\n", w,h,g_bb_w,g_bb_h); }
                w = g_bb_w; h = g_bb_h;
            }
            uint32_t pp=A(c,7); uint32_t t=com_new(IF_TEX,64);
            MR32(t+4)=w; MR32(t+8)=h; MR32(t+20)=fmt; MR32(t+24)=lev?lev:1u;
            /* HD build: yansima hedefleri Metal'de buyur, oyun eski boyutu
             * gormeye devam eder (orijinal build'de derlenmez). */
            uint32_t mw = w, mh = h, mlev = lev ? lev : 1u;
            int ekind = (usage & 1u) ? enh_rt_size(c->eip, g_bb_w, g_bb_h, &mw, &mh) : ENH_NONE;
            /* Yol yansimasi: bulaniklik gecisi icin tam mip zinciri (bkz. enhance.c). */
            if (ekind == ENH_ROAD){ mlev = 1u; for (uint32_t q = mw > mh ? mw : mh; q > 1u; q >>= 1) mlev++; }
            if (metal_available()){
                /* D3DUSAGE_RENDERTARGET = 1 */
                uint32_t tid = metal_create_texture_ex(mw,mh,mlev,fmt,(usage & 1u)?1u:0u);
                MR32(t+28)=tid;
                if (ekind == ENH_ROAD && tid){ void metal_hd_mark_road(unsigned,unsigned,unsigned);
                                               metal_hd_mark_road(tid, w, h); }
                if ((usage & 1u) && ENVC("RTIDLOG"))
                    fprintf(stderr,"  [RT-id] %ux%u (metal %ux%u) id=%u cagiran=0x%08x\n",
                            w, h, mw, mh, tid, c->eip);
                if (usage & 1u){ g_rt_tex++;
                    static int sr3=0;
                    if (sr3<6){ sr3++;
                        fprintf(stderr,"  [RT-doku] %ux%u fmt=%u seviye=%u -> id=%u\n",
                                w,h,fmt,lev?lev:1u,tid); } }
                if (!tid){ g_tex_fail++;
                    static uint32_t bad[16]; static int nb=0; int seen=0;
                    for (int q=0;q<nb;q++) if (bad[q]==fmt) seen=1;
                    if (!seen && nb<16){ bad[nb++]=fmt;
                        fprintf(stderr,"  [doku] DESTEKLENMEYEN format %u (0x%x) %ux%u usage=0x%x\n",
                                fmt, fmt, w, h, usage); }
                }
            }
            { static uint64_t fmt_hist[64]; static int sh=0;
              /* D3DFMT: 21=A8R8G8B8 22=X8R8G8B8 23=R5G6B5 25=A1R5G5B5 26=A4R4G4B4
                 827611204='DXT1' 861165636='DXT3' 894720068='DXT5' */
              const char *fn2 = "?";
              switch(fmt){ case 21: fn2="A8R8G8B8"; break; case 22: fn2="X8R8G8B8"; break;
                           case 23: fn2="R5G6B5"; break; case 25: fn2="A1R5G5B5"; break;
                           case 26: fn2="A4R4G4B4"; break; case 28: fn2="A8"; break;
                           case 0x31545844: fn2="DXT1"; break; case 0x33545844: fn2="DXT3"; break;
                           case 0x35545844: fn2="DXT5"; break; case 41: fn2="L8"; break; }
              if (fmt<64) fmt_hist[fmt]++;
              if (VB() && sh<12){ sh++;
                fprintf(stderr,"  [doku] %ux%u lev=%u usage=0x%x fmt=%u(%s) pool=%u\n",
                        w,h,lev,usage,fmt,fn2,pool); } }
            if(pp) MR32(pp)=t; r=0; }
        else if (MID(CreateCubeTexture)){
            /* (this, EdgeLength, Levels, Usage, Format, Pool, ppCubeTexture, pShared)
             * Once bos bir COM nesnesi donuluyordu: boyut/format yok, Metal
             * dokusu yok -> yansima yolunda cop deger okunuyordu. */
            uint32_t edge=A(c,1), lev=A(c,2), usage=A(c,3), fmt=A(c,4), pp=A(c,6);
            uint32_t t2=com_new(IF_TEX,160);        /* yuz tamponlari icin genis */
            if (!edge || edge > 4096u) edge = 256u;
            MR32(t2+4)=edge; MR32(t2+8)=edge; MR32(t2+20)=fmt?fmt:22u;
            MR32(t2+24)=lev?lev:1u;
            uint32_t medge = edge, medge2 = edge;
            if (usage & 1u) enh_rt_size(c->eip, g_bb_w, g_bb_h, &medge, &medge2);
            if (metal_available()){
                unsigned metal_create_texture_cube(unsigned,unsigned,unsigned,unsigned);
                /* Kup doku HER ZAMAN render hedefi: oyun cevre haritasini
                 * alti yuze cizip sonra ornekliyor. */
                MR32(t2+28)= getenv("NOCUBE")
                    ? metal_create_texture_ex(medge,medge,lev?lev:1u,fmt?fmt:22u,1u)
                    : metal_create_texture_cube(medge,lev?lev:1u,fmt?fmt:22u,1u);
            }
            cube_mark(t2);                          /* KUP DOKU kaydi */
            if(pp) MR32(pp)=t2; r=0; }
        else if (MID(GetCubeMapSurface)){
            /* (this, FaceType, Level, ppCubeMapSurface) */
            uint32_t self2=A(c,0), pp=A(c,3); uint32_t sf=com_new(IF_SURF,64);
            MR32(sf+40)=2;
            /* +44: YUZ indeksi + 1 (0 = kup yuzeyi degil).  SetRenderTarget
             * bunu hedef alanina paketliyor; yoksa alti yuz ust uste ayni
             * dokuya cizilir. */
            { uint32_t face = A(c,1); MR32(sf+44) = (face < 6u) ? face + 1u : 1u; }
            if (self2 && mem_is_committed(self2+31)){
                MR32(sf+4)=MR32(self2+4); MR32(sf+8)=MR32(self2+8);
                MR32(sf+20)=MR32(self2+20); MR32(sf+28)=MR32(self2+28);
            }
            if(pp) MR32(pp)=sf; r=0; }
        else if (MID(CreateVolumeTexture)){ uint32_t pp=A(c,8); if(pp) MR32(pp)=com_new(IF_TEX,64); r=0; }
        else if (MID(CreateVertexBuffer)){
            uint32_t pp=A(c,5); uint32_t b=com_new(IF_VB,64);
            MR32(b+4)=A(c,1); MR32(b+8)=gbuf_alloc(A(c,1)?A(c,1):64);
            if(pp) MR32(pp)=b; r=0; }
        else if (MID(CreateIndexBuffer)){
            uint32_t pp=A(c,5); uint32_t b=com_new(IF_IB,64);
            MR32(b+4)=A(c,1); MR32(b+8)=gbuf_alloc(A(c,1)?A(c,1):64);
            MR32(b+12)=A(c,3);              /* Format: 101=16bit, 102=32bit */
            if(pp) MR32(pp)=b; r=0; }
        else if (MID(CreateRenderTarget)){
            /* (this,W,H,Format,MultiSample,MSQuality,Lockable,ppSurface,pShared) */
            uint32_t w2=A(c,1), h2=A(c,2), f2=A(c,3), pp=A(c,7);
            uint32_t sf=com_new(IF_SURF,64);
            MR32(sf+4)=w2; MR32(sf+8)=h2; MR32(sf+20)=f2; MR32(sf+40)=3;
            if (metal_available()) MR32(sf+28)=metal_create_texture_ex(w2,h2,1,f2,1);
            if(pp) MR32(pp)=sf; r=0; }
        else if (MID(CreateDepthStencilSurface)){
            uint32_t pp=A(c,7); uint32_t sf=com_new(IF_SURF,64);
            MR32(sf+4)=A(c,1); MR32(sf+8)=A(c,2); MR32(sf+20)=A(c,3); MR32(sf+40)=5;
            /* Z yuzeyi kimligi (metal.m g_ztex): 1 ortuk, >=2 yaratilan. */
            /* Reset'te oyun yuzeyleri yeniden yaratir; tablo dolunca basa
             * sar (eskiler o zamana birakilmis olur), dokuyu sifirlat. */
            static uint32_t next_zid = 2;
            if (next_zid >= 64u) next_zid = 2;
            uint32_t zid = next_zid++;
            MR32(sf+52) = zid; MR32(sf+56) = ZSURF_MAGIC;
            if (metal_available()){ void metal_z_forget(unsigned); metal_z_forget(zid); }
            if(pp) MR32(pp)=sf; r=0; }
        else if (MID(CreateOffscreenPlainSurface)){ uint32_t pp=A(c,5); if(pp) MR32(pp)=com_new(IF_SURF,64); r=0; }
        else if (MID(GetBackBuffer)){ uint32_t pp=A(c,4); uint32_t sf=com_new(IF_SURF,64);
            MR32(sf+4)=g_bb_w; MR32(sf+8)=g_bb_h; MR32(sf+20)=22u;
            MR32(sf+40)=1; if(pp) MR32(pp)=sf; r=0; }
        else if (MID(GetRenderTarget)||MID(GetDepthStencilSurface)){
            /* Bu yuzeyler de gercek geri tampon boyutunu tasimali; yoksa
             * GetDesc 0 dondurur. */
            uint32_t pp=A(c, nargs-1); uint32_t sf=com_new(IF_SURF,64);
            MR32(sf+40)=4; MR32(sf+28)=g_cur_rt;   /* etkin hedefi geri ver */
            MR32(sf+4)=g_bb_w; MR32(sf+8)=g_bb_h; MR32(sf+20)=22u;
            if (MID(GetDepthStencilSurface)){
                /* etkin Z yuzeyini geri ver (oyun sonra SetDepthStencilSurface
                 * ile geri bagliyor) */
                MR32(sf+28)=0; MR32(sf+20)=80u;
                MR32(sf+52)=g_cur_zid; MR32(sf+56)=ZSURF_MAGIC;
            }
            if(pp) MR32(pp)=sf; r=0; }
        else if (MID(SetDepthStencilSurface)){
            /* (this, pNewZStencil).  NULL -> Z testi/yazmasi etkisiz. */
            uint32_t sf=A(c,1), zid=0;
            if (sf){
                zid = 1;   /* taninmayan yuzey: ortuk */
                if (mem_is_committed(sf+59) && MR32(sf+56)==ZSURF_MAGIC) zid = MR32(sf+52);
            }
            g_cur_zid = zid;
            if (metal_available()){ void metal_set_zsurf(unsigned); metal_set_zsurf(zid); }
            r=0; }
        else if (MID(CreateVertexDeclaration)){
            uint32_t el=A(c,1), pp=A(c,2);
            { static int sh=0;
              if (VB() && sh<16 && el){ sh++;
                static const char *TY[]={"FLOAT1","FLOAT2","FLOAT3","FLOAT4","D3DCOLOR",
                    "UBYTE4","SHORT2","SHORT4","UBYTE4N","SHORT2N","SHORT4N","USHORT2N",
                    "USHORT4N","UDEC3","DEC3N","FLOAT16_2","FLOAT16_4","UNUSED"};
                static const char *US[]={"POSITION","BLENDWEIGHT","BLENDINDICES","NORMAL",
                    "PSIZE","TEXCOORD","TANGENT","BINORMAL","TESSFACTOR","POSITIONT",
                    "COLOR","FOG","DEPTH","SAMPLE"};
                fprintf(stderr,"  [vdecl] bildirim:\n");
                for (uint32_t k=0;k<32;k++){
                    uint32_t e=el+8u*k;
                    if (!mem_is_committed(e+7)) break;
                    uint32_t st=MR16(e), of=MR16(e+2);
                    uint32_t ty=MR8(e+4), me=MR8(e+5), us=MR8(e+6), ui=MR8(e+7);
                    if (st==0xFFu && ty==17u){ fprintf(stderr,"     [%u] END\n",k); break; }
                    fprintf(stderr,"     [%u] akis=%u ofset=%-3u tip=%-8s kullanim=%-10s idx=%u (yontem=%u)\n",
                            k, st, of, ty<18?TY[ty]:"?", us<14?US[us]:"?", ui, me);
                }
              } }
            uint32_t h=com_new(IF_VDECL,32);
            uint32_t n=0;
            if (el){
                for(; n<64; n++){
                    uint32_t e=el+8u*n;
                    if (!mem_is_committed(e+7)) break;
                    if (MR16(e)==0xFFu && MR8(e+4)==17u){ n++; break; }
                }
            }
            uint32_t bytes = 8u*n;
            uint32_t copy = bytes ? gbuf_alloc(bytes) : 0;
            if (copy && el) memcpy(g_mem+copy, g_mem+el, bytes);
            MR32(h+4)=copy; MR32(h+8)=bytes;
            if(pp) MR32(pp)=h; r=0; }
        else if (MID(CreateVertexShader)||MID(CreatePixelShader)){
            uint32_t fn=A(c,1), pp=A(c,2);
            uint32_t h=com_new(IF_VSHADER,32);
            /* shader token akisini tara: 0x0000FFFF END token'ina kadar */
            uint32_t sz=0;
            if (fn){ uint32_t k=0;
                for (; k<16384; k++){
                    if (!mem_is_committed(fn+4u*k)) break;
                    uint32_t t=MR32(fn+4u*k); if(t==0x0000FFFFu){ k++; break; }
                }
                sz = 4u*k; }
            uint32_t copy = sz ? gbuf_alloc(sz) : 0;
            if (copy && fn) memcpy(g_mem+copy, g_mem+fn, sz);
            MR32(h+4)=copy; MR32(h+8)=sz; MR32(h+12)=0;
            /* PSDUMP: piksel gölgelendiricileri de ayni token bicimini
             * kullaniyor; derleyip sokumleyerek NE yaptiklarini gor. */
            if (MID(CreatePixelShader) && copy && sz >= 8 && ENVC("PSDUMP")){
                static int pn = 0;
                if (pn < atoi(ENVC("PSDUMP"))){
                    VSProg *pp2 = vs_compile((const uint32_t *)(g_mem+copy), sz/4u);
                    if (pp2 && vs_ninstr(pp2) < 5u){ pp2 = 0; }   /* basit UI'yi atla */
                    if (pp2){ pn++;
                        fprintf(stderr,"  --- PIKSEL GOLGELENDIRICI #%d (%u komut) ---\n",
                                pn, vs_ninstr(pp2));
                        vs_disasm(pp2, stderr);
                        { static char msl[16384];
                          int len = ps_to_msl(pp2, pn, msl, sizeof msl);
                          if (len > 0) fprintf(stderr, "--- URETILEN MSL ---\n%s\n", msl);
                          else fprintf(stderr, "--- MSL uretilemedi ---\n"); }
                    }
                }
            }
            /* Piksel gölgelendiricisini MSL'ye cevirip Metal'e kaydet:
             * oyunun kendi gölgelendiricisi GPU'da AYNEN calisir. */
            if (MID(CreatePixelShader) && copy && sz >= 8){
                VSProg *pp3 = vs_compile((const uint32_t *)(g_mem+copy), sz/4u);
                if (pp3){
                    static char msl[16384];
                    int id = ++g_psn;
                    void metal_register_ps(int, const char *);
                    if (ps_to_msl(pp3, id, msl, sizeof msl) > 0){
                        metal_register_ps(id, msl);
                        MR32(h+12) = (uint32_t)id;
                    }
                    /* KUP VARYANTI (id + PSCUBE): D3D9'da ps_1_x doku
                     * TIPINI bildirmez, tip baglamadan gelir.  Araba cevre
                     * haritasi asama 0'a kup doku bagliyor; ayni yuvaya
                     * texture2d bekleyen bir gölgelendirici baglanirsa
                     * Metal cizimi reddediyor (validateCommonDrawErrors).
                     * Cizim aninda bagli dokuya gore varyant seciliyor. */
                    if (id < PSCUBE &&
                        ps_to_msl_ex(pp3, id + PSCUBE, msl, sizeof msl, 1u) > 0)
                        metal_register_ps(id + PSCUBE, msl);
#if NFS_ENHANCED
                    /* HD: yansima ayristirma varyanti cizim aninda, ihtiyac
                     * olunca uretilir (com_ps_refl_variant); program saklanir. */
                    if (id < PSCUBE && !g_psprog[id]) g_psprog[id] = pp3;
                    else
#endif
                    vs_free(pp3);
                }
            }
            if (MID(CreateVertexShader) && copy && sz >= 8 && g_vsprogn < MAXVS){
                VSProg *vp2 = vs_compile((const uint32_t *)(g_mem+copy), sz/4u);
                if (vp2){ g_vsprog[g_vsprogn] = vp2; MR32(h+12) = g_vsprogn++;
                    if (ENVC("VSDUMP")){
                        static int vd2 = 0;
                        if (vd2 < atoi(ENVC("VSDUMP")) && vs_ninstr(vp2) >= 10u){ vd2++;
                            static int dcl[16][2];
                            for (int q7=0;q7<16;q7++){ dcl[q7][0]=-1; dcl[q7][1]=0; }
                            dcl[0][0]=0;  dcl[0][1]=2;    /* pos  FLOAT3 @0  */
                            dcl[1][0]=12; dcl[1][1]=4;    /* renk D3DCOLOR   */
                            dcl[2][0]=16; dcl[2][1]=1;    /* uv   FLOAT2     */
                            static char msl2[24576];
                            int L2 = vs_to_msl(vp2, (int)g_vsprogn, dcl, msl2, sizeof msl2);
                            fprintf(stderr,"--- KOSE GOLGELENDIRICI #%u (%u komut) MSL ---\n%s\n",
                                    g_vsprogn, vs_ninstr(vp2), L2>0?msl2:"(uretilemedi)");
                        }
                    }
                }
            }
            if(pp) MR32(pp)=h; r=0; }
        else if (MID(BeginStateBlock)){
            /* D3D9: ic ice Begin GECERSIZDIR (ikincisi D3DERR_INVALIDCALL
             * doner).  Oyun bunu yapiyor (izde 845 ve 846 pes pese);
             * ikinciyi yeni blok saymak disaridaki blogu kaybettiriyordu. */
            if (g_sb_rec) r=0x8876086Cu;          /* D3DERR_INVALIDCALL */
            else {
                int slot = 0;
                for (int q = 1; q < g_sbn; q++) if (!g_sb[q].used){ slot = q; break; }
                if (!slot && g_sbn < MAXSB) slot = g_sbn++;
                if (slot){ g_sb_rec = slot; g_sb[slot].n = 0; g_sb[slot].used = 1; }
                else { g_sb_rec = 0; g_sb_full++; }
                r=0;
            } }
        else if (MID(CreateStateBlock)){
            /* Onceden tanimli kume (D3DSBT_ALL vb.) yakalar.  Izde hic
             * kullanilmiyor; bos blok birakiyoruz. */
            uint32_t pp=A(c,2);
            if(pp){ uint32_t o=com_new(IF_STATEBLOCK,32); MR32(o+8)=0; MR32(pp)=o; }
            r=0; }
        else if (MID(CreateQuery)){ uint32_t pp=A(c,2); if(pp) MR32(pp)=com_new(IF_QUERY,32); r=0; }
        else if (MID(EndStateBlock)){
            uint32_t pp=A(c,1);
            uint32_t o=com_new(IF_STATEBLOCK,32);
            MR32(o+8) = (uint32_t)g_sb_rec;         /* blok indeksi */
            g_sb_rec = 0;
            if(pp) MR32(pp)=o;
            r=0; }
        else if (MID(CreateAdditionalSwapChain)){ uint32_t pp=A(c,2); if(pp) MR32(pp)=com_new(IF_SWAPCHAIN,32); r=0; }
        else if (MID(GetSwapChain)){ uint32_t pp=A(c,2); if(pp) MR32(pp)=com_new(IF_SWAPCHAIN,32); r=0; }
        else if (MID(GetNumberOfSwapChains)) r=1;
        else if (!strncmp(nm,"Get",3) && nargs>=2){
            /* cikis parametresini guvenli varsayilanla doldur */
            uint32_t p=A(c,nargs-1); uint32_t sz=4;
            if      (MID(GetTransform))           sz=64;
            else if (MID(GetMaterial))            sz=68;
            else if (MID(GetLight))               sz=104;
            else if (MID(GetViewport))            sz=24;
            else if (MID(GetScissorRect))         sz=16;
            else if (MID(GetClipStatus))          sz=8;
            else if (MID(GetCreationParameters))  sz=16;
            else if (MID(GetRasterStatus))        sz=8;
            else if (MID(GetGammaRamp))           sz=1536;
            else if (MID(GetClipPlane))           sz=16;
            else if (!strncmp(nm,"GetVertexShaderConstant",23)
                  || !strncmp(nm,"GetPixelShaderConstant",22)) sz=16*4;
            if (p && p < 0xE0000000u){ mem_commit(p,sz); memset(g_mem+p,0,sz); }
            if (MID(GetViewport) && p){ MR32(p+8)=1280; MR32(p+12)=720; MRF32(p+20)=1.0f; }
            r=0; }
        else r=0;                        /* geri kalan Set* -> D3D_OK */
    }
    /* --- kaynaklar --- */
    else if (iface==IF_TEX || iface==IF_VB || iface==IF_IB || iface==IF_SURF){
        uint32_t self=A(c,0);
        if      (MID(GetDevice)){ uint32_t p=A(c,1); if(p) MR32(p)=g_dev; r=0; }
        else if (MID(Lock)){
            uint32_t off=A(c,1), sz=A(c,2), pp=A(c,3);
            uint32_t base = self?MR32(self+8):0;
            if(!base){ base=gbuf_alloc(sz?sz:65536); if(self) MR32(self+8)=base; }
            if(pp) MR32(pp)=base+off; r=0; }
        else if (MID(LockRect)){
            /* 2B doku : (Level, pLockedRect, pRect, Flags)
             * KUP doku : (FaceType, Level, pLockedRect, pRect, Flags)
             * Yuzey    : (pLockedRect, pRect, Flags)
             * Kup imzasi 2B gibi ele alinirsa pLockedRect diye Level
             * okunur; oyunun gercek yapisi DOLDURULMAZ ve cop isaretci
             * ile cokme olur (olculdu: guest adres 0x400). */
            int is_cube = (iface==IF_TEX && is_cube_tex(self));
            uint32_t face  = is_cube ? A(c,1) : 0u;
            uint32_t level = is_cube ? A(c,2) : ((iface==IF_TEX) ? A(c,1) : 0u);
            uint32_t plr   = is_cube ? A(c,3) : ((iface==IF_TEX) ? A(c,2) : A(c,1));
            uint32_t w = self?MR32(self+4):256, h = self?MR32(self+8):256;
            uint32_t fmt = self?MR32(self+20):21u;
            if(!w) w=256; if(!h) h=256;
            if(w>8192) w=8192; if(h>8192) h=8192;
            if(level>7) level=7;
            uint32_t pitch = metal_available() ? metal_level_pitch(w,level,fmt) : 0;
            uint32_t rows  = metal_available() ? metal_level_rows(h,level,fmt)  : 0;
            if(!pitch){ uint32_t lw=w>>level; if(!lw)lw=1; pitch=lw*4u; }
            if(!rows){  uint32_t lh=h>>level; if(!lh)lh=1; rows=lh; }
            uint32_t slot = self ? (is_cube ? (self + 104u + 4u*(face & 7u))
                                            : (self + 32u + 4u*level)) : 0;
            uint32_t buf = slot ? MR32(slot) : 0;
            if(!buf){ buf = gbuf_alloc(pitch*rows + 64u); if(slot) MR32(slot)=buf; }
            if(plr){ mem_commit(plr,8); MR32(plr)=pitch; MR32(plr+4)=buf; }
            r=0; }
        else if (MID(UnlockRect)){
            /* 2B: (Level)  |  KUP: (FaceType, Level)  |  Yuzey: () */
            int is_cube2 = (iface==IF_TEX && is_cube_tex(self));
            uint32_t face2 = is_cube2 ? A(c,1) : 0u;
            uint32_t level = is_cube2 ? A(c,2) : ((iface==IF_TEX) ? A(c,1) : 0u);
            if(level>7) level=7;
            uint32_t w = self?MR32(self+4):0, h = self?MR32(self+8):0;
            uint32_t fmt = self?MR32(self+20):21u, mid = self?MR32(self+28):0;
            uint32_t buf = self ? (is_cube2 ? MR32(self + 104u + 4u*(face2 & 7u))
                                            : MR32(self + 32u + 4u*level)) : 0;
            if (metal_available() && mid && buf && w && h)
                metal_upload_texture(mid, level, g_mem+buf,
                                     metal_level_pitch(w,level,fmt), w, h, fmt);
            if (ENVC("LOCKLOG") && w==640 && h==480){
                static int nl=0;
                if (nl<3){ nl++;
                    fprintf(stderr,"  [kilit] %ux%u fmt=%u lev=%u mid=%u adim=%u  ilk 32 bayt:\n    ",
                            w,h,fmt,level,mid, metal_level_pitch(w,level,fmt));
                    for (int q=0;q<32;q++) fprintf(stderr,"%02x ", MR8(buf+q));
                    fprintf(stderr,"\n    ikinci satir: ");
                    uint32_t pt = metal_level_pitch(w,level,fmt);
                    for (int q=0;q<16;q++) fprintf(stderr,"%02x ", MR8(buf+pt+q));
                    fprintf(stderr,"\n");
                }
            }
            r=0; }
        else if (MID(GetLevelCount)) r=(self&&MR32(self+24))?MR32(self+24):1u;
        else if (MID(GetType)) r=(iface==IF_TEX)?3u:(iface==IF_SURF?1u:(iface==IF_VB?4u:5u));
        else if (MID(GetSurfaceLevel)||MID(GetCubeMapSurface)){
            /* Yuzey, ait oldugu dokunun Metal kimligini tasimali; yoksa
             * SetRenderTarget hangi dokuya cizilecegini bilemez.
             * KUP doku ayni vtable yuvasini kullanir ama imzasi farklidir:
             *   2B  : GetSurfaceLevel(Level, ppSurface)
             *   KUP : GetCubeMapSurface(FaceType, Level, ppSurface)
             * 2B gibi ele alinirsa isaretci YANLIS arguman'a yazilir. */
            uint32_t self0=A(c,0);
            int cube3 = is_cube_tex(self0);
            uint32_t pp = cube3 ? A(c,3) : A(c,2);
            uint32_t sf=com_new(IF_SURF,64);
            MR32(sf+40)=2;
            uint32_t self=A(c,0);
            if (self && mem_is_committed(self+31)){
                MR32(sf+4)=MR32(self+4); MR32(sf+8)=MR32(self+8);
                MR32(sf+20)=MR32(self+20); MR32(sf+28)=MR32(self+28);
            }
            if(pp) MR32(pp)=sf; r=0; }
        else if (MID(GetDesc)||MID(GetLevelDesc)){
            /* D3DSURFACE_DESC yerlesimi:
             *   0 Format, 4 Type, 8 Usage, 12 Pool,
             *  16 MultiSampleType, 20 MultiSampleQuality,
             *  24 Width, 28 Height
             * Genislik/yukseklik ONCEDEN 16/20'ye yaziliyordu (yani
             * MultiSample alanlarina) -> oyun boyutu 0 okuyup yuksek
             * detayda cop isaretci hesapliyor ve cokuyordu. */
            uint32_t p=A(c,nargs-1);
            if(p){ mem_commit(p,32); memset(g_mem+p,0,32);
                uint32_t fw = self?MR32(self+4):256, fh = self?MR32(self+8):256;
                uint32_t ff = self?MR32(self+20):0;
                if (!fw) fw = 640; if (!fh) fh = 480;
                if (!ff) ff = 22;                    /* D3DFMT_X8R8G8B8 */
                MR32(p)    = ff;                     /* Format          */
                MR32(p+4)  = (iface==IF_TEX) ? 3u : 1u;  /* Type: TEXTURE/SURFACE */
                MR32(p+8)  = 0;                      /* Usage           */
                MR32(p+12) = 0;                      /* Pool: DEFAULT   */
                MR32(p+16) = 0;                      /* MultiSampleType */
                MR32(p+20) = 0;                      /* MS Quality      */
                MR32(p+24) = fw;                     /* Width           */
                MR32(p+28) = fh;                     /* Height          */
            }
            r=0; }
        else r=0;
    }
    /* --- DirectSound --- */
    else if (iface==IF_DS){
        if      (MID(CreateSoundBuffer)){
            uint32_t desc=A(c,1), pp=A(c,2);
            uint32_t b=com_new(IF_DSBUF,64);
            /* DSBUFFERDESC: dwSize@0 dwFlags@4 dwBufferBytes@8 dwReserved@12
             *               lpwfxFormat@16 guid3DAlgorithm@20
             * DIKKAT: boyut +12'den (dwReserved) okunuyordu -> her zaman 0
             * gelip 65536'ya dusuyordu.  Oyun akis tamponunu KENDI istedigi
             * boyuta gore besler; bizim imlec cok daha hizli turlayinca oyun
             * sarkiyi hizli aktariyor ve yetisemedigi yerde sessizlik
             * yaziyor -- "muzik hizli akiyor ve arada kesiliyor". */
            uint32_t bytes = (desc && mem_is_committed(desc+11)) ? MR32(desc+8) : 0;
            if (ENVC("SNDBUF"))
                fprintf(stderr, "  [ses] CreateSoundBuffer istenen=%u bayt "
                        "(bayrak=%08x, eski-yanlis-alan=%u)\n",
                        bytes, desc?MR32(desc+4):0, desc?MR32(desc+12):0);
            /* DSLOG=1: hangi guest alt sistemi bu tamponu yaratti?  SFX
             * tamponlari (#1-#4) hic calinmadigi icin sahibini bulmak sart. */
            if (ENVC("DSLOG")){
                fprintf(stderr, "  [ses] --- CreateSoundBuffer #%d cagri zinciri ---\n",
                        (int)g_snd_play);
                guest_backtrace(c, 10);
            }
            if(!bytes || bytes>0x400000u) bytes=65536u;
            MR32(b+4)=bytes; MR32(b+8)=gbuf_alloc(bytes); MR32(b+12)=0;
            /* WAVEFORMATEX'ten bayt/saniye: oynatma imleci GERCEKCI ilerlemeli.
             * Video oynatici kareleri ses saatine gore suruyor; imlec sahte
             * olursa oynatma hic bitmiyor. */
            { uint32_t bps = 176400u;                /* varsayilan 44.1k 16-bit stereo */
              uint32_t pfmt = (desc && mem_is_committed(desc+19)) ? MR32(desc+16) : 0;
              if (pfmt && mem_is_committed(pfmt+15)){
                  uint32_t v = MR32(pfmt+8);         /* nAvgBytesPerSec */
                  if (v >= 8000u && v <= 1000000u) bps = v;
              }
              MR32(b+20)=bps; MR32(b+24)=0;
              /* CoreAudio karistiricisina kaydet.  Birincil tampon
               * (DSBCAPS_PRIMARYBUFFER = 0x1) calinmaz. */
              uint32_t flags = (desc && mem_is_committed(desc+7)) ? MR32(desc+4) : 0;
              uint32_t rate = 44100u, chans = 2u, bits = 16u;
              if (pfmt && mem_is_committed(pfmt+15)){
                  uint32_t ch = MR16(pfmt+2), sr = MR32(pfmt+4), bt = MR16(pfmt+14);
                  if (ch>=1u && ch<=2u) chans = ch;
                  if (sr>=4000u && sr<=192000u) rate = sr;
                  if (bt==8u || bt==16u) bits = bt;
              }
              MR32(b+28)=0;
              if (!(flags & 0x1u)){
                  audio_init();
                  MR32(b+28) = (uint32_t)audio_register(MR32(b+8), bytes, rate, chans, bits);
              } }
            if(pp) MR32(pp)=b; r=0; }
        else if (MID(GetCaps)){
            /* DSCAPS: dwSize@0 dwFlags@4 dwMinSecondarySampleRate@8
             *         dwMaxSecondarySampleRate@12 dwPrimaryBuffers@16
             *         dwMaxHwMixingAllBuffers@20 ...
             * Eskiden bayrak yalnizca 0x200 (SECONDARYSTEREO) idi ve
             * dwPrimaryBuffers'a 100000 yaziliyordu.  Oyun ses yeteneklerini
             * buradan ogreniyor. */
            uint32_t p=A(c,1);
            if(p){ uint32_t sz=MR32(p); if(!sz||sz>256u) sz=0x64u; mem_commit(p,sz);
                   memset(g_mem+p,0,sz); MR32(p)=sz;
                   MR32(p+4)=0x00000F1Fu;   /* birincil mono/stereo/8/16 +
                                             * ikincil mono/stereo/8/16 +
                                             * CONTINUOUSRATE */
                   MR32(p+8)=100u; MR32(p+12)=100000u;
                   MR32(p+16)=1u;           /* dwPrimaryBuffers */
                   MR32(p+20)=64; MR32(p+24)=64;
                   MR32(p+28)=64; MR32(p+32)=64;
                   MR32(p+36)=0x7FFFFFFFu;  /* dwTotalHwMemBytes  */
                   MR32(p+40)=0x7FFFFFFFu;  /* dwFreeHwMemBytes   */
                   MR32(p+44)=0x7FFFFFFFu;
                   MR32(p+48)=64; MR32(p+52)=64; MR32(p+56)=64; }
            r=0; }
        else if (MID(SetCooperativeLevel)) r=0;
        else if (MID(GetSpeakerConfig)){ uint32_t p=A(c,1); if(p){mem_commit(p,4);MR32(p)=0x00000006u;} r=0; }
        else if (MID(DuplicateSoundBuffer)){
            /* (this, pOriginal, ppDuplicate) - kopya ayni PCM verisini
             * paylasir.  Eskiden BOS bir COM nesnesi donuyordu: boyut yok,
             * veri yok, karistiricida sesi yok.  Oyun es zamanli efektleri
             * (motor, menu tiklamalari) boyle caldigi icin HEPSI sessizdi. */
            uint32_t src=A(c,1), pp=A(c,2);
            uint32_t b=com_new(IF_DSBUF,64);
            if (src && mem_is_committed(src+31)){
                MR32(b+4)=MR32(src+4);          /* boyut   */
                MR32(b+8)=MR32(src+8);          /* veri    */
                MR32(b+20)=MR32(src+20);        /* bayt/sn */
                MR32(b+12)=0; MR32(b+16)=0; MR32(b+24)=0;
                int audio_duplicate(int);
                MR32(b+28)=(uint32_t)audio_duplicate((int)MR32(src+28));
            }
            if(pp) MR32(pp)=b; r=0; }
        else r=0;
    }
    else if (iface==IF_DSBUF){
        uint32_t self=A(c,0);
        uint32_t sz = self?MR32(self+4):65536u, buf = self?MR32(self+8):0;
        if      (MID(Lock)){
            /* Hangi tampon nesnesi kilitleniyor?  SFX olaylari tampon
             * kilitliyor ama duyulmuyor; kaydedilmemis (birincil ya da
             * rapor disi) bir tampona yaziliyor olabilir. */
            /* Hangi IS PARCACIGI kilitliyor?  Akis (muzik) ayri bir
             * thread'te; SFX olaylari ANA thread'ten geliyor.  Ikisini
             * ayirmak, SFX verisinin gercekten yazilip yazilmadigini
             * gostermenin en dogrudan yolu. */
            { uint32_t tid = (c->fs_base && mem_is_committed(c->fs_base+0x24u))
                             ? MR32(c->fs_base+0x24u) : 0u;
              static uint32_t tl[8]; static unsigned tc[8]; static uint64_t tb[8];
              int ti=-1; for(int q=0;q<8;q++) if(tl[q]==tid+1u) ti=q;
              if(ti<0) for(int q=0;q<8;q++) if(!tl[q]){ tl[q]=tid+1u; ti=q; break; }
              if(ti>=0){ tc[ti]++; tb[ti]+=A(c,2); }
              g_lk_tid=tl; g_lk_cnt=tc; g_lk_byt=tb; }
            { static uint32_t obj[16]; static unsigned cnt[16]; static int nobj=0;
              static int reg_atexit=0;
              int fi=-1; for(int q=0;q<nobj;q++) if(obj[q]==self) fi=q;
              if(fi<0 && nobj<16){ fi=nobj++; obj[fi]=self; cnt[fi]=0; }
              if(fi>=0) cnt[fi]++;
              (void)reg_atexit;
              g_lock_obj = obj; g_lock_cnt = cnt; g_lock_n = nobj; }
            if (ENVC("DSLOG")){ static int nl3=0;
                if (nl3 < 2){ nl3++;
                    fprintf(stderr,"  [ses] --- Lock cagri zinciri (tampon %u) ---\n",
                            self?MR32(self+28):0);
                    guest_backtrace(c, 12); } }
            /* (this, dwOffset, dwBytes, ppv1, pcb1, ppv2, pcb2, dwFlags)
             *
             * DirectSound tamponu DAIRESELDIR: istenen aralik sonu asarsa
             * IKI bolge dondurulur (sonuna kadar + bastan devami).  Eskiden
             * tek duz bolge veriliyordu; tamponun basina yazilmasi gereken
             * kisim hic yazilmiyor, her sarmada sessiz bosluk olusuyordu. */
            uint32_t off=A(c,1), want=A(c,2), pp1=A(c,3), pn1=A(c,4),
                     pp2=A(c,5), pn2=A(c,6), fl=A(c,7);
            if(!buf){ buf=gbuf_alloc(sz?sz:65536u); if(self) MR32(self+8)=buf; }
            if(!sz) sz = 65536u;
            if (fl & 0x2u){ off = 0; want = sz; }        /* ENTIREBUFFER */
            off %= sz;
            if (!want || want > sz) want = sz;
            uint32_t avail = sz - off;
            uint32_t b1 = (want <= avail) ? want : avail;
            uint32_t b2 = want - b1;
            if (ENVC("SNDLOG")){
                static int nl=0;
                uint32_t sid2 = (self && mem_is_committed(self+31)) ? MR32(self+28) : 0;
                if (nl<40 && sid2){ nl++;
                    uint32_t rp=0, rw=0;
                    void audio_cursors(int, uint32_t*, uint32_t*);
                    audio_cursors((int)sid2, &rp, &rw);
                    fprintf(stderr,"  [snd] kilit ofs=%6u boyut=%6u  |  oynatma=%6u yazma=%6u"
                            "  tampon=%u  ileri=%d\n", off, want, rp, rw, sz,
                            (int)((off + sz - rp) % sz));
                }
            }
            if(pp1){ mem_commit(pp1,4); MR32(pp1)=buf+off; }
            if(pn1){ mem_commit(pn1,4); MR32(pn1)=b1; }
            if(pp2){ mem_commit(pp2,4); MR32(pp2)= b2 ? buf : 0u; }
            if(pn2){ mem_commit(pn2,4); MR32(pn2)= b2; }
            r=0; }
        else if (MID(Unlock)){
            /* VOICELOG=1: oyunun KENDI ses kanali tablosu.
             * sub_00725378 tabloyu [0x8b466c] adres, 0x90 adim, sayi
             * [0x8b460c] (int16) olarak geziyor; +0x26 baytini "kullanimda"
             * olarak sinaviyor.  SFX olaylari kanal aciyor mu, dogrudan
             * olcmenin tek yolu bu. */
            if (ENVC("VOICELOG")){
                static unsigned nu=0; static int peak=0;
                if ((nu++ % 120u) == 0u){
                    uint32_t tb = mem_is_committed(0x8b466cu) ? MR32(0x8b466cu) : 0u;
                    int cnt = mem_is_committed(0x8b460cu) ? (int)(int16_t)MR16(0x8b460cu) : 0;
                    int act = 0;
                    if (tb && cnt > 0 && cnt < 256)
                        for (int q=0;q<cnt;q++){
                            uint32_t e = tb + (uint32_t)q*0x90u;
                            if (mem_is_committed(e+0x26u) && MR8(e+0x26u)) act++;
                        }
                    if (act > peak) peak = act;
                    /* Tablo GENELINDE degisim: yorumum (+0x26) yanlis olsa
                     * bile herhangi bir alan yaziliyor mu, bunu gorur. */
                    unsigned nz = 0, ent_nz = 0;
                    if (tb && cnt > 0 && cnt < 256)
                        for (int q=0;q<cnt;q++){
                            uint32_t e = tb + (uint32_t)q*0x90u; unsigned any=0;
                            if (!mem_is_committed(e+0x8fu)) continue;
                            for (unsigned k=0;k<0x90u;k++) if (MR8(e+k)){ nz++; any=1; }
                            if (any) ent_nz++;
                        }
                    /* Ses seviyesi genelleri: sub_0072d3ec sesi
                     * ((uint16)[0x8b44c6] << 8) * param olarak olcekliyor. */
                    fprintf(stderr,"  [seviye] [0x8b44c6]=%u  [0x8b44c4]=%u  [0x8b44c8]=%u"
                            "  [0x8b44ca]=%u  [0x8b44cc]=%u\n",
                            mem_is_committed(0x8b44c6u)?MR16(0x8b44c6u):0xFFFFu,
                            mem_is_committed(0x8b44c4u)?MR16(0x8b44c4u):0xFFFFu,
                            mem_is_committed(0x8b44c8u)?MR16(0x8b44c8u):0xFFFFu,
                            mem_is_committed(0x8b44cau)?MR16(0x8b44cau):0xFFFFu,
                            mem_is_committed(0x8b44ccu)?MR16(0x8b44ccu):0xFFFFu);
                    fprintf(stderr,"  [kanal] tablo=0x%08x sayi=%d etkin=%d (tepe=%d)"
                            "  sifir-olmayan giris=%u bayt=%u\n",
                            tb, cnt, act, peak, ent_nz, nz);
                }
            }
            /* Oyun bu tampona gercek PCM yaziyor mu?  Yaziyorsa ses motoru
             * (dolayisiyla on yuz mantigi) CALISIYOR demektir. */
            uint32_t p1=A(c,1), n1=A(c,2);
            if (p1 && n1 && n1 < 1u<<22 && mem_is_committed(p1+ (n1>16?15:0))){
                uint32_t nz=0, lim = n1 < 4096u ? n1 : 4096u;
                for (uint32_t q=0;q<lim;q++) if (MR8(p1+q)) nz++;
                if (nz){ g_snd_bytes += n1; g_snd_nonzero++; }
                else      g_snd_zero++;
                { uint32_t sid4 = (self && mem_is_committed(self+31)) ? MR32(self+28) : 0;
                  if (sid4){ void audio_note_write_stat(int, uint32_t, int);
                             audio_note_write_stat((int)sid4, n1, nz ? 1 : 0); } }
                /* SNDZERO: TAMAMEN SIFIR yazilan bolgeleri, cagiran guest
                 * adresiyle birlikte bildir.  Olculdu: oyun tam 30 sn'de bir
                 * ~50-95 ms sessizlik YAZIYOR (tampon ac degil, veri bos). */
                if (ENVC("SNDZERO")){
                    double audio_time(void);
                    double _t = audio_time();
                    /* Baslangictaki tampon temizlemeleri ilgi cekmiyor; asil
                     * olay ilk 20 sn'den SONRA, 30 sn'lik akis blogu
                     * tazelenirken oluyor.  Tum bolgeye bakariz (ilk 4 KB'ye
                     * degil): yazmanin ikinci yarisi bos olabilir. */
                    if (_t > 20.0 && n1 >= 1024u){
                        uint32_t nz2 = 0, lim2 = n1 < 65536u ? n1 : 65536u;
                        for (uint32_t q = 0; q < lim2; q++) if (MR8(p1+q)) nz2++;
                        if (nz2 * 4u < lim2){          /* %75'ten fazlasi sifir */
                            static int nzl = 0;
                            if (nzl < 120){ nzl++;
                                fprintf(stderr, "  [sifir-yazma] t=%.3f sn  ofs=%u boyut=%u "
                                        "dolu=%.1f%%  cagiran=guest:%08x\n",
                                        _t, p1 - (self?MR32(self+8):0), n1,
                                        100.0*nz2/lim2, c->eip);
                            }
                        }
                    }
                }
                /* Karistiriciya yazma konumunu bildir (servo icin).
                 * DIKKAT: kilit SARMIS olabilir -- ikinci bolge (pv2,cb2)
                 * tamponun BASINDAN devam eder.  Yalnizca birinci bolgeye
                 * bakmak, sarmada yazma imlecini tamponun sonunda sanmaya
                 * ve servoyu yanlis beslemeye yol aciyordu. */
                { uint32_t sid3 = (self && mem_is_committed(self+31)) ? MR32(self+28) : 0;
                  uint32_t base3 = self ? MR32(self+8) : 0;
                  uint32_t n2u   = A(c,4);
                  if (sid3 && base3 && p1 >= base3){
                      void audio_note_write(int, uint32_t);
                      if (n2u && n2u < 1u<<22)
                          audio_note_write((int)sid3, n2u);          /* bastan devam */
                      else
                          audio_note_write((int)sid3, (p1 - base3) + n1);
                  } }
            }
            r=0; }
        else if (MID(Play)){
            if(self){ MR32(self+12)=1; uint64_t host_us(void);
                      MR32(self+24)=(uint32_t)(host_us()/1000ull);
                      /* (this, Reserved, Priority, Flags); DSBPLAY_LOOPING=1 */
                      if (MR32(self+28)) audio_play((int)MR32(self+28), (A(c,3) & 1u) ? 1 : 0); }
            g_snd_play++;
            if (ENVC("DSLOG")){
                fprintf(stderr, "  [ses] --- Play cagri zinciri (tampon %u) ---\n",
                        self?MR32(self+28):0);
                guest_backtrace(c, 10);
            }
            r=0; }
        else if (MID(Stop)){
            if(self){ MR32(self+12)=0; MR32(self+24)=0;
                      if (MR32(self+28)) audio_stop((int)MR32(self+28)); }
            r=0; }
        else if (MID(SetVolume)){
            if(self && MR32(self+28)) audio_set_volume((int)MR32(self+28), (long)(int32_t)A(c,1));
            r=0; }
        else if (MID(SetFrequency)){
            if(self && MR32(self+28)){ void audio_set_freq(int, uint32_t);
                                       audio_set_freq((int)MR32(self+28), A(c,1)); }
            r=0; }
        else if (MID(GetStatus)){
            uint32_t p=A(c,1); if(p){mem_commit(p,4); MR32(p)= (self&&MR32(self+12))?1u:0u;} r=0; }
        else if (MID(GetCurrentPosition)){
            int break_gcp = 0;
            /* Oynatma imleci GECEN ZAMANA gore ilerler (sahte sabit adim
             * degil): video oynatici kareleri bu saate gore suruyor. */
            uint32_t p1=A(c,1), p2=A(c,2);
            uint64_t host_us(void);
            uint32_t bps = (self && mem_is_committed(self+27)) ? MR32(self+20) : 176400u;
            if (!bps) bps = 176400u;
            uint32_t playing = self ? MR32(self+12) : 0;
            uint32_t t0 = self ? MR32(self+24) : 0;
            uint64_t now = host_us();
            if (playing && !t0){ t0 = (uint32_t)(now/1000ull); if(self) MR32(self+24)=t0; }
            uint32_t pos = 0;
            /* Karistiriciya kayitliysa GERCEK oynatma imlecini kullan:
             * oyunun akis (streaming) mantigi yazdigi yeri buna gore
             * seciyor; tahmini saatle gercek calma ayrisirsa ses bozulur. */
            { uint32_t sid = (self && mem_is_committed(self+31)) ? MR32(self+28) : 0;
              if (sid && playing){
                  uint32_t rp=0, rw=0;
                  void audio_cursors(int, uint32_t*, uint32_t*);
                  audio_cursors((int)sid, &rp, &rw);
                  if (p1){ mem_commit(p1,4); MR32(p1)=rp; }
                  if (p2){ mem_commit(p2,4); MR32(p2)=rw; }
                  if (self) MR32(self+16)=rp;
                  r=0; break_gcp = 1;
              } }
            if (!break_gcp && playing && t0){
                uint64_t ms = (now/1000ull) - (uint64_t)t0;
                uint64_t byt = ms * (uint64_t)bps / 1000ull;
                pos = sz ? (uint32_t)(byt % (uint64_t)sz) : 0;
            } else if (self) pos = MR32(self+16);
            if (!break_gcp){
                if(self) MR32(self+16)=pos;
                if(p1){mem_commit(p1,4); MR32(p1)=pos;}
                if(p2){mem_commit(p2,4); MR32(p2)=sz?((pos+2048u)%sz):0;}
            }
            r=0; }
        else if (MID(GetFormat)){
            uint32_t p=A(c,1), avail=A(c,2), pw=A(c,3);
            if(p && avail>=18u){ mem_commit(p,18); memset(g_mem+p,0,18);
                MR16(p)=1; MR16(p+2)=2; MR32(p+4)=44100; MR32(p+8)=44100*4;
                MR16(p+12)=4; MR16(p+14)=16; }
            if(pw){mem_commit(pw,4); MR32(pw)=18;}
            r=0; }
        else if (MID(GetCaps)){
            uint32_t p=A(c,1);
            if(p){ uint32_t s2=MR32(p); if(!s2||s2>256u) s2=16u; mem_commit(p,s2);
                   memset(g_mem+p,0,s2); MR32(p)=s2; MR32(p+8)=sz; }
            r=0; }
        else if (!strncmp(nm,"Get",3) && nargs>=2){
            uint32_t p=A(c,nargs-1);
            if(p && p<0xE0000000u){ mem_commit(p,8); memset(g_mem+p,0,8); }
            r=0; }
        else r=0;
    }
    /* --- DirectInput8 --- */
    else if (iface==IF_DI8){
        if      (MID(CreateDevice)){
            /* (rguid, lplpDirectInputDevice, pUnkOuter) */
            uint32_t g=A(c,1), pp=A(c,2); uint32_t d=com_new(IF_DIDEV,128);
            uint32_t kind = 0;
            if (g && mem_is_committed(g+15)){
                uint32_t d1 = MR32(g);
                if (d1 == 0x6F1D2B61u) kind = 1;        /* GUID_SysKeyboard */
                else if (d1 == 0x6F1D2B60u) kind = 2;   /* GUID_SysMouse    */
                fprintf(stderr,"  [DI] CreateDevice GUID %08x-%04x-%04x -> %s\n",
                        d1, MR16(g+4), MR16(g+6),
                        kind==1?"KLAVYE":(kind==2?"FARE":"bilinmeyen"));
            }
            MR32(d+8) = kind;
            if(pp) MR32(pp)=d; r=0; }
        else if (MID(EnumDevices)){
            /* (dwDevType, lpCallback, pvRef, dwFlags)
             * Geri cagirmayi GERCEKTEN cagir: klavye ve fare bildir. */
            uint32_t devtype=A(c,1), cb=A(c,2), ref=A(c,3);
            if (cb && ENVC("DI_ENUM")){
                for (int k=0;k<2;k++){
                    int is_kb = (k==0);
                    uint32_t t = is_kb ? 0x13u : 0x12u;
                    /* 0 = DI8DEVCLASS_ALL -> hepsi */
                    if (devtype && (devtype & 0xFFu) != t && devtype > 4u) continue;
                    uint32_t args[2] = { make_devinst(is_kb), ref };
                    if (call_guest(c, cb, 2, args) == 0) break;   /* DIENUM_STOP */
                }
                g_di_enum++;
            }
            r=0; }
        else if (MID(EnumDevicesBySemantics)){
            /* (ptszUserName, lpdiActionFormat, lpCallback, pvRef, dwFlags)
             * geri cagirma: (lpddi, lpdid, dwFlags, dwRemaining, pvRef) */
            uint32_t cb=A(c,3), ref=A(c,4);
            if (cb && !ENVC("DI_NO_SEM")){
                for (int k=0;k<2;k++){
                    int is_kb = (k==0);
                    uint32_t dev = com_new(IF_DIDEV,128);
                    MR32(dev+8) = is_kb ? 1u : 2u;     /* cihaz turu isareti */
                    uint32_t args[5] = { make_devinst(is_kb), dev,
                                         0x00000001u,          /* DIEDBS_MAPPEDPRI1 */
                                         (uint32_t)(1-k), ref };
                    if (call_guest(c, cb, 5, args) == 0) break;
                }
                g_di_enum++;
            }
            r=0; }
        else if (MID(GetDeviceStatus)) r=0;
        else if (MID(Initialize))      r=0;
        else if (MID(FindDevice))      r=0x80070002u;
        else r=0;
    }
    else if (iface==IF_DIDEV){
        uint32_t self=A(c,0);
        uint32_t dkind = (self && mem_is_committed(self+11)) ? MR32(self+8) : 0;
        /* DITRACE=1: cihaz basina cagri zinciri.  Oyun klavye cihazini
         * kuruyor ama hic okumuyor; zincirin nerede kesildigini gorelim. */
        int di_tr = 0;
        { static int n2 = 0;
          if (ENVC("DITRACE") && n2 < 140){ n2++; di_tr = 1;
              fprintf(stderr,"  [DI%u/%08x] %-24s", dkind, self, nm); } }
        if (MID(EnumObjects)){
            /* (lpCallback, pvRef, dwFlags) -- geri cagirma:
             * BOOL(LPCDIDEVICEOBJECTINSTANCE, LPVOID)
             * Cagrilmazsa oyun cihazi "nesnesiz" gorup terk ediyor. */
            uint32_t cb=A(c,1), ref=A(c,2), flags=A(c,3);
            if (cb && dkind == 1u){                 /* klavye */
                uint32_t oi = gbuf_alloc(340);
                for (uint32_t dik = 1; dik < 0xEEu; dik++){
                    if (!(flags & 0x0Cu) && flags != 0u) break;   /* DIDFT_BUTTON/ALL */
                    memset(g_mem+oi, 0, 340);
                    MR32(oi) = 316u;                              /* dwSize */
                    put_guid(oi+4, 0x55728220u, 0xD33C, 0x11CF,
                             (const uint8_t[8]){0xBF,0xC7,0x44,0x45,0x53,0x54,0,0});
                    MR32(oi+20) = dik;                            /* dwOfs */
                    MR32(oi+24) = 0x04u | (dik << 8);             /* PSHBUTTON|inst */
                    MR32(oi+28) = 0;
                    snprintf((char*)(g_mem+oi+32), 260, "Key %u", dik);
                    uint32_t args[2] = { oi, ref };
                    if (call_guest(c, cb, 2, args) == 0) break;
                }
                g_di_objs++;
            }
            r=0; }
        else if (MID(GetObjectInfo)){
            /* (pdidoi, dwObj, dwHow) */
            uint32_t oi=A(c,1), obj=A(c,2);
            if (oi && mem_is_committed(oi+3)){
                uint32_t sz = MR32(oi); if (!sz || sz > 400u) sz = 316u;
                mem_commit(oi, sz); memset(g_mem+oi, 0, sz); MR32(oi)=sz;
                put_guid(oi+4, 0x55728220u, 0xD33C, 0x11CF,
                         (const uint8_t[8]){0xBF,0xC7,0x44,0x45,0x53,0x54,0,0});
                uint32_t dik = (obj >> 8) & 0xFFu; if (!dik) dik = obj & 0xFFu;
                MR32(oi+20) = dik;
                MR32(oi+24) = 0x04u | (dik << 8);
                snprintf((char*)(g_mem+oi+32), 260, "Key %u", dik);
            }
            r=0; }
        else if (MID(GetDeviceInfo)){
            uint32_t p=A(c,1);
            if (p && mem_is_committed(p+3)){
                uint32_t sz=MR32(p); if(!sz||sz>700u) sz=580u;
                uint32_t src = make_devinst(dkind != 2u);
                mem_commit(p, sz);
                memcpy(g_mem+p, g_mem+src, sz < 580u ? sz : 580u);
                MR32(p)=sz;
            }
            r=0; }
        else
        if      (MID(SetDataFormat)){ if(self) MR32(self+4)=A(c,1); r=0; }
        else if (MID(SetCooperativeLevel)) r=0;
        else if (MID(Acquire)||MID(Unacquire)||MID(Poll)) r=0;
        else if (MID(GetDeviceState)){
            uint32_t cb=A(c,1), p=A(c,2);
            { static uint32_t seen_cb[8]; static int ns=0;
              int f2=0; for (int q=0;q<ns;q++) if (seen_cb[q]==cb) f2=1;
              if (!f2 && ns<8){ seen_cb[ns++]=cb;
                fprintf(stderr,"  [giris] GetDeviceState boyut=%u (veriBicimi=0x%08x cihaz=%u)\n",
                        cb, self?MR32(self+4):0,
                        (self && mem_is_committed(self+11)) ? MR32(self+8) : 0); } }
            if(p && cb && cb<4096u){
                mem_commit(p,cb); memset(g_mem+p,0,cb);
                /* 256 baytlik istek = DirectInput klavye tablosu */
                if (cb == 20u && metal_available()){        /* DIMOUSESTATE2 */
                    metal_fill_mouse(g_mem+p, cb);
                    g_dikey_calls++;
                    for (uint32_t q=12;q<20;q++)
                        if (MR8(p+q)){ g_dikey_hits++; break; }
                }
                else if (cb == 16u && metal_available()){   /* DIMOUSESTATE */
                    unsigned char tmpm[20];
                    metal_fill_mouse(tmpm, 20);
                    memcpy(g_mem+p, tmpm, 16);
                    g_dikey_calls++;
                }
                else if (cb == 256u && metal_available()){
                    metal_fill_keyboard(g_mem+p, 256u);
                    g_dikey_calls++;
                    for (uint32_t q=0;q<256;q++)
                        if (MR8(p+q)){ g_dikey_hits++;
                            if (g_dikey_hits <= 3)
                                fprintf(stderr,"  [giris] tus 0x%02x basili (cagri %llu)\n",
                                        q, (unsigned long long)g_dikey_calls);
                            break; }
                }
            }
            r=0; }
        else if (MID(BuildActionMap)||MID(SetActionMap)){
            /* DIACTIONFORMATA: +4 dwActionSize +12 dwNumActions +16 rgoAction
             * DIACTIONA (40): +0 uAppData +4 dwSemantic +32 dwObjID +36 dwHow */
            uint32_t af=A(c,1);
            int is_build = MID(BuildActionMap);
            if (!is_build){
                memset(g_act_of_key, 0, sizeof g_act_of_key);
                memset(g_act_ofs, 0, sizeof g_act_ofs);
                g_act_count = 0;
                if (ENVC("DIMAP")) fprintf(stderr,"  [harita] --- yeni etkin harita ---\n");
            }
            if (af && mem_is_committed(af+71)){
                uint32_t nact=MR32(af+12), rg=MR32(af+16), asz=MR32(af+4);
                if (!asz) asz=40u;
                { static int sl=0; if (sl<4){ sl++;
                    fprintf(stderr,"  [DI] %s: %u eylem, veriBoyutu=%u eylemBoyutu=%u tampon=%u cihaz=%u\n",
                            nm, nact, MR32(af+8), asz, MR32(af+40),
                            (self && mem_is_committed(self+11)) ? MR32(self+8) : 0); } }
                if (nact && nact < 1024u && rg){
                    for (uint32_t k=0;k<nact;k++){
                        uint32_t a2=rg+k*asz;
                        if (!mem_is_committed(a2+39)) break;
                        uint32_t sem=MR32(a2+4), app=MR32(a2);
                        if ((sem & 0xFFFFFF00u) != DI_KB_SEM_BASE) continue;
                        uint32_t dik = sem & 0xFFu;
                        if (!dik) continue;
                        if (is_build){
                            /* DirectInput bu alanlari doldurur:
                             * dwObjID = DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(dik) */
                            MR32(a2+32) = 0x04u | (dik << 8);
                            MR32(a2+36) = 0x20u;             /* DIAH_DEFAULT */
                            /* guidInstance = klavye */
                            put_guid(a2+16, 0x6F1D2B61u, 0xD5A0, 0x11CF,
                                     (const uint8_t[8]){0xBF,0xC7,0x44,0x45,0x53,0x54,0,0});
                        } else {
                            g_act_of_key[dik] = app;
                            g_act_ofs[dik]    = k * 4u;      /* veri ofseti */
                            g_act_count++;
                            if (ENVC("DIMAP"))
                                fprintf(stderr,"  [harita] dik=0x%02x -> eylem 0x%x (ofs %u)\n",
                                        dik, app, k*4u);
                        }
                    }
                }
            }
            r=0; }
        else if (MID(GetDeviceData)){
            /* (cbObjectData, rgdod, pdwInOut, dwFlags)
             * DIDEVICEOBJECTDATA: +0 dwOfs +4 dwData +8 dwTimeStamp
             *                     +12 dwSequence +16 uAppData          */
            uint32_t cb=A(c,1), rg=A(c,2), pcnt=A(c,3), flags=A(c,4);
            uint32_t want = (pcnt && mem_is_committed(pcnt+3)) ? MR32(pcnt) : 0;
            uint32_t n = 0;
            { static int sg=0;
              if (ENVC("DIDATA") && sg<4){ sg++;
                fprintf(stderr,"  [DIdata] cb=%u istenen=%u bayrak=0x%x eylem=%d\n",
                        cb, want, flags, g_act_count); } }
            if (g_act_count && metal_available() && cb >= 16u && cb <= 32u){
                unsigned char cur[256];
                metal_fill_keyboard(cur, 256);
                for (uint32_t k = 1; k < 256u && n < want; k++){
                    if (!g_act_of_key[k]) continue;
                    unsigned char now = cur[k] ? 1 : 0, was = g_kb_prev[k];
                    if (now == was) continue;
                    if (rg && !(flags & 1u)){          /* DIGDD_PEEK degilse tuket */
                        uint32_t e = rg + n*cb;
                        mem_commit(e, cb); memset(g_mem+e, 0, cb);
                        MR32(e)    = g_act_ofs[k];
                        MR32(e+4)  = now ? 0x80u : 0u;
                        MR32(e+8)  = (uint32_t)(host_us_ms());
                        MR32(e+12) = g_di_seq++;
                        if (cb >= 20u) MR32(e+16) = g_act_of_key[k];
                        { static int se=0;
                          if (ENVC("DIDATA") && se<6){ se++;
                            fprintf(stderr,"  [DIdata] olay: dik=%u ofs=%u veri=0x%x app=0x%x\n",
                                    k, g_act_ofs[k], now?0x80u:0u, g_act_of_key[k]); } }
                    }
                    if (!(flags & 1u)) g_kb_prev[k] = now;
                    n++; g_di_events++;
                }
            }
            if(pcnt) MR32(pcnt)=n; r=0; }
        else if (MID(GetCapabilities)){
            uint32_t p=A(c,1);
            if(p){ uint32_t sz=MR32(p); if(!sz||sz>256u) sz=44u; mem_commit(p,sz);
                   memset(g_mem+p,0,sz); MR32(p)=sz;
                   MR32(p+4)=0x00000010u;   /* DIDC_ATTACHED */
                   MR32(p+8)=0x00000013u;   /* DI8DEVTYPE_KEYBOARD */
                   MR32(p+12)=256; }        /* dwAxes/dwButtons yaklasik */
            r=0; }
        else if (MID(GetProperty)){
            uint32_t p=A(c,2);
            if(p){ uint32_t sz=MR32(p); if(sz && sz<=256u){ mem_commit(p,sz);
                   memset(g_mem+p+16,0,sz-16>0?sz-16:0); } }
            r=0; }
        else if (MID(SetProperty)) r=0;
        else if (MID(EnumObjects)) r=0;
        else if (MID(GetObjectInfo)||MID(GetDeviceInfo)){
            uint32_t p=A(c,nargs-1);
            if(p){ uint32_t sz=MR32(p); if(!sz||sz>512u) sz=32u; mem_commit(p,sz);
                   memset(g_mem+p,0,sz); MR32(p)=sz; }
            r=0; }
        else if (MID(SetEventNotification)) r=0;
        else if (!strncmp(nm,"Get",3) && nargs>=2){
            uint32_t p=A(c,nargs-1);
            if(p && p<0xE0000000u){ mem_commit(p,32); memset(g_mem+p,0,32); }
            r=0; }
        else r=0;
        if (di_tr) fprintf(stderr, " -> 0x%08x\n", r);
    }
    else if (iface==IF_QUERY){
        if      (MID(GetDevice)){ uint32_t p=A(c,1); if(p) MR32(p)=g_dev; r=0; }
        else if (MID(GetDataSize)) r=4;
        else if (MID(GetData)){ uint32_t p=A(c,1); if(p){mem_commit(p,4);MR32(p)=0;} r=0; }
        else r=0;
    }
    else if (iface==IF_SWAPCHAIN){
        if      (MID(GetDevice)){ uint32_t p=A(c,1); if(p) MR32(p)=g_dev; r=0; }
        else if (MID(GetBackBuffer)){ uint32_t pp=A(c,3); if(pp) MR32(pp)=com_new(IF_SURF,64); r=0; }
        else if (MID(Present)){ g_present++; r=0; }
        else if (!strncmp(nm,"Get",3) && nargs>=2){ uint32_t p=A(c,nargs-1);
            if(p && p<0xE0000000u){ mem_commit(p,64); memset(g_mem+p,0,64);} r=0; }
        else r=0;
    }
    else if (iface==IF_VSHADER || iface==IF_VDECL || iface==IF_STATEBLOCK || iface==IF_SIMPLE){
        if (iface==IF_STATEBLOCK && MID(Apply)){
            uint32_t self5=A(c,0);
            int bi = (self5 && mem_is_committed(self5+11)) ? (int)MR32(self5+8) : 0;
            if (bi > 0 && bi < MAXSB && g_sb[bi].used){
                SBlock *B = &g_sb[bi];
                const uint32_t *save_ovr = g_arg_ovr;
                int save_rep = g_sb_replay, save_rec = g_sb_rec;
                g_sb_replay = 1; g_sb_rec = 0;
                for (int k = 0; k < B->n; k++){
                    g_arg_ovr = B->r[k].a;
                    com_call(c, B->r[k].magic);
                }
                g_arg_ovr = save_ovr; g_sb_replay = save_rep; g_sb_rec = save_rec;
                g_sb_applied++;
            }
            r=0; }
        else if (iface==IF_STATEBLOCK && MID(Capture)){
            /* Bloktaki her durumun SU ANKI degerini yeniden yaz.  Oyun
             * kaydet/geri-yukle deyimini boyle kuruyor: bir kez kaydet,
             * degistirmeden once Capture, sonra Apply. */
            uint32_t self6=A(c,0);
            int bi = (self6 && mem_is_committed(self6+11)) ? (int)MR32(self6+8) : 0;
            if (bi > 0 && bi < MAXSB && g_sb[bi].used){
                SBlock *B = &g_sb[bi];
                for (int k = 0; k < B->n; k++){
                    SBRec *R = &B->r[k];
                    uint32_t mm = R->magic & 0xFFFFu;
                    uint32_t ifc = (R->magic>>16) & 0x1FFFu;
                    if (ifc >= NEL(g_if) || mm >= g_if[ifc].n) continue;
                    const char *n2 = g_if[ifc].m[mm].name;
                    if      (NMEQ(n2, "SetRenderState")){
                        if (R->a[1] < 256u) R->a[2] = g_rs[R->a[1]]; }
                    else if (NMEQ(n2, "SetTextureStageState")){
                        if (R->a[1] < 8u && R->a[2] < 40u) R->a[3] = g_tss_raw[R->a[1]][R->a[2]]; }
                    else if (NMEQ(n2, "SetSamplerState")){
                        if (R->a[1] < 16u && R->a[2] < 20u) R->a[3] = g_smp_raw[R->a[1]][R->a[2]]; }
                    else if (NMEQ(n2, "SetTexture")){
                        if (R->a[1] < 8u) R->a[2] = g_raw_tex[R->a[1]]; }
                    else if (NMEQ(n2, "SetVertexShader"))      R->a[1] = g_raw_vs;
                    else if (NMEQ(n2, "SetPixelShader"))       R->a[1] = g_raw_ps;
                    else if (NMEQ(n2, "SetVertexDeclaration")) R->a[1] = g_raw_decl;
                    else if (NMEQ(n2, "SetFVF"))               R->a[1] = g_cur_fvf;
                }
                g_sb_captured++;
            }
            r=0; }
        else if (MID(GetDevice)){ uint32_t p=A(c,1); if(p) MR32(p)=g_dev; r=0; }
        else if (MID(GetFunction)||MID(GetDeclaration)){
            uint32_t self2=A(c,0), pdata=A(c,1), psize=A(c,2);
            uint32_t src = self2?MR32(self2+4):0, sz = self2?MR32(self2+8):0;
            if (pdata && src && sz){ mem_commit(pdata,sz); memcpy(g_mem+pdata, g_mem+src, sz); }
            if (psize){ mem_commit(psize,4); MR32(psize)=sz; }
            r=0; }
        else r=0;
    }
    else r=0;

    if (g_com_trace && !MID(Present) && strncmp(nm,"Draw",4)
        && !MID(SetRenderState) && !MID(SetTextureStageState))
        fprintf(stderr,"  [COM] %s::%-28s (%d arg) -> 0x%08x%s\n",
                g_if[iface].name, nm, nargs, r, impl?"":" [YOK]");
    if(!impl) g_com_unimpl++;
    c->eax=r;
    if (!g_sb_replay) c->esp += 4u*(uint32_t)nargs;
    return 1;
}

static uint32_t g_ds = 0;
/* DirectSoundCreate(lpGuid, ppDS, pUnkOuter) */
uint32_t com_directsoundcreate(uint32_t ppDS){
    if (!g_ds){ g_ds = com_new(IF_DS, 64);
        fprintf(stderr,"com: IDirectSound nesnesi 0x%08x\n", g_ds); }
    if (ppDS) MR32(ppDS) = g_ds;
    return 0;
}

static uint32_t g_di8 = 0;
/* DirectInput8Create(hinst, dwVersion, riidltf, ppvOut, punkOuter) */
uint32_t com_directinput8create(uint32_t ppvOut){
    if (!g_di8){ g_di8 = com_new(IF_DI8, 64);
        fprintf(stderr,"com: IDirectInput8 nesnesi 0x%08x\n", g_di8); }
    if (ppvOut) MR32(ppvOut) = g_di8;
    return 0;                       /* DI_OK */
}

uint32_t com_direct3dcreate9(void){
    if(!g_d3d9){ g_d3d9=com_new(IF_D3D9,64);
        fprintf(stderr,"com: IDirect3D9 nesnesi 0x%08x\n", g_d3d9); }
    return g_d3d9;
}
void com_init(void){ mem_commit(COM_BASE,0x10000);
    { extern char **environ; (void)environ; }
}
static uint64_t *g_ptype = 0, *g_stride = 0;
void draw_stats_save(uint64_t *a, uint64_t *b){ g_ptype=a; g_stride=b; }

void com_report(void){
    fprintf(stderr,"\n=== COM / D3D9 KATMANI ===\n");
    if (g_aspfix != 1.f)
        { fprintf(stderr,"  genis ekran      : aspfix %.4f, %lu perspektif / %lu ortografik cizim\n",
                  g_aspfix, g_asp_persp, g_asp_ortho);
          for (unsigned v2 = 0; v2 < MAXVS; v2++)
              if (g_asp_np[v2] || g_asp_no[v2])
                  fprintf(stderr,"     vs=%-3u cx=%-3d cw=%-3d  persp %8lu / orto %8lu  c[cw].w=%.3f\n",
                          v2, g_pos_cx[v2], g_pos_cw[v2], g_asp_np[v2], g_asp_no[v2], g_asp_cw3[v2]); }
    if (ENVC("RSLOG")){
        /* rs_apply()'in gercekten kullandigi state'ler */
        static const unsigned handled[] = {7,14,15,19,20,22,23,24,25,27,168,171,193};
        fprintf(stderr,"  --- SetRenderState kullanimi (ELE ALINMAYANLAR yildizli) ---\n");
        for (unsigned i=0;i<256;i++){
            if (!g_rs_hits[i]) continue;
            int h=0; for (unsigned k=0;k<sizeof handled/sizeof*handled;k++) if(handled[k]==i) h=1;
            fprintf(stderr,"   %c st=%-3u %8u kez  degerler:", h?' ':'*', i, g_rs_hits[i]);
            for (int k=0;k<g_rs_nval[i];k++){
                float f; memcpy(&f,&g_rs_vals[i][k],4);
                fprintf(stderr," 0x%x(%g)", g_rs_vals[i][k], f);
            }
            fprintf(stderr,"%s\n", g_rs_nval[i]>=4?" ...":"");
        }
    }
    fprintf(stderr,"  SetRenderState    : %llu uygulandi, %llu ATLANDI (%%%.1f gereksizdi)\n",
            (unsigned long long)g_rs_run, (unsigned long long)g_rs_skip,
            100.0*(double)g_rs_skip/(double)((g_rs_skip+g_rs_run) ? (g_rs_skip+g_rs_run) : 1));
    fprintf(stderr,"  toplam COM cagrisi : %llu\n",(unsigned long long)g_com_calls);
    fprintf(stderr,"  Present (kare)     : %llu\n",(unsigned long long)g_present);
    if (metal_available())
        fprintf(stderr,"  Metal ekrana sunum : %ld kare, %ld ucgen\n"
                       "  Metal doku         : %ld olusturuldu, %ld yukleme\n",
                metal_frames(), metal_tris(),
                metal_tex_created(), metal_tex_uploaded());
    fprintf(stderr,"  Draw* cagrisi      : %llu\n",(unsigned long long)g_draw);
    fprintf(stderr,"  ps sabit dosyasi   : en yuksek c%u kullanildi, %u yazma 32 ustu\n",
            g_psc_max ? g_psc_max-1u : 0u, g_psc_drop);
    fprintf(stderr,"  guest COM heap     : %u KB\n",(com_ptr-COM_BASE)/1024u);
    fprintf(stderr,"  guest tampon heap  : %u KB\n",(gbuf_ptr-GBUF_BASE)/1024u);
    if (g_ptype){
        static const char *PT[] = {"?","POINTLIST","LINELIST","LINESTRIP",
                                   "TRIANGLELIST","TRIANGLESTRIP","TRIANGLEFAN","?"};
        fprintf(stderr,"  --- DrawPrimitiveUP: primitif tipleri ---\n");
        for (int i=0;i<8;i++) if (g_ptype[i])
            fprintf(stderr,"    %-16s %llu\n", PT[i], (unsigned long long)g_ptype[i]);
        fprintf(stderr,"  --- kose (vertex) adim boyutlari ---\n");
        for (int i=0;i<64;i++) if (g_stride[i])
            fprintf(stderr,"    %3d bayt  %llu\n", i*4, (unsigned long long)g_stride[i]);
    }
    fprintf(stderr,"  --- DirectInput / DirectSound cagrilari ---\n");
    { int any=0;
      for (unsigned i=1;i<NEL(g_if) && i<MSTAT_IF;i++){
        if (i!=IF_DI8 && i!=IF_DIDEV && i!=IF_DS && i!=IF_DSBUF && i!=IF_QUERY) continue;
        for (unsigned j=0;j<g_if[i].n && j<MSTAT_M;j++)
            if (g_mstat[i][j]){ any=1;
                fprintf(stderr,"    %-26s %-26s %llu\n",
                        g_if[i].name, g_if[i].m[j].name,
                        (unsigned long long)g_mstat[i][j]); }
      }
      if(!any) fprintf(stderr,"    (hic cagri yok)\n");
    }
    fprintf(stderr,"  kose shader'i     : %llu cizim shader'li, %llu shader'siz, %u derlendi\n",
            (unsigned long long)g_vs_draws, (unsigned long long)g_ff_draws, g_vsprogn-1);
    fprintf(stderr,"  durum blogu       : %d yuva, %llu Apply, %llu Capture, "
            "%llu kayit tasti, %llu yuva bulunamadi\n",
            g_sbn-1, (unsigned long long)g_sb_applied,
            (unsigned long long)g_sb_captured, (unsigned long long)g_sb_dropped,
            (unsigned long long)g_sb_full);
    fprintf(stderr,"  GPU kose gölgelendirici: %llu cizim GPU'da, %llu ceviri reddedildi,"
            " %d/%d (shader,bildirim) cifti kayitli\n",
            (unsigned long long)g_gpuvs_draws, (unsigned long long)g_gpuvs_fail,
            g_gvs_next-1, g_gvsn);
    { unsigned ovrv=0, ovkb=0; void metal_raw_overflow(unsigned*,unsigned*);
      metal_raw_overflow(&ovrv,&ovkb);
      if (ovrv || ovkb || g_gpuvs_drop)
          fprintf(stderr,"  GPU kose arena tasmasi : %u kose, %u sabit, %llu cizim CPU'ya dondu\n",
                  ovrv, ovkb, (unsigned long long)g_gpuvs_drop); }
    if (metal_available())
        fprintf(stderr,"  atlanan kare      : %ld\n", metal_frames_dropped());
    { extern uint64_t g_icall_hits;
      if (ENVC("ICALL"))
          fprintf(stderr,"  ICALL isabet      : %llu\n", (unsigned long long)g_icall_hits); }
    { const char *cp = ENVC("COVREPORT");
      if (cp){ void cov_report(const char*); cov_report(cp); } }
    { extern uint64_t g_timer_ticks;
      fprintf(stderr,"  zamanlayici tik   : %llu\n", (unsigned long long)g_timer_ticks); }
    fprintf(stderr,"  cizim matrisi     : %llu gecerli, %llu bozuk\n",
            (unsigned long long)g_mat_ok,(unsigned long long)g_mat_bad);
    fprintf(stderr,"  DI eylem haritasi : %d tus bagli, %llu olay uretildi\n",
            g_act_count, (unsigned long long)g_di_events);
    fprintf(stderr,"  DI cihaz sayimi   : %llu sayim, %llu nesne sayimi\n",
            (unsigned long long)g_di_enum, (unsigned long long)g_di_objs);
    for (int q2 = 0; g_lk_tid && q2 < 8; q2++){
        if (!g_lk_tid[q2]) continue;
        fprintf(stderr,"  [kilit] tid=%u  kilit=%u  toplam=%.1f KB\n",
                g_lk_tid[q2]-1u, g_lk_cnt[q2], g_lk_byt[q2]/1024.0);
    }
    for (int q2 = 0; q2 < g_lock_n; q2++){
        uint32_t o2 = g_lock_obj[q2];
        fprintf(stderr,"  [kilit] nesne 0x%08x  ses-id=%u  boyut=%u  kilit=%u\n",
                o2, (o2 && mem_is_committed(o2+31)) ? MR32(o2+28) : 0u,
                (o2 && mem_is_committed(o2+7)) ? MR32(o2+4) : 0u, g_lock_cnt[q2]);
    }
    fprintf(stderr,"  ses tamponu       : %llu dolu yazma, %llu bos yazma, %.1f KB, %llu Play\n",
            (unsigned long long)g_snd_nonzero,(unsigned long long)g_snd_zero,
            g_snd_bytes/1024.0,(unsigned long long)g_snd_play);
    audio_report();
    fprintf(stderr,"  klavye okuma      : %llu cagri, %llu tanesinde tus basili\n",
            (unsigned long long)g_dikey_calls,(unsigned long long)g_dikey_hits);
    fprintf(stderr,"  RT dokusu olusturuldu: %llu\n",(unsigned long long)g_rt_tex);
    { void metal_miplevel_report(void); if (metal_available()) metal_miplevel_report(); }
    fprintf(stderr,"  render hedefi     : dokuya %llu, ekrana %llu\n",
            (unsigned long long)g_rt_set,(unsigned long long)g_rt_screen);
    fprintf(stderr,"  SetTexture        : gecerli %llu, NULL %llu, id=0 %llu, asama!=0 %llu\n",
            (unsigned long long)g_st_ok,(unsigned long long)g_st_null,
            (unsigned long long)g_st_zero,(unsigned long long)g_st_other);
    fprintf(stderr,"  doku olusturma    : %llu basarisiz\n", (unsigned long long)g_tex_fail);
    fprintf(stderr,"  bunun vs_run'da   : %.2f sn\n", g_t_vs_ns/1e9);
    fprintf(stderr,"  --- SHADER BASINA KOSE ---\n");
    for (int q=0;q<8;q++){
        uint64_t best=0; int bi=-1;
        for (int w=0;w<64;w++) if (g_v_per_vs[w]>best){ best=g_v_per_vs[w]; bi=w; }
        if (bi<0) break;
        fprintf(stderr,"     vs%-3d %12llu kose (%.1f%%)  %u komut\n", bi,
                (unsigned long long)best,
                g_v_xform ? 100.0*(double)best/(double)g_v_xform : 0.0,
                (bi>0 && (unsigned)bi<g_vsprogn && g_vsprog[bi]) ? vs_ninstr(g_vsprog[bi]) : 0u);
        g_v_per_vs[bi]=0;
    }
    fprintf(stderr,"  cizim yolu suresi : %.2f sn, %llu kose donusturuldu (%.1f M kose/sn)\n",
            g_t_draw_ns/1e9, (unsigned long long)g_v_xform,
            g_t_draw_ns ? (double)g_v_xform/(g_t_draw_ns/1000.0) : 0.0);
    fprintf(stderr,"  bagli doku asamasi: 1'li %llu, 2'li %llu, 3'lu %llu, 4'lu %llu cizim\n",
            (unsigned long long)g_stage_hist[0],(unsigned long long)g_stage_hist[1],
            (unsigned long long)g_stage_hist[2],(unsigned long long)g_stage_hist[3]);
    fprintf(stderr,"  indeksli cizim   : %llu cizim, %llu ucgen, %llu atlandi\n",
            (unsigned long long)g_dip_draws,(unsigned long long)g_dip_tris,
            (unsigned long long)g_dip_skip);
    fprintf(stderr,"  ham konum araligi : x[%.4g..%.4g] y[%.4g..%.4g] z[%.4g..%.4g]\n",
            g_rmin[0],g_rmax[0],g_rmin[1],g_rmax[1],g_rmin[2],g_rmax[2]);
    fprintf(stderr,"  kose konumu       : ekran ici %llu, disi %llu, w<=0 %llu\n",
            (unsigned long long)g_v_in, (unsigned long long)g_v_out,
            (unsigned long long)g_v_badw);
    for (unsigned q=1; q<g_vsprogn && q<6; q++)
        fprintf(stderr,"     vs%u: surum %u.%u, %u komut, %u bilinmeyen\n", q,
                vs_version(g_vsprog[q])>>8, vs_version(g_vsprog[q])&0xFF,
                vs_ninstr(g_vsprog[q]), vs_unknown(g_vsprog[q]));
    fprintf(stderr,"  --- EN COK CAGRILAN D3D9/COM METOTLARI ---\n");
    for (int k=0;k<120;k++){
        uint64_t best=0; int bi=0,bm=0;
        for (unsigned i=1;i<NEL(g_if) && i<MSTAT_IF;i++)
            for (unsigned j=0;j<g_if[i].n && j<MSTAT_M;j++)
                if (g_mstat[i][j]>best){ best=g_mstat[i][j]; bi=(int)i; bm=(int)j; }
        if (!best) break;
        fprintf(stderr,"    %-26s %-28s %llu\n",
                g_if[bi].name, g_if[bi].m[bm].name, (unsigned long long)best);
        g_mstat[bi][bm]=0;
    }
}

static void fill_caps(uint32_t p){
    mem_commit(p,0x150); memset(g_mem+p,0,0x150);
    MR32(p+0x00)=1; MR32(p+0x08)=0x00020000; MR32(p+0x0C)=0x00000002;
    MR32(p+0x10)=0x00000020; MR32(p+0x14)=0x80000001; MR32(p+0x18)=1;
    MR32(p+0x1C)=0x001BBEF0; MR32(p+0x20)=0x0000033F; MR32(p+0x24)=0x0F730191;
    MR32(p+0x28)=0x000000FF; MR32(p+0x2C)=0x00003FFF; MR32(p+0x30)=0x00003FFF;
    MR32(p+0x34)=0x000000FF; MR32(p+0x38)=0x0008FFFF; MR32(p+0x3C)=0x0021EDA5;
    MR32(p+0x40)=0x07030700; MR32(p+0x44)=0x07030700; MR32(p+0x48)=0x07030700;
    MR32(p+0x4C)=0x0000003F; MR32(p+0x50)=0x0000003F; MR32(p+0x54)=0x0000001F;
    MR32(p+0x58)=4096; MR32(p+0x5C)=4096; MR32(p+0x60)=256; MR32(p+0x64)=8192;
    MR32(p+0x68)=4096; MR32(p+0x6C)=16;
    MRF32(p+0x70)=1.0e10f; MRF32(p+0x74)=-32768.0f; MRF32(p+0x78)=-32768.0f;
    MRF32(p+0x7C)=32768.0f; MRF32(p+0x80)=32768.0f; MRF32(p+0x84)=0.0f;
    MR32(p+0x88)=0x000001FF; MR32(p+0x8C)=0x00100008; MR32(p+0x90)=0x03FFFFFF;
    MR32(p+0x94)=8; MR32(p+0x98)=8; MR32(p+0x9C)=0x0000013B;
    MR32(p+0xA0)=8; MR32(p+0xA4)=6; MR32(p+0xA8)=4; MR32(p+0xAC)=8;
    MRF32(p+0xB0)=256.0f; MR32(p+0xB4)=1048575; MR32(p+0xB8)=1048575;
    MR32(p+0xBC)=16; MR32(p+0xC0)=508;
    { /* Shader surumu ortam degiskeniyle ayarlanabilir (SM=0 -> sabit fonksiyon) */
      const char *sm = ENVC("D3D_SM");
      uint32_t vs=0xFFFE0200u, ps=0xFFFF0200u;
      if (sm){ int v=atoi(sm);
        if (v==0){ vs=0; ps=0; }
        else if (v==11){ vs=0xFFFE0101u; ps=0xFFFF0101u; }
        else if (v==14){ vs=0xFFFE0101u; ps=0xFFFF0104u; }
      }
      MR32(p+0xC4)=vs; MR32(p+0xC8)=256; MR32(p+0xCC)=ps; }
    MRF32(p+0xD0)=8.0f; MR32(p+0xD4)=0x00000051;
}
