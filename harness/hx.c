/* differential-test host for generated modules: runs one guest function on a given state. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include <signal.h>
#include <pthread.h>
#include <sys/mman.h>
#include "rt.h"

uint8_t *M;

enum { OUT_RET = 0, OUT_IMPORT = 1, OUT_EXTERNAL = 2, OUT_MEMFAULT = 3, OUT_FAULT = 4, OUT_BUDGET = 5, OUT_NOFUNC = 6 };

typedef struct {
    uint32_t r[8];          /* eax ecx edx ebx esp ebp esi edi */
    uint32_t df, top;
    uint32_t sw, cw;
    double st[8];           /* logical ST(i) */
} Regs;

#ifndef LOOKUP
#define LOOKUP ao_lookup
#endif
extern guest_fn LOOKUP(uint32_t va);

static sigjmp_buf jb;
static volatile int outcome;
static volatile uint32_t outarg;
static CPU cpu;
uint32_t rt_shadow[RT_SHADOW_MAX]; int rt_shadow_sp;
static uint32_t imp_base = 0x7FFE0000u, imp_n = 0, fs_base_g = 0;
void hx_set_fs(uint32_t b) { fs_base_g = b; }
#ifdef RT_TRACE
typedef struct { uint32_t va, r[8], top; double st[8]; } TraceEnt;
#define TRACE_MAX 200000
TraceEnt hx_trace[TRACE_MAX]; int hx_trace_n;
void rt_trace(CPU *c, uint32_t va) {
    if (hx_trace_n >= TRACE_MAX) return;
    TraceEnt *t = &hx_trace[hx_trace_n++];
    t->va = va; t->r[0] = c->eax; t->r[1] = c->ecx; t->r[2] = c->edx; t->r[3] = c->ebx;
    t->r[4] = c->esp; t->r[5] = c->ebp; t->r[6] = c->esi; t->r[7] = c->edi; t->top = c->top;
    for (int i = 0; i < 8; i++) t->st[i] = c->st[(c->top + i) & 7];
}
#endif

void rt_call_import(CPU *c, int idx) { (void)c; outcome = OUT_IMPORT; outarg = idx; siglongjmp(jb, 1); }
void rt_call_external(CPU *c, uint32_t t) {
    (void)c;
    if (t >= imp_base && t < imp_base + 16 * imp_n && (t - imp_base) % 16 == 0) { outcome = OUT_IMPORT; outarg = (t - imp_base) / 16; }
    else { outcome = OUT_EXTERNAL; outarg = t; }
    siglongjmp(jb, 1);
}
void rt_fault(CPU *c, uint32_t a, int kind) { (void)c; (void)a; outcome = OUT_FAULT; outarg = kind; siglongjmp(jb, 1); }
void rt_budget_exhausted(CPU *c) { (void)c; outcome = OUT_BUDGET; siglongjmp(jb, 1); }
/* setjmp/longjmp are exercised in the full host only: here a longjmp ends the trial like a fault */
static void *sj_dummy[5];
void **rt_sj_begin(CPU *c, uint32_t buf) { (void)c; (void)buf; return sj_dummy; }
void rt_sj_resume(CPU *c, uint32_t buf) { (void)c; (void)buf; }
void rt_longjmp(CPU *c) { (void)c; outcome = OUT_FAULT; outarg = 3; siglongjmp(jb, 1); }

static void on_sig(int s, siginfo_t *si, void *u) {
    (void)u;
    uint8_t *a = (uint8_t *)si->si_addr;
    if (s == SIGSEGV || s == SIGBUS) {
        if (a >= M && a < M + 0x100000000ull) { outcome = OUT_MEMFAULT; outarg = (uint32_t)(a - M); siglongjmp(jb, 1); }
    }
    outcome = OUT_FAULT; outarg = 100 + s;   /* host crash: report rather than die */
    siglongjmp(jb, 1);
}

int hx_init(uint32_t import_base, uint32_t n_imports) {
    M = mmap(0, 0x100000000ull, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (M == MAP_FAILED) return -1;
    imp_base = import_base; imp_n = n_imports;
    static uint8_t altstack[1 << 16];
    stack_t ss = { .ss_sp = altstack, .ss_size = sizeof altstack };
    sigaltstack(&ss, 0);
    struct sigaction sa = {0};
    sa.sa_sigaction = on_sig; sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigaction(SIGSEGV, &sa, 0); sigaction(SIGBUS, &sa, 0); sigaction(SIGFPE, &sa, 0); sigaction(SIGILL, &sa, 0);
    return 0;
}
uint8_t *hx_mem(void) { return M; }
int hx_protect(uint32_t a, uint32_t n, int prot) {
    return mprotect(M + a, n, prot);
}

struct job { uint32_t entry; Regs *in, *out; int64_t budget; uint32_t *arg; int res; };

static void *runner(void *p) {
    struct job *j = p;
    /* each thread needs its own alt stack */
    static __thread uint8_t alt[1 << 16];
    stack_t ss = { .ss_sp = alt, .ss_size = sizeof alt };
    sigaltstack(&ss, 0);
    guest_fn f = LOOKUP(j->entry);
    if (!f) { j->res = OUT_NOFUNC; return 0; }
    CPU *c = &cpu;
    memset(c, 0, sizeof *c);
    c->eax = j->in->r[0]; c->ecx = j->in->r[1]; c->edx = j->in->r[2]; c->ebx = j->in->r[3];
    c->esp = j->in->r[4]; c->ebp = j->in->r[5]; c->esi = j->in->r[6]; c->edi = j->in->r[7];
    c->df = j->in->df; c->top = j->in->top; c->sw = (uint16_t)j->in->sw; c->cw = (uint16_t)j->in->cw;
    for (int i = 0; i < 8; i++) c->st[(c->top + i) & 7] = j->in->st[i];
    c->budget = j->budget;
    c->fs_base = fs_base_g;
    rt_shadow_sp = 0; rt_shadow[rt_shadow_sp++] = RD32(c->esp);
    outcome = OUT_RET; outarg = 0;
#ifdef RT_TRACE
    hx_trace_n = 0;
#endif
    if (sigsetjmp(jb, 1) == 0) f(c);
    Regs *o = j->out;
    o->r[0] = c->eax; o->r[1] = c->ecx; o->r[2] = c->edx; o->r[3] = c->ebx;
    o->r[4] = c->esp; o->r[5] = c->ebp; o->r[6] = c->esi; o->r[7] = c->edi;
    o->df = c->df; o->top = c->top; o->sw = FSW(c); o->cw = c->cw;
    for (int i = 0; i < 8; i++) o->st[i] = c->st[(c->top + i) & 7];
    *j->arg = outarg;
    j->res = outcome;
    return 0;
}

int hx_run(uint32_t entry, Regs *in, Regs *out, uint32_t *arg, int64_t budget) {
    struct job j = { entry, in, out, budget, arg, -1 };
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 512u << 20);
    pthread_t th; pthread_create(&th, &at, runner, &j); pthread_join(th, 0);
    return j.res;
}
