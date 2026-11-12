/**
 * @file vpl_lower_views_test.cpp
 * @brief Card V4: transpose, reverse, dimension values and the dimension view
 * of dyadic shape run as graphs on the Vortex VM.
 */
#include <algorithm>
#include <array>
#include <cstdint>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <vector>

#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vpl/compiler.hpp"
#include "common/xanadu/vpl/execution.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vpl {
namespace {

using zigzag::CellRef;
using zigzag::DimVector;
using zigzag::vortex::VortexCore;
using zigzag::vortex::VortexVM;

constexpr std::size_t kBudget = 200000;
// Few enough that no program can finish: the VM must stop it, not a host loop.
constexpr std::size_t kTinyBudget = 3;

using Values = std::vector<std::int64_t>;

class VplViewsTest : public ::testing::Test {
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

  std::optional<Values> values(const VplView &view) {
    Values out;
    for (const auto cell : view.collectCells(arena)) {
      const auto value = arena.asInt64(cell);
      if (!value) return std::nullopt;
      out.push_back(*value);
    }
    return out;
  }

  zigzag::DimRef dim(const char *name) {
    return core->findOrMintDimension(name);
  }

  void expectRefusal(const std::string &source, const std::string &text) {
    const auto result = executor->execute(source, kBudget);
    ASSERT_FALSE(result.has_value()) << source;
    EXPECT_EQ(result.error().message, "Compilation error: " + text) << source;
  }

