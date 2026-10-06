/* win32b.c: the Windows API the recompiled DLLs add (they link parts of the C runtime statically, so they call the
 * locale/TLS/file-handle functions that the exe gets from MSVCR90). Same rules as win32.c. */
#define _GNU_SOURCE
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include "port.h"

extern const uint32_t hd_base;
extern void set_last_error(uint32_t e);
static int fd_of(uint32_t h) { return h >= 0x2000 && h < 0x2000 + 4 * 1024 ? (int)((h - 0x2000) / 4) : (h >= 0x10 && h <= 0x12 ? (int)(h - 0x10) : -1); }

/* files by handle (handles from CreateFileA are 0x2000 + 4*fd; the standard handles are 0x10..0x12) */
SHIM(ReadFile) { int fd = fd_of(A(0)); if (fd < 0) { RET(0); return; } ssize_t n = read(fd, GP(A(1)), A(2)); if (n < 0) { set_last_error(5); RET(0); return; } if (A(3)) WR32(A(3), (uint32_t)n); RET(1); }
SHIM(WriteFile) { int fd = fd_of(A(0)); if (fd < 0) { RET(0); return; } ssize_t n = write(fd, GP(A(1)), A(2)); if (n < 0) { set_last_error(5); RET(0); return; } if (A(3)) WR32(A(3), (uint32_t)n); RET(1); }
SHIM(SetFilePointer) {
    int fd = fd_of(A(0)); if (fd < 0) { RET(0xFFFFFFFFu); return; } int64_t off = (int32_t)A(1); if (A(2)) off |= (int64_t)(int32_t)RD32(A(2)) << 32;
    off_t r = lseek(fd, off, (int)A(3)); if (r < 0) { RET(0xFFFFFFFFu); return; } if (A(2)) WR32(A(2), (uint32_t)((uint64_t)r >> 32)); RET((uint32_t)r);
}
SHIM(SetEndOfFile) { int fd = fd_of(A(0)); RET(fd >= 0 && !ftruncate(fd, lseek(fd, 0, SEEK_CUR))); }
SHIM(FlushFileBuffers) { RET(1); }
SHIM(GetFileType) { uint32_t h = A(0); RET(h >= 0x10 && h <= 0x12 ? 2 : 1); }
SHIM(SetHandleCount) { RET(A(0)); }
SHIM(SetStdHandle) { RET(1); }
SHIM(GetFileAttributesA) { char hp[1400]; struct stat st; host_path(gs(A(0)), hp, sizeof hp); if (stat(hp, &st)) { set_last_error(2); RET(0xFFFFFFFFu); return; } RET(S_ISDIR(st.st_mode) ? 0x10 : 0x80); }
SHIM(DeleteFileA) { char hp[1400]; host_path(gs(A(0)), hp, sizeof hp); RET(unlink(hp) == 0); }
SHIM(MoveFileA) { char a[1400], b[1400]; host_path(gs(A(0)), a, sizeof a); host_path(gs(A(1)), b, sizeof b); RET(rename(a, b) == 0); }
SHIM(GetCurrentDirectoryA) { const char *w = "C:\\"; if (A(0) < 4) { RET(4); return; } memcpy(GP(A(1)), w, 4); RET(3); }
SHIM(SetCurrentDirectoryA) { char hp[1400]; host_path(gs(A(0)), hp, sizeof hp); RET(chdir(hp) == 0); }
SHIM(GetFullPathNameA) { const char *s = gs(A(0)); uint32_t l = (uint32_t)strlen(s); if (l + 1 > A(1)) { RET(l + 1); return; } memcpy(GP(A(2)), s, l + 1); if (A(3)) { const char *b = strrchr(s, '\\'); WR32(A(3), A(2) + (b ? (uint32_t)(b + 1 - s) : 0)); } RET(l); }

