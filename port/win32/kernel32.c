/* kernel32.c: KERNEL32 process, memory, synchronisation, threads, time, locale and module functions. Files are in
 * kernel32_file.c. Everything is built on the core (heap, handles, threads) and plat.h; nothing here is OS-specific. */
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include "core.h"

extern const uint32_t hd_base;
uint32_t thread_create(uint32_t fn, uint32_t arg, uint32_t stack, int suspended, uint32_t *tid, const char *name);
int thread_exit_code(uint32_t h, uint32_t *code);
void thread_exit_current(uint32_t code) __attribute__((noreturn));
KObj *kmutex_new(int owned); int kmutex_release(KObj *o);
KObj *ksem_new(int32_t initial, int32_t max); int ksem_release(KObj *o, int32_t n, int32_t *prev);
int kevent_pulse(KObj *o);

/* ---------------------------------------------------------------- process */
SHIM(ExitProcess) { port_log("ExitProcess(%u)", A(0)); port_exit((int)A(0)); }
SHIM(TerminateProcess) { port_log("TerminateProcess(%u)", A(1)); port_exit((int)A(1)); }
SHIM(FatalAppExitA) { port_die("FatalAppExit: %s", gs(A(1))); }
SHIM(DebugBreak) { port_die("DebugBreak"); }
SHIM(IsDebuggerPresent) { RET(0); }
SHIM(SetUnhandledExceptionFilter) { RET(0); }
SHIM(UnhandledExceptionFilter) { port_die("an unhandled exception reached UnhandledExceptionFilter"); }
SHIM(RaiseException) { port_die("RaiseException(%08x): structured exceptions are not supported by the portable host", A(0)); }
SHIM(RtlUnwind) { port_die("RtlUnwind: structured exception unwinding is not supported by the portable host"); }
SHIM(SetErrorMode) { static uint32_t m; uint32_t o = m; m = A(0); RET(o); }
SHIM(IsProcessorFeaturePresent) { RET(0); }                         /* no SSE/SSE2: the engine takes its x87 paths */
SHIM(GetVersion) { RET(0x0A280105u); }                             /* Windows XP SP2, build 2600 */
static void version_info(uint32_t p, int wide) {
    WR32(p + 4, 5); WR32(p + 8, 1); WR32(p + 12, 2600); WR32(p + 16, 2);
    if (wide) WR16(p + 20, 0); else WR8(p + 20, 0);
    uint32_t sz = RD32(p), ex = wide ? 276 : 148;
    if (sz >= ex + 8) { WR16(p + ex, 2); WR16(p + ex + 2, 0); WR16(p + ex + 4, 0x100); WR8(p + ex + 6, 1); }   /* SP 2, workstation */
}
SHIM(GetVersionExA) { version_info(A(0), 0); RET(1); }
SHIM(GetVersionExW) { version_info(A(0), 1); RET(1); }
SHIM(GetCurrentProcess) { RET(0xFFFFFFFFu); }
SHIM(GetCurrentProcessId) { RET(0x100); }
SHIM(GetCommandLineA) { static uint32_t s; if (!s) s = g_str(g_cfg.cmdline); RET(s); }
SHIM(GetCommandLineW) { static uint32_t s; if (!s) { size_t n = strlen(g_cfg.cmdline); s = g_alloc((uint32_t)(2 * n + 2)); for (size_t i = 0; i < n; i++) WR16(s + 2 * (uint32_t)i, (uint8_t)g_cfg.cmdline[i]); } RET(s); }
SHIM(GetStartupInfoA) { memset(GP(A(0)), 0, 68); WR32(A(0), 68); }
SHIM(GetEnvironmentStrings) { static uint32_t e; if (!e) e = g_alloc(16); RET(e); }
SHIM(FreeEnvironmentStrings) { RET(1); }
SHIM(GetEnvironmentVariableA) { set_last_error(203); RET(0); }     /* ERROR_ENVVAR_NOT_FOUND */
SHIM(SetEnvironmentVariableA) { RET(1); }
SHIM(OutputDebugStringA) { port_debug("OutputDebugString: %s", gs(A(0))); }
SHIM(GetComputerNameA) { const char *n = "PORTABLE"; if (A(1) && RD32(A(1)) > strlen(n)) { memcpy(GP(A(0)), n, strlen(n) + 1); WR32(A(1), (uint32_t)strlen(n)); RET(1); return; } RET(0); }
SHIM(GetLastError) { RET(get_last_error()); }
SHIM(SetLastError) { set_last_error(A(0)); }
SHIM(FormatMessageA) {
    char b[64]; snprintf(b, sizeof b, "Error %u.\r\n", A(2)); uint32_t l = (uint32_t)strlen(b);
    if (A(0) & 0x100) { uint32_t p = g_str(b); WR32(A(4), p); RET(l); return; }                  /* FORMAT_MESSAGE_ALLOCATE_BUFFER */
    if (A(5) <= l) { RET(0); return; } memcpy(GP(A(4)), b, l + 1); RET(l);
}
SHIM(Beep) { RET(1); }

/* ---------------------------------------------------------------- memory */
SHIM(GetProcessHeap) { RET(0x10000u); }
SHIM(HeapCreate) { RET(0x10000u); }                             /* every heap is the one guest heap */
SHIM(HeapDestroy) { RET(1); }
SHIM(HeapAlloc) { uint32_t p = g_alloc(A(2)); if (!p) { set_last_error(8); if (A(1) & 4) port_die("HeapAlloc(%u): out of guest memory", A(2)); } RET(p); }   /* HEAP_GENERATE_EXCEPTIONS */
SHIM(HeapFree) { if (A(2) && !g_owns(A(2))) { set_last_error(87); RET(0); return; } g_free(A(2)); RET(1); }
SHIM(HeapReAlloc) {
    uint32_t p = A(2), n = A(3);
    if (A(1) & 0x10) { if (n > g_size(p)) { RET(0); return; } }    /* HEAP_REALLOC_IN_PLACE_ONLY: only shrinking is safe to promise */
    RET(g_realloc(p, n));
}
SHIM(HeapSize) { RET(A(2) && g_owns(A(2)) ? g_size(A(2)) : 0xFFFFFFFFu); }
SHIM(HeapValidate) { if (!A(2)) heap_check(); RET(!A(2) || g_owns(A(2))); }
SHIM(HeapCompact) { RET(0x100000); }
SHIM(HeapLockUnlock) { RET(1); }
SHIM(GlobalAlloc) { RET(g_alloc(A(1) ? A(1) : 1)); }        /* GMEM_MOVEABLE handles are the memory itself */
SHIM(GlobalFree) { if (A(0)) g_free(A(0)); RET(0); }
SHIM(GlobalReAlloc) { RET(g_realloc(A(0), A(1))); }
SHIM(GlobalSize) { RET(g_size(A(0))); }
SHIM(GlobalLock) { RET(A(0)); }
SHIM(GlobalUnlock) { RET(1); }
SHIM(GlobalHandle) { RET(A(0)); }
SHIM(LocalAlloc) { RET(g_alloc(A(1) ? A(1) : 1)); }
SHIM(LocalFree) { if (A(0)) g_free(A(0)); RET(0); }
SHIM(LocalReAlloc) { RET(g_realloc(A(0), A(1))); }
SHIM(LocalSize) { RET(g_size(A(0))); }
SHIM(VirtualAlloc) {
    uint32_t want = A(0), size = A(1), type = A(2);
    if (want && (type & 0x1000) && !(type & 0x2000)) {           /* MEM_COMMIT inside something already reserved: it is all backed */
        uint32_t base = want & ~0xFFFFu; for (uint32_t a = base; a >= 0x10000u; a -= 0x10000u) { uint32_t s = vm_region_size(a); if (s) { if (want + size <= a + s) { RET(want); return; } break; } if (base - a > (256u << 20)) break; }
    }
    uint32_t p = vm_alloc(size ? size : 1, want & ~0xFFFFu, "VirtualAlloc");
    if (!p && want) { set_last_error(487); RET(0); return; }     /* ERROR_INVALID_ADDRESS */
    if (!p) { set_last_error(8); RET(0); return; }
    RET(want ? want : p);
}
SHIM(VirtualFree) { if ((A(2) & 0x8000) && vm_region_size(A(0))) vm_free(A(0)); RET(1); }       /* MEM_RELEASE; MEM_DECOMMIT keeps it */
SHIM(VirtualProtect) { if (A(3)) WR32(A(3), 4); RET(1); }
SHIM(VirtualQuery) {
    uint32_t a = A(0), p = A(1); memset(GP(p), 0, 28); uint32_t base = a & ~0xFFFu;
    WR32(p, base); WR32(p + 4, base); WR32(p + 8, 4); WR32(p + 12, 0x1000); WR32(p + 16, g_valid(a, 1) ? 0x1000 : 0x10000); WR32(p + 20, 4); WR32(p + 24, 0x20000);
    RET(28);
}
SHIM(IsBadPtr) { RET(!g_valid(A(0), A(1) ? A(1) : 1)); }
SHIM(IsBadCodePtr) { RET(0); }
SHIM(IsBadStringPtrA) { RET(!g_valid(A(0), 1)); }

/* ---------------------------------------------------------------- synchronisation */
/* CRITICAL_SECTION in guest memory: +4 LockCount, +8 RecursionCount, +12 OwningThread. Its state only changes while the
 * guest lock is held, so a waiter just re-checks after every kernel-object change. */
