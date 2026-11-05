#include <gtest/gtest.h>

#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"

namespace {

using zigzag::ArenaManifold;
using zigzag::CellRef;
using zigzag::DimVector;
using zigzag::vortex::OpcodeKind;

struct Harness {
  ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  xanadu::MultiStoreCoordinator coordinator{core};
  zigzag::vortex::VortexVM vm{core};
  zigzag::DimRef valueDimension{coordinator.resolveDimension("d.value")};

  Harness() { vm.setContractCoordinator(coordinator); }

  CellRef probe(const CellRef owner, const std::string_view name,
                const std::int64_t direction, const CellRef output) {
    const auto opcode = vm.mintOpcode(OpcodeKind::VqlFieldProbe);
    core.bindInput(opcode, owner);
    core.bindInput(opcode, arena.makeCell(name));
    core.bindInput(opcode, arena.makeScalarCell(direction));
    core.bindOutput(opcode, output);
    return opcode;
  }

  void next(const CellRef from, const CellRef to) {
    ASSERT_TRUE(arena.link(from, core.dims().spin, DimVector::POS, to));
  }
};

TEST(VortexFieldProbeTest, PresenceIncludesZeroFalseAndEmptyText) {
  Harness h;
  const CellRef owner = h.arena.makeCell("owner");
  const std::vector<CellRef> values{h.arena.makeScalarCell(std::int64_t{0}),
                                    h.arena.makeScalarCell(false),
                                    h.arena.makeCell("")};
  for (const CellRef value : values) {
    ASSERT_TRUE(h.arena.link(owner, h.valueDimension, DimVector::POS, value));
    const CellRef output = h.arena.makeCell();
    const auto result =
        h.vm.step(h.vm.spawnCursor(h.probe(owner, "d.value", 1, output)));
    ASSERT_TRUE(result.success) << result.errorMessage;
    EXPECT_EQ(h.vm.resultCell(output), value);
    ASSERT_TRUE(h.arena.unlink(owner, h.valueDimension, DimVector::POS));
  }

  const CellRef absentOutput = h.arena.makeCell();
  const auto absent =
      h.vm.step(h.vm.spawnCursor(h.probe(owner, "d.value", 1, absentOutput)));
  ASSERT_TRUE(absent.success) << absent.errorMessage;
  EXPECT_FALSE(h.vm.resultCell(absentOutput));

  const CellRef reverse = h.arena.makeCell("reverse");
  ASSERT_TRUE(h.arena.link(owner, h.valueDimension, DimVector::NEG, reverse));
  const CellRef reverseOutput = h.arena.makeCell();
  const auto reverseResult =
      h.vm.step(h.vm.spawnCursor(h.probe(owner, "d.value", -1, reverseOutput)));
  ASSERT_TRUE(reverseResult.success) << reverseResult.errorMessage;
  EXPECT_EQ(h.vm.resultCell(reverseOutput), reverse);
}

TEST(VortexFieldProbeTest, ResolvesNameInsideScopeAndRestoresMintedDimension) {
  Harness h;
  const CellRef owner       = h.arena.makeCell("owner");
  const CellRef probeOut    = h.arena.makeCell();
  const CellRef scoped      = h.vm.mintOpcode(OpcodeKind::ScopedChoice);
  const CellRef probe       = h.probe(owner, "d.temporary", 1, probeOut);
  const CellRef fail        = h.vm.mintOpcode(OpcodeKind::Fail);
  const CellRef alternative = h.vm.mintOpcode(OpcodeKind::Halt);
  h.core.bindInput(scoped, alternative);
  h.next(scoped, probe);
  h.next(probe, fail);
  EXPECT_FALSE(h.core.findDimension("d.temporary"));
  const CellRef cursor   = h.vm.spawnCursor(scoped);
  const auto priorResult = h.vm.resultCells(probeOut);

  ASSERT_TRUE(h.vm.step(cursor).success);
  EXPECT_FALSE(h.core.findDimension("d.temporary"));
  ASSERT_TRUE(h.vm.step(cursor).success);
  EXPECT_TRUE(h.core.findDimension("d.temporary"));
  ASSERT_TRUE(h.vm.step(cursor).success);
  EXPECT_EQ(h.vm.getCursorOpcode(cursor), alternative);
  EXPECT_EQ(h.vm.resultCells(probeOut), priorResult);
  EXPECT_FALSE(h.core.findDimension("d.temporary"));
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);
}

