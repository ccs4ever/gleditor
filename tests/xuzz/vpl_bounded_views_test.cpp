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
  emit::walkRank(
      ctx_, cells[3], dim_, DimVector::NEG, [&](CellRef) { bump(unbounded); },
      std::nullopt);
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
  const auto whole =
      emit::extentOf(ctx_, cells[0], dim_, DimVector::POS, std::nullopt);
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

TEST_F(GraphStopTest,
       WalkAxisReadsAReversedAxisLiveFromItsOriginNotItsPredecessor) {
  // 9 precedes the view's origin (the home cell, in real programs).
  const auto cells = makeRank({9, 1, 2, 3, 4});
  const auto tail  = arena_.makeScalarCell(std::int64_t{0});
  auto record      = emit::aliasRegister(ctx_, tail);
  const auto count = counter();
  emit::walkAxis(ctx_, cells[1], dim_, DimVector::POS, true, [&](CellRef item) {
    bump(count);
    const auto fresh = emit::mintAfter(ctx_, record, other_, DimVector::POS);
    emit::copyValue(ctx_, item, fresh);
  });
  const auto forward = arena_.makeScalarCell(std::int64_t{0});
  auto forwardTail   = emit::aliasRegister(ctx_, forward);
  emit::walkAxis(
      ctx_, cells[1], dim_, DimVector::POS, false, [&](CellRef item) {
        const auto fresh =
            emit::mintAfter(ctx_, forwardTail, other_, DimVector::POS);
        emit::copyValue(ctx_, item, fresh);
      });
  // A rank that is empty or has one cell is its own reverse.
  const auto lone = makeRank({7});
  const auto one  = counter();
  emit::walkAxis(ctx_, lone[0], dim_, DimVector::POS, true,
                 [&](CellRef) { bump(one); });
  const auto gone = arena_.makeCell();
  const auto clr  = ctx_.emitOp(OpcodeKind::ClearCellRefs, "#TEST_ABSENT");
  ctx_.emitOutput(clr, gone);
  const auto none = counter();
  emit::walkAxis(ctx_, gone, dim_, DimVector::POS, true,
                 [&](CellRef) { bump(none); });
  run();
  const auto read = [&](CellRef head) {
    std::vector<std::int64_t> out;
    std::optional<CellRef> at = arena_.linked(head, other_, DimVector::POS);
    while (at) {
      out.push_back(valueOf(*at));
      at = arena_.linked(*at, other_, DimVector::POS);
    }
    return out;
  };
  EXPECT_EQ(read(tail), (std::vector<std::int64_t>{4, 3, 2, 1}));
  EXPECT_EQ(read(forward), (std::vector<std::int64_t>{1, 2, 3, 4}));
  EXPECT_EQ(valueOf(count), 4);
  EXPECT_EQ(valueOf(one), 1);
  EXPECT_EQ(valueOf(none), 0);
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

  // Rows of possibly different lengths, linked along d.2 within a row and d.1
  // between rows wherever the next row has that column.
  CellRef raggedMatrix(const std::vector<std::vector<std::int64_t>> &rows,
                       const std::string &name) {
    const auto d1 = core->findOrMintDimension("d.1");
    const auto d2 = core->findOrMintDimension("d.2");
    std::vector<std::vector<CellRef>> grid;
    for (const auto &row : rows) {
      grid.emplace_back();
      for (std::size_t c = 0; c < row.size(); ++c) {
        const auto cell = arena.makeCell();
        core->writeValue(cell, 0, std::nullopt, std::int64_t{row[c]});
        grid.back().push_back(cell);
      }
      zigzag::expectWritten(arena.linkChain(grid.back(), d2, DimVector::POS));
    }
    for (std::size_t r = 0; r + 1 < grid.size(); ++r) {
      for (std::size_t c = 0; c < std::min(grid[r].size(), grid[r + 1].size());
           ++c) {
        zigzag::expectWritten(
            arena.link(grid[r][c], d1, false, grid[r + 1][c]));
      }
    }
    executor->setVariable(
        name,
        VplView{grid[0][0], {{d1, DimVector::POS}, {d2, DimVector::POS}}});
    return grid[0][0];
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
  // Nothing moved: the origin is A's own, only the flag differs.
  EXPECT_EQ(r.view.origin(), a.view.origin());
}

TEST_F(VplBoundedViewsTest, AReversedViewIsLiveAndSeesCellsAppendedLater) {
  run("A ← 5⍳d.1; B ← ⌽A; 3⍳d.1");
  const auto a = run("A");
  const auto b = run("B");
  EXPECT_EQ(values(a.view), (Values{1, 2, 3, 4, 5, 1, 2, 3}));
  auto expected = values(a.view);
  std::ranges::reverse(expected);
  EXPECT_EQ(values(b.view), expected);
  EXPECT_EQ(shapeOf("⍴B"), 8);
  EXPECT_EQ(shapeOf("⍴A"), 8);
  // Within one program too: the append runs before B is read.
  EXPECT_EQ(values(run("C ← 2⍳d.2; D ← ⌽C; 3⍳d.2; D").view),
            (Values{3, 2, 1, 2, 1}));
}

TEST_F(VplBoundedViewsTest, ReverseOfReverseIsTheOperandItselfAndStaysLive) {
  const auto a = run("A ← 4⍳d.1; A");
  const auto r = run("⌽⌽A");
  EXPECT_EQ(r.view.origin(), a.view.origin());
  EXPECT_TRUE(r.view.reversed().empty());
  EXPECT_EQ(r.view.collectCells(arena), a.view.collectCells(arena));
  EXPECT_EQ(values(run("⊖⊖A").view), (Values{1, 2, 3, 4}));
  const auto held = run("W ← ⌽A; ⌽W");
  EXPECT_EQ(held.view.collectCells(arena), a.view.collectCells(arena));
  EXPECT_EQ(values(run("W").view), (Values{4, 3, 2, 1}));
  EXPECT_EQ(values(run("⌽W").view), (Values{1, 2, 3, 4}));
  // Growth reaches the double reversal too.
  run("2⍳d.1");
  EXPECT_EQ(values(run("⌽⌽A").view), (Values{1, 2, 3, 4, 1, 2}));
  EXPECT_EQ(values(run("⌽W").view), (Values{1, 2, 3, 4, 1, 2}));
}

TEST_F(VplBoundedViewsTest,
       ABindingAndItsReversalShareAnOriginWithoutConfusion) {
  run("A ← 3⍳d.1; W ← ⌽A");
  EXPECT_EQ(values(run("A").view), (Values{1, 2, 3}));
  EXPECT_EQ(values(run("W").view), (Values{3, 2, 1}));
  EXPECT_EQ(values(run("W; A").view), (Values{1, 2, 3}));
  EXPECT_EQ(values(run("A; W").view), (Values{3, 2, 1}));
}

TEST_F(VplBoundedViewsTest, LiteralVectorsOneCellAndEmptyArraysStayExact) {
  EXPECT_EQ(values(run("⌽ 10 20 30").view), (Values{30, 20, 10}));
  EXPECT_EQ(values(run("V ← 10 20 30; ⌽⌽V").view), (Values{10, 20, 30}));
  EXPECT_EQ(values(run("O ← 1⍳d.3; ⌽O").view), (Values{1}));
  const auto empty = run("E ← 0⍳d.1; ⌽E");
  EXPECT_FALSE(empty.view.origin());
  EXPECT_TRUE(empty.view.collectCells(arena).empty());
  EXPECT_EQ(empty.view.shape(arena), std::vector<std::size_t>{0});
  EXPECT_TRUE(run("E ← 0⍳d.1; ⌽⌽E").view.collectCells(arena).empty());
}

TEST_F(VplBoundedViewsTest, AnEmptyArrayFromAnEarlierProgramIsRefusedByName) {
  run("E ← 0⍳d.1");
  const auto result = executor->execute("⌽E", kBudget);
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().message.find(
                "holds an empty array or a dimension value from an earlier "
                "program"),
            std::string::npos)
      << result.error().message;
}

