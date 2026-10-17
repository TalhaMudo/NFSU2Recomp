/* NFSRef -- Windows'ta orijinal oyundan TEKRARLANABILIR referans kareleri.
 *
 * Clean Edition'daki dinput8.dll bir ASI yukleyicisi; bu DLL "nfsref.asi"
 * adiyla SCRIPTS/ klasorune konur.  Port'un otomatik test kosusunu
 * (PACE60 + AUTOSEQ + DUMPFRAME) orijinal oyunda tekrarlar:
 *
 *   sanal saat  : QPC/timeGetTime/GetTickCount kare sayisindan turetilir
 *                 (kare basi tam 16667 us) -> "kare N" her makinede ayni an
 *   girdi       : klavye olaylari DirectInput eylem haritasindan, port'un
 *                 urettigi bicimde sentezlenir (gercek klavye/fare yok sayilir)
 *   ayarlar     : oyunun kayit defteri okumalari nfsref_kayit.txt'ten
 *                 (port'un ~/.nfsu2_registry bicimi); yazmalar diske gitmez
 *   kayitlar    : SHGetFolderPath -> bos bir klasor (mevcut profil karismaz)
 *   videolar    : port'un NOMOVIES=1'i gibi acilmaz
 *   cikti       : secilen karelerde arka tampon BMP, her kare icin ozet satiri,
 *                 secilen karelerde D3D cagri dokumu
 *
 * Her sey <oyun>/NFSRef_cikti/ altina yazilir.  Ayarlar nfsref.ini'de.
 * Cikti bicimi port tarafindaki REFLOG ile ayni tutulur (karsilastirma). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <d3d9.h>
/* dinput.h yerine gereken sabitler */
#define DI_OK S_OK
#define DIERR_INPUTLOST ((HRESULT)0x8007001EL)
#define DIERR_NOTACQUIRED ((HRESULT)0x8007000CL)
#define DIGDD_PEEK 0x00000001
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
static FILE *g_log, *g_kare;
static FILE *g_slog, *g_sdat; static CRITICAL_SECTION g_slk;   /* ses kaydi */
static char g_game[MAX_PATH], g_out[MAX_PATH], g_save[MAX_PATH];
static CRITICAL_SECTION g_lk;

static void L(const char *fmt, ...){
    if (!g_log) return;
    va_list ap; va_start(ap, fmt);
    EnterCriticalSection(&g_lk);
    vfprintf(g_log, fmt, ap); fflush(g_log);
    LeaveCriticalSection(&g_lk);
    va_end(ap);
}

/* ---- ayarlar ------------------------------------------------------ */
#define SQMAX 64
static long  sq_f[SQMAX], sq_d[SQMAX]; static unsigned char sq_k[SQMAX]; static int sq_n;
#define CAPMAX 64
static long  cap_f[CAPMAX]; static int cap_n;
static long  dump_f[CAPMAX]; static int dump_n;
static long  g_quit = 0;
static int   g_mx = 5, g_my = 5, g_nomovies = 1, g_pace = 1;

static void parse_seq(const char *q){
    for (; q && *q && sq_n < SQMAX; ){
        while (*q == ',' || *q == ' ') q++;
        if (!*q) break;
        sq_f[sq_n] = strtol(q, 0, 0);
        const char *c2 = strchr(q, ':'); if (!c2) break;
        sq_k[sq_n] = (unsigned char)strtoul(c2+1, 0, 0);
        const char *c3 = strchr(c2+1, ':'), *cm = strchr(q, ',');
        sq_d[sq_n] = (c3 && (!cm || c3 < cm)) ? strtol(c3+1, 0, 0) : 8;
        if (sq_d[sq_n] < 1) sq_d[sq_n] = 8;
        sq_n++;
        while (*q && *q != ',') q++;
    }
}
static int parse_list(const char *q, long *a, int max){
    int n = 0;
    for (; q && *q && n < max; ){
        while (*q == ',' || *q == ' ') q++;
        if (!*q) break;
        a[n++] = strtol(q, 0, 0);
        while (*q && *q != ',') q++;
    }
    return n;
}
static int in_list(const long *a, int n, long f){
    for (int i = 0; i < n; i++) if (a[i] == f) return 1;
    return 0;
}

/* ---- kayit defteri (port'un ~/.nfsu2_registry bicimi: "d ad=sayi" / "s ad=metin") */
#define MAXREG 128
static struct { char name[64]; int is_str; DWORD dw; char sv[260]; } g_reg[MAXREG];
static int g_regn;
static int reg_find(const char *nm){
    for (int i = 0; i < g_regn; i++) if (!_stricmp(g_reg[i].name, nm)) return i;
    return -1;
}
static void reg_put(const char *nm, int is_str, DWORD dw, const char *sv){
    int i = reg_find(nm);
    if (i < 0){ if (g_regn >= MAXREG) return; i = g_regn++;
                snprintf(g_reg[i].name, 64, "%s", nm); }
    g_reg[i].is_str = is_str; g_reg[i].dw = dw;
    snprintf(g_reg[i].sv, sizeof g_reg[i].sv, "%s", sv ? sv : "");
}
static void reg_load(const char *path){
    FILE *f = fopen(path, "r");
    if (!f){ L("UYARI: %s yok, varsayilan kayit degerleri\n", path); return; }
    char ln[512];
    while (fgets(ln, sizeof ln, f)){
        ln[strcspn(ln, "\r\n")] = 0;
        if ((ln[0] != 'd' && ln[0] != 's') || ln[1] != ' ') continue;
        char *eq = strchr(ln+2, '='); if (!eq) continue;
        *eq = 0;
        if (ln[0] == 'd') reg_put(ln+2, 0, (DWORD)strtoul(eq+1, 0, 0), 0);
        else              reg_put(ln+2, 1, 0, eq+1);
    }
    fclose(f);
}

static void modof(DWORD a, char *out, int n);

/* ---- sanal saat ---------------------------------------------------- */
static volatile LONG g_frame;            /* simdiye kadarki Present sayisi */
static DWORD g_main_tid;                 /* Present cagiran thread */
static volatile LONG g_polls;            /* ana thread, bu karedeki saat sorgusu */
static LONG g_polls_max, g_valve_frames;
static volatile LONGLONG g_extra_us;     /* emniyet valfinin ekledigi */
#define VALVE_AT   50000                 /* bu kadar sorgudan sonra ... */
#define VALVE_STEP 500                   /* ... her 500 sorguda +1 ms */

static LARGE_INTEGER g_t0, g_qf;
static volatile LONG g_started;          /* ilk Present oldu mu */
static uint64_t real_us(void){
    LARGE_INTEGER t; QueryPerformanceCounter(&t);   /* kendi IAT'imiz: gercek */
    return (uint64_t)((t.QuadPart - g_t0.QuadPart) * 1000000.0 / (double)g_qf.QuadPart);
}
/* Ilk kareye kadar saat GERCEK zamanla akar (port'ta da baslangicta saat
 * ilerliyor; donuk saat baslangic kodunu bozabilir).  Ilk Present'te
 * deterministik sayima gecilir: t = 1 sn + kare*16667 us + ek. */
static volatile LONGLONG g_last_pres_us;   /* son Present'in gercek zamani */
#define STALL_US 250000ull
static LONG g_qpc_main, g_qpc_other;        /* durum satiri icin */
/* Kareler durursa (yukleme, ya da kare sayimi bozulursa) saat GERCEK zamana
 * duser: oyun asla gercekten hizli akamaz. */
static uint64_t stall_us(void){
    uint64_t r = real_us(), lp = (uint64_t)g_last_pres_us;
    return (g_started && r > lp + STALL_US) ? r - lp - STALL_US : 0;
}
static uint64_t vt_us(void){
    if (!g_started) return 1000000ull + real_us();
    if (GetCurrentThreadId() == g_main_tid){
        g_qpc_main++;
        LONG n = InterlockedIncrement(&g_polls);
        if (n > VALVE_AT && (n % VALVE_STEP) == 0) g_extra_us += 1000;
    } else g_qpc_other++;
    return 1000000ull + (uint64_t)g_frame * 16667ull + (uint64_t)g_extra_us + stall_us();
}
static BOOL WINAPI h_QPC(LARGE_INTEGER *p){ if (p) p->QuadPart = (LONGLONG)vt_us(); return TRUE; }
static BOOL WINAPI h_QPF(LARGE_INTEGER *p){ if (p) p->QuadPart = 1000000; return TRUE; }
static DWORD WINAPI h_GetTickCount(void){ return (DWORD)(vt_us() / 1000ull) + 1000u; }
static DWORD WINAPI h_timeGetTime(void){ return (DWORD)(vt_us() / 1000ull) + 1000u; }
static void WINAPI h_GetSystemTimeAsFileTime(FILETIME *p){
    /* port ile ayni: sabit taban, her cagrida +10 ms */
    static volatile LONGLONG ft = 130000000000000000LL;
    LONGLONG v = InterlockedExchangeAdd64(&ft, 100000) + 100000;
    if (p){ p->dwLowDateTime = (DWORD)v; p->dwHighDateTime = (DWORD)(v >> 32); }
}

/* ---- girdi durumu (port metal.m AUTOSEQ ile ayni kural) ------------- */
static unsigned char g_keys[256];
static void seq_update(long f){
    for (int q = 0; q < sq_n; q++){
        int on = (f >= sq_f[q] && f < sq_f[q] + sq_d[q]);
        if (on || f == sq_f[q] + sq_d[q]) g_keys[sq_k[q]] = on ? 1 : 0;
    }
}

/* ---- IAT yamasi ----------------------------------------------------- */
static int iat_patch(HMODULE m, const char *dll, const char *fn, WORD ord, void *nw, void **old){
    BYTE *b = (BYTE *)m;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew);
    IMAGE_DATA_DIRECTORY *dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dd->VirtualAddress) return 0;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (void *)(b + dd->VirtualAddress); d->Name; d++){
        if (_stricmp((char *)(b + d->Name), dll)) continue;
        IMAGE_THUNK_DATA *ot = (void *)(b + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        IMAGE_THUNK_DATA *ft = (void *)(b + d->FirstThunk);
        for (; ot->u1.AddressOfData; ot++, ft++){
            int hit;
            if (IMAGE_SNAP_BY_ORDINAL(ot->u1.Ordinal)) hit = (fn == 0 && IMAGE_ORDINAL(ot->u1.Ordinal) == ord);
            else hit = fn && !strcmp((char *)((IMAGE_IMPORT_BY_NAME *)(b + ot->u1.AddressOfData))->Name, fn);
            if (!hit) continue;
            DWORD op;
            VirtualProtect(&ft->u1.Function, 4, PAGE_READWRITE, &op);
            if (old) *old = (void *)ft->u1.Function;
            ft->u1.Function = (DWORD_PTR)nw;
            VirtualProtect(&ft->u1.Function, 4, op, &op);
            return 1;
        }
    }
    return 0;
}
static void vt_patch(void **vt, int idx, void *nw, void **old){
    if (vt[idx] == nw) return;            /* ayni vtable ikinci kez */
    DWORD op;
    VirtualProtect(&vt[idx], 4, PAGE_READWRITE, &op);
    if (old) *old = vt[idx];
    vt[idx] = nw;
    VirtualProtect(&vt[idx], 4, op, &op);
}

/* ---- kayit defteri kancalari ------------------------------------- */
#define FAKEKEY ((HKEY)(ULONG_PTR)0x4E465200u)
#define ILK(nm) do { static volatile LONG once; if (!InterlockedExchange(&once, 1)) \
        L("ilk cagri %s (thread %lu, kare %ld)\n", nm, (unsigned long)GetCurrentThreadId(), g_frame); } while (0)
