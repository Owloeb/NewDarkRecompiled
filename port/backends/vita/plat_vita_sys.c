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
 * A file created empty for writing (a save, the level state in current/) is kept in memory and written out in one go on close or
 * flush: the game writes those in small pieces with a seek between them, and each sceIoWrite/lseek costs milliseconds.
 * Other files opened for writing gather their writes in a 64 KB buffer (flushed on read, seek, close, flush).
 * A file call over 0.5 s is logged. */
#define VITA_MEMFILE_MAX (32u << 20)
#define VITA_RDBUF (64u << 10)
#define VITA_WRBUF (64u << 10)
struct PlatFile { int fd, pfd, warm; int64_t lastend; int ro; int64_t pos, size; uint8_t *buf; int64_t bstart; uint32_t blen; uint8_t *wbuf; uint32_t wlen; int werr; int mem, dirty; uint8_t *mbuf; uint64_t mcap; char name[96]; };
/* what the file layer costs (reported with the frame rate by vita_profile_report in plat_vita.c) */
void vita_profile_tick(uint64_t now);
uint64_t vp_fs_ns, vp_fs_open, vp_fs_stat, vp_fs_read_calls, vp_fs_sys_reads, vp_fs_bytes;
volatile uint64_t vp_fs_activity;                           /* any file call: the hang watchdog counts these as progress */
uint64_t vita_now_ns(void);
static inline uint64_t vp_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec; }
uint64_t vita_now_ns(void) { return vp_now(); }
struct PlatDir { DIR *d; };
/* the file call in progress (for the hang watchdog): what, which file, how many bytes, since when */
/* one slot per kind: reads run on several threads at once, writes and opens only on the thread holding the guest lock */
enum { OP_READ, OP_WRITE, OP_OPEN, OP_KINDS };
static const char *const op_kind[OP_KINDS] = { "read", "write", "open" };
static volatile int op_on[OP_KINDS]; static char op_name[OP_KINDS][96]; static volatile uint64_t op_bytes[OP_KINDS], op_t0[OP_KINDS];
static void op_begin(int k, const char *name, uint64_t bytes) { snprintf(op_name[k], sizeof op_name[k], "%s", name); op_bytes[k] = bytes; op_t0[k] = vp_now(); op_on[k] = 1; }
static void op_end(int k) { op_on[k] = 0; }
/* every open file, so the watchdog can list them */
#define VITA_MAXFILES 128
static PlatFile *open_files[VITA_MAXFILES]; static pthread_mutex_t open_m = PTHREAD_MUTEX_INITIALIZER;
static void reg_file(PlatFile *f, int add) {
    pthread_mutex_lock(&open_m);
    for (int i = 0; i < VITA_MAXFILES; i++) if (add ? !open_files[i] : open_files[i] == f) { open_files[i] = add ? f : NULL; break; }
    pthread_mutex_unlock(&open_m);
}
void vita_log_inflight(void) {
    char b[240]; int any = 0;
    for (int k = 0; k < OP_KINDS; k++) if (op_on[k]) {
        any = 1; snprintf(b, sizeof b, "[vita] file call in progress: %s '%s' (%llu bytes) for %u s", op_kind[k], op_name[k], (unsigned long long)op_bytes[k], (unsigned)((vp_now() - op_t0[k]) / 1000000000u)); plat_log_write(PLAT_LOG_WARN, b);
    }
    if (!any) plat_log_write(PLAT_LOG_INFO, "[vita] no file call in progress");
    pthread_mutex_lock(&open_m);
    for (int i = 0; i < VITA_MAXFILES; i++) if (open_files[i]) {
        PlatFile *f = open_files[i]; snprintf(b, sizeof b, "[vita] open file: '%s' %s (fd %08x, %u bytes not yet written)", f->name, f->ro ? "read-only" : "writable", (unsigned)f->fd, f->wlen); plat_log_write(PLAT_LOG_INFO, b);
    }
    pthread_mutex_unlock(&open_m);
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
    op_begin(OP_OPEN, path, 0); int fd = open(path, of, 0644); op_end(OP_OPEN);
    if (fd < 0) { if (err) *err = err_of(errno); vp_fs_ns += vp_now() - t0; return NULL; }
    struct stat st; if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) { close(fd); if (err) *err = PLAT_E_ISDIR; vp_fs_ns += vp_now() - t0; return NULL; }   /* folders are not files */
    PlatFile *f = calloc(1, sizeof *f); snprintf(f->name, sizeof f->name, "%s", path); f->fd = fd; f->pfd = -1; f->ro = of == O_RDONLY; f->size = (int64_t)st.st_size; f->bstart = -1;
    if (f->ro) f->buf = malloc(VITA_RDBUF);
    if (f->ro && !f->buf) f->ro = 0;
    if (!f->ro) f->wbuf = malloc(VITA_WRBUF);
    if (!f->ro && st.st_size == 0 && !(flags & PLAT_APPEND)) f->mem = 1;
    reg_file(f, 1); vp_fs_activity++;
    if (!f->ro) { char b[200]; snprintf(b, sizeof b, "[vita] file: opened for writing '%s' (flags %x, fd %08x, %lld bytes%s)", path, flags, (unsigned)fd, (long long)f->size, f->mem ? ", kept in memory" : ""); plat_log_write(PLAT_LOG_INFO, b); }               /* tiny writes each cost a full sceIoWrite: gather them */
    uint64_t t1 = vp_now(); vp_fs_ns += t1 - t0; if (t1 - t0 > 500000000u) slow_note("open", path, t1 - t0); return f;
}
/* a file call that takes long is named in the log, so a hang can be traced to its file */
static void slow_note(const char *what, const char *name, uint64_t ns) {
    char b[200]; snprintf(b, sizeof b, "[vita] slow file call: %s '%s' took %u ms", what, name, (unsigned)(ns / 1000000u)); plat_log_write(PLAT_LOG_WARN, b);
}
static int mem_out(PlatFile *f) {                          /* a memory file goes to disk as a whole */
    if (!f->mem || !f->dirty) return 0;
    uint64_t t0 = vp_now(); op_begin(OP_WRITE, f->name, (uint64_t)f->size); int bad = 0;
    if (lseek(f->fd, 0, SEEK_SET) < 0) bad = 1;
    uint64_t done = 0; while (!bad && done < (uint64_t)f->size) { ssize_t r = write(f->fd, f->mbuf + done, (size_t)((uint64_t)f->size - done)); if (r < 0) { if (errno == EINTR) continue; bad = 1; break; } done += (uint64_t)r; }
    if (!bad && ftruncate(f->fd, (off_t)f->size)) bad = 1;
    op_end(OP_WRITE); uint64_t d = vp_now() - t0; vp_fs_ns += d; if (d > 500000000u) slow_note("write", f->name, d);
    if (bad) { f->werr = 1; return -1; }
    f->dirty = 0; return 0;
}
static int mem_room(PlatFile *f, uint64_t need) {          /* 0, or -1 when the file has to leave memory */
    if (need <= f->mcap) return 0;
    if (need > VITA_MEMFILE_MAX) return -1;
    uint64_t c = f->mcap ? f->mcap : 65536; while (c < need) c *= 2; if (c > VITA_MEMFILE_MAX) c = VITA_MEMFILE_MAX;
    uint8_t *n = realloc(f->mbuf, (size_t)c); if (!n) return -1;
    memset(n + f->mcap, 0, (size_t)(c - f->mcap)); f->mbuf = n; f->mcap = c; return 0;
}
static int mem_leave(PlatFile *f) {                         /* too big for memory: write it out, carry on as a plain file */
    f->dirty = 1; if (mem_out(f)) return -1;
    f->mem = 0; free(f->mbuf); f->mbuf = NULL; f->mcap = 0;
    return lseek(f->fd, (off_t)f->pos, SEEK_SET) < 0 ? -1 : 0;
}
static int flush_w(PlatFile *f) {                           /* pending writes go to the file at its current position */
    if (!f->wlen) return 0;
    uint64_t t0 = vp_now(); uint32_t done = 0; op_begin(OP_WRITE, f->name, f->wlen);
    while (done < f->wlen) { ssize_t r = write(f->fd, f->wbuf + done, f->wlen - done); if (r < 0) { if (errno == EINTR) continue; f->werr = 1; break; } done += (uint32_t)r; }
    op_end(OP_WRITE); f->wlen = 0; uint64_t d = vp_now() - t0; vp_fs_ns += d; if (d > 500000000u) slow_note("write", f->name, d);
    return f->werr ? -1 : 0;
}
static void pf_forget(PlatFile *f);
static int64_t pf_take(PlatFile *f);
static void pf_post(PlatFile *f, int64_t off);
static void rs_note(PlatFile *f, uint64_t ns, int64_t bytes, int seq, unsigned refills, unsigned adj, unsigned hits);
void plat_fs_close(PlatFile *f) {
    if (!f) return;
    if (!f->ro) { char b[160]; snprintf(b, sizeof b, "[vita] file: closing '%s' (%u bytes to write)", f->name, f->mem && f->dirty ? (unsigned)f->size : f->wlen); plat_log_write(PLAT_LOG_INFO, b); }
    reg_file(f, 0); vp_fs_activity++;
    pf_forget(f); mem_out(f); flush_w(f); close(f->fd); free(f->buf); free(f->wbuf); free(f->mbuf); free(f);
}

