#include <chrono>
#include <gtest/gtest.h>
#include <vector>

#include "common/xanadu/ops.hpp"
#include "common/xanadu/view/transductive_view.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/dim_vector.hpp"

namespace xanadu::view {
namespace {

TEST(TransductiveViewTest, FilterTenThousandCellsZeroShadowing) {
  zigzag::ArenaManifold base;
  const auto dSeq = base.ensureDimension("d.seq");

  // Create a 10,000 cell rank on base
  std::vector<zigzag::CellRef> cells;
  cells.reserve(10000);
  for (std::size_t i = 0; i < 10000; ++i) {
    const auto c = base.makeCell();
    zigzag::expectWritten(base.setValueBits(c, xanadu::ValueKind::Int64,
                                            static_cast<std::uint64_t>(i)));
    if (!cells.empty()) {
      zigzag::expectWritten(
          base.link(cells.back(), dSeq, zigzag::DimVector::POS, c));
    }
    cells.push_back(c);
  }

  ViewManifold viewManifold(base);
  TransductiveView transView(viewManifold);

  // Filter for even numbers >= 5000 (2,500 matches)
  auto start = std::chrono::steady_clock::now();
  const auto filteredHead =
      transView.materializeFilter(cells.front(), dSeq, [&](zigzag::CellRef c) {
        const auto val = base.asInt64(c).value_or(0);
        return val >= 5000 && (val % 2 == 0);
      });
  auto elapsed = std::chrono::steady_clock::now() - start;
  (void)elapsed;

  ASSERT_TRUE(filteredHead.has_value());
  EXPECT_TRUE(viewManifold.verifyViewSpace());
  EXPECT_EQ(viewManifold.shadowCount(Layer::Binding), 0U);
  EXPECT_EQ(viewManifold.shadowCount(Layer::Derived), 0U);

  // Walk the derived rank and verify matches
  std::size_t count = 0;
  ViewCellRef curr  = *filteredHead;
  const auto dFiltered =
      ViewCellRef{.ref   = viewManifold.derivedArena().refOf(0),
                  .epoch = viewManifold.epoch()};

  std::int64_t expectedVal = 5000;
  while (curr.valid()) {
    const auto real = viewManifold.resolveReal(curr);
    ASSERT_TRUE(real.has_value());
    EXPECT_EQ(base.asInt64(*real).value_or(0), expectedVal);
    expectedVal += 2;
    count++;

    const auto next = viewManifold.derivedArena().linked(
        *curr.ref, viewManifold.derivedArena().refOf(0),
        zigzag::DimVector::POS);
    if (!next.has_value()) {
      break;
    }
    curr = ViewCellRef{.ref = *next, .epoch = viewManifold.epoch()};
  }
  EXPECT_EQ(count, 2500U);

  // Verify O(1) toss
  viewManifold.toss();
  EXPECT_EQ(viewManifold.epoch(), 2U);
  // Stale epoch access fails
  EXPECT_FALSE(viewManifold
                   .linked(Layer::Derived, *filteredHead, dFiltered,
                           zigzag::DimVector::POS)
                   .has_value());
}

TEST(TransductiveViewTest, SortedMaterialization) {
  zigzag::ArenaManifold base;
  const auto dSeq = base.ensureDimension("d.seq");

  std::vector<std::int64_t> values = {42, 10, 88, 5, 23, 100, 1};
  std::vector<zigzag::CellRef> cells;
  for (const auto val : values) {
    const auto c = base.makeCell();
    zigzag::expectWritten(base.setValueBits(c, xanadu::ValueKind::Int64,
                                            static_cast<std::uint64_t>(val)));
    if (!cells.empty()) {
      zigzag::expectWritten(
          base.link(cells.back(), dSeq, zigzag::DimVector::POS, c));
    }
    cells.push_back(c);
  }

  ViewManifold viewManifold(base);
  TransductiveView transView(viewManifold);

  const auto sortedHead = transView.materializeSorted(
      cells.front(), dSeq, [&](zigzag::CellRef a, zigzag::CellRef b) -> bool {
        return base.asInt64(a).value_or(0) < base.asInt64(b).value_or(0);
      });

  ASSERT_TRUE(sortedHead.has_value());

  // Check that the resolved sequence is strictly sorted: 1, 5, 10, 23, 42, 88,
  // 100
  std::vector<std::int64_t> resolved;
  ViewCellRef curr = *sortedHead;
  while (curr.valid()) {
    const auto real = viewManifold.resolveReal(curr);
    ASSERT_TRUE(real.has_value());
    resolved.push_back(base.asInt64(*real).value_or(0));
    const auto next = viewManifold.derivedArena().linked(
        *curr.ref, viewManifold.derivedArena().refOf(0),
        zigzag::DimVector::POS);
    if (!next.has_value()) {
      break;
    }
    curr = ViewCellRef{.ref = *next, .epoch = viewManifold.epoch()};
  }

  const std::vector<std::int64_t> expected = {1, 5, 10, 23, 42, 88, 100};
  EXPECT_EQ(resolved, expected);
}

TEST(TransductiveViewTest, CycleDetectionInMalformedUnionRank) {
  zigzag::ArenaManifold base;
  const auto dSeq = base.ensureDimension("d.seq");

  // Create a cyclic rank: c1 -> c2 -> c3 -> c1
  const auto c1 = base.makeCell();
  const auto c2 = base.makeCell();
  const auto c3 = base.makeCell();

  zigzag::expectWritten(base.link(c1, dSeq, zigzag::DimVector::POS, c2));
  zigzag::expectWritten(base.link(c2, dSeq, zigzag::DimVector::POS, c3));
  zigzag::expectWritten(
      base.link(c3, dSeq, zigzag::DimVector::POS, c1)); // Cycle!

  ViewManifold viewManifold(base);
  TransductiveView transView(viewManifold);

  std::pair<zigzag::CellRef, zigzag::DimRef> rank1{c1, dSeq};
  const auto unionHead = transView.materializeUnion(
      std::span<const std::pair<zigzag::CellRef, zigzag::DimRef>>(&rank1, 1));

  ASSERT_TRUE(unionHead.has_value());

  // Should have exactly 3 cells without hanging indefinitely
  std::size_t count = 0;
  ViewCellRef curr  = *unionHead;
  while (curr.valid()) {
    count++;
    const auto next = viewManifold.derivedArena().linked(
        *curr.ref, viewManifold.derivedArena().refOf(0),
        zigzag::DimVector::POS);
    if (!next.has_value()) {
      break;
    }
    curr = ViewCellRef{.ref = *next, .epoch = viewManifold.epoch()};
  }
  EXPECT_EQ(count, 3U);
}

} // namespace
} // namespace xanadu::view
