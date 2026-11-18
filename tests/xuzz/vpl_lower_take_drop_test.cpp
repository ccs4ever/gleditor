/**
 * @file vpl_lower_take_drop_test.cpp
 * @brief Card V5: drop runs as a live view on the Vortex VM; take is refused
 * by name until a view can carry a far-end bound.
 */
#include <algorithm>
#include <array>
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
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vpl {
namespace {

using zigzag::CellRef;
using zigzag::DimVector;
using zigzag::vortex::VortexCore;
using zigzag::vortex::VortexVM;

constexpr std::size_t kBudget = 200000;

using Values = std::vector<std::int64_t>;

class VplTakeDropTest : public ::testing::Test {
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

  void expectCompileRefusal(const std::string &source,
                            const std::string &text) {
    const auto result = executor->execute(source, kBudget);
    ASSERT_FALSE(result.has_value()) << source;
    EXPECT_EQ(result.error().stage, ExecutionError::Stage::Compile) << source;
    EXPECT_NE(result.error().message.find(text), std::string::npos)
        << source << ": " << result.error().message;
  }

  zigzag::ArenaManifold arena;
  std::unique_ptr<VortexCore> core;
  std::unique_ptr<VortexVM> vm;
  std::unique_ptr<Executor> executor;
};

TEST_F(VplTakeDropTest, DropSkipsTheFirstCellsOfTheOperandsOwnRank) {
  const auto a        = run("A ← 5⍳d.1; A");
  const auto original = a.view.collectCells(arena);
  ASSERT_EQ(original.size(), 5U);
  for (const auto *source : {"2 ↓ A", "2 }. A"}) {
    const auto r = run(source);
    ASSERT_EQ(r.view.axes().size(), 1U) << source;
    EXPECT_EQ(r.view.axes()[0].dim, dim("d.1")) << source;
    EXPECT_FALSE(r.view.isReversed(0)) << source;
    // No copy: the origin is A's third cell, not a new one.
    EXPECT_EQ(r.view.origin(), original[2]) << source;
    EXPECT_EQ(values(r.view), (Values{3, 4, 5})) << source;
    EXPECT_EQ(r.view.shape(arena), std::vector<std::size_t>{3}) << source;
  }
  // A dropped view leaves the operand untouched and mints nothing.
  const auto after = run("A");
  EXPECT_EQ(after.view.collectCells(arena), original);
}

TEST_F(VplTakeDropTest, DropOfZeroIsTheWholeRank) {
  const auto a = run("A ← 4⍳d.1; A");
  const auto r = run("0 ↓ A");
  EXPECT_EQ(r.view.origin(), a.view.origin());
  EXPECT_EQ(values(r.view), (Values{1, 2, 3, 4}));
}

TEST_F(VplTakeDropTest, DropOfTheWholeLengthOrMoreIsAnEmptyArrayWithItsAxis) {
  run("A ← 5⍳d.1");
  for (const auto *source : {"5 ↓ A", "6 ↓ A", "40 ↓ A"}) {
    const auto r = run(source);
    ASSERT_EQ(r.view.axes().size(), 1U) << source;
    EXPECT_EQ(r.view.axes()[0].dim, dim("d.1")) << source;
    EXPECT_FALSE(r.view.origin().has_value()) << source;
    EXPECT_TRUE(r.view.collectCells(arena).empty()) << source;
    EXPECT_EQ(r.view.shape(arena), std::vector<std::size_t>{0}) << source;
  }
}

TEST_F(VplTakeDropTest, DropOfAnEmptyArrayStaysEmpty) {
  // A bound empty array cannot cross programs (the compiler says so by name),
  // so the empty operand is made in the same program.
  const auto r = run("E ← 0⍳d.1; 3 ↓ E");
  EXPECT_FALSE(r.view.origin().has_value());
  EXPECT_EQ(r.view.shape(arena), std::vector<std::size_t>{0});
  const auto none = run("E ← 0⍳d.1; 0 ↓ E");
  EXPECT_EQ(none.view.shape(arena), std::vector<std::size_t>{0});
}

TEST_F(VplTakeDropTest, DropCountMayBeComputedAtRunTime) {
  const auto a = run("A ← 5⍳d.1; A");
  const auto r = run("n ← 1 + 2; n ↓ A");
  EXPECT_EQ(r.view.origin(), a.view.collectCells(arena)[3]);
  EXPECT_EQ(values(r.view), (Values{4, 5}));
}

TEST_F(VplTakeDropTest, DropOfDropComposesAndRunsOffTheEndCleanly) {
  const auto a      = run("A ← 6⍳d.1; A");
  const auto cells  = a.view.collectCells(arena);
  const auto nested = run("1 ↓ 2 ↓ A");
  EXPECT_EQ(nested.view.origin(), cells[3]);
  EXPECT_EQ(values(nested.view), (Values{4, 5, 6}));
  // Bound drops compose too: the second one starts from the first's view.
  run("D ← 2 ↓ A");
  const auto onBound = run("2 ↓ D");
  EXPECT_EQ(values(onBound.view), (Values{5, 6}));
  // 4 + 3 > 6: the second hop starts from an absent register and stays there.
  const auto beyond = run("3 ↓ 4 ↓ A");
  EXPECT_FALSE(beyond.view.origin().has_value());
  EXPECT_EQ(beyond.view.shape(arena), std::vector<std::size_t>{0});
}

TEST_F(VplTakeDropTest, DropIsLiveWhenTheOperandGrows) {
  run("A ← 5⍳d.1");
  run("D ← 2 ↓ A");
  EXPECT_EQ(values(run("D").view), (Values{3, 4, 5}));
  // A later mint after A's tail lengthens the rank, and the dropped view with
  // it, because the view holds only the origin.
  run("3 ⍳ d.1");
  EXPECT_EQ(values(run("D").view), (Values{3, 4, 5, 1, 2, 3}));
}

TEST_F(VplTakeDropTest, ReverseOfADropReadsTheDroppedCellsBackward) {
  run("A ← 5⍳d.1");
  const auto r = run("⌽ 2 ↓ A");
  EXPECT_TRUE(r.view.isReversed(0));
  auto cells = r.view.collectCells(arena);
  std::ranges::reverse(cells);
  EXPECT_EQ(cells.size(), 3U);
  EXPECT_EQ(values(r.view), (Values{5, 4, 3}));
}

TEST_F(VplTakeDropTest, DropKeepsTheOperandsInputDependencies) {
  const auto a = run("A ← 5⍳d.1; A");
  const auto r = run("2 ↓ A");
  EXPECT_EQ(r.view.dependencies(), a.view.dependencies());
}

TEST_F(VplTakeDropTest, NegativeDropCountsFromTheEnd) {
  // Ruled 2026-11-18 with the view bound: the spec's "negative counts from the
  // end" is now lowered, so what this test pinned as a refusal is a result.
  run("A ← 5⍳d.1");
  EXPECT_EQ(values(run("¯2 ↓ A").view), (Values{1, 2, 3}));
  EXPECT_EQ(values(run("(-2) ↓ A").view), (Values{1, 2, 3}));
  EXPECT_EQ(values(run("n ← 0 - 2; n ↓ A").view), (Values{1, 2, 3}));
}

TEST_F(VplTakeDropTest, FormsThatStillCannotBeAViewAreRefusedByName) {
  run("A ← 5⍳d.1");
  expectCompileRefusal("¯3 ↑ A", "take of a negative count is unsupported");
  expectCompileRefusal("2.5 ↓ A", "non-integer count");
  expectCompileRefusal("2 ↓ 7", "drop of a scalar");
  expectCompileRefusal("1 2 ↓ A", "rank as its count");
  expectCompileRefusal("d.1 ↓ A", "dimension value as its count");
}

TEST_F(VplTakeDropTest, TakeIsAViewWithAFarEndBound) {
  run("A ← 5⍳d.1");
  const auto r = run("3 ↑ A");
  ASSERT_EQ(r.view.bounds().size(), 1U);
  EXPECT_TRUE(r.view.bounds()[0].has_value());
  EXPECT_EQ(values(r.view), (Values{1, 2, 3}));
}

TEST_F(VplTakeDropTest, DropOfAMatrixIsRefusedByName) {
  const auto d1 = dim("d.1");
  const auto d2 = dim("d.2");
  std::vector<std::vector<CellRef>> grid(2);
  for (auto &row : grid) {
    for (int c = 0; c < 3; ++c) row.push_back(arena.makeCell());
  }
  for (std::size_t r = 0; r < 2; ++r) {
    zigzag::expectWritten(arena.linkChain(grid[r], d2, DimVector::POS));
  }
  for (std::size_t c = 0; c < 3; ++c) {
    const std::array<CellRef, 2> column{grid[0][c], grid[1][c]};
    zigzag::expectWritten(arena.linkChain(column, d1, DimVector::POS));
  }
  executor->setVariable(
      "M", VplView{grid[0][0], {{d1, DimVector::POS}, {d2, DimVector::POS}}});
  expectCompileRefusal("1 ↓ M", "more than one axis");
}

} // namespace
} // namespace xanadu::vpl
