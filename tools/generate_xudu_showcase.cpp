#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

#include <gleditor/beams.hpp>
#include <gleditor/color.hpp>
#include <gleditor/doc.hpp>
#include <gleditor/render/device.hpp>
#include <gleditor/render/types.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/sdl_wrap.hpp>
#include <gleditor/text/font.hpp>
#include <gleditor/text_source.hpp>

using namespace gleditor;

namespace {

void writePPM(const render::FrameImage &image, const std::string &path) {
  std::ofstream out(path, std::ios::binary);
  out << "P6\n" << image.width << " " << image.height << "\n255\n";
  const std::size_t stride = static_cast<std::size_t>(image.width) * 4;
  std::vector<char> row(static_cast<std::size_t>(image.width) * 3);

  for (int y = 0; y < image.height; ++y) {
    const auto *src = reinterpret_cast<const uint8_t *>(
        image.pixels.data() + static_cast<std::size_t>(y) * stride);
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

void renderScene(
    const std::string &outPpm,
    const std::vector<std::pair<std::string, glm::vec3>> &docs,
    const std::vector<std::tuple<glm::vec3, glm::vec3, float, uint32_t>>
        &beamsData,
    const glm::vec3 &cameraPos, const float fov) {
  const int width  = 1600;
  const int height = 900;

  render::configureBackendWindowAttributes(render::Backend::OpenGL);
  AutoSDL sdl(SDL_INIT_VIDEO);
  AutoSDLWindow window("Showcase", width, height,
                       render::backendWindowFlags(render::Backend::OpenGL) |
                           SDL_WINDOW_HIDDEN);

  auto device = render::createDevice(render::Backend::OpenGL);
  device->initialize(window);

  RenderState state;
  state.device            = device.get();
  state.view.screenWidth  = width;
  state.view.screenHeight = height;
  state.view.pos          = cameraPos;
  state.view.target       = glm::vec3(cameraPos.x, cameraPos.y, 0.0F);
  state.view.up           = glm::vec3(0.0F, 1.0F, 0.0F);
  state.fov               = fov;

  // Create Docs
  for (const auto &[text, pos] : docs) {
    StaticTextSource src(text, "Doc");
    auto d = Doc::create(nullptr, device.get(),
                         glm::translate(glm::mat4(1.0F), pos), src);
    d->makePages();
    state.docs.push_back(d);
  }

  // Create Beams
  auto beams = std::make_unique<Beams>(device.get());
  beams->createPipeline("assets/shaders", "assets/shaders/vulkan", true);

  for (const auto &[p1, p2, w, col] : beamsData) {
    beams->add(p1, p2, w, col, 1U, 0.0F, 1.0F);
  }
  beams->commit();

  // Matrices
  const float aspect = static_cast<float>(width) / static_cast<float>(height);
  const glm::mat4 proj =
      glm::perspective(glm::radians(fov), aspect, 0.1F, 10000.0F);
  const glm::mat4 view =
      glm::lookAt(cameraPos, glm::vec3(cameraPos.x, cameraPos.y, 0.0F),
                  glm::vec3(0.0F, 1.0F, 0.0F));
  const glm::mat4 viewProj = proj * view;

  // Frame context and draw
  device->beginFrame(state);

  std::vector<render::GlyphBatch> batches;
  Doc::DrawBudget budget;
  Doc::DrawStats stats;
  for (const auto &doc : state.docs) {
    doc->collect(batches, viewProj, budget, stats);
  }

  device->drawTextBatches(batches);
  beams->draw(state, viewProj, 1.0F, render::packTagIdentity(0, 0, 0));
  device->endFrame(state);

  auto img = device->captureFrame();
  writePPM(img, outPpm);
  std::cout << "Rendered scene: " << outPpm << " (" << img.width << "x"
            << img.height << ")\n";
}

} // namespace

int main() {
  std::filesystem::create_directories("scratch");

  // Scenario 1: Sparse / Focused Transclusion Configuration
  {
    std::string t1 =
        "AS WE MAY THINK\nVannevar Bush - July 1945\n\n"
        "Consider a future device for individual use, which is a sort of\n"
        "mechanized private file and library. It needs a name, and to coin\n"
        "one at random, 'memex' will do. A memex is a device in which an\n"
        "individual stores all his books, records, and communications,\n"
        "and which is mechanized so that it may be consulted with exceeding\n"
        "speed and flexibility. It is an enlarged intimate supplement to\n"
        "his memory.\n\n"
        "The process of tying two items together is the important thing.\n"
        "When the user is building a trail, he names it, inserts the\n"
        "nickname in his code book, and taps it out on his keyboard.";

    std::string t2 =
        "PROJECT XANADU AND DEEP TRANSCLUSION\nTed Nelson - 1965\n\n"
        "In 1945, Vannevar Bush envisioned the memex:\n\n"
        "\"A memex is a device in which an individual stores all his\n"
        "books, records, and communications, and which is mechanized\n"
        "so that it may be consulted with exceeding speed and "
        "flexibility.\"\n\n"
        "In Xanadu, this quotation is not dead copied characters, but a live\n"
        "optical transclusion conduit anchored across the 3D docuverse.";

    std::vector<std::pair<std::string, glm::vec3>> docs = {
        {t1, glm::vec3(0.0F, 0.0F, 0.0F)},
        {t2, glm::vec3(45.0F, 0.0F, 0.0F)},
    };

    // Emerald / Mint Quotation beam bridging the memex definition
    std::vector<std::tuple<glm::vec3, glm::vec3, float, uint32_t>> beams = {
        {glm::vec3(22.0F, -6.2F, 0.1F), glm::vec3(45.0F, -7.0F, 0.1F), 1.6F,
         0x7FE0A8F0U},
        {glm::vec3(22.0F, -8.2F, 0.1F), glm::vec3(45.0F, -9.0F, 0.1F), 1.4F,
         0x7FE0A8D0U},
    };

    renderScene("scratch/showcase_sparse_transclusion.ppm", docs, beams,
                glm::vec3(34.0F, -11.0F, 78.0F), 22.0F);
  }

  // Scenario 2: Multi-Document Corpus & 3-Way Tension Cosmos (Foreground flying
  // page + Background source)
  {
    std::string t1 = "MEMEX ARCHITECTURE (1945)\n"
                     "Associative trails connecting records\n"
                     "through mechanical microfilm projections.\n\n"
                     "Trail building forms enduring linkages\n"
                     "across all scientific disciplines.";

    std::string t2 = "LITERARY MACHINES (1965)\n"
                     "Intertwingled hypermedia docuverse.\n"
                     "No artificial walls or windows.\n\n"
                     "Every quote is a windowed portal\n"
                     "retaining dynamic origin provenance.";

    std::string t3 = "CRITIQUE & SYNTHESIS (2026)\n"
                     "The 3-way tension engine dynamically\n"
                     "aligns linked passages collinearly while\n"
                     "faint elastic tethers connect back to\n"
                     "background parent documents in depth.";

    std::vector<std::pair<std::string, glm::vec3>> docs = {
        {t1, glm::vec3(0.0F, 0.0F, 0.0F)},
        {t2, glm::vec3(38.0F, 0.0F, 0.0F)},
        {t3, glm::vec3(76.0F, -2.0F, 0.0F)},
    };

    std::vector<std::tuple<glm::vec3, glm::vec3, float, uint32_t>> beams = {
        // Bush -> Nelson Quotation (Mint)
        {glm::vec3(20.0F, -4.8F, 0.1F), glm::vec3(38.0F, -5.2F, 0.1F), 1.4F,
         0x7FE0A8F0U},
        // Nelson -> Critique Comment (Azure)
        {glm::vec3(58.0F, -6.2F, 0.1F), glm::vec3(76.0F, -5.2F, 0.1F), 1.4F,
         0x7FB2FFF0U},
        // Direct overarching bypass beam (Gold)
        {glm::vec3(20.0F, -9.5F, 0.1F), glm::vec3(76.0F, -10.5F, 0.1F), 1.2F,
         0xFFC46BD0U},
    };

    renderScene("scratch/showcase_multidoc_triad.ppm", docs, beams,
                glm::vec3(48.0F, -9.0F, 98.0F), 24.0F);
  }

  // Scenario 3: High-Density Hypertext Matrix with 5 Colorful Link Ribbons
  {
    std::string t1 =
        "HYPERTEXT CORE PRINCIPLES\nTed Nelson - Literary Machines\n\n"
        "[1] Non-sequential multi-path reading structures.\n\n"
        "[2] Two-way visible links preserving unbroken context.\n\n"
        "[3] Deep transclusion replacing duplicate copies.\n\n"
        "[4] Micro-version history preserving every change.\n\n"
        "[5] Continuous 3D tension physics for link layout.";

    std::string t2 =
        "ARCHITECTURAL COMMENTARY & ANALYSIS\nHypermedia Design Studies\n\n"
        "Analysis 1: Branching paths prevent linear cognitive bias.\n\n"
        "Analysis 2: Bidirectional links eliminate broken 404 paths.\n\n"
        "Analysis 3: Transclusion enforces automated royalty micropayments.\n\n"
        "Analysis 4: Micro-version trees enable hypertime scrubbing.\n\n"
        "Analysis 5: Spring tension balances readability and ribbon beauty.";

    std::vector<std::pair<std::string, glm::vec3>> docs = {
        {t1, glm::vec3(0.0F, 0.0F, 0.0F)},
        {t2, glm::vec3(45.0F, 0.0F, 0.0F)},
    };

    std::vector<std::tuple<glm::vec3, glm::vec3, float, uint32_t>> beams = {
        // [1] Comment (Azure / Blue)
        {glm::vec3(24.0F, -5.2F, 0.1F), glm::vec3(45.0F, -4.9F, 0.1F), 1.5F,
         0x7FB2FFF0U},
        // [2] Disagreement / Correction (Coral / Red)
        {glm::vec3(24.0F, -9.5F, 0.1F), glm::vec3(45.0F, -9.2F, 0.1F), 1.6F,
         0xFF7A6BF0U},
        // [3] Quotation / Transclusion (Mint / Emerald)
        {glm::vec3(24.0F, -13.8F, 0.1F), glm::vec3(45.0F, -13.5F, 0.1F), 1.7F,
         0x7FE0A8F0U},
        // [4] Illustration / Commentary (Amber / Gold)
        {glm::vec3(24.0F, -18.0F, 0.1F), glm::vec3(45.0F, -17.8F, 0.1F), 1.5F,
         0xFFC46BF0U},
        // [5] Authorship / Tension Physics (Violet / Purple)
        {glm::vec3(24.0F, -22.2F, 0.1F), glm::vec3(45.0F, -22.0F, 0.1F), 1.6F,
         0xB98CFFF0U},
    };

    renderScene("scratch/showcase_dense_matrix.ppm", docs, beams,
                glm::vec3(34.0F, -14.0F, 82.0F), 24.0F);
  }

  return 0;
}