static int is_fake(HKEY k){ return ((ULONG_PTR)k & 0xFFFFFF00u) == 0x4E465200u; }
static int is_game_path(const char *s){
    if (!s) return 0;
    char t[512]; snprintf(t, sizeof t, "%s", s); _strlwr(t);
    return strstr(t, "underground") || strstr(t, "ea games") || strstr(t, "electronic arts");
}
typedef LONG (WINAPI *RegOpenKeyExA_t)(HKEY, LPCSTR, DWORD, REGSAM, PHKEY);
typedef LONG (WINAPI *RegOpenKeyA_t)(HKEY, LPCSTR, PHKEY);
typedef LONG (WINAPI *RegQueryValueExA_t)(HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
typedef LONG (WINAPI *RegSetValueExA_t)(HKEY, LPCSTR, DWORD, DWORD, const BYTE *, DWORD);
typedef LONG (WINAPI *RegCloseKey_t)(HKEY);
static RegOpenKeyExA_t o_RegOpenKeyExA; static RegOpenKeyA_t o_RegOpenKeyA, o_RegCreateKeyA;
static RegQueryValueExA_t o_RegQueryValueExA; static RegSetValueExA_t o_RegSetValueExA;
static RegCloseKey_t o_RegCloseKey;

static LONG WINAPI h_RegOpenKeyExA(HKEY k, LPCSTR s, DWORD o, REGSAM sam, PHKEY out){
    if (is_fake(k) || is_game_path(s)){ L("kayit: OpenKeyEx(\"%s\") -> sanal\n", s ? s : ""); if (out) *out = FAKEKEY; return ERROR_SUCCESS; }
    return o_RegOpenKeyExA(k, s, o, sam, out);
}
static LONG WINAPI h_RegOpenKeyA(HKEY k, LPCSTR s, PHKEY out){
    if (is_fake(k) || is_game_path(s)){ L("kayit: OpenKey(\"%s\") -> sanal\n", s ? s : ""); if (out) *out = FAKEKEY; return ERROR_SUCCESS; }
    return o_RegOpenKeyA(k, s, out);
}
static LONG WINAPI h_RegCreateKeyA(HKEY k, LPCSTR s, PHKEY out){
    if (is_fake(k) || is_game_path(s)){ L("kayit: CreateKey(\"%s\") -> sanal\n", s ? s : ""); if (out) *out = FAKEKEY; return ERROR_SUCCESS; }
    return o_RegCreateKeyA(k, s, out);
}
static LONG WINAPI h_RegQueryValueExA(HKEY k, LPCSTR nm, LPDWORD rsv, LPDWORD type, LPBYTE data, LPDWORD cb){
    if (!is_fake(k)) return o_RegQueryValueExA(k, nm, rsv, type, data, cb);
    const char *vn = nm ? nm : "";
    int i = reg_find(vn);
    LONG r;
    if (i < 0){ if (cb) *cb = 0; r = ERROR_FILE_NOT_FOUND; }
    else if (g_reg[i].is_str){
        DWORD need = (DWORD)strlen(g_reg[i].sv) + 1;
        if (type) *type = REG_SZ;
        if (!data){ if (cb) *cb = need; r = ERROR_SUCCESS; }
        else if (cb && *cb < need){ *cb = need; r = ERROR_MORE_DATA; }
        else { memcpy(data, g_reg[i].sv, need); if (cb) *cb = need; r = ERROR_SUCCESS; }
    } else {
        if (type) *type = REG_DWORD;
        if (!data){ if (cb) *cb = 4; r = ERROR_SUCCESS; }
        else if (cb && *cb < 4){ *cb = 4; r = ERROR_MORE_DATA; }
        else { memcpy(data, &g_reg[i].dw, 4); if (cb) *cb = 4; r = ERROR_SUCCESS; }
    }
    L("kayit: Query(\"%s\") -> %s\n", vn, r == ERROR_SUCCESS ? "var" : "yok");
    return r;
}
static LONG WINAPI h_RegSetValueExA(HKEY k, LPCSTR nm, DWORD rsv, DWORD type, const BYTE *data, DWORD cb){
    if (!is_fake(k)) return o_RegSetValueExA(k, nm, rsv, type, data, cb);
    if (nm && *nm && data){
        if (type == REG_DWORD && cb >= 4){ DWORD v; memcpy(&v, data, 4); reg_put(nm, 0, v, 0); L("kayit: Set(\"%s\")=%lu\n", nm, (unsigned long)v); }
        else if (type == REG_SZ){ char t[260]; snprintf(t, sizeof t, "%.*s", (int)cb, (const char *)data); reg_put(nm, 1, 0, t); L("kayit: Set(\"%s\")=\"%s\"\n", nm, t); }
    }
    return ERROR_SUCCESS;             /* diske yazilmaz */
}
static LONG WINAPI h_RegCloseKey(HKEY k){ ILK("RegCloseKey"); return is_fake(k) ? ERROR_SUCCESS : o_RegCloseKey(k); }

/* ---- dosya / kabuk / imlec / surucu -------------------------------- */
typedef HANDLE (WINAPI *CreateFileA_t)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static CreateFileA_t o_CreateFileA;
static HANDLE WINAPI h_CreateFileA(LPCSTR p, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD cd, DWORD fl, HANDLE t){ ILK("CreateFileA");
    if (g_nomovies && p){
        const char *e = strrchr(p, '.');
        if (e && (!_stricmp(e, ".vp6") || !_stricmp(e, ".vp8") || !_stricmp(e, ".bik") || !_stricmp(e, ".mpg"))){
            L("dosya: video atlandi %s\n", p);
            SetLastError(ERROR_FILE_NOT_FOUND); return INVALID_HANDLE_VALUE;
        }
    }
    return o_CreateFileA(p, a, s, sa, cd, fl, t);
}
static HRESULT WINAPI h_SHGetFolderPathA(HWND w, int csidl, HANDLE tok, DWORD fl, LPSTR out){ ILK("SHGetFolderPathA");
    (void)w; (void)tok; (void)fl;
    L("kabuk: SHGetFolderPathA(csidl=0x%x) -> %s\n", csidl, g_save);
    if (out) snprintf(out, MAX_PATH, "%s", g_save);
    return S_OK;
}
static BOOL WINAPI h_GetCursorPos(POINT *p){ ILK("GetCursorPos"); if (p){ p->x = g_mx; p->y = g_my; } return TRUE; }
static UINT WINAPI h_GetDriveTypeA(LPCSTR r){ ILK("GetDriveTypeA"); (void)r; return DRIVE_FIXED; }
static DWORD WINAPI h_GetLogicalDrives(void){ ILK("GetLogicalDrives"); return 0x0000000Cu; }

/* ---- DirectInput ---------------------------------------------------- */
/* Oyun eylem haritasi kullaniyor: cihazlari EnumDevicesBySemantics ile
 * DirectInput'un KENDISININ yarattigi nesneler olarak alabilir, yani
 * CreateDevice'a ugramayabilir.  Bu yuzden: DirectInput8Create'ten hemen
 * sonra kendimiz bir klavye ve fare yaratip vtable'larini yamaliyoruz
 * (ayni turdeki tum cihazlar ayni vtable'i paylasir); kancada cihazin
 * turunu GetDeviceInfo ile sorup taniyoruz.
 * IDirectInput8A: 3 CreateDevice.  IDirectInputDevice8A: 2 Release,
 * 7 Acquire, 9 GetDeviceState, 10 GetDeviceData, 15 GetDeviceInfo,
 * 30 SetActionMap */
typedef HRESULT (WINAPI *DI8Create_t)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);
typedef HRESULT (WINAPI *CreateDevice_t)(void *, REFGUID, void **, LPUNKNOWN);
typedef HRESULT (WINAPI *GetDeviceState_t)(void *, DWORD, LPVOID);
typedef HRESULT (WINAPI *GetDeviceData_t)(void *, DWORD, void *, LPDWORD, DWORD);
typedef HRESULT (WINAPI *SetActionMap_t)(void *, void *, LPCSTR, DWORD);
typedef HRESULT (WINAPI *Acquire_t)(void *);
typedef HRESULT (WINAPI *GetDeviceInfo_t)(void *, void *);
typedef ULONG   (WINAPI *Release_t)(void *);
static DI8Create_t o_DI8Create; static CreateDevice_t o_CreateDevice;
static const GUID G_SysKeyboard = {0x6F1D2B61,0xD5A0,0x11CF,{0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00}};
static const GUID G_SysMouse    = {0x6F1D2B60,0xD5A0,0x11CF,{0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00}};
static DWORD g_act_app[256], g_act_ofs[256]; static int g_act_count;
static unsigned char g_kb_prev[256]; static DWORD g_di_seq = 1;
static LONG g_gdd_kbd, g_gds_kbd, g_ev_out;

/* vtable basina asil fonksiyonlar (klavye/fare vtable'lari farkli olabilir) */
static struct { void **vt; GetDeviceState_t gds; GetDeviceData_t gdd; SetActionMap_t sam;
                Acquire_t acq; GetDeviceInfo_t gdi; } g_dvt[8];
static int g_ndvt;
static int dvt_of(void *self){
    void **vt = *(void ***)self;
    for (int i = 0; i < g_ndvt; i++) if (g_dvt[i].vt == vt) return i;
    return 0;
}
/* cihaz turu: 1 klavye, 2 fare, 0 diger (onbellekli) */
static struct { void *p; int k; } g_dk[32]; static int g_ndk;
static int dev_kind(void *self){
    for (int i = 0; i < g_ndk; i++) if (g_dk[i].p == self) return g_dk[i].k;
    BYTE di[580]; memset(di, 0, sizeof di); *(DWORD *)di = sizeof di;
    int k = 0;
    if (SUCCEEDED(g_dvt[dvt_of(self)].gdi(self, di))){
        DWORD t = *(DWORD *)(di + 36) & 0xFF;            /* dwDevType */
        k = (t == 0x13) ? 1 : (t == 0x12) ? 2 : 0;       /* DI8DEVTYPE_KEYBOARD / MOUSE */
        L("giris: cihaz %p tur 0x%02lx \"%s\" -> %s\n", self, (unsigned long)t, (char *)(di + 40),
          k == 1 ? "klavye" : k == 2 ? "fare" : "diger (dokunulmuyor)");
    } else L("giris: cihaz %p GetDeviceInfo basarisiz\n", self);
    if (g_ndk < 32){ g_dk[g_ndk].p = self; g_dk[g_ndk].k = k; g_ndk++; }
    return k;
}

static HRESULT WINAPI h_SetActionMap(void *self, void *af_, LPCSTR user, DWORD fl){
    HRESULT r = g_dvt[dvt_of(self)].sam(self, af_, user, fl);
    if (dev_kind(self) == 1 && af_){
        /* DIACTIONFORMATA: +4 dwActionSize +12 dwNumActions +16 rgoAction
         * DIACTIONA: +0 uAppData +4 dwSemantic (0x810004xx = klavye DIK) */
        BYTE *af = af_;
        DWORD nact = *(DWORD *)(af+12), asz = *(DWORD *)(af+4); BYTE *rg = *(BYTE **)(af+16);
        if (!asz) asz = 40;
        memset(g_act_app, 0, sizeof g_act_app); memset(g_act_ofs, 0, sizeof g_act_ofs); g_act_count = 0;
        for (DWORD k = 0; rg && k < nact && k < 1024; k++){
            DWORD app = *(DWORD *)(rg + k*asz), sem = *(DWORD *)(rg + k*asz + 4);
            if ((sem & 0xFFFFFF00u) != 0x81000400u) continue;
            DWORD dik = sem & 0xFF; if (!dik) continue;
            g_act_app[dik] = app; g_act_ofs[dik] = k*4; g_act_count++;
        }
        L("giris: SetActionMap klavye %lu eylem, %d tus esli, Enter %s (kare %ld, sonuc 0x%08lx)\n",
          (unsigned long)nact, g_act_count, g_act_app[0x1C] ? "esli" : "ESLI DEGIL", g_frame, (unsigned long)r);
    }
    return r;
}
static HRESULT WINAPI h_GetDeviceState(void *self, DWORD cb, LPVOID p){
    int k = dev_kind(self);
    if (!k) return g_dvt[dvt_of(self)].gds(self, cb, p);
    BYTE tmp[512]; if (cb <= sizeof tmp) g_dvt[dvt_of(self)].gds(self, cb, tmp);   /* gercek girdiyi yut */
    if (p && cb){
        memset(p, 0, cb);
        if (k == 1 && cb == 256){ g_gds_kbd++; for (int q = 0; q < 256; q++) if (g_keys[q]) ((BYTE *)p)[q] = 0x80; }
    }
    return DI_OK;
}
static HRESULT WINAPI h_GetDeviceData(void *self, DWORD cb, void *rg, LPDWORD pn, DWORD fl){
    int k = dev_kind(self), vi = dvt_of(self);
    if (!k) return g_dvt[vi].gdd(self, cb, rg, pn, fl);
    /* gercek olaylari bosalt (kullanicinin dokunusu karismasin) */
    { BYTE tmp[32*64]; DWORD n; HRESULT rr;
      if (cb <= 32){ for (int g = 0; g < 8; g++){ n = 64; rr = g_dvt[vi].gdd(self, cb, tmp, &n, 0);
          if (rr == DIERR_INPUTLOST || rr == DIERR_NOTACQUIRED){ g_dvt[vi].acq(self); break; }
          if (FAILED(rr) || n == 0) break; } } }
    DWORD want = pn ? *pn : 0, n = 0;
    if (k == 1){
        g_gdd_kbd++;
        if (g_act_count && cb >= 16 && cb <= 32){
            for (int q = 1; q < 256 && n < want; q++){
                if (!g_act_app[q]) continue;
                unsigned char now = g_keys[q] ? 1 : 0;
                if (now == g_kb_prev[q]) continue;
                if (rg && !(fl & DIGDD_PEEK)){
                    BYTE *e = (BYTE *)rg + n*cb; memset(e, 0, cb);
                    ((DWORD *)e)[0] = g_act_ofs[q];
                    ((DWORD *)e)[1] = now ? 0x80 : 0;
                    ((DWORD *)e)[2] = (DWORD)(vt_us() / 1000ull);
                    ((DWORD *)e)[3] = g_di_seq++;
                    if (cb >= 20) ((DWORD *)e)[4] = g_act_app[q];
                    g_ev_out++;
                    L("giris: kare %ld dik 0x%02x %s\n", g_frame, q, now ? "bas" : "birak");
                }
                if (!(fl & DIGDD_PEEK)) g_kb_prev[q] = now;
                n++;
            }
        }
    }
    if (pn) *pn = n;
    return DI_OK;
}
static void di_patch_vt(void *dev, const char *ne){
    void **vt = *(void ***)dev;
    for (int i = 0; i < g_ndvt; i++) if (g_dvt[i].vt == vt){ L("giris: %s vtable %p zaten yamali\n", ne, vt); return; }
    if (g_ndvt >= 8) return;
    int i = g_ndvt++;
    g_dvt[i].vt = vt; g_dvt[i].acq = (Acquire_t)vt[7]; g_dvt[i].gdi = (GetDeviceInfo_t)vt[15];
    vt_patch(vt, 9,  (void *)h_GetDeviceState, (void **)&g_dvt[i].gds);
    vt_patch(vt, 10, (void *)h_GetDeviceData,  (void **)&g_dvt[i].gdd);
    vt_patch(vt, 30, (void *)h_SetActionMap,   (void **)&g_dvt[i].sam);
    L("giris: %s vtable %p yamandi\n", ne, vt);
}
static HRESULT WINAPI h_CreateDevice2(void *self, REFGUID g, void **out, LPUNKNOWN u){
    HRESULT r = o_CreateDevice(self, g, out, u);
    if (SUCCEEDED(r) && out && *out){
        L("giris: oyun CreateDevice -> %p\n", *out);
        di_patch_vt(*out, "oyun cihazi");
    }
    return r;
}
static HRESULT WINAPI h_DI8Create(HINSTANCE h, DWORD v, REFIID id, LPVOID *out, LPUNKNOWN u){ ILK("DirectInput8Create");
    HRESULT r = o_DI8Create(h, v, id, out, u);
    L("giris: DirectInput8Create -> 0x%08lx\n", (unsigned long)r);
    if (SUCCEEDED(r) && out && *out){
        void **vt = *(void ***)*out;
        vt_patch(vt, 3, (void *)h_CreateDevice2, (void **)&o_CreateDevice);
        /* klavye ve fare vtable'larini simdiden yamala (EnumDevicesBySemantics yolu) */
        void *d = 0;
        if (SUCCEEDED(o_CreateDevice(*out, &G_SysKeyboard, &d, 0)) && d){ di_patch_vt(d, "klavye"); ((Release_t)(*(void ***)d)[2])(d); }
        else L("giris: UYARI deneme klavyesi yaratilamadi\n");
        d = 0;
        if (SUCCEEDED(o_CreateDevice(*out, &G_SysMouse, &d, 0)) && d){ di_patch_vt(d, "fare"); ((Release_t)(*(void ***)d)[2])(d); }
        else L("giris: UYARI deneme faresi yaratilamadi\n");
    }
    return r;
}

