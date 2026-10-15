/* Optional fast paths for the portable host (python3 port/build.py ... --fastpaths).
 *
 * Each function below does exactly what one small, very hot recompiled engine function does, but as plain C instead of
 * instruction-by-instruction recompiled code. With --fastpaths, tools/apply_hooks.py makes the recompiled function try its
 * fast path first on entry: a fast path returns 1 when it handled the call (it has set the result registers and popped the
 * return address, as the original would) or 0 to let the recompiled code run unchanged (unusual input).
 *
 * They change speed only, never behaviour. Each one notes what the original does and which registers it leaves different
 * from the original (only ones the game's compiler treats as free to clobber after a call). Off by default.
 *
 * To add one: write `int fp_XXXXXXXX(CPU *c)` (tools/apply_hooks.py finds it by name). Check it against the game's own code with
 * port/tests/fastpaths/check.py (needs a tools/where.py dump of the function; nothing from the game goes into the repository). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"
void port_warn(const char *fmt, ...);

/* 0x006a17d0: case-insensitive string hash. eax = NUL-terminated string. Two interleaved 8-bit table lookups
 * (table of dwords at 0x7df4a8), returns (hi << 8) | lo in eax. Calls toupper (C locale: ASCII only, as the host's toupper). */
static inline uint32_t up_(uint32_t ch) { return (ch >= 'a' && ch <= 'z') ? ch - 32 : ch; }
int fp_006a17d0(CPU *c) {
    uint32_t s = c->eax, hi = 0, lo = 0, a = up_(RD8(s));
    if (a) for (;;) {
        a ^= hi; hi = RD32(0x7df4a8u + a * 4);
        a = up_(RD8(s + 1)); s += 2;
        if (!a) break;
        a ^= lo; lo = RD32(0x7df4a8u + a * 4);
        a = up_(RD8(s));
        if (!a) break;
    }
    RETCHK(c); c->esp += 4;
    c->eax = (hi << 8) | lo;
    return 1;
}

/* 0x006f7206: the C runtime's float-to-int helper (_ftol): truncates st(0) to a 64-bit integer in edx:eax and pops it.
 * Handled only for a finite value below 2^52 with the default rounding mode; anything else runs the original. */
int fp_006f7206(CPU *c) {
    double x = ST(0);
    if (!(x > -4503599627370496.0 && x < 4503599627370496.0) || (c->cw & 0x0C00)) return 0;   /* also rejects NaN */
    int64_t v = (int64_t)x;                                                                   /* truncates toward zero */
    FPOP(c);
    RETCHK(c); c->esp += 4;
    c->eax = (uint32_t)v; c->edx = (uint32_t)((uint64_t)v >> 32);
    return 1;
}

/* ---- the engine's hash tables (hot in combat: AI queries object properties and links through them) ----
 * Table layout (both key sizes): +4 size (slots), +8 a second modulus for the probe step, +0x14 slot states (byte: 0 empty,
 * 1 deleted, 2 used), +0x18 slot array. Probing: start at hash % size; step = 1 << (hash % [+8]); wrap by subtracting size;
 * at most `size` probes; an empty slot ends the search. */
static inline uint32_t T_(uint32_t i) { return RD32(0x7df4a8u + i * 4u); }   /* the engine's 256-entry byte-hash table */

