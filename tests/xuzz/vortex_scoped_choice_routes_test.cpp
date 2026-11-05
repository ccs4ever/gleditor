#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vortex/program_export.hpp"
#include "common/xanadu/vortex/vortex_stdlib.hpp"

namespace {
using zigzag::CellRef;
using zigzag::DimVector;
using namespace zigzag::vortex;

struct Harness {
  zigzag::ArenaManifold arena;
  VortexCore core{arena};
  VortexVM vm{core};
  std::vector<CellRef> opcodes;

  CellRef op(OpcodeKind kind) {
    const auto cell = vm.mintOpcode(kind);
    opcodes.push_back(cell);
    return cell;
  }
  void next(CellRef from, CellRef to) {
    zigzag::expectWritten(
        arena.link(from, core.dims().spin, DimVector::POS, to));
  }
  CellRef scope(CellRef target) {
    const auto cell = op(OpcodeKind::ScopedChoice);
    core.bindInput(cell, target);
    return cell;
  }
  CellRef add(CellRef output) {
    const auto cell = op(OpcodeKind::Add);
    core.bindInput(cell, arena.makeScalarCell(std::int64_t{20}));
    core.bindInput(cell, arena.makeScalarCell(std::int64_t{22}));
    core.bindOutput(cell, output);
    return cell;
  }
};

TEST(VortexScopedChoiceRoutesTest, GraphTargetChainRunsOnlyAfterRollback) {
  Harness h;
  const auto output = h.arena.makeScalarCell(std::int64_t{99});
  const auto target = h.add(output);
  const auto halt   = h.op(OpcodeKind::Halt);
  const auto scoped = h.scope(target);
  const auto fail   = h.op(OpcodeKind::Fail);
  h.next(scoped, fail);
  h.next(target, halt);
  const auto cursor = h.vm.spawnCursor(scoped);
  ASSERT_TRUE(h.vm.step(cursor).success);
  EXPECT_EQ(h.arena.asInt64(output), 99);
  ASSERT_TRUE(h.vm.step(cursor).success);
  EXPECT_EQ(h.vm.getCursorOpcode(cursor), target);
  EXPECT_EQ(h.arena.asInt64(output), 99);
  const auto execution = h.vm.run(cursor);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  EXPECT_EQ(h.arena.asInt64(output), 42);
  EXPECT_EQ(h.arena.outstandingMarks(), 0u);
}

TEST(VortexScopedChoiceRoutesTest,
     ValidationSuccessRollsBackThenCutsOuterScope) {
  Harness h;
  const auto callerAlternative = h.op(OpcodeKind::Halt);
  const auto failure           = h.op(OpcodeKind::Halt);
  const auto halt              = h.op(OpcodeKind::Halt);
  const auto depth             = h.arena.makeCell();
  const auto getDepth          = h.op(OpcodeKind::ChoiceDepth);
  h.core.bindOutput(getDepth, depth);
  const auto cut = h.op(OpcodeKind::Cut);
  h.core.bindInput(cut, depth);
  h.next(cut, halt);
  const auto outer   = h.scope(failure);
  const auto inner   = h.scope(cut);
  const auto fail    = h.op(OpcodeKind::Fail);
  const auto output  = h.arena.makeScalarCell(std::int64_t{99});
  const auto attempt = h.add(output);
  h.next(getDepth, outer);
  h.next(outer, inner);
  h.next(inner, attempt);
  h.next(attempt, fail);
  const auto cursor = h.vm.spawnCursor(getDepth);
  h.vm.pushChoicePoint(cursor, callerAlternative);
  const auto execution = h.vm.run(cursor);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  EXPECT_EQ(h.arena.asInt64(output), 99);
  EXPECT_EQ(h.vm.choiceDepth(), 1u);
  ASSERT_TRUE(h.vm.backtrack(cursor));
  EXPECT_EQ(h.vm.getCursorOpcode(cursor), callerAlternative);
  EXPECT_EQ(h.arena.outstandingMarks(), 0u);
}

TEST(VortexScopedChoiceRoutesTest, CrossCursorAndUnclosedScopeRefuse) {
  Harness h;
  const auto halt   = h.op(OpcodeKind::Halt);
  const auto scoped = h.scope(halt);
  h.next(scoped, halt);
  const auto owner = h.vm.spawnCursor(scoped);
  const auto other = h.vm.spawnCursor(halt);
  ASSERT_TRUE(h.vm.step(owner).success);
  EXPECT_FALSE(h.vm.step(other).success);
  EXPECT_THROW(h.vm.backtrack(other), std::invalid_argument);
  EXPECT_THROW(h.vm.pushChoicePoint(other, halt), std::invalid_argument);
  EXPECT_EQ(h.vm.choiceDepth(), 1u);
  EXPECT_EQ(h.arena.outstandingMarks(), 1u);
  EXPECT_FALSE(h.vm.run(owner).success);
  h.vm.setCursorOpcode(owner, std::nullopt);
  EXPECT_FALSE(h.vm.run(owner).success);
  ASSERT_TRUE(h.vm.backtrack(owner));
  EXPECT_TRUE(h.vm.run(owner).success);
  EXPECT_EQ(h.arena.outstandingMarks(), 0u);
}

TEST(VortexScopedChoiceRoutesTest, UnsupportedRoutesRefuseBeforeWrites) {
  for (int mode = 0; mode < 5; ++mode) {
    Harness h;
    const auto output   = h.arena.makeScalarCell(std::int64_t{99});
    const auto target   = h.op(OpcodeKind::Halt);
    const auto scoped   = h.scope(target);
    const auto ordinary = h.add(output);
    CellRef entry       = scoped;
    if (mode == 0) h.core.attachPrecondition(scoped, h.op(OpcodeKind::Nop));
    if (mode == 1) h.vm.enableMemoization(scoped, "scope-rejected");
    if (mode == 2) {
      h.core.attachPostcondition(ordinary, scoped);
      entry = ordinary;
    }
    if (mode == 3) {
      h.core.attachPipeline(h.core.inputsOf(ordinary).front(), scoped);
      entry = ordinary;
    }
    if (mode == 4) {
      VortexStdLib stdlib(h.core, h.vm);
      EXPECT_THROW(stdlib.call(scoped, {}), std::invalid_argument);
    } else {
      const auto cursor = h.vm.spawnCursor(entry);
      EXPECT_FALSE(h.vm.step(cursor).success) << mode;
      EXPECT_EQ(h.vm.getCursorOpcode(cursor), entry);
    }
    EXPECT_EQ(h.arena.asInt64(output), 99);
    EXPECT_EQ(h.vm.choiceDepth(), 0u);
    EXPECT_EQ(h.arena.outstandingMarks(), 0u);
  }
}

TEST(VortexScopedChoiceRoutesTest, SnapshotBudgetRefusesBeforeTakingMark) {
  Harness h;
  const auto halt   = h.op(OpcodeKind::Halt);
  const auto scoped = h.scope(halt);
  const auto clear  = h.op(OpcodeKind::StreamClear);
  const auto stream = h.arena.makeCell();
  h.core.bindOutput(clear, stream);
  h.next(clear, scoped);
  const auto cursor = h.vm.spawnCursor(clear);
  ASSERT_TRUE(h.vm.step(cursor).success);
  const auto before = h.vm.resultCells(stream);
  h.vm.setScopedChoiceLimits({.maxDepth = 128, .maxSnapshotEntries = 1});
  EXPECT_FALSE(h.vm.step(cursor).success);
  EXPECT_EQ(h.vm.resultCells(stream), before);
  EXPECT_EQ(h.vm.getCursorOpcode(cursor), scoped);
  EXPECT_EQ(h.vm.choiceDepth(), 0u);
  EXPECT_EQ(h.arena.outstandingMarks(), 0u);
}

TEST(VortexScopedChoiceRoutesTest, NestedOrdinaryChoiceCanStartStream) {
  Harness h;
  const auto halt   = h.op(OpcodeKind::Halt);
  const auto scoped = h.scope(halt);
  const auto clear  = h.op(OpcodeKind::StreamClear);
  const auto stream = h.arena.makeCell();
  h.core.bindOutput(clear, stream);
  h.next(scoped, clear);
  const auto cursor = h.vm.spawnCursor(scoped);
  ASSERT_TRUE(h.vm.step(cursor).success);
  h.vm.pushChoicePoint(cursor, halt);
  ASSERT_TRUE(h.vm.step(cursor).success);
  EXPECT_TRUE(h.vm.resultCells(stream).empty());
  ASSERT_TRUE(h.vm.backtrack(cursor));
  ASSERT_TRUE(h.vm.backtrack(cursor));
  EXPECT_EQ(h.arena.outstandingMarks(), 0u);
}

TEST(VortexScopedChoiceRoutesTest,
     CuttingNestedOrdinaryChoiceDoesNotPruneWorkers) {
  Harness h;
  const auto halt   = h.op(OpcodeKind::Halt);
  const auto scoped = h.scope(halt);
  const auto cursor = h.vm.spawnCursor(scoped);
  std::vector<std::string> pruned;
  h.vm.setCutCallback(
      [&](std::string_view path) { pruned.emplace_back(path); });
  h.vm.pushChoicePoint(cursor, halt);
  ASSERT_TRUE(h.vm.step(cursor).success);
  h.vm.pushChoicePoint(cursor, halt);
  h.vm.cut(1);
  EXPECT_TRUE(pruned.empty());
  EXPECT_EQ(h.vm.choiceDepth(), 1u);
  h.vm.cut(0);
  EXPECT_FALSE(pruned.empty());
  EXPECT_EQ(h.arena.outstandingMarks(), 0u);
}

TEST(VortexScopedChoiceRoutesTest, CallerPrefixCountsTowardTotalDepthLimit) {
  Harness h;
  const auto halt   = h.op(OpcodeKind::Halt);
  const auto scoped = h.scope(halt);
  const auto cursor = h.vm.spawnCursor(scoped);
  h.vm.pushChoicePoint(cursor, halt);
  h.vm.setScopedChoiceLimits({.maxDepth = 1});
  const auto refused = h.vm.step(cursor);
  EXPECT_FALSE(refused.success);
  EXPECT_NE(refused.errorMessage.find("depth"), std::string::npos);
  EXPECT_EQ(h.vm.choiceDepth(), 1u);
  EXPECT_EQ(h.vm.getCursorOpcode(cursor), scoped);
  EXPECT_EQ(h.arena.outstandingMarks(), 1u);
  ASSERT_TRUE(h.vm.backtrack(cursor));
  EXPECT_EQ(h.arena.outstandingMarks(), 0u);
}

TEST(VortexScopedChoiceRoutesTest,
     FailedValidationDiscardsInnerThenRollsBackOuter) {
  Harness h;
  const auto callerAlternative = h.op(OpcodeKind::Halt);
  const auto failure           = h.op(OpcodeKind::Halt);
  const auto success           = h.op(OpcodeKind::Halt);
  const auto outer             = h.scope(failure);
  const auto inner             = h.scope(success);
  const auto innerDepth        = h.arena.makeCell();
  const auto getDepth          = h.op(OpcodeKind::ChoiceDepth);
  h.core.bindOutput(getDepth, innerDepth);
  const auto output   = h.arena.makeScalarCell(std::int64_t{99});
  const auto attempt  = h.add(output);
  const auto cutInner = h.op(OpcodeKind::Cut);
  h.core.bindInput(cutInner, innerDepth);
  const auto fail = h.op(OpcodeKind::Fail);
  h.next(outer, getDepth);
  h.next(getDepth, inner);
  h.next(inner, attempt);
  h.next(attempt, cutInner);
  h.next(cutInner, fail);
  const auto cursor = h.vm.spawnCursor(outer);
  h.vm.pushChoicePoint(cursor, callerAlternative);
  const auto execution = h.vm.run(cursor);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  EXPECT_EQ(h.vm.getCursorOpcode(cursor), failure);
  EXPECT_EQ(h.arena.asInt64(output), 99);
  EXPECT_EQ(h.vm.choiceDepth(), 1u);
  ASSERT_TRUE(h.vm.backtrack(cursor));
  EXPECT_EQ(h.vm.getCursorOpcode(cursor), callerAlternative);
  EXPECT_EQ(h.arena.outstandingMarks(), 0u);
}

struct TempDirectory {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("vortex_scoped_" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  TempDirectory() { std::filesystem::create_directories(path); }
  ~TempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

TEST(VortexScopedChoiceRoutesTest, GraphReloadsAfterSourceVmDestruction) {
  TempDirectory temporary;
  auto scroll = std::make_shared<xanadu::UserPermascroll>(
      xanadu::UserPermascroll::Config{.storageDir = temporary.path / "scroll"});
  {
    Harness h;
    const auto halt        = h.op(OpcodeKind::Halt);
    const auto alternative = h.op(OpcodeKind::Nop);
    const auto scoped      = h.scope(alternative);
    const auto fail        = h.op(OpcodeKind::Fail);
    h.next(scoped, fail);
    h.next(alternative, halt);
    h.next(h.core.home(), scoped);
    xanadu::Store store(scroll);
    (void)exportProgram(h.core, h.vm, h.opcodes, store);
    store.save((temporary.path / "program").string());
    const auto cursor = h.vm.spawnCursor(scoped);
    ASSERT_TRUE(h.vm.step(cursor).success);
    const auto before = store.latest();
    EXPECT_THROW((void)exportProgram(h.core, h.vm, h.opcodes, store),
                 std::runtime_error);
    EXPECT_EQ(store.latest(), before);
    h.vm.clearChoicePoints();
  }
  xanadu::Store store(scroll);
  store.load((temporary.path / "program").string());
  const auto manifold = store.rebuildManifold(*store.structureHead());
  zigzag::ArenaManifold arena(&manifold, store);
  VortexCore core(arena);
  VortexVM vm(core);
  const auto entry =
      arena.linked(core.home(), core.dims().spin, DimVector::POS);
  ASSERT_TRUE(entry);
  const auto cursor    = vm.spawnCursor(*entry);
  const auto execution = vm.run(cursor);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  EXPECT_EQ(vm.choiceDepth(), 0u);
  EXPECT_EQ(arena.outstandingMarks(), 0u);
}
} // namespace
