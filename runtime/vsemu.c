#include "vsemu.h"
#include "enhance.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

/* ---- token cozumleme ---- */
#define OPC(t)     ((unsigned)((t) & 0x0000FFFFu))
#define ILEN(t)    ((unsigned)(((t) & 0x0F000000u) >> 24))
#define PRED(t)    (((t) & 0x10000000u) != 0)
#define RN(t)      ((int)((t) & 0x000007FFu))
#define RT(t)      ((int)((((t) & 0x00001800u) >> 8) | (((t) & 0x70000000u) >> 28)))
#define WMASK(t)   ((unsigned)(((t) >> 16) & 0xFu))
#define SATUR(t)   (((t) & 0x00100000u) != 0)
/* Hedef KAYDIRMA degistiricisi (D3DSP_DSTSHIFT, bit 24-27).
 * ps_1_x'te parlaklik olcegi bununla yapilir: _x2 _x4 _x8 _d2 _d4 _d8.
 * Hic uygulanmiyordu -> "mul_x2_sat" yazan her gölgelendirici YARIM
 * parlaklikta cikiyordu (NFSU2'de 23 ps_1_1 gölgelendiricisi var:
 * farlar, yansimalar, yol yuzeyi).  4 bit ISARETLIDIR: 0..7 = x2^n,
 * 8..15 = x2^(n-16), yani 0xF = /2. */
#define DSHIFT(t)  ((int)(((t) >> 24) & 0xFu) >= 8 ? \
                    (int)(((t) >> 24) & 0xFu) - 16 : (int)(((t) >> 24) & 0xFu))
#define SWIZ(t)    ((unsigned)(((t) >> 16) & 0xFFu))
#define SMOD(t)    ((unsigned)(((t) >> 24) & 0xFu))
#define RELADDR(t) (((t) & 0x00002000u) != 0)

/* kayit tipleri */
enum { R_TEMP=0, R_INPUT, R_CONST, R_ADDR, R_RASTOUT, R_ATTROUT, R_OUTPUT,
       R_CONSTINT, R_COLOROUT, R_DEPTHOUT, R_SAMPLER, R_CONST2, R_CONST3,
       R_CONST4, R_CONSTBOOL, R_LOOP, R_TEMPFLOAT16, R_MISCTYPE, R_LABEL,
       R_PREDICATE };

/* opkodlar */
enum { O_NOP=0, O_MOV, O_ADD, O_SUB, O_MAD, O_MUL, O_RCP, O_RSQ, O_DP3, O_DP4,
       O_MIN, O_MAX, O_SLT, O_SGE, O_EXP, O_LOG, O_LIT, O_DST, O_LRP, O_FRC,
       O_M4x4, O_M4x3, O_M3x4, O_M3x3, O_M3x2, O_CALL, O_CALLNZ, O_LOOP,
       O_RET, O_ENDLOOP, O_LABEL, O_DCL, O_POW, O_CRS, O_SGN, O_ABS, O_NRM,
       O_SINCOS, O_REP, O_ENDREP, O_IF, O_IFC, O_ELSE, O_ENDIF, O_BREAK,
       O_BREAKC, O_MOVA, O_DEFB, O_DEFI,
       /* --- piksel gölgelendirici komutlari (D3D numaralandirmasi) --- */
       O_TEXCOORD=64, O_TEXKILL=65, O_TEX=66,   /* ps_1_x tex / ps_2_0 texld */
       O_EXPP=78, O_LOGP=79, O_CND=80, O_DEF=81,
       O_CMP=88, O_DP2ADD=90, O_TEXLDD=95, O_TEXLDL=97,
       O_END=0xFFFF };

typedef struct {
    unsigned op;
    unsigned nsrc;
    int      has_dst;
    unsigned dst;
    unsigned src[4];
    unsigned rel[4];        /* goreli adresleme tokeni, yoksa 0 */
    float    imm[4];        /* def / defi icin */
    int      match;         /* if->else|endif, rep->endrep, loop->endloop */
    int      match2;        /* else->endif */
    unsigned cmp;           /* ifc/breakc karsilastirma kodu */
    /* --- onceden cozulmus operand plani (hiz icin) ---------------------
     * src_read/dst_write her KOSE icin token'i yeniden cozuyordu: bit
     * ayiklama, reg_ptr cagrisi, swizzle dongusu, switch, memcpy.  Oysa
     * bunlarin hepsi yalnizca KOMUTA bagli.  Bir kez burada cozulup
     * kose dongusunde sadece aritmetik birakiliyor. */
    unsigned char fsrc;        /* tum kaynaklar duz (reladdr/mod yok) */
    unsigned char fdst;        /* dst = r#, tam maske, satur yok       */
    signed char   s_type[4];
    short         s_num[4];
    unsigned char s_def[4];    /* sabit def'li mi (p->def kullan)      */
    unsigned char s_sw[4][4];
    unsigned char s_swk[4];    /* 0=birebir(.xyzw) 1=tek bilesen 2=genel */
    short         d_num;
} VSIns;

struct VSProg {
    unsigned  version;
    int       is_ps;            /* 1 = piksel gölgelendiricisi */
    VSIns    *ins;
    unsigned  n;
    float     def[256][4];      /* def ile tanimlanan sabitler */
    unsigned  defmask[8];       /* hangi c# def edildi (bit alani) */
    int32_t   defi[16][4];
    int       inmap[16][2];     /* v# -> (usage, index); usage<0 = kullanilmiyor */
    int       smptype[16];      /* s# doku tipi: 2=2D 3=CUBE 4=VOLUME, 0=bildirilmemis */
    int       maxreg;           /* kullanilan en yuksek gecici yazmac (r#) */
    unsigned  unknown;
};

static int is_param(uint32_t t){ return (t & 0x80000000u) != 0; }

/* ---- derleme ---- */
VSProg *vs_compile(const uint32_t *tok, size_t max_dw){
    if (!tok || max_dw < 2) return 0;
    uint32_t ver = tok[0];
    /* 0xFFFE = vertex shader, 0xFFFF = pixel shader.  Ikisi de ayni token
     * bicimini kullaniyor; piksel gölgelendiricileri (simdilik) yalnizca
     * sokumlemek/incelemek icin kabul ediyoruz. */
    if ((ver >> 16) != 0xFFFEu && (ver >> 16) != 0xFFFFu) return 0;

    VSProg *p = calloc(1, sizeof *p);
    if (!p) return 0;
    p->version = ver & 0xFFFFu;
    p->is_ps = ((ver >> 16) == 0xFFFFu);
    for (int i = 0; i < 16; i++){ p->inmap[i][0] = -1; p->inmap[i][1] = 0; p->smptype[i] = 0; }

    unsigned cap = 64;
    p->ins = malloc(cap * sizeof(VSIns));
    if (!p->ins){ free(p); return 0; }

    size_t i = 1;
    while (i < max_dw){
        uint32_t t = tok[i];
        unsigned op = OPC(t);
        if (op == O_END) break;
        if (op == 0xFFFEu){                       /* comment */
            unsigned len = (t & 0x7FFF0000u) >> 16;
            i += 1 + len; continue;
        }
        if (p->n == cap){
            cap *= 2; VSIns *ni = realloc(p->ins, cap * sizeof(VSIns));
            if (!ni){ vs_free(p); return 0; }
            p->ins = ni;
        }
        VSIns *ix = &p->ins[p->n];
        memset(ix, 0, sizeof *ix);
        ix->op = op;
        ix->match = ix->match2 = -1;
        ix->cmp = (t >> 16) & 0x7u;               /* ifc/breakc icin */

        unsigned len = ILEN(t);
        size_t body = i + 1;                      /* ilk operand tokeni */
        size_t next;

        if (op == O_DEF || op == O_DEFI){
            if (body + 4 >= max_dw){ p->unknown++; break; }
            ix->has_dst = 1; ix->dst = tok[body];
            memcpy(ix->imm, &tok[body+1], 16);
            next = body + 5;
            int rn = RN(ix->dst);
            if (op == O_DEF && rn < 256){
                memcpy(p->def[rn], ix->imm, 16);
                p->defmask[rn >> 5] |= 1u << (rn & 31);
            } else if (op == O_DEFI && rn < 16){
                memcpy(p->defi[rn], &tok[body+1], 16);
            }
        } else if (op == O_DEFB){
            if (body + 1 >= max_dw){ p->unknown++; break; }
            ix->has_dst = 1; ix->dst = tok[body];
            memcpy(ix->imm, &tok[body+1], 4);
            next = body + 2;
        } else if (op == O_DCL){
            if (body + 1 >= max_dw){ p->unknown++; break; }
            uint32_t ut = tok[body];
            ix->dst = tok[body+1]; ix->has_dst = 1;
            next = body + 2;
            if (RT(ix->dst) == R_INPUT){
                int rn = RN(ix->dst);
                if (rn >= 0 && rn < 16){
                    p->inmap[rn][0] = (int)(ut & 0x1Fu);
                    p->inmap[rn][1] = (int)((ut & 0x000F0000u) >> 16);
                }
            }
            /* ps_2_0: `dcl_2d s0` / `dcl_cube s1` -- doku TIPI *DCL
             * token'inin* 27-30. bitlerinde (D3DSAMPLER_TEXTURE_TYPE:
             * 2=2D, 3=CUBE, 4=VOLUME).  DST token'inda DEGIL: SM2+'da
             * dst'in 28-30. bitleri kayit tipinin ust bitleri, oradan
             * okuyunca her sey "4" cikiyor. */
            if (RT(ix->dst) == R_SAMPLER){
                int rn = RN(ix->dst);
                if (rn >= 0 && rn < 16)
                    p->smptype[rn] = (int)((ut >> 27) & 0xFu);
            }
        } else {
            /* operandlari bit31'e gore tara; SM2+ uzunluk alani varsa ona guven */
            size_t j = body;
            if (PRED(t)) { /* yuklem kaydi en sonda; asagida atlanacak */ }
            size_t limit = len ? (body + len) : max_dw;
            if (limit > max_dw) limit = max_dw;
            int first = 1;
            while (j < limit && (len || is_param(tok[j]))){
                uint32_t pt = tok[j];
                if (first && op != O_IF && op != O_IFC && op != O_REP &&
                    op != O_LOOP && op != O_BREAK && op != O_BREAKC &&
                    op != O_CALL && op != O_CALLNZ && op != O_LABEL &&
                    op != O_RET && op != O_ELSE && op != O_ENDIF &&
                    op != O_ENDREP && op != O_ENDLOOP){
                    ix->has_dst = 1; ix->dst = pt; first = 0; j++;
                    continue;
                }
                first = 0;
                if (ix->nsrc < 4){
                    ix->src[ix->nsrc] = pt;
                    if (RELADDR(pt) && p->version >= 0x0200 && j + 1 < limit){
                        ix->rel[ix->nsrc] = tok[j+1]; j++;
                    }
                    ix->nsrc++;
                }
                j++;
            }
            next = len ? (body + len) : j;
        }
        if (next <= i){ p->unknown++; break; }
        i = next;
        p->n++;
    }

    /* akis denetimi eslestirme */
    int stack[64]; int sp = 0;
    for (unsigned k = 0; k < p->n; k++){
        unsigned op = p->ins[k].op;
        if (op == O_IF || op == O_IFC || op == O_REP || op == O_LOOP){
            if (sp < 64) stack[sp++] = (int)k;
        } else if (op == O_ELSE){
            if (sp > 0) p->ins[stack[sp-1]].match = (int)k;   /* if -> else */
            if (sp < 64) stack[sp-1] = (int)k;                /* else -> endif */
        } else if (op == O_ENDIF || op == O_ENDREP || op == O_ENDLOOP){
            if (sp > 0){ int o = stack[--sp];
                if (p->ins[o].op == O_ELSE) p->ins[o].match = (int)k;
                else if (p->ins[o].match < 0) p->ins[o].match = (int)k;
                else p->ins[o].match2 = (int)k;
                p->ins[k].match = o;
            }
        }
    }
    /* Kose basina yalnizca kullanilan yazmaclari temizleyebilmek icin
     * en yuksek gecici yazmac numarasini burada bir kez hesapla. */
    for (unsigned q = 0; q < p->n; q++){
        VSIns *x = &p->ins[q];
        x->fsrc = 1; x->fdst = 0;
        for (unsigned t = 0; t < x->nsrc && t < 4; t++){
            unsigned tok = x->src[t];
            int ty = RT(tok), nu = RN(tok);
            if (RELADDR(tok) || SMOD(tok) != 0u){ x->fsrc = 0; break; }
            if (ty == R_CONST2) nu += 2048; else if (ty == R_CONST3) nu += 4096;
            else if (ty == R_CONST4) nu += 6144;
            if (ty == R_CONST2 || ty == R_CONST3 || ty == R_CONST4) ty = R_CONST;
            if (ty == R_TEMP){ if (nu < 0 || nu >= 32){ x->fsrc = 0; break; } }
            else if (ty == R_INPUT){ if (nu < 0 || nu >= 16){ x->fsrc = 0; break; } }
            else if (ty == R_CONST){ if (nu < 0 || nu >= 256){ x->fsrc = 0; break; } }
            else { x->fsrc = 0; break; }
            x->s_type[t] = (signed char)ty;
            x->s_num[t]  = (short)nu;
            x->s_def[t]  = (ty == R_CONST &&
                            (p->defmask[nu >> 5] & (1u << (nu & 31)))) ? 1u : 0u;
            unsigned sw = SWIZ(tok);
            for (int c2 = 0; c2 < 4; c2++)
                x->s_sw[t][c2] = (unsigned char)((sw >> (2*c2)) & 3u);
            /* En sik iki durumu ayirt et: .xyzw (tek 16 baytlik okuma) ve
             * .xxxx gibi tek-bilesen yayilimi.  Geri kalan genel yol. */
            if (sw == 0xE4u) x->s_swk[t] = 0u;
            else if (x->s_sw[t][0]==x->s_sw[t][1] && x->s_sw[t][1]==x->s_sw[t][2] &&
                     x->s_sw[t][2]==x->s_sw[t][3]) x->s_swk[t] = 1u;
            else x->s_swk[t] = 2u;
        }
        if (x->has_dst && RT(x->dst) == R_TEMP && WMASK(x->dst) == 0xFu &&
            !SATUR(x->dst) && !RELADDR(x->dst)){
            int dn = RN(x->dst);
            if (dn >= 0 && dn < 32){ x->fdst = 1; x->d_num = (short)dn; }
        }
    }
    { int mr = -1;
      for (unsigned q = 0; q < p->n; q++){
          const VSIns *x = &p->ins[q];
          if (x->has_dst && RT(x->dst) == R_TEMP){
              int nnum = RN(x->dst); if (nnum > mr) mr = nnum; }
          for (unsigned t = 0; t < x->nsrc; t++)
              if (RT(x->src[t]) == R_TEMP){
                  int nnum = RN(x->src[t]); if (nnum > mr) mr = nnum; }
      }
      if (mr > 31) mr = 31;
      p->maxreg = mr; }
    return p;
}

