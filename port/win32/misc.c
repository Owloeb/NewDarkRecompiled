/* misc.c: GDI32, ADVAPI32, SHELL32, ole32 and VERSION. The engine draws everything through Direct3D, so GDI only has to
 * answer the questions its start-up and its 2D fallback ask; there is no registry, shell or COM class factory. */
#include <stdio.h>
#include "core.h"

/* ---------------------------------------------------------------- GDI32 */
SHIM(GetDeviceCaps) {
    int dw, dh; plat_video_display_size(&dw, &dh);
    switch (A(1)) { case 8: RET((uint32_t)dw); break; case 10: RET((uint32_t)dh); break; case 12: RET(32); break; case 14: RET(1); break; case 24: RET(0xFFFFFFFFu); break;
                    case 88: case 90: RET(96); break; case 104: RET(0); break; case 116: RET(60); break; case 118: RET((uint32_t)dw); break; case 117: RET((uint32_t)dh); break; default: RET(0); }
}
SHIM(GetObjectA) { if (A(2)) memset(GP(A(2)), 0, A(1)); RET(0); }
SHIM(GetStockObject) { RET(0x50001 + A(0)); }
SHIM(CreateCompatibleDC) { RET(0x30002); }
SHIM(CreateDIBSection) { if (A(3)) WR32(A(3), 0); RET(0); }        /* ppvBits is the 4th argument */
SHIM(CreatePalette) { RET(0x50100); }
SHIM(CreateSolidBrush) { RET(0x50200); }
SHIM(CreateFontA) { RET(0x50300); }
SHIM(GetSystemPaletteUse) { RET(1); }
SHIM(GetSystemPaletteEntries) { if (A(3)) memset(GP(A(3)), 0, 4 * A(2)); RET(A(2)); }
SHIM(SetGdiZero) { RET(0); }
SHIM(SetGdiOne) { RET(1); }
SHIM(TextOutA) { RET(1); }
SHIM(GetTextExtentPoint32A) { WR32(A(3), A(2) * 8); WR32(A(3) + 4, 16); RET(1); }
SHIM(GetTextMetricsA) { memset(GP(A(1)), 0, 56); WR32(A(1), 16); WR32(A(1) + 20, 8); WR32(A(1) + 24, 8); RET(1); }
SHIM(SetDeviceGammaRamp) { RET(1); }
SHIM(GetDeviceGammaRamp) { for (uint32_t ch = 0; ch < 3; ch++) for (uint32_t i = 0; i < 256; i++) WR16(A(1) + ch * 512 + i * 2, (uint16_t)(i * 257)); RET(1); }

const ShimDef gdi32_shims[] = {
    S(GetDeviceCaps, STD(2)), S(GetObjectA, STD(3)), S(GetStockObject, STD(1)), S(CreateCompatibleDC, STD(1)), S(CreateDIBSection, STD(6)), S(CreatePalette, STD(1)),
    S(CreateSolidBrush, STD(1)), S(CreateFontA, STD(14)), S(GetSystemPaletteUse, STD(1)), S(GetSystemPaletteEntries, STD(4)), S(TextOutA, STD(5)), S(GetTextExtentPoint32A, STD(4)),
    S(GetTextMetricsA, STD(2)), S(SetDeviceGammaRamp, STD(2)), S(GetDeviceGammaRamp, STD(2)),
    SA("DeleteObject", SetGdiOne, STD(1)), SA("GdiFlush", SetGdiOne, 0), SA("DeleteDC", SetGdiOne, STD(1)), SA("BitBlt", SetGdiOne, STD(9)), SA("SelectObject", SetGdiZero, STD(2)),
    SA("SelectPalette", SetGdiZero, STD(3)), SA("RealizePalette", SetGdiZero, STD(1)), SA("SetDIBColorTable", SetGdiZero, STD(4)), SA("StretchBlt", SetGdiOne, STD(11)),
    SA("SetSystemPaletteUse", SetGdiOne, STD(2)), SA("SetBkMode", SetGdiOne, STD(2)), SA("SetBkColor", SetGdiZero, STD(2)), SA("SetTextColor", SetGdiZero, STD(2)),
    SA("PatBlt", SetGdiOne, STD(6)), SA("StretchDIBits", SetGdiZero, STD(13)), SA("SetDIBitsToDevice", SetGdiZero, STD(12)), SA("CreateCompatibleBitmap", SetGdiZero, STD(3)),
    { 0, 0, 0 }
};

/* ---------------------------------------------------------------- ADVAPI32: no registry (every key is missing, so the engine uses its defaults) */
SHIM(RegMissing) { RET(2); }
SHIM(RegOk) { RET(0); }
SHIM(GetUserNameA) { const char *n = "Player"; if (A(1) && RD32(A(1)) > 6) { memcpy(GP(A(0)), n, 7); WR32(A(1), 7); RET(1); return; } if (A(1)) WR32(A(1), 7); set_last_error(122); RET(0); }
const ShimDef advapi32_shims[] = {
    SA("RegOpenKeyExA", RegMissing, STD(5)), SA("RegOpenKeyA", RegMissing, STD(3)), SA("RegQueryValueExA", RegMissing, STD(6)), SA("RegSetValueExA", RegMissing, STD(6)),
    SA("RegCreateKeyExA", RegMissing, STD(9)), SA("RegCreateKeyA", RegMissing, STD(3)), SA("RegDeleteKeyA", RegMissing, STD(2)), SA("RegDeleteValueA", RegMissing, STD(2)),
    SA("RegEnumKeyExA", RegMissing, STD(8)), SA("RegEnumValueA", RegMissing, STD(8)), SA("RegCloseKey", RegOk, STD(1)), S(GetUserNameA, STD(2)),
    { 0, 0, 0 }
};

/* ---------------------------------------------------------------- SHELL32, ole32, VERSION */
SHIM(Zero) { RET(0); }
SHIM(CoInitialize) { RET(0); }
SHIM(CoCreateInstance) { if (A(4)) WR32(A(4), 0); RET(0x80040154u); }          /* REGDB_E_CLASSNOTREG */
SHIM(ShellExecuteA) { RET(2); }
SHIM(SHGetFolderPathA) { const char *p = "C:\\"; memcpy(GP(A(4)), p, 4); RET(0); }
const ShimDef misc_shims[] = {
    SA("DragQueryFileA", Zero, STD(4)), SA("DragFinish", Zero, STD(1)), SA("DragAcceptFiles", Zero, STD(2)), S(ShellExecuteA, STD(6)), S(SHGetFolderPathA, STD(5)),
    S(CoInitialize, STD(1)), SA("CoInitializeEx", CoInitialize, STD(2)), SA("CoUninitialize", Zero, 0), S(CoCreateInstance, STD(5)), SA("CoTaskMemFree", Zero, STD(1)),
    SA("GetFileVersionInfoSizeA", Zero, STD(2)), SA("GetFileVersionInfoA", Zero, STD(4)), SA("VerQueryValueA", Zero, STD(4)),
    { 0, 0, 0 }
};