/* ---- read-ahead: when a file is read straight through (each window starts where the last one ended), a worker thread reads the
 * next 64 KB window while the game works on this one. One request at a time, one shared buffer, the worker has its own handle per file.
 * Also per-file read statistics, logged every 15 s: which files cost the time, and how much of it is sequential. */
int vita_readahead = 1;
static struct { PlatFile *f; int64_t off; uint32_t want; int state; int64_t got; uint8_t *buf; int started; } pf;   /* state: 0 idle, 1 reading, 2 done */
static pthread_mutex_t pf_m = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t pf_c = PTHREAD_COND_INITIALIZER;
static void *pf_worker(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&pf_m);
        while (pf.state != 1) pthread_cond_wait(&pf_c, &pf_m);
        PlatFile *f = pf.f; int64_t off = pf.off; uint32_t want = pf.want;
        if (f->pfd < 0) f->pfd = open(f->name, O_RDONLY);
        int fd = f->pfd; pthread_mutex_unlock(&pf_m);
        int64_t got = -1;
        if (fd >= 0 && lseek(fd, (off_t)off, SEEK_SET) >= 0) {
            got = 0; while ((uint64_t)got < want) { ssize_t r = read(fd, pf.buf + got, want - (uint32_t)got); if (r < 0 && errno == EINTR) continue; if (r <= 0) break; got += r; }
        }
        pthread_mutex_lock(&pf_m); pf.got = got; pf.state = 2; pthread_mutex_unlock(&pf_m);
    }
    return NULL;
}
static void pf_post(PlatFile *f, int64_t off) {
    if (!vita_readahead || off >= f->size) return;
    pthread_mutex_lock(&pf_m);
    if (pf.state != 1) {
        if (!pf.started) {
            pf.buf = malloc(VITA_RDBUF); pthread_t t; pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 64u << 10);
            if (pf.buf && !pthread_create(&t, &a, pf_worker, NULL)) pf.started = 1; pthread_attr_destroy(&a);
        }
        if (pf.started) { pf.f = f; pf.off = off; pf.want = (uint32_t)(f->size - off < VITA_RDBUF ? f->size - off : VITA_RDBUF); pf.state = 1; pthread_cond_signal(&pf_c); }
    }
    pthread_mutex_unlock(&pf_m);
}
static int64_t pf_take(PlatFile *f) {                       /* the window at f->pos, if it was read ahead (waits if it is on its way) */
    int64_t r = -1; pthread_mutex_lock(&pf_m);
    while (pf.f == f && pf.off == f->pos && pf.state == 1) { pthread_mutex_unlock(&pf_m); sceKernelDelayThread(300); pthread_mutex_lock(&pf_m); }
    if (pf.f == f && pf.off == f->pos && pf.state == 2 && pf.got > 0) { memcpy(f->buf, pf.buf, (size_t)pf.got); r = pf.got; pf.state = 0; pf.f = NULL; }
    pthread_mutex_unlock(&pf_m); return r;
}
static void pf_forget(PlatFile *f) {                        /* before the file closes */
    pthread_mutex_lock(&pf_m);
    while (pf.f == f && pf.state == 1) { pthread_mutex_unlock(&pf_m); sceKernelDelayThread(300); pthread_mutex_lock(&pf_m); }
    if (pf.f == f) { pf.f = NULL; pf.state = 0; }
    int pfd = f->pfd; f->pfd = -1; pthread_mutex_unlock(&pf_m); if (pfd >= 0) close(pfd);
}
typedef struct { char name[48]; uint64_t calls, bytes, ns, seq, refills, adj, hits; } RStat;
static RStat rst[64]; static int nrst; static pthread_mutex_t rst_m = PTHREAD_MUTEX_INITIALIZER;
static void rs_note(PlatFile *f, uint64_t ns, int64_t bytes, int seq, unsigned refills, unsigned adj, unsigned hits) {
    const char *b = strrchr(f->name, '/'); b = b ? b + 1 : f->name; int i;
    pthread_mutex_lock(&rst_m);
    for (i = 0; i < nrst && strncmp(rst[i].name, b, sizeof rst[i].name - 1); i++) {}
    if (i == nrst && nrst < 64) { snprintf(rst[i].name, sizeof rst[i].name, "%s", b); nrst++; }
    if (i < nrst) { RStat *r = &rst[i]; r->calls++; r->bytes += bytes > 0 ? (uint64_t)bytes : 0; r->ns += ns; r->seq += (uint64_t)seq; r->refills += refills; r->adj += adj; r->hits += hits; }
    pthread_mutex_unlock(&rst_m);
}
void vita_log_readstats(void) {                             /* the six files that cost the most read time since the last call */
    pthread_mutex_lock(&rst_m);
    for (int k = 0; k < 6; k++) {
        int best = -1; for (int i = 0; i < nrst; i++) if (rst[i].ns && (best < 0 || rst[i].ns > rst[best].ns)) best = i;
        if (best < 0) break; RStat *r = &rst[best]; char b[260];
        snprintf(b, sizeof b, "[vita] reads %-24s %5.0f ms | %llu calls, %.1f MB | %llu%% sequential | %llu window refills, %llu in sequence, %llu read ahead",
                 r->name, (double)r->ns / 1e6, (unsigned long long)r->calls, (double)r->bytes / 1048576.0, (unsigned long long)(r->calls ? 100 * r->seq / r->calls : 0),
                 (unsigned long long)r->refills, (unsigned long long)r->adj, (unsigned long long)r->hits);
        plat_log_write(PLAT_LOG_INFO, b); r->ns = 0;
    }
    for (int i = 0; i < nrst; i++) { rst[i].calls = rst[i].bytes = rst[i].ns = rst[i].seq = rst[i].refills = rst[i].adj = rst[i].hits = 0; }
    pthread_mutex_unlock(&rst_m);
}
static int64_t raw_read(int fd, void *buf, uint64_t n) {
    uint64_t got = 0; while (got < n) { vp_fs_sys_reads++; ssize_t r = read(fd, (char *)buf + got, (size_t)(n - got)); if (r < 0) { if (errno == EINTR) continue; return got ? (int64_t)got : -1; } if (!r) break; got += (uint64_t)r; }
    return (int64_t)got;
}
int64_t plat_fs_read(PlatFile *f, void *buf, uint64_t n) {
    uint64_t t0 = vp_now(); vp_fs_read_calls++; op_begin(OP_READ, f->name, n);
    int64_t out; unsigned refills = 0, adjn = 0, hitn = 0; int seq = f->pos == f->lastend;
    if (f->mem) { int64_t k = f->size - f->pos; if (k < 0) k = 0; if ((uint64_t)k > n) k = (int64_t)n; if (k) memcpy(buf, f->mbuf + f->pos, (size_t)k); f->pos += k; out = k; }
    else if (!f->ro) { flush_w(f); out = raw_read(f->fd, buf, n); }
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
            int adj = f->bstart >= 0 && f->pos == f->bstart + (int64_t)f->blen; refills++; adjn += adj;   /* sequential: the next window starts where this one ended */
            int64_t r = adj ? pf_take(f) : -1;                                                       /* already read ahead? */
            if (r > 0) hitn++;
            else {
                if (lseek(f->fd, (off_t)f->pos, SEEK_SET) < 0) break;                                /* refill the window at the current position */
                uint32_t len = VITA_RDBUF;                                                          /* a file read at random gets what was asked for (the card moves ~10 MB/s, so a 64 KB window for a 30 KB request wastes half the time) */
                if (!adj && !f->warm) { uint64_t need = n - got; len = (uint32_t)((need + 4095u) & ~4095ull); if (len < (16u << 10)) len = 16u << 10; if (len > VITA_RDBUF) len = VITA_RDBUF; }
                r = raw_read(f->fd, f->buf, len); if (r <= 0) break;
            }
            f->warm = adj;
            f->bstart = f->pos; f->blen = (uint32_t)r;
            if (adj) pf_post(f, f->bstart + (int64_t)f->blen);                                       /* start on the window after it */
        }
        out = (int64_t)got;
    }
    op_end(OP_READ); if (out > 0) vp_fs_bytes += (uint64_t)out;
    f->lastend = f->pos;
    uint64_t t1 = vp_now(); vp_fs_ns += t1 - t0; rs_note(f, t1 - t0, out, seq, refills, adjn, hitn); vita_profile_tick(t1); return out;      /* also reports while loading, when no frames are drawn */
}
int64_t plat_fs_write(PlatFile *f, const void *buf, uint64_t n) {
    if (f->werr) return -1;
    vp_fs_activity++;
    if (!n) return 0;
    if (f->mem) {
        if (!mem_room(f, (uint64_t)f->pos + n)) {           /* a gap left by a seek past the end reads back as zeros (mem_room clears new space) */
            memcpy(f->mbuf + f->pos, buf, (size_t)n); f->pos += (int64_t)n; if (f->pos > f->size) f->size = f->pos; f->dirty = 1; return (int64_t)n;
        }
        if (mem_leave(f)) return -1;
    }
    if (f->wbuf && n < VITA_WRBUF) {
        if (f->wlen + n > VITA_WRBUF && flush_w(f)) return -1;
        memcpy(f->wbuf + f->wlen, buf, (size_t)n); f->wlen += (uint32_t)n; return (int64_t)n;
    }
    if (flush_w(f)) return -1;
    op_begin(OP_WRITE, f->name, n);
    uint64_t done = 0; while (done < n) { ssize_t r = write(f->fd, (const char *)buf + done, (size_t)(n - done)); if (r < 0) { if (errno == EINTR) continue; op_end(OP_WRITE); return done ? (int64_t)done : -1; } done += (uint64_t)r; }
    op_end(OP_WRITE); return (int64_t)done;
}
int64_t plat_fs_seek(PlatFile *f, int64_t off, int whence) {
    vp_fs_activity++;
    if (f->ro || f->mem) { int64_t np = whence == PLAT_SEEK_SET ? off : whence == PLAT_SEEK_CUR ? f->pos + off : f->size + off; if (np < 0) return -1; f->pos = np; return np; }       /* buffered: just move the logical position */
    if (f->wlen && whence == PLAT_SEEK_CUR && off == 0) { off_t c = lseek(f->fd, 0, SEEK_CUR); return c < 0 ? -1 : (int64_t)c + f->wlen; }    /* position query: no flush */
    flush_w(f);
    off_t r = lseek(f->fd, (off_t)off, whence == PLAT_SEEK_SET ? SEEK_SET : whence == PLAT_SEEK_CUR ? SEEK_CUR : SEEK_END);
    if (r > (off_t)(64 << 20)) { char m[200]; snprintf(m, sizeof m, "[vita] file: seek to %lld in '%s' (a write there fills the gap)", (long long)r, f->name); plat_log_write(PLAT_LOG_WARN, m); }
    return r < 0 ? -1 : (int64_t)r;
}
int plat_fs_truncate(PlatFile *f, uint64_t size) {
    if (f->mem) {
        if (size > (uint64_t)f->size && mem_room(f, size) && mem_leave(f)) return PLAT_E_IO;
        if (f->mem) { if (size < (uint64_t)f->size) memset(f->mbuf + size, 0, (size_t)((uint64_t)f->size - size)); f->size = (int64_t)size; f->dirty = 1; return PLAT_OK; }
    }
    flush_w(f); return ftruncate(f->fd, (off_t)size) ? PLAT_E_IO : PLAT_OK; }
