/* plat_posix.c: the "sys" and "fs" parts of plat.h for POSIX systems (Linux, macOS, the BSDs, and most console and
 * handheld SDKs with a POSIX layer). Backends for windows, input, audio and rendering are separate files. */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <pthread.h>
#include <sched.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include "plat.h"

/* ---------------------------------------------------------------- log */
static pthread_mutex_t log_m = PTHREAD_MUTEX_INITIALIZER;
void plat_log_write(PlatLogLevel level, const char *line) {
    pthread_mutex_lock(&log_m);
    FILE *o = level >= PLAT_LOG_WARN ? stderr : stdout;
    fputs(line, o); fputc('\n', o); fflush(o);
    pthread_mutex_unlock(&log_m);
}


/* ---------------------------------------------------------------- memory */
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif
void *plat_mem_reserve(uint64_t size) {
    void *p = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}
void plat_mem_release(void *base, uint64_t size) { munmap(base, (size_t)size); }
void plat_mem_discard(void *addr, uint64_t size) {
#if defined(__linux__) && defined(MADV_DONTNEED)
    madvise(addr, (size_t)size, MADV_DONTNEED);      /* Linux: private anonymous memory reads back as zero afterwards */
#else
    /* elsewhere MADV_DONTNEED may keep the old contents: map fresh zero pages over the page-aligned middle, clear the ends */
    long pg = sysconf(_SC_PAGESIZE); uintptr_t a = (uintptr_t)addr, e = a + (uintptr_t)size, lo = (a + (uintptr_t)pg - 1) & ~((uintptr_t)pg - 1), hi = e & ~((uintptr_t)pg - 1);
    if (hi <= lo || mmap((void *)lo, hi - lo, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0) == MAP_FAILED) { memset(addr, 0, (size_t)size); return; }
    if (lo > a) memset(addr, 0, lo - a);
    if (e > hi) memset((void *)hi, 0, e - hi);
#endif
}

/* ---------------------------------------------------------------- time */
uint64_t plat_time_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec; }
int64_t plat_wall_time_ns(void) { struct timespec t; clock_gettime(CLOCK_REALTIME, &t); return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec; }
int plat_utc_offset_minutes(void) { time_t now = time(NULL); struct tm l; localtime_r(&now, &l); return (int)(l.tm_gmtoff / 60); }
void plat_sleep_ns(uint64_t ns) { struct timespec t = { (time_t)(ns / 1000000000u), (long)(ns % 1000000000u) }; while (nanosleep(&t, &t) && errno == EINTR) ; }

/* ---------------------------------------------------------------- threads */
typedef struct { void (*fn)(void *); void *arg; } Start;
struct PlatMutex { pthread_mutex_t m; };
struct PlatCond { pthread_cond_t c; };
static void *thread_main(void *p) { Start s = *(Start *)p; free(p); s.fn(s.arg); return NULL; }
int plat_thread_start(void (*fn)(void *), void *arg, const char *name) {
    Start *s = malloc(sizeof *s); s->fn = fn; s->arg = arg; pthread_t t;
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 8u << 20); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    int r = pthread_create(&t, &a, thread_main, s); pthread_attr_destroy(&a);
    if (r) { free(s); return -1; }
#if defined(__linux__)
    if (name) { char n[16]; snprintf(n, sizeof n, "%s", name); pthread_setname_np(t, n); }
#else
    (void)name;
#endif
    return 0;
}
void plat_thread_yield(void) { sched_yield(); }
PlatMutex *plat_mutex_new(void) { PlatMutex *m = calloc(1, sizeof *m); pthread_mutex_init(&m->m, NULL); return m; }
void plat_mutex_free(PlatMutex *m) { pthread_mutex_destroy(&m->m); free(m); }
void plat_mutex_lock(PlatMutex *m) { pthread_mutex_lock(&m->m); }
void plat_mutex_unlock(PlatMutex *m) { pthread_mutex_unlock(&m->m); }
PlatCond *plat_cond_new(void) {
    PlatCond *c = calloc(1, sizeof *c); pthread_condattr_t a; pthread_condattr_init(&a);
#if !defined(__APPLE__)
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
#endif
    pthread_cond_init(&c->c, &a); pthread_condattr_destroy(&a); return c;
}
void plat_cond_free(PlatCond *c) { pthread_cond_destroy(&c->c); free(c); }
void plat_cond_wait(PlatCond *c, PlatMutex *m) { pthread_cond_wait(&c->c, &m->m); }
int plat_cond_wait_ns(PlatCond *c, PlatMutex *m, uint64_t ns) {
#if defined(__APPLE__)
    struct timespec rel = { (time_t)(ns / 1000000000u), (long)(ns % 1000000000u) };
    return pthread_cond_timedwait_relative_np(&c->c, &m->m, &rel) == ETIMEDOUT;
#else
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    uint64_t total = (uint64_t)t.tv_nsec + ns % 1000000000u; t.tv_sec += (time_t)(ns / 1000000000u + total / 1000000000u); t.tv_nsec = (long)(total % 1000000000u);
    return pthread_cond_timedwait(&c->c, &m->m, &t) == ETIMEDOUT;
#endif
}
void plat_cond_signal(PlatCond *c) { pthread_cond_signal(&c->c); }
void plat_cond_broadcast(PlatCond *c) { pthread_cond_broadcast(&c->c); }

