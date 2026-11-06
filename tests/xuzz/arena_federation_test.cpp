/**
 * @file arena_federation_test.cpp
 * @brief Unit tests for Section 5.7 (Arena Federation: tissue of manifolds with
 * scoped presentation cells).
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
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

struct ForeignLine {
  std::shared_ptr<Store> store;
  Manifold manifold;
  DimRef dim{noCell};
  std::vector<CellRef> cells;
};

ForeignLine
makeForeignLine(const std::string &name, const std::size_t count,
                const std::vector<std::pair<std::size_t, std::size_t>> &edges) {
  auto store           = createTestStore(tempStoreDir(name));
  auto version         = store->sliceGenesis(MicroversionId{});
  const auto dimension = store->makeDimension(version, "d.edge");
  version              = dimension.version;

  std::vector<CellRef> cells;
  cells.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    version = store->makeCell(version, "cell " + std::to_string(index));
    cells.push_back(store->cellRefOf(version).value());
  }
  for (const auto &[from, to] : edges) {
    version =
        store->setLink(version, cells[from], dimension.dim, false, cells[to]);
  }

  auto manifold = store->rebuildManifold(version);
  return ForeignLine{.store    = std::move(store),
                     .manifold = std::move(manifold),
                     .dim      = dimension.dim,
                     .cells    = std::move(cells)};
}

std::pair<CellRef, DimRef>
hydrateFullStoreRefsTail(ArenaManifold &arena, std::vector<CellRef> &refs) {
  ArenaManifold source;
  const DimRef dimStores = source.ensureDimension("d.stores");
  refs.reserve(zigzag::maxCellLinkRunCount + 2);
  refs.push_back(dimStores);
  for (std::size_t index = 1; index < zigzag::maxCellLinkRunCount + 2;
       ++index) {
    refs.push_back(source.makeCell());
  }

  const auto mark = source.mark();
  auto snapshot   = source.exportPrefix(mark);
  if (!snapshot) {
    throw std::runtime_error("could not export saturated arena fixture");
  }
  snapshot->links.reserve(zigzag::maxCellLinkRunCount);
  for (std::size_t index = 0; index < zigzag::maxCellLinkRunCount; ++index) {
    snapshot->links.push_back(zigzag::DimLink{.dim = refs[index]});
  }
  snapshot->links.front().pos = refs[1];
  auto &storeCell             = snapshot->slots.back();
  storeCell.linkOffset        = 0;
  storeCell.linkCount = static_cast<std::uint16_t>(zigzag::maxCellLinkRunCount);
  snapshot->mark.linkSize =
      static_cast<std::uint32_t>(zigzag::maxCellLinkRunCount);
  snapshot->mark.liveLinks =
      static_cast<std::uint32_t>(zigzag::maxCellLinkRunCount);

  if (!arena.hydratePrefix(*snapshot)) {
    throw std::runtime_error("could not hydrate saturated arena fixture");
  }
  return {refs.back(), dimStores};
}

TEST(ArenaFederationTest, SourceIdentityIncludesBaseCellsButNotScratchQuotes) {
  const auto dir      = tempStoreDir("source_identity");
  const auto source   = createTestStore(dir);
  const auto genesis  = source->sliceGenesis({});
  const auto head     = source->makeCell(genesis, "authored");
  const auto cell     = source->cellRefOf(head).value();
  const auto manifold = source->rebuildManifold(head);
  ArenaManifold arena;
  const auto space = arena.attach(Space{.manifold = &manifold,
                                        .store    = source.get(),
                                        .reader   = source.get(),
                                        .label    = "source"});
  const auto expected =
      std::make_pair(static_cast<const Store *>(source.get()), cell);
  ASSERT_TRUE(arena.resolveSourceCell(cell));
  EXPECT_EQ(*arena.resolveSourceCell(cell), expected);
  const auto proxy = arena.proxyFor(space, cell);
  EXPECT_EQ(*arena.resolveSourceCell(proxy), expected);
  EXPECT_EQ(
      *arena.resolveSourceCell(arena.quoteOccurrence(QuoteViewId{1}, proxy)),
      expected);
  EXPECT_FALSE(arena.resolveSourceCell(arena.makeCell("constructed")));
  EXPECT_FALSE(arena.resolveSourceCell(
      arena.makeQuote(space, manifold.contentOf(cell))));
  fs::remove_all(dir);
}

// 1. twoStoresWithTheSameOperationIndexDoNotCollide
TEST(ArenaFederationTest, TwoStoresWithTheSameOperationIndexDoNotCollide) {
  const auto dir1 = tempStoreDir("store1");
  const auto dir2 = tempStoreDir("store2");
  auto store1     = createTestStore(dir1);
  auto store2     = createTestStore(dir2);

  auto v1_0        = store1->sliceGenesis(MicroversionId{});
  auto v1_1        = store1->makeCell(v1_0, "doc1_cell");
  const auto cell1 = store1->cellRefOf(v1_1).value();

  auto v2_0        = store2->sliceGenesis(MicroversionId{});
  auto v2_1        = store2->makeCell(v2_0, "doc2_cell");
  const auto cell2 = store2->cellRefOf(v2_1).value();

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
  ASSERT_TRUE(spOpt->storeCell);
  const CellRef storeCell = *spOpt->storeCell;

  const CellRef proxy     = arena.proxyFor(spaceId, 99);
  const auto dimStoreRefs = arena.ensureDimension("d.store-refs");

  const auto negCell = arena.linked(proxy, dimStoreRefs, DimVector::NEG);
  ASSERT_TRUE(negCell);
  EXPECT_EQ(*negCell, storeCell);
}

// 4. manyProxiesDoNotDisplaceTheStoresRank
TEST(ArenaFederationTest, ManyProxiesDoNotDisplaceTheStoresRank) {
  ArenaManifold arena;
  const auto s1 = arena.attach(Space{.label = "s1"});
  const auto s2 = arena.attach(Space{.label = "s2"});

  ASSERT_TRUE(arena.spaceAt(s1)->storeCell);
  ASSERT_TRUE(arena.spaceAt(s2)->storeCell);
  const CellRef sc1    = *arena.spaceAt(s1)->storeCell;
  const CellRef sc2    = *arena.spaceAt(s2)->storeCell;
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
  auto cur                = std::optional<CellRef>{sc1};
  std::size_t count1      = 0;
  while (cur) {
    cur = arena.linked(*cur, dimStoreRefs, DimVector::POS);
    if (!cur) break;
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

  auto vL1 = store->setLink(cD, store->cellRefOf(cA).value(), d1.dim, false,
                            store->cellRefOf(cB).value());
  auto vL2 = store->setLink(vL1, store->cellRefOf(cB).value(), d1.dim, false,
                            store->cellRefOf(cC).value());
  auto vL3 = store->setLink(vL2, store->cellRefOf(cC).value(), d1.dim, false,
                            store->cellRefOf(cD).value());

  auto manifold = store->rebuildManifold(vL3);

  ArenaManifold arena;
  const auto sp = arena.attach(
      Space{.manifold = &manifold, .store = store.get(), .label = "doc"});

  const CellRef pA = arena.proxyFor(sp, store->cellRefOf(cA).value());
  const CellRef pB = arena.proxyFor(sp, store->cellRefOf(cB).value());
  const CellRef pC = arena.proxyFor(sp, store->cellRefOf(cC).value());

  const QuoteViewId view = 42;
  const CellRef qA       = arena.quoteOccurrence(view, pA);
  const CellRef qB       = arena.quoteOccurrence(view, pB);
  const CellRef qC       = arena.quoteOccurrence(view, pC);

  const auto dimArena1 = arena.ensureDimension("d.1");
  zigzag::expectWritten(arena.link(qA, dimArena1, DimVector::POS, qB));
  zigzag::expectWritten(arena.link(qB, dimArena1, DimVector::POS, qC));

  EXPECT_EQ(arena.linked(qA, dimArena1, DimVector::POS), qB);
  EXPECT_EQ(arena.linked(qB, dimArena1, DimVector::POS), qC);
  EXPECT_FALSE(arena.linked(qC, dimArena1, DimVector::POS));
  EXPECT_FALSE(arena.linked(qA, dimArena1, DimVector::NEG));
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

  EXPECT_FALSE(arena.linked(qB1, dim2, DimVector::POS));
  EXPECT_FALSE(arena.linked(qB2, dim1, DimVector::NEG));
}

// 7. aReadOnlyHopDoesNotMintAProxy
TEST(ArenaFederationTest, AReadOnlyHopDoesNotMintAProxy) {
  const auto dir = tempStoreDir("readonly_hop");
  auto store     = createTestStore(dir);

  auto v0 = store->sliceGenesis(MicroversionId{});
  auto d1 = store->makeDimension(v0, "d.1");
  auto cX = store->makeCell(d1.version, "X");
  auto cY = store->makeCell(cX, "Y");
  auto vL = store->setLink(cY, store->cellRefOf(cX).value(), d1.dim, false,
                           store->cellRefOf(cY).value());

  auto manifold = store->rebuildManifold(vL);

  ArenaManifold arena;
  const auto sp =
      arena.attach(Space{.manifold = &manifold, .store = store.get()});

  const CellRef pX     = arena.proxyFor(sp, store->cellRefOf(cX).value());
  const auto dimArena1 = arena.ensureDimension("d.1");

  const std::size_t countBefore = arena.cellCount();
  const auto ans                = arena.linked(pX, dimArena1, DimVector::POS);

  EXPECT_FALSE(ans);
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
    cells.push_back(store->cellRefOf(vCur).value());
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

  auto cur = arena.findExistingProxy(sp, cells[0]);
  ASSERT_TRUE(cur);

  for (std::size_t i = 0; i < 9; ++i) {
    const auto nxt = arena.linked(*cur, dimArena1, DimVector::POS);
    ASSERT_TRUE(nxt);
    const auto foreign = arena.foreignOf(*nxt);
    ASSERT_TRUE(foreign.has_value());
    EXPECT_EQ(foreign->space, sp);
    EXPECT_EQ(foreign->index, cells[i + 1]);
    cur = *nxt;
  }
  EXPECT_FALSE(arena.linked(*cur, dimArena1, DimVector::POS));
}

// 9. aFederatedRefKeepsItsIdentity
TEST(ArenaFederationTest, AFederatedRefKeepsItsIdentity) {
  const auto dir = tempStoreDir("identity");
  auto store     = createTestStore(dir);

  auto v0         = store->sliceGenesis(MicroversionId{});
  auto v1         = store->makeCell(v0, "ident");
  const auto cRef = store->cellRefOf(v1).value();

  ArenaManifold arena;
  const auto sp =
      arena.attach(Space{.store = store.get(), .label = "ident_doc"});

  const CellRef proxy = arena.proxyFor(sp, cRef);
  const auto resolved = arena.resolveForeign(proxy);
  ASSERT_TRUE(resolved.has_value());
  EXPECT_EQ(resolved->first, store.get());
  EXPECT_EQ(resolved->second, cRef);
  const auto occurrence = arena.quoteOccurrence(42, proxy);
  EXPECT_NE(occurrence, proxy);
  const auto quotedSource = arena.resolveForeign(occurrence);
  ASSERT_TRUE(quotedSource);
  EXPECT_EQ(quotedSource->first, store.get());
  EXPECT_EQ(quotedSource->second, cRef);
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
  arena.proxyFor(s1, store1->cellRefOf(v1).value());
  EXPECT_EQ(arena.cellCount(), afterAttach + 1);

  // Minting same proxy again does not mint additional cells
  arena.proxyFor(s1, store1->cellRefOf(v1).value());
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
  auto vL = store->setLink(cB, store->cellRefOf(cA).value(), d1.dim, false,
                           store->cellRefOf(cB).value());

  auto manifold = store->rebuildManifold(vL);

  ArenaManifold arena;
  const auto sp = arena.attach(
      Space{.manifold = &manifold, .store = store.get(), .label = "doc"});
  const auto dimArena1 = arena.ensureDimension("d.1");

  const CellRef pA = arena.proxyFor(sp, store->cellRefOf(cA).value());
  const CellRef pB = arena.proxyFor(sp, store->cellRefOf(cB).value());

  EXPECT_EQ(arena.linked(pA, dimArena1, DimVector::POS), pB);

  // Local break to noCell
  zigzag::expectWritten(
      arena.link(pA, dimArena1, DimVector::POS, std::nullopt));
  EXPECT_FALSE(arena.linked(pA, dimArena1, DimVector::POS));

  // Foreign manifold is completely untouched
  EXPECT_EQ(
      manifold.linked(store->cellRefOf(cA).value(), d1.dim, DimVector::POS),
      store->cellRefOf(cB).value());

  // Local override to custom cell
  const CellRef custom = arena.makeCell("local_override");
  zigzag::expectWritten(arena.link(pA, dimArena1, DimVector::POS, custom));
  EXPECT_EQ(arena.linked(pA, dimArena1, DimVector::POS), custom);
}

TEST(ArenaFederationTest,
     UnlinkingAnUnmaterializedForeignHopClearsOnlyTheArenaReciprocal) {
  auto foreign    = makeForeignLine("unlink_unmaterialized", 2, {{0, 1}});
  const CellRef a = foreign.cells[0];
  const CellRef b = foreign.cells[1];
  ArenaManifold arena;
  const auto space = arena.attach(Space{.manifold = &foreign.manifold,
                                        .store    = foreign.store.get(),
                                        .label    = "foreign"});
  const auto dim   = arena.ensureDimension("d.edge");
  ASSERT_TRUE(arena.bindDimension(dim, space, foreign.dim));
  const CellRef proxyA = arena.proxyFor(space, a);

  ASSERT_FALSE(arena.findExistingProxy(space, b));
  EXPECT_FALSE(arena.linked(proxyA, dim, DimVector::POS));
  zigzag::expectWritten(arena.link(proxyA, dim, DimVector::POS, std::nullopt));

  const auto proxyB = arena.findExistingProxy(space, b);
  ASSERT_TRUE(proxyB);
  EXPECT_FALSE(arena.linked(*proxyB, dim, DimVector::NEG));
  EXPECT_EQ(foreign.manifold.linked(a, foreign.dim, DimVector::POS), b);
  EXPECT_EQ(foreign.manifold.linked(b, foreign.dim, DimVector::NEG), a);
}

TEST(ArenaFederationTest,
     ReplacingForeignEdgesOnBothEndsClearsTheirArenaReciprocals) {
  auto foreign    = makeForeignLine("replace_both_ends", 4, {{0, 1}, {3, 2}});
  const CellRef a = foreign.cells[0];
  const CellRef b = foreign.cells[1];
  const CellRef d = foreign.cells[3];
  const CellRef c = foreign.cells[2];
  ArenaManifold arena;
  const auto space = arena.attach(Space{.manifold = &foreign.manifold,
                                        .store    = foreign.store.get(),
                                        .label    = "foreign"});
  const auto dim   = arena.ensureDimension("d.edge");
  ASSERT_TRUE(arena.bindDimension(dim, space, foreign.dim));
  const CellRef proxyA = arena.proxyFor(space, a);
  const CellRef proxyC = arena.proxyFor(space, c);

  zigzag::expectWritten(arena.link(proxyA, dim, DimVector::POS, proxyC));

  const auto proxyB = arena.findExistingProxy(space, b);
  const auto proxyD = arena.findExistingProxy(space, d);
  ASSERT_TRUE(proxyB);
  ASSERT_TRUE(proxyD);
  EXPECT_FALSE(arena.linked(*proxyB, dim, DimVector::NEG));
  EXPECT_FALSE(arena.linked(*proxyD, dim, DimVector::POS));
  EXPECT_EQ(arena.linked(proxyA, dim, DimVector::POS), proxyC);
  EXPECT_EQ(arena.linked(proxyC, dim, DimVector::NEG), proxyA);
  EXPECT_EQ(foreign.manifold.linked(a, foreign.dim, DimVector::POS), b);
  EXPECT_EQ(foreign.manifold.linked(d, foreign.dim, DimVector::POS), c);
}

TEST(ArenaFederationTest,
     EditingOneProxyDirectionPreservesItsOtherDelegatedDirection) {
  auto foreign = makeForeignLine("shadow_one_direction", 3, {{0, 1}, {2, 0}});
  const CellRef a = foreign.cells[0];
  const CellRef b = foreign.cells[1];
  const CellRef c = foreign.cells[2];
  ArenaManifold arena;
  const auto space = arena.attach(Space{.manifold = &foreign.manifold,
                                        .store    = foreign.store.get(),
                                        .label    = "foreign"});
  const auto dim   = arena.ensureDimension("d.edge");
  ASSERT_TRUE(arena.bindDimension(dim, space, foreign.dim));
  const CellRef proxyA    = arena.proxyFor(space, a);
  const CellRef proxyC    = arena.proxyFor(space, c);
  const CellRef customPos = arena.makeCell("local pos");
  const CellRef customNeg = arena.makeCell("local neg");

  zigzag::expectWritten(arena.link(proxyA, dim, DimVector::POS, customPos));
  const auto proxyB = arena.findExistingProxy(space, b);
  ASSERT_TRUE(proxyB);
  EXPECT_FALSE(arena.linked(*proxyB, dim, DimVector::NEG));
  EXPECT_EQ(arena.linked(proxyA, dim, DimVector::NEG), proxyC);

  zigzag::expectWritten(arena.link(proxyA, dim, DimVector::NEG, customNeg));
  ASSERT_EQ(arena.findExistingProxy(space, c), proxyC);
  EXPECT_FALSE(arena.linked(proxyC, dim, DimVector::POS));
  EXPECT_EQ(arena.linked(proxyA, dim, DimVector::POS), customPos);
  EXPECT_EQ(arena.linked(proxyA, dim, DimVector::NEG), customNeg);
  EXPECT_EQ(arena.linked(customPos, dim, DimVector::NEG), proxyA);
}

TEST(ArenaFederationTest, ReplacementKeepsEqualForeignIndexesInTheirOwnSpaces) {
  auto first  = makeForeignLine("same_index_first", 2, {{0, 1}});
  auto second = makeForeignLine("same_index_second", 2, {{0, 1}});
  ASSERT_EQ(first.cells[0], second.cells[0]);
  ASSERT_EQ(first.cells[1], second.cells[1]);
  ArenaManifold arena;
  const auto firstSpace  = arena.attach(Space{.manifold = &first.manifold,
                                              .store    = first.store.get(),
                                              .label    = "first"});
  const auto secondSpace = arena.attach(Space{.manifold = &second.manifold,
                                              .store    = second.store.get(),
                                              .label    = "second"});
  const auto dim         = arena.ensureDimension("d.edge");
  ASSERT_TRUE(arena.bindDimension(dim, firstSpace, first.dim));
  ASSERT_TRUE(arena.bindDimension(dim, secondSpace, second.dim));
  const CellRef firstA  = arena.proxyFor(firstSpace, first.cells[0]);
  const CellRef secondB = arena.proxyFor(secondSpace, second.cells[1]);
  const CellRef firstB  = arena.proxyFor(firstSpace, first.cells[1]);
  const CellRef secondA = arena.proxyFor(secondSpace, second.cells[0]);
  ASSERT_NE(firstA, secondA);
  ASSERT_NE(firstB, secondB);

  zigzag::expectWritten(arena.link(firstA, dim, DimVector::POS, secondB));

  EXPECT_EQ(arena.linked(firstA, dim, DimVector::POS), secondB);
  EXPECT_FALSE(arena.linked(firstB, dim, DimVector::NEG));
  EXPECT_FALSE(arena.linked(secondA, dim, DimVector::POS));
  EXPECT_EQ(
      arena.foreignOf(firstB),
      (std::optional<ForeignRef>{ForeignRef{firstSpace, first.cells[1]}}));
  EXPECT_EQ(
      arena.foreignOf(secondB),
      (std::optional<ForeignRef>{ForeignRef{secondSpace, second.cells[1]}}));
  EXPECT_EQ(first.manifold.linked(first.cells[0], first.dim, DimVector::POS),
            first.cells[1]);
  EXPECT_EQ(second.manifold.linked(second.cells[0], second.dim, DimVector::POS),
            second.cells[1]);
}

TEST(ArenaFederationTest,
     ProxySelfLinkDoesNotDuplicateStoreRefsOrMintAnotherIdentity) {
  auto foreign = makeForeignLine("proxy_self_link", 1, {});
  ArenaManifold arena;
  const auto space     = arena.attach(Space{.manifold = &foreign.manifold,
                                            .store    = foreign.store.get(),
                                            .label    = "foreign"});
  const auto dim       = arena.ensureDimension("d.self");
  const CellRef proxy  = arena.proxyFor(space, foreign.cells.front());
  const auto storeRefs = arena.dimensionNamed("d.store-refs");
  ASSERT_TRUE(storeRefs);
  const CellRef storeCell = *arena.spaceAt(space)->storeCell;
  ASSERT_EQ(arena.linked(storeCell, *storeRefs, DimVector::POS), proxy);
  EXPECT_FALSE(arena.linked(proxy, *storeRefs, DimVector::POS));

  zigzag::expectWritten(arena.link(proxy, dim, DimVector::POS, proxy));

  EXPECT_EQ(arena.linked(proxy, dim, DimVector::POS), proxy);
  EXPECT_EQ(arena.linked(proxy, dim, DimVector::NEG), proxy);
  EXPECT_EQ(arena.proxyFor(space, foreign.cells.front()), proxy);
  EXPECT_EQ(arena.linked(storeCell, *storeRefs, DimVector::POS), proxy);
  EXPECT_FALSE(arena.linked(proxy, *storeRefs, DimVector::POS));
}

TEST(ArenaFederationTest, ProxyBookkeepingShadowsABaseOwnedRootStoreCell) {
  auto store                = createTestStore(tempStoreDir("proxy_base_root"));
  auto version              = store->sliceGenesis(MicroversionId{});
  version                   = store->makeCell(version, "foreign payload");
  const CellRef foreignCell = store->cellRefOf(version).value();
  const Manifold base       = store->rebuildManifold(version);
  const auto root           = base.home();
  ASSERT_TRUE(root);

  ArenaManifold arena(&base, store.get());
  const auto space     = arena.attach(Space{.manifold  = &base,
                                            .store     = store.get(),
                                            .storeCell = *root,
                                            .label     = "base root"});
  const CellRef proxy  = arena.proxyFor(space, foreignCell);
  const auto storeRefs = arena.dimensionNamed("d.store-refs");
  ASSERT_TRUE(storeRefs);

  EXPECT_TRUE(arena.holdsOwn(*root));
  EXPECT_EQ(arena.linked(*root, *storeRefs, DimVector::POS), proxy);
  EXPECT_EQ(arena.linked(proxy, *storeRefs, DimVector::NEG), *root);
  EXPECT_EQ(arena.findExistingProxy(space, foreignCell), proxy);
}

TEST(ArenaFederationTest,
     ProxyFactoryRefusesARequestedCellWithAuthoredStoreRefsEdges) {
  auto store      = createTestStore(tempStoreDir("proxy_store_refs_collision"));
  auto version    = store->sliceGenesis(MicroversionId{});
  const auto axis = store->makeDimension(version, "d.store-refs");
  version         = axis.version;
  version         = store->makeCell(version, "source A");
  const CellRef a = store->cellRefOf(version).value();
  version         = store->makeCell(version, "source B");
  const CellRef b = store->cellRefOf(version).value();
  version         = store->setLink(version, a, axis.dim, false, b);
  const Manifold manifold = store->rebuildManifold(version);

  ArenaManifold arena;
  const auto space = arena.attach(
      Space{.manifold = &manifold, .store = store.get(), .label = "source"});
  const auto internalAxis = arena.dimensionNamed("d.store-refs");
  ASSERT_TRUE(internalAxis);
  ASSERT_EQ(arena.dimIn(space, *internalAxis), axis.dim);
  const auto storeCell   = *arena.spaceAt(space)->storeCell;
  const auto cellsBefore = arena.cellCount();

  try {
    static_cast<void>(arena.proxyFor(space, a));
    FAIL()
        << "proxying a source cell on the internal store-refs axis must refuse";
  } catch (const std::logic_error &error) {
    EXPECT_THAT(
        error.what(),
        ::testing::HasSubstr(
            "proxy store-ref append conflicts with a source-axis neighbor"));
  }

  EXPECT_EQ(arena.cellCount(), cellsBefore);
  EXPECT_FALSE(arena.findExistingProxy(space, a));
  EXPECT_FALSE(arena.linked(storeCell, *internalAxis, DimVector::POS));
  std::size_t proxyCount = 0;
  arena.forEachProxy(space,
                     [&](const CellRef, const CellRef) { proxyCount++; });
  EXPECT_EQ(proxyCount, 0U);
  EXPECT_EQ(manifold.linked(a, axis.dim, DimVector::POS), b);
  EXPECT_EQ(manifold.linked(b, axis.dim, DimVector::NEG), a);
}

TEST(ArenaFederationTest,
     ProxyCollisionLookupBudgetRefusalRollsBackAndCanRetry) {
  auto foreign = makeForeignLine("proxy_collision_budget", 2, {});
  ArenaManifold arena;
  const auto space      = arena.attach(Space{.manifold = &foreign.manifold,
                                             .store    = foreign.store.get(),
                                             .label    = "foreign"});
  auto limits           = arena.dimensionLookupLimits();
  limits.directoryCells = 0;
  arena.setDimensionLookupLimits(limits);

  const auto cellsBefore        = arena.cellCount();
  const auto storeVersionBefore = foreign.store->primaryCurrentVersion();
  const auto storeCell          = *arena.spaceAt(space)->storeCell;
  EXPECT_THROW((void)arena.proxyFor(space, foreign.cells[0]),
               std::runtime_error);

  EXPECT_EQ(arena.cellCount(), cellsBefore);
  EXPECT_FALSE(arena.findExistingProxy(space, foreign.cells[0]));
  EXPECT_FALSE(arena.linked(storeCell, *arena.dimensionNamed("d.store-refs"),
                            DimVector::POS));
  EXPECT_EQ(arena.outstandingMarks(), 0U);
  EXPECT_EQ(foreign.store->primaryCurrentVersion(), storeVersionBefore);

  limits.directoryCells = 4096;
  arena.setDimensionLookupLimits(limits);
  EXPECT_NO_THROW((void)arena.proxyFor(space, foreign.cells[0]));
  EXPECT_TRUE(arena.findExistingProxy(space, foreign.cells[0]));
  EXPECT_EQ(arena.outstandingMarks(), 0U);
  EXPECT_EQ(foreign.store->primaryCurrentVersion(), storeVersionBefore);
}

TEST(ArenaFederationTest,
     ProxyCollisionLookupFuelRefusalRollsBackAndKeepsFuelSpent) {
  auto foreign = makeForeignLine("proxy_collision_fuel", 2, {});
  ArenaManifold arena;
  const auto space       = arena.attach(Space{.manifold = &foreign.manifold,
                                              .store    = foreign.store.get(),
                                              .label    = "foreign"});
  const auto cellsBefore = arena.cellCount();
  const auto storeVersionBefore = foreign.store->primaryCurrentVersion();
  const auto storeCell          = *arena.spaceAt(space)->storeCell;
  arena.setExecutionLimits(cellsBefore + 8, 1, 1024);

  EXPECT_THROW((void)arena.proxyFor(space, foreign.cells[0]),
               std::runtime_error);

  EXPECT_EQ(arena.cellCount(), cellsBefore);
  EXPECT_FALSE(arena.findExistingProxy(space, foreign.cells[0]));
  EXPECT_FALSE(arena.linked(storeCell, *arena.dimensionNamed("d.store-refs"),
                            DimVector::POS));
  EXPECT_EQ(arena.outstandingMarks(), 0U);
  EXPECT_EQ(foreign.store->primaryCurrentVersion(), storeVersionBefore);
  EXPECT_THROW(arena.consumeWork(), std::runtime_error);

  arena.setExecutionLimits(cellsBefore + 8, 4096, 1024);
  EXPECT_NO_THROW((void)arena.proxyFor(space, foreign.cells[0]));
  EXPECT_TRUE(arena.findExistingProxy(space, foreign.cells[0]));
  EXPECT_EQ(arena.outstandingMarks(), 0U);
  EXPECT_EQ(foreign.store->primaryCurrentVersion(), storeVersionBefore);
}

TEST(ArenaFederationTest,
     LateDimensionBindingRefusesAnUnshadowedSourceTailNeighbor) {
  auto firstStore = createTestStore(tempStoreDir("late_store_refs_first"));
  auto firstVer   = firstStore->sliceGenesis(MicroversionId{});
  const auto axis = firstStore->makeDimension(firstVer, "d.late-edge");
  firstVer        = axis.version;
  firstVer        = firstStore->makeCell(firstVer, "tail source");
  const CellRef tailSource = firstStore->cellRefOf(firstVer).value();
  firstVer                 = firstStore->makeCell(firstVer, "tail neighbor");
  const CellRef sourceNeighbor = firstStore->cellRefOf(firstVer).value();
  firstVer = firstStore->makeCell(firstVer, "unrelated requested source");
  const CellRef requestedSource = firstStore->cellRefOf(firstVer).value();
  firstVer = firstStore->setLink(firstVer, tailSource, axis.dim, false,
                                 sourceNeighbor);
  const Manifold firstManifold = firstStore->rebuildManifold(firstVer);

  auto secondStore = createTestStore(tempStoreDir("late_store_refs_second"));
  auto secondVer   = secondStore->sliceGenesis(MicroversionId{});
  secondVer        = secondStore->makeCell(secondVer, "other source");
  const Manifold secondManifold = secondStore->rebuildManifold(secondVer);

  ArenaManifold arena;
  const auto firstSpace   = arena.attach(Space{
        .manifold = &firstManifold, .store = firstStore.get(), .label = "first"});
  const auto secondSpace  = arena.attach(Space{.manifold = &secondManifold,
                                               .store    = secondStore.get(),
                                               .label    = "second"});
  const auto internalAxis = arena.dimensionNamed("d.store-refs");
  ASSERT_TRUE(internalAxis);
  ASSERT_FALSE(arena.dimIn(firstSpace, *internalAxis));
  const CellRef tailProxy = arena.proxyFor(firstSpace, tailSource);
  ASSERT_EQ(arena.findExistingProxy(firstSpace, tailSource), tailProxy);
  ASSERT_FALSE(arena.findExistingProxy(firstSpace, sourceNeighbor));
  ASSERT_EQ(arena.spaceCount(), 2U);

  arena.bindDimension(*internalAxis, firstSpace, axis.dim);
  ASSERT_EQ(arena.dimIn(firstSpace, *internalAxis), axis.dim);
  const auto cellsBefore = arena.cellCount();
  const auto storeCell   = *arena.spaceAt(firstSpace)->storeCell;

  try {
    static_cast<void>(arena.proxyFor(firstSpace, requestedSource));
    FAIL() << "late binding must not hide the tail's delegated POS edge";
  } catch (const std::logic_error &error) {
    EXPECT_THAT(
        error.what(),
        ::testing::HasSubstr("proxy store-ref tail has a source POS neighbor"));
  }

  EXPECT_EQ(arena.cellCount(), cellsBefore);
  EXPECT_FALSE(arena.findExistingProxy(firstSpace, requestedSource));
  EXPECT_EQ(arena.findExistingProxy(firstSpace, tailSource), tailProxy);
  EXPECT_EQ(arena.linked(storeCell, *internalAxis, DimVector::POS), tailProxy);
  EXPECT_EQ(arena.linked(tailProxy, *internalAxis, DimVector::NEG), storeCell);
  EXPECT_EQ(firstManifold.linked(tailSource, axis.dim, DimVector::POS),
            sourceNeighbor);
  EXPECT_EQ(firstManifold.linked(sourceNeighbor, axis.dim, DimVector::NEG),
            tailSource);
  std::size_t proxyCount = 0;
  arena.forEachProxy(firstSpace,
                     [&](const CellRef, const CellRef) { proxyCount++; });
  EXPECT_EQ(proxyCount, 1U);
  EXPECT_EQ(secondSpace, 2U);
}

TEST(ArenaFederationTest,
     ProxyFactoryRefusesUnboundLiteralStoreRefsAxisNeighbors) {
  auto firstStore = createTestStore(tempStoreDir("unbound_store_refs_first"));
  auto firstVer   = firstStore->sliceGenesis(MicroversionId{});
  const auto axis = firstStore->makeDimension(firstVer, "d.store-refs");
  firstVer        = axis.version;
  firstVer        = firstStore->makeCell(firstVer, "source neighbor");
  const CellRef neighbor  = firstStore->cellRefOf(firstVer).value();
  firstVer                = firstStore->makeCell(firstVer, "requested source");
  const CellRef requested = firstStore->cellRefOf(firstVer).value();
  firstVer =
      firstStore->setLink(firstVer, neighbor, axis.dim, false, requested);
  const Manifold firstManifold = firstStore->rebuildManifold(firstVer);

  auto secondStore = createTestStore(tempStoreDir("unbound_store_refs_second"));
  auto secondVer   = secondStore->sliceGenesis(MicroversionId{});
  secondVer        = secondStore->makeCell(secondVer, "second source");
  const Manifold secondManifold = secondStore->rebuildManifold(secondVer);

  ArenaManifold arena;
  const auto firstSpace = arena.attach(Space{
      .manifold = &firstManifold, .store = firstStore.get(), .label = "first"});
  static_cast<void>(arena.attach(Space{.manifold = &secondManifold,
                                       .store    = secondStore.get(),
                                       .label    = "second"}));
  const auto internalAxis = arena.dimensionNamed("d.store-refs");
  ASSERT_TRUE(internalAxis);
  ASSERT_FALSE(arena.dimIn(firstSpace, *internalAxis));
  const auto cellsBefore = arena.cellCount();
  const auto storeCell   = *arena.spaceAt(firstSpace)->storeCell;

  try {
    static_cast<void>(arena.proxyFor(firstSpace, requested));
    FAIL() << "an unbound literal store-refs source axis must refuse proxying";
  } catch (const std::logic_error &error) {
    EXPECT_THAT(
        error.what(),
        ::testing::HasSubstr(
            "proxy store-ref append conflicts with a source-axis neighbor"));
  }

  EXPECT_EQ(arena.cellCount(), cellsBefore);
  EXPECT_FALSE(arena.findExistingProxy(firstSpace, requested));
  EXPECT_FALSE(arena.linked(storeCell, *internalAxis, DimVector::POS));
  EXPECT_EQ(firstManifold.linked(requested, axis.dim, DimVector::NEG),
            neighbor);
  EXPECT_EQ(firstManifold.linked(neighbor, axis.dim, DimVector::POS),
            requested);
  std::size_t proxyCount = 0;
  arena.forEachProxy(firstSpace,
                     [&](const CellRef, const CellRef) { proxyCount++; });
  EXPECT_EQ(proxyCount, 0U);
}

TEST(ArenaFederationTest, CapacityRefusalOnOwnedTargetDoesNotShadowBaseSource) {
  auto store   = createTestStore(tempStoreDir("base_capacity_refusal"));
  auto version = store->sliceGenesis(MicroversionId{});
  version      = store->makeCell(version, "base source");
  const CellRef baseSource = store->cellRefOf(version).value();
  const Manifold base      = store->rebuildManifold(version);
  ArenaManifold arena(&base, store.get());
  std::vector<CellRef> refs;
  const auto [fullTarget, dimStores] = hydrateFullStoreRefsTail(arena, refs);
  (void)dimStores;
  const DimRef newDimension = fullTarget;
  const auto cellsBefore    = arena.cellCount();
  const auto shadowsBefore  = arena.shadowCount();

  EXPECT_THROW(
      (void)arena.link(baseSource, newDimension, DimVector::POS, fullTarget),
      std::length_error);

  EXPECT_EQ(arena.cellCount(), cellsBefore);
  EXPECT_EQ(arena.shadowCount(), shadowsBefore);
  EXPECT_FALSE(arena.linked(baseSource, newDimension, DimVector::POS));
  EXPECT_FALSE(arena.linked(fullTarget, newDimension, DimVector::NEG));
  EXPECT_EQ(arena.slot(fullTarget)->linkCount, zigzag::maxCellLinkRunCount);
}

TEST(ArenaFederationTest,
     FullStoreRefsTailRefusalDoesNotPublishProxyAndNormalTailCanRetry) {
  ArenaManifold saturated;
  std::vector<CellRef> refs;
  const auto [fullTail, dimStores] = hydrateFullStoreRefsTail(saturated, refs);
  const auto fullSpace             = saturated.attach(
      Space{.storeCell = fullTail, .label = "full store refs tail"});
  const auto storeRefs = saturated.dimensionNamed("d.store-refs");
  ASSERT_TRUE(storeRefs);
  const auto fullCount = saturated.cellCount();

  EXPECT_THROW((void)saturated.proxyFor(fullSpace, 42), std::length_error);

  EXPECT_EQ(saturated.cellCount(), fullCount);
  EXPECT_FALSE(saturated.findExistingProxy(fullSpace, 42));
  EXPECT_FALSE(saturated.linked(fullTail, *storeRefs, DimVector::POS));
  std::size_t saturatedProxyCount = 0;
  saturated.forEachProxy(
      fullSpace, [&](const CellRef, const CellRef) { saturatedProxyCount++; });
  EXPECT_EQ(saturatedProxyCount, 0U);
  EXPECT_EQ(saturated.linked(fullTail, dimStores, DimVector::POS), refs[1]);
  EXPECT_THROW((void)saturated.proxyFor(fullSpace, 42), std::length_error);
  EXPECT_EQ(saturated.cellCount(), fullCount);
  EXPECT_FALSE(saturated.findExistingProxy(fullSpace, 42));
  saturatedProxyCount = 0;
  saturated.forEachProxy(
      fullSpace, [&](const CellRef, const CellRef) { saturatedProxyCount++; });
  EXPECT_EQ(saturatedProxyCount, 0U);

  ArenaManifold retry;
  const auto retrySpace = retry.attach(Space{.label = "retry"});
  const auto retryTail  = *retry.spaceAt(retrySpace)->storeCell;
  const auto retryRefs  = retry.dimensionNamed("d.store-refs");
  ASSERT_TRUE(retryRefs);
  const auto retryCount = retry.cellCount();
  retry.setAllocationLimitForTesting(static_cast<std::uint32_t>(retryCount));

  EXPECT_THROW((void)retry.proxyFor(retrySpace, 42), std::runtime_error);

  EXPECT_EQ(retry.cellCount(), retryCount);
  EXPECT_FALSE(retry.findExistingProxy(retrySpace, 42));
  EXPECT_FALSE(retry.linked(retryTail, *retryRefs, DimVector::POS));
  std::size_t retryProxyCount = 0;
  retry.forEachProxy(retrySpace,
                     [&](const CellRef, const CellRef) { retryProxyCount++; });
  EXPECT_EQ(retryProxyCount, 0U);
  retry.setAllocationLimitForTesting(
      static_cast<std::uint32_t>(retryCount + 1));
  const CellRef proxy = retry.proxyFor(retrySpace, 42);

  EXPECT_EQ(retry.proxyFor(retrySpace, 42), proxy);
  EXPECT_EQ(retry.cellCount(), retryCount + 1);
  EXPECT_EQ(retry.findExistingProxy(retrySpace, 42), proxy);
  retryProxyCount = 0;
  retry.forEachProxy(retrySpace,
                     [&](const CellRef actual, const CellRef index) {
                       EXPECT_EQ(actual, proxy);
                       EXPECT_EQ(index, 42U);
                       retryProxyCount++;
                     });
  EXPECT_EQ(retryProxyCount, 1U);
  EXPECT_EQ(retry.linked(retryTail, *retryRefs, DimVector::POS), proxy);
  EXPECT_EQ(retry.linked(proxy, *retryRefs, DimVector::NEG), retryTail);
  EXPECT_FALSE(retry.linked(proxy, *retryRefs, DimVector::POS));
}

TEST(ArenaFederationTest,
     OuterRollbackRestoresProxyIdentityBookkeepingAndDelegatedEdges) {
  auto foreign    = makeForeignLine("proxy_outer_rollback", 2, {{0, 1}});
  const CellRef a = foreign.cells[0];
  const CellRef b = foreign.cells[1];
  ArenaManifold arena;
  const auto space = arena.attach(Space{.manifold = &foreign.manifold,
                                        .store    = foreign.store.get(),
                                        .label    = "foreign"});
  const auto dim   = arena.ensureDimension("d.edge");
  ASSERT_TRUE(arena.bindDimension(dim, space, foreign.dim));
  const auto storeRefs = arena.dimensionNamed("d.store-refs");
  ASSERT_TRUE(storeRefs);
  const CellRef storeCell       = *arena.spaceAt(space)->storeCell;
  const std::size_t beforeCells = arena.cellCount();
  const auto outer              = arena.mark();
  const CellRef proxyA          = arena.proxyFor(space, a);
  zigzag::expectWritten(arena.link(proxyA, dim, DimVector::POS, std::nullopt));
  ASSERT_TRUE(arena.findExistingProxy(space, b));
  ASSERT_TRUE(arena.linked(proxyA, *storeRefs, DimVector::NEG));

  arena.release(outer);

  EXPECT_EQ(arena.cellCount(), beforeCells);
  EXPECT_FALSE(arena.findExistingProxy(space, a));
  EXPECT_FALSE(arena.findExistingProxy(space, b));
  EXPECT_FALSE(arena.linked(storeCell, *storeRefs, DimVector::POS));
  EXPECT_EQ(foreign.manifold.linked(a, foreign.dim, DimVector::POS), b);
  EXPECT_EQ(foreign.manifold.linked(b, foreign.dim, DimVector::NEG), a);
}

TEST(ArenaFederationTest,
     FailedForeignReplacementRollsBackMintsButKeepsFuelSpent) {
  auto foreign = makeForeignLine("proxy_atomic_failure", 4, {{0, 1}, {3, 2}});
  ArenaManifold arena;
  const auto space = arena.attach(Space{.manifold = &foreign.manifold,
                                        .store    = foreign.store.get(),
                                        .label    = "foreign"});
  const auto dim   = arena.ensureDimension("d.edge");
  ASSERT_TRUE(arena.bindDimension(dim, space, foreign.dim));
  const CellRef proxyA   = arena.proxyFor(space, foreign.cells[0]);
  const CellRef proxyC   = arena.proxyFor(space, foreign.cells[2]);
  const auto cellsBefore = arena.cellCount();
  arena.setAllocationLimitForTesting(
      static_cast<std::uint32_t>(cellsBefore + 1));

  EXPECT_THROW((void)arena.link(proxyA, dim, DimVector::POS, proxyC),
               std::runtime_error);

  EXPECT_EQ(arena.cellCount(), cellsBefore);
  EXPECT_FALSE(arena.findExistingProxy(space, foreign.cells[1]));
  EXPECT_FALSE(arena.findExistingProxy(space, foreign.cells[3]));
  EXPECT_FALSE(arena.linked(proxyA, dim, DimVector::POS));
  EXPECT_FALSE(arena.linked(proxyC, dim, DimVector::NEG));
  EXPECT_EQ(
      foreign.manifold.linked(foreign.cells[0], foreign.dim, DimVector::POS),
      foreign.cells[1]);
  EXPECT_EQ(
      foreign.manifold.linked(foreign.cells[3], foreign.dim, DimVector::POS),
      foreign.cells[2]);

  arena.setExecutionLimits(static_cast<std::uint32_t>(cellsBefore + 8), 1,
                           1024);
  EXPECT_THROW((void)arena.link(proxyA, dim, DimVector::POS, proxyC),
               std::runtime_error);
  EXPECT_THROW(arena.consumeWork(), std::runtime_error);
  EXPECT_EQ(arena.cellCount(), cellsBefore);
  EXPECT_FALSE(arena.findExistingProxy(space, foreign.cells[1]));
  EXPECT_FALSE(arena.findExistingProxy(space, foreign.cells[3]));
  EXPECT_THROW((void)arena.link(proxyA, dim, DimVector::POS, proxyC),
               std::runtime_error);
  EXPECT_EQ(arena.cellCount(), cellsBefore);

  arena.setExecutionLimits(static_cast<std::uint32_t>(cellsBefore + 8), 100'000,
                           1024);
  EXPECT_NO_THROW(
      zigzag::expectWritten(arena.link(proxyA, dim, DimVector::POS, proxyC)));
}

// 12. promotingAProxyFilesAPlaceholder
TEST(ArenaFederationTest, PromotingAProxyFilesAPlaceholder) {
  const auto dirF   = tempStoreDir("foreign_store");
  const auto dirL   = tempStoreDir("local_store");
  auto foreignStore = createTestStore(dirF);
  auto localStore   = createTestStore(dirL);

  auto vF0             = foreignStore->sliceGenesis(MicroversionId{});
  auto vF1             = foreignStore->makeCell(vF0, "foreign payload");
  const auto fCell     = foreignStore->cellRefOf(vF1).value();
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
  // One author, one permascroll: the foreign document's bytes are at the same
  // offsets for the local one. Across two permascrolls the promotion would
  // have to name a scroll the local store cannot read, and is refused
  // (AQuotationAnotherPermascrollHoldsIsRefused).
  UserPermascroll::Config cfg;
  cfg.storageDir    = tempStoreDir("fork") / "permascroll";
  auto perma        = std::make_shared<UserPermascroll>(cfg);
  auto foreignStore = std::make_shared<Store>(perma);
  auto localStore   = std::make_shared<Store>(perma);

  auto vF0             = foreignStore->sliceGenesis(MicroversionId{});
  auto vF1             = foreignStore->makeCell(vF0, "shared primedia text");
  const auto fCell     = foreignStore->cellRefOf(vF1).value();
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

  // The foreign cell's own bytes, neither copied nor moved to scratch.
  const auto spans    = localFolded.contentOf(authored);
  const auto original = foreignManifold.contentOf(fCell);
  ASSERT_FALSE(spans.empty());
  EXPECT_TRUE(std::ranges::equal(spans, original));
}

// 14. aBoundDimensionResolvesPerSpace and aBoundDimensionRecordsHowItWasBound
TEST(ArenaFederationTest, ABoundDimensionResolvesPerSpaceAndRecordsMode) {
  ArenaManifold arena;
  const auto sp1 = arena.attach(Space{.label = "sp1"});
  const auto sp2 = arena.attach(Space{.label = "sp2"});

  const auto dArena = arena.ensureDimension("d.global");
  arena.bindDimension(dArena, sp1, 101, DimensionBindingMode::Explicit);
  arena.bindDimension(dArena, sp2, 202, DimensionBindingMode::NameMatch);

  ASSERT_TRUE(arena.dimIn(sp1, dArena));
  ASSERT_TRUE(arena.dimIn(sp2, dArena));
  EXPECT_EQ(*arena.dimIn(sp1, dArena), 101u);
  EXPECT_EQ(*arena.dimIn(sp2, dArena), 202u);

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
  zigzag::expectWritten(arena.link(p, dim1, DimVector::POS, std::nullopt));

  ASSERT_TRUE(arena.findExistingProxy(sp, 50));
  EXPECT_EQ(*arena.findExistingProxy(sp, 50), p);
  EXPECT_TRUE(arena.isQuoteOccurrence(occ));

  arena.release(mark);

  EXPECT_FALSE(arena.findExistingProxy(sp, 50));
  EXPECT_FALSE(arena.isQuoteOccurrence(occ));
}

// 16. aMintAtTheRefCeilingIsRefused
TEST(ArenaFederationTest, ProxyFactoryRejectsReservedZeroForeignIndex) {
  ArenaManifold arena;
  const auto space = arena.attach(Space{.label = "zero-index"});
  EXPECT_THROW(arena.proxyFor(space, CellRef{0}), std::invalid_argument);
}

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

// A quotation names bytes in a federated store's scrolls; read through that
// store, whatever reader the caller has.
TEST(ArenaFederationTest, AQuotationReadsThroughItsSpace) {
  auto store       = createTestStore(tempStoreDir("quote_read"));
  const auto typed = store->insert(MicroversionId{}, 0, "hello world");
  const auto spans = store->rebuild(typed).spansFor(6, 5);

  ArenaManifold arena;
  const auto sp    = arena.attach(Space{.store = store.get(), .label = "q"});
  const auto quote = arena.makeQuote(sp, spans);

  EXPECT_EQ(arena.textOf(quote), "world");
  const auto quoted = arena.quotedContent(quote);
  ASSERT_TRUE(quoted.has_value());
  EXPECT_EQ(quoted->store, store.get());
  EXPECT_EQ(quoted->spans, spans);
  EXPECT_FALSE(arena.quotedContent(arena.makeCell("constructed")).has_value());
}

TEST(ArenaFederationTest, AQuotationVanishesWithItsMark) {
  auto first       = createTestStore(tempStoreDir("quote_mark_first"));
  auto quoted      = createTestStore(tempStoreDir("quote_mark_quoted"));
  const auto typed = quoted->insert(MicroversionId{}, 0, "hello");
  const auto spans = quoted->rebuild(typed).spansFor(0, 5);

  ArenaManifold arena;
  // The first space attached is the one an ordinary cell's spans are read in.
  std::ignore     = arena.attach(Space{.store = first.get(), .label = "a"});
  const auto sp   = arena.attach(Space{.store = quoted.get(), .label = "q"});
  const auto mark = arena.mark();
  const auto gone = arena.makeQuote(sp, spans);
  arena.release(mark);

  // The ref is free again, and what takes it is no quotation of that store.
  const auto reused = arena.makeCell(spans.front());
  EXPECT_EQ(reused, gone);
  const auto content = arena.quotedContent(reused);
  ASSERT_TRUE(content.has_value());
  EXPECT_EQ(content->store, first.get());
}

// Promoted, a quotation is a transclusion: the same bytes, no copy.
TEST(ArenaFederationTest, PromotingAQuotationTranscludesItsBytes) {
  const auto dir = tempStoreDir("quote_promote");
  UserPermascroll::Config cfg;
  cfg.storageDir   = dir / "permascroll";
  auto perma       = std::make_shared<UserPermascroll>(cfg);
  auto source      = std::make_shared<Store>(perma);
  auto target      = std::make_shared<Store>(perma);
  const auto typed = source->insert(MicroversionId{}, 0, "hello world");
  const auto spans = source->rebuild(typed).spansFor(0, 5);

  ArenaManifold arena;
  const auto sp    = arena.attach(Space{.store = source.get(), .label = "q"});
  const auto quote = arena.makeQuote(sp, spans);

  const auto promoted =
      promote(*target, target->sliceGenesis(MicroversionId{}), arena, quote);
  ASSERT_TRUE(promoted.has_value());
  const auto folded  = target->rebuildManifold(promoted->version);
  const auto cell    = promoted->cells.front();
  const auto content = folded.contentOf(cell);
  EXPECT_EQ(std::vector<PrimediaSpan>(content.begin(), content.end()), spans);
  EXPECT_EQ(folded.textOf(cell, *target), "hello");
}

// Bytes in a permascroll the target cannot read cannot be transcluded, and
// copying them is not the answer: the promotion is refused, nothing written.
TEST(ArenaFederationTest, AQuotationAnotherPermascrollHoldsIsRefused) {
  auto source      = createTestStore(tempStoreDir("quote_foreign_src"));
  auto target      = createTestStore(tempStoreDir("quote_foreign_dst"));
  const auto typed = source->insert(MicroversionId{}, 0, "hello");
  const auto spans = source->rebuild(typed).spansFor(0, 5);

  ArenaManifold arena;
  const auto sp    = arena.attach(Space{.store = source.get(), .label = "q"});
  const auto quote = arena.makeQuote(sp, spans);

  const auto genesis = target->sliceGenesis(MicroversionId{});
  const auto before  = target->opCount();
  EXPECT_FALSE(promote(*target, genesis, arena, quote).has_value());
  EXPECT_EQ(target->opCount(), before);
}

} // namespace
