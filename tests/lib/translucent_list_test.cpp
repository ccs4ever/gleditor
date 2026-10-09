/**
 * @file translucent_list_test.cpp
 * @brief Translucent sheets and beams drawn back to front as one list.
 *
 * The scene is the one the view project's spike R5 measured
 * (design/view-system-implementation-plan.md section 3.1): a faded page, a
 * faded sheet behind it and to one side, and a beam running from in front of
 * the page to behind the sheet, crossing both -- seen from in front and from
 * behind. Drawn the way the renderer used to draw it, tens of thousands of
 * pixels were wrong from either side.
 *
 * The first tests read the order off a mock device; the last draws the scene
 * on every backend this machine can open and compares it with the same scene
 * put in order by hand.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <gleditor/beams.hpp>
#include <gleditor/doc.hpp>
#include <gleditor/paths.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/translucent_list.hpp>

#include "headless_device.hpp"
#include "mocks/device.hpp"

using gleditor::Beams;
using gleditor::TranslucentList;
using Row = Doc::VBORow;

namespace {

render::DrawUniforms uniforms(const glm::mat4 &mvp, const float opacity,
                              const std::uint32_t identity) {
  render::DrawUniforms out;
  std::copy_n(glm::value_ptr(mvp), 16, out.mvp.begin());
  out.opacity  = opacity;
  out.identity = identity;
  return out;
}

// ---------------------------------------------------------------------------
// The scene, in world units.

constexpr float perPixel = 1.0F / 50.0F;
const glm::vec3 pageCentre{-0.8F, 0.0F, 0.0F};
const glm::vec3 sheetCentre{0.8F, 0.0F, -1.5F};
constexpr float sheetOpacity = 0.55F;
// From in front of the page to behind the sheet: through the page's plane a
// third of the way along, and the sheet's two thirds of the way.
const glm::vec3 beamFrom{-3.0F, 0.0F, 1.5F};
const glm::vec3 beamTo{3.0F, 0.0F, -3.0F};
constexpr float beamWidth          = 0.18F;
constexpr std::uint32_t beamColour = 0x20F0F0FFU;
const glm::vec3 target{0.0F, 0.0F, -0.75F};
const glm::vec3 inFront{1.2F, 1.0F, 8.0F};
const glm::vec3 behind{1.2F, 1.0F, -9.5F};

glm::mat4 sheetModel(const glm::vec3 &centre) {
  return glm::scale(glm::translate(glm::mat4(1.0F), centre),
                    glm::vec3(perPixel));
}

glm::mat4 camera(const glm::vec3 &eye, const float aspect) {
  return glm::perspective(glm::radians(45.0F), aspect, 0.1F, 100.0F) *
         glm::lookAt(eye, target, glm::vec3(0.0F, 1.0F, 0.0F));
}

// ---------------------------------------------------------------------------
// Order, on a mock device.

/// Records each draw's identity and, for beams, the rows it covered.
class OrderDevice : public testing::NiceMock<MockRenderDevice> {
public:
  struct Call {
    std::uint32_t identity;
    std::uint32_t pipeline;
    std::vector<Beams::Row> beams;
  };
  std::map<std::uint32_t, std::vector<std::byte>> buffers;
  std::vector<Call> calls;
  std::uint32_t nextBuffer{1};
  std::uint32_t nextPipeline{1};
  std::uint32_t bound{};
  std::size_t uploads{};

  OrderDevice() {
    ON_CALL(*this, createBuffer)
        .WillByDefault([this](render::BufferKind, std::size_t bytes) {
          const auto id = nextBuffer++;
          buffers[id].resize(bytes);
          return render::BufferHandle{id};
        });
    ON_CALL(*this, resizeBuffer)
        .WillByDefault([this](render::BufferHandle buffer, std::size_t bytes) {
          buffers[buffer.id].resize(bytes);
          return buffer;
        });
    ON_CALL(*this, updateBuffer)
        .WillByDefault([this](render::BufferHandle buffer, std::size_t offset,
                              std::span<const std::byte> bytes) {
          ++uploads;
          auto &storage = buffers[buffer.id];
          storage.resize(std::max(storage.size(), offset + bytes.size()));
          std::memcpy(storage.data() + offset, bytes.data(), bytes.size());
        });
    ON_CALL(*this, createPipeline)
        .WillByDefault([this](const render::PipelineDesc &) {
          return render::PipelineHandle{nextPipeline++};
        });
    ON_CALL(*this, bindPipeline)
        .WillByDefault(
            [this](render::PipelineHandle pipeline) { bound = pipeline.id; });
    ON_CALL(*this, drawGlyphs)
        .WillByDefault([this](const render::DrawUniforms &drawn,
                              render::BufferHandle buffer, std::size_t offset,
                              std::uint32_t count) {
          Call call{.identity = drawn.identity, .pipeline = bound, .beams = {}};
          if (beamPipelines.contains(bound)) {
            const auto &storage = buffers.at(buffer.id);
            for (std::uint32_t i = 0; i < count; ++i) {
              Beams::Row row{};
              std::memcpy(&row, storage.data() + offset + (i * sizeof(row)),
                          sizeof(row));
              call.beams.push_back(row);
            }
          }
          calls.push_back(std::move(call));
        });
  }

  std::map<std::uint32_t, bool> beamPipelines;
};

class TranslucentOrderTest : public testing::Test {
protected:
  std::unique_ptr<OrderDevice> device;
  std::unique_ptr<RenderState> state;
  render::PipelineHandle sheetPipeline;

  static constexpr std::uint32_t page  = 10;
  static constexpr std::uint32_t sheet = 20;
  static constexpr std::uint32_t beam  = 30;

  void SetUp() override {
    device = std::make_unique<OrderDevice>();
    ON_CALL(*device, textureLimits())
        .WillByDefault(testing::Return(render::TextureLimits{2048, 10}));
    state         = std::make_unique<RenderState>(device.get());
    sheetPipeline = device->createPipeline(render::PipelineDesc{});
  }

  std::unique_ptr<Beams> beams(const glm::vec3 &from, const glm::vec3 &to) {
    auto out = std::make_unique<Beams>(device.get(), 8);
    out->createPipeline(gleditor::assetPath("shaders"),
                        gleditor::assetPath("shaders") + "/vulkan", true,
                        false);
    device->beamPipelines[out->pipelineHandle().id] = true;
    out->add(from, to, beamWidth, beamColour, 1);
    out->commit();
    return out;
  }

  /// The scene from @p eye, through the list; returns the identities drawn.
  std::vector<std::uint32_t> drawFrom(const glm::vec3 &eye,
                                      TranslucentList &list) {
    const auto worldToClip = camera(eye, 4.0F / 3.0F);
    const auto line        = beams(beamFrom, beamTo);
    list.clear()
        ->addSheet(sheetPipeline,
                   render::GlyphBatch{.uniforms = uniforms(
                                          worldToClip * sheetModel(pageCentre),
                                          sheetOpacity, page),
                                      .vertices      = render::BufferHandle{99},
                                      .instanceCount = 1})
        ->addSheet(sheetPipeline,
                   render::GlyphBatch{.uniforms = uniforms(
                                          worldToClip * sheetModel(sheetCentre),
                                          sheetOpacity, sheet),
                                      .vertices      = render::BufferHandle{99},
                                      .instanceCount = 1})
        ->addBeams(*line, worldToClip, 1.0F, beam);
    device->calls.clear();
    list.draw(*state);
    std::vector<std::uint32_t> out;
    for (const auto &call : device->calls) {
      out.push_back(call.identity);
    }
    return out;
  }
};

} // namespace

// The beam is cut where it crosses each sheet and the pieces are put either
// side of the sheets: behind the far sheet, between the two, in front of the
// page. From behind, the same list in reverse.
TEST_F(TranslucentOrderTest, aBeamIsCutWhereItCrossesEachSheet) {
  TranslucentList list(device.get());
  EXPECT_EQ(drawFrom(inFront, list),
            (std::vector<std::uint32_t>{beam, sheet, beam, page, beam}));
  EXPECT_EQ(list.lastDraw().sheets, 2U);
  EXPECT_EQ(list.lastDraw().beams, 1U);
  EXPECT_EQ(list.lastDraw().beamPieces, 3U);
  EXPECT_EQ(list.lastDraw().draws, 5U);

  EXPECT_EQ(drawFrom(behind, list),
            (std::vector<std::uint32_t>{beam, page, beam, sheet, beam}));
}

// The pieces are the beam, end to end: each starts where the last stopped, and
// the fade along it runs once over all three.
TEST_F(TranslucentOrderTest, thePiecesJoinUpAndShareOneFade) {
  TranslucentList list(device.get());
  drawFrom(inFront, list);
  std::vector<Beams::Row> pieces;
  for (const auto &call : device->calls) {
    pieces.insert(pieces.end(), call.beams.begin(), call.beams.end());
  }
  ASSERT_EQ(pieces.size(), 3U);
  // Drawn far end first from in front.
  std::ranges::reverse(pieces);
  EXPECT_NEAR(pieces[0].from[0], beamFrom.x, 1e-5F);
  EXPECT_NEAR(pieces[2].to[2], beamTo.z, 1e-5F);
  EXPECT_NEAR(pieces[0].along[0], 0.0F, 1e-6F);
  EXPECT_NEAR(pieces[2].along[1], 1.0F, 1e-6F);
  for (std::size_t i = 1; i < pieces.size(); ++i) {
    for (std::size_t axis = 0; axis < 3; ++axis) {
      EXPECT_FLOAT_EQ(pieces[i].from[axis], pieces[i - 1].to[axis]);
    }
    EXPECT_FLOAT_EQ(pieces[i].along[0], pieces[i - 1].along[1]);
  }
  // Cut at each sheet's plane: z = 0, a third of the way, and z = -1.5.
  EXPECT_NEAR(pieces[0].to[2], 0.0F, 1e-5F);
  EXPECT_NEAR(pieces[0].along[1], 1.0F / 3.0F, 1e-5F);
  EXPECT_NEAR(pieces[1].to[2], -1.5F, 1e-5F);
}

// Sorted by depth from the camera, not by world z: the bug the renderer had
// was a sort on z, which is back to front only from in front.
TEST_F(TranslucentOrderTest, sheetsAreSortedByDepthFromTheCamera) {
  TranslucentList list(device.get());
  for (const auto &[eye, first] :
       {std::pair{inFront, sheet}, std::pair{behind, page}}) {
    const auto worldToClip = camera(eye, 1.0F);
    list.clear()
        ->addSheet(sheetPipeline,
                   render::GlyphBatch{.uniforms = uniforms(
                                          worldToClip * sheetModel(pageCentre),
                                          sheetOpacity, page),
                                      .vertices      = render::BufferHandle{99},
                                      .instanceCount = 1})
        ->addSheet(sheetPipeline,
                   render::GlyphBatch{.uniforms = uniforms(
                                          worldToClip * sheetModel(sheetCentre),
                                          sheetOpacity, sheet),
                                      .vertices      = render::BufferHandle{99},
                                      .instanceCount = 1});
    device->calls.clear();
    list.draw(*state);
    ASSERT_EQ(device->calls.size(), 2U);
    EXPECT_EQ(device->calls.front().identity, first);
  }
}

// Every page of a document lies in one plane, so a beam crossing it crosses
// every page's plane at one place: one cut, not one per page.
TEST_F(TranslucentOrderTest, coplanarSheetsCutABeamOnce) {
  TranslucentList list(device.get());
  const auto worldToClip = camera(inFront, 1.0F);
  const auto line        = beams(beamFrom, beamTo);
  list.clear();
  for (int column = 0; column < 4; ++column) {
    const glm::vec3 at{-2.0F + (static_cast<float>(column) * 1.5F), 0.0F, 0.0F};
    list.addSheet(
        sheetPipeline,
        render::GlyphBatch{.uniforms = uniforms(worldToClip * sheetModel(at),
                                                sheetOpacity, page),
                           .vertices = render::BufferHandle{99},
                           .instanceCount = 1});
  }
  list.addBeams(*line, worldToClip, 1.0F, beam);
  list.draw(*state);
  EXPECT_EQ(list.lastDraw().beamPieces, 2U);
}

// With nothing translucent to be on the wrong side of, a batch of beams is
// drawn whole from its own storage: one draw, nothing uploaded.
TEST_F(TranslucentOrderTest, beamsWithNoSheetAreDrawnWhole) {
  TranslucentList list(device.get());
  const auto line = beams(beamFrom, beamTo);
  const auto sent = device->uploads;
  list.clear()->addBeams(*line, camera(inFront, 1.0F), 1.0F, beam);
  list.draw(*state);
  EXPECT_EQ(device->uploads, sent);
  EXPECT_EQ(list.lastDraw().draws, 1U);
  EXPECT_EQ(list.lastDraw().beamPieces, 1U);
}

TEST_F(TranslucentOrderTest, anEmptyListDrawsNothing) {
  TranslucentList list(device.get());
  EXPECT_TRUE(list.empty());
  list.draw(*state);
  EXPECT_TRUE(device->calls.empty());
  EXPECT_EQ(list.lastDraw().draws, 0U);
}

// ---------------------------------------------------------------------------
// The scene, drawn.

namespace {

constexpr int sceneWidth  = 800;
constexpr int sceneHeight = 600;
/// A pixel is wrong when a channel is further than this from the reference:
/// what blending rounds differently between two orders that are both right.
constexpr int channelTolerance = 2;

Row solid(const float cx, const float cy, const unsigned width,
          const unsigned height, const unsigned rgb, const unsigned depth) {
  return Row{.pos        = {cx, cy},
             .foreground = Row::fill(rgb, depth),
             .atlas      = 0,
             .quad       = Row::box(0, width, height, 0),
             .paper      = Row::paperAt(rgb, 0)};
}

std::size_t differing(const render::FrameImage &a,
                      const render::FrameImage &b) {
  std::size_t count = 0;
  for (std::size_t i = 0; i + 3 < a.rgba.size(); i += 4) {
    for (std::size_t channel = 0; channel < 3; ++channel) {
      if (std::abs(static_cast<int>(a.rgba[i + channel]) -
                   static_cast<int>(b.rgba[i + channel])) > channelTolerance) {
        ++count;
        break;
      }
    }
  }
  return count;
}

class TranslucentSceneTest : public testing::TestWithParam<render::Backend> {
protected:
  std::unique_ptr<headless::Device> session;
  std::unique_ptr<RenderState> state;

  void SetUp() override {
    auto opened = headless::open(GetParam(), sceneWidth, sceneHeight);
    if (!opened) {
      GTEST_SKIP() << opened.error();
    }
    session = std::move(*opened);
    state   = std::make_unique<RenderState>(session->device.get());
  }

  void TearDown() override {
    if (session) {
      session->device->waitIdle();
    }
    state.reset();
  }
};

} // namespace

TEST_P(TranslucentSceneTest, isRightFromBothSides) {
  auto &device         = *session->device;
  const auto writing   = device.createPipeline(headless::glyphPipeline(true));
  const auto unwritten = device.createPipeline(headless::glyphPipeline(false));
  const auto shaders   = gleditor::assetPath("shaders");

  // A page of paper with lines of text on it, and an amber card.
  std::vector<Row> rows;
  rows.push_back(solid(0, 0, 220, 280, 0xEEEEF4FFU, Row::onPaper));
  for (int line = 0; line < 12; ++line) {
    rows.push_back(solid(-10.0F, 120.0F - (static_cast<float>(line) * 20.0F),
                         180, 8, 0x303040FFU, Row::onText));
  }
  const auto pageRows = static_cast<std::uint32_t>(rows.size());
  rows.push_back(solid(0, 0, 220, 160, 0xE0A030FFU, Row::onPaper));
  for (int line = 0; line < 4; ++line) {
    rows.push_back(solid(0, 50.0F - (static_cast<float>(line) * 30.0F), 160, 14,
                         0x402000FFU, Row::onText));
  }
  const auto sheetRows = static_cast<std::uint32_t>(rows.size()) - pageRows;
  const auto buffer    = device.createBuffer(render::BufferKind::Vertex,
                                             rows.size() * sizeof(Row));
  device.updateBuffer(buffer, 0, std::as_bytes(std::span<const Row>(rows)));

  const auto batchOf = [&](const glm::mat4 &worldToClip, const bool isPage) {
    return render::GlyphBatch{
        .uniforms         = uniforms(worldToClip *
                                         sheetModel(isPage ? pageCentre : sheetCentre),
                                     sheetOpacity, 0),
        .vertices         = buffer,
        .vertexByteOffset = isPage ? 0 : pageRows * sizeof(Row),
        .instanceCount    = isPage ? pageRows : sheetRows};
  };

  Beams writtenBeam(&device);
  writtenBeam.createPipeline(shaders, shaders + "/vulkan", true, true);
  writtenBeam.add(beamFrom, beamTo, beamWidth, beamColour, 1);
  writtenBeam.commit();
  Beams beam(&device);
  beam.createPipeline(shaders, shaders + "/vulkan", true, false);
  beam.add(beamFrom, beamTo, beamWidth, beamColour, 1);
  beam.commit();
  // The reference: the beam in three pieces, cut by hand where it crosses each
  // sheet, independently of the list's own cutting and sorting.
  const auto along = [](const float t) {
    return beamFrom + ((beamTo - beamFrom) * t);
  };
  std::array<std::unique_ptr<Beams>, 3> pieces;
  const std::array<float, 4> cuts{0.0F, 1.0F / 3.0F, 2.0F / 3.0F, 1.0F};
  for (std::size_t i = 0; i < pieces.size(); ++i) {
    pieces[i] = std::make_unique<Beams>(&device);
    pieces[i]->createPipeline(shaders, shaders + "/vulkan", true, false);
    pieces[i]->add(along(cuts[i]), along(cuts[i + 1]), beamWidth, beamColour, 1,
                   cuts[i], cuts[i + 1]);
    pieces[i]->commit();
  }

  TranslucentList list(&device);

  const auto capture = [&](auto &&drawing) {
    render::FrameImage image;
    // Twice, so a frame is drawn over one that has already been drawn.
    for (int frame = 0; frame < 2; ++frame) {
      EXPECT_TRUE(device.beginFrame());
      device.setHighlights({});
      drawing();
      device.endFrame();
      device.waitIdle();
      image = device.captureColorTarget();
    }
    return image;
  };

  struct Side {
    const char *name;
    glm::vec3 eye;
    bool pageNearer;
  };
  for (const auto &side :
       {Side{"in front", inFront, true}, Side{"behind", behind, false}}) {
    const auto worldToClip =
        camera(side.eye, static_cast<float>(sceneWidth) / sceneHeight);
    const auto drawSheet = [&](const render::PipelineHandle pipeline,
                               const bool isPage) {
      device.bindPipeline(pipeline);
      device.bindAtlasTexture(state->glyphCache.textureHandle());
      const auto batch = batchOf(worldToClip, isPage);
      device.drawGlyphs(batch.uniforms, batch.vertices, batch.vertexByteOffset,
                        batch.instanceCount);
    };

    // How the renderer drew it before: documents in order of world z, lowest
    // first, writing depth; then the program's beams over them.
    const auto before = capture([&] {
      drawSheet(writing, false);
      drawSheet(writing, true);
      writtenBeam.draw(*state, worldToClip, 1.0F, 0);
    });

    const auto reference = capture([&] {
      const auto far  = [&] { drawSheet(unwritten, !side.pageNearer); };
      const auto near = [&] { drawSheet(unwritten, side.pageNearer); };
      // In front, the far end of the beam is behind the sheet; behind, the
      // near end is behind the page.
      const auto &first = side.pageNearer ? pieces[2] : pieces[0];
      const auto &last  = side.pageNearer ? pieces[0] : pieces[2];
      first->draw(*state, worldToClip, 1.0F, 0);
      far();
      pieces[1]->draw(*state, worldToClip, 1.0F, 0);
      near();
      last->draw(*state, worldToClip, 1.0F, 0);
    });

    const auto after = capture([&] {
      list.clear()
          ->addSheet(unwritten, batchOf(worldToClip, true))
          ->addSheet(unwritten, batchOf(worldToClip, false))
          ->addBeams(beam, worldToClip, 1.0F, 0);
      list.draw(*state);
    });

    const auto wrongBefore = differing(before, reference);
    const auto wrongAfter  = differing(after, reference);
    std::cout << "[ R5 ] " << render::backendName(GetParam()) << ' '
              << side.name << ": " << wrongBefore << " of "
              << (sceneWidth * sceneHeight) << " pixels wrong before, "
              << wrongAfter << " after\n";
    RecordProperty(std::string("wrongBefore ") + side.name,
                   static_cast<int>(wrongBefore));
    RecordProperty(std::string("wrongAfter ") + side.name,
                   static_cast<int>(wrongAfter));
    // The scene shows the fault: drawn as before, the beam is wrong from in
    // front, and from behind the sheets are blended in the wrong order too.
    EXPECT_GT(wrongBefore, 1000U) << side.name;
    EXPECT_EQ(wrongAfter, 0U) << side.name;
  }
  EXPECT_TRUE(device.takeDiagnostics().empty());
  device.waitIdle();
  device.destroyBuffer(buffer);
}

INSTANTIATE_TEST_SUITE_P(
    Backends, TranslucentSceneTest,
    testing::ValuesIn(headless::compiledBackends()),
    [](const testing::TestParamInfo<render::Backend> &info) {
      return render::backendName(info.param);
    });
