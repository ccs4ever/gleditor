/**
 * @file link_beam_occlusion_test.cpp
 * @brief A link's beam is blended in depth order with a faded page.
 *
 * LinkBeams used to draw its beams in drawFrame(), after the frame's
 * translucent list, so a beam passing behind a faded page was drawn over it
 * from every side. It now hands them to FrameContext::translucent from
 * drawScene(), and the list sorts them with the page.
 *
 * The scene: two documents side by side, a link from one to the other, and a
 * faded page standing in front of the gap between them, so the beam crosses
 * behind it. Seen from in front, the page is between the camera and the beam
 * and should mostly hide it; seen from behind, the beam is nearer and should
 * show over the page. Drawn on a real device, on every backend this machine
 * can open.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <unistd.h>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <gleditor/doc.hpp>
#include <gleditor/frame_contributor.hpp>
#include <gleditor/paths.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/renderer.hpp>
#include <gleditor/state.hpp>
#include <gleditor/text_source.hpp>
#include <gleditor/translucent_list.hpp>

#include "../lib/headless_device.hpp"
#include "common/ui/xanadoc/beams.hpp"
#include "common/ui/xanadoc/session.hpp"
#include "common/xanadu/user_permascroll.hpp"

namespace {

namespace fs = std::filesystem;
using Row    = Doc::VBORow;

constexpr int sceneWidth  = 480;
constexpr int sceneHeight = 360;

constexpr const char *sentence  = "alpha beta gamma delta";
constexpr std::uint32_t alphaAt = 0;
constexpr std::uint32_t gammaAt = 11;
constexpr std::uint32_t wordLen = 5;

/// Opaque enough that a beam behind the page is mostly hidden, translucent
/// enough that it is still a faded page and so drawn from the list.
constexpr float sheetOpacity        = 0.8F;
constexpr std::uint32_t sheetColour = 0xE8E8F0FFU;
/// The page's resolution, in its own pixels per world unit.
constexpr float sheetPxPerWorld = 100.0F;

/// A channel further than this from the background is part of what was drawn.
constexpr int drawnThreshold = 12;
/// Pixels where the beam crosses the page that the comparison needs, so a
/// beam that missed the page cannot pass by having nothing to measure.
constexpr std::size_t minimumCrossing = 50;

int channelDistance(const render::FrameImage &a, const render::FrameImage &b,
                    const std::size_t pixel) {
  int most = 0;
  for (std::size_t channel = 0; channel < 3; ++channel) {
    const auto at = (pixel * 4U) + channel;
    most          = std::max(most, std::abs(static_cast<int>(a.rgba[at]) -
                                            static_cast<int>(b.rgba[at])));
  }
  return most;
}

/// How much the beam changed the page where it crosses it, on average, and
/// over how many pixels.
struct Crossing {
  double meanChange{};
  std::size_t pixels{};
};

/**
 * @param empty Neither page nor beam.
 * @param beamAlone The beam with no page.
 * @param pageAlone The page with no beam.
 * @param both The page and the beam, as the path under test drew them.
 */
Crossing crossing(const render::FrameImage &empty,
                  const render::FrameImage &beamAlone,
                  const render::FrameImage &pageAlone,
                  const render::FrameImage &both) {
  Crossing out;
  double total      = 0.0;
  const auto pixels = empty.rgba.size() / 4U;
  for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
    if (channelDistance(empty, beamAlone, pixel) <= drawnThreshold ||
        channelDistance(empty, pageAlone, pixel) <= drawnThreshold) {
      continue;
    }
    total += channelDistance(pageAlone, both, pixel);
    ++out.pixels;
  }
  out.meanChange =
      out.pixels > 0 ? total / static_cast<double>(out.pixels) : 0.0;
  return out;
}

class LinkBeamOcclusionTest : public testing::TestWithParam<render::Backend> {
protected:
  std::unique_ptr<headless::Device> session;
  std::unique_ptr<RenderState> state;
  fs::path root;
  std::map<std::string, std::optional<std::string>> environment;

  void SetUp() override {
    auto opened = headless::open(GetParam(), sceneWidth, sceneHeight);
    if (!opened) {
      GTEST_SKIP() << opened.error();
    }
    session = std::move(*opened);
    state   = std::make_unique<RenderState>(session->device.get());
    // The session's store and the permascroll it writes to go somewhere of
    // the test's own, not into whatever XDG directories the run was given.
    root = fs::temp_directory_path() /
           ("link-beam-occlusion-" + render::backendName(GetParam()) + "-" +
            std::to_string(::getpid()));
    for (const auto *name : {"XDG_CONFIG_HOME", "XDG_DATA_HOME"}) {
      const auto *old   = std::getenv(name);
      environment[name] = old ? std::optional<std::string>{old} : std::nullopt;
      setenv(name, (root / name).c_str(), 1);
    }
  }

