/* host.c: the portable host. Loads the user's SS2.exe into a flat guest address space, hooks every import up to a host C
 * function ("shim"), and runs the recompiled entry point. No Windows API, no x86 code, no 32-bit assumptions: guest memory is
 * one block that the generated code addresses as M + address (the generated C is built without RT_IDENTITY).
 *
 *   ss2port [--list-missing] [--frames N] [--trace] <path to SS2.exe>
 *
 * This is the first step of the platform layer (see port/README.md): everything the game needs from the operating system is
 * behind the tables in win32.c, crt.c and com.c. A port to another platform implements those; the recompiled game and this
 * file do not change. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <setjmp.h>
#include "port.h"

/* ---- generated tables (out/nd) ---- */
typedef struct { const char *dll, *name; } ImpDef;
extern const ImpDef hd_imports[]; extern const unsigned hd_nimports; extern const uint32_t hd_iat_slot[];
extern const uint32_t hd_entry, hd_base, hd_exe_size, hd_exe_crc;
void nd_call(CPU *c, uint32_t t);
extern guest_fn nd_lookup(uint32_t va);

uint8_t *M;
int port_trace;
uint32_t g_frames, g_max_frames;
static const char *ring_names[16]; static uint32_t ring_ret[16]; static unsigned ring_n;
static CPU g_cpu; CPU *g_cpup = &g_cpu;

void port_miss(const char *what, const char *guest, const char *host) { static int n; if (n++ < 60) port_log("not found (%s): \"%s\" -> %s", what, guest, host); }
void port_log(const char *fmt, ...) { va_list ap; va_start(ap, fmt); fputs("[port] ", stderr); vfprintf(stderr, fmt, ap); fputc('\n', stderr); va_end(ap); }
void port_exit(int code) { fflush(stdout); fflush(stderr); _exit(code); }

#ifdef RT_TRACE
/* coverage trace (debug build: -DRT_TRACE): remembers every guest instruction address in the order it first ran */
#define COV_LO 0x400000u
#define COV_N 0x700000u
static uint8_t cov_bits[COV_N / 8 + 1]; static uint32_t *cov_ord; static uint32_t cov_cnt; static uint32_t tr_ring[64]; static uint32_t tr_pos;
void rt_trace(CPU *c, uint32_t va) {
    (void)c; tr_ring[tr_pos++ & 63] = va; uint32_t i = va - COV_LO; if (i >= COV_N) return;
    if (cov_bits[i >> 3] & (1u << (i & 7))) return; cov_bits[i >> 3] |= (uint8_t)(1u << (i & 7));
    if (!cov_ord) cov_ord = malloc(sizeof(uint32_t) * COV_N); cov_ord[cov_cnt++] = va;
}
void port_cov_dump(void) {
    const char *p = getenv("PORT_COV"); if (!p || !cov_ord) return; FILE *f = fopen(p, "w"); if (!f) return;
    fprintf(f, "# last 64 instructions, oldest first\n"); for (int k = 0; k < 64; k++) fprintf(f, "%08x\n", tr_ring[(tr_pos + k) & 63]);
    fprintf(f, "# first-execution order of %u instructions\n", cov_cnt); for (uint32_t k = 0; k < cov_cnt; k++) fprintf(f, "%08x\n", cov_ord[k]); fclose(f);
}
#else
void port_cov_dump(void) {}
#endif
void port_cov_dump(void);
void port_die(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); fputs("[port] FATAL: ", stderr); vfprintf(stderr, fmt, ap); fputc('\n', stderr); va_end(ap);
    fprintf(stderr, "[port]   guest esp=%08x ebp=%08x eax=%08x ecx=%08x edx=%08x ebx=%08x esi=%08x edi=%08x\n", g_cpu.esp, g_cpu.ebp, g_cpu.eax, g_cpu.ecx, g_cpu.edx, g_cpu.ebx, g_cpu.esi, g_cpu.edi);
    fprintf(stderr, "[port]   last host calls:"); for (unsigned i = ring_n > 12 ? ring_n - 12 : 0; i < ring_n; i++) fprintf(stderr, " %s@%x", ring_names[i & 15], ring_ret[i & 15]); fputc(10, stderr);
    { fprintf(stderr, "[port]   frames (ebp chain, return addresses):"); uint32_t bp = g_cpu.ebp; for (int i = 0; i < 12 && bp > 0x1000 && bp < 0x08400000u; i++) { fprintf(stderr, " %08x", RD32(bp + 4)); uint32_t nb = RD32(bp); if (nb <= bp) break; bp = nb; } fputc(10, stderr); }
    fprintf(stderr, "[port]   guest stack:"); for (int i = 0; i < 12; i++) fprintf(stderr, " %08x", RD32(g_cpu.esp + 4 * i)); fputc(10, stderr);
    port_cov_dump(); port_exit(2);
}

