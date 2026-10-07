/* plat_null.c: the headless backend. No window, no picture, no sound device: the renderer keeps textures in memory
 * (so read-backs and copies work and can be tested) and draws nothing. Useful for automated tests and as the starting
 * point of a new backend: every function here is one a port has to provide. */
#include <stdlib.h>
#include <string.h>
#include "plat.h"

static int win_w = 640, win_h = 480, opened;
int  plat_video_open(int w, int h, int fullscreen, const char *title) { (void)fullscreen; (void)title; win_w = w; win_h = h; opened = 1; return 0; }
void plat_video_close(void) { opened = 0; }
void plat_video_caps(PlatVideoCaps *c) { memset(c, 0, sizeof *c); c->max_texture_size = 4096; c->dxt = 0; c->npot = 1; c->render_targets = 1; c->max_anisotropy = 16; }
void plat_video_window_size(int *w, int *h) { *w = win_w; *h = win_h; }
void plat_video_display_size(int *w, int *h) { *w = 1920; *h = 1080; }
void plat_video_poll(void (*sink)(const PlatEvent *, void *), void *user) { (void)sink; (void)user; }
void plat_video_set_title(const char *t) { (void)t; }
void plat_video_mouse_mode(int relative, int visible) { (void)relative; (void)visible; }
void plat_video_warp_mouse(int x, int y) { (void)x; (void)y; }

struct PlatTexture { int w, h, levels; PlatTexFormat fmt; uint8_t *rgba; };
PlatTexture *plat_tex_create(int w, int h, int levels, PlatTexFormat fmt, int rt) {
    (void)rt; PlatTexture *t = calloc(1, sizeof *t); t->w = w; t->h = h; t->levels = levels; t->fmt = fmt;
    if (fmt == PLAT_TEX_RGBA8) t->rgba = calloc((size_t)w * (size_t)h, 4);
    return t;
}
void plat_tex_upload(PlatTexture *t, int level, int x, int y, int w, int h, const void *data, int pitch) {
    if (level || !t->rgba) return;
    for (int r = 0; r < h; r++) if (y + r < t->h) memcpy(t->rgba + ((size_t)(y + r) * (size_t)t->w + (size_t)x) * 4, (const uint8_t *)data + (size_t)r * (size_t)pitch, (size_t)(x + w <= t->w ? w : t->w - x) * 4);
}
void plat_tex_destroy(PlatTexture *t) { if (t) { free(t->rgba); free(t); } }
int plat_tex_read(PlatTexture *t, int x, int y, int w, int h, void *out, int pitch) {
    for (int r = 0; r < h; r++) {
        uint8_t *o = (uint8_t *)out + (size_t)r * (size_t)pitch;
        if (t && t->rgba && y + r < t->h) memcpy(o, t->rgba + ((size_t)(y + r) * (size_t)t->w + (size_t)x) * 4, (size_t)w * 4); else memset(o, 0, (size_t)w * 4);
    }
    return 0;
}
void plat_gfx_begin_frame(void) {}
void plat_gfx_set_target(PlatTexture *t) { (void)t; }
void plat_gfx_viewport(const PlatViewport *vp) { (void)vp; }
void plat_gfx_scissor(int enable, int x, int y, int w, int h) { (void)enable; (void)x; (void)y; (void)w; (void)h; }
void plat_gfx_clear(int color, uint32_t argb, int depth, float z, int stencil, uint32_t s) { (void)color; (void)argb; (void)depth; (void)z; (void)stencil; (void)s; }
void plat_gfx_draw(int prim, const PlatVertex *v, int n, const uint16_t *idx, int nidx, const PlatDrawState *st) { (void)prim; (void)v; (void)n; (void)idx; (void)nidx; (void)st; }
void plat_gfx_copy(PlatTexture *src, int sx, int sy, int sw, int sh, PlatTexture *dst, int dx, int dy, int dw, int dh, int linear) {
    (void)src; (void)sx; (void)sy; (void)sw; (void)sh; (void)dst; (void)dx; (void)dy; (void)dw; (void)dh; (void)linear;
}
void plat_gfx_present(void) {}
void plat_gfx_gamma(const uint16_t ramp[3][256]) { (void)ramp; }

int  plat_audio_open(int rate, void (*fill)(int16_t *, int, void *), void *user) { (void)rate; (void)fill; (void)user; return 0; }
void plat_audio_close(void) {}
void plat_audio_pause(int p) { (void)p; }

int port_main(int argc, char **argv);
int main(int argc, char **argv) { return port_main(argc, argv); }
