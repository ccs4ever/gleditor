#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vortex/mesh_evaluator.hpp"
#include "common/xanadu/vortex/vortex_host.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/vql/execution.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vql {
namespace {

using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;
using zigzag::vortex::OpcodeKind;

struct RunResult {
  CompilationResult compilation;
  bool executed{false};
  std::string error;
  std::vector<CellRef> values;
  std::size_t maskOps{0};
  std::size_t boxOps{0};
  std::size_t beginGroups{0};
  std::size_t endGroups{0};
};

struct Harness {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  MultiStoreCoordinator coordinator{core};
  zigzag::vortex::VortexVM vm{core};

  DimRef dim(const std::string_view name) {
    return coordinator.resolveDimension(name);
  }

  CellRef cell(const std::string_view text) { return arena.makeCell(text); }

  void link(const CellRef owner, const std::string_view name,
            const DimVector direction, const CellRef target) {
    zigzag::expectWritten(arena.link(owner, dim(name), direction, target));
  }

  CompilationResult compile(const std::string_view source,
                            ContractBindings bindings = {}) {
    CompilationOptions options;
    options.initialBindings = std::move(bindings);
    VQLCompiler compiler(coordinator, vm);
    return compiler.compile(source, options);
  }

  RunResult run(const std::string_view source, ContractBindings bindings = {}) {
    RunResult result;
    result.compilation = compile(source, std::move(bindings));
    if (!result.compilation.success) {
      result.error = result.compilation.errorMessage;
      return result;
    }
    for (const CellRef opcode : result.compilation.generatedOpcodes) {
      const auto kind = vm.getOpcodeKind(opcode);
      result.maskOps += kind == OpcodeKind::VqlMaskLinks;
      result.boxOps += kind == OpcodeKind::VqlBoxScalar;
      result.beginGroups += kind == OpcodeKind::BeginMutationGroup;
      result.endGroups += kind == OpcodeKind::EndMutationGroup;
    }
    if (!result.compilation.cursor) {
      result.error = "compiled projection has no cursor";
      return result;
    }
    const auto execution = vm.run(*result.compilation.cursor, 100'000);
    result.executed      = execution.success;
    result.error         = execution.errorMessage;
    vm.setCursorOpcode(*result.compilation.cursor, std::nullopt);
    if (!execution.success) return result;
    for (const CellRef slot : result.compilation.resultSlots) {
      const auto cells = vm.resultCells(slot);
      result.values.insert(result.values.end(), cells.begin(), cells.end());
    }
    return result;
  }
};

struct TemporaryDirectory {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("vql_shape_projection_" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));

