/* Grafik iyilestirmeleri (yalnizca HD build).  Bkz. enhance.h, GRAFIK_ARASTIRMA.md.
 *
 * 1) YANSIMA COZUNURLUGU
 *    Oyun uc yansimayi ayri render hedeflerine ciziyor:
 *      yol yansimasi   320x240   (1080p'de yatayda 6x buyutuluyor)
 *      dikiz aynasi    256x256
 *      arac kup harita 128/yuz
 *    Hedefler CAGRI ADRESINDEN taniniyor (bizim exe, SHA 8f6a9ae2...).
 *    Topluluk modlarinin adresleri bu exe'de kayik; buradakiler kendi
 *    ayrıştırmamızdan:
 *      0x5BA0A7  CreateTexture(320,240,RT)   yol yansimasi
 *      0x5BA11D  CreateTexture(320,240,RT)   yol yansimasi bulanikligi
 *                                            (yalnizca yansima seviyesi 3)
 *      0x5BA1DD  CreateTexture([7FEE80],[7FEE84],RT)  dikiz aynasi
 *      0x5BA559  CreateCubeTexture([7FEE6C],RT)       arac cevre haritasi
 *    Z yuzeyleri (0x5BA0E9, 0x5BA21C, 0x5BA5AA) dokunmadan kalir: Metal Z
 *    dokusunu ilk kullanildigi hedefin boyutunda yaratiyor.
 *
 *    Ortam:  HDREFL=0       yansima iyilestirmesini kapat (A/B icin)
 *            HD_ROAD=1.0    yol yansimasi: geri tamponun kati
 *            HD_MIRROR=1024 ayna kenari (piksel)
 *            HD_CUBE=512    kup yuzu kenari (piksel)
 *            HD_BLUR=0.5    yol yansimasi bulanikligi: 1 = oyunun kendi
 *                           genisligi, 0 = keskin (bkz. asagi)
 *
 * 2) YOL YANSIMASI BULANIKLIGI
 *    Oyun yansimayi ps 19 ile iki gecis bulaniklastiriyor (13->14->13):
 *    4 ornek, ofsetler 320 piksele gore UV'de sabit.  Doku 1920'ye
 *    buyuyunce ornekler 6 piksel aralikli dusuyor ve dikey cizgili desen
 *    cikiyordu.  Cozum: yansima hedeflerine mip zinciri, bulaniklik
 *    gecisinde L seviyesinden oku ve ornek ofsetlerini merkeze gore
 *    s = 2^L / olcek ile olcekle.  Boylece ornek araligi hep ~1 teksel:
 *      HD_BLUR=1 -> L = log2(olcek), s = 1: oyunun bulaniklik genisligi
 *      HD_BLUR=0 -> L = 0, s = 1/olcek: tam cozunurlukte ince bulaniklik
 *
 * 3) MSAA (kodu metal.m'de, "HD: ekran icin MSAA")
 *    HD_MSAA=4 (varsayilan; 2/8 da olur, 0/1 kapali).  Yalnizca EKRAN
 *    gecisleri cok ornekli; her gecis StoreAndMultisampleResolve ile
 *    drawable'a cozulur.  Z'nin cok ornekli ikizi tutulur ve derinligi de
 *    tek ornekli Z'ye COZULUR (HD_ZRESOLVE=0 kapatir): oyunun derinlik
 *    testli tam ekran dortgeni (ps 17 / vs 11, 1920x1080 hedefe) sahnenin
 *    derinligini gormek zorunda.  Cozulmeyince ekran disi hedefte Z testi
 *    yapan cizim sayisi kare basina ~11 olculdu.
 *
 * 4) SSR + yansima ayristirma (metal.m "HD: SSR", vsemu.c ps_to_msl_ex2,
 *    kamera: overrides.c sub_005c7ed0).  HD_SSR=0 kapali, HD_SSR_DEBUG=1.
 *    Ayrinti: GRAFIK_ARASTIRMA.md §8.
 *
 * 5) ISIN IZLEMELI YANSIMA (metal.m "RT cekirdegi" + "SABIT SAHNE ONBELLEGI",
 *    vsemu.c vs_to_msl'in vsk_N hesaplama cekirdegi).  macOS 13+.
 *    Yol ve araba pikselleri icin yansima isini gercek sahne geometrisine
 *    atilir; isabet oyunun kendi malzemesiyle golgelenir (doku, kose rengi,
 *    far isigi terimi, sis, alfa testi).  Iska -> oyunun kendi yansimasi.
 *      HD_RT=0         kapali (SSR'ye duser)
 *      HD_RT_SCALE=1..4 isin cozunurlugu boleni; verilmezse OTOMATIK (genislige
 *                      gore: 1080p 2, 2560/3024 3, 4K 4)
 *      HD_RT_CACHE=0   sabit sahne onbellegini kapat (kamera arkasi kaybolur)
 *      HD_RT_MAXD=600  isin menzili (dunya birimi ~ metre)
 *      HD_RT_LOD=0.05  uzakliga gore doku mip'i
 *      HD_RT_FIX=0     kose denetimini kapat (NaN / sonsuz / 100 km'den uzak
 *                      koseli ucgenler ivme yapisini zehirliyor: §11)
 *    Tani: HD_SSR_DEBUG=4 (yalniz yansima), HD_RT_LOG=1/2, HD_RT_DUMPTAB=<kare>,
 *    HD_RT_STRESS=<N>, HD_RT_STAGE/NOTEX/PLAIN, HD_RTPROBE.
 *    Ayrinti: GRAFIK_ARASTIRMA.md §9-§10.
 *
 * 6) GPU BUTCESI (metal.m "MSAA: gereksiz saklama / cozme", "SAHNEYI BOLEN ARA
 *    GECISI ERTELE").  Karede 5 cok ornekli ekran gecisi 1'e indirildi.
 *      HD_MSLAZY=0     tembel saklama/cozme kapali (eski davranis)
 *      HD_REORDER=0    ara gecis erteleme kapali
 *    Olcum (iki build'de): GPUWIN=a,b  PASSPROF=N [PASSPROF_MODE=1]
 *    yalniz HD: HD_COMP=0  HD_RT_AB=1.   Ayrinti: GRAFIK_ARASTIRMA.md §11.   */
#include "enhance.h"
#if NFS_ENHANCED
#include <stdio.h>
#include <stdlib.h>

#define RET_ROAD       0x005BA0A7u
#define RET_ROAD_BLUR  0x005BA11Du
#define RET_MIRROR     0x005BA1DDu
#define RET_CUBE       0x005BA559u

static int    g_init, g_refl = 1;
static double g_road = 1.0, g_blur = 0.5;
static uint32_t g_mirror = 1024, g_cube = 512;
static uint32_t g_hits[4];
static uint32_t g_last_w[4], g_last_h[4];

static uint32_t clampu(double v){
    if (v < 16.0) return 16u;
    if (v > 8192.0) return 8192u;
    return (uint32_t)(v + 0.5);
}

static void enh_init(void){
    if (g_init) return;
    g_init = 1;
    const char *e;
    if ((e = getenv("HDREFL"))   && *e == '0') g_refl = 0;
    if ((e = getenv("HD_ROAD"))   && *e) g_road   = atof(e);
    if ((e = getenv("HD_MIRROR")) && *e) g_mirror = clampu(atof(e));
    if ((e = getenv("HD_CUBE"))   && *e) g_cube   = clampu(atof(e));
    if ((e = getenv("HD_BLUR"))   && *e) g_blur   = atof(e);
    if (g_blur < 0.0) g_blur = 0.0;
    if (g_blur > 1.0) g_blur = 1.0;
    if (g_road <= 0.0) g_road = 1.0;
    fprintf(stderr, "[HD] yansima: %s  yol=%.2fx geri tampon  bulaniklik=%.2f  ayna=%u  kup=%u\n",
            g_refl ? "ACIK" : "KAPALI", g_road, g_blur, g_mirror, g_cube);
}

const char *enh_build_name(void){ return "HD"; }
float enh_blur(void){ enh_init(); return (float)g_blur; }

int enh_rt_size(uint32_t ret, uint32_t bb_w, uint32_t bb_h,
                uint32_t *w, uint32_t *h){
    enh_init();
    if (!g_refl) return ENH_NONE;
    int k, kind;
    uint32_t nw, nh;
    switch (ret){
    case RET_ROAD: case RET_ROAD_BLUR:
        /* Beklenen boyut degilse (baska bir surum/yama) dokunma. */
        if (*w != 320u || *h != 240u) return ENH_NONE;
        k  = (ret == RET_ROAD) ? 0 : 1; kind = ENH_ROAD;
        nw = clampu(bb_w * g_road); nh = clampu(bb_h * g_road);
        break;
    case RET_MIRROR:
        if (*w != 256u || *h != 256u) return ENH_NONE;
        k = 2; kind = ENH_MIRROR; nw = nh = g_mirror;
        break;
    case RET_CUBE:
        if (*w != 128u) return ENH_NONE;
        k = 3; kind = ENH_CUBE; nw = nh = g_cube;
        break;
    default:
        return ENH_NONE;
    }
    if (g_hits[k] < 3 || g_last_w[k] != nw || g_last_h[k] != nh){
        static const char *nm[4] = { "yol yansimasi", "yol bulanikligi",
                                     "dikiz aynasi", "kup haritasi" };
        fprintf(stderr, "[HD] %s %ux%u -> %ux%u\n", nm[k], *w, *h, nw, nh);
    }
    g_hits[k]++; g_last_w[k] = nw; g_last_h[k] = nh;
    *w = nw; *h = nh;
    return kind;
}

/* Ana gorunumun (kimlik 1) matrislerini Metal'e kare verisi olarak ver. */
void enh_view_setup(uint32_t id, const float *m, uint32_t cam){
    (void)cam;
    /* VIEWLOG=1: her gorunum kimliginin gorunum matrisi (3x3 + konum),
     * kimlik basina 4000. cagridan sonra bir kez.  Kup yuzu kameralarinin
     * eksenlerini okumak icin. */
    { static int vl = -1; static unsigned cnt[32]; static unsigned char done[32];
      if (vl < 0) vl = getenv("VIEWLOG") ? 1 : 0;
      if (vl && id < 32u && !done[id] && ++cnt[id] > 4000u){ done[id] = 1;
          fprintf(stderr, "[gorunum %u] sag=(%.3f %.3f %.3f) y=(%.3f %.3f %.3f) ileri=(%.3f %.3f %.3f) P00=%.3f P11=%.3f\n",
                  id, m[0], m[4], m[8], m[1], m[5], m[9], m[2], m[6], m[10], m[16], m[21]); } }
    /* Kup harita yuzu kameralari (gorunum 16..21) dunya eksenlerine DEGIL
     * baska bir cerceveye hizali (olculdu).  Kup ornekleme vektorunu dunyaya
     * cevirmek icin yuz kameralarinin ileri yonleri: kup +x = gorunum 17,
     * +y = gorunum 18, +z = gorunum 21 (olculdu: arabada sag/geri/yukari;
     * HD_SSR_DEBUG=3 normal uyum testiyle dogrulandi). */
    static float cube_b[9] = { 1,0,0, 0,1,0, 0,0,1 };
    if (id == 17u || id == 18u || id == 21u){
        int a = (id == 17u) ? 0 : (id == 18u) ? 1 : 2;
        cube_b[a*3] = m[2]; cube_b[a*3+1] = m[6]; cube_b[a*3+2] = m[10];
        return;
    }
    if (id != 1u) return;
    void metal_hd_set_camera(const float *m48, float k, const float *cube9);
    float com_aspfix(void);
    metal_hd_set_camera(m, com_aspfix(), cube_b);
}

void enh_report(void){
    if (!g_init) return;
    fprintf(stderr, "[HD] yansima hedefleri: yol=%u bulaniklik=%u ayna=%u kup=%u"
            " (son: yol %ux%u, ayna %u, kup %u)\n",
            g_hits[0], g_hits[1], g_hits[2], g_hits[3],
            g_last_w[0], g_last_h[0], g_last_w[2], g_last_w[3]);
}
#endif /* NFS_ENHANCED */
