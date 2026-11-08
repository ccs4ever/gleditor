#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/zigzag/query_scalar.hpp"

namespace zigzag::vortex {
namespace {

struct BindingHarness {
  ArenaManifold arena;
  VortexCore core{arena};
  VortexVM vm{core};

  CellRef restore(const CellRef descriptor) {
    const auto op = vm.mintOpcode(OpcodeKind::RestoreBindings);
    core.bindInput(op, descriptor);
    return op;
  }

  void next(const CellRef from, const CellRef to) {
    ASSERT_TRUE(arena.link(from, core.dims().spin, DimVector::POS, to));
  }
};

TEST(VortexBindingRuntimeTest,
     LiteralAliasesCyclesAndStreamMembersStayOneLevel) {
  BindingHarness h;
  const auto a       = h.arena.makeCell("a data");
  const auto b       = h.arena.makeCell("b data");
  const auto stream  = h.arena.makeCell("ordered stream register");
  const auto empty   = h.arena.makeCell("empty stream register");
  const auto missing = h.arena.makeCell("missing cell register");
  const auto open    = h.arena.makeCell("open cell");
  RuntimeBindingSnapshot source;
  source.cellReferences.emplace(a, b);
  source.cellReferences.emplace(b, a);
  source.cellReferences.emplace(missing, std::nullopt);
  source.streams.emplace(stream, std::vector<CellRef>{a, b, a});
  source.streams.emplace(empty, std::vector<CellRef>{});
  const auto descriptor = h.vm.makeBindingsDescriptor(source);
  ASSERT_TRUE(descriptor) << descriptor.error().message;
  const auto parsed = h.vm.bindingsDescriptor(*descriptor);
  ASSERT_TRUE(parsed) << parsed.error().message;
  EXPECT_EQ(parsed->snapshot.cellReferences, source.cellReferences);
  EXPECT_EQ(parsed->snapshot.streams, source.streams);

  // A second VM has no source maps or pointer registry to consult.
  VortexVM fresh(h.core);
  const auto restore = fresh.mintOpcode(OpcodeKind::RestoreBindings);
  const auto halt    = fresh.mintOpcode(OpcodeKind::Halt);
  h.core.bindInput(restore, *descriptor);
  h.next(restore, halt);
  const auto cursor = fresh.spawnCursor(restore, "literal-binding-restore");
  const auto run    = fresh.run(cursor, 10);
  ASSERT_TRUE(run.success) << run.errorMessage;
  EXPECT_EQ(fresh.resultCell(a), b);
  EXPECT_EQ(fresh.resultCell(b), a);
  EXPECT_EQ(fresh.resultCells(stream), (std::vector<CellRef>{a, b, a}));
  EXPECT_TRUE(fresh.resultCells(empty).empty());
  EXPECT_TRUE(fresh.resultCells(missing).empty());
  EXPECT_EQ(fresh.resultCells(open), (std::vector<CellRef>{open}));
  EXPECT_EQ(h.arena.textOf(a), "a data");
  EXPECT_EQ(h.arena.textOf(b), "b data");
  const auto snapshot = fresh.snapshotBindings();
  EXPECT_EQ(snapshot.cellReferences, source.cellReferences);
  EXPECT_EQ(snapshot.streams, source.streams);
}

TEST(VortexBindingRuntimeTest,
     ReplacementRemovesStaleKindsAndKeepsNumericData) {
  BindingHarness h;
  const auto numeric = h.arena.makeScalarCell(std::int64_t{0});
  const auto exact   = h.arena.makeScalarCell(std::int64_t{9007199254740993LL});
  const auto oldStream = h.arena.makeCell();
  RuntimeBindingSnapshot before;
  before.streams.emplace(oldStream, std::vector<CellRef>{exact});
  h.vm.restoreBindings(before);
  RuntimeBindingSnapshot after;
  after.cellReferences.emplace(numeric, exact);
  h.vm.restoreBindings(after);
  EXPECT_EQ(h.vm.resultCells(oldStream), (std::vector<CellRef>{oldStream}));
  EXPECT_EQ(h.vm.resultCell(numeric), exact);
  EXPECT_FALSE(h.vm.getOpcodeKind(numeric));
  EXPECT_EQ(h.arena.asInt64(numeric), std::int64_t{0});
  EXPECT_EQ(h.arena.asInt64(exact), std::int64_t{9007199254740993LL});
}

TEST(VortexBindingRuntimeTest, InvalidOverlappingKindsRefuseAtomically) {
  BindingHarness h;
  const auto slot  = h.arena.makeCell("register");
  const auto value = h.arena.makeCell("value");
  RuntimeBindingSnapshot valid;
  valid.cellReferences.emplace(slot, value);
  h.vm.restoreBindings(valid);
  auto invalid = valid;
  invalid.streams.emplace(slot, std::vector<CellRef>{value});
  const auto before = h.arena.cellCount();
  EXPECT_THROW(h.vm.restoreBindings(invalid), std::exception);
  EXPECT_FALSE(h.vm.makeBindingsDescriptor(invalid));
  EXPECT_EQ(h.arena.cellCount(), before);
  EXPECT_EQ(h.vm.resultCell(slot), value);
  EXPECT_TRUE(h.vm.snapshotBindings().streams.empty());
}

TEST(VortexBindingRuntimeTest, RestoreInstructionCanReinitializeOnLaterCursor) {
  BindingHarness h;
  const auto slot  = h.arena.makeCell();
  const auto value = h.arena.makeCell("stable member");
  RuntimeBindingSnapshot initial;
  initial.streams.emplace(slot, std::vector<CellRef>{value, value});
  const auto descriptor = h.vm.makeBindingsDescriptor(initial);
  ASSERT_TRUE(descriptor);
  const auto restore = h.restore(*descriptor);
  const auto clear   = h.vm.mintOpcode(OpcodeKind::StreamClear);
  const auto halt    = h.vm.mintOpcode(OpcodeKind::Halt);
  h.core.bindOutput(clear, slot);
  h.next(restore, clear);
  h.next(clear, halt);
  const auto first = h.vm.spawnCursor(restore, "restore-first");
  ASSERT_TRUE(h.vm.step(first).success);
  EXPECT_EQ(h.vm.resultCells(slot), (std::vector<CellRef>{value, value}));
  ASSERT_TRUE(h.vm.run(first, 10).success);
  EXPECT_TRUE(h.vm.resultCells(slot).empty());
  const auto second = h.vm.spawnCursor(restore, "restore-second");
  const auto again  = h.vm.step(second);
  ASSERT_TRUE(again.success) << again.errorMessage;
  EXPECT_EQ(h.vm.resultCells(slot), (std::vector<CellRef>{value, value}));
}

TEST(VortexBindingRuntimeTest, ClosedIteratorMarkersKeepTheirDistinctBehavior) {
  BindingHarness h;
  const auto unavailable = h.arena.makeCell();
  const auto exhausted   = h.arena.makeCell();
  RuntimeBindingSnapshot state;
  state.iterators.emplace(unavailable, ClosedIteratorState::Unavailable);
  state.iterators.emplace(exhausted, ClosedIteratorState::Exhausted);
  const auto descriptor = h.vm.makeBindingsDescriptor(state);
  ASSERT_TRUE(descriptor);
  const auto parsed = h.vm.bindingsDescriptor(*descriptor);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->snapshot.iterators, state.iterators);
  const auto restore     = h.restore(*descriptor);
  const auto next        = h.vm.mintOpcode(OpcodeKind::IteratorNext);
  const auto missingNext = h.vm.mintOpcode(OpcodeKind::IteratorNext);
  const auto item        = h.arena.makeCell();
  const auto present     = h.arena.makeCell();
  h.core.bindInput(next, exhausted);
  h.core.bindOutput(next, item);
  h.core.bindOutput(next, present);
  h.core.bindInput(missingNext, unavailable);
  h.core.bindOutput(missingNext, item);
  h.core.bindOutput(missingNext, present);
  h.next(restore, next);
  h.next(next, missingNext);
  const auto cursor = h.vm.spawnCursor(restore, "closed-iterators");
  ASSERT_TRUE(h.vm.step(cursor).success);
  const auto exhaustedResult = h.vm.step(cursor);
  ASSERT_TRUE(exhaustedResult.success) << exhaustedResult.errorMessage;
  EXPECT_TRUE(h.vm.resultCells(item).empty());
  EXPECT_EQ(h.arena.asBool(present), false);
  const auto unavailableResult = h.vm.step(cursor);
  EXPECT_FALSE(unavailableResult.success);
  EXPECT_NE(unavailableResult.errorMessage.find("initialized"),
            std::string::npos);
}

TEST(VortexBindingRuntimeTest, RestoreRefusesChoicesWithoutPublishingBindings) {
  BindingHarness h;
  const auto slot  = h.arena.makeCell();
  const auto value = h.arena.makeCell();
  RuntimeBindingSnapshot state;
  state.cellReferences.emplace(slot, value);
  const auto descriptor = h.vm.makeBindingsDescriptor(state);
  ASSERT_TRUE(descriptor);
  const auto restore = h.restore(*descriptor);
  const auto halt    = h.vm.mintOpcode(OpcodeKind::Halt);
  h.next(restore, halt);
  const auto cursor = h.vm.spawnCursor(restore, "choice-restore-refusal");
  h.vm.pushChoicePoint(cursor, halt);
  const auto before = h.arena.cellCount();
  const auto run    = h.vm.step(cursor);
  EXPECT_FALSE(run.success);
  EXPECT_EQ(h.vm.resultCell(slot), slot);
  EXPECT_EQ(h.arena.cellCount(), before);
  EXPECT_EQ(h.vm.getCursorOpcode(cursor), restore);
}

TEST(VortexBindingRuntimeTest, LimitsRefuseBeforeAllocationAndAllowRetry) {
  BindingHarness h;
  const auto slot   = h.arena.makeCell();
  const auto member = h.arena.makeCell();
  RuntimeBindingSnapshot state;
  state.streams.emplace(slot, std::vector<CellRef>{member, member});
  auto limits          = h.vm.bindingsLimits();
  limits.maxReferences = 2; // Slot plus both duplicate references costs three.
  h.vm.setBindingsLimits(limits);
  const auto before = h.arena.cellCount();
  EXPECT_FALSE(h.vm.makeBindingsDescriptor(state));
  EXPECT_THROW(h.vm.restoreBindings(state), std::exception);
  EXPECT_EQ(h.arena.cellCount(), before);
  EXPECT_EQ(h.vm.resultCell(slot), slot);
  limits.maxReferences = 3;
  limits.maxRestores   = 1;
  h.vm.setBindingsLimits(limits);
  const auto descriptor = h.vm.makeBindingsDescriptor(state);
  ASSERT_TRUE(descriptor);
  EXPECT_NO_THROW(h.vm.restoreBindings(state));
  EXPECT_EQ(h.vm.resultCells(slot), (std::vector<CellRef>{member, member}));
  EXPECT_THROW(h.vm.restoreBindings(RuntimeBindingSnapshot{}), std::exception);
  EXPECT_EQ(h.vm.resultCells(slot), (std::vector<CellRef>{member, member}));
}

TEST(VortexBindingRuntimeTest, ProbeImitationsAndCyclicMetadataRefuse) {
  BindingHarness h;
  const auto imitation = h.arena.makeCell("<runtime-object>");
  EXPECT_FALSE(h.vm.bindingsDescriptor(imitation));
  const auto slot = h.arena.makeCell();
  RuntimeBindingSnapshot state;
  state.cellReferences.emplace(slot, std::nullopt);
  const auto descriptor = h.vm.makeBindingsDescriptor(state);
  ASSERT_TRUE(descriptor);
  const auto first =
      h.arena.linked(*descriptor, h.core.dims().grab, DimVector::POS);
  ASSERT_TRUE(first);
  ASSERT_TRUE(h.arena.link(*first, h.core.dims().step, DimVector::POS, *first));
  EXPECT_FALSE(h.vm.bindingsDescriptor(*descriptor));
  const auto restore = h.restore(*descriptor);
  const auto cursor  = h.vm.spawnCursor(restore, "cyclic-binding-metadata");
  const auto result  = h.vm.step(cursor);
  EXPECT_FALSE(result.success);
  EXPECT_EQ(h.vm.resultCell(slot), slot);
  EXPECT_EQ(h.vm.getCursorOpcode(cursor), restore);
}

TEST(VortexBindingRuntimeTest, InactiveCursorDoesNotMakeLiveIteratorsPortable) {
  for (const auto kind :
       {OpcodeKind::StreamBegin, OpcodeKind::StreamReverseBegin}) {
    BindingHarness h;
    const auto value    = h.arena.makeCell("unconsumed member");
    const auto iterator = h.arena.makeCell();
    const auto begin    = h.vm.mintOpcode(kind);
    const auto halt     = h.vm.mintOpcode(OpcodeKind::Halt);
    h.core.bindInput(begin, value);
    h.core.bindOutput(begin, iterator);
    h.next(begin, halt);
    const auto cursor = h.vm.spawnCursor(begin, "live-iterator-snapshot");
    ASSERT_TRUE(h.vm.run(cursor, 10).success);
    h.vm.setCursorOpcode(cursor, std::nullopt);
    EXPECT_THROW((void)h.vm.snapshotBindings(), std::exception);
  }
}

TEST(VortexBindingRuntimeTest, InspectionBudgetAndRelationLimitsApplyLocally) {
  BindingHarness h;
  const auto slot   = h.arena.makeCell();
  const auto member = h.arena.makeCell();
  RuntimeBindingSnapshot state;
  state.streams.emplace(slot, std::vector<CellRef>(32, member));
  const auto descriptor = h.vm.makeBindingsDescriptor(state);
  ASSERT_TRUE(descriptor);
  const auto claim = *h.core.dims().runtimeObjectClaim;
  EXPECT_EQ(classifyRuntimeObjectRoot(h.arena, *descriptor, claim),
            RuntimeObjectClaimResult::Claimed);
  const auto scalar =
      readQueryScalar(h.arena, *descriptor, h.core.dims().clone, claim);
  ASSERT_FALSE(scalar);
  EXPECT_EQ(scalar.error(), QueryScalarReadError::UnsupportedHandle);
  auto limits              = h.vm.bindingsLimits();
  limits.maxInspectionWork = 100;
  h.vm.setBindingsLimits(limits);
  EXPECT_FALSE(h.vm.bindingsDescriptor(*descriptor));
  limits.maxInspectionWork = 10000;
  h.vm.setBindingsLimits(limits);
  EXPECT_TRUE(h.vm.bindingsDescriptor(*descriptor));
  limits.maxMetadataRelations = 1;
  h.vm.setBindingsLimits(limits);
  const auto before = h.arena.cellCount();
  EXPECT_FALSE(h.vm.makeBindingsDescriptor(state));
  EXPECT_EQ(h.arena.cellCount(), before);
}

TEST(VortexBindingRuntimeTest, EveryFuelFailureRestoresMapsCursorAndLoadQuota) {
  bool completed       = false;
  std::size_t failures = 0;
  for (std::size_t fuel = 0; fuel < 1000; ++fuel) {
    BindingHarness h;
    const auto slot        = h.arena.makeCell();
    const auto old         = h.arena.makeCell("old");
    const auto replacement = h.arena.makeCell("replacement");
    RuntimeBindingSnapshot original;
    original.streams.emplace(slot, std::vector<CellRef>{old});
    h.vm.restoreBindings(original);
    RuntimeBindingSnapshot next;
    next.streams.emplace(slot, std::vector<CellRef>{replacement, replacement});
    const auto descriptor = h.vm.makeBindingsDescriptor(next);
    ASSERT_TRUE(descriptor);
    auto limits        = h.vm.bindingsLimits();
    limits.maxRestores = 2;
    h.vm.setBindingsLimits(limits);
    const auto restore = h.restore(*descriptor);
    const auto halt    = h.vm.mintOpcode(OpcodeKind::Halt);
    h.next(restore, halt);
    const auto cursor = h.vm.spawnCursor(restore, "restore-fuel-sweep");
    h.arena.setExecutionLimits(std::numeric_limits<std::uint32_t>::max(), fuel,
                               std::numeric_limits<std::size_t>::max());
    const auto result = h.vm.step(cursor);
    if (result.success) {
      EXPECT_EQ(h.vm.resultCells(slot),
                (std::vector<CellRef>{replacement, replacement}));
      completed = true;
      break;
    }
    ++failures;
    EXPECT_EQ(h.vm.resultCells(slot), (std::vector<CellRef>{old}));
    EXPECT_EQ(h.vm.getCursorOpcode(cursor), restore);
    EXPECT_EQ(h.arena.asInt64(cursor), static_cast<std::int64_t>(restore));
    h.arena.setExecutionLimits(std::numeric_limits<std::uint32_t>::max(),
                               std::numeric_limits<std::size_t>::max(),
                               std::numeric_limits<std::size_t>::max());
    const auto retry = h.vm.step(cursor);
    ASSERT_TRUE(retry.success) << retry.errorMessage << " after fuel " << fuel;
    EXPECT_EQ(h.vm.resultCells(slot),
              (std::vector<CellRef>{replacement, replacement}));
  }
  EXPECT_GT(failures, 0u);
  EXPECT_TRUE(completed);
}

} // namespace
} // namespace zigzag::vortex
