/* .app paketi destegi
 *
 * Kabuk betigi olmadan Finder'dan calistirilabilmesi icin gereken uc sey:
 *   1) run.sh'in export ettigi ayarlar (paket icinde kendiliginden kurulur),
 *   2) oyun verisinin nerede oldugu (paket disinda; kullanicidan sorulur),
 *   3) stderr'in bir yere yazilmasi (Finder'dan calisinca terminal yok).
 *
 * Bu dosyadaki hicbir sey terminalden calistirmayi degistirmez: tum yollar
 * once "paketten mi calisiyoruz" diye bakar, degilse hemen doner.        */
#import <Cocoa/Cocoa.h>
#include <string.h>
#include "enhance.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <libgen.h>
#include <mach-o/dyld.h>

static char g_exe_dir[1024];      /* .../NFSU2.app/Contents/MacOS      */
static char g_app_dir[1024];      /* .app dosyasinin BULUNDUGU dizin   */
static char g_res_dir[1024];      /* .../NFSU2.app/Contents/Resources  */
static int  g_bundled = -1;

/* Paketten mi calisiyoruz?  Sembolik baglantilari cozulmus gercek yola
 * bakiyoruz: ".../X.app/Contents/MacOS/Y" ise evet.                     */
int app_is_bundled(void){
    if (g_bundled >= 0) return g_bundled;
    char raw[1024]; uint32_t n = sizeof raw;
    if (_NSGetExecutablePath(raw, &n) != 0){ g_bundled = 0; return 0; }
    char real[1024];
    if (!realpath(raw, real)) snprintf(real, sizeof real, "%s", raw);

    snprintf(g_exe_dir, sizeof g_exe_dir, "%s", real);
    char *s = strrchr(g_exe_dir, '/'); if (s) *s = 0;

    const char *tail = "/Contents/MacOS";
    size_t el = strlen(g_exe_dir), tl = strlen(tail);
    if (el <= tl || strcmp(g_exe_dir + el - tl, tail) != 0){ g_bundled = 0; return 0; }

    /* .../X.app  ->  Resources ve .app'in bulundugu dizin */
    char app[1024]; snprintf(app, sizeof app, "%.*s", (int)(el - tl), g_exe_dir);
    snprintf(g_res_dir, sizeof g_res_dir, "%s/Contents/Resources", app);
    snprintf(g_app_dir, sizeof g_app_dir, "%s", app);
    s = strrchr(g_app_dir, '/'); if (s) *s = 0;
    g_bundled = 1;
    return 1;
}

/* run.sh'teki export'larin paket ici karsiligi.  Cevrede zaten varsa
 * dokunmuyoruz: "NFSU2_DATA=... open -a" ile ezmek mumkun kalsin.
 * ALARM ozellikle 0: boot.c'nin 20 sn'lik tani zaman asimi oyunu
 * kullanicinin elinden alirdi.                                          */
static void setdef(const char *k, const char *v){ if (!getenv(k)) setenv(k, v, 1); }
void app_set_defaults(void){
    setdef("WINDOW", "1");
    setdef("ALARM", "0");
    setdef("FIXMAT", "1");
    setdef("FIXMAT_MODE", "3");
    setdef("NOMOVIES", "1");
    setdef("SNDSYNC", "1");
    setdef("GPUVS", "1");
}

/* Finder'dan calisinca stderr /dev/null'a gider; hata raporu isteyince
 * elimizde hicbir sey olmaz.  Terminalden calistirildiginda (stderr bir
 * tty ise) dokunmuyoruz.                                                */
void app_open_log(void){
    if (isatty(2)) return;
    const char *home = getenv("HOME"); if (!home) return;
    char dir[1024], pth[1200];
    snprintf(dir, sizeof dir, "%s/Library/Logs/NFSU2", home);
    mkdir(dir, 0755);
    /* HD build ayri log yazar: iki surum yan yana calistirilinca birbirinin
     * logunu ezmesin. */
    const char *lg = NFS_ENHANCED ? "nfsu2-hd" : "nfsu2";
    snprintf(pth, sizeof pth, "%s/%s.log", dir, lg);
    /* "w": her calistirmada bastan.  Onceki kosu tanida lazim olabilir. */
    char prev[1300]; snprintf(prev, sizeof prev, "%s/%s-onceki.log", dir, lg);
    rename(pth, prev);
    freopen(pth, "w", stderr);
    setvbuf(stderr, NULL, _IOLBF, 0);
    freopen(pth, "a", stdout);
}

static int has_exe(const char *dir, char *out, size_t osz){
    char p[1200]; struct stat st;
    static const char *names[] = { "SPEED2.EXE", "speed2.exe", "Speed2.exe" };
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++){
        snprintf(p, sizeof p, "%s/%s", dir, names[i]);
        if (stat(p, &st) == 0 && S_ISREG(st.st_mode)){ snprintf(out, osz, "%s", p); return 1; }
    }
    return 0;
}

/* Secilen dizin ya SPEED2.EXE'yi dogrudan icerir ya da bir ust dizindir
 * ("Need for Speed Underground 2 - Clean Edition" gibi).  Iki seviye
 * asagi bakiyoruz; daha derini kullaniciya sormaya deger.               */
static int find_under(const char *dir, char *out, size_t osz, int depth){
    if (has_exe(dir, out, osz)) return 1;
    if (depth <= 0) return 0;
    NSFileManager *fm = [NSFileManager defaultManager];
    NSArray *kids = [fm contentsOfDirectoryAtPath:@(dir) error:nil];
    for (NSString *k in kids){
        if ([k hasPrefix:@"."]) continue;
        char sub[1200]; snprintf(sub, sizeof sub, "%s/%s", dir, [k UTF8String]);
        struct stat st;
        if (stat(sub, &st) == 0 && S_ISDIR(st.st_mode) &&
            find_under(sub, out, osz, depth - 1)) return 1;
    }
    return 0;
}

static NSString *const kPrefKey = @"OyunDizini";
static int g_embedded = 0;        /* veri paketin icinden bulundu */

/* Oyun verisi paketin icindeyse kayitlarin yazilacagi dizin; degilse NULL
 * (o zaman oyun eskisi gibi kendi klasorune yazar).  Paketin icine yazmak
 * imzayi bozar ve uygulama yenilenince kayitlari siler.
 * NFSU2_SAVEDIR ile her kosulda zorlanabilir. */
const char *app_save_dir(void){
    static char dir[1200];
    const char *env = getenv("NFSU2_SAVEDIR");
    if (env && *env){ snprintf(dir, sizeof dir, "%s", env); return dir; }
    if (!g_embedded) return NULL;
    const char *home = getenv("HOME"); if (!home) return NULL;
    snprintf(dir, sizeof dir, "%s/Library/Application Support/NFSU2 Kayitlar", home);
    return dir;
}

static void app_alert(const char *title, const char *msg){
    NSAlert *a = [NSAlert new];
    [a setMessageText:@(title)];
    [a setInformativeText:@(msg)];
    [a addButtonWithTitle:@"Tamam"];
    [a runModal];
}

/* Kullaniciya oyun klasorunu sordur.  Iptal ederse 0 doner.            */
static int app_ask(char *out, size_t osz){
    NSOpenPanel *op = [NSOpenPanel openPanel];
    [op setCanChooseFiles:NO];
    [op setCanChooseDirectories:YES];
    [op setAllowsMultipleSelection:NO];
    [op setPrompt:@"Sec"];
    [op setMessage:@"Need for Speed Underground 2 klasorunu sec "
                   @"(icinde SPEED2.EXE olan klasor)."];
    [NSApp activateIgnoringOtherApps:YES];
    if ([op runModal] != NSModalResponseOK) return 0;
    NSString *dir = [[op URL] path];
    if (!find_under([dir UTF8String], out, osz, 2)){
        app_alert("Bu klasorde SPEED2.EXE yok",
                  "Oyunun kurulu oldugu klasoru sec: icinde SPEED2.EXE ve "
                  "CARS / TRACKS / SOUND klasorleri bulunur.");
        return 0;
    }
    return 1;
}

