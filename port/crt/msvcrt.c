/* msvcrt.c: the Microsoft C runtime (MSVCR90) apart from stdio: start-up and exit, memory, strings, character classes,
 * conversions, math (including the x87 _CI helpers), sorting, time, threads and the floating-point control word.
 * Behaviour follows MSVCR90 where programs can tell the difference (rand's sequence, qsort's order of equal elements,
 * _itoa of negative numbers in other bases, _snprintf's truncation, per-thread errno and strtok). */
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>
#include "crt.h"

uint32_t thread_create(uint32_t fn, uint32_t arg, uint32_t stack, int suspended, uint32_t *tid, const char *name);
void thread_exit_current(uint32_t code) __attribute__((noreturn));
uint32_t crt_iob(void);
static double argd(CPU *c, int i) { uint64_t v = ((uint64_t)A(i + 1) << 32) | A(i); double d; memcpy(&d, &v, 8); return d; }

/* ---------------------------------------------------------------- data imports and start-up */
static uint32_t v_acmdln, v_adjust_fdiv, v_fmode, v_commode, v_initenv, v_environ, v_pgmptr, v_mb_cur_max, v_argc, v_argv;
static void init_data(void) {
    if (v_fmode) return;
    v_fmode = g_alloc(4); v_commode = g_alloc(4); v_adjust_fdiv = g_alloc(4); v_mb_cur_max = g_alloc(4); WR32(v_mb_cur_max, 1);
    v_acmdln = g_alloc(4); WR32(v_acmdln, g_str(g_cfg.cmdline));
    v_initenv = g_alloc(4); v_environ = g_alloc(4); { uint32_t env = g_alloc(8); WR32(v_initenv, env); WR32(v_environ, env); }
    v_pgmptr = g_alloc(4); WR32(v_pgmptr, g_str("C:\\SS2.exe"));
    /* argv from the command line: words separated by spaces, "quoted words" kept together */
    uint32_t argv = g_alloc(4 * 64); int argc = 0; const char *p = g_cfg.cmdline;
    while (*p && argc < 63) {
        while (*p == ' ' || *p == '\t') p++; if (!*p) break;
        char w[512]; size_t k = 0; int q = 0;
        while (*p && (q || (*p != ' ' && *p != '\t'))) { if (*p == '"') q = !q; else if (k < sizeof w - 1) w[k++] = *p; p++; }
        w[k] = 0; WR32(argv + 4 * (uint32_t)argc++, g_str(w));
    }
    v_argc = g_alloc(4); WR32(v_argc, (uint32_t)argc); v_argv = g_alloc(4); WR32(v_argv, argv);
}
uint32_t crt_data_import(const char *n) {
    init_data();
    if (!strcmp(n, "_acmdln")) return v_acmdln;
    if (!strcmp(n, "_adjust_fdiv")) return v_adjust_fdiv;
    if (!strcmp(n, "_fmode")) return v_fmode;
    if (!strcmp(n, "_commode")) return v_commode;
    if (!strcmp(n, "__initenv")) return v_initenv;
    if (!strcmp(n, "_environ")) return v_environ;
    if (!strcmp(n, "_pgmptr")) return v_pgmptr;
    if (!strcmp(n, "__mb_cur_max")) return v_mb_cur_max;
    if (!strcmp(n, "_iob")) return crt_iob();
    return 0;
}
int crt_default_text(void) { init_data(); return RD32(v_fmode) != 0x8000; }
SHIM(initterm) { for (uint32_t p = A(0); p < A(1); p += 4) { uint32_t f = RD32(p); if (f) g_call(c, f, 0); } }
SHIM(initterm_e) { for (uint32_t p = A(0); p < A(1); p += 4) { uint32_t f = RD32(p); if (f) { uint32_t r = g_call(c, f, 0); if (r) { RET(r); return; } } } RET(0); }
SHIM(getmainargs) { init_data(); WR32(A(0), RD32(v_argc)); WR32(A(1), RD32(v_argv)); WR32(A(2), RD32(v_environ)); RET(0); }
SHIM(p_fmode) { init_data(); RET(v_fmode); }
SHIM(p_commode) { init_data(); RET(v_commode); }
SHIM(p_argc) { init_data(); RET(v_argc); }
SHIM(p_argv) { init_data(); RET(v_argv); }
SHIM(p_acmdln) { init_data(); RET(v_acmdln); }
SHIM(set_fmode) { init_data(); WR32(v_fmode, A(0)); RET(0); }
SHIM(get_fmode) { init_data(); WR32(A(0), RD32(v_fmode)); RET(0); }
SHIM(errno_) { RET(crt_teb_slot(0)); }
SHIM(get_errno) { WR32(A(0), RD32(crt_teb_slot(0))); RET(0); }
SHIM(zero) { RET(0); }
SHIM(ident) { RET(A(0)); }
SHIM(nop) { }
/* exit: the functions registered with _onexit / atexit / __dllonexit run in reverse order first */
static uint32_t onexit_fns[256]; static int n_onexit, exiting;
SHIM(onexit) { if (n_onexit < 256) onexit_fns[n_onexit++] = A(0); RET(A(0)); }
SHIM(atexit_) { if (n_onexit < 256) onexit_fns[n_onexit++] = A(0); RET(0); }
static void run_onexit(CPU *c) { if (exiting++) return; while (n_onexit > 0) { uint32_t f = onexit_fns[--n_onexit]; if (f) g_call(c, f, 0); } }
SHIM(exit_) { port_log("exit(%d)", (int)A(0)); run_onexit(c); port_exit((int)A(0)); }
SHIM(quick_exit_) { port_log("_exit(%d)", (int)A(0)); port_exit((int)A(0)); }
SHIM(cexit) { run_onexit(c); }
SHIM(abort_) { port_die("abort() was called"); }
SHIM(die_amsg) { port_die("_amsg_exit(%u): the C runtime hit a fatal error", A(0)); }
SHIM(die_watson) { port_die("_invoke_watson: the C runtime's parameter validation failed"); }
SHIM(invalid_param) { static int n; if (!n++) port_warn("_invalid_parameter_noinfo: the game passed an invalid argument to a C runtime function (continuing)"); }
SHIM(die_xcpt) { port_die("an exception reached _XcptFilter"); }
SHIM(purecall) { port_die("pure virtual function call"); }
SHIM(heapchk) { heap_check(); RET((uint32_t)-2); }       /* _HEAPOK */
SHIM(ismbblead) { RET(0); }
SHIM(setlocale_) { static uint32_t s; if (!s) s = g_str("C"); RET(s); }
SHIM(signal_) { RET(0); }
SHIM(getpid_) { RET(0x100); }

/* ---------------------------------------------------------------- the floating-point control word (MSVC encoding <-> x87) */
static uint32_t cw_to_ms(uint16_t cw) {
    uint32_t m = 0;
    if (cw & 0x01) m |= 0x10; if (cw & 0x02) m |= 0x80000; if (cw & 0x04) m |= 0x08; if (cw & 0x08) m |= 0x04; if (cw & 0x10) m |= 0x02; if (cw & 0x20) m |= 0x01;
    m |= (uint32_t)((cw >> 10) & 3) << 8;
    switch ((cw >> 8) & 3) { case 0: m |= 0x20000; break; case 2: m |= 0x10000; break; default: break; }
    return m;
}
static uint16_t ms_to_cw(uint32_t m, uint16_t cw) {
    cw &= (uint16_t)~0x0F3Fu;
    if (m & 0x10) cw |= 0x01; if (m & 0x80000) cw |= 0x02; if (m & 0x08) cw |= 0x04; if (m & 0x04) cw |= 0x08; if (m & 0x02) cw |= 0x10; if (m & 0x01) cw |= 0x20;
    cw |= (uint16_t)(((m >> 8) & 3) << 10);
    cw |= (uint16_t)((m & 0x30000) == 0x20000 ? 0 : (m & 0x30000) == 0x10000 ? 2 << 8 : 3 << 8);
    return cw;
}
static uint32_t controlfp(CPU *c, uint32_t nw, uint32_t mask) {
    uint32_t cur = cw_to_ms(c->cw);
    if (mask) { cur = (cur & ~mask) | (nw & mask); c->cw = ms_to_cw(cur, c->cw); }
    return cw_to_ms(c->cw);
}
SHIM(controlfp_) { RET(controlfp(c, A(0), A(1) & ~0x40000u)); }
SHIM(control87_) { RET(controlfp(c, A(0), A(1))); }
SHIM(controlfp_s) { uint32_t r = controlfp(c, A(1), A(2)); if (A(0)) WR32(A(0), r); RET(0); }
SHIM(clearfp_) { uint32_t s = c->sw & 0x3F; c->sw &= (uint16_t)~0x3Fu; RET(s); }
SHIM(statusfp_) { RET(c->sw & 0x3F); }
SHIM(fpreset_) { c->cw = 0x27f; }

/* ---------------------------------------------------------------- memory */
SHIM(malloc_) { uint32_t p = g_alloc(A(0) ? A(0) : 1); if (!p) crt_set_errno(12); RET(p); }
SHIM(calloc_) { uint64_t n = (uint64_t)A(0) * A(1); RET(n > 0xC0000000u ? 0 : g_alloc(n ? (uint32_t)n : 1)); }
SHIM(realloc_) { if (A(1) == 0) { g_free(A(0)); RET(0); return; } RET(g_realloc(A(0), A(1))); }
SHIM(recalloc_) { uint64_t n = (uint64_t)A(1) * A(2); RET(g_realloc(A(0), (uint32_t)n)); }
SHIM(free_) { g_free(A(0)); }
SHIM(msize) { RET(g_size(A(0))); }
SHIM(expand_) { RET(A(1) <= g_size(A(0)) ? A(0) : 0); }
SHIM(aligned_malloc) { uint32_t al = A(1) < 16 ? 16 : A(1), raw = g_alloc(A(0) + al + 4); if (!raw) { RET(0); return; } uint32_t p = (raw + 4 + al - 1) & ~(al - 1); WR32(p - 4, raw); RET(p); }
SHIM(aligned_free) { if (A(0)) g_free(RD32(A(0) - 4)); }
SHIM(memcpy_) { memmove(GP(A(0)), GP(A(1)), A(2)); RET(A(0)); }
SHIM(memset_) { memset(GP(A(0)), (int)A(1), A(2)); RET(A(0)); }
SHIM(memcmp_) { int r = memcmp(GP(A(0)), GP(A(1)), A(2)); RET((uint32_t)(r < 0 ? -1 : r > 0)); }
SHIM(memicmp_) { const uint8_t *a = GP(A(0)), *b = GP(A(1)); for (uint32_t i = 0; i < A(2); i++) { int d = tolower(a[i]) - tolower(b[i]); if (d) { RET((uint32_t)d); return; } } RET(0); }
SHIM(memchr_) { const uint8_t *s = GP(A(0)); for (uint32_t i = 0; i < A(2); i++) if (s[i] == (uint8_t)A(1)) { RET(A(0) + i); return; } RET(0); }
SHIM(memmove_s) { if (A(3) > A(1)) { RET(34); return; } memmove(GP(A(0)), GP(A(2)), A(3)); RET(0); }
SHIM(memcpy_s) { if (A(3) > A(1)) { memset(GP(A(0)), 0, A(1)); RET(34); return; } memmove(GP(A(0)), GP(A(2)), A(3)); RET(0); }

/* ---------------------------------------------------------------- strings */
static uint32_t gofs(const char *base, uint32_t g, const char *p) { return p ? g + (uint32_t)(p - base) : 0; }
static int ilow(int ch) { return ch >= 'A' && ch <= 'Z' ? ch + 32 : ch; }
SHIM(strlen_) { RET(strlen(gs(A(0)))); }
SHIM(strcpy_) { const char *s = gs(A(1)); memmove(GP(A(0)), s, strlen(s) + 1); RET(A(0)); }
SHIM(strcat_) { char *d = (char *)GP(A(0)); const char *s = gs(A(1)); memmove(d + strlen(d), s, strlen(s) + 1); RET(A(0)); }
SHIM(strcmp_) { int r = strcmp(gs(A(0)), gs(A(1))); RET((uint32_t)(r < 0 ? -1 : r > 0)); }
SHIM(strncmp_) { int r = strncmp(gs(A(0)), gs(A(1)), A(2)); RET((uint32_t)(r < 0 ? -1 : r > 0)); }
SHIM(stricmp_) { const unsigned char *a = (const unsigned char *)gs(A(0)), *b = (const unsigned char *)gs(A(1)); int d; do { d = ilow(*a) - ilow(*b); } while (!d && *a++ && *b++); RET((uint32_t)d); }
SHIM(strnicmp_) { const unsigned char *a = (const unsigned char *)gs(A(0)), *b = (const unsigned char *)gs(A(1)); uint32_t n = A(2); int d = 0; while (n-- && !(d = ilow(*a) - ilow(*b)) && *a) a++, b++; RET((uint32_t)d); }
SHIM(strncpy_) { char *d = (char *)GP(A(0)); const char *s = gs(A(1)); uint32_t n = A(2), i = 0; for (; i < n && s[i]; i++) d[i] = s[i]; for (; i < n; i++) d[i] = 0; RET(A(0)); }
SHIM(strncat_) { char *d = (char *)GP(A(0)); const char *s = gs(A(1)); size_t dl = strlen(d), i = 0; for (; i < A(2) && s[i]; i++) d[dl + i] = s[i]; d[dl + i] = 0; RET(A(0)); }
SHIM(strcpy_s) { const char *s = gs(A(2)); size_t l = strlen(s); if (!A(1) || l >= A(1)) { if (A(1)) WR8(A(0), 0); RET(34); return; } memmove(GP(A(0)), s, l + 1); RET(0); }
SHIM(strcat_s) { char *d = (char *)GP(A(0)); size_t dl = strlen(d), l = strlen(gs(A(2))); if (dl + l >= A(1)) { if (A(1)) WR8(A(0), 0); RET(34); return; } memmove(d + dl, gs(A(2)), l + 1); RET(0); }
SHIM(strncpy_s) {
    uint32_t cnt = A(3), sz = A(1); const char *s = gs(A(2)); size_t l = strlen(s); if (cnt != 0xFFFFFFFFu && l > cnt) l = cnt;
    if (l >= sz) { if (sz) WR8(A(0), 0); RET(cnt == 0xFFFFFFFFu ? 80 : 34); return; } memmove(GP(A(0)), s, l); WR8(A(0) + (uint32_t)l, 0); RET(0);
}
SHIM(strncat_s) {
    char *d = (char *)GP(A(0)); size_t dl = strlen(d), sz = A(1); const char *s = gs(A(2)); size_t l = strlen(s); if (A(3) != 0xFFFFFFFFu && l > A(3)) l = A(3);
    if (dl + l >= sz) { RET(34); return; } memmove(d + dl, s, l); d[dl + l] = 0; RET(0);
}
SHIM(strchr_) { const char *b = gs(A(0)); RET(gofs(b, A(0), strchr(b, (int)(char)A(1)))); }
SHIM(strrchr_) { const char *b = gs(A(0)); RET(gofs(b, A(0), strrchr(b, (int)(char)A(1)))); }
SHIM(strstr_) { const char *b = gs(A(0)); RET(gofs(b, A(0), strstr(b, gs(A(1))))); }
SHIM(strpbrk_) { const char *b = gs(A(0)); RET(gofs(b, A(0), strpbrk(b, gs(A(1))))); }
SHIM(strspn_) { RET(strspn(gs(A(0)), gs(A(1)))); }
SHIM(strcspn_) { RET(strcspn(gs(A(0)), gs(A(1)))); }
SHIM(strdup_) { RET(A(0) ? g_str(gs(A(0))) : 0); }
SHIM(strlwr_) { for (char *s = (char *)GP(A(0)); *s; s++) *s = (char)ilow((unsigned char)*s); RET(A(0)); }
SHIM(strupr_) { for (char *s = (char *)GP(A(0)); *s; s++) if (*s >= 'a' && *s <= 'z') *s -= 32; RET(A(0)); }
SHIM(strrev_) { char *s = (char *)GP(A(0)); size_t n = strlen(s); for (size_t i = 0; i < n / 2; i++) { char t = s[i]; s[i] = s[n - 1 - i]; s[n - 1 - i] = t; } RET(A(0)); }
SHIM(strtok_) {
    uint32_t slot = crt_teb_slot(2), s = A(0) ? A(0) : RD32(slot); const char *d = gs(A(1)); if (!s) { RET(0); return; }
    while (RD8(s) && strchr(d, RD8(s))) s++;
    if (!RD8(s)) { WR32(slot, s); RET(0); return; }
    uint32_t b = s; while (RD8(s) && !strchr(d, RD8(s))) s++;
    if (RD8(s)) { WR8(s, 0); WR32(slot, s + 1); } else WR32(slot, s); RET(b);
}
SHIM(strtok_s) {
    uint32_t ctx = A(2), s = A(0) ? A(0) : RD32(ctx); const char *d = gs(A(1)); if (!s) { RET(0); return; }
    while (RD8(s) && strchr(d, RD8(s))) s++;
    if (!RD8(s)) { WR32(ctx, s); RET(0); return; }
    uint32_t b = s; while (RD8(s) && !strchr(d, RD8(s))) s++;
    if (RD8(s)) { WR8(s, 0); WR32(ctx, s + 1); } else WR32(ctx, s); RET(b);
}
/* character classes: MSVCR90's "C" locale (ASCII only) */
#define CT(name, expr) SHIM(name) { int ch = (int)A(0); RET(ch >= 0 && ch < 128 && (expr)); }
CT(isupper_, isupper(ch)) CT(islower_, islower(ch)) CT(isspace_, isspace(ch)) CT(isdigit_, isdigit(ch)) CT(isxdigit_, isxdigit(ch)) CT(isalpha_, isalpha(ch))
CT(isprint_, isprint(ch)) CT(isalnum_, isalnum(ch)) CT(ispunct_, ispunct(ch)) CT(iscntrl_, iscntrl(ch)) CT(isgraph_, isgraph(ch))
SHIM(tolower_) { int ch = (int)A(0); RET((uint32_t)(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch)); }
SHIM(toupper_) { int ch = (int)A(0); RET((uint32_t)(ch >= 'a' && ch <= 'z' ? ch - 32 : ch)); }
SHIM(iswspace_) { uint32_t w = A(0) & 0xFFFF; RET(w == ' ' || (w >= 9 && w <= 13)); }
SHIM(iswctype_) {
    uint32_t w = A(0) & 0xFFFF, t = A(1); int r = 0; if (w < 128) { int ch = (int)w;
        if ((t & 1) && isupper(ch)) r = 1; if ((t & 2) && islower(ch)) r = 1; if ((t & 4) && isdigit(ch)) r = 1; if ((t & 8) && isspace(ch)) r = 1;
        if ((t & 0x10) && ispunct(ch)) r = 1; if ((t & 0x20) && iscntrl(ch)) r = 1; if ((t & 0x40) && ch == ' ') r = 1; if ((t & 0x80) && isxdigit(ch)) r = 1; if ((t & 0x100) && isalpha(ch)) r = 1; }
    RET(r);
}
/* conversions */
SHIM(strtol_) { char *e; const char *b = gs(A(0)); long long v = strtoll(b, &e, (int)A(2)); if (v > 2147483647ll) { v = 2147483647ll; crt_set_errno(34); } if (v < -2147483648ll) { v = -2147483648ll; crt_set_errno(34); } if (A(1)) WR32(A(1), A(0) + (uint32_t)(e - b)); RET((uint32_t)(int32_t)v); }
SHIM(strtoul_) { char *e; const char *b = gs(A(0)); unsigned long long v = strtoull(b, &e, (int)A(2)); int neg = 0; for (const char *q = b; *q == ' ' || *q == '\t'; q++) ; { const char *q = b; while (isspace((unsigned char)*q)) q++; neg = *q == '-'; }
    if (!neg && v > 0xFFFFFFFFull) { v = 0xFFFFFFFFull; crt_set_errno(34); } if (A(1)) WR32(A(1), A(0) + (uint32_t)(e - b)); RET((uint32_t)v); }
