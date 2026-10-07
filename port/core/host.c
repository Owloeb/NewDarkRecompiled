/* host.c: start-up, guest address space, thunks, calls between host and guest, faults.
 *
 * The recompiled game is C that addresses memory as M + guest address (built without RT_IDENTITY) and calls out through
 * rt_call_import / rt_call_external for everything the operating system used to do. This file loads the user's own
 * executable into guest memory, binds every import to a shim, sets up the main thread and runs the recompiled entry
 * point. It is the same on every platform: plat.h is its only window to the outside. */
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include "core.h"

/* ---------------------------------------------------------------- generated tables (lifter output, host/gen_hostdata.py) */
typedef struct { const char *dll, *name; } ImpDef;
extern const ImpDef hd_imports[]; extern const unsigned hd_nimports; extern const uint32_t hd_iat_slot[];
extern const uint32_t hd_entry, hd_base, hd_exe_size, hd_exe_crc;
void nd_call(CPU *c, uint32_t t);
guest_fn nd_lookup(uint32_t va);
typedef struct { uint32_t va; const char *name; } SymEnt;              /* optional: host/gen_symtab.py */
extern const SymEnt nd_symtab[] __attribute__((weak)); extern const unsigned nd_symtab_n __attribute__((weak));

uint8_t *M;
uint64_t g_space;
PortConfig g_cfg;
int port_verbose, port_trace;
uint32_t g_frames;

/* ---------------------------------------------------------------- logging */
static void vlog(PlatLogLevel lv, const char *prefix, const char *fmt, va_list ap) {
    char b[2048]; int n = snprintf(b, sizeof b, "%s", prefix); vsnprintf(b + n, sizeof b - (size_t)n, fmt, ap); plat_log_write(lv, b);
}
void port_log(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vlog(PLAT_LOG_INFO, "[port] ", fmt, ap); va_end(ap); }
void port_warn(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vlog(PLAT_LOG_WARN, "[port] warning: ", fmt, ap); va_end(ap); }
void port_debug(const char *fmt, ...) { if (!port_verbose) return; va_list ap; va_start(ap, fmt); vlog(PLAT_LOG_DEBUG, "[port] ", fmt, ap); va_end(ap); }
static const char *ring_names[16]; static uint32_t ring_ret[16]; static unsigned ring_n;
const char *guest_symbol(uint32_t va) {
    if (!nd_symtab || !&nd_symtab_n) return NULL;
    unsigned lo = 0, hi = nd_symtab_n;
    while (lo < hi) { unsigned mid = (lo + hi) / 2; if (nd_symtab[mid].va < va) lo = mid + 1; else hi = mid; }
    return lo < nd_symtab_n && nd_symtab[lo].va == va ? nd_symtab[lo].name : NULL;
}
/* --trace keeps the last TRACE_N calls in memory (never streams: a spinning game would fill the disk) and prints them on a
   crash, on SIGINT / SIGTERM (Ctrl-C, `timeout`), or at exit. */
#define TRACE_N 4096
#define TRACE_HEAD 8192             /* also keep the first calls: the start of a loop matters as much as its end */
static char trace_ring[TRACE_N][220], trace_head[TRACE_HEAD][220]; static unsigned trace_n;
static char trace_only[512];        /* --trace-only a,b,c: trace just these functions (",name," match) */
static int trace_wanted(const char *name) {
    if (!trace_only[0]) return 1;
    char k[96]; snprintf(k, sizeof k, ",%s,", name); return strstr(trace_only, k) != NULL;
}
static void trace_put(const char *fmt, ...) {
    unsigned n = __atomic_fetch_add(&trace_n, 1, __ATOMIC_RELAXED);
    char *d = n < TRACE_HEAD ? trace_head[n] : trace_ring[n % TRACE_N];
    va_list ap; va_start(ap, fmt); vsnprintf(d, sizeof trace_ring[0], fmt, ap); va_end(ap);
}
static void trace_dump(void) {
    unsigned n = __atomic_load_n(&trace_n, __ATOMIC_RELAXED); if (!port_trace || !n) return;
    unsigned h = n < TRACE_HEAD ? n : TRACE_HEAD;
    port_log("---- first %u traced calls ----", h);
    for (unsigned i = 0; i < h; i++) plat_log_write(PLAT_LOG_INFO, trace_head[i]);
    if (n <= TRACE_HEAD) return;
    unsigned from = n - TRACE_N > TRACE_HEAD ? n - TRACE_N : TRACE_HEAD;
    if (from > TRACE_HEAD) port_log("---- %u calls not kept ----", from - TRACE_HEAD);
    port_log("---- last %u traced calls (of %u) ----", n - from, n);
    for (unsigned i = from; i < n; i++) plat_log_write(PLAT_LOG_INFO, trace_ring[i % TRACE_N]);
}
static void dump_state(void) {
    CPU *c = cur_cpu(); if (!c) return;
    char b[1024]; int n;
    port_log("  guest esp=%08x ebp=%08x eax=%08x ecx=%08x edx=%08x ebx=%08x esi=%08x edi=%08x (thread %u)", c->esp, c->ebp, c->eax, c->ecx, c->edx, c->ebx, c->esi, c->edi, cur_thread_id());
    n = snprintf(b, sizeof b, "  last host calls:"); for (unsigned i = ring_n > 12 ? ring_n - 12 : 0; i < ring_n && n < 900; i++) n += snprintf(b + n, sizeof b - (size_t)n, " %s@%x", ring_names[i & 15], ring_ret[i & 15]); port_log("%s", b);
    n = snprintf(b, sizeof b, "  frames (ebp chain):"); uint32_t bp = c->ebp;
    for (int i = 0; i < 12 && g_valid(bp, 8) && n < 900; i++) { uint32_t ra = RD32(bp + 4); const char *s = guest_symbol(ra); n += snprintf(b + n, sizeof b - (size_t)n, " %08x%s%s", ra, s ? "=" : "", s ? s : ""); uint32_t nb = RD32(bp); if (nb <= bp) break; bp = nb; }
    port_log("%s", b);
    if (g_valid(c->esp, 48)) { n = snprintf(b, sizeof b, "  guest stack:"); for (int i = 0; i < 12; i++) n += snprintf(b + n, sizeof b - (size_t)n, " %08x", RD32(c->esp + 4 * (uint32_t)i)); port_log("%s", b); }
}
static void on_signal(int sig) {          /* Ctrl-C or timeout: show where the game was, then stop */
    port_log("stopped by signal %d", sig); dump_state(); trace_dump(); _Exit(3);
}
void port_exit(int code) { trace_dump(); plat_audio_close(); plat_video_close(); exit(code); }
void port_die(const char *fmt, ...) {
    char b[1024]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    plat_log_write(PLAT_LOG_ERROR, "[port] FATAL: "); plat_log_write(PLAT_LOG_ERROR, b);
    dump_state(); trace_dump(); plat_alert("System Shock 2", b); port_exit(2);
}
void port_miss(const char *what, const char *guest) { static int n; if (n++ < (port_verbose ? 400 : 60)) port_log("not found (%s): \"%s\"", what, guest); }

/* ---------------------------------------------------------------- guest address space: 64 KB regions */
#define VM_GRAN 0x10000u
static uint32_t *vm_len;       /* per 64 KB page: region length in pages at the region's first page, 0 elsewhere */
static uint8_t *vm_used;       /* per page: in use */
static uint32_t vm_pages;
static int vm_free_range(uint32_t p, uint32_t n) { if (p + n > vm_pages) return 0; for (uint32_t i = 0; i < n; i++) if (vm_used[p + i]) return 0; return 1; }
static void vm_take(uint32_t p, uint32_t n) { memset(vm_used + p, 1, n); vm_len[p] = n; }
static int vm_quiet;
uint32_t vm_alloc_quiet(uint32_t size) { vm_quiet = 1; uint32_t a = vm_alloc(size, 0, "heap arena"); vm_quiet = 0; return a; }
uint32_t vm_alloc(uint32_t size, uint32_t want, const char *what) {
    uint32_t n = (uint32_t)(((uint64_t)size + VM_GRAN - 1) / VM_GRAN); if (!n) n = 1;
    if (want) { uint32_t p = want / VM_GRAN; if (want % VM_GRAN == 0 && vm_free_range(p, n)) { vm_take(p, n); return want; } return 0; }
    for (uint32_t p = 1; p + n <= vm_pages; p++) {
        if (vm_used[p]) continue;
        uint32_t k = 0; while (k < n && !vm_used[p + k]) k++;
        if (k == n) { vm_take(p, n); memset(GP(p * VM_GRAN), 0, (size_t)n * VM_GRAN); port_debug("vm: %s %08x +%x", what, p * VM_GRAN, n * VM_GRAN); return p * VM_GRAN; }
        p += k;
    }
    if (!vm_quiet) port_warn("guest address space exhausted (%s, %u KB)", what, size >> 10);
    return 0;
}
void vm_free(uint32_t a) {
    uint32_t p = a / VM_GRAN; if (a % VM_GRAN || p >= vm_pages || !vm_len[p]) return;
    uint32_t n = vm_len[p]; vm_len[p] = 0; memset(vm_used + p, 0, n);
    plat_mem_discard(GP(a), (uint64_t)n * VM_GRAN);
}
uint32_t vm_region_size(uint32_t a) { uint32_t p = a / VM_GRAN; return p < vm_pages && a % VM_GRAN == 0 ? vm_len[p] * VM_GRAN : 0; }
int vm_mark(uint32_t lo, uint32_t hi, const char *what) {
    uint32_t p = lo / VM_GRAN, e = (uint32_t)(((uint64_t)hi + VM_GRAN - 1) / VM_GRAN);
    if (e > vm_pages) { port_warn("%s (%08x-%08x) lies outside the %llu MB guest address space", what, lo, hi, (unsigned long long)(g_space >> 20)); return 0; }
    if (!vm_free_range(p, e - p)) { port_warn("%s (%08x-%08x) overlaps another region", what, lo, hi); return 0; }
    vm_take(p, e - p); return 1;
}
static void vm_init(uint64_t need_hi) {
    uint64_t want = g_cfg.guest_space;
    if (!want) want = sizeof(void *) >= 8 ? (1ull << 32) : need_hi + (512ull << 20);     /* 32-bit hosts: images + 512 MB */
    if (want > (1ull << 32)) want = 1ull << 32;
    if (want < need_hi) port_die("the guest address space (%llu MB) is smaller than the game's images need (%llu MB); raise --guest-space",
                                 (unsigned long long)(want >> 20), (unsigned long long)(need_hi >> 20));
    want = (want + VM_GRAN - 1) & ~(uint64_t)(VM_GRAN - 1);
    M = plat_mem_reserve(want);
    if (!M) port_die("could not reserve %llu MB of address space for the game", (unsigned long long)(want >> 20));
    g_space = want; vm_pages = (uint32_t)(want / VM_GRAN);
    vm_len = calloc(vm_pages, sizeof *vm_len); vm_used = calloc(vm_pages, 1);
    vm_used[0] = 1;                                      /* the first 64 KB stay unmapped for the guest: null pointers */
    if (g_space > THUNK_BASE) vm_mark(THUNK_BASE, (uint32_t)(g_space - 1) & ~(VM_GRAN - 1), "host function thunks");
    port_debug("guest address space: %llu MB at %p", (unsigned long long)(g_space >> 20), (void *)M);
}

/* ---------------------------------------------------------------- thunks */
typedef struct { shim_fn fn; int pop; const char *name; uint32_t arg; } Thunk;
uint32_t g_targ;
static Thunk *thunks; static unsigned nthunks, capthunks;
uint32_t g_thunk_arg(shim_fn fn, int pop, const char *name, uint32_t arg) {
    if (nthunks == capthunks) { capthunks = capthunks ? capthunks * 2 : 1024; thunks = realloc(thunks, capthunks * sizeof *thunks); }
    thunks[nthunks] = (Thunk){ fn, pop, name, arg }; return THUNK_BASE + 16 * nthunks++;
}
uint32_t g_thunk(shim_fn fn, int pop, const char *name) { return g_thunk_arg(fn, pop, name, 0); }
int thunk_is(uint32_t a) { return a >= THUNK_BASE && a < THUNK_BASE + 16 * nthunks && !(a & 15); }
const char *thunk_name(uint32_t a) { return thunk_is(a) ? thunks[(a - THUNK_BASE) / 16].name : "?"; }
static const ShimDef *const *shim_tables(void) {
    static const ShimDef *const t[] = { kernel32_shims, kernel32_file_shims, user32_shims, gdi32_shims, advapi32_shims, winmm_shims, misc_shims,
                                        msvcrt_shims, msvcrt_stdio_shims, cxx_shims, d3d9_shims, dsound_shims, dinput_shims, ddraw_shims, NULL };
    return t;
}
const ShimDef *shim_find(const char *name) {
    for (const ShimDef *const *t = shim_tables(); *t; t++) for (const ShimDef *s = *t; s->name; s++) if (!strcmp(s->name, name)) return s;
    return NULL;
}
uint32_t port_proc(const char *n) {
    static struct { const ShimDef *s; uint32_t a; } cache[512]; static int nc;
    for (int i = 0; i < nc; i++) if (!strcmp(cache[i].s->name, n)) return cache[i].a;
    const ShimDef *s = shim_find(n); if (!s || nc >= 512) return 0;
    cache[nc].s = s; cache[nc].a = g_thunk(s->fn, s->pop, s->name); return cache[nc++].a;
}
static void thunk_run(CPU *c, unsigned i) {
    Thunk *t = &thunks[i];
    if (!t->fn) port_die("the game called %s, which the portable host does not implement yet", t->name);
    int tr = port_trace && trace_wanted(t->name);
    char ss[3][48] = { "", "", "" }; uint32_t ta[4] = { 0, 0, 0, 0 }, tret = 0;
    if (tr) {
        for (int k = 0; k < 4; k++) ta[k] = A(k);
        tret = RD32(c->esp);
        for (int k = 0; k < 3; k++) { uint32_t a = A(k); if (!g_valid(a, 48) || a >= THUNK_BASE) continue; const uint8_t *p = GP(a); int n = 0; while (n < 44 && p[n] >= 32 && p[n] < 127) n++; if (n >= 2 && (p[n] == 0 || n == 44)) { memcpy(ss[k], p, (size_t)n); ss[k][n] = 0; } }
    }
    ring_ret[ring_n & 15] = RD32(c->esp); ring_names[ring_n++ & 15] = t->name;
    g_targ = t->arg;
    t->fn(c);
    if (tr) trace_put("[t%u] %s(%08x, %08x, %08x, %08x) = %08x  from %08x%s%s%s%s%s%s", cur_thread_id(), t->name, ta[0], ta[1], ta[2], ta[3], c->eax, tret,
                      ss[0][0] ? " \"" : "", ss[0], ss[0][0] ? "\"" : "", ss[1][0] ? " \"" : "", ss[1], ss[1][0] ? "\"" : "");
    c->esp += 4 + (uint32_t)t->pop;     /* return address + the arguments the real function pops */
    thread_preempt_tick();
}

/* ---------------------------------------------------------------- the interface the generated code expects */
void rt_call_import(CPU *c, int idx) { thunk_run(c, (unsigned)idx); }
void rt_call_external(CPU *c, uint32_t t) {
    if (thunk_is(t)) { thunk_run(c, (t - THUNK_BASE) / 16); return; }
    guest_fn f = nd_lookup(t); if (f) { f(c); return; }
    if (mod_call(c, t)) return;
    port_die("call or jump to %08x, which is neither recompiled code nor a host function", t);
}
void rt_fault(CPU *c, uint32_t addr, int kind) {
    static const char *names[] = { "?", "unimplemented instruction", "divide error", "trap", "bad jump", "bad return" };
    (void)c; const char *s = guest_symbol(addr);
    port_die("guest fault: %s at %08x%s%s", kind >= 0 && kind <= 5 ? names[kind] : "?", addr, s ? " in " : "", s ? s : "");
}
void rt_budget_exhausted(CPU *c) { (void)c; port_die("budget exhausted"); }

