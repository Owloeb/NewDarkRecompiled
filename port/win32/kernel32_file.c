/* kernel32_file.c: KERNEL32 files, directories, consoles and .ini files, on plat_fs through the path mapping in vfs.c. */
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include "core.h"

/* ---------------------------------------------------------------- file objects */
typedef struct { KObj k; PlatFile *f; int console; volatile int busy; char host[1024]; } KFile;   /* busy: a read is running without the guest lock */
static void kf_destroy(KObj *o) { KFile *f = (KFile *)o; if (f->f) plat_fs_close(f->f); free(f); }
static int kf_sig(KObj *o, GuestThread *t) { (void)o; (void)t; return 1; }
static uint32_t new_file(PlatFile *pf, int console, const char *host) {
    KFile *f = calloc(1, sizeof *f); f->k = (KObj){ K_FILE, 1, kf_destroy, kf_sig, NULL }; f->f = pf; f->console = console;
    if (host) snprintf(f->host, sizeof f->host, "%s", host);
    return handle_new(&f->k);
}
static KFile *FH(uint32_t h) {            /* waits while another thread is reading the same handle (see ReadFile) */
    KFile *f = (KFile *)handle_get(h, K_FILE);
    while (f && f->busy) { BLOCKING(plat_thread_yield()); f = (KFile *)handle_get(h, K_FILE); }
    return f;
}
/* the console: lines go to the host log */
static char con_buf[2][1024]; static size_t con_n[2];
void console_write(int err, const void *p, uint32_t n) {
    const char *s = p; char *b = con_buf[err]; size_t *k = &con_n[err];
    for (uint32_t i = 0; i < n; i++) {
        if (s[i] == '\n' || *k == sizeof con_buf[0] - 1) { b[*k] = 0; if (*k && b[*k - 1] == '\r') b[*k - 1] = 0; plat_log_write(err ? PLAT_LOG_WARN : PLAT_LOG_INFO, b); *k = 0; if (s[i] == '\n') continue; }
        b[(*k)++] = s[i];
    }
}
static uint32_t std_h[3];
uint32_t std_handle(int which) {      /* 0 in, 1 out, 2 err */
    if (!std_h[which]) std_h[which] = new_file(NULL, which, NULL);
    return std_h[which];
}
static int open_flags(uint32_t access, uint32_t disp) {
    int fl = 0;
    if (access & (0x80000000u | 0x10000000u | 0x1u)) fl |= PLAT_READ;              /* GENERIC_READ, GENERIC_ALL, FILE_READ_DATA */
    if (access & (0x40000000u | 0x10000000u | 0x2u | 0x4u)) fl |= PLAT_WRITE;       /* GENERIC_WRITE, ..., FILE_WRITE_DATA, FILE_APPEND_DATA */
    if (!fl) fl = PLAT_READ;
    switch (disp) { case 1: fl |= PLAT_CREATE | PLAT_EXCLUSIVE; break; case 2: fl |= PLAT_CREATE | PLAT_TRUNCATE; break; case 4: fl |= PLAT_CREATE; break; case 5: fl |= PLAT_TRUNCATE; break; }
    if (fl & (PLAT_CREATE | PLAT_TRUNCATE)) fl |= PLAT_WRITE;
    return fl;
}
SHIM(CreateFileA) {
    const char *name = gs(A(0)); uint32_t disp = A(4); char hp[1200];
    if (!_stricmp_ascii(name, "CONOUT$")) { RET(std_handle(1)); return; }
    char probe[1200]; vfs_map(name, probe, sizeof probe); PlatStat st; int existed = !plat_fs_stat(probe, &st);
    if (existed && st.is_dir) { set_last_error(5); RET(0xFFFFFFFFu); return; }
    int err = 0; PlatFile *f = vfs_open(name, open_flags(A(1), disp), &err, hp, sizeof hp);
    if (!f) { if (err == PLAT_E_NOENT) port_miss("CreateFile", name); set_last_error(err == PLAT_E_NOENT && disp == 3 ? 2 : err == PLAT_E_NOENT ? 3 : (uint32_t)vfs_err_to_win(err)); RET(0xFFFFFFFFu); return; }
    set_last_error(existed && (disp == 2 || disp == 4) ? 183 : 0);
    RET(new_file(f, 0, hp));
}
SHIM(ReadFile) {
    KFile *f = FH(A(0)); if (A(3)) WR32(A(3), 0);
    if (!f || !f->f) { set_last_error(6); RET(0); return; }
    /* the read runs without the guest lock, so other guest threads (the main loop) keep running during slow I/O */
    int64_t n; f->busy = 1; kobj_ref(&f->k); BLOCKING(n = plat_fs_read(f->f, GP(A(1)), A(2))); f->busy = 0; kobj_unref(&f->k);
    if (n < 0) { set_last_error(30); RET(0); return; }
    if (A(3)) WR32(A(3), (uint32_t)n); RET(1);
}
SHIM(WriteFile) {
    KFile *f = FH(A(0)); if (A(3)) WR32(A(3), 0);
    if (!f) { set_last_error(6); RET(0); return; }
    if (!f->f) { console_write(f->console == 2, GP(A(1)), A(2)); if (A(3)) WR32(A(3), A(2)); RET(1); return; }
    int64_t n = plat_fs_write(f->f, GP(A(1)), A(2)); if (n < 0) { set_last_error(29); RET(0); return; }
    if (A(3)) WR32(A(3), (uint32_t)n); RET(1);
}
SHIM(SetFilePointer) {
    KFile *f = FH(A(0)); if (!f || !f->f) { set_last_error(6); RET(0xFFFFFFFFu); return; }
    int64_t off = (int32_t)A(1); if (A(2)) off = (int64_t)(((uint64_t)RD32(A(2)) << 32) | A(1));
    int64_t r = plat_fs_seek(f->f, off, (int)A(3)); if (r < 0) { set_last_error(131); RET(0xFFFFFFFFu); return; }
    if (A(2)) WR32(A(2), (uint32_t)((uint64_t)r >> 32)); set_last_error(0); RET((uint32_t)r);
}
SHIM(GetFileSize) { KFile *f = FH(A(0)); PlatStat st; if (!f || !f->f || plat_fs_fstat(f->f, &st)) { RET(0xFFFFFFFFu); return; } if (A(1)) WR32(A(1), (uint32_t)(st.size >> 32)); RET((uint32_t)st.size); }
SHIM(SetEndOfFile) { KFile *f = FH(A(0)); if (!f || !f->f) { RET(0); return; } int64_t pos = plat_fs_seek(f->f, 0, PLAT_SEEK_CUR); RET(pos >= 0 && !plat_fs_truncate(f->f, (uint64_t)pos)); }
SHIM(FlushFileBuffers) { KFile *f = FH(A(0)); if (f && f->f) plat_fs_flush(f->f); RET(1); }
SHIM(GetFileType) { KFile *f = FH(A(0)); RET(!f ? 0 : f->f ? 1 : 2); }
SHIM(GetFileTime) {
    KFile *f = FH(A(0)); PlatStat st; if (!f || !f->f || plat_fs_fstat(f->f, &st)) { RET(0); return; }
    if (A(1)) WR64(A(1), unix_ns_to_filetime(st.ctime_ns)); if (A(2)) WR64(A(2), unix_ns_to_filetime(st.atime_ns)); if (A(3)) WR64(A(3), unix_ns_to_filetime(st.mtime_ns)); RET(1);
}
SHIM(SetFileTime) { RET(1); }
SHIM(GetFileAttributesA) {
    char hp[1200]; PlatStat st; vfs_map(gs(A(0)), hp, sizeof hp);
    if (plat_fs_stat(hp, &st)) { port_miss("GetFileAttributes", gs(A(0))); set_last_error(2); RET(0xFFFFFFFFu); return; }
    RET(st.is_dir ? 0x10 : (st.readonly ? 0x01 : 0) | 0x20);          /* DIRECTORY, or ARCHIVE (+READONLY) */
}
SHIM(SetFileAttributesA) { RET(1); }
SHIM(DeleteFileA) { char hp[1200]; vfs_map_write(gs(A(0)), hp, sizeof hp); int e = plat_fs_remove(hp); if (e) { set_last_error((uint32_t)vfs_err_to_win(e)); RET(0); return; } vfs_changed(hp); RET(1); }
SHIM(MoveFileA) {
    char a[1200], b[1200]; vfs_map(gs(A(0)), a, sizeof a); vfs_map_write(gs(A(1)), b, sizeof b); PlatStat st;
    if (!plat_fs_stat(b, &st)) { set_last_error(183); RET(0); return; }
    int e = plat_fs_rename(a, b); if (e) { set_last_error((uint32_t)vfs_err_to_win(e)); RET(0); return; } vfs_changed(a); vfs_changed(b); RET(1);
}
SHIM(CopyFileA) {
    char a[1200], b[1200]; vfs_map(gs(A(0)), a, sizeof a); vfs_map_write(gs(A(1)), b, sizeof b); PlatStat st;
    if (plat_fs_stat(a, &st)) { set_last_error(2); RET(0); return; }
    if (A(2) && !plat_fs_stat(b, &st)) { set_last_error(80); RET(0); return; }
    RET(vfs_copy(a, b));
}
SHIM(CreateDirectoryA) { char hp[1200]; vfs_map_write(gs(A(0)), hp, sizeof hp); int e = plat_fs_mkdir(hp); if (e) { set_last_error(e == PLAT_E_EXIST ? 183 : (uint32_t)vfs_err_to_win(e)); RET(0); return; } vfs_changed(hp); RET(1); }
SHIM(RemoveDirectoryA) { char hp[1200]; vfs_map_write(gs(A(0)), hp, sizeof hp); int e = plat_fs_rmdir(hp); if (e) { set_last_error((uint32_t)vfs_err_to_win(e)); RET(0); return; } vfs_changed(hp); RET(1); }
static uint32_t put_str(uint32_t dst, uint32_t cap, const char *s) {        /* the A-function convention for "copy into a buffer" */
    uint32_t l = (uint32_t)strlen(s); if (l + 1 > cap) return l + 1;
    memcpy(GP(dst), s, l + 1); return l;
}
SHIM(GetCurrentDirectoryA) { char b[600]; snprintf(b, sizeof b, "C:\\%s", vfs_cwd()); for (char *p = b; *p; p++) if (*p == '/') *p = '\\'; RET(put_str(A(1), A(0), b)); }
SHIM(SetCurrentDirectoryA) { if (!vfs_chdir(gs(A(0)))) { set_last_error(3); RET(0); return; } RET(1); }
SHIM(GetFullPathNameA) {
    char b[1100]; vfs_full_win(gs(A(0)), b, sizeof b); uint32_t r = put_str(A(2), A(1), b);
    if (A(3) && r < A(1)) { const char *sl = strrchr(b, '\\'); WR32(A(3), A(2) + (uint32_t)(sl ? sl + 1 - b : 0)); }
    RET(r);
}
SHIM(GetShortPathNameA) { RET(put_str(A(1), A(2), gs(A(0)))); }
SHIM(GetLongPathNameA) { RET(put_str(A(1), A(2), gs(A(0)))); }
SHIM(GetTempPathA) { RET(put_str(A(1), A(0), "C:\\temp\\")); }
SHIM(GetWindowsDirectoryA) { RET(put_str(A(0), A(1), "C:\\WINDOWS")); }
SHIM(GetSystemDirectoryA) { RET(put_str(A(0), A(1), "C:\\WINDOWS\\system32")); }
SHIM(GetDriveTypeA) { RET(3); }                                            /* DRIVE_FIXED */
SHIM(GetLogicalDrives) { RET(4); }                                         /* C: */
SHIM(GetVolumeInformationA) { if (A(1) && A(2)) WR8(A(1), 0); if (A(3)) WR32(A(3), 0x12345678u); if (A(4)) WR32(A(4), 255); if (A(5)) WR32(A(5), 0x3); if (A(6) && A(7)) put_str(A(6), A(7), "NTFS"); RET(1); }
SHIM(GetDiskFreeSpaceExA) { if (A(1)) WR64(A(1), 8ull << 30); if (A(2)) WR64(A(2), 64ull << 30); if (A(3)) WR64(A(3), 8ull << 30); RET(1); }
SHIM(GetDiskFreeSpaceA) { if (A(1)) WR32(A(1), 8); if (A(2)) WR32(A(2), 512); if (A(3)) WR32(A(3), 2000000); if (A(4)) WR32(A(4), 16000000); RET(1); }

/* ---------------------------------------------------------------- FindFirstFile */
typedef struct { KObj k; PlatDir *d; char dir[1024], pat[300]; int dots; } KFind;
static void kfind_destroy(KObj *o) { KFind *f = (KFind *)o; if (f->d) plat_fs_closedir(f->d); free(f); }
static int find_next(KFind *h, uint32_t out) {
    for (;;) {
        const char *e; char nm[300];
        if (h->dots < 2) { e = h->dots++ ? ".." : "."; } else if (!(e = plat_fs_readdir(h->d))) return 0;
        if (!win_glob(h->pat, e)) continue;
        snprintf(nm, sizeof nm, "%s", e);
        char full[1400]; PlatStat st; snprintf(full, sizeof full, "%s/%s", h->dir, e);
        if (h->dots && strcmp(e, ".") && strcmp(e, "..") && plat_fs_stat(full, &st)) continue;
        if (!strcmp(e, ".") || !strcmp(e, "..")) { memset(&st, 0, sizeof st); st.is_dir = 1; }
        memset(GP(out), 0, 320);
        WR32(out, st.is_dir ? 0x10 : 0x20); uint64_t ft = unix_ns_to_filetime(st.mtime_ns);
        WR64(out + 4, unix_ns_to_filetime(st.ctime_ns)); WR64(out + 12, unix_ns_to_filetime(st.atime_ns)); WR64(out + 20, ft);
        WR32(out + 28, (uint32_t)(st.size >> 32)); WR32(out + 32, (uint32_t)st.size);
        snprintf((char *)GP(out + 44), 260, "%s", nm);
        return 1;
    }
}
SHIM(FindFirstFileA) {
    const char *w = gs(A(0)); char wdir[1024]; snprintf(wdir, sizeof wdir, "%s", w);
    char *sl = strrchr(wdir, '\\'), *s2 = strrchr(wdir, '/'); if (s2 > sl) sl = s2;
    const char *pat = sl ? sl + 1 : wdir; char patc[300]; snprintf(patc, sizeof patc, "%s", pat);
    if (sl) sl[1] = 0; else wdir[0] = 0;
    KFind *f = calloc(1, sizeof *f); f->k = (KObj){ K_FIND, 1, kfind_destroy, NULL, NULL };
    vfs_map(wdir[0] ? wdir : ".", f->dir, sizeof f->dir); snprintf(f->pat, sizeof f->pat, "%s", patc);
    f->dots = strpbrk(patc, "*?") ? 0 : 2;                  /* "." and ".." only for wildcard searches */
    f->d = plat_fs_opendir(f->dir);
    if (!f->d) { free(f); port_miss("FindFirstFile", w); set_last_error(3); RET(0xFFFFFFFFu); return; }
    if (!find_next(f, A(1))) { kfind_destroy(&f->k); set_last_error(2); RET(0xFFFFFFFFu); return; }
    RET(handle_new(&f->k));
}
SHIM(FindNextFileA) { KFind *f = (KFind *)handle_get(A(0), K_FIND); if (!f) { set_last_error(6); RET(0); return; } if (!find_next(f, A(1))) { set_last_error(18); RET(0); return; } RET(1); }
SHIM(FindClose) { RET(handle_close(A(0))); }

