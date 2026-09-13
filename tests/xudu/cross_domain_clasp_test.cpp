/**
 * @file cross_domain_clasp_test.cpp
 * @brief Unit tests for cross-domain pouch items, drop zones, and clasp
 * forging.
 */
#include <gtest/gtest.h>

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
  const auto v1 = store.insert(v0, 100, "Cell span: Parallel Dimension");

  const auto docText  = store.rebuild(v0);
  const auto cellText = store.rebuild(v1);

  const auto leftSpans  = docText.spansFor(0, 8);    // "Doc span"
  const auto rightSpans = cellText.spansFor(100, 9); // "Cell span"

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
