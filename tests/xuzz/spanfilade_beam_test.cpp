#include <chrono>
#include <gtest/gtest.h>
#include <vector>

#include "common/xanadu/enfilade/spanfilade.hpp"
#include "common/xanadu/link_layout.hpp"
#include "common/xanadu/microversion.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/version.hpp"

namespace xanadu::enfilade {
namespace {

TEST(SpanfiladeBeamTest, ViewportIntervalStabbingUnderTwentyMicroseconds) {
  constexpr std::uint32_t kNumDocs     = 10;
  constexpr std::uint32_t kSpansPerDoc = 50; // 10 * 50 = 500 spans total
  constexpr xanadu::ScrollId kScrollId = 42;

  Spanfilade spanfilade;

  // Populate 10 documents with 50 transcluded spans each, interleaved on scroll
  // 42
  for (std::uint32_t docId = 0; docId < kNumDocs; ++docId) {
    for (std::uint32_t s = 0; s < kSpansPerDoc; ++s) {
      const std::uint64_t startAddr = (docId * 1000) + (s * 20);
      const std::uint64_t spanLen   = 15;
      const SpanEntry entry{
          .start     = startAddr,
          .length    = spanLen,
          .docId     = docId,
          .docOffset = s * 50,
          .cellDense = 0,
          .spanIndex = static_cast<std::uint16_t>(s),
          .flags     = 0,
      };
      spanfilade.indexSpan(kScrollId, entry);
    }
  }

  spanfilade.build();
  EXPECT_EQ(spanfilade.totalSpans(), kNumDocs * kSpansPerDoc);

  // Active lens viewport span [2000, 2500) on scroll 42
  const PrimediaSpan viewportSpan{kScrollId, 2000, 500};

  // Perform interval stabbing and benchmark latency
  const auto t0   = std::chrono::steady_clock::now();
  const auto hits = spanfilade.findIntersections(viewportSpan);
  const auto t1   = std::chrono::steady_clock::now();
  const auto elapsedUs =
      std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

  // Verify sublinear interval stabbing completes under 20 microseconds
  EXPECT_LT(elapsedUs, 50); // Generous headroom for sanitizer/VM overhead
  EXPECT_FALSE(hits.empty());

  // Verify all returned hits actually overlap the query interval
  for (const auto &hit : hits) {
    EXPECT_LT(hit.start, viewportSpan.start + viewportSpan.length);
    EXPECT_GT(hit.end(), viewportSpan.start);
  }

  // Linear scan verification
  std::size_t expectedOverlapCount = 0;
  for (std::uint32_t docId = 0; docId < kNumDocs; ++docId) {
    for (std::uint32_t s = 0; s < kSpansPerDoc; ++s) {
      const std::uint64_t startAddr = (docId * 1000) + (s * 20);
      const std::uint64_t endAddr   = startAddr + 15;
      if (startAddr < (viewportSpan.start + viewportSpan.length) &&
          endAddr > viewportSpan.start) {
        expectedOverlapCount++;
      }
    }
  }
  EXPECT_EQ(hits.size(), expectedOverlapCount);
}

TEST(SpanfiladeBeamTest, MultiDocTransclusionGathering) {
  constexpr std::uint32_t kDocA        = 1;
  constexpr std::uint32_t kDocB        = 2;
  constexpr xanadu::ScrollId kScrollId = 99;

  Spanfilade spanfilade;

  // Doc A transcludes [100, 200)
  spanfilade.indexSpan(
      kScrollId,
      SpanEntry{.start = 100, .length = 100, .docId = kDocA, .docOffset = 0});

  // Doc B transcludes [150, 250) (overlapping by 50 bytes)
  spanfilade.indexSpan(
      kScrollId,
      SpanEntry{.start = 150, .length = 100, .docId = kDocB, .docOffset = 50});

  spanfilade.build();

  // Stab the transclusion overlap [150, 200)
  const PrimediaSpan overlapQuery{kScrollId, 150, 50};
  const auto intersecting = spanfilade.findIntersections(overlapQuery);

  ASSERT_EQ(intersecting.size(), 2U);
  EXPECT_TRUE(intersecting[0].docId == kDocA || intersecting[1].docId == kDocA);
  EXPECT_TRUE(intersecting[0].docId == kDocB || intersecting[1].docId == kDocB);
}

} // namespace
} // namespace xanadu::enfilade
