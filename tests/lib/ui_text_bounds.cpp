#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <spdlog/sinks/ostream_sink.h>

#include <gleditor/canvas.hpp>
#include <gleditor/glyphcache/cache.hpp>
#include <gleditor/logging.hpp>
#include <gleditor/render/types.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/text/diagnostics.hpp>
#include <gleditor/text/font.hpp>
#include <gleditor/text/layout.hpp>
#include <gleditor/ui/text_diagnostics.hpp>

#include "mocks/device.hpp"

namespace {

using gleditor::ui::TextBounds;

TEST(TextBoundsTest, IncludesEveryEdgeInCanvasCoordinates) {
  const TextBounds parent{
      .left = 20.0F, .bottom = 30.0F, .width = 100.0F, .height = 50.0F};
  EXPECT_TRUE(parent.contains(parent));
  EXPECT_TRUE(parent.contains(
      {.left = 120.0F, .bottom = 80.0F, .width = 0.0F, .height = 0.0F}));
  EXPECT_FALSE(parent.contains(
      {.left = 19.0F, .bottom = 30.0F, .width = 10.0F, .height = 10.0F}));
  EXPECT_FALSE(parent.contains(
      {.left = 20.0F, .bottom = 29.0F, .width = 10.0F, .height = 10.0F}));
  EXPECT_FALSE(parent.contains(
      {.left = 111.0F, .bottom = 30.0F, .width = 10.0F, .height = 10.0F}));
  EXPECT_FALSE(parent.contains(
      {.left = 20.0F, .bottom = 71.0F, .width = 10.0F, .height = 10.0F}));
}

TEST(TextBoundsTest, InvalidGeometryCannotContainText) {
  const TextBounds parent{.width = 100.0F, .height = 100.0F};
  for (const auto invalid : {
           TextBounds{.width = -1.0F, .height = 10.0F},
           TextBounds{.width = 10.0F, .height = -1.0F},
           TextBounds{.left   = std::numeric_limits<float>::quiet_NaN(),
                      .width  = 10.0F,
                      .height = 10.0F},
           TextBounds{.bottom = std::numeric_limits<float>::infinity(),
                      .width  = 10.0F,
                      .height = 10.0F},
       }) {
    EXPECT_FALSE(parent.contains(invalid));
    EXPECT_FALSE(invalid.contains(parent));
  }
}

TEST(TextBoundsTest, DetectsTheHeightOfRealWrappedText) {
  const auto font =
      gleditor::text::FontManager::instance().getFont("Monospace 16");
  ASSERT_NE(font, nullptr);
  const auto shaping = gleditor::text::TextLayout::layoutPage(
      "A long label that needs several lines in a narrow parent", font,
      {.maxWidthPx = 80.0F, .maxHeightPx = 0.0F});
  ASSERT_GT(shaping.lineCount, 1U);

  const TextBounds parent{.left   = 20.0F,
                          .bottom = 30.0F,
                          .width  = 80.0F,
                          .height = font->metrics().lineHeight};
  const auto top = parent.bottom + parent.height;
  const TextBounds run{.left   = parent.left,
                       .bottom = top - static_cast<float>(shaping.textHeightPx),
                       .width  = static_cast<float>(shaping.textWidthPx),
                       .height = static_cast<float>(shaping.textHeightPx)};
  EXPECT_FALSE(parent.contains(run));
  EXPECT_TRUE(gleditor::ui::reportTextOverflow(run, parent, 7, 42));
  EXPECT_FALSE(gleditor::ui::reportTextOverflow(parent, parent, 7, 42));
}

class CanvasTextBoundsTest : public testing::Test {
protected:
  testing::NiceMock<MockRenderDevice> device;
  std::unique_ptr<RenderState> state;
  std::unique_ptr<gleditor::Canvas> canvas;
  std::ostringstream messages;
  std::shared_ptr<spdlog::logger> logger;
  std::vector<spdlog::sink_ptr> previousSinks;
  spdlog::level::level_enum previousLevel{};