TEST(VortexFieldProbeTest,
     AbortRestoresRegistersAndLeavesCallerChoiceUnpruned) {
  Harness h;
  const CellRef oldValue   = h.arena.makeCell("old");
  const CellRef newValue   = h.arena.makeCell("new");
  const CellRef reference  = h.arena.makeCell();
  const CellRef stream     = h.arena.makeCell();
  const CellRef copyOld    = h.vm.mintOpcode(OpcodeKind::CopyCellRefs);
  const CellRef clearOld   = h.vm.mintOpcode(OpcodeKind::StreamClear);
  const CellRef appendOld  = h.vm.mintOpcode(OpcodeKind::StreamAppend);
  const CellRef scoped     = h.vm.mintOpcode(OpcodeKind::ScopedChoice);
  const CellRef copyNew    = h.vm.mintOpcode(OpcodeKind::CopyCellRefs);
  const CellRef clearNew   = h.vm.mintOpcode(OpcodeKind::StreamClear);
  const CellRef appendNew  = h.vm.mintOpcode(OpcodeKind::StreamAppend);
  const CellRef box        = h.vm.mintOpcode(OpcodeKind::VqlBoxScalar);
  const CellRef boxed      = h.arena.makeCell();
  const CellRef scalar     = h.arena.makeScalarCell(std::int64_t{17});
  const CellRef invoke     = h.vm.mintOpcode(OpcodeKind::InvokeFrame);
  const CellRef frameBody  = h.vm.mintOpcode(OpcodeKind::Nop);
  const CellRef frameRet   = h.vm.mintOpcode(OpcodeKind::ReturnFrame);
  const CellRef descriptor = *h.vm.makeFrameDescriptor(
      zigzag::vortex::FrameDescriptorSpec{.entry = frameBody});
  const CellRef halt              = h.vm.mintOpcode(OpcodeKind::Halt);
  const CellRef callerAlternative = h.vm.mintOpcode(OpcodeKind::Halt);
  const CellRef scopeAlternative  = h.vm.mintOpcode(OpcodeKind::Halt);
  h.core.bindInput(copyOld, oldValue);
  h.core.bindOutput(copyOld, reference);
  h.core.bindOutput(clearOld, stream);
  h.core.bindInput(appendOld, oldValue);
  h.core.bindOutput(appendOld, stream);
  h.core.bindInput(scoped, scopeAlternative);
  h.core.bindInput(copyNew, newValue);
  h.core.bindOutput(copyNew, reference);
  h.core.bindOutput(clearNew, stream);
  h.core.bindInput(appendNew, newValue);
  h.core.bindOutput(appendNew, stream);
  h.core.bindInput(box, scalar);
  h.core.bindOutput(box, boxed);
  h.core.bindInput(invoke, descriptor);
  h.core.bindInput(frameRet, descriptor);
  h.next(frameBody, frameRet);
  const CellRef code[]{copyOld,  clearOld,  appendOld, scoped, copyNew,
                       clearNew, appendNew, box,       invoke, halt};
  for (std::size_t i = 1; i < std::size(code); ++i)
    h.next(code[i - 1], code[i]);

  std::vector<std::string> callbacks;
  h.vm.setCutCallback(
      [&](const std::string_view path) { callbacks.emplace_back(path); });
  const CellRef cursor = h.vm.spawnCursor(copyOld);
  ASSERT_TRUE(h.vm.step(cursor).success);
  ASSERT_TRUE(h.vm.step(cursor).success);
  ASSERT_TRUE(h.vm.step(cursor).success);
  EXPECT_EQ(h.vm.resultCell(reference), oldValue);
  ASSERT_EQ(h.vm.resultCells(stream), (std::vector<CellRef>{oldValue}));
  h.vm.pushChoicePoint(cursor, callerAlternative);
  const auto priorBoxed        = h.vm.resultCells(boxed);
  const auto cellsAtScopeEntry = h.arena.cellCount();
  ASSERT_TRUE(h.vm.step(cursor).success);
  ASSERT_TRUE(h.vm.step(cursor).success);
  ASSERT_TRUE(h.vm.step(cursor).success);
  ASSERT_TRUE(h.vm.step(cursor).success);
  ASSERT_TRUE(h.vm.step(cursor).success);
  ASSERT_TRUE(h.vm.step(cursor).success);
  EXPECT_EQ(h.vm.activationDepth(cursor), 1U);
  EXPECT_EQ(h.vm.resultCell(reference), newValue);
  EXPECT_EQ(h.vm.resultCells(stream), (std::vector<CellRef>{newValue}));
  ASSERT_TRUE(h.vm.resultCell(boxed));
  ASSERT_EQ(h.vm.choiceDepth(), 2U);

  h.vm.abortScopedChoices(cursor);
  h.vm.setCursorOpcode(cursor, std::nullopt);
  EXPECT_EQ(h.vm.choiceDepth(), 1U);
  EXPECT_FALSE(h.vm.choiceStack().back().scoped);
  EXPECT_EQ(h.vm.resultCell(reference), oldValue);
  EXPECT_EQ(h.vm.resultCells(stream), (std::vector<CellRef>{oldValue}));
  EXPECT_EQ(h.vm.resultCells(boxed), priorBoxed);
  EXPECT_EQ(h.arena.cellCount(), cellsAtScopeEntry);
  EXPECT_EQ(h.vm.activationDepth(cursor), 0U);
  EXPECT_FALSE(h.vm.hasActiveFrames());
  EXPECT_TRUE(callbacks.empty());
  EXPECT_EQ(h.arena.outstandingMarks(), 1U);
  ASSERT_TRUE(h.vm.backtrack(cursor));
  EXPECT_EQ(h.vm.getCursorOpcode(cursor), callerAlternative);
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);
}

