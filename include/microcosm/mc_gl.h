/*
 * mc_gl.h - a hand-rolled OpenGL 4.5 core loader.
 *
 * The engine uses OpenGL because, for something this small, it still costs the
 * fewest bytes. Classic GL leaves the state tracking, allocation and
 * synchronisation to the driver, which costs the executable nothing. Vulkan
 * and D3D12 hand that work to the application: pages of pipeline and
 * descriptor setup plus a bytecode shader toolchain, all of it in the
 * executable. For a size-limited intro, less control and more done by the
 * driver is the better trade.
 *
 * Linking libGL would import ~60 symbols by name and make the binary bigger.
 * Instead, this loader fetches every entry point at runtime through
 * eglGetProcAddress into one table, `mcgl`, and #defines the usual gl* names
 * onto it. The call sites read like plain OpenGL, and the ELF imports no GL
 * symbols.
 *
 * <GL/gl.h> and <GL/glext.h> are included only for the types (GLenum,
 * GLuint), the GL_* enum values and the PFNGL*PROC typedefs. We never define
 * GL_GLEXT_PROTOTYPES, so nothing is referenced at link time.
 */
#ifndef MICROCOSM_MC_GL_H
#define MICROCOSM_MC_GL_H

#include <GL/gl.h>
#include <GL/glext.h>

/* The GL 1.0/1.1 calls are declared by <GL/gl.h> but have no PFN typedefs
 * there; these fill the gap so they can sit in the table with the rest. */
