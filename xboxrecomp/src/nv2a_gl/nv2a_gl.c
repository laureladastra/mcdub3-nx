/*
 * nv2a_gl.c -- an OpenGL 3.3 core renderer for the pushbuffer executor.
 *
 * Registered as the executor's back end (nv2a_backend.h, draw_raw). The
 * executor decodes the pushbuffer, gathers vertices and keeps the method
 * shadow; this file turns that state into GL:
 *
 *   surfaces   one framebuffer object per colour-surface address, rows
 *              top-first like guest memory. A texture whose address is a
 *              surface samples the FBO directly -- render-to-texture never
 *              round-trips through guest memory.
 *   textures   decoded from guest memory by the executor's own decoder
 *              (swizzled, linear, DXT), cached by address and format, and
 *              re-uploaded when a sampled hash of the bytes changes.
 *   shaders    vertex programs and the fixed-function transform (gl_vsh.c),
 *              register combiners and texture modes (gl_psh.c), linked in
 *              pairs and cached by what generated them.
 *   state      blend, depth, stencil, cull and colour mask straight from the
 *              method shadow; alpha test in the fragment shader.
 *
 * Everything runs on the executor's thread, which is where the GL context is
 * created (lazily, on the first call) and where the window is presented and
 * its events pumped, on flip.
 *
 * Runtime switches:
 *   RECOMP_GL_DUMP=<prefix>[,every[,from[,to]]]  write presented frames as BMP
 *   RECOMP_GL_TRACE=1                shader sources and link errors
 *   RECOMP_GL_WATCH=<hex va>         state, vertices and pixels read back for
 *                                    the first draws into / sampling that VA
 *   RECOMP_GL_SCALE=<k>              render surfaces at k times the title's
 *                                    resolution, fractions allowed (1.5;
 *                                    0.5..4, default 1)
 *   RECOMP_GL_SHARED_Z=0             depth buffer per colour surface instead of
 *                                    per zeta address (the old behaviour)
 */
#include "nv2a_gl.h"
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir(p, mode) _mkdir(p)          /* Windows: no mode argument */
#endif
#include <unistd.h>
#include "gl_api.h"
#include "gl_psh.h"
#include "gl_vsh.h"
#include "../platform/host_main.h"
#include "hd_pack.h"
#include "../kernel/nv2a_present.h"
#include "../kernel/nv2a_backend.h"

#include <SDL.h>

#if (!defined(_WIN32) || defined(XBOXRECOMP_SDL_HOST)) && !defined(__SWITCH__)
void xbox_sdl_keyboard_handle(const SDL_Event *e);   /* input/sdl_keyboard.c */
int  xbox_sdl_keyboard_down(int scancode);
#endif
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern ptrdiff_t xbox_GetMemoryOffset(void);

int nv2a_gl_adopt_window(void **win, void **ctx);
int nv2a_gl_draw_placeholder(void);
int nv2a_gl_screen_size(int *w, int *h);
static SDL_Window   *s_win;
static SDL_GLContext s_ctx;
static int           s_state;           /* 0 untried, 1 ready, -1 failed */
static int           s_trace;
static int           s_finish = -1;      /* RECOMP_GL_FINISH */

static int           s_new_obj;          /* this operation uses something new */

/* RECOMP_GL_FINISH: wait for the GPU after every operation, so a hard GPU
 * hang happens at the operation that causes it. Only operations that touch
 * something new (a shader just compiled, a surface or texture just made)
 * are logged -- the rest is thousands of lines a frame, far too slow for a
 * synchronous log on an SD card. */
static uint32_t      s_frame;

/* RECOMP_GL_PROBE=1: where does the picture get lost? Clears are red, every
 * draw writes green (gl_psh.c), and a blue square goes straight into the
 * window before each swap. Black window: nothing reaches the screen; blue
 * only: nothing reaches the title's surfaces; red: clears do, draws not;
 * green: draws do, so their inputs (textures) are what is missing. For the
 * first Windows runs on Parallels' GL, black throughout (2026-10-23). */
static int gl_probe(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_GL_PROBE");
        on = e && *e == '1';
        if (on)
            fprintf(stderr, "  [GL] probe: clears red, draws green, a blue square in the window (RECOMP_GL_PROBE)\n");
    }
    return on;
}
static void gl_step_done(const char *what)
{
    int upload = what[0] == 't';          /* "texture upload": part of a draw */
    {
        /* RECOMP_GL_ERRORS=1: the first GL errors, by the step that raised
         * them -- for a driver that refuses what others accept (Parallels'
         * GL on Metal drew nothing on the first Windows run, 2026-10-23). */
        static int on = -1;
        static unsigned reported;
        if (on < 0)
            on = getenv("RECOMP_GL_ERRORS") != NULL;
        if (on) {
            GLenum err;
            while ((err = glGetError()) != GL_NO_ERROR)
                if (reported++ < 40)
                    fprintf(stderr, "  [GL] error 0x%04X after %s (frame %u)\n", err, what, s_frame);
        }
    }
    if (s_finish < 0)
        s_finish = getenv("RECOMP_GL_FINISH") != NULL;
    if (s_finish) {
        glFinish();
        if (s_new_obj)
            fprintf(stderr, "  [GL] %s done\n", what);
        else if (what[0] == 's') {        /* "swap": one line a frame, a heartbeat */
            static unsigned swaps;
            fprintf(stderr, "  [GL] frame %u swapped\n", ++swaps);
        }
    }
    if (!upload)
        s_new_obj = 0;                    /* the draw that uses it reports too */
}
static GLuint        s_vao, s_vbo, s_ibo;
/* Streaming vertex / index buffers (see ring_alloc). */
#define RING_SEGS 4
typedef struct {
    GLuint buf; GLenum target; GLsizeiptr cap, off;
    uint8_t *pmap;              /* persistent mapping (ring_persist), or NULL */
    GLsync fence[RING_SEGS];    /* per quarter: the GPU's last use of it */
    int seg;                    /* the quarter being filled */
} GlRing;
static GlRing s_vring = { 0, GL_ARRAY_BUFFER, 16 << 20, 0 };
static GlRing s_iring = { 0, GL_ELEMENT_ARRAY_BUFFER, 4 << 20, 0 };
static int ring_persist(GlRing *g);
static const uint32_t *s_regs;          /* the executor's method shadow */
/* Hitch accounting (gl_flip logs frames over 50 ms): per frame, programs
 * compiled and textures uploaded, and the time spent on each. */
static uint32_t s_hz_progs, s_hz_texs, s_hz_draws;
static uint64_t s_hz_prog_ns, s_hz_tex_ns, s_hz_tex_bytes;
/* Of the textures' time: GL's own upload calls, and the mip levels
 * (glGenerateMipmap, or the title's levels uploaded); the rest is decoding
 * and hashing. Per format, how many were made. For the race start's
 * hundreds of uploads (2026-10-26). */
static uint64_t s_hz_gl_ns, s_hz_mip_ns;
static uint16_t s_hz_fmt[64];

/* Where the GL thread's time goes, per phase of a draw (and the swap), for
 * the perf report (nv2a_gl_prof_stats; on with MC3_PERF or RECOMP_GL_PROF).
 * On the Switch a race is ~2,400 draws a frame and the GL thread is the
 * limit (idle 0%, 2026-10-25): this says which part of a draw costs. */
enum { PF_SURF, PF_PROG, PF_TEX, PF_UNIF, PF_STATE, PF_VTX, PF_DRAW, PF_SWAP, PF_N };
static const char *const s_pf_name[PF_N] = {
    "surface", "program", "textures", "uniforms", "state", "vertices", "draw", "swap" };
static int s_pf_on = -1;
static uint64_t s_pf_ns[PF_N], s_pf_t;
static uint32_t s_pf_draws, s_pf_frames, s_pf_verts, s_pf_idx;
static uint32_t s_pf_cdraws, s_pf_crows, s_pf_udraws;   /* constants / other uniforms changed */
/* Draws that changed no GL state at all since the previous one -- same
 * surface, program, textures and samplers, state, uniforms and constants,
 * vertex layout and primitive: the ones a merge of consecutive draws could
 * fold into their predecessor (s_draw_changed is set wherever a draw
 * changes something). Measured before building that merge. */
static int s_draw_changed;
static uint32_t s_pf_same;
static uint64_t hz_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static inline void pf_start(void)
{
    if (s_pf_on < 0)
        s_pf_on = getenv("MC3_PERF") != NULL || getenv("RECOMP_GL_PROF") != NULL;
    if (s_pf_on)
        s_pf_t = hz_now();
}

static inline void pf_mark(int phase)
{
    if (s_pf_on > 0) {
        uint64_t now = hz_now();
        s_pf_ns[phase] += now - s_pf_t;
        s_pf_t = now;
    }
}
static GLuint        s_cur_prog;         /* what glUseProgram last got */
static void state_dirty(void);
/* Last sampler state set on a GL texture (bind_stage). */
typedef struct { GLuint id; uint32_t key; } TexParam;
#define TEX_PARAM_SLOTS 4096
static TexParam s_tex_param[TEX_PARAM_SLOTS];
static void tex_param_forget(GLuint id)
{
    TexParam *tp = &s_tex_param[id % TEX_PARAM_SLOTS];
    if (tp->id == id)
        tp->id = 0;
}

/* Texture bindings per unit, so a draw rebinds only what changed. -1: not
 * known (after anything outside the draw path may have bound textures). */
static int    s_active_unit = -1;
static GLuint s_bound[4];
static int    s_bound_known;
static void tex_bind_forget(void) { s_active_unit = -1; s_bound_known = 0; }
static void active_unit(int i)
{
    if (s_active_unit != i) {
        glActiveTexture(GL_TEXTURE0 + (GLenum)i);
        s_active_unit = i;
    }
}
/* Bind on the active unit (known after active_unit). */
static void bind_tex(GLuint tex)
{
    int u = s_active_unit;
    if (u >= 0 && u < 4 && (s_bound_known & (1 << u)) && s_bound[u] == tex)
        return;
    s_draw_changed = 1;
    glBindTexture(GL_TEXTURE_2D, tex);
    if (u >= 0 && u < 4) {
        s_bound[u] = tex;
        s_bound_known |= 1 << u;
    }
}

/* ── Surfaces ──────────────────────────────────────────────────────── */

/* w, h are the surface in the title's pixels (anti-aliasing included) --
 * what the shaders, clip rectangles and texture lookups work in. pw, ph are
 * the GL storage: w, h times the render scale. */
typedef struct {
    uint32_t va, w, h, pw, ph;
    uint32_t vw;                        /* drawn width (clip), <= w: what is shown */
    uint32_t vw_frame;                  /* frame vw was last set in */
    GLuint   fbo, tex, ds;
    GLuint   ds_on;                     /* depth renderbuffer attached now */
    uint32_t used;
    uint32_t aa_sx, aa_sy;
} GlSurf;

/* Depth buffers belong to the title's zeta address, not to a colour surface:
 * NFSU2's light flares (eRenderLightFlares, sub_000AC560) switch colour
 * target but keep the scene's Z buffer, and with depth per colour surface
 * they tested against an empty buffer -- car lights, neon and street lamps
 * showed through walls and cars. Keyed by (zeta, stored size); a zeta used
 * with a colour surface of another size gets a buffer of its own. */
typedef struct {
    uint32_t va, pw, ph;
    GLuint   rb;
    uint32_t used;
    /* Sampled as a colour texture (depth_as_color): the surface it was last
     * used with (logical size, anti-aliasing), when it was last bound for
     * drawing or clearing, and the converted copy as of then. */
    uint32_t lw, lh, ax, ay, wseq, cseq, cfmt;
    GLuint   dtex, rgba, fbo_a, fbo_b;
} GlDepth;
static GlDepth *s_depth_cur;            /* what depth_get returned last */
static uint32_t s_zseq;

#define GL_MAX_DEPTH 16
static GlDepth s_depth[GL_MAX_DEPTH];
static int     s_shared_z = -1;         /* RECOMP_GL_SHARED_Z=0: old behaviour */

/* RECOMP_GL_SCALE: render resolution multiple (0.5..4, fractions allowed).
 * Everything the title sees stays at its own size; only the pixels behind a
 * surface grow, so the viewport, clear rectangles, read-backs and the
 * present blit scale and nothing else does. Capped per surface by the
 * driver's size limit. */
static double s_scale = 1.0;

/* RECOMP_GL_SQUARE (on by default in 16:9): square pixels. A title told the
 * TV is widescreen still renders 640 or 720 x 480 and the TV stretches it,
 * so every pixel is wider than tall and stretching it to 16:9 throws away
 * horizontal detail -- at 1.5x MC3's race was 1080 stored columns shown
 * 1280 wide. Screen-sized surfaces without horizontal anti-aliasing get
 * their stored columns widened by the pixel aspect (32/27 for 720 wide,
 * 4/3 for 640), so the picture is 1280 x 720 at 1.5x. The title sees its
 * own size as before; only the pixels behind it change. From nfsu2-sw's
 * Vulkan renderer (the idea, "Square Pixels"). =0 turns it off. */
static int s_square = -1;
static GLint  s_max_size = 4096;

/* v title pixels of a surface l wide, in the p stored pixels behind it. */
static GLint to_stored(uint32_t v, uint32_t p, uint32_t l)
{
    return (GLint)(((uint64_t)v * p + l / 2) / l);
}

#define GL_MAX_SURF 32
static GlSurf  s_surf[GL_MAX_SURF];
static GlSurf *s_last;                  /* last surface drawn or cleared */
static int     s_drew_any;              /* the title has drawn something */

/* RECOMP_GL_FRAMETIME=2: what each presented frame did, surface by surface,
 * in order -- runs of draws (d), clears (c) and 2D blits (b) -- and which
 * one it showed. It showed MC3's loading screen drawing only a spinner
 * segment a frame on a buffer nobody had copied the last frame into: the
 * 2D engine was ignored (2026-10-24). */
static int s_ft = -1;
static char s_ft_log[512];
static size_t s_ft_len;
static uint32_t s_ft_va, s_ft_n;
static char s_ft_kind;
static void ft_flush_run(void)
{
    if (s_ft_n && s_ft_len < sizeof s_ft_log - 32)
        s_ft_len += (size_t)snprintf(s_ft_log + s_ft_len, sizeof s_ft_log - s_ft_len,
                                     " %X:%u%c", s_ft_va, s_ft_n, s_ft_kind);
    s_ft_n = 0;
}
static void ft_note(uint32_t va, char kind)
{
    if (s_ft < 2)
        return;
    if (s_ft_n && (va != s_ft_va || kind != s_ft_kind))
        ft_flush_run();
    s_ft_va = va; s_ft_kind = kind; s_ft_n++;
}

static int s_watch_hit;
static int s_watch_dumping;   /* inside a dumped frame: clears are logged too */
static void watch_shot(const GlSurf *s);   /* RECOMP_GL_WATCH_SHOTS */

static GlSurf *surf_find(uint32_t va)
{
    int i;
    for (i = 0; i < GL_MAX_SURF; i++)
        if (s_surf[i].fbo && s_surf[i].va == va)
            return &s_surf[i];
    return NULL;
}

static GlSurf *surf_get(uint32_t va, uint32_t w, uint32_t h,
                        uint32_t aa_sx, uint32_t aa_sy)
{
    GlSurf *s = surf_find(va), *victim = NULL;
    int i;

    /* h is the surface clip, not the allocation: NFSU2's split screen clips
     * the back buffer to 640x240 for each player's view and back to 640x480
     * for the HUD, every frame. Rebuilding (and clearing) the surface on
     * each change left only the HUD, over a black 3D world. Keep the
     * surface while the clip fits; grow it when it does not. */
    /* Both ways: MC3 draws its race minimap into the back buffer clipped
     * to 175x451 (pitch unset for that pass, so the width came from the
     * clip). Matching the width exactly rebuilt the surface at 175 wide --
     * the finished race scene deleted, the minimap drawn into an empty
     * target, and that target presented: a black screen with a minimap,
     * every frame of a race. A clip that fits uses the surface; one that
     * does not grows it to cover both, never shrinks it. */
    if (s && s->w >= w && s->h >= h) {
        s->used = s_frame;
        return s;
    }
    if (s) {
        if (s->w > w) w = s->w;
        if (s->h > h) h = s->h;
    }
    if (!s) {
        for (i = 0; i < GL_MAX_SURF; i++) {
            if (!s_surf[i].fbo) { victim = &s_surf[i]; break; }
            if (!victim || s_surf[i].used < victim->used) victim = &s_surf[i];
        }
        s = victim;
    }
    if (s->fbo) {
        glDeleteFramebuffers(1, &s->fbo);
        glDeleteTextures(1, &s->tex);
        tex_bind_forget();
        glDeleteRenderbuffers(1, &s->ds);
    }
    memset(s, 0, sizeof *s);
    s_new_obj = 1;
    s->va = va; s->w = w; s->h = h; s->used = s_frame;
    s->aa_sx = aa_sx; s->aa_sy = aa_sy;
    {
        double kx = s_scale, ky = s_scale;
        if (s_square < 0) {
            const char *e = getenv("RECOMP_GL_SQUARE");
            s_square = !(e && *e == '0') && xbox_video_widescreen();
        }
        if (s_square && aa_sx <= 1 && w >= 512 && h >= 400 && h <= 576)
            kx *= w > 640 ? 32.0 / 27.0 : 4.0 / 3.0;
        if (w * kx > (double)s_max_size) kx = (double)s_max_size / w;
        if (h * ky > (double)s_max_size) ky = (double)s_max_size / h;
        s->pw = (uint32_t)(w * kx + 0.5); s->ph = (uint32_t)(h * ky + 0.5);
        if (!s->pw) s->pw = 1;
        if (!s->ph) s->ph = 1;
    }
    glGenTextures(1, &s->tex);
    tex_param_forget(s->tex);
    glBindTexture(GL_TEXTURE_2D, s->tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)s->pw, (GLsizei)s->ph, 0,
                 GL_BGRA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glGenRenderbuffers(1, &s->ds);
    glBindRenderbuffer(GL_RENDERBUFFER, s->ds);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, (GLsizei)s->pw, (GLsizei)s->ph);
    glGenFramebuffers(1, &s->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s->tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                              GL_RENDERBUFFER, s->ds);
    s->ds_on = s->ds;
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        fprintf(stderr, "  [GL] surface 0x%08X %ux%u incomplete\n", va, s->pw, s->ph);
    glViewport(0, 0, (GLsizei)s->pw, (GLsizei)s->ph);
    glClearColor(0, 0, 0, 1);
    glClearDepth(1.0);
    glClearStencil(0);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(1, 1, 1, 1);
    glDepthMask(1);
    glStencilMask(0xFF);
    state_dirty();
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    if (s_trace)
        fprintf(stderr, "  [GL] surface 0x%08X %ux%u (aa %ux%u, stored %ux%u)\n",
                va, w, h, aa_sx, aa_sy, s->pw, s->ph);
    return s;
}

static GLuint depth_get(uint32_t va, uint32_t pw, uint32_t ph, int *fresh)
{
    GlDepth *d, *victim = NULL;
    int i;

    for (i = 0; i < GL_MAX_DEPTH; i++) {
        if (s_depth[i].rb && s_depth[i].va == va
            && s_depth[i].pw == pw && s_depth[i].ph == ph) {
            s_depth[i].used = s_frame;
            s_depth_cur = &s_depth[i];
            return s_depth[i].rb;
        }
        if (!victim || (victim->rb && (!s_depth[i].rb || s_depth[i].used < victim->used)))
            victim = &s_depth[i];
    }
    d = victim;
    if (d->rb) {
        /* Surfaces still pointing at it re-attach on their next bind. */
        for (i = 0; i < GL_MAX_SURF; i++)
            if (s_surf[i].ds_on == d->rb)
                s_surf[i].ds_on = 0;
        glDeleteRenderbuffers(1, &d->rb);
        if (d->rgba) {
            glDeleteFramebuffers(1, &d->fbo_a);
            glDeleteFramebuffers(1, &d->fbo_b);
            glDeleteTextures(1, &d->dtex);
            glDeleteTextures(1, &d->rgba);
        }
    }
    s_new_obj = 1;
    *fresh = 1;
    memset(d, 0, sizeof *d);
    s_depth_cur = d;
    d->va = va; d->pw = pw; d->ph = ph; d->used = s_frame;
    glGenRenderbuffers(1, &d->rb);
    glBindRenderbuffer(GL_RENDERBUFFER, d->rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, (GLsizei)pw, (GLsizei)ph);
    if (s_trace)
        fprintf(stderr, "  [GL] depth 0x%08X %ux%u\n", va, pw, ph);
    return d->rb;
}

static GlSurf *surf_bind(const Nv2aSurface *sf, uint32_t zeta_va)
{
    uint32_t bpp = sf->bytes_per_pixel ? sf->bytes_per_pixel : 4;
    uint32_t w = sf->pitch ? sf->pitch / bpp : sf->width;
    uint32_t h = sf->clip_y + sf->height;   /* the clip need not start at 0 */
    GlSurf *s;
    GLuint ds;
    int fresh = 0;

    if (w < sf->clip_x + sf->width) w = sf->clip_x + sf->width;
    if (!w || !h || !sf->color_va)
        return NULL;
    s = surf_get(sf->color_va, w, h, sf->aa_sx ? sf->aa_sx : 1,
                 sf->aa_sy ? sf->aa_sy : 1);
    {   /* The widest clip of the frame: a narrower pass into the same
         * surface (MC3's minimap, 175 wide) must not crop what is shown. */
        uint32_t cw = sf->clip_x + sf->width < s->w ? sf->clip_x + sf->width : s->w;
        if (s->vw_frame != s_frame || cw > s->vw)
            s->vw = cw;
        s->vw_frame = s_frame;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, s->fbo);
    if (s_shared_z < 0) {
        const char *e = getenv("RECOMP_GL_SHARED_Z");
        s_shared_z = !(e && e[0] == '0');
    }
    ds = zeta_va && s_shared_z ? depth_get(zeta_va, s->pw, s->ph, &fresh) : s->ds;
    if (zeta_va && s_shared_z) {        /* drawn or cleared from here: see depth_as_color */
        GlDepth *d = s_depth_cur;
        d->lw = s->w; d->lh = s->h; d->ax = s->aa_sx; d->ay = s->aa_sy;
        d->wseq = ++s_zseq;
    }
    if (s->ds_on != ds) {
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                  GL_RENDERBUFFER, ds);
        s->ds_on = ds;
    }
    if (fresh) {                        /* new storage is undefined */
        glDisable(GL_SCISSOR_TEST);
        glDepthMask(1);
        glStencilMask(0xFF);
        glClearDepth(1.0);
        glClearStencil(0);
        state_dirty();
        glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    }
    glViewport(0, 0, (GLsizei)s->pw, (GLsizei)s->ph);
    s_last = s;
    return s;
}

/* ── Textures ─────────────────────────────────────────────────────── */

typedef struct {
    uint32_t va, color, w, h, pitch, hash, bytes;
    GLuint   tex;
    uint32_t used, checked;
    uint32_t levels;                    /* mip levels the title gave it */
    uint32_t mips;                      /* levels the GL texture holds */
    GLuint   hd;                        /* drawn instead: a pack texture or a UI 2x copy */
    uint32_t hd_hash;                   /* ... made for this content */
    uint32_t hd_checked;                /* content (hash) last looked up in the pack */
    int      hd_seen;                   /* hd_checked is valid */
    int      hd_mips;                   /* hd has mip levels (pack textures) */
    uint64_t hd_key;                    /* pack texture requested for this content */
    int      hd_pending;                /* ... on the worker, not here yet */
    int      rewrites;                  /* content changes at this address */
    int      hd_ui;                     /* hd is the UI upscale's own RGBA copy */
} GlTex;

#define GL_MAX_TEX 1024
static GlTex     s_tex[GL_MAX_TEX];
/* (va, format, size) -> s_tex index, chained; entries hold index + 1. A
 * draw binds up to four textures, and a scan of all 1024 entries per stage
 * was the biggest CPU cost of a draw after the driver. */
#define TEX_BUCKETS 2048
static uint16_t s_tex_head[TEX_BUCKETS], s_tex_next[GL_MAX_TEX];
static uint32_t tex_bucket(uint32_t va, uint32_t color, uint32_t w, uint32_t h)
{
    uint32_t k = va ^ (color << 24) ^ (w << 12) ^ (h << 4);
    k ^= k >> 15; k *= 0x2C1B3C6Du; k ^= k >> 12;
    return k & (TEX_BUCKETS - 1);
}
static void tex_index_add(GlTex *t)
{
    uint32_t b = tex_bucket(t->va, t->color, t->w, t->h);
    uint16_t i = (uint16_t)(t - s_tex);
    s_tex_next[i] = s_tex_head[b];
    s_tex_head[b] = (uint16_t)(i + 1);
}
static void tex_index_del(GlTex *t)
{
    uint16_t *at = &s_tex_head[tex_bucket(t->va, t->color, t->w, t->h)];
    uint16_t i = (uint16_t)(t - s_tex);
    while (*at) {
        if (*at == i + 1) {
            *at = s_tex_next[i];
            return;
        }
        at = &s_tex_next[*at - 1];
    }
}
static uint32_t *s_decode;
static size_t    s_decode_cap;

static int tex_size_from_format(uint32_t color)
{
    /* Swizzled (0x00-0x0B, 0x19, 0x1A, 0x27-0x3F except linear) and DXT
     * carry their size in the format word; linear images use IMAGE_RECT. */
    if (color >= 0x0C && color <= 0x0F)
        return 1;
    switch (color) {
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15:
    case 0x16: case 0x17: case 0x18: case 0x1B: case 0x1C: case 0x1D:
    case 0x1E: case 0x1F: case 0x20: case 0x24: case 0x25: case 0x26:
    case 0x2E: case 0x2F: case 0x30: case 0x31: case 0x34: case 0x35:
    case 0x36: case 0x37: case 0x40: case 0x41: case 0x43: case 0x44:
    case 0x45: case 0x46:
        return 0;
    default:
        return 1;
    }
}

static uint32_t bytes_hash(uint32_t va, uint32_t bytes)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset() + va;
    uint32_t h = 2166136261u ^ bytes, step, i;

    if (!bytes)
        return h;
    step = bytes / 256;
    if (step < 4) step = 4;
    step &= ~3u;
    for (i = 0; i + 4 <= bytes; i += step) {
        uint32_t w;
        memcpy(&w, mem + i, 4);
        h = (h ^ w) * 16777619u;
    }
    memcpy(&i, mem + bytes - 4, 4);
    return (h ^ i) * 16777619u;
}

/* Every byte, for images the CPU rewrites in place. The sampled hash above
 * looks at 256 words: in a 640x480 A8R8G8B8 movie frame that is one pixel
 * every 4800 bytes, so a change between samples -- the last letter of a
 * title being typed out on a dark frame -- left the texture stale until a
 * sampled pixel changed too (MC3's intro, 2026-10-15). ~0.2 ms per 1.2 MB. */
static uint32_t bytes_hash_full(uint32_t va, uint32_t bytes)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset() + va;
    uint64_t h0 = 1469598103934665603ull ^ bytes, h1 = 0x9E3779B97F4A7C15ull;
    uint32_t i;

    for (i = 0; i + 16 <= bytes; i += 16) {     /* two chains: twice the speed */
        uint64_t a, b;
        memcpy(&a, mem + i, 8);
        memcpy(&b, mem + i + 8, 8);
        h0 = (h0 ^ a) * 1099511628211ull;
        h1 = (h1 ^ b) * 0xBF58476D1CE4E5B9ull;
    }
    for (; i < bytes; i++)
        h0 = (h0 ^ mem[i]) * 1099511628211ull;
    h0 ^= h1 * 31u;
    return (uint32_t)(h0 ^ (h0 >> 32));
}

static uint32_t tex_bytes(uint32_t color, uint32_t w, uint32_t h, uint32_t pitch);

/* Bytes a texel of a swizzled format takes (0 for DXT and linear images). */
static uint32_t tex_texel_bytes(uint32_t color)
{
    switch (color) {
    case 0x00: case 0x01: case 0x0B: case 0x19:                       /* L8 AY8 I8 A8 */
        return 1;
    case 0x02: case 0x03: case 0x04: case 0x05:                       /* 16-bit RGB */
    case 0x1A: case 0x27: case 0x28: case 0x29:                       /* A8Y8 R6G5B5 G8B8 R8B8 */
    case 0x2C: case 0x2D:                                             /* D16, F16 */
        return 2;
    default:
        return 4;
    }
}

/* The bytes that are the texture's own: level 0 at its real size, and the
 * title's mip levels after it. The change check hashed tex_bytes -- 4 bytes
 * a texel whatever the format -- so a 1- or 2-byte texture took in the
 * memory after it, a neighbour's changes made it "rewritten", and from then
 * on it was hashed in full at every use and re-decoded and uploaded at
 * every change of that neighbour: ~1.8 textures, 6 ms a frame in an MC3
 * race on the Switch (2026-10-26). */
static uint32_t tex_own_bytes(uint32_t color, uint32_t w, uint32_t h,
                              uint32_t pitch, uint32_t levels)
{
    uint32_t total = 0, l, bpp;
    if (pitch)
        return pitch * h;                                           /* linear */
    bpp = (color >= 0x0C && color <= 0x0F) ? 0 : tex_texel_bytes(color);
    if (levels < 1) levels = 1;
    for (l = 0; l < levels && l < 13; l++) {
        uint32_t lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1;
        total += bpp ? lw * lh * bpp : tex_bytes(color, lw, lh, 0);
        if (lw == 1 && lh == 1)
            break;
    }
    return total;
}

static uint32_t tex_bytes(uint32_t color, uint32_t w, uint32_t h, uint32_t pitch)
{
    if (color == 0x0C) return ((w + 3) / 4) * ((h + 3) / 4) * 8;
    if (color == 0x0E || color == 0x0F) return ((w + 3) / 4) * ((h + 3) / 4) * 16;
    if (pitch) return pitch * h;
    return w * h * 4;          /* upper bound for swizzled formats */
}

/* Xbox swizzle: texel (u, v) of a power-of-two image sits at the index made
 * by interleaving the coordinate bits, u in the even positions and v in the
 * odd ones, until the smaller dimension runs out; the larger one's remaining
 * bits follow on top. */
static uint32_t swizzle_index(uint32_t u, uint32_t v, uint32_t w, uint32_t h)
{
    uint32_t out = 0, bit = 0, mw = w - 1, mh = h - 1, m;

    for (m = 1; mw >= m || mh >= m; m <<= 1) {
        if (mw >= m) { if (u & m) out |= 1u << bit; bit++; }
        if (mh >= m) { if (v & m) out |= 1u << bit; bit++; }
    }
    return out;
}

/* Morton order per axis: a texel's index is sx[x] | sy[y], the bits of x
 * and y interleaving independently -- one table pass per texture instead of
 * swizzle_index's bit loop per texel (~18 steps at 512x512) in every
 * decode. */