/* ---- D3D9 ----------------------------------------------------------- */
typedef IDirect3D9 *(WINAPI *D3DCreate9_t)(UINT);
typedef HRESULT (WINAPI *D3DCreateDevice_t)(IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **);
static D3DCreate9_t o_D3DCreate9; static D3DCreateDevice_t o_D3DCreateDevice;
static LONG c_pres_vt, c_pres_sc, c_begin, c_end, c_draw_all;
static IDirect3DDevice9 *g_dev; static void **g_dev_vt; static int g_hp_on = 1;
static int g_dump_on;                         /* bu karede cagri dokumu */
static FILE *g_dump;
static LONG g_draws, g_prims;                 /* bu karedeki */
static float g_cam[16]; static int g_cam_have;

/* Asil fonksiyonlar vtable BASINA saklanir: oyunun kullandigi nesne
 * CreateDevice'in dondurdugunden farkli bir vtable tasiyabiliyor (Windows'ta
 * olculdu: tablo kancalari hic cagri gormedi, Present hotpatch'i gordu). */
static void *volatile g_cur[160];          /* yuva -> su an cagrilacak asil fonksiyon */
#define O(name, d) ((name##_t)g_cur[name##_IDX])
#define DEVFN(idx, ret, name, args) typedef ret (WINAPI *name##_t) args; enum { name##_IDX = idx };
DEVFN(13, HRESULT, CreateAdditionalSwapChain, (IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *, void **))
DEVFN(14, HRESULT, GetSwapChain, (IDirect3DDevice9 *, UINT, void **))
DEVFN(41, HRESULT, BeginScene, (IDirect3DDevice9 *))
DEVFN(42, HRESULT, EndScene, (IDirect3DDevice9 *))
DEVFN(16, HRESULT, Reset,   (IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *))
DEVFN(17, HRESULT, Present, (IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA *))
DEVFN(34, HRESULT, StretchRect, (IDirect3DDevice9 *, IDirect3DSurface9 *, const RECT *, IDirect3DSurface9 *, const RECT *, D3DTEXTUREFILTERTYPE))
DEVFN(37, HRESULT, SetRenderTarget, (IDirect3DDevice9 *, DWORD, IDirect3DSurface9 *))
DEVFN(43, HRESULT, Clear,   (IDirect3DDevice9 *, DWORD, const D3DRECT *, DWORD, D3DCOLOR, float, DWORD))
DEVFN(44, HRESULT, SetTransform, (IDirect3DDevice9 *, D3DTRANSFORMSTATETYPE, const D3DMATRIX *))
DEVFN(47, HRESULT, SetViewport, (IDirect3DDevice9 *, const D3DVIEWPORT9 *))
DEVFN(57, HRESULT, SetRenderState, (IDirect3DDevice9 *, D3DRENDERSTATETYPE, DWORD))
DEVFN(60, HRESULT, BeginStateBlock, (IDirect3DDevice9 *))
DEVFN(61, HRESULT, EndStateBlock, (IDirect3DDevice9 *, IDirect3DStateBlock9 **))
DEVFN(65, HRESULT, SetTexture, (IDirect3DDevice9 *, DWORD, IDirect3DBaseTexture9 *))
DEVFN(67, HRESULT, SetTextureStageState, (IDirect3DDevice9 *, DWORD, D3DTEXTURESTAGESTATETYPE, DWORD))
DEVFN(69, HRESULT, SetSamplerState, (IDirect3DDevice9 *, DWORD, D3DSAMPLERSTATETYPE, DWORD))
DEVFN(81, HRESULT, DrawPrimitive, (IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT))
DEVFN(82, HRESULT, DrawIndexedPrimitive, (IDirect3DDevice9 *, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT))
DEVFN(83, HRESULT, DrawPrimitiveUP, (IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, const void *, UINT))
DEVFN(84, HRESULT, DrawIndexedPrimitiveUP, (IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void *, D3DFORMAT, const void *, UINT))
DEVFN(59, HRESULT, CreateStateBlock, (IDirect3DDevice9 *, D3DSTATEBLOCKTYPE, IDirect3DStateBlock9 **))
DEVFN(86, HRESULT, CreateVertexDeclaration, (IDirect3DDevice9 *, const D3DVERTEXELEMENT9 *, IDirect3DVertexDeclaration9 **))
DEVFN(87, HRESULT, SetVertexDeclaration, (IDirect3DDevice9 *, IDirect3DVertexDeclaration9 *))
DEVFN(89, HRESULT, SetFVF, (IDirect3DDevice9 *, DWORD))
DEVFN(100, HRESULT, SetStreamSource, (IDirect3DDevice9 *, UINT, IDirect3DVertexBuffer9 *, UINT, UINT))
DEVFN(91, HRESULT, CreateVertexShader, (IDirect3DDevice9 *, const DWORD *, IDirect3DVertexShader9 **))
DEVFN(92, HRESULT, SetVertexShader, (IDirect3DDevice9 *, IDirect3DVertexShader9 *))
DEVFN(94, HRESULT, SetVertexShaderConstantF, (IDirect3DDevice9 *, UINT, const float *, UINT))
DEVFN(106, HRESULT, CreatePixelShader, (IDirect3DDevice9 *, const DWORD *, IDirect3DPixelShader9 **))
DEVFN(107, HRESULT, SetPixelShader, (IDirect3DDevice9 *, IDirect3DPixelShader9 *))
DEVFN(109, HRESULT, SetPixelShaderConstantF, (IDirect3DDevice9 *, UINT, const float *, UINT))

/* gölgelendirici kimligi: bayt kodunun FNV-1a ozeti (port da ayni sekilde) */
#define MAXSH 1024
static struct { void *p; uint32_t h; } g_sh[MAXSH]; static int g_shn;
static uint32_t sh_hash(const DWORD *t){
    uint32_t h = 2166136261u; int n = 0;
    for (; n < 65536; n++){ uint32_t w = t[n];
        for (int b = 0; b < 4; b++){ h ^= (w >> (8*b)) & 0xFF; h *= 16777619u; }
        if (w == 0x0000FFFFu) break; }
    return h;
}
static void sh_put(void *p, uint32_t h){
    for (int i = 0; i < g_shn; i++) if (g_sh[i].p == p){ g_sh[i].h = h; return; }
    if (g_shn < MAXSH){ g_sh[g_shn].p = p; g_sh[g_shn].h = h; g_shn++; }
}
static uint32_t sh_get(void *p){
    if (!p) return 0;
    for (int i = 0; i < g_shn; i++) if (g_sh[i].p == p) return g_sh[i].h;
    return 0xFFFFFFFFu;
}
/* ---- dokum cikisi + durum blogu kaydi ----------------------------------
 * Cihaz PUREDEVICE (bayrak 0x50): Get* durum sorgulari YOK.  Bu yuzden
 * durumun golgesini kendimiz tutuyoruz: her Set* golgeye yazilir; durum
 * blogu kaydi (BeginStateBlock..End) sirasinda cagrilar cihaza UYGULANMAZ,
 * bloga YAPI olarak kaydedilir; Capture blogu golgeden tazeler; Apply blogu
 * golgeye uygular ve dokum karesindeyse icerigini (gercek degerleriyle) yazar.
 * Port'taki com.c durum blogu modeliyle ayni. */
static int g_sbrec;
#define DON (g_dump_on && g_dump)
#define D(...) do { if (DON) fprintf(g_dump, __VA_ARGS__); } while (0)
static void dumpf(const float *v, UINT n4){
    for (UINT i = 0; i < n4*4; i++) D(" %.9g", v[i]);
    D("\n");
}
enum { E_RS = 1, E_TSS, E_SS, E_TEX, E_VS, E_PS, E_VSC, E_PSC, E_XF, E_VP, E_DECL, E_FVF };
typedef struct { BYTE t; WORD a, b, n; DWORD v; void *p; float *f; char *desc; } SBE;
typedef struct { void *obj; int n, cap, kind; SBE *e; void *snap; } SB;
#define MAXSB 4096
static SB g_sb[MAXSB]; static int g_nsb; static SB g_sbcur;
static LONG c_sb_apply, c_sb_capture;
static int g_in_sb;                     /* d3d9 Apply/Capture ICINDE: ic cagrilar dokulmez */
static LONG c_sb_inner;
/* golge durum */
typedef struct {
    DWORD rs[256], tss[8][33], ss[16][14], fvf;
    void *tex[16], *vs, *ps, *decl;
    float vsc[256][4], psc[32][4];
} Shadow;
static Shadow sh;

/* doku kimligi: boyut/bicim + (yonetilen dokuda) seviye 0 iceriginin FNV ozeti */
#define MAXTH 8192
static struct { void *p; uint32_t h; } g_th[MAXTH]; static int g_nth;
static int fmt_rowbytes(D3DFORMAT f, UINT w, UINT *rows_div){
    *rows_div = 1;
    switch ((DWORD)f){
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_A8B8G8R8: case D3DFMT_X8B8G8R8: return (int)w*4;
    case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4: case D3DFMT_A8L8: return (int)w*2;
    case D3DFMT_A8: case D3DFMT_L8: case D3DFMT_P8: return (int)w;
    case D3DFMT_DXT1: *rows_div = 4; return (int)((w+3)/4)*8;
    case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5: *rows_div = 4; return (int)((w+3)/4)*16;
    default: return 0;
    }
}
static uint32_t fnv(uint32_t h, const BYTE *p, size_t n){ for (size_t i = 0; i < n; i++){ h ^= p[i]; h *= 16777619u; } return h; }
static uint32_t locked_hash(const D3DLOCKED_RECT *lr, D3DSURFACE_DESC *ds){
    UINT rd; int rb = fmt_rowbytes(ds->Format, ds->Width, &rd);
    if (rb <= 0 || !lr->pBits) return 0;
    UINT rows = (ds->Height + rd - 1) / rd; uint32_t h = 2166136261u;
    for (UINT y = 0; y < rows; y++) h = fnv(h, (const BYTE *)lr->pBits + y*lr->Pitch, (size_t)rb);
    return h;
}
/* RT dokulari: dokum karesinde gorulenler, kare sonunda goruntu olarak yazilir */
#define MAXRTD 48
static IDirect3DBaseTexture9 *g_rtd[MAXRTD]; static int g_nrtd;
static int g_tex_hash_on = 1, g_rt_dump_on = 1;
static void rt_note(IDirect3DBaseTexture9 *t){
    for (int i = 0; i < g_nrtd; i++) if (g_rtd[i] == t) return;
    if (g_nrtd < MAXRTD){ IDirect3DBaseTexture9_AddRef(t); g_rtd[g_nrtd++] = t; }
}
/* RT dokulara kalici kimlik (ilk gorulme sirasi) */
#define MAXRTID 256
static void *g_rtid[MAXRTID]; static int g_nrtid;
static int rt_id(void *t){
    for (int i = 0; i < g_nrtid; i++) if (g_rtid[i] == t) return i;
    if (g_nrtid < MAXRTID){ g_rtid[g_nrtid] = t; return g_nrtid++; }
    return -1;
}
/* doku tanimi: nesne YASARKEN cagrilmali (dogrudan SetTexture ya da kayit/Capture ani) */
static void tex_desc(void *tp, char *out, int n, int *is_rt){
    IDirect3DBaseTexture9 *t = tp; *is_rt = 0;
    if (!t){ snprintf(out, n, "yok"); return; }
    if (IsBadReadPtr(t, 4)){ snprintf(out, n, "gecersiz"); return; }
    D3DRESOURCETYPE ty = IDirect3DBaseTexture9_GetType(t);
    D3DSURFACE_DESC ds; memset(&ds, 0, sizeof ds);
    if (ty == D3DRTYPE_TEXTURE) IDirect3DTexture9_GetLevelDesc((IDirect3DTexture9 *)t, 0, &ds);
    else if (ty == D3DRTYPE_CUBETEXTURE) IDirect3DCubeTexture9_GetLevelDesc((IDirect3DCubeTexture9 *)t, 0, &ds);
    else { snprintf(out, n, "tur%u", (unsigned)ty); return; }
    const char *kind = ty == D3DRTYPE_CUBETEXTURE ? "kup" : "2d";
    char id[32];
    if (ds.Usage & D3DUSAGE_RENDERTARGET){ *is_rt = 1; snprintf(id, sizeof id, "rt%d", rt_id(t)); }
    else if (ds.Pool == D3DPOOL_DEFAULT) snprintf(id, sizeof id, "dinamik");
    else {
        uint32_t h = 0; int f = -1;
        for (int i = 0; i < g_nth; i++) if (g_th[i].p == t){ f = i; h = g_th[i].h; break; }
        if (f < 0 && g_tex_hash_on){
            D3DLOCKED_RECT lr; HRESULT r;
            if (ty == D3DRTYPE_TEXTURE) r = IDirect3DTexture9_LockRect((IDirect3DTexture9 *)t, 0, &lr, 0, D3DLOCK_READONLY);
            else r = IDirect3DCubeTexture9_LockRect((IDirect3DCubeTexture9 *)t, D3DCUBEMAP_FACE_POSITIVE_X, 0, &lr, 0, D3DLOCK_READONLY);
            if (SUCCEEDED(r)){ h = locked_hash(&lr, &ds);
                if (ty == D3DRTYPE_TEXTURE) IDirect3DTexture9_UnlockRect((IDirect3DTexture9 *)t, 0);
                else IDirect3DCubeTexture9_UnlockRect((IDirect3DCubeTexture9 *)t, D3DCUBEMAP_FACE_POSITIVE_X, 0); }
            if (g_nth < MAXTH){ g_th[g_nth].p = t; g_th[g_nth].h = h; g_nth++; }
        }
        snprintf(id, sizeof id, "%08x", h);
    }
    snprintf(out, n, "%s %ux%u f%u l%u %s", kind, ds.Width, ds.Height, (unsigned)ds.Format,
             (unsigned)IDirect3DBaseTexture9_GetLevelCount(t), id);
}
static void tex_print_live(DWORD st, void *tp){
    char b[96]; int rt; tex_desc(tp, b, sizeof b, &rt);
    if (rt && g_rt_dump_on) rt_note(tp);
    D("TEX %lu %s\n", (unsigned long)st, b);
}
/* bildirim (vertex declaration) kimligi */
#define MAXDECL 1024
static struct { void *p; uint32_t h; } g_decl[MAXDECL]; static int g_ndecl;
static uint32_t decl_get(void *p){
    if (!p) return 0;
    for (int i = 0; i < g_ndecl; i++) if (g_decl[i].p == p) return g_decl[i].h;
    return 0xFFFFFFFFu;
}
static void print_e(const SBE *e){
    switch (e->t){
    case E_RS:  D("RS %u %lu\n", e->a, (unsigned long)e->v); break;
    case E_TSS: D("TSS %u %u %lu\n", e->a, e->b, (unsigned long)e->v); break;
    case E_SS:  D("SS %u %u %lu\n", e->a, e->b, (unsigned long)e->v); break;
    case E_TEX: if (e->desc){ D("TEX %u %s\n", e->a, e->desc);
                    if (g_rt_dump_on && !strncmp(strrchr(e->desc, ' ') ? strrchr(e->desc, ' ') + 1 : "", "rt", 2)) rt_note(e->p); }
                else tex_print_live(e->a, e->p);
                break;
    case E_VS:  D("VS %08x\n", sh_get(e->p)); break;
    case E_PS:  D("PS %08x\n", sh_get(e->p)); break;
    case E_VSC: if (e->f){ D("VSC %u %u", e->a, e->n); dumpf(e->f, e->n); } break;
    case E_PSC: if (e->f){ D("PSC %u %u", e->a, e->n); dumpf(e->f, e->n); } break;
    case E_XF:  if (e->f){ D("XF %u", e->a); dumpf(e->f, 4); } break;
    case E_VP:  if (e->f) D("VP %lu %lu %lu %lu %.6g %.6g\n", (unsigned long)e->f[0], (unsigned long)e->f[1],
                            (unsigned long)e->f[2], (unsigned long)e->f[3], e->f[4], e->f[5]); break;
    case E_DECL: D("DECL %08x\n", decl_get(e->p)); break;
    case E_FVF: D("FVF %lx\n", (unsigned long)e->v); break;
    }
}
static void shadow_set(const SBE *e){
    switch (e->t){
    case E_RS:  if (e->a < 256) sh.rs[e->a] = e->v; break;
    case E_TSS: if (e->a < 8 && e->b < 33) sh.tss[e->a][e->b] = e->v; break;
    case E_SS:  if (e->a < 16 && e->b < 14) sh.ss[e->a][e->b] = e->v; break;
    case E_TEX: if (e->a < 16) sh.tex[e->a] = e->p; break;
    case E_VS:  sh.vs = e->p; break;
    case E_PS:  sh.ps = e->p; break;
    case E_VSC: if (e->f) for (UINT i = 0; i < e->n && e->a + i < 256; i++) memcpy(sh.vsc[e->a+i], e->f + 4*i, 16); break;
    case E_PSC: if (e->f) for (UINT i = 0; i < e->n && e->a + i < 32; i++) memcpy(sh.psc[e->a+i], e->f + 4*i, 16); break;
    case E_DECL: sh.decl = e->p; break;
    case E_FVF: sh.fvf = e->v; break;
    default: break;
    }
}
/* Capture: kayitli girdileri golgedeki SIMDIKI degerlerle tazele */
static void shadow_get(SBE *e){
    switch (e->t){
    case E_RS:  if (e->a < 256) e->v = sh.rs[e->a]; break;
    case E_TSS: if (e->a < 8 && e->b < 33) e->v = sh.tss[e->a][e->b]; break;
    case E_SS:  if (e->a < 16 && e->b < 14) e->v = sh.ss[e->a][e->b]; break;
    case E_TEX: if (e->a < 16){ e->p = sh.tex[e->a];
                    char b[96]; int rt; tex_desc(e->p, b, sizeof b, &rt); free(e->desc); e->desc = _strdup(b); } break;
    case E_VS:  e->p = sh.vs; break;
    case E_PS:  e->p = sh.ps; break;
    case E_VSC: if (e->f) for (UINT i = 0; i < e->n && e->a + i < 256; i++) memcpy(e->f + 4*i, sh.vsc[e->a+i], 16); break;
    case E_PSC: if (e->f) for (UINT i = 0; i < e->n && e->a + i < 32; i++) memcpy(e->f + 4*i, sh.psc[e->a+i], 16); break;
    case E_DECL: e->p = sh.decl; break;
    case E_FVF: e->v = sh.fvf; break;
    default: break;
    }
}
/* her Set* buradan gecer: kayittaysa bloga, degilse golgeye + dokume */
static void emit(SBE e, const float *src, int nf){
    if (g_in_sb){ e.f = (float *)src; e.desc = 0; shadow_set(&e); c_sb_inner++; return; }
    if (g_sbrec){
        if (g_sbcur.n >= g_sbcur.cap){
            int nc = g_sbcur.cap ? g_sbcur.cap*2 : 16;
            SBE *ne = realloc(g_sbcur.e, nc * sizeof(SBE)); if (!ne) return;
            g_sbcur.e = ne; g_sbcur.cap = nc;
        }
        e.f = 0; e.desc = 0;
        if (src && nf > 0){ e.f = malloc(nf * sizeof(float)); if (e.f) memcpy(e.f, src, nf * sizeof(float)); }
        if (e.t == E_TEX){ char b[96]; int rt; tex_desc(e.p, b, sizeof b, &rt); e.desc = _strdup(b); }
        g_sbcur.e[g_sbcur.n++] = e;
        return;
    }
    e.f = (float *)src; e.desc = 0;                /* dogrudan cagri: nesne canli */
    shadow_set(&e);
    if (DON) print_e(&e);
}
static SB *sb_find(void *obj){
    for (int i = 0; i < g_nsb; i++) if (g_sb[i].obj == obj) return &g_sb[i];
    return 0;
}

/* gölgelendirici bayt kodu: NFSRef_cikti/golge/ altina bir kez yazilir */
static int g_sh_dump_on = 1;
static void shader_save(const char *pre, uint32_t h, const DWORD *t){
    if (!g_sh_dump_on || !t) return;
    int n = 0; while (n < 65536 && t[n] != 0x0000FFFFu) n++;
    char p[MAX_PATH]; snprintf(p, sizeof p, "%s\\golge\\%s_%08x.bin", g_out, pre, h);
    if (GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES) return;
    FILE *o = fopen(p, "wb"); if (!o) return;
    fwrite(t, 4, (size_t)n + 1, o); fclose(o);
}
static HRESULT WINAPI h_CreateVertexShader(IDirect3DDevice9 *d, const DWORD *t, IDirect3DVertexShader9 **o){
    HRESULT r = O(CreateVertexShader, d)(d, t, o);
    if (SUCCEEDED(r) && o && *o && t){ uint32_t h = sh_hash(t); sh_put(*o, h); shader_save("vs", h, t); }
    return r;
}
static HRESULT WINAPI h_CreatePixelShader(IDirect3DDevice9 *d, const DWORD *t, IDirect3DPixelShader9 **o){
    HRESULT r = O(CreatePixelShader, d)(d, t, o);
    if (SUCCEEDED(r) && o && *o && t){ uint32_t h = sh_hash(t); sh_put(*o, h); shader_save("ps", h, t); }
    return r;
}
static HRESULT WINAPI h_CreateVertexDeclaration(IDirect3DDevice9 *d, const D3DVERTEXELEMENT9 *el, IDirect3DVertexDeclaration9 **o){
    HRESULT r = O(CreateVertexDeclaration, d)(d, el, o);
    if (SUCCEEDED(r) && o && *o && el){
        int n = 0; while (n < 64 && el[n].Stream != 0xFF) n++;
        uint32_t h = fnv(2166136261u, (const BYTE *)el, (size_t)(n + 1) * sizeof *el);
        if (g_ndecl < MAXDECL){ g_decl[g_ndecl].p = *o; g_decl[g_ndecl].h = h; g_ndecl++; }
        if (g_sh_dump_on){ char p[MAX_PATH]; snprintf(p, sizeof p, "%s\\golge\\decl_%08x.bin", g_out, h);
            if (GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES){ FILE *f = fopen(p, "wb");
                if (f){ fwrite(el, sizeof *el, (size_t)n + 1, f); fclose(f); } } }
    }
    return r;
}
static HRESULT WINAPI h_SetVertexDeclaration(IDirect3DDevice9 *d, IDirect3DVertexDeclaration9 *v){
    SBE e = { E_DECL, 0, 0, 0, 0, v, 0 }; emit(e, 0, 0); return O(SetVertexDeclaration, d)(d, v); }
static HRESULT WINAPI h_SetFVF(IDirect3DDevice9 *d, DWORD f){
    SBE e = { E_FVF, 0, 0, 0, f, 0, 0 }; emit(e, 0, 0); return O(SetFVF, d)(d, f); }
static HRESULT WINAPI h_SetStreamSource(IDirect3DDevice9 *d, UINT s, IDirect3DVertexBuffer9 *vb, UINT off, UINT stride){
    if (!g_sbrec) D("STRM %u %u %u\n", s, off, stride);
    return O(SetStreamSource, d)(d, s, vb, off, stride); }
static HRESULT WINAPI h_SetVertexShader(IDirect3DDevice9 *d, IDirect3DVertexShader9 *s){
    SBE e = { E_VS, 0, 0, 0, 0, s, 0 }; emit(e, 0, 0); return O(SetVertexShader, d)(d, s); }
static HRESULT WINAPI h_SetPixelShader(IDirect3DDevice9 *d, IDirect3DPixelShader9 *s){
    SBE e = { E_PS, 0, 0, 0, 0, s, 0 }; emit(e, 0, 0); return O(SetPixelShader, d)(d, s); }
static HRESULT WINAPI h_SetVertexShaderConstantF(IDirect3DDevice9 *d, UINT st, const float *v, UINT n){
    if (!g_sbrec && !g_cam_have && st == 0 && n >= 4 && v){ memcpy(g_cam, v, 64); g_cam_have = 1; }
    if (v && n < 1024){ SBE e = { E_VSC, (WORD)st, 0, (WORD)n, 0, 0, 0 }; emit(e, v, (int)n*4); }
    return O(SetVertexShaderConstantF, d)(d, st, v, n);
}
static HRESULT WINAPI h_SetPixelShaderConstantF(IDirect3DDevice9 *d, UINT st, const float *v, UINT n){
    if (v && n < 1024){ SBE e = { E_PSC, (WORD)st, 0, (WORD)n, 0, 0, 0 }; emit(e, v, (int)n*4); }
    return O(SetPixelShaderConstantF, d)(d, st, v, n);
}
static HRESULT WINAPI h_SetRenderState(IDirect3DDevice9 *d, D3DRENDERSTATETYPE s, DWORD v){
    SBE e = { E_RS, (WORD)s, 0, 0, v, 0, 0 }; emit(e, 0, 0); return O(SetRenderState, d)(d, s, v); }
static HRESULT WINAPI h_SetTextureStageState(IDirect3DDevice9 *d, DWORD st, D3DTEXTURESTAGESTATETYPE s, DWORD v){
    SBE e = { E_TSS, (WORD)st, (WORD)s, 0, v, 0, 0 }; emit(e, 0, 0); return O(SetTextureStageState, d)(d, st, s, v); }
static HRESULT WINAPI h_SetSamplerState(IDirect3DDevice9 *d, DWORD st, D3DSAMPLERSTATETYPE s, DWORD v){
    SBE e = { E_SS, (WORD)st, (WORD)s, 0, v, 0, 0 }; emit(e, 0, 0); return O(SetSamplerState, d)(d, st, s, v); }
static HRESULT WINAPI h_SetTexture(IDirect3DDevice9 *d, DWORD st, IDirect3DBaseTexture9 *t){
    SBE e = { E_TEX, (WORD)st, 0, 0, 0, t, 0 }; emit(e, 0, 0); return O(SetTexture, d)(d, st, t); }
static HRESULT WINAPI h_SetTransform(IDirect3DDevice9 *d, D3DTRANSFORMSTATETYPE s, const D3DMATRIX *m){
    if (m){ SBE e = { E_XF, (WORD)s, 0, 0, 0, 0, 0 }; emit(e, &m->_11, 16); }
    return O(SetTransform, d)(d, s, m);
}
static HRESULT WINAPI h_SetViewport(IDirect3DDevice9 *d, const D3DVIEWPORT9 *v){
    if (v){ float f[6] = { (float)v->X, (float)v->Y, (float)v->Width, (float)v->Height, v->MinZ, v->MaxZ };
            SBE e = { E_VP, 0, 0, 0, 0, 0, 0 }; emit(e, f, 6); }
    return O(SetViewport, d)(d, v);
}
static HRESULT WINAPI h_SetRenderTarget(IDirect3DDevice9 *d, DWORD i, IDirect3DSurface9 *s){
    if (DON && s){ D3DSURFACE_DESC ds; IDirect3DSurface9_GetDesc(s, &ds); D("RT %lu %ux%u f%u\n", (unsigned long)i, ds.Width, ds.Height, (unsigned)ds.Format); }
    else if (DON) D("RT %lu yok\n", (unsigned long)i);
    return O(SetRenderTarget, d)(d, i, s);
}
static HRESULT WINAPI h_Clear(IDirect3DDevice9 *d, DWORD n, const D3DRECT *r, DWORD f, D3DCOLOR c, float z, DWORD s){
    D("CLEAR %lu 0x%08lx %.6g %lu\n", (unsigned long)f, (unsigned long)c, z, (unsigned long)s); return O(Clear, d)(d, n, r, f, c, z, s); }
static HRESULT WINAPI h_StretchRect(IDirect3DDevice9 *d, IDirect3DSurface9 *a, const RECT *ra, IDirect3DSurface9 *b, const RECT *rb, D3DTEXTUREFILTERTYPE f){
    D("STRETCH %u\n", (unsigned)f); return O(StretchRect, d)(d, a, ra, b, rb, f); }

/* durum bloklari: IDirect3DStateBlock9 4 Capture, 5 Apply */
typedef HRESULT (WINAPI *SBFn_t)(IDirect3DStateBlock9 *);
enum { CI_SB_CAPTURE = 120, CI_SB_APPLY = 121 };
static void hook_sb_vt(void **vt);
static HRESULT WINAPI h_SBCapture(IDirect3DStateBlock9 *b){
    c_sb_capture++;
    SB *s = sb_find(b);
    if (s){ if (s->kind == 0) for (int i = 0; i < s->n; i++) shadow_get(&s->e[i]);
            else if (s->snap) memcpy(s->snap, &sh, sizeof sh); }
    D("SBCAPTURE %d\n", s ? (int)(s - g_sb) : -1);
    SBFn_t o = (SBFn_t)g_cur[CI_SB_CAPTURE];
    g_in_sb++; HRESULT r = o(b); g_in_sb--;
    return r;
}
static HRESULT WINAPI h_SBApply(IDirect3DStateBlock9 *b){
    c_sb_apply++;
    SB *s = sb_find(b);
    if (!s) D("SBAPPLY ? bilinmeyen\n");
    else if (s->kind == 0){
        D("SBAPPLY %d %d\n", (int)(s - g_sb), s->n);
        for (int i = 0; i < s->n; i++){ shadow_set(&s->e[i]); if (DON) print_e(&s->e[i]); }
        D("SBEND\n");
    } else {
        if (s->snap) memcpy(&sh, s->snap, sizeof sh);
        D("SBAPPLY %d tum-durum (CreateStateBlock)\n", (int)(s - g_sb));
    }
    SBFn_t o = (SBFn_t)g_cur[CI_SB_APPLY];
    g_in_sb++; HRESULT r = o(b); g_in_sb--;
    return r;
}
static SB *sb_new(void *obj){
    SB *s = sb_find(obj);
    if (s){ for (int i = 0; i < s->n; i++){ free(s->e[i].f); free(s->e[i].desc); } free(s->e); free(s->snap); memset(s, 0, sizeof *s); }
    else { if (g_nsb >= MAXSB) return 0; s = &g_sb[g_nsb++]; memset(s, 0, sizeof *s); }
    s->obj = obj;
    if (!IsBadReadPtr(obj, 4)) hook_sb_vt(*(void ***)obj);
    return s;
}
static HRESULT WINAPI h_BeginStateBlock(IDirect3DDevice9 *d){
    for (int i = 0; i < g_sbcur.n; i++){ free(g_sbcur.e[i].f); free(g_sbcur.e[i].desc); }
    g_sbcur.n = 0; g_sbrec = 1;
    return O(BeginStateBlock, d)(d);
}
static HRESULT WINAPI h_EndStateBlock(IDirect3DDevice9 *d, IDirect3DStateBlock9 **o){
    g_sbrec = 0;
    HRESULT r = O(EndStateBlock, d)(d, o);
    if (SUCCEEDED(r) && o && *o){
        SB *s = sb_new(*o);
        if (s){ s->kind = 0; s->n = g_sbcur.n; s->cap = g_sbcur.cap; s->e = g_sbcur.e;
                g_sbcur.e = 0; g_sbcur.n = g_sbcur.cap = 0; }
    }
    return r;
}
static HRESULT WINAPI h_CreateStateBlock(IDirect3DDevice9 *d, D3DSTATEBLOCKTYPE ty, IDirect3DStateBlock9 **o){
    HRESULT r = O(CreateStateBlock, d)(d, ty, o);
    if (SUCCEEDED(r) && o && *o){
        SB *s = sb_new(*o);
        if (s){ s->kind = 1; s->snap = malloc(sizeof sh); if (s->snap) memcpy(s->snap, &sh, sizeof sh); }
        L("CreateStateBlock tur %u -> blok %d (kare %ld)\n", (unsigned)ty, s ? (int)(s - g_sb) : -1, g_frame);
    }
    return r;
}
static HRESULT WINAPI h_DrawPrimitive(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, UINT a, UINT n){
    c_draw_all++; g_draws++; g_prims += n; D("DRAW %u %u\n", (unsigned)t, n); return O(DrawPrimitive, d)(d, t, a, n); }
static HRESULT WINAPI h_DrawIndexedPrimitive(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, INT b, UINT mi, UINT nv, UINT si, UINT n){
    c_draw_all++; g_draws++; g_prims += n; D("DIP %u %u %u\n", (unsigned)t, nv, n); return O(DrawIndexedPrimitive, d)(d, t, b, mi, nv, si, n); }
static HRESULT WINAPI h_DrawPrimitiveUP(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, UINT n, const void *v, UINT st){
    c_draw_all++; g_draws++; g_prims += n; D("DUP %u %u %u\n", (unsigned)t, n, st); return O(DrawPrimitiveUP, d)(d, t, n, v, st); }
static HRESULT WINAPI h_DrawIndexedPrimitiveUP(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, UINT mi, UINT nv, UINT n, const void *ix, D3DFORMAT f, const void *v, UINT st){
    c_draw_all++; g_draws++; g_prims += n; D("DIUP %u %u %u\n", (unsigned)t, nv, n); return O(DrawIndexedPrimitiveUP, d)(d, t, mi, nv, n, ix, f, v, st); }

/* yuzey -> BMP (RGB) [+ ayri alfa BMP]; desteklenmeyen bicim ham .bin */
static int write_bmp(const char *path, const BYTE *bits, int pitch, int w, int h, D3DFORMAT fmt, int alpha){
    int bpp = 0;
    if (fmt == D3DFMT_A8R8G8B8 || fmt == D3DFMT_X8R8G8B8) bpp = 4;
    else if (fmt == D3DFMT_R5G6B5 || fmt == D3DFMT_X1R5G5B5 || fmt == D3DFMT_A1R5G5B5) bpp = 2;
    if (!bpp) return 0;
    FILE *o = fopen(path, "wb"); if (!o) return 0;
    int rs = (w*3 + 3) & ~3;
    BITMAPFILEHEADER fh = {0}; BITMAPINFOHEADER ih = {0};
    fh.bfType = 0x4D42; fh.bfOffBits = sizeof fh + sizeof ih; fh.bfSize = fh.bfOffBits + rs*h;
    ih.biSize = sizeof ih; ih.biWidth = w; ih.biHeight = h; ih.biPlanes = 1; ih.biBitCount = 24;
    fwrite(&fh, sizeof fh, 1, o); fwrite(&ih, sizeof ih, 1, o);
    BYTE *row = calloc(1, rs);
    for (int y = h-1; y >= 0; y--){
        const BYTE *s = bits + y*pitch;
        for (int x = 0; x < w; x++){
            BYTE R, G, B;
            if (fmt == D3DFMT_R5G6B5){ WORD v = ((const WORD *)s)[x];
                R = (BYTE)(((v >> 11) & 31) * 255 / 31); G = (BYTE)(((v >> 5) & 63) * 255 / 63); B = (BYTE)((v & 31) * 255 / 31); }
            else if (bpp == 2){ WORD v = ((const WORD *)s)[x];
                R = (BYTE)(((v >> 10) & 31) * 255 / 31); G = (BYTE)(((v >> 5) & 31) * 255 / 31); B = (BYTE)((v & 31) * 255 / 31); }
            else if (alpha){ R = G = B = s[x*4+3]; }
            else { B = s[x*4]; G = s[x*4+1]; R = s[x*4+2]; }
            row[x*3] = B; row[x*3+1] = G; row[x*3+2] = R;
        }
        fwrite(row, rs, 1, o);
    }
    free(row); fclose(o);
    return 1;
}
static void dump_surface(IDirect3DDevice9 *d, IDirect3DSurface9 *s, const char *base){
    D3DSURFACE_DESC ds; IDirect3DSurface9 *sys = 0;
    if (FAILED(IDirect3DSurface9_GetDesc(s, &ds))) return;
    if (ds.MultiSampleType != D3DMULTISAMPLE_NONE){ L("  %s: cok ornekli, atlandi\n", base); return; }
    if (FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(d, ds.Width, ds.Height, ds.Format, D3DPOOL_SYSTEMMEM, &sys, 0))){ L("  %s: sistem yuzeyi yok (f%u)\n", base, (unsigned)ds.Format); return; }
    if (SUCCEEDED(IDirect3DDevice9_GetRenderTargetData(d, s, sys))){
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(IDirect3DSurface9_LockRect(sys, &lr, 0, D3DLOCK_READONLY))){
            char p[MAX_PATH]; snprintf(p, sizeof p, "%s.bmp", base);
            if (write_bmp(p, lr.pBits, lr.Pitch, (int)ds.Width, (int)ds.Height, ds.Format, 0)){
                if (ds.Format == D3DFMT_A8R8G8B8){ snprintf(p, sizeof p, "%s_alfa.bmp", base);
                    write_bmp(p, lr.pBits, lr.Pitch, (int)ds.Width, (int)ds.Height, ds.Format, 1); }
            } else {                                      /* ham */
                snprintf(p, sizeof p, "%s_f%u_%ux%u_pitch%d.bin", base, (unsigned)ds.Format, ds.Width, ds.Height, lr.Pitch);
                FILE *o = fopen(p, "wb"); if (o){ fwrite(lr.pBits, (size_t)lr.Pitch, ds.Height, o); fclose(o); }
            }
            IDirect3DSurface9_UnlockRect(sys);
        }
    } else L("  %s: GetRenderTargetData basarisiz (f%u)\n", base, (unsigned)ds.Format);
    IDirect3DSurface9_Release(sys);
}
static void rt_dump_all(IDirect3DDevice9 *d, long f){
    for (int i = 0; i < g_nrtd; i++){
        IDirect3DBaseTexture9 *t = g_rtd[i];
        D3DRESOURCETYPE ty = IDirect3DBaseTexture9_GetType(t);
        char base[MAX_PATH];
        if (ty == D3DRTYPE_TEXTURE){
            IDirect3DSurface9 *s = 0;
            if (SUCCEEDED(IDirect3DTexture9_GetSurfaceLevel((IDirect3DTexture9 *)t, 0, &s)) && s){
                snprintf(base, sizeof base, "%s\\rt_%05ld_rt%d", g_out, f, rt_id(t)); dump_surface(d, s, base); IDirect3DSurface9_Release(s); }
        } else if (ty == D3DRTYPE_CUBETEXTURE){
            for (int fc = 0; fc < 6; fc++){ IDirect3DSurface9 *s = 0;
                if (SUCCEEDED(IDirect3DCubeTexture9_GetCubeMapSurface((IDirect3DCubeTexture9 *)t, (D3DCUBEMAP_FACES)fc, 0, &s)) && s){
                    snprintf(base, sizeof base, "%s\\rt_%05ld_rt%d_kup%d", g_out, f, rt_id(t), fc); dump_surface(d, s, base); IDirect3DSurface9_Release(s); } }
        }
        IDirect3DBaseTexture9_Release(t);
    }
    if (g_nrtd) L("kare %ld: %d hedef doku kaydedildi\n", f, g_nrtd);
    g_nrtd = 0;
}

