/* stdio.c: MSVCR90 streams (FILE *), low-level descriptors (_open/_read), directories and file information.
 *
 * A guest FILE is a 32-byte MSVC _iobuf in guest memory whose _file field (offset 16) indexes a host stream table.
 * Streams are buffered and implement Windows text mode exactly: CR LF reads back as LF, Ctrl-Z ends the file, and
 * LF is written as CR LF, so files the game writes are byte-identical to the ones it writes on Windows. */
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include "crt.h"

void console_write(int err, const void *p, uint32_t n);    /* win32/kernel32_file.c */
char *vfs_map_write(const char *win, char *out, size_t n);
int vfs_chdir(const char *win); const char *vfs_cwd(void); char *vfs_full_win(const char *win, char *out, size_t n);
PlatFile *vfs_open(const char *win, int flags, int *err, char *host, size_t hn);
void vfs_changed(const char *host_path);

int crt_default_text(void);      /* msvcrt.c: _fmode is not _O_BINARY */
struct Stream {
    PlatFile *pf; int console;          /* console: 1 stdout, 2 stderr, 3 stdin */
    int text, rd, wr, append, eof, err;
    uint8_t buf[4096]; int bpos, blen;  /* read buffer */
    int ungot[4], nungot;
    uint32_t guest;                     /* the guest FILE */
    int fd;                             /* low-level descriptor it was opened from, or -1 */
};
#define MAXS 256
static Stream *streams[MAXS];
static uint32_t iob;                    /* guest _iob[3]: stdin, stdout, stderr */

/* ---------------------------------------------------------------- errno */
uint32_t crt_teb_slot(int which) { return cur_teb() + 0xF80 + 4 * (uint32_t)which; }
void crt_set_errno(int e) { if (cur_teb()) WR32(crt_teb_slot(0), (uint32_t)e); }
int crt_errno_of(int pe) { switch (pe) { case PLAT_E_NOENT: case PLAT_E_NOTDIR: return 2; case PLAT_E_EXIST: return 17; case PLAT_E_ACCESS: case PLAT_E_ISDIR: return 13; case PLAT_E_NOTEMPTY: return 41; default: return 5; } }

/* ---------------------------------------------------------------- streams */
static void init_std(void);
Stream *crt_stream(uint32_t g) {
    init_std();
    if (!g || !g_valid(g, 32)) return NULL;
    uint32_t i = RD32(g + 16); return i < MAXS && streams[i] && streams[i]->guest == g ? streams[i] : NULL;
}
static uint32_t stream_new(PlatFile *pf, int console, int text, int rd, int wr, int append) {
    for (uint32_t i = 3; i < MAXS; i++) if (!streams[i]) {
        Stream *s = calloc(1, sizeof *s); s->pf = pf; s->console = console; s->text = text; s->rd = rd; s->wr = wr; s->append = append; s->fd = -1;
        s->guest = g_alloc(32); WR32(s->guest + 12, (rd ? 1u : 0) | (wr ? 2u : 0)); WR32(s->guest + 16, i); streams[i] = s; return s->guest;
    }
    return 0;
}
static void init_std(void) {
    if (iob) return;
    iob = g_alloc(3 * 32);
    for (uint32_t i = 0; i < 3; i++) { Stream *s = calloc(1, sizeof *s); s->console = i == 0 ? 3 : (int)i; s->text = 1; s->rd = i == 0; s->wr = i != 0; s->guest = iob + 32 * i; s->fd = (int)i; WR32(s->guest + 16, i); WR32(s->guest + 12, i ? 2 : 1); streams[i] = s; }
}
uint32_t crt_iob(void) { init_std(); return iob; }
static int raw_getc(Stream *s) {
    if (s->bpos >= s->blen) {
        if (!s->pf) return -1;
        int64_t n = plat_fs_read(s->pf, s->buf, sizeof s->buf); if (n <= 0) { if (n < 0) s->err = 1; s->blen = s->bpos = 0; return -1; }
        s->blen = (int)n; s->bpos = 0;
    }
    return s->buf[s->bpos++];
}
static void drop_readbuf(Stream *s) {        /* before writing or seeking: give back what was read ahead */
    if (s->blen - s->bpos > 0 && s->pf) plat_fs_seek(s->pf, -(int64_t)(s->blen - s->bpos), PLAT_SEEK_CUR);
    s->bpos = s->blen = 0; s->nungot = 0;
}
int stream_getc(Stream *s) {
    if (s->nungot) return s->ungot[--s->nungot];
    if (s->eof) return -1;
    int ch = raw_getc(s);
    if (s->text) {
        if (ch == 0x1A) { s->eof = 1; if (s->bpos > 0) s->bpos--; return -1; }
        if (ch == '\r') { int n = raw_getc(s); if (n == '\n') return '\n'; if (n >= 0) s->bpos--; }
    }
    if (ch < 0) s->eof = 1;
    return ch;
}
void stream_ungetc(Stream *s, int ch) { if (ch >= 0 && s->nungot < 4) { s->ungot[s->nungot++] = ch; s->eof = 0; } }
static int64_t stream_write(Stream *s, const uint8_t *p, uint64_t n) {
    if (s->console) { if (s->console != 3) console_write(s->console == 2, p, (uint32_t)n); return (int64_t)n; }
    if (!s->pf || !s->wr) { s->err = 1; return -1; }
    drop_readbuf(s);
    if (s->append) plat_fs_seek(s->pf, 0, PLAT_SEEK_END);
    if (!s->text) { int64_t r = plat_fs_write(s->pf, p, n); if (r < 0) s->err = 1; return r; }
    uint8_t tmp[8192]; uint64_t done = 0;
    while (done < n) {
        size_t k = 0; uint64_t used = 0;
        while (done + used < n && k < sizeof tmp - 2) { uint8_t ch = p[done + used++]; if (ch == '\n') tmp[k++] = '\r'; tmp[k++] = ch; }
        if (plat_fs_write(s->pf, tmp, k) != (int64_t)k) { s->err = 1; return (int64_t)done; }
        done += used;
    }
    return (int64_t)n;
}
static int64_t stream_tell(Stream *s) {
    if (!s->pf) return -1;
    int64_t p = plat_fs_seek(s->pf, 0, PLAT_SEEK_CUR); if (p < 0) return -1;
    return p - (s->blen - s->bpos) - s->nungot;
}
static int stream_seek(Stream *s, int64_t off, int whence) {
    if (!s->pf) return -1;
    if (whence == PLAT_SEEK_CUR) { off += stream_tell(s); whence = PLAT_SEEK_SET; }
    s->bpos = s->blen = 0; s->nungot = 0; s->eof = 0;
    return plat_fs_seek(s->pf, off, whence) < 0 ? -1 : 0;
}
static void stream_close(uint32_t g) {
    Stream *s = crt_stream(g); if (!s || g == iob || g == iob + 32 || g == iob + 64) return;
    if (s->pf) plat_fs_close(s->pf);
    streams[RD32(g + 16)] = NULL; g_free(g); free(s);
}
/* "r", "w+b", "at", "rb, ccs=UNICODE" ... */
static int parse_mode(const char *m, int *rd, int *wr, int *append, int *text) {
    int fl = 0; *rd = *wr = *append = 0; *text = crt_default_text();
    switch (m[0]) { case 'r': fl = PLAT_READ; *rd = 1; break; case 'w': fl = PLAT_WRITE | PLAT_CREATE | PLAT_TRUNCATE; *wr = 1; break; case 'a': fl = PLAT_WRITE | PLAT_CREATE; *wr = *append = 1; break; default: return -1; }
    for (const char *p = m + 1; *p && *p != ','; p++) { if (*p == '+') { fl |= PLAT_READ | PLAT_WRITE; *rd = *wr = 1; } else if (*p == 'b') *text = 0; else if (*p == 't') *text = 1; }
    return fl;
}
static uint32_t do_fopen(const char *name, const char *mode) {
    int rd, wr, ap, text, fl = parse_mode(mode, &rd, &wr, &ap, &text);
    if (fl < 0) { crt_set_errno(22); return 0; }
    int err; PlatFile *f = vfs_open(name, fl, &err, NULL, 0);
    if (!f) { if (err == PLAT_E_NOENT) port_miss("fopen", name); crt_set_errno(crt_errno_of(err)); return 0; }
    uint32_t g = stream_new(f, 0, text, rd, wr, ap); if (!g) { plat_fs_close(f); crt_set_errno(24); } return g;
}
SHIM(fopen_) { RET(do_fopen(gs(A(0)), gs(A(1)))); }
SHIM(fsopen_) { RET(do_fopen(gs(A(0)), gs(A(1)))); }
SHIM(fopen_s_) { uint32_t g = do_fopen(gs(A(1)), gs(A(2))); WR32(A(0), g); RET(g ? 0 : 2); }
SHIM(freopen_) { stream_close(A(2)); RET(do_fopen(gs(A(0)), gs(A(1)))); }
SHIM(fclose_) { Stream *s = crt_stream(A(0)); if (!s) { RET(0xFFFFFFFFu); return; } stream_close(A(0)); RET(0); }
SHIM(fcloseall_) { int n = 0; for (uint32_t i = 3; i < MAXS; i++) if (streams[i]) { stream_close(streams[i]->guest); n++; } RET((uint32_t)n); }
SHIM(fread_) {
    Stream *s = crt_stream(A(3)); uint32_t sz = A(1), cnt = A(2), dst = A(0); if (!s || !sz || !cnt) { RET(0); return; }
    uint64_t want = (uint64_t)sz * cnt, got = 0; uint8_t *d = GP(dst);
    if (!s->text) {
        while (s->nungot && got < want) d[got++] = (uint8_t)s->ungot[--s->nungot];
        while (got < want && s->bpos < s->blen) d[got++] = s->buf[s->bpos++];
        if (got < want && s->pf) { int64_t r = plat_fs_read(s->pf, d + got, want - got); if (r > 0) got += (uint64_t)r; else if (r < 0) s->err = 1; }
        if (got < want) s->eof = 1;
    } else { int ch; while (got < want && (ch = stream_getc(s)) >= 0) d[got++] = (uint8_t)ch; }
    RET((uint32_t)(got / sz));
}
SHIM(fwrite_) { Stream *s = crt_stream(A(3)); uint32_t sz = A(1), cnt = A(2); if (!s || !sz || !cnt) { RET(0); return; } int64_t r = stream_write(s, GP(A(0)), (uint64_t)sz * cnt); RET(r < 0 ? 0 : (uint32_t)((uint64_t)r / sz)); }
SHIM(fseek_) { Stream *s = crt_stream(A(0)); RET(s && !stream_seek(s, (int32_t)A(1), (int)A(2)) ? 0 : 0xFFFFFFFFu); }
SHIM(fseeki64_) { Stream *s = crt_stream(A(0)); RET(s && !stream_seek(s, (int64_t)(((uint64_t)A(2) << 32) | A(1)), (int)A(3)) ? 0 : 0xFFFFFFFFu); }
SHIM(ftell_) { Stream *s = crt_stream(A(0)); RET(s ? (uint32_t)stream_tell(s) : 0xFFFFFFFFu); }
SHIM(ftelli64_) { Stream *s = crt_stream(A(0)); ret64(c, s ? (uint64_t)stream_tell(s) : (uint64_t)-1); }
SHIM(rewind_) { Stream *s = crt_stream(A(0)); if (s) { stream_seek(s, 0, PLAT_SEEK_SET); s->err = 0; } }
SHIM(fgetpos_) { Stream *s = crt_stream(A(0)); if (!s) { RET(0xFFFFFFFFu); return; } WR64(A(1), (uint64_t)stream_tell(s)); RET(0); }
SHIM(fsetpos_) { Stream *s = crt_stream(A(0)); RET(s && !stream_seek(s, (int64_t)RD64(A(1)), PLAT_SEEK_SET) ? 0 : 0xFFFFFFFFu); }
SHIM(feof_) { Stream *s = crt_stream(A(0)); RET(s ? s->eof && !s->nungot : 1); }
SHIM(ferror_) { Stream *s = crt_stream(A(0)); RET(s ? s->err : 1); }
SHIM(clearerr_) { Stream *s = crt_stream(A(0)); if (s) s->eof = s->err = 0; }
SHIM(fflush_) { Stream *s = crt_stream(A(0)); if (s && s->pf) { drop_readbuf(s); plat_fs_flush(s->pf); } RET(0); }
SHIM(setbuf_) { }
SHIM(setvbuf_) { RET(0); }
SHIM(fgetc_) { Stream *s = crt_stream(A(0)); RET(s ? (uint32_t)stream_getc(s) : 0xFFFFFFFFu); }
SHIM(ungetc_) { Stream *s = crt_stream(A(1)); if (!s || (int32_t)A(0) < 0) { RET(0xFFFFFFFFu); return; } stream_ungetc(s, (int)(A(0) & 0xFF)); RET(A(0) & 0xFF); }
SHIM(fgets_) {
    Stream *s = crt_stream(A(2)); uint32_t d = A(0); int n = (int32_t)A(1); if (!s || n <= 0) { RET(0); return; }
    int k = 0, ch = 0; while (k < n - 1 && (ch = stream_getc(s)) >= 0) { WR8(d + (uint32_t)k++, (uint8_t)ch); if (ch == '\n') break; }
    if (!k) { RET(0); return; } WR8(d + (uint32_t)k, 0); RET(d);
}
SHIM(fputc_) { Stream *s = crt_stream(A(1)); uint8_t ch = (uint8_t)A(0); RET(s && stream_write(s, &ch, 1) == 1 ? ch : 0xFFFFFFFFu); }
SHIM(fputs_) { Stream *s = crt_stream(A(1)); const char *p = gs(A(0)); RET(s && stream_write(s, (const uint8_t *)p, strlen(p)) >= 0 ? 0 : 0xFFFFFFFFu); }
SHIM(puts_) { const char *p = gs(A(0)); console_write(0, p, (uint32_t)strlen(p)); console_write(0, "\n", 1); RET(0); }
SHIM(putchar_) { char ch = (char)A(0); console_write(0, &ch, 1); RET(A(0) & 0xFF); }
SHIM(getchar_) { RET(0xFFFFFFFFu); }
SHIM(fileno_) { Stream *s = crt_stream(A(0)); RET(s ? (uint32_t)(s->fd >= 0 ? s->fd : (int)RD32(A(0) + 16) + 1000) : 0xFFFFFFFFu); }
SHIM(fprintf_) { Stream *s = crt_stream(A(0)); char *o; int n = crt_format(&o, gs(A(1)), c->esp + 12, 0); if (s) stream_write(s, (const uint8_t *)o, (uint64_t)n); free(o); RET(n); }
SHIM(vfprintf_) { Stream *s = crt_stream(A(0)); char *o; int n = crt_format(&o, gs(A(1)), A(2), 0); if (s) stream_write(s, (const uint8_t *)o, (uint64_t)n); free(o); RET(n); }
SHIM(printf_) { char *o; int n = crt_format(&o, gs(A(0)), c->esp + 8, 0); console_write(0, o, (uint32_t)n); free(o); RET(n); }
SHIM(vprintf_) { char *o; int n = crt_format(&o, gs(A(0)), A(1), 0); console_write(0, o, (uint32_t)n); free(o); RET(n); }
SHIM(fscanf_) { Stream *s = crt_stream(A(0)); RET(s ? (uint32_t)crt_fscanf(s, gs(A(1)), c->esp + 12) : 0xFFFFFFFFu); }
SHIM(iob_func) { RET(crt_iob()); }
SHIM(tmpfile_) { RET(0); }

