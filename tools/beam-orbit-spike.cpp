// Spike R3 (design/view-system-implementation-plan.md): draw one beam along
// each world axis, orbit the camera round it, capture every frame and count
// the beam's pixels. Throwaway; L10's world-scene-probe replaces it.
//
//   beam-orbit-spike <opengl|opengles|vulkan> <outdir>
//
// Prints one line per capture: backend axis azimuth elevation pixels path.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <SDL.h>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/trigonometric.hpp>

#include <gleditor/beams.hpp>
#include <gleditor/render/device.hpp>
#include <gleditor/render/types.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/sdl_wrap.hpp>

namespace {

constexpr int width  = 640;
constexpr int height = 480;

void writePPM(const render::FrameImage &image, const std::string &path) {
  std::ofstream out(path, std::ios::binary);
  out << "P6\n" << image.width << " " << image.height << "\n255\n";
  for (std::size_t i = 0; i < image.rgba.size(); i += 4) {
    out.write(reinterpret_cast<const char *>(&image.rgba[i]), 3);
  }
}

// Red beam on whatever the clear colour is: a pixel counts when red clearly
// dominates.
int beamPixels(const render::FrameImage &image) {
  int count = 0;
  for (std::size_t i = 0; i < image.rgba.size(); i += 4) {
    const int red   = image.rgba[i];
    const int green = image.rgba[i + 1];
    const int blue  = image.rgba[i + 2];
    if (red - std::max(green, blue) > 40) {
      count++;
    }
  }
  return count;
}

struct Camera {
  float azimuth;   // degrees about +Y, from +Z towards +X
  float elevation; // degrees above the XZ plane
};

} // namespace

int main(int argc, char **argv) {
  if (argc < 3) {
    std::cerr << "usage: beam-orbit-spike <backend> <outdir>\n";
    return 2;
  }
  const std::string name   = argv[1];
  const std::string outDir = argv[2];
  std::filesystem::create_directories(outDir);
  const auto backend = name == "vulkan"     ? render::Backend::Vulkan
                       : name == "opengles" ? render::Backend::OpenGLES
                                            : render::Backend::OpenGL;

  const AutoSDL sdl(SDL_INIT_VIDEO);
  render::configureBackendWindowAttributes(backend);
  AutoSDLWindow window("beam-orbit-spike", width, height,
                       render::backendWindowFlags(backend));
  auto device = render::createDevice(backend);
  device->initialize(window);
  // Vulkan returns its swapchain image only by presenting, so it presents
  // (into Xvfb); the GL family captures without.
  if (backend != render::Backend::Vulkan) {
    device->setPresentEnabled(false);
  }
  RenderState state(device.get());

  const std::vector<std::pair<std::string, glm::vec3>> axes{
      {"x", {1.0F, 0.0F, 0.0F}},
      {"y", {0.0F, 1.0F, 0.0F}},
      {"z", {0.0F, 0.0F, 1.0F}}};
  const std::vector<Camera> cameras{
      {0, 0},  {20, 10}, {30, 0},  {45, 30}, {60, 0}, {90, 0}, {135, 20},
      {0, 60}, {0, 85},  {180, 0}, {3, 0},   {10, 0}, {80, 0}, {87, 0}};

  constexpr float halfLength = 30.0F;
  constexpr float radius     = 150.0F;
  constexpr float beamWidth  = 3.0F;
  const glm::mat4 projection = glm::perspective(
      glm::radians(45.0F), static_cast<float>(width) / height, 1.0F, 2000.0F);

  for (const auto &[axisName, axis] : axes) {
    gleditor::Beams beams(device.get());
    beams.createPipeline("assets/shaders", "assets/shaders/vulkan", true);
    beams.add(-halfLength * axis, halfLength * axis, beamWidth, 0xFF2020FFU,
              1U);
    beams.commit();

    for (const auto &camera : cameras) {
      const float az = glm::radians(camera.azimuth);
      const float el = glm::radians(camera.elevation);
      const glm::vec3 eye{radius * std::cos(el) * std::sin(az),
                          radius * std::sin(el),
                          radius * std::cos(el) * std::cos(az)};
      const glm::mat4 view =
          glm::lookAt(eye, glm::vec3(0.0F), glm::vec3(0.0F, 1.0F, 0.0F));

      // Two frames: the first readback on some backends precedes the draw.
      render::FrameImage image;
      for (int frame = 0; frame < 2; frame++) {
        if (!device->beginFrame()) {
          std::cerr << "beginFrame failed\n";
          return 1;
        }
        beams.draw(state, projection * view, 1.0F, 0U);
        device->endFrame();
        image = device->captureColorTarget();
      }
      const auto path = std::format("{}/{}-{}-az{}-el{}.ppm", outDir, name,
                                    axisName, static_cast<int>(camera.azimuth),
                                    static_cast<int>(camera.elevation));
      writePPM(image, path);
      std::cout << std::format("{} {} {} {} {} {}\n", name, axisName,
                               camera.azimuth, camera.elevation,
                               beamPixels(image), path);
    }
  }
  device->waitIdle();
  return 0;
}
