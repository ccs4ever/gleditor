/**
 * @file clickable_registry_test.cpp
 * @brief Unit tests for ClickableRegistry and compile-time static tag
 * auto-generation.
 */
#include <gleditor/clickable_registry.hpp>
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

} // namespace
} // namespace gleditor
