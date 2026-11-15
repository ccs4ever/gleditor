#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/xanadu/vortex/vortex_stdlib.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/query_scalar.hpp"

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

  CellRef cursor(const CellRef start) {
    const auto halt = op(OpcodeKind::Halt);
    auto tail       = start;
    while (const auto following =
               arena.linked(tail, core.dims().spin, DimVector::POS))
      tail = *following;
    next(tail, halt);
    return vm.spawnCursor(start, "closure_test");
  }

  CellRef streamFrame() {
    const auto captured   = arena.makeCell("captured stream argument");
    const auto applied    = arena.makeCell("applied stream argument");
    const auto ret        = op(OpcodeKind::ReturnFrame);
    const auto descriptor = vm.makeFrameDescriptor(FrameDescriptorSpec{
        .entry         = ret,
        .locals        = {captured, applied},
        .argumentPorts = {{captured, FramePortKind::Stream},
                          {applied, FramePortKind::Stream}},
        .resultPorts   = {{captured, FramePortKind::Stream},
                          {applied, FramePortKind::Stream}},
    });
    EXPECT_TRUE(descriptor);
    if (!descriptor) throw std::runtime_error("invalid stream frame");
    core.bindInput(ret, *descriptor);
    const auto closure = vm.makeClosureDescriptor(ClosureDescriptorSpec{
        .frameDescriptor    = *descriptor,
        .captureStreamCount = 1,
        .maxFormalCount     = 1,
        .frameInputs        = {{ClosureInputSource::Capture, 0},
                               {ClosureInputSource::Applied, 0}},
    });
    EXPECT_TRUE(closure);
    if (!closure) throw std::runtime_error("invalid closure descriptor");
    return *closure;
  }

  CellRef makeClosure(const CellRef descriptor, const CellRef captureSlot,
                      const CellRef output) {
    const auto create = op(OpcodeKind::CreateClosure);
    core.bindInput(create, descriptor);
    core.bindInput(create, captureSlot);
    core.bindOutput(create, output);
    return create;
  }

  CellRef applyClosure(const CellRef handleSlot, const CellRef appliedSlot,
                       const CellRef firstOutput, const CellRef secondOutput) {
    const auto apply = op(OpcodeKind::ApplyClosure);
    const auto mode  = arena.makeScalarCell(
        static_cast<std::int64_t>(ClosureCallMode::Explicit));
    const auto context = arena.makeCell("hidden context");
    core.bindInput(apply, handleSlot);
    core.bindInput(apply, mode);
    core.bindInput(apply, context);
    core.bindInput(apply, arena.makeScalarCell(std::int64_t{1}));
    core.bindInput(apply, arena.makeCell(""));
    core.bindInput(apply, appliedSlot);
    core.bindOutput(apply, firstOutput);
    core.bindOutput(apply, secondOutput);
    return apply;
  }

  CellRef applyClosure(const CellRef handleSlot, const CellRef contextSlot,
                       const std::vector<CellRef> &actuals,
                       const std::vector<CellRef> &outputs) {
    const auto apply = op(OpcodeKind::ApplyClosure);
    const auto mode  = arena.makeScalarCell(
        static_cast<std::int64_t>(ClosureCallMode::Explicit));
    core.bindInput(apply, handleSlot);
    core.bindInput(apply, mode);
    core.bindInput(apply, contextSlot);
    core.bindInput(
        apply, arena.makeScalarCell(static_cast<std::int64_t>(actuals.size())));
    for (const auto actual : actuals) {
      core.bindInput(apply, arena.makeCell(""));
      core.bindInput(apply, actual);
    }
    for (const auto output : outputs) core.bindOutput(apply, output);
    return apply;
  }

  CellRef checkClosureCall(const CellRef calleeSlot, const ClosureCallMode mode,
                           const std::int64_t rawArgumentCount,
                           const CellRef output) {
    const auto check = op(OpcodeKind::CheckClosureCall);
    core.bindInput(check, calleeSlot);
    core.bindInput(check,
                   arena.makeScalarCell(static_cast<std::int64_t>(mode)));
    core.bindInput(check, arena.makeScalarCell(rawArgumentCount));
    for (std::int64_t index = 0; index < rawArgumentCount; ++index)
      core.bindInput(check, arena.makeCell(""));
    core.bindOutput(check, output);
    return check;
  }
};

TEST(VortexClosureTest, DescriptorRoundTripsAndRejectsMalformedPortMaps) {
  Harness h;
  const auto frame  = h.streamFrame();
  const auto parsed = h.vm.closureDescriptor(frame);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->frameDescriptor,
            h.vm.closureDescriptor(frame)->frameDescriptor);
  EXPECT_EQ(parsed->captureStreamCount, 1u);
  EXPECT_EQ(parsed->minFormalCount, 1u);
  EXPECT_EQ(parsed->maxFormalCount, 1u);
  ASSERT_TRUE(parsed->presenceTrue);
  ASSERT_TRUE(parsed->presenceFalse);
  EXPECT_EQ(h.arena.asBool(*parsed->presenceTrue), true);
  EXPECT_EQ(h.arena.asBool(*parsed->presenceFalse), false);
  EXPECT_NE(parsed->presenceTrue, parsed->presenceFalse);
  ASSERT_EQ(parsed->frameInputs.size(), 2u);
  EXPECT_EQ(parsed->frameInputs[0].source, ClosureInputSource::Capture);
  EXPECT_EQ(parsed->frameInputs[1].source, ClosureInputSource::Applied);

  const auto ordinary = h.arena.makeCell("ordinary cell");
  const auto before   = h.arena.cellCount();
  EXPECT_FALSE(h.vm.makeClosureDescriptor(ClosureDescriptorSpec{
      .frameDescriptor    = parsed->frameDescriptor,
      .captureStreamCount = 1,
      .maxFormalCount     = 1,
      .frameInputs        = {{ClosureInputSource::Capture, 0},
                             {ClosureInputSource::Applied, 1}},
  }));
  EXPECT_FALSE(h.vm.makeClosureDescriptor(ClosureDescriptorSpec{
      .frameDescriptor    = parsed->frameDescriptor,
      .captureStreamCount = 1,
      .maxFormalCount     = 1,
      .frameInputs        = {{ClosureInputSource::Capture, 0}},
  }));
  EXPECT_EQ(h.arena.cellCount(), before);
  const auto cellInput  = h.arena.makeCell("cell argument");
  const auto resultSlot = h.arena.makeCell("cell result");
  const auto returnOp   = h.op(OpcodeKind::ReturnFrame);
  const auto cellFrame  = h.vm.makeFrameDescriptor(FrameDescriptorSpec{
       .entry         = returnOp,
       .locals        = {cellInput, resultSlot},
       .argumentPorts = {{cellInput, FramePortKind::Cell}},
       .resultPorts   = {resultSlot},
  });
  ASSERT_TRUE(cellFrame);
  EXPECT_TRUE(h.vm.makeClosureDescriptor(ClosureDescriptorSpec{
      .frameDescriptor    = *cellFrame,
      .captureStreamCount = 1,
      .maxFormalCount     = 0,
      .frameInputs        = {{ClosureInputSource::Capture, 0}},
  }));
  EXPECT_FALSE(h.vm.makeClosureDescriptor(ClosureDescriptorSpec{
      .frameDescriptor    = *cellFrame,
      .captureStreamCount = 1,
      .maxFormalCount     = 0,
      .frameInputs        = {{ClosureInputSource::Context, 0}},
  }));
  EXPECT_FALSE(h.vm.closureDescriptor(ordinary));
}

TEST(VortexClosureTest, DescriptorThreeKeepsNamesAndFinalRestGroupMapping) {
  Harness h;
  const auto fixed = h.arena.makeCell("fixed formal");
  const auto rest  = h.arena.makeCell("rest formal");
  const auto ret   = h.op(OpcodeKind::ReturnFrame);
  const auto frame = h.vm.makeFrameDescriptor(FrameDescriptorSpec{
      .entry         = ret,
      .locals        = {fixed, rest},
      .argumentPorts = {{fixed, FramePortKind::Stream},
                        {rest, FramePortKind::Groups}},
  });
  ASSERT_TRUE(frame);
  h.core.bindInput(ret, *frame);
  const auto descriptor = h.vm.makeClosureDescriptor(ClosureDescriptorSpec{
      .frameDescriptor     = *frame,
      .minFormalCount      = 0,
      .maxFormalCount      = 2,
      .frameInputs         = {{ClosureInputSource::Applied, 0},
                              {ClosureInputSource::AppliedRestGroups, 1}},
      .variadicFormalIndex = 1,
      .formalNames = std::vector<std::optional<std::string>>{"fixed", "rest"},
  });
  ASSERT_TRUE(descriptor);

  const auto parsed = h.vm.closureDescriptor(*descriptor);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->maxFormalCount, 2u);
  EXPECT_EQ(parsed->minFormalCount, 0u);
  EXPECT_EQ(parsed->variadicFormalIndex, 1u);
  ASSERT_TRUE(parsed->formalNames);
  ASSERT_EQ(parsed->formalNames->size(), 2u);
  EXPECT_EQ((*parsed->formalNames)[0], "fixed");
  EXPECT_EQ((*parsed->formalNames)[1], "rest");
  EXPECT_EQ(parsed->frameInputs[1].source,
            ClosureInputSource::AppliedRestGroups);

  EXPECT_FALSE(h.vm.makeClosureDescriptor(ClosureDescriptorSpec{
      .frameDescriptor     = *frame,
      .minFormalCount      = 0,
      .maxFormalCount      = 2,
      .frameInputs         = {{ClosureInputSource::Applied, 0},
                              {ClosureInputSource::AppliedRestGroups, 1}},
      .variadicFormalIndex = 0,
  }));
  EXPECT_FALSE(h.vm.makeClosureDescriptor(ClosureDescriptorSpec{
      .frameDescriptor     = *frame,
      .minFormalCount      = 0,
      .maxFormalCount      = 2,
      .frameInputs         = {{ClosureInputSource::Applied, 0},
                              {ClosureInputSource::AppliedRestGroups, 1}},
      .variadicFormalIndex = 1,
      .formalNames = std::vector<std::optional<std::string>>{"same", "same"},
  }));
  EXPECT_FALSE(h.vm.makeClosureDescriptor(ClosureDescriptorSpec{
      .frameDescriptor     = *frame,
      .minFormalCount      = 0,
      .maxFormalCount      = 2,
      .frameInputs         = {{ClosureInputSource::Applied, 0},
                              {ClosureInputSource::AppliedRest, 1}},
      .variadicFormalIndex = 1,
  }));
}

TEST(VortexClosureTest, CheckClosureCallPreflightsNamedBindingFailures) {
  const auto attempt = [](const std::vector<std::pair<std::string, bool>>
                              &names,
                          const bool expectedSuccess) {
    Harness h;
    const auto fixed = h.arena.makeCell("fixed formal");
    const auto rest  = h.arena.makeCell("rest formal");
    const auto ret   = h.op(OpcodeKind::ReturnFrame);
    const auto frame = h.vm.makeFrameDescriptor(FrameDescriptorSpec{
        .entry         = ret,
        .locals        = {fixed, rest},
        .argumentPorts = {{fixed, FramePortKind::Stream},
                          {rest, FramePortKind::Groups}},
    });
    ASSERT_TRUE(frame);
    h.core.bindInput(ret, *frame);
    const auto descriptor = h.vm.makeClosureDescriptor(ClosureDescriptorSpec{
        .frameDescriptor     = *frame,
        .minFormalCount      = 0,
        .maxFormalCount      = 2,
        .frameInputs         = {{ClosureInputSource::Applied, 0},
                                {ClosureInputSource::AppliedRestGroups, 1}},
        .variadicFormalIndex = 1,
        .formalNames = std::vector<std::optional<std::string>>{"fixed", "rest"},
    });
    ASSERT_TRUE(descriptor);
    const auto handle = h.arena.makeCell("closure handle");
    const auto create = h.op(OpcodeKind::CreateClosure);
    h.core.bindInput(create, *descriptor);
    h.core.bindOutput(create, handle);
    const auto output = h.arena.makeCell("checked handle");
    const auto check  = h.op(OpcodeKind::CheckClosureCall);
    h.core.bindInput(check, handle);
    h.core.bindInput(check, h.arena.makeScalarCell(static_cast<std::int64_t>(
                                ClosureCallMode::Explicit)));
    h.core.bindInput(
        check, h.arena.makeScalarCell(static_cast<std::int64_t>(names.size())));
    for (const auto &[name, numeric] : names)
      h.core.bindInput(check, numeric ? h.arena.makeScalarCell(std::int64_t{1})
                                      : h.arena.makeCell(name));
    h.core.bindOutput(check, output);
    h.next(create, check);
    const auto cursor    = h.cursor(create);
    const auto execution = h.vm.run(cursor, 20);
    EXPECT_EQ(execution.success, expectedSuccess) << execution.errorMessage;
    if (expectedSuccess)
      EXPECT_EQ(h.vm.cellReferenceOf(output), h.vm.cellReferenceOf(handle));
    else {
      EXPECT_FALSE(h.vm.cellReferenceOf(output));
      EXPECT_EQ(h.vm.getCursorOpcode(cursor), check);
    }
  };

  attempt({{"rest", false}, {"fixed", false}}, true);
  attempt({{"unknown", false}}, false);
  attempt({{"fixed", false}, {"fixed", false}}, false);
  attempt({{"", false}, {"fixed", false}}, false);
  attempt({{"fixed", false}, {"", false}}, false);
  attempt({{"", true}}, false);
}

