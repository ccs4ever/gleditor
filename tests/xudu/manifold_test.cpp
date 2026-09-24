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
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <common/xanadu/zigzag/arena_manifold.hpp>
#include <xudu/core/link_layout.hpp>
#include <xudu/core/microversion.hpp>
#include <xudu/core/ops.hpp>
#include <xudu/core/provenance.hpp>
#include <xudu/core/store.hpp>
#include <xudu/core/user_permascroll.hpp>
#include <zigzag/core/manifold.hpp>

namespace {

using xudu::Link;
using xudu::LinkType;
using xudu::MicroversionId;
using xudu::Op;
using xudu::OpKind;
using xudu::PrimediaSpan;
using xudu::ProminenceTier;
using xudu::Store;
using xudu::StructureVerb;
using xudu::ValueKind;
using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;
using zigzag::DirectedDim;
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
    const auto manifold = store.rebuildManifold(at);
    const auto existing = manifold.dimensionNamed(name, store);
    if (existing) {
      return *existing;
    }
    const auto minted = store.makeDimension(at, name);
    at                = minted.version;
    return minted.dim;
  }

  void link(const CellRef from, const DimRef dim, const DimVector dir,
            const CellRef to) {
    at = store.setLink(at, from, dim, dir, to);
  }

  void link(const CellRef from, const DimRef dim, const bool negward,
            const CellRef to) {
    at = store.setLink(at, from, dim, negward, to);
  }

  CellRef opHandle(const std::uint32_t target,
                   const std::string_view text = {}) {
    at = store.makeOpHandle(at, target, text);
    return store.cellRefOf(at);
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
  slice.link(one, dim, DimVector::POS, two);

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
  slice.link(one, dim, DimVector::POS, two);

  const auto manifold = slice.store.rebuildManifold(slice.at);
  EXPECT_EQ(manifold.linked(one, dim, DimVector::POS), two);
  // The backlink is not a second operation. It is what the one operation
  // means, maintained by the fold the way zzcore's loader derives it.
  EXPECT_EQ(manifold.linked(two, dim, DimVector::NEG), one);
  EXPECT_EQ(manifold.linked(one, dim, DimVector::NEG), noCell);
  EXPECT_EQ(manifold.linked(two, dim, DimVector::POS), noCell);
}

TEST(ManifoldTest, relinkingBreaksWhatEitherEndWasHolding) {
  Slice slice;
  const auto dim   = slice.dimension("d.1");
  const auto one   = slice.cell("one");
  const auto two   = slice.cell("two");
  const auto three = slice.cell("three");
  slice.link(one, dim, DimVector::POS, two);
  slice.link(one, dim, DimVector::POS, three);

  const auto manifold = slice.store.rebuildManifold(slice.at);
  EXPECT_EQ(manifold.linked(one, dim, DimVector::POS), three);
  EXPECT_EQ(manifold.linked(three, dim, DimVector::NEG), one);
  // two kept a dangling backlink under any implementation that treats the two
  // directions as independent facts.
  EXPECT_EQ(manifold.linked(two, dim, DimVector::NEG), noCell);
}

TEST(ManifoldTest, displacingACellFromARankLeavesNoHalfLink) {
  Slice slice;
  const auto dim   = slice.dimension("d.1");
  const auto one   = slice.cell("one");
  const auto two   = slice.cell("two");
  const auto three = slice.cell("three");
  slice.link(one, dim, DimVector::POS, three);
  // three's negward side is taken; giving it to two has to cost one its link.
  slice.link(two, dim, DimVector::POS, three);

  const auto manifold = slice.store.rebuildManifold(slice.at);
  EXPECT_EQ(manifold.linked(three, dim, DimVector::NEG), two);
  EXPECT_EQ(manifold.linked(two, dim, DimVector::POS), three);
  EXPECT_EQ(manifold.linked(one, dim, DimVector::POS), noCell);
}

TEST(ManifoldTest, linkingToNoCellClearsBothEnds) {
  Slice slice;
  const auto dim = slice.dimension("d.1");
  const auto one = slice.cell("one");
  const auto two = slice.cell("two");
  slice.link(one, dim, DimVector::POS, two);
  slice.link(one, dim, DimVector::POS, noCell);

  const auto manifold = slice.store.rebuildManifold(slice.at);
  EXPECT_EQ(manifold.linked(one, dim, DimVector::POS), noCell);
  EXPECT_EQ(manifold.linked(two, dim, DimVector::NEG), noCell);
}

TEST(ManifoldTest, aRankWalksBothWays) {
  Slice slice;
  const auto dim = slice.dimension("d.doc");
  std::vector<CellRef> rank;
  for (int i = 0; i < 12; i++) {
    rank.push_back(slice.cell("cell " + std::to_string(i)));
    if (rank.size() > 1) {
      slice.link(rank[rank.size() - 2], dim, DimVector::POS, rank.back());
    }
  }

  const auto manifold = slice.store.rebuildManifold(slice.at);
  auto cursor         = rank.front();
  for (std::size_t i = 1; i < rank.size(); i++) {
    cursor = manifold.linked(cursor, dim, DimVector::POS);
    EXPECT_EQ(cursor, rank[i]) << "posward, step " << i;
  }
  for (std::size_t i = rank.size() - 1; i > 0; i--) {
    cursor = manifold.linked(cursor, dim, DimVector::NEG);
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
  slice.link(middle, first, DimVector::POS, other);
  slice.link(middle, third, DimVector::NEG, other);

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
  EXPECT_EQ(manifold.dimensionNamed("d.nothing", slice.store), std::nullopt);
}

TEST(ManifoldTest, cloneMasterFollowsTheCloneDimensionNegward) {
  Slice slice;
  const auto clone      = slice.dimension("d.clone");
  const auto master     = slice.cell("quoted");
  const auto copy       = slice.cell("quoted");
  const auto copyOfCopy = slice.cell("quoted");
  slice.link(master, clone, DimVector::POS, copy);
  slice.link(copy, clone, DimVector::POS, copyOfCopy);

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
  slice.link(cell, dim, DimVector::POS, other);
  const auto firstLink = slice.store.cellRefOf(slice.at);
  slice.link(cell, dim, DimVector::POS, noCell);
  const auto secondLink = slice.store.cellRefOf(slice.at);

  const auto manifold = slice.store.rebuildManifold(slice.at);
  const auto slot     = manifold.slot(cell);
  ASSERT_TRUE(slot.has_value());
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
                                   slice.store.dimsDimension(), DimVector::POS,
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
                                     DimVector::POS, rank.back(), &manifold);
      ASSERT_TRUE(manifold.advance(slice.store, slice.at));
      slice.at = slice.store.setLink(slice.at, rank.back(), meta,
                                     DimVector::NEG, rank.front(), &manifold);
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
  slice.link(one, dim, DimVector::POS, two);
  // The drift R9 exists to catch: the caller folded the last operation and
  // never folded the one before it, so the view is a state the spool does not
  // hold. It is the hook that says so, not a crash -- and advance() now says
  // which rule the skipped operation left it unable to satisfy.
  const auto advanced = manifold.advance(slice.store, slice.at);
  ASSERT_FALSE(advanced);
  EXPECT_EQ(
      advanced.error(),
      (zigzag::AdvanceError{.kind    = zigzag::AdvanceError::Kind::Refused,
                            .refusal = zigzag::FoldRefusal::UnknownTarget}));
  EXPECT_FALSE(manifold.verifyAgainstFullRebuild(slice.store));
}

