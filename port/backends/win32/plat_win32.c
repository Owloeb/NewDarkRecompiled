/* plat_win32.c: the "sys" and "fs" parts of plat.h for Windows (Windows 7 and later; 64-bit recommended).
 * Window, input, audio and rendering come from another backend (backends/sdl2 works on Windows unchanged).
 *
 * Guest memory: the core asks for 4 GB on 64-bit hosts. Committing that much up front would count against the system
 * commit limit even though the game touches only a fraction, so the range is only reserved and pages are committed on
 * first touch by a vectored exception handler, 64 KB at a time. Untouched memory reads as zero, as on POSIX. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"

/* ---------------------------------------------------------------- log */
static SRWLOCK log_lock = SRWLOCK_INIT;
void plat_log_write(PlatLogLevel level, const char *line) {
    AcquireSRWLockExclusive(&log_lock);
    FILE *o = level >= PLAT_LOG_WARN ? stderr : stdout;
    fputs(line, o); fputc('\n', o); fflush(o);
    ReleaseSRWLockExclusive(&log_lock);
}

/* ---------------------------------------------------------------- memory: reserve, commit on first touch */
#define COMMIT_CHUNK 0x10000u
typedef struct { uint8_t *base; uint64_t size; } Region;
static Region regions[8]; static SRWLOCK region_lock = SRWLOCK_INIT; static PVOID veh;
static LONG CALLBACK on_fault(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    uint8_t *a = (uint8_t *)er->ExceptionInformation[1];
    for (int i = 0; i < 8; i++) {
        Region r = regions[i];
        if (!r.base || a < r.base || a >= r.base + r.size) continue;
        uint8_t *p = r.base + (((uint64_t)(a - r.base)) & ~(uint64_t)(COMMIT_CHUNK - 1));
        size_t n = COMMIT_CHUNK; if ((uint64_t)(p - r.base) + n > r.size) n = (size_t)(r.size - (uint64_t)(p - r.base));
        /* another thread may have committed it already: committing again is harmless */
        return VirtualAlloc(p, n, MEM_COMMIT, PAGE_READWRITE) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
void *plat_mem_reserve(uint64_t size) {
    void *p = VirtualAlloc(NULL, (SIZE_T)size, MEM_RESERVE, PAGE_NOACCESS);
    if (!p) return NULL;
    AcquireSRWLockExclusive(&region_lock);
    if (!veh) veh = AddVectoredExceptionHandler(1, on_fault);
    int slot = -1; for (int i = 0; i < 8; i++) if (!regions[i].base) { slot = i; break; }
    if (slot >= 0) { regions[slot].size = size; regions[slot].base = p; }      /* base last: the handler reads without the lock */
    ReleaseSRWLockExclusive(&region_lock);
    if (slot < 0) { VirtualFree(p, 0, MEM_RELEASE); return NULL; }
    return p;
}
void plat_mem_release(void *base, uint64_t size) {
    (void)size;
    AcquireSRWLockExclusive(&region_lock);
    for (int i = 0; i < 8; i++) if (regions[i].base == base) { regions[i].base = NULL; regions[i].size = 0; }
    ReleaseSRWLockExclusive(&region_lock);
    VirtualFree(base, 0, MEM_RELEASE);
}
void plat_mem_discard(void *addr, uint64_t size) {
    /* whole chunks are decommitted (they fault back in as zero); the partial ends are cleared */
    uint8_t *a = addr, *e = a + size, *lo = (uint8_t *)(((uintptr_t)a + COMMIT_CHUNK - 1) & ~(uintptr_t)(COMMIT_CHUNK - 1)), *hi = (uint8_t *)((uintptr_t)e & ~(uintptr_t)(COMMIT_CHUNK - 1));
    if (hi <= lo) { memset(a, 0, (size_t)size); return; }
    if (a < lo) memset(a, 0, (size_t)(lo - a));
    if (e > hi) memset(hi, 0, (size_t)(e - hi));
    VirtualFree(lo, (SIZE_T)(hi - lo), MEM_DECOMMIT);
}

/* ---------------------------------------------------------------- time */
static LARGE_INTEGER qpf;
uint64_t plat_time_ns(void) {
    if (!qpf.QuadPart) QueryPerformanceFrequency(&qpf);
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    uint64_t f = (uint64_t)qpf.QuadPart, t = (uint64_t)c.QuadPart;
    return t / f * 1000000000u + t % f * 1000000000u / f;
}
int64_t plat_wall_time_ns(void) {
    typedef void (WINAPI *Precise)(LPFILETIME); static Precise precise; static int looked;
    if (!looked) { looked = 1; precise = (Precise)(void (*)(void))GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetSystemTimePreciseAsFileTime"); }
    FILETIME ft; if (precise) precise(&ft); else GetSystemTimeAsFileTime(&ft);
    int64_t t = (int64_t)(((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime);
    return (t - 116444736000000000LL) * 100;                          /* 100 ns units since 1601 -> ns since 1970 */
}
int plat_utc_offset_minutes(void) {
    TIME_ZONE_INFORMATION tz; DWORD r = GetTimeZoneInformation(&tz);
    LONG bias = tz.Bias + (r == TIME_ZONE_ID_DAYLIGHT ? tz.DaylightBias : r == TIME_ZONE_ID_STANDARD ? tz.StandardBias : 0);
    return (int)-bias;
}
void plat_sleep_ns(uint64_t ns) {
    /* a high-resolution waitable timer (Windows 10 1803+) sleeps accurately; otherwise Sleep with 1 ms timer resolution */
    static __thread HANDLE timer; static __thread int tried;
    if (!tried) { tried = 1; timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS); }
    if (!ns) { SwitchToThread(); return; }
    if (timer) {
        LARGE_INTEGER due; due.QuadPart = -(LONGLONG)((ns + 99) / 100);
        if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) { WaitForSingleObject(timer, INFINITE); return; }
    }
    static int period; if (!period) { period = 1; typedef UINT (WINAPI *TBP)(UINT); HMODULE w = LoadLibraryW(L"winmm.dll"); TBP tbp = w ? (TBP)(void (*)(void))GetProcAddress(w, "timeBeginPeriod") : NULL; if (tbp) tbp(1); }
    DWORD ms = (DWORD)((ns + 999999) / 1000000); Sleep(ms ? ms : 1);
}

/* ---------------------------------------------------------------- threads */
typedef struct { void (*fn)(void *); void *arg; } Start;
struct PlatMutex { SRWLOCK l; };
struct PlatCond { CONDITION_VARIABLE c; };
static unsigned __stdcall thread_main(void *p) { Start s = *(Start *)p; free(p); s.fn(s.arg); return 0; }
int plat_thread_start(void (*fn)(void *), void *arg, const char *name) {
    Start *s = malloc(sizeof *s); if (!s) return -1; s->fn = fn; s->arg = arg;
    uintptr_t h = _beginthreadex(NULL, 8u << 20, thread_main, s, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!h) { free(s); return -1; }
    typedef HRESULT (WINAPI *SetDesc)(HANDLE, PCWSTR); static SetDesc set_desc; static int looked;
    if (!looked) { looked = 1; set_desc = (SetDesc)(void (*)(void))GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription"); }
    if (set_desc && name) { wchar_t w[64]; MultiByteToWideChar(CP_UTF8, 0, name, -1, w, 64); w[63] = 0; set_desc((HANDLE)h, w); }
    CloseHandle((HANDLE)h);                                         /* detached */
    return 0;
}
void plat_thread_yield(void) { SwitchToThread(); }
PlatMutex *plat_mutex_new(void) { PlatMutex *m = calloc(1, sizeof *m); InitializeSRWLock(&m->l); return m; }
void plat_mutex_free(PlatMutex *m) { free(m); }
void plat_mutex_lock(PlatMutex *m) { AcquireSRWLockExclusive(&m->l); }
void plat_mutex_unlock(PlatMutex *m) { ReleaseSRWLockExclusive(&m->l); }
PlatCond *plat_cond_new(void) { PlatCond *c = calloc(1, sizeof *c); InitializeConditionVariable(&c->c); return c; }
void plat_cond_free(PlatCond *c) { free(c); }
void plat_cond_wait(PlatCond *c, PlatMutex *m) { SleepConditionVariableSRW(&c->c, &m->l, INFINITE, 0); }
int plat_cond_wait_ns(PlatCond *c, PlatMutex *m, uint64_t ns) {
    uint64_t ms = (ns + 999999) / 1000000; if (ms >= INFINITE) ms = INFINITE - 1;
    return !SleepConditionVariableSRW(&c->c, &m->l, (DWORD)ms, 0) && GetLastError() == ERROR_TIMEOUT;
}
void plat_cond_signal(PlatCond *c) { WakeConditionVariable(&c->c); }
void plat_cond_broadcast(PlatCond *c) { WakeAllConditionVariable(&c->c); }

/* ---------------------------------------------------------------- files (paths are UTF-8; '/' and '\' both work) */
struct PlatFile { HANDLE h; int append; };
struct PlatDir { HANDLE h; WIN32_FIND_DATAW fd; int first; char name[MAX_PATH * 3]; };
static wchar_t *wide(const char *path) {                          /* caller frees */
    int n = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0); if (n <= 0) return NULL;
    wchar_t *w = malloc((size_t)n * sizeof *w); if (!w) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, path, -1, w, n);
    for (wchar_t *p = w; *p; p++) if (*p == L'/') *p = L'\\';
    return w;
}
static int err_of(DWORD e) {
    switch (e) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_INVALID_NAME: case ERROR_INVALID_DRIVE: case ERROR_BAD_NETPATH: return PLAT_E_NOENT;
    case ERROR_FILE_EXISTS: case ERROR_ALREADY_EXISTS: return PLAT_E_EXIST;
    case ERROR_ACCESS_DENIED: case ERROR_SHARING_VIOLATION: case ERROR_LOCK_VIOLATION: case ERROR_WRITE_PROTECT: return PLAT_E_ACCESS;
    case ERROR_DIRECTORY: return PLAT_E_NOTDIR;
    case ERROR_DIR_NOT_EMPTY: return PLAT_E_NOTEMPTY;
    default: return PLAT_E_IO;
    }
}
static int64_t ft_ns(FILETIME ft) { int64_t t = (int64_t)(((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime); return t ? (t - 116444736000000000LL) * 100 : 0; }
PlatFile *plat_fs_open(const char *path, int flags, int *err) {
    wchar_t *w = wide(path); if (!w) { if (err) *err = PLAT_E_NOENT; return NULL; }
    DWORD attr = GetFileAttributesW(w);
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) { free(w); if (err) *err = PLAT_E_ISDIR; return NULL; }
    DWORD acc = ((flags & PLAT_READ) || !(flags & PLAT_WRITE) ? GENERIC_READ : 0) | (flags & PLAT_WRITE ? GENERIC_WRITE : 0);
    DWORD disp = (flags & PLAT_CREATE) ? ((flags & PLAT_EXCLUSIVE) ? CREATE_NEW : (flags & PLAT_TRUNCATE) ? CREATE_ALWAYS : OPEN_ALWAYS)
                                       : ((flags & PLAT_TRUNCATE) ? TRUNCATE_EXISTING : OPEN_EXISTING);
    /* share everything: POSIX lets other handles read, write, rename and delete an open file */
    HANDLE h = CreateFileW(w, acc, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, disp, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD e = GetLastError(); free(w);
    if (h == INVALID_HANDLE_VALUE) { if (err) *err = err_of(e); return NULL; }
    PlatFile *f = malloc(sizeof *f); f->h = h; f->append = (flags & PLAT_APPEND) != 0; return f;
}
void plat_fs_close(PlatFile *f) { if (f) { CloseHandle(f->h); free(f); } }
int64_t plat_fs_read(PlatFile *f, void *buf, uint64_t n) {
    uint64_t got = 0;
    while (got < n) {
        DWORD want = n - got > 0x40000000u ? 0x40000000u : (DWORD)(n - got), r = 0;
        if (!ReadFile(f->h, (char *)buf + got, want, &r, NULL)) return got ? (int64_t)got : -1;
        if (!r) break; got += r;
    }
    return (int64_t)got;
}
int64_t plat_fs_write(PlatFile *f, const void *buf, uint64_t n) {
    if (f->append) { LARGE_INTEGER z = { 0 }; SetFilePointerEx(f->h, z, NULL, FILE_END); }
    uint64_t done = 0;
    while (done < n) {
        DWORD want = n - done > 0x40000000u ? 0x40000000u : (DWORD)(n - done), r = 0;
        if (!WriteFile(f->h, (const char *)buf + done, want, &r, NULL)) return done ? (int64_t)done : -1;
        done += r;
    }
    return (int64_t)done;
}
int64_t plat_fs_seek(PlatFile *f, int64_t off, int whence) {
    LARGE_INTEGER d, r; d.QuadPart = off;
    if (!SetFilePointerEx(f->h, d, &r, whence == PLAT_SEEK_SET ? FILE_BEGIN : whence == PLAT_SEEK_CUR ? FILE_CURRENT : FILE_END)) return -1;
    return r.QuadPart;
}
int plat_fs_truncate(PlatFile *f, uint64_t size) {
    LARGE_INTEGER z = { 0 }, cur, to; to.QuadPart = (LONGLONG)size;
    if (!SetFilePointerEx(f->h, z, &cur, FILE_CURRENT) || !SetFilePointerEx(f->h, to, NULL, FILE_BEGIN)) return PLAT_E_IO;
    int ok = SetEndOfFile(f->h); SetFilePointerEx(f->h, cur, NULL, FILE_BEGIN);   /* the position does not move, as with ftruncate */
    return ok ? PLAT_OK : err_of(GetLastError());
}
int plat_fs_flush(PlatFile *f) { (void)f; return PLAT_OK; }
int plat_fs_fstat(PlatFile *f, PlatStat *st) {
    BY_HANDLE_FILE_INFORMATION i; if (!GetFileInformationByHandle(f->h, &i)) return err_of(GetLastError());
    st->size = ((uint64_t)i.nFileSizeHigh << 32) | i.nFileSizeLow; st->is_dir = (i.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    st->readonly = (i.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0;
    st->mtime_ns = ft_ns(i.ftLastWriteTime); st->atime_ns = ft_ns(i.ftLastAccessTime); st->ctime_ns = ft_ns(i.ftCreationTime);
    return PLAT_OK;
}
int plat_fs_stat(const char *path, PlatStat *st) {
    wchar_t *w = wide(path); if (!w) return PLAT_E_NOENT;
    WIN32_FILE_ATTRIBUTE_DATA a; BOOL ok = GetFileAttributesExW(w, GetFileExInfoStandard, &a); DWORD e = GetLastError(); free(w);
    if (!ok) return err_of(e);
    st->size = ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow; st->is_dir = (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    st->readonly = (a.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0;
    st->mtime_ns = ft_ns(a.ftLastWriteTime); st->atime_ns = ft_ns(a.ftLastAccessTime); st->ctime_ns = ft_ns(a.ftCreationTime);
    return PLAT_OK;
}
static int path_op(const char *path, int op) {
    wchar_t *w = wide(path); if (!w) return PLAT_E_NOENT;
    BOOL ok; DWORD attr;
    switch (op) {
    case 0: ok = CreateDirectoryW(w, NULL); break;
    case 1: ok = RemoveDirectoryW(w); break;
    default: attr = GetFileAttributesW(w); if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) { free(w); return PLAT_E_ISDIR; } ok = DeleteFileW(w); break;
    }
    DWORD e = GetLastError(); free(w); return ok ? PLAT_OK : err_of(e);
}
int plat_fs_mkdir(const char *path) { return path_op(path, 0); }
int plat_fs_rmdir(const char *path) { return path_op(path, 1); }
int plat_fs_remove(const char *path) { return path_op(path, 2); }
int plat_fs_rename(const char *from, const char *to) {          /* replaces an existing target, as POSIX rename does */
    wchar_t *a = wide(from), *b = wide(to); BOOL ok = a && b && MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED);
    DWORD e = GetLastError(); free(a); free(b); return ok ? PLAT_OK : err_of(e);
}
PlatDir *plat_fs_opendir(const char *path) {
    size_t n = strlen(path); char *pat = malloc(n + 3); if (!pat) return NULL;
    memcpy(pat, path, n); if (n && pat[n - 1] != '/' && pat[n - 1] != '\\') pat[n++] = '\\'; pat[n++] = '*'; pat[n] = 0;
    wchar_t *w = wide(pat); free(pat); if (!w) return NULL;
    PlatDir *d = calloc(1, sizeof *d); d->h = FindFirstFileW(w, &d->fd); free(w);
    if (d->h == INVALID_HANDLE_VALUE) { free(d); return NULL; }
    d->first = 1; return d;
}
const char *plat_fs_readdir(PlatDir *d) {
    for (;;) {
        if (!d->first && !FindNextFileW(d->h, &d->fd)) return NULL;
        d->first = 0;
        const wchar_t *n = d->fd.cFileName;
        if (n[0] == L'.' && (!n[1] || (n[1] == L'.' && !n[2]))) continue;
        if (WideCharToMultiByte(CP_UTF8, 0, n, -1, d->name, (int)sizeof d->name, NULL, NULL) <= 0) continue;
        return d->name;
    }
}
void plat_fs_closedir(PlatDir *d) { if (d) { FindClose(d->h); free(d); } }
const char *plat_fs_write_dir(void) { const char *w = getenv("SS2PORT_WRITE_DIR"); return w && *w ? w : NULL; }
