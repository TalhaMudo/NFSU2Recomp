/* CoreAudio output.
 *
 * Until now the DirectSound layer only did bookkeeping (written bytes were
 * counted, the play cursor advanced with elapsed time) but there was NO SOUND
 * OUTPUT AT ALL.  The game writes real PCM (measured: ~9 MB, 5000+ non-empty
 * writes); here we mix those buffers and play them with AudioQueue.
 *
 * The mixer converts to 44.1 kHz, 2-channel, 16-bit output; source buffers may
 * have different rates/channels (nearest-neighbor resampling). */
#import <AudioToolbox/AudioToolbox.h>
#import <AudioUnit/AudioUnit.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <math.h>

extern uint8_t *g_mem;
uint64_t host_us(void);          /* the game's VIRTUAL clock (16.67 ms per frame) */

/* The game produces audio data in its own virtual time; the mixer consumed it
 * in real time.  When the emulator stays below 60 fps, virtual time falls
 * behind and on every buffer lap an unwritten region is left -> the sound cuts
 * out at regular intervals (measured: the gap period is exactly the buffer
 * duration).  Solution: tie consumption to the game's production rate. */
static double g_rate_ratio = 1.0;
static int    g_sync_mode = -1;      /* SNDSYNC: 1 = tie to the game's speed */
static void update_rate_ratio(void){
    if (g_sync_mode < 0){
        const char *e = getenv("SNDSYNC");
        g_sync_mode = (e && *e) ? (*e != '0') : 1;      /* default: on */
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
            /* CEILING 1.0: sound must not play FASTER THAN REAL TIME.  This ratio
             * came from the game's clock, and the clock (because of the creep
             * bug) ran up to 2x fast in places -> songs played fast.  The clock
             * was fixed, but we make the link safe too: through this path a clock
             * bug can never again turn into a PITCH bug. */
            if (k > 1.00) k = 1.00;
            /* smooth: so sudden frame rate fluctuations don't make the pitch jump */
            g_rate_ratio += (k - g_rate_ratio) * 0.10;
        }
    }
    lv = vus; lr = rus;
}

#define OUT_RATE   44100
#define OUT_CH     2
/* Since the emulator fills the CPU, 3x1024 (~70 ms) of buffering wasn't enough;
 * when a callback was late the sound cut out from queue starvation. */
#define NBUF       6
#define FRAMES     2048

#define MAXSND 128
typedef struct {
    uint32_t data, bytes, rate, chans, bits;
    int      used, playing, loop;
    double   cursor;          /* in source samples */
    float    vol;             /* 0..1 */
    uint32_t wpos;            /* end of the last byte the game wrote */
    int      wseen;           /* have we seen any write at all */
    double   trim;            /* rate correction (servo) */
    double   target;          /* fill ratio to lock to (adaptive) */
    double   freq;            /* SetFrequency ratio (1.0 = original rate) */
    uint64_t plays, stops, wr_bytes, wr_calls, wr_nonzero;
    uint64_t over;            /* number of callbacks where we passed the writer */
    /* STALE DATA GUARD (see audio_note_region): which 256-byte blocks hold data
     * the game wrote and we have not played yet.  Only on streaming buffers. */
    uint8_t *fresh;
    uint32_t wr_play;         /* writes since this Play started (>= 8: a streaming buffer) */
    uint64_t muted;           /* output frames muted because the data was stale */
    uint32_t lastblk;         /* block the cursor was in at the end of the last callback */
} SndBuf;
#define FRESH_SH 8u           /* block = 256 bytes (~1.5 ms of 44.1 kHz stereo 16-bit) */

static SndBuf g_snd[MAXSND];
static int    g_sndn = 1;                    /* 0 = invalid id */
static pthread_mutex_t g_slock = PTHREAD_MUTEX_INITIALIZER;
static AudioQueueRef        g_q;
static AudioQueueBufferRef  g_qb[NBUF];
static AudioUnit            g_au;            /* low-latency path */
static int g_audio_on = 0;
static int g_backend = 0;                    /* 1 = AudioUnit, 2 = AudioQueue */
static unsigned g_au_slice = 0;              /* frames per callback */
static uint64_t g_mixed_frames = 0, g_active_peak = 0;
static uint64_t g_replay = 0, g_reissue = 0, g_starved = 0, g_worst_gap_us = 0;
static uint64_t g_out_frames = 0, g_out_zero = 0, g_gap_cb = 0;

/* --- diagnostics: dump the mixer output and buffer state to disk --- */
static FILE *g_dump = NULL;      /* SNDDUMP=<path> : raw s16le stereo */
static FILE *g_csv  = NULL;      /* SNDCSV=<path>  : state per callback */
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
        b->fresh = calloc((bytes >> FRESH_SH) + 1u, 1); b->wr_play = 0; b->muted = 0; b->lastblk = 0xFFFFFFFFu;
        fprintf(stderr, "  [sound] buffer #%d: %u Hz, %u channels, %u bits, %u bytes (%.2f s)\n",
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
        /* CAUTION: DON'T RESET the cursor on a buffer that is already playing.  The game
         * calls Play over and over on its streaming buffers; going back to the
         * start on every call made the song stutter "stop-start". */
        if (!g_snd[id].playing){ g_snd[id].cursor = 0.0; g_snd[id].wr_play = 0; g_replay++; }
        else g_reissue++;
        g_snd[id].playing = 1;
        g_snd[id].loop = loop;
        g_snd[id].plays++;
    }
    pthread_mutex_unlock(&g_slock);
}
/* From the guest's Unlock: the END offset of the written region.  The mixer
 * keeps the play cursor a fixed distance behind it (servo) - so neither an
 * unwritten region is played (gap) nor does the cursor fall behind (latency). */
void audio_note_write(int id, uint32_t end_off){
    if (id <= 0 || id >= MAXSND) return;
    pthread_mutex_lock(&g_slock);
    if (g_snd[id].used && g_snd[id].bytes){
        g_snd[id].wpos = end_off % g_snd[id].bytes;
        g_snd[id].wseen = 1;
    }
    pthread_mutex_unlock(&g_slock);
}
/* ---- STALE DATA GUARD --------------------------------------------------------
 * The game streams music (and the mixed sound) into a looping 0.5 s buffer,
 * writing what we consumed and keeping ~180 ms ahead (measured).  If it stops
 * writing for a while (a stall: slow disk, a long frame), the play cursor -- which
 * has to keep moving, the game picks where to write from it -- goes round the ring
 * and plays the SAME old half second again and again: the "broken record, it
 * jumps back for two seconds" glitch reported from menus.  Windows does the same
 * with a looping buffer, the game just never stalls there.  Now every written
 * region is marked fresh, a block is unmarked once played, and an unmarked block
 * of a streaming buffer plays as silence: a stall becomes a short gap instead of
 * a repeat.  The cursor keeps moving exactly as before (no change for the game).
 * Static buffers (written once, then looped: engine sounds) are not streaming
 * (fewer than 8 writes since Play) and loop as before.  SNDSTALE=0 disables. */
