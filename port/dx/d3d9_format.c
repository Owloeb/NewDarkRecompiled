/* d3d9_format.c: Direct3D 9 surface formats: sizes, conversion to and from RGBA8 (R, G, B, A bytes), DXT decoding and
 * box-filtered mipmaps. Everything a backend receives is RGBA8 or (when it says it can) DXT1/3/5 as stored. */
#include <stdlib.h>
#include "core.h"
#include "d3d9_format.h"

int fmt_known(uint32_t f) {
    switch (f) { case FMT_A8R8G8B8: case FMT_X8R8G8B8: case FMT_R8G8B8: case FMT_R5G6B5: case FMT_X1R5G5B5: case FMT_A1R5G5B5: case FMT_A4R4G4B4: case FMT_X4R4G4B4:
                 case FMT_A8: case FMT_L8: case FMT_A8L8: case FMT_P8: case FMT_A8B8G8R8: case FMT_X8B8G8R8: case FMT_DXT1: case FMT_DXT2: case FMT_DXT3: case FMT_DXT4: case FMT_DXT5:
                 case FMT_D16: case FMT_D24S8: case FMT_D24X8: case FMT_D32: case FMT_D16_LOCKABLE: case FMT_INDEX16: case FMT_INDEX32: return 1; }
    return 0;
}
int fmt_is_dxt(uint32_t f) { return f == FMT_DXT1 || f == FMT_DXT2 || f == FMT_DXT3 || f == FMT_DXT4 || f == FMT_DXT5; }
int fmt_is_depth(uint32_t f) { return f == FMT_D16 || f == FMT_D24S8 || f == FMT_D24X8 || f == FMT_D32 || f == FMT_D16_LOCKABLE || f == FMT_D15S1 || f == FMT_D24X4S4 || f == FMT_D32F_LOCKABLE || f == FMT_D24FS8; }
int fmt_bpp(uint32_t f) {     /* bytes per pixel; 0 for block formats */
    switch (f) {
    case FMT_A8: case FMT_L8: case FMT_P8: return 1;
    case FMT_R5G6B5: case FMT_X1R5G5B5: case FMT_A1R5G5B5: case FMT_A4R4G4B4: case FMT_X4R4G4B4: case FMT_A8L8: case FMT_D16: case FMT_D16_LOCKABLE: case FMT_D15S1: case FMT_INDEX16: return 2;
    case FMT_R8G8B8: return 3;
    default: return fmt_is_dxt(f) ? 0 : 4;
    }
}
void fmt_layout(uint32_t f, uint32_t w, uint32_t h, uint32_t *pitch, uint32_t *size) {
    if (!w) w = 1; if (!h) h = 1;
    if (fmt_is_dxt(f)) { uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4; *pitch = bw * (f == FMT_DXT1 ? 8 : 16); *size = *pitch * bh; return; }
    *pitch = w * (uint32_t)fmt_bpp(f); *pitch = (*pitch + 3) & ~3u; *size = *pitch * h;
}
static uint8_t x5(uint32_t v) { return (uint8_t)((v << 3) | (v >> 2)); }
static uint8_t x6(uint32_t v) { return (uint8_t)((v << 2) | (v >> 4)); }
static uint8_t x4(uint32_t v) { return (uint8_t)(v * 17); }
static void rgb565(uint16_t c, uint8_t *o) { o[0] = x5(c >> 11); o[1] = x6((c >> 5) & 63); o[2] = x5(c & 31); }
static void dxt_color_block(const uint8_t *b, uint8_t out[16][4], int dxt1) {
    uint16_t c0 = (uint16_t)(b[0] | b[1] << 8), c1 = (uint16_t)(b[2] | b[3] << 8); uint8_t pal[4][4];
    rgb565(c0, pal[0]); rgb565(c1, pal[1]); pal[0][3] = pal[1][3] = 255;
    if (!dxt1 || c0 > c1) { for (int i = 0; i < 3; i++) { pal[2][i] = (uint8_t)((2 * pal[0][i] + pal[1][i]) / 3); pal[3][i] = (uint8_t)((pal[0][i] + 2 * pal[1][i]) / 3); } pal[2][3] = pal[3][3] = 255; }
    else { for (int i = 0; i < 3; i++) { pal[2][i] = (uint8_t)((pal[0][i] + pal[1][i]) / 2); pal[3][i] = 0; } pal[2][3] = 255; pal[3][3] = 0; }
    uint32_t bits = (uint32_t)b[4] | (uint32_t)b[5] << 8 | (uint32_t)b[6] << 16 | (uint32_t)b[7] << 24;
    for (int i = 0; i < 16; i++) memcpy(out[i], pal[(bits >> (2 * i)) & 3], 4);
}
static void dxt_decode(uint32_t f, const uint8_t *src, uint32_t pitch, uint32_t w, uint32_t h, uint8_t *dst, uint32_t dpitch) {
    uint32_t bs = f == FMT_DXT1 ? 8 : 16;
    for (uint32_t by = 0; by < (h + 3) / 4; by++) for (uint32_t bx = 0; bx < (w + 3) / 4; bx++) {
        const uint8_t *b = src + by * pitch + bx * bs; uint8_t px[16][4];
        dxt_color_block(b + bs - 8, px, f == FMT_DXT1);
        if (f == FMT_DXT2 || f == FMT_DXT3) for (int i = 0; i < 16; i++) px[i][3] = x4((b[i / 2] >> (4 * (i & 1))) & 15);
        else if (f == FMT_DXT4 || f == FMT_DXT5) {
            uint8_t a[8]; a[0] = b[0]; a[1] = b[1];
            if (a[0] > a[1]) for (int i = 1; i < 7; i++) a[i + 1] = (uint8_t)(((7 - i) * a[0] + i * a[1]) / 7);
            else { for (int i = 1; i < 5; i++) a[i + 1] = (uint8_t)(((5 - i) * a[0] + i * a[1]) / 5); a[6] = 0; a[7] = 255; }
            uint64_t bits = 0; for (int i = 0; i < 6; i++) bits |= (uint64_t)b[2 + i] << (8 * i);
            for (int i = 0; i < 16; i++) px[i][3] = a[(bits >> (3 * i)) & 7];
        }
        for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
            uint32_t X = bx * 4 + (uint32_t)x, Y = by * 4 + (uint32_t)y; if (X >= w || Y >= h) continue;
            memcpy(dst + Y * dpitch + X * 4, px[y * 4 + x], 4);
        }
    }
}
/* src (format f, pitch bytes per row) -> RGBA8 rows of w * 4 bytes. palette: 256 PALETTEENTRY (R, G, B, flags) for P8 */
void fmt_to_rgba8(uint32_t f, const uint8_t *src, uint32_t pitch, uint32_t w, uint32_t h, uint8_t *dst, const uint8_t *palette) {
    if (fmt_is_dxt(f)) { dxt_decode(f, src, pitch, w, h, dst, w * 4); return; }
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *s = src + y * pitch; uint8_t *o = dst + y * w * 4;
        for (uint32_t x = 0; x < w; x++, o += 4) {
            uint32_t v;
            switch (f) {
            case FMT_A8R8G8B8: o[0] = s[4 * x + 2]; o[1] = s[4 * x + 1]; o[2] = s[4 * x]; o[3] = s[4 * x + 3]; break;
            case FMT_X8R8G8B8: o[0] = s[4 * x + 2]; o[1] = s[4 * x + 1]; o[2] = s[4 * x]; o[3] = 255; break;
            case FMT_A8B8G8R8: memcpy(o, s + 4 * x, 4); break;
            case FMT_X8B8G8R8: memcpy(o, s + 4 * x, 3); o[3] = 255; break;
            case FMT_R8G8B8: o[0] = s[3 * x + 2]; o[1] = s[3 * x + 1]; o[2] = s[3 * x]; o[3] = 255; break;
            case FMT_R5G6B5: v = (uint32_t)(s[2 * x] | s[2 * x + 1] << 8); rgb565((uint16_t)v, o); o[3] = 255; break;
            case FMT_X1R5G5B5: case FMT_A1R5G5B5: v = (uint32_t)(s[2 * x] | s[2 * x + 1] << 8); o[0] = x5((v >> 10) & 31); o[1] = x5((v >> 5) & 31); o[2] = x5(v & 31); o[3] = f == FMT_X1R5G5B5 || (v & 0x8000) ? 255 : 0; break;
            case FMT_A4R4G4B4: case FMT_X4R4G4B4: v = (uint32_t)(s[2 * x] | s[2 * x + 1] << 8); o[0] = x4((v >> 8) & 15); o[1] = x4((v >> 4) & 15); o[2] = x4(v & 15); o[3] = f == FMT_X4R4G4B4 ? 255 : x4(v >> 12); break;
            case FMT_A8: o[0] = o[1] = o[2] = 255; o[3] = s[x]; break;
            case FMT_L8: o[0] = o[1] = o[2] = s[x]; o[3] = 255; break;
            case FMT_A8L8: o[0] = o[1] = o[2] = s[2 * x]; o[3] = s[2 * x + 1]; break;
            case FMT_P8: if (palette) { const uint8_t *p = palette + 4 * s[x]; o[0] = p[0]; o[1] = p[1]; o[2] = p[2]; o[3] = p[3]; } else { o[0] = o[1] = o[2] = s[x]; o[3] = 255; } break;
            default: o[0] = o[1] = o[2] = o[3] = 255; break;
            }
        }
    }
}
/* RGBA8 -> format f (uncompressed formats only); returns 0 if f cannot be written */
int fmt_from_rgba8(uint32_t f, const uint8_t *src, uint32_t w, uint32_t h, uint8_t *dst, uint32_t pitch) {
    if (fmt_is_dxt(f) || f == FMT_P8) return 0;
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *s = src + y * w * 4; uint8_t *o = dst + y * pitch;
        for (uint32_t x = 0; x < w; x++, s += 4) {
            uint32_t r = s[0], g = s[1], b = s[2], a = s[3], v;
            switch (f) {
            case FMT_A8R8G8B8: case FMT_X8R8G8B8: o[4 * x] = (uint8_t)b; o[4 * x + 1] = (uint8_t)g; o[4 * x + 2] = (uint8_t)r; o[4 * x + 3] = (uint8_t)(f == FMT_X8R8G8B8 ? 255 : a); break;
            case FMT_A8B8G8R8: case FMT_X8B8G8R8: memcpy(o + 4 * x, s, 4); break;
            case FMT_R8G8B8: o[3 * x] = (uint8_t)b; o[3 * x + 1] = (uint8_t)g; o[3 * x + 2] = (uint8_t)r; break;
            case FMT_R5G6B5: v = (r >> 3) << 11 | (g >> 2) << 5 | b >> 3; o[2 * x] = (uint8_t)v; o[2 * x + 1] = (uint8_t)(v >> 8); break;
            case FMT_X1R5G5B5: case FMT_A1R5G5B5: v = (a >= 128 || f == FMT_X1R5G5B5 ? 0x8000u : 0) | (r >> 3) << 10 | (g >> 3) << 5 | b >> 3; o[2 * x] = (uint8_t)v; o[2 * x + 1] = (uint8_t)(v >> 8); break;
            case FMT_A4R4G4B4: case FMT_X4R4G4B4: v = (a >> 4) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | b >> 4; o[2 * x] = (uint8_t)v; o[2 * x + 1] = (uint8_t)(v >> 8); break;
            case FMT_A8: o[x] = (uint8_t)a; break;
            case FMT_L8: o[x] = (uint8_t)((r * 77 + g * 151 + b * 28) >> 8); break;
            case FMT_A8L8: o[2 * x] = (uint8_t)((r * 77 + g * 151 + b * 28) >> 8); o[2 * x + 1] = (uint8_t)a; break;
            default: return 0;
            }
        }
    }
    return 1;
}
/* 2x2 box filter of an RGBA8 image (w x h) into (w/2 x h/2, at least 1) */
void rgba8_half(const uint8_t *src, uint32_t w, uint32_t h, uint8_t *dst) {
    uint32_t nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;
    for (uint32_t y = 0; y < nh; y++) for (uint32_t x = 0; x < nw; x++) for (int ch = 0; ch < 4; ch++) {
        uint32_t x0 = x * 2 < w ? x * 2 : w - 1, x1 = x * 2 + 1 < w ? x * 2 + 1 : x0, y0 = y * 2 < h ? y * 2 : h - 1, y1 = y * 2 + 1 < h ? y * 2 + 1 : y0;
        uint32_t s = src[(y0 * w + x0) * 4 + ch] + src[(y0 * w + x1) * 4 + ch] + src[(y1 * w + x0) * 4 + ch] + src[(y1 * w + x1) * 4 + ch];
        dst[(y * nw + x) * 4 + ch] = (uint8_t)((s + 2) / 4);
    }
}
