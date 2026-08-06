/**
 * @file device.cpp
 * @brief Backend naming, window configuration and device construction.
 */
#include <gleditor/render/device.hpp> // IWYU pragma: associated

#include <gleditor/sdl_compat.hpp>

#include <memory>
#include <stdexcept>
#include <string>

#include <gleditor/render/gl/device_gl.hpp>
#ifdef GLEDITOR_ENABLE_VULKAN
#include <gleditor/render/vulkan/device_vk.hpp>
#endif

namespace render {

std::uint64_t backendWindowFlags(const Backend backend) {
  switch (backend) {
  case Backend::OpenGL:
  case Backend::OpenGLES:
    return SDL_WINDOW_OPENGL;
  case Backend::Vulkan:
    return SDL_WINDOW_VULKAN;
  }
  return 0;
}

void configureBackendWindowAttributes(const Backend backend) {
  switch (backend) {
  case Backend::OpenGL:
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                        SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    break;
  case Backend::OpenGLES:
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    break;
  case Backend::Vulkan:
    // Vulkan surfaces are created from the window directly; there are no GL
    // attributes to set.
    break;
  }

  if (Backend::Vulkan != backend) {
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  }
}

bool backendCompiledIn(const Backend backend) {
  switch (backend) {
  case Backend::OpenGL:
  case Backend::OpenGLES:
    return true;
  case Backend::Vulkan:
#ifdef GLEDITOR_ENABLE_VULKAN
    return true;
#else
    return false;
#endif
  }
  return false;
}

std::unique_ptr<RenderDevice> createDevice(const Backend backend) {
  switch (backend) {
  case Backend::OpenGL:
  case Backend::OpenGLES:
    return std::make_unique<gl::DeviceGL>(backend);
  case Backend::Vulkan:
#ifdef GLEDITOR_ENABLE_VULKAN
    return std::make_unique<vulkan::DeviceVK>();
#else
    throw std::runtime_error(
        "The Vulkan backend was not compiled in. Rebuild with "
        "GLEDITOR_ENABLE_VULKAN=1.");
#endif
  }
  throw std::invalid_argument("createDevice: unknown backend");
}

} // namespace render
// vi: set sw=2 sts=2 ts=2 et:
