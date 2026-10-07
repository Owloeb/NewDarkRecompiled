/* d3d9.c: Direct3D 9 front end.
 *
 * Implements the IDirect3D9 / IDirect3DDevice9 objects and resources the engine uses, keeps the whole device state
 * (render, texture stage and sampler states, transforms, lights, streams...), and turns every draw into the
 * backend's small interface (plat.h): vertices in clip space with colours, fog and texture coordinates already
 * computed, plus a PlatDrawState for the fixed-function pixel pipeline. Transforms, lighting, fog, texture coordinate
 * transforms, FVF and vertex declaration decoding, format conversion, state blocks and surface copies all happen here,
 * once, for every backend. The capabilities it reports describe exactly this pipeline: fixed function only (no
 * shaders), two texture stages, one vertex stream. */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "com.h"
#include "d3d9_format.h"

#define D3D_OK 0u
#define D3DERR_INVALIDCALL 0x8876086Cu
#define D3DERR_NOTAVAILABLE 0x8876086Au
#define D3DERR_NOTFOUND 0x88760866u
#define S_FALSE_ 1u

extern ComClass c_d3d9, c_dev, c_tex, c_cube, c_vol, c_surf, c_vb, c_ib, c_swap, c_query, c_sb, c_decl, c_vs, c_ps;
typedef struct { float m[16]; } Mat;
static const Mat IDENT = { { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 } };
static Mat mat_mul(const Mat *a, const Mat *b) { Mat r; for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { float s = 0; for (int k = 0; k < 4; k++) s += a->m[i * 4 + k] * b->m[k * 4 + j]; r.m[i * 4 + j] = s; } return r; }
static float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

/* ---------------------------------------------------------------- resources */
enum { R_TEX, R_CUBE, R_VOL, R_SURF, R_VB, R_IB };
typedef struct Device Device;
typedef struct Res {
    int kind; Device *dev; ComObj *obj;
    uint32_t fmt, pool, usage, w, h, levels, autogen;
    uint32_t mem[6][16];             /* guest memory of each face/level (textures), allocated when first touched */
    uint16_t dirty[6];               /* levels changed since the last upload */
    PlatTexture *pt; int pt_dxt, pt_rt; uint32_t pal_ver;
    ComObj *level_surf[6][16];       /* the surface objects GetSurfaceLevel hands out (kept until the texture dies) */
    /* surfaces */
    ComObj *container; int face, level; int backbuffer, depth; uint32_t smem; PlatTexture *srt; int lock_dirty;
    /* buffers */
    uint32_t size, data, fvf;
    uint32_t lod, priority;
} Res;
static Res *R(ComObj *o) { return o ? (Res *)o->data : NULL; }

/* ---------------------------------------------------------------- device state (what state blocks capture) */
typedef struct { uint32_t type; float diffuse[4], specular[4], ambient[4], pos[3], dir[3], range, falloff, att0, att1, att2, theta, phi; } Light;
#define MAX_LIGHTS 8
typedef struct {
    uint32_t rs[256], tss[8][33], ss[16][14];
    Mat world, view, proj, texm[8];   /* texm: texture coordinate transforms */
    uint32_t vp[6];                  /* X, Y, Width, Height, MinZ, MaxZ (float bits) */
    float material[17];              /* diffuse, ambient, specular, emissive (RGBA), power */
    Light light[MAX_LIGHTS]; uint8_t light_on[MAX_LIGHTS];
    uint32_t fvf; ComObj *decl, *vs, *ps;
    ComObj *vb; uint32_t vb_off, vb_stride;
    ComObj *ib; ComObj *tex[8];
    int32_t scissor[4];
    uint32_t palette;
} State;
/* which parts of State a state block holds */
typedef struct {
    uint8_t rs[256], tss[8][33], ss[16][14];
    uint8_t world, view, proj, tex[8], vp, material, light[MAX_LIGHTS], light_on[MAX_LIGHTS], fvf, decl, vs, ps, vb, ib, texture[8], scissor, palette;
} Mask;
typedef struct { Device *dev; State st; Mask m; } StateBlock;
struct Device {
    ComObj *obj, *d3d; uint32_t hwnd, behavior;
    uint32_t bb_w, bb_h, bb_fmt, depth_fmt; int windowed;
    State st;
    StateBlock *recording;
    ComObj *rt, *ds, *backbuf, *autodepth, *swap;
    uint8_t palettes[256][1024]; uint32_t pal_ver[256];
    PlatVertex *vbuf; int vcap; uint16_t *ibuf; int icap; uint8_t *scratch; size_t scratch_n;
    uint16_t gamma[3][256];
    int in_scene;
};
static Device *DEV(ComObj *o) { return (Device *)o->data; }
static void bind(ComObj **slot, ComObj *o) { if (*slot == o) return; if (o) com_addref(o); if (*slot) com_release(*slot); *slot = o; }
static void *scratch(Device *d, size_t n) { if (d->scratch_n < n) { d->scratch = realloc(d->scratch, n); d->scratch_n = n; } return d->scratch; }

/* ---------------------------------------------------------------- texture memory and uploads */
static uint32_t level_w(Res *t, int l) { uint32_t v = t->w >> l; return v ? v : 1; }
static uint32_t level_h(Res *t, int l) { uint32_t v = t->h >> l; return v ? v : 1; }
static uint32_t level_mem(Res *t, int face, int l) {
    if (!t->mem[face][l]) { uint32_t pitch, size; fmt_layout(t->fmt, level_w(t, l), level_h(t, l), &pitch, &size); t->mem[face][l] = g_alloc(size ? size : 4); }
    return t->mem[face][l];
}
static void upload(Res *t) {
    if (t->kind != R_TEX) return;                                    /* cube and volume textures are not drawn (see the README) */
    Device *d = t->dev;
    if (t->fmt == FMT_P8 && t->pal_ver != d->pal_ver[d->st.palette]) { t->dirty[0] = 0xFFFF; t->pal_ver = d->pal_ver[d->st.palette]; }
    if (!t->dirty[0] && t->pt) return;
    PlatVideoCaps caps; plat_video_caps(&caps);
    int levels = (int)t->levels;
    if (!t->pt) {
        t->pt_dxt = fmt_is_dxt(t->fmt) && caps.dxt && t->fmt != FMT_DXT2 && t->fmt != FMT_DXT4;
        t->pt_rt = (t->usage & 1) != 0;                              /* D3DUSAGE_RENDERTARGET */
        if (t->autogen && !t->pt_dxt) { levels = 1; uint32_t m = t->w > t->h ? t->w : t->h; while (m > 1) { levels++; m >>= 1; } }
        t->pt = plat_tex_create((int)t->w, (int)t->h, levels, t->pt_dxt ? (t->fmt == FMT_DXT1 ? PLAT_TEX_DXT1 : t->fmt == FMT_DXT3 ? PLAT_TEX_DXT3 : PLAT_TEX_DXT5) : PLAT_TEX_RGBA8, t->pt_rt);
        if (!t->pt) return;
        if (t->pt_rt) { t->dirty[0] = 0; return; }
        t->dirty[0] = 0xFFFF;
    }
    if (t->pt_rt) { t->dirty[0] = 0; return; }
    const uint8_t *pal = t->fmt == FMT_P8 ? d->palettes[d->st.palette] : NULL;
    int last = t->autogen && !t->pt_dxt ? 0 : (int)t->levels - 1;
    for (int l = 0; l <= last; l++) {
        if (!(t->dirty[0] & (1u << l)) || !t->mem[0][l]) continue;
        uint32_t w = level_w(t, l), h = level_h(t, l), pitch, size; fmt_layout(t->fmt, w, h, &pitch, &size);
        if (t->pt_dxt) { plat_tex_upload(t->pt, l, 0, 0, (int)w, (int)h, GP(t->mem[0][l]), (int)pitch); continue; }
        uint8_t *rgba = scratch(d, (size_t)w * h * 4 * 2);
        fmt_to_rgba8(t->fmt, GP(t->mem[0][l]), pitch, w, h, rgba, pal);
        plat_tex_upload(t->pt, l, 0, 0, (int)w, (int)h, rgba, (int)w * 4);
        if (t->autogen && !t->pt_dxt) {                              /* D3DUSAGE_AUTOGENMIPMAP: box-filter the chain */
            uint8_t *a = rgba, *b = rgba + (size_t)w * h * 4; uint32_t cw = w, ch = h;
            for (int k = 1; cw > 1 || ch > 1; k++) { rgba8_half(a, cw, ch, b); cw = cw > 1 ? cw / 2 : 1; ch = ch > 1 ? ch / 2 : 1; plat_tex_upload(t->pt, k, 0, 0, (int)cw, (int)ch, b, (int)cw * 4); uint8_t *s = a; a = b; b = s; }
        }
    }
    t->dirty[0] = 0;
}

/* ---------------------------------------------------------------- state defaults */
static void default_state(State *s, int zenable) {
    memset(s, 0, sizeof *s);
    uint32_t *rs = s->rs;
    rs[7] = (uint32_t)zenable; rs[8] = 3; rs[9] = 2; rs[14] = 1; rs[16] = 1; rs[19] = 2; rs[20] = 1; rs[22] = 3; rs[23] = 4; rs[25] = 8; rs[34] = 0;
    rs[36] = f2u(0.0f); rs[37] = f2u(1.0f); rs[38] = f2u(1.0f); rs[52] = 0; rs[53] = 1; rs[54] = 1; rs[55] = 1; rs[56] = 8; rs[57] = 0; rs[58] = 0xFFFFFFFFu; rs[59] = 0xFFFFFFFFu;
    rs[60] = 0xFFFFFFFFu; rs[136] = 1; rs[137] = 1; rs[141] = 1; rs[142] = 1; rs[145] = 1; rs[146] = 2; rs[154] = f2u(1.0f); rs[155] = f2u(1.0f); rs[158] = f2u(1.0f);
    rs[159] = f2u(0.0f); rs[160] = f2u(0.0f); rs[161] = 1; rs[162] = 0xFFFFFFFFu; rs[166] = f2u(64.0f); rs[168] = 0xF; rs[170] = 0; rs[171] = 1; rs[190] = 0xF; rs[191] = 0xF; rs[192] = 0xF;
    rs[193] = 0xFFFFFFFFu; rs[207] = 2; rs[208] = 1; rs[209] = 1;
    for (int i = 0; i < 8; i++) {
        uint32_t *t = s->tss[i]; t[1] = i ? 1 : 4; t[2] = 2; t[3] = 1; t[4] = i ? 1 : 2; t[5] = 2; t[6] = 1; t[11] = (uint32_t)i; t[26] = 1; t[27] = 1; t[28] = 1;
        s->texm[i] = IDENT;
    }
    for (int i = 0; i < 16; i++) { uint32_t *t = s->ss[i]; t[1] = t[2] = t[3] = 1; t[5] = t[6] = 1; t[7] = 0; t[10] = 1; }
    s->world = s->view = s->proj = IDENT;
    s->material[0] = s->material[1] = s->material[2] = s->material[3] = 1.0f;
    for (int i = 0; i < MAX_LIGHTS; i++) { s->light[i].type = 3; s->light[i].diffuse[0] = s->light[i].diffuse[1] = s->light[i].diffuse[2] = 1.0f; s->light[i].dir[2] = 1.0f; }
    s->vp[4] = f2u(0.0f); s->vp[5] = f2u(1.0f);
}
static void apply_viewport(Device *d) {
    PlatViewport v = { (int)d->st.vp[0], (int)d->st.vp[1], (int)d->st.vp[2], (int)d->st.vp[3], u2f(d->st.vp[4]), u2f(d->st.vp[5]) };
    plat_gfx_viewport(&v);
    plat_gfx_scissor((int)d->st.rs[174], d->st.scissor[0], d->st.scissor[1], d->st.scissor[2] - d->st.scissor[0], d->st.scissor[3] - d->st.scissor[1]);
}
static void target_size(Device *d, uint32_t *w, uint32_t *h) {
    Res *r = R(d->rt); if (!r || r->backbuffer) { *w = d->bb_w; *h = d->bb_h; return; }
    if (r->container) { Res *t = R(r->container); *w = level_w(t, r->level); *h = level_h(t, r->level); } else { *w = r->w; *h = r->h; }
}
static PlatTexture *surface_target(Res *s) {          /* the backend texture behind a render-target surface (NULL: back buffer) */
    if (!s || s->backbuffer) return NULL;
    if (s->container) { Res *t = R(s->container); if (!t->pt) { t->usage |= 1; upload(t); } return t->pt; }
    if (!s->srt) s->srt = plat_tex_create((int)s->w, (int)s->h, 1, PLAT_TEX_RGBA8, 1);
    return s->srt;
}

/* ---------------------------------------------------------------- the fixed-function vertex pipeline */
typedef struct { int stride, pos, pos_n, rhw, normal, diffuse, specular, psize, ntex, tex[8], tex_n[8]; } Layout;
static int layout_fvf(uint32_t fvf, Layout *L) {
    memset(L, 0, sizeof *L); L->normal = L->diffuse = L->specular = L->psize = -1; for (int i = 0; i < 8; i++) L->tex[i] = -1;
    int off = 0, p = (int)(fvf & 0x400E);
    switch (p) {
    case 0x002: L->pos_n = 3; off = 12; break;
    case 0x004: L->pos_n = 4; L->rhw = 1; off = 16; break;
    case 0x4002: L->pos_n = 4; off = 16; break;
    case 0x006: case 0x008: case 0x00A: case 0x00C: case 0x00E: L->pos_n = 3; off = 12 + 4 * ((p - 4) / 2); break;     /* XYZB1..5: blend weights ignored */
    default: return 0;
    }
    if (fvf & 0x10) { L->normal = off; off += 12; }
    if (fvf & 0x20) { L->psize = off; off += 4; }
    if (fvf & 0x40) { L->diffuse = off; off += 4; }
    if (fvf & 0x80) { L->specular = off; off += 4; }
    L->ntex = (int)((fvf >> 8) & 15); if (L->ntex > 8) L->ntex = 8;
    for (int i = 0; i < L->ntex; i++) { static const int n[4] = { 2, 3, 4, 1 }; L->tex[i] = off; L->tex_n[i] = n[(fvf >> (16 + 2 * i)) & 3]; off += 4 * L->tex_n[i]; }
    L->stride = off; return 1;
}
typedef struct { uint32_t n; uint8_t *elems; } Decl;
static int layout_decl(Decl *dc, Layout *L) {
    memset(L, 0, sizeof *L); L->normal = L->diffuse = L->specular = L->psize = -1; for (int i = 0; i < 8; i++) L->tex[i] = -1;
    int ok = 0;
    for (uint32_t i = 0; i < dc->n; i++) {
        const uint8_t *e = dc->elems + 8 * i; int stream = e[0] | e[1] << 8, off = e[2] | e[3] << 8, type = e[4], usage = e[6], idx = e[7];
        if (stream != 0) continue;
        static const int nf[] = { 1, 2, 3, 4, 0, 0, 0, 0 };
        static const int tsz[18] = { 4, 8, 12, 16, 4, 4, 4, 8, 4, 4, 8, 4, 8, 4, 4, 4, 8, 0 };   /* D3DDECLTYPE sizes */
        if (type < 18 && off + tsz[type] > L->stride) L->stride = off + tsz[type];                 /* bytes a vertex spans (for bounds checks) */
        switch (usage) {
        case 0: L->pos = off; L->pos_n = type <= 3 ? nf[type] : 3; ok = 1; break;
        case 9: L->pos = off; L->pos_n = 4; L->rhw = 1; ok = 1; break;
        case 3: L->normal = off; break;
        case 4: L->psize = off; break;
        case 10: if (type == 4) { if (idx == 0) L->diffuse = off; else if (idx == 1) L->specular = off; } break;
        case 5: if (idx < 8 && type <= 3) { L->tex[idx] = off; L->tex_n[idx] = nf[type]; if (L->ntex < idx + 1) L->ntex = idx + 1; } break;
        default: break;
        }
    }
    return ok;
}
static uint32_t pack(float r, float g, float b, float a) {
    #define CL(v) ((uint32_t)((v) <= 0 ? 0 : (v) >= 1 ? 255 : (int)((v) * 255.0f + 0.5f)))
    return CL(a) << 24 | CL(r) << 16 | CL(g) << 8 | CL(b);
    #undef CL
}
static void unpack(uint32_t c, float o[4]) { o[0] = ((c >> 16) & 255) / 255.0f; o[1] = ((c >> 8) & 255) / 255.0f; o[2] = (c & 255) / 255.0f; o[3] = (c >> 24) / 255.0f; }
static void xform3(const Mat *m, const float *v, float w, float *o) { for (int j = 0; j < 4; j++) o[j] = v[0] * m->m[j] + v[1] * m->m[4 + j] + v[2] * m->m[8 + j] + w * m->m[12 + j]; }
static float fog_factor(uint32_t mode, float d, const State *s) {
    float start = u2f(s->rs[36]), end = u2f(s->rs[37]), dens = u2f(s->rs[38]), f;
    switch (mode) { case 1: f = expf(-d * dens); break; case 2: f = expf(-(d * dens) * (d * dens)); break; case 3: f = end != start ? (end - d) / (end - start) : 1.0f; break; default: f = 1.0f; }
    return f < 0 ? 0 : f > 1 ? 1 : f;
}
typedef struct { Mat wv, wvp; int lighting, nlights; const Light *lights[MAX_LIGHTS]; Light vlight[MAX_LIGHTS]; float half_px[2]; } Xf;
static void prepare(Device *d, const Layout *L, Xf *x) {
    State *s = &d->st;
    x->wv = mat_mul(&s->world, &s->view); x->wvp = mat_mul(&x->wv, &s->proj);
    x->lighting = s->rs[137] && !L->rhw && L->normal >= 0;
    x->nlights = 0;
    if (x->lighting) for (int i = 0; i < MAX_LIGHTS; i++) if (s->light_on[i]) {     /* lights into view space */
        Light *l = &x->vlight[x->nlights]; *l = s->light[i]; float t[4];
        xform3(&s->view, s->light[i].pos, 1, t); memcpy(l->pos, t, 12);
        float dv[3] = { s->light[i].dir[0], s->light[i].dir[1], s->light[i].dir[2] }; xform3(&s->view, dv, 0, t);
        float n = sqrtf(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]); if (n > 0) { t[0] /= n; t[1] /= n; t[2] /= n; } memcpy(l->dir, t, 12);
        x->lights[x->nlights++] = l;
    }
    x->half_px[0] = s->vp[2] ? 1.0f / (float)s->vp[2] : 0; x->half_px[1] = s->vp[3] ? 1.0f / (float)s->vp[3] : 0;
}
static const float *mat_color(const State *s, uint32_t src, const float *mat, const float *c1, const float *c2) {
    if (!s->rs[141]) return mat;                       /* COLORVERTEX off */
    return src == 1 && c1 ? c1 : src == 2 && c2 ? c2 : mat;
}
static void vertex(Device *d, const Layout *L, const Xf *x, const uint8_t *v, PlatVertex *o) {
    State *s = &d->st; float p[4];
    memcpy(p, v + L->pos, 4 * (size_t)(L->pos_n < 4 ? L->pos_n : 4)); if (L->pos_n < 3) p[2] = 0; if (L->pos_n < 2) p[1] = 0;
    float c1[4] = { 1, 1, 1, 1 }, c2[4] = { 0, 0, 0, 0 }; uint32_t vd = 0xFFFFFFFFu, vs = 0;
    if (L->diffuse >= 0) { memcpy(&vd, v + L->diffuse, 4); unpack(vd, c1); }
    if (L->specular >= 0) { memcpy(&vs, v + L->specular, 4); unpack(vs, c2); }
    float eye_z = 0;
    if (L->rhw) {
        float vx = (float)s->vp[0], vy = (float)s->vp[1], vw = s->vp[2] ? (float)s->vp[2] : 1, vh = s->vp[3] ? (float)s->vp[3] : 1, mnz = u2f(s->vp[4]), mxz = u2f(s->vp[5]);
        float w = p[3] != 0 ? 1.0f / p[3] : 1.0f; if (!isfinite(w) || w <= 0) w = 1.0f;
        float nx = ((p[0] + 0.5f - vx) / vw) * 2 - 1, ny = 1 - ((p[1] + 0.5f - vy) / vh) * 2, nz = mxz != mnz ? (p[2] - mnz) / (mxz - mnz) : p[2];
        o->x = nx * w; o->y = ny * w; o->z = nz * w; o->w = w; eye_z = w;
        o->diffuse = vd; o->specular = vs;
    } else {
        float cl[4]; xform3(&x->wvp, p, 1, cl);
        o->x = cl[0] + cl[3] * x->half_px[0]; o->y = cl[1] - cl[3] * x->half_px[1]; o->z = cl[2]; o->w = cl[3];
        float e[4]; xform3(&x->wv, p, 1, e); eye_z = e[2];
        if (x->lighting) {
            float n[3], t[4]; memcpy(n, v + L->normal, 12); xform3(&x->wv, n, 0, t); float ln = sqrtf(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]); if (ln > 0) { t[0] /= ln; t[1] /= ln; t[2] /= ln; }
            float amb[3] = { 0, 0, 0 }, dif[3] = { 0, 0, 0 }, spe[3] = { 0, 0, 0 };
            float ga[4]; unpack(s->rs[139], ga);
            const float *md = mat_color(s, s->rs[145], &s->material[0], L->diffuse >= 0 ? c1 : NULL, L->specular >= 0 ? c2 : NULL);
            const float *ma = mat_color(s, s->rs[147], &s->material[4], L->diffuse >= 0 ? c1 : NULL, L->specular >= 0 ? c2 : NULL);
            const float *ms = mat_color(s, s->rs[146], &s->material[8], L->diffuse >= 0 ? c1 : NULL, L->specular >= 0 ? c2 : NULL);
            const float *me = mat_color(s, s->rs[148], &s->material[12], L->diffuse >= 0 ? c1 : NULL, L->specular >= 0 ? c2 : NULL);
            float power = s->material[16];
            for (int i = 0; i < x->nlights; i++) {
                const Light *l = x->lights[i]; float Lv[3], att = 1, spot = 1;
                if (l->type == 3) { Lv[0] = -l->dir[0]; Lv[1] = -l->dir[1]; Lv[2] = -l->dir[2]; }
                else {
                    Lv[0] = l->pos[0] - e[0]; Lv[1] = l->pos[1] - e[1]; Lv[2] = l->pos[2] - e[2];
                    float dist = sqrtf(Lv[0] * Lv[0] + Lv[1] * Lv[1] + Lv[2] * Lv[2]); if (l->range > 0 && dist > l->range) continue;
                    if (dist > 0) { Lv[0] /= dist; Lv[1] /= dist; Lv[2] /= dist; }
                    float den = l->att0 + l->att1 * dist + l->att2 * dist * dist; att = den > 0 ? 1.0f / den : 1.0f;
                    if (l->type == 2) {
                        float rho = -(Lv[0] * l->dir[0] + Lv[1] * l->dir[1] + Lv[2] * l->dir[2]), ct = cosf(l->theta / 2), cp = cosf(l->phi / 2);
                        spot = rho > ct ? 1.0f : rho <= cp ? 0.0f : powf((rho - cp) / (ct - cp > 1e-6f ? ct - cp : 1e-6f), l->falloff);
                    }
                }
                float k = att * spot, nd = t[0] * Lv[0] + t[1] * Lv[1] + t[2] * Lv[2];
                for (int ch = 0; ch < 3; ch++) amb[ch] += l->ambient[ch] * k;
                if (nd > 0) {
                    for (int ch = 0; ch < 3; ch++) dif[ch] += l->diffuse[ch] * nd * k;
                    if (s->rs[29]) {
                        float h[3]; if (s->rs[142]) { float el = sqrtf(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]); for (int ch = 0; ch < 3; ch++) h[ch] = Lv[ch] - (el > 0 ? e[ch] / el : 0); } else { h[0] = Lv[0]; h[1] = Lv[1]; h[2] = Lv[2] - 1; }
                        float hl = sqrtf(h[0] * h[0] + h[1] * h[1] + h[2] * h[2]); float nh = hl > 0 ? (t[0] * h[0] + t[1] * h[1] + t[2] * h[2]) / hl : 0;
                        if (nh > 0) { float sp = powf(nh, power) * k; for (int ch = 0; ch < 3; ch++) spe[ch] += l->specular[ch] * sp; }
                    }
                }
            }
            float r[4]; for (int ch = 0; ch < 3; ch++) r[ch] = me[ch] + ma[ch] * (ga[ch] + amb[ch]) + md[ch] * dif[ch]; r[3] = md[3];
            o->diffuse = pack(r[0], r[1], r[2], r[3]);
            o->specular = s->rs[29] ? pack(ms[0] * spe[0], ms[1] * spe[1], ms[2] * spe[2], c2[3]) : (vs & 0xFF000000u);
        } else { o->diffuse = vd; o->specular = vs; }
    }
    /* fog factor: table (per pixel, approximated per vertex) > vertex fog > the specular alpha the application supplied */
    o->fog = 1.0f;
    if (s->rs[28]) {
        if (s->rs[35]) o->fog = fog_factor(s->rs[35], L->rhw ? eye_z : fabsf(eye_z), s);
        else if (s->rs[140] && !L->rhw) { float dist = fabsf(eye_z); if (s->rs[48]) { float e[4]; xform3(&x->wv, p, 1, e); dist = sqrtf(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]); } o->fog = fog_factor(s->rs[140], dist, s); }
        else o->fog = (o->specular >> 24) / 255.0f;
    }
    /* texture coordinates for stages 0 and 1 */
    for (int st = 0; st < PLAT_MAX_STAGES; st++) {
        uint32_t tci = s->tss[st][11]; int set = (int)(tci & 7); float uv[4] = { 0, 0, 0, 1 };
        if ((tci & 0xFFFF0000u) == 0x10000u && L->normal >= 0 && !L->rhw) { float n[3]; memcpy(n, v + L->normal, 12); float t[4]; xform3(&x->wv, n, 0, t); memcpy(uv, t, 12); }   /* CAMERASPACENORMAL */
        else if ((tci & 0xFFFF0000u) == 0x20000u && !L->rhw) { float e[4]; xform3(&x->wv, p, 1, e); memcpy(uv, e, 12); }                                               /* CAMERASPACEPOSITION */
        else if (set < L->ntex && L->tex[set] >= 0) {
            /* vertex coordinates are padded the way Direct3D's fixed function does before the texture matrix:
               1D (u,1,0,0), 2D (u,v,1,0) - so 2D translation lives in _31/_32 - 3D (u,v,w,1) */
            int nt = L->tex_n[set]; memcpy(uv, v + L->tex[set], 4 * (size_t)nt);
            if (nt == 1) { uv[1] = 1; uv[2] = 0; uv[3] = 0; } else if (nt == 2) { uv[2] = 1; uv[3] = 0; }
        }
        uint32_t ttf = s->tss[st][24];
        if (ttf & 0xFF) {
            float t[4]; xform3(&s->texm[st], uv, uv[3], t);
            int cnt = (int)(ttf & 0xFF); if (ttf & 0x100) { float q = t[cnt - 1]; if (q != 0) { t[0] /= q; t[1] /= q; } }
            uv[0] = t[0]; uv[1] = t[1];
        }
        if (st == 0) { o->u0 = uv[0]; o->v0 = uv[1]; } else { o->u1 = uv[0]; o->v1 = uv[1]; }
    }
}

