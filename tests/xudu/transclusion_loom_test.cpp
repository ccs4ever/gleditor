/**
 * @file transclusion_loom_test.cpp
 * @brief Unit tests for Braided Transclusion Loom detection and bundling.
 */
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "common/xanadu/link_layout.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/zigzag/manifold.hpp"

namespace {

using namespace xanadu;
using namespace zigzag;

TEST(TransclusionLoomTest, DetectsContiguousRankLooms) {
  Store store;
  auto at = store.sliceGenesis(MicroversionId{});

  // Create document with 3 paragraphs / sections
  const std::string docText =
      "Section One: The foundational architecture of Nelsonian hypermedia.\n"
      "Section Two: Transclusion ribbons weaving through coordinate space.\n"
      "Section Three: Multidimensional manifolds projecting over primedia.\n";
  const auto vDoc = store.insert(at, 0, docText);
  const auto doc  = store.rebuild(vDoc);
  ASSERT_FALSE(doc.pieces().empty());

  const auto p0 = doc.pieces().front();
  // Paragraph spans:
  // 1: offset 0, len 68 ("Section One: ...\n")
  // 2: offset 68, len 67 ("Section Two: ...\n")
  // 3: offset 135, len 68 ("Section Three: ...\n")
  const PrimediaSpan span1{p0.scroll, p0.start + 0, 68};
  const PrimediaSpan span2{p0.scroll, p0.start + 68, 67};
  const PrimediaSpan span3{p0.scroll, p0.start + 135, 68};

  // Mint 3 cells in Zigzag manifold
  at               = store.makeCell(vDoc, "cell1");
  const auto cell1 = store.cellRefOf(at);
  at               = store.spliceCellSpan(at, cell1, 0, 5, span1);

  at               = store.makeCell(at, "cell2");
  const auto cell2 = store.cellRefOf(at);
  at               = store.spliceCellSpan(at, cell2, 0, 5, span2);

  at               = store.makeCell(at, "cell3");
  const auto cell3 = store.cellRefOf(at);
  at               = store.spliceCellSpan(at, cell3, 0, 5, span3);

  // Link cells along sequence dimension: cell1 -> cell2 -> cell3
  const auto dSeq = store.rebuildManifold(at).dimensions().front();
  at              = store.setLink(at, cell1, dSeq, DimVector::POS, cell2);
  at              = store.setLink(at, cell2, dSeq, DimVector::POS, cell3);

  const auto manifold = store.rebuildManifold(at);

  const UniversalViewContext ctx{
      .docViews      = {&doc},
      .manifoldViews = {&manifold},
      .manifoldFoci  = {cell1},
      .cellRadius    = 5,
  };

  std::vector<TransclusionPair> pairs;
  placeTransclusions(ctx, pairs);

  ASSERT_EQ(pairs.size(), 3U);

  // Detect looms
  const auto looms = detectTransclusionLooms(ctx, pairs);
  ASSERT_EQ(looms.size(), 1U);

  const auto &loom = looms.front();
  EXPECT_EQ(loom.docIndex, 0U);
  EXPECT_EQ(loom.dimension, dSeq);
  EXPECT_TRUE(loom.posward);
  EXPECT_EQ(loom.strandIndices.size(), 3U);
  EXPECT_EQ(loom.docStartOffset, 0U);
  EXPECT_EQ(loom.docEndOffset, 203U);
  EXPECT_EQ(loom.headCell, cell1);
  EXPECT_EQ(loom.tailCell, cell3);
}

TEST(TransclusionLoomTest, NonContiguousOrDifferentDimensionsDoNotBundle) {
  Store store;
  auto at = store.sliceGenesis(MicroversionId{});

  const std::string docText = "Alpha Beta Gamma Delta Epsilon Zeta";
  const auto vDoc           = store.insert(at, 0, docText);
  const auto doc            = store.rebuild(vDoc);
  const auto p0             = doc.pieces().front();

  const PrimediaSpan span1{p0.scroll, p0.start + 0, 10};
  const PrimediaSpan span2{p0.scroll, p0.start + 11, 10};

  // Mint 2 cells in Zigzag manifold
  at               = store.makeCell(vDoc, "cellA");
  const auto cellA = store.cellRefOf(at);
  at               = store.spliceCellSpan(at, cellA, 0, 5, span1);

  at               = store.makeCell(at, "cellB");
  const auto cellB = store.cellRefOf(at);
  at               = store.spliceCellSpan(at, cellB, 0, 5, span2);

  // They are UNLINKED in the manifold (separate nodes)
  const auto manifold = store.rebuildManifold(at);

  const UniversalViewContext ctx{
      .docViews      = {&doc},
      .manifoldViews = {&manifold},
      .manifoldFoci  = {cellA},
      .cellRadius    = -1,
  };

  std::vector<TransclusionPair> pairs;
  placeTransclusions(ctx, pairs);

  ASSERT_EQ(pairs.size(), 2U);

  // Since cells are not linked along any dimension rank, no loom should be
  // detected
  const auto looms = detectTransclusionLooms(ctx, pairs);
  EXPECT_TRUE(looms.empty());
}

TEST(TransclusionLoomTest, DetectsNegativeRankLooms) {
  Store store;
  auto at = store.sliceGenesis(MicroversionId{});

  const std::string docText = "Alpha passage here.\n"
                              "Beta passage here.\n";
  const auto vDoc           = store.insert(at, 0, docText);
  const auto doc            = store.rebuild(vDoc);
  const auto p0             = doc.pieces().front();

  const PrimediaSpan span1{p0.scroll, p0.start + 0, 20};
  const PrimediaSpan span2{p0.scroll, p0.start + 20, 19};

  at            = store.makeCell(vDoc, "c1");
  const auto c1 = store.cellRefOf(at);
  at            = store.spliceCellSpan(at, c1, 0, 2, span1);

  at            = store.makeCell(at, "c2");
  const auto c2 = store.cellRefOf(at);
  at            = store.spliceCellSpan(at, c2, 0, 2, span2);

  const auto dSeq = store.rebuildManifold(at).dimensions().front();
  at              = store.setLink(at, c1, dSeq, DimVector::NEG, c2);

  const auto manifold = store.rebuildManifold(at);

  const UniversalViewContext ctx{
      .docViews      = {&doc},
      .manifoldViews = {&manifold},
      .manifoldFoci  = {c1},
      .cellRadius    = -1,
  };

  std::vector<TransclusionPair> pairs;
  placeTransclusions(ctx, pairs);
  ASSERT_EQ(pairs.size(), 2U);

  const auto looms = detectTransclusionLooms(ctx, pairs);
  ASSERT_EQ(looms.size(), 1U);
  EXPECT_FALSE(looms.front().posward);
  EXPECT_EQ(looms.front().strandIndices.size(), 2U);
}

} // namespace
