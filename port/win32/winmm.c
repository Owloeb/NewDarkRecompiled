/* winmm.c: WINMM time and multimedia timers, mmio (RIFF file reading for .wav), and the CD/joystick queries.
 * Multimedia timer callbacks run on their own guest thread, like Windows' timer thread. */
#include <stdio.h>
#include <stdlib.h>
#include "core.h"

SHIM(timeGetTime) { RET(ms_ticks()); }
SHIM(timeGetDevCaps) { if (A(0) && A(1) >= 8) { WR32(A(0), 1); WR32(A(0) + 4, 1000000); } RET(0); }
SHIM(timeBeginEnd) { RET(0); }

/* ---------------------------------------------------------------- multimedia timers */
typedef struct { uint32_t id, delay, flags, proc, user; uint64_t due; int active; } MmTimer;
#define NTIMERS 32
static MmTimer timers[NTIMERS];
static PlatMutex *tm_m; static PlatCond *tm_cv; static GuestThread *tm_thread; static uint32_t tm_next_id = 1;
extern KObj *handle_get(uint32_t h, KType type);
static void timer_main(void *arg) {
    (void)arg;
    plat_mutex_lock(tm_m);
    for (;;) {
        MmTimer *due = NULL; uint64_t now = plat_time_ns();
        for (int i = 0; i < NTIMERS; i++) if (timers[i].active && (!due || timers[i].due < due->due)) due = &timers[i];
        if (!due) { plat_cond_wait(tm_cv, tm_m); continue; }
        if (due->due > now) { plat_cond_wait_ns(tm_cv, tm_m, due->due - now); continue; }
        MmTimer t = *due;
        if (t.flags & 1) { due->due += (uint64_t)t.delay * 1000000u; if (due->due < now) due->due = now + (uint64_t)t.delay * 1000000u; }   /* TIME_PERIODIC: no catch-up bursts */
        else due->active = 0;
        plat_mutex_unlock(tm_m);
        gil_acquire(tm_thread);
        if (t.flags & 0x10) { KObj *e = handle_get(t.proc, K_EVENT); if (e) kevent_set(e); }        /* TIME_CALLBACK_EVENT_SET */
        else if (t.flags & 0x20) { KObj *e = handle_get(t.proc, K_EVENT); if (e) { kevent_set(e); kevent_reset(e); } }   /* TIME_CALLBACK_EVENT_PULSE */
        else g_call(cur_cpu(), t.proc, 5, t.id, 0u, t.user, 0u, 0u);
        gil_release();
        plat_mutex_lock(tm_m);
    }
}
SHIM(timeSetEvent) {
    if (!tm_m) { tm_m = plat_mutex_new(); tm_cv = plat_cond_new(); }
    if (!tm_thread) { tm_thread = thread_new_host_side("winmm timer"); if (!tm_thread || plat_thread_start(timer_main, NULL, "winmm timer")) { RET(0); return; } }
    plat_mutex_lock(tm_m);
    MmTimer *t = NULL; for (int i = 0; i < NTIMERS; i++) if (!timers[i].active) { t = &timers[i]; break; }
    if (!t) { plat_mutex_unlock(tm_m); RET(0); return; }
    uint32_t delay = A(0) ? A(0) : 1;
    *t = (MmTimer){ tm_next_id++, delay, A(4), A(2), A(3), plat_time_ns() + (uint64_t)delay * 1000000u, 1 };
    plat_cond_signal(tm_cv); plat_mutex_unlock(tm_m);
    RET(t->id);
}
SHIM(timeKillEvent) {
    if (!tm_m) { RET(97); return; }
    plat_mutex_lock(tm_m); int found = 0; for (int i = 0; i < NTIMERS; i++) if (timers[i].active && timers[i].id == A(0)) { timers[i].active = 0; found = 1; }
    plat_cond_signal(tm_cv); plat_mutex_unlock(tm_m); RET(found ? 0 : 97);          /* TIMERR_NOCANDO */
}