TEST(VortexClosureTest, RuntimeObjectsAreNotPlainClosureNameStrings) {
  {
    Harness h;
    const auto formal = h.arena.makeCell("runtime-object formal");
    const auto ret    = h.op(OpcodeKind::ReturnFrame);
    const auto frame  = h.vm.makeFrameDescriptor(FrameDescriptorSpec{
         .entry         = ret,
         .locals        = {formal},
         .argumentPorts = {{formal, FramePortKind::Stream}},
    });
    ASSERT_TRUE(frame);
    h.core.bindInput(ret, *frame);
    const auto descriptor = h.vm.makeClosureDescriptor(ClosureDescriptorSpec{
        .frameDescriptor = *frame,
        .maxFormalCount  = 1,
        .frameInputs     = {{ClosureInputSource::Applied, 0}},
        .formalNames     = std::vector<std::optional<std::string>>{std::string(
            zigzag::runtimeObjectProbeText)},
    });
    ASSERT_TRUE(descriptor);
    const auto handle = h.arena.makeCell("closure handle");
    const auto create = h.op(OpcodeKind::CreateClosure);
    h.core.bindInput(create, *descriptor);
    h.core.bindOutput(create, handle);
    const auto structuralName =
        h.vm.makeClosureValue(*descriptor, std::vector<std::vector<CellRef>>{});
    ASSERT_TRUE(structuralName);
    const auto output = h.arena.makeScalarCell(std::int64_t{123});
    const auto check  = h.op(OpcodeKind::CheckClosureCall);
    h.core.bindInput(check, handle);
    h.core.bindInput(check, h.arena.makeScalarCell(static_cast<std::int64_t>(
                                ClosureCallMode::Explicit)));
    h.core.bindInput(check, h.arena.makeScalarCell(std::int64_t{1}));
    h.core.bindInput(check, *structuralName);
    h.core.bindOutput(check, output);
    h.next(create, check);
    const auto cursor = h.cursor(create);
    ASSERT_TRUE(h.vm.step(cursor).success);

    const auto execution = h.vm.step(cursor);
    EXPECT_FALSE(execution.success);
    EXPECT_NE(execution.errorMessage.find("unclaimed plain String"),
              std::string::npos);
    EXPECT_EQ(h.vm.getCursorOpcode(cursor), check);
    EXPECT_EQ(h.arena.asInt64(output), std::optional{std::int64_t{123}});
    EXPECT_FALSE(h.vm.cellReferenceOf(output));
  }

  {
    Harness h;
    const auto formal = h.arena.makeCell("runtime-object formal");
    const auto result = h.arena.makeScalarCell(std::int64_t{456});
    const auto marker = h.arena.makeCell("frame body untouched");
    const auto write  = h.op(OpcodeKind::Value);
    h.core.bindInput(write, marker);
    h.core.bindInput(write, h.arena.makeScalarCell(std::int64_t{0}));
    h.core.bindInput(write, h.arena.makeScalarCell(std::int64_t{-1}));
    h.core.bindInput(write, h.arena.makeCell("frame body ran"));
    const auto ret = h.op(OpcodeKind::ReturnFrame);
    h.next(write, ret);
    const auto frame = h.vm.makeFrameDescriptor(FrameDescriptorSpec{
        .entry         = write,
        .locals        = {formal, result},
        .argumentPorts = {{formal, FramePortKind::Stream}},
        .resultPorts   = {result},
    });
    ASSERT_TRUE(frame);
    h.core.bindInput(ret, *frame);
    const auto descriptor = h.vm.makeClosureDescriptor(ClosureDescriptorSpec{
        .frameDescriptor = *frame,
        .maxFormalCount  = 1,
        .frameInputs     = {{ClosureInputSource::Applied, 0}},
        .formalNames     = std::vector<std::optional<std::string>>{std::string(
            zigzag::runtimeObjectProbeText)},
    });
    ASSERT_TRUE(descriptor);
    const auto handle = h.arena.makeCell("closure handle");
    const auto create = h.op(OpcodeKind::CreateClosure);
    h.core.bindInput(create, *descriptor);
    h.core.bindOutput(create, handle);
    const auto checked = h.arena.makeCell("checked closure handle");
    const auto check   = h.op(OpcodeKind::CheckClosureCall);
    const auto name    = h.arena.makeCell(zigzag::runtimeObjectProbeText);
    h.core.bindInput(check, handle);
    h.core.bindInput(check, h.arena.makeScalarCell(static_cast<std::int64_t>(
                                ClosureCallMode::Explicit)));
    h.core.bindInput(check, h.arena.makeScalarCell(std::int64_t{1}));
    h.core.bindInput(check, name);
    h.core.bindOutput(check, checked);

    const auto context      = h.arena.makeCell("empty context");
    const auto clearContext = h.op(OpcodeKind::StreamClear);
    h.core.bindOutput(clearContext, context);
    const auto actual      = h.arena.makeCell("empty actual");
    const auto clearActual = h.op(OpcodeKind::StreamClear);
    h.core.bindOutput(clearActual, actual);
    const auto applyOutput = h.arena.makeScalarCell(std::int64_t{789});
    const auto apply       = h.op(OpcodeKind::ApplyClosure);
    h.core.bindInput(apply, checked);
    h.core.bindInput(apply, h.arena.makeScalarCell(static_cast<std::int64_t>(
                                ClosureCallMode::Explicit)));
    h.core.bindInput(apply, context);
    h.core.bindInput(apply, h.arena.makeScalarCell(std::int64_t{1}));
    h.core.bindInput(apply, name);
    h.core.bindInput(apply, actual);
    h.core.bindOutput(apply, applyOutput);
    h.next(create, check);
    h.next(check, clearContext);
    h.next(clearContext, clearActual);
    h.next(clearActual, apply);
    const auto cursor = h.cursor(create);

    ASSERT_TRUE(h.vm.step(cursor).success);
    ASSERT_TRUE(h.vm.step(cursor).success);
    ASSERT_TRUE(h.core.dims().runtimeObjectClaim);
    const auto claimDimension = *h.core.dims().runtimeObjectClaim;
    const auto objectTag      = h.arena.makeCell("VORTEX_CLOSURE_VALUE/1");
    ASSERT_TRUE(h.arena.link(name, claimDimension, DimVector::POS, objectTag));
    ASSERT_TRUE(h.arena.link(objectTag, claimDimension, DimVector::NEG, name));
    EXPECT_EQ(classifyRuntimeObjectRoot(h.arena, name, claimDimension),
              RuntimeObjectClaimResult::Claimed);
    ASSERT_TRUE(h.vm.step(cursor).success);
    ASSERT_TRUE(h.vm.step(cursor).success);

    const auto execution = h.vm.step(cursor);
    EXPECT_FALSE(execution.success);
    EXPECT_NE(execution.errorMessage.find("unclaimed plain String"),
              std::string::npos);
    EXPECT_EQ(h.vm.getCursorOpcode(cursor), apply);
    EXPECT_EQ(h.arena.textOf(marker), "frame body untouched");
    EXPECT_EQ(h.arena.asInt64(applyOutput), std::optional{std::int64_t{789}});
    EXPECT_FALSE(h.vm.hasActiveFrames());
  }
}

TEST(VortexClosureTest,
     CheckAndApplyRejectAggregateActualNameBytesBeforeBinding) {
  constexpr std::size_t metadataByteLimit = 80;
  const std::string longName(41, 'x');

  {
    Harness h;
    auto limits             = h.vm.closureLimits();
    limits.maxMetadataBytes = metadataByteLimit;
    h.vm.setClosureLimits(limits);
    const auto fixed = h.arena.makeCell("fixed formal");
    const auto rest  = h.arena.makeCell("rest formal");
    const auto ret   = h.op(OpcodeKind::ReturnFrame);
    const auto frame = h.vm.makeFrameDescriptor(FrameDescriptorSpec{
        .entry         = ret,
        .locals        = {fixed, rest},
        .argumentPorts = {{fixed, FramePortKind::Stream},
                          {rest, FramePortKind::Stream}},
    });
    ASSERT_TRUE(frame);
    h.core.bindInput(ret, *frame);
    const auto descriptor = h.vm.makeClosureDescriptor(ClosureDescriptorSpec{
        .frameDescriptor     = *frame,
        .minFormalCount      = 0,
        .maxFormalCount      = 2,
        .frameInputs         = {{ClosureInputSource::Applied, 0},
                                {ClosureInputSource::AppliedRest, 1}},
        .variadicFormalIndex = 1,
        .formalNames = std::vector<std::optional<std::string>>{"fixed", "rest"},
    });
    ASSERT_TRUE(descriptor);
    const auto handle = h.arena.makeCell("closure handle");
    const auto create = h.op(OpcodeKind::CreateClosure);
    h.core.bindInput(create, *descriptor);
    h.core.bindOutput(create, handle);
    const auto output = h.arena.makeScalarCell(std::int64_t{123});
    const auto check  = h.op(OpcodeKind::CheckClosureCall);
    h.core.bindInput(check, handle);
    h.core.bindInput(check, h.arena.makeScalarCell(static_cast<std::int64_t>(
                                ClosureCallMode::Explicit)));
    h.core.bindInput(check, h.arena.makeScalarCell(std::int64_t{2}));
    h.core.bindInput(check, h.arena.makeCell(longName));
    h.core.bindInput(check, h.arena.makeCell(longName));
    h.core.bindOutput(check, output);
    h.next(create, check);
    const auto cursor = h.cursor(create);
    ASSERT_TRUE(h.vm.step(cursor).success);

    const auto before    = h.arena.cellCount();
    const auto execution = h.vm.step(cursor);
    EXPECT_FALSE(execution.success);
    EXPECT_NE(execution.errorMessage.find("aggregate byte limit"),
              std::string::npos);
    EXPECT_EQ(h.vm.getCursorOpcode(cursor), check);
    EXPECT_EQ(h.arena.asInt64(output), std::optional{std::int64_t{123}});
    EXPECT_FALSE(h.vm.cellReferenceOf(output));
    EXPECT_EQ(h.arena.cellCount(), before);
  }

  {
    Harness h;
    auto limits             = h.vm.closureLimits();
    limits.maxMetadataBytes = metadataByteLimit;
    h.vm.setClosureLimits(limits);
    const auto fixed  = h.arena.makeCell("fixed formal");
    const auto rest   = h.arena.makeCell("rest formal");
    const auto result = h.arena.makeScalarCell(std::int64_t{456});
    const auto marker = h.arena.makeCell("frame body untouched");
    const auto write  = h.op(OpcodeKind::Value);
    h.core.bindInput(write, marker);
    h.core.bindInput(write, h.arena.makeScalarCell(std::int64_t{0}));
    h.core.bindInput(write, h.arena.makeScalarCell(std::int64_t{-1}));
    h.core.bindInput(write, h.arena.makeCell("frame body ran"));
    const auto ret = h.op(OpcodeKind::ReturnFrame);
    h.next(write, ret);
    const auto frame = h.vm.makeFrameDescriptor(FrameDescriptorSpec{
        .entry         = write,
        .locals        = {fixed, rest, result},
        .argumentPorts = {{fixed, FramePortKind::Stream},
                          {rest, FramePortKind::Stream}},
        .resultPorts   = {result},
    });
    ASSERT_TRUE(frame);
    h.core.bindInput(ret, *frame);
    const auto descriptor = h.vm.makeClosureDescriptor(ClosureDescriptorSpec{
        .frameDescriptor     = *frame,
        .minFormalCount      = 0,
        .maxFormalCount      = 2,
        .frameInputs         = {{ClosureInputSource::Applied, 0},
                                {ClosureInputSource::AppliedRest, 1}},
        .variadicFormalIndex = 1,
        .formalNames = std::vector<std::optional<std::string>>{"fixed", "rest"},
    });
    ASSERT_TRUE(descriptor);
    const auto handle = h.arena.makeCell("closure handle");
    const auto create = h.op(OpcodeKind::CreateClosure);
    h.core.bindInput(create, *descriptor);
    h.core.bindOutput(create, handle);
    const auto checked = h.arena.makeCell("checked closure handle");
    const auto check   = h.op(OpcodeKind::CheckClosureCall);
    h.core.bindInput(check, handle);
    h.core.bindInput(check, h.arena.makeScalarCell(static_cast<std::int64_t>(
                                ClosureCallMode::Explicit)));
    h.core.bindInput(check, h.arena.makeScalarCell(std::int64_t{2}));
    const auto firstName  = h.arena.makeCell("");
    const auto secondName = h.arena.makeCell("");
    h.core.bindInput(check, firstName);
    h.core.bindInput(check, secondName);
    h.core.bindOutput(check, checked);

    const auto context      = h.arena.makeCell("empty context");
    const auto clearContext = h.op(OpcodeKind::StreamClear);
    h.core.bindOutput(clearContext, context);
    const auto actual      = h.arena.makeCell("empty actual");
    const auto clearActual = h.op(OpcodeKind::StreamClear);
    h.core.bindOutput(clearActual, actual);
    const auto applyOutput = h.arena.makeScalarCell(std::int64_t{789});
    const auto apply       = h.op(OpcodeKind::ApplyClosure);
    h.core.bindInput(apply, checked);
    h.core.bindInput(apply, h.arena.makeScalarCell(static_cast<std::int64_t>(
                                ClosureCallMode::Explicit)));
    h.core.bindInput(apply, context);
    h.core.bindInput(apply, h.arena.makeScalarCell(std::int64_t{2}));
    h.core.bindInput(apply, firstName);
    h.core.bindInput(apply, actual);
    h.core.bindInput(apply, secondName);
    h.core.bindInput(apply, actual);
    h.core.bindOutput(apply, applyOutput);
    h.next(create, check);
    h.next(check, clearContext);
    h.next(clearContext, clearActual);
    h.next(clearActual, apply);
    const auto cursor = h.cursor(create);

    ASSERT_TRUE(h.vm.step(cursor).success);
    ASSERT_TRUE(h.vm.step(cursor).success);
    const auto nameSpan = h.arena.intern(longName);
    const std::vector<xanadu::PrimediaSpan> nameContent{nameSpan};
    ASSERT_TRUE(h.arena.setContent(firstName, nameContent));
    ASSERT_TRUE(h.arena.setContent(secondName, nameContent));
    ASSERT_TRUE(h.vm.step(cursor).success);
    ASSERT_TRUE(h.vm.step(cursor).success);

    const auto execution = h.vm.step(cursor);
    EXPECT_FALSE(execution.success);
    EXPECT_NE(execution.errorMessage.find("aggregate byte limit"),
              std::string::npos);
    EXPECT_EQ(h.vm.getCursorOpcode(cursor), apply);
    EXPECT_EQ(h.arena.textOf(marker), "frame body untouched");
    EXPECT_EQ(h.arena.asInt64(applyOutput), std::optional{std::int64_t{789}});
    EXPECT_FALSE(h.vm.hasActiveFrames());
  }
}

