/* gl_render.h: the OpenGL 2.1 / OpenGL ES 2.0 renderer behind plat.h's renderer functions.
 *
 * gl_render.c is the renderer itself; its entry points are the glr_* functions below and must all run on the thread
 * that owns the GL context. gl_thread.c implements plat.h's plat_tex_* / plat_gfx_* on top of them: either directly
 * (the caller's thread owns the context) or by recording each call and replaying it on a render thread of its own. */
#ifndef GL_RENDER_H
#define GL_RENDER_H
#include "plat.h"
int  gl_init(void *(*get_proc)(const char *), int gles);
void gl_shutdown(void);
void gl_caps(PlatVideoCaps *c);
void gl_resize_backbuffer(int w, int h);
/* supplied by the windowing code; called on the renderer's thread */
void gl_present_rect(int *x, int *y, int *w, int *h, int *win_w, int *win_h);
void gl_swap(void);

/* the renderer (gl_render.c), on the GL thread */
PlatTexture *glr_tex_new(int w, int h, int levels, PlatTexFormat fmt, int render_target);   /* no GL: any thread (after gl_init) */
size_t glr_tex_upload_bytes(const PlatTexture *t, int level, int w, int h, int *rgba8);     /* bytes plat_tex_upload reads (rows packed); *rgba8: rows of w*4 bytes at `pitch` (else one compressed level); any thread */
void glr_tex_init(PlatTexture *t);
void glr_tex_upload(PlatTexture *t, int level, int x, int y, int w, int h, const void *data, int pitch);
void glr_tex_destroy(PlatTexture *t);
int  glr_tex_read(PlatTexture *t, int x, int y, int w, int h, void *out, int pitch);
void glr_begin_frame(void);
void glr_set_target(PlatTexture *target);
void glr_viewport(const PlatViewport *vp);
void glr_scissor(int enable, int x, int y, int w, int h);
void glr_clear(int color, uint32_t argb, int depth, float z, int stencil, uint32_t s);
void glr_draw(int prim, const PlatVertex *v, int nverts, const uint16_t *idx, int nidx, const PlatDrawState *st);
void glr_copy(PlatTexture *src, int sx, int sy, int sw, int sh, PlatTexture *dst, int dx, int dy, int dw, int dh, int linear);
void glr_present(void);
void glr_gamma(const uint16_t ramp[3][256]);

/* gl_thread.c: the render thread. The windowing code calls glt_start() before setting up GL when it wants one, then does
 * all its GL work (context, gl_init, gl_resize_backbuffer, gl_caps) through glt_call, which runs fn on the GL thread and
 * waits (or just calls it when there is no render thread). */
int  glt_start(void);                              /* 0 on success */
void glt_call(void (*fn)(void *), void *arg);
void glt_set_caps(const PlatVideoCaps *c);         /* what plat_video_caps returns from then on (call it after gl_init) */
int  glt_get_caps(PlatVideoCaps *c);               /* 1 if set */
extern int glt_on;                                 /* a render thread is running */
extern uint64_t glt_wait_ns;                       /* time the game thread spent waiting for the render thread */
/* supplied by the windowing code: start fn(arg) on a new thread suitable for rendering (stack, core); 0 on success */
int  gl_thread_spawn(void (*fn)(void *), void *arg);
#endif