/* ---------------------------------------------------------------- files */
struct PlatFile { int fd; };
struct PlatDir { DIR *d; };
static int err_of(int e) {
    switch (e) { case ENOENT: return PLAT_E_NOENT; case EEXIST: return PLAT_E_EXIST; case EACCES: case EPERM: case EROFS: return PLAT_E_ACCESS;
                 case ENOTDIR: return PLAT_E_NOTDIR; case EISDIR: return PLAT_E_ISDIR; case ENOTEMPTY: return PLAT_E_NOTEMPTY; default: return PLAT_E_IO; }
}
PlatFile *plat_fs_open(const char *path, int flags, int *err) {
    int of = (flags & PLAT_READ) && (flags & PLAT_WRITE) ? O_RDWR : (flags & PLAT_WRITE) ? O_WRONLY : O_RDONLY;
    if (flags & PLAT_CREATE) of |= O_CREAT; if (flags & PLAT_TRUNCATE) of |= O_TRUNC; if (flags & PLAT_EXCLUSIVE) of |= O_EXCL; if (flags & PLAT_APPEND) of |= O_APPEND;
    int fd = open(path, of | O_CLOEXEC, 0644);
    if (fd < 0) { if (err) *err = err_of(errno); return NULL; }
    struct stat st; if (!fstat(fd, &st) && S_ISDIR(st.st_mode)) { close(fd); if (err) *err = PLAT_E_ISDIR; return NULL; }
    PlatFile *f = malloc(sizeof *f); f->fd = fd; return f;
}
void plat_fs_close(PlatFile *f) { if (f) { close(f->fd); free(f); } }
int64_t plat_fs_read(PlatFile *f, void *buf, uint64_t n) {
    uint64_t got = 0; while (got < n) { ssize_t r = read(f->fd, (char *)buf + got, n - got); if (r < 0) { if (errno == EINTR) continue; return got ? (int64_t)got : -1; } if (!r) break; got += (uint64_t)r; }
    return (int64_t)got;
}
int64_t plat_fs_write(PlatFile *f, const void *buf, uint64_t n) {
    uint64_t done = 0; while (done < n) { ssize_t r = write(f->fd, (const char *)buf + done, n - done); if (r < 0) { if (errno == EINTR) continue; return done ? (int64_t)done : -1; } done += (uint64_t)r; }
    return (int64_t)done;
}
int64_t plat_fs_seek(PlatFile *f, int64_t off, int whence) { off_t r = lseek(f->fd, (off_t)off, whence == PLAT_SEEK_SET ? SEEK_SET : whence == PLAT_SEEK_CUR ? SEEK_CUR : SEEK_END); return r < 0 ? -1 : (int64_t)r; }
int plat_fs_truncate(PlatFile *f, uint64_t size) { return ftruncate(f->fd, (off_t)size) ? PLAT_E_IO : PLAT_OK; }
int plat_fs_flush(PlatFile *f) { (void)f; return PLAT_OK; }
static void fill_stat(const struct stat *s, PlatStat *o) {
    o->size = (uint64_t)s->st_size; o->is_dir = S_ISDIR(s->st_mode); o->readonly = !(s->st_mode & S_IWUSR);
#if defined(__APPLE__)
    o->mtime_ns = (int64_t)s->st_mtimespec.tv_sec * 1000000000 + s->st_mtimespec.tv_nsec; o->atime_ns = (int64_t)s->st_atimespec.tv_sec * 1000000000 + s->st_atimespec.tv_nsec; o->ctime_ns = (int64_t)s->st_ctimespec.tv_sec * 1000000000 + s->st_ctimespec.tv_nsec;
#else
    o->mtime_ns = (int64_t)s->st_mtim.tv_sec * 1000000000 + s->st_mtim.tv_nsec; o->atime_ns = (int64_t)s->st_atim.tv_sec * 1000000000 + s->st_atim.tv_nsec; o->ctime_ns = (int64_t)s->st_ctim.tv_sec * 1000000000 + s->st_ctim.tv_nsec;
#endif
}
int plat_fs_fstat(PlatFile *f, PlatStat *st) { struct stat s; if (fstat(f->fd, &s)) return err_of(errno); fill_stat(&s, st); return PLAT_OK; }
int plat_fs_stat(const char *path, PlatStat *st) { struct stat s; if (stat(path, &s)) return err_of(errno); fill_stat(&s, st); return PLAT_OK; }
int plat_fs_mkdir(const char *path) { return mkdir(path, 0755) ? err_of(errno) : PLAT_OK; }
int plat_fs_rmdir(const char *path) { return rmdir(path) ? err_of(errno) : PLAT_OK; }
int plat_fs_remove(const char *path) { struct stat s; if (!stat(path, &s) && S_ISDIR(s.st_mode)) return PLAT_E_ISDIR; return unlink(path) ? err_of(errno) : PLAT_OK; }
int plat_fs_rename(const char *from, const char *to) { return rename(from, to) ? err_of(errno) : PLAT_OK; }
PlatDir *plat_fs_opendir(const char *path) { DIR *d = opendir(path); if (!d) return NULL; PlatDir *p = malloc(sizeof *p); p->d = d; return p; }
const char *plat_fs_readdir(PlatDir *d) { struct dirent *e; while ((e = readdir(d->d))) if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) return e->d_name; return NULL; }
void plat_fs_closedir(PlatDir *d) { if (d) { closedir(d->d); free(d); } }
const char *plat_fs_write_dir(void) { const char *w = getenv("SS2PORT_WRITE_DIR"); return w && *w ? w : NULL; }
