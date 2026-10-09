/**
 * @file depth_write_test.cpp
 * @brief PipelineDesc::depthWrite, on every backend this machine can open.
 *
 * Depth test and depth write used to be one flag, so a translucent plane drawn
 * after the opaque ones either hid everything drawn after it and behind it, or
 * was not hidden by what was in front of it. The cases are the blend orders
 * the rendering plan names (design/world-space-rendering-plan.md section 4):
 * a translucent plane behind an opaque one is hidden; in front of it, it
 * blends; two translucent planes drawn back to front both show. One more says
 * the flag does what it says: a plane drawn without depth write does not hide
 * one drawn after it and behind it, where one drawn with it does.
 */
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

#include <gleditor/canvas.hpp>
#include <gleditor/render_state.hpp>

#include "headless_device.hpp"

namespace {

constexpr int side            = 32;
constexpr std::uint32_t red   = 0xFF0000FFU;
constexpr std::uint32_t green = 0x00FF00FFU;
/// Within this of the expected channel value: blending rounds differently on
/// each backend, by a step or so.
constexpr int tolerance = 3;

class DepthWriteTest : public testing::TestWithParam<render::Backend> {
protected:
  std::unique_ptr<headless::Device> session;
  std::unique_ptr<RenderState> state;
  const glm::mat4 projection =
      glm::ortho(0.0F, static_cast<float>(side), 0.0F, static_cast<float>(side),
                 0.1F, 10.0F);

  void SetUp() override {
    auto opened = headless::open(GetParam(), side, side);
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

  /// A plane covering the whole target, filled with @p colour, depth tested
  /// and written or not.
  std::unique_ptr<gleditor::Canvas> plane(const std::uint32_t colour,
                                          const bool depthWrite) {
    auto out =
        std::make_unique<gleditor::Canvas>(session->device.get(), "Sans 12", 4);
    out->createPipeline(headless::glyphPipeline(depthWrite), true);
    out->addRect(0.0F, 0.0F, static_cast<float>(side), static_cast<float>(side),
                 colour);
    out->commit();
    return out;
  }

  /// Where a plane at @p eyeZ (negative, in front of the camera) is drawn.
  [[nodiscard]] glm::mat4 at(const float eyeZ) const {
    return projection * glm::translate(glm::mat4(1.0F), {0.0F, 0.0F, eyeZ});
  }

  struct Draw {
    const gleditor::Canvas *canvas;
    float eyeZ;
    float opacity;
  };

  /// Draw @p draws in order, twice -- the second frame shows that the first
  /// left nothing behind -- and return the middle pixel.
  template <std::size_t N>
  std::array<int, 3> render(const std::array<Draw, N> &draws) {
    render::FrameImage image;
    for (int frame = 0; frame < 2; ++frame) {
      EXPECT_TRUE(session->device->beginFrame());
      session->device->setHighlights({});
      for (const auto &draw : draws) {
        draw.canvas->draw(*state, at(draw.eyeZ), draw.opacity);
      }
      session->device->endFrame();
      image = session->device->captureColorTarget();
    }
    EXPECT_TRUE(session->device->takeDiagnostics().empty());
    return headless::pixel(image, side / 2, side / 2);
  }

  static void expectNear(const std::array<int, 3> &actual,
                         const std::array<int, 3> &expected) {
    for (std::size_t channel = 0; channel < 3; ++channel) {
      EXPECT_LE(std::abs(actual[channel] - expected[channel]), tolerance)
          << "channel " << channel << ": " << actual[channel] << " against "
          << expected[channel];
    }
  }
};

} // namespace

TEST_P(DepthWriteTest, aTranslucentPlaneBehindAnOpaqueOneIsHidden) {
  const auto opaque      = plane(red, true);
  const auto translucent = plane(green, false);
  expectNear(render(std::array{Draw{opaque.get(), -1.0F, 1.0F},
                               Draw{translucent.get(), -2.0F, 0.5F}}),
             {255, 0, 0});
}

TEST_P(DepthWriteTest, aTranslucentPlaneInFrontOfAnOpaqueOneBlends) {
  const auto opaque      = plane(red, true);
  const auto translucent = plane(green, false);
  expectNear(render(std::array{Draw{opaque.get(), -2.0F, 1.0F},
                               Draw{translucent.get(), -1.0F, 0.5F}}),
             {128, 128, 0});
}

TEST_P(DepthWriteTest, twoTranslucentPlanesDrawnBackToFrontBothShow) {
  const auto back  = plane(red, false);
  const auto front = plane(green, false);
  // Half of the red over black, then half of the green over that.
  expectNear(render(std::array{Draw{back.get(), -2.0F, 0.5F},
                               Draw{front.get(), -1.0F, 0.5F}}),
             {64, 128, 0});
}

// The flag itself. Drawn front first, a plane that writes its depth hides the
// one behind it, which is what a translucent plane must not do; one that does
// not write leaves it to show through.
TEST_P(DepthWriteTest, onlyAPlaneThatWritesDepthHidesWhatIsDrawnAfterBehind) {
  const auto writing   = plane(green, true);
  const auto unwritten = plane(green, false);
  const auto behind    = plane(red, false);
  expectNear(render(std::array{Draw{writing.get(), -1.0F, 0.5F},
                               Draw{behind.get(), -2.0F, 1.0F}}),
             {0, 128, 0});
  const auto shown = render(std::array{Draw{unwritten.get(), -1.0F, 0.5F},
                                       Draw{behind.get(), -2.0F, 1.0F}});
  EXPECT_GT(shown[0], 200) << "the plane behind was hidden";
}

// A frame that ends on a draw without depth write must not leave the next
// frame's depth uncleared: the mask governs clears as well as draws.
TEST_P(DepthWriteTest, aFrameEndingWithoutDepthWriteStillClearsDepth) {
  const auto near        = plane(red, true);
  const auto translucent = plane(green, false);
  render(std::array{Draw{near.get(), -1.0F, 1.0F},
                    Draw{translucent.get(), -1.0F, 0.5F}});
  // Nothing near this frame: a plane far back must show.
  const auto far = plane(green, true);
  expectNear(render(std::array{Draw{far.get(), -9.0F, 1.0F}}), {0, 255, 0});
}

INSTANTIATE_TEST_SUITE_P(
    Backends, DepthWriteTest, testing::ValuesIn(headless::compiledBackends()),
    [](const testing::TestParamInfo<render::Backend> &info) {
      return render::backendName(info.param);
    });