static uint32_t s_swz_x[4096], s_swz_y[4096];
static void swizzle_tables(uint32_t w, uint32_t h)
{
    uint32_t i;
    for (i = 0; i < w && i < 4096; i++)
        s_swz_x[i] = swizzle_index(i, 0, w, h);
    for (i = 0; i < h && i < 4096; i++)
        s_swz_y[i] = swizzle_index(0, i, w, h);
}

/* SZ_I8_A8R8G8B8: 8-bit indices, swizzled, into an A8R8G8B8 palette named by
 * SET_TEXTURE_PALETTE (offset in the upper bits, length code in [3:2]:
 * 256, 128, 64 or 32 entries). The executor's decoder has no palette state,
 * so this format is decoded here. */
static uint32_t s_palette_reg, s_palette_va;   /* stage being bound */
static const Nv2aRawBatch *s_batch;          /* draw being executed */

static int decode_indexed(uint32_t va, uint32_t w, uint32_t h, uint32_t *out)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t pal_va = s_palette_va;
    uint32_t entries = 256u >> ((s_palette_reg >> 2) & 3);
    const uint8_t *idx = mem + va;
    uint32_t x, y;

    if (!(s_palette_reg & ~0x3Fu))
        return 0;
    swizzle_tables(w, h);
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            uint32_t i = idx[s_swz_x[x] | s_swz_y[y]];
            uint32_t c;
            if (i >= entries)
                i = 0;
            memcpy(&c, mem + pal_va + i * 4, 4);
            out[y * w + x] = c;
        }
    return 1;
}

/* n bits of colour scaled to 8, as the executor's decoder does
 * (d3d8_expand_channel): the same texels either way. */
static inline uint32_t bits8(uint32_t v, int n)
{
    return v * 255u / ((1u << n) - 1u);
}

/* Swizzled A1R5G5B5, X1R5G5B5, A4R4G4B4, R5G6B5, L8 and A8 to A8R8G8B8,
 * with the conversions of nv2a_backend_decode_texture (sample_texture).
 * 0 for any other format. */
static int decode_swizzled_small(uint32_t va, uint32_t color, uint32_t w, uint32_t h,
                                 uint32_t *out)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset() + va;
    const uint16_t *m16 = (const uint16_t *)mem;
    uint32_t x, y;
    if (!(color <= 0x05 && color != 0x01) && color != 0x19)
        return 0;
    if (w > 4096 || h > 4096)
        return 0;
    swizzle_tables(w, h);
    for (y = 0; y < h; y++) {
        uint32_t *row = out + (size_t)y * w, sy = s_swz_y[y];
        switch (color) {
        case 0x02:                                      /* A1R5G5B5 */
        case 0x03:                                      /* X1R5G5B5 */
            for (x = 0; x < w; x++) {
                uint32_t t = m16[s_swz_x[x] | sy];
                row[x] = (color == 0x03 || (t & 0x8000u) ? 0xFF000000u : 0u)
                       | (bits8((t >> 10) & 0x1F, 5) << 16)
                       | (bits8((t >> 5) & 0x1F, 5) << 8) | bits8(t & 0x1F, 5);
            }
            break;
        case 0x04:                                      /* A4R4G4B4 */
            for (x = 0; x < w; x++) {
                uint32_t t = m16[s_swz_x[x] | sy];
                row[x] = (bits8((t >> 12) & 0xF, 4) << 24) | (bits8((t >> 8) & 0xF, 4) << 16)
                       | (bits8((t >> 4) & 0xF, 4) << 8) | bits8(t & 0xF, 4);
            }
            break;
        case 0x05:                                      /* R5G6B5 */
            for (x = 0; x < w; x++) {
                uint32_t t = m16[s_swz_x[x] | sy];
                row[x] = 0xFF000000u | (bits8((t >> 11) & 0x1F, 5) << 16)
                       | (bits8((t >> 5) & 0x3F, 6) << 8) | bits8(t & 0x1F, 5);
            }
            break;
        case 0x00:                                      /* L8 */
            for (x = 0; x < w; x++) {
                uint32_t t = mem[s_swz_x[x] | sy];
                row[x] = 0xFF000000u | (t << 16) | (t << 8) | t;
            }
            break;
        case 0x19:                                      /* A8 */
            for (x = 0; x < w; x++)
                row[x] = ((uint32_t)mem[s_swz_x[x] | sy] << 24) | 0x00FFFFFFu;
            break;
        }
    }
    return 1;
}

/* GPU memory held by the texture cache, and its budget. The cache was
 * bounded by entries (GL_MAX_TEX) only; 1024 large textures is gigabytes,
 * and a console out of GPU memory does not fail an allocation -- it hangs.
 * Over budget, the least recently used textures go. RECOMP_GL_TEX_MB sets
 * it (default 256). */
static uint64_t s_tex_bytes;

uint64_t nv2a_gl_texture_bytes(void) { return s_tex_bytes; }
uint32_t nv2a_gl_frame_count(void) { return s_frame; }

static void tex_account(GlTex *t, uint32_t bytes)
{
    s_hz_texs++;
    s_hz_tex_bytes += bytes;
    if (gl_probe() && bytes >= (1u << 20) && t) {
        /* Probe: a large texture as the title left it in memory (a movie
         * frame) -- all zero here means the title's side, not GL. */
        static unsigned n_seen;
        if (n_seen++ % 30 == 0) {
            const uint8_t *p = (const uint8_t *)xbox_GetMemoryOffset() + t->va;
            uint32_t n = t->pitch ? t->pitch * t->h : t->w * t->h * 4, k, lo = 255, hi = 0, nz = 0;
            for (k = 0; k < n; k += 97) {
                if (p[k] < lo) lo = p[k];
                if (p[k] > hi) hi = p[k];
                nz += p[k] != 0;
            }
            fprintf(stderr, "  [GL] probe: texture 0x%08X format 0x%02X %ux%u pitch %u: bytes %u..%u, %u of %u sampled non-zero\n",
                    t->va, t->color, t->w, t->h, t->pitch, lo, hi, nz, n / 97 + 1);
        }
    }
    static uint64_t budget;
    if (!budget) {
        const char *e = getenv("RECOMP_GL_TEX_MB");
        budget = (uint64_t)(e && atoi(e) > 0 ? atoi(e) : 256) << 20;
    }
    s_tex_bytes += (uint64_t)bytes - t->bytes;
    t->bytes = bytes;
    while (s_tex_bytes > budget) {
        GlTex *lru = NULL;
        uint32_t i;
        for (i = 0; i < GL_MAX_TEX; i++) {
            GlTex *c = &s_tex[i];
            if (c->tex && c != t && c->used != s_frame && (!lru || c->used < lru->used))
                lru = c;
        }
        if (!lru)
            break;                      /* everything left is in use this frame */
        glDeleteTextures(1, &lru->tex);
        if (lru->hd)
            glDeleteTextures(1, &lru->hd);   /* its upscaled or pack copy too */
        tex_bind_forget();
        s_tex_bytes -= lru->bytes;
        tex_index_del(lru);
        memset(lru, 0, sizeof *lru);
    }
}

/* ── Mipmaps ──────────────────────────────────────────────────────────
 *
 * Only level 0 was uploaded, so everything a few metres away was sampled
 * from the full-size image: distant roads and buildings shimmered and
 * crawled. Most of MC3's race textures are DXT with 4 levels of their own
 * (2026-10-18 survey). The title's levels follow level 0 in memory: DXT and
 * swizzled A8R8G8B8/X8R8G8B8 upload them as they are; other decoded formats
 * that ask for levels get them from the GPU. Single-level textures stay
 * single-level, as on the hardware. Sampling (bind_stage) then goes
 * trilinear with anisotropic filtering. From nfsu2-sw's Vulkan renderer
 * (the idea). RECOMP_GL_MIPS=0 uploads level 0 only, as before. */
static int s_mips_on = -1;

static int mips_wanted(const GlTex *t)
{
    if (s_mips_on < 0) {
        const char *e = getenv("RECOMP_GL_MIPS");
        s_mips_on = !(e && *e == '0');
    }
    return s_mips_on && t->levels > 1 && t->w > 1 && t->h > 1
        && !(t->w & (t->w - 1)) && !(t->h & (t->h - 1));
}

/* The texture is bound; it holds `n` levels. */
static void tex_set_levels(GlTex *t, uint32_t n)
{
    t->mips = n ? n : 1;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, (GLint)t->mips - 1);
}

static uint64_t hz_now(void);
#define HZ_GL(call) do { uint64_t hz_t0_ = hz_now(); call; s_hz_gl_ns += hz_now() - hz_t0_; } while (0)

static void tex_mips_dxt_(GlTex *t, GLenum fmt, uint32_t size0);
static void tex_mips_dxt(GlTex *t, GLenum fmt, uint32_t size0)
{
    uint64_t t0 = hz_now();
    tex_mips_dxt_(t, fmt, size0);
    s_hz_mip_ns += hz_now() - t0;
}
static void tex_mips_dxt_(GlTex *t, GLenum fmt, uint32_t size0)
{
    const uint8_t *base = (const uint8_t *)xbox_GetMemoryOffset() + t->va;
    uint32_t l, off = size0, n = 1;
    if (mips_wanted(t)) {
        for (l = 1; l < t->levels; l++) {
            uint32_t lw = t->w >> l ? t->w >> l : 1, lh = t->h >> l ? t->h >> l : 1;
            uint32_t sz = tex_bytes(t->color, lw, lh, 0);
            glCompressedTexImage2D(GL_TEXTURE_2D, (GLint)l, fmt, (GLsizei)lw, (GLsizei)lh, 0,
                                   (GLsizei)sz, base + off);
            off += sz;
            n++;
            if (lw == 1 && lh == 1)
                break;
        }
        tex_account(t, off - size0);
    }
    tex_set_levels(t, n);
}

static void tex_mips_argb_(GlTex *t, const uint32_t *src, uint32_t fill);
static void tex_mips_argb(GlTex *t, const uint32_t *src, uint32_t fill)
{
    uint64_t t0 = hz_now();
    tex_mips_argb_(t, src, fill);
    s_hz_mip_ns += hz_now() - t0;
}
static void tex_mips_argb_(GlTex *t, const uint32_t *src, uint32_t fill)
{
    uint32_t l, n = 1, x, y;
    size_t off;
    if (mips_wanted(t)) {
        off = (size_t)t->w * t->h;
        for (l = 1; l < t->levels; l++) {
            uint32_t lw = t->w >> l ? t->w >> l : 1, lh = t->h >> l ? t->h >> l : 1;
            const uint32_t *ls = src + off;
            swizzle_tables(lw, lh);
            for (y = 0; y < lh; y++)
                for (x = 0; x < lw; x++)
                    s_decode[(size_t)y * lw + x] = ls[s_swz_x[x] | s_swz_y[y]] | fill;
            glTexImage2D(GL_TEXTURE_2D, (GLint)l, GL_RGBA8, (GLsizei)lw, (GLsizei)lh, 0,
                         GL_BGRA, GL_UNSIGNED_BYTE, s_decode);
            off += (size_t)lw * lh;
            n++;
            if (lw == 1 && lh == 1)
                break;
        }
        tex_account(t, (uint32_t)((off - (size_t)t->w * t->h) * 4));
    }
    tex_set_levels(t, n);
}

static void tex_mips_generate_(GlTex *t);
static void tex_mips_generate(GlTex *t)
{
    uint64_t t0 = hz_now();
    tex_mips_generate_(t);
    s_hz_mip_ns += hz_now() - t0;
}
static void tex_mips_generate_(GlTex *t)
{
    if (mips_wanted(t) && glGenerateMipmap) {
        glGenerateMipmap(GL_TEXTURE_2D);
        tex_account(t, t->w * t->h * 4 / 3);
        tex_set_levels(t, 32);               /* the chain to 1x1 */
        t->mips = t->levels;                 /* > 1: sampled with mipmaps */
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 1000);
        return;
    }
    tex_set_levels(t, 1);
}

static GlTex *s_tex_cur;                     /* what tex_get returned last */
static void tex_hd(GlTex *t);
static GLuint tex_done(GlTex *t)
{
    s_tex_cur = t;
    /* The pack (hd_pack.c): look each distinct content up once. Images
     * rewritten in place (movies, CPU-drawn: linear, with a pitch) are not
     * looked up: their content changes every frame. */
    if (!t->pitch && (!t->hd_seen || t->hd_checked != t->hash) && hd_pack_init())
        tex_hd(t);
    return t->hd && t->hd_hash == t->hash ? t->hd : t->tex;
}

static GLuint tex_get(uint32_t va, uint32_t color, uint32_t w, uint32_t h,
                      uint32_t pitch, uint32_t levels)
{
    GlTex *t = NULL, *victim = NULL;
    uint32_t i, hash;
    Nv2aTexture nt;

    for (i = s_tex_head[tex_bucket(va, color, w, h)]; i; i = s_tex_next[i - 1]) {
        GlTex *c = &s_tex[i - 1];
        if (c->tex && c->va == va && c->color == color && c->w == w && c->h == h
            && c->levels == levels) {
            t = c;
            break;
        }
    }
    if (!t)                                  /* a new texture: find a slot */
        for (i = 0; i < GL_MAX_TEX; i++) {
            GlTex *c = &s_tex[i];
            if (!c->tex) { if (!victim || victim->tex) victim = c; }
            else if (!victim || (victim->tex && c->used < victim->used)) victim = c;
        }
    /* Once per frame per texture is enough to notice an update -- for
     * textures that never changed. One the title has rewritten before
     * (t->rewrites) is checked at every use and in full: MC3 writes text
     * into a swizzled texture between draws, and a once-a-frame, sampled
     * check showed the letters of an older string (the tutorial line under
     * the race results, scrambled, 2026-10-22). */
    if (t && t->checked == s_frame && !t->rewrites) {
        t->used = s_frame;
        return tex_done(t);
    }
    /* Linear images (movies, UI, CPU-drawn) in full; swizzled ones, mostly
     * static game textures, sampled -- in full once they have changed. */
    hash = tex_size_from_format(color) && !(t && t->rewrites)
               ? bytes_hash(va, tex_own_bytes(color, w, h, pitch, levels))
               : bytes_hash_full(va, tex_own_bytes(color, w, h, pitch, levels));
    if (color == 0x0B)                       /* the palette is content too */
        hash ^= bytes_hash(s_palette_va, 1024) * 31u;
    static unsigned probe_tex_n;
    if (gl_probe() && color == 0x12 && w >= 320 && (probe_tex_n++ % 30) == 0) {
        /* Probe: the movie frame as the title left it in memory -- black
         * here means the title's blitter, not GL. */
        const uint8_t *p = (const uint8_t *)xbox_GetMemoryOffset() + va;
        uint32_t n = pitch ? pitch * h : w * h * 4, k, lo = 255, hi = 0, nz = 0;
        for (k = 0; k < n; k += 97) {
            if (p[k] < lo) lo = p[k];
            if (p[k] > hi) hi = p[k];
            nz += p[k] != 0;
        }
        fprintf(stderr, "  [GL] probe: movie frame at 0x%08X %ux%u pitch %u in memory: bytes %u..%u, %u of %u sampled non-zero\n",
                va, w, h, pitch, lo, hi, nz, n / 97 + 1);
    }
    if (t && t->hash == hash) {
        t->used = t->checked = s_frame;
        return tex_done(t);
    }
    if (t)
        t->rewrites++;                       /* same image, new content */
    if (!t) {
        t = victim;
        if (t->tex) {
            glDeleteTextures(1, &t->tex);
            if (t->hd)
                glDeleteTextures(1, &t->hd);
            tex_bind_forget();
            s_tex_bytes -= t->bytes;
            tex_index_del(t);
        }
        memset(t, 0, sizeof *t);
        t->va = va; t->color = color; t->w = w; t->h = h; t->levels = levels;
        tex_index_add(t);
        glGenTextures(1, &t->tex);
        tex_param_forget(t->tex);
    } else if (color == 0x12 && t->pitch == pitch && pitch && !(pitch & 3)
               && pitch / 4 >= w) {
        /* A linear A8R8G8B8 image whose contents changed -- every frame of a
         * movie. That is GL's BGRA/UNSIGNED_BYTE byte for byte: update the
         * existing storage straight from guest memory, no decode, no
         * reallocation. */
        t->hash = hash; t->used = t->checked = s_frame;
        bind_tex(t->tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)(pitch / 4));
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)w, (GLsizei)h, GL_BGRA,
                        GL_UNSIGNED_BYTE, (const uint8_t *)xbox_GetMemoryOffset() + va);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        gl_step_done("texture upload");
        return tex_done(t);
    }
    t->va = va; t->color = color; t->w = w; t->h = h; t->pitch = pitch;
    t->hash = hash; t->used = t->checked = s_frame;
    s_new_obj = 1;
    s_hz_fmt[color & 63]++;
    if (s_trace)
        fprintf(stderr, "  [GL] texture 0x%08X format 0x%02X %ux%u pitch %u\n",
                va, color, w, h, pitch);

    if (color == 0x12 && pitch && !(pitch & 3) && pitch / 4 >= w) {
        bind_tex(t->tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)(pitch / 4));
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0, GL_BGRA,
                     GL_UNSIGNED_BYTE, (const uint8_t *)xbox_GetMemoryOffset() + va);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        tex_account(t, w * h * 4);
        return tex_done(t);
    }

    if (color == 0x0C || color == 0x0E || color == 0x0F) {
        /* DXT: the Switch's GPU samples it as it is. Uploaded compressed:
         * no per-texel decode (it cost ~70 ms per MB of textures on the
         * console, the race's mid-lap hitches) and 4-8x less to copy. Falls
         * back to the decoder if the driver says no. RECOMP_GL_DXT=0 off. */
        static int s3tc = -1;
        if (s3tc < 0) {
            const char *e = getenv("RECOMP_GL_DXT");
            s3tc = !(e && *e == '0');
        }
        if (s3tc) {
            GLenum fmt = color == 0x0C ? GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
                       : color == 0x0E ? GL_COMPRESSED_RGBA_S3TC_DXT3_EXT
                                       : GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
            uint32_t size = tex_bytes(color, w, h, 0);
            bind_tex(t->tex);
            while (glGetError() != GL_NO_ERROR) { }
            HZ_GL(glCompressedTexImage2D(GL_TEXTURE_2D, 0, fmt, (GLsizei)w, (GLsizei)h, 0,
                                         (GLsizei)size, (const uint8_t *)xbox_GetMemoryOffset() + va));
            if (glGetError() == GL_NO_ERROR) {
                tex_account(t, size);
                tex_mips_dxt(t, fmt, size);
                gl_step_done("texture upload");
                return tex_done(t);
            }
            s3tc = 0;
            fprintf(stderr, "  [GL] compressed DXT upload refused; decoding DXT on the CPU\n");
        }
    }
    if ((size_t)w * h > s_decode_cap) {
        free(s_decode);
        s_decode_cap = (size_t)w * h;
        s_decode = (uint32_t *)malloc(s_decode_cap * 4);
    }
    memset(&nt, 0, sizeof nt);
    nt.offset = va; nt.width = w; nt.height = h; nt.pitch = pitch; nt.color = color;
    nt.addr_u = nt.addr_v = 1;
    bind_tex(t->tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (s_decode && (color == 0x06 || color == 0x07) && w >= 1 && h >= 1) {
        /* Swizzled A8R8G8B8 / X8R8G8B8: un-Morton the texels in a loop
         * instead of a sampler call per texel. */
        const uint32_t *src = (const uint32_t *)((const uint8_t *)xbox_GetMemoryOffset() + va);
        uint32_t x, y, fill = color == 0x07 ? 0xFF000000u : 0;
        swizzle_tables(w, h);
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++)
                s_decode[(size_t)y * w + x] = src[s_swz_x[x] | s_swz_y[y]] | fill;
        HZ_GL(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0,
                           GL_BGRA, GL_UNSIGNED_BYTE, s_decode));
        tex_account(t, w * h * 4);
        tex_mips_argb(t, src, fill);
        gl_step_done("texture upload");
    } else if (s_decode && decode_swizzled_small(va, color, w, h, s_decode)) {
        /* Swizzled 16- and 8-bit formats: one loop, not a sampler call per
         * texel (nv2a_backend_decode_texture). The same conversions;
         * RECOMP_GL_DECODE_CHECK=1 decodes each both ways and compares. */
        static int check = -1;
        if (check < 0)
            check = getenv("RECOMP_GL_DECODE_CHECK") != NULL;
        if (check) {
            uint32_t *ref = (uint32_t *)malloc((size_t)w * h * 4), k, bad = 0;
            if (ref && nv2a_backend_decode_texture(&nt, ref)) {
                for (k = 0; k < w * h; k++)
                    bad += ref[k] != s_decode[k];
                fprintf(stderr, "  [GL] decode check fmt 0x%02X %ux%u: %u of %u texels differ\n",
                        color, w, h, bad, w * h);
            }
            free(ref);
        }
        HZ_GL(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0,
                           GL_BGRA, GL_UNSIGNED_BYTE, s_decode));
        tex_account(t, w * h * 4);
        tex_mips_generate(t);
        gl_step_done("texture upload");
    } else if (s_decode && (color == 0x0B ? decode_indexed(va, w, h, s_decode)
                                    : nv2a_backend_decode_texture(&nt, s_decode))) {
        HZ_GL(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0,
                           GL_BGRA, GL_UNSIGNED_BYTE, s_decode));
        tex_account(t, w * h * 4);
        tex_mips_generate(t);
        gl_step_done("texture upload");
    } else {
        static const uint32_t magenta = 0xFFFF00FFu;
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_BGRA,
                     GL_UNSIGNED_BYTE, &magenta);
        tex_account(t, 4);
        if (s_trace)
            fprintf(stderr, "  [GL] texture 0x%08X format 0x%02X not decodable\n",
                    va, color);
    }
    return tex_done(t);
}

/* ── UI art upscaled once on the GPU ─────────────────────────────────
 *
 * The HUD (speedometer and tachometer dials, minimap frame, digits) and
 * menu art are small single-level textures sized for a 480-line TV -- MC3's
 * dials are 128x128 and 256x256 palettised images drawn ~108 title pixels
 * wide (2026-10-20) -- so at 1.5x with square pixels they were stretched
 * ~1.7x bilinearly: soft. Such a texture gets, once per content, a copy
 * RECOMP_GL_UI_SCALE times larger (default 2, 1 = off) made by one GPU
 * pass: Catmull-Rom on premultiplied colour (no dark fringes at alpha
 * edges), clamped. Draws then sample the copy (tex_done). Which textures:
 * one mip level, not linear (movies, CPU-drawn images), 16..256 a side,
 * DXT only up to 64x64 (digits; bigger single-level DXT is world art).
 * Made before the draw binds its surface (stage_prepare). */
/* ── Replacement textures and the texture dump (hd_pack.c) ───────────
 *
 * Key: FNV-1a 64 over the level-0 bytes the title wrote, its format and
 * size, and for a palettised image its palette -- one key per distinct
 * image, whatever address it was loaded at. A replacement may be any size;
 * it gets a full mip chain, so a 4x image drawn small does not shimmer. */
static uint64_t tex_full_hash(const GlTex *t)
{
    const uint8_t *p = (const uint8_t *)xbox_GetMemoryOffset() + t->va;
    uint64_t h = 1469598103934665603ull;
    uint32_t n, i;
    /* Level 0's real size: tex_bytes is exact for DXT, an upper bound (4
     * bytes a texel) otherwise, and bytes past a texture are not its own. */
    switch (t->color) {
    case 0x00: case 0x01: case 0x0B: case 0x19:                       /* L8 AY8 I8 A8 */
        n = t->w * t->h; break;
    case 0x02: case 0x03: case 0x04: case 0x05:                       /* 16-bit RGB */
    case 0x1A: case 0x27: case 0x28: case 0x29:                       /* A8Y8 R6G5B5 G8B8 R8B8 */
        n = t->w * t->h * 2; break;
    default:
        n = tex_bytes(t->color, t->w, t->h, 0); break;                /* DXT, 32-bit */
    }
    for (i = 0; i < n; i++)
        h = (h ^ p[i]) * 1099511628211ull;
    h = (h ^ t->color) * 1099511628211ull;
    h = (h ^ ((uint64_t)t->w << 16 | t->h)) * 1099511628211ull;
    if (t->color == 0x0B && s_palette_va) {
        const uint8_t *pal = (const uint8_t *)xbox_GetMemoryOffset() + s_palette_va;
        for (i = 0; i < 1024; i++)
            h = (h ^ pal[i]) * 1099511628211ull;
    }
    return h;
}

static int s_hd_off;                         /* the driver refused compressed uploads */

static void tex_hd(GlTex *t)
{
    uint64_t key;
    t->hd_seen = 1;
    t->hd_pending = 0;                       /* the content changed: an older request is moot */
    t->hd_checked = t->hash;
    key = tex_full_hash(t);
    if (hd_pack_dumping() && s_decode && (size_t)t->w * t->h <= s_decode_cap) {
        Nv2aTexture nt;
        int ok;
        memset(&nt, 0, sizeof nt);
        nt.offset = t->va; nt.width = t->w; nt.height = t->h; nt.pitch = 0; nt.color = t->color;
        nt.addr_u = nt.addr_v = 1;
        ok = t->color == 0x0B ? decode_indexed(t->va, t->w, t->h, s_decode)
                              : nv2a_backend_decode_texture(&nt, s_decode);
        if (ok)
            hd_pack_dump(key, s_decode, (int)t->w, (int)t->h);
    }
    if (!s_hd_off && hd_pack_has(key)) {
        /* Decoded, mipmapped and compressed on hd_pack's worker; the
         * original is drawn until hd_drain uploads the result. */
        t->hd_key = key;
        t->hd_pending = 1;
        hd_pack_request(key);
    }
}


/* Upload h as a new texture; 0 on failure. */
static GLuint hd_upload(const HdTex *h, uint32_t *bytes)
{
    GLenum fmt = h->alpha ? GL_COMPRESSED_RGBA_S3TC_DXT5_EXT : GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
    GLuint id = 0;
    int l;
    *bytes = 0;
    glGenTextures(1, &id);
    tex_param_forget(id);
    glBindTexture(GL_TEXTURE_2D, id);
    while (glGetError() != GL_NO_ERROR) { }
    for (l = 0; l < h->levels; l++) {
        glCompressedTexImage2D(GL_TEXTURE_2D, l, fmt, h->lw[l], h->lh[l], 0,
                               (GLsizei)h->bytes[l], h->data[l]);
        *bytes += h->bytes[l];
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, h->levels - 1);
    tex_bind_forget();
    if (glGetError() != GL_NO_ERROR) {
        glDeleteTextures(1, &id);
        return 0;
    }
    return id;
}

/* Once a frame (renderer thread): upload the replacements the worker
 * finished -- a few a frame, so not even the upload is a hitch -- into
 * every texture slot waiting for that content (each its own GL texture:
 * a slot's eviction deletes its own). */
static void hd_drain(void)
{
    static const int per_frame = 8;
    int n, i;
    for (n = 0; n < per_frame; n++) {
        HdTex *h = hd_pack_poll();
        if (!h)
            break;
        for (i = 0; i < GL_MAX_TEX && !s_hd_off; i++) {
            GlTex *t = &s_tex[i];
            uint32_t bytes;
            GLuint id;
            if (!t->tex || !t->hd_pending || t->hd_key != h->key
                || t->hd_checked != t->hash)
                continue;
            if (!(id = hd_upload(h, &bytes))) {
                fprintf(stderr, "  [GL] HD pack: compressed upload refused; pack off\n");
                s_hd_off = 1;
                break;
            }
            if (t->hd)
                glDeleteTextures(1, &t->hd);         /* a UI 2x copy: the pack wins */
            t->hd = id;
            t->hd_ui = 0;
            t->hd_hash = t->hash;
            t->hd_mips = h->levels > 1;
            t->hd_pending = 0;
            tex_account(t, bytes);
            s_new_obj = 1;
        }
        hd_pack_tex_free(h);
    }
}

static GLuint compile(GLenum type, const char *const *src, int n);
static GLuint s_ui_prog, s_ui_vao, s_ui_fbo;
static GLint  s_ui_usz;
static int    s_ui_scale = -1;

static int ui_eligible(const GlTex *t)
{
    if (s_ui_scale < 0) {
        const char *e = getenv("RECOMP_GL_UI_SCALE");
        s_ui_scale = e ? atoi(e) : 2;
        if (s_ui_scale < 1) s_ui_scale = 1;
        if (s_ui_scale > 5) s_ui_scale = 5;
        fprintf(stderr, "  [GL] UI art upscaled %dx (RECOMP_GL_UI_SCALE)\n", s_ui_scale);
    }
    if (s_ui_scale <= 1 || t->levels > 1 || t->pitch)       /* pitch: linear */
        return 0;
    /* Rewritten in place more than a few times: text or a gauge the title
     * draws into the image while it is shown. The 2x copy lags it or mixes
     * old and new: MC3's tutorial line under the race results came out as
     * scrambled letters (2026-10-22). Upscaled art is static art. */
    if (t->rewrites > 4) {
        static int logged;
        if (logged++ < 10)
            fprintf(stderr, "  [GL] UI upscale: 0x%08X (format 0x%02X %ux%u) changes while shown: "
                            "kept as drawn\n", t->va, t->color, t->w, t->h);
        return 0;                            /* the old copy: hash differs, unused */
    }
    if (t->w < 16 || t->h < 16 || t->w > 256 || t->h > 256)
        return 0;
    if ((t->color == 0x0C || t->color == 0x0E || t->color == 0x0F) && t->w * t->h > 64 * 64)
        return 0;
    return 1;
}

static int ui_prog_init(void)
{
    static const char *vs[1] = {
        "#version 330 core\n"
        "void main() {\n"
        "    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
        "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
        "}\n" };
    static const char *fs[1] = {
        "#version 330 core\n"
        "uniform sampler2D u_src;\n"
        "uniform vec4 u_sz;            /* out w, h; source w, h */\n"
        "out vec4 o;\n"
        "vec4 cr(float t) {\n"
        "    float t2 = t * t, t3 = t2 * t;\n"
        "    return vec4(-0.5 * t3 + t2 - 0.5 * t, 1.5 * t3 - 2.5 * t2 + 1.0,\n"
        "                -1.5 * t3 + 2.0 * t2 + 0.5 * t, 0.5 * t3 - 0.5 * t2);\n"
        "}\n"
        "void main() {\n"
        "    vec2 p = gl_FragCoord.xy / u_sz.xy * u_sz.zw - 0.5, f = fract(p);\n"
        "    ivec2 b = ivec2(floor(p)) - 1, mx = ivec2(u_sz.zw) - 1;\n"
        "    vec4 wx = cr(f.x), wy = cr(f.y), acc = vec4(0.0);\n"
        "    for (int j = 0; j < 4; j++)\n"
        "        for (int i = 0; i < 4; i++) {\n"
        "            vec4 c = texelFetch(u_src, clamp(b + ivec2(i, j), ivec2(0), mx), 0);\n"
        "            acc += wx[i] * wy[j] * vec4(c.rgb * c.a, c.a);   /* premultiplied */\n"
        "        }\n"
        "    float a = clamp(acc.a, 0.0, 1.0);\n"
        "    o = vec4(a > 0.0 ? clamp(acc.rgb / max(acc.a, 1e-4), 0.0, 1.0) : vec3(0.0), a);\n"
        "}\n" };
    GLuint v, f;
    GLint ok = 0;
    if (s_ui_prog)
        return 1;
    v = compile(GL_VERTEX_SHADER, vs, 1);
    f = compile(GL_FRAGMENT_SHADER, fs, 1);
    if (!v || !f) { s_ui_scale = 1; return 0; }
    s_ui_prog = glCreateProgram();
    glAttachShader(s_ui_prog, v);
    glAttachShader(s_ui_prog, f);
    glLinkProgram(s_ui_prog);
    glDeleteShader(v);
    glDeleteShader(f);
    glGetProgramiv(s_ui_prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        fprintf(stderr, "  [GL] UI upscale link failed: UI art as is\n");
        glDeleteProgram(s_ui_prog);
        s_ui_prog = 0;
        s_ui_scale = 1;
        return 0;
    }
    glUseProgram(s_ui_prog);
    s_cur_prog = s_ui_prog;
    glUniform1i(glGetUniformLocation(s_ui_prog, "u_src"), 0);
    s_ui_usz = glGetUniformLocation(s_ui_prog, "u_sz");
    glGenVertexArrays(1, &s_ui_vao);
    glGenFramebuffers(1, &s_ui_fbo);
    return 1;
}

/* t was just looked up for a draw: make or refresh its upscaled copy. */
static void dump_bmp(const char *path, const uint8_t *bgra, uint32_t w, uint32_t h);
int stbi_write_png(char const *filename, int w, int h, int comp, const void *data,
                   int stride_in_bytes);                 /* hd_pack.c */

/* RECOMP_GL_UI_DUMP=<dir>: every texture the UI upscale handles, once per
 * content, written as <dir>/ui_<va>_<format>_<w>x<h>_<hash>_orig.png and
 * _up.png -- to find the one whose 2x copy comes out wrong (MC3's
 * tutorial line under the race results, 2026-10-22). */
static void ui_dump(const GlTex *t, uint32_t ow, uint32_t oh)
{
    static int on = -1;
    static char dir[256];
    static uint32_t seen[4096];
    static unsigned nseen;
    unsigned i;
    uint8_t *buf;
    char path[400];
    if (on < 0) {
        const char *e = getenv("RECOMP_GL_UI_DUMP");
        on = e && *e;
        if (on) {
            char cwd[256];
            snprintf(dir, sizeof dir, "%s", e);
            mkdir(dir, 0755);                /* a relative path is from the run's folder */
            fprintf(stderr, "  [GL] UI upscale dump: %s%s%s\n",
                    dir[0] == '/' ? "" : (getcwd(cwd, sizeof cwd) ? cwd : "."),
                    dir[0] == '/' ? "" : "/", dir);
        }
    }
    if (!on)
        return;
    for (i = 0; i < nseen; i++)
        if (seen[i] == t->hash)
            return;
    if (nseen < 4096)
        seen[nseen++] = t->hash;
    buf = (uint8_t *)malloc((size_t)ow * oh * 4);
    if (!buf)
        return;
    /* read back through the upscale's own framebuffer (no glGetTexImage
     * in the loader); PNG keeps the alpha, where font pages keep their
     * letters */
    glBindFramebuffer(GL_FRAMEBUFFER, s_ui_fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->tex, 0);
    glReadPixels(0, 0, (GLsizei)t->w, (GLsizei)t->h, GL_RGBA, GL_UNSIGNED_BYTE, buf);
    snprintf(path, sizeof path, "%s/ui_%08X_%02X_%ux%u_%08X_orig.png", dir, t->va, t->color,
             t->w, t->h, t->hash);
    stbi_write_png(path, (int)t->w, (int)t->h, 4, buf, (int)t->w * 4);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->hd, 0);
    glReadPixels(0, 0, (GLsizei)ow, (GLsizei)oh, GL_RGBA, GL_UNSIGNED_BYTE, buf);
    snprintf(path, sizeof path, "%s/ui_%08X_%02X_%ux%u_%08X_up.png", dir, t->va, t->color,
             t->w, t->h, t->hash);
    stbi_write_png(path, (int)ow, (int)oh, 4, buf, (int)ow * 4);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    free(buf);
    state_dirty();
}

static void ui_upscale(GlTex *t)
{
    uint32_t ow, oh;
    float sz[4];
    if ((t->hd && t->hd_hash == t->hash) || !ui_eligible(t) || !ui_prog_init())
        return;
    t->hd_mips = 0;
    ow = t->w * (uint32_t)s_ui_scale;
    oh = t->h * (uint32_t)s_ui_scale;
    /* Only ever render into a copy made here. t->hd can be an HD pack
     * replacement (hd_drain: DXT, other content), and rendering into a
     * compressed texture does nothing: the pack texture of another font
     * page was then taken as this content's copy, and MC3's tutorial line
     * under the race results showed another page's letters (2026-10-22,
     * with a texture pack installed). */
    if (t->hd && !t->hd_ui) {
        glDeleteTextures(1, &t->hd);
        t->hd = 0;
        tex_bind_forget();
    }
    if (!t->hd) {
        glGenTextures(1, &t->hd);
        tex_param_forget(t->hd);
        glBindTexture(GL_TEXTURE_2D, t->hd);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)ow, (GLsizei)oh, 0,
                     GL_BGRA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        tex_account(t, ow * oh * 4);
        t->hd_ui = 1;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, s_ui_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->hd, 0);
    glViewport(0, 0, (GLsizei)ow, (GLsizei)oh);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(1, 1, 1, 1);
    glUseProgram(s_ui_prog);
    s_cur_prog = s_ui_prog;
    sz[0] = (float)ow; sz[1] = (float)oh; sz[2] = (float)t->w; sz[3] = (float)t->h;
    glUniform4fv(s_ui_usz, 1, sz);
    /* Bound for real, not through the binding cache: the cache could
     * believe t->tex was on unit 0 when another texture was, and the 2x
     * copy was then made from that one -- MC3's tutorial line under the
     * race results drew from another font page, scrambled letters
     * (2026-10-22, found with RECOMP_GL_UI_DUMP). */
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, t->tex);
    tex_bind_forget();
    glBindVertexArray(s_ui_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(s_vao);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    state_dirty();
    t->hd_hash = t->hash;
    s_new_obj = 1;
    ui_dump(t, ow, oh);
}

static GLint wrap_mode(uint32_t m)
{
    switch (m) {
    case 2:  return GL_MIRRORED_REPEAT;
    case 3:  return GL_CLAMP_TO_EDGE;
    case 4:  return GL_CLAMP_TO_BORDER;
    case 5:  return GL_CLAMP_TO_EDGE;
    default: return GL_REPEAT;
    }
}

/* ── A depth buffer sampled as a colour texture ──────────────────────
 *
 * Titles read the depth buffer back through a texture of a 32-bit colour
 * format whose address is the zeta buffer: the D24S8 dword (depth in the
 * top 24 bits, stencil in the bottom 8) seen as A8R8G8B8 or A8B8G8R8. MC3's
 * motion blur builds its per-pixel strength that way (2026-10-18: a copy
 * as LIN_A8R8G8B8 into a 256x256 target, then 8 combiner stages over the
 * scene and LIN_A8B8G8R8 depth into an alpha-only mask): near things sharp,
 * the distance streaked. Depth lives in GL renderbuffers here, never in
 * guest memory, so the texture decoded stale bytes -- a constant mask, the
 * whole scene smeared. Now the depth is blitted to a depth texture and
 * packed into an RGBA8 texture in the title's byte order (GL depth is z /
 * zmax, so z = depth * 0xFFFFFF; stencil reads 0). Converted again only
 * after the depth was drawn to. RECOMP_GL_DEPTH_TEX=0 turns it off. */
static GLuint s_zprog, s_zvao;
static GLint  s_zprog_abgr;
static int    s_ztex_on = -1;
static GLuint compile(GLenum type, const char *const *src, int n);
static void state_dirty(void);

static int zconv_init(void)
{
    static const char *vs[1] = {
        "#version 330 core\n"
        "void main() {\n"
        "    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
        "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
        "}\n" };
    static const char *fs[1] = {
        "#version 330 core\n"
        "uniform sampler2D u_depth;\n"
        "uniform int u_abgr;\n"
        "out vec4 o;\n"
        "void main() {\n"
        "    float z = texelFetch(u_depth, ivec2(gl_FragCoord.xy), 0).r;\n"
        "    uint v = uint(clamp(z, 0.0, 1.0) * 16777215.0 + 0.5);\n"
        "    float hi = float(v >> 16u) / 255.0, mid = float((v >> 8u) & 255u) / 255.0,\n"
        "          lo = float(v & 255u) / 255.0;\n"
        "    /* the texel the decoder would make of the dword (ARGB, uploaded BGRA) */\n"
        "    o = u_abgr != 0 ? vec4(0.0, lo, mid, hi) : vec4(mid, lo, 0.0, hi);\n"
        "}\n" };
    GLuint v, f;
    GLint ok = 0;
    if (s_zprog)
        return 1;
    if (s_ztex_on < 0) {
        const char *e = getenv("RECOMP_GL_DEPTH_TEX");
        s_ztex_on = !(e && *e == '0');
    }
    if (!s_ztex_on)
        return 0;
    v = compile(GL_VERTEX_SHADER, vs, 1);
    f = compile(GL_FRAGMENT_SHADER, fs, 1);
    if (!v || !f) {
        s_ztex_on = 0;
        return 0;
    }
    s_zprog = glCreateProgram();
    glAttachShader(s_zprog, v);
    glAttachShader(s_zprog, f);
    glLinkProgram(s_zprog);
    glDeleteShader(v);
    glDeleteShader(f);
    glGetProgramiv(s_zprog, GL_LINK_STATUS, &ok);
    if (!ok) {
        glDeleteProgram(s_zprog);
        s_zprog = 0;
        s_ztex_on = 0;
        return 0;
    }
    s_zprog_abgr = glGetUniformLocation(s_zprog, "u_abgr");
    glGenVertexArrays(1, &s_zvao);
    return 1;
}

/* The zeta buffer at `va` as a texture of colour format `color`, or 0.
 * convert = 0 only looks up what was converted (inside a draw, whose state
 * a conversion would trample); the draw path converts before it binds. */
static GLuint depth_as_color(uint32_t va, uint32_t color, int convert, const GlDepth **out)
{
    GlDepth *d = NULL;
    uint32_t fmt;
    int i;
    if (color == 0x12 || color == 0x06)      fmt = 1;   /* A8R8G8B8 */
    else if (color == 0x3F || color == 0x3A) fmt = 2;   /* A8B8G8R8 */
    else return 0;
    if (s_ztex_on == 0 || s_shared_z <= 0)
        return 0;
    for (i = 0; i < GL_MAX_DEPTH; i++)       /* the largest: the scene's */
        if (s_depth[i].rb && s_depth[i].va == va
            && (!d || s_depth[i].pw * s_depth[i].ph > d->pw * d->ph))
            d = &s_depth[i];
    if (!d)
        return 0;
    *out = d;
    if (d->rgba && d->cseq == d->wseq && d->cfmt == fmt)
        return d->rgba;
    if (!convert)
        return d->rgba;                      /* stale or 0: better than guest bytes */
    if (!zconv_init())
        return 0;
    if (!d->rgba) {
        glGenTextures(1, &d->dtex);
        glBindTexture(GL_TEXTURE_2D, d->dtex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, (GLsizei)d->pw, (GLsizei)d->ph, 0,
                     GL_DEPTH_STENCIL, 0x84FA /* UNSIGNED_INT_24_8 */, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glGenTextures(1, &d->rgba);
        tex_param_forget(d->dtex);
        tex_param_forget(d->rgba);
        glBindTexture(GL_TEXTURE_2D, d->rgba);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)d->pw, (GLsizei)d->ph, 0,
                     GL_BGRA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        /* a: the copy as colour, with the live depth (blit source);
         * b: the depth texture (blit target). Both complete with a colour. */
        glGenFramebuffers(1, &d->fbo_a);
        glBindFramebuffer(GL_FRAMEBUFFER, d->fbo_a);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, d->rgba, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, d->rb);
        glGenFramebuffers(1, &d->fbo_b);
        glBindFramebuffer(GL_FRAMEBUFFER, d->fbo_b);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, d->rgba, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, d->dtex, 0);
        tex_bind_forget();
        s_new_obj = 1;
        fprintf(stderr, "  [GL] depth 0x%08X (%ux%u) sampled as colour format 0x%02X\n",
                va, d->pw, d->ph, color);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, d->fbo_a);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, d->fbo_b);
    glBlitFramebuffer(0, 0, (GLint)d->pw, (GLint)d->ph, 0, 0, (GLint)d->pw, (GLint)d->ph,
                      GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, d->fbo_a);
    glViewport(0, 0, (GLsizei)d->pw, (GLsizei)d->ph);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(1, 1, 1, 1);
    glUseProgram(s_zprog);
    s_cur_prog = s_zprog;
    glUniform1i(s_zprog_abgr, fmt == 2);
    active_unit(0);
    bind_tex(d->dtex);
    glBindVertexArray(s_zvao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(s_vao);
    state_dirty();
    d->cseq = d->wseq;
    d->cfmt = fmt;
    return d->rgba;
}

/* A surface kept taller than its clip (surf_get) and sampled as a texture
 * of the clip's size: texcoords cover the w x h texels at the top left, not
 * the whole surface. A texture of the surface's logical or real size keeps
 * the old full-surface mapping (anti-aliased surfaces). */
static void rt_scale(uint32_t w, uint32_t h, uint32_t sw, uint32_t sh,
                     uint32_t aa_sx, uint32_t aa_sy, float scale[2])
{
    uint32_t lw = sw / (aa_sx ? aa_sx : 1), lh = sh / (aa_sy ? aa_sy : 1);

    if (w < lw) scale[0] *= (float)w / (float)lw;
    if (h < lh) scale[1] *= (float)h / (float)lh;
}

/* ── Movie frames, upscaled on the GPU ───────────────────────────────
 *
 * A movie reached the screen as the title's own 640x480 A8R8G8B8 texture,
 * converted from YUV on the CPU by the title's player and then stretched
 * bilinearly. A host that decodes the movie itself (MC3's bink_native.c)
 * hands each frame's Y, U and V planes here, tagged with the address of
 * the texture the title will draw (nv2a_gl_movie_frame). When a draw
 * samples that texture, the planes are uploaded and one pass converts and
 * upscales them -- bicubic (Catmull-Rom) luma, bilinear chroma -- into a
 * texture RECOMP_GL_MOVIE_SCALE times larger (default 2), drawn instead.
 * The colour matrix comes from the host (fitted to the title's own
 * conversion), so colours match. RECOMP_GL_MOVIE_HD=0 turns it off. */
typedef struct {
    uint32_t va, w, h, seq, done_seq;
    uint8_t *pl[3];
    size_t   cap[3];
    float    m[12];
    float    alpha;                          /* written as the title's player does */
    int      active;
    uint32_t shown;                          /* s_frame it was last drawn */
    GLuint   tp[3], out, fbo;
    uint32_t ow, oh, tw, th;
} MovieSlot;
static MovieSlot       s_mv;
static pthread_mutex_t s_mv_lock = PTHREAD_MUTEX_INITIALIZER;
static GLuint          s_mv_prog, s_mv_vao;
static GLint           s_mv_um, s_mv_usz, s_mv_ua;
static int             s_mv_on = -1;
static float           s_mv_scale = 2.0f;

static int movie_hd_on(void)
{
    if (s_mv_on < 0) {
        const char *e = getenv("RECOMP_GL_MOVIE_HD"), *k = getenv("RECOMP_GL_MOVIE_SCALE");
        s_mv_on = !(e && *e == '0');
        if (k && atof(k) >= 1.0) s_mv_scale = (float)atof(k);
        if (s_mv_scale > 4.0f) s_mv_scale = 4.0f;
    }
    return s_mv_on;
}

/* Any thread (the title's): the planes of the frame the title is about to
 * copy into its texture at va (w x h, 4:2:0). m: R, G, B rows of a 3x4
 * matrix applied to (Y, U, V, 1), all 0..1. */
void nv2a_gl_movie_frame(uint32_t va, uint32_t w, uint32_t h,
                         const uint8_t *y, int ys, const uint8_t *u, int us,
                         const uint8_t *v, int vs, const float m[12], float alpha)
{
    const uint8_t *src[3];
    int str[3], k;
    uint32_t pw[3], ph[3];
    if (!movie_hd_on() || !w || !h || w > 2048 || h > 2048)
        return;
    src[0] = y; src[1] = u; src[2] = v;
    str[0] = ys; str[1] = us; str[2] = vs;
    pw[0] = w; ph[0] = h;
    pw[1] = pw[2] = (w + 1) / 2;
    ph[1] = ph[2] = (h + 1) / 2;
    pthread_mutex_lock(&s_mv_lock);
    for (k = 0; k < 3; k++) {
        size_t need = (size_t)pw[k] * ph[k];
        uint32_t r;
        if (s_mv.cap[k] < need) {
            uint8_t *n = realloc(s_mv.pl[k], need);
            if (!n) { s_mv.active = 0; pthread_mutex_unlock(&s_mv_lock); return; }
            s_mv.pl[k] = n;
            s_mv.cap[k] = need;
        }
        for (r = 0; r < ph[k]; r++)
            memcpy(s_mv.pl[k] + (size_t)r * pw[k], src[k] + (size_t)r * str[k], pw[k]);
    }
    memcpy(s_mv.m, m, sizeof s_mv.m);
    s_mv.alpha = alpha;
    s_mv.va = va; s_mv.w = w; s_mv.h = h;
    s_mv.seq++;
    s_mv.active = 1;
    pthread_mutex_unlock(&s_mv_lock);
}

/* The movie at va ended: draw the title's texture again. */
void nv2a_gl_movie_end(uint32_t va)
{
    pthread_mutex_lock(&s_mv_lock);
    if (s_mv.va == va)
        s_mv.active = 0;
    pthread_mutex_unlock(&s_mv_lock);
}

/* Was the upscaled frame for va drawn lately? The host may then skip its
 * own YUV -> RGB copy, which nothing else sees. */
int nv2a_gl_movie_shown(uint32_t va)
{
    int r;
    pthread_mutex_lock(&s_mv_lock);
    r = s_mv.active && s_mv.va == va && s_mv.shown && s_frame - s_mv.shown <= 2;
    pthread_mutex_unlock(&s_mv_lock);
    return r;
}

static int movie_prog_init(void)
{
    static const char *vs[1] = {
        "#version 330 core\n"
        "void main() {\n"
        "    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
        "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
        "}\n" };
    static const char *fs[1] = {
        "#version 330 core\n"
        "uniform sampler2D u_y, u_u, u_v;\n"
        "uniform vec4 u_m[3];\n"
        "uniform vec4 u_sz;            /* out w, h; luma w, h */\n"
        "uniform float u_alpha;\n"
        "out vec4 o;\n"
        "/* Catmull-Rom weights for the 4 taps around fraction t */\n"
        "vec4 cr(float t) {\n"
        "    float t2 = t * t, t3 = t2 * t;\n"
        "    return vec4(-0.5 * t3 + t2 - 0.5 * t, 1.5 * t3 - 2.5 * t2 + 1.0,\n"
        "                -1.5 * t3 + 2.0 * t2 + 0.5 * t, 0.5 * t3 - 0.5 * t2);\n"
        "}\n"
        "void main() {\n"
        "    vec2 uv = gl_FragCoord.xy / u_sz.xy;              /* row 0 = top, as the planes */\n"
        "    vec2 p = uv * u_sz.zw - 0.5, f = fract(p);\n"
        "    ivec2 b = ivec2(floor(p)) - 1, mx = ivec2(u_sz.zw) - 1;\n"
        "    vec4 wx = cr(f.x), wy = cr(f.y);\n"
        "    float yv = 0.0;\n"
        "    for (int j = 0; j < 4; j++) {\n"
        "        float row = 0.0;\n"
        "        for (int i = 0; i < 4; i++)\n"
        "            row += wx[i] * texelFetch(u_y, clamp(b + ivec2(i, j), ivec2(0), mx), 0).r;\n"
        "        yv += wy[j] * row;\n"
        "    }\n"
        "    vec4 c = vec4(clamp(yv, 0.0, 1.0), texture(u_u, uv).r, texture(u_v, uv).r, 1.0);\n"
        "    o = vec4(clamp(vec3(dot(u_m[0], c), dot(u_m[1], c), dot(u_m[2], c)), 0.0, 1.0), u_alpha);\n"
        "}\n" };
    GLuint v, f;
    GLint ok = 0;
    if (s_mv_prog)
        return 1;
    v = compile(GL_VERTEX_SHADER, vs, 1);
    f = compile(GL_FRAGMENT_SHADER, fs, 1);
    if (!v || !f) {
        s_mv_on = 0;
        return 0;
    }
    s_mv_prog = glCreateProgram();
    glAttachShader(s_mv_prog, v);
    glAttachShader(s_mv_prog, f);
    glLinkProgram(s_mv_prog);
    glDeleteShader(v);
    glDeleteShader(f);
    glGetProgramiv(s_mv_prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        fprintf(stderr, "  [GL] movie upscale link failed: movies as the title draws them\n");
        glDeleteProgram(s_mv_prog);
        s_mv_prog = 0;
        s_mv_on = 0;
        return 0;
    }
    glUseProgram(s_mv_prog);
    s_cur_prog = s_mv_prog;
    glUniform1i(glGetUniformLocation(s_mv_prog, "u_y"), 0);
    glUniform1i(glGetUniformLocation(s_mv_prog, "u_u"), 1);
    glUniform1i(glGetUniformLocation(s_mv_prog, "u_v"), 2);
    s_mv_um = glGetUniformLocation(s_mv_prog, "u_m");
    s_mv_usz = glGetUniformLocation(s_mv_prog, "u_sz");
    s_mv_ua = glGetUniformLocation(s_mv_prog, "u_alpha");
    glGenVertexArrays(1, &s_mv_vao);
    fprintf(stderr, "  [GL] movies upscaled %gx on the GPU (RECOMP_GL_MOVIE_HD)\n", s_mv_scale);
    return 1;
}

/* Before a draw binds its surface: the upscaled frame for a stage sampling
 * the movie texture va (A8R8G8B8 linear), converted if a new frame came. */
static GLuint movie_prepare(uint32_t va, uint32_t color)
{
    int k, fresh;
    uint32_t pw[3], ph[3];
    float sz[4];
    if (color != 0x12 || !s_mv_on || !s_mv.active || s_mv.va != va)
        return 0;
    pthread_mutex_lock(&s_mv_lock);
    if (!s_mv.active || s_mv.va != va) {
        pthread_mutex_unlock(&s_mv_lock);
        return 0;
    }
    s_mv.shown = s_frame;
    if (s_mv.done_seq == s_mv.seq && s_mv.out) {
        pthread_mutex_unlock(&s_mv_lock);
        return s_mv.out;
    }
    if (!movie_prog_init()) {
        pthread_mutex_unlock(&s_mv_lock);
        return 0;
    }
    pw[0] = s_mv.w; ph[0] = s_mv.h;
    pw[1] = pw[2] = (s_mv.w + 1) / 2;
    ph[1] = ph[2] = (s_mv.h + 1) / 2;
    fresh = !s_mv.out || s_mv.tw != s_mv.w || s_mv.th != s_mv.h;
    if (fresh) {
        if (s_mv.out) {
            glDeleteTextures(3, s_mv.tp);
            glDeleteTextures(1, &s_mv.out);
            glDeleteFramebuffers(1, &s_mv.fbo);
        }
        s_mv.tw = s_mv.w; s_mv.th = s_mv.h;
        s_mv.ow = (uint32_t)(s_mv.w * s_mv_scale + 0.5f);
        s_mv.oh = (uint32_t)(s_mv.h * s_mv_scale + 0.5f);
        if (s_mv.ow > (uint32_t)s_max_size) s_mv.ow = (uint32_t)s_max_size;
        if (s_mv.oh > (uint32_t)s_max_size) s_mv.oh = (uint32_t)s_max_size;
        glGenTextures(3, s_mv.tp);
        for (k = 0; k < 3; k++) {
            glBindTexture(GL_TEXTURE_2D, s_mv.tp[k]);
            glTexImage2D(GL_TEXTURE_2D, 0, 0x8229 /* R8 */, (GLsizei)pw[k], (GLsizei)ph[k], 0,
                         0x1903 /* RED */, GL_UNSIGNED_BYTE, NULL);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            tex_param_forget(s_mv.tp[k]);
        }
        glGenTextures(1, &s_mv.out);
        tex_param_forget(s_mv.out);
        glBindTexture(GL_TEXTURE_2D, s_mv.out);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)s_mv.ow, (GLsizei)s_mv.oh, 0,
                     GL_BGRA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glGenFramebuffers(1, &s_mv.fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, s_mv.fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_mv.out, 0);
        s_new_obj = 1;
        tex_bind_forget();
    }
    static unsigned probe_mv_n;
    if (gl_probe() && s_mv.pl[0] && (probe_mv_n++ % 30) == 0) {
        const uint8_t *y = (const uint8_t *)s_mv.pl[0];
        uint32_t n = pw[0] * ph[0], k2, lo = 255, hi = 0;
        for (k2 = 0; k2 < n; k2 += 97) { if (y[k2] < lo) lo = y[k2]; if (y[k2] > hi) hi = y[k2]; }
        fprintf(stderr, "  [GL] probe: movie Y plane %ux%u from the decoder: %u..%u; matrix row 0 %g %g %g %g, alpha %g\n",
                pw[0], ph[0], lo, hi, s_mv.m[0], s_mv.m[1], s_mv.m[2], s_mv.m[3], s_mv.alpha);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    for (k = 0; k < 3; k++) {
        active_unit(k);
        bind_tex(s_mv.tp[k]);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)pw[k], (GLsizei)ph[k],
                        0x1903 /* RED */, GL_UNSIGNED_BYTE, s_mv.pl[k]);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindFramebuffer(GL_FRAMEBUFFER, s_mv.fbo);
    glViewport(0, 0, (GLsizei)s_mv.ow, (GLsizei)s_mv.oh);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(1, 1, 1, 1);
    glUseProgram(s_mv_prog);
    s_cur_prog = s_mv_prog;
    glUniform4fv(s_mv_um, 3, s_mv.m);
    sz[0] = (float)s_mv.ow; sz[1] = (float)s_mv.oh; sz[2] = (float)s_mv.w; sz[3] = (float)s_mv.h;
    glUniform4fv(s_mv_usz, 1, sz);
    glUniform1f(s_mv_ua, s_mv.alpha);
    glBindVertexArray(s_mv_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(s_vao);
    state_dirty();
    s_mv.done_seq = s_mv.seq;
    pthread_mutex_unlock(&s_mv_lock);
    return s_mv.out;
}

static const GlDepth *s_depth_tex[4];     /* this draw's stages that sample a zeta buffer */
static GLuint s_movie_tex[4];              /* ... that sample an upscaled movie frame */

/* Before a draw binds its surface: convert the zeta buffers its stages
 * sample (depth_as_color), which uses its own framebuffer and program. */
static void depth_tex_prepare(const uint32_t *regs, const Nv2aRawBatch *b)
{
    int i;
    for (i = 0; i < 4; i++) {
        uint32_t base = (0x1B00u + (uint32_t)i * 0x40u) / 4;
        const GlDepth *d = NULL;
        s_depth_tex[i] = NULL;
        s_movie_tex[i] = 0;
        if (!(regs[base + 3] & 0x40000000u) || !regs[base] || !b->tex_va[i]
            || surf_find(b->tex_va[i]))
            continue;
        if (s_mv.active
            && (s_movie_tex[i] = movie_prepare(b->tex_va[i], (regs[base + 1] >> 8) & 0xFF)) != 0)
            continue;
        if (depth_as_color(b->tex_va[i], (regs[base + 1] >> 8) & 0xFF, 1, &d) && d) {
            s_depth_tex[i] = d;
            continue;
        }
        {   /* a swizzled single-level texture: upload it now and make its
             * upscaled copy (ui_upscale) while no surface is bound yet */
            uint32_t format = regs[base + 1], color = (format >> 8) & 0xFF;
            if (s_ui_scale != 1 && tex_size_from_format(color) && ((format >> 16) & 0xF) <= 1) {
                uint32_t w = 1u << ((format >> 20) & 0xF), h = 1u << ((format >> 24) & 0xF);
                if (w >= 16 && h >= 16 && w <= 256 && h <= 256) {
                    s_palette_reg = regs[base + 8];
                    s_palette_va = b->pal_va[i];
                    s_tex_cur = NULL;
                    (void)tex_get(b->tex_va[i], color, w, h, 0, (format >> 16) & 0xF);
                    if (s_tex_cur)
                        ui_upscale(s_tex_cur);
                }
            }
        }
    }
}

/* RECOMP_GL_ANISO: anisotropic filtering of mipmapped textures, 1..16
 * (default 16, 1 = off; capped by the driver). RECOMP_GL_LOD_BIAS: mip
 * bias, negative = sharper (default -0.25). Both as nfsu2-sw's Vulkan. */
static float s_aniso_max = -1.0f;          /* the driver's, 1 = none */
static float tex_aniso(void);
static float tex_aniso_max(void)
{
    if (s_aniso_max < 0.0f)
        (void)tex_aniso();
    return s_aniso_max;
}
static float tex_aniso(void)
{
    static float a = -1.0f;
    if (a < 0.0f) {
        const char *e = getenv("RECOMP_GL_ANISO");
        GLfloat mx = 0.0f;
        a = e ? (float)atof(e) : 16.0f;
        while (glGetError() != GL_NO_ERROR) { }
        {
            GLint mi = 0;
            glGetIntegerv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &mi);   /* float on most; int is fine */
            mx = (GLfloat)mi;
        }
        if (glGetError() != GL_NO_ERROR || mx < 1.0f)
            mx = 1.0f;                                          /* not supported */
        s_aniso_max = mx;
        if (a < 1.0f) a = 1.0f;
        if (a > mx) a = mx;
        fprintf(stderr, "  [GL] mipmaps %s, anisotropic filtering %gx (driver max %g)\n",
                s_mips_on == 0 ? "off" : "on", a, mx);
    }
    return a;
}

static float tex_lod_bias(void)
{
    static int set;
    static float b;
    if (!set) {
        const char *e = getenv("RECOMP_GL_LOD_BIAS");
        b = e ? (float)atof(e) : -0.25f;
        set = 1;
    }
    return b;
}

