/* D3DX HLE Stage 2: recording the IDirect3DDevice9 calls a guest function
 * makes, and the "would it have no effect if made now" check.
 * The record is kept in com.c (the copies of the device state are there); the
 * user is runtime/d3dx_hle.c.  Details: docs/d3dx-hle.md, Stage 2.               */
#ifndef DXREC_H
#define DXREC_H
#include <stdint.h>


typedef struct {
    int16_t  mid;            /* com.c method id */
    uint16_t doff, dlen;     /* constant data within data[] */
    uint8_t  red;            /* had no effect at record time */
    uint32_t a[3];           /* the first 3 arguments after this */
} DxCall;

/* Capacity per record: a state record 8 calls / 256 bytes, a pass record
 * larger.  The call/data arrays live inside the structure carrying the
 * record; they are attached with DXREC_INIT. */
typedef struct {
    uint8_t  kind;           /* DXREC_PASS / DXREC_STATE */
    uint8_t  ok;             /* 0: not memoizable (unsupported call, nested, overflow, state block) */
    uint8_t  allred;         /* every call had no effect during recording */
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
/* A state record inside a pass is normal.  If a new record starts inside a
 * STATE record (nested application = compound) ALL the outer records become
 * invalid; a nested record of the same kind also invalidates the outer one. */
void dxrec_begin(DxRec *r, int kind);
void dxrec_append(const DxRec *src); /* append the recorded calls of a skipped state to the open records */
void dxrec_end(void);
int  dxrec_redundant(const DxRec *r);/* does every recorded call have no effect NOW */
int  dxrec_sb_recording(void);
const char *dxrec_mid_name(int mid);       /* is state block recording open (skipping forbidden) */

#endif
