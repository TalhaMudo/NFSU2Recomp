/* Graphics enhancements: compiled ONLY in the "HD" build.
 *
 * There are two separate binaries (see link.sh):
 *   out/nfsu2_boot     original: the game's 2004 look, a faithful port
 *   out/nfsu2_hd_boot  HD: the same lifted code + the enhancements in this file
 *
 * The lifted shards are SHARED by both builds; the difference is only in the
 * runtime.  In the original build NFS_ENHANCED=0 and every hook here folds to
 * a constant 0 at compile time -- the code path doesn't even exist -- except
 * the reflection target sizes, which the player can raise there too
 * (reflect.c, off by default).
 *
 * Principle: don't touch the game's logic.  The game keeps seeing its own
 * sizes (GetDesc says 320x240); only the real texture in Metal grows.
 * We already set the viewport to the whole target, so the scene fills the
 * large texture, and since sampling is in UV space it is size-independent.  */
#ifndef ENHANCE_H
#define ENHANCE_H
#include <stdint.h>

#ifndef NFS_ENHANCED
#define NFS_ENHANCED 0
#endif

enum { ENH_NONE = 0, ENH_ROAD = 1, ENH_MIRROR = 2, ENH_CUBE = 3 };

#if NFS_ENHANCED
/* Render target size: enlarges the (w,h) the game asked for based on the
 * CALLING address.  ret = return address of the COM call (c->eip).  Returns
 * the target's kind (ENH_*); ENH_NONE if untouched. */
int  enh_rt_size(uint32_t ret, uint32_t bb_w, uint32_t bb_h,
                 uint32_t *w, uint32_t *h);
/* Road reflection blur: 1 = the game's original width, 0 = sharp. */
float enh_blur(void);
/* View setup (overrides.c, sub_005c7ed0): id = eView id,
 * m = 48 floats (view, projection, product; row-vector layout),
 * cam = guest Camera* (may be 0). */
void enh_view_setup(uint32_t id, const float *m, uint32_t cam);
void enh_report(void);
const char *enh_build_name(void);
#else
/* Original build: the reflection targets only grow when the player asks for it
 * (NFSU2_REFLECTIONS, reflect.c), with the same kinds and blur rule. */
int   refl_rt_size(uint32_t ret, uint32_t bb_w, uint32_t bb_h, uint32_t *w, uint32_t *h);
float refl_blur(void);
static inline int  enh_rt_size(uint32_t r, uint32_t bw, uint32_t bh,
                               uint32_t *w, uint32_t *h)
{ return refl_rt_size(r, bw, bh, w, h); }
static inline float enh_blur(void) { return refl_blur(); }
static inline void enh_view_setup(uint32_t id, const float *m, uint32_t cam)
{ (void)id; (void)m; (void)cam; }
static inline void enh_report(void) {}
static inline const char *enh_build_name(void) { return "original"; }
#endif

#endif