int plat_fs_flush(PlatFile *f) { return mem_out(f) || flush_w(f) ? PLAT_E_IO : PLAT_OK; }
static void fill_stat(const struct stat *s, PlatStat *o) {
    o->size = (uint64_t)s->st_size; o->is_dir = S_ISDIR(s->st_mode); o->readonly = !(s->st_mode & S_IWUSR);
    /* the Vita's newlib has plain time_t fields (SVR4 layout), no timespec members: whole seconds only */
    o->mtime_ns = (int64_t)s->st_mtime * 1000000000; o->atime_ns = (int64_t)s->st_atime * 1000000000; o->ctime_ns = (int64_t)s->st_ctime * 1000000000;
}
int plat_fs_fstat(PlatFile *f, PlatStat *st) { flush_w(f); struct stat s; if (fstat(f->fd, &s)) return err_of(errno); fill_stat(&s, st); if (f->mem) st->size = (uint64_t)f->size; return PLAT_OK; }
int plat_fs_stat(const char *path, PlatStat *st) { uint64_t t0 = vp_now(); vp_fs_stat++; vp_fs_activity++; struct stat s; int bad = stat(path, &s); int e = errno; vp_fs_ns += vp_now() - t0; if (bad) return err_of(e); fill_stat(&s, st); return PLAT_OK; }
int plat_fs_mkdir(const char *path) { return mkdir(path, 0755) ? err_of(errno) : PLAT_OK; }
int plat_fs_rmdir(const char *path) { return rmdir(path) ? err_of(errno) : PLAT_OK; }
int plat_fs_remove(const char *path) { struct stat s; if (!stat(path, &s) && S_ISDIR(s.st_mode)) return PLAT_E_ISDIR; return unlink(path) ? err_of(errno) : PLAT_OK; }
int plat_fs_rename(const char *from, const char *to) { return rename(from, to) ? err_of(errno) : PLAT_OK; }
PlatDir *plat_fs_opendir(const char *path) { DIR *d = opendir(path); if (!d) return NULL; PlatDir *p = malloc(sizeof *p); p->d = d; return p; }
const char *plat_fs_readdir(PlatDir *d) { struct dirent *e; while ((e = readdir(d->d))) if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) return e->d_name; return NULL; }
void plat_fs_closedir(PlatDir *d) { if (d) { closedir(d->d); free(d); } }
const char *plat_fs_write_dir(void) { const char *w = getenv("SS2PORT_WRITE_DIR"); return w && *w ? w : NULL; }
