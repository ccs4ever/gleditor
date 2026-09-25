/**
 * @file tests/xudu/pouch_test.cpp
 * @brief Unit tests for PouchManager, DropZone, and backing system store.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <gleditor/cpp26.hpp>

#include "common/xanadu/extern_ref.hpp"
#include "common/xanadu/kinetic_tether.hpp"
#include "common/xanadu/publication.hpp"
#include "common/xanadu/system_docs.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"
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

TEST(PouchTest, pouchItemsSurviveReload) {
  const auto perma = std::make_shared<UserPermascroll>();
  Store store(perma);
  const auto v0 =
      store.insert(MicroversionId{}, 0, "Hello pouch persistent items test!");

  PouchManager pm(store);
  pm.loadManifest();

  const PrimediaSpan spanLeft{.scroll = 0, .start = 0, .length = 5};  // "Hello"
  const PrimediaSpan spanRight{.scroll = 0, .start = 6, .length = 5}; // "pouch"
  const PrimediaSpan spanLeft2{
      .scroll = 0, .start = 12, .length = 10}; // "persistent"

  const auto itemL1 = pm.dropSpan("to_link_left", spanLeft, "Hello", v0);
  const auto itemL2 = pm.dropSpan("to_link_left", spanLeft2, "persistent", v0);
  const auto itemR1 = pm.dropSpan("to_link_right", spanRight, "pouch", v0);

  EXPECT_NE(itemL1.itemId, 0U);
  EXPECT_NE(itemL2.itemId, 0U);
  EXPECT_NE(itemR1.itemId, 0U);
  EXPECT_NE(itemL1.itemId, itemL2.itemId);

  // Reload via fresh PouchManager pointing at same store
  PouchManager reloadedPm(store);
  reloadedPm.loadManifest();

  const auto leftZone = reloadedPm.zoneById("to_link_left");
  ASSERT_TRUE(leftZone.has_value());
  ASSERT_EQ(leftZone->items().size(), 2U);
  EXPECT_EQ(leftZone->items()[0].itemId, itemL1.itemId);
  EXPECT_EQ(leftZone->items()[0].span, spanLeft);
  EXPECT_EQ(leftZone->items()[0].previewText, "Hello");
  EXPECT_EQ(leftZone->items()[1].itemId, itemL2.itemId);
  EXPECT_EQ(leftZone->items()[1].span, spanLeft2);
  EXPECT_EQ(leftZone->items()[1].previewText, "persistent");

  const auto rightZone = reloadedPm.zoneById("to_link_right");
  ASSERT_TRUE(rightZone.has_value());
  ASSERT_EQ(rightZone->items().size(), 1U);
  EXPECT_EQ(rightZone->items()[0].itemId, itemR1.itemId);
  EXPECT_EQ(rightZone->items()[0].span, spanRight);
  EXPECT_EQ(rightZone->items()[0].previewText, "pouch");

  // In-order span equality
  const auto leftSpans = leftZone->allSpans();
  ASSERT_EQ(leftSpans.size(), 2U);
  EXPECT_EQ(leftSpans[0], spanLeft);
  EXPECT_EQ(leftSpans[1], spanLeft2);
}

TEST(PouchTest, aCellDropKeepsItsGlobalIdentity) {
  const auto perma = std::make_shared<UserPermascroll>();

  Store foreignStore(perma);
  auto atF = foreignStore.sliceGenesis(MicroversionId{});
  atF      = foreignStore.makeCell(atF, "Cross-store dragged cell");
  const auto foreignCellRef = foreignStore.cellRefOf(atF);

  Scroll sealedAs;
  sealedAs.publisher = PublicKey::fromHex(std::string(64, 'f'));
  sealedAs.salt      = "foreign_doc_salt";
  const auto expectedGlobalRef =
      opRefOf(foreignStore, foreignCellRef, sealedAs);
  ASSERT_FALSE(expectedGlobalRef.empty());

  Store pouchStore(perma);
  PouchManager pm(pouchStore);
  pm.loadManifest();

  const auto foreignManifold = foreignStore.rebuildManifold(atF);
  const auto spans           = foreignManifold.contentOf(foreignCellRef);
  ASSERT_FALSE(spans.empty());
  const auto cellSpan = spans.front();

  const auto item =
      pm.dropCell("notes", cellSpan, "Cross-store dragged cell",
                  expectedGlobalRef, "d.1: #" + std::to_string(foreignCellRef));
  EXPECT_NE(item.itemId, 0U);
  EXPECT_EQ(item.originKind, PouchOriginKind::ZigzagCell);

  // Add a branch to source store (shifts/alters foreign store history)
  auto branch =
      foreignStore.insert(MicroversionId{}, 0, "Independent branch prefix");
  (void)branch;

  // Reload the pouch and verify the extern placeholder resolves the same
  // GlobalOpRef
  PouchManager reloadedPm(pouchStore);
  reloadedPm.loadManifest();

  const auto notesZone = reloadedPm.zoneById("notes");
  ASSERT_TRUE(notesZone.has_value());
  ASSERT_EQ(notesZone->items().size(), 1U);

  const auto &reloadedItem = notesZone->items().front();
  EXPECT_EQ(reloadedItem.itemId, item.itemId);
  EXPECT_EQ(reloadedItem.originKind, PouchOriginKind::ZigzagCell);
  ASSERT_TRUE(reloadedItem.originOpRef.has_value());
  EXPECT_EQ(reloadedItem.originOpRef->scroll, expectedGlobalRef.scroll);
  EXPECT_EQ(reloadedItem.originOpRef->produces, expectedGlobalRef.produces);
}

TEST(PouchTest, droppingDoesNotWriteVersionAnnotations) {
  const auto perma = std::make_shared<UserPermascroll>();
  Store store(perma);
  const auto v0 = store.insert(MicroversionId{}, 0, "Zero-copy text stream");

  PouchManager pm(store);
  pm.loadManifest();

  const auto opsBeforeDrop = store.opCount();

  const PrimediaSpan docSpan{.scroll = 0, .start = 5, .length = 4};
  pm.dropSpan("to_link_left", docSpan, "copy", v0);

  const PrimediaSpan cellSpan{.scroll = 0, .start = 0, .length = 4};
  GlobalOpRef opRef{.scroll = "scroll_key_test", .produces = MicroversionId{}};
  pm.dropCell("to_link_right", cellSpan, "Zero", opRef, "d.1: #1");

  EXPECT_GT(store.opCount(), opsBeforeDrop);

  // Verify that the retired string protocol is completely absent from all
  // versions
  for (std::uint32_t i = 1; i <= store.opCount(); ++i) {
    const auto ver = store.segmentedOps().idOf(i);
    const auto ann = store.versionAnnotation(ver);
    if (ann.has_value()) {
      EXPECT_NE(ann->tag, "pouch-drop");
      EXPECT_NE(ann->tag, "pouch-cell-drop");
      EXPECT_NE(ann->tag, "pouch-manifest");
    }
  }
}

TEST(PouchTest, aPreviewIsDerivedNotDuplicated) {
  const auto perma = std::make_shared<UserPermascroll>();
  Store store(perma);
  initializeSystemStore(store, SystemDocKind::Pouches);
  const std::string text = "Theodor Holm Nelson on Hypertext and Transclusion";
  const auto v0          = store.insert(store.latest(), 0, text);

  PouchManager pm(store);
  pm.loadManifest();

  // Span pointing to "Nelson" (start 13, length 6) in v0
  const auto ver0  = store.rebuild(v0);
  const auto spans = ver0.spansFor(13, 6);
  ASSERT_FALSE(spans.empty());
  const auto span = spans.front();

  const auto opsBefore = store.opCount();
  // Drop with empty preview string
  const auto item = pm.dropSpan("notes", span, "", v0);
  EXPECT_NE(item.itemId, 0U);

  // Ensure no Insert operations were minted that duplicate the string "Nelson"
  for (std::uint32_t i = opsBefore + 1; i <= store.opCount(); ++i) {
    const auto *node = store.getCompactOp(i);
    ASSERT_NE(node, nullptr);
    EXPECT_NE(node->kind, OpKind::Insert);
  }

  // Reload pouch and verify preview is derived through SpanReader
  PouchManager reloadedPm(store);
  reloadedPm.loadManifest();

  const auto notes = reloadedPm.zoneById("notes");
  ASSERT_TRUE(notes.has_value());
  ASSERT_EQ(notes->items().size(), 1U);
  EXPECT_EQ(notes->items().front().previewText, "Nelson");
}

TEST(PouchTest, dismissalIsAnAuthoredRankMove) {
  const auto perma = std::make_shared<UserPermascroll>();
  Store store(perma);
  const auto v0 = store.insert(MicroversionId{}, 0, "Dismissable item content");

  PouchManager pm(store);
  pm.loadManifest();

  const PrimediaSpan span{
      .scroll = 0, .start = 0, .length = 11}; // "Dismissable"
  const auto item   = pm.dropSpan("scratch", span, "Dismissable", v0);
  const auto itemId = item.itemId;

  const auto scratchZone = pm.zoneById("scratch");
  ASSERT_TRUE(scratchZone.has_value());
  ASSERT_EQ(scratchZone->items().size(), 1U);

  const auto opsBeforeDismiss = store.opCount();
  const bool dismissed        = pm.dismissItem(itemId);
  EXPECT_TRUE(dismissed);

  // Authored rank move was recorded in store history
  EXPECT_GT(store.opCount(), opsBeforeDismiss);

  // Zone no longer holds the item
  EXPECT_EQ(scratchZone->items().size(), 0U);

  // Check the manifold topology directly: item left d.items, entered
  // d.dismissed
  const auto manifold     = store.rebuildManifold(pm.currentVersion());
  const auto dimItems     = manifold.dimensionNamed("d.items", store);
  const auto dimDismissed = manifold.dimensionNamed("d.dismissed", store);
  ASSERT_TRUE(dimItems.has_value());
  ASSERT_TRUE(dimDismissed.has_value());

  const auto zoneCell = scratchZone->cell();
  ASSERT_NE(zoneCell, zigzag::noCell);

  bool inItems = false;
  for (const auto c : zigzag::rankAfter(manifold, zoneCell, *dimItems)) {
    if (c == itemId) {
      inItems = true;
      break;
    }
  }
  EXPECT_FALSE(inItems);

  bool inDismissed = false;
  for (const auto c : zigzag::rankAfter(manifold, zoneCell, *dimDismissed)) {
    if (c == itemId) {
      inDismissed = true;
      break;
    }
  }
  EXPECT_TRUE(inDismissed);

  // Slot retains its cell identity and content
  const auto slot = manifold.slot(itemId);
  ASSERT_TRUE(slot.has_value());
  const auto content = manifold.contentOf(itemId);
  ASSERT_FALSE(content.empty());
  EXPECT_EQ(content.front(), span);

  // Reloading the pouch does not resurrect the dismissed item in d.items
  PouchManager reloadedPm(store);
  reloadedPm.loadManifest();
  EXPECT_EQ(reloadedPm.zoneById("scratch")->items().size(), 0U);
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
  // The hit is the zone itself, not a copy of it.
  EXPECT_EQ(&pm.zoneAt(50.0F, 120.0F).value(), &zone);
  EXPECT_FALSE((pm.zoneAt(0.0F, 0.0F)).has_value());

  // Remove zone
  EXPECT_TRUE(pm.removeZone("rebuttal"));
  EXPECT_FALSE((pm.zoneById("rebuttal")).has_value());
  EXPECT_FALSE(pm.removeZone("nonexistent"));
}

TEST(PouchTest, cancelledDragWritesNothing) {
  Store store;
  const auto initialOpCount = store.opCount();

  KineticTetherEngine engine;
  TetherPayload payload{
      .span             = PrimediaSpan{.scroll = 0, .start = 10, .length = 20},
      .previewText      = "Ephemeral drag payload",
      .originVersion    = store.primaryCurrentVersion(),
      .originDocIndex   = 0,
      .originCharStart  = 10,
      .originCharEnd    = 30,
      .originScreenPos  = glm::vec2(100.0F, 100.0F),
      .originKind       = PouchOriginKind::Document,
      .originCell       = 0,
      .originSliceIndex = 0,
      .originRankCoord  = {},
      .originOpRef      = std::nullopt,
      .originDocState   = std::nullopt,
  };

  engine.startDrag(std::move(payload), 100.0F, 100.0F);
  EXPECT_TRUE(engine.isDragging());
  EXPECT_TRUE(engine.busy());

  engine.updateDrag(300.0F, 300.0F);
  EXPECT_TRUE(engine.isDetached());

  engine.cancelDrag();
  EXPECT_FALSE(engine.isDragging());
  EXPECT_TRUE(engine.busy());
  EXPECT_EQ(engine.state(), TetherState::SnappingBack);

  while (engine.busy()) {
    engine.stepPhysics();
  }
  EXPECT_FALSE(engine.busy());
  EXPECT_EQ(engine.state(), TetherState::Idle);

  // R8 boundary preserved: absolutely zero operations written to store
  EXPECT_EQ(store.opCount(), initialOpCount);
}

TEST(PouchTest, forgeSlotsRemainEphemeralUntilForge) {
  Store store;
  const auto v0 = store.insert(MicroversionId{}, 0, "Left side homestead text");
  const auto v1 = store.insert(v0, 0, "Right side toward text");
  const auto opCountBeforeForge = store.opCount();

  // Ephemeral clasp bench staging slots (RAM only)
  std::vector<PouchItem> leftSlot;
  std::vector<PouchItem> rightSlot;

  const PrimediaSpan leftSpan{.scroll = 0, .start = 0, .length = 9};
  const PrimediaSpan rightSpan{.scroll = 0, .start = 0, .length = 10};

  // Half-drop into left slot
  leftSlot.push_back(PouchItem{
      .itemId           = 1,
      .span             = leftSpan,
      .previewText      = "Left side",
      .originVersion    = v0,
      .originDocIndex   = 0,
      .originCharStart  = 0,
      .originCharEnd    = 9,
      .timestampUtc     = 0,
      .originKind       = PouchOriginKind::Document,
      .originCell       = 0,
      .originSliceIndex = 0,
      .originRankCoord  = {},
      .originOpRef      = std::nullopt,
      .originDocState   = std::nullopt,
  });
  EXPECT_EQ(store.opCount(), opCountBeforeForge);

  // Half-drop into right slot
  rightSlot.push_back(PouchItem{
      .itemId           = 2,
      .span             = rightSpan,
      .previewText      = "Right side",
      .originVersion    = v1,
      .originDocIndex   = 1,
      .originCharStart  = 0,
      .originCharEnd    = 10,
      .timestampUtc     = 0,
      .originKind       = PouchOriginKind::Document,
      .originCell       = 0,
      .originSliceIndex = 0,
      .originRankCoord  = {},
      .originOpRef      = std::nullopt,
      .originDocState   = std::nullopt,
  });
  EXPECT_EQ(store.opCount(), opCountBeforeForge);

  // Staging is ephemeral and writes nothing; forging is the authorial operation
  Link claspLink;
  claspLink.type  = LinkType::Comment;
  claspLink.tier  = ProminenceTier::Author;
  claspLink.owner = "local_author";
  claspLink.left  = {leftSlot.front().span};
  claspLink.right = {rightSlot.front().span};

  const auto forgedVer = store.addLink(v1, claspLink);
  EXPECT_NE(forgedVer, v1);
  EXPECT_GT(store.opCount(), opCountBeforeForge);

  auto touching = store.linksTouching(leftSpan);
  ASSERT_EQ(std::ranges::distance(touching), 1);
  EXPECT_EQ(touching.front().type, LinkType::Comment);
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
  auto touchingLeft = st.linksTouching(leftSpans.front());
  ASSERT_FALSE(touchingLeft.empty());
  EXPECT_EQ(touchingLeft.front().type, LinkType::Comment);

  auto touchingRight = st.linksTouching(rightSpans.front());
  ASSERT_FALSE(touchingRight.empty());
  EXPECT_EQ(touchingRight.front().type, LinkType::Comment);
}

TEST(PouchTest, SwingBackResolvesExactByteSpan) {
  Store st;
  const auto root = MicroversionId{};
  const auto v1 =
      st.insert(root, 0, "Header text. Target premise passage. Footer notes.");

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
  const auto v2 =
      st.insert(v1, 0, "A long introductory section that shifts all offsets! ");
  const auto ver2 = st.rebuild(v2);

  // Primedia span address did not change! In v2, occurrencesOf finds the
  // shifted byte position
  const auto shiftedOccurrences = ver2.occurrencesOf(spans.front());
  ASSERT_EQ(shiftedOccurrences.size(), 1U);
  EXPECT_GT(shiftedOccurrences.front().start, 13U);
  EXPECT_EQ(shiftedOccurrences.front().end - shiftedOccurrences.front().start,
            23U);
}

TEST(PouchTest, BackedBySystemStore) {
  Store store;
  const auto v0 = store.insert(MicroversionId{}, 0, "placeholder content\n");
  (void)v0;

  PouchManager pm(store);
  pm.loadManifest();
  EXPECT_EQ(pm.zones().size(), 4U);
  EXPECT_EQ(pm.zones()[0]->id(), "to_link_left");

  // Drop a span into system-backed manager
  const PrimediaSpan span{.scroll = 0, .start = 10, .length = 5};
  const auto item = pm.dropSpan("notes", span, "Notes excerpt", store.latest());
  EXPECT_EQ(item.previewText, "Notes excerpt");
  EXPECT_EQ(pm.zoneById("notes")->items().size(), 1U);
}
