/**
 * @file universal_link_endpoint_test.cpp
 * @brief Unit tests for universal link endpoints, transclusion pairs, and
 * layout data structures bridging linear documents and 3D Zigzag cells.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "common/xanadu/universal_link_endpoint.hpp"
#include "xudu/core/link_layout.hpp"

namespace {

using xanadu::CompactTransclusionPair;
using xanadu::HalfLink;
using xanadu::LinkedPair;
using xanadu::LinkEnd;
using xanadu::LinkTargetKind;
using xanadu::LinkType;
using xanadu::PrimediaSpan;
using xanadu::ProminenceTier;
using xanadu::TransclusionPair;
using xanadu::UniversalHalfLink;
using xanadu::UniversalLinkedPair;
using xanadu::UniversalLinkEnd;
using xanadu::UniversalTransclusionPair;

// -- 1. ABI Layout & Size Assertions -----------------------------------------

TEST(UniversalLinkEndpointTest, abiSizeAndAlignmentRequirements) {
  // UniversalLinkEnd must be strictly 16 bytes, alignas(4)
  static_assert(sizeof(UniversalLinkEnd) == 16);
  static_assert(alignof(UniversalLinkEnd) == 4);
  EXPECT_EQ(sizeof(UniversalLinkEnd), 16U);
  EXPECT_EQ(alignof(UniversalLinkEnd), 4U);

  // UniversalTransclusionPair must be exactly 64 bytes (1 cache line),
  // alignas(8)
  static_assert(sizeof(UniversalTransclusionPair) == 64);
  static_assert(alignof(UniversalTransclusionPair) == 8);
  EXPECT_EQ(sizeof(UniversalTransclusionPair), 64U);
  EXPECT_EQ(alignof(UniversalTransclusionPair), 8U);

  // CompactTransclusionPair must be exactly 32 bytes, alignas(8)
  static_assert(sizeof(CompactTransclusionPair) == 32);
  static_assert(alignof(CompactTransclusionPair) == 8);
  EXPECT_EQ(sizeof(CompactTransclusionPair), 32U);
  EXPECT_EQ(alignof(CompactTransclusionPair), 8U);
}

// -- 2. Aggregate Initialization & Legacy Compatibility ----------------------

TEST(UniversalLinkEndpointTest, aggregateInitializationMatchesLegacyLinkEnd) {
  // Legacy code initializes LinkEnd{doc, start, end}
  const LinkEnd end{2U, 100U, 145U};

  EXPECT_EQ(end.doc, 2U);
  EXPECT_EQ(end.targetId, 2U);
  EXPECT_EQ(end.start, 100U);
  EXPECT_EQ(end.end, 145U);
  EXPECT_EQ(end.length(), 45U);
  EXPECT_EQ(end.spanIndex, 0U);
  EXPECT_EQ(end.kind, static_cast<std::uint8_t>(LinkTargetKind::Document));
  EXPECT_EQ(end.flags, 0U);

  EXPECT_TRUE(end.isDocument());
  EXPECT_FALSE(end.isCell());
  EXPECT_FALSE(end.isWithheld());
  EXPECT_FALSE(end.isEphemeral());
  EXPECT_EQ(end.cell(), zigzag::noCell);
}

TEST(UniversalLinkEndpointTest, memberUnionBidirectionalAccess) {
  UniversalLinkEnd end{};
  end.doc = 42U;
  EXPECT_EQ(end.targetId, 42U);

  end.targetId = 99U;
  EXPECT_EQ(end.doc, 99U);
}

TEST(UniversalLinkEndpointTest, factoryHelpersForDocumentAndCell) {
  const auto docEnd = UniversalLinkEnd::forDocument(3U, 50U, 90U);
  EXPECT_TRUE(docEnd.isDocument());
  EXPECT_FALSE(docEnd.isCell());
  EXPECT_EQ(docEnd.doc, 3U);
  EXPECT_EQ(docEnd.targetId, 3U);
  EXPECT_EQ(docEnd.start, 50U);
  EXPECT_EQ(docEnd.end, 90U);
  EXPECT_EQ(docEnd.length(), 40U);
  EXPECT_EQ(docEnd.cell(), zigzag::noCell);

  const auto cellEnd = UniversalLinkEnd::forCell(777U, 10U, 35U, 4U, 1U);
  EXPECT_FALSE(cellEnd.isDocument());
  EXPECT_TRUE(cellEnd.isCell());
  EXPECT_EQ(cellEnd.cell(), 777U);
  EXPECT_EQ(cellEnd.targetId, 777U);
  EXPECT_EQ(cellEnd.start, 10U);
  EXPECT_EQ(cellEnd.end, 35U);
  EXPECT_EQ(cellEnd.length(), 25U);
  EXPECT_EQ(cellEnd.spanIndex, 4U);
  EXPECT_TRUE(cellEnd.isWithheld());
  EXPECT_FALSE(cellEnd.isEphemeral());
}

// -- 3. Transclusion Pair Functionality ---------------------------------------

TEST(UniversalLinkEndpointTest,
     universalTransclusionPairFieldAccessAndAccessors) {
  const UniversalLinkEnd from = UniversalLinkEnd::forDocument(0U, 10U, 25U);
  const UniversalLinkEnd to   = UniversalLinkEnd::forCell(105U, 0U, 15U, 1U);
  const PrimediaSpan span{2U, 5000U, 15U};

  const UniversalTransclusionPair pair{
      .from      = from,
      .to        = to,
      .span      = span,
      ._reserved = 0U,
  };

  // Direct field access matching legacy code
  EXPECT_EQ(pair.from.doc, 0U);
  EXPECT_EQ(pair.from.start, 10U);
  EXPECT_EQ(pair.from.end, 25U);
  EXPECT_EQ(pair.to.cell(), 105U);
  EXPECT_EQ(pair.to.start, 0U);
  EXPECT_EQ(pair.to.end, 15U);
  EXPECT_EQ(pair.span.scroll, 2U);
  EXPECT_EQ(pair.span.start, 5000U);
  EXPECT_EQ(pair.span.length, 15U);

  // Helper accessors
  EXPECT_EQ(pair.fromTargetId(), 0U);
  EXPECT_EQ(pair.fromOffset(), 10U);
  EXPECT_EQ(pair.toTargetId(), 105U);
  EXPECT_EQ(pair.toOffset(), 0U);
  EXPECT_EQ(pair.length(), 15U);
  EXPECT_EQ(pair.scrollId(), 2U);
  EXPECT_EQ(pair.spanStart(), 5000U);

  // Equality
  const UniversalTransclusionPair pairCopy = pair;
  EXPECT_EQ(pair, pairCopy);
}

TEST(UniversalLinkEndpointTest, compactTransclusionPairRoundtrip) {
  const UniversalTransclusionPair uPair{
      .from      = UniversalLinkEnd::forDocument(1U, 20U, 60U),
      .to        = UniversalLinkEnd::forCell(404U, 5U, 45U),
      .span      = PrimediaSpan{3U, 1200U, 40U},
      ._reserved = 0U,
  };

  const auto compact = CompactTransclusionPair::fromUniversal(uPair);
  EXPECT_EQ(compact.fromTargetId, 1U);
  EXPECT_EQ(compact.fromOffset, 20U);
  EXPECT_EQ(compact.toTargetId, 404U);
  EXPECT_EQ(compact.toOffset, 5U);
  EXPECT_EQ(compact.length, 40U);
  EXPECT_EQ(compact.scrollId, 3U);
  EXPECT_EQ(compact.spanStart, 1200U);

  const auto reconstructed = compact.toUniversal();
  EXPECT_EQ(reconstructed.from.targetId, 1U);
  EXPECT_EQ(reconstructed.from.start, 20U);
  EXPECT_EQ(reconstructed.from.end, 60U);
  EXPECT_EQ(reconstructed.to.targetId, 404U);
  EXPECT_EQ(reconstructed.to.start, 5U);
  EXPECT_EQ(reconstructed.to.end, 45U);
  EXPECT_EQ(reconstructed.span, uPair.span);
}

// -- 4. LinkedPair and HalfLink Tests ----------------------------------------

TEST(UniversalLinkEndpointTest, linkedPairAndHalfLinkOperations) {
  const UniversalLinkEnd left  = UniversalLinkEnd::forDocument(0U, 5U, 15U);
  const UniversalLinkEnd right = UniversalLinkEnd::forCell(50U, 0U, 10U);

  const LinkedPair lp{
      .link = 1001U,
      .type = LinkType::Comment,
      .tier = ProminenceTier::Curated,
      .from = left,
      .to   = right,
  };

  EXPECT_EQ(lp.link, 1001U);
  EXPECT_EQ(lp.linkId(), 1001U);
  EXPECT_EQ(lp.type, LinkType::Comment);
  EXPECT_EQ(lp.tier, ProminenceTier::Curated);
  EXPECT_EQ(lp.from.doc, 0U);
  EXPECT_EQ(lp.to.cell(), 50U);

  const HalfLink hl{
      .link      = 2002U,
      .type      = LinkType::Illustration,
      .tier      = ProminenceTier::Author,
      .here      = left,
      .elsewhere = {PrimediaSpan{1U, 800U, 10U}},
  };

  EXPECT_EQ(hl.link, 2002U);
  EXPECT_EQ(hl.linkId(), 2002U);
  EXPECT_EQ(hl.type, LinkType::Illustration);
  EXPECT_EQ(hl.here.doc, 0U);
  ASSERT_EQ(hl.elsewhere.size(), 1U);
  EXPECT_EQ(hl.elsewhere[0].start, 800U);
}

} // namespace
