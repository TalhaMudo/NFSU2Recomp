/* D3DX HLE Asama 2: bir konuk fonksiyonunun yaptigi IDirect3DDevice9
 * cagrilarinin kaydi ve "simdi yapilsa etkisiz mi" denetimi.
 * Kayit com.c'de tutulur (cihaz durumunun kopyalari orada); kullanan
 * runtime/d3dx_hle.c.  Ayrintilar: D3DX_HLE.md, Asama 2.               */
#ifndef DXREC_H
#define DXREC_H
#include <stdint.h>


typedef struct {
    int16_t  mid;            /* com.c metot kimligi */
    uint16_t doff, dlen;     /* data[] icindeki sabit verisi */
    uint8_t  red;            /* kayit aninda etkisizdi */
    uint32_t a[3];           /* this'ten sonraki ilk 3 arguman */
} DxCall;

/* Kapasite kayit basina: durum kaydi 8 cagri / 256 bayt, gecis kaydi daha
 * buyuk.  call/data dizileri kaydi tasiyan yapinin icinde durur;
 * DXREC_INIT ile baglanir. */
typedef struct {
    uint8_t  kind;           /* DXREC_PASS / DXREC_STATE */
    uint8_t  ok;             /* 0: notlanamaz (desteklenmeyen cagri, ic ice, tasma, durum blogu) */
    uint8_t  allred;         /* kayit sirasinda her cagri etkisizdi */
    uint16_t ncall, maxcall;
    uint16_t ndata, maxdata;
    DxCall  *call;
    uint8_t *data;
} DxRec;
#define DXREC_INIT(r, calls, databuf) do{ (r)->call = (calls); (r)->data = (databuf); \
    (r)->maxcall = (uint16_t)(sizeof(calls)/sizeof((calls)[0])); \
    (r)->maxdata = (uint16_t)sizeof(databuf); (r)->ok = 0; (r)->ncall = (r)->ndata = 0; }while(0)

#define DXREC_PASS  1
#define DXREC_STATE 2
/* Gecis icinde durum kaydi normaldir.  Bir DURUM kaydinin icinde yeni bir
 * kayit baslarsa (ic ice uygulama = bilesik) distaki BUTUN kayitlar
 * gecersizlesir; ayni turden ic ice kayit da distakini gecersiz kilar. */
void dxrec_begin(DxRec *r, int kind);
void dxrec_append(const DxRec *src); /* atlanan bir durumun kayitli cagrilarini acik kayitlara ekle */
void dxrec_end(void);
int  dxrec_redundant(const DxRec *r);/* kaydedilen her cagri SIMDI etkisiz mi */
int  dxrec_sb_recording(void);
const char *dxrec_mid_name(int mid);       /* durum blogu kaydi acik mi (atlama yasak) */

#endif
