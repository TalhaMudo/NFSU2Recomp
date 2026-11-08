/* libhuff: EA HUFF (huffcodex) blocks unpacked by the game's own decoder.
 *
 *   int huff_decode(const uint8_t *in, int inlen, uint8_t *out, int outcap)
 *
 * in points at a whole "HUFF" block (16-byte header, then the 0x30FB stream).
 * Returns the decoded size, or -1.  The lifted function runs on a private 32-bit
 * guest space: stack below 0x100000, input at 0x100000, output after it. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "cpu.h"

uint8_t *g_mem;
void sub_00573840_m(CPU *restrict c, uint8_t *restrict _mb);

int huff_decode(const uint8_t *in, int inlen, uint8_t *out, int outcap)
{
    if (!in || inlen < 0x14 || memcmp(in, "HUFF", 4) || in[4] != 1) return -1;
    uint32_t want = *(const uint32_t *)(in + 8);
    if ((int64_t)want > outcap) return -1;

    const uint32_t SRC = 0x100000u;
    uint32_t dst = (SRC + (uint32_t)inlen + 0x10000u + 0xfffu) & ~0xfffu;
    size_t size = (size_t)dst + want + 0x10000u;          /* slack after the output */
    uint8_t *m = calloc(1, size);
    if (!m) return -1;
    memcpy(m + SRC, in, (size_t)inlen);

    CPU c; memset(&c, 0, sizeof c);
    c.esp = 0xF0000u;
    c.esp -= 4; *(uint32_t *)(m + c.esp) = dst;           /* the one stack argument */
    c.esp -= 4; *(uint32_t *)(m + c.esp) = 0xDEAD0000u;   /* return address */
    c.eax = SRC + 0x10u;                                  /* the stream, after the header */
    g_mem = m;
    sub_00573840_m(&c, m);

    memcpy(out, m + dst, want);
    free(m);
    return (int)want;
}
