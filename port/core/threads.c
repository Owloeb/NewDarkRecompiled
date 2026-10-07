/* threads.c: guest threads, the guest lock, kernel objects, handles and waits.
 *
 * Every guest thread is a host thread with its own CPU state, guest stack and thread environment block (TEB, which
 * fs: points at). Only the holder of the guest lock runs guest code or shims; the lock is a FIFO ticket lock so a thread
 * that yields really lets the next one in. Kernel objects (events, mutexes, semaphores, threads) keep their state under
 * a separate small lock, so host threads that are not guest threads (the audio mixer) can signal events too. */
#include <stdio.h>
#include <stdlib.h>
#include "core.h"

struct GuestThread {
    CPU cpu;
    uint32_t id, teb, region, stack_lo, stack_hi;
    PlatThread *pt;
    struct KThread *obj;
    uint32_t start, arg; int crt_style;     /* _beginthreadex threads return their code; CreateThread the same */
    char name[32];
};
typedef struct KThread { KObj k; GuestThread *t; int done; uint32_t code; } KThread;

GuestThread *g_cur;
volatile int rt_preempt_req;
static PlatMutex *gil_m; static PlatCond *gil_cv;
static uint64_t gil_next, gil_serving;
static uint64_t slice_start; static unsigned slice_tick;
static uint32_t next_tid = 1000;
static uint32_t g_peb;
#define SLICE_NS 2000000ull
#define STACK_SIZE (1u << 20)
#define TEB_SIZE 0x2000u

CPU *cur_cpu(void) { return g_cur ? &g_cur->cpu : NULL; }
uint32_t cur_thread_id(void) { return g_cur ? g_cur->id : 0; }
uint32_t cur_teb(void) { return g_cur ? g_cur->teb : 0; }
void set_last_error(uint32_t e) { if (g_cur) WR32(g_cur->teb + 0x34, e); }
uint32_t get_last_error(void) { return g_cur ? RD32(g_cur->teb + 0x34) : 0; }

/* ---------------------------------------------------------------- the guest lock */
static void gil_init(void) { if (!gil_m) { gil_m = plat_mutex_new(); gil_cv = plat_cond_new(); } }
void gil_acquire(GuestThread *t) {
    plat_mutex_lock(gil_m);
    uint64_t my = gil_next++;
    if (my != gil_serving) { rt_preempt_req = 1; while (my != gil_serving) plat_cond_wait(gil_cv, gil_m); }
    rt_preempt_req = gil_next != gil_serving + 1;          /* someone is still queued behind us */
    plat_mutex_unlock(gil_m);
    g_cur = t; slice_start = plat_time_ns(); slice_tick = 0;
}
void gil_release(void) {
    g_cur = NULL;
    plat_mutex_lock(gil_m); gil_serving++; plat_cond_broadcast(gil_cv); plat_mutex_unlock(gil_m);
}
static void maybe_yield(void) {
    if (++slice_tick & 63) return;
    if (plat_time_ns() - slice_start < SLICE_NS) return;
    GuestThread *t = g_cur; gil_release(); plat_thread_yield(); gil_acquire(t);
}
void rt_preempt(CPU *c) { (void)c; maybe_yield(); }
void thread_preempt_tick(void) { if (rt_preempt_req) maybe_yield(); }

/* ---------------------------------------------------------------- thread environment blocks */
static uint32_t peb(void) {
    if (!g_peb) { extern const uint32_t hd_base; g_peb = vm_alloc(0x1000, 0, "PEB"); WR32(g_peb + 0x08, hd_base); WR32(g_peb + 0x18, 0x100); }
    return g_peb;
}
static GuestThread *thread_alloc(const char *name, uint32_t stack) {
    GuestThread *t = calloc(1, sizeof *t);
    if (stack < 0x10000u) stack = STACK_SIZE;
    stack = (stack + 0xFFFFu) & ~0xFFFFu;
    t->region = vm_alloc(stack + 0x10000u, 0, "thread stack");
    if (!t->region) { free(t); return NULL; }
    t->stack_lo = t->region; t->stack_hi = t->region + stack; t->teb = t->stack_hi;
    t->id = next_tid += 4;
    uint32_t teb = t->teb;
    WR32(teb + 0x00, 0xFFFFFFFFu);                 /* SEH chain: empty */
    WR32(teb + 0x04, t->stack_hi); WR32(teb + 0x08, t->stack_lo);
    WR32(teb + 0x18, teb);                         /* self */
    WR32(teb + 0x20, 0x100); WR32(teb + 0x24, t->id);
    WR32(teb + 0x2c, teb + 0x1000);                /* static TLS pointer array (empty) */
    WR32(teb + 0x30, peb());
    memset(&t->cpu, 0, sizeof t->cpu);
    t->cpu.cw = 0x27f; t->cpu.fs_base = teb; t->cpu.esp = t->stack_hi - 0x40;
    snprintf(t->name, sizeof t->name, "%s", name);
    return t;
}
GuestThread *thread_main_init(void) { gil_init(); GuestThread *t = thread_alloc("main", 4u << 20); gil_acquire(t); return t; }
GuestThread *thread_new_host_side(const char *name) { return thread_alloc(name, 256u << 10); }

