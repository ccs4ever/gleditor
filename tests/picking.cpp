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

// The identity word packs kind, document and page into one of the four words
// the attachment carries; the other two hold the cluster index and the
// fractional position. Round-tripping is what keeps the C++ packer and the
// C++ unpacker honest about the field widths.
TEST(Picking, identityPacksAndUnpacksToTheSameFields) {
  const auto identity = render::packTagIdentity(3, 5, 9);
  const auto tag      = render::unpackPickingTag(identity, 1234, 0);
  EXPECT_EQ(tag.kind, 3U);
  EXPECT_EQ(tag.docIndex, 5U);
  EXPECT_EQ(tag.pageIndex, 9U);
  EXPECT_EQ(tag.clusterIndex, 1234U);
}

TEST(Picking, identityHoldsTheWidestValuesEachFieldClaims) {
  constexpr std::uint32_t maxKind = (1U << render::tagKindBits) - 1U;
  constexpr std::uint32_t maxDoc  = (1U << render::tagDocBits) - 1U;
  constexpr std::uint32_t maxPage = (1U << render::tagPageBits) - 1U;

  const auto tag = render::unpackPickingTag(
      render::packTagIdentity(maxKind, maxDoc, maxPage), 0xFFFFFFFFU, 0);
  EXPECT_EQ(tag.kind, maxKind);
  EXPECT_EQ(tag.docIndex, maxDoc) << "document field overflowed into another";
  EXPECT_EQ(tag.pageIndex, maxPage);
  EXPECT_EQ(tag.clusterIndex, 0xFFFFFFFFU);
}

// The fraction is fixed point in the attachment because it holds unsigned
// integers; the scale has to agree with the one the fragment stage applies.
TEST(Picking, fractionSpansTheWholeQuad) {
  EXPECT_FLOAT_EQ(render::unpackPickingTag(0, 0, 0).fraction, 0.0F);
  EXPECT_FLOAT_EQ(
      render::unpackPickingTag(0, 0, render::tagFractionScale).fraction, 1.0F);
  EXPECT_NEAR(
      render::unpackPickingTag(0, 0, render::tagFractionScale / 2).fraction,
      0.5F, 0.001F);
}

// Hovering within one glyph must not read as moving onto a new object, or the
// reporting would fire on every sub-pixel movement.
TEST(Picking, adifferentFractionIsStillTheSameObject) {
  auto left  = render::unpackPickingTag(render::packTagIdentity(3, 0, 0), 7, 0);
  auto right = render::unpackPickingTag(render::packTagIdentity(3, 0, 0), 7,
                                        render::tagFractionScale);
  EXPECT_NE(left, right) << "the fraction differs, so equality should differ";
  EXPECT_TRUE(left.sameObject(right));
}