/* ---------------------------------------------------------------- guest heap (first-fit-free size classes inside the guest address space) */
#define HEAP_LO 0x40000000u
#define HEAP_HI 0x7E000000u
typedef struct { uint32_t cap, size, pad[2]; } HBlk;       /* 16 bytes before every payload */
static uint32_t heap_top = HEAP_LO, freel[128];
static int cls_of(uint32_t n, uint32_t *cap) {
    if (n <= 1024) { int k = (int)((n + 15) / 16); if (!k) k = 1; *cap = (uint32_t)k * 16; return k; }
    int b = 11; while (b < 31 && (1u << b) < n) b++;
    *cap = 1u << b; return 64 + b - 10;
}
uint32_t g_alloc(uint32_t n) {
    uint32_t cap; if (n > 0x40000000u) return 0;
    int k = cls_of(n, &cap); uint32_t p;
    if (freel[k]) { p = freel[k]; freel[k] = RD32(p); memset(GP(p), 0, cap); }
    else {
        if ((uint64_t)heap_top + 16 + cap > HEAP_HI) return 0;
        p = heap_top + 16; heap_top += 16 + cap;
    }
    HBlk *h = (HBlk *)GP(p - 16); h->cap = cap; h->size = n; return p;
}
uint32_t g_size(uint32_t p) { return p ? ((HBlk *)GP(p - 16))->size : 0; }
void g_free(uint32_t p) {
    if (!p) return;
    if (p < HEAP_LO + 16 || p >= heap_top) return;   /* not ours: ignore, like the guest's own free of a static would be a bug anyway */
    HBlk *h = (HBlk *)GP(p - 16); uint32_t cap; int k = cls_of(h->cap, &cap);
    if (cap != h->cap) return;
    WR32(p, freel[k]); freel[k] = p;
}
uint32_t g_realloc(uint32_t p, uint32_t n) {
    if (!p) return g_alloc(n);
    HBlk *h = (HBlk *)GP(p - 16);
    if (n <= h->cap) { if (n > h->size) memset(GP(p + h->size), 0, n - h->size); h->size = n; return p; }
    uint32_t q = g_alloc(n); if (!q) return 0;
    memcpy(GP(q), GP(p), h->size); g_free(p); return q;
}
uint32_t g_str(const char *s) { size_t n = strlen(s) + 1; uint32_t g = g_alloc((uint32_t)n); if (g) memcpy(GP(g), s, n); return g; }

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
static const ShimDef *const shim_tabs[] = { win32_shims, crt_shims, com_shims, mmio_shims, win32b_shims };
const ShimDef *shim_find(const char *name) {
    for (unsigned t = 0; t < sizeof shim_tabs / sizeof *shim_tabs; t++)
        for (const ShimDef *s = shim_tabs[t]; s->name; s++) if (!strcmp(s->name, name)) return s;
    return NULL;
}
static void thunk_run(CPU *c, unsigned i) {
    Thunk *t = &thunks[i];
    if (!t->fn) port_die("the game called %s, which the portable host has no implementation for yet", t->name);
    if (port_trace) {
        char ss[4][48] = { "", "", "", "" };
        for (int i = 0; i < 4; i++) { uint32_t a = A(i); if (a < 0x10000 || a >= 0x7F000000u) continue; const uint8_t *p = GP(a); int n = 0; while (n < 44 && p[n] >= 32 && p[n] < 127) n++; if (n >= 3 && (p[n] == 0 || n == 44)) { memcpy(ss[i], p, (size_t)n); ss[i][n] = 0; } }
        port_log("call %s(%08x, %08x, %08x, %08x)%s%s%s%s%s%s%s%s", t->name, A(0), A(1), A(2), A(3), ss[0][0] ? " 0=\"" : "", ss[0], ss[0][0] ? "\"" : "", ss[1][0] ? " 1=\"" : "", ss[1], ss[1][0] ? "\"" : "", ss[2][0] ? " 2=\"" : "", ss[2]);
    }
    ring_ret[ring_n & 15] = RD32(c->esp); ring_names[ring_n++ & 15] = t->name; g_targ = t->arg;
    t->fn(c);
    c->esp += 4 + t->pop;     /* return address + the arguments the real function pops */
}

