/* gl_render.c: plat.h's renderer on OpenGL 2.1 or OpenGL ES 2.0 (the subset both share).
 *
 * The fixed-function pixel pipeline the Direct3D front end describes (two texture stages, alpha test, fog, specular)
 * becomes a small generated fragment shader, cached per state combination. Everything is drawn into framebuffer
 * objects whose rows run top to bottom (row 0 = the top of the picture, like Direct3D surfaces and like the texture
 * data the front end uploads); the back buffer is one of them and is scaled into the window on present. Function
 * pointers are loaded at run time, so no OpenGL headers or libraries are needed at build time. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gl_render.h"

/* ---------------------------------------------------------------- the OpenGL subset */
typedef unsigned int GLenum, GLuint, GLbitfield; typedef int GLint, GLsizei; typedef unsigned char GLboolean, GLubyte; typedef float GLfloat; typedef char GLchar; typedef ptrdiff_t GLsizeiptr, GLintptr;
#define GL_ZERO 0
#define GL_ONE 1
#define GL_TRIANGLES 4
#define GL_LINES 1
#define GL_POINTS 0
#define GL_NEVER 0x0200
#define GL_FRONT 0x0404
#define GL_BACK 0x0405
#define GL_CW 0x0900
#define GL_CCW 0x0901
#define GL_CULL_FACE 0x0B44
#define GL_DEPTH_TEST 0x0B71
#define GL_BLEND 0x0BE2
#define GL_SCISSOR_TEST 0x0C11
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_MAX_TEXTURE_SIZE 0x0D33
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_BORDER_COLOR 0x1004
#define GL_UNSIGNED_BYTE 0x1401
#define GL_UNSIGNED_SHORT 0x1403
#define GL_FLOAT 0x1406
#define GL_RGBA 0x1908
#define GL_EXTENSIONS 0x1F03
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_NEAREST_MIPMAP_NEAREST 0x2700
#define GL_LINEAR_MIPMAP_NEAREST 0x2701
#define GL_NEAREST_MIPMAP_LINEAR 0x2702
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_REPEAT 0x2901
#define GL_CLAMP_TO_BORDER 0x812D
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_TEXTURE_MAX_LEVEL 0x813D
#define GL_MIRRORED_REPEAT 0x8370
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE_MAX_ANISOTROPY 0x84FE
#define GL_MAX_TEXTURE_MAX_ANISOTROPY 0x84FF
#define GL_COMPRESSED_RGBA_S3TC_DXT1 0x83F1
#define GL_COMPRESSED_RGBA_S3TC_DXT3 0x83F2
#define GL_COMPRESSED_RGBA_S3TC_DXT5 0x83F3
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_STREAM_DRAW 0x88E0
#define GL_DEPTH24_STENCIL8 0x88F0
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_FRAMEBUFFER 0x8D40
#define GL_RENDERBUFFER 0x8D41
#define GL_DEPTH_COMPONENT16 0x81A5
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_STENCIL_ATTACHMENT 0x8D20
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_DEPTH_BUFFER_BIT 0x0100
#define GL_STENCIL_BUFFER_BIT 0x0400
#define GL_NO_ERROR 0
#define GL_VERSION 0x1F02
#define GL_RENDERER 0x1F01
#if defined(_WIN32) && !defined(_WIN64)
#define GLCALL __stdcall          /* OpenGL entry points use stdcall on 32-bit Windows */
#else
#define GLCALL
#endif
#define GLF(ret, name, args) static ret (GLCALL *name) args;
#define GL_FUNCS \
    GLF(const GLubyte *, glGetString, (GLenum)) GLF(void, glGetIntegerv, (GLenum, GLint *)) GLF(void, glGetFloatv, (GLenum, GLfloat *)) GLF(void, glEnable, (GLenum)) \
    GLF(void, glDisable, (GLenum)) GLF(void, glBlendFunc, (GLenum, GLenum)) GLF(void, glDepthFunc, (GLenum)) GLF(void, glDepthMask, (GLboolean)) \
    GLF(void, glColorMask, (GLboolean, GLboolean, GLboolean, GLboolean)) GLF(void, glCullFace, (GLenum)) GLF(void, glFrontFace, (GLenum)) \
    GLF(void, glViewport, (GLint, GLint, GLsizei, GLsizei)) GLF(void, glScissor, (GLint, GLint, GLsizei, GLsizei)) GLF(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat)) \
    GLF(void, glClearDepth, (double)) GLF(void, glClearDepthf, (GLfloat)) GLF(void, glClearStencil, (GLint)) GLF(void, glClear, (GLbitfield)) \
    GLF(void, glGenTextures, (GLsizei, GLuint *)) GLF(void, glDeleteTextures, (GLsizei, const GLuint *)) GLF(void, glBindTexture, (GLenum, GLuint)) \
    GLF(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
    GLF(void, glTexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *)) \
    GLF(void, glCompressedTexImage2D, (GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void *)) \
    GLF(void, glTexParameteri, (GLenum, GLenum, GLint)) GLF(void, glTexParameterf, (GLenum, GLenum, GLfloat)) GLF(void, glTexParameterfv, (GLenum, GLenum, const GLfloat *)) \
    GLF(void, glActiveTexture, (GLenum)) GLF(void, glPixelStorei, (GLenum, GLint)) GLF(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *)) \
    GLF(void, glGenFramebuffers, (GLsizei, GLuint *)) GLF(void, glDeleteFramebuffers, (GLsizei, const GLuint *)) GLF(void, glBindFramebuffer, (GLenum, GLuint)) \
    GLF(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) GLF(void, glFramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint)) \
    GLF(GLenum, glCheckFramebufferStatus, (GLenum)) GLF(void, glGenRenderbuffers, (GLsizei, GLuint *)) GLF(void, glDeleteRenderbuffers, (GLsizei, const GLuint *)) \
    GLF(void, glBindRenderbuffer, (GLenum, GLuint)) GLF(void, glRenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei)) \
    GLF(GLuint, glCreateShader, (GLenum)) GLF(void, glShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *)) GLF(void, glCompileShader, (GLuint)) \
    GLF(void, glGetShaderiv, (GLuint, GLenum, GLint *)) GLF(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) GLF(GLuint, glCreateProgram, (void)) \
    GLF(void, glAttachShader, (GLuint, GLuint)) GLF(void, glBindAttribLocation, (GLuint, GLuint, const GLchar *)) GLF(void, glLinkProgram, (GLuint)) \
    GLF(void, glGetProgramiv, (GLuint, GLenum, GLint *)) GLF(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) GLF(void, glUseProgram, (GLuint)) \
    GLF(GLint, glGetUniformLocation, (GLuint, const GLchar *)) GLF(void, glUniform1i, (GLint, GLint)) GLF(void, glUniform1f, (GLint, GLfloat)) \
    GLF(void, glUniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat)) GLF(void, glDeleteShader, (GLuint)) GLF(void, glDeleteProgram, (GLuint)) \
    GLF(void, glGenBuffers, (GLsizei, GLuint *)) GLF(void, glBindBuffer, (GLenum, GLuint)) GLF(void, glBufferData, (GLenum, GLsizeiptr, const void *, GLenum)) \
    GLF(void, glDeleteBuffers, (GLsizei, const GLuint *)) GLF(void, glEnableVertexAttribArray, (GLuint)) GLF(void, glDisableVertexAttribArray, (GLuint)) \
    GLF(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) GLF(void, glDrawElements, (GLenum, GLsizei, GLenum, const void *)) \
    GLF(void, glDrawArrays, (GLenum, GLint, GLsizei)) GLF(void, glFinish, (void)) GLF(GLenum, glGetError, (void))
GL_FUNCS
#undef GLF

static int gles, has_dxt, has_aniso, has_border, has_maxlevel, packed_depth; static float max_aniso = 1; static int max_tex = 2048;

/* ---------------------------------------------------------------- textures and targets */
struct PlatTexture {
    GLuint tex, fbo, depth; int w, h, levels, rt; PlatTexFormat fmt;
    int last_min, last_mag, last_wu, last_wv, last_aniso; uint32_t last_border;
};
static PlatTexture bb;            /* the back buffer */
static PlatTexture *target;       /* current render target (&bb for the back buffer) */
static void ensure_fbo(PlatTexture *t) {
    if (t->fbo) return;
    glGenFramebuffers(1, &t->fbo); glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->tex, 0);
    if (t->rt) {
        glGenRenderbuffers(1, &t->depth); glBindRenderbuffer(GL_RENDERBUFFER, t->depth);
        glRenderbufferStorage(GL_RENDERBUFFER, packed_depth ? GL_DEPTH24_STENCIL8 : GL_DEPTH_COMPONENT16, t->w, t->h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, t->depth);
        if (packed_depth) glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, t->depth);
    }
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) fprintf(stderr, "[gl] framebuffer %dx%d incomplete\n", t->w, t->h);
}
static void bind_target(PlatTexture *t) { ensure_fbo(t); glBindFramebuffer(GL_FRAMEBUFFER, t->fbo); }
static GLenum cfmt(PlatTexFormat f) { return f == PLAT_TEX_DXT1 ? GL_COMPRESSED_RGBA_S3TC_DXT1 : f == PLAT_TEX_DXT3 ? GL_COMPRESSED_RGBA_S3TC_DXT3 : GL_COMPRESSED_RGBA_S3TC_DXT5; }
static int csize(PlatTexFormat f, int w, int h) { return ((w + 3) / 4) * ((h + 3) / 4) * (f == PLAT_TEX_DXT1 ? 8 : 16); }
static void tex_storage(PlatTexture *t) {
    glBindTexture(GL_TEXTURE_2D, t->tex);
    for (int l = 0; l < t->levels; l++) {
        int w = t->w >> l, h = t->h >> l; if (w < 1) w = 1; if (h < 1) h = 1;
        if (t->fmt == PLAT_TEX_RGBA8) glTexImage2D(GL_TEXTURE_2D, l, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        else { void *z = calloc(1, (size_t)csize(t->fmt, w, h)); glCompressedTexImage2D(GL_TEXTURE_2D, l, cfmt(t->fmt), w, h, 0, csize(t->fmt, w, h), z); free(z); }
    }
    if (has_maxlevel) glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, t->levels - 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    t->last_min = t->last_mag = GL_NEAREST; t->last_wu = t->last_wv = GL_REPEAT; t->last_aniso = 1;
}
PlatTexture *plat_tex_create(int w, int h, int levels, PlatTexFormat fmt, int rt) {
    if (w < 1 || h < 1 || w > max_tex || h > max_tex) return NULL;
    PlatTexture *t = calloc(1, sizeof *t); t->w = w; t->h = h; t->levels = levels < 1 ? 1 : levels; t->fmt = rt ? PLAT_TEX_RGBA8 : fmt; t->rt = rt;
    if (rt) t->levels = 1;
    if (gles && t->levels > 1) { int full = 1, m = w > h ? w : h; while (m > 1) { full++; m >>= 1; } if (t->levels != full) t->levels = 1; }   /* ES 2.0 needs complete chains */
    glGenTextures(1, &t->tex); tex_storage(t); return t;
}
void plat_tex_upload(PlatTexture *t, int level, int x, int y, int w, int h, const void *data, int pitch) {
    if (!t || level >= t->levels) return;
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, t->tex); glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (t->fmt != PLAT_TEX_RGBA8) { int lw = t->w >> level, lh = t->h >> level; if (lw < 1) lw = 1; if (lh < 1) lh = 1; glCompressedTexImage2D(GL_TEXTURE_2D, level, cfmt(t->fmt), lw, lh, 0, csize(t->fmt, lw, lh), data); return; }
    if (pitch == w * 4) { glTexSubImage2D(GL_TEXTURE_2D, level, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, data); return; }
    uint8_t *tight = malloc((size_t)w * h * 4); for (int r = 0; r < h; r++) memcpy(tight + (size_t)r * w * 4, (const uint8_t *)data + (size_t)r * pitch, (size_t)w * 4);
    glTexSubImage2D(GL_TEXTURE_2D, level, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, tight); free(tight);
}
void plat_tex_destroy(PlatTexture *t) {
    if (!t || t == &bb) return;
    if (target == t) { target = &bb; bind_target(&bb); }
    if (t->fbo) glDeleteFramebuffers(1, &t->fbo); if (t->depth) glDeleteRenderbuffers(1, &t->depth); glDeleteTextures(1, &t->tex); free(t);
}
int plat_tex_read(PlatTexture *t, int x, int y, int w, int h, void *out, int pitch) {
    if (!t) t = &bb;
    if (t->fmt != PLAT_TEX_RGBA8) return -1;
    ensure_fbo(t); glBindFramebuffer(GL_FRAMEBUFFER, t->fbo); glPixelStorei(GL_PACK_ALIGNMENT, 1);
    if (pitch == w * 4) glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, out);
    else { uint8_t *tmp = malloc((size_t)w * h * 4); glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, tmp); for (int r = 0; r < h; r++) memcpy((uint8_t *)out + (size_t)r * pitch, tmp + (size_t)r * w * 4, (size_t)w * 4); free(tmp); }
    bind_target(target); return 0;
}

