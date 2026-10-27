/*
 * gl_vsh.c -- NV2A vertex microcode to GLSL.
 *
 * The translation mirrors the executor's CPU interpreter (vp_run in
 * nv2a_pb_exec.c) instruction for instruction: the same field decode (the
 * 128-bit layout documented by xemu's vsh.c), the same parallel issue -- MAC
 * and ILU read every source before either writes -- and the same output
 * registers. The two are kept deliberately alike so a picture that differs
 * between the CPU and GPU renderers points at the renderer, not at the
 * decode.
 *
 * All three transform paths end the same way. The shader produces a
 * position in real surface pixels with w still the clip w, and turns that
 * into GL clip space as (screen * w) mapped to NDC. For every path the
 * executor handles, screen*w is linear in the clip-space position, so the
 * GPU's own near-plane clipping is exact -- no CPU clipping is needed.
 *
 * Surface rows are top-first in guest memory and in the framebuffer objects
 * alike (y_ndc = 2y/H - 1), so a surface later sampled as a texture needs no
 * flip; only presentation flips.
 */
#include "gl_vsh.h"

#include <stdarg.h>
#include <stdio.h>
#include <math.h>
#include <string.h>

typedef struct {
    char  *p;
    size_t left;
    int    overflow;
} Out;

static void emit(Out *o, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (o->overflow)
        return;
    va_start(ap, fmt);
    n = vsnprintf(o->p, o->left, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= o->left) {
        o->overflow = 1;
        return;
    }
    o->p += n;
    o->left -= (size_t)n;
}

static uint32_t f(const uint32_t *t, int dw, int pos, int bits)
{
    return (t[dw] >> pos) & ((1u << bits) - 1u);
}

static const char *const s_swz = "xyzw";
extern int nv2a_shader_vk;

/* Source A (0), B (1) or C (2) as a GLSL vec4 expression. */
static void src(Out *o, const uint32_t *t, int which, int consts_local)
{
    uint32_t mux, reg, neg, sw;
    char comp[5];

    switch (which) {
    case 0:  mux = f(t, 2, 26, 2); reg = f(t, 2, 28, 4);
             neg = f(t, 1, 8, 1);  sw = f(t, 1, 0, 8);  break;
    case 1:  mux = f(t, 2, 11, 2); reg = f(t, 2, 13, 4);
             neg = f(t, 2, 25, 1); sw = f(t, 2, 17, 8); break;
    default: mux = f(t, 3, 28, 2);
             reg = (f(t, 2, 0, 2) << 2) | f(t, 3, 30, 2);
             neg = f(t, 2, 10, 1); sw = f(t, 2, 2, 8);  break;
    }
    comp[0] = s_swz[(sw >> 6) & 3];
    comp[1] = s_swz[(sw >> 4) & 3];
    comp[2] = s_swz[(sw >> 2) & 3];
    comp[3] = s_swz[sw & 3];
    comp[4] = 0;

    emit(o, "%s", neg ? "-" : "");
    if (mux == 1) {
        if (reg < 12)
            emit(o, "R%u.%s", reg, comp);
        else if (reg == 12)
            emit(o, "oPos.%s", comp);
        else
            emit(o, "vec4(0.0).%s", comp);
    } else if (mux == 2) {
        emit(o, "v%u.%s", f(t, 1, 9, 4), comp);
    } else if (mux == 3) {
        uint32_t ci = f(t, 1, 13, 8);
        const char *arr = consts_local ? "cl" : "c";
        /* Vulkan: one 192-constant set per instance (nv2a_vk instancing). */
        const char *base = nv2a_shader_vk && !consts_local ? "nv2a_ib + " : "";
        if (f(t, 3, 1, 1))
            emit(o, "%s[%sclamp(%u + a0, 0, 191)].%s", arr, base, ci, comp);
        else
            emit(o, "%s[%s%u].%s", arr, base, ci < 192 ? ci : 191, comp);
    } else {
        emit(o, "vec4(0.0).%s", comp);
    }
}

/* ".xz" for mask bits 3=x .. 0=w; empty when nothing is written. */
static void mask_str(uint32_t mask, char out[6])
{
    int i, n = 0;

    out[n++] = '.';
    for (i = 0; i < 4; i++)
        if (mask & (8u >> i))
            out[n++] = s_swz[i];
    out[n] = 0;
}

/* dst.mask = val.mask; */
static void write_masked(Out *o, const char *dst, uint32_t mask, const char *val)
{
    char m[6];

    if (!mask)
        return;
    mask_str(mask, m);
    emit(o, "        %s%s = (%s)%s;\n", dst, m, val, m);
}

static const char *const s_out_names[13] = {
    "oPos", "o1", "o2", "oD0", "oD1", "oFog", "oPts", "oB0", "oB1",
    "oT0", "oT1", "oT2", "oT3",
};

int nv2a_gl_vsh_program(const uint32_t (*prog)[4], uint32_t slots,
                        uint32_t start, char *buf, size_t cap)
{
    Out o = { buf, cap, 0 };
    uint32_t pc;
    int writes_consts = 0;

    for (pc = start; pc < slots; pc++) {
        const uint32_t *t = prog[pc];
        if (f(t, 3, 12, 4) && !f(t, 3, 11, 1))
            writes_consts = 1;
        if (f(t, 3, 0, 1))
            break;
    }

    emit(&o, "void nv2a_program(void)\n{\n");
    emit(&o, "    int a0 = 0;\n");
    if (writes_consts)
        emit(&o, "    vec4 cl[192];\n    for (int i = 0; i < 192; i++) cl[i] = c[%si];\n",
             nv2a_shader_vk ? "nv2a_ib + " : "");

    for (pc = start; pc < slots; pc++) {
        const uint32_t *t = prog[pc];
        uint32_t mac = f(t, 1, 21, 4), ilu = f(t, 1, 25, 3);
        uint32_t omask = f(t, 3, 12, 4);
        uint32_t rd = f(t, 3, 20, 4);

        /* Only what this slot reads and writes: every slot used to declare
         * and fill A, B, C, m and l whatever the operation, and the Switch's
         * Mesa spent ~70 ms a program (vertex compile 22 ms, link 35 ms)
         * parsing and optimising code it then threw away (2026-10-20). The
         * MAC reads A (all), B (two-operand ops) and C (ADD, MAD); the ILU
         * reads C. m / l exist when a write below uses them. */
        {
            int out_l = omask && f(t, 3, 2, 1);
            int ref_m = (mac && mac != 13 && rd < 13) || (omask && !out_l);
            int ref_l = (ilu && (mac ? 1u : rd) < 13) || out_l;
            int mac_op = mac >= 1 && mac <= 12;
            int need_a = (ref_m && mac_op) || mac == 13;
            int need_b = ref_m && mac_op && mac != 1 && mac != 3;
            int need_c = (ref_m && (mac == 3 || mac == 4)) || (ref_l && ilu);
            emit(&o, "    { /* %u */\n", pc);
            if (need_a) { emit(&o, "        vec4 A = "); src(&o, t, 0, writes_consts); emit(&o, ";\n"); }
            if (need_b) { emit(&o, "        vec4 B = "); src(&o, t, 1, writes_consts); emit(&o, ";\n"); }
            if (need_c) { emit(&o, "        vec4 C = "); src(&o, t, 2, writes_consts); emit(&o, ";\n"); }
            if (ref_m) {
                switch (mac_op ? mac : 0) {
                case 1:  emit(&o, "        vec4 m = A;\n"); break;
                case 2:  emit(&o, "        vec4 m = A * B;\n"); break;
                case 3:  emit(&o, "        vec4 m = A + C;\n"); break;
                case 4:  emit(&o, "        vec4 m = A * B + C;\n"); break;
                case 5:  emit(&o, "        vec4 m = vec4(dot(A.xyz, B.xyz));\n"); break;
                case 6:  emit(&o, "        vec4 m = vec4(dot(A.xyz, B.xyz) + B.w);\n"); break;
                case 7:  emit(&o, "        vec4 m = vec4(dot(A, B));\n"); break;
                case 8:  emit(&o, "        vec4 m = vec4(1.0, A.y * B.y, A.z, B.w);\n"); break;
                case 9:  emit(&o, "        vec4 m = min(A, B);\n"); break;
                case 10: emit(&o, "        vec4 m = max(A, B);\n"); break;
                case 11: emit(&o, "        vec4 m = vec4(lessThan(A, B));\n"); break;
                case 12: emit(&o, "        vec4 m = vec4(greaterThanEqual(A, B));\n"); break;
                default: emit(&o, "        vec4 m = vec4(0.0);\n"); break;
                }
            }
            if (ref_l) {
                switch (ilu) {
                case 1: emit(&o, "        vec4 l = C;\n"); break;
                case 2: emit(&o, "        vec4 l = vec4(nv2a_rcp(C.x));\n"); break;
                case 3: emit(&o, "        vec4 l = vec4(nv2a_rcc(C.x));\n"); break;
                case 4: emit(&o, "        vec4 l = vec4(nv2a_rcp(sqrt(abs(C.x))));\n"); break;
                case 5: emit(&o, "        vec4 l = nv2a_exp(C.x);\n"); break;
                case 6: emit(&o, "        vec4 l = nv2a_log(C.x);\n"); break;
                case 7: emit(&o, "        vec4 l = nv2a_lit(C);\n"); break;
                default: emit(&o, "        vec4 l = vec4(0.0);\n"); break;
                }
            }
        }

        if (mac == 13) {
            emit(&o, "        a0 = int(floor(A.x + 0.001));\n");
        } else if (mac && rd < 13) {
            char dst[8];
            if (rd == 12) snprintf(dst, sizeof dst, "oPos");
            else          snprintf(dst, sizeof dst, "R%u", rd);
            write_masked(&o, dst, f(t, 3, 24, 4), "m");
        }
        if (ilu) {
            uint32_t ld = mac ? 1 : rd;              /* paired ILU -> R1 */
            if (ld < 13) {
                char dst[8];
                if (ld == 12) snprintf(dst, sizeof dst, "oPos");
                else          snprintf(dst, sizeof dst, "R%u", ld);
                write_masked(&o, dst, f(t, 3, 16, 4), "l");
            }
        }
        if (omask) {
            const char *val = f(t, 3, 2, 1) ? "l" : "m";
            uint32_t addr = f(t, 3, 3, 8);
            if (f(t, 3, 11, 1)) {
                if (addr < 13)
                    write_masked(&o, s_out_names[addr], omask, val);
            } else {
                char dst[48];
                if (f(t, 3, 1, 1))
                    snprintf(dst, sizeof dst, "cl[clamp(%u + a0, 0, 191)]", addr);
                else
                    snprintf(dst, sizeof dst, "cl[%u]", addr < 192 ? addr : 191);
                write_masked(&o, dst, omask, val);
            }
        }
        emit(&o, "    }\n");
        if (f(t, 3, 0, 1))                          /* FINAL */
            break;
    }
    emit(&o, "}\n");
    return o.overflow ? -1 : (int)(cap - o.left);
}

/* The NV2A rasterizer takes screen positions as fixed point with a 4-bit
 * fraction, truncated (xemu's roundScreenCoords). Xbox D3D's viewport
 * offset carries a +0.53125 bias, so a clip-space full-screen pass starts at
 * 0.53125 -- 0.5 on the hardware, which covers row and column 0; unsnapped,
 * GL left them out and NFSU2's glow buffer kept a stale edge that its
 * composite added back as a light line at the top and left.
 * Under a render scale k (RECOMP_GL_SCALE) a title pixel is k x k real
 * pixels, and the first real centre of title pixel i sits at i + 0.5/k, not
 * i + 0.5: an edge snapped to 0.5 still missed real row/column 0. u_surf.w =
 * 0.5 - 0.5/k moves positions so that real centre lands on i + 0.5 (0 at
 * k = 1).
 * With square pixels (nv2a_gl RECOMP_GL_SQUARE) the two axes scale apart,
 * so the GL path takes the y offset from u_snapy; Vulkan keeps one. */
#define NV2A_SNAP_GLSL_OFF(off) \
    "vec2 nv2a_snap(vec2 sw, float w) {\n" \
    "    return w > 0.0 ? (trunc(sw / w * 16.0) / 16.0 - " off ") * w : sw;\n" \
    "}\n"
#define NV2A_SNAP_GLSL NV2A_SNAP_GLSL_OFF("vec2(u_surf.w)")

/* Everything around the program body: inputs, the register file, the ILU
 * helpers (same constants as the interpreter), and the three position
 * paths. u_xform selects the path at run time so one program object serves
 * a vertex program under any surface. */
/* Fixed-function fog inputs (see nv2a_ffp_fog below); in Vulkan they are
 * members of the VsU block instead. */
#define NV2A_FFP_FOG_DECL \
    "uniform vec4 u_mv[4 * 16]; /* model-view rows (fog distance), per draw */\n" \
    "uniform vec4 u_fogplane;\n" \
    "uniform vec4 u_fog0;      /* enabled, gen, equation, abs */\n" \
    "uniform vec4 u_fog1;      /* param0, param1 */\n"

/* Vulkan GLSL (nv2a_vk): the same shader with explicit locations, the
 * uniforms in a std140 block (binding 0) and Vulkan's 0..1 clip depth. */
static const char s_vk_prelude[] =
    "#version 450\n"
    "layout(location = 0) in vec4 v0;\n"
    "layout(location = 1) in vec4 v1;\n"
    "layout(location = 2) in vec4 v2;\n"
    "layout(location = 3) in vec4 v3;\n"
    "layout(location = 4) in vec4 v4;\n"
    "layout(location = 5) in vec4 v5;\n"
    "layout(location = 6) in vec4 v6;\n"
    "layout(location = 7) in vec4 v7;\n"
    "layout(location = 8) in vec4 v8;\n"
    "layout(location = 9) in vec4 v9;\n"
    "layout(location = 10) in vec4 v10;\n"
    "layout(location = 11) in vec4 v11;\n"
    "layout(location = 12) in vec4 v12;\n"
    "layout(location = 13) in vec4 v13;\n"
    "layout(location = 14) in vec4 v14;\n"
    "layout(location = 15) in vec4 v15;\n"
    "layout(std140, set = 0, binding = 0) uniform VsU {\n"
    "    vec4 u_surf;\n"
    "    vec4 u_m[4];\n"
    "    vec4 u_vpoff;\n"
    "    vec2 u_aa;\n"
    "    int u_xform;\n"
    "    vec4 u_mv[4];\n"
    "    vec4 u_fogplane;\n"
    "    vec4 u_fog0;\n"
    "    vec4 u_fog1;\n"
    "};\n"
    /* Transform constants, 192 per instance: an instanced draw (nv2a_vk.c)
     * repeats one mesh with each instance's own set. NV2A_VK_INSTANCES. */
    "layout(std140, set = 0, binding = 6) uniform VsC { vec4 c[192 * 16]; };\n"
    "int nv2a_ib;\n"
    "int nv2a_di;\n"
    "int nv2a_draw_index() { return 0; }\n"
    "layout(location = 0) out vec4 vD0; layout(location = 1) out vec4 vD1;\n"
    "layout(location = 2) out vec4 vT0; layout(location = 3) out vec4 vT1;\n"
    "layout(location = 4) out vec4 vT2; layout(location = 5) out vec4 vT3;\n"
    "layout(location = 6) out float vFog;\n"
    "vec4 R0, R1, R2, R3, R4, R5, R6, R7, R8, R9, R10, R11;\n"
    "vec4 oPos, o1, o2, oD0, oD1, oFog, oPts, oB0, oB1, oT0, oT1, oT2, oT3;\n"
    "float nv2a_rcp(float x) { return x != 0.0 ? 1.0 / x : 1.884467e+19; }\n"
    "float nv2a_fog_eq(float d) {             /* FOG_MODE on a fog distance */\n"
    "    if (u_fog0.w != 0.0) d = abs(d);\n"
    "    int eq = int(u_fog0.z);\n"
    "    if (eq == 0) return u_fog1.x + d * u_fog1.y - 1.0;\n"
    "    if (eq == 1) return u_fog1.x + exp2(d * u_fog1.y * 16.0) - 1.5;\n"
    "    return u_fog1.x + exp2(-d * d * u_fog1.y * u_fog1.y * 32.0) - 1.5;\n"
    "}\n"
    "float nv2a_rcc(float x) {\n"
    "    float y = nv2a_rcp(x); float a = clamp(abs(y), 5.42101e-20, 1.884467e+19);\n"
    "    return y < 0.0 ? -a : a;\n"
    "}\n"
    "vec4 nv2a_exp(float x) { float f = floor(x); return vec4(exp2(f), x - f, exp2(x), 1.0); }\n"
    "vec4 nv2a_log(float x) {\n"
    "    float a = abs(x);\n"
    "    if (a == 0.0) return vec4(-1.884467e+19, 1.0, -1.884467e+19, 1.0);\n"
    "    float e = floor(log2(a));\n"
    "    return vec4(e, a / exp2(e), log2(a), 1.0);\n"
    "}\n"
    "vec4 nv2a_lit(vec4 s) {\n"
    "    float lx = max(s.x, 0.0), ly = max(s.y, 0.0), w = clamp(s.w, -127.996, 127.996);\n"
    "    return vec4(1.0, lx, (lx > 0.0 && ly > 0.0) ? exp2(w * log2(ly)) : 0.0, 1.0);\n"
    "}\n"
    /* z clamped only in front of the eye (w > 0). Behind it (w < 0) the
     * vertex is clipped by x/y anyway and its true z is about w: clamping it
     * to 0 skewed the interpolated depth of triangles crossing the eye plane
     * (walls in hood view turned transparent, objects showed through them). */
    NV2A_SNAP_GLSL
    "vec4 nv2a_clip(vec3 sw, float w) {\n"
    "    float z = sw.z * u_surf.z;\n"
    "    sw.xy = nv2a_snap(sw.xy, w);\n"
    "    return vec4(sw.x * u_surf.x - w, sw.y * u_surf.y - w,\n"
    "                w > 0.0 ? clamp(z, 0.0, w) : z, w);\n"
    "}\n";