/* ---------------------------------------------------------------- pixel pipeline state */
static uint8_t blend_of(uint32_t b) {
    static const uint8_t m[16] = { PLAT_BLEND_ONE, PLAT_BLEND_ZERO, PLAT_BLEND_ONE, PLAT_BLEND_SRC_COLOR, PLAT_BLEND_INV_SRC_COLOR, PLAT_BLEND_SRC_ALPHA, PLAT_BLEND_INV_SRC_ALPHA,
                                   PLAT_BLEND_DST_ALPHA, PLAT_BLEND_INV_DST_ALPHA, PLAT_BLEND_DST_COLOR, PLAT_BLEND_INV_DST_COLOR, PLAT_BLEND_SRC_ALPHA_SAT, PLAT_BLEND_SRC_ALPHA, PLAT_BLEND_INV_SRC_ALPHA, PLAT_BLEND_ONE, PLAT_BLEND_ONE };
    return b < 16 ? m[b] : PLAT_BLEND_ONE;
}
static uint8_t cmp_of(uint32_t f) { return f >= 1 && f <= 8 ? (uint8_t)(f - 1) : PLAT_CMP_ALWAYS; }
static uint8_t top_of(uint32_t op) {
    switch (op) {
    case 1: return PLAT_TOP_DISABLE; case 2: return PLAT_TOP_SELECTARG1; case 3: return PLAT_TOP_SELECTARG2; case 4: return PLAT_TOP_MODULATE; case 5: return PLAT_TOP_MODULATE2X;
    case 6: return PLAT_TOP_MODULATE4X; case 7: return PLAT_TOP_ADD; case 8: case 9: return PLAT_TOP_ADDSIGNED; case 10: return PLAT_TOP_SUBTRACT; case 11: return PLAT_TOP_ADD;
    case 12: return PLAT_TOP_BLENDDIFFUSEALPHA; case 13: case 15: return PLAT_TOP_BLENDTEXTUREALPHA; case 16: return PLAT_TOP_BLENDCURRENTALPHA;
    default: { static int warned[32]; if (op < 32 && !warned[op]++) port_debug("Direct3D: texture operation %u is drawn as MODULATE", op); return PLAT_TOP_MODULATE; }
    }
}
static uint8_t arg_of(uint32_t a) { uint8_t r = (uint8_t)(a & 7); if (r > 4) r = PLAT_ARG_CURRENT; return (uint8_t)(r | (a & 0x10 ? PLAT_ARG_COMPLEMENT : 0) | (a & 0x20 ? PLAT_ARG_ALPHAREPLICATE : 0)); }
static uint8_t filt(uint32_t f) { return f >= 2 ? PLAT_FILTER_LINEAR : PLAT_FILTER_NEAREST; }
static uint8_t wrap(uint32_t a) { return a == 2 ? PLAT_WRAP_MIRROR : a == 3 ? PLAT_WRAP_CLAMP : a == 4 ? PLAT_WRAP_BORDER : a == 5 ? PLAT_WRAP_MIRROR : PLAT_WRAP_REPEAT; }
static void draw_state(Device *d, PlatDrawState *ps) {
    State *s = &d->st; memset(ps, 0, sizeof *ps);
    ps->blend = (uint8_t)(s->rs[27] != 0);
    ps->src_blend = s->rs[19] == 12 ? PLAT_BLEND_SRC_ALPHA : s->rs[19] == 13 ? PLAT_BLEND_INV_SRC_ALPHA : blend_of(s->rs[19]);
    ps->dst_blend = s->rs[19] == 12 ? PLAT_BLEND_INV_SRC_ALPHA : s->rs[19] == 13 ? PLAT_BLEND_SRC_ALPHA : blend_of(s->rs[20]);
    ps->alpha_test = (uint8_t)(s->rs[15] != 0); ps->alpha_func = cmp_of(s->rs[25]); ps->alpha_ref = (uint8_t)(s->rs[24] & 255);
    ps->depth_test = (uint8_t)(s->rs[7] != 0 && d->ds != NULL); ps->depth_write = (uint8_t)(s->rs[14] != 0 && ps->depth_test); ps->depth_func = cmp_of(s->rs[23]);
    ps->cull = s->rs[22] == 2 ? PLAT_CULL_CW : s->rs[22] == 3 ? PLAT_CULL_CCW : PLAT_CULL_NONE;
    ps->fog = (uint8_t)(s->rs[28] != 0); ps->fog_color = s->rs[34];
    ps->specular = (uint8_t)(s->rs[29] != 0); ps->flat_shading = (uint8_t)(s->rs[9] == 1);
    ps->color_write = (uint8_t)(s->rs[168] & 15); ps->texture_factor = s->rs[60]; ps->depth_bias = u2f(s->rs[195]);
    for (int i = 0; i < PLAT_MAX_STAGES; i++) {
        PlatStage *g = &ps->stage[i]; const uint32_t *t = s->tss[i], *sm = s->ss[i];
        int disabled = i > 0 && ps->stage[i - 1].color_op == PLAT_TOP_DISABLE;
        g->color_op = disabled ? PLAT_TOP_DISABLE : top_of(t[1]); g->color_arg1 = arg_of(t[2]); g->color_arg2 = arg_of(t[3]);
        g->alpha_op = g->color_op == PLAT_TOP_DISABLE ? PLAT_TOP_DISABLE : top_of(t[4]); g->alpha_arg1 = arg_of(t[5]); g->alpha_arg2 = arg_of(t[6]);
        if (g->alpha_op == PLAT_TOP_DISABLE && g->color_op != PLAT_TOP_DISABLE) { g->alpha_op = PLAT_TOP_SELECTARG1; g->alpha_arg1 = PLAT_ARG_CURRENT; }
        g->texcoord = (uint8_t)i;
        g->mag_filter = filt(sm[5]); g->min_filter = filt(sm[6]); g->mip_filter = sm[7] == 1 ? PLAT_MIP_NEAREST : sm[7] >= 2 ? PLAT_MIP_LINEAR : PLAT_MIP_NONE;
        g->wrap_u = wrap(sm[1]); g->wrap_v = wrap(sm[2]); g->border_color = sm[4]; g->anisotropy = (uint8_t)(sm[6] == 3 || sm[5] == 3 ? (sm[10] > 16 ? 16 : sm[10]) : 1);
        Res *tx = R(s->tex[i]);
        if (tx && tx->kind == R_TEX && g->color_op != PLAT_TOP_DISABLE) { upload(tx); g->texture = tx->pt; if (tx->levels <= 1 && !tx->autogen) g->mip_filter = PLAT_MIP_NONE; }
    }
    if (s->tss[PLAT_MAX_STAGES][1] != 1 && s->tss[PLAT_MAX_STAGES - 1][1] != 1) { static int w; if (!w++) port_debug("Direct3D: more than %d texture stages enabled; the rest are ignored", PLAT_MAX_STAGES); }
}

/* ---------------------------------------------------------------- draws */
static int prim_counts(uint32_t type, uint32_t prims, uint32_t *nverts, int *plat) {
    switch (type) {
    case 1: *nverts = prims; *plat = PLAT_PRIM_POINTS; return 1;
    case 2: *nverts = prims * 2; *plat = PLAT_PRIM_LINES; return 1;
    case 3: *nverts = prims + 1; *plat = PLAT_PRIM_LINES; return 1;
    case 4: *nverts = prims * 3; *plat = PLAT_PRIM_TRIANGLES; return 1;
    case 5: case 6: *nverts = prims + 2; *plat = PLAT_PRIM_TRIANGLES; return 1;
    }
    return 0;
}
/* list of indices (into the vertex range) for prims of type, from the raw element sequence seq[0..n); 32-bit so that
   ranges wider than 65535 vertices stay correct (the narrow path converts afterwards) */
static int expand(uint32_t type, uint32_t prims, const uint32_t *seq, uint32_t *out) {
    int k = 0;
    switch (type) {
    case 1: case 2: case 4: { uint32_t n = type == 1 ? prims : type == 2 ? prims * 2 : prims * 3; for (uint32_t i = 0; i < n; i++) out[k++] = seq[i]; break; }
    case 3: for (uint32_t i = 0; i < prims; i++) { out[k++] = seq[i]; out[k++] = seq[i + 1]; } break;
    case 5: for (uint32_t i = 0; i < prims; i++) { if (i & 1) { out[k++] = seq[i + 1]; out[k++] = seq[i]; } else { out[k++] = seq[i]; out[k++] = seq[i + 1]; } out[k++] = seq[i + 2]; } break;
    case 6: for (uint32_t i = 0; i < prims; i++) { out[k++] = seq[0]; out[k++] = seq[i + 1]; out[k++] = seq[i + 2]; } break;
    }
    return k;
}
static int current_layout(Device *d, Layout *L) {
    if (d->st.vs || d->st.ps) { static int w; if (!w++) port_warn("Direct3D: a shader is set; shaders are not supported, the draw is skipped"); return 0; }
    if (d->st.decl) return layout_decl((Decl *)d->st.decl->data, L);
    return layout_fvf(d->st.fvf, L);
}
/* draws prims of type from vertices at guest address vbase (stride bytes apart); indices: NULL for sequential from 0,
 * else guest address of 16- or 32-bit indices, which are offset by base_vertex. min/num give the vertex range. */
static void draw(Device *d, uint32_t type, uint32_t prims, uint32_t vbase, uint32_t stride, uint32_t idx, int idx32, int32_t base_vertex, uint32_t min_index, uint32_t num_vertices) {
    uint32_t nseq; int plat_prim;
    if (!prims || !prim_counts(type, prims, &nseq, &plat_prim)) return;
    Layout L; if (!current_layout(d, &L)) return;
    if (!stride) stride = (uint32_t)L.stride;
    if (!d->in_scene) { static int w; if (!w++) port_debug("Direct3D: draw outside BeginScene/EndScene"); }
    Xf x; prepare(d, &L, &x);
    PlatDrawState ps; draw_state(d, &ps);
    uint32_t *seq = scratch(d, (size_t)nseq * 4);
    uint32_t lo = 0xFFFFFFFFu, hi = 0;
    for (uint32_t i = 0; i < nseq; i++) {
        uint32_t v = idx ? (idx32 ? RD32(idx + 4 * i) : RD16(idx + 2 * i)) : i;
        v = (uint32_t)((int32_t)v + (idx ? base_vertex : 0)); seq[i] = v; if (v < lo) lo = v; if (v > hi) hi = v;
    }
    if (idx && num_vertices && hi - lo + 1 > num_vertices + min_index) { /* trust the indices */ }
    uint32_t span = hi - lo + 1;
    size_t maxidx = (size_t)nseq * 3 + 6;
    if (d->icap < (int)maxidx) { d->icap = (int)maxidx; d->ibuf = realloc(d->ibuf, (size_t)d->icap * sizeof *d->ibuf); }
    if (span <= 65535) {
        if (d->vcap < (int)span) { d->vcap = (int)span; d->vbuf = realloc(d->vbuf, (size_t)d->vcap * sizeof *d->vbuf); }
        uint8_t *used = calloc(span, 1);
        for (uint32_t i = 0; i < nseq; i++) { uint32_t r = seq[i] - lo; if (!used[r]) { used[r] = 1; uint32_t a = vbase + (seq[i]) * stride; if (g_valid(a, (uint32_t)L.stride)) vertex(d, &L, &x, GP(a), &d->vbuf[r]); else memset(&d->vbuf[r], 0, sizeof d->vbuf[r]); } seq[i] = r; }
        free(used);
        uint32_t *e32 = malloc(maxidx * sizeof *e32); int n = expand(type, prims, seq, e32);
        for (int i = 0; i < n; i++) d->ibuf[i] = (uint16_t)e32[i];      /* < span <= 65535 */
        free(e32);
        plat_gfx_draw(plat_prim, d->vbuf, (int)span, d->ibuf, n, &ps);
    } else {                                                          /* rare: a range too wide for 16-bit indices, draw it unindexed */
        uint32_t per = plat_prim == PLAT_PRIM_TRIANGLES ? 3 : plat_prim == PLAT_PRIM_LINES ? 2 : 1;
        uint32_t *tmp = malloc(maxidx * sizeof *tmp); for (uint32_t i = 0; i < nseq; i++) seq[i] -= lo;
        int n = expand(type, prims, seq, tmp);
        if (d->vcap < 65535) { d->vcap = 65535; d->vbuf = realloc(d->vbuf, (size_t)d->vcap * sizeof *d->vbuf); }
        for (int start = 0; start < n; start += (int)(65535 / per * per)) {
            int cnt = n - start; if (cnt > (int)(65535 / per * per)) cnt = (int)(65535 / per * per);
            for (int i = 0; i < cnt; i++) {
                uint32_t a = vbase + (lo + tmp[start + i]) * stride;
                if (g_valid(a, (uint32_t)L.stride)) vertex(d, &L, &x, GP(a), &d->vbuf[i]); else memset(&d->vbuf[i], 0, sizeof d->vbuf[i]);
                d->ibuf[i] = (uint16_t)i;
            }
            plat_gfx_draw(plat_prim, d->vbuf, cnt, d->ibuf, cnt, &ps);
        }
        free(tmp);
    }
}

/* ---------------------------------------------------------------- surfaces: copies and fills */
static uint32_t surf_mem(Res *s, uint32_t *pitch) {          /* guest memory of a lockable surface */
    uint32_t w = s->w, h = s->h, size; if (s->container) { Res *t = R(s->container); w = level_w(t, s->level); h = level_h(t, s->level); }
    fmt_layout(s->fmt, w, h, pitch, &size);
    if (s->container) return level_mem(R(s->container), s->face, s->level);
    if (!s->smem) s->smem = g_alloc(size ? size : 4);
    return s->smem;
}
static void surf_size(Res *s, uint32_t *w, uint32_t *h) { if (s->container) { Res *t = R(s->container); *w = level_w(t, s->level); *h = level_h(t, s->level); } else { *w = s->w; *h = s->h; } }
static void surf_changed(Res *s) { if (s->container) R(s->container)->dirty[s->face] |= (uint16_t)(1u << s->level); else if (s->srt || s->backbuffer) s->lock_dirty = 1; }
static int is_target(Res *s) { return s->backbuffer || (s->usage & 1) || (s->container && (R(s->container)->usage & 1)) || s->srt; }
/* draws an RGBA8 image into rectangle dst of target t (NULL = back buffer), stretched */
static void blit_rgba(Device *d, const uint8_t *rgba, uint32_t w, uint32_t h, const int32_t *sr, PlatTexture *t, const int32_t *dr, int linear) {
    PlatTexture *tmp = plat_tex_create((int)w, (int)h, 1, PLAT_TEX_RGBA8, 0); if (!tmp) return;
    plat_tex_upload(tmp, 0, 0, 0, (int)w, (int)h, rgba, (int)w * 4);
    plat_gfx_set_target(t);
    uint32_t tw, th; if (t == NULL) { tw = d->bb_w; th = d->bb_h; } else { tw = dr[2]; th = dr[3]; }
    (void)tw; (void)th;
    PlatViewport vp = { dr[0], dr[1], dr[2] - dr[0], dr[3] - dr[1], 0, 1 }; plat_gfx_viewport(&vp); plat_gfx_scissor(0, 0, 0, 0, 0);
    float u0 = (float)sr[0] / (float)w, v0 = (float)sr[1] / (float)h, u1 = (float)sr[2] / (float)w, v1 = (float)sr[3] / (float)h;
    PlatVertex q[4] = { { -1, 1, 0, 1, 0xFFFFFFFFu, 0, 1, u0, v0, 0, 0 }, { 1, 1, 0, 1, 0xFFFFFFFFu, 0, 1, u1, v0, 0, 0 }, { -1, -1, 0, 1, 0xFFFFFFFFu, 0, 1, u0, v1, 0, 0 }, { 1, -1, 0, 1, 0xFFFFFFFFu, 0, 1, u1, v1, 0, 0 } };
    uint16_t ix[6] = { 0, 1, 2, 2, 1, 3 };
    PlatDrawState ps; memset(&ps, 0, sizeof ps); ps.color_write = 15; ps.depth_func = PLAT_CMP_ALWAYS; ps.alpha_func = PLAT_CMP_ALWAYS;
    ps.stage[0] = (PlatStage){ PLAT_TOP_SELECTARG1, PLAT_ARG_TEXTURE, PLAT_ARG_DIFFUSE, PLAT_TOP_SELECTARG1, PLAT_ARG_TEXTURE, PLAT_ARG_DIFFUSE, 0, (uint8_t)linear, (uint8_t)linear, PLAT_MIP_NONE, PLAT_WRAP_CLAMP, PLAT_WRAP_CLAMP, 1, 0, tmp };
    ps.stage[1].color_op = ps.stage[1].alpha_op = PLAT_TOP_DISABLE;
    plat_gfx_draw(PLAT_PRIM_TRIANGLES, q, 4, ix, 6, &ps);
    plat_tex_destroy(tmp);
    plat_gfx_set_target(surface_target(R(d->rt))); apply_viewport(d);
}
static void rect_or_full(uint32_t r, uint32_t w, uint32_t h, int32_t o[4]) {
    if (r) { o[0] = (int32_t)RD32(r); o[1] = (int32_t)RD32(r + 4); o[2] = (int32_t)RD32(r + 8); o[3] = (int32_t)RD32(r + 12); } else { o[0] = o[1] = 0; o[2] = (int32_t)w; o[3] = (int32_t)h; }
}
/* RGBA8 copy of a surface's contents (render targets are read back) */
static uint8_t *surf_rgba(Device *d, Res *s, uint32_t *w, uint32_t *h) {
    surf_size(s, w, h); uint8_t *rgba = malloc((size_t)*w * *h * 4);
    if (is_target(s) && !s->lock_dirty) { if (plat_tex_read(surface_target(s), 0, 0, (int)*w, (int)*h, rgba, (int)*w * 4)) memset(rgba, 0, (size_t)*w * *h * 4); return rgba; }
    uint32_t pitch, m = surf_mem(s, &pitch); fmt_to_rgba8(s->fmt, GP(m), pitch, *w, *h, rgba, s->fmt == FMT_P8 ? d->palettes[d->st.palette] : NULL); return rgba;
}

/* ================================================================ IDirect3D9 */
typedef struct { int dummy; } D3D9;
static const uint32_t modes[][2] = { { 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1152, 864 }, { 1280, 720 }, { 1280, 800 }, { 1280, 960 }, { 1280, 1024 }, { 1360, 768 }, { 1366, 768 },
                                     { 1440, 900 }, { 1600, 900 }, { 1600, 1200 }, { 1680, 1050 }, { 1920, 1080 }, { 1920, 1200 }, { 2560, 1440 }, { 3840, 2160 } };
