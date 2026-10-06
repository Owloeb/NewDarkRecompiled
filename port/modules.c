/* modules.c: loader for the recompiled DLLs (allobjs.osm, Squirrel.osm, lgvid.dll, fmsel.dll). The engine loads them with
 * LoadLibrary; here the file is read from the game folder, mapped into guest memory at the address it was recompiled for
 * (relocated if the file's own base differs), checked against the CRC of the code that was lifted, its imports are bound to
 * host functions, and its recompiled entry points are found through the module's lookup table. A module whose bytes differ
 * (a mod's copy, another game version) is refused, like on the Windows host. */
#define _GNU_SOURCE
#include <strings.h>
#include "port.h"
#include "recomp_mod.h"

typedef struct { int refs; } ModSt;
static ModSt st[16];
static int nmods(void) { int n = 0; while (recomp_mods[n]) n++; return n; }
static int find_by_name(const char *n) {
    const char *b = strrchr(n, '\\'), *b2 = strrchr(n, '/'); if (b2 > b) b = b2; b = b ? b + 1 : n;
    for (int i = 0; recomp_mods[i]; i++) if (!strcasecmp(b, recomp_mods[i]->name)) return i;
    return -1;
}
static int find_by_handle(uint32_t h) { for (int i = 0; recomp_mods[i]; i++) if (st[i].refs && recomp_mods[i]->base == h) return i; return -1; }
uint32_t mod_handle_of(const char *n) { int k = find_by_name(n); return k >= 0 && st[k].refs ? recomp_mods[k]->base : 0; }

static uint32_t code_crc(const RecompModDesc *d) {
    static uint32_t tab[256]; uint32_t crc = 0;
    if (!tab[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; tab[i] = c; }
    for (unsigned r = 0; r < d->ncode; r++) {
        const uint8_t *q = GP(d->code_lo[r]); size_t len = d->code_hi[r] - d->code_lo[r];
        uint32_t c = ~crc; while (len--) c = tab[(c ^ *q++) & 0xFF] ^ (c >> 8); crc = ~c;
    }
    return crc;
}
static uint32_t import_thunk(const char *dll, const char *fn) {
    uint32_t a = port_proc(fn); if (a) return a;
    char *nm = malloc(strlen(dll) + strlen(fn) + 2); sprintf(nm, "%s!%s", dll, fn);
    return g_thunk(NULL, 0, nm);       /* dies with this name if the module ever calls it */
}
uint32_t mod_load(CPU *c, const char *name) {
    int k = find_by_name(name); if (k < 0) return 0;
    const RecompModDesc *d = recomp_mods[k];
    if (!strcmp(d->name, "fmsel.dll")) { port_log("%s (the fan-mission selector, a GDI/shell user interface) is not loaded by the portable host yet", name); return 0; }
    if (st[k].refs) { st[k].refs++; return d->base; }
    char hp[1400]; host_path(name, hp, sizeof hp); FILE *f = fopen(hp, "rb");
    if (!f) { port_log("%s: could not be opened (%s)", name, hp); return 0; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); uint8_t *file = malloc((size_t)n);
    if (fread(file, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(file); return 0; } fclose(f);
    uint32_t lf = *(uint32_t *)(file + 0x3c); const uint8_t *nt = file + lf;
    uint32_t image_base = *(uint32_t *)(nt + 24 + 28), size_of_image = *(uint32_t *)(nt + 24 + 56), hdrs = *(uint32_t *)(nt + 24 + 60);
    unsigned nsec = *(uint16_t *)(nt + 6), optsz = *(uint16_t *)(nt + 20); const uint8_t *sec = nt + 24 + optsz;
    if (size_of_image != d->size || image_base != d->file_base) { port_log("%s is a different version than the one that was recompiled (a mod's copy?)", name); free(file); return 0; }
    memset(GP(d->base), 0, d->size); memcpy(GP(d->base), file, hdrs);
    for (unsigned i = 0; i < nsec; i++) {
        const uint8_t *s = sec + 40 * i; uint32_t vsz = *(uint32_t *)(s + 8), va = *(uint32_t *)(s + 12), rsz = *(uint32_t *)(s + 16), raw = *(uint32_t *)(s + 20);
        uint32_t cp = rsz < vsz || !vsz ? rsz : vsz; if (raw + cp > (uint32_t)n) cp = (uint32_t)n - raw; if (va + cp > d->size) continue;
        memcpy(GP(d->base + va), file + raw, cp);
    }
    free(file);
    uint32_t delta = d->base - d->file_base; const uint8_t *dir = nt + 24 + 96;       /* data directories */
    uint32_t reloc_rva = *(uint32_t *)(dir + 8 * 5), reloc_sz = *(uint32_t *)(dir + 8 * 5 + 4);
    if (delta && reloc_rva) {
        for (uint32_t p = d->base + reloc_rva, e = p + reloc_sz; p < e;) {
            uint32_t page = RD32(p), blk = RD32(p + 4); if (!blk) break;
            for (uint32_t q = p + 8; q < p + blk; q += 2) { uint16_t en = RD16(q); if ((en >> 12) == 3) WR32(d->base + page + (en & 0xFFF), RD32(d->base + page + (en & 0xFFF)) + delta); }
            p += blk;
        }
    }
    if (code_crc(d) != d->crc) { port_log("%s is a different version than the one that was recompiled (code checksum differs; a mod's copy?)", name); memset(GP(d->base), 0, d->size); return 0; }
    uint32_t irva = *(uint32_t *)(dir + 8), nb = 0;
    if (irva) for (uint32_t p = d->base + irva; RD32(p + 12); p += 20) {
        const char *dll = (const char *)GP(d->base + RD32(p + 12)); uint32_t ot = RD32(p) ? RD32(p) : RD32(p + 16), ft = RD32(p + 16);
        for (uint32_t i = 0; RD32(d->base + ot + 4 * i); i++, nb++) {
            uint32_t e = RD32(d->base + ot + 4 * i), a;
            if (e & 0x80000000u) { char on[32]; snprintf(on, sizeof on, "#%u", e & 0xFFFF); a = import_thunk(dll, on); } else a = import_thunk(dll, (const char *)GP(d->base + e + 2));
            WR32(d->base + ft + 4 * i, a);
        }
    }
    st[k].refs = 1;
    uint32_t ok = g_call(c, d->entry, 3, d->base, 1u, 0u);
    port_log("%s: loaded the recompiled module at %08x (%u imports bound), DllMain -> %u", d->name, d->base, nb, ok);
    if (!ok) { st[k].refs = 0; memset(GP(d->base), 0, d->size); return 0; }
    return d->base;
}
int mod_free(CPU *c, uint32_t h) {
    int k = find_by_handle(h); if (k < 0) return 0;
    if (--st[k].refs > 0) return 1;
    g_call(c, recomp_mods[k]->entry, 3, h, 0u, 0u); memset(GP(recomp_mods[k]->base), 0, recomp_mods[k]->size); return 1;
}
uint32_t mod_export(uint32_t h, const char *name, int *is_mod) {
    int k = find_by_handle(h); *is_mod = k >= 0; if (k < 0) return 0;
    uint32_t b = recomp_mods[k]->base, nt = b + RD32(b + 0x3c), erva = RD32(nt + 24 + 96); if (!erva) return 0;
    uint32_t ex = b + erva, base = RD32(ex + 16), nfn = RD32(ex + 20), nnm = RD32(ex + 24), fn = b + RD32(ex + 28), nm = b + RD32(ex + 32), ord = b + RD32(ex + 36);
    if (!name) { return 0; }
    for (uint32_t i = 0; i < nnm; i++) if (!strcmp((const char *)GP(b + RD32(nm + 4 * i)), name)) { uint32_t o = RD16(ord + 2 * i); return o < nfn ? b + RD32(fn + 4 * o) : 0; }
    (void)base; return 0;
}
int mod_call(CPU *c, uint32_t t) {
    for (int i = 0; recomp_mods[i]; i++) {
        const RecompModDesc *d = recomp_mods[i]; if (!st[i].refs || t < d->base || t >= d->base + d->size) continue;
        guest_fn f = d->lookup(t); if (f) { f(c); return 1; }
    }
    (void)nmods; return 0;
}