int nv2a_shader_vk;

const char *nv2a_gl_vsh_prelude(void)
{
    if (nv2a_shader_vk)
        return s_vk_prelude;
    return
        "#version 330 core\n"
        "layout(location = 0) in vec4 v0;\n"
        "layout(location = 1) in vec4 v1;\n"
        "layout(location = 2) in vec4 v2;\n"
        "layout(location = 3) in vec4 v3;\n"
        "layout(location = 4) in vec4 v4;\n"
        "layout(location = 5) in vec4 v5;\n"
        "layout(location = 6) in vec4 v6;\n"
        "layout(location = 7) in vec4 v7;\n"
        "layout(location = 8) in vec4 v8;\n"
        "layout(location = 9) in vec4 v9;\n"
        "layout(location = 10) in vec4 v10;\n"
        "layout(location = 11) in vec4 v11;\n"
        "layout(location = 12) in vec4 v12;\n"
        "layout(location = 13) in vec4 v13;\n"
        "layout(location = 14) in vec4 v14;\n"
        "layout(location = 15) in vec4 v15;\n"
        "uniform vec4 c[192];\n"
        "uniform vec4 u_surf;      /* 2/W, 2/H, 1/zmax, scale offset */\n"
        "uniform float u_snapy;    /* scale offset, y (square pixels) */\n"
        "uniform vec2 u_aa;        /* logical -> real pixels */\n"
        "uniform vec4 u_m[4 * 16]; /* fixed-function composite rows, per draw */\n"
        /* Draws merged into one call (nv2a_gl.c, merge_flush) each with its
         * own matrices: draw i's vertices end at index u_dend[i], its rows
         * are u_m / u_mv [4i .. 4i+3]. One draw: u_dend[0] is past every
         * index, set 0. */
        "uniform int u_dend[16];\n"
        "int nv2a_di;\n"
        "int nv2a_draw_index() {\n"
        "    int i = 0;\n"
        "    while (i < 15 && gl_VertexID >= u_dend[i]) i++;\n"
        "    return i * 4;\n"
        "}\n"
        "uniform vec4 u_vpoff;     /* fixed-function viewport offset */\n"
        NV2A_FFP_FOG_DECL
        "out vec4 vD0; out vec4 vD1; out vec4 vT0; out vec4 vT1;\n"
        "out vec4 vT2; out vec4 vT3; out float vFog;\n"
        "vec4 R0, R1, R2, R3, R4, R5, R6, R7, R8, R9, R10, R11;\n"
        "vec4 oPos, o1, o2, oD0, oD1, oFog, oPts, oB0, oB1, oT0, oT1, oT2, oT3;\n"
        "float nv2a_rcp(float x) { return x != 0.0 ? 1.0 / x : 1.884467e+19; }\n"
    "float nv2a_fog_eq(float d) {             /* FOG_MODE on a fog distance */\n"
    "    if (u_fog0.w != 0.0) d = abs(d);\n"
    "    int eq = int(u_fog0.z);\n"
    "    if (eq == 0) return u_fog1.x + d * u_fog1.y - 1.0;\n"
    "    if (eq == 1) return u_fog1.x + exp2(d * u_fog1.y * 16.0) - 1.5;\n"
    "    return u_fog1.x + exp2(-d * d * u_fog1.y * u_fog1.y * 32.0) - 1.5;\n"
    "}\n"
        "float nv2a_rcc(float x) {\n"
        "    float y = nv2a_rcp(x); float a = clamp(abs(y), 5.42101e-20, 1.884467e+19);\n"
        "    return y < 0.0 ? -a : a;\n"
        "}\n"
        "vec4 nv2a_exp(float x) { float f = floor(x); return vec4(exp2(f), x - f, exp2(x), 1.0); }\n"
        "vec4 nv2a_log(float x) {\n"
        "    float a = abs(x);\n"
        "    if (a == 0.0) return vec4(-1.884467e+19, 1.0, -1.884467e+19, 1.0);\n"
        "    float e = floor(log2(a));\n"
        "    return vec4(e, a / exp2(e), log2(a), 1.0);\n"
        "}\n"
        "vec4 nv2a_lit(vec4 s) {\n"
        "    float lx = max(s.x, 0.0), ly = max(s.y, 0.0), w = clamp(s.w, -127.996, 127.996);\n"
        "    return vec4(1.0, lx, (lx > 0.0 && ly > 0.0) ? exp2(w * log2(ly)) : 0.0, 1.0);\n"
        "}\n"
        "/* screen*w (real pixels) and clip w to GL clip space. */\n"
        /* z clamped to the clip volume: the NV2A clamps depth rather than
         * clipping at the near and far planes, and GL_DEPTH_CLAMP alone did
         * not reach every driver -- on Eden NFSU2's loading screen, drawn at
         * z = 1.0, still vanished. */
        NV2A_SNAP_GLSL_OFF("vec2(u_surf.w, u_snapy)")
        "vec4 nv2a_clip(vec3 sw, float w) {\n"
        "    float z = sw.z * u_surf.z * 2.0 - w;\n"
        "    sw.xy = nv2a_snap(sw.xy, w);\n"
        "    return vec4(sw.x * u_surf.x - w, sw.y * u_surf.y - w,\n"
        "                clamp(z, -abs(w), abs(w)), w);\n"
        "}\n";
}

#define NV2A_VSH_MAIN(set_ib) \
    "void main() {\n" \
    "    R0 = R1 = R2 = R3 = R4 = R5 = R6 = R7 = R8 = R9 = R10 = R11 = vec4(0.0);\n" \
    "    oPos = o1 = o2 = oFog = oPts = vec4(0.0);\n" \
    "    oD0 = oD1 = oB0 = oB1 = vec4(0.0, 0.0, 0.0, 1.0);\n" \
    "    oT0 = oT1 = oT2 = oT3 = vec4(0.0, 0.0, 0.0, 1.0);\n" \
    set_ib \
    "    nv2a_program();\n" \
    "    float w = abs(oPos.w) < 1e-6 ? 1e-6 : oPos.w;\n" \
    "    vec3 s = vec3(oPos.xy * u_aa, oPos.z);\n" \
    "    gl_Position = nv2a_clip(s * w, w);\n" \
    "    vD0 = clamp(oD0, 0.0, 1.0); vD1 = clamp(oD1, 0.0, 1.0);\n" \
    "    vT0 = oT0; vT1 = oT1; vT2 = oT2; vT3 = oT3;\n" \
    "    vFog = u_fog0.x == 0.0 ? 1.0 : nv2a_fog_eq(oFog.x);\n" \
    "}\n"

