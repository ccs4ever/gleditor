/**
 * @file arena_manifold_test.cpp
 * @brief The ephemeral half of R8: cells with no operations behind them.
 *
 * These are the tests for design/store-slice-convergence.md's step 21, and
 * they are the driver that step was waiting for -- it used to be gated on "a
 * VQL interpreter to drive it", which was withdrawn once three design notes
 * had landed requirements here.
 *
 * The claims being checked are mostly claims about *cost*, which is why an
 * arena exists at all: taking a choice point writes nothing, undoing one is a
 * truncation, and the trail stays empty through a call that only touches cells
 * it minted itself. Each of those is a line in
 * design/vlog-logic-extension.md §5.2-§5.5, and each is cheap to assert and
 * would be easy to lose silently.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <xudu/core/compact_op.hpp>
#include <xudu/core/microversion.hpp>
#include <xudu/core/ops.hpp>
#include <xudu/core/store.hpp>
#include <zigzag/core/arena_manifold.hpp>
#include <zigzag/core/manifold.hpp>

namespace {

using xudu::MicroversionId;
using xudu::PrimediaSpan;
using xudu::Store;
using xudu::ValueKind;
using zigzag::ArenaManifold;
using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;
using zigzag::DirectedDim;
using zigzag::isEphemeral;
using zigzag::Manifold;
using zigzag::noCell;

/// An arena with two dimensions minted, since every link needs one and a
/// dimension here is just a cell (R12).
struct Arena {
  ArenaManifold m;
  DimRef clone{noCell};
  DimRef step{noCell};

  Arena() {
    clone = m.makeCell("d.clone");
    step  = m.makeCell("d.step");
  }
};

// -- identity ---------------------------------------------------------------

TEST(ArenaManifoldTest, everyRefCarriesTheEphemeralBit) {
  Arena arena;
  const auto cell = arena.m.makeCell();

  // Not a convention: it is what makes every R8 refusal written before this
  // class existed catch a leak from it.
  EXPECT_TRUE(isEphemeral(cell));
  EXPECT_TRUE(arena.m.contains(cell));
  EXPECT_EQ(arena.m.denseOf(cell), 2U);

  // A real operation index is not a cell of this arena, and vice versa.
  EXPECT_FALSE(arena.m.contains(1U));
  EXPECT_FALSE(arena.m.contains(noCell));
}

TEST(ArenaManifoldTest, thePersistentSideRefusesAnArenaRefWithoutBeingTold) {
  Store store;
  const auto at = store.sliceGenesis(MicroversionId{});

  Arena arena;
  const auto ephemeral = arena.m.makeCell();

  // Nothing in Store knows this class exists. The refusal is R8's bit doing
  // the work, which is the whole argument for numbering arena cells this way.
  EXPECT_THROW(store.setLink(at, store.homeCell(), store.dimsDimension(), false,
                             ephemeral),
               std::invalid_argument);
}

// -- links ------------------------------------------------------------------

TEST(ArenaManifoldTest, aLinkIsOneEdgeWithTwoEnds) {
  Arena arena;
  const auto a = arena.m.makeCell("a");
  const auto b = arena.m.makeCell("b");

  ASSERT_TRUE(arena.m.link(a, arena.step, DimVector::POS, b));

  // One call, both ends -- which is what makes Vlog's "unification is one
  // link" literally true rather than three links and a repair.
  EXPECT_EQ(arena.m.linked(a, arena.step, DimVector::POS), b);
  EXPECT_EQ(arena.m.linked(b, arena.step, DimVector::NEG), a);
}

TEST(ArenaManifoldTest, aLinkEvictsWhateverEitherEndHeld) {
  Arena arena;
  const auto a = arena.m.makeCell("a");
  const auto b = arena.m.makeCell("b");
  const auto c = arena.m.makeCell("c");

  ASSERT_TRUE(arena.m.link(a, arena.step, DimVector::POS, b));
  ASSERT_TRUE(arena.m.link(a, arena.step, DimVector::POS, c));

  EXPECT_EQ(arena.m.linked(a, arena.step, DimVector::POS), c);
  EXPECT_EQ(arena.m.linked(c, arena.step, DimVector::NEG), a);
  // b kept nothing: the edge it shared with a is the edge that moved.
  EXPECT_EQ(arena.m.linked(b, arena.step, DimVector::NEG), noCell);
}

TEST(ArenaManifoldTest, aLinkNeedsCellsThisArenaHolds) {
  Arena arena;
  const auto a = arena.m.makeCell("a");
  EXPECT_FALSE(
      arena.m.link(a, arena.step, DimVector::POS, ArenaManifold::refOf(99U)));
  EXPECT_FALSE(arena.m.link(a, 7U, DimVector::POS, a));
  EXPECT_EQ(arena.m.linked(a, arena.step, DimVector::POS), noCell);
}

// -- deref, which is cloneMaster --------------------------------------------

TEST(ArenaManifoldTest, cloneMasterIsDerefAndWalksNegward) {
  Arena arena;
  const auto master = arena.m.makeCell("foo");
  const auto mid    = arena.m.makeCell();
  const auto tail   = arena.m.makeCell();

  ASSERT_TRUE(arena.m.link(master, arena.clone, DimVector::POS, mid));
  ASSERT_TRUE(arena.m.link(mid, arena.clone, DimVector::POS, tail));

  EXPECT_EQ(arena.m.cloneMaster(tail, arena.clone), master);
  EXPECT_EQ(arena.m.cloneMaster(mid, arena.clone), master);
  EXPECT_EQ(arena.m.cloneMaster(master, arena.clone), master);
}

TEST(ArenaManifoldTest, aRankThatLoopsAnswersTheCellItStartedFrom) {
  Arena arena;
  const auto one = arena.m.makeCell("f");
  const auto two = arena.m.makeCell();
  ASSERT_TRUE(arena.m.link(one, arena.clone, DimVector::POS, two));
  ASSERT_TRUE(arena.m.link(two, arena.clone, DimVector::POS, one));

  // X = f(X) builds a rational term rather than being refused, so the guard is
  // exercised in ordinary operation and not only by a pathological program.
  EXPECT_EQ(arena.m.cloneMaster(one, arena.clone), one);
  EXPECT_EQ(arena.m.cloneMaster(two, arena.clone), two);
}

TEST(ArenaManifoldTest, bindingAVariableToAVariableThenToATermPropagates) {
  // Vlog §4.2: the var-var case is what decides whether a binding mechanism is
  // correct, and it is the one a copying or pointer-based representation gets
  // wrong. X = Y with both unbound, then Y = foo, must leave X reading foo.
  Arena arena;
  const auto x = arena.m.makeCell(); // bare: unbound
  const auto y = arena.m.makeCell();

  // X = Y: splice X's rank onto the posward tail of Y's. One link.
  ASSERT_TRUE(arena.m.link(y, arena.clone, DimVector::POS, x));
  EXPECT_EQ(arena.m.cloneMaster(x, arena.clone), y);
  EXPECT_TRUE(arena.m.contentOf(y).empty()); // still unbound

  // Y = foo: the master gains content, and X reads it because X reads through
  // the master. No step of this overwrote a pointer.
  const auto foo = arena.m.makeCell("foo");
  ASSERT_TRUE(arena.m.link(foo, arena.clone, DimVector::POS, y));

  EXPECT_EQ(arena.m.cloneMaster(x, arena.clone), foo);
  EXPECT_EQ(arena.m.textOf(arena.m.cloneMaster(x, arena.clone)), "foo");
}

// -- choice points ----------------------------------------------------------

TEST(ArenaManifoldTest, releaseTruncatesCellsMintedUnderTheMark) {
  Arena arena;
  const auto before = arena.m.cellCount();
  const auto mark   = arena.m.mark();

  const auto a = arena.m.makeCell("a");
  const auto b = arena.m.makeCell("b");
  ASSERT_TRUE(arena.m.link(a, arena.step, DimVector::POS, b));
  EXPECT_EQ(arena.m.cellCount(), before + 2);

  arena.m.release(mark);

  // Gone, not garbage: there is nothing to collect afterwards.
  EXPECT_EQ(arena.m.cellCount(), before);
  EXPECT_FALSE(arena.m.contains(a));
  EXPECT_EQ(arena.m.outstandingMarks(), 0U);
}

TEST(ArenaManifoldTest, theTrailStaysEmptyWhileOnlyYoungCellsAreWritten) {
  Arena arena;
  const auto mark = arena.m.mark();

  const auto a = arena.m.makeCell();
  const auto b = arena.m.makeCell();
  ASSERT_TRUE(arena.m.link(a, arena.step, DimVector::POS, b));
  ASSERT_TRUE(arena.m.link(b, arena.clone, DimVector::POS, a));

  // §5.3's claim, measured: binding a variable minted for this activation
  // writes no trail entry, because the truncation that removes the cell is
  // already the undo. This is the WAM's conditional-trailing rule.
  EXPECT_EQ(arena.m.trailSize(), 0U);
  arena.m.release(mark);
}

TEST(ArenaManifoldTest, writingAnOlderCellTrailsItOnceAndReleaseRestoresIt) {
  Arena arena;
  const auto old   = arena.m.makeCell("old");
  const auto first = arena.m.makeCell("first");
  ASSERT_TRUE(arena.m.link(old, arena.step, DimVector::POS, first));

  const auto mark = arena.m.mark();
  const auto next = arena.m.makeCell("next");

  // Head unification binds a caller's variable on every call, so this is the
  // common case rather than an edge one.
  ASSERT_TRUE(arena.m.link(old, arena.step, DimVector::POS, next));
  EXPECT_EQ(arena.m.linked(old, arena.step, DimVector::POS), next);

  // *Two* entries, not one, and the second is the interesting one: setting a
  // link evicts whatever the far end held, and `first` is an older cell too.
  // So the trail cost of a binding is one entry per old cell the edit touches,
  // and an eviction touches one. A displaced occupant is easy to forget when
  // counting what an undo has to restore.
  EXPECT_EQ(arena.m.trailSize(), 2U);

  // Written twice under one mark, and still two entries: both cells' runs were
  // copied above the mark by the first write, so the second needs no undo of
  // its own.
  ASSERT_TRUE(arena.m.link(old, arena.clone, DimVector::POS, next));
  EXPECT_EQ(arena.m.trailSize(), 2U);

  arena.m.release(mark);

  // The link *inside a run below the mark* is what a header-only trail entry
  // would have got wrong: truncating the arena cannot unwrite a DimLink that
  // was overwritten in place.
  EXPECT_EQ(arena.m.linked(old, arena.step, DimVector::POS), first);
  EXPECT_EQ(arena.m.linked(first, arena.step, DimVector::NEG), old);
  EXPECT_EQ(arena.m.linked(old, arena.clone, DimVector::POS), noCell);
  EXPECT_EQ(arena.m.trailSize(), 0U);
  EXPECT_EQ(arena.m.trailSize(), 0U);
}

TEST(ArenaManifoldTest, releaseRestoresContentRestatedUnderTheMark) {
  Arena arena;
  const auto cell = arena.m.makeCell("before");
  const auto mark = arena.m.mark();

  const auto after = arena.m.intern("after");
  ASSERT_TRUE(arena.m.setContent(cell, std::span{&after, 1}));
  EXPECT_EQ(arena.m.textOf(cell), "after");

  arena.m.release(mark);
  EXPECT_EQ(arena.m.textOf(cell), "before");
}

TEST(ArenaManifoldTest, marksNestAndReleaseInReverse) {
  Arena arena;
  const auto base = arena.m.makeCell("base");

  const auto outer = arena.m.mark();
  const auto one   = arena.m.makeCell("one");
  ASSERT_TRUE(arena.m.link(base, arena.step, DimVector::POS, one));

  const auto inner = arena.m.mark();
  const auto two   = arena.m.makeCell("two");
  ASSERT_TRUE(arena.m.link(base, arena.step, DimVector::POS, two));
  EXPECT_EQ(arena.m.outstandingMarks(), 2U);

  arena.m.release(inner);
  EXPECT_EQ(arena.m.linked(base, arena.step, DimVector::POS), one);
  EXPECT_TRUE(arena.m.contains(one));

  arena.m.release(outer);
  EXPECT_EQ(arena.m.linked(base, arena.step, DimVector::POS), noCell);
  EXPECT_FALSE(arena.m.contains(one));
}

TEST(ArenaManifoldTest, discardKeepsTheBindingsMadeUnderTheChoicePoint) {
  Arena arena;
  const auto base = arena.m.makeCell("base");

  const auto mark  = arena.m.mark();
  const auto bound = arena.m.makeCell("bound");
  ASSERT_TRUE(arena.m.link(base, arena.step, DimVector::POS, bound));

  // Cut, and success: §5.4's distinction. What is thrown away is the ability
  // to retry, not the work.
  arena.m.discard(mark);
  EXPECT_EQ(arena.m.outstandingMarks(), 0U);
  EXPECT_EQ(arena.m.linked(base, arena.step, DimVector::POS), bound);
  EXPECT_TRUE(arena.m.contains(bound));
}

TEST(ArenaManifoldTest, compactionIsRefusedWhileAChoicePointIsOutstanding) {
  Arena arena;
  const auto mark = arena.m.mark();

  // Load-bearing rather than cautious: a Mark is a set of arena offsets, and
  // compaction moves every run those offsets name.
  EXPECT_FALSE(arena.m.compact());

  arena.m.release(mark);
  EXPECT_TRUE(arena.m.compact());
}

TEST(ArenaManifoldTest, releaseReclaimsTheDeadRunsAFailedBranchLeft) {
  Arena arena;
  const auto old = arena.m.makeCell("old");
  ASSERT_TRUE(arena.m.link(old, arena.step, DimVector::POS, old));
  ASSERT_TRUE(arena.m.compact());
  const auto tightBefore = arena.m.deadLinks();

  const auto mark = arena.m.mark();
  for (int i = 0; i < 8; i++) {
    const auto fresh = arena.m.makeCell();
    ASSERT_TRUE(arena.m.link(old, arena.clone, DimVector::POS, fresh));
  }
  EXPECT_GT(arena.m.deadLinks(), tightBefore);

  // Which is why refusing to compact costs nothing: the truncation is the
  // reclamation.
  arena.m.release(mark);
  EXPECT_EQ(arena.m.deadLinks(), tightBefore);
}

// -- scalars and scratch, §5.5 ----------------------------------------------

TEST(ArenaManifoldTest, anArenaScalarCarriesBitsAndAllocatesNoBytes) {
  Arena arena;
  const auto cell = arena.m.makeScalarCell(42.0);

  EXPECT_EQ(arena.m.valueKindOf(cell), ValueKind::Double);
  EXPECT_EQ(arena.m.asDouble(cell), 42.0);
  // R6's other half is deliberately absent: unification and arithmetic read
  // the bits, so an evaluation that only computes allocates nothing.
  EXPECT_TRUE(arena.m.contentOf(cell).empty());
  EXPECT_EQ(arena.m.textOf(cell), "");

  EXPECT_EQ(arena.m.asBool(cell), std::nullopt);
  EXPECT_EQ(arena.m.asInt64(cell), std::nullopt);
}

TEST(ArenaManifoldTest, everyNaNComparesAsOneValue) {
  Arena arena;
  const auto quiet =
      arena.m.makeScalarCell(std::numeric_limits<double>::quiet_NaN());
  const auto negated =
      arena.m.makeScalarCell(-std::numeric_limits<double>::quiet_NaN());

  // §4.1 compares scalar bits, so canonicalisation is what makes numeric
  // unification a 64-bit comparison rather than a special case per pattern.
  EXPECT_EQ(arena.m.slot(quiet)->valueBits, arena.m.slot(negated)->valueBits);
}

TEST(ArenaManifoldTest, constructedTextLandsInTheScratchScroll) {
  Arena arena;
  const auto span = arena.m.intern("concatenated");

  EXPECT_EQ(span.scroll, xudu::scratchScroll);
  EXPECT_EQ(arena.m.scratchTextOf(span), "concatenated");

  const auto cell = arena.m.makeCell("atom");
  ASSERT_EQ(arena.m.contentOf(cell).size(), 1U);
  EXPECT_EQ(arena.m.contentOf(cell).front().scroll, xudu::scratchScroll);
  EXPECT_EQ(arena.m.textOf(cell), "atom");
}

TEST(ArenaManifoldTest, theApiRefusesToRecordAScratchSpan) {
  Store store;
  const auto at = store.sliceGenesis(MicroversionId{});
  const PrimediaSpan scratch{
      .scroll = xudu::scratchScroll, .start = 0, .length = 4};

  // An address a failed branch throws away was never permanent, so it may not
  // reach an operation by any route.
  EXPECT_THROW(store.makeCell(at, scratch), std::invalid_argument);
  EXPECT_THROW(
      store.setValue(at, store.homeCell(), scratch, ValueKind::None, 0),
      std::invalid_argument);
  EXPECT_THROW(store.spliceCellSpan(at, store.homeCell(), 0, 0, scratch),
               std::invalid_argument);
}

TEST(ArenaManifoldTest, theFoldRefusesAScratchSpanByNumber) {
  Store store;
  const auto at     = store.sliceGenesis(MicroversionId{});
  auto manifold     = store.rebuildManifold(at);
  const auto before = manifold.cellCount();

  xudu::CompactOpNode node;
  node.kind  = xudu::OpKind::Structure;
  node.flags = xudu::structureFlags(xudu::StructureVerb::MakeCell);
  node.setSpan(
      PrimediaSpan{.scroll = xudu::scratchScroll, .start = 0, .length = 4});

  // The fold cannot throw, so a refusal is a count -- the same mechanism an
  // ephemeral link target gets.
  manifold.applyStructure(400U, node);
  EXPECT_EQ(manifold.refusedOps(), 1U);
  EXPECT_EQ(manifold.cellCount(), before);
}

// -- promotion --------------------------------------------------------------

TEST(ArenaManifoldTest, promoteWritesTheReachableAnswerAndNothingElse) {
  Store store;
  auto at = store.sliceGenesis(MicroversionId{});

  Arena arena;
  const auto answer = arena.m.makeCell("answer");
  const auto tail   = arena.m.makeCell("tail");
  ASSERT_TRUE(arena.m.link(answer, arena.step, DimVector::POS, tail));

  // A failed branch's cells: minted, linked to each other, and not reachable
  // from the answer. Reachability is what makes "promote the answer, not the
  // search" a graph walk rather than bookkeeping.
  const auto deadOne = arena.m.makeCell("dead");
  const auto deadTwo = arena.m.makeCell("also dead");
  ASSERT_TRUE(arena.m.link(deadOne, arena.step, DimVector::POS, deadTwo));

  const auto promoted = zigzag::promote(store, at, arena.m, answer);
  ASSERT_TRUE(promoted.has_value());
  at = promoted->version;

  // answer, tail, and d.step -- the dimension is a cell and comes along
  // because the link names it. The dead pair does not.
  EXPECT_EQ(promoted->cells.size(), 3U);

  const auto manifold   = store.rebuildManifold(at);
  const auto realAnswer = promoted->cells.front();
  EXPECT_EQ(manifold.textOf(realAnswer, store), "answer");
  EXPECT_EQ(manifold.refusedOps(), 0U);

  for (const auto cell : promoted->cells) {
    EXPECT_FALSE(isEphemeral(cell));
    EXPECT_TRUE(manifold.contains(cell));
    // The bytes have a real address now, which is the whole point of the road
    // going through here.
    for (const auto &span : manifold.contentOf(cell)) {
      EXPECT_NE(span.scroll, xudu::scratchScroll);
    }
  }
}

TEST(ArenaManifoldTest, promotionCarriesTheLinksWithBothEndsIntact) {
  Store store;
  auto at = store.sliceGenesis(MicroversionId{});

  Arena arena;
  const auto head = arena.m.makeCell("head");
  const auto next = arena.m.makeCell("next");
  ASSERT_TRUE(arena.m.link(head, arena.step, DimVector::POS, next));

  const auto promoted = zigzag::promote(store, at, arena.m, head);
  ASSERT_TRUE(promoted.has_value());
  const auto manifold = store.rebuildManifold(promoted->version);

  // Which real cell is which is promote()'s discovery order: root first.
  const auto realHead = promoted->cells[0];
  std::vector<CellRef> others(promoted->cells.begin() + 1,
                              promoted->cells.end());

  CellRef found = noCell;
  for (const auto dim : others) {
    if (const auto to = manifold.linked(realHead, dim, DimVector::POS);
        noCell != to) {
      found = to;
    }
  }
  ASSERT_NE(found, noCell);
  EXPECT_EQ(manifold.textOf(found, store), "next");
}

TEST(ArenaManifoldTest, aPromotedScalarRegainsTheRenderingTheArenaDeferred) {
  Store store;
  auto at = store.sliceGenesis(MicroversionId{});

  Arena arena;
  const auto number = arena.m.makeScalarCell(std::int64_t{42});
  ASSERT_TRUE(arena.m.contentOf(number).empty());

  const auto promoted = zigzag::promote(store, at, arena.m, number);
  ASSERT_TRUE(promoted.has_value());

  const auto manifold = store.rebuildManifold(promoted->version);
  const auto cell     = promoted->cells.front();

  // §5.5's deferral, paid off: both halves of R6 exist on the persistent side,
  // and neither was carried through the search.
  EXPECT_EQ(manifold.asInt64(cell), 42);
  EXPECT_EQ(manifold.textOf(cell, store), "42");
}

TEST(ArenaManifoldTest, promotionRefusesAboveItsBudgetAndWritesNothing) {
  Store store;
  const auto at = store.sliceGenesis(MicroversionId{});

  Arena arena;
  const auto root = arena.m.makeCell("root");
  auto previous   = root;
  for (int i = 0; i < 12; i++) {
    const auto fresh = arena.m.makeCell("link in a chain");
    ASSERT_TRUE(arena.m.link(previous, arena.step, DimVector::POS, fresh));
    previous = fresh;
  }

  const auto refused =
      zigzag::promote(store, at, arena.m, root, zigzag::PromotionBudget{4});
  EXPECT_FALSE(refused.has_value());

  // Nothing written: the walk finishes and the budget is checked before the
  // first operation, so a refusal leaves the store where it was.
  const auto manifold = store.rebuildManifold(at);
  EXPECT_EQ(manifold.cellCount(), 2U); // home and d.dims, and no more

  EXPECT_FALSE(zigzag::promote(store, at, arena.m, noCell).has_value());
}

// -- the overlay ------------------------------------------------------------

/// A document with two cells on a dimension, to read through.
struct Document {
  Store store;
  MicroversionId at;
  DimRef dim{noCell};
  CellRef first{noCell};
  CellRef second{noCell};

  Document() {
    at                = store.sliceGenesis(MicroversionId{});
    const auto minted = store.makeDimension(at, "d.step");
    at                = minted.version;
    dim               = minted.dim;
    at                = store.makeCell(at, "first");
    first             = store.cellRefOf(at);
    at                = store.makeCell(at, "second");
    second            = store.cellRefOf(at);
    at                = store.setLink(at, first, dim, DimVector::POS, second);
  }

  [[nodiscard]] Manifold manifold() const { return store.rebuildManifold(at); }
};

TEST(ArenaManifoldTest, anOverlayReadsThroughToTheDocument) {
  Document doc;
  const auto base = doc.manifold();
  ArenaManifold arena{&base};

  // Nothing copied, and every question answered: an evaluation pays for the
  // cells it writes to, not for the ones it walks past.
  EXPECT_EQ(arena.cellCount(), 0U);
  EXPECT_TRUE(arena.contains(doc.first));
  EXPECT_FALSE(arena.holdsOwn(doc.first));
  EXPECT_EQ(arena.linked(doc.first, doc.dim, DimVector::POS), doc.second);
  EXPECT_EQ(arena.linked(doc.second, doc.dim, DimVector::NEG), doc.first);
  EXPECT_EQ(arena.textOf(doc.first, &doc.store), "first");
}

TEST(ArenaManifoldTest, writingAnOverlaidCellShadowsItAndLeavesTheBaseAlone) {
  Document doc;
  const auto base = doc.manifold();
  ArenaManifold arena{&base};

  const auto fresh = arena.makeCell("fresh");
  ASSERT_TRUE(arena.link(doc.first, doc.dim, DimVector::POS, fresh));

  // The arena's answer changed; the document's did not. That is the whole
  // point -- resolution against a clause database must not edit it.
  EXPECT_EQ(arena.linked(doc.first, doc.dim, DimVector::POS), fresh);
  EXPECT_EQ(base.linked(doc.first, doc.dim, DimVector::POS), doc.second);

  // A shadow keeps the base cell's ref as its name: it is the same cell.
  EXPECT_TRUE(arena.holdsOwn(doc.first));
  EXPECT_FALSE(zigzag::isEphemeral(doc.first));
  EXPECT_EQ(arena.textOf(doc.first, &doc.store), "first");

  // And the displaced occupant was shadowed too, since its end of the edge
  // changed -- a base cell nobody named directly.
  EXPECT_TRUE(arena.holdsOwn(doc.second));
  EXPECT_EQ(arena.linked(doc.second, doc.dim, DimVector::NEG), noCell);
  EXPECT_EQ(base.linked(doc.second, doc.dim, DimVector::NEG), doc.first);
}

TEST(ArenaManifoldTest, releasingDropsTheShadowsAFailedBranchTook) {
  Document doc;
  const auto base = doc.manifold();
  ArenaManifold arena{&base};

  const auto mark  = arena.mark();
  const auto fresh = arena.makeCell("fresh");
  ASSERT_TRUE(arena.link(doc.first, doc.dim, DimVector::POS, fresh));
  EXPECT_TRUE(arena.holdsOwn(doc.first));

  arena.release(mark);

  // Dropping the shadow *is* the undo for an overlaid cell: the unmodified
  // state was never moved out of the base, so nothing had to be saved to
  // restore it.
  EXPECT_FALSE(arena.holdsOwn(doc.first));
  EXPECT_EQ(arena.cellCount(), 0U);
  EXPECT_EQ(arena.linked(doc.first, doc.dim, DimVector::POS), doc.second);
}

TEST(ArenaManifoldTest, aShadowedCellIsStillTrailedWhenItIsOlderThanTheMark) {
  Document doc;
  const auto base = doc.manifold();
  ArenaManifold arena{&base};

  // Shadowed before the mark, so the truncation cannot undo the next write and
  // the conditional trail has to.
  const auto early = arena.makeCell("early");
  ASSERT_TRUE(arena.link(doc.first, doc.dim, DimVector::POS, early));

  const auto mark = arena.mark();
  const auto late = arena.makeCell("late");
  ASSERT_TRUE(arena.link(doc.first, doc.dim, DimVector::POS, late));
  EXPECT_GT(arena.trailSize(), 0U);

  arena.release(mark);
  EXPECT_EQ(arena.linked(doc.first, doc.dim, DimVector::POS), early);
}

TEST(ArenaManifoldTest, promotingAnOverlayMintsOnlyWhatTheEvaluationInvented) {
  Document doc;
  const auto base = doc.manifold();
  ArenaManifold arena{&base};

  const auto answer = arena.makeCell("answer");
  ASSERT_TRUE(arena.link(doc.first, doc.dim, DimVector::POS, answer));

  const auto before   = base.cellCount();
  const auto promoted = zigzag::promote(doc.store, doc.at, arena, doc.first);
  ASSERT_TRUE(promoted.has_value());

  const auto after = doc.store.rebuildManifold(promoted->version);
  // One new cell -- the answer. d.step, first and second were already named.
  EXPECT_EQ(after.cellCount(), before + 1);
  EXPECT_EQ(after.linked(doc.first, doc.dim, DimVector::POS),
            promoted->cells.front());
  EXPECT_EQ(after.textOf(promoted->cells.front(), doc.store), "answer");
  EXPECT_EQ(after.refusedOps(), 0U);
}

} // namespace