TEST(VortexClosureTest, ApplyClosurePreflightsAndPassesEmptyRestGroups) {
  const auto attempt = [](const std::size_t retainedLimit,
                          const std::size_t invocationLimit,
                          const bool expectedSuccess) {
    Harness h;
    const auto fixed = h.arena.makeCell("fixed formal");
    const auto rest  = h.arena.makeCell("rest formal");
    const auto ret   = h.op(OpcodeKind::ReturnFrame);
    const auto frame = h.vm.makeFrameDescriptor(FrameDescriptorSpec{
        .entry         = ret,
        .locals        = {fixed, rest},
        .argumentPorts = {{fixed, FramePortKind::Stream},
                          {rest, FramePortKind::Groups}},
    });
    ASSERT_TRUE(frame);
    h.core.bindInput(ret, *frame);
    const auto descriptor = h.vm.makeClosureDescriptor(ClosureDescriptorSpec{
        .frameDescriptor     = *frame,
        .minFormalCount      = 0,
        .maxFormalCount      = 2,
        .frameInputs         = {{ClosureInputSource::Applied, 0},
                                {ClosureInputSource::AppliedRestGroups, 1}},
        .variadicFormalIndex = 1,
        .formalNames = std::vector<std::optional<std::string>>{"fixed", "rest"},
    });
    ASSERT_TRUE(descriptor);
    auto frameLimits                        = h.vm.frameLimits();
    frameLimits.maxRetainedGroupsReferences = retainedLimit;
    h.vm.setFrameLimits(frameLimits);

    const auto handle = h.arena.makeCell("closure handle");
    const auto create = h.op(OpcodeKind::CreateClosure);
    h.core.bindInput(create, *descriptor);
    h.core.bindOutput(create, handle);
    const auto checked = h.arena.makeCell("checked handle");
    const auto check   = h.op(OpcodeKind::CheckClosureCall);
    h.core.bindInput(check, handle);
    h.core.bindInput(check, h.arena.makeScalarCell(static_cast<std::int64_t>(
                                ClosureCallMode::Explicit)));
    h.core.bindInput(check, h.arena.makeScalarCell(std::int64_t{1}));
    const auto restName = h.arena.makeCell("rest");
    h.core.bindInput(check, restName);
    h.core.bindOutput(check, checked);

    const auto context      = h.arena.makeCell("empty context stream");
    const auto clearContext = h.op(OpcodeKind::StreamClear);
    h.core.bindOutput(clearContext, context);
    const auto actual      = h.arena.makeCell("empty rest actual");
    const auto clearActual = h.op(OpcodeKind::StreamClear);
    h.core.bindOutput(clearActual, actual);
    const auto apply = h.op(OpcodeKind::ApplyClosure);
    h.core.bindInput(apply, checked);
    h.core.bindInput(apply, h.arena.makeScalarCell(static_cast<std::int64_t>(
                                ClosureCallMode::Explicit)));
    h.core.bindInput(apply, context);
    h.core.bindInput(apply, h.arena.makeScalarCell(std::int64_t{1}));
    h.core.bindInput(apply, restName);
    h.core.bindInput(apply, actual);
    h.next(create, check);
    h.next(check, clearContext);
    h.next(clearContext, clearActual);
    h.next(clearActual, apply);
    const auto cursor = h.cursor(create);

    ASSERT_TRUE(h.vm.step(cursor).success);
    ASSERT_TRUE(h.vm.step(cursor).success);
    ASSERT_TRUE(h.vm.step(cursor).success);
    ASSERT_TRUE(h.vm.step(cursor).success);
    auto closureLimits              = h.vm.closureLimits();
    closureLimits.maxInvocationRefs = invocationLimit;
    h.vm.setClosureLimits(closureLimits);

    const auto execution = h.vm.run(cursor, 50);
    EXPECT_EQ(execution.success, expectedSuccess) << execution.errorMessage;
    EXPECT_EQ(h.vm.activationDepth(cursor), 0u);
    EXPECT_FALSE(h.vm.hasLiveGroupsState());
    if (!expectedSuccess) EXPECT_EQ(h.vm.getCursorOpcode(cursor), apply);
  };

  attempt(0, 100, false);
  attempt(1, 1, false);
  attempt(1, 100, true);
}

TEST(VortexClosureTest, ApplyRevalidatesDescriptorAfterArgumentEffects) {
  Harness h;
  const auto argumentEffect = h.arena.makeCell("original");
  const auto bodyEffect     = h.arena.makeCell("pristine");
  const auto write          = [&](const CellRef subject, const char *text) {
    const auto op = h.op(OpcodeKind::Value);
    h.core.bindInput(op, subject);
    h.core.bindInput(op, h.arena.makeScalarCell(std::int64_t{0}));
    h.core.bindInput(op, h.arena.makeScalarCell(std::int64_t{-1}));
    h.core.bindInput(op, h.arena.makeCell(text));
    return op;
  };
  const auto body = write(bodyEffect, "ran");
  const auto ret  = h.op(OpcodeKind::ReturnFrame);
  h.next(body, ret);
  const auto frame =
      h.vm.makeFrameDescriptor(FrameDescriptorSpec{.entry = body});
  ASSERT_TRUE(frame);
  h.core.bindInput(ret, *frame);
  const auto descriptor = h.vm.makeClosureDescriptor(
      ClosureDescriptorSpec{.frameDescriptor = *frame});
  ASSERT_TRUE(descriptor);
  const auto parsed = h.vm.closureDescriptor(*descriptor);
  ASSERT_TRUE(parsed);
  ASSERT_TRUE(parsed->presenceTrue);
  const auto handle = h.arena.makeCell("handle");
  const auto create = h.op(OpcodeKind::CreateClosure);
  h.core.bindInput(create, *descriptor);
  h.core.bindOutput(create, handle);
  const auto checked = h.arena.makeCell("checked");
  const auto check =
      h.checkClosureCall(handle, ClosureCallMode::Explicit, 0, checked);
  const auto argument = write(argumentEffect, "touched");
  const auto context  = h.arena.makeCell("empty context stream");
  const auto clear    = h.op(OpcodeKind::StreamClear);
  h.core.bindOutput(clear, context);
  const auto apply = h.applyClosure(checked, context, std::vector<CellRef>{},
                                    std::vector<CellRef>{});
  h.next(create, check);
  h.next(check, argument);
  h.next(argument, clear);
  h.next(clear, apply);
  const auto cursor = h.cursor(create);
  ASSERT_TRUE(h.vm.step(cursor).success);
  ASSERT_TRUE(h.vm.step(cursor).success);
  ASSERT_TRUE(h.vm.step(cursor).success);
  ASSERT_EQ(h.arena.textOf(argumentEffect), "touched");
  ASSERT_TRUE(
      h.arena.setValueBits(*parsed->presenceTrue, xanadu::ValueKind::Int64, 0));
  const auto execution = h.vm.run(cursor, 1000);
  EXPECT_FALSE(execution.success);
  EXPECT_EQ(h.arena.textOf(bodyEffect), "pristine");
  EXPECT_FALSE(h.vm.hasActiveFrames());
  EXPECT_EQ(h.arena.outstandingMarks(), 0u);
}