/* Oyun verisini bul.  Sirasiyla: cevre degiskeni, daha once secilmis yol,
 * paketin ici, .app'in yani, ev dizini, sonra kullaniciya sor.          */
const char *app_find_game(void){
    static char found[1200];
    char dir[1200];

    const char *env = getenv("NFSU2_DATA");
    if (env && *env){
        struct stat st;
        if (stat(env, &st) == 0 && S_ISREG(st.st_mode)){
            snprintf(found, sizeof found, "%s", env); return found; }
        if (find_under(env, found, sizeof found, 2)) return found;
        fprintf(stderr, "NFSU2_DATA=%s icinde SPEED2.EXE yok\n", env);
    }

    /* Paketin icine gomulu veri (./pack.sh --embed) her seyden once gelir:
     * "tek parca" uygulama, daha once secilmis baska bir klasore kaymasin. */
    if (app_is_bundled()){
        snprintf(dir, sizeof dir, "%s/Game", g_res_dir);
        if (find_under(dir, found, sizeof found, 2)){ g_embedded = 1; return found; }
    }

    NSString *saved = [[NSUserDefaults standardUserDefaults] stringForKey:kPrefKey];
    if (saved && find_under([saved UTF8String], found, sizeof found, 2)) return found;

    if (app_is_bundled()){
        /* .app'in yani ve bir ust dizin: kaynak agacinda .app out/ icinde
         * duruyor, oyun ise proje kokunde. */
        if (find_under(g_app_dir, found, sizeof found, 2)) return found;
        snprintf(dir, sizeof dir, "%s", g_app_dir);
        { char *s2 = strrchr(dir, '/'); if (s2 && s2 != dir){ *s2 = 0;
          if (find_under(dir, found, sizeof found, 2)) return found; } }
    }

    const char *home = getenv("HOME");
    if (home){
        static const char *cand[] = {
            "%s/Library/Application Support/NFSU2",
            "%s/Documents/Need for Speed Underground 2",
            "%s/Downloads/Need for Speed Underground 2",
        };
        for (unsigned i = 0; i < sizeof cand / sizeof cand[0]; i++){
            snprintf(dir, sizeof dir, cand[i], home);
            if (find_under(dir, found, sizeof found, 2)) return found;
        }
    }

    /* Bulunamadi: kullaniciya sor.  Sectigi yolu sakla ki bir daha
     * sorulmasin.  NSOpenPanel icin NSApp gerekli.                      */
    fprintf(stderr, "oyun bulunamadi -> kullaniciya klasor soruluyor\n");
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    for (int tries = 0; tries < 3; tries++){
        if (!app_ask(found, sizeof found)) return NULL;
        char keep[1200]; snprintf(keep, sizeof keep, "%s", found);
        char *s = strrchr(keep, '/'); if (s) *s = 0;
        [[NSUserDefaults standardUserDefaults] setObject:@(keep) forKey:kPrefKey];
        [[NSUserDefaults standardUserDefaults] synchronize];
        return found;
    }
    return NULL;
}

/* Oyun bulunamadan cikarken kullaniciya sebebini soyle. */
void app_no_game(void){
    app_alert("Oyun dosyalari bulunamadi",
              "NFSU2 bu uygulamanin icinde gelmez; kendi kurulumun gerekli.\n\n"
              "Oyun klasorunu (icinde SPEED2.EXE olan) uygulamanin yanina "
              "koyabilir ya da uygulamayi tekrar acip klasoru secebilirsin.");
}
