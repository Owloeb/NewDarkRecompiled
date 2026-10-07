/* video.c: the host side of the built-in cutscene decoder (video/lavshim.c), which stands in for the 2011 ffmpeg.dll
 * that the recompiled lgvid.dll loads. LoadLibrary("ffmpeg.dll") returns a pseudo module (kernel32.c) and
 * GetProcAddress on it returns thunks to lav_exports; the functions below are the services lavshim.h asks a host for. */
#include <stdio.h>
#include <stdlib.h>
#include "core.h"
#include "lavshim.h"

static PlatMutex *lav_m;
uint32_t lavh_alloc(uint32_t size) { return g_alloc(size ? size : 1); }
void lavh_free(uint32_t p) { if (p) g_free(p); }
void lavh_call(CPU *c, uint32_t fn, int n, const uint32_t *a) { g_callv(c, fn, n, a); }
typedef struct { lav_fn fn; uint32_t addr; } LavThunk;
static LavThunk thunks[64]; static int nthunks;
static void lav_dispatch(CPU *c) { int i = (int)g_targ; if (i >= 0 && i < nthunks) thunks[i].fn(c); }
uint32_t lavh_fnaddr(lav_fn fn) {
    if (!fn) return 0;
    for (int i = 0; i < nthunks; i++) if (thunks[i].fn == fn) return thunks[i].addr;
    if (nthunks == 64) return 0;
    thunks[nthunks].fn = fn; thunks[nthunks].addr = g_thunk_arg(lav_dispatch, 0, "ffmpeg.dll (built-in)", (uint32_t)nthunks);   /* cdecl: the caller pops the arguments */
    return thunks[nthunks++].addr;
}
int64_t lavh_time_us(void) { return (int64_t)(plat_time_ns() / 1000u); }
void lavh_lock(void) { if (!lav_m) lav_m = plat_mutex_new(); plat_mutex_lock(lav_m); }
void lavh_unlock(void) { plat_mutex_unlock(lav_m); }
void lavh_log(const char *fmt, ...) { char b[512]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap); port_debug("cutscenes: %s", strncmp(b, "LAVSHIM ", 8) ? b : b + 8); }
/* GetProcAddress on the ffmpeg.dll pseudo module */
uint32_t video_export(const char *name) {
    for (const LavExport *e = lav_exports; e->name; e++) if (!strcmp(e->name, name)) return lavh_fnaddr(e->fn);
    return 0;
}
