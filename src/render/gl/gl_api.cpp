/**
 * @file gl_api.cpp
 * @brief Resolution of the GL/GLES entry point table through SDL.
 */
#include <gleditor/render/gl/gl_api.hpp> // IWYU pragma: associated

#include <SDL3/SDL_video.h>

#include <stdexcept>
#include <string>

namespace render::gl {

namespace {

/**
 * @brief Resolve one entry point, failing loudly rather than storing null.
 *
 * A missing entry point means the context is older or less capable than the
 * backend requires, and finding that out here beats a null call later.
 */
template <typename Fn> void resolve(Fn &slot, const char *name) {
  // SDL returns a generic function pointer; the cast to the specific
  // signature is the documented way to consume it.
  auto *addr = SDL_GL_GetProcAddress(name);
  if (nullptr == addr) {
    throw std::runtime_error(std::string("Missing GL entry point: ") + name);
  }
  slot = reinterpret_cast<Fn>(addr);
}

} // namespace

void GLApi::load() {
#define GLEDITOR_RESOLVE(member) resolve(member, "gl" #member)

  GLEDITOR_RESOLVE(GenBuffers);
  GLEDITOR_RESOLVE(DeleteBuffers);
  GLEDITOR_RESOLVE(BindBuffer);
  GLEDITOR_RESOLVE(BindBufferBase);
  GLEDITOR_RESOLVE(BufferData);
  GLEDITOR_RESOLVE(BufferSubData);
  GLEDITOR_RESOLVE(CopyBufferSubData);

  GLEDITOR_RESOLVE(GenVertexArrays);
  GLEDITOR_RESOLVE(DeleteVertexArrays);
  GLEDITOR_RESOLVE(BindVertexArray);
  GLEDITOR_RESOLVE(EnableVertexAttribArray);
  GLEDITOR_RESOLVE(VertexAttribPointer);
  GLEDITOR_RESOLVE(VertexAttribIPointer);
  GLEDITOR_RESOLVE(VertexAttribDivisor);

  GLEDITOR_RESOLVE(GenTextures);
  GLEDITOR_RESOLVE(DeleteTextures);
  GLEDITOR_RESOLVE(BindTexture);
  GLEDITOR_RESOLVE(ActiveTexture);
  GLEDITOR_RESOLVE(TexParameteri);
  GLEDITOR_RESOLVE(TexImage3D);
  GLEDITOR_RESOLVE(TexSubImage3D);
  GLEDITOR_RESOLVE(PixelStorei);

  GLEDITOR_RESOLVE(CreateShader);
  GLEDITOR_RESOLVE(ShaderSource);
  GLEDITOR_RESOLVE(CompileShader);
  GLEDITOR_RESOLVE(GetShaderiv);
  GLEDITOR_RESOLVE(GetShaderInfoLog);
  GLEDITOR_RESOLVE(DeleteShader);
  GLEDITOR_RESOLVE(CreateProgram);
  GLEDITOR_RESOLVE(AttachShader);
  GLEDITOR_RESOLVE(DetachShader);
  GLEDITOR_RESOLVE(LinkProgram);
  GLEDITOR_RESOLVE(GetProgramiv);
  GLEDITOR_RESOLVE(GetProgramInfoLog);
  GLEDITOR_RESOLVE(UseProgram);
  GLEDITOR_RESOLVE(DeleteProgram);
  GLEDITOR_RESOLVE(GetUniformLocation);
  GLEDITOR_RESOLVE(Uniform1i);
  GLEDITOR_RESOLVE(UniformMatrix4fv);
  GLEDITOR_RESOLVE(GetUniformBlockIndex);
  GLEDITOR_RESOLVE(UniformBlockBinding);

  GLEDITOR_RESOLVE(GenFramebuffers);
  GLEDITOR_RESOLVE(DeleteFramebuffers);
  GLEDITOR_RESOLVE(BindFramebuffer);
  GLEDITOR_RESOLVE(GenRenderbuffers);
  GLEDITOR_RESOLVE(DeleteRenderbuffers);
  GLEDITOR_RESOLVE(BindRenderbuffer);
  GLEDITOR_RESOLVE(RenderbufferStorage);
  GLEDITOR_RESOLVE(FramebufferRenderbuffer);
  GLEDITOR_RESOLVE(CheckFramebufferStatus);
  GLEDITOR_RESOLVE(DrawBuffers);
  GLEDITOR_RESOLVE(BlitFramebuffer);
  GLEDITOR_RESOLVE(ReadBuffer);
  GLEDITOR_RESOLVE(ReadPixels);
  GLEDITOR_RESOLVE(ClearBufferuiv);

  GLEDITOR_RESOLVE(Enable);
  GLEDITOR_RESOLVE(Disable);
  GLEDITOR_RESOLVE(Viewport);
  GLEDITOR_RESOLVE(Clear);
  GLEDITOR_RESOLVE(ClearColor);
  GLEDITOR_RESOLVE(FrontFace);
  GLEDITOR_RESOLVE(GetIntegerv);
  GLEDITOR_RESOLVE(GetError);
  GLEDITOR_RESOLVE(GetString);
  GLEDITOR_RESOLVE(DrawArraysInstanced);

#undef GLEDITOR_RESOLVE
}

} // namespace render::gl
// vi: set sw=2 sts=2 ts=2 et:
