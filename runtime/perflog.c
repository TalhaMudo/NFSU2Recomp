/* PERFLOG=<dizin>: GERCEK oyun oturumu kaydi (kullanici oynarken).
 *
 * Otomatik test kosulari (AUTOSEQ yarisi) birebir karsilastirma icin; bu kayit
 * ise kullanicinin kendi oynadigi oturumda (hizli surus, efektler, kaza, menu)
 * yukun NEREDE ve NE ZAMAN olustugunu gormek icin.  Dusuk yuklu:
 *
 *   konuk.csv   konuk kare basina: duvar/CPU suresi, kare sinirlayici uykusu
 *               (bos pay), Present beklemesi, COM/Win32 cagrilari, derleme ve
 *               doku olaylari
 *   render.csv  ana thread kare basina: kare araligi, nextDrawable beklemesi,
 *               kodlama suresi, cizim/kose sayisi
 *   gpu.csv     kare basina GPU suresi (tamamlanma isleyicisinden)
 *   saniye.csv  saniyede bir: surec enerjisi (CPU, P cekirdek), komut/cevrim,
 *               GPU kullanimi, termal durum
 *   thread.csv  saniyede bir: thread basina kullanici/sistem suresi (birikimli)
 *   ornek.bin   1 ms'de bir konuk thread'i, 4 ms'de bir ana thread'i: PC + cerceve
 *               zinciri (thread_suspend -> durum -> resume; kilit alinmaz)
 *   sembol.txt  ana ikili disindaki PC'lerin dladdr adlari (sistem kutuphaneleri)
 *   ekran/      2 sn'de bir kucuk ekran goruntusu (metal.m, arka planda yazilir)
 *   meta.txt    ikili yolu, kaydirma, saat esleme (powermetrics icin)
 *
 * Cozumleme: tools/oturum_analiz.py <dizin>.  Kayit betigi: tools/oturum.sh. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/event.h>
#include <sys/resource.h>
#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach-o/dyld.h>
#include "perflog.h"

int g_perflog = 0;
_Atomic int g_pl_thumb_req = 0;
char g_pl_dir[900];

static uint64_t t0_us;
static FILE *f_konuk, *f_render, *f_gpu, *f_sec, *f_thr, *f_samp;
static pthread_mutex_t g_gpu_lock = PTHREAD_MUTEX_INITIALIZER;

static _Atomic uint64_t ev_n[PL_NEV], ev_ns[PL_NEV], ev_b[PL_NEV];

static uint64_t mono_us(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}
uint64_t perflog_now_us(void){ return mono_us() - t0_us; }

void perflog_event(int k, uint64_t ns, uint64_t bytes){
    if (!g_perflog || k < 0 || k >= PL_NEV) return;
    atomic_fetch_add(&ev_n[k], 1ull);
    atomic_fetch_add(&ev_ns[k], ns);
    atomic_fetch_add(&ev_b[k], bytes);
}

/* ---- ornekleyici ---- */
typedef struct { mach_port_t port; uint64_t lo, hi; } PlThr;
static PlThr th_konuk, th_render;
static _Atomic uint32_t g_gframe;

static void thr_register(PlThr *t){
    pthread_t me = pthread_self();
    uint64_t hi = (uint64_t)(uintptr_t)pthread_get_stackaddr_np(me);
    uint64_t sz = (uint64_t)pthread_get_stacksize_np(me);
    t->lo = hi - sz; t->hi = hi;
    t->port = mach_thread_self();
}

#define PL_DEPTH 28
typedef struct { uint64_t t_us; uint32_t gframe; uint8_t role, n; uint16_t pad; } PlRec;

/* ana ikili disindaki PC'ler (sistem kutuphaneleri) icin kucuk tablo */
#define PL_SYS 4096
static uint64_t pl_sys[PL_SYS]; static int pl_sysn;
static uint64_t img_lo, img_hi;

static void sys_note(uint64_t pc){
    if (pc >= img_lo && pc < img_hi) return;
    uint32_t h = (uint32_t)((pc >> 2) * 2654435761u) & (PL_SYS - 1);
    for (int i = 0; i < PL_SYS; i++, h = (h + 1) & (PL_SYS - 1)){
        if (pl_sys[h] == pc) return;
        if (!pl_sys[h]){ if (pl_sysn < PL_SYS * 3 / 4){ pl_sys[h] = pc; pl_sysn++; } return; }
    }
}