static int mode_list(uint32_t out[32][2]) {
    int dw, dh, n = 0; plat_video_display_size(&dw, &dh); if (dw <= 0) { dw = 1920; dh = 1080; }
    for (unsigned i = 0; i < sizeof modes / sizeof *modes; i++) if (modes[i][0] <= (uint32_t)dw && modes[i][1] <= (uint32_t)dh && n < 31) { out[n][0] = modes[i][0]; out[n][1] = modes[i][1]; n++; }
    int have = 0; for (int i = 0; i < n; i++) have |= out[i][0] == (uint32_t)dw && out[i][1] == (uint32_t)dh;
    if (!have) { out[n][0] = (uint32_t)dw; out[n][1] = (uint32_t)dh; n++; }
    return n;
}
static int adapter_format_ok(uint32_t f) { return f == FMT_X8R8G8B8 || f == FMT_R5G6B5 || f == FMT_A8R8G8B8 || f == FMT_X1R5G5B5; }
static void put_mode(uint32_t p, uint32_t w, uint32_t h, uint32_t fmt) { WR32(p, w); WR32(p + 4, h); WR32(p + 8, 60); WR32(p + 12, fmt); }
static void fill_caps(uint32_t p) {
    PlatVideoCaps vc; plat_video_caps(&vc); if (vc.max_texture_size <= 0) vc.max_texture_size = 2048;
    memset(GP(p), 0, 304);
    static const uint32_t v[] = {
        /* 0 */ 1, 0, 0x20000, 0x60020000, 0x320, 0x8000000F, 1, 0x001BBEF0, 0x0002FEF2, 0x07732191, 0xFF, 0x1FFF, 0x1FFF, 0xFF, 0x00084208,
        /* 60 TextureCaps */ 0, 0x03030700, 0, 0, 0x3F, 0, 0x1F };
    for (unsigned i = 0; i < sizeof v / sizeof *v; i++) WR32(p + 4 * i, v[i]);
    WR32(p + 60, 0x1 | 0x4 | 0x400 | 0x4000 | (vc.npot ? 0 : 0x2));                                   /* PERSPECTIVE ALPHA PROJECTED MIPMAP (+POW2) */
    WR32(p + 88, (uint32_t)vc.max_texture_size); WR32(p + 92, (uint32_t)vc.max_texture_size); WR32(p + 96, 0); WR32(p + 100, 8192); WR32(p + 104, (uint32_t)vc.max_texture_size);
    WR32(p + 108, vc.max_anisotropy > 0 ? (uint32_t)vc.max_anisotropy : 1); WR32(p + 112, f2u(1e10f));
    WR32(p + 116, f2u(-1e8f)); WR32(p + 120, f2u(-1e8f)); WR32(p + 124, f2u(1e8f)); WR32(p + 128, f2u(1e8f)); WR32(p + 132, f2u(0));
    WR32(p + 136, 0x1FF); WR32(p + 140, 0x80008); WR32(p + 144, 0x0001FFFF & ~0x00006000u);   /* TextureOpCaps */
    WR32(p + 148, PLAT_MAX_STAGES); WR32(p + 152, PLAT_MAX_STAGES); WR32(p + 156, 0x17B); WR32(p + 160, MAX_LIGHTS); WR32(p + 164, 0); WR32(p + 168, 0); WR32(p + 172, 0);
    WR32(p + 176, f2u(64.0f)); WR32(p + 180, 0xFFFFF); WR32(p + 184, 0xFFFFF); WR32(p + 188, 1); WR32(p + 192, 255);
    WR32(p + 196, 0xFFFE0000u); WR32(p + 200, 0); WR32(p + 204, 0xFFFF0000u); WR32(p + 208, f2u(0)); WR32(p + 212, 0x11); WR32(p + 216, f2u(0));
    WR32(p + 224, 0); WR32(p + 228, 0); WR32(p + 232, 1); WR32(p + 236, 0x3); WR32(p + 240, 1); WR32(p + 244, 0x03000300);
}
static void d3d_adapter_id(CPU *c, ComObj *s) {
    (void)s; uint32_t p = A(3); memset(GP(p), 0, 1104);
    strcpy((char *)GP(p), "portable.dll"); strcpy((char *)GP(p + 512), "darkrecomp portable renderer"); strcpy((char *)GP(p + 1024), "\\\\.\\DISPLAY1");
    WR32(p + 1056, 0x00010000); WR32(p + 1060, 0x00060000); WR32(p + 1064, 0x10DE); WR32(p + 1068, 0x0391); WR32(p + 1092, 1);   /* looks like a GeForce: engines key quirks off vendors */
}
static void d3d_modecount(CPU *c, ComObj *s) { (void)s; uint32_t m[32][2]; RET(adapter_format_ok(A(2)) ? (uint32_t)mode_list(m) : 0); }
static void d3d_enummodes(CPU *c, ComObj *s) { (void)s; uint32_t m[32][2]; int n = mode_list(m); if (!adapter_format_ok(A(2)) || A(3) >= (uint32_t)n) { RET(D3DERR_INVALIDCALL); return; } put_mode(A(4), m[A(3)][0], m[A(3)][1], A(2)); RET(0); }
static void d3d_dispmode(CPU *c, ComObj *s) { (void)s; int w, h; plat_video_display_size(&w, &h); put_mode(A(2), (uint32_t)w, (uint32_t)h, FMT_X8R8G8B8); RET(0); }
static void d3d_count(CPU *c, ComObj *s) { (void)s; RET(1); }
static void d3d_checktype(CPU *c, ComObj *s) { (void)s; RET(A(1) == 0 && adapter_format_ok(A(3)) ? 0 : D3DERR_NOTAVAILABLE); }
static void d3d_checkformat(CPU *c, ComObj *s) {
    (void)s; uint32_t usage = A(4), rtype = A(5), f = A(6); int ok;
    if (usage & 2) ok = fmt_is_depth(f) && f != FMT_D32F_LOCKABLE;                                     /* DEPTHSTENCIL */
    else if (usage & 1) ok = f == FMT_A8R8G8B8 || f == FMT_X8R8G8B8 || f == FMT_R5G6B5 || f == FMT_X1R5G5B5;   /* RENDERTARGET */
    else ok = fmt_known(f) && !fmt_is_depth(f) && f != FMT_P8 && f != FMT_INDEX16 && f != FMT_INDEX32;
    if (rtype == 5 || rtype == 4) ok = 0;                                                             /* cube and volume textures are not offered */
    RET(ok ? 0 : D3DERR_NOTAVAILABLE);
}
static void d3d_msaa(CPU *c, ComObj *s) { (void)s; if (A(5) > 0) { RET(D3DERR_NOTAVAILABLE); return; } OUTP(6, 1); RET(0); }
static void d3d_depthmatch(CPU *c, ComObj *s) { (void)s; RET(0); }
static void d3d_getcaps(CPU *c, ComObj *s) { (void)s; fill_caps(A(3)); RET(0); }
static void d3d_monitor(CPU *c, ComObj *s) { (void)s; RET(0x50001); }
static ComObj *new_surface(Device *d, uint32_t w, uint32_t h, uint32_t fmt, uint32_t usage, uint32_t pool);
static void dev_present_params(Device *d, uint32_t pp, int reset) {
    uint32_t w = RD32(pp), h = RD32(pp + 4), fmt = RD32(pp + 8);
    if (!w || !h) { int ww, wh; plat_video_window_size(&ww, &wh); w = ww > 0 ? (uint32_t)ww : 640; h = wh > 0 ? (uint32_t)wh : 480; WR32(pp, w); WR32(pp + 4, h); }
    if (!fmt) { fmt = FMT_X8R8G8B8; WR32(pp + 8, fmt); }
    d->bb_w = w; d->bb_h = h; d->bb_fmt = fmt; d->windowed = RD32(pp + 32) != 0;
    if (plat_video_open((int)w, (int)h, !d->windowed && !g_cfg.windowed, "System Shock 2")) port_warn("Direct3D: the platform could not set up a %ux%u picture", w, h);
    if (reset || !d->backbuf) { if (d->backbuf) com_release(d->backbuf); d->backbuf = new_surface(d, w, h, fmt, 1, 0); R(d->backbuf)->backbuffer = 1; }
    if (d->autodepth) { com_release(d->autodepth); d->autodepth = NULL; }
    if (RD32(pp + 36)) { d->depth_fmt = RD32(pp + 40); d->autodepth = new_surface(d, w, h, d->depth_fmt, 2, 0); R(d->autodepth)->depth = 1; }
    bind(&d->rt, d->backbuf); bind(&d->ds, d->autodepth);
    d->st.vp[0] = d->st.vp[1] = 0; d->st.vp[2] = w; d->st.vp[3] = h; d->st.vp[4] = f2u(0); d->st.vp[5] = f2u(1);
    d->st.scissor[0] = d->st.scissor[1] = 0; d->st.scissor[2] = (int32_t)w; d->st.scissor[3] = (int32_t)h;
    plat_gfx_begin_frame(); plat_gfx_set_target(NULL); apply_viewport(d);
}
static void d3d_createdevice(CPU *c, ComObj *s) {
    uint32_t pp = A(5); if (!pp) { RET(D3DERR_INVALIDCALL); return; }
    Device *d = calloc(1, sizeof *d); d->d3d = s; com_addref(s); d->hwnd = A(3) ? RD32(pp + 28) ? RD32(pp + 28) : A(3) : RD32(pp + 28); d->behavior = A(4);
    for (int i = 0; i < 256; i++) for (int k = 0; k < 256; k++) { uint8_t *e = d->palettes[i] + 4 * k; e[0] = e[1] = e[2] = (uint8_t)k; e[3] = 255; }
    for (int ch = 0; ch < 3; ch++) for (int i = 0; i < 256; i++) d->gamma[ch][i] = (uint16_t)(i * 257);
    default_state(&d->st, RD32(pp + 36) != 0);
    d->obj = com_create(&c_dev, d);
    dev_present_params(d, pp, 0);
    port_log("Direct3D 9: device %ux%u %s, back buffer format %u%s", d->bb_w, d->bb_h, d->windowed ? "windowed" : "fullscreen", d->bb_fmt, d->autodepth ? ", depth buffer" : "");
    OUTP(6, d->obj->guest); RET(0);
}
static void d3d_destroy(ComObj *o) { free(o->data); }
static const ComMethod d3d_m[] = {
    { "GetAdapterCount", d3d_count }, { "GetAdapterIdentifier", d3d_adapter_id }, { "GetAdapterModeCount", d3d_modecount }, { "EnumAdapterModes", d3d_enummodes },
    { "GetAdapterDisplayMode", d3d_dispmode }, { "CheckDeviceType", d3d_checktype }, { "CheckDeviceFormat", d3d_checkformat }, { "CheckDeviceMultiSampleType", d3d_msaa },
    { "CheckDepthStencilMatch", d3d_depthmatch }, { "CheckDeviceFormatConversion", d3d_depthmatch }, { "GetDeviceCaps", d3d_getcaps }, { "GetAdapterMonitor", d3d_monitor },
    { "CreateDevice", d3d_createdevice }, { 0, 0 } };
static const uint8_t iids_d3d9[][16] = { IID_IDirect3D9 };
ComClass c_d3d9 = { "IDirect3D9", IFACE_IDirect3D9, iids_d3d9, 1, d3d_m, D3D_OK, d3d_destroy };

/* ================================================================ resources: common */
static void res_destroy(ComObj *o) {
    Res *r = R(o);
    for (int f = 0; f < 6; f++) for (int l = 0; l < 16; l++) { if (r->mem[f][l]) g_free(r->mem[f][l]); if (r->level_surf[f][l]) { R(r->level_surf[f][l])->container = NULL; com_release(r->level_surf[f][l]); } }
    if (r->pt) plat_tex_destroy(r->pt); if (r->srt) plat_tex_destroy(r->srt);
    if (r->smem) g_free(r->smem); if (r->data) g_free(r->data);
    free(r);
}
static Res *new_res(Device *d, int kind) { Res *r = calloc(1, sizeof *r); r->kind = kind; r->dev = d; return r; }
static void res_getdevice(CPU *c, ComObj *s) { Device *d = R(s)->dev; com_addref(d->obj); OUTP(1, d->obj->guest); RET(0); }
static void res_setpriority(CPU *c, ComObj *s) { uint32_t o = R(s)->priority; R(s)->priority = A(1); RET(o); }
static void res_getpriority(CPU *c, ComObj *s) { RET(R(s)->priority); }
static void res_gettype(CPU *c, ComObj *s) { static const uint32_t t[] = { 3, 5, 4, 1, 6, 7 }; RET(t[R(s)->kind]); }
static void res_preload(CPU *c, ComObj *s) { (void)c; Res *r = R(s); if (r->kind == R_TEX) upload(r); }
static void res_privnotfound(CPU *c, ComObj *s) { (void)s; RET(D3DERR_NOTFOUND); }

/* ================================================================ textures */
static void tex_setlod(CPU *c, ComObj *s) { uint32_t o = R(s)->lod; R(s)->lod = A(1); RET(o); }
static void tex_getlod(CPU *c, ComObj *s) { RET(R(s)->lod); }
static void tex_levels(CPU *c, ComObj *s) { RET(R(s)->autogen ? 1 : R(s)->levels); }
static void tex_genmips(CPU *c, ComObj *s) { (void)c; R(s)->dirty[0] |= 1; }
static void tex_autofilter(CPU *c, ComObj *s) { (void)s; RET(2); }
static void put_desc(uint32_t p, Res *t, uint32_t w, uint32_t h) { memset(GP(p), 0, 32); WR32(p, t->fmt); WR32(p + 4, 1); WR32(p + 8, t->usage); WR32(p + 12, t->pool); WR32(p + 24, w); WR32(p + 28, h); }
static int lvl_ok(Res *t, uint32_t l) { return l < t->levels && l < 16; }
static void tex_leveldesc(CPU *c, ComObj *s) { Res *t = R(s); uint32_t l = A(1); if (!lvl_ok(t, l)) { RET(D3DERR_INVALIDCALL); return; } put_desc(A(2), t, level_w(t, (int)l), level_h(t, (int)l)); RET(0); }
static ComObj *level_surface(Res *t, ComObj *tobj, int face, int l) {
    if (!t->level_surf[face][l]) {
        Res *sr = new_res(t->dev, R_SURF); sr->fmt = t->fmt; sr->usage = t->usage; sr->pool = t->pool; sr->w = level_w(t, l); sr->h = level_h(t, l);
        sr->container = tobj; sr->face = face; sr->level = l;      /* not a counted reference: the texture owns its level surfaces */
        t->level_surf[face][l] = com_create(&c_surf, sr);
    }
    return t->level_surf[face][l];
}
static void tex_getsurf(CPU *c, ComObj *s) { Res *t = R(s); uint32_t l = A(1); if (!lvl_ok(t, l)) { RET(D3DERR_INVALIDCALL); return; } ComObj *o = level_surface(t, s, 0, (int)l); com_addref(o); OUTP(2, o->guest); RET(0); }
static void lock_out(uint32_t out, Res *t, int face, int l, uint32_t rect) {
    uint32_t w = level_w(t, l), h = level_h(t, l), pitch, size; fmt_layout(t->fmt, w, h, &pitch, &size); (void)size;
    uint32_t m = level_mem(t, face, l), off = 0;
    if (rect) { uint32_t x = RD32(rect), y = RD32(rect + 4); off = fmt_is_dxt(t->fmt) ? (y / 4) * pitch + (x / 4) * (t->fmt == FMT_DXT1 ? 8 : 16) : y * pitch + x * (uint32_t)fmt_bpp(t->fmt); }
    WR32(out, pitch); WR32(out + 4, m + off);
}
static void tex_lock(CPU *c, ComObj *s) {
    Res *t = R(s); uint32_t l = A(1); if (!lvl_ok(t, l)) { RET(D3DERR_INVALIDCALL); return; }
    if (t->pt_rt) { /* reading a render-target texture back */ uint32_t w = level_w(t, (int)l), h = level_h(t, (int)l), pitch, size; fmt_layout(t->fmt, w, h, &pitch, &size); uint8_t *rgba = malloc((size_t)w * h * 4); if (!plat_tex_read(t->pt, 0, 0, (int)w, (int)h, rgba, (int)w * 4)) fmt_from_rgba8(t->fmt, rgba, w, h, GP(level_mem(t, 0, (int)l)), pitch); free(rgba); }
    lock_out(A(2), t, 0, (int)l, A(3)); RET(0);
}
static void tex_unlock(CPU *c, ComObj *s) { Res *t = R(s); uint32_t l = A(1); if (lvl_ok(t, l)) t->dirty[0] |= (uint16_t)(1u << l); RET(0); }
static void tex_dirtyrect(CPU *c, ComObj *s) { R(s)->dirty[0] = 0xFFFF; RET(0); }
static void cube_leveldesc(CPU *c, ComObj *s) { tex_leveldesc(c, s); }
static void cube_getsurf(CPU *c, ComObj *s) { Res *t = R(s); uint32_t f = A(1), l = A(2); if (f > 5 || !lvl_ok(t, l)) { RET(D3DERR_INVALIDCALL); return; } ComObj *o = level_surface(t, s, (int)f, (int)l); com_addref(o); OUTP(3, o->guest); RET(0); }
static void cube_lock(CPU *c, ComObj *s) { Res *t = R(s); uint32_t f = A(1), l = A(2); if (f > 5 || !lvl_ok(t, l)) { RET(D3DERR_INVALIDCALL); return; } lock_out(A(3), t, (int)f, (int)l, A(4)); RET(0); }
static void cube_unlock(CPU *c, ComObj *s) { Res *t = R(s); if (A(1) <= 5 && lvl_ok(t, A(2))) t->dirty[A(1)] |= (uint16_t)(1u << A(2)); RET(0); }
#define RES_METHODS { "GetDevice", res_getdevice }, { "SetPriority", res_setpriority }, { "GetPriority", res_getpriority }, { "GetType", res_gettype }, { "PreLoad", res_preload }, \
                    { "GetPrivateData", res_privnotfound }, { "FreePrivateData", res_privnotfound }
static const ComMethod tex_m[] = { RES_METHODS, { "SetLOD", tex_setlod }, { "GetLOD", tex_getlod }, { "GetLevelCount", tex_levels }, { "GenerateMipSubLevels", tex_genmips },
    { "GetAutoGenFilterType", tex_autofilter }, { "GetLevelDesc", tex_leveldesc }, { "GetSurfaceLevel", tex_getsurf }, { "LockRect", tex_lock }, { "UnlockRect", tex_unlock },
    { "AddDirtyRect", tex_dirtyrect }, { 0, 0 } };
static const ComMethod cube_m[] = { RES_METHODS, { "SetLOD", tex_setlod }, { "GetLOD", tex_getlod }, { "GetLevelCount", tex_levels }, { "GenerateMipSubLevels", tex_genmips },
    { "GetAutoGenFilterType", tex_autofilter }, { "GetLevelDesc", cube_leveldesc }, { "GetCubeMapSurface", cube_getsurf }, { "LockRect", cube_lock }, { "UnlockRect", cube_unlock }, { 0, 0 } };
static const ComMethod vol_m[] = { RES_METHODS, { "GetLevelCount", tex_levels }, { 0, 0 } };
static const uint8_t iids_tex[][16] = { IID_IDirect3DTexture9 }, iids_cube[][16] = { IID_IDirect3DCubeTexture9 }, iids_vol[][16] = { IID_IDirect3DVolumeTexture9 };
ComClass c_tex = { "IDirect3DTexture9", IFACE_IDirect3DTexture9, iids_tex, 1, tex_m, D3D_OK, res_destroy };
ComClass c_cube = { "IDirect3DCubeTexture9", IFACE_IDirect3DCubeTexture9, iids_cube, 1, cube_m, D3D_OK, res_destroy };
ComClass c_vol = { "IDirect3DVolumeTexture9", IFACE_IDirect3DVolumeTexture9, iids_vol, 1, vol_m, D3DERR_INVALIDCALL, res_destroy };

/* ================================================================ surfaces */
static ComObj *new_surface(Device *d, uint32_t w, uint32_t h, uint32_t fmt, uint32_t usage, uint32_t pool) {
    Res *s = new_res(d, R_SURF); s->w = w; s->h = h; s->fmt = fmt; s->usage = usage; s->pool = pool; s->depth = (usage & 2) != 0; return com_create(&c_surf, s);
}
static void surf_getdesc(CPU *c, ComObj *s) { Res *r = R(s); uint32_t w, h; surf_size(r, &w, &h); put_desc(A(1), r, w, h); RET(0); }
static void surf_container(CPU *c, ComObj *s) { Res *r = R(s); ComObj *o = r->container ? r->container : r->dev->obj; com_addref(o); OUTP(2, o->guest); RET(0); }
static void surf_lock(CPU *c, ComObj *s) {
    Res *r = R(s); if (r->depth) { RET(D3DERR_INVALIDCALL); return; }
    uint32_t w, h, pitch; surf_size(r, &w, &h); uint32_t m = surf_mem(r, &pitch);
    if (is_target(r) && !r->container) {        /* render target or back buffer: read the picture back into the lockable copy */
        uint8_t *rgba = malloc((size_t)w * h * 4); if (!plat_tex_read(surface_target(r), 0, 0, (int)w, (int)h, rgba, (int)w * 4)) fmt_from_rgba8(r->fmt, rgba, w, h, GP(m), pitch); free(rgba);
    }
    uint32_t off = 0, rect = A(2); if (rect) { uint32_t x = RD32(rect), y = RD32(rect + 4); off = fmt_is_dxt(r->fmt) ? (y / 4) * pitch + (x / 4) * (r->fmt == FMT_DXT1 ? 8 : 16) : y * pitch + x * (uint32_t)fmt_bpp(r->fmt); }
    WR32(A(1), pitch); WR32(A(1) + 4, m + off); RET(0);
}
static void surf_unlock(CPU *c, ComObj *s) {
    Res *r = R(s); surf_changed(r);
    if (is_target(r) && !r->container) {        /* write the locked picture back to the target */
        uint32_t w, h, pitch; surf_size(r, &w, &h); uint32_t m = surf_mem(r, &pitch); uint8_t *rgba = malloc((size_t)w * h * 4);
        fmt_to_rgba8(r->fmt, GP(m), pitch, w, h, rgba, NULL); int32_t full[4] = { 0, 0, (int32_t)w, (int32_t)h };
        blit_rgba(r->dev, rgba, w, h, full, surface_target(r), full, 0); free(rgba); r->lock_dirty = 0;
    }
    RET(0);
}
static void surf_getdc(CPU *c, ComObj *s) { (void)s; RET(D3DERR_INVALIDCALL); }
static const ComMethod surf_m[] = { RES_METHODS, { "GetContainer", surf_container }, { "GetDesc", surf_getdesc }, { "LockRect", surf_lock }, { "UnlockRect", surf_unlock },
    { "GetDC", surf_getdc }, { 0, 0 } };
static const uint8_t iids_surf[][16] = { IID_IDirect3DSurface9 };
static void surf_destroy(ComObj *o) { Res *r = R(o); r->container = NULL; res_destroy(o); }
ComClass c_surf = { "IDirect3DSurface9", IFACE_IDirect3DSurface9, iids_surf, 1, surf_m, D3D_OK, surf_destroy };

/* ================================================================ vertex and index buffers */
static void buf_lock(CPU *c, ComObj *s) { Res *r = R(s); uint32_t off = A(1) < r->size ? A(1) : 0; OUTP(3, r->data + off); RET(0); }
static void buf_unlock(CPU *c, ComObj *s) { (void)s; RET(0); }
static void vb_desc(CPU *c, ComObj *s) { Res *r = R(s); uint32_t p = A(1); memset(GP(p), 0, 24); WR32(p, 100); WR32(p + 4, 6); WR32(p + 8, r->usage); WR32(p + 12, r->pool); WR32(p + 16, r->size); WR32(p + 20, r->fvf); RET(0); }
static void ib_desc(CPU *c, ComObj *s) { Res *r = R(s); uint32_t p = A(1); memset(GP(p), 0, 20); WR32(p, r->fmt); WR32(p + 4, 7); WR32(p + 8, r->usage); WR32(p + 12, r->pool); WR32(p + 16, r->size); RET(0); }
static const ComMethod vb_m[] = { RES_METHODS, { "Lock", buf_lock }, { "Unlock", buf_unlock }, { "GetDesc", vb_desc }, { 0, 0 } };
static const ComMethod ib_m[] = { RES_METHODS, { "Lock", buf_lock }, { "Unlock", buf_unlock }, { "GetDesc", ib_desc }, { 0, 0 } };
static const uint8_t iids_vb[][16] = { IID_IDirect3DVertexBuffer9 }, iids_ib[][16] = { IID_IDirect3DIndexBuffer9 };
ComClass c_vb = { "IDirect3DVertexBuffer9", IFACE_IDirect3DVertexBuffer9, iids_vb, 1, vb_m, D3D_OK, res_destroy };
ComClass c_ib = { "IDirect3DIndexBuffer9", IFACE_IDirect3DIndexBuffer9, iids_ib, 1, ib_m, D3D_OK, res_destroy };

/* ================================================================ queries, declarations, shaders, swap chain */
typedef struct { Device *dev; uint32_t type; } Query;
static void q_getdevice(CPU *c, ComObj *s) { Device *d = ((Query *)s->data)->dev; com_addref(d->obj); OUTP(1, d->obj->guest); RET(0); }
static void q_type(CPU *c, ComObj *s) { RET(((Query *)s->data)->type); }
static void q_size(CPU *c, ComObj *s) { uint32_t t = ((Query *)s->data)->type; RET(t == 8 || t == 9 ? 4 : t == 2 ? 4 : 0); }
static void q_getdata(CPU *c, ComObj *s) { uint32_t t = ((Query *)s->data)->type; if (A(1) && A(2) >= 4) WR32(A(1), t == 9 ? 1000 : 1); RET(0); }   /* events done; occlusion: visible */
static const ComMethod q_m[] = { { "GetDevice", q_getdevice }, { "GetType", q_type }, { "GetDataSize", q_size }, { "GetData", q_getdata }, { 0, 0 } };
static void free_data(ComObj *o) { free(o->data); }
static const uint8_t iids_q[][16] = { IID_IDirect3DQuery9 };
ComClass c_query = { "IDirect3DQuery9", IFACE_IDirect3DQuery9, iids_q, 1, q_m, D3D_OK, free_data };
static void decl_getdecl(CPU *c, ComObj *s) { Decl *dc = s->data; if (A(1)) memcpy(GP(A(1)), dc->elems, 8 * (size_t)(dc->n + 1)); OUTP(2, dc->n + 1); RET(0); }
static void decl_destroy(ComObj *o) { Decl *dc = o->data; free(dc->elems); free(dc); }
static const ComMethod decl_m[] = { { "GetDeclaration", decl_getdecl }, { 0, 0 } };
static const uint8_t iids_decl[][16] = { IID_IDirect3DVertexDeclaration9 }, iids_vs[][16] = { IID_IDirect3DVertexShader9 }, iids_ps[][16] = { IID_IDirect3DPixelShader9 };
ComClass c_decl = { "IDirect3DVertexDeclaration9", IFACE_IDirect3DVertexDeclaration9, iids_decl, 1, decl_m, D3D_OK, decl_destroy };
ComClass c_vs = { "IDirect3DVertexShader9", IFACE_IDirect3DVertexShader9, iids_vs, 1, NULL, D3DERR_INVALIDCALL, NULL };
ComClass c_ps = { "IDirect3DPixelShader9", IFACE_IDirect3DPixelShader9, iids_ps, 1, NULL, D3DERR_INVALIDCALL, NULL };
static void dev_present(CPU *c, ComObj *s);
static void swap_present(CPU *c, ComObj *s) { Device *d = s->data; dev_present(c, d->obj); }
static void swap_backbuffer(CPU *c, ComObj *s) { Device *d = s->data; com_addref(d->backbuf); OUTP(3, d->backbuf->guest); RET(0); }
static void swap_getdevice(CPU *c, ComObj *s) { Device *d = s->data; com_addref(d->obj); OUTP(1, d->obj->guest); RET(0); }
static void swap_dispmode(CPU *c, ComObj *s) { Device *d = s->data; put_mode(A(1), d->bb_w, d->bb_h, d->bb_fmt); RET(0); }
static void swap_params(CPU *c, ComObj *s) { Device *d = s->data; uint32_t p = A(1); memset(GP(p), 0, 56); WR32(p, d->bb_w); WR32(p + 4, d->bb_h); WR32(p + 8, d->bb_fmt); WR32(p + 12, 1); WR32(p + 24, 1); WR32(p + 28, d->hwnd); WR32(p + 32, (uint32_t)d->windowed); WR32(p + 36, d->autodepth != NULL); WR32(p + 40, d->depth_fmt); RET(0); }
static void swap_raster(CPU *c, ComObj *s) { (void)s; memset(GP(A(1)), 0, 8); WR32(A(1), 1); RET(0); }
static const ComMethod swap_m[] = { { "Present", swap_present }, { "GetBackBuffer", swap_backbuffer }, { "GetDevice", swap_getdevice }, { "GetDisplayMode", swap_dispmode },
    { "GetPresentParameters", swap_params }, { "GetRasterStatus", swap_raster }, { 0, 0 } };
static const uint8_t iids_swap[][16] = { IID_IDirect3DSwapChain9 };
ComClass c_swap = { "IDirect3DSwapChain9", IFACE_IDirect3DSwapChain9, iids_swap, 1, swap_m, D3D_OK, NULL };

