/* Win32 functions the exe never imports but an ASI plugin does, and the few the
 * exe imports that used to fall through to the stub.
 *
 * A modern plugin (Unlimiter: Visual Studio 2022, static CRT) brings the CRT's
 * Vista-era needs: fiber-local storage, SRW locks, condition variables, the
 * "Ex" locale calls, and the wide (W) file API that std::filesystem and
 * std::ifstream sit on.  The W file calls convert the path and hand over to the
 * A versions in fileio.c, so they see the same game folder, the same
 * case-insensitive lookup and the same save layer.
 *
 * Called from win32_call() after every other layer, right before the stub; a
 * name handled elsewhere never reaches this file.                          */
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define AW(c,k) MR32((c)->esp + 4u*(uint32_t)(k))
#define SETERR(c,e) do { if ((c)->fs_base) MR32((c)->fs_base + 0x34u) = (e); } while (0)
#define ERROR_FILE_NOT_FOUND 2u
#define ERROR_NO_MORE_FILES 18u

int fileio_call(CPU*, const char*, uint32_t*);
int win32_ext_call(CPU*, const char*, uint32_t*);
uint32_t gheap_alloc(uint32_t n, int zero);
int fileio_resolve_read(const char *win, char *out, size_t outsz);
uint32_t asi_module_handle(const char *name, int *known);
uint32_t asi_module_path(uint32_t hmod, char *out, uint32_t outsz);
const char *asi_current_dir(void);
uint32_t win32_tls_alloc_plugin(void);
uint32_t win32_tls_get(CPU *c, uint32_t i);
void win32_tls_set(CPU *c, uint32_t i, uint32_t v);

/* ---- helpers ---------------------------------------------------------- */

/* Guest scratch for converted strings and find data.  Guest code runs one
 * thread at a time and these are used only for the length of one call. */
static uint32_t scratch(uint32_t k){
    static uint32_t base = 0;
    if (!base) base = gheap_alloc(4u * 0x1000u, 1);
    return base + (k & 3u) * 0x1000u;
}

/* UTF-16 guest string -> host UTF-8 (only ASCII and Latin-1 appear in paths). */
static void w2a(uint32_t p, char *out, size_t n){
    size_t k = 0;
    if (p) for (; k + 1 < n; p += 2){
        uint32_t ch = MR16(p); if (!ch) break;
        if (ch < 0x80) out[k++] = (char)ch;
        else if (ch < 0x800 && k + 2 < n){ out[k++] = (char)(0xC0 | (ch >> 6)); out[k++] = (char)(0x80 | (ch & 0x3F)); }
        else out[k++] = '?';
    }
    out[k] = 0;
}
/* host string -> UTF-16 guest buffer of cap characters; returns the length */
static uint32_t a2w(const char *s, uint32_t p, uint32_t cap){
    uint32_t l = (uint32_t)strlen(s);
    if (!p || !cap) return l;
    uint32_t k = 0;
    for (; k < l && k + 1 < cap; k++) MR16(p + 2u*k) = (uint8_t)s[k];
    MR16(p + 2u*k) = 0;
    return k;
}
static uint32_t put_guest_str(const char *s, uint32_t slot){
    uint32_t p = scratch(slot);
    snprintf((char*)(g_mem + p), 0x1000, "%s", s);
    return p;
}

/* Run another layer's handler with some of the stack arguments replaced. */
static int call_with(CPU *c, const char *name, const uint32_t *args, int n, uint32_t *r){
    uint32_t save[8];
    for (int k = 0; k < n; k++){ save[k] = AW(c,k); AW(c,k) = args[k]; }
    int h = fileio_call(c, name, r) || win32_ext_call(c, name, r);
    for (int k = 0; k < n; k++) AW(c,k) = save[k];
    return h;
}

/* WIN32_FIND_DATAA (320 bytes, name at +44, alternate at +304) -> W (592 bytes) */
static void find_a2w(uint32_t fa, uint32_t fw){
    if (!fw) return;
    memset(g_mem + fw, 0, 592);
    memcpy(g_mem + fw, g_mem + fa, 44);
    a2w((const char*)(g_mem + fa + 44), fw + 44, 260);
}

