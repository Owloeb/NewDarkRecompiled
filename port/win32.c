/* win32.c: the Windows API the game uses (KERNEL32, USER32, GDI32, ADVAPI32, SHELL32, ole32, WINMM timers), implemented on
 * plain C and POSIX. Windowing is a stand-in: one fake window with a size, no messages arrive, nothing is drawn.
 * A real port replaces the USER32/GDI32 part with its own window, input and event loop. */
#define _GNU_SOURCE
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <dirent.h>
#include "port.h"

extern const uint32_t hd_base;
static uint32_t last_error;
static int64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000000000ll + t.tv_nsec; }
static int64_t t0_ns;
static uint32_t tick_ms(void) { if (!t0_ns) t0_ns = now_ns(); return (uint32_t)((now_ns() - t0_ns) / 1000000) + 100000u; }
#define SETERR(e) (last_error = (e))
void set_last_error(uint32_t e) { last_error = e; }
static void ret64(CPU *c, uint64_t v) { c->eax = (uint32_t)v; c->edx = (uint32_t)(v >> 32); }

/* ---------------------------------------------------------------- data imports and the import address table */
void port_data_import(unsigned idx, const char *name, uint32_t slot) {
    if (!strcmp(name, "_acmdln")) { uint32_t v = g_alloc(4); WR32(v, g_str("SS2.exe")); WR32(slot, v); }
    else if (!strcmp(name, "_adjust_fdiv")) WR32(slot, g_alloc(4));
    else WR32(slot, THUNK_BASE + 16 * idx);
}

/* ---------------------------------------------------------------- KERNEL32: process, time, misc */
SHIM(Sleep) { uint32_t ms = A(0); if (ms) { struct timespec t = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&t, NULL); } }
SHIM(GetTickCount) { RET(tick_ms()); }
SHIM(timeGetTime) { RET(tick_ms()); }
SHIM(QueryPerformanceCounter) { uint64_t v = (uint64_t)(now_ns() / 100); WR64(A(0), v); RET(1); }
SHIM(QueryPerformanceFrequency) { WR64(A(0), 10000000ull); RET(1); }
SHIM(GetLastError) { RET(last_error); }
SHIM(ExitProcess) { port_log("ExitProcess(%u)", A(0)); port_exit((int)A(0)); }
SHIM(TerminateProcess) { port_log("TerminateProcess(%u)", A(1)); port_exit((int)A(1)); }
SHIM(DebugBreak) { port_die("DebugBreak"); }
SHIM(IsDebuggerPresent) { RET(0); }
SHIM(SetUnhandledExceptionFilter) { RET(0); }
SHIM(UnhandledExceptionFilter) { port_die("unhandled exception reached UnhandledExceptionFilter"); }
SHIM(GetVersion) { RET(0x0A280105u); }
SHIM(GetVersionExA) { uint32_t p = A(0); WR32(p + 4, 5); WR32(p + 8, 1); WR32(p + 12, 2600); WR32(p + 16, 2); WR8(p + 20, 0); RET(1); }
SHIM(GetCurrentProcess) { RET(0xFFFFFFFFu); }
SHIM(GetCurrentProcessId) { RET(1000); }
SHIM(GetCurrentThread) { RET(0xFFFFFFFEu); }
SHIM(GetCurrentThreadId) { RET(1001); }
SHIM(SetThreadPriority) { RET(1); }
SHIM(GetThreadPriority) { RET(0); }
SHIM(SuspendThread) { RET(0); }
SHIM(ResumeThread) { RET(1); }
SHIM(GetExitCodeThread) { if (A(1)) WR32(A(1), 259); RET(1); }
SHIM(GetUserDefaultLangID) { RET(0x0409); }
SHIM(GetSystemDefaultLangID) { RET(0x0409); }
SHIM(GetStartupInfoA) { memset(GP(A(0)), 0, 68); WR32(A(0), 68); }
SHIM(GetCommandLineA) { static uint32_t s; if (!s) s = g_str("SS2.exe"); RET(s); }
SHIM(GetModuleFileNameA) { const char *n = "C:\\SS2.exe"; uint32_t cap = A(2); if (A(0) && A(0) != hd_base) n = "C:\\unknown.dll"; uint32_t l = (uint32_t)strlen(n); if (l >= cap) l = cap ? cap - 1 : 0; memcpy(GP(A(1)), n, l); if (cap) WR8(A(1) + l, 0); RET(l); }
SHIM(GetSystemInfo) { uint32_t p = A(0); memset(GP(p), 0, 36); WR32(p + 4, 4096); WR32(p + 8, 0x10000); WR32(p + 12, 0x7FFEFFFFu); WR32(p + 16, 1); WR32(p + 20, 1); WR32(p + 24, 586); WR32(p + 28, 0x10000); WR16(p + 32, 6); }
SHIM(GlobalMemoryStatus) { uint32_t p = A(0); memset(GP(p), 0, 32); WR32(p, 32); WR32(p + 4, 10); WR32(p + 8, 0x40000000u); WR32(p + 12, 0x30000000u); WR32(p + 16, 0x7FFE0000u); WR32(p + 20, 0x70000000u); WR32(p + 24, 0x7FFE0000u); WR32(p + 28, 0x70000000u); }
SHIM(GlobalMemoryStatusEx) { uint32_t p = A(0); memset(GP(p), 0, 64); WR32(p, 64); WR32(p + 4, 10); WR64(p + 8, 0x40000000ull); WR64(p + 16, 0x30000000ull); WR64(p + 24, 0x7FFE0000ull); WR64(p + 32, 0x70000000ull); WR64(p + 40, 0x7FFE0000ull); WR64(p + 48, 0x70000000ull); RET(1); }
SHIM(OutputDebugStringA) { if (port_trace) fprintf(stderr, "[debug] %s", gs(A(0))); }
SHIM(GetStdHandle) { RET(0x10 + (A(0) & 3)); }
SHIM(WriteConsoleA) { fwrite(GP(A(1)), 1, A(2), stdout); if (A(3)) WR32(A(3), A(2)); RET(1); }
SHIM(ConsoleNop) { RET(1); }
SHIM(GetConsoleScreenBufferInfo) { memset(GP(A(1)), 0, 22); RET(1); }

/* locale / string conversion (Latin-1 is enough: the game's own text is ASCII) */
SHIM(MultiByteToWideChar) {
    uint32_t src = A(2), n = A(3), dst = A(4), cap = A(5); int whole = (int32_t)n < 0; if (whole) n = (uint32_t)strlen(gs(src)) + 1;
    if (!cap) { RET(n); return; } if (n > cap) { SETERR(122); RET(0); return; }
    for (uint32_t i = 0; i < n; i++) WR16(dst + 2 * i, RD8(src + i)); RET(n);
}
SHIM(WideCharToMultiByte) {
    uint32_t src = A(2), n = A(3), dst = A(4), cap = A(5); if ((int32_t)n < 0) { n = 0; while (RD16(src + 2 * n)) n++; n++; }
    if (!cap) { RET(n); return; } if (n > cap) { SETERR(122); RET(0); return; }
    for (uint32_t i = 0; i < n; i++) { uint16_t w = RD16(src + 2 * i); WR8(dst + i, w < 256 ? (uint8_t)w : '?'); } RET(n);
}

/* critical sections, events, mutexes: the host runs the game on one thread, so none of them ever blocks */
SHIM(CsNop) { }
SHIM(Ret1) { RET(1); }
SHIM(Ret0) { RET(0); }
static uint32_t next_handle = 0x400;
SHIM(NewHandle) { RET(next_handle += 4); }
SHIM(WaitForSingleObject) { RET(0); }
SHIM(CloseHandle) { uint32_t h = A(0); if (h >= 0x2000 && h < 0x2000 + 4 * 1024) close((int)((h - 0x2000) / 4)); RET(1); }
SHIM(DuplicateHandle) { if (A(3)) WR32(A(3), A(1)); RET(1); }
SHIM(InterlockedIncrement) { uint32_t p = A(0); uint32_t v = RD32(p) + 1; WR32(p, v); RET(v); }
SHIM(InterlockedDecrement) { uint32_t p = A(0); uint32_t v = RD32(p) - 1; WR32(p, v); RET(v); }
SHIM(InterlockedExchange) { uint32_t p = A(0), o = RD32(p); WR32(p, A(1)); RET(o); }
SHIM(InterlockedCompareExchange) { uint32_t p = A(0), o = RD32(p); if (o == A(2)) WR32(p, A(1)); RET(o); }

/* heaps and virtual memory */
SHIM(GetProcessHeap) { RET(0x100); }
SHIM(HeapCreate) { RET(0x104); }
SHIM(HeapAlloc) { uint32_t p = g_alloc(A(2)); if (!p) SETERR(8); RET(p); }
SHIM(HeapFree) { g_free(A(2)); RET(1); }
SHIM(HeapReAlloc) { RET(g_realloc(A(2), A(3))); }
SHIM(HeapSize) { RET(g_size(A(2))); }
SHIM(HeapCompact) { RET(0x10000); }
static uint32_t va_top = 0x80000000u;
SHIM(VirtualAlloc) {
    uint32_t want = A(0), sz = (A(1) + 0xFFFF) & ~0xFFFFu; if (!sz) sz = 0x10000;
    if (want && want >= 0x80000000u && want + sz <= 0xE0000000u) { RET(want); return; }
    if ((uint64_t)va_top + sz > 0xE0000000u) { SETERR(8); RET(0); return; }
    uint32_t p = va_top; va_top += sz; RET(p);
}
SHIM(VirtualFree) { if ((A(2) & 0x4000) == 0) { /* MEM_DECOMMIT */ } RET(1); }
SHIM(VirtualProtect) { if (A(3)) WR32(A(3), 4); RET(1); }

/* time */
static uint64_t unix_to_ft(time_t t, long ns) { return ((uint64_t)t + 11644473600ull) * 10000000ull + (uint64_t)ns / 100; }
SHIM(GetSystemTimeAsFileTime) { struct timespec t; clock_gettime(CLOCK_REALTIME, &t); WR64(A(0), unix_to_ft(t.tv_sec, t.tv_nsec)); }
static void put_systime(uint32_t p, struct tm *tm, int ms) {
    WR16(p, (uint16_t)(tm->tm_year + 1900)); WR16(p + 2, (uint16_t)(tm->tm_mon + 1)); WR16(p + 4, (uint16_t)tm->tm_wday); WR16(p + 6, (uint16_t)tm->tm_mday);
    WR16(p + 8, (uint16_t)tm->tm_hour); WR16(p + 10, (uint16_t)tm->tm_min); WR16(p + 12, (uint16_t)tm->tm_sec); WR16(p + 14, (uint16_t)ms);
}
SHIM(GetLocalTime) { struct timespec t; clock_gettime(CLOCK_REALTIME, &t); struct tm tm; localtime_r(&t.tv_sec, &tm); put_systime(A(0), &tm, (int)(t.tv_nsec / 1000000)); }
SHIM(FileTimeToSystemTime) { uint64_t ft = RD64(A(0)); time_t t = (time_t)(ft / 10000000ull - 11644473600ull); struct tm tm; gmtime_r(&t, &tm); put_systime(A(1), &tm, (int)((ft / 10000) % 1000)); RET(1); }
SHIM(FileTimeToLocalFileTime) { WR64(A(1), RD64(A(0))); RET(1); }
SHIM(FileTimeToDosDateTime) {
    uint64_t ft = RD64(A(0)); time_t t = (time_t)(ft / 10000000ull - 11644473600ull); struct tm tm; gmtime_r(&t, &tm);
    WR16(A(1), (uint16_t)(((tm.tm_year - 80) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday)); WR16(A(2), (uint16_t)((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2))); RET(1);
}

/* files */
SHIM(CreateFileA) {
    char hp[1400]; host_path(gs(A(0)), hp, sizeof hp); uint32_t acc = A(1), disp = A(4); int fl = (acc & 0x40000000u) ? ((acc & 0x80000000u) ? O_RDWR : O_WRONLY) : O_RDONLY;
    if (disp == 1) fl |= O_CREAT | O_EXCL; else if (disp == 2) fl |= O_CREAT | O_TRUNC; else if (disp == 4) fl |= O_CREAT; else if (disp == 5) fl |= O_TRUNC;
    int fd = open(hp, fl, 0644);
    if (fd < 0) { SETERR(errno == ENOENT ? 2 : 5); RET(0xFFFFFFFFu); return; }
    RET(0x2000 + 4 * fd);
}
SHIM(GetFileTime) {
    int fd = (int)((A(0) - 0x2000) / 4); struct stat st; if (fstat(fd, &st)) { RET(0); return; }
    uint64_t m = unix_to_ft(st.st_mtime, 0); if (A(1)) WR64(A(1), unix_to_ft(st.st_ctime, 0)); if (A(2)) WR64(A(2), unix_to_ft(st.st_atime, 0)); if (A(3)) WR64(A(3), m); RET(1);
}
SHIM(CopyFileA) {
    char a[1400], b[1400]; host_path(gs(A(0)), a, sizeof a); host_path(gs(A(1)), b, sizeof b);
    FILE *f = fopen(a, "rb"); if (!f) { SETERR(2); RET(0); return; }
    if (A(2)) { struct stat st; if (!stat(b, &st)) { fclose(f); SETERR(80); RET(0); return; } }
    FILE *g = fopen(b, "wb"); if (!g) { fclose(f); SETERR(5); RET(0); return; }
    char buf[8192]; size_t n; while ((n = fread(buf, 1, sizeof buf, f)) > 0) fwrite(buf, 1, n, g); fclose(f); fclose(g); RET(1);
}
SHIM(GetDriveTypeA) { RET(3); }
SHIM(GetVolumeInformationA) { if (A(1) && A(2)) { WR8(A(1), 0); } if (A(3)) WR32(A(3), 0x12345678u); if (A(4)) WR32(A(4), 255); if (A(5)) WR32(A(5), 0); RET(1); }
SHIM(GetDiskFreeSpaceExA) { if (A(1)) WR64(A(1), 8ull << 30); if (A(2)) WR64(A(2), 64ull << 30); if (A(3)) WR64(A(3), 8ull << 30); RET(1); }
SHIM(FindResourceA) { RET(0); }
SHIM(WinExec) { RET(2); }
SHIM(CreateProcessA) { SETERR(2); RET(0); }

/* private profile (.ini) files */
static int ini_find(const char *file, const char *sec, const char *key, char *out, size_t n) {
    char hp[1400]; host_path(file, hp, sizeof hp); FILE *f = fopen(hp, "r"); if (!f) return 0;
    char line[1024]; int in = 0, found = 0;
    while (fgets(line, sizeof line, f)) {
        char *s = line; while (*s == ' ' || *s == '\t') s++;
        size_t l = strlen(s); while (l && (s[l - 1] == '\n' || s[l - 1] == '\r' || s[l - 1] == ' ')) s[--l] = 0;
        if (*s == '[') { char *e = strchr(s, ']'); in = e && (size_t)(e - s - 1) == strlen(sec) && !strncasecmp(s + 1, sec, strlen(sec)); continue; }
        if (!in || *s == ';' || !*s) continue;
        char *eq = strchr(s, '='); if (!eq) continue; char *ke = eq; while (ke > s && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
        if ((size_t)(ke - s) == strlen(key) && !strncasecmp(s, key, strlen(key))) { char *v = eq + 1; while (*v == ' ' || *v == '\t') v++; snprintf(out, n, "%s", v); found = 1; break; }
    }
    fclose(f); return found;
}
SHIM(GetPrivateProfileIntA) { char v[256]; RET(ini_find(gs(A(3)), gs(A(0)), gs(A(1)), v, sizeof v) ? (uint32_t)strtol(v, NULL, 0) : A(2)); }
SHIM(GetPrivateProfileStringA) {
    char v[1024]; const char *r = ini_find(gs(A(5)), gs(A(0)), gs(A(1)), v, sizeof v) ? v : gs(A(2)); uint32_t cap = A(4), l = (uint32_t)strlen(r);
    if (!cap) { RET(0); return; } if (l >= cap) l = cap - 1; memcpy(GP(A(3)), r, l); WR8(A(3) + l, 0); RET(l);
}

/* ---------------------------------------------------------------- modules: the DirectX DLLs are built in */
static const char *const dll_names[] = { "d3d9.dll", "d3dx9_43.dll", "d3dx9_42.dll", "d3dx9_41.dll", "d3dx9_40.dll", "dsound.dll", "dinput.dll", "dinput8.dll", "version.dll", "ddraw.dll", 0 };
static const ShimDef *proc_cache[256]; static uint32_t proc_addr[256]; static int nproc;
SHIM(LoadLibraryA) {
    const char *n = gs(A(0)); const char *b = strrchr(n, '\\'); b = b ? b + 1 : n;
    { uint32_t m = mod_load(c, n); if (m) { RET(m); return; } }
    for (int i = 0; dll_names[i]; i++) if (!strncasecmp(b, dll_names[i], strlen(dll_names[i]) - 4)) { RET(0xEE000000u + 0x10000u * (i + 1)); return; }
    if (port_trace) port_log("LoadLibraryA(%s): not available on this host", n);
    SETERR(126); RET(0);
}
SHIM(FreeLibrary) { mod_free(c, A(0)); RET(1); }
SHIM(GetModuleHandleA) {
    if (!A(0)) { RET(hd_base); return; }
    const char *n = gs(A(0));
    { extern uint32_t mod_handle_of(const char *); uint32_t m = mod_handle_of(n); if (m) { RET(m); return; } }
    for (int i = 0; dll_names[i]; i++) if (!strcasecmp(n, dll_names[i])) { RET(0xEE000000u + 0x10000u * (i + 1)); return; }
    RET(0);
}
uint32_t port_proc(const char *n) {
    for (int i = 0; i < nproc; i++) if (!strcmp(proc_cache[i]->name, n)) return proc_addr[i];
    const ShimDef *s = shim_find(n);
    if (!s || nproc >= 256) return 0;
    proc_cache[nproc] = s; proc_addr[nproc] = g_thunk(s->fn, s->pop, s->name); return proc_addr[nproc++];
}
SHIM(GetProcAddress) {
    uint32_t np = A(1);
    { int ism = 0; uint32_t e = mod_export(A(0), np < 0x10000 ? NULL : gs(np), &ism); if (ism) { if (!e) SETERR(127); RET(e); return; } }
    if (np < 0x10000) { RET(0); return; }
    const char *n = gs(np);
    uint32_t a = port_proc(n);
    if (!a) { if (port_trace) port_log("GetProcAddress(%s): not available", n); SETERR(127); }
    RET(a);
}

/* ---------------------------------------------------------------- WINMM timers */
SHIM(timeGetDevCaps) { if (A(0)) { WR32(A(0), 1); WR32(A(0) + 4, 1000000); } RET(0); }
SHIM(timeSetEvent) { RET(1); }       /* the callbacks run on another thread in Windows; the single-threaded host never fires them */
SHIM(mciSendCommandA) { RET(0x10B); }  /* MCIERR_DEVICE_NOT_INSTALLED-ish: no CD audio */

/* ---------------------------------------------------------------- USER32: one fake window */
uint32_t g_ww = 1280, g_wh = 720; static uint32_t wndproc, g_hwnd = 0x20001, wl[64];
#define ww g_ww
#define wh g_wh
static int shown;
SHIM(RegisterClassA) { wndproc = RD32(A(0) + 4); RET(0xC001); }
SHIM(CreateWindowExA) {
    uint32_t cs = g_alloc(48); WR32(cs, A(11)); WR32(cs + 4, A(10)); WR32(cs + 8, A(9)); WR32(cs + 12, A(8)); WR32(cs + 16, A(7)); WR32(cs + 20, A(6)); WR32(cs + 24, A(5)); WR32(cs + 28, A(4));
    WR32(cs + 32, A(3)); WR32(cs + 36, A(2)); WR32(cs + 40, A(1)); WR32(cs + 44, A(0));
    if ((int32_t)A(6) > 0 && (int32_t)A(7) > 0) { ww = A(6); wh = A(7); }
    wl[(uint32_t)-4 & 63] = wndproc; wl[(uint32_t)-16 & 63] = A(3); wl[(uint32_t)-20 & 63] = A(0);
    if (wndproc) { g_call(g_cpup, wndproc, 4, g_hwnd, 0x81, 0, cs); g_call(g_cpup, wndproc, 4, g_hwnd, 0x01, 0, cs); }
    RET(g_hwnd);
}
static void activate(void) {
    if (shown || !wndproc) return; shown = 1;
    g_call(g_cpup, wndproc, 4, g_hwnd, 0x18, 1, 0);     /* WM_SHOWWINDOW */
    g_call(g_cpup, wndproc, 4, g_hwnd, 0x1C, 1, 1001);  /* WM_ACTIVATEAPP */
    g_call(g_cpup, wndproc, 4, g_hwnd, 0x06, 1, 0);     /* WM_ACTIVATE */
    g_call(g_cpup, wndproc, 4, g_hwnd, 0x07, 0, 0);     /* WM_SETFOCUS */
}
SHIM(ShowWindow) { activate(); RET(0); }
SHIM(SetForegroundWindow) { activate(); RET(1); }
SHIM(SetFocus) { activate(); RET(0); }
SHIM(DefWindowProcA) { RET(A(1) == 0x81 ? 1 : 0); }
SHIM(GetClientRect) { uint32_t p = A(1); WR32(p, 0); WR32(p + 4, 0); WR32(p + 8, ww); WR32(p + 12, wh); RET(1); }
SHIM(GetWindowRect) { uint32_t p = A(1); WR32(p, 0); WR32(p + 4, 0); WR32(p + 8, ww); WR32(p + 12, wh); RET(1); }
SHIM(GetSystemMetrics) { switch (A(0)) { case 0: RET(1920); break; case 1: RET(1080); break; case 16: RET(1920); break; case 17: RET(1080); break; default: RET(0); } }
SHIM(GetWindowLongA) { RET(wl[A(1) & 63]); }
SHIM(SetWindowLongA) { uint32_t o = wl[A(1) & 63]; wl[A(1) & 63] = A(2); RET(o); }
SHIM(GetDesktopWindow) { RET(0x10010); }
SHIM(GetActiveWindow) { RET(g_hwnd); }
SHIM(GetForegroundWindow) { RET(g_hwnd); }
SHIM(GetDC) { RET(0x30001); }
SHIM(GetCursorPos) { WR32(A(0), ww / 2); WR32(A(0) + 4, wh / 2); RET(1); }
SHIM(ScreenToClient) { RET(1); }
SHIM(ClientToScreen) { RET(1); }
SHIM(MapWindowPoints) { RET(0); }
SHIM(AdjustWindowRectEx) { RET(1); }
SHIM(IsRectEmpty) { uint32_t p = A(0); RET((int32_t)RD32(p + 8) <= (int32_t)RD32(p) || (int32_t)RD32(p + 12) <= (int32_t)RD32(p + 4)); }
SHIM(PtInRect) { uint32_t r = A(0); int32_t x = (int32_t)A(1), y = (int32_t)A(2); RET(x >= (int32_t)RD32(r) && x < (int32_t)RD32(r + 8) && y >= (int32_t)RD32(r + 4) && y < (int32_t)RD32(r + 12)); }
SHIM(IntersectRect) {
    uint32_t d = A(0), a = A(1), b = A(2); int32_t l = (int32_t)RD32(a) > (int32_t)RD32(b) ? (int32_t)RD32(a) : (int32_t)RD32(b), t = (int32_t)RD32(a + 4) > (int32_t)RD32(b + 4) ? (int32_t)RD32(a + 4) : (int32_t)RD32(b + 4);
    int32_t r = (int32_t)RD32(a + 8) < (int32_t)RD32(b + 8) ? (int32_t)RD32(a + 8) : (int32_t)RD32(b + 8), bt = (int32_t)RD32(a + 12) < (int32_t)RD32(b + 12) ? (int32_t)RD32(a + 12) : (int32_t)RD32(b + 12);
    if (r <= l || bt <= t) { memset(GP(d), 0, 16); RET(0); return; } WR32(d, l); WR32(d + 4, t); WR32(d + 8, r); WR32(d + 12, bt); RET(1);
}
SHIM(GetMonitorInfoA) { uint32_t p = A(1); WR32(p + 4, 0); WR32(p + 8, 0); WR32(p + 12, 1920); WR32(p + 16, 1080); WR32(p + 20, 0); WR32(p + 24, 0); WR32(p + 28, 1920); WR32(p + 32, 1080); WR32(p + 36, 1); RET(1); }
SHIM(PeekMessageA) { RET(0); }
SHIM(GetMessageA) { RET(0); }
SHIM(MsgWaitForMultipleObjects) { struct timespec t = { 0, 2000000L }; nanosleep(&t, NULL); RET(258); }
SHIM(MessageBoxA) { fprintf(stderr, "[port] MessageBox \"%s\": %s\n", gs(A(2)), gs(A(1))); RET(1); }
SHIM(DialogBoxParamA) { RET(0); }
SHIM(GetKeyboardType) { RET(4); }
SHIM(GetKeyboardLayout) { RET(0x04090409u); }
SHIM(MapVirtualKeyA) { RET(0); }
SHIM(GetSysColor) { RET(0); }
SHIM(LoadIconA) { RET(0x40001); }
SHIM(LoadCursorA) { RET(0x40002); }
SHIM(GetMessageTime) { RET(tick_ms()); }

/* ---------------------------------------------------------------- GDI32: palette/DIB calls the 2D fallback path makes; nothing is drawn */
SHIM(GetDeviceCaps) { switch (A(1)) { case 8: RET(1920); break; case 10: RET(1080); break; case 12: RET(32); break; case 14: RET(1); break; case 88: case 90: RET(96); break; default: RET(0); } }
SHIM(GetObjectA) { RET(0); }
SHIM(GetStockObject) { RET(0x50001); }
SHIM(CreateCompatibleDC) { RET(0x30002); }
SHIM(CreateDIBSection) { RET(0); }
SHIM(CreatePalette) { RET(0x50002); }
SHIM(GetSystemPaletteUse) { RET(1); }

/* ---------------------------------------------------------------- ADVAPI32 / SHELL32 / ole32: no registry, no shell, no COM */
SHIM(RegFail) { RET(2); }
SHIM(CoInitialize) { RET(0); }
SHIM(CoCreateInstance) { RET(0x80040154u); }

#define S(n, p) { #n, sh_##n, p }
#define SA(name, impl, p) { name, sh_##impl, p }
const ShimDef win32_shims[] = {
    S(Sleep, STD(1)), S(GetTickCount, 0), S(timeGetTime, 0), S(QueryPerformanceCounter, STD(1)), S(QueryPerformanceFrequency, STD(1)), S(GetLastError, 0), S(ExitProcess, STD(1)),
    S(TerminateProcess, STD(2)), S(DebugBreak, 0), S(IsDebuggerPresent, 0), S(SetUnhandledExceptionFilter, STD(1)), S(UnhandledExceptionFilter, STD(1)), S(GetVersion, 0), S(GetVersionExA, STD(1)),
    S(GetCurrentProcess, 0), S(GetCurrentProcessId, 0), S(GetCurrentThread, 0), S(GetCurrentThreadId, 0), S(SetThreadPriority, STD(2)), S(GetThreadPriority, STD(1)), S(SuspendThread, STD(1)), S(ResumeThread, STD(1)),
    S(GetExitCodeThread, STD(2)), S(GetUserDefaultLangID, 0), S(GetSystemDefaultLangID, 0), S(GetStartupInfoA, STD(1)), S(GetCommandLineA, 0), S(GetModuleFileNameA, STD(3)), S(GetSystemInfo, STD(1)),
    S(GlobalMemoryStatus, STD(1)), S(GlobalMemoryStatusEx, STD(1)), S(OutputDebugStringA, STD(1)), S(GetStdHandle, STD(1)), S(WriteConsoleA, STD(5)),
    SA("SetConsoleCursorPosition", ConsoleNop, STD(2)), SA("SetConsoleTextAttribute", ConsoleNop, STD(2)), S(GetConsoleScreenBufferInfo, STD(2)),
    S(MultiByteToWideChar, STD(6)), S(WideCharToMultiByte, STD(8)),
    SA("InitializeCriticalSection", CsNop, STD(1)), SA("DeleteCriticalSection", CsNop, STD(1)), SA("EnterCriticalSection", CsNop, STD(1)), SA("LeaveCriticalSection", CsNop, STD(1)),
    SA("SetEvent", Ret1, STD(1)), SA("ResetEvent", Ret1, STD(1)), SA("ReleaseMutex", Ret1, STD(1)), SA("CreateEventA", NewHandle, STD(4)), SA("CreateMutexA", NewHandle, STD(3)),
    S(WaitForSingleObject, STD(2)), S(CloseHandle, STD(1)), S(DuplicateHandle, STD(7)),
    S(InterlockedIncrement, STD(1)), S(InterlockedDecrement, STD(1)), S(InterlockedExchange, STD(2)), S(InterlockedCompareExchange, STD(3)),
    S(GetProcessHeap, 0), S(HeapCreate, STD(3)), S(HeapAlloc, STD(3)), S(HeapFree, STD(3)), S(HeapReAlloc, STD(4)), S(HeapSize, STD(3)), S(HeapCompact, STD(2)),
    S(VirtualAlloc, STD(4)), S(VirtualFree, STD(3)), S(VirtualProtect, STD(4)),
    S(GetSystemTimeAsFileTime, STD(1)), S(GetLocalTime, STD(1)), S(FileTimeToSystemTime, STD(2)), S(FileTimeToLocalFileTime, STD(2)), S(FileTimeToDosDateTime, STD(3)),
    S(CreateFileA, STD(7)), S(GetFileTime, STD(4)), S(CopyFileA, STD(3)), S(GetDriveTypeA, STD(1)), S(GetVolumeInformationA, STD(8)), S(GetDiskFreeSpaceExA, STD(4)), S(FindResourceA, STD(3)),
    S(WinExec, STD(2)), S(CreateProcessA, STD(10)), S(GetPrivateProfileIntA, STD(4)), S(GetPrivateProfileStringA, STD(6)),
    S(LoadLibraryA, STD(1)), S(FreeLibrary, STD(1)), S(GetModuleHandleA, STD(1)), S(GetProcAddress, STD(2)),
    S(timeGetDevCaps, STD(2)), S(timeSetEvent, STD(5)), SA("timeKillEvent", Ret0, STD(1)), SA("timeBeginPeriod", Ret0, STD(1)), SA("timeEndPeriod", Ret0, STD(1)), S(mciSendCommandA, STD(4)),
    S(RegisterClassA, STD(1)), S(CreateWindowExA, STD(12)), S(ShowWindow, STD(2)), S(SetForegroundWindow, STD(1)), S(SetFocus, STD(1)), S(DefWindowProcA, STD(4)),
    S(GetClientRect, STD(2)), S(GetWindowRect, STD(2)), S(GetSystemMetrics, STD(1)), S(GetWindowLongA, STD(2)), S(SetWindowLongA, STD(3)), S(GetDesktopWindow, 0), S(GetActiveWindow, 0),
    S(GetForegroundWindow, 0), S(GetDC, STD(1)), S(GetCursorPos, STD(1)), S(ScreenToClient, STD(2)), S(ClientToScreen, STD(2)), S(MapWindowPoints, STD(4)), S(AdjustWindowRectEx, STD(4)),
    S(IsRectEmpty, STD(1)), S(PtInRect, STD(3)), S(IntersectRect, STD(3)), S(GetMonitorInfoA, STD(2)), S(PeekMessageA, STD(5)), S(GetMessageA, STD(4)),
    S(MsgWaitForMultipleObjects, STD(5)), S(MessageBoxA, STD(4)), S(DialogBoxParamA, STD(5)), S(GetKeyboardType, STD(1)), S(GetKeyboardLayout, STD(1)), S(MapVirtualKeyA, STD(2)),
    S(GetSysColor, STD(1)), S(LoadIconA, STD(2)), S(LoadCursorA, STD(2)), S(GetMessageTime, 0),
    SA("MoveWindow", Ret1, STD(6)), SA("SetSysColors", Ret1, STD(3)), SA("GetMenu", Ret0, STD(1)), SA("FillRect", Ret1, STD(3)), SA("InvalidateRect", Ret1, STD(3)), SA("IsIconic", Ret0, STD(1)),
    SA("BringWindowToTop", Ret1, STD(1)), SA("SetCapture", Ret0, STD(1)), SA("GetCapture", Ret0, 0), SA("SetCursorPos", Ret1, STD(2)), SA("ReleaseCapture", Ret1, 0), SA("IsWindowVisible", Ret1, STD(1)),
    SA("EndPaint", Ret1, STD(2)), SA("DestroyWindow", Ret1, STD(1)), SA("SetCursor", Ret0, STD(1)), SA("CloseClipboard", Ret1, 0), SA("PostQuitMessage", Ret0, STD(1)), SA("GetParent", Ret0, STD(1)),
    SA("BeginPaint", Ret0, STD(2)), SA("WinHelpA", Ret1, STD(4)), SA("EmptyClipboard", Ret1, 0), SA("PostMessageA", Ret1, STD(4)), SA("OpenClipboard", Ret0, STD(1)), SA("UpdateWindow", Ret1, STD(1)),
    SA("GetWindow", Ret0, STD(2)), SA("TranslateMessage", Ret0, STD(1)), SA("DispatchMessageA", Ret0, STD(1)), SA("ClipCursor", Ret1, STD(1)), SA("SetWindowTextA", Ret1, STD(2)), SA("SetWindowPos", Ret1, STD(7)),
    SA("GetKeyState", Ret0, STD(1)), SA("IsDlgButtonChecked", Ret0, STD(2)), SA("ReleaseDC", Ret1, STD(2)), SA("DestroyMenu", Ret1, STD(1)), SA("GetAsyncKeyState", Ret0, STD(1)), SA("GetDlgItem", Ret0, STD(2)), SA("EndDialog", Ret1, STD(2)),
    S(GetDeviceCaps, STD(2)), S(GetObjectA, STD(3)), S(GetStockObject, STD(1)), S(CreateCompatibleDC, STD(1)), S(CreateDIBSection, STD(6)), S(CreatePalette, STD(1)), S(GetSystemPaletteUse, STD(1)),
    SA("DeleteObject", Ret1, STD(1)), SA("GdiFlush", Ret1, 0), SA("DeleteDC", Ret1, STD(1)), SA("BitBlt", Ret1, STD(9)), SA("SelectObject", Ret0, STD(2)), SA("SelectPalette", Ret0, STD(3)),
    SA("RealizePalette", Ret0, STD(1)), SA("SetDIBColorTable", Ret0, STD(4)), SA("StretchBlt", Ret1, STD(11)), SA("SetSystemPaletteUse", Ret1, STD(2)),
    SA("RegOpenKeyExA", RegFail, STD(5)), SA("RegQueryValueExA", RegFail, STD(6)), SA("RegSetValueExA", RegFail, STD(6)), SA("RegCloseKey", Ret0, STD(1)),
    SA("DragQueryFileA", Ret0, STD(4)), SA("DragFinish", Ret0, STD(1)), S(CoInitialize, STD(1)), S(CoCreateInstance, STD(5)),
    { 0, 0, 0 }
};
