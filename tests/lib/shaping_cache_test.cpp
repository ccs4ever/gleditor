#include <gleditor/text/diagnostics.hpp>
#include <gleditor/text/fit.hpp>
#include <gleditor/text/shaping_cache.hpp>
#include <gtest/gtest.h>

#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

using namespace gleditor;
using namespace gleditor::text;

namespace {

void expectSamePage(const PageShaping &actual, const PageShaping &expected) {
  EXPECT_EQ(actual.textWidthPx, expected.textWidthPx);
  EXPECT_EQ(actual.textHeightPx, expected.textHeightPx);
  EXPECT_EQ(actual.page, expected.page);
  EXPECT_EQ(actual.limit, expected.limit);
  EXPECT_EQ(actual.lineCount, expected.lineCount);
  ASSERT_EQ(actual.glyphs.size(), expected.glyphs.size());
  ASSERT_EQ(actual.clusters.size(), expected.clusters.size());
  ASSERT_EQ(actual.lines.size(), expected.lines.size());
  ASSERT_EQ(actual.boxes.size(), expected.boxes.size());
  for (std::size_t i = 0; i < actual.glyphs.size(); ++i) {
    const auto &a = actual.glyphs[i];
    const auto &b = expected.glyphs[i];
    EXPECT_EQ(a.chr, b.chr);
    EXPECT_EQ(a.clusterLeft, b.clusterLeft);
    EXPECT_EQ(a.clusterTop, b.clusterTop);
    EXPECT_EQ(a.clusterIndex, b.clusterIndex);
    EXPECT_EQ(a.lineIndex, b.lineIndex);
    EXPECT_EQ(a.decorations, b.decorations);
  }
  for (std::size_t i = 0; i < actual.clusters.size(); ++i) {
    EXPECT_EQ(actual.clusters[i].byteStart, expected.clusters[i].byteStart);
    EXPECT_EQ(actual.clusters[i].byteLength, expected.clusters[i].byteLength);
    EXPECT_EQ(actual.clusters[i].charCount, expected.clusters[i].charCount);
  }
  for (std::size_t i = 0; i < actual.lines.size(); ++i) {
    const auto &a = actual.lines[i];
    const auto &b = expected.lines[i];
    EXPECT_EQ(a.barWidth, b.barWidth);
    EXPECT_EQ(a.barHeight, b.barHeight);
    EXPECT_EQ(a.left, b.left);
    EXPECT_EQ(a.top, b.top);
    EXPECT_EQ(a.byteStart, b.byteStart);
    EXPECT_EQ(a.byteLength, b.byteLength);
  }
  for (std::size_t i = 0; i < actual.boxes.size(); ++i) {
    const auto &a = actual.boxes[i];
    const auto &b = expected.boxes[i];
    EXPECT_EQ(a.anchorByteOffset, b.anchorByteOffset);
    EXPECT_EQ(a.id, b.id);
    EXPECT_EQ(a.left, b.left);
    EXPECT_EQ(a.top, b.top);
    EXPECT_EQ(a.width, b.width);
    EXPECT_EQ(a.height, b.height);
    EXPECT_EQ(a.placement, b.placement);
  }
}

} // namespace

TEST(CoreShapingCacheTest, WarmPageAndFitAvoidAllShaping) {
  ShapingCache cache;
  const auto font = FontManager::instance().getFont("Sans 16");
  const std::string text =
      "Arabic العربية, combining a\u0301, emoji 👩‍💻";
  const LayoutOptions options{.maxWidthPx = 190.0F, .maxHeightPx = 300.0F};
  const TextFit constraints{.maxWidthPx = 130.0F};
  const auto expectedPage = TextLayout::layoutPage(text, font, options);
  const auto expectedFit  = fit(text, font, constraints);
  expectSamePage(cache.page(text, font, options), expectedPage);
  const auto firstFit = fit(text, font, constraints, &cache);
  expectSamePage(firstFit.shaping, expectedFit.shaping);

  ShapingStatsScope capture;
  expectSamePage(cache.page(text, font, options), expectedPage);
  const auto warmFit = fit(text, font, constraints, &cache);
  expectSamePage(warmFit.shaping, expectedFit.shaping);
  EXPECT_EQ(warmFit.truncated, expectedFit.truncated);
  EXPECT_EQ(warmFit.visibleBytes, expectedFit.visibleBytes);
  EXPECT_EQ(capture.stats().layoutCalls, 0U);
  EXPECT_EQ(capture.stats().harfbuzzCalls, 0U);
  EXPECT_EQ(cache.stats().entries, 2U);
  EXPECT_EQ(cache.stats().hits, 2U);
  EXPECT_EQ(cache.stats().misses, 2U);
}

TEST(CoreShapingCacheTest, ChangedLayoutProducesItsOwnPage) {
  ShapingCache cache;
  const auto font        = FontManager::instance().getFont("Monospace 16");
  const std::string text = "first paragraph\nsecond paragraph with more words";
  LayoutOptions options{.maxWidthPx = 300.0F, .maxHeightPx = 300.0F};
  const auto check = [&] {
    expectSamePage(cache.page(text, font, options),
                   TextLayout::layoutPage(text, font, options));
  };
  check();
  options.maxWidthPx = 100.0F;
  check();
  options.maxHeightPx = font->metrics().lineHeight;
  check();
  options.singleParagraph = true;
  check();
  options.decoratedRanges = {
      {.start = 0, .end = 5, .decorations = decorationBit(Decoration::Bold)}};
  check();
  options.blockStyles = {{.start = 0, .end = 50, .align = TextAlign::Right}};
  check();
  options.page = {.mode     = PageSizing::Fixed,
                  .widthPx  = 700.0F,
                  .heightPx = 800.0F,
                  .marginPx = 24.0F};
  check();
  EXPECT_EQ(cache.stats().misses, 7U);
  EXPECT_EQ(cache.stats().hits, 0U);
  std::ignore           = cache.page(text + "!", font, options);
  const auto largerFont = FontManager::instance().getFont("Monospace 20");
  expectSamePage(cache.page(text, largerFont, options),
                 TextLayout::layoutPage(text, largerFont, options));
  EXPECT_EQ(cache.stats().misses, 9U);
}

TEST(CoreShapingCacheTest, FittingConstraintsRemainIndependent) {
  ShapingCache cache;
  const auto font        = FontManager::instance().getFont("Sans 16");
  const std::string text = "first line with words\nsecond line with words";
  TextFit constraints{.maxWidthPx = 100.0F};
  const auto check = [&] {
    const auto &cached = cache.fitted(text, font, constraints);
    const auto direct  = fit(text, font, constraints);
    expectSamePage(cached.shaping, direct.shaping);
    EXPECT_EQ(cached.truncated, direct.truncated);
    EXPECT_EQ(cached.visibleBytes, direct.visibleBytes);
  };
  check();
  constraints.maxWidthPx = 150.0F;
  check();
  constraints.maxHeightPx = font->metrics().lineHeight * 2.0F;
  check();
  constraints.maxLines = 2;
  check();
  constraints.overflow = Overflow::Wrap;
  check();
  constraints.at = EllipsisAt::Start;
  check();
  constraints.at = EllipsisAt::Middle;
  check();
  constraints.align = TextAlign::Right;
  check();
  constraints.overflow = Overflow::Clip;
  check();
  EXPECT_EQ(cache.stats().misses, 9U);
  EXPECT_EQ(cache.stats().entries, 9U);
}