/* ---------------------------------------------------------------- the interface the generated code expects */
void rt_call_import(CPU *c, int idx) { thunk_run(c, (unsigned)idx); }
void rt_call_external(CPU *c, uint32_t t) {
    if (thunk_is(t)) { thunk_run(c, (t - THUNK_BASE) / 16); return; }
    { extern guest_fn nd_lookup(uint32_t); guest_fn f = nd_lookup(t); if (f) { f(c); return; } }
    if (mod_call(c, t)) return;
    port_die("indirect call/jump to %08x, which is neither a recompiled function nor a host function", t);
}
static void dump_gs_frame(CPU *c) {     /* __report_gsfailure entry: [ebp+4] is the call site of __security_check_cookie in the damaged function */
    uint32_t top = c->ebp + 8, cookie = RD32(0x7dd400);
    fprintf(stderr, "[port]   stack cookie value %08x; the damaged frame starts at guest %08x (dumping 0x180 bytes, 16 per line, '*' = the word the cookie check compares)\n", cookie, top);
    for (uint32_t o = 0; o < 0x180; o += 16) { fprintf(stderr, "[port]   %08x:", top + o); for (int k = 0; k < 4; k++) fprintf(stderr, " %08x", RD32(top + o + 4 * (uint32_t)k)); fputc(10, stderr); }
}
void rt_fault(CPU *c, uint32_t addr, int kind) {
    if (addr == 0x6f7376u) { fprintf(stderr, "[port] the game's stack-overrun check (security cookie) failed\n"); dump_gs_frame(c); }
    static const char *names[] = { "?", "unimplemented instruction", "divide error", "trap", "bad jump", "bad return" };
    (void)c; port_die("guest fault: %s at %08x", kind >= 0 && kind <= 5 ? names[kind] : "?", addr);
}
void rt_budget_exhausted(CPU *c) { (void)c; port_die("budget exhausted"); }

uint32_t g_call(CPU *c, uint32_t fn, int n, ...) {
    uint32_t save = c->esp, a[16]; va_list ap; va_start(ap, n);
    for (int i = 0; i < n && i < 16; i++) a[i] = va_arg(ap, uint32_t);
    va_end(ap);
    for (int i = n - 1; i >= 0; i--) PUSH32(c, a[i]);
    PUSH32(c, 0xFEEDF00Du);
    nd_call(c, fn);
    c->esp = save; return c->eax;
}

