/* gl_thread.c: plat.h's renderer functions (plat_tex_*, plat_gfx_*) on top of gl_render.c, optionally on a render thread.
 *
 * Without a render thread every call goes straight to the renderer. With one (glt_start, which the windowing code calls
 * before it sets up GL), each call is copied into a ring buffer (vertices, indices, texture data and all) and the
 * caller returns at once; the render thread replays the calls in order on its own core, so the game computes the next
 * frame while the GPU driver works through this one. Nothing the renderer does is visible to the game except:
 *   - plat_tex_create returns the texture handle at once (glr_tex_new, no GL); the GL texture is made on the thread;
 *   - plat_tex_read waits for everything before it and returns the pixels;
 *   - plat_gfx_present lets the game run at most one frame ahead of what has been shown (frame pacing, bounded latency);
 *   - a call too big for the ring (a huge draw or texture) waits and runs with the caller's own data.
 * The ring holds only host data: no guest pointer is kept past the call that passed it.
 *
 * Single producer (the game thread, which holds the guest lock for every plat_* call) and single consumer. */
#include <stdlib.h>
#include <string.h>
#include "gl_render.h"

int glt_on; uint64_t glt_wait_ns;

#define RING (4u << 20)                     /* bytes; a frame of SS2 is typically 0.2-2 MB of vertices */
#define BIG  (RING / 4)                     /* a single call larger than this waits and runs with the caller's data */
#define ALIGN(n) (((n) + 15u) & ~(size_t)15u)

enum { C_WRAP, C_CALL, C_TEX_INIT, C_TEX_UPLOAD, C_TEX_DESTROY, C_BEGIN, C_TARGET, C_VIEWPORT, C_SCISSOR, C_CLEAR, C_DRAW,
       C_COPY, C_PRESENT, C_GAMMA, C_DEFER };
typedef struct { uint32_t type, size; } Hdr;               /* size: the whole command, header included, 16-aligned */
typedef struct { Hdr h; void (*fn)(void *); void *arg; volatile int *done; } CCall;
typedef struct { Hdr h; PlatTexture *t; } CTex;
typedef struct { Hdr h; PlatTexture *t; int level, x, y, w, h_, pitch; } CUpload;           /* + data */
typedef struct { Hdr h; PlatViewport vp; } CViewport;
typedef struct { Hdr h; int enable, x, y, w, h_; } CScissor;
typedef struct { Hdr h; int color, depth, stencil; uint32_t argb, s; float z; } CClear;
typedef struct { Hdr h; int prim, nverts, nidx; PlatDrawState st; } CDraw;               /* + vertices + indices */
typedef struct { Hdr h; PlatTexture *src, *dst; int sx, sy, sw, sh, dx, dy, dw, dh, linear; } CCopy;
typedef struct { Hdr h; uint16_t ramp[3][256]; } CGamma;
typedef struct { Hdr h; void (*fn)(void *, PlatDrawFn); uint32_t n, pad; } CDefer;                   /* + data (16-aligned) */

static uint8_t *ring;
static uint64_t wpos;                       /* producer: end of what it has written */
static uint64_t rpos_seen;                  /* producer: last rpos it read (under the lock) */
static uint64_t pub_w, pub_r;               /* under mx: written and published / consumed and released */
static uint64_t frames_q, frames_done;      /* presents queued / done (frames_done under mx) */
static int consumer_idle;                   /* under mx: the render thread is waiting for work */
static PlatMutex *mx; static PlatCond *cv_work, *cv_prod;
static PlatVideoCaps caps_cache; static int caps_ok;

static uint64_t now_ns(void) { return plat_time_ns(); }

