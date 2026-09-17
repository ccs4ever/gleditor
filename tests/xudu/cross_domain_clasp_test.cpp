/**
 * @file cross_domain_clasp_test.cpp
 * @brief Unit tests for cross-domain pouch items, drop zones, and clasp
 * forging.
 */
#include <gtest/gtest.h>

#include "common/xanadu/kinetic_tether.hpp"
#include "xudu/core/link_layout.hpp"
#include "xudu/core/ops.hpp"
#include "xudu/core/pouch_zone.hpp"
#include "xudu/core/store.hpp"

using namespace xudu;

TEST(CrossDomainClaspTest, PouchItemDualCardRepresentation) {
  // 1. Document card defaults
  PouchItem docItem;
  EXPECT_EQ(docItem.originKind, PouchOriginKind::Document);
  EXPECT_EQ(docItem.originCell, 0U);
  EXPECT_EQ(docItem.originSliceIndex, 0U);
  EXPECT_TRUE(docItem.originRankCoord.empty());

  // 2. Zigzag cell card item representation
  PouchItem cellItem;
  cellItem.itemId           = 42;
  cellItem.originKind       = PouchOriginKind::ZigzagCell;
  cellItem.originCell       = 108;
  cellItem.originSliceIndex = 2;
  cellItem.originRankCoord  = "d.sequence: #5";
  cellItem.previewText      = "Hypermedia Cell 108";

  EXPECT_EQ(cellItem.originKind, PouchOriginKind::ZigzagCell);
  EXPECT_EQ(cellItem.originCell, 108U);
  EXPECT_EQ(cellItem.originSliceIndex, 2U);
  EXPECT_EQ(cellItem.originRankCoord, "d.sequence: #5");
  EXPECT_EQ(cellItem.previewText, "Hypermedia Cell 108");
}

TEST(CrossDomainClaspTest, PouchManagerDropCell) {
  PouchManager pm;
  const PrimediaSpan span{.scroll = 0, .start = 200, .length = 40};
  const std::string preview = "Nelsonian Intertwingularity";

  const auto item =
      pm.dropCell("to_link_right", span, preview, 77, "d.concept: #3", 1);
  EXPECT_EQ(item.itemId, 1U);
  EXPECT_EQ(item.originKind, PouchOriginKind::ZigzagCell);
  EXPECT_EQ(item.originCell, 77U);
  EXPECT_EQ(item.originSliceIndex, 1U);
  EXPECT_EQ(item.originRankCoord, "d.concept: #3");
  EXPECT_EQ(item.previewText, preview);
  EXPECT_EQ(item.span.start, 200U);
  EXPECT_EQ(item.span.length, 40U);

  const auto *zone = pm.zoneById("to_link_right");
  ASSERT_NE(zone, nullptr);
  ASSERT_EQ(zone->items().size(), 1U);
  EXPECT_EQ(zone->items().front().originCell, 77U);
  EXPECT_EQ(zone->items().front().originRankCoord, "d.concept: #3");

  // Verify dismissing the cell item works properly
  EXPECT_TRUE(pm.dismissItem(item.itemId));
  EXPECT_EQ(zone->items().size(), 0U);
}

TEST(CrossDomainClaspTest, LinkForgeCrossDomainAssemblyInStore) {
  Store store;
  const auto v0 =
      store.insert(MicroversionId{}, 0, "Doc span: Literary Machines");
  const auto v1 = store.insert(v0, 27, "Cell span: Parallel Dimension");

  const auto docText  = store.rebuild(v0);
  const auto cellText = store.rebuild(v1);

  const auto leftSpans  = docText.spansFor(0, 8);   // "Doc span"
  const auto rightSpans = cellText.spansFor(27, 9); // "Cell span"

  Link claspLink;
  claspLink.type  = LinkType::Quotation;
  claspLink.owner = "curator";
  claspLink.left  = leftSpans;
  claspLink.right = rightSpans;

  const auto forgedVer = store.addLink(v1, claspLink);
  EXPECT_NE(forgedVer, v1);

  const auto &links = store.links();
  ASSERT_FALSE(links.empty());

  const auto &forged = links.rbegin()->second;
  EXPECT_EQ(forged.type, LinkType::Quotation);
  EXPECT_EQ(forged.owner, "curator");
  ASSERT_EQ(forged.left.size(), leftSpans.size());
  ASSERT_EQ(forged.right.size(), rightSpans.size());
  EXPECT_EQ(forged.left[0].start, leftSpans[0].start);
  EXPECT_EQ(forged.right[0].start, rightSpans[0].start);
}

TEST(CrossDomainClaspTest, PouchManagerCellPartitionDrop) {
  PouchManager pm;
  pm.addZone(DropZoneConfig{
      .id        = "zz_staging",
      .label     = "Zigzag Staging",
      .auraColor = 0x8B5CF6FFU,
  });

  const PrimediaSpan s1{.scroll = 0, .start = 10, .length = 15};
  const PrimediaSpan s2{.scroll = 0, .start = 30, .length = 20};

  const auto i1 =
      pm.dropCell("to_link_left", s1, "Left Cell", 101, "d.x: #1", 0);
  const auto i2 =
      pm.dropCell("zz_staging", s2, "Staged Cell", 102, "d.y: #2", 0);

  const auto *leftZone    = pm.zoneById("to_link_left");
  const auto *stagingZone = pm.zoneById("zz_staging");

  ASSERT_NE(leftZone, nullptr);
  ASSERT_NE(stagingZone, nullptr);

  EXPECT_EQ(leftZone->items().size(), 1U);
  EXPECT_EQ(leftZone->items().front().itemId, i1.itemId);
  EXPECT_EQ(leftZone->items().front().originCell, 101U);

  EXPECT_EQ(stagingZone->items().size(), 1U);
  EXPECT_EQ(stagingZone->items().front().itemId, i2.itemId);
  EXPECT_EQ(stagingZone->items().front().originCell, 102U);
}

TEST(CrossDomainClaspTest, TetherPayloadCrossDomainCellMetadata) {
  TetherPayload payload;
  EXPECT_EQ(payload.originKind, PouchOriginKind::Document);
  EXPECT_EQ(payload.originCell, 0U);
  EXPECT_EQ(payload.originSliceIndex, 0U);
  EXPECT_TRUE(payload.originRankCoord.empty());

  payload.originKind       = PouchOriginKind::ZigzagCell;
  payload.originCell       = 42U;
  payload.originSliceIndex = 1U;
  payload.originRankCoord  = "d.1: #5";

  EXPECT_EQ(payload.originKind, PouchOriginKind::ZigzagCell);
  EXPECT_EQ(payload.originCell, 42U);
  EXPECT_EQ(payload.originSliceIndex, 1U);
  EXPECT_EQ(payload.originRankCoord, "d.1: #5");
}

TEST(CrossDomainClaspTest,
     PouchManagerCellDropHomesteadAndTowardBilateralForge) {
  PouchManager pm;
  const PrimediaSpan docSpan{.scroll = 0, .start = 100, .length = 20};
  const PrimediaSpan cellSpan{.scroll = 1, .start = 500, .length = 35};

  const auto docItem  = pm.dropSpan("to_link_left", docSpan, "Doc quote",
                                    MicroversionId{}, 0, 10, 30);
  const auto cellItem = pm.dropCell("to_link_right", cellSpan, "Cell node", 88,
                                    "d.concept: #7", 0);

  EXPECT_EQ(docItem.originKind, PouchOriginKind::Document);
  EXPECT_EQ(cellItem.originKind, PouchOriginKind::ZigzagCell);
  EXPECT_EQ(cellItem.originCell, 88U);
  EXPECT_EQ(cellItem.originRankCoord, "d.concept: #7");

  // Forge bilateral clasp in store
  Store store;
  const auto v0 =
      store.insert(MicroversionId{}, 0, "Host document content here");
  Link clasp;
  clasp.type  = LinkType::Quotation;
  clasp.owner = "curator";
  clasp.tier  = ProminenceTier::Curated;
  clasp.left  = {docItem.span};
  clasp.right = {cellItem.span};

  const auto v1 = store.addLink(v0, clasp);
  EXPECT_NE(v1, v0);

  const auto &links = store.links();
  ASSERT_FALSE(links.empty());
  const auto &forged = links.rbegin()->second;
  EXPECT_EQ(forged.type, LinkType::Quotation);
  EXPECT_EQ(forged.tier, ProminenceTier::Curated);
  ASSERT_EQ(forged.left.size(), 1U);
  ASSERT_EQ(forged.right.size(), 1U);
  EXPECT_EQ(forged.left[0].start, 100U);
  EXPECT_EQ(forged.left[0].length, 20U);
  EXPECT_EQ(forged.right[0].start, 500U);
  EXPECT_EQ(forged.right[0].length, 35U);
}

TEST(CrossDomainClaspTest, PouchManagerCrossDomainCellSpanPreservation) {
  PouchManager pm;
  const PrimediaSpan s1{.scroll = 3, .start = 1000, .length = 50};
  const PrimediaSpan s2{.scroll = 3, .start = 2000, .length = 80};

  pm.dropCell("to_link_left", s1, "Cell Span 1", 201, "d.time: #10", 0);
  pm.dropCell("to_link_left", s2, "Cell Span 2", 202, "d.time: #11", 0);

  const auto *zone = pm.zoneById("to_link_left");
  ASSERT_NE(zone, nullptr);
  ASSERT_EQ(zone->items().size(), 2U);

  const auto allSpans = zone->allSpans();
  ASSERT_EQ(allSpans.size(), 2U);
  EXPECT_EQ(allSpans[0].scroll, 3U);
  EXPECT_EQ(allSpans[0].start, 1000U);
  EXPECT_EQ(allSpans[0].length, 50U);
  EXPECT_EQ(allSpans[1].scroll, 3U);
  EXPECT_EQ(allSpans[1].start, 2000U);
  EXPECT_EQ(allSpans[1].length, 80U);
}