  void TearDown() override {
    if (session) {
      session->device->waitIdle();
    }
    state.reset();
    for (const auto &[name, old] : environment) {
      if (old) {
        setenv(name.c_str(), old->c_str(), 1);
      } else {
        unsetenv(name.c_str());
      }
    }
    if (!root.empty()) {
      fs::remove_all(root);
    }
  }
};

} // namespace

TEST_P(LinkBeamOcclusionTest, aBeamBehindAFadedPageIsHiddenFromInFrontOnly) {
  auto &device = *session->device;

  // A link from "alpha" to "gamma", and a second document quoting "gamma":
  // the link is then a relation between two documents, which is a beam.
  xanadu::Session workspace((root / "workspace").string(),
                            std::make_shared<xanadu::UserPermascroll>());
  auto &store      = workspace.store();
  const auto typed = store.insert(xanadu::MicroversionId{}, 0, sentence);
  const auto text  = store.rebuild(typed);
  xanadu::Link link;
  link.type         = xanadu::LinkType::Comment;
  link.owner        = "someone";
  link.left         = text.spansFor(alphaAt, wordLen);
  link.right        = text.spansFor(gammaAt, wordLen);
  const auto linked = store.addLink(typed, link);
  const auto quoted =
      store.transclude(xanadu::MicroversionId{}, 0, linked, gammaAt, wordLen);
  workspace.views().push_back(
      {.version = linked, .pieces = store.rebuild(linked)});
  workspace.views().push_back(
      {.version = quoted, .pieces = store.rebuild(quoted)});

  // A document lays itself out in its renderer's font; the renderer is not
  // started, only asked.
  auto appState             = std::make_shared<AppState>();
  appState->defaultFontName = "Sans 10";
  const auto renderer       = Renderer::create(appState, GetParam());

  // Side by side, with a gap of half a page between them.
  const float pageWidth = Doc::defaultPageWidthWorld();
  const float apart     = pageWidth * 1.5F;
  for (const auto &[contents, x] :
       {std::pair{std::string(sentence), -apart / 2.0F},
        std::pair{std::string(sentence).substr(gammaAt, wordLen),
                  apart / 2.0F}}) {
    auto doc = Doc::create(renderer, &device,
                           glm::translate(glm::mat4(1.0F), {x, 0.0F, 0.0F}),
                           gleditor::MemoryTextSource(contents, "linked"));
    doc->makePages();
    for (int call = 0; call < 50 && !doc->isFullyLoaded(); ++call) {
      std::ignore = doc->buildPendingPages(*state);
    }
    ASSERT_TRUE(doc->isFullyLoaded());
    state->docs.push_back(doc);
  }

  const auto firstLine = state->docs[0]->anchorFor(alphaAt);
  const auto frameA    = state->docs[0]->pageFrame(0);
  const auto frameB    = state->docs[1]->pageFrame(0);
  ASSERT_TRUE(firstLine && frameA && frameB);
  const auto lineAt = state->docs[0]->worldPoint(*firstLine);
  ASSERT_TRUE(lineAt);
  const float gapLeft =
      (frameA->localToWorld * glm::vec4(frameA->rightPx, 0, 0, 1)).x;
  const float gapRight =
      (frameB->localToWorld * glm::vec4(frameB->leftPx, 0, 0, 1)).x;
  ASSERT_LT(gapLeft, gapRight);
  const float gap = gapRight - gapLeft;

  // The faded page: in front of the gap, across the beam, a quarter of the
  // gap nearer the front than the documents are.
  const glm::vec3 sheetCentre{(gapLeft + gapRight) / 2.0F, lineAt->y,
                              gap * 0.25F};
  const auto sheetSide =
      static_cast<unsigned>(std::lround(gap * 0.6F * sheetPxPerWorld));
  const std::vector<Row> rows{
      Row{.pos        = {0, 0},
          .foreground = Row::fill(sheetColour, Row::onPaper),
          .atlas      = 0,
          .quad       = Row::box(0, sheetSide, sheetSide, 0),
          .paper      = Row::paperAt(sheetColour, 0)}};
  const auto buffer = device.createBuffer(render::BufferKind::Vertex,
                                          rows.size() * sizeof(Row));
  device.updateBuffer(buffer, 0, std::as_bytes(std::span<const Row>(rows)));
  const auto unwritten = device.createPipeline(headless::glyphPipeline(false));
  const auto sheetModel =
      glm::scale(glm::translate(glm::mat4(1.0F), sheetCentre),
                 glm::vec3(1.0F / sheetPxPerWorld));

  xanadu::LinkBeams links(workspace, nullptr);
  links.setSworph(false);
  links.deviceReady(device, headless::glyphPipeline(true));
  gleditor::TranslucentList list(&device);
  ch::Timeline timeline;

  struct Side {
    const char *name;
    float towards;
  };
  for (const auto &side : {Side{"in front", 1.0F}, Side{"behind", -1.0F}}) {
    const float distance = gap / std::tan(glm::radians(45.0F) / 2.0F);
    const glm::vec3 target{sheetCentre.x, sheetCentre.y, 0.0F};
    const auto worldToClip =
        glm::perspective(glm::radians(45.0F),
                         static_cast<float>(sceneWidth) / sceneHeight, 0.1F,
                         distance * 4.0F) *
        glm::lookAt(target + glm::vec3(0.0F, 0.0F, distance * side.towards),
                    target, glm::vec3(0.0F, 1.0F, 0.0F));
    render::GlyphBatch sheet{};
    std::copy_n(glm::value_ptr(worldToClip * sheetModel), 16,
                sheet.uniforms.mvp.begin());
    sheet.uniforms.opacity = sheetOpacity;
    sheet.vertices         = buffer;
    sheet.instanceCount    = static_cast<std::uint32_t>(rows.size());

    enum class Path : std::uint8_t {
      /// What the renderer does now: the beams handed to the list.
      Listed,
      /// What it did before: the beams drawn over the list once it is drawn.
      /// The right order when the beam is nearer than the page.
      AfterList,
      /// The beams drawn under the list: the right order when the page is
      /// nearer than the beam.
      BeforeList,
    };
    const auto capture = [&](const bool page, const bool beam,
                             const Path path) {
      links.setVisible(beam);
      render::FrameImage image;
      // Twice, so a frame is drawn over one that has already been drawn.
      for (int frame = 0; frame < 2; ++frame) {
        EXPECT_TRUE(device.beginFrame());
        device.setHighlights({});
        state->beginPickScene();
        list.clear();
        if (page) {
          list.addSheet(unwritten, sheet);
        }
        gleditor::FrameContext ctx{.state          = *state,
                                   .viewProjection = worldToClip,
                                   .screenWidth    = sceneWidth,
                                   .screenHeight   = sceneHeight,
                                   .timeline       = timeline,
                                   .translucent =
                                       Path::Listed == path ? &list : nullptr};
        if (Path::AfterList == path) {
          list.draw(*state);
          links.drawScene(ctx);
        } else {
          links.drawScene(ctx);
          list.draw(*state);
        }
        links.drawFrame(ctx);
        device.endFrame();
        device.waitIdle();
        image = device.captureColorTarget();
      }
      return image;
    };

    const auto empty     = capture(false, false, Path::Listed);
    const auto beamAlone = capture(false, true, Path::Listed);
    const auto pageAlone = capture(true, false, Path::Listed);
    const auto listed    = capture(true, true, Path::Listed);
    const auto over      = capture(true, true, Path::AfterList);
    const auto under     = capture(true, true, Path::BeforeList);

    // How much the beam changes the page where it crosses it: drawn as the
    // list draws it, over the page, and under it.
    const auto now        = crossing(empty, beamAlone, pageAlone, listed);
    const auto overPage   = crossing(empty, beamAlone, pageAlone, over);
    const auto underPage  = crossing(empty, beamAlone, pageAlone, under);
    const bool pageNearer = side.towards > 0.0F;
    const auto &right     = pageNearer ? underPage : overPage;
    const auto &wrong     = pageNearer ? overPage : underPage;
    std::cout << "[ link beam ] " << render::backendName(GetParam()) << ' '
              << side.name << ": " << now.pixels
              << " pixels of beam across the page; mean change "
              << now.meanChange << " listed, " << underPage.meanChange
              << " under the page, " << overPage.meanChange << " over it\n";
    ASSERT_GE(now.pixels, minimumCrossing) << side.name;
    // The two orders must look different here, or the scene proves nothing.
    const double apart = std::abs(right.meanChange - wrong.meanChange);
    ASSERT_GT(apart, static_cast<double>(drawnThreshold)) << side.name;
    // In front, the page hides most of the beam behind it; from behind, the
    // beam shows over the page. Compared in aggregate rather than pixel by
    // pixel because the beam's pulse moves on every frame.
    EXPECT_LT(std::abs(now.meanChange - right.meanChange), apart * 0.25)
        << side.name;
    if (pageNearer) {
      EXPECT_LT(now.meanChange, overPage.meanChange) << side.name;
    }
  }
  EXPECT_TRUE(device.takeDiagnostics().empty());
  device.waitIdle();
  device.destroyBuffer(buffer);
}

INSTANTIATE_TEST_SUITE_P(
    Backends, LinkBeamOcclusionTest,
    testing::ValuesIn(headless::compiledBackends()),
    [](const testing::TestParamInfo<render::Backend> &info) {
      return render::backendName(info.param);
    });
