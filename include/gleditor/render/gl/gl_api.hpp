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
  PFNGLVIEWPORTPROC Viewport{};
  PFNGLCLEARPROC Clear{};
  PFNGLCLEARCOLORPROC ClearColor{};
  PFNGLFRONTFACEPROC FrontFace{};
  PFNGLGETINTEGERVPROC GetIntegerv{};
  PFNGLGETERRORPROC GetError{};
  PFNGLGETSTRINGPROC GetString{};
  PFNGLDRAWARRAYSINSTANCEDPROC DrawArraysInstanced{};

  /**
   * @brief Resolve every entry point above against the current context.
   * @throws std::runtime_error naming the first entry point that is missing.
   */
  void load();
};

} // namespace render::gl

#endif // GLEDITOR_RENDER_GL_API_H
// vi: set sw=2 sts=2 ts=2 et:
