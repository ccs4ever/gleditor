/**
 * @file manifold_test.cpp
 * @brief The second replay product: cells folded out of Structure operations.
 *
 * These are the tests for design/store-slice-convergence.md's step 14. Nothing
 * in the tree consumes a Manifold yet, so this file is the only thing asserting
 * that the fold means what R7, R9 and R12 say it means -- and in particular
 * that the incremental path and a cold fold agree, which is the one promise
 * R9 makes in exchange for the view being materialised at all.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <xudu/core/microversion.hpp>
#include <xudu/core/ops.hpp>
#include <xudu/core/store.hpp>
#include <xudu/core/user_permascroll.hpp>
#include <zigzag/core/manifold.hpp>

namespace {

using xudu::MicroversionId;
using xudu::Op;
using xudu::OpKind;
using xudu::PrimediaSpan;
using xudu::Store;
using xudu::StructureVerb;
using xudu::ValueKind;
using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::ephemeralBit;
using zigzag::Manifold;
using zigzag::noCell;

/// A store with a slice already begun, and the state that begins it.
struct Slice {
  Store store;
  MicroversionId at;

  Slice() { at = store.sliceGenesis(MicroversionId{}); }

  /// Mint a cell and hand back both halves of what a caller needs: the state,
  /// which is what the next operation is applied to, and the cell, which is
  /// what the operations after that name.
  CellRef cell(const std::string_view text) {
    at = store.makeCell(at, text);
    return store.cellRefOf(at);
  }

  DimRef dimension(const std::string_view name) {
    const auto minted = store.makeDimension(at, name);
    at                = minted.version;
    return minted.dim;
  }

  void link(const CellRef from, const DimRef dim, const bool negward,
            const CellRef to) {
    at = store.setLink(at, from, dim, negward, to);
  }
};

TEST(ManifoldTest, genesisMintsHomeAndTheDimsDimension) {
  Slice slice;

  // Index 1 and index 2, which is what the design describes for a store that
  // was a slice from its first operation.
  EXPECT_EQ(slice.store.homeCell(), 1U);
  EXPECT_EQ(slice.store.dimsDimension(), 2U);

  const auto manifold = slice.store.rebuildManifold(slice.at);
  EXPECT_EQ(manifold.cellCount(), 2U);
  EXPECT_EQ(manifold.home(), slice.store.homeCell());
  EXPECT_EQ(manifold.dimsDimension(), slice.store.dimsDimension());
  EXPECT_EQ(manifold.refusedOps(), 0U);

  // A dimension is a cell whose content is its name -- ordinary spooled
  // primedia, read back through the store like any other span.
  EXPECT_EQ(manifold.textOf(manifold.home(), slice.store), "home");
  EXPECT_EQ(manifold.textOf(manifold.dimsDimension(), slice.store), "d.dims");

  // d.dims is on its own rank, so it is a dimension the slice reports rather
  // than the one dimension that is invisible.
  EXPECT_THAT(manifold.dimensions(),
              testing::ElementsAre(manifold.dimsDimension()));
}

TEST(ManifoldTest, genesisHappensOnce) {
  Slice slice;
  EXPECT_THROW(slice.store.sliceGenesis(slice.at), std::invalid_argument);
}

TEST(ManifoldTest, aDimensionCannotBeMintedBeforeThereIsADimsRank) {
  Store store;
  EXPECT_THROW(store.makeDimension(MicroversionId{}, "d.1"),
               std::invalid_argument);
}

TEST(ManifoldTest, structureOperationsChangeNoText) {
  Slice slice;
  const auto text = slice.store.insert(slice.at, 0, "the document");
  slice.at        = text;
  const auto one  = slice.cell("a");
  const auto dim  = slice.dimension("d.1");
  const auto two  = slice.cell("b");
  slice.link(one, dim, false, two);

  // Six more operations, and the concatext is exactly what it was: a slice's
  // structure is the *other* replay product of this spool.
  EXPECT_EQ(slice.store.textOf(slice.at), "the document");
  EXPECT_EQ(slice.store.textOf(text), "the document");
}

TEST(ManifoldTest, aLinkIsOneEdgeSharedByTwoCells) {
  Slice slice;
  const auto dim = slice.dimension("d.1");
  const auto one = slice.cell("one");
  const auto two = slice.cell("two");
  slice.link(one, dim, false, two);

  const auto manifold = slice.store.rebuildManifold(slice.at);
  EXPECT_EQ(manifold.linked(one, dim, false), two);
  // The backlink is not a second operation. It is what the one operation
  // means, maintained by the fold the way zzcore's loader derives it.
  EXPECT_EQ(manifold.linked(two, dim, true), one);
  EXPECT_EQ(manifold.linked(one, dim, true), noCell);
  EXPECT_EQ(manifold.linked(two, dim, false), noCell);
}

TEST(ManifoldTest, relinkingBreaksWhatEitherEndWasHolding) {
  Slice slice;
  const auto dim   = slice.dimension("d.1");
  const auto one   = slice.cell("one");
  const auto two   = slice.cell("two");
  const auto three = slice.cell("three");
  slice.link(one, dim, false, two);
  slice.link(one, dim, false, three);

  const auto manifold = slice.store.rebuildManifold(slice.at);
  EXPECT_EQ(manifold.linked(one, dim, false), three);
  EXPECT_EQ(manifold.linked(three, dim, true), one);
  // two kept a dangling backlink under any implementation that treats the two
  // directions as independent facts.
  EXPECT_EQ(manifold.linked(two, dim, true), noCell);
}

TEST(ManifoldTest, displacingACellFromARankLeavesNoHalfLink) {
  Slice slice;
  const auto dim   = slice.dimension("d.1");
  const auto one   = slice.cell("one");
  const auto two   = slice.cell("two");
  const auto three = slice.cell("three");
  slice.link(one, dim, false, three);
  // three's negward side is taken; giving it to two has to cost one its link.
  slice.link(two, dim, false, three);

  const auto manifold = slice.store.rebuildManifold(slice.at);
  EXPECT_EQ(manifold.linked(three, dim, true), two);
  EXPECT_EQ(manifold.linked(two, dim, false), three);
  EXPECT_EQ(manifold.linked(one, dim, false), noCell);
}

TEST(ManifoldTest, linkingToNoCellClearsBothEnds) {
  Slice slice;
  const auto dim = slice.dimension("d.1");
  const auto one = slice.cell("one");
  const auto two = slice.cell("two");
  slice.link(one, dim, false, two);
  slice.link(one, dim, false, noCell);

  const auto manifold = slice.store.rebuildManifold(slice.at);
  EXPECT_EQ(manifold.linked(one, dim, false), noCell);
  EXPECT_EQ(manifold.linked(two, dim, true), noCell);
}

TEST(ManifoldTest, aRankWalksBothWays) {
  Slice slice;
  const auto dim = slice.dimension("d.doc");
  std::vector<CellRef> rank;
  for (int i = 0; i < 12; i++) {
    rank.push_back(slice.cell("cell " + std::to_string(i)));
    if (rank.size() > 1) {
      slice.link(rank[rank.size() - 2], dim, false, rank.back());
    }
  }

  const auto manifold = slice.store.rebuildManifold(slice.at);
  auto cursor         = rank.front();
  for (std::size_t i = 1; i < rank.size(); i++) {
    cursor = manifold.linked(cursor, dim, false);
    EXPECT_EQ(cursor, rank[i]) << "posward, step " << i;
  }
  for (std::size_t i = rank.size() - 1; i > 0; i--) {
    cursor = manifold.linked(cursor, dim, true);
    EXPECT_EQ(cursor, rank[i - 1]) << "negward, step " << i;
  }
  EXPECT_EQ(manifold.textOf(rank[5], slice.store), "cell 5");
}

TEST(ManifoldTest, aCellsRunIsExactlyTheDimensionsItLinksOn) {
  Slice slice;
  const auto first  = slice.dimension("d.1");
  const auto second = slice.dimension("d.2");
  const auto third  = slice.dimension("d.3");
  const auto middle = slice.cell("middle");
  const auto other  = slice.cell("other");
  slice.link(middle, first, false, other);
  slice.link(middle, third, true, other);

  const auto manifold = slice.store.rebuildManifold(slice.at);
  std::vector<DimRef> on;
  for (const auto &link : manifold.dimensionsOf(middle)) {
    on.push_back(link.dim);
  }
  // Exact and uncapped: d.meta-dims read straight off the run, with the
  // dimension it does not participate in absent rather than a zeroed slot.
  EXPECT_THAT(on, testing::UnorderedElementsAre(first, third));
  EXPECT_THAT(second, testing::Not(testing::AnyOfArray(on)));
}

TEST(ManifoldTest, dimensionsComeBackInTheOrderTheyWereMinted) {
  Slice slice;
  const auto first  = slice.dimension("d.1");
  const auto second = slice.dimension("d.2");
  const auto third  = slice.dimension("d.clone");

  const auto manifold = slice.store.rebuildManifold(slice.at);
  EXPECT_THAT(
      manifold.dimensions(),
      testing::ElementsAre(manifold.dimsDimension(), first, second, third));
  EXPECT_EQ(manifold.dimensionNamed("d.2", slice.store), second);
  EXPECT_EQ(manifold.dimensionNamed("d.clone", slice.store), third);
  EXPECT_EQ(manifold.dimensionNamed("d.nothing", slice.store), noCell);
}

TEST(ManifoldTest, cloneMasterFollowsTheCloneDimensionNegward) {
  Slice slice;
  const auto clone      = slice.dimension("d.clone");
  const auto master     = slice.cell("quoted");
  const auto copy       = slice.cell("quoted");
  const auto copyOfCopy = slice.cell("quoted");
  slice.link(master, clone, false, copy);
  slice.link(copy, clone, false, copyOfCopy);

  const auto manifold = slice.store.rebuildManifold(slice.at);
  EXPECT_EQ(manifold.cloneMaster(copyOfCopy, clone), master);
  EXPECT_EQ(manifold.cloneMaster(copy, clone), master);
  EXPECT_EQ(manifold.cloneMaster(master, clone), master);
  // Two cells holding identical content stay two cells, which is the whole
  // reason a cell is not a Version piece: d.clone exists so identity survives
  // appearing in a second place.
  EXPECT_NE(master, copy);
  EXPECT_EQ(manifold.textOf(master, slice.store),
            manifold.textOf(copy, slice.store));
}

TEST(ManifoldTest, aCellsMicroHistoryIsAChainOfOperations) {
  Slice slice;
  const auto dim   = slice.dimension("d.1");
  const auto cell  = slice.cell("edited");
  const auto other = slice.cell("elsewhere");
  slice.link(cell, dim, false, other);
  const auto firstLink = slice.store.cellRefOf(slice.at);
  slice.link(cell, dim, false, noCell);
  const auto secondLink = slice.store.cellRefOf(slice.at);

  const auto manifold    = slice.store.rebuildManifold(slice.at);
  const auto *const slot = manifold.slot(cell);
  ASSERT_NE(slot, nullptr);
  EXPECT_EQ(slot->birthOp, cell);
  EXPECT_EQ(slot->lastOp, secondLink);

  // Walked with no index at all: each operation names the previous one on the
  // same cell, and the chain ends at the MakeCell whose index is the cell.
  std::vector<std::uint32_t> chain;
  for (auto step = slot->lastOp; step != 0;
       step      = slice.store.getCompactOp(step)->sourceOpIndex) {
    chain.push_back(step);
  }
  EXPECT_THAT(chain, testing::ElementsAre(secondLink, firstLink, cell));

  // And every operation in that chain resolves to the cell it belongs to,
  // which is how a SetLink names its subject without a field of its own.
  EXPECT_EQ(manifold.slot(firstLink), slot);
  EXPECT_EQ(manifold.slot(secondLink), slot);
}

TEST(ManifoldTest, setValueRestatesContentAndBits) {
  Slice slice;
  const auto cell = slice.cell("42");
  const auto span = slice.store.userPermascrollPtr()->append("43");
  slice.at = slice.store.setValue(slice.at, cell, span, ValueKind::Double,
                                  0x4045800000000000ULL);

  const auto manifold = slice.store.rebuildManifold(slice.at);
  EXPECT_EQ(manifold.textOf(cell, slice.store), "43");
  ASSERT_TRUE(manifold.asDouble(cell).has_value());
  EXPECT_DOUBLE_EQ(*manifold.asDouble(cell), 43.0);
  // A cell that carries no bits answers nothing rather than zero.
  EXPECT_FALSE(manifold.asDouble(manifold.home()).has_value());
}

TEST(ManifoldTest, incrementalFoldingEqualsAColdFold) {
  Slice slice;
  auto manifold = slice.store.rebuildManifold(slice.at);

  // Built the way an editor would: one operation recorded, one operation
  // folded, and the manifold carried forward rather than rebuilt. The two
  // dimensions are minted longhand rather than through makeDimension(), which
  // is two operations and would leave the first of them unfolded -- and that
  // is what the sugar costs a caller driving a manifold incrementally.
  std::vector<DimRef> dims;
  for (const auto *const name : {"d.doc", "d.meta"}) {
    slice.at = slice.store.makeCell(slice.at, name);
    dims.push_back(slice.store.cellRefOf(slice.at));
    ASSERT_TRUE(manifold.advance(slice.store, slice.at));
    slice.at = slice.store.setLink(slice.at, slice.store.homeCell(),
                                   slice.store.dimsDimension(), false,
                                   dims.back(), &manifold);
    ASSERT_TRUE(manifold.advance(slice.store, slice.at));
  }
  const auto dim  = dims.front();
  const auto meta = dims.back();

  std::vector<CellRef> rank;
  for (int i = 0; i < 40; i++) {
    rank.push_back(slice.cell("cell " + std::to_string(i)));
    ASSERT_TRUE(manifold.advance(slice.store, slice.at));
    if (rank.size() > 1) {
      // Interleaved across two dimensions so that runs outgrow their place in
      // the arena and have to be relocated, which is the part of the CSR
      // arrangement a cold fold would never exercise.
      slice.at = slice.store.setLink(slice.at, rank[rank.size() - 2], dim,
                                     false, rank.back(), &manifold);
      ASSERT_TRUE(manifold.advance(slice.store, slice.at));
      slice.at = slice.store.setLink(slice.at, rank.back(), meta, true,
                                     rank.front(), &manifold);
      ASSERT_TRUE(manifold.advance(slice.store, slice.at));
    }
  }

  EXPECT_EQ(manifold.cellCount(), rank.size() + 4);
  EXPECT_EQ(manifold.refusedOps(), 0U);
  EXPECT_TRUE(manifold.verifyAgainstFullRebuild(slice.store));
  EXPECT_TRUE(manifold.equivalentTo(slice.store.rebuildManifold(slice.at)));

  // Compaction reclaims the runs those relocations left dead, and means the
  // same manifold afterwards.
  manifold.compact();
  EXPECT_TRUE(manifold.verifyAgainstFullRebuild(slice.store));
}

TEST(ManifoldTest, verifyAgainstFullRebuildNoticesAMissedOperation) {
  Slice slice;
  const auto dim = slice.dimension("d.1");
  const auto one = slice.cell("one");
  auto manifold  = slice.store.rebuildManifold(slice.at);

  const auto two = slice.cell("two");
  slice.link(one, dim, false, two);
  // The drift R9 exists to catch: the caller folded the last operation and
  // never folded the one before it, so the view is a state the spool does not
  // hold. It is the hook that says so, not a crash.
  ASSERT_TRUE(manifold.advance(slice.store, slice.at));
  EXPECT_FALSE(manifold.verifyAgainstFullRebuild(slice.store));
}

TEST(ManifoldTest, anEphemeralReferenceIsRefusedAtTheApiAndInTheFold) {
  Slice slice;
  const auto dim  = slice.dimension("d.1");
  const auto cell = slice.cell("real");

  // R8's boundary at the API.
  EXPECT_THROW(
      slice.store.setLink(slice.at, cell, dim, false, ephemeralBit | 3U),
      std::invalid_argument);
  EXPECT_THROW(
      slice.store.setLink(slice.at, cell, ephemeralBit | dim, false, noCell),
      std::invalid_argument);

  // And in the fold, which is what makes it an invariant of the encoding
  // rather than a rule the caller is trusted to follow. Recorded the long way
  // round, because the API above will not write one.
  Op op;
  op.kind   = OpKind::Structure;
  op.flags  = xudu::structureFlags(StructureVerb::SetLink);
  op.to     = ephemeralBit | 3U;
  op.link   = dim;
  op.source = slice.store.segmentedOps().idOf(cell);
  slice.at  = slice.store.apply(slice.at, op);

  const auto manifold = slice.store.rebuildManifold(slice.at);
  EXPECT_EQ(manifold.refusedOps(), 1U);
  EXPECT_THAT(manifold.dimensionsOf(cell), testing::IsEmpty());
}

TEST(ManifoldTest, aReferenceThatIsNotACellIsRefusedAtTheApi) {
  Slice slice;
  const auto dim  = slice.dimension("d.1");
  const auto cell = slice.cell("real");
  // A text operation's index is not a cell, however plausible a number it is.
  slice.at          = slice.store.insert(slice.at, 0, "typed");
  const auto textOp = slice.store.cellRefOf(slice.at);

  EXPECT_THROW(slice.store.setLink(slice.at, textOp, dim, false, cell),
               std::invalid_argument);
  EXPECT_THROW(slice.store.setLink(slice.at, cell, textOp, false, cell),
               std::invalid_argument);
  EXPECT_THROW(slice.store.setLink(slice.at, cell, dim, false, textOp),
               std::invalid_argument);
  EXPECT_THROW(slice.store.setLink(slice.at, cell, dim, false, 9999),
               std::invalid_argument);
  EXPECT_THROW(slice.store.setValue(slice.at, textOp, PrimediaSpan{},
                                    ValueKind::None, 0),
               std::invalid_argument);

  // And nothing was recorded by any of them: an operation the fold would drop
  // is worse than an exception, because the document would look edited.
  EXPECT_EQ(slice.store.cellRefOf(slice.at), textOp);
  EXPECT_EQ(slice.store.rebuildManifold(slice.at).refusedOps(), 0U);
}

TEST(ManifoldTest, aLinkNamingACellTheFoldDoesNotHoldIsRefused) {
  Slice slice;
  const auto dim  = slice.dimension("d.1");
  const auto cell = slice.cell("real");

  Op op;
  op.kind   = OpKind::Structure;
  op.flags  = xudu::structureFlags(StructureVerb::SetLink);
  op.to     = 9999; // no operation, let alone a cell
  op.link   = dim;
  op.source = slice.store.segmentedOps().idOf(cell);
  slice.at  = slice.store.apply(slice.at, op);

  const auto manifold = slice.store.rebuildManifold(slice.at);
  EXPECT_EQ(manifold.refusedOps(), 1U);
  EXPECT_EQ(manifold.linked(cell, dim, false), noCell);
}

TEST(ManifoldTest, aSetLinkWithNoChainHasNoSubjectAndIsRefused) {
  Slice slice;
  const auto dim = slice.dimension("d.1");

  Op op;
  op.kind  = OpKind::Structure;
  op.flags = xudu::structureFlags(StructureVerb::SetLink);
  op.link  = dim;
  op.to    = slice.store.homeCell();
  // No source, so nothing says whose link this is.
  slice.at = slice.store.apply(slice.at, op);

  const auto manifold = slice.store.rebuildManifold(slice.at);
  EXPECT_EQ(manifold.refusedOps(), 1U);
}

TEST(ManifoldTest, twoFuturesOfOneStateAreTwoManifolds) {
  Slice slice;
  const auto dim   = slice.dimension("d.1");
  const auto one   = slice.cell("one");
  const auto two   = slice.cell("two");
  const auto three = slice.cell("three");
  const auto fork  = slice.at;

  const auto left  = slice.store.setLink(fork, one, dim, false, two);
  const auto right = slice.store.setLink(fork, one, dim, false, three);
  ASSERT_NE(left, right);

  EXPECT_EQ(slice.store.rebuildManifold(left).linked(one, dim, false), two);
  EXPECT_EQ(slice.store.rebuildManifold(right).linked(one, dim, false), three);
  // Neither branch's link exists at the state they forked from, which is what
  // makes structural editing scrubbable in hypertime.
  EXPECT_EQ(slice.store.rebuildManifold(fork).linked(one, dim, false), noCell);
}

TEST(ManifoldTest, aChainIsFollowedPerBranchRatherThanAcrossBranches) {
  Slice slice;
  const auto dim   = slice.dimension("d.1");
  const auto one   = slice.cell("one");
  const auto two   = slice.cell("two");
  const auto three = slice.cell("three");
  const auto fork  = slice.at;

  const auto left = slice.store.setLink(fork, one, dim, false, two);
  // The right branch's operation has to chain to one's *own* history as the
  // fork saw it, not to the operation the left branch appended afterwards.
  const auto right  = slice.store.setLink(fork, one, dim, true, three);
  const auto onward = slice.store.setLink(right, one, dim, false, three);

  const auto manifold = slice.store.rebuildManifold(onward);
  EXPECT_EQ(manifold.linked(one, dim, false), three);
  EXPECT_EQ(manifold.linked(one, dim, true), three);
  EXPECT_EQ(manifold.refusedOps(), 0U);
  EXPECT_TRUE(manifold.verifyAgainstFullRebuild(slice.store));

  const auto leftManifold = slice.store.rebuildManifold(left);
  EXPECT_EQ(leftManifold.linked(one, dim, false), two);
  EXPECT_EQ(leftManifold.linked(one, dim, true), noCell);
  EXPECT_EQ(leftManifold.refusedOps(), 0U);
}

// -- what R12's price actually costs, measured -------------------------------
//
// §12.5 of the design note records 4.96 ns/hop sequential and 21.02 ns
// scattered for the CSR run against 1.84 and 7.94 for the fixed array it
// replaced -- 2.7x, accepted deliberately. Those numbers came from a
// standalone harness over a model of the design. This measures the *real*
// Manifold, and the array baseline beside it, so the ruling's price is a test
// rather than an assertion. See R12, and the falsifiable threshold it states.

namespace {

/// The design R12 rejected, built here only to be measured against: eight
/// privileged dimensions inline, so a hop is one dependent load instead of two.
struct ArrayCell {
  xanadu::PrimediaSpan span{};
  std::uint32_t birthOp{0};
  std::uint32_t lastOp{0};
  std::uint64_t valueBits{0};
  struct Slot {
    DimRef dim{noCell};
    CellRef pos{noCell};
    CellRef neg{noCell};
  };
  std::array<Slot, 8> dims{};

  [[nodiscard]] CellRef linked(const DimRef dim, const bool negward) const {
    for (const auto &slot : dims) {
      if (slot.dim == dim) {
        return negward ? slot.neg : slot.pos;
      }
    }
    return noCell;
  }
};

/// ns/hop for a dependent chase of @p hops steps starting at @p from.
template <typename Hop>
double nsPerHop(const CellRef from, const std::size_t hops, Hop &&hop) {
  constexpr int repetitions = 20;
  const auto start          = std::chrono::steady_clock::now();
  CellRef sink              = 0;
  for (int rep = 0; rep < repetitions; rep++) {
    CellRef cursor = from;
    for (std::size_t i = 0; i < hops; i++) {
      const CellRef next = hop(cursor);
      // The chase is the point: the next hop needs this hop's answer, so the
      // loop cannot be pipelined into hiding the load.
      cursor = (noCell == next) ? from : next;
    }
    sink ^= cursor;
  }
  const auto elapsed = std::chrono::duration<double, std::nano>(
                           std::chrono::steady_clock::now() - start)
                           .count();
  EXPECT_NE(sink, 0xFFFFFFFFU) << "the chase must not be optimised away";
  return elapsed / static_cast<double>(hops * repetitions);
}

} // namespace

TEST(ManifoldTest, aHopCostsWhatR12SaysItCosts) {
  constexpr int cellCount = 10000;
  constexpr int dimCount  = 5;

  Slice slice;
  std::vector<DimRef> dims;
  dims.reserve(dimCount);
  auto manifold = slice.store.rebuildManifold(slice.at);
  for (int d = 0; d < dimCount; d++) {
    slice.at = slice.store.makeCell(slice.at, "d." + std::to_string(d + 1));
    dims.push_back(slice.store.cellRefOf(slice.at));
    ASSERT_TRUE(manifold.advance(slice.store, slice.at));
  }

  std::vector<CellRef> cells;
  cells.reserve(cellCount);
  for (int i = 0; i < cellCount; i++) {
    slice.at = slice.store.makeCell(slice.at, "c");
    cells.push_back(slice.store.cellRefOf(slice.at));
    ASSERT_TRUE(manifold.advance(slice.store, slice.at));
  }

  // A rank in id order on dims[0], and a rank that is one random cycle over
  // every cell on dims[1]. The two differ only in locality: same number of
  // hops, same dependent chase, different cache behaviour. dims[2..4] are
  // linked too so that each cell's run really holds five entries -- a run of
  // one would measure a shape the application does not have.
  std::vector<CellRef> shuffled = cells;
  // A fixed multiplier rather than a random engine, so the permutation is the
  // same on every machine and run: a benchmark whose input varies is a
  // benchmark whose output cannot be compared.
  for (std::size_t i = shuffled.size(); i > 1; i--) {
    const std::size_t j = (i * 2654435761U) % i;
    std::swap(shuffled[i - 1], shuffled[j]);
  }

  for (int i = 0; i + 1 < cellCount; i++) {
    slice.at = slice.store.setLink(slice.at, cells[i], dims[0], false,
                                   cells[i + 1], &manifold);
    ASSERT_TRUE(manifold.advance(slice.store, slice.at));
    slice.at = slice.store.setLink(slice.at, shuffled[i], dims[1], false,
                                   shuffled[i + 1], &manifold);
    ASSERT_TRUE(manifold.advance(slice.store, slice.at));
    for (int d = 2; d < dimCount; d++) {
      slice.at =
          slice.store.setLink(slice.at, cells[i], dims[d], false,
                              cells[(i + d * 977) % cellCount], &manifold);
      ASSERT_TRUE(manifold.advance(slice.store, slice.at));
    }
  }
  // Close both ranks into cycles so a chase never runs off the end.
  slice.at = slice.store.setLink(slice.at, cells.back(), dims[0], false,
                                 cells.front(), &manifold);
  ASSERT_TRUE(manifold.advance(slice.store, slice.at));
  slice.at = slice.store.setLink(slice.at, shuffled.back(), dims[1], false,
                                 shuffled.front(), &manifold);
  ASSERT_TRUE(manifold.advance(slice.store, slice.at));
  manifold.compact();

  // The array baseline, filled from the manifold so both hold identical links.
  std::unordered_map<CellRef, std::size_t> denseOf;
  std::vector<ArrayCell> array(static_cast<std::size_t>(cellCount));
  for (std::size_t i = 0; i < cells.size(); i++) {
    denseOf[cells[i]] = i;
  }
  for (std::size_t i = 0; i < cells.size(); i++) {
    const auto *const slot = manifold.slot(cells[i]);
    ASSERT_NE(slot, nullptr);
    array[i].span      = manifold.contentOf(cells[i]).front();
    array[i].birthOp   = slot->birthOp;
    array[i].lastOp    = slot->lastOp;
    array[i].valueBits = slot->valueBits;
    std::size_t next   = 0;
    for (const auto &link : manifold.dimensionsOf(cells[i])) {
      ASSERT_LT(next, array[i].dims.size());
      array[i].dims[next++] = {link.dim, link.pos, link.neg};
    }
  }

  const auto arrayHop = [&](const DimRef dim) {
    return [&, dim](const CellRef from) {
      const auto found = denseOf.find(from);
      return found == denseOf.end() ? noCell
                                    : array[found->second].linked(dim, false);
    };
  };

  // Both designs must agree before either is timed: a faster wrong answer is
  // not a data point.
  for (const auto cell : {cells.front(), cells[cellCount / 2], cells.back()}) {
    for (int d = 0; d < dimCount; d++) {
      EXPECT_EQ(manifold.linked(cell, dims[d], false), arrayHop(dims[d])(cell))
          << "cell " << cell << " dim " << d;
    }
  }

  const auto runSeq = nsPerHop(cells.front(), cellCount, [&](const CellRef c) {
    return manifold.linked(c, dims[0], false);
  });
  const auto runRnd =
      nsPerHop(shuffled.front(), cellCount, [&](const CellRef c) {
        return manifold.linked(c, dims[1], false);
      });
  // The array baseline pays a hash lookup the manifold does not, because its
  // cells are indexed densely and a CellRef is an op index -- so its numbers
  // here are an upper bound on the design R12 rejected, not a faithful one.
  const auto arrSeq = nsPerHop(cells.front(), cellCount, arrayHop(dims[0]));
  const auto arrRnd = nsPerHop(shuffled.front(), cellCount, arrayHop(dims[1]));

  std::cout << "R12 hop cost, " << cellCount << " cells x " << dimCount
            << " dims, ns/hop:\n"
            << "  CSR run (as built):  seq " << runSeq << "  scattered "
            << runRnd << '\n'
            << "  inline array + map:  seq " << arrSeq << "  scattered "
            << arrRnd << '\n'
            << "  design note claims:  seq 4.96/1.84  scattered 21.02/7.94\n";

  // Asserted as an order of magnitude, not as the figures above: this runs on
  // whatever CI is. What would falsify R12 is a hop costing microseconds --
  // the ruling's own threshold is a traversal visiting >100k cells per frame,
  // which at anything under a hundred nanoseconds a hop is still inside an
  // 8.33 ms budget.
  EXPECT_LT(runSeq, 500.0) << "a sequential hop should be nanoseconds";
  EXPECT_LT(runRnd, 2000.0) << "a scattered hop should be nanoseconds";
  // And the falsifiable claim R12 actually rests on: 300 hops is a frame's
  // worth of traversal and must be a rounding error against 8.33 ms.
  EXPECT_LT(runRnd * 300.0 / 1000.0, 100.0)
      << "300 scattered hops must be well under a millisecond";
}

TEST(ManifoldTest, aSliceSurvivesSavingAndReopening) {
  const auto dir =
      std::filesystem::temp_directory_path() / "xudu_manifold_reload_test";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);

  // One permascroll, shared: a store holds no primedia, so a reloaded slice
  // reads its cell names out of the scroll the first one typed them into.
  const auto permascroll = std::make_shared<xudu::UserPermascroll>();

  CellRef dim{noCell};
  CellRef head{noCell};
  CellRef tail{noCell};
  {
    Store store(permascroll);
    auto at           = store.sliceGenesis(MicroversionId{});
    const auto minted = store.makeDimension(at, "d.doc");
    at                = minted.version;
    dim               = minted.dim;
    at                = store.makeCell(at, "first");
    head              = store.cellRefOf(at);
    at                = store.makeCell(at, "second");
    tail              = store.cellRefOf(at);
    at                = store.setLink(at, head, dim, false, tail);
    store.setCurrentVersions({at});
    store.save(dir.string());
  }

  Store reopened(permascroll);
  reopened.load(dir.string());
  EXPECT_EQ(reopened.homeCell(), 1U);
  EXPECT_EQ(reopened.dimsDimension(), 2U);

  const auto manifold =
      reopened.rebuildManifold(reopened.primaryCurrentVersion());
  EXPECT_EQ(manifold.linked(head, dim, false), tail);
  EXPECT_EQ(manifold.linked(tail, dim, true), head);
  EXPECT_EQ(manifold.textOf(head, reopened), "first");
  EXPECT_EQ(manifold.textOf(tail, reopened), "second");
  EXPECT_THAT(manifold.dimensions(),
              testing::ElementsAre(manifold.dimsDimension(), dim));
  EXPECT_TRUE(manifold.verifyAgainstFullRebuild(reopened));

  std::filesystem::remove_all(dir);
}

} // namespace

// -- U3: an edit keeps the addresses of the text it did not touch -------------
//
// A cell used to hold one span, so setCellText() re-spooled the whole content
// and moved every byte to a new address. Measured then: editing one byte of a
// 1000-byte cell appended 1000 permascroll bytes and left zero overlap with the
// old span, which silently severed every transclusion that shared it. These are
// the tests for the run of spans that replaced it.

TEST(ManifoldTest, aSpliceKeepsTheAddressesItDidNotTouch) {
  Slice slice;
  const auto cell   = slice.cell("the quick brown fox");
  const auto before = slice.store.rebuildManifold(slice.at).contentOf(cell);
  ASSERT_EQ(before.size(), 1U);
  const auto original = before.front();

  const auto spooledBefore = slice.store.userPermascroll().size();
  // Replace "quick" with "slow": four bytes typed, not nineteen re-spooled.
  slice.at           = slice.store.spliceCell(slice.at, cell, 4, 5, "slow");
  const auto spooled = slice.store.userPermascroll().size() - spooledBefore;

  const auto manifold = slice.store.rebuildManifold(slice.at);
  EXPECT_EQ(manifold.textOf(cell, slice.store), "the slow brown fox");
  EXPECT_EQ(spooled, 4U) << "an edit spooled more than the text it inserted";

  // Three pieces: the head and tail keep their original addresses, and only the
  // middle is new. That is the property the whole ruling is about.
  const auto run = manifold.contentOf(cell);
  ASSERT_EQ(run.size(), 3U);
  EXPECT_EQ(run[0].start, original.start);
  EXPECT_EQ(run[0].length, 4U);
  EXPECT_EQ(run[2].start, original.start + 9);
  EXPECT_EQ(run[2].length, 10U);
  EXPECT_NE(run[1].start, original.start);
}

TEST(ManifoldTest, aQuotationSurvivesAnEditElsewhereInTheCell) {
  Slice slice;
  const auto quoted = slice.cell("the quick brown fox");
  const auto original =
      slice.store.rebuildManifold(slice.at).contentOf(quoted).front();

  // A second cell transcluding the last ten bytes -- "brown fox" -- by address.
  // Sharing the address *is* the transclusion, which is what the gold beams
  // draw and what diffVersions() classifies as Identity Gold.
  const auto quoter = slice.cell("");
  slice.at          = slice.store.spliceCellSpan(
      slice.at, quoter, 0, 0,
      PrimediaSpan{original.scroll, original.start + 9, 10});

  // Now edit the *front* of the quoted cell, well away from the quotation.
  slice.at = slice.store.spliceCell(slice.at, quoted, 0, 3, "one");

  const auto manifold = slice.store.rebuildManifold(slice.at);
  EXPECT_EQ(manifold.textOf(quoted, slice.store), "one quick brown fox");
  EXPECT_EQ(manifold.textOf(quoter, slice.store), " brown fox");

  // The quotation still shares an address with the cell it quotes. Under one
  // span per cell this failed: the edit re-spooled all nineteen bytes and the
  // overlap went to zero.
  const auto run       = manifold.contentOf(quoted);
  std::uint64_t shared = 0;
  const PrimediaSpan quotation{original.scroll, original.start + 9, 10};
  for (const auto &piece : run) {
    shared += piece.intersect(quotation).length;
  }
  EXPECT_EQ(shared, 10U)
      << "editing one end of a cell severed a quotation of the other";
}

TEST(ManifoldTest, adjacentPiecesCoalesceWithoutLosingAnAddress) {
  Slice slice;
  const auto cell = slice.cell("hello world");
  const auto original =
      slice.store.rebuildManifold(slice.at).contentOf(cell).front();

  // Cut a hole, then splice back a span naming exactly the bytes removed. The
  // run should return to one piece: joins() merges only same-scroll contiguous
  // pieces, so the merged piece covers precisely the addresses the two did.
  slice.at = slice.store.spliceCell(slice.at, cell, 5, 6, "");
  ASSERT_EQ(slice.store.rebuildManifold(slice.at).textOf(cell, slice.store),
            "hello");
  slice.at = slice.store.spliceCellSpan(
      slice.at, cell, 5, 0,
      PrimediaSpan{original.scroll, original.start + 5, 6});

  const auto manifold = slice.store.rebuildManifold(slice.at);
  EXPECT_EQ(manifold.textOf(cell, slice.store), "hello world");
  const auto run = manifold.contentOf(cell);
  EXPECT_EQ(run.size(), 1U)
      << "two contiguous pieces of one scroll stayed apart";
  EXPECT_EQ(run.front().start, original.start);
  EXPECT_EQ(run.front().length, original.length);
}

TEST(ManifoldTest, splicingFoldsTheSameIncrementallyAsCold) {
  Slice slice;
  auto manifold   = slice.store.rebuildManifold(slice.at);
  const auto cell = slice.cell("abcdefghij");
  ASSERT_TRUE(manifold.advance(slice.store, slice.at));

  for (int i = 0; i < 12; i++) {
    slice.at = slice.store.spliceCell(slice.at, cell, (i * 3) % 6, 1,
                                      "X" + std::to_string(i), &manifold);
    ASSERT_TRUE(manifold.advance(slice.store, slice.at));
  }

  EXPECT_EQ(manifold.refusedOps(), 0U);
  EXPECT_TRUE(manifold.verifyAgainstFullRebuild(slice.store));
  EXPECT_EQ(manifold.textOf(cell, slice.store),
            slice.store.rebuildManifold(slice.at).textOf(cell, slice.store));
}

TEST(ManifoldTest, aSpliceRefusesToBePublishedRatherThanArriveWrong) {
  Slice slice;
  const auto cell = slice.cell("the quick brown fox");
  slice.at        = slice.store.spliceCell(slice.at, cell, 4, 5, "slow");

  // The wire encoding has no field for a splice's offset or length, so it would
  // arrive as a splice at offset zero removing nothing -- a change of meaning,
  // not a failure to load. Refused by name until CompactBinaryV4. A store that
  // has never been spliced still publishes.
  EXPECT_THROW(static_cast<void>(slice.store.exportBinaryOps()),
               std::runtime_error);

  Slice plain;
  static_cast<void>(plain.cell("unspliced"));
  EXPECT_NO_THROW(static_cast<void>(plain.store.exportBinaryOps()));
}