static void pl_sample(PlThr *t, int role){
    uint64_t st[PL_DEPTH]; int n = 0;
    if (thread_suspend(t->port) != KERN_SUCCESS) return;
    arm_thread_state64_t s; mach_msg_type_number_t cnt = ARM_THREAD_STATE64_COUNT;
    if (thread_get_state(t->port, ARM_THREAD_STATE64, (thread_state_t)&s, &cnt) == KERN_SUCCESS){
        uint64_t pc = (uint64_t)arm_thread_state64_get_pc(s);
        uint64_t lr = (uint64_t)arm_thread_state64_get_lr(s);
        uint64_t fp = (uint64_t)arm_thread_state64_get_fp(s);
        st[n++] = pc; st[n++] = lr;
        /* askidayken yalnizca ham bellek okumasi: kilit/malloc YOK */
        while (n < PL_DEPTH && fp >= t->lo && fp + 16 <= t->hi && !(fp & 7)){
            uint64_t nfp = ((uint64_t *)(uintptr_t)fp)[0], ret = ((uint64_t *)(uintptr_t)fp)[1];
            if (!ret) break;
            st[n++] = ret;
            if (nfp <= fp) break;
            fp = nfp;
        }
    }
    thread_resume(t->port);
    if (!n) return;
    PlRec r = { perflog_now_us(), atomic_load(&g_gframe), (uint8_t)role, (uint8_t)n, 0 };
    fwrite(&r, sizeof r, 1, f_samp);
    fwrite(st, sizeof(uint64_t), (size_t)n, f_samp);
    sys_note(st[0]);
}

/* Diger tum thread'ler (Metal/sistem/ses/konuk yan thread'leri) 10 ms'de bir:
 * rol 2, gframe alani yerine thread kimligi (thread.csv ile eslesir).  Yigin
 * siniri bilinmedigi icin yalniz PC + LR (hangi cagrida/beklemede oldugu). */
static void pl_sample_others(void){
    thread_act_array_t list; mach_msg_type_number_t nl = 0;
    if (task_threads(mach_task_self(), &list, &nl) != KERN_SUCCESS) return;
    mach_port_t self = mach_thread_self();
    for (mach_msg_type_number_t i = 0; i < nl; i++){
        mach_port_t th = list[i];
        if (th != self && th != th_konuk.port && th != th_render.port){
            struct thread_identifier_info ii; mach_msg_type_number_t c2 = THREAD_IDENTIFIER_INFO_COUNT;
            uint64_t tid = 0;
            if (thread_info(th, THREAD_IDENTIFIER_INFO, (thread_info_t)&ii, &c2) == KERN_SUCCESS) tid = ii.thread_id;
            uint64_t st[2]; int n = 0;
            if (thread_suspend(th) == KERN_SUCCESS){
                arm_thread_state64_t s; mach_msg_type_number_t cnt = ARM_THREAD_STATE64_COUNT;
                if (thread_get_state(th, ARM_THREAD_STATE64, (thread_state_t)&s, &cnt) == KERN_SUCCESS){
                    st[n++] = (uint64_t)arm_thread_state64_get_pc(s);
                    st[n++] = (uint64_t)arm_thread_state64_get_lr(s);
                }
                thread_resume(th);
            }
            if (n){
                PlRec r = { perflog_now_us(), (uint32_t)tid, 2, (uint8_t)n, 0 };
                fwrite(&r, sizeof r, 1, f_samp); fwrite(st, sizeof(uint64_t), (size_t)n, f_samp);
                sys_note(st[0]); sys_note(st[1]);
            }
        }
        mach_port_deallocate(mach_task_self(), th);
    }
    mach_port_deallocate(mach_task_self(), self);
    vm_deallocate(mach_task_self(), (vm_address_t)list, nl * sizeof(thread_act_t));
}

static void write_syms(void){
    char p[1024]; snprintf(p, sizeof p, "%s/sembol.txt", g_pl_dir);
    FILE *f = fopen(p, "w"); if (!f) return;
    for (int i = 0; i < PL_SYS; i++){
        if (!pl_sys[i]) continue;
        Dl_info di;
        if (dladdr((void *)(uintptr_t)pl_sys[i], &di) && di.dli_sname){
            const char *im = di.dli_fname ? strrchr(di.dli_fname, '/') : NULL;
            fprintf(f, "%llx %s %s\n", (unsigned long long)pl_sys[i], di.dli_sname, im ? im + 1 : "?");
        } else fprintf(f, "%llx ? ?\n", (unsigned long long)pl_sys[i]);
    }
    fclose(f);
}

/* ---- saniyelik ---- */
int metal_thermal_state(void);
static void pl_second(void){
    uint64_t t = perflog_now_us();
    struct rusage_info_v6 r; memset(&r, 0, sizeof r);
    proc_pid_rusage(getpid(), RUSAGE_INFO_V6, (rusage_info_t *)&r);
    struct task_power_info_v2 p; memset(&p, 0, sizeof p);
    mach_msg_type_number_t pn = TASK_POWER_INFO_V2_COUNT;
    task_info(mach_task_self(), TASK_POWER_INFO_V2, (task_info_t)&p, &pn);
    fprintf(f_sec, "%.3f,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%d,%llu\n", t / 1e3,
            (unsigned long long)r.ri_energy_nj, (unsigned long long)r.ri_penergy_nj,
            (unsigned long long)r.ri_instructions, (unsigned long long)r.ri_cycles,
            (unsigned long long)r.ri_pinstructions, (unsigned long long)r.ri_pcycles,
            (unsigned long long)r.ri_user_time, (unsigned long long)r.ri_system_time,
            (unsigned long long)r.ri_user_ptime, (unsigned long long)r.ri_system_ptime,
            (unsigned long long)p.gpu_energy.task_gpu_utilisation, metal_thermal_state(),
            (unsigned long long)r.ri_phys_footprint);
    thread_act_array_t list; mach_msg_type_number_t nl = 0;
    if (task_threads(mach_task_self(), &list, &nl) == KERN_SUCCESS){
        for (mach_msg_type_number_t i = 0; i < nl; i++){
            struct thread_extended_info xi; mach_msg_type_number_t c1 = THREAD_EXTENDED_INFO_COUNT;
            struct thread_identifier_info ii; mach_msg_type_number_t c2 = THREAD_IDENTIFIER_INFO_COUNT;
            if (thread_info(list[i], THREAD_EXTENDED_INFO, (thread_info_t)&xi, &c1) == KERN_SUCCESS &&
                thread_info(list[i], THREAD_IDENTIFIER_INFO, (thread_info_t)&ii, &c2) == KERN_SUCCESS){
                const char *rol = list[i] == th_konuk.port ? "KONUK" : list[i] == th_render.port ? "ANA" : "";
                for (char *q = xi.pth_name; *q; q++) if (*q == ',') *q = ';';
                fprintf(f_thr, "%.3f,%llu,%s,%s,%llu,%llu\n", t / 1e3, (unsigned long long)ii.thread_id,
                        rol, xi.pth_name, (unsigned long long)xi.pth_user_time,
                        (unsigned long long)xi.pth_system_time);
            }
            mach_port_deallocate(mach_task_self(), list[i]);
        }
        vm_deallocate(mach_task_self(), (vm_address_t)list, nl * sizeof(thread_act_t));
    }
    fflush(f_sec); fflush(f_thr); fflush(f_samp);
    if (f_konuk) fflush(f_konuk);
    if (f_render) fflush(f_render);
    pthread_mutex_lock(&g_gpu_lock); if (f_gpu) fflush(f_gpu); pthread_mutex_unlock(&g_gpu_lock);
}

static void *pl_thread(void *a){
    (void)a;
    pthread_setname_np("perflog");
    int kq = kqueue();
    struct kevent ev;
    EV_SET(&ev, 1, EVFILT_TIMER, EV_ADD, NOTE_NSECONDS | NOTE_CRITICAL, 1000000, 0);   /* 1 ms */
    kevent(kq, &ev, 1, NULL, 0, NULL);
    uint64_t tick = 0, next_sec = 1000000, next_sym = 30000000, next_thumb = 2000000;
    for (;;){
        struct kevent out;
        if (kevent(kq, NULL, 0, &out, 1, NULL) < 1) continue;
        tick++;
        if (th_konuk.port) pl_sample(&th_konuk, 0);
        if ((tick & 3) == 0 && th_render.port) pl_sample(&th_render, 1);
        if (tick % 10 == 5) pl_sample_others();
        uint64_t t = perflog_now_us();
        if (t >= next_sec){ next_sec = t + 1000000; pl_second(); }
        if (t >= next_thumb){ next_thumb = t + 2000000; atomic_store(&g_pl_thumb_req, 1); }
        if (t >= next_sym){ next_sym = t + 30000000; write_syms(); }
    }
    return NULL;
}