void vs_free(VSProg *p){ if (p){ free(p->ins); free(p); } }
unsigned vs_version(const VSProg *p){ return p ? p->version : 0; }
unsigned vs_ninstr(const VSProg *p){ return p ? p->n : 0; }
unsigned vs_unknown(const VSProg *p){ return p ? p->unknown : 0; }

int vs_input_reg(const VSProg *p, int usage, int usage_index){
    if (!p) return -1;
    for (int i = 0; i < 16; i++)
        if (p->inmap[i][0] == usage && p->inmap[i][1] == usage_index) return i;
    return -1;
}

/* ---- yurutme ---- */
typedef struct {
    const VSProg  *p;
    const VSConst *k;
    float r[32][4];
    const float (*v)[4];
    int   a0[4];
    int   aL, aLstep, loopreg;
    VSOut *o;
} Exec;

static const float ZERO4[4] = {0,0,0,0};

static const float *reg_ptr(Exec *e, int type, int num, float tmp[4]){
    switch (type){
    case R_TEMP:   return (num >= 0 && num < 32) ? e->r[num] : ZERO4;
    case R_INPUT:  return (num >= 0 && num < 16) ? e->v[num] : ZERO4;
    case R_CONST:  case R_CONST2: case R_CONST3: case R_CONST4: {
        int n = num;
        if (type == R_CONST2) n += 2048; else if (type == R_CONST3) n += 4096;
        else if (type == R_CONST4) n += 6144;
        if (n < 0 || n >= 256) return ZERO4;
        /* def edilen sabitler oyunun yazdigini ezmez; def yalnizca varsayilandir,
         * ancak D3D'de def sabitleri ayri bir alan degildir: shader'a gomulu
         * sabit her zaman gecerlidir. */
        if (e->p->defmask[n >> 5] & (1u << (n & 31))) return e->p->def[n];
        return e->k->f[n];
    }
    case R_CONSTINT:
        if (num >= 0 && num < 16){
            tmp[0]=(float)e->k->i[num][0]; tmp[1]=(float)e->k->i[num][1];
            tmp[2]=(float)e->k->i[num][2]; tmp[3]=(float)e->k->i[num][3];
            return tmp;
        }
        return ZERO4;
    case R_ADDR:
        tmp[0]=(float)e->a0[0]; tmp[1]=(float)e->a0[1];
        tmp[2]=(float)e->a0[2]; tmp[3]=(float)e->a0[3];
        return tmp;
    case R_LOOP:
        tmp[0]=(float)e->aL; tmp[1]=tmp[2]=tmp[3]=0.f; return tmp;
    default: return ZERO4;
    }
}

static void src_read(Exec *e, unsigned tok, unsigned rel, float out[4]){
    int type = RT(tok), num = RN(tok);
    if (RELADDR(tok)){
        int off = e->a0[0];
        if (rel){
            int rt = RT(rel), rnum = RN(rel);
            unsigned sw = SWIZ(rel) & 3u;
            float t2[4]; const float *rp = reg_ptr(e, rt, rnum, t2);
            off = (int)rp[sw];
            if (rt == R_LOOP) off = e->aL;
        }
        num += off;
    }
    float tmp[4];
    const float *base = reg_ptr(e, type, num, tmp);
    unsigned sw = SWIZ(tok);
    float s[4];
    for (int i = 0; i < 4; i++) s[i] = base[(sw >> (2*i)) & 3u];
    switch (SMOD(tok)){
    case 1: for (int i=0;i<4;i++) s[i] = -s[i]; break;                 /* NEG  */
    case 2: for (int i=0;i<4;i++) s[i] -= 0.5f; break;                 /* BIAS */
    case 3: for (int i=0;i<4;i++) s[i] = -(s[i]-0.5f); break;
    case 4: for (int i=0;i<4;i++) s[i] = 2.f*s[i]-1.f; break;          /* SIGN */
    case 5: for (int i=0;i<4;i++) s[i] = -(2.f*s[i]-1.f); break;
    case 6: for (int i=0;i<4;i++) s[i] = 1.f - s[i]; break;            /* COMP */
    case 7: for (int i=0;i<4;i++) s[i] *= 2.f; break;                  /* X2   */
    case 8: for (int i=0;i<4;i++) s[i] *= -2.f; break;
    case 11:for (int i=0;i<4;i++) s[i] = fabsf(s[i]); break;           /* ABS  */
    case 12:for (int i=0;i<4;i++) s[i] = -fabsf(s[i]); break;
    default: break;
    }
    memcpy(out, s, 16);
}

static void dst_write(Exec *e, unsigned tok, const float val[4]){
    int type = RT(tok), num = RN(tok);
    unsigned mask = WMASK(tok);
    float v[4]; memcpy(v, val, 16);
    { int sh = (e->p && e->p->is_ps) ? DSHIFT(tok) : 0;
      if (sh){ float k = ldexpf(1.f, sh); for (int i=0;i<4;i++) v[i] *= k; } }
    if (SATUR(tok)) for (int i=0;i<4;i++) v[i] = v[i] < 0.f ? 0.f : (v[i] > 1.f ? 1.f : v[i]);
    float *d = 0;
    switch (type){
    case R_TEMP:    if (num >= 0 && num < 32) d = e->r[num]; break;
    case R_RASTOUT: if (num == 0) d = e->o->pos;
                    else if (num == 1){ if (mask & 1) e->o->fog = v[0]; return; }
                    else if (num == 2){ if (mask & 1) e->o->psize = v[0]; return; }
                    break;
    case R_ATTROUT: if (num >= 0 && num < 2) d = e->o->col[num]; break;
    case R_OUTPUT:  if (num >= 0 && num < 8) d = e->o->tex[num]; break;
    case R_ADDR:    for (int i=0;i<4;i++) if (mask & (1u<<i))
                        e->a0[i] = (int)lrintf(v[i]);
                    return;
    default: return;
    }
    if (!d) return;
    for (int i = 0; i < 4; i++) if (mask & (1u << i)) d[i] = v[i];
}

static float safe_rcp(float x){ return (x == 0.f) ? 3.402823466e38f : 1.f/x; }
static float safe_rsq(float x){ x = fabsf(x); return (x == 0.f) ? 3.402823466e38f : 1.f/sqrtf(x); }

static int cmp_ok(unsigned c, float a, float b){
    switch (c){
    case 1: return a >  b;  case 2: return a == b;  case 3: return a >= b;
    case 4: return a <  b;  case 5: return a != b;  case 6: return a <= b;
    default: return a > b;
    }
}

