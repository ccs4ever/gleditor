#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace zigzag::vortex {
namespace {

using zigzag::CellRef;
using zigzag::DimVector;

struct Harness {
  zigzag::ArenaManifold arena;
  VortexCore core{arena};
  VortexVM vm{core};

  CellRef op(const OpcodeKind kind) { return vm.mintOpcode(kind); }

  void next(const CellRef from, const CellRef to) {
    ASSERT_TRUE(arena.link(from, core.dims().spin, DimVector::POS, to));
  }

  CellRef entry(const CellRef start) {
    const auto halt = op(OpcodeKind::Halt);
    auto tail       = start;
    while (const auto following =
               arena.linked(tail, core.dims().spin, DimVector::POS))
      tail = *following;
    next(tail, halt);
    return vm.spawnCursor(start, "frame_test");
  }

  CellRef descriptor(const CellRef body, const CellRef argument,
                     const CellRef result,
                     std::vector<CellRef> additionalLocals = {}) {
    std::vector<CellRef> locals{argument, result};
    locals.insert(locals.end(), additionalLocals.begin(),
                  additionalLocals.end());
    const auto made = vm.makeFrameDescriptor(FrameDescriptorSpec{
        .entry         = body,
        .locals        = std::move(locals),
        .argumentPorts = {argument},
        .resultPorts   = {result},
    });
    EXPECT_TRUE(made);
    if (!made) throw std::runtime_error("invalid test frame descriptor");
    return *made;
  }

  CellRef addOneFrame(const CellRef one) {
    const auto argument = arena.makeCell("argument");
    const auto result   = arena.makeCell("result");
    const auto add      = op(OpcodeKind::Add);
    core.bindInput(add, argument);
    core.bindInput(add, one);
    core.bindOutput(add, result);
    const auto ret = op(OpcodeKind::ReturnFrame);
    next(add, ret);
    const auto desc = descriptor(add, argument, result);
    core.bindInput(ret, desc);
    return desc;
  }

