/* D3D9 vertex shader (vs_1_1 / vs_2_x) bytecode interpreter.
 *
 * Why we interpret on the CPU: NFSU2 does all its draws with DrawPrimitiveUP,
 * 4 vertices per draw. At this scale doing vertex processing on the CPU is
 * both much simpler than generating MSL + compiling a separate pipeline for
 * every state, and fast enough.                                          */
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
    float pos[4];        /* oPos  - clip space */
    float col[2][4];     /* oD0, oD1 */
    float tex[8][4];     /* oT0..oT7 */
    float fog, psize;
} VSOut;

/* tok: start of the bytecode, max_dw: number of dwords safe to read.
 * NULL on failure. */
VSProg *vs_compile(const uint32_t *tok, size_t max_dw);
void    vs_free(VSProg *p);

/* from the dcl_usages: which v# register holds the given (usage, index)?  -1 if none */
int  vs_input_reg(const VSProg *p, int usage, int usage_index);

/* v: 16 input registers. 1 on success. */
int  vs_run(const VSProg *p, const VSConst *k, const float v[16][4], VSOut *o);
/* D3D9 vs_1_1 - vs_2_x color output clamping (RENKSAT=0 turns it off); major version number */
int  vs_colsat_on(void);
/* POST-PROCESS CHAIN: stage 0 sample [[color(in)]], result [[color(out)]] (metal.m) */
int  ps_to_msl_fb(const VSProg *p, int id, char *out, size_t cap, int in_idx, int out_idx);
int  vs_version_major(const VSProg *p);

/* Translate a pixel shader to MSL source (0 = failure).
 * The generated function is named "ps_<id>".  The caller provides the
 * VOut/PSPar structures and the sampler. */
int  ps_to_msl(const VSProg *p, int id, char *out, size_t cap);
int  ps_to_msl_ex(const VSProg *p, int id, char *out, size_t cap, unsigned cubemask);
int  ps_to_msl_ex2(const VSProg *p, int id, char *out, size_t cap, unsigned cubemask, int reflk);
int  ps_uses_colorout(const VSProg *p);

/* Translate a vertex shader to MSL.  decl[i] = (byte offset, D3DDECLTYPE),
 * or if offset < 0 that input is unused.  The generated function is "vs_<id>". */
int  vs_to_msl(const VSProg *p, int id, const int decl[16][2], char *out, size_t cap);

/* The highest constant register the shader touches + 1 (VSPar size). */
int  vs_kmax(const VSProg *p);
int  vs_writes_fog(const VSProg *p);
int  vs_pos_creg(const VSProg *p, int *wreg);
int  vs_fog_creg(const VSProg *p);
int  vs_light_creg(const VSProg *p, float ramp[2]);

/* diagnostics: how many instructions, which version */
unsigned vs_version(const VSProg *p);
unsigned vs_ninstr(const VSProg *p);
int      ps_plain_copy(const VSProg *p);   /* out = tex0(t0) exactly (picture.m) */
/* Untranslatable instruction counter (0 means the shader ran completely) */
unsigned vs_unknown(const VSProg *p);
#include <stdio.h>
void vs_disasm(const VSProg *p, FILE *f);
#endif
