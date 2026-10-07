/* darkrecomp runtime: guest CPU state + helpers used by generated code.
 * Guest is 32-bit x86, little-endian. Host must be little-endian.
 * Guest memory: guest address a lives at host (M + a). */
#ifndef DARKRECOMP_RT_H
#define DARKRECOMP_RT_H
#include <stdint.h>
#include <string.h>
#include <math.h>

typedef struct CPU {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t df;            /* direction flag (0/1) */
    double   st[8];         /* x87 stack (53-bit precision, matches MSVC's PC=double) */
    uint32_t top;           /* x87 TOP */
    uint16_t sw, cw;        /* x87 status / control words */
    uint8_t  cf, zf, sf, of, pf; /* arithmetic flags crossing call/return boundaries */
    uint32_t fs_base;       /* guest address of the TIB (fs:[0] = SEH chain) */
    int64_t  budget;        /* harness: block budget; <0 => abort */
} CPU;

typedef void (*guest_fn)(CPU *);

extern uint8_t *M;          /* guest memory base */

/* ---- hooks supplied by the host / harness ---- */
void rt_call_import(CPU *c, int idx);
/* CPUID: a baseline CPU with no SSE/SSE2, so the CRT and engine take their x87/MMX-free fallbacks */
static inline void rt_cpuid(CPU *c){
    uint32_t leaf = c->eax;
    if (leaf == 0) { c->eax = 1; c->ebx = 0x756e6547; c->edx = 0x49656e69; c->ecx = 0x6c65746e; }  /* GenuineIntel */
    else if (leaf == 1) { c->eax = 0x00000633; c->ebx = 0; c->ecx = 0; c->edx = 0x00008001 | 0x10 | 0x100; } /* FPU, TSC, CX8; no MMX/SSE/SSE2 */
    else { c->eax = c->ebx = c->ecx = c->edx = 0; }
}           /* stdcall/cdecl import thunk */
void rt_call_external(CPU *c, uint32_t target); /* indirect call target not in this module */
void rt_fault(CPU *c, uint32_t addr, int kind); /* never returns */
void rt_budget_exhausted(CPU *c);               /* never returns */
enum { RT_FAULT_UNIMPL = 1, RT_FAULT_DIVIDE = 2, RT_FAULT_TRAP = 3, RT_FAULT_BADJUMP = 4, RT_FAULT_BADRET = 5 };

/* ---- memory ---- */
#ifdef RT_IDENTITY   /* guest address == host address (32-bit Windows host) */
#define GP(a) ((uint8_t *)(uintptr_t)(uint32_t)(a))
#else
#define GP(a) (M + (uintptr_t)(uint32_t)(a))
#endif
static inline uint8_t  RD8 (uint32_t a){ return *GP(a); }
static inline uint16_t RD16(uint32_t a){ uint16_t v; memcpy(&v, GP(a), 2); return v; }
static inline uint32_t RD32(uint32_t a){ uint32_t v; memcpy(&v, GP(a), 4); return v; }
static inline uint64_t RD64(uint32_t a){ uint64_t v; memcpy(&v, GP(a), 8); return v; }
static inline void WR8 (uint32_t a, uint8_t  v){ *GP(a) = v; }
static inline void WR16(uint32_t a, uint16_t v){ memcpy(GP(a), &v, 2); }
static inline void WR32(uint32_t a, uint32_t v){ memcpy(GP(a), &v, 4); }
static inline void WR64(uint32_t a, uint64_t v){ memcpy(GP(a), &v, 8); }

static inline void PUSH32(CPU *c, uint32_t v){ c->esp -= 4; WR32(c->esp, v); }
static inline uint32_t POP32(CPU *c){ uint32_t v = RD32(c->esp); c->esp += 4; return v; }

static inline uint8_t PAR(uint32_t r){ return (uint8_t)!__builtin_parity(r & 0xFF); }

/* call/return. Generated code ignores the popped return address (the host call stack is the
 * real one). RT_SHADOW builds verify it against a shadow stack so a corrupted or forged return
 * is caught instead of silently diverging. */
#ifdef RT_SHADOW
extern uint32_t rt_shadow[]; extern int rt_shadow_sp;
#define RT_SHADOW_MAX (1 << 20)
#define CALLPUSH(c, r) do { PUSH32(c, r); if (rt_shadow_sp >= RT_SHADOW_MAX) rt_fault(c, r, RT_FAULT_BADRET); \
                            rt_shadow[rt_shadow_sp++] = (r); } while (0)
#define RETCHK(c) do { if (rt_shadow_sp <= 0 || rt_shadow[--rt_shadow_sp] != RD32((c)->esp)) \
                           rt_fault(c, RD32((c)->esp), RT_FAULT_BADRET); } while (0)
#else
#define CALLPUSH(c, r) PUSH32(c, r)
#define RETCHK(c) do {} while (0)
#endif

/* setjmp/longjmp. A guest setjmp must snapshot the state of its caller, so the lifter translates a call to setjmp
 * (an import, or the CRT's own _setjmp3 in a statically linked module) into RT_SETJMP, which takes a host
 * __builtin_setjmp in the recompiled caller's own C frame. A call to longjmp becomes rt_longjmp, which restores the
 * guest registers saved for that jump buffer and __builtin_longjmp's back there; RT_SETJMP then returns the value as if
 * from setjmp. (No C++ destructors run in between, unlike MSVC's longjmp; leaks at worst.) */
void **rt_sj_begin(CPU *c, uint32_t buf);     /* record the guest state for setjmp(buf); returns the host jump buffer */
void rt_sj_resume(CPU *c, uint32_t buf);      /* after a longjmp to buf: restore the guest registers, eax = value */
void rt_longjmp(CPU *c);                      /* longjmp(buf = [esp+4], value = [esp+8]); does not return */
#define RT_SETJMP(c) do { uint32_t sjb_ = RD32((c)->esp + 4); void **hj_ = rt_sj_begin((c), sjb_); \
                          if (__builtin_setjmp(hj_) == 0) (c)->eax = 0; else rt_sj_resume((c), sjb_); \
                          RETCHK(c); (c)->esp += 4; } while (0)

#ifdef RT_TRACE
void rt_trace(CPU *c, uint32_t va);
#define TRACE(va) rt_trace(c, va)
#else
#define TRACE(va) do {} while (0)
#endif

#ifdef RT_BUDGET
#define BUDGET() do { if (--c->budget < 0) rt_budget_exhausted(c); } while (0)
#elif defined(RT_RING)   /* diagnostic: remember the last guest functions / loop heads entered */
extern const char *rt_ring[4096]; extern volatile unsigned rt_ring_i;
extern unsigned char rt_cov[1u << 22]; extern const char *rt_seq[32768]; extern volatile unsigned rt_nseq;
#define BUDGET() do { const char *f_ = __func__; rt_ring[rt_ring_i++ & 4095] = f_; unsigned h_ = (unsigned)(uintptr_t)f_ & 0x3FFFFFu; if (!rt_cov[h_]) { rt_cov[h_] = 1; rt_seq[rt_nseq++ & 32767] = f_; } } while (0)
#elif defined(RT_PREEMPT)   /* portable host: lets another guest thread run when it has waited a time slice (one load per loop head) */
extern volatile int rt_preempt_req;
void rt_preempt(CPU *c);
#define BUDGET() do { if (__builtin_expect(rt_preempt_req, 0)) rt_preempt(c); } while (0)
#else
#define BUDGET() do {} while (0)
#endif

/* ---- x87 ---- */
#define ST(i) (c->st[(c->top + (i)) & 7])
static inline void FPUSH(CPU *c, double v){ c->top = (c->top - 1) & 7; c->st[c->top] = v; }
static inline double FPOP(CPU *c){ double v = c->st[c->top]; c->top = (c->top + 1) & 7; return v; }
static inline uint16_t FSW(CPU *c){ return (uint16_t)((c->sw & ~0x3800) | ((c->top & 7) << 11)); }

