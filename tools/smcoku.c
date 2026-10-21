/* SMC sicaklik / fan okuyucu (sudo gerekmez; AppleSMC kullanici istemcisi).
 *
 *   smcoku ANAHTAR...                 anahtarlari bir kez yazar (ornek: smcoku F0Ac Tp01)
 *   smcoku -k <ms> <cikti.csv>        oturum kaydi: ms'de bir satir, sinyalle (INT/TERM) biter
 *
 * CSV: epoch_s,fan0,fan1,cpu_ort,cpu_max,gpu_ort,gpu_max,sistem_w
 * cpu = Tp* (P/E cekirdek kumeleri), gpu = Tg*, sistem_w = PSTR (SMC sistem gucu,
 * ~1 sn'de bir guncellenir).  Bu makinede olmayan anahtarlar atlanir.
 * Derleme: clang -O1 -framework IOKit -o smcoku smcoku.c */
#include <IOKit/IOKitLib.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

typedef struct { char major, minor, build, reserved; uint16_t release; } SVers;
typedef struct { uint16_t version, length; uint32_t cpuPLimit, gpuPLimit, memPLimit; } SPLimit;
typedef struct { uint32_t dataSize, dataType; char dataAttributes; } SKeyInfo;
typedef struct {
    uint32_t key; SVers vers; SPLimit pLimitData; SKeyInfo keyInfo;
    char result, status, data8; uint32_t data32; char bytes[32];
} SKeyData;

static io_connect_t g_c;
static volatile sig_atomic_t g_stop;

static uint32_t k4(const char *s){ return ((uint32_t)s[0] << 24) | ((uint32_t)s[1] << 16) | ((uint32_t)s[2] << 8) | (uint32_t)s[3]; }

/* 9 = anahtar bilgisi, 5 = deger oku */
static int smc_read(const char *key, double *v){
    SKeyData in, out; size_t sz = sizeof out;
    memset(&in, 0, sizeof in); memset(&out, 0, sizeof out);
    in.key = k4(key); in.data8 = 9;
    if (IOConnectCallStructMethod(g_c, 2, &in, sizeof in, &out, &sz) || out.result) return -1;
    uint32_t t = out.keyInfo.dataType;
    in.keyInfo.dataSize = out.keyInfo.dataSize; in.data8 = 5; sz = sizeof out;
    if (IOConnectCallStructMethod(g_c, 2, &in, sizeof in, &out, &sz) || out.result) return -1;
    const uint8_t *b = (const uint8_t *)out.bytes;
    if (t == k4("flt ")){ float f; memcpy(&f, b, 4); *v = f; }
    else if (t == k4("ui8 ")) *v = b[0];
    else if (t == k4("ui16")) *v = (b[0] << 8) | b[1];
    else if (t == k4("ui32")) *v = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | (b[2] << 8) | b[3];
    else return -1;
    return 0;
}

/* M1 Pro/Max kume sensorleri; digerleri yoksa atlanir */
static const char *kCpu[] = { "Tp01", "Tp05", "Tp09", "Tp0D", "Tp0H", "Tp0L", "Tp0P", "Tp0T", "Tp0X", "Tp0b", NULL };
static const char *kGpu[] = { "Tg05", "Tg0D", "Tg0L", "Tg0T", "Tg0f", "Tg0j", NULL };

static void group(const char **keys, double *avg, double *mx){
    double s = 0, m = -1; int n = 0;
    for (int i = 0; keys[i]; i++){ double v;
        if (smc_read(keys[i], &v) || v <= 0 || v > 150) continue;
        s += v; n++; if (v > m) m = v; }
    *avg = n ? s / n : -1; *mx = m;
}

static void on_sig(int s){ (void)s; g_stop = 1; }

int main(int argc, char **argv){
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AppleSMC"));
    if (!s || IOServiceOpen(s, mach_task_self(), 0, &g_c)){ fprintf(stderr, "smcoku: AppleSMC acilamadi\n"); return 1; }
    if (argc == 4 && !strcmp(argv[1], "-k")){
        int ms = atoi(argv[2]); FILE *f = fopen(argv[3], "w");
        if (ms < 100 || !f){ fprintf(stderr, "smcoku: -k <ms>=100.. <cikti.csv>\n"); return 1; }
        signal(SIGINT, on_sig); signal(SIGTERM, on_sig);
        fprintf(f, "epoch_s,fan0,fan1,cpu_ort,cpu_max,gpu_ort,gpu_max,sistem_w\n");
        while (!g_stop){
            struct timeval tv; gettimeofday(&tv, NULL);
            double f0 = -1, f1 = -1, ca, cm, ga, gm, p = -1;
            smc_read("F0Ac", &f0); smc_read("F1Ac", &f1); smc_read("PSTR", &p);
            group(kCpu, &ca, &cm); group(kGpu, &ga, &gm);
            fprintf(f, "%.3f,%.0f,%.0f,%.1f,%.1f,%.1f,%.1f,%.2f\n",
                    tv.tv_sec + tv.tv_usec / 1e6, f0, f1, ca, cm, ga, gm, p);
            fflush(f);
            usleep((useconds_t)ms * 1000);
        }
        fclose(f);
        return 0;
    }
    for (int a = 1; a < argc; a++){ double v;
        if (smc_read(argv[a], &v)) printf("%s yok\n", argv[a]); else printf("%s %.1f\n", argv[a], v); }
    return 0;
}
