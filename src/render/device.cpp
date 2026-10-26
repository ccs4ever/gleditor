/**
 * @file device.cpp
 * @brief Backend naming, window configuration and device construction.
 */
#include <gleditor/render/device.hpp> // IWYU pragma: associated

#include <gleditor/sdl_compat.hpp>

#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

#include <gleditor/render/gl/device_gl.hpp>
#ifdef GLEDITOR_ENABLE_VULKAN
#include <gleditor/render/vulkan/device_vk.hpp>
#endif

namespace render {

void RenderDevice::beginRegions(const int width, const int height) {
  regions_.clear();
  currentRegion_.reset();
  fullRegion_    = {.viewWidth  = width,
                    .viewHeight = height,
                    .clipWidth  = width,
                    .clipHeight = height};
  regionsActive_ = true;
}

void RenderDevice::endRegions() {
  regionsActive_ = false;
  regions_.clear();
  currentRegion_.reset();
}

RegionId RenderDevice::defineRegion(const RenderRegion &region) {
  if (!regionsActive_) throw std::logic_error("defineRegion outside a frame");
  if (region.viewWidth <= 0 || region.viewHeight <= 0 || region.clipWidth < 0 ||
      region.clipHeight < 0 || !std::isfinite(region.nearDepth) ||
      !std::isfinite(region.farDepth) || region.nearDepth < 0.F ||
      region.farDepth > 1.F || region.nearDepth > region.farDepth)
    throw std::invalid_argument("Invalid render region");
  if (regions_.size() > std::numeric_limits<RegionId>::max())
    throw std::length_error("Too many render regions");
  auto clipped         = region;
  const auto clampEdge = [](const std::int64_t value, const int bound) {
    return static_cast<int>(std::clamp<std::int64_t>(value, 0, bound));
  };
  clipped.clipLeft   = clampEdge(region.clipLeft, fullRegion_.viewWidth);
  clipped.clipBottom = clampEdge(region.clipBottom, fullRegion_.viewHeight);
  clipped.clipWidth =
      clampEdge(static_cast<std::int64_t>(region.clipLeft) + region.clipWidth,
                fullRegion_.viewWidth) -
      clipped.clipLeft;
  clipped.clipHeight = clampEdge(static_cast<std::int64_t>(region.clipBottom) +
                                     region.clipHeight,
                                 fullRegion_.viewHeight) -
                       clipped.clipBottom;
  const auto id = static_cast<RegionId>(regions_.size());
  regions_.push_back(clipped);
  return id;
}

const RenderRegion &
RenderDevice::regionOf(const std::optional<RegionId> id) const {
  if (!id) return fullRegion_;
  if (!regionsActive_ || *id >= regions_.size())
    throw std::invalid_argument("Render region is not valid in this frame");
  return regions_[*id];
}

RenderDevice *RenderDevice::setRegion(const std::optional<RegionId> region) {
  (void)regionOf(region);
  currentRegion_ = region;
  return this;
}

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
  // OpenGL/OpenGLES's shared `return true;` happens to match Vulkan's own
  // when GLEDITOR_ENABLE_VULKAN is defined; the two can't be merged since
  // Vulkan's body is itself conditional on that #ifdef.
  case Backend::OpenGL: // NOLINT(bugprone-branch-clone)
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
        "The Vulkan backend was not compiled in. Rebuild without "
        "GLEDITOR_DISABLE_VULKAN=1.");
#endif
  }
  throw std::invalid_argument("createDevice: unknown backend");
}

} // namespace render
