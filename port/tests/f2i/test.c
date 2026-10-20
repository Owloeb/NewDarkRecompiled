/* port/tests/f2i/test.c: fist/fistp conversions (F2I32, F2I16 in runtime/rt.h) against the libm version they replace
 * (FROUND, then a range check on the rounded value), in all four x87 rounding modes. Every bound of every mode is probed
 * with the neighbouring doubles and the halfway points, plus NaN, infinities, zeros, subnormals and random values from
 * 2^-30 to 2^64. port/tests/f2i/run.sh builds it natively and, when a cross compiler and qemu-arm are installed, for
 * 32-bit ARM with hard float (the Vita's ABI). */
#include <stdio.h>
#include <stdlib.h>
#include "rt.h"

uint8_t *M;

static uint32_t ref32(CPU *c, double v) {
    double r = FROUND(c, v);
    if (!(r >= -2147483648.0 && r <= 2147483647.0)) return 0x80000000u;
    return (uint32_t)(int32_t)r;
}
static uint16_t ref16(CPU *c, double v) {
    double r = FROUND(c, v);
    if (!(r >= -32768.0 && r <= 32767.0)) return 0x8000u;
    return (uint16_t)(int16_t)r;
}

static long checked, failed;
static void check(double v) {
    CPU c; memset(&c, 0, sizeof c);
    for (int rc = 0; rc < 4; rc++) {
        c.cw = (uint16_t)(0x027F | (rc << 10));
        uint32_t a = F2I32(&c, v), b = ref32(&c, v);
        uint16_t x = F2I16(&c, v), y = ref16(&c, v);
        checked += 2;
        if (a != b && failed++ < 20) printf("F2I32 rc=%d v=%.17g (%a): got %08x, want %08x\n", rc, v, v, a, b);
        if (x != y && failed++ < 20) printf("F2I16 rc=%d v=%.17g (%a): got %04x, want %04x\n", rc, v, v, x, y);
    }
}
static void around(double b) {                                 /* b, its neighbours, and the halfway points next to it */
    double d[] = { 0, 0.5, -0.5, 1, -1, 0.25, -0.25, 0.75, -0.75, 1.5, -1.5 };
    for (unsigned i = 0; i < sizeof d / sizeof *d; i++) {
        double v = b + d[i], u = v, w = v;
        for (int k = 0; k < 4; k++) { check(u); check(w); u = nextafter(u, INFINITY); w = nextafter(w, -INFINITY); }
    }
}
static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t next(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

int main(void) {
    double specials[] = { NAN, -NAN, INFINITY, -INFINITY, 0.0, -0.0, 4.9e-324, -4.9e-324, 2.2250738585072014e-308,
                          1e300, -1e300, 4503599627370496.0, -4503599627370496.0, 6755399441055744.0, 9.2233720368547758e18 };
    for (unsigned i = 0; i < sizeof specials / sizeof *specials; i++) check(specials[i]);
    double bounds[] = { 0, 2147483647.0, -2147483648.0, 2147483648.0, -2147483649.0, 32767.0, -32768.0, 32768.0, -32769.0,
                        65535.0, 65536.0, 4294967295.0, 4294967296.0, 2251799813685248.0, -2251799813685248.0 };
    for (unsigned i = 0; i < sizeof bounds / sizeof *bounds; i++) around(bounds[i]);
    for (int i = -100; i <= 100; i++) around(i);
    for (long i = 0; i < 2000000; i++) {
        uint64_t r = next();
        double m = (double)(r >> 11) / 9007199254740992.0;             /* [0, 1) */
        int e = (int)(next() % 95) - 30;                               /* 2^-30 .. 2^64 */
        double v = ldexp(m, e); if (r & 1) v = -v;
        check(v);
        if ((i & 7) == 0) check(nearbyint(v * 2) / 2);                 /* exact halves */
        uint64_t bits = next(); double raw; memcpy(&raw, &bits, 8); check(raw);   /* any bit pattern */
    }
#ifdef RT_F2I_FAST
    const char *path = "fast";
#else
    const char *path = "libm";
#endif
    printf("f2i (%s path): %ld checks, %ld failed\n", path, checked, failed);
    return failed != 0;
}
