/**
 * @file gl_api.hpp
 * @brief Entry point table for the OpenGL and OpenGL ES backends.
 *
 * The entry points are resolved through SDL rather than linked against
 * libGL or libGLESv2 directly. That matters for two reasons: the two
 * libraries export the same symbol names, so a binary that supports both
 * cannot link both; and it keeps the choice of desktop GL versus ES a runtime
 * decision made from the context SDL actually created.
 *
 * Only the subset the renderer uses is loaded. Every entry here exists in both
 * OpenGL 3.3 core and OpenGL ES 3.0 with identical signatures and token
 * values, which is what lets one backend implementation drive both.
 *
 * GL/glcorearb.h is included purely for its typedefs and enum values; it
 * declares no prototypes unless GL_GLEXT_PROTOTYPES is defined, so nothing
 * here creates a link-time dependency on a GL library.
 */
#ifndef GLEDITOR_RENDER_GL_API_H
#define GLEDITOR_RENDER_GL_API_H

// On Windows, GL/glcorearb.h includes windows.h for its calling conventions,
// and windows.h brings in a great deal besides. Two pieces of that actively
// break C++ that has nothing to do with it: winspool.h defines
// DeviceCapabilities as a macro aliasing DeviceCapabilitiesA, which renamed
// this project's own render::DeviceCapabilities out from under every use of
// it, and windows.h defines min and max as macros, which breaks std::min and
// std::max. Both are asked not to appear. The defines are guarded so that a
// translation unit that already set them is left alone.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef NOGDI
#define NOGDI
#endif
#endif

#include <GL/glcorearb.h>

namespace render::gl {

/**
 * @brief Resolved OpenGL / OpenGL ES entry points.
 *
 * Members keep the GL names minus the `gl` prefix, so `api.DrawArraysInstanced`
 * reads as the call it makes.
 */
struct GLApi {
  // -- buffers
  PFNGLGENBUFFERSPROC GenBuffers{};
  PFNGLDELETEBUFFERSPROC DeleteBuffers{};
  PFNGLBINDBUFFERPROC BindBuffer{};
  PFNGLBINDBUFFERBASEPROC BindBufferBase{};
  PFNGLBUFFERDATAPROC BufferData{};
  PFNGLBUFFERSUBDATAPROC BufferSubData{};
  PFNGLCOPYBUFFERSUBDATAPROC CopyBufferSubData{};
  PFNGLMAPBUFFERRANGEPROC MapBufferRange{};
  PFNGLUNMAPBUFFERPROC UnmapBuffer{};

  // -- vertex arrays
  PFNGLGENVERTEXARRAYSPROC GenVertexArrays{};
  PFNGLDELETEVERTEXARRAYSPROC DeleteVertexArrays{};
  PFNGLBINDVERTEXARRAYPROC BindVertexArray{};
  PFNGLENABLEVERTEXATTRIBARRAYPROC EnableVertexAttribArray{};
  PFNGLVERTEXATTRIBPOINTERPROC VertexAttribPointer{};
  PFNGLVERTEXATTRIBIPOINTERPROC VertexAttribIPointer{};
  PFNGLVERTEXATTRIBDIVISORPROC VertexAttribDivisor{};

  // -- textures
  PFNGLGENTEXTURESPROC GenTextures{};
  PFNGLDELETETEXTURESPROC DeleteTextures{};
  PFNGLBINDTEXTUREPROC BindTexture{};
  PFNGLACTIVETEXTUREPROC ActiveTexture{};
  PFNGLTEXPARAMETERIPROC TexParameteri{};
  PFNGLTEXIMAGE3DPROC TexImage3D{};
  PFNGLTEXSUBIMAGE3DPROC TexSubImage3D{};
  PFNGLPIXELSTOREIPROC PixelStorei{};
  PFNGLGENERATEMIPMAPPROC GenerateMipmap{};