  TemporaryDirectory() { std::filesystem::create_directories(path); }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

void expectProjectedOwners(const RunResult &result,
                           const std::vector<CellRef> &owners) {
  ASSERT_TRUE(result.compilation.success) << result.compilation.errorMessage;
  ASSERT_TRUE(result.executed) << result.error;
  EXPECT_EQ(result.values, owners);
  EXPECT_GT(result.maskOps, 0U);
  EXPECT_GT(result.beginGroups, 0U);
  EXPECT_GT(result.endGroups, 0U);
}

TEST(VQLShapeProjectionVMTest, EmptyAndEverythingWildcardsMaskAllOrKeepAll) {
  Harness h;
  const CellRef owner = h.cell("owner");
  const CellRef kept  = h.cell("kept");
  const CellRef other = h.cell("other");
  h.link(owner, "d.keep", DimVector::POS, kept);
  h.link(owner, "d.other", DimVector::POS, other);

  const auto everything = h.run("$owners{*}", {{"owners", {owner}}});
  expectProjectedOwners(everything, {owner});
  EXPECT_EQ(h.arena.linked(owner, h.dim("d.keep"), DimVector::POS), kept);
  EXPECT_EQ(h.arena.linked(owner, h.dim("d.other"), DimVector::POS), other);

  const auto empty = h.run("$owners{}", {{"owners", {owner}}});
  expectProjectedOwners(empty, {owner});
  EXPECT_FALSE(h.arena.linked(owner, h.dim("d.keep"), DimVector::POS));
  EXPECT_FALSE(h.arena.linked(kept, h.dim("d.keep"), DimVector::NEG));
  EXPECT_FALSE(h.arena.linked(owner, h.dim("d.other"), DimVector::POS));
  EXPECT_FALSE(h.arena.linked(other, h.dim("d.other"), DimVector::NEG));
}

TEST(VQLShapeProjectionVMTest,
     DefaultExclusiveProjectionKeepsOnlyNamedDirectionAndReciprocates) {
  Harness h;
  const CellRef owner   = h.cell("owner");
  const CellRef keep    = h.cell("keep");
  const CellRef drop    = h.cell("drop");
  const CellRef negKeep = h.cell("neg keep");
  const CellRef negDrop = h.cell("neg drop");
  h.link(owner, "d.axis", DimVector::POS, keep);
  h.link(owner, "d.axis", DimVector::NEG, negDrop);
  h.link(negKeep, "d.reverse", DimVector::POS, owner);
  h.link(owner, "d.discard", DimVector::POS, drop);

  const auto result = h.run("$owners{+d.axis}", {{"owners", {owner}}});
  expectProjectedOwners(result, {owner});
  EXPECT_EQ(h.arena.linked(owner, h.dim("d.axis"), DimVector::POS), keep);
  EXPECT_FALSE(h.arena.linked(owner, h.dim("d.axis"), DimVector::NEG));
  EXPECT_FALSE(h.arena.linked(negDrop, h.dim("d.axis"), DimVector::POS));
  EXPECT_FALSE(h.arena.linked(owner, h.dim("d.reverse"), DimVector::NEG));
  EXPECT_FALSE(h.arena.linked(owner, h.dim("d.discard"), DimVector::POS));
  EXPECT_FALSE(h.arena.linked(drop, h.dim("d.discard"), DimVector::NEG));
}

TEST(VQLShapeProjectionVMTest,
     DirectionalAndBidirectionalRulesRetainTheRequestedLinks) {
  Harness h;
  const CellRef owner   = h.cell("owner");
  const CellRef pos     = h.cell("pos");
  const CellRef neg     = h.cell("neg");
  const CellRef bothPos = h.cell("both pos");
  const CellRef bothNeg = h.cell("both neg");
  h.link(owner, "d.pos", DimVector::POS, pos);
  h.link(owner, "d.neg", DimVector::NEG, neg);
  h.link(owner, "d.both", DimVector::POS, bothPos);
  h.link(owner, "d.both", DimVector::NEG, bothNeg);

  const auto result =
      h.run("$owners{+d.pos, -d.neg, ~d.both}", {{"owners", {owner}}});
  expectProjectedOwners(result, {owner});
  EXPECT_EQ(h.arena.linked(owner, h.dim("d.pos"), DimVector::POS), pos);
  EXPECT_FALSE(h.arena.linked(owner, h.dim("d.pos"), DimVector::NEG));
  EXPECT_EQ(h.arena.linked(owner, h.dim("d.neg"), DimVector::NEG), neg);
  EXPECT_FALSE(h.arena.linked(owner, h.dim("d.neg"), DimVector::POS));
  EXPECT_EQ(h.arena.linked(owner, h.dim("d.both"), DimVector::POS), bothPos);
  EXPECT_EQ(h.arena.linked(owner, h.dim("d.both"), DimVector::NEG), bothNeg);
}

TEST(VQLShapeProjectionVMTest, NegatedDimensionExcludesBothDirections) {
  Harness h;
  const CellRef owner = h.cell("owner");
  const CellRef pos   = h.cell("pos");
  const CellRef neg   = h.cell("neg");
  const CellRef other = h.cell("other");
  h.link(owner, "d.excluded", DimVector::POS, pos);
  h.link(owner, "d.excluded", DimVector::NEG, neg);
  h.link(owner, "d.other", DimVector::POS, other);

  const auto result = h.run("$owners{*, !d.excluded}", {{"owners", {owner}}});
  expectProjectedOwners(result, {owner});
  EXPECT_FALSE(h.arena.linked(owner, h.dim("d.excluded"), DimVector::POS));
  EXPECT_FALSE(h.arena.linked(owner, h.dim("d.excluded"), DimVector::NEG));
  EXPECT_FALSE(h.arena.linked(pos, h.dim("d.excluded"), DimVector::NEG));
  EXPECT_FALSE(h.arena.linked(neg, h.dim("d.excluded"), DimVector::POS));
  EXPECT_EQ(h.arena.linked(owner, h.dim("d.other"), DimVector::POS), other);
}

TEST(VQLShapeProjectionVMTest, WildcardResetsEarlierMasks) {
  Harness h;
  const CellRef owner = h.cell("owner");
  const CellRef keep  = h.cell("keep");
  const CellRef drop  = h.cell("drop");
  const CellRef extra = h.cell("extra");
  h.link(owner, "d.keep", DimVector::POS, keep);
  h.link(owner, "d.drop", DimVector::POS, drop);
  h.link(owner, "d.extra", DimVector::POS, extra);

  const auto result =
      h.run("$owners{+d.keep, !*, +d.drop}", {{"owners", {owner}}});
  expectProjectedOwners(result, {owner});
  EXPECT_FALSE(h.arena.linked(owner, h.dim("d.keep"), DimVector::POS));
  EXPECT_EQ(h.arena.linked(owner, h.dim("d.drop"), DimVector::POS), drop);
  EXPECT_FALSE(h.arena.linked(owner, h.dim("d.extra"), DimVector::POS));
}

TEST(VQLShapeProjectionVMTest,
     LiteralAssignmentsPreserveOwnerStreamAndBoxExactScalarsPerOwner) {
  Harness h;
  const CellRef first  = h.cell("first");
  const CellRef second = h.cell("second");
  const CellRef oldA   = h.cell("old A");
  const CellRef oldB   = h.cell("old B");
  h.link(first, "d.keep", DimVector::POS, oldA);
  h.link(second, "d.keep", DimVector::POS, oldB);
  h.link(first, "d.unmentioned", DimVector::POS, h.cell("remove A"));
  h.link(second, "d.unmentioned", DimVector::POS, h.cell("remove B"));

  const auto integer = h.run("$owners{+d.keep, +d.score: 9007199254740993}",
                             {{"owners", {first, second}}});
  expectProjectedOwners(integer, {first, second});
  EXPECT_GT(integer.boxOps, 0U);
  const auto score       = h.dim("d.score");
  const auto firstScore  = h.arena.linked(first, score, DimVector::POS);
  const auto secondScore = h.arena.linked(second, score, DimVector::POS);
  ASSERT_TRUE(firstScore);
  ASSERT_TRUE(secondScore);
  EXPECT_NE(*firstScore, *secondScore);
  EXPECT_EQ(h.arena.asInt64(*firstScore), INT64_C(9007199254740993));
  EXPECT_EQ(h.arena.asInt64(*secondScore), INT64_C(9007199254740993));
  EXPECT_FALSE(h.arena.linked(first, h.dim("d.unmentioned"), DimVector::POS));
  EXPECT_FALSE(h.arena.linked(second, h.dim("d.unmentioned"), DimVector::POS));
  EXPECT_EQ(h.arena.linked(first, h.dim("d.keep"), DimVector::POS), oldA);
  EXPECT_EQ(h.arena.linked(second, h.dim("d.keep"), DimVector::POS), oldB);

  const auto boolean =
      h.run("$owners{+d.flag: false}", {{"owners", {first, second}}});
  expectProjectedOwners(boolean, {first, second});
  EXPECT_GT(boolean.boxOps, 0U);
  const auto firstFlag = h.arena.linked(first, h.dim("d.flag"), DimVector::POS);
  const auto secondFlag =
      h.arena.linked(second, h.dim("d.flag"), DimVector::POS);
  ASSERT_TRUE(firstFlag);
  ASSERT_TRUE(secondFlag);
  EXPECT_NE(*firstFlag, *secondFlag);
  EXPECT_EQ(h.arena.asBool(*firstFlag), false);
  EXPECT_EQ(h.arena.asBool(*secondFlag), false);

  const auto real = h.run("$owners{+d.real: 9007199254740992.0}",
                          {{"owners", {first, second}}});
  expectProjectedOwners(real, {first, second});
  EXPECT_GT(real.boxOps, 0U);
  const auto firstReal = h.arena.linked(first, h.dim("d.real"), DimVector::POS);
  const auto secondReal =
      h.arena.linked(second, h.dim("d.real"), DimVector::POS);
  ASSERT_TRUE(firstReal);
  ASSERT_TRUE(secondReal);
  EXPECT_NE(*firstReal, *secondReal);
  EXPECT_EQ(h.arena.asDouble(*firstReal), 0x1p53);
  EXPECT_EQ(h.arena.asDouble(*secondReal), 0x1p53);
}

TEST(VQLShapeProjectionVMTest,
     LiteralAssignmentForRepeatedOwnersKeepsResultDuplicatesAndCurrentTarget) {
  Harness h;
  const CellRef owner = h.cell("owner");
  const CellRef old   = h.cell("old");
  h.link(owner, "d.tag", DimVector::POS, old);

  const auto result =
      h.run("$owners{+d.tag: \"new\"}", {{"owners", {owner, owner}}});
  expectProjectedOwners(result, {owner, owner});
  EXPECT_GT(result.boxOps, 0U);
  const auto target = h.arena.linked(owner, h.dim("d.tag"), DimVector::POS);
  ASSERT_TRUE(target);
  EXPECT_EQ(h.arena.textOf(*target), "new");
  EXPECT_FALSE(h.arena.linked(old, h.dim("d.tag"), DimVector::NEG));
}

TEST(VQLShapeProjectionVMTest,
     TypedCellGuardRollsBackInvalidProjectionAndAcceptsValidReplacement) {
  Harness h;
  const CellRef owner = h.cell("owner");
  const CellRef value = h.arena.makeScalarCell(std::int64_t{12});
  h.link(owner, "d.keep", DimVector::POS, value);

  QueryExecutor invalidExecutor(h.coordinator, h.vm, {}, {{"owner", {owner}}});
  const auto invalid = invalidExecutor.execute(
      "def weave replace($owner: Cell { +d.keep: Int }): Cell := "
      "$owner{+d.keep: \"bad\"}; replace($owner)");
  ASSERT_FALSE(invalid) << "result count=" << invalid->size() << "\n"
                        << invalidExecutor.disassembly();
  EXPECT_EQ(invalid.error().category,
            QueryExecutionError::Category::TypeContract);
  EXPECT_NE(invalid.error().message.find("replace"), std::string::npos);
  EXPECT_EQ(h.arena.linked(owner, h.dim("d.keep"), DimVector::POS), value);
  EXPECT_EQ(h.arena.linked(value, h.dim("d.keep"), DimVector::NEG), owner);
  EXPECT_EQ(h.arena.asInt64(value), 12);
  EXPECT_EQ(h.vm.choiceDepth(), 0U);
  EXPECT_FALSE(h.vm.hasActiveFrames());
  EXPECT_FALSE(h.vm.hasActiveGuards());
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);

  const auto valid =
      h.run("def weave replace($owner: Cell { +d.keep: Int }): Cell := "
            "$owner{+d.keep: 24}; replace($owner)",
            {{"owner", {owner}}});
  ASSERT_TRUE(valid.compilation.success) << valid.compilation.errorMessage;
  ASSERT_TRUE(valid.executed) << valid.error;
  EXPECT_EQ(valid.values, (std::vector<CellRef>{owner}));
  const auto replacement =
      h.arena.linked(owner, h.dim("d.keep"), DimVector::POS);
  ASSERT_TRUE(replacement);
  EXPECT_NE(*replacement, value);
  EXPECT_EQ(h.arena.asInt64(*replacement), 24);
  EXPECT_FALSE(h.arena.linked(value, h.dim("d.keep"), DimVector::NEG));
  EXPECT_FALSE(h.vm.hasActiveGuards());
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);
}

TEST(VQLShapeProjectionVMTest,
     UnsupportedAssignmentFormsRefuseBeforeGraphEffects) {
  Harness h;
  const CellRef owner = h.cell("owner");
  const CellRef value = h.cell("existing");
  h.link(owner, "d.keep", DimVector::POS, value);
  const auto keep  = h.dim("d.keep");
  const auto other = h.dim("d.other");

  for (const std::string_view source : {
           "$owners{+d.keep: $target}",
           "$owners{+d.keep: ./d.keep}",
           "$owners{+d.keep: string(\"value\")}",
           "$owners{+d.keep: {+d.nested}}",
           "$owners{~d.keep: \"value\"}",
           "$owners{!d.keep: \"value\"}",
       }) {
    SCOPED_TRACE(source);
    const auto compiled =
        h.compile(source, {{"owners", {owner}}, {"target", {value}}});
    EXPECT_FALSE(compiled.success);
    EXPECT_FALSE(h.vm.hasActiveGuards());
    EXPECT_EQ(h.arena.linked(owner, keep, DimVector::POS), value);
    EXPECT_EQ(h.arena.linked(value, keep, DimVector::NEG), owner);
    EXPECT_FALSE(h.arena.linked(owner, other, DimVector::POS));
  }
}

TEST(VQLShapeProjectionVMTest,
     LinkBudgetFailureLeavesOwnerAndReciprocalsUntouched) {
  Harness h;
  const CellRef owner = h.cell("owner");
  const CellRef keep  = h.cell("keep");
  const CellRef drop  = h.cell("drop");
  h.link(owner, "d.keep", DimVector::POS, keep);
  h.link(owner, "d.drop", DimVector::POS, drop);
  h.vm.setProjectionLimits({.maxLinksPerOwner = 0});

  QueryExecutor executor(h.coordinator, h.vm, {}, {{"owners", {owner}}});
  const auto result = executor.execute("$owners{+d.keep}");
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().category, QueryExecutionError::Category::Budget);
  EXPECT_NE(result.error().message.find("limit"), std::string::npos);
  EXPECT_EQ(h.arena.linked(owner, h.dim("d.keep"), DimVector::POS), keep);
  EXPECT_EQ(h.arena.linked(keep, h.dim("d.keep"), DimVector::NEG), owner);
  EXPECT_EQ(h.arena.linked(owner, h.dim("d.drop"), DimVector::POS), drop);
  EXPECT_EQ(h.arena.linked(drop, h.dim("d.drop"), DimVector::NEG), owner);
  EXPECT_FALSE(h.vm.hasActiveGuards());
  EXPECT_EQ(h.vm.choiceDepth(), 0U);
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);
}

