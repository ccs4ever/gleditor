#include <gtest/gtest.h>

#include "common/xanadu/enfilade/spanfilade.hpp"
#include "common/xanadu/link_layout.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/zigzag/manifold.hpp"

namespace {

using namespace xanadu;
using namespace xanadu::enfilade;
using namespace zigzag;

TEST(UniversalTransclusionTest, DocDocTransclusionEquivalence) {
  Store store;
  const std::string text = "Alpha Beta Gamma Delta Epsilon Zeta Eta Theta";
  const auto v0          = store.insert(MicroversionId{}, 0, text);

  // Doc 1 transcludes "Beta Gamma" [6, 16)
  const auto v1 = store.transclude(MicroversionId{}, 0, v0, 6, 10);

  const auto doc0 = store.rebuild(v0);
  const auto doc1 = store.rebuild(v1);

  const UniversalViewContext ctx{
      .docViews      = {&doc0, &doc1},
      .manifoldViews = {},
      .manifoldFoci  = {},
      .cellRadius    = 3,
  };

  std::vector<TransclusionPair> layoutPairs;
  placeTransclusions(ctx, layoutPairs);

  ASSERT_EQ(layoutPairs.size(), 1U);
  EXPECT_TRUE(layoutPairs[0].from.isDocument());
  EXPECT_TRUE(layoutPairs[0].to.isDocument());
  EXPECT_EQ(layoutPairs[0].from.doc, 0U);
  EXPECT_EQ(layoutPairs[0].to.doc, 1U);
  EXPECT_EQ(layoutPairs[0].from.start, 6U);
  EXPECT_EQ(layoutPairs[0].from.end, 16U);
  EXPECT_EQ(layoutPairs[0].to.start, 0U);
  EXPECT_EQ(layoutPairs[0].to.end, 10U);
  EXPECT_EQ(layoutPairs[0].length(), 10U);

  // Spanfilade equivalence
  const auto filade = Spanfilade::fromContext(ctx);
  std::vector<TransclusionPair> filadePairs;
  filade.placeTransclusions(ctx, filadePairs);

  ASSERT_EQ(filadePairs.size(), layoutPairs.size());
  EXPECT_EQ(filadePairs[0], layoutPairs[0]);
}

TEST(UniversalTransclusionTest, DocCellTransclusionDiscovery) {
  Store store;
  auto at = store.sliceGenesis(MicroversionId{});

  // Create document with initial text
  const std::string docText = "0123456789Nelsonian Xanadu Zigzag Hypermedia";
  const auto vDoc           = store.insert(at, 0, docText);
  const auto doc            = store.rebuild(vDoc);
  ASSERT_FALSE(doc.pieces().empty());

  // Primedia span corresponding to "Xanadu Zigzag" (offset 20, len 13)
  const auto docSpan = doc.pieces().front();
  const PrimediaSpan transSpan{docSpan.scroll, docSpan.start + 20, 13};

  // Mint Cell in Zigzag manifold containing that exact span
  at                = store.makeCell(vDoc, "placeholder");
  const auto cellId = store.cellRefOf(at);
  at                = store.spliceCellSpan(at, cellId, 0, 11, transSpan);

  const auto manifold = store.rebuildManifold(at);
  EXPECT_TRUE(manifold.contains(cellId));

  // Focus on cellId so it resides within viewport radius 3
  const UniversalViewContext ctx{
      .docViews      = {&doc},
      .manifoldViews = {&manifold},
      .manifoldFoci  = {cellId},
      .cellRadius    = 3,
  };

  std::vector<TransclusionPair> layoutPairs;
  placeTransclusions(ctx, layoutPairs);

  ASSERT_EQ(layoutPairs.size(), 1U);
  EXPECT_TRUE(layoutPairs[0].from.isDocument());
  EXPECT_TRUE(layoutPairs[0].to.isCell());
  EXPECT_EQ(layoutPairs[0].from.doc, 0U);
  EXPECT_EQ(layoutPairs[0].from.start, 20U);
  EXPECT_EQ(layoutPairs[0].from.end, 33U);
  EXPECT_EQ(layoutPairs[0].to.cell(), cellId);
  EXPECT_EQ(layoutPairs[0].to.start, 0U);
  EXPECT_EQ(layoutPairs[0].to.end, 13U);
  EXPECT_EQ(layoutPairs[0].length(), 13U);

  // Spanfilade equivalence
  const auto filade = Spanfilade::fromContext(ctx);
  std::vector<TransclusionPair> filadePairs;
  filade.placeTransclusions(ctx, filadePairs);

  ASSERT_EQ(filadePairs.size(), 1U);
  EXPECT_EQ(filadePairs[0], layoutPairs[0]);
}

TEST(UniversalTransclusionTest, CellCellTransclusionDiscovery) {
  Store store;
  auto at = store.sliceGenesis(MicroversionId{});

  // Cell 1
  at               = store.makeCell(at, "Universal Transclusion Across Slices");
  const auto cell1 = store.cellRefOf(at);

  const auto manifold1    = store.rebuildManifold(at);
  const auto cell1Content = manifold1.contentOf(cell1);
  ASSERT_FALSE(cell1Content.empty());
  const auto sharedSpan = cell1Content.front();

  // Cell 2 sharing the exact same span
  at               = store.makeCell(at, "dummy");
  const auto cell2 = store.cellRefOf(at);
  at               = store.spliceCellSpan(at, cell2, 0, 5, sharedSpan);

  const auto manifold = store.rebuildManifold(at);

  const UniversalViewContext ctx{
      .docViews      = {},
      .manifoldViews = {&manifold},
      .manifoldFoci  = {},
      .cellRadius    = -1, // Unbounded
  };

  std::vector<TransclusionPair> layoutPairs;
  placeTransclusions(ctx, layoutPairs);

  ASSERT_FALSE(layoutPairs.empty());
  bool foundCellCell = false;
  for (const auto &tp : layoutPairs) {
    if (tp.from.isCell() && tp.to.isCell()) {
      if ((tp.from.cell() == cell1 && tp.to.cell() == cell2) ||
          (tp.from.cell() == cell2 && tp.to.cell() == cell1)) {
        foundCellCell = true;
        EXPECT_EQ(tp.length(), sharedSpan.length);
        break;
      }
    }
  }
  EXPECT_TRUE(foundCellCell);

  // Spanfilade equivalence
  const auto filade = Spanfilade::fromContext(ctx);
  std::vector<TransclusionPair> filadePairs;
  filade.placeTransclusions(ctx, filadePairs);

  bool filadeFound = false;
  for (const auto &tp : filadePairs) {
    if (tp.from.isCell() && tp.to.isCell()) {
      if ((tp.from.cell() == cell1 && tp.to.cell() == cell2) ||
          (tp.from.cell() == cell2 && tp.to.cell() == cell1)) {
        filadeFound = true;
        EXPECT_EQ(tp.length(), sharedSpan.length);
        break;
      }
    }
  }
  EXPECT_TRUE(filadeFound);
}

TEST(UniversalTransclusionTest, MultiSpanCellOffsetAccuracy) {
  Store store;
  auto at = store.sliceGenesis(MicroversionId{});

  // Base text
  const auto vBase   = store.insert(at, 0, "PREFIX_MIDDLE_SUFFIX");
  const auto baseDoc = store.rebuild(vBase);
  ASSERT_GE(baseDoc.pieces().size(), 1U);
  const auto fullSpan = baseDoc.pieces().front();

  // Slice middle span
  const PrimediaSpan midSpan{fullSpan.scroll, fullSpan.start + 7,
                             6}; // "MIDDLE"

  // Make Cell with 3 spans: "AAA" + midSpan + "BBB"
  at              = store.makeCell(vBase, "AAA");
  const auto cell = store.cellRefOf(at);
  at              = store.spliceCellSpan(at, cell, 3, 0, midSpan);
  at              = store.spliceCell(at, cell, 9, 0, "BBB");

  const auto manifold = store.rebuildManifold(at);
  EXPECT_GE(manifold.contentOf(cell).size(), 3U);

  // Document 2 quoting just the middle span in a fresh document
  const auto vDoc2 = store.transclude(MicroversionId{}, 0, vBase, 7, 6);
  const auto doc2  = store.rebuild(vDoc2);

  const UniversalViewContext ctx{
      .docViews      = {&doc2},
      .manifoldViews = {&manifold},
      .manifoldFoci  = {cell},
      .cellRadius    = 3,
  };

  std::vector<TransclusionPair> pairs;
  placeTransclusions(ctx, pairs);

  ASSERT_EQ(pairs.size(), 1U);
  EXPECT_TRUE(pairs[0].from.isDocument());
  EXPECT_TRUE(pairs[0].to.isCell());
  EXPECT_EQ(pairs[0].from.doc, 0U);
  EXPECT_EQ(pairs[0].from.start, 0U);
  EXPECT_EQ(pairs[0].from.end, 6U);
  EXPECT_EQ(pairs[0].to.cell(), cell);
  EXPECT_EQ(pairs[0].to.start, 3U); // After "AAA"
  EXPECT_EQ(pairs[0].to.end, 9U);
  EXPECT_EQ(pairs[0].to.spanIndex, 1U); // Second span in run
}

TEST(UniversalTransclusionTest, ViewportRadiusBoundingPrunesDistantCells) {
  Store store;
  auto at = store.sliceGenesis(MicroversionId{});

  // Mint dimension cell d.1
  at            = store.makeCell(at, "d.1");
  const auto d1 = store.cellRefOf(at);

  // Document with text
  const auto vDoc =
      store.insert(at, 0, "Target Shared Content in Distant Cell");
  const auto doc = store.rebuild(vDoc);
  ASSERT_FALSE(doc.pieces().empty());
  const auto docSpan = doc.pieces().front();

  // Cell 4 quotes that docSpan
  at               = store.makeCell(vDoc, "placeholder");
  const auto cell4 = store.cellRefOf(at);
  at               = store.spliceCellSpan(at, cell4, 0, 11, docSpan);

  at               = store.makeCell(at, "Cell Three");
  const auto cell3 = store.cellRefOf(at);

  at               = store.makeCell(at, "Cell Two");
  const auto cell2 = store.cellRefOf(at);

  at               = store.makeCell(at, "Cell One");
  const auto cell1 = store.cellRefOf(at);

  const auto homeCell = store.homeCell();

  // Link along d.1: homeCell -> cell1 -> cell2 -> cell3 -> cell4
  at = store.setLink(at, homeCell, d1, DimVector::POS, cell1);
  at = store.setLink(at, cell1, d1, DimVector::POS, cell2);
  at = store.setLink(at, cell2, d1, DimVector::POS, cell3);
  at = store.setLink(at, cell3, d1, DimVector::POS, cell4);

  const auto manifold = store.rebuildManifold(at);

  // Context with radius 2 from homeCell: cell4 is at distance 4, so it should
  // be PRUNED
  const UniversalViewContext ctxPruned{
      .docViews      = {&doc},
      .manifoldViews = {&manifold},
      .manifoldFoci  = {homeCell},
      .cellRadius    = 2,
  };

  std::vector<TransclusionPair> prunedPairs;
  placeTransclusions(ctxPruned, prunedPairs);
  EXPECT_TRUE(prunedPairs.empty());

  const auto filadePruned = Spanfilade::fromContext(ctxPruned);
  std::vector<TransclusionPair> filadePrunedPairs;
  filadePruned.placeTransclusions(ctxPruned, filadePrunedPairs);
  EXPECT_TRUE(filadePrunedPairs.empty());

  // Context with radius 4: cell4 is within radius, so it should be DISCOVERED
  const UniversalViewContext ctxIncluded{
      .docViews      = {&doc},
      .manifoldViews = {&manifold},
      .manifoldFoci  = {homeCell},
      .cellRadius    = 4,
  };

  std::vector<TransclusionPair> includedPairs;
  placeTransclusions(ctxIncluded, includedPairs);
  ASSERT_EQ(includedPairs.size(), 1U);
  EXPECT_EQ(includedPairs[0].to.cell(), cell4);

  const auto filadeIncluded = Spanfilade::fromContext(ctxIncluded);
  std::vector<TransclusionPair> filadeIncludedPairs;
  filadeIncluded.placeTransclusions(ctxIncluded, filadeIncludedPairs);
  ASSERT_EQ(filadeIncludedPairs.size(), 1U);
  EXPECT_EQ(filadeIncludedPairs[0].to.cell(), cell4);
}

TEST(UniversalTransclusionTest, CrossDomainLinkPlacement) {
  Store store;
  auto at = store.sliceGenesis(MicroversionId{});

  // Document text
  const auto vDoc = store.insert(at, 0, "Document Source Passage");
  const auto doc  = store.rebuild(vDoc);
  ASSERT_FALSE(doc.pieces().empty());
  const auto docSpan = doc.pieces().front();

  // Cell text
  at                   = store.makeCell(vDoc, "Cell Destination Target");
  const auto cell      = store.cellRefOf(at);
  const auto manifold  = store.rebuildManifold(at);
  const auto cellSpans = manifold.contentOf(cell);
  ASSERT_FALSE(cellSpans.empty());
  const auto cellSpan = cellSpans.front();

  // Create link between Document span and Cell span
  const Link lnk{
      .type  = LinkType::Comment,
      .tier  = ProminenceTier::Author,
      .left  = {docSpan},
      .right = {cellSpan},
  };
  store.addLink(at, lnk);

  const UniversalViewContext ctx{
      .docViews      = {&doc},
      .manifoldViews = {&manifold},
      .manifoldFoci  = {cell},
      .cellRadius    = 3,
  };

  std::vector<LinkedPair> between;
  std::vector<HalfLink> leaving;
  placeLinks(store.links(), ctx, between, leaving);

  ASSERT_EQ(between.size(), 1U);
  EXPECT_TRUE(between[0].from.isDocument());
  EXPECT_TRUE(between[0].to.isCell());
  EXPECT_EQ(between[0].from.doc, 0U);
  EXPECT_EQ(between[0].to.cell(), cell);
  EXPECT_EQ(between[0].type, LinkType::Comment);

  // If manifold is closed, the link becomes a HalfLink leaving the document
  const UniversalViewContext ctxDocOnly{
      .docViews      = {&doc},
      .manifoldViews = {},
      .manifoldFoci  = {},
      .cellRadius    = 3,
  };

  std::vector<LinkedPair> betweenHalf;
  std::vector<HalfLink> leavingHalf;
  placeLinks(store.links(), ctxDocOnly, betweenHalf, leavingHalf);

  EXPECT_TRUE(betweenHalf.empty());
  ASSERT_EQ(leavingHalf.size(), 1U);
  EXPECT_TRUE(leavingHalf[0].here.isDocument());
  EXPECT_EQ(leavingHalf[0].here.doc, 0U);
  ASSERT_EQ(leavingHalf[0].elsewhere.size(), 1U);
  EXPECT_EQ(leavingHalf[0].elsewhere[0], cellSpan);
}

} // namespace
