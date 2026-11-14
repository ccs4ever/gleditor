/**
 * @file vpl_lower_fold_test.cpp
 * @brief Card V7: fold (reduce, scan and tally) runs on the Vortex VM.
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

class VplLowerFoldTest : public ::testing::Test {
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
      if (const auto value = arena.asInt64(cell)) {
        out.push_back(*value);
      } else if (const auto b = arena.asBool(cell)) {
        out.push_back(*b ? 1 : 0);
      } else {
        return std::nullopt;
      }
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

TEST_F(VplLowerFoldTest, ReduceOverRuntimeRanks) {
  // Sum reduction on dynamic rank: 1+2+3+4+5 = 15
  {
    const auto r = run("A ← 5⍳d.1; +/ A");
    EXPECT_TRUE(r.view.isScalar());
    EXPECT_EQ(r.view.scalarInt(), 15);
  }

  // Product reduction: 1*2*3*4 = 24
  {
    const auto r = run("A ← 4⍳d.1; ×/ A");
    EXPECT_TRUE(r.view.isScalar());
    EXPECT_EQ(r.view.scalarInt(), 24);
  }

  // Max and Min reductions
  {
    run("data ← 12 10 5");
    const auto sum = run("+/ data");
    EXPECT_TRUE(sum.view.isScalar());
    EXPECT_EQ(sum.view.scalarInt(), 27);

    const auto max = run("⌈/ data");
    EXPECT_TRUE(max.view.isScalar());
    EXPECT_EQ(max.view.scalarInt(), 12);

    const auto min = run("⌊/ data");
    EXPECT_TRUE(min.view.isScalar());
    EXPECT_EQ(min.view.scalarInt(), 5);
  }

  // Single-cell rank reduction produces its sole value
  {
    const auto r = run("A ← 1⍳d.1; +/ A");
    EXPECT_TRUE(r.view.isScalar());
    EXPECT_EQ(r.view.scalarInt(), 1);
  }

  // Scalar reduction is identity
  {
    const auto r = run("+/ 42");
    EXPECT_TRUE(r.view.isScalar());
    EXPECT_EQ(r.view.scalarInt(), 42);
  }
}

TEST_F(VplLowerFoldTest, ReduceOverReversedViews) {
  run("A ← 5⍳d.1");
  // Sum over reversed view is commutative
  const auto sumRev = run("+/ ⌽A");
  EXPECT_TRUE(sumRev.view.isScalar());
  EXPECT_EQ(sumRev.view.scalarInt(), 15);

  // Subtraction reduction over reversed: 5 - 4 - 3 - 2 - 1 = -5
  // Right-associative fold in APL: 5 - (4 - (3 - (2 - 1))) = 5 - (4 - (3 - 1))
  // = 5 - 2 = 3 In our left-to-right walk: ((5 - 4) - 3) - 2) - 1 = 1 - 3 - 2 -
  // 1 = -5
  const auto subRev = run("-/ ⌽A");
  EXPECT_TRUE(subRev.view.isScalar());
  EXPECT_EQ(subRev.view.scalarInt(), -5);
}

TEST_F(VplLowerFoldTest, ScanOverRuntimeRanks) {
  // Sum scan on dynamic rank: 1 3 6 10 15
  {
    const auto r = run("A ← 5⍳d.1; +\\ A");
    ASSERT_EQ(r.view.axes().size(), 1U);
    EXPECT_EQ(values(r.view), (Values{1, 3, 6, 10, 15}));
  }

  // Scan on a scalar produces a single-cell rank
  {
    const auto r = run("+\\ 42");
    ASSERT_EQ(r.view.axes().size(), 1U);
    EXPECT_EQ(values(r.view), (Values{42}));
  }
}

TEST_F(VplLowerFoldTest, TallyOverRanksAndScalars) {
  // Tally on dynamic rank: ≢
  {
    const auto r = run("A ← 5⍳d.1; ≢ A");
    EXPECT_TRUE(r.view.isScalar());
    EXPECT_EQ(r.view.scalarInt(), 5);
  }

  // J syntax tally: #
  {
    const auto r = run("B =. 3 i. d.1; # B");
    EXPECT_TRUE(r.view.isScalar());
    EXPECT_EQ(r.view.scalarInt(), 3);
  }

  // Tally on literal vector
  {
    const auto r = run("≢ 10 20 30 40");
    EXPECT_TRUE(r.view.isScalar());
    EXPECT_EQ(r.view.scalarInt(), 4);
  }

  // Tally on scalar is 1
  {
    const auto r = run("≢ 42");
    EXPECT_TRUE(r.view.isScalar());
    EXPECT_EQ(r.view.scalarInt(), 1);
  }
}

TEST_F(VplLowerFoldTest, RefusesUnsupportedOperands) {
  // Matrix operand
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

  expectCompileRefusal("+/ M", "more than one axis");
  expectCompileRefusal("+\\ M", "more than one axis");
  expectCompileRefusal("≢ M", "more than one axis");

  // Dimension value
  expectCompileRefusal("+/ d.1", "dimension value");
  expectCompileRefusal("≢ d.1", "dimension value");
}

} // namespace
} // namespace xanadu::vpl