int vs_run(const VSProg *p, const VSConst *k, const float v[16][4], VSOut *o){
    if (!p || !k || !o) return 0;
    /* Kose BASINA 512 baytlik r[32][4] temizligi olcumde en pahali is
     * cikti (surenin ~%45'i vs_run'da).  Yalnizca programin gercekten
     * kullandigi yazmaclari sifirla. */
    Exec e;
    { int mr = p->maxreg; if (mr < 0) mr = -1; if (mr > 31) mr = 31;
      if (mr >= 0) memset(e.r, 0, (size_t)(mr+1) * sizeof e.r[0]); }
    e.a0[0]=e.a0[1]=e.a0[2]=e.a0[3]=0;
    e.aL=0; e.aLstep=0; e.loopreg=0;
    e.p = p; e.k = k; e.v = v; e.o = o;
    memset(o, 0, sizeof *o);
    o->pos[3] = 1.f;
    o->col[0][0]=o->col[0][1]=o->col[0][2]=o->col[0][3]=1.f;

    int retstack[16]; int rsp = 0;
    struct { int start, count, iter, step, idx; } lp[8]; int lsp = 0;

    unsigned steps = 0;
    for (unsigned pc = 0; pc < p->n; pc++){
        if (++steps > 200000u) break;
        const VSIns *x = &p->ins[pc];
        float s[4][4];
#define DSTW(_x,_d) do{ if ((_x)->fdst) memcpy(e.r[(_x)->d_num],(_d),16); \
                        else dst_write(&e,(_x)->dst,(_d)); }while(0)
        if (x->fsrc){
            for (unsigned i = 0; i < x->nsrc; i++){
                const float *b;
                switch (x->s_type[i]){
                case R_TEMP:  b = e.r[x->s_num[i]]; break;
                case R_INPUT: b = e.v[x->s_num[i]]; break;
                default:      b = x->s_def[i] ? p->def[x->s_num[i]]
                                              : k->f[x->s_num[i]]; break;
                }
                if (x->s_swk[i] == 0u) memcpy(s[i], b, 16);
                else if (x->s_swk[i] == 1u){
                    float t2 = b[x->s_sw[i][0]];
                    s[i][0]=t2; s[i][1]=t2; s[i][2]=t2; s[i][3]=t2;
                } else {
                    const unsigned char *sw = x->s_sw[i];
                    s[i][0]=b[sw[0]]; s[i][1]=b[sw[1]];
                    s[i][2]=b[sw[2]]; s[i][3]=b[sw[3]];
                }
            }
        } else
            for (unsigned i = 0; i < x->nsrc; i++) src_read(&e, x->src[i], x->rel[i], s[i]);
        float d[4] = {0,0,0,0};
        switch (x->op){
        case O_NOP: case O_DCL: case O_DEF: case O_DEFI: case O_DEFB:
        case O_LABEL: case O_ENDIF: case O_ELSE:
            if (x->op == O_ELSE && x->match >= 0) pc = (unsigned)x->match;  /* if dalindan geldi */
            break;
        case O_MOV: memcpy(d, s[0], 16); DSTW(x,d); break;
        case O_MOVA:
            for (int i=0;i<4;i++) d[i]=s[0][i];
            DSTW(x,d); break;
        case O_ADD: for(int i=0;i<4;i++) d[i]=s[0][i]+s[1][i]; DSTW(x,d); break;
        case O_SUB: for(int i=0;i<4;i++) d[i]=s[0][i]-s[1][i]; DSTW(x,d); break;
        case O_MUL: for(int i=0;i<4;i++) d[i]=s[0][i]*s[1][i]; DSTW(x,d); break;
        case O_MAD: for(int i=0;i<4;i++) d[i]=s[0][i]*s[1][i]+s[2][i]; DSTW(x,d); break;
        case O_MIN: for(int i=0;i<4;i++) d[i]=s[0][i]<s[1][i]?s[0][i]:s[1][i]; DSTW(x,d); break;
        case O_MAX: for(int i=0;i<4;i++) d[i]=s[0][i]>s[1][i]?s[0][i]:s[1][i]; DSTW(x,d); break;
        case O_SLT: for(int i=0;i<4;i++) d[i]=s[0][i]<s[1][i]?1.f:0.f; DSTW(x,d); break;
        case O_SGE: for(int i=0;i<4;i++) d[i]=s[0][i]>=s[1][i]?1.f:0.f; DSTW(x,d); break;
        case O_FRC: for(int i=0;i<4;i++) d[i]=s[0][i]-floorf(s[0][i]); DSTW(x,d); break;
        case O_ABS: for(int i=0;i<4;i++) d[i]=fabsf(s[0][i]); DSTW(x,d); break;
        case O_SGN: for(int i=0;i<4;i++) d[i]=(s[0][i]>0.f)-(s[0][i]<0.f); DSTW(x,d); break;
        case O_LRP: for(int i=0;i<4;i++) d[i]=s[1][i]+s[0][i]*(s[2][i]-s[1][i]); DSTW(x,d); break;
        case O_RCP: { float r=safe_rcp(s[0][0]); d[0]=d[1]=d[2]=d[3]=r; DSTW(x,d); } break;
        case O_RSQ: { float r=safe_rsq(s[0][0]); d[0]=d[1]=d[2]=d[3]=r; DSTW(x,d); } break;
        case O_DP3: { float r=s[0][0]*s[1][0]+s[0][1]*s[1][1]+s[0][2]*s[1][2];
                      d[0]=d[1]=d[2]=d[3]=r; DSTW(x,d); } break;
        case O_DP4: { float r=s[0][0]*s[1][0]+s[0][1]*s[1][1]+s[0][2]*s[1][2]+s[0][3]*s[1][3];
                      d[0]=d[1]=d[2]=d[3]=r; DSTW(x,d); } break;
        case O_CRS: d[0]=s[0][1]*s[1][2]-s[0][2]*s[1][1];
                    d[1]=s[0][2]*s[1][0]-s[0][0]*s[1][2];
                    d[2]=s[0][0]*s[1][1]-s[0][1]*s[1][0];
                    DSTW(x,d); break;
        case O_NRM: { float l=sqrtf(s[0][0]*s[0][0]+s[0][1]*s[0][1]+s[0][2]*s[0][2]);
                      float r=(l==0.f)?0.f:1.f/l;
                      d[0]=s[0][0]*r; d[1]=s[0][1]*r; d[2]=s[0][2]*r; d[3]=r;
                      DSTW(x,d); } break;
        case O_POW: { float b=fabsf(s[0][0]); float r=(b==0.f)?0.f:powf(b,s[1][0]);
                      d[0]=d[1]=d[2]=d[3]=r; DSTW(x,d); } break;
        case O_EXP: { float r=exp2f(s[0][0]); d[0]=d[1]=d[2]=d[3]=r;
                      DSTW(x,d); } break;
        case O_LOG: { float a=fabsf(s[0][0]);
                      float r=(a==0.f)?-3.402823466e38f:log2f(a);
                      d[0]=d[1]=d[2]=d[3]=r; DSTW(x,d); } break;
        /* DIKKAT: expp/logp, exp/log'un "dusuk hassasiyetli" hali DEGILDIR;
         * dort bilesene FARKLI degerler yazarlar.  Ikisi de exp2/log2 olarak
         * cevriliyordu.  NFSU2'nin deri-eklem gölgelendiricisi
         * "expp r6.y, v4.zzzz" ile kemik indisinin KESIR kismini aliyor;
         * biz .y'ye exp2 yazinca ucuncu kemik matrisinin indisi cop oluyor
         * ve kalabalik/ara sahne karakterleri dagiliyordu. */
        case O_EXPP: { float w=s[0][0], f=floorf(w);
                      d[0]=exp2f(f); d[1]=w-f; d[2]=exp2f(w); d[3]=1.f;
                      DSTW(x,d); } break;
        case O_LOGP: { float a=fabsf(s[0][0]);
                      if (a==0.f){ d[0]=d[2]=-3.402823466e38f; d[1]=1.f; d[3]=1.f; }
                      else { float e=floorf(log2f(a));
                             d[0]=e; d[1]=a/exp2f(e); d[2]=log2f(a); d[3]=1.f; }
                      DSTW(x,d); } break;
        case O_SINCOS: d[0]=cosf(s[0][0]); d[1]=sinf(s[0][0]); d[2]=d[3]=0.f;
                       DSTW(x,d); break;
        case O_LIT: { float nl=s[0][0], nh=s[0][1], pw=s[0][3];
                      if (pw < -128.f) pw = -128.f; if (pw > 128.f) pw = 128.f;
                      d[0]=1.f; d[1]=(nl>0.f)?nl:0.f;
                      d[2]=(nl>0.f&&nh>0.f)?powf(nh,pw):0.f; d[3]=1.f;
                      DSTW(x,d); } break;
        case O_DST: d[0]=1.f; d[1]=s[0][1]*s[1][1]; d[2]=s[0][2]; d[3]=s[1][3];
                    DSTW(x,d); break;
        case O_CND: for(int i=0;i<4;i++) d[i]= s[0][i]>0.5f ? s[1][i] : s[2][i];
                    DSTW(x,d); break;
        case O_M4x4: case O_M4x3: case O_M3x4: case O_M3x3: case O_M3x2: {
            int rows = (x->op==O_M4x4||x->op==O_M3x4) ? 4 :
                       (x->op==O_M4x3||x->op==O_M3x3) ? 3 : 2;
            int cols = (x->op==O_M4x4||x->op==O_M4x3) ? 4 : 3;
            unsigned base = x->src[1];
            for (int rw = 0; rw < rows; rw++){
                unsigned t2 = (base & ~0x7FFu) | ((RN(base) + rw) & 0x7FFu);
                float m[4]; src_read(&e, t2, x->rel[1], m);
                float acc = 0.f;
                for (int cc = 0; cc < cols; cc++) acc += s[0][cc]*m[cc];
                d[rw] = acc;
            }
            dst_write(&e, x->dst, d);
        } break;
        case O_IF:  if (!(s[0][0] != 0.f)){ pc = (x->match >= 0) ? (unsigned)x->match : pc; } break;
        case O_IFC: if (!cmp_ok(x->cmp, s[0][0], s[1][0])){ pc = (x->match >= 0) ? (unsigned)x->match : pc; } break;
        case O_REP: {
            int cnt = (int)s[0][0];
            if (cnt <= 0){ if (x->match >= 0) pc = (unsigned)x->match; }
            else if (lsp < 8){ lp[lsp].start=(int)pc; lp[lsp].count=cnt;
                               lp[lsp].iter=0; lp[lsp].step=0; lp[lsp].idx=0; lsp++; }
        } break;
        case O_LOOP: {
            int cnt = (int)s[1][0], init = (int)s[1][1], step = (int)s[1][2];
            if (cnt <= 0){ if (x->match >= 0) pc = (unsigned)x->match; }
            else if (lsp < 8){ lp[lsp].start=(int)pc; lp[lsp].count=cnt;
                               lp[lsp].iter=0; lp[lsp].step=step; lp[lsp].idx=init;
                               e.aL = init; lsp++; }
        } break;
        case O_ENDREP: case O_ENDLOOP:
            if (lsp > 0){
                lp[lsp-1].iter++;
                if (lp[lsp-1].iter < lp[lsp-1].count){
                    lp[lsp-1].idx += lp[lsp-1].step; e.aL = lp[lsp-1].idx;
                    pc = (unsigned)lp[lsp-1].start;
                } else lsp--;
            }
            break;
        case O_BREAK: if (lsp > 0){ int st=lp[lsp-1].start;
                        if (p->ins[st].match >= 0) pc = (unsigned)p->ins[st].match;
                        lsp--; } break;
        case O_BREAKC: if (cmp_ok(x->cmp, s[0][0], s[1][0]) && lsp > 0){
                        int st=lp[lsp-1].start;
                        if (p->ins[st].match >= 0) pc = (unsigned)p->ins[st].match;
                        lsp--; } break;
        case O_CALL: case O_CALLNZ: {
            if (x->op == O_CALLNZ && s[1][0] == 0.f) break;
            int target = -1;
            int want = RN(x->src[0]);
            for (unsigned q = 0; q < p->n; q++)
                if (p->ins[q].op == O_LABEL && RN(p->ins[q].src[0]) == want){ target=(int)q; break; }
            if (target >= 0 && rsp < 16){ retstack[rsp++] = (int)pc; pc = (unsigned)target; }
        } break;
        case O_RET: if (rsp > 0) pc = (unsigned)retstack[--rsp]; else pc = p->n; break;
        default: break;
        }
    }
    return 1;
}

