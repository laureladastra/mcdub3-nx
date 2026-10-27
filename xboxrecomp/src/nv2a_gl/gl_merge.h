/*
 * gl_merge.h -- nv2a_gl's GL calls, made safe for merging draws.
 *
 * nv2a_gl holds a draw back (merge_flush in nv2a_gl.c) when the next one may
 * be folded into it; the held draw must reach GL before anything it depends
 * on changes. Rather than a flush at each of the dozens of places that
 * change GL state, every state-changing call goes through a wrapper that
 * flushes first. Binding what is already bound (the framebuffer, the vertex
 * array, a buffer) is skipped: it changes nothing, and done for every draw
 * it would end every merge. Included by nv2a_gl.c only, after gl_api.h.
 * Generated from gl_api.h's list; a new GL call there that changes state
 * belongs in the list below.
 */
#ifndef NV2A_GL_MERGE_H
#define NV2A_GL_MERGE_H

static int s_mg_active;                 /* a draw is held (nv2a_gl.c) */
static void merge_flush(void);

/* The viewport, cached: surf_bind sets it for every draw. */
static GLint s_mg_vp[4] = { -1, -1, -1, -1 };
static inline void mg_glViewport(GLint x, GLint y, GLsizei w, GLsizei h)
{
    if (s_mg_vp[0] == x && s_mg_vp[1] == y && s_mg_vp[2] == w && s_mg_vp[3] == h)
        return;
    if (s_mg_active) merge_flush();
    s_mg_vp[0] = x; s_mg_vp[1] = y; s_mg_vp[2] = w; s_mg_vp[3] = h;
    p_glViewport(x, y, w, h);
}
#undef glViewport
#define glViewport mg_glViewport

static inline void mg_glScissor(GLint a0, GLint a1, GLsizei a2, GLsizei a3)
{ if (s_mg_active) merge_flush(); p_glScissor(a0, a1, a2, a3); }
#undef glScissor
#define glScissor mg_glScissor

static inline void mg_glClear(GLbitfield a0)
{ if (s_mg_active) merge_flush(); p_glClear(a0); }
#undef glClear
#define glClear mg_glClear

static inline void mg_glEnable(GLenum a0)
{ if (s_mg_active) merge_flush(); p_glEnable(a0); }
#undef glEnable
#define glEnable mg_glEnable

static inline void mg_glDisable(GLenum a0)
{ if (s_mg_active) merge_flush(); p_glDisable(a0); }
#undef glDisable
#define glDisable mg_glDisable

static inline void mg_glBlendFunc(GLenum a0, GLenum a1)
{ if (s_mg_active) merge_flush(); p_glBlendFunc(a0, a1); }
#undef glBlendFunc
#define glBlendFunc mg_glBlendFunc

static inline void mg_glBlendEquation(GLenum a0)
{ if (s_mg_active) merge_flush(); p_glBlendEquation(a0); }
#undef glBlendEquation
#define glBlendEquation mg_glBlendEquation

static inline void mg_glBlendColor(GLfloat a0, GLfloat a1, GLfloat a2, GLfloat a3)
{ if (s_mg_active) merge_flush(); p_glBlendColor(a0, a1, a2, a3); }
#undef glBlendColor
#define glBlendColor mg_glBlendColor

static inline void mg_glDepthFunc(GLenum a0)
{ if (s_mg_active) merge_flush(); p_glDepthFunc(a0); }
#undef glDepthFunc
#define glDepthFunc mg_glDepthFunc

static inline void mg_glDepthMask(GLboolean a0)
{ if (s_mg_active) merge_flush(); p_glDepthMask(a0); }
#undef glDepthMask
#define glDepthMask mg_glDepthMask

static inline void mg_glDepthRange(GLdouble a0, GLdouble a1)
{ if (s_mg_active) merge_flush(); p_glDepthRange(a0, a1); }
#undef glDepthRange
#define glDepthRange mg_glDepthRange

static inline void mg_glColorMask(GLboolean a0, GLboolean a1, GLboolean a2, GLboolean a3)
{ if (s_mg_active) merge_flush(); p_glColorMask(a0, a1, a2, a3); }
#undef glColorMask
#define glColorMask mg_glColorMask

static inline void mg_glStencilFunc(GLenum a0, GLint a1, GLuint a2)
{ if (s_mg_active) merge_flush(); p_glStencilFunc(a0, a1, a2); }
#undef glStencilFunc
#define glStencilFunc mg_glStencilFunc

static inline void mg_glStencilOp(GLenum a0, GLenum a1, GLenum a2)
{ if (s_mg_active) merge_flush(); p_glStencilOp(a0, a1, a2); }
#undef glStencilOp
#define glStencilOp mg_glStencilOp

static inline void mg_glStencilMask(GLuint a0)
{ if (s_mg_active) merge_flush(); p_glStencilMask(a0); }
#undef glStencilMask
#define glStencilMask mg_glStencilMask

static inline void mg_glCullFace(GLenum a0)
{ if (s_mg_active) merge_flush(); p_glCullFace(a0); }
#undef glCullFace
#define glCullFace mg_glCullFace

static inline void mg_glFrontFace(GLenum a0)
{ if (s_mg_active) merge_flush(); p_glFrontFace(a0); }
#undef glFrontFace
#define glFrontFace mg_glFrontFace

static inline void mg_glReadPixels(GLint a0, GLint a1, GLsizei a2, GLsizei a3, GLenum a4, GLenum a5, void * a6)
{ if (s_mg_active) merge_flush(); p_glReadPixels(a0, a1, a2, a3, a4, a5, a6); }
#undef glReadPixels
#define glReadPixels mg_glReadPixels

static inline void mg_glFinish(void)
{ if (s_mg_active) merge_flush(); p_glFinish(); }
#undef glFinish
#define glFinish mg_glFinish

static inline void mg_glDeleteTextures(GLsizei a0, const GLuint * a1)
{ if (s_mg_active) merge_flush(); p_glDeleteTextures(a0, a1); }
#undef glDeleteTextures
#define glDeleteTextures mg_glDeleteTextures

static inline void mg_glBindTexture(GLenum a0, GLuint a1)
{ if (s_mg_active) merge_flush(); p_glBindTexture(a0, a1); }
#undef glBindTexture
#define glBindTexture mg_glBindTexture

static inline void mg_glTexImage2D(GLenum a0, GLint a1, GLint a2, GLsizei a3, GLsizei a4, GLint a5, GLenum a6, GLenum a7, const void * a8)
{ if (s_mg_active) merge_flush(); p_glTexImage2D(a0, a1, a2, a3, a4, a5, a6, a7, a8); }
#undef glTexImage2D
#define glTexImage2D mg_glTexImage2D

static inline void mg_glTexSubImage2D(GLenum a0, GLint a1, GLint a2, GLint a3, GLsizei a4, GLsizei a5, GLenum a6, GLenum a7, const void * a8)
{ if (s_mg_active) merge_flush(); p_glTexSubImage2D(a0, a1, a2, a3, a4, a5, a6, a7, a8); }
#undef glTexSubImage2D
#define glTexSubImage2D mg_glTexSubImage2D

static inline void mg_glCompressedTexImage2D(GLenum a0, GLint a1, GLenum a2, GLsizei a3, GLsizei a4, GLint a5, GLsizei a6, const void * a7)
{ if (s_mg_active) merge_flush(); p_glCompressedTexImage2D(a0, a1, a2, a3, a4, a5, a6, a7); }
#undef glCompressedTexImage2D
#define glCompressedTexImage2D mg_glCompressedTexImage2D

static inline void mg_glTexParameteri(GLenum a0, GLenum a1, GLint a2)
{ if (s_mg_active) merge_flush(); p_glTexParameteri(a0, a1, a2); }
#undef glTexParameteri
#define glTexParameteri mg_glTexParameteri

static inline void mg_glFramebufferTexture2D(GLenum a0, GLenum a1, GLenum a2, GLuint a3, GLint a4)
{ if (s_mg_active) merge_flush(); p_glFramebufferTexture2D(a0, a1, a2, a3, a4); }
#undef glFramebufferTexture2D
#define glFramebufferTexture2D mg_glFramebufferTexture2D

static inline void mg_glFramebufferRenderbuffer(GLenum a0, GLenum a1, GLenum a2, GLuint a3)
{ if (s_mg_active) merge_flush(); p_glFramebufferRenderbuffer(a0, a1, a2, a3); }
#undef glFramebufferRenderbuffer
#define glFramebufferRenderbuffer mg_glFramebufferRenderbuffer

static inline void mg_glBlitFramebuffer(GLint a0, GLint a1, GLint a2, GLint a3, GLint a4, GLint a5, GLint a6, GLint a7, GLbitfield a8, GLenum a9)
{ if (s_mg_active) merge_flush(); p_glBlitFramebuffer(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9); }
#undef glBlitFramebuffer
#define glBlitFramebuffer mg_glBlitFramebuffer

static inline void mg_glDeleteRenderbuffers(GLsizei a0, const GLuint * a1)
{ if (s_mg_active) merge_flush(); p_glDeleteRenderbuffers(a0, a1); }
#undef glDeleteRenderbuffers
#define glDeleteRenderbuffers mg_glDeleteRenderbuffers

static inline void mg_glRenderbufferStorage(GLenum a0, GLenum a1, GLsizei a2, GLsizei a3)
{ if (s_mg_active) merge_flush(); p_glRenderbufferStorage(a0, a1, a2, a3); }
#undef glRenderbufferStorage
#define glRenderbufferStorage mg_glRenderbufferStorage

static inline void mg_glDeleteProgram(GLuint a0)
{ if (s_mg_active) merge_flush(); p_glDeleteProgram(a0); }
#undef glDeleteProgram
#define glDeleteProgram mg_glDeleteProgram

static inline void mg_glLinkProgram(GLuint a0)
{ if (s_mg_active) merge_flush(); p_glLinkProgram(a0); }
#undef glLinkProgram
#define glLinkProgram mg_glLinkProgram

static inline void mg_glUseProgram(GLuint a0)
{ if (s_mg_active) merge_flush(); p_glUseProgram(a0); }
#undef glUseProgram
#define glUseProgram mg_glUseProgram

static inline void mg_glUniform1i(GLint a0, GLint a1)
{ if (s_mg_active) merge_flush(); p_glUniform1i(a0, a1); }
#undef glUniform1i
#define glUniform1i mg_glUniform1i

static inline void mg_glUniform1iv(GLint a0, GLsizei a1, const GLint * a2)
{ if (s_mg_active) merge_flush(); p_glUniform1iv(a0, a1, a2); }
#undef glUniform1iv
#define glUniform1iv mg_glUniform1iv

static inline void mg_glUniform1f(GLint a0, GLfloat a1)
{ if (s_mg_active) merge_flush(); p_glUniform1f(a0, a1); }
#undef glUniform1f
#define glUniform1f mg_glUniform1f

static inline void mg_glUniform2fv(GLint a0, GLsizei a1, const GLfloat * a2)
{ if (s_mg_active) merge_flush(); p_glUniform2fv(a0, a1, a2); }
#undef glUniform2fv
#define glUniform2fv mg_glUniform2fv

static inline void mg_glUniform4fv(GLint a0, GLsizei a1, const GLfloat * a2)
{ if (s_mg_active) merge_flush(); p_glUniform4fv(a0, a1, a2); }
#undef glUniform4fv
#define glUniform4fv mg_glUniform4fv

static inline void mg_glBufferData(GLenum a0, GLsizeiptr a1, const void * a2, GLenum a3)
{ if (s_mg_active) merge_flush(); p_glBufferData(a0, a1, a2, a3); }
#undef glBufferData
#define glBufferData mg_glBufferData

static inline void mg_glVertexAttribPointer(GLuint a0, GLint a1, GLenum a2, GLboolean a3, GLsizei a4, const void * a5)
{ if (s_mg_active) merge_flush(); p_glVertexAttribPointer(a0, a1, a2, a3, a4, a5); }
#undef glVertexAttribPointer
#define glVertexAttribPointer mg_glVertexAttribPointer

static inline void mg_glEnableVertexAttribArray(GLuint a0)
{ if (s_mg_active) merge_flush(); p_glEnableVertexAttribArray(a0); }
#undef glEnableVertexAttribArray
#define glEnableVertexAttribArray mg_glEnableVertexAttribArray

static inline void mg_glDisableVertexAttribArray(GLuint a0)
{ if (s_mg_active) merge_flush(); p_glDisableVertexAttribArray(a0); }
#undef glDisableVertexAttribArray
#define glDisableVertexAttribArray mg_glDisableVertexAttribArray

static inline void mg_glVertexAttrib4f(GLuint a0, GLfloat a1, GLfloat a2, GLfloat a3, GLfloat a4)
{ if (s_mg_active) merge_flush(); p_glVertexAttrib4f(a0, a1, a2, a3, a4); }
#undef glVertexAttrib4f
#define glVertexAttrib4f mg_glVertexAttrib4f

static inline void mg_glDrawElements(GLenum a0, GLsizei a1, GLenum a2, const void * a3)
{ if (s_mg_active) merge_flush(); p_glDrawElements(a0, a1, a2, a3); }
#undef glDrawElements
#define glDrawElements mg_glDrawElements

static inline void mg_glDrawArrays(GLenum a0, GLint a1, GLsizei a2)
{ if (s_mg_active) merge_flush(); p_glDrawArrays(a0, a1, a2); }
#undef glDrawArrays
#define glDrawArrays mg_glDrawArrays


/* Bindings, cached: unchanged ones skip the call (and the flush). The
 * element-array binding belongs to the vertex array, so a new vertex array
 * forgets it. A deleted framebuffer may have been bound: forget them. */
static GLuint s_mg_fb_draw = 0xFFFFFFFFu, s_mg_fb_read = 0xFFFFFFFFu;
static GLuint s_mg_vao = 0xFFFFFFFFu, s_mg_ab = 0xFFFFFFFFu, s_mg_eab = 0xFFFFFFFFu;

static inline void mg_glBindFramebuffer(GLenum t, GLuint fb)
{
    if (t == 0x8D40) {                          /* GL_FRAMEBUFFER: both */
        if (s_mg_fb_draw == fb && s_mg_fb_read == fb) return;
        s_mg_fb_draw = s_mg_fb_read = fb;
    } else if (t == 0x8CA9) {                   /* GL_DRAW_FRAMEBUFFER */
        if (s_mg_fb_draw == fb) return;
        s_mg_fb_draw = fb;
    } else if (t == 0x8CA8) {                   /* GL_READ_FRAMEBUFFER */
        if (s_mg_fb_read == fb) return;
        s_mg_fb_read = fb;
    }
    if (s_mg_active) merge_flush();
    p_glBindFramebuffer(t, fb);
}
#undef glBindFramebuffer
#define glBindFramebuffer mg_glBindFramebuffer

static inline void mg_glDeleteFramebuffers(GLsizei n, const GLuint *fbs)
{
    if (s_mg_active) merge_flush();
    s_mg_fb_draw = s_mg_fb_read = 0xFFFFFFFFu;
    p_glDeleteFramebuffers(n, fbs);
}
#undef glDeleteFramebuffers
#define glDeleteFramebuffers mg_glDeleteFramebuffers

static inline void mg_glBindVertexArray(GLuint v)
{
    if (s_mg_vao == v) return;
    if (s_mg_active) merge_flush();
    s_mg_vao = v;
    s_mg_eab = 0xFFFFFFFFu;
    p_glBindVertexArray(v);
}
#undef glBindVertexArray
#define glBindVertexArray mg_glBindVertexArray

static inline void mg_glBindBuffer(GLenum t, GLuint b)
{
    GLuint *c = t == 0x8892 ? &s_mg_ab : t == 0x8893 ? &s_mg_eab : NULL;  /* ARRAY / ELEMENT_ARRAY */
    if (c && *c == b) return;
    if (s_mg_active && t == 0x8893) merge_flush();   /* the held draw's indices */
    if (c) *c = b;
    p_glBindBuffer(t, b);
}
#undef glBindBuffer
#define glBindBuffer mg_glBindBuffer

#endif
