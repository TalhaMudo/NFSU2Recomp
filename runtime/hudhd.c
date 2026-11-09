/* SHARPER HUD AND MENUS (launcher Settings › More; NFSU2_HUDHD)
 *
 * The game draws its HUD, menus, fonts and logos from small textures (most
 * 128-512 pixels) that look soft on today's screens.  hudhd.pak (tools/hudhd_pack.py,
 * built from LunicAura106's NFSU2 Upscaled HUD) holds redrawn versions at up to four
 * times the size, keyed by the game's own texture name hash.
 *
 * How they get in: the game's texture loader (sub_005ce180) calls CreateTexture
 * with esi pointing at the texture's info, which carries its name (+0x0C) and the
 * name's bStringHash (+0x24).  When the hash is in the pack, the Metal texture is
 * made at the pack's size and filled from it once; the game's own (small) uploads to
 * it are then dropped (metal_tex_fill_fixed).  The game itself keeps seeing the
 * original size everywhere: its D3D texture object, its memory, the font glyph
 * tables and the cursor are as before, and since it samples with 0..1 coordinates
 * the larger picture lands exactly where the small one was.  So nothing per frame:
 * the cost is a few ms of unpacking at load time and the larger textures' memory.
 *
 * On a PC the mod is installed into the game's bundles, and a plugin
 * (NFSU2HDContentSupport.asi) then has to tell the game the old sizes where it
 * measures textures in texels; here neither is needed.
 *
 * NFSU2_HUDHD=1: the pack from the app's Resources (or next to the engine, out/);
 *   =<path>: that pack; unset / 0: off, the original game.  HUDHDLOG=1: every texture
 *   replaced.  Both engines, both executables (GA addresses). */
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

const char *app_resource_dir(void);
const char *app_exe_dir(void);
unsigned metal_create_texture_ex(unsigned w, unsigned h, unsigned levels, unsigned d3dfmt, unsigned is_rt);
void metal_tex_fill_fixed(unsigned id_, unsigned levels, unsigned w, unsigned h,
                          unsigned d3dfmt, const void *data, size_t size);

/* The two CreateTexture calls of the texture loader (return addresses). */
#define RET_TPK_SCALED  GA(0x005ce2eau)    /* with a texture detail drop */
#define RET_TPK         GA(0x005ce30eu)

typedef struct {
    uint32_t hash, fmt;
    uint16_t w, h, levels, pad;
    uint32_t off, csize, size;
} PakEntry;                                  /* 28 bytes, as written by hudhd_pack.py */

static const uint8_t  *g_pak;
static size_t          g_pak_size;
static const PakEntry *g_ent;
static uint32_t        g_nent;
static int             g_state = -1;         /* -1 not read, 0 off, 1 on */
static unsigned        g_replaced, g_failed;

static int pak_open(const char *path){
    int fd = open(path, O_RDONLY);
    if (fd < 0){ fprintf(stderr, "hudhd: no %s, the HUD stays as in the game\n", path); return 0; }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 16){ close(fd); return 0; }
    void *m = mmap(0, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) return 0;
    const uint8_t *p = m;
    uint32_t ver, n;
    memcpy(&ver, p + 4, 4); memcpy(&n, p + 8, 4);
    if (memcmp(p, "NHHD", 4) || ver != 1 || 16 + (uint64_t)n * sizeof(PakEntry) > (uint64_t)st.st_size){
        fprintf(stderr, "hudhd: %s is not a pack of this version\n", path);
        munmap(m, (size_t)st.st_size); return 0;
    }
    g_pak = p; g_pak_size = (size_t)st.st_size;
    g_ent = (const PakEntry *)(p + 16); g_nent = n;
    fprintf(stderr, "hudhd: %u sharper HUD and menu textures (%s)\n", n, path);
    return 1;
}

static int hudhd_on(void){
    if (g_state >= 0) return g_state;
    g_state = 0;
    const char *e = getenv("NFSU2_HUDHD");
    if (!e || !*e || !strcmp(e, "0")) return 0;
    char path[1200];
    if (strchr(e, '/')) snprintf(path, sizeof path, "%s", e);
    else {
        const char *d = app_resource_dir();
        if (!d) d = app_exe_dir();
        snprintf(path, sizeof path, "%s/hudhd.pak", d ? d : ".");
    }
    g_state = pak_open(path);
    return g_state;
}

static const PakEntry *pak_find(uint32_t hash){
    uint32_t lo = 0, hi = g_nent;
    while (lo < hi){
        uint32_t mid = (lo + hi) / 2;
        if (g_ent[mid].hash < hash) lo = mid + 1;
        else if (g_ent[mid].hash > hash) hi = mid;
        else return &g_ent[mid];
    }
    return 0;
}

/* From com.c CreateTexture, before the texture is made.  info: the game's texture
 * info (esi at the loader's call).  Returns the Metal texture id when one was made
 * from the pack, 0 to go on as usual. */
unsigned hudhd_create(uint32_t ret_addr, uint32_t info){
    if (ret_addr != RET_TPK && ret_addr != RET_TPK_SCALED) return 0;
    if (!hudhd_on() || info < 0x10000u || !mem_is_committed(info) || !mem_is_committed(info + 0x50)) return 0;
    const PakEntry *pe = pak_find(MR32(info + 0x24));
    if (!pe || (uint64_t)pe->off + pe->csize > g_pak_size) return 0;
    uint8_t *buf = malloc(pe->size);
    if (!buf) return 0;
    uLongf got = pe->size;
    unsigned id = 0;
    if (uncompress(buf, &got, g_pak + pe->off, pe->csize) == Z_OK && got == pe->size){
        id = metal_create_texture_ex(pe->w, pe->h, pe->levels, pe->fmt, 0);
        if (id) metal_tex_fill_fixed(id, pe->levels, pe->w, pe->h, pe->fmt, buf, pe->size);
    }
    free(buf);
    if (!id){ g_failed++; return 0; }
    g_replaced++;
    if (getenv("HUDHDLOG"))
        fprintf(stderr, "  [hudhd] %.24s %ux%u -> %ux%u (id %u)\n", (const char *)(g_mem + info + 0x0C),
                (unsigned)MR16(info + 0x44), (unsigned)MR16(info + 0x46), pe->w, pe->h, id);
    return id;
}

/* For the end-of-run report. */
void hudhd_report(void){
    if (g_state == 1) fprintf(stderr, "hudhd: %u textures made sharper, %u failed\n", g_replaced, g_failed);
}