/* Bind stage `i` from the method shadow; returns the texcoord scale. */
static void bind_stage(const uint32_t *regs, int i, float scale[2])
{
    uint32_t base = (0x1B00u + (uint32_t)i * 0x40u) / 4;
    uint32_t control0 = regs[base + 3];
    uint32_t format = regs[base + 1];
    uint32_t color = (format >> 8) & 0xFF;
    uint32_t va, w, h, pitch = 0;
    GLuint tex = 0;
    GlSurf *rt;
    int mipped = 0;

    scale[0] = scale[1] = 1.0f;
    active_unit(i);
    if (!(control0 & 0x40000000u) || !regs[base]) {
        bind_tex(0);
        return;
    }
    va = s_batch->tex_va[i];
    if (tex_size_from_format(color)) {
        w = 1u << ((format >> 20) & 0xF);
        h = 1u << ((format >> 24) & 0xF);
    } else {
        uint32_t rect = regs[base + 7];
        w = rect >> 16;
        h = rect & 0xFFFF;
        pitch = regs[base + 4] >> 16;
        if (w) scale[0] = 1.0f / (float)w;
        if (h) scale[1] = 1.0f / (float)h;
    }
    if (!w || !h || w > 4096 || h > 4096) {
        bind_tex(0);
        return;
    }
    s_palette_reg = regs[base + 8];          /* SET_TEXTURE_PALETTE */
    s_palette_va = s_batch->pal_va[i];
    rt = surf_find(va);
    if (rt) {
        tex = rt->tex;
        /* The image the title samples is the logical one; the FBO holds it
         * at the anti-aliased size. Normalised coordinates cover both. */
        rt_scale(w, h, rt->w, rt->h, rt->aa_sx, rt->aa_sy, scale);
    } else if (s_movie_tex[i]) {          /* an upscaled movie frame (movie_prepare) */
        tex = s_movie_tex[i];
    } else if (s_depth_tex[i]) {          /* a zeta buffer (depth_as_color) */
        const GlDepth *zd = s_depth_tex[i];
        tex = zd->rgba;
        rt_scale(w, h, zd->lw, zd->lh, zd->ax ? zd->ax : 1, zd->ay ? zd->ay : 1, scale);
    } else {
        tex = tex_get(va, color, w, h, pitch, (format >> 16) & 0xF);
        mipped = s_tex_cur && ((s_tex_cur->tex == tex && s_tex_cur->mips > 1)
                               || (s_tex_cur->hd == tex && s_tex_cur->hd_mips));
    }
    bind_tex(tex);
    {
        /* Sampler state lives in the texture object: set it only when it
         * differs from what that texture last got. A texture id reused for a
         * new texture starts from GL's defaults, so the cache entry is keyed
         * by id and invalidated where textures are created (tex_param_forget). */
        uint32_t addr = regs[base + 2];
        uint32_t filter = regs[base + 5];
        GLint mag = ((filter >> 24) & 0xF) == 1 ? GL_NEAREST : GL_LINEAR;
        uint32_t nmin = (filter >> 16) & 0xFF;
        GLint min = nmin == 1 ? GL_NEAREST : GL_LINEAR;
        uint32_t key;
        /* A mipmapped filter on a texture that has its levels: trilinear
         * (nearest-based modes keep nearest texels), anisotropic. */
        /* A pack texture (larger than the original, with its own levels) is
         * filtered trilinearly whatever the title asked for: plain bilinear
         * on a 4x image drawn small would shimmer. */
        int pack = mipped && s_tex_cur && s_tex_cur->hd == tex;
        int mip = mipped && ((nmin >= 3 && nmin <= 6) || (pack && nmin == 2));
        if (mip)
            min = (nmin == 3 || nmin == 5) ? 0x2702 /* NEAREST_MIPMAP_LINEAR */
                                           : GL_LINEAR_MIPMAP_LINEAR;
        (void)pack;
        key = (addr & 0xF0F) | ((uint32_t)(mag == GL_NEAREST) << 16)
            | ((uint32_t)(min == GL_NEAREST) << 17) | ((uint32_t)mip << 18)
            | ((uint32_t)(min == 0x2702) << 19) | 0x80000000u;
        TexParam *tp = &s_tex_param[tex % TEX_PARAM_SLOTS];
        if (tp->id != tex || tp->key != key) {
            s_draw_changed = 1;
            tp->id = tex;
            tp->key = key;
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap_mode(addr & 0xF));
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap_mode((addr >> 8) & 0xF));
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min);
            if (glTexParameterf) {
                /* Only where the driver has it (GL 4.6 or the extension):
                 * Parallels' GL 3.3 refused the parameter, even at 1x, with
                 * GL_INVALID_ENUM (2026-10-23). */
                if (tex_aniso_max() > 1.0f)
                    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY,
                                    mip && min == GL_LINEAR_MIPMAP_LINEAR ? tex_aniso() : 1.0f);
                glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, mip ? tex_lod_bias() : 0.0f);
            }
        }
    }
}

/* ── Programs ─────────────────────────────────────────────────────── */

typedef struct {
    uint64_t   vkey;            /* 0: fixed-function; else program hash */
    Nv2aPshKey pkey;
    GLuint     prog;
    GLint      u_c, u_surf, u_snapy, u_aa, u_m, u_vpoff, u_xform;
    GLint      n_c, n_m, n_mv, n_tscale, n_c0, n_c1;   /* active array sizes (arr_active) */
    GLint      u_mv, u_fogplane, u_fog0, u_fog1;   /* fixed-function fog */
    GLint      u_t[4], u_tscale, u_c0, u_c1, u_fc0, u_fc1, u_fogcolor, u_afunc, u_aref;
    uint32_t   used;
    int        vpc_valid;       /* vpc holds what this program's "c" was given */
    uint32_t   vpc_gen;         /* ... as of the executor's constant version */
    int        vpc_rows;        /* c[i]'s location is u_c + i: rows can go alone */
    uint32_t   hash;            /* bucket of (vkey, pkey) */
    int        next;            /* chain in s_prog_head, index + 1 */
    float      vpc[192][4];
    int        uni_valid;       /* uni holds the rest of its uniforms */
    struct GlUni {
        float surf[4], aa[2], m[16], vpoff[4], tscale[4][2];
        float c0[8][4], c1[8][4], fog[4], fc[2][4], aref;
        float mv[16], fogplane[4], fog0[4], fog1[4], snapy;
        int   xform, afunc;
    } uni;
} GlProg;

/* How many elements of the uniform array `name` (declared with `max`) the
 * linked program keeps. Compilers drop the unused tail; an upload of more
 * elements than that is legal and ignored by most drivers, but Parallels'
 * GL refused the whole call (GL_INVALID_OPERATION), so u_tscale stayed 0 and
 * every movie frame was sampled at its black corner (2026-10-23). The live
 * elements are a prefix: binary search on name[k]. */
static GLint arr_active(GLuint prog, const char *name, int max)
{
    char n[64];
    int lo = 0, hi = max;                /* answer in [lo, hi] */
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;     /* is name[mid - 1] live? */
        snprintf(n, sizeof n, "%s[%d]", name, mid - 1);
        if (glGetUniformLocation(prog, n) >= 0) lo = mid;
        else hi = mid - 1;
    }
    /* No name[0], but the name itself: an array cut down to one element
     * and reported as a plain uniform (which is why a count above 1 was
     * refused). One element. */
    if (lo == 0 && glGetUniformLocation(prog, name) >= 0)
        lo = 1;
    return lo;
}

#define GL_MAX_PROG 1024
static GlProg s_prog[GL_MAX_PROG];
static uint32_t s_prog_made, s_prog_live;
#define PROG_BUCKETS 1024
static int s_prog_head[PROG_BUCKETS];      /* index + 1 */
static uint32_t prog_bucket(uint64_t vkey, const Nv2aPshKey *pk)
{
    const uint32_t *w = (const uint32_t *)pk;
    uint64_t h = vkey ^ 1469598103934665603ull;
    size_t i;
    for (i = 0; i < sizeof *pk / 4; i++)
        h = (h ^ w[i]) * 1099511628211ull;
    return (uint32_t)(h ^ (h >> 32)) & (PROG_BUCKETS - 1);
}
static void prog_unlink(GlProg *p)
{
    int *at = &s_prog_head[p->hash], i = (int)(p - s_prog) + 1;
    while (*at) {
        if (*at == i) { *at = p->next; return; }
        at = &s_prog[*at - 1].next;
    }
}

uint32_t nv2a_gl_program_count(void) { return s_prog_live; }

static GLuint compile(GLenum type, const char *const *src, int n)
{
    GLuint sh = glCreateShader(type);
    GLint ok = 0;

    glShaderSource(sh, n, src, NULL);
    glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        int i;
        glGetShaderInfoLog(sh, sizeof log, NULL, log);
        fprintf(stderr, "  [GL] %s shader failed:\n%s\n",
                type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
        if (s_trace)
            for (i = 0; i < n; i++)
                fprintf(stderr, "%s", src[i]);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static uint64_t prog_hash(const Nv2aRawBatch *b)
{
    uint64_t h = 1469598103934665603ull;
    uint32_t pc, k;

    for (pc = b->vp_start; pc < b->vp_slots; pc++) {
        for (k = 0; k < 4; k++)
            h = (h ^ b->vp_program[pc][k]) * 1099511628211ull;
        if (b->vp_program[pc][3] & 1)           /* FINAL */
            break;
    }
    return h ? h : 1;
}

/* ── Programs remembered across runs ─────────────────────────────────
 *
 * Mesa compiles a program the first time a draw needs it, ~40 ms each on
 * the Switch; a race start needs ~40 new ones and stalled 1.5 s over two
 * frames. Each program compiled is appended to a file (what building it
 * takes: transform mode, program slots, combiner key), and at start-up the
 * renderer compiles everything in it before the title draws, so a later run
 * finds them ready. RECOMP_PROG_CACHE=<path> moves the file, =0 disables. */
#define PROG_REC_MAGIC 0x4350564Eu          /* "NVPC" */
typedef struct {
    uint32_t   magic, xform, vp_start, slots;
    Nv2aPshKey pk;
    uint32_t   prog[136][4];
} ProgRec;
static int s_prewarming;

static const char *prog_cache_path(void)
{
    const char *e = getenv("RECOMP_PROG_CACHE");
    if (e && *e == '0' && !e[1])
        return NULL;
    if (e && *e)
        return e;
#if defined(__SWITCH__)
    return "sdmc:/switch/" XBOXRECOMP_APP_NAME "/progcache.bin";
#else
    return "progcache.bin";
#endif
}

static void prog_rec_build(const Nv2aRawBatch *b, const Nv2aPshKey *pk, ProgRec *rec)
{
    memset(rec, 0, sizeof *rec);
    rec->magic = PROG_REC_MAGIC;
    rec->xform = b->xform == 2 ? 2 : 1;
    rec->pk = *pk;
    if (rec->xform == 2 && b->vp_program && b->vp_slots <= 136) {
        rec->vp_start = b->vp_start;
        rec->slots = b->vp_slots;
        memcpy(rec->prog, b->vp_program, (size_t)b->vp_slots * 16);
    }
}

static void prog_cache_append(const Nv2aRawBatch *b, const Nv2aPshKey *pk)
{
    const char *path = prog_cache_path();
    ProgRec rec;
    FILE *f;
    if (s_prewarming || !path)
        return;
    prog_rec_build(b, pk, &rec);
    f = fopen(path, "ab");
    if (f) {
        fwrite(&rec, sizeof rec, 1, f);
        fclose(f);
    }
}

/* ── Compiled programs remembered across runs (program binaries) ──────
 *
 * The list above still compiles every program at start-up: 113 programs
 * took 4.8 s on the Switch (2026-10-18), all of it black screen before the
 * first logo, and the list grows with every place the title draws. So each
 * program linked is also kept as the driver's binary (glGetProgramBinary)
 * in <cache>-gpu.bin, keyed by its record and the driver, and loaded with
 * glProgramBinary next time; a binary the driver refuses is compiled from
 * source as before. Needs GL_NUM_PROGRAM_BINARY_FORMATS > 0 (reported at
 * start-up). RECOMP_PROG_BINARY=0 turns it off. */
#define PROG_BIN_MAGIC 0x4250564Eu          /* "NVPB" */
typedef struct { uint32_t magic, format; uint64_t key; uint32_t len, pad; } ProgBinHdr;
typedef struct { uint64_t key; uint32_t format, len; unsigned char *data; } ProgBin;
static ProgBin  *s_bins;
static unsigned  s_nbins, s_capbins, s_bin_hits;
static int       s_bin_on = -1;
static uint64_t  s_bin_driver;               /* hash of vendor, renderer, version */
static char      s_bin_path[512];

static uint64_t fnv64(uint64_t h, const void *p, size_t n)
{
    const unsigned char *c = p;
    while (n--)
        h = (h ^ *c++) * 1099511628211ull;
    return h;
}

static void prog_bin_add(uint64_t key, uint32_t format, uint32_t len, unsigned char *data)
{
    if (s_nbins == s_capbins) {
        unsigned cap = s_capbins ? s_capbins * 2 : 256;
        ProgBin *nb = realloc(s_bins, cap * sizeof *nb);
        if (!nb) { free(data); return; }
        s_bins = nb;
        s_capbins = cap;
    }
    s_bins[s_nbins].key = key;
    s_bins[s_nbins].format = format;
    s_bins[s_nbins].len = len;
    s_bins[s_nbins].data = data;
    s_nbins++;
}

static ProgBin *prog_bin_find(uint64_t key)
{
    unsigned i;
    for (i = s_nbins; i-- > 0; )             /* the newest wins */
        if (s_bins[i].key == key)
            return &s_bins[i];
    return NULL;
}

/* Once, with the context current: is it on, and what is already stored. */
static int prog_bin_init(void)
{
    const char *path = prog_cache_path(), *e = getenv("RECOMP_PROG_BINARY");
    const char *str[3];
    GLint formats = 0;
    FILE *f;
    size_t n;
    int k;
    if (s_bin_on >= 0)
        return s_bin_on;
    s_bin_on = 0;
    if ((e && *e == '0') || !path || !glGetProgramBinary || !glProgramBinary)
        return 0;
    glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &formats);
    fprintf(stderr, "  [GL] program binaries: %d format(s)%s\n", formats,
            formats > 0 ? "" : " -- programs compile from source at every start");
    if (formats <= 0)
        return 0;
    s_bin_driver = 1469598103934665603ull;
    str[0] = (const char *)glGetString(GL_VENDOR);
    str[1] = (const char *)glGetString(GL_RENDERER);
    str[2] = (const char *)glGetString(GL_VERSION);
    for (k = 0; k < 3; k++)
        if (str[k])
            s_bin_driver = fnv64(s_bin_driver, str[k], strlen(str[k]));
    {   /* and the shader source around each program: a binary from older
         * source must not load (the key holds only what is translated) */
        const char *src[3];
        src[0] = nv2a_gl_vsh_prelude();
        src[1] = nv2a_gl_vsh_main_program();
        src[2] = nv2a_gl_vsh_fixed();
        for (k = 0; k < 3; k++)
            if (src[k])
                s_bin_driver = fnv64(s_bin_driver, src[k], strlen(src[k]));
    }
    n = strlen(path);
    if (n > 4 && !strcmp(path + n - 4, ".bin"))
        n -= 4;
    snprintf(s_bin_path, sizeof s_bin_path, "%.*s-gpu.bin", (int)n, path);
    if ((f = fopen(s_bin_path, "rb"))) {
        ProgBinHdr h;
        while (fread(&h, sizeof h, 1, f) == 1 && h.magic == PROG_BIN_MAGIC
               && h.len && h.len < (64u << 20)) {
            unsigned char *d = malloc(h.len);
            if (!d || fread(d, 1, h.len, f) != h.len) { free(d); break; }
            prog_bin_add(h.key, h.format, h.len, d);
        }
        fclose(f);
    }
    s_bin_on = 1;
    return 1;
}

static uint64_t prog_bin_key(const Nv2aRawBatch *b, const Nv2aPshKey *pk)
{
    ProgRec rec;
    prog_rec_build(b, pk, &rec);
    return fnv64(s_bin_driver, &rec, sizeof rec);
}

/* A program from its stored binary, or 0. */
static GLuint prog_bin_load(uint64_t key)
{
    ProgBin *pb = prog_bin_find(key);
    GLuint prog;
    GLint ok = 0;
    if (!pb)
        return 0;
    prog = glCreateProgram();
    glProgramBinary(prog, pb->format, pb->data, (GLsizei)pb->len);
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        glDeleteProgram(prog);
        pb->key = 0;                         /* refused: compile, store anew */
        return 0;
    }
    s_bin_hits++;
    return prog;
}

static void prog_bin_store(uint64_t key, GLuint prog)
{
    ProgBinHdr h;
    GLint len = 0;
    GLsizei got = 0;
    GLenum format = 0;
    unsigned char *d;
    FILE *f;
    if (prog_bin_find(key))
        return;
    glGetProgramiv(prog, GL_PROGRAM_BINARY_LENGTH, &len);
    if (len <= 0 || !(d = malloc((size_t)len)))
        return;
    glGetProgramBinary(prog, len, &got, &format, d);
    if (got <= 0) { free(d); return; }
    memset(&h, 0, sizeof h);
    h.magic = PROG_BIN_MAGIC;
    h.format = format;
    h.key = key;
    h.len = (uint32_t)got;
    if ((f = fopen(s_bin_path, "ab"))) {
        fwrite(&h, sizeof h, 1, f);
        fwrite(d, 1, (size_t)got, f);
        fclose(f);
    }
    prog_bin_add(key, format, (uint32_t)got, d);
}

static GlProg *prog_get(const Nv2aRawBatch *b, const Nv2aPshKey *pk);

/* When the list is compiled. With program binaries (Linux: 104 programs in
 * 137 ms) all of it at start-up, as before. Without them (the Switch's Mesa
 * 20.1 has no binary format: 113 programs, 4.8 s of black screen before the
 * first logo, 2026-10-18) it waits for the title to say a static screen is
 * up (nv2a_gl_prewarm_start: MC3's legal screen, which shows while the game
 * loads) and then compiles a program per presented frame; anything drawn
 * before that compiles at first use. RECOMP_PROG_PREWARM=boot or =defer
 * forces one, =0 compiles nothing ahead; RECOMP_PROG_PREWARM_PER_FRAME sets
 * the pace (default 1, ~40 ms a program on the Switch). */
static FILE         *s_pw_file;
static unsigned      s_pw_done, s_pw_bad, s_pw_per_frame = 1;
static double        s_pw_ms;
static volatile int  s_pw_go;

void nv2a_gl_prewarm_start(void)
{
    if (!s_pw_go)
        s_pw_go = 1;
}

static void cs_report(const char *list_path);

