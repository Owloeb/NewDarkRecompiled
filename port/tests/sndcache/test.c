/* port/tests/sndcache/test.c: host test of the Vita sound cache (backends/vita/sndcache.c).   sh port/tests/sndcache/run.sh
 * A 64 MB stand-in archive; the "game" replays sounds from a working set of about 15 MB: each play reads one sound in
 * ~32 KB pieces from its start (sequential, so read-ahead kicks in), and plays overlap. Every byte returned must equal
 * the file; prints the hit rate. Built with ThreadSanitizer (the read-ahead worker runs on its own thread). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include "sndcache.h"
#define SIZE (64u << 20)
static char line[400];
static void out(const char *s) { snprintf(line, sizeof line, "%s", s); }
static uint8_t byte_at(uint64_t p) { return (uint8_t)((p * 2654435761u) >> 13 ^ p >> 7); }
int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "build/sndcache_test/SND.CRF";
    FILE *f = fopen(path, "wb"); static uint8_t blk[1 << 16];
    for (uint64_t o = 0; o < SIZE; o += sizeof blk) { for (unsigned i = 0; i < sizeof blk; i++) blk[i] = byte_at(o + i); fwrite(blk, 1, sizeof blk, f); }
    fclose(f);
    int fd = open(path, O_RDONLY), fid = sc_file_id(path); if (fd < 0 || !fid) { puts("sndcache: setup failed"); return 2; }
    uint64_t rs = 12345; int bad = 0; long reads = 0;
    typedef struct { int64_t pos, end; } Play; Play play[6] = { { 0 } };
    static uint8_t buf[40000];
    for (int step = 0; step < 60000; step++) {
        Play *p = &play[step % 6];
        if (p->pos >= p->end) {                                  /* start another sound: one of 120 sounds of 64-256 KB in the first 15 MB */
            rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; uint32_t k = (uint32_t)(rs % 120);
            p->pos = (int64_t)k * 128 * 1024; p->end = p->pos + 64 * 1024 + (int64_t)(k * 37 % 4) * 64 * 1024;
        }
        uint32_t n = 32768; if (p->pos + n > p->end) n = (uint32_t)(p->end - p->pos);
        int64_t got = sc_read(fid, path, fd, SIZE, p->pos, buf, n); reads++;
        if (got != n) bad++; else for (uint32_t i = 0; i < n; i++) if (buf[i] != byte_at((uint64_t)p->pos + i)) { bad++; break; }
        p->pos += n;
    }
    sc_log_stats(out);
    printf("sndcache: %ld reads, %d wrong; %s\n", reads, bad, line);
    return bad ? 1 : 0;
}
