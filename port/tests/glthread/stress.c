/* port/tests/glthread/stress.c: stress test of gl_thread.c (the render thread's command ring) against a fake renderer.
 *
 *   sh port/tests/glthread/run.sh        (builds with ThreadSanitizer and runs)
 *
 * A seeded random stream of plat_tex_* / plat_gfx_* calls (draws of 1..20000 vertices, texture uploads up to 2 MB, padded
 * rows, read-backs, presents, gamma, copies) goes through gl_thread.c twice: once without a render thread (each call goes
 * straight to the fake renderer) and once with one. The fake renderer hashes every call it receives, arguments and data,
 * in order, and checks it runs on the right thread. Both runs must produce the same hash; read-backs must return what the
 * fake renderer computed at that point of the stream. Covers ring wrap-around, back-pressure, oversized calls and pacing. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include "gl_render.h"

/* ---------------------------------------------------------------- a minimal plat.h sys layer (pthreads) */
struct PlatMutex { pthread_mutex_t m; }; struct PlatCond { pthread_cond_t c; };
PlatMutex *plat_mutex_new(void) { PlatMutex *m = calloc(1, sizeof *m); pthread_mutex_init(&m->m, NULL); return m; }
void plat_mutex_lock(PlatMutex *m) { pthread_mutex_lock(&m->m); }
void plat_mutex_unlock(PlatMutex *m) { pthread_mutex_unlock(&m->m); }
PlatCond *plat_cond_new(void) { PlatCond *c = calloc(1, sizeof *c); pthread_cond_init(&c->c, NULL); return c; }
void plat_cond_wait(PlatCond *c, PlatMutex *m) { pthread_cond_wait(&c->c, &m->m); }
void plat_cond_signal(PlatCond *c) { pthread_cond_signal(&c->c); }
void plat_cond_broadcast(PlatCond *c) { pthread_cond_broadcast(&c->c); }
uint64_t plat_time_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec; }
typedef struct { void (*fn)(void *); void *arg; } St;
static void *tramp(void *p) { St s = *(St *)p; free(p); s.fn(s.arg); return NULL; }
static pthread_t gl_tid; static int gl_tid_set;
int gl_thread_spawn(void (*fn)(void *), void *arg) {
    St *s = malloc(sizeof *s); s->fn = fn; s->arg = arg; pthread_t t;
    if (pthread_create(&t, NULL, tramp, s)) return -1; pthread_detach(t); gl_tid = t; gl_tid_set = 1; return 0;
}