/* ---- the calls ---------------------------------------------------------- */

int win32_asi_call(CPU *c, const char *n, uint32_t *out){
    uint32_t r = 0;
    char path[1024];

    /* -- pointers, console, startup -- */
    if      (!strcmp(n,"EncodePointer") || !strcmp(n,"DecodePointer")) r = AW(c,0);
    else if (!strcmp(n,"AreFileApisANSI"))      r = 1;
    else if (!strcmp(n,"GetConsoleMode"))       r = 0;
    else if (!strcmp(n,"GetConsoleOutputCP"))   r = 437;
    else if (!strcmp(n,"ReadConsoleW") || !strcmp(n,"WriteConsoleW")) r = 0;
    else if (!strcmp(n,"SetStdHandle"))         r = 1;
    else if (!strcmp(n,"FormatMessageA"))       r = 0;
    else if (!strcmp(n,"GetStartupInfoW")){
        uint32_t p = AW(c,0); if (p){ memset(g_mem + p, 0, 68); MR32(p) = 68; } r = 0; }
    else if (!strcmp(n,"GetCommandLineW")){
        static uint32_t cl = 0;
        if (!cl){ cl = gheap_alloc(64, 1); a2w("SPEED2.EXE", cl, 32); }
        r = cl; }
    else if (!strcmp(n,"InitializeSListHead")){ uint32_t p = AW(c,0); if (p){ MR32(p) = 0; MR32(p+4) = 0; } r = 0; }
    else if (!strcmp(n,"InterlockedFlushSList")){
        uint32_t p = AW(c,0); r = p ? MR32(p) : 0; if (p){ MR32(p) = 0; MR16(p+4) = 0; } }

    /* -- locale -- */
    else if (!strcmp(n,"GetUserDefaultLCID"))   r = 0x0409;          /* en-US */
    else if (!strcmp(n,"IsValidLocale"))        r = 1;
    else if (!strcmp(n,"EnumSystemLocalesW"))   r = 1;
    else if (!strcmp(n,"GetLocaleInfoEx"))      r = 0;               /* the CRT keeps the "C" locale */
    else if (!strcmp(n,"LCMapStringEx")){
        /* (locale, flags, src, srclen, dst, dstlen, ...) : case mapping only */
        uint32_t fl = AW(c,1), src = AW(c,2), dst = AW(c,4); int sl = (int)AW(c,3), dl = (int)AW(c,5);
        if (sl < 0){ sl = 0; while (MR16(src + 2u*(uint32_t)sl)) sl++; sl++; }
        if (!dl) r = (uint32_t)sl;
        else { int k = 0;
            for (; k < sl && k < dl; k++){
                uint32_t ch = MR16(src + 2u*(uint32_t)k);
                if ((fl & 0x100u) && ch >= 'A' && ch <= 'Z') ch += 32;       /* LCMAP_LOWERCASE */
                if ((fl & 0x200u) && ch >= 'a' && ch <= 'z') ch -= 32;       /* LCMAP_UPPERCASE */
                MR16(dst + 2u*(uint32_t)k) = (uint16_t)ch; }
            r = (uint32_t)k; } }

    /* -- fiber-local storage = thread-local storage (no fibers here) -- */
    else if (!strcmp(n,"FlsAlloc"))    r = win32_tls_alloc_plugin();   /* only plugins import it */
    else if (!strcmp(n,"FlsFree"))     r = 1;
    else if (!strcmp(n,"FlsGetValue")) r = win32_tls_get(c, AW(c,0));
    else if (!strcmp(n,"FlsSetValue")){ win32_tls_set(c, AW(c,0), AW(c,1)); r = 1; }

    /* -- SRW locks and condition variables.  The GIL is handed over every 1024
     *    Win32 calls, so an exclusive SRW lock must really exclude: it is the
     *    critical-section lock of win32_ext.c, keyed by the lock's address. -- */
    else if (!strcmp(n,"InitializeCriticalSectionEx")){
        uint32_t a[1] = { AW(c,0) }; call_with(c, "InitializeCriticalSection", a, 1, &r); r = 1; }
    else if (!strcmp(n,"AcquireSRWLockExclusive")){
        uint32_t a[1] = { AW(c,0) }; call_with(c, "EnterCriticalSection", a, 1, &r); r = 0; }
    else if (!strcmp(n,"ReleaseSRWLockExclusive")){
        uint32_t a[1] = { AW(c,0) }; call_with(c, "LeaveCriticalSection", a, 1, &r); r = 0; }
    else if (!strcmp(n,"SleepConditionVariableSRW")){
        /* (cv, srw, ms, flags): only the CRT's thread-safe statics use it, and only
         * when two threads construct the same static at once.  Release, let the
         * other thread run for a moment, take the lock again; the CRT loops. */
        uint32_t a[1] = { AW(c,1) }, t;
        call_with(c, "LeaveCriticalSection", a, 1, &t);
        { uint32_t ms[1] = { 1 }; call_with(c, "Sleep", ms, 1, &t); }   /* releases the GIL */
        call_with(c, "EnterCriticalSection", a, 1, &t);
        r = 1; }
    else if (!strcmp(n,"WakeAllConditionVariable")) r = 0;

    /* -- modules -- */
    else if (!strcmp(n,"GetModuleHandleW")){
        uint32_t p = AW(c,0); int known = 0;
        if (!p) r = 0x00400000u;
        else { w2a(p, path, sizeof path); r = asi_module_handle(path, &known);
               if (!known){
                   /* the CRT asks for kernel32 (fine) and mscoree.dll (the .NET
                    * shim: must be absent, or it tries CorExitProcess) */
                   const char *b = strrchr(path, '\\'); b = b ? b + 1 : path;
                   r = !strncasecmp(b, "kernel32", 8) || !strncasecmp(b, "ntdll", 5) ? 0x7C800000u : 0u; } } }
    else if (!strcmp(n,"GetModuleHandleExW")){
        /* (flags, name or address, *module) */
        uint32_t fl = AW(c,0), p = AW(c,1), ph = AW(c,2); int known = 0;
        if (fl & 4u){                                   /* FROM_ADDRESS */
            extern int asi_owns(uint32_t); r = asi_owns(p) ? asi_module_handle(0, &known) : 0x00400000u; }
        else if (!p) r = 0x00400000u;
        else { w2a(p, path, sizeof path); r = asi_module_handle(path, &known); }
        if (ph) MR32(ph) = r;
        r = r != 0; }
    else if (!strcmp(n,"GetModuleFileNameW")){
        uint32_t hm = AW(c,0), p = AW(c,1), sz = AW(c,2);
        char mp[300];
        if (!asi_module_path(hm, mp, sizeof mp)) snprintf(mp, sizeof mp, "C:\\NFSU2\\SPEED2.EXE");
        r = a2w(mp, p, sz); }
    else if (!strcmp(n,"LoadLibraryExW")) r = 0;   /* api-ms-win-* probes: the CRT falls back to kernel32 */

    /* -- files: convert the path, hand over to the A version -- */
    else if (!strcmp(n,"GetCurrentDirectoryW")){
        uint32_t cap = AW(c,0), p = AW(c,1);
        const char *d = asi_current_dir();
        uint32_t l = (uint32_t)strlen(d);
        if (!p || cap <= l) r = l + 1;
        else r = a2w(d, p, cap); }
    else if (!strcmp(n,"CreateFileW") || !strcmp(n,"CreateDirectoryW")){
        w2a(AW(c,0), path, sizeof path);
        uint32_t a[1] = { put_guest_str(path, 0) };
        call_with(c, n[6] == 'F' ? "CreateFileA" : "CreateDirectoryA", a, 1, &r);
        if (n[6] == 'F' ? r == 0xFFFFFFFFu : !r) SETERR(c, ERROR_FILE_NOT_FOUND); }
    else if (!strcmp(n,"GetFileAttributesExW")){
        /* (name, level, WIN32_FILE_ATTRIBUTE_DATA*: attr, 3 FILETIME, size hi, lo) */
        w2a(AW(c,0), path, sizeof path);
        char real[1024]; struct stat st; uint32_t p = AW(c,2);
        if (!fileio_resolve_read(path, real, sizeof real) || stat(real, &st) != 0){ r = 0; SETERR(c, ERROR_FILE_NOT_FOUND); }
        else { if (p){ memset(g_mem + p, 0, 36);
                   MR32(p) = S_ISDIR(st.st_mode) ? 0x10u : 0x80u;
                   uint64_t ft = ((uint64_t)st.st_mtime + 11644473600ull) * 10000000ull;
                   for (int k = 0; k < 3; k++){ MR32(p+4+8u*k) = (uint32_t)ft; MR32(p+8+8u*k) = (uint32_t)(ft >> 32); }
                   if (!S_ISDIR(st.st_mode)){ MR32(p+28) = (uint32_t)((uint64_t)st.st_size >> 32); MR32(p+32) = (uint32_t)st.st_size; } }
               r = 1; } }
    else if (!strcmp(n,"FindFirstFileW") || !strcmp(n,"FindFirstFileExW")){
        int ex = n[13] == 'E';
        w2a(AW(c,0), path, sizeof path);
        uint32_t fa = scratch(1), fw = AW(c, ex ? 2 : 1);
        uint32_t a[2] = { put_guest_str(path, 0), fa };        /* FindFirstFileA(name, data) */
        call_with(c, "FindFirstFileA", a, 2, &r);
        if (r != 0xFFFFFFFFu) find_a2w(fa, fw); else SETERR(c, ERROR_FILE_NOT_FOUND); }
    else if (!strcmp(n,"FindNextFileW")){
        uint32_t fa = scratch(1), fw = AW(c,1);
        uint32_t a[2] = { AW(c,0), fa };
        call_with(c, "FindNextFileA", a, 2, &r);
        if (r) find_a2w(fa, fw); else SETERR(c, ERROR_NO_MORE_FILES); }
    else if (!strcmp(n,"GetFileSizeEx")){
        uint32_t hi = scratch(2), p = AW(c,1);
        uint32_t a[2] = { AW(c,0), hi };
        call_with(c, "GetFileSize", a, 2, &r);
        if (r == 0xFFFFFFFFu) r = 0;
        else { if (p){ MR32(p) = r; MR32(p+4) = MR32(hi); } r = 1; } }
    else if (!strcmp(n,"SetFilePointerEx")){
        /* (h, dist lo, dist hi, *newpos, method): 32-bit offsets are enough here */
        uint32_t np = AW(c,3);
        uint32_t a[4] = { AW(c,0), AW(c,1), 0, AW(c,4) };
        call_with(c, "SetFilePointer", a, 4, &r);
        if (r == 0xFFFFFFFFu) r = 0;
        else { if (np){ MR32(np) = r; MR32(np+4) = 0; } r = 1; } }
    else if (!strcmp(n,"GetFileInformationByHandleEx") || !strcmp(n,"SetFileInformationByHandle")
             || !strcmp(n,"MoveFileExW")) r = 0;   /* std::filesystem reports an error code */

    /* -- the exe's own late stubs (seen in session logs) -- */
    else if (!strcmp(n,"VirtualProtect")){
        /* (addr, size, new, *old): guest memory is all read/write.  A plugin
         * calls this before patching code; the patch itself is taken care of by
         * runtime/asi.c (the lifted code does not read its own bytes). */
        uint32_t po = AW(c,3); if (po) MR32(po) = 0x40u;        /* PAGE_EXECUTE_READWRITE */
        r = 1; }
    else if (!strcmp(n,"SetThreadPriority"))   r = 1;
    else if (!strcmp(n,"DeleteObject"))        r = 1;
    else if (!strcmp(n,"BeginPaint")){
        uint32_t ps = AW(c,1); if (ps) memset(g_mem + ps, 0, 64);
        r = 0x0000D0C0u; if (ps) MR32(ps) = r; }                 /* a fake HDC */
    else if (!strcmp(n,"EndPaint"))            r = 1;
    else return 0;
    *out = r;
    return 1;
}
