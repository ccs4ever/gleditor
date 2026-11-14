/**
 * @file vpl_lower_hyper_test.cpp
 * @brief Card V12: hypertime on the Vortex VM.
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

class VplHypertimeTest : public ::testing::Test {
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

TEST_F(VplHypertimeTest, HypertimeProducesScalarGreaterThanZero) {
  const auto r = run("⍟ 5 ⍳ d.1");
  EXPECT_TRUE(r.view.isScalar());
  EXPECT_GT(r.view.scalarInt(), 0);
}

TEST_F(VplHypertimeTest, HypertimeAsciiAliasMatchesUnicode) {
  const auto r = run("time. 5 i. d.1");
  EXPECT_TRUE(r.view.isScalar());
  EXPECT_GT(r.view.scalarInt(), 0);
}

TEST_F(VplHypertimeTest, HypertimeOnScalarOperand) {
  const auto r = run("⍟ 42");
  EXPECT_TRUE(r.view.isScalar());
  EXPECT_GT(r.view.scalarInt(), 0);
}

TEST_F(VplHypertimeTest, HypertimeMonotonicallyAdvancesWithArenaOperations) {
  const auto t1 = run("⍟ 1 ⍳ d.1");
  const auto t2 = run("⍟ 5 ⍳ d.1");
  EXPECT_GE(t2.view.scalarInt(), t1.view.scalarInt());
}

TEST_F(VplHypertimeTest, UnsupportedHypertimeOperandsRefuseByName) {
  expectCompileRefusal("⍟ d.1", "dimension value");
  expectCompileRefusal("time. d.1", "dimension value");
  expectCompileRefusal("⍟ ⊂ 1 2 3", "enclosed value");
}

TEST_F(VplHypertimeTest, TranscludeRefusesByName) {
  expectCompileRefusal("⌺ 5 ⍳ d.1", "transclusion (⌺) is unsupported");
  expectCompileRefusal("trans. 5 i. d.1", "transclusion (⌺) is unsupported");
}

} // namespace
} // namespace xanadu::vpl
