/* CoreAudio cikisi.
 *
 * DirectSound katmani simdiye kadar yalnizca muhasebe yapiyordu (yazilan
 * bayt sayilir, oynatma imleci gecen zamana gore ilerlerdi) ama SES
 * CIKISI HIC YOKTU.  Oyun gercek PCM yaziyor (olculdu: ~9 MB, 5000+ dolu
 * yazma); burada o tamponlari karistirip AudioQueue ile calariz.
 *
 * Karistirici 44.1 kHz, 2 kanal, 16-bit cikisa donusturur; kaynak
 * tamponlar farkli hizda/kanalda olabilir (en yakin komsu ornekleme). */
#import <AudioToolbox/AudioToolbox.h>
#import <AudioUnit/AudioUnit.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <math.h>

extern uint8_t *g_mem;
uint64_t host_us(void);          /* oyunun SANAL saati (kare basina 16.67 ms) */

/* Oyun ses verisini kendi sanal zamaninda uretir; karistirici ise gercek
 * zamanda tuketiyordu.  Emulator 60 fps'in altinda kalinca sanal zaman
 * geride kalir ve her tampon turunda yazilmamis bir bolge kalir -> ses
 * duzenli araliklarla kesilir (olculdu: bosluk periyodu tam olarak tampon
 * suresi).  Cozum: tuketimi oyunun uretim hizina baglamak. */
static double g_rate_ratio = 1.0;
static int    g_sync_mode = -1;      /* SNDSYNC: 1 = oyun hizina bagla */
static void update_rate_ratio(void){
    if (g_sync_mode < 0){
        const char *e = getenv("SNDSYNC");
        g_sync_mode = (e && *e) ? (*e != '0') : 1;      /* varsayilan: acik */
    }
    if (!g_sync_mode){ g_rate_ratio = 1.0; return; }
    static uint64_t lv = 0, lr = 0;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t rus = (uint64_t)ts.tv_sec*1000000ull + (uint64_t)ts.tv_nsec/1000ull;
    uint64_t vus = host_us();
    if (lv && rus > lr){
        double dv = (double)(vus - lv), dr = (double)(rus - lr);
        if (dr > 1000.0){
            double k = dv / dr;
            if (k < 0.25) k = 0.25;
            /* TAVAN 1.0: ses GERCEK ZAMANDAN HIZLI calmamali.  Bu oran
             * oyunun saatinden geliyordu ve saat (creep hatasi yuzunden)
             * yer yer 2 kata kadar hizli kosuyordu -> sarkilar hizli
             * caliyordu.  Saat duzeltildi, ama bagi da guvenli yapiyoruz:
             * bu yolla bir saat hatasi bir daha PERDE hatasina donusemez. */
            if (k > 1.00) k = 1.00;
            /* yumusat: ani kare hizi dalgalanmalari perdeyi zipllatmasin */
            g_rate_ratio += (k - g_rate_ratio) * 0.10;
        }
    }
    lv = vus; lr = rus;
}

#define OUT_RATE   44100
#define OUT_CH     2
/* Emulator CPU'yu doldurdugu icin 3x1024 (~70 ms) tamponlama yetmiyor,
 * geri cagirma geciktiginde kuyruk aciligindan ses kesiliyordu. */
#define NBUF       6
#define FRAMES     2048

#define MAXSND 128
typedef struct {
    uint32_t data, bytes, rate, chans, bits;
    int      used, playing, loop;
    double   cursor;          /* kaynak ornek cinsinden */
    float    vol;             /* 0..1 */
    uint32_t wpos;            /* oyunun yazdigi son baytin bitisi */
    int      wseen;           /* hic yazma gorduk mu */
    double   trim;            /* hiz duzeltmesi (servo) */
    double   target;          /* kilitlenecek dolgu orani (uyarlanir) */
    double   freq;            /* SetFrequency orani (1.0 = ozgun hiz) */
    uint64_t plays, stops, wr_bytes, wr_calls, wr_nonzero;
    uint64_t over;            /* yaziciyi gectigimiz geri-cagirma sayisi */
} SndBuf;

static SndBuf g_snd[MAXSND];
static int    g_sndn = 1;                    /* 0 = gecersiz kimlik */
static pthread_mutex_t g_slock = PTHREAD_MUTEX_INITIALIZER;
static AudioQueueRef        g_q;
static AudioQueueBufferRef  g_qb[NBUF];
static AudioUnit            g_au;            /* dusuk gecikmeli yol */
static int g_audio_on = 0;
static int g_backend = 0;                    /* 1 = AudioUnit, 2 = AudioQueue */
static unsigned g_au_slice = 0;              /* geri cagirma basina kare */
static uint64_t g_mixed_frames = 0, g_active_peak = 0;
static uint64_t g_replay = 0, g_reissue = 0, g_starved = 0, g_worst_gap_us = 0;
static uint64_t g_out_frames = 0, g_out_zero = 0, g_gap_cb = 0;

/* --- tani: karistirici cikisini ve tampon durumunu diske dok --- */
static FILE *g_dump = NULL;      /* SNDDUMP=<yol> : ham s16le stereo */
static FILE *g_csv  = NULL;      /* SNDCSV=<yol>  : geri-cagirma basina durum */
static uint64_t g_cb_no = 0;

int audio_register(uint32_t data, uint32_t bytes, uint32_t rate,
                   uint32_t chans, uint32_t bits){
    if (!data || !bytes) return 0;
    if (rate < 4000u || rate > 192000u) rate = 44100u;
    if (chans < 1u || chans > 2u) chans = 1u;
    if (bits != 8u && bits != 16u) bits = 16u;
    pthread_mutex_lock(&g_slock);
    int id = 0;
    if (g_sndn < MAXSND){
        id = g_sndn++;
        SndBuf *b = &g_snd[id];
        b->data = data; b->bytes = bytes; b->rate = rate;
        b->chans = chans; b->bits = bits;
        b->used = 1; b->playing = 0; b->loop = 0; b->cursor = 0.0; b->vol = 1.f;
        b->wpos = 0; b->wseen = 0; b->trim = 1.0; b->target = 0.0; b->over = 0;
        b->freq = 1.0; b->plays = b->stops = 0;
        b->wr_bytes = b->wr_calls = b->wr_nonzero = 0;
        fprintf(stderr, "  [ses] tampon #%d: %u Hz, %u kanal, %u bit, %u bayt (%.2f sn)\n",
                id, rate, chans, bits, bytes,
                (double)bytes / (double)(rate * chans * (bits/8u)));
    }
    pthread_mutex_unlock(&g_slock);
    return id;
}
void audio_play(int id, int loop){
    if (id <= 0 || id >= MAXSND) return;
    pthread_mutex_lock(&g_slock);
    if (g_snd[id].used){
        /* DIKKAT: zaten calan bir tamponda imleci SIFIRLAMA.  Oyun akis
         * tamponlarinda Play'i tekrar tekrar cagiriyor; her cagride basa
         * donulunce sarki "dur-basla" seklinde tekliyordu. */
        if (!g_snd[id].playing){ g_snd[id].cursor = 0.0; g_replay++; }
        else g_reissue++;
        g_snd[id].playing = 1;
        g_snd[id].loop = loop;
        g_snd[id].plays++;
    }
    pthread_mutex_unlock(&g_slock);
}
/* Guest Unlock'tan: yazilan bolgenin BITIS ofseti.  Karistirici oynatma
 * imlecini bunun sabit bir mesafe gerisinde tutar (servo) - boylece ne
 * yazilmamis bolge calinir (bosluk) ne de imlec geri kalir (gecikme). */
void audio_note_write(int id, uint32_t end_off){
    if (id <= 0 || id >= MAXSND) return;
    pthread_mutex_lock(&g_slock);
    if (g_snd[id].used && g_snd[id].bytes){
        g_snd[id].wpos = end_off % g_snd[id].bytes;
        g_snd[id].wseen = 1;
    }
    pthread_mutex_unlock(&g_slock);
}
void audio_note_write_stat(int id, uint32_t n, int nonzero){
    if (id <= 0 || id >= MAXSND) return;
    pthread_mutex_lock(&g_slock);
    if (g_snd[id].used){ g_snd[id].wr_calls++; g_snd[id].wr_bytes += n;
                         if (nonzero) g_snd[id].wr_nonzero++; }
    pthread_mutex_unlock(&g_slock);
}
/* DirectSound SetFrequency: oynatma ornekleme hizini degistirir.
 * Oyun motor sesi gibi efektlerin PERDESINI boyle ayarliyor; uygulanmayinca
 * her ses ozgun hizinda calar. */
void audio_set_freq(int id, uint32_t hz){
    if (id <= 0 || id >= MAXSND) return;
    pthread_mutex_lock(&g_slock);
    if (g_snd[id].used && g_snd[id].rate){
        if (!hz) g_snd[id].freq = 1.0;                      /* DSBFREQUENCY_ORIGINAL */
        else {
            double f = (double)hz / (double)g_snd[id].rate;
            if (f < 0.05) f = 0.05;
            if (f > 8.00) f = 8.00;
            g_snd[id].freq = f;
        }
    }
    pthread_mutex_unlock(&g_slock);
}
/* Ayni PCM verisini paylasan ikinci bir ses (DuplicateSoundBuffer).
 * Oyun ES ZAMANLI efektleri boyle calar; kopya bos donunce hepsi
 * sessiz kaliyordu. */
int audio_duplicate(int src){
    if (src <= 0 || src >= MAXSND) return 0;
    int id = 0;
    pthread_mutex_lock(&g_slock);
    if (g_snd[src].used && g_sndn < MAXSND){
        id = g_sndn++;
        g_snd[id] = g_snd[src];
        g_snd[id].playing = 0; g_snd[id].cursor = 0.0; g_snd[id].trim = 1.0;
        g_snd[id].target = 0.0; g_snd[id].over = 0;
        g_snd[id].plays = g_snd[id].stops = 0;
        g_snd[id].wr_bytes = g_snd[id].wr_calls = g_snd[id].wr_nonzero = 0;
    }
    pthread_mutex_unlock(&g_slock);
    return id;
}
void audio_stop(int id){
    if (id <= 0 || id >= MAXSND) return;
    pthread_mutex_lock(&g_slock);
    if (g_snd[id].used){ g_snd[id].playing = 0; g_snd[id].stops++; }
    pthread_mutex_unlock(&g_slock);
}
/* Guest'in GetCurrentPosition'i icin.
 * DirectSound iki imlec dondurur: OYNATMA ve YAZMA.  Yazma imleci
 * oynatmanin ONUNDEDIR (donanimin okumakta oldugu guvenli sinir); oyun
 * veriyi oradan itibaren yazar.  Ikisini esit dondurmek, oyunun tam
 * caldigimiz noktaya yazmasina ve sesin "dur-basla" seklinde teklemesine
 * yol aciyordu. */
#define WRITE_LEAD_MS 50u
void audio_cursors(int id, uint32_t *play, uint32_t *write){
    uint32_t p = 0, w = 0;
    if (id > 0 && id < MAXSND){
        pthread_mutex_lock(&g_slock);
        if (g_snd[id].used && g_snd[id].bytes){
            uint32_t bpf = g_snd[id].chans * (g_snd[id].bits/8u);
            uint32_t sz  = g_snd[id].bytes;
            p = (uint32_t)(g_snd[id].cursor * (double)bpf) % sz;
            uint32_t lead = (g_snd[id].rate * WRITE_LEAD_MS / 1000u) * bpf;
            if (lead >= sz) lead = sz / 4u;
            w = (p + lead) % sz;
        }
        pthread_mutex_unlock(&g_slock);
    }
    if (play)  *play  = p;
    if (write) *write = w;
}
/* Karistiricinin zaman ekseni (SNDDUMP dosyasiyla ayni t).  Tani
 * kayitlarini ses dokumuyle hizalamak icin. */
double audio_time(void){ return (double)g_mixed_frames / (double)OUT_RATE; }
uint32_t audio_cursor(int id){ uint32_t p; audio_cursors(id, &p, 0); return p; }
void audio_set_volume(int id, long millibel){
    if (id <= 0 || id >= MAXSND) return;
    /* DirectSound: 0 = tam ses, -10000 = sessiz (yuzde yuz logaritmik) */
    float v = (millibel <= -10000L) ? 0.f : powf(10.f, (float)millibel / 2000.f);
    if (v > 1.f) v = 1.f;
    pthread_mutex_lock(&g_slock);
    if (g_snd[id].used) g_snd[id].vol = v;
    pthread_mutex_unlock(&g_slock);
}

static void mix_into(int16_t *out, unsigned frames){
    static float acc[FRAMES * OUT_CH];
    if (frames > FRAMES) frames = FRAMES;
    memset(acc, 0, sizeof(float) * frames * OUT_CH);
    uint64_t active = 0;
    update_rate_ratio();

    pthread_mutex_lock(&g_slock);
    for (int i = 1; i < g_sndn; i++){
        SndBuf *b = &g_snd[i];
        if (!b->used || !b->playing || !b->bytes) continue;
        uint32_t bps  = b->bits / 8u;
        uint32_t bpf  = b->chans * bps;
        if (!bpf) continue;
        uint32_t nsrc = b->bytes / bpf;
        if (!nsrc) continue;
        /* TEK YONLU servo: hiz orani (feedforward) uretimle tuketimi zaten
         * esitliyor.  Servo yalnizca ONUMUZDEKI YAZILI VERI AZALDIGINDA
         * devreye girip yavaslatir; boylece yazilmamis bolge hic calinmaz.
         * Iki yonlu servo hedefi yanlis secilince durumu kotulestirmisti. */
        /* --- oynatma imlecini oyunun YAZMA imlecine kilitle (PLL) ---
         * Serbest calisan imlec, %1'lik bir hiz farkiyla bile tampon
         * boyunca yavasca kayar ve her turda yazilmamis bolgeden bir kez
         * gecer.  Olculdu: tam 30 sn'de bir 50-95 ms sessizlik; hiz orani
         * 0.99 iken bile.  Periyot = tampon_suresi / hiz_hatasi, yani
         * ileri-besleme hatayi kucultur ama SIFIRLAMAZ.
         * Cozum: tuketim hizini, yazili veri miktarini (avail) sabit
         * tutacak sekilde surekli duzelt.  Hedef, oyunun kendi dogal
         * denge noktasi olsun -- sabit bir sayi secmek yanlis olur
         * (once 0.25 secilmisti, oyun 0.30-0.64 arasinda calisiyor). */
        if (b->wseen){
            uint32_t bpf2 = b->chans * (b->bits/8u);
            uint32_t pbyte = ((uint32_t)(b->cursor * (double)bpf2)) % b->bytes;
            uint32_t avail = (b->wpos + b->bytes - pbyte) % b->bytes;
            double frac = (double)avail / (double)b->bytes;
            double want;
            if (frac > 0.85){
                /* imlec yaziciyi GECTI (avail modulo sarmasiyla buyuk
                 * gorunuyor).  Hedefe dogru servo yapmak yerine sert
                 * yavasla ve hedefi bozma. */
                b->over++;
                want = 0.94;
            } else {
                if (b->target <= 0.0) b->target = frac;
                else b->target += (frac - b->target) * 0.0008;   /* cok yavas */
                if (b->target < 0.12) b->target = 0.12;
                if (b->target > 0.60) b->target = 0.60;
                /* Kp kucuk + kelepce dar: perde degisimi 3 sentin altinda
                 * kalir, yani duyulmaz; ama kayma tamamen yok olur. */
                want = 1.0 + (frac - b->target) * 1.5;
                if (want < 0.97) want = 0.97;
                if (want > 1.03) want = 1.03;
            }
            b->trim += (want - b->trim) * 0.05;
        } else b->trim += (1.0 - b->trim) * 0.08;
        if (b->trim < 0.5 || b->trim > 2.0) b->trim = 1.0;
        if (g_csv){
            uint32_t bpf3 = b->chans * (b->bits/8u);
            uint32_t pb = ((uint32_t)(b->cursor * (double)bpf3)) % b->bytes;
            uint32_t av = b->wseen ? ((b->wpos + b->bytes - pb) % b->bytes) : 0u;
            fprintf(g_csv, "%llu,%.4f,%d,%u,%u,%u,%u,%u,%.4f,%.4f,%.4f,%.4f,",
                    (unsigned long long)g_cb_no,
                    (double)g_mixed_frames/(double)OUT_RATE, i, b->rate, b->bytes,
                    pb, b->wpos, av, (double)av/(double)b->bytes,
                    b->target, b->trim, g_rate_ratio);
        }
        double step = (double)b->rate / (double)OUT_RATE * g_rate_ratio * b->trim * b->freq;
        active++;
        for (unsigned f = 0; f < frames; f++){
            double sp = b->cursor + step * (double)f;
            if (sp >= (double)nsrc){
                if (b->loop) sp = fmod(sp, (double)nsrc);
                else { b->playing = 0; break; }
            }
            uint32_t si = (uint32_t)sp;
            for (int c = 0; c < OUT_CH; c++){
                uint32_t sc = (b->chans == 2u) ? (uint32_t)c : 0u;
                uint32_t off = b->data + si*bpf + sc*bps;
                float s;
                if (b->bits == 16u){
                    int16_t v; memcpy(&v, g_mem + off, 2);
                    s = (float)v / 32768.f;
                } else {
                    s = ((float)g_mem[off] - 128.f) / 128.f;
                }
                acc[f*OUT_CH + c] += s * b->vol;
            }
        }
        if (g_csv){
            /* bu tamponun OKUDUGU kaynak bolgesinde kac bayt sifir? */
            uint32_t bpf4 = b->chans * (b->bits/8u);
            uint32_t p0 = ((uint32_t)(b->cursor * (double)bpf4)) % b->bytes;
            uint32_t nby = (uint32_t)(step * (double)frames * (double)bpf4);
            if (nby > b->bytes) nby = b->bytes;
            uint32_t z = 0;
            for (uint32_t q = 0; q < nby; q++)
                if (!g_mem[b->data + ((p0 + q) % b->bytes)]) z++;
            fprintf(g_csv, "%.4f\n", nby ? (double)z/(double)nby : 0.0);
        }
        b->cursor += step * (double)frames;
        if (nsrc){
            if (b->cursor >= (double)nsrc){
                if (b->loop) b->cursor = fmod(b->cursor, (double)nsrc);
                else { b->cursor = 0.0; b->playing = 0; }
            }
        }
    }
    pthread_mutex_unlock(&g_slock);
    if (active > g_active_peak) g_active_peak = active;
    g_mixed_frames += frames;
    /* Periyodik durum: hangi tampon caliyor, dolu mu? */
    if (getenv("SNDSTAT")){
        static uint64_t next = 0;
        if (g_mixed_frames >= next){
            next = g_mixed_frames + (uint64_t)OUT_RATE * 2ull;
            pthread_mutex_lock(&g_slock);
            fprintf(stderr, "  [durum] ");
            for (int i2 = 1; i2 < g_sndn; i2++){
                if (!g_snd[i2].used) continue;
                uint32_t nz = 0, lim = g_snd[i2].bytes < 8192u ? g_snd[i2].bytes : 8192u;
                for (uint32_t q = 0; q < lim; q++) if (g_mem[g_snd[i2].data + q]) nz++;
                fprintf(stderr, "#%d[%s%s %uHz veri=%u%%] ", i2,
                        g_snd[i2].playing ? "cal" : "dur",
                        g_snd[i2].loop ? "+dongu" : "",
                        g_snd[i2].rate, lim ? (unsigned)(100u*nz/lim) : 0u);
            }
            fprintf(stderr, "\n");
            pthread_mutex_unlock(&g_slock);
        }
    }

    /* Okudugumuz veri gercekten sessiz mi?  Sessizse oyun o bolgeyi
     * HENUZ YAZMAMIS demektir (bizim imlec yaziciyi geciyor). */
    if (active){
        unsigned zero = 0;
        for (unsigned k = 0; k < frames * OUT_CH; k++) if (acc[k] == 0.f) zero++;
        g_out_frames += frames * OUT_CH;
        g_out_zero   += zero;
        if (zero > (frames * OUT_CH) * 3u / 4u){
            g_gap_cb++;
            static int shown = 0;
            if (getenv("SNDGAP") && shown < 40 &&
                g_mixed_frames > (uint64_t)OUT_RATE * 5ull){ shown++;
                fprintf(stderr, "  [bosluk] t=%.2f sn\n",
                        (double)g_mixed_frames / (double)OUT_RATE);
            }
            if (0){
                fprintf(stderr, "  [bosluk] calan tamponlar:");
                pthread_mutex_lock(&g_slock);
                for (int i2 = 1; i2 < g_sndn; i2++)
                    if (g_snd[i2].used && g_snd[i2].playing)
                        fprintf(stderr, " #%d(imlec=%.0f/%u dongu=%d)", i2,
                                g_snd[i2].cursor,
                                g_snd[i2].bytes / (g_snd[i2].chans*(g_snd[i2].bits/8u)),
                                g_snd[i2].loop);
                pthread_mutex_unlock(&g_slock);
                fprintf(stderr, "\n");
            }
        }
    }
    for (unsigned k = 0; k < frames * OUT_CH; k++){
        float v = acc[k];
        if (v >  1.f) v =  1.f;
        if (v < -1.f) v = -1.f;
        out[k] = (int16_t)(v * 32767.f);
    }
    g_cb_no++;
    if (g_dump) fwrite(out, sizeof(int16_t), (size_t)frames * OUT_CH, g_dump);
}

