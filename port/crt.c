/* crt.c: the Microsoft C runtime (MSVCR90) and the few MSVCP90 std::string members the game imports, on top of libc.
 * FILE* handles seen by the game are small guest-side structs that index a host FILE table. printf-style formatting
 * reads its arguments straight from the guest stack (or a guest va_list). */
#define _GNU_SOURCE
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <fnmatch.h>
#include <dirent.h>
#include <sys/stat.h>
#include "port.h"

static void ret64(CPU *c, uint64_t v) { c->eax = (uint32_t)v; c->edx = (uint32_t)(v >> 32); }
static double argd(CPU *c, int i) { uint64_t v = ((uint64_t)A(i + 1) << 32) | A(i); double d; memcpy(&d, &v, 8); return d; }
static double ci0(CPU *c) { return ST(0); }

/* ---------------------------------------------------------------- start-up */
SHIM(initterm) { for (uint32_t p = A(0); p < A(1); p += 4) { uint32_t f = RD32(p); if (f) g_call(c, f, 0); } }
SHIM(initterm_e) { for (uint32_t p = A(0); p < A(1); p += 4) { uint32_t f = RD32(p); if (f) { uint32_t r = g_call(c, f, 0); if (r) { RET(r); return; } } } RET(0); }
SHIM(getmainargs) {
    uint32_t av = g_alloc(8), ev = g_alloc(4); WR32(av, g_str("SS2.exe")); WR32(A(0), 1); WR32(A(1), av); WR32(A(2), ev); RET(0);
}
SHIM(p_commode) { static uint32_t p; if (!p) p = g_alloc(4); RET(p); }
SHIM(p_fmode) { static uint32_t p; if (!p) p = g_alloc(4); RET(p); }
SHIM(errno_) { static uint32_t p; if (!p) p = g_alloc(4); WR32(p, (uint32_t)errno); RET(p); }
SHIM(zero) { RET(0); }
SHIM(ident) { RET(A(0)); }
SHIM(nop) { }
SHIM(exit_) { port_log("exit(%d)", (int)A(0)); port_exit((int)A(0)); }
SHIM(die_amsg) { port_die("_amsg_exit(%u)", A(0)); }
SHIM(die_watson) { port_die("_invoke_watson: the CRT's parameter validation failed"); }
SHIM(die_xcpt) { port_die("an unhandled exception filter was reached (_XcptFilter)"); }
SHIM(die_cxx) { port_die("C++ exception thrown (_CxxThrowException); exceptions are not supported by this host yet"); }
SHIM(die_terminate) { port_die("std::terminate"); }
SHIM(onexit) { RET(A(0)); }
SHIM(controlfp_s) { if (A(0)) WR32(A(0), 0x9001F); RET(0); }
SHIM(heapchk) { RET((uint32_t)-2); }
SHIM(ismbblead) { RET(0); }

/* ---------------------------------------------------------------- memory */
SHIM(malloc_) { RET(g_alloc(A(0) ? A(0) : 1)); }
SHIM(calloc_) { RET(g_alloc((uint32_t)((uint64_t)A(0) * A(1)))); }
SHIM(realloc_) { RET(A(1) ? g_realloc(A(0), A(1)) : (g_free(A(0)), 0)); }
SHIM(free_) { g_free(A(0)); }
SHIM(msize) { RET(g_size(A(0))); }
SHIM(memcpy_) { memmove(GP(A(0)), GP(A(1)), A(2)); RET(A(0)); }
SHIM(memset_) { memset(GP(A(0)), (int)A(1), A(2)); RET(A(0)); }
SHIM(memchr_) { void *p = memchr(GP(A(0)), (int)A(1), A(2)); RET(p ? A(0) + (uint32_t)((uint8_t *)p - GP(A(0))) : 0); }
SHIM(memmove_s) { if (A(3) > A(1)) { RET(34); return; } memmove(GP(A(0)), GP(A(2)), A(3)); RET(0); }

/* ---------------------------------------------------------------- strings */
SHIM(strdup_) { RET(g_str(gs(A(0)))); }
SHIM(stricmp_) { RET((uint32_t)strcasecmp(gs(A(0)), gs(A(1)))); }
SHIM(strnicmp_) { RET((uint32_t)strncasecmp(gs(A(0)), gs(A(1)), A(2))); }
SHIM(strncmp_) { RET((uint32_t)strncmp(gs(A(0)), gs(A(1)), A(2))); }
SHIM(strlwr_) { for (char *s = (char *)GP(A(0)); *s; s++) *s = (char)tolower((unsigned char)*s); RET(A(0)); }
SHIM(strncpy_) { strncpy((char *)GP(A(0)), gs(A(1)), A(2)); RET(A(0)); }
SHIM(strncat_) { strncat((char *)GP(A(0)), gs(A(1)), A(2)); RET(A(0)); }
SHIM(strncpy_s) {
    uint32_t cnt = A(3), sz = A(1); const char *s = gs(A(2)); size_t l = strlen(s); if (cnt != 0xFFFFFFFFu && l > cnt) l = cnt;
    if (l >= sz) { if (sz) WR8(A(0), 0); RET(cnt == 0xFFFFFFFFu ? 80 : 34); return; } memcpy(GP(A(0)), s, l); WR8(A(0) + (uint32_t)l, 0); RET(0);
}
SHIM(strncat_s) {
    char *d = (char *)GP(A(0)); size_t dl = strlen(d), sz = A(1); const char *s = gs(A(2)); size_t l = strlen(s); if (A(3) != 0xFFFFFFFFu && l > A(3)) l = A(3);
    if (dl + l >= sz) { RET(34); return; } memcpy(d + dl, s, l); d[dl + l] = 0; RET(0);
}
static uint32_t gofs(const char *base, uint32_t g, const char *p) { return p ? g + (uint32_t)(p - base) : 0; }
SHIM(strchr_) { const char *b = gs(A(0)); RET(gofs(b, A(0), strchr(b, (int)A(1)))); }
SHIM(strrchr_) { const char *b = gs(A(0)); RET(gofs(b, A(0), strrchr(b, (int)A(1)))); }
SHIM(strstr_) { const char *b = gs(A(0)); RET(gofs(b, A(0), strstr(b, gs(A(1))))); }
SHIM(strpbrk_) { const char *b = gs(A(0)); RET(gofs(b, A(0), strpbrk(b, gs(A(1))))); }
SHIM(strcspn_) { RET((uint32_t)strcspn(gs(A(0)), gs(A(1)))); }
static uint32_t tok_next;
SHIM(strtok_) {
    uint32_t s = A(0) ? A(0) : tok_next; const char *d = gs(A(1)); if (!s) { RET(0); return; }
    while (RD8(s) && strchr(d, RD8(s))) s++;
    if (!RD8(s)) { tok_next = 0; RET(0); return; }
    uint32_t b = s; while (RD8(s) && !strchr(d, RD8(s))) s++;
    if (RD8(s)) { WR8(s, 0); tok_next = s + 1; } else tok_next = 0; RET(b);
}
SHIM(strtol_) { char *e; const char *b = gs(A(0)); long v = strtol(b, &e, (int)A(2)); if (A(1)) WR32(A(1), A(0) + (uint32_t)(e - b)); RET((uint32_t)v); }
SHIM(strtoul_) { char *e; const char *b = gs(A(0)); unsigned long v = strtoul(b, &e, (int)A(2)); if (A(1)) WR32(A(1), A(0) + (uint32_t)(e - b)); RET((uint32_t)v); }
SHIM(atoi_) { RET((uint32_t)atoi(gs(A(0)))); }
SHIM(atof_) { RETF(atof(gs(A(0)))); }
SHIM(itoa_) { char b[40]; int r = (int)A(2); int32_t v = (int32_t)A(0); const char *d = "0123456789abcdefghijklmnopqrstuvwxyz"; char t[40]; int n = 0, neg = r == 10 && v < 0; uint32_t u = neg ? (uint32_t)-v : (uint32_t)v;
    if (!u) t[n++] = '0'; while (u) { t[n++] = d[u % (unsigned)r]; u /= (unsigned)r; } int k = 0; if (neg) b[k++] = '-'; while (n) b[k++] = t[--n]; b[k] = 0; memcpy(GP(A(1)), b, (size_t)k + 1); RET(A(1)); }