/* ---------------------------------------------------------------- the render thread */
static void exec(Hdr *h) {
    switch (h->type) {
    case C_CALL: { CCall *c = (CCall *)h; c->fn(c->arg); plat_mutex_lock(mx); *c->done = 1; plat_cond_broadcast(cv_prod); plat_mutex_unlock(mx); break; }
    case C_TEX_INIT: glr_tex_init(((CTex *)h)->t); break;
    case C_TEX_DESTROY: glr_tex_destroy(((CTex *)h)->t); break;
    case C_TEX_UPLOAD: { CUpload *u = (CUpload *)h; glr_tex_upload(u->t, u->level, u->x, u->y, u->w, u->h_, u + 1, u->pitch); break; }
    case C_BEGIN: glr_begin_frame(); break;
    case C_TARGET: glr_set_target(((CTex *)h)->t); break;
    case C_VIEWPORT: glr_viewport(&((CViewport *)h)->vp); break;
    case C_SCISSOR: { CScissor *c = (CScissor *)h; glr_scissor(c->enable, c->x, c->y, c->w, c->h_); break; }
    case C_CLEAR: { CClear *c = (CClear *)h; glr_clear(c->color, c->argb, c->depth, c->z, c->stencil, c->s); break; }
    case C_DRAW: { CDraw *d = (CDraw *)h; const PlatVertex *v = (const PlatVertex *)(d + 1);
                   glr_draw(d->prim, v, d->nverts, (const uint16_t *)(v + d->nverts), d->nidx, &d->st); break; }
    case C_COPY: { CCopy *c = (CCopy *)h; glr_copy(c->src, c->sx, c->sy, c->sw, c->sh, c->dst, c->dx, c->dy, c->dw, c->dh, c->linear); break; }
    case C_PRESENT: glr_present(); plat_mutex_lock(mx); frames_done++; plat_cond_broadcast(cv_prod); plat_mutex_unlock(mx); break;
    case C_GAMMA: glr_gamma((const uint16_t (*)[256])((CGamma *)h)->ramp); break;
    case C_DEFER: { CDefer *c = (CDefer *)h; c->fn((uint8_t *)c + ALIGN(sizeof *c), glr_draw); break; }
    }
}
static void render_thread(void *arg) {
    (void)arg; uint64_t r = 0;
    for (;;) {
        plat_mutex_lock(mx);
        pub_r = r; plat_cond_broadcast(cv_prod);                      /* give back the space used so far */
        while (pub_w == r) { consumer_idle = 1; plat_cond_wait(cv_work, mx); }
        consumer_idle = 0; uint64_t end = pub_w;
        plat_mutex_unlock(mx);
        size_t done = 0;
        while (r != end) {
            Hdr *h = (Hdr *)(ring + (r % RING));
            exec(h); r += h->size; done += h->size;
            if (done >= RING / 8) { plat_mutex_lock(mx); pub_r = r; plat_cond_broadcast(cv_prod); plat_mutex_unlock(mx); done = 0; }
        }
    }
}

/* ---------------------------------------------------------------- the producer side */
static void wait_cond(void) { uint64_t t = now_ns(); plat_cond_wait(cv_prod, mx); glt_wait_ns += now_ns() - t; }
/* room for a command of `size` bytes (16-aligned); returns where to write it */
static Hdr *reserve(size_t size) {
    size_t off = (size_t)(wpos % RING), need = size;
    if (off + size > RING) need += RING - off;                        /* the command must not wrap: skip to the start */
    if (RING - (wpos - rpos_seen) < need) {
        plat_mutex_lock(mx);
        while (RING - (wpos - pub_r) < need) wait_cond();
        rpos_seen = pub_r; plat_mutex_unlock(mx);
    }
    if (off + size > RING) { Hdr *w = (Hdr *)(ring + off); w->type = C_WRAP; w->size = (uint32_t)(RING - off); wpos += RING - off; }
    Hdr *h = (Hdr *)(ring + (wpos % RING)); h->size = (uint32_t)size; return h;
}
static void publish(Hdr *h, uint32_t type) {
    h->type = type; wpos += h->size;
    plat_mutex_lock(mx); pub_w = wpos; if (consumer_idle) plat_cond_signal(cv_work); plat_mutex_unlock(mx);
}
void glt_call(void (*fn)(void *), void *arg) {
    if (!glt_on) { fn(arg); return; }
    volatile int done = 0;
    CCall *c = (CCall *)reserve(ALIGN(sizeof *c)); c->fn = fn; c->arg = arg; c->done = &done; publish(&c->h, C_CALL);
    plat_mutex_lock(mx); while (!done) wait_cond(); plat_mutex_unlock(mx);
}
static void tex_cmd(uint32_t type, PlatTexture *t) { CTex *c = (CTex *)reserve(ALIGN(sizeof *c)); c->t = t; publish(&c->h, type); }

