/* ddraw.c: DirectDraw, only as far as the engine's start-up uses it to detect the display hardware. */
#include "com.h"
static void dd_caps(CPU *c, ComObj *s) {
    (void)s;
    for (int i = 1; i <= 2; i++) { uint32_t p = A(i); if (!p) continue; uint32_t sz = RD32(p); if (sz < 68) continue;
        memset(GP(p) + 4, 0, sz - 4); WR32(p + 4, 0x95C47A41u); WR32(p + 8, 0xA06AB210u); WR32(p + 60, 256u << 20); WR32(p + 64, 256u << 20); }
    RET(0);
}
static void dd_displaymode(CPU *c, ComObj *s) {
    (void)s; uint32_t p = A(1), sz = RD32(p); if (sz < 108) { RET(0x80070057u); return; } int w, h; plat_video_display_size(&w, &h);
    memset(GP(p) + 4, 0, sz - 4); WR32(p + 4, 0x100F); WR32(p + 8, (uint32_t)h); WR32(p + 12, (uint32_t)w); WR32(p + 16, (uint32_t)w * 4); WR32(p + 24, 60);
    WR32(p + 72, 32); WR32(p + 76, 0x40); WR32(p + 84, 32); WR32(p + 88, 0xFF0000); WR32(p + 92, 0xFF00); WR32(p + 96, 0xFF); RET(0);
}
static const ComMethod dd_m[] = { { "GetCaps", dd_caps }, { "GetDisplayMode", dd_displaymode }, { 0, 0 } };
static const uint8_t iids_dd[][16] = { IID_IDirectDraw };
ComClass c_dd = { "IDirectDraw", IFACE_IDirectDraw, iids_dd, 1, dd_m, 0, NULL };
SHIM(DirectDrawCreate) { if (!A(1)) { RET(0x80070057u); return; } WR32(A(1), com_create(&c_dd, NULL)->guest); RET(0); }
SHIM(DirectDrawEnumerateA) { g_call(c, A(0), 4, 0u, g_str("Primary Display Driver"), g_str("display"), A(1)); RET(0); }
const ShimDef ddraw_shims[] = { { "DirectDrawCreate", sh_DirectDrawCreate, STD(3) }, { "DirectDrawEnumerateA", sh_DirectDrawEnumerateA, STD(2) }, { 0, 0, 0 } };