uint32_t g_callv(CPU *c, uint32_t fn, int n, const uint32_t *a) {
    uint32_t save = c->esp;
    for (int i = n - 1; i >= 0; i--) PUSH32(c, a[i]);
    PUSH32(c, 0xFEEDF00Du);
    rt_call_external(c, fn);
    c->esp = save; return c->eax;
}
uint32_t g_call(CPU *c, uint32_t fn, int n, ...) {
    uint32_t a[16]; va_list ap; va_start(ap, n);
    for (int i = 0; i < n && i < 16; i++) a[i] = va_arg(ap, uint32_t);
    va_end(ap); return g_callv(c, fn, n, a);
}

/* setjmp/longjmp: the lifter turns a guest setjmp into RT_SETJMP, a host __builtin_setjmp in the caller's C frame */
typedef struct { uint32_t buf, esp, ebp, ebx, esi, edi, fs0, val; void *hj[5]; int used; } SjRec;
static SjRec sj[64]; static int sj_tick;
static SjRec *sj_find(uint32_t buf, int create) {
    SjRec *lru = &sj[0];
    for (int i = 0; i < 64; i++) { if (sj[i].used && sj[i].buf == buf) return &sj[i]; if (sj[i].used < lru->used) lru = &sj[i]; }
    if (!create) return NULL;
    memset(lru, 0, sizeof *lru); lru->buf = buf; return lru;
}
void **rt_sj_begin(CPU *c, uint32_t buf) {
    SjRec *r = sj_find(buf, 1); r->used = ++sj_tick;
    r->esp = c->esp; r->ebp = c->ebp; r->ebx = c->ebx; r->esi = c->esi; r->edi = c->edi; r->fs0 = RD32(c->fs_base);
    WR32(buf, c->ebp); WR32(buf + 4, c->ebx); WR32(buf + 8, c->edi); WR32(buf + 12, c->esi); WR32(buf + 16, c->esp);   /* MSVC _JUMP_BUFFER */
    WR32(buf + 20, RD32(c->esp)); WR32(buf + 24, r->fs0); WR32(buf + 28, 0xFFFFFFFFu); WR32(buf + 32, 0x56433230u); WR32(buf + 36, 0);
    return r->hj;
}
void rt_sj_resume(CPU *c, uint32_t buf) {
    SjRec *r = sj_find(buf, 0); if (!r) port_die("setjmp resumed for an unknown jump buffer %08x", buf);
    c->esp = r->esp; c->ebp = r->ebp; c->ebx = r->ebx; c->esi = r->esi; c->edi = r->edi; c->eax = r->val; WR32(c->fs_base, r->fs0);
}
void rt_longjmp(CPU *c) {
    uint32_t buf = RD32(c->esp + 4), val = RD32(c->esp + 8); SjRec *r = sj_find(buf, 0);
    if (!r) port_die("longjmp to jump buffer %08x, which recompiled code never passed to setjmp", buf);
    r->val = val ? val : 1; __builtin_longjmp(r->hj, 1);
}

