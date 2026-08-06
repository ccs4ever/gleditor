#include <gtest/gtest.h>

#include <gleditor/render/types.hpp>

#include <gmock/gmock.h>
#include <optional>

#include "mocks/device.hpp"

using render::PickingResult;
using render::PickingTag;
using testing::NiceMock;
using testing::Return;

// Nothing drawn at a pixel leaves the picking attachment at its cleared value,
// which callers need to tell apart from a real identity.
TEST(Picking, emptyTagIsTheClearedValue) {
  EXPECT_TRUE(PickingTag{}.empty());
  EXPECT_TRUE((PickingTag{0, 0}).empty());
  EXPECT_FALSE((PickingTag{2, 1}).empty());
  // A glyph at text offset zero is a real hit even though the index is zero.
  EXPECT_FALSE((PickingTag{3, 0}).empty());
}

TEST(Picking, tagsCompareByBothFields) {
  EXPECT_EQ((PickingTag{3, 27}), (PickingTag{3, 27}));
  EXPECT_NE((PickingTag{3, 27}), (PickingTag{3, 28}));
  EXPECT_NE((PickingTag{2, 27}), (PickingTag{3, 27}));
}

// The readback is asynchronous, so a result has to name the pixel it came from;
// attributing it to wherever the cursor happens to be now would be wrong.
TEST(Picking, resultCarriesTheQueriedPixel) {
  NiceMock<MockRenderDevice> device;
  ON_CALL(device, takePickingTag)
      .WillByDefault(Return(std::optional{PickingResult{12, 34, {3, 27}}}));

  const auto result = device.takePickingTag();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->x, 12);
  EXPECT_EQ(result->y, 34);
  EXPECT_EQ(result->tag, (PickingTag{3, 27}));
}

TEST(Picking, noResultYetIsNotAnEmptyTag) {
  NiceMock<MockRenderDevice> device;
  ON_CALL(device, takePickingTag)
      .WillByDefault(Return(std::optional<PickingResult>{}));

  // "not ready" and "ready, nothing there" are different answers: the first
  // means poll again, the second means the pixel really is empty.
  EXPECT_FALSE(device.takePickingTag().has_value());
}

// vi: set sw=2 sts=2 ts=2 et:
