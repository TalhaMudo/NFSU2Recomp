/* ASI plugins (NFSU2 Unlimiter): load the user's own .asi next to the
 * recompiled game, the way an ASI loader does on Windows.
 *
 * On Windows a plugin is a DLL that patches the running exe from DllMain.  Here
 * both halves are lifted ahead of time:
 *   - the plugin's code: tools/mods/asi_rebase.py + genall (FNTAB_PREFIX=ul_)
 *     -> gen/ul, called through dispatch_add_module();
 *   - its patches to the game's code: recorded once from a real DllMain run
 *     (ASIREC=<file>), turned into xpatch sites (tools/mods/asi_sites.py) and
 *     lifted into the game engine as switches, off by default.
 * At start-up the user's .asi is checked against the lifted build (sha256),
 * mapped at the same base, relocated, its imports bound to the Win32 layer, its
 * TLS set up, and DllMain run with the working directory at scripts\, as the
 * ASI loader does.  Then every byte DllMain changed in the game's .text must
 * match a recorded site: matching sites switch on, anything else is reported.
 * Data patches (pool sizes, tables) need nothing: they land in guest memory,
 * which the lifted code reads.
 *
 * Without a lifted plugin (gen/ul missing) this file only answers "no plugin".
 * NFSU2_ASI=0 skips loading even when the file is there.                    */
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <CommonCrypto/CommonDigest.h>

typedef void (*guest_fn)(CPU*);
uint32_t win32_add_import(const char *dll, const char *name, int nargs, uint32_t iat);
uint32_t win32_tls_alloc(void); uint32_t win32_tls_alloc_plugin(void);
uint32_t gheap_alloc(uint32_t n, int zero);
void mem_commit(uint32_t, uint32_t);
void dispatch_add_module(uint32_t lo, uint32_t size, const uint32_t *addr, const guest_fn *ptr, uint32_t n);
void dispatch_indirect(CPU *c, uint32_t addr);
int fileio_resolve_read(const char *win, char *out, size_t outsz);
int xpatch_set(const char *name, int on);

#if NFS_ASI
#include "asi_ul.h"                     /* gen/ul: ASI_UL_FILE/SHA/BASE/PREF, asi_ul_sites[] */
extern const uint32_t g_ul_fn_count; extern const uint32_t g_ul_fn_addr[]; extern const guest_fn g_ul_fn_ptr[];
#endif

static uint32_t g_base, g_size;         /* the loaded plugin, 0 if none */
static char g_name[128];
static uint32_t g_tls_idx = 0xFFFFFFFFu, g_tls_start, g_tls_len, g_tls_zero;
static const char *g_cwd = "C:\\NFSU2";

int asi_owns(uint32_t a){ return g_size && a - g_base < g_size; }
const char *asi_current_dir(void){ return g_cwd; }

/* GetModuleHandle by name: known=1 when the name is a plugin (loaded or not) */
uint32_t asi_module_handle(const char *name, int *known){
    *known = 0;
    if (!name) { *known = g_size != 0; return g_base; }
    const char *b = strrchr(name, '\\'); b = b ? b + 1 : name;
    const char *s = strrchr(b, '/'); b = s ? s + 1 : b;
    size_t l = strlen(b);
    if (g_size && !strcasecmp(b, g_name)){ *known = 1; return g_base; }
    if (l > 4 && !strcasecmp(b + l - 4, ".asi")){ *known = 1; return 0; }
    return 0;
}
uint32_t asi_module_path(uint32_t hmod, char *out, uint32_t outsz){
    if (!g_size || hmod != g_base) return 0;
    snprintf(out, outsz, "C:\\NFSU2\\scripts\\%s", g_name);
    return 1;
}

/* stdcall argument counts of everything a plugin may import (the exe's own
 * imports keep the counts from impargs.c).  -1: unknown. */
int asi_nargs(const char *n){
    static const struct { const char *n; int a; } T[] = {
     {"GetModuleFileNameA",3},{"FindFirstFileA",2},{"VirtualProtect",4},{"EnterCriticalSection",1},
     {"FindNextFileA",2},{"LeaveCriticalSection",1},{"InitializeCriticalSection",1},{"FindClose",1},
     {"GetModuleHandleA",1},{"IsBadReadPtr",2},{"WriteConsoleW",5},{"SetEndOfFile",1},{"HeapSize",3},
     {"GetCurrentDirectoryW",2},{"CreateDirectoryW",2},{"CreateFileW",7},{"FindFirstFileW",2},
     {"FindFirstFileExW",6},{"FindNextFileW",2},{"GetFileAttributesExW",3},{"SetFileInformationByHandle",4},
     {"AreFileApisANSI",0},{"CloseHandle",1},{"GetLastError",0},{"GetModuleHandleW",1},{"GetProcAddress",2},
     {"MoveFileExW",3},{"GetFileInformationByHandleEx",4},{"MultiByteToWideChar",6},{"WideCharToMultiByte",8},
     {"LocalFree",1},{"FormatMessageA",7},{"GetLocaleInfoEx",4},{"InitializeCriticalSectionEx",3},
     {"DeleteCriticalSection",1},{"EncodePointer",1},{"DecodePointer",1},{"LCMapStringEx",9},
     {"GetStringTypeW",4},{"GetCPInfo",2},{"ReleaseSRWLockExclusive",1},{"AcquireSRWLockExclusive",1},
     {"WakeAllConditionVariable",1},{"SleepConditionVariableSRW",4},{"IsProcessorFeaturePresent",1},
     {"IsDebuggerPresent",0},{"UnhandledExceptionFilter",1},{"SetUnhandledExceptionFilter",1},
     {"GetStartupInfoW",1},{"QueryPerformanceCounter",1},{"GetCurrentProcessId",0},{"GetCurrentThreadId",0},
     {"GetSystemTimeAsFileTime",1},{"InitializeSListHead",1},{"GetCurrentProcess",0},{"TerminateProcess",2},
     {"RtlUnwind",4},{"RaiseException",4},{"InterlockedFlushSList",1},{"SetLastError",1},
     {"InitializeCriticalSectionAndSpinCount",2},{"TlsAlloc",0},{"TlsGetValue",1},{"TlsSetValue",2},
     {"TlsFree",1},{"FreeLibrary",1},{"LoadLibraryExW",3},{"ExitProcess",1},{"GetModuleHandleExW",3},
     {"GetModuleFileNameW",3},{"GetFileSizeEx",2},{"SetFilePointerEx",5},{"GetStdHandle",1},{"GetFileType",1},
     {"HeapAlloc",3},{"HeapFree",3},{"FlsAlloc",1},{"FlsGetValue",1},{"FlsSetValue",2},{"FlsFree",1},
     {"LCMapStringW",6},{"GetLocaleInfoW",4},{"IsValidLocale",2},{"GetUserDefaultLCID",0},
     {"EnumSystemLocalesW",2},{"FlushFileBuffers",1},{"WriteFile",5},{"GetConsoleOutputCP",0},
     {"GetConsoleMode",2},{"ReadFile",5},{"ReadConsoleW",5},{"HeapReAlloc",4},{"IsValidCodePage",1},
     {"GetACP",0},{"GetOEMCP",0},{"GetCommandLineA",0},{"GetCommandLineW",0},{"GetEnvironmentStringsW",0},
     {"FreeEnvironmentStringsW",1},{"GetProcessHeap",0},{"SetStdHandle",2},{"MessageBoxA",4},
     {"Sleep",1},{"GetTickCount",0},{"OutputDebugStringA",1},{"OutputDebugStringW",1},
    };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) if (!strcmp(T[i].n, n)) return T[i].a;
    return -1;
}