/* setjmp/longjmp (the Squirrel compiler reports syntax errors with them) */
typedef struct { uint32_t buf, esp, ebp, ebx, esi, edi, fs0, val; void *hj[5]; int used; } SjRec;
static SjRec sj[32]; static int sj_tick;
static SjRec *sj_find(uint32_t buf, int create) {
    SjRec *lru = &sj[0];
    for (int i = 0; i < 32; i++) { if (sj[i].used && sj[i].buf == buf) return &sj[i]; if (sj[i].used < lru->used) lru = &sj[i]; }
    if (!create) return NULL;
    memset(lru, 0, sizeof *lru); lru->buf = buf; return lru;
}
void **rt_sj_begin(CPU *c, uint32_t buf) {
    SjRec *r = sj_find(buf, 1); r->used = ++sj_tick;
    r->esp = c->esp; r->ebp = c->ebp; r->ebx = c->ebx; r->esi = c->esi; r->edi = c->edi; r->fs0 = RD32(c->fs_base);
    WR32(buf, c->ebp); WR32(buf + 4, c->ebx); WR32(buf + 8, c->edi); WR32(buf + 12, c->esi); WR32(buf + 16, c->esp);
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

/* ---------------------------------------------------------------- files: Windows paths on a case-sensitive host */
char *host_path(const char *w, char *out, size_t n) {
    const char *w0 = w;
    char buf[1024]; size_t k = 0;
    if (w[0] && w[1] == ':') { w += 2; while (*w == '\\' || *w == '/') w++; }   /* drive letter: everything is relative to the game folder */
    for (; *w && k < sizeof buf - 1; w++) buf[k++] = *w == '\\' ? '/' : *w;
    buf[k] = 0;
    char cur[1024]; size_t cl = 0; const char *p = buf;
    if (*p == '/') { cur[cl++] = '/'; p++; } else { cur[cl++] = '.'; }
    cur[cl] = 0;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        char comp[256]; size_t q = 0; while (*p && *p != '/' && q < sizeof comp - 1) comp[q++] = *p++; comp[q] = 0;
        if (!strcmp(comp, ".")) continue;
        char try_[1400]; snprintf(try_, sizeof try_, "%s%s%s", cur, cl && cur[cl - 1] == '/' ? "" : "/", comp);
        struct stat st;
        if (stat(try_, &st) != 0) {                       /* look for a differently-cased name */
            DIR *d = opendir(cur);
            if (d) { struct dirent *e; while ((e = readdir(d))) if (!strcasecmp(e->d_name, comp)) { snprintf(try_, sizeof try_, "%s%s%s", cur, cl && cur[cl - 1] == '/' ? "" : "/", e->d_name); break; } closedir(d); }
        }
        snprintf(cur, sizeof cur, "%s", try_); cl = strlen(cur);
    }
    snprintf(out, n, "%s", cur);
    { static int on = -1; if (on < 0) on = getenv("PORT_FILES") != NULL; if (on) port_log("path: \"%s\" -> %s", w0, cur); }
    return out;
}

/* ---------------------------------------------------------------- PE loading */
static uint32_t crc32_of(const uint8_t *d, size_t n) {
    uint32_t c = 0xFFFFFFFFu, t[256];
    for (uint32_t i = 0; i < 256; i++) { uint32_t v = i; for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1; t[i] = v; }
    for (size_t i = 0; i < n; i++) c = t[(c ^ d[i]) & 255] ^ (c >> 8);
    return ~c;
}
static void load_image(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(n); if (fread(d, 1, n, f) != (size_t)n) { fprintf(stderr, "read error\n"); exit(1); } fclose(f);
    if ((uint32_t)n != hd_exe_size || crc32_of(d, n) != hd_exe_crc) {
        fprintf(stderr, "%s is not the SS2.exe this build was generated from (size %ld, crc %08x; expected size %u, crc %08x)\n", path, n, crc32_of(d, n), hd_exe_size, hd_exe_crc); exit(1);
    }
    uint32_t lfanew = *(uint32_t *)(d + 0x3c); const uint8_t *nt = d + lfanew;
    unsigned nsec = *(uint16_t *)(nt + 6), optsz = *(uint16_t *)(nt + 20); uint32_t hdrs = *(uint32_t *)(nt + 24 + 60);
    const uint8_t *sec = nt + 24 + optsz;
    memcpy(GP(hd_base), d, hdrs);
    for (unsigned i = 0; i < nsec; i++) {
        const uint8_t *s = sec + 40 * i; uint32_t vsz = *(uint32_t *)(s + 8), va = *(uint32_t *)(s + 12), rsz = *(uint32_t *)(s + 16), raw = *(uint32_t *)(s + 20);
        uint32_t cp = rsz < vsz ? rsz : vsz;
        if (raw + cp > (uint32_t)n) cp = (uint32_t)n - raw;
        memcpy(GP(hd_base + va), d + raw, cp);
    }
    free(d);
}