static FILE *pl_open(const char *nm, const char *hdr){
    char p[1024]; snprintf(p, sizeof p, "%s/%s", g_pl_dir, nm);
    FILE *f = fopen(p, "w");
    if (!f) return NULL;
    setvbuf(f, NULL, _IOFBF, 1 << 20);
    if (hdr) fprintf(f, "%s\n", hdr);
    return f;
}

static void pl_exit(void){
    if (!g_perflog) return;
    pl_second();
    write_syms();
    fprintf(stderr, "perflog: kayit kapandi -> %s\n", g_pl_dir);
}

__attribute__((constructor)) static void perflog_init(void){
    const char *e = getenv("PERFLOG");
    if (!e || !*e) return;
    snprintf(g_pl_dir, sizeof g_pl_dir, "%s", e);
    mkdir(g_pl_dir, 0755);
    char ek[1024]; snprintf(ek, sizeof ek, "%s/ekran", g_pl_dir); mkdir(ek, 0755);
    t0_us = mono_us();
    f_konuk  = pl_open("konuk.csv", "t_ms,gkare,duvar_us,cpu_us,uyku_us,present_bekle_us,com,win32,"
                                    "vs_n,ps_n,pso_n,derleme_us,doku_n,doku_kb,doku_us");
    f_render = pl_open("render.csv", "t_ms,kare,aralik_us,drawable_bekle_us,kodlama_us,cizim,kose,dusen,doku_farkli");
    f_gpu    = pl_open("gpu.csv", "kare,t_ms,gpu_us");
    f_sec    = pl_open("saniye.csv", "t_ms,enerji_nj,penerji_nj,komut,cevrim,pkomut,pcevrim,"
                                     "kull_t,sis_t,pkull_t,psis_t,gpu_kullanim,termal,bellek");
    f_thr    = pl_open("thread.csv", "t_ms,tid,rol,ad,kull_ns,sis_ns");
    f_samp   = pl_open("ornek.bin", NULL);
    if (!f_konuk || !f_render || !f_gpu || !f_sec || !f_thr || !f_samp){
        fprintf(stderr, "perflog: %s yazilamiyor, kayit KAPALI\n", g_pl_dir); return; }
    /* ana ikili: PC -> sembol icin yol ve kaydirma; saat esleme */
    const struct mach_header_64 *mh = (const struct mach_header_64 *)_dyld_get_image_header(0);
    intptr_t slide = _dyld_get_image_vmaddr_slide(0);
    img_lo = (uint64_t)(uintptr_t)mh;
    {   /* __TEXT boyutu */
        const uint8_t *p = (const uint8_t *)(mh + 1);
        for (uint32_t i = 0; i < mh->ncmds; i++){
            const struct load_command *lc = (const struct load_command *)p;
            if (lc->cmd == LC_SEGMENT_64){
                const struct segment_command_64 *sg = (const struct segment_command_64 *)p;
                if (!strcmp(sg->segname, "__TEXT")) img_hi = img_lo + sg->vmsize;
            }
            p += lc->cmdsize;
        }
    }
    struct timeval tv; gettimeofday(&tv, NULL);
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    FILE *m = pl_open("meta.txt", NULL);
    if (m){
        fprintf(m, "ikili %s\nkaydirma %lld\nmetin %llx %llx\n", _dyld_get_image_name(0),
                (long long)slide, (unsigned long long)img_lo, (unsigned long long)img_hi);
        fprintf(m, "baslangic_epoch_us %llu\n", (unsigned long long)tv.tv_sec * 1000000ull + (unsigned long long)tv.tv_usec);
        fprintf(m, "zaman_tabani %u %u\n", tb.numer, tb.denom);
#ifndef NFS_SURUM
#define NFS_SURUM 0
#endif
        fprintf(m, "surum %d\n", NFS_SURUM);
#if NFS_ENHANCED
        fprintf(m, "build HD\n");
#else
        fprintf(m, "build normal\n");
#endif
        const char *keys[] = { "NFSRES", "NFSFULL", "SPINSLEEP", "PACE60", "HD_RT", "HD_MSAA", NULL };
        for (int i = 0; keys[i]; i++){ const char *v = getenv(keys[i]); if (v) fprintf(m, "env %s=%s\n", keys[i], v); }
        fclose(m);
    }
    g_perflog = 1;
    atexit(pl_exit);
    pthread_t th; pthread_attr_t at; pthread_attr_init(&at);
    pthread_attr_set_qos_class_np(&at, QOS_CLASS_USER_INTERACTIVE, 0);
    pthread_create(&th, &at, pl_thread, NULL); pthread_detach(th);
    fprintf(stderr, "perflog: oturum kaydi ACIK -> %s\n", g_pl_dir);
}

