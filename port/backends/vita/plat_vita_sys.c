/* plat_vita_sys.c: the "sys" and "fs" parts of plat.h for the PS Vita (vitasdk: newlib + pthread-embedded).
 *
 * Derived from backends/posix/plat_posix.c, kept separate so the shared backends carry nothing Vita-specific. The
 * differences:
 * - Guest memory is one sceKernelAllocMemBlock block. The Vita has no lazy commit, so every byte of the guest space is
 *   real RAM from the start: size it with --guest-space (ss2port.txt), not by the 32-bit default of images + 512 MB.
 * - Host threads get VITA_THREAD_STACK bytes of stack instead of 8 MB (recompiled code runs on the host stack, so do
 *   not make it much smaller).
 * - newlib has no tm_gmtoff; the UTC offset comes from the RTC.
 * The CMake setup builds this file instead of backends/posix when PORT_BACKEND=vita. */
#define _GNU_SOURCE
#include <stdio.h>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/rtc.h>
#include "plat.h"

#ifndef VITA_THREAD_STACK
#define VITA_THREAD_STACK (2u << 20)
#endif

/* ---------------------------------------------------------------- log (stderr is redirected to the log file by main) */
static pthread_mutex_t log_m = PTHREAD_MUTEX_INITIALIZER;
void plat_log_write(PlatLogLevel level, const char *line) {
    (void)level;
    pthread_mutex_lock(&log_m);
    fputs(line, stderr); fputc('\n', stderr); fflush(stderr);
    pthread_mutex_unlock(&log_m);
}

/* ---------------------------------------------------------------- memory: one block of real RAM */
static SceUID guest_block = -1;
static uint8_t *guest_base; static uint32_t guest_sz;
void vita_log_free_memory(const char *when) {
    SceKernelFreeMemorySizeInfo i; memset(&i, 0, sizeof i); i.size = sizeof i;
    if (sceKernelGetFreeMemorySize(&i) < 0) return;
    struct mallinfo mi = mallinfo();
    char b[200]; snprintf(b, sizeof b, "[vita] free memory %s: user %d MB, cdram %d MB, phycont %d MB (host heap: %d MB in use, %d MB taken)", when, i.size_user >> 20, i.size_cdram >> 20, i.size_phycont >> 20, (int)(mi.uordblks >> 20), (int)(mi.arena >> 20));
    plat_log_write(PLAT_LOG_INFO, b);
}
void *plat_mem_reserve(uint64_t size) {
    if (guest_block >= 0 || size > 0xFFFFFFFFull) return NULL;
    if (g_plat_backed && g_plat_backed < size) size = g_plat_backed;               /* the rest of the address space is never touched */
    uint32_t sz = (uint32_t)((size + 0xFFFFu) & ~0xFFFFull);
    vita_log_free_memory("before the guest block");
    guest_block = sceKernelAllocMemBlock("ss2 guest", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, sz, NULL);
    if (guest_block < 0) {
        char b[160]; snprintf(b, sizeof b, "[vita] could not allocate %u MB for the guest (error %08x); lower --guest-space in ss2port.txt", sz >> 20, (unsigned)guest_block);
        plat_log_write(PLAT_LOG_ERROR, b); guest_block = -1; return NULL;
    }
    void *base = NULL; sceKernelGetMemBlockBase(guest_block, &base);
    memset(base, 0, sz); guest_base = base; guest_sz = sz;
    vita_log_free_memory("after the guest block");
    return base;
}
/* How far up the real guest block anything has been written (it starts all zero). Close to its end means the game outgrew the backed
 * part of the guest address space, and what lies beyond is not memory the game owns. */
void vita_log_guest_highwater(const char *when) {
    if (!guest_base) return;
    uint32_t top = guest_sz; const uint32_t *w = (const uint32_t *)guest_base;
    while (top >= 4096u && !w[(top >> 2) - 1]) { uint32_t i = (top >> 2) - 1024u, k; for (k = 0; k < 1024u; k++) if (w[i + k]) break; if (k < 1024u) break; top -= 4096u; }
    char b[160]; snprintf(b, sizeof b, "[vita] guest memory %s: highest used page ends near %u.%u MB of %u MB%s", when, top >> 20, ((top & 0xFFFFFu) * 10u) >> 20, guest_sz >> 20, guest_sz - top < (1u << 20) ? "  <-- AT THE END OF REAL MEMORY" : "");
    plat_log_write(PLAT_LOG_INFO, b);
}
void plat_mem_release(void *base, uint64_t size) { (void)base; (void)size; if (guest_block >= 0) { sceKernelFreeMemBlock(guest_block); guest_block = -1; } }
void plat_mem_discard(void *addr, uint64_t size) { memset(addr, 0, (size_t)size); }   /* no decommit on the Vita: just honour "reads back as zero" */

/* ---------------------------------------------------------------- time */
uint64_t plat_time_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec; }
int64_t plat_wall_time_ns(void) { struct timespec t; clock_gettime(CLOCK_REALTIME, &t); return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec; }
int plat_utc_offset_minutes(void) {
    SceRtcTick utc, local;
    if (sceRtcGetCurrentTick(&utc) < 0 || sceRtcConvertUtcToLocalTime(&utc, &local) < 0) return 0;
    return (int)(((int64_t)local.tick - (int64_t)utc.tick) / 60000000);       /* ticks are microseconds */
}
void plat_sleep_ns(uint64_t ns) { sceKernelDelayThread((SceUInt)(ns / 1000u)); }

/* ---------------------------------------------------------------- threads */
typedef struct { void (*fn)(void *); void *arg; } Start;
struct PlatMutex { pthread_mutex_t m; };
struct PlatCond { pthread_cond_t c; };
static void *thread_main(void *p) {          /* host and guest worker threads stay off core 0, which belongs to the game's main thread */
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_1 | SCE_KERNEL_CPU_MASK_USER_2);
    Start s = *(Start *)p; free(p); s.fn(s.arg); return NULL;
}
int plat_thread_start(void (*fn)(void *), void *arg, const char *name) {
    (void)name;
    Start *s = malloc(sizeof *s); s->fn = fn; s->arg = arg; pthread_t t;
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, VITA_THREAD_STACK); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    int r = pthread_create(&t, &a, thread_main, s); pthread_attr_destroy(&a);
    if (r) { free(s); return -1; }
    return 0;
}
void plat_thread_yield(void) { sceKernelDelayThread(0); }
PlatMutex *plat_mutex_new(void) { PlatMutex *m = calloc(1, sizeof *m); pthread_mutex_init(&m->m, NULL); return m; }
void plat_mutex_free(PlatMutex *m) { pthread_mutex_destroy(&m->m); free(m); }
void plat_mutex_lock(PlatMutex *m) { pthread_mutex_lock(&m->m); }
void plat_mutex_unlock(PlatMutex *m) { pthread_mutex_unlock(&m->m); }
PlatCond *plat_cond_new(void) {
    /* pthread-embedded accepts pthread_condattr_setclock but ignores it: every deadline is measured against the wall
     * clock. So no clock attribute here, and plat_cond_wait_ns builds its deadline from CLOCK_REALTIME. */
    PlatCond *c = calloc(1, sizeof *c); pthread_cond_init(&c->c, NULL); return c;
}
void plat_cond_free(PlatCond *c) { pthread_cond_destroy(&c->c); free(c); }
void plat_cond_wait(PlatCond *c, PlatMutex *m) { pthread_cond_wait(&c->c, &m->m); }
int plat_cond_wait_ns(PlatCond *c, PlatMutex *m, uint64_t ns) {
    struct timespec t; clock_gettime(CLOCK_REALTIME, &t);              /* see plat_cond_new */
    uint64_t total = (uint64_t)t.tv_nsec + ns % 1000000000u; t.tv_sec += (time_t)(ns / 1000000000u + total / 1000000000u); t.tv_nsec = (long)(total % 1000000000u);
    return pthread_cond_timedwait(&c->c, &m->m, &t) == ETIMEDOUT;
}
void plat_cond_signal(PlatCond *c) { pthread_cond_signal(&c->c); }
void plat_cond_broadcast(PlatCond *c) { pthread_cond_broadcast(&c->c); }