TEST(VortexClosureTest,
     CheckClosureCallRefusesExcessArityWithoutPublishingAndCanRetry) {
  Harness h;
  const auto ret = h.op(OpcodeKind::ReturnFrame);
  const auto frame =
      h.vm.makeFrameDescriptor(FrameDescriptorSpec{.entry = ret});
  ASSERT_TRUE(frame);
  h.core.bindInput(ret, *frame);
  const auto descriptor = h.vm.makeClosureDescriptor(
      ClosureDescriptorSpec{.frameDescriptor = *frame});
  ASSERT_TRUE(descriptor);
  const auto handle = h.arena.makeCell("closure handle");
  const auto create = h.op(OpcodeKind::CreateClosure);
  h.core.bindInput(create, *descriptor);
  h.core.bindOutput(create, handle);
  const auto count = h.arena.makeScalarCell(std::int64_t{1});
  const auto mode  = h.arena.makeScalarCell(
      static_cast<std::int64_t>(ClosureCallMode::Explicit));
  const auto checked =
      h.arena.makeScalarCell(std::numeric_limits<std::int64_t>::max());
  const auto check = h.op(OpcodeKind::CheckClosureCall);
  h.core.bindInput(check, handle);
  h.core.bindInput(check, mode);
  h.core.bindInput(check, count);
  h.core.bindInput(check, h.arena.makeCell(""));
  h.core.bindOutput(check, checked);
  const auto contextualOutput = h.arena.makeCell("contextual checked handle");
  const auto contextualCheck  = h.checkClosureCall(
      handle, ClosureCallMode::Contextual, 0, contextualOutput);
  h.next(create, check);
  h.next(check, contextualCheck);
  const auto cursor = h.cursor(create);

  ASSERT_TRUE(h.vm.step(cursor).success);
  const auto before  = h.arena.cellCount();
  const auto refused = h.vm.step(cursor);
  EXPECT_FALSE(refused.success);
  EXPECT_EQ(h.arena.cellCount(), before);
  EXPECT_EQ(h.vm.getCursorOpcode(cursor), check);
  EXPECT_EQ(h.arena.asInt64(checked), std::numeric_limits<std::int64_t>::max());
  EXPECT_FALSE(h.vm.cellReferenceOf(checked));

  ASSERT_TRUE(h.arena.setValueBits(count, xanadu::ValueKind::Int64, 0));
  auto countSlot = h.arena.linked(check, h.core.dims().grab, DimVector::POS);
  ASSERT_TRUE(countSlot);
  for (int index = 0; index < 2; ++index) {
    countSlot = h.arena.linked(*countSlot, h.core.dims().step, DimVector::POS);
    ASSERT_TRUE(countSlot);
  }
  ASSERT_TRUE(h.arena.unlink(*countSlot, h.core.dims().step, DimVector::POS));
  const auto retried = h.vm.step(cursor);
  EXPECT_TRUE(retried.success) << retried.errorMessage;
  const auto afterRetry = h.vm.getCursorOpcode(cursor);
  ASSERT_TRUE(afterRetry);
  EXPECT_EQ(*afterRetry, contextualCheck);
  EXPECT_EQ(h.vm.cellReferenceOf(checked), h.vm.cellReferenceOf(handle));
  const auto contextual = h.vm.step(cursor);
  EXPECT_TRUE(contextual.success) << contextual.errorMessage;
  EXPECT_EQ(h.vm.cellReferenceOf(contextualOutput),
            h.vm.cellReferenceOf(handle));
}

TEST(VortexClosureTest, CheckClosureCallRejectsInvalidModeAndInvalidHandle) {
  const auto runInvalid = [](const int invalidCase) {
    Harness h;
    const auto ret = h.op(OpcodeKind::ReturnFrame);
    const auto frame =
        h.vm.makeFrameDescriptor(FrameDescriptorSpec{.entry = ret});
    ASSERT_TRUE(frame);
    h.core.bindInput(ret, *frame);
    const auto descriptor = h.vm.makeClosureDescriptor(
        ClosureDescriptorSpec{.frameDescriptor = *frame});
    ASSERT_TRUE(descriptor);
    const auto handle = h.arena.makeCell("closure handle");
    const auto create = h.op(OpcodeKind::CreateClosure);
    h.core.bindInput(create, *descriptor);
    h.core.bindOutput(create, handle);
    const auto malformed = h.arena.makeCell("not a closure");
    const auto mode =
        invalidCase == 1
            ? h.arena.makeScalarCell(0.0)
            : h.arena.makeScalarCell(std::int64_t{invalidCase == 0 ? 99 : 0});
    const auto count  = invalidCase == 2
                            ? h.arena.makeScalarCell(0.0)
                            : h.arena.makeScalarCell(std::int64_t{0});
    const auto output = h.arena.makeScalarCell(std::int64_t{123});
    const auto check  = h.op(OpcodeKind::CheckClosureCall);
    h.core.bindInput(check, invalidCase == 3 ? malformed : handle);
    h.core.bindInput(check, mode);
    h.core.bindInput(check, count);
    h.core.bindOutput(check, output);
    h.next(create, check);
    const auto cursor = h.cursor(create);
    ASSERT_TRUE(h.vm.step(cursor).success);
    const auto refused = h.vm.step(cursor);
    EXPECT_FALSE(refused.success);
    EXPECT_EQ(h.vm.getCursorOpcode(cursor), check);
    EXPECT_EQ(h.arena.asInt64(output), 123);
    EXPECT_FALSE(h.vm.cellReferenceOf(output));
  };
  runInvalid(0);
  runInvalid(1);
  runInvalid(2);
  runInvalid(3);
}

TEST(VortexClosureTest,
     AppliedPresenceDistinguishesOmittedFromExplicitEmptyStream) {
  Harness h;
  const auto context = h.arena.makeCell("context frame port");
  const auto formal  = h.arena.makeCell("formal frame port");
  const auto present = h.arena.makeCell("presence frame port");
  const auto ret     = h.op(OpcodeKind::ReturnFrame);
  const auto frame   = h.vm.makeFrameDescriptor(FrameDescriptorSpec{
        .entry         = ret,
        .locals        = {context, formal, present},
        .argumentPorts = {{context, FramePortKind::Stream},
                          {formal, FramePortKind::Stream},
                          {present, FramePortKind::Cell}},
        .resultPorts   = {{present, FramePortKind::Cell}},
  });
  ASSERT_TRUE(frame);
  h.core.bindInput(ret, *frame);
  const auto descriptor = h.vm.makeClosureDescriptor(ClosureDescriptorSpec{
      .frameDescriptor    = *frame,
      .captureStreamCount = 0,
      .minFormalCount     = 0,
      .maxFormalCount     = 1,
      .frameInputs        = {{ClosureInputSource::Context, 0},
                             {ClosureInputSource::Applied, 0},
                             {ClosureInputSource::AppliedPresence, 0}},
  });
  ASSERT_TRUE(descriptor);

  const auto handle = h.arena.makeCell("closure handle");
  const auto create = h.op(OpcodeKind::CreateClosure);
  h.core.bindInput(create, *descriptor);
  h.core.bindOutput(create, handle);
  const auto clearContext  = h.op(OpcodeKind::StreamClear);
  const auto contextStream = h.arena.makeCell("empty context stream");
  h.core.bindOutput(clearContext, contextStream);
  const auto clearActual  = h.op(OpcodeKind::StreamClear);
  const auto actualStream = h.arena.makeCell("explicit empty actual stream");
  h.core.bindOutput(clearActual, actualStream);
  const auto missingResult = h.arena.makeCell("omitted presence result");
  const auto omitted =
      h.applyClosure(handle, contextStream, std::vector<CellRef>{},
                     std::vector<CellRef>{missingResult});
  const auto suppliedResult = h.arena.makeCell("supplied presence result");
  const auto supplied =
      h.applyClosure(handle, contextStream, std::vector<CellRef>{actualStream},
                     std::vector<CellRef>{suppliedResult});
  h.next(create, clearContext);
  h.next(clearContext, clearActual);
  h.next(clearActual, omitted);
  h.next(omitted, supplied);
  const auto cursor = h.cursor(create);

  const auto execution = h.vm.run(cursor, 100);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  const auto missingCell  = h.vm.resultCell(cursor, missingResult);
  const auto suppliedCell = h.vm.resultCell(cursor, suppliedResult);
  ASSERT_TRUE(missingCell);
  ASSERT_TRUE(suppliedCell);
  EXPECT_EQ(h.arena.asBool(*missingCell), false);
  EXPECT_EQ(h.arena.asBool(*suppliedCell), true);
}

TEST(VortexClosureTest, CheckClosureCallFreezesHandleBeforeSourceStreamRebind) {
  Harness h;
  const auto captured = h.arena.makeCell("captured stream");
  const auto ret      = h.op(OpcodeKind::ReturnFrame);
  const auto frame    = h.vm.makeFrameDescriptor(FrameDescriptorSpec{
         .entry         = ret,
         .locals        = {captured},
         .argumentPorts = {{captured, FramePortKind::Stream}},
         .resultPorts   = {{captured, FramePortKind::Stream}},
  });
  ASSERT_TRUE(frame);
  h.core.bindInput(ret, *frame);
  const auto descriptor = h.vm.makeClosureDescriptor(ClosureDescriptorSpec{
      .frameDescriptor    = *frame,
      .captureStreamCount = 1,
      .maxFormalCount     = 0,
      .frameInputs        = {{ClosureInputSource::Capture, 0}},
  });
  ASSERT_TRUE(descriptor);

  const auto markerF      = h.arena.makeCell("closure F result");
  const auto markerG      = h.arena.makeCell("closure G result");
  const auto captureF     = h.arena.makeCell("capture F");
  const auto captureG     = h.arena.makeCell("capture G");
  const auto makeCaptureF = h.op(OpcodeKind::StreamAppend);
  h.core.bindInput(makeCaptureF, markerF);
  h.core.bindOutput(makeCaptureF, captureF);
  const auto makeCaptureG = h.op(OpcodeKind::StreamAppend);
  h.core.bindInput(makeCaptureG, markerG);
  h.core.bindOutput(makeCaptureG, captureG);

  const auto handleF = h.arena.makeCell("closure F handle");
  const auto createF = h.makeClosure(*descriptor, captureF, handleF);
  const auto source  = h.arena.makeCell("mutable callee stream");
  const auto appendF = h.op(OpcodeKind::StreamAppend);
  h.core.bindInput(appendF, handleF);
  h.core.bindOutput(appendF, source);
  const auto checked = h.arena.makeCell("frozen closure reference");
  const auto check =
      h.checkClosureCall(source, ClosureCallMode::Explicit, 0, checked);

  const auto handleG     = h.arena.makeCell("closure G handle");
  const auto createG     = h.makeClosure(*descriptor, captureG, handleG);
  const auto clearSource = h.op(OpcodeKind::StreamClear);
  h.core.bindOutput(clearSource, source);
  const auto appendG = h.op(OpcodeKind::StreamAppend);
  h.core.bindInput(appendG, handleG);
  h.core.bindOutput(appendG, source);
  const auto clearContext = h.op(OpcodeKind::StreamClear);
  const auto context      = h.arena.makeCell("empty hidden context");
  h.core.bindOutput(clearContext, context);
  const auto result = h.arena.makeCell("closure apply result");
  const auto apply  = h.applyClosure(checked, context, std::vector<CellRef>{},
                                     std::vector<CellRef>{result});

  h.next(makeCaptureF, makeCaptureG);
  h.next(makeCaptureG, createF);
  h.next(createF, appendF);
  h.next(appendF, check);
  h.next(check, createG);
  h.next(createG, clearSource);
  h.next(clearSource, appendG);
  h.next(appendG, clearContext);
  h.next(clearContext, apply);
  const auto cursor = h.cursor(makeCaptureF);

  const auto execution = h.vm.run(cursor, 100);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  EXPECT_EQ(h.vm.resultCells(cursor, result), (std::vector<CellRef>{markerF}));
  EXPECT_EQ(h.vm.cellReferenceOf(checked), h.vm.cellReferenceOf(handleF));
}

TEST(VortexClosureTest, ContextualApplyUsesCandidateAsContextAndFirstFormal) {
  Harness h;
  const auto contextPort = h.arena.makeCell("hidden context port");
  const auto formalPort  = h.arena.makeCell("first formal port");
  const auto ret         = h.op(OpcodeKind::ReturnFrame);
  const auto frame       = h.vm.makeFrameDescriptor(FrameDescriptorSpec{
            .entry         = ret,
            .locals        = {contextPort, formalPort},
            .argumentPorts = {{contextPort, FramePortKind::Stream},
                              {formalPort, FramePortKind::Stream}},
            .resultPorts   = {{contextPort, FramePortKind::Stream},
                              {formalPort, FramePortKind::Stream}},
  });
  ASSERT_TRUE(frame);
  h.core.bindInput(ret, *frame);
  const auto descriptor = h.vm.makeClosureDescriptor(ClosureDescriptorSpec{
      .frameDescriptor    = *frame,
      .captureStreamCount = 0,
      .minFormalCount     = 0,
      .maxFormalCount     = 1,
      .frameInputs        = {{ClosureInputSource::Context, 0},
                             {ClosureInputSource::Applied, 0}},
  });
  ASSERT_TRUE(descriptor);
  const auto handle = h.arena.makeCell("contextual closure handle");
  const auto create = h.op(OpcodeKind::CreateClosure);
  h.core.bindInput(create, *descriptor);
  h.core.bindOutput(create, handle);
  const auto candidate       = h.arena.makeCell("path candidate");
  const auto candidateStream = h.arena.makeCell("candidate stream");
  const auto appendCandidate = h.op(OpcodeKind::StreamAppend);
  h.core.bindInput(appendCandidate, candidate);
  h.core.bindOutput(appendCandidate, candidateStream);
  const auto checked = h.arena.makeCell("checked closure handle");
  const auto check =
      h.checkClosureCall(handle, ClosureCallMode::Contextual, 0, checked);
  const auto outputs = std::vector<CellRef>{h.arena.makeCell("context result"),
                                            h.arena.makeCell("formal result")};
  const auto apply   = h.op(OpcodeKind::ApplyClosure);
  h.core.bindInput(apply, checked);
  h.core.bindInput(apply, h.arena.makeScalarCell(static_cast<std::int64_t>(
                              ClosureCallMode::Contextual)));
  h.core.bindInput(apply, candidateStream);
  h.core.bindInput(apply, h.arena.makeScalarCell(std::int64_t{0}));
  for (const auto output : outputs) h.core.bindOutput(apply, output);
  h.next(create, appendCandidate);
  h.next(appendCandidate, check);
  h.next(check, apply);
  const auto cursor = h.cursor(create);

  const auto execution = h.vm.run(cursor, 100);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  EXPECT_EQ(h.vm.resultCells(cursor, outputs[0]),
            (std::vector<CellRef>{candidate}));
  EXPECT_EQ(h.vm.resultCells(cursor, outputs[1]),
            (std::vector<CellRef>{candidate}));
}

