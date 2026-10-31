/* Guest file I/O -> POSIX.  Maps Windows paths to the game directory and does
 * case-insensitive resolution.                                         */
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>
static uint64_t now_ns_io(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return (uint64_t)ts.tv_sec*1000000000ull + (uint64_t)ts.tv_nsec;
}

void mem_commit(uint32_t,uint32_t);

static char g_root[1024] = ".";
static char g_cwd_rel[512] = "";           /* current directory inside the game */
uint64_t g_file_open=0, g_file_fail=0, g_file_read=0, g_bytes_read=0;
uint64_t g_overlapped_reads=0;
static int g_empty_next = 0;

void fileio_set_root(const char *exe_path){
    strncpy(g_root, exe_path, sizeof g_root - 1);
    char *slash = strrchr(g_root, '/');
    if (slash) *slash = 0; else strcpy(g_root, ".");
    fprintf(stderr,"file: game root directory = %s\n", g_root);
}

/* Convert a Windows path to a real path.
 *   "C:\\NFSU2\\TRACKS\\X.BUN" | "TRACKS\\X.BUN" | ".\\X" -> <root>/TRACKS/X.BUN
 * Since the virtual game directory is "C:\\NFSU2", that prefix is dropped.
 * Components are resolved case-insensitively.                          */
/* For opens meant for writing, create the missing parent directories (on
 * Windows the game creates them separately; here the whole path must be ready). */
static void mkparents(const char *path){
    char tmp[1024]; snprintf(tmp, sizeof tmp, "%s", path);
    for (char *q = tmp + 1; *q; q++){
        if (*q != '/') continue;
        *q = 0; mkdir(tmp, 0755); *q = '/';
    }
}

static int resolve_in(const char *root, const char *win, char *out, size_t outsz){
    char rel[1024]; size_t k=0;
    const char *p = win;
    if (p[0] && p[1]==':') p += 2;                       /* drive letter   */
    while (*p=='\\' || *p=='/') p++;
    if (!strncasecmp(p,"NFSU2",5) && (p[5]=='\\'||p[5]=='/'||p[5]==0)){
        p += 5; while (*p=='\\'||*p=='/') p++;           /* virtual root   */
    }
    while (p[0]=='.' && (p[1]=='\\'||p[1]=='/')) p += 2;
    for (; *p && k<sizeof rel-1; p++) rel[k++] = (*p=='\\') ? '/' : *p;
    rel[k]=0;
    if (!rel[0]){ snprintf(out,outsz,"%s",root); return 1; }

    snprintf(out, outsz, "%s/%s", root, rel);
    struct stat st;
    if (stat(out, &st)==0) return 1;

    /* case-insensitive, component by component */
    char cur[1024]; snprintf(cur, sizeof cur, "%s", root);
    char tmp[1024]; snprintf(tmp,sizeof tmp,"%s",rel);
    char *save=0; int missing=0;
    for (char *tok=strtok_r(tmp,"/",&save); tok; tok=strtok_r(0,"/",&save)){
        if (missing){ size_t l=strlen(cur); snprintf(cur+l,sizeof cur-l,"/%s",tok); continue; }
        char cand[1024]; snprintf(cand,sizeof cand,"%s/%s",cur,tok);
        if (stat(cand,&st)==0){ snprintf(cur,sizeof cur,"%s",cand); continue; }
        DIR *d = opendir(cur); int found=0;
        if (d){ struct dirent *e;
            while ((e=readdir(d))){
                if (!strcasecmp(e->d_name, tok)){
                    snprintf(cand,sizeof cand,"%s/%s",cur,e->d_name);
                    found=1; break; } }
            closedir(d); }
        if (found) snprintf(cur,sizeof cur,"%s",cand);
        else { snprintf(cand,sizeof cand,"%s/%s",cur,tok);
               snprintf(cur,sizeof cur,"%s",cand); missing=1; }
    }
    snprintf(out,outsz,"%s",cur);
    return !missing;
}

/* Does the game folder hold this file?  Windows-style path, case-insensitive. */
int fileio_game_file_exists(const char *win){
    char p[1024]; struct stat st;
    return resolve_in(g_root, win, p, sizeof p) && stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

/* --- write root (save layer) ---------------------------------------------
 * The game writes its saves and profiles into its OWN folder ("C:\NFSU2\NFS
 * Underground 2\<name>\<name>").  Writing there deletes the saves when a new
 * version is distributed and the .app or game folder is replaced; when the data
 * is embedded inside the .app it also breaks the bundle's signature.  So the
 * write root is now ALWAYS set (bundle.m app_save_dir, ~/Library/Application
 * Support):
 *   - writes / creates / deletes always go to the write root,
 *   - reads look in the write root first, otherwise in the game root,
 *   - directory listings are the union of both.
 * If it isn't set (only if there is no HOME) everything stays in a single root. */
static char g_wroot[1024] = "";
void fileio_set_write_root(const char *dir){
    if (!dir || !*dir){ g_wroot[0] = 0; return; }
    snprintf(g_wroot, sizeof g_wroot, "%s", dir);
    mkparents(g_wroot); mkdir(g_wroot, 0755);
    fprintf(stderr,"file: save directory = %s\n", g_wroot);
}
/* Read: the write root if it exists there, otherwise the game root. */
static int resolve(const char *win, char *out, size_t outsz){
    if (g_wroot[0]){
        char w[1024];
        if (resolve_in(g_wroot, win, w, sizeof w) && strcmp(w, g_wroot) != 0){
            snprintf(out, outsz, "%s", w); return 1; }
    }
    return resolve_in(g_root, win, out, outsz);
}
/* Write: ALWAYS the write root if there is one. */
static int resolve_w(const char *win, char *out, size_t outsz){
    return resolve_in(g_wroot[0] ? g_wroot : g_root, win, out, outsz);
}
static int copy_file(const char *a, const char *b){
    FILE *fa = fopen(a, "rb"); if (!fa) return 0;
    mkparents(b);
    FILE *fb = fopen(b, "wb"); if (!fb){ fclose(fa); return 0; }
    char bufr[65536]; size_t k;
    while ((k = fread(bufr, 1, sizeof bufr, fa)) > 0) fwrite(bufr, 1, k, fb);
    fclose(fb); fclose(fa); return 1;
}
/* If a file to be written without truncation exists only in the game root,
 * copy it to the write root first; otherwise the game writes over an empty file. */
static void copy_up(const char *win, const char *wpath){
    if (!g_wroot[0]) return;
    struct stat st; if (stat(wpath, &st) == 0) return;
    char src[1024];
    if (resolve_in(g_root, win, src, sizeof src) && stat(src, &st) == 0 && S_ISREG(st.st_mode))
        copy_file(src, wpath);
}

/* --- handle table --- */
#define FH_BASE 0x00900000u
#define FH_MAX  256
typedef struct { int fd; int empty; int used; char path[512]; DIR *dir; char pat[128];
                 DIR *dir2; char path1[512]; } FH;   /* dir2: the game root (while the save layer is active) */
static FH g_fh[FH_MAX];
static uint32_t fh_new(void){
    for (int i=1;i<FH_MAX;i++) if(!g_fh[i].used){ memset(&g_fh[i],0,sizeof g_fh[i]);
        g_fh[i].used=1; g_fh[i].fd=-1; return FH_BASE+(uint32_t)i; }
    return 0;
}
static FH *fh_get(uint32_t h){
    if (h<FH_BASE || h>=FH_BASE+FH_MAX) return 0;
    FH *f=&g_fh[h-FH_BASE]; return f->used?f:0;
}
int fileio_is_handle(uint32_t h){ return fh_get(h)!=0; }

/* ---- BOOT MOVIES (see overrides.c) ----------------------------------------------
 * BOOTMOVIES=off: the logos, the blank screen, the notice and the opening movie
 * are not opened; the game goes on as for a missing file. */
int bootmovies_mode(void);                 /* overrides.c: 0 original, 1 skippable, 2 off */
static int is_boot_movie(const char *path){
    const char *b = strrchr(path, '/'); b = b ? b + 1 : path;
    const char *ext = strrchr(b, '.');
    if (!ext || strcasecmp(ext, ".vp6")) return 0;
    static const char *const pre[] = { "ealogo", "THX_logo", "PSA", "blank", "FMVOpening" };
    for (int i = 0; i < 5; i++) if (!strncasecmp(b, pre[i], strlen(pre[i]))) return 1;
    return 0;
}

static uint32_t A(CPU *c,int i){ return MR32(c->esp + 4u*(uint32_t)i); }
#define GS(a) ((const char*)(g_mem + (uint32_t)(a)))
#define INVALID_HANDLE 0xFFFFFFFFu

/* Windows wildcard matching: case-insensitive, "*.*" matches everything. */
static int win_match(const char *pat, const char *name){
    if (!pat[0] || !strcmp(pat, "*") || !strcmp(pat, "*.*")) return 1;
    return fnmatch(pat, name, FNM_CASEFOLD) == 0;
}
/* Give the next name in a directory scan: write root first, then the game root.
 * full: the real path of the entry found (for attributes/size). */
static int find_next(FH *f, char *nm, size_t nsz, char *full, size_t fsz){
    struct dirent *e;
    if (f->dir){
        while ((e = readdir(f->dir))){
            if (e->d_name[0] == '.') continue;
            if (!win_match(f->pat, e->d_name)) continue;
            snprintf(full, fsz, "%s/%s", f->path1, e->d_name);
            snprintf(nm, nsz, "%s", e->d_name); return 1; }
    }
    if (f->dir2){
        while ((e = readdir(f->dir2))){
            if (e->d_name[0] == '.') continue;
            if (!win_match(f->pat, e->d_name)) continue;
            if (f->path1[0]){               /* already given from the write root */
                char p[1024]; struct stat st;
                snprintf(p, sizeof p, "%s/%s", f->path1, e->d_name);
                if (stat(p, &st) == 0) continue; }
            snprintf(full, fsz, "%s/%s", f->path, e->d_name);
            snprintf(nm, nsz, "%s", e->d_name); return 1; }
    }
    return 0;
}
/* WIN32_FIND_DATAA (320 bytes): +0 attributes, +4/+12/+20 creation/access/
 * write FILETIME, +28 size (high), +32 size (low), +44 name.
 * The attribute used to always be 0x80 (normal file): the game looks up career
 * profiles as FOLDERS under "NFS Underground 2", took the folder for a file
 * and never found the save. */
static void fill_find_data(uint32_t fd_, const char *full, const char *nm){
    if (!fd_) return;
    mem_commit(fd_, 320); memset(g_mem+fd_, 0, 320);
    struct stat st; uint32_t attr = 0x80u;          /* FILE_ATTRIBUTE_NORMAL */
    if (stat(full, &st) == 0){
        if (S_ISDIR(st.st_mode)) attr = 0x10u;      /* FILE_ATTRIBUTE_DIRECTORY */
        else {
            MR32(fd_+28) = (uint32_t)((uint64_t)st.st_size >> 32);
            MR32(fd_+32) = (uint32_t)st.st_size;
        }
        /* Unix seconds -> FILETIME (100 ns since 1601) */
        uint64_t ft = ((uint64_t)st.st_mtime + 11644473600ull) * 10000000ull;
        for (int k = 0; k < 3; k++){
            MR32(fd_+4+8u*k) = (uint32_t)ft; MR32(fd_+8+8u*k) = (uint32_t)(ft >> 32); }
    }
    MR32(fd_) = attr;
    snprintf((char*)(g_mem+fd_+44), 260, "%s", nm);
}

static int fileio_call_impl(CPU *c, const char *n, uint32_t *out);
/* FILEOPLOG=1: log file operations (except reads/seeks) with their results.
 * For tracking save (profile) bugs. */
int fileio_call(CPU *c, const char *n, uint32_t *out){
    static int lg = -1; if (lg < 0) lg = getenv("FILEOPLOG") != 0;
    int h = fileio_call_impl(c, n, out);
    if (lg && h && strcmp(n,"ReadFile") && strcmp(n,"SetFilePointer")
        && strcmp(n,"GetOverlappedResult") && strcmp(n,"HasOverlappedIoCompleted")){
        uint32_t a0 = A(c,0);
        const char *s0 = (a0 >= 0x10000u && mem_is_committed(a0)) ? GS(a0) : "";
        if (strcmp(n,"CreateFileA") && strcmp(n,"GetFileAttributesA") && strcmp(n,"FindFirstFileA")
            && strcmp(n,"DeleteFileA") && strcmp(n,"CreateDirectoryA") && strcmp(n,"MoveFileA")
            && strcmp(n,"CopyFileA")) s0 = "";
        fprintf(stderr, "  [file] %-18s %.200s -> 0x%x  (arg1=0x%x)\n", n, s0, *out, A(c,1));
    }
    return h;
}
static int fileio_call_impl(CPU *c, const char *n, uint32_t *out){
    uint32_t r = 0;
    if (!strcmp(n,"CreateFileA")){
        uint32_t pn=A(c,0), access=A(c,1), disp=A(c,4);
        char real[1024];
        int flags;
        if (disp==2 /*CREATE_ALWAYS*/ || disp==1 /*CREATE_NEW*/) flags=O_RDWR|O_CREAT|O_TRUNC;
        else if (disp==4 /*OPEN_ALWAYS*/) flags=O_RDWR|O_CREAT;
        else flags = (access & 0x40000000u) ? O_RDWR : O_RDONLY;
        int ok;
        if (flags != O_RDONLY){
            ok = resolve_w(GS(pn), real, sizeof real);
            if (!(flags & O_TRUNC)) copy_up(GS(pn), real);
        } else ok = resolve(GS(pn), real, sizeof real);
        if (flags & O_CREAT) mkparents(real);
        /* Language suffix fallback: in this version the movie path has the "_fr.vp6"
         * suffix embedded, but the folder has "_en" files.  If the requested one
         * doesn't exist, try "_en" and the suffix-less version.         */
        struct stat _st;
        if (!(flags & O_CREAT) && stat(real, &_st) != 0){
            char alt[1024]; const char *dot = strrchr(real, '.');
            if (dot && dot - real > 3){
                size_t base = (size_t)(dot - real);
                const char *sfx[] = { "_en", "", "_dn", "_du", "_sw" };
                for (int q = 0; q < 5; q++){
                    /* remove the current language suffix (if any) */
                    size_t cut = base;
                    if (base >= 3 && real[base-3] == '_') cut = base - 3;
                    snprintf(alt, sizeof alt, "%.*s%s%s",
                             (int)cut, real, sfx[q], dot);
                    if (stat(alt, &_st) == 0){
                        if (getenv("VERBOSE") || getenv("FILELIST"))
                            fprintf(stderr,"  [file] language fallback: \"%s\" -> \"%s\"\n",
                                    real, alt);
                        snprintf(real, sizeof real, "%s", alt);
                        break;
                    }
                }
            }
        }
        /* NOMOVIES=1: ignore video files.  There is no VP6 decoder; if the game tries
         * to play a video and waits, it gets stuck there. */
        int fd;
        { const char *ext = strrchr(real, '.');
          const char *nm2 = getenv("NOMOVIES");
          int movie_mode = (nm2 && *nm2 && *nm2 != '0') ? atoi(nm2) : 0;
          /* mode 3: skip only the "blank" movie (let the real movies play) */
          const char *bn2 = strrchr(real, '/'); bn2 = bn2 ? bn2+1 : real;
          if (movie_mode == 3 && !strncasecmp(bn2, "blank", 5)) fd = -1;
          else if (bootmovies_mode() == 2 && !(flags & O_CREAT) && is_boot_movie(real)) fd = -1;
          else if (movie_mode == 1 && ext &&
              (!strcasecmp(ext,".vp6") || !strcasecmp(ext,".vp8") ||
               !strcasecmp(ext,".bik") || !strcasecmp(ext,".mpg")))
              fd = -1;
          else if (movie_mode == 3 && !strncasecmp(bn2, "blank", 5)) { }
          else {
              fd = open(real, flags, 0644);
              /* mode 2: let it open but look empty -> the player says "done" */
              if (fd >= 0 && movie_mode == 2 && ext &&
                  (!strcasecmp(ext,".vp6") || !strcasecmp(ext,".vp8") ||
                   !strcasecmp(ext,".bik") || !strcasecmp(ext,".mpg")))
                  g_empty_next = 1;
          } }
        { static int shown=0; if ((getenv("VERBOSE")||getenv("FILELIST")) && shown<200){ shown++;
            fprintf(stderr,"  [file] CreateFileA(\"%s\") -> \"%s\"  %s\n",
                    GS(pn), real, fd>=0?"OPEN":"FAILED"); } }
        if (fd<0){ g_file_fail++; r=INVALID_HANDLE;
            fprintf(stderr,"  [file] COULD NOT OPEN: \"%s\" -> \"%s\" (disp=%u access=0x%x)\n",
                    GS(pn), real, disp, access); }
        else {
            uint32_t h=fh_new();
            if(!h){ close(fd); r=INVALID_HANDLE; }
            else { FH*f=fh_get(h); f->fd=fd; f->empty = g_empty_next; g_empty_next = 0;
                   snprintf(f->path,sizeof f->path,"%s",real);
                   g_file_open++; r=h; }
        }
    }
    else if (!strcmp(n,"ReadFile")){
        /* (hFile, lpBuffer, nBytes, lpNumberRead, lpOverlapped)
         * If an OVERLAPPED is given: the offset is taken from the structure, the
         * result is written into it and hEvent is signalled.  Without this the
         * game waits forever for the asynchronous read to finish.        */
        FH*f=fh_get(A(c,0)); uint32_t buf=A(c,1), len=A(c,2), pnr=A(c,3), ov=A(c,4);
        if(!f||f->fd<0){ if(pnr) MR32(pnr)=0; r=0; }
        else { if(len) mem_commit(buf,len);
               ssize_t got;
               uint64_t _rd0 = now_ns_io();
               if (ov && mem_is_committed(ov+19)){
                   uint64_t off = (uint64_t)MR32(ov+8) | ((uint64_t)MR32(ov+12) << 32);
                   got = pread(f->fd, g_mem+buf, len, (off_t)off);
                   if(got<0) got=0;
                   MR32(ov+0) = 0;                      /* Internal = STATUS_SUCCESS */
                   MR32(ov+4) = (uint32_t)got;          /* InternalHigh = bytes read */
                   uint32_t hev = MR32(ov+16);
                   if (hev){ void win32_signal_event(uint32_t); win32_signal_event(hev); }
                   { const char *mf2 = getenv("MUTEFILE");
                     const char *ms2 = getenv("MUTESIZE");
                     const char *mn2 = getenv("MUTEMIN");
                     long msz2 = ms2 ? atol(ms2) : 0, mmin2 = mn2 ? atol(mn2) : 0;
                     if (mf2 && *mf2 && got > 0 && strstr(f->path, mf2)
                         && (mmin2 ? ((long)len >= mmin2) : (!msz2 || (long)len == msz2)))
                         memset(g_mem+buf, 0, (size_t)got); }
                   g_overlapped_reads++;
               } else if (f->empty){
                   got = 0;                       /* immediate end of file */
               } else {
                   got = read(f->fd, g_mem+buf, len);
                   if(got<0) got=0;
               }
               /* MUTEFILE=<substring>: fill the reads of the matching file with ZEROS.
                * To measure objectively whether sound effects remain once the
                * music stream (sdat.viv) is silenced.  We don't return end of
                * file, so the streaming logic keeps working normally -- only the
                * content goes silent. */
               { const char *mf = getenv("MUTEFILE");
                 const char *ms = getenv("MUTESIZE");
                 const char *mn = getenv("MUTEMIN");
                 long msz = ms ? atol(ms) : 0, mmin = mn ? atol(mn) : 0;
                 if (mf && *mf && got > 0 && strstr(f->path, mf)
                     && (mmin ? ((long)len >= mmin) : (!msz || (long)len == msz)))
                     memset(g_mem+buf, 0, (size_t)got); }
               if(pnr) MR32(pnr)=(uint32_t)got;
               if (getenv("READLOG")){
                   const char *bn = strrchr(f->path,'/'); bn = bn?bn+1:f->path;
                   double audio_time(void);
                   fprintf(stderr,"  [read] t=%8.3f %-28s requested=%-9u got=%-9ld time=%.2f ms\n",
                           audio_time(), bn, len, (long)got,
                           (double)(now_ns_io() - _rd0)/1e6);
               }
               g_file_read++; g_bytes_read += (uint64_t)got; r=1; }
    }
    else if (!strcmp(n,"GetOverlappedResult")){
        /* (hFile, lpOverlapped, lpNumberOfBytesTransferred, bWait) */
        uint32_t ov=A(c,1), pn=A(c,2);
        if (ov && pn && mem_is_committed(ov+7)) MR32(pn) = MR32(ov+4);
        r=1; }
    else if (!strcmp(n,"HasOverlappedIoCompleted")){ r=1; }
    else if (!strcmp(n,"WriteFile")){
        FH*f=fh_get(A(c,0)); uint32_t buf=A(c,1), len=A(c,2), pnw=A(c,3);
        if(!f||f->fd<0){
            uint32_t h=A(c,0);
            if(h>=0x10u && h<=0x13u && len){ mem_commit(buf,len);
                fprintf(stdout,"[guest] %.*s",(int)len,(char*)(g_mem+buf)); }
            if(pnw) MR32(pnw)=len; r=1; }
        else { ssize_t w=write(f->fd, g_mem+buf, len); if(w<0)w=0;
               if(pnw) MR32(pnw)=(uint32_t)w; r=1; }
    }
    else if (!strcmp(n,"SetFilePointer")){
        FH*f=fh_get(A(c,0)); int32_t dist=(int32_t)A(c,1); uint32_t method=A(c,3);
        if(!f||f->fd<0) r=0xFFFFFFFFu;
        else { int wh = method==1?SEEK_CUR : (method==2?SEEK_END:SEEK_SET);
               off_t p = lseek(f->fd, dist, wh);
               r = (p<0)?0xFFFFFFFFu:(uint32_t)p; }
    }
    else if (!strcmp(n,"GetFileSize")){
        FH*f=fh_get(A(c,0)); uint32_t ph=A(c,1);
        if(!f||f->fd<0) r=0xFFFFFFFFu;
        else { struct stat st; if(fstat(f->fd,&st)==0){ r=(uint32_t)st.st_size;
                 if(ph) MR32(ph)=(uint32_t)(((uint64_t)st.st_size)>>32); } else r=0xFFFFFFFFu; }
    }
    else if (!strcmp(n,"SetEndOfFile")){
        FH*f=fh_get(A(c,0));
        if(f&&f->fd>=0){ off_t p=lseek(f->fd,0,SEEK_CUR); r=(ftruncate(f->fd,p)==0); } else r=0;
    }
    else if (!strcmp(n,"FlushFileBuffers")){ FH*f=fh_get(A(c,0)); if(f&&f->fd>=0) fsync(f->fd); r=1; }
    else if (!strcmp(n,"CloseHandle")){
        FH*f=fh_get(A(c,0));
        if(!f) return 0;                       /* not ours - let the other layer look */
        if(f->fd>=0) close(f->fd);
        if(f->dir) closedir(f->dir);
        f->used=0; r=1;
    }
    else if (!strcmp(n,"GetFileAttributesA")){
        char real[1024]; resolve(GS(A(c,0)), real, sizeof real);
        struct stat st;
        if(stat(real,&st)!=0) r=0xFFFFFFFFu;
        else r = S_ISDIR(st.st_mode) ? 0x10u : 0x80u;   /* DIRECTORY / NORMAL */
    }
    else if (!strcmp(n,"SetFileAttributesA")) r=1;
    else if (!strcmp(n,"DeleteFileA")){
        char real[1024]; resolve_w(GS(A(c,0)), real, sizeof real);
        r = (unlink(real)==0);
    }
    else if (!strcmp(n,"SHGetFolderPathA")){
        /* (hwnd, csidl, hToken, dwFlags, pszPath) -> S_OK
         * We give the virtual root; resolve() maps it to the game directory.
         * If it returns empty the game can't find its save/profile directory. */
        uint32_t pp=A(c,4);
        if (pp){ mem_commit(pp, 260); snprintf((char*)(g_mem+pp), 260, "C:\\NFSU2"); }
        r=0; }
    else if (!strcmp(n,"CreateDirectoryA")){
        char real[1024]; resolve_w(GS(A(c,0)), real, sizeof real);
        mkparents(real);
        r = (mkdir(real,0755)==0 || errno==EEXIST);
    }
    else if (!strcmp(n,"RemoveDirectoryA")){
        /* When a profile is deleted it also removes the folder after the file. */
        char real[1024]; resolve_w(GS(A(c,0)), real, sizeof real);
        r = (rmdir(real)==0);
    }
    else if (!strcmp(n,"MoveFileA")){
        char a[1024],b[1024]; resolve(GS(A(c,0)),a,sizeof a); resolve_w(GS(A(c,1)),b,sizeof b);
        mkparents(b);
        /* If the source is in the game root (read-only), copy instead of moving. */
        if (g_wroot[0] && strncmp(a, g_wroot, strlen(g_wroot)) != 0) r = (uint32_t)copy_file(a, b);
        else r = (rename(a,b)==0);
    }
    else if (!strcmp(n,"CopyFileA")){
        char a[1024],b[1024]; resolve(GS(A(c,0)),a,sizeof a); resolve_w(GS(A(c,1)),b,sizeof b);
        r = (uint32_t)copy_file(a, b);
    }
    /* --- directory scan: WIN32_FIND_DATAA (320 bytes), +0 attr, +44 name --- */
    else if (!strcmp(n,"FindFirstFileA")){
        const char *spec = GS(A(c,0));
        char win[1024]; snprintf(win,sizeof win,"%s",spec);
        char *bs = strrchr(win,'\\'); char *fs2 = strrchr(win,'/');
        char *sep = bs>fs2?bs:fs2;
        char pat[128]="*";
        if (sep){ snprintf(pat,sizeof pat,"%s",sep+1); *sep=0; } else { snprintf(pat,sizeof pat,"%s",win); win[0]=0; }
        /* If the save layer is active, the directory in the write root is scanned
         * first, then the one in the game root (the same name isn't given twice). */
        char real[1024], wreal[1024]; wreal[0] = 0;
        if (win[0]) resolve_in(g_root, win, real, sizeof real); else snprintf(real,sizeof real,"%s",g_root);
        if (g_wroot[0]){
            if (win[0]) resolve_in(g_wroot, win, wreal, sizeof wreal);
            else snprintf(wreal, sizeof wreal, "%s", g_wroot); }
        DIR *d1 = wreal[0] ? opendir(wreal) : 0;
        DIR *d2 = opendir(real);
        if(!d1 && !d2){ r=INVALID_HANDLE; }
        else { uint32_t h=fh_new(); FH*f=fh_get(h);
               if(!f){ if(d1) closedir(d1); if(d2) closedir(d2); r=INVALID_HANDLE; }
               else { f->dir=d1; f->dir2=d2; snprintf(f->pat,sizeof f->pat,"%s",pat);
                      snprintf(f->path1,sizeof f->path1,"%s",wreal);
                      snprintf(f->path,sizeof f->path,"%s",real);
                      char nm[260], full[1400];
                      if (!find_next(f, nm, sizeof nm, full, sizeof full)){
                          if(f->dir) closedir(f->dir); if(f->dir2) closedir(f->dir2);
                          f->used=0; r=INVALID_HANDLE; }
                      else { fill_find_data(A(c,1), full, nm); r=h; } } }
    }
    else if (!strcmp(n,"FindNextFileA")){
        FH*f=fh_get(A(c,0)); r=0; char nm[260], full[1400];
        if(f && (f->dir || f->dir2) && find_next(f, nm, sizeof nm, full, sizeof full)){
            fill_find_data(A(c,1), full, nm);
            r=1; }
    }
    else if (!strcmp(n,"FindClose")){
        FH*f=fh_get(A(c,0));
        if(f){ if(f->dir) closedir(f->dir); if(f->dir2) closedir(f->dir2); f->used=0; } r=1;
    }
    else return 0;
    *out = r;
    return 1;
}

void fileio_report(void){
    if (g_overlapped_reads)
        fprintf(stderr,"  file: %llu asynchronous (overlapped) reads\n",
                (unsigned long long)g_overlapped_reads);
    fprintf(stderr,"  file: %llu opened, %llu could not be opened, %llu reads, %.1f MB\n",
            (unsigned long long)g_file_open,(unsigned long long)g_file_fail,
            (unsigned long long)g_file_read,(double)g_bytes_read/1048576.0);
}