static void render_init(void *arg) { (void)arg; }
int glt_start(void) {
    if (glt_on) return 0;
    ring = malloc(RING); mx = plat_mutex_new(); cv_work = plat_cond_new(); cv_prod = plat_cond_new();
    if (!ring || !mx || !cv_work || !cv_prod) return -1;
    if (gl_thread_spawn(render_thread, NULL)) return -1;
    glt_on = 1; glt_call(render_init, NULL);                          /* the thread is up and taking work */
    return 0;
}
void glt_set_caps(const PlatVideoCaps *c) { caps_cache = *c; caps_ok = 1; }
int glt_get_caps(PlatVideoCaps *c) { if (!caps_ok) return 0; *c = caps_cache; return 1; }

/* ---------------------------------------------------------------- plat.h */
PlatTexture *plat_tex_create(int w, int h, int levels, PlatTexFormat fmt, int rt) {
    PlatTexture *t = glr_tex_new(w, h, levels, fmt, rt); if (!t) return NULL;
    if (!glt_on) glr_tex_init(t); else tex_cmd(C_TEX_INIT, t);
    return t;
}
typedef struct { PlatTexture *t; int level, x, y, w, h, pitch; const void *data; } UploadArgs;
static void upload_direct(void *p) { UploadArgs *a = p; glr_tex_upload(a->t, a->level, a->x, a->y, a->w, a->h, a->data, a->pitch); }
void plat_tex_upload(PlatTexture *t, int level, int x, int y, int w, int h, const void *data, int pitch) {
    if (!glt_on) { glr_tex_upload(t, level, x, y, w, h, data, pitch); return; }
    int rgba8; size_t n = glr_tex_upload_bytes(t, level, w, h, &rgba8); if (!n) return;
    if (n > BIG) { UploadArgs a = { t, level, x, y, w, h, pitch, data }; glt_call(upload_direct, &a); return; }
    CUpload *u = (CUpload *)reserve(ALIGN(sizeof *u + n));
    u->t = t; u->level = level; u->x = x; u->y = y; u->w = w; u->h_ = h;
    uint8_t *d = (uint8_t *)(u + 1);
    if (rgba8 && pitch != w * 4) {                                    /* RGBA8 rows with padding: pack them */
        for (int r = 0; r < h; r++) memcpy(d + (size_t)r * w * 4, (const uint8_t *)data + (size_t)r * pitch, (size_t)w * 4);
        u->pitch = w * 4;
    } else { memcpy(d, data, n); u->pitch = pitch; }
    publish(&u->h, C_TEX_UPLOAD);
}
void plat_tex_destroy(PlatTexture *t) { if (!t) return; if (!glt_on) glr_tex_destroy(t); else tex_cmd(C_TEX_DESTROY, t); }
typedef struct { PlatTexture *t; int x, y, w, h, pitch, ret; void *out; } ReadArgs;
static void read_direct(void *p) { ReadArgs *a = p; a->ret = glr_tex_read(a->t, a->x, a->y, a->w, a->h, a->out, a->pitch); }
int plat_tex_read(PlatTexture *t, int x, int y, int w, int h, void *out, int pitch) {
    ReadArgs a = { t, x, y, w, h, pitch, -1, out }; glt_call(read_direct, &a); return a.ret;
}
void plat_gfx_begin_frame(void) { if (!glt_on) glr_begin_frame(); else publish(reserve(ALIGN(sizeof(Hdr))), C_BEGIN); }
void plat_gfx_set_target(PlatTexture *t) { if (!glt_on) glr_set_target(t); else tex_cmd(C_TARGET, t); }
void plat_gfx_viewport(const PlatViewport *vp) {
    if (!glt_on) { glr_viewport(vp); return; }
    CViewport *c = (CViewport *)reserve(ALIGN(sizeof *c)); c->vp = *vp; publish(&c->h, C_VIEWPORT);
}
void plat_gfx_scissor(int enable, int x, int y, int w, int h) {
    if (!glt_on) { glr_scissor(enable, x, y, w, h); return; }
    CScissor *c = (CScissor *)reserve(ALIGN(sizeof *c)); c->enable = enable; c->x = x; c->y = y; c->w = w; c->h_ = h; publish(&c->h, C_SCISSOR);
}
void plat_gfx_clear(int color, uint32_t argb, int depth, float z, int stencil, uint32_t s) {
    if (!glt_on) { glr_clear(color, argb, depth, z, stencil, s); return; }
    CClear *c = (CClear *)reserve(ALIGN(sizeof *c)); c->color = color; c->argb = argb; c->depth = depth; c->z = z; c->stencil = stencil; c->s = s;
    publish(&c->h, C_CLEAR);
}
typedef struct { int prim, nverts, nidx; const PlatVertex *v; const uint16_t *idx; const PlatDrawState *st; } DrawArgs;
static void draw_direct(void *p) { DrawArgs *a = p; glr_draw(a->prim, a->v, a->nverts, a->idx, a->nidx, a->st); }
void plat_gfx_draw(int prim, const PlatVertex *v, int nverts, const uint16_t *idx, int nidx, const PlatDrawState *st) {
    if (!glt_on) { glr_draw(prim, v, nverts, idx, nidx, st); return; }
    if (nverts <= 0 || nidx <= 0) return;
    size_t vb = (size_t)nverts * sizeof *v, ib = (size_t)nidx * 2, n = ALIGN(sizeof(CDraw) + vb + ib);
    if (n > BIG) { DrawArgs a = { prim, nverts, nidx, v, idx, st }; glt_call(draw_direct, &a); return; }
    CDraw *d = (CDraw *)reserve(n); d->prim = prim; d->nverts = nverts; d->nidx = nidx; d->st = *st;
    memcpy(d + 1, v, vb); memcpy((uint8_t *)(d + 1) + vb, idx, ib);
    publish(&d->h, C_DRAW);
}
void plat_gfx_copy(PlatTexture *src, int sx, int sy, int sw, int sh, PlatTexture *dst, int dx, int dy, int dw, int dh, int linear) {
    if (!glt_on) { glr_copy(src, sx, sy, sw, sh, dst, dx, dy, dw, dh, linear); return; }
    CCopy *c = (CCopy *)reserve(ALIGN(sizeof *c));
    c->src = src; c->sx = sx; c->sy = sy; c->sw = sw; c->sh = sh; c->dst = dst; c->dx = dx; c->dy = dy; c->dw = dw; c->dh = dh; c->linear = linear;
    publish(&c->h, C_COPY);
}
void plat_gfx_present(void) {
    if (!glt_on) { glr_present(); return; }
    publish(reserve(ALIGN(sizeof(Hdr))), C_PRESENT); frames_q++;
    plat_mutex_lock(mx); while (frames_done + 1 < frames_q) wait_cond(); plat_mutex_unlock(mx);   /* at most one frame ahead */
}
void plat_gfx_gamma(const uint16_t ramp[3][256]) {
    if (!glt_on) { glr_gamma(ramp); return; }
    CGamma *c = (CGamma *)reserve(ALIGN(sizeof *c)); memcpy(c->ramp, ramp, sizeof c->ramp); publish(&c->h, C_GAMMA);
}
typedef struct { void (*fn)(void *, PlatDrawFn); void *data; } DeferArgs;
static void defer_direct(void *p) { DeferArgs *a = p; a->fn(a->data, glr_draw); }
void plat_gfx_deferred(void (*fn)(void *data, PlatDrawFn draw), const void *data, size_t n) {
    if (!glt_on) { fn((void *)data, glr_draw); return; }
    size_t total = ALIGN(sizeof(CDefer)) + ALIGN(n);
    if (total > BIG) { DeferArgs a = { fn, (void *)data }; glt_call(defer_direct, &a); return; }
    CDefer *c = (CDefer *)reserve(total); c->fn = fn; c->n = (uint32_t)n;
    memcpy((uint8_t *)c + ALIGN(sizeof *c), data, n);
    publish(&c->h, C_DEFER);
}