/* ================================================================ state blocks */
static void mask_all(Mask *m, int pixel, int vertex) {
    /* D3DSBT_PIXELSTATE / D3DSBT_VERTEXSTATE groups, simplified: pixel = blending, depth, fog colour, stages; vertex = transforms, lights, fog modes */
    static const uint8_t vtx_rs[] = { 22, 28, 35, 36, 37, 38, 48, 136, 137, 139, 140, 141, 142, 143, 145, 146, 147, 148, 151, 152, 154, 155, 156, 157 };
    if (pixel) { for (int i = 0; i < 256; i++) m->rs[i] = 1; for (int s = 0; s < 8; s++) for (int t = 0; t < 33; t++) m->tss[s][t] = 1; for (int s = 0; s < 16; s++) for (int t = 0; t < 14; t++) m->ss[s][t] = 1; m->palette = m->scissor = 1;
                 for (int s = 0; s < 8; s++) m->texture[s] = 1; }
    if (vertex) { for (unsigned i = 0; i < sizeof vtx_rs; i++) m->rs[vtx_rs[i]] = 1; m->world = m->view = m->proj = m->vp = m->material = m->fvf = m->decl = m->vs = m->vb = m->ib = 1;
                  for (int s = 0; s < 8; s++) m->tex[s] = 1; for (int i = 0; i < MAX_LIGHTS; i++) m->light[i] = m->light_on[i] = 1; for (int s = 0; s < 8; s++) m->tss[s][11] = m->tss[s][24] = 1; }
    if (pixel && vertex) m->ps = m->vp = 1;
}
static void hold(StateBlock *b, int on) {            /* the references a block holds on the objects it captured */
    void (*f)(ComObj *) = on ? com_addref : com_release; State *s = &b->st; Mask *m = &b->m;
    if (m->decl && s->decl) f(s->decl); if (m->vs && s->vs) f(s->vs); if (m->ps && s->ps) f(s->ps); if (m->vb && s->vb) f(s->vb); if (m->ib && s->ib) f(s->ib);
    for (int i = 0; i < 8; i++) if (m->texture[i] && s->tex[i]) f(s->tex[i]);
}
static void sb_copy(State *dst, const State *src, const Mask *m, Device *d, int to_device) {
    for (int i = 0; i < 256; i++) if (m->rs[i]) dst->rs[i] = src->rs[i];
    for (int s = 0; s < 8; s++) for (int t = 0; t < 33; t++) if (m->tss[s][t]) dst->tss[s][t] = src->tss[s][t];
    for (int s = 0; s < 16; s++) for (int t = 0; t < 14; t++) if (m->ss[s][t]) dst->ss[s][t] = src->ss[s][t];
    if (m->world) dst->world = src->world; if (m->view) dst->view = src->view; if (m->proj) dst->proj = src->proj; for (int i = 0; i < 8; i++) if (m->tex[i]) dst->texm[i] = src->texm[i];
    if (m->vp) memcpy(dst->vp, src->vp, sizeof dst->vp); if (m->material) memcpy(dst->material, src->material, sizeof dst->material);
    for (int i = 0; i < MAX_LIGHTS; i++) { if (m->light[i]) dst->light[i] = src->light[i]; if (m->light_on[i]) dst->light_on[i] = src->light_on[i]; }
    if (m->fvf) dst->fvf = src->fvf; if (m->scissor) memcpy(dst->scissor, src->scissor, sizeof dst->scissor); if (m->palette) dst->palette = src->palette;
    if (m->vb) { dst->vb_off = src->vb_off; dst->vb_stride = src->vb_stride; }
    if (to_device) {                 /* objects: through bind() so the device's references stay right */
        if (m->decl) bind(&dst->decl, src->decl); if (m->vs) bind(&dst->vs, src->vs); if (m->ps) bind(&dst->ps, src->ps); if (m->vb) bind(&dst->vb, src->vb); if (m->ib) bind(&dst->ib, src->ib);
        for (int i = 0; i < 8; i++) if (m->texture[i]) bind(&dst->tex[i], src->tex[i]);
        apply_viewport(d);
    } else {
        if (m->decl) dst->decl = src->decl; if (m->vs) dst->vs = src->vs; if (m->ps) dst->ps = src->ps; if (m->vb) dst->vb = src->vb; if (m->ib) dst->ib = src->ib;
        for (int i = 0; i < 8; i++) if (m->texture[i]) dst->tex[i] = src->tex[i];
    }
}
static void sb_capture(CPU *c, ComObj *s) { StateBlock *b = s->data; hold(b, 0); sb_copy(&b->st, &b->dev->st, &b->m, b->dev, 0); hold(b, 1); RET(0); }
static void sb_apply(CPU *c, ComObj *s) { StateBlock *b = s->data; sb_copy(&b->dev->st, &b->st, &b->m, b->dev, 1); RET(0); }
static void sb_getdevice(CPU *c, ComObj *s) { StateBlock *b = s->data; com_addref(b->dev->obj); OUTP(1, b->dev->obj->guest); RET(0); }
static void sb_destroy(ComObj *o) { StateBlock *b = o->data; hold(b, 0); free(b); }
static const ComMethod sb_m[] = { { "GetDevice", sb_getdevice }, { "Capture", sb_capture }, { "Apply", sb_apply }, { 0, 0 } };
static const uint8_t iids_sb[][16] = { IID_IDirect3DStateBlock9 };
ComClass c_sb = { "IDirect3DStateBlock9", IFACE_IDirect3DStateBlock9, iids_sb, 1, sb_m, D3D_OK, sb_destroy };