/* ---- tani: sokum ---- */
static const char *OPN(unsigned op){
    switch(op){
    case O_NOP:return"nop"; case O_MOV:return"mov"; case O_ADD:return"add";
    case O_SUB:return"sub"; case O_MAD:return"mad"; case O_MUL:return"mul";
    case O_RCP:return"rcp"; case O_RSQ:return"rsq"; case O_DP3:return"dp3";
    case O_DP4:return"dp4"; case O_MIN:return"min"; case O_MAX:return"max";
    case O_SLT:return"slt"; case O_SGE:return"sge"; case O_EXP:return"exp";
    case O_LOG:return"log"; case O_LIT:return"lit"; case O_DST:return"dst";
    case O_LRP:return"lrp"; case O_FRC:return"frc"; case O_M4x4:return"m4x4";
    case O_M4x3:return"m4x3"; case O_M3x4:return"m3x4"; case O_M3x3:return"m3x3";
    case O_M3x2:return"m3x2"; case O_CALL:return"call"; case O_CALLNZ:return"callnz";
    case O_LOOP:return"loop"; case O_RET:return"ret"; case O_ENDLOOP:return"endloop";
    case O_LABEL:return"label"; case O_DCL:return"dcl"; case O_POW:return"pow";
    case O_CRS:return"crs"; case O_SGN:return"sgn"; case O_ABS:return"abs";
    case O_NRM:return"nrm"; case O_SINCOS:return"sincos"; case O_REP:return"rep";
    case O_ENDREP:return"endrep"; case O_IF:return"if"; case O_IFC:return"ifc";
    case O_ELSE:return"else"; case O_ENDIF:return"endif"; case O_BREAK:return"break";
    case O_BREAKC:return"breakc"; case O_MOVA:return"mova"; case O_DEF:return"def";
    case O_DEFI:return"defi"; case O_DEFB:return"defb"; case O_EXPP:return"expp";
    case O_LOGP:return"logp"; case O_CND:return"cnd";
    default: return "???";
    }
}
static void regname(char *b, size_t n, unsigned tok){
    static const char *T[]={"r","v","c","a","oPos","oD","oT","i","oC","oDepth",
                            "s","c","c","c","b","aL","h","misc","l","p"};
    int ty=RT(tok), num=RN(tok);
    const char *nm = (ty>=0 && ty<20) ? T[ty] : "?";
    if (ty==R_RASTOUT) snprintf(b,n,"%s", num==0?"oPos":num==1?"oFog":"oPts");
    else snprintf(b,n,"%s%d%s", nm, num, RELADDR(tok)?"[a0]":"");
}
void vs_disasm(const VSProg *p, FILE *f){
    if (!p) return;
    fprintf(f,"    vs_%u_%u, %u komut\n", p->version>>8, p->version&0xFF, p->n);
    for (unsigned k=0;k<p->n;k++){
        const VSIns *x=&p->ins[k];
        char b[32];
        fprintf(f,"      %2u %-7s", k, OPN(x->op));
        if (x->has_dst){
            regname(b,sizeof b,x->dst);
            unsigned m=WMASK(x->dst);
            fprintf(f," %s",b);
            if (m && m!=0xF){ fputc('.',f);
                if(m&1)fputc('x',f); if(m&2)fputc('y',f);
                if(m&4)fputc('z',f); if(m&8)fputc('w',f); }
        }
        for (unsigned i=0;i<x->nsrc;i++){
            regname(b,sizeof b,x->src[i]);
            unsigned sw=SWIZ(x->src[i]);
            fprintf(f,", %s%s",SMOD(x->src[i])==1?"-":"",b);
            if (sw!=0xE4u){ static const char C[]="xyzw"; fputc('.',f);
                for(int j=0;j<4;j++) fputc(C[(sw>>(2*j))&3],f); }
        }
        if (x->op==O_DEF) fprintf(f," = %g %g %g %g",
            x->imm[0],x->imm[1],x->imm[2],x->imm[3]);
        if (x->op==O_DCL){
            static const char *US[]={"POSITION","BLENDWEIGHT","BLENDINDICES","NORMAL",
                "PSIZE","TEXCOORD","TANGENT","BINORMAL","TESSFACTOR","POSITIONT",
                "COLOR","FOG","DEPTH","SAMPLE"};
            int rn=RN(x->dst);
            if (RT(x->dst)==R_INPUT && rn<16 && p->inmap[rn][0]>=0)
                fprintf(f,"   <- %s%d",
                        p->inmap[rn][0]<14?US[p->inmap[rn][0]]:"?", p->inmap[rn][1]);
        }
        fputc('\n',f);
    }
}

/* ====================================================================
 * Piksel gölgelendirici -> Metal Shading Language
 *
 * Oyunun kendi ps_1_x / ps_2_0 gölgelendiricilerini GPU'da aynen
 * calistirmak icin MSL'ye ceviriyoruz.  Sabit bir "renk * doku"
 * yaklasimi araba boyasini ve isigi dogru veremiyordu; birebir sonuc
 * icin gölgelendiricinin kendisi gerekiyor.
 *
 * Yazmac eslemesi:  r# -> r[#]   t# (R_ADDR) -> t[#]   v# -> v[#]
 *                   c# -> def'liyse gomulu sabit, degilse P.c[#]
 * Cikis: ps_1_x'te r0, ps_2_0'da oC0 -> her ikisi de "oC".
 * ==================================================================== */
static const char SWCH[4] = { 'x','y','z','w' };
/* Sabit tamponunun MSL'deki adi: piksel gölgelendiricide "P", köşede "K".
 * msl_src iki ureticide de paylasildigi icin buradan secilir. */
static const char *g_cbuf = "P";

/* DIKKAT: bu uretici once yalnizca PIKSEL gölgelendiricisi icin yazilmisti.
 * Köşe tarafinda yazmac uzayi farkli: v0..v15 (piksel: v0/v1), r0..r31
 * (piksel: r0..r7) ve R_ADDR piksel tarafinda doku yazmaci, köşe tarafinda
 * adres yazmaci a0.  Kipe bakmadan uretmek v2+ girdilerini v[0]'a, r8+
 * yazmaclarini r[0]'a katliyordu. */
#define VSMODE() (g_cbuf[0] == 'K')
static void msl_reg(char *o, size_t cap, const VSProg *p, unsigned tok, unsigned rel){
    int ty = RT(tok), n = RN(tok);
    int vs = VSMODE();
    switch (ty){
    case R_TEMP:     snprintf(o, cap, "r[%d]", n & (vs ? 31 : 15)); break;
    case R_INPUT:    snprintf(o, cap, "v[%d]", n & (vs ? 15 : 1));  break;
    case R_ADDR:     if (vs) snprintf(o, cap, "float4(a0)");
                     else    snprintf(o, cap, "t[%d]", n & 3);      break;
    case R_LOOP:     snprintf(o, cap, "float4(float(aL))");         break;
    case R_COLOROUT: snprintf(o, cap, "oC");           break;
    case R_CONSTINT: if (n >= 0 && n < 16)
                         snprintf(o, cap, "float4(%d.0,%d.0,%d.0,%d.0)",
                                  p->defi[n][0], p->defi[n][1],
                                  p->defi[n][2], p->defi[n][3]);
                     else snprintf(o, cap, "float4(0.0)");
                     break;
    case R_CONST: case R_CONST2: case R_CONST3: case R_CONST4: {
        int idx = n;
        if (ty == R_CONST2) idx += 2048; else if (ty == R_CONST3) idx += 4096;
        else if (ty == R_CONST4) idx += 6144;
        /* goreli adresleme: c[a0.<bilesen> + idx] */
        if (RELADDR(tok) && vs){
            char comp = 'x';
            if (rel){
                if (RT(rel) == R_LOOP) comp = 0;         /* aL */
                else comp = SWCH[SWIZ(rel) & 3u];
            }
            if (comp) snprintf(o, cap, "%s.c[clamp(a0.%c + %d, 0, 255)]",
                               g_cbuf, comp, idx);
            else      snprintf(o, cap, "%s.c[clamp(aL + %d, 0, 255)]", g_cbuf, idx);
            break;
        }
        if (idx >= 0 && idx < 256 && (p->defmask[idx >> 5] & (1u << (idx & 31))))
            snprintf(o, cap, "float4(%g,%g,%g,%g)", (double)p->def[idx][0],
                     (double)p->def[idx][1], (double)p->def[idx][2],
                     (double)p->def[idx][3]);
        else snprintf(o, cap, "%s.c[%d]", g_cbuf, (idx & (vs ? 255 : 31)));
        break; }
    default: snprintf(o, cap, "float4(0.0)"); break;
    }
}

/* Kaynak operandi: yazmac + swizzle + kaynak degistirici */
static void msl_src(char *o, size_t cap, const VSProg *p, unsigned tok, unsigned rel){
    char reg[96]; msl_reg(reg, sizeof reg, p, tok, rel);
    unsigned sw = SWIZ(tok);
    char sws[8];
    if (sw == 0xE4u) sws[0] = 0;                 /* .xyzw = swizzle yok */
    else { sws[0]='.';
           for (int i=0;i<4;i++) sws[1+i] = SWCH[(sw >> (2*i)) & 3u];
           sws[5]=0; }
    char body[128];
    snprintf(body, sizeof body, "%s%s", reg, sws);
    switch (SMOD(tok)){
    case 1:  snprintf(o, cap, "(-%s)", body); break;                    /* NEG  */
    case 2:  snprintf(o, cap, "(%s - 0.5)", body); break;               /* BIAS */
    case 3:  snprintf(o, cap, "(-(%s - 0.5))", body); break;
    case 4:  snprintf(o, cap, "(2.0*%s - 1.0)", body); break;           /* SIGN */
    case 5:  snprintf(o, cap, "(-(2.0*%s - 1.0))", body); break;
    case 6:  snprintf(o, cap, "(1.0 - %s)", body); break;               /* COMP */
    case 7:  snprintf(o, cap, "(2.0*%s)", body); break;                 /* X2   */
    case 8:  snprintf(o, cap, "(-2.0*%s)", body); break;
    case 11: snprintf(o, cap, "abs(%s)", body); break;                  /* ABS  */
    case 12: snprintf(o, cap, "(-abs(%s))", body); break;
    default: snprintf(o, cap, "%s", body); break;
    }
}

/* Hedef: yazmac + yazma maskesi.  ncomp maskedeki bilesen sayisini verir. */
static int msl_dst(char *o, size_t cap, const VSProg *p, unsigned tok){
    char reg[96]; msl_reg(reg, sizeof reg, p, tok, 0);
    unsigned m = WMASK(tok);
    if (m == 0xFu || m == 0u){ snprintf(o, cap, "%s", reg); return 4; }
    char sws[8]; int k = 0; sws[k++]='.';
    for (int i=0;i<4;i++) if (m & (1u<<i)) sws[k++] = SWCH[i];
    sws[k]=0;
    snprintf(o, cap, "%s%s", reg, sws);
    return k-1;
}

/* Hedef yazma maskesinin bilesen harfleri (".yz" gibi); tam maske -> "".
 * DIKKAT: D3D'de maske BILESEN-BILESEN calisir (r0.yz = r1  ->  r0.y=r1.y,
 * r0.z=r1.z).  Onceki surum ifadeyi basa sikistiriyordu ((expr).xy), yani
 * bitisik olmayan her maskede YANLIS bileseni aliyordu. */
static void msl_mask_swz(char *o, size_t cap, unsigned tok){
    unsigned m = WMASK(tok);
    if (cap) o[0] = 0;
    if (!m || m == 0xFu || cap < 6) return;
    size_t k = 0; o[k++] = '.';
    for (int i = 0; i < 4; i++) if (m & (1u<<i)) o[k++] = SWCH[i];
    o[k] = 0;
}

/* Bir ifadeyi hedefin bilesen sayisina daralt (float4 -> float3 vb.) */
static void msl_fit(char *o, size_t cap, const char *expr, int n){
    if (n >= 4) snprintf(o, cap, "%s", expr);
    else if (n == 1) snprintf(o, cap, "(%s).x", expr);
    else if (n == 2) snprintf(o, cap, "(%s).xy", expr);
    else             snprintf(o, cap, "(%s).xyz", expr);
}

int ps_uses_colorout(const VSProg *p);
/* cubemask: hangi asamalara KUP doku bagli (bit k = asama k).
 * D3D9'da ps_1_x doku TIPINI bildirmez -- tip baglamadan gelir; bu yuzden
 * ayni gölgelendiriciden iki varyant uretiyoruz ve cizim aninda bagli
 * dokuya gore secliyoruz. */
/* reflk >= 0 (yalnizca HD): YANSIMA AYRISTIRMA varyanti.  k. asamanin
 * ornegi (yol yansimasi ya da kup harita) gövdede dogrusal kullaniliyor
 * (olculdu: ps 5, 6, 265 -- arada saturate yok).  Gövde uc kez calisir:
 *   ev 0: gercek ornek      -> renk (orijinalle birebir)
 *   ev 1: ornek.rgb = 0     -> A
 *   ev 2: ornek.rgb = 1     -> A + B
 * G-tamponuna B = o2 - o1 (sis dahil) ve orijinal ornek R0 yazilir; sahne
 * sonunda birlestirme gecisi renk + B*(R_yeni - R0) hesaplar (bkz.
 * metal.m "HD: SSR").  Sinif: 1 = 2B (yol), 2 = kup (araba).          */
