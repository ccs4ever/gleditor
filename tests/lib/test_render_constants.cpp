/**
 * @file test_render_constants.cpp
 * @brief Unit tests verifying strongly-typed render constants.
 */
#include <gtest/gtest.h>

#include <chrono>
#include <gleditor/render/constants.hpp>

namespace {

TEST(RenderConstantsTest, VerifyPhysicalAndTimingConstants) {
  EXPECT_GT(render::kDefaultNearClipZ, 0.0F);
  EXPECT_LT(render::kDefaultNearClipZ, 1.0F);

  EXPECT_GT(render::kDefaultFarClipZ, render::kDefaultNearClipZ);
  EXPECT_GE(render::kDefaultFarClipZ, 1000.0F);

  EXPECT_GT(render::kDefaultDocumentGap, 0.0F);

  EXPECT_GT(render::kNoPresentYieldDuration.count(), 0);
  EXPECT_LE(render::kNoPresentYieldDuration.count(), 50);
}

} // namespace