/* ---------------------------------------------------------------- self test: drives the null Direct3D9/DirectSound/DirectInput objects through their vtables the way the game does */
static uint32_t gcall_thunk(CPU *c, uint32_t fn, int n, const uint32_t *a) {
    uint32_t save = c->esp; for (int i = n - 1; i >= 0; i--) PUSH32(c, a[i]); PUSH32(c, 0xFEEDF00Du);
    uint32_t esp0 = c->esp; rt_call_external(c, fn); if (c->esp != save) { fprintf(stderr, "stack imbalance calling %s: esp %08x, expected %08x (popped %d)\n", thunk_name(fn), c->esp, save, (int)(c->esp - esp0)); exit(1); }
    return c->eax;
}
static uint32_t vcall(CPU *c, uint32_t obj, int slot, int n, ...) {
    uint32_t a[16]; a[0] = obj; va_list ap; va_start(ap, n); for (int i = 0; i < n; i++) a[i + 1] = va_arg(ap, uint32_t); va_end(ap);
    return gcall_thunk(c, RD32(RD32(obj) + 4 * (uint32_t)slot), n + 1, a);
}
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "selftest FAILED: %s (line %d)\n", #x, __LINE__); return 1; } } while (0)
static int run_selftest(void) {
    CPU *c = &g_cpu; memset(c, 0, sizeof *c); c->esp = 0x08000000u; c->cw = 0x27f;
    int missing = 0; for (unsigned i = 0; i < hd_nimports; i++) { const ShimDef *s = shim_find(hd_imports[i].name); g_thunk(s ? s->fn : NULL, s ? s->pop : 0, hd_imports[i].name); missing += !s; }
    uint32_t pa = g_alloc(64), out = g_alloc(64), mem = g_alloc(4096);
    #define PROC(nm_, n, ...) ({ const ShimDef *s_ = shim_find(nm_); CHECK(s_); uint32_t t_ = g_thunk(s_->fn, s_->pop, s_->name); uint32_t aa_[] = { __VA_ARGS__, 0 }; gcall_thunk(c, t_, n, aa_); })
    #define V(obj, iface, meth, n, ...) ({ int sl_ = com_slot(iface, meth); CHECK(sl_ >= 0); vcall(c, obj, sl_, n, ##__VA_ARGS__); })
    uint32_t d3d = PROC("Direct3DCreate9", 1, 32); CHECK(d3d);
    CHECK(V(d3d, "IDirect3D9", "GetAdapterCount", 0) == 1);
    WR32(pa, 640); WR32(pa + 4, 480); WR32(pa + 8, 22);
    CHECK(V(d3d, "IDirect3D9", "CreateDevice", 6, 0u, 1u, 0u, 0u, pa, out) == 0); uint32_t dev = RD32(out); CHECK(dev);
    CHECK(V(dev, "IDirect3DDevice9", "BeginScene", 0) == 0); CHECK(V(dev, "IDirect3DDevice9", "Clear", 6, 0u, 0u, 0u, 0u, 0u, 0u) == 0);
    CHECK(V(dev, "IDirect3DDevice9", "CreateTexture", 8, 64u, 64u, 0u, 0u, 21u, 1u, out, 0u) == 0); uint32_t tex = RD32(out); CHECK(tex);
    CHECK(V(tex, "IDirect3DTexture9", "GetLevelCount", 0) == 7);
    CHECK(V(tex, "IDirect3DTexture9", "LockRect", 4, 0u, mem, 0u, 0u) == 0); CHECK(RD32(mem) == 256 && RD32(mem + 4));
    CHECK(V(tex, "IDirect3DTexture9", "GetSurfaceLevel", 2, 1u, out) == 0); CHECK(RD32(out));
    CHECK(V(dev, "IDirect3DDevice9", "CreateVertexBuffer", 6, 1024u, 8u, 0u, 1u, out, 0u) == 0); uint32_t vb = RD32(out);
    CHECK(V(vb, "IDirect3DBuffer9", "Lock", 4, 0u, 0u, mem, 0u) == 0); CHECK(RD32(mem) != 0);
    CHECK(V(dev, "IDirect3DDevice9", "DrawPrimitive", 3, 4u, 0u, 1u) == 0);
    CHECK(V(dev, "IDirect3DDevice9", "Present", 4, 0u, 0u, 0u, 0u) == 0); CHECK(g_frames == 1);
    uint32_t ds = 0; PROC("DirectSoundCreate", 3, 0u, out, 0u); ds = RD32(out); CHECK(ds);
    WR32(pa, 36); WR32(pa + 4, 0x4000); WR32(pa + 8, 4096); WR32(pa + 16, 0);
    CHECK(V(ds, "IDirectSound", "CreateSoundBuffer", 3, pa, out, 0u) == 0); uint32_t sb = RD32(out); CHECK(sb);
    CHECK(V(sb, "IDirectSoundBuffer", "Lock", 7, 0u, 100u, mem, mem + 8, mem + 16, mem + 24, 0u) == 0); CHECK(RD32(mem + 8) == 100);
    CHECK(V(sb, "IDirectSoundBuffer", "Play", 3, 0u, 0u, 1u) == 0);
    uint32_t di = 0; PROC("DirectInputCreateA", 4, 0u, 0x800u, out, 0u); di = RD32(out); CHECK(di);
    CHECK(V(di, "IDirectInput", "CreateDevice", 3, 0u, out, 0u) == 0); uint32_t kb = RD32(out); CHECK(V(kb, "IDirectInputDevice", "GetDeviceState", 2, 256u, mem) == 0);
    printf("selftest OK: Direct3D9 device/texture/buffer/Present, DirectSound buffer, DirectInput device (%d imports without implementation)\n", missing); return 0;
}

