/* Grafik iyilestirmeleri: YALNIZCA "HD" build'de derlenir.
 *
 * Iki ayri ikili var (bkz. link.sh):
 *   out/nfsu2_boot     orijinal: oyunun 2004'teki goruntusu, sadik port
 *   out/nfsu2_hd_boot  HD: ayni lift edilmis kod + bu dosyadaki iyilestirmeler
 *
 * Lift edilmis shard'lar iki build'de ORTAKTIR; fark yalnizca runtime'da.
 * Orijinal build'de NFS_ENHANCED=0 ve buradaki her kanca derleme aninda
 * sabit 0'a duser -- kod yolu bile olusmaz.
 *
 * Ilke: oyunun mantigina dokunma.  Oyun kendi boyutlarini gormeye devam
 * eder (GetDesc 320x240 der); yalnizca Metal'deki gercek doku buyur.
 * Viewport'u zaten hedefin tamamina kuruyoruz, yani sahne buyuk dokuyu
 * doldurur ve ornekleme UV uzayinda oldugu icin boyuttan bagimsizdir.  */
#ifndef ENHANCE_H
#define ENHANCE_H
#include <stdint.h>

#ifndef NFS_ENHANCED
#define NFS_ENHANCED 0
#endif

enum { ENH_NONE = 0, ENH_ROAD = 1, ENH_MIRROR = 2, ENH_CUBE = 3 };

#if NFS_ENHANCED
/* Render hedefi boyutu: oyunun istedigi (w,h)'yi CAGIRAN adrese gore
 * buyutur.  ret = COM cagrisinin donus adresi (c->eip).  Hedefin turunu
 * (ENH_*) dondurur; dokunmadiysa ENH_NONE. */
int  enh_rt_size(uint32_t ret, uint32_t bb_w, uint32_t bb_h,
                 uint32_t *w, uint32_t *h);
/* Yol yansimasi bulanikligi: 1 = oyunun orijinal genisligi, 0 = keskin. */
float enh_blur(void);
/* Gorunum kurulumu (overrides.c, sub_005c7ed0): id = eView kimligi,
 * m = 48 float (gorunum, projeksiyon, carpim; satir vektoru duzeni),
 * cam = guest Camera* (0 olabilir). */
void enh_view_setup(uint32_t id, const float *m, uint32_t cam);
void enh_report(void);
const char *enh_build_name(void);
#else
static inline int  enh_rt_size(uint32_t r, uint32_t bw, uint32_t bh,
                               uint32_t *w, uint32_t *h)
{ (void)r; (void)bw; (void)bh; (void)w; (void)h; return 0; }
static inline float enh_blur(void) { return 1.f; }
static inline void enh_view_setup(uint32_t id, const float *m, uint32_t cam)
{ (void)id; (void)m; (void)cam; }
static inline void enh_report(void) {}
static inline const char *enh_build_name(void) { return "orijinal"; }
#endif

#endif