/* ---------------------------------------------------------------- kernel objects */
static PlatMutex *ks_m; static PlatCond *ks_cv; static uint64_t ks_gen;
static void ks_init(void) { if (!ks_m) { ks_m = plat_mutex_new(); ks_cv = plat_cond_new(); } }
void kobj_ref(KObj *o) { o->refs++; }
void kobj_unref(KObj *o) { if (o && --o->refs <= 0) { if (o->destroy) o->destroy(o); else free(o); } }
void kobj_changed(void) { ks_init(); plat_mutex_lock(ks_m); ks_gen++; plat_cond_broadcast(ks_cv); plat_mutex_unlock(ks_m); }

typedef struct { KObj k; int manual, state; } KEvent;
static int ev_sig(KObj *o, GuestThread *t) { (void)t; return ((KEvent *)o)->state; }
static void ev_acq(KObj *o, GuestThread *t) { (void)t; if (!((KEvent *)o)->manual) ((KEvent *)o)->state = 0; }
KObj *kevent_new(int manual, int initial) { KEvent *e = calloc(1, sizeof *e); e->k = (KObj){ K_EVENT, 1, NULL, ev_sig, ev_acq }; e->manual = manual; e->state = initial; return &e->k; }
void kevent_set(KObj *o) { ks_init(); plat_mutex_lock(ks_m); ((KEvent *)o)->state = 1; ks_gen++; plat_cond_broadcast(ks_cv); plat_mutex_unlock(ks_m); }
void kevent_reset(KObj *o) { ks_init(); plat_mutex_lock(ks_m); ((KEvent *)o)->state = 0; plat_mutex_unlock(ks_m); }
int kevent_pulse(KObj *o) { kevent_set(o); kevent_reset(o); return 1; }

typedef struct { KObj k; GuestThread *owner; int count; } KMutex;
static int mx_sig(KObj *o, GuestThread *t) { KMutex *m = (KMutex *)o; return !m->owner || m->owner == t; }
static void mx_acq(KObj *o, GuestThread *t) { KMutex *m = (KMutex *)o; m->owner = t; m->count++; }
KObj *kmutex_new(int owned) { KMutex *m = calloc(1, sizeof *m); m->k = (KObj){ K_MUTEX, 1, NULL, mx_sig, mx_acq }; if (owned) { m->owner = g_cur; m->count = 1; } return &m->k; }
int kmutex_release(KObj *o) {
    KMutex *m = (KMutex *)o; int ok = 0; plat_mutex_lock(ks_m);
    if (m->owner == g_cur && m->count > 0) { ok = 1; if (!--m->count) m->owner = NULL; ks_gen++; plat_cond_broadcast(ks_cv); }
    plat_mutex_unlock(ks_m); return ok;
}

typedef struct { KObj k; int32_t count, max; } KSem;
static int sem_sig(KObj *o, GuestThread *t) { (void)t; return ((KSem *)o)->count > 0; }
static void sem_acq(KObj *o, GuestThread *t) { (void)t; ((KSem *)o)->count--; }
KObj *ksem_new(int32_t initial, int32_t max) { KSem *s = calloc(1, sizeof *s); s->k = (KObj){ K_SEMAPHORE, 1, NULL, sem_sig, sem_acq }; s->count = initial; s->max = max; return &s->k; }
int ksem_release(KObj *o, int32_t n, int32_t *prev) {
    KSem *s = (KSem *)o; int ok = 0; plat_mutex_lock(ks_m);
    if (prev) *prev = s->count;
    if (n > 0 && s->count + n <= s->max) { s->count += n; ok = 1; ks_gen++; plat_cond_broadcast(ks_cv); }
    plat_mutex_unlock(ks_m); return ok;
}

static int th_sig(KObj *o, GuestThread *t) { (void)t; return ((KThread *)o)->done; }

/* ---------------------------------------------------------------- handles */
#define HBASE 0x100u
static KObj **htab; static uint32_t hcap;
uint32_t handle_new(KObj *o) {
    for (uint32_t i = 0; i < hcap; i++) if (!htab[i]) { htab[i] = o; return HBASE + 4 * i; }
    uint32_t i = hcap; hcap = hcap ? hcap * 2 : 256; htab = realloc(htab, hcap * sizeof *htab); memset(htab + i, 0, (hcap - i) * sizeof *htab);
    htab[i] = o; return HBASE + 4 * i;
}
static KObj *self_thread_obj(void);
KObj *handle_get(uint32_t h, KType type) {
    KObj *o = NULL;
    if (h == 0xFFFFFFFEu) o = self_thread_obj();
    else if (h >= HBASE && !(h & 3) && (h - HBASE) / 4 < hcap) o = htab[(h - HBASE) / 4];
    return o && (type == K_NONE || o->type == type) ? o : NULL;
}
uint32_t handle_dup(uint32_t h) { KObj *o = handle_get(h, K_NONE); if (!o) return 0; kobj_ref(o); return handle_new(o); }
int handle_close(uint32_t h) {
    if (h < HBASE || (h & 3) || (h - HBASE) / 4 >= hcap || !htab[(h - HBASE) / 4]) return 0;
    KObj *o = htab[(h - HBASE) / 4]; htab[(h - HBASE) / 4] = NULL; kobj_unref(o); return 1;
}