static uint64_t now_us_a(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return (uint64_t)ts.tv_sec*1000000ull + (uint64_t)ts.tv_nsec/1000ull;
}
static void cb(void *ud, AudioQueueRef q, AudioQueueBufferRef buf){
    (void)ud;
    /* Geri cagirmalar arasi sure tampon suresini asarsa kuyruk BOSALMIS,
     * yani ses kesilmis demektir.  Olculuyor. */
    { static uint64_t last = 0;
      uint64_t now = now_us_a();
      uint64_t budget = (uint64_t)FRAMES * 1000000ull / (uint64_t)OUT_RATE;
      if (last && now - last > budget * (uint64_t)NBUF) g_starved++;
      if (last){ uint64_t d = now - last; if (d > g_worst_gap_us) g_worst_gap_us = d; }
      last = now; }
    mix_into((int16_t *)buf->mAudioData, FRAMES);
    buf->mAudioDataByteSize = FRAMES * OUT_CH * sizeof(int16_t);
    AudioQueueEnqueueBuffer(q, buf, 0, NULL);
}

/* AudioUnit render geri cagirmasi.  AudioQueue yolundan tek farki, ciktinin
 * hazir bir kuyruk tamponuna degil dogrudan aygitin ABL'sine yazilmasi. */
static OSStatus au_cb(void *ud, AudioUnitRenderActionFlags *flags,
                      const AudioTimeStamp *ts, UInt32 bus, UInt32 nframes,
                      AudioBufferList *abl){
    (void)ud; (void)flags; (void)ts; (void)bus;
    { static uint64_t last = 0;
      uint64_t now = now_us_a();
      if (last){ uint64_t d = now - last; if (d > g_worst_gap_us) g_worst_gap_us = d; }
      last = now; }
    if (!abl || abl->mNumberBuffers < 1) return noErr;
    int16_t *out = (int16_t *)abl->mBuffers[0].mData;
    if (!out) return noErr;
    if (nframes > (UInt32)FRAMES) nframes = (UInt32)FRAMES;   /* acc dizisi siniri */
    if (nframes > g_au_slice) g_au_slice = nframes;
    mix_into(out, (unsigned)nframes);
    abl->mBuffers[0].mDataByteSize = nframes * OUT_CH * sizeof(int16_t);
    return noErr;
}

/* AudioQueue 6 x 2048 kare kuyruklar: 278.6 ms.  Bu, Windows'ta OLMAYAN,
 * tamamen bizim ekledigimiz gecikmeydi ve oyunun kendi DirectSound
 * gecikmesinin (~275 ms) uzerine biniyordu.  AudioUnit aygitin kendi
 * dilimiyle (genelde 512 kare = 11.6 ms) calisir. */
static int au_start(const AudioStreamBasicDescription *f){
    AudioComponentDescription d;
    memset(&d, 0, sizeof d);
    d.componentType         = kAudioUnitType_Output;
    d.componentSubType      = kAudioUnitSubType_DefaultOutput;
    d.componentManufacturer = kAudioUnitManufacturer_Apple;
    AudioComponent comp = AudioComponentFindNext(NULL, &d);
    if (!comp) return 0;
    if (AudioComponentInstanceNew(comp, &g_au) != noErr) return 0;
    if (AudioUnitSetProperty(g_au, kAudioUnitProperty_StreamFormat,
                             kAudioUnitScope_Input, 0, f, sizeof *f) != noErr) goto fail;
    { UInt32 mx = FRAMES;
      AudioUnitSetProperty(g_au, kAudioUnitProperty_MaximumFramesPerSlice,
                           kAudioUnitScope_Global, 0, &mx, sizeof mx); }
    { AURenderCallbackStruct rc; rc.inputProc = au_cb; rc.inputProcRefCon = NULL;
      if (AudioUnitSetProperty(g_au, kAudioUnitProperty_SetRenderCallback,
                               kAudioUnitScope_Input, 0, &rc, sizeof rc) != noErr) goto fail; }
    if (AudioUnitInitialize(g_au) != noErr) goto fail;
    if (AudioOutputUnitStart(g_au) != noErr){ AudioUnitUninitialize(g_au); goto fail; }
    return 1;
fail:
    AudioComponentInstanceDispose(g_au); g_au = NULL; return 0;
}