SHIM(ultoa_) { char b[40]; int r = (int)A(2); uint32_t u = A(0); const char *d = "0123456789abcdefghijklmnopqrstuvwxyz"; char t[40]; int n = 0; if (!u) t[n++] = '0'; while (u) { t[n++] = d[u % (unsigned)r]; u /= (unsigned)r; } int k = 0; while (n) b[k++] = t[--n]; b[k] = 0; memcpy(GP(A(1)), b, (size_t)k + 1); RET(A(1)); }
SHIM(gcvt_) { char b[64]; snprintf(b, sizeof b, "%.*g", (int)A(2), argd(c, 0)); memcpy(GP(A(3)), b, strlen(b) + 1); RET(A(3)); }
#define CT(name, fn) SHIM(name) { RET((uint32_t)(fn((int)A(0)) != 0)); }
CT(isupper_, isupper) CT(islower_, islower) CT(isspace_, isspace) CT(isdigit_, isdigit) CT(isxdigit_, isxdigit) CT(isalpha_, isalpha) CT(isprint_, isprint) CT(isalnum_, isalnum)
SHIM(tolower_) { RET((uint32_t)tolower((int)A(0))); }
SHIM(toupper_) { RET((uint32_t)toupper((int)A(0))); }
SHIM(iswspace_) { uint32_t w = A(0); RET(w == ' ' || (w >= 9 && w <= 13)); }
SHIM(iswctype_) { uint32_t w = A(0), t = A(1); int r = 0; if (w < 128) { if ((t & 1) && isupper((int)w)) r = 1; if ((t & 2) && islower((int)w)) r = 1; if ((t & 4) && isdigit((int)w)) r = 1; if ((t & 8) && isspace((int)w)) r = 1; if ((t & 0x10) && ispunct((int)w)) r = 1; if ((t & 0x80) && isxdigit((int)w)) r = 1; if ((t & 0x100) && isalpha((int)w)) r = 1; } RET(r); }
SHIM(getenv_) { RET(0); }
SHIM(putenv_) { RET(0); }
static uint32_t rnd_seed = 1;
SHIM(srand_) { rnd_seed = A(0); }
SHIM(rand_) { rnd_seed = rnd_seed * 214013u + 2531011u; RET((rnd_seed >> 16) & 0x7FFF); }

/* ---------------------------------------------------------------- math: the doubles arrive on the guest stack, the x87 "_CI" forms in st(0)/st(1) */
SHIM(ceil_) { RETF(ceil(argd(c, 0))); }
SHIM(floor_) { RETF(floor(argd(c, 0))); }
SHIM(ldexp_) { RETF(ldexp(argd(c, 0), (int)A(2))); }
SHIM(finite_) { RET(isfinite(argd(c, 0)) != 0); }
#define CI1(n, f) SHIM(n) { ST(0) = f(ci0(c)); }
CI1(CIsqrt, sqrt) CI1(CIsin, sin) CI1(CIcos, cos) CI1(CItan, tan) CI1(CIasin, asin) CI1(CIacos, acos) CI1(CIatan, atan) CI1(CIexp, exp) CI1(CIlog, log) CI1(CIlog10, log10)
SHIM(CIatan2) { double x = ST(0), y = ST(1); FPOP(c); ST(0) = atan2(y, x); }
SHIM(CIpow) { double e = ST(0), b = ST(1); FPOP(c); ST(0) = pow(b, e); }
SHIM(CIfmod) { double d = ST(0), n = ST(1); FPOP(c); ST(0) = fmod(n, d); }

/* ---------------------------------------------------------------- printf / scanf over guest arguments */
typedef struct { uint32_t p; } VA;
static uint32_t va32(VA *v) { uint32_t x = RD32(v->p); v->p += 4; return x; }
static uint64_t va64(VA *v) { uint64_t x = RD64(v->p); v->p += 8; return x; }
static double vad(VA *v) { double d = LDF64(v->p); v->p += 8; return d; }
static int msvc_format(char **out, const char *f, VA *va) {      /* returns length; *out is malloc'ed */
    size_t cap = 256, n = 0; char *o = malloc(cap);
#define PUT(s, l) do { size_t l_ = (l); while (n + l_ + 1 > cap) { cap *= 2; o = realloc(o, cap); } memcpy(o + n, (s), l_); n += l_; } while (0)
    while (*f) {
        if (*f != '%') { const char *e = f; while (*e && *e != '%') e++; PUT(f, (size_t)(e - f)); f = e; continue; }
        f++; if (*f == '%') { PUT("%", 1); f++; continue; }
        char spec[48]; int sl = 0; spec[sl++] = '%';
        while (*f && strchr("-+ #0", *f) && sl < 40) spec[sl++] = *f++;
        if (*f == '*') { sl += snprintf(spec + sl, 12, "%d", (int)va32(va)); f++; } else while (isdigit((unsigned char)*f) && sl < 40) spec[sl++] = *f++;
        if (*f == '.') { spec[sl++] = *f++; if (*f == '*') { sl += snprintf(spec + sl, 12, "%d", (int)va32(va)); f++; } else while (isdigit((unsigned char)*f) && sl < 40) spec[sl++] = *f++; }
        int ll = 0, h = 0, w = 0;
        for (;;) {
            if (*f == 'l') { f++; if (*f == 'l') { ll = 1; f++; } else w = 1; }
            else if (*f == 'h') { h = 1; f++; } else if (*f == 'L') { ll = 1; f++; }
            else if (f[0] == 'I' && f[1] == '6' && f[2] == '4') { ll = 1; f += 3; } else if (f[0] == 'I' && f[1] == '3' && f[2] == '2') f += 3;
            else break;
        }
        char t[400]; char cv = *f ? *f++ : 0; spec[sl] = 0;
        switch (cv) {
        case 'd': case 'i': { strcat(spec, ll ? "lld" : "d"); if (ll) snprintf(t, sizeof t, spec, (long long)va64(va)); else { int v = (int)va32(va); if (h) v = (short)v; snprintf(t, sizeof t, spec, v); } PUT(t, strlen(t)); break; }
        case 'u': case 'x': case 'X': case 'o': { char s2[4] = { ll ? 'l' : 0, ll ? 'l' : cv, ll ? cv : 0, 0 }; if (!ll) { s2[0] = cv; s2[1] = 0; } strcat(spec, s2);
            if (ll) snprintf(t, sizeof t, spec, (unsigned long long)va64(va)); else { unsigned v = va32(va); if (h) v &= 0xFFFF; snprintf(t, sizeof t, spec, v); } PUT(t, strlen(t)); break; }
        case 'c': { int v = (int)va32(va); strcat(spec, "c"); snprintf(t, sizeof t, spec, v); PUT(t, strlen(t)); break; }
        case 'p': { snprintf(t, sizeof t, "%08X", va32(va)); PUT(t, strlen(t)); break; }
        case 's': { uint32_t a = va32(va); const char *s = a ? (w ? "(wide)" : (const char *)GP(a)) : "(null)"; size_t need = strlen(s) + 400; char *b2 = malloc(need); strcat(spec, "s"); snprintf(b2, need, spec, s); PUT(b2, strlen(b2)); free(b2); break; }
        case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A': { char s2[2] = { cv, 0 }; strcat(spec, s2); double d = vad(va); char *b2 = malloc(512); snprintf(b2, 512, spec, d); PUT(b2, strlen(b2)); free(b2); break; }
        case 'n': (void)va32(va); break;
        default: break;
        }
    }
    o[n] = 0; *out = o; return (int)n;
#undef PUT
}
/* result of an _snprintf-family call with the MSVC rules for truncation */
static int put_trunc(uint32_t dst, uint32_t cap, const char *s, int len, int nulterm_always) {
    if (!cap) return -1;
    if ((uint32_t)len < cap) { memcpy(GP(dst), s, (size_t)len + 1); return len; }
    if (nulterm_always) { memcpy(GP(dst), s, cap - 1); WR8(dst + cap - 1, 0); return -1; }
    memcpy(GP(dst), s, cap); return (uint32_t)len == cap ? len : -1;
}
int port_format(char **o, const char *f, uint32_t va) { VA v = { va }; return msvc_format(o, f, &v); }
SHIM(sprintf_) { char *s; VA v = { c->esp + 12 }; int n = msvc_format(&s, gs(A(1)), &v); memcpy(GP(A(0)), s, (size_t)n + 1); free(s); RET(n); }
SHIM(sprintf_s) { char *s; VA v = { c->esp + 16 }; int n = msvc_format(&s, gs(A(2)), &v); int r; if ((uint32_t)n >= A(1)) { if (A(1)) WR8(A(0), 0); r = -1; } else { memcpy(GP(A(0)), s, (size_t)n + 1); r = n; } free(s); RET(r); }
SHIM(snprintf_) { char *s; VA v = { c->esp + 16 }; int n = msvc_format(&s, gs(A(2)), &v); int r = put_trunc(A(0), A(1), s, n, 0); free(s); RET(r); }
SHIM(snprintf_s) { char *s; VA v = { c->esp + 20 }; int n = msvc_format(&s, gs(A(3)), &v); int r; uint32_t cnt = A(2), sz = A(1);
    if (cnt == 0xFFFFFFFFu) r = put_trunc(A(0), sz, s, n, 1); else { uint32_t m = (uint32_t)n < cnt ? (uint32_t)n : cnt; if (m >= sz) { if (sz) WR8(A(0), 0); r = -1; } else { memcpy(GP(A(0)), s, m); WR8(A(0) + m, 0); r = (int)m; } }
    free(s); RET(r); }
SHIM(vsnprintf_) { char *s; VA v = { A(3) }; int n = msvc_format(&s, gs(A(2)), &v); int r = put_trunc(A(0), A(1), s, n, 0); free(s); RET(r); }