typedef void      (APIENTRYP MC_PFN_CLEAR)(GLbitfield);
typedef void      (APIENTRYP MC_PFN_CLEARCOLOR)(GLfloat, GLfloat, GLfloat, GLfloat);
typedef void      (APIENTRYP MC_PFN_CLEARDEPTH)(GLdouble);
typedef void      (APIENTRYP MC_PFN_SCISSOR)(GLint, GLint, GLsizei, GLsizei);
typedef void      (APIENTRYP MC_PFN_VIEWPORT)(GLint, GLint, GLsizei, GLsizei);
typedef void      (APIENTRYP MC_PFN_ENABLE)(GLenum);
typedef void      (APIENTRYP MC_PFN_DISABLE)(GLenum);
typedef void      (APIENTRYP MC_PFN_DEPTHFUNC)(GLenum);
typedef void      (APIENTRYP MC_PFN_DEPTHMASK)(GLboolean);
typedef void      (APIENTRYP MC_PFN_BLENDFUNC)(GLenum, GLenum);
typedef void      (APIENTRYP MC_PFN_BLENDCOLOR)(GLfloat, GLfloat, GLfloat, GLfloat);
typedef void      (APIENTRYP MC_PFN_CULLFACE)(GLenum);
typedef void      (APIENTRYP MC_PFN_FRONTFACE)(GLenum);
typedef void      (APIENTRYP MC_PFN_FINISH)(void);
typedef GLenum    (APIENTRYP MC_PFN_GETERROR)(void);
typedef void      (APIENTRYP MC_PFN_GETINTEGERV)(GLenum, GLint *);
typedef const GLubyte *(APIENTRYP MC_PFN_GETSTRING)(GLenum);
typedef void      (APIENTRYP MC_PFN_READPIXELS)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
typedef void      (APIENTRYP MC_PFN_PIXELSTOREI)(GLenum, GLint);
typedef void      (APIENTRYP MC_PFN_GENTEXTURES)(GLsizei, GLuint *);
typedef void      (APIENTRYP MC_PFN_BINDTEXTURE)(GLenum, GLuint);
typedef void      (APIENTRYP MC_PFN_TEXIMAGE2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
typedef void      (APIENTRYP MC_PFN_TEXPARAMETERI)(GLenum, GLenum, GLint);
typedef void      (APIENTRYP MC_PFN_COPYTEXSUBIMAGE2D)(GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei);
typedef void      (APIENTRYP MC_PFN_DELETETEXTURES)(GLsizei, const GLuint *);
typedef void      (APIENTRYP MC_PFN_DRAWARRAYS)(GLenum, GLint, GLsizei);
typedef void      (APIENTRYP MC_PFN_DRAWELEMENTS)(GLenum, GLsizei, GLenum, const void *);
typedef void      (APIENTRYP MC_PFN_BUFFERMODE)(GLenum);

/* Every entry point we resolve, as X(Name, PFN type), where Name is the GL
 * name without its "gl". This one list lays out both the table and the
 * loader's name blob, so adding a function takes one line here plus its
 * #define below. */
#define MC_GL_FUNCS(X) \
    /* shaders & programs */ \
    X(CreateShader,            PFNGLCREATESHADERPROC) \
    X(ShaderSource,            PFNGLSHADERSOURCEPROC) \
    X(CompileShader,           PFNGLCOMPILESHADERPROC) \
    X(GetShaderiv,             PFNGLGETSHADERIVPROC) \
    X(CreateProgram,           PFNGLCREATEPROGRAMPROC) \
    X(AttachShader,            PFNGLATTACHSHADERPROC) \
    X(LinkProgram,             PFNGLLINKPROGRAMPROC) \
    X(GetProgramiv,            PFNGLGETPROGRAMIVPROC) \
    X(UseProgram,              PFNGLUSEPROGRAMPROC) \
    X(DeleteProgram,           PFNGLDELETEPROGRAMPROC) \
    X(Uniform1i,               PFNGLUNIFORM1IPROC) \
    X(Uniform2f,               PFNGLUNIFORM2FPROC) \
    X(Uniform4f,               PFNGLUNIFORM4FPROC) \
    X(Uniform4fv,              PFNGLUNIFORM4FVPROC) \
    X(UniformMatrix4fv,        PFNGLUNIFORMMATRIX4FVPROC) \
    /* buffers & vertex arrays */ \
    X(GenVertexArrays,         PFNGLGENVERTEXARRAYSPROC) \
    X(BindVertexArray,         PFNGLBINDVERTEXARRAYPROC) \
    X(GenBuffers,              PFNGLGENBUFFERSPROC) \
    X(BindBuffer,              PFNGLBINDBUFFERPROC) \
    X(BindBufferBase,          PFNGLBINDBUFFERBASEPROC) \
    X(BufferData,              PFNGLBUFFERDATAPROC) \
    X(BufferSubData,           PFNGLBUFFERSUBDATAPROC) \
    X(GetBufferSubData,        PFNGLGETBUFFERSUBDATAPROC) \
    X(TexImage3D,              PFNGLTEXIMAGE3DPROC) \
    X(DeleteBuffers,           PFNGLDELETEBUFFERSPROC) \
    X(EnableVertexAttribArray, PFNGLENABLEVERTEXATTRIBARRAYPROC) \
    X(VertexAttribPointer,     PFNGLVERTEXATTRIBPOINTERPROC) \
    X(VertexAttribDivisor,     PFNGLVERTEXATTRIBDIVISORPROC) \
    X(DrawArraysInstanced,     PFNGLDRAWARRAYSINSTANCEDPROC) \
    X(DrawElementsInstanced,   PFNGLDRAWELEMENTSINSTANCEDPROC) \
    X(DispatchCompute,         PFNGLDISPATCHCOMPUTEPROC) \
    X(MemoryBarrier,           PFNGLMEMORYBARRIERPROC) \
    /* textures & framebuffers */ \
    X(ActiveTexture,           PFNGLACTIVETEXTUREPROC) \
    X(GenerateMipmap,          PFNGLGENERATEMIPMAPPROC) \
    X(GenFramebuffers,         PFNGLGENFRAMEBUFFERSPROC) \
    X(BindFramebuffer,         PFNGLBINDFRAMEBUFFERPROC) \
    X(FramebufferTexture2D,    PFNGLFRAMEBUFFERTEXTURE2DPROC) \
    X(CheckFramebufferStatus,  PFNGLCHECKFRAMEBUFFERSTATUSPROC) \
    X(DeleteFramebuffers,      PFNGLDELETEFRAMEBUFFERSPROC) \
    X(GenSamplers,             PFNGLGENSAMPLERSPROC) \
    X(SamplerParameteri,       PFNGLSAMPLERPARAMETERIPROC) \
    X(BindSampler,             PFNGLBINDSAMPLERPROC) \
    /* the 1.0/1.1 calls */ \
    X(Clear,                   MC_PFN_CLEAR) \
    X(ClearDepth,              MC_PFN_CLEARDEPTH) \
    X(Scissor,                 MC_PFN_SCISSOR) \
    X(ClearColor,              MC_PFN_CLEARCOLOR) \
    X(Viewport,                MC_PFN_VIEWPORT) \
    X(Enable,                  MC_PFN_ENABLE) \
    X(Disable,                 MC_PFN_DISABLE) \
    X(DepthFunc,               MC_PFN_DEPTHFUNC) \
    X(DepthMask,               MC_PFN_DEPTHMASK) \
    X(BlendFunc,               MC_PFN_BLENDFUNC) \
    X(BlendColor,              MC_PFN_BLENDCOLOR) \
    X(CullFace,                MC_PFN_CULLFACE) \
    X(FrontFace,               MC_PFN_FRONTFACE) \
    X(GetIntegerv,             MC_PFN_GETINTEGERV) \
    X(GenTextures,             MC_PFN_GENTEXTURES) \
    X(BindTexture,             MC_PFN_BINDTEXTURE) \
    X(TexImage2D,              MC_PFN_TEXIMAGE2D) \
    X(TexParameteri,           MC_PFN_TEXPARAMETERI) \
    X(CopyTexSubImage2D,       MC_PFN_COPYTEXSUBIMAGE2D) \
    X(DeleteTextures,          MC_PFN_DELETETEXTURES) \
    X(DrawArrays,              MC_PFN_DRAWARRAYS) \
    X(DrawBuffer,              MC_PFN_BUFFERMODE) \
    X(ReadBuffer,              MC_PFN_BUFFERMODE)

/* Entry points the shipped payload (MC_IMPORT_HASH) never calls, so it doesn't
 * resolve them: diagnostics, the headless backend's depth renderbuffer, pixel
 * readback, uniforms by name and the float/vec3 setters, the non-instanced
 * indexed draw (the engine draws through the instanced one), and the teardown
 * the payload skips. A stray use in the payload fails to compile instead of
 * calling through NULL. Going the other way, tools/importcheck.py fails the
 * payload build if a slot in the list above goes unused. */
#ifndef MC_IMPORT_HASH
#define MC_GL_TOOLS(X) \
    X(GetUniformLocation,      PFNGLGETUNIFORMLOCATIONPROC) \
    X(Uniform1f,               PFNGLUNIFORM1FPROC) \
    X(Uniform3f,               PFNGLUNIFORM3FPROC) \
    X(ReadPixels,              MC_PFN_READPIXELS) \
    X(PixelStorei,             MC_PFN_PIXELSTOREI) \
    X(DeleteShader,            PFNGLDELETESHADERPROC) \
    X(DeleteVertexArrays,      PFNGLDELETEVERTEXARRAYSPROC) \
    X(GetShaderInfoLog,        PFNGLGETSHADERINFOLOGPROC) \
    X(GetProgramInfoLog,       PFNGLGETPROGRAMINFOLOGPROC) \
    X(GenRenderbuffers,        PFNGLGENRENDERBUFFERSPROC) \
    X(BindRenderbuffer,        PFNGLBINDRENDERBUFFERPROC) \
    X(RenderbufferStorage,     PFNGLRENDERBUFFERSTORAGEPROC) \
    X(FramebufferRenderbuffer, PFNGLFRAMEBUFFERRENDERBUFFERPROC) \
    X(Finish,                  MC_PFN_FINISH) \
    X(GetError,                MC_PFN_GETERROR) \
    X(GetString,               MC_PFN_GETSTRING) \
    X(DrawElements,            MC_PFN_DRAWELEMENTS)
#else
#define MC_GL_TOOLS(X)
#endif

#define MC_GL_ALL(X) MC_GL_FUNCS(X) MC_GL_TOOLS(X)

typedef void (*mc_gl_proc)(void);

#define MC_GL_INDEX_(name, type)  mc_gl_index_##name,
#define MC_GL_MEMBER_(name, type) type name;
enum { MC_GL_ALL(MC_GL_INDEX_) MC_GL_COUNT };

/* The resolved entry points: typed members for the call sites, and the same
 * storage as a plain array so the loader can fill it in one loop. */
typedef union mc_gl_api {
    struct { MC_GL_ALL(MC_GL_MEMBER_) };
    mc_gl_proc slot[MC_GL_COUNT];
} mc_gl_api;

extern mc_gl_api mcgl;
#define mcgl1 mcgl   /* an alias, which some of the checks use */

/* Resolve all entry points. Returns 1 on success, 0 if any are missing.
 * `getproc` is eglGetProcAddress (passed in so this file needs no EGL). */
int mc_gl_load(mc_gl_proc (*getproc)(const char *name));
/* mc_gl_fullscreen_vao is an empty VAO, made on first use, for draws with no
 * vertex data. One engine context per process at a time: call
 * mc_gl_context_release while the context is still current, before destroying
 * it. The platform backends do this for you. */
GLuint mc_gl_fullscreen_vao(void);
#ifndef MC_IMPORT_HASH   /* the payload never closes its context: it exits */
void mc_gl_context_release(void);
#endif
/* The per-frame uniform block: upload `bytes` of std140 data to the buffer on
 * binding point 0. Every program's blocks start out on binding 0 after each
 * (re)link, so any shader that declares the block sees it with no
 * per-program setup. You can re-upload between draws as often as you like;
 * earlier draws keep the data they had. mc_renderer_end uploads the post
 * stack's own block here, so upload yours each frame before your draws. */
void mc_gl_frame_block(const void *data, GLsizeiptr bytes);

/* Map the usual gl* names onto the table. A macro can't generate #defines, so
 * this list mirrors MC_GL_FUNCS and MC_GL_TOOLS by hand. gl.h's own 1.0/1.1
 * prototypes are harmless, since libGL is never linked. */
#define glCreateShader            mcgl.CreateShader
#define glShaderSource            mcgl.ShaderSource
#define glCompileShader           mcgl.CompileShader
#define glGetShaderiv             mcgl.GetShaderiv
#define glGetShaderInfoLog        mcgl.GetShaderInfoLog
#define glDeleteShader            mcgl.DeleteShader
#define glCreateProgram           mcgl.CreateProgram
#define glAttachShader            mcgl.AttachShader
#define glLinkProgram             mcgl.LinkProgram
#define glGetProgramiv            mcgl.GetProgramiv
#define glGetProgramInfoLog       mcgl.GetProgramInfoLog
#define glUseProgram              mcgl.UseProgram
#define glDeleteProgram           mcgl.DeleteProgram
#define glGetUniformLocation      mcgl.GetUniformLocation
#define glUniform1i               mcgl.Uniform1i
#define glUniform1f               mcgl.Uniform1f
#define glUniform2f               mcgl.Uniform2f
#define glUniform3f               mcgl.Uniform3f
#define glUniform4f               mcgl.Uniform4f
#define glUniform4fv              mcgl.Uniform4fv
#define glUniformMatrix4fv        mcgl.UniformMatrix4fv
#define glGenVertexArrays         mcgl.GenVertexArrays
#define glBindVertexArray         mcgl.BindVertexArray
#define glDeleteVertexArrays      mcgl.DeleteVertexArrays
#define glGenBuffers              mcgl.GenBuffers
#define glBindBuffer              mcgl.BindBuffer
#define glBindBufferBase          mcgl.BindBufferBase
#define glBufferData              mcgl.BufferData
#define glBufferSubData           mcgl.BufferSubData
#define glGetBufferSubData        mcgl.GetBufferSubData
#define glTexImage3D              mcgl.TexImage3D
#define glDeleteBuffers           mcgl.DeleteBuffers
#define glEnableVertexAttribArray mcgl.EnableVertexAttribArray
#define glVertexAttribPointer     mcgl.VertexAttribPointer
#define glVertexAttribDivisor     mcgl.VertexAttribDivisor
#define glDrawElementsInstanced   mcgl.DrawElementsInstanced
#define glDispatchCompute         mcgl.DispatchCompute
#define glMemoryBarrier           mcgl.MemoryBarrier
#define glDrawArraysInstanced     mcgl.DrawArraysInstanced
#define glActiveTexture           mcgl.ActiveTexture
#define glGenerateMipmap          mcgl.GenerateMipmap
#define glGenFramebuffers         mcgl.GenFramebuffers
#define glBindFramebuffer         mcgl.BindFramebuffer
#define glFramebufferTexture2D    mcgl.FramebufferTexture2D
#define glGenRenderbuffers        mcgl.GenRenderbuffers
#define glBindRenderbuffer        mcgl.BindRenderbuffer
#define glRenderbufferStorage     mcgl.RenderbufferStorage
#define glFramebufferRenderbuffer mcgl.FramebufferRenderbuffer
#define glCheckFramebufferStatus  mcgl.CheckFramebufferStatus
#define glDeleteFramebuffers      mcgl.DeleteFramebuffers
#define glGenSamplers             mcgl.GenSamplers
#define glSamplerParameteri       mcgl.SamplerParameteri
#define glBindSampler             mcgl.BindSampler
#define glClear                   mcgl.Clear
#define glClearDepth              mcgl.ClearDepth
#define glScissor                 mcgl.Scissor
#define glClearColor              mcgl.ClearColor
#define glViewport                mcgl.Viewport
#define glEnable                  mcgl.Enable
#define glDisable                 mcgl.Disable
#define glDepthFunc               mcgl.DepthFunc
#define glDepthMask               mcgl.DepthMask
#define glBlendFunc               mcgl.BlendFunc
#define glBlendColor              mcgl.BlendColor
#define glCullFace                mcgl.CullFace
#define glFrontFace               mcgl.FrontFace
#define glFinish                  mcgl.Finish
#define glGetError                mcgl.GetError
#define glGetIntegerv             mcgl.GetIntegerv
#define glGetString               mcgl.GetString
#define glReadPixels              mcgl.ReadPixels
#define glPixelStorei             mcgl.PixelStorei
#define glGenTextures             mcgl.GenTextures
#define glBindTexture             mcgl.BindTexture
#define glTexImage2D              mcgl.TexImage2D
#define glTexParameteri           mcgl.TexParameteri
#define glCopyTexSubImage2D       mcgl.CopyTexSubImage2D
#define glDeleteTextures          mcgl.DeleteTextures
#define glDrawArrays              mcgl.DrawArrays
#define glDrawElements            mcgl.DrawElements
#define glDrawBuffer              mcgl.DrawBuffer
#define glReadBuffer              mcgl.ReadBuffer

/* GL error check: active only in dev builds. */
#if defined(MC_DEV)
void mc_gl_check_(const char *where);
#define MC_GL_CHECK() mc_gl_check_(__func__)
#else
#define MC_GL_CHECK() ((void)0)
#endif

#endif /* MICROCOSM_MC_GL_H */