/* Plugin TLS (the CRT's thread-safe statics): one block per thread in the slot
 * the image's TLS index names, reached through fs:[0x2C] as on Windows. */
/* Thread start runs before the thread takes the GIL, so the blocks come from a
 * pool allocated at load time, handed out with an atomic counter. */
#define TLS_POOL 256u
static uint32_t g_tls_pool, g_tls_stride, g_tls_used;
void asi_thread_init(uint32_t teb){
    if (g_tls_idx == 0xFFFFFFFFu) return;
    uint32_t k = __sync_fetch_and_add(&g_tls_used, 1u);
    if (k >= TLS_POOL){ fprintf(stderr, "asi: out of TLS blocks (%u threads)\n", k); return; }
    uint32_t blk = g_tls_pool + k * g_tls_stride;
    memcpy(g_mem + blk, g_mem + g_tls_start, g_tls_len);
    MR32(MR32(teb + 0x2Cu) + 4u*g_tls_idx) = blk;
    if (getenv("ASITLSLOG")) fprintf(stderr, "asi: TLS teb=0x%08x arr=0x%08x slot %u = 0x%08x (k %u pool 0x%08x stride %u)\n",
                                     teb, MR32(teb + 0x2Cu), g_tls_idx, blk, k, g_tls_pool, g_tls_stride);
}

#if NFS_ASI
static int sha_ok(const uint8_t *buf, size_t n){
    uint8_t d[CC_SHA256_DIGEST_LENGTH]; char hex[65];
    CC_SHA256(buf, (CC_LONG)n, d);
    for (int i = 0; i < 32; i++) sprintf(hex + 2*i, "%02x", d[i]);
    return !strcmp(hex, ASI_UL_SHA);
}

/* the exe's .text range, from the headers mem_load_pe copied to 0x400000 */
static void exe_text(uint32_t *lo, uint32_t *hi){
    uint32_t nt = 0x400000u + MR32(0x40003Cu);
    uint32_t nsec = MR16(nt + 6), sh = nt + 24 + MR16(nt + 20);
    *lo = *hi = 0;
    for (uint32_t i = 0; i < nsec; i++, sh += 40)
        if (!memcmp(g_mem + sh, ".text", 6)){ *lo = 0x400000u + MR32(sh + 12); *hi = *lo + MR32(sh + 8); }
}
static uint8_t *g_text0; static uint32_t g_tlo, g_thi;

/* Map the plugin; 1 when it is loaded.  Called after win32_init and the main
 * thread's TEB are set up, before any guest code runs. */
