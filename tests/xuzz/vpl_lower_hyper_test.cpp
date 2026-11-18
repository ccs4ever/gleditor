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

#include "common/xanadu/store.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vpl/compiler.hpp"
#include "common/xanadu/vpl/execution.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/manifold.hpp"

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

// The semantics under test, as the VM defines them (Hypertime opcode): the
// result is the evaluation-time operation high-water mark and does not depend
// on the operand's value. Over a store it is the base manifold's folded-through
// operation index; over a bare arena every cell is an operation, so it is the
// arena's cell count when the opcode runs, which includes the cells the
// compiled program itself occupies.
TEST_F(VplHypertimeTest, OperandValueDoesNotChangeTheResultOverAStore) {
  xanadu::Store store;
  auto at           = store.sliceGenesis(xanadu::MicroversionId{});
  const auto minted = store.makeDimension(at, "d.step");
  at                = minted.version;
  at                = store.makeCell(at, "first");
  const auto base   = store.rebuildManifold(at);

  zigzag::ArenaManifold backed(&base, &store);
  VortexCore backedCore(backed);
  VortexVM backedVm(backedCore);
  Executor backedExecutor(backedCore, backedVm);
  const auto expected = static_cast<std::int64_t>(store.opCount());
  for (const auto *source : {"⍟ 5", "⍟ 1 2 3", "⍟ ⍳0", "time. 5 i. d.1"}) {
    const auto result = backedExecutor.execute(source, kBudget);
    ASSERT_TRUE(result.has_value())
        << source << ": " << (result ? "" : result.error().message);
    EXPECT_TRUE(result->view.isScalar()) << source;
    EXPECT_EQ(result->view.scalarInt(), expected) << source;
  }
}

TEST_F(VplHypertimeTest, ANewOperationMovesTheMarkOverAStore) {
  xanadu::Store store;
  auto at                = store.sliceGenesis(xanadu::MicroversionId{});
  const auto minted      = store.makeDimension(at, "d.step");
  at                     = minted.version;
  at                     = store.makeCell(at, "first");
  const auto before      = store.rebuildManifold(at);
  const auto beforeCount = static_cast<std::int64_t>(store.opCount());
  at                     = store.makeCell(at, "second");
  const auto after       = store.rebuildManifold(at);

  const auto evaluate = [&](const zigzag::Manifold &base) {
    zigzag::ArenaManifold backed(&base, &store);
    VortexCore backedCore(backed);
    VortexVM backedVm(backedCore);
    Executor backedExecutor(backedCore, backedVm);
    const auto result = backedExecutor.execute("⍟ 1 2 3", kBudget);
    EXPECT_TRUE(result.has_value());
    return result ? result->view.scalarInt() : std::int64_t{-1};
  };
  EXPECT_EQ(evaluate(before), beforeCount);
  EXPECT_EQ(evaluate(after), static_cast<std::int64_t>(store.opCount()));
  EXPECT_NE(evaluate(before), evaluate(after));
}

TEST_F(VplHypertimeTest, OverABareArenaItIsTheCellCountWhenTheOpcodeRuns) {
  const auto first = run("⍟ 5");
  ASSERT_TRUE(first.view.isScalar());
  const auto firstMark = first.view.scalarInt();
  EXPECT_LE(static_cast<std::size_t>(firstMark), arena.cellCount());

  constexpr int kMinted = 64;
  for (int i = 0; i < kMinted; ++i) (void)arena.makeCell();
  const auto second = run("⍟ 5");
  // Both programs are the same size, so what separates the two marks is
  // exactly the cells minted between them plus the first program's own.
  EXPECT_GE(second.view.scalarInt(), firstMark + kMinted);
}

TEST_F(VplHypertimeTest, TheOperandIsEvaluatedForEffectAndItsValueIgnored) {
  const auto result = executor->execute("⍟ (10 20 30)[7]", kBudget);
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().message.find("index out of range"),
            std::string::npos)
      << result.error().message;
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
