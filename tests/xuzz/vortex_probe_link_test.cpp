#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/xanadu/vortex/mesh_evaluator.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"

namespace {
using zigzag::CellRef;
using zigzag::DimVector;
using zigzag::vortex::OpcodeKind;

struct Harness {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  zigzag::vortex::VortexVM vm{core};
  const zigzag::DimRef dimension{core.findOrMintDimension("d.probe")};

  CellRef probe(CellRef anchor, CellRef dim, CellRef direction,
                CellRef output) {
    const auto opcode = vm.mintOpcode(OpcodeKind::ProbeLink);
    core.bindInput(opcode, anchor);
    core.bindInput(opcode, dim);
    core.bindInput(opcode, direction);
    core.bindOutput(opcode, output);
    return opcode;
  }

  CellRef probe(CellRef anchor, std::int64_t direction, CellRef output) {
    return probe(anchor,
                 arena.makeScalarCell(static_cast<std::int64_t>(dimension)),
                 arena.makeScalarCell(direction), output);
  }
};

TEST(VortexProbeLinkTest, SingleHopPreservesSelfLinksAndFalseValuedNeighbors) {
  Harness h;
  const auto self   = h.arena.makeCell("self");
  const auto output = h.arena.makeCell();
  zigzag::expectWritten(h.arena.link(self, h.dimension, DimVector::POS, self));
  for (const auto direction : {std::int64_t{1}, std::int64_t{-1}}) {
    const auto cursor = h.vm.spawnCursor(h.probe(self, direction, output));
    ASSERT_TRUE(h.vm.step(cursor).success);
    EXPECT_EQ(h.vm.resultCell(output), self);
  }

  const auto anchor   = h.arena.makeCell("anchor");
  const auto neighbor = h.arena.makeScalarCell(false);
  const auto beyond   = h.arena.makeCell("beyond");
  zigzag::expectWritten(
      h.arena.link(anchor, h.dimension, DimVector::POS, neighbor));
  zigzag::expectWritten(
      h.arena.link(neighbor, h.dimension, DimVector::POS, beyond));
  const auto opcode = h.probe(anchor, 1, output);
  const auto cursor = h.vm.spawnCursor(opcode);
  ASSERT_TRUE(h.vm.step(cursor).success);
  EXPECT_EQ(h.vm.resultCell(output), neighbor);
  EXPECT_EQ(h.core.render(*h.vm.resultCell(output)),
            zigzag::vortex::CellValue{false});

  zigzag::expectWritten(h.arena.unlink(anchor, h.dimension, DimVector::POS));
  h.vm.setCursorOpcode(cursor, opcode);
  ASSERT_TRUE(h.vm.step(cursor).success);
  EXPECT_FALSE(h.vm.resultCell(output));
}

TEST(VortexProbeLinkTest, InvalidAxisOrDirectionRefusesBeforeOutputChanges) {
  Harness h;
  const auto anchor   = h.arena.makeCell("anchor");
  const auto neighbor = h.arena.makeCell("neighbor");
  const auto output   = h.arena.makeCell();
  zigzag::expectWritten(
      h.arena.link(anchor, h.dimension, DimVector::POS, neighbor));
  const auto valid = h.probe(anchor, 1, output);
  ASSERT_TRUE(h.vm.step(h.vm.spawnCursor(valid)).success);
  ASSERT_EQ(h.vm.resultCell(output), neighbor);
  const std::vector<std::pair<CellRef, CellRef>> malformed{
      {h.arena.makeScalarCell(std::int64_t{-1}),
       h.arena.makeScalarCell(std::int64_t{1})},
      {h.arena.makeScalarCell(static_cast<std::int64_t>(
                                  std::numeric_limits<zigzag::DimRef>::max()) +
                              1),
       h.arena.makeScalarCell(std::int64_t{1})},
      {h.arena.makeScalarCell(static_cast<std::int64_t>(h.dimension)),
       h.arena.makeScalarCell(std::int64_t{0})},
      {h.arena.makeScalarCell(static_cast<std::int64_t>(h.dimension)),
       h.arena.makeScalarCell(1.0)},
      {h.arena.makeCell("not a dimension"),
       h.arena.makeScalarCell(std::int64_t{1})}};
  for (const auto &[dimension, direction] : malformed) {
    const auto opcode = h.probe(anchor, dimension, direction, output);
    const auto failed = h.vm.step(h.vm.spawnCursor(opcode));
    EXPECT_FALSE(failed.success);
    EXPECT_EQ(h.vm.resultCell(output), neighbor);
  }
}

TEST(VortexProbeLinkTest, MalformedArityRefusesWithoutChangingOutputs) {
  for (int mode = 0; mode < 4; ++mode) {
    Harness h;
    const auto opcode = h.vm.mintOpcode(OpcodeKind::ProbeLink);
    const auto output = h.arena.makeScalarCell(true);
    if (mode != 0) {
      h.core.bindInput(opcode, h.core.home());
      h.core.bindInput(opcode, h.arena.makeScalarCell(
                                   static_cast<std::int64_t>(h.dimension)));
      if (mode != 1)
        h.core.bindInput(opcode, h.arena.makeScalarCell(std::int64_t{1}));
    }
    if (mode != 2) h.core.bindOutput(opcode, output);
    if (mode == 3) h.core.bindOutput(opcode, h.arena.makeCell());
    const auto cursor = h.vm.spawnCursor(opcode);
    EXPECT_FALSE(h.vm.step(cursor).success);
    EXPECT_EQ(h.arena.asBool(output), std::optional{true});
    EXPECT_FALSE(h.vm.cellReferenceOf(output));
  }
}

TEST(VortexProbeLinkTest, ProbeSpendsWorkAndCanRecoverAfterBudgetReset) {
  Harness h;
  const auto anchor   = h.arena.makeCell("anchor");
  const auto neighbor = h.arena.makeCell("neighbor");
  const auto output   = h.arena.makeCell();
  zigzag::expectWritten(
      h.arena.link(anchor, h.dimension, DimVector::POS, neighbor));
  const auto opcode = h.probe(anchor, 1, output);
  const auto cursor = h.vm.spawnCursor(opcode);
  h.arena.setExecutionLimits(100'000, 0, 100'000);
  EXPECT_FALSE(h.vm.step(cursor).success);
  EXPECT_FALSE(h.vm.cellReferenceOf(output));
  h.arena.setExecutionLimits(100'000, 100'000, 100'000);
  h.vm.setCursorOpcode(cursor, opcode);
  ASSERT_TRUE(h.vm.step(cursor).success);
  EXPECT_EQ(h.vm.resultCell(output), neighbor);
}

TEST(VortexProbeLinkTest, UnsupportedCompositionRefusesBeforeProbing) {
  for (int mode = 0; mode < 4; ++mode) {
    Harness h;
    const auto anchor = h.arena.makeCell("anchor");
    const auto output = h.arena.makeScalarCell(true);
    const auto opcode = h.probe(anchor, 1, output);
    const auto side   = h.vm.mintOpcode(OpcodeKind::Nop);
    if (mode == 0)
      h.core.attachPipeline(output, side);
    else if (mode == 1)
      h.core.attachPrecondition(opcode, side);
    else if (mode == 2)
      h.core.attachPostcondition(opcode, side);
    else
      h.vm.enableMemoization(opcode, "probe-link");
    const auto cursor = h.vm.spawnCursor(opcode);
    const auto before = h.arena.cellCount();
    EXPECT_FALSE(h.vm.step(cursor).success);
    EXPECT_FALSE(h.vm.cellReferenceOf(output));
    EXPECT_EQ(h.arena.asBool(output), std::optional{true});
    EXPECT_EQ(h.arena.cellCount(), before);
  }
}
TEST(VortexProbeLinkTest,
     ProbeAsAnotherOpcodeConditionRefusesBeforeItsEffects) {
  for (const bool postcondition : {false, true}) {
    Harness h;
    const auto output      = h.arena.makeScalarCell(std::int64_t{91});
    const auto probeResult = h.arena.makeCell();
    const auto probe       = h.probe(h.core.home(), 1, probeResult);
    const auto add         = h.vm.mintOpcode(OpcodeKind::Add);
    h.core.bindInput(add, h.arena.makeScalarCell(std::int64_t{7}));
    h.core.bindInput(add, h.arena.makeScalarCell(std::int64_t{3}));
    h.core.bindOutput(add, output);
    if (postcondition)
      h.core.attachPostcondition(add, probe);
    else
      h.core.attachPrecondition(add, probe);
    const auto result = h.vm.step(h.vm.spawnCursor(add));
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.errorMessage.find("ProbeLink"), std::string::npos);
    EXPECT_EQ(h.arena.asInt64(output), std::optional{std::int64_t{91}});
    EXPECT_FALSE(h.vm.cellReferenceOf(probeResult));
  }
}

TEST(VortexProbeLinkTest, SealedMeshArtifactRelocatesTheProbeDimension) {
  namespace mesh = zigzag::vortex::mesh;
  const mesh::VortexProgram program{
      .cells        = {{},
                       {},
                       {zigzag::vortex::CellValue{std::string("anchor")},
                        {{"d.probe", 3}}},
                       {zigzag::vortex::CellValue{std::string("neighbor")}, {}},
                       {mesh::VortexDimension{"d.probe"}, {}},
                       {mesh::VortexReference{4}, {}},
                       {zigzag::vortex::CellValue{std::int64_t{1}}, {}},
                       {}},
      .instructions = {{0, OpcodeKind::ProbeLink, {2, 5, 6}, {7}, 1},
                       {1, OpcodeKind::Halt, {}, {}, {}}},
      .entry        = 0,
      .outputs      = {{7, true}}};
  const auto bytes = mesh::sealVortexProgram(program);
  ASSERT_TRUE(bytes) << bytes.error();
  const auto result = mesh::evaluate(mesh::Frontend::Vortex, *bytes,
                                     {100'000, 100'000, 16, 1'000'000});
  ASSERT_TRUE(result) << result.error();
  const auto &arena = result->context->arena;
  const auto along =
      [&](const CellRef cell,
          const std::string_view name) -> std::optional<CellRef> {
    for (const auto &edge : arena.dimensionsOf(cell))
      if (arena.textOf(edge.dim) == name)
        return arena.linked(cell, edge.dim, DimVector::POS);
    return std::nullopt;
  };
  const auto record = along(result->root, "d.mesh.items");
  ASSERT_TRUE(record);
  const auto value = along(*record, "d.mesh.value");
  ASSERT_TRUE(value);
  EXPECT_EQ(arena.textOf(*value), "neighbor");
}

} // namespace
