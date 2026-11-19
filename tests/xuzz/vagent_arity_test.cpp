#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <set>
#include <string>

#include "common/xanadu/vagent/meta.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"

// vagent's `validate` knows an op kind only through the bound arity table.
// These tests keep that table honest against the VM's opcode enum: a kind
// the VM decodes is either in the table or on the named list of kinds
// vagent deliberately refuses, so appending an opcode cannot be skipped
// silently.
namespace {

using zigzag::CellRef;
using zigzag::vortex::OpcodeKind;

constexpr auto kLastKind             = OpcodeKind::IntOfCell;
constexpr auto kLastOrdinal          = static_cast<std::int64_t>(kLastKind);
constexpr std::int64_t kOrdinalSpace = 256; // OpcodeKind is a uint8_t

// Host-state, stream, guard, closure and frame kinds the table has never
// listed: validate refuses them by ordinal. Adding a kind to the table
// removes it from here; adding a kind to the VM without choosing a side
// fails VagentArityTest.EveryOpcodeIsInTheTableOrDeliberatelyRefused.
const std::set<OpcodeKind> &deliberatelyRefused() {
  static const std::set<OpcodeKind> kinds{
      OpcodeKind::MatchContract,
      OpcodeKind::RequireContract,
      OpcodeKind::CopyCellRefs,
      OpcodeKind::ClearCellRefs,
      OpcodeKind::BranchFalseCell,
      OpcodeKind::StreamClear,
      OpcodeKind::StreamAppend,
      OpcodeKind::StreamBegin,
      OpcodeKind::RankBegin,
      OpcodeKind::IteratorNext,
      OpcodeKind::StreamReverseBegin,
      OpcodeKind::VqlCompare,
      OpcodeKind::ResumeInit,
      OpcodeKind::ResumeSet,
      OpcodeKind::ProbeLink,
      OpcodeKind::InvokeFrame,
      OpcodeKind::ReturnFrame,
      OpcodeKind::VqlString,
      OpcodeKind::VqlAdd,
      OpcodeKind::VqlBoxScalar,
      OpcodeKind::ScopedChoice,
      OpcodeKind::VqlFieldProbe,
      OpcodeKind::PushGuard,
      OpcodeKind::PopGuard,
      OpcodeKind::CheckGuards,
      OpcodeKind::OpenGuardScope,
      OpcodeKind::CloseGuardScope,
      OpcodeKind::BeginMutationGroup,
      OpcodeKind::EndMutationGroup,
      OpcodeKind::VqlMaskLinks,
      OpcodeKind::CreateClosure,
      OpcodeKind::ApplyClosure,
      OpcodeKind::RestoreBindings,
      OpcodeKind::CurrentTimestamp,
      OpcodeKind::RelationNext,
      OpcodeKind::DimByName,
      OpcodeKind::Hypertime,
      OpcodeKind::Pow,
      OpcodeKind::SameCell,
      OpcodeKind::PinHistorical,
      OpcodeKind::CheckClosureCall,
      OpcodeKind::GroupBegin,
      OpcodeKind::GroupNext,
  };
  return kinds;
}

struct Harness {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  zigzag::vortex::VortexVM vm{core};
  std::optional<xanadu::vagent::MetaRuntime> meta;

  Harness() {
    auto binds = xanadu::vagent::mintMetaBindSet(arena, core, arena.makeCell(),
                                                 arena.makeCell());
    EXPECT_TRUE(binds.has_value()) << (binds ? "" : binds.error());
    if (binds) meta.emplace(core, vm, *binds);
  }