SHIM(strtod_) { char *e; const char *b = gs(A(0)); double v = strtod(b, &e); if (A(1)) WR32(A(1), A(0) + (uint32_t)(e - b)); RETF(v); }
SHIM(atoi_) { RET((uint32_t)(int32_t)strtol(gs(A(0)), NULL, 10)); }
SHIM(atoi64_) { ret64(c, (uint64_t)strtoll(gs(A(0)), NULL, 10)); }
SHIM(atof_) { RETF(strtod(gs(A(0)), NULL)); }
static uint32_t to_radix(uint64_t u, int neg, uint32_t dst, int radix) {
    const char *d = "0123456789abcdefghijklmnopqrstuvwxyz"; char t[72]; int n = 0;
    if (radix < 2 || radix > 36) radix = 10;
    if (!u) t[n++] = '0'; while (u) { t[n++] = d[u % (unsigned)radix]; u /= (unsigned)radix; }
    uint32_t k = 0; if (neg) WR8(dst + k++, '-'); while (n) WR8(dst + k++, (uint8_t)t[--n]); WR8(dst + k, 0); return dst;
}
SHIM(itoa_) { int32_t v = (int32_t)A(0); int r = (int)A(2); RET(r == 10 && v < 0 ? to_radix((uint64_t)-(int64_t)v, 1, A(1), r) : to_radix((uint32_t)v, 0, A(1), r)); }   /* negative only in base 10 */
SHIM(ultoa_) { RET(to_radix(A(0), 0, A(1), (int)A(2))); }
SHIM(i64toa_) { int64_t v = (int64_t)(((uint64_t)A(1) << 32) | A(0)); int r = (int)A(3); RET(r == 10 && v < 0 ? to_radix((uint64_t)-v, 1, A(2), r) : to_radix((uint64_t)v, 0, A(2), r)); }
SHIM(itoa_s) { char t[40]; int32_t v = (int32_t)A(0); int r = (int)A(3); uint32_t tmp = g_alloc(40); if (r == 10 && v < 0) to_radix((uint64_t)-(int64_t)v, 1, tmp, r); else to_radix((uint32_t)v, 0, tmp, r);
    snprintf(t, sizeof t, "%s", gs(tmp)); g_free(tmp); if (strlen(t) + 1 > A(2)) { RET(34); return; } memcpy(GP(A(1)), t, strlen(t) + 1); RET(0); }
SHIM(gcvt_) { char b[64]; snprintf(b, sizeof b, "%.*g", (int)A(2), argd(c, 0)); memcpy(GP(A(3)), b, strlen(b) + 1); RET(A(3)); }
SHIM(getenv_) { RET(0); }
SHIM(putenv_) { RET(0); }
/* rand: MSVC's generator, one seed per thread (kept in the thread's TEB, starting at 1) */
static uint32_t *rand_seed_slot(uint32_t *addr) { *addr = crt_teb_slot(1); return NULL; }
SHIM(srand_) { uint32_t a; rand_seed_slot(&a); WR32(a, A(0)); WR32(crt_teb_slot(3), 1); }
SHIM(rand_) { uint32_t a; rand_seed_slot(&a); uint32_t s = RD32(crt_teb_slot(3)) ? RD32(a) : 1; s = s * 214013u + 2531011u; WR32(a, s); WR32(crt_teb_slot(3), 1); RET((s >> 16) & 0x7FFF); }