const char *nv2a_gl_vsh_main_program(void)
{
    if (nv2a_shader_vk)
        return NV2A_VSH_MAIN("    nv2a_ib = gl_InstanceIndex * 192;\n");
    return NV2A_VSH_MAIN("");
}

/* Fixed-function fog.
 *
 * The fixed-function shader used to end with vFog = 0.0. The pixel shader's
 * final combiner blends with FOG.a = vFog, and D3D's fog blend is
 * fog.a * colour + (1 - fog.a) * fog colour -- so every fixed-function draw
 * with fog on came out as the plain fog colour. Midnight Club 3 draws its
 * city that way: untextured dark grey buildings under visible lights and sky
 * (2026-10-16). Now the NV2A's fog unit: a distance by FOG_GEN_MODE (0x2A0),
 * then FOG_MODE's equation (0x29C) with FOG_PARAMS (0x9C0); off (0x2A4 = 0)
 * is factor 1, no fog.
 *
 * ponytail: the equations' biases (-1 linear, -1.5 exp/exp2) and the
 * constants 16 and 32 are xemu's reading of the hardware; specular-alpha fog
 * is taken as the factor itself, which is what D3D means by it.
 *
 * Vertex programs: the fog unit runs on their oFog.x the same way (xemu
 * does likewise), and fog off is factor 1. Passing oFog.x through as the
 * factor turned every program that writes no fog into 100% fog colour:
 * Midnight Club 3's blur pass came out as the fog grey and was blended over
 * the whole screen, the in-game street went dark and the light glow
 * vanished (2026-10-16). */
#define NV2A_FFP_FOG_GLSL \
    "float nv2a_ffp_fog() {\n" \
    "    if (u_fog0.x == 0.0) return 1.0;\n" \
    "    int gen = int(u_fog0.y);\n" \
    "    if (gen == 0) return v4.a;                       /* SPEC_ALPHA */\n" \
    "    vec3 eye = vec3(dot(u_mv[nv2a_di], v0), dot(u_mv[nv2a_di + 1], v0),\n" \
    "                    dot(u_mv[nv2a_di + 2], v0));\n" \
    "    float d;\n" \
    "    if (gen == 1) d = length(eye);                   /* RADIAL */\n" \
    "    else if (gen == 6) d = v5.x;                     /* FOG_X */\n" \
    "    else d = dot(u_fogplane.xyz, eye) + u_fogplane.w; /* PLANAR */\n" \
    "    if (gen == 3) d = abs(d);\n" \
    "    return nv2a_fog_eq(d);\n" \
    "}\n"

static float reg_float(const uint32_t *r, uint32_t method)
{
    float f;
    memcpy(&f, &r[method / 4], 4);
    return f;
}

