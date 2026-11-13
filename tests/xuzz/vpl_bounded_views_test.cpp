/**
 * @file vpl_bounded_views_test.cpp
 * @brief Card V4b: a reversed view ends at a stop cell, and every walker and
 * reader honours it.
 *
 * Two layers. GraphStopTest drives the graph-emit walkers by hand with a stop
 * register, so each walker is proved on the VM on its own. VplBoundedViewsTest
 * runs VPL source and reads the views back.
 */
#include <algorithm>
#include <cstdint>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vpl/compiler.hpp"
#include "common/xanadu/vpl/execution.hpp"
#include "common/xanadu/vpl/graph_emit.hpp"
#include "common/xanadu/vpl/lower_decls.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vpl {
namespace {

using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;
using zigzag::vortex::OpcodeKind;
using zigzag::vortex::VortexCore;
using zigzag::vortex::VortexVM;

constexpr std::size_t kBudget = 200000;

// ---------------------------------------------------------------------------
// The walkers, by hand.
// ---------------------------------------------------------------------------
class GraphStopTest : public ::testing::Test {
protected:
  void SetUp() override {
    dim_   = core_.findOrMintDimension("d.stop-test");
    other_ = core_.findOrMintDimension("d.stop-other");
    entry_ = ctx_.emitOp(OpcodeKind::Nop, "#TEST_ENTRY");
  }

  CellRef counter() {
    const auto cell = arena_.makeCell();
    const auto op   = ctx_.emitOp(OpcodeKind::Add, "#TEST_COUNTER");
    ctx_.emitInputConstant(op, std::int64_t{0});
    ctx_.emitInputConstant(op, std::int64_t{0});
    ctx_.emitOutput(op, cell);
    return cell;
  }
  void add(CellRef to, CellRef amount) {
    const auto op = ctx_.emitOp(OpcodeKind::Add, "#TEST_ADD");
    ctx_.emitInput(op, to);
    ctx_.emitInput(op, amount);
    ctx_.emitOutput(op, to);
  }
  void bump(CellRef to) { add(to, ctx_.emitConstant(std::int64_t{1})); }

  std::vector<CellRef> makeRank(const std::vector<std::int64_t> &values) {
    std::vector<CellRef> cells;
    for (const auto value : values) {
      const auto cell = arena_.makeScalarCell(value);
      if (!cells.empty()) {
        zigzag::expectWritten(arena_.link(cells.back(), dim_, false, cell));
      }
      cells.push_back(cell);
    }
    return cells;
  }

  // A register that holds a reference to `cell`.
  CellRef stopAt(CellRef cell) { return emit::aliasRegister(ctx_, cell); }

  void run() {
    (void)ctx_.emitOp(OpcodeKind::Halt, "#TEST_HALT");
    const auto cursor = vm_.spawnCursor(entry_, "stop-test");
    const auto result = vm_.run(cursor, kBudget);
    ASSERT_TRUE(result.success) << result.errorMessage;
  }

  std::int64_t valueOf(CellRef cell) {
    return arena_.asInt64(cell).value_or(-999);
  }

  zigzag::ArenaManifold arena_;
  VortexCore core_{arena_};
  VortexVM vm_{core_};
  VPLCompiler compiler_{core_, vm_};
  LowerContext ctx_{compiler_};
  DimRef dim_{};
  DimRef other_{};
  CellRef entry_{};
};

TEST_F(GraphStopTest, WalkRankEndsAfterTheStopCellInclusive) {
  const auto cells = makeRank({1, 2, 3, 4, 5});
  struct Case {
    std::size_t anchor;
    std::size_t stop;
    CellRef hits;
    CellRef sum;
  };
  std::vector<Case> cases;
  for (const auto [anchor, stop] :
       {std::pair{0U, 2U}, {0U, 4U}, {1U, 1U}, {3U, 4U}, {0U, 0U}}) {
    const auto reg  = stopAt(cells[stop]);
    const auto hits = counter();
    const auto sum  = counter();
    emit::walkRank(
        ctx_, cells[anchor], dim_, DimVector::POS,
        [&](CellRef item) {
          bump(hits);
          add(sum, item);
        },
        reg);
    cases.push_back({anchor, stop, hits, sum});
  }
  run();
  for (const auto &c : cases) {
    std::int64_t total = 0;
    for (auto i = c.anchor; i <= c.stop; ++i) total += valueOf(cells[i]);
    EXPECT_EQ(valueOf(c.hits), static_cast<std::int64_t>(c.stop - c.anchor + 1))
        << c.anchor << ".." << c.stop;
    EXPECT_EQ(valueOf(c.sum), total) << c.anchor << ".." << c.stop;
  }
}

TEST_F(GraphStopTest, StopIsIdentityNotEqualValue) {
  // Two cells holding 7: stopping at the second must not stop at the first.
  const auto cells = makeRank({7, 7, 7});
  const auto hits  = counter();
  emit::walkRank(
      ctx_, cells[0], dim_, DimVector::POS, [&](CellRef) { bump(hits); },
      stopAt(cells[1]));
  run();
  EXPECT_EQ(valueOf(hits), 2);
}

TEST_F(GraphStopTest, WalkRankAfterSkipsTheAnchorAndHonoursTheStop) {
  const auto cells = makeRank({1, 2, 3, 4});
  const auto some  = counter();
  const auto none  = counter();
  emit::walkRankAfter(
      ctx_, cells[0], dim_, DimVector::POS, [&](CellRef) { bump(some); },
      stopAt(cells[2]));
  // The anchor is the stop: nothing comes after it in the view.
  emit::walkRankAfter(
      ctx_, cells[1], dim_, DimVector::POS, [&](CellRef) { bump(none); },
      stopAt(cells[1]));
  run();
  EXPECT_EQ(valueOf(some), 2);
  EXPECT_EQ(valueOf(none), 0);
}

TEST_F(GraphStopTest, ANegwardWalkEndsAtTheHeadNotThePredecessor) {
  // The V4b case: the rank goes on negward past the view's head.
  const auto cells = makeRank({9, 1, 2, 3});
  const auto hits  = counter();
  const auto sum   = counter();
  emit::walkRank(
      ctx_, cells[3], dim_, DimVector::NEG,
      [&](CellRef item) {
        bump(hits);
        add(sum, item);
      },
      stopAt(cells[1]));
  const auto unbounded = counter();
  emit::walkRank(ctx_, cells[3], dim_, DimVector::NEG,
                 [&](CellRef) { bump(unbounded); });
  run();
  EXPECT_EQ(valueOf(hits), 3);
  EXPECT_EQ(valueOf(sum), 6);
  EXPECT_EQ(valueOf(unbounded), 4);
}

TEST_F(GraphStopTest, ExtremeAndExtentStopAtTheStop) {
  const auto cells = makeRank({1, 2, 3, 4});
  const auto last =
      emit::extreme(ctx_, cells[0], dim_, DimVector::POS, stopAt(cells[2]));
  const auto same =
      emit::extreme(ctx_, cells[1], dim_, DimVector::POS, stopAt(cells[1]));
  const auto three =
      emit::extentOf(ctx_, cells[0], dim_, DimVector::POS, stopAt(cells[2]));
  const auto one =
      emit::extentOf(ctx_, cells[2], dim_, DimVector::POS, stopAt(cells[2]));
  const auto whole = emit::extentOf(ctx_, cells[0], dim_, DimVector::POS);
  run();
  EXPECT_EQ(vm_.resultCell(last), cells[2]);
  EXPECT_EQ(vm_.resultCell(same), cells[1]);
  EXPECT_EQ(valueOf(three), 3);
  EXPECT_EQ(valueOf(one), 1);
  EXPECT_EQ(valueOf(whole), 4);
}

TEST_F(GraphStopTest, LockstepBoundsEachRankByItsOwnStop) {
  const auto a    = makeRank({1, 2, 3, 4});
  const auto b    = makeRank({10, 20, 30, 40});
  const auto hits = counter();
  const auto sum  = counter();
  emit::walkRanksLockstep(
      ctx_, a[3], dim_, DimVector::NEG, b[0], dim_, DimVector::POS,
      [&](CellRef x, CellRef y) {
        bump(hits);
        add(sum, x);
        add(sum, y);
      },
      stopAt(a[1]), std::nullopt);
  run();
  // Three pairs: (4,10) (3,20) (2,30); the second rank is unbounded.
  EXPECT_EQ(valueOf(hits), 3);
  EXPECT_EQ(valueOf(sum), 4 + 3 + 2 + 10 + 20 + 30);
}

TEST_F(GraphStopTest, NestedBoundedWalksRestartTheirStopTest) {
  const auto cells = makeRank({1, 2, 3, 4});
  const auto hits  = counter();
  const auto stop  = stopAt(cells[2]);
  emit::walkRank(
      ctx_, cells[0], dim_, DimVector::POS,
      [&](CellRef) {
        emit::walkRank(
            ctx_, cells[0], dim_, DimVector::POS, [&](CellRef) { bump(hits); },
            stop);
      },
      stop);
  run();
  EXPECT_EQ(valueOf(hits), 9);
}

TEST_F(GraphStopTest, ReduceAndScanOverABoundedNegwardWalk) {
  const auto cells = makeRank({100, 1, 2, 3, 4, 5});
  const auto sum   = counter();
  const auto tail  = arena_.makeScalarCell(std::int64_t{0});
  auto scanTail    = emit::aliasRegister(ctx_, tail);
  const auto stop  = stopAt(cells[1]);
  emit::walkRank(
      ctx_, cells[5], dim_, DimVector::NEG,
      [&](CellRef item) {
        add(sum, item);
        const auto fresh =
            emit::mintAfter(ctx_, scanTail, other_, DimVector::POS);
        emit::copyValue(ctx_, sum, fresh);
      },
      stop);
  run();
  EXPECT_EQ(valueOf(sum), 15);
  std::vector<std::int64_t> scan;
  std::optional<CellRef> at = arena_.linked(tail, other_, DimVector::POS);
  while (at) {
    scan.push_back(valueOf(*at));
    at = arena_.linked(*at, other_, DimVector::POS);
  }
  EXPECT_EQ(scan, (std::vector<std::int64_t>{5, 9, 12, 14, 15}));
}

// ---------------------------------------------------------------------------
// VPL source.
// ---------------------------------------------------------------------------
using Values = std::vector<std::int64_t>;

class VplBoundedViewsTest : public ::testing::Test {
protected:
  void SetUp() override {
    core     = std::make_unique<VortexCore>(arena);
    vm       = std::make_unique<VortexVM>(*core);
    executor = std::make_unique<Executor>(*core, *vm);
  }