/* ---------------------------------------------------------------- stdio: guest FILE structs index a host table */
#define MAXF 256
static FILE *ftab[MAXF];
static uint32_t file_new(FILE *f) { for (int i = 1; i < MAXF; i++) if (!ftab[i]) { ftab[i] = f; uint32_t g = g_alloc(32); WR32(g, (uint32_t)i); return g; } fclose(f); return 0; }
static FILE *F(uint32_t g) { uint32_t i = g ? RD32(g) : 0; return i && i < MAXF ? ftab[i] : NULL; }
SHIM(fopen_) {
    char hp[1400]; host_path(gs(A(0)), hp, sizeof hp); char m[16]; snprintf(m, sizeof m, "%s", gs(A(1)));
    char *t = strchr(m, 't'); if (t) memmove(t, t + 1, strlen(t)); char *b = strchr(m, 'b'); (void)b;
    FILE *f = fopen(hp, m); if (!f) { if (errno == ENOENT) port_miss("fopen", gs(A(0)), hp); RET(0); return; } RET(file_new(f));
}
SHIM(fclose_) { FILE *f = F(A(0)); if (!f) { RET((uint32_t)-1); return; } ftab[RD32(A(0))] = NULL; g_free(A(0)); RET((uint32_t)fclose(f)); }
SHIM(fread_) { FILE *f = F(A(3)); RET(f ? (uint32_t)fread(GP(A(0)), A(1), A(2), f) : 0); }
SHIM(fwrite_) { FILE *f = F(A(3)); RET(f ? (uint32_t)fwrite(GP(A(0)), A(1), A(2), f) : 0); }
SHIM(fseek_) { FILE *f = F(A(0)); RET(f ? (uint32_t)fseek(f, (long)(int32_t)A(1), (int)A(2)) : (uint32_t)-1); }
SHIM(fseeki64_) { FILE *f = F(A(0)); int64_t o = (int64_t)(((uint64_t)A(2) << 32) | A(1)); RET(f ? (uint32_t)fseeko(f, o, (int)A(3)) : (uint32_t)-1); }
SHIM(ftell_) { FILE *f = F(A(0)); RET(f ? (uint32_t)ftell(f) : (uint32_t)-1); }
SHIM(ftelli64_) { FILE *f = F(A(0)); ret64(c, f ? (uint64_t)ftello(f) : (uint64_t)-1); }
SHIM(feof_) { FILE *f = F(A(0)); RET(f ? feof(f) != 0 : 1); }
SHIM(ferror_) { FILE *f = F(A(0)); RET(f ? ferror(f) != 0 : 1); }
SHIM(fflush_) { FILE *f = F(A(0)); RET(f ? (uint32_t)fflush(f) : 0); }
SHIM(setbuf_) { }
SHIM(fgets_) { FILE *f = F(A(2)); if (!f || !fgets((char *)GP(A(0)), (int)A(1), f)) { RET(0); return; } RET(A(0)); }
SHIM(fputs_) { FILE *f = F(A(1)); RET(f ? (uint32_t)fputs(gs(A(0)), f) : (uint32_t)-1); }
SHIM(fprintf_) { FILE *f = F(A(0)); char *s; VA v = { c->esp + 12 }; int n = msvc_format(&s, gs(A(1)), &v); if (f) fwrite(s, 1, (size_t)n, f); free(s); RET(n); }
SHIM(vfprintf_) { FILE *f = F(A(0)); char *s; VA v = { A(2) }; int n = msvc_format(&s, gs(A(1)), &v); if (f) fwrite(s, 1, (size_t)n, f); free(s); RET(n); }
SHIM(fileno_) { FILE *f = F(A(0)); RET(f ? (uint32_t)fileno(f) : (uint32_t)-1); }
SHIM(remove_) { char hp[1400]; host_path(gs(A(0)), hp, sizeof hp); RET((uint32_t)remove(hp)); }
SHIM(rename_) { char a[1400], b[1400]; host_path(gs(A(0)), a, sizeof a); host_path(gs(A(1)), b, sizeof b); RET((uint32_t)rename(a, b)); }
SHIM(mkdir_) { char hp[1400]; host_path(gs(A(0)), hp, sizeof hp); RET((uint32_t)mkdir(hp, 0755)); }
SHIM(chdir_) { char hp[1400]; host_path(gs(A(0)), hp, sizeof hp); RET((uint32_t)chdir(hp)); }
SHIM(getcwd_) { const char *w = "C:\\"; if (!A(0) || A(1) < 4) { RET(0); return; } memcpy(GP(A(0)), w, 4); RET(A(0)); }
SHIM(getdcwd_) { const char *w = "C:\\"; if (!A(1) || A(2) < 4) { RET(0); return; } memcpy(GP(A(1)), w, 4); RET(A(1)); }
SHIM(getdrive_) { RET(3); }

/* low-level descriptors (the game's own file system code uses these) */
SHIM(open_) {
    char hp[1400]; host_path(gs(A(0)), hp, sizeof hp); uint32_t fl = A(1); int of = (int)(fl & 3);
    if (fl & 0x8) of |= O_APPEND; if (fl & 0x100) of |= O_CREAT; if (fl & 0x200) of |= O_TRUNC; if (fl & 0x400) of |= O_EXCL;
    int r = open(hp, of, 0644); if (r < 0 && errno == ENOENT) port_miss("_open", gs(A(0)), hp); RET((uint32_t)r);
}
SHIM(close_) { RET((uint32_t)close((int)A(0))); }
SHIM(read_) { RET((uint32_t)read((int)A(0), GP(A(1)), A(2))); }
SHIM(write_) { RET((uint32_t)write((int)A(0), GP(A(1)), A(2))); }
SHIM(filelength_) { struct stat st; RET(fstat((int)A(0), &st) ? (uint32_t)-1 : (uint32_t)st.st_size); }
SHIM(filelengthi64_) { struct stat st; ret64(c, fstat((int)A(0), &st) ? (uint64_t)-1 : (uint64_t)st.st_size); }
SHIM(umask_) { RET(0); }
static void put_stat(uint32_t p, const struct stat *st) {   /* struct _stat64i32 */
    memset(GP(p), 0, 48); WR16(p + 6, (uint16_t)((S_ISDIR(st->st_mode) ? 0x4000 : 0x8000) | 0x1B6)); WR16(p + 8, 1); WR32(p + 20, (uint32_t)st->st_size);
    WR64(p + 24, (uint64_t)st->st_atime); WR64(p + 32, (uint64_t)st->st_mtime); WR64(p + 40, (uint64_t)st->st_ctime);
}
SHIM(stat64i32_) { char hp[1400]; struct stat st; host_path(gs(A(0)), hp, sizeof hp); if (stat(hp, &st)) { port_miss("stat", gs(A(0)), hp); RET((uint32_t)-1); return; } put_stat(A(1), &st); RET(0); }
SHIM(fstat64i32_) { struct stat st; if (fstat((int)A(0), &st)) { RET((uint32_t)-1); return; } put_stat(A(1), &st); RET(0); }

