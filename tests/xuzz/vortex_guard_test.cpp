/**
 * @file vortex_guard_test.cpp
 * @brief Bounded runtime checks for opaque, frame-backed Vortex guards.
 */
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vortex/program_export.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace zigzag::vortex {
namespace {

using zigzag::CellRef;
using zigzag::DimVector;

struct TemporaryDirectory {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("vortex_guard_test_" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));

  TemporaryDirectory() { std::filesystem::create_directories(path); }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

struct Harness {
  zigzag::ArenaManifold arena;
  VortexCore core{arena};
  VortexVM vm{core};

  CellRef op(const OpcodeKind kind) { return vm.mintOpcode(kind); }

  void next(const CellRef from, const CellRef to) {
    ASSERT_TRUE(arena.link(from, core.dims().spin, DimVector::POS, to));
  }

  CellRef entry(const CellRef first) {
    auto last = first;
    while (const auto tail =
               arena.linked(last, core.dims().spin, DimVector::POS))
      last = *tail;
    next(last, op(OpcodeKind::Halt));
    return vm.spawnCursor(first, "guard-test");
  }

  CellRef booleanValidator() {
    const CellRef argument = arena.makeCell("validator argument");
    const CellRef result   = arena.makeCell("validator result");
    const CellRef copy     = op(OpcodeKind::CopyCellRefs);
    const CellRef ret      = op(OpcodeKind::ReturnFrame);
    core.bindInput(copy, argument);
    core.bindOutput(copy, result);
    next(copy, ret);
    const auto descriptor = vm.makeFrameDescriptor(FrameDescriptorSpec{
        .entry         = copy,
        .locals        = {argument, result},
        .argumentPorts = {argument},
        .resultPorts   = {result},
    });
    EXPECT_TRUE(descriptor);
    if (!descriptor) throw std::runtime_error(descriptor.error().message);
    core.bindInput(ret, *descriptor);
    return *descriptor;
  }

  CellRef pushGuard(const CellRef descriptor, const CellRef diagnostic,
                    const CellRef argument,
                    const std::optional<CellRef> tokenSlot = std::nullopt) {
    const CellRef push = op(OpcodeKind::PushGuard);
    const CellRef token =
        tokenSlot ? *tokenSlot : arena.makeCell("guard token");
    core.bindInput(push, descriptor);
    core.bindInput(push, diagnostic);
    core.bindInput(push, argument);
    core.bindOutput(push, token);
    return push;
  }

  CellRef guardDiagnostic(const std::string_view text) {
    return arena.makeCell(text);
  }
};

std::optional<CellRef>
contractValidatorInvoke(const Harness &h,
                        const xanadu::vql::CompilationResult &program) {
  for (const CellRef opcode : program.generatedOpcodes) {
    if (h.arena.textOf(opcode).starts_with("#INVOKE_CONTRACT_VALIDATOR "))
      return opcode;
  }
  return std::nullopt;
}

TEST(VortexGuardTest,
     CompilerValidatorDescriptorCanBeCapturedAndCheckedAsNativeFrame) {
  Harness h;
  const CellRef item = h.arena.makeCell("returned identity");
  xanadu::MultiStoreCoordinator coordinator(h.core);
  xanadu::vql::VQLCompiler compiler(coordinator, h.vm);
  const auto program = compiler.compile(
      "def pure fallback(): Cell := \"temporary default\"; "
      "def pure keep($item: Cell): Cell { +d.optional: Cell := "
      "fallback() } := $item; keep($item)",
      xanadu::vql::CompilationOptions{
          .initialBindings = {{"item", {item}}},
      });
  ASSERT_TRUE(program.success) << program.errorMessage;
  const auto invoke = contractValidatorInvoke(h, program);
  ASSERT_TRUE(invoke);
  ASSERT_EQ(h.vm.getOpcodeKind(*invoke), OpcodeKind::InvokeFrame);
  const auto invokeInputs = h.core.inputsOf(*invoke);
  ASSERT_GE(invokeInputs.size(), 2U);
  const CellRef descriptorCell = invokeInputs.front();
  const auto descriptor        = h.vm.frameDescriptor(descriptorCell);
  ASSERT_TRUE(descriptor) << descriptor.error().message;
  ASSERT_EQ(descriptor->resultPorts.size(), 1U);
  EXPECT_EQ(descriptor->resultPorts.front().kind, FramePortKind::Cell);
  ASSERT_EQ(descriptor->argumentPorts.size(), 2U);
  for (const auto &port : descriptor->argumentPorts)
    EXPECT_EQ(port.kind, FramePortKind::Stream);

  const CellRef diagnostic = h.guardDiagnostic("keep return contract");
  const CellRef push       = h.op(OpcodeKind::PushGuard);
  h.core.bindInput(push, descriptorCell);
  h.core.bindInput(push, diagnostic);
  for (std::size_t index = 0; index < descriptor->argumentPorts.size(); ++index)
    h.core.bindInput(push, item);
  const CellRef tokenSlot = h.arena.makeCell("guard token");
  h.core.bindOutput(push, tokenSlot);
  const CellRef check    = h.op(OpcodeKind::CheckGuards);
  const auto pushOutputs = h.core.outputsOf(push);
  ASSERT_EQ(pushOutputs.size(), 1U);
  const CellRef token = pushOutputs.front();
  const CellRef pop   = h.op(OpcodeKind::PopGuard);
  h.core.bindInput(pop, token);
  h.next(push, check);
  h.next(check, pop);
  const CellRef cursor = h.entry(push);

  const auto execution = h.vm.run(cursor, 10'000);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  EXPECT_FALSE(h.core.findDimension("d.optional"));
  EXPECT_FALSE(h.vm.hasActiveGuards());
  EXPECT_FALSE(h.vm.hasActiveFrames());
}

TEST(VortexGuardTest,
     FrozenLexicalStreamCapturesPreserveOrderDuplicatesAndEmptyStream) {
  Harness h;
  const CellRef item   = h.arena.makeCell("stream contract owner");
  const CellRef first  = h.arena.makeCell("first text");
  const CellRef middle = h.arena.makeScalarCell(std::int64_t{2});
  xanadu::MultiStoreCoordinator coordinator(h.core);
  xanadu::vql::VQLCompiler compiler(coordinator, h.vm);
  constexpr std::string_view source =
      "def pure identity(): Stream<Cell> := .; "
      "def pure keep($item: Cell): Cell { +d.first: String := "
      "$xs/identity()[1], +d.middle: Int := $xs/identity()[2], "
      "+d.last: String := $xs/identity()[3] } := $item; keep($item)";
  xanadu::vql::CompilationOptions options;
  options.initialBindings = {{"item", {item}}, {"xs", {first, middle, first}}};
  const auto program      = compiler.compile(source, options);
  ASSERT_TRUE(program.success) << program.errorMessage;
  const auto invoke = contractValidatorInvoke(h, program);
  ASSERT_TRUE(invoke);
  const auto invokeInputs = h.core.inputsOf(*invoke);
  ASSERT_GE(invokeInputs.size(), 2U);
  const auto descriptor = h.vm.frameDescriptor(invokeInputs.front());
  ASSERT_TRUE(descriptor) << descriptor.error().message;
  ASSERT_EQ(descriptor->argumentPorts.size(), 3U);
  EXPECT_EQ(descriptor->argumentPorts[0].kind, FramePortKind::Stream);
  EXPECT_EQ(descriptor->argumentPorts[1].kind, FramePortKind::Stream);
  EXPECT_EQ(descriptor->argumentPorts[2].kind, FramePortKind::Stream);

  // The compiler's frozen invocation slots provide the exact argument order:
  // context, returned subject, then the lexical Stream<Cell> capture.
  const CellRef streamSlot     = invokeInputs.back();
  const CellRef clearOriginal  = h.op(OpcodeKind::StreamClear);
  const CellRef appendOriginal = h.op(OpcodeKind::StreamAppend);
  h.core.bindOutput(clearOriginal, streamSlot);
  h.core.bindInput(appendOriginal, first);
  h.core.bindInput(appendOriginal, middle);
  h.core.bindInput(appendOriginal, first);
  h.core.bindOutput(appendOriginal, streamSlot);
  const CellRef pushOriginal = h.op(OpcodeKind::PushGuard);
  h.core.bindInput(pushOriginal, invokeInputs.front());
  h.core.bindInput(pushOriginal, h.guardDiagnostic("original stream"));
  h.core.bindInput(pushOriginal, item);
  h.core.bindInput(pushOriginal, item);
  h.core.bindInput(pushOriginal, streamSlot);
  const CellRef originalToken = h.arena.makeCell("original token");
  h.core.bindOutput(pushOriginal, originalToken);

  const CellRef clearRebound  = h.op(OpcodeKind::StreamClear);
  const CellRef appendRebound = h.op(OpcodeKind::StreamAppend);
  h.core.bindOutput(clearRebound, streamSlot);
  h.core.bindInput(appendRebound, middle);
  h.core.bindInput(appendRebound, first);
  h.core.bindInput(appendRebound, first);
  h.core.bindOutput(appendRebound, streamSlot);
  const CellRef pushRebound = h.op(OpcodeKind::PushGuard);
  h.core.bindInput(pushRebound, invokeInputs.front());
  h.core.bindInput(pushRebound, h.guardDiagnostic("rebound stream"));
  h.core.bindInput(pushRebound, item);
  h.core.bindInput(pushRebound, item);
  h.core.bindInput(pushRebound, streamSlot);
  const CellRef reboundToken = h.arena.makeCell("rebound token");
  h.core.bindOutput(pushRebound, reboundToken);

  const CellRef check       = h.op(OpcodeKind::CheckGuards);
  const CellRef popRebound  = h.op(OpcodeKind::PopGuard);
  const CellRef popOriginal = h.op(OpcodeKind::PopGuard);
  h.core.bindInput(popRebound, reboundToken);
  h.core.bindInput(popOriginal, originalToken);
  h.next(clearOriginal, appendOriginal);
  h.next(appendOriginal, pushOriginal);
  h.next(pushOriginal, clearRebound);
  h.next(clearRebound, appendRebound);
  h.next(appendRebound, pushRebound);
  h.next(pushRebound, check);
  h.next(check, popRebound);
  h.next(popRebound, popOriginal);
  const CellRef cursor = h.entry(clearOriginal);

  const auto rebound = h.vm.run(cursor, 10'000);
  EXPECT_FALSE(rebound.success);
  EXPECT_NE(rebound.errorMessage.find("rebound stream"), std::string::npos);
  h.vm.abortScopedChoices(cursor);
  h.vm.setCursorOpcode(cursor, std::nullopt);

  const CellRef clearEmpty = h.op(OpcodeKind::StreamClear);
  h.core.bindOutput(clearEmpty, streamSlot);
  const CellRef pushEmpty = h.op(OpcodeKind::PushGuard);
  h.core.bindInput(pushEmpty, invokeInputs.front());
  h.core.bindInput(pushEmpty, h.guardDiagnostic("empty stream"));
  h.core.bindInput(pushEmpty, item);
  h.core.bindInput(pushEmpty, item);
  h.core.bindInput(pushEmpty, streamSlot);
  h.core.bindOutput(pushEmpty, h.arena.makeCell("empty token"));
  const CellRef emptyCheck = h.op(OpcodeKind::CheckGuards);
  h.next(clearEmpty, pushEmpty);
  h.next(pushEmpty, emptyCheck);
  h.next(emptyCheck, h.op(OpcodeKind::Halt));
  const CellRef emptyCursor = h.vm.spawnCursor(clearEmpty, "empty-stream");
  const auto empty          = h.vm.run(emptyCursor, 10'000);
  EXPECT_FALSE(empty.success);
  EXPECT_NE(empty.errorMessage.find("empty stream"), std::string::npos);
  h.vm.abortScopedChoices(emptyCursor);
  h.vm.setCursorOpcode(emptyCursor, std::nullopt);
}

TEST(VortexGuardTest, MultipleChecksRunInInsertionOrderAndNameTheFailure) {
  Harness h;
  const CellRef validator        = h.booleanValidator();
  const CellRef trueValue        = h.arena.makeScalarCell(true);
  const CellRef falseValue       = h.arena.makeScalarCell(false);
  const CellRef firstDiagnostic  = h.guardDiagnostic("first contract");
  const CellRef secondDiagnostic = h.guardDiagnostic("second contract");
  const CellRef first  = h.pushGuard(validator, firstDiagnostic, trueValue);
  const CellRef second = h.pushGuard(validator, secondDiagnostic, falseValue);
  const CellRef check  = h.op(OpcodeKind::CheckGuards);
  const auto firstOutputs = h.core.outputsOf(first);
  ASSERT_EQ(firstOutputs.size(), 1U);
  const CellRef token = firstOutputs.front();
  const CellRef pop   = h.op(OpcodeKind::PopGuard);
  h.core.bindInput(pop, token);
  h.next(first, second);
  h.next(second, check);
  h.next(check, pop);
  const CellRef cursor = h.entry(first);

  const auto failed = h.vm.run(cursor, 1000);
  EXPECT_FALSE(failed.success);
  EXPECT_NE(failed.errorMessage.find("second contract"), std::string::npos);
  EXPECT_FALSE(h.vm.hasActiveFrames());
  h.vm.abortScopedChoices(cursor);
  h.vm.setCursorOpcode(cursor, std::nullopt);
  EXPECT_FALSE(h.vm.hasActiveGuards());

  Harness order;
  const CellRef orderedValidator = order.booleanValidator();
  const CellRef bad              = order.arena.makeScalarCell(false);
  const CellRef good             = order.arena.makeScalarCell(true);
  const CellRef earlier =
      order.pushGuard(orderedValidator, order.guardDiagnostic("earlier"), bad);
  const CellRef later =
      order.pushGuard(orderedValidator, order.guardDiagnostic("later"), good);
  const CellRef orderCheck = order.op(OpcodeKind::CheckGuards);
  order.next(earlier, later);
  order.next(later, orderCheck);
  const CellRef orderCursor = order.entry(earlier);
  const auto firstFailure   = order.vm.run(orderCursor, 1000);
  EXPECT_FALSE(firstFailure.success);
  EXPECT_NE(firstFailure.errorMessage.find("earlier"), std::string::npos);
  EXPECT_EQ(firstFailure.errorMessage.find("later"), std::string::npos);
  order.vm.abortScopedChoices(orderCursor);
  order.vm.setCursorOpcode(orderCursor, std::nullopt);
}

TEST(VortexGuardTest, TokensEnforceOwnerLifoAndSingleUse) {
  Harness h;
  const CellRef validator  = h.booleanValidator();
  const CellRef value      = h.arena.makeScalarCell(true);
  const CellRef diagnostic = h.guardDiagnostic("token test");
  const CellRef first      = h.pushGuard(validator, diagnostic, value);
  const CellRef second     = h.pushGuard(validator, diagnostic, value);
  const auto firstOutputs  = h.core.outputsOf(first);
  const auto secondOutputs = h.core.outputsOf(second);
  ASSERT_EQ(firstOutputs.size(), 1U);
  ASSERT_EQ(secondOutputs.size(), 1U);
  const CellRef firstToken = firstOutputs.front();
  const CellRef invalidPop = h.op(OpcodeKind::PopGuard);
  h.core.bindInput(invalidPop, firstToken);
  h.next(first, second);
  h.next(second, invalidPop);
  const CellRef cursor = h.entry(first);
  ASSERT_TRUE(h.vm.step(cursor).success);
  ASSERT_TRUE(h.vm.step(cursor).success);
  const auto wrongOrder = h.vm.step(cursor);
  EXPECT_FALSE(wrongOrder.success);
  EXPECT_NE(wrongOrder.errorMessage.find("guard"), std::string::npos);
  h.vm.abortScopedChoices(cursor);
  h.vm.setCursorOpcode(cursor, std::nullopt);

  Harness stale;
  const CellRef staleValidator = stale.booleanValidator();
  const CellRef staleValue     = stale.arena.makeScalarCell(true);
  const CellRef stalePush      = stale.pushGuard(
      staleValidator, stale.guardDiagnostic("stale"), staleValue);
  const auto staleOutputs = stale.core.outputsOf(stalePush);
  ASSERT_EQ(staleOutputs.size(), 1U);
  const CellRef staleToken = staleOutputs.front();
  const CellRef popOnce    = stale.op(OpcodeKind::PopGuard);
  const CellRef popAgain   = stale.op(OpcodeKind::PopGuard);
  stale.core.bindInput(popOnce, staleToken);
  stale.core.bindInput(popAgain, staleToken);
  stale.next(stalePush, popOnce);
  stale.next(popOnce, popAgain);
  const CellRef staleCursor = stale.entry(stalePush);
  ASSERT_TRUE(stale.vm.step(staleCursor).success);
  ASSERT_TRUE(stale.vm.step(staleCursor).success);
  EXPECT_FALSE(stale.vm.step(staleCursor).success);
  stale.vm.abortScopedChoices(staleCursor);
  stale.vm.setCursorOpcode(staleCursor, std::nullopt);

  Harness owner;
  const CellRef ownerValidator = owner.booleanValidator();
  const CellRef ownerValue     = owner.arena.makeScalarCell(true);
  const CellRef ownerPush      = owner.pushGuard(
      ownerValidator, owner.guardDiagnostic("owner"), ownerValue);
  const auto ownerOutputs = owner.core.outputsOf(ownerPush);
  ASSERT_EQ(ownerOutputs.size(), 1U);
  const CellRef ownerToken = ownerOutputs.front();
  const CellRef foreignPop = owner.op(OpcodeKind::PopGuard);
  owner.core.bindInput(foreignPop, ownerToken);
  owner.next(ownerPush, owner.op(OpcodeKind::Halt));
  const CellRef ownerCursor   = owner.vm.spawnCursor(ownerPush, "owner");
  const CellRef foreignCursor = owner.vm.spawnCursor(foreignPop, "foreign");
  ASSERT_TRUE(owner.vm.step(ownerCursor).success);
  EXPECT_FALSE(owner.vm.step(foreignCursor).success);
  owner.vm.abortScopedChoices(ownerCursor);
  owner.vm.abortScopedChoices(foreignCursor);
  owner.vm.setCursorOpcode(ownerCursor, std::nullopt);
  owner.vm.setCursorOpcode(foreignCursor, std::nullopt);
}

TEST(VortexGuardTest, ChoiceSnapshotRestoresStackAndPendingValidatorPosition) {
  Harness h;
  const CellRef validator = h.booleanValidator();
  const CellRef value     = h.arena.makeScalarCell(true);
  const CellRef push =
      h.pushGuard(validator, h.guardDiagnostic("choice contract"), value);
  const CellRef check    = h.op(OpcodeKind::CheckGuards);
  const auto pushOutputs = h.core.outputsOf(push);
  ASSERT_EQ(pushOutputs.size(), 1U);
  const CellRef token = pushOutputs.front();
  const CellRef pop   = h.op(OpcodeKind::PopGuard);
  h.core.bindInput(pop, token);
  const CellRef done = h.op(OpcodeKind::Halt);
  h.next(push, check);
  h.next(check, pop);
  h.next(pop, done);
  const CellRef cursor = h.vm.spawnCursor(push, "guard-choice");
  ASSERT_TRUE(h.vm.step(cursor).success);
  ASSERT_TRUE(h.vm.step(cursor).success);
  while (h.vm.activationDepth(cursor) == 0) {
    const auto step = h.vm.step(cursor);
    ASSERT_TRUE(step.success) << step.errorMessage;
  }
  const CellRef alternative = h.op(OpcodeKind::Halt);
  h.vm.pushChoicePoint(cursor, alternative);
  const auto completed = h.vm.run(cursor, 1000);
  ASSERT_TRUE(completed.success) << completed.errorMessage;
  ASSERT_TRUE(h.vm.backtrack(cursor));
  EXPECT_EQ(h.vm.getCursorOpcode(cursor), alternative);
  EXPECT_TRUE(h.vm.hasActiveGuards());
  h.vm.setCursorOpcode(cursor, std::nullopt);
  EXPECT_FALSE(h.vm.hasActiveGuards());
  EXPECT_FALSE(h.vm.hasActiveFrames());
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);

  Harness cancelled;
  const CellRef cancelValidator = cancelled.booleanValidator();
  const CellRef cancelPush      = cancelled.pushGuard(
      cancelValidator, cancelled.guardDiagnostic("cancelled"),
      cancelled.arena.makeScalarCell(true));
  const CellRef cancelCheck = cancelled.op(OpcodeKind::CheckGuards);
  const auto cancelOutputs  = cancelled.core.outputsOf(cancelPush);
  ASSERT_EQ(cancelOutputs.size(), 1U);
  const CellRef cancelPop = cancelled.op(OpcodeKind::PopGuard);
  cancelled.core.bindInput(cancelPop, cancelOutputs.front());
  const CellRef cancelHalt       = cancelled.op(OpcodeKind::Halt);
  const CellRef scoped           = cancelled.op(OpcodeKind::ScopedChoice);
  const CellRef scopeAlternative = cancelled.op(OpcodeKind::Halt);
  cancelled.core.bindInput(scoped, scopeAlternative);
  cancelled.next(scoped, cancelPush);
  cancelled.next(cancelPush, cancelCheck);
  cancelled.next(cancelCheck, cancelPop);
  cancelled.next(cancelPop, cancelHalt);
  const CellRef cancelCursor = cancelled.vm.spawnCursor(scoped, "cancel");
  ASSERT_TRUE(cancelled.vm.step(cancelCursor).success);
  ASSERT_TRUE(cancelled.vm.step(cancelCursor).success);
  ASSERT_TRUE(cancelled.vm.step(cancelCursor).success);
  ASSERT_GT(cancelled.vm.activationDepth(cancelCursor), 0U);
  std::stop_source stop;
  stop.request_stop();
  EXPECT_FALSE(cancelled.vm.run(cancelCursor, 1000, stop.get_token()).success);
  cancelled.vm.abortScopedChoices(cancelCursor);
  cancelled.vm.setCursorOpcode(cancelCursor, std::nullopt);
  EXPECT_FALSE(cancelled.vm.hasActiveGuards());
  EXPECT_FALSE(cancelled.vm.hasActiveFrames());
  EXPECT_EQ(cancelled.vm.choiceDepth(), 0U);
  EXPECT_EQ(cancelled.arena.outstandingMarks(), 0U);
}

TEST(VortexGuardTest,
     GuardLimitsRefuseBeforePublishingTokenAndKeepCountsAtomic) {
  Harness h;
  const auto limits = GuardLimits{
      .maxGuards = 0, .maxCapturedCells = 4, .maxDiagnosticBytes = 32};
  h.vm.setGuardLimits(limits);
  const CellRef validator = h.booleanValidator();
  const CellRef push      = h.pushGuard(validator, h.guardDiagnostic("bounded"),
                                        h.arena.makeScalarCell(true));
  const CellRef cursor    = h.entry(push);
  const auto before       = h.arena.cellCount();
  const auto refused      = h.vm.step(cursor);
  EXPECT_FALSE(refused.success);
  EXPECT_FALSE(h.vm.hasActiveGuards());
  EXPECT_EQ(h.arena.cellCount(), before);
  h.vm.setCursorOpcode(cursor, std::nullopt);

  Harness overCaptured;
  overCaptured.vm.setGuardLimits(GuardLimits{
      .maxGuards = 2, .maxCapturedCells = 0, .maxDiagnosticBytes = 32});
  const CellRef capturePush = overCaptured.pushGuard(
      overCaptured.booleanValidator(), overCaptured.guardDiagnostic("bounded"),
      overCaptured.arena.makeScalarCell(true));
  const CellRef captureCursor = overCaptured.entry(capturePush);
  const auto captureBefore    = overCaptured.arena.cellCount();
  EXPECT_FALSE(overCaptured.vm.step(captureCursor).success);
  EXPECT_FALSE(overCaptured.vm.hasActiveGuards());
  EXPECT_EQ(overCaptured.arena.cellCount(), captureBefore);
  overCaptured.vm.setCursorOpcode(captureCursor, std::nullopt);

  Harness countBound;
  countBound.vm.setGuardLimits(GuardLimits{
      .maxGuards = 1, .maxCapturedCells = 8, .maxDiagnosticBytes = 32});
  const CellRef countValidator = countBound.booleanValidator();
  const CellRef value          = countBound.arena.makeScalarCell(true);
  const CellRef first          = countBound.pushGuard(
      countValidator, countBound.guardDiagnostic("first"), value);
  const CellRef second = countBound.pushGuard(
      countValidator, countBound.guardDiagnostic("second"), value);
  countBound.next(first, second);
  const CellRef countCursor = countBound.entry(first);
  ASSERT_TRUE(countBound.vm.step(countCursor).success);
  ASSERT_TRUE(countBound.vm.hasActiveGuards());
  const auto beforeSecond = countBound.arena.cellCount();
  EXPECT_FALSE(countBound.vm.step(countCursor).success);
  EXPECT_EQ(countBound.vm.getCursorOpcode(countCursor), second);
  EXPECT_EQ(countBound.arena.cellCount(), beforeSecond);
  EXPECT_TRUE(countBound.vm.hasActiveGuards());
  countBound.vm.abortScopedChoices(countCursor);
  countBound.vm.setCursorOpcode(countCursor, std::nullopt);
}

TEST(VortexGuardTest, CapturesRequireCellCardinalityAndValidatorReturnsBool) {
  Harness multiple;
  const CellRef validator = multiple.booleanValidator();
  const CellRef stream    = multiple.arena.makeCell("capture stream");
  const CellRef clear     = multiple.op(OpcodeKind::StreamClear);
  const CellRef append    = multiple.op(OpcodeKind::StreamAppend);
  multiple.core.bindOutput(clear, stream);
  multiple.core.bindInput(append,
                          multiple.arena.makeScalarCell(std::int64_t{1}));
  multiple.core.bindInput(append,
                          multiple.arena.makeScalarCell(std::int64_t{2}));
  multiple.core.bindOutput(append, stream);
  const CellRef push = multiple.op(OpcodeKind::PushGuard);
  multiple.core.bindInput(push, validator);
  multiple.core.bindInput(push, multiple.guardDiagnostic("one cell"));
  multiple.core.bindInput(push, stream);
  multiple.core.bindOutput(push, multiple.arena.makeCell("token"));
  multiple.next(clear, append);
  multiple.next(append, push);
  const CellRef cursor = multiple.entry(clear);
  const auto rejected  = multiple.vm.run(cursor, 1000);
  EXPECT_FALSE(rejected.success);
  EXPECT_NE(rejected.errorMessage.find("multiple cells"), std::string::npos);
  EXPECT_FALSE(multiple.vm.hasActiveGuards());
  multiple.vm.abortScopedChoices(cursor);
  multiple.vm.setCursorOpcode(cursor, std::nullopt);

  Harness nonBoolean;
  const CellRef nonBoolValidator = nonBoolean.booleanValidator();
  const CellRef text             = nonBoolean.arena.makeCell("not a Bool");
  const CellRef badPush          = nonBoolean.pushGuard(
      nonBoolValidator, nonBoolean.guardDiagnostic("strict boolean"), text);
  const CellRef check = nonBoolean.op(OpcodeKind::CheckGuards);
  nonBoolean.next(badPush, check);
  nonBoolean.next(check, nonBoolean.op(OpcodeKind::Halt));
  const CellRef badCursor = nonBoolean.vm.spawnCursor(badPush, "non-bool");
  const auto badResult    = nonBoolean.vm.run(badCursor, 1000);
  EXPECT_FALSE(badResult.success);
  EXPECT_NE(badResult.errorMessage.find("strict boolean"), std::string::npos);
  EXPECT_FALSE(nonBoolean.vm.hasActiveFrames());
  nonBoolean.vm.abortScopedChoices(badCursor);
  nonBoolean.vm.setCursorOpcode(badCursor, std::nullopt);
}

TEST(VortexGuardTest, ValidatorResultPortCountMustBeExactlyOne) {
  Harness h;
  const CellRef argument     = h.arena.makeCell("argument");
  const CellRef firstResult  = h.arena.makeCell("first result");
  const CellRef secondResult = h.arena.makeCell("second result");
  const CellRef copy         = h.op(OpcodeKind::CopyCellRefs);
  const CellRef ret          = h.op(OpcodeKind::ReturnFrame);
  h.core.bindInput(copy, argument);
  h.core.bindOutput(copy, firstResult);
  h.core.bindOutput(copy, secondResult);
  h.next(copy, ret);
  const auto descriptor = h.vm.makeFrameDescriptor(FrameDescriptorSpec{
      .entry         = copy,
      .locals        = {argument, firstResult, secondResult},
      .argumentPorts = {argument},
      .resultPorts   = {firstResult, secondResult},
  });
  ASSERT_TRUE(descriptor) << descriptor.error().message;
  h.core.bindInput(ret, *descriptor);
  const CellRef push =
      h.pushGuard(*descriptor, h.guardDiagnostic("multi-result"),
                  h.arena.makeScalarCell(true));
  const CellRef cursor = h.vm.spawnCursor(push, "multi-result");
  const auto before    = h.arena.cellCount();
  const auto refused   = h.vm.step(cursor);
  EXPECT_FALSE(refused.success);
  EXPECT_NE(refused.errorMessage.find("exactly one Cell result"),
            std::string::npos);
  EXPECT_FALSE(h.vm.hasActiveGuards());
  EXPECT_EQ(h.arena.cellCount(), before);
  h.vm.setCursorOpcode(cursor, std::nullopt);
}

TEST(VortexGuardTest, WorkBudgetRefusalKeepsGuardOpsRetryable) {
  Harness h;
  const CellRef validator = h.booleanValidator();
  const CellRef push      = h.pushGuard(validator, h.guardDiagnostic("fuel"),
                                        h.arena.makeScalarCell(true));
  const auto pushOutputs  = h.core.outputsOf(push);
  ASSERT_EQ(pushOutputs.size(), 1U);
  const CellRef check = h.op(OpcodeKind::CheckGuards);
  const CellRef pop   = h.op(OpcodeKind::PopGuard);
  h.core.bindInput(pop, pushOutputs.front());
  h.next(push, check);
  h.next(check, pop);
  const CellRef cursor = h.vm.spawnCursor(push, "guard-fuel");

  h.arena.setExecutionLimits(10000, 0, 10000);
  const auto cellsBeforePush = h.arena.cellCount();
  EXPECT_FALSE(h.vm.step(cursor).success);
  EXPECT_EQ(h.vm.getCursorOpcode(cursor), push);
  EXPECT_FALSE(h.vm.hasActiveGuards());
  EXPECT_EQ(h.arena.cellCount(), cellsBeforePush);

  h.arena.setExecutionLimits(10000, 10000, 10000);
  ASSERT_TRUE(h.vm.step(cursor).success);
  ASSERT_TRUE(h.vm.hasActiveGuards());
  h.arena.setExecutionLimits(10000, 0, 10000);
  EXPECT_FALSE(h.vm.step(cursor).success);
  EXPECT_EQ(h.vm.getCursorOpcode(cursor), check);
  EXPECT_TRUE(h.vm.hasActiveGuards());

  // The first check has no pending frame until it passes its own fuel gate.
  h.arena.setExecutionLimits(10000, 2, 10000);
  EXPECT_FALSE(h.vm.step(cursor).success);
  EXPECT_EQ(h.vm.getCursorOpcode(cursor), check);
  EXPECT_TRUE(h.vm.hasActiveGuards());
  EXPECT_EQ(h.vm.activationDepth(cursor), 0U);

  h.arena.setExecutionLimits(10000, 10000, 10000);
  const auto retried = h.vm.run(cursor, 1000);
  ASSERT_TRUE(retried.success) << retried.errorMessage;
  EXPECT_FALSE(h.vm.hasActiveGuards());
  EXPECT_FALSE(h.vm.hasActiveFrames());
}

TEST(VortexGuardTest, GuardOpsRefuseConditionsAndPipelinesBeforeTheirEffects) {
  Harness h;
  const CellRef validator = h.booleanValidator();
  const CellRef subject   = h.arena.makeCell("condition subject");
  const CellRef target    = h.arena.makeCell("condition target");
  const CellRef link      = h.op(OpcodeKind::Link);
  h.core.bindInput(link, subject);
  h.core.bindInput(link, h.arena.makeScalarCell(
                             static_cast<std::int64_t>(h.core.dims().role)));
  h.core.bindInput(link, h.arena.makeScalarCell(std::int64_t{1}));
  h.core.bindInput(link, target);
  const CellRef push =
      h.pushGuard(validator, h.guardDiagnostic("condition blocked"),
                  h.arena.makeScalarCell(true));
  h.core.attachPrecondition(push, link);
  const CellRef cursor        = h.vm.spawnCursor(push, "guard-condition");
  const auto refusedCondition = h.vm.step(cursor);
  EXPECT_FALSE(refusedCondition.success);
  EXPECT_FALSE(h.core.readLink(subject, h.core.dims().role, DimVector::POS));
  EXPECT_FALSE(h.vm.hasActiveGuards());
  EXPECT_EQ(h.vm.getCursorOpcode(cursor), push);

  Harness piped;
  const CellRef pipeValidator = piped.booleanValidator();
  const CellRef argument      = piped.arena.makeScalarCell(true);
  const CellRef increment     = piped.op(OpcodeKind::Add);
  piped.core.bindInput(increment, argument);
  piped.core.bindInput(increment, piped.arena.makeScalarCell(std::int64_t{1}));
  piped.core.attachPipeline(argument, increment);
  const CellRef pipePush = piped.pushGuard(
      pipeValidator, piped.guardDiagnostic("pipeline blocked"), argument);
  const CellRef pipeCursor   = piped.vm.spawnCursor(pipePush, "guard-pipeline");
  const auto refusedPipeline = piped.vm.step(pipeCursor);
  EXPECT_FALSE(refusedPipeline.success);
  EXPECT_EQ(piped.arena.valueKindOf(argument), xanadu::ValueKind::Bool);
  EXPECT_EQ(piped.arena.asBool(argument), std::optional{true});
  EXPECT_FALSE(piped.vm.hasActiveGuards());
  EXPECT_EQ(piped.vm.getCursorOpcode(pipeCursor), pipePush);
}

TEST(VortexGuardTest, ReturnFrameTrimsCalleeGuardsAndKeepsCallerGuard) {
  Harness h;
  const CellRef validator   = h.booleanValidator();
  const CellRef value       = h.arena.makeScalarCell(true);
  const CellRef calleeToken = h.arena.makeCell("callee token");
  const CellRef calleePush  = h.op(OpcodeKind::PushGuard);
  h.core.bindInput(calleePush, validator);
  h.core.bindInput(calleePush, h.guardDiagnostic("callee guard"));
  h.core.bindInput(calleePush, value);
  h.core.bindOutput(calleePush, calleeToken);
  const CellRef ret = h.op(OpcodeKind::ReturnFrame);
  h.next(calleePush, ret);
  const auto callee = h.vm.makeFrameDescriptor(FrameDescriptorSpec{
      .entry  = calleePush,
      .locals = {calleeToken},
  });
  ASSERT_TRUE(callee) << callee.error().message;
  h.core.bindInput(ret, *callee);

  const CellRef callerPush =
      h.pushGuard(validator, h.guardDiagnostic("caller guard"), value);
  const auto callerOutputs = h.core.outputsOf(callerPush);
  ASSERT_EQ(callerOutputs.size(), 1U);
  const CellRef invoke = h.op(OpcodeKind::InvokeFrame);
  h.core.bindInput(invoke, *callee);
  const CellRef check = h.op(OpcodeKind::CheckGuards);
  const CellRef pop   = h.op(OpcodeKind::PopGuard);
  h.core.bindInput(pop, callerOutputs.front());
  h.next(callerPush, invoke);
  h.next(invoke, check);
  h.next(check, pop);
  const CellRef cursor = h.entry(callerPush);

  const auto execution = h.vm.run(cursor, 1000);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  EXPECT_FALSE(h.vm.hasActiveGuards());
  EXPECT_EQ(h.vm.activationDepth(cursor), 0U);
}

TEST(VortexGuardTest,
     ExportRelocatesValidatorAndRejectsLiveOrRestorableGuards) {
  TemporaryDirectory temporary;
  auto scroll = std::make_shared<xanadu::UserPermascroll>(
      xanadu::UserPermascroll::Config{.storageDir = temporary.path / "scroll"});
  {
    Harness h;
    const CellRef validator = h.booleanValidator();
    const CellRef push  = h.pushGuard(validator, h.guardDiagnostic("exported"),
                                      h.arena.makeScalarCell(true));
    const CellRef check = h.op(OpcodeKind::CheckGuards);
    const auto pushOutputs = h.core.outputsOf(push);
    ASSERT_EQ(pushOutputs.size(), 1U);
    const CellRef pop = h.op(OpcodeKind::PopGuard);
    h.core.bindInput(pop, pushOutputs.front());
    const CellRef halt = h.op(OpcodeKind::Halt);
    h.next(push, check);
    h.next(check, pop);
    h.next(pop, halt);
    ASSERT_TRUE(
        h.arena.link(h.core.home(), h.core.dims().spin, DimVector::POS, push));
    const std::vector<CellRef> opcodes{
        push,
        check,
        pop,
        halt,
        *h.vm.frameDescriptor(validator)->entry,
        *h.arena.linked(*h.vm.frameDescriptor(validator)->entry,
                        h.core.dims().spin, DimVector::POS)};
    xanadu::Store store(scroll);
    EXPECT_FALSE(
        zigzag::vortex::exportProgram(h.core, h.vm, opcodes, store).isZero());
    store.save((temporary.path / "program").string());
  }

  xanadu::Store reloaded(scroll);
  reloaded.load((temporary.path / "program").string());
  ASSERT_TRUE(reloaded.structureHead());
  const auto manifold = reloaded.rebuildManifold(*reloaded.structureHead());
  zigzag::ArenaManifold arena(&manifold, reloaded);
  VortexCore core(arena);
  VortexVM vm(core);
  const auto entry =
      arena.linked(core.home(), core.dims().spin, DimVector::POS);
  ASSERT_TRUE(entry);
  const CellRef cursor = vm.spawnCursor(*entry, "reopened-guard");
  const auto execution = vm.run(cursor, 1000);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  EXPECT_FALSE(vm.hasActiveGuards());
  EXPECT_FALSE(vm.hasActiveFrames());

  Harness live;
  const CellRef liveValidator = live.booleanValidator();
  const CellRef livePush =
      live.pushGuard(liveValidator, live.guardDiagnostic("live export"),
                     live.arena.makeScalarCell(true));
  const CellRef livePop  = live.op(OpcodeKind::PopGuard);
  const auto liveOutputs = live.core.outputsOf(livePush);
  ASSERT_EQ(liveOutputs.size(), 1U);
  live.core.bindInput(livePop, liveOutputs.front());
  live.next(livePush, livePop);
  const CellRef liveCursor = live.vm.spawnCursor(livePush, "live-export");
  ASSERT_TRUE(live.vm.step(liveCursor).success);
  live.vm.pushChoicePoint(liveCursor, livePop);
  xanadu::Store liveStore(scroll);
  const auto before = liveStore.latest();
  const std::vector<CellRef> liveOps{livePush, livePop};
  EXPECT_THROW((void)zigzag::vortex::exportProgram(live.core, live.vm, liveOps,
                                                   liveStore),
               std::exception);
  EXPECT_EQ(liveStore.latest(), before);
  ASSERT_TRUE(live.vm.step(liveCursor).success);
  // A choice still retains the popped guard and must prevent runtime export.
  EXPECT_TRUE(live.vm.hasActiveGuards());
  EXPECT_THROW((void)zigzag::vortex::exportProgram(live.core, live.vm, liveOps,
                                                   liveStore),
               std::exception);
  EXPECT_EQ(liveStore.latest(), before);
  live.vm.clearChoicePoints();
  EXPECT_FALSE(live.vm.hasActiveGuards());
  live.vm.setCursorOpcode(liveCursor, std::nullopt);
  EXPECT_EQ(live.arena.outstandingMarks(), 0U);
}

TEST(VortexGuardTest, RecursiveUseOfSameCheckOpcodeIsRejectedByPendingOwner) {
  Harness h;
  const CellRef argument = h.arena.makeCell("recursive argument");
  const CellRef result   = h.arena.makeCell("recursive result");
  const CellRef check    = h.op(OpcodeKind::CheckGuards);
  const auto reserved    = h.vm.reserveFrameDescriptor();
  ASSERT_TRUE(reserved) << reserved.error().message;
  const auto finalized =
      h.vm.finalizeFrameDescriptor(*reserved, FrameDescriptorSpec{
                                                  .entry  = check,
                                                  .locals = {argument, result},
                                                  .argumentPorts = {argument},
                                                  .resultPorts   = {result},
                                              });
  ASSERT_TRUE(finalized) << finalized.error().message;

  const CellRef push =
      h.pushGuard(*reserved, h.guardDiagnostic("recursive check"),
                  h.arena.makeScalarCell(true));
  const auto pushOutputs = h.core.outputsOf(push);
  ASSERT_EQ(pushOutputs.size(), 1U);
  const CellRef pop = h.op(OpcodeKind::PopGuard);
  h.core.bindInput(pop, pushOutputs.front());
  const CellRef halt = h.op(OpcodeKind::Halt);
  h.next(push, check);
  h.next(check, pop);
  h.next(pop, halt);
  const CellRef cursor = h.vm.spawnCursor(push, "recursive-check");

  const auto rejected = h.vm.run(cursor, 1000);
  EXPECT_FALSE(rejected.success);
  EXPECT_NE(rejected.errorMessage.find("recursive CheckGuards"),
            std::string::npos);
  EXPECT_TRUE(h.vm.hasActiveGuards());
  h.vm.abortScopedChoices(cursor);
  h.vm.setCursorOpcode(cursor, std::nullopt);
  EXPECT_FALSE(h.vm.hasActiveGuards());
  EXPECT_FALSE(h.vm.hasActiveFrames());
}

TEST(VortexGuardTest, FailedSecondDispatchRetriesFromFirstCurrentCapture) {
  Harness h;
  const CellRef validator   = h.booleanValidator();
  const CellRef firstValue  = h.arena.makeScalarCell(true);
  const CellRef secondValue = h.arena.makeScalarCell(true);
  const CellRef first       = h.pushGuard(
      validator, h.guardDiagnostic("first guard rechecked"), firstValue);
  const CellRef second =
      h.pushGuard(validator, h.guardDiagnostic("second guard"), secondValue);
  const auto firstOutputs  = h.core.outputsOf(first);
  const auto secondOutputs = h.core.outputsOf(second);
  ASSERT_EQ(firstOutputs.size(), 1U);
  ASSERT_EQ(secondOutputs.size(), 1U);
  const CellRef check     = h.op(OpcodeKind::CheckGuards);
  const CellRef popSecond = h.op(OpcodeKind::PopGuard);
  const CellRef popFirst  = h.op(OpcodeKind::PopGuard);
  const CellRef halt      = h.op(OpcodeKind::Halt);
  h.core.bindInput(popSecond, secondOutputs.front());
  h.core.bindInput(popFirst, firstOutputs.front());
  h.next(first, second);
  h.next(second, check);
  h.next(check, popSecond);
  h.next(popSecond, popFirst);
  h.next(popFirst, halt);
  const CellRef cursor = h.vm.spawnCursor(first, "retry-guards");

  ASSERT_TRUE(h.vm.step(cursor).success);
  ASSERT_TRUE(h.vm.step(cursor).success);
  ASSERT_TRUE(h.vm.step(cursor).success); // Dispatch the first validator.
  ASSERT_EQ(h.vm.activationDepth(cursor), 1U);
  ASSERT_TRUE(h.vm.step(cursor).success); // Copy the first Bool result.
  ASSERT_TRUE(h.vm.step(cursor).success); // Return to CheckGuards.
  ASSERT_EQ(h.vm.activationDepth(cursor), 0U);

  h.vm.setFrameLimits(FrameLimits{.maxDepth = 0});
  const auto secondDispatch = h.vm.step(cursor);
  EXPECT_FALSE(secondDispatch.success);
  EXPECT_NE(secondDispatch.errorMessage.find("depth"), std::string::npos);
  EXPECT_EQ(h.vm.getCursorOpcode(cursor), check);
  EXPECT_TRUE(h.vm.hasActiveGuards());
  EXPECT_EQ(h.vm.activationDepth(cursor), 0U);

  ASSERT_TRUE(h.arena.setValueBits(firstValue, xanadu::ValueKind::Bool, 0));
  h.vm.setFrameLimits(FrameLimits{});
  const auto retried = h.vm.run(cursor, 1000);
  EXPECT_FALSE(retried.success);
  EXPECT_NE(retried.errorMessage.find("first guard rechecked"),
            std::string::npos);
  EXPECT_EQ(retried.errorMessage.find("second guard"), std::string::npos);
  EXPECT_FALSE(h.vm.hasActiveFrames());
  EXPECT_EQ(h.vm.activationDepth(cursor), 0U);
  h.vm.abortScopedChoices(cursor);
  h.vm.setCursorOpcode(cursor, std::nullopt);
  EXPECT_FALSE(h.vm.hasActiveGuards());
  EXPECT_FALSE(h.vm.hasActiveFrames());
}

TEST(VortexGuardTest, PipelineStopsAtHaltBeforeUnreachableGuardOpcode) {
  Harness h;
  const CellRef argument = h.arena.makeScalarCell(std::int64_t{5});
  const CellRef add      = h.op(OpcodeKind::Add);
  const CellRef three    = h.arena.makeScalarCell(std::int64_t{3});
  const CellRef output   = h.arena.makeCell("sum");
  h.core.bindInput(add, argument);
  h.core.bindInput(add, three);
  h.core.bindOutput(add, output);

  const CellRef halt        = h.op(OpcodeKind::Halt);
  const CellRef unreachable = h.op(OpcodeKind::PushGuard);
  h.next(halt, unreachable);
  h.core.attachPipeline(argument, halt);
  const CellRef cursor = h.vm.spawnCursor(add, "pipeline-halt");

  const auto execution = h.vm.step(cursor);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  EXPECT_EQ(h.arena.asInt64(argument), std::optional{std::int64_t{5}});
  EXPECT_EQ(h.core.render(output), CellValue{std::int64_t{8}});
  EXPECT_FALSE(h.vm.hasActiveGuards());
}

} // namespace
} // namespace zigzag::vortex