int asi_load(uint32_t main_teb){
    /* ASIPAD=<MB> (diagnostic): take that much guest heap first, as a plugin would */
    { const char *pd = getenv("ASIPAD"); if (pd) gheap_alloc((uint32_t)atoi(pd) << 20, 0); }
    { const char *pt = getenv("ASITLS"); if (pt) for (int k = atoi(pt); k > 0; k--) win32_tls_alloc(); }
    const char *e = getenv("NFSU2_ASI");
    if (e && *e == '0') return 0;
    /* list scripts\*.asi: the lifted one is loaded, others are named as ignored */
    char dir[1024], path[1200];
    DIR *d = fileio_resolve_read("C:\\NFSU2\\scripts", dir, sizeof dir) ? opendir(dir) : 0;
    struct dirent *de; path[0] = 0;
    while (d && (de = readdir(d))){
        size_t l = strlen(de->d_name);
        if (l < 5 || strcasecmp(de->d_name + l - 4, ".asi")) continue;
        if (!strcasecmp(de->d_name, ASI_UL_FILE)){ snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
                                                   snprintf(g_name, sizeof g_name, "%s", de->d_name); }
        else fprintf(stderr, "asi: %s ignored (no recompiled code for it)\n", de->d_name);
    }
    if (d) closedir(d);
    /* The plugin itself is looked up as a file, through every layer: with mods on
     * (NFSU2_MODS) it comes from the launcher's mod files even when the game folder
     * has a scripts\ folder of its own, which is the one listed above. */
    if (!path[0]){
        char win[300]; struct stat st;
        snprintf(win, sizeof win, "C:\\NFSU2\\scripts\\%s", ASI_UL_FILE);
        if (fileio_resolve_read(win, path, sizeof path) && stat(path, &st) == 0 && S_ISREG(st.st_mode))
            snprintf(g_name, sizeof g_name, "%s", ASI_UL_FILE);
        else path[0] = 0;
    }
    if (!path[0]) return 0;
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n){ fclose(f); free(buf); return 0; }
    fclose(f);
    if (!sha_ok(buf, (size_t)n)){
        fprintf(stderr, "asi: %s is a different build from the recompiled one (%s); not loaded\n",
                g_name, ASI_UL_VERSION);
        free(buf); return 0; }

    /* --- map --- */
    uint32_t nt = *(uint32_t*)(buf + 0x3C);
    uint32_t pref = *(uint32_t*)(buf + nt + 52), isz = *(uint32_t*)(buf + nt + 80), hsz = *(uint32_t*)(buf + nt + 84);
    uint16_t nsec = *(uint16_t*)(buf + nt + 6), osz = *(uint16_t*)(buf + nt + 20);
    uint32_t ddg = 0;                                  /* data directory, in the mapped image */
    g_base = ASI_UL_BASE; g_size = (isz + 0xFFFu) & ~0xFFFu;
    mem_commit(g_base, g_size);
    memcpy(g_mem + g_base, buf, hsz);
    uint8_t *sh = buf + nt + 24 + osz;
    for (int i = 0; i < nsec; i++, sh += 40){
        uint32_t vs = *(uint32_t*)(sh + 8), va = *(uint32_t*)(sh + 12), rs = *(uint32_t*)(sh + 16), ro = *(uint32_t*)(sh + 20);
        memcpy(g_mem + g_base + va, buf + ro, rs < vs ? rs : vs);
    }
    free(buf);
    ddg = g_base + nt + 24 + 96;
    uint8_t *dd = g_mem + ddg;
    /* --- relocate (HIGHLOW only; asi_rebase.py refuses anything else) --- */
    uint32_t delta = g_base - pref, rr = *(uint32_t*)(dd + 5*8), rn = *(uint32_t*)(dd + 5*8 + 4), nrel = 0;
    for (uint32_t p = g_base + rr; p < g_base + rr + rn; ){
        uint32_t page = MR32(p), bsz = MR32(p + 4);
        if (bsz < 8) break;
        for (uint32_t k = 8; k + 2 <= bsz; k += 2){
            uint16_t w = MR16(p + k);
            if ((w >> 12) == 3){ MR32(g_base + page + (w & 0xFFFu)) += delta; nrel++; }
        }
        p += bsz;
    }
    /* --- imports --- */
    uint32_t ir = getenv("ASI_NOIMP") ? 0 : *(uint32_t*)(dd + 1*8), nimp = 0, bad = 0;
    for (uint32_t de2 = g_base + ir; ir && MR32(de2 + 12); de2 += 20){
        const char *dll = (const char*)(g_mem + g_base + MR32(de2 + 12));
        uint32_t ilt = MR32(de2) ? MR32(de2) : MR32(de2 + 16), iat = MR32(de2 + 16);
        for (uint32_t k = 0; MR32(g_base + ilt + 4*k); k++){
            uint32_t v = MR32(g_base + ilt + 4*k);
            if (v & 0x80000000u){ fprintf(stderr, "asi: import by ordinal %s #%u\n", dll, v & 0xFFFFu); bad++; continue; }
            const char *nm = (const char*)(g_mem + g_base + v + 2);
            int na = asi_nargs(nm);
            if (na < 0){ fprintf(stderr, "asi: no argument count for %s!%s\n", dll, nm); bad++; na = 0; }
            win32_add_import(dll, nm, na, g_base + iat + 4*k); nimp++;
        }
    }
    /* --- TLS --- */
    uint32_t tr = getenv("ASI_NOTLS") ? 0 : *(uint32_t*)(dd + 9*8);
    if (tr){
        uint32_t t = g_base + tr;
        g_tls_start = MR32(t); g_tls_len = MR32(t + 4) - MR32(t); g_tls_zero = MR32(t + 16);   /* +12 is the callback list */
        g_tls_idx = win32_tls_alloc_plugin();
        MR32(MR32(t + 8)) = g_tls_idx;                 /* AddressOfIndex */
        g_tls_stride = (g_tls_len + g_tls_zero + 15u) & ~15u; if (!g_tls_stride) g_tls_stride = 16u;
        g_tls_pool = gheap_alloc(TLS_POOL * g_tls_stride, 1);
        asi_thread_init(main_teb);
    }
    dispatch_add_module(g_base, g_size, g_ul_fn_addr, g_ul_fn_ptr, g_ul_fn_count);
    exe_text(&g_tlo, &g_thi);
    g_text0 = malloc(g_thi - g_tlo);
    memcpy(g_text0, g_mem + g_tlo, g_thi - g_tlo);
    fprintf(stderr, "asi: %s loaded at 0x%08x (%u relocations, %u imports%s, TLS slot %d)\n",
            g_name, g_base, nrel, nimp, bad ? ", SOME UNBOUND" : "", (int)g_tls_idx);
    return 1;
}

