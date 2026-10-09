/**
 * @file headless_device.hpp
 * @brief A real device on a hidden window, for tests that read back pixels.
 *
 * A mock device says what was asked of a backend; only a real one says what
 * the backend drew. Tests of depth and blending state are about the second,
 * so they open each compiled-in backend here and skip the ones this machine
 * cannot open: SDL2's offscreen driver makes no Vulkan window, which is the
 * usual reason, and a test run under xvfb-run gets Vulkan back.
 */
#ifndef GLEDITOR_TESTS_LIB_HEADLESS_DEVICE_H
#define GLEDITOR_TESTS_LIB_HEADLESS_DEVICE_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <memory>
#include <string>
#include <vector>

#include <gleditor/doc.hpp>
#include <gleditor/paths.hpp>
#include <gleditor/render/device.hpp>
#include <gleditor/render/shader_source.hpp>
#include <gleditor/render/types.hpp>
#include <gleditor/sdl_wrap.hpp>

namespace headless {

/// Every backend this build has, in the order the tests report them.
inline std::vector<render::Backend> compiledBackends() {
  std::vector<render::Backend> out;
  for (const auto backend : {render::Backend::OpenGL, render::Backend::OpenGLES,
                             render::Backend::Vulkan}) {
    if (render::backendCompiledIn(backend)) {
      out.push_back(backend);
    }
  }
  return out;
}

/// An initialised device and what keeps it alive, torn down in order.
struct Device {
  std::unique_ptr<AutoSDL> sdl;
  std::unique_ptr<AutoSDLWindow> window;
  std::unique_ptr<render::RenderDevice> device;

  Device()                          = default;
  Device(const Device &)            = delete;
  Device &operator=(const Device &) = delete;
  Device(Device &&)                 = default;
  Device &operator=(Device &&)      = default;
  ~Device() {
    if (device) {
      device->waitIdle();
      device->shutdown();
    }
  }
};

/**
 * @brief Open @p backend on a hidden window of the given size.
 * @return The device, or why this machine could not open it.
 */
inline std::expected<std::unique_ptr<Device>, std::string>
open(const render::Backend backend, const int width, const int height) {
  auto out = std::make_unique<Device>();
  try {
    out->sdl = std::make_unique<AutoSDL>(SDL_INIT_VIDEO);
    render::configureBackendWindowAttributes(backend);
    out->window = std::make_unique<AutoSDLWindow>(
        "headless test", width, height,
        render::backendWindowFlags(backend) | SDL_WINDOW_HIDDEN);
    out->device = render::createDevice(backend);
    out->device->initialize(*out->window);
  } catch (const std::exception &error) {
    out->device.reset();
    return std::unexpected(render::backendName(backend) +
                           " unavailable: " + error.what());
  }
  out->device->setStrictDiagnostics(true);
  // Vulkan hands its image back only by presenting, so it presents into
  // whatever display it was given; the GL family captures without.
  if (render::Backend::Vulkan != backend) {
    out->device->setPresentEnabled(false);
  }
  return out;
}

/// The glyph pipeline as the renderer describes it, with the depth state
/// given.
inline render::PipelineDesc glyphPipeline(const bool depthWrite = true) {
  const auto shaders = gleditor::assetPath("shaders");
  render::PipelineDesc desc;
  desc.name           = depthWrite ? "glyph" : "glyph-translucent";
  desc.vertexSource   = render::readShaderBody(shaders + "/glyph.vert.glsl");
  desc.fragmentSource = render::readShaderBody(shaders + "/glyph.frag.glsl");
  desc.spirvDir       = shaders + "/vulkan";
  desc.layout         = Doc::vertexLayout();
  desc.depthWrite     = depthWrite;
  return desc;
}

/// RGB at (x, y), with y counted up from the bottom as projection does.
inline std::array<int, 3> pixel(const render::FrameImage &image, const int x,
                                const int yUp) {
  const auto row = static_cast<std::size_t>(image.height - 1 - yUp);
  const auto at  = ((row * static_cast<std::size_t>(image.width)) +
                   static_cast<std::size_t>(x)) *
                  4U;
  return {image.rgba.at(at), image.rgba.at(at + 1), image.rgba.at(at + 2)};
}

} // namespace headless

#endif // GLEDITOR_TESTS_LIB_HEADLESS_DEVICE_H
