/* darkrecomp runtime: reports for -DRT_ABI_CHECK builds of --cache-regs code (see RT_ABICHK in rt_fast.h).
 *
 * Compile this file into a build whose generated C was compiled with -DRT_ABI_CHECK. Each call site that breaks the
 * calling-convention contract is reported once: the guest address of the call, its target (a guest address, or
 * "import #N"), which callee-saved registers came back changed, and their values before and after. The generated code
 * then restores those registers, so the rest of the program stays consistent. To give a reported site the old,
 * reload-everything treatment instead, list its address in a file passed to lift.py --abi-exempt and relift.
 *
 * Reports go through RT_ABI_LOG (printf-style). Define it to your platform's logger, e.g.
 *     -DRT_ABI_LOG=ss2port_log
 * and it defaults to stderr. rt_abi_report() prints the totals (call it at exit, or from a debug key). */
#include <stdio.h>
#include <stdint.h>
#include "rt.h"

#ifndef RT_ABI_LOG
#define RT_ABI_LOG(...) (fprintf(stderr, __VA_ARGS__), fputc('\n', stderr), fflush(stderr))
#else
void RT_ABI_LOG(const char *fmt, ...);
#endif

#define ABI_SITES 1024   /* distinct sites remembered for de-duplication; more are still counted */

static volatile uint32_t abi_seen[ABI_SITES];
static volatile uint32_t abi_nsites, abi_nviolations;

static int abi_first_report(uint32_t site) {
    uint32_t h = (site * 2654435761u) >> 22;            /* 10 bits */
    for (uint32_t k = 0; k < ABI_SITES; k++) {
        uint32_t *slot = (uint32_t *)&abi_seen[(h + k) & (ABI_SITES - 1)];
        uint32_t cur = __atomic_load_n(slot, __ATOMIC_RELAXED);
        if (cur == site) return 0;
        if (cur == 0) {
            uint32_t expect = 0;
            if (__atomic_compare_exchange_n(slot, &expect, site, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
                __atomic_add_fetch(&abi_nsites, 1, __ATOMIC_RELAXED);
                return 1;
            }
            if (expect == site) return 0;
        }
    }
    return 0;                                            /* table full: count only */
}

void rt_abi_violation(CPU *c, uint32_t site, uint32_t target, uint32_t mask,
                      uint32_t ebx, uint32_t ebp, uint32_t esi, uint32_t edi) {
    __atomic_add_fetch(&abi_nviolations, 1, __ATOMIC_RELAXED);
    if (!abi_first_report(site)) return;
    char tgt[32];
    if ((target & 0xFFFF0000u) == 0xFFFF0000u) snprintf(tgt, sizeof tgt, "import #%u", (unsigned)(target & 0xFFFFu));
    else snprintf(tgt, sizeof tgt, "%08x", (unsigned)target);
    RT_ABI_LOG("ABI VIOLATION: call at %08x to %s changed%s%s%s%s  (before -> after: ebx %08x->%08x ebp %08x->%08x "
               "esi %08x->%08x edi %08x->%08x)",
               (unsigned)site, tgt,
               (mask & 1) ? " ebx" : "", (mask & 2) ? " ebp" : "", (mask & 4) ? " esi" : "", (mask & 8) ? " edi" : "",
               (unsigned)ebx, (unsigned)c->ebx, (unsigned)ebp, (unsigned)c->ebp,
               (unsigned)esi, (unsigned)c->esi, (unsigned)edi, (unsigned)c->edi);
}

void rt_abi_report(void) {
    RT_ABI_LOG("ABI CHECK: %u violation(s) at %u distinct call site(s)",
               (unsigned)abi_nviolations, (unsigned)abi_nsites);
}