  /// An op of @p kind with @p inputs scalar operands and @p outputs output
  /// slots, validated as a lone (not run-reachable) op.
  xanadu::vagent::MetaValidateReport validateShape(
      OpcodeKind kind, std::size_t inputs, std::size_t outputs,
      xanadu::vagent::MetaValidateOptions options = {.runReachable = false}) {
    const auto op = vm.mintOpcode(kind);
    for (std::size_t i = 0; i < inputs; ++i)
      core.bindInput(op, arena.makeScalarCell(std::int64_t{1}));
    for (std::size_t i = 0; i < outputs; ++i)
      core.bindOutput(op, arena.makeCell());
    return meta->validate(op, options);
  }
};

bool refusedAsAbsent(const xanadu::vagent::MetaValidateReport &report) {
  return !report.ok &&
         report.detail.find("absent from the arity table") != std::string::npos;
}

TEST(VagentArityTest, TheCeilingIsTheLastOrdinalTheVmDecodes) {
  Harness h;
  std::int64_t lastDecoded = -1;
  for (std::int64_t ordinal = 0; ordinal < kOrdinalSpace; ++ordinal)
    if (h.vm.getOpcodeKind(h.arena.makeScalarCell(ordinal)))
      lastDecoded = ordinal;
  EXPECT_EQ(lastDecoded, kLastOrdinal)
      << "the VM's decode ceiling moved: mirror it in vagent/meta.cpp";
  EXPECT_EQ(h.arena.asInt64(h.meta->binds().validate.linkedCeiling),
            lastDecoded);
}

TEST(VagentArityTest, EveryOpcodeIsInTheTableOrDeliberatelyRefused) {
  Harness h;
  for (std::int64_t ordinal = 0; ordinal <= kLastOrdinal; ++ordinal) {
    const auto kind   = static_cast<OpcodeKind>(ordinal);
    const auto op     = h.vm.mintOpcode(kind);
    const auto report = h.meta->validate(op, {.runReachable = false});
    const bool absent = refusedAsAbsent(report);
    EXPECT_EQ(absent, deliberatelyRefused().contains(kind))
        << "opcode ordinal " << ordinal
        << (absent ? " has no arity entry and is not on the deliberately "
                     "refused list"
                   : " has an arity entry but is listed as deliberately "
                     "refused")
        << ": " << report.detail;
  }
}

TEST(VagentArityTest, AnAbsentKindIsRefusedNamingItsOrdinal) {
  Harness h;
  const auto report = h.validateShape(OpcodeKind::GroupNext, 0, 0);
  ASSERT_TRUE(refusedAsAbsent(report)) << report.detail;
  EXPECT_NE(report.detail.find(
                std::to_string(static_cast<int>(OpcodeKind::GroupNext))),
            std::string::npos)
      << report.detail;
}

TEST(VagentArityTest, ProjectTopologyTakesAHeadAndEdgePairsAndOneOutput) {
  Harness h;
  // subject, wildcard, added-count, then (dimension, sign) pairs.
  for (const std::size_t inputs : {3U, 5U, 9U}) {
    const auto report = h.validateShape(OpcodeKind::ProjectTopology, inputs, 1);
    EXPECT_TRUE(report.ok) << inputs << " inputs: " << report.detail;
  }
  for (const std::size_t inputs : {0U, 2U, 4U, 6U}) {
    const auto report = h.validateShape(OpcodeKind::ProjectTopology, inputs, 1);
    EXPECT_FALSE(report.ok) << inputs << " inputs";
    EXPECT_NE(report.detail.find("arity"), std::string::npos) << report.detail;
  }
  for (const std::size_t outputs : {0U, 2U})
    EXPECT_FALSE(h.validateShape(OpcodeKind::ProjectTopology, 3, outputs).ok)
        << outputs << " outputs";
}

TEST(VagentArityTest, VqlMaskProbeTakesOwnerDefaultAndMaskPairsAndOneOutput) {
  Harness h;
  // owner, default, then (dimension, mask) pairs.
  for (const std::size_t inputs : {2U, 4U, 8U}) {
    const auto report = h.validateShape(OpcodeKind::VqlMaskProbe, inputs, 1);
    EXPECT_TRUE(report.ok) << inputs << " inputs: " << report.detail;
  }
  for (const std::size_t inputs : {0U, 1U, 3U, 5U}) {
    const auto report = h.validateShape(OpcodeKind::VqlMaskProbe, inputs, 1);
    EXPECT_FALSE(report.ok) << inputs << " inputs";
    EXPECT_NE(report.detail.find("arity"), std::string::npos) << report.detail;
  }
  for (const std::size_t outputs : {0U, 2U})
    EXPECT_FALSE(h.validateShape(OpcodeKind::VqlMaskProbe, 2, outputs).ok)
        << outputs << " outputs";
}

TEST(VagentArityTest, TheNewKindsAreNotImpureStructuralWrites) {
  // ProjectTopology mints detached ephemeral cells and VqlMaskProbe only
  // reads; neither writes a Structure operation, so a pure-flagged fragment
  // may hold them (Clone is the precedent for minting without impurity).
  Harness h;
  const xanadu::vagent::MetaValidateOptions pure{.runReachable = false,
                                                 .expectPure   = true};
  EXPECT_TRUE(h.validateShape(OpcodeKind::ProjectTopology, 3, 1, pure).ok);
  EXPECT_TRUE(h.validateShape(OpcodeKind::VqlMaskProbe, 2, 1, pure).ok);
  EXPECT_FALSE(h.validateShape(OpcodeKind::New, 4, 1, pure).ok);
}

TEST(VagentArityTest, FixedKindsStayExact) {
  Harness h;
  EXPECT_TRUE(h.validateShape(OpcodeKind::Add, 2, 1).ok);
  EXPECT_FALSE(h.validateShape(OpcodeKind::Add, 4, 1).ok);
}

} // namespace