/* ---------------------------------------------------------------- loading the executable */
static uint32_t crc32_of(const uint8_t *d, size_t n) {
    uint32_t c = 0xFFFFFFFFu, t[256];
    for (uint32_t i = 0; i < 256; i++) { uint32_t v = i; for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1; t[i] = v; }
    for (size_t i = 0; i < n; i++) c = t[(c ^ d[i]) & 255] ^ (c >> 8);
    return ~c;
}
static uint8_t *read_file(const char *path, size_t *n) {
    int err; PlatFile *f = plat_fs_open(path, PLAT_READ, &err); if (!f) return NULL;
    PlatStat st; if (plat_fs_fstat(f, &st)) { plat_fs_close(f); return NULL; }
    uint8_t *d = malloc((size_t)st.size + 1); size_t got = 0;
    while (got < st.size) { int64_t r = plat_fs_read(f, d + got, st.size - got); if (r <= 0) break; got += (size_t)r; }
    plat_fs_close(f); if (got != st.size) { free(d); return NULL; }
    *n = got; return d;
}
static uint32_t image_size(const uint8_t *d) { return *(const uint32_t *)(d + *(const uint32_t *)(d + 0x3c) + 24 + 56); }
static void load_image(const uint8_t *d, size_t n) {
    uint32_t lfanew = *(const uint32_t *)(d + 0x3c); const uint8_t *nt = d + lfanew;
    unsigned nsec = *(const uint16_t *)(nt + 6), optsz = *(const uint16_t *)(nt + 20); uint32_t hdrs = *(const uint32_t *)(nt + 24 + 60);
    const uint8_t *sec = nt + 24 + optsz;
    memcpy(GP(hd_base), d, hdrs);
    for (unsigned i = 0; i < nsec; i++) {
        const uint8_t *s = sec + 40 * i; uint32_t vsz = *(const uint32_t *)(s + 8), va = *(const uint32_t *)(s + 12), rsz = *(const uint32_t *)(s + 16), raw = *(const uint32_t *)(s + 20);
        uint32_t cp = rsz < vsz || !vsz ? rsz : vsz; if (raw + cp > n) cp = raw < n ? (uint32_t)n - raw : 0;
        memcpy(GP(hd_base + va), d + raw, cp);
    }
}
/* the IAT: every slot holds the thunk of its import (thunk i == import i, which is what rt_call_import expects), except
 * data imports, which get a real variable */
static int is_data_import(const char *n) { static const char *const d[] = { "_acmdln", "_adjust_fdiv", "_fmode", "_commode", "__initenv", "_environ", "_pgmptr", "__mb_cur_max", "_iob", 0 }; for (int i = 0; d[i]; i++) if (!strcmp(n, d[i])) return 1; return 0; }
static void bind_imports(int list_missing, int *missing) {
    for (unsigned i = 0; i < hd_nimports; i++) {
        const ShimDef *s = shim_find(hd_imports[i].name);
        g_thunk(s ? s->fn : NULL, s ? s->pop : 0, hd_imports[i].name);
        if (!s && !is_data_import(hd_imports[i].name)) { (*missing)++; if (list_missing) printf("%s!%s\n", hd_imports[i].dll, hd_imports[i].name); }
    }
}
uint32_t crt_data_import(const char *name);   /* crt/msvcrt.c: _acmdln, _adjust_fdiv, ... (0 = not a data import) */
static void fill_iat(void) {
    for (unsigned i = 0; i < hd_nimports; i++) {
        uint32_t v = crt_data_import(hd_imports[i].name);
        WR32(hd_iat_slot[i], v ? v : THUNK_BASE + 16 * i);
    }
}

/* ---------------------------------------------------------------- start-up */
static void usage(void) {
    fprintf(stderr,
        "usage: ss2port [options] <path to SS2.exe> [arguments for the game]\n"
        "  --frames N        stop after N presented frames\n"
        "  --windowed        force a window\n"
        "  --guest-space MB  guest address space to reserve (default: 4096 on 64-bit hosts)\n"
        "  --verbose         more logging; --trace: record import calls (first 8192 and last 4096, printed on exit/crash/Ctrl-C)\n"
        "  --trace-only a,b  record only these imports (e.g. fopen,fread,fseek)\n"
        "  --list-missing    list the game's imports the host does not implement\n"
        "  --list-shims      list every implemented import\n");
    exit(1);
}
int port_main(int argc, char **argv) {
    signal(SIGINT, on_signal); signal(SIGTERM, on_signal);
    int list_missing = 0; const char *exe = NULL; int i;
    for (i = 1; i < argc && !exe; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--list-missing")) list_missing = 1;
        else if (!strcmp(a, "--list-shims")) { for (const ShimDef *const *t = shim_tables(); *t; t++) for (const ShimDef *s = *t; s->name; s++) puts(s->name); return 0; }
        else if (!strcmp(a, "--trace")) port_trace = port_verbose = 1;
        else if (!strcmp(a, "--trace-only") && i + 1 < argc) { port_trace = port_verbose = 1; snprintf(trace_only, sizeof trace_only, ",%s,", argv[++i]); }
        else if (!strcmp(a, "--verbose")) port_verbose = 1;
        else if (!strcmp(a, "--windowed")) g_cfg.windowed = 1;
        else if (!strcmp(a, "--frames") && i + 1 < argc) g_cfg.max_frames = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "--guest-space") && i + 1 < argc) g_cfg.guest_space = (uint64_t)strtoull(argv[++i], NULL, 10) << 20;
        else if (a[0] == '-' && a[1] == '-') usage();
        else exe = a;
    }
    if (list_missing) { int missing = 0; bind_imports(1, &missing); printf("# %d of %u imports have no implementation yet\n", missing, hd_nimports); return 0; }
    if (!exe) usage();
    /* guest command line: the exe name and whatever followed it on ours */
    int n = snprintf(g_cfg.cmdline, sizeof g_cfg.cmdline, "SS2.exe");
    for (; i < argc && n < (int)sizeof g_cfg.cmdline - 2; i++) n += snprintf(g_cfg.cmdline + n, sizeof g_cfg.cmdline - (size_t)n, " %s", argv[i]);
    g_cfg.exe_path = exe;
    { static char dir[1024]; snprintf(dir, sizeof dir, "%s", exe); char *sl = strrchr(dir, '/'); char *bs = strrchr(dir, '\\'); if (bs > sl) sl = bs;
      if (sl) { *sl = 0; if (!dir[0]) strcpy(dir, "/"); } else strcpy(dir, "."); g_cfg.game_dir = dir; }

    size_t sz; uint8_t *img = read_file(exe, &sz);
    if (!img) port_die("cannot read %s", exe);
    if ((uint32_t)sz != hd_exe_size || crc32_of(img, sz) != hd_exe_crc)
        port_die("%s is not the executable this build was generated from (size %zu, crc %08x; expected size %u, crc %08x)", exe, sz, crc32_of(img, sz), hd_exe_size, hd_exe_crc);
    uint32_t img_end = hd_base + image_size(img);
    extern uint32_t mod_highest_end(void);
    uint64_t need = mod_highest_end() > img_end ? mod_highest_end() : img_end;
    vm_init(need);
    if (!vm_mark(hd_base, img_end, "SS2.exe")) port_die("the executable does not fit the guest address space");
    mod_reserve_all();
    load_image(img, sz); free(img);
    int missing = 0; bind_imports(0, &missing); fill_iat();
    GuestThread *mt = thread_main_init();
    CPU *c = cur_cpu(); (void)mt;
    port_log("running the recompiled entry point %08x (%u imports, %d without an implementation)", hd_entry, hd_nimports, missing);
    PUSH32(c, 0xFEEDF00Du);
    nd_call(c, hd_entry);
    port_log("the game returned from its entry point, eax=%u", c->eax);
    port_exit((int)c->eax);
}

void port_frame_presented(void) {
    g_frames++;
    if (g_frames == 1) port_log("first frame presented");
    if (g_cfg.max_frames && g_frames >= g_cfg.max_frames) { port_log("stopping after %u frames", g_frames); port_exit(0); }
    input_pump();
}
uint32_t ms_ticks(void) { static uint64_t t0; uint64_t t = plat_time_ns(); if (!t0) t0 = t; return (uint32_t)((t - t0) / 1000000u) + 100000u; }
uint64_t unix_ns_to_filetime(int64_t ns) { return (uint64_t)(ns / 100) + 116444736000000000ull; }