static inline float  LDF32(uint32_t a){ float f;  uint32_t v = RD32(a); memcpy(&f, &v, 4); return f; }
static inline double LDF64(uint32_t a){ double d; uint64_t v = RD64(a); memcpy(&d, &v, 8); return d; }
static inline void   STF32(uint32_t a, double d){ float f = (float)d; uint32_t v; memcpy(&v, &f, 4); WR32(a, v); }
static inline void   STF64(uint32_t a, double d){ uint64_t v; memcpy(&v, &d, 8); WR64(a, v); }

/* 80-bit extended <-> double */
static inline double LDF80(uint32_t a){
    uint64_t mant = RD64(a); uint16_t se = RD16(a + 8);
    int sign = se >> 15, exp = se & 0x7FFF;
    double r;
    if (exp == 0 && mant == 0) r = 0.0;
    else if (exp == 0x7FFF) r = (mant << 1) ? NAN : INFINITY;
    else r = ldexp((double)mant, exp - 16383 - 63);
    return sign ? -r : r;
}
static inline void STF80(uint32_t a, double d){
    uint64_t mant = 0; uint16_t se = signbit(d) ? 0x8000 : 0;
    if (isnan(d)) { se |= 0x7FFF; mant = 0xC000000000000000ull; }
    else if (isinf(d)) { se |= 0x7FFF; mant = 0x8000000000000000ull; }
    else if (d != 0.0) {
        int e; double m = frexp(fabs(d), &e);           /* m in [0.5,1) */
        mant = (uint64_t)ldexp(m, 64);
        se |= (uint16_t)(e - 1 + 16383);
    }
    WR64(a, mant); WR16(a + 8, se);
}

/* x87 compare: C0=0x100 C2=0x400 C3=0x4000 */
static inline void FCOM(CPU *c, double a, double b){
    uint16_t f;
    if (isnan(a) || isnan(b)) f = 0x4500;
    else if (a > b) f = 0;
    else if (a < b) f = 0x100;
    else f = 0x4000;
    c->sw = (uint16_t)((c->sw & ~0x4700) | f);
}
static inline void FXAM(CPU *c){
    double v = ST(0); uint16_t f;
    if (isnan(v)) f = 0x100; else if (isinf(v)) f = 0x500;
    else if (v == 0) f = 0x4000; else f = 0x400;
    if (signbit(v)) f |= 0x200;
    c->sw = (uint16_t)((c->sw & ~0x4700) | f);
}

/* rounding per control word RC bits */
static inline double FROUND(CPU *c, double v){
    switch ((c->cw >> 10) & 3) {
    case 0: return nearbyint(v);
    case 1: return floor(v);
    case 2: return ceil(v);
    default: return trunc(v);
    }
}
static inline uint32_t F2I32(CPU *c, double v){
    double r = FROUND(c, v);
    if (!(r >= -2147483648.0 && r <= 2147483647.0)) return 0x80000000u;
    return (uint32_t)(int32_t)r;
}
static inline uint16_t F2I16(CPU *c, double v){
    double r = FROUND(c, v);
    if (!(r >= -32768.0 && r <= 32767.0)) return 0x8000u;
    return (uint16_t)(int16_t)r;
}
static inline uint64_t F2I64(CPU *c, double v){
    double r = FROUND(c, v);
    if (!(r >= -9223372036854775808.0 && r < 9223372036854775808.0)) return 0x8000000000000000ull;
    return (uint64_t)(int64_t)r;
}

/* eflags pack/unpack (for pushfd/popfd/lahf/sahf) */
#define EFL_PACK(CF,PF,ZF,SF,OF,DF) \
    ((uint32_t)(CF) | 0x2u | ((uint32_t)(PF) << 2) | ((uint32_t)(ZF) << 6) | ((uint32_t)(SF) << 7) | \
     0x200u | ((uint32_t)(DF) << 10) | ((uint32_t)(OF) << 11))

#endif
