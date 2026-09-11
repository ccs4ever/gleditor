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

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
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
