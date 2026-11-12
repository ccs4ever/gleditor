/**
 * @file vpl_lower_iota_test.cpp
 * @brief Card V3: iota and monadic shape run as graphs on the Vortex VM.
 */
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
using zigzag::vortex::OpcodeKind;
using zigzag::vortex::VortexCore;
using zigzag::vortex::VortexVM;

constexpr std::size_t kBudget = 200000;

class VplIotaTest : public ::testing::Test {
protected:
  void SetUp() override {
    core     = std::make_unique<VortexCore>(arena);
    vm       = std::make_unique<VortexVM>(*core);
    executor = std::make_unique<Executor>(*core, *vm);
  }

  ExecutionResult run(const std::string &source, std::size_t budget = kBudget) {
    auto result = executor->execute(source, budget);
    EXPECT_TRUE(result.has_value())
        << source << ": " << (result ? "" : result.error().message);
    if (!result) return ExecutionResult{};
    return std::move(*result);
  }

  // The integers of a view's cells in order, or nullopt when a cell holds no
  // integer; a test comparing against nullopt fails rather than matching a
  // placeholder value.
  std::optional<std::vector<std::int64_t>> values(const VplView &view) {
    std::vector<std::int64_t> out;
    for (const auto cell : view.collectCells(arena)) {
      const auto value = arena.asInt64(cell);
      if (!value) return std::nullopt;
      out.push_back(*value);
    }
    return out;
  }

  // Asserts that a program is refused, and that the refusal names `text`.
  void expectRefusal(const std::string &source, const std::string &text) {
    const auto result = executor->execute(source);
    ASSERT_FALSE(result.has_value()) << source;
    EXPECT_NE(result.error().message.find(text), std::string::npos)
        << source << ": " << result.error().message;
  }

  zigzag::ArenaManifold arena;
  std::unique_ptr<VortexCore> core;
  std::unique_ptr<VortexVM> vm;
  std::unique_ptr<Executor> executor;
};

using Values = std::vector<std::int64_t>;

Values countUp(std::int64_t n) {
  Values out;
  for (std::int64_t i = 1; i <= n; ++i) out.push_back(i);
  return out;
}

TEST_F(VplIotaTest, DyadicIotaMintsOneToNOnDimensionOneInBothSyntaxes) {
  for (const auto *source : {"5⍳d.1", "5 i. d.1"}) {
    const auto result = run(source);
    EXPECT_EQ(values(result.view), countUp(5)) << source;
    EXPECT_EQ(result.view.valence(), 1U);
    EXPECT_EQ(result.view.shape(arena), std::vector<std::size_t>{5});
  }
}

TEST_F(VplIotaTest, SizesZeroOneAndFive) {
  const auto empty = run("0⍳d.1");
  EXPECT_FALSE(empty.view.origin());
  EXPECT_EQ(empty.view.valence(), 1U);
  EXPECT_EQ(empty.view.shape(arena), std::vector<std::size_t>{0});
  EXPECT_EQ(values(run("1⍳d.1").view), countUp(1));
  EXPECT_EQ(values(run("5⍳d.1").view), countUp(5));
  // A negative count is a DOMAIN ERROR. The literal form is refused at compile
  // time; a computed count is checked by the VM when the program runs.
  expectRefusal("⍳ -3", "DOMAIN ERROR");
  const auto negative = executor->execute("n ← 0 - 3; n ⍳ d.1");
  ASSERT_FALSE(negative.has_value());
  EXPECT_EQ(negative.error().stage, ExecutionError::Stage::Runtime);
  EXPECT_NE(negative.error().message.find("DOMAIN ERROR"), std::string::npos)
      << negative.error().message;
}

TEST_F(VplIotaTest, CountComputedAtRunTimeDrivesTheLoop) {
  const auto result = run("n ← 2 + 2; n ⍳ d.1");
  EXPECT_EQ(values(result.view), countUp(4));
  // The same name bound on an earlier line reaches a later program.
  executor->setVariable("m", VplView::makeScalar(std::int64_t{3}));
  EXPECT_EQ(values(run("m ⍳ d.2").view), countUp(3));
}

TEST_F(VplIotaTest, AnyDimensionNotOnlyDimensionOne) {
  const auto result    = run("4⍳d.2");
  const auto dimension = core->findOrMintDimension("d.2");
  ASSERT_EQ(result.view.axes().size(), 1U);
  EXPECT_EQ(result.view.axes().front().dim, dimension);
  EXPECT_EQ(values(result.view), countUp(4));
  // Nothing was placed on d.1.
  const auto head = result.view.origin();
  ASSERT_TRUE(head);
  EXPECT_FALSE(
      arena.linked(*head, core->findOrMintDimension("d.1"), DimVector::POS));
}

TEST_F(VplIotaTest, ElementsAreLinkedBothWaysAlongTheDimension) {
  const auto result    = run("5⍳d.1");
  const auto dimension = core->findOrMintDimension("d.1");
  const auto cells     = result.view.collectCells(arena);
  ASSERT_EQ(cells.size(), 5U);
  for (std::size_t i = 1; i < cells.size(); ++i) {
    EXPECT_EQ(arena.linked(cells[i - 1], dimension, DimVector::POS), cells[i]);
    EXPECT_EQ(arena.linked(cells[i], dimension, DimVector::NEG), cells[i - 1]);
  }
  EXPECT_FALSE(arena.linked(cells.back(), dimension, DimVector::POS));
}

TEST_F(VplIotaTest, DyadicIotaAppendsAfterTheTailOfHomesRank) {
  const auto first  = run("2⍳d.1");
  const auto second = run("3⍳d.1");
  EXPECT_EQ(values(second.view), countUp(3));
  const auto dimension = core->findOrMintDimension("d.1");
  const auto before    = first.view.collectCells(arena).back();
  EXPECT_EQ(arena.linked(before, dimension, DimVector::POS),
            second.view.origin());
}

TEST_F(VplIotaTest, MonadicIotaInBothSyntaxes) {
  EXPECT_EQ(values(run("⍳ 4").view), countUp(4));
  EXPECT_EQ(values(run("i. 3").view), countUp(3));
  const auto empty = run("⍳ 0");
  EXPECT_FALSE(empty.view.origin());
  EXPECT_EQ(empty.view.valence(), 1U);
  EXPECT_EQ(values(run("n ← 6; ⍳ n").view), countUp(6));
}

TEST_F(VplIotaTest, ShapeOfEachIotaIsItsExtent) {
  EXPECT_EQ(values(run("⍴ 5⍳d.1").view), Values{5});
  EXPECT_EQ(values(run("$ 3 i. d.1").view), Values{3});
  EXPECT_EQ(values(run("⍴ ⍳ 7").view), Values{7});
  EXPECT_EQ(values(run("⍴ ⍴ 5⍳d.1").view), Values{1});
  EXPECT_EQ(values(run("⍴ 4⍳d.2").view), Values{4});
  // The shape of an empty array is its length, zero.
  EXPECT_EQ(values(run("⍴ 0⍳d.1").view), Values{0});
  // A scalar has no axes to measure: its shape is an empty rank-1 array.
  const auto scalar = run("⍴ 42");
  EXPECT_FALSE(scalar.view.isScalar());
  EXPECT_EQ(scalar.view.valence(), 1U);
  EXPECT_EQ(scalar.view.shape(arena), std::vector<std::size_t>{0});
  EXPECT_EQ(values(scalar.view), Values{});
}

TEST_F(VplIotaTest, ShapeOfABoundRankIsOneRankedNumberWithScalarPayload) {
  run("A ← 5⍳d.1");
  const auto shape = run("⍴A");
  EXPECT_EQ(shape.view.scalarInt(), 5);
  EXPECT_EQ(shape.view.valence(), 1U);
  EXPECT_EQ(run("⍴⍴A").view.scalarInt(), 1);
}

TEST_F(VplIotaTest, ShapeOfATwoAxisArrayListsOneExtentPerAxis) {
  const auto rows = core->findOrMintDimension("d.1");
  const auto cols = core->findOrMintDimension("d.2");
  std::vector<std::vector<CellRef>> grid;
  for (std::int64_t r = 0; r < 2; ++r) {
    grid.emplace_back();
    for (std::int64_t c = 0; c < 3; ++c) {
      grid.back().push_back(arena.makeScalarCell(r * 3 + c));
    }
  }
  for (std::size_t r = 0; r < 2; ++r) {
    for (std::size_t c = 0; c < 3; ++c) {
      if (c + 1 < 3) {
        zigzag::expectWritten(
            arena.link(grid[r][c], cols, DimVector::POS, grid[r][c + 1]));
      }
      if (r + 1 < 2) {
        zigzag::expectWritten(
            arena.link(grid[r][c], rows, DimVector::POS, grid[r + 1][c]));
      }
    }
  }
  executor->setVariable("M",
                        VplView{grid[0][0],
                                {zigzag::DirectedDim{rows, DimVector::POS},
                                 zigzag::DirectedDim{cols, DimVector::POS}}});
  const auto shape = run("⍴M");
  EXPECT_EQ(values(shape.view), (Values{2, 3}));
}

TEST_F(VplIotaTest, ABudgetTooSmallForNFailsAtTheBudgetStage) {
  const auto cheap = executor->execute("3⍳d.1", kBudget);
  EXPECT_TRUE(cheap.has_value());
  const auto dear = executor->execute("5000⍳d.1", 200);
  ASSERT_FALSE(dear.has_value());
  EXPECT_EQ(dear.error().stage, ExecutionError::Stage::Budget);
  // The cycles used grow with N: a budget that fits 3 does not fit 5000.
  const auto fits = executor->execute("3⍳d.1", 200);
  EXPECT_TRUE(fits.has_value());
}

TEST_F(VplIotaTest, CountersStayIntegersWhenNExceedsTheLiveCells) {
  // 3000 is far above the number of cells alive at the start, so a counter
  // that turned into a cell reference would show up as a wrong or absent
  // element.
  const auto result = run("3000⍳d.1", 5000000);
  const auto got    = values(result.view);
  ASSERT_TRUE(got.has_value());
  ASSERT_EQ(got->size(), 3000U);
  EXPECT_EQ(*got, countUp(3000));
  EXPECT_EQ(values(run("⍴ 3000⍳d.2", 5000000).view), Values{3000});
}

TEST_F(VplIotaTest, NonIntegerAndNegativeLiteralCountsAreRefusedAtCompile) {
  expectRefusal("2.5 ⍳ d.1", "non-integer count is a DOMAIN ERROR");
  expectRefusal("⍳ 2.5", "non-integer count is a DOMAIN ERROR");
  expectRefusal("¯3 ⍳ d.1", "negative count is a DOMAIN ERROR");
  expectRefusal("(-3) ⍳ d.1", "negative count is a DOMAIN ERROR");
  expectRefusal("⍳ ¯1", "negative count is a DOMAIN ERROR");
}

TEST_F(VplIotaTest, DimensionValueCountIsRefusedAsADimension) {
  expectRefusal("⍳ d.1", "iota of a dimension value is unsupported");
  expectRefusal("d.1 ⍳ d.1", "iota of a dimension value is unsupported");
}

TEST_F(VplIotaTest, UnsupportedFormsStillRefuseByName) {
  expectRefusal("1 2 ⍳ d.1", "iota of a rank argument");
  expectRefusal("⍳ 2 3", "iota of a rank argument");
  expectRefusal("5 ⍳ 3", "single dimension");
  // A literal enclosure is refused by the enclose lowering before the shape
  // sees it; an enclosed variable reaches the shape refusal in full.
  expectRefusal("⍴ ⊂ 1 2 3", "unsupported VPL monadic verb in VM compiler");
  VplView boxed = VplView::makeScalar(std::int64_t{0});
  boxed.enclose(
      std::make_shared<VplView>(VplView::makeScalar(std::int64_t{1})));
  executor->setVariable("E", boxed);
  expectRefusal(
      "⍴ E", "VPL monadic shape of an enclosed value is unsupported by the VM");
}

TEST_F(VplIotaTest, ZeroCountOnANonEmptyHomeRankLinksNothing) {
  const auto first = run("3⍳d.1");
  const auto cells = first.view.collectCells(arena);
  ASSERT_EQ(cells.size(), 3U);
  const auto dimension = core->findOrMintDimension("d.1");
  const auto empty     = run("0⍳d.1");
  EXPECT_FALSE(empty.view.origin());
  EXPECT_EQ(empty.view.valence(), 1U);
  EXPECT_FALSE(arena.linked(cells.back(), dimension, DimVector::POS));
  EXPECT_EQ(values(first.view), countUp(3));
}

} // namespace
} // namespace xanadu::vpl