/* 0x006a1960: hash of the 32-bit integer in eax (register argument), result in eax. Leaves ecx and edx as the original. */
int fp_006a1960(CPU *c) {
    uint32_t x = c->eax, b0 = x & 0xff, b1 = (x >> 8) & 0xff, b2 = (x >> 16) & 0xff, b3 = x >> 24;
    uint32_t e1 = T_(b0), s = T_(T_(b1) ^ b0), d = T_(b1 ^ e1), a = T_(b3 ^ d), cc = b2 ^ s;
    RETCHK(c); c->esp += 4;
    c->eax = (a << 8) | T_(cc); c->ecx = cc; c->edx = d;
    return 1;
}
/* 0x006a19d0: hash of the eax bytes at ecx (register arguments; eax >= 1), result in eax. Leaves ecx and edx as the original. */
static uint32_t hashn_(uint32_t n, uint32_t *pp, uint32_t *pedx) {
    uint32_t p = *pp, a = 0, b = 0, edx;
    for (;;) {
        edx = RD8(p) ^ a; a = T_(edx); uint32_t t = --n; p++; n--;
        if (!t) { edx = 0; break; }
        edx = RD8(p) ^ b; b = T_(edx); p++;
        if (!n) break;
    }
    *pp = p; *pedx = edx; return (a << 8) | b;
}
int fp_006a19d0(CPU *c) {
    uint32_t n = c->eax, p = c->ecx, edx; if (n == 0 || n > (1u << 24)) return 0;   /* 0 would loop ~2^32 times in the original */
    uint32_t h = hashn_(n, &p, &edx);
    RETCHK(c); c->esp += 4;
    c->eax = h; c->ecx = p; c->edx = edx;
    return 1;
}
/* probe for a key; returns 1 found / 0 not, *slot = where the search stopped; -1: let the original run (a division by zero) */
static int probe_(uint32_t tbl, uint32_t h, uint32_t esz, uint32_t k0, uint32_t k1, int two, uint32_t *slot) {
    uint32_t size = RD32(tbl + 4); if ((int32_t)size <= 0) return -1;          /* 0: the original faults; negative: rare, original */
    uint32_t i = h % size, step = 0, states = RD32(tbl + 0x14), ents = RD32(tbl + 0x18);
    for (int32_t n = 0; n < (int32_t)size; n++) {
        uint8_t st = RD8(states + i);
        if (!st) { *slot = i; return 0; }
        if (st == 2) { uint32_t e = ents + i * esz; if (RD32(e) == k0 && (!two || RD32(e + 4) == k1)) { *slot = i; return 1; } }
        if (!step) { uint32_t m = RD32(tbl + 8); if (!m) return -1; step = 1u << ((h % m) & 31); }
        i += step; while ((int32_t)i >= (int32_t)size) i -= size;
    }
    *slot = i; return 0;
}
/* 0x00496010: find a 4-byte key in a table. eax = pointer to the key; stack: table, out (stdcall, ret 8). *out = the slot
 * where the search stopped; eax = 1 if found. ecx/edx differ from the original (the caller treats them as clobbered). */
int fp_00496010(CPU *c) {
    uint32_t key = RD32(c->eax), tbl = RD32(c->esp + 4), out = RD32(c->esp + 8), slot;
    uint32_t x = key, b0 = x & 0xff, b1 = (x >> 8) & 0xff, b2 = (x >> 16) & 0xff, b3 = x >> 24;
    uint32_t h = (T_(b3 ^ T_(b1 ^ T_(b0))) << 8) | T_(b2 ^ T_(T_(b1) ^ b0));   /* = fp_006a1960 */
    int r = probe_(tbl, h, 8, key, 0, 0, &slot); if (r < 0) return 0;
    WR32(out, slot);
    RETCHK(c); c->esp += 12;
    c->eax = (uint32_t)r; c->ecx = out; c->edx = 0;
    return 1;
}
/* 0x00536430: find an 8-byte key in a table with 12-byte entries. stack: table, key pointer, out (stdcall, ret 0xc). */
int fp_00536430(CPU *c) {
    uint32_t tbl = RD32(c->esp + 4), kp = RD32(c->esp + 8), out = RD32(c->esp + 12), slot, p = kp, edx;
    uint32_t h = hashn_(8, &p, &edx);
    int r = probe_(tbl, h, 12, RD32(kp), RD32(kp + 4), 1, &slot); if (r < 0) return 0;
    WR32(out, slot);
    RETCHK(c); c->esp += 16;
    c->eax = (uint32_t)r; c->ecx = out; c->edx = 0;
    return 1;
}
/* 0x0057f640: eax = 1 if the 8 bytes at ecx and edx are equal (register arguments). edx = eax, as the original; ecx differs. */
int fp_0057f640(CPU *c) {
    uint32_t eq = RD32(c->ecx) == RD32(c->edx) && RD32(c->ecx + 4) == RD32(c->edx + 4);
    RETCHK(c); c->esp += 4;
    c->eax = eq; c->edx = eq;
    return 1;
}
/* 0x006f71d0: _ftol2. With the SSE2 flag at 0xa4ebc8 clear (rt_cpuid reports no SSE2) it jumps to _ftol (0x006f7206)
 * inside the same recompiled function, so fp_006f7206 never sees those calls: the same conversion here. */
int fp_006f71d0(CPU *c) {
    if (RD32(0xa4ebc8u)) return 0;
    double x = ST(0);
    if (!(x > -4503599627370496.0 && x < 4503599627370496.0) || (c->cw & 0x0C00)) return 0;
    int64_t v = (int64_t)x;
    FPOP(c);
    RETCHK(c); c->esp += 4;
    c->eax = (uint32_t)v; c->edx = (uint32_t)((uint64_t)v >> 32);
    return 1;
}
/* 0x006ebc90: copy an 8-bit bitmap onto the software canvas, skipping pixels equal to 0 (colour key).
 * cdecl (bitmap, x, y). bitmap: bits at +0, width (int16) at +8, height (int16) at +0xa, row stride (uint16) at +0xc.
 * canvas descriptor at [0xa523e8]: bits at +0, row pitch (uint16) at +0xc. Leaves ecx = width, as the original does. */
int fp_006ebc90(CPU *c) {
    uint32_t sp = c->esp, bm = RD32(sp + 4), x = RD32(sp + 8), y = RD32(sp + 12), G = RD32(0xa523e8u);
    int32_t w = (int16_t)RD16(bm + 8), h = (int16_t)RD16(bm + 0xa);
    if (w < 0 || h < 0) return 0;
    uint32_t pitch = RD16(G + 0xc), dst = pitch * y + x + RD32(G), src = RD32(bm), sstride = RD16(bm + 0xc);
    for (int32_t r = 0; r < h; r++, src += sstride, dst += pitch)
        for (int32_t i = 0; i < w; i++) { uint8_t p = RD8(src + (uint32_t)i); if (p) WR8(dst + (uint32_t)i, p); }
    RETCHK(c); c->esp += 4;
    c->eax = dst; c->edx = bm; c->ecx = (uint32_t)w;
    return 1;
}

/* ---------------------------------------------------------------- entry traces (SS2PORT_DRAWTRACE=1)
 * Not fast paths: each of these only records who called the function (the call chain recovered from the guest stack,
 * as dx/d3d9.c's draw trace does) and returns 0, so the original always runs and nothing changes. They exist to find
 * the engine code that lights objects, upstream of the draws the draw trace sees. Logged with the draw trace. */
typedef struct { uint32_t fn, chain[6]; int n; uint64_t calls; uint32_t arg[3]; } EntryRow;
static EntryRow *entry_rows; static int entry_n, entry_on = -1;
static int entry_retaddr(uint32_t v) {
    if (v < 0x00401006u || v >= 0x00780000u) return 0;
    const uint8_t *p = GP(v);
    return p[-5] == 0xE8 || (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10) || (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10) || (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10);
}
static int entry_trace(CPU *c, uint32_t fn) {
    if (entry_on < 0) { const char *e = getenv("SS2PORT_DRAWTRACE"); entry_on = e && *e && *e != '0'; }
    if (!entry_on) return 0;
    EntryRow r; memset(&r, 0, sizeof r); r.fn = fn;
    for (uint32_t a = c->esp; r.n < 6 && a < c->esp + 2048; a += 4) { uint32_t v = RD32(a); if (entry_retaddr(v)) r.chain[r.n++] = v; }
    for (int i = 0; i < entry_n; i++) if (entry_rows[i].fn == fn && entry_rows[i].n == r.n && !memcmp(entry_rows[i].chain, r.chain, sizeof r.chain)) { entry_rows[i].calls++; return 0; }
    if (entry_n >= 2048) return 0;
    r.calls = 1; for (int k = 0; k < 3; k++) r.arg[k] = RD32(c->esp + 4 + 4 * (uint32_t)k);
    entry_rows = realloc(entry_rows, (size_t)(entry_n + 1) * sizeof *entry_rows); entry_rows[entry_n++] = r;
    return 0;
}
void port_entrytrace_dump(void) {
    for (int i = 0; i < entry_n; i++) {
        EntryRow *q = &entry_rows[i]; char ch[96]; int o = 0;
        for (int k = 0; k < q->n; k++) o += snprintf(ch + o, sizeof ch - (size_t)o, " %06x", q->chain[k]);
        port_warn("ENTRYTRACE %06x %9llu first args %08x %08x %08x callers:%s", q->fn, (unsigned long long)q->calls, q->arg[0], q->arg[1], q->arg[2], ch);
    }
}
int fp_006380a0(CPU *c) { return entry_trace(c, 0x6380a0); }   /* draws a list of polygons (items of 0x164 bytes) through the renderer interface */
int fp_00640f00(CPU *c) { return entry_trace(c, 0x640f00); }   /* renderer interface (vtable 0x77efb0) slot 0x48 */
int fp_006410e0(CPU *c) { return entry_trace(c, 0x6410e0); }   /* slot 0x4c */
int fp_006412e0(CPU *c) { return entry_trace(c, 0x6412e0); }   /* slot 0x50 */
int fp_006414f0(CPU *c) { return entry_trace(c, 0x6414f0); }   /* slot 0x54 */
int fp_00641760(CPU *c) { return entry_trace(c, 0x641760); }   /* slot 0x58 */
int fp_00637650(CPU *c) { return entry_trace(c, 0x637650); }   /* float-heavy helper of 0x6380a0 */

