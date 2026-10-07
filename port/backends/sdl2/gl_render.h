/* gl_render.h: the OpenGL 2.1 / OpenGL ES 2.0 renderer behind plat.h's renderer functions. */
#ifndef GL_RENDER_H
#define GL_RENDER_H
#include "plat.h"
int  gl_init(void *(*get_proc)(const char *), int gles);
void gl_shutdown(void);
void gl_caps(PlatVideoCaps *c);
void gl_resize_backbuffer(int w, int h);
/* supplied by the windowing code */
void gl_present_rect(int *x, int *y, int *w, int *h, int *win_w, int *win_h);
void gl_swap(void);
#endif
