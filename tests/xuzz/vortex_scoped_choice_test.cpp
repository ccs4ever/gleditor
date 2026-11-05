#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace zigzag::vortex {
namespace {

using zigzag::ArenaManifold;
using zigzag::CellRef;
using zigzag::DimVector;

struct Harness {
  ArenaManifold arena;
  VortexCore core{arena};
  VortexVM vm{core};

  CellRef op(const OpcodeKind kind) { return vm.mintOpcode(kind); }

  void next(const CellRef from, const CellRef to) {
    ASSERT_TRUE(arena.link(from, core.dims().spin, DimVector::POS, to));
  }

  void scopeTarget(const CellRef scoped, const CellRef target) {
    core.bindInput(scoped, target);
  }

  CellRef cursorAt(const CellRef entry) {
    return vm.spawnCursor(entry, "scoped-choice-test");
  }
};

TEST(VortexScopedChoiceTest,
     NestedRollbackRestoresFreshBoxAndKeepsCallerChoice) {
  Harness h;
  const CellRef callerAlternative = h.op(OpcodeKind::Halt);
  const CellRef outer             = h.op(OpcodeKind::ScopedChoice);
  const CellRef inner             = h.op(OpcodeKind::ScopedChoice);
  const CellRef box               = h.op(OpcodeKind::VqlBoxScalar);
  const CellRef failInner         = h.op(OpcodeKind::Fail);
  const CellRef failOuter         = h.op(OpcodeKind::Fail);
  const CellRef outerAlternative  = h.op(OpcodeKind::Halt);
  const CellRef input             = h.arena.makeScalarCell(std::int64_t{73});
  const CellRef output            = h.arena.makeCell("boxed output");

  h.scopeTarget(outer, outerAlternative);
  h.scopeTarget(inner, failOuter);
  h.core.bindInput(box, input);
  h.core.bindOutput(box, output);
  h.next(outer, inner);
  h.next(inner, box);
  h.next(box, failInner);
  const CellRef cursor = h.cursorAt(outer);
  h.vm.pushChoicePoint(cursor, callerAlternative);
  const auto baselineCells = h.arena.cells().size();

  ASSERT_TRUE(h.vm.step(cursor).success);
  ASSERT_EQ(h.vm.choiceDepth(), 2U);
  ASSERT_TRUE(h.vm.step(cursor).success);
  ASSERT_EQ(h.vm.choiceDepth(), 3U);
  ASSERT_TRUE(h.vm.step(cursor).success);
  ASSERT_TRUE(h.vm.cellReferenceOf(output));
  EXPECT_EQ(h.arena.cells().size(), baselineCells + 1);

  ASSERT_TRUE(h.vm.step(cursor).success) << "inner rollback";
  EXPECT_EQ(h.vm.getCursorOpcode(cursor), failOuter);
  EXPECT_EQ(h.vm.choiceDepth(), 2U);
  EXPECT_EQ(h.vm.cellReferenceOf(output), std::nullopt);
  EXPECT_EQ(h.arena.cells().size(), baselineCells);

  ASSERT_TRUE(h.vm.step(cursor).success) << "outer rollback";
  EXPECT_EQ(h.vm.getCursorOpcode(cursor), outerAlternative);
  EXPECT_EQ(h.vm.choiceDepth(), 1U);
  EXPECT_EQ(h.vm.cellReferenceOf(output), std::nullopt);
  ASSERT_TRUE(h.vm.backtrack(cursor));
  EXPECT_EQ(h.vm.choiceDepth(), 0U);
  EXPECT_EQ(h.vm.getCursorOpcode(cursor), callerAlternative);
}

TEST(VortexScopedChoiceTest, OracleDirectiveBelongsToOrdinaryChoice) {
  Harness h;
  const CellRef scoped              = h.op(OpcodeKind::ScopedChoice);
  const CellRef choice              = h.op(OpcodeKind::Choice);
  const CellRef afterScope          = h.op(OpcodeKind::Halt);
  const CellRef ordinaryAlternative = h.op(OpcodeKind::Fail);
  const CellRef scopeAlternative    = h.op(OpcodeKind::Halt);
  h.scopeTarget(scoped, scopeAlternative);
  h.core.bindInput(choice, h.arena.makeScalarCell(
                               static_cast<std::int64_t>(ordinaryAlternative)));
  h.next(scoped, choice);
  h.next(choice, afterScope);
  const CellRef cursor = h.cursorAt(scoped);
  const std::uint16_t oracle[]{1};
  const std::uint16_t parentPath[]{4, 2};
  h.vm.setOraclePath(oracle);
  h.vm.setCurrentDeweyPath(parentPath);

  ASSERT_TRUE(h.vm.step(cursor).success);
  EXPECT_EQ(h.vm.getCursorOpcode(cursor), choice);
  EXPECT_EQ(h.vm.choiceDepth(), 1U);
  ASSERT_TRUE(h.vm.step(cursor).success);
  EXPECT_EQ(h.vm.getCursorOpcode(cursor), ordinaryAlternative);
  const std::uint16_t branchedPath[]{4, 2, 1};
  EXPECT_TRUE(std::ranges::equal(h.vm.currentDeweyPath(), branchedPath));
  ASSERT_TRUE(h.vm.step(cursor).success);
  EXPECT_EQ(h.vm.getCursorOpcode(cursor), scopeAlternative);
  EXPECT_TRUE(std::ranges::equal(h.vm.currentDeweyPath(), parentPath));
  EXPECT_EQ(h.vm.choiceDepth(), 0U);
}

TEST(VortexScopedChoiceTest, ScopeCutDoesNotCallPruningCallbackOrCutCaller) {
  Harness h;
  const CellRef callerAlternative = h.op(OpcodeKind::Halt);
  const CellRef scoped            = h.op(OpcodeKind::ScopedChoice);
  const CellRef cut               = h.op(OpcodeKind::Cut);
  const CellRef halt              = h.op(OpcodeKind::Halt);
  const CellRef scopeAlternative  = h.op(OpcodeKind::Halt);
  h.scopeTarget(scoped, scopeAlternative);
  h.core.bindInput(cut, h.arena.makeScalarCell(std::int64_t{1}));
  h.next(scoped, cut);
  h.next(cut, halt);
  const CellRef cursor = h.cursorAt(scoped);
  std::vector<std::string> pruned;
  h.vm.setCutCallback([&pruned](const std::string_view prefix) {
    pruned.emplace_back(prefix);
  });
  h.vm.pushChoicePoint(cursor, callerAlternative);

  ASSERT_TRUE(h.vm.step(cursor).success);
  ASSERT_EQ(h.vm.choiceDepth(), 2U);
  ASSERT_TRUE(h.vm.step(cursor).success);
  EXPECT_EQ(h.vm.choiceDepth(), 1U);
  EXPECT_FALSE(h.vm.choiceStack().back().scoped);
  EXPECT_TRUE(pruned.empty());
  EXPECT_TRUE(h.vm.backtrack(cursor));
  EXPECT_EQ(h.vm.getCursorOpcode(cursor), callerAlternative);
}

TEST(VortexScopedChoiceTest, ChoiceStealingIsDisabledForEntireScope) {
  Harness h;
  const CellRef scoped           = h.op(OpcodeKind::ScopedChoice);
  const CellRef innerAlternative = h.op(OpcodeKind::Halt);
  const CellRef body             = h.op(OpcodeKind::Nop);
  const CellRef scopeAlternative = h.op(OpcodeKind::Halt);
  h.scopeTarget(scoped, scopeAlternative);
  h.next(scoped, body);
  const CellRef cursor = h.cursorAt(scoped);

  ASSERT_TRUE(h.vm.step(cursor).success);
  EXPECT_FALSE(h.vm.stealNearRootChoicePoint());
  h.vm.pushChoicePoint(cursor, innerAlternative);
  EXPECT_FALSE(h.vm.stealNearRootChoicePoint());
  ASSERT_TRUE(h.vm.backtrack(cursor));
  ASSERT_TRUE(h.vm.backtrack(cursor));
  EXPECT_EQ(h.vm.choiceDepth(), 0U);
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);
}