TEST(VQLShapeProjectionVMTest, ExportedProjectionExecutesInFreshVM) {
  TemporaryDirectory temporary;
  auto scroll = std::make_shared<UserPermascroll>(
      UserPermascroll::Config{.storageDir = temporary.path / "scroll"});
  {
    zigzag::ArenaManifold sourceArena;
    zigzag::vortex::VortexCore sourceCore(sourceArena);
    MultiStoreCoordinator sourceCoordinator(sourceCore);
    zigzag::vortex::VortexVM sourceVm(sourceCore);
    VQLCompiler compiler(sourceCoordinator, sourceVm);
    const auto compiled = compiler.compile("##{+d.exported: 9007199254740993}");
    ASSERT_TRUE(compiled.success) << compiled.errorMessage;
    ASSERT_TRUE(compiled.entryOpcode);
    std::size_t maskOps = 0;
    std::size_t boxOps  = 0;
    std::size_t groups  = 0;
    for (const CellRef opcode : compiled.generatedOpcodes) {
      const auto kind = sourceVm.getOpcodeKind(opcode);
      maskOps += kind == OpcodeKind::VqlMaskLinks;
      boxOps += kind == OpcodeKind::VqlBoxScalar;
      groups += kind == OpcodeKind::BeginMutationGroup ||
                kind == OpcodeKind::EndMutationGroup;
    }
    EXPECT_GT(maskOps, 0U);
    EXPECT_GT(boxOps, 0U);
    EXPECT_GE(groups, 2U);

    Store artifact(scroll);
    compiler.exportToStore(artifact);
    artifact.save((temporary.path / "projection").string());
  }

  Store reloaded(scroll);
  ASSERT_NO_THROW(reloaded.load((temporary.path / "projection").string()));
  ASSERT_TRUE(reloaded.structureHead());
  const auto manifold = reloaded.rebuildManifold(*reloaded.structureHead());
  zigzag::ArenaManifold arena(&manifold, reloaded);
  zigzag::vortex::VortexCore core(arena);
  MultiStoreCoordinator coordinator(core);
  zigzag::vortex::VortexVM vm(core);
  const auto entry =
      arena.linked(core.home(), core.dims().spin, DimVector::POS);
  ASSERT_TRUE(entry);
  const CellRef cursor = vm.spawnCursor(*entry, "shape-projection-reload");
  const auto execution = vm.run(cursor, 100'000);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  const auto projected = arena.linked(
      core.home(), coordinator.resolveDimension("d.exported"), DimVector::POS);
  ASSERT_TRUE(projected);
  EXPECT_EQ(arena.asInt64(*projected), INT64_C(9007199254740993));
  vm.setCursorOpcode(cursor, std::nullopt);
  EXPECT_FALSE(vm.hasActiveGuards());
  EXPECT_FALSE(vm.hasActiveFrames());
  EXPECT_EQ(vm.choiceDepth(), 0U);
  EXPECT_EQ(arena.outstandingMarks(), 0U);
}