/* ---------------------------------------------------------------- shaders */
static const char *VS =
    "attribute vec4 a_pos; attribute vec4 a_diff; attribute vec4 a_spec; attribute float a_fog; attribute vec4 a_uv;\n"
    "varying vec4 v_diff; varying vec4 v_spec; varying float v_fog; varying vec4 v_uv; uniform float u_zbias;\n"
    "void main() { v_diff = a_diff.bgra; v_spec = a_spec.bgra; v_fog = a_fog; v_uv = a_uv;\n"
    "  gl_Position = vec4(a_pos.x, -a_pos.y, 2.0 * a_pos.z - a_pos.w + 2.0 * u_zbias * a_pos.w, a_pos.w); }\n";
static GLuint compile(GLenum type, const char *src) {
    /* GLSL ES: vertex shaders default to highp; only the fragment shader needs (and gets) a default precision */
    char head[96]; snprintf(head, sizeof head, "%s", !gles ? "#version 120\n" : type == GL_FRAGMENT_SHADER ? "#version 100\nprecision mediump float;\n" : "#version 100\n");
    const char *parts[2] = { head, src }; GLuint s = glCreateShader(type); glShaderSource(s, 2, parts, NULL); glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[2048]; glGetShaderInfoLog(s, sizeof log, NULL, log); fprintf(stderr, "[gl] shader compile failed:\n%s\n%s\n", log, src); }
    return s;
}
static GLuint link(const char *vs, const char *fs) {
    GLuint p = glCreateProgram(), v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
    glAttachShader(p, v); glAttachShader(p, f);
    glBindAttribLocation(p, 0, "a_pos"); glBindAttribLocation(p, 1, "a_diff"); glBindAttribLocation(p, 2, "a_spec"); glBindAttribLocation(p, 3, "a_fog"); glBindAttribLocation(p, 4, "a_uv");
    glLinkProgram(p); GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char log[2048]; glGetProgramInfoLog(p, sizeof log, NULL, log); fprintf(stderr, "[gl] program link failed: %s\n", log); }
    glDeleteShader(v); glDeleteShader(f); return p;
}
typedef struct { uint8_t key[24]; GLuint prog; GLint u_t0, u_t1, u_tf, u_fogc, u_aref, u_zbias; } Prog;
static Prog progs[256]; static int nprogs;
static void arg(char *o, size_t n, uint8_t a, const char *swz) {
    const char *b = (a & 7) == PLAT_ARG_DIFFUSE ? "d" : (a & 7) == PLAT_ARG_CURRENT ? "cur" : (a & 7) == PLAT_ARG_TEXTURE ? "t" : (a & 7) == PLAT_ARG_TFACTOR ? "u_tf" : "s";
    char x[64]; if (a & PLAT_ARG_ALPHAREPLICATE) snprintf(x, sizeof x, "vec4(%s.a)", b); else snprintf(x, sizeof x, "%s", b);
    if (a & PLAT_ARG_COMPLEMENT) snprintf(o, n, "(1.0 - %s.%s)", x, swz); else snprintf(o, n, "%s.%s", x, swz);
}
static void op(char *o, size_t n, uint8_t opc, const char *a1, const char *a2, const char *swz) {
    switch (opc) {
    case PLAT_TOP_SELECTARG1: snprintf(o, n, "%s", a1); break;
    case PLAT_TOP_SELECTARG2: snprintf(o, n, "%s", a2); break;
    case PLAT_TOP_MODULATE2X: snprintf(o, n, "%s * %s * 2.0", a1, a2); break;
    case PLAT_TOP_MODULATE4X: snprintf(o, n, "%s * %s * 4.0", a1, a2); break;
    case PLAT_TOP_ADD: snprintf(o, n, "%s + %s", a1, a2); break;
    case PLAT_TOP_ADDSIGNED: snprintf(o, n, "%s + %s - 0.5", a1, a2); break;
    case PLAT_TOP_SUBTRACT: snprintf(o, n, "%s - %s", a1, a2); break;
    case PLAT_TOP_BLENDDIFFUSEALPHA: snprintf(o, n, "mix(%s, %s, d.a)", a2, a1); break;
    case PLAT_TOP_BLENDTEXTUREALPHA: snprintf(o, n, "mix(%s, %s, t.a)", a2, a1); break;
    case PLAT_TOP_BLENDCURRENTALPHA: snprintf(o, n, "mix(%s, %s, cur.a)", a2, a1); break;
    default: snprintf(o, n, "%s * %s", a1, a2); break;
    }
    (void)swz;
}
static Prog *program(const PlatDrawState *st) {
    uint8_t key[24] = { 0 }; int k = 0;
    for (int i = 0; i < PLAT_MAX_STAGES; i++) { const PlatStage *g = &st->stage[i]; key[k++] = g->color_op; key[k++] = g->color_arg1; key[k++] = g->color_arg2; key[k++] = g->alpha_op; key[k++] = g->alpha_arg1; key[k++] = g->alpha_arg2; key[k++] = g->texture != NULL; }
    key[k++] = st->alpha_test ? (uint8_t)(st->alpha_func + 1) : 0; key[k++] = st->fog; key[k++] = st->specular;
    for (int i = 0; i < nprogs; i++) if (!memcmp(progs[i].key, key, sizeof key)) return &progs[i];
    char fs[8192]; size_t n = 0;
    n += (size_t)snprintf(fs + n, sizeof fs - n, "varying vec4 v_diff; varying vec4 v_spec; varying float v_fog; varying vec4 v_uv;\n"
                          "uniform sampler2D u_t0; uniform sampler2D u_t1; uniform vec4 u_tf; uniform vec4 u_fogc; uniform float u_aref;\n"
                          "void main() { vec4 d = v_diff; vec4 s = v_spec; vec4 cur = d; vec4 t = vec4(1.0); vec3 c; float a;\n");
    for (int i = 0; i < PLAT_MAX_STAGES; i++) {
        const PlatStage *g = &st->stage[i]; if (g->color_op == PLAT_TOP_DISABLE) break;
        if (g->texture) n += (size_t)snprintf(fs + n, sizeof fs - n, "  t = texture2D(u_t%d, v_uv.%s);\n", i, i ? "zw" : "xy"); else n += (size_t)snprintf(fs + n, sizeof fs - n, "  t = vec4(1.0);\n");
        char a1[96], a2[96], e[256];
        arg(a1, sizeof a1, g->color_arg1, "rgb"); arg(a2, sizeof a2, g->color_arg2, "rgb"); op(e, sizeof e, g->color_op, a1, a2, "rgb");
        n += (size_t)snprintf(fs + n, sizeof fs - n, "  c = clamp(%s, 0.0, 1.0);\n", e);
        arg(a1, sizeof a1, g->alpha_arg1, "a"); arg(a2, sizeof a2, g->alpha_arg2, "a"); op(e, sizeof e, g->alpha_op == PLAT_TOP_DISABLE ? PLAT_TOP_SELECTARG1 : g->alpha_op, g->alpha_op == PLAT_TOP_DISABLE ? "cur.a" : a1, a2, "a");
        n += (size_t)snprintf(fs + n, sizeof fs - n, "  a = clamp(%s, 0.0, 1.0);\n  cur = vec4(c, a);\n", e);
    }
    if (st->specular) n += (size_t)snprintf(fs + n, sizeof fs - n, "  cur.rgb = min(cur.rgb + s.rgb, 1.0);\n");
    if (st->fog) n += (size_t)snprintf(fs + n, sizeof fs - n, "  cur.rgb = mix(u_fogc.rgb, cur.rgb, clamp(v_fog, 0.0, 1.0));\n");
    if (st->alpha_test) {
        static const char *cmp[8] = { "false", "<", "==", "<=", ">", "!=", ">=", "true" };
        if (st->alpha_func == PLAT_CMP_NEVER) n += (size_t)snprintf(fs + n, sizeof fs - n, "  discard;\n");
        else if (st->alpha_func != PLAT_CMP_ALWAYS) n += (size_t)snprintf(fs + n, sizeof fs - n, "  if (!(floor(cur.a * 255.0 + 0.5) %s u_aref)) discard;\n", cmp[st->alpha_func]);
    }
    snprintf(fs + n, sizeof fs - n, "  gl_FragColor = cur; }\n");
    if (nprogs == 256) { glDeleteProgram(progs[0].prog); memmove(progs, progs + 1, sizeof progs[0] * 255); nprogs--; }
    Prog *p = &progs[nprogs++]; memcpy(p->key, key, sizeof key); p->prog = link(VS, fs);
    p->u_t0 = glGetUniformLocation(p->prog, "u_t0"); p->u_t1 = glGetUniformLocation(p->prog, "u_t1"); p->u_tf = glGetUniformLocation(p->prog, "u_tf");
    p->u_fogc = glGetUniformLocation(p->prog, "u_fogc"); p->u_aref = glGetUniformLocation(p->prog, "u_aref"); p->u_zbias = glGetUniformLocation(p->prog, "u_zbias");
    glUseProgram(p->prog); glUniform1i(p->u_t0, 0); glUniform1i(p->u_t1, 1);
    return p;
}
/* the quad shader for copies and present: position in NDC, uv; with an optional gamma table */
static GLuint quad_prog, gamma_prog, gamma_tex; static int gamma_on;
static const char *QVS = "attribute vec4 a_pos; attribute vec4 a_uv; varying vec2 v_uv; void main() { v_uv = a_uv.xy; gl_Position = vec4(a_pos.xy, 0.0, 1.0); }\n";
static const char *QFS = "varying vec2 v_uv; uniform sampler2D u_t0; void main() { gl_FragColor = texture2D(u_t0, v_uv); }\n";
static const char *GFS = "varying vec2 v_uv; uniform sampler2D u_t0; uniform sampler2D u_t1; void main() { vec4 c = texture2D(u_t0, v_uv);\n"
                         "  gl_FragColor = vec4(texture2D(u_t1, vec2(c.r * 255.0 / 256.0 + 0.5 / 256.0, 0.5)).r, texture2D(u_t1, vec2(c.g * 255.0 / 256.0 + 0.5 / 256.0, 0.5)).g,"
                         " texture2D(u_t1, vec2(c.b * 255.0 / 256.0 + 0.5 / 256.0, 0.5)).b, 1.0); }\n";

