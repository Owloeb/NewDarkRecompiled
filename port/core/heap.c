/* heap.c: the guest heap behind malloc, HeapAlloc, LocalAlloc, operator new and the host's own guest-side objects.
 *
 * A two-level segregated-fit allocator (TLSF): O(1) allocation and free, immediate coalescing of neighbours, and good
 * worst-case fragmentation, which matters on hosts whose guest address space is only a few hundred megabytes. Arenas of
 * 32 MB come from the guest address space (vm_alloc); requests of 1 MB and more get their own region, like Windows heaps
 * hand big blocks to VirtualAlloc. Blocks are 16-byte aligned and zero-filled. Every block carries a header that is
 * checked on free, so a double free or a corrupted header stops the host with a clear message instead of silently
 * damaging the heap. Only the thread holding the guest lock calls into here, so there is no lock. */
#include "core.h"

#define HDR 16u
#define ALIGN 16u
#define MIN_BLOCK 32u                  /* header + room for the two free-list links */
#define ARENA (32u << 20)
#define BIG (1u << 20)
#define SL_LOG 4
#define SL_N (1 << SL_LOG)
#define SMALL (1u << (SL_LOG + 4))      /* 256: below this, one class per 16 bytes */
#define FL_N 25
#define F_FREE 1u
#define F_BIG 2u
#define F_END 4u
#define MAGIC 0xA110C8EDu
/* header at block address b (payload at b + 16) */
#define B_PREV(b) ((b) + 0)            /* physical predecessor's block address, 0 for the first block of an arena */
#define B_SIZE(b) ((b) + 4)            /* block size (header included) | flags in the low 4 bits */
#define B_REQ(b) ((b) + 8)             /* size the caller asked for */
#define B_MAGIC(b) ((b) + 12)          /* MAGIC ^ b */
#define F_NEXT(b) ((b) + 16)           /* free blocks: list links in the payload */
#define F_PREVL(b) ((b) + 20)

static uint32_t fl_map, sl_map[FL_N], heads[FL_N][SL_N];
static uint32_t arenas[256]; static int narenas;
static uint32_t bigs_live;

