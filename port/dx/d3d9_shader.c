/* d3d9_shader.c: Direct3D 9 pixel shader bytecode to GLSL (see d3d9_shader.h).
 *
 * Shader model 2 instructions map one to one onto GLSL expressions on vec4 registers: each instruction becomes one
 * assignment through its write mask, with its source swizzles, negation / abs and saturation. The register files:
 *   r#  temporaries                        vec4 r#
 *   c#  float constants                    vec4 c#_ from `def` when the shader defines it, else u_c[#]
 *   t0, t1  texture coordinates            vec4(v_uv.xy, 0, 1), vec4(v_uv.zw, 0, 1)   (sets 0 and 1)
 *   v0, v1  diffuse and specular colour    v_diff, v_spec
 *   s0, s1  samplers                       u_t0, u_t1   (the textures and sampler states of stages 0 and 1)
 *   oC0     the result                     returned
 * The output is valid GLSL 1.20 and GLSL ES 1.00. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "d3d9_shader.h"

enum { RT_TEMP = 0, RT_INPUT = 1, RT_CONST = 2, RT_TEXTURE = 3, RT_COLOROUT = 8, RT_DEPTHOUT = 9, RT_SAMPLER = 10 };
typedef struct { char *s; size_t n, cap; } Buf;
#include <stdarg.h>
static void put(Buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void put(Buf *b, const char *fmt, ...) {
    va_list a; va_start(a, fmt); int k = vsnprintf(NULL, 0, fmt, a); va_end(a);
    if (k <= 0) return;
    if (b->n + (size_t)k + 1 > b->cap) { b->cap = (b->n + (size_t)k + 1) * 2; b->s = realloc(b->s, b->cap); }
    va_start(a, fmt); vsnprintf(b->s + b->n, (size_t)k + 1, fmt, a); va_end(a); b->n += (size_t)k;
}

size_t d3d_shader_length(const uint32_t *tok, size_t max) {
    if (max < 1) return 0;
    for (size_t i = 1; i < max; ) {
        uint32_t t = tok[i];
        if (t == 0x0000FFFFu) return i + 1;
        if ((t & 0xFFFF) == 0xFFFE) { i += 1 + ((t >> 16) & 0x7FFF); continue; }   /* comment */
        uint32_t len = (t >> 24) & 0xF;
        if ((tok[0] & 0xFFFF) < 0x200) {                                             /* ps_1_x: no length field; parameters have bit 31 set */
            i++; while (i < max && (tok[i] & 0x80000000u)) i++; continue;
        }
        i += 1 + len;
    }
    return 0;
}

static int rtype(uint32_t p) { return (int)(((p >> 28) & 7) | (((p >> 11) & 3) << 3)); }
static int rnum(uint32_t p) { return (int)(p & 0x7FF); }

typedef struct { int defined[32]; float defv[32][4]; int used_const; int used_temp[32]; char err[160]; } Ctx;

/* the register as a vec4 expression */
static int reg_name(Ctx *x, uint32_t p, char *o, size_t n) {
    int t = rtype(p), i = rnum(p);
    switch (t) {
    case RT_TEMP: if (i >= 32) break; x->used_temp[i] = 1; snprintf(o, n, "r%d", i); return 1;
    case RT_CONST: if (i >= 32) break;
        if (x->defined[i]) snprintf(o, n, "c%d_", i); else { snprintf(o, n, "u_c[%d]", i); if (i + 1 > x->used_const) x->used_const = i + 1; }
        return 1;
    case RT_TEXTURE: if (i > 1) { snprintf(x->err, sizeof x->err, "texture coordinate set t%d (only t0 and t1)", i); return 0; }
        snprintf(o, n, i ? "vec4(v_uv.zw, 0.0, 1.0)" : "vec4(v_uv.xy, 0.0, 1.0)"); return 1;
    case RT_INPUT: if (i > 1) break; snprintf(o, n, i ? "v_spec" : "v_diff"); return 1;
    }
    if (!x->err[0]) snprintf(x->err, sizeof x->err, "register type %d #%d", t, i);
    return 0;
}
static const char SW[] = "xyzw";
/* a source operand: swizzle and modifier, as a vec4 expression */
static int src(Ctx *x, uint32_t p, char *o, size_t n) {
    char r[96]; if (!reg_name(x, p, r, sizeof r)) return 0;
    uint32_t s = (p >> 16) & 0xFF; char sw[6] = "."; for (int i = 0; i < 4; i++) sw[1 + i] = SW[(s >> (2 * i)) & 3]; sw[5] = 0;
    char e[128]; if (s == 0xE4) snprintf(e, sizeof e, "%s", r); else snprintf(e, sizeof e, "(%s)%s", r, sw);
    switch ((p >> 24) & 0xF) {
    case 0: snprintf(o, n, "%s", e); return 1;
    case 1: snprintf(o, n, "(-%s)", e); return 1;
    case 11: snprintf(o, n, "abs(%s)", e); return 1;
    case 12: snprintf(o, n, "(-abs(%s))", e); return 1;
    }
    snprintf(x->err, sizeof x->err, "source modifier %u", (p >> 24) & 0xF); return 0;
}

