/* d3d9_shader.h: Direct3D 9 pixel shader bytecode (ps_2_0 and below) to a GLSL fragment function.
 *
 * The output defines `vec4 ps_main()` against the interface plat.h describes for PlatShader (the varyings v_diff,
 * v_spec, v_uv, the samplers u_t0 / u_t1 and the constant array u_c). Only what the translation can express exactly
 * is accepted; anything else (texture coordinate sets above 1, samplers above 1, depth output, flow control, ps_3_0...)
 * makes it fail with a reason, and the front end then reports the shader as unsupported, as it did before. */
#ifndef D3D9_SHADER_H
#define D3D9_SHADER_H
#include <stddef.h>
#include <stdint.h>
/* tok: the shader's tokens (from the version token to the end token, at most ntok); returns malloc'ed GLSL or NULL with
 * a reason in err. *nconst: how many float constants (c0..) the shader reads from u_c. */
char *d3d_ps_to_glsl(const uint32_t *tok, size_t ntok, int *nconst, char *err, size_t errn);
/* the number of tokens of a shader starting at tok (through the end token), or 0 if there is none within max */
size_t d3d_shader_length(const uint32_t *tok, size_t max);
#endif