/* FindFirstFile / FindNextFile (WIN32_FIND_DATAA) */
typedef struct { DIR *d; char pat[300], dir[1400]; int used; } Fh;
static Fh fhs[16];
static int ff_fill(Fh *h, uint32_t out) {
    struct dirent *e;
    while ((e = readdir(h->d))) {
        if (fnmatch(h->pat, e->d_name, FNM_CASEFOLD)) continue;
        char full[1900]; struct stat st; snprintf(full, sizeof full, "%s/%s", h->dir, e->d_name); if (stat(full, &st)) continue;
        memset(GP(out), 0, 320); WR32(out, S_ISDIR(st.st_mode) ? 0x10 : 0x80); WR32(out + 28, (uint32_t)((uint64_t)st.st_size >> 32)); WR32(out + 32, (uint32_t)st.st_size);
        uint64_t ft = ((uint64_t)st.st_mtime + 11644473600ull) * 10000000ull; WR64(out + 4, ft); WR64(out + 12, ft); WR64(out + 20, ft);
        snprintf((char *)GP(out + 44), 260, "%s", e->d_name); return 1;
    }
    return 0;
}
SHIM(FindFirstFileA) {
    char hp[1400]; host_path(gs(A(0)), hp, sizeof hp); char *sl = strrchr(hp, '/'); char dir[1400]; const char *pat;
    if (sl) { *sl = 0; snprintf(dir, sizeof dir, "%s", hp[0] ? hp : "/"); pat = sl + 1; } else { snprintf(dir, sizeof dir, "."); pat = hp; }
    int i; for (i = 0; i < 16 && fhs[i].used; i++) ; if (i == 16) { RET(0xFFFFFFFFu); return; }
    DIR *d = opendir(dir); if (!d) { set_last_error(3); RET(0xFFFFFFFFu); return; }
    fhs[i].d = d; fhs[i].used = 1; snprintf(fhs[i].pat, sizeof fhs[i].pat, "%s", pat); snprintf(fhs[i].dir, sizeof fhs[i].dir, "%s", dir);
    if (!ff_fill(&fhs[i], A(1))) { closedir(d); fhs[i].used = 0; set_last_error(2); RET(0xFFFFFFFFu); return; } RET(0x6000 + (uint32_t)i);
}
SHIM(FindNextFileA) { int i = (int)A(0) - 0x6000; if (i < 0 || i >= 16 || !fhs[i].used) { RET(0); return; } RET(ff_fill(&fhs[i], A(1))); }
SHIM(FindClose) { int i = (int)A(0) - 0x6000; if (i >= 0 && i < 16 && fhs[i].used) { closedir(fhs[i].d); fhs[i].used = 0; } RET(1); }

/* thread-local storage (one thread) */
static uint32_t tls[64]; static int tls_used[64];
SHIM(TlsAlloc) { for (int i = 0; i < 64; i++) if (!tls_used[i]) { tls_used[i] = 1; tls[i] = 0; RET(i); return; } RET(0xFFFFFFFFu); }
SHIM(TlsFree) { if (A(0) < 64) tls_used[A(0)] = 0; RET(1); }
SHIM(TlsGetValue) { RET(A(0) < 64 ? tls[A(0)] : 0); }
SHIM(TlsSetValue) { if (A(0) < 64) tls[A(0)] = A(1); RET(1); }

/* environment, code pages, locale (the statically linked CRT probes these at start-up; "ASCII, US" answers satisfy it) */
SHIM(GetEnvironmentStrings) { static uint32_t e; if (!e) e = g_alloc(8); RET(e); }
SHIM(FreeEnvironmentStrings) { RET(1); }
SHIM(SetEnvironmentVariableA) { RET(1); }
SHIM(GetACP) { RET(1252); }
SHIM(GetOEMCP) { RET(437); }
SHIM(GetCPInfo) { uint32_t p = A(1); memset(GP(p), 0, 20); WR32(p, 1); WR8(p + 4, '?'); RET(1); }
SHIM(IsValidCodePage) { RET(1); }
SHIM(IsValidLocale) { RET(1); }
SHIM(EnumSystemLocalesA) { RET(1); }
SHIM(GetUserDefaultLCID) { RET(0x409); }
SHIM(GetLocaleInfoA) { if (A(3) && A(2)) WR8(A(2), 0); RET(0); }
SHIM(GetLocaleInfoW) { RET(0); }
SHIM(GetDateFormatA) { RET(0); }
SHIM(GetTimeFormatA) { RET(0); }
SHIM(GetTimeZoneInformation) { memset(GP(A(0)), 0, 172); RET(0); }
SHIM(GetConsoleCP) { RET(437); }
SHIM(GetConsoleOutputCP) { RET(437); }
SHIM(GetConsoleMode) { RET(0); }
SHIM(SetConsoleCtrlHandler) { RET(1); }
SHIM(WriteConsoleW) { if (A(3)) WR32(A(3), A(2)); RET(1); }
SHIM(CompareStringA) {
    const char *a = gs(A(2)), *b = gs(A(4)); int la = (int)A(3), lb = (int)A(5); if (la < 0) la = (int)strlen(a); if (lb < 0) lb = (int)strlen(b);
    int n = la < lb ? la : lb, r = (A(1) & 1) ? strncasecmp(a, b, (size_t)n) : strncmp(a, b, (size_t)n); if (!r) r = la - lb; RET(r < 0 ? 1 : r > 0 ? 3 : 2);
}
SHIM(CompareStringW) {
    uint32_t a = A(2), b = A(4); int la = (int)A(3), lb = (int)A(5); if (la < 0) { la = 0; while (RD16(a + 2 * (uint32_t)la)) la++; } if (lb < 0) { lb = 0; while (RD16(b + 2 * (uint32_t)lb)) lb++; }
    int r = 0, i; for (i = 0; i < la && i < lb && !r; i++) { int x = RD16(a + 2 * (uint32_t)i), y = RD16(b + 2 * (uint32_t)i); if (A(1) & 1) { x = x < 128 ? tolower(x) : x; y = y < 128 ? tolower(y) : y; } r = x - y; }
    if (!r) r = la - lb; RET(r < 0 ? 1 : r > 0 ? 3 : 2);
}
static uint16_t ctype1(int ch) { uint16_t t = 0; if (ch >= 128) return 0; if (isupper(ch)) t |= 0x1 | 0x100; if (islower(ch)) t |= 0x2 | 0x100; if (isdigit(ch)) t |= 0x4; if (isspace(ch)) t |= 0x8; if (ispunct(ch)) t |= 0x10; if (iscntrl(ch)) t |= 0x20; if (ch == ' ' || ch == '\t') t |= 0x40; if (isxdigit(ch)) t |= 0x80; return t; }
SHIM(GetStringTypeA) { int n = (int)A(3); const char *s = gs(A(2)); if (n < 0) n = (int)strlen(s) + 1; for (int i = 0; i < n; i++) WR16(A(4) + 2 * (uint32_t)i, ctype1((unsigned char)s[i])); RET(1); }
SHIM(GetStringTypeW) { int n = (int)A(2); uint32_t s = A(1); if (n < 0) { n = 0; while (RD16(s + 2 * (uint32_t)n)) n++; n++; } for (int i = 0; i < n; i++) WR16(A(3) + 2 * (uint32_t)i, ctype1(RD16(s + 2 * (uint32_t)i))); RET(1); }
SHIM(LCMapStringA) {
    const char *s = gs(A(2)); int n = (int)A(3); if (n < 0) n = (int)strlen(s) + 1; uint32_t cap = A(5), f = A(1); if (!cap) { RET((uint32_t)n); return; } if ((uint32_t)n > cap) { RET(0); return; }
    for (int i = 0; i < n; i++) { int ch = (unsigned char)s[i]; if (f & 0x100) ch = tolower(ch); else if (f & 0x200) ch = toupper(ch); WR8(A(4) + (uint32_t)i, (uint8_t)ch); } RET((uint32_t)n);
}
SHIM(LCMapStringW) {
    uint32_t s = A(2); int n = (int)A(3); if (n < 0) { n = 0; while (RD16(s + 2 * (uint32_t)n)) n++; n++; } uint32_t cap = A(5), f = A(1); if (!cap) { RET((uint32_t)n); return; } if ((uint32_t)n > cap) { RET(0); return; }
    for (int i = 0; i < n; i++) { int ch = RD16(s + 2 * (uint32_t)i); if (ch < 128) { if (f & 0x100) ch = tolower(ch); else if (f & 0x200) ch = toupper(ch); } WR16(A(4) + 2 * (uint32_t)i, (uint16_t)ch); } RET((uint32_t)n);
}
SHIM(GetModuleHandleW) { RET(A(0) ? 0 : hd_base); }