/* ---------------------------------------------------------------- mmio: RIFF chunk reading */
/* A handle reads a file, or a block of guest memory: mmioOpen(NULL, &info, ...) with info.fccIOProc == FOURCC_MEM and
   info.pchBuffer / info.cchBuffer. The engine loads every sound into memory and parses it that way. */
#define MAXM 256
#define FOURCC_MEM 0x204D454Du
typedef struct { PlatFile *f; uint32_t mem, size, pos; int used; } Mmio;
static Mmio mt[MAXM];
static Mmio *H(uint32_t h) { return h && h < MAXM && mt[h].used ? &mt[h] : NULL; }
static int64_t m_tell(Mmio *m) { return m->f ? plat_fs_seek(m->f, 0, PLAT_SEEK_CUR) : (int64_t)m->pos; }
static int64_t m_seek(Mmio *m, int64_t off, int whence) {
    if (m->f) return plat_fs_seek(m->f, off, whence);
    int64_t base = whence == PLAT_SEEK_CUR ? (int64_t)m->pos : whence == PLAT_SEEK_END ? (int64_t)m->size : 0, n = base + off;
    if (n < 0) return -1;
    m->pos = (uint32_t)n; return n;                     /* past the end is allowed, like a file; reads there return 0 */
}
static int64_t m_read(Mmio *m, void *dst, uint64_t n) {
    if (m->f) return plat_fs_read(m->f, dst, n);
    uint64_t left = m->pos < m->size ? m->size - m->pos : 0; if (n > left) n = left;
    memcpy(dst, GP(m->mem + m->pos), (size_t)n); m->pos += (uint32_t)n; return (int64_t)n;
}
SHIM(mmioOpenA) {
    uint32_t fl = A(2), info = A(1);
    int slot = 0; for (int i = 1; i < MAXM; i++) if (!mt[i].used) { slot = i; break; }
    if (!slot) { if (info) WR32(info + 12, 258); RET(0); return; }                 /* MMIOERR_OUTOFMEMORY */
    Mmio *m = &mt[slot]; memset(m, 0, sizeof *m);
    if (info && (RD32(info + 4) == FOURCC_MEM || (!A(0) && RD32(info + 24)))) {      /* also: no name, a buffer, no I/O procedure */
        if (!RD32(info + 24) || !RD32(info + 20)) { port_warn("mmioOpen: memory file without a buffer"); WR32(info + 12, 258); RET(0); return; }
        m->mem = RD32(info + 24); m->size = RD32(info + 20); m->used = 1;
    } else {
        int flags = (fl & 3) == 0 ? PLAT_READ : PLAT_READ | PLAT_WRITE; if (fl & 0x1000) flags = PLAT_WRITE | PLAT_CREATE | PLAT_TRUNCATE;   /* MMIO_CREATE */
        int err; PlatFile *f = A(0) ? vfs_open(gs(A(0)), flags, &err, NULL, 0) : NULL;
        if (!f) { if (A(0)) port_miss("mmioOpen", gs(A(0))); else port_warn("mmioOpen: no file name and no memory buffer"); if (info) WR32(info + 12, 257); RET(0); return; }
        m->f = f; m->used = 1;
    }
    if (info) WR32(info + 12, 0);
    RET((uint32_t)slot);
}
SHIM(mmioClose) { Mmio *m = H(A(0)); if (m) { if (m->f) plat_fs_close(m->f); memset(m, 0, sizeof *m); } RET(0); }
SHIM(mmioRead) { Mmio *m = H(A(0)); int64_t r = m ? m_read(m, GP(A(1)), A(2)) : -1; RET(r < 0 ? 0xFFFFFFFFu : (uint32_t)r); }
SHIM(mmioWrite) {
    Mmio *m = H(A(0)); int64_t r = -1;
    if (m && m->f) r = plat_fs_write(m->f, GP(A(1)), A(2));
    else if (m) { uint32_t n = A(2), left = m->pos < m->size ? m->size - m->pos : 0; if (n > left) n = left; memcpy(GP(m->mem + m->pos), GP(A(1)), n); m->pos += n; r = n; }
    RET(r < 0 ? 0xFFFFFFFFu : (uint32_t)r);
}
SHIM(mmioSeek) { Mmio *m = H(A(0)); int64_t r = m ? m_seek(m, (int32_t)A(1), (int)A(2)) : -1; RET(r < 0 ? 0xFFFFFFFFu : (uint32_t)r); }
SHIM(mmioGetInfo) {
    Mmio *m = H(A(0)); uint32_t p = A(1); memset(GP(p), 0, 72); if (!m) { RET(257); return; }
    if (!m->f) { WR32(p + 4, FOURCC_MEM); WR32(p + 20, m->size); WR32(p + 24, m->mem); WR32(p + 28, m->mem + m->pos); WR32(p + 32, m->mem + m->size); WR32(p + 36, m->mem + m->size); }
    WR32(p + 44, (uint32_t)m_tell(m)); WR32(p + 68, A(0)); RET(0);
}
SHIM(mmioDescend) {
    Mmio *m = H(A(0)); uint32_t ck = A(1), par = A(2), fl = A(3); if (!m) { RET(257); return; }
    int64_t limit = par ? (int64_t)RD32(par + 12) + RD32(par + 4) : INT64_MAX;
    for (;;) {
        int64_t pos = m_tell(m); uint32_t h[2];
        if (pos + 8 > limit || m_read(m, h, 8) != 8) { RET(265); return; }      /* MMIOERR_CHUNKNOTFOUND */
        int list = h[0] == 0x46464952u || h[0] == 0x5453494Cu; uint32_t form = 0;
        if (list && m_read(m, &form, 4) != 4) { RET(265); return; }
        int want = (int)(fl & 0x70), ok = 1;
        if (want == 0x10) ok = h[0] == RD32(ck);
        else if (want == 0x20) ok = h[0] == 0x46464952u && form == RD32(ck + 8);
        else if (want == 0x40) ok = h[0] == 0x5453494Cu && form == RD32(ck + 8);
        if (ok) { WR32(ck, h[0]); WR32(ck + 4, h[1]); WR32(ck + 8, list ? form : 0); WR32(ck + 12, (uint32_t)pos + 8); WR32(ck + 16, 0); RET(0); return; }
        m_seek(m, pos + 8 + (int64_t)h[1] + (h[1] & 1), PLAT_SEEK_SET);
    }
}
SHIM(mmioAscend) { Mmio *m = H(A(0)); uint32_t ck = A(1); if (!m) { RET(257); return; } int64_t end = (int64_t)RD32(ck + 12) + RD32(ck + 4); m_seek(m, end + (end & 1), PLAT_SEEK_SET); RET(0); }

