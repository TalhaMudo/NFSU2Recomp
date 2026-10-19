/* D3D9 vertex shader (vs_1_1 / vs_2_x) bytecode yorumlayicisi.
 *
 * Neden CPU'da yorumluyoruz: NFSU2 tum cizimlerini DrawPrimitiveUP ile,
 * cizim basina 4 kose olarak yapiyor. Bu olcekte kose islemeyi CPU'da
 * yapmak MSL uretmek + her durum icin ayri pipeline derlemekten hem cok
 * daha basit hem de yeterince hizli.                                    */
#ifndef VSEMU_H
#define VSEMU_H
#include <stdint.h>
#include <stddef.h>

typedef struct VSProg VSProg;

typedef struct {
    float   f[256][4];
    int32_t i[16][4];
    int32_t b[16];
} VSConst;

typedef struct {
    float pos[4];        /* oPos  - kirpma uzayi (clip space) */
    float col[2][4];     /* oD0, oD1 */
    float tex[8][4];     /* oT0..oT7 */
    float fog, psize;
} VSOut;

/* tok: bytecode basi, max_dw: guvenli okunabilir dword sayisi.
 * Basarisizsa NULL. */
VSProg *vs_compile(const uint32_t *tok, size_t max_dw);
void    vs_free(VSProg *p);

/* dcl_usage'lardan: verilen (kullanim, indeks) hangi v# kaydinda?  yoksa -1 */
int  vs_input_reg(const VSProg *p, int usage, int usage_index);

/* v: 16 giris kaydi. Basarida 1. */
int  vs_run(const VSProg *p, const VSConst *k, const float v[16][4], VSOut *o);
/* D3D9 vs_1_1 - vs_2_x renk cikisi kirpmasi (RENKSAT=0 kapatir); surum ana numarasi */
int  vs_colsat_on(void);
/* SON ISLEM ZINCIRI: asama 0 ornegi [[color(in)]], sonuc [[color(out)]] (metal.m) */
int  ps_to_msl_fb(const VSProg *p, int id, char *out, size_t cap, int in_idx, int out_idx);
int  vs_version_major(const VSProg *p);

/* Piksel gölgelendiricisini MSL kaynagina cevir (0 = basarisiz).
 * Uretilen fonksiyonun adi "ps_<id>".  Cagiran taraf VOut/PSPar
 * yapilarini ve ornekleyiciyi saglar. */
int  ps_to_msl(const VSProg *p, int id, char *out, size_t cap);
int  ps_to_msl_ex(const VSProg *p, int id, char *out, size_t cap, unsigned cubemask);
int  ps_to_msl_ex2(const VSProg *p, int id, char *out, size_t cap, unsigned cubemask, int reflk);
int  ps_uses_colorout(const VSProg *p);

/* Kose gölgelendiricisini MSL'ye cevir.  decl[i] = (bayt ofseti, D3DDECLTYPE)
 * ya da ofset < 0 ise o girdi kullanilmiyor.  Uretilen fonksiyon "vs_<id>". */
int  vs_to_msl(const VSProg *p, int id, const int decl[16][2], char *out, size_t cap);

/* Gölgelendiricinin dokundugu en yuksek sabit yazmaci + 1 (VSPar boyutu). */
int  vs_kmax(const VSProg *p);
int  vs_writes_fog(const VSProg *p);
int  vs_pos_creg(const VSProg *p, int *wreg);
int  vs_fog_creg(const VSProg *p);
int  vs_light_creg(const VSProg *p, float ramp[2]);

/* tani: kac komut, hangi surum */
unsigned vs_version(const VSProg *p);
unsigned vs_ninstr(const VSProg *p);
/* Cevrilemeyen komut sayaci (0 ise shader tam calisti) */
unsigned vs_unknown(const VSProg *p);
#include <stdio.h>
void vs_disasm(const VSProg *p, FILE *f);
#endif