TEST(VortexScopedChoiceTest, StreamOwnershipCanBeginAndRollBackInsideScope) {
  Harness h;
  const CellRef scoped      = h.op(OpcodeKind::ScopedChoice);
  const CellRef clear       = h.op(OpcodeKind::StreamClear);
  const CellRef append      = h.op(OpcodeKind::StreamAppend);
  const CellRef fail        = h.op(OpcodeKind::Fail);
  const CellRef halt        = h.op(OpcodeKind::Halt);
  const CellRef alternative = h.op(OpcodeKind::Halt);
  const CellRef stream      = h.arena.makeCell("stream register");
  const CellRef item        = h.arena.makeCell("item");
  h.scopeTarget(scoped, alternative);
  h.core.bindOutput(clear, stream);
  h.core.bindInput(append, item);
  h.core.bindOutput(append, stream);
  h.next(scoped, clear);
  h.next(clear, append);
  h.next(append, fail);
  h.next(fail, halt);
  const CellRef cursor       = h.cursorAt(scoped);
  const auto originalResults = h.vm.resultCells(stream);

  const auto run = h.vm.run(cursor, 20);
  ASSERT_TRUE(run.success) << run.errorMessage;
  EXPECT_EQ(h.vm.resultCells(stream), originalResults);
  EXPECT_EQ(h.vm.choiceDepth(), 0U);
  EXPECT_EQ(h.vm.getCursorOpcode(cursor), alternative);
}

TEST(VortexScopedChoiceTest, DirectTargetChainRunsOnlyAfterRollback) {
  Harness h;
  const CellRef scoped     = h.op(OpcodeKind::ScopedChoice);
  const CellRef body       = h.op(OpcodeKind::Nop);
  const CellRef fail       = h.op(OpcodeKind::Fail);
  const CellRef marker     = h.op(OpcodeKind::Add);
  const CellRef markerHalt = h.op(OpcodeKind::Halt);
  const CellRef input      = h.arena.makeScalarCell(std::int64_t{19});
  const CellRef one        = h.arena.makeScalarCell(std::int64_t{1});
  const CellRef output     = h.arena.makeCell("marker output");
  h.scopeTarget(scoped, marker);
  h.core.bindInput(marker, input);
  h.core.bindInput(marker, one);
  h.core.bindOutput(marker, output);
  h.next(scoped, body);
  h.next(body, fail);
  h.next(marker, markerHalt);
  const CellRef cursor = h.cursorAt(scoped);

  ASSERT_TRUE(h.vm.step(cursor).success);
  EXPECT_EQ(h.vm.getCursorOpcode(cursor), body);
  EXPECT_EQ(h.core.render(output), CellValue{std::string{"marker output"}});
  ASSERT_TRUE(h.vm.step(cursor).success);
  ASSERT_TRUE(h.vm.step(cursor).success);
  EXPECT_EQ(h.vm.getCursorOpcode(cursor), marker);
  EXPECT_EQ(h.core.render(output), CellValue{std::string{"marker output"}});
  ASSERT_TRUE(h.vm.step(cursor).success);
  EXPECT_EQ(h.core.render(output), CellValue{std::int64_t{20}});
}

TEST(VortexScopedChoiceTest, RollbackRestoresNestedActivationSnapshot) {
  Harness h;
  const CellRef argument   = h.arena.makeCell("argument");
  const CellRef result     = h.arena.makeCell("result");
  const CellRef body       = h.op(OpcodeKind::CopyCellRefs);
  const CellRef ret        = h.op(OpcodeKind::ReturnFrame);
  const CellRef descriptor = *h.vm.makeFrameDescriptor(FrameDescriptorSpec{
      .entry         = body,
      .locals        = {argument, result},
      .argumentPorts = {argument},
      .resultPorts   = {result},
  });
  h.core.bindInput(body, argument);
  h.core.bindOutput(body, result);
  h.next(body, ret);
  h.core.bindInput(ret, descriptor);
  const CellRef scoped       = h.op(OpcodeKind::ScopedChoice);
  const CellRef invoke       = h.op(OpcodeKind::InvokeFrame);
  const CellRef fail         = h.op(OpcodeKind::Fail);
  const CellRef alternative  = h.op(OpcodeKind::Halt);
  const CellRef callerResult = h.arena.makeCell("caller result");
  const CellRef actual       = h.arena.makeScalarCell(std::int64_t{12});
  h.scopeTarget(scoped, alternative);
  h.core.bindInput(invoke, descriptor);
  h.core.bindInput(invoke, actual);
  h.core.bindOutput(invoke, callerResult);
  h.next(scoped, invoke);
  h.next(invoke, fail);
  const CellRef cursor       = h.cursorAt(scoped);
  const auto previousResults = h.vm.resultCells(cursor, callerResult);

  ASSERT_TRUE(h.vm.step(cursor).success);
  ASSERT_TRUE(h.vm.step(cursor).success);
  EXPECT_EQ(h.vm.activationDepth(cursor), 1U);
  ASSERT_TRUE(h.vm.step(cursor).success);
  EXPECT_EQ(h.vm.activationDepth(cursor), 1U);
  ASSERT_TRUE(h.vm.step(cursor).success);
  EXPECT_EQ(h.vm.activationDepth(cursor), 0U);
  EXPECT_TRUE(h.vm.resultCell(cursor, callerResult));
  ASSERT_TRUE(h.vm.step(cursor).success);
  EXPECT_EQ(h.vm.getCursorOpcode(cursor), alternative);
  EXPECT_EQ(h.vm.activationDepth(cursor), 0U);
  EXPECT_EQ(h.vm.resultCells(cursor, callerResult), previousResults);
  EXPECT_FALSE(h.vm.hasActiveFrames());
}

