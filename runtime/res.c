/* PE kaynak dizini (.rsrc) - FindResource / LoadResource / LockResource */
#include "cpu.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

extern uint32_t g_image_base, g_res_dir_va, g_res_dir_size;

/* IMAGE_RESOURCE_DIRECTORY: +12 NumberOfNamedEntries, +14 NumberOfIdEntries, girisler +16'dan */
/* IMAGE_RESOURCE_DIRECTORY_ENTRY: +0 Name/Id, +4 OffsetToData (bit31 = alt dizin) */
/* IMAGE_RESOURCE_DATA_ENTRY:      +0 OffsetToData(RVA), +4 Size, +8 CodePage        */

static int name_eq(uint32_t name_rva, const char *want){
    uint32_t p = g_res_dir_va + (name_rva & 0x7FFFFFFFu);
    uint32_t len = MR16(p);
    for (uint32_t i=0;i<len;i++){
        uint32_t ch = MR16(p + 2u + 2u*i);
        unsigned char w = (unsigned char)want[i];
        if (!w) return 0;
        if (toupper((int)ch) != toupper((int)w)) return 0;
    }
    return want[len] == 0;
}

/* dir icinde ara; want_str NULL ise want_id kullanilir. Bulunursa giris ofsetini dondurur. */
static uint32_t dir_find(uint32_t dir, const char *want_str, uint32_t want_id, uint32_t *is_dir){
    uint32_t nnamed = MR16(dir + 12), nid = MR16(dir + 14);
    uint32_t base = dir + 16;
    if (want_str){
        for (uint32_t i=0;i<nnamed;i++){
            uint32_t e = base + 8u*i;
            uint32_t nm = MR32(e);
            if (!(nm & 0x80000000u)) continue;
            if (name_eq(nm, want_str)){
                uint32_t off = MR32(e+4);
                *is_dir = (off & 0x80000000u) ? 1u : 0u;
                return g_res_dir_va + (off & 0x7FFFFFFFu);
            }
        }
        return 0;
    }
    for (uint32_t i=0;i<nid;i++){
        uint32_t e = base + 8u*(nnamed + i);
        if (MR32(e) != want_id) continue;
        uint32_t off = MR32(e+4);
        *is_dir = (off & 0x80000000u) ? 1u : 0u;
        return g_res_dir_va + (off & 0x7FFFFFFFu);
    }
    return 0;
}

/* ilk cocugu al (dil seviyesi icin) */
static uint32_t dir_first(uint32_t dir, uint32_t *is_dir){
    uint32_t n = (uint32_t)MR16(dir+12) + (uint32_t)MR16(dir+14);
    if (!n) return 0;
    uint32_t off = MR32(dir + 16 + 4);
    *is_dir = (off & 0x80000000u) ? 1u : 0u;
    return g_res_dir_va + (off & 0x7FFFFFFFu);
}

uint64_t g_res_found = 0, g_res_missed = 0;

/* lpName/lpType: yuksek word 0 ise tamsayi kimlik, degilse guest string isaretcisi */
uint32_t res_find(uint32_t lpName, uint32_t lpType, int wide){
    if (!g_res_dir_va) return 0;
    char nbuf[128], tbuf[128];
    const char *ns = 0, *ts = 0; uint32_t nid = 0, tid = 0;

    if (lpType > 0xFFFFu){
        if (wide){ uint32_t k=0; while(k<127 && MR16(lpType+2u*k)){ tbuf[k]=(char)MR16(lpType+2u*k); k++; } tbuf[k]=0; }
        else     { uint32_t k=0; while(k<127 && MR8(lpType+k)){ tbuf[k]=(char)MR8(lpType+k); k++; } tbuf[k]=0; }
        ts = tbuf;
    } else tid = lpType;

    if (lpName > 0xFFFFu){
        if (wide){ uint32_t k=0; while(k<127 && MR16(lpName+2u*k)){ nbuf[k]=(char)MR16(lpName+2u*k); k++; } nbuf[k]=0; }
        else     { uint32_t k=0; while(k<127 && MR8(lpName+k)){ nbuf[k]=(char)MR8(lpName+k); k++; } nbuf[k]=0; }
        ns = nbuf;
    } else nid = lpName;

    uint32_t isd = 0;
    uint32_t td = dir_find(g_res_dir_va, ts, tid, &isd);
    if (!td || !isd){ g_res_missed++; return 0; }
    uint32_t nd = dir_find(td, ns, nid, &isd);
    if (!nd){ g_res_missed++; return 0; }
    uint32_t leaf = nd;
    if (isd){ leaf = dir_first(nd, &isd); if (!leaf || isd){ g_res_missed++; return 0; } }
    g_res_found++;
    { static int shown=0; if(getenv("VERBOSE") && shown<8){ shown++;
        uint32_t da = g_image_base + MR32(leaf), sz = MR32(leaf+4);
        fprintf(stderr,"  [kaynak] tip=%s%u ad=%s -> VA 0x%08x boyut %u  ilk: %02x %02x %02x %02x (imza %s)\n",
                ts?"":"#", ts?0u:tid, ns?ns:"(id)", da, sz,
                MR8(da),MR8(da+1),MR8(da+2),MR8(da+3),
                MR32(da)==0xFEFF0901u?"EFEKT OK":"?" ); } }
    return leaf;                      /* IMAGE_RESOURCE_DATA_ENTRY adresi = HRSRC */
}
uint32_t res_data(uint32_t hrsrc){ return hrsrc ? g_image_base + MR32(hrsrc) : 0; }
uint32_t res_size(uint32_t hrsrc){ return hrsrc ? MR32(hrsrc + 4) : 0; }

void res_report(void){
    fprintf(stderr,"  kaynak: %llu bulundu, %llu bulunamadi\n",
            (unsigned long long)g_res_found,(unsigned long long)g_res_missed);
}
