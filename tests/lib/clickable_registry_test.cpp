/**
 * @file clickable_registry_test.cpp
 * @brief Unit tests for ClickableRegistry and compile-time static tag
 * auto-generation across tagKindOverlay, tagKindPage, and tagKindGlyph.
 */
#include <gleditor/clickable_registry.hpp>
#include <gleditor/render/types.hpp>
#include <gtest/gtest.h>

#include <string>

namespace gleditor {
namespace {

TEST(ClickableRegistryTest, CompileTimeStaticTagGeneration) {
  constexpr auto tagPlay  = StaticSubTag<"play">::offset;
  constexpr auto tagPause = StaticSubTag<"pause">::offset;
  constexpr auto tagStop  = StaticSubTag<"stop">::offset;
  constexpr auto tagSpeed = StaticSubTag<"speed">::offset;

  EXPECT_GE(tagPlay, 1U);
  EXPECT_LE(tagPlay, 99U);

  EXPECT_NE(tagPlay, tagPause);
  EXPECT_NE(tagPlay, tagStop);
  EXPECT_NE(tagPlay, tagSpeed);
  EXPECT_NE(tagPause, tagStop);
}

TEST(ClickableRegistryTest, StaticTagMultiKind) {
  constexpr auto overlayKind = StaticOverlayTag<"btn">::kind;
  constexpr auto pageKind    = StaticPageTag<"page_turn">::kind;
  constexpr auto glyphKind   = StaticGlyphTag<"link">::kind;

  EXPECT_EQ(overlayKind, render::tagKindOverlay);
  EXPECT_EQ(pageKind, render::tagKindPage);
  EXPECT_EQ(glyphKind, render::tagKindGlyph);

  constexpr auto overlayOffset = StaticOverlayTag<"btn">::offset;
  constexpr auto pageOffset    = StaticPageTag<"btn">::offset;
  EXPECT_EQ(overlayOffset, pageOffset); // Same hash, different kind
}

TEST(ClickableRegistryTest, RegisterExplicitAndDispatch) {
  ClickableRegistry registry;
  int playCalls = 0;
  int stopCalls = 0;

  registry.registerControl("play", 1U, "▶", "Play", [&] { playCalls++; });
  registry.registerControl("stop", 3U, "⏹", "Stop", [&] { stopCalls++; });

  EXPECT_TRUE(registry.has(1U));
  EXPECT_TRUE(registry.has(3U));
  EXPECT_FALSE(registry.has(2U));

  EXPECT_TRUE(registry.dispatch(1U));
  EXPECT_EQ(1, playCalls);
  EXPECT_EQ(0, stopCalls);

  EXPECT_TRUE(registry.dispatch(3U));
  EXPECT_EQ(1, playCalls);
  EXPECT_EQ(1, stopCalls);

  EXPECT_FALSE(registry.dispatch(99U));
}

TEST(ClickableRegistryTest, RegisterAutoAndDispatch) {
  ClickableRegistry registry;
  bool clicked = false;

  registry.registerAuto<"custom">("Label", "A11y Label",
                                  [&] { clicked = true; });

  constexpr auto expectedTag = StaticSubTag<"custom">::offset;
  EXPECT_TRUE(registry.has(expectedTag));
  EXPECT_TRUE(registry.dispatch(expectedTag));
  EXPECT_TRUE(clicked);
}

TEST(ClickableRegistryTest, DynamicLabels) {
  ClickableRegistry registry;
  bool muted = false;

  registry.registerControl(
      "volume", 4U, [&] { return muted ? "🔈" : "🔊"; },
      [&] { return muted ? "Unmute" : "Mute"; }, [&] { muted = !muted; });

  const auto *ctrl = registry.find(4U);
  ASSERT_NE(nullptr, ctrl);
  EXPECT_EQ("🔊", ctrl->getLabel());
  EXPECT_EQ("Mute", ctrl->getA11yLabel());

  registry.dispatch(4U);
  EXPECT_TRUE(muted);
  EXPECT_EQ("🔈", ctrl->getLabel());
  EXPECT_EQ("Unmute", ctrl->getA11yLabel());
}

TEST(ClickableRegistryTest, OverlayDispatchWithTagBase) {
  ClickableRegistry registry;
  registry.setTagBase(0x8000U);
  EXPECT_EQ(0x8000U, registry.tagBase());

  bool playClicked = false;
  registry.registerControl("play", 1U, "▶", "Play",
                           [&] { playClicked = true; });

  render::PickingResult pick;
  pick.x                = 100;
  pick.y                = 200;
  pick.tag.kind         = render::tagKindOverlay;
  pick.tag.clusterIndex = 0x8000U + 1U; // tagBase + offset

  EXPECT_TRUE(registry.dispatch(pick));
  EXPECT_TRUE(playClicked);

  // Unregistered overlay offset
  pick.tag.clusterIndex = 0x8000U + 2U;
  EXPECT_FALSE(registry.dispatch(pick));
}

TEST(ClickableRegistryTest, PageControlDispatch) {
  ClickableRegistry registry;
  bool page0Clicked = false;
  bool page1Clicked = false;

  // Specific doc 0, page 0
  registry.registerPageControl("cover_page", 0U, 0U,
                               [&] { page0Clicked = true; });

  // Specific doc 0, page 1
  registry.registerPageControl("content_page", 0U, 1U,
                               [&] { page1Clicked = true; });

  render::PickingResult pick;
  pick.tag.kind      = render::tagKindPage;
  pick.tag.docIndex  = 0U;
  pick.tag.pageIndex = 0U;

  EXPECT_TRUE(registry.dispatch(pick));
  EXPECT_TRUE(page0Clicked);
  EXPECT_FALSE(page1Clicked);

  pick.tag.pageIndex = 1U;
  EXPECT_TRUE(registry.dispatch(pick));
  EXPECT_TRUE(page1Clicked);

  // Different document: doc 1, page 0
  pick.tag.docIndex  = 1U;
  pick.tag.pageIndex = 0U;
  EXPECT_FALSE(registry.dispatch(pick));
}

TEST(ClickableRegistryTest, GlyphControlAndSpanDispatch) {
  ClickableRegistry registry;
  bool wordClicked = false;
  bool linkClicked = false;

  // Single glyph cluster at index 42
  registry.registerGlyphControl(
      "single_word", 42U, [&] { wordClicked = true; }, 0U, 0U);

  // Multi-cluster span covering [100, 150)
  registry.registerGlyphSpan(
      "hyperlink", 100U, 150U, [&] { linkClicked = true; }, 0U, 0U);

  render::PickingTag tag;
  tag.kind         = render::tagKindGlyph;
  tag.docIndex     = 0U;
  tag.pageIndex    = 0U;
  tag.clusterIndex = 42U;

  EXPECT_TRUE(registry.dispatch(tag));
  EXPECT_TRUE(wordClicked);

  // Pick within span [100, 150)
  tag.clusterIndex = 125U;
  EXPECT_TRUE(registry.dispatch(tag));
  EXPECT_TRUE(linkClicked);

  // Pick at span start
  linkClicked      = false;
  tag.clusterIndex = 100U;
  EXPECT_TRUE(registry.dispatch(tag));
  EXPECT_TRUE(linkClicked);

  // Pick at span end (exclusive bound -> false)
  linkClicked      = false;
  tag.clusterIndex = 150U;
  EXPECT_FALSE(registry.dispatch(tag));
  EXPECT_FALSE(linkClicked);

  // Outside both
  tag.clusterIndex = 99U;
  EXPECT_FALSE(registry.dispatch(tag));
}

TEST(ClickableRegistryTest, RichPickingCallbacks) {
  ClickableRegistry registry;
  float recordedFraction = 0.0F;
  int recordedX          = 0;
  int recordedY          = 0;
  std::uint32_t recDoc   = 99;
  std::uint32_t recPage  = 99;

  registry.registerGlyphControl(
      "rich_glyph", 50U,
      [&](const render::PickingTag &tag) {
        recordedFraction = tag.fraction;
        recDoc           = tag.docIndex;
        recPage          = tag.pageIndex;
      },
      std::nullopt, std::nullopt);

  ClickableControl customPick;
  customPick.id           = "custom_pick";
  customPick.tagKind      = render::tagKindGlyph;
  customPick.tagOffset    = 60U;
  customPick.tagEndOffset = 61U;
  customPick.onPick       = [&](const render::PickingResult &pick) {
    recordedX = pick.x;
    recordedY = pick.y;
  };
  registry.registerControl(std::move(customPick));

  render::PickingResult res;
  res.x                = 320;
  res.y                = 240;
  res.tag.kind         = render::tagKindGlyph;
  res.tag.docIndex     = 2U;
  res.tag.pageIndex    = 5U;
  res.tag.clusterIndex = 50U;
  res.tag.fraction     = 0.75F;

  EXPECT_TRUE(registry.dispatch(res));
  EXPECT_FLOAT_EQ(0.75F, recordedFraction);
  EXPECT_EQ(2U, recDoc);
  EXPECT_EQ(5U, recPage);

  res.tag.clusterIndex = 60U;
  EXPECT_TRUE(registry.dispatch(res));
  EXPECT_EQ(320, recordedX);
  EXPECT_EQ(240, recordedY);
}

TEST(ClickableRegistryTest, MultiKindSeparation) {
  ClickableRegistry registry;
  int overlayHits = 0;
  int pageHits    = 0;
  int glyphHits   = 0;

  // Same offset (10) registered across three different kinds
  registry.registerControl("overlay_10", 10U, "O", "Overlay",
                           [&] { overlayHits++; });
  registry.registerPageControl("page_10", std::nullopt, 10U,
                               [&] { pageHits++; });
  registry.registerGlyphControl("glyph_10", 10U, [&] { glyphHits++; });

  render::PickingTag tag;
  tag.clusterIndex = 10U;
  tag.pageIndex    = 10U;

  // 1. Overlay tag
  tag.kind = render::tagKindOverlay;
  EXPECT_TRUE(registry.dispatch(tag));
  EXPECT_EQ(1, overlayHits);
  EXPECT_EQ(0, pageHits);
  EXPECT_EQ(0, glyphHits);

  // 2. Page tag
  tag.kind = render::tagKindPage;
  EXPECT_TRUE(registry.dispatch(tag));
  EXPECT_EQ(1, overlayHits);
  EXPECT_EQ(1, pageHits);
  EXPECT_EQ(0, glyphHits);

  // 3. Glyph tag
  tag.kind = render::tagKindGlyph;
  EXPECT_TRUE(registry.dispatch(tag));
  EXPECT_EQ(1, overlayHits);
  EXPECT_EQ(1, pageHits);
  EXPECT_EQ(1, glyphHits);

  // 4. Kind controls filtering
  EXPECT_EQ(1U, registry.controlsForKind(render::tagKindOverlay).size());
  EXPECT_EQ(1U, registry.controlsForKind(render::tagKindPage).size());
  EXPECT_EQ(1U, registry.controlsForKind(render::tagKindGlyph).size());
  EXPECT_EQ(0U, registry.controlsForKind(render::tagKindBeam).size());
}

} // namespace
} // namespace gleditor
