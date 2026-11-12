/**
 * @file vpl_lower_iota_test.cpp
 * @brief Card V3: iota and monadic shape run as graphs on the Vortex VM.
 */
#include <cstdint>
#include <gtest/gtest.h>
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

  ExecutionResult run(const std::string &source,
                      std::size_t budget = kBudget) {
    auto result = executor->execute(source, budget);
    EXPECT_TRUE(result.has_value())
        << source << ": "
        << (result ? "" : result.error().message);
    if (!result) return ExecutionResult{};
    return std::move(*result);
  }

  std::vector<std::int64_t> values(const VplView &view) {
    std::vector<std::int64_t> out;
    for (const auto cell : view.collectCells(arena)) {
      out.push_back(arena.asInt64(cell).value_or(-999));
    }
    return out;
  }

  zigzag::ArenaManifold arena;
  std::unique_ptr<VortexCore> core;
  std::unique_ptr<VortexVM> vm;
  std::unique_ptr<Executor> executor;
};

TEST_F(VplIotaTest, PrototypeFiveIotaOnDimensionOne) {
  const auto result = run("5⍳d.1");
  EXPECT_EQ(values(result.view), (std::vector<std::int64_t>{1, 2, 3, 4, 5}));
  EXPECT_EQ(result.view.valence(), 1U);
}

} // namespace
} // namespace xanadu::vpl
