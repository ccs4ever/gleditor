/**
 * @file gl_api.cpp
 * @brief Resolution of the GL/GLES entry point table through SDL.
 */
#include <gleditor/render/gl/gl_api.hpp> // IWYU pragma: associated

#include <gleditor/sdl_compat.hpp>

#include <cstring>
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
/// Resolve an entry point that may legitimately be absent.
template <typename Fn> bool resolveOptional(Fn &slot, const char *name) {
  auto *addr = SDL_GL_GetProcAddress(name);
  slot       = reinterpret_cast<Fn>(addr);
  return nullptr != addr;
}

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
  GLEDITOR_RESOLVE(MapBufferRange);
  GLEDITOR_RESOLVE(UnmapBuffer);

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
  GLEDITOR_RESOLVE(GenerateMipmap);
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
  GLEDITOR_RESOLVE(Uniform1f);
  GLEDITOR_RESOLVE(Uniform1ui);
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
  GLEDITOR_RESOLVE(DepthFunc);
  GLEDITOR_RESOLVE(Disable);
  GLEDITOR_RESOLVE(BlendFunc);
  GLEDITOR_RESOLVE(Viewport);
  GLEDITOR_RESOLVE(Clear);
  GLEDITOR_RESOLVE(ClearColor);
  GLEDITOR_RESOLVE(FrontFace);
  GLEDITOR_RESOLVE(GetIntegerv);
  GLEDITOR_RESOLVE(GetError);
  GLEDITOR_RESOLVE(GetString);
  GLEDITOR_RESOLVE(GetStringi);
  GLEDITOR_RESOLVE(DrawArraysInstanced);

  GLEDITOR_RESOLVE(FenceSync);
  GLEDITOR_RESOLVE(ClientWaitSync);
  GLEDITOR_RESOLVE(DeleteSync);

#undef GLEDITOR_RESOLVE
}

bool GLApi::hasExtension(const char *name) const {
  if (nullptr == GetStringi || nullptr == GetIntegerv) {
    return false;
  }
  // A core profile returns nothing for glGetString(GL_EXTENSIONS); the indexed
  // query is the only way to read the list.
  GLint count = 0;
  GetIntegerv(GL_NUM_EXTENSIONS, &count);
  for (GLint i = 0; i < count; i++) {
    const auto *entry = GetStringi(GL_EXTENSIONS, static_cast<GLuint>(i));
    if (nullptr != entry &&
        0 == std::strcmp(reinterpret_cast<const char *>(entry), name)) {
      return true;
    }
  }
  return false;
}

bool GLApi::loadDebugOutput() {
  if (!hasExtension("GL_KHR_debug")) {
    return false;
  }
  // OpenGL ES exposes the extension under its suffixed names; desktop OpenGL
  // promoted them to core spellings. Either may be the one this context has.
  const bool haveCallback =
      resolveOptional(DebugMessageCallback, "glDebugMessageCallback") ||
      resolveOptional(DebugMessageCallback, "glDebugMessageCallbackKHR");
  const bool haveControl =
      resolveOptional(DebugMessageControl, "glDebugMessageControl") ||
      resolveOptional(DebugMessageControl, "glDebugMessageControlKHR");
  return haveCallback && haveControl;
}

} // namespace render::gl
