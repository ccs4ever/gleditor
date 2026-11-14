/**
 * @file vpl_lower_index_test.cpp
 * @brief Card V11: indexing, grade, where, match on the Vortex VM.
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

class VplIndexTest : public ::testing::Test {
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

  std::vector<std::int64_t> values(const VplView &view) {
    std::vector<std::int64_t> out;
    for (const auto cell : view.collectCells(arena)) {
      const auto value = arena.asInt64(cell);
      if (value) out.push_back(*value);
    }
    return out;
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

TEST_F(VplIndexTest, IndexingPicksElementByZeroBasedOffset) {
  run("A ← 10 20 30 40 50");
  const auto r0 = run("A[0]");
  EXPECT_TRUE(r0.view.isScalar());
  EXPECT_EQ(r0.view.scalarInt(), 10);

  const auto r2 = run("A[2]");
  EXPECT_TRUE(r2.view.isScalar());
  EXPECT_EQ(r2.view.scalarInt(), 30);

  const auto r4 = run("A[4]");
  EXPECT_TRUE(r4.view.isScalar());
  EXPECT_EQ(r4.view.scalarInt(), 50);
}

TEST_F(VplIndexTest, VectorIndexingPicksMultipleElements) {
  run("A ← 10 20 30 40 50");
  const auto r = run("A[1 3]");
  EXPECT_FALSE(r.view.isScalar());
  EXPECT_EQ(values(r.view), (std::vector<std::int64_t>{20, 40}));
}

TEST_F(VplIndexTest, GradeUpSortsIndicesAscending) {
  const auto r = run("⍋ 30 10 20");
  EXPECT_EQ(values(r.view), (std::vector<std::int64_t>{1, 2, 0}));
}

TEST_F(VplIndexTest, GradeDownSortsIndicesDescending) {
  const auto r = run("⍒ 30 10 20");
  EXPECT_EQ(values(r.view), (std::vector<std::int64_t>{0, 2, 1}));
}

TEST_F(VplIndexTest, GradeIsStableWithEqualElements) {
  const auto r = run("⍋ 20 10 20");
  EXPECT_EQ(values(r.view), (std::vector<std::int64_t>{1, 0, 2}));
}

TEST_F(VplIndexTest, WhereReturnsIndicesOfNonZeroElements) {
  const auto r = run("⍸ 0 1 0 1 1");
  EXPECT_EQ(values(r.view), (std::vector<std::int64_t>{1, 3, 4}));
}

TEST_F(VplIndexTest, MonadicMatchReturnsValence) {
  const auto rRank = run("≡ 1 2 3");
  EXPECT_TRUE(rRank.view.isScalar());
  EXPECT_EQ(rRank.view.scalarInt(), 1);

  const auto rScalar = run("≡ 42");
  EXPECT_TRUE(rScalar.view.isScalar());
  EXPECT_EQ(rScalar.view.scalarInt(), 0);
}

TEST_F(VplIndexTest, UnsupportedIndexingFormsRefuseByName) {
  expectCompileRefusal("(d.1)[0]", "dimension value");
  expectCompileRefusal("(42)[0]", "scalar");
  expectCompileRefusal("(1 2)[d.1]", "dimension value");
  expectCompileRefusal("(1 2)[¯1]", "negative index");
}

} // namespace
} // namespace xanadu::vpl