  zigzag::ArenaManifold arena;
  std::unique_ptr<VortexCore> core;
  std::unique_ptr<VortexVM> vm;
  std::unique_ptr<Executor> executor;
};

TEST_F(VplViewsTest, ReverseRankOneWalksTheSameCellsNegward) {
  const auto a        = run("A ← 5⍳d.1; A");
  const auto original = a.view.collectCells(arena);
  ASSERT_EQ(original.size(), 5U);
  const auto predecessor =
      arena.linked(original.front(), dim("d.1"), DimVector::NEG);
  ASSERT_TRUE(predecessor);
  for (const auto *source : {"⌽A", "|. A", "⊖A", "|.. A"}) {
    const auto r = run(source);
    ASSERT_EQ(r.view.axes().size(), 1U) << source;
    EXPECT_EQ(r.view.axes()[0].dir, DimVector::NEG) << source;
    EXPECT_EQ(r.view.origin(), original.back()) << source;
    // The same cells in the opposite order: nothing was copied. A view has no
    // extent, so the negward walk also runs on through the cell that precedes
    // A's head on d.1 (the home cell here); only a copy could cut it off.
    auto cells = r.view.collectCells(arena);
    ASSERT_EQ(cells.size(), original.size() + 1) << source;
    EXPECT_EQ(cells.back(), *predecessor) << source;
    cells.pop_back();
    std::ranges::reverse(cells);
    EXPECT_EQ(cells, original) << source;
  }
}

TEST_F(VplViewsTest, ReverseOfReverseIsTheOriginal) {
  const auto a    = run("A ← 4⍳d.1; A");
  const auto r    = run("⌽⌽A");
  const auto same = run("⊖⊖A");
  EXPECT_EQ(same.view.origin(), a.view.origin());
  EXPECT_EQ(r.view.axes()[0].dir, DimVector::POS);
  EXPECT_EQ(r.view.origin(), a.view.origin());
  EXPECT_EQ(values(r.view), (Values{1, 2, 3, 4}));
  EXPECT_EQ(r.view.shape(arena), std::vector<std::size_t>{4});
}

TEST_F(VplViewsTest, ReverseFirstAndLastFlipDifferentAxesOfAMatrix) {
  // H ⍴ dims names a 2-axis view over three dimensions' worth of structure;
  // the cells are built by hand so the grid is a known 2x3.
  const auto d1 = dim("d.1");
  const auto d2 = dim("d.2");
  std::vector<std::vector<CellRef>> grid(2);
  for (auto &row : grid) {
    for (std::int64_t c = 0; c < 3; ++c) row.push_back(arena.makeCell());
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

  const auto first = run("⌽M");
  EXPECT_EQ(first.view.axes()[0].dir, DimVector::NEG);
  EXPECT_EQ(first.view.axes()[1].dir, DimVector::POS);
  EXPECT_EQ(first.view.origin(), grid[1][0]);
  EXPECT_EQ(first.view.shape(arena), (std::vector<std::size_t>{2, 3}));

  const auto last = run("⊖M");
  EXPECT_EQ(last.view.axes()[0].dir, DimVector::POS);
  EXPECT_EQ(last.view.axes()[1].dir, DimVector::NEG);
  EXPECT_EQ(last.view.origin(), grid[0][2]);
  EXPECT_EQ(last.view.shape(arena), (std::vector<std::size_t>{2, 3}));

  const auto transposed = run("⍉M");
  EXPECT_EQ(transposed.view.axes()[0].dim, d2);
  EXPECT_EQ(transposed.view.axes()[1].dim, d1);
  EXPECT_EQ(transposed.view.origin(), grid[0][0]);
  EXPECT_EQ(transposed.view.shape(arena), (std::vector<std::size_t>{3, 2}));

  // A view of a view: transposing twice restores the axis order.
  const auto twice = run("⍉⍉M");
  EXPECT_EQ(twice.view.axes()[0].dim, d1);
  EXPECT_EQ(twice.view.axes()[1].dim, d2);
  EXPECT_EQ(twice.view.shape(arena), (std::vector<std::size_t>{2, 3}));
  const auto back = run("⌽⌽M");
  EXPECT_EQ(run("⊖⊖M").view.origin(), grid[0][0]);
  EXPECT_EQ(back.view.origin(), grid[0][0]);
  EXPECT_EQ(back.view.axes()[0].dir, DimVector::POS);
}

TEST_F(VplViewsTest, DimensionViewHasOneAxisPerDimensionAndNoCellsOfItsOwn) {
  const auto before = arena.cellCount();
  const auto v      = run("V ← H ⍴ (d.1 d.2 d.3); V");
  ASSERT_EQ(v.view.axes().size(), 3U);
  EXPECT_EQ(v.view.axes()[0].dim, dim("d.1"));
  EXPECT_EQ(v.view.axes()[2].dim, dim("d.3"));
  EXPECT_EQ(v.view.origin(), core->home());
  // Only compile-time registers and dimension cells appear; no value cells
  // are minted when the program runs.
  EXPECT_LT(arena.cellCount() - before, 64U);

  const auto t = run("⍉V");
  ASSERT_EQ(t.view.axes().size(), 3U);
  EXPECT_EQ(t.view.axes()[0].dim, dim("d.3"));
  EXPECT_EQ(t.view.axes()[2].dim, dim("d.1"));
  EXPECT_EQ(t.view.origin(), core->home());
  const auto ascii = run("|: V");
  EXPECT_EQ(ascii.view.axes()[0].dim, dim("d.3"));
  const auto twice = run("⍉⍉V");
  EXPECT_EQ(twice.view.axes()[0].dim, dim("d.1"));
  EXPECT_EQ(twice.view.axes()[2].dim, dim("d.3"));
}

TEST_F(VplViewsTest, DyadicTransposePermutesTheAxisList) {
  run("V ← H ⍴ (d.1 d.2 d.3)");
  const auto p = run("(2 0 1) ⍉ V");
  ASSERT_EQ(p.view.axes().size(), 3U);
  EXPECT_EQ(p.view.axes()[0].dim, dim("d.3"));
  EXPECT_EQ(p.view.axes()[1].dim, dim("d.1"));
  EXPECT_EQ(p.view.axes()[2].dim, dim("d.2"));
  const auto id = run("(0 1 2) ⍉ V");
  EXPECT_EQ(id.view.axes()[0].dim, dim("d.1"));
  expectRefusal("(0 1) ⍉ V",
                "VPL dyadic transpose left operand must be a permutation of "
                "the axes");
  expectRefusal("(0 0 1) ⍉ V",
                "VPL dyadic transpose left operand must be a permutation of "
                "the axes");
  expectRefusal("(0 1 5) ⍉ V",
                "VPL dyadic transpose left operand must be a permutation of "
                "the axes");
  expectRefusal("(0 1 2.5) ⍉ V",
                "VPL dyadic transpose requires a literal integer permutation");
}

TEST_F(VplViewsTest, DimensionValueIsStructureWithNoCells) {
  const auto one = run("d.1");
  EXPECT_FALSE(one.view.origin());
  ASSERT_EQ(one.view.axes().size(), 1U);
  EXPECT_EQ(one.view.axes()[0].dim, dim("d.1"));
  EXPECT_EQ(one.view.axes()[0].dir, DimVector::POS);

  const auto many = run("(d.1 d.2 d.3)");
  EXPECT_FALSE(many.view.origin());
  ASSERT_EQ(many.view.axes().size(), 3U);
  EXPECT_EQ(many.view.axes()[1].dim, dim("d.2"));
}

TEST_F(VplViewsTest, ReverseOfABoundedChainIsExactAndUndoesItselfInTheGraph) {
  // A literal vector has no cell before its head, so the negward walk stops
  // where the vector does, and even a reversal held in a variable undoes.
  const auto v = run("V ← 10 20 30; V");
  const auto r = run("⌽V");
  EXPECT_EQ(values(r.view), (Values{30, 20, 10}));
  EXPECT_EQ(r.view.shape(arena), std::vector<std::size_t>{3});
  const auto back = run("W ← ⌽V; ⌽W");
  EXPECT_EQ(back.view.origin(), v.view.origin());
  EXPECT_EQ(values(back.view), (Values{10, 20, 30}));
}

TEST_F(VplViewsTest, ViewsOfEmptyArraysStayEmptyAndKeepTheirAxes) {
  // Within one program, since a binding carries no cell for an empty array.
  for (const auto *source :
       {"E ← 0⍳d.2; ⌽E", "E ← 0⍳d.2; ⊖E", "E ← 0⍳d.2; ⍉E", "E ← 0⍳d.2; ⌽⌽E"}) {
    const auto r = run(source);
    EXPECT_FALSE(r.view.origin()) << source;
    ASSERT_EQ(r.view.axes().size(), 1U) << source;
    EXPECT_EQ(r.view.axes()[0].dim, dim("d.2")) << source;
    EXPECT_EQ(r.view.shape(arena), std::vector<std::size_t>{0}) << source;
  }
  EXPECT_EQ(run("E ← 0⍳d.2; ⌽E").view.axes()[0].dir, DimVector::NEG);
}

TEST_F(VplViewsTest, ScalarOperandsAreTheirOwnViews) {
  for (const auto *source : {"⌽ 7", "⊖ 7", "⍉ 7"}) {
    const auto r = run(source);
    EXPECT_TRUE(r.view.axes().empty()) << source;
    EXPECT_EQ(r.view.scalarInt(), 7) << source;
  }
}

TEST_F(VplViewsTest, RefusalsNameTheForm) {
  expectRefusal("⍉ d.1",
                "VPL transpose of a dimension value is unsupported by the VM");
  expectRefusal("⌽ d.1",
                "VPL reverse of a dimension value is unsupported by the VM");
  expectRefusal("⊖ (d.1 d.2)",
                "VPL reverse of a dimension value is unsupported by the VM");
  expectRefusal("⍉ ⊂ 1 2 3", "unsupported VPL monadic verb in VM compiler");
  expectRefusal("(1 2 3) ⍴ d.1",
                "VPL dimension view of a rank is unsupported by the VM; use "
                "the shape link form");
  expectRefusal("d.1 ⍴ (d.2 d.3)",
                "VPL dimension view of a dimension value is unsupported by "
                "the VM");
}

TEST_F(VplViewsTest, ACycleBudgetStopsAReverseThatWalksToTheTail) {
  run("A ← 50⍳d.1");
  const auto result = executor->execute("⌽A", kTinyBudget);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().stage, ExecutionError::Stage::Budget);
}

} // namespace
} // namespace xanadu::vpl
