/**
 * @file vpl_lower_elementwise_test.cpp
 * @brief Card V6: scalar arithmetic on valence-1 ranks and scalar extension
 * execute as graphs on the Vortex VM.
 */
#include <algorithm>
#include <array>
#include <cmath>
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

class VplLowerElementwiseTest : public ::testing::Test {
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

  std::optional<std::vector<double>> valuesDouble(const VplView &view) {
    std::vector<double> out;
    for (const auto cell : view.collectCells(arena)) {
      const auto value = arena.asDouble(cell);
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

  void expectRuntimeError(const std::string &source, const std::string &text) {
    const auto result = executor->execute(source, kBudget);
    ASSERT_FALSE(result.has_value()) << source;
    EXPECT_EQ(result.error().stage, ExecutionError::Stage::Runtime) << source;
    EXPECT_NE(result.error().message.find(text), std::string::npos)
        << source << ": " << result.error().message;
  }

  zigzag::ArenaManifold arena;
  std::unique_ptr<VortexCore> core;
  std::unique_ptr<VortexVM> vm;
  std::unique_ptr<Executor> executor;
};

TEST_F(VplLowerElementwiseTest, MonadicArithmeticOnValenceOneRank) {
  // Negation
  {
    const auto r = run("- 1 2 3");
    ASSERT_EQ(r.view.axes().size(), 1U);
    EXPECT_EQ(values(r.view), (Values{-1, -2, -3}));
  }

  // Negation on named rank
  {
    const auto a        = run("A ← 5⍳d.1; A");
    const auto orig     = a.view.collectCells(arena);
    const auto r        = run("- A");
    const auto resCells = r.view.collectCells(arena);
    ASSERT_EQ(resCells.size(), 5U);
    EXPECT_EQ(values(r.view), (Values{-1, -2, -3, -4, -5}));
    // Newly minted cells, not the same cell refs as operand.
    for (std::size_t i = 0; i < 5U; ++i) {
      EXPECT_NE(resCells[i], orig[i]);
    }
    // Result preserves the operand's axis dimension.
    ASSERT_EQ(r.view.axes().size(), 1U);
    EXPECT_EQ(r.view.axes()[0].dim, dim("d.1"));
  }

  // Magnitude (abs)
  {
    const auto r = run("| (_1 2 _3)");
    EXPECT_EQ(values(r.view), (Values{1, 2, 3}));
  }

  // Reciprocal (1.0 / x)
  {
    const auto r = run("% 1 2 4");
    ASSERT_EQ(r.view.axes().size(), 1U);
    const auto v = valuesDouble(r.view);
    ASSERT_TRUE(v.has_value());
    ASSERT_EQ(v->size(), 3U);
    EXPECT_DOUBLE_EQ((*v)[0], 1.0);
    EXPECT_DOUBLE_EQ((*v)[1], 0.5);
    EXPECT_DOUBLE_EQ((*v)[2], 0.25);
  }

  // Not
  {
    const auto r = run("∼ 0 1 0");
    EXPECT_EQ(values(r.view), (Values{1, 0, 1}));
  }
}

TEST_F(VplLowerElementwiseTest, DyadicArithmeticOnRanksOfMatchingLength) {
  // Addition
  {
    const auto r = run("1 2 3 + 4 5 6");
    EXPECT_EQ(values(r.view), (Values{5, 7, 9}));
  }

  // Subtraction
  {
    const auto r = run("10 20 30 - 1 2 3");
    EXPECT_EQ(values(r.view), (Values{9, 18, 27}));
  }

  // Multiplication
  {
    const auto r = run("2 3 4 * 3 4 5");
    EXPECT_EQ(values(r.view), (Values{6, 12, 20}));
  }

  // Division
  {
    // Division is on doubles, as in APL (`5 ÷ 2` is 2.5), so the whole
    // quotients are doubles with whole values, not integers.
    const auto r         = run("10 20 30 % 2 4 5");
    const auto quotients = r.view.collectCells(arena);
    ASSERT_EQ(quotients.size(), 3U);
    EXPECT_EQ(arena.asDouble(quotients[0]), 5.0);
    EXPECT_EQ(arena.asDouble(quotients[1]), 5.0);
    EXPECT_EQ(arena.asDouble(quotients[2]), 6.0);
  }

  // Modulo (magnitude dyadic)
  {
    const auto r = run("10 5 12 | 3 4 5");
    EXPECT_EQ(values(r.view), (Values{1, 1, 2}));
  }

  // Ceiling and Floor
  {
    const auto c = run("1 5 3 ⌈ 4 2 3");
    EXPECT_EQ(values(c.view), (Values{4, 5, 3}));
    const auto f = run("1 5 3 ⌊ 4 2 3");
    EXPECT_EQ(values(f.view), (Values{1, 2, 3}));
  }

  // Comparisons
  {
    const auto eq = run("1 5 3 = 1 2 3");
    EXPECT_EQ(values(eq.view), (Values{1, 0, 1}));
    const auto ne = run("1 5 3 ≠ 1 2 3");
    EXPECT_EQ(values(ne.view), (Values{0, 1, 0}));
    const auto le = run("1 5 3 ≤ 2 5 2");
    EXPECT_EQ(values(le.view), (Values{1, 1, 0}));
    const auto ge = run("1 5 3 ≥ 1 2 4");
    EXPECT_EQ(values(ge.view), (Values{1, 1, 0}));
  }

  // Logical And / Or
  {
    const auto a = run("0 1 0 ∧ 1 1 0");
    EXPECT_EQ(values(a.view), (Values{0, 1, 0}));
    const auto o = run("0 1 0 ∨ 1 0 0");
    EXPECT_EQ(values(o.view), (Values{1, 1, 0}));
  }

  // J syntax
  {
    const auto c = run("1 5 3 >. 4 2 3");
    EXPECT_EQ(values(c.view), (Values{4, 5, 3}));
    const auto f = run("1 5 3 <. 4 2 3");
    EXPECT_EQ(values(f.view), (Values{1, 2, 3}));
    const auto ne = run("1 2 3 ~= 1 0 3");
    EXPECT_EQ(values(ne.view), (Values{0, 1, 0}));
    const auto le = run("1 5 3 <= 2 5 2");
    EXPECT_EQ(values(le.view), (Values{1, 1, 0}));
    const auto ge = run("1 5 3 >= 1 2 4");
    EXPECT_EQ(values(ge.view), (Values{1, 1, 0}));
    const auto a = run("0 1 0 *. 1 1 0");
    EXPECT_EQ(values(a.view), (Values{0, 1, 0}));
    const auto o = run("0 1 0 +. 1 0 0");
    EXPECT_EQ(values(o.view), (Values{1, 1, 0}));
  }
}

TEST_F(VplLowerElementwiseTest, ScalarExtensionBothSides) {
  // Scalar + Rank
  {
    const auto r = run("10 + 1 2 3");
    ASSERT_EQ(r.view.axes().size(), 1U);
    EXPECT_EQ(values(r.view), (Values{11, 12, 13}));
  }

  // Rank + Scalar
  {
    const auto r = run("1 2 3 + 10");
    ASSERT_EQ(r.view.axes().size(), 1U);
    EXPECT_EQ(values(r.view), (Values{11, 12, 13}));
  }

  // Scalar - Rank
  {
    const auto r = run("10 - 1 2 3");
    EXPECT_EQ(values(r.view), (Values{9, 8, 7}));
  }

  // Rank - Scalar
  {
    const auto r = run("1 2 3 - 10");
    EXPECT_EQ(values(r.view), (Values{-9, -8, -7}));
  }

  // Scalar * Rank and Rank * Scalar
  {
    const auto r1 = run("3 * 1 2 3");
    EXPECT_EQ(values(r1.view), (Values{3, 6, 9}));
    const auto r2 = run("1 2 3 * 3");
    EXPECT_EQ(values(r2.view), (Values{3, 6, 9}));
  }
}

TEST_F(VplLowerElementwiseTest, ReversedViewsInRankArithmetic) {
  // A is 1..5 on d.1; ⌽A is a reversed view (5..1)
  run("A ← 5⍳d.1");

  // Monadic negation on reversed view
  {
    const auto r = run("- ⌽A");
    ASSERT_EQ(r.view.axes().size(), 1U);
    EXPECT_EQ(values(r.view), (Values{-5, -4, -3, -2, -1}));
    // Result is a freshly minted forward rank, not a reversed view.
    EXPECT_FALSE(r.view.isReversed(0));
  }

  // A + ⌽A: lockstep zip of forward and reversed ranks
  // 1+5=6, 2+4=6, 3+3=6, 4+2=6, 5+1=6
  {
    const auto r = run("A + ⌽A");
    ASSERT_EQ(r.view.axes().size(), 1U);
    EXPECT_EQ(values(r.view), (Values{6, 6, 6, 6, 6}));
  }

  // 10 + ⌽A
  {
    const auto r = run("10 + ⌽A");
    EXPECT_EQ(values(r.view), (Values{15, 14, 13, 12, 11}));
  }

  // (⌽A) - A: 5-1=4, 4-2=2, 3-3=0, 2-4=-2, 1-5=-4
  {
    const auto r = run("(⌽A) - A");
    EXPECT_EQ(values(r.view), (Values{4, 2, 0, -2, -4}));
  }

  // J syntax reverse |.
  {
    const auto r = run("A + |. A");
    EXPECT_EQ(values(r.view), (Values{6, 6, 6, 6, 6}));
  }
}

TEST_F(VplLowerElementwiseTest, LengthMismatchFailsRuntimeAssertion) {
  // Mismatched lengths on dynamic ranks
  expectRuntimeError("(3⍳d.1) + (4⍳d.1)", "length error");

  // Mismatched lengths on literal vectors
  expectRuntimeError("1 2 3 + 1 2 3 4", "length error");

  // Mismatched lengths with reverse
  expectRuntimeError("A ← 3⍳d.1; B ← 4⍳d.1; (⌽A) + B", "length error");
}

TEST_F(VplLowerElementwiseTest, RefusesUnsupportedOperandsAndVerbs) {
  // Matrix operand (valence > 1)
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

  expectCompileRefusal("M + 1", "more than one axis");
  expectCompileRefusal("1 + M", "more than one axis");
  expectCompileRefusal("M + M", "more than one axis");
  expectCompileRefusal("- M", "more than one axis");

  // Dimension value
  expectCompileRefusal("d.1 + 1 2 3", "dimension value");
  expectCompileRefusal("1 2 3 + d.1", "dimension value");
  expectCompileRefusal("- d.1", "dimension value");

  // Unsupported dyadic verb on ranks (e.g. power)
  expectCompileRefusal("2 ^ (1 2 3)", "dyadic");
  expectCompileRefusal("(1 2 3) ^ 2", "dyadic");
}

} // namespace
} // namespace xanadu::vpl