  // -- programs
  PFNGLCREATESHADERPROC CreateShader{};
  PFNGLSHADERSOURCEPROC ShaderSource{};
  PFNGLCOMPILESHADERPROC CompileShader{};
  PFNGLGETSHADERIVPROC GetShaderiv{};
  PFNGLGETSHADERINFOLOGPROC GetShaderInfoLog{};
  PFNGLDELETESHADERPROC DeleteShader{};
  PFNGLCREATEPROGRAMPROC CreateProgram{};
  PFNGLATTACHSHADERPROC AttachShader{};
  PFNGLDETACHSHADERPROC DetachShader{};
  PFNGLLINKPROGRAMPROC LinkProgram{};
  PFNGLGETPROGRAMIVPROC GetProgramiv{};
  PFNGLGETPROGRAMINFOLOGPROC GetProgramInfoLog{};
  PFNGLUSEPROGRAMPROC UseProgram{};
  PFNGLDELETEPROGRAMPROC DeleteProgram{};
  PFNGLGETUNIFORMLOCATIONPROC GetUniformLocation{};
  PFNGLUNIFORM1IPROC Uniform1i{};
  PFNGLUNIFORM1FPROC Uniform1f{};
  PFNGLUNIFORM1UIPROC Uniform1ui{};
  PFNGLUNIFORMMATRIX4FVPROC UniformMatrix4fv{};
  PFNGLGETUNIFORMBLOCKINDEXPROC GetUniformBlockIndex{};
  PFNGLUNIFORMBLOCKBINDINGPROC UniformBlockBinding{};

  // -- framebuffers
  PFNGLGENFRAMEBUFFERSPROC GenFramebuffers{};
  PFNGLDELETEFRAMEBUFFERSPROC DeleteFramebuffers{};
  PFNGLBINDFRAMEBUFFERPROC BindFramebuffer{};
  PFNGLGENRENDERBUFFERSPROC GenRenderbuffers{};
  PFNGLDELETERENDERBUFFERSPROC DeleteRenderbuffers{};
  PFNGLBINDRENDERBUFFERPROC BindRenderbuffer{};
  PFNGLRENDERBUFFERSTORAGEPROC RenderbufferStorage{};
  PFNGLFRAMEBUFFERRENDERBUFFERPROC FramebufferRenderbuffer{};
  PFNGLCHECKFRAMEBUFFERSTATUSPROC CheckFramebufferStatus{};
  PFNGLDRAWBUFFERSPROC DrawBuffers{};
  PFNGLBLITFRAMEBUFFERPROC BlitFramebuffer{};
  PFNGLREADBUFFERPROC ReadBuffer{};
  PFNGLREADPIXELSPROC ReadPixels{};
  PFNGLCLEARBUFFERUIVPROC ClearBufferuiv{};

  // -- state
  PFNGLENABLEPROC Enable{};
  PFNGLDISABLEPROC Disable{};
  PFNGLDEPTHFUNCPROC DepthFunc{};
  PFNGLBLENDFUNCPROC BlendFunc{};
  PFNGLVIEWPORTPROC Viewport{};
  PFNGLCLEARPROC Clear{};
  PFNGLCLEARCOLORPROC ClearColor{};
  PFNGLFRONTFACEPROC FrontFace{};
  PFNGLGETINTEGERVPROC GetIntegerv{};
  PFNGLGETERRORPROC GetError{};
  PFNGLGETSTRINGPROC GetString{};
  PFNGLGETSTRINGIPROC GetStringi{};
  PFNGLDRAWARRAYSINSTANCEDPROC DrawArraysInstanced{};

  // -- fences, used to tell when an asynchronous pixel read has landed
  PFNGLFENCESYNCPROC FenceSync{};
  PFNGLCLIENTWAITSYNCPROC ClientWaitSync{};
  PFNGLDELETESYNCPROC DeleteSync{};

  // -- debug output. KHR_debug is core only in OpenGL 4.3 and OpenGL ES 3.2,
  // above what this backend requires, so these stay null when the context does
  // not offer them and the caller checks before use.
  PFNGLDEBUGMESSAGECALLBACKPROC DebugMessageCallback{};
  PFNGLDEBUGMESSAGECONTROLPROC DebugMessageControl{};

  /**
   * @brief Resolve every mandatory entry point against the current context.
   * @throws std::runtime_error naming the first entry point that is missing.
   */
  void load();

  /// True when the context advertises @p name in its extension list.
  [[nodiscard]] bool hasExtension(const char *name) const;

  /**
   * @brief Resolve the optional debug output entry points.
   * @return true if debug output can be used on this context.
   */
  bool loadDebugOutput();
};

} // namespace render::gl

#endif // GLEDITOR_RENDER_GL_API_H