/* ---------------------------------------------------------------- start-up */
static void usage(void) { fprintf(stderr, "usage: ss2port [--list-missing] [--trace] [--frames N] <SS2.exe>\n"); exit(1); }
int main(int argc, char **argv) {
    int list_missing = 0, selftest = 0; const char *exe = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--list-missing")) list_missing = 1;
        else if (!strcmp(argv[i], "--selftest")) selftest = 1;
        else if (!strcmp(argv[i], "--list-shims")) { static const ShimDef *const tabs[] = { win32_shims, crt_shims, com_shims, mmio_shims, win32b_shims }; for (unsigned t = 0; t < 5; t++) for (const ShimDef *s = tabs[t]; s->name; s++) puts(s->name); return 0; }
        else if (!strcmp(argv[i], "--trace")) port_trace = 1;
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) g_max_frames = (uint32_t)atoi(argv[++i]);
        else if (argv[i][0] != '-') exe = argv[i]; else usage();
    }
    if (getenv("PORT_TRACE")) port_trace = 1;
    /* imports -> thunks (thunk i == import i, which is what rt_call_import expects) */
    int missing = 0;
    for (unsigned i = 0; i < hd_nimports; i++) {
        const ShimDef *s = shim_find(hd_imports[i].name);
        g_thunk(s ? s->fn : NULL, s ? s->pop : 0, hd_imports[i].name);
        if (!s) { missing++; if (list_missing) printf("%s!%s\n", hd_imports[i].dll, hd_imports[i].name); }
    }
    if (selftest) { M = mmap(NULL, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0); return run_selftest(); }
    if (list_missing) { printf("# %d of %u imports have no implementation yet\n", missing, hd_nimports); return 0; }
    if (!exe) usage();
    M = mmap(NULL, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (M == MAP_FAILED) { perror("mmap of the guest address space"); return 1; }
    char dir[1024]; snprintf(dir, sizeof dir, "%s", exe); char *sl = strrchr(dir, '/');
    if (sl) { *sl = 0; if (chdir(dir)) perror("chdir"); }
    char *ex = realpath(exe, NULL); (void)ex;
    load_image(exe);
    for (unsigned i = 0; i < hd_nimports; i++) {
        extern void port_data_import(unsigned idx, const char *name, uint32_t slot);
        port_data_import(i, hd_imports[i].name, hd_iat_slot[i]);
    }
    /* thread environment block, process environment block, TLS slots, stack */
    const uint32_t STACK_TOP = 0x08400000u, TIB = STACK_TOP, PEB = STACK_TOP + 0x2000;
    CPU *c = &g_cpu; memset(c, 0, sizeof *c); c->cw = 0x27f; c->fs_base = TIB;
    WR32(TIB + 0x00, 0xFFFFFFFFu); WR32(TIB + 0x04, STACK_TOP); WR32(TIB + 0x08, STACK_TOP - 0x400000u);
    WR32(TIB + 0x18, TIB); WR32(TIB + 0x2c, g_alloc(4 * 1088)); WR32(TIB + 0x30, PEB); WR32(PEB + 0x08, hd_base);
    c->esp = STACK_TOP - 0x100;
    PUSH32(c, 0xFEEDF00Du);
    port_log("running the recompiled entry point %08x (%u imports, %d without an implementation)", hd_entry, hd_nimports, missing);
    nd_call(c, hd_entry);
    port_log("the game returned from its entry point, eax=%u", c->eax);
    return (int)c->eax;
}