/* ---------------------------------------------------------------- math: doubles arrive on the guest stack, the x87 "_CI" forms in st(0)/st(1) */
#define M1(name, f) SHIM(name) { RETF(f(argd(c, 0))); }
M1(ceil_, ceil) M1(floor_, floor) M1(fabs_, fabs) M1(sqrt_, sqrt) M1(sin_, sin) M1(cos_, cos) M1(tan_, tan) M1(asin_, asin) M1(acos_, acos) M1(atan_, atan) M1(exp_, exp) M1(log_, log) M1(log10_, log10)
M1(sinh_, sinh) M1(cosh_, cosh) M1(tanh_, tanh)
SHIM(atan2_) { RETF(atan2(argd(c, 0), argd(c, 2))); }
SHIM(pow_) { RETF(pow(argd(c, 0), argd(c, 2))); }
SHIM(fmod_) { RETF(fmod(argd(c, 0), argd(c, 2))); }
SHIM(hypot_) { RETF(hypot(argd(c, 0), argd(c, 2))); }
SHIM(ldexp_) { RETF(ldexp(argd(c, 0), (int)A(2))); }
SHIM(frexp_) { int e; double m = frexp(argd(c, 0), &e); WR32(A(2), (uint32_t)e); RETF(m); }
SHIM(modf_) { double ip; double f = modf(argd(c, 0), &ip); uint64_t r; memcpy(&r, &ip, 8); WR64(A(2), r); RETF(f); }
SHIM(finite_) { RET(isfinite(argd(c, 0)) != 0); }
SHIM(isnan_) { RET(isnan(argd(c, 0)) != 0); }
SHIM(copysign_) { RETF(copysign(argd(c, 0), argd(c, 2))); }
SHIM(abs_) { int32_t v = (int32_t)A(0); RET((uint32_t)(v < 0 ? -v : v)); }
SHIM(labs_) { int32_t v = (int32_t)A(0); RET((uint32_t)(v < 0 ? -v : v)); }
SHIM(ftol_) { int64_t v = (int64_t)trunc(FPOP(c)); ret64(c, (uint64_t)v); }
#define CI1(n, f) SHIM(n) { ST(0) = f(ST(0)); }
CI1(CIsqrt, sqrt) CI1(CIsin, sin) CI1(CIcos, cos) CI1(CItan, tan) CI1(CIasin, asin) CI1(CIacos, acos) CI1(CIatan, atan) CI1(CIexp, exp) CI1(CIlog, log) CI1(CIlog10, log10)
CI1(CIsinh, sinh) CI1(CIcosh, cosh) CI1(CItanh, tanh)
SHIM(CIatan2) { double x = ST(0), y = ST(1); FPOP(c); ST(0) = atan2(y, x); }
SHIM(CIpow) { double e = ST(0), b = ST(1); FPOP(c); ST(0) = pow(b, e); }
SHIM(CIfmod) { double d = ST(0), n = ST(1); FPOP(c); ST(0) = fmod(n, d); }

/* ---------------------------------------------------------------- qsort and bsearch, MSVCR90's algorithms (same order for equal elements) */
static void gswap(uint32_t a, uint32_t b, uint32_t w) { if (a == b) return; uint8_t t[256]; while (w) { uint32_t k = w > 256 ? 256 : w; memcpy(t, GP(a), k); memmove(GP(a), GP(b), k); memcpy(GP(b), t, k); a += k; b += k; w -= k; } }
static int gcmp(CPU *c, uint32_t fn, uint32_t a, uint32_t b) { return (int32_t)g_call(c, fn, 2, a, b); }
static void shortsort(CPU *c, uint32_t lo, uint32_t hi, uint32_t w, uint32_t fn) {
    while (hi > lo) { uint32_t max = lo; for (uint32_t p = lo + w; p <= hi; p += w) if (gcmp(c, fn, p, max) > 0) max = p; gswap(max, hi, w); hi -= w; }
}
SHIM(qsort_) {
    uint32_t base = A(0), num = A(1), w = A(2), fn = A(3);
    if (num < 2 || !w) return;
    uint32_t lostk[30], histk[30]; int sp = 0;
    uint32_t lo = base, hi = base + w * (num - 1), mid, loguy, higuy, size;
recurse:
    size = (hi - lo) / w + 1;
    if (size <= 8) shortsort(c, lo, hi, w, fn);
    else {
        mid = lo + (size / 2) * w;
        if (gcmp(c, fn, lo, mid) > 0) gswap(lo, mid, w);
        if (gcmp(c, fn, lo, hi) > 0) gswap(lo, hi, w);
        if (gcmp(c, fn, mid, hi) > 0) gswap(mid, hi, w);
        loguy = lo; higuy = hi;
        for (;;) {
            if (mid > loguy) { do { loguy += w; } while (loguy < mid && gcmp(c, fn, loguy, mid) <= 0); }
            if (mid <= loguy) { do { loguy += w; } while (loguy <= hi && gcmp(c, fn, loguy, mid) <= 0); }
            do { higuy -= w; } while (higuy > mid && gcmp(c, fn, higuy, mid) > 0);
            if (higuy < loguy) break;
            gswap(loguy, higuy, w);
            if (mid == higuy) mid = loguy;
        }
        higuy += w;
        if (mid < higuy) { do { higuy -= w; } while (higuy > mid && gcmp(c, fn, higuy, mid) == 0); }
        if (mid >= higuy) { do { higuy -= w; } while (higuy > lo && gcmp(c, fn, higuy, mid) == 0); }
        if (higuy - lo >= hi - loguy) {
            if (lo < higuy) { lostk[sp] = lo; histk[sp] = higuy; ++sp; }
            if (loguy < hi) { lo = loguy; goto recurse; }
        } else {
            if (loguy < hi) { lostk[sp] = loguy; histk[sp] = hi; ++sp; }
            if (lo < higuy) { hi = higuy; goto recurse; }
        }
    }
    if (--sp >= 0) { lo = lostk[sp]; hi = histk[sp]; goto recurse; }
}
SHIM(bsearch_) {
    uint32_t key = A(0), lo = A(1), num = A(2), w = A(3), fn = A(4), hi = lo + (num ? num - 1 : 0) * w;
    while (lo <= hi) {
        uint32_t half = num / 2;
        if (half) {
            uint32_t mid = lo + (num & 1 ? half : half - 1) * w; int r = gcmp(c, fn, key, mid);
            if (!r) { RET(mid); return; }
            if (r < 0) { if (mid < w) break; hi = mid - w; num = num & 1 ? half : half - 1; } else { lo = mid + w; num = half; }
        } else if (num) { RET(gcmp(c, fn, key, lo) ? 0 : lo); return; }
        else break;
    }
    RET(0);
}

/* ---------------------------------------------------------------- time */
static int64_t now_s(void) { return plat_wall_time_ns() / 1000000000; }
SHIM(time64_) { int64_t t = now_s(); if (A(0)) WR64(A(0), (uint64_t)t); ret64(c, (uint64_t)t); }
SHIM(time32_) { int64_t t = now_s(); if (A(0)) WR32(A(0), (uint32_t)t); RET((uint32_t)t); }
static const int mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
static int leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static void put_tm(uint32_t tm, int64_t t) {      /* seconds since 1970 -> struct tm (9 ints) */
    int64_t days = t / 86400, rem = t % 86400; if (rem < 0) { rem += 86400; days--; }
    int wday = (int)((days + 4) % 7); if (wday < 0) wday += 7;
    int y = 1970; for (;;) { int dy = leap(y) ? 366 : 365; if (days >= 0 && days < dy) break; if (days < 0) { y--; days += leap(y) ? 366 : 365; } else { days -= dy; y++; } }
    int yday = (int)days, m = 0; for (;; m++) { int dm = mdays[m] + (m == 1 && leap(y)); if (days < dm) break; days -= dm; }
    int32_t v[9] = { (int32_t)(rem % 60), (int32_t)(rem / 60 % 60), (int32_t)(rem / 3600), (int32_t)days + 1, m, y - 1900, wday, yday, 0 };
    for (int i = 0; i < 9; i++) WR32(tm + 4 * (uint32_t)i, (uint32_t)v[i]);
}
static int64_t get_tm(uint32_t tm) {
    int32_t sec = (int32_t)RD32(tm), min = (int32_t)RD32(tm + 4), hour = (int32_t)RD32(tm + 8), mday = (int32_t)RD32(tm + 12), mon = (int32_t)RD32(tm + 16), year = (int32_t)RD32(tm + 20) + 1900;
    year += mon / 12; mon %= 12; if (mon < 0) { mon += 12; year--; }
    int64_t days = 0; for (int y = 1970; y < year; y++) days += leap(y) ? 366 : 365; for (int y = year; y < 1970; y++) days -= leap(y) ? 366 : 365;
    for (int m = 0; m < mon; m++) days += mdays[m] + (m == 1 && leap(year));
    return ((days + mday - 1) * 24 + hour) * 3600 + (int64_t)min * 60 + sec;
}
static uint32_t tm_buf(void) { static uint32_t b; if (!b) b = g_alloc(36); return b; }
SHIM(localtime64_) { if (!A(0)) { RET(0); return; } put_tm(tm_buf(), (int64_t)RD64(A(0)) + (int64_t)plat_utc_offset_minutes() * 60); RET(tm_buf()); }
SHIM(gmtime64_) { if (!A(0)) { RET(0); return; } put_tm(tm_buf(), (int64_t)RD64(A(0))); RET(tm_buf()); }
SHIM(localtime32_) { if (!A(0)) { RET(0); return; } put_tm(tm_buf(), (int32_t)RD32(A(0)) + (int64_t)plat_utc_offset_minutes() * 60); RET(tm_buf()); }
SHIM(mktime64_) { int64_t t = get_tm(A(0)) - (int64_t)plat_utc_offset_minutes() * 60; put_tm(A(0), t + (int64_t)plat_utc_offset_minutes() * 60); ret64(c, (uint64_t)t); }
SHIM(asctime_) {
    static uint32_t s; static const char *wd[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" }, *mn[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    if (!s) s = g_alloc(32); uint32_t t = A(0); char b[64];
    snprintf(b, sizeof b, "%s %s %2d %02d:%02d:%02d %d\n", wd[RD32(t + 24) % 7], mn[RD32(t + 16) % 12], (int)RD32(t + 12), (int)RD32(t + 8), (int)RD32(t + 4), (int)RD32(t), (int)RD32(t + 20) + 1900);
    memcpy(GP(s), b, strlen(b) + 1); RET(s);
}
SHIM(clock_) { static uint64_t t0; uint64_t t = plat_time_ns(); if (!t0) t0 = t; RET((uint32_t)((t - t0) / 1000000u)); }
SHIM(ftime64_) { int64_t ns = plat_wall_time_ns(); WR64(A(0), (uint64_t)(ns / 1000000000)); WR16(A(0) + 8, (uint16_t)(ns / 1000000 % 1000)); WR16(A(0) + 10, (uint16_t)-plat_utc_offset_minutes()); WR16(A(0) + 12, 0); }
SHIM(strdate_) { int64_t t = now_s() + plat_utc_offset_minutes() * 60; uint32_t tm = g_alloc(36); put_tm(tm, t); char b[16]; snprintf(b, sizeof b, "%02u/%02u/%02u", RD32(tm + 16) + 1, RD32(tm + 12), RD32(tm + 20) % 100); g_free(tm); memcpy(GP(A(0)), b, 9); RET(A(0)); }
SHIM(strtime_) { int64_t t = now_s() + plat_utc_offset_minutes() * 60; uint32_t tm = g_alloc(36); put_tm(tm, t); char b[16]; snprintf(b, sizeof b, "%02u:%02u:%02u", RD32(tm + 8), RD32(tm + 4), RD32(tm)); g_free(tm); memcpy(GP(A(0)), b, 9); RET(A(0)); }

/* ---------------------------------------------------------------- threads */
SHIM(beginthreadex_) { uint32_t tid = 0, h = thread_create(A(2), A(3), A(1), (A(4) & 4) != 0, &tid, "_beginthreadex"); if (A(5)) WR32(A(5), tid); if (!h) crt_set_errno(11); RET(h); }
SHIM(beginthread_) { uint32_t h = thread_create(A(0), A(2), A(1), 0, NULL, "_beginthread"); RET(h ? h : 0xFFFFFFFFu); }
SHIM(endthreadex_) { thread_exit_current(A(0)); }
SHIM(endthread_) { thread_exit_current(0); }

/* ---------------------------------------------------------------- formatted output into strings (format.c does the work) */
static int put_trunc(uint32_t dst, uint32_t cap, const char *s, int len) {     /* _snprintf: no terminator when it does not fit, -1 if truncated */
    if ((uint32_t)len < cap) { memcpy(GP(dst), s, (size_t)len + 1); return len; }
    memcpy(GP(dst), s, cap); return (uint32_t)len == cap ? len : -1;
}
SHIM(sprintf_) { char *o; int n = crt_format(&o, gs(A(1)), c->esp + 12, 0); memcpy(GP(A(0)), o, (size_t)n + 1); free(o); RET(n); }
SHIM(vsprintf_) { char *o; int n = crt_format(&o, gs(A(1)), A(2), 0); memcpy(GP(A(0)), o, (size_t)n + 1); free(o); RET(n); }
SHIM(snprintf_) { char *o; int n = crt_format(&o, gs(A(2)), c->esp + 16, 0); int r = put_trunc(A(0), A(1), o, n); free(o); RET(r); }
SHIM(vsnprintf_) { char *o; int n = crt_format(&o, gs(A(2)), A(3), 0); int r = put_trunc(A(0), A(1), o, n); free(o); RET(r); }
static int secure_put(uint32_t dst, uint32_t sz, uint32_t cnt, const char *o, int n) {      /* the _s variants: always terminated */
    if (!sz) return -1;
    if (cnt == 0xFFFFFFFFu) { if ((uint32_t)n < sz) { memcpy(GP(dst), o, (size_t)n + 1); return n; } memcpy(GP(dst), o, sz - 1); WR8(dst + sz - 1, 0); return -1; }
    uint32_t m = (uint32_t)n < cnt ? (uint32_t)n : cnt; if (m >= sz) { WR8(dst, 0); return -1; } memcpy(GP(dst), o, m); WR8(dst + m, 0); return (uint32_t)n > cnt ? -1 : (int)m;
}
SHIM(sprintf_s) { char *o; int n = crt_format(&o, gs(A(2)), c->esp + 16, 0); int r = (uint32_t)n < A(1) ? (memcpy(GP(A(0)), o, (size_t)n + 1), n) : (A(1) ? (WR8(A(0), 0), -1) : -1); free(o); RET(r); }
SHIM(vsprintf_s) { char *o; int n = crt_format(&o, gs(A(2)), A(3), 0); int r = (uint32_t)n < A(1) ? (memcpy(GP(A(0)), o, (size_t)n + 1), n) : (A(1) ? (WR8(A(0), 0), -1) : -1); free(o); RET(r); }
SHIM(snprintf_s) { char *o; int n = crt_format(&o, gs(A(3)), c->esp + 20, 0); int r = secure_put(A(0), A(1), A(2), o, n); free(o); RET(r); }
SHIM(vsnprintf_s) { char *o; int n = crt_format(&o, gs(A(3)), A(4), 0); int r = secure_put(A(0), A(1), A(2), o, n); free(o); RET(r); }
SHIM(scprintf_) { char *o; int n = crt_format(&o, gs(A(0)), c->esp + 8, 0); free(o); RET(n); }
SHIM(vscprintf_) { char *o; int n = crt_format(&o, gs(A(0)), A(1), 0); free(o); RET(n); }
SHIM(sscanf_) { RET((uint32_t)crt_sscanf(gs(A(0)), gs(A(1)), c->esp + 12)); }
SHIM(wsprintfA_) { char *o; int n = crt_format(&o, gs(A(1)), c->esp + 12, 0); if (n > 1023) n = 1023; memcpy(GP(A(0)), o, (size_t)n); WR8(A(0) + (uint32_t)n, 0); free(o); RET(n); }
SHIM(wvsprintfA_) { char *o; int n = crt_format(&o, gs(A(1)), A(2), 0); if (n > 1023) n = 1023; memcpy(GP(A(0)), o, (size_t)n); WR8(A(0) + (uint32_t)n, 0); free(o); RET(n); }

const ShimDef msvcrt_shims[] = {
    SA("_initterm", initterm, CDECL), SA("_initterm_e", initterm_e, CDECL), SA("__getmainargs", getmainargs, CDECL), SA("__p__fmode", p_fmode, CDECL), SA("__p__commode", p_commode, CDECL),
    SA("__p___argc", p_argc, CDECL), SA("__p___argv", p_argv, CDECL), SA("__p__acmdln", p_acmdln, CDECL), SA("_set_fmode", set_fmode, CDECL), SA("_get_fmode", get_fmode, CDECL),
    SA("_errno", errno_, CDECL), SA("_get_errno", get_errno, CDECL), SA("__set_app_type", nop, CDECL), SA("__setusermatherr", nop, CDECL), SA("_configthreadlocale", zero, CDECL),
    SA("_crt_debugger_hook", nop, CDECL), SA("_decode_pointer", ident, CDECL), SA("_encode_pointer", ident, CDECL), SA("_encoded_null", zero, CDECL), SA("_onexit", onexit, CDECL),
    SA("__dllonexit", onexit, CDECL), SA("atexit", atexit_, CDECL), SA("_lock", nop, CDECL), SA("_unlock", nop, CDECL), SA("_exit", quick_exit_, CDECL), SA("exit", exit_, CDECL),
    SA("_cexit", cexit, CDECL), SA("_c_exit", nop, CDECL), SA("abort", abort_, CDECL), SA("_amsg_exit", die_amsg, CDECL), SA("_invoke_watson", die_watson, CDECL),
    SA("_invalid_parameter_noinfo", invalid_param, CDECL), SA("_XcptFilter", die_xcpt, CDECL), SA("_purecall", purecall, CDECL), SA("_heapchk", heapchk, CDECL),
    SA("_ismbblead", ismbblead, CDECL), SA("setlocale", setlocale_, CDECL), SA("signal", signal_, CDECL), SA("_getpid", getpid_, CDECL),
    SA("_controlfp", controlfp_, CDECL), SA("_control87", control87_, CDECL), SA("_controlfp_s", controlfp_s, CDECL), SA("_clearfp", clearfp_, CDECL), SA("_statusfp", statusfp_, CDECL), SA("_fpreset", fpreset_, CDECL),
    SA("malloc", malloc_, CDECL), SA("calloc", calloc_, CDECL), SA("realloc", realloc_, CDECL), SA("_recalloc", recalloc_, CDECL), SA("free", free_, CDECL), SA("_msize", msize, CDECL),
    SA("_expand", expand_, CDECL), SA("_aligned_malloc", aligned_malloc, CDECL), SA("_aligned_free", aligned_free, CDECL), SA("_malloc_crt", malloc_, CDECL), SA("_calloc_crt", calloc_, CDECL),
    SA("_realloc_crt", realloc_, CDECL),
    SA("memcpy", memcpy_, CDECL), SA("memmove", memcpy_, CDECL), SA("memset", memset_, CDECL), SA("memcmp", memcmp_, CDECL), SA("_memicmp", memicmp_, CDECL), SA("memchr", memchr_, CDECL),
    SA("memmove_s", memmove_s, CDECL), SA("memcpy_s", memcpy_s, CDECL),
    SA("strlen", strlen_, CDECL), SA("strcpy", strcpy_, CDECL), SA("strcat", strcat_, CDECL), SA("strcmp", strcmp_, CDECL), SA("strncmp", strncmp_, CDECL), SA("_stricmp", stricmp_, CDECL),
    SA("_strcmpi", stricmp_, CDECL), SA("_strnicmp", strnicmp_, CDECL), SA("strncpy", strncpy_, CDECL), SA("strncat", strncat_, CDECL), SA("strcpy_s", strcpy_s, CDECL), SA("strcat_s", strcat_s, CDECL),
    SA("strncpy_s", strncpy_s, CDECL), SA("strncat_s", strncat_s, CDECL), SA("strchr", strchr_, CDECL), SA("strrchr", strrchr_, CDECL), SA("strstr", strstr_, CDECL), SA("strpbrk", strpbrk_, CDECL),
    SA("strspn", strspn_, CDECL), SA("strcspn", strcspn_, CDECL), SA("_strdup", strdup_, CDECL), SA("_strlwr", strlwr_, CDECL), SA("_strupr", strupr_, CDECL), SA("_strrev", strrev_, CDECL),
    SA("strtok", strtok_, CDECL), SA("strtok_s", strtok_s, CDECL), SA("_mbschr", strchr_, CDECL), SA("_mbsrchr", strrchr_, CDECL), SA("_mbsstr", strstr_, CDECL), SA("_mbsicmp", stricmp_, CDECL),
    SA("isupper", isupper_, CDECL), SA("islower", islower_, CDECL), SA("isspace", isspace_, CDECL), SA("isdigit", isdigit_, CDECL), SA("isxdigit", isxdigit_, CDECL), SA("isalpha", isalpha_, CDECL),
    SA("isprint", isprint_, CDECL), SA("isalnum", isalnum_, CDECL), SA("ispunct", ispunct_, CDECL), SA("iscntrl", iscntrl_, CDECL), SA("isgraph", isgraph_, CDECL), SA("tolower", tolower_, CDECL),
    SA("toupper", toupper_, CDECL), SA("_tolower", tolower_, CDECL), SA("_toupper", toupper_, CDECL), SA("iswspace", iswspace_, CDECL), SA("iswctype", iswctype_, CDECL),
    SA("strtol", strtol_, CDECL), SA("strtoul", strtoul_, CDECL), SA("strtod", strtod_, CDECL), SA("atoi", atoi_, CDECL), SA("atol", atoi_, CDECL), SA("_atoi64", atoi64_, CDECL), SA("atof", atof_, CDECL),
    SA("_itoa", itoa_, CDECL), SA("_ltoa", itoa_, CDECL), SA("_ultoa", ultoa_, CDECL), SA("_i64toa", i64toa_, CDECL), SA("_itoa_s", itoa_s, CDECL), SA("_gcvt", gcvt_, CDECL),
    SA("getenv", getenv_, CDECL), SA("_putenv", putenv_, CDECL), SA("srand", srand_, CDECL), SA("rand", rand_, CDECL),
    SA("ceil", ceil_, CDECL), SA("floor", floor_, CDECL), SA("fabs", fabs_, CDECL), SA("sqrt", sqrt_, CDECL), SA("sin", sin_, CDECL), SA("cos", cos_, CDECL), SA("tan", tan_, CDECL),
    SA("asin", asin_, CDECL), SA("acos", acos_, CDECL), SA("atan", atan_, CDECL), SA("exp", exp_, CDECL), SA("log", log_, CDECL), SA("log10", log10_, CDECL), SA("sinh", sinh_, CDECL),
    SA("cosh", cosh_, CDECL), SA("tanh", tanh_, CDECL), SA("atan2", atan2_, CDECL), SA("pow", pow_, CDECL), SA("fmod", fmod_, CDECL), SA("_hypot", hypot_, CDECL), SA("ldexp", ldexp_, CDECL),
    SA("frexp", frexp_, CDECL), SA("modf", modf_, CDECL), SA("_finite", finite_, CDECL), SA("_isnan", isnan_, CDECL), SA("_copysign", copysign_, CDECL), SA("abs", abs_, CDECL), SA("labs", labs_, CDECL),
    SA("_ftol", ftol_, CDECL), SA("_ftol2", ftol_, CDECL), SA("_ftol2_sse", ftol_, CDECL),
    SA("_CIsqrt", CIsqrt, CDECL), SA("_CIsin", CIsin, CDECL), SA("_CIcos", CIcos, CDECL), SA("_CItan", CItan, CDECL), SA("_CIasin", CIasin, CDECL), SA("_CIacos", CIacos, CDECL), SA("_CIatan", CIatan, CDECL),
    SA("_CIexp", CIexp, CDECL), SA("_CIlog", CIlog, CDECL), SA("_CIlog10", CIlog10, CDECL), SA("_CIsinh", CIsinh, CDECL), SA("_CIcosh", CIcosh, CDECL), SA("_CItanh", CItanh, CDECL),
    SA("_CIatan2", CIatan2, CDECL), SA("_CIpow", CIpow, CDECL), SA("_CIfmod", CIfmod, CDECL),
    SA("qsort", qsort_, CDECL), SA("bsearch", bsearch_, CDECL),
    SA("_time64", time64_, CDECL), SA("_time32", time32_, CDECL), SA("time", time32_, CDECL), SA("_localtime64", localtime64_, CDECL), SA("_gmtime64", gmtime64_, CDECL), SA("_localtime32", localtime32_, CDECL),
    SA("_mktime64", mktime64_, CDECL), SA("asctime", asctime_, CDECL), SA("clock", clock_, CDECL), SA("_ftime64", ftime64_, CDECL), SA("_strdate", strdate_, CDECL), SA("_strtime", strtime_, CDECL),
    SA("_beginthreadex", beginthreadex_, CDECL), SA("_beginthread", beginthread_, CDECL), SA("_endthreadex", endthreadex_, CDECL), SA("_endthread", endthread_, CDECL),
    SA("sprintf", sprintf_, CDECL), SA("vsprintf", vsprintf_, CDECL), SA("_snprintf", snprintf_, CDECL), SA("_vsnprintf", vsnprintf_, CDECL), SA("sprintf_s", sprintf_s, CDECL),
    SA("vsprintf_s", vsprintf_s, CDECL), SA("_snprintf_s", snprintf_s, CDECL), SA("_vsnprintf_s", vsnprintf_s, CDECL), SA("_scprintf", scprintf_, CDECL), SA("_vscprintf", vscprintf_, CDECL),
    SA("sscanf", sscanf_, CDECL), SA("wsprintfA", wsprintfA_, CDECL), SA("wvsprintfA", wvsprintfA_, STD(3)),
    { 0, 0, 0 }
};