/* _findfirst / _findnext: handle = index into a table of open directories */
typedef struct { DIR *d; char pat[300]; char dir[1400]; int used; } FindH;
static FindH fh[16];
static int find_fill(FindH *h, uint32_t out) {
    struct dirent *e;
    while ((e = readdir(h->d))) {
        if (fnmatch(h->pat, e->d_name, FNM_CASEFOLD)) continue;
        char full[1900]; struct stat st; snprintf(full, sizeof full, "%s/%s", h->dir, e->d_name); if (stat(full, &st)) continue;
        memset(GP(out), 0, 296); WR32(out, S_ISDIR(st.st_mode) ? 0x10 : 0x20); WR64(out + 8, (uint64_t)st.st_ctime); WR64(out + 16, (uint64_t)st.st_atime); WR64(out + 24, (uint64_t)st.st_mtime);
        WR32(out + 32, (uint32_t)st.st_size); snprintf((char *)GP(out + 36), 260, "%s", e->d_name); return 1;
    }
    return 0;
}
SHIM(findfirst_) {
    char hp[1400]; host_path(gs(A(0)), hp, sizeof hp); char *sl = strrchr(hp, '/'); char dir[1400]; const char *pat;
    if (sl) { *sl = 0; snprintf(dir, sizeof dir, "%s", hp[0] ? hp : "/"); pat = sl + 1; } else { snprintf(dir, sizeof dir, "."); pat = hp; }
    int i; for (i = 0; i < 16 && fh[i].used; i++) ; if (i == 16) { RET((uint32_t)-1); return; }
    DIR *d = opendir(dir); if (!d) { RET((uint32_t)-1); return; }
    fh[i].d = d; fh[i].used = 1; snprintf(fh[i].pat, sizeof fh[i].pat, "%s", pat); snprintf(fh[i].dir, sizeof fh[i].dir, "%s", dir);
    if (!find_fill(&fh[i], A(1))) { closedir(d); fh[i].used = 0; RET((uint32_t)-1); return; } RET((uint32_t)i + 1);
}
SHIM(findnext_) { int i = (int)A(0) - 1; if (i < 0 || i >= 16 || !fh[i].used) { RET((uint32_t)-1); return; } RET(find_fill(&fh[i], A(1)) ? 0 : (uint32_t)-1); }
SHIM(findclose_) { int i = (int)A(0) - 1; if (i >= 0 && i < 16 && fh[i].used) { closedir(fh[i].d); fh[i].used = 0; } RET(0); }

/* time */
SHIM(time64_) { time_t t = time(NULL); if (A(0)) WR64(A(0), (uint64_t)t); ret64(c, (uint64_t)t); }
SHIM(localtime64_) {
    static uint32_t tm; if (!tm) tm = g_alloc(36); time_t t = (time_t)RD64(A(0)); struct tm l; localtime_r(&t, &l);
    int32_t v[9] = { l.tm_sec, l.tm_min, l.tm_hour, l.tm_mday, l.tm_mon, l.tm_year, l.tm_wday, l.tm_yday, l.tm_isdst }; for (int i = 0; i < 9; i++) WR32(tm + 4 * (uint32_t)i, (uint32_t)v[i]); RET(tm);
}
SHIM(asctime_) {
    static uint32_t s; if (!s) s = g_alloc(32); struct tm l; memset(&l, 0, sizeof l); int32_t *f[9] = { &l.tm_sec, &l.tm_min, &l.tm_hour, &l.tm_mday, &l.tm_mon, &l.tm_year, &l.tm_wday, &l.tm_yday, &l.tm_isdst };
    for (int i = 0; i < 9; i++) *f[i] = (int32_t)RD32(A(0) + 4 * (uint32_t)i); char b[40]; asctime_r(&l, b); memcpy(GP(s), b, strlen(b) + 1); RET(s);
}

/* sorting with guest comparators */
static CPU *sort_cpu; static uint32_t sort_fn;
static int sort_cmp(const void *a, const void *b) { return (int)g_call(sort_cpu, sort_fn, 2, (uint32_t)((const uint8_t *)a - M), (uint32_t)((const uint8_t *)b - M)); }
SHIM(qsort_) { sort_cpu = c; sort_fn = A(3); qsort(GP(A(0)), A(1), A(2), sort_cmp); }
SHIM(bsearch_) {
    uint32_t key = A(0), base = A(1), n = A(2), sz = A(3), fn = A(4), lo = 0, hi = n;
    while (lo < hi) { uint32_t mid = (lo + hi) / 2, e = base + mid * sz; int r = (int)g_call(c, fn, 2, key, e); if (!r) { RET(e); return; } if (r < 0) hi = mid; else lo = mid + 1; }
    RET(0);
}

