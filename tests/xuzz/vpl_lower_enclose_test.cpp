/**
 * @file vpl_lower_enclose_test.cpp
 * @brief Card V10: enclose and disclose on the Vortex VM.
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

class VplEncloseTest : public ::testing::Test {
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

TEST_F(VplEncloseTest, EncloseProducesEnclosedView) {
  const auto r = run("⊂ 1 2 3");
  EXPECT_TRUE(r.view.isEnclosed());
  ASSERT_NE(r.view.enclosedView(), nullptr);
  EXPECT_EQ(r.view.enclosedView()->shape(arena), (std::vector<std::size_t>{3}));
}

TEST_F(VplEncloseTest, EncloseScalarProducesEnclosedScalar) {
  const auto r = run("⊂ 42");
  EXPECT_TRUE(r.view.isEnclosed());
  ASSERT_NE(r.view.enclosedView(), nullptr);
  EXPECT_TRUE(r.view.enclosedView()->isScalar());
  EXPECT_EQ(r.view.enclosedView()->scalarInt(), 42);
}

TEST_F(VplEncloseTest, DiscloseUnboxesEnclosedView) {
  run("B ← ⊂ 1 2 3");
  const auto r = run("⊃ B");
  EXPECT_FALSE(r.view.isEnclosed());
  EXPECT_EQ(r.view.shape(arena), (std::vector<std::size_t>{3}));
}

TEST_F(VplEncloseTest, DiscloseOnNonEnclosedIsIdentity) {
  const auto r = run("⊃ 1 2 3");
  EXPECT_FALSE(r.view.isEnclosed());
  EXPECT_EQ(r.view.shape(arena), (std::vector<std::size_t>{3}));
}

TEST_F(VplEncloseTest, DimensionOperandsRefuseByName) {
  expectCompileRefusal("⊂ d.1", "dimension value");
  expectCompileRefusal("⊃ d.1", "dimension value");
}

} // namespace
} // namespace xanadu::vpl