  void SetUp() override {
    ON_CALL(device, textureLimits())
        .WillByDefault(testing::Return(render::TextureLimits{2048, 10}));
    ON_CALL(device,
            createTextureArray(testing::_, testing::_, testing::_, testing::_))
        .WillByDefault(testing::Return(render::TextureHandle{1}));
    ON_CALL(device, createBuffer(testing::_, testing::_))
        .WillByDefault(testing::Return(render::BufferHandle{1}));
    state  = std::make_unique<RenderState>(&device);
    canvas = std::make_unique<gleditor::Canvas>(&device, "Monospace 16");

    logger        = gleditor::logging::category("ui.layout");
    previousSinks = logger->sinks();
    previousLevel = logger->level();
    auto sink     = std::make_shared<spdlog::sinks::ostream_sink_mt>(messages);
    sink->set_pattern("%v");
    logger->sinks() = {std::move(sink)};
    logger->set_level(spdlog::level::debug);
  }

  void TearDown() override {
    canvas.reset();
    state.reset();
    logger->sinks() = std::move(previousSinks);
    logger->set_level(previousLevel);
  }

  void drawOutsideParent() {
    canvas->setTag(render::tagKindOverlay, 42);
    canvas->addText(*state, 10.0F, 100.0F,
                    "PRIVATE_CANVAS_TEXT_THAT_MUST_STAY_OUT_OF_LOG",
                    0xFFFFFFFFU, 0x000000FFU);
  }

  void resetMessages() {
    messages.str({});
    messages.clear();
  }
};

TEST_F(CanvasTextBoundsTest, LogsOnlyGeometryAndPickingIdentity) {
  canvas->setTextBounds(TextBounds{.width = 1.0F, .height = 1.0F});
  drawOutsideParent();

  EXPECT_THAT(messages.str(), testing::HasSubstr("text overflow: tag="));
  EXPECT_THAT(messages.str(), testing::HasSubstr(":42"));
  EXPECT_THAT(messages.str(),
              testing::Not(testing::HasSubstr("PRIVATE_CANVAS_TEXT")));
}

TEST_F(CanvasTextBoundsTest, ClearRemovesThePreviousParentBounds) {
  canvas->setTextBounds(TextBounds{.width = 1.0F, .height = 1.0F});
  drawOutsideParent();
  ASSERT_FALSE(messages.str().empty());
  resetMessages();

  canvas->clear();
  drawOutsideParent();
  EXPECT_TRUE(messages.str().empty());
}

TEST_F(CanvasTextBoundsTest, BoundsCanBeExplicitlyDisabled) {
  canvas->setTextBounds(TextBounds{.width = 1.0F, .height = 1.0F});
  canvas->setTextBounds(std::nullopt);
  drawOutsideParent();
  EXPECT_TRUE(messages.str().empty());
}

TEST_F(CanvasTextBoundsTest, DebugLoggingIsOptIn) {
  logger->set_level(spdlog::level::info);
  canvas->setTextBounds(TextBounds{.width = 1.0F, .height = 1.0F});
  drawOutsideParent();
  EXPECT_TRUE(messages.str().empty());
}

TEST_F(CanvasTextBoundsTest, CountsColdGlyphShapingButNotCachedGlyphs) {
  const auto font =
      gleditor::text::FontManager::instance().getFont("Monospace 16");
  ASSERT_NE(font, nullptr);
  {
    gleditor::text::ShapingStatsScope capture;
    ASSERT_TRUE(state->glyphCache.put("A", font).has_value());
    EXPECT_EQ(capture.stats(),
              (gleditor::text::ShapingStats{.harfbuzzCalls = 1}));
  }
  {
    gleditor::text::ShapingStatsScope capture;
    ASSERT_TRUE(state->glyphCache.put("A", font).has_value());
    EXPECT_EQ(capture.stats(), gleditor::text::ShapingStats{});
  }
}

} // namespace