/* scanf */
typedef struct { const uint8_t *s; FILE *f; int pos; } Src;
static int sg(Src *r) { if (r->f) { int ch = fgetc(r->f); if (ch != EOF) r->pos++; return ch; } int ch = r->s[r->pos]; if (!ch) return EOF; r->pos++; return ch; }
static void su(Src *r, int ch) { if (ch == EOF) return; r->pos--; if (r->f) ungetc(ch, r->f); }
static int msvc_scan(Src *r, const char *f, VA *va) {
    int count = 0, any = 0;
    while (*f) {
        if (isspace((unsigned char)*f)) { int ch; while ((ch = sg(r)) != EOF && isspace(ch)) ; su(r, ch); f++; continue; }
        if (*f != '%') { int ch = sg(r); if (ch != (unsigned char)*f) { su(r, ch); return any || count ? count : (ch == EOF ? -1 : 0); } f++; continue; }
        f++; if (*f == '%') { int ch = sg(r); if (ch != '%') { su(r, ch); return count; } f++; continue; }
        int supp = 0, width = 0, lng = 0, sh = 0; if (*f == '*') { supp = 1; f++; } while (isdigit((unsigned char)*f)) width = width * 10 + (*f++ - '0');
        while (*f == 'l' || *f == 'h' || *f == 'L') { if (*f == 'l' || *f == 'L') lng = 1; else sh = 1; f++; }
        char cv = *f++; if (!cv) break; if (!width) width = 1 << 20;
        if (cv == 'n') { if (!supp) WR32(va32(va), (uint32_t)r->pos); continue; }
        if (cv != 'c' && cv != '[') { int ch; while ((ch = sg(r)) != EOF && isspace(ch)) ; su(r, ch); }
        char buf[512]; int n = 0, ch;
        if (cv == 'c') { if (width == 1 << 20) width = 1; uint32_t d = supp ? 0 : va32(va); int k = 0; while (k < width && (ch = sg(r)) != EOF) { if (d) WR8(d + (uint32_t)k, (uint8_t)ch); k++; } if (!k) return count ? count : -1; if (!supp) count++; any = 1; continue; }
        if (cv == 's') { uint32_t d = supp ? 0 : va32(va); while (n < width && (ch = sg(r)) != EOF) { if (isspace(ch)) { su(r, ch); break; } if (d) WR8(d + (uint32_t)n, (uint8_t)ch); n++; } if (!n) return count ? count : -1; if (d) { WR8(d + (uint32_t)n, 0); count++; } any = 1; continue; }
        if (cv == '[') {
            int neg = 0; char set[256] = { 0 }; if (*f == '^') { neg = 1; f++; } if (*f == ']') { set[(unsigned char)']'] = 1; f++; }
            while (*f && *f != ']') { if (f[1] == '-' && f[2] && f[2] != ']') { for (int q = (unsigned char)f[0]; q <= (unsigned char)f[2]; q++) set[q] = 1; f += 3; } else set[(unsigned char)*f++] = 1; } if (*f == ']') f++;
            uint32_t d = supp ? 0 : va32(va); while (n < width && (ch = sg(r)) != EOF) { if (set[ch] == neg) { su(r, ch); break; } if (d) WR8(d + (uint32_t)n, (uint8_t)ch); n++; }
            if (!n) return count ? count : (any ? count : -1); if (d) { WR8(d + (uint32_t)n, 0); count++; } any = 1; continue;
        }
        while (n < width && n < 500 && (ch = sg(r)) != EOF) {
            int ok = isdigit(ch) || ((ch == '-' || ch == '+') && (n == 0 || (cv != 'd' && cv != 'u' && cv != 'i' && cv != 'x' && cv != 'X' && (buf[n - 1] == 'e' || buf[n - 1] == 'E'))))
                  || ((cv == 'x' || cv == 'X' || cv == 'i') && (isxdigit(ch) || ((ch == 'x' || ch == 'X') && n >= 1)))
                  || ((cv == 'f' || cv == 'e' || cv == 'g' || cv == 'E' || cv == 'G') && (ch == '.' || ch == 'e' || ch == 'E'));
            if (!ok) { su(r, ch); break; } buf[n++] = (char)ch;
        }
        buf[n] = 0; if (!n) return count ? count : (ch == EOF ? -1 : 0); any = 1;
        if (cv == 'f' || cv == 'e' || cv == 'g' || cv == 'E' || cv == 'G') { double v = atof(buf); if (!supp) { uint32_t d = va32(va); if (lng) WR64(d, *(uint64_t *)&v); else { float fv = (float)v; WR32(d, *(uint32_t *)&fv); } count++; } }
        else { long v = (cv == 'x' || cv == 'X') ? (long)strtoul(buf, NULL, 16) : cv == 'i' ? strtol(buf, NULL, 0) : cv == 'o' ? (long)strtoul(buf, NULL, 8) : cv == 'u' ? (long)strtoul(buf, NULL, 10) : strtol(buf, NULL, 10);
               if (!supp) { uint32_t d = va32(va); if (sh) WR16(d, (uint16_t)v); else WR32(d, (uint32_t)v); count++; } }
    }
    return count;
}
SHIM(sscanf_) { Src r = { GP(A(0)), NULL, 0 }; VA v = { c->esp + 12 }; RET((uint32_t)msvc_scan(&r, gs(A(1)), &v)); }
SHIM(fscanf_) { FILE *f = F(A(0)); if (!f) { RET((uint32_t)-1); return; } Src r = { NULL, f, 0 }; VA v = { c->esp + 12 }; RET((uint32_t)msvc_scan(&r, gs(A(1)), &v)); }

