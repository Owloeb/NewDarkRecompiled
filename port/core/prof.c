/* prof.c: sampling profiler for profiling builds (cmake -DPORT_PROF=ON, which compiles the game with -DRT_PROF).
 * Every recompiled function entry and loop head stores its name in rt_prof_cur (runtime/rt.h); while the game is inside a host
 * function (an import such as fread or IDirect3DDevice9::DrawPrimitiveUP) it holds that import's name instead (core/host.c).
 * A thread reads it about every millisecond and every 20 s logs the 30 names seen most often:
 *     [port] PROF  12.3% nd_0068bff0
 * A function is charged until the next function entry or loop head, so straight-line code after a call is charged to the callee.
 * tools/fnprof.py averages these tables and describes the hottest guest functions. Nothing here exists in a normal build. */
#include "plat.h"
#ifdef RT_PROF
#include <stdio.h>
#include <stdint.h>
#include <string.h>

const char *volatile rt_prof_cur;

#define PROF_SLOTS 4096
static const char *prof_key[PROF_SLOTS]; static unsigned prof_cnt[PROF_SLOTS];

static void plog(const char *fmt, const char *s, double pct, unsigned n) {
    char b[200]; if (s) snprintf(b, sizeof b, fmt, pct, s); else snprintf(b, sizeof b, fmt, n); plat_log_write(PLAT_LOG_INFO, b);
}
static void prof_thread(void *arg) {
    (void)arg; unsigned total = 0; uint64_t t0 = plat_time_ns();
    for (;;) {
        plat_sleep_ns(1000000u);
        const char *k = rt_prof_cur; if (!k) continue;
        unsigned h = (unsigned)(((uintptr_t)k >> 2) * 2654435761u) % PROF_SLOTS, n = 0;
        while (prof_key[h] && prof_key[h] != k && n++ < PROF_SLOTS) h = (h + 1) % PROF_SLOTS;
        if (n >= PROF_SLOTS) continue;
        prof_key[h] = k; prof_cnt[h]++; total++;
        if (plat_time_ns() - t0 >= 20000000000ull) {
            t0 = plat_time_ns();
            plog("[port] PROF %u samples; top guest functions:", NULL, 0, total);
            for (int r = 0; r < 30; r++) {
                int best = -1; for (int i = 0; i < PROF_SLOTS; i++) if (prof_cnt[i] && (best < 0 || prof_cnt[i] > prof_cnt[best])) best = i;
                if (best < 0) break;
                plog("[port] PROF %5.1f%% %s", prof_key[best], 100.0 * prof_cnt[best] / total, 0);
                prof_cnt[best] = 0;
            }
            memset(prof_key, 0, sizeof prof_key); memset(prof_cnt, 0, sizeof prof_cnt); total = 0;
        }
    }
}
void port_prof_start(void) { if (!plat_thread_start(prof_thread, NULL, "profiler")) plat_log_write(PLAT_LOG_INFO, "[port] sampling profiler on"); }
#else
void port_prof_start(void) {}
#endif
