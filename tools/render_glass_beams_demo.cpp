#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <vector>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

#include <gleditor/beams.hpp>
#include <gleditor/color.hpp>
#include <gleditor/doc.hpp>
#include <gleditor/render/device.hpp>
#include <gleditor/render/gl/device_gl.hpp>
#include <gleditor/render/types.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/renderer.hpp>
#include <gleditor/text_source.hpp>

namespace fs = std::filesystem;

namespace {

void writePPM(const render::FrameImage &image, const std::string &path) {
  std::ofstream out(path, std::ios::binary);
  out << "P6\n" << image.width << " " << image.height << "\n255\n";
  const std::size_t stride = static_cast<std::size_t>(image.width) * 4;
  std::vector<char> row(static_cast<std::size_t>(image.width) * 3);

  for (int y = 0; y < image.height; ++y) {
    const auto *src = reinterpret_cast<const uint8_t *>(
        image.rgba.data() + static_cast<std::size_t>(y) * stride);
    for (int x = 0; x < image.width; ++x) {
      row[static_cast<std::size_t>(x) * 3 + 0] =
          static_cast<char>(src[x * 4 + 0]);
      row[static_cast<std::size_t>(x) * 3 + 1] =
          static_cast<char>(src[x * 4 + 1]);
      row[static_cast<std::size_t>(x) * 3 + 2] =
          static_cast<char>(src[x * 4 + 2]);
    }
    out.write(row.data(), static_cast<std::streamsize>(row.size()));
  }
}

} // namespace

int main(int argc, char **argv) {
  const std::string outPath =
      (argc > 1) ? argv[1] : "/tmp/glass_beams_demo.ppm";

  auto state          = std::make_shared<AppState>();
  state->windowWidth  = 1920;
  state->windowHeight = 1080;
  state->noPresent    = true;
  state->profiling    = false;
  state->fov          = 25.0F;
  state->view.pos     = glm::vec3(45.0F, -20.0F, 120.0F);

  auto renderer =
      std::make_shared<Renderer>(state, render::Backend::OpenGL, 1920, 1080);
  renderer->init();

  // Create documents
  gleditor::StaticTextSource src1(
      "Project Xanadu: Deep Multi-Modal Hypermedia Connections.\n"
      "This passage establishes a direct bidirectional link\n"
      "anchored to primary text and streaming media.\n"
      "Exploring authentic transclusion mechanisms...",
      "Source Document");
  auto doc1 = Doc::create(
      renderer, renderer->device(),
      glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, 0.0F, 0.0F)), src1);

  gleditor::StaticTextSource src2(
      "Commentary & Transcluded Quoted Text:\n"
      "\"Project Xanadu: Deep Multi-Modal Hypermedia Connections\"\n"
      "The connection is rendered as a continuous optical glass ribbon\n"
      "carrying traveling photonic energy pulses.",
      "Target Document");
  auto doc2 = Doc::create(
      renderer, renderer->device(),
      glm::translate(glm::mat4(1.0F), glm::vec3(60.0F, -5.0F, 0.0F)), src2);

  renderer->runWithState([&](RenderState &rState) {
    rState.docs.push_back(doc1);
    rState.docs.push_back(doc2);
  });

  // Setup Beams
  auto beams = std::make_unique<gleditor::Beams>(renderer->device());
  beams->createPipeline("assets/shaders", "assets/shaders/vulkan", true);

  // Add multiple colored glass link ribbons with varying link types:
  // Quotation (Emerald/Mint: 0x7FE0A8E0)
  beams->add(glm::vec3(26.0F, -10.0F, 0.1F), glm::vec3(60.0F, -15.0F, 0.1F),
             1.2F, 0x7FE0A8E0U, 1U, 0.0F, 1.0F);
  // Comment (Azure/Cyan: 0x7FB2FFE0)
  beams->add(glm::vec3(26.0F, -18.0F, 0.1F), glm::vec3(60.0F, -24.0F, 0.1F),
             1.1F, 0x7FB2FFE0U, 2U, 0.0F, 1.0F);
  // Illustration (Amber/Gold: 0xFFC46BE0)
  beams->add(glm::vec3(26.0F, -26.0F, 0.1F), glm::vec3(60.0F, -33.0F, 0.1F),
             1.3F, 0xFFC46BE0U, 3U, 0.0F, 1.0F);
  // Authorship (Violet: 0xB98CFFE0)
  beams->add(glm::vec3(26.0F, -34.0F, 0.1F), glm::vec3(60.0F, -42.0F, 0.1F),
             1.1F, 0xB98CFFE0U, 4U, 0.0F, 1.0F);
  beams->commit();

  struct BeamContributor : public FrameContributor {
    gleditor::Beams *b;
    explicit BeamContributor(gleditor::Beams *aB) : b(aB) {}
    void deviceReady(render::RenderDevice &,
                     const render::PipelineDesc &) override {}
    void drawFrame(FrameContext &ctx) override {
      if (b) {
        b->draw(ctx.state, ctx.viewProjection, 1.0F,
                render::packTagIdentity(0, 0, 0));
      }
    }
  };

  BeamContributor contrib(beams.get());
  renderer->addFrameContributor(&contrib);

  // Render frames until settled
  for (int f = 0; f < 30; ++f) {
    renderer->renderFrame();
  }

  auto img = renderer->device()->captureColorTarget();
  writePPM(img, outPath);
  std::cout << "Successfully rendered glass beams demo to " << outPath << " ("
            << img.width << "x" << img.height << ")\n";
  return 0;
}