/* ---------------------------------------------------------------- C++: operator new/delete, std::exception, std::string (VC9 layout, 28 bytes: _Myproxy, _Bx{buf[16]|ptr}, _Mysize, _Myres) */
SHIM(op_new) { uint32_t p = g_alloc(A(0) ? A(0) : 1); if (!p) port_die("operator new(%u): out of guest memory", A(0)); RET(p); }
SHIM(op_delete) { g_free(A(0)); }
SHIM(exc_ctor_str) { WR32(c->ecx, 0); RET(c->ecx); }
SHIM(exc_dtor) { }
SHIM(exc_what) { static uint32_t s; if (!s) s = g_str("exception"); RET(s); }
SHIM(typeinfo_dtor) { }
#define SS_BUF(s) ((s) + 4)
#define SS_SIZE(s) ((s) + 20)
#define SS_RES(s) ((s) + 24)
static uint32_t ss_data(uint32_t s) { return RD32(SS_RES(s)) >= 16 ? RD32(SS_BUF(s)) : SS_BUF(s); }
static void ss_assign(uint32_t s, const char *p, uint32_t n) {
    if (n >= RD32(SS_RES(s))) {
        uint32_t cap = (n | 15) + 1; uint32_t d = g_alloc(cap); if (RD32(SS_RES(s)) >= 16) g_free(RD32(SS_BUF(s)));
        WR32(SS_BUF(s), d); WR32(SS_RES(s), cap - 1);
    }
    uint32_t d = ss_data(s); memmove(GP(d), p, n); WR8(d + n, 0); WR32(SS_SIZE(s), n);
}
SHIM(ss_ctor0) { uint32_t s = c->ecx; memset(GP(s), 0, 28); WR32(SS_RES(s), 15); RET(s); }
SHIM(ss_ctor_cstr) { uint32_t s = c->ecx; memset(GP(s), 0, 28); WR32(SS_RES(s), 15); const char *p = gs(A(0)); ss_assign(s, p, (uint32_t)strlen(p)); RET(s); }
SHIM(ss_ctor_copy) { uint32_t s = c->ecx, o = A(0); memset(GP(s), 0, 28); WR32(SS_RES(s), 15); ss_assign(s, (const char *)GP(ss_data(o)), RD32(SS_SIZE(o))); RET(s); }
SHIM(ss_dtor) { uint32_t s = c->ecx; if (RD32(SS_RES(s)) >= 16) g_free(RD32(SS_BUF(s))); WR32(SS_RES(s), 15); WR32(SS_SIZE(s), 0); WR8(SS_BUF(s), 0); }
SHIM(ss_assign_cstr) { uint32_t s = c->ecx; const char *p = gs(A(0)); char *tmp = strdup(p); ss_assign(s, tmp, (uint32_t)strlen(tmp)); free(tmp); RET(s); }
SHIM(ss_append_cstr) {
    uint32_t s = c->ecx; const char *p = gs(A(0)); uint32_t l = (uint32_t)strlen(p), n = RD32(SS_SIZE(s)); char *tmp = malloc(n + l + 1);
    memcpy(tmp, GP(ss_data(s)), n); memcpy(tmp + n, p, l); ss_assign(s, tmp, n + l); free(tmp); RET(s);
}
SHIM(ss_begin) { RET(ss_data(c->ecx)); }
SHIM(ss_end) { RET(ss_data(c->ecx) + RD32(SS_SIZE(c->ecx))); }
SHIM(ss_eq) { uint32_t a = A(0), b = A(1); RET(RD32(SS_SIZE(a)) == RD32(SS_SIZE(b)) && !memcmp(GP(ss_data(a)), GP(ss_data(b)), RD32(SS_SIZE(a)))); }
SHIM(ss_lt) { uint32_t a = A(0), b = A(1); uint32_t la = RD32(SS_SIZE(a)), lb = RD32(SS_SIZE(b)); int r = memcmp(GP(ss_data(a)), GP(ss_data(b)), la < lb ? la : lb); RET(r < 0 || (r == 0 && la < lb)); }
SHIM(exc_ctor0) { WR32(c->ecx, 0); RET(c->ecx); }
SHIM(die_seh) { port_die("structured/C++ exception dispatch (%s) was called; exceptions are not supported by this host yet", thunk_name(c->esp)); }
SHIM(beginthreadex_) { port_log("_beginthreadex: threads are not supported by this host yet (the game continues without it)"); RET(0); }
SHIM(alloc_allocate) { RET(g_alloc(A(0))); }
SHIM(alloc_deallocate) { g_free(A(0)); }

#define S(n, p) { #n, sh_##n, p }
#define SA(name, impl, p) { name, sh_##impl, p }
const ShimDef crt_shims[] = {
    SA("_initterm", initterm, CDECL), SA("_initterm_e", initterm_e, CDECL), SA("__getmainargs", getmainargs, CDECL), SA("__p__commode", p_commode, CDECL), SA("__p__fmode", p_fmode, CDECL),
    SA("_errno", errno_, CDECL), SA("__set_app_type", nop, CDECL), SA("__setusermatherr", nop, CDECL), SA("_configthreadlocale", zero, CDECL), SA("_crt_debugger_hook", nop, CDECL),
    SA("_decode_pointer", ident, CDECL), SA("_encode_pointer", ident, CDECL), SA("_onexit", onexit, CDECL), SA("__dllonexit", onexit, CDECL), SA("_lock", nop, CDECL), SA("_unlock", nop, CDECL),
    SA("_exit", exit_, CDECL), SA("exit", exit_, CDECL), SA("_cexit", nop, CDECL), SA("_amsg_exit", die_amsg, CDECL), SA("_invoke_watson", die_watson, CDECL), SA("_XcptFilter", die_xcpt, CDECL),
    SA("_CxxThrowException", die_cxx, STD(2)), SA("?terminate@@YAXXZ", die_terminate, CDECL), SA("_controlfp_s", controlfp_s, CDECL), SA("_heapchk", heapchk, CDECL), SA("_ismbblead", ismbblead, CDECL),
    SA("_invalid_parameter_noinfo", nop, CDECL), SA("_adjust_fdiv", nop, CDECL), SA("_acmdln", nop, CDECL),
    SA("malloc", malloc_, CDECL), SA("calloc", calloc_, CDECL), SA("realloc", realloc_, CDECL), SA("free", free_, CDECL), SA("_msize", msize, CDECL),
    SA("memcpy", memcpy_, CDECL), SA("memmove", memcpy_, CDECL), SA("memset", memset_, CDECL), SA("memchr", memchr_, CDECL), SA("memmove_s", memmove_s, CDECL),
    SA("_strdup", strdup_, CDECL), SA("_stricmp", stricmp_, CDECL), SA("_strnicmp", strnicmp_, CDECL), SA("strncmp", strncmp_, CDECL), SA("_strlwr", strlwr_, CDECL), SA("strncpy", strncpy_, CDECL),
    SA("strncat", strncat_, CDECL), SA("strncpy_s", strncpy_s, CDECL), SA("strncat_s", strncat_s, CDECL), SA("strchr", strchr_, CDECL), SA("strrchr", strrchr_, CDECL), SA("strstr", strstr_, CDECL),
    SA("strpbrk", strpbrk_, CDECL), SA("strcspn", strcspn_, CDECL), SA("strtok", strtok_, CDECL), SA("strtol", strtol_, CDECL), SA("strtoul", strtoul_, CDECL), SA("atoi", atoi_, CDECL), SA("atof", atof_, CDECL),
    SA("_itoa", itoa_, CDECL), SA("_ltoa", itoa_, CDECL), SA("_gcvt", gcvt_, CDECL),
    SA("isupper", isupper_, CDECL), SA("islower", islower_, CDECL), SA("isspace", isspace_, CDECL), SA("isdigit", isdigit_, CDECL), SA("isxdigit", isxdigit_, CDECL), SA("isalpha", isalpha_, CDECL),
    SA("isprint", isprint_, CDECL), SA("isalnum", isalnum_, CDECL), SA("tolower", tolower_, CDECL), SA("toupper", toupper_, CDECL), SA("iswspace", iswspace_, CDECL), SA("iswctype", iswctype_, CDECL),
    SA("getenv", getenv_, CDECL), SA("_putenv", putenv_, CDECL), SA("srand", srand_, CDECL), SA("rand", rand_, CDECL),
    SA("ceil", ceil_, CDECL), SA("floor", floor_, CDECL), SA("ldexp", ldexp_, CDECL), SA("_finite", finite_, CDECL),
    SA("_CIsqrt", CIsqrt, CDECL), SA("_CIsin", CIsin, CDECL), SA("_CIcos", CIcos, CDECL), SA("_CItan", CItan, CDECL), SA("_CIasin", CIasin, CDECL), SA("_CIacos", CIacos, CDECL), SA("_CIatan", CIatan, CDECL),
    SA("_CIexp", CIexp, CDECL), SA("_CIlog", CIlog, CDECL), SA("_CIlog10", CIlog10, CDECL), SA("_CIatan2", CIatan2, CDECL), SA("_CIpow", CIpow, CDECL), SA("_CIfmod", CIfmod, CDECL),
    SA("sprintf", sprintf_, CDECL), SA("sprintf_s", sprintf_s, CDECL), SA("_snprintf", snprintf_, CDECL), SA("_snprintf_s", snprintf_s, CDECL), SA("_vsnprintf", vsnprintf_, CDECL),
    SA("fopen", fopen_, CDECL), SA("fclose", fclose_, CDECL), SA("fread", fread_, CDECL), SA("fwrite", fwrite_, CDECL), SA("fseek", fseek_, CDECL), SA("_fseeki64", fseeki64_, CDECL), SA("ftell", ftell_, CDECL),
    SA("_ftelli64", ftelli64_, CDECL), SA("feof", feof_, CDECL), SA("ferror", ferror_, CDECL), SA("fflush", fflush_, CDECL), SA("setbuf", setbuf_, CDECL), SA("fgets", fgets_, CDECL), SA("fputs", fputs_, CDECL),
    SA("fprintf", fprintf_, CDECL), SA("vfprintf", vfprintf_, CDECL), SA("_fileno", fileno_, CDECL), SA("remove", remove_, CDECL), SA("rename", rename_, CDECL), SA("_mkdir", mkdir_, CDECL), SA("_chdir", chdir_, CDECL),
    SA("_getcwd", getcwd_, CDECL), SA("_getdcwd", getdcwd_, CDECL), SA("_getdrive", getdrive_, CDECL),
    SA("_open", open_, CDECL), SA("_close", close_, CDECL), SA("_read", read_, CDECL), SA("_write", write_, CDECL), SA("_filelength", filelength_, CDECL), SA("_filelengthi64", filelengthi64_, CDECL),
    SA("_umask", umask_, CDECL), SA("_stat64i32", stat64i32_, CDECL), SA("_fstat64i32", fstat64i32_, CDECL), SA("_findfirst64i32", findfirst_, CDECL), SA("_findnext64i32", findnext_, CDECL), SA("_findclose", findclose_, CDECL),
    SA("_time64", time64_, CDECL), SA("_localtime64", localtime64_, CDECL), SA("asctime", asctime_, CDECL), SA("qsort", qsort_, CDECL), SA("bsearch", bsearch_, CDECL),
    SA("sscanf", sscanf_, CDECL), SA("fscanf", fscanf_, CDECL),
    SA("??2@YAPAXI@Z", op_new, CDECL), SA("??3@YAXPAX@Z", op_delete, CDECL), SA("??_V@YAXPAX@Z", op_delete, CDECL),
    SA("??0exception@std@@QAE@ABQBD@Z", exc_ctor_str, STD(1)), SA("??0exception@std@@QAE@ABV01@@Z", exc_ctor_str, STD(1)), SA("??1exception@std@@UAE@XZ", exc_dtor, 0), SA("?what@exception@std@@UBEPBDXZ", exc_what, 0),
    SA("??$?MDU?$char_traits@D@std@@V?$allocator@D@1@@std@@YA_NABV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@0@0@Z", ss_lt, CDECL),
    SA("??0exception@std@@QAE@XZ", exc_ctor0, 0), SA("_except_handler4_common", die_seh, CDECL), SA("__CxxFrameHandler3", die_seh, CDECL),
    SA("_beginthreadex", beginthreadex_, CDECL), SA("_setjmp3", die_seh, CDECL), SA("longjmp", die_seh, CDECL),
    SA("?_type_info_dtor_internal_method@type_info@@QAEXXZ", typeinfo_dtor, 0),
    /* std::string members (thiscall: this in ecx, callee pops its arguments) */
    SA("?begin@?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAE?AV?$_String_iterator@DU?$char_traits@D@std@@V?$allocator@D@2@@2@XZ", ss_begin, 0),
    SA("?end@?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAE?AV?$_String_iterator@DU?$char_traits@D@std@@V?$allocator@D@2@@2@XZ", ss_end, 0),
    SA("??0?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAE@PBD@Z", ss_ctor_cstr, STD(1)),
    SA("??0?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAE@ABV01@@Z", ss_ctor_copy, STD(1)),
    SA("??0?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAE@XZ", ss_ctor0, 0),
    SA("??1?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAE@XZ", ss_dtor, 0),
    SA("??Y?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAEAAV01@PBD@Z", ss_append_cstr, STD(1)),
    SA("??4?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAEAAV01@PBD@Z", ss_assign_cstr, STD(1)),
    SA("??$?8DU?$char_traits@D@std@@V?$allocator@D@1@@std@@YA_NABV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@0@0@Z", ss_eq, CDECL),
    SA("?allocate@?$allocator@D@std@@QAEPADI@Z", alloc_allocate, STD(1)), SA("?deallocate@?$allocator@D@std@@QAEXPADI@Z", alloc_deallocate, STD(2)),
    { 0, 0, 0 }
};
