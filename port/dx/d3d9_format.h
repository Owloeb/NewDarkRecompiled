/* d3d9_format.h: D3DFORMAT values and the conversions in d3d9_format.c */
#ifndef D3D9_FORMAT_H
#define D3D9_FORMAT_H
#include <stdint.h>
enum {
    FMT_UNKNOWN = 0, FMT_R8G8B8 = 20, FMT_A8R8G8B8 = 21, FMT_X8R8G8B8 = 22, FMT_R5G6B5 = 23, FMT_X1R5G5B5 = 24, FMT_A1R5G5B5 = 25, FMT_A4R4G4B4 = 26,
    FMT_A8 = 28, FMT_X4R4G4B4 = 30, FMT_A8B8G8R8 = 32, FMT_X8B8G8R8 = 33, FMT_P8 = 41, FMT_L8 = 50, FMT_A8L8 = 51,
    FMT_D16_LOCKABLE = 70, FMT_D32 = 71, FMT_D15S1 = 73, FMT_D24S8 = 75, FMT_D24X8 = 77, FMT_D24X4S4 = 79, FMT_D16 = 80, FMT_D32F_LOCKABLE = 82, FMT_D24FS8 = 83,
    FMT_INDEX16 = 101, FMT_INDEX32 = 102,
    FMT_DXT1 = 0x31545844, FMT_DXT2 = 0x32545844, FMT_DXT3 = 0x33545844, FMT_DXT4 = 0x34545844, FMT_DXT5 = 0x35545844
};
int  fmt_known(uint32_t f);
int  fmt_is_dxt(uint32_t f);
int  fmt_is_depth(uint32_t f);
int  fmt_bpp(uint32_t f);
void fmt_layout(uint32_t f, uint32_t w, uint32_t h, uint32_t *pitch, uint32_t *size);
void fmt_to_rgba8(uint32_t f, const uint8_t *src, uint32_t pitch, uint32_t w, uint32_t h, uint8_t *dst, const uint8_t *palette);
int  fmt_from_rgba8(uint32_t f, const uint8_t *src, uint32_t w, uint32_t h, uint8_t *dst, uint32_t pitch);
void rgba8_half(const uint8_t *src, uint32_t w, uint32_t h, uint8_t *dst);
#endif