/* Up to `max` programs compiled (0: the rest); closes the list at its end. */
static void prog_cache_prewarm_step(unsigned max)
{
    struct timespec t0, t1;
    ProgRec rec;
    unsigned made0 = s_prog_made;
    if (!s_pw_file)
        return;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    s_prewarming = 1;
    for (;;) {
        Nv2aRawBatch bb;
        if (max && s_prog_made - made0 >= max)
            break;
        if (fread(&rec, sizeof rec, 1, s_pw_file) != 1) {
            fclose(s_pw_file);
            s_pw_file = NULL;
            break;
        }
        if (rec.magic != PROG_REC_MAGIC || rec.slots > 136) {
            s_pw_bad++;
            fclose(s_pw_file);
            s_pw_file = NULL;
            break;
        }
        memset(&bb, 0, sizeof bb);
        bb.xform = rec.xform;
        bb.vp_program = (const uint32_t (*)[4])rec.prog;
        bb.vp_slots = rec.slots ? rec.slots : 136;
        bb.vp_start = rec.vp_start;
        bb.vp_prog_gen = 0x80000000u + s_pw_done;   /* hash it: not an executor version */
        if (prog_get(&bb, &rec.pk))
            s_pw_done++;
    }
    s_prewarming = 0;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    s_pw_ms += (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    if (!s_pw_file) {
        fprintf(stderr, "  [GL] %u shader programs precompiled from %s in %.0f ms (%u from binaries)%s\n",
                s_pw_done, prog_cache_path(), s_pw_ms, s_bin_hits,
                s_pw_bad ? " (file ends in a bad record)" : "");
        cs_report(prog_cache_path());
    }
}

static int ct_start(const char *path, unsigned skip);

static void prog_cache_prewarm(void)
{
    const char *path = prog_cache_path(), *e = getenv("RECOMP_PROG_PREWARM");
    const char *n = getenv("RECOMP_PROG_PREWARM_PER_FRAME");
    int boot;
    if ((e && *e == '0') || !path || !(s_pw_file = fopen(path, "rb")))
        return;
    if (n && atoi(n) > 0)
        s_pw_per_frame = (unsigned)atoi(n);
    boot = prog_bin_init();
    if (e && !strcmp(e, "boot")) boot = 1;
    if (e && !strcmp(e, "defer")) boot = 0;
    {
        const char *t = getenv("RECOMP_GL_COMPILE_THREAD");
        /* Opt-in: on the Switch (Mesa 20.1, nouveau) the renderer's and this
         * context deadlocked inside Mesa at the first logo (2026-10-20). */
        int ct = t && *t == '1';
        if (ct && !(e && (!strcmp(e, "boot") || !strcmp(e, "defer")))) {
            const char *h = getenv("RECOMP_PROG_PREWARM_HEAD");
            unsigned head = h ? (unsigned)atoi(h) : 6;
            if (head)
                prog_cache_prewarm_step(head);   /* the first screens' programs, now */
            if (s_pw_file && ct_start(path, head)) {
                fclose(s_pw_file);
                s_pw_file = NULL;
                fprintf(stderr, "  [GL] shader programs from %s: %u now, the rest on a compile"
                        " thread (RECOMP_GL_COMPILE_THREAD)\n", path, head);
                return;
            }
            if (s_pw_file)
                fprintf(stderr, "  [GL] compile thread unavailable: shader programs from %s:"
                        " %u now, the rest %u a frame once the title starts it\n",
                        path, head, s_pw_per_frame);
            return;
        }
    }
    if (boot) {
        prog_cache_prewarm_step(0);
    } else {
        /* The list is in first-use order: its head is what the first
         * screens draw (MC3: the Rockstar logo). Compiled now, during the
         * black screen; compiled at the logo's first frame instead, the
         * picture came ~0.2 s after its sound (2026-10-19).
         * RECOMP_PROG_PREWARM_HEAD sets how many (default 6). */
        const char *h = getenv("RECOMP_PROG_PREWARM_HEAD");
        unsigned head = h ? (unsigned)atoi(h) : 6;
        if (head)
            prog_cache_prewarm_step(head);
        fprintf(stderr, "  [GL] shader programs from %s: %u now, the rest later, %u a frame,"
                " once the title starts it (nv2a_gl_prewarm_start)\n", path, head, s_pw_per_frame);
    }
}

/* Where a program's compile time goes (RECOMP_GL_COMPILE_STATS=1, or always
 * reported with the precompiled list): vertex compile, fragment compile and
 * link, summed; the three slowest programs' sources are written next to the
 * list (<cache>-slowN.vs/.fs). The Switch's Mesa took ~75 ms a program for
 * sources of ~7 KB + 3.5 KB (2026-10-20): something in them is expensive. */
static double s_cs_vs, s_cs_fs, s_cs_link;
static unsigned s_cs_n;
static struct { double ms; char *vs, *fs; } s_cs_slow[3];

static double cs_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static char *cs_join(const char *const *src, int n)
{
    size_t len = 1;
    int i;
    char *o;
    for (i = 0; i < n; i++) len += strlen(src[i]);
    if (!(o = malloc(len))) return NULL;
    o[0] = 0;
    for (i = 0; i < n; i++) strcat(o, src[i]);
    return o;
}

static void cs_note(double vs, double fs, double link, const char *const *vsrc, int nvs,
                    const char *fsrc)
{
    double ms = vs + fs + link;
    int i, k = -1;
    s_cs_vs += vs; s_cs_fs += fs; s_cs_link += link; s_cs_n++;
    for (i = 0; i < 3; i++)
        if (!s_cs_slow[i].vs || ms > s_cs_slow[i].ms) {
            if (k < 0 || s_cs_slow[i].ms < s_cs_slow[k].ms || !s_cs_slow[i].vs)
                k = i;
            if (!s_cs_slow[i].vs) break;
        }
    if (k < 0 || (s_cs_slow[k].vs && ms <= s_cs_slow[k].ms))
        return;
    free(s_cs_slow[k].vs);
    free(s_cs_slow[k].fs);
    s_cs_slow[k].ms = ms;
    s_cs_slow[k].vs = cs_join(vsrc, nvs);
    s_cs_slow[k].fs = cs_join(&fsrc, 1);
}

/* Print the averages and write the slowest sources (once, after the list). */
static void cs_report(const char *list_path)
{
    char path[600];
    size_t n;
    int i;
    if (!s_cs_n)
        return;
    fprintf(stderr, "  [GL] compile time per program (avg of %u): vertex %.1f ms, fragment %.1f ms,"
            " link %.1f ms\n", s_cs_n, s_cs_vs / s_cs_n, s_cs_fs / s_cs_n, s_cs_link / s_cs_n);
    n = strlen(list_path);
    if (n > 4 && !strcmp(list_path + n - 4, ".bin"))
        n -= 4;
    for (i = 0; i < 3; i++) {
        FILE *f;
        if (!s_cs_slow[i].vs)
            continue;
        snprintf(path, sizeof path, "%.*s-slow%d.vs", (int)n, list_path, i);
        if ((f = fopen(path, "w"))) { fputs(s_cs_slow[i].vs, f); fclose(f); }
        snprintf(path, sizeof path, "%.*s-slow%d.fs", (int)n, list_path, i);
        if ((f = fopen(path, "w"))) { fputs(s_cs_slow[i].fs, f); fclose(f); }
        fprintf(stderr, "  [GL]   slow program %d: %.0f ms -> %.*s-slow%d.vs/.fs\n",
                i, s_cs_slow[i].ms, (int)n, list_path, i);
    }
}

/* Generate, compile and link the program for (b, pk); 0 on failure. Any
 * thread with a current context sharing objects with the renderer's (the
 * compile thread uses its own buffers). bkey != 0: keep it retrievable. */
static GLuint prog_build(const Nv2aRawBatch *b, const Nv2aPshKey *pk, uint64_t vkey,
                         char *vbody, size_t vcap, char *fsrc, size_t fcap, uint64_t bkey)
{
    const char *vs[4];
    int nvs = 0;
    GLuint v, fr, prog;
    GLint ok = 0;
    double t0, t1, t2;
    vs[nvs++] = nv2a_gl_vsh_prelude();
    if (vkey) {
        if (nv2a_gl_vsh_program(b->vp_program, b->vp_slots, b->vp_start, vbody, vcap) < 0)
            return 0;
        vs[nvs++] = vbody;
        vs[nvs++] = nv2a_gl_vsh_main_program();
    } else {
        vs[nvs++] = nv2a_gl_vsh_fixed();
    }
    if (nv2a_gl_psh(pk, fsrc, fcap) < 0)
        return 0;
    t0 = cs_now();
    v = compile(GL_VERTEX_SHADER, vs, nvs);
    t1 = cs_now();
    {
        const char *fs[1] = { fsrc };
        fr = compile(GL_FRAGMENT_SHADER, fs, 1);
    }
    t2 = cs_now();
    if (!v || !fr) {
        if (v) glDeleteShader(v);
        if (fr) glDeleteShader(fr);
        return 0;
    }
    prog = glCreateProgram();
    glAttachShader(prog, v);
    glAttachShader(prog, fr);
    if (bkey && glProgramParameteri)
        glProgramParameteri(prog, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
    glLinkProgram(prog);
    glDeleteShader(v);
    glDeleteShader(fr);
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(prog, sizeof log, NULL, log);
        fprintf(stderr, "  [GL] link failed: %s\n", log);
        glDeleteProgram(prog);
        return 0;
    }
    if (s_prewarming)                        /* the list's programs, on the renderer thread */
        cs_note(t1 - t0, t2 - t1, cs_now() - t2, vs, nvs, fsrc);
    return prog;
}

static GlProg *prog_find(uint64_t vkey, const Nv2aPshKey *pk)
{
    int i;
    for (i = s_prog_head[prog_bucket(vkey, pk)]; i; i = s_prog[i - 1].next) {
        GlProg *c = &s_prog[i - 1];
        if (c->prog && c->vkey == vkey && !memcmp(&c->pkey, pk, sizeof *pk))
            return c;
    }
    return NULL;
}

/* Enter a linked program in the table (renderer thread), evicting the
 * least recently used slot if full; sets its uniforms up. */
static GlProg *prog_register(uint64_t vkey, const Nv2aPshKey *pk, GLuint prog)
{
    GlProg *p = NULL, *victim = NULL;
    uint32_t bucket = prog_bucket(vkey, pk);
    int i;
    for (i = 0; i < GL_MAX_PROG; i++) {
        GlProg *c = &s_prog[i];
        if (!c->prog) { if (!victim || victim->prog) victim = c; }
        else if (!victim || (victim->prog && c->used < victim->used)) victim = c;
    }
    p = victim;
    if (p->prog)
        prog_unlink(p);
    if (p->prog) {
        /* Delete it, not just forget it: an evicted program left alive
         * keeps its code in the driver's GPU code heap for good, and on the
         * Switch's Mesa a full code heap is a hung GPU. */
        glUseProgram(0);
        s_cur_prog = 0;
        glDeleteProgram(p->prog);
        s_prog_live--;
    }
    memset(p, 0, sizeof *p);
    p->prog = prog;
    s_prog_live++;
    s_new_obj = 1;
    p->vkey = vkey;
    p->pkey = *pk;
    p->used = s_frame;
    p->hash = bucket;
    p->next = s_prog_head[bucket];
    s_prog_head[bucket] = (int)(p - s_prog) + 1;
    p->u_c = glGetUniformLocation(p->prog, "c");
    p->n_c = arr_active(p->prog, "c", 192);
    p->n_m = arr_active(p->prog, "u_m", 4);
    p->n_mv = arr_active(p->prog, "u_mv", 4);
    p->n_tscale = arr_active(p->prog, "u_tscale", 4);
    p->n_c0 = arr_active(p->prog, "u_c0", 8);
    p->n_c1 = arr_active(p->prog, "u_c1", 8);
    p->vpc_rows = p->u_c >= 0 && glGetUniformLocation(p->prog, "c[191]") == p->u_c + 191;
    p->u_surf = glGetUniformLocation(p->prog, "u_surf");
    p->u_snapy = glGetUniformLocation(p->prog, "u_snapy");
    p->u_aa = glGetUniformLocation(p->prog, "u_aa");
    p->u_m = glGetUniformLocation(p->prog, "u_m");
    p->u_vpoff = glGetUniformLocation(p->prog, "u_vpoff");
    p->u_xform = glGetUniformLocation(p->prog, "u_xform");
    p->u_mv = glGetUniformLocation(p->prog, "u_mv");
    p->u_fogplane = glGetUniformLocation(p->prog, "u_fogplane");
    p->u_fog0 = glGetUniformLocation(p->prog, "u_fog0");
    p->u_fog1 = glGetUniformLocation(p->prog, "u_fog1");
    p->u_tscale = glGetUniformLocation(p->prog, "u_tscale");
    p->u_c0 = glGetUniformLocation(p->prog, "u_c0");
    p->u_c1 = glGetUniformLocation(p->prog, "u_c1");
    p->u_fogcolor = glGetUniformLocation(p->prog, "u_fogcolor");
    p->u_fc0 = glGetUniformLocation(p->prog, "u_fc0");
    p->u_fc1 = glGetUniformLocation(p->prog, "u_fc1");
    p->u_afunc = glGetUniformLocation(p->prog, "u_alpha_func");
    p->u_aref = glGetUniformLocation(p->prog, "u_alpha_ref");
    glUseProgram(p->prog);
    s_cur_prog = p->prog;
    for (i = 0; i < 4; i++) {
        char n[4] = { 't', (char)('0' + i), 0, 0 };
        p->u_t[i] = glGetUniformLocation(p->prog, n);
        if (p->u_t[i] >= 0)
            glUniform1i(p->u_t[i], i);
    }
    return p;
}

static void ct_drain(void);

static GlProg *prog_get(const Nv2aRawBatch *b, const Nv2aPshKey *pk)
{
    static uint32_t last_gen;
    static uint64_t last_vkey;
    static GlProg *last;
    uint64_t vkey = 0;
    GlProg *p = NULL;
    static char vbody[256 * 1024], fsrc[64 * 1024];
    GLuint prog;
    uint64_t bkey = 0;

    if (b->xform == 2) {
        /* The program's hash walks up to 136 slots: redo it only when the
         * executor says the program changed. */
        if (b->vp_prog_gen != last_gen || !last_vkey) {
            last_vkey = prog_hash(b);
            last_gen = b->vp_prog_gen;
        }
        vkey = last_vkey;
    }
    /* Runs of draws share their program. */
    if (last && last->prog && last->vkey == vkey && !memcmp(&last->pkey, pk, sizeof *pk)) {
        last->used = s_frame;
        return last;
    }
    if ((p = prog_find(vkey, pk)) != NULL) {
        p->used = s_frame;
        last = p;
        return p;
    }
    ct_drain();                              /* the compile thread may have it */
    if ((p = prog_find(vkey, pk)) != NULL) {
        p->used = s_frame;
        last = p;
        return p;
    }
    last = NULL;
    if ((++s_prog_made % 100) == 0)
        fprintf(stderr, "  [GL] %u shader programs compiled (%u live)\n",
                s_prog_made, s_prog_live + 1);
    if (prog_bin_init()) {
        bkey = prog_bin_key(b, pk);
        if ((prog = prog_bin_load(bkey)) != 0)
            goto linked;
    }
    prog = prog_build(b, pk, vkey, vbody, sizeof vbody, fsrc, sizeof fsrc, bkey);
    if (!prog)
        return NULL;
    if (bkey)
        prog_bin_store(bkey, prog);
linked:
    prog_cache_append(b, pk);
    p = prog_register(vkey, pk, prog);
    last = p;
    if (s_trace) {
        static int shown;
        if (shown++ < 4)
            fprintf(stderr, "  [GL] program %016llX linked\n", (unsigned long long)vkey);
    }
    return p;
}

/* ── The shader list compiled on a thread of its own ──────────────────
 *
 * Without program binaries (the Switch's Mesa 20.1) the remembered list
 * has to be compiled at every start: ~108 programs, ~75 ms each. Behind the
 * legal screen, one a frame, that was 59 hitches over 8 s (2026-10-20). A
 * second GL context sharing objects with the renderer's now does it on a
 * thread of its own: it generates, compiles and links each record, and the
 * renderer enters finished programs into its table between frames
 * (ct_drain) -- uniform look-ups only. A draw that needs a program not yet
 * done compiles it itself, as before. RECOMP_GL_COMPILE_THREAD=1 turns it
 * on; off by default: on the Switch (Mesa 20.1, nouveau) the two contexts
 * deadlocked inside Mesa at the first logo (2026-10-20). On the Switch the context
 * is made through EGL without a surface; elsewhere through SDL. */
#if defined(__SWITCH__)
#include <EGL/egl.h>
#include <EGL/eglext.h>
#endif

typedef struct { uint64_t vkey; Nv2aPshKey pk; GLuint prog; } CtDone;
static pthread_mutex_t s_ct_lock = PTHREAD_MUTEX_INITIALIZER;
static CtDone  *s_ct_done;
static unsigned s_ct_n, s_ct_cap, s_ct_skip;
static volatile int s_ct_running;
static char     s_ct_path[512];
#if defined(__SWITCH__)
static EGLDisplay s_ct_dpy;
static EGLContext s_ct_ctx;
#else
static SDL_GLContext s_ct_ctx;
#endif

static void ct_push(uint64_t vkey, const Nv2aPshKey *pk, GLuint prog)
{
    pthread_mutex_lock(&s_ct_lock);
    if (s_ct_n == s_ct_cap) {
        unsigned cap = s_ct_cap ? s_ct_cap * 2 : 64;
        CtDone *n = realloc(s_ct_done, cap * sizeof *n);
        if (!n) { pthread_mutex_unlock(&s_ct_lock); return; }
        s_ct_done = n;
        s_ct_cap = cap;
    }
    s_ct_done[s_ct_n].vkey = vkey;
    s_ct_done[s_ct_n].pk = *pk;
    s_ct_done[s_ct_n].prog = prog;
    s_ct_n++;
    pthread_mutex_unlock(&s_ct_lock);
}

/* Renderer thread: enter what the compile thread finished. */
static void ct_drain(void)
{
    CtDone batch[32];
    unsigned n, i;
    if (!s_ct_n)
        return;
    do {
        pthread_mutex_lock(&s_ct_lock);
        n = s_ct_n < 32 ? s_ct_n : 32;
        memcpy(batch, s_ct_done, n * sizeof *batch);
        memmove(s_ct_done, s_ct_done + n, (s_ct_n - n) * sizeof *batch);
        s_ct_n -= n;
        pthread_mutex_unlock(&s_ct_lock);
        for (i = 0; i < n; i++) {
            if (prog_find(batch[i].vkey, &batch[i].pk))
                glDeleteProgram(batch[i].prog);  /* compiled here meanwhile */
            else
                (void)prog_register(batch[i].vkey, &batch[i].pk, batch[i].prog);
        }
    } while (n == 32);
}

static void *ct_main(void *arg)
{
    static char vbody[256 * 1024], fsrc[64 * 1024];
    struct timespec t0, t1;
    ProgRec rec;
    unsigned idx = 0, made = 0;
    FILE *f;
    (void)arg;
#if defined(__SWITCH__)
    eglBindAPI(EGL_OPENGL_API);
    if (!eglMakeCurrent(s_ct_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, s_ct_ctx)) {
        fprintf(stderr, "  [GL] compile thread: eglMakeCurrent failed (0x%X)\n", eglGetError());
        s_ct_running = 0;
        return NULL;
    }
#else
    if (SDL_GL_MakeCurrent(s_win, s_ct_ctx) != 0) {
        fprintf(stderr, "  [GL] compile thread: %s\n", SDL_GetError());
        s_ct_running = 0;
        return NULL;
    }
#endif
    clock_gettime(CLOCK_MONOTONIC, &t0);
    if ((f = fopen(s_ct_path, "rb"))) {
        while (fread(&rec, sizeof rec, 1, f) == 1) {
            Nv2aRawBatch bb;
            uint64_t vkey;
            GLuint prog;
            if (rec.magic != PROG_REC_MAGIC || rec.slots > 136)
                break;
            if (idx++ < s_ct_skip)               /* compiled at start-up */
                continue;
            memset(&bb, 0, sizeof bb);
            bb.xform = rec.xform;
            bb.vp_program = (const uint32_t (*)[4])rec.prog;
            bb.vp_slots = rec.slots ? rec.slots : 136;
            bb.vp_start = rec.vp_start;
            vkey = bb.xform == 2 ? prog_hash(&bb) : 0;
            prog = prog_build(&bb, &rec.pk, vkey, vbody, sizeof vbody, fsrc, sizeof fsrc, 0);
            if (prog) {
                glFinish();                      /* complete before the renderer uses it */
                ct_push(vkey, &rec.pk, prog);
                made++;
            }
        }
        fclose(f);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    fprintf(stderr, "  [GL] compile thread: %u shader programs from %s in %.0f ms\n", made, s_ct_path,
            (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
#if defined(__SWITCH__)
    eglMakeCurrent(s_ct_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
#else
    SDL_GL_MakeCurrent(s_win, NULL);
#endif
    s_ct_running = 0;
    return NULL;
}

/* Renderer thread, context current: make the shared context and start the
 * thread on the list at path, skipping its first `skip` records. */
static int ct_start(const char *path, unsigned skip)
{
    pthread_t th;
    snprintf(s_ct_path, sizeof s_ct_path, "%s", path);
    s_ct_skip = skip;
#if defined(__SWITCH__)
    {
        EGLContext cur = eglGetCurrentContext();
        EGLint cfg_id = 0, n = 0, attr[16], k = 0;
        EGLint want[] = { EGL_CONFIG_ID, 0, EGL_NONE };
        EGLConfig cfg;
        s_ct_dpy = eglGetCurrentDisplay();
        if (!s_ct_dpy || cur == EGL_NO_CONTEXT
                || !eglQueryContext(s_ct_dpy, cur, EGL_CONFIG_ID, &cfg_id))
            return 0;
        want[1] = cfg_id;
        if (!eglChooseConfig(s_ct_dpy, want, &cfg, 1, &n) || n < 1)
            return 0;
        attr[k++] = EGL_CONTEXT_MAJOR_VERSION_KHR; attr[k++] = 3;
        attr[k++] = EGL_CONTEXT_MINOR_VERSION_KHR; attr[k++] = 3;
        attr[k++] = EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR;
        attr[k++] = EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR;
        attr[k++] = EGL_NONE;
        eglBindAPI(EGL_OPENGL_API);
        s_ct_ctx = eglCreateContext(s_ct_dpy, cfg, cur, attr);
        if (s_ct_ctx == EGL_NO_CONTEXT) {
            fprintf(stderr, "  [GL] compile thread: no shared context (0x%X)\n", eglGetError());
            return 0;
        }
    }
#else
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
    s_ct_ctx = SDL_GL_CreateContext(s_win);       /* makes it current here */
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
    if (!s_ct_ctx) {
        fprintf(stderr, "  [GL] compile thread: no shared context: %s\n", SDL_GetError());
        SDL_GL_MakeCurrent(s_win, s_ctx);
        return 0;
    }
    SDL_GL_MakeCurrent(s_win, s_ctx);
#endif
    s_ct_running = 1;
    if (pthread_create(&th, NULL, ct_main, NULL) != 0) {
        s_ct_running = 0;
        return 0;
    }
    pthread_detach(th);
    return 1;
}

/* ── Context ──────────────────────────────────────────────────────── */

/* SDL's video, the window and the context (run on the main thread, see
 * ready). s_win / s_ctx stay NULL on failure. A context made for another
 * thread is let go of here, so that thread can take it. */
static int s_win_for_other;                  /* made for a thread other than main */

static void gl_make_window(void *unused)
{
    (void)unused;
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "  [GL] SDL video: %s\n", SDL_GetError());
        return;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
#if defined(__APPLE__)
    /* macOS hands out a 3.2+ core context only when asked for a
     * forward-compatible one (GL 2.1 otherwise); it runs GL 4.1 on Metal. */
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    s_win = SDL_CreateWindow("NV2A", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             1280, 720, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE
                             | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!s_win) {
        fprintf(stderr, "  [GL] window: %s\n", SDL_GetError());
        return;
    }
    s_ctx = SDL_GL_CreateContext(s_win);
    if (!s_ctx) {
        fprintf(stderr, "  [GL] context: %s\n", SDL_GetError());
        return;
    }
    if (s_win_for_other)
        SDL_GL_MakeCurrent(s_win, NULL);
}

static int ready(void)
{
    if (s_state)
        return s_state > 0;
    s_state = -1;
    s_trace = getenv("RECOMP_GL_TRACE") != NULL;
    if (nv2a_gl_adopt_window((void **)&s_win, (void **)&s_ctx)) {
        /* The host already made the window and context (a loading screen)
         * and has let go of the context: take it on this thread. */
        if (SDL_GL_MakeCurrent(s_win, s_ctx) != 0) {
            fprintf(stderr, "  [GL] adopting the window: %s\n", SDL_GetError());
            return 0;
        }
    } else {
        /* SDL's video, the window and the context on the main thread
         * (platform/host_main.c: macOS allows no other), the context then
         * made current here. Elsewhere a direct call, as before. */
        s_win_for_other = !xbox_host_on_main();
        xbox_host_main_call(gl_make_window, NULL);
        if (!s_win || !s_ctx)
            return 0;
        if (s_win_for_other && SDL_GL_MakeCurrent(s_win, s_ctx) != 0) {
            fprintf(stderr, "  [GL] context to the renderer thread: %s\n", SDL_GetError());
            return 0;
        }
    }
    SDL_GL_SetSwapInterval(0);
    if (nv2a_gl_load(SDL_GL_GetProcAddress) != 0)
        return 0;
    {
        extern void nv2a_gl_check_install(void);   /* gl_check.c: RECOMP_GL_ERRORS=2 */
        nv2a_gl_check_install();
    }
    fprintf(stderr, "  [GL] %s / %s / %s\n", (const char *)glGetString(GL_VENDOR),
            (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));
    {
        const char *e = getenv("RECOMP_GL_SCALE");
        GLint mt = 0, mr = 0;
        double k = e ? strtod(e, NULL) : 1.0;
        if (!(k > 0.0)) k = 1.0;                    /* unset, 0 or garbage */
        s_scale = k < 0.5 ? 0.5 : k > 5.0 ? 5.0 : k;   /* 5: 3600x2400, a 4K screen */
        glGetIntegerv(0x0D33, &mt);                 /* GL_MAX_TEXTURE_SIZE */
        glGetIntegerv(0x84E8, &mr);                 /* GL_MAX_RENDERBUFFER_SIZE */
        s_max_size = mt > 0 && mr > 0 ? (mt < mr ? mt : mr) : 4096;
        if (s_scale != 1.0)
            fprintf(stderr, "  [GL] rendering at %gx (RECOMP_GL_SCALE), surfaces up to %d\n",
                    s_scale, s_max_size);
    }
    {
        /* Which size presenting will use. SDL reports the window as created;
         * a host with a better answer (the Switch's EGL surface, which
         * follows the display) supplies it through nv2a_gl_screen_size. */
        int dw = 0, dh = 0, sw = 0, sh = 0;
        SDL_GL_GetDrawableSize(s_win, &dw, &dh);
        if (!nv2a_gl_screen_size(&sw, &sh)) { sw = dw; sh = dh; }
        fprintf(stderr, "  [GL] screen %dx%d, SDL drawable %dx%d\n", sw, sh, dw, dh);
    }
    glGenVertexArrays(1, &s_vao);
    glBindVertexArray(s_vao);
    glGenBuffers(1, &s_vbo);
    glGenBuffers(1, &s_ibo);
    s_vring.buf = s_vbo;
    s_iring.buf = s_ibo;
    if (!ring_persist(&s_vring)) {           /* each on its own: storage is immutable */
        glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
        glBufferData(GL_ARRAY_BUFFER, s_vring.cap, NULL, GL_STREAM_DRAW);
    }
    if (!ring_persist(&s_iring)) {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, s_iring.cap, NULL, GL_STREAM_DRAW);
    }
    s_state = 1;
    prog_cache_prewarm();
    return 1;
}

/* Called once, when the renderer first needs the display. A host that shows
 * something of its own while the title boots (the Switch's loading screen)
 * overrides this: it stops drawing, releases its GL context, and hands back
 * its SDL window and context (as void *) for the renderer to use. Returning
 * 0 lets the renderer create its own. */
__attribute__((weak)) int nv2a_gl_adopt_window(void **win, void **ctx)
{
    (void)win; (void)ctx;
    return 0;
}

/* Called on a present with nothing of the title's to show. Returns 1 if the
 * host drew something (its loading screen) into the default framebuffer. */
__attribute__((weak)) int nv2a_gl_draw_placeholder(void)
{
    return 0;
}

/* The on-screen size, if the host knows it better than SDL. */
__attribute__((weak)) int nv2a_gl_screen_size(int *w, int *h)
{
    (void)w; (void)h;
    return 0;
}

/* ── Render state ─────────────────────────────────────────────────── */

static float byte_f(uint32_t v, int shift) { return (float)((v >> shift) & 0xFF) / 255.0f; }

static void argb_vec4(uint32_t v, float out[4])
{
    out[0] = byte_f(v, 16); out[1] = byte_f(v, 8);
    out[2] = byte_f(v, 0);  out[3] = byte_f(v, 24);
}

static GLenum blend_eq(uint32_t v)
{
    switch (v) {
    case 0x800A: case 0x800B: case 0x8007: case 0x8008: return v;
    default: return GL_FUNC_ADD;             /* also the signed variants */
    }
}

/* The render state as apply_state sets it. Consecutive draws mostly share
 * it, and re-setting ~20 pieces of GL state per draw -- about 900 draws a
 * frame in a race -- is driver work for nothing. Anything else that touches
 * this state (clears, presents, new surfaces) calls state_dirty(). */
typedef struct {
    uint32_t blend, bsrc, bdst, beq, bcolor;
    uint32_t depth, dfunc, dmask;
    uint32_t stencil, smask, sfunc, sref, sread, sop[3];
    uint32_t cull, cullface, front, cm;
    GLint    sc[4];                     /* scissor, stored pixels; sc[2] 0 = off */
} GlState;
static GlState s_st;
static int s_st_valid;

static void state_dirty(void) { s_st_valid = 0; tex_bind_forget(); }

/* SET_SURFACE_CLIP as a scissor in s's stored pixels (sc[2] = 0: the clip
 * covers the surface). NFSU2's split screen draws each player's world with
 * the clip set to that player's half; the vertex programs place it there
 * but do not stop at the edge, so without the scissor each view spilled
 * over the other. xemu scissors the same way. */
static void surface_scissor(const uint32_t *r, const GlSurf *s, GLint sc[4])
{
    uint32_t x0 = (r[0x200 / 4] & 0xFFFF) * s->aa_sx, x1 = x0 + (r[0x200 / 4] >> 16) * s->aa_sx;
    uint32_t y0 = (r[0x204 / 4] & 0xFFFF) * s->aa_sy, y1 = y0 + (r[0x204 / 4] >> 16) * s->aa_sy;

    sc[0] = sc[1] = sc[2] = sc[3] = 0;
    if (x1 > s->w) x1 = s->w;
    if (y1 > s->h) y1 = s->h;
    if (x1 <= x0 || y1 <= y0 || (x0 == 0 && y0 == 0 && x1 == s->w && y1 == s->h))
        return;
    sc[0] = to_stored(x0, s->pw, s->w);
    sc[1] = to_stored(y0, s->ph, s->h);
    sc[2] = to_stored(x1, s->pw, s->w) - sc[0];
    sc[3] = to_stored(y1, s->ph, s->h) - sc[1];
}

static void apply_state(const uint32_t *r, int has_depth, const GlSurf *s)
{
    uint32_t cm = r[0x358 / 4];
    GlState want;

    memset(&want, 0, sizeof want);
    surface_scissor(r, s, want.sc);
    if (r[0x304 / 4]) {
        want.blend = 1; want.bsrc = r[0x344 / 4]; want.bdst = r[0x348 / 4];
        want.beq = r[0x350 / 4]; want.bcolor = r[0x34C / 4];
    }
    if (has_depth && r[0x30C / 4]) {
        want.depth = 1; want.dfunc = r[0x354 / 4];
    }
    want.dmask = has_depth && r[0x35C / 4];
    if (has_depth && r[0x32C / 4]) {
        want.stencil = 1; want.smask = r[0x360 / 4]; want.sfunc = r[0x364 / 4];
        want.sref = r[0x368 / 4]; want.sread = r[0x36C / 4];
        want.sop[0] = r[0x370 / 4]; want.sop[1] = r[0x374 / 4]; want.sop[2] = r[0x378 / 4];
    }
    if (r[0x308 / 4]) {
        want.cull = 1; want.cullface = r[0x39C / 4]; want.front = r[0x3A0 / 4];
    }
    want.cm = cm;
    if (s_st_valid && !memcmp(&want, &s_st, sizeof want))
        return;
    s_draw_changed = 1;
    s_st = want;
    s_st_valid = 1;

    if (r[0x304 / 4]) {
        float bc[4];
        glEnable(GL_BLEND);
        glBlendFunc(r[0x344 / 4], r[0x348 / 4]);
        glBlendEquation(blend_eq(r[0x350 / 4]));
        argb_vec4(r[0x34C / 4], bc);
        glBlendColor(bc[0], bc[1], bc[2], bc[3]);
    } else {
        glDisable(GL_BLEND);
    }
    if (has_depth && r[0x30C / 4]) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(r[0x354 / 4] ? r[0x354 / 4] : 0x203);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    glDepthMask(has_depth && r[0x35C / 4] ? 1 : 0);
    if (has_depth && r[0x32C / 4]) {
        glEnable(GL_STENCIL_TEST);
        glStencilMask(r[0x360 / 4] & 0xFF);
        glStencilFunc(r[0x364 / 4] ? r[0x364 / 4] : GL_ALWAYS, (GLint)(r[0x368 / 4] & 0xFF),
                      r[0x36C / 4] & 0xFF);
        glStencilOp(r[0x370 / 4] ? r[0x370 / 4] : GL_KEEP,
                    r[0x374 / 4] ? r[0x374 / 4] : GL_KEEP,
                    r[0x378 / 4] ? r[0x378 / 4] : GL_KEEP);
    } else {
        glDisable(GL_STENCIL_TEST);
    }
    if (r[0x308 / 4]) {
        glEnable(GL_CULL_FACE);
        glCullFace(r[0x39C / 4] ? r[0x39C / 4] : GL_BACK);
        /* Rows are top-first here, which mirrors the winding. */
        glFrontFace(r[0x3A0 / 4] == GL_CCW ? GL_CW : GL_CCW);
    } else {
        glDisable(GL_CULL_FACE);
    }
    glColorMask((cm & 0x00010000u) != 0, (cm & 0x00000100u) != 0,
                (cm & 0x00000001u) != 0, (cm & 0x01000000u) != 0);
    if (want.sc[2] > 0) {               /* rows are top-first, as in clears */
        glEnable(GL_SCISSOR_TEST);
        glScissor(want.sc[0], want.sc[1], want.sc[2], want.sc[3]);
    } else {
        glDisable(GL_SCISSOR_TEST);
    }
    /* The NV2A does not clip against the near and far planes; it clamps
     * depth. GL clips there, so a screen quad placed at the far plane
     * (NFSU2's loading screen, z = 1.0 through its vertex program) vanished
     * whenever rounding put it a hair beyond. */
    glEnable(0x864F);                               /* GL_DEPTH_CLAMP */
}

static void psh_key(const uint32_t *r, Nv2aPshKey *k)
{
    int i;

    memset(k, 0, sizeof *k);
    for (i = 0; i < 8; i++) {
        k->color_icw[i] = r[0xAC0 / 4 + i];
        k->color_ocw[i] = r[0x1E40 / 4 + i];
        k->alpha_icw[i] = r[0x260 / 4 + i];
        k->alpha_ocw[i] = r[0xAA0 / 4 + i];
    }
    k->control = r[0x1E60 / 4];
    k->final0 = r[0x288 / 4];
    k->final1 = r[0x28C / 4];
    k->shader_program = r[0x1E70 / 4];
    k->other_input = r[0x1E78 / 4];
}

/* NV097 primitive -> GL, rewriting the ones core GL lacks. */
static uint32_t *s_prim_idx;
static size_t    s_prim_cap;

static GLenum gl_prim(const Nv2aRawBatch *b, const uint32_t **idx, uint32_t *n)
{
    uint32_t i, m = 0;

    *idx = b->indices;
    *n = b->index_count;
    switch (b->prim) {
    case 1: return 0x0000;             /* POINTS */
    case 2: return 0x0001;             /* LINES */
    case 3: return 0x0002;             /* LINE_LOOP */
    case 4: return 0x0003;             /* LINE_STRIP */
    case 5: return GL_TRIANGLES;
    case 6: return 0x0005;             /* TRIANGLE_STRIP */
    case 7: case 10: return 0x0006;    /* TRIANGLE_FAN, POLYGON */
    case 9: return 0x0005;             /* QUAD_STRIP == triangle strip */
    case 8: {                          /* QUADS -> triangles */
        size_t need = (size_t)b->index_count / 4 * 6;
        if (need > s_prim_cap) {
            free(s_prim_idx);
            s_prim_cap = need + 1024;
            s_prim_idx = (uint32_t *)malloc(s_prim_cap * 4);
        }
        if (!s_prim_idx) { *n = 0; return GL_TRIANGLES; }
        for (i = 0; i + 3 < b->index_count; i += 4) {
            const uint32_t *q = b->indices + i;
            s_prim_idx[m++] = q[0]; s_prim_idx[m++] = q[1]; s_prim_idx[m++] = q[2];
            s_prim_idx[m++] = q[0]; s_prim_idx[m++] = q[2]; s_prim_idx[m++] = q[3];
        }
        *idx = s_prim_idx;
        *n = m;
        return GL_TRIANGLES;
    }
    default:
        *n = 0;
        return GL_TRIANGLES;
    }
}

/* ── Back end entry points ────────────────────────────────────────── */

static uint32_t zmax_of(const uint32_t *r)
{
    return (((r[0x208 / 4] >> 4) & 0xF) == 1) ? 0xFFFFu : 0xFFFFFFu;
}

/* Streaming vertex and index data.
 *
 * glBufferData per draw re-specifies the buffer every time -- a driver
 * allocation, twice per draw, ~900 draws a frame. Instead each buffer is one
 * large allocation filled front to back: a draw maps its own range without
 * synchronisation (nothing the GPU may still read is ever overwritten) and
 * the buffer is orphaned only when it is full. */

/* Space for `size` bytes; returns the offset, and a pointer to write them to
 * (NULL if mapping failed: then ring_put the data instead). */
/* Persistent streaming (GL 4.4 / ARB_buffer_storage; RECOMP_GL_PERSIST=0
 * off): each buffer is immutable storage mapped once, for good, and a draw
 * writes straight into it -- no map and unmap per draw. On the Switch those
 * two calls per buffer per draw were a good part of the "vertices" phase
 * (10.5 ms of a 55 ms race frame, 2026-10-26). The buffer is used in
 * quarters: leaving one fences it, entering one waits for its fence, so
 * nothing the GPU may still read is written over. */
static void ring_enter(GlRing *g, int s)
{
    if (g->fence[g->seg] == NULL)
        g->fence[g->seg] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    if (g->fence[s]) {
        glClientWaitSync(g->fence[s], GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
        glDeleteSync(g->fence[s]);
        g->fence[s] = NULL;
    }
    g->seg = s;
}

static int ring_persist(GlRing *g)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_GL_PERSIST");
        on = !(e && *e == '0') && glBufferStorage && glFenceSync && glClientWaitSync
             && glDeleteSync && SDL_GL_ExtensionSupported("GL_ARB_buffer_storage");
        fprintf(stderr, "  [GL] streaming buffers: %s\n",
                on ? "persistent mapping (ARB_buffer_storage)" : "mapped per draw");
    }
    if (!on)
        return 0;
    glBindBuffer(g->target, g->buf);
    glBufferStorage(g->target, g->cap, NULL,
                    GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT
                    | GL_DYNAMIC_STORAGE_BIT);
    g->pmap = (uint8_t *)glMapBufferRange(g->target, 0, g->cap,
                                          GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT
                                          | GL_MAP_COHERENT_BIT);
    g->off = 0;
    g->seg = 0;
    return g->pmap != NULL;
}

static GLintptr ring_alloc(GlRing *g, GLsizeiptr size, void **ptr)
{
    GLintptr at;

    glBindBuffer(g->target, g->buf);
    if (g->pmap && size <= g->cap / RING_SEGS) {
        GLsizeiptr q = g->cap / RING_SEGS;
        int end;
        at = (g->off + 63) & ~(GLintptr)63;
        if (at + size > g->cap) {           /* wrap: through the rest to 0 */
            while (g->seg != RING_SEGS - 1)
                ring_enter(g, g->seg + 1);
            ring_enter(g, 0);
            at = 0;
        }
        end = (int)((at + size - 1) / q);
        while (g->seg != end)
            ring_enter(g, g->seg + 1);
        g->off = at + size;
        *ptr = g->pmap + at;
        return at;
    }
    if (g->pmap) {
        /* Bigger than a quarter (never seen): the whole buffer is waited
         * for, then the data copied to its start; the ring goes on after. */
        int k;
        for (k = 0; k < RING_SEGS; k++)
            ring_enter(g, (g->seg + 1) % RING_SEGS);
        if (size > g->cap)
            size = g->cap;
        g->seg = (int)((size - 1) / (g->cap / RING_SEGS));
        g->off = size;
        *ptr = NULL;
        return 0;
    }
    if (size > g->cap) {
        g->cap = size * 2;
        g->off = g->cap;                    /* forces the orphan below */
    }
    at = (g->off + 63) & ~(GLintptr)63;
    if (at + size > g->cap) {
        glBufferData(g->target, g->cap, NULL, GL_STREAM_DRAW);
        at = 0;
    }
    g->off = at + size;
    {
        /* RECOMP_GL_MAP=0: copy with glBufferSubData instead of writing
         * through a mapping (the callers' fallback) -- for a driver whose
         * unsynchronised maps go wrong (forwarding drivers: Parallels). */
        static int map = -1;
        if (map < 0) {
            const char *e = getenv("RECOMP_GL_MAP");
            map = !(e && *e == '0');
            if (!map)
                fprintf(stderr, "  [GL] vertex and index data copied, not mapped (RECOMP_GL_MAP=0)\n");
        }
        if (!map) {
            *ptr = NULL;
            return at;
        }
    }
    *ptr = glMapBufferRange(g->target, at, size,
                            GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT
                            );
    /* No GL_MAP_INVALIDATE_RANGE_BIT: nothing the GPU may still read is
     * ever overwritten, and with it nouveau gave each map a staging buffer
     * of its own (nouveau_bo_new + map + free: ~3% of the GL thread). */
    return at;
}

static void ring_done(GlRing *g, GLintptr at, GLsizeiptr size, void *ptr,
                      const void *fallback)
{
    if (ptr && g->pmap)
        return;                             /* persistent: written in place */
    if (ptr)
        glUnmapBuffer(g->target);
    else if (fallback)
        glBufferSubData(g->target, at, size, fallback);
}

/* Which vertex attribute arrays are enabled in s_vao (only this file
 * touches it): enable and disable only on change. */
static uint32_t s_attr_on;
static void attr_enable(uint32_t a, int on)
{
    uint32_t bit = 1u << a;
    if (!!(s_attr_on & bit) == !!on)
        return;
    if (on) { glEnableVertexAttribArray(a); s_attr_on |= bit; }
    else    { glDisableVertexAttribArray(a); s_attr_on &= ~bit; }
}

/* Vertex data for one draw, in one ring allocation.
 *
 * Attributes in b->attr_direct go up as the title stored them: every NV097
 * type but the packed CMP normal is a GL vertex format with the same
 * meaning (D3DCOLOR is GL_BGRA; S1 normalises with GL's max(c/32767, -1)).
 * Attributes that share a vertex buffer -- interleaved, the usual case --
 * are copied once, as the byte range they span together, from a 4-aligned
 * start so each keeps its alignment. The rest (CMP, and every attribute for
 * a back end without direct batches) are packed float4 from b->attrs. */
/* The byte ranges the direct attributes of `b` span, merged where they
 * overlap (interleaved attributes share one), each from a 4-aligned start;
 * of_run[a] says which range attribute a is in. Returns the range count. */
typedef struct { const uint8_t *lo, *hi; GLintptr at; } VRun;
static uint32_t vertex_runs(const Nv2aRawBatch *b, VRun *run, int *of_run)
{
    uint32_t nv = b->vertex_count, nrun = 0, a, i;

    for (a = 0; a < NV2A_RAW_ATTRS; a++) {
        const uint8_t *lo, *hi;
        uint32_t elem;
        if (!(b->attr_direct & (1u << a)))
            continue;
        switch (b->direct[a].type) {
        case 0: elem = 4; break;
        case 2: elem = 4 * b->direct[a].size; break;
        case 4: elem = b->direct[a].size; break;
        default: elem = 2 * b->direct[a].size; break;
        }
        lo = (const uint8_t *)((uintptr_t)b->direct[a].ptr & ~(uintptr_t)3);
        hi = b->direct[a].ptr + (size_t)(nv - 1) * b->direct[a].stride + elem;
        for (i = 0; i < nrun; i++)
            if (lo <= run[i].hi && run[i].lo <= hi)
                break;
        if (i == nrun) {
            run[nrun].lo = lo;
            run[nrun].hi = hi;
            nrun++;
        } else {
            if (lo < run[i].lo) run[i].lo = lo;
            if (hi > run[i].hi) run[i].hi = hi;
        }
        of_run[a] = (int)i;
    }
    /* A widened run can now overlap another: merge until none do. */
    for (i = 0; i + 1 < nrun; ) {
        uint32_t j;
        for (j = i + 1; j < nrun; j++)
            if (run[j].lo <= run[i].hi && run[i].lo <= run[j].hi)
                break;
        if (j == nrun) { i++; continue; }
        /* Fold run j into run i, then move the last run into slot j. */
        if (run[j].lo < run[i].lo) run[i].lo = run[j].lo;
        if (run[j].hi > run[i].hi) run[i].hi = run[j].hi;
        for (a = 0; a < NV2A_RAW_ATTRS; a++) {
            if (!(b->attr_direct & (1u << a)))
                continue;
            if (of_run[a] == (int)j)
                of_run[a] = (int)i;
            else if (of_run[a] == (int)(nrun - 1))
                of_run[a] = (int)j;
        }
        run[j] = run[nrun - 1];
        nrun--;
    }
    return nrun;
}

static int upload_vertices(const Nv2aRawBatch *b)
{
    VRun run[NV2A_RAW_ATTRS];
    int of_run[NV2A_RAW_ATTRS];
    uint32_t slot[NV2A_RAW_ATTRS], np = 0, nrun = 0, a, i, v;
    uint32_t nv = b->vertex_count;
    uint16_t conv = b->attr_present & (uint16_t)~b->attr_direct;
    GLsizeiptr bytes = 0, pack_at;
    GLintptr at;
    uint8_t *dst;
    void *map;
    static uint8_t *staging;
    static size_t staging_cap;

    if (!nv)
        return 0;
    nrun = vertex_runs(b, run, of_run);
    for (i = 0; i < nrun; i++) {
        run[i].at = bytes;
        bytes += ((GLsizeiptr)(run[i].hi - run[i].lo) + 15) & ~(GLsizeiptr)15;
    }
    for (a = 0; a < NV2A_RAW_ATTRS; a++)
        if (conv & (1u << a))
            slot[np++] = a;
    pack_at = bytes;
    bytes += (GLsizeiptr)nv * np * 4 * sizeof(float);
    if (!bytes)
        return 0;

    at = ring_alloc(&s_vring, bytes, &map);
    dst = (uint8_t *)map;
    if (!dst) {
        if ((size_t)bytes > staging_cap) {
            free(staging);
            staging_cap = (size_t)bytes + 65536;
            staging = (uint8_t *)malloc(staging_cap);
            if (!staging) { staging_cap = 0; return 0; }
        }
        dst = staging;
    }
    for (i = 0; i < nrun; i++)
        memcpy(dst + run[i].at, run[i].lo, (size_t)(run[i].hi - run[i].lo));
    if (np) {
        float *out = (float *)(dst + pack_at);
        for (v = 0; v < nv; v++)
            for (i = 0; i < np; i++)
                memcpy(out + ((size_t)v * np + i) * 4,
                       b->attrs + ((size_t)v * NV2A_RAW_ATTRS + slot[i]) * 4,
                       4 * sizeof(float));
    }
    ring_done(&s_vring, at, bytes, map, staging);

    for (a = 0; a < NV2A_RAW_ATTRS; a++) {
        GLintptr off;
        GLint size;
        GLenum type;
        GLboolean norm;
        if (!(b->attr_present & (1u << a))) {
            attr_enable(a, 0);
            continue;
        }
        attr_enable(a, 1);
        if (!(b->attr_direct & (1u << a)))
            continue;
        off = at + run[of_run[a]].at + (b->direct[a].ptr - run[of_run[a]].lo);
        size = (GLint)b->direct[a].size;
        switch (b->direct[a].type) {
        case 0:  size = GL_BGRA; type = GL_UNSIGNED_BYTE; norm = GL_TRUE; break;
        case 1:  type = GL_SHORT; norm = GL_TRUE; break;
        case 2:  type = GL_FLOAT; norm = GL_FALSE; break;
        case 4:  type = GL_UNSIGNED_BYTE; norm = GL_TRUE; break;
        default: type = GL_SHORT; norm = GL_FALSE; break;
        }
        glVertexAttribPointer(a, size, type, norm, (GLsizei)b->direct[a].stride,
                              (const void *)(uintptr_t)off);
    }
    for (i = 0; i < np; i++)
        glVertexAttribPointer(slot[i], 4, GL_FLOAT, GL_FALSE,
                              (GLsizei)(np * 4 * sizeof(float)),
                              (const void *)(uintptr_t)(at + pack_at + i * 4 * sizeof(float)));
    return 1;
}

static void gl_draw_raw(const Nv2aRawBatch *b)
{
    GlSurf *s;
    GlProg *p;
    Nv2aPshKey pk;
    const uint32_t *r = b->regs;
    const uint32_t *idx;
    uint32_t n, a, i;
    GLintptr idx_at;
    GLenum mode, idx_type = GL_UNSIGNED_INT;
    float tscale[4][2], c0[8][4], c1[8][4], fog[4], surf[4], aa[2], m[16], snapy;

    if (!ready())
        return;
    s_regs = r;
    s_batch = b;
    depth_tex_prepare(r, b);
    pf_start();
    s_draw_changed = 0;
    s = surf_bind(&b->surface, b->zeta_va);
    pf_mark(PF_SURF);
    if (!s)
        return;
    s_drew_any = 1;
    ft_note(s->va, 'd');
    psh_key(r, &pk);
    if (s_trace) {
        /* Texture modes not yet done in gl_psh.c: bump, dot product, cube. */
        static uint32_t seen[32];
        static int nseen;
        uint32_t sp = pk.shader_program, t, odd = 0;
        int k2;
        for (t = 0; t < 4; t++) {
            uint32_t m = (sp >> (5 * t)) & 0x1F;
            if (m == 3 || (m >= 6 && m <= 14) || m == 17)
                odd = 1;
        }
        for (k2 = 0; k2 < nseen && seen[k2] != sp; k2++) { }
        if (odd && k2 == nseen && nseen < 32) {
            seen[nseen++] = sp;
            fprintf(stderr, "  [GL] texture modes 0x%05X (stages %u %u %u %u) prim %u n %u"
                    " fmt %02X %02X %02X %02X bumpmat1 %08X %08X %08X %08X\n",
                    sp, sp & 0x1F, (sp >> 5) & 0x1F, (sp >> 10) & 0x1F, (sp >> 15) & 0x1F,
                    b->prim, b->index_count,
                    (r[0x1B04 / 4] >> 8) & 0xFF, (r[0x1B44 / 4] >> 8) & 0xFF,
                    (r[0x1B84 / 4] >> 8) & 0xFF, (r[0x1BC4 / 4] >> 8) & 0xFF,
                    r[0x1B68 / 4], r[0x1B6C / 4], r[0x1B70 / 4], r[0x1B74 / 4]);
        }
    }
    {
        uint32_t made = s_prog_made;
        uint64_t t0 = hz_now();
        p = prog_get(b, &pk);
        if (s_prog_made != made) {
            s_hz_progs += s_prog_made - made;
            s_hz_prog_ns += hz_now() - t0;
        }
    }
    if (!p)
        return;
    s_hz_draws++;
    if (p->prog != s_cur_prog) {
        glUseProgram(p->prog);
        s_cur_prog = p->prog;
        s_draw_changed = 1;
    }
    pf_mark(PF_PROG);

    {
        uint32_t texs = s_hz_texs;
        uint64_t t0 = hz_now();
        for (i = 0; i < 4; i++)
            bind_stage(r, (int)i, tscale[i]);
        if (s_hz_texs != texs)
            s_hz_tex_ns += hz_now() - t0;
    }
    pf_mark(PF_TEX);
    for (i = 0; i < 8; i++) {
        argb_vec4(r[0xA60 / 4 + i], c0[i]);
        argb_vec4(r[0xA80 / 4 + i], c1[i]);
    }
    {
        uint32_t fc = r[0x2A8 / 4];            /* R in the low byte */
        fog[0] = byte_f(fc, 0); fog[1] = byte_f(fc, 8);
        fog[2] = byte_f(fc, 16); fog[3] = byte_f(fc, 24);
    }
    surf[0] = 2.0f / (float)s->w;
    surf[1] = 2.0f / (float)s->h;
    surf[2] = 1.0f / (float)zmax_of(r);
    surf[3] = 0.5f - 0.5f * (float)s->w / (float)s->pw;   /* see nv2a_snap */
    snapy = 0.5f - 0.5f * (float)s->h / (float)s->ph;     /* differs: square pixels */
    aa[0] = b->aa_sx > 0 ? b->aa_sx : 1.0f;
    aa[1] = b->aa_sy > 0 ? b->aa_sy : 1.0f;
    memcpy(m, b->composite, sizeof m);

    /* 192 constants are 3 KB, and most draws in a row share them: upload
     * only when they changed since this program last got them. */
    if (p->u_c >= 0 && !(p->vpc_valid && p->vpc_gen == b->vp_const_gen)) {
        /* Same executor version: same contents, no compare needed. */
        s_pf_cdraws++;
        s_draw_changed = 1;
        if (!p->vpc_valid || !p->vpc_rows) {
            memcpy(p->vpc, b->vp_consts, sizeof p->vpc);
            s_pf_crows += (uint32_t)p->n_c;
            glUniform4fv(p->u_c, p->n_c, &b->vp_consts[0][0]);
        } else {
            /* Only the rows that changed, as runs: a draw usually changes a
             * matrix or two of the 192, and sending all 3 KB each time was
             * copied again by Mesa and by nouveau's constbuf upload. */
            int r = 0;
            while (r < p->n_c) {
                int e;
                if (!memcmp(p->vpc[r], b->vp_consts[r], 16)) { r++; continue; }
                for (e = r + 1; e < p->n_c && memcmp(p->vpc[e], b->vp_consts[e], 16); e++) { }
                memcpy(p->vpc[r], b->vp_consts[r], (size_t)(e - r) * 16);
                s_pf_crows += (uint32_t)(e - r);
                glUniform4fv(p->u_c + r, e - r, &b->vp_consts[r][0]);
                r = e;
            }
        }
        p->vpc_valid = 1;
        p->vpc_gen = b->vp_const_gen;
    }
    {
        /* The rest, likewise: set only when something differs from what this
         * program already holds. */
        struct GlUni u;
        memset(&u, 0, sizeof u);
        memcpy(u.surf, surf, sizeof u.surf);
        u.snapy = snapy;
        memcpy(u.aa, aa, sizeof u.aa);
        memcpy(u.m, m, sizeof u.m);
        memcpy(u.vpoff, b->vp_offset, sizeof u.vpoff);
        memcpy(u.tscale, tscale, sizeof u.tscale);
        memcpy(u.c0, c0, sizeof u.c0);
        memcpy(u.c1, c1, sizeof u.c1);
        memcpy(u.fog, fog, sizeof u.fog);
        argb_vec4(r[0x1E20 / 4], u.fc[0]);      /* SPECULAR_FOG_FACTOR0/1 */
        argb_vec4(r[0x1E24 / 4], u.fc[1]);
        u.aref = (float)(r[0x340 / 4] & 0xFF);
        u.xform = b->xform == 1 ? 1 : 0;
        if (p->u_fog0 >= 0)                     /* every program applies the fog unit */
            nv2a_ffp_fog_uniforms(r, u.mv, u.fogplane, u.fog0, u.fog1);
        u.afunc = r[0x300 / 4] ? (GLint)r[0x33C / 4] : 0x207;
        if (!p->uni_valid || memcmp(&u, &p->uni, sizeof u) != 0) {
            s_pf_udraws++;
            s_draw_changed = 1;
            p->uni = u;
            p->uni_valid = 1;
            if (p->u_surf >= 0) glUniform4fv(p->u_surf, 1, u.surf);
            if (p->u_snapy >= 0) glUniform1f(p->u_snapy, u.snapy);
            if (p->u_aa >= 0) glUniform2fv(p->u_aa, 1, u.aa);
            if (p->u_m >= 0) glUniform4fv(p->u_m, p->n_m, u.m);
            if (p->u_vpoff >= 0) glUniform4fv(p->u_vpoff, 1, u.vpoff);
            if (p->u_xform >= 0) glUniform1i(p->u_xform, u.xform);
            if (p->u_mv >= 0) glUniform4fv(p->u_mv, p->n_mv, u.mv);
            if (p->u_fogplane >= 0) glUniform4fv(p->u_fogplane, 1, u.fogplane);
            if (p->u_fog0 >= 0) glUniform4fv(p->u_fog0, 1, u.fog0);
            if (p->u_fog1 >= 0) glUniform4fv(p->u_fog1, 1, u.fog1);
            if (p->u_tscale >= 0) glUniform2fv(p->u_tscale, p->n_tscale, &u.tscale[0][0]);
            if (p->u_c0 >= 0) glUniform4fv(p->u_c0, p->n_c0, &u.c0[0][0]);
            if (p->u_c1 >= 0) glUniform4fv(p->u_c1, p->n_c1, &u.c1[0][0]);
            if (p->u_fogcolor >= 0) glUniform4fv(p->u_fogcolor, 1, u.fog);
            if (p->u_fc0 >= 0) glUniform4fv(p->u_fc0, 1, u.fc[0]);
            if (p->u_fc1 >= 0) glUniform4fv(p->u_fc1, 1, u.fc[1]);
            if (p->u_afunc >= 0) glUniform1i(p->u_afunc, u.afunc);
            if (p->u_aref >= 0) glUniform1f(p->u_aref, u.aref);
        }
    }

    {
        /* RECOMP_GL_WATCH=<hex va>: the state of draws that render into or
         * sample that surface/texture (first 24).
         * RECOMP_GL_WATCH=all: every draw of one frame -- "the whole scene
         * comes out wrong" has no address to watch yet. Which frame:
         * RECOMP_GL_WATCH_FRAME=<n>, or =key for the frame on screen when F12
         * is pressed in the window (Linux). RECOMP_GL_WATCH_N=<n> caps the
         * count, or with "all" keeps going past that frame up to n draws. */
        static int watch_init, watch_n, watch_max = 24, watch_all, watch_key, watch_capped;
        static uint32_t watch_va, watch_frame = 0xFFFFFFFFu;
        if (!watch_init) {
            const char *w = getenv("RECOMP_GL_WATCH");
            const char *e;
            watch_init = 1;
            watch_all = w && !strcmp(w, "all");
            watch_va = w && !watch_all ? (uint32_t)strtoul(w, NULL, 16) : 0;
            e = getenv("RECOMP_GL_WATCH_FRAME");
            watch_key = e && !strcmp(e, "key");
            if (watch_all && !watch_key)
                watch_frame = e ? (uint32_t)strtoul(e, NULL, 10) : 0;
            if ((e = getenv("RECOMP_GL_WATCH_N")) != NULL) {
                watch_max = atoi(e);
                watch_capped = 1;
            } else if (watch_all) {
                watch_max = 100000;             /* the whole frame */
            }
            if (w && watch_key)
                fprintf(stderr, "  [WATCH] every draw of the frame on screen at F12%s\n",
                        watch_capped ? " onwards (capped by RECOMP_GL_WATCH_N)" : "");
            else if (w)
                fprintf(stderr, "  [WATCH] watching %s from frame %u, up to %d draws\n",
                        watch_all ? "every draw" : w, watch_all ? watch_frame : 0, watch_max);
        }
#if (!defined(_WIN32) || defined(XBOXRECOMP_SDL_HOST)) && !defined(__SWITCH__)
        if (watch_all && watch_key && watch_frame == 0xFFFFFFFFu
            && xbox_sdl_keyboard_down(SDL_SCANCODE_F12)) {
            watch_frame = s_frame;
            fprintf(stderr, "  [WATCH] F12: dumping frame %u\n", watch_frame);
        }
#endif
        s_watch_dumping = watch_n < watch_max && watch_all && watch_frame != 0xFFFFFFFFu
                          && (watch_capped ? s_frame >= watch_frame : s_frame == watch_frame);
        if (watch_n < watch_max &&
            ((watch_all && watch_frame != 0xFFFFFFFFu &&
              (watch_capped ? s_frame >= watch_frame : s_frame == watch_frame)) ||
             (watch_va && (s->va == watch_va || b->tex_va[0] == watch_va ||
              b->tex_va[1] == watch_va || b->tex_va[2] == watch_va ||
              b->tex_va[3] == watch_va)))) {
            watch_n++;
            fprintf(stderr, "  [WATCH] surf %08X %ux%u prim %u n %u xform %u attrs %04X direct %04X"
                    " modes %05X other %08X ctl %08X\n"
                    "          c0 icw %08X ocw %08X a icw %08X ocw %08X final %08X %08X\n",
                    s->va, s->w, s->h, b->prim, b->index_count, b->xform, b->attr_present,
                    b->attr_direct, pk.shader_program, pk.other_input, pk.control,
                    pk.color_icw[0], pk.color_ocw[0], pk.alpha_icw[0], pk.alpha_ocw[0],
                    pk.final0, pk.final1);
            {
                /* Every combiner stage in use, with its constant colours. */
                uint32_t st, nst = pk.control & 0xF;
                for (st = 1; st < nst && st < 8; st++)
                    fprintf(stderr, "          c%u icw %08X ocw %08X a icw %08X ocw %08X"
                            " factor0 %08X factor1 %08X\n", st,
                            pk.color_icw[st], pk.color_ocw[st], pk.alpha_icw[st],
                            pk.alpha_ocw[st], r[0xA60 / 4 + st], r[0xA80 / 4 + st]);
                fprintf(stderr, "          stages %u factor0 %08X factor1 %08X | lighting %u"
                        " lights %04X colormat %08X ambient %g %g %g\n", nst,
                        r[0xA60 / 4], r[0xA80 / 4], r[0x314 / 4], r[0x3BC / 4] & 0xFFFF,
                        r[0x298 / 4], *(const float *)&r[0xA10 / 4],
                        *(const float *)&r[0xA14 / 4], *(const float *)&r[0xA18 / 4]);
            }
            for (i = 0; i < 4; i++) {
                uint32_t base = (0x1B00u + i * 0x40u) / 4;
                if (!(r[base + 3] & 0x40000000u))
                    continue;
                fprintf(stderr, "          t%u va %08X%s fmt %08X ctl0 %08X ctl1 %08X filt %08X rect %08X"
                        " tscale %g %g\n", i, b->tex_va[i], surf_find(b->tex_va[i]) ? " (surface)" : "",
                        r[base + 1], r[base + 3], r[base + 4], r[base + 5], r[base + 7],
                        tscale[i][0], tscale[i][1]);
                if (((r[base + 1] >> 8) & 0xFF) == 0x0B) {
                    /* Palettized: the palette it decodes through, and the
                     * colours its first texels come out as (0x00000000 for
                     * all of them is a texture that draws as nothing). */
                    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
                    uint32_t pv = b->pal_va[i], k, c;
                    fprintf(stderr, "             palette reg %08X va %08X:", r[base + 8], pv);
                    for (k = 0; pv && k < 4; k++) {
                        memcpy(&c, mem + pv + k * 4, 4);
                        fprintf(stderr, " %08X", c);
                    }
                    fprintf(stderr, " | first indices");
                    for (k = 0; b->tex_va[i] && k < 8; k++)
                        fprintf(stderr, " %02X", mem[b->tex_va[i] + k]);
                    fprintf(stderr, "\n");
                }
            }
            fprintf(stderr, "          4UB spec %08X diff %08X 4F spec %08X %08X %08X %08X fog en %u fc0 %08X fc1 %08X\n",
                    r[0x1940 / 4 + 4], r[0x1940 / 4 + 3], r[0x1A40 / 4], r[0x1A44 / 4],
                    r[0x1A48 / 4], r[0x1A4C / 4], r[0x29C / 4], r[0x1E20 / 4], r[0x1E24 / 4]);
            {
                float mv[16], pl[4], f0[4], f1[4];
                nv2a_ffp_fog_uniforms(r, mv, pl, f0, f1);
                fprintf(stderr, "          fog on %g gen %g eq %g abs %g params %g %g plane %g %g %g %g color %08X\n",
                        f0[0], f0[1], f0[2], f0[3], f1[0], f1[1], pl[0], pl[1], pl[2], pl[3],
                        r[0x2A8 / 4]);
            }
            fprintf(stderr, "          blend %u %04X %04X alphatest %u func %X ref %u zmask %u ztest %u cmask %08X\n",
                    r[0x304 / 4], r[0x344 / 4], r[0x348 / 4], r[0x300 / 4], r[0x33C / 4], r[0x340 / 4],
                    r[0x35C / 4], r[0x30C / 4], r[0x358 / 4]);
            s_watch_hit = 1;
            {
                /* Stage 0's image where v0's texcoords point, if it is a surface. */
                GlSurf *src = surf_find(b->tex_va[0]);
                float u0 = 0, v0 = 0;
                if (src && (b->attr_direct & 0x200) && b->direct[9].type == 2 && b->direct[9].ptr) {
                    uint8_t px[4];
                    int q;
                    memcpy(&u0, b->direct[9].ptr, 4);
                    memcpy(&v0, b->direct[9].ptr + 4, 4);
                    glBindFramebuffer(0x8CA8, src->fbo);         /* GL_READ_FRAMEBUFFER */
                    fprintf(stderr, "          src %ux%u at (%g,%g)+4/12/20/28:", src->w, src->h, u0, v0);
                    for (q = 0; q < 4; q++) {
                        GLint xx = (GLint)u0 + 4 + q * 8, yy = (GLint)v0 + 4 + q * 8;
                        if (xx < 0 || yy < 0 || xx >= (GLint)src->w || yy >= (GLint)src->h) continue;
                        glReadPixels(to_stored((uint32_t)xx, src->pw, src->w), to_stored((uint32_t)yy, src->ph, src->h), 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
                        fprintf(stderr, " %02X%02X%02X%02X", px[0], px[1], px[2], px[3]);
                    }
                    fprintf(stderr, "\n");
                    glBindFramebuffer(0x8CA8, s->fbo);
                }
            }
            {
                static const int wa[4] = { 0, 3, 9, 10 };
                uint32_t v, j;
                for (v = 0; v < b->vertex_count && v < 4; v++) {
                    fprintf(stderr, "          v%u", v);
                    for (j = 0; j < 4; j++) {
                        int a2 = wa[j];
                        if (!(b->attr_direct & (1u << a2)) || !b->direct[a2].ptr)
                            continue;
                        const uint8_t *q = b->direct[a2].ptr + (size_t)v * b->direct[a2].stride;
                        if (b->direct[a2].type == 2) {
                            float f4[4] = { 0 };
                            memcpy(f4, q, 4 * (b->direct[a2].size < 4 ? b->direct[a2].size : 4));
                            fprintf(stderr, " a%d/%u(%g %g %g %g)", a2, (unsigned)b->direct[a2].size,
                                    f4[0], f4[1], f4[2], f4[3]);
                        } else {
                            uint32_t w4;
                            memcpy(&w4, q, 4);
                            fprintf(stderr, " a%d(t%u %08X)", a2, b->direct[a2].type, w4);
                        }
                    }
                    fprintf(stderr, "\n");
                }
                /* A vertex program's output at the first vertices, from the
                 * executor's CPU interpreter: where they land on the surface
                 * (x, y in pixels after the divide by w), and with what
                 * colour and texcoord. Off-surface or w <= 0 is invisible. */
                if (b->xform == 2) {
                    extern int nv2a_vp_debug_vertex(uint32_t, float[4], float[4], float[4]);
                    for (v = 0; v < b->vertex_count && v < 4; v++) {
                        float p4[4], d4[4], t4[4];
                        int ok = nv2a_vp_debug_vertex(v, p4, d4, t4);
                        float w = p4[3] != 0.0f ? p4[3] : 1.0f;
                        fprintf(stderr, "          vp v%u%s oPos %g %g %g %g -> screen %g %g | oD0 %g %g %g %g"
                                " | oT0 %g %g %g %g\n", v, ok ? "" : " (no image)",
                                p4[0], p4[1], p4[2], p4[3], p4[0] / w, p4[1] / w,
                                d4[0], d4[1], d4[2], d4[3], t4[0], t4[1], t4[2], t4[3]);
                    }
                }
                /* Fixed function: the composite matrix this draw is
                 * transformed by (world x view x projection x viewport, as
                 * D3D uploads it) and where the first vertices land in
                 * surface pixels (before the viewport offset). A small
                 * model filling the screen has the wrong matrix here. */
                if (b->xform == 1 && (b->attr_direct & 1) && b->direct[0].type == 2
                    && b->direct[0].ptr) {
                    const float *cm4 = b->composite;
                    fprintf(stderr, "          composite r0 %g %g %g %g | r1 %g %g %g %g | r2 %g %g %g %g | r3 %g %g %g %g\n",
                            cm4[0], cm4[1], cm4[2], cm4[3], cm4[4], cm4[5], cm4[6], cm4[7],
                            cm4[8], cm4[9], cm4[10], cm4[11], cm4[12], cm4[13], cm4[14], cm4[15]);
                    fprintf(stderr, "          ffp screen:");
                    for (v = 0; v < b->vertex_count && v < 4; v++) {
                        float pv[4] = { 0, 0, 0, 1 }, o4[4];
                        int k4;
                        const uint8_t *q = b->direct[0].ptr + (size_t)v * b->direct[0].stride;
                        memcpy(pv, q, 4 * (b->direct[0].size < 4 ? b->direct[0].size : 4));
                        if (b->direct[0].size < 4) pv[3] = 1.0f;
                        for (k4 = 0; k4 < 4; k4++)
                            o4[k4] = cm4[4 * k4] * pv[0] + cm4[4 * k4 + 1] * pv[1]
                                   + cm4[4 * k4 + 2] * pv[2] + cm4[4 * k4 + 3] * pv[3];
                        fprintf(stderr, " (%g, %g w %g)", o4[3] != 0 ? o4[0] / o4[3] : 0,
                                o4[3] != 0 ? o4[1] / o4[3] : 0, o4[3]);
                    }
                    fprintf(stderr, "\n");
                    {   /* All vertices: the screen box of those in front of
                         * the eye, the smallest w, and how many are at or
                         * behind it -- triangles joining those stretch
                         * across the screen if they are not clipped. */
                        float xmin = 1e30f, xmax = -1e30f, ymin = 1e30f, ymax = -1e30f, wmin = 1e30f;
                        uint32_t behind = 0, vv;
                        for (vv = 0; vv < b->vertex_count; vv++) {
                            float pv[4] = { 0, 0, 0, 1 }, o4[4];
                            int k4;
                            const uint8_t *q = b->direct[0].ptr + (size_t)vv * b->direct[0].stride;
                            memcpy(pv, q, 4 * (b->direct[0].size < 4 ? b->direct[0].size : 4));
                            if (b->direct[0].size < 4) pv[3] = 1.0f;
                            for (k4 = 0; k4 < 4; k4++)
                                o4[k4] = cm4[4 * k4] * pv[0] + cm4[4 * k4 + 1] * pv[1]
                                       + cm4[4 * k4 + 2] * pv[2] + cm4[4 * k4 + 3] * pv[3];
                            if (o4[3] < wmin) wmin = o4[3];
                            if (o4[3] <= 1e-6f) { behind++; continue; }
                            if (o4[0] / o4[3] < xmin) xmin = o4[0] / o4[3];
                            if (o4[0] / o4[3] > xmax) xmax = o4[0] / o4[3];
                            if (o4[1] / o4[3] < ymin) ymin = o4[1] / o4[3];
                            if (o4[1] / o4[3] > ymax) ymax = o4[1] / o4[3];
                        }
                        fprintf(stderr, "          ffp all %u vertices: x %g..%g y %g..%g, w min %g,"
                                " %u at or behind the eye\n", b->vertex_count, xmin, xmax, ymin, ymax,
                                wmin, behind);
                    }
                }
                /* The fixed-function fog factor at the first vertices: 0 is
                 * all fog colour, 1 none. */
                if (b->xform == 1 && (b->attr_direct & 1) && b->direct[0].type == 2
                    && b->direct[0].ptr) {
                    fprintf(stderr, "          ffp fog factor:");
                    for (v = 0; v < b->vertex_count && v < 4; v++) {
                        float pos[4] = { 0, 0, 0, 1 };
                        const uint8_t *q = b->direct[0].ptr + (size_t)v * b->direct[0].stride;
                        memcpy(pos, q, 4 * (b->direct[0].size < 4 ? b->direct[0].size : 4));
                        if (b->direct[0].size < 4) pos[3] = 1.0f;
                        fprintf(stderr, " %g", nv2a_ffp_fog_factor(r, pos, 0.0f));
                    }
                    fprintf(stderr, "\n");
                }
            }
            if (b->attrs && !(b->attr_direct & 0x0209)) {
                uint32_t v;
                for (v = 0; v < b->vertex_count && v < 4; v++) {
                    const float *at = b->attrs + (size_t)v * 64;
                    fprintf(stderr, "          v%u pos %g %g %g %g d %g %g %g %g t0 %g %g %g %g t1 %g %g\n", v,
                            at[0], at[1], at[2], at[3], at[12], at[13], at[14], at[15],
                            at[36], at[37], at[38], at[39], at[40], at[41]);
                }
            }
        }
    }

    pf_mark(PF_UNIF);
    apply_state(r, b->zeta_va != 0, s);
    pf_mark(PF_STATE);

    glBindVertexArray(s_vao);
    if (!upload_vertices(b))
        return;
    {
        /* The absent attributes read the title's SET_VERTEX_DATA* values
         * (Nv2aRawBatch.attr_const); set them only when those or the set of
         * attributes changed. A fixed (0,0,0,1) gave NFSU2's lens drops a
         * specular alpha, so fog 1, of 1 instead of 0: their screen copy
         * came out as the fog colour -- grey squares. */
        static uint32_t last_mask = 0xFFFFFFFFu, last_gen;
        if (b->attr_present != last_mask || b->attr_const_gen != last_gen) {
            s_draw_changed = 1;
            last_mask = b->attr_present;
            last_gen = b->attr_const_gen;
            for (a = 0; a < NV2A_RAW_ATTRS; a++)
                if (!(b->attr_present & (1u << a)))
                    glVertexAttrib4f(a, b->attr_const[a][0], b->attr_const[a][1],
                                     b->attr_const[a][2], b->attr_const[a][3]);
        }
    }
    mode = gl_prim(b, &idx, &n);
    if (!n)
        return;
    {
        /* 16-bit indices whenever the batch's vertices allow: half the bytes
         * through the ring, and the GPU's native index size. */
        static uint16_t *idx16;
        static size_t idx16_cap;
        void *dst;
        idx_type = b->vertex_count <= 65536 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;
        if (idx_type == GL_UNSIGNED_INT) {
            idx_at = ring_alloc(&s_iring, (GLsizeiptr)n * 4, &dst);
            if (dst)
                memcpy(dst, idx, (size_t)n * 4);
            ring_done(&s_iring, idx_at, (GLsizeiptr)n * 4, dst, idx);
        } else {
            uint16_t *o;
            idx_at = ring_alloc(&s_iring, (GLsizeiptr)n * 2, &dst);
            o = (uint16_t *)dst;
            if (!o) {
                if (n > idx16_cap) {
                    free(idx16);
                    idx16_cap = n + 4096;
                    idx16 = (uint16_t *)malloc(idx16_cap * 2);
                    if (!idx16) { idx16_cap = 0; return; }
                }
                o = idx16;
            }
            for (i = 0; i < n; i++)
                o[i] = (uint16_t)idx[i];
            ring_done(&s_iring, idx_at, (GLsizeiptr)n * 2, dst, idx16);
        }
    }
    {
        /* RECOMP_GL_FINISH=1: name each draw before it is submitted and wait
         * for the GPU to finish it. With a synchronous log, the last line
         * of a hard GPU hang is the draw that caused it. Slow. */
        static uint32_t draws;
        if (s_finish < 0)
            s_finish = getenv("RECOMP_GL_FINISH") != NULL;
        ++draws;
        if (s_finish && s_new_obj)
            fprintf(stderr, "  [GL] draw %u: surf %08X %ux%u prim %u idx %u verts %u attrs %04X"
                    " xform %u vp %u shaders %08X combiners %08X tex0 %08X/%02X\n",
                    draws, s->va, s->w, s->h, b->prim, n, b->vertex_count,
                    b->attr_present, b->xform, b->vp_start, pk.shader_program, pk.control,
                    r[0x1B00 / 4], (r[0x1B04 / 4] >> 8) & 0xFF);
        pf_mark(PF_VTX);
        if (s_pf_on > 0) {
            /* Same as the previous draw in everything but its vertices? */
            static const GlSurf *last_s;
            static GLuint last_ds;
            static uint32_t last_frame = 0xFFFFFFFFu, last_sig[2 + 3 * NV2A_RAW_ATTRS];
            static GLenum last_mode;
            uint32_t sig[2 + 3 * NV2A_RAW_ATTRS];
            memset(sig, 0, sizeof sig);
            sig[0] = b->attr_present;
            sig[1] = b->attr_direct;
            for (a = 0; a < NV2A_RAW_ATTRS; a++)
                if (b->attr_direct & (1u << a)) {
                    sig[2 + 3 * a] = b->direct[a].type;
                    sig[3 + 3 * a] = b->direct[a].size;
                    sig[4 + 3 * a] = b->direct[a].stride;
                }
            if (!s_draw_changed && last_frame == s_frame && last_s == s && last_ds == s->ds_on
                && last_mode == mode && !memcmp(sig, last_sig, sizeof sig))
                s_pf_same++;
            last_s = s; last_ds = s->ds_on; last_frame = s_frame; last_mode = mode;
            memcpy(last_sig, sig, sizeof sig);
        }
        glDrawElements(mode, (GLsizei)n, idx_type, (const void *)(uintptr_t)idx_at);
        pf_mark(PF_DRAW);
        if (s_pf_on > 0) {
            s_pf_draws++;
            s_pf_verts += b->vertex_count;
            s_pf_idx += n;
        }
        gl_step_done("draw");
        if (s_watch_hit)
            watch_shot(s);
        if (s_watch_hit) {
            /* What the watched draw left: the target's centre and corner. */
            uint8_t px[4];
            float x0 = 0, y0 = 0;
            int q;
            s_watch_hit = 0;
            if ((b->attr_direct & 1) && b->direct[0].type == 2 && b->direct[0].ptr) {
                memcpy(&x0, b->direct[0].ptr, 4);
                memcpy(&y0, b->direct[0].ptr + 4, 4);
            }
            fprintf(stderr, "          after (x,y from v0 + 4/12/20/28):");
            for (q = 0; q < 4; q++) {
                GLint xx = (GLint)(x0 + 0.5f) + 4 + q * 8, yy = (GLint)(y0 + 0.5f) + 4 + q * 8;
                if (xx < 0 || yy < 0 || xx >= (GLint)s->w || yy >= (GLint)s->h) continue;
                glReadPixels(to_stored((uint32_t)xx, s->pw, s->w), to_stored((uint32_t)yy, s->ph, s->h), 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
                fprintf(stderr, " %02X%02X%02X%02X", px[0], px[1], px[2], px[3]);
            }
            fprintf(stderr, "\n");
        }
    }
}

static void gl_clear(const Nv2aSurface *sf, const Nv2aRenderState *rs,
                     uint32_t flags, uint32_t argb, uint32_t zstencil)
{
    GlSurf *s;
    GLbitfield bits = 0;
    float c[4];

    state_dirty();

    if (!ready())
        return;
    s = surf_bind(sf, rs ? rs->zeta_va : 0);
    if (!s)
        return;
    ft_note(s->va, 'c');
    if (s_watch_dumping)
        fprintf(stderr, "  [WATCH] clear surf %08X %ux%u flags %02X (z %u stencil %u color %X) argb %08X"
                " zs %08X zeta %08X rect h %08X v %08X\n", s->va, s->w, s->h, flags, flags & 1,
                (flags >> 1) & 1, (flags >> 4) & 0xF, argb, zstencil, rs ? rs->zeta_va : 0,
                s_regs ? s_regs[0x1D98 / 4] : 0, s_regs ? s_regs[0x1D9C / 4] : 0);
    glDisable(GL_SCISSOR_TEST);
    if (s_regs) {
        uint32_t hz = s_regs[0x1D98 / 4], vt = s_regs[0x1D9C / 4];
        /* The clear rectangle is in logical pixels, like SURFACE_CLIP; the
         * surface is larger by the anti-aliasing factor. Unscaled, a 2x1
         * surface cleared only its left half, and stale depth on the right
         * kept old frames of a moving car alive behind the background. */
        uint32_t x0 = (hz & 0xFFFF) * s->aa_sx, x1 = ((hz >> 16) + 1) * s->aa_sx;
        uint32_t y0 = (vt & 0xFFFF) * s->aa_sy, y1 = ((vt >> 16) + 1) * s->aa_sy;
        if (x1 > s->w) x1 = s->w;
        if (y1 > s->h) y1 = s->h;
        if (x1 > x0 && y1 > y0 && (x1 - x0 < s->w || y1 - y0 < s->h)) {
            GLint px0 = to_stored(x0, s->pw, s->w), py0 = to_stored(y0, s->ph, s->h);
            glEnable(GL_SCISSOR_TEST);
            glScissor(px0, py0, to_stored(x1, s->pw, s->w) - px0,
                      to_stored(y1, s->ph, s->h) - py0);
        }
    }
    if (flags & 0xF0) {
        argb_vec4(argb, c);
        if (gl_probe()) { c[0] = 1.0f; c[1] = 0.0f; c[2] = 0.0f; c[3] = 1.0f; }   /* red */
        glColorMask((flags & 0x10) != 0, (flags & 0x20) != 0,
                    (flags & 0x40) != 0, (flags & 0x80) != 0);
        glClearColor(c[0], c[1], c[2], c[3]);
        bits |= GL_COLOR_BUFFER_BIT;
    }
    if (flags & 0x1) {
        uint32_t zmax = s_regs ? zmax_of(s_regs) : 0xFFFFFFu;
        uint32_t z = zmax == 0xFFFFu ? (zstencil & 0xFFFF) : (zstencil >> 8);
        glDepthMask(1);
        glClearDepth((double)z / (double)zmax);
        bits |= GL_DEPTH_BUFFER_BIT;
    }
    if (flags & 0x2) {
        glStencilMask(0xFF);
        glClearStencil((GLint)(zstencil & 0xFF));
        bits |= GL_STENCIL_BUFFER_BIT;
    }
    if (bits)
        glClear(bits);
    gl_step_done("clear");
    glDisable(GL_SCISSOR_TEST);
}

static void dump_bmp(const char *path, const uint8_t *bgra, uint32_t w, uint32_t h)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        /* Said once: a dump to a folder that is not there wrote nothing,
         * silently (a run starts in work/, so work/logs/... was work/work/,
         * 2026-10-24). */
        static int told;
        if (!told++)
            fprintf(stderr, "  [GL] cannot write %s: frame dumps lost (does the folder exist?)\n", path);
    }
    uint8_t hdr[54] = { 'B', 'M' };
    uint32_t row = w * 3, pad = (4 - (row & 3)) & 3, size = 54 + (row + pad) * h, y, x;

    if (!f)
        return;
    memcpy(hdr + 2, &size, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, 54, f);
    /* BMP rows are bottom-first; the surface is top-first. */
    for (y = h; y-- > 0; ) {
        const uint8_t *p = bgra + (size_t)y * w * 4;
        for (x = 0; x < w; x++)
            fwrite(p + x * 4, 1, 3, f);
        fwrite("\0\0\0", 1, pad, f);
    }
    fclose(f);
}

/* RECOMP_GL_WATCH_SHOTS=<prefix>: during a watched frame (RECOMP_GL_WATCH),
 * the screen-sized target as a BMP after every 50th draw into it, named by
 * the draw's place in the dump (<prefix>_d0050.bmp ...). Where a picture
 * goes wrong between two shots, the draws between them in the log are the
 * suspects -- for a frame whose final image is wrong but whose every draw
 * looks plausible on its own (MC3's race view: the 3D scene drawn, then
 * gone, 2D on top). */
static void watch_shot(const GlSurf *s)
{
    static int on = -1, n;
    static char prefix[256];
    static uint8_t *pix;
    static size_t cap;
    if (on < 0) {
        const char *e = getenv("RECOMP_GL_WATCH_SHOTS");
        on = e && *e;
        if (on) snprintf(prefix, sizeof prefix, "%s", e);
    }
    if (!on || s->w < 640)
        return;
    if (++n % 50)
        return;
    {
        size_t need = (size_t)s->pw * s->ph * 4;
        char path[320];
        if (need > cap) {
            free(pix);
            cap = need;
            pix = (uint8_t *)malloc(cap);
        }
        if (!pix)
            return;
        glBindFramebuffer(GL_READ_FRAMEBUFFER, s->fbo);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(0, 0, (GLsizei)s->pw, (GLsizei)s->ph, GL_BGRA, GL_UNSIGNED_BYTE, pix);
        snprintf(path, sizeof path, "%s_d%04d.bmp", prefix, n);
        dump_bmp(path, pix, s->pw, s->ph);
        fprintf(stderr, "          [shot] %s (screen draw %d)\n", path, n);
    }
}

/* ── FXAA in the present pass ─────────────────────────────────────────
 *
 * Present was a scaled blit, so every jagged edge of the title went to the
 * screen as it was. Now (unless RECOMP_GL_FXAA=0) it is a full-screen pass
 * that does FXAA 3.11 (Lottes): find the local luma contrast, decide
 * whether the edge runs horizontally or vertically, walk along it to both
 * ends, and blend across it by how far this pixel is from the nearer end
 * (plus a sub-pixel term for single-pixel detail) -- while scaling to the
 * screen in the same step. Written from the published algorithm; nfsu2-sw's
 * Vulkan renderer does the same in its present pass (the idea). */
static GLuint s_fx_prog, s_fx_vao;
static volatile int s_fill_hold;             /* nv2a_gl_fill_hold */

/* While > 0, 4:3 screens are shown as the title framed them (RECOMP_GL_FILL
 * off): a host keeps chosen movies in their original format. Any thread. */
void nv2a_gl_fill_hold(int delta)
{
    __atomic_add_fetch(&s_fill_hold, delta, __ATOMIC_RELAXED);
}

static GLint  s_fx_rcp, s_fx_max, s_fx_sharp, s_fx_full, s_fx_crop, s_fx_band, s_fx_fill;
static float  s_fx_sharpness = -1.0f;
static int    s_fx_on = -1;

static int fxaa_init(void)
{
    static const char *vs[1] = {
        "#version 330 core\n"
        "out vec2 vT;\n"
        "void main() {\n"
        "    vec2 t = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
        "    vT = vec2(t.x, 1.0 - t.y);                        /* rows top-first */\n"
        "    gl_Position = vec4(t * 2.0 - 1.0, 0.0, 1.0);\n"
        "}\n" };
    static const char *fs[1] = {
        "#version 330 core\n"
        "uniform sampler2D u_tex;\n"
        "uniform vec2 u_rcp;           /* one texel */\n"
        "uniform vec2 u_max;           /* last drawn texel centre */\n"
        "uniform float u_sharp;        /* CAS weight: 0 = off */\n"
        "uniform vec4 u_full;          /* u0 u1 v0 v1: the drawn picture */\n"
        "uniform vec4 u_crop;          /* ... its 4:3 middle, filled to 16:9 */\n"
        "uniform vec2 u_band;          /* u of the left and right side bands */\n"
        "uniform int u_fill;\n"
        "in vec2 vT;\n"
        "out vec4 o;\n"
        "vec3 S(vec2 p) { return texture(u_tex, clamp(p, 0.5 * u_rcp, u_max)).rgb; }\n"
        "float L(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }\n"
        "float LA(vec2 p) { return L(S(p)); }\n"
        "/* Both side bands black from top to bottom: a 4:3 screen the title\n"
        " * pillarboxed itself. Same texels for every pixel: cache hits. */\n"
        "bool bars() {\n"
        "    for (int k = 0; k < 8; k++) {\n"
        "        float v = mix(u_full.z, u_full.w, (float(k) + 0.5) / 8.0);\n"
        "        vec3 a = texture(u_tex, vec2(u_band.x, v)).rgb, b = texture(u_tex, vec2(u_band.y, v)).rgb;\n"
        "        if (max(max(a.r, a.g), max(a.b, max(b.r, max(b.g, b.b)))) > 0.008) return false;\n"
        "    }\n"
        "    return true;\n"
        "}\n"
        "void main() {\n"
        "    vec4 R = (u_fill != 0 && bars()) ? u_crop : u_full;\n"
        "    vec2 uv = vec2(mix(R.x, R.y, vT.x), mix(R.z, R.w, vT.y)), r = u_rcp;\n"
        "    vec3 cM = S(uv);\n"
        "    float m = L(cM);\n"
        "    float n = LA(uv + vec2(0.0, -r.y)), s = LA(uv + vec2(0.0, r.y));\n"
        "    float e = LA(uv + vec2(r.x, 0.0)), w = LA(uv + vec2(-r.x, 0.0));\n"
        "    float lo = min(m, min(min(n, s), min(e, w)));\n"
        "    float hi = max(m, max(max(n, s), max(e, w)));\n"
        "    float range = hi - lo;\n"
        "    if (range < max(0.0312, hi * 0.125)) {\n"
        "        /* no edge: contrast-adaptive sharpening (the cross around it) */\n"
        "        if (u_sharp > 0.0) {\n"
        "            vec3 cn = S(uv + vec2(0.0, -r.y)), cs = S(uv + vec2(0.0, r.y));\n"
        "            vec3 ce = S(uv + vec2(r.x, 0.0)), cw = S(uv + vec2(-r.x, 0.0));\n"
        "            vec3 mn = min(cM, min(min(cn, cs), min(ce, cw)));\n"
        "            vec3 mx = max(cM, max(max(cn, cs), max(ce, cw)));\n"
        "            vec3 amp = sqrt(clamp(min(mn, 1.0 - mx) / max(mx, vec3(1e-4)), 0.0, 1.0));\n"
        "            vec3 wt = -amp * u_sharp;         /* the negative lobe */\n"
        "            cM = clamp((cM + wt * (cn + cs + ce + cw)) / (1.0 + 4.0 * wt), 0.0, 1.0);\n"
        "        }\n"
        "        o = vec4(cM, 1.0); return;\n"
        "    }\n"
        "    float nw = LA(uv + vec2(-r.x, -r.y)), ne = LA(uv + vec2(r.x, -r.y));\n"
        "    float sw = LA(uv + vec2(-r.x, r.y)), se = LA(uv + vec2(r.x, r.y));\n"
        "    /* edge direction: second differences across rows vs columns */\n"
        "    float eh = abs(nw + sw - 2.0 * w) + 2.0 * abs(n + s - 2.0 * m) + abs(ne + se - 2.0 * e);\n"
        "    float ev = abs(nw + ne - 2.0 * n) + 2.0 * abs(w + e - 2.0 * m) + abs(sw + se - 2.0 * s);\n"
        "    bool horz = eh >= ev;\n"
        "    float a = horz ? n : w, b = horz ? s : e;          /* the two sides */\n"
        "    float ga = a - m, gb = b - m;\n"
        "    bool sideA = abs(ga) >= abs(gb);\n"
        "    float grad = 0.25 * max(abs(ga), abs(gb));\n"
        "    float stp = horz ? r.y : r.x;\n"
        "    float side = sideA ? a : b;\n"
        "    if (sideA) stp = -stp;\n"
        "    float mid = 0.5 * (m + side);                     /* luma on the edge */\n"
        "    vec2 p0 = uv, dir = horz ? vec2(r.x, 0.0) : vec2(0.0, r.y);\n"
        "    if (horz) p0.y += 0.5 * stp; else p0.x += 0.5 * stp;\n"
        "    vec2 pn = p0 - dir, pp = p0 + dir;\n"
        "    float en = LA(pn) - mid, ep = LA(pp) - mid;\n"
        "    bool dn = abs(en) >= grad, dp = abs(ep) >= grad;\n"
        "    const float Q[11] = float[](1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0);\n"
        "    for (int i = 0; i < 11 && !(dn && dp); i++) {\n"
        "        if (!dn) { pn -= dir * Q[i]; en = LA(pn) - mid; dn = abs(en) >= grad; }\n"
        "        if (!dp) { pp += dir * Q[i]; ep = LA(pp) - mid; dp = abs(ep) >= grad; }\n"
        "    }\n"
        "    float distN = horz ? uv.x - pn.x : uv.y - pn.y;\n"
        "    float distP = horz ? pp.x - uv.x : pp.y - uv.y;\n"
        "    bool nearN = distN < distP;\n"
        "    float dist = min(distN, distP), span = distN + distP;\n"
        "    bool good = ((nearN ? en : ep) < 0.0) != (m - mid < 0.0);\n"
        "    float off = good ? 0.5 - dist / span : 0.0;\n"
        "    /* sub-pixel: how much this pixel differs from its neighbourhood */\n"
        "    float avg = (2.0 * (n + s + e + w) + nw + ne + sw + se) / 12.0;\n"
        "    float sp = clamp(abs(avg - m) / range, 0.0, 1.0);\n"
        "    sp = (-2.0 * sp + 3.0) * sp * sp;\n"
        "    off = max(off, sp * sp * 0.75);\n"
        "    vec2 f = uv;\n"
        "    if (horz) f.y += off * stp; else f.x += off * stp;\n"
        "    o = vec4(S(f), 1.0);\n"
        "}\n" };
    GLuint v, f;
    GLint ok = 0;
    if (s_fx_prog)
        return 1;
    if (s_fx_on < 0) {
        const char *e = getenv("RECOMP_GL_FXAA");
        s_fx_on = !(e && *e == '0');
        fprintf(stderr, "  [GL] FXAA %s (RECOMP_GL_FXAA)\n", s_fx_on ? "on" : "off");
    }
    if (!s_fx_on)
        return 0;
    v = compile(GL_VERTEX_SHADER, vs, 1);
    f = compile(GL_FRAGMENT_SHADER, fs, 1);
    if (!v || !f) {
        s_fx_on = 0;
        return 0;
    }
    s_fx_prog = glCreateProgram();
    glAttachShader(s_fx_prog, v);
    glAttachShader(s_fx_prog, f);
    glLinkProgram(s_fx_prog);
    glDeleteShader(v);
    glDeleteShader(f);
    glGetProgramiv(s_fx_prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        fprintf(stderr, "  [GL] FXAA link failed: plain present\n");
        glDeleteProgram(s_fx_prog);
        s_fx_prog = 0;
        s_fx_on = 0;
        return 0;
    }
    s_fx_rcp = glGetUniformLocation(s_fx_prog, "u_rcp");
    s_fx_full = glGetUniformLocation(s_fx_prog, "u_full");
    s_fx_crop = glGetUniformLocation(s_fx_prog, "u_crop");
    s_fx_band = glGetUniformLocation(s_fx_prog, "u_band");
    s_fx_fill = glGetUniformLocation(s_fx_prog, "u_fill");
    s_fx_max = glGetUniformLocation(s_fx_prog, "u_max");
    s_fx_sharp = glGetUniformLocation(s_fx_prog, "u_sharp");
    {
        /* RECOMP_GL_SHARPEN: 0..1 (default 0.5, 0 = off), AMD CAS's
         * "sharpness": the negative lobe -1/mix(8, 5, k). */
        const char *e = getenv("RECOMP_GL_SHARPEN");
        float k = e ? (float)atof(e) : 0.5f;
        k = k < 0.0f ? 0.0f : k > 1.0f ? 1.0f : k;
        s_fx_sharpness = k > 0.0f ? -1.0f / (8.0f + (5.0f - 8.0f) * k) : 0.0f;
        fprintf(stderr, "  [GL] sharpening %g (RECOMP_GL_SHARPEN)\n", k);
    }
    glUseProgram(s_fx_prog);
    glUniform1i(glGetUniformLocation(s_fx_prog, "u_tex"), 0);
    s_cur_prog = s_fx_prog;
    glGenVertexArrays(1, &s_fx_vao);
    return 1;
}

/* The present pass's program, compiled before the title draws (prepare):
 * compiled at the first present it held back the first logo frame. */
static void fxaa_init_early(void)
{
    (void)fxaa_init();
    state_dirty();
}

/* Present s (sw stored columns drawn) into the window rectangle through
 * FXAA; 0 when it is off (the caller blits). Default framebuffer bound. */
static int present_fxaa(GlSurf *s, GLint sw, int dx, int dy, int dw, int dh)
{
    float rcp[2], mx[2], full[4], crop[4], band[2];
    uint32_t vw = s->vw ? s->vw : s->w;
    int fill = 0, held43 = 0;
    if (!fxaa_init())
        return 0;
    /* Held (nv2a_gl_fill_hold: MC3's boot movies): a 4:3 picture the title
     * centred in a widescreen frame is shown at its own 4:3 shape -- the
     * middle 640 columns in a 4:3 rectangle -- not stretched with the
     * frame to 16:9 (18% too wide for 720 columns). */
    if (s_fill_hold > 0 && xbox_video_widescreen() && s->aa_sx <= 1 && vw > 640
            && s->h >= 400) {
        int nw = dh * 4 / 3;
        if (nw < dw) {
            dx += (dw - nw) / 2;
            dw = nw;
        }
        held43 = 1;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(dx, dy, dw, dh);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glColorMask(1, 1, 1, 1);
    glUseProgram(s_fx_prog);
    s_cur_prog = s_fx_prog;
    rcp[0] = 1.0f / (float)s->pw;            rcp[1] = 1.0f / (float)s->ph;
    /* texture u of title column c is c / w (stored columns scale alike) */
    full[0] = 0.0f; full[1] = (float)vw / (float)s->w; full[2] = 0.0f; full[3] = 1.0f;
    if (held43) {
        full[0] = (float)(vw - 640) * 0.5f / (float)s->w;
        full[1] = ((float)vw - (float)(vw - 640) * 0.5f) / (float)s->w;
    }
    {
        /* RECOMP_GL_FILL (default on in 16:9): a widescreen frame wider
         * than 640 whose side bands are black -- MC3 lays its menus and
         * videos out in the middle 640 of 720 columns and clears the rest
         * -- shows the middle filled to the screen's width, trimming top
         * and bottom to keep the shape (27 of 480 lines each). */
        static int on = -1;
        if (on < 0) {
            const char *e = getenv("RECOMP_GL_FILL");
            on = !(e && *e == '0') && xbox_video_widescreen();
            fprintf(stderr, "  [GL] 4:3 screens filled to 16:9 %s (RECOMP_GL_FILL)\n", on ? "on" : "off");
        }
        if (on && s_fill_hold <= 0 && s->aa_sx <= 1 && vw > 640 && s->h >= 400) {
            float bandw = (float)(vw - 640) * 0.5f;                  /* title columns */
            float pa = (16.0f / 9.0f) / ((float)vw / (float)s->h);   /* pixel aspect */
            float keep = 640.0f * pa * 9.0f / 16.0f;                 /* lines kept */
            float cy = ((float)s->h - keep) * 0.5f;
            if (cy < 0.0f) cy = 0.0f;
            crop[0] = bandw / (float)s->w;
            crop[1] = ((float)vw - bandw) / (float)s->w;
            crop[2] = cy / (float)s->h;
            crop[3] = 1.0f - cy / (float)s->h;
            band[0] = bandw * 0.5f / (float)s->w;
            band[1] = ((float)vw - bandw * 0.5f) / (float)s->w;
            fill = 1;
        }
    }
    mx[0] = ((float)sw - 0.5f) / (float)s->pw;
    mx[1] = ((float)s->ph - 0.5f) / (float)s->ph;
    glUniform2fv(s_fx_rcp, 1, rcp);
    glUniform4fv(s_fx_full, 1, full);
    glUniform1i(s_fx_fill, fill);
    if (fill) {
        glUniform4fv(s_fx_crop, 1, crop);
        glUniform2fv(s_fx_band, 1, band);
    }
    glUniform2fv(s_fx_max, 1, mx);
    glUniform1f(s_fx_sharp, -s_fx_sharpness);   /* 1 / mix(8, 5, k); 0 = off */
    active_unit(0);
    bind_tex(s->tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    tex_param_forget(s->tex);                /* bind_stage sets its own again */
    glBindVertexArray(s_fx_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(s_vao);
    state_dirty();
    return 1;
}

/* The 2D engine's blit between two surfaces the renderer holds (the
 * executor has already copied guest memory, which is all a texture read
 * from memory needs). A source the renderer does not hold was written by
 * the CPU: nothing to do here. Rows are top-first, as everywhere. */
static void gl_blit(const Nv2aBlit *b)
{
    GlSurf *src, *dst;
    uint32_t sw, sh, dw, dh;

    if (!ready())
        return;
    src = surf_find(b->src_va);
    dst = surf_find(b->dst_va);
    if (!src || !dst)
        return;
    if (src == dst) {
        static int shown;
        if (shown++ < 4)
            fprintf(stderr, "  [GL] blit within surface %08X not done\n", b->src_va);
        return;
    }
    /* Clamp to both surfaces (title pixels). */
    sw = b->sx < src->w ? src->w - b->sx : 0;
    sh = b->sy < src->h ? src->h - b->sy : 0;
    dw = b->dx < dst->w ? dst->w - b->dx : 0;
    dh = b->dy < dst->h ? dst->h - b->dy : 0;
    if (sw > b->w) sw = b->w;
    if (sh > b->h) sh = b->h;
    if (dw > sw) dw = sw;
    if (dh > sh) dh = sh;
    if (!dw || !dh)
        return;
    state_dirty();
    glDisable(GL_SCISSOR_TEST);
    glColorMask(1, 1, 1, 1);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, src->fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dst->fbo);
    glBlitFramebuffer(to_stored(b->sx, src->pw, src->w), to_stored(b->sy, src->ph, src->h),
                      to_stored(b->sx + dw, src->pw, src->w), to_stored(b->sy + dh, src->ph, src->h),
                      to_stored(b->dx, dst->pw, dst->w), to_stored(b->dy, dst->ph, dst->h),
                      to_stored(b->dx + dw, dst->pw, dst->w), to_stored(b->dy + dh, dst->ph, dst->h),
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, dst->fbo);
    if (dst->vw_frame != s_frame || dst->vw < src->vw)   /* what is shown of it */
        dst->vw = src->vw;
    dst->vw_frame = s_frame;
    ft_note(dst->va, 'b');
}

static void gl_flip(void)
{
    state_dirty();
    static int dump_every = -1;
    static uint32_t dump_from, dump_to;     /* RECOMP_GL_DUMP=prefix,every,from,to */
    static char dump_prefix[256];
    static uint8_t *pix;
    static size_t pix_cap;
    int ww, wh;
    GlSurf *s = s_last;

    if (!ready())
        return;
    s_frame++;
    {
        /* A frame over 50 ms (a hitch): say what it did -- compiling programs
         * (Mesa, at first use), uploading textures (decode + glTexImage), or
         * neither (the game or the executor). At most 30 lines per 10 s. */
        static uint64_t last_ns, window_ns;
        static int lines;
        uint64_t now = hz_now();
        {
            /* RECOMP_GL_FRAMETIME=1: every presented frame's distance to the
             * last, with the time since the first -- uneven pacing at an even
             * average (the loading screen's circles, 2026-10-24) shows here
             * and in no per-second figure. */
            static uint64_t t0;
            if (s_ft < 0) {
                const char *e = getenv("RECOMP_GL_FRAMETIME");
                s_ft = e ? (atoi(e) > 1 ? 2 : 1) : 0;
            }
            if (s_ft) {
                if (!t0) t0 = now;
                ft_flush_run();
                fprintf(stderr, "[ft] %8.3f s frame %u %6.1f ms%s%s show %X\n",
                        (now - t0) / 1e9, s_frame,
                        last_ns ? (now - last_ns) / 1e6 : 0.0,
                        s_ft_len ? " |" : "", s_ft_log, s ? s->va : 0);
                s_ft_len = 0; s_ft_log[0] = 0;
            }
        }
        if (last_ns) {
            uint64_t dt = now - last_ns;
            if (now - window_ns > 10000000000ull) { window_ns = now; lines = 0; }
            /* At most 30 lines per 10 s -- a slow stretch of 50-60 ms frames
             * used them all up and a stall of several hundred ms in it went
             * unlogged (2026-10-26) -- except frames over 150 ms: always. */
            if (dt > 50000000ull && (lines++ < 30 || dt > 150000000ull))
            {
                char fm[160] = "";
                size_t k = 0;
                int c;
                for (c = 0; c < 64; c++)
                    if (s_hz_fmt[c] && k < sizeof fm - 12)
                        k += (size_t)snprintf(fm + k, sizeof fm - k, " %02X:%u", c, s_hz_fmt[c]);
                fprintf(stderr, "[hitch] frame %u: %.0f ms -- %u programs compiled (%.0f ms),"
                        " %u textures %.1f MB (%.0f ms: GL %.0f, mips %.0f), %u draws%s%s\n",
                        s_frame, dt / 1e6, s_hz_progs, s_hz_prog_ns / 1e6, s_hz_texs,
                        s_hz_tex_bytes / 1048576.0, s_hz_tex_ns / 1e6, s_hz_gl_ns / 1e6,
                        s_hz_mip_ns / 1e6, s_hz_draws, fm[0] ? "; formats" : "", fm);
            }
        }
        last_ns = now;
        s_hz_progs = s_hz_texs = s_hz_draws = 0;
        s_hz_prog_ns = s_hz_tex_ns = s_hz_tex_bytes = 0;
        s_hz_gl_ns = s_hz_mip_ns = 0;
        memset(s_hz_fmt, 0, sizeof s_hz_fmt);
    }
    if (s_pw_go && s_pw_file)
        prog_cache_prewarm_step(s_pw_per_frame);   /* the deferred list, a little a frame */
    ct_drain();                                    /* what the compile thread finished */
    hd_drain();                                    /* pack textures the worker finished */
    {
        /* RECOMP_FPS_LOG=1: presented frames per 10 s on stderr (the Switch
         * build has its own [perf] report). */
        static int on = -1;
        static uint32_t last_frame;
        static struct timespec last;
        struct timespec now;
        if (on < 0) {
            const char *e = getenv("RECOMP_FPS_LOG");
            on = e && *e == '1';
            clock_gettime(CLOCK_MONOTONIC, &last);
        }
        if (on) {
            double dt;
            clock_gettime(CLOCK_MONOTONIC, &now);
            dt = (double)(now.tv_sec - last.tv_sec) + (now.tv_nsec - last.tv_nsec) / 1e9;
            if (dt >= 10.0) {
                extern void xbox_vblank_report(double, char *, size_t);
                char vb[128];
                xbox_vblank_report(dt, vb, sizeof vb);
                fprintf(stderr, "[fps] %.1f fps (frame %u), %s\n",
                        (s_frame - last_frame) / dt, s_frame, vb);
                last_frame = s_frame;
                last = now;
            }
        }
    }
    if (dump_every < 0) {
        const char *d = getenv("RECOMP_GL_DUMP");
        dump_every = 0;
        if (d && *d) {
            const char *comma = strchr(d, ',');
            size_t n = comma ? (size_t)(comma - d) : strlen(d);
            if (n >= sizeof dump_prefix) n = sizeof dump_prefix - 1;
            memcpy(dump_prefix, d, n);
            dump_prefix[n] = 0;
            dump_every = comma ? atoi(comma + 1) : 60;
            if (dump_every <= 0) dump_every = 60;
            /* ,from[,to]: only frames in that range (a loading screen's
             * frames without slowing the boot by dumping it too). */
            if (comma && (comma = strchr(comma + 1, ',')) != NULL) {
                dump_from = (uint32_t)strtoul(comma + 1, NULL, 10);
                if ((comma = strchr(comma + 1, ',')) != NULL)
                    dump_to = (uint32_t)strtoul(comma + 1, NULL, 10);
            }
        }
    }
    if (!s || !s_drew_any) {
        /* Nothing to show yet -- no surface, or only cleared ones (a title
         * clears to black long before its first frame): the host's
         * placeholder (a loading screen), or
         * at least a clear -- a buffer never drawn to reaches the screen as
         * whatever the driver left in it. */
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(1, 1, 1, 1);
        if (!nv2a_gl_draw_placeholder()) {
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT);
        }
        glDisable(GL_SCISSOR_TEST);
        goto swap;
    }

    {
        /* RECOMP_GL_DUMP_KEY=<prefix>: every frame between two presses of R (F11 is macOS's Mission Control) -- a
         * stretch the player reaches by hand (a loading screen) without
         * dumping, and slowing, everything before it. */
        static int key_on = -1;
        if (key_on < 0) {
            const char *k = getenv("RECOMP_GL_DUMP_KEY");
            key_on = k && *k;
            if (key_on) {
                char dir[256], cwd[256], *slash;
                snprintf(dump_prefix, sizeof dump_prefix, "%s", k);
                snprintf(dir, sizeof dir, "%s", k);
                if ((slash = strrchr(dir, '/')) != NULL) {   /* the folder, made if missing */
                    *slash = 0;
                    mkdir(dir, 0755);
                }
                if (k[0] != '/' && getcwd(cwd, sizeof cwd))
                    fprintf(stderr, "  [GL] (relative to %s)\n", cwd);
                dump_every = 1;
                dump_from = 0xFFFFFFFFu;             /* nothing until the key */
                fprintf(stderr, "  [GL] press R to start and stop writing every frame as %s<frame>.bmp\n", k);
            }
        }
#if (!defined(_WIN32) || defined(XBOXRECOMP_SDL_HOST)) && !defined(__SWITCH__)
        if (key_on > 0) {
            /* R toggles: a press starts, the next stops -- a short tap is
             * enough, and each change is logged with its frame, so a key
             * that never arrives shows as a missing line. */
            static int was_down, recording;
            int down = xbox_sdl_keyboard_down(SDL_SCANCODE_R);
            if (down && !was_down) {
                recording = !recording;
                fprintf(stderr, "  [GL] recording %s at frame %u\n", recording ? "on" : "off", s_frame);
            }
            was_down = down;
            dump_from = recording ? 0 : 0xFFFFFFFFu;
        }
#endif
    }
    if (dump_every && (s_frame % (uint32_t)dump_every) == 0
            && s_frame >= dump_from && (!dump_to || s_frame <= dump_to)) {
        size_t need = (size_t)s->pw * s->ph * 4;
        if (need > pix_cap) {
            free(pix);
            pix_cap = need;
            pix = (uint8_t *)malloc(pix_cap);
        }
        if (pix) {
            char path[320];
            glBindFramebuffer(GL_READ_FRAMEBUFFER, s->fbo);
            glPixelStorei(GL_PACK_ALIGNMENT, 4);
            glReadPixels(0, 0, (GLsizei)s->pw, (GLsizei)s->ph, GL_BGRA,
                         GL_UNSIGNED_BYTE, pix);
            snprintf(path, sizeof path, "%s%05u.bmp", dump_prefix, s_frame);
            dump_bmp(path, pix, s->pw, s->ph);
        }
    }

    /* Present: the logical aspect (surface / AA factor; 16:9 for a
     * widescreen title), letterboxed, with
     * the top-first rows turned the right way up. */
    /* The host may know the real on-screen size better than SDL (the
     * Switch's EGL surface follows the display, SDL keeps the window's
     * creation size). */
    if (!nv2a_gl_screen_size(&ww, &wh) || ww <= 0 || wh <= 0)
        SDL_GL_GetDrawableSize(s_win, &ww, &wh);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s->fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(1, 1, 1, 1);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    {
        /* nv2a_present.h: the drawn width, 16:9 for a widescreen title. */
        uint32_t vw = s->vw ? s->vw : s->w;
        GLint sw = (GLint)((uint64_t)s->pw * vw / s->w);
        int rc[4], dx, dy, dw, dh;
        nv2a_present_rect((float)vw / (float)s->aa_sx, (float)s->h / (float)s->aa_sy,
                          ww, wh, rc);
        dx = rc[0]; dy = rc[1]; dw = rc[2]; dh = rc[3];
        /* Scissored to the picture: Switch Mesa (nouveau) runs a scaled,
         * flipped blit over more than the destination rectangle, and the
         * pillarbox bars filled with the source's edge columns stretched
         * sideways. Blits obey the scissor test, so the bars stay cleared. */
        glEnable(GL_SCISSOR_TEST);
        glScissor(dx, dy, dw, dh);
        if (!present_fxaa(s, sw, dx, dy, dw, dh))
            glBlitFramebuffer(0, 0, sw, (GLint)s->ph,
                              dx, dy + dh, dx + dw, dy, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        glDisable(GL_SCISSOR_TEST);
    }
swap:
    if (gl_probe()) {
        /* A blue square straight into the window: does anything we draw
         * reach the screen? */
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glColorMask(1, 1, 1, 1);
        glEnable(GL_SCISSOR_TEST);
        glScissor(16, 16, 96, 96);
        glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glDisable(GL_SCISSOR_TEST);
        state_dirty();
    }
    gl_step_done("present");
    pf_start();
    SDL_GL_SwapWindow(s_win);
    pf_mark(PF_SWAP);
    if (s_pf_on > 0)
        s_pf_frames++;
    gl_step_done("swap");
    tex_bind_forget();
    {
        /* Where the main thread pumps the window system's events itself
         * (macOS, platform/host_main.c), only take what it queued: pumping
         * from here is not allowed there. */
        SDL_Event e;
        int pump = xbox_host_on_main();
        while (pump ? SDL_PollEvent(&e)
                    : SDL_PeepEvents(&e, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT) > 0) {
            if (e.type == SDL_QUIT) {
                /* The window's close button, Cmd+Q, the Dock's Quit: the
                 * title has no way out of its own, so leave here (exit
                 * handlers flush the log and any audio dump). */
                fprintf(stderr, "  [GL] window closed: quitting\n");
                fflush(stderr);
                exit(0);
            }
#if (!defined(_WIN32) || defined(XBOXRECOMP_SDL_HOST)) && !defined(__SWITCH__)
            xbox_sdl_keyboard_handle(&e);       /* RECOMP_KEYBOARD (input/) */
#endif
        }
    }
}

/* ── Threaded submission (RECOMP_GL_THREAD=1) ─────────────────────────
 *
 * On the Switch the executor thread spent ~35 us per draw in Mesa and ~18 us
 * decoding the pushbuffer, one after the other, and that thread was the frame
 * rate limit. With this on, the executor only decodes and queues; a GL
 * thread of its own replays the queue, so the two halves run on two cores.
 *
 * A queued draw must not point at anything the executor or the title may
 * change once it moves on, so the executor thread copies: the method shadow,
 * transform program and constants go as 64/16-byte blocks that differ from
 * what was last queued (the GL thread keeps its own copies), vertex data as
 * the byte ranges the draw reads (vertex_runs), indices, and converted
 * attributes. Texture images are still read from guest memory when the GL
 * thread gets to them (at most two frames later); texture and palette
 * addresses come pre-resolved in the batch, so nothing here reads the
 * executor's DMA state. At most two flips are queued: the executor waits
 * for the GL thread beyond that.
 *
 * Records are 16-byte aligned in one ring. Every record first carries the
 * shadow's changes, because clears and presents read registers too (the
 * clip, the depth range) and saw the executor's live shadow before. */
#include <pthread.h>
#include <time.h>

enum { Q_WRAP = 0, Q_DRAW = 1, Q_CLEAR = 2, Q_FLIP = 3, Q_BLIT = 4 };
typedef struct { uint32_t type, size, nreg, nprog, nconst, pad[3]; } QHdr;
typedef struct { uint32_t idx, pad[3]; uint32_t w[16]; } QRegChunk;   /* 64 B of shadow */
typedef struct { uint32_t idx, pad[3]; uint32_t w[4]; } QRow;         /* 16 B row */
typedef struct {
    Nv2aSurface s;
    Nv2aRenderState rs;
    uint32_t flags, argb, zs, pad;
} QClear;

#define Q_BYTES   (48u << 20)
#define Q_ALIGN(x) (((x) + 15u) & ~(size_t)15u)
#define REG_WORDS (0x2000 / 4)
#define PROG_ROWS 136
#define CONST_ROWS 192

static int s_q_on;
static uint8_t *s_q;
static size_t s_q_head, s_q_tail;               /* bytes written / read, ever */
static int s_q_consumer_waits, s_q_producer_waits, s_q_flips;
static pthread_mutex_t s_q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_q_cv = PTHREAD_COND_INITIALIZER;
static uint64_t s_q_dropped;
/* For the perf report: time the GL thread sat with an empty queue, and the
 * executor was held back (queue full, or two flips queued). ns. */
static uint64_t s_q_gl_idle_ns, s_q_exec_wait_ns;
static uint64_t q_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Executor thread: what was last queued, and where the live shadow is. */
static uint32_t s_q_regs[REG_WORDS], s_q_prog[PROG_ROWS][4];
static float s_q_consts[CONST_ROWS][4];
static const uint32_t *s_q_live_regs;
static uint32_t s_q_prog_gen, s_q_const_gen;
/* GL thread: its own copies, rebuilt from the records. */
static uint32_t g_regs[REG_WORDS], g_prog[PROG_ROWS][4];
static float g_consts[CONST_ROWS][4];

static void q_timed_wait(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 2000000;
    if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
    pthread_cond_timedwait(&s_q_cv, &s_q_lock, &ts);
}

/* Room for `size` contiguous bytes; returns where to write them. */
static uint8_t *q_reserve(size_t size)
{
    size_t off = s_q_head % Q_BYTES, need = size;
    if (off + size > Q_BYTES)
        need += Q_BYTES - off;                  /* the wrap filler */
    if (Q_BYTES - (s_q_head - __atomic_load_n(&s_q_tail, __ATOMIC_ACQUIRE)) < need) {
        uint64_t t0 = q_now_ns();
        pthread_mutex_lock(&s_q_lock);
        s_q_producer_waits = 1;
        while (Q_BYTES - (s_q_head - __atomic_load_n(&s_q_tail, __ATOMIC_ACQUIRE)) < need)
            q_timed_wait();
        s_q_producer_waits = 0;
        pthread_mutex_unlock(&s_q_lock);
        __atomic_add_fetch(&s_q_exec_wait_ns, q_now_ns() - t0, __ATOMIC_RELAXED);
    }
    if (off + size > Q_BYTES) {
        QHdr *w = (QHdr *)(s_q + off);
        w->type = Q_WRAP;                        /* ring has slack past the end */
        w->size = (uint32_t)(Q_BYTES - off);
        __atomic_store_n(&s_q_head, s_q_head + (Q_BYTES - off), __ATOMIC_RELEASE);
        off = 0;
    }
    return s_q + off;
}

static void q_publish(size_t size)
{
    __atomic_store_n(&s_q_head, s_q_head + size, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&s_q_consumer_waits, __ATOMIC_SEQ_CST)) {
        pthread_mutex_lock(&s_q_lock);
        pthread_cond_broadcast(&s_q_cv);
        pthread_mutex_unlock(&s_q_lock);
    }
}

/* The shadow blocks that changed since the last record. The executor marks
 * the 64-byte blocks it writes (nv2a_pb_reg_dirty; comparing all 8 KB on
 * every draw was 14% of its thread on the console); those are compared and
 * the ones that really differ kept in s_q_pend until q_reg_emit. Both run on
 * the executor thread. */
extern uint8_t *nv2a_pb_reg_dirty(void);
static uint16_t s_q_pend[REG_WORDS / 16];
static uint32_t s_q_npend;

static uint32_t q_reg_collect(void)
{
    uint8_t *dirty = nv2a_pb_reg_dirty();
    uint32_t c;
    if (!s_q_live_regs)
        return 0;
    for (c = 0; c < REG_WORDS / 16; c++) {
        if (!dirty[c])
            continue;
        dirty[c] = 0;
        if (memcmp(s_q_live_regs + c * 16, s_q_regs + c * 16, 64) != 0) {
            uint32_t k;
            for (k = 0; k < s_q_npend && s_q_pend[k] != c; k++) { }
            if (k == s_q_npend)
                s_q_pend[s_q_npend++] = (uint16_t)c;
        }
    }
    return s_q_npend;
}

static uint32_t q_reg_emit(QRegChunk *out)
{
    uint32_t k, n = s_q_npend;
    for (k = 0; k < n; k++) {
        uint32_t c = s_q_pend[k];
        out[k].idx = c;
        memcpy(out[k].w, s_q_live_regs + c * 16, 64);
        memcpy(s_q_regs + c * 16, s_q_live_regs + c * 16, 64);
    }
    s_q_npend = 0;
    return n;
}

static uint32_t q_rows_diff(const uint32_t (*live)[4], uint32_t (*sent)[4], uint32_t rows,
                            QRow *out)
{
    uint32_t r, n = 0;
    for (r = 0; r < rows; r++)
        if (memcmp(live[r], sent[r], 16) != 0) {
            if (out) {
                out[n].idx = r;
                memcpy(out[n].w, live[r], 16);
                memcpy(sent[r], live[r], 16);
            }
            n++;
        }
    return n;
}

/* Header plus the shadow changes; returns the offset after them. */
static size_t q_write_state(uint8_t *rec, uint32_t type, uint32_t nreg,
                            uint32_t nprog, uint32_t nconst, const Nv2aRawBatch *b)
{
    QHdr *h = (QHdr *)rec;
    size_t at = sizeof(QHdr);
    memset(h, 0, sizeof *h);
    h->type = type;
    h->nreg = q_reg_emit((QRegChunk *)(rec + at));
    (void)nreg;
    at += (size_t)h->nreg * sizeof(QRegChunk);
    if (b && nprog) {
        h->nprog = q_rows_diff(b->vp_program, s_q_prog, PROG_ROWS, (QRow *)(rec + at));
        at += (size_t)h->nprog * sizeof(QRow);
    }
    if (b && nconst) {
        h->nconst = q_rows_diff((const uint32_t (*)[4])b->vp_consts,
                                (uint32_t (*)[4])s_q_consts, CONST_ROWS, (QRow *)(rec + at));
        at += (size_t)h->nconst * sizeof(QRow);
    }
    return at;
}

static void q_draw(const Nv2aRawBatch *b)
{
    VRun run[NV2A_RAW_ATTRS];
    int of_run[NV2A_RAW_ATTRS];
    uint32_t nrun = vertex_runs(b, run, of_run), i, a, nreg, nprog = 0, nconst = 0;
    uint16_t conv = b->attr_present & (uint16_t)~b->attr_direct;
    size_t size, at, runs_at[NV2A_RAW_ATTRS], attrs_at = 0, idx_at, batch_at;
    Nv2aRawBatch *qb;
    uint8_t *rec;

    s_q_live_regs = b->regs;
    nreg = q_reg_collect();
    if (b->vp_program && b->vp_prog_gen != s_q_prog_gen && b->vp_slots <= PROG_ROWS)
        nprog = q_rows_diff(b->vp_program, s_q_prog, PROG_ROWS, NULL);
    if (b->vp_consts && b->vp_const_gen != s_q_const_gen && b->vp_const_count <= CONST_ROWS)
        nconst = q_rows_diff((const uint32_t (*)[4])b->vp_consts,
                             (uint32_t (*)[4])s_q_consts, CONST_ROWS, NULL);
    size = sizeof(QHdr) + (size_t)nreg * sizeof(QRegChunk)
         + (size_t)(nprog + nconst) * sizeof(QRow);
    batch_at = size;
    size += Q_ALIGN(sizeof(Nv2aRawBatch));
    for (i = 0; i < nrun; i++) {
        runs_at[i] = size;
        size += Q_ALIGN((size_t)(run[i].hi - run[i].lo));
    }
    if (conv) {
        attrs_at = size;
        size += Q_ALIGN((size_t)b->vertex_count * NV2A_RAW_ATTRS * 16);
    }
    idx_at = size;
    size += Q_ALIGN((size_t)b->index_count * 4);
    if (size > Q_BYTES / 4) {
        if ((s_q_dropped++ & 1023) == 0)
            fprintf(stderr, "  [GL] queued draw of %zu bytes dropped (%llu so far)\n",
                    size, (unsigned long long)s_q_dropped);
        return;
    }

    rec = q_reserve(size);
    at = q_write_state(rec, Q_DRAW, nreg, nprog, nconst, b);
    s_q_prog_gen = b->vp_prog_gen;
    s_q_const_gen = b->vp_const_gen;
    if (at != batch_at) {                       /* cannot happen: same diffs */
        fprintf(stderr, "  [GL] queue record mismatch %zu/%zu\n", at, batch_at);
        abort();
    }
    qb = (Nv2aRawBatch *)(rec + batch_at);
    *qb = *b;
    /* Pointers become offsets into the record; the GL thread fixes them. */
    for (i = 0; i < nrun; i++)
        memcpy(rec + runs_at[i], run[i].lo, (size_t)(run[i].hi - run[i].lo));
    for (a = 0; a < NV2A_RAW_ATTRS; a++)
        if (b->attr_direct & (1u << a))
            qb->direct[a].ptr = (const uint8_t *)(uintptr_t)
                (runs_at[of_run[a]] + (size_t)(b->direct[a].ptr - run[of_run[a]].lo));
    if (conv)
        memcpy(rec + attrs_at, b->attrs, (size_t)b->vertex_count * NV2A_RAW_ATTRS * 16);
    qb->attrs = (const float *)(uintptr_t)attrs_at;
    memcpy(rec + idx_at, b->indices, (size_t)b->index_count * 4);
    qb->indices = (const uint32_t *)(uintptr_t)idx_at;
    ((QHdr *)rec)->size = (uint32_t)size;
    q_publish(size);
}

static void q_clear(const Nv2aSurface *sf, const Nv2aRenderState *rs,
                    uint32_t flags, uint32_t argb, uint32_t zs)
{
    uint32_t nreg = q_reg_collect();
    size_t size = sizeof(QHdr) + (size_t)nreg * sizeof(QRegChunk) + Q_ALIGN(sizeof(QClear));
    uint8_t *rec = q_reserve(size);
    size_t at = q_write_state(rec, Q_CLEAR, nreg, 0, 0, NULL);
    QClear *c = (QClear *)(rec + at);
    c->s = *sf;
    c->rs = *rs;
    c->flags = flags;
    c->argb = argb;
    c->zs = zs;
    ((QHdr *)rec)->size = (uint32_t)size;
    q_publish(size);
}

static void q_blit(const Nv2aBlit *b)
{
    uint32_t nreg = q_reg_collect();
    size_t size = sizeof(QHdr) + (size_t)nreg * sizeof(QRegChunk) + Q_ALIGN(sizeof(Nv2aBlit));
    uint8_t *rec = q_reserve(size);
    size_t at = q_write_state(rec, Q_BLIT, nreg, 0, 0, NULL);
    memcpy(rec + at, b, sizeof *b);
    ((QHdr *)rec)->size = (uint32_t)size;
    q_publish(size);
}

static void q_flip(void)
{
    uint32_t nreg;
    size_t size;
    uint8_t *rec;

    /* No more than two frames behind the executor. */
    if (__atomic_load_n(&s_q_flips, __ATOMIC_ACQUIRE) >= 2) {
        uint64_t t0 = q_now_ns();
        pthread_mutex_lock(&s_q_lock);
        s_q_producer_waits = 1;
        while (__atomic_load_n(&s_q_flips, __ATOMIC_ACQUIRE) >= 2)
            q_timed_wait();
        s_q_producer_waits = 0;
        pthread_mutex_unlock(&s_q_lock);
        __atomic_add_fetch(&s_q_exec_wait_ns, q_now_ns() - t0, __ATOMIC_RELAXED);
    }
    nreg = q_reg_collect();
    size = sizeof(QHdr) + (size_t)nreg * sizeof(QRegChunk);
    rec = q_reserve(size);
    q_write_state(rec, Q_FLIP, nreg, 0, 0, NULL);
    ((QHdr *)rec)->size = (uint32_t)size;
    __atomic_add_fetch(&s_q_flips, 1, __ATOMIC_ACQ_REL);
    q_publish(size);
}

static void *q_gl_thread(void *arg)
{
    extern void xbox_nx_spread_thread(void);
    extern void xbox_nx_track_thread(void *entry);
    (void)arg;
    xbox_nx_spread_thread();
    xbox_nx_track_thread((void *)q_gl_thread);
    for (;;) {
        size_t avail = __atomic_load_n(&s_q_head, __ATOMIC_SEQ_CST) - s_q_tail;
        uint8_t *rec;
        const QHdr *h;
        size_t at;
        uint32_t i;

        if (!avail) {
            uint64_t t0 = q_now_ns();
            pthread_mutex_lock(&s_q_lock);
            __atomic_store_n(&s_q_consumer_waits, 1, __ATOMIC_SEQ_CST);
            while (__atomic_load_n(&s_q_head, __ATOMIC_SEQ_CST) == s_q_tail)
                q_timed_wait();
            __atomic_store_n(&s_q_consumer_waits, 0, __ATOMIC_SEQ_CST);
            pthread_mutex_unlock(&s_q_lock);
            __atomic_add_fetch(&s_q_gl_idle_ns, q_now_ns() - t0, __ATOMIC_RELAXED);
            continue;
        }
        rec = s_q + s_q_tail % Q_BYTES;
        h = (const QHdr *)rec;
        if (h->type != Q_WRAP) {
            at = sizeof(QHdr);
            for (i = 0; i < h->nreg; i++, at += sizeof(QRegChunk)) {
                const QRegChunk *c = (const QRegChunk *)(rec + at);
                memcpy(g_regs + c->idx * 16, c->w, 64);
            }
            for (i = 0; i < h->nprog; i++, at += sizeof(QRow)) {
                const QRow *r = (const QRow *)(rec + at);
                memcpy(g_prog[r->idx], r->w, 16);
            }
            for (i = 0; i < h->nconst; i++, at += sizeof(QRow)) {
                const QRow *r = (const QRow *)(rec + at);
                memcpy(g_consts[r->idx], r->w, 16);
            }
            s_regs = g_regs;                    /* what clears/presents read */
            if (h->type == Q_DRAW) {
                Nv2aRawBatch bb = *(const Nv2aRawBatch *)(rec + at);
                uint32_t a;
                bb.regs = g_regs;
                bb.vp_program = (const uint32_t (*)[4])g_prog;
                bb.vp_consts = (const float (*)[4])g_consts;
                for (a = 0; a < NV2A_RAW_ATTRS; a++)
                    if (bb.attr_direct & (1u << a))
                        bb.direct[a].ptr = rec + (uintptr_t)bb.direct[a].ptr;
                bb.attrs = (const float *)(rec + (uintptr_t)bb.attrs);
                bb.indices = (const uint32_t *)(rec + (uintptr_t)bb.indices);
                gl_draw_raw(&bb);
            } else if (h->type == Q_CLEAR) {
                const QClear *c = (const QClear *)(rec + at);
                gl_clear(&c->s, &c->rs, c->flags, c->argb, c->zs);
            } else if (h->type == Q_BLIT) {
                gl_blit((const Nv2aBlit *)(rec + at));
            } else if (h->type == Q_FLIP) {
                gl_flip();
                __atomic_sub_fetch(&s_q_flips, 1, __ATOMIC_ACQ_REL);
            }
        }
        __atomic_store_n(&s_q_tail, s_q_tail + h->size, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&s_q_producer_waits, __ATOMIC_SEQ_CST)) {
            pthread_mutex_lock(&s_q_lock);
            pthread_cond_broadcast(&s_q_cv);
            pthread_mutex_unlock(&s_q_lock);
        }
    }
    return NULL;
}

/* Share of the last `secs` the GL thread sat idle and the executor was held
 * back by the queue; -1 when the GL thread is off. */
/* One line for the perf report: per frame shown, the draws and how many
 * ms each phase of them took on the GL thread (the rest of its busy time is
 * clears, blits, the present pass, queue handling). Resets the counts. */
void nv2a_gl_prof_stats(double secs, char *out, size_t n)
{
    uint32_t fr = s_pf_frames ? s_pf_frames : 1;
    size_t k;
    int i;
    (void)secs;
    if (s_pf_on <= 0) {
        if (n) out[0] = 0;
        return;
    }
    k = (size_t)snprintf(out, n, "%u frames, per frame: %u draws (%u verts, %u indices), "
                         "%u with new constants (%u rows), %u new uniforms, %u same as the one before; ms:",
                         s_pf_frames, s_pf_draws / fr, s_pf_verts / fr, s_pf_idx / fr,
                         s_pf_cdraws / fr, s_pf_crows / fr, s_pf_udraws / fr, s_pf_same / fr);
    for (i = 0; i < PF_N && k < n; i++)
        k += (size_t)snprintf(out + k, n - k, " %s %.1f", s_pf_name[i], s_pf_ns[i] / 1e6 / fr);
    memset(s_pf_ns, 0, sizeof s_pf_ns);
    s_pf_draws = s_pf_frames = s_pf_verts = s_pf_idx = 0;
    s_pf_cdraws = s_pf_crows = s_pf_udraws = s_pf_same = 0;
}

void nv2a_gl_queue_stats(double secs, int *gl_idle_pct, int *exec_wait_pct)
{
    if (!s_q_on) {
        *gl_idle_pct = *exec_wait_pct = -1;
        return;
    }
    *gl_idle_pct = (int)(__atomic_exchange_n(&s_q_gl_idle_ns, 0, __ATOMIC_RELAXED) / (secs * 1e7));
    *exec_wait_pct = (int)(__atomic_exchange_n(&s_q_exec_wait_ns, 0, __ATOMIC_RELAXED) / (secs * 1e7));
}

static int q_start(void)
{
    pthread_t t;
    pthread_attr_t at;
    s_q = (uint8_t *)malloc(Q_BYTES + 256);
    if (!s_q)
        return 0;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 1u << 20);
    if (pthread_create(&t, &at, q_gl_thread, NULL) != 0) {
        free(s_q);
        s_q = NULL;
        return 0;
    }
    pthread_detach(t);
    return 1;
}

static Nv2aBackend s_backend_q = {
    q_clear,
    NULL,
    q_flip,
    q_draw,
    NV2A_BACKEND_RAW_DIRECT,
    NULL,
    q_blit,
};

/* Window, context and the shader cache before the title draws (prepare). */
static void fxaa_init_early(void);
static void gl_prepare(void)
{
    if (ready())
        fxaa_init_early();
}

static const Nv2aBackend s_backend_gl = {
    gl_clear,
    NULL,              /* draw: everything arrives through draw_raw */
    gl_flip,
    gl_draw_raw,
    NV2A_BACKEND_RAW_DIRECT,
    gl_prepare,
    gl_blit,
};

void nv2a_gl_install(void)
{
    {
        /* Default on for the Switch: MC3's race went from 7 to 10-12 fps
         * with it, the title's GPU wait from 370 to ~25 ms/s (2026-10-20). */
        const char *e = getenv("RECOMP_GL_THREAD");
#if defined(__SWITCH__)
        int want = !(e && *e == '0');
#else
        int want = e && *e == '1';
#endif
        s_q_on = want && q_start();
    }
    if (s_q_on) {
        nv2a_backend_register(&s_backend_q);
        fprintf(stderr, "  [GL] NV2A OpenGL renderer registered, on a thread of its own"
                        " (RECOMP_GL_THREAD=1)\n");
        return;
    }
    nv2a_backend_register(&s_backend_gl);
    fprintf(stderr, "  [GL] NV2A OpenGL renderer registered\n");
}