/* ---------------------------------------------------------------- low-level descriptors */
typedef struct { PlatFile *pf; int text, append, used; } Fd;
#define MAXFD 256
static Fd fds[MAXFD];
static Fd *FD(uint32_t fd) { return fd < MAXFD && fds[fd].used ? &fds[fd] : NULL; }
static int fd_open(const char *name, uint32_t of) {
    int fl = (of & 3) == 0 ? PLAT_READ : (of & 3) == 1 ? PLAT_WRITE : PLAT_READ | PLAT_WRITE;
    if (of & 0x100) fl |= PLAT_CREATE; if (of & 0x200) fl |= PLAT_TRUNCATE; if (of & 0x400) fl |= PLAT_EXCLUSIVE;
    int err; PlatFile *f = vfs_open(name, fl, &err, NULL, 0);
    if (!f) { if (err == PLAT_E_NOENT) port_miss("_open", name); crt_set_errno(crt_errno_of(err)); return -1; }
    for (int i = 3; i < MAXFD; i++) if (!fds[i].used) { fds[i] = (Fd){ f, !(of & 0x8000) && ((of & 0x4000) || crt_default_text()), (of & 8) != 0, 1 }; return i; }
    plat_fs_close(f); crt_set_errno(24); return -1;
}
SHIM(open_) { RET((uint32_t)fd_open(gs(A(0)), A(1))); }
SHIM(sopen_) { RET((uint32_t)fd_open(gs(A(0)), A(1))); }
SHIM(sopen_s_) { int fd = fd_open(gs(A(1)), A(2)); WR32(A(0), (uint32_t)fd); RET(fd < 0 ? 2 : 0); }
SHIM(close_) { Fd *f = FD(A(0)); if (!f) { if (A(0) < 3) { RET(0); return; } crt_set_errno(9); RET(0xFFFFFFFFu); return; } plat_fs_close(f->pf); f->used = 0; RET(0); }
SHIM(read_) {
    Fd *f = FD(A(0)); if (!f) { crt_set_errno(9); RET(0xFFFFFFFFu); return; }
    uint8_t *d = GP(A(1)); int64_t n = plat_fs_read(f->pf, d, A(2)); if (n < 0) { crt_set_errno(5); RET(0xFFFFFFFFu); return; }
    if (f->text) {        /* CR LF -> LF, Ctrl-Z ends the file */
        int64_t k = 0; for (int64_t i = 0; i < n; i++) {
            if (d[i] == 0x1A) { plat_fs_seek(f->pf, i - n, PLAT_SEEK_CUR); break; }
            if (d[i] == '\r' && i + 1 < n && d[i + 1] == '\n') continue;
            d[k++] = d[i];
        }
        n = k;
    }
    RET((uint32_t)n);
}
SHIM(write_) {
    uint32_t fd = A(0); if (fd == 1 || fd == 2) { console_write(fd == 2, GP(A(1)), A(2)); RET(A(2)); return; }
    Fd *f = FD(fd); if (!f) { crt_set_errno(9); RET(0xFFFFFFFFu); return; }
    if (f->append) plat_fs_seek(f->pf, 0, PLAT_SEEK_END);
    int64_t n;
    if (f->text) { const uint8_t *p = GP(A(1)); uint32_t len = A(2); uint8_t tmp[8192]; n = 0;
        for (uint32_t i = 0; i < len;) { size_t k = 0; uint32_t used = 0; while (i + used < len && k < sizeof tmp - 2) { uint8_t ch = p[i + used++]; if (ch == '\n') tmp[k++] = '\r'; tmp[k++] = ch; }
            if (plat_fs_write(f->pf, tmp, k) != (int64_t)k) break; n += used; i += used; } }
    else n = plat_fs_write(f->pf, GP(A(1)), A(2));
    RET(n < 0 ? 0xFFFFFFFFu : (uint32_t)n);
}
SHIM(lseek_) { Fd *f = FD(A(0)); int64_t r = f ? plat_fs_seek(f->pf, (int32_t)A(1), (int)A(2)) : -1; if (r < 0) crt_set_errno(f ? 22 : 9); RET(r < 0 ? 0xFFFFFFFFu : (uint32_t)r); }
SHIM(lseeki64_) { Fd *f = FD(A(0)); int64_t r = f ? plat_fs_seek(f->pf, (int64_t)(((uint64_t)A(2) << 32) | A(1)), (int)A(3)) : -1; ret64(c, (uint64_t)r); }
SHIM(tell_) { Fd *f = FD(A(0)); RET(f ? (uint32_t)plat_fs_seek(f->pf, 0, PLAT_SEEK_CUR) : 0xFFFFFFFFu); }
SHIM(eof_) { Fd *f = FD(A(0)); if (!f) { RET(0xFFFFFFFFu); return; } PlatStat st; plat_fs_fstat(f->pf, &st); RET((uint64_t)plat_fs_seek(f->pf, 0, PLAT_SEEK_CUR) >= st.size); }
SHIM(filelength_) { Fd *f = FD(A(0)); PlatStat st; RET(f && !plat_fs_fstat(f->pf, &st) ? (uint32_t)st.size : 0xFFFFFFFFu); }
SHIM(filelengthi64_) { Fd *f = FD(A(0)); PlatStat st; ret64(c, f && !plat_fs_fstat(f->pf, &st) ? st.size : (uint64_t)-1); }
SHIM(chsize_) { Fd *f = FD(A(0)); RET(f && !plat_fs_truncate(f->pf, A(1)) ? 0 : 0xFFFFFFFFu); }
SHIM(commit_) { Fd *f = FD(A(0)); if (f) plat_fs_flush(f->pf); RET(0); }
SHIM(setmode_) { Fd *f = FD(A(0)); if (!f) { RET(0x8000); return; } uint32_t old = f->text ? 0x4000 : 0x8000; f->text = A(1) == 0x4000; RET(old); }
SHIM(isatty_) { RET(A(0) < 3); }
SHIM(dup_) { RET(0xFFFFFFFFu); }
SHIM(get_osfhandle_) { RET(A(0) + 0x7000); }
SHIM(fdopen_) { Fd *f = FD(A(0)); if (!f) { RET(0); return; } int rd, wr, ap, text; parse_mode(gs(A(1)), &rd, &wr, &ap, &text); uint32_t g = stream_new(f->pf, 0, f->text, rd, wr, ap); if (g) { f->used = 0; crt_stream(g)->fd = (int)A(0); } RET(g); }
SHIM(umask_) { RET(0); }

/* ---------------------------------------------------------------- files and directories by name */
static void put_stat(uint32_t p, const PlatStat *st) {        /* struct _stat64i32 (48 bytes) */
    memset(GP(p), 0, 48); WR16(p + 6, (uint16_t)((st->is_dir ? 0x4000 : 0x8000) | (st->readonly ? 0x124 : 0x1B6) | (st->is_dir ? 0x49 : 0))); WR16(p + 8, 1); WR32(p + 4 + 12, 2);
    WR32(p + 20, (uint32_t)st->size); WR64(p + 24, (uint64_t)(st->atime_ns / 1000000000)); WR64(p + 32, (uint64_t)(st->mtime_ns / 1000000000)); WR64(p + 40, (uint64_t)(st->ctime_ns / 1000000000));
}
static void put_stat32(uint32_t p, const PlatStat *st) {      /* struct _stat (32-bit times, 36 bytes) */
    memset(GP(p), 0, 36); WR16(p + 6, (uint16_t)((st->is_dir ? 0x4000 : 0x8000) | 0x1B6)); WR16(p + 8, 1);
    WR32(p + 20, (uint32_t)st->size); WR32(p + 24, (uint32_t)(st->atime_ns / 1000000000)); WR32(p + 28, (uint32_t)(st->mtime_ns / 1000000000)); WR32(p + 32, (uint32_t)(st->ctime_ns / 1000000000));
}
static int stat_name(const char *name, PlatStat *st) {
    char hp[1200]; vfs_map(name, hp, sizeof hp); int e = plat_fs_stat(hp, st);
    if (e) { port_miss("stat", name); crt_set_errno(2); } return e;
}
SHIM(stat64i32_) { PlatStat st; if (stat_name(gs(A(0)), &st)) { RET(0xFFFFFFFFu); return; } put_stat(A(1), &st); RET(0); }
SHIM(stat32_) { PlatStat st; if (stat_name(gs(A(0)), &st)) { RET(0xFFFFFFFFu); return; } put_stat32(A(1), &st); RET(0); }
SHIM(fstat64i32_) { Fd *f = FD(A(0)); PlatStat st; if (!f || plat_fs_fstat(f->pf, &st)) { crt_set_errno(9); RET(0xFFFFFFFFu); return; } put_stat(A(1), &st); RET(0); }
SHIM(access_) { PlatStat st; char hp[1200]; vfs_map(gs(A(0)), hp, sizeof hp); if (plat_fs_stat(hp, &st)) { crt_set_errno(2); RET(0xFFFFFFFFu); return; } if ((A(1) & 2) && st.readonly) { crt_set_errno(13); RET(0xFFFFFFFFu); return; } RET(0); }
SHIM(remove_) { char hp[1200]; vfs_map_write(gs(A(0)), hp, sizeof hp); int e = plat_fs_remove(hp); if (e) { crt_set_errno(crt_errno_of(e)); RET(0xFFFFFFFFu); return; } vfs_changed(hp); RET(0); }
SHIM(rename_) {
    char a[1200], b[1200]; vfs_map(gs(A(0)), a, sizeof a); vfs_map_write(gs(A(1)), b, sizeof b); PlatStat st;
    if (!plat_fs_stat(b, &st)) { crt_set_errno(13); RET(0xFFFFFFFFu); return; }      /* MSVC rename does not replace */
    int e = plat_fs_rename(a, b); if (e) { crt_set_errno(crt_errno_of(e)); RET(0xFFFFFFFFu); return; } vfs_changed(a); vfs_changed(b); RET(0);
}
SHIM(mkdir_) { char hp[1200]; vfs_map_write(gs(A(0)), hp, sizeof hp); int e = plat_fs_mkdir(hp); if (e) { crt_set_errno(crt_errno_of(e)); RET(0xFFFFFFFFu); return; } vfs_changed(hp); RET(0); }
SHIM(rmdir_) { char hp[1200]; vfs_map_write(gs(A(0)), hp, sizeof hp); int e = plat_fs_rmdir(hp); if (e) { crt_set_errno(crt_errno_of(e)); RET(0xFFFFFFFFu); return; } vfs_changed(hp); RET(0); }
SHIM(chdir_) { if (!vfs_chdir(gs(A(0)))) { crt_set_errno(2); RET(0xFFFFFFFFu); return; } RET(0); }
static uint32_t cwd_into(uint32_t buf, uint32_t n) {
    char b[600]; snprintf(b, sizeof b, "C:\\%s", vfs_cwd()); for (char *p = b; *p; p++) if (*p == '/') *p = '\\';
    uint32_t l = (uint32_t)strlen(b) + 1;
    if (!buf) { buf = g_alloc(n > l ? n : l); }         /* NULL buffer: the CRT allocates one */
    else if (n < l) { crt_set_errno(34); return 0; }
    memcpy(GP(buf), b, l); return buf;
}
SHIM(getcwd_) { RET(cwd_into(A(0), A(1))); }
SHIM(getdcwd_) { RET(cwd_into(A(1), A(2))); }
SHIM(getdrive_) { RET(3); }
SHIM(fullpath_) { char b[1100]; vfs_full_win(gs(A(1)), b, sizeof b); uint32_t l = (uint32_t)strlen(b) + 1, d = A(0); if (!d) d = g_alloc(l); else if (A(2) < l) { RET(0); return; } memcpy(GP(d), b, l); RET(d); }
/* _findfirst: struct _finddata64i32_t (attrib, time_create, time_access, time_write as 64-bit, size, name[260]) */
typedef struct { KObj k; PlatDir *d; char dir[1024], pat[300]; int dots; } FindH;
static FindH *finds[64];
static int find_fill(FindH *h, uint32_t out, int t32) {
    for (;;) {
        const char *e; if (h->dots < 2) e = h->dots++ ? ".." : "."; else if (!(e = plat_fs_readdir(h->d))) return 0;
        if (!win_glob(h->pat, e)) continue;
        PlatStat st; memset(&st, 0, sizeof st); char full[1400]; snprintf(full, sizeof full, "%s/%s", h->dir, e);
        if (strcmp(e, ".") && strcmp(e, "..")) { if (plat_fs_stat(full, &st)) continue; } else st.is_dir = 1;
        if (t32) { memset(GP(out), 0, 280); WR32(out, st.is_dir ? 0x10 : 0x20); WR32(out + 4, (uint32_t)(st.ctime_ns / 1000000000)); WR32(out + 8, (uint32_t)(st.atime_ns / 1000000000)); WR32(out + 12, (uint32_t)(st.mtime_ns / 1000000000)); WR32(out + 16, (uint32_t)st.size); snprintf((char *)GP(out + 20), 260, "%s", e); }
        else { memset(GP(out), 0, 296); WR32(out, st.is_dir ? 0x10 : 0x20); WR64(out + 8, (uint64_t)(st.ctime_ns / 1000000000)); WR64(out + 16, (uint64_t)(st.atime_ns / 1000000000)); WR64(out + 24, (uint64_t)(st.mtime_ns / 1000000000)); WR32(out + 32, (uint32_t)st.size); snprintf((char *)GP(out + 36), 260, "%s", e); }
        return 1;
    }
}
static uint32_t do_findfirst(const char *w, uint32_t out, int t32) {
    char wdir[1024]; snprintf(wdir, sizeof wdir, "%s", w); char *sl = strrchr(wdir, '\\'), *s2 = strrchr(wdir, '/'); if (s2 > sl) sl = s2;
    char pat[300]; snprintf(pat, sizeof pat, "%s", sl ? sl + 1 : wdir); if (sl) sl[1] = 0; else wdir[0] = 0;
    int i; for (i = 0; i < 64 && finds[i]; i++) ; if (i == 64) { crt_set_errno(24); return 0xFFFFFFFFu; }
    FindH *h = calloc(1, sizeof *h); vfs_map(wdir[0] ? wdir : ".", h->dir, sizeof h->dir); snprintf(h->pat, sizeof h->pat, "%s", pat);
    h->dots = strpbrk(pat, "*?") ? 0 : 2; h->d = plat_fs_opendir(h->dir);
    if (!h->d) { free(h); crt_set_errno(2); return 0xFFFFFFFFu; }
    if (!find_fill(h, out, t32)) { plat_fs_closedir(h->d); free(h); crt_set_errno(2); return 0xFFFFFFFFu; }
    finds[i] = h; return 0x4000u + (uint32_t)i;
}
static FindH *FIND(uint32_t h) { return h >= 0x4000 && h < 0x4040 ? finds[h - 0x4000] : NULL; }
SHIM(findfirst64i32_) { RET(do_findfirst(gs(A(0)), A(1), 0)); }
SHIM(findfirst32_) { RET(do_findfirst(gs(A(0)), A(1), 1)); }
SHIM(findnext64i32_) { FindH *h = FIND(A(0)); if (!h || !find_fill(h, A(1), 0)) { crt_set_errno(2); RET(0xFFFFFFFFu); return; } RET(0); }
SHIM(findnext32_) { FindH *h = FIND(A(0)); if (!h || !find_fill(h, A(1), 1)) { crt_set_errno(2); RET(0xFFFFFFFFu); return; } RET(0); }
SHIM(findclose_) { FindH *h = FIND(A(0)); if (!h) { RET(0xFFFFFFFFu); return; } plat_fs_closedir(h->d); finds[A(0) - 0x4000] = NULL; free(h); RET(0); }

const ShimDef msvcrt_stdio_shims[] = {
    SA("fopen", fopen_, CDECL), SA("_fsopen", fsopen_, CDECL), SA("fopen_s", fopen_s_, CDECL), SA("freopen", freopen_, CDECL), SA("fclose", fclose_, CDECL), SA("_fcloseall", fcloseall_, CDECL),
    SA("fread", fread_, CDECL), SA("fwrite", fwrite_, CDECL), SA("fseek", fseek_, CDECL), SA("_fseeki64", fseeki64_, CDECL), SA("ftell", ftell_, CDECL), SA("_ftelli64", ftelli64_, CDECL),
    SA("rewind", rewind_, CDECL), SA("fgetpos", fgetpos_, CDECL), SA("fsetpos", fsetpos_, CDECL), SA("feof", feof_, CDECL), SA("ferror", ferror_, CDECL), SA("clearerr", clearerr_, CDECL),
    SA("fflush", fflush_, CDECL), SA("setbuf", setbuf_, CDECL), SA("setvbuf", setvbuf_, CDECL), SA("fgetc", fgetc_, CDECL), SA("getc", fgetc_, CDECL), SA("_fgetchar", getchar_, CDECL),
    SA("ungetc", ungetc_, CDECL), SA("fgets", fgets_, CDECL), SA("fputc", fputc_, CDECL), SA("putc", fputc_, CDECL), SA("fputs", fputs_, CDECL), SA("puts", puts_, CDECL),
    SA("putchar", putchar_, CDECL), SA("getchar", getchar_, CDECL), SA("_fileno", fileno_, CDECL), SA("fprintf", fprintf_, CDECL), SA("vfprintf", vfprintf_, CDECL), SA("printf", printf_, CDECL),
    SA("vprintf", vprintf_, CDECL), SA("fscanf", fscanf_, CDECL), SA("__iob_func", iob_func, CDECL), SA("tmpfile", tmpfile_, CDECL),
    SA("_open", open_, CDECL), SA("_sopen", sopen_, CDECL), SA("_sopen_s", sopen_s_, CDECL), SA("_close", close_, CDECL), SA("_read", read_, CDECL), SA("_write", write_, CDECL),
    SA("_lseek", lseek_, CDECL), SA("_lseeki64", lseeki64_, CDECL), SA("_tell", tell_, CDECL), SA("_eof", eof_, CDECL), SA("_filelength", filelength_, CDECL), SA("_filelengthi64", filelengthi64_, CDECL),
    SA("_chsize", chsize_, CDECL), SA("_commit", commit_, CDECL), SA("_setmode", setmode_, CDECL), SA("_isatty", isatty_, CDECL), SA("_dup", dup_, CDECL), SA("_get_osfhandle", get_osfhandle_, CDECL),
    SA("_fdopen", fdopen_, CDECL), SA("_umask", umask_, CDECL),
    SA("_stat64i32", stat64i32_, CDECL), SA("_stat32", stat32_, CDECL), SA("_stat", stat32_, CDECL), SA("_fstat64i32", fstat64i32_, CDECL), SA("_access", access_, CDECL),
    SA("remove", remove_, CDECL), SA("_unlink", remove_, CDECL), SA("rename", rename_, CDECL), SA("_mkdir", mkdir_, CDECL), SA("_rmdir", rmdir_, CDECL), SA("_chdir", chdir_, CDECL),
    SA("_getcwd", getcwd_, CDECL), SA("_getdcwd", getdcwd_, CDECL), SA("_getdrive", getdrive_, CDECL), SA("_fullpath", fullpath_, CDECL),
    SA("_findfirst64i32", findfirst64i32_, CDECL), SA("_findnext64i32", findnext64i32_, CDECL), SA("_findfirst32", findfirst32_, CDECL), SA("_findnext32", findnext32_, CDECL),
    SA("_findfirst", findfirst32_, CDECL), SA("_findnext", findnext32_, CDECL), SA("_findclose", findclose_, CDECL),
    { 0, 0, 0 }
};
