/* darkrecomp runtime, register-caching mode (lift.py --cache-regs).
 *
 * In the default mode generated code keeps every guest register in the CPU struct (c->eax ...). Guest memory is written
 * through a byte pointer, which C lets alias anything, so after every guest store the compiler must reload every register
 * and the memory base M. In this mode each recompiled function keeps the eight general registers and the memory base in
 * locals (r_eax ..., MB_), which never alias guest memory. They go back to the CPU struct wherever something
 * else may look at them: before a call to another guest function, the host (imports, faults, setjmp/longjmp, cpuid,
 * preemption) and before returning; and are read back (RELOAD_) after anything that may have changed them. At calls and
 * returns the lifter stores only the registers this function may have changed since they were last in sync (it tracks
 * that per instruction); rare exits (faults, longjmp, preemption) store all of them (SPILLALL_).
 *
 * The x87 state, flags crossing calls and everything else stay in the CPU struct exactly as in the default mode.
 * Nothing here is used unless lift.py was run with --cache-regs. */
#ifndef DARKRECOMP_RT_FAST_H
#define DARKRECOMP_RT_FAST_H
#include "rt.h"

/* GCC would merge the register loads at function entry into vector loads and then move each lane to a core register,
 * which is slow on ARM (Cortex-A9) and keeps loads alive that are otherwise dead. */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize ("no-tree-slp-vectorize")
#endif

#ifdef RT_IDENTITY
#define RT_MB_INIT ((uint8_t *)0)
#define MGP_(mb, a) ((uint8_t *)(uintptr_t)(uint32_t)(a))
#else
#define RT_MB_INIT M
#define MGP_(mb, a) ((mb) + (uintptr_t)(uint32_t)(a))
#endif

static inline uint8_t  MRD8_ (uint8_t *mb, uint32_t a){ return *MGP_(mb, a); }
static inline uint16_t MRD16_(uint8_t *mb, uint32_t a){ uint16_t v; memcpy(&v, MGP_(mb, a), 2); return v; }
static inline uint32_t MRD32_(uint8_t *mb, uint32_t a){ uint32_t v; memcpy(&v, MGP_(mb, a), 4); return v; }
static inline uint64_t MRD64_(uint8_t *mb, uint32_t a){ uint64_t v; memcpy(&v, MGP_(mb, a), 8); return v; }
static inline void MWR8_ (uint8_t *mb, uint32_t a, uint8_t  v){ *MGP_(mb, a) = v; }
static inline void MWR16_(uint8_t *mb, uint32_t a, uint16_t v){ memcpy(MGP_(mb, a), &v, 2); }
static inline void MWR32_(uint8_t *mb, uint32_t a, uint32_t v){ memcpy(MGP_(mb, a), &v, 4); }
static inline void MWR64_(uint8_t *mb, uint32_t a, uint64_t v){ memcpy(MGP_(mb, a), &v, 8); }
static inline float  MLDF32_(uint8_t *mb, uint32_t a){ float f; uint32_t v = MRD32_(mb, a); memcpy(&f, &v, 4); return f; }
static inline double MLDF64_(uint8_t *mb, uint32_t a){ double d; uint64_t v = MRD64_(mb, a); memcpy(&d, &v, 8); return d; }
static inline void   MSTF32_(uint8_t *mb, uint32_t a, double d){ float f = (float)d; uint32_t v; memcpy(&v, &f, 4); MWR32_(mb, a, v); }
static inline void   MSTF64_(uint8_t *mb, uint32_t a, double d){ uint64_t v; memcpy(&v, &d, 8); MWR64_(mb, a, v); }

#define MRD8(a)      MRD8_(MB_, a)
#define MRD16(a)     MRD16_(MB_, a)
#define MRD32(a)     MRD32_(MB_, a)
#define MRD64(a)     MRD64_(MB_, a)
#define MWR8(a, v)   MWR8_(MB_, a, v)
#define MWR16(a, v)  MWR16_(MB_, a, v)
#define MWR32(a, v)  MWR32_(MB_, a, v)
#define MWR64(a, v)  MWR64_(MB_, a, v)
#define MLDF32(a)    MLDF32_(MB_, a)
#define MLDF64(a)    MLDF64_(MB_, a)
#define MSTF32(a, d) MSTF32_(MB_, a, d)
#define MSTF64(a, d) MSTF64_(MB_, a, d)
#define MLDF80(a)    LDF80(a)            /* rare (fld tbyte, frstor): the default helpers are fine */
#define MSTF80(a, d) STF80(a, d)

/* the cached registers of the function being run */
#define RT_LOCALS uint32_t r_eax = c->eax, r_ecx = c->ecx, r_edx = c->edx, r_ebx = c->ebx, \
                           r_esp = c->esp, r_ebp = c->ebp, r_esi = c->esi, r_edi = c->edi; \
                  uint8_t *const MB_ = RT_MB_INIT; \
                  (void)r_eax; (void)r_ecx; (void)r_edx; (void)r_ebx; (void)r_esp; (void)r_ebp; (void)r_esi; (void)r_edi; (void)MB_
#define SPILLALL_  ((void)(c->eax = r_eax, c->ecx = r_ecx, c->edx = r_edx, c->ebx = r_ebx, \
                        c->esp = r_esp, c->ebp = r_ebp, c->esi = r_esi, c->edi = r_edi))
#define RELOAD_ ((void)(r_eax = c->eax, r_ecx = c->ecx, r_edx = c->edx, r_ebx = c->ebx, \
                        r_esp = c->esp, r_ebp = c->ebp, r_esi = c->esi, r_edi = c->edi))

#define MPUSH32(v) do { uint32_t pv_ = (uint32_t)(v); r_esp -= 4; MWR32(r_esp, pv_); } while (0)
#define MPOP32()   (r_esp += 4, MRD32(r_esp - 4u))

#ifdef RT_SHADOW
#define MCALLPUSH(r) do { MPUSH32(r); if (rt_shadow_sp >= RT_SHADOW_MAX) { SPILLALL_; rt_fault(c, r, RT_FAULT_BADRET); } \
                          rt_shadow[rt_shadow_sp++] = (r); } while (0)
#define MRETCHK() do { if (rt_shadow_sp <= 0 || rt_shadow[--rt_shadow_sp] != MRD32(r_esp)) \
                           { SPILLALL_; rt_fault(c, MRD32(r_esp), RT_FAULT_BADRET); } } while (0)
#else
#define MCALLPUSH(r) MPUSH32(r)
#define MRETCHK() do {} while (0)
#endif

/* loop heads and function entries: like BUDGET(), with the registers handed over when the host takes control */
#if defined(RT_BUDGET)
#define MBUDGET() do { if (--c->budget < 0) { SPILLALL_; rt_budget_exhausted(c); } } while (0)
#elif defined(RT_PREEMPT)
#define MBUDGET() do { if (__builtin_expect(__atomic_load_n(&rt_preempt_req, __ATOMIC_RELAXED), 0)) { SPILLALL_; rt_preempt(c); RELOAD_; } } while (0)
#else
#define MBUDGET() BUDGET()
#endif

#ifdef RT_TRACE
#undef TRACE
#define TRACE(va) do { SPILLALL_; rt_trace(c, va); } while (0)
#endif

#endif