void audio_init(void){
    if (g_audio_on) return;
    { const char *e = getenv("NOSOUND"); if (e && *e && *e != '0') return; }
    { const char *d = getenv("SNDDUMP");
      if (d && *d){ g_dump = fopen(d, "wb");
          fprintf(stderr, "ses: cikis dokumu -> %s (%d Hz s16le %d kanal)\n", d, OUT_RATE, OUT_CH); } }
    { const char *d = getenv("SNDCSV");
      if (d && *d){ g_csv = fopen(d, "w");
          if (g_csv) fprintf(g_csv, "cb,t,id,rate,bytes,pbyte,wpos,avail,frac,target,trim,ratio,zero\n"); } }
    AudioStreamBasicDescription f;
    memset(&f, 0, sizeof f);
    f.mSampleRate       = OUT_RATE;
    f.mFormatID         = kAudioFormatLinearPCM;
    f.mFormatFlags      = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    f.mChannelsPerFrame = OUT_CH;
    f.mBitsPerChannel   = 16;
    f.mFramesPerPacket  = 1;
    f.mBytesPerFrame    = OUT_CH * sizeof(int16_t);
    f.mBytesPerPacket   = f.mBytesPerFrame;
    /* SNDBACKEND=queue ile eski (yuksek gecikmeli) yola donulebilir. */
    { const char *e = getenv("SNDBACKEND");
      if (!(e && *e == 'q') && au_start(&f)){
          g_backend = 1; g_audio_on = 1;
          fprintf(stderr, "ses: CoreAudio cikisi acildi (AudioUnit, %d Hz, %d kanal)\n",
                  OUT_RATE, OUT_CH);
          return;
      } }
    if (AudioQueueNewOutput(&f, cb, NULL, NULL, NULL, 0, &g_q) != noErr){
        fprintf(stderr, "ses: AudioQueue acilamadi\n"); return;
    }
    for (int i = 0; i < NBUF; i++){
        if (AudioQueueAllocateBuffer(g_q, FRAMES*OUT_CH*sizeof(int16_t), &g_qb[i]) != noErr)
            continue;
        memset(g_qb[i]->mAudioData, 0, FRAMES*OUT_CH*sizeof(int16_t));
        g_qb[i]->mAudioDataByteSize = FRAMES*OUT_CH*sizeof(int16_t);
        AudioQueueEnqueueBuffer(g_q, g_qb[i], 0, NULL);
    }
    AudioQueueSetParameter(g_q, kAudioQueueParam_Volume, 1.0f);
    if (AudioQueueStart(g_q, NULL) != noErr){
        fprintf(stderr, "ses: AudioQueue baslatilamadi\n"); return;
    }
    g_audio_on = 1; g_backend = 2;
    fprintf(stderr, "ses: CoreAudio cikisi acildi (AudioQueue, %d Hz, %d kanal)\n",
            OUT_RATE, OUT_CH);
}