char *d3d_ps_to_glsl(const uint32_t *tok, size_t ntok, int *nconst, char *err, size_t errn) {
    Ctx x; memset(&x, 0, sizeof x); Buf body = { 0 }, out = { 0 };
    size_t len = d3d_shader_length(tok, ntok);
    #define FAIL(...) do { if (!x.err[0]) snprintf(x.err, sizeof x.err, __VA_ARGS__); goto fail; } while (0)
    if (!len) FAIL("no end token");
    if ((tok[0] & 0xFFFF0000u) != 0xFFFF0000u) FAIL("not a pixel shader (version %08x)", tok[0]);
    if ((tok[0] & 0xFFFF) < 0x200 || (tok[0] & 0xFFFF) > 0x2FF) FAIL("pixel shader version %u.%u (only 2.x)", (tok[0] >> 8) & 0xFF, tok[0] & 0xFF);
    int wrote_out = 0;
    for (size_t i = 1; i < len - 1; ) {
        uint32_t t = tok[i], op = t & 0xFFFF;
        if (op == 0xFFFE) { i += 1 + ((t >> 16) & 0x7FFF); continue; }
        uint32_t n = (t >> 24) & 0xF; const uint32_t *a = tok + i + 1; i += 1 + n;
        if (t & (1u << 28)) FAIL("predicated instruction");
        if (op == 31) continue;                                                       /* dcl: inputs and samplers are fixed */
        if (op == 0) continue;                                                        /* nop */
        if (op == 81) {                                                               /* def c#, x, y, z, w */
            int r = rnum(a[0]); if (rtype(a[0]) != RT_CONST || r >= 32) FAIL("def of a non-constant");
            x.defined[r] = 1; for (int k = 0; k < 4; k++) memcpy(&x.defv[r][k], &a[1 + k], 4);
            continue;
        }
        if (op == 65) {                                                               /* texkill */
            char s0[160]; if (!reg_name(&x, a[0], s0, sizeof s0)) goto fail;
            put(&body, "  if (any(lessThan((%s).xyz, vec3(0.0)))) discard;\n", s0); continue;
        }
        if (n < 1) FAIL("instruction %u without operands", op);
        /* destination */
        uint32_t d = a[0]; int dt = rtype(d), dn = rnum(d), wm = (int)((d >> 16) & 0xF), mod = (int)((d >> 20) & 0xF), shift = (int)((d >> 24) & 0xF);
        if (shift) FAIL("result shift");
        char dn_s[16];
        if (dt == RT_TEMP && dn < 32) { snprintf(dn_s, sizeof dn_s, "r%d", dn); x.used_temp[dn] = 1; }
        else if (dt == RT_COLOROUT && dn == 0) { snprintf(dn_s, sizeof dn_s, "oC0"); wrote_out = 1; }
        else FAIL("destination register type %d #%d", dt, dn);
        char s[3][256] = { "", "", "" };
        if (op == 66) {                                                               /* texld dst, t#, s# */
            if (rtype(a[2]) != RT_SAMPLER || rnum(a[2]) > 1) FAIL("sampler s%d (only s0 and s1)", rnum(a[2]));
            if (t & (3u << 16)) FAIL("projected or biased texld");
            char c0[200]; if (!src(&x, a[1], c0, sizeof c0)) goto fail;
            snprintf(s[0], sizeof s[0], "texture2D(u_t%d, (%s).xy)", rnum(a[2]), c0);
        } else for (uint32_t k = 1; k < n && k <= 3; k++) if (!src(&x, a[k], s[k - 1], sizeof s[k - 1])) goto fail;
        char e[1024];
        switch (op) {
        case 66: snprintf(e, sizeof e, "%s", s[0]); break;
        case 1: snprintf(e, sizeof e, "%s", s[0]); break;                                                    /* mov */
        case 2: snprintf(e, sizeof e, "%s + %s", s[0], s[1]); break;                                         /* add */
        case 3: snprintf(e, sizeof e, "%s - %s", s[0], s[1]); break;                                         /* sub */
        case 4: snprintf(e, sizeof e, "%s * %s + %s", s[0], s[1], s[2]); break;                              /* mad */
        case 5: snprintf(e, sizeof e, "%s * %s", s[0], s[1]); break;                                         /* mul */
        case 6: snprintf(e, sizeof e, "vec4(1.0 / (%s).x)", s[0]); break;                                    /* rcp */
        case 7: snprintf(e, sizeof e, "vec4(inversesqrt(abs((%s).x)))", s[0]); break;                        /* rsq */
        case 8: snprintf(e, sizeof e, "vec4(dot((%s).xyz, (%s).xyz))", s[0], s[1]); break;                   /* dp3 */
        case 9: snprintf(e, sizeof e, "vec4(dot(%s, %s))", s[0], s[1]); break;                               /* dp4 */
        case 10: snprintf(e, sizeof e, "min(%s, %s)", s[0], s[1]); break;
        case 11: snprintf(e, sizeof e, "max(%s, %s)", s[0], s[1]); break;
        case 12: snprintf(e, sizeof e, "vec4(lessThan(%s, %s))", s[0], s[1]); break;                         /* slt */
        case 13: snprintf(e, sizeof e, "vec4(greaterThanEqual(%s, %s))", s[0], s[1]); break;                 /* sge */
        case 14: snprintf(e, sizeof e, "vec4(exp2((%s).x))", s[0]); break;                                   /* exp */
        case 15: snprintf(e, sizeof e, "vec4(log2(abs((%s).x)))", s[0]); break;                              /* log */
        case 18: snprintf(e, sizeof e, "mix(%s, %s, %s)", s[2], s[1], s[0]); break;                          /* lrp: s0*s1 + (1-s0)*s2 */
        case 19: snprintf(e, sizeof e, "fract(%s)", s[0]); break;                                            /* frc */
        case 32: snprintf(e, sizeof e, "vec4(pow(abs((%s).x), (%s).x))", s[0], s[1]); break;                 /* pow */
        case 33: snprintf(e, sizeof e, "vec4(cross((%s).xyz, (%s).xyz), 0.0)", s[0], s[1]); break;          /* crs */
        case 35: snprintf(e, sizeof e, "abs(%s)", s[0]); break;                                              /* abs */
        case 36: snprintf(e, sizeof e, "vec4(normalize((%s).xyz), 0.0)", s[0]); break;                       /* nrm (w unspecified) */
        case 88: snprintf(e, sizeof e, "mix(%s, %s, vec4(greaterThanEqual(%s, vec4(0.0))))", s[2], s[1], s[0]); break;   /* cmp: s0 >= 0 ? s1 : s2 */
        case 90: snprintf(e, sizeof e, "vec4(dot((%s).xy, (%s).xy) + (%s).x)", s[0], s[1], s[2]); break;    /* dp2add */
        case 23: case 24: case 22: case 21: case 20: FAIL("matrix instruction %u", op);
        default: FAIL("instruction %u", op);
        }
        if (mod & ~3) FAIL("result modifier %d", mod);                               /* _pp (2) is only a precision hint */
        char full[1100];
        if (mod & 1) snprintf(full, sizeof full, "clamp(%s, 0.0, 1.0)", e); else snprintf(full, sizeof full, "(%s)", e);
        if (wm == 0xF) put(&body, "  %s = %s;\n", dn_s, full);
        else { char m[5]; int k = 0; for (int c = 0; c < 4; c++) if (wm >> c & 1) m[k++] = SW[c]; m[k] = 0; put(&body, "  %s.%s = (%s).%s;\n", dn_s, m, full, m); }
    }
    if (!wrote_out) FAIL("no colour output");
    put(&out, "uniform vec4 u_c[%d];\n", x.used_const ? x.used_const : 1);
    put(&out, "vec4 ps_main() {\n  vec4 oC0 = vec4(0.0);\n");
    for (int r = 0; r < 32; r++) if (x.used_temp[r]) put(&out, "  vec4 r%d = vec4(0.0);\n", r);
    for (int r = 0; r < 32; r++) if (x.defined[r]) put(&out, "  vec4 c%d_ = vec4(%.9g, %.9g, %.9g, %.9g);\n", r, x.defv[r][0], x.defv[r][1], x.defv[r][2], x.defv[r][3]);
    put(&out, "%s  return oC0;\n}\n", body.s ? body.s : "");
    free(body.s); *nconst = x.used_const;
    return out.s;
fail:
    if (err && errn) snprintf(err, errn, "%s", x.err[0] ? x.err : "unknown");
    free(body.s); free(out.s); return NULL;
    #undef FAIL
}