/* ASIXPOFF=lo-hi[,lo-hi...] (diagnostic): leave the switches in these ranges off */
static int xp_off(uint32_t a){
    const char *e = getenv("ASIXPOFF");
    while (e && *e){ uint32_t lo, hi; if (sscanf(e, "%x-%x", &lo, &hi) == 2 && a >= lo && a < hi) return 1;
                     e = strchr(e, ','); if (e) e++; }
    return 0;
}
/* Run DllMain(PROCESS_ATTACH) on the guest main thread (under the GIL), then
 * account for every byte it changed in the game's code. */
void asi_attach(CPU *c){
    if (!g_size) return;
    uint32_t nt = g_base + MR32(g_base + 0x3C), entry = g_base + MR32(nt + 40);
    uint32_t esp0 = c->esp;
    /* ASIDATA=1 (diagnostic): list what DllMain changed in the exe's data;
     * ASIUNDO=lo-hi: put back the data changes inside [lo,hi) (bisection) */
    uint32_t ilo = g_thi, ihi = 0x400000u + MR32(0x400000u + MR32(0x40003Cu) + 80);
    uint8_t *img0 = 0;
    if (getenv("ASIDATA") || getenv("ASIUNDO")){ img0 = malloc(ihi - ilo); memcpy(img0, g_mem + ilo, ihi - ilo); }
    g_cwd = "C:\\NFSU2\\scripts";               /* the ASI loader's working directory */
    if (getenv("ASINOMAIN")) { g_cwd = "C:\\NFSU2"; fprintf(stderr, "asi: DllMain skipped (ASINOMAIN)\n"); return; }
    PUSH32(0u); PUSH32(1u); PUSH32(g_base); PUSH32(0xDEADA51Au);
    dispatch_indirect(c, entry);
    g_cwd = "C:\\NFSU2";
    fprintf(stderr, "asi: after DllMain fcw=%04x mxcsr=%08x top=%d df=%d\n",
            (unsigned)c->fcw, (unsigned)c->mxcsr, (int)c->top, (int)c->df);
    fprintf(stderr, "asi: DllMain returned %u%s\n", c->eax,
            c->esp == esp0 ? "" : "  (STACK NOT BALANCED)");
    c->esp = esp0;
    if (img0){
        uint32_t ulo = 0, uhi = 0; const char *u = getenv("ASIUNDO");
        if (u) sscanf(u, "%x-%x", &ulo, &uhi);
        uint32_t nd = 0, nu = 0;
        for (uint32_t i = 0; i < ihi - ilo; ){
            if (g_mem[ilo + i] == img0[i]){ i++; continue; }
            uint32_t j = i; while (j < ihi - ilo && g_mem[ilo + j] != img0[j]) j++;
            uint32_t a = ilo + i; nd++;
            if (getenv("ASIDATA")){
                fprintf(stderr, "asi: data 0x%08x %u:", a, j - i);
                for (uint32_t k = i; k < j && k < i + 16; k++) fprintf(stderr, " %02x>%02x", img0[k], g_mem[ilo + k]);
                fprintf(stderr, "\n"); }
            if (u && a >= ulo && a < uhi){ memcpy(g_mem + a, img0 + i, j - i); nu++; }
            i = j;
        }
        fprintf(stderr, "asi: %u data ranges changed, %u put back\n", nd, nu);
        free(img0);
    }

    /* the changed ranges of .text */
    const char *rec = getenv("ASIREC");
    FILE *rf = rec ? fopen(rec, "w") : 0;
    uint32_t n = g_thi - g_tlo, nr = 0, on = 0, data = 0, miss = 0;
    for (uint32_t i = 0; i < n; ){
        if (g_mem[g_tlo + i] == g_text0[i]){ i++; continue; }
        /* One range = changed bytes less than 8 apart: a 5-byte jmp whose second
         * byte happens to equal the old one is still one patch. */
        uint32_t j = i;
        for (;;){
            while (j < n && g_mem[g_tlo + j] != g_text0[j]) j++;
            uint32_t k = j; while (k < n && k < j + 8 && g_mem[g_tlo + k] == g_text0[k]) k++;
            if (k < n && k < j + 8) { j = k; continue; }
            break;
        }
        uint32_t a = g_tlo + i, len = j - i; nr++;
        if (rf){ fprintf(rf, "%08x %u ", a, len);
                 for (uint32_t k = 0; k < len; k++) fprintf(rf, "%02x", g_mem[a + k]);
                 fprintf(rf, "\n"); }
        int found = 0;
        for (int s = 0; s < ASI_UL_NSITES; s++){
            if (asi_ul_sites[s].addr != a || asi_ul_sites[s].len != len) continue;
            found = 1;
            if (memcmp(g_mem + a, asi_ul_sites[s].bytes, len)){ found = 2; break; }
            if (!asi_ul_sites[s].xp) data++;                         /* a table inside .text */
            else if (xp_off(a)) on++;                                  /* ASIXPOFF: left off */
            else if (xpatch_set(asi_ul_sites[s].xp, 1)) on++;
            else found = 3;
            break;
        }
        if (found != 1){
            miss++;
            if (miss <= 20) fprintf(stderr, "asi: code patch at 0x%08x (%u bytes) %s\n", a, len,
                                    found == 0 ? "was not recorded" : found == 2 ? "has other bytes than recorded"
                                                                    : "has no switch in this engine");
        }
        i = j;
    }
    if (rf){ fclose(rf); fprintf(stderr, "asi: %u changed ranges written to %s\n", nr, rec); }
    /* ASIUNDOTEXT=lo-hi (diagnostic): put the original .text bytes back in [lo,hi)
     * (the switches stay as they are) */
    { const char *u = getenv("ASIUNDOTEXT"); uint32_t lo = 0, hi = 0;
      if (u && sscanf(u, "%x-%x", &lo, &hi) == 2)
          for (uint32_t a = lo; a < hi; a++) if (a - g_tlo < n) g_mem[a] = g_text0[a - g_tlo]; }
    fprintf(stderr, "asi: %u code patches: %u switched on, %u data in .text, %u NOT HONOURED\n",
            nr, on, data, miss);
}
#else
int asi_load(uint32_t main_teb){ (void)main_teb; return 0; }
void asi_attach(CPU *c){ (void)c; }
#endif