  CellRef invoke(const CellRef descriptorCell, const CellRef argument,
                 const CellRef output) {
    const auto call = op(OpcodeKind::InvokeFrame);
    core.bindInput(call, descriptorCell);
    core.bindInput(call, argument);
    core.bindOutput(call, output);
    return call;
  }
};

TEST(VortexFrameTest, InvocationUsesFreshLocalsAndReturnsComputedScalar) {
  Harness h;
  const auto one        = h.arena.makeScalarCell(std::int64_t{1});
  const auto descriptor = h.addOneFrame(one);
  const auto first      = h.arena.makeScalarCell(std::int64_t{40});
  const auto second     = h.arena.makeScalarCell(std::int64_t{90});
  const auto firstOut   = h.arena.makeCell("first result");
  const auto secondOut  = h.arena.makeCell("second result");
  const auto callFirst  = h.invoke(descriptor, first, firstOut);
  const auto callSecond = h.invoke(descriptor, second, secondOut);
  h.next(callFirst, callSecond);
  const auto cursor = h.entry(callFirst);

  const auto execution = h.vm.run(cursor, 100);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  const auto firstResult  = h.vm.resultCell(cursor, firstOut);
  const auto secondResult = h.vm.resultCell(cursor, secondOut);
  ASSERT_TRUE(firstResult);
  ASSERT_TRUE(secondResult);
  EXPECT_EQ(h.core.render(*firstResult), CellValue{std::int64_t{41}});
  EXPECT_EQ(h.core.render(*secondResult), CellValue{std::int64_t{91}});
  EXPECT_EQ(h.vm.activationDepth(cursor), 0u);
  EXPECT_FALSE(h.vm.hasActiveFrames());
}

TEST(VortexFrameTest, ReturnedArgumentPreservesExternalCellIdentity) {
  Harness h;
  const auto argument = h.arena.makeCell("formal argument");
  const auto result   = h.arena.makeCell("formal result");
  const auto copy     = h.op(OpcodeKind::CopyCellRefs);
  h.core.bindInput(copy, argument);
  h.core.bindOutput(copy, result);
  const auto ret = h.op(OpcodeKind::ReturnFrame);
  h.next(copy, ret);
  const auto desc = h.descriptor(copy, argument, result);
  h.core.bindInput(ret, desc);
  const auto external = h.arena.makeScalarCell(std::int64_t{73});
  const auto output   = h.arena.makeCell("caller result");
  const auto call     = h.invoke(desc, external, output);
  const auto cursor   = h.entry(call);

  const auto execution = h.vm.run(cursor, 20);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  EXPECT_EQ(h.vm.resultCells(cursor, output), (std::vector<CellRef>{external}));
  EXPECT_EQ(h.vm.resultCell(cursor, output), external);
}

TEST(VortexFrameTest, SeparateCursorsDoNotShareFrameLocalBacking) {
  Harness h;
  const auto one        = h.arena.makeScalarCell(std::int64_t{1});
  const auto descriptor = h.addOneFrame(one);
  const auto leftInput  = h.arena.makeScalarCell(std::int64_t{10});
  const auto rightInput = h.arena.makeScalarCell(std::int64_t{20});
  const auto leftOut    = h.arena.makeCell("left result");
  const auto rightOut   = h.arena.makeCell("right result");
  const auto leftCall   = h.invoke(descriptor, leftInput, leftOut);
  const auto rightCall  = h.invoke(descriptor, rightInput, rightOut);
  const auto left       = h.entry(leftCall);
  const auto right      = h.entry(rightCall);

  ASSERT_TRUE(h.vm.step(left).success);
  ASSERT_TRUE(h.vm.step(right).success);
  EXPECT_EQ(h.vm.activationDepth(left), 1u);
  EXPECT_EQ(h.vm.activationDepth(right), 1u);
  ASSERT_TRUE(h.vm.run(left, 20).success);
  EXPECT_EQ(h.vm.activationDepth(right), 1u);
  ASSERT_TRUE(h.vm.run(right, 20).success);
  EXPECT_EQ(h.core.render(h.vm.resultCell(left, leftOut).value()),
            CellValue{std::int64_t{11}});
  EXPECT_EQ(h.core.render(h.vm.resultCell(right, rightOut).value()),
            CellValue{std::int64_t{21}});
}

TEST(VortexFrameTest, BacktrackingRestoresActivationAndResultRegisters) {
  Harness h;
  const auto one        = h.arena.makeScalarCell(std::int64_t{1});
  const auto descriptor = h.addOneFrame(one);
  const auto firstInput = h.arena.makeScalarCell(std::int64_t{7});
  const auto nextInput  = h.arena.makeScalarCell(std::int64_t{30});
  const auto firstOut   = h.arena.makeCell("first result");
  const auto nextOut    = h.arena.makeCell("next result");
  const auto firstCall  = h.invoke(descriptor, firstInput, firstOut);
  const auto nextCall   = h.invoke(descriptor, nextInput, nextOut);
  const auto cursor     = h.entry(firstCall);
  const auto halt       = h.op(OpcodeKind::Halt);
  h.next(nextCall, halt);
  const auto originalFirst = h.vm.resultCells(cursor, firstOut);
  h.vm.pushChoicePoint(cursor, nextCall);

  const auto firstRun = h.vm.run(cursor, 20);
  ASSERT_TRUE(firstRun.success) << firstRun.errorMessage;
  const auto firstResult = h.vm.resultCell(cursor, firstOut);
  ASSERT_TRUE(firstResult);
  EXPECT_EQ(h.core.render(*firstResult), CellValue{std::int64_t{8}});

  ASSERT_TRUE(h.vm.backtrack(cursor));
  EXPECT_EQ(h.vm.resultCells(cursor, firstOut), originalFirst);
  const auto nextRun = h.vm.run(cursor, 20);
  ASSERT_TRUE(nextRun.success) << nextRun.errorMessage;
  const auto nextResult = h.vm.resultCell(cursor, nextOut);
  ASSERT_TRUE(nextResult);
  EXPECT_EQ(h.core.render(*nextResult), CellValue{std::int64_t{31}});
}

TEST(VortexFrameTest, NestedInvocationReturnsIntoCallerLocal) {
  Harness h;
  const auto one       = h.arena.makeScalarCell(std::int64_t{1});
  const auto inner     = h.addOneFrame(one);
  const auto argument  = h.arena.makeCell("outer argument");
  const auto temporary = h.arena.makeCell("outer temporary");
  const auto result    = h.arena.makeCell("outer result");
  const auto nested    = h.invoke(inner, argument, temporary);
  const auto add       = h.op(OpcodeKind::Add);
  h.core.bindInput(add, temporary);
  h.core.bindInput(add, one);
  h.core.bindOutput(add, result);
  const auto ret = h.op(OpcodeKind::ReturnFrame);
  h.next(nested, add);
  h.next(add, ret);
  const auto outer = h.descriptor(nested, argument, result, {temporary});
  h.core.bindInput(ret, outer);
  const auto output = h.arena.makeCell();
  const auto call =
      h.invoke(outer, h.arena.makeScalarCell(std::int64_t{40}), output);
  const auto cursor    = h.entry(call);
  const auto execution = h.vm.run(cursor, 100);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  const auto answer = h.vm.resultCell(cursor, output);
  ASSERT_TRUE(answer);
  EXPECT_EQ(h.core.render(*answer), CellValue{std::int64_t{42}});
  EXPECT_EQ(h.vm.activationDepth(cursor), 0u);
}

TEST(VortexFrameTest, UnwrittenResultRemainsEmpty) {
  Harness h;
  const auto argument   = h.arena.makeCell("argument");
  const auto result     = h.arena.makeCell("template text is not a result");
  const auto ret        = h.op(OpcodeKind::ReturnFrame);
  const auto descriptor = h.descriptor(ret, argument, result);
  h.core.bindInput(ret, descriptor);
  const auto output = h.arena.makeCell();
  const auto call   = h.invoke(descriptor, h.arena.makeCell("input"), output);
  const auto cursor = h.entry(call);
  const auto execution = h.vm.run(cursor, 20);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  EXPECT_TRUE(h.vm.resultCells(cursor, output).empty());
}

TEST(VortexFrameTest, ChoiceInsideCalleeRestoresReturnedActivation) {
  Harness h;
  const auto argument    = h.arena.makeCell();
  const auto result      = h.arena.makeCell();
  const auto one         = h.arena.makeScalarCell(std::int64_t{1});
  const auto first       = h.op(OpcodeKind::Add);
  const auto alternative = h.op(OpcodeKind::Sub);
  for (const auto operation : {first, alternative}) {
    h.core.bindInput(operation, argument);
    h.core.bindInput(operation, one);
    h.core.bindOutput(operation, result);
  }
  const auto firstReturn       = h.op(OpcodeKind::ReturnFrame);
  const auto alternativeReturn = h.op(OpcodeKind::ReturnFrame);
  h.next(first, firstReturn);
  h.next(alternative, alternativeReturn);
  const auto descriptor = h.descriptor(first, argument, result);
  h.core.bindInput(firstReturn, descriptor);
  h.core.bindInput(alternativeReturn, descriptor);
  const auto output = h.arena.makeCell();
  const auto call =
      h.invoke(descriptor, h.arena.makeScalarCell(std::int64_t{10}), output);
  const auto cursor = h.entry(call);
  ASSERT_TRUE(h.vm.step(cursor).success);
  ASSERT_EQ(h.vm.activationDepth(cursor), 1u);
  h.vm.pushChoicePoint(cursor, alternative);
  const auto firstRun = h.vm.run(cursor, 20);
  ASSERT_TRUE(firstRun.success) << firstRun.errorMessage;
  ASSERT_TRUE(h.vm.resultCell(cursor, output));
  EXPECT_EQ(h.core.render(*h.vm.resultCell(cursor, output)),
            CellValue{std::int64_t{11}});
  ASSERT_TRUE(h.vm.backtrack(cursor));
  EXPECT_EQ(h.vm.activationDepth(cursor), 1u);
  const auto secondRun = h.vm.run(cursor, 20);
  ASSERT_TRUE(secondRun.success) << secondRun.errorMessage;
  ASSERT_TRUE(h.vm.resultCell(cursor, output));
  EXPECT_EQ(h.core.render(*h.vm.resultCell(cursor, output)),
            CellValue{std::int64_t{9}});
}

TEST(VortexFrameTest, DescriptorsCanShareLogicalSlots) {
  Harness h;
  const auto argument  = h.arena.makeCell();
  const auto result    = h.arena.makeCell();
  const auto one       = h.arena.makeScalarCell(std::int64_t{1});
  const auto add       = h.op(OpcodeKind::Add);
  const auto sub       = h.op(OpcodeKind::Sub);
  const auto addReturn = h.op(OpcodeKind::ReturnFrame);
  const auto subReturn = h.op(OpcodeKind::ReturnFrame);
  for (const auto operation : {add, sub}) {
    h.core.bindInput(operation, argument);
    h.core.bindInput(operation, one);
    h.core.bindOutput(operation, result);
  }
  h.next(add, addReturn);
  h.next(sub, subReturn);
  const auto addDescriptor = h.descriptor(add, argument, result);
  const auto subDescriptor = h.descriptor(sub, argument, result);
  h.core.bindInput(addReturn, addDescriptor);
  h.core.bindInput(subReturn, subDescriptor);
  const auto input  = h.arena.makeScalarCell(std::int64_t{10});
  const auto left   = h.arena.makeCell();
  const auto right  = h.arena.makeCell();
  const auto first  = h.invoke(addDescriptor, input, left);
  const auto second = h.invoke(subDescriptor, input, right);
  h.next(first, second);
  const auto cursor    = h.entry(first);
  const auto execution = h.vm.run(cursor, 40);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  ASSERT_TRUE(h.vm.resultCell(cursor, left));
  ASSERT_TRUE(h.vm.resultCell(cursor, right));
  EXPECT_EQ(h.core.render(*h.vm.resultCell(cursor, left)),
            CellValue{std::int64_t{11}});
  EXPECT_EQ(h.core.render(*h.vm.resultCell(cursor, right)),
            CellValue{std::int64_t{9}});
}

TEST(VortexFrameTest, RecursiveGraphPreservesEveryCallerLocal) {
  Harness h;
  const auto n           = h.arena.makeCell();
  const auto condition   = h.arena.makeCell();
  const auto decremented = h.arena.makeCell();
  const auto child       = h.arena.makeCell();
  const auto result      = h.arena.makeCell();
  const auto zero        = h.arena.makeScalarCell(std::int64_t{0});
  const auto one         = h.arena.makeScalarCell(std::int64_t{1});
  const auto compare     = h.op(OpcodeKind::Eq);
  h.core.bindInput(compare, n);
  h.core.bindInput(compare, zero);
  h.core.bindOutput(compare, condition);
  const auto branch    = h.op(OpcodeKind::BranchFalseCell);
  const auto decrement = h.op(OpcodeKind::Sub);
  h.core.bindInput(branch, condition);
  h.core.bindInput(branch, decrement);
  h.core.bindInput(decrement, n);
  h.core.bindInput(decrement, one);
  h.core.bindOutput(decrement, decremented);
  const auto base = h.op(OpcodeKind::CopyCellRefs);
  h.core.bindInput(base, zero);
  h.core.bindOutput(base, result);
  const auto baseReturn = h.op(OpcodeKind::ReturnFrame);
  const auto recurse    = h.op(OpcodeKind::InvokeFrame);
  const auto accumulate = h.op(OpcodeKind::Add);
  h.core.bindInput(accumulate, child);
  h.core.bindInput(accumulate, one);
  h.core.bindOutput(accumulate, result);
  const auto recursiveReturn = h.op(OpcodeKind::ReturnFrame);
  h.next(compare, branch);
  h.next(branch, base);
  h.next(base, baseReturn);
  h.next(decrement, recurse);
  h.next(recurse, accumulate);
  h.next(accumulate, recursiveReturn);
  const auto descriptor =
      h.descriptor(compare, n, result, {condition, decremented, child});
  h.core.bindInput(recurse, descriptor);
  h.core.bindInput(recurse, decremented);
  h.core.bindOutput(recurse, child);
  h.core.bindInput(baseReturn, descriptor);
  h.core.bindInput(recursiveReturn, descriptor);
  const auto output    = h.arena.makeCell();
  const auto input     = h.arena.makeScalarCell(std::int64_t{5});
  const auto call      = h.invoke(descriptor, input, output);
  const auto cursor    = h.entry(call);
  const auto execution = h.vm.run(cursor, 200);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  ASSERT_TRUE(h.vm.resultCell(cursor, output));
  EXPECT_EQ(h.core.render(*h.vm.resultCell(cursor, output)),
            CellValue{std::int64_t{5}});
  EXPECT_EQ(h.core.render(input), CellValue{std::int64_t{5}});
  EXPECT_EQ(h.vm.activationDepth(cursor), 0u);
}

TEST(VortexFrameTest, StreamPortsPreserveOrderDuplicatesAndScalarSingleton) {
  Harness h;
  const auto argument   = h.arena.makeCell();
  const auto ret        = h.op(OpcodeKind::ReturnFrame);
  const auto descriptor = h.vm.makeFrameDescriptor(
      FrameDescriptorSpec{.entry         = ret,
                          .locals        = {argument},
                          .argumentPorts = {{argument, FramePortKind::Stream}},
                          .resultPorts = {{argument, FramePortKind::Stream}}});
  ASSERT_TRUE(descriptor);
  h.core.bindInput(ret, *descriptor);
  const auto a      = h.arena.makeCell("a");
  const auto b      = h.arena.makeCell("b");
  const auto stream = h.arena.makeCell();
  const auto append = h.op(OpcodeKind::StreamAppend);
  h.core.bindInput(append, a);
  h.core.bindInput(append, b);
  h.core.bindInput(append, a);
  h.core.bindOutput(append, stream);
  const auto streamOutput    = h.arena.makeCell();
  const auto singletonOutput = h.arena.makeCell();
  const auto streamCall      = h.invoke(*descriptor, stream, streamOutput);
  const auto singletonCall   = h.invoke(*descriptor, b, singletonOutput);
  h.next(append, streamCall);
  h.next(streamCall, singletonCall);
  const auto cursor = h.entry(singletonCall);
  h.vm.setCursorOpcode(cursor, append);
  const auto execution = h.vm.run(cursor, 100);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  EXPECT_EQ(h.vm.resultCells(cursor, streamOutput),
            (std::vector<CellRef>{a, b, a}));
  EXPECT_EQ(h.vm.resultCells(cursor, singletonOutput),
            (std::vector<CellRef>{b}));
}

TEST(VortexFrameTest, PartialLocalAllocationFailureRollsBackAndCanRetry) {
  Harness h;
  const auto descriptor =
      h.addOneFrame(h.arena.makeScalarCell(std::int64_t{1}));
  const auto input          = h.arena.makeScalarCell(std::int64_t{40});
  const auto output         = h.arena.makeCell("original output");
  const auto call           = h.invoke(descriptor, input, output);
  const auto cursor         = h.entry(call);
  const auto before         = h.arena.cellCount();
  const auto originalOutput = h.vm.resultCells(cursor, output);
  h.arena.setAllocationLimitForTesting(before + 1);
  const auto refused = h.vm.step(cursor);
  EXPECT_FALSE(refused.success);
  EXPECT_EQ(h.vm.activationDepth(cursor), 0u);
  EXPECT_EQ(h.arena.cellCount(), before);
  EXPECT_EQ(h.vm.resultCells(cursor, output), originalOutput);
  EXPECT_EQ(h.vm.getCursorOpcode(cursor), call);
  h.arena.setAllocationLimitForTesting(before + 100);
  const auto retried = h.vm.run(cursor, 100);
  ASSERT_TRUE(retried.success) << retried.errorMessage;
  ASSERT_TRUE(h.vm.resultCell(cursor, output));
  EXPECT_EQ(h.core.render(*h.vm.resultCell(cursor, output)),
            CellValue{std::int64_t{41}});
}

TEST(VortexFrameTest, FramePostconditionRefusesBeforeOrdinaryOpcodeWrites) {
  for (const auto frameKind :
       {OpcodeKind::InvokeFrame, OpcodeKind::ReturnFrame}) {
    Harness h;
    const auto condition = h.op(frameKind);
    const auto add       = h.op(OpcodeKind::Add);
    h.core.bindInput(add, h.arena.makeScalarCell(std::int64_t{1}));
    h.core.bindInput(add, h.arena.makeScalarCell(std::int64_t{2}));
    const auto output = h.arena.makeScalarCell(std::int64_t{99});
    h.core.bindOutput(add, output);
    h.core.attachPostcondition(add, condition);
    const auto cursor = h.entry(add);
    EXPECT_FALSE(h.vm.step(cursor).success);
    EXPECT_EQ(h.arena.asInt64(output), std::optional{std::int64_t{99}});
    EXPECT_EQ(h.vm.activationDepth(cursor), 0u);
  }
}

TEST(VortexFrameTest, ClosingCursorCleansFramesAndChoiceCanRestoreSnapshot) {
  Harness h;
  const auto descriptor =
      h.addOneFrame(h.arena.makeScalarCell(std::int64_t{1}));
  const auto output = h.arena.makeCell();
  const auto call =
      h.invoke(descriptor, h.arena.makeScalarCell(std::int64_t{40}), output);
  const auto cursor = h.entry(call);
  ASSERT_TRUE(h.vm.step(cursor).success);
  ASSERT_EQ(h.vm.activationDepth(cursor), 1u);
  const auto resume = h.vm.getCursorOpcode(cursor);
  ASSERT_TRUE(resume);
  h.vm.pushChoicePoint(cursor, *resume);
  h.vm.setCursorOpcode(cursor, std::nullopt);
  EXPECT_EQ(h.vm.activationDepth(cursor), 0u);
  EXPECT_FALSE(h.vm.hasActiveFrames());
  ASSERT_TRUE(h.vm.backtrack(cursor));
  EXPECT_EQ(h.vm.activationDepth(cursor), 1u);
  const auto execution = h.vm.run(cursor, 40);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  ASSERT_TRUE(h.vm.resultCell(cursor, output));
  EXPECT_EQ(h.core.render(*h.vm.resultCell(cursor, output)),
            CellValue{std::int64_t{41}});
}

TEST(VortexFrameTest, MissingNestedContinuationRefusesAndCallerCanResume) {
  Harness h;
  const auto inner    = h.addOneFrame(h.arena.makeScalarCell(std::int64_t{1}));
  const auto argument = h.arena.makeCell();
  const auto result   = h.arena.makeCell();
  const auto nested   = h.invoke(inner, argument, result);
  const auto outer    = h.descriptor(nested, argument, result);
  const auto output   = h.arena.makeCell();
  const auto call =
      h.invoke(outer, h.arena.makeScalarCell(std::int64_t{40}), output);
  const auto cursor = h.entry(call);
  ASSERT_TRUE(h.vm.step(cursor).success);
  const auto before = h.arena.cellCount();
  EXPECT_FALSE(h.vm.step(cursor).success);
  EXPECT_EQ(h.arena.cellCount(), before);
  EXPECT_EQ(h.vm.activationDepth(cursor), 1u);
  EXPECT_EQ(h.vm.getCursorOpcode(cursor), nested);
  const auto ret = h.op(OpcodeKind::ReturnFrame);
  h.core.bindInput(ret, outer);
  h.next(nested, ret);
  const auto resumed = h.vm.run(cursor, 40);
  ASSERT_TRUE(resumed.success) << resumed.errorMessage;
  ASSERT_TRUE(h.vm.resultCell(cursor, output));
  EXPECT_EQ(h.core.render(*h.vm.resultCell(cursor, output)),
            CellValue{std::int64_t{41}});
}

TEST(VortexFrameTest, InPlaceIdentityPreservesScalarPayload) {
  Harness h;
  const auto argument = h.arena.makeCell();
  const auto ret      = h.op(OpcodeKind::ReturnFrame);
  const auto descriptor =
      h.vm.makeFrameDescriptor(FrameDescriptorSpec{.entry         = ret,
                                                   .locals        = {argument},
                                                   .argumentPorts = {argument},
                                                   .resultPorts = {argument}});
  ASSERT_TRUE(descriptor);
  h.core.bindInput(ret, *descriptor);
  const auto scalar    = h.arena.makeScalarCell(std::int64_t{95});
  const auto call      = h.invoke(*descriptor, scalar, scalar);
  const auto cursor    = h.entry(call);
  const auto execution = h.vm.run(cursor, 20);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  EXPECT_EQ(h.vm.resultCell(cursor, scalar), scalar);
  EXPECT_EQ(h.arena.asInt64(scalar), std::optional{std::int64_t{95}});
}

TEST(VortexFrameTest, FrameCannotHaltOrFallOffWithoutReturning) {
  for (const auto kind : {OpcodeKind::Halt, OpcodeKind::Nop}) {
    Harness h;
    const auto argument   = h.arena.makeCell();
    const auto result     = h.arena.makeCell();
    const auto body       = h.op(kind);
    const auto descriptor = h.descriptor(body, argument, result);
    const auto output     = h.arena.makeCell("unchanged caller");
    const auto original   = h.vm.resultCells(output);
    const auto call   = h.invoke(descriptor, h.arena.makeCell("input"), output);
    const auto cursor = h.entry(call);
    const auto execution = h.vm.run(cursor, 20);
    EXPECT_FALSE(execution.success);
    EXPECT_EQ(h.vm.activationDepth(cursor), 1u);
    EXPECT_EQ(h.vm.resultCells(cursor, output), original);
    h.vm.setCursorOpcode(cursor, std::nullopt);
    EXPECT_FALSE(h.vm.hasActiveFrames());
  }
}

TEST(VortexFrameTest, CumulativeBackingLimitBoundsSequentialCalls) {
  Harness h;
  const auto descriptor =
      h.addOneFrame(h.arena.makeScalarCell(std::int64_t{1}));
  h.vm.setFrameLimits(FrameLimits{.maxBackingCells = 2});
  const auto input        = h.arena.makeScalarCell(std::int64_t{40});
  const auto firstOutput  = h.arena.makeCell();
  const auto secondOutput = h.arena.makeCell("unchanged");
  const auto first        = h.invoke(descriptor, input, firstOutput);
  const auto second       = h.invoke(descriptor, input, secondOutput);
  h.next(first, second);
  const auto cursor         = h.entry(first);
  const auto originalSecond = h.vm.resultCells(cursor, secondOutput);
  const auto execution      = h.vm.run(cursor, 40);
  EXPECT_FALSE(execution.success);
  EXPECT_NE(execution.errorMessage.find("backing"), std::string::npos);
  ASSERT_TRUE(h.vm.resultCell(cursor, firstOutput));
  EXPECT_EQ(h.core.render(*h.vm.resultCell(cursor, firstOutput)),
            CellValue{std::int64_t{41}});
  EXPECT_EQ(h.vm.resultCells(cursor, secondOutput), originalSecond);
  EXPECT_EQ(h.vm.activationDepth(cursor), 0u);
}

TEST(VortexFrameTest, ConditionalBranchWithoutFallthroughCanReturnOrRefuse) {
  for (const bool condition : {false, true}) {
    Harness h;
    const auto argument = h.arena.makeCell();
    const auto ret      = h.op(OpcodeKind::ReturnFrame);
    const auto branch   = h.op(OpcodeKind::BranchFalseCell);
    h.core.bindInput(branch, h.arena.makeScalarCell(condition));
    h.core.bindInput(branch, ret);
    const auto descriptor = h.vm.makeFrameDescriptor(
        FrameDescriptorSpec{.entry         = branch,
                            .locals        = {argument},
                            .argumentPorts = {argument},
                            .resultPorts   = {argument}});
    ASSERT_TRUE(descriptor);
    h.core.bindInput(ret, *descriptor);
    const auto input     = h.arena.makeCell("actual input");
    const auto output    = h.arena.makeCell();
    const auto call      = h.invoke(*descriptor, input, output);
    const auto cursor    = h.entry(call);
    const auto execution = h.vm.run(cursor, 20);
    EXPECT_EQ(execution.success, !condition) << execution.errorMessage;
    EXPECT_EQ(h.vm.activationDepth(cursor), condition ? 1u : 0u);
    if (!condition)
      EXPECT_EQ(h.vm.resultCell(cursor, output), input);
    else
      h.vm.setCursorOpcode(cursor, std::nullopt);
  }
}

TEST(VortexFrameTest, DescriptorLabelsRefuseOversizedTextBeforeReadingPayload) {
  Harness h;
  const auto oversized = h.arena.makeCell(std::string(128, 'x'));
  EXPECT_FALSE(h.vm.frameDescriptor(oversized));
  const auto descriptor = h.arena.makeCell("VORTEX_FRAME/1");
  const auto target     = h.op(OpcodeKind::ReturnFrame);
  zigzag::expectWritten(
      h.arena.setValueBits(oversized, xanadu::ValueKind::Int64, target));
  zigzag::expectWritten(
      h.arena.link(descriptor, h.core.dims().grab, DimVector::POS, oversized));
  const auto parsed = h.vm.frameDescriptor(descriptor);
  ASSERT_FALSE(parsed);
  EXPECT_NE(parsed.error().message.find("label"), std::string::npos);
}

TEST(VortexFrameTest, ForwardDescriptorIsUnexecutableUntilFinalizedOnce) {
  Harness h;
  const auto reserved = h.vm.reserveFrameDescriptor();
  ASSERT_TRUE(reserved);
  const auto argument = h.arena.makeCell();
  const auto ret      = h.op(OpcodeKind::ReturnFrame);
  h.core.bindInput(ret, *reserved);
  const FrameDescriptorSpec spec{.entry         = ret,
                                 .locals        = {argument},
                                 .argumentPorts = {argument},
                                 .resultPorts   = {argument}};
  const auto input  = h.arena.makeScalarCell(std::int64_t{73});
  const auto output = h.arena.makeCell();
  const auto call   = h.invoke(*reserved, input, output);
  const auto cursor = h.entry(call);
  const auto before = h.arena.cellCount();
  EXPECT_FALSE(h.vm.step(cursor).success);
  EXPECT_EQ(h.arena.cellCount(), before);
  EXPECT_EQ(h.vm.activationDepth(cursor), 0u);
  ASSERT_TRUE(h.vm.finalizeFrameDescriptor(*reserved, spec));
  EXPECT_FALSE(h.vm.finalizeFrameDescriptor(*reserved, spec));
  const auto execution = h.vm.run(cursor, 20);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  EXPECT_EQ(h.vm.resultCell(cursor, output), input);
}

TEST(VortexFrameTest, PartialDescriptorFinalizationRollsBackAndCanRetry) {
  Harness h;
  const auto reserved = h.vm.reserveFrameDescriptor();
  ASSERT_TRUE(reserved);
  const auto argument = h.arena.makeCell();
  const auto ret      = h.op(OpcodeKind::ReturnFrame);
  const FrameDescriptorSpec spec{.entry         = ret,
                                 .locals        = {argument},
                                 .argumentPorts = {argument},
                                 .resultPorts   = {argument}};
  const auto before = h.arena.cellCount();
  h.arena.setAllocationLimitForTesting(before + 1);
  EXPECT_FALSE(h.vm.finalizeFrameDescriptor(*reserved, spec));
  EXPECT_EQ(h.arena.cellCount(), before);
  EXPECT_FALSE(h.vm.frameDescriptor(*reserved));
  h.arena.setAllocationLimitForTesting(before + 100);
  ASSERT_TRUE(h.vm.finalizeFrameDescriptor(*reserved, spec));
  EXPECT_TRUE(h.vm.frameDescriptor(*reserved));
}

TEST(VortexFrameTest, DescriptorRejectsLocalLimitAndZeroDepthRejectsCall) {
  Harness h;
  const auto argument = h.arena.makeCell("argument");
  const auto result   = h.arena.makeCell("result");
  const auto body     = h.op(OpcodeKind::ReturnFrame);
  h.vm.setFrameLimits(FrameLimits{.maxDepth = 8, .maxLocalsPerFrame = 1});
  const auto refused = h.vm.makeFrameDescriptor(FrameDescriptorSpec{
      .entry         = body,
      .locals        = {argument, result},
      .argumentPorts = {argument},
      .resultPorts   = {result},
  });
  ASSERT_FALSE(refused);

  h.vm.setFrameLimits(FrameLimits{.maxDepth = 0, .maxLocalsPerFrame = 8});
  const auto accepted = h.vm.makeFrameDescriptor(FrameDescriptorSpec{
      .entry         = body,
      .locals        = {argument, result},
      .argumentPorts = {argument},
      .resultPorts   = {result},
  });
  ASSERT_TRUE(accepted);
  h.core.bindInput(body, *accepted);
  const auto input  = h.arena.makeScalarCell(std::int64_t{3});
  const auto output = h.arena.makeCell("output");
  const auto call   = h.invoke(*accepted, input, output);
  const auto cursor = h.entry(call);

  const auto originalOutput = h.vm.resultCells(cursor, output);
  const auto execution      = h.vm.run(cursor, 20);
  EXPECT_FALSE(execution.success);
  EXPECT_EQ(h.vm.resultCells(cursor, output), originalOutput);
  EXPECT_EQ(h.vm.activationDepth(cursor), 0u);
  EXPECT_FALSE(h.vm.hasActiveFrames());
}

} // namespace
} // namespace zigzag::vortex