/* ---------------------------------------------------------------- drawing */
static GLuint vbo, ibo;
static PlatViewport cur_vp; static int sc_on, sc_x, sc_y, sc_w, sc_h;
static void apply_scissor(void) {
    /* the viewport always clips like Direct3D's; the user scissor narrows it further */
    int x0 = cur_vp.x, y0 = cur_vp.y, x1 = cur_vp.x + cur_vp.w, y1 = cur_vp.y + cur_vp.h;
    if (sc_on) { if (sc_x > x0) x0 = sc_x; if (sc_y > y0) y0 = sc_y; if (sc_x + sc_w < x1) x1 = sc_x + sc_w; if (sc_y + sc_h < y1) y1 = sc_y + sc_h; }
    if (x1 < x0) x1 = x0; if (y1 < y0) y1 = y0;
    glEnable(GL_SCISSOR_TEST); glScissor(x0, y0, x1 - x0, y1 - y0);
}
void plat_gfx_begin_frame(void) {}
void plat_gfx_set_target(PlatTexture *t) { target = t ? t : &bb; bind_target(target); }
void plat_gfx_viewport(const PlatViewport *vp) { cur_vp = *vp; glViewport(vp->x, vp->y, vp->w, vp->h); apply_scissor(); }
void plat_gfx_scissor(int enable, int x, int y, int w, int h) { sc_on = enable; sc_x = x; sc_y = y; sc_w = w; sc_h = h; apply_scissor(); }
void plat_gfx_clear(int color, uint32_t argb, int depth, float z, int stencil, uint32_t s) {
    GLbitfield m = 0;
    if (color) { glColorMask(1, 1, 1, 1); glClearColor(((argb >> 16) & 255) / 255.0f, ((argb >> 8) & 255) / 255.0f, (argb & 255) / 255.0f, (argb >> 24) / 255.0f); m |= GL_COLOR_BUFFER_BIT; }
    if (depth) { glDepthMask(1); if (glClearDepthf) glClearDepthf(z); else glClearDepth(z); m |= GL_DEPTH_BUFFER_BIT; }
    if (stencil) { glClearStencil((GLint)s); m |= GL_STENCIL_BUFFER_BIT; }
    if (m) glClear(m);
}
static const GLenum blend_gl[] = { GL_ZERO, GL_ONE, 0x0300, 0x0301, 0x0302, 0x0303, 0x0304, 0x0305, 0x0306, 0x0307, 0x0308 };
static GLint wrap_gl(uint8_t w) { return w == PLAT_WRAP_MIRROR ? GL_MIRRORED_REPEAT : w == PLAT_WRAP_CLAMP ? GL_CLAMP_TO_EDGE : w == PLAT_WRAP_BORDER ? (has_border ? GL_CLAMP_TO_BORDER : GL_CLAMP_TO_EDGE) : GL_REPEAT; }
static void sampler(int unit, const PlatStage *g) {
    PlatTexture *t = g->texture; glActiveTexture(GL_TEXTURE0 + (GLenum)unit); glBindTexture(GL_TEXTURE_2D, t ? t->tex : 0); if (!t) return;
    int mip = t->levels > 1 ? g->mip_filter : PLAT_MIP_NONE;
    GLint mn = g->min_filter == PLAT_FILTER_LINEAR ? (mip == PLAT_MIP_NONE ? GL_LINEAR : mip == PLAT_MIP_NEAREST ? GL_LINEAR_MIPMAP_NEAREST : GL_LINEAR_MIPMAP_LINEAR)
                                                   : (mip == PLAT_MIP_NONE ? GL_NEAREST : mip == PLAT_MIP_NEAREST ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_LINEAR);
    GLint mg = g->mag_filter == PLAT_FILTER_LINEAR ? GL_LINEAR : GL_NEAREST, wu = wrap_gl(g->wrap_u), wv = wrap_gl(g->wrap_v);
    if (mn != t->last_min) { glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mn); t->last_min = mn; }
    if (mg != t->last_mag) { glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mg); t->last_mag = mg; }
    if (wu != t->last_wu) { glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wu); t->last_wu = wu; }
    if (wv != t->last_wv) { glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wv); t->last_wv = wv; }
    if (has_border && g->border_color != t->last_border) { float b[4] = { ((g->border_color >> 16) & 255) / 255.0f, ((g->border_color >> 8) & 255) / 255.0f, (g->border_color & 255) / 255.0f, (g->border_color >> 24) / 255.0f }; glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, b); t->last_border = g->border_color; }
    int an = g->anisotropy > 1 ? g->anisotropy : 1; if (has_aniso && an != t->last_aniso) { glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, an > max_aniso ? max_aniso : (float)an); t->last_aniso = an; }
}
static void attribs(int quad) {
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    if (quad) { glVertexAttribPointer(0, 2, GL_FLOAT, 0, 16, (void *)0); glVertexAttribPointer(4, 2, GL_FLOAT, 0, 16, (void *)8); for (int i = 1; i <= 3; i++) glDisableVertexAttribArray((GLuint)i); glEnableVertexAttribArray(0); glEnableVertexAttribArray(4); return; }
    glVertexAttribPointer(0, 4, GL_FLOAT, 0, sizeof(PlatVertex), (void *)0);
    glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, 1, sizeof(PlatVertex), (void *)16);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, 1, sizeof(PlatVertex), (void *)20);
    glVertexAttribPointer(3, 1, GL_FLOAT, 0, sizeof(PlatVertex), (void *)24);
    glVertexAttribPointer(4, 4, GL_FLOAT, 0, sizeof(PlatVertex), (void *)28);
    for (GLuint i = 0; i <= 4; i++) glEnableVertexAttribArray(i);
}
void plat_gfx_draw(int prim, const PlatVertex *v, int nverts, const uint16_t *idx, int nidx, const PlatDrawState *st) {
    if (!nverts || !nidx) return;
    Prog *p = program(st); glUseProgram(p->prog);
    glUniform4f(p->u_tf, ((st->texture_factor >> 16) & 255) / 255.0f, ((st->texture_factor >> 8) & 255) / 255.0f, (st->texture_factor & 255) / 255.0f, (st->texture_factor >> 24) / 255.0f);
    glUniform4f(p->u_fogc, ((st->fog_color >> 16) & 255) / 255.0f, ((st->fog_color >> 8) & 255) / 255.0f, (st->fog_color & 255) / 255.0f, 1.0f);
    glUniform1f(p->u_aref, (float)st->alpha_ref); glUniform1f(p->u_zbias, st->depth_bias);
    for (int i = 0; i < PLAT_MAX_STAGES; i++) sampler(i, &st->stage[i]);
    if (st->blend) { glEnable(GL_BLEND); glBlendFunc(blend_gl[st->src_blend < 11 ? st->src_blend : 1], blend_gl[st->dst_blend < 11 ? st->dst_blend : 0]); } else glDisable(GL_BLEND);
    if (st->depth_test) { glEnable(GL_DEPTH_TEST); glDepthFunc(GL_NEVER + st->depth_func); } else glDisable(GL_DEPTH_TEST);
    glDepthMask(st->depth_write ? 1 : 0);
    glColorMask(st->color_write & 1, (st->color_write >> 1) & 1, (st->color_write >> 2) & 1, (st->color_write >> 3) & 1);
    /* rows run top-down here, so a triangle that is clockwise on the target is counter-clockwise to OpenGL */
    if (st->cull == PLAT_CULL_NONE || prim != PLAT_PRIM_TRIANGLES) glDisable(GL_CULL_FACE);
    else { glEnable(GL_CULL_FACE); glFrontFace(GL_CCW); glCullFace(st->cull == PLAT_CULL_CW ? GL_FRONT : GL_BACK); }
    glBindBuffer(GL_ARRAY_BUFFER, vbo); glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)nverts * sizeof *v), v, GL_STREAM_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo); glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)((size_t)nidx * 2), idx, GL_STREAM_DRAW);
    attribs(0);
    glDrawElements(prim == PLAT_PRIM_TRIANGLES ? GL_TRIANGLES : prim == PLAT_PRIM_LINES ? GL_LINES : GL_POINTS, nidx, GL_UNSIGNED_SHORT, (void *)0);
}
/* draws texture src (u0,v0)-(u1,v1) over the whole current viewport */
static void quad(GLuint prog, GLuint tex, float u0, float v0, float u1, float v1, int linear) {
    float q[16] = { -1, -1, u0, v0, 1, -1, u1, v0, -1, 1, u0, v1, 1, 1, u1, v1 };
    glUseProgram(prog); glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glColorMask(1, 1, 1, 1);
    glBindBuffer(GL_ARRAY_BUFFER, vbo); glBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STREAM_DRAW); attribs(1);
    glDrawArrays(0x0005, 0, 4);     /* GL_TRIANGLE_STRIP */
}
void plat_gfx_copy(PlatTexture *src, int sx, int sy, int sw, int sh, PlatTexture *dst, int dx, int dy, int dw, int dh, int linear) {
    if (!src) src = &bb; if (!dst) dst = &bb; if (src == dst) return;
    bind_target(dst); glViewport(dx, dy, dw, dh); glDisable(GL_SCISSOR_TEST);
    /* NDC y -1 is the first row of the target here (top-down rows), which must show the first row of the source rectangle */
    quad(quad_prog, src->tex, (float)sx / src->w, (float)sy / src->h, (float)(sx + sw) / src->w, (float)(sy + sh) / src->h, linear);
    /* the next draw re-applies this target's state */
    bind_target(target); glViewport(cur_vp.x, cur_vp.y, cur_vp.w, cur_vp.h); apply_scissor();
    for (int i = 0; i < 2; i++) { glActiveTexture(GL_TEXTURE0 + (GLenum)i); glBindTexture(GL_TEXTURE_2D, 0); }
    src->last_min = src->last_mag = -1;
}
void plat_gfx_present(void) {
    int x, y, w, h, ww, wh; gl_present_rect(&x, &y, &w, &h, &ww, &wh);
    glBindFramebuffer(GL_FRAMEBUFFER, 0); glDisable(GL_SCISSOR_TEST); glViewport(0, 0, ww, wh); glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
    glViewport(x, y, w, h);
    /* the window's framebuffer is bottom-up: the top of the picture (row 0, v = 0) goes to NDC y = +1 */
    if (gamma_on) { glActiveTexture(GL_TEXTURE0 + 1); glBindTexture(GL_TEXTURE_2D, gamma_tex); }
    quad(gamma_on ? gamma_prog : quad_prog, bb.tex, 0, 1, 1, 0, w != bb.w || h != bb.h);
    bb.last_min = bb.last_mag = -1;
    gl_swap();
    bind_target(target); glViewport(cur_vp.x, cur_vp.y, cur_vp.w, cur_vp.h); apply_scissor();
}
void plat_gfx_gamma(const uint16_t ramp[3][256]) {
    int ident = 1; uint8_t lut[256 * 4];
    for (int i = 0; i < 256; i++) { for (int c = 0; c < 3; c++) { lut[i * 4 + c] = (uint8_t)(ramp[c][i] >> 8); if (ramp[c][i] >> 8 != i) ident = 0; } lut[i * 4 + 3] = 255; }
    gamma_on = !ident; if (!gamma_on) return;
    if (!gamma_tex) glGenTextures(1, &gamma_tex);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, gamma_tex); glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, lut);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