/* ---- konuk kare (Present sonrasi, konuk thread) ---- */
extern uint64_t g_com_calls, g_win32_calls;
uint64_t spin_slept_us(void);
unsigned long long metal_wait_present_ns(void);
long metal_frames_dropped(void);
void perflog_guest_frame(void){
    static uint64_t lw, lc, ls, lp, lcom, lw32, le[PL_NEV][3];
    static int first = 1;
    if (first){ first = 0; thr_register(&th_konuk); }
    struct timespec ts; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    uint64_t cpu = (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
    uint64_t w = perflog_now_us(), sl = spin_slept_us(), pw = metal_wait_present_ns() / 1000ull;
    uint64_t e[PL_NEV][3];
    for (int k = 0; k < PL_NEV; k++){ e[k][0] = atomic_load(&ev_n[k]); e[k][1] = atomic_load(&ev_ns[k]); e[k][2] = atomic_load(&ev_b[k]); }
    uint32_t gf = atomic_fetch_add(&g_gframe, 1u) + 1u;
    if (lw){
        uint64_t der_ns = (e[PL_VS][1] - le[PL_VS][1]) + (e[PL_PS][1] - le[PL_PS][1]) + (e[PL_PSO][1] - le[PL_PSO][1]);
        fprintf(f_konuk, "%.3f,%u,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
                w / 1e3, gf, (unsigned long long)(w - lw), (unsigned long long)(cpu - lc),
                (unsigned long long)(sl - ls), (unsigned long long)(pw - lp),
                (unsigned long long)(g_com_calls - lcom), (unsigned long long)(g_win32_calls - lw32),
                (unsigned long long)(e[PL_VS][0] - le[PL_VS][0]), (unsigned long long)(e[PL_PS][0] - le[PL_PS][0]),
                (unsigned long long)(e[PL_PSO][0] - le[PL_PSO][0]), (unsigned long long)(der_ns / 1000ull),
                (unsigned long long)(e[PL_TEX][0] - le[PL_TEX][0]),
                (unsigned long long)((e[PL_TEXUP][2] - le[PL_TEXUP][2]) / 1024ull),
                (unsigned long long)((e[PL_TEXUP][1] - le[PL_TEXUP][1]) / 1000ull));
    }
    lw = w; lc = cpu; ls = sl; lp = pw; lcom = g_com_calls; lw32 = g_win32_calls;
    memcpy(le, e, sizeof le);
}

/* ---- ana thread kare (commit sonrasi) ---- */
void perflog_render_frame(long fno, unsigned ntex, uint64_t drawable_ns, uint64_t encode_ns, unsigned draws, unsigned verts){
    static uint64_t lw; static int first = 1;
    if (first){ first = 0; thr_register(&th_render); }
    uint64_t w = perflog_now_us();
    fprintf(f_render, "%.3f,%ld,%llu,%llu,%llu,%u,%u,%ld,%u\n", w / 1e3, fno,
            (unsigned long long)(lw ? w - lw : 0), (unsigned long long)(drawable_ns / 1000ull),
            (unsigned long long)(encode_ns / 1000ull), draws, verts, metal_frames_dropped(), ntex);
    lw = w;
}

/* ---- GPU tamamlanma (Metal tamamlanma thread'i) ---- */
void perflog_gpu(long fno, double gpu_ns){
    pthread_mutex_lock(&g_gpu_lock);
    if (f_gpu) fprintf(f_gpu, "%ld,%.3f,%.1f\n", fno, perflog_now_us() / 1e3, gpu_ns / 1e3);
    pthread_mutex_unlock(&g_gpu_lock);
}
