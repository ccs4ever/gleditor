/**
 * @file rank_walk_test.cpp
 * @brief Unit tests for the rank views and optional-returning steps in
 *        cell_views.hpp, and the expected-returning fold and arena writes.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <ranges>
#include <vector>

#include "common/xanadu/store.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"
#include "common/xanadu/zigzag/manifold.hpp"
#include "common/xanadu/zigzag/vlog.hpp"

namespace {

using namespace zigzag;

/// Three cells on d.1, first -> second -> third.
struct Line {
  ArenaManifold arena;
  DimRef d1{arena.makeCell("d.1")};
  CellRef c1{arena.makeCell("first")};
  CellRef c2{arena.makeCell("second")};
  CellRef c3{arena.makeCell("third")};

  Line() {
    EXPECT_TRUE(arena.link(c1, d1, DimVector::POS, c2));
    EXPECT_TRUE(arena.link(c2, d1, DimVector::POS, c3));
  }
};

TEST(RankWalkTest, ArenaManifoldLinearForwardWalk) {
  const Line line;
  const auto visited =
      rank(line.arena, line.c1, line.d1) | std::ranges::to<std::vector>();
  EXPECT_EQ(visited, (std::vector{line.c1, line.c2, line.c3}));

  // The step index the old callback walk handed out is views::enumerate.
  const auto steps = rank(line.arena, line.c1, line.d1) |
                     std::views::enumerate |
                     std::views::transform([](const auto indexed) {
                       return static_cast<std::size_t>(std::get<0>(indexed));
                     }) |
                     std::ranges::to<std::vector>();
  EXPECT_EQ(steps, (std::vector<std::size_t>{0, 1, 2}));
}

TEST(RankWalkTest, ArenaManifoldLinearBackwardWalk) {
  const Line line;
  const auto visited = rank(line.arena, line.c3, line.d1, DimVector::NEG) |
                       std::ranges::to<std::vector>();
  EXPECT_EQ(visited, (std::vector{line.c3, line.c2, line.c1}));
}

TEST(RankWalkTest, ArenaManifoldEarlyTermination) {
  const Line line;
  const auto visited =
      rank(line.arena, line.c1, line.d1) |
      std::views::take_while([&](const CellRef c) { return c != line.c3; }) |
      std::ranges::to<std::vector>();
  EXPECT_EQ(visited, (std::vector{line.c1, line.c2}));
}

TEST(RankWalkTest, ArenaManifoldCycleProtection) {
  Line line;
  // Close the ring: c3 -> c1.
  ASSERT_TRUE(line.arena.link(line.c3, line.d1, DimVector::POS, line.c1));

  // Stops when the next cell is the start, visiting each cell once.
  EXPECT_EQ(rank(line.arena, line.c1, line.d1) | std::ranges::to<std::vector>(),
            (std::vector{line.c1, line.c2, line.c3}));

  // rankAfter drops the start and still stops there.
  EXPECT_EQ(rankAfter(line.arena, line.c1, line.d1) |
                std::ranges::to<std::vector>(),
            (std::vector{line.c2, line.c3}));

  // On a ring the tail is the cell before the start, and the end is the start.
  EXPECT_EQ(rankTail(line.arena, line.c1, line.d1), line.c3);
  EXPECT_EQ(rankEnd(line.arena, line.c1, line.d1), line.c1);
}

TEST(RankWalkTest, SelfLinkStopsTheWalk) {
  Line line;
  // A link keeps both its ends, so a self-link on c3 evicts c2 -> c3: c3 is
  // a rank of one that loops on itself, and the walk visits it once.
  ASSERT_TRUE(line.arena.link(line.c3, line.d1, DimVector::POS, line.c3));
  EXPECT_EQ(rank(line.arena, line.c3, line.d1) | std::ranges::to<std::vector>(),
            (std::vector{line.c3}));
  EXPECT_EQ(rank(line.arena, line.c1, line.d1) | std::ranges::to<std::vector>(),
            (std::vector{line.c1, line.c2}));
}

TEST(RankWalkTest, ARingWalkedFromItsMiddleVisitsEachCellOnce) {
  Line line;
  ASSERT_TRUE(line.arena.link(line.c3, line.d1, DimVector::POS, line.c1));
  // Every cell of a consistent ring is on it once, from wherever the walk
  // starts. (A lasso cannot be built: a link keeps both of its ends, so
  // closing c3 -> c2 would evict c1 -> c2. traversalBound() bounds only a
  // structure that is already inconsistent.)
  EXPECT_EQ(rank(line.arena, line.c2, line.d1) | std::ranges::to<std::vector>(),
            (std::vector{line.c2, line.c3, line.c1}));
  EXPECT_EQ(rank(line.arena, line.c2, line.d1, DimVector::NEG) |
                std::ranges::to<std::vector>(),
            (std::vector{line.c2, line.c1, line.c3}));
}

TEST(RankWalkTest, AbsentStartOrDimensionIsAnEmptyRank) {
  const Line line;
  EXPECT_TRUE(std::ranges::empty(rank(line.arena, noCell, line.d1)));
  EXPECT_TRUE(std::ranges::empty(rank(line.arena, line.c1, noCell)));
  EXPECT_EQ(rankTail(line.arena, line.c1, noCell), line.c1);
}

TEST(RankWalkTest, StepAnswersOptional) {
  const Line line;
  EXPECT_EQ(step(line.arena, line.c1, line.d1), line.c2);
  EXPECT_EQ(step(line.arena, line.c3, line.d1), std::nullopt);
  EXPECT_EQ(step(line.arena, line.c1, line.d1)
                .and_then(hop(line.arena, line.d1))
                .and_then(hop(line.arena, line.d1)),
            std::nullopt);
  EXPECT_EQ(
      step(line.arena, line.c1, line.d1).and_then(hop(line.arena, line.d1)),
      line.c3);
}

TEST(RankWalkTest, NeighboursAndFilters) {
  Line line;
  const DimRef d2   = line.arena.makeCell("d.2");
  const CellRef off = line.arena.makeCell("off");
  ASSERT_TRUE(line.arena.link(line.c2, d2, DimVector::POS, off));

  EXPECT_EQ(neighbours(line.arena, line.c2, DimVector::POS) |
                std::ranges::to<std::vector>(),
            (std::vector{Hop{.dim = line.d1, .cell = line.c3},
                         Hop{.dim = d2, .cell = off}}));

  const auto all =
      neighbours(line.arena, line.c2) | std::ranges::to<std::vector>();
  EXPECT_EQ(all.size(), 3U);
  EXPECT_TRUE(std::ranges::contains(all, Hop{.dim = line.d1, .cell = line.c1}));

  EXPECT_EQ(rank(line.arena, line.c1, line.d1) |
                std::views::filter(linksAlong(line.arena, d2)) |
                std::ranges::to<std::vector>(),
            (std::vector{line.c2}));

  const auto named = [&](const std::string_view text) {
    return firstOf(rank(line.arena, line.c1, line.d1) |
                   std::views::filter([&, text](const CellRef c) {
                     return line.arena.textOf(c) == text;
                   }));
  };
  EXPECT_EQ(named("third"), line.c3);
  EXPECT_EQ(named("absent"), std::nullopt);
}

TEST(RankWalkTest, SlotProvidesMonadicBorrowedAccess) {
  Line line;
  const auto s1 = line.arena.slot(line.c1);
  ASSERT_TRUE(s1.has_value());
  EXPECT_EQ(s1->birthOp, line.c1);

  EXPECT_FALSE(line.arena.slot(noCell).has_value());
  EXPECT_FALSE(line.arena.slot(line.c3 + 9999).has_value());

  const auto directFind = line.arena.slot(line.c2);
  ASSERT_TRUE(directFind.has_value());
  EXPECT_EQ(directFind->birthOp, line.c2);

  const auto birth =
      line.arena.slot(line.c1).transform(&CellSlot::birthOp).value_or(noCell);
  EXPECT_EQ(birth, line.c1);
}

TEST(RankWalkTest, CopyOnWriteArenaWalksTheWholeBase) {
  xanadu::Store store;
  auto at = store.sliceGenesis(xanadu::MicroversionId{});
  std::vector<CellRef> cells;
  for (const auto *name : {"a", "b", "c", "d", "e"}) {
    at = store.makeCell(at, name);
    cells.push_back(store.cellRefOf(at));
  }
  const auto dSeq = store.rebuildManifold(at).dimensions().front();
  for (const auto [from, to] : cells | std::views::adjacent<2>) {
    at = store.setLink(at, from, dSeq, DimVector::POS, to);
  }
  const auto base = store.rebuildManifold(at);

  // An arena over the base with no cells of its own. Bounding the walk by the
  // arena's own cellCount() cut it off after one cell.
  const ArenaManifold view{&base};
  EXPECT_EQ(rank(view, cells.front(), dSeq) | std::ranges::to<std::vector>(),
            cells);
}

TEST(RankWalkTest, ManifoldRankWalk) {
  xanadu::Store store;
  auto at = store.sliceGenesis(xanadu::MicroversionId{});

  at            = store.makeCell(at, "cell_A");
  const auto cA = store.cellRefOf(at);

  at            = store.makeCell(at, "cell_B");
  const auto cB = store.cellRefOf(at);

  at            = store.makeCell(at, "cell_C");
  const auto cC = store.cellRefOf(at);

  const auto dSeq = store.rebuildManifold(at).dimensions().front();

  at = store.setLink(at, cA, dSeq, DimVector::POS, cB);
  at = store.setLink(at, cB, dSeq, DimVector::POS, cC);

  const auto manifold = store.rebuildManifold(at);
  EXPECT_EQ(rank(manifold, cA, dSeq) | std::ranges::to<std::vector>(),
            (std::vector{cA, cB, cC}));
  EXPECT_EQ(manifold.dimensionNamed("absent", store), std::nullopt);
  EXPECT_EQ(manifold.cloneMaster(cC + 1000, dSeq), std::nullopt);
}

// Not a pass/fail on speed -- timing assertions are flaky under load -- but
// the measurement behind "a RankView costs what the loop it replaced cost":
// the same linked() calls, through an iterator instead of a while loop.
TEST(RankWalkTest, aRankViewCostsWhatTheHandWrittenLoopCost) {
  constexpr std::size_t kCells  = 100'000;
  constexpr std::size_t kPasses = 20;
  ArenaManifold arena;
  const DimRef d1 = arena.makeCell("d.1");
  std::vector<CellRef> cells(kCells);
  std::ranges::generate(cells, [&] { return arena.makeCell(); });
  for (const auto [from, to] : cells | std::views::adjacent<2>) {
    ASSERT_TRUE(arena.link(from, d1, DimVector::POS, to));
  }

  const auto time = [&](const auto &walk) {
    CellRef sink     = 0;
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t pass = 0; pass < kPasses; ++pass) {
      sink ^= walk();
    }
    const auto elapsed = std::chrono::duration<double, std::nano>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    EXPECT_NE(sink, 0xFFFFFFFFU) << "the walk must not be optimised away";
    return elapsed / static_cast<double>(kCells * kPasses);
  };

  const double loop   = time([&] {
    CellRef acc = 0;
    CellRef cur = cells.front();
    for (std::size_t limit                 = arena.traversalBound() + 1;
         cur != noCell && limit-- > 0; cur = arena.linked(cur, d1)) {
      acc ^= cur;
    }
    return acc;
  });
  const double view   = time([&] {
    CellRef acc = 0;
    for (const auto cell : rank(arena, cells.front(), d1)) {
      acc ^= cell;
    }
    return acc;
  });
  const double folded = time([&] {
    return std::ranges::fold_left(rank(arena, cells.front(), d1), CellRef{0},
                                  std::bit_xor<>{});
  });
  std::printf("rank walk, %zu cells, ns/cell: while-loop %.3f  RankView %.3f  "
              "fold_left %.3f\n",
              kCells, loop, view, folded);
}

TEST(FoldResultTest, ApplyStructureNamesItsRefusal) {
  Manifold manifold;
  xanadu::CompactOpNode node{};
  node.kind = xanadu::OpKind::Structure;

  // A SetLink whose subject chain reaches no cell.
  node.flags =
      xanadu::structureFlags(xanadu::StructureVerb::SetLink, DimVector::POS);
  node.sourceOpIndex = 77;
  const auto refused = manifold.applyStructure(100, node);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error(), FoldRefusal::UnknownSubject);
  EXPECT_EQ(manifold.refusedOps(), 1U);

  // Anything that is not Structure is not refused.
  node.kind = xanadu::OpKind::Insert;
  EXPECT_TRUE(manifold.applyStructure(101, node));
  EXPECT_EQ(manifold.refusedOps(), 1U);
}

TEST(FoldResultTest, AdvanceReportsUnknownVersion) {
  xanadu::Store store;
  const auto at = store.sliceGenesis(xanadu::MicroversionId{});
  auto manifold = store.rebuildManifold(at);
  const auto result =
      manifold.advance(store, xanadu::MicroversionId::parse("999"));
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, AdvanceError::Kind::UnknownVersion);
}

TEST(ArenaResultTest, WritesNameTheRefUsedWrongly) {
  Line line;
  const CellRef stranger = line.c3 + 1000;
  EXPECT_EQ(line.arena.link(stranger, line.d1, DimVector::POS, line.c1).error(),
            ArenaRefusal::UnknownCell);
  EXPECT_EQ(line.arena.link(line.c1, stranger, DimVector::POS, line.c2).error(),
            ArenaRefusal::UnknownDimension);
  EXPECT_EQ(line.arena.link(line.c1, line.d1, DimVector::POS, stranger).error(),
            ArenaRefusal::UnknownTarget);

  const auto mark = line.arena.mark();
  EXPECT_EQ(line.arena.compact().error(), ArenaRefusal::MarksOutstanding);
  line.arena.discard(mark);
  EXPECT_TRUE(line.arena.compact());
}

TEST(UnifyResultTest, FailureSaysWhichRuleFailed) {
  ArenaManifold arena;
  auto vlog = Vlog::over(arena);

  EXPECT_EQ(vlog.unify(arena.makeScalarCell(std::int64_t{1}),
                       arena.makeScalarCell(std::int64_t{2}))
                .error(),
            UnifyFailure::ValueMismatch);
  EXPECT_EQ(vlog.unify(arena.makeCell("foo"), arena.makeCell("bar")).error(),
            UnifyFailure::FunctorMismatch);
  EXPECT_EQ(
      vlog.unify(vlog.makeTerm("f", {arena.makeCell("a")}),
                 vlog.makeTerm("f", {arena.makeCell("a"), arena.makeCell("b")}))
          .error(),
      UnifyFailure::ArityMismatch);
  EXPECT_TRUE(vlog.unify(vlog.makeVar(), arena.makeCell("x")));
}

} // namespace
