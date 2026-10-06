/* lavshim: a portable stand-in for the 2011 ffmpeg.dll (Lavc 52.114 / Lavf 52.103) that lgvid.dll, the game's
 * cutscene player, loads with LoadLibrary/GetProcAddress.
 *
 * The recompiled lgvid.dll keeps running unchanged; when it asks for "ffmpeg.dll" the host hands it these functions
 * instead. Each one is called the way the original DLL's export was (cdecl, arguments on the guest stack, structs in
 * guest memory laid out exactly like FFmpeg 0.7's 32-bit ABI). Behind them is plain C: an AVI demuxer, FFmpeg's
 * Indeo 5 decoder (video/ffmpeg, LGPL), PCM audio and a YUV->RGB scaler. Nothing here is x86- or Windows-specific;
 * a port only has to provide the few host functions declared below.
 *
 * MIT licensed (see LICENSE); the Indeo decoder it links against is LGPL 2.1 (see video/ffmpeg). */
#ifndef LAVSHIM_H
#define LAVSHIM_H
#include "rt.h"

typedef void (*lav_fn)(CPU *c);                 /* a host function called like a guest cdecl function; the caller pops the return address */
typedef struct { const char *name; lav_fn fn; } LavExport;
extern const LavExport lav_exports[];           /* the ffmpeg.dll exports lgvid.dll looks up; terminated by {0, 0} */

/* provided by the host */
uint32_t lavh_alloc(uint32_t size);             /* zeroed, 16-byte aligned guest memory; 0 when out of memory */
void     lavh_free(uint32_t p);
void     lavh_call(CPU *c, uint32_t fn, int nargs, const uint32_t *args);   /* call a guest cdecl function; result in c->eax (and c->edx) */
uint32_t lavh_fnaddr(lav_fn fn);                /* a guest address that calls fn (for function pointers stored in guest structs) */
int64_t  lavh_time_us(void);                    /* monotonic microseconds */
void     lavh_lock(void);
void     lavh_unlock(void);
void     lavh_log(const char *fmt, ...);
#endif