/* ---------------------------------------------------------------- waits */
static int (*msg_check)(void);
void kwait_set_msg_check(int (*fn)(void)) { msg_check = fn; }
uint32_t kwait(KObj **objs, int n, int all, uint32_t timeout_ms, int msgs) {
    ks_init();
    uint64_t deadline = timeout_ms == 0xFFFFFFFFu ? UINT64_MAX : plat_time_ns() + (uint64_t)timeout_ms * 1000000u;
    GuestThread *t = g_cur;
    plat_mutex_lock(ks_m);
    for (;;) {
        if (all) {
            int ok = n > 0; for (int i = 0; i < n; i++) if (!objs[i]->signaled(objs[i], t)) { ok = 0; break; }
            if (ok) { for (int i = 0; i < n; i++) if (objs[i]->acquire) objs[i]->acquire(objs[i], t); plat_mutex_unlock(ks_m); return 0; }
        } else for (int i = 0; i < n; i++) if (objs[i]->signaled(objs[i], t)) { if (objs[i]->acquire) objs[i]->acquire(objs[i], t); plat_mutex_unlock(ks_m); return (uint32_t)i; }
        if (msgs && msg_check) { plat_mutex_unlock(ks_m); int m = msg_check(); plat_mutex_lock(ks_m); if (m) { plat_mutex_unlock(ks_m); return (uint32_t)n; } }
        uint64_t now = plat_time_ns();
        if (now >= deadline) { plat_mutex_unlock(ks_m); return 0x102; }     /* WAIT_TIMEOUT */
        uint64_t gen = ks_gen, slice = deadline - now;
        if (msgs && slice > 5000000u) slice = 5000000u;                    /* messages come from polling: look again soon */
        gil_release();
        while (ks_gen == gen) if (plat_cond_wait_ns(ks_cv, ks_m, slice)) break;
        plat_mutex_unlock(ks_m);
        gil_acquire(t);
        plat_mutex_lock(ks_m);
    }
}

/* ---------------------------------------------------------------- creating and ending threads */
static void th_destroy(KObj *o) { (void)o; /* the GuestThread may still be running; it frees itself */ }
static void thread_entry(void *p) {
    GuestThread *t = p;
    gil_acquire(t);
    CPU *c = &t->cpu;
    uint32_t code = g_call(c, t->start, 1, t->arg);
    extern void thread_exit_cleanup(GuestThread *t);
    t->obj->code = code; t->obj->done = 1; kobj_changed();
    port_debug("thread %u (%s) ended with %u", t->id, t->name, code);
    kobj_unref(&t->obj->k);
    gil_release();
}
static KObj *self_thread_obj(void) { return g_cur && g_cur->obj ? &g_cur->obj->k : NULL; }
/* starts fn(arg) (stdcall, one argument) as a new guest thread; returns its handle */
uint32_t thread_create(uint32_t fn, uint32_t arg, uint32_t stack, int suspended, uint32_t *tid, const char *name) {
    GuestThread *t = thread_alloc(name, stack); if (!t) return 0;
    if (!g_cur->obj) { KThread *m = calloc(1, sizeof *m); m->k = (KObj){ K_THREAD, 1, th_destroy, th_sig, NULL }; m->t = g_cur; g_cur->obj = m; }
    KThread *k = calloc(1, sizeof *k); k->k = (KObj){ K_THREAD, 2, th_destroy, th_sig, NULL }; k->t = t; t->obj = k;   /* one ref for the handle, one for the thread */
    t->start = fn; t->arg = arg;
    if (tid) *tid = t->id;
    if (suspended) port_warn("CreateThread: CREATE_SUSPENDED is not supported; the thread starts at once");
    t->pt = plat_thread_start(thread_entry, t, name);
    if (!t->pt) { port_warn("could not start a host thread"); return 0; }
    port_debug("thread %u (%s) started at %08x", t->id, name, fn);
    return handle_new(&k->k);
}
int thread_exit_code(uint32_t h, uint32_t *code) {
    KObj *o = handle_get(h, K_THREAD); if (!o) return 0;
    KThread *k = (KThread *)o; *code = k->done ? k->code : 259; return 1;      /* STILL_ACTIVE */
}
/* ExitThread / _endthreadex: unwinding the host stack of a recompiled thread is not possible from inside it, so the
 * thread is marked finished and parked; its host thread never runs guest code again. */
void thread_exit_current(uint32_t code) {
    GuestThread *t = g_cur;
    if (!t->obj || t == NULL) port_exit((int)code);
    t->obj->code = code; t->obj->done = 1; kobj_changed();
    port_debug("thread %u exits with %u", t->id, code);
    gil_release();
    for (;;) plat_sleep_ns(1000000000ull);
}
uint32_t thread_id_of(uint32_t h) { KObj *o = handle_get(h, K_THREAD); return o ? ((KThread *)o)->t->id : 0; }