TEST_F(VplBoundedViewsTest, CellsAreSharedNotCopiedAndReverseMintsNothing) {
  const auto a      = run("A ← 50⍳d.1; A");
  const auto before = arena.cellCount();
  const auto r      = run("⌽A");
  // Registers and an alias op: fifty value cells would be at least fifty.
  EXPECT_LT(arena.cellCount() - before, 50U);
  auto cells = r.view.collectCells(arena);
  std::ranges::reverse(cells);
  EXPECT_EQ(cells, a.view.collectCells(arena));
}

TEST_F(VplBoundedViewsTest, MintingThroughAReversedViewShowsNewCellsFirst) {
  run("A ← 3⍳d.1; B ← ⌽A");
  run("2⍳d.1");
  EXPECT_EQ(values(run("B").view), (Values{2, 1, 3, 2, 1}));
  EXPECT_EQ(values(run("A").view), (Values{1, 2, 3, 1, 2}));
}

TEST_F(VplBoundedViewsTest, RectangularMatricesReverseToKnownValues) {
  raggedMatrix({{1, 2, 3}, {4, 5, 6}}, "M");
  EXPECT_EQ(values(run("M").view), (Values{1, 2, 3, 4, 5, 6}));
  EXPECT_EQ(values(run("⌽M").view), (Values{4, 5, 6, 1, 2, 3}));
  EXPECT_EQ(values(run("⊖M").view), (Values{3, 2, 1, 6, 5, 4}));
  EXPECT_EQ(values(run("⊖⌽M").view), (Values{6, 5, 4, 3, 2, 1}));
  EXPECT_EQ(values(run("⌽⊖M").view), (Values{6, 5, 4, 3, 2, 1}));
  // Formerly refused combinations are just flags.
  EXPECT_EQ(values(run("⊖⌽⌽M").view), (Values{3, 2, 1, 6, 5, 4}));
  EXPECT_EQ(values(run("⍉⌽M").view), (Values{4, 1, 5, 2, 6, 3}));
  EXPECT_EQ(values(run("⌽⍉⌽M").view), (Values{6, 3, 5, 2, 4, 1}));
  EXPECT_EQ(values(run("⌽⌽M").view), (Values{1, 2, 3, 4, 5, 6}));
  EXPECT_EQ(run("⌽M").view.shape(arena), (std::vector<std::size_t>{2, 3}));
  EXPECT_EQ(run("⍉⌽M").view.shape(arena), (std::vector<std::size_t>{3, 2}));
}

