/**
 * @file vpl_lower_outer_test.cpp
 * @brief Card V9: outer product runs on the Vortex VM as a 2-valence grid.
 */
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

class VplOuterProductTest : public ::testing::Test {
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

TEST_F(VplOuterProductTest, OuterProductProducesTwoDimensionalGrid) {
  const auto r = run("(1 2 3) ∘.+ (10 20)");
  EXPECT_FALSE(r.view.isScalar());
  EXPECT_EQ(r.view.valence(), 2U);
  const auto sh = r.view.shape(arena);
  ASSERT_EQ(sh.size(), 2U);
  EXPECT_EQ(sh[0], 3U);
  EXPECT_EQ(sh[1], 2U);

  ASSERT_EQ(r.view.axes().size(), 2U);
  EXPECT_EQ(r.view.axes()[0].dim, dim("d.1"));
  EXPECT_EQ(r.view.axes()[1].dim, dim("d.2"));

  // Check the values at coordinates (row, col)
  const auto origin = r.view.origin();
  ASSERT_TRUE(origin.has_value());
  EXPECT_EQ(arena.asInt64(*origin), 11); // 1 + 10

  const auto col1 = arena.linked(*origin, dim("d.2"), DimVector::POS);
  ASSERT_TRUE(col1.has_value());
  EXPECT_EQ(arena.asInt64(*col1), 21); // 1 + 20

  const auto row1 = arena.linked(*origin, dim("d.1"), DimVector::POS);
  ASSERT_TRUE(row1.has_value());
  EXPECT_EQ(arena.asInt64(*row1), 12); // 2 + 10

  const auto row1col1 = arena.linked(*row1, dim("d.2"), DimVector::POS);
  ASSERT_TRUE(row1col1.has_value());
  EXPECT_EQ(arena.asInt64(*row1col1), 22); // 2 + 20

  const auto row2 = arena.linked(*row1, dim("d.1"), DimVector::POS);
  ASSERT_TRUE(row2.has_value());
  EXPECT_EQ(arena.asInt64(*row2), 13); // 3 + 10

  const auto row2col1 = arena.linked(*row2, dim("d.2"), DimVector::POS);
  ASSERT_TRUE(row2col1.has_value());
  EXPECT_EQ(arena.asInt64(*row2col1), 23); // 3 + 20
}

TEST_F(VplOuterProductTest, DualAsciiSyntaxMatchesUnicode) {
  const auto r = run("(1 2 3) o.+ (10 20)");
  EXPECT_EQ(r.view.valence(), 2U);
  const auto sh = r.view.shape(arena);
  ASSERT_EQ(sh.size(), 2U);
  EXPECT_EQ(sh[0], 3U);
  EXPECT_EQ(sh[1], 2U);
}

TEST_F(VplOuterProductTest, OtherScalarVerbsWorkInOuterProduct) {
  const auto r = run("(1 2) ∘.× (10 20 30)");
  EXPECT_EQ(r.view.valence(), 2U);
  const auto sh = r.view.shape(arena);
  ASSERT_EQ(sh.size(), 2U);
  EXPECT_EQ(sh[0], 2U);
  EXPECT_EQ(sh[1], 3U);

  const auto origin = r.view.origin();
  ASSERT_TRUE(origin.has_value());
  EXPECT_EQ(arena.asInt64(*origin), 10); // 1 * 10
}

TEST_F(VplOuterProductTest, OuterProductWithReversedOperandReadsReversed) {
  const auto r = run("(⌽ 1 2) ∘.+ (10 20)");
  EXPECT_EQ(r.view.valence(), 2U);
  const auto origin = r.view.origin();
  ASSERT_TRUE(origin.has_value());
  EXPECT_EQ(arena.asInt64(*origin), 12); // 2 + 10
}

TEST_F(VplOuterProductTest, UnsupportedFormsAreRefusedByName) {
  expectCompileRefusal("d.1 ∘.+ (1 2)", "dimension value");
  expectCompileRefusal("(1 2) ∘.+ d.1", "dimension value");
  expectCompileRefusal("2 3 ⍴ (1 2 3 4 5 6)", "dyadic reshape is unsupported");
}

} // namespace
} // namespace xanadu::vpl