static int stale_guard_on(void){
    static int v = -1; if (v < 0){ const char *e = getenv("SNDSTALE"); v = !(e && *e == '0'); }
    return v;
}
void audio_note_region(int id, uint32_t off, uint32_t n){
    if (id <= 0 || id >= MAXSND || !n) return;
    pthread_mutex_lock(&g_slock);
    SndBuf *b = &g_snd[id];
    if (b->used && b->bytes && b->fresh){
        if (off >= b->bytes) off %= b->bytes;
        if (n > b->bytes) n = b->bytes;
        uint32_t end = off + n;                           /* may run past the end: wrap */
        for (uint32_t q = off >> FRESH_SH; (q << FRESH_SH) < end; q++)
            b->fresh[q % ((b->bytes >> FRESH_SH) + 1u)] = 1;
        if (b->playing) b->wr_play++;
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
/* DirectSound SetFrequency: changes the playback sample rate.
 * The game adjusts the PITCH of effects like the engine sound this way; without
 * it every sound plays at its original rate. */
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
/* A second sound sharing the same PCM data (DuplicateSoundBuffer).
 * The game plays SIMULTANEOUS effects this way; when the duplicate returned
 * empty, they all stayed silent. */
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
/* For the guest's GetCurrentPosition.
 * DirectSound returns two cursors: PLAY and WRITE.  The write cursor is AHEAD
 * of the play cursor (the safe limit the hardware is reading); the game writes
 * data from there on.  Returning both as equal made the game write exactly at
 * the point we were playing, and the sound stuttered "stop-start". */
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
/* The mixer's time axis (the same t as the SNDDUMP file).  For aligning
 * diagnostic records with the audio dump. */
double audio_time(void){ return (double)g_mixed_frames / (double)OUT_RATE; }
uint32_t audio_cursor(int id){ uint32_t p; audio_cursors(id, &p, 0); return p; }
void audio_set_volume(int id, long millibel){
    if (id <= 0 || id >= MAXSND) return;
    /* DirectSound: 0 = full volume, -10000 = silent (hundredths of a dB) */
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
        /* ONE-WAY servo: the rate ratio (feedforward) already balances production and
         * consumption.  The servo only kicks in WHEN THE WRITTEN DATA AHEAD OF US
         * SHRINKS and slows down; so an unwritten region is never played.
         * A two-way servo had made things worse when its target was picked wrong. */
        /* --- lock the play cursor to the game's WRITE cursor (PLL) ---
         * A free-running cursor drifts slowly through the buffer even with a 1%
         * rate difference, and passes through the unwritten region once per lap.
         * Measured: 50-95 ms of silence exactly every 30 s, even with the rate
         * ratio at 0.99.  Period = buffer_duration / rate_error, so feedforward
         * shrinks the error but doesn't ZERO it.
         * Solution: keep correcting the consumption rate so the amount of written
         * data (avail) stays constant.  The target should be the game's own
         * natural equilibrium -- picking a fixed number would be wrong (0.25 was
         * picked first, the game runs between 0.30 and 0.64). */
        /* SERVO OFF BY DEFAULT (2026-10-22).  The game's stream chooses where it writes
         * based on the REAL play cursor we report (com.c GetCurrentPosition ->
         * audio_cursors): when we slow playback down, the game produces the music
         * just as much slower, avail (the write lead) stays independent of the
         * rate and the servo can never close its error.  Measured (SNDMUTE +
         * SNDCSV, Quick Race 3 min): after every load / stutter the lead settles
         * at a new level (0.19 / 0.52 / 0.40), the servo hit the clamp (3%) and
         * stayed there for 40-50 s -- the music 3% slow for 50 s (half a semitone
         * flat, the user: "from that moment the music flows slower"), rate
         * deviation > 1% in 33% of the calls.  The 30 s gaps the servo targeted are
         * written by the game itself (see SNDZERO); the servo never fixed them
         * anyway.  SNDPLL=1 is the old servo. */
        static int pll = -1;
        if (pll < 0){ const char *e = getenv("SNDPLL"); pll = e && *e == '1'; }
        if (!pll){
            if (b->wseen){
                uint32_t bpf2 = b->chans * (b->bits/8u);
                uint32_t pbyte = ((uint32_t)(b->cursor * (double)bpf2)) % b->bytes;
                uint32_t avail = (b->wpos + b->bytes - pbyte) % b->bytes;
                int ov_now = (double)avail / (double)b->bytes > 0.85;
                if (ov_now) b->over++;   /* counter only */
                /* Log the START of each overrun (audible as a click / short gap): the
                 * mixer time, the buffer, and how long the game had not written. */
                { static int ov_prev[MAXSND]; static int ov_logged = 0;
                  int bi = (int)(b - g_snd);
                  if (bi > 0 && bi < MAXSND){
                      if (ov_now && !ov_prev[bi] && ov_logged < 300){ ov_logged++;
                          fprintf(stderr, "  [sound] overrun t=%.2f s buffer #%d (%u bytes, %.0f ms)\n",
                                  audio_time(), bi, b->bytes,
                                  1000.0 * (double)b->bytes / (double)(b->rate * b->chans * (b->bits/8u))); }
                      ov_prev[bi] = ov_now; } }
            }
            b->trim = 1.0;
        } else
        if (b->wseen){
            uint32_t bpf2 = b->chans * (b->bits/8u);
            uint32_t pbyte = ((uint32_t)(b->cursor * (double)bpf2)) % b->bytes;
            uint32_t avail = (b->wpos + b->bytes - pbyte) % b->bytes;
            double frac = (double)avail / (double)b->bytes;
            double want;
            if (frac > 0.85){
                /* the cursor PASSED the writer (looks large because of avail's modulo
                 * wrap).  Instead of servoing toward the target, slow down hard
                 * and don't disturb the target. */
                b->over++;
                want = 0.94;
            } else {
                if (b->target <= 0.0) b->target = frac;
                else b->target += (frac - b->target) * 0.0008;   /* very slow */
                if (b->target < 0.12) b->target = 0.12;
                if (b->target > 0.60) b->target = 0.60;
                /* small Kp + a narrow clamp: the pitch change stays under 3 cents, i.e.
                 * inaudible; but the drift disappears completely. */
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
        /* see STALE DATA GUARD */
        int guard = b->fresh && b->loop && b->wr_play >= 8u && stale_guard_on();
        uint32_t nblk = (b->bytes >> FRESH_SH) + 1u, lastblk = b->lastblk;
        for (unsigned f = 0; f < frames; f++){
            double sp = b->cursor + step * (double)f;
            if (sp >= (double)nsrc){
                if (b->loop) sp = fmod(sp, (double)nsrc);
                else { b->playing = 0; break; }
            }
            uint32_t si = (uint32_t)sp;
            if (guard){
                uint32_t blk = (si * bpf) >> FRESH_SH;
                if (blk != lastblk){                       /* leaving a block: it is played */
                    if (lastblk != 0xFFFFFFFFu) b->fresh[lastblk % nblk] = 0;
                    lastblk = blk; }
                if (!b->fresh[blk % nblk]){ b->muted++; continue; }   /* stale: silence */
            }
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
            /* how many bytes are zero in the source region this buffer READS? */
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
        b->lastblk = guard ? lastblk : 0xFFFFFFFFu;
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
    /* Periodic state: which buffer is playing, is it full? */
    if (getenv("SNDSTAT")){
        static uint64_t next = 0;
        if (g_mixed_frames >= next){
            next = g_mixed_frames + (uint64_t)OUT_RATE * 2ull;
            pthread_mutex_lock(&g_slock);
            fprintf(stderr, "  [state] ");
            for (int i2 = 1; i2 < g_sndn; i2++){
                if (!g_snd[i2].used) continue;
                uint32_t nz = 0, lim = g_snd[i2].bytes < 8192u ? g_snd[i2].bytes : 8192u;
                for (uint32_t q = 0; q < lim; q++) if (g_mem[g_snd[i2].data + q]) nz++;
                fprintf(stderr, "#%d[%s%s %uHz veri=%u%%] ", i2,
                        g_snd[i2].playing ? "play" : "stop",
                        g_snd[i2].loop ? "+loop" : "",
                        g_snd[i2].rate, lim ? (unsigned)(100u*nz/lim) : 0u);
            }
            fprintf(stderr, "\n");
            pthread_mutex_unlock(&g_slock);
        }
    }

    /* Is the data we read really silent?  If it is, the game HASN'T WRITTEN that
     * region YET (our cursor is passing the writer). */
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
                fprintf(stderr, "  [gap] t=%.2f s\n",
                        (double)g_mixed_frames / (double)OUT_RATE);
            }
            if (0){
                fprintf(stderr, "  [gap] playing buffers:");
                pthread_mutex_lock(&g_slock);
                for (int i2 = 1; i2 < g_sndn; i2++)
                    if (g_snd[i2].used && g_snd[i2].playing)
                        fprintf(stderr, " #%d(cursor=%.0f/%u loop=%d)", i2,
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
    /* If the time between callbacks exceeds the buffer duration, the queue has
     * DRAINED, i.e. the sound cut out.  It is being measured. */
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

/* AudioUnit render callback.  The only difference from the AudioQueue path is
 * that the output is written directly to the device's ABL instead of a ready
 * queue buffer. */
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
    if (nframes > (UInt32)FRAMES) nframes = (UInt32)FRAMES;   /* acc array bound */
    if (nframes > g_au_slice) g_au_slice = nframes;
    mix_into(out, (unsigned)nframes);
    /* SNDMUTE=1 (diagnostic): mixing, servo and timing run exactly the same, silence goes to the device */
    { static int mute = -1; if (mute < 0){ const char *e = getenv("SNDMUTE"); mute = e && *e && *e != '0'; }
      if (mute) memset(out, 0, (size_t)nframes * OUT_CH * sizeof(int16_t)); }
    abl->mBuffers[0].mDataByteSize = nframes * OUT_CH * sizeof(int16_t);
    return noErr;
}

/* AudioQueue queues 6 x 2048 frames: 278.6 ms.  This was latency that DOESN'T
 * EXIST on Windows, added entirely by us, and it stacked on top of the game's
 * own DirectSound latency (~275 ms).  AudioUnit runs with the device's own
 * slice (usually 512 frames = 11.6 ms). */
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
          fprintf(stderr, "sound: output dump -> %s (%d Hz s16le %d channels)\n", d, OUT_RATE, OUT_CH); } }
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
    /* SNDBACKEND=queue goes back to the old (high-latency) path. */
    { const char *e = getenv("SNDBACKEND");
      if (!(e && *e == 'q') && au_start(&f)){
          g_backend = 1; g_audio_on = 1;
          fprintf(stderr, "sound: CoreAudio output opened (AudioUnit, %d Hz, %d channels)\n",
                  OUT_RATE, OUT_CH);
          return;
      } }
    if (AudioQueueNewOutput(&f, cb, NULL, NULL, NULL, 0, &g_q) != noErr){
        fprintf(stderr, "sound: could not open AudioQueue\n"); return;
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
        fprintf(stderr, "sound: could not start AudioQueue\n"); return;
    }
    g_audio_on = 1; g_backend = 2;
    fprintf(stderr, "sound: CoreAudio output opened (AudioQueue, %d Hz, %d channels)\n",
            OUT_RATE, OUT_CH);
}

void audio_report(void){
    if (g_dump){ fflush(g_dump); fclose(g_dump); g_dump = NULL; }
    if (g_csv){  fflush(g_csv);  fclose(g_csv);  g_csv  = NULL; }
    fprintf(stderr, "  sound output      : %s, %llu frames mixed, "
            "at most %llu simultaneous buffers, %d registered\n",
            g_audio_on ? "on" : "OFF",
            (unsigned long long)g_mixed_frames,
            (unsigned long long)g_active_peak, g_sndn - 1);
    fprintf(stderr, "  sound Play calls  : %llu started from the beginning, %llu were already playing\n",
            (unsigned long long)g_replay, (unsigned long long)g_reissue);
    fprintf(stderr, "  sound rate ratio  : %.3f (game virtual time / real time)%s\n",
            g_rate_ratio, g_sync_mode ? "" : "  [SNDSYNC=0: off]");
    fprintf(stderr, "  sound data gaps   : %.1f%% of samples silent, %llu callbacks mostly empty\n",
            g_out_frames ? 100.0*(double)g_out_zero/(double)g_out_frames : 0.0,
            (unsigned long long)g_gap_cb);
    fprintf(stderr, "  --- per sound buffer ---\n");
    for (int i = 1; i < g_sndn; i++){
        SndBuf *b = &g_snd[i];
        if (!b->used) continue;
        fprintf(stderr, "   #%-3d %6u Hz %uch %5.2f sn  Play=%-4llu Stop=%-4llu  "
                "writes=%llu (%llu non-empty, %.1f KB)  %s\n",
                i, b->rate, b->chans,
                (double)b->bytes/(double)(b->rate*b->chans*(b->bits/8u)),
                (unsigned long long)b->plays, (unsigned long long)b->stops,
                (unsigned long long)b->wr_calls, (unsigned long long)b->wr_nonzero,
                b->wr_bytes/1024.0, b->playing ? "PLAYING" : "stopped");
    }
    { uint64_t mu = 0; for (int i = 1; i < g_sndn; i++) mu += g_snd[i].muted;
      fprintf(stderr, "  sound stale guard : %s, %.1f ms of not-yet-rewritten data muted instead of replayed\n",
              stale_guard_on() ? "on" : "off (SNDSTALE=0)", (double)mu * 1000.0 / (double)OUT_RATE); }
    { uint64_t ov = 0; for (int i = 1; i < g_sndn; i++) ov += g_snd[i].over;
      fprintf(stderr, "  sound cursor overrun: %llu callbacks where playback passed the writer\n",
              (unsigned long long)ov); }
    fprintf(stderr, "  sound output path : %s, callback %u frames (%.1f ms queue latency)\n",
            g_backend == 1 ? "AudioUnit" : g_backend == 2 ? "AudioQueue" : "none",
            g_backend == 1 ? g_au_slice : (unsigned)(FRAMES*NBUF),
            (g_backend == 1 ? (double)g_au_slice : (double)(FRAMES*NBUF))
                * 1000.0 / (double)OUT_RATE);
    fprintf(stderr, "  sound dropouts    : %llu starvations, longest callback interval %.1f ms"
            " (buffer %.1f ms)\n",
            (unsigned long long)g_starved, g_worst_gap_us/1000.0,
            (double)FRAMES*NBUF*1000.0/(double)OUT_RATE);
}
