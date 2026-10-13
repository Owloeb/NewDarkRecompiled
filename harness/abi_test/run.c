/* harness/abi_test/run.c: Linux test host for the --cache-regs calling-convention contract (see run.sh).
 * Loads abitest.exe's memory image, runs its exported test_good and test_bad through the recompiled code, with qsort and
 * GetTickCount provided here (qsort calls back into guest code like win_host's cb_dispatch does).
 * usage: run <image.bin> <image base> <test_good va> <test_bad va>   (all hex) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "rt.h"
#include "imps.h"
uint8_t *M;
extern guest_fn ab_lookup(uint32_t va);
static CPU cpu;
void rt_fault(CPU *c, uint32_t a, int k) { (void)c; printf("FAULT at %08x kind %d\n", a, k); exit(2); }
void rt_budget_exhausted(CPU *c) { (void)c; puts("BUDGET"); exit(2); }
void **rt_sj_begin(CPU *c, uint32_t b) { (void)c; (void)b; puts("setjmp"); exit(2); }
void rt_sj_resume(CPU *c, uint32_t b) { (void)c; (void)b; }
void rt_longjmp(CPU *c) { (void)c; puts("longjmp"); exit(2); }
void rt_call_external(CPU *c, uint32_t t) { guest_fn f = ab_lookup(t); if (!f) { printf("external %08x\n", t); exit(2); } f(c); }
static uint32_t call_guest(CPU *c, uint32_t fn, int n, const uint32_t *a) {   /* like win_host cb_dispatch: whole CPU saved */
    CPU save = *c;
    for (int i = n - 1; i >= 0; i--) PUSH32(c, a[i]);
    CALLPUSH(c, 0xFEEDFACEu);
    guest_fn f = ab_lookup(fn); if (!f) { printf("callback to %08x\n", fn); exit(2); }
    f(c);
    uint32_t r = c->eax; *c = save; return r;
}
void rt_call_import(CPU *c, int idx) {
    const char *n = imp_names[idx]; uint32_t arg0 = RD32(c->esp + 4);
    if (!strcmp(n, "GetTickCount")) c->eax = 12345;
    else if (!strcmp(n, "qsort")) {   /* cdecl qsort(base, n, size, cmp): insertion sort with the guest comparator */
        uint32_t base = arg0, cnt = RD32(c->esp + 8), sz = RD32(c->esp + 12), cmp = RD32(c->esp + 16);
        if (sz != 4) { puts("qsort size"); exit(2); }
        for (uint32_t i = 1; i < cnt; i++)
            for (uint32_t j = i; j > 0; j--) {
                uint32_t a[2] = { base + 4 * (j - 1), base + 4 * j };
                if ((int32_t)call_guest(c, cmp, 2, a) <= 0) break;
                uint32_t t = RD32(a[0]); WR32(a[0], RD32(a[1])); WR32(a[1], t);
            }
        c->eax = 0;
    } else { printf("import %s not provided\n", n); exit(2); }
    c->ecx = 0xC1C1C1C1u; c->edx = 0xD2D2D2D2u;   /* scratch registers: a real callee leaves garbage */
    c->esp += 4;                                   /* pop the return address (both are cdecl or take no arguments) */
}
#ifdef RT_ABI_CHECK
void rt_abi_report(void);
#endif
int main(int argc, char **argv) {
    M = mmap(NULL, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (argc != 5) { fprintf(stderr, "usage: run <image.bin> <base> <test_good> <test_bad>\n"); return 1; }
    FILE *f = fopen(argv[1], "rb"); if (!f) { perror(argv[1]); return 1; }
    size_t n = fread(M + strtoul(argv[2], 0, 16), 1, 64 << 20, f); fclose(f); (void)n;
    struct { const char *name; uint32_t va; } T[] = { { "test_good", (uint32_t)strtoul(argv[3], 0, 16) }, { "test_bad", (uint32_t)strtoul(argv[4], 0, 16) } };
    for (int t = 0; t < 2; t++) for (int seed = 1; seed <= 3; seed++) {
        memset(&cpu, 0, sizeof cpu); cpu.esp = 0x200000; cpu.cw = 0x27F;
        cpu.ebx = 0xB0B0B0B0u; cpu.esi = 0x51515151u; cpu.edi = 0xD1D1D1D1u; cpu.ebp = 0xBBBBBBBBu;
        uint32_t a[1] = { (uint32_t)seed * 17 + 2u };
        uint32_t r = call_guest(&cpu, T[t].va, 1, a);
        printf("%s(%u) = %d\n", T[t].name, a[0], (int)r);
    }
#ifdef RT_ABI_CHECK
    rt_abi_report();
#endif
    return 0;
}
