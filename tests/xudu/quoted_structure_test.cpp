/**
 * @file quoted_structure_test.cpp
 * @brief Specification unit tests for Section 5.10: Quoted structure.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "common/xanadu/compact_op.hpp"
#include "common/xanadu/extern_ref.hpp"
#include "common/xanadu/ops.hpp"
#include "common/xanadu/publication.hpp"
#include "common/xanadu/quoted_structure.hpp"
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

// Helper to create an authorial store with genesis
struct TestStore {
  std::shared_ptr<UserPermascroll> perma;
  std::unique_ptr<Store> store;
  MicroversionId head;
  std::string scrollKey;

  explicit TestStore(std::string key = "btpk:1111:doc")
      : perma(std::make_shared<UserPermascroll>()),
        store(std::make_unique<Store>(perma)), scrollKey(std::move(key)) {
    head = store->sliceGenesis(MicroversionId{});
  }

  zigzag::DimRef makeDim(std::string_view name) {
    const auto m = store->makeDimension(head, name);
    head         = m.version;
    return m.dim;
  }

  zigzag::DimRef makeDimAt(MicroversionId &at, std::string_view name) {
    const auto m = store->makeDimension(at, name);
    at           = m.version;
    return m.dim;
  }
};

// 1. aQuotedKeymapIsOneQuotation
TEST(QuotedStructureTest, aQuotedKeymapIsOneQuotation) {
  TestStore alice("btpk:aaaa:alice");
  const auto homeAlice = alice.store->homeCell();

  // Alice builds a mini-keymap:
  // home --d.vars--> setting1 ("vim.insert") --d.values--> val1 ("i")
  //                  |--d.notes--> note1 ("Insert mode")
  //                  |--d.groups--> group1 ("Editing")
  auto atAlice          = alice.head;
  const auto dimVarsA   = alice.makeDimAt(atAlice, "d.vars");
  const auto dimValsA   = alice.makeDimAt(atAlice, "d.values");
  const auto dimNotesA  = alice.makeDimAt(atAlice, "d.notes");
  const auto dimGroupsA = alice.makeDimAt(atAlice, "d.groups");

  atAlice       = alice.store->makeCell(atAlice, "vim.insert");
  const auto s1 = alice.store->cellRefOf(atAlice);
  atAlice       = alice.store->makeCell(atAlice, "i");
  const auto v1 = alice.store->cellRefOf(atAlice);
  atAlice       = alice.store->makeCell(atAlice, "Insert mode");
  const auto n1 = alice.store->cellRefOf(atAlice);
  atAlice       = alice.store->makeCell(atAlice, "Editing");
  const auto g1 = alice.store->cellRefOf(atAlice);

  atAlice = alice.store->setLink(atAlice, homeAlice, dimVarsA,
                                 zigzag::DimVector::POS, s1);
  atAlice =
      alice.store->setLink(atAlice, s1, dimValsA, zigzag::DimVector::POS, v1);
  atAlice =
      alice.store->setLink(atAlice, s1, dimNotesA, zigzag::DimVector::POS, n1);
  atAlice =
      alice.store->setLink(atAlice, s1, dimGroupsA, zigzag::DimVector::POS, g1);
  alice.head = atAlice;

  // Bob quotes Alice's keymap using a closure selector carrying the dimensions
  TestStore bob("btpk:bbbb:bob");
  bob.head = bob.store->registerScroll(bob.head, alice.scrollKey);
  const auto aliceScrollId =
      *bob.store->scrollRegistry().scrollIdForKey(alice.scrollKey);

  const GlobalDocumentState pinnedState{
      .scroll  = alice.scrollKey,
      .version = alice.head,
  };

  const ExternOpRef rootRef{
      .scroll   = aliceScrollId,
      .produces = alice.store->segmentedOps().idOf(homeAlice),
  };
  const ExternOpRef varsRef{
      .scroll   = aliceScrollId,
      .produces = alice.store->segmentedOps().idOf(dimVarsA),
  };
  const ExternOpRef valsRef{
      .scroll   = aliceScrollId,
      .produces = alice.store->segmentedOps().idOf(dimValsA),
  };
  const ExternOpRef notesRef{
      .scroll   = aliceScrollId,
      .produces = alice.store->segmentedOps().idOf(dimNotesA),
  };
  const ExternOpRef groupsRef{
      .scroll   = aliceScrollId,
      .produces = alice.store->segmentedOps().idOf(dimGroupsA),
  };

  SelectorSpec spec{
      .kind      = Selector::Kind::Closure,
      .rootRef   = rootRef,
      .carryRefs = {varsRef, valsRef, notesRef, groupsRef},
  };

  const auto dimVarsB = bob.makeDim("d.vars");

  const auto appended =
      bob.store->quote(bob.head, bob.store->homeCell(), dimVarsB,
                       "Alice's Vim Keymap", pinnedState, spec);
  bob.head = appended.version;

  // Assert it is exactly ONE quotation
  EXPECT_NE(appended.quotationCell, zigzag::noCell);
  EXPECT_NE(appended.placeholderCell, zigzag::noCell);
  EXPECT_NE(appended.stateCell, zigzag::noCell);
  EXPECT_NE(appended.selectorCell, zigzag::noCell);

  const auto bobFold = bob.store->rebuildManifold(bob.head);
  const auto readQ = readQuotation(bobFold, *bob.store, appended.quotationCell);
  ASSERT_TRUE(readQ.has_value());
  EXPECT_EQ(readQ->selectorSpec.kind, Selector::Kind::Closure);
  EXPECT_EQ(readQ->selectorSpec.carryRefs.size(), 4U);

  // Materialize into ArenaManifold
  InMemoryForeignSource source;
  source.registerDocument(pinnedState, alice.store.get(), nullptr);

  zigzag::ArenaManifold arena(&bobFold, bob.store.get());
  resolveQuotations(arena, bobFold, *bob.store, source, QuotationBudget{});

  // Verify that the setting, value, note, and group are accessible through the
  // arena
  const auto dimVarsOpt = arena.dimensionNamed("d.vars", *bob.store);
  ASSERT_TRUE(dimVarsOpt.has_value());
  const auto qSucc =
      arena.linked(appended.quotationCell, *dimVarsOpt, zigzag::DimVector::POS);
  EXPECT_NE(qSucc, zigzag::noCell);
}

// 2. aForeignShapeFoldsWithoutDocumentContent
TEST(QuotedStructureTest, aForeignShapeFoldsWithoutDocumentContent) {
  TestStore alice("btpk:aaaa:alice");
  auto at         = alice.head;
  const auto dimX = alice.makeDimAt(at, "d.x");

  at = alice.store->makeCell(at, "Full Permascroll Document Content");
  const auto c1 = alice.store->cellRefOf(at);
  at            = alice.store->setLink(at, alice.store->homeCell(), dimX,
                                       zigzag::DimVector::POS, c1);
  alice.head    = at;

  // Folding foreign store without any SpanReader content reads
  const auto folded = alice.store->rebuildManifold(alice.head);
  EXPECT_TRUE(folded.contains(c1));
  EXPECT_EQ(
      folded.linked(alice.store->homeCell(), dimX, zigzag::DimVector::POS), c1);
}

// 3. aClosureSelectorResolvesWithoutReadingDocumentContent
TEST(QuotedStructureTest,
     aClosureSelectorResolvesWithoutReadingDocumentContent) {
  TestStore alice("btpk:aaaa:alice");
  auto atAlice        = alice.head;
  const auto dimVarsA = alice.makeDimAt(atAlice, "d.vars");

  atAlice       = alice.store->makeCell(atAlice, "Heavy Foreign Content");
  const auto s1 = alice.store->cellRefOf(atAlice);
  atAlice    = alice.store->setLink(atAlice, alice.store->homeCell(), dimVarsA,
                                    zigzag::DimVector::POS, s1);
  alice.head = atAlice;

  TestStore bob("btpk:bbbb:bob");
  bob.head = bob.store->registerScroll(bob.head, alice.scrollKey);
  const auto aliceScrollId =
      *bob.store->scrollRegistry().scrollIdForKey(alice.scrollKey);

  const GlobalDocumentState pinnedState{.scroll  = alice.scrollKey,
                                        .version = alice.head};
  const ExternOpRef rootRef{
      .scroll   = aliceScrollId,
      .produces = alice.store->segmentedOps().idOf(alice.store->homeCell())};
  const ExternOpRef varsRef{.scroll = aliceScrollId,
                            .produces =
                                alice.store->segmentedOps().idOf(dimVarsA)};

  SelectorSpec spec{.kind      = Selector::Kind::Closure,
                    .rootRef   = rootRef,
                    .carryRefs = {varsRef}};
  const auto dimVarsB = bob.makeDim("d.vars");

  const auto appended = bob.store->quote(bob.head, bob.store->homeCell(),
                                         dimVarsB, "Quote", pinnedState, spec);
  bob.head            = appended.version;

  const auto bobFold = bob.store->rebuildManifold(bob.head);
  InMemoryForeignSource source;
  source.registerDocument(pinnedState, alice.store.get(), nullptr);

  zigzag::ArenaManifold arena(&bobFold, bob.store.get());
  // Resolution should complete and materialize without reading any foreign
  // content
  EXPECT_NO_THROW(
      resolveQuotations(arena, bobFold, *bob.store, source, QuotationBudget{}));
}

// 4. aQuotationKeepsTheRestOfTheLocalRank
TEST(QuotedStructureTest, aQuotationKeepsTheRestOfTheLocalRank) {
  TestStore alice("btpk:aaaa:alice");
  auto atAlice    = alice.head;
  const auto dimA = alice.makeDimAt(atAlice, "d.vars");
  atAlice         = alice.store->makeCell(atAlice, "Alice1");
  const auto f1   = alice.store->cellRefOf(atAlice);
  atAlice         = alice.store->setLink(atAlice, alice.store->homeCell(), dimA,
                                         zigzag::DimVector::POS, f1);
  alice.head      = atAlice;

  TestStore bob("btpk:bbbb:bob");
  auto atBob      = bob.head;
  const auto dimB = bob.makeDimAt(atBob, "d.vars");

  atBob         = bob.store->makeCell(atBob, "BobBefore");
  const auto l1 = bob.store->cellRefOf(atBob);
  atBob         = bob.store->makeCell(atBob, "BobAfter");
  const auto l2 = bob.store->cellRefOf(atBob);

  atBob    = bob.store->setLink(atBob, bob.store->homeCell(), dimB,
                                zigzag::DimVector::POS, l1);
  atBob    = bob.store->setLink(atBob, l1, dimB, zigzag::DimVector::POS, l2);
  bob.head = atBob;

  bob.head = bob.store->registerScroll(bob.head, alice.scrollKey);
  const auto scrollId =
      *bob.store->scrollRegistry().scrollIdForKey(alice.scrollKey);

  const GlobalDocumentState pin{.scroll  = alice.scrollKey,
                                .version = alice.head};
  const ExternOpRef rootRef{.scroll   = scrollId,
                            .produces = alice.store->segmentedOps().idOf(f1)};
  SelectorSpec spec{.kind = Selector::Kind::Cell, .rootRef = rootRef};

  // Splice Q after l1 on dimB: original rank was l1 -> l2
  const auto appended = bob.store->quote(bob.head, l1, dimB, "Q", pin, spec);
  bob.head            = appended.version;

  const auto bobFold = bob.store->rebuildManifold(bob.head);
  InMemoryForeignSource source;
  source.registerDocument(pin, alice.store.get(), nullptr);

  zigzag::ArenaManifold arena(&bobFold, bob.store.get());
  resolveQuotations(arena, bobFold, *bob.store, source, QuotationBudget{});

  // Walk rank along dimB from l1
  std::vector<zigzag::CellRef> visited;
  zigzag::walkRank(arena, l1, dimB, zigzag::DimVector::POS,
                   [&](const zigzag::CellRef c) { visited.push_back(c); });

  // Expected: l1 -> Q -> occ(F1) -> l2
  ASSERT_GE(visited.size(), 4U);
  EXPECT_EQ(visited[0], l1);
  EXPECT_EQ(visited[1], appended.quotationCell);
  EXPECT_EQ(visited.back(), l2);
}

// 5. twoQuotationsOfTheSameRankKeepBothOccurrences
TEST(QuotedStructureTest, twoQuotationsOfTheSameRankKeepBothOccurrences) {
  TestStore alice("btpk:aaaa:alice");
  auto atAlice    = alice.head;
  const auto dimA = alice.makeDimAt(atAlice, "d.rank");
  atAlice         = alice.store->makeCell(atAlice, "F1");
  const auto f1   = alice.store->cellRefOf(atAlice);
  alice.head      = atAlice;

  TestStore bob("btpk:bbbb:bob");
  bob.head = bob.store->registerScroll(bob.head, alice.scrollKey);
  const auto scrollId =
      *bob.store->scrollRegistry().scrollIdForKey(alice.scrollKey);

  const auto dimB = bob.makeDim("d.rank");

  const GlobalDocumentState pin{.scroll  = alice.scrollKey,
                                .version = alice.head};
  const ExternOpRef rootRef{.scroll   = scrollId,
                            .produces = alice.store->segmentedOps().idOf(f1)};
  SelectorSpec spec{.kind = Selector::Kind::Cell, .rootRef = rootRef};

  const auto q1 =
      bob.store->quote(bob.head, bob.store->homeCell(), dimB, "Q1", pin, spec);
  bob.head = q1.version;

  bob.head       = bob.store->makeCell(bob.head, "Mid");
  const auto mid = bob.store->cellRefOf(bob.head);

  const auto q2 = bob.store->quote(bob.head, mid, dimB, "Q2", pin, spec);
  bob.head      = q2.version;

  const auto bobFold = bob.store->rebuildManifold(bob.head);
  InMemoryForeignSource source;
  source.registerDocument(pin, alice.store.get(), nullptr);

  zigzag::ArenaManifold arena(&bobFold, bob.store.get());
  resolveQuotations(arena, bobFold, *bob.store, source, QuotationBudget{});

  const auto occ1 =
      arena.linked(q1.quotationCell, dimB, zigzag::DimVector::POS);
  const auto occ2 =
      arena.linked(q2.quotationCell, dimB, zigzag::DimVector::POS);

  EXPECT_NE(occ1, zigzag::noCell);
  EXPECT_NE(occ2, zigzag::noCell);
  EXPECT_NE(occ1, occ2);
  EXPECT_EQ(arena.linked(occ1, dimB, zigzag::DimVector::NEG), q1.quotationCell);
  EXPECT_EQ(arena.linked(occ2, dimB, zigzag::DimVector::NEG), q2.quotationCell);
}

// 6. overlappingQuotationSelectorsDoNotLeakEdges
TEST(QuotedStructureTest, overlappingQuotationSelectorsDoNotLeakEdges) {
  TestStore alice("btpk:aaaa:alice");
  auto atA          = alice.head;
  const auto dimX_A = alice.makeDimAt(atA, "d.x");
  const auto dimY_A = alice.makeDimAt(atA, "d.y");

  atA           = alice.store->makeCell(atA, "A");
  const auto cA = alice.store->cellRefOf(atA);
  atA           = alice.store->makeCell(atA, "B");
  const auto cB = alice.store->cellRefOf(atA);
  atA           = alice.store->makeCell(atA, "C");
  const auto cC = alice.store->cellRefOf(atA);

  atA = alice.store->setLink(atA, cA, dimX_A, zigzag::DimVector::POS, cB);
  atA = alice.store->setLink(atA, cA, dimY_A, zigzag::DimVector::POS, cC);
  alice.head = atA;

  TestStore bob("btpk:bbbb:bob");
  bob.head       = bob.store->registerScroll(bob.head, alice.scrollKey);
  const auto sId = *bob.store->scrollRegistry().scrollIdForKey(alice.scrollKey);

  const auto dimR = bob.makeDim("d.r");

  const GlobalDocumentState pin{.scroll  = alice.scrollKey,
                                .version = alice.head};
  const ExternOpRef refA{.scroll   = sId,
                         .produces = alice.store->segmentedOps().idOf(cA)};
  const ExternOpRef refX{.scroll   = sId,
                         .produces = alice.store->segmentedOps().idOf(dimX_A)};
  const ExternOpRef refY{.scroll   = sId,
                         .produces = alice.store->segmentedOps().idOf(dimY_A)};

  // Q1 carries only d.x
  SelectorSpec spec1{
      .kind = Selector::Kind::Closure, .rootRef = refA, .carryRefs = {refX}};
  const auto q1 =
      bob.store->quote(bob.head, bob.store->homeCell(), dimR, "Q1", pin, spec1);
  bob.head = q1.version;

  // Q2 carries only d.y
  SelectorSpec spec2{
      .kind = Selector::Kind::Closure, .rootRef = refA, .carryRefs = {refY}};
  const auto q2 =
      bob.store->quote(bob.head, bob.store->homeCell(), dimR, "Q2", pin, spec2);
  bob.head = q2.version;

  const auto bobFold = bob.store->rebuildManifold(bob.head);
  InMemoryForeignSource source;
  source.registerDocument(pin, alice.store.get(), nullptr);

  zigzag::ArenaManifold arena(&bobFold, bob.store.get());
  resolveQuotations(arena, bobFold, *bob.store, source, QuotationBudget{});

  const auto dimY_B = arena.ensureDimension("d.y");
  const auto dimX_B = arena.ensureDimension("d.x");

  const auto occ1A =
      arena.linked(q1.quotationCell, dimR, zigzag::DimVector::POS);
  ASSERT_NE(occ1A, zigzag::noCell);
  // View 1 has d.x edge, but NOT d.y edge
  EXPECT_NE(arena.linked(occ1A, dimX_B, zigzag::DimVector::POS),
            zigzag::noCell);
  EXPECT_EQ(arena.linked(occ1A, dimY_B, zigzag::DimVector::POS),
            zigzag::noCell);
}

// 7. aRankSelectorCanWalkEitherWayAroundARing
TEST(QuotedStructureTest, aRankSelectorCanWalkEitherWayAroundARing) {
  TestStore alice("btpk:aaaa:alice");
  auto at            = alice.head;
  const auto dimRing = alice.makeDimAt(at, "d.ring");

  at            = alice.store->makeCell(at, "A");
  const auto cA = alice.store->cellRefOf(at);
  at            = alice.store->makeCell(at, "B");
  const auto cB = alice.store->cellRefOf(at);
  at            = alice.store->makeCell(at, "C");
  const auto cC = alice.store->cellRefOf(at);

  // Ring: A -> B -> C -> A
  at = alice.store->setLink(at, cA, dimRing, zigzag::DimVector::POS, cB);
  at = alice.store->setLink(at, cB, dimRing, zigzag::DimVector::POS, cC);
  at = alice.store->setLink(at, cC, dimRing, zigzag::DimVector::POS, cA);
  alice.head = at;

  const auto aliceFold = alice.store->rebuildManifold(alice.head);

  // POS walk starting at B: B -> C -> A
  Selector selPos{
      .kind          = Selector::Kind::Rank,
      .root          = cB,
      .rankDim       = dimRing,
      .rankDirection = zigzag::DimVector::POS,
  };
  QuotationState statePos = QuotationState::Resolved;
  const auto resPos       = evaluateSelector(selPos, aliceFold, *alice.store,
                                             QuotationBudget{}, statePos);
  ASSERT_EQ(resPos.size(), 3U);
  EXPECT_EQ(resPos[0], cB);
  EXPECT_EQ(resPos[1], cC);
  EXPECT_EQ(resPos[2], cA);

  // NEG walk starting at B: B -> A -> C
  Selector selNeg{
      .kind          = Selector::Kind::Rank,
      .root          = cB,
      .rankDim       = dimRing,
      .rankDirection = zigzag::DimVector::NEG,
  };
  QuotationState stateNeg = QuotationState::Resolved;
  const auto resNeg       = evaluateSelector(selNeg, aliceFold, *alice.store,
                                             QuotationBudget{}, stateNeg);
  ASSERT_EQ(resNeg.size(), 3U);
  EXPECT_EQ(resNeg[0], cB);
  EXPECT_EQ(resNeg[1], cA);
  EXPECT_EQ(resNeg[2], cC);
}

// 8. anExplicitQuotationScopeDoesNotLeakPastTheSelector
TEST(QuotedStructureTest, anExplicitQuotationScopeDoesNotLeakPastTheSelector) {
  TestStore alice("btpk:aaaa:alice");
  auto at         = alice.head;
  const auto dimX = alice.makeDimAt(at, "d.vars");

  std::vector<zigzag::CellRef> cells;
  for (int i = 0; i < 10; ++i) {
    at = alice.store->makeCell(at, "Setting_" + std::to_string(i));
    cells.push_back(alice.store->cellRefOf(at));
  }
  for (std::size_t i = 0; i + 1 < cells.size(); ++i) {
    at = alice.store->setLink(at, cells[i], dimX, zigzag::DimVector::POS,
                              cells[i + 1]);
  }
  alice.head = at;

  TestStore bob("btpk:bbbb:bob");
  bob.head       = bob.store->registerScroll(bob.head, alice.scrollKey);
  const auto sId = *bob.store->scrollRegistry().scrollIdForKey(alice.scrollKey);

  const auto dimB = bob.makeDim("d.vars");

  const GlobalDocumentState pin{.scroll  = alice.scrollKey,
                                .version = alice.head};
  const ExternOpRef rootRef{
      .scroll = sId, .produces = alice.store->segmentedOps().idOf(cells[0])};
  const ExternOpRef dimRef{.scroll   = sId,
                           .produces = alice.store->segmentedOps().idOf(dimX)};

  SelectorSpec spec{
      .kind = Selector::Kind::Rank, .rootRef = rootRef, .rankDimRef = dimRef};
  const auto q =
      bob.store->quote(bob.head, bob.store->homeCell(), dimB, "Q", pin, spec);
  bob.head = q.version;

  const auto bobFold = bob.store->rebuildManifold(bob.head);
  InMemoryForeignSource source;
  source.registerDocument(pin, alice.store.get(), nullptr);

  // Set budget maxCells to 3
  QuotationBudget budget;
  budget.maxCells = 3;

  zigzag::ArenaManifold arena(&bobFold, bob.store.get());
  resolveQuotations(arena, bobFold, *bob.store, source, budget);

  // Traversal along dimB must not leak past the 3 selected cells
  std::vector<zigzag::CellRef> visited;
  zigzag::walkRank(arena, q.quotationCell, dimB, zigzag::DimVector::POS,
                   [&](const zigzag::CellRef c) { visited.push_back(c); });
  // Q + 3 occurrences = 4 cells total
  EXPECT_LE(visited.size(), 4U);
}

// 9. aQuerySelectorAnswersWhatVqlAnswers
TEST(QuotedStructureTest, aQuerySelectorAnswersWhatVqlAnswers) {
  TestStore alice("btpk:aaaa:alice");
  auto at         = alice.head;
  const auto dimX = alice.makeDimAt(at, "d.vars");

  at            = alice.store->makeCell(at, "item1");
  const auto c1 = alice.store->cellRefOf(at);
  at            = alice.store->makeCell(at, "item2");
  const auto c2 = alice.store->cellRefOf(at);
  at            = alice.store->setLink(at, alice.store->homeCell(), dimX,
                                       zigzag::DimVector::POS, c1);
  at         = alice.store->setLink(at, c1, dimX, zigzag::DimVector::POS, c2);
  alice.head = at;

  const auto aliceFold = alice.store->rebuildManifold(alice.head);

  Selector selQuery{
      .kind                 = Selector::Kind::Query,
      .query                = "##/d.vars",
      .queryLanguageVersion = 13,
      .orderPolicy          = "identity",
  };
  QuotationState state = QuotationState::Resolved;
  const auto res       = evaluateSelector(selQuery, aliceFold, *alice.store,
                                          QuotationBudget{}, state);
  EXPECT_EQ(state, QuotationState::Resolved);
  EXPECT_EQ(res.size(), 2U);
  EXPECT_EQ(res[0], c1);
  EXPECT_EQ(res[1], c2);
}

// 10. aQueryFromAnUnknownLanguageVersionIsRefusedByNumber
TEST(QuotedStructureTest, aQueryFromAnUnknownLanguageVersionIsRefusedByNumber) {
  TestStore alice;
  const auto aliceFold = alice.store->rebuildManifold(alice.head);

  Selector selQuery{
      .kind                 = Selector::Kind::Query,
      .query                = "/home",
      .queryLanguageVersion = 999, // Unknown version
  };
  QuotationState state = QuotationState::Resolved;
  const auto res       = evaluateSelector(selQuery, aliceFold, *alice.store,
                                          QuotationBudget{}, state);
  EXPECT_EQ(state, QuotationState::Unintelligible);
  EXPECT_TRUE(res.empty());
}

// 11. anOverrideShadowsByGlobalNameNotByPosition
TEST(QuotedStructureTest, anOverrideShadowsByGlobalNameNotByPosition) {
  TestStore alice("btpk:aaaa:alice");
  auto atA          = alice.head;
  const auto dimX_A = alice.makeDimAt(atA, "d.rank");

  atA           = alice.store->makeCell(atA, "F1");
  const auto f1 = alice.store->cellRefOf(atA);
  atA           = alice.store->makeCell(atA, "F2");
  const auto f2 = alice.store->cellRefOf(atA);
  atA = alice.store->setLink(atA, f1, dimX_A, zigzag::DimVector::POS, f2);
  alice.head = atA;

  TestStore bob("btpk:bbbb:bob");
  bob.head       = bob.store->registerScroll(bob.head, alice.scrollKey);
  const auto sId = *bob.store->scrollRegistry().scrollIdForKey(alice.scrollKey);

  const auto dimX_B = bob.makeDim("d.rank");

  const GlobalDocumentState pin1{.scroll  = alice.scrollKey,
                                 .version = alice.head};
  const ExternOpRef ref1{.scroll   = sId,
                         .produces = alice.store->segmentedOps().idOf(f1)};
  const ExternOpRef ref2{.scroll   = sId,
                         .produces = alice.store->segmentedOps().idOf(f2)};
  const ExternOpRef refDim{
      .scroll = sId, .produces = alice.store->segmentedOps().idOf(dimX_A)};

  SelectorSpec spec{
      .kind = Selector::Kind::Rank, .rootRef = ref1, .rankDimRef = refDim};
  const auto q = bob.store->quote(bob.head, bob.store->homeCell(), dimX_B, "Q",
                                  pin1, spec);
  bob.head     = q.version;

  // Bob overrides F2 by its birth op ref
  bob.head = bob.store->overrideQuotedCell(bob.head, q.quotationCell, ref2,
                                           "Bob's Override of F2");

  // Now Alice prepends F0 before F1
  atA           = alice.store->makeCell(atA, "F0");
  const auto f0 = alice.store->cellRefOf(atA);
  atA = alice.store->setLink(atA, f0, dimX_A, zigzag::DimVector::POS, f1);
  alice.head = atA;

  // Bob updates pin to Alice's new state
  const GlobalDocumentState pin2{.scroll  = alice.scrollKey,
                                 .version = alice.head};
  InMemoryForeignSource source;
  source.registerDocument(pin1, alice.store.get(), nullptr);
  source.registerDocument(pin2, alice.store.get(), nullptr);

  const auto bobFold = bob.store->rebuildManifold(bob.head);
  zigzag::ArenaManifold arena(&bobFold, bob.store.get());
  resolveQuotations(arena, bobFold, *bob.store, source, QuotationBudget{});

  // Verify that the override still replaces F2 and does not affect F1 or F0
  const auto occ =
      arena.linked(q.quotationCell, dimX_B, zigzag::DimVector::POS);
  ASSERT_NE(occ, zigzag::noCell);
  const auto occ2 = arena.linked(occ, dimX_B, zigzag::DimVector::POS);
  ASSERT_NE(occ2, zigzag::noCell);
  EXPECT_EQ(arena.textOf(occ2, *bob.store), "Bob's Override of F2");
}

// 12. anOverrideMayShadowACellOffTheRank
TEST(QuotedStructureTest, anOverrideMayShadowACellOffTheRank) {
  TestStore alice("btpk:aaaa:alice");
  auto atA            = alice.head;
  const auto dimValsA = alice.makeDimAt(atA, "d.values");

  atA              = alice.store->makeCell(atA, "tab_size");
  const auto sName = alice.store->cellRefOf(atA);
  atA              = alice.store->makeCell(atA, "4");
  const auto sVal  = alice.store->cellRefOf(atA);

  atA =
      alice.store->setLink(atA, sName, dimValsA, zigzag::DimVector::POS, sVal);
  alice.head = atA;

  TestStore bob("btpk:bbbb:bob");
  bob.head       = bob.store->registerScroll(bob.head, alice.scrollKey);
  const auto sId = *bob.store->scrollRegistry().scrollIdForKey(alice.scrollKey);

  const auto dimVarsB = bob.makeDim("d.vars");

  const GlobalDocumentState pin{.scroll  = alice.scrollKey,
                                .version = alice.head};
  const ExternOpRef refName{
      .scroll = sId, .produces = alice.store->segmentedOps().idOf(sName)};
  const ExternOpRef refVal{.scroll   = sId,
                           .produces = alice.store->segmentedOps().idOf(sVal)};
  const ExternOpRef refValsDim{
      .scroll = sId, .produces = alice.store->segmentedOps().idOf(dimValsA)};

  SelectorSpec spec{.kind      = Selector::Kind::Closure,
                    .rootRef   = refName,
                    .carryRefs = {refValsDim}};
  const auto q = bob.store->quote(bob.head, bob.store->homeCell(), dimVarsB,
                                  "Q", pin, spec);
  bob.head     = q.version;

  // Bob overrides sVal with "2"
  bob.head =
      bob.store->overrideQuotedCell(bob.head, q.quotationCell, refVal, "2");

  const auto bobFold = bob.store->rebuildManifold(bob.head);
  InMemoryForeignSource source;
  source.registerDocument(pin, alice.store.get(), nullptr);

  zigzag::ArenaManifold arena(&bobFold, bob.store.get());
  resolveQuotations(arena, bobFold, *bob.store, source, QuotationBudget{});

  const auto occSetting =
      arena.linked(q.quotationCell, dimVarsB, zigzag::DimVector::POS);
  ASSERT_NE(occSetting, zigzag::noCell);
  EXPECT_EQ(arena.textOf(occSetting, *alice.store), "tab_size");

  const auto dimValsB = arena.ensureDimension("d.values");
  const auto occVal =
      arena.linked(occSetting, dimValsB, zigzag::DimVector::POS);
  ASSERT_NE(occVal, zigzag::noCell);
  EXPECT_EQ(arena.textOf(occVal, *bob.store), "2");
}

// 13. anUnresolvedQuotationIsAVisibleCitation
TEST(QuotedStructureTest, anUnresolvedQuotationIsAVisibleCitation) {
  TestStore alice("btpk:aaaa:alice");
  TestStore bob("btpk:bbbb:bob");
  bob.head       = bob.store->registerScroll(bob.head, alice.scrollKey);
  const auto sId = *bob.store->scrollRegistry().scrollIdForKey(alice.scrollKey);

  const auto dimVarsB = bob.makeDim("d.vars");

  const GlobalDocumentState pin{.scroll  = alice.scrollKey,
                                .version = alice.head};
  const ExternOpRef refRoot{
      .scroll   = sId,
      .produces = alice.store->segmentedOps().idOf(alice.store->homeCell())};

  SelectorSpec spec{.kind = Selector::Kind::Cell, .rootRef = refRoot};
  const auto q = bob.store->quote(bob.head, bob.store->homeCell(), dimVarsB,
                                  "Unresolved Q", pin, spec);
  bob.head     = q.version;

  const auto bobFold = bob.store->rebuildManifold(bob.head);
  InMemoryForeignSource emptySource; // Alice is not in source -> Absent

  zigzag::ArenaManifold arena(&bobFold, bob.store.get());
  resolveQuotations(arena, bobFold, *bob.store, emptySource, QuotationBudget{});

  // Q remains visible on the rank with label intact
  EXPECT_EQ(bobFold.textOf(q.quotationCell, *bob.store), "Unresolved Q");
  EXPECT_EQ(
      arena.linked(bob.store->homeCell(), dimVarsB, zigzag::DimVector::POS),
      q.quotationCell);
}

// 14. aQuotationPinsItsState
TEST(QuotedStructureTest, aQuotationPinsItsState) {
  TestStore alice("btpk:aaaa:alice");
  auto atA           = alice.head;
  const auto dimX    = alice.makeDimAt(atA, "d.vars");
  atA                = alice.store->makeCell(atA, "v1_cell");
  const auto c1      = alice.store->cellRefOf(atA);
  atA                = alice.store->setLink(atA, alice.store->homeCell(), dimX,
                                            zigzag::DimVector::POS, c1);
  alice.head         = atA;
  const auto aliceV1 = alice.head;

  // Alice advances to v2 adding v2_cell
  atA           = alice.store->makeCell(atA, "v2_cell");
  const auto c2 = alice.store->cellRefOf(atA);
  atA        = alice.store->setLink(atA, c1, dimX, zigzag::DimVector::POS, c2);
  alice.head = atA;

  TestStore bob("btpk:bbbb:bob");
  bob.head       = bob.store->registerScroll(bob.head, alice.scrollKey);
  const auto sId = *bob.store->scrollRegistry().scrollIdForKey(alice.scrollKey);
  const auto dimB = bob.makeDim("d.vars");

  // Bob quotes pinned to aliceV1
  const GlobalDocumentState pinV1{.scroll  = alice.scrollKey,
                                  .version = aliceV1};
  const ExternOpRef ref1{.scroll   = sId,
                         .produces = alice.store->segmentedOps().idOf(c1)};
  const ExternOpRef refDim{.scroll   = sId,
                           .produces = alice.store->segmentedOps().idOf(dimX)};

  SelectorSpec spec{
      .kind = Selector::Kind::Rank, .rootRef = ref1, .rankDimRef = refDim};
  const auto q =
      bob.store->quote(bob.head, bob.store->homeCell(), dimB, "Q", pinV1, spec);
  bob.head = q.version;

  InMemoryForeignSource source;
  source.registerDocument(pinV1, alice.store.get(), nullptr);

  const auto bobFold = bob.store->rebuildManifold(bob.head);
  zigzag::ArenaManifold arena(&bobFold, bob.store.get());
  resolveQuotations(arena, bobFold, *bob.store, source, QuotationBudget{});

  const auto occ1 = arena.linked(q.quotationCell, dimB, zigzag::DimVector::POS);
  ASSERT_NE(occ1, zigzag::noCell);
  // occ1 has no successor along dimB because v2_cell does not exist at aliceV1
  EXPECT_EQ(arena.linked(occ1, dimB, zigzag::DimVector::POS), zigzag::noCell);
}

// 15. aCellBirthDoesNotPinItsLaterSnapshot
TEST(QuotedStructureTest, aCellBirthDoesNotPinItsLaterSnapshot) {
  TestStore alice("btpk:aaaa:alice");
  auto atA           = alice.head;
  atA                = alice.store->makeCell(atA, "state_one");
  const auto c       = alice.store->cellRefOf(atA);
  const auto aliceV1 = atA;

  atA                = alice.store->setCellText(atA, c, "state_two");
  const auto aliceV2 = atA;
  alice.head         = atA;

  const auto foldV1 = alice.store->rebuildManifold(aliceV1);
  const auto foldV2 = alice.store->rebuildManifold(aliceV2);

  EXPECT_EQ(foldV1.textOf(c, *alice.store), "state_one");
  EXPECT_EQ(foldV2.textOf(c, *alice.store), "state_two");
}

// 16. aLateFetchCannotReplaceANewerView
TEST(QuotedStructureTest, aLateFetchCannotReplaceANewerView) {
  CancellationToken token;
  token.cancel();
  EXPECT_TRUE(token.isCancelled());

  ForeignRequest req{.state = {}, .cancellation = token};
  InMemoryForeignSource source;
  bool called = false;
  source.requestDocument(req, [&](FetchState state, ForeignDocument) {
    called = true;
    EXPECT_EQ(state, FetchState::NotFetched);
  });
  EXPECT_TRUE(called);
}

// 17. preselectionBudgetsBoundTheForeignFold
TEST(QuotedStructureTest, preselectionBudgetsBoundTheForeignFold) {
  TestStore alice("btpk:aaaa:alice");
  TestStore bob("btpk:bbbb:bob");
  bob.head       = bob.store->registerScroll(bob.head, alice.scrollKey);
  const auto sId = *bob.store->scrollRegistry().scrollIdForKey(alice.scrollKey);
  const auto dimB = bob.makeDim("d.vars");

  const GlobalDocumentState pin{.scroll  = alice.scrollKey,
                                .version = alice.head};
  const ExternOpRef refRoot{
      .scroll   = sId,
      .produces = alice.store->segmentedOps().idOf(alice.store->homeCell())};

  SelectorSpec spec{.kind = Selector::Kind::Cell, .rootRef = refRoot};
  const auto q =
      bob.store->quote(bob.head, bob.store->homeCell(), dimB, "Q", pin, spec);
  bob.head = q.version;

  InMemoryForeignSource source;
  source.registerDocument(pin, alice.store.get(), nullptr);

  // Set budget maxOpBytes very small (e.g. 1 byte) to reject fold
  QuotationBudget tightBudget;
  tightBudget.maxOpBytes = 1;

  const auto bobFold = bob.store->rebuildManifold(bob.head);
  zigzag::ArenaManifold arena(&bobFold, bob.store.get());
  resolveQuotations(arena, bobFold, *bob.store, source, tightBudget);

  // Q should not have resolved occurrences due to budget
  EXPECT_EQ(arena.linked(q.quotationCell, dimB, zigzag::DimVector::POS),
            zigzag::noCell);
}

// 18. aQuotationOfAQuotationResolves
TEST(QuotedStructureTest, aQuotationOfAQuotationResolves) {
  // Store A -> Store B -> Store C
  TestStore a("btpk:aaaa:a");
  auto atA         = a.head;
  const auto dimA  = a.makeDimAt(atA, "d.vars");
  atA              = a.store->makeCell(atA, "RootItemA");
  const auto itemA = a.store->cellRefOf(atA);
  atA = a.store->setLink(atA, a.store->homeCell(), dimA, zigzag::DimVector::POS,
                         itemA);
  a.head = atA;

  TestStore b("btpk:bbbb:b");
  b.head          = b.store->registerScroll(b.head, a.scrollKey);
  const auto sIdA = *b.store->scrollRegistry().scrollIdForKey(a.scrollKey);
  const auto dimB = b.makeDim("d.vars");

  const GlobalDocumentState pinA{.scroll = a.scrollKey, .version = a.head};
  const ExternOpRef refA{.scroll   = sIdA,
                         .produces = a.store->segmentedOps().idOf(itemA)};
  SelectorSpec specA{.kind = Selector::Kind::Cell, .rootRef = refA};
  const auto qB =
      b.store->quote(b.head, b.store->homeCell(), dimB, "QB", pinA, specA);
  b.head = qB.version;

  TestStore c("btpk:cccc:c");
  c.head          = c.store->registerScroll(c.head, b.scrollKey);
  const auto sIdB = *c.store->scrollRegistry().scrollIdForKey(b.scrollKey);
  const auto dimC = c.makeDim("d.vars");

  const GlobalDocumentState pinB{.scroll = b.scrollKey, .version = b.head};
  const ExternOpRef refB{.scroll = sIdB,
                         .produces =
                             b.store->segmentedOps().idOf(qB.quotationCell)};
  SelectorSpec specB{.kind = Selector::Kind::Cell, .rootRef = refB};
  const auto qC =
      c.store->quote(c.head, c.store->homeCell(), dimC, "QC", pinB, specB);
  c.head = qC.version;

  InMemoryForeignSource source;
  source.registerDocument(pinA, a.store.get(), nullptr);
  source.registerDocument(pinB, b.store.get(), nullptr);

  const auto cFold = c.store->rebuildManifold(c.head);
  zigzag::ArenaManifold arena(&cFold, c.store.get());
  resolveQuotations(arena, cFold, *c.store, source, QuotationBudget{});

  const auto occ = arena.linked(qC.quotationCell, dimC, zigzag::DimVector::POS);
  EXPECT_NE(occ, zigzag::noCell);
}

// 19. aQuotationCycleTerminates
TEST(QuotedStructureTest, aQuotationCycleTerminates) {
  TestStore a("btpk:aaaa:a");
  TestStore b("btpk:bbbb:b");

  a.head = a.store->registerScroll(a.head, b.scrollKey);
  b.head = b.store->registerScroll(b.head, a.scrollKey);

  const auto sIdB_inA = *a.store->scrollRegistry().scrollIdForKey(b.scrollKey);
  const auto sIdA_inB = *b.store->scrollRegistry().scrollIdForKey(a.scrollKey);

  const auto dimA = a.makeDim("d.vars");
  const auto dimB = b.makeDim("d.vars");

  const GlobalDocumentState pinB{.scroll = b.scrollKey, .version = b.head};
  const GlobalDocumentState pinA{.scroll = a.scrollKey, .version = a.head};

  const ExternOpRef refB{.scroll = sIdB_inA,
                         .produces =
                             b.store->segmentedOps().idOf(b.store->homeCell())};
  const ExternOpRef refA{.scroll = sIdA_inB,
                         .produces =
                             a.store->segmentedOps().idOf(a.store->homeCell())};

  SelectorSpec specB{.kind = Selector::Kind::Cell, .rootRef = refB};
  SelectorSpec specA{.kind = Selector::Kind::Cell, .rootRef = refA};

  const auto qA =
      a.store->quote(a.head, a.store->homeCell(), dimA, "QA", pinB, specB);
  a.head = qA.version;
  const auto qB =
      b.store->quote(b.head, b.store->homeCell(), dimB, "QB", pinA, specA);
  b.head = qB.version;

  InMemoryForeignSource source;
  source.registerDocument(pinB, b.store.get(), nullptr);
  source.registerDocument(pinA, a.store.get(), nullptr);

  const auto aFold = a.store->rebuildManifold(a.head);
  zigzag::ArenaManifold arena(&aFold, a.store.get());

  // Must terminate cleanly without infinite loop or stack overflow
  EXPECT_NO_THROW(
      resolveQuotations(arena, aFold, *a.store, source, QuotationBudget{}));
}

// 20. materialisationWritesNothing
TEST(QuotedStructureTest, materialisationWritesNothing) {
  TestStore alice("btpk:aaaa:alice");
  TestStore bob("btpk:bbbb:bob");
  bob.head       = bob.store->registerScroll(bob.head, alice.scrollKey);
  const auto sId = *bob.store->scrollRegistry().scrollIdForKey(alice.scrollKey);
  const auto dimB = bob.makeDim("d.vars");

  const GlobalDocumentState pin{.scroll  = alice.scrollKey,
                                .version = alice.head};
  const ExternOpRef refRoot{
      .scroll   = sId,
      .produces = alice.store->segmentedOps().idOf(alice.store->homeCell())};

  SelectorSpec spec{.kind = Selector::Kind::Cell, .rootRef = refRoot};
  const auto q =
      bob.store->quote(bob.head, bob.store->homeCell(), dimB, "Q", pin, spec);
  bob.head = q.version;

  const auto opsBefore = bob.store->opCount();
  const auto bobFold   = bob.store->rebuildManifold(bob.head);

  InMemoryForeignSource source;
  source.registerDocument(pin, alice.store.get(), nullptr);

  zigzag::ArenaManifold arena(&bobFold, bob.store.get());
  resolveQuotations(arena, bobFold, *bob.store, source, QuotationBudget{});

  // Local store ops and base manifold equality must be strictly preserved (R8)
  EXPECT_EQ(bob.store->opCount(), opsBefore);
  const auto bobFoldAfter = bob.store->rebuildManifold(bob.head);
  EXPECT_TRUE(bobFold.equivalentTo(bobFoldAfter));
}

// 21. aQuotedKeymapReadsThroughSystemStoreModel
TEST(QuotedStructureTest, aQuotedKeymapReadsThroughSystemStoreModel) {
  TestStore alice("btpk:aaaa:alice");
  const auto homeAlice = alice.store->homeCell();

  auto atA            = alice.head;
  const auto dimVarsA = alice.makeDimAt(atA, "d.vars");
  const auto dimValsA = alice.makeDimAt(atA, "d.values");

  atA              = alice.store->makeCell(atA, "editor.tab_size");
  const auto sName = alice.store->cellRefOf(atA);
  atA              = alice.store->makeCell(atA, "4");
  const auto sVal  = alice.store->cellRefOf(atA);

  atA = alice.store->setLink(atA, homeAlice, dimVarsA, zigzag::DimVector::POS,
                             sName);
  atA =
      alice.store->setLink(atA, sName, dimValsA, zigzag::DimVector::POS, sVal);
  alice.head = atA;

  TestStore bob("btpk:bbbb:bob");
  bob.head       = bob.store->registerScroll(bob.head, alice.scrollKey);
  const auto sId = *bob.store->scrollRegistry().scrollIdForKey(alice.scrollKey);

  const auto dimVarsB = bob.makeDim("d.vars");

  const GlobalDocumentState pin{.scroll  = alice.scrollKey,
                                .version = alice.head};
  const ExternOpRef refHome{
      .scroll = sId, .produces = alice.store->segmentedOps().idOf(homeAlice)};
  const ExternOpRef refVal{.scroll   = sId,
                           .produces = alice.store->segmentedOps().idOf(sVal)};
  const ExternOpRef refVarsDim{
      .scroll = sId, .produces = alice.store->segmentedOps().idOf(dimVarsA)};
  const ExternOpRef refValsDim{
      .scroll = sId, .produces = alice.store->segmentedOps().idOf(dimValsA)};

  SelectorSpec spec{.kind      = Selector::Kind::Closure,
                    .rootRef   = refHome,
                    .carryRefs = {refVarsDim, refValsDim}};
  const auto q = bob.store->quote(bob.head, bob.store->homeCell(), dimVarsB,
                                  "AliceConfig", pin, spec);
  bob.head     = q.version;

  // Bob overrides tab_size from 4 to 8
  bob.head =
      bob.store->overrideQuotedCell(bob.head, q.quotationCell, refVal, "8");

  InMemoryForeignSource source;
  source.registerDocument(pin, alice.store.get(), nullptr);

  const auto bobFold = bob.store->rebuildManifold(bob.head);
  zigzag::ArenaManifold arena(&bobFold, bob.store.get());
  resolveQuotations(arena, bobFold, *bob.store, source, QuotationBudget{});

  // Read through SystemStoreModel off the ArenaManifold overlay
  const auto model =
      SystemStoreModel::fromManifold(arena, arena.home(), bob.store.get());
  EXPECT_TRUE(model.isValid());

  const auto entry = model.find("editor.tab_size");
  ASSERT_TRUE(entry.has_value());
  EXPECT_EQ(entry->name, "editor.tab_size");
  ASSERT_FALSE(entry->value.elements.empty());
  EXPECT_EQ(entry->value.asString(0), "8");
}

} // namespace

#if defined(__clang__)
#pragma clang diagnostic pop
#endif