int ps_to_msl_ex2(const VSProg *p, int id, char *out, size_t cap, unsigned cubemask, int reflk);
int ps_to_msl_ex(const VSProg *p, int id, char *out, size_t cap, unsigned cubemask){
    return ps_to_msl_ex2(p, id, out, cap, cubemask, -1);
}
int ps_to_msl_ex2(const VSProg *p, int id, char *out, size_t cap, unsigned cubemask, int reflk){
    if (!p || !out || cap < 512) return 0;
#if !NFS_ENHANCED
    if (reflk >= 0) return 0;
#endif
    g_cbuf = "P";                       /* piksel: P.c[0..31] */
    size_t o = 0;
    #define EMIT(...) do{ int _w = snprintf(out+o, cap-o, __VA_ARGS__); \
                          if (_w < 0 || (size_t)_w >= cap-o) return 0; o += (size_t)_w; }while(0)

    { int anycube = 0;
      for (int q = 0; q < 4; q++) if (p->smptype[q] == 3) anycube = 1;
      if (anycube || getenv("SMPTYPE")){
          fprintf(stderr, "  [ps%d] ornekleyici tipleri: s0=%d s1=%d s2=%d s3=%d%s\n",
                  id, p->smptype[0], p->smptype[1], p->smptype[2], p->smptype[3],
                  anycube ? "   <- KUP VAR" : "");
      } }
    EMIT("fragment %s ps_%d(VOut i [[stage_in]],\n", NFS_ENHANCED ? "PSOut" : "float4", id);
    for (int k = 0; k < 4; k++)
        EMIT("    %s<float> s%d [[texture(%d)]],\n",
             ((cubemask >> k) & 1u) ? "texturecube" : "texture2d", k, k);
    /* ASAMA BASINA ORNEKLEYICI.  Once dort dokunun hepsi tek "sm"yi
     * kullaniyordu, yani asama 0'in adres kipi digerlerine de gidiyordu.
     * Yolun diffuse dokusu WRAP; far isigi dokusu ve sonum rampasi ise
     * CLAMP ister.  WRAP ile far konisi TUM yol boyunca tekrarlaniyor ve
     * zemin bastan basa patlak parliyordu. */
    EMIT("    sampler sm0 [[sampler(0)]], sampler sm1 [[sampler(1)]],\n");
    EMIT("    sampler sm2 [[sampler(2)]], sampler sm3 [[sampler(3)]],\n");
    EMIT("    constant PSPar &P [[buffer(0)]]) {\n");
    EMIT("  float4 r[16]; for (int q=0;q<16;q++) r[q]=float4(0.0);\n");
    EMIT("  float4 t[4]; for (int q=0;q<4;q++) t[q]=float4(0.0);\n");
    EMIT("  float4 v[2] = { i.col0, i.col1 };\n");
    EMIT("  float4 oC = float4(0.0);\n");
    /* Asama -> koordinat kumesi eslemesi D3DTSS_TEXCOORDINDEX ile degisir
     * (izde 422 bin cagride asama 1 ve 2 koordinat 0'i kullaniyor) ve
     * D3DTTFF_PROJECTED bolme ister.  Ikisi de P.proj'da: idx + 16*kip. */
    EMIT("#define TSEL(i) (int(P.proj[i]))\n");
    /* DIKKAT: doku koordinati DINAMIK INDEKSLI bir yerel diziden
     * (float4 uv[4]) okunuyordu.  Metal derleyicisi boyle bir diziyi
     * scratch bellege koyuyor; ornekleme koordinati artik dogrudan
     * interpolanttan gelmedigi icin DOKU TUREVLERI (mip secimi)
     * bozuluyor ve her ucgen KENARINDA bir-iki seviye kaba mip
     * seciliyordu.  Ekranda her dortgenin cercevesi ve kosegeni
     * 1 piksellik cizgi olarak goruluyordu (harflerde en belirgin;
     * olculdu: kosegen uzerinde 2676 -> 533 koyu piksel).
     * Cozum: diziyi kaldirip interpolantlar arasindan TEKIL secim.
     * Secim kosulu tekduze (uniform buffer) oldugu icin dallanma
     * turevleri etkilemiyor. */
    /* Ornekleme koordinati DOGRUDAN interpolanttan gelmeli.  Eskiden ternary
     * zinciri + kosullu bolme ile uretiliyordu; turev dortgeni ucgen kenarina
     * tastiginda LOD patliyor ve HER dortgenin cercevesinde / iki ucgenin
     * paylastigi kosegende 2 piksellik cizgi olusuyordu.  (Olculdu: kenarda
     * LOD >> 1; lodMaxClamp=1 cizgiyi siliyordu, mip icerigi ise eksiksiz.)
     * D3DTSS_TEXCOORDINDEX secimi artik KOSE golgelendiricisinde yapiliyor,
     * burada asama k dogrudan i.uvk okuyor.  Yansitmali bolme tek ve
     * SUREKLI: eski "abs(w)<1e-6 ? 1.0 : w" korumasi kenarda 1.0 ile 1e-6
     * arasinda ziplayip koordinati alti buyukluk mertebesi kaydiriyordu. */
    /* Yalnizca GERCEKTEN ornekleme yapilan asamalar icin hesapla.  Dordunu
     * birden uretmek her pikselde 4 bolme + 8 secim demekti (olculdu: yaris
     * karesi 15.7 -> 18.7 ms). */
    unsigned tcuse = 0;
    for (unsigned q2 = 0; q2 < p->n; q2++){
        const VSIns *y = &p->ins[q2];
        if (y->op != O_TEX && y->op != O_TEXLDD && y->op != O_TEXLDL) continue;
        if (y->nsrc == 1 && RT(y->src[0]) == R_ADDR) tcuse |= 1u << (RN(y->src[0]) & 3);
        else if (y->nsrc == 0)                       tcuse |= 1u << (RN(y->dst) & 3);
    }
    for (int k = 0; k < 4; k++){
        if (!(tcuse & (1u << k))) continue;
        EMIT("  float2 tc%d; { float4 q = i.uv%d;"
             " float d = (TSEL(%d)>>4)==2 ? q.w : (TSEL(%d)>>4)==1 ? q.z : 1.0;"
             " tc%d = q.xy / d; }\n", k, k, k, k, k);
    }
#if NFS_ENHANCED
    /* HD bulaniklik kipi (bkz. enhance.c): P.pad0 > 0 iken coklu ornekli
     * 2B gölgelendiricide ornek ofsetleri merkeze gore P.pad0 ile olceklenir
     * ve dokular P.pad1 mip seviyesinden okunur.  Yalnizca yol yansimasi
     * bulaniklik cizimlerinde acilir; diger her cizimde pad0 = 0 (dal
     * sabit tampondan, yani tum dalga icin ayni). */
    int hdblur = __builtin_popcount(tcuse) >= 2 && !(cubemask & tcuse);
    if (hdblur){
        int n = __builtin_popcount(tcuse);
        EMIT("  if (P.pad0 > 0.0) { float2 m = float2(0.0)");
        for (int k = 0; k < 4; k++) if (tcuse & (1u << k)) EMIT(" + tc%d", k);
        EMIT("; m /= %d.0;", n);
        for (int k = 0; k < 4; k++) if (tcuse & (1u << k)) EMIT(" tc%d = m + (tc%d - m) * P.pad0;", k, k);
        EMIT(" }\n");
    }
#else
    int hdblur = 0;
#endif
    /* Asama basina ornekleme: 2B'de uv2, kupte uv3 kullanilir. */
    for (int k = 0; k < 4; k++){
        if (hdblur && !((cubemask >> k) & 1u))
            EMIT("#define SMP%d(uv2, uv3) (P.pad0 > 0.0 ? s%d.sample(sm%d, uv2, level(P.pad1)) : s%d.sample(sm%d, uv2))\n",
                 k, k, k, k, k);
        else
            EMIT("#define SMP%d(uv2, uv3) s%d.sample(sm%d, %s)\n",
                 k, k, k, ((cubemask >> k) & 1u) ? "uv3" : "uv2");
    }
    EMIT("#define TPICK(k) i.uv##k\n");
    EMIT("#define TCRD(k) tc##k\n");
    if (reflk >= 0){
        EMIT("  float4 hd_o[3]; float4 hd_rs = float4(0.0);\n");
        EMIT("  for (int hd_ev = 0; hd_ev < 3; hd_ev++) {\n");
        EMIT("  for (int q=0;q<16;q++) r[q]=float4(0.0);\n");
        EMIT("  for (int q=0;q<4;q++) t[q]=float4(0.0);\n");
        EMIT("  oC = float4(0.0);\n");
    }
    int hd_seen = 0;

    for (unsigned q = 0; q < p->n; q++){
        const VSIns *x = &p->ins[q];
        char d[128], s0[160], s1[160], s2b[160], ex[512], fit[560];
        int nc = x->has_dst ? msl_dst(d, sizeof d, p, x->dst) : 4;
        if (x->nsrc > 0) msl_src(s0,  sizeof s0,  p, x->src[0], x->rel[0]);
        if (x->nsrc > 1) msl_src(s1,  sizeof s1,  p, x->src[1], x->rel[1]);
        if (x->nsrc > 2) msl_src(s2b, sizeof s2b, p, x->src[2], x->rel[2]);
        int sat = x->has_dst && SATUR(x->dst);
        ex[0] = 0;

        switch (x->op){
        case O_NOP: case O_DCL: case O_DEF: case O_DEFI: case O_DEFB:
        case O_LABEL: case O_END:
            continue;
        case O_TEX: case O_TEXLDD: case O_TEXLDL: {
            /* ps_1_x : "tex tN"            -> tN = ornekle(sN, uvN)
             * ps_1_4 : "texld rN, tM"      -> rN = ornekle(sN, uvM)
             * ps_2_0 : "texld dst, coord, sampler"
             *
             * DIKKAT: sonuc EskIDEN HER ZAMAN t[] dizisine yaziliyordu.
             * ps_1_4/ps_2_0'da hedef bir GECICI yazmac (r0) olabiliyor;
             * o zaman sonraki komutlar sifir okuyor ve gölgelendirici
             * siyah donduruyordu.  Belirti: bleach-bypass acikken tum
             * 3B sahne kararıyordu (ana menu tamamen siyah) -- birlestirme
             * gölgelendiricileri "t[0]=ornekle(...)" yazip hemen ardindan
             * r[0] okuyordu.  Artik hedef, diger komutlarla ayni ortak
             * yolla (maske/saturate/kaydirma dahil) yaziliyor. */
            int dn = RN(x->dst) & 3;
            if (x->nsrc >= 2){                     /* ps_2_0 */
                int smp = RN(x->src[1]) & 3;
                char cs[160]; msl_src(cs, sizeof cs, p, x->src[0], x->rel[0]);
                snprintf(ex, sizeof ex, "SMP%d((%s).xy, (%s).xyz)", smp, cs, cs);
            } else if (x->nsrc == 1 && RT(x->src[0]) == R_ADDR){
                /* ps_1_4: kaynak tM doku KOORDINATI, ornek DEGERI degil */
                snprintf(ex, sizeof ex, "SMP%d(TCRD(%d), TPICK(%d).xyz)",
                         dn, RN(x->src[0]) & 3, RN(x->src[0]) & 3);
            } else if (x->nsrc == 1){
                char cs[160]; msl_src(cs, sizeof cs, p, x->src[0], x->rel[0]);
                snprintf(ex, sizeof ex, "SMP%d((%s).xy, (%s).xyz)", dn, cs, cs);
            } else {                               /* ps_1_x: tex tN */
                snprintf(ex, sizeof ex, "SMP%d(TCRD(%d), TPICK(%d).xyz)", dn, dn, dn);
                if (reflk >= 0 && dn == reflk){
                    /* yalnizca bu bicimi destekliyoruz: hedef t[dn] */
                    EMIT("  t[%d] = %s;\n", dn, ex);
                    EMIT("  if (hd_ev == 0) hd_rs = t[%d]; else t[%d].rgb = float3(hd_ev == 1 ? 0.0 : 1.0);\n", dn, dn);
                    hd_seen = 1;
                    continue;
                }
            }
            break; }
        case O_TEXCOORD: {
            int dn = RN(x->dst) & 3;
            EMIT("  t[%d] = TPICK(%d);\n", dn, dn);
            continue; }
        case O_TEXKILL:
            EMIT("  if (any((%s).xyz < 0.0)) discard_fragment();\n", s0);
            continue;
        case O_MOV:    snprintf(ex, sizeof ex, "%s", s0); break;
        case O_ADD:    snprintf(ex, sizeof ex, "(%s) + (%s)", s0, s1); break;
        case O_SUB:    snprintf(ex, sizeof ex, "(%s) - (%s)", s0, s1); break;
        case O_MUL:    snprintf(ex, sizeof ex, "(%s) * (%s)", s0, s1); break;
        case O_MAD:    snprintf(ex, sizeof ex, "(%s) * (%s) + (%s)", s0, s1, s2b); break;
        case O_LRP:    snprintf(ex, sizeof ex, "mix((%s), (%s), (%s))", s2b, s1, s0); break;
        case O_MIN:    snprintf(ex, sizeof ex, "min((%s), (%s))", s0, s1); break;
        case O_MAX:    snprintf(ex, sizeof ex, "max((%s), (%s))", s0, s1); break;
        case O_DP3:    snprintf(ex, sizeof ex, "float4(dot((%s).xyz, (%s).xyz))", s0, s1); break;
        case O_DP4:    snprintf(ex, sizeof ex, "float4(dot((%s), (%s)))", s0, s1); break;
        case O_DP2ADD: snprintf(ex, sizeof ex, "float4(dot((%s).xy,(%s).xy) + (%s).x)", s0, s1, s2b); break;
        case O_RCP:    snprintf(ex, sizeof ex, "float4((%s).x == 0.0 ? 3.402823466e38 : 1.0/(%s).x)", s0, s0); break;
        case O_RSQ:    snprintf(ex, sizeof ex, "float4(abs((%s).x) == 0.0 ? 3.402823466e38 : rsqrt(abs((%s).x)))", s0, s0); break;
        case O_FRC:    snprintf(ex, sizeof ex, "fract(%s)", s0); break;
        case O_ABS:    snprintf(ex, sizeof ex, "abs(%s)", s0); break;
        case O_CMP:    snprintf(ex, sizeof ex, "select((%s), (%s), (%s) >= 0.0)", s2b, s1, s0); break;
        case O_CND:    snprintf(ex, sizeof ex, "select((%s), (%s), (%s) > 0.5)", s2b, s1, s0); break;
        case O_POW:    snprintf(ex, sizeof ex, "float4(pow(abs((%s).x), (%s).x))", s0, s1); break;
        case O_NRM:    snprintf(ex, sizeof ex, "float4(length((%s).xyz) == 0.0 ? float3(0.0) : normalize((%s).xyz), 1.0)", s0, s0); break;
        case O_EXP: snprintf(ex, sizeof ex, "float4(exp2((%s).x))", s0); break;
        /* expp: (2^floor(w), frac(w), 2^w, 1) -- exp'in esdegeri DEGIL */
        case O_EXPP: snprintf(ex, sizeof ex,
                        "float4(exp2(floor((%s).x)), fract((%s).x), exp2((%s).x), 1.0)",
                        s0, s0, s0); break;
        case O_LOGP: snprintf(ex, sizeof ex,
                        "float4(floor(log2(max(abs((%s).x),1e-38))),"
                        " abs((%s).x)*exp2(-floor(log2(max(abs((%s).x),1e-38)))),"
                        " log2(max(abs((%s).x),1e-38)), 1.0)", s0, s0, s0, s0); break;
        case O_LOG: snprintf(ex, sizeof ex,
                       "float4((%s).x == 0.0 ? -3.4e38 : log2(abs((%s).x)))", s0, s0); break;
        default:
            continue;                       /* desteklenmeyen: atla */
        }
        if (!x->has_dst) continue;
        { char mz[8]; msl_mask_swz(mz, sizeof mz, x->dst);
          int sh = DSHIFT(x->dst);
          if (sh) snprintf(fit, sizeof fit, "((%s)%s * %.9g)", ex, mz, ldexp(1.0, sh));
          else    snprintf(fit, sizeof fit, "(%s)%s", ex, mz); }
        if (sat) EMIT("  %s = saturate(%s);\n", d, fit);
        else     EMIT("  %s = %s;\n", d, fit);
    }
    /* ps_1_x sonucu r0'da, ps_2_0 oC0'da.  Hangisi yazildiysa onu dondur. */
    EMIT("  float4 res = %s;\n", ps_uses_colorout(p) ? "oC" : "r[0]");
    EMIT("  if (res.a < P.aref) discard_fragment();\n");
    EMIT("#undef TSEL\n#undef TPICK\n#undef TCRD\n");
    /* D3D9 sis: C = f*Cs + (1-f)*Cfog, yalnizca RGB.  P.fog.a = FOGENABLE. */
    EMIT("  res.rgb = mix(P.fog.rgb, res.rgb, mix(1.0, saturate(i.fog), P.fog.a));\n");
    if (reflk >= 0){
        if (!hd_seen) return 0;             /* asama farkli bicimde: varyant yok */
        EMIT("  hd_o[hd_ev] = res;\n  }\n");
        EMIT("  PSOut hd_out; hd_out.c0 = hd_o[0];\n");
        EMIT("  hd_out.c1 = float4(hd_o[2].rgb - hd_o[1].rgb, %d.0);\n", ((cubemask >> reflk) & 1u) ? 2 : 1);
        EMIT("  hd_out.c2 = float4(saturate(hd_rs.rgb), 1.0);\n");
        /* kup: ornekleme vektoru = yansima yonu (araba boyasi icin isin) */
        if ((cubemask >> reflk) & 1u) EMIT("  hd_out.c3 = float4(i.uv%d.xyz, 1.0);\n", reflk);
        else                          EMIT("  hd_out.c3 = float4(0.0);\n");
        EMIT("  return hd_out;\n}\n");
    } else if (NFS_ENHANCED){
        /* HD: yansimasiz opak cizim G-tamponunu SIFIRLAR (onundeki yolun
         * verisini ortmeli); karisimli cizimlerde yazma maskesi kapali. */
        EMIT("  PSOut hd_out; hd_out.c0 = res; hd_out.c1 = float4(0.0); hd_out.c2 = float4(0.0); hd_out.c3 = float4(0.0);\n");
        EMIT("  return hd_out;\n}\n");
    } else
        EMIT("  return res;\n}\n");
    #undef EMIT
    return (int)o;
}

/* Gölgelendirici oC0'a mi yaziyor (ps_2_0) yoksa r0'a mi (ps_1_x)? */
int ps_uses_colorout(const VSProg *p){
    if (!p) return 0;
    for (unsigned q = 0; q < p->n; q++)
        if (p->ins[q].has_dst && RT(p->ins[q].dst) == R_COLOROUT) return 1;
    return 0;
}

/* ====================================================================
 * KOSE (vertex) gölgelendiricisi -> Metal Shading Language
 *
 * Kose donusumu simdiye kadar CPU'da YORUMLANIYORDU: olcumde calisma
 * suresinin ~%45'i vs_run icindeydi ve tavan ~3.5 M kose/sn idi.  Oyunun
 * kendi vs_1_1/vs_2_x bytecode'unu MSL'ye cevirip GPU'da kosturuyoruz.
 *
 * Kose verisi GPU'ya HAM gonderilir; okuma yerlesimi (offset/tip) D3D
 * kose bildiriminden alinip gölgelendiriciye GOMULUR, boylece
 * MTLVertexDescriptor kombinasyon patlamasi olmaz.
 * ==================================================================== */

/* D3DDECLTYPE -> MSL ifadesi (ham bayt tamponundan oku).
 * DIKKAT: com.c'deki decl_fetch ile BIREBIR ayni olmali; ayrisirsa CPU ve
 * GPU yollari sessizce farkli goruntu uretir.  (Ilk surumde UBYTE4 yanlisla
 * 255'e bolunuyordu ve 8..12 tipleri hic yoktu.) */
static int decl_read_msl(char *o, size_t cap, unsigned type, unsigned off){
    switch (type){
    case 0: snprintf(o,cap,"float4(LDF(%u),0.0,0.0,1.0)",off); break;          /* FLOAT1 */
    case 1: snprintf(o,cap,"float4(LDF(%u),LDF(%u),0.0,1.0)",off,off+4); break;/* FLOAT2 */
    case 2: snprintf(o,cap,"float4(LDF(%u),LDF(%u),LDF(%u),1.0)",
                     off,off+4,off+8); break;                                   /* FLOAT3 */
    case 3: snprintf(o,cap,"float4(LDF(%u),LDF(%u),LDF(%u),LDF(%u))",
                     off,off+4,off+8,off+12); break;                            /* FLOAT4 */
    case 4: snprintf(o,cap,"float4(LDB(%u),LDB(%u),LDB(%u),LDB(%u))/255.0",
                     off+2,off+1,off,off+3); break;                             /* D3DCOLOR */
    case 5: snprintf(o,cap,"float4(LDB(%u),LDB(%u),LDB(%u),LDB(%u))",
                     off,off+1,off+2,off+3); break;                             /* UBYTE4  */
    case 8: snprintf(o,cap,"float4(LDB(%u),LDB(%u),LDB(%u),LDB(%u))/255.0",
                     off,off+1,off+2,off+3); break;                             /* UBYTE4N */
    case 6: snprintf(o,cap,"float4(LDS(%u),LDS(%u),0.0,1.0)",off,off+2); break; /* SHORT2 */
    case 7: snprintf(o,cap,"float4(LDS(%u),LDS(%u),LDS(%u),LDS(%u))",
                     off,off+2,off+4,off+6); break;                             /* SHORT4 */
    case 9: snprintf(o,cap,"float4(LDS(%u)/32767.0,LDS(%u)/32767.0,0.0,1.0)",
                     off,off+2); break;                                         /* SHORT2N */
    case 10:snprintf(o,cap,"float4(LDS(%u),LDS(%u),LDS(%u),LDS(%u))/32767.0",
                     off,off+2,off+4,off+6); break;                             /* SHORT4N */
    case 11:snprintf(o,cap,"float4(LDU(%u)/65535.0,LDU(%u)/65535.0,0.0,1.0)",
                     off,off+2); break;                                         /* USHORT2N */
    case 12:snprintf(o,cap,"float4(LDU(%u),LDU(%u),LDU(%u),LDU(%u))/65535.0",
                     off,off+2,off+4,off+6); break;                             /* USHORT4N */
    default: return 0;             /* bilinmiyor -> ceviriyi reddet */
    }
    return 1;
}

/* dst yazmaci -> MSL hedefi.  Cikislar: oPos, oD0/oD1, oT0..oT3 */
static int vs_dst_msl(char *o, size_t cap, unsigned tok){
    int ty = RT(tok), n = RN(tok);
    switch (ty){
    case R_TEMP:    snprintf(o,cap,"r[%d]", n & 31); return 1;
    case R_ADDR:    snprintf(o,cap,"a0");            return 1;
    case R_RASTOUT: if (n==0){ snprintf(o,cap,"oPos"); return 1; }
                    if (n==1){ snprintf(o,cap,"oFog"); return 1; }
                    snprintf(o,cap,"dummy"); return 1;
    case R_ATTROUT: snprintf(o,cap,"oD[%d]", n & 1); return 1;
    case R_OUTPUT:  snprintf(o,cap,"oT[%d]", n & 3); return 1;
    default:        snprintf(o,cap,"dummy"); return 1;
    }
}

/* Gölgelendiricinin dokundugu EN YUKSEK sabit yazmaci (+1).  Sabit
 * tamponunu cizim basina 4 KB yerine yalnizca gerektigi kadar
 * yuklemek icin.  Goreli adresleme varsa sinir bilinemez -> 256. */
