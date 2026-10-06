/* mmio.c: the WINMM multimedia file I/O (RIFF chunk reading) the game uses to load .wav files. */
#include "port.h"
#define MAXM 64
static FILE *mt[MAXM];
SHIM(mmioOpenA) {
    char hp[1400]; host_path(gs(A(0)), hp, sizeof hp); uint32_t fl = A(2); const char *m = (fl & 3) == 0 ? "rb" : "r+b";
    if (fl & 0x1000) m = "wb";   /* MMIO_CREATE */
    FILE *f = fopen(hp, m); if (!f) { if (A(1)) WR32(A(1) + 12, 257); RET(0); return; }
    for (int i = 1; i < MAXM; i++) if (!mt[i]) { mt[i] = f; RET(i); return; }
    fclose(f); RET(0);
}
static FILE *H(uint32_t h) { return h && h < MAXM ? mt[h] : NULL; }
SHIM(mmioClose) { FILE *f = H(A(0)); if (f) { fclose(f); mt[A(0)] = NULL; } RET(0); }
SHIM(mmioRead) { FILE *f = H(A(0)); RET(f ? (uint32_t)fread(GP(A(1)), 1, A(2), f) : (uint32_t)-1); }
SHIM(mmioSeek) { FILE *f = H(A(0)); if (!f || fseek(f, (long)(int32_t)A(1), (int)A(2))) { RET((uint32_t)-1); return; } RET((uint32_t)ftell(f)); }
SHIM(mmioGetInfo) { memset(GP(A(1)), 0, 72); RET(0); }
SHIM(mmioDescend) {
    FILE *f = H(A(0)); uint32_t ck = A(1), par = A(2), fl = A(3); if (!f) { RET(257); return; }
    long limit = par ? (long)(RD32(par + 12) + RD32(par + 4)) : 0x7FFFFFFF;
    for (;;) {
        long pos = ftell(f); uint32_t h[2];
        if (pos + 8 > limit || fread(h, 4, 2, f) != 2) { RET(265); return; }
        int list = h[0] == 0x46464952u || h[0] == 0x5453494Cu; uint32_t form = 0;
        if (list && fread(&form, 4, 1, f) != 1) { RET(265); return; }
        int want = fl & 0x70, ok = 1;
        if (want == 0x10) ok = h[0] == RD32(ck);
        else if (want == 0x20) ok = h[0] == 0x46464952u && form == RD32(ck + 8);
        else if (want == 0x40) ok = h[0] == 0x5453494Cu && form == RD32(ck + 8);
        if (ok) { WR32(ck, h[0]); WR32(ck + 4, h[1]); WR32(ck + 8, list ? form : 0); WR32(ck + 12, (uint32_t)pos + 8); WR32(ck + 16, 0); RET(0); return; }
        fseek(f, pos + 8 + (long)h[1] + (long)(h[1] & 1), SEEK_SET);
    }
}
SHIM(mmioAscend) { FILE *f = H(A(0)); uint32_t ck = A(1); if (!f) { RET(257); return; } long end = (long)(RD32(ck + 12) + RD32(ck + 4)); fseek(f, end + (end & 1), SEEK_SET); RET(0); }
const ShimDef mmio_shims[] = {
    { "mmioOpenA", sh_mmioOpenA, STD(3) }, { "mmioClose", sh_mmioClose, STD(2) }, { "mmioRead", sh_mmioRead, STD(3) }, { "mmioSeek", sh_mmioSeek, STD(3) },
    { "mmioGetInfo", sh_mmioGetInfo, STD(3) }, { "mmioDescend", sh_mmioDescend, STD(4) }, { "mmioAscend", sh_mmioAscend, STD(3) }, { 0, 0, 0 }
};
