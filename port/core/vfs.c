/* vfs.c: Windows paths to host paths.
 *
 * The game believes it lives on drive C: (any drive letter maps to the game folder) and uses Windows paths: backslashes,
 * any letter case, a current directory, "." and "..", trailing dots. This turns such a path into a host path below the
 * game folder, matching each component case-insensitively against what exists (directory listings are cached). When the
 * platform has a separate folder for files the game writes (plat_fs_write_dir), that folder overlays the game folder:
 * reads look there first, writes always go there. */
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include "core.h"

static char guest_cwd[512];             /* relative to the game folder, '/' separated, no leading or trailing slash */

/* ---------------------------------------------------------------- directory listing cache */
typedef struct DirCache { char *path; char **names; int n; struct DirCache *next; } DirCache;
#define DC_BUCKETS 256
static DirCache *dc[DC_BUCKETS];
static char *tok(char **s) {           /* next '/'-separated component of *s (modifies it), NULL at the end */
    char *p = *s; while (*p == '/') p++; if (!*p) return NULL;
    char *e = p; while (*e && *e != '/') e++; if (*e) *e++ = 0; *s = e; return p;
}
static char *dupstr(const char *s) { size_t n = strlen(s) + 1; char *d = malloc(n); memcpy(d, s, n); return d; }
static unsigned hash_str(const char *s) { unsigned h = 2166136261u; while (*s) h = (h ^ (unsigned char)*s++) * 16777619u; return h; }
static DirCache *dir_list(const char *dir) {
    unsigned h = hash_str(dir) % DC_BUCKETS;
    for (DirCache *d = dc[h]; d; d = d->next) if (!strcmp(d->path, dir)) return d;
    PlatDir *pd = plat_fs_opendir(dir); if (!pd) return NULL;
    DirCache *d = calloc(1, sizeof *d); d->path = dupstr(dir); int cap = 0; const char *e;
    while ((e = plat_fs_readdir(pd))) { if (d->n == cap) { cap = cap ? cap * 2 : 32; d->names = realloc(d->names, (size_t)cap * sizeof *d->names); } d->names[d->n++] = dupstr(e); }
    plat_fs_closedir(pd);
    d->next = dc[h]; dc[h] = d; return d;
}
static void vfs_forget(const char *dir) {
    unsigned h = hash_str(dir) % DC_BUCKETS;
    for (DirCache **pp = &dc[h]; *pp; pp = &(*pp)->next) if (!strcmp((*pp)->path, dir)) {
        DirCache *d = *pp; *pp = d->next; for (int i = 0; i < d->n; i++) free(d->names[i]); free(d->names); free(d->path); free(d); return;
    }
}
/* call after creating, removing or renaming host_path: its directory's cached listing is stale */
void vfs_changed(const char *host_path) {
    char dir[1024]; snprintf(dir, sizeof dir, "%s", host_path); char *sl = strrchr(dir, '/');
    if (sl) { *sl = 0; vfs_forget(dir[0] ? dir : "/"); } else vfs_forget(".");
    vfs_forget(host_path);
}

/* ---------------------------------------------------------------- normalisation */
static int ieq(const char *a, const char *b) { while (*a && tolower((unsigned char)*a) == tolower((unsigned char)*b)) a++, b++; return !*a && !*b; }
/* Windows path -> component list relative to the game folder ("Data/res/a.crf"), "" for the folder itself */
static void normalise(const char *w, char *out, size_t n) {
    char tmp[1024]; size_t k = 0;
    if (w[0] && w[1] == ':') { w += 2; if (*w != '\\' && *w != '/') k = (size_t)snprintf(tmp, sizeof tmp, "%s/", guest_cwd); }   /* "C:foo": relative to the cwd */
    else if (w[0] == '\\' && w[1] == '\\') { w += 2; while (*w && *w != '\\' && *w != '/') w++; }                                  /* \\server or \\?\: drop the prefix */
    else if (*w != '\\' && *w != '/') k = (size_t)snprintf(tmp, sizeof tmp, "%s/", guest_cwd);
    for (; *w && k < sizeof tmp - 1; w++) tmp[k++] = *w == '\\' ? '/' : *w;
    tmp[k] = 0;
    /* split, drop "." and empty components, apply "..", strip trailing dots and spaces from components */
    char *parts[128]; int np = 0; char *cur = tmp;
    for (char *t; (t = tok(&cur));) {
        size_t l = strlen(t); while (l && (t[l - 1] == '.' || t[l - 1] == ' ') && strcmp(t, "..")) t[--l] = 0;
        if (!l || !strcmp(t, ".")) continue;
        if (!strcmp(t, "..")) { if (np) np--; continue; }
        if (np < 128) parts[np++] = t;
    }
    size_t o = 0; out[0] = 0;
    for (int i = 0; i < np && o < n - 1; i++) o += (size_t)snprintf(out + o, n - o, "%s%s", i ? "/" : "", parts[i]);
}
/* resolve rel (normalised) below base, case-insensitively; out = host path; returns 1 if every component exists */
static int resolve(const char *base, const char *rel, char *out, size_t n) {
    snprintf(out, n, "%s", base);
    char buf[1024]; snprintf(buf, sizeof buf, "%s", rel); char *cur = buf; int exists = 1;
    for (char *t; (t = tok(&cur));) {
        size_t l = strlen(out); const char *name = t;
        if (exists) {
            DirCache *d = dir_list(out); const char *hit = NULL;
            if (d) { for (int i = 0; i < d->n; i++) if (!strcmp(d->names[i], t)) { hit = d->names[i]; break; }
                     if (!hit) for (int i = 0; i < d->n; i++) if (ieq(d->names[i], t)) { hit = d->names[i]; break; } }
            if (hit) name = hit; else exists = 0;
        }
        snprintf(out + l, n - l, "%s%s", l && out[l - 1] == '/' ? "" : "/", name);
    }
    return exists;
}
char *vfs_map(const char *win, char *out, size_t n) {
    char rel[1024]; normalise(win, rel, sizeof rel);
    const char *wd = plat_fs_write_dir();
    if (wd && resolve(wd, rel, out, n)) return out;
    resolve(g_cfg.game_dir, rel, out, n);
    return out;
}
char *vfs_map_write(const char *win, char *out, size_t n) {
    char rel[1024]; normalise(win, rel, sizeof rel);
    const char *wd = plat_fs_write_dir();
    if (!wd) { resolve(g_cfg.game_dir, rel, out, n); return out; }
    if (!resolve(wd, rel, out, n)) {           /* create the folders the file needs in the write folder */
        char p[1024]; snprintf(p, sizeof p, "%s", out); for (char *s = p + strlen(wd) + 1; (s = strchr(s, '/')); s++) { *s = 0; plat_fs_mkdir(p); *s = '/'; }
    }
    return out;
}
const char *vfs_cwd(void) { return guest_cwd; }
int vfs_chdir(const char *win) {
    char rel[1024], host[1200]; normalise(win, rel, sizeof rel); vfs_map(win, host, sizeof host);
    PlatStat st; if (plat_fs_stat(host, &st) || !st.is_dir) return 0;
    snprintf(guest_cwd, sizeof guest_cwd, "%s", rel); return 1;
}
int vfs_err_to_win(int e) {
    switch (e) { case PLAT_E_NOENT: return 2; case PLAT_E_EXIST: return 183; case PLAT_E_ACCESS: return 5; case PLAT_E_NOTDIR: return 3;
                 case PLAT_E_ISDIR: return 5; case PLAT_E_NOTEMPTY: return 145; default: return 29; }
}
int win_glob(const char *p, const char *s) {
    if (!strcmp(p, "*.*") || !strcmp(p, "*")) return 1;
    while (*p) {
        if (*p == '*') { p++; if (!*p) return 1; for (; *s; s++) if (win_glob(p, s)) return 1; return win_glob(p, s); }
        if (!*s) { while (*p == '*' || (*p == '.' && !p[1])) p++; return !*p; }
        if (*p != '?' && tolower((unsigned char)*p) != tolower((unsigned char)*s)) return 0;
        p++; s++;
    }
    return !*s;
}

/* the full Windows path of win ("C:\Data\a.txt") */
char *vfs_full_win(const char *win, char *out, size_t n) {
    char rel[1024]; normalise(win, rel, sizeof rel);
    size_t k = (size_t)snprintf(out, n, "C:\\%s", rel);
    for (size_t i = 3; i < k && i < n; i++) if (out[i] == '/') out[i] = '\\';
    return out;
}
static int copy_file(const char *from, const char *to) {
    int err; PlatFile *a = plat_fs_open(from, PLAT_READ, &err); if (!a) return 0;
    PlatFile *b = plat_fs_open(to, PLAT_WRITE | PLAT_CREATE | PLAT_TRUNCATE, &err); if (!b) { plat_fs_close(a); return 0; }
    char buf[65536]; int64_t r; int ok = 1;
    while ((r = plat_fs_read(a, buf, sizeof buf)) > 0) if (plat_fs_write(b, buf, (uint64_t)r) != r) { ok = 0; break; }
    plat_fs_close(a); plat_fs_close(b); return ok && r == 0;
}
int vfs_copy(const char *from_host, const char *to_host) { int ok = copy_file(from_host, to_host); vfs_changed(to_host); return ok; }
/* opens a Windows path with PLAT_* flags; the host path ends up in host (if not NULL). Writes go to the write folder, which
 * first receives a copy of the game's file when it is opened for update. */
PlatFile *vfs_open(const char *win, int flags, int *err, char *host, size_t hn) {
    char hp[1200]; PlatFile *f;
    if (!(flags & (PLAT_WRITE | PLAT_CREATE | PLAT_TRUNCATE))) {
        vfs_map(win, hp, sizeof hp); f = plat_fs_open(hp, flags, err);
    } else {
        const char *wd = plat_fs_write_dir();
        vfs_map_write(win, hp, sizeof hp);
        if (wd && !(flags & PLAT_TRUNCATE)) {
            PlatStat st; char src[1200]; resolve(g_cfg.game_dir, "", src, sizeof src); { char rel[1024]; normalise(win, rel, sizeof rel); resolve(g_cfg.game_dir, rel, src, sizeof src); }
            if (plat_fs_stat(hp, &st) && !plat_fs_stat(src, &st) && !st.is_dir) copy_file(src, hp);
        }
        f = plat_fs_open(hp, flags, err);
        if (f && (flags & PLAT_CREATE)) vfs_changed(hp);
    }
    if (host) snprintf(host, hn, "%s", hp);
    return f;
}