/* ---------------------------------------------------------------- files (newlib maps these onto sceIo) */
/* Read-only files are read through a 64 KB window: the game reads in tiny pieces and every sceIoRead has a high fixed cost.
 * Files opened for writing gather their writes in a 64 KB buffer (flushed on read, seek, close, flush); a file call over 0.5 s is logged. */
#define VITA_RDBUF (64u << 10)
#define VITA_WRBUF (64u << 10)
struct PlatFile { int fd; int ro; int64_t pos, size; uint8_t *buf; int64_t bstart; uint32_t blen; uint8_t *wbuf; uint32_t wlen; int werr; char name[96]; };
/* what the file layer costs (reported with the frame rate by vita_profile_report in plat_vita.c) */
void vita_profile_tick(uint64_t now);
uint64_t vp_fs_ns, vp_fs_open, vp_fs_stat, vp_fs_read_calls, vp_fs_sys_reads, vp_fs_bytes;
uint64_t vita_now_ns(void);
static inline uint64_t vp_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec; }
uint64_t vita_now_ns(void) { return vp_now(); }
struct PlatDir { DIR *d; };
/* the file call in progress (for the hang watchdog): what, which file, how many bytes, since when */
static volatile int op_on; static char op_what[16], op_name[96]; static volatile uint64_t op_bytes, op_t0;
static void op_begin(const char *what, const char *name, uint64_t bytes) { snprintf(op_what, sizeof op_what, "%s", what); snprintf(op_name, sizeof op_name, "%s", name); op_bytes = bytes; op_t0 = vp_now(); op_on = 1; }
static void op_end(void) { op_on = 0; }
void vita_log_inflight(void) {
    char b[240]; if (!op_on) { plat_log_write(PLAT_LOG_INFO, "[vita] no file call in progress"); return; }
    snprintf(b, sizeof b, "[vita] file call in progress: %s '%s' (%llu bytes) for %u s", op_what, op_name, (unsigned long long)op_bytes, (unsigned)((vp_now() - op_t0) / 1000000000u)); plat_log_write(PLAT_LOG_WARN, b);
}
static int err_of(int e) {
    switch (e) { case ENOENT: return PLAT_E_NOENT; case EEXIST: return PLAT_E_EXIST; case EACCES: case EPERM: case EROFS: return PLAT_E_ACCESS;
                 case ENOTDIR: return PLAT_E_NOTDIR; case EISDIR: return PLAT_E_ISDIR; case ENOTEMPTY: return PLAT_E_NOTEMPTY; default: return PLAT_E_IO; }
}
static void slow_note(const char *what, const char *name, uint64_t ns);
PlatFile *plat_fs_open(const char *path, int flags, int *err) {
    uint64_t t0 = vp_now(); vp_fs_open++;
    int of = (flags & PLAT_READ) && (flags & PLAT_WRITE) ? O_RDWR : (flags & PLAT_WRITE) ? O_WRONLY : O_RDONLY;
    if (flags & PLAT_CREATE) of |= O_CREAT; if (flags & PLAT_TRUNCATE) of |= O_TRUNC; if (flags & PLAT_EXCLUSIVE) of |= O_EXCL; if (flags & PLAT_APPEND) of |= O_APPEND;
    op_begin("open", path, 0); int fd = open(path, of, 0644); op_end();
    if (fd < 0) { if (err) *err = err_of(errno); vp_fs_ns += vp_now() - t0; return NULL; }
    struct stat st; if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) { close(fd); if (err) *err = PLAT_E_ISDIR; vp_fs_ns += vp_now() - t0; return NULL; }   /* folders are not files */
    PlatFile *f = calloc(1, sizeof *f); snprintf(f->name, sizeof f->name, "%s", path); f->fd = fd; f->ro = of == O_RDONLY; f->size = (int64_t)st.st_size; f->bstart = -1;
    if (f->ro) f->buf = malloc(VITA_RDBUF);
    if (f->ro && !f->buf) f->ro = 0;
    if (!f->ro) f->wbuf = malloc(VITA_WRBUF);               /* tiny writes each cost a full sceIoWrite: gather them */
    uint64_t t1 = vp_now(); vp_fs_ns += t1 - t0; if (t1 - t0 > 500000000u) slow_note("open", path, t1 - t0); return f;
}
/* a file call that takes long is named in the log, so a hang can be traced to its file */
static void slow_note(const char *what, const char *name, uint64_t ns) {
    char b[200]; snprintf(b, sizeof b, "[vita] slow file call: %s '%s' took %u ms", what, name, (unsigned)(ns / 1000000u)); plat_log_write(PLAT_LOG_WARN, b);
}
static int flush_w(PlatFile *f) {                           /* pending writes go to the file at its current position */
    if (!f->wlen) return 0;
    uint64_t t0 = vp_now(); uint32_t done = 0; op_begin("write", f->name, f->wlen);
    while (done < f->wlen) { ssize_t r = write(f->fd, f->wbuf + done, f->wlen - done); if (r < 0) { if (errno == EINTR) continue; f->werr = 1; break; } done += (uint32_t)r; }
    op_end(); f->wlen = 0; uint64_t d = vp_now() - t0; vp_fs_ns += d; if (d > 500000000u) slow_note("write", f->name, d);
    return f->werr ? -1 : 0;
}
void plat_fs_close(PlatFile *f) { if (f) { flush_w(f); close(f->fd); free(f->buf); free(f->wbuf); free(f); } }
static int64_t raw_read(int fd, void *buf, uint64_t n) {
    uint64_t got = 0; while (got < n) { vp_fs_sys_reads++; ssize_t r = read(fd, (char *)buf + got, (size_t)(n - got)); if (r < 0) { if (errno == EINTR) continue; return got ? (int64_t)got : -1; } if (!r) break; got += (uint64_t)r; }
    return (int64_t)got;
}
int64_t plat_fs_read(PlatFile *f, void *buf, uint64_t n) {
    uint64_t t0 = vp_now(); vp_fs_read_calls++; op_begin("read", f->name, n);
    int64_t out;
    if (!f->ro) { flush_w(f); out = raw_read(f->fd, buf, n); }
    else {
        uint8_t *d = buf; uint64_t got = 0;
        while (got < n && f->pos < f->size) {
            if (f->bstart >= 0 && f->pos >= f->bstart && f->pos < f->bstart + f->blen) {          /* from the window */
                uint64_t k = (uint64_t)(f->bstart + f->blen - f->pos); if (k > n - got) k = n - got;
                memcpy(d + got, f->buf + (f->pos - f->bstart), (size_t)k); f->pos += (int64_t)k; got += k; continue;
            }
            if (n - got >= VITA_RDBUF) {                                                            /* a big read goes straight to the file */
                if (lseek(f->fd, (off_t)f->pos, SEEK_SET) < 0) break;
                int64_t r = raw_read(f->fd, d + got, n - got); if (r <= 0) break; f->pos += r; got += (uint64_t)r; f->bstart = -1; continue;
            }
            if (lseek(f->fd, (off_t)f->pos, SEEK_SET) < 0) break;                                   /* refill the window at the current position */
            int64_t r = raw_read(f->fd, f->buf, VITA_RDBUF); if (r <= 0) break; f->bstart = f->pos; f->blen = (uint32_t)r;
        }
        out = (int64_t)got;
    }
    op_end(); if (out > 0) vp_fs_bytes += (uint64_t)out;
    uint64_t t1 = vp_now(); vp_fs_ns += t1 - t0; vita_profile_tick(t1); return out;      /* also reports while loading, when no frames are drawn */
}
int64_t plat_fs_write(PlatFile *f, const void *buf, uint64_t n) {
    if (f->werr) return -1;
    if (f->wbuf && n < VITA_WRBUF) {
        if (f->wlen + n > VITA_WRBUF && flush_w(f)) return -1;
        memcpy(f->wbuf + f->wlen, buf, (size_t)n); f->wlen += (uint32_t)n; return (int64_t)n;
    }
    if (flush_w(f)) return -1;
    op_begin("write", f->name, n);
    uint64_t done = 0; while (done < n) { ssize_t r = write(f->fd, (const char *)buf + done, (size_t)(n - done)); if (r < 0) { if (errno == EINTR) continue; op_end(); return done ? (int64_t)done : -1; } done += (uint64_t)r; }
    op_end(); return (int64_t)done;
}
int64_t plat_fs_seek(PlatFile *f, int64_t off, int whence) {
    if (f->ro) { int64_t np = whence == PLAT_SEEK_SET ? off : whence == PLAT_SEEK_CUR ? f->pos + off : f->size + off; if (np < 0) return -1; f->pos = np; return np; }       /* buffered: just move the logical position */
    if (f->wlen && whence == PLAT_SEEK_CUR && off == 0) { off_t c = lseek(f->fd, 0, SEEK_CUR); return c < 0 ? -1 : (int64_t)c + f->wlen; }    /* position query: no flush */
    flush_w(f);
    off_t r = lseek(f->fd, (off_t)off, whence == PLAT_SEEK_SET ? SEEK_SET : whence == PLAT_SEEK_CUR ? SEEK_CUR : SEEK_END); return r < 0 ? -1 : (int64_t)r;
}
int plat_fs_truncate(PlatFile *f, uint64_t size) { flush_w(f); return ftruncate(f->fd, (off_t)size) ? PLAT_E_IO : PLAT_OK; }
int plat_fs_flush(PlatFile *f) { return flush_w(f) ? PLAT_E_IO : PLAT_OK; }
static void fill_stat(const struct stat *s, PlatStat *o) {
    o->size = (uint64_t)s->st_size; o->is_dir = S_ISDIR(s->st_mode); o->readonly = !(s->st_mode & S_IWUSR);
    /* the Vita's newlib has plain time_t fields (SVR4 layout), no timespec members: whole seconds only */
    o->mtime_ns = (int64_t)s->st_mtime * 1000000000; o->atime_ns = (int64_t)s->st_atime * 1000000000; o->ctime_ns = (int64_t)s->st_ctime * 1000000000;
}
int plat_fs_fstat(PlatFile *f, PlatStat *st) { flush_w(f); struct stat s; if (fstat(f->fd, &s)) return err_of(errno); fill_stat(&s, st); return PLAT_OK; }
int plat_fs_stat(const char *path, PlatStat *st) { uint64_t t0 = vp_now(); vp_fs_stat++; struct stat s; int bad = stat(path, &s); int e = errno; vp_fs_ns += vp_now() - t0; if (bad) return err_of(e); fill_stat(&s, st); return PLAT_OK; }
int plat_fs_mkdir(const char *path) { return mkdir(path, 0755) ? err_of(errno) : PLAT_OK; }
int plat_fs_rmdir(const char *path) { return rmdir(path) ? err_of(errno) : PLAT_OK; }
int plat_fs_remove(const char *path) { struct stat s; if (!stat(path, &s) && S_ISDIR(s.st_mode)) return PLAT_E_ISDIR; return unlink(path) ? err_of(errno) : PLAT_OK; }
int plat_fs_rename(const char *from, const char *to) { return rename(from, to) ? err_of(errno) : PLAT_OK; }
PlatDir *plat_fs_opendir(const char *path) { DIR *d = opendir(path); if (!d) return NULL; PlatDir *p = malloc(sizeof *p); p->d = d; return p; }
const char *plat_fs_readdir(PlatDir *d) { struct dirent *e; while ((e = readdir(d->d))) if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) return e->d_name; return NULL; }
void plat_fs_closedir(PlatDir *d) { if (d) { closedir(d->d); free(d); } }
const char *plat_fs_write_dir(void) { const char *w = getenv("SS2PORT_WRITE_DIR"); return w && *w ? w : NULL; }
