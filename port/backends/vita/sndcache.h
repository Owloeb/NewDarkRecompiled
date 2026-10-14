/* sndcache.h: read cache for the game's sound archives (SND.CRF, SND2.CRF) on the Vita. See sndcache.c. */
#ifndef SNDCACHE_H
#define SNDCACHE_H
#include <stdint.h>
int sc_eligible(const char *path);                                  /* is this a sound archive? */
int sc_file_id(const char *path);                                   /* small id (> 0) for a path, 0 if there is no room */
/* read n bytes at pos from the (open, read-only) file fd of size `size`: returns the bytes read, 0 at the end, -1 on error */
int64_t sc_read(int fid, const char *path, int fd, int64_t size, int64_t pos, void *buf, uint32_t n);
void sc_log_stats(void (*out)(const char *line));                   /* one line since the last call (nothing if idle) */
#endif
