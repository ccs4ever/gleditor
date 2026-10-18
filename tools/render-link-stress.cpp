/**
 * @file render-link-stress.cpp
 * @brief Native component evidence for placed document Link geometry.
 *
 * Uses real Session sources, LinkContext, LinkBeams and the Renderer loop.
 * Fixture cloning and direct scene placement are diagnostics, not UI journeys.
 */
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/sha.h>

#include <glm/ext/matrix_transform.hpp>

#include <gleditor/doc.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/renderer.hpp>
#include <gleditor/sdl_wrap.hpp>
#include <gleditor/text_source.hpp>

#include "common/ui/xanadoc/beams.hpp"
#include "common/ui/xanadoc/link_context.hpp"
#include "common/ui/xanadoc/link_panel_overlay.hpp"
#include "common/ui/xanadoc/session.hpp"

namespace {
namespace fs = std::filesystem;

struct Options {
  render::Backend backend{render::Backend::OpenGL};
  fs::path fixture{"build/ux-full-20261010/stress-dense/crisscross10"};
  fs::path out;
  std::string scene{"rotated"};
  std::string font{"Monospace 18"};
  int width{1600};
  int height{1000};
  float uiScale{1};
  float fontScale{1};
  float angle{25};
  float tilt{20};
  float depth{12};
  std::size_t selectedIndex{};
  bool dark{};
  bool unselected{};
};

float positiveNumber(const std::string &value) {
  std::size_t consumed{};
  const auto parsed = std::stof(value, &consumed);
  if (consumed != value.size() || !std::isfinite(parsed) || parsed <= 0)
    throw std::invalid_argument("expected a finite positive number: " + value);
  return parsed;
}

int drawableExtent(const std::string &value) {
  const auto parsed = positiveNumber(value);
  if (parsed > 8192 || std::floor(parsed) != parsed)
    throw std::invalid_argument(
        "drawable extent must be an integer from 1 to 8192");
  return static_cast<int>(parsed);
}

std::optional<Options> parse(const int argc, char **argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string_view arg{argv[index]};
    if (arg == "--help") {
      std::cout << "Usage: render-link-stress --out DIR [--fixture DIR] "
                   "[--backend opengl|opengles|vulkan] "
                   "[--case coplanar|rotated|depth|occluded|one-visible] "
                   "[--dark] [--unselected] [--selected-index N] "
                   "[--ui-scale N] [--font-scale N] [--font NAME] "
                   "[--width N] [--height N] [--angle N] [--tilt N] "
                   "[--depth N]\n"
                   "Headless component evidence only. Clones stores and "
                   "permascroll, places real pages and captures frame.ppm. "
                   "No authoring, input or complete navigation journey pass.\n";
      return std::nullopt;
    }
    if (arg == "--dark") {
      options.dark = true;
      continue;
    }
    if (arg == "--unselected") {
      options.unselected = true;
      continue;
    }
    if (index + 1 >= argc)
      throw std::invalid_argument("missing argument for " + std::string(arg));
    const std::string value{argv[++index]};
    if (arg == "--out")
      options.out = value;
    else if (arg == "--fixture")
      options.fixture = value;
    else if (arg == "--backend")
      options.backend = render::backendFromName(value);
    else if (arg == "--case")
      options.scene = value;
    else if (arg == "--font")
      options.font = value;
    else if (arg == "--ui-scale")
      options.uiScale = positiveNumber(value);
    else if (arg == "--font-scale")
      options.fontScale = positiveNumber(value);
    else if (arg == "--angle")
      options.angle = positiveNumber(value);
    else if (arg == "--tilt")
      options.tilt = positiveNumber(value);
    else if (arg == "--depth")
      options.depth = positiveNumber(value);
    else if (arg == "--width")
      options.width = drawableExtent(value);
    else if (arg == "--height")
      options.height = drawableExtent(value);
    else if (arg == "--selected-index") {
      std::size_t consumed{};
      options.selectedIndex = std::stoull(value, &consumed);
      if (consumed != value.size() || value.starts_with('-'))
        throw std::invalid_argument("invalid selected index");
    } else
      throw std::invalid_argument("unknown argument " + std::string(arg));
  }
  if (options.out.empty()) throw std::invalid_argument("--out is required");
  if (options.width < 1 || options.width > 8192 || options.height < 1 ||
      options.height > 8192)
    throw std::invalid_argument("drawable extent must be between 1 and 8192");
  const std::array scenes{"coplanar", "rotated", "depth", "occluded",
                          "one-visible"};
  if (std::ranges::find(scenes, options.scene) == scenes.end())
    throw std::invalid_argument("unknown scene " + options.scene);
  options.fixture = fs::canonical(options.fixture);
  options.out     = fs::absolute(options.out).lexically_normal();
  return options;
}

std::string sha256(const fs::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("cannot read " + path.string());
  const std::string bytes{std::istreambuf_iterator<char>(input), {}};
  if (input.bad()) throw std::runtime_error("read failed " + path.string());
  std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
  SHA256(reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size(),
         digest.data());
  std::ostringstream result;
  result << std::hex << std::setfill('0');
  for (const auto byte : digest)
    result << std::setw(2) << static_cast<int>(byte);
  return result.str();
}

std::vector<fs::path> sourceFiles() {
  return {"left/ops.nodes", "left/store.tables", "right/ops.nodes",
          "right/store.tables",
          "data/xuzz/permascroll/default/active.primedia"};
}

std::map<std::string, std::string> hashes(const fs::path &root) {
  std::map<std::string, std::string> result;
  for (const auto &path : sourceFiles())
    result.emplace(path.generic_string(), sha256(root / path));
  return result;
}

void cloneFixture(const Options &options, const fs::path &clone) {
  if (fs::exists(clone))
    throw std::runtime_error(
        "refusing to overwrite previous diagnostic fixture " + clone.string());
  for (const auto &path : sourceFiles()) {
    fs::create_directories((clone / path).parent_path());
    fs::copy_file(options.fixture / path, clone / path);
  }
  const auto set = [](const char *name, const fs::path &value) {
    if (setenv(name, value.string().c_str(), 1) != 0)
      throw std::runtime_error(std::string("cannot set ") + name);
  };
  set("XDG_DATA_HOME", clone / "data");
  set("XDG_CONFIG_HOME", clone / "config");
  set("XDG_CACHE_HOME", clone / "cache");
  // Explicit headless-driver overrides (including Xvfb) remain authoritative.
  setenv("SDL_VIDEODRIVER", "offscreen", 0);
  setenv("SDL_AUDIODRIVER", "dummy", 0);
  setenv("LIBGL_ALWAYS_SOFTWARE", "1", 0);
}

class DiagnosticPageSource : public gleditor::TextSource {
public:
  explicit DiagnosticPageSource(
      std::shared_ptr<xanadu::VersionTextSource> source)
      : source(std::move(source)) {}
  std::string text() const override { return source->text(); }
  std::string name() const override { return source->name(); }
  std::vector<std::uint32_t> forcedBreaks() const override {
    return source->forcedBreaks();
  }
  std::vector<gleditor::LayoutBox> layoutBoxes() const override {
    return source->layoutBoxes();
  }
  std::vector<gleditor::BlockStyleRange> blockStyles() const override {
    return source->blockStyles();
  }
  std::vector<gleditor::DecoratedRange> decoratedRanges() const override {
    return source->decoratedRanges();
  }
  std::shared_ptr<const render::PickSemanticTarget>
  pickSemanticTarget() const override {
    return source->pickSemanticTarget();
  }
  gleditor::PageSize pageSize() const override {
    return {.mode     = gleditor::PageSizing::Fixed,
            .widthPx  = 600,
            .heightPx = 520,
            .marginPx = 24};
  }

private:
  std::shared_ptr<xanadu::VersionTextSource> source;
};