/* misc */
SHIM(SetLastError) { set_last_error(A(0)); }
SHIM(Ret1) { RET(1); }
SHIM(Ret0) { RET(0); }
SHIM(HeapDestroy) { RET(1); }
SHIM(RaiseException) { port_die("RaiseException(%08x)", A(0)); }
SHIM(RtlUnwind) { port_die("RtlUnwind: structured exception unwinding is not supported by this host yet"); }
SHIM(FatalAppExitA) { port_die("FatalAppExit: %s", gs(A(1))); }
SHIM(CreateSemaphoreA) { static uint32_t h = 0x800; RET(h += 4); }
SHIM(purecall) { port_die("pure virtual function called"); }
SHIM(malloc_crt) { RET(g_alloc(A(0) ? A(0) : 1)); }
SHIM(encoded_null) { RET(0); }
SHIM(vsprintf_) { extern int port_format(char **, const char *, uint32_t); char *s; int n = port_format(&s, gs(A(1)), A(2)); memcpy(GP(A(0)), s, (size_t)n + 1); free(s); RET((uint32_t)n); }
SHIM(cppxcpt) { RET(0); }

#define S(n, p) { #n, sh_##n, p }
#define SA(name, impl, p) { name, sh_##impl, p }
const ShimDef win32b_shims[] = {
    S(ReadFile, STD(5)), S(WriteFile, STD(5)), S(SetFilePointer, STD(4)), S(SetEndOfFile, STD(1)), S(FlushFileBuffers, STD(1)), S(GetFileType, STD(1)), S(SetHandleCount, STD(1)), S(SetStdHandle, STD(2)),
    S(GetFileAttributesA, STD(1)), S(DeleteFileA, STD(1)), S(MoveFileA, STD(2)), S(GetCurrentDirectoryA, STD(2)), S(SetCurrentDirectoryA, STD(1)), S(GetFullPathNameA, STD(4)),
    S(FindFirstFileA, STD(2)), S(FindNextFileA, STD(2)), S(FindClose, STD(1)),
    S(TlsAlloc, 0), S(TlsFree, STD(1)), S(TlsGetValue, STD(1)), S(TlsSetValue, STD(2)),
    S(GetEnvironmentStrings, 0), SA("GetEnvironmentStringsW", GetEnvironmentStrings, 0), SA("FreeEnvironmentStringsA", FreeEnvironmentStrings, STD(1)), SA("FreeEnvironmentStringsW", FreeEnvironmentStrings, STD(1)),
    S(SetEnvironmentVariableA, STD(2)), S(GetACP, 0), S(GetOEMCP, 0), S(GetCPInfo, STD(2)), S(IsValidCodePage, STD(1)), S(IsValidLocale, STD(2)), S(EnumSystemLocalesA, STD(2)), S(GetUserDefaultLCID, 0),
    S(GetLocaleInfoA, STD(4)), S(GetLocaleInfoW, STD(4)), S(GetDateFormatA, STD(6)), S(GetTimeFormatA, STD(6)), S(GetTimeZoneInformation, STD(1)),
    S(GetConsoleCP, 0), S(GetConsoleOutputCP, 0), S(GetConsoleMode, STD(2)), S(SetConsoleCtrlHandler, STD(2)), S(WriteConsoleW, STD(5)),
    S(CompareStringA, STD(6)), S(CompareStringW, STD(6)), S(GetStringTypeA, STD(5)), S(GetStringTypeW, STD(4)), S(LCMapStringA, STD(6)), S(LCMapStringW, STD(6)), S(GetModuleHandleW, STD(1)),
    S(SetLastError, STD(1)), S(HeapDestroy, STD(1)), S(RaiseException, STD(4)), S(RtlUnwind, STD(4)), S(FatalAppExitA, STD(2)),
    SA("InitializeCriticalSectionAndSpinCount", Ret1, STD(2)), SA("DisableThreadLibraryCalls", Ret1, STD(1)), S(CreateSemaphoreA, STD(4)), SA("ReleaseSemaphore", Ret1, STD(3)),
    SA("_purecall", purecall, CDECL), SA("_malloc_crt", malloc_crt, CDECL), SA("_encoded_null", encoded_null, CDECL), SA("vsprintf", vsprintf_, CDECL), SA("__CppXcptFilter", cppxcpt, CDECL),
    SA("__clean_type_info_names_internal", Ret0, CDECL),
    { 0, 0, 0 }
};