TEST(VortexFieldProbeTest, RawCycleLimitCanResumeAnOpenScopedChoice) {
  Harness h;
  const CellRef scoped      = h.vm.mintOpcode(OpcodeKind::ScopedChoice);
  const CellRef fail        = h.vm.mintOpcode(OpcodeKind::Fail);
  const CellRef alternative = h.vm.mintOpcode(OpcodeKind::Halt);
  h.core.bindInput(scoped, alternative);
  h.next(scoped, fail);
  const CellRef cursor = h.vm.spawnCursor(scoped);

  const auto limited = h.vm.run(cursor, 1);
  EXPECT_FALSE(limited.success);
  EXPECT_EQ(h.vm.choiceDepth(), 1U);
  EXPECT_EQ(h.arena.outstandingMarks(), 1U);
  const auto resumed = h.vm.run(cursor, 8);
  ASSERT_TRUE(resumed.success) << resumed.errorMessage;
  EXPECT_EQ(h.vm.choiceDepth(), 0U);
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);
}

TEST(VortexFieldProbeTest, RejectsOversizedAndFragmentedNamesBeforeEffects) {
  for (const bool fragmented : {false, true}) {
    Harness h;
    const auto owner  = h.arena.makeCell("owner");
    const auto output = h.arena.makeCell("unchanged");
    const auto name =
        h.arena.makeCell(fragmented ? "" : std::string(65537, 'x'));
    if (fragmented) {
      const auto fragment = h.arena.intern("xy");
      const std::vector<xanadu::PrimediaSpan> fragments(32769, fragment);
      ASSERT_TRUE(h.arena.setContent(name, fragments));
    }
    const auto probe = h.vm.mintOpcode(OpcodeKind::VqlFieldProbe);
    h.core.bindInput(probe, owner);
    h.core.bindInput(probe, name);
    h.core.bindInput(probe, h.arena.makeScalarCell(std::int64_t{1}));
    h.core.bindOutput(probe, output);
    const auto cursor = h.vm.spawnCursor(probe);
    const auto cells  = h.arena.cellCount();
    const auto result = h.vm.step(cursor);
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.errorMessage.find("text limit"), std::string::npos);
    EXPECT_EQ(h.arena.textOf(output), "unchanged");
    EXPECT_EQ(h.arena.cellCount(), cells);
    EXPECT_EQ(h.arena.outstandingMarks(), 0u);
  }
}

TEST(VortexFieldProbeTest, WorkRefusalRestoresDimensionScratchAndOutput) {
  Harness h;
  const auto owner       = h.arena.makeCell("owner");
  const auto output      = h.arena.makeCell("unchanged");
  const std::string name = "d.never";
  const auto cursor      = h.vm.spawnCursor(h.probe(owner, name, 1, output));
  const auto before      = h.arena.mark();
  h.arena.discard(before);
  h.arena.setExecutionLimits(100000, name.size() + 2, 100000);
  const auto result = h.vm.step(cursor);
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.errorMessage.find("work budget"), std::string::npos);
  EXPECT_FALSE(h.core.findDimension(name));
  EXPECT_EQ(h.arena.textOf(output), "unchanged");
  const auto after = h.arena.mark();
  h.arena.discard(after);
  EXPECT_EQ(after.cellCount, before.cellCount);
  EXPECT_EQ(after.scratchSize, before.scratchSize);
  EXPECT_EQ(h.arena.outstandingMarks(), 0u);
}

TEST(VortexFieldProbeTest, AssertRetainsLegacyFailureAndSupportsDiagnostic) {
  for (const bool custom : {false, true}) {
    Harness h;
    const auto assertion = h.vm.mintOpcode(OpcodeKind::Assert);
    h.core.bindInput(assertion, h.arena.makeScalarCell(false));
    if (custom)
      h.core.bindInput(assertion, h.arena.makeCell("specific failure"));
    const auto result = h.vm.step(h.vm.spawnCursor(assertion));
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.contractViolation,
              zigzag::vortex::ContractViolationKind::None);
    EXPECT_EQ(result.errorMessage,
              custom ? "specific failure" : "Assertion failed");
  }
}

} // namespace