/* ================================================================ IDirect3DDevice9 */
/* While a state block is being recorded, Set* calls go into it instead of the device. */
#define RECORDING(d) ((d)->recording)
#define TARGET_STATE(d) (RECORDING(d) ? &(d)->recording->st : &(d)->st)
static void dev_testcoop(CPU *c, ComObj *s) { (void)s; RET(0); }
static void dev_texmem(CPU *c, ComObj *s) { (void)s; RET(512u << 20); }
static void dev_getd3d(CPU *c, ComObj *s) { Device *d = DEV(s); com_addref(d->d3d); OUTP(1, d->d3d->guest); RET(0); }
static void dev_getcaps(CPU *c, ComObj *s) { (void)s; fill_caps(A(1)); RET(0); }
static void dev_dispmode(CPU *c, ComObj *s) { Device *d = DEV(s); put_mode(A(2), d->bb_w, d->bb_h, d->bb_fmt); RET(0); }
static void dev_creation(CPU *c, ComObj *s) { Device *d = DEV(s); uint32_t p = A(1); WR32(p, 0); WR32(p + 4, 1); WR32(p + 8, d->hwnd); WR32(p + 12, d->behavior); RET(0); }
static void dev_showcursor(CPU *c, ComObj *s) { (void)s; RET(0); }
static void dev_getswap(CPU *c, ComObj *s) { Device *d = DEV(s); if (A(1)) { RET(D3DERR_INVALIDCALL); return; } if (!d->swap) d->swap = com_create(&c_swap, d); com_addref(d->swap); OUTP(2, d->swap->guest); RET(0); }
static void dev_nswap(CPU *c, ComObj *s) { (void)s; RET(1); }
static void dev_reset(CPU *c, ComObj *s) {
    Device *d = DEV(s);
    for (int i = 0; i < 8; i++) bind(&d->st.tex[i], NULL);        /* default_state clears these slots: drop their references first */
    bind(&d->st.vb, NULL); bind(&d->st.ib, NULL); bind(&d->st.decl, NULL); bind(&d->st.vs, NULL); bind(&d->st.ps, NULL);
    default_state(&d->st, RD32(A(1) + 36) != 0); dev_present_params(d, A(1), 1); RET(0);
}
static void dev_present(CPU *c, ComObj *s) {
    Device *d = DEV(s); (void)c;
    plat_gfx_present(); port_frame_presented(); plat_gfx_begin_frame(); plat_gfx_set_target(surface_target(R(d->rt))); apply_viewport(d);
    c->eax = 0;
}
static void dev_getbackbuffer(CPU *c, ComObj *s) { Device *d = DEV(s); if (A(1) || A(2)) { RET(D3DERR_INVALIDCALL); return; } com_addref(d->backbuf); OUTP(4, d->backbuf->guest); RET(0); }
static void dev_raster(CPU *c, ComObj *s) { (void)s; memset(GP(A(2)), 0, 8); WR32(A(2), 1); RET(0); }
static void dev_setgamma(CPU *c, ComObj *s) { Device *d = DEV(s); memcpy(d->gamma, GP(A(3)), sizeof d->gamma); plat_gfx_gamma((const uint16_t (*)[256])d->gamma); }
static void dev_getgamma(CPU *c, ComObj *s) { Device *d = DEV(s); memcpy(GP(A(2)), d->gamma, sizeof d->gamma); }
static void dev_createtexture(CPU *c, ComObj *s) {
    Device *d = DEV(s); uint32_t w = A(1), h = A(2), levels = A(3), usage = A(4), fmt = A(5), pool = A(6);
    if (!w || !h || !fmt_known(fmt) || fmt_is_depth(fmt)) { RET(D3DERR_INVALIDCALL); return; }
    Res *t = new_res(d, R_TEX); t->w = w; t->h = h; t->fmt = fmt; t->usage = usage; t->pool = pool;
    uint32_t full = 1, m = w > h ? w : h; while (m > 1) { full++; m >>= 1; }
    t->levels = !levels || levels > full ? full : levels; if (t->levels > 16) t->levels = 16;
    if (usage & 0x400) { t->autogen = 1; }                                          /* D3DUSAGE_AUTOGENMIPMAP */
    ComObj *o = com_create(&c_tex, t); t->obj = o; OUTP(7, o->guest); RET(0);
}
static void dev_createcube(CPU *c, ComObj *s) {
    Device *d = DEV(s); uint32_t e = A(1), levels = A(2), fmt = A(4); if (!e || !fmt_known(fmt)) { RET(D3DERR_INVALIDCALL); return; }
    Res *t = new_res(d, R_CUBE); t->w = t->h = e; t->fmt = fmt; t->usage = A(3); t->pool = A(5);
    uint32_t full = 1, m = e; while (m > 1) { full++; m >>= 1; } t->levels = !levels || levels > full ? full : levels;
    ComObj *o = com_create(&c_cube, t); t->obj = o; OUTP(6, o->guest); RET(0);
}
static void dev_createvolume(CPU *c, ComObj *s) { (void)s; OUTP(8, 0); RET(D3DERR_NOTAVAILABLE); }
static void dev_createvb(CPU *c, ComObj *s) {
    Device *d = DEV(s); Res *b = new_res(d, R_VB); b->size = A(1); b->usage = A(2); b->fvf = A(3); b->pool = A(4); b->data = g_alloc(b->size ? b->size : 4);
    if (!b->data) { free(b); RET(0x8007000Eu); return; } ComObj *o = com_create(&c_vb, b); b->obj = o; OUTP(5, o->guest); RET(0);
}
static void dev_createib(CPU *c, ComObj *s) {
    Device *d = DEV(s); Res *b = new_res(d, R_IB); b->size = A(1); b->usage = A(2); b->fmt = A(3); b->pool = A(4); b->data = g_alloc(b->size ? b->size : 4);
    if (!b->data) { free(b); RET(0x8007000Eu); return; } ComObj *o = com_create(&c_ib, b); b->obj = o; OUTP(5, o->guest); RET(0);
}
static void dev_creatert(CPU *c, ComObj *s) { Device *d = DEV(s); ComObj *o = new_surface(d, A(1), A(2), A(3), 1, 0); OUTP(7, o->guest); RET(0); }
static void dev_createds(CPU *c, ComObj *s) { Device *d = DEV(s); ComObj *o = new_surface(d, A(1), A(2), A(3), 2, 0); OUTP(7, o->guest); RET(0); }
static void dev_createoff(CPU *c, ComObj *s) { Device *d = DEV(s); ComObj *o = new_surface(d, A(1), A(2), A(3), 0, A(4)); OUTP(5, o->guest); RET(0); }
static void dev_updatesurface(CPU *c, ComObj *s) {
    Device *d = DEV(s); Res *src = R(com_get(A(1))), *dst = R(com_get(A(3))); if (!src || !dst || src->fmt != dst->fmt) { RET(D3DERR_INVALIDCALL); return; }
    uint32_t sw, sh, dw, dh, sp, dp; surf_size(src, &sw, &sh); surf_size(dst, &dw, &dh); int32_t r[4]; rect_or_full(A(2), sw, sh, r);
    uint32_t dx = A(4) ? RD32(A(4)) : 0, dy = A(4) ? RD32(A(4) + 4) : 0, sm = surf_mem(src, &sp), dm = surf_mem(dst, &dp); int bpp = fmt_bpp(src->fmt);
    if (!bpp) { memcpy(GP(dm), GP(sm), sp * ((sh + 3) / 4) < dp * ((dh + 3) / 4) ? sp * ((sh + 3) / 4) : dp * ((dh + 3) / 4)); }
    else for (int32_t y = r[1]; y < r[3]; y++) if (dy + (uint32_t)(y - r[1]) < dh) memcpy(GP(dm + (dy + (uint32_t)(y - r[1])) * dp + dx * (uint32_t)bpp), GP(sm + (uint32_t)y * sp + (uint32_t)r[0] * (uint32_t)bpp), (size_t)(r[2] - r[0]) * (size_t)bpp);
    surf_changed(dst); (void)d; RET(0);
}
static void dev_updatetexture(CPU *c, ComObj *s) {
    (void)s; Res *src = R(com_get(A(1))), *dst = R(com_get(A(2))); if (!src || !dst || src->fmt != dst->fmt || src->kind != dst->kind) { RET(D3DERR_INVALIDCALL); return; }
    /* like Direct3D 9: a source with more levels skips its top ones, and the first copied level must match the
       destination's top level in size; anything else is INVALIDCALL (it would overrun the destination) */
    if (src->levels < dst->levels) { RET(D3DERR_INVALIDCALL); return; }
    int skip = (int)(src->levels - dst->levels);
    if (level_w(src, skip) != level_w(dst, 0) || level_h(src, skip) != level_h(dst, 0)) { RET(D3DERR_INVALIDCALL); return; }
    int faces = src->kind == R_CUBE ? 6 : 1;
    for (int f = 0; f < faces; f++) for (uint32_t l = 0; l < dst->levels; l++) {
        uint32_t pitch, size; fmt_layout(dst->fmt, level_w(dst, (int)l), level_h(dst, (int)l), &pitch, &size);
        memcpy(GP(level_mem(dst, f, (int)l)), GP(level_mem(src, f, (int)l + skip)), size); dst->dirty[f] |= (uint16_t)(1u << l);
    }
    RET(0);
}
static void dev_getrtdata(CPU *c, ComObj *s) {
    Device *d = DEV(s); Res *src = R(com_get(A(1))), *dst = R(com_get(A(2))); if (!src || !dst) { RET(D3DERR_INVALIDCALL); return; }
    uint32_t w, h, pitch; uint8_t *rgba = surf_rgba(d, src, &w, &h); uint32_t m = surf_mem(dst, &pitch); fmt_from_rgba8(dst->fmt, rgba, w, h, GP(m), pitch); free(rgba); RET(0);
}
static void dev_stretchrect(CPU *c, ComObj *s) {
    Device *d = DEV(s); Res *src = R(com_get(A(1))), *dst = R(com_get(A(3))); if (!src || !dst) { RET(D3DERR_INVALIDCALL); return; }
    uint32_t sw, sh, dw, dh; surf_size(src, &sw, &sh); surf_size(dst, &dw, &dh); int32_t sr[4], dr[4]; rect_or_full(A(2), sw, sh, sr); rect_or_full(A(4), dw, dh, dr);
    int linear = A(5) == 2;
    if (is_target(dst)) {
        if (is_target(src) && !src->lock_dirty) plat_gfx_copy(surface_target(src), sr[0], sr[1], sr[2] - sr[0], sr[3] - sr[1], surface_target(dst), dr[0], dr[1], dr[2] - dr[0], dr[3] - dr[1], linear);
        else { uint32_t w, h; uint8_t *rgba = surf_rgba(d, src, &w, &h); blit_rgba(d, rgba, w, h, sr, surface_target(dst), dr, linear); free(rgba); }
        plat_gfx_set_target(surface_target(R(d->rt))); apply_viewport(d);
    } else {                          /* into memory: nearest-neighbour scale through RGBA8 */
        uint32_t w, h, pitch; uint8_t *rgba = surf_rgba(d, src, &w, &h); uint8_t *out = calloc((size_t)dw * dh * 4, 1); uint32_t m = surf_mem(dst, &pitch);
        fmt_to_rgba8(dst->fmt, GP(m), pitch, dw, dh, out, NULL);
        for (int32_t y = dr[1]; y < dr[3]; y++) for (int32_t x = dr[0]; x < dr[2]; x++) {
            int32_t sx = sr[0] + (x - dr[0]) * (sr[2] - sr[0]) / (dr[2] - dr[0] ? dr[2] - dr[0] : 1), sy = sr[1] + (y - dr[1]) * (sr[3] - sr[1]) / (dr[3] - dr[1] ? dr[3] - dr[1] : 1);
            if (x >= 0 && y >= 0 && (uint32_t)x < dw && (uint32_t)y < dh && sx >= 0 && sy >= 0 && (uint32_t)sx < w && (uint32_t)sy < h) memcpy(out + ((size_t)y * dw + (size_t)x) * 4, rgba + ((size_t)sy * w + (size_t)sx) * 4, 4);
        }
        fmt_from_rgba8(dst->fmt, out, dw, dh, GP(m), pitch); surf_changed(dst); free(out); free(rgba);
    }
    RET(0);
}
static void dev_colorfill(CPU *c, ComObj *s) {
    Device *d = DEV(s); Res *r = R(com_get(A(1))); if (!r) { RET(D3DERR_INVALIDCALL); return; }
    uint32_t w, h; surf_size(r, &w, &h); int32_t rc[4]; rect_or_full(A(2), w, h, rc);
    if (is_target(r)) {
        plat_gfx_set_target(surface_target(r)); PlatViewport v = { 0, 0, (int)w, (int)h, 0, 1 }; plat_gfx_viewport(&v);
        plat_gfx_scissor(1, rc[0], rc[1], rc[2] - rc[0], rc[3] - rc[1]); plat_gfx_clear(1, A(3), 0, 0, 0, 0);
        plat_gfx_set_target(surface_target(R(d->rt))); apply_viewport(d);
    } else {
        uint32_t pitch, m = surf_mem(r, &pitch); uint8_t px[4] = { (uint8_t)(A(3) >> 16), (uint8_t)(A(3) >> 8), (uint8_t)A(3), (uint8_t)(A(3) >> 24) }; int bpp = fmt_bpp(r->fmt);
        uint8_t enc[4]; if (bpp && fmt_from_rgba8(r->fmt, px, 1, 1, enc, 4)) for (int32_t y = rc[1]; y < rc[3]; y++) for (int32_t x = rc[0]; x < rc[2]; x++) memcpy(GP(m + (uint32_t)y * pitch + (uint32_t)x * (uint32_t)bpp), enc, (size_t)bpp);
        surf_changed(r);
    }
    RET(0);
}
static void dev_setrt(CPU *c, ComObj *s) {
    Device *d = DEV(s); if (A(1) != 0) { RET(A(2) ? D3DERR_INVALIDCALL : 0); return; }
    ComObj *o = com_get(A(2)); if (!o) { RET(D3DERR_INVALIDCALL); return; }
    bind(&d->rt, o); plat_gfx_set_target(surface_target(R(o)));
    uint32_t w, h; target_size(d, &w, &h); d->st.vp[0] = d->st.vp[1] = 0; d->st.vp[2] = w; d->st.vp[3] = h; d->st.vp[4] = f2u(0); d->st.vp[5] = f2u(1);
    d->st.scissor[0] = d->st.scissor[1] = 0; d->st.scissor[2] = (int32_t)w; d->st.scissor[3] = (int32_t)h; apply_viewport(d); RET(0);
}
static void dev_getrt(CPU *c, ComObj *s) { Device *d = DEV(s); if (A(1) || !d->rt) { OUTP(2, 0); RET(D3DERR_NOTFOUND); return; } com_addref(d->rt); OUTP(2, d->rt->guest); RET(0); }
static void dev_setds(CPU *c, ComObj *s) { Device *d = DEV(s); bind(&d->ds, com_get(A(1))); RET(0); }
static void dev_getds(CPU *c, ComObj *s) { Device *d = DEV(s); if (!d->ds) { OUTP(1, 0); RET(D3DERR_NOTFOUND); return; } com_addref(d->ds); OUTP(1, d->ds->guest); RET(0); }
static void dev_beginscene(CPU *c, ComObj *s) { Device *d = DEV(s); if (d->in_scene) { RET(D3DERR_INVALIDCALL); return; } d->in_scene = 1; RET(0); }
static void dev_endscene(CPU *c, ComObj *s) { Device *d = DEV(s); if (!d->in_scene) { RET(D3DERR_INVALIDCALL); return; } d->in_scene = 0; RET(0); }
static void dev_clear(CPU *c, ComObj *s) {
    Device *d = DEV(s); uint32_t n = A(1), rects = A(2), flags = A(3); float z = u2f(A(5));
    int color = (flags & 1) != 0, depth = (flags & 2) && d->ds, sten = (flags & 4) && d->ds;
    if (!n || !rects) { plat_gfx_clear(color, A(4), depth, z, sten, A(6)); RET(0); return; }
    for (uint32_t i = 0; i < n; i++) { uint32_t r = rects + 16 * i; plat_gfx_scissor(1, (int32_t)RD32(r), (int32_t)RD32(r + 4), (int32_t)(RD32(r + 8) - RD32(r)), (int32_t)(RD32(r + 12) - RD32(r + 4))); plat_gfx_clear(color, A(4), depth, z, sten, A(6)); }
    apply_viewport(d); RET(0);
}
static Mat *transform_slot(State *st, uint32_t t) { if (t == 2) return &st->view; if (t == 3) return &st->proj; if (t >= 16 && t < 24) return &st->texm[t - 16]; if (t == 256) return &st->world; return NULL; }
static void mark_transform(Device *d, uint32_t t) { if (!RECORDING(d)) return; Mask *m = &d->recording->m; if (t == 2) m->view = 1; else if (t == 3) m->proj = 1; else if (t >= 16 && t < 24) m->tex[t - 16] = 1; else if (t == 256) m->world = 1; }
static void dev_settransform(CPU *c, ComObj *s) { Device *d = DEV(s); Mat *m = transform_slot(TARGET_STATE(d), A(1)); if (m) { memcpy(m->m, GP(A(2)), 64); mark_transform(d, A(1)); } RET(0); }
static void dev_gettransform(CPU *c, ComObj *s) { Device *d = DEV(s); Mat *m = transform_slot(&d->st, A(1)); if (m) memcpy(GP(A(2)), m->m, 64); else memcpy(GP(A(2)), IDENT.m, 64); RET(0); }
static void dev_multransform(CPU *c, ComObj *s) { Device *d = DEV(s); Mat *m = transform_slot(TARGET_STATE(d), A(1)); if (m) { Mat b; memcpy(b.m, GP(A(2)), 64); *m = mat_mul(&b, m); mark_transform(d, A(1)); } RET(0); }
static void dev_setviewport(CPU *c, ComObj *s) { Device *d = DEV(s); State *st = TARGET_STATE(d); memcpy(st->vp, GP(A(1)), 24); if (RECORDING(d)) d->recording->m.vp = 1; else apply_viewport(d); RET(0); }
static void dev_getviewport(CPU *c, ComObj *s) { Device *d = DEV(s); memcpy(GP(A(1)), d->st.vp, 24); RET(0); }
static void dev_setmaterial(CPU *c, ComObj *s) { Device *d = DEV(s); memcpy(TARGET_STATE(d)->material, GP(A(1)), 68); if (RECORDING(d)) d->recording->m.material = 1; RET(0); }
static void dev_getmaterial(CPU *c, ComObj *s) { Device *d = DEV(s); memcpy(GP(A(1)), d->st.material, 68); RET(0); }
static void dev_setlight(CPU *c, ComObj *s) { Device *d = DEV(s); uint32_t i = A(1); if (i >= MAX_LIGHTS) { RET(0); return; } memcpy(&TARGET_STATE(d)->light[i], GP(A(2)), 104); if (RECORDING(d)) d->recording->m.light[i] = 1; RET(0); }
static void dev_getlight(CPU *c, ComObj *s) { Device *d = DEV(s); uint32_t i = A(1); if (i >= MAX_LIGHTS) { RET(D3DERR_INVALIDCALL); return; } memcpy(GP(A(2)), &d->st.light[i], 104); RET(0); }
static void dev_lightenable(CPU *c, ComObj *s) { Device *d = DEV(s); uint32_t i = A(1); if (i >= MAX_LIGHTS) { static int w; if (!w++) port_debug("Direct3D: light %u ignored (at most %d)", i, MAX_LIGHTS); RET(0); return; } TARGET_STATE(d)->light_on[i] = A(2) != 0; if (RECORDING(d)) d->recording->m.light_on[i] = 1; RET(0); }
static void dev_getlightenable(CPU *c, ComObj *s) { Device *d = DEV(s); uint32_t i = A(1); OUTP(2, i < MAX_LIGHTS && d->st.light_on[i] ? 1u : 0u); RET(0); }
static void dev_setrs(CPU *c, ComObj *s) {
    Device *d = DEV(s); uint32_t k = A(1); if (k >= 256) { RET(0); return; }
    TARGET_STATE(d)->rs[k] = A(2); if (RECORDING(d)) d->recording->m.rs[k] = 1; else if (k == 174) apply_viewport(d);
    RET(0);
}
static void dev_getrs(CPU *c, ComObj *s) { Device *d = DEV(s); OUTP(2, A(1) < 256 ? d->st.rs[A(1)] : 0); RET(0); }
static void dev_createsb(CPU *c, ComObj *s) {
    Device *d = DEV(s); StateBlock *b = calloc(1, sizeof *b); b->dev = d; uint32_t t = A(1);
    mask_all(&b->m, t == 1 || t == 2, t == 1 || t == 3);                       /* D3DSBT_ALL 1, PIXELSTATE 2, VERTEXSTATE 3 */
    sb_copy(&b->st, &d->st, &b->m, d, 0); hold(b, 1);
    ComObj *o = com_create(&c_sb, b); OUTP(2, o->guest); RET(0);
}
static void dev_beginsb(CPU *c, ComObj *s) { Device *d = DEV(s); if (d->recording) { RET(D3DERR_INVALIDCALL); return; } d->recording = calloc(1, sizeof *d->recording); d->recording->dev = d; d->recording->st = d->st; RET(0); }
static void dev_endsb(CPU *c, ComObj *s) {
    Device *d = DEV(s); StateBlock *b = d->recording; if (!b) { RET(D3DERR_INVALIDCALL); return; }
    d->recording = NULL; hold(b, 1); ComObj *o = com_create(&c_sb, b); OUTP(1, o->guest); RET(0);
}
static void dev_gettexture(CPU *c, ComObj *s) { Device *d = DEV(s); uint32_t i = A(1); ComObj *t = i < 8 ? d->st.tex[i] : NULL; if (t) com_addref(t); OUTP(2, t ? t->guest : 0); RET(0); }
static void dev_settexture(CPU *c, ComObj *s) {
    Device *d = DEV(s); uint32_t i = A(1); if (i >= 8) { RET(0); return; }   /* displacement-map samplers and above: ignored */
    ComObj *t = A(2) ? com_get(A(2)) : NULL; if (A(2) && !t) { RET(D3DERR_INVALIDCALL); return; }
    if (RECORDING(d)) { d->recording->st.tex[i] = t; d->recording->m.texture[i] = 1; RET(0); return; }
    bind(&d->st.tex[i], t); RET(0);
}
static void dev_gettss(CPU *c, ComObj *s) { Device *d = DEV(s); OUTP(3, A(1) < 8 && A(2) < 33 ? d->st.tss[A(1)][A(2)] : 0); RET(0); }
static void dev_settss(CPU *c, ComObj *s) { Device *d = DEV(s); if (A(1) < 8 && A(2) < 33) { TARGET_STATE(d)->tss[A(1)][A(2)] = A(3); if (RECORDING(d)) d->recording->m.tss[A(1)][A(2)] = 1; } RET(0); }
static void dev_getss(CPU *c, ComObj *s) { Device *d = DEV(s); OUTP(3, A(1) < 16 && A(2) < 14 ? d->st.ss[A(1)][A(2)] : 0); RET(0); }
static void dev_setss(CPU *c, ComObj *s) { Device *d = DEV(s); if (A(1) < 16 && A(2) < 14) { TARGET_STATE(d)->ss[A(1)][A(2)] = A(3); if (RECORDING(d)) d->recording->m.ss[A(1)][A(2)] = 1; } RET(0); }
static void dev_validate(CPU *c, ComObj *s) { (void)s; OUTP(1, 1); RET(0); }
static void dev_setpal(CPU *c, ComObj *s) { Device *d = DEV(s); uint32_t n = A(1); if (n >= 256) { RET(D3DERR_INVALIDCALL); return; } memcpy(d->palettes[n], GP(A(2)), 1024); d->pal_ver[n]++; RET(0); }
static void dev_getpal(CPU *c, ComObj *s) { Device *d = DEV(s); uint32_t n = A(1); if (n >= 256) { RET(D3DERR_INVALIDCALL); return; } memcpy(GP(A(2)), d->palettes[n], 1024); RET(0); }
static void dev_setcurpal(CPU *c, ComObj *s) { Device *d = DEV(s); if (A(1) < 256) { TARGET_STATE(d)->palette = A(1); if (RECORDING(d)) d->recording->m.palette = 1; } RET(0); }
static void dev_getcurpal(CPU *c, ComObj *s) { Device *d = DEV(s); OUTP(1, d->st.palette); RET(0); }
static void dev_setscissor(CPU *c, ComObj *s) { Device *d = DEV(s); memcpy(TARGET_STATE(d)->scissor, GP(A(1)), 16); if (RECORDING(d)) d->recording->m.scissor = 1; else apply_viewport(d); RET(0); }
static void dev_getscissor(CPU *c, ComObj *s) { Device *d = DEV(s); memcpy(GP(A(1)), d->st.scissor, 16); RET(0); }
static void dev_swvp(CPU *c, ComObj *s) { (void)s; RET(0); }
static void dev_drawprim(CPU *c, ComObj *s) {
    Device *d = DEV(s); Res *vb = R(d->st.vb); if (!vb) { RET(D3DERR_INVALIDCALL); return; }
    draw(d, A(1), A(3), vb->data + d->st.vb_off + A(2) * d->st.vb_stride, d->st.vb_stride, 0, 0, 0, 0, 0); RET(0);
}
static void dev_drawindexed(CPU *c, ComObj *s) {
    Device *d = DEV(s); Res *vb = R(d->st.vb), *ib = R(d->st.ib); if (!vb || !ib) { RET(D3DERR_INVALIDCALL); return; }
    int i32 = ib->fmt == FMT_INDEX32;
    draw(d, A(1), A(6), vb->data + d->st.vb_off, d->st.vb_stride, ib->data + A(5) * (i32 ? 4u : 2u), i32, (int32_t)A(2), A(3), A(4)); RET(0);
}
static void dev_drawup(CPU *c, ComObj *s) {
    Device *d = DEV(s); draw(d, A(1), A(2), A(3), A(4), 0, 0, 0, 0, 0);
    bind(&d->st.vb, NULL); d->st.vb_off = d->st.vb_stride = 0; RET(0);                 /* D3D9 unbinds stream 0 after a UP draw */
}
static void dev_drawindexedup(CPU *c, ComObj *s) {
    Device *d = DEV(s); draw(d, A(1), A(4), A(7), A(8), A(5), A(6) == FMT_INDEX32, 0, A(2), A(3));
    bind(&d->st.vb, NULL); bind(&d->st.ib, NULL); d->st.vb_off = d->st.vb_stride = 0; RET(0);
}
static void dev_processvertices(CPU *c, ComObj *s) { (void)s; static int w; if (!w++) port_warn("Direct3D: ProcessVertices is not implemented"); RET(0); }
static void dev_createdecl(CPU *c, ComObj *s) {
    (void)s; uint32_t p = A(1), n = 0; while (RD8(p + 8 * n) != 0xFF && n < 64) n++;
    Decl *dc = calloc(1, sizeof *dc); dc->n = n; dc->elems = malloc(8 * (size_t)(n + 1)); memcpy(dc->elems, GP(p), 8 * (size_t)(n + 1));
    ComObj *o = com_create(&c_decl, dc); OUTP(2, o->guest); RET(0);
}
static void dev_setdecl(CPU *c, ComObj *s) { Device *d = DEV(s); ComObj *o = A(1) ? com_get_as(A(1), &c_decl) : NULL; if (RECORDING(d)) { d->recording->st.decl = o; d->recording->m.decl = 1; } else bind(&d->st.decl, o); RET(0); }
static void dev_getdecl(CPU *c, ComObj *s) { Device *d = DEV(s); if (d->st.decl) com_addref(d->st.decl); OUTP(1, d->st.decl ? d->st.decl->guest : 0); RET(0); }
static void dev_setfvf(CPU *c, ComObj *s) {
    Device *d = DEV(s); State *st = TARGET_STATE(d); st->fvf = A(1);
    if (RECORDING(d)) { d->recording->m.fvf = 1; d->recording->st.decl = NULL; d->recording->m.decl = 1; } else bind(&d->st.decl, NULL); RET(0);
}
static void dev_getfvf(CPU *c, ComObj *s) { Device *d = DEV(s); OUTP(1, d->st.fvf); RET(0); }
static void dev_createshader(CPU *c, ComObj *s) { (void)s; static int w; if (!w++) port_warn("Direct3D: the game asked for a shader; shaders are not supported (fixed function only)"); OUTP(2, 0); RET(D3DERR_INVALIDCALL); }
static void dev_setvs(CPU *c, ComObj *s) { Device *d = DEV(s); ComObj *o = A(1) ? com_get(A(1)) : NULL; if (RECORDING(d)) { d->recording->st.vs = o; d->recording->m.vs = 1; } else bind(&d->st.vs, o); RET(0); }
static void dev_setps(CPU *c, ComObj *s) { Device *d = DEV(s); ComObj *o = A(1) ? com_get(A(1)) : NULL; if (RECORDING(d)) { d->recording->st.ps = o; d->recording->m.ps = 1; } else bind(&d->st.ps, o); RET(0); }
static void dev_getnull(CPU *c, ComObj *s) { (void)s; OUTP(1, 0); RET(0); }
static void dev_setstream(CPU *c, ComObj *s) {
    Device *d = DEV(s); if (A(1) != 0) { if (A(2)) { static int w; if (!w++) port_warn("Direct3D: vertex stream %u is not supported (one stream only)", A(1)); } RET(0); return; }
    ComObj *o = A(2) ? com_get_as(A(2), &c_vb) : NULL; State *st = TARGET_STATE(d);
    if (RECORDING(d)) { st->vb = o; d->recording->m.vb = 1; } else bind(&d->st.vb, o);
    st->vb_off = A(3); st->vb_stride = A(4); RET(0);
}
static void dev_getstream(CPU *c, ComObj *s) { Device *d = DEV(s); ComObj *o = A(1) == 0 ? d->st.vb : NULL; if (o) com_addref(o); OUTP(2, o ? o->guest : 0); OUTP(3, A(1) == 0 ? d->st.vb_off : 0); OUTP(4, A(1) == 0 ? d->st.vb_stride : 0); RET(0); }
static void dev_setstreamfreq(CPU *c, ComObj *s) { (void)s; RET(A(2) == 1 ? 0 : D3DERR_INVALIDCALL); }
static void dev_getstreamfreq(CPU *c, ComObj *s) { (void)s; OUTP(2, 1); RET(0); }
static void dev_setindices(CPU *c, ComObj *s) { Device *d = DEV(s); ComObj *o = A(1) ? com_get_as(A(1), &c_ib) : NULL; if (RECORDING(d)) { d->recording->st.ib = o; d->recording->m.ib = 1; } else bind(&d->st.ib, o); RET(0); }
static void dev_getindices(CPU *c, ComObj *s) { Device *d = DEV(s); if (d->st.ib) com_addref(d->st.ib); OUTP(1, d->st.ib ? d->st.ib->guest : 0); RET(0); }
static void dev_createquery(CPU *c, ComObj *s) {
    Device *d = DEV(s); uint32_t t = A(1); if (t != 8 && t != 9 && t != 2) { RET(D3DERR_NOTAVAILABLE); return; }    /* EVENT, OCCLUSION, VCACHE */
    if (!A(2)) { RET(0); return; }                                                                                  /* asking whether the type is supported */
    Query *q = calloc(1, sizeof *q); q->dev = d; q->type = t; ComObj *o = com_create(&c_query, q); OUTP(2, o->guest); RET(0);
}
static void dev_evict(CPU *c, ComObj *s) { (void)s; RET(0); }
static void dev_setclipplane(CPU *c, ComObj *s) { (void)s; RET(0); }
static void dev_getclipplane(CPU *c, ComObj *s) { (void)s; memset(GP(A(2)), 0, 16); RET(0); }
static void dev_destroy(ComObj *o) {
    Device *d = DEV(o);
    for (int i = 0; i < 8; i++) bind(&d->st.tex[i], NULL);
    bind(&d->st.vb, NULL); bind(&d->st.ib, NULL); bind(&d->st.decl, NULL); bind(&d->st.vs, NULL); bind(&d->st.ps, NULL); bind(&d->rt, NULL); bind(&d->ds, NULL);
    if (d->backbuf) com_release(d->backbuf); if (d->autodepth) com_release(d->autodepth); if (d->swap) com_release(d->swap);
    com_release(d->d3d); free(d->vbuf); free(d->ibuf); free(d->scratch); free(d);
}
static const ComMethod dev_m[] = {
    { "TestCooperativeLevel", dev_testcoop }, { "GetAvailableTextureMem", dev_texmem }, { "EvictManagedResources", dev_evict }, { "GetDirect3D", dev_getd3d },
    { "GetDeviceCaps", dev_getcaps }, { "GetDisplayMode", dev_dispmode }, { "GetCreationParameters", dev_creation }, { "ShowCursor", dev_showcursor },
    { "GetSwapChain", dev_getswap }, { "GetNumberOfSwapChains", dev_nswap }, { "Reset", dev_reset }, { "Present", dev_present }, { "GetBackBuffer", dev_getbackbuffer },
    { "GetRasterStatus", dev_raster }, { "SetGammaRamp", dev_setgamma }, { "GetGammaRamp", dev_getgamma }, { "CreateTexture", dev_createtexture },
    { "CreateVolumeTexture", dev_createvolume }, { "CreateCubeTexture", dev_createcube }, { "CreateVertexBuffer", dev_createvb }, { "CreateIndexBuffer", dev_createib },
    { "CreateRenderTarget", dev_creatert }, { "CreateDepthStencilSurface", dev_createds }, { "UpdateSurface", dev_updatesurface }, { "UpdateTexture", dev_updatetexture },
    { "GetRenderTargetData", dev_getrtdata }, { "StretchRect", dev_stretchrect }, { "ColorFill", dev_colorfill }, { "CreateOffscreenPlainSurface", dev_createoff },
    { "SetRenderTarget", dev_setrt }, { "GetRenderTarget", dev_getrt }, { "SetDepthStencilSurface", dev_setds }, { "GetDepthStencilSurface", dev_getds },
    { "BeginScene", dev_beginscene }, { "EndScene", dev_endscene }, { "Clear", dev_clear }, { "SetTransform", dev_settransform }, { "GetTransform", dev_gettransform },
    { "MultiplyTransform", dev_multransform }, { "SetViewport", dev_setviewport }, { "GetViewport", dev_getviewport }, { "SetMaterial", dev_setmaterial },
    { "GetMaterial", dev_getmaterial }, { "SetLight", dev_setlight }, { "GetLight", dev_getlight }, { "LightEnable", dev_lightenable }, { "GetLightEnable", dev_getlightenable },
    { "SetClipPlane", dev_setclipplane }, { "GetClipPlane", dev_getclipplane }, { "SetRenderState", dev_setrs }, { "GetRenderState", dev_getrs },
    { "CreateStateBlock", dev_createsb }, { "BeginStateBlock", dev_beginsb }, { "EndStateBlock", dev_endsb }, { "GetTexture", dev_gettexture }, { "SetTexture", dev_settexture },
    { "GetTextureStageState", dev_gettss }, { "SetTextureStageState", dev_settss }, { "GetSamplerState", dev_getss }, { "SetSamplerState", dev_setss },
    { "ValidateDevice", dev_validate }, { "SetPaletteEntries", dev_setpal }, { "GetPaletteEntries", dev_getpal }, { "SetCurrentTexturePalette", dev_setcurpal },
    { "GetCurrentTexturePalette", dev_getcurpal }, { "SetScissorRect", dev_setscissor }, { "GetScissorRect", dev_getscissor }, { "GetSoftwareVertexProcessing", dev_swvp },
    { "DrawPrimitive", dev_drawprim }, { "DrawIndexedPrimitive", dev_drawindexed }, { "DrawPrimitiveUP", dev_drawup }, { "DrawIndexedPrimitiveUP", dev_drawindexedup },
    { "ProcessVertices", dev_processvertices }, { "CreateVertexDeclaration", dev_createdecl }, { "SetVertexDeclaration", dev_setdecl }, { "GetVertexDeclaration", dev_getdecl },
    { "SetFVF", dev_setfvf }, { "GetFVF", dev_getfvf }, { "CreateVertexShader", dev_createshader }, { "SetVertexShader", dev_setvs }, { "GetVertexShader", dev_getnull },
    { "SetStreamSource", dev_setstream }, { "GetStreamSource", dev_getstream }, { "SetStreamSourceFreq", dev_setstreamfreq }, { "GetStreamSourceFreq", dev_getstreamfreq },
    { "SetIndices", dev_setindices }, { "GetIndices", dev_getindices }, { "CreatePixelShader", dev_createshader }, { "SetPixelShader", dev_setps }, { "GetPixelShader", dev_getnull },
    { "CreateQuery", dev_createquery }, { 0, 0 } };
