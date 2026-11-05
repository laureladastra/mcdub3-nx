/* gl_api.c -- fill the entry-point table declared in gl_api.h. */
#include "gl_api.h"

#include <stdio.h>
#include <string.h>

#define NV2A_GL_DEFINE(ret, name, args) PFN_##name p_##name;
NV2A_GL_FUNCS(NV2A_GL_DEFINE)
NV2A_GL_OPT_FUNCS(NV2A_GL_DEFINE)
#undef NV2A_GL_DEFINE

/* OpenGL ES (nv2a_gl_es, Android) has float versions of these two. */
static void (GLAPIENTRY *s_clear_depthf)(float);
static void (GLAPIENTRY *s_depth_rangef)(float, float);
static void GLAPIENTRY es_clear_depth(GLdouble d) { s_clear_depthf((float)d); }
static void GLAPIENTRY es_depth_range(GLdouble n, GLdouble f) { s_depth_rangef((float)n, (float)f); }

int nv2a_gl_es;

int nv2a_gl_load(void *(*getproc)(const char *name))
{
    int missing = 0;

    if (nv2a_gl_es) {
        /* Before the checks below: the double versions are not in ES. */
        s_clear_depthf = (void (GLAPIENTRY *)(float))getproc("glClearDepthf");
        s_depth_rangef = (void (GLAPIENTRY *)(float, float))getproc("glDepthRangef");
    }

#define NV2A_GL_RESOLVE(ret, name, args) \
    p_##name = (PFN_##name)getproc(#name); \
    if (!p_##name && nv2a_gl_es && !strcmp(#name, "glClearDepth") && s_clear_depthf) \
        p_##name = (PFN_##name)(void (*)(void))es_clear_depth; \
    if (!p_##name && nv2a_gl_es && !strcmp(#name, "glDepthRange") && s_depth_rangef) \
        p_##name = (PFN_##name)(void (*)(void))es_depth_range; \
    if (!p_##name) { \
        fprintf(stderr, "  [GL] missing entry point %s\n", #name); \
        missing++; \
    }
    NV2A_GL_FUNCS(NV2A_GL_RESOLVE)
#undef NV2A_GL_RESOLVE
#define NV2A_GL_RESOLVE_OPT(ret, name, args) p_##name = (PFN_##name)getproc(#name); \
    if (!p_##name && nv2a_gl_es) p_##name = (PFN_##name)getproc(#name "EXT");
    NV2A_GL_OPT_FUNCS(NV2A_GL_RESOLVE_OPT)
#undef NV2A_GL_RESOLVE_OPT
    return missing;
}