typedef struct { KObj k; uint32_t cs; } CsWait;
static int cs_sig(KObj *o, GuestThread *t) { (void)t; uint32_t own = RD32(((CsWait *)o)->cs + 12); return own == 0 || own == cur_thread_id(); }
static void cs_take(uint32_t cs) {
    uint32_t me = cur_thread_id();
    if (RD32(cs + 12) == me) { WR32(cs + 8, RD32(cs + 8) + 1); WR32(cs + 4, RD32(cs + 4) + 1); return; }
    if (RD32(cs + 12)) { CsWait w = { { K_OTHER, 1, NULL, cs_sig, NULL }, cs }; KObj *o = &w.k; kwait(&o, 1, 0, 0xFFFFFFFFu, 0); }
    WR32(cs + 12, me); WR32(cs + 8, 1); WR32(cs + 4, 0);
}
SHIM(InitializeCriticalSection) { memset(GP(A(0)), 0, 24); WR32(A(0) + 4, 0xFFFFFFFFu); }
SHIM(InitializeCriticalSectionAndSpinCount) { memset(GP(A(0)), 0, 24); WR32(A(0) + 4, 0xFFFFFFFFu); WR32(A(0) + 20, A(1)); RET(1); }
SHIM(DeleteCriticalSection) { }
SHIM(EnterCriticalSection) { cs_take(A(0)); }
SHIM(TryEnterCriticalSection) { uint32_t cs = A(0), own = RD32(cs + 12); if (own && own != cur_thread_id()) { RET(0); return; } cs_take(cs); RET(1); }
SHIM(LeaveCriticalSection) {
    uint32_t cs = A(0); if (RD32(cs + 12) != cur_thread_id()) return;
    uint32_t r = RD32(cs + 8) - 1; WR32(cs + 8, r);
    if (!r) { WR32(cs + 12, 0); WR32(cs + 4, 0xFFFFFFFFu); kobj_changed(); } else WR32(cs + 4, RD32(cs + 4) - 1);
}
SHIM(CreateEventA) { RET(handle_new(kevent_new(A(1) != 0, A(2) != 0))); }
SHIM(SetEvent) { KObj *e = handle_get(A(0), K_EVENT); if (!e) { set_last_error(6); RET(0); return; } kevent_set(e); RET(1); }
SHIM(ResetEvent) { KObj *e = handle_get(A(0), K_EVENT); if (!e) { set_last_error(6); RET(0); return; } kevent_reset(e); RET(1); }
SHIM(PulseEvent) { KObj *e = handle_get(A(0), K_EVENT); if (!e) { RET(0); return; } RET(kevent_pulse(e)); }
SHIM(CreateMutexA) { RET(handle_new(kmutex_new(A(1) != 0))); }
SHIM(ReleaseMutex) { KObj *m = handle_get(A(0), K_MUTEX); if (!m || !kmutex_release(m)) { set_last_error(288); RET(0); return; } RET(1); }
SHIM(CreateSemaphoreA) { RET(handle_new(ksem_new((int32_t)A(1), (int32_t)A(2)))); }
SHIM(ReleaseSemaphore) { KObj *s = handle_get(A(0), K_SEMAPHORE); int32_t prev = 0; if (!s || !ksem_release(s, (int32_t)A(1), &prev)) { set_last_error(298); RET(0); return; } if (A(2)) WR32(A(2), (uint32_t)prev); RET(1); }
SHIM(OpenEventA) { set_last_error(2); RET(0); }
SHIM(WaitForSingleObject) {
    KObj *o = handle_get(A(0), K_NONE); if (!o) { set_last_error(6); RET(0xFFFFFFFFu); return; }
    if (!o->signaled) { RET(0); return; }      /* files and the like are always signalled */
    RET(kwait(&o, 1, 0, A(1), 0));
}
SHIM(WaitForSingleObjectEx) { sh_WaitForSingleObject(c); }
SHIM(WaitForMultipleObjects) {
    uint32_t n = A(0); KObj *o[64]; if (n == 0 || n > 64) { set_last_error(87); RET(0xFFFFFFFFu); return; }
    for (uint32_t i = 0; i < n; i++) { o[i] = handle_get(RD32(A(1) + 4 * i), K_NONE); if (!o[i] || !o[i]->signaled) { set_last_error(6); RET(0xFFFFFFFFu); return; } }
    RET(kwait(o, (int)n, A(2) != 0, A(3), 0));
}
SHIM(CloseHandle) { if (!handle_close(A(0))) { set_last_error(6); RET(0); return; } RET(1); }
SHIM(DuplicateHandle) {
    uint32_t h = A(1) == 0xFFFFFFFEu ? 0 : handle_dup(A(1));
    if (A(1) == 0xFFFFFFFEu) { KObj *self = handle_get(0xFFFFFFFEu, K_NONE); if (self) { kobj_ref(self); h = handle_new(self); } }
    if (!h) { set_last_error(6); RET(0); return; }
    if (A(3)) WR32(A(3), h); if (A(6) & 1) handle_close(A(1)); RET(1);
}
SHIM(InterlockedIncrement) { uint32_t p = A(0), v = RD32(p) + 1; WR32(p, v); RET(v); }        /* atomic: only one guest thread runs at a time */
SHIM(InterlockedDecrement) { uint32_t p = A(0), v = RD32(p) - 1; WR32(p, v); RET(v); }
SHIM(InterlockedExchange) { uint32_t p = A(0), o = RD32(p); WR32(p, A(1)); RET(o); }
SHIM(InterlockedExchangeAdd) { uint32_t p = A(0), o = RD32(p); WR32(p, o + A(1)); RET(o); }
SHIM(InterlockedCompareExchange) { uint32_t p = A(0), o = RD32(p); if (o == A(2)) WR32(p, A(1)); RET(o); }
SHIM(Sleep) { uint32_t ms = A(0); BLOCKING(if (ms) plat_sleep_ns((uint64_t)ms * 1000000u); else plat_thread_yield()); }
SHIM(SleepEx) { uint32_t ms = A(0); BLOCKING(if (ms) plat_sleep_ns((uint64_t)ms * 1000000u); else plat_thread_yield()); RET(0); }
SHIM(SwitchToThread) { BLOCKING(plat_thread_yield()); RET(1); }

/* ---------------------------------------------------------------- threads and TLS */
SHIM(CreateThread) { uint32_t tid = 0, h = thread_create(A(2), A(3), A(1), (A(4) & 4) != 0, &tid, "CreateThread"); if (A(5)) WR32(A(5), tid); if (!h) set_last_error(8); RET(h); }
SHIM(ExitThread) { thread_exit_current(A(0)); }
SHIM(GetCurrentThread) { RET(0xFFFFFFFEu); }
SHIM(GetCurrentThreadId) { RET(cur_thread_id()); }
SHIM(GetExitCodeThread) { uint32_t code; if (!thread_exit_code(A(0), &code)) { set_last_error(6); RET(0); return; } if (A(1)) WR32(A(1), code); RET(1); }
SHIM(TerminateThread) { port_warn("TerminateThread is not supported; the thread keeps running"); RET(0); }
SHIM(SetThreadPriority) { RET(1); }
SHIM(GetThreadPriority) { RET(0); }
SHIM(SuspendThread) { RET(0); }
SHIM(ResumeThread) { RET(1); }
SHIM(SetThreadAffinityMask) { RET(1); }
SHIM(SetPriorityClass) { RET(1); }
SHIM(GetPriorityClass) { RET(0x20); }
static uint8_t tls_used[64];
SHIM(TlsAlloc) { for (uint32_t i = 0; i < 64; i++) if (!tls_used[i]) { tls_used[i] = 1; RET(i); return; } RET(0xFFFFFFFFu); }
SHIM(TlsFree) { if (A(0) < 64) tls_used[A(0)] = 0; RET(A(0) < 64); }
SHIM(TlsGetValue) { if (A(0) >= 64) { set_last_error(87); RET(0); return; } set_last_error(0); RET(RD32(cur_teb() + 0xE10 + 4 * A(0))); }
SHIM(TlsSetValue) { if (A(0) >= 64) { set_last_error(87); RET(0); return; } WR32(cur_teb() + 0xE10 + 4 * A(0), A(1)); RET(1); }