TEST(ManifoldTest, anEphemeralReferenceIsRefusedAtTheApiAndInTheFold) {
  Slice slice;
  const auto dim  = slice.dimension("d.1");
  const auto cell = slice.cell("real");

  // R8's boundary at the API.
  EXPECT_THROW(slice.store.setLink(slice.at, cell, dim, DimVector::POS,
                                   ephemeralBit | 3U),
               std::invalid_argument);
  EXPECT_THROW(slice.store.setLink(slice.at, cell, ephemeralBit | dim,
                                   DimVector::POS, noCell),
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

  EXPECT_THROW(slice.store.setLink(slice.at, textOp, dim, DimVector::POS, cell),
               std::invalid_argument);
  EXPECT_THROW(
      slice.store.setLink(slice.at, cell, textOp, DimVector::POS, cell),
      std::invalid_argument);
  EXPECT_THROW(slice.store.setLink(slice.at, cell, dim, DimVector::POS, textOp),
               std::invalid_argument);
  EXPECT_THROW(slice.store.setLink(slice.at, cell, dim, DimVector::POS, 9999),
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
  EXPECT_EQ(manifold.linked(cell, dim, DimVector::POS), noCell);
}

TEST(ManifoldTest, aSetLinkWithNoChainHasNoSubjectAndIsRefused) {
  Slice slice;
  const auto dim                  = slice.dimension("d.1");
  [[maybe_unused]] const auto one = slice.cell("one");
  const auto two                  = slice.cell("two");

  Op op;
  op.kind  = OpKind::Structure;
  op.flags = xudu::structureFlags(StructureVerb::SetLink);
  op.to    = two;
  op.link  = dim;
  // op.source is empty, so it names no subject
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

  const auto left  = slice.store.setLink(fork, one, dim, DimVector::POS, two);
  const auto right = slice.store.setLink(fork, one, dim, DimVector::POS, three);
  ASSERT_NE(left, right);

  EXPECT_EQ(slice.store.rebuildManifold(left).linked(one, dim, DimVector::POS),
            two);
  EXPECT_EQ(slice.store.rebuildManifold(right).linked(one, dim, DimVector::POS),
            three);
  // Neither branch's link exists at the state they forked from, which is what
  // makes structural editing scrubbable in hypertime.
  EXPECT_EQ(slice.store.rebuildManifold(fork).linked(one, dim, DimVector::POS),
            noCell);
}

TEST(ManifoldTest, aChainIsFollowedPerBranchRatherThanAcrossBranches) {
  Slice slice;
  const auto dim   = slice.dimension("d.1");
  const auto one   = slice.cell("one");
  const auto two   = slice.cell("two");
  const auto three = slice.cell("three");
  const auto fork  = slice.at;

  const auto left = slice.store.setLink(fork, one, dim, DimVector::POS, two);
  // The right branch's operation has to chain to one's *own* history as the
  // fork saw it, not to the operation the left branch appended afterwards.
  const auto right = slice.store.setLink(fork, one, dim, DimVector::NEG, three);
  const auto onward =
      slice.store.setLink(right, one, dim, DimVector::POS, three);

  const auto manifold = slice.store.rebuildManifold(onward);
  EXPECT_EQ(manifold.linked(one, dim, DimVector::POS), three);
  EXPECT_EQ(manifold.linked(one, dim, DimVector::NEG), three);
  EXPECT_EQ(manifold.refusedOps(), 0U);
  EXPECT_TRUE(manifold.verifyAgainstFullRebuild(slice.store));

  const auto leftManifold = slice.store.rebuildManifold(left);
  EXPECT_EQ(leftManifold.linked(one, dim, DimVector::POS), two);
  EXPECT_EQ(leftManifold.linked(one, dim, DimVector::NEG), noCell);
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

  [[nodiscard]] CellRef linked(const DimRef dim,
                               const DimVector dir = DimVector::POS) const {
    for (const auto &slot : dims) {
      if (slot.dim == dim) {
        return dir == DimVector::POS ? slot.pos : slot.neg;
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
    slice.at = slice.store.setLink(slice.at, cells[i], dims[0], DimVector::POS,
                                   cells[i + 1], &manifold);
    ASSERT_TRUE(manifold.advance(slice.store, slice.at));
    slice.at = slice.store.setLink(slice.at, shuffled[i], dims[1],
                                   DimVector::POS, shuffled[i + 1], &manifold);
    ASSERT_TRUE(manifold.advance(slice.store, slice.at));
    for (int d = 2; d < dimCount; d++) {
      slice.at =
          slice.store.setLink(slice.at, cells[i], dims[d], DimVector::POS,
                              cells[(i + d * 977) % cellCount], &manifold);
      ASSERT_TRUE(manifold.advance(slice.store, slice.at));
    }
  }
  // Close both ranks into cycles so a chase never runs off the end.
  slice.at = slice.store.setLink(slice.at, cells.back(), dims[0],
                                 DimVector::POS, cells.front(), &manifold);
  ASSERT_TRUE(manifold.advance(slice.store, slice.at));
  slice.at = slice.store.setLink(slice.at, shuffled.back(), dims[1],
                                 DimVector::POS, shuffled.front(), &manifold);
  ASSERT_TRUE(manifold.advance(slice.store, slice.at));
  manifold.compact();

  // The array baseline, filled from the manifold so both hold identical links.
  std::unordered_map<CellRef, std::size_t> denseOf;
  std::vector<ArrayCell> array(static_cast<std::size_t>(cellCount));
  for (std::size_t i = 0; i < cells.size(); i++) {
    denseOf[cells[i]] = i;
  }
  for (std::size_t i = 0; i < cells.size(); i++) {
    const auto slot = manifold.slot(cells[i]);
    ASSERT_TRUE(slot.has_value());
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
      return found == denseOf.end()
                 ? noCell
                 : array[found->second].linked(dim, DimVector::POS);
    };
  };

  // Both designs must agree before either is timed: a faster wrong answer is
  // not a data point.
  for (const auto cell : {cells.front(), cells[cellCount / 2], cells.back()}) {
    for (int d = 0; d < dimCount; d++) {
      EXPECT_EQ(manifold.linked(cell, dims[d], DimVector::POS),
                arrayHop(dims[d])(cell))
          << "cell " << cell << " dim " << d;
    }
  }

  const auto runSeq = nsPerHop(cells.front(), cellCount, [&](const CellRef c) {
    return manifold.linked(c, dims[0], DimVector::POS);
  });
  const auto runRnd =
      nsPerHop(shuffled.front(), cellCount, [&](const CellRef c) {
        return manifold.linked(c, dims[1], DimVector::POS);
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
  // render-path latency.
  EXPECT_LT(runSeq, 500.0) << "a sequential hop should be nanoseconds";
  EXPECT_LT(runRnd, 2000.0) << "a scattered hop should be nanoseconds";
  // And the falsifiable claim R12 actually rests on: 300 hops is a frame's
  // worth of traversal and must be a rounding error in absolute terms.
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
    at                = store.setLink(at, head, dim, DimVector::POS, tail);
    store.setCurrentVersions({at});
    store.save(dir.string());
  }

  Store reopened(permascroll);
  reopened.load(dir.string());
  EXPECT_EQ(reopened.homeCell(), 1U);
  EXPECT_EQ(reopened.dimsDimension(), 2U);

  const auto manifold =
      reopened.rebuildManifold(reopened.primaryCurrentVersion());
  EXPECT_EQ(manifold.linked(head, dim, DimVector::POS), tail);
  EXPECT_EQ(manifold.linked(tail, dim, DimVector::NEG), head);
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

TEST(ManifoldTest, aSplicePublishesSuccessfullyInCompactBinaryV4) {
  Slice slice;
  const auto cell = slice.cell("the quick brown fox");
  slice.at        = slice.store.spliceCell(slice.at, cell, 4, 5, "slow");

  // In CompactBinaryV4, splice offset and length are carried on the wire, so
  // publishing a spliced slice succeeds.
  EXPECT_NO_THROW(static_cast<void>(slice.store.exportBinaryOps()));

  Slice plain;
  static_cast<void>(plain.cell("unspliced"));
  EXPECT_NO_THROW(static_cast<void>(plain.store.exportBinaryOps()));
}

TEST(ManifoldTest, aCellsHistoryIsEveryOperationThatShapedIt) {
  Slice slice;
  const auto dim  = slice.dimension("d.custom");
  const auto cell = slice.cell("step 1");
  const auto op1  = slice.store.cellRefOf(slice.at);

  slice.at       = slice.store.setCellText(slice.at, cell, "step 2");
  const auto op2 = slice.store.segmentedOps().indexOf(slice.at);

  const auto other = slice.cell("other");
  slice.at = slice.store.setLink(slice.at, cell, dim, DimVector::POS, other);
  const auto op3 = slice.store.segmentedOps().indexOf(slice.at);

  slice.at       = slice.store.setCellText(slice.at, cell, "step 4");
  const auto op4 = slice.store.segmentedOps().indexOf(slice.at);

  const auto manifold = slice.store.rebuildManifold(slice.at);
  const auto hist     = manifold.historyOf(cell);
  EXPECT_EQ(hist, (std::vector<std::uint32_t>{op1, op2, op3, op4}));
  EXPECT_EQ(hist.front(), cell);
}

TEST(ManifoldTest, aCellsHistoryIsPerBranch) {
  Slice slice;
  const auto cell      = slice.cell("initial");
  slice.at             = slice.store.setCellText(slice.at, cell, "common");
  const auto forkState = slice.at;

  const auto branch1Ver =
      slice.store.setCellText(forkState, cell, "branch1 edit");
  const auto branch2Ver =
      slice.store.setCellText(forkState, cell, "branch2 edit");

  const auto manifold1 = slice.store.rebuildManifold(branch1Ver);
  const auto manifold2 = slice.store.rebuildManifold(branch2Ver);

  const auto hist1 = manifold1.historyOf(cell);
  const auto hist2 = manifold2.historyOf(cell);

  ASSERT_EQ(hist1.size(), 3U);
  ASSERT_EQ(hist2.size(), 3U);
  EXPECT_EQ(hist1[0], hist2[0]);
  EXPECT_EQ(hist1[1], hist2[1]);
  EXPECT_NE(hist1[2], hist2[2]);
}

TEST(ManifoldTest, anUnattachedManifoldHasNoHistory) {
  Manifold unattached;
  EXPECT_TRUE(unattached.historyOf(1).empty());
  EXPECT_TRUE(unattached.contentAsOf(1, 1).empty());
}

TEST(ManifoldTest, aCellNobodyTouchedHasAHistoryOfOne) {
  Slice slice;
  const auto cell     = slice.cell("untouched");
  const auto op       = slice.store.cellRefOf(slice.at);
  const auto manifold = slice.store.rebuildManifold(slice.at);

  const auto hist = manifold.historyOf(cell);
  ASSERT_EQ(hist.size(), 1U);
  EXPECT_EQ(hist.front(), op);
  EXPECT_EQ(hist.front(), cell);

  const auto spans = manifold.contentAsOf(cell, op);
  ASSERT_FALSE(spans.empty());
  EXPECT_EQ(slice.store.read(spans.front()), "untouched");
}

TEST(ManifoldTest, contentAsOfReplaysASplice) {
  Slice slice;
  const auto cell = slice.cell("the quick brown fox");
  const auto op1  = slice.store.cellRefOf(slice.at);

  const auto ver2 = slice.store.spliceCell(slice.at, cell, 4, 5, "slow");
  const auto op2  = slice.store.segmentedOps().indexOf(ver2);

  const auto ver3 = slice.store.spliceCell(ver2, cell, 0, 3, "A");
  const auto op3  = slice.store.segmentedOps().indexOf(ver3);

  const auto manifold = slice.store.rebuildManifold(ver3);

  const auto spans1 = manifold.contentAsOf(cell, op1);
  std::string text1;
  for (const auto &s : spans1) {
    text1 += slice.store.read(s);
  }
  EXPECT_EQ(text1, "the quick brown fox");

  const auto spans2 = manifold.contentAsOf(cell, op2);
  std::string text2;
  for (const auto &s : spans2) {
    text2 += slice.store.read(s);
  }
  EXPECT_EQ(text2, "the slow brown fox");

  const auto spans3 = manifold.contentAsOf(cell, op3);
  std::string text3;
  for (const auto &s : spans3) {
    text3 += slice.store.read(s);
  }
  EXPECT_EQ(text3, "A slow brown fox");

  const auto currentSpans = manifold.contentOf(cell);
  ASSERT_EQ(spans3.size(), currentSpans.size());
  for (std::size_t i = 0; i < currentSpans.size(); ++i) {
    EXPECT_EQ(spans3[i], currentSpans[i]);
  }
}

TEST(ManifoldTest, contentAsOfRefusesAnOperationFromAnotherCell) {
  Slice slice;
  const auto cell1                  = slice.cell("first cell");
  [[maybe_unused]] const auto cell2 = slice.cell("second cell");
  const auto opCell2                = slice.store.cellRefOf(slice.at);

  const auto manifold = slice.store.rebuildManifold(slice.at);
  EXPECT_TRUE(manifold.contentAsOf(cell1, opCell2).empty());
  EXPECT_TRUE(manifold.contentAsOf(cell1, 0).empty());
  EXPECT_TRUE(manifold.contentAsOf(cell1, 999999).empty());
}

TEST(ManifoldTest, anOpHandleNamesTheOperationItWasMintedFor) {
  Slice slice;
  const auto vInsert  = slice.store.insert(slice.at, 0, "underlying text");
  const auto targetOp = slice.store.segmentedOps().indexOf(vInsert);
  slice.at            = vInsert;

  const auto handle   = slice.opHandle(targetOp, "commentary");
  const auto manifold = slice.store.rebuildManifold(slice.at);

  EXPECT_EQ(manifold.valueKindOf(handle), ValueKind::OpHandle);
  EXPECT_EQ(manifold.handleTarget(handle), std::optional<CellRef>{targetOp});
  EXPECT_EQ(manifold.textOf(handle, slice.store), "commentary");

  // Non-handle cell answers nullopt
  EXPECT_EQ(manifold.handleTarget(slice.store.homeCell()), std::nullopt);
}

TEST(ManifoldTest, anOpHandleIsNotAScalar) {
  Slice slice;
  const auto vInsert  = slice.store.insert(slice.at, 0, "text");
  const auto targetOp = slice.store.segmentedOps().indexOf(vInsert);
  slice.at            = vInsert;

  const auto handle   = slice.opHandle(targetOp, "label");
  const auto manifold = slice.store.rebuildManifold(slice.at);

  EXPECT_EQ(manifold.asDouble(handle), std::nullopt);
  EXPECT_EQ(manifold.asBool(handle), std::nullopt);
  EXPECT_EQ(manifold.asInt64(handle), std::nullopt);
}

TEST(ManifoldTest, aHandleHasItsOwnHistory) {
  Slice slice;
  const auto vInsert  = slice.store.insert(slice.at, 0, "text");
  const auto targetOp = slice.store.segmentedOps().indexOf(vInsert);
  slice.at            = vInsert;

  const auto handle    = slice.opHandle(targetOp, "initial note");
  const auto manifold1 = slice.store.rebuildManifold(slice.at);

  const auto hist1 = manifold1.historyOf(handle);
  ASSERT_EQ(hist1.size(), 1U);
  EXPECT_EQ(hist1.front(), handle);

  slice.at          = slice.store.setCellText(slice.at, handle, "updated note");
  const auto editOp = slice.store.segmentedOps().indexOf(slice.at);
  const auto manifold2 = slice.store.rebuildManifold(slice.at);

  const auto hist2 = manifold2.historyOf(handle);
  EXPECT_EQ(hist2, (std::vector<std::uint32_t>{handle, editOp}));
}

TEST(ManifoldTest, aHandleMayNameAnyKindOfOperation) {
  Slice slice;
  // 1. Insert
  const auto vInsert  = slice.store.insert(slice.at, 0, "hello");
  const auto opInsert = slice.store.segmentedOps().indexOf(vInsert);
  slice.at            = vInsert;

  // 2. Delete (erase)
  const auto vDelete  = slice.store.erase(slice.at, 0, 2);
  const auto opDelete = slice.store.segmentedOps().indexOf(vDelete);
  slice.at            = vDelete;

  // 3. Link (SetLink)
  const auto dim = slice.dimension("d.test");
  const auto c1  = slice.cell("c1");
  const auto c2  = slice.cell("c2");
  slice.link(c1, dim, DimVector::POS, c2);
  const auto opLink = slice.store.segmentedOps().indexOf(slice.at);

  // 4. MakeCell
  const auto c3     = slice.cell("c3");
  const auto opMake = c3;

  // Mint handles for each kind of operation
  const auto h1 = slice.opHandle(opInsert, "h_insert");
  const auto h2 = slice.opHandle(opDelete, "h_delete");
  const auto h3 = slice.opHandle(opLink, "h_link");
  const auto h4 = slice.opHandle(opMake, "h_make");

  const auto manifold = slice.store.rebuildManifold(slice.at);
  EXPECT_EQ(manifold.handleTarget(h1), std::optional<CellRef>{opInsert});
  EXPECT_EQ(manifold.handleTarget(h2), std::optional<CellRef>{opDelete});
  EXPECT_EQ(manifold.handleTarget(h3), std::optional<CellRef>{opLink});
  EXPECT_EQ(manifold.handleTarget(h4), std::optional<CellRef>{opMake});
}

TEST(ManifoldTest, anEphemeralTargetIsRefused) {
  Slice slice;
  EXPECT_THROW(static_cast<void>(slice.store.makeOpHandle(
                   slice.at, zigzag::ephemeralBit | 42)),
               std::invalid_argument);
  EXPECT_THROW(static_cast<void>(slice.store.makeOpHandle(slice.at, 0)),
               std::invalid_argument);
  EXPECT_THROW(static_cast<void>(slice.store.makeOpHandle(slice.at, 999999)),
               std::invalid_argument);
}

TEST(ManifoldTest, anEditionSurvivesBeingRepointed) {
  Slice slice;
  const auto v1 = slice.store.insert(slice.at, 0, "Version 1 text");
  const auto v2 = slice.store.insert(v1, 0, "Version 2 text");
  slice.at      = v2;

  // Mint "French" pointing to v1
  slice.at        = slice.store.designateEdition(slice.at, "French", v1);
  const auto eds1 = slice.store.editions(slice.at);
  ASSERT_EQ(eds1.size(), 1U);
  EXPECT_EQ(eds1[0].name, "French");
  EXPECT_EQ(eds1[0].targetVersion, v1);
  const auto frenchCell = eds1[0].cell;
  EXPECT_NE(frenchCell, zigzag::noCell);

  // Repoint "French" pointing to v2
  slice.at        = slice.store.designateEdition(slice.at, "French", v2);
  const auto eds2 = slice.store.editions(slice.at);
  ASSERT_EQ(eds2.size(), 1U);
  EXPECT_EQ(eds2[0].name, "French");
  EXPECT_EQ(eds2[0].targetVersion, v2);
  // The edition's identity is the cell itself: it persists across repointing
  EXPECT_EQ(eds2[0].cell, frenchCell);

  // In the folded manifold:
  const auto manifold = slice.store.rebuildManifold(slice.at);
  const auto mEds     = manifold.editions();
  ASSERT_EQ(mEds.size(), 1U);
  EXPECT_EQ(mEds[0].cell, frenchCell);
  EXPECT_EQ(mEds[0].name, "French");
  EXPECT_EQ(mEds[0].targetOp, slice.store.segmentedOps().indexOf(v2));

  // Assert Manifold::historyOf shows both designations
  const auto hist = manifold.historyOf(frenchCell);
  EXPECT_GE(hist.size(), 2U);
  EXPECT_EQ(hist.front(), frenchCell);
}

TEST(ManifoldTest, twoBranchesCarryTheirOwnEditions) {
  Slice slice;
  const auto v0    = slice.store.insert(slice.at, 0, "Base document");
  slice.at         = slice.store.designateEdition(v0, "French", v0);
  const auto vBase = slice.at;

  // Branch A
  const auto vA1      = slice.store.insert(vBase, 0, "Branch A changes ");
  const auto vBranchA = slice.store.designateEdition(vA1, "French", vA1);

  // Branch B
  const auto vB1 = slice.store.insert(vBase, 0, "Branch B changes ");
  // In Branch B, French was never repointed. It should still point to v0.

  const auto edA = slice.store.editionNamed(vBranchA, "French");
  ASSERT_TRUE(edA.has_value());
  EXPECT_EQ(edA->targetVersion, vA1);

  const auto edB = slice.store.editionNamed(vB1, "French");
  ASSERT_TRUE(edB.has_value());
  EXPECT_EQ(edB->targetVersion, v0);

  // In folded manifolds
  const auto manifoldA = slice.store.rebuildManifold(vBranchA);
  const auto mEdA      = manifoldA.editionNamed("French");
  ASSERT_TRUE(mEdA.has_value());
  EXPECT_EQ(mEdA->targetOp, slice.store.segmentedOps().indexOf(vA1));

  const auto manifoldB = slice.store.rebuildManifold(vB1);
  const auto mEdB      = manifoldB.editionNamed("French");
  ASSERT_TRUE(mEdB.has_value());
  EXPECT_EQ(mEdB->targetOp, slice.store.segmentedOps().indexOf(v0));
}

TEST(ManifoldTest, theEditionsRankAndTheTableCacheAgree) {
  Slice slice;
  const auto v1 = slice.store.insert(slice.at, 0, "French text");
  const auto v2 = slice.store.insert(v1, 0, "English text");
  const auto v3 = slice.store.insert(v2, 0, "German text");
  slice.at      = v3;

  slice.at = slice.store.designateEdition(slice.at, "French", v1);
  slice.at = slice.store.designateEdition(slice.at, "English", v2);
  slice.at = slice.store.designateEdition(slice.at, "German", v3);

  // Rebuilding the manifold reconciles currentVersions cache to rank order
  const auto manifold = slice.store.rebuildManifold(slice.at);
  const auto expected = std::vector<MicroversionId>{v1, v2, v3};
  EXPECT_EQ(slice.store.currentVersions(), expected);

  // If table cache is diverged or tampered with:
  slice.store.setCurrentVersions({v3, v1});
  EXPECT_NE(slice.store.currentVersions(), expected);

  // A fold enforces the rank over the cache
  const auto foldAfterDivergence = slice.store.rebuildManifold(slice.at);
  EXPECT_EQ(slice.store.currentVersions(), expected);
}

TEST(ManifoldTest, aVersionAnnotationBecomesAHandleCell) {
  Slice slice;
  const auto v1 = slice.store.insert(slice.at, 0, "Release content");
  slice.at      = slice.store.designateEdition(v1, "v1.0-release", v1);

  const auto manifold = slice.store.rebuildManifold(slice.at);
  const auto ed       = manifold.editionNamed("v1.0-release");
  ASSERT_TRUE(ed.has_value());
  EXPECT_NE(ed->handle, zigzag::noCell);

  // The handle is a ValueKind::OpHandle cell pointing to v1's operation
  EXPECT_EQ(manifold.valueKindOf(ed->handle), ValueKind::OpHandle);
  const auto expectedOp = slice.store.segmentedOps().indexOf(v1);
  EXPECT_EQ(manifold.handleTarget(ed->handle),
            std::optional<CellRef>{expectedOp});

  // Querying aliases and display names resolves through the edition
  EXPECT_EQ(slice.store.resolveAlias("v1.0-release"), v1);
  EXPECT_EQ(slice.store.displayName(v1), "v1.0-release");
}

TEST(ManifoldTest, anAnnotationKeepsStateSeparateFromClaimedTime) {
  Slice slice;
  const auto v1          = slice.store.insert(slice.at, 0, "Annotated state");
  const auto claimedTime = "2026-09-24T03:00:00Z";

  // Annotate v1 with full metadata including claimed timestamp
  slice.at = slice.store.annotateVersion(v1, v1,
                                         {.alias       = "v1.0",
                                          .description = "First milestone",
                                          .tag         = "milestone",
                                          .timestamp   = claimedTime});

  const auto manifold = slice.store.rebuildManifold(slice.at);
  const auto targetOp = slice.store.segmentedOps().indexOf(v1);

  // The OpHandle cell names the microversion operation index (state identity)
  const auto handle = manifold.findOpHandle(targetOp);
  ASSERT_TRUE(handle.has_value());
  EXPECT_EQ(manifold.valueKindOf(*handle), ValueKind::OpHandle);
  EXPECT_EQ(manifold.handleTarget(*handle), std::optional<CellRef>{targetOp});

  // Querying the annotation returns claimed wall-clock time as a distinct field
  const auto ann = slice.store.versionAnnotation(v1);
  ASSERT_TRUE(ann.has_value());
  EXPECT_EQ(ann->alias, "v1.0");
  EXPECT_EQ(ann->description, "First milestone");
  EXPECT_EQ(ann->tag, "milestone");
  EXPECT_EQ(ann->timestamp, claimedTime);

  // An annotation without claimed timestamp has an empty timestamp field;
  // it is never synthesized or derived from the MicroversionId
  const auto v2 = slice.store.insert(slice.at, 15, " and another");
  slice.at = slice.store.annotateVersion(v2, v2,
                                         {.alias       = "v2.0",
                                          .description = "No clock reading",
                                          .tag         = "unclocked",
                                          .timestamp   = ""});

  const auto ann2 = slice.store.versionAnnotation(v2);
  ASSERT_TRUE(ann2.has_value());
  EXPECT_TRUE(ann2->timestamp.empty())
      << "timestamp must never be inferred from a MicroversionId";
  EXPECT_EQ(ann2->alias, "v2.0");
}

TEST(ManifoldTest, theFoldedRegistryMatchesAColdRebuild) {
  Slice slice;
  const auto key1 = "btpk:0123456789abcdef0123456789abcdef0123456789abcdef"
                    "0123456789abcdef:salt1";
  const auto key2 = "btpk:fedcba9876543210fedcba9876543210fedcba9876543210"
                    "fedcba9876543210:salt2";

  slice.at = slice.store.registerScroll(slice.at, key1);
  slice.at = slice.store.registerScroll(slice.at, key2);

  const auto manifold = slice.store.rebuildManifold(slice.at);
  EXPECT_TRUE(manifold.verifyAgainstFullRebuild(slice.store))
      << "folded registry must match cold rebuild exactly";

  const auto reg = slice.store.scrollRegistry();
  ASSERT_EQ(reg.size(), 2U);
  EXPECT_EQ(reg.scrollIdForKey(key1), std::optional<xudu::ScrollId>{1});
  EXPECT_EQ(reg.scrollIdForKey(key2), std::optional<xudu::ScrollId>{2});
  ASSERT_NE(reg.recordForId(1), nullptr);
  EXPECT_EQ(reg.recordForId(1)->globalKey, key1);
  ASSERT_NE(reg.recordForId(2), nullptr);
  EXPECT_EQ(reg.recordForId(2)->globalKey, key2);
}

TEST(ManifoldTest, aRegistryCellMayUseAnAlreadyMappedNonZeroScroll) {
  Slice slice;
  const auto key1 = "btpk:" + std::string(64, '1') + ":salt1";
  const auto key2 = "btpk:" + std::string(64, '2') + ":salt2";

  // Register first scroll using local content (scroll 0)
  slice.at        = slice.store.registerScroll(slice.at, key1);
  const auto reg1 = slice.store.scrollRegistry();
  ASSERT_EQ(reg1.size(), 1U);
  const auto scroll1Id = reg1.scrolls.front().id;
  EXPECT_EQ(scroll1Id, 1U);

  // Mint a second cell on d.scrolls whose content span points into scroll 1!
  Op op;
  op.kind  = OpKind::Structure;
  op.flags = xudu::structureFlags(StructureVerb::MakeCell);
  op.span =
      PrimediaSpan{.scroll = scroll1Id, .start = 0, .length = key2.size()};
  slice.at         = slice.store.apply(slice.at, op);
  const auto cell2 = slice.store.cellRefOf(slice.at);

  const auto dimScrolls = slice.dimension("d.scrolls");
  const auto cell1      = reg1.scrolls.front().cell;
  slice.link(cell1, dimScrolls, DimVector::POS, cell2);

  // Define a mock reader that resolves scroll 0 (from store) and scroll 1
  // (key2)
  struct MockReader : public xudu::SpanReader {
    const Store &store;
    std::string key2Content;
    MockReader(const Store &s, std::string k2)
        : store(s), key2Content(std::move(k2)) {}
    std::string read(const PrimediaSpan &span) const override {
      if (span.scroll == xudu::localScroll) {
        return store.read(span);
      }
      if (span.scroll == 1) {
        return key2Content.substr(span.start, span.length);
      }
      return {};
    }
  } reader(slice.store, key2);

  const auto manifold = slice.store.rebuildManifold(slice.at);
  const auto reg2     = manifold.scrollRegistry(reader);
  ASSERT_EQ(reg2.size(), 2U);
  EXPECT_EQ(reg2.scrolls[0].globalKey, key1);
  EXPECT_EQ(reg2.scrolls[1].globalKey, key2);
  EXPECT_EQ(reg2.scrolls[1].id, 2U);
}

TEST(ManifoldTest, anUnrootedRegistryDependencyIsRefused) {
  Slice slice;
  const auto key1  = "btpk:" + std::string(64, '1') + ":salt1";
  slice.at         = slice.store.registerScroll(slice.at, key1);
  const auto reg1  = slice.store.scrollRegistry();
  const auto cell1 = reg1.scrolls.front().cell;

  // Mint an unrooted cell referencing unmapped scroll 99
  Op op;
  op.kind  = OpKind::Structure;
  op.flags = xudu::structureFlags(StructureVerb::MakeCell);
  op.span  = PrimediaSpan{.scroll = 99, .start = 0, .length = 10};
  slice.at = slice.store.apply(slice.at, op);
  const auto unrootedCell = slice.store.cellRefOf(slice.at);

  const auto dimScrolls = slice.dimension("d.scrolls");
  slice.link(cell1, dimScrolls, DimVector::POS, unrootedCell);

  const auto manifold = slice.store.rebuildManifold(slice.at);
  EXPECT_THROW(static_cast<void>(manifold.scrollRegistry(slice.store)),
               zigzag::UnrootedRegistryDependency);
}

TEST(ManifoldTest, aPlaceholderLinksToItsScrollCell) {
  Slice slice;
  const auto key = "btpk:" + std::string(64, '3') + ":root";
  slice.at       = slice.store.registerScroll(slice.at, key);

  const auto reg = slice.store.scrollRegistry();
  ASSERT_EQ(reg.size(), 1U);
  const auto scrollCell = reg.scrolls.front().cell;
  const auto expectedId = reg.scrolls.front().id;

  // Mint placeholder cells P1 and P2
  const auto p1 = slice.cell("extern_placeholder_1");
  const auto p2 = slice.cell("extern_placeholder_2");

  slice.at = slice.store.linkScrollRef(slice.at, scrollCell, p1);
  slice.at = slice.store.linkScrollRef(slice.at, scrollCell, p2);

  const auto manifold  = slice.store.rebuildManifold(slice.at);
  const auto activeReg = manifold.scrollRegistry(slice.store);

  // Placeholders resolve in O(1) byCell lookup to the parent scroll's id
  EXPECT_EQ(activeReg.scrollIdForCell(p1),
            std::optional<xudu::ScrollId>{expectedId});
  EXPECT_EQ(activeReg.scrollIdForCell(p2),
            std::optional<xudu::ScrollId>{expectedId});
}

TEST(ManifoldTest, publishedHistoryBootstrapsThroughAuthorship) {
  // Author side: create document and register a scroll
  const auto authorPerma = std::make_shared<xudu::UserPermascroll>();
  Store authorStore(authorPerma);
  auto at                    = authorStore.sliceGenesis(MicroversionId{});
  const auto authorGlobalKey = "btpk:" + std::string(64, 'a') + ":permascroll";
  const auto targetKey       = "btpk:" + std::string(64, 'b') + ":salt";
  at                         = authorStore.registerScroll(at, targetKey);

  const auto scratchDir =
      std::filesystem::temp_directory_path() /
      ("published-bootstrap-test-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(scratchDir);
  authorStore.save(scratchDir.string());

  // Opener side: fresh opener with their own empty permascroll
  const auto openerPerma = std::make_shared<xudu::UserPermascroll>();
  Store openerStore(openerPerma);
  // Configure bootstrap reader for author's global permascroll
  openerStore.setBootstrapPermascroll(
      authorGlobalKey, [&](const PrimediaSpan &span) {
        return xudu::ResolveResult{.status =
                                       xudu::ResolutionStatus::VerifiedBytes,
                                   .text       = authorPerma->read(span),
                                   .lockInfo   = std::nullopt,
                                   .holeRecord = std::nullopt};
      });

  openerStore.load(scratchDir.string());
  const auto reg = openerStore.scrollRegistry();
  ASSERT_EQ(reg.size(), 1U);
  EXPECT_EQ(reg.scrolls.front().globalKey, targetKey);
  EXPECT_EQ(reg.scrollIdForKey(targetKey), std::optional<xudu::ScrollId>{1});
}

TEST(ManifoldTest, aStoreDirectoryHoldsOnlyOpsAndLocalFacts) {
  const auto dir =
      std::filesystem::temp_directory_path() /
      ("local-facts-test-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(dir);

  const auto perma = std::make_shared<xudu::UserPermascroll>();
  const auto key   = "btpk:" + std::string(64, 'c') + ":scroll";
  {
    Store store(perma);
    auto at = store.sliceGenesis(MicroversionId{});
    at      = store.registerScroll(at, key);
    xudu::ScrollSegment seg;
    seg.at       = 0;
    seg.length   = 100;
    seg.mimeType = "text/plain";
    store.addSegment(xudu::localScroll, seg);
    store.save(dir.string());
  }

  // Inspect the store.tables file directly
  const auto tables = xudu::readStoreTables(dir / "store.tables");
  EXPECT_FALSE(tables.documentId.str().empty());
  EXPECT_EQ(tables.localSegments.size(), 1U);

  // Reopen and verify scroll was replayed from ops rather than store.tables
  Store reopened(perma);
  reopened.load(dir.string());
  const auto reg = reopened.scrollRegistry();
  ASSERT_EQ(reg.size(), 1U);
  EXPECT_EQ(reg.scrolls.front().globalKey, key);
}

class Keyring {
public:
  Keyring() {
    path = std::filesystem::temp_directory_path() /
           ("xudu-gpg-" + std::to_string(getpid()));
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
    std::filesystem::create_directories(path);
    std::filesystem::permissions(path, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace,
                                 ignored);
    setenv("GNUPGHOME", path.c_str(), 1);

    {
      std::ofstream agent(path / "gpg-agent.conf");
      agent << "allow-loopback-pinentry\n";
      std::ofstream options(path / "gpg.conf");
      options << "pinentry-mode loopback\n";
    }
    made = 0 == std::system(("gpg --batch --passphrase '' --quick-generate-key "
                             "'Ada Lovelace <ada@example.org>' ed25519 sign "
                             "never >/dev/null 2>&1"));
  }
  ~Keyring() {
    std::system("gpgconf --kill gpg-agent >/dev/null 2>&1");
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
    unsetenv("GNUPGHOME");
  }

  Keyring(const Keyring &)            = delete;
  Keyring &operator=(const Keyring &) = delete;
  Keyring(Keyring &&)                 = delete;
  Keyring &operator=(Keyring &&)      = delete;

  [[nodiscard]] bool usable() const { return made; }

private:
  std::filesystem::path path;
  bool made{};
};

xudu::Provenance sampleProv() {
  xudu::Provenance record;
  record.author.name   = "Ada Lovelace";
  record.author.email  = "ada@example.org";
  record.author.gpgKey = "ada@example.org";
  record.title         = "Notes: on the Analytical Engine";
  record.salt          = "notes";
  record.publisher     = std::string(64, 'a');
  record.permascroll   = "btpk:" + std::string(64, 'a') + ":permascroll";
  record.version       = "2a4";
  record.published     = 1700000000;
  record.contentLength = 4096;
  record.contentDigest = std::string(64, 'b');
  record.extra.emplace_back("custom_field", "custom_val");
  return record;
}

TEST(ManifoldTest, verifiedAuthorshipAppearsAsEphemeralArenaCells) {
  Keyring keyring;
  if (!keyring.usable()) {
    GTEST_SKIP() << "gpg is not available or could not create test key";
  }

  const auto record     = sampleProv();
  const auto signedProv = xudu::signProvenance(record);
  ASSERT_FALSE(signedProv.signature.empty());

  const auto perma = std::make_shared<xudu::UserPermascroll>();
  Store store(perma);
  auto at = store.sliceGenesis(MicroversionId{});
  at      = store.registerScroll(at, record.permascroll);

  store.setBootstrapPermascroll(
      record.permascroll, [perma](const PrimediaSpan &span) {
        return xudu::ResolveResult{.status =
                                       xudu::ResolutionStatus::VerifiedBytes,
                                   .text       = perma->read(span),
                                   .lockInfo   = std::nullopt,
                                   .holeRecord = std::nullopt};
      });
  store.setProvenance(signedProv);

  const auto base = store.rebuildManifold(at);
  const zigzag::ArenaManifold arena(&base, &store);

  // Root reachable from arena home on d.authorship
  const auto dimAuthorship = arena.dimensionNamed("d.authorship");
  ASSERT_NE(dimAuthorship, zigzag::noCell);
  const auto authRoot =
      arena.linked(arena.home(), dimAuthorship, zigzag::DimVector::POS);
  ASSERT_NE(authRoot, zigzag::noCell);
  EXPECT_TRUE(zigzag::isEphemeral(authRoot));

  // Source link points back to authenticated bootstrap scroll
  const auto dimSource = arena.dimensionNamed("d.source");
  ASSERT_NE(dimSource, zigzag::noCell);
  const auto bootstrapCell =
      arena.linked(authRoot, dimSource, zigzag::DimVector::POS);
  EXPECT_NE(bootstrapCell, zigzag::noCell);

  // Canonical fields
  const auto dimAuthor = arena.dimensionNamed("d.author");
  ASSERT_NE(dimAuthor, zigzag::noCell);
  const auto authorCell =
      arena.linked(authRoot, dimAuthor, zigzag::DimVector::POS);
  ASSERT_NE(authorCell, zigzag::noCell);
  EXPECT_TRUE(zigzag::isEphemeral(authorCell));
  EXPECT_EQ(arena.textOf(authorCell), "Ada Lovelace");

  const auto dimEmail = arena.dimensionNamed("d.email");
  ASSERT_NE(dimEmail, zigzag::noCell);
  const auto emailCell =
      arena.linked(authRoot, dimEmail, zigzag::DimVector::POS);
  ASSERT_NE(emailCell, zigzag::noCell);
  EXPECT_TRUE(zigzag::isEphemeral(emailCell));
  EXPECT_EQ(arena.textOf(emailCell), "ada@example.org");

  const auto dimPublisher = arena.dimensionNamed("d.publisher");
  ASSERT_NE(dimPublisher, zigzag::noCell);
  const auto pubCell =
      arena.linked(authRoot, dimPublisher, zigzag::DimVector::POS);
  ASSERT_NE(pubCell, zigzag::noCell);
  EXPECT_TRUE(zigzag::isEphemeral(pubCell));
  EXPECT_EQ(arena.textOf(pubCell), record.publisher);

  const auto dimPerma = arena.dimensionNamed("d.permascroll");
  ASSERT_NE(dimPerma, zigzag::noCell);
  const auto permaCell =
      arena.linked(authRoot, dimPerma, zigzag::DimVector::POS);
  ASSERT_NE(permaCell, zigzag::noCell);
  EXPECT_TRUE(zigzag::isEphemeral(permaCell));
  EXPECT_EQ(arena.textOf(permaCell), record.permascroll);

  const auto dimPublished = arena.dimensionNamed("d.published");
  ASSERT_NE(dimPublished, zigzag::noCell);
  const auto pubDateCell =
      arena.linked(authRoot, dimPublished, zigzag::DimVector::POS);
  ASSERT_NE(pubDateCell, zigzag::noCell);
  EXPECT_TRUE(zigzag::isEphemeral(pubDateCell));
  EXPECT_EQ(arena.textOf(pubDateCell), std::to_string(record.published));

  const auto dimSig = arena.dimensionNamed("d.signature");
  ASSERT_NE(dimSig, zigzag::noCell);
  const auto sigCell = arena.linked(authRoot, dimSig, zigzag::DimVector::POS);
  ASSERT_NE(sigCell, zigzag::noCell);
  EXPECT_TRUE(zigzag::isEphemeral(sigCell));
  EXPECT_EQ(arena.textOf(sigCell), signedProv.signature);

  // Unknown TSV keys remain visible on derived field dimensions
  const auto dimCustom = arena.dimensionNamed("d.custom_field");
  ASSERT_NE(dimCustom, zigzag::noCell);
  const auto customCell =
      arena.linked(authRoot, dimCustom, zigzag::DimVector::POS);
  ASSERT_NE(customCell, zigzag::noCell);
  EXPECT_TRUE(zigzag::isEphemeral(customCell));
  EXPECT_EQ(arena.textOf(customCell), "custom_val");
}

TEST(ManifoldTest, authorshipProjectionNeverMintsPersistentOps) {
  Keyring keyring;
  if (!keyring.usable()) {
    GTEST_SKIP() << "gpg is not available or could not create test key";
  }

  const auto record     = sampleProv();
  const auto signedProv = xudu::signProvenance(record);
  ASSERT_FALSE(signedProv.signature.empty());

  const auto perma = std::make_shared<xudu::UserPermascroll>();
  Store store(perma);
  auto at = store.sliceGenesis(MicroversionId{});
  at      = store.registerScroll(at, record.permascroll);
  store.setProvenance(signedProv);

  const auto opsBefore = store.opCount();

  {
    const auto base = store.rebuildManifold(at);
    zigzag::ArenaManifold arena(&base, &store);

    const auto dimAuthorship = arena.dimensionNamed("d.authorship");
    ASSERT_NE(dimAuthorship, zigzag::noCell);
    const auto authRoot =
        arena.linked(arena.home(), dimAuthorship, zigzag::DimVector::POS);
    ASSERT_NE(authRoot, zigzag::noCell);

    // Promoting the authorship root directly must be refused
    const auto res = zigzag::promote(store, at, arena, authRoot);
    EXPECT_FALSE(res.has_value());

    // Promoting from home must exclude provenance cells
    const auto resHome = zigzag::promote(store, at, arena, arena.home());
    if (resHome.has_value()) {
      for (const auto cell : resHome->cells) {
        EXPECT_FALSE(arena.isProvenanceCell(cell));
      }
    }
  }

  // Building, querying, and dropping the view leaves the store's op count
  // unchanged
  EXPECT_EQ(store.opCount(), opsBefore);
}

TEST(ManifoldTest, tamperedAuthorshipIsNotProjected) {
  Keyring keyring;
  if (!keyring.usable()) {
    GTEST_SKIP() << "gpg is not available or could not create test key";
  }

  const auto record = sampleProv();
  auto signedProv   = xudu::signProvenance(record);
  ASSERT_FALSE(signedProv.signature.empty());

  // Tamper with the TSV content
  signedProv.tsv += "tampered\ttrue\n";

  const auto perma = std::make_shared<xudu::UserPermascroll>();
  Store store(perma);
  auto at = store.sliceGenesis(MicroversionId{});
  at      = store.registerScroll(at, record.permascroll);

  // Failed verification prevents publication open / provenance setting
  EXPECT_THROW(store.setProvenance(signedProv), std::runtime_error);

  // If setVerifiedProvenance is forced, ArenaManifold verification still
  // prevents projection
  store.setVerifiedProvenance(signedProv);
  const auto base = store.rebuildManifold(at);
  const zigzag::ArenaManifold arena(&base, &store);

  EXPECT_EQ(arena.dimensionNamed("d.authorship"), zigzag::noCell);
  EXPECT_EQ(arena.authorshipRoot(), zigzag::noCell);
}

TEST(ManifoldTest, aLinkIsACellWithAnAddress) {
  const auto perma = std::make_shared<xudu::UserPermascroll>();
  Store store(perma);
  auto at = store.sliceGenesis(MicroversionId{});

  const auto spanA = perma->append("Theodor Holm Nelson");
  const auto spanB = perma->append("Literary Machines");

  Link link;
  link.type  = LinkType::Comment;
  link.tier  = ProminenceTier::Author;
  link.owner = "Ted";
  link.left  = {spanA};
  link.right = {spanB};

  at = store.addLink(at, link);

  const auto links = store.links();
  ASSERT_EQ(links.size(), 1U);
  const auto &[id, stored] = *links.begin();

  EXPECT_NE(id, zigzag::noCell);
  EXPECT_EQ(stored.id, id);
  EXPECT_EQ(stored.type, LinkType::Comment);
  EXPECT_EQ(stored.tier, ProminenceTier::Author);
  EXPECT_EQ(stored.owner, "Ted");
  EXPECT_EQ(stored.left, link.left);
  EXPECT_EQ(stored.right, link.right);

  const auto manifold = store.rebuildManifold(at);
  EXPECT_TRUE(manifold.contains(id));
  EXPECT_TRUE(manifold.verifyAgainstFullRebuild(store));

  const auto dimLinks = manifold.dimensionNamed("d.links", store);
  ASSERT_TRUE(dimLinks.has_value());
  const auto dimFrom = manifold.dimensionNamed("d.from", store);
  ASSERT_TRUE(dimFrom.has_value());
  const auto dimTo = manifold.dimensionNamed("d.to", store);
  ASSERT_TRUE(dimTo.has_value());

  // Link cell is linked on d.links posward from home
  EXPECT_EQ(manifold.linked(manifold.home(), *dimLinks, zigzag::DimVector::POS),
            id);

  // Link cell has endpoints on d.from and d.to
  const auto fromCell = manifold.linked(id, *dimFrom, zigzag::DimVector::POS);
  EXPECT_NE(fromCell, zigzag::noCell);
  EXPECT_EQ(manifold.contentOf(fromCell).size(), 1U);
  EXPECT_EQ(manifold.contentOf(fromCell).front(), spanA);

  const auto toCell = manifold.linked(id, *dimTo, zigzag::DimVector::POS);
  EXPECT_NE(toCell, zigzag::noCell);
  EXPECT_EQ(manifold.contentOf(toCell).size(), 1U);
  EXPECT_EQ(manifold.contentOf(toCell).front(), spanB);
}

TEST(ManifoldTest, aMultiSpanLinkSurvivesPublicationAndTransclusion) {
  const auto dir =
      std::filesystem::temp_directory_path() /
      ("multispan-link-test-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(dir);

  const auto perma = std::make_shared<xudu::UserPermascroll>();
  Link originalLink;
  {
    Store store(perma);
    auto at = store.sliceGenesis(MicroversionId{});

    const auto spanA1 = perma->append("Multi-span left end piece 1. ");
    perma->append(
        "Filler separating left pieces so R9 does not coalesce them.");
    const auto spanA2 = perma->append("Multi-span left end piece 2.");
    const auto spanB1 = perma->append("Multi-span right end piece 1. ");
    perma->append(
        "Filler separating right pieces so R9 does not coalesce them.");
    const auto spanB2 = perma->append("Multi-span right end piece 2.");

    originalLink.type    = LinkType::Quotation;
    originalLink.tier    = ProminenceTier::Curated;
    originalLink.owner   = "Theodor_Holm_Nelson";
    originalLink.curator = "btpk:" + std::string(64, 'e');
    originalLink.left    = {spanA1, spanA2};
    originalLink.right   = {spanB1, spanB2};

    at = store.addLink(at, originalLink);
    store.save(dir.string());
  }

  // Reload store from disk
  Store reloaded(perma);
  reloaded.load(dir.string());

  const auto &links = reloaded.links();
  ASSERT_EQ(links.size(), 1U);
  const auto &[id, loaded] = *links.begin();

  EXPECT_NE(id, zigzag::noCell);
  EXPECT_EQ(loaded.id, id);
  EXPECT_EQ(loaded.type, LinkType::Quotation);
  EXPECT_EQ(loaded.tier, ProminenceTier::Curated);
  EXPECT_EQ(loaded.owner, "Theodor_Holm_Nelson");
  EXPECT_EQ(loaded.curator, "btpk:" + std::string(64, 'e'));
  EXPECT_EQ(loaded.left.size(), 2U);
  EXPECT_EQ(loaded.right.size(), 2U);
  EXPECT_EQ(loaded.left, originalLink.left);
  EXPECT_EQ(loaded.right, originalLink.right);

  const auto manifold = reloaded.rebuildManifold(reloaded.latest());
  EXPECT_TRUE(manifold.verifyAgainstFullRebuild(reloaded));
}

TEST(ManifoldTest, anotherAuthorsLinkSetFoldsOverMine) {
  const auto perma = std::make_shared<xudu::UserPermascroll>();

  Store storeMine(perma);
  auto atMine          = storeMine.sliceGenesis(MicroversionId{});
  const auto spanMineA = perma->append("My text on side A");
  const auto spanMineB = perma->append("My text on side B");

  Link linkMine;
  linkMine.type  = LinkType::Comment;
  linkMine.tier  = ProminenceTier::Author;
  linkMine.owner = "AuthorMine";
  linkMine.left  = {spanMineA};
  linkMine.right = {spanMineB};
  atMine         = storeMine.addLink(atMine, linkMine);

  // Author Theirs receives Author Mine's document, and authors links over it
  Store storeTheirs(perma);
  storeTheirs.adoptOpRecords(storeMine.opRecords());
  auto atTheirs          = atMine;
  const auto spanTheirsA = perma->append("Their commentary on side A");
  const auto spanTheirsB = perma->append("Their response on side B");

  Link linkTheirs;
  linkTheirs.type  = LinkType::Disagreement;
  linkTheirs.tier  = ProminenceTier::Curated;
  linkTheirs.owner = "AuthorTheirs";
  linkTheirs.left  = {spanTheirsA};
  linkTheirs.right = {spanTheirsB};
  atTheirs         = storeTheirs.addLink(atTheirs, linkTheirs);

  // Adopt Author Theirs op records into storeMine
  storeMine.adoptOpRecords(storeTheirs.opRecords());

  const auto allLinks = storeMine.links();
  ASSERT_EQ(allLinks.size(), 2U);

  std::vector<std::string> owners;
  for (const auto &[id, l] : allLinks) {
    owners.push_back(l.owner);
  }
  EXPECT_THAT(owners,
              testing::UnorderedElementsAre("AuthorMine", "AuthorTheirs"));

  const auto manifold = storeMine.rebuildManifold(storeMine.latest());
  EXPECT_TRUE(manifold.verifyAgainstFullRebuild(storeMine));
}
