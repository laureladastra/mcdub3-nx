/* gl_vsh.h -- NV2A vertex transform as GLSL (see gl_vsh.c). */
#ifndef NV2A_GL_VSH_H
#define NV2A_GL_VSH_H

#include <stddef.h>
#include <stdint.h>

/* The body of a vertex program, as `void nv2a_program(void)`, from slot
 * `start` to the FINAL instruction. Returns bytes written, -1 on overflow. */
int nv2a_gl_vsh_program(const uint32_t (*prog)[4], uint32_t slots,
                        uint32_t start, char *buf, size_t cap);

/* Nonzero: emit Vulkan GLSL (nv2a_vk). Set once, before the first shader. */
extern int nv2a_shader_vk;

/* Declarations and helpers every vertex shader starts with. */
const char *nv2a_gl_vsh_prelude(void);
int nv2a_gl_vsh_cubo(void);             /* constants in a uniform block (RECOMP_GL_CUBO) */
void nv2a_gl_vsh_cubo_set(int ok);      /* the renderer can stream them (at start) */
/* main() for a vertex-program shader (follows the program body). */
const char *nv2a_gl_vsh_main_program(void);
/* main() for fixed-function / pre-transformed batches (no program body). */
const char *nv2a_gl_vsh_fixed(void);

/* Fixed-function fog (NV097 FOG_*), for nv2a_gl_vsh_fixed's uniforms: the
 * model-view rows (u_mv), the fog plane, u_fog0 = (enabled, gen mode,
 * equation: 0 linear 1 exp 2 exp2, abs) and u_fog1 = (param0, param1, 0, 0).
 * Shared by nv2a_gl and nv2a_vk so both read the registers alike. */
void nv2a_ffp_fog_uniforms(const uint32_t *r, float mv[16], float plane[4],
                           float fog0[4], float fog1[4]);
/* The same factor on the CPU for one object-space position and specular
 * alpha (diagnostics: RECOMP_GL_WATCH). */
float nv2a_ffp_fog_factor(const uint32_t *r, const float pos[4], float spec_a);

#endif