/* ---------------------------------------------------------------- time */
SHIM(GetTickCount) { RET(ms_ticks()); }
SHIM(QueryPerformanceCounter) { WR64(A(0), plat_time_ns() / 100u); RET(1); }       /* 10 MHz, like modern Windows */
SHIM(QueryPerformanceFrequency) { WR64(A(0), 10000000ull); RET(1); }
static const int mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
static int leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static void ft_to_st(uint64_t ft, uint32_t p) {            /* FILETIME -> SYSTEMTIME, proleptic Gregorian */
    uint64_t ms = ft / 10000u; uint64_t days = ms / 86400000u; uint32_t rem = (uint32_t)(ms % 86400000u);
    int y = 1601; for (;;) { int dy = leap(y) ? 366 : 365; if (days < (uint64_t)dy) break; days -= (uint64_t)dy; y++; }
    int m = 0; for (;; m++) { int dm = mdays[m] + (m == 1 && leap(y)); if (days < (uint64_t)dm) break; days -= (uint64_t)dm; }
    uint64_t total = ft / 864000000000ull; int wd = (int)((total + 1) % 7);     /* 1601-01-01 was a Monday */
    WR16(p, (uint16_t)y); WR16(p + 2, (uint16_t)(m + 1)); WR16(p + 4, (uint16_t)wd); WR16(p + 6, (uint16_t)(days + 1));
    WR16(p + 8, (uint16_t)(rem / 3600000u)); WR16(p + 10, (uint16_t)(rem / 60000u % 60)); WR16(p + 12, (uint16_t)(rem / 1000u % 60)); WR16(p + 14, (uint16_t)(rem % 1000u));
}
static uint64_t st_to_ft(uint32_t p) {
    int y = RD16(p), m = RD16(p + 2), d = RD16(p + 6); uint64_t days = 0;
    for (int yy = 1601; yy < y; yy++) days += leap(yy) ? 366 : 365;
    for (int mm = 0; mm < m - 1 && mm < 12; mm++) days += (uint64_t)(mdays[mm] + (mm == 1 && leap(y)));
    days += (uint64_t)(d - 1);
    uint64_t ms = days * 86400000u + RD16(p + 8) * 3600000u + RD16(p + 10) * 60000u + RD16(p + 12) * 1000u + RD16(p + 14);
    return ms * 10000u;
}
static uint64_t now_ft(void) { return unix_ns_to_filetime(plat_wall_time_ns()); }
static int64_t tz_ft(void) { return (int64_t)plat_utc_offset_minutes() * 600000000ll; }
SHIM(GetSystemTimeAsFileTime) { WR64(A(0), now_ft()); }
SHIM(GetSystemTime) { ft_to_st(now_ft(), A(0)); }
SHIM(GetLocalTime) { ft_to_st(now_ft() + (uint64_t)tz_ft(), A(0)); }
SHIM(FileTimeToSystemTime) { ft_to_st(RD64(A(0)), A(1)); RET(1); }
SHIM(SystemTimeToFileTime) { WR64(A(1), st_to_ft(A(0))); RET(1); }
SHIM(FileTimeToLocalFileTime) { WR64(A(1), RD64(A(0)) + (uint64_t)tz_ft()); RET(1); }
SHIM(LocalFileTimeToFileTime) { WR64(A(1), RD64(A(0)) - (uint64_t)tz_ft()); RET(1); }
SHIM(CompareFileTime) { uint64_t a = RD64(A(0)), b = RD64(A(1)); RET(a < b ? 0xFFFFFFFFu : a > b); }
SHIM(FileTimeToDosDateTime) {
    uint32_t st = g_alloc(16); ft_to_st(RD64(A(0)), st);
    WR16(A(1), (uint16_t)(((RD16(st) - 1980) << 9) | (RD16(st + 2) << 5) | RD16(st + 6))); WR16(A(2), (uint16_t)((RD16(st + 8) << 11) | (RD16(st + 10) << 5) | (RD16(st + 12) / 2)));
    g_free(st); RET(1);
}
SHIM(DosDateTimeToFileTime) {
    uint32_t st = g_alloc(16), d = A(0), t = A(1);
    WR16(st, (uint16_t)((d >> 9) + 1980)); WR16(st + 2, (uint16_t)((d >> 5) & 15)); WR16(st + 6, (uint16_t)(d & 31)); WR16(st + 8, (uint16_t)(t >> 11)); WR16(st + 10, (uint16_t)((t >> 5) & 63)); WR16(st + 12, (uint16_t)((t & 31) * 2));
    WR64(A(2), st_to_ft(st)); g_free(st); RET(1);
}
SHIM(GetTimeZoneInformation) { memset(GP(A(0)), 0, 172); WR32(A(0), (uint32_t)-plat_utc_offset_minutes()); RET(0); }

/* ---------------------------------------------------------------- code pages and strings (the game's own text is 8-bit; Windows-1252 maps 1:1 below 0x80 and for 0xA0-0xFF) */
static uint16_t cp1252_hi[32] = { 0x20AC, 0x81, 0x201A, 0x192, 0x201E, 0x2026, 0x2020, 0x2021, 0x2C6, 0x2030, 0x160, 0x2039, 0x152, 0x8D, 0x17D, 0x8F,
                                   0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x2DC, 0x2122, 0x161, 0x203A, 0x153, 0x9D, 0x17E, 0x178 };
static uint16_t to_wide(uint8_t ch) { return ch >= 0x80 && ch < 0xA0 ? cp1252_hi[ch - 0x80] : ch; }
static uint8_t from_wide(uint16_t w) { if (w < 0x80 || (w >= 0xA0 && w < 0x100)) return (uint8_t)w; for (int i = 0; i < 32; i++) if (cp1252_hi[i] == w) return (uint8_t)(0x80 + i); return '?'; }
SHIM(MultiByteToWideChar) {
    uint32_t src = A(2), dst = A(4), cap = A(5); int32_t n = (int32_t)A(3); if (n < 0) n = (int32_t)strlen(gs(src)) + 1;
    if (!cap) { RET(n); return; } if ((uint32_t)n > cap) { set_last_error(122); RET(0); return; }
    for (int32_t i = 0; i < n; i++) WR16(dst + 2 * (uint32_t)i, to_wide(RD8(src + (uint32_t)i))); RET(n);
}
SHIM(WideCharToMultiByte) {
    uint32_t src = A(2), dst = A(4), cap = A(5); int32_t n = (int32_t)A(3); if (n < 0) { n = 0; while (RD16(src + 2 * (uint32_t)n)) n++; n++; }
    if (!cap) { RET(n); return; } if ((uint32_t)n > cap) { set_last_error(122); RET(0); return; }
    for (int32_t i = 0; i < n; i++) WR8(dst + (uint32_t)i, from_wide(RD16(src + 2 * (uint32_t)i))); if (A(7)) WR32(A(7), 0); RET(n);
}
SHIM(GetACP) { RET(1252); }
SHIM(GetOEMCP) { RET(437); }
SHIM(GetCPInfo) { uint32_t p = A(1); memset(GP(p), 0, 20); WR32(p, 1); WR8(p + 4, '?'); RET(1); }
SHIM(IsValidCodePage) { RET(A(0) == 1252 || A(0) == 437 || A(0) == 0 || A(0) == 1 || A(0) == 20127 || A(0) == 28591); }
SHIM(IsDBCSLeadByte) { RET(0); }
SHIM(IsValidLocale) { RET(1); }
SHIM(EnumSystemLocalesA) { RET(1); }
SHIM(GetUserDefaultLCID) { RET(0x409); }
SHIM(GetUserDefaultLangID) { RET(0x409); }
SHIM(GetSystemDefaultLangID) { RET(0x409); }
SHIM(GetThreadLocale) { RET(0x409); }
static const char *locale_value(uint32_t type) {
    switch (type & 0xFFFF) {
    case 0x01: return "0409"; case 0x02: return "English (United States)"; case 0x03: return "ENU"; case 0x04: return "English";
    case 0x0E: return "."; case 0x0F: return ","; case 0x10: return "3;0"; case 0x14: return "$"; case 0x15: return "."; case 0x16: return ",";
    case 0x1D: return "/"; case 0x1E: return ":"; case 0x1F: return "M/d/yyyy"; case 0x20: return "dddd, MMMM dd, yyyy"; case 0x1003: return "h:mm:ss tt";
    case 0x1001: return "eng"; case 0x1004: return "1252"; case 0x0B: return "437"; case 0x59: return "en"; case 0x5A: return "US"; case 0x5C: return "en-US";
    default: return NULL;
    }
}
SHIM(GetLocaleInfoA) {
    const char *v = locale_value(A(1)); if (!v) { set_last_error(1004); RET(0); return; }
    uint32_t l = (uint32_t)strlen(v) + 1; if (!A(3)) { RET(l); return; } if (A(3) < l) { set_last_error(122); RET(0); return; } memcpy(GP(A(2)), v, l); RET(l);
}
SHIM(GetLocaleInfoW) {
    const char *v = locale_value(A(1)); if (!v) { set_last_error(1004); RET(0); return; }
    uint32_t l = (uint32_t)strlen(v) + 1; if (!A(3)) { RET(l); return; } if (A(3) < l) { set_last_error(122); RET(0); return; } for (uint32_t i = 0; i < l; i++) WR16(A(2) + 2 * i, (uint8_t)v[i]); RET(l);
}
SHIM(GetDateFormatA) { RET(0); }
SHIM(GetTimeFormatA) { RET(0); }
static int fold(int ch, int nocase) { return nocase && ch < 128 ? tolower(ch) : ch; }
SHIM(CompareStringA) {
    const char *a = gs(A(2)), *b = gs(A(4)); int la = (int)A(3), lb = (int)A(5); if (la < 0) la = (int)strlen(a); if (lb < 0) lb = (int)strlen(b);
    int r = 0, i; for (i = 0; i < la && i < lb && !r; i++) r = fold((unsigned char)a[i], A(1) & 1) - fold((unsigned char)b[i], A(1) & 1);
    if (!r) r = la - lb; RET(r < 0 ? 1 : r > 0 ? 3 : 2);
}
SHIM(CompareStringW) {
    uint32_t a = A(2), b = A(4); int la = (int)A(3), lb = (int)A(5); if (la < 0) { la = 0; while (RD16(a + 2 * (uint32_t)la)) la++; } if (lb < 0) { lb = 0; while (RD16(b + 2 * (uint32_t)lb)) lb++; }
    int r = 0, i; for (i = 0; i < la && i < lb && !r; i++) r = fold(RD16(a + 2 * (uint32_t)i), A(1) & 1) - fold(RD16(b + 2 * (uint32_t)i), A(1) & 1);
    if (!r) r = la - lb; RET(r < 0 ? 1 : r > 0 ? 3 : 2);
}
static uint16_t ctype1(int ch) {
    uint16_t t = 0; if (ch >= 128) return ch >= 0xC0 && ch != 0xD7 && ch != 0xF7 ? (ch < 0xE0 ? 0x101 : 0x102) : 0x10;
    if (isupper(ch)) t |= 0x1 | 0x100; if (islower(ch)) t |= 0x2 | 0x100; if (isdigit(ch)) t |= 0x4; if (isspace(ch)) t |= 0x8; if (ispunct(ch)) t |= 0x10;
    if (iscntrl(ch)) t |= 0x20; if (ch == ' ' || ch == '\t') t |= 0x40; if (isxdigit(ch)) t |= 0x80; return t;
}
SHIM(GetStringTypeA) { int n = (int)A(3); const char *s = gs(A(2)); if (n < 0) n = (int)strlen(s) + 1; for (int i = 0; i < n; i++) WR16(A(4) + 2 * (uint32_t)i, A(1) == 1 ? ctype1((unsigned char)s[i]) : 0); RET(1); }
SHIM(GetStringTypeExA) { int n = (int)A(3); const char *s = gs(A(2)); if (n < 0) n = (int)strlen(s) + 1; for (int i = 0; i < n; i++) WR16(A(4) + 2 * (uint32_t)i, A(1) == 1 ? ctype1((unsigned char)s[i]) : 0); RET(1); }
SHIM(GetStringTypeW) { int n = (int)A(2); uint32_t s = A(1); if (n < 0) { n = 0; while (RD16(s + 2 * (uint32_t)n)) n++; n++; } for (int i = 0; i < n; i++) { uint16_t w = RD16(s + 2 * (uint32_t)i); WR16(A(3) + 2 * (uint32_t)i, A(0) == 1 ? ctype1(w < 256 ? w : 0x80) : 0); } RET(1); }
static int lcmap(int ch, uint32_t f) { if (f & 0x100) return ch < 128 ? tolower(ch) : (ch >= 0xC0 && ch < 0xDF && ch != 0xD7 ? ch + 32 : ch); if (f & 0x200) return ch < 128 ? toupper(ch) : (ch >= 0xE0 && ch < 0xFF && ch != 0xF7 ? ch - 32 : ch); return ch; }
SHIM(LCMapStringA) {
    const char *s = gs(A(2)); int n = (int)A(3); if (n < 0) n = (int)strlen(s) + 1; uint32_t cap = A(5), f = A(1);
    if (!cap) { RET((uint32_t)n); return; } if ((uint32_t)n > cap) { set_last_error(122); RET(0); return; }
    for (int i = 0; i < n; i++) WR8(A(4) + (uint32_t)i, (uint8_t)lcmap((unsigned char)s[i], f)); RET((uint32_t)n);
}
SHIM(LCMapStringW) {
    uint32_t s = A(2); int n = (int)A(3); if (n < 0) { n = 0; while (RD16(s + 2 * (uint32_t)n)) n++; n++; } uint32_t cap = A(5), f = A(1);
    if (!cap) { RET((uint32_t)n); return; } if ((uint32_t)n > cap) { set_last_error(122); RET(0); return; }
    for (int i = 0; i < n; i++) { int ch = RD16(s + 2 * (uint32_t)i); WR16(A(4) + 2 * (uint32_t)i, (uint16_t)(ch < 256 ? lcmap(ch, f) : ch)); } RET((uint32_t)n);
}
SHIM(lstrlenA) { RET(A(0) ? strlen(gs(A(0))) : 0); }
SHIM(lstrcpyA) { const char *s = gs(A(1)); memmove(GP(A(0)), s, strlen(s) + 1); RET(A(0)); }
SHIM(lstrcpynA) { uint32_t n = A(2); if (!n) { RET(A(0)); return; } const char *s = gs(A(1)); uint32_t l = (uint32_t)strlen(s); if (l > n - 1) l = n - 1; memmove(GP(A(0)), s, l); WR8(A(0) + l, 0); RET(A(0)); }
SHIM(lstrcatA) { char *d = (char *)GP(A(0)); const char *s = gs(A(1)); memmove(d + strlen(d), s, strlen(s) + 1); RET(A(0)); }
SHIM(lstrcmpA) { int r = strcmp(gs(A(0)), gs(A(1))); RET(r < 0 ? 0xFFFFFFFFu : r > 0); }
SHIM(lstrcmpiA) { const char *a = gs(A(0)), *b = gs(A(1)); int r; do { r = tolower((unsigned char)*a) - tolower((unsigned char)*b); } while (!r && *a++ && *b++); RET(r < 0 ? 0xFFFFFFFFu : r > 0); }

/* ---------------------------------------------------------------- modules: the DirectX and system DLLs are built in */
static const char *const dll_names[] = { "kernel32", "user32", "gdi32", "advapi32", "winmm", "shell32", "ole32", "version", "msvcrt", "msvcr90", "msvcp90",
                                         "d3d9", "d3dx9_43", "d3dx9_42", "d3dx9_41", "d3dx9_40", "dsound", "dinput", "dinput8", "ddraw", "ntdll", 0 };
#define DLL_HANDLE(i) (0xFF000000u + 0x10000u * (uint32_t)(i))
static int dll_index(const char *n) {
    const char *b = strrchr(n, '\\'), *b2 = strrchr(n, '/'); if (b2 > b) b = b2; b = b ? b + 1 : n;
    char base[64]; size_t k = 0; for (; b[k] && b[k] != '.' && k < 63; k++) base[k] = (char)tolower((unsigned char)b[k]); base[k] = 0;
    for (int i = 0; dll_names[i]; i++) if (!strcmp(base, dll_names[i])) return i;
    return -1;
}
SHIM(LoadLibraryA) {
    const char *n = gs(A(0));
    uint32_t m = mod_load(c, n); if (m) { RET(m); return; }
    int i = dll_index(n); if (i >= 0) { RET(DLL_HANDLE(i)); return; }
    port_debug("LoadLibraryA(%s): not available on this host", n); set_last_error(126); RET(0);
}
SHIM(LoadLibraryExA) { sh_LoadLibraryA(c); }
SHIM(FreeLibrary) { mod_free(c, A(0)); RET(1); }
SHIM(GetModuleHandleA) {
    if (!A(0)) { RET(hd_base); return; }
    const char *n = gs(A(0)); uint32_t m = mod_handle_of(n); if (m) { RET(m); return; }
    int i = dll_index(n); if (i >= 0) { RET(DLL_HANDLE(i)); return; }
    { const char *b = strrchr(n, '\\'); b = b ? b + 1 : n; if (!strncmp(b, "SS2", 3) || !strncmp(b, "ss2", 3)) { RET(hd_base); return; } }
    set_last_error(126); RET(0);
}
SHIM(GetModuleHandleW) { if (!A(0)) { RET(hd_base); return; } char b[260]; uint32_t i = 0; for (; i < 259 && RD16(A(0) + 2 * i); i++) b[i] = (char)RD16(A(0) + 2 * i); b[i] = 0; int k = dll_index(b); RET(k >= 0 ? DLL_HANDLE(k) : 0); }
SHIM(GetModuleFileNameA) {
    char nm[300]; if (!A(0) || A(0) == hd_base) snprintf(nm, sizeof nm, "C:\\SS2.exe"); else snprintf(nm, sizeof nm, "C:\\module_%08x.dll", A(0));
    uint32_t cap = A(2), l = (uint32_t)strlen(nm); if (!cap) { RET(0); return; } if (l >= cap) { memcpy(GP(A(1)), nm, cap - 1); WR8(A(1) + cap - 1, 0); set_last_error(122); RET(cap); return; }
    memcpy(GP(A(1)), nm, l + 1); RET(l);
}
SHIM(GetModuleFileNameW) { const char *nm = "C:\\SS2.exe"; uint32_t l = (uint32_t)strlen(nm); if (A(2) <= l) { RET(0); return; } for (uint32_t i = 0; i <= l; i++) WR16(A(1) + 2 * i, (uint8_t)nm[i]); RET(l); }
SHIM(GetProcAddress) {
    uint32_t np = A(1);
    { int ism = 0; uint32_t e = mod_export(A(0), np < 0x10000 ? NULL : gs(np), &ism); if (ism) { if (!e) set_last_error(127); RET(e); return; } }
    if (np < 0x10000) { set_last_error(127); RET(0); return; }
    uint32_t a = port_proc(gs(np));
    if (!a) { port_debug("GetProcAddress(%s): not available", gs(np)); set_last_error(127); }
    RET(a);
}
SHIM(DisableThreadLibraryCalls) { RET(1); }
SHIM(FindResourceA) { RET(0); }
SHIM(WinExec) { RET(2); }
SHIM(CreateProcessA) { set_last_error(2); RET(0); }

/* ---------------------------------------------------------------- system information */
SHIM(GetSystemInfo) { uint32_t p = A(0); memset(GP(p), 0, 36); WR32(p + 4, 4096); WR32(p + 8, 0x10000); WR32(p + 12, 0x7FFEFFFFu); WR32(p + 16, 1); WR32(p + 20, 1); WR32(p + 24, 586); WR32(p + 28, 0x10000); WR16(p + 32, 6); }
SHIM(GlobalMemoryStatus) { uint32_t p = A(0); memset(GP(p), 0, 32); WR32(p, 32); WR32(p + 4, 10); WR32(p + 8, 0x40000000u); WR32(p + 12, 0x30000000u); WR32(p + 16, 0x7FFE0000u); WR32(p + 20, 0x70000000u); WR32(p + 24, 0x7FFE0000u); WR32(p + 28, 0x70000000u); }
SHIM(GlobalMemoryStatusEx) { uint32_t p = A(0); memset(GP(p) + 4, 0, 60); WR32(p + 4, 10); WR64(p + 8, 0x40000000ull); WR64(p + 16, 0x30000000ull); WR64(p + 24, 0x7FFE0000ull); WR64(p + 32, 0x70000000ull); WR64(p + 40, 0x7FFE0000ull); WR64(p + 48, 0x70000000ull); RET(1); }
SHIM(SetHandleCount) { RET(A(0)); }

const ShimDef kernel32_shims[] = {
    S(ExitProcess, STD(1)), S(TerminateProcess, STD(2)), S(FatalAppExitA, STD(2)), S(DebugBreak, 0), S(IsDebuggerPresent, 0), S(SetUnhandledExceptionFilter, STD(1)),
    S(UnhandledExceptionFilter, STD(1)), S(RaiseException, STD(4)), S(RtlUnwind, STD(4)), S(SetErrorMode, STD(1)), S(IsProcessorFeaturePresent, STD(1)), S(GetVersion, 0),
    S(GetVersionExA, STD(1)), S(GetVersionExW, STD(1)), S(GetCurrentProcess, 0), S(GetCurrentProcessId, 0), S(GetCommandLineA, 0), S(GetCommandLineW, 0), S(GetStartupInfoA, STD(1)),
    SA("GetStartupInfoW", GetStartupInfoA, STD(1)), S(GetEnvironmentStrings, 0), SA("GetEnvironmentStringsA", GetEnvironmentStrings, 0), SA("GetEnvironmentStringsW", GetEnvironmentStrings, 0),
    SA("FreeEnvironmentStringsA", FreeEnvironmentStrings, STD(1)), SA("FreeEnvironmentStringsW", FreeEnvironmentStrings, STD(1)), S(GetEnvironmentVariableA, STD(3)), S(SetEnvironmentVariableA, STD(2)),
    S(OutputDebugStringA, STD(1)), S(GetComputerNameA, STD(2)), S(GetLastError, 0), S(SetLastError, STD(1)), S(FormatMessageA, STD(7)), S(Beep, STD(2)),
    S(GetProcessHeap, 0), S(HeapCreate, STD(3)), S(HeapDestroy, STD(1)), S(HeapAlloc, STD(3)), S(HeapFree, STD(3)), S(HeapReAlloc, STD(4)), S(HeapSize, STD(3)), S(HeapValidate, STD(3)),
    S(HeapCompact, STD(2)), SA("HeapLock", HeapLockUnlock, STD(1)), SA("HeapUnlock", HeapLockUnlock, STD(1)),
    S(GlobalAlloc, STD(2)), S(GlobalFree, STD(1)), S(GlobalReAlloc, STD(3)), S(GlobalSize, STD(1)), S(GlobalLock, STD(1)), S(GlobalUnlock, STD(1)), S(GlobalHandle, STD(1)),
    S(LocalAlloc, STD(2)), S(LocalFree, STD(1)), S(LocalReAlloc, STD(3)), S(LocalSize, STD(1)),
    S(VirtualAlloc, STD(4)), S(VirtualFree, STD(3)), S(VirtualProtect, STD(4)), S(VirtualQuery, STD(3)), SA("IsBadReadPtr", IsBadPtr, STD(2)), SA("IsBadWritePtr", IsBadPtr, STD(2)),
    S(IsBadCodePtr, STD(1)), S(IsBadStringPtrA, STD(2)),
    S(InitializeCriticalSection, STD(1)), S(InitializeCriticalSectionAndSpinCount, STD(2)), S(DeleteCriticalSection, STD(1)), S(EnterCriticalSection, STD(1)),
    S(TryEnterCriticalSection, STD(1)), S(LeaveCriticalSection, STD(1)), S(CreateEventA, STD(4)), S(SetEvent, STD(1)), S(ResetEvent, STD(1)), S(PulseEvent, STD(1)),
    S(CreateMutexA, STD(3)), S(ReleaseMutex, STD(1)), S(CreateSemaphoreA, STD(4)), S(ReleaseSemaphore, STD(3)), S(OpenEventA, STD(3)),
    S(WaitForSingleObject, STD(2)), S(WaitForSingleObjectEx, STD(3)), S(WaitForMultipleObjects, STD(4)), S(CloseHandle, STD(1)), S(DuplicateHandle, STD(7)),
    S(InterlockedIncrement, STD(1)), S(InterlockedDecrement, STD(1)), S(InterlockedExchange, STD(2)), S(InterlockedExchangeAdd, STD(2)), S(InterlockedCompareExchange, STD(3)),
    S(Sleep, STD(1)), S(SleepEx, STD(2)), S(SwitchToThread, 0),
    S(CreateThread, STD(6)), S(ExitThread, STD(1)), S(GetCurrentThread, 0), S(GetCurrentThreadId, 0), S(GetExitCodeThread, STD(2)), S(TerminateThread, STD(2)),
    S(SetThreadPriority, STD(2)), S(GetThreadPriority, STD(1)), S(SuspendThread, STD(1)), S(ResumeThread, STD(1)), S(SetThreadAffinityMask, STD(2)), S(SetPriorityClass, STD(2)), S(GetPriorityClass, STD(1)),
    S(TlsAlloc, 0), S(TlsFree, STD(1)), S(TlsGetValue, STD(1)), S(TlsSetValue, STD(2)),
    S(GetTickCount, 0), S(QueryPerformanceCounter, STD(1)), S(QueryPerformanceFrequency, STD(1)), S(GetSystemTimeAsFileTime, STD(1)), S(GetSystemTime, STD(1)), S(GetLocalTime, STD(1)),
    S(FileTimeToSystemTime, STD(2)), S(SystemTimeToFileTime, STD(2)), S(FileTimeToLocalFileTime, STD(2)), S(LocalFileTimeToFileTime, STD(2)), S(CompareFileTime, STD(2)),
    S(FileTimeToDosDateTime, STD(3)), S(DosDateTimeToFileTime, STD(3)), S(GetTimeZoneInformation, STD(1)),
    S(MultiByteToWideChar, STD(6)), S(WideCharToMultiByte, STD(8)), S(GetACP, 0), S(GetOEMCP, 0), S(GetCPInfo, STD(2)), S(IsValidCodePage, STD(1)), S(IsDBCSLeadByte, STD(1)),
    S(IsValidLocale, STD(2)), S(EnumSystemLocalesA, STD(2)), S(GetUserDefaultLCID, 0), S(GetUserDefaultLangID, 0), S(GetSystemDefaultLangID, 0), S(GetThreadLocale, 0),
    S(GetLocaleInfoA, STD(4)), S(GetLocaleInfoW, STD(4)), S(GetDateFormatA, STD(6)), S(GetTimeFormatA, STD(6)), S(CompareStringA, STD(6)), S(CompareStringW, STD(6)),
    S(GetStringTypeA, STD(5)), S(GetStringTypeExA, STD(5)), S(GetStringTypeW, STD(4)), S(LCMapStringA, STD(6)), S(LCMapStringW, STD(6)),
    S(lstrlenA, STD(1)), S(lstrcpyA, STD(2)), S(lstrcpynA, STD(3)), S(lstrcatA, STD(2)), S(lstrcmpA, STD(2)), S(lstrcmpiA, STD(2)),
    S(LoadLibraryA, STD(1)), S(LoadLibraryExA, STD(3)), S(FreeLibrary, STD(1)), S(GetModuleHandleA, STD(1)), S(GetModuleHandleW, STD(1)), S(GetModuleFileNameA, STD(3)),
    S(GetModuleFileNameW, STD(3)), S(GetProcAddress, STD(2)), S(DisableThreadLibraryCalls, STD(1)), S(FindResourceA, STD(3)), S(WinExec, STD(2)), S(CreateProcessA, STD(10)),
    S(GetSystemInfo, STD(1)), S(GlobalMemoryStatus, STD(1)), S(GlobalMemoryStatusEx, STD(1)), S(SetHandleCount, STD(1)),
    { 0, 0, 0 }
};