/* ---------------------------------------------------------------- set-up */
static int has_ext(const char *e) { const char *x = (const char *)glGetString(GL_EXTENSIONS); if (!x) return 0; size_t n = strlen(e); for (const char *p = x; (p = strstr(p, e)); p += n) if ((p == x || p[-1] == ' ') && (p[n] == ' ' || !p[n])) return 1; return 0; }
int gl_init(void *(*get)(const char *), int es) {
    gles = es;
    #define GLF(ret, name, args) name = (ret (GLCALL *) args)get(#name); if (!name) { char ext[64]; snprintf(ext, sizeof ext, "%sEXT", #name); name = (ret (GLCALL *) args)get(ext); }
    GL_FUNCS
    #undef GLF
    if (!glGetString || !glCreateShader || !glGenFramebuffers || !glDrawElements) { fprintf(stderr, "[gl] OpenGL 2.1 with framebuffer objects (or OpenGL ES 2.0) is required\n"); return -1; }
    fprintf(stderr, "[gl] %s / %s\n", (const char *)glGetString(GL_VERSION), (const char *)glGetString(GL_RENDERER));
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_tex);
    has_dxt = has_ext("GL_EXT_texture_compression_s3tc") && glCompressedTexImage2D;
    has_aniso = has_ext("GL_EXT_texture_filter_anisotropic") || has_ext("GL_ARB_texture_filter_anisotropic"); if (has_aniso) glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &max_aniso);
    has_border = !gles; has_maxlevel = !gles; packed_depth = !gles || has_ext("GL_OES_packed_depth_stencil");
    glGenBuffers(1, &vbo); glGenBuffers(1, &ibo);
    quad_prog = link(QVS, QFS); gamma_prog = link(QVS, GFS);
    glUseProgram(quad_prog); glUniform1i(glGetUniformLocation(quad_prog, "u_t0"), 0);
    glUseProgram(gamma_prog); glUniform1i(glGetUniformLocation(gamma_prog, "u_t0"), 0); glUniform1i(glGetUniformLocation(gamma_prog, "u_t1"), 1);
    target = &bb; return 0;
}
void gl_shutdown(void) {}
void gl_caps(PlatVideoCaps *c) {
    memset(c, 0, sizeof *c); c->max_texture_size = max_tex; c->dxt = has_dxt; c->npot = !gles || has_ext("GL_OES_texture_npot"); c->render_targets = 1; c->max_anisotropy = has_aniso ? (int)max_aniso : 1;
}
void gl_resize_backbuffer(int w, int h) {
    if (bb.tex && bb.w == w && bb.h == h) return;
    if (bb.fbo) { glDeleteFramebuffers(1, &bb.fbo); bb.fbo = 0; } if (bb.depth) { glDeleteRenderbuffers(1, &bb.depth); bb.depth = 0; } if (bb.tex) glDeleteTextures(1, &bb.tex);
    bb.w = w; bb.h = h; bb.levels = 1; bb.fmt = PLAT_TEX_RGBA8; bb.rt = 1; glGenTextures(1, &bb.tex); tex_storage(&bb);
    bind_target(&bb); target = &bb; cur_vp = (PlatViewport){ 0, 0, w, h, 0, 1 }; glViewport(0, 0, w, h); sc_on = 0; apply_scissor();
    glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
}
