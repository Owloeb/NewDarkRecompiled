/* sndcache.c: a read cache with per-stream read-ahead for SND.CRF / SND2.CRF (PS Vita backend).
 *
 * While you play, the game streams sound from these archives in pieces of about 30 KB. Several sounds play at once, so the reads of
 * the different sounds interleave and the file looks random to the 64 KB window in plat_vita_sys.c (0% sequential in the log). Each
 * read then costs the game thread a full memory-card access (about 5 ms). This cache does two things:
 *  - it remembers recently read pieces (up to 24 MB, less if the host heap runs short), so a sound that is read again costs nothing
 *    (combat and ambient loops replay the same sounds; at 4 MB half the reads still went to the card), and
 *  - it follows each sound separately: when a read starts exactly where an earlier read ended (the next piece of the same sound),
 *    a worker thread reads the following two pieces in the background.
 * The data returned is always exactly what the file holds. A read the cache does not hold is read from the file as before. */
#include "sndcache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#ifdef __vita__
#include <malloc.h>
#include <psp2/kernel/threadmgr.h>
extern unsigned int _newlib_heap_size_user;
#define SC_SLEEP_US(us) sceKernelDelayThread(us)
#else
#define SC_SLEEP_US(us) usleep(us)
#endif
#ifndef VITA_SOUND_CACHE_MB
#define VITA_SOUND_CACHE_MB 24
#endif
#ifndef VITA_SOUND_CACHE_RESERVE_MB   /* the cache never grows while the host heap has less than this free */
#define VITA_SOUND_CACHE_RESERVE_MB 8
#endif
#define SC_MAX_BYTES ((uint32_t)VITA_SOUND_CACHE_MB << 20)
#ifdef __vita__
#define VITA_HEAP_MB_FOR_LOG ((int)(_newlib_heap_size_user >> 20))
#else
#define VITA_HEAP_MB_FOR_LOG 0
#endif
#define SC_ENTRIES 1024
#define SC_FILES 4
#define SC_STREAMS 12
#define SC_AHEAD 2
#define SC_PENDING_MAX 4

enum { E_FREE = 0, E_QUEUED = 1, E_READY = 2, E_READING = 3 };
typedef struct { int fid, state, pf; int64_t off; uint32_t len, cap; uint8_t *data; uint64_t used; } Ent;
typedef struct { int fid; int64_t next; uint32_t len; uint64_t used; } Stream;
static Ent ent[SC_ENTRIES]; static Stream str[SC_STREAMS]; static uint32_t bytes_held; static uint64_t tick;
static char names[SC_FILES + 1][96]; static int wfd[SC_FILES + 1];
static uint64_t st_hit, st_hit_pf, st_waited, st_miss, st_issued, st_miss_bytes;
static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t cv = PTHREAD_COND_INITIALIZER; static int started;

