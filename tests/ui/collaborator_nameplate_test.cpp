#include "common/ui/xanadoc/collaborator_overlay.hpp"
#include <gleditor/text/diagnostics.hpp>
#include <gtest/gtest.h>
#include <limits>

namespace {
using namespace gleditor::ui;
void contains(Rect outer, Rect inner) {
  EXPECT_GE(inner.left, outer.left - .01F);
  EXPECT_GE(inner.bottom, outer.bottom - .01F);
  EXPECT_LE(inner.left + inner.width, outer.left + outer.width + .01F);
  EXPECT_LE(inner.bottom + inner.height, outer.bottom + outer.height + .01F);
}
TEST(CollaboratorNameplateTest, UnicodeNamesFitLiveTypographyAndSafeViewport) {
  const std::string name =
      "A long author name 世界 مرحبا 👨‍👩‍👧‍👦 é "
      "with a suffix";
  const std::string identity = "0123456789abcdef0123456789abcdef01234567";
  for (const auto size : {Size{640, 480}, Size{1280, 800}, Size{2560, 1440}})
    for (float scale : {.8F, 1.F, 1.5F, 2.F})
      for (float content : {1.F, 1.25F, 2.F})
        for (const auto *family :
             {"Sans", "Serif", "Monospace", "Noto Sans CJK JP"}) {
          SCOPED_TRACE(std::string{family} + "/" + std::to_string(scale));
          UiMetrics metrics{.contentScale = content,
                            .fontScale    = scale,
                            .screenWidth  = static_cast<int>(size.width),
                            .screenHeight = static_cast<int>(size.height)};
          Theme theme;
          theme.fonts[static_cast<std::size_t>(FontRole::Caption)].family =
              family;
          xanadu::CollaboratorNameplate plate;
          const auto &fit =
              plate.prepare(name, identity, {size.width - 1, size.height - 1},
                            metrics, theme);
          EXPECT_EQ(fit.accessibleLabel, name + " ✦ " + identity);
          EXPECT_EQ(fit.fontDescription,
                    metrics.fontDescription(FontRole::Caption, theme));
          contains(metrics.pixelSafeArea(), fit.bounds);
          contains(fit.bounds, fit.content);
          EXPECT_LE(fit.fitted.widthPx, fit.content.width + .01F);
          EXPECT_LE(fit.fitted.heightPx, fit.content.height + .01F);
          EXPECT_GT(fit.bounds.height, 0);
          EXPECT_GT(fit.fitted.lines, 0);
        }
}
TEST(CollaboratorNameplateTest,
     MovingAnchorsReuseShapingAndLiveFontsInvalidate) {
  xanadu::CollaboratorNameplate plate;
  UiMetrics metrics{.screenWidth = 640, .screenHeight = 480};
  Theme theme;
  const auto firstHeight =
      plate.prepare("Author 世界", "identity", {10, 10}, metrics, theme)
          .bounds.height;
  gleditor::text::ShapingStatsScope capture;
  for (int i = 0; i < 100; ++i)
    static_cast<void>(plate.prepare("Author 世界", "identity",
                                    {static_cast<float>(i), 20}, metrics,
                                    theme));
  EXPECT_EQ(capture.stats().layoutCalls, 0U);
  EXPECT_EQ(capture.stats().harfbuzzCalls, 0U);
  metrics.fontScale = 2;
  const auto &large =
      plate.prepare("Author 世界", "identity", {10, 10}, metrics, theme);
  EXPECT_GT(large.bounds.height, firstHeight);
  EXPECT_LE(large.fitted.heightPx, large.content.height + .01F);
}
TEST(CollaboratorNameplateTest, InvalidAnchorsAndTinyViewportsStayBounded) {
  xanadu::CollaboratorNameplate plate;
  UiMetrics metrics{.screenWidth = 640, .screenHeight = 480};
  const Theme theme;
  const auto &invalid =
      plate.prepare("First", "id", {std::numeric_limits<float>::infinity(), 0},
                    metrics, theme);
  EXPECT_EQ(invalid.bounds.width, 0);
  const auto &valid = plate.prepare("First", "id", {0, 0}, metrics, theme);
  EXPECT_GT(valid.fitted.lines, 0);
  metrics.screenWidth = metrics.screenHeight = 1;
  const auto &tiny =
      plate.prepare("Changed 世界", "id", {-100, 100}, metrics, theme);
  EXPECT_EQ(tiny.accessibleLabel, "Changed 世界 ✦ id");
  contains(metrics.pixelSafeArea(), tiny.bounds);
  contains(tiny.bounds, tiny.content);
  EXPECT_LE(tiny.fitted.widthPx, tiny.content.width + .01F);
  EXPECT_LE(tiny.fitted.heightPx, tiny.content.height + .01F);
}
} // namespace