TEST(VortexClosureTest,
     CaptureAndAppliedStreamsPreserveIdentityOrderAndDuplicates) {
  Harness h;
  const auto descriptor  = h.streamFrame();
  const auto capturedA   = h.arena.makeScalarCell(std::int64_t{11});
  const auto capturedB   = h.arena.makeScalarCell(std::int64_t{12});
  const auto appliedA    = h.arena.makeScalarCell(std::int64_t{21});
  const auto appliedB    = h.arena.makeScalarCell(std::int64_t{22});
  const auto captureSlot = h.arena.makeCell("capture stream");
  const auto appliedSlot = h.arena.makeCell("applied stream");
  const auto captured = std::vector<CellRef>{capturedB, capturedA, capturedB};
  const auto applied  = std::vector<CellRef>{appliedA, appliedB, appliedA};
  const auto appendCaptures = h.op(OpcodeKind::StreamAppend);
  for (const auto cell : captured) h.core.bindInput(appendCaptures, cell);
  h.core.bindOutput(appendCaptures, captureSlot);
  const auto appendApplied = h.op(OpcodeKind::StreamAppend);
  for (const auto cell : applied) h.core.bindInput(appendApplied, cell);
  h.core.bindOutput(appendApplied, appliedSlot);
  const auto handle        = h.arena.makeCell("closure handle");
  const auto create        = h.makeClosure(descriptor, captureSlot, handle);
  const auto outputCapture = h.arena.makeCell("returned captures");
  const auto outputApplied = h.arena.makeCell("returned applied");
  const auto apply =
      h.applyClosure(handle, appliedSlot, outputCapture, outputApplied);
  h.next(appendCaptures, appendApplied);
  h.next(appendApplied, create);
  h.next(create, apply);
  const auto cursor = h.cursor(appendCaptures);

  const auto execution = h.vm.run(cursor, 100);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  EXPECT_EQ(h.vm.resultCells(cursor, outputCapture), captured);
  EXPECT_EQ(h.vm.resultCells(cursor, outputApplied), applied);
}

TEST(VortexClosureTest,
     FactorySnapshotsCaptureMembershipAndRejectsBadCardinality) {
  Harness h;
  const auto descriptor = h.streamFrame();
  auto captures         = std::vector<std::vector<CellRef>>{
      {h.arena.makeCell("a"), h.arena.makeCell("b")}};
  const auto originalCaptures = captures;
  const auto before           = h.arena.cellCount();
  const auto closure          = h.vm.makeClosureValue(descriptor, captures);
  ASSERT_TRUE(closure);
  EXPECT_GT(h.arena.cellCount(), before);
  captures.front().front() = h.arena.makeCell("caller rebound capture");
  const auto parsed        = h.vm.closureValue(*closure);
  ASSERT_TRUE(parsed);
  ASSERT_EQ(parsed->captures.size(), 1u);
  EXPECT_EQ(parsed->captures.front(), originalCaptures.front());

  const auto unchanged = h.arena.cellCount();
  EXPECT_FALSE(h.vm.makeClosureValue(descriptor, {}));
  EXPECT_EQ(h.arena.cellCount(), unchanged);
  const std::vector<std::vector<CellRef>> emptyStream{{}};
  const auto emptyClosure = h.vm.makeClosureValue(descriptor, emptyStream);
  ASSERT_TRUE(emptyClosure);
  const auto emptyParsed = h.vm.closureValue(*emptyClosure);
  ASSERT_TRUE(emptyParsed);
  ASSERT_EQ(emptyParsed->captures.size(), 1u);
  EXPECT_TRUE(emptyParsed->captures.front().empty());
}

TEST(VortexClosureTest, CreateApplyOpcodesAndReturnedDataReferencesWork) {
  Harness h;
  const auto descriptor    = h.streamFrame();
  const auto captured      = h.arena.makeScalarCell(std::int64_t{7});
  const auto actual        = h.arena.makeScalarCell(std::int64_t{9});
  const auto captureSlot   = h.arena.makeCell("capture");
  const auto appliedSlot   = h.arena.makeCell("apply");
  const auto captureSource = h.op(OpcodeKind::CopyCellRefs);
  h.core.bindInput(captureSource, captured);
  h.core.bindOutput(captureSource, captureSlot);
  const auto appliedSource = h.op(OpcodeKind::CopyCellRefs);
  h.core.bindInput(appliedSource, actual);
  h.core.bindOutput(appliedSource, appliedSlot);
  const auto handle        = h.arena.makeCell("handle");
  const auto create        = h.makeClosure(descriptor, captureSlot, handle);
  const auto resultCapture = h.arena.makeCell("capture result");
  const auto resultApplied = h.arena.makeCell("applied result");
  const auto apply =
      h.applyClosure(handle, appliedSlot, resultCapture, resultApplied);
  h.next(captureSource, appliedSource);
  h.next(appliedSource, create);
  h.next(create, apply);
  const auto cursor = h.cursor(captureSource);

  const auto execution = h.vm.run(cursor, 100);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  EXPECT_EQ(h.vm.getOpcodeKind(create), OpcodeKind::CreateClosure);
  EXPECT_EQ(h.vm.getOpcodeKind(apply), OpcodeKind::ApplyClosure);
  EXPECT_EQ(h.vm.resultCells(cursor, resultCapture),
            (std::vector<CellRef>{captured}));
  EXPECT_EQ(h.vm.resultCells(cursor, resultApplied),
            (std::vector<CellRef>{actual}));
}

TEST(VortexClosureTest, RepeatedApplyUsesFreshFrameLocals) {
  Harness h;
  const auto descriptor   = h.streamFrame();
  const auto captured     = h.arena.makeCell("same capture");
  const auto applied      = h.arena.makeCell("same applied");
  const auto handle       = h.arena.makeCell("closure");
  const auto create       = h.makeClosure(descriptor, captured, handle);
  const auto firstCapture = h.arena.makeCell("first capture result");
  const auto firstApplied = h.arena.makeCell("first applied result");
  const auto firstApply =
      h.applyClosure(handle, applied, firstCapture, firstApplied);
  const auto secondCapture = h.arena.makeCell("second capture result");
  const auto secondApplied = h.arena.makeCell("second applied result");
  const auto secondApply =
      h.applyClosure(handle, applied, secondCapture, secondApplied);
  h.next(create, firstApply);
  h.next(firstApply, secondApply);
  const auto cursor = h.cursor(create);

  const auto execution = h.vm.run(cursor, 100);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  EXPECT_EQ(h.vm.resultCells(cursor, firstCapture),
            (std::vector<CellRef>{captured}));
  EXPECT_EQ(h.vm.resultCells(cursor, secondCapture),
            (std::vector<CellRef>{captured}));
  EXPECT_EQ(h.vm.resultCells(cursor, firstApplied),
            (std::vector<CellRef>{applied}));
  EXPECT_EQ(h.vm.resultCells(cursor, secondApplied),
            (std::vector<CellRef>{applied}));
  EXPECT_EQ(h.vm.activationDepth(cursor), 0u);
}

TEST(VortexClosureTest, CapturesRetainLiveCellIdentityWhenContentChanges) {
  Harness h;
  const auto descriptor = h.streamFrame();
  const auto captured   = h.arena.makeCell("before");
  const auto closure    = h.vm.makeClosureValue(
      descriptor, std::vector<std::vector<CellRef>>{{captured}});
  ASSERT_TRUE(closure);
  const auto newSpan = h.arena.intern("after");
  const std::vector<xanadu::PrimediaSpan> replacement{newSpan};
  ASSERT_TRUE(h.arena.setContent(captured, replacement));
  const auto applied       = h.arena.makeCell("argument");
  const auto outputCapture = h.arena.makeCell("capture output");
  const auto outputApplied = h.arena.makeCell("applied output");
  const auto apply =
      h.applyClosure(*closure, applied, outputCapture, outputApplied);
  const auto cursor = h.cursor(apply);

  const auto execution = h.vm.run(cursor, 40);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  const auto results = h.vm.resultCells(cursor, outputCapture);
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results.front(), captured);
  EXPECT_EQ(h.core.render(results.front()),
            std::optional{CellValue{std::string{"after"}}});
}

TEST(VortexClosureTest,
     ClosureReturnedFromFrameSurvivesCursorCloseAndCanApply) {
  Harness h;
  const auto innerDescriptor = h.streamFrame();
  const auto inputValue      = h.arena.makeCell("owned frame data");
  const auto localData       = h.arena.makeCell("fresh frame-local data");
  const auto closureLocal    = h.arena.makeCell("closure return local");
  const auto box             = h.op(OpcodeKind::VqlBoxScalar);
  h.core.bindInput(box, inputValue);
  h.core.bindOutput(box, localData);
  const auto create = h.op(OpcodeKind::CreateClosure);
  h.core.bindInput(create, innerDescriptor);
  h.core.bindInput(create, localData);
  h.core.bindOutput(create, closureLocal);
  const auto ret = h.op(OpcodeKind::ReturnFrame);
  h.next(box, create);
  h.next(create, ret);
  const auto outerDescriptor = h.vm.makeFrameDescriptor(FrameDescriptorSpec{
      .entry       = box,
      .locals      = {localData, closureLocal},
      .resultPorts = {closureLocal},
  });
  ASSERT_TRUE(outerDescriptor);
  h.core.bindInput(ret, *outerDescriptor);
  const auto invocation = h.op(OpcodeKind::InvokeFrame);
  h.core.bindInput(invocation, *outerDescriptor);
  const auto returnedHandle = h.arena.makeCell("returned closure");
  h.core.bindOutput(invocation, returnedHandle);
  const auto applied       = h.arena.makeCell("later argument");
  const auto outputCapture = h.arena.makeCell("capture result");
  const auto outputApplied = h.arena.makeCell("applied result");
  const auto apply =
      h.applyClosure(returnedHandle, applied, outputCapture, outputApplied);
  const auto originalCellCount = h.arena.cellCount();
  h.next(invocation, apply);
  const auto cursor = h.cursor(invocation);

  ASSERT_TRUE(h.vm.step(cursor).success);
  ASSERT_EQ(h.vm.activationDepth(cursor), 1u);
  ASSERT_TRUE(h.vm.step(cursor).success);
  ASSERT_TRUE(h.vm.step(cursor).success);
  ASSERT_TRUE(h.vm.step(cursor).success);
  EXPECT_EQ(h.vm.activationDepth(cursor), 0u);
  EXPECT_TRUE(h.vm.resultCell(cursor, returnedHandle));
  const auto localClosure = h.vm.resultCell(cursor, returnedHandle);
  ASSERT_TRUE(localClosure);
  const auto parsed = h.vm.closureValue(*localClosure);
  ASSERT_TRUE(parsed);
  ASSERT_EQ(parsed->captures.size(), 1u);
  ASSERT_EQ(parsed->captures.front().size(), 1u);
  EXPECT_GE(parsed->captures.front().front(), originalCellCount);
  EXPECT_EQ(h.arena.textOf(parsed->captures.front().front()),
            "owned frame data");
  h.vm.setCursorOpcode(cursor, std::nullopt);

  const auto laterCursor = h.cursor(apply);
  const auto execution   = h.vm.run(laterCursor, 60);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  EXPECT_EQ(h.vm.resultCells(laterCursor, outputCapture),
            (std::vector<CellRef>{parsed->captures.front().front()}));
  EXPECT_EQ(h.vm.resultCells(laterCursor, outputApplied),
            (std::vector<CellRef>{applied}));
}

