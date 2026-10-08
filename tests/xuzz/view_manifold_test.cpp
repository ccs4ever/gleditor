/**
 * @file view_manifold_test.cpp
 * @brief The slice view space: two arenas over one manifold, the one write
 *        path into them, and the toss (design/view-system.md §6, §8.2).
 *
 * The first test is spike S4 (plan §3): the two arenas number their cells
 * from the same base, so the first cell of each has the same CellRef, and
 * every call has to be shown never to answer one from the other.
 */
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include "common/xanadu/view/view_manifold.hpp"
#include "common/xanadu/zigzag/manifold.hpp"
#include "view_space_fixture.hpp"

namespace {

using xanadu::view::Layer;
using xanadu::view::ViewCellRef;
using xanadu::view::ViewError;
using xanadu::view::ViewManifold;
using xuzz_test::expectSound;
using xuzz_test::RealSlice;
using xuzz_test::violationsOf;
using zigzag::DimVector;

constexpr auto pos = DimVector::POS;
constexpr auto neg = DimVector::NEG;

ViewCellRef mint(ViewManifold &space, const Layer layer) {
  const auto cell = space.mint(layer);
  EXPECT_TRUE(cell.has_value());
  return cell.value_or(ViewCellRef{});
}

ViewCellRef occurrence(ViewManifold &space, const Layer layer,
                       const zigzag::CellRef target) {
  const auto cell = space.mintOccurrence(layer, target);
  EXPECT_TRUE(cell.has_value());
  return cell.value_or(ViewCellRef{});
}

// -- S4: equal-numbered refs of the two arenas -------------------------------

TEST(ViewManifoldTest, theFirstCellOfEachArenaIsNeverReadFromTheOther) {
  RealSlice slice;
  const auto base = slice.manifold();
  ViewManifold space{base};

  // The premise: the same number in both arenas, told apart only by layer.
  const auto bound   = occurrence(space, Layer::Binding, slice.cells[0]);
  const auto derived = occurrence(space, Layer::Derived, slice.cells[2]);
  ASSERT_EQ(bound.ref, derived.ref);
  ASSERT_NE(bound, derived);

  const auto bDim  = mint(space, Layer::Binding);
  const auto dDim  = mint(space, Layer::Derived);
  const auto bNext = mint(space, Layer::Binding);
  const auto dNext = mint(space, Layer::Derived);
  ASSERT_EQ(bDim.ref, dDim.ref);
  ASSERT_EQ(bNext.ref, dNext.ref);

  // Every read answers from the ref's own arena.
  EXPECT_EQ(space.target(bound), slice.cells[0]);
  EXPECT_EQ(space.target(derived), slice.cells[2]);
  EXPECT_EQ(space.resolveReal(bound), slice.cells[0]);
  EXPECT_EQ(space.resolveReal(derived), slice.cells[2]);

  ASSERT_TRUE(space.link(Layer::Binding, bound, bDim, pos, bNext));
  EXPECT_EQ(space.linked(bound, bDim, pos), bNext);
  EXPECT_EQ(space.linked(bNext, bDim, neg), bound);
  // The same numbers asked of the derived arena: nothing was linked there.
  EXPECT_EQ(space.linked(derived, dDim, pos), std::nullopt);
  EXPECT_EQ(space.linked(dNext, dDim, neg), std::nullopt);
  // And a read that mixes the two arenas is no read at all.
  EXPECT_EQ(space.linked(bound, dDim, pos), std::nullopt);
  EXPECT_EQ(space.linked(derived, bDim, pos), std::nullopt);

  // A write whose layer disagrees with any one of its refs is refused (G1).
  EXPECT_EQ(space.link(Layer::Derived, bound, dDim, pos, dNext),
            std::unexpected{ViewError::RealCellInViewLink});
  EXPECT_EQ(space.link(Layer::Derived, derived, bDim, pos, dNext),
            std::unexpected{ViewError::RealCellInViewLink});
  EXPECT_EQ(space.link(Layer::Derived, derived, dDim, pos, bNext),
            std::unexpected{ViewError::RealCellInViewLink});
  EXPECT_EQ(space.link(Layer::Binding, derived, dDim, pos, dNext),
            std::unexpected{ViewError::RealCellInViewLink});
  EXPECT_EQ(space.unlink(Layer::Derived, bound, bDim, pos),
            std::unexpected{ViewError::RealCellInViewLink});
  EXPECT_EQ(space.unlink(Layer::Binding, bound, dDim, pos),
            std::unexpected{ViewError::RealCellInViewLink});

  // A ref claiming the other arena's generation is stale, not reinterpreted.
  const ViewCellRef forged{
      .ref = bound.ref, .epoch = space.epoch(), .layer = Layer::Binding};
  EXPECT_EQ(space.link(Layer::Binding, forged, bDim, neg, bNext),
            std::unexpected{ViewError::StaleEpoch});
  EXPECT_EQ(space.target(forged), std::nullopt);

  // The refused writes changed nothing in either arena.
  EXPECT_EQ(space.linked(bound, bDim, pos), bNext);
  EXPECT_EQ(space.linked(derived, dDim, pos), std::nullopt);

  // The derived generation goes; the binding one, with the same numbers,
  // stays.
  space.toss();
  EXPECT_EQ(space.target(derived), std::nullopt);
  EXPECT_EQ(space.resolveReal(derived), std::unexpected{ViewError::StaleEpoch});
  EXPECT_EQ(space.target(bound), slice.cells[0]);
  EXPECT_EQ(space.linked(bound, bDim, pos), bNext);

  // And the next derived cell to take that number is still not the binding
  // one.
  const auto again = occurrence(space, Layer::Derived, slice.cells[1]);
  EXPECT_EQ(again.ref, bound.ref);
  EXPECT_EQ(space.target(again), slice.cells[1]);
  EXPECT_EQ(space.target(bound), slice.cells[0]);
  expectSound(space);
}

// -- I1: one neighbour per direction -----------------------------------------

TEST(ViewManifoldTest, aLinkRefusesToDisplaceAnOccupantAtEitherEnd) {
  RealSlice slice;
  const auto base = slice.manifold();
  ViewManifold space{base};
  const auto dim = mint(space, Layer::Derived);
  const auto a   = mint(space, Layer::Derived);
  const auto b   = mint(space, Layer::Derived);
  const auto c   = mint(space, Layer::Derived);

  ASSERT_TRUE(space.link(Layer::Derived, a, dim, pos, b));

  // a already has a posward neighbour, and b a negward one.
  EXPECT_EQ(space.link(Layer::Derived, a, dim, pos, c),
            std::unexpected{ViewError::OccupiedDirection});
  EXPECT_EQ(space.link(Layer::Derived, c, dim, pos, b),
            std::unexpected{ViewError::OccupiedDirection});
  EXPECT_EQ(space.link(Layer::Derived, b, dim, neg, c),
            std::unexpected{ViewError::OccupiedDirection});
  expectSound(space);
  EXPECT_EQ(space.linked(a, dim, pos), b);
  EXPECT_EQ(space.linked(b, dim, neg), a);
  EXPECT_EQ(space.linked(c, dim, pos), std::nullopt);
  EXPECT_EQ(space.linked(c, dim, neg), std::nullopt);

  // The same edge again displaces nothing, so it is not refused.
  EXPECT_TRUE(space.link(Layer::Derived, a, dim, pos, b));
  EXPECT_TRUE(space.link(Layer::Derived, b, dim, neg, a));

  // Another dimension is another pair of slots.
  const auto other = mint(space, Layer::Derived);
  EXPECT_TRUE(space.link(Layer::Derived, a, other, pos, c));
  expectSound(space);
}

TEST(ViewManifoldTest, unlinkClearsBothEndsAndNothingToClearWritesNothing) {
  RealSlice slice;
  const auto base = slice.manifold();
  ViewManifold space{base};
  const auto dim = mint(space, Layer::Binding);
  const auto a   = mint(space, Layer::Binding);
  const auto b   = mint(space, Layer::Binding);
  ASSERT_TRUE(space.link(Layer::Binding, a, dim, pos, b));

  ASSERT_TRUE(space.unlink(Layer::Binding, b, dim, neg));
  expectSound(space);
  EXPECT_EQ(space.linked(a, dim, pos), std::nullopt);
  EXPECT_EQ(space.linked(b, dim, neg), std::nullopt);

  EXPECT_TRUE(space.unlink(Layer::Binding, a, dim, pos));
  // Freed, so the slot takes a new occupant.
  EXPECT_TRUE(space.link(Layer::Binding, b, dim, pos, a));
  expectSound(space);
}

// -- I2: a view cell never reaches a store -----------------------------------

TEST(ViewManifoldTest, aViewCellNeverReachesAStore) {
  RealSlice slice;
  const auto base = slice.manifold();
  ViewManifold space{base};

  for (const auto layer : {Layer::Binding, Layer::Derived}) {
    const auto cell = occurrence(space, layer, slice.cells[0]);
    EXPECT_THROW(std::ignore = slice.store.setLink(slice.at, slice.cells[0],
                                                   slice.dim, pos, cell.ref),
                 std::invalid_argument);
    EXPECT_THROW(std::ignore = slice.store.setLink(
                     slice.at, cell.ref, slice.dim, pos, slice.cells[0]),
                 std::invalid_argument);
  }
}

// -- I3: no real cell is shadowed --------------------------------------------

TEST(ViewManifoldTest, aRealCellIsNeitherAnEndNorAKey) {
  RealSlice slice;
  const auto base = slice.manifold();
  ViewManifold space{base};

  for (const auto layer : {Layer::Binding, Layer::Derived}) {
    const auto dim  = mint(space, layer);
    const auto cell = mint(space, layer);
    const ViewCellRef real{
        .ref = slice.cells[1], .epoch = cell.epoch, .layer = layer};
    const ViewCellRef realDim{
        .ref = slice.dim, .epoch = cell.epoch, .layer = layer};
    EXPECT_EQ(space.link(layer, cell, dim, pos, real),
              std::unexpected{ViewError::RealCellInViewLink});
    EXPECT_EQ(space.link(layer, real, dim, pos, cell),
              std::unexpected{ViewError::RealCellInViewLink});
    EXPECT_EQ(space.link(layer, cell, realDim, pos, cell),
              std::unexpected{ViewError::RealCellInViewLink});
    EXPECT_EQ(space.unlink(layer, real, dim, pos),
              std::unexpected{ViewError::RealCellInViewLink});
    // Reads of a real cell through the view space find nothing either: real
    // structure is read from base(), never merged in.
    EXPECT_EQ(space.linked(real, realDim, pos), std::nullopt);
    EXPECT_EQ(space.target(real), std::nullopt);
  }
  expectSound(space);
  // The real rank is exactly as the store left it.
  EXPECT_EQ(space.base().linked(slice.cells[0], slice.dim, pos),
            slice.cells[1]);
}

// -- occurrences --------------------------------------------------------------

TEST(ViewManifoldTest, anOccurrenceHoldsItsTargetsOwnName) {
  RealSlice slice;
  const auto base = slice.manifold();
  ViewManifold space{base};

  const auto one = occurrence(space, Layer::Derived, slice.cells[1]);
  const auto two = occurrence(space, Layer::Derived, slice.cells[1]);
  EXPECT_NE(one, two);
  EXPECT_EQ(space.target(one), space.target(two));
  expectSound(space);
  // Standing for a cell is a value, not a link: the real cell gained nothing.
  EXPECT_EQ(space.base().linked(slice.cells[1], slice.dim, neg),
            slice.cells[0]);
}

TEST(ViewManifoldTest, anOccurrenceOfWhatIsNotThereIsRefused) {
  RealSlice slice;
  const auto base = slice.manifold();
  ViewManifold space{base};
  const auto group = mint(space, Layer::Binding);

  // Not a cell of the base.
  constexpr zigzag::CellRef nowhere = 0x7fff'0000U;
  EXPECT_EQ(space.mintOccurrence(Layer::Derived, nowhere),
            std::unexpected{ViewError::UnknownTarget});
  EXPECT_EQ(space.mintOccurrence(Layer::Derived, zigzag::noCell),
            std::unexpected{ViewError::UnknownTarget});
  // A binding cell may be the target of a binding occurrence (a group)...
  const auto ofGroup = occurrence(space, Layer::Binding, group.ref);
  EXPECT_EQ(space.target(ofGroup), group.ref);
  // ...but not of a derived one, whose handle could not say which arena.
  EXPECT_EQ(space.mintOccurrence(Layer::Derived, group.ref),
            std::unexpected{ViewError::UnknownTarget});
  // And not one past the binding arena's end.
  EXPECT_EQ(
      space.mintOccurrence(Layer::Binding, zigzag::ArenaManifold::refOf(1000U)),
      std::unexpected{ViewError::UnknownTarget});
  expectSound(space);
}

// -- I4: the toss
// --------------------------------------------------------------

class ViewManifoldTossTest : public ::testing::TestWithParam<std::size_t> {};

TEST_P(ViewManifoldTossTest, aTossTruncatesWhateverWasMinted) {
  const std::size_t minted = GetParam();
  RealSlice slice;
  const auto base = slice.manifold();
  ViewManifold space{base};
  const auto bound = occurrence(space, Layer::Binding, slice.cells[0]);

  const auto empty = space.derivedCellCount();
  const auto step  = space.axisStepDim(0);
  std::vector<ViewCellRef> kept;
  std::optional<ViewCellRef> previous;
  for (std::size_t i = 0; i < minted; ++i) {
    const auto cell =
        occurrence(space, Layer::Derived, slice.cells[i % slice.cells.size()]);
    if (previous.has_value()) {
      ASSERT_TRUE(space.link(Layer::Derived, *previous, step, pos, cell));
    }
    previous = cell;
    if (0 == i || i + 1 == minted || i == minted / 2) {
      kept.push_back(cell);
    }
  }

  // The preconditions that make release() loop-free are the proof (§6.5):
  // no shadow, no trail entry, no attached space.
  ASSERT_EQ(space.shadowCount(Layer::Derived), 0U);
  ASSERT_EQ(space.trailSize(Layer::Derived), 0U);
  ASSERT_EQ(space.spaceCount(Layer::Derived), 0U);
  ASSERT_EQ(space.spaceCount(Layer::Binding), 0U);
  if (minted < 4'096) {
    expectSound(space);
  }

  const auto before = space.epoch();
  space.toss();
  EXPECT_EQ(space.epoch(), before + 1);
  EXPECT_EQ(space.derivedCellCount(), empty); // the step dimension, too
  kept.push_back(step);
  for (const auto &cell : kept) {
    EXPECT_FALSE(space.isCurrent(cell));
    EXPECT_EQ(space.target(cell), std::nullopt);
    EXPECT_EQ(space.resolveReal(cell), std::unexpected{ViewError::StaleEpoch});
    EXPECT_EQ(space.linked(cell, step, pos), std::nullopt);
    EXPECT_EQ(space.link(Layer::Derived, cell, step, pos, cell),
              std::unexpected{ViewError::StaleEpoch});
  }
  EXPECT_EQ(space.target(bound), slice.cells[0]);
  expectSound(space);

  // The next generation reuses the numbers; the dimension is minted afresh.
  const auto fresh = space.axisStepDim(0);
  EXPECT_NE(fresh, step);
  EXPECT_TRUE(space.isCurrent(fresh));
}

INSTANTIATE_TEST_SUITE_P(Sizes, ViewManifoldTossTest,
                         ::testing::Values(0U, 1'000U, 1'000'000U));

TEST(ViewManifoldTest, aTossLeavesTheBindingArenaAlone) {
  RealSlice slice;
  const auto base = slice.manifold();
  ViewManifold space{base};
  const auto dim  = mint(space, Layer::Binding);
  const auto axis = occurrence(space, Layer::Binding, slice.dim);
  const auto next = occurrence(space, Layer::Binding, slice.cells[0]);
  ASSERT_TRUE(space.link(Layer::Binding, axis, dim, pos, next));
  const auto cells = space.cellCount(Layer::Binding);

  for (int i = 0; i < 3; ++i) {
    std::ignore = occurrence(space, Layer::Derived, slice.cells[2]);
    space.toss();
  }
  EXPECT_EQ(space.cellCount(Layer::Binding), cells);
  EXPECT_TRUE(space.isCurrent(axis));
  EXPECT_EQ(space.linked(axis, dim, pos), next);
  EXPECT_EQ(space.resolveReal(next), slice.cells[0]);
  expectSound(space);
}

// -- I5: every view cell resolves to a real cell
// -------------------------------

TEST(ViewManifoldTest, aPackResolvesToItsFirstPresentConstituent) {
  RealSlice slice;
  const auto base = slice.manifold();
  ViewManifold space{base};
  const auto pack    = space.packDim();
  const auto packing = space.packingDim();
  const auto link    = [&](ViewCellRef from, ViewCellRef dim, ViewCellRef to) {
    ASSERT_TRUE(space.link(Layer::Derived, from, dim, pos, to));
  };

  // outer -> [inner -> [], second, third]; inner is a pack with nothing in it,
  // so the first constituent that resolves is second.
  const auto outer  = mint(space, Layer::Derived);
  const auto inner  = mint(space, Layer::Derived);
  const auto second = occurrence(space, Layer::Derived, slice.cells[1]);
  const auto third  = occurrence(space, Layer::Derived, slice.cells[2]);
  link(outer, pack, inner);
  link(inner, packing, second);
  link(second, packing, third);
  expectSound(space);

  EXPECT_EQ(space.resolveReal(outer), slice.cells[1]);
  EXPECT_EQ(space.resolveReal(third), slice.cells[2]);
  EXPECT_EQ(space.resolveReal(inner),
            std::unexpected{ViewError::UnknownTarget});

  // One level of nesting is one more step: give inner a constituent.
  const auto first = occurrence(space, Layer::Derived, slice.cells[0]);
  link(inner, pack, first);
  expectSound(space);
  EXPECT_EQ(space.resolveReal(inner), slice.cells[0]);
  EXPECT_EQ(space.resolveReal(outer), slice.cells[0]);

  // A dimension cell is no occurrence and no pack.
  EXPECT_EQ(space.resolveReal(pack), std::unexpected{ViewError::UnknownTarget});
}

TEST(ViewManifoldTest, aPackThatLoopsStopsInsteadOfSpinning) {
  RealSlice slice;
  const auto base = slice.manifold();
  ViewManifold space{base};
  const auto pack    = space.packDim();
  const auto packing = space.packingDim();
  const auto a       = mint(space, Layer::Derived);
  const auto b       = mint(space, Layer::Derived);
  ASSERT_TRUE(space.link(Layer::Derived, a, pack, pos, b));
  ASSERT_TRUE(space.link(Layer::Derived, b, packing, pos, a));
  ASSERT_TRUE(space.link(Layer::Derived, a, packing, pos, b));

  EXPECT_EQ(space.resolveReal(a), std::unexpected{ViewError::UnknownTarget});
  // A loop on d.packing is the derivation's bug, and the verifier says so.
  EXPECT_NE(violationsOf(space).find("rank-shape"), std::string::npos);
}

// -- I6, as far as the view space goes ----------------------------------------

TEST(ViewManifoldTest, aRealCellOutlivesTheOccurrenceThatShowedIt) {
  RealSlice slice;
  const auto base = slice.manifold();
  ViewManifold space{base};

  // A cursor keeps real refs only (§8.5). Whatever a toss does to the cells
  // that showed one, re-deriving from the same real ref finds the same cell.
  for (const auto real : slice.cells) {
    const auto shown = occurrence(space, Layer::Derived, real);
    const auto kept  = space.resolveReal(shown);
    ASSERT_TRUE(kept.has_value());
    space.toss();
    EXPECT_FALSE(space.resolveReal(shown).has_value());
    EXPECT_EQ(space.resolveReal(occurrence(space, Layer::Derived, *kept)),
              real);
  }
  expectSound(space);
}

// -- the verifier finds what it is there for
// -----------------------------------

TEST(ViewManifoldTest, theVerifierReportsAShadowAndTheLinkThatMadeIt) {
  RealSlice slice;
  const auto base = slice.manifold();
  ViewManifold space{base};
  const auto dim  = mint(space, Layer::Derived);
  const auto cell = mint(space, Layer::Derived);
  expectSound(space);

  // What link() refuses, done behind its back.
  ASSERT_TRUE(space.arenaForTesting(Layer::Derived)
                  .link(cell.ref, dim.ref, pos, slice.cells[0]));
  EXPECT_EQ(space.shadowCount(Layer::Derived), 1U);
  const auto found = violationsOf(space);
  EXPECT_NE(found.find("I3"), std::string::npos) << found;
  EXPECT_NE(found.find("foreign-end"), std::string::npos) << found;
}

TEST(ViewManifoldTest, theVerifierReportsAnOccurrenceOfNothing) {
  RealSlice slice;
  const auto base = slice.manifold();
  ViewManifold space{base};
  const auto cell = mint(space, Layer::Derived);
  ASSERT_TRUE(
      space.arenaForTesting(Layer::Derived)
          .setValueBits(cell.ref, xanadu::ValueKind::OpHandle, 0x7fff'0000U));
  EXPECT_NE(violationsOf(space).find("occurrence-target"), std::string::npos);
}

TEST(ViewManifoldTest, theVerifierReportsATrailOnTheDerivedArena) {
  RealSlice slice;
  const auto base = slice.manifold();
  ViewManifold space{base};
  const auto dim = mint(space, Layer::Derived);
  const auto a   = mint(space, Layer::Derived);
  const auto b   = mint(space, Layer::Derived);

  ASSERT_TRUE(space.link(Layer::Derived, a, dim, pos, b));
  expectSound(space);

  // A second mark makes a's link run older than the innermost one, so
  // clearing it trails a, and the toss would replay that trail.
  auto &arena     = space.arenaForTesting(Layer::Derived);
  const auto mark = arena.mark();
  ASSERT_TRUE(arena.unlink(a.ref, dim.ref, pos));
  EXPECT_GT(space.trailSize(Layer::Derived), 0U);
  EXPECT_NE(violationsOf(space).find("I4"), std::string::npos);
  arena.discard(mark);
}

} // namespace