static void save_backbuffer(IDirect3DDevice9 *d, long f){
    IDirect3DSurface9 *bb = 0, *rt = 0, *sys = 0;
    D3DSURFACE_DESC ds;
    if (FAILED(IDirect3DDevice9_GetBackBuffer(d, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb){ L("HATA: GetBackBuffer\n"); return; }
    IDirect3DSurface9_GetDesc(bb, &ds);
    IDirect3DSurface9 *src = bb;
    if (ds.MultiSampleType != D3DMULTISAMPLE_NONE){
        if (SUCCEEDED(IDirect3DDevice9_CreateRenderTarget(d, ds.Width, ds.Height, ds.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, 0))
            && SUCCEEDED(IDirect3DDevice9_StretchRect(d, bb, 0, rt, 0, D3DTEXF_NONE))) src = rt;
        else { L("HATA: MSAA cozumu\n"); goto out; }
    }
    if (FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(d, ds.Width, ds.Height, ds.Format, D3DPOOL_SYSTEMMEM, &sys, 0))){ L("HATA: CreateOffscreenPlainSurface\n"); goto out; }
    if (FAILED(IDirect3DDevice9_GetRenderTargetData(d, src, sys))){ L("HATA: GetRenderTargetData\n"); goto out; }
    D3DLOCKED_RECT lr;
    if (FAILED(IDirect3DSurface9_LockRect(sys, &lr, 0, D3DLOCK_READONLY))){ L("HATA: LockRect\n"); goto out; }
    {
        char p[MAX_PATH]; snprintf(p, sizeof p, "%s\\kare_%05ld.bmp", g_out, f);
        FILE *o = fopen(p, "wb");
        if (o){
            int w = (int)ds.Width, h = (int)ds.Height, rs = (w*3 + 3) & ~3;
            BITMAPFILEHEADER fh = {0}; BITMAPINFOHEADER ih = {0};
            fh.bfType = 0x4D42; fh.bfOffBits = sizeof fh + sizeof ih; fh.bfSize = fh.bfOffBits + rs*h;
            ih.biSize = sizeof ih; ih.biWidth = w; ih.biHeight = h; ih.biPlanes = 1; ih.biBitCount = 24;
            fwrite(&fh, sizeof fh, 1, o); fwrite(&ih, sizeof ih, 1, o);
            BYTE *row = calloc(1, rs);
            for (int y = h-1; y >= 0; y--){
                BYTE *s = (BYTE *)lr.pBits + y*lr.Pitch;
                for (int x = 0; x < w; x++){
                    BYTE R, G, B;
                    if (ds.Format == D3DFMT_R5G6B5){ WORD v = ((WORD *)s)[x];
                        R = (BYTE)(((v >> 11) & 31) * 255 / 31); G = (BYTE)(((v >> 5) & 63) * 255 / 63); B = (BYTE)((v & 31) * 255 / 31); }
                    else if (ds.Format == D3DFMT_X1R5G5B5 || ds.Format == D3DFMT_A1R5G5B5){ WORD v = ((WORD *)s)[x];
                        R = (BYTE)(((v >> 10) & 31) * 255 / 31); G = (BYTE)(((v >> 5) & 31) * 255 / 31); B = (BYTE)((v & 31) * 255 / 31); }
                    else { B = s[x*4]; G = s[x*4+1]; R = s[x*4+2]; }
                    row[x*3] = B; row[x*3+1] = G; row[x*3+2] = R;
                }
                fwrite(row, rs, 1, o);
            }
            free(row); fclose(o);
            L("kare %ld kaydedildi %ux%u bicim %u\n", f, ds.Width, ds.Height, (unsigned)ds.Format);
        }
    }
    IDirect3DSurface9_UnlockRect(sys);
out:
    if (sys) IDirect3DSurface9_Release(sys);
    if (rt) IDirect3DSurface9_Release(rt);
    IDirect3DSurface9_Release(bb);
}

static double real_s(void){ return real_us() / 1e6; }

/* ---- kare sinirlari: TUM Present yollari buradan gecer ---------------- */
static volatile DWORD g_pres_tid;            /* ic ice Present'i bir kez say */

static void hook_device_vt(void **vt);
static void frame_pre(IDirect3DDevice9 *d, const char *yol, void *ra){
    long f = g_frame + 1;                     /* bu Present ile tamamlanan kare */
    g_main_tid = GetCurrentThreadId();
    if (!g_started){ g_extra_us = (LONGLONG)real_us(); g_last_pres_us = (LONGLONG)real_us(); g_started = 1;
                     L("ilk Present: gercek %.3f sn, deterministik saat basladi\n", real_s()); }
    { uint64_t st = stall_us();                 /* durgunlugu kalici yap (saat geri gitmesin) */
      if (st){ g_extra_us += (LONGLONG)st; g_last_pres_us = (LONGLONG)real_us(); } }
    if (d && !IsBadReadPtr(d, 4)) hook_device_vt(*(void ***)d);   /* tablo girdileri degisti mi? */
    if (f <= 5 || f == 50 || f == 500){ char m[MAX_PATH + 32]; modof((DWORD)ra, m, sizeof m);
        L("Present kare %ld yol %s cagiran %s thread %lu\n", f, yol, m, (unsigned long)GetCurrentThreadId()); }
    if (d && in_list(cap_f, cap_n, f)) save_backbuffer(d, f);
    if (g_kare){
        fprintf(g_kare, "%ld,%ld,%ld,%ld,%lld", f, g_draws, g_prims, g_polls, (long long)g_extra_us);
        for (int i = 0; i < 16; i++) fprintf(g_kare, ",%.7g", g_cam_have ? g_cam[i] : 0.0f);
        fprintf(g_kare, "\n");
        if ((f % 300) == 0) fflush(g_kare);
    }
    if (g_polls > g_polls_max) g_polls_max = g_polls;
    if (g_polls > VALVE_AT) g_valve_frames++;
    if (g_dump_on && g_dump){ fprintf(g_dump, "PRESENT %ld\n", f); fclose(g_dump); g_dump = 0; g_dump_on = 0;
        if (d) rt_dump_all(d, f); }
}
static void frame_post(void){
    /* GERCEK 60 fps'e sabitle (port'un PACE60'i).  Sanal saat kare basina
     * 16.667 ms ilerledigi icin bu olmadan hizli bir PC'de oyun zamani
     * gercek zamandan kat kat hizli akiyor. */
    if (g_pace){
        static uint64_t next = 0;
        if (!next) timeBeginPeriod(1);            /* Sleep(1) gercekten ~1 ms olsun */
        uint64_t now = real_us();
        if (!next || now > next + 100000ull) next = now;
        next += 16667ull;
        while ((now = real_us()) < next){ if (next - now > 2000ull) Sleep(1); }
    }
    g_last_pres_us = (LONGLONG)real_us();
    /* sonraki kare */
    InterlockedIncrement(&g_frame);
    g_polls = 0; g_draws = 0; g_prims = 0; g_cam_have = 0;
    seq_update(g_frame + 1);
    if (in_list(dump_f, dump_n, g_frame + 1)){
        char p[MAX_PATH]; snprintf(p, sizeof p, "%s\\cagri_%05ld.txt", g_out, g_frame + 1);
        g_dump = fopen(p, "w"); g_dump_on = g_dump != 0;
    }
    if (g_quit && g_frame >= g_quit){
        L("BITTI: %ld kare, gercek %.1f sn, en cok sorgu/kare %ld, valf %ld kare, ek %lld us\n",
          g_frame, real_s(), g_polls_max, g_valve_frames, (long long)g_extra_us);
        if (g_kare) fclose(g_kare), g_kare = 0;
        EnterCriticalSection(&g_slk);
        if (g_slog) fclose(g_slog), g_slog = 0;
        if (g_sdat) fclose(g_sdat), g_sdat = 0;
        LeaveCriticalSection(&g_slk);
        if (g_log) fclose(g_log), g_log = 0;
        TerminateProcess(GetCurrentProcess(), 0);
    }
}
#define PRES_WRAP(cnt, yol, dev, call) do { \
        DWORD me = GetCurrentThreadId(); \
        if (g_pres_tid == me) return call;          /* ic ice: zaten sayiliyor */ \
        cnt++; g_pres_tid = me; \
        frame_pre(dev, yol, __builtin_return_address(0)); \
        HRESULT hr_ = call; \
        frame_post(); g_pres_tid = 0; \
        return hr_; } while (0)

static HRESULT WINAPI h_Present(IDirect3DDevice9 *d, const RECT *a, const RECT *b, HWND w, const RGNDATA *r){
    PRES_WRAP(c_pres_vt, "cihaz", d, O(Present, d)(d, a, b, w, r)); }
/* IDirect3DSwapChain9: 3 Present(src, dst, hwnd, dirty, flags) */
typedef HRESULT (WINAPI *SCPresent_t)(void *, const RECT *, const RECT *, HWND, const RGNDATA *, DWORD);
static SCPresent_t o_SCPresent;
static HRESULT WINAPI h_SCPresent(void *sc, const RECT *a, const RECT *b, HWND w, const RGNDATA *r, DWORD fl){
    PRES_WRAP(c_pres_sc, "swapchain", g_dev, o_SCPresent(sc, a, b, w, r, fl)); }
static void sc_hook(void *sc, const char *ne){
    if (!sc) return;
    void **vt = *(void ***)sc;
    if (vt[3] != (void *)h_SCPresent){ vt_patch(vt, 3, (void *)h_SCPresent, (void **)&o_SCPresent);
        L("swapchain %s %p vtable %p kancalandi\n", ne, sc, vt); }
}
static HRESULT WINAPI h_GetSwapChain(IDirect3DDevice9 *d, UINT i, void **o){
    HRESULT r = O(GetSwapChain, d)(d, i, o); if (SUCCEEDED(r) && o) sc_hook(*o, "GetSwapChain"); return r; }
static HRESULT WINAPI h_CreateAdditionalSwapChain(IDirect3DDevice9 *d, D3DPRESENT_PARAMETERS *pp, void **o){
    HRESULT r = O(CreateAdditionalSwapChain, d)(d, pp, o); if (SUCCEEDED(r) && o) sc_hook(*o, "CreateAdditionalSwapChain"); return r; }
static HRESULT WINAPI h_BeginScene(IDirect3DDevice9 *d){ c_begin++; return O(BeginScene, d)(d); }
static HRESULT WINAPI h_EndScene(IDirect3DDevice9 *d){ c_end++; return O(EndScene, d)(d); }

/* Windows sistem DLL'leri "hotpatch" girisiyle derlenir: fonksiyondan once
 * 5 bayt bosluk, ilk komut 2 baytlik "mov edi,edi".  Oraya atlama yazinca
 * fonksiyon HANGI yoldan cagrilirsa cagrilsin (vtable, kopya vtable, onbellege
 * alinmis isaretci) kancamizdan gecer. */
static int hotpatch(void *fn, void *hook, void **tramp){
    BYTE *p = fn;
    if (IsBadReadPtr(p - 5, 7)) return 0;
    int pad = 1; for (int i = 1; i <= 5; i++) if (p[-i] != 0x90 && p[-i] != 0xCC) pad = 0;
    if (!pad || p[0] != 0x8B || p[1] != 0xFF) return 0;
    DWORD op;
    if (!VirtualProtect(p - 5, 7, PAGE_EXECUTE_READWRITE, &op)) return 0;
    p[-5] = 0xE9; *(DWORD *)(p - 4) = (DWORD)((BYTE *)hook - p);   /* jmp hook */
    *tramp = p + 2;
    *(volatile WORD *)p = 0xF9EB;                                      /* jmp short -7 */
    VirtualProtect(p - 5, 7, op, &op);
    FlushInstructionCache(GetCurrentProcess(), p - 5, 7);
    return 1;
}
static void bytes_of(void *fn, char *out, int n){
    BYTE *p = fn; out[0] = 0;
    if (!p || IsBadReadPtr(p, 8)){ snprintf(out, n, "(okunamadi)"); return; }
    snprintf(out, n, "%02x %02x %02x %02x %02x %02x %02x %02x", p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
}

static HRESULT WINAPI h_Reset(IDirect3DDevice9 *d, D3DPRESENT_PARAMETERS *pp){
    L("Reset kare %ld\n", g_frame);
    return O(Reset, d)(d, pp);
}

/* ---- D3D cihaz kancalari: koprucuk + hotpatch ------------------------
 * Windows'ta OLCULDU: cihaz tablosu d3d9.dll'de degil yigin bellekte ve
 * ilk kareden sonra girdileri d3d9 tarafindan yeniden yaziliyor; tabloya
 * takilan kancalar siliniyordu.  Artik her yuvanin HEDEF FONKSIYONU kancalanir
 * (hotpatch girisi).  Her (yuva, hedef) icin 16 baytlik bir koprucuk:
 *     mov dword [g_cur+4*yuva], hedef+2 ; jmp C_kancasi
 * C kancasi asili g_cur'dan cagirir.  Tablo her Present'te taranir; yeni bir
 * hedef gorulurse o da kancalanir ve log'a yazilir. */
static BYTE *g_stubmem; static int g_stubn;
#define STUBMAX 2048
static struct { int slot; void *tgt; void *stub; } g_sk[STUBMAX]; static int g_nsk;
static void *make_stub(int slot, void *orig, void *hook){
    if (!g_stubmem) g_stubmem = VirtualAlloc(0, STUBMAX*16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_stubmem || g_stubn >= STUBMAX) return 0;
    BYTE *st = g_stubmem + 16*g_stubn++;
    st[0] = 0xC7; st[1] = 0x05; *(DWORD *)(st+2) = (DWORD)&g_cur[slot]; *(DWORD *)(st+6) = (DWORD)orig;
    st[10] = 0xE9; *(DWORD *)(st+11) = (DWORD)((BYTE *)hook - (st+15));
    st[15] = 0xCC;
    FlushInstructionCache(GetCurrentProcess(), st, 16);
    return st;
}
static int is_stub(void *p){ return g_stubmem && (BYTE *)p >= g_stubmem && (BYTE *)p < g_stubmem + STUBMAX*16; }
static int hp_ok(void *fn){
    BYTE *p = fn;
    if (IsBadReadPtr(p - 5, 7)) return 0;
    for (int i = 1; i <= 5; i++) if (p[-i] != 0x90 && p[-i] != 0xCC) return 0;
    return p[0] == 0x8B && p[1] == 0xFF;
}
static LONG c_tgt_new, c_vt_rewrite;
static CRITICAL_SECTION g_hk_lk;
static void hook_slot2(void **vt, int vslot, int slot, void *hook, const char *nm){
    void *t = vt[vslot];
    if (!t || is_stub(t)) return;
    for (int i = 0; i < g_nsk; i++) if (g_sk[i].slot == slot && g_sk[i].tgt == t){
        if (g_sk[i].stub){ vt_patch(vt, vslot, g_sk[i].stub, 0); c_vt_rewrite++; }   /* vtable yedegi: tekrar tak */
        return; }                                       /* hotpatch'li hedef: cagri zaten bize gelir */
    if (g_nsk >= STUBMAX) return;
    char m[MAX_PATH + 32], bb[64]; modof((DWORD)t, m, sizeof m); bytes_of(t, bb, sizeof bb);
    for (int i = 0; i < g_nsk; i++) if (g_sk[i].tgt == t && !g_sk[i].stub){   /* baska yuva icin hotpatch'li */
        static int warned; if (warned++ < 20) L("UYARI: %s yuva %d hedef %p [%s] baska yuvayla PAYLASILIYOR, atlandi\n", nm, slot, t, m);
        return; }
    void *dummy;
    if (g_hp_on && hp_ok(t)){
        void *st = make_stub(slot, (BYTE *)t + 2, hook);
        if (st && hotpatch(t, st, &dummy)){
            g_sk[g_nsk].slot = slot; g_sk[g_nsk].tgt = t; g_sk[g_nsk].stub = 0; g_nsk++; c_tgt_new++;
            if (c_tgt_new <= 300) L("kanca %-24s yuva %3d hedef %p [%s] hotpatch (kare %ld)\n", nm, slot, t, m, g_frame);
            return;
        }
    }
    void *st = make_stub(slot, t, hook);
    if (!st) return;
    g_sk[g_nsk].slot = slot; g_sk[g_nsk].tgt = t; g_sk[g_nsk].stub = st; g_nsk++; c_tgt_new++;
    vt_patch(vt, vslot, st, 0);
    if (c_tgt_new <= 300) L("kanca %-24s yuva %3d hedef %p [%s] VTABLE (baytlar %s, kare %ld)\n", nm, slot, t, m, bb, g_frame);
}
static void hook_slot(void **vt, int slot, void *hook, const char *nm){ hook_slot2(vt, slot, slot, hook, nm); }
static void hook_sb_vt(void **vt){
    if (!vt || IsBadReadPtr(vt, 6*4)) return;
    EnterCriticalSection(&g_hk_lk);
    hook_slot2(vt, 4, CI_SB_CAPTURE, (void *)h_SBCapture, "SB.Capture");
    hook_slot2(vt, 5, CI_SB_APPLY,   (void *)h_SBApply,   "SB.Apply");
    LeaveCriticalSection(&g_hk_lk);
}
#define HK(idx, name) hook_slot(vt, idx, (void *)h_##name, #name)
static void hook_device_vt(void **vt){
    if (!vt || IsBadReadPtr(vt, 120*4)) return;
    EnterCriticalSection(&g_hk_lk);
    HK(17, Present);
    HK(13, CreateAdditionalSwapChain); HK(14, GetSwapChain); HK(41, BeginScene); HK(42, EndScene);
    HK(16, Reset); HK(34, StretchRect); HK(37, SetRenderTarget);
    HK(43, Clear); HK(44, SetTransform); HK(47, SetViewport); HK(57, SetRenderState);
    HK(60, BeginStateBlock); HK(61, EndStateBlock); HK(65, SetTexture);
    HK(67, SetTextureStageState); HK(69, SetSamplerState);
    HK(81, DrawPrimitive); HK(82, DrawIndexedPrimitive); HK(83, DrawPrimitiveUP);
    HK(84, DrawIndexedPrimitiveUP); HK(91, CreateVertexShader); HK(92, SetVertexShader);
    HK(94, SetVertexShaderConstantF); HK(106, CreatePixelShader); HK(107, SetPixelShader);
    HK(109, SetPixelShaderConstantF);
    HK(59, CreateStateBlock); HK(86, CreateVertexDeclaration); HK(87, SetVertexDeclaration);
    HK(89, SetFVF); HK(100, SetStreamSource);
    LeaveCriticalSection(&g_hk_lk);
}
static HRESULT WINAPI h_D3DCreateDevice(IDirect3D9 *self, UINT ad, D3DDEVTYPE ty, HWND w, DWORD bf, D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **out){
    unsigned short cw0, cw1;
    __asm__ volatile("fnstcw %0" : "=m"(cw0));
    if (pp) L("CreateDevice: %ux%u bicim %u pencereli %d msaa %u bayrak 0x%lx vsync 0x%x\n",
              pp->BackBufferWidth, pp->BackBufferHeight, (unsigned)pp->BackBufferFormat, pp->Windowed,
              (unsigned)pp->MultiSampleType, (unsigned long)bf, pp->PresentationInterval);
    HRESULT r = o_D3DCreateDevice(self, ad, ty, w, bf, pp, out);
    __asm__ volatile("fnstcw %0" : "=m"(cw1));
    L("CreateDevice -> 0x%08lx  x87 kontrol sozcugu 0x%04x -> 0x%04x\n", (unsigned long)r, cw0, cw1);
    if (SUCCEEDED(r) && out && *out){
        g_dev = *out; g_main_tid = GetCurrentThreadId();
        void **vt = *(void ***)*out;
        g_dev_vt = vt;
        { char bb[64], m[MAX_PATH + 32]; bytes_of(vt[17], bb, sizeof bb); modof((DWORD)vt[17], m, sizeof m);
          L("cihaz vtable %p, Present = %p [%s] baytlar %s\n", vt, vt[17], m, bb); }
        hook_device_vt(vt);
    }
    return r;
}
static IDirect3D9 *WINAPI h_D3DCreate9(UINT v){ ILK("Direct3DCreate9");
    IDirect3D9 *d = o_D3DCreate9(v);
    L("Direct3DCreate9(%u) -> %p\n", v, d);
    if (d){ void **vt = *(void ***)d; vt_patch(vt, 16, (void *)h_D3DCreateDevice, (void **)&o_D3DCreateDevice); }
    return d;
}

/* ---- DirectSound kaydi -------------------------------------------------
 * Oyunun sese yazdigi her sey: tampon tanimlari (bicim, boyut, bayrak),
 * Play/Stop/SetCurrentPosition/Volume/Pan/Frequency/Release ve Unlock ile
 * tampona yazilan veri.  Ses birden fazla thread'den cagrildigi icin asil
 * fonksiyon NESNENIN vtable'ina gore bulunur (koprucuk kullanilmaz).
 * IDirectSound: 3 CreateSoundBuffer, 5 DuplicateSoundBuffer
 * IDirectSoundBuffer: 2 Release, 4 GetCurrentPosition, 11 Lock, 12 Play,
 *   13 SetCurrentPosition, 15 SetVolume, 16 SetPan, 17 SetFrequency,
 *   18 Stop, 19 Unlock */
typedef HRESULT (WINAPI *DSCreate_t)(const GUID *, void **, void *);
static DSCreate_t o_DSCreate;
static int g_ses_on = 1;
static long long g_sdat_pos, g_ses_cap = 200ll << 20;
static LONG c_unlock_bytes;
#define MAXAVT 16
static struct { void **vt; void *o[24]; } g_avt[MAXAVT]; static int g_navt;
static void *avt_orig(void *self, int idx){
    void **vt = *(void ***)self;
    for (int i = 0; i < g_navt; i++) if (g_avt[i].vt == vt) return g_avt[i].o[idx];
    return 0;
}
static void avt_hook(void **vt, int idx, void *hook){
    int k = -1;
    for (int i = 0; i < g_navt; i++) if (g_avt[i].vt == vt) k = i;
    if (k < 0){ if (g_navt >= MAXAVT) return; k = g_navt++; g_avt[k].vt = vt;
                for (int i = 0; i < 24; i++) g_avt[k].o[i] = 0; }
    if (vt[idx] == hook) return;
    if (!g_avt[k].o[idx]) g_avt[k].o[idx] = vt[idx];
    vt_patch(vt, idx, hook, 0);
}
#define MAXBUF 4096
static struct { void *p; int id; DWORD lockoff; } g_buf[MAXBUF]; static int g_nbuf;
static int buf_id(void *p){
    for (int i = g_nbuf - 1; i >= 0; i--) if (g_buf[i].p == p) return g_buf[i].id;
    return -1;
}
static int buf_new(void *p){
    EnterCriticalSection(&g_slk);
    int id = g_nbuf;
    for (int i = 0; i < g_nbuf; i++) if (g_buf[i].p == p){ g_buf[i].p = 0; }   /* yeniden kullanilan adres */
    if (g_nbuf < MAXBUF){ g_buf[g_nbuf].p = p; g_buf[g_nbuf].id = id; g_buf[g_nbuf].lockoff = 0; g_nbuf++; }
    LeaveCriticalSection(&g_slk);
    return id;
}
static void S(const char *fmt, ...){
    if (!g_slog) return;
    va_list ap; va_start(ap, fmt);
    EnterCriticalSection(&g_slk);
    fprintf(g_slog, "%ld %.3f ", g_frame, real_us() / 1000.0);
    vfprintf(g_slog, fmt, ap);
    LeaveCriticalSection(&g_slk);
    va_end(ap);
}
typedef HRESULT (WINAPI *BLock_t)(void *, DWORD, DWORD, void **, DWORD *, void **, DWORD *, DWORD);
typedef HRESULT (WINAPI *BUnlock_t)(void *, void *, DWORD, void *, DWORD);
typedef HRESULT (WINAPI *BPlay_t)(void *, DWORD, DWORD, DWORD);
typedef HRESULT (WINAPI *BDword_t)(void *, DWORD);
typedef HRESULT (WINAPI *BLong_t)(void *, LONG);
typedef HRESULT (WINAPI *BVoid_t)(void *);
typedef ULONG   (WINAPI *BRel_t)(void *);
typedef HRESULT (WINAPI *DSCreateBuf_t)(void *, const void *, void **, void *);
typedef HRESULT (WINAPI *DSDup_t)(void *, void *, void **);
static void buf_hook(void *b);
static HRESULT WINAPI h_BLock(void *b, DWORD off, DWORD n, void **p1, DWORD *n1, void **p2, DWORD *n2, DWORD fl){
    HRESULT r = ((BLock_t)avt_orig(b, 11))(b, off, n, p1, n1, p2, n2, fl);
    int id = buf_id(b);
    if (SUCCEEDED(r)){
        EnterCriticalSection(&g_slk);
        for (int i = g_nbuf - 1; i >= 0; i--) if (g_buf[i].p == b){ g_buf[i].lockoff = off; break; }
        LeaveCriticalSection(&g_slk);
    }
    S("LOCK %d off %lu n %lu bayrak 0x%lx -> %lu+%lu  r=0x%08lx\n", id, (unsigned long)off, (unsigned long)n, (unsigned long)fl,
      (unsigned long)(n1 ? *n1 : 0), (unsigned long)(n2 ? *n2 : 0), (unsigned long)r);
    return r;
}
static HRESULT WINAPI h_BUnlock(void *b, void *p1, DWORD n1, void *p2, DWORD n2){
    int id = buf_id(b); DWORD off = 0;
    EnterCriticalSection(&g_slk);
    for (int i = g_nbuf - 1; i >= 0; i--) if (g_buf[i].p == b){ off = g_buf[i].lockoff; break; }
    long long pos = g_sdat_pos;
    int yaz = g_sdat && g_sdat_pos + n1 + n2 <= g_ses_cap;
    if (yaz){
        if (p1 && n1) fwrite(p1, 1, n1, g_sdat);
        if (p2 && n2) fwrite(p2, 1, n2, g_sdat);
        g_sdat_pos += (p1 ? n1 : 0) + (p2 ? n2 : 0);
    }
    LeaveCriticalSection(&g_slk);
    c_unlock_bytes += (LONG)(n1 + n2);
    S("UNLOCK %d off %lu n1 %lu n2 %lu veri %lld%s\n", id, (unsigned long)off, (unsigned long)n1, (unsigned long)n2,
      yaz ? pos : -1ll, yaz ? "" : " (sinir/yok)");
    return ((BUnlock_t)avt_orig(b, 19))(b, p1, n1, p2, n2);
}
static HRESULT WINAPI h_BPlay(void *b, DWORD r1, DWORD pr, DWORD fl){
    S("PLAY %d oncelik %lu bayrak 0x%lx\n", buf_id(b), (unsigned long)pr, (unsigned long)fl);
    return ((BPlay_t)avt_orig(b, 12))(b, r1, pr, fl); }
static HRESULT WINAPI h_BStop(void *b){ S("STOP %d\n", buf_id(b)); return ((BVoid_t)avt_orig(b, 18))(b); }
static HRESULT WINAPI h_BSetPos(void *b, DWORD p){ S("SETPOS %d %lu\n", buf_id(b), (unsigned long)p); return ((BDword_t)avt_orig(b, 13))(b, p); }
static HRESULT WINAPI h_BSetVol(void *b, LONG v){ S("VOL %d %ld\n", buf_id(b), (long)v); return ((BLong_t)avt_orig(b, 15))(b, v); }
static HRESULT WINAPI h_BSetPan(void *b, LONG v){ S("PAN %d %ld\n", buf_id(b), (long)v); return ((BLong_t)avt_orig(b, 16))(b, v); }
static HRESULT WINAPI h_BSetFreq(void *b, DWORD v){ S("FREQ %d %lu\n", buf_id(b), (unsigned long)v); return ((BDword_t)avt_orig(b, 17))(b, v); }
static ULONG WINAPI h_BRelease(void *b){
    int id = buf_id(b);
    BRel_t o = (BRel_t)avt_orig(b, 2);
    ULONG rc = o(b);
    S("RELEASE %d -> %lu\n", id, (unsigned long)rc);
    return rc;
}
static void buf_hook(void *b){
    if (!b || IsBadReadPtr(b, 4)) return;
    void **vt = *(void ***)b;
    if (IsBadReadPtr(vt, 21*4)) return;
    EnterCriticalSection(&g_slk);
    avt_hook(vt, 2, (void *)h_BRelease);  avt_hook(vt, 11, (void *)h_BLock);   avt_hook(vt, 12, (void *)h_BPlay);
    avt_hook(vt, 13, (void *)h_BSetPos);  avt_hook(vt, 15, (void *)h_BSetVol); avt_hook(vt, 16, (void *)h_BSetPan);
    avt_hook(vt, 17, (void *)h_BSetFreq); avt_hook(vt, 18, (void *)h_BStop);   avt_hook(vt, 19, (void *)h_BUnlock);
    LeaveCriticalSection(&g_slk);
}
static HRESULT WINAPI h_DSCreateBuf(void *ds, const void *desc_, void **out, void *u){
    HRESULT r = ((DSCreateBuf_t)avt_orig(ds, 3))(ds, desc_, out, u);
    const BYTE *desc = desc_;
    if (SUCCEEDED(r) && out && *out){
        int id = buf_new(*out);
        DWORD fl = desc ? *(const DWORD *)(desc+4) : 0, by = desc ? *(const DWORD *)(desc+8) : 0;
        const BYTE *wf = desc ? *(const BYTE * const *)(desc+16) : 0;
        if (wf && !IsBadReadPtr(wf, 16))
            S("CREATE %d bayrak 0x%lx boyut %lu bicim tag %u kanal %u hz %lu bit %u blok %u\n", id, (unsigned long)fl, (unsigned long)by,
              *(const WORD *)wf, *(const WORD *)(wf+2), (unsigned long)*(const DWORD *)(wf+4), *(const WORD *)(wf+14), *(const WORD *)(wf+12));
        else S("CREATE %d bayrak 0x%lx boyut %lu (bicim yok)\n", id, (unsigned long)fl, (unsigned long)by);
        buf_hook(*out);
    } else S("CREATE basarisiz r=0x%08lx\n", (unsigned long)r);
    return r;
}
static HRESULT WINAPI h_DSDup(void *ds, void *src, void **out){
    HRESULT r = ((DSDup_t)avt_orig(ds, 5))(ds, src, out);
    if (SUCCEEDED(r) && out && *out){ int id = buf_new(*out); S("DUP %d -> %d\n", buf_id(src), id); buf_hook(*out); }
    return r;
}
static HRESULT WINAPI h_DSCreate(const GUID *g, void **out, void *u){
    HRESULT r = o_DSCreate(g, out, u);
    L("ses: DirectSoundCreate -> 0x%08lx\n", (unsigned long)r);
    if (SUCCEEDED(r) && out && *out && !IsBadReadPtr(*out, 4)){
        void **vt = *(void ***)*out;
        EnterCriticalSection(&g_slk);
        avt_hook(vt, 3, (void *)h_DSCreateBuf); avt_hook(vt, 5, (void *)h_DSDup);
        LeaveCriticalSection(&g_slk);
    }
    return r;
}

/* ---- baslangic ------------------------------------------------------ */
static uint32_t crc32_file(const char *p, long *sz){
    static uint32_t T[256];
    for (uint32_t i = 0; i < 256; i++){ uint32_t c = i; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & -(c & 1)); T[i] = c; }
    FILE *f = fopen(p, "rb"); if (!f) return 0;
    uint32_t c = 0xFFFFFFFFu; static BYTE buf[65536]; size_t n; *sz = 0;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0){ *sz += (long)n; for (size_t i = 0; i < n; i++) c = T[(c ^ buf[i]) & 0xFF] ^ (c >> 8); }
    fclose(f);
    return c ^ 0xFFFFFFFFu;
}


