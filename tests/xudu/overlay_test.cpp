/**
 * @file overlay_test.cpp
 * @brief Unit tests for Section 5.12: Plural structure maps: overlays without a
 *        second writer.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "common/xanadu/compact_op.hpp"
#include "common/xanadu/extern_ref.hpp"
#include "common/xanadu/link_discovery.hpp"
#include "common/xanadu/link_package.hpp"
#include "common/xanadu/ops.hpp"
#include "common/xanadu/overlay.hpp"
#include "common/xanadu/publication.hpp"
#include "common/xanadu/published_vocabulary.hpp"
#include "common/xanadu/scroll.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/system_docs.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"
#include "common/xanadu/zigzag/manifold.hpp"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#endif

namespace {

using namespace xanadu;

struct TestStore {
  std::shared_ptr<UserPermascroll> perma;
  std::unique_ptr<Store> store;
  MicroversionId head;
  std::string scrollKey;
  PublicKey authorKey;

  explicit TestStore(std::string key = "btpk:1111:doc")
      : perma(std::make_shared<UserPermascroll>()),
        store(std::make_unique<Store>(perma)), scrollKey(std::move(key)) {
    authorKey.bytes.fill(0x11);
    head = store->sliceGenesis(MicroversionId{});
    store->setBootstrapPermascroll(scrollKey, nullptr);
  }

  zigzag::DimRef makeDim(const std::string_view name) {
    const auto m = store->makeDimension(head, name);
    head         = m.version;
    return m.dim;
  }

  zigzag::CellRef makeCell(const std::string_view text) {
    head = store->makeCell(head, text);
    return store->cellRefOf(head);
  }

  MicroversionId versionOf(const zigzag::CellRef cell) const {
    return store->segmentedOps().idOf(cell);
  }

  zigzag::CellRef makeExternPlaceholder(const std::string &targetScroll,
                                        const MicroversionId &produces) {
    auto fold = store->rebuildManifold(head);
    if (!store->scrollRegistry().scrollIdForKey(targetScroll)) {
      head = store->registerScroll(head, targetScroll, &fold);
      fold = store->rebuildManifold(head);
    }
    const auto sid = *store->scrollRegistry().scrollIdForKey(targetScroll);
    const ExternOpRef ext{.scroll = sid, .produces = produces};
    head          = store->makeExternRef(head, ext, &fold);
    fold          = store->rebuildManifold(head);
    const auto ph = fold.scrollRegistry(*store).placeholderForExtern(ext);
    return ph.value_or(zigzag::noCell);
  }

  zigzag::CellRef makeExternPlaceholder(const TestStore &target,
                                        const zigzag::CellRef cell) {
    return makeExternPlaceholder(target.scrollKey, target.versionOf(cell));
  }
};

// 1. anOverlayWritesOnlyItsOwnStore
TEST(OverlayTest, anOverlayWritesOnlyItsOwnStore) {
  TestStore target("btpk:author:docA");
  const auto dimD1  = target.makeDim("d.1");
  const auto cellA1 = target.makeCell("Cell A1");
  const auto cellA2 = target.makeCell("Cell A2");
  target.head       = target.store->setLink(target.head, cellA1, dimD1,
                                            zigzag::DimVector::POS, cellA2);
  const auto targetFoldBefore = target.store->rebuildManifold(target.head);

  const auto targetOpCountBefore = target.store->segmentedOps().size();
  const auto targetLatestBefore  = target.store->latest();

  TestStore overlay("btpk:curator:overlay");
  const auto phA1 =
      overlay.makeExternPlaceholder(target.scrollKey, target.head);
  const auto localB = overlay.makeCell("Local Note");
  const auto dimAlt = overlay.makeDim("d.alternate");

  const auto claimRes = authorOverlayClaim(*overlay.store, overlay.head, phA1,
                                           dimAlt, zigzag::DimVector::POS,
                                           localB, zigzag::noCell, "claim");
  overlay.head        = claimRes.version;

  // Target store is completely unchanged: one-writer rule
  EXPECT_EQ(target.store->segmentedOps().size(), targetOpCountBefore);
  EXPECT_EQ(target.store->latest(), targetLatestBefore);

  const auto targetFoldAfter = target.store->rebuildManifold(target.head);
  EXPECT_EQ(targetFoldAfter.cellCount(), targetFoldBefore.cellCount());
  EXPECT_EQ(targetFoldAfter.linked(cellA1, dimD1, zigzag::DimVector::POS),
            cellA2);

  // Overlay has written its own operations
  EXPECT_GT(overlay.store->segmentedOps().size(), 0u);
}

// 2. anOverlayClaimIsAHandledSetLink
TEST(OverlayTest, anOverlayClaimIsAHandledSetLink) {
  TestStore overlay("btpk:curator:overlay");
  const auto c1   = overlay.makeCell("C1");
  const auto c2   = overlay.makeCell("C2");
  const auto dim1 = overlay.makeDim("d.1");

  const auto rootRes =
      declareOverlayTarget(*overlay.store, overlay.head,
                           GlobalDocumentState{.scroll  = "btpk:target:doc",
                                               .version = MicroversionId{}});
  overlay.head = rootRes.version;

  const auto claimRes = authorOverlayClaim(*overlay.store, overlay.head, c1,
                                           dim1, zigzag::DimVector::POS, c2,
                                           rootRes.targetCell, "my_claim");
  overlay.head        = claimRes.version;

  // Reload overlay manifold and recover from d.overlay-claims
  const auto fold = overlay.store->rebuildManifold(overlay.head);
  const auto dimClaimsOpt =
      fold.dimensionNamed(kDimOverlayClaims, *overlay.store);
  ASSERT_TRUE(dimClaimsOpt.has_value());

  auto claims = zigzag::rankAfter(fold, rootRes.targetCell, *dimClaimsOpt);
  auto it     = claims.begin();
  ASSERT_NE(it, claims.end());
  const auto handleCell = *it;

  const auto opIndexOpt = fold.handleTarget(handleCell);
  ASSERT_TRUE(opIndexOpt.has_value());

  const auto *node = overlay.store->getCompactOp(*opIndexOpt);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node->kind, OpKind::Structure);
  EXPECT_EQ(structureVerbOf(node->flags), StructureVerb::SetLink);
  EXPECT_EQ(node->linkId, dim1);
  EXPECT_EQ(node->to, c2);
  EXPECT_EQ(node->flags & structureNegward, 0); // POS direction
}

// 3. anExplicitBreakSurvivesTheFold
TEST(OverlayTest, anExplicitBreakSurvivesTheFold) {
  TestStore overlay("btpk:curator:overlay");
  const auto c1   = overlay.makeCell("C1");
  const auto c2   = overlay.makeCell("C2");
  const auto dim1 = overlay.makeDim("d.1");

  // First link c1 to c2
  overlay.head = overlay.store->setLink(overlay.head, c1, dim1,
                                        zigzag::DimVector::POS, c2);

  // Author an explicit break: to == noCell
  const auto breakRes = authorOverlayBreak(*overlay.store, overlay.head, c1,
                                           dim1, zigzag::DimVector::POS);
  overlay.head        = breakRes.version;

  const auto fold = overlay.store->rebuildManifold(overlay.head);

  // Folded manifold slot is empty (noCell)
  EXPECT_EQ(fold.linked(c1, dim1, zigzag::DimVector::POS), zigzag::noCell);

  // But the break survives via the operation handle on d.overlay-claims
  const auto dimClaimsOpt =
      fold.dimensionNamed(kDimOverlayClaims, *overlay.store);
  ASSERT_TRUE(dimClaimsOpt.has_value());

  const auto opIndex = fold.handleTarget(breakRes.handleCell);
  ASSERT_TRUE(opIndex.has_value());
  const auto *node = overlay.store->getCompactOp(*opIndex);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(structureVerbOf(node->flags), StructureVerb::SetLink);
  EXPECT_EQ(node->to, zigzag::noCell); // Explicit break preserved!
}

// 4. aClaimOutsideThePinnedSnapshotIsRejectedSeparately
TEST(OverlayTest, aClaimOutsideThePinnedSnapshotIsRejectedSeparately) {
  TestStore target("btpk:author:doc");
  const auto cA = target.makeCell("Cell A");
  const auto v1 = target.head; // Pinned snapshot T1

  // Target advances to T2 with new cell B
  const auto cB = target.makeCell("Cell B (minted after T1)");
  const auto v2 = target.head; // State T2

  TestStore overlay("btpk:curator:overlay");
  const auto rootRes = declareOverlayTarget(
      *overlay.store, overlay.head,
      GlobalDocumentState{.scroll = target.scrollKey, .version = v1});
  overlay.head = rootRes.version;

  // Overlay authors a claim referencing cB at v2 (outside snapshot T1)
  const auto phA = overlay.makeExternPlaceholder(target.scrollKey, v1);
  const auto phB = overlay.makeExternPlaceholder(target.scrollKey, v2);
  const auto dim = overlay.makeDim("d.outline");

  const auto claimRes =
      authorOverlayClaim(*overlay.store, overlay.head, phA, dim,
                         zigzag::DimVector::POS, phB, rootRes.targetCell);
  overlay.head = claimRes.version;

  const auto targetFoldT1 = target.store->rebuildManifold(v1);

  OverlayComposition comp;
  comp.withTarget(OverlayComposition::TargetSpec{
                      .spaceId = 0,
                      .state   = GlobalDocumentState{.scroll  = target.scrollKey,
                                                     .version = v1},
                      .store   = target.store.get(),
                      .manifold  = &targetFoldT1,
                      .scrollKey = target.scrollKey,
                  })
      .withOverlay(OverlayAttachment{
          .store          = overlay.store.get(),
          .publicationKey = overlay.scrollKey,
          .releaseCell    = rootRes.targetCell,
      })
      .compose();

  EXPECT_EQ(targetFoldT1.refusedOps(), 0u);

  // Claim is reported as OutsideSnapshot
  const auto &cands = comp.allCandidates();
  ASSERT_FALSE(cands.empty());
  EXPECT_EQ(cands.front().state, OverlayClaimState::OutsideSnapshot);
}

// 5. anOverlayMayAlignTwoTargetStores
TEST(OverlayTest, anOverlayMayAlignTwoTargetStores) {
  TestStore target1("btpk:author1:english");
  const auto cEnglish = target1.makeCell("Hello World");
  const auto v1       = target1.head;
  const auto fold1    = target1.store->rebuildManifold(v1);

  TestStore target2("btpk:author2:french");
  const auto cFrench = target2.makeCell("Bonjour le monde");
  const auto v2      = target2.head;
  const auto fold2   = target2.store->rebuildManifold(v2);

  TestStore overlay("btpk:translator:align");
  const auto r1 = declareOverlayTarget(
      *overlay.store, overlay.head,
      GlobalDocumentState{.scroll = target1.scrollKey, .version = v1});
  overlay.head  = r1.version;
  const auto r2 = declareOverlayTarget(
      *overlay.store, overlay.head,
      GlobalDocumentState{.scroll = target2.scrollKey, .version = v2},
      r1.targetCell);
  overlay.head = r2.version;

  const auto phEn  = overlay.makeExternPlaceholder(target1.scrollKey, v1);
  const auto phFr  = overlay.makeExternPlaceholder(target2.scrollKey, v2);
  const auto dimTr = overlay.makeDim("d.translate");
  const auto clmRes =
      authorOverlayClaim(*overlay.store, overlay.head, phEn, dimTr,
                         zigzag::DimVector::POS, phFr, r1.targetCell);
  overlay.head = clmRes.version;

  OverlayComposition comp;
  comp.withTarget(OverlayComposition::TargetSpec{
                      .spaceId   = 1,
                      .state     = {.scroll = target1.scrollKey, .version = v1},
                      .store     = target1.store.get(),
                      .manifold  = &fold1,
                      .scrollKey = target1.scrollKey,
                  })
      .withTarget(OverlayComposition::TargetSpec{
          .spaceId   = 2,
          .state     = {.scroll = target2.scrollKey, .version = v2},
          .store     = target2.store.get(),
          .manifold  = &fold2,
          .scrollKey = target2.scrollKey,
      })
      .withOverlay(OverlayAttachment{
          .store          = overlay.store.get(),
          .publicationKey = overlay.scrollKey,
          .releaseCell    = r1.targetCell,
      })
      .compose();

  const auto pEn = comp.view().findExistingProxy(1, cEnglish);
  const auto pFr = comp.view().findExistingProxy(2, cFrench);
  ASSERT_NE(pEn, zigzag::noCell);
  ASSERT_NE(pFr, zigzag::noCell);

  const auto dimArenaTr = comp.allCandidates().front().dimension;

  EXPECT_EQ(comp.effectiveLink(pEn, dimArenaTr, zigzag::DimVector::POS), pFr);
  EXPECT_EQ(comp.effectiveLink(pFr, dimArenaTr, zigzag::DimVector::NEG), pEn);
}

// 6. intrinsicStructureOutranksEveryOverlay
TEST(OverlayTest, intrinsicStructureOutranksEveryOverlay) {
  TestStore target("btpk:author:doc");
  const auto dimD1 = target.makeDim("d.1");
  const auto cA    = target.makeCell("A");
  const auto cB    = target.makeCell("B");
  target.head =
      target.store->setLink(target.head, cA, dimD1, zigzag::DimVector::POS, cB);
  const auto targetFold = target.store->rebuildManifold(target.head);

  // Author overlay contests (cA -> cX)
  TestStore ovAuthor("btpk:author:overlay");
  ovAuthor.authorKey = target.authorKey; // Matching publisher
  const auto phA     = ovAuthor.makeExternPlaceholder(target, cA);
  const auto cX      = ovAuthor.makeCell("X");
  const auto dimOvA  = ovAuthor.makeDim("d.1");
  const auto clmA    = authorOverlayClaim(*ovAuthor.store, ovAuthor.head, phA,
                                          dimOvA, zigzag::DimVector::POS, cX);
  ovAuthor.head      = clmA.version;

  // Curated overlay contests (cA -> cY)
  TestStore ovCurated("btpk:curator:overlay");
  PublicKey curatorKey;
  curatorKey.bytes.fill(0x22);
  ovCurated.authorKey = curatorKey;
  const auto phAC     = ovCurated.makeExternPlaceholder(target, cA);
  const auto cY       = ovCurated.makeCell("Y");
  const auto dimOvC   = ovCurated.makeDim("d.1");
  const auto clmC = authorOverlayClaim(*ovCurated.store, ovCurated.head, phAC,
                                       dimOvC, zigzag::DimVector::POS, cY);
  ovCurated.head  = clmC.version;

  // Public overlay contests (cA -> cZ)
  TestStore ovPublic("btpk:public:overlay");
  PublicKey publicKey;
  publicKey.bytes.fill(0x33);
  ovPublic.authorKey = publicKey;
  const auto phAP    = ovPublic.makeExternPlaceholder(target, cA);
  const auto cZ      = ovPublic.makeCell("Z");
  const auto dimOvP  = ovPublic.makeDim("d.1");
  const auto clmP    = authorOverlayClaim(*ovPublic.store, ovPublic.head, phAP,
                                          dimOvP, zigzag::DimVector::POS, cZ);
  ovPublic.head      = clmP.version;

  OverlayComposition comp;
  comp
      .withTarget(OverlayComposition::TargetSpec{
          .spaceId   = 1,
          .state     = {.scroll = target.scrollKey, .version = target.head},
          .store     = target.store.get(),
          .manifold  = &targetFold,
          .author    = target.authorKey,
          .scrollKey = target.scrollKey,
      })
      .withOverlay(OverlayAttachment{
          .store          = ovAuthor.store.get(),
          .publisher      = target.authorKey,
          .publicationKey = "author_overlay",
      })
      .withOverlay(OverlayAttachment{
          .store          = ovCurated.store.get(),
          .publisher      = curatorKey,
          .publicationKey = "curated_overlay",
      })
      .withOverlay(OverlayAttachment{
          .store          = ovPublic.store.get(),
          .publisher      = publicKey,
          .publicationKey = "public_overlay",
      })
      .withCurators({curatorKey})
      .compose();

  const auto pA     = comp.view().findExistingProxy(1, cA);
  const auto pB     = comp.view().findExistingProxy(1, cB);
  const auto dArena = comp.view().arenaDimFor(1, dimD1);

  // Intrinsic target link remains effective!
  EXPECT_EQ(comp.effectiveLink(pA, dArena, zigzag::DimVector::POS), pB);

  // All 3 alternative claims are inspectable and marked Shadowed
  const auto cands = comp.candidates(pA, dArena, zigzag::DimVector::POS);
  EXPECT_GE(cands.size(), 3u);
  for (const auto &c : cands) {
    EXPECT_EQ(c.state, OverlayClaimState::Shadowed);
  }
}

// 7. aHigherTierBreakSuppressesOnlyLowerOverlayClaims
TEST(OverlayTest, aHigherTierBreakSuppressesOnlyLowerOverlayClaims) {
  TestStore target("btpk:author:doc");
  const auto cA         = target.makeCell("A");
  const auto targetFold = target.store->rebuildManifold(target.head);

  PublicKey curatorKey;
  curatorKey.bytes.fill(0x22);
  TestStore ovCurated("btpk:curator:overlay");
  const auto phA1 = ovCurated.makeExternPlaceholder(target, cA);
  const auto dCur = ovCurated.makeDim("d.1");
  const auto brk  = authorOverlayBreak(*ovCurated.store, ovCurated.head, phA1,
                                       dCur, zigzag::DimVector::POS);
  ovCurated.head  = brk.version;

  PublicKey pubKey;
  pubKey.bytes.fill(0x33);
  TestStore ovPublic("btpk:public:overlay");
  const auto phA2 = ovPublic.makeExternPlaceholder(target, cA);
  const auto cP   = ovPublic.makeCell("Public Cell");
  const auto dPub = ovPublic.makeDim("d.1");
  const auto clm  = authorOverlayClaim(*ovPublic.store, ovPublic.head, phA2,
                                       dPub, zigzag::DimVector::POS, cP);
  ovPublic.head   = clm.version;

  OverlayComposition comp;
  comp
      .withTarget(OverlayComposition::TargetSpec{
          .spaceId   = 1,
          .state     = {.scroll = target.scrollKey, .version = target.head},
          .store     = target.store.get(),
          .manifold  = &targetFold,
          .author    = target.authorKey,
          .scrollKey = target.scrollKey,
      })
      .withOverlay(OverlayAttachment{
          .store          = ovCurated.store.get(),
          .publisher      = curatorKey,
          .publicationKey = "curated_overlay",
      })
      .withOverlay(OverlayAttachment{
          .store          = ovPublic.store.get(),
          .publisher      = pubKey,
          .publicationKey = "public_overlay",
      })
      .withCurators({curatorKey})
      .compose();

  const auto pA     = comp.view().findExistingProxy(1, cA);
  const auto dArena = comp.allCandidates().front().dimension;

  // The break won: slot is empty
  EXPECT_EQ(comp.effectiveLink(pA, dArena, zigzag::DimVector::POS),
            std::nullopt);

  // The public positive claim was shadowed
  const auto cands = comp.candidates(pA, dArena, zigzag::DimVector::POS);
  ASSERT_EQ(cands.size(), 2u);
  EXPECT_EQ(cands[0].tier, ProminenceTier::Curated);
  EXPECT_EQ(cands[0].state, OverlayClaimState::Applied);
  EXPECT_EQ(cands[1].tier, ProminenceTier::Public);
  EXPECT_EQ(cands[1].state, OverlayClaimState::Shadowed);
}

// 8. reciprocalSlotConflictsAreResolvedBeforeMaterialisation
TEST(OverlayTest, reciprocalSlotConflictsAreResolvedBeforeMaterialisation) {
  TestStore target("btpk:author:doc");
  const auto cA         = target.makeCell("A");
  const auto cB         = target.makeCell("B");
  const auto cC         = target.makeCell("C");
  const auto targetFold = target.store->rebuildManifold(target.head);

  // Overlay 1: A -> B along d.1
  TestStore ov1("btpk:ov1");
  const auto phA1 = ov1.makeExternPlaceholder(target, cA);
  const auto phB1 = ov1.makeExternPlaceholder(target, cB);
  const auto d1   = ov1.makeDim("d.1");
  const auto clm1 = authorOverlayClaim(*ov1.store, ov1.head, phA1, d1,
                                       zigzag::DimVector::POS, phB1);
  ov1.head        = clm1.version;

  // Overlay 2: C -> B along d.1 (conflicts on B's NEG slot!)
  TestStore ov2("btpk:ov2");
  const auto phC2 = ov2.makeExternPlaceholder(target, cC);
  const auto phB2 = ov2.makeExternPlaceholder(target, cB);
  const auto d2   = ov2.makeDim("d.1");
  const auto clm2 = authorOverlayClaim(*ov2.store, ov2.head, phC2, d2,
                                       zigzag::DimVector::POS, phB2);
  ov2.head        = clm2.version;

  OverlayComposition comp;
  comp
      .withTarget(OverlayComposition::TargetSpec{
          .spaceId   = 1,
          .state     = {.scroll = target.scrollKey, .version = target.head},
          .store     = target.store.get(),
          .manifold  = &targetFold,
          .scrollKey = target.scrollKey,
      })
      .withOverlay(OverlayAttachment{
          .store          = ov1.store.get(),
          .publicationKey = "ov1",
          .sequence       = 100, // Higher score wins
      })
      .withOverlay(OverlayAttachment{
          .store          = ov2.store.get(),
          .publicationKey = "ov2",
          .sequence       = 50,
      })
      .compose();

  const auto pA     = comp.view().findExistingProxy(1, cA);
  const auto pB     = comp.view().findExistingProxy(1, cB);
  const auto pC     = comp.view().findExistingProxy(1, cC);
  const auto dArena = comp.allCandidates().front().dimension;

  // Ov1 won and was applied symmetrically
  EXPECT_EQ(comp.effectiveLink(pA, dArena, zigzag::DimVector::POS), pB);
  EXPECT_EQ(comp.effectiveLink(pB, dArena, zigzag::DimVector::NEG), pA);

  // Ov2 had a slot conflict on B's negative slot
  EXPECT_EQ(comp.effectiveLink(pC, dArena, zigzag::DimVector::POS),
            std::nullopt);
  const auto candsC = comp.candidates(pC, dArena, zigzag::DimVector::POS);
  ASSERT_FALSE(candsC.empty());
  EXPECT_EQ(candsC.front().state, OverlayClaimState::SlotConflict);
}

// 9. networkArrivalOrderDoesNotChangeTheView
TEST(OverlayTest, networkArrivalOrderDoesNotChangeTheView) {
  TestStore target("btpk:target:doc");
  const auto cA         = target.makeCell("A");
  const auto cB         = target.makeCell("B");
  const auto targetFold = target.store->rebuildManifold(target.head);

  TestStore ov1("btpk:ov1");
  const auto phA1 = ov1.makeExternPlaceholder(target, cA);
  const auto phB1 = ov1.makeExternPlaceholder(target, cB);
  const auto d1   = ov1.makeDim("d.1");
  const auto clm1 = authorOverlayClaim(*ov1.store, ov1.head, phA1, d1,
                                       zigzag::DimVector::POS, phB1);
  ov1.head        = clm1.version;

  TestStore ov2("btpk:ov2");
  const auto phA2 = ov2.makeExternPlaceholder(target, cA);
  const auto cX   = ov2.makeCell("X");
  const auto d2   = ov2.makeDim("d.1");
  const auto clm2 = authorOverlayClaim(*ov2.store, ov2.head, phA2, d2,
                                       zigzag::DimVector::POS, cX);
  ov2.head        = clm2.version;

  const OverlayAttachment att1{
      .store          = ov1.store.get(),
      .publicationKey = "ov1",
      .sequence       = 20,
  };
  const OverlayAttachment att2{
      .store          = ov2.store.get(),
      .publicationKey = "ov2",
      .sequence       = 10,
  };

  // Order 1: att1 then att2
  OverlayComposition comp1;
  comp1
      .withTarget(
          {.spaceId   = 1,
           .state     = {.scroll = target.scrollKey, .version = target.head},
           .store     = target.store.get(),
           .manifold  = &targetFold,
           .scrollKey = target.scrollKey})
      .withOverlay(att1)
      .withOverlay(att2)
      .compose();

  // Order 2: att2 then att1
  OverlayComposition comp2;
  comp2
      .withTarget(
          {.spaceId   = 1,
           .state     = {.scroll = target.scrollKey, .version = target.head},
           .store     = target.store.get(),
           .manifold  = &targetFold,
           .scrollKey = target.scrollKey})
      .withOverlay(att2)
      .withOverlay(att1)
      .compose();

  const auto pA1 = comp1.view().findExistingProxy(1, cA);
  const auto pB1 = comp1.view().findExistingProxy(1, cB);
  const auto pA2 = comp2.view().findExistingProxy(1, cA);
  const auto pB2 = comp2.view().findExistingProxy(1, cB);

  const auto dArena1 = comp1.allCandidates().front().dimension;
  const auto dArena2 = comp2.allCandidates().front().dimension;

  EXPECT_EQ(comp1.effectiveLink(pA1, dArena1, zigzag::DimVector::POS), pB1);
  EXPECT_EQ(comp2.effectiveLink(pA2, dArena2, zigzag::DimVector::POS), pB2);

  const auto cands1 = comp1.candidates(pA1, dArena1, zigzag::DimVector::POS);
  const auto cands2 = comp2.candidates(pA2, dArena2, zigzag::DimVector::POS);
  ASSERT_EQ(cands1.size(), cands2.size());
  for (std::size_t i = 0; i < cands1.size(); ++i) {
    EXPECT_EQ(cands1[i].state, cands2[i].state);
    EXPECT_EQ(cands1[i].publicationKey, cands2[i].publicationKey);
  }
}

// 10. anOverlayAuthorCannotSelfAssignAuthorProminence
TEST(OverlayTest, anOverlayAuthorCannotSelfAssignAuthorProminence) {
  TestStore target("btpk:author:doc");
  const auto cA = target.makeCell("A");
  (void)cA;
  const auto targetFold = target.store->rebuildManifold(target.head);

  TestStore overlay("btpk:untrusted:doc");
  PublicKey untrustedKey;
  untrustedKey.bytes.fill(0x99);
  overlay.authorKey = untrustedKey;

  const auto phA = overlay.makeExternPlaceholder(target, cA);
  const auto cB  = overlay.makeCell("B");
  const auto d1  = overlay.makeDim("d.1");
  const auto clm = authorOverlayClaim(*overlay.store, overlay.head, phA, d1,
                                      zigzag::DimVector::POS, cB);
  overlay.head   = clm.version;

  OverlayComposition comp;
  comp
      .withTarget(OverlayComposition::TargetSpec{
          .spaceId   = 1,
          .state     = {.scroll = target.scrollKey, .version = target.head},
          .store     = target.store.get(),
          .manifold  = &targetFold,
          .author    = target.authorKey,
          .scrollKey = target.scrollKey,
      })
      .withOverlay(OverlayAttachment{
          .store          = overlay.store.get(),
          .publisher      = untrustedKey, // Untrusted key
          .publicationKey = "untrusted",
          .explicitTier   = ProminenceTier::Author, // Self-claimed "Author"!
      })
      .compose();

  ASSERT_FALSE(comp.allCandidates().empty());
  // Self-assignment rejected: evaluated as Public!
  EXPECT_EQ(comp.allCandidates().front().tier, ProminenceTier::Public);
}

// 11. aPrivateDimensionDoesNotBindBySpelling
TEST(OverlayTest, aPrivateDimensionDoesNotBindBySpelling) {
  TestStore target("btpk:target:doc");
  const auto dimTargetSupports =
      target.makeDim("supports"); // Private local dimension
  const auto cA = target.makeCell("Target Claim");
  (void)cA;
  const auto targetFold = target.store->rebuildManifold(target.head);

  TestStore overlay("btpk:overlay:doc");
  const auto dimOverlaySupports =
      overlay.makeDim("supports"); // Private local dimension
  const auto phA       = overlay.makeExternPlaceholder(target, cA);
  const auto cEvidence = overlay.makeCell("Evidence");
  const auto clm =
      authorOverlayClaim(*overlay.store, overlay.head, phA, dimOverlaySupports,
                         zigzag::DimVector::POS, cEvidence);
  overlay.head = clm.version;

  OverlayComposition comp;
  comp
      .withTarget(OverlayComposition::TargetSpec{
          .spaceId   = 1,
          .state     = {.scroll = target.scrollKey, .version = target.head},
          .store     = target.store.get(),
          .manifold  = &targetFold,
          .scrollKey = target.scrollKey,
      })
      .withOverlay(OverlayAttachment{
          .store          = overlay.store.get(),
          .publicationKey = overlay.scrollKey,
      })
      .compose();

  // The arena dimensions for target's "supports" and overlay's "supports" stay
  // distinct
  const auto arenaDimTarget  = comp.view().arenaDimFor(1, dimTargetSupports);
  const auto arenaDimOverlay = comp.allCandidates().front().dimension;
  EXPECT_NE(arenaDimTarget, arenaDimOverlay);
}

// 12. anOverlayPinsAndRebasesAsTwoReleases
TEST(OverlayTest, anOverlayPinsAndRebasesAsTwoReleases) {
  TestStore target("btpk:target:doc");
  const auto cA = target.makeCell("A");
  (void)cA;
  const auto v1 = target.head;

  TestStore overlay("btpk:overlay:doc");
  const auto rootRes = declareOverlayTarget(
      *overlay.store, overlay.head,
      GlobalDocumentState{.scroll = target.scrollKey, .version = v1});
  overlay.head = rootRes.version;

  const auto phA     = overlay.makeExternPlaceholder(target.scrollKey, v1);
  const auto cNote   = overlay.makeCell("Note for A");
  const auto dimNote = overlay.makeDim("d.notes");

  const auto clmRes =
      authorOverlayClaim(*overlay.store, overlay.head, phA, dimNote,
                         zigzag::DimVector::POS, cNote, rootRes.releaseCell);
  overlay.head = clmRes.version;

  const auto rel1 = sealOverlayRelease(*overlay.store, overlay.head,
                                       rootRes.releaseCell, "release1");
  overlay.head    = rel1.version;

  // Target evolves to v2
  const auto cB = target.makeCell("B");
  (void)cB;
  const auto v2 = target.head;

  // Rebase overlay onto v2
  const auto rebaseRes = rebaseOverlay(
      *overlay.store, overlay.head, rel1.releaseCell, *target.store,
      GlobalDocumentState{.scroll = target.scrollKey, .version = v2},
      "release2");
  overlay.head = rebaseRes.version;

  EXPECT_EQ(rebaseRes.conflictingClaims.size(), 0u);
  EXPECT_NE(rebaseRes.newReleaseCell, rel1.releaseCell);

  // Both releases are distinct and reproducible
  const auto foldAfterRebase = overlay.store->rebuildManifold(overlay.head);
  const auto dimTargets =
      *foldAfterRebase.dimensionNamed(kDimOverlayTargets, *overlay.store);

  // Old releaseCell still points to target v1
  const auto t1Cell = foldAfterRebase.linked(rel1.releaseCell, dimTargets,
                                             zigzag::DimVector::POS);
  ASSERT_NE(t1Cell, zigzag::noCell);
  const auto state1 =
      readGlobalDocumentState(foldAfterRebase.textOf(t1Cell, *overlay.store));
  ASSERT_TRUE(state1.has_value());
  EXPECT_EQ(state1->version, v1);

  // New releaseCell points to target v2
  const auto t2Cell = foldAfterRebase.linked(
      rebaseRes.newReleaseCell, dimTargets, zigzag::DimVector::POS);
  ASSERT_NE(t2Cell, zigzag::noCell);
  const auto state2 =
      readGlobalDocumentState(foldAfterRebase.textOf(t2Cell, *overlay.store));
  ASSERT_TRUE(state2.has_value());
  EXPECT_EQ(state2->version, v2);
}

// 13. anUnavailableOverlayIsNotTargetCorruption
TEST(OverlayTest, anUnavailableOverlayIsNotTargetCorruption) {
  TestStore target("btpk:target:doc");
  const auto cA = target.makeCell("A");
  (void)cA;
  const auto targetFold = target.store->rebuildManifold(target.head);

  TestStore overlay("btpk:overlay:doc");
  // Declare target scroll that does not exist in target spaces
  const auto r =
      declareOverlayTarget(*overlay.store, overlay.head,
                           GlobalDocumentState{.scroll = "btpk:nonexistent:doc",
                                               .version = MicroversionId{}});
  overlay.head = r.version;

  const auto phUnknown =
      overlay.makeExternPlaceholder("btpk:nonexistent:doc", MicroversionId{});
  const auto cLocal = overlay.makeCell("Local");
  const auto dim    = overlay.makeDim("d.1");
  const auto clm =
      authorOverlayClaim(*overlay.store, overlay.head, phUnknown, dim,
                         zigzag::DimVector::POS, cLocal, r.targetCell);
  overlay.head = clm.version;

  OverlayComposition comp;
  comp
      .withTarget(OverlayComposition::TargetSpec{
          .spaceId   = 1,
          .state     = {.scroll = target.scrollKey, .version = target.head},
          .store     = target.store.get(),
          .manifold  = &targetFold,
          .scrollKey = target.scrollKey,
      })
      .withOverlay(OverlayAttachment{
          .store          = overlay.store.get(),
          .publicationKey = overlay.scrollKey,
          .releaseCell    = r.targetCell,
      })
      .compose();

  // Target manifold remains pristine
  EXPECT_EQ(targetFold.refusedOps(), 0u);
  EXPECT_EQ(comp.allCandidates().front().state,
            OverlayClaimState::TargetNotFetched);
}

// 14. publicOverlayDiscoveryIsBounded
TEST(OverlayTest, publicOverlayDiscoveryIsBounded) {
  TestStore target("btpk:target:doc");
  const auto cA = target.makeCell("A");
  (void)cA;
  const auto targetFold = target.store->rebuildManifold(target.head);

  std::vector<std::unique_ptr<TestStore>> publicStores;
  std::vector<OverlayAttachment> attachments;

  for (int i = 1; i <= 5; ++i) {
    auto ov = std::make_unique<TestStore>("btpk:pub" + std::to_string(i));
    const auto phA = ov->makeExternPlaceholder(target, cA);
    const auto c   = ov->makeCell("Item " + std::to_string(i));
    const auto d   = ov->makeDim("d.list");
    const auto clm = authorOverlayClaim(*ov->store, ov->head, phA, d,
                                        zigzag::DimVector::POS, c);
    ov->head       = clm.version;

    PublicKey k;
    k.bytes.fill(static_cast<std::uint8_t>(i));
    attachments.push_back(OverlayAttachment{
        .store          = ov->store.get(),
        .publisher      = k,
        .publicationKey = "pub" + std::to_string(i),
        .sequence       = i * 10, // Sequences: 10, 20, 30, 40, 50
    });
    publicStores.push_back(std::move(ov));
  }

  OverlayComposition comp;
  comp
      .withTarget(OverlayComposition::TargetSpec{
          .spaceId   = 1,
          .state     = {.scroll = target.scrollKey, .version = target.head},
          .store     = target.store.get(),
          .manifold  = &targetFold,
          .scrollKey = target.scrollKey,
      })
      .withMaxPublicOverlays(2); // Bound to top 2!

  for (const auto &att : attachments) {
    comp.withOverlay(att);
  }
  comp.compose();

  // Only top 2 public overlays (pub5 sequence 50 and pub4 sequence 40) were
  // arbitrated
  const auto &cands = comp.allCandidates();
  EXPECT_EQ(cands.size(), 2u);
  EXPECT_EQ(cands[0].publicationKey, "pub5");
  EXPECT_EQ(cands[1].publicationKey, "pub4");
}

// 15. pluralButterflyLinksShareAnEndpoint
TEST(OverlayTest, pluralButterflyLinksShareAnEndpoint) {
  TestStore target("btpk:author:doc");
  const auto targetFold = target.store->rebuildManifold(target.head);

  const GlobalSpan sharedEndpoint{
      .scroll = target.scrollKey, .start = 100, .length = 50};
  const GlobalSpan endB{.scroll = "btpk:b:doc", .start = 200, .length = 30};
  const GlobalSpan endC{.scroll = "btpk:c:doc", .start = 300, .length = 40};

  GlobalLink link1;
  link1.type = LinkType::Comment;
  link1.left.push_back(sharedEndpoint);
  link1.right.push_back(endB);

  GlobalLink link2;
  link2.type = LinkType::Quotation;
  link2.left.push_back(sharedEndpoint);
  link2.right.push_back(endC);

  OverlayComposition comp;
  comp
      .withTarget(OverlayComposition::TargetSpec{
          .spaceId   = 1,
          .state     = {.scroll = target.scrollKey, .version = target.head},
          .store     = target.store.get(),
          .manifold  = &targetFold,
          .scrollKey = target.scrollKey,
      })
      .withClassicLink(link1)
      .withClassicLink(link2)
      .compose();

  // Both classic butterfly links sharing the same endpoint span remain
  // discoverable
  const auto matching = comp.classicLinksTouching(sharedEndpoint);
  ASSERT_EQ(matching.size(), 2u);
  EXPECT_EQ(matching[0].type, LinkType::Comment);
  EXPECT_EQ(matching[1].type, LinkType::Quotation);
}

} // namespace

#if defined(__clang__)
#pragma clang diagnostic pop
#endif