/* ---------------------------------------------------------------- per-pixel lighting: what the light callback sees
 * SS2PORT_PPL_LIGHTS=1: entry hook on obj_light_callback (0x5ed7d0, symbols/manual.sym) that writes the inputs of a few
 * calls (after the game has run a while) to ppl_lights.txt: the light lists, the object transform and the first vertices
 * with their transformed points, so the light and view transforms can be worked out offline. Returns 0: the game is
 * unchanged. */
static void dump_words(FILE *f, const char *tag, uint32_t a, uint32_t n) {
    fprintf(f, "%s %08x", tag, a);
    for (uint32_t i = 0; i < n; i++) { uint32_t v = RD32(a + 4 * i); float x; memcpy(&x, &v, 4); fprintf(f, " %08x:%.6g", v, x); }
    fprintf(f, "\n");
}
int fp_005ed7d0(CPU *c) {
    static int on = -1; static uint32_t calls; static FILE *f;
    if (on < 0) { const char *e = getenv("SS2PORT_PPL_LIGHTS"); on = e && *e && *e != '0'; }
    if (!on || ++calls < 20000 || calls > 20012) { if (f && calls > 20012) { fclose(f); f = NULL; port_warn("per-pixel lighting: wrote ppl_lights.txt"); } return 0; }
    if (!f && !(f = fopen("ppl_lights.txt", "w"))) { on = 0; return 0; }
    uint32_t sp = c->esp, n = RD32(sp + 4), out = RD32(sp + 8), ent = RD32(sp + 12), verts = RD32(sp + 16), pts = RD32(sp + 20);
    uint32_t ctx = RD32(0xa458b0u), np = RD32(0x940458u), nd = RD32(0x94045cu);
    fprintf(f, "CALL %u count=%u out=%08x entries=%08x verts=%08x points=%08x ctx=%08x pointlights=%u otherlights=%u rgb=%u selfillum_flag=%u lightsdirty=%u\n",
            calls, n, out, ent, verts, pts, ctx, np, nd, RD8(0xa3acdeu), RD8(0xa459b4u), RD32(0x7e7bf0u));
    dump_words(f, "CAM", 0xa57e00u, 8);
    dump_words(f, "CTX", ctx, 64);
    dump_words(f, "WLIGHTS", 0x93fe48u, 12 * (np < 8 ? np : 8));
    dump_words(f, "OLIGHTS", 0x940468u, 12 * (np < 8 ? np : 8));
    dump_words(f, "AROUND", 0x9403f0u, 32);
    dump_words(f, "MODEL", RD32(0xa4597cu), 32);
    for (uint32_t i = 0; i < n && i < 10; i++) {
        uint32_t e = ent + 8 * i, pi = RD16(e + 2);
        fprintf(f, "ENTRY %u point=%u normal=%08x\n", i, pi, RD32(e + 4));
        dump_words(f, " VERT", verts + 12 * pi, 3);
    }
    dump_words(f, "POINTS", pts, 64);
    dump_words(f, "PTOFFS", RD32(0xa45ee0u), 16);
    return 0;
}