int vs_kmax(const VSProg *p){
    if (!p) return 0;
    int mx = -1;
    for (unsigned q = 0; q < p->n; q++){
        const VSIns *x = &p->ins[q];
        int rows = 1;
        if (x->op==O_M4x4 || x->op==O_M3x4) rows = 4;
        else if (x->op==O_M4x3 || x->op==O_M3x3) rows = 3;
        else if (x->op==O_M3x2) rows = 2;
        for (unsigned t = 0; t < x->nsrc; t++){
            unsigned tok = x->src[t];
            int ty = RT(tok);
            if (ty != R_CONST && ty != R_CONST2 && ty != R_CONST3 && ty != R_CONST4)
                continue;
            if (RELADDR(tok)) return 256;
            int idx = RN(tok);
            if (ty == R_CONST2) idx += 2048; else if (ty == R_CONST3) idx += 4096;
            else if (ty == R_CONST4) idx += 6144;
            if (idx < 0 || idx > 255) return 256;
            int top = idx + ((t == 1) ? rows - 1 : 0);
            if (top > mx) mx = top;
        }
    }
    if (mx < 0) mx = 0;
    if (mx > 255) mx = 255;
    return mx + 1;
}

/* Gölgelendirici oFog (RASTOUT#1) yaziyor mu?  D3D9'da kose gölgelendiricisi
 * varken sis carpani bu yazmactan gelir; sabit islevli asama pikseli
 * FOGCOLOR'a dogru karistirir.  Biz oFog'u atiyorduk -> hic sis yoktu. */
/* oPos'u ureten SABIT YAZMACI bul.  Konum matrisi her golgelendiricide
 * c0..c3'te DEGIL (olculdu: vs_9'da c0..c3 baska sabitleri tutuyor ve
 * "c3.w==0 -> perspektif" testi yanlis pozitif veriyordu).
 * Doner: oPos.x'i ureten c# (yoksa -1); *wreg = oPos.w'yi ureten c#. */
int vs_pos_creg(const VSProg *p, int *wreg){
    int cx = -1, cw = -1;
    if (wreg) *wreg = -1;
    if (!p) return -1;
    /* GECICI YAZMAC IZI: yol gölgelendiricileri (vs_4, vs_14) konumu once
     * gecicilere hesaplayip sonra tasiyor:
     *     dp4 r1.x, v0, c0 ... dp4 r0.w, v0, c3 ... mov oPos.x, r1.xxxx
     *     mov oPos.yzw, r0
     * Eski surum yalnizca DOGRUDAN oPos'a yazan dp4/m4x4'u goruyordu ve
     * bu gölgelendiricilerde -1 donuyordu -> genis ekran (Hor+) duzeltmesi
     * YOL YUZEYINE HIC UYGULANMIYORDU (yol yatayda 1/0.75 gerili).
     * tsrc[yazmac][bilesen] = o bileseni "dp4 v0, c#" ile ureten sabit. */
    int16_t tsrc[32][4];
    memset(tsrc, 0xFF, sizeof tsrc);
    for (unsigned q = 0; q < p->n; q++){
        const VSIns *x = &p->ins[q];
        if (!x->has_dst) continue;
        unsigned m = WMASK(x->dst); if (!m) m = 0xFu;
        if (RT(x->dst) == R_TEMP && RN(x->dst) < 32){
            int k = -1;
            if (x->op == O_DP4 && x->nsrc >= 2 && RT(x->src[1]) == R_CONST && !RELADDR(x->src[1]) &&
                RT(x->src[0]) == R_INPUT)
                k = RN(x->src[1]);
            for (int c = 0; c < 4; c++) if (m & (1u << c)) tsrc[RN(x->dst)][c] = (int16_t)k;
            continue;
        }
        if (RT(x->dst) != R_RASTOUT || RN(x->dst) != 0) continue;   /* oPos degil */
        if (x->op == O_M4x4 || x->op == O_M4x3){
            if (x->nsrc >= 2 && RT(x->src[1]) == R_CONST && !RELADDR(x->src[1])){
                cx = RN(x->src[1]); cw = cx + 3; }
        } else if (x->op == O_DP4 && x->nsrc >= 2 &&
                   RT(x->src[1]) == R_CONST && !RELADDR(x->src[1])){
            if (m & 1u) cx = RN(x->src[1]);
            if (m & 8u) cw = RN(x->src[1]);
        } else if (x->op == O_MOV && x->nsrc >= 1 && RT(x->src[0]) == R_TEMP &&
                   RN(x->src[0]) < 32 && SMOD(x->src[0]) == 0u){
            for (int c = 0; c < 4; c += 3){                 /* yalnizca x ve w */
                if (!(m & (1u << c))) continue;
                int sc = (int)((SWIZ(x->src[0]) >> (2 * c)) & 3u);
                int k = tsrc[RN(x->src[0])][sc];
                if (k >= 0){ if (c == 0) cx = k; else cw = k; }
            }
        }
    }
    if (wreg) *wreg = cw;
    return cx;
}

/* Sis sabitlerinin yazmaci: NFSU2 kalibi (vs_4, vs_14, ...)
 *     add r.w, -r.wwww, cK.xxxx ; mul r.w, r.wwww, cK.yyyy ; max r.w, r.wwww, cK.zzzz
 *     min oFog, r.wwww, c#.yyyy
 * yani sis = clamp((cK.x - derinlik) * cK.y, cK.z, 1).  Doner: K ya da -1. */
int vs_writes_fog(const VSProg *p);
int vs_fog_creg(const VSProg *p){
    if (!p) return -1;
    int k = -1;
    for (unsigned q = 0; q < p->n; q++){
        const VSIns *x = &p->ins[q];
        if (!x->has_dst || x->op != O_MAX || x->nsrc < 2) continue;
        if (RT(x->dst) != R_TEMP || RT(x->src[1]) != R_CONST || RELADDR(x->src[1])) continue;
        if (((SWIZ(x->src[1])) & 3u) != 2u) continue;          /* .zzzz */
        k = RN(x->src[1]);
    }
    return (k >= 0 && vs_writes_fog(p)) ? k : -1;
}

/* Far isigi izdusumu (vs_13 sahne, vs_14 yol):
 *     dp4 oT1.x, v0, cK ; .y cK+1 ; .z cK+2 ; oT1.w = v0.c[K+3]
 *     mad oT2.x, r, cA.xxxx, cA.yyyy        (sonum rampasi = olcek*w + kayma)
 * Donus K (kalip yoksa -1); ramp[0..1] = olcek, kayma.  HD isin izleme sabit
 * kopyalarda far terimini isabet noktasinda bu matrisle yeniden kurar. */
int vs_light_creg(const VSProg *p, float ramp[2]){
    if (!p) return -1;
    int pr = vs_input_reg(p, 0, 0), k[3] = { -1, -1, -1 };
    for (unsigned q = 0; q < p->n; q++){
        const VSIns *x = &p->ins[q];
        if (!x->has_dst || x->op != O_DP4 || x->nsrc < 2) continue;
        if (RT(x->dst) != R_OUTPUT || RN(x->dst) != 1) continue;
        if (RT(x->src[0]) != R_INPUT || RN(x->src[0]) != pr) continue;
        if (RT(x->src[1]) != R_CONST || RELADDR(x->src[1])) continue;
        unsigned m = WMASK(x->dst);
        if (m == 1u) k[0] = RN(x->src[1]); else if (m == 2u) k[1] = RN(x->src[1]); else if (m == 4u) k[2] = RN(x->src[1]);
    }
    if (k[0] < 0 || k[1] != k[0] + 1 || k[2] != k[0] + 2 || k[0] + 3 > 255) return -1;
    if (ramp){
        ramp[0] = 0.005f; ramp[1] = 0.5f;
        for (unsigned q = 0; q < p->n; q++){
            const VSIns *x = &p->ins[q];
            if (!x->has_dst || x->op != O_MAD || x->nsrc < 3) continue;
            if (RT(x->dst) != R_OUTPUT || RN(x->dst) != 2 || !(WMASK(x->dst) & 1u)) continue;
            if (RT(x->src[1]) != R_CONST || RT(x->src[2]) != R_CONST) continue;
            int a = RN(x->src[1]), b = RN(x->src[2]);
            if (a > 255 || b > 255) continue;
            if (!(p->defmask[a >> 5] & (1u << (a & 31))) || !(p->defmask[b >> 5] & (1u << (b & 31)))) continue;
            ramp[0] = p->def[a][SWIZ(x->src[1]) & 3u]; ramp[1] = p->def[b][SWIZ(x->src[2]) & 3u];
        }
    }
    return k[0];
}

int vs_writes_fog(const VSProg *p){
    if (!p) return 0;
    for (unsigned q = 0; q < p->n; q++){
        const VSIns *x = &p->ins[q];
        if (x->has_dst && RT(x->dst) == R_RASTOUT && RN(x->dst) == 1) return 1;
    }
    return 0;
}

