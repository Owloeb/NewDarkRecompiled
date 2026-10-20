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
    void    *rc;            /* this guest thread's indirect-call cache (rt_icache, --cache-regs); NULL until first used */
} CPU;

typedef void (*guest_fn)(CPU *);

/* --cache-regs: indirect calls (<prefix>_call, lift.py dispatch_code) remember recent targets in a cache owned by the guest
 * thread (reached through its CPU struct), so threads never share one and need no synchronisation: 1024 sets of 2
 * entries, the most recently used first. Keys are guest addresses, which are unique across recompiled modules, so one
 * cache serves all of them. Hosts create each guest thread's CPU struct zeroed (rc = NULL). */
typedef struct { uint32_t va; guest_fn fn; } rt_icache_ent;
typedef struct { rt_icache_ent e[1024][2]; } rt_icache;
#include <stdlib.h>
static inline rt_icache *rt_icache_of(CPU *c) {
    if (__builtin_expect(c->rc == 0, 0)) c->rc = calloc(1, sizeof(rt_icache));
    return (rt_icache *)c->rc;
}

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
enum { RT_FAULT_UNIMPL = 1, RT_FAULT_DIVIDE = 2, RT_FAULT_TRAP = 3, RT_FAULT_BADJUMP = 4, RT_FAULT_BADRET = 5,
       RT_FAULT_X87 = 6 /* --cache-regs: x87 stack depth after a call is not what lift_x87.py inferred (list the address in lift.py --x87-exempt) */ };

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
#if defined(_WIN64)
/* Clang's __builtin_setjmp loses the caller's locals on 64-bit Windows (checked with a minimal program under Zig 0.16),
 * so use the C runtime's _setjmp with a NULL frame: longjmp then restores every callee-saved register and skips SEH
 * unwinding, which is what the builtin does elsewhere. */
#include <setjmp.h>
#define RT_HJ_WORDS (sizeof(jmp_buf) / sizeof(void *) + 1)
#define RT_HOST_SETJMP(b) _setjmp((_JBTYPE *)(void *)(b), NULL)
#define RT_HOST_LONGJMP(b) longjmp(*(jmp_buf *)(void *)(b), 1)
#else
#define RT_HJ_WORDS 5
#define RT_HOST_SETJMP(b) __builtin_setjmp(b)
#define RT_HOST_LONGJMP(b) __builtin_longjmp(b, 1)
#endif
void **rt_sj_begin(CPU *c, uint32_t buf);     /* record the guest state for setjmp(buf); returns the host jump buffer */
void rt_sj_resume(CPU *c, uint32_t buf);      /* after a longjmp to buf: restore the guest registers, eax = value */
void rt_longjmp(CPU *c);                      /* longjmp(buf = [esp+4], value = [esp+8]); does not return */
#define RT_SETJMP(c) do { uint32_t sjb_ = RD32((c)->esp + 4); void **hj_ = rt_sj_begin((c), sjb_); \
                          if (RT_HOST_SETJMP(hj_) == 0) (c)->eax = 0; else rt_sj_resume((c), sjb_); \
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
#elif defined(RT_PROF)   /* profiling build: the preempt check plus a store of the current function's name, read by a sampling thread */
extern int rt_preempt_req; extern const char *volatile rt_prof_cur;
void rt_preempt(CPU *c);
#define BUDGET() do { rt_prof_cur = __func__; if (__builtin_expect(__atomic_load_n(&rt_preempt_req, __ATOMIC_RELAXED), 0)) rt_preempt(c); } while (0)
#elif defined(RT_PREEMPT)   /* portable host: lets another guest thread run when it has waited a time slice (one load per loop head) */
extern int rt_preempt_req;          /* a hint, read with a relaxed atomic load (a plain load on every CPU) */
void rt_preempt(CPU *c);
#define BUDGET() do { if (__builtin_expect(__atomic_load_n(&rt_preempt_req, __ATOMIC_RELAXED), 0)) rt_preempt(c); } while (0)
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
/* fist/fistp (and the 16-bit forms): round per the control word and store the integer, or the "integer indefinite" when
 * the rounded value does not fit (or v is NaN). Same results as FROUND + a range check, without libm: on ARMv7 (the
 * Vita) floor/ceil/trunc/nearbyint are library calls, and nearbyint saves and restores FPSCR, which stalls the VFP
 * pipeline. The range test is done on v itself, with the bounds each rounding mode maps into [lo, hi]; inside them v
 * fits an int32, so the C conversion (which truncates) is exact, and floor/ceil correct it by one.
 * Nearest-even uses the 1.5 * 2^52 trick, valid for |v| < 2^51 under the host's default rounding mode (the mode nearbyint
 * uses too). It needs double arithmetic without excess precision, so a host that evaluates in x87 extended precision
 * (32-bit x86 without SSE2 math) keeps the libm path. port/tests/f2i checks the two against each other. */
#if defined(__FLT_EVAL_METHOD__) && __FLT_EVAL_METHOD__ == 0 && !defined(RT_F2I_LIBM)
#define RT_F2I_FAST 1
#endif
static inline int32_t rt_f2i_(CPU *c, double v, double lo, double hi, int *ok){
#ifdef RT_F2I_FAST
    switch ((c->cw >> 10) & 3) {
    case 0:                                                     /* nearest, ties to even */
        if (!(v >= lo - 0.5 && v < hi + 0.5)) break;            /* hi + 0.5 rounds (to even) past hi */
        *ok = 1; return (int32_t)((v + 6755399441055744.0) - 6755399441055744.0);
    case 1: {                                                   /* down */
        if (!(v >= lo && v < hi + 1.0)) break;
        int32_t t = (int32_t)v; *ok = 1; return t - ((double)t > v);
    }
    case 2: {                                                   /* up */
        if (!(v > lo - 1.0 && v <= hi)) break;
        int32_t t = (int32_t)v; *ok = 1; return t + ((double)t < v);
    }
    default:                                                    /* toward zero */
        if (!(v > lo - 1.0 && v < hi + 1.0)) break;
        *ok = 1; return (int32_t)v;
    }
    *ok = 0; return 0;
#else
    double r = FROUND(c, v);
    if (!(r >= lo && r <= hi)) { *ok = 0; return 0; }
    *ok = 1; return (int32_t)r;
#endif
}
static inline uint32_t F2I32(CPU *c, double v){
    int ok; int32_t r = rt_f2i_(c, v, -2147483648.0, 2147483647.0, &ok);
    return ok ? (uint32_t)r : 0x80000000u;
}
static inline uint16_t F2I16(CPU *c, double v){
    int ok; int32_t r = rt_f2i_(c, v, -32768.0, 32767.0, &ok);
    return ok ? (uint16_t)(int16_t)r : 0x8000u;
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