void audio_report(void){
    if (g_dump){ fflush(g_dump); fclose(g_dump); g_dump = NULL; }
    if (g_csv){  fflush(g_csv);  fclose(g_csv);  g_csv  = NULL; }
    fprintf(stderr, "  ses cikisi        : %s, %llu kare karistirildi, "
            "en fazla %llu es zamanli tampon, %d kayitli\n",
            g_audio_on ? "acik" : "KAPALI",
            (unsigned long long)g_mixed_frames,
            (unsigned long long)g_active_peak, g_sndn - 1);
    fprintf(stderr, "  ses Play cagrisi  : %llu bastan basladi, %llu zaten caliyordu\n",
            (unsigned long long)g_replay, (unsigned long long)g_reissue);
    fprintf(stderr, "  ses hiz orani    : %.3f (oyun sanal zamani / gercek zaman)%s\n",
            g_rate_ratio, g_sync_mode ? "" : "  [SNDSYNC=0: kapali]");
    fprintf(stderr, "  ses veri boslugu  : ornek %.1f%% sessiz, %llu geri-cagirmanin cogu bos\n",
            g_out_frames ? 100.0*(double)g_out_zero/(double)g_out_frames : 0.0,
            (unsigned long long)g_gap_cb);
    fprintf(stderr, "  --- ses tamponu basina ---\n");
    for (int i = 1; i < g_sndn; i++){
        SndBuf *b = &g_snd[i];
        if (!b->used) continue;
        fprintf(stderr, "   #%-3d %6u Hz %uch %5.2f sn  Play=%-4llu Stop=%-4llu  "
                "yazma=%llu (%llu dolu, %.1f KB)  %s\n",
                i, b->rate, b->chans,
                (double)b->bytes/(double)(b->rate*b->chans*(b->bits/8u)),
                (unsigned long long)b->plays, (unsigned long long)b->stops,
                (unsigned long long)b->wr_calls, (unsigned long long)b->wr_nonzero,
                b->wr_bytes/1024.0, b->playing ? "CALIYOR" : "durdu");
    }
    { uint64_t ov = 0; for (int i = 1; i < g_sndn; i++) ov += g_snd[i].over;
      fprintf(stderr, "  ses imlec asimi   : %llu geri-cagirmada oynatma yaziciyi gecti\n",
              (unsigned long long)ov); }
    fprintf(stderr, "  ses cikis yolu    : %s, geri cagirma %u kare (%.1f ms kuyruk gecikmesi)\n",
            g_backend == 1 ? "AudioUnit" : g_backend == 2 ? "AudioQueue" : "yok",
            g_backend == 1 ? g_au_slice : (unsigned)(FRAMES*NBUF),
            (g_backend == 1 ? (double)g_au_slice : (double)(FRAMES*NBUF))
                * 1000.0 / (double)OUT_RATE);
    fprintf(stderr, "  ses kesilmesi     : %llu aclik, en uzun geri-cagirma araligi %.1f ms"
            " (tampon %.1f ms)\n",
            (unsigned long long)g_starved, g_worst_gap_us/1000.0,
            (double)FRAMES*NBUF*1000.0/(double)OUT_RATE);
}