TEST(VortexClosureTest, FailedCreationDoesNotPublishHandleAndCanRetry) {
  Harness h;
  const auto descriptor   = h.streamFrame();
  const auto capture      = h.arena.makeCell("capture");
  const auto handle       = h.arena.makeCell("old handle");
  const auto create       = h.makeClosure(descriptor, capture, handle);
  const auto cursor       = h.cursor(create);
  const auto original     = h.vm.resultCells(cursor, handle);
  auto limits             = h.vm.closureLimits();
  limits.maxValuesCreated = 0;
  h.vm.setClosureLimits(limits);
  const auto before = h.arena.cellCount();
  EXPECT_FALSE(h.vm.step(cursor).success);
  EXPECT_EQ(h.arena.cellCount(), before);
  EXPECT_EQ(h.vm.resultCells(cursor, handle), original);
  EXPECT_EQ(h.vm.getCursorOpcode(cursor), create);

  limits.maxValuesCreated = 16;
  h.vm.setClosureLimits(limits);
  const auto retried = h.vm.run(cursor, 20);
  ASSERT_TRUE(retried.success) << retried.errorMessage;
  ASSERT_TRUE(h.vm.resultCell(cursor, handle));
  EXPECT_TRUE(h.vm.closureValue(*h.vm.resultCell(cursor, handle)));
}

TEST(VortexClosureTest, ChoiceBacktrackingRestoresClosureAndApplyResults) {
  Harness h;
  const auto descriptor     = h.streamFrame();
  const auto captured       = h.arena.makeCell("capture");
  const auto applied        = h.arena.makeCell("argument");
  const auto handle         = h.arena.makeCell("handle");
  const auto create         = h.makeClosure(descriptor, captured, handle);
  const auto streamRegister = h.arena.makeCell("choice stream register");
  const auto establishStreamCursor = h.op(OpcodeKind::StreamClear);
  h.core.bindOutput(establishStreamCursor, streamRegister);
  const auto outCapture = h.arena.makeCell("output capture");
  const auto outApplied = h.arena.makeCell("output applied");
  const auto apply = h.applyClosure(handle, applied, outCapture, outApplied);
  h.next(establishStreamCursor, create);
  h.next(create, apply);
  const auto cursor = h.cursor(establishStreamCursor);
  ASSERT_TRUE(h.vm.step(cursor).success);
  ASSERT_TRUE(h.vm.step(cursor).success);
  const auto resume = h.vm.getCursorOpcode(cursor);
  ASSERT_TRUE(resume);
  const auto priorCapture = h.vm.resultCells(cursor, outCapture);
  const auto priorApplied = h.vm.resultCells(cursor, outApplied);
  h.vm.pushChoicePoint(cursor, *resume);
  const auto firstStep = h.vm.step(cursor);
  ASSERT_TRUE(firstStep.success) << firstStep.errorMessage;
  const auto secondStep = h.vm.step(cursor);
  ASSERT_TRUE(secondStep.success) << secondStep.errorMessage;
  EXPECT_EQ(h.vm.resultCells(cursor, outCapture),
            (std::vector<CellRef>{captured}));
  ASSERT_TRUE(h.vm.backtrack(cursor));
  EXPECT_EQ(h.vm.resultCells(cursor, outCapture), priorCapture);
  EXPECT_EQ(h.vm.resultCells(cursor, outApplied), priorApplied);
  const auto replay = h.vm.run(cursor, 40);
  ASSERT_TRUE(replay.success) << replay.errorMessage;
  EXPECT_EQ(h.vm.resultCells(cursor, outCapture),
            (std::vector<CellRef>{captured}));
  EXPECT_EQ(h.vm.resultCells(cursor, outApplied),
            (std::vector<CellRef>{applied}));
}

TEST(VortexClosureTest,
     BacktrackingBeforeCreateClearsHandleWithoutRefundingQuota) {
  Harness h;
  const auto descriptor   = h.streamFrame();
  const auto captured     = h.arena.makeCell("capture");
  const auto handle       = h.arena.makeCell("handle");
  const auto create       = h.makeClosure(descriptor, captured, handle);
  const auto cursor       = h.cursor(create);
  auto limits             = h.vm.closureLimits();
  limits.maxValuesCreated = 1;
  h.vm.setClosureLimits(limits);
  h.vm.pushChoicePoint(cursor, create);

  ASSERT_TRUE(h.vm.step(cursor).success);
  ASSERT_TRUE(h.vm.resultCell(cursor, handle));
  ASSERT_TRUE(h.vm.backtrack(cursor));
  EXPECT_FALSE(h.vm.cellReferenceOf(handle));
  EXPECT_EQ(h.vm.getCursorOpcode(cursor), create);
  const auto beforeRetry = h.arena.cellCount();
  EXPECT_FALSE(h.vm.step(cursor).success);
  EXPECT_EQ(h.arena.cellCount(), beforeRetry);
  EXPECT_FALSE(h.vm.cellReferenceOf(handle));
}

TEST(VortexClosureTest, ClosedCursorRetainsClosureForLaterCursorAndCompaction) {
  Harness h;
  const auto descriptor  = h.streamFrame();
  const auto captured    = h.arena.makeCell("capture");
  const auto handle      = h.arena.makeCell("handle");
  const auto create      = h.makeClosure(descriptor, captured, handle);
  const auto firstCursor = h.cursor(create);
  ASSERT_TRUE(h.vm.run(firstCursor, 30).success);
  const auto closureHandle = h.vm.resultCell(firstCursor, handle);
  ASSERT_TRUE(closureHandle);
  h.vm.setCursorOpcode(firstCursor, std::nullopt);
  EXPECT_EQ(h.vm.activationDepth(firstCursor), 0u);

  const auto beforeCompact = h.vm.closureValue(*closureHandle);
  ASSERT_TRUE(beforeCompact);
  ASSERT_TRUE(h.arena.compact());
  const auto afterCompact = h.vm.closureValue(*closureHandle);
  ASSERT_TRUE(afterCompact);
  EXPECT_EQ(afterCompact->captures, beforeCompact->captures);

  const auto applied       = h.arena.makeCell("later argument");
  const auto outputCapture = h.arena.makeCell("later capture");
  const auto outputApplied = h.arena.makeCell("later applied");
  const auto apply =
      h.applyClosure(handle, applied, outputCapture, outputApplied);
  const auto laterCursor = h.cursor(apply);
  EXPECT_TRUE(h.vm.run(laterCursor, 30).success);
  EXPECT_EQ(h.vm.resultCells(laterCursor, outputCapture),
            (std::vector<CellRef>{captured}));
  EXPECT_EQ(h.vm.resultCells(laterCursor, outputApplied),
            (std::vector<CellRef>{applied}));
}

TEST(VortexClosureTest, ApplyingOnAnotherCursorWaitsForActiveFrameCursor) {
  Harness h;
  const auto descriptor   = h.streamFrame();
  const auto capture      = h.arena.makeCell("capture");
  const auto createHandle = h.arena.makeCell("created closure");
  const auto create       = h.makeClosure(descriptor, capture, createHandle);
  const auto creator      = h.cursor(create);
  ASSERT_TRUE(h.vm.run(creator, 20).success);
  const auto closureHandle = h.vm.resultCell(creator, createHandle);
  ASSERT_TRUE(closureHandle);
  h.vm.setCursorOpcode(creator, std::nullopt);

  const auto applied      = h.arena.makeCell("applied argument");
  const auto firstCapture = h.arena.makeCell("first output");
  const auto firstApplied = h.arena.makeCell("first applied output");
  const auto firstApply =
      h.applyClosure(*closureHandle, applied, firstCapture, firstApplied);
  const auto secondCapture = h.arena.makeCell("second output");
  const auto secondApplied = h.arena.makeCell("second applied output");
  const auto secondApply =
      h.applyClosure(*closureHandle, applied, secondCapture, secondApplied);
  const auto firstCursor  = h.cursor(firstApply);
  const auto secondCursor = h.cursor(secondApply);

  ASSERT_TRUE(h.vm.step(firstCursor).success);
  EXPECT_FALSE(h.vm.step(secondCursor).success);
  h.vm.setCursorOpcode(firstCursor, std::nullopt);
  EXPECT_TRUE(h.vm.step(secondCursor).success);
}

TEST(VortexClosureTest,
     DirectClosureFactoriesRespectCaptureAndDescriptorQuotas) {
  Harness h;
  const auto frame          = h.streamFrame();
  auto limits               = h.vm.closureLimits();
  const auto originalLimits = limits;
  const auto parsedFrame    = h.vm.closureDescriptor(frame);
  ASSERT_TRUE(parsedFrame);
  const auto before           = h.arena.cellCount();
  limits.maxDescriptorRecords = 0;
  h.vm.setClosureLimits(limits);
  EXPECT_FALSE(h.vm.makeClosureDescriptor(ClosureDescriptorSpec{
      .frameDescriptor    = parsedFrame->frameDescriptor,
      .captureStreamCount = 1,
      .maxFormalCount     = 1,
      .frameInputs        = {{ClosureInputSource::Capture, 0},
                             {ClosureInputSource::Applied, 0}},
  }));
  EXPECT_EQ(h.arena.cellCount(), before);
  limits                 = originalLimits;
  limits.maxCapturedRefs = 0;
  h.vm.setClosureLimits(limits);
  const auto oneCapture =
      std::vector<std::vector<CellRef>>{{h.arena.makeCell()}};
  const auto afterInput = h.arena.cellCount();
  EXPECT_FALSE(h.vm.makeClosureValue(frame, oneCapture));
  EXPECT_EQ(h.arena.cellCount(), afterInput);

  limits                            = originalLimits;
  limits.maxCapturedRefs            = 1'000'000;
  limits.maxEnvironmentCellsCreated = 0;
  h.vm.setClosureLimits(limits);
  const std::vector<std::vector<CellRef>> emptyStream{{}};
  EXPECT_FALSE(h.vm.makeClosureValue(frame, emptyStream));
  EXPECT_EQ(h.arena.cellCount(), afterInput);

  limits.maxEnvironmentCellsCreated = 100;
  h.vm.setClosureLimits(limits);
  const auto beforeFirstEmpty = h.arena.cellCount();
  const auto firstEmpty       = h.vm.makeClosureValue(frame, emptyStream);
  ASSERT_TRUE(firstEmpty);
  const auto recordsCreated = h.arena.cellCount() - beforeFirstEmpty;
  ASSERT_GT(recordsCreated, 0u);
  limits.maxEnvironmentCellsCreated = recordsCreated;
  h.vm.setClosureLimits(limits);
  const auto beforeSecondEmpty = h.arena.cellCount();
  EXPECT_FALSE(h.vm.makeClosureValue(frame, emptyStream));
  EXPECT_EQ(h.arena.cellCount(), beforeSecondEmpty);
}

