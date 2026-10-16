/* PERFLOG=<dizin>: gercek oyun oturumu kaydi (bkz. perflog.c) */
#ifndef PERFLOG_H
#define PERFLOG_H
#include <stdint.h>
#include <stdatomic.h>

enum { PL_VS, PL_PS, PL_PSO, PL_TEX, PL_TEXUP, PL_NEV };

extern int g_perflog;                 /* 0: kapali (kancalar tek dal) */
extern _Atomic int g_pl_thumb_req;    /* ornekleyici 2 sn'de bir kurar, ana thread alir */
extern char g_pl_dir[900];

uint64_t perflog_now_us(void);
void perflog_event(int k, uint64_t ns, uint64_t bytes);
void perflog_guest_frame(void);
void perflog_render_frame(long fno, unsigned ntex, uint64_t drawable_ns, uint64_t encode_ns, unsigned draws, unsigned verts);
void perflog_gpu(long fno, double gpu_ns);
#endif