/* ---------------------------------------------------------------- CD audio and joysticks: none */
SHIM(mciSendCommandA) { RET(0x113); }        /* MCIERR_DEVICE_NOT_INSTALLED-ish: the engine falls back to no CD music */
SHIM(mciSendStringA) { RET(0x113); }
SHIM(Zero) { RET(0); }
SHIM(JoyErr) { RET(167); }                   /* JOYERR_UNPLUGGED */

const ShimDef winmm_shims[] = {
    S(timeGetTime, 0), S(timeGetDevCaps, STD(2)), SA("timeBeginPeriod", timeBeginEnd, STD(1)), SA("timeEndPeriod", timeBeginEnd, STD(1)), S(timeSetEvent, STD(5)), S(timeKillEvent, STD(1)),
    S(mmioOpenA, STD(3)), S(mmioClose, STD(2)), S(mmioRead, STD(3)), S(mmioWrite, STD(3)), S(mmioSeek, STD(3)), S(mmioGetInfo, STD(3)), S(mmioDescend, STD(4)), S(mmioAscend, STD(3)),
    S(mciSendCommandA, STD(4)), S(mciSendStringA, STD(4)), SA("joyGetNumDevs", Zero, 0), SA("joyGetPosEx", JoyErr, STD(2)), SA("joyGetDevCapsA", JoyErr, STD(3)), SA("joyGetPos", JoyErr, STD(2)),
    { 0, 0, 0 }
};