  ExecutionResult run(const std::string &source) {
    auto result = executor->execute(source, kBudget);
    EXPECT_TRUE(result.has_value())
        << source << ": " << (result ? "" : result.error().message);
    if (!result) return ExecutionResult{};
    return std::move(*result);
  }

  Values values(const VplView &view) {
    Values out;
    for (const auto cell : view.collectCells(arena)) {
      out.push_back(arena.asInt64(cell).value_or(-999));
    }
    return out;
  }

  std::int64_t shapeOf(const std::string &source) {
    const auto r = run(source);
    EXPECT_EQ(r.view.axes().size(), 1U) << source;
    const auto cells = r.view.collectCells(arena);
    EXPECT_EQ(cells.size(), 1U) << source;
    return cells.empty() ? -1 : arena.asInt64(cells.front()).value_or(-999);
  }

  zigzag::ArenaManifold arena;
  std::unique_ptr<VortexCore> core;
  std::unique_ptr<VortexVM> vm;
  std::unique_ptr<Executor> executor;
};

TEST_F(VplBoundedViewsTest, ShapeOfAReversedIotaIsFiveNotSix) {
  run("A ← 5⍳d.1");
  EXPECT_EQ(shapeOf("⍴A"), 5);
  EXPECT_EQ(shapeOf("⍴⌽A"), 5);
  EXPECT_EQ(shapeOf("⍴⊖A"), 5);
  EXPECT_EQ(shapeOf("⍴⌽⌽A"), 5);
  EXPECT_EQ(run("⌽A").view.shape(arena), std::vector<std::size_t>{5});
}

TEST_F(VplBoundedViewsTest, ReverseYieldsExactlyTheFiveCellsInReverse) {
  const auto a        = run("A ← 5⍳d.1; A");
  const auto original = a.view.collectCells(arena);
  ASSERT_EQ(original.size(), 5U);
  const auto r = run("⌽A");
  auto cells   = r.view.collectCells(arena);
  std::ranges::reverse(cells);
  EXPECT_EQ(cells, original);
  EXPECT_EQ(values(r.view), (Values{5, 4, 3, 2, 1}));
}

TEST_F(VplBoundedViewsTest, ReverseOfReverseViewsExactlyTheOperandsCells) {
  const auto a        = run("A ← 4⍳d.1; A");
  const auto original = a.view.collectCells(arena);
  EXPECT_EQ(run("⌽⌽A").view.collectCells(arena), original);
  EXPECT_EQ(run("⊖⊖A").view.collectCells(arena), original);
  const auto held = run("W ← ⌽A; ⌽W");
  EXPECT_EQ(held.view.collectCells(arena), original);
  EXPECT_EQ(held.view.origin(), a.view.origin());
  // The held reversal also survives a later program.
  EXPECT_EQ(values(run("⌽W").view), (Values{1, 2, 3, 4}));
  EXPECT_EQ(values(run("W").view), (Values{4, 3, 2, 1}));
  EXPECT_EQ(shapeOf("⍴W"), 4);
}

TEST_F(VplBoundedViewsTest, LiteralVectorsAndEmptyArraysStayExact) {
  EXPECT_EQ(values(run("⌽ 10 20 30").view), (Values{30, 20, 10}));
  EXPECT_EQ(values(run("V ← 10 20 30; ⌽⌽V").view), (Values{10, 20, 30}));
  const auto empty = run("E ← 0⍳d.1; ⌽E");
  EXPECT_FALSE(empty.view.origin());
  EXPECT_TRUE(empty.view.collectCells(arena).empty());
  EXPECT_EQ(empty.view.shape(arena), std::vector<std::size_t>{0});
  EXPECT_TRUE(run("E ← 0⍳d.1; ⌽⌽E").view.collectCells(arena).empty());
}

TEST_F(VplBoundedViewsTest, AReversedViewIsASnapshotAndTheOperandStillGrows) {
  run("A ← 5⍳d.1");
  run("W ← ⌽A");
  run("3⍳d.1");
  // A has no stop, so reading it again sees the appended cells; W was bounded
  // when it was made, so it neither grows nor shifts.
  EXPECT_EQ(values(run("A").view), (Values{1, 2, 3, 4, 5, 1, 2, 3}));
  EXPECT_EQ(values(run("W").view), (Values{5, 4, 3, 2, 1}));
  EXPECT_EQ(shapeOf("⍴W"), 5);
  EXPECT_EQ(shapeOf("⍴A"), 8);
  // Reversing afterwards takes the new far end.
  EXPECT_EQ(values(run("⌽A").view), (Values{3, 2, 1, 5, 4, 3, 2, 1}));
  // And undoing the old snapshot lands on the old cells, not the new ones.
  EXPECT_EQ(values(run("⌽W").view), (Values{1, 2, 3, 4, 5}));
}

TEST_F(VplBoundedViewsTest, CellsAreSharedNotCopied) {
  const auto a      = run("A ← 50⍳d.1; A");
  const auto before = arena.cellCount();
  const auto r      = run("⌽A");
  const auto added  = arena.cellCount() - before;
  // Registers and ops only: fifty value cells would be at least fifty.
  EXPECT_LT(added, 50U);
  auto cells = r.view.collectCells(arena);
  std::ranges::reverse(cells);
  EXPECT_EQ(cells, a.view.collectCells(arena));
}

TEST_F(VplBoundedViewsTest, TransposeOfAReversedViewKeepsTheStop) {
  run("A ← 5⍳d.1");
  const auto t = run("⍉⌽A");
  EXPECT_EQ(values(t.view), (Values{5, 4, 3, 2, 1}));
  EXPECT_EQ(t.view.stops().size(), 1U);
  const auto u = run("V ← H ⍴ (d.1 d.2)");
  EXPECT_TRUE(u.view.stops().empty());
}

TEST_F(VplBoundedViewsTest, StopsTravelWithTheirAxesThroughATranspose) {
  // A 2x3 grid built by hand; ⌽ bounds axis 0, ⍉ moves that bound to axis 1.
  const auto d1 = core->findOrMintDimension("d.1");
  const auto d2 = core->findOrMintDimension("d.2");
  std::vector<std::vector<CellRef>> grid(2);
  for (auto &row : grid) {
    for (int c = 0; c < 3; ++c) row.push_back(arena.makeCell());
  }
  std::int64_t n = 0;
  for (std::size_t r = 0; r < 2; ++r) {
    zigzag::expectWritten(arena.linkChain(grid[r], d2, DimVector::POS));
    for (std::size_t c = 0; c < 3; ++c) {
      core->writeValue(grid[r][c], 0, std::nullopt, std::int64_t{++n});
    }
  }
  for (std::size_t c = 0; c < 3; ++c) {
    const std::array<CellRef, 2> column{grid[0][c], grid[1][c]};
    zigzag::expectWritten(arena.linkChain(column, d1, DimVector::POS));
  }
  executor->setVariable(
      "M", VplView{grid[0][0], {{d1, DimVector::POS}, {d2, DimVector::POS}}});
  const auto r = run("⍉⌽M");
  ASSERT_EQ(r.view.axes().size(), 2U);
  EXPECT_EQ(r.view.axes()[0].dim, d2);
  EXPECT_FALSE(r.view.stopAt(0));
  EXPECT_EQ(r.view.stopAt(1), grid[0][0]);
  EXPECT_EQ(r.view.shape(arena), (std::vector<std::size_t>{3, 2}));
  // The bound along d.1 (rows 1 then 0) holds for the first column.
  const auto first = run("⌽M");
  EXPECT_EQ(first.view.stopAt(0), grid[0][0]);
  EXPECT_FALSE(first.view.stopAt(1));
  EXPECT_EQ(first.view.shape(arena), (std::vector<std::size_t>{2, 3}));
  EXPECT_EQ(values(first.view), (Values{4, 5, 6, 1, 2, 3}));
  // Reversing both axes would need a stop per row: refused, not guessed.
  auto both = executor->execute("⊖⌽M", kBudget);
  ASSERT_FALSE(both.has_value());
  EXPECT_NE(both.error().message.find("already reversed along another axis"),
            std::string::npos)
      << both.error().message;
  // Reversing the same axis again is exact.
  EXPECT_EQ(values(run("⌽⌽M").view), (Values{1, 2, 3, 4, 5, 6}));
}

} // namespace
} // namespace xanadu::vpl