void nv2a_ffp_fog_uniforms(const uint32_t *r, float mv[16], float plane[4],
                           float fog0[4], float fog1[4])
{
    uint32_t mode = r[0x29C / 4], i;

    for (i = 0; i < 16; i++)
        mv[i] = reg_float(r, 0x480 + i * 4);
    for (i = 0; i < 4; i++)
        plane[i] = reg_float(r, 0x9D0 + i * 4);
    fog0[0] = r[0x2A4 / 4] ? 1.0f : 0.0f;
    fog0[1] = (float)(r[0x2A0 / 4] & 7);
    switch (mode) {
    case 0x0800: case 0x0802: fog0[2] = 1.0f; break;    /* EXP, EXP_ABS */
    case 0x0801: case 0x0803: fog0[2] = 2.0f; break;    /* EXP2, EXP2_ABS */
    default:                  fog0[2] = 0.0f; break;    /* LINEAR, LINEAR_ABS */
    }
    fog0[3] = (mode == 0x0802 || mode == 0x0803 || mode == 0x0804) ? 1.0f : 0.0f;
    fog1[0] = reg_float(r, 0x9C0);
    fog1[1] = reg_float(r, 0x9C4);
    fog1[2] = fog1[3] = 0.0f;
}

float nv2a_ffp_fog_factor(const uint32_t *r, const float pos[4], float spec_a)
{
    float mv[16], plane[4], f0[4], f1[4], eye[3], d;
    int i, gen;

    nv2a_ffp_fog_uniforms(r, mv, plane, f0, f1);
    if (f0[0] == 0.0f)
        return 1.0f;
    gen = (int)f0[1];
    if (gen == 0)
        return spec_a;
    for (i = 0; i < 3; i++)
        eye[i] = mv[4 * i] * pos[0] + mv[4 * i + 1] * pos[1] + mv[4 * i + 2] * pos[2]
               + mv[4 * i + 3] * pos[3];
    if (gen == 1)
        d = sqrtf(eye[0] * eye[0] + eye[1] * eye[1] + eye[2] * eye[2]);
    else
        d = plane[0] * eye[0] + plane[1] * eye[1] + plane[2] * eye[2] + plane[3];
    if (gen == 3 || f0[3] != 0.0f)
        d = fabsf(d);
    if (f0[2] == 0.0f)
        return f1[0] + d * f1[1] - 1.0f;
    if (f0[2] == 1.0f)
        return f1[0] + exp2f(d * f1[1] * 16.0f) - 1.5f;
    return f1[0] + exp2f(-d * d * f1[1] * f1[1] * 32.0f) - 1.5f;
}

const char *nv2a_gl_vsh_fixed(void)
{
    /* Vulkan: these uniforms are members of the prelude's uniform block. */
    static const char decl[] =
        "uniform int u_xform;      /* 0 pre-transformed, 1 fixed-function */\n";
    static const char body[] =
        "uniform int u_xform;      /* 0 pre-transformed, 1 fixed-function */\n"
        NV2A_FFP_FOG_GLSL
        "void main() {\n"
        "    nv2a_di = nv2a_draw_index();\n"
        "    if (u_xform == 1) {\n"
        "        vec4 clip = vec4(dot(u_m[nv2a_di], v0), dot(u_m[nv2a_di + 1], v0),\n"
        "                         dot(u_m[nv2a_di + 2], v0), dot(u_m[nv2a_di + 3], v0));\n"
        "        float w = abs(clip.w) < 1e-6 ? 1e-6 : clip.w;\n"
        "        vec3 sw = clip.xyz + u_vpoff.xyz * w;\n"
        "        sw.xy *= u_aa;\n"
        "        gl_Position = nv2a_clip(sw, w);\n"
        "    } else {\n"
        "        float w = (v0.w > 0.0) ? 1.0 / v0.w : 1.0;\n"
        "        gl_Position = nv2a_clip(v0.xyz * w, w);\n"
        "    }\n"
        "    vD0 = clamp(v3, 0.0, 1.0); vD1 = clamp(v4, 0.0, 1.0);\n"
        "    vT0 = v9; vT1 = v10; vT2 = v11; vT3 = v12; vFog = nv2a_ffp_fog();\n"
        "}\n";
    return nv2a_shader_vk ? body + sizeof decl - 1 : body;
}
