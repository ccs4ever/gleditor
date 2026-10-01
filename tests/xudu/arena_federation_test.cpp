/**
 * @file arena_federation_test.cpp
 * @brief Unit tests for Section 5.7 (Arena Federation: tissue of manifolds with
 * scoped presentation cells).
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "common/xanadu/extern_ref.hpp"
#include "common/xanadu/microversion.hpp"
#include "common/xanadu/ops.hpp"
#include "common/xanadu/publication.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/manifold.hpp"
#include <gleditor/cpp26.hpp>
#include <gleditor/ranges.hpp>

namespace {

namespace fs = std::filesystem;
using xanadu::ExternOpRef;
using xanadu::GlobalOpRef;
using xanadu::MicroversionId;
using xanadu::opRefOf;
using xanadu::PrimediaSpan;
using xanadu::Scroll;
using xanadu::Store;
using xanadu::UserPermascroll;
using xanadu::ValueKind;
using zigzag::ArenaManifold;
using zigzag::CellRef;
using zigzag::DimensionBindingMode;
using zigzag::DimRef;
using zigzag::DimVector;
using zigzag::ForeignRef;
using zigzag::isEphemeral;
using zigzag::Manifold;
using zigzag::noCell;
using zigzag::promote;
using zigzag::QuoteViewId;
using zigzag::Space;

fs::path tempStoreDir(const std::string &name) {
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto dir = fs::temp_directory_path() /
                   ("arena_fed_test_" + name + "_" + std::to_string(now));
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

std::shared_ptr<Store> createTestStore(const fs::path &dir) {
  UserPermascroll::Config cfg;
  cfg.storageDir = dir / "permascroll";
  auto perma     = std::make_shared<UserPermascroll>(cfg);
  return std::make_shared<Store>(perma);
}

// 1. twoStoresWithTheSameOperationIndexDoNotCollide
TEST(ArenaFederationTest, TwoStoresWithTheSameOperationIndexDoNotCollide) {
  const auto dir1 = tempStoreDir("store1");
  const auto dir2 = tempStoreDir("store2");
  auto store1     = createTestStore(dir1);
  auto store2     = createTestStore(dir2);

  auto v1_0        = store1->sliceGenesis(MicroversionId{});
  auto v1_1        = store1->makeCell(v1_0, "doc1_cell");
  const auto cell1 = store1->cellRefOf(v1_1);

  auto v2_0        = store2->sliceGenesis(MicroversionId{});
  auto v2_1        = store2->makeCell(v2_0, "doc2_cell");
  const auto cell2 = store2->cellRefOf(v2_1);

  EXPECT_EQ(cell1, cell2); // Same local operation index

  auto m1 = store1->rebuildManifold(v1_1);
  auto m2 = store2->rebuildManifold(v2_1);

  ArenaManifold arena;
  const auto space1 = arena.attach(Space{.manifold = &m1,
                                         .store    = store1.get(),
                                         .reader   = store1.get(),
                                         .label    = "doc1"});
  const auto space2 = arena.attach(Space{.manifold = &m2,
                                         .store    = store2.get(),
                                         .reader   = store2.get(),
                                         .label    = "doc2"});

  EXPECT_EQ(space1, 1u);
  EXPECT_EQ(space2, 2u);

  const CellRef proxy1 = arena.proxyFor(space1, cell1);
  const CellRef proxy2 = arena.proxyFor(space2, cell2);

  EXPECT_NE(proxy1, proxy2);
  EXPECT_TRUE(arena.isProxy(proxy1));
  EXPECT_TRUE(arena.isProxy(proxy2));

  EXPECT_EQ(arena.textOf(proxy1, store1.get()), "doc1_cell");
  EXPECT_EQ(arena.textOf(proxy2, store2.get()), "doc2_cell");

  const auto f1 = arena.foreignOf(proxy1);
  const auto f2 = arena.foreignOf(proxy2);
  ASSERT_TRUE(f1.has_value());
  ASSERT_TRUE(f2.has_value());
  EXPECT_EQ(f1->space, space1);
  EXPECT_EQ(f1->index, cell1);
  EXPECT_EQ(f2->space, space2);
  EXPECT_EQ(f2->index, cell2);

  std::vector<CellRef> proxies1;
  arena.forEachProxy(space1, [&](const CellRef p, const CellRef f) {
    proxies1.push_back(p);
    EXPECT_EQ(f, cell1);
  });
  EXPECT_EQ(proxies1.size(), 1u);
  EXPECT_EQ(proxies1[0], proxy1);
}

// 2. aProxyIsMintedOncePerForeignCell
TEST(ArenaFederationTest, AProxyIsMintedOncePerForeignCell) {
  ArenaManifold arena;
  Space sp{.label = "test_space"};
  const auto spaceId = arena.attach(sp);

  const CellRef p1 = arena.proxyFor(spaceId, 42);
  const CellRef p2 = arena.proxyFor(spaceId, 42);
  EXPECT_EQ(p1, p2);

  const CellRef occ1 = arena.quoteOccurrence(1001, p1);
  const CellRef occ2 = arena.quoteOccurrence(1002, p1);
  EXPECT_NE(occ1, occ2);
  EXPECT_TRUE(arena.isQuoteOccurrence(occ1));
  EXPECT_TRUE(arena.isQuoteOccurrence(occ2));
  EXPECT_FALSE(arena.isProxy(occ1));
  EXPECT_FALSE(arena.isProxy(occ2));
  EXPECT_EQ(arena.canonicalProxyOf(occ1), p1);
  EXPECT_EQ(arena.canonicalProxyOf(occ2), p1);

  std::size_t visited = 0;
  arena.forEachQuoteOccurrence(1001, [&](const CellRef occ, const CellRef can) {
    visited++;
    EXPECT_EQ(occ, occ1);
    EXPECT_EQ(can, p1);
  });
  EXPECT_EQ(visited, 1u);
}

// 3. aProxyNamesItsStoreOnDStoreRefs
TEST(ArenaFederationTest, AProxyNamesItsStoreOnDStoreRefs) {
  ArenaManifold arena;
  Space sp{.label = "space_a"};
  const auto spaceId = arena.attach(sp);
  const auto spOpt   = arena.spaceAt(spaceId);
  ASSERT_TRUE(spOpt.has_value());
  const CellRef storeCell = spOpt->storeCell;
  EXPECT_NE(storeCell, noCell);

  const CellRef proxy     = arena.proxyFor(spaceId, 99);
  const auto dimStoreRefs = arena.ensureDimension("d.store-refs");

  const CellRef negCell = arena.linked(proxy, dimStoreRefs, DimVector::NEG);
  EXPECT_EQ(negCell, storeCell);
}

// 4. manyProxiesDoNotDisplaceTheStoresRank
TEST(ArenaFederationTest, ManyProxiesDoNotDisplaceTheStoresRank) {
  ArenaManifold arena;
  const auto s1 = arena.attach(Space{.label = "s1"});
  const auto s2 = arena.attach(Space{.label = "s2"});

  const CellRef sc1    = arena.spaceAt(s1)->storeCell;
  const CellRef sc2    = arena.spaceAt(s2)->storeCell;
  const auto dimStores = arena.ensureDimension("d.stores");

  EXPECT_EQ(arena.linked(sc1, dimStores, DimVector::POS), sc2);

  for (CellRef idx = 10; idx < 15; ++idx) {
    arena.proxyFor(s1, idx);
    arena.proxyFor(s2, idx);
  }

  // d.stores rank is unchanged
  EXPECT_EQ(arena.linked(sc1, dimStores, DimVector::POS), sc2);
  EXPECT_EQ(arena.linked(sc2, dimStores, DimVector::NEG), sc1);

  const auto dimStoreRefs = arena.ensureDimension("d.store-refs");
  CellRef cur             = sc1;
  std::size_t count1      = 0;
  while (true) {
    const CellRef nxt = arena.linked(cur, dimStoreRefs, DimVector::POS);
    if (nxt == noCell) break;
    cur = nxt;
    count1++;
  }
  EXPECT_EQ(count1, 5u);
}

// 5. aQuotationOccurrenceCannotEscapeItsInducedEdges
TEST(ArenaFederationTest, AQuotationOccurrenceCannotEscapeItsInducedEdges) {
  const auto dir = tempStoreDir("quotation_scope");
  auto store     = createTestStore(dir);

  auto v0 = store->sliceGenesis(MicroversionId{});
  auto d1 = store->makeDimension(v0, "d.1");
  auto cA = store->makeCell(d1.version, "A");
  auto cB = store->makeCell(cA, "B");
  auto cC = store->makeCell(cB, "C");
  auto cD = store->makeCell(cC, "D");

  auto vL1 = store->setLink(cD, store->cellRefOf(cA), d1.dim, false,
                            store->cellRefOf(cB));
  auto vL2 = store->setLink(vL1, store->cellRefOf(cB), d1.dim, false,
                            store->cellRefOf(cC));
  auto vL3 = store->setLink(vL2, store->cellRefOf(cC), d1.dim, false,
                            store->cellRefOf(cD));

  auto manifold = store->rebuildManifold(vL3);

  ArenaManifold arena;
  const auto sp = arena.attach(
      Space{.manifold = &manifold, .store = store.get(), .label = "doc"});

  const CellRef pA = arena.proxyFor(sp, store->cellRefOf(cA));
  const CellRef pB = arena.proxyFor(sp, store->cellRefOf(cB));
  const CellRef pC = arena.proxyFor(sp, store->cellRefOf(cC));

  const QuoteViewId view = 42;
  const CellRef qA       = arena.quoteOccurrence(view, pA);
  const CellRef qB       = arena.quoteOccurrence(view, pB);
  const CellRef qC       = arena.quoteOccurrence(view, pC);

  const auto dimArena1 = arena.ensureDimension("d.1");
  zigzag::expectWritten(arena.link(qA, dimArena1, DimVector::POS, qB));
  zigzag::expectWritten(arena.link(qB, dimArena1, DimVector::POS, qC));

  EXPECT_EQ(arena.linked(qA, dimArena1, DimVector::POS), qB);
  EXPECT_EQ(arena.linked(qB, dimArena1, DimVector::POS), qC);
  EXPECT_EQ(arena.linked(qC, dimArena1, DimVector::POS),
            noCell); // Boundary hop returns noCell
  EXPECT_EQ(arena.linked(qA, dimArena1, DimVector::NEG), noCell);
}

// 6. overlappingQuotationsKeepIndependentScopes
TEST(ArenaFederationTest, OverlappingQuotationsKeepIndependentScopes) {
  ArenaManifold arena;
  const auto sp = arena.attach(Space{.label = "src"});

  const CellRef pA = arena.proxyFor(sp, 1);
  const CellRef pB = arena.proxyFor(sp, 2);
  const CellRef pC = arena.proxyFor(sp, 3);

  const QuoteViewId view1 = 1;
  const QuoteViewId view2 = 2;

  const CellRef qA1 = arena.quoteOccurrence(view1, pA);
  const CellRef qB1 = arena.quoteOccurrence(view1, pB);

  const CellRef qB2 = arena.quoteOccurrence(view2, pB);
  const CellRef qC2 = arena.quoteOccurrence(view2, pC);

  const auto dim1 = arena.ensureDimension("d.1");
  const auto dim2 = arena.ensureDimension("d.2");

  zigzag::expectWritten(arena.link(qA1, dim1, DimVector::POS, qB1));
  zigzag::expectWritten(arena.link(qB2, dim2, DimVector::POS, qC2));

  EXPECT_EQ(arena.linked(qB1, dim2, DimVector::POS), noCell);
  EXPECT_EQ(arena.linked(qB2, dim1, DimVector::NEG), noCell);
}

// 7. aReadOnlyHopDoesNotMintAProxy
TEST(ArenaFederationTest, AReadOnlyHopDoesNotMintAProxy) {
  const auto dir = tempStoreDir("readonly_hop");
  auto store     = createTestStore(dir);

  auto v0 = store->sliceGenesis(MicroversionId{});
  auto d1 = store->makeDimension(v0, "d.1");
  auto cX = store->makeCell(d1.version, "X");
  auto cY = store->makeCell(cX, "Y");
  auto vL = store->setLink(cY, store->cellRefOf(cX), d1.dim, false,
                           store->cellRefOf(cY));

  auto manifold = store->rebuildManifold(vL);

  ArenaManifold arena;
  const auto sp =
      arena.attach(Space{.manifold = &manifold, .store = store.get()});

  const CellRef pX     = arena.proxyFor(sp, store->cellRefOf(cX));
  const auto dimArena1 = arena.ensureDimension("d.1");

  const std::size_t countBefore = arena.cellCount();
  const CellRef ans             = arena.linked(pX, dimArena1, DimVector::POS);

  EXPECT_EQ(ans, noCell);
  EXPECT_EQ(arena.cellCount(), countBefore);
}

// 8. aFederatedHopAnswersTheForeignCell and aWalkInsideASpaceNeverLeavesIt
TEST(ArenaFederationTest,
     AFederatedHopAnswersTheForeignCellAndWalkInsideNeverLeaves) {
  const auto dir = tempStoreDir("federated_chain");
  auto store     = createTestStore(dir);

  auto vCur = store->sliceGenesis(MicroversionId{});
  auto d1   = store->makeDimension(vCur, "d.1");
  vCur      = d1.version;

  std::vector<CellRef> cells;
  for (int i = 0; i < 10; ++i) {
    vCur = store->makeCell(vCur, "cell_" + std::to_string(i));
    cells.push_back(store->cellRefOf(vCur));
  }
  for (std::size_t i = 0; i < 9; ++i) {
    vCur = store->setLink(vCur, cells[i], d1.dim, false, cells[i + 1]);
  }

  auto manifold = store->rebuildManifold(vCur);

  ArenaManifold arena;
  const auto sp =
      arena.attach(Space{.manifold = &manifold, .store = store.get()});
  const auto dimArena1 = arena.ensureDimension("d.1");

  arena.materializeFrontier(sp, cells[0], d1.dim, DimVector::POS, 10);

  CellRef cur = arena.findExistingProxy(sp, cells[0]);
  EXPECT_NE(cur, noCell);

  for (std::size_t i = 0; i < 9; ++i) {
    const CellRef nxt = arena.linked(cur, dimArena1, DimVector::POS);
    ASSERT_NE(nxt, noCell);
    const auto foreign = arena.foreignOf(nxt);
    ASSERT_TRUE(foreign.has_value());
    EXPECT_EQ(foreign->space, sp);
    EXPECT_EQ(foreign->index, cells[i + 1]);
    cur = nxt;
  }
  EXPECT_EQ(arena.linked(cur, dimArena1, DimVector::POS), noCell);
}

// 9. aFederatedRefKeepsItsIdentity
TEST(ArenaFederationTest, AFederatedRefKeepsItsIdentity) {
  const auto dir = tempStoreDir("identity");
  auto store     = createTestStore(dir);

  auto v0         = store->sliceGenesis(MicroversionId{});
  auto v1         = store->makeCell(v0, "ident");
  const auto cRef = store->cellRefOf(v1);

  ArenaManifold arena;
  const auto sp =
      arena.attach(Space{.store = store.get(), .label = "ident_doc"});

  const CellRef proxy = arena.proxyFor(sp, cRef);
  const auto resolved = arena.resolveForeign(proxy);
  ASSERT_TRUE(resolved.has_value());
  EXPECT_EQ(resolved->first, store.get());
  EXPECT_EQ(resolved->second, cRef);
}

// 10. attachingAStoreCopiesNothing
TEST(ArenaFederationTest, AttachingAStoreCopiesNothing) {
  const auto dir1 = tempStoreDir("copy_nothing_1");
  const auto dir2 = tempStoreDir("copy_nothing_2");
  auto store1     = createTestStore(dir1);
  auto store2     = createTestStore(dir2);

  auto v1 = store1->sliceGenesis(MicroversionId{});
  auto v2 = store2->sliceGenesis(MicroversionId{});
  for (int i = 0; i < 20; ++i) {
    v1 = store1->makeCell(v1, "s1_" + std::to_string(i));
    v2 = store2->makeCell(v2, "s2_" + std::to_string(i));
  }
  auto m1 = store1->rebuildManifold(v1);
  auto m2 = store2->rebuildManifold(v2);

  ArenaManifold arena;
  const auto s1 = arena.attach(
      Space{.manifold = &m1, .store = store1.get(), .label = "doc1"});
  const auto s2 = arena.attach(
      Space{.manifold = &m2, .store = store2.get(), .label = "doc2"});
  (void)s2;

  // Stores have 40 cells combined, but attaching copied 0 of them into arena!
  const std::size_t afterAttach = arena.cellCount();
  EXPECT_LT(afterAttach, 10u);

  // Minting one proxy increments count by exactly 1
  arena.proxyFor(s1, store1->cellRefOf(v1));
  EXPECT_EQ(arena.cellCount(), afterAttach + 1);

  // Minting same proxy again does not mint additional cells
  arena.proxyFor(s1, store1->cellRefOf(v1));
  EXPECT_EQ(arena.cellCount(), afterAttach + 1);
}

// 11. shadowedForeignLinkWinsOverDelegation
TEST(ArenaFederationTest, ShadowedForeignLinkWinsOverDelegation) {
  const auto dir = tempStoreDir("shadow_test");
  auto store     = createTestStore(dir);

  auto v0 = store->sliceGenesis(MicroversionId{});
  auto d1 = store->makeDimension(v0, "d.1");
  auto cA = store->makeCell(d1.version, "A");
  auto cB = store->makeCell(cA, "B");
  auto vL = store->setLink(cB, store->cellRefOf(cA), d1.dim, false,
                           store->cellRefOf(cB));

  auto manifold = store->rebuildManifold(vL);

  ArenaManifold arena;
  const auto sp = arena.attach(
      Space{.manifold = &manifold, .store = store.get(), .label = "doc"});
  const auto dimArena1 = arena.ensureDimension("d.1");

  const CellRef pA = arena.proxyFor(sp, store->cellRefOf(cA));
  const CellRef pB = arena.proxyFor(sp, store->cellRefOf(cB));

  EXPECT_EQ(arena.linked(pA, dimArena1, DimVector::POS), pB);

  // Local break to noCell
  zigzag::expectWritten(arena.link(pA, dimArena1, DimVector::POS, noCell));
  EXPECT_EQ(arena.linked(pA, dimArena1, DimVector::POS), noCell);

  // Foreign manifold is completely untouched
  EXPECT_EQ(manifold.linked(store->cellRefOf(cA), d1.dim, DimVector::POS),
            store->cellRefOf(cB));

  // Local override to custom cell
  const CellRef custom = arena.makeCell("local_override");
  zigzag::expectWritten(arena.link(pA, dimArena1, DimVector::POS, custom));
  EXPECT_EQ(arena.linked(pA, dimArena1, DimVector::POS), custom);
}

// 12. promotingAProxyFilesAPlaceholder
TEST(ArenaFederationTest, PromotingAProxyFilesAPlaceholder) {
  const auto dirF   = tempStoreDir("foreign_store");
  const auto dirL   = tempStoreDir("local_store");
  auto foreignStore = createTestStore(dirF);
  auto localStore   = createTestStore(dirL);

  auto vF0             = foreignStore->sliceGenesis(MicroversionId{});
  auto vF1             = foreignStore->makeCell(vF0, "foreign payload");
  const auto fCell     = foreignStore->cellRefOf(vF1);
  auto foreignManifold = foreignStore->rebuildManifold(vF1);

  ArenaManifold arena;
  const auto sp = arena.attach(Space{.manifold = &foreignManifold,
                                     .store    = foreignStore.get(),
                                     .label    = "btpk:testforeignkey"});

  const CellRef pForeign = arena.proxyFor(sp, fCell);
  const CellRef cLocal   = arena.makeCell("local cell");
  const auto dimLink     = arena.makeCell("d.link");
  zigzag::expectWritten(arena.link(cLocal, dimLink, DimVector::POS, pForeign));

  auto promotedOpt = promote(
      *localStore, localStore->sliceGenesis(MicroversionId{}), arena, cLocal);
  ASSERT_TRUE(promotedOpt.has_value());

  auto localFolded = localStore->rebuildManifold(promotedOpt->version);
  ASSERT_EQ(promotedOpt->cells.size(), 3u);

  CellRef placeholder = noCell;
  for (const auto cell : promotedOpt->cells) {
    if (localFolded.valueKindOf(cell) == ValueKind::ExternRef) {
      placeholder = cell;
      break;
    }
  }
  ASSERT_NE(placeholder, noCell);
  EXPECT_EQ(localFolded.valueKindOf(placeholder), ValueKind::ExternRef);
}

// 13. forkingAnOccurrenceAuthorsLocalStructureWithoutCopyingPrimedia
TEST(ArenaFederationTest,
     ForkingAnOccurrenceAuthorsLocalStructureWithoutCopyingPrimedia) {
  const auto dirF   = tempStoreDir("foreign_fork");
  const auto dirL   = tempStoreDir("local_fork");
  auto foreignStore = createTestStore(dirF);
  auto localStore   = createTestStore(dirL);

  auto vF0             = foreignStore->sliceGenesis(MicroversionId{});
  auto vF1             = foreignStore->makeCell(vF0, "shared primedia text");
  const auto fCell     = foreignStore->cellRefOf(vF1);
  auto foreignManifold = foreignStore->rebuildManifold(vF1);

  ArenaManifold arena;
  const auto sp = arena.attach(Space{.manifold = &foreignManifold,
                                     .store    = foreignStore.get(),
                                     .label    = "foreign_doc"});

  const CellRef proxy = arena.proxyFor(sp, fCell);
  const CellRef qOcc  = arena.quoteOccurrence(1, proxy);

  const auto localGenesis = localStore->sliceGenesis(MicroversionId{});
  auto promotedOpt        = promote(*localStore, localGenesis, arena, qOcc);
  ASSERT_TRUE(promotedOpt.has_value());

  auto localFolded       = localStore->rebuildManifold(promotedOpt->version);
  const CellRef authored = promotedOpt->cells.front();

  // Preserves foreign span without copying into local scratch
  const auto spans = localFolded.contentOf(authored);
  ASSERT_FALSE(spans.empty());
  EXPECT_NE(spans.front().scroll, xanadu::scratchScroll);
}

// 14. aBoundDimensionResolvesPerSpace and aBoundDimensionRecordsHowItWasBound
TEST(ArenaFederationTest, ABoundDimensionResolvesPerSpaceAndRecordsMode) {
  ArenaManifold arena;
  const auto sp1 = arena.attach(Space{.label = "sp1"});
  const auto sp2 = arena.attach(Space{.label = "sp2"});

  const auto dArena = arena.ensureDimension("d.global");
  arena.bindDimension(dArena, sp1, 101, DimensionBindingMode::Explicit);
  arena.bindDimension(dArena, sp2, 202, DimensionBindingMode::NameMatch);

  EXPECT_EQ(arena.dimIn(sp1, dArena), 101u);
  EXPECT_EQ(arena.dimIn(sp2, dArena), 202u);

  const auto bSetOpt = arena.boundDimensionSet(dArena);
  ASSERT_TRUE(bSetOpt.has_value());
  EXPECT_EQ(bSetOpt->members.size(), 2u);
  EXPECT_EQ(bSetOpt->members[0].space, sp1);
  EXPECT_EQ(bSetOpt->members[0].dim, 101u);
  EXPECT_EQ(bSetOpt->members[0].mode, DimensionBindingMode::Explicit);
  EXPECT_EQ(bSetOpt->members[1].space, sp2);
  EXPECT_EQ(bSetOpt->members[1].dim, 202u);
  EXPECT_EQ(bSetOpt->members[1].mode, DimensionBindingMode::NameMatch);
}

// 15. proxiesVanishWithTheirMark
TEST(ArenaFederationTest, ProxiesVanishWithTheirMark) {
  ArenaManifold arena;
  const auto sp   = arena.attach(Space{.label = "mark_space"});
  const auto dim1 = arena.ensureDimension("d.1");

  const auto mark   = arena.mark();
  const CellRef p   = arena.proxyFor(sp, 50);
  const CellRef occ = arena.quoteOccurrence(1, p);
  zigzag::expectWritten(arena.link(p, dim1, DimVector::POS, noCell));

  EXPECT_EQ(arena.findExistingProxy(sp, 50), p);
  EXPECT_TRUE(arena.isQuoteOccurrence(occ));

  arena.release(mark);

  EXPECT_EQ(arena.findExistingProxy(sp, 50), noCell);
  EXPECT_FALSE(arena.isQuoteOccurrence(occ));
}

// 16. aMintAtTheRefCeilingIsRefused
TEST(ArenaFederationTest, AMintAtTheRefCeilingIsRefused) {
  ArenaManifold arena;
  arena.setAllocationLimitForTesting(
      static_cast<std::uint32_t>(arena.cellCount() + 1));

  EXPECT_NO_THROW({ arena.makeCell("first"); });
  try {
    arena.makeCell("second");
    FAIL() << "expected exception on ceiling exhaustion";
  } catch (const std::runtime_error &e) {
    EXPECT_THAT(e.what(), ::testing::HasSubstr("cannot allocate beyond"));
  }
}

} // namespace