/* ---- cokme kaydi: adres, modul, thread, yazmaclar, yigin ------------ */
static void modof(DWORD a, char *out, int n){
    HMODULE h = 0; char p[MAX_PATH];
    if (a > 0x10000 && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &h) && h){
        GetModuleFileNameA(h, p, MAX_PATH); char *b = strrchr(p, '\\');
        snprintf(out, n, "%s+0x%lx", b ? b+1 : p, (unsigned long)(a - (DWORD)h));
    } else snprintf(out, n, "?");
}
static LONG CALLBACK veh(EXCEPTION_POINTERS *e){
    DWORD code = e->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_STACK_OVERFLOW &&
        code != EXCEPTION_PRIV_INSTRUCTION) return EXCEPTION_CONTINUE_SEARCH;
    static volatile LONG n; if (InterlockedIncrement(&n) > 8) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT *c = e->ContextRecord; char m[MAX_PATH + 32];
    modof((DWORD)e->ExceptionRecord->ExceptionAddress, m, sizeof m);
    L("ISTISNA kod %08lx adres %p [%s] thread %lu kare %ld\n", (unsigned long)code,
      e->ExceptionRecord->ExceptionAddress, m, (unsigned long)GetCurrentThreadId(), g_frame);
    if (code == EXCEPTION_ACCESS_VIOLATION)
        L("  %s erisimi, adres 0x%08lx\n", e->ExceptionRecord->ExceptionInformation[0] ? "yazma" : "okuma",
          (unsigned long)e->ExceptionRecord->ExceptionInformation[1]);
    L("  eax %08lx ebx %08lx ecx %08lx edx %08lx esi %08lx edi %08lx ebp %08lx esp %08lx\n",
      c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
    for (int i = 0; i < 64; i++){
        DWORD *sp = (DWORD *)c->Esp + i;
        if (IsBadReadPtr(sp, 4)) break;
        modof(*sp, m, sizeof m);
        if (m[0] != '?') L("  [esp+%03x] %08lx %s\n", i*4, *sp, m);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* ---- durum satiri: saniyede bir, ne oldugunu OLCEREK gosterir ---------- */
static double g_limit_s = 420;
static DWORD WINAPI durum_thread(LPVOID u){
    (void)u;
    LONG lf = 0, lpv = 0, lps = 0, lb = 0, le = 0, ld = 0, lqm = 0, lqo = 0, lg = 0, lev = 0;
    for (;;){
        Sleep(1000);
        char vs[MAX_PATH + 64] = "-";
        if (g_dev && !IsBadReadPtr(g_dev, 4)){
            void **cur = *(void ***)g_dev;
            if (cur != g_dev_vt){
                L("UYARI: cihaz vtable degisti %p -> %p, yeniden kancalaniyor\n", g_dev_vt, cur);
                g_dev_vt = cur; hook_device_vt(cur);
            }
            char m[MAX_PATH + 32]; modof((DWORD)cur[17], m, sizeof m);
            snprintf(vs, sizeof vs, "%s (%s)", m, is_stub(cur[17]) ? "koprucuk" : "hedef");
        }
        { static DWORD lastdev; DWORD gd = 0;
          if (!IsBadReadPtr((void *)0x870974, 4)) gd = *(DWORD *)0x870974;
          if (gd != lastdev){ lastdev = gd;
              L("oyunun cihaz isaretcisi [0x870974] = %08lx (CreateDevice %p)%s\n", (unsigned long)gd, g_dev,
                (gd && gd != (DWORD)g_dev) ? "  <-- FARKLI NESNE" : ""); } }
        long f = g_frame;
        double sanal = (1000000.0 + f * 16667.0 + (double)g_extra_us + (double)stall_us()) / 1e6;
        L("durum %.0f sn | kare %ld (+%ld) sanal %.1f sn | Present %ld sc %ld | yeni hedef %ld tablo yeniden %ld | Begin %ld End %ld cizim %ld | saat ana %ld diger %ld | klavye okuma %ld olay %ld | Present -> %s\n",
          real_s(), f, f - lf, sanal, c_pres_vt - lpv, c_pres_sc - lps, c_tgt_new, c_vt_rewrite, c_begin - lb, c_end - le,
          c_draw_all - ld, g_qpc_main - lqm, g_qpc_other - lqo, g_gdd_kbd - lg, g_ev_out - lev, vs);
        { static LONG la, lc, lu; L("       durum blogu Apply %ld Capture %ld ic cagri %ld | ses yazilan %ld bayt\n",
              c_sb_apply - la, c_sb_capture - lc, c_sb_inner, c_unlock_bytes - lu); la = c_sb_apply; lc = c_sb_capture; lu = c_unlock_bytes; }
        EnterCriticalSection(&g_slk); if (g_slog) fflush(g_slog); LeaveCriticalSection(&g_slk);
        lf = f; lpv = c_pres_vt; lps = c_pres_sc; lb = c_begin; le = c_end; ld = c_draw_all;
        lqm = g_qpc_main; lqo = g_qpc_other; lg = g_gdd_kbd; lev = g_ev_out;
        if (real_s() > g_limit_s){
            L("SURE ASIMI: %.0f sn gecti, kare %ld -- oyun kapatiliyor\n", real_s(), f);
            if (g_kare) fflush(g_kare);
            EnterCriticalSection(&g_slk); if (g_slog) fflush(g_slog); if (g_sdat) fflush(g_sdat); LeaveCriticalSection(&g_slk);
            TerminateProcess(GetCurrentProcess(), 2);
        }
    }
    return 0;
}

static void start(HMODULE self){
    InitializeCriticalSection(&g_lk); InitializeCriticalSection(&g_hk_lk); InitializeCriticalSection(&g_slk);
    QueryPerformanceCounter(&g_t0); QueryPerformanceFrequency(&g_qf);
    AddVectoredExceptionHandler(1, veh);
    char exe[MAX_PATH]; GetModuleFileNameA(0, exe, MAX_PATH);
    snprintf(g_game, sizeof g_game, "%s", exe);
    { char *s = strrchr(g_game, '\\'); if (s) *s = 0; }
    char ini[MAX_PATH], me[MAX_PATH];
    GetModuleFileNameA(self, me, MAX_PATH);
    { char *s = strrchr(me, '\\'); if (s) *s = 0; }
    snprintf(ini, sizeof ini, "%s\\nfsref.ini", me);

    snprintf(g_out, sizeof g_out, "%s\\NFSRef_cikti", g_game);
    CreateDirectoryA(g_out, 0);
    for (int i = 1; i < 1000; i++){                 /* her kosuda bos kayit klasoru */
        snprintf(g_save, sizeof g_save, "%s\\kayit_%03d", g_out, i);
        if (GetFileAttributesA(g_save) == INVALID_FILE_ATTRIBUTES){ CreateDirectoryA(g_save, 0); break; }
    }
    char p[MAX_PATH];
    snprintf(p, sizeof p, "%s\\golge", g_out); CreateDirectoryA(p, 0);
    snprintf(p, sizeof p, "%s\\nfsref_log.txt", g_out); g_log = fopen(p, "w");
    snprintf(p, sizeof p, "%s\\kareler.csv", g_out); g_kare = fopen(p, "w");
    if (g_kare){ fprintf(g_kare, "kare,cizim,ilkel,saat_sorgu,ek_us"); for (int i = 0; i < 16; i++) fprintf(g_kare, ",c%d", i); fprintf(g_kare, "\n"); }

    char buf[2048];
    GetPrivateProfileStringA("nfsref", "autoseq", "", buf, sizeof buf, ini); parse_seq(buf);
    GetPrivateProfileStringA("nfsref", "kareler", "", buf, sizeof buf, ini); cap_n = parse_list(buf, cap_f, CAPMAX);
    GetPrivateProfileStringA("nfsref", "dokum", "", buf, sizeof buf, ini); dump_n = parse_list(buf, dump_f, CAPMAX);
    g_quit = GetPrivateProfileIntA("nfsref", "cikis", 0, ini);
    g_mx = GetPrivateProfileIntA("nfsref", "fare_x", 5, ini);
    g_my = GetPrivateProfileIntA("nfsref", "fare_y", 5, ini);
    g_nomovies = GetPrivateProfileIntA("nfsref", "video_yok", 1, ini);
    g_pace = GetPrivateProfileIntA("nfsref", "hiz60", 1, ini);
    g_hp_on = GetPrivateProfileIntA("nfsref", "hotpatch", 1, ini);
    g_tex_hash_on = GetPrivateProfileIntA("nfsref", "doku_ozet", 1, ini);
    g_rt_dump_on  = GetPrivateProfileIntA("nfsref", "hedef_doku", 1, ini);
    g_sh_dump_on  = GetPrivateProfileIntA("nfsref", "golge_dokum", 1, ini);
    g_ses_on      = GetPrivateProfileIntA("nfsref", "ses", 1, ini);
    g_ses_cap     = (long long)GetPrivateProfileIntA("nfsref", "ses_sinir_mb", 200, ini) << 20;
    if (g_ses_on){ char q[MAX_PATH];
        snprintf(q, sizeof q, "%s\\ses_log.txt", g_out);  g_slog = fopen(q, "w");
        snprintf(q, sizeof q, "%s\\ses_veri.bin", g_out); g_sdat = fopen(q, "wb");
        if (g_slog) fprintf(g_slog, "# kare gercek_ms OLAY ...  (UNLOCK 'veri' = ses_veri.bin icindeki bayt konumu)\n"); }
    g_limit_s = GetPrivateProfileIntA("nfsref", "sure_siniri", 420, ini);
    { char md[32] = "";
      GetEnvironmentVariableA("NFSREF_MOD", md, sizeof md);
      if (!_stricmp(md, "teshis")){
          /* kisa teshis: ~40 sn, ilk dort Enter'i ve uc menu karesini dener */
          g_quit = 2100; g_limit_s = 75; dump_n = 0;
          cap_n = 0; cap_f[cap_n++] = 900; cap_f[cap_n++] = 1300; cap_f[cap_n++] = 1650; cap_f[cap_n++] = 2050;
          L("TESHIS MODU: cikis 2100. kare, sure siniri 75 sn\n");
      } }
    int k_saat  = GetPrivateProfileIntA("nfsref", "saat", 1, ini);
    int k_girdi = GetPrivateProfileIntA("nfsref", "girdi", 1, ini);
    int k_kayit = GetPrivateProfileIntA("nfsref", "kayit", 1, ini);
    int k_dosya = GetPrivateProfileIntA("nfsref", "dosya", 1, ini);

    long sz = 0; uint32_t crc = crc32_file(exe, &sz);
    L("NFSRef 6  exe %s  boyut %ld  crc32 %08x\n", exe, sz, crc);
    L("ini %s: %d tus, %d kare, %d dokum, cikis %ld, fare %d,%d, video_yok %d | hiz60 %d hotpatch %d sinir %.0f sn saat %d girdi %d kayit %d dosya %d\n", ini, sq_n, cap_n, dump_n, g_quit, g_mx, g_my, g_nomovies, g_pace, g_hp_on, g_limit_s, k_saat, k_girdi, k_kayit, k_dosya);

    /* port'un varsayilanlari, sonra dosya */
    reg_put("NotFirstTime", 0, 1, 0); reg_put("Language", 1, 0, "English");
    reg_put("Install Dir", 1, 0, g_game); reg_put("CD Drive", 1, 0, g_game);
    reg_put("StreamingInstall", 0, 0, 0); reg_put("VERSION", 1, 0, "1.2.0.0"); reg_put("SIZE", 0, 0, 0);
    snprintf(p, sizeof p, "%s\\nfsref_kayit.txt", me); reg_load(p);
    L("kayit defteri: %d deger\n", g_regn);

    HMODULE m = GetModuleHandleA(0);
    int ok = 0;
#define IAT(dll, fn, h, o) ok += iat_patch(m, dll, fn, 0, (void *)h, (void **)o)
    if (k_saat){
    IAT("KERNEL32.dll", "QueryPerformanceCounter", h_QPC, 0);
    IAT("KERNEL32.dll", "QueryPerformanceFrequency", h_QPF, 0);
    IAT("KERNEL32.dll", "GetTickCount", h_GetTickCount, 0);
    IAT("KERNEL32.dll", "GetSystemTimeAsFileTime", h_GetSystemTimeAsFileTime, 0);
    IAT("WINMM.dll", "timeGetTime", h_timeGetTime, 0); }
    if (k_dosya){
    IAT("KERNEL32.dll", "CreateFileA", h_CreateFileA, &o_CreateFileA);
    IAT("KERNEL32.dll", "GetDriveTypeA", h_GetDriveTypeA, 0);
    IAT("KERNEL32.dll", "GetLogicalDrives", h_GetLogicalDrives, 0);
    IAT("SHFOLDER.dll", "SHGetFolderPathA", h_SHGetFolderPathA, 0); }
    if (k_kayit){
    IAT("ADVAPI32.dll", "RegOpenKeyExA", h_RegOpenKeyExA, &o_RegOpenKeyExA);
    IAT("ADVAPI32.dll", "RegOpenKeyA", h_RegOpenKeyA, &o_RegOpenKeyA);
    IAT("ADVAPI32.dll", "RegCreateKeyA", h_RegCreateKeyA, &o_RegCreateKeyA);
    IAT("ADVAPI32.dll", "RegQueryValueExA", h_RegQueryValueExA, &o_RegQueryValueExA);
    IAT("ADVAPI32.dll", "RegSetValueExA", h_RegSetValueExA, &o_RegSetValueExA);
    IAT("ADVAPI32.dll", "RegCloseKey", h_RegCloseKey, &o_RegCloseKey); }
    if (k_girdi){
    IAT("USER32.dll", "GetCursorPos", h_GetCursorPos, 0);
    IAT("DINPUT8.dll", "DirectInput8Create", h_DI8Create, &o_DI8Create); }
    IAT("d3d9.dll", "Direct3DCreate9", h_D3DCreate9, &o_D3DCreate9);
    if (g_ses_on) ok += iat_patch(m, "DSOUND.dll", 0, 1, (void *)h_DSCreate, (void **)&o_DSCreate);
    L("IAT: %d yama\n", ok);
    seq_update(1);
    CreateThread(0, 0, durum_thread, 0, 0, 0);
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD why, LPVOID r){
    (void)r;
    if (why == DLL_PROCESS_ATTACH){ DisableThreadLibraryCalls(h); start(h); }
    return TRUE;
}