static const uint8_t iids_dev[][16] = { IID_IDirect3DDevice9 };
ComClass c_dev = { "IDirect3DDevice9", IFACE_IDirect3DDevice9, iids_dev, 1, dev_m, D3D_OK, dev_destroy };

/* ================================================================ the DLL exports (d3d9.dll, d3dx9_*.dll) */
SHIM(Direct3DCreate9) { RET(com_create(&c_d3d9, calloc(1, sizeof(D3D9)))->guest); }
SHIM(D3DXCreateEffect) { if (A(7)) WR32(A(7), 0); RET(D3DERR_INVALIDCALL); }     /* no effect files: the engine keeps its fixed-function path */
SHIM(D3DXFilterTexture) {
    ComObj *o = com_get(A(0)); Res *t = R(o); if (!t || t->kind != R_TEX) { RET(D3DERR_INVALIDCALL); return; }
    uint32_t src = A(2) == 0xFFFFFFFFu ? 0 : A(2); if (src + 1 >= t->levels) { RET(0); return; }
    uint32_t w = level_w(t, (int)src), h = level_h(t, (int)src), pitch, size; fmt_layout(t->fmt, w, h, &pitch, &size);
    uint8_t *a = malloc((size_t)w * h * 4), *b = malloc((size_t)w * h * 4); fmt_to_rgba8(t->fmt, GP(level_mem(t, 0, (int)src)), pitch, w, h, a, NULL);
    for (uint32_t l = src + 1; l < t->levels; l++) {
        rgba8_half(a, w, h, b); w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1; fmt_layout(t->fmt, w, h, &pitch, &size);
        if (!fmt_from_rgba8(t->fmt, b, w, h, GP(level_mem(t, 0, (int)l)), pitch)) break;
        t->dirty[0] |= (uint16_t)(1u << l); uint8_t *x = a; a = b; b = x;
    }
    free(a); free(b); RET(0);
}
SHIM(D3DXLoadSurfaceFromSurface) {
    Res *dst = R(com_get(A(0))), *src = R(com_get(A(3))); if (!dst || !src) { RET(D3DERR_INVALIDCALL); return; }
    Device *d = dst->dev; uint32_t sw, sh, dw, dh, pitch; surf_size(src, &sw, &sh); surf_size(dst, &dw, &dh); int32_t sr[4], dr[4]; rect_or_full(A(5), sw, sh, sr); rect_or_full(A(2), dw, dh, dr);
    uint32_t w, h; uint8_t *rgba = surf_rgba(d, src, &w, &h); uint8_t *out = calloc((size_t)dw * dh * 4, 1); uint32_t m = surf_mem(dst, &pitch);
    fmt_to_rgba8(dst->fmt, GP(m), pitch, dw, dh, out, NULL);
    for (int32_t y = dr[1]; y < dr[3]; y++) for (int32_t x = dr[0]; x < dr[2]; x++) {
        int32_t sx = sr[0] + (x - dr[0]) * (sr[2] - sr[0]) / (dr[2] - dr[0] ? dr[2] - dr[0] : 1), sy = sr[1] + (y - dr[1]) * (sr[3] - sr[1]) / (dr[3] - dr[1] ? dr[3] - dr[1] : 1);
        if (x >= 0 && y >= 0 && (uint32_t)x < dw && (uint32_t)y < dh && sx >= 0 && sy >= 0 && (uint32_t)sx < w && (uint32_t)sy < h) memcpy(out + ((size_t)y * dw + (size_t)x) * 4, rgba + ((size_t)sy * w + (size_t)sx) * 4, 4);
    }
    int ok = fmt_from_rgba8(dst->fmt, out, dw, dh, GP(m), pitch); surf_changed(dst); free(out); free(rgba);
    RET(ok ? 0 : D3DERR_INVALIDCALL);
}
const ShimDef d3d9_shims[] = {
    { "Direct3DCreate9", sh_Direct3DCreate9, STD(1) }, { "D3DXCreateEffect", sh_D3DXCreateEffect, STD(9) }, { "D3DXFilterTexture", sh_D3DXFilterTexture, STD(4) },
    { "D3DXLoadSurfaceFromSurface", sh_D3DXLoadSurfaceFromSurface, STD(8) },
    { 0, 0, 0 }
};
