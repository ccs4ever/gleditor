#include <gtest/gtest.h>

#include <string_view>
#include <thread>

#include <gleditor/text/diagnostics.hpp>
#include <gleditor/text/font.hpp>
#include <gleditor/text/layout.hpp>

namespace {

using gleditor::text::FontManager;
using gleditor::text::LayoutOptions;
using gleditor::text::ShapingStats;
using gleditor::text::ShapingStatsScope;
using gleditor::text::TextLayout;

TEST(TextDiagnosticsTest, SingleLineOverloadsCountOneLayoutEach) {
  const auto font = FontManager::instance().getFont("Monospace 16");
  ASSERT_NE(font, nullptr);
  constexpr std::string_view text = "ASCII label";

  ShapingStatsScope capture;
  EXPECT_FALSE(TextLayout::layoutSingleLine(text, font).glyphs.empty());
  EXPECT_FALSE(
      TextLayout::layoutSingleLine(text, font, LayoutOptions{}).glyphs.empty());

  EXPECT_EQ(capture.stats(), (ShapingStats{.layoutCalls   = 2,
                                           .harfbuzzCalls = 2,
                                           .inputBytes    = text.size() * 2}));
}

TEST(TextDiagnosticsTest, EmptyTextAndMissingFontNeverCallHarfBuzz) {
  const auto font = FontManager::instance().getFont("Monospace 16");
  ASSERT_NE(font, nullptr);
  constexpr std::string_view text = "not shaped";

  ShapingStatsScope capture;
  EXPECT_TRUE(TextLayout::layoutPage({}, font, LayoutOptions{}).glyphs.empty());
  EXPECT_TRUE(
      TextLayout::layoutPage(text, nullptr, LayoutOptions{}).glyphs.empty());

  EXPECT_EQ(capture.stats(),
            (ShapingStats{.layoutCalls = 2, .inputBytes = text.size()}));
}

TEST(TextDiagnosticsTest, NestedCaptureRestoresItsOuterCapture) {
  ShapingStatsScope outer;
  gleditor::text::detail::recordLayout(5);
  {
    ShapingStatsScope inner;
    gleditor::text::detail::recordLayout(7);
    gleditor::text::detail::recordHarfBuzz(false);
    gleditor::text::detail::recordHarfBuzz(true);
    EXPECT_EQ(inner.stats(), (ShapingStats{.layoutCalls   = 1,
                                           .harfbuzzCalls = 2,
                                           .fallbackCalls = 1,
                                           .inputBytes    = 7}));
    EXPECT_EQ(outer.stats(), (ShapingStats{.layoutCalls = 1, .inputBytes = 5}));
  }
  gleditor::text::detail::recordLayout(11);
  EXPECT_EQ(outer.stats(), (ShapingStats{.layoutCalls = 2, .inputBytes = 16}));
}

TEST(TextDiagnosticsTest, DisabledCaptureLeavesOuterCaptureActive) {
  ShapingStatsScope outer;
  {
    ShapingStatsScope disabled(false);
    gleditor::text::detail::recordLayout(13);
    gleditor::text::detail::recordHarfBuzz(false);
    EXPECT_EQ(disabled.stats(), ShapingStats{});
  }
  EXPECT_EQ(
      outer.stats(),
      (ShapingStats{.layoutCalls = 1, .harfbuzzCalls = 1, .inputBytes = 13}));
}

TEST(TextDiagnosticsTest, CaptureDoesNotLeakAcrossThreads) {
  ShapingStatsScope mainCapture;
  ShapingStats workerStats;
  std::thread worker([&] {
    gleditor::text::detail::recordLayout(100);
    ShapingStatsScope workerCapture;
    gleditor::text::detail::recordLayout(17);
    gleditor::text::detail::recordHarfBuzz(true);
    workerStats = workerCapture.stats();
  });
  worker.join();

  EXPECT_EQ(mainCapture.stats(), ShapingStats{});
  EXPECT_EQ(workerStats, (ShapingStats{.layoutCalls   = 1,
                                       .harfbuzzCalls = 1,
                                       .fallbackCalls = 1,
                                       .inputBytes    = 17}));
}

TEST(TextDiagnosticsTest, CompletedCaptureDoesNotPolluteNextCapture) {
  {
    ShapingStatsScope capture;
    gleditor::text::detail::recordLayout(19);
    EXPECT_EQ(capture.stats().layoutCalls, 1U);
  }
  gleditor::text::detail::recordLayout(100);
  ShapingStatsScope nextCapture;
  EXPECT_EQ(nextCapture.stats(), ShapingStats{});
}

} // namespace