/* ---------------------------------------------------------------- the fake renderer */
struct PlatTexture { int w, h, levels, fmt, inited, id; };
static uint64_t H = 1469598103934665603ull; static int bad_thread, ntex;
static void hb(const void *p, size_t n) { const uint8_t *b = p; for (size_t i = 0; i < n; i++) { H ^= b[i]; H *= 1099511628211ull; } }
static void hi(int64_t v) { hb(&v, sizeof v); }
static void on_gl(void) { if (gl_tid_set && !pthread_equal(pthread_self(), gl_tid)) bad_thread++; }
static void ht(const PlatTexture *t) { hi(t ? t->id : -1); }
PlatTexture *glr_tex_new(int w, int h, int levels, PlatTexFormat fmt, int rt) {
    if (w < 1 || h < 1 || w > 4096 || h > 4096) return NULL;
    PlatTexture *t = calloc(1, sizeof *t); t->w = w; t->h = h; t->levels = rt ? 1 : levels; t->fmt = rt ? PLAT_TEX_RGBA8 : (int)fmt; t->id = ++ntex; return t;
}
size_t glr_tex_upload_bytes(const PlatTexture *t, int level, int w, int h, int *rgba8) {
    *rgba8 = 0; if (!t || level >= t->levels) return 0;
    *rgba8 = t->fmt == PLAT_TEX_RGBA8;
    if (t->fmt != PLAT_TEX_RGBA8) { int lw = t->w >> level, lh = t->h >> level; if (lw < 1) lw = 1; if (lh < 1) lh = 1; return (size_t)((lw + 3) / 4) * ((lh + 3) / 4) * (t->fmt == PLAT_TEX_DXT1 ? 8 : 16); }
    return (size_t)w * h * 4;
}
void glr_tex_init(PlatTexture *t) { on_gl(); t->inited = 1; hi(1); ht(t); hi(t->w); hi(t->h); }
void glr_tex_upload(PlatTexture *t, int level, int x, int y, int w, int h, const void *data, int pitch) {
    if (level >= t->levels) return;                             /* as gl_render.c: no such level, nothing to do */
    on_gl(); if (!t->inited) bad_thread += 1000; hi(2); ht(t); hi(level); hi(x); hi(y); hi(w); hi(h);
    int rgba8; size_t n = glr_tex_upload_bytes(t, level, w, h, &rgba8);
    if (t->fmt != PLAT_TEX_RGBA8) hb(data, n); else for (int r = 0; r < h; r++) hb((const uint8_t *)data + (size_t)r * pitch, (size_t)w * 4);
}
void glr_tex_destroy(PlatTexture *t) { on_gl(); hi(3); ht(t); memset(t, 0xAB, sizeof *t); free(t); }
int glr_tex_read(PlatTexture *t, int x, int y, int w, int h, void *out, int pitch) {
    on_gl(); hi(4); ht(t); hi(x); hi(y); hi(w); hi(h);
    for (int r = 0; r < h; r++) for (int i = 0; i < w * 4; i++) ((uint8_t *)out)[(size_t)r * pitch + i] = (uint8_t)(H >> ((i + r) % 8 * 8));
    return 0;
}
void glr_begin_frame(void) { on_gl(); hi(5); }
void glr_set_target(PlatTexture *t) { on_gl(); hi(6); ht(t); }
void glr_viewport(const PlatViewport *vp) { on_gl(); hi(7); hb(vp, sizeof *vp); }
void glr_scissor(int e, int x, int y, int w, int h) { on_gl(); hi(8); hi(e); hi(x); hi(y); hi(w); hi(h); }
void glr_clear(int c, uint32_t argb, int d, float z, int s, uint32_t sv) { on_gl(); hi(9); hi(c); hi(argb); hi(d); hb(&z, 4); hi(s); hi(sv); }
void glr_draw(int prim, const PlatVertex *v, int nv, const uint16_t *idx, int ni, const PlatDrawState *st) {
    on_gl(); hi(10); hi(prim); hi(nv); hi(ni); hb(v, (size_t)nv * sizeof *v); hb(idx, (size_t)ni * 2);
    hi(st->blend); hi(st->fog_color); ht(st->stage[0].texture); ht(st->stage[1].texture);
}
void glr_copy(PlatTexture *s, int sx, int sy, int sw, int sh, PlatTexture *d, int dx, int dy, int dw, int dh, int l) { on_gl(); hi(11); ht(s); ht(d); hi(sx + sy * 7 + sw * 13 + sh * 17 + dx * 19 + dy * 23 + dw * 29 + dh * 31 + l); }
static int presents;
void glr_present(void) { on_gl(); hi(12); presents++; }
void glr_gamma(const uint16_t ramp[3][256]) { on_gl(); hi(13); hb(ramp, 3 * 256 * 2); }

/* ---------------------------------------------------------------- the call stream */
static uint64_t rs; static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)rs; }
static uint64_t run(unsigned seed, int calls, uint64_t *readsum) {
    rs = 0x9E3779B97F4A7C15ull ^ seed; PlatTexture *tex[64] = { 0 }; *readsum = 0;
    static PlatVertex vb[20000]; static uint16_t ib[60000]; static uint8_t buf[2048 * 1024 + 4096];
    for (int k = 0; k < calls; k++) {
        uint32_t op = rnd() % 100;
        if (op < 40) {                                           /* draw: mostly small, now and then huge (> the ring's limit) */
            int nv = (rnd() % 50 == 0) ? 12000 + rnd() % 8000 : 1 + rnd() % 600, ni = nv * 3 > 60000 ? 60000 : nv * 3;
            for (int i = 0; i < nv; i++) { vb[i].x = (float)rnd(); vb[i].diffuse = rnd(); vb[i].u1 = (float)i; }
            for (int i = 0; i < ni; i++) ib[i] = (uint16_t)(rnd() % (uint32_t)nv);
            PlatDrawState st; memset(&st, 0, sizeof st); st.blend = (uint8_t)rnd(); st.fog_color = rnd(); st.stage[0].texture = tex[rnd() % 64]; st.stage[1].texture = tex[rnd() % 64];
            plat_gfx_draw(PLAT_PRIM_TRIANGLES, vb, nv, ib, ni, &st);
        } else if (op < 50) {                                    /* create / destroy */
            int i = (int)(rnd() % 64);
            if (tex[i]) { plat_tex_destroy(tex[i]); tex[i] = NULL; }
            else { int w = 1 << (rnd() % 10), h = 1 << (rnd() % 10); tex[i] = plat_tex_create(w, h, 1 + (int)(rnd() % 3), (PlatTexFormat)(rnd() % 4), rnd() % 8 == 0); }
        } else if (op < 62) {                                    /* upload, sometimes with padded rows, sometimes 2 MB */
            int i = (int)(rnd() % 64); if (!tex[i]) continue; PlatTexture *t = tex[i];
            int big = rnd() % 40 == 0, w = big ? 724 : 1 + (int)(rnd() % (uint32_t)t->w), h = big ? 724 : 1 + (int)(rnd() % (uint32_t)t->h), pitch = w * 4 + (rnd() % 2 ? 0 : 12);
            if ((size_t)pitch * h > sizeof buf) continue;
            int level = (int)(rnd() % 3), rgba8; size_t fill = glr_tex_upload_bytes(t, level, w, h, &rgba8);   /* compressed: the whole level */
            if (fill < (size_t)pitch * h) fill = (size_t)pitch * h; if (fill > sizeof buf) continue;
            for (size_t b = 0; b < fill; b += 4) { uint32_t v = rnd(); memcpy(buf + b, &v, 4); }
            plat_tex_upload(t, level, 0, 0, w, h, buf, pitch);
        } else if (op < 64) {                                    /* read-back: must see everything before it */
            int w = 1 + (int)(rnd() % 32), h = 1 + (int)(rnd() % 8); static uint8_t out[32 * 4 * 8 + 64];
            plat_tex_read(tex[rnd() % 64], 0, 0, w, h, out, w * 4); hb(out, (size_t)w * 4 * h); *readsum = *readsum * 31 + out[0] + out[w * 4 * h - 1];
        } else if (op < 70) plat_gfx_present();
        else if (op < 72) { static uint16_t ramp[3][256]; for (int i = 0; i < 256; i++) ramp[i % 3][i] = (uint16_t)rnd(); plat_gfx_gamma((const uint16_t (*)[256])ramp); }
        else if (op < 76) plat_gfx_copy(tex[rnd() % 64], 1, 2, 3, 4, tex[rnd() % 64], 5, 6, 7, 8, (int)(rnd() % 2));
        else if (op < 82) { PlatViewport v = { (int)(rnd() % 99), 1, 2, 3, 0, 1 }; plat_gfx_viewport(&v); }
        else if (op < 86) plat_gfx_scissor((int)(rnd() % 2), 1, 2, 3, 4);
        else if (op < 92) plat_gfx_clear(1, rnd(), 1, 0.5f, 0, 0);
        else if (op < 96) plat_gfx_set_target(tex[rnd() % 64]);
        else plat_gfx_begin_frame();
    }
    for (int i = 0; i < 64; i++) if (tex[i]) plat_tex_destroy(tex[i]);
    uint64_t r; plat_tex_read(NULL, 0, 0, 1, 1, &r, 4);         /* waits for everything */
    return H;
}
static void reset(void *p) { (void)p; H = 1469598103934665603ull; ntex = 0; presents = 0; }

int main(int argc, char **argv) {
    unsigned seed = argc > 1 ? (unsigned)atoi(argv[1]) : 1; int calls = argc > 2 ? atoi(argv[2]) : 200000;
    uint64_t rs1, rs2;
    uint64_t direct = run(seed, calls, &rs1); int p1 = presents;
    if (glt_start()) { puts("glthread: could not start the render thread"); return 2; }
    glt_call(reset, NULL);
    uint64_t threaded = run(seed, calls, &rs2);
    int ok = direct == threaded && rs1 == rs2 && !bad_thread && p1 == presents;
    printf("glthread seed %u, %d calls, %d presents: direct %016llx threaded %016llx, read-backs %s, wrong-thread calls %d: %s\n", seed, calls, p1,
           (unsigned long long)direct, (unsigned long long)threaded, rs1 == rs2 ? "same" : "DIFFER", bad_thread, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
