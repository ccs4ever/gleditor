/**
 * @file backend.cpp
 * @brief Backend naming.
 *
 * Kept apart from the device factory so that build-time tooling can reuse it
 * without linking -- or even being able to compile against -- any backend.
 */
#include <gleditor/render/types.hpp> // IWYU pragma: associated

#include <format>
#include <stdexcept>
#include <string>

namespace render {

Backend backendFromName(const std::string &name) {
  if ("opengl" == name || "gl" == name) {
    return Backend::OpenGL;
  }
  if ("opengles" == name || "gles" == name || "es" == name) {
    return Backend::OpenGLES;
  }
  if ("vulkan" == name || "vk" == name) {
    return Backend::Vulkan;
  }
  throw std::invalid_argument(
      std::format("Unknown render backend: {}. Expected one of "
                  "opengl, opengles, vulkan.",
                  name));
}

std::string backendName(const Backend backend) {
  switch (backend) {
  case Backend::OpenGL:
    return "opengl";
  case Backend::OpenGLES:
    return "opengles";
  case Backend::Vulkan:
    return "vulkan";
  }
  return "unknown";
}

} // namespace render