/* ---------------------------------------------------------------- consoles */
SHIM(GetStdHandle) { int32_t w = (int32_t)A(0); RET(w == -10 ? std_handle(0) : w == -11 ? std_handle(1) : w == -12 ? std_handle(2) : 0xFFFFFFFFu); }
SHIM(SetStdHandle) { RET(1); }
SHIM(WriteConsoleA) { console_write(0, GP(A(1)), A(2)); if (A(3)) WR32(A(3), A(2)); RET(1); }
SHIM(WriteConsoleW) { char b[512]; uint32_t n = A(2) < 511 ? A(2) : 511; for (uint32_t i = 0; i < n; i++) { uint16_t w = RD16(A(1) + 2 * i); b[i] = w < 128 ? (char)w : '?'; } console_write(0, b, n); if (A(3)) WR32(A(3), A(2)); RET(1); }
SHIM(ConsoleOk) { RET(1); }
SHIM(GetConsoleScreenBufferInfo) { memset(GP(A(1)), 0, 22); WR16(A(1), 80); WR16(A(1) + 2, 25); RET(1); }
SHIM(GetConsoleMode) { if (A(1)) WR32(A(1), 3); RET(1); }
SHIM(GetConsoleCP) { RET(437); }

/* ---------------------------------------------------------------- private profile (.ini) files */
static char *read_text(const char *win, size_t *n) {
    int err; PlatFile *f = vfs_open(win, PLAT_READ, &err, NULL, 0); if (!f) return NULL;
    PlatStat st; plat_fs_fstat(f, &st); char *b = malloc((size_t)st.size + 1); int64_t r = plat_fs_read(f, b, st.size); plat_fs_close(f);
    if (r < 0) r = 0; b[r] = 0; *n = (size_t)r; return b;
}
static const char *ini_name(uint32_t g) { const char *n = gs(g); return n[0] ? n : "win.ini"; }
static int ieqn(const char *a, const char *b, size_t n) { for (size_t i = 0; i < n; i++) if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return 0; return 1; }
/* finds key in section; returns 1 and the value (trimmed) */
static int ini_get(const char *file, const char *sec, const char *key, char *out, size_t on) {
    size_t n; char *t = read_text(file, &n); if (!t) return 0;
    int in = 0, found = 0; size_t sl = strlen(sec), kl = strlen(key);
    for (char *line = t; line && *line && !found;) {
        char *nl = strchr(line, '\n'); if (nl) *nl = 0;
        char *s = line; while (*s == ' ' || *s == '\t') s++;
        size_t l = strlen(s); while (l && (s[l - 1] == '\r' || s[l - 1] == ' ' || s[l - 1] == '\t')) s[--l] = 0;
        if (*s == '[') { char *e = strchr(s, ']'); in = e && (size_t)(e - s - 1) == sl && ieqn(s + 1, sec, sl); }
        else if (in && *s && *s != ';') {
            char *eq = strchr(s, '='); if (eq) { char *ke = eq; while (ke > s && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
                if ((size_t)(ke - s) == kl && ieqn(s, key, kl)) { char *v = eq + 1; while (*v == ' ' || *v == '\t') v++;
                    size_t vl = strlen(v); if (vl >= 2 && (v[0] == '"' || v[0] == '\'') && v[vl - 1] == v[0]) { v[vl - 1] = 0; v++; }
                    snprintf(out, on, "%s", v); found = 1; } }
        }
        line = nl ? nl + 1 : NULL;
    }
    free(t); return found;
}
SHIM(GetPrivateProfileIntA) { char v[256]; RET(ini_get(ini_name(A(3)), gs(A(0)), gs(A(1)), v, sizeof v) ? (uint32_t)strtol(v, NULL, 10) : A(2)); }
SHIM(GetPrivateProfileStringA) {
    uint32_t cap = A(4); if (!cap) { RET(0); return; }
    char v[2048]; const char *r = A(0) && A(1) && ini_get(ini_name(A(5)), gs(A(0)), gs(A(1)), v, sizeof v) ? v : gs(A(2));
    uint32_t l = (uint32_t)strlen(r); if (l >= cap) l = cap - 1; memcpy(GP(A(3)), r, l); WR8(A(3) + l, 0); RET(l);
}
SHIM(GetProfileIntA) { RET(A(2)); }
SHIM(WritePrivateProfileStringA) {
    const char *file = ini_name(A(3)), *sec = gs(A(0)), *key = gs(A(1)); int del = !A(2);
    size_t n = 0; char *t = read_text(file, &n); if (!t) { t = calloc(1, 1); n = 0; }
    size_t cap = n + strlen(sec) + strlen(key) + strlen(gs(A(2))) + 64; char *o = malloc(cap); size_t k = 0;
    int in = 0, done = 0, seen_sec = 0; size_t sl = strlen(sec), kl = strlen(key);
    for (char *line = t; line && *line;) {
        char *nl = strchr(line, '\n'); size_t ll = nl ? (size_t)(nl - line) + 1 : strlen(line);
        char *s = line; while (*s == ' ' || *s == '\t') s++;
        if (*s == '[') { if (in && !done && A(1) && !del) { k += (size_t)snprintf(o + k, cap - k, "%s=%s\r\n", key, gs(A(2))); done = 1; } char *e = strchr(s, ']'); in = e && (size_t)(e - s - 1) == sl && ieqn(s + 1, sec, sl); seen_sec |= in; }
        else if (in && A(1)) { char *eq = strchr(s, '='); char *ke = eq; while (ke && ke > s && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
            if (eq && (eq < (nl ? nl : s + strlen(s))) && (size_t)(ke - s) == kl && ieqn(s, key, kl)) { if (!del && !done) k += (size_t)snprintf(o + k, cap - k, "%s=%s\r\n", key, gs(A(2))); done = 1; line += ll; continue; } }
        if (!(in && !A(1))) { memcpy(o + k, line, ll); k += ll; }
        line += ll;
    }
    if (!done && A(1) && !del) { if (!seen_sec) k += (size_t)snprintf(o + k, cap - k, "%s[%s]\r\n", k && o[k - 1] != '\n' ? "\r\n" : "", sec); else if (k && o[k - 1] != '\n') k += (size_t)snprintf(o + k, cap - k, "\r\n"); k += (size_t)snprintf(o + k, cap - k, "%s=%s\r\n", key, gs(A(2))); }
    int err; PlatFile *f = vfs_open(file, PLAT_WRITE | PLAT_CREATE | PLAT_TRUNCATE, &err, NULL, 0);
    if (f) { plat_fs_write(f, o, k); plat_fs_close(f); }
    free(t); free(o); RET(f != NULL);
}

const ShimDef kernel32_file_shims[] = {
    S(CreateFileA, STD(7)), S(ReadFile, STD(5)), S(WriteFile, STD(5)), S(SetFilePointer, STD(4)), S(GetFileSize, STD(2)), S(SetEndOfFile, STD(1)), S(FlushFileBuffers, STD(1)),
    S(GetFileType, STD(1)), S(GetFileTime, STD(4)), S(SetFileTime, STD(4)), S(GetFileAttributesA, STD(1)), S(SetFileAttributesA, STD(2)), S(DeleteFileA, STD(1)), S(MoveFileA, STD(2)),
    S(CopyFileA, STD(3)), S(CreateDirectoryA, STD(2)), S(RemoveDirectoryA, STD(1)), S(GetCurrentDirectoryA, STD(2)), S(SetCurrentDirectoryA, STD(1)), S(GetFullPathNameA, STD(4)),
    S(GetShortPathNameA, STD(3)), S(GetLongPathNameA, STD(3)), S(GetTempPathA, STD(2)), S(GetWindowsDirectoryA, STD(2)), S(GetSystemDirectoryA, STD(2)), S(GetDriveTypeA, STD(1)), S(GetLogicalDrives, 0),
    S(GetVolumeInformationA, STD(8)), S(GetDiskFreeSpaceExA, STD(4)), S(GetDiskFreeSpaceA, STD(5)),
    S(FindFirstFileA, STD(2)), S(FindNextFileA, STD(2)), S(FindClose, STD(1)),
    S(GetStdHandle, STD(1)), S(SetStdHandle, STD(2)), S(WriteConsoleA, STD(5)), S(WriteConsoleW, STD(5)), SA("SetConsoleCursorPosition", ConsoleOk, STD(2)), SA("SetConsoleTextAttribute", ConsoleOk, STD(2)),
    S(GetConsoleScreenBufferInfo, STD(2)), S(GetConsoleMode, STD(2)), S(GetConsoleCP, 0), SA("GetConsoleOutputCP", GetConsoleCP, 0), SA("SetConsoleCtrlHandler", ConsoleOk, STD(2)),
    S(GetPrivateProfileIntA, STD(4)), S(GetPrivateProfileStringA, STD(6)), S(GetProfileIntA, STD(3)), S(WritePrivateProfileStringA, STD(4)),
    { 0, 0, 0 }
};