TEST(CoreShapingCacheTest, ChangedMediaReservationIsNotReused) {
  ShapingCache cache;
  const auto font        = FontManager::instance().getFont("Sans 16");
  const std::string text = "\uFFFCwords after media";
  LayoutOptions options{.maxWidthPx = 300.0F, .maxHeightPx = 400.0F};
  options.boxes = {{.anchor = 0, .widthPx = 40.0F, .heightPx = 50.0F, .id = 1}};
  expectSamePage(cache.page(text, font, options),
                 TextLayout::layoutPage(text, font, options));
  options.boxes.front().marginPx = 20.0F;
  options.boxes.front().id       = 2;
  expectSamePage(cache.page(text, font, options),
                 TextLayout::layoutPage(text, font, options));
  options.boxes.front().placement = BoxPlacement::FloatRight;
  expectSamePage(cache.page(text, font, options),
                 TextLayout::layoutPage(text, font, options));
  EXPECT_EQ(cache.stats().misses, 3U);
}

TEST(CoreShapingCacheTest, LeastRecentlyUsedEntryIsEvictedAcrossResultKinds) {
  ShapingCache cache(2);
  const auto font = FontManager::instance().getFont("Sans 16");
  const LayoutOptions options{};
  std::ignore = cache.page("retained", font, options);
  std::ignore = cache.page("oldest", font, options);
  std::ignore = cache.page("retained", font, options);
  std::ignore = cache.fitted("new", font, TextFit{});
  EXPECT_EQ(cache.stats().entries, 2U);
  EXPECT_EQ(cache.stats().evictions, 1U);
  {
    ShapingStatsScope capture;
    std::ignore = cache.page("retained", font, options);
    EXPECT_EQ(capture.stats().layoutCalls, 0U);
  }
  {
    ShapingStatsScope capture;
    std::ignore = cache.page("oldest", font, options);
    EXPECT_GT(capture.stats().layoutCalls, 0U);
  }
  EXPECT_EQ(cache.stats().evictions, 2U);
}

TEST(CoreShapingCacheTest, EntriesKeepFontOwnersUntilClear) {
  ShapingCache cache;
  const auto original = FontManager::instance().getFont("Sans 16");
  // A distinct owner isolates the cache's ownership from FontManager's own
  // retained entry while sharing its valid FreeType/HarfBuzz face.
  FontFacePtr font(original.get(), [owner = original](FontFace *) {});
  const std::weak_ptr<FontFace> owner = font;
  std::ignore = cache.page("owned", font, LayoutOptions{});
  font.reset();
  EXPECT_FALSE(owner.expired());
  cache.clear();
  EXPECT_TRUE(owner.expired());
  EXPECT_EQ(cache.stats().entries, 0U);
  EXPECT_EQ(cache.stats().misses, 1U);
}

TEST(CoreShapingCacheTest, MovePreservesEntriesAndRecency) {
  ShapingCache original(2);
  const auto font = FontManager::instance().getFont("Sans 16");
  std::ignore     = original.page("move me", font, LayoutOptions{});
  ShapingCache moved(std::move(original));
  ShapingStatsScope capture;
  std::ignore = moved.page("move me", font, LayoutOptions{});
  EXPECT_EQ(capture.stats().layoutCalls, 0U);
  EXPECT_EQ(moved.stats().hits, 1U);
}

TEST(CoreShapingCacheTest, ZeroCapacityIsRefused) {
  EXPECT_THROW(ShapingCache(0), std::invalid_argument);
}

TEST(CoreShapingCacheTest, NonFiniteKeysCannotEscapeTheCapacityBudget) {
  ShapingCache cache(1);
  const auto font = FontManager::instance().getFont("Sans 16");
  const float nan = std::numeric_limits<float>::quiet_NaN();
  LayoutOptions options;
  options.maxWidthPx = nan;
  EXPECT_THROW((void)cache.page("invalid", font, options),
               std::invalid_argument);
  options.maxWidthPx = 100.0F;
  options.boxes      = {{.widthPx = nan}};
  EXPECT_THROW((void)cache.page("invalid box", font, options),
               std::invalid_argument);
  EXPECT_THROW((void)cache.fitted("invalid fit", font, {.maxHeightPx = nan}),
               std::invalid_argument);
  EXPECT_EQ(cache.stats().entries, 0U);
  EXPECT_EQ(cache.stats().misses, 0U);
}