TEST(VortexClosureTest, MappedFanoutRefusesBeforeAllocatingFrameBacking) {
  Harness h;
  const auto firstArgument  = h.arena.makeCell("first stream port");
  const auto secondArgument = h.arena.makeCell("second stream port");
  const auto firstReturn    = h.op(OpcodeKind::ReturnFrame);
  const auto frame          = h.vm.makeFrameDescriptor(FrameDescriptorSpec{
               .entry         = firstReturn,
               .locals        = {firstArgument, secondArgument},
               .argumentPorts = {{firstArgument, FramePortKind::Stream},
                                 {secondArgument, FramePortKind::Stream}},
               .resultPorts   = {{firstArgument, FramePortKind::Stream},
                                 {secondArgument, FramePortKind::Stream}},
  });
  ASSERT_TRUE(frame);
  h.core.bindInput(firstReturn, *frame);
  const auto descriptor = h.vm.makeClosureDescriptor(ClosureDescriptorSpec{
      .frameDescriptor    = *frame,
      .captureStreamCount = 1,
      .maxFormalCount     = 0,
      .frameInputs        = {{ClosureInputSource::Capture, 0},
                             {ClosureInputSource::Capture, 0}},
  });
  ASSERT_TRUE(descriptor);
  const auto data = h.arena.makeCell("many fanout refs");
  std::vector<CellRef> references(32, data);
  const std::vector<std::vector<CellRef>> captures{references};
  const auto closure = h.vm.makeClosureValue(*descriptor, captures);
  ASSERT_TRUE(closure);
  const auto firstOutput  = h.arena.makeCell("first output");
  const auto secondOutput = h.arena.makeCell("second output");
  const auto apply        = h.op(OpcodeKind::ApplyClosure);
  h.core.bindInput(apply, *closure);
  h.core.bindInput(apply, h.arena.makeScalarCell(static_cast<std::int64_t>(
                              ClosureCallMode::Explicit)));
  h.core.bindInput(apply, h.arena.makeCell("empty hidden context"));
  h.core.bindInput(apply, h.arena.makeScalarCell(std::int64_t{0}));
  h.core.bindOutput(apply, firstOutput);
  h.core.bindOutput(apply, secondOutput);
  auto limits              = h.vm.closureLimits();
  limits.maxInvocationRefs = 63;
  h.vm.setClosureLimits(limits);
  const auto cursor      = h.cursor(apply);
  const auto before      = h.arena.cellCount();
  const auto priorFirst  = h.vm.resultCells(cursor, firstOutput);
  const auto priorSecond = h.vm.resultCells(cursor, secondOutput);

  const auto refused = h.vm.step(cursor);
  EXPECT_FALSE(refused.success);
  EXPECT_EQ(h.arena.cellCount(), before);
  EXPECT_EQ(h.vm.activationDepth(cursor), 0u);
  EXPECT_EQ(h.vm.getCursorOpcode(cursor), apply);
  EXPECT_EQ(h.vm.resultCells(cursor, firstOutput), priorFirst);
  EXPECT_EQ(h.vm.resultCells(cursor, secondOutput), priorSecond);
}

TEST(VortexClosureTest,
     ConditionsPipelinesAndStdlibCallsRefuseClosureBeforeEffects) {
  Harness h;
  zigzag::vortex::VortexStdLib stdlib{h.core, h.vm};
  const auto descriptor          = h.streamFrame();
  const auto capture             = h.arena.makeCell("unchanged capture input");
  const auto handle              = h.arena.makeCell("unchanged output handle");
  const auto createWithCondition = h.makeClosure(descriptor, capture, handle);
  h.core.attachPrecondition(createWithCondition, h.op(OpcodeKind::Nop));
  auto cursor                = h.cursor(createWithCondition);
  const auto beforeCondition = h.arena.cellCount();
  EXPECT_FALSE(h.vm.step(cursor).success);
  EXPECT_EQ(h.arena.cellCount(), beforeCondition);
  EXPECT_FALSE(h.vm.cellReferenceOf(handle));

  const auto pipelineParam = h.arena.makeCell("pipeline trigger");
  const auto owner         = h.arena.makeCell("owner unaffected by pipeline");
  const auto dimension =
      h.arena.makeScalarCell(static_cast<std::int64_t>(h.core.dims().step));
  const auto direction = h.arena.makeScalarCell(std::int64_t{1});
  const auto newCell   = h.op(OpcodeKind::New);
  h.core.bindInput(newCell, owner);
  h.core.bindInput(newCell, dimension);
  h.core.bindInput(newCell, direction);
  const auto pipelineHandle = h.arena.makeCell("pipeline output handle");
  const auto createWithPipeline =
      h.makeClosure(descriptor, pipelineParam, pipelineHandle);
  h.next(newCell, createWithPipeline);
  h.core.attachPipeline(pipelineParam, newCell);
  const auto add  = h.op(OpcodeKind::Add);
  const auto zero = h.arena.makeScalarCell(std::int64_t{0});
  const auto sum  = h.arena.makeCell("main opcode result");
  h.core.bindInput(add, pipelineParam);
  h.core.bindInput(add, zero);
  h.core.bindOutput(add, sum);
  cursor                    = h.cursor(add);
  const auto beforePipeline = h.arena.cellCount();
  EXPECT_FALSE(h.vm.step(cursor).success);
  EXPECT_EQ(h.arena.cellCount(), beforePipeline);
  EXPECT_FALSE(h.vm.cellReferenceOf(pipelineHandle));
  EXPECT_FALSE(h.arena.linked(owner, h.core.dims().step, DimVector::POS));

  const auto beforeStdlib = h.arena.cellCount();
  EXPECT_THROW(stdlib.call(createWithPipeline, {}), std::invalid_argument);
  EXPECT_EQ(h.arena.cellCount(), beforeStdlib);
  EXPECT_FALSE(h.vm.cellReferenceOf(pipelineHandle));
}

TEST(VortexClosureProfileTest,
     DisabledUnlessOptedInMeasuresCaptureShapeRefsAndRepeatedApply) {
  const char *profile = std::getenv("GLEDITOR_CLOSURE_PROFILE");
  if (profile == nullptr || std::string_view{profile} != "1")
    GTEST_SKIP() << "set GLEDITOR_CLOSURE_PROFILE=1 to collect observations";

  for (const std::size_t captureStreams : {0u, 1u, 8u}) {
    for (const std::size_t refsPerStream : {0u, 1u, 32u}) {
      Harness h;
      std::vector<CellRef> frameLocals;
      std::vector<FramePort> arguments;
      std::vector<FramePort> results;
      std::vector<ClosureFrameInput> frameInputs;
      for (std::size_t stream = 0; stream < captureStreams; ++stream) {
        const auto local = h.arena.makeCell("profile stream port");
        frameLocals.push_back(local);
        arguments.emplace_back(local, FramePortKind::Stream);
        results.emplace_back(local, FramePortKind::Stream);
        frameInputs.push_back({ClosureInputSource::Capture, stream});
      }
      const auto ret   = h.op(OpcodeKind::ReturnFrame);
      const auto frame = h.vm.makeFrameDescriptor(FrameDescriptorSpec{
          .entry         = ret,
          .locals        = std::move(frameLocals),
          .argumentPorts = std::move(arguments),
          .resultPorts   = std::move(results),
      });
      ASSERT_TRUE(frame);
      h.core.bindInput(ret, *frame);
      const auto descriptor = h.vm.makeClosureDescriptor(ClosureDescriptorSpec{
          .frameDescriptor    = *frame,
          .captureStreamCount = captureStreams,
          .maxFormalCount     = 0,
          .frameInputs        = std::move(frameInputs),
      });
      ASSERT_TRUE(descriptor);

      const auto datum = h.arena.makeCell("profile datum");
      std::vector<std::vector<CellRef>> captures(
          captureStreams, std::vector<CellRef>(refsPerStream, datum));
      const auto closure = h.vm.makeClosureValue(*descriptor, captures);
      ASSERT_TRUE(closure);

      std::optional<CellRef> firstApply;
      std::optional<CellRef> previous;
      for (std::size_t repetition = 0; repetition < 32; ++repetition) {
        const auto apply = h.op(OpcodeKind::ApplyClosure);
        h.core.bindInput(apply, *closure);
        h.core.bindInput(
            apply, h.arena.makeScalarCell(
                       static_cast<std::int64_t>(ClosureCallMode::Explicit)));
        h.core.bindInput(apply, h.arena.makeCell("empty hidden context"));
        h.core.bindInput(apply, h.arena.makeScalarCell(std::int64_t{0}));
        for (std::size_t stream = 0; stream < captureStreams; ++stream)
          h.core.bindOutput(apply, h.arena.makeCell("profile output"));
        if (previous)
          h.next(*previous, apply);
        else
          firstApply = apply;
        previous = apply;
      }
      ASSERT_TRUE(firstApply);
      const auto cursor      = h.cursor(*firstApply);
      const auto beforeCells = h.arena.cellCount();
      const auto started     = std::chrono::steady_clock::now();
      const auto execution   = h.vm.run(cursor, 1000);
      const auto elapsed     = std::chrono::steady_clock::now() - started;
      ASSERT_TRUE(execution.success) << execution.errorMessage;
      const auto elapsedNs =
          std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
      std::cout << "closure_profile capture_streams=" << captureStreams
                << " refs_per_stream=" << refsPerStream
                << " applies=32 elapsed_ns=" << elapsedNs
                << " arena_cells_delta=" << (h.arena.cellCount() - beforeCells)
                << '\n';
    }
  }
}

TEST(VortexClosureTest,
     RuntimeObjectClaimIsStructuralAndScalarReaderRefusesIt) {
  Harness h;
  const auto descriptor = h.streamFrame();
  const auto closure    = h.vm.makeClosureValue(
      descriptor, std::vector<std::vector<CellRef>>{{h.arena.makeCell("x")}});
  ASSERT_TRUE(closure);
  ASSERT_TRUE(h.core.dims().runtimeObjectClaim);
  const auto claimDim = *h.core.dims().runtimeObjectClaim;

  EXPECT_EQ(classifyRuntimeObjectRoot(h.arena, descriptor, claimDim),
            RuntimeObjectClaimResult::Claimed);
  EXPECT_EQ(classifyRuntimeObjectRoot(h.arena, *closure, claimDim),
            RuntimeObjectClaimResult::Claimed);
  const auto readDescriptor =
      readQueryScalar(h.arena, descriptor, h.core.dims().clone, claimDim);
  ASSERT_FALSE(readDescriptor);
  EXPECT_EQ(readDescriptor.error(), QueryScalarReadError::UnsupportedHandle);
  const auto readClosure =
      readQueryScalar(h.arena, *closure, h.core.dims().clone, claimDim);
  ASSERT_FALSE(readClosure);
  EXPECT_EQ(readClosure.error(), QueryScalarReadError::UnsupportedHandle);

  const auto typed = h.arena.makeScalarCell(std::int64_t{19});
  EXPECT_EQ(classifyRuntimeObjectRoot(h.arena, typed, claimDim),
            RuntimeObjectClaimResult::NoClaim);
  const auto typedRead =
      readQueryScalar(h.arena, typed, h.core.dims().clone, claimDim);
  ASSERT_TRUE(typedRead);
  EXPECT_EQ(*typedRead, QueryScalar{std::int64_t{19}});

  const auto plainProbe = h.arena.makeCell(runtimeObjectProbeText);
  EXPECT_EQ(classifyRuntimeObjectRoot(h.arena, plainProbe, claimDim),
            RuntimeObjectClaimResult::NoClaim);
  const auto plainRead =
      readQueryScalar(h.arena, plainProbe, h.core.dims().clone, claimDim);
  ASSERT_TRUE(plainRead);
  EXPECT_EQ(*plainRead, QueryScalar{std::string{runtimeObjectProbeText}});
  const auto marker = h.arena.linked(descriptor, claimDim, DimVector::POS);
  ASSERT_TRUE(marker);
  ASSERT_TRUE(h.arena.link(plainProbe, claimDim, DimVector::NEG, *marker));
  EXPECT_EQ(classifyRuntimeObjectRoot(h.arena, plainProbe, claimDim),
            RuntimeObjectClaimResult::NoClaim);
}

TEST(VortexClosureTest,
     RuntimeObjectProbeSeparatesLocalWorkLimitFromGlobalFuel) {
  {
    Harness h;
    const auto descriptor = h.streamFrame();
    ASSERT_TRUE(h.core.dims().runtimeObjectClaim);
    const auto claimDim = *h.core.dims().runtimeObjectClaim;
    EXPECT_EQ(classifyRuntimeObjectRoot(
                  h.arena, descriptor, claimDim,
                  {.maxRootRelations = 0, .maxMappingWork = 4096}),
              RuntimeObjectClaimResult::ProbeLimitExceeded);

    h.arena.setExecutionLimits(4096, 0, 1'048'576);
    EXPECT_EQ(classifyRuntimeObjectRoot(h.arena, descriptor, claimDim),
              RuntimeObjectClaimResult::FuelExhausted);
  }

  {
    Harness h;
    const auto descriptor = h.streamFrame();
    const auto spans      = h.arena.contentOf(descriptor);
    h.arena.setExecutionLimits(4096, spans.size(), 1'048'576);

    const auto parsed = h.vm.closureDescriptor(descriptor);
    ASSERT_FALSE(parsed);
    EXPECT_NE(
        parsed.error().message.find("Vortex evaluation work budget exhausted"),
        std::string::npos);
  }
}