TEST(VortexScopedChoiceTest, ScopeDoesNotRefundExecutionWork) {
  Harness h;
  const CellRef scoped      = h.op(OpcodeKind::ScopedChoice);
  const CellRef box         = h.op(OpcodeKind::VqlBoxScalar);
  const CellRef fail        = h.op(OpcodeKind::Fail);
  const CellRef alternative = h.op(OpcodeKind::Halt);
  const CellRef input       = h.arena.makeScalarCell(std::int64_t{27});
  const CellRef output      = h.arena.makeCell("boxed result");
  h.scopeTarget(scoped, alternative);
  h.core.bindInput(box, input);
  h.core.bindOutput(box, output);
  h.next(scoped, box);
  h.next(box, fail);
  const CellRef cursor = h.cursorAt(scoped);
  h.arena.setExecutionLimits(100, 100, 100);

  ASSERT_TRUE(h.vm.step(cursor).success);
  h.arena.setExecutionLimits(100, 4, 100);
  const auto cyclesBeforeBox = h.vm.totalCycles();
  ASSERT_TRUE(h.vm.step(cursor).success);
  ASSERT_TRUE(h.vm.step(cursor).success);
  EXPECT_GT(h.vm.totalCycles(), cyclesBeforeBox);
  EXPECT_EQ(h.vm.getCursorOpcode(cursor), alternative);
  EXPECT_THROW(h.arena.consumeWork(), std::runtime_error);
}

TEST(VortexScopedChoiceTest, InvalidScopeDoesNotLeaveMarkOrMoveCursor) {
  const auto checkRejected = [](const bool bindTarget, const bool bindOutput,
                                const bool targetIsOpcode) {
    Harness h;
    const CellRef scoped = h.op(OpcodeKind::ScopedChoice);
    const CellRef target = targetIsOpcode ? h.op(OpcodeKind::Halt)
                                          : h.arena.makeCell("not an opcode");
    if (bindTarget) h.scopeTarget(scoped, target);
    if (bindOutput) h.core.bindOutput(scoped, h.arena.makeCell("output"));
    const CellRef cursor = h.cursorAt(scoped);

    const auto result = h.vm.step(cursor);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(h.vm.choiceDepth(), 0U);
    EXPECT_EQ(h.vm.getCursorOpcode(cursor), scoped);
    EXPECT_EQ(h.arena.outstandingMarks(), 0U);
  };

  checkRejected(false, false, true);
  checkRejected(true, false, false);
  checkRejected(true, true, true);
}

TEST(VortexScopedChoiceTest, ScopedChoiceDepthLimitRefusesBeforeAddingMark) {
  Harness h;
  h.vm.setScopedChoiceLimits(
      ScopedChoiceLimits{.maxDepth = 1, .maxSnapshotEntries = 32});
  const CellRef outer            = h.op(OpcodeKind::ScopedChoice);
  const CellRef inner            = h.op(OpcodeKind::ScopedChoice);
  const CellRef outerAlternative = h.op(OpcodeKind::Halt);
  const CellRef innerAlternative = h.op(OpcodeKind::Halt);
  h.scopeTarget(outer, outerAlternative);
  h.scopeTarget(inner, innerAlternative);
  h.next(outer, inner);
  const CellRef cursor = h.cursorAt(outer);

  ASSERT_TRUE(h.vm.step(cursor).success);
  const auto refused = h.vm.step(cursor);
  EXPECT_FALSE(refused.success);
  EXPECT_EQ(h.vm.choiceDepth(), 1U);
  EXPECT_EQ(h.vm.getCursorOpcode(cursor), inner);
  EXPECT_EQ(h.arena.outstandingMarks(), 1U);
  ASSERT_TRUE(h.vm.backtrack(cursor));
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);
}

} // namespace
} // namespace zigzag::vortex
