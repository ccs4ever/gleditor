/**
 * @file vortex_mutation_groups_test.cpp
 * @brief Exact VM rollback and scope cleanup for source mutation groups.
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <stop_token>

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

  CellRef booleanValidator() {
    const CellRef guardedArgument = arena.makeCell("guarded argument");
    const CellRef booleanArgument = arena.makeCell("boolean argument");
    const CellRef result          = arena.makeCell("validator result");
    const CellRef copy            = op(OpcodeKind::CopyCellRefs);
    const CellRef ret             = op(OpcodeKind::ReturnFrame);
    core.bindInput(copy, booleanArgument);
    core.bindOutput(copy, result);
    next(copy, ret);
    const auto descriptor = vm.makeFrameDescriptor(FrameDescriptorSpec{
        .entry         = copy,
        .locals        = {guardedArgument, booleanArgument, result},
        .argumentPorts = {guardedArgument, booleanArgument},
        .resultPorts   = {result},
    });
    EXPECT_TRUE(descriptor);
    if (!descriptor) throw std::runtime_error(descriptor.error().message);
    core.bindInput(ret, *descriptor);
    return *descriptor;
  }

  CellRef pushGuard(const CellRef descriptor, const CellRef guarded,
                    const CellRef booleanValue) {
    const CellRef push  = op(OpcodeKind::PushGuard);
    const CellRef token = arena.makeCell("guard token slot");
    core.bindInput(push, descriptor);
    core.bindInput(push, arena.makeCell("cross-cursor guard"));
    core.bindInput(push, guarded);
    core.bindInput(push, booleanValue);
    core.bindOutput(push, token);
    return push;
  }

  void next(const CellRef from, const CellRef to) {
    ASSERT_TRUE(arena.link(from, core.dims().spin, DimVector::POS, to));
  }

  CellRef valueMutation(const CellRef subject, const CellRef replacement) {
    const CellRef value = op(OpcodeKind::Value);
    core.bindInput(value, subject);
    core.bindInput(value, arena.makeScalarCell(std::int64_t{0}));
    core.bindInput(value, arena.makeScalarCell(std::int64_t{-1}));
    core.bindInput(value, replacement);
    return value;
  }

  CellRef groupProgram(const CellRef subject, const CellRef target,
                       const OpcodeKind terminal) {
    const CellRef begin = op(OpcodeKind::BeginMutationGroup);
    const CellRef token = arena.makeCell("group token slot");
    core.bindOutput(begin, token);

    const CellRef link = op(OpcodeKind::Link);
    core.bindInput(link, subject);
    core.bindInput(link, arena.makeScalarCell(
                             static_cast<std::int64_t>(core.dims().role)));
    core.bindInput(link, arena.makeScalarCell(std::int64_t{1}));
    core.bindInput(link, target);

    CellRef end = op(OpcodeKind::EndMutationGroup);
    core.bindInput(end, token);
    const CellRef finish = op(terminal);
    next(begin, link);
    if (terminal == OpcodeKind::Halt) {
      next(link, end);
      next(end, finish);
    } else {
      const CellRef halt = op(OpcodeKind::Halt);
      next(link, finish);
      next(finish, end);
      next(end, halt);
    }
    return begin;
  }

  CellRef valueGroupProgram(const CellRef subject, const CellRef replacement) {
    const CellRef begin = op(OpcodeKind::BeginMutationGroup);
    const CellRef token = arena.makeCell("group token slot");
    core.bindOutput(begin, token);

    const CellRef value = valueMutation(subject, replacement);

    const CellRef end = op(OpcodeKind::EndMutationGroup);
    core.bindInput(end, token);
    const CellRef finish = op(OpcodeKind::Halt);
    next(begin, value);
    next(value, end);
    next(end, finish);
    return begin;
  }

  CellRef cursorAt(const CellRef entry) {
    return vm.spawnCursor(entry, "mutation-group-test");
  }
};

TEST(VortexMutationGroupsTest,
     SuccessfulGroupCommitsAndKeepsCallerChoiceForItsOwnRollback) {
  Harness h;
  const CellRef subject  = h.arena.makeCell("subject");
  const CellRef oldValue = h.arena.makeCell("old target");
  const CellRef newValue = h.arena.makeCell("new target");
  ASSERT_TRUE(
      h.arena.link(subject, h.core.dims().role, DimVector::POS, oldValue));
  const CellRef entry  = h.groupProgram(subject, newValue, OpcodeKind::Halt);
  const CellRef cursor = h.cursorAt(entry);
  const CellRef callerAlternative = h.op(OpcodeKind::Halt);
  h.vm.pushChoicePoint(cursor, callerAlternative);

  const auto execution = h.vm.run(cursor, 100);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  EXPECT_EQ(h.arena.linked(subject, h.core.dims().role, DimVector::POS),
            newValue);
  ASSERT_EQ(h.vm.choiceDepth(), 1U);

  ASSERT_TRUE(h.vm.backtrack(cursor));
  EXPECT_EQ(h.vm.getCursorOpcode(cursor), callerAlternative);
  EXPECT_EQ(h.arena.linked(subject, h.core.dims().role, DimVector::POS),
            oldValue);
  EXPECT_EQ(h.vm.choiceDepth(), 0U);
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);
}

TEST(VortexMutationGroupsTest,
     CutCannotConsumeGroupAndFailureRestoresOnlyItsDescendants) {
  Harness h;
  const CellRef subject  = h.arena.makeCell("subject");
  const CellRef oldValue = h.arena.makeCell("old target");
  const CellRef newValue = h.arena.makeCell("new target");
  ASSERT_TRUE(
      h.arena.link(subject, h.core.dims().role, DimVector::POS, oldValue));
  const CellRef entry  = h.groupProgram(subject, newValue, OpcodeKind::Cut);
  const CellRef cursor = h.cursorAt(entry);
  const CellRef callerAlternative = h.op(OpcodeKind::Halt);
  h.vm.pushChoicePoint(cursor, callerAlternative);

  const auto execution = h.vm.run(cursor, 100);
  EXPECT_FALSE(execution.success);
  EXPECT_NE(execution.errorMessage.find("active mutation group"),
            std::string::npos);
  EXPECT_EQ(h.arena.linked(subject, h.core.dims().role, DimVector::POS),
            oldValue);
  ASSERT_EQ(h.vm.choiceDepth(), 1U);
  ASSERT_TRUE(h.vm.backtrack(cursor));
  EXPECT_EQ(h.vm.getCursorOpcode(cursor), callerAlternative);
  EXPECT_EQ(h.vm.choiceDepth(), 0U);
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);
}

TEST(VortexMutationGroupsTest,
     FailCannotReplayGroupOrBacktrackIntoCallerAlternative) {
  Harness h;
  const CellRef subject  = h.arena.makeCell("subject");
  const CellRef oldValue = h.arena.makeCell("old target");
  const CellRef newValue = h.arena.makeCell("new target");
  ASSERT_TRUE(
      h.arena.link(subject, h.core.dims().role, DimVector::POS, oldValue));
  const CellRef entry  = h.groupProgram(subject, newValue, OpcodeKind::Fail);
  const CellRef cursor = h.cursorAt(entry);
  const CellRef callerAlternative = h.op(OpcodeKind::Halt);
  h.vm.pushChoicePoint(cursor, callerAlternative);

  const auto execution = h.vm.run(cursor, 100);
  EXPECT_FALSE(execution.success);
  EXPECT_EQ(h.arena.linked(subject, h.core.dims().role, DimVector::POS),
            oldValue);
  ASSERT_EQ(h.vm.choiceDepth(), 1U);
  ASSERT_TRUE(h.vm.backtrack(cursor));
  EXPECT_EQ(h.vm.getCursorOpcode(cursor), callerAlternative);
  EXPECT_EQ(h.vm.choiceDepth(), 0U);
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);
}

TEST(VortexMutationGroupsTest,
     OpenGuardScopeBlocksMutationFromAnotherCursorUntilClosed) {
  Harness h;
  const CellRef value       = h.arena.makeScalarCell(std::int64_t{1});
  const CellRef replacement = h.arena.makeScalarCell(std::int64_t{2});

  const CellRef open  = h.op(OpcodeKind::OpenGuardScope);
  const CellRef token = h.arena.makeCell("scope token slot");
  h.core.bindOutput(open, token);
  const CellRef close = h.op(OpcodeKind::CloseGuardScope);
  h.core.bindInput(close, token);
  const CellRef scopeHalt = h.op(OpcodeKind::Halt);
  h.next(open, close);
  h.next(close, scopeHalt);

  const CellRef mutation     = h.valueMutation(value, replacement);
  const CellRef mutationHalt = h.op(OpcodeKind::Halt);
  h.next(mutation, mutationHalt);
  const CellRef scopeCursor    = h.cursorAt(open);
  const CellRef mutationCursor = h.cursorAt(mutation);

  ASSERT_TRUE(h.vm.step(scopeCursor).success);
  EXPECT_TRUE(h.vm.hasActiveGuards());
  const auto refused = h.vm.step(mutationCursor);
  EXPECT_FALSE(refused.success);
  EXPECT_EQ(h.vm.getCursorOpcode(mutationCursor), mutation);
  EXPECT_EQ(h.arena.asInt64(value), 1);

  ASSERT_TRUE(h.vm.step(scopeCursor).success);
  EXPECT_FALSE(h.vm.hasActiveGuards());
  const auto resumed = h.vm.step(mutationCursor);
  ASSERT_TRUE(resumed.success) << resumed.errorMessage;
  EXPECT_EQ(h.arena.asInt64(value), 2);
  EXPECT_TRUE(h.vm.step(scopeCursor).success);
  EXPECT_TRUE(h.vm.step(mutationCursor).success);
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);
}

TEST(VortexMutationGroupsTest,
     ActiveCellGuardBlocksAliasedMutationFromAnotherCursorUntilPopped) {
  Harness h;
  const CellRef value       = h.arena.makeScalarCell(std::int64_t{1});
  const CellRef replacement = h.arena.makeScalarCell(std::int64_t{2});
  const CellRef boolean     = h.arena.makeScalarCell(true);
  const CellRef descriptor  = h.booleanValidator();
  const CellRef push        = h.pushGuard(descriptor, value, boolean);
  const auto pushOutputs    = h.core.outputsOf(push);
  ASSERT_EQ(pushOutputs.size(), 1U);
  const CellRef pop = h.op(OpcodeKind::PopGuard);
  h.core.bindInput(pop, pushOutputs.front());
  const CellRef guardHalt = h.op(OpcodeKind::Halt);
  h.next(push, pop);
  h.next(pop, guardHalt);

  const CellRef mutation     = h.valueMutation(value, replacement);
  const CellRef mutationHalt = h.op(OpcodeKind::Halt);
  h.next(mutation, mutationHalt);
  const CellRef guardCursor    = h.cursorAt(push);
  const CellRef mutationCursor = h.cursorAt(mutation);

  ASSERT_TRUE(h.vm.step(guardCursor).success);
  EXPECT_TRUE(h.vm.hasActiveGuards());
  const auto refused = h.vm.step(mutationCursor);
  EXPECT_FALSE(refused.success);
  EXPECT_EQ(h.vm.getCursorOpcode(mutationCursor), mutation);
  EXPECT_EQ(h.arena.asInt64(value), 1);

  ASSERT_TRUE(h.vm.step(guardCursor).success);
  EXPECT_FALSE(h.vm.hasActiveGuards());
  const auto resumed = h.vm.step(mutationCursor);
  ASSERT_TRUE(resumed.success) << resumed.errorMessage;
  EXPECT_EQ(h.arena.asInt64(value), 2);
  EXPECT_TRUE(h.vm.step(guardCursor).success);
  EXPECT_TRUE(h.vm.step(mutationCursor).success);
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);
}

TEST(VortexMutationGroupsTest,
     CancellationAbortsOpenGroupButPreservesCallerChoice) {
  Harness h;
  const CellRef subject  = h.arena.makeCell("subject");
  const CellRef oldValue = h.arena.makeCell("old target");
  const CellRef newValue = h.arena.makeCell("new target");
  ASSERT_TRUE(
      h.arena.link(subject, h.core.dims().role, DimVector::POS, oldValue));
  const CellRef entry  = h.groupProgram(subject, newValue, OpcodeKind::Halt);
  const CellRef cursor = h.cursorAt(entry);
  const CellRef callerAlternative = h.op(OpcodeKind::Halt);
  h.vm.pushChoicePoint(cursor, callerAlternative);
  ASSERT_TRUE(h.vm.step(cursor).success);
  ASSERT_EQ(h.vm.choiceDepth(), 2U);

  std::stop_source stop;
  stop.request_stop();
  const auto execution = h.vm.run(cursor, 100, stop.get_token());
  EXPECT_FALSE(execution.success);
  EXPECT_EQ(execution.errorMessage, "Cancelled by cut");
  EXPECT_EQ(h.arena.linked(subject, h.core.dims().role, DimVector::POS),
            oldValue);
  ASSERT_EQ(h.vm.choiceDepth(), 1U);
  ASSERT_TRUE(h.vm.backtrack(cursor));
  EXPECT_EQ(h.vm.getCursorOpcode(cursor), callerAlternative);
  EXPECT_EQ(h.vm.choiceDepth(), 0U);
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);
}

TEST(VortexMutationGroupsTest, PerCallCycleLimitLeavesGroupResumable) {
  Harness h;
  const CellRef subject  = h.arena.makeCell("subject");
  const CellRef oldValue = h.arena.makeCell("old target");
  const CellRef newValue = h.arena.makeCell("new target");
  ASSERT_TRUE(
      h.arena.link(subject, h.core.dims().role, DimVector::POS, oldValue));
  const CellRef entry  = h.groupProgram(subject, newValue, OpcodeKind::Halt);
  const CellRef cursor = h.cursorAt(entry);

  const auto limited = h.vm.run(cursor, 1);
  EXPECT_FALSE(limited.success);
  EXPECT_EQ(limited.errorMessage, "Exceeded max cycles");
  EXPECT_EQ(h.vm.getCursorOpcode(cursor),
            h.arena.linked(entry, h.core.dims().spin, DimVector::POS));
  EXPECT_EQ(h.vm.choiceDepth(), 1U);
  EXPECT_EQ(h.arena.linked(subject, h.core.dims().role, DimVector::POS),
            oldValue);

  const auto resumed = h.vm.run(cursor, 100);
  ASSERT_TRUE(resumed.success) << resumed.errorMessage;
  EXPECT_EQ(h.arena.linked(subject, h.core.dims().role, DimVector::POS),
            newValue);
  EXPECT_EQ(h.vm.choiceDepth(), 0U);
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);
}

TEST(VortexMutationGroupsTest,
     HardGroupCycleCapSpansResumableCallsAndRestoresCallerPrefix) {
  Harness h;
  const CellRef value       = h.arena.makeScalarCell(std::int64_t{1});
  const CellRef replacement = h.arena.makeScalarCell(std::int64_t{2});
  h.vm.setGuardLimits(GuardLimits{.maxMutationGroupCycles = 1});
  const CellRef entry             = h.valueGroupProgram(value, replacement);
  const CellRef cursor            = h.cursorAt(entry);
  const CellRef callerAlternative = h.op(OpcodeKind::Halt);
  h.vm.pushChoicePoint(cursor, callerAlternative);

  const auto beginPause = h.vm.run(cursor, 1);
  EXPECT_FALSE(beginPause.success);
  EXPECT_EQ(beginPause.errorMessage, "Exceeded max cycles");
  EXPECT_EQ(h.arena.asInt64(value), 1);
  EXPECT_EQ(h.vm.choiceDepth(), 2U);

  const auto mutationPause = h.vm.run(cursor, 1);
  EXPECT_FALSE(mutationPause.success);
  EXPECT_EQ(mutationPause.errorMessage, "Exceeded max cycles");
  EXPECT_EQ(h.arena.asInt64(value), 2);
  EXPECT_EQ(h.vm.choiceDepth(), 2U);

  const auto capped = h.vm.run(cursor, 1);
  EXPECT_FALSE(capped.success);
  EXPECT_EQ(capped.errorMessage, "mutation-group cycle limit exceeded");
  EXPECT_EQ(h.arena.asInt64(value), 1);
  ASSERT_EQ(h.vm.choiceDepth(), 1U);
  ASSERT_TRUE(h.vm.backtrack(cursor));
  EXPECT_EQ(h.vm.getCursorOpcode(cursor), callerAlternative);
  EXPECT_EQ(h.vm.choiceDepth(), 0U);
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);
}

TEST(VortexMutationGroupsTest, GuardScopeTokenMustCloseItsOwnScope) {
  Harness h;
  const CellRef open  = h.op(OpcodeKind::OpenGuardScope);
  const CellRef token = h.arena.makeCell("scope token slot");
  h.core.bindOutput(open, token);
  const CellRef close = h.op(OpcodeKind::CloseGuardScope);
  h.core.bindInput(close, token);
  const CellRef finish = h.op(OpcodeKind::Halt);
  h.next(open, close);
  h.next(close, finish);
  const CellRef cursor = h.cursorAt(open);

  const auto execution = h.vm.run(cursor, 100);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  ASSERT_TRUE(h.vm.cellReferenceOf(token));
  EXPECT_EQ(h.vm.activationDepth(cursor), 0U);
  EXPECT_FALSE(h.vm.hasActiveGuards());
  EXPECT_EQ(h.vm.choiceDepth(), 0U);
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);
}

TEST(VortexMutationGroupsTest,
     ActiveGuardScopeRefusesMutatingPreconditionBeforeItsWrite) {
  Harness h;
  const CellRef target      = h.arena.makeCell("precondition target");
  const CellRef replacement = h.arena.makeCell("condition write");
  const CellRef open        = h.op(OpcodeKind::OpenGuardScope);
  const CellRef token       = h.arena.makeCell("scope token slot");
  h.core.bindOutput(open, token);
  const CellRef outer     = h.op(OpcodeKind::Nop);
  const CellRef condition = h.valueMutation(target, replacement);
  h.core.attachPrecondition(outer, condition);
  const CellRef close = h.op(OpcodeKind::CloseGuardScope);
  h.core.bindInput(close, token);
  const CellRef finish = h.op(OpcodeKind::Halt);
  h.next(open, outer);
  h.next(outer, close);
  h.next(close, finish);
  const CellRef cursor = h.cursorAt(open);

  ASSERT_TRUE(h.vm.step(cursor).success);
  EXPECT_TRUE(h.vm.hasActiveGuards());
  const auto refused = h.vm.step(cursor);
  EXPECT_FALSE(refused.success);
  EXPECT_NE(refused.errorMessage.find("source-mutating conditions"),
            std::string::npos);
  EXPECT_EQ(h.arena.textOf(target), "precondition target");
  EXPECT_TRUE(h.vm.hasActiveGuards());

  h.vm.setCursorOpcode(cursor, close);
  const auto cleanup = h.vm.run(cursor, 10);
  ASSERT_TRUE(cleanup.success) << cleanup.errorMessage;
  EXPECT_FALSE(h.vm.hasActiveGuards());
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);
}

TEST(VortexMutationGroupsTest,
     ActiveMutationGroupRefusesMutatingPostconditionBeforeItsWrite) {
  Harness h;
  const CellRef groupTarget     = h.arena.makeCell("group target original");
  const CellRef conditionTarget = h.arena.makeCell("condition target original");
  const CellRef replacement     = h.arena.makeCell("condition write");
  const CellRef begin           = h.op(OpcodeKind::BeginMutationGroup);
  const CellRef token           = h.arena.makeCell("group token slot");
  h.core.bindOutput(begin, token);
  const CellRef groupMutation =
      h.valueMutation(groupTarget, h.arena.makeCell("group write"));
  const CellRef outer     = h.op(OpcodeKind::Nop);
  const CellRef condition = h.valueMutation(conditionTarget, replacement);
  h.core.attachPostcondition(outer, condition);
  const CellRef end = h.op(OpcodeKind::EndMutationGroup);
  h.core.bindInput(end, token);
  const CellRef finish = h.op(OpcodeKind::Halt);
  h.next(begin, groupMutation);
  h.next(groupMutation, outer);
  h.next(outer, end);
  h.next(end, finish);
  const CellRef cursor            = h.cursorAt(begin);
  const CellRef callerAlternative = h.op(OpcodeKind::Halt);
  h.vm.pushChoicePoint(cursor, callerAlternative);

  const auto refused = h.vm.run(cursor, 100);
  EXPECT_FALSE(refused.success);
  EXPECT_NE(refused.errorMessage.find("source-mutating conditions"),
            std::string::npos);
  EXPECT_EQ(h.arena.textOf(groupTarget), "group target original");
  EXPECT_EQ(h.arena.textOf(conditionTarget), "condition target original");
  ASSERT_EQ(h.vm.choiceDepth(), 1U);
  EXPECT_FALSE(h.vm.hasActiveGuards());
  ASSERT_TRUE(h.vm.backtrack(cursor));
  EXPECT_EQ(h.vm.getCursorOpcode(cursor), callerAlternative);
  EXPECT_EQ(h.vm.choiceDepth(), 0U);
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);
}

} // namespace
} // namespace zigzag::vortex