TEST(VortexClosureTest,
     CreateClosureSnapshotsReboundVMStreamAndScalarReferenceSlots) {
  {
    Harness h;
    const auto descriptor    = h.streamFrame();
    const auto captureSlot   = h.arena.makeCell("capture stream register");
    const auto capturedA     = h.arena.makeCell("stream capture A");
    const auto capturedB     = h.arena.makeCell("stream capture B");
    const auto applied       = h.arena.makeCell("applied value");
    const auto handle        = h.arena.makeCell("stream closure handle");
    const auto outputCapture = h.arena.makeCell("stream capture result");
    const auto outputApplied = h.arena.makeCell("stream applied result");
    const auto clearBefore   = h.op(OpcodeKind::StreamClear);
    const auto appendA       = h.op(OpcodeKind::StreamAppend);
    const auto create        = h.makeClosure(descriptor, captureSlot, handle);
    const auto clearAfter    = h.op(OpcodeKind::StreamClear);
    const auto appendB       = h.op(OpcodeKind::StreamAppend);
    const auto apply =
        h.applyClosure(handle, applied, outputCapture, outputApplied);
    h.core.bindOutput(clearBefore, captureSlot);
    h.core.bindInput(appendA, capturedA);
    h.core.bindOutput(appendA, captureSlot);
    h.core.bindOutput(clearAfter, captureSlot);
    h.core.bindInput(appendB, capturedB);
    h.core.bindOutput(appendB, captureSlot);
    h.next(clearBefore, appendA);
    h.next(appendA, create);
    h.next(create, clearAfter);
    h.next(clearAfter, appendB);
    h.next(appendB, apply);
    const auto cursor = h.cursor(clearBefore);

    const auto execution = h.vm.run(cursor, 100);
    ASSERT_TRUE(execution.success) << execution.errorMessage;
    EXPECT_EQ(h.vm.resultCells(cursor, captureSlot),
              (std::vector<CellRef>{capturedB}));
    EXPECT_EQ(h.vm.resultCells(cursor, outputCapture),
              (std::vector<CellRef>{capturedA}));
    EXPECT_EQ(h.vm.resultCells(cursor, outputApplied),
              (std::vector<CellRef>{applied}));
  }

  {
    Harness h;
    const auto descriptor    = h.streamFrame();
    const auto captureSlot   = h.arena.makeCell("scalar reference register");
    const auto capturedA     = h.arena.makeCell("scalar capture A");
    const auto capturedB     = h.arena.makeCell("scalar capture B");
    const auto applied       = h.arena.makeCell("scalar applied value");
    const auto handle        = h.arena.makeCell("scalar closure handle");
    const auto outputCapture = h.arena.makeCell("scalar capture result");
    const auto outputApplied = h.arena.makeCell("scalar applied result");
    const auto copyA         = h.op(OpcodeKind::CopyCellRefs);
    h.core.bindInput(copyA, capturedA);
    h.core.bindOutput(copyA, captureSlot);
    const auto create = h.makeClosure(descriptor, captureSlot, handle);
    const auto copyB  = h.op(OpcodeKind::CopyCellRefs);
    h.core.bindInput(copyB, capturedB);
    h.core.bindOutput(copyB, captureSlot);
    const auto apply =
        h.applyClosure(handle, applied, outputCapture, outputApplied);
    h.next(copyA, create);
    h.next(create, copyB);
    h.next(copyB, apply);
    const auto cursor = h.cursor(copyA);

    const auto execution = h.vm.run(cursor, 100);
    ASSERT_TRUE(execution.success) << execution.errorMessage;
    EXPECT_EQ(h.vm.cellReferenceOf(captureSlot), capturedB);
    EXPECT_EQ(h.vm.resultCells(cursor, outputCapture),
              (std::vector<CellRef>{capturedA}));
    EXPECT_EQ(h.vm.resultCells(cursor, outputApplied),
              (std::vector<CellRef>{applied}));
  }
}

TEST(VortexClosureTest,
     DescriptorAndValueParsersRejectCorruptionAndBoundMetadataTraversal) {
  const auto restateText = [](Harness &h, const CellRef cell,
                              const std::string_view text) {
    const auto span = h.arena.intern(text);
    const std::vector<xanadu::PrimediaSpan> content{span};
    return h.arena.setContent(cell, content);
  };

  {
    Harness h;
    const auto descriptor = h.streamFrame();
    const auto first =
        h.arena.linked(descriptor, h.core.dims().grab, DimVector::POS);
    ASSERT_TRUE(first);
    CellRef tail = *first;
    for (std::size_t i = 0; i < 8; ++i) {
      const auto next =
          h.arena.linked(tail, h.core.dims().step, DimVector::POS);
      if (!next) break;
      tail = *next;
    }
    ASSERT_TRUE(h.arena.link(tail, h.core.dims().step, DimVector::POS, *first));
    EXPECT_FALSE(h.vm.closureDescriptor(descriptor));
  }

  {
    Harness h;
    const auto descriptor = h.streamFrame();
    const auto data       = h.arena.makeCell("captured data");
    const auto value      = h.vm.makeClosureValue(
        descriptor, std::vector<std::vector<CellRef>>{{data}});
    ASSERT_TRUE(value);
    const auto first =
        h.arena.linked(*value, h.core.dims().grab, DimVector::POS);
    ASSERT_TRUE(first);
    CellRef tail = *first;
    for (std::size_t i = 0; i < 8; ++i) {
      const auto next =
          h.arena.linked(tail, h.core.dims().step, DimVector::POS);
      if (!next) break;
      tail = *next;
    }
    ASSERT_TRUE(h.arena.link(tail, h.core.dims().step, DimVector::POS, *first));
    EXPECT_FALSE(h.vm.closureValue(*value));
  }

  {
    Harness h;
    const auto descriptor = h.streamFrame();
    const auto tag        = h.arena.linked(
        descriptor, *h.core.dims().runtimeObjectClaim, DimVector::POS);
    ASSERT_TRUE(tag);
    ASSERT_TRUE(restateText(h, *tag, "VORTEX_CLOSURE_DESCRIPTOR/2"));
    EXPECT_FALSE(h.vm.closureDescriptor(descriptor));
  }

  {
    Harness h;
    const auto descriptor = h.streamFrame();
    const auto value      = h.vm.makeClosureValue(
        descriptor, std::vector<std::vector<CellRef>>{{h.arena.makeCell("x")}});
    ASSERT_TRUE(value);
    const auto tag = h.arena.linked(*value, *h.core.dims().runtimeObjectClaim,
                                    DimVector::POS);
    ASSERT_TRUE(tag);
    ASSERT_TRUE(restateText(h, *tag, "VORTEX_CLOSURE_DESCRIPTOR/1"));
    EXPECT_FALSE(h.vm.closureValue(*value));
  }

  {
    Harness h;
    const auto descriptor = h.streamFrame();
    const auto first =
        h.arena.linked(descriptor, h.core.dims().grab, DimVector::POS);
    ASSERT_TRUE(first);
    ASSERT_TRUE(h.arena.setValueBits(*first, xanadu::ValueKind::None, 0));
    EXPECT_FALSE(h.vm.closureDescriptor(descriptor));
  }

  {
    Harness h;
    const auto descriptor = h.streamFrame();
    const auto first =
        h.arena.linked(descriptor, h.core.dims().grab, DimVector::POS);
    ASSERT_TRUE(first);
    const auto originalValue = h.arena.asInt64(*first);
    ASSERT_TRUE(originalValue);
    ASSERT_TRUE(restateText(h, *first, "unknown.closure.role"));
    EXPECT_EQ(h.arena.asInt64(*first), originalValue);
    EXPECT_FALSE(h.vm.closureDescriptor(descriptor));
  }

  {
    Harness h;
    const auto descriptor = h.streamFrame();
    const auto unrelated  = h.arena.makeCell("unrelated relation");
    ASSERT_TRUE(h.arena.link(descriptor, h.core.dims().contract, DimVector::POS,
                             unrelated));
    auto limits                 = h.vm.closureLimits();
    limits.maxMetadataRelations = 2;
    h.vm.setClosureLimits(limits);
    EXPECT_FALSE(h.vm.closureDescriptor(descriptor));
  }

  {
    Harness h;
    const auto descriptor       = h.streamFrame();
    auto limits                 = h.vm.closureLimits();
    limits.maxDescriptorRecords = 4;
    h.vm.setClosureLimits(limits);
    EXPECT_FALSE(h.vm.closureDescriptor(descriptor));
  }

  {
    Harness h;
    const auto descriptor = h.streamFrame();
    const auto value      = h.vm.makeClosureValue(
        descriptor, std::vector<std::vector<CellRef>>{{h.arena.makeCell("x")}});
    ASSERT_TRUE(value);
    auto limits            = h.vm.closureLimits();
    limits.maxValueRecords = 2;
    h.vm.setClosureLimits(limits);
    EXPECT_FALSE(h.vm.closureValue(*value));
  }

  {
    Harness h;
    const auto descriptor = h.streamFrame();
    const auto value      = h.vm.makeClosureValue(
        descriptor, std::vector<std::vector<CellRef>>{{h.arena.makeCell("x")}});
    ASSERT_TRUE(value);
    const auto first =
        h.arena.linked(*value, h.core.dims().grab, DimVector::POS);
    ASSERT_TRUE(first);
    ASSERT_TRUE(h.arena.setValueBits(*first, xanadu::ValueKind::None, 0));
    EXPECT_FALSE(h.vm.closureValue(*value));
  }

  {
    Harness h;
    const auto descriptor = h.streamFrame();
    const auto value      = h.vm.makeClosureValue(
        descriptor, std::vector<std::vector<CellRef>>{{h.arena.makeCell("x")}});
    ASSERT_TRUE(value);
    const auto first =
        h.arena.linked(*value, h.core.dims().grab, DimVector::POS);
    ASSERT_TRUE(first);
    const auto originalValue = h.arena.asInt64(*first);
    ASSERT_TRUE(originalValue);
    ASSERT_TRUE(restateText(h, *first, "unknown.value.role"));
    EXPECT_EQ(h.arena.asInt64(*first), originalValue);
    EXPECT_FALSE(h.vm.closureValue(*value));
  }
}

TEST(VortexClosureTest, OperandWingsRefuseBeforeUnboundedMaterialization) {
  for (const bool excessiveInputs : {true, false}) {
    Harness h;
    const auto descriptor    = h.streamFrame();
    const auto capture       = h.arena.makeCell("capture");
    const auto output        = h.arena.makeCell("unpublished handle");
    const auto create        = h.makeClosure(descriptor, capture, output);
    auto limits              = h.vm.closureLimits();
    limits.maxCaptureStreams = 1;
    h.vm.setClosureLimits(limits);
    for (std::size_t index = 0; index < 512; ++index) {
      if (excessiveInputs)
        h.core.bindInput(create, capture);
      else
        h.core.bindOutput(create, h.arena.makeCell());
    }
    const auto cursor = h.cursor(create);
    const auto before = h.arena.cellCount();
    h.arena.setExecutionLimits(std::numeric_limits<std::uint32_t>::max(), 256,
                               std::numeric_limits<std::size_t>::max());
    const auto execution = h.vm.step(cursor);
    EXPECT_FALSE(execution.success);
    EXPECT_NE(execution.errorMessage.find("operand wing"), std::string::npos)
        << execution.errorMessage;
    EXPECT_EQ(h.arena.cellCount(), before);
    EXPECT_EQ(h.vm.getCursorOpcode(cursor), create);
    EXPECT_FALSE(h.vm.cellReferenceOf(output));
  }
  Harness h;
  const auto descriptor = h.streamFrame();
  const auto capture    = h.arena.makeCell("capture");
  const auto output     = h.arena.makeCell("unpublished handle");
  const auto create     = h.makeClosure(descriptor, capture, output);
  const auto first = h.arena.linked(create, h.core.dims().grab, DimVector::POS);
  ASSERT_TRUE(first);
  const auto second =
      h.arena.linked(*first, h.core.dims().step, DimVector::POS);
  ASSERT_TRUE(second);
  ASSERT_TRUE(
      h.arena.link(*second, h.core.dims().step, DimVector::POS, *first));
  const auto cursor    = h.cursor(create);
  const auto before    = h.arena.cellCount();
  const auto execution = h.vm.step(cursor);
  EXPECT_FALSE(execution.success);
  EXPECT_NE(execution.errorMessage.find("cycles"), std::string::npos);
  EXPECT_EQ(h.arena.cellCount(), before);
  EXPECT_FALSE(h.vm.cellReferenceOf(output));
}

} // namespace
} // namespace zigzag::vortex