int vs_to_msl(const VSProg *p, int id, const int decl[16][2], char *out, size_t cap){
    if (!p || !out || cap < 1024) return 0;
    g_cbuf = "K";                       /* kose: K.c[0..255] */
    size_t o = 0;
    #define VEMIT(...) do{ int _w = snprintf(out+o, cap-o, __VA_ARGS__); \
                           if (_w < 0 || (size_t)_w >= cap-o) return 0; o += (size_t)_w; }while(0)

    /* Sabit tamponu gölgelendirici basina boyutlandirilir: cizim basina
     * 4 KB yuklemek yerine yalnizca dokunulan sabitler gonderilir. */
    { int nk = vs_kmax(p); if (nk < 1) nk = 1;
      VEMIT("struct VSPar { uint stride; uint tsel; uint pad1; uint pad2;\n");
      VEMIT("               float4 c[%d]; };\n", nk); }
    VEMIT("vertex VOut vs_%d(uint vid [[vertex_id]],\n", id);
    VEMIT("    device const uchar *VB [[buffer(1)]],\n");
    VEMIT("    constant VSPar &K [[buffer(2)]]) {\n");
    VEMIT("  device const uchar *B = VB + vid * K.stride;\n");
    VEMIT("#define LDF(x)  (as_type<float>(*(device const uint*)(B+(x))))\n");
    VEMIT("#define LDB(x)  ((float)(B[(x)]))\n");
    VEMIT("#define LDS(x)  ((float)(*(device const short*)(B+(x))))\n");
    VEMIT("#define LDU(x)  ((float)(*(device const ushort*)(B+(x))))\n");
    size_t body_a = o;                  /* (HD) yakalama cekirdegi ayni govdeyi kullanir */
    VEMIT("  float4 r[32]; for (int q=0;q<32;q++) r[q]=float4(0.0);\n");
    VEMIT("  float4 v[16]; for (int q=0;q<16;q++) v[q]=float4(0.0,0.0,0.0,1.0);\n");
    VEMIT("  float4 oPos=float4(0.0,0.0,0.0,1.0), dummy=float4(0.0);\n");
    /* oFog varsayilani 1.0 = SIS YOK; gölgelendirici yazmazsa dogru davranis. */
    VEMIT("  float4 oFog=float4(1.0);\n");
    VEMIT("  float4 oD[2]  = { float4(1.0), float4(0.0) };\n");
    VEMIT("  float4 oT[4]  = { float4(0.0), float4(0.0), float4(0.0), float4(0.0) };\n");
    VEMIT("  int4 a0 = int4(0); int aL = 0;\n");

    /* Girdi yazmaclarini bildirimden oku */
    for (int i = 0; i < 16; i++){
        if (decl[i][0] < 0) continue;                 /* kullanilmiyor */
        char e[160];
        if (!decl_read_msl(e, sizeof e, (unsigned)decl[i][1], (unsigned)decl[i][0])){
            g_cbuf = "P";
            return 0;                                 /* CPU yoluna dus */
        }
        VEMIT("  v[%d] = %s;\n", i, e);
    }

    for (unsigned q = 0; q < p->n; q++){
        const VSIns *x = &p->ins[q];
        char d[96], s0[160], s1[160], s2b[160], ex[512], fit[560];
        int nc = 4;
        if (x->has_dst){ vs_dst_msl(d, sizeof d, x->dst);
                         unsigned m = WMASK(x->dst);
                         if (m && m != 0xFu){
                             char sw[8]; int k=0; sw[k++]='.';
                             for (int c2=0;c2<4;c2++) if (m & (1u<<c2)) sw[k++]=SWCH[c2];
                             sw[k]=0; strncat(d, sw, sizeof d - strlen(d) - 1);
                             nc = k-1;
                         } }
        if (x->nsrc > 0) msl_src(s0,  sizeof s0,  p, x->src[0], x->rel[0]);
        if (x->nsrc > 1) msl_src(s1,  sizeof s1,  p, x->src[1], x->rel[1]);
        if (x->nsrc > 2) msl_src(s2b, sizeof s2b, p, x->src[2], x->rel[2]);
        ex[0] = 0;

        switch (x->op){
        case O_NOP: case O_DCL: case O_DEF: case O_DEFI: case O_DEFB:
        case O_LABEL: case O_END: continue;
        case O_MOV:  snprintf(ex,sizeof ex,"%s", s0); break;
        case O_ADD:  snprintf(ex,sizeof ex,"(%s) + (%s)", s0, s1); break;
        case O_SUB:  snprintf(ex,sizeof ex,"(%s) - (%s)", s0, s1); break;
        case O_MUL:  snprintf(ex,sizeof ex,"(%s) * (%s)", s0, s1); break;
        case O_MAD:  snprintf(ex,sizeof ex,"(%s) * (%s) + (%s)", s0, s1, s2b); break;
        case O_MIN:  snprintf(ex,sizeof ex,"min((%s),(%s))", s0, s1); break;
        case O_MAX:  snprintf(ex,sizeof ex,"max((%s),(%s))", s0, s1); break;
        case O_SLT:  snprintf(ex,sizeof ex,"select(float4(0.0),float4(1.0),(%s)<(%s))", s0, s1); break;
        case O_SGE:  snprintf(ex,sizeof ex,"select(float4(0.0),float4(1.0),(%s)>=(%s))", s0, s1); break;
        case O_DP3:  snprintf(ex,sizeof ex,"float4(dot((%s).xyz,(%s).xyz))", s0, s1); break;
        case O_DP4:  snprintf(ex,sizeof ex,"float4(dot((%s),(%s)))", s0, s1); break;
        case O_LRP:  snprintf(ex,sizeof ex,"mix((%s),(%s),(%s))", s2b, s1, s0); break;
        case O_FRC:  snprintf(ex,sizeof ex,"fract(%s)", s0); break;
        case O_ABS:  snprintf(ex,sizeof ex,"abs(%s)", s0); break;
        case O_RCP:  snprintf(ex,sizeof ex,"float4((%s).x==0.0?3.402823466e38:1.0/(%s).x)", s0, s0); break;
        case O_RSQ:  snprintf(ex,sizeof ex,"float4(abs((%s).x)==0.0?3.402823466e38:rsqrt(abs((%s).x)))", s0, s0); break;
        case O_EXP: snprintf(ex,sizeof ex,"float4(exp2((%s).x))", s0); break;
        case O_EXPP: snprintf(ex,sizeof ex,
                        "float4(exp2(floor((%s).x)), fract((%s).x), exp2((%s).x), 1.0)",
                        s0, s0, s0); break;
        case O_LOG: case O_LOGP: snprintf(ex,sizeof ex,
                        "float4((%s).x==0.0?-3.4e38:log2(abs((%s).x)))", s0, s0); break;
        case O_POW:  snprintf(ex,sizeof ex,"float4(pow(abs((%s).x),(%s).x))", s0, s1); break;
        case O_CRS:  snprintf(ex,sizeof ex,"float4(cross((%s).xyz,(%s).xyz),1.0)", s0, s1); break;
        case O_NRM:  snprintf(ex,sizeof ex,"float4(length((%s).xyz)==0.0?float3(0.0):normalize((%s).xyz),1.0)", s0, s0); break;
        case O_SGN:  snprintf(ex,sizeof ex,"sign(%s)", s0); break;
        case O_M4x4: case O_M4x3: case O_M3x3: case O_M3x4: case O_M3x2: {
            /* m?x? dst, src0, src1 : src1 ARDISIK sabit yazmaclarin ilki.
             * Satir sayisi komuta gore degisir. */
            int rows = (x->op==O_M4x4) ? 4 : (x->op==O_M4x3 ? 3 :
                       (x->op==O_M3x3 ? 3 : (x->op==O_M3x4 ? 4 : 2)));
            int vec3 = (x->op==O_M3x3 || x->op==O_M3x4 || x->op==O_M3x2);
            char rowbuf[4][96];
            for (int rr = 0; rr < rows && rr < 4; rr++){
                unsigned tok2 = x->src[1];
                int ty2 = RT(tok2), nn2 = RN(tok2) + rr;
                if (ty2 == R_CONST2) nn2 += 2048; else if (ty2 == R_CONST3) nn2 += 4096;
                else if (ty2 == R_CONST4) nn2 += 6144;
                if (ty2 == R_TEMP) snprintf(rowbuf[rr], 96, "r[%d]", nn2 & 31);
                else if (ty2 == R_INPUT) snprintf(rowbuf[rr], 96, "v[%d]", nn2 & 15);
                else snprintf(rowbuf[rr], 96, "K.c[%d]", nn2 & 255);
            }
            const char *sw2 = vec3 ? ".xyz" : "";
            char acc[512]; size_t ao = 0;
            ao += (size_t)snprintf(acc+ao, sizeof acc-ao, "float4(");
            for (int rr = 0; rr < rows; rr++)
                ao += (size_t)snprintf(acc+ao, sizeof acc-ao, "%sdot((%s)%s,(%s)%s)",
                                       rr?",":"", s0, sw2, rowbuf[rr], sw2);
            for (int rr = rows; rr < 4; rr++)
                ao += (size_t)snprintf(acc+ao, sizeof acc-ao, ",%s", rr==3?"1.0":"0.0");
            snprintf(acc+ao, sizeof acc-ao, ")");
            snprintf(ex, sizeof ex, "%s", acc);
            break; }
        case O_SINCOS: snprintf(ex,sizeof ex,
                        "float4(cos((%s).x), sin((%s).x), 0.0, 0.0)", s0, s0); break;
        case O_MOVA: snprintf(ex,sizeof ex,"%s", s0); break;
        case O_LIT:  snprintf(ex,sizeof ex,
                        "float4(1.0, max((%s).x,0.0), ((%s).x>0.0)?pow(max((%s).y,0.0),"
                        "clamp((%s).w,-127.9,127.9)):0.0, 1.0)", s0,s0,s0,s0); break;
        case O_DST:  snprintf(ex,sizeof ex,
                        "float4(1.0,(%s).y*(%s).y,(%s).z,(%s).w)", s0,s1,s0,s1); break;
        default:
            /* Cevrilemeyen komut (akis denetimi, cagri, vb.).  Sessizce
             * atlamak YANLIS gölgelendirici uretir; ceviriyi tumden
             * reddedip kanitlanmis CPU yoluna dusuyoruz. */
            g_cbuf = "P";
            return 0;
        }
        if (!x->has_dst) continue;
        if (x->op == O_MOVA){
            VEMIT("  a0 = int4(round(%s));\n", ex);
            continue;
        }
        { char mz[8]; msl_mask_swz(mz, sizeof mz, x->dst);
          snprintf(fit, sizeof fit, "(%s)%s", ex, mz); }
        if (SATUR(x->dst)) VEMIT("  %s = saturate(%s);\n", d, fit);
        else               VEMIT("  %s = %s;\n", d, fit);
    }

    size_t body_b = o;
    VEMIT("  VOut O;\n");
    VEMIT("  O.pos = oPos;\n");
    VEMIT("  O.col0 = oD[0]; O.col1 = oD[1];\n");
    /* D3DTSS_TEXCOORDINDEX: asama k, oT[sel] okur.  Secim BURADA yapilir ki
     * piksel golgelendiricisinde koordinat saf interpolant kalsin. */
    VEMIT("  O.uv0 = oT[(K.tsel      ) & 3u]; O.uv1 = oT[(K.tsel >>  8) & 3u];\n");
    VEMIT("  O.uv2 = oT[(K.tsel >> 16) & 3u]; O.uv3 = oT[(K.tsel >> 24) & 3u];\n");
    VEMIT("  O.fog = oFog.x;\n");
    VEMIT("  (void)dummy; (void)a0; (void)aL;\n");
    VEMIT("#undef LDF\n#undef LDB\n#undef LDS\n#undef LDU\n");
    VEMIT("  return O;\n}\n");
#if NFS_ENHANCED
    /* YAKALAMA CEKIRDEGI (isin izleme): ayni govde, cikti tampona.
     * Bir dagitim ayni gölgelendiriciyi kullanan TUM cizimleri kapsar;
     * kose basina ITEM[g] cizim kaydini verir (ham kose ve sabit ofseti).
     * Dunya konumu kirpma uzayindan iptal hatasi OLMADAN cikar: perspektifte
     * oPos.w = gorus derinligi, yani gorus = (x/P00', y/P11, w) ve
     * dunya = gorus * V^-1 (yakin 0.5 / uzak 16 km'de z-w farki float32'de
     * coker; bu yol o farki hic kullanmaz). */
    {
        size_t blen = body_b - body_a;
        VEMIT("kernel void vsk_%d(uint tid [[thread_position_in_grid]],\n", id);
        VEMIT("    device const uchar *VBASE [[buffer(1)]], device const uchar *KBASE [[buffer(2)]],\n");
        VEMIT("    device const uint *ITEM [[buffer(3)]], device const HDRec *REC [[buffer(4)]],\n");
        VEMIT("    device packed_float3 *OUTP [[buffer(5)]], device HDAttr *OUTA [[buffer(6)]],\n");
        VEMIT("    constant HDCap &C [[buffer(7)]]) {\n");
        VEMIT("  if (tid >= C.count) return;\n");
        VEMIT("  uint g = C.first + tid; HDRec rc = REC[ITEM[g]]; uint vid = g - rc.first;\n");
        VEMIT("  device const VSPar &K = *(device const VSPar *)(KBASE + rc.koff);\n");
        VEMIT("  device const uchar *B = VBASE + rc.rawoff + vid * K.stride;\n");
        VEMIT("#define LDF(x)  (as_type<float>(*(device const uint*)(B+(x))))\n");
        VEMIT("#define LDB(x)  ((float)(B[(x)]))\n");
        VEMIT("#define LDS(x)  ((float)(*(device const short*)(B+(x))))\n");
        VEMIT("#define LDU(x)  ((float)(*(device const ushort*)(B+(x))))\n");
        if (blen >= cap - o) return 0;
        memmove(out + o, out + body_a, blen); o += blen; out[o] = 0;
        VEMIT("  float4 w4 = C.invV * float4(oPos.x * C.prm.x, oPos.y * C.prm.y, oPos.w, 1.0);\n");
        VEMIT("  OUTP[g] = w4.xyz;\n");
        VEMIT("  HDAttr a; a.col0 = oD[0];\n");
        VEMIT("  a.uv0 = oT[(K.tsel) & 3u].xy; a.uv1 = oT[(K.tsel >> 8) & 3u]; a.uv2 = oT[(K.tsel >> 16) & 3u];\n");
        VEMIT("  a.fog = oFog.x; a.pad = float(rc.mat); OUTA[g] = a;\n");
        VEMIT("  (void)dummy; (void)a0; (void)aL;\n");
        VEMIT("#undef LDF\n#undef LDB\n#undef LDS\n#undef LDU\n");
        VEMIT("}\n");
    }
#else
    (void)body_a; (void)body_b;
#endif
    #undef VEMIT
    g_cbuf = "P";
    return (int)o;
}

int ps_to_msl(const VSProg *p, int id, char *out, size_t cap){
    return ps_to_msl_ex(p, id, out, cap, 0u);
}