TEST(VQLShapeProjectionVMTest, HostAndMeshSourceRoutesExecuteProjectionGraphs) {
  constexpr std::string_view source =
      "let $owner := ##{+d.status: 7} return $owner/+d.status[1]";
  zigzag::vortex::VortexHost host;
  const auto result = host.executeVQLChecked(source);
  ASSERT_TRUE(result) << result.error().message;
  ASSERT_EQ(result->size(), 1U);
  EXPECT_EQ(host.arena().asInt64(result->front()), 7);

  namespace mesh = zigzag::vortex::mesh;
  const auto evaluated =
      mesh::evaluate(mesh::Frontend::Vquery, source,
                     mesh::EvaluationBudget{.maxCycles      = 100'000,
                                            .maxCells       = 100'000,
                                            .maxSolutions   = 16,
                                            .maxOutputBytes = 1024 * 1024});
  ASSERT_TRUE(evaluated) << evaluated.error();
  auto &arena = evaluated->context->arena;
  std::optional<zigzag::DimRef> items;
  std::optional<zigzag::DimRef> value;
  for (const auto &cell : arena.cells()) {
    if (arena.textOf(cell.birthOp) == "d.mesh.items") items = cell.birthOp;
    if (arena.textOf(cell.birthOp) == "d.mesh.value") value = cell.birthOp;
  }
  ASSERT_TRUE(items);
  ASSERT_TRUE(value);
  const auto record = arena.linked(evaluated->root, *items, DimVector::POS);
  ASSERT_TRUE(record);
  const auto output = arena.linked(*record, *value, DimVector::POS);
  ASSERT_TRUE(output);
  EXPECT_EQ(arena.asInt64(*output), 7);
}

} // namespace
} // namespace xanadu::vql
