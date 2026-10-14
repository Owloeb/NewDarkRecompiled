/* port/tests/fastpaths/shim.c: port/core/fastpaths.c as a shared library for check.py (guest memory = a 4 GB reservation). */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <sys/mman.h>
#include "rt.h"
uint8_t *M;
void rt_fault(CPU *c, uint32_t addr, int kind) { (void)c; (void)addr; (void)kind; __builtin_trap(); }
uint8_t *shim_mem(void) {
    if (!M) M = mmap(NULL, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return M;
}
size_t shim_cpu_size(void) { return sizeof(CPU); }
size_t shim_off(int which) {
    switch (which) { case 0: return offsetof(CPU, eax); case 1: return offsetof(CPU, st); case 2: return offsetof(CPU, top); case 3: return offsetof(CPU, cw); }
    return 0;
}
