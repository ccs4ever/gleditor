/**
 * @file spanfilade_test.cpp
 * @brief Comprehensive unit tests for the True Spanfilade.
 */
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "common/xanadu/enfilade/spanfilade.hpp"
#include "common/xanadu/link_layout.hpp"
#include "common/xanadu/microversion.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/version.hpp"
#include "common/xanadu/zigzag/manifold.hpp"

namespace {

using xanadu::Extent;
using xanadu::MicroversionId;
using xanadu::PrimediaSpan;
using xanadu::Store;
using xanadu::TransclusionPair;
using xanadu::Version;
using xanadu::enfilade::DisplacementMonoid;
using xanadu::enfilade::EnfiladeAction;
using xanadu::enfilade::ScrollSpanfilade;
using xanadu::enfilade::SpanDsp;
using xanadu::enfilade::SpanEntry;
using xanadu::enfilade::Spanfilade;
using xanadu::enfilade::SpanWid;
using xanadu::enfilade::WidthMonoid;
using zigzag::Manifold;

// 1. Concept Verification
static_assert(DisplacementMonoid<SpanDsp>);
static_assert(WidthMonoid<SpanWid>);
static_assert(EnfiladeAction<SpanDsp, SpanWid>);
static_assert(sizeof(SpanEntry) == 32);

TEST(SpanfiladeTest, MonoidAxiomsAndAction) {
  // Identity and composition
  SpanDsp d0{};
  EXPECT_TRUE(d0.isIdentity());

  SpanDsp d1{100};
  SpanDsp d2{250};
  const auto d12 = d1.compose(d2);
  EXPECT_EQ(d12.delta, 350);

  // Width monoid combination
  SpanWid wEmpty{};
  EXPECT_TRUE(wEmpty.isEmpty());

  SpanWid w1{10, 50, 1};
  SpanWid w2{30, 90, 1};
  const auto wCombined = w1.combine(w2);
  EXPECT_FALSE(wCombined.isEmpty());
  EXPECT_EQ(wCombined.minStart, 10U);
  EXPECT_EQ(wCombined.maxEnd, 90U);
  EXPECT_EQ(wCombined.count, 2U);

  // Distributive action d . w
  const auto wShifted = d1.act(w1);
  EXPECT_EQ(wShifted.minStart, 110U);
  EXPECT_EQ(wShifted.maxEnd, 150U);
  EXPECT_EQ(wShifted.count, 1U);

  // Overlap checks
  EXPECT_TRUE(wCombined.overlaps(40, 60));
  EXPECT_TRUE(wCombined.overlaps(0, 20));
  EXPECT_FALSE(wCombined.overlaps(90, 120)); // disjoint to the right
  EXPECT_FALSE(wCombined.overlaps(0, 10));   // disjoint to the left
}

TEST(SpanfiladeTest, SingleDocumentOccurrencesOfAndR9) {
  Store store;
  const std::string text = "The quick brown fox jumps over the lazy dog.";
  const auto v0          = store.insert(MicroversionId{}, 0, text);
  const auto doc0        = store.rebuild(v0);

  auto filade = Spanfilade::fromVersion(doc0, 0);
  EXPECT_FALSE(filade.empty());
  EXPECT_EQ(filade.totalSpans(), doc0.pieces().size());

  // Test full span query
  const auto &p0 = doc0.pieces().front();
  EXPECT_TRUE(filade.verifyAgainstLinearScan(p0, doc0, 0));

  // Test partial sub-span query
  const PrimediaSpan subSpan{p0.scroll, p0.start + 4, 15}; // "quick brown fox"
  const auto occs = filade.occurrencesOf(subSpan, 0);
  ASSERT_EQ(occs.size(), 1U);
  EXPECT_EQ(occs[0].start, 4U);
  EXPECT_EQ(occs[0].end, 19U);
  EXPECT_TRUE(filade.verifyAgainstLinearScan(subSpan, doc0, 0));

  // Test non-overlapping span query
  const PrimediaSpan missingSpan{p0.scroll, p0.start + 1000, 10};
  EXPECT_TRUE(filade.occurrencesOf(missingSpan, 0).empty());
  EXPECT_TRUE(filade.verifyAgainstLinearScan(missingSpan, doc0, 0));
}

TEST(SpanfiladeTest, MultiDocumentTransclusionDiscovery) {
  Store store;
  const std::string text = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNO"
                           "PQRSTUVWXYZ!@#$%^&*()_+~`|}{[]:;?><,./-=01234567";
  const auto v0          = store.insert(MicroversionId{}, 0, text);

  // Doc 1 transcludes [10, 50) and [50, 90) as two separate operations
  const auto v1a = store.transclude(MicroversionId{}, 0, v0, 10, 40);
  const auto v1b = store.transclude(v1a, 40, v0, 50, 40);

  const auto doc0 = store.rebuild(v0);
  const auto doc1 = store.rebuild(v1b);

  const std::vector<const Version *> views{&doc0, &doc1};

  // Run legacy placeTransclusions
  std::vector<TransclusionPair> legacyPairs;
  xanadu::placeTransclusions(views, legacyPairs);

  // Run Spanfilade placeTransclusions
  auto filade = Spanfilade::fromViews(views);
  std::vector<TransclusionPair> filadePairs;
  filade.placeTransclusions(views, filadePairs);

  ASSERT_EQ(filadePairs.size(), legacyPairs.size());
  ASSERT_EQ(filadePairs.size(), 1U);

  // Ruling R9: Mathematical equivalence
  EXPECT_EQ(filadePairs[0].from.doc, legacyPairs[0].from.doc);
  EXPECT_EQ(filadePairs[0].from.start, legacyPairs[0].from.start);
  EXPECT_EQ(filadePairs[0].from.end, legacyPairs[0].from.end);

  EXPECT_EQ(filadePairs[0].to.doc, legacyPairs[0].to.doc);
  EXPECT_EQ(filadePairs[0].to.start, legacyPairs[0].to.start);
  EXPECT_EQ(filadePairs[0].to.end, legacyPairs[0].to.end);

  EXPECT_EQ(filadePairs[0].span, legacyPairs[0].span);
}

TEST(SpanfiladeTest, ZigzagManifoldIndexingAndU3) {
  Store store;
  auto at = store.sliceGenesis(MicroversionId{});

  // Cell 1
  at               = store.makeCell(at, "Cell One Content");
  const auto cell1 = store.cellRefOf(at);

  // Cell 2 with multiple spans (via spliceCell)
  at               = store.makeCell(at, "the quick brown fox");
  const auto cell2 = store.cellRefOf(at);
  at               = store.spliceCell(at, cell2, 4, 5, "slow");

  const auto manifold   = store.rebuildManifold(at);
  const auto cell2Spans = manifold.contentOf(cell2);
  ASSERT_GT(cell2Spans.size(), 1U);

  // Index into Spanfilade
  auto filade = Spanfilade::fromManifold(manifold);
  EXPECT_FALSE(filade.empty());

  // Query for cell 2's spans
  for (std::size_t sIdx = 0; sIdx < cell2Spans.size(); ++sIdx) {
    const auto hits = filade.findIntersections(cell2Spans[sIdx]);
    ASSERT_FALSE(hits.empty());
    bool foundCell2 = false;
    for (const auto &h : hits) {
      if (h.isCell() && h.spanIndex == sIdx &&
          manifold.cells()[h.cellDense].birthOp == cell2) {
        foundCell2 = true;
        break;
      }
    }
    EXPECT_TRUE(foundCell2);
  }

  // Also query cell 1's span
  const auto cell1Spans = manifold.contentOf(cell1);
  ASSERT_FALSE(cell1Spans.empty());
  const auto hits1 = filade.findIntersections(cell1Spans[0]);
  ASSERT_FALSE(hits1.empty());
  EXPECT_EQ(manifold.cells()[hits1[0].cellDense].birthOp, cell1);
}

TEST(SpanfiladeTest, MultiScrollIsolation) {
  Spanfilade filade;

  // Insert span on scroll 0
  SpanEntry e0{
      .start     = 100,
      .length    = 50,
      .docId     = 0,
      .docOffset = 0,
  };
  filade.indexSpan(0, e0);

  // Insert span with identical start/length on scroll 42
  SpanEntry e42{
      .start     = 100,
      .length    = 50,
      .docId     = 1,
      .docOffset = 0,
  };
  filade.indexSpan(42, e42);
  filade.build();

  EXPECT_EQ(filade.scrollCount(), 2U);

  // Querying scroll 0 should only return e0
  const auto hits0 = filade.findIntersections(PrimediaSpan{0, 120, 10});
  ASSERT_EQ(hits0.size(), 1U);
  EXPECT_EQ(hits0[0].docId, 0U);

  // Querying scroll 42 should only return e42
  const auto hits42 = filade.findIntersections(PrimediaSpan{42, 120, 10});
  ASSERT_EQ(hits42.size(), 1U);
  EXPECT_EQ(hits42[0].docId, 1U);

  // Querying unindexed scroll 99 returns empty
  const auto hits99 = filade.findIntersections(PrimediaSpan{99, 120, 10});
  EXPECT_TRUE(hits99.empty());
}

TEST(SpanfiladeTest, CachedSpanfiladeDifferentialAndSignatureTest) {
  Store store;
  auto at = store.sliceGenesis(MicroversionId{});

  at            = store.makeCell(at, "d.1");
  const auto d1 = store.cellRefOf(at);

  const auto vDoc = store.insert(at, 0, "0123456789abcdefghijklmnopqrstuvwxyz");
  const auto doc0 = store.rebuild(vDoc);
  ASSERT_FALSE(doc0.pieces().empty());
  const auto docSpan = doc0.pieces().front();

  // Document 1 transcludes part of document 0
  const auto vDoc1 = store.transclude(vDoc, 0, vDoc, 10, 15);
  const auto doc1  = store.rebuild(vDoc1);

  // Cell 1 quotes docSpan
  at               = store.makeCell(vDoc1, "placeholder1");
  const auto cell1 = store.cellRefOf(at);
  at               = store.spliceCellSpan(at, cell1, 0, 12, docSpan);

  // Intermediate cell 2
  at               = store.makeCell(at, "cell2");
  const auto cell2 = store.cellRefOf(at);

  // Distant cell 3 quotes docSpan
  at               = store.makeCell(at, "placeholder3");
  const auto cell3 = store.cellRefOf(at);
  at               = store.spliceCellSpan(at, cell3, 0, 12, docSpan);

  const auto homeCell = store.homeCell();

  // Link along d.1: homeCell -> cell1 -> cell2 -> (chain...) -> cell3
  at = store.setLink(at, homeCell, d1, zigzag::DimVector::POS, cell1);
  at = store.setLink(at, cell1, d1, zigzag::DimVector::POS, cell2);

  auto prev = cell2;
  for (int i = 0; i < 3; ++i) {
    at                      = store.makeCell(at, "chain");
    const auto intermediate = store.cellRefOf(at);
    at   = store.setLink(at, prev, d1, zigzag::DimVector::POS, intermediate);
    prev = intermediate;
  }
  at = store.setLink(at, prev, d1, zigzag::DimVector::POS, cell3);

  const auto manifold = store.rebuildManifold(at);

  // 1) Context with radius 2 from cell1: cell3 is at distance 5, so it must be
  // pruned
  const xanadu::UniversalViewContext ctxPruned{
      .docViews      = {&doc0, &doc1},
      .manifoldViews = {&manifold},
      .manifoldFoci  = {cell1},
      .cellRadius    = 2,
  };

  std::vector<TransclusionPair> referencePruned;
  xanadu::placeTransclusions(ctxPruned, referencePruned);
  ASSERT_FALSE(referencePruned.empty());

  // Fresh Spanfilade
  auto freshPruned = Spanfilade::fromContext(ctxPruned);
  std::vector<TransclusionPair> freshPrunedPairs;
  freshPruned.placeTransclusions(ctxPruned, freshPrunedPairs);
  EXPECT_EQ(freshPrunedPairs, referencePruned);

  // Cached Spanfilade re-evaluated
  auto cachedSpanfilade = Spanfilade::fromContext(ctxPruned);
  std::vector<TransclusionPair> cachedPrunedPairs;
  cachedSpanfilade.placeTransclusions(ctxPruned, cachedPrunedPairs);
  EXPECT_EQ(cachedPrunedPairs, referencePruned);

  // Verify cell3 was pruned
  for (const auto &pair : cachedPrunedPairs) {
    if (pair.from.isCell()) {
      EXPECT_NE(pair.from.cell(), cell3);
    }
    if (pair.to.isCell()) {
      EXPECT_NE(pair.to.cell(), cell3);
    }
  }

  // 2) Context with radius 10: cell3 is included
  const xanadu::UniversalViewContext ctxIncluded{
      .docViews      = {&doc0, &doc1},
      .manifoldViews = {&manifold},
      .manifoldFoci  = {cell1},
      .cellRadius    = 10,
  };

  std::vector<TransclusionPair> referenceIncluded;
  xanadu::placeTransclusions(ctxIncluded, referenceIncluded);
  ASSERT_GT(referenceIncluded.size(), referencePruned.size());

  auto freshIncluded = Spanfilade::fromContext(ctxIncluded);
  std::vector<TransclusionPair> freshIncludedPairs;
  freshIncluded.placeTransclusions(ctxIncluded, freshIncludedPairs);
  EXPECT_EQ(freshIncludedPairs, referenceIncluded);

  // Cached Spanfilade with ctxIncluded
  cachedSpanfilade = Spanfilade::fromContext(ctxIncluded);
  std::vector<TransclusionPair> cachedIncludedPairs;
  cachedSpanfilade.placeTransclusions(ctxIncluded, cachedIncludedPairs);
  EXPECT_EQ(cachedIncludedPairs, referenceIncluded);

  // Verify cell3 is present
  bool foundCell3 = false;
  for (const auto &pair : cachedIncludedPairs) {
    if ((pair.from.isCell() && pair.from.cell() == cell3) ||
        (pair.to.isCell() && pair.to.cell() == cell3)) {
      foundCell3 = true;
      break;
    }
  }
  EXPECT_TRUE(foundCell3);
}

} // namespace
