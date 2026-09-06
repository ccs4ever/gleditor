/**
 * @file tests/xudu/pouch_test.cpp
 * @brief Unit tests for PouchManager, DropZone, and backing system store.
 */
#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "xudu/core/ops.hpp"
#include "xudu/core/pouch_zone.hpp"
#include "xudu/core/spool.hpp"
#include "xudu/core/store.hpp"
#include "xudu/core/user_permascroll.hpp"

using namespace xudu;

TEST(PouchTest, DefaultPartitionsAreInitialized) {
  PouchManager pm;
  const auto &zones = pm.zones();
  ASSERT_EQ(zones.size(), 4U);

  EXPECT_EQ(zones[0]->id(), "to_link_left");
  EXPECT_EQ(zones[0]->label(), "To Link (Left)");
  EXPECT_EQ(zones[0]->auraColor(), 0x06B6D4FFU);

  EXPECT_EQ(zones[1]->id(), "to_link_right");
  EXPECT_EQ(zones[1]->label(), "To Link (Right)");
  EXPECT_EQ(zones[1]->auraColor(), 0xEC4899FFU);

  EXPECT_EQ(zones[2]->id(), "notes");
  EXPECT_EQ(zones[2]->label(), "Notes");
  EXPECT_EQ(zones[2]->auraColor(), 0xEAB308FFU);

  EXPECT_EQ(zones[3]->id(), "scratch");
  EXPECT_EQ(zones[3]->label(), "Scratch");
  EXPECT_EQ(zones[3]->auraColor(), 0x10B981FFU);
}

TEST(PouchTest, DropSpanIntoZoneAppendsTransclusionWithZeroByteDuplication) {
  PouchManager pm;
  const auto root = MicroversionId{};

  // Create a virtual span (e.g. 25 characters from scroll 0)
  const PrimediaSpan span{.scroll = 0, .start = 100, .length = 25};
  const std::string preview = "Theodor Holm Nelson";

  const auto item = pm.dropSpan("to_link_left", span, preview, root, 0, 10, 35);
  EXPECT_EQ(item.itemId, 1U);
  EXPECT_EQ(item.previewText, preview);
  EXPECT_EQ(item.span.scroll, 0U);
  EXPECT_EQ(item.span.start, 100U);
  EXPECT_EQ(item.span.length, 25U);
  EXPECT_EQ(item.originDocIndex, 0U);

  // Check drop zone holds item
  const auto *zone = pm.zoneById("to_link_left");
  ASSERT_NE(zone, nullptr);
  ASSERT_EQ(zone->items().size(), 1U);
  EXPECT_EQ(zone->items().front().itemId, 1U);

  // Verify backing store recorded the operation
  EXPECT_NE(pm.currentVersion(), root);
  const auto ann = pm.store().versionAnnotation(pm.currentVersion());
  ASSERT_TRUE(ann.has_value());
  EXPECT_EQ(ann->alias, "to_link_left");
  EXPECT_EQ(ann->description, preview);

  // All spans returned from zone
  const auto allSpans = zone->allSpans();
  ASSERT_EQ(allSpans.size(), 1U);
  EXPECT_EQ(allSpans[0].start, 100U);
}

TEST(PouchTest, DismissItemMovesToNonDestructiveLimbo) {
  PouchManager pm;
  const PrimediaSpan span{.scroll = 0, .start = 50, .length = 15};
  const auto item =
      pm.dropSpan("notes", span, "Important citation", MicroversionId{});

  auto *zone = pm.zoneById("notes");
  ASSERT_NE(zone, nullptr);
  EXPECT_EQ(zone->items().size(), 1U);

  // Dismiss item
  const auto prevVer   = pm.currentVersion();
  const bool dismissed = pm.dismissItem(item.itemId);
  EXPECT_TRUE(dismissed);
  EXPECT_EQ(zone->items().size(), 0U);

  // Backing store advanced (erased to limbo)
  EXPECT_NE(pm.currentVersion(), prevVer);

  // Dismissing unknown id returns false
  EXPECT_FALSE(pm.dismissItem(9999U));
}

TEST(PouchTest, AddRemoveCustomZoneAndHitTesting) {
  PouchManager pm;

  // Add custom zone
  auto &zone = pm.addZone(DropZoneConfig{
      .id              = "rebuttal",
      .label           = "Rebuttal Arguments",
      .backgroundColor = glm::vec4(0.25F, 0.1F, 0.1F, 0.8F),
      .auraColor       = 0xFF5533FFU,
      .heightWeight    = 1.5F,
  });
  EXPECT_EQ(zone.id(), "rebuttal");
  EXPECT_EQ(zone.label(), "Rebuttal Arguments");

  // Set rect geometry: x=10, y=100, w=200, h=80
  zone.setRect(10.0F, 100.0F, 200.0F, 80.0F);
  EXPECT_TRUE(zone.contains(50.0F, 120.0F));
  EXPECT_TRUE(zone.contains(10.0F, 100.0F));
  EXPECT_TRUE(zone.contains(210.0F, 180.0F));
  EXPECT_FALSE(zone.contains(5.0F, 120.0F));
  EXPECT_FALSE(zone.contains(50.0F, 190.0F));

  // Hit test via manager
  EXPECT_EQ(pm.zoneAt(50.0F, 120.0F), &zone);
  EXPECT_EQ(pm.zoneAt(0.0F, 0.0F), nullptr);

  // Remove zone
  EXPECT_TRUE(pm.removeZone("rebuttal"));
  EXPECT_EQ(pm.zoneById("rebuttal"), nullptr);
  EXPECT_FALSE(pm.removeZone("nonexistent"));
}

TEST(PouchTest, ManifestSerializationRoundTrip) {
  PouchManager pm;
  pm.addZone(DropZoneConfig{
      .id              = "custom_zone",
      .label           = "Custom Quotes",
      .backgroundColor = glm::vec4(0.1F, 0.2F, 0.3F, 0.9F),
      .auraColor       = 0x4488FFFFU,
      .heightWeight    = 2.0F,
  });

  pm.saveManifest();

  // Create second manager and restore
  PouchManager pm2;
  // Copy annotation over to simulate shared or reloaded store
  const auto ann = pm.store().versionAnnotation(MicroversionId{});
  ASSERT_TRUE(ann.has_value());
  pm2.store().setVersionAnnotation(MicroversionId{}, *ann);

  pm2.loadManifest();
  const auto *restored = pm2.zoneById("custom_zone");
  ASSERT_NE(restored, nullptr);
  EXPECT_EQ(restored->label(), "Custom Quotes");
  EXPECT_EQ(restored->auraColor(), 0x4488FFFFU);
  EXPECT_FLOAT_EQ(restored->heightWeight(), 2.0F);
}

TEST(PouchTest, ForgeClaspCreatesBidirectionalCompoundLink) {
  Store st;
  const auto root = MicroversionId{};
  const auto vA   = st.insert(root, 0, "The quick brown fox jumps");
  const auto vB   = st.insert(root, 0, "Counterargument: The lazy dog sleeps");

  const auto verA = st.rebuild(vA);
  const auto verB = st.rebuild(vB);

  const auto leftSpans  = verA.spansFor(4, 5);  // "quick"
  const auto rightSpans = verB.spansFor(18, 8); // "The lazy"
  ASSERT_FALSE(leftSpans.empty());
  ASSERT_FALSE(rightSpans.empty());

  // Compound N x M Link
  Link link;
  link.type  = LinkType::Comment;
  link.tier  = ProminenceTier::Author;
  link.owner = "local_author";
  link.left  = leftSpans;
  link.right = rightSpans;

  const auto vLinked = st.addLink(vA, link);
  EXPECT_NE(vLinked, vA);

  // Verify link exists and touches both ends
  const auto touchingLeft = st.linksTouching(leftSpans.front());
  ASSERT_FALSE(touchingLeft.empty());
  EXPECT_EQ(touchingLeft.front()->type, LinkType::Comment);

  const auto touchingRight = st.linksTouching(rightSpans.front());
  ASSERT_FALSE(touchingRight.empty());
  EXPECT_EQ(touchingRight.front()->type, LinkType::Comment);
}

TEST(PouchTest, SwingBackResolvesExactByteSpan) {
  Store st;
  const auto root = MicroversionId{};
  const auto v1   = st.insert(root, 0, "Header text. Target premise passage. Footer notes.");

  const auto ver1 = st.rebuild(v1);
  // "Target premise passage." is at offset 13, length 23
  const auto spans = ver1.spansFor(13, 23);
  ASSERT_EQ(spans.size(), 1U);

  // Swing back query: find where this span resides in v1
  const auto occurrences = ver1.occurrencesOf(spans.front());
  ASSERT_EQ(occurrences.size(), 1U);
  EXPECT_EQ(occurrences.front().start, 13U);
  EXPECT_EQ(occurrences.front().end, 36U);

  // Now create branch v2 that prepends 50 characters
  const auto v2   = st.insert(v1, 0, "A long introductory section that shifts all offsets! ");
  const auto ver2 = st.rebuild(v2);

  // Primedia span address did not change! In v2, occurrencesOf finds the shifted byte position
  const auto shiftedOccurrences = ver2.occurrencesOf(spans.front());
  ASSERT_EQ(shiftedOccurrences.size(), 1U);
  EXPECT_GT(shiftedOccurrences.front().start, 13U);
  EXPECT_EQ(shiftedOccurrences.front().end - shiftedOccurrences.front().start, 23U);
}
