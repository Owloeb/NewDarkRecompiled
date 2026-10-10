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
 * To add one: write `int fp_XXXXXXXX(CPU *c)` and list it in FASTPATHS at the bottom. */
#include "rt.h"

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