TEST_F(VplBoundedViewsTest, RaggedMatricesReverseToTheSameCellsReordered) {
  const std::vector<std::vector<std::vector<std::int64_t>>> cases{
      {{1, 2, 3}, {4, 5}},
      {{1, 2}, {3, 4, 5}},
      {{1, 2}, {3, 4, 5}, {6, 7}},
  };
  for (const auto &rows : cases) {
    SCOPED_TRACE(rows.size());
    raggedMatrix(rows, "M");
    const auto plain = run("M");
    auto expected    = values(plain.view);
    std::ranges::sort(expected);
    for (const auto *source : {"⌽M", "⊖M", "⊖⌽M", "⍉⌽M", "⌽⍉M"}) {
      const auto r = run(source);
      auto got     = values(r.view);
      std::ranges::sort(got);
      EXPECT_EQ(got, expected) << source;
      auto shape = r.view.shape(arena);
      std::ranges::sort(shape);
      auto plainShape = plain.view.shape(arena);
      std::ranges::sort(plainShape);
      EXPECT_EQ(shape, plainShape) << source;
    }
    EXPECT_EQ(values(run("⌽⌽M").view), values(plain.view));
    EXPECT_EQ(values(run("⊖⊖M").view), values(plain.view));
  }
}

TEST_F(VplBoundedViewsTest, ATransposedReversalCarriesItsFlagWithTheAxis) {
  raggedMatrix({{1, 2, 3}, {4, 5, 6}}, "M");
  const auto r = run("⍉⌽M");
  ASSERT_EQ(r.view.axes().size(), 2U);
  EXPECT_FALSE(r.view.isReversed(0));
  EXPECT_TRUE(r.view.isReversed(1));
  EXPECT_EQ(r.view.origin(), run("M").view.origin());
}

TEST_F(VplBoundedViewsTest, RankThreeViewsReverseAxisByAxis) {
  const auto d1 = core->findOrMintDimension("d.1");
  const auto d2 = core->findOrMintDimension("d.2");
  const auto d3 = core->findOrMintDimension("d.3");
  CellRef cube[2][2][2];
  std::int64_t n = 0;
  for (auto &plane : cube) {
    for (auto &row : plane) {
      for (auto &cell : row) {
        cell = arena.makeCell();
        core->writeValue(cell, 0, std::nullopt, std::int64_t{++n});
      }
    }
  }
  for (int i = 0; i < 2; ++i) {
    for (int j = 0; j < 2; ++j) {
      zigzag::expectWritten(
          arena.link(cube[i][j][0], d3, false, cube[i][j][1]));
      zigzag::expectWritten(
          arena.link(cube[i][0][j], d2, false, cube[i][1][j]));
      zigzag::expectWritten(
          arena.link(cube[0][i][j], d1, false, cube[1][i][j]));
    }
  }
  executor->setVariable("C", VplView{cube[0][0][0],
                                     {{d1, DimVector::POS},
                                      {d2, DimVector::POS},
                                      {d3, DimVector::POS}}});
  EXPECT_EQ(values(run("C").view), (Values{1, 2, 3, 4, 5, 6, 7, 8}));
  EXPECT_EQ(values(run("⌽C").view), (Values{5, 6, 7, 8, 1, 2, 3, 4}));
  EXPECT_EQ(values(run("⊖C").view), (Values{2, 1, 4, 3, 6, 5, 8, 7}));
  EXPECT_EQ(values(run("⊖⌽C").view), (Values{6, 5, 8, 7, 2, 1, 4, 3}));
  EXPECT_EQ(values(run("⌽⌽C").view), (Values{1, 2, 3, 4, 5, 6, 7, 8}));
  EXPECT_EQ(run("⊖⌽C").view.shape(arena), (std::vector<std::size_t>{2, 2, 2}));
}

TEST_F(VplBoundedViewsTest, WalkableViewReadsTheSameCellsWithoutAFlag) {
  run("A ← 5⍳d.1");
  const auto r    = run("⌽A");
  const auto wire = r.view.walkable(arena);
  ASSERT_TRUE(wire.has_value());
  EXPECT_TRUE(wire->reversed().empty());
  EXPECT_EQ(wire->axes()[0].dir, DimVector::NEG);
  EXPECT_EQ(wire->collectCells(arena), r.view.collectCells(arena));
  // Whenever a ragged reversal has a walkable form it reads the same cells.
  raggedMatrix({{1, 2, 3}, {4, 5}}, "M");
  const auto m = run("⊖M");
  if (const auto w = m.view.walkable(arena)) {
    EXPECT_EQ(w->collectCells(arena), m.view.collectCells(arena));
  }
}

} // namespace
} // namespace xanadu::vpl