glm::mat4 placement(const Options &options, const std::size_t index) {
  glm::vec3 origin{index == 0 ? -25.F : 25.F, 10.F, 0.F};
  if (index == 1 && (options.scene == "depth" || options.scene == "occluded"))
    origin.z = options.depth;
  if (index == 1 && options.scene == "one-visible") origin.y -= 70.F;
  auto matrix = glm::translate(glm::mat4(1), origin);
  if (options.scene == "rotated") {
    const auto direction = index == 0 ? -1.F : 1.F;
    matrix = glm::rotate(matrix, glm::radians(options.angle * direction),
                         glm::vec3(0, 0, 1));
    matrix = glm::rotate(matrix, glm::radians(options.tilt * direction),
                         glm::vec3(0, 1, 0));
  }
  return matrix;
}

void writeMatrix(std::ostream &out, const glm::mat4 &matrix) {
  out << '[';
  for (int column = 0; column < 4; ++column)
    for (int row = 0; row < 4; ++row) {
      if (column != 0 || row != 0) out << ',';
      out << matrix[column][row];
    }
  out << ']';
}

} // namespace

int main(const int argc, char **argv) {
  try {
    const auto parsed = parse(argc, argv);
    if (!parsed) return 0;
    const auto &options = *parsed;
    if (!render::backendCompiledIn(options.backend)) return 77;
    fs::create_directories(options.out);
    const auto before = hashes(options.fixture);
    const auto clone  = options.out / "fixture";
    cloneFixture(options, clone);
    if (hashes(clone) != before)
      throw std::runtime_error("fixture clone differs from source");

    xanadu::Session session(
        (clone / "left").string(),
        xanadu::PermascrollRegistry::instance().defaultUser());
    const auto rightStore =
        session.loadAuxiliaryStore((clone / "right").string());
    auto state               = std::make_shared<AppState>();
    state->defaultFontName   = options.font;
    state->uiScale           = options.uiScale;
    state->fontScale         = options.fontScale;
    state->view.screenWidth  = options.width;
    state->view.screenHeight = options.height;
    state->view.pos          = glm::vec3(0, -6, 100);
    state->view.fov          = 35;
    state->noPresent         = options.backend != render::Backend::Vulkan;
    state->strictDiagnostics = true;
    state->profiling         = true;
    state->benchmarkFrames   = 8;
    state->coarseBelow       = 0;
    state->screenshotPath    = (options.out / "frame.ppm").string();
    if (options.dark)
      state->documentPalette = std::make_shared<const DocumentPalette>(
          DocumentPalette{.paper = 0x181C24FFU, .ink = 0xE8EDF4FFU});
    auto renderer = Renderer::create(state, options.backend);
    xanadu::LinkContext context(session);
    xanadu::LinkBeams links(session, renderer);
    links.setSworph(false);
    links.setLinkContext(&context);
    xanadu::LinkPanelOverlay panel(context, session);
    renderer->addSpanDecorator(&session);
    renderer->addSpanDecorator(&panel);
    renderer->addFrameContributor(&links);
    renderer->addFrameContributor(&panel);
    renderer->addPickObserver(&links);
    renderer->addPickObserver(&panel);
    renderer->runWithState([&](RenderState &renderState) {
      const std::array storeIndices{std::size_t{0}, rightStore};
      for (std::size_t index = 0; index < storeIndices.size(); ++index) {
        const auto storeIndex = storeIndices[index];
        const auto version = session.store(storeIndex).primaryCurrentVersion();
        session.viewOpened(version, storeIndex);
        const DiagnosticPageSource source(
            session.sourceFor(version, storeIndex));
        auto doc = Doc::create(renderer, renderState.device,
                               placement(options, index), source);
        doc->setDocIndex(static_cast<std::uint32_t>(index));
        doc->makePages();
        renderState.pickTargets.push_back(source.pickSemanticTarget());
        renderState.docs.push_back(std::move(doc));
      }
      if (options.scene == "occluded") {
        const gleditor::MemoryTextSource source(
            "Foreground document\nUnrelated paper must occlude the remote "
            "route.\n",
            "Diagnostic occluder");
        auto matrix =
            glm::translate(glm::mat4(1), glm::vec3(0, 10, options.depth + 8));
        matrix   = glm::scale(matrix, glm::vec3(.35F, .35F, 1));
        auto doc = Doc::create(renderer, renderState.device, matrix, source);
        doc->setDocIndex(2);
        doc->makePages();
        renderState.docs.push_back(std::move(doc));
        renderState.pickTargets.push_back(nullptr);
      }
      std::vector<zigzag::CellRef> candidates;
      for (const auto &[id, link] : session.store().links())
        candidates.push_back(id);
      if (candidates.empty()) throw std::runtime_error("fixture has no Links");
      if (options.selectedIndex >= candidates.size())
        throw std::invalid_argument(
            "selected index exceeds fixture Link count");
      context.setCandidates(candidates);
      if (!options.unselected) {
        const auto key    = context.keyOf(candidates[options.selectedIndex]);
        const auto result = context.execute(xanadu::nav::SelectLink{
            .key  = key,
            .hint = {
                .side = xanadu::LinkSide::Left, .member = 0, .occurrence = 0}});
        if (!result)
          throw std::runtime_error("diagnostic Link selection refused");
      }
    });
    renderer->setShutdownHook([&](RenderState &renderState) {
      std::ofstream metadata(options.out / "scene.json");
      if (!metadata) throw std::runtime_error("cannot write scene metadata");
      metadata
          << "{\"evidence\":\"native component diagnostic; not UI journey\","
          << "\"backend\":\"" << render::backendName(options.backend) << "\","
          << "\"scene\":\"" << options.scene
          << "\",\"darkPaper\":" << (options.dark ? "true" : "false")
          << ",\"width\":" << options.width << ",\"height\":" << options.height
          << ",\"uiScale\":" << options.uiScale
          << ",\"fontScale\":" << options.fontScale
          << ",\"angle\":" << options.angle << ",\"tilt\":" << options.tilt
          << ",\"depth\":" << options.depth
          << ",\"diagnosticPageWidthPx\":600,\"diagnosticPageHeightPx\":520"
          << ",\"selectedIndex\":" << options.selectedIndex
          << ",\"linkCount\":" << session.store().links().size()
          << ",\"strandCount\":" << links.strandCount() << ",\"pages\":[";
      bool first = true;
      for (std::size_t index = 0; index < renderState.docs.size(); ++index) {
        const auto &doc = renderState.docs[index];
        if (!first) metadata << ',';
        first = false;
        metadata << "{\"doc\":" << index
                 << ",\"built\":" << doc->builtPageCount() << ",\"model\":";
        writeMatrix(metadata, doc->modelMatrix());
        if (const auto frame = doc->pageFrame(0)) {
          metadata << ",\"page0BoundsPx\":[" << frame->leftPx << ','
                   << frame->rightPx << ',' << frame->topPx << ','
                   << frame->bottomPx << ']';
          metadata << ",\"page0LocalToWorld\":";
          writeMatrix(metadata, frame->localToWorld);
        }
        metadata << '}';
      }
      metadata << "]}\n";
    });

    AutoSDL sdl(SDL_INIT_VIDEO);
    render::configureBackendWindowAttributes(options.backend);
    AutoSDLWindow window(
        "Link stress diagnostic", options.width, options.height,
        render::backendWindowFlags(options.backend) | SDL_WINDOW_HIDDEN);
    (*renderer)(window);
    const auto after       = hashes(options.fixture);
    const auto clonedAfter = hashes(clone);
    std::ofstream integrity(options.out / "source-integrity.json");
    integrity << "{\"originalUnchanged\":"
              << (after == before ? "true" : "false") << ",\"cloneUnchanged\":"
              << (clonedAfter == before ? "true" : "false") << ",\"files\":{";
    bool first = true;
    for (const auto &[name, hash] : before) {
      if (!first) integrity << ',';
      first = false;
      integrity << '"' << name << "\":{\"before\":\"" << hash
                << "\",\"originalAfter\":\"" << after.at(name)
                << "\",\"cloneAfter\":\"" << clonedAfter.at(name) << "\"}";
    }
    integrity << "}}\n";
    if (state->renderFailed || after != before || clonedAfter != before ||
        !fs::exists(options.out / "frame.ppm"))
      return 1;
    std::cout << "Native Link component evidence: " << options.out << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "render-link-stress: " << error.what() << '\n';
    return 1;
  }
}