static inline uint32_t bsize(uint32_t b) { return RD32(B_SIZE(b)) & ~15u; }
static inline uint32_t bflags(uint32_t b) { return RD32(B_SIZE(b)) & 15u; }
static inline void set_hdr(uint32_t b, uint32_t size, uint32_t flags) { WR32(B_SIZE(b), size | flags); WR32(B_MAGIC(b), MAGIC ^ b); }
static inline int fls32(uint32_t v) { return 31 - __builtin_clz(v); }
static void mapping(uint32_t size, int *fl, int *sl) {
    if (size < SMALL) { *fl = 0; *sl = (int)(size / 16); return; }
    int m = fls32(size); *sl = (int)((size >> (m - SL_LOG)) ^ SL_N); *fl = m - (SL_LOG + 4) + 1;
}
static void insert_free(uint32_t b) {
    int fl, sl; mapping(bsize(b), &fl, &sl);
    uint32_t h = heads[fl][sl]; WR32(F_NEXT(b), h); WR32(F_PREVL(b), 0); if (h) WR32(F_PREVL(h), b);
    heads[fl][sl] = b; fl_map |= 1u << fl; sl_map[fl] |= 1u << sl;
}
static void remove_free(uint32_t b) {
    int fl, sl; mapping(bsize(b), &fl, &sl);
    uint32_t n = RD32(F_NEXT(b)), p = RD32(F_PREVL(b));
    if (p) WR32(F_NEXT(p), n); else heads[fl][sl] = n;
    if (n) WR32(F_PREVL(n), p);
    if (!heads[fl][sl]) { sl_map[fl] &= ~(1u << sl); if (!sl_map[fl]) fl_map &= ~(1u << fl); }
}
static uint32_t find_free(uint32_t size) {
    if (size >= SMALL) size += (1u << (fls32(size) - SL_LOG)) - 1;     /* round up so any block in the class fits */
    int fl, sl; mapping(size, &fl, &sl); if (fl >= FL_N) return 0;
    uint32_t sm = sl_map[fl] & (~0u << sl);
    if (!sm) { uint32_t fm = fl + 1 < 32 ? fl_map & (~0u << (fl + 1)) : 0; if (!fm) return 0; fl = __builtin_ctz(fm); sm = sl_map[fl]; }
    return heads[fl][__builtin_ctz(sm)];
}
static uint32_t next_phys(uint32_t b) { return b + bsize(b); }
static void bad(uint32_t p, const char *what) {
    const char *s = NULL; (void)s;
    port_die("guest heap: %s (block %08x). The game freed something twice or wrote past the end of a block.", what, p);
}
static int add_arena(uint32_t min) {
    if (narenas >= 256) return 0;
    uint32_t need = (min + 2 * HDR + 0xFFFFu) & ~0xFFFFu, sz = ARENA, a = 0;
    while (sz < need) sz *= 2;
    for (; sz >= need && !(a = vm_alloc_quiet(sz)); sz /= 2) ;     /* small guest spaces: take smaller arenas */
    if (!a) { port_warn("guest heap: out of memory (%u bytes requested)", min); return 0; }
    arenas[narenas++] = a;
    uint32_t end = a + sz - HDR;                            /* sentinel: size 0, never free */
    WR32(B_PREV(a), 0); set_hdr(a, end - a, F_FREE); WR32(B_REQ(a), 0);
    WR32(B_PREV(end), a); set_hdr(end, 0, F_END); WR32(B_REQ(end), 0);
    insert_free(a); return 1;
}
static void split(uint32_t b, uint32_t want) {       /* b is in use and at least want + MIN_BLOCK big: give the rest back */
    uint32_t total = bsize(b); if (total < want + MIN_BLOCK) return;
    uint32_t r = b + want, n = b + total;
    set_hdr(b, want, bflags(b)); WR32(B_PREV(r), b); set_hdr(r, total - want, F_FREE); WR32(B_REQ(r), 0); WR32(B_PREV(n), r);
    if (bflags(n) & F_FREE) { remove_free(n); uint32_t nn = next_phys(n); set_hdr(r, bsize(r) + bsize(n), F_FREE); WR32(B_PREV(nn), r); }
    insert_free(r);
}
uint32_t g_alloc(uint32_t n) {
    if (n > 0xC0000000u) return 0;
    uint32_t need = (n + HDR + ALIGN - 1) & ~(ALIGN - 1); if (need < MIN_BLOCK) need = MIN_BLOCK;
    if (need >= BIG) {
        uint32_t r = vm_alloc(need, 0, "big heap block"); if (!r) return 0;
        WR32(B_PREV(r), 0); set_hdr(r, (need + 0xFFFFu) & ~0xFFFFu, F_BIG); WR32(B_REQ(r), n); bigs_live++;
        return r + HDR;                                     /* vm_alloc zero-fills */
    }
    uint32_t b = find_free(need);
    if (!b) { if (!add_arena(need)) return 0; b = find_free(need); if (!b) return 0; }
    remove_free(b); set_hdr(b, bsize(b), 0); split(b, need); WR32(B_REQ(b), n);
    memset(GP(b + HDR), 0, bsize(b) - HDR);
    return b + HDR;
}
static uint32_t check_block(uint32_t p) {
    if (!p || (p & (ALIGN - 1)) || !g_valid(p - HDR, HDR)) return 0;
    uint32_t b = p - HDR; if (RD32(B_MAGIC(b)) != (MAGIC ^ b)) return 0;
    return b;
}
int g_owns(uint32_t p) { uint32_t b = check_block(p); return b && !(bflags(b) & (F_FREE | F_END)); }
uint32_t g_size(uint32_t p) { uint32_t b = check_block(p); return b && !(bflags(b) & F_FREE) ? RD32(B_REQ(b)) : 0; }
void g_free(uint32_t p) {
    if (!p) return;
    uint32_t b = check_block(p);
    if (!b) bad(p, "free of a pointer that is not a heap block");
    uint32_t f = bflags(b);
    if (f & F_FREE) bad(p, "double free");
    if (f & F_BIG) { WR32(B_MAGIC(b), 0); bigs_live--; vm_free(b); return; }
    uint32_t n = next_phys(b), pr = RD32(B_PREV(b));
    if (RD32(B_PREV(n)) != b) bad(p, "corrupted heap (next block does not point back)");
    set_hdr(b, bsize(b), F_FREE);
    if (bflags(n) & F_FREE) { remove_free(n); set_hdr(b, bsize(b) + bsize(n), F_FREE); WR32(B_MAGIC(n), 0); n = next_phys(b); WR32(B_PREV(n), b); }
    if (pr && (bflags(pr) & F_FREE)) { remove_free(pr); set_hdr(pr, bsize(pr) + bsize(b), F_FREE); WR32(B_MAGIC(b), 0); WR32(B_PREV(n), pr); b = pr; }
    insert_free(b);
}
uint32_t g_realloc(uint32_t p, uint32_t n) {
    if (!p) return g_alloc(n);
    uint32_t b = check_block(p); if (!b || (bflags(b) & F_FREE)) bad(p, "realloc of a pointer that is not a live heap block");
    uint32_t old = RD32(B_REQ(b)), need = (n + HDR + ALIGN - 1) & ~(ALIGN - 1); if (need < MIN_BLOCK) need = MIN_BLOCK;
    if (!(bflags(b) & F_BIG)) {
        if (need <= bsize(b)) { if (n > old) memset(GP(p + old), 0, n - old); WR32(B_REQ(b), n); split(b, need); return p; }
        uint32_t nx = next_phys(b);
        if ((bflags(nx) & F_FREE) && bsize(b) + bsize(nx) >= need && need < BIG) {      /* grow into the free neighbour */
            remove_free(nx); uint32_t nn = next_phys(nx); set_hdr(b, bsize(b) + bsize(nx), 0); WR32(B_MAGIC(nx), 0); WR32(B_PREV(nn), b);
            memset(GP(p + old), 0, n - old); WR32(B_REQ(b), n); split(b, need); return p;
        }
    } else if (need <= bsize(b)) { if (n > old) memset(GP(p + old), 0, n - old); WR32(B_REQ(b), n); return p; }
    uint32_t q = g_alloc(n); if (!q) return 0;
    memcpy(GP(q), GP(p), old < n ? old : n); g_free(p); return q;
}
uint32_t g_str(const char *s) { size_t n = strlen(s) + 1; uint32_t g = g_alloc((uint32_t)n); if (g) memcpy(GP(g), s, n); return g; }
void heap_check(void) {
    for (int i = 0; i < narenas; i++) {
        uint32_t b = arenas[i], prev = 0;
        for (;;) {
            if (RD32(B_MAGIC(b)) != (MAGIC ^ b) || RD32(B_PREV(b)) != prev) bad(b + HDR, "corrupted heap (heap_check)");
            if (bflags(b) & F_END) break;
            if (!bsize(b)) bad(b + HDR, "zero-sized block");
            prev = b; b = next_phys(b);
        }
    }
}