int sc_eligible(const char *path) {
    const char *b = strrchr(path, '/'); const char *c = strrchr(path, ':'); const char *d = strrchr(path, '\\');
    if (c && (!b || c > b)) b = c; if (d && (!b || d > b)) b = d; b = b ? b + 1 : path;
    size_t n = strlen(b); return n >= 7 && !strncasecmp(b, "SND", 3) && !strcasecmp(b + n - 4, ".CRF");
}
int sc_file_id(const char *path) {
    pthread_mutex_lock(&mx); int r = 0;
    for (int i = 1; i <= SC_FILES && !r; i++) if (names[i][0] && !strcmp(names[i], path)) r = i;
    for (int i = 1; i <= SC_FILES && !r; i++) if (!names[i][0]) { snprintf(names[i], sizeof names[i], "%s", path); wfd[i] = -1; r = i; }
    pthread_mutex_unlock(&mx); return r;
}
static Ent *find(int fid, int64_t pos, uint32_t n) {           /* an entry (queued, reading or ready) that covers [pos, pos+n) */
    for (int i = 0; i < SC_ENTRIES; i++) { Ent *e = &ent[i]; if (e->state && e->fid == fid && pos >= e->off && pos + n <= e->off + e->len) return e; }
    return NULL;
}
static void drop(Ent *e) { if (e->state == E_FREE) return; free(e->data); bytes_held -= e->cap; memset(e, 0, sizeof *e); }
/* free bytes in the host heap (newlib's: _newlib_heap_size_user in plat_vita.c), checked before the cache grows past 4 MB */
#ifdef __vita__
static int heap_short(uint32_t len) {
    if (bytes_held + len <= (4u << 20)) return 0;
    struct mallinfo mi = mallinfo(); uint64_t used = (uint64_t)mi.uordblks;
    return used + len + ((uint64_t)VITA_SOUND_CACHE_RESERVE_MB << 20) > (uint64_t)_newlib_heap_size_user;
}
#else
static int heap_short(uint32_t len) { (void)len; return 0; }
#endif
static Ent *slot_for(uint32_t len) {                           /* a free entry with `len` bytes allowed, evicting the least recently used ready ones */
    int short_ = heap_short(len);                              /* heap tight: only reuse space, never grow */
    for (;;) {
        Ent *fr = NULL, *lru = NULL;
        for (int i = 0; i < SC_ENTRIES; i++) { Ent *e = &ent[i]; if (!e->state) fr = fr ? fr : e; else if (e->state == E_READY && (!lru || e->used < lru->used)) lru = e; }
        if (short_) { if (!fr || !lru) return NULL; drop(lru); return fr; }   /* heap tight: one out for one in (pieces are all ~32 KB) */
        if (fr && bytes_held + len <= SC_MAX_BYTES) return fr;
        if (!lru) return NULL;
        drop(lru);
    }
}
static void *worker(void *arg) {
    (void)arg; pthread_mutex_lock(&mx);
    for (;;) {
        Ent *e = NULL; for (int i = 0; i < SC_ENTRIES; i++) if (ent[i].state == E_QUEUED && (!e || ent[i].used < e->used)) e = &ent[i];
        if (!e) { pthread_cond_wait(&cv, &mx); continue; }
        e->state = E_READING; int fid = e->fid; int64_t off = e->off; uint32_t want = e->len; uint8_t *dst = e->data;
        if (wfd[fid] < 0) wfd[fid] = open(names[fid], O_RDONLY);
        int fd = wfd[fid]; pthread_mutex_unlock(&mx);
        int64_t got = -1; if (fd >= 0 && lseek(fd, (off_t)off, SEEK_SET) >= 0) { got = 0; while ((uint64_t)got < want) { ssize_t r = read(fd, dst + got, want - (uint32_t)got); if (r <= 0) break; got += r; } }
        pthread_mutex_lock(&mx);
        if (got > 0) { e->len = (uint32_t)got; e->state = E_READY; } else drop(e);
    }
    return NULL;
}
static void post(int fid, int64_t off, uint32_t len, int64_t size) {          /* queue a background read (mx held) */
    if (off >= size || len == 0) return; if (off + len > (uint64_t)size) len = (uint32_t)(size - off);
    if (find(fid, off, len)) return;
    int pending = 0; for (int i = 0; i < SC_ENTRIES; i++) if (ent[i].state == E_QUEUED || ent[i].state == E_READING) pending++;
    if (pending >= SC_PENDING_MAX) return;
    if (!started) { pthread_t t; pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 64u << 10); if (!pthread_create(&t, &a, worker, NULL)) started = 1; pthread_attr_destroy(&a); if (!started) return; }
    Ent *e = slot_for(len); if (!e) return;
    uint8_t *d = malloc(len); if (!d) return;
    e->fid = fid; e->off = off; e->len = e->cap = len; e->data = d; e->state = E_QUEUED; e->pf = 1; e->used = ++tick; bytes_held += len; st_issued++;
    pthread_cond_signal(&cv);
}
int64_t sc_read(int fid, const char *path, int fd, int64_t size, int64_t pos, void *buf, uint32_t n) {
    (void)path; if (pos >= size) return 0; if ((int64_t)n > size - pos) n = (uint32_t)(size - pos); if (!n) return 0;
    int waited = 0, hit = 0;
    pthread_mutex_lock(&mx);
    for (;;) {
        Ent *e = find(fid, pos, n); if (!e) break;
        if (e->state == E_READY) { memcpy(buf, e->data + (pos - e->off), n); e->used = ++tick; hit = 1; st_hit++; if (e->pf) { e->pf = 0; st_hit_pf++; } if (waited) st_waited++; break; }
        waited = 1; pthread_mutex_unlock(&mx); SC_SLEEP_US(300); pthread_mutex_lock(&mx);
    }
    if (!hit) {
        pthread_mutex_unlock(&mx);
        int64_t got = -1; if (lseek(fd, (off_t)pos, SEEK_SET) >= 0) { got = 0; while ((uint64_t)got < n) { ssize_t r = read(fd, (char *)buf + got, n - (uint32_t)got); if (r < 0) { if (got == 0) { got = -1; } break; } if (r == 0) break; got += r; } }
        if (got <= 0) return got < 0 ? -1 : 0;
        n = (uint32_t)got;
        pthread_mutex_lock(&mx); st_miss++; st_miss_bytes += n;
        if (!find(fid, pos, n)) { Ent *e = slot_for(n); uint8_t *d = e ? malloc(n) : NULL;
            if (e && d) { memcpy(d, buf, n); e->fid = fid; e->off = pos; e->len = e->cap = n; e->data = d; e->state = E_READY; e->pf = 0; e->used = ++tick; bytes_held += n; } }
    }
    /* follow this sound: a read that starts where an earlier one ended is the next piece of the same sound */
    Stream *s = NULL, *old = &str[0]; for (int i = 0; i < SC_STREAMS; i++) { if (str[i].len && str[i].fid == fid && str[i].next == pos) s = &str[i]; if (str[i].used < old->used) old = &str[i]; }
    if (s) { for (int k = 1; k <= SC_AHEAD; k++) post(fid, pos + (int64_t)k * n, n, size); } else s = old;
    s->fid = fid; s->next = pos + n; s->len = n; s->used = ++tick;
    pthread_mutex_unlock(&mx);
    return n;
}
void sc_log_stats(void (*out)(const char *)) {
    pthread_mutex_lock(&mx); uint64_t h = st_hit, hp = st_hit_pf, w = st_waited, m = st_miss, is = st_issued, mb = st_miss_bytes; uint32_t held = bytes_held;
    st_hit = st_hit_pf = st_waited = st_miss = st_issued = st_miss_bytes = 0; pthread_mutex_unlock(&mx);
    if (!h && !m) return;
    int heap_mb = -1;
#ifdef __vita__
    { struct mallinfo mi = mallinfo(); heap_mb = (int)(mi.uordblks >> 20); }
#endif
    char b[300]; snprintf(b, sizeof b, "[vita] sound cache: %llu reads from memory (%llu of them read ahead, %llu had to wait), %llu from the card (%.1f MB), %llu read-aheads started, %.1f MB held (host heap %d of %d MB in use)",
                          (unsigned long long)h, (unsigned long long)hp, (unsigned long long)w, (unsigned long long)m, (double)mb / 1048576.0, (unsigned long long)is, (double)held / 1048576.0,
                          heap_mb, VITA_HEAP_MB_FOR_LOG);
    out(b);
}
