#include <gtest/gtest.h>

#include <gleditor/svg_animator.hpp>

#include <cstdint>
#include <string_view>
#include <vector>

namespace gleditor {
namespace {

TEST(SvgAnimatorTest, IsAnimatedDetection) {
  const std::string_view staticSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100" viewBox="0 0 100 100">
           <rect x="0" y="0" width="50" height="50" fill="#ff0000"/>
         </svg>)";
  EXPECT_FALSE(SvgAnimator::isAnimated(
      {reinterpret_cast<const std::uint8_t *>(staticSvg.data()),
       staticSvg.size()}));

  const std::string_view animatedSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100" viewBox="0 0 100 100">
           <rect x="0" y="0" width="50" height="50" fill="#ff0000">
             <animate attributeName="x" from="0" to="50" dur="2s" repeatCount="indefinite"/>
           </rect>
         </svg>)";
  EXPECT_TRUE(SvgAnimator::isAnimated(
      {reinterpret_cast<const std::uint8_t *>(animatedSvg.data()),
       animatedSvg.size()}));

  const std::string_view transformSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100" viewBox="0 0 100 100">
           <rect x="25" y="25" width="50" height="50" fill="#00ff00">
             <animateTransform attributeName="transform" type="rotate" from="0 50 50" to="360 50 50" dur="4s"/>
           </rect>
         </svg>)";
  EXPECT_TRUE(SvgAnimator::isAnimated(
      {reinterpret_cast<const std::uint8_t *>(transformSvg.data()),
       transformSvg.size()}));

  EXPECT_FALSE(SvgAnimator::isAnimated({}));
}

TEST(SvgAnimatorTest, StaticSvgReturnsNullOnLoad) {
  const std::string_view staticSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64" viewBox="0 0 64 64">
           <rect x="0" y="0" width="64" height="64" fill="#0000ff"/>
         </svg>)";
  auto animator = SvgAnimator::load(
      {reinterpret_cast<const std::uint8_t *>(staticSvg.data()),
       staticSvg.size()});
  EXPECT_EQ(animator, nullptr);
}

TEST(SvgAnimatorTest, RenderAnimatedRectMovement) {
  const std::string_view animatedSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100" viewBox="0 0 100 100">
           <rect x="0" y="0" width="20" height="20" fill="#ff0000">
             <animate attributeName="x" from="0" to="80" dur="2s" repeatCount="indefinite"/>
           </rect>
         </svg>)";
  auto animator = SvgAnimator::load(
      {reinterpret_cast<const std::uint8_t *>(animatedSvg.data()),
       animatedSvg.size()});
  ASSERT_NE(animator, nullptr);

  EXPECT_EQ(animator->width(), 100);
  EXPECT_EQ(animator->height(), 100);
  EXPECT_FLOAT_EQ(animator->duration(), 2.0F);

  // At t = 0.0s: rect is at x in [0, 20]
  std::vector<std::uint32_t> frame0;
  ASSERT_TRUE(animator->renderFrame(0.0F, frame0));
  ASSERT_EQ(frame0.size(), 100U * 100U);
  // Pixel (10, 10) should be colored
  EXPECT_NE(frame0[10 * 100 + 10], 0U);
  // Pixel (50, 10) should be transparent
  EXPECT_EQ(frame0[10 * 100 + 50], 0U);

  // At t = 1.0s (midpoint of 2s duration): rect moved to x in [40, 60]
  std::vector<std::uint32_t> frame1;
  ASSERT_TRUE(animator->renderFrame(1.0F, frame1));
  ASSERT_EQ(frame1.size(), 100U * 100U);
  // Pixel (10, 10) should now be transparent
  EXPECT_EQ(frame1[10 * 100 + 10], 0U);
  // Pixel (50, 10) should now be colored
  EXPECT_NE(frame1[10 * 100 + 50], 0U);
}

TEST(SvgAnimatorTest, RenderAnimatedTransformRotation) {
  const std::string_view transformSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100" viewBox="0 0 100 100">
           <rect x="40" y="10" width="20" height="40" fill="#0000ff">
             <animateTransform attributeName="transform" type="rotate" from="0 50 50" to="360 50 50" dur="4s"/>
           </rect>
         </svg>)";
  auto animator = SvgAnimator::load(
      {reinterpret_cast<const std::uint8_t *>(transformSvg.data()),
       transformSvg.size()});
  ASSERT_NE(animator, nullptr);
  EXPECT_FLOAT_EQ(animator->duration(), 4.0F);

  std::vector<std::uint32_t> frame0;
  ASSERT_TRUE(animator->renderFrame(0.0F, frame0));
  std::vector<std::uint32_t> frame1;
  ASSERT_TRUE(animator->renderFrame(1.0F, frame1));

  // Frames at t=0s and t=1s (90 deg rotation) should have different pixels
  EXPECT_NE(frame0, frame1);
}

TEST(SvgAnimatorTest, DetectLottieViaThorvgAnimationContext) {
  const std::string_view lottieJson =
      R"({"v":"5.5.2","fr":30,"ip":0,"op":60,"w":100,"h":100,"nm":"Test","ddd":0,"assets":[],"layers":[]})";
  EXPECT_TRUE(SvgAnimator::isAnimated(
      {reinterpret_cast<const std::uint8_t *>(lottieJson.data()),
       lottieJson.size()}));

  auto animator = SvgAnimator::load(
      {reinterpret_cast<const std::uint8_t *>(lottieJson.data()),
       lottieJson.size()});
  ASSERT_NE(animator, nullptr);
  EXPECT_FLOAT_EQ(animator->duration(), 2.0F);
  EXPECT_EQ(animator->width(), 100);
  EXPECT_EQ(animator->height(), 100);
}

} // namespace
} // namespace gleditor
