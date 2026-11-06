/**
 * @file vql_projection_targets_test.cpp
 * @brief Read-only aliases and paths as transactional projection targets.
 */
#include <gtest/gtest.h>

#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
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
            const CellRef target) {
    zigzag::expectWritten(arena.link(owner, dim(name), DimVector::POS, target));
  }

  std::expected<std::vector<CellRef>, QueryExecutionError>
  run(const std::string_view source, ContractBindings bindings = {}) {
    QueryExecutor executor(coordinator, vm, {}, std::move(bindings));
    return executor.execute(source);
  }

  CompilationResult compile(const std::string_view source,
                            ContractBindings bindings = {}) {
    CompilationOptions options;
    options.initialBindings = std::move(bindings);
    VQLCompiler compiler(coordinator, vm);
    return compiler.compile(source, options);
  }

  std::optional<CellRef> field(const CellRef owner, const std::string_view name,
                               const DimVector direction = DimVector::POS) {
    return arena.linked(owner, dim(name), direction);
  }

  std::vector<CellRef> cloneChildren(const CellRef master) {
    std::vector<CellRef> result;
    auto child = field(master, "d.clone", DimVector::POS);
    while (child && *child != master) {
      result.push_back(*child);
      child = field(*child, "d.clone", DimVector::POS);
    }
    return result;
  }
};

struct TemporaryDirectory {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("vql_projection_targets_" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));

  TemporaryDirectory() { std::filesystem::create_directories(path); }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

bool expectTypeContract(
    const std::expected<std::vector<CellRef>, QueryExecutionError> &result) {
  EXPECT_FALSE(result);
  if (result) return false;
  EXPECT_EQ(result.error().category,
            QueryExecutionError::Category::TypeContract);
  return true;
}

TEST(VQLProjectionTargetsTest, PathReadsOriginalOwnerBeforeDefaultMask) {
  Harness h;
  const CellRef owner    = h.cell("owner");
  const CellRef oldTag   = h.arena.makeScalarCell(INT64_C(9007199254740993));
  const CellRef dropped  = h.cell("dropped");
  const CellRef backlink = h.cell("target backlink");
  h.link(owner, "d.old_tag", oldTag);
  h.link(owner, "d.discard", dropped);
  h.link(oldTag, "d.annotation", backlink);

  const auto result =
      h.run("$owners{+d.tag: ./d.old_tag}", {{"owners", {owner}}});
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{owner}));

  const auto tag = h.field(owner, "d.tag");
  ASSERT_TRUE(tag);
  EXPECT_NE(*tag, oldTag);
  EXPECT_EQ(h.arena.cloneMaster(*tag, h.dim("d.clone")), oldTag);
  EXPECT_EQ(h.core.render(*tag), h.core.render(oldTag));
  EXPECT_EQ(h.core.render(oldTag), std::optional<zigzag::vortex::CellValue>{
                                       INT64_C(9007199254740993)});
  EXPECT_FALSE(h.field(owner, "d.old_tag"));
  EXPECT_FALSE(h.field(oldTag, "d.old_tag", DimVector::NEG));
  EXPECT_FALSE(h.field(owner, "d.discard"));
  EXPECT_FALSE(h.field(dropped, "d.discard", DimVector::NEG));
  EXPECT_EQ(h.field(oldTag, "d.annotation"), backlink);
  EXPECT_EQ(h.field(backlink, "d.annotation", DimVector::NEG), oldTag);
}

TEST(VQLProjectionTargetsTest,
     AllTargetsPrepareFromOriginalOwnerAndClonePerOwnerOccurrence) {
  Harness h;
  const CellRef first     = h.cell("first");
  const CellRef second    = h.cell("second");
  const CellRef targetA   = h.arena.makeScalarCell(INT64_C(9007199254740993));
  const CellRef targetB   = h.arena.makeScalarCell(0x1.0000000000001p53);
  const CellRef retainedA = h.cell("retained A");
  const CellRef retainedB = h.cell("retained B");
  const CellRef firstA    = h.cell("");
  const CellRef secondA   = h.cell("");
  const CellRef firstB    = h.cell("");
  const CellRef secondB   = h.cell("");
  h.link(targetA, "d.clone", firstA);
  h.link(firstA, "d.clone", secondA);
  h.link(targetB, "d.clone", firstB);
  h.link(firstB, "d.clone", secondB);
  h.link(first, "d.old_a", firstA);
  h.link(first, "d.old_b", firstB);
  h.link(second, "d.old_a", secondA);
  h.link(second, "d.old_b", secondB);
  h.link(targetA, "d.annotation", retainedA);
  h.link(targetB, "d.annotation", retainedB);

  const auto result =
      h.run("$owners{+d.copy_a: ./d.old_a, +d.copy_b: ./d.old_b}",
            {{"owners", {first, second}}});
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{first, second}));

  std::vector<CellRef> allA;
  std::vector<CellRef> allB;
  for (const CellRef owner : {first, second}) {
    const auto copyA = h.field(owner, "d.copy_a");
    const auto copyB = h.field(owner, "d.copy_b");
    ASSERT_TRUE(copyA);
    ASSERT_TRUE(copyB);
    EXPECT_NE(*copyA, targetA);
    EXPECT_NE(*copyB, targetB);
    EXPECT_EQ(h.arena.cloneMaster(*copyA, h.dim("d.clone")), targetA);
    EXPECT_EQ(h.arena.cloneMaster(*copyB, h.dim("d.clone")), targetB);
    EXPECT_EQ(h.core.render(*copyA), h.core.render(targetA));
    EXPECT_EQ(h.core.render(*copyB), h.core.render(targetB));
    EXPECT_FALSE(h.field(owner, "d.old_a"));
    EXPECT_FALSE(h.field(owner, "d.old_b"));
    allA.push_back(*copyA);
    allB.push_back(*copyB);
  }
  EXPECT_NE(allA[0], allA[1]);
  EXPECT_NE(allB[0], allB[1]);
  EXPECT_EQ(h.core.render(targetA), std::optional<zigzag::vortex::CellValue>{
                                        INT64_C(9007199254740993)});
  const auto renderedB = h.core.render(targetB);
  ASSERT_TRUE(renderedB);
  const auto targetBDouble = std::get_if<double>(&*renderedB);
  ASSERT_NE(targetBDouble, nullptr);
  EXPECT_EQ(std::bit_cast<std::uint64_t>(*targetBDouble),
            std::bit_cast<std::uint64_t>(0x1.0000000000001p53));
  EXPECT_EQ(h.field(targetA, "d.annotation"), retainedA);
  EXPECT_EQ(h.field(targetB, "d.annotation"), retainedB);
  EXPECT_EQ(h.field(retainedA, "d.annotation", DimVector::NEG), targetA);
  EXPECT_EQ(h.field(retainedB, "d.annotation", DimVector::NEG), targetB);

  const CellRef aliasFirst    = h.cell("alias first");
  const CellRef aliasSecond   = h.cell("alias second");
  const CellRef aliasTarget   = h.arena.makeScalarCell(0x1.0000000000001p53);
  const CellRef aliasBacklink = h.cell("alias backlink");
  h.link(aliasTarget, "d.annotation", aliasBacklink);
  const auto aliasResult =
      h.run("$owners{+d.copy: $target}",
            {{"owners", {aliasFirst, aliasSecond}}, {"target", {aliasTarget}}});
  ASSERT_TRUE(aliasResult) << aliasResult.error().message;
  EXPECT_EQ(*aliasResult, (std::vector<CellRef>{aliasFirst, aliasSecond}));
  const auto aliasCopyA = h.field(aliasFirst, "d.copy");
  const auto aliasCopyB = h.field(aliasSecond, "d.copy");
  ASSERT_TRUE(aliasCopyA);
  ASSERT_TRUE(aliasCopyB);
  EXPECT_NE(*aliasCopyA, *aliasCopyB);
  EXPECT_EQ(h.arena.cloneMaster(*aliasCopyA, h.dim("d.clone")), aliasTarget);
  EXPECT_EQ(h.arena.cloneMaster(*aliasCopyB, h.dim("d.clone")), aliasTarget);
  const auto aliasRendered = h.core.render(*aliasCopyA);
  ASSERT_TRUE(aliasRendered);
  const auto aliasDouble = std::get_if<double>(&*aliasRendered);
  ASSERT_NE(aliasDouble, nullptr);
  EXPECT_EQ(std::bit_cast<std::uint64_t>(*aliasDouble),
            std::bit_cast<std::uint64_t>(0x1.0000000000001p53));
  EXPECT_EQ(h.field(aliasTarget, "d.annotation"), aliasBacklink);
  EXPECT_EQ(h.field(aliasBacklink, "d.annotation", DimVector::NEG),
            aliasTarget);

  const CellRef duplicateOwner  = h.cell("duplicate owner");
  const CellRef duplicateTarget = h.arena.makeScalarCell(INT64_C(44));
  h.link(duplicateOwner, "d.old_tag", duplicateTarget);
  const auto duplicate = h.run("$owners{+d.tag: $target}",
                               {{"owners", {duplicateOwner, duplicateOwner}},
                                {"target", {duplicateTarget}}});
  ASSERT_TRUE(duplicate) << duplicate.error().message;
  EXPECT_EQ(*duplicate, (std::vector<CellRef>{duplicateOwner, duplicateOwner}));
  const auto resultingTag = h.field(duplicateOwner, "d.tag");
  ASSERT_TRUE(resultingTag);
  const auto duplicateClones = h.cloneChildren(duplicateTarget);
  ASSERT_EQ(duplicateClones.size(), 2U);
  EXPECT_NE(duplicateClones[0], duplicateClones[1]);
  EXPECT_EQ(h.arena.cloneMaster(*resultingTag, h.dim("d.clone")),
            duplicateTarget);
  EXPECT_FALSE(h.field(duplicateOwner, "d.old_tag"));
}

TEST(VQLProjectionTargetsTest,
     InvalidTargetCardinalityFailsBeforeMaskOrEarlierLiteralAssignment) {
  for (const std::size_t targetCount : {0U, 2U}) {
    Harness h;
    const CellRef owner  = h.cell("owner");
    const CellRef old    = h.cell("old");
    const CellRef oldTag = h.cell("old tag");
    std::vector<CellRef> targets;
    for (std::size_t index = 0; index < targetCount; ++index)
      targets.push_back(h.cell("target"));
    h.link(owner, "d.keep", old);
    h.link(owner, "d.old_tag", oldTag);

    const auto result = h.run("$owners{+d.new: 5, +d.copy: $target}",
                              {{"owners", {owner}}, {"target", targets}});
    ASSERT_TRUE(expectTypeContract(result));
    EXPECT_EQ(h.field(owner, "d.keep"), old);
    EXPECT_EQ(h.field(old, "d.keep", DimVector::NEG), owner);
    EXPECT_EQ(h.field(owner, "d.old_tag"), oldTag);
    EXPECT_EQ(h.field(oldTag, "d.old_tag", DimVector::NEG), owner);
    EXPECT_FALSE(h.field(owner, "d.new"));
    EXPECT_FALSE(h.field(owner, "d.copy"));
    EXPECT_FALSE(h.vm.hasActiveGuards());
    EXPECT_FALSE(h.vm.hasActiveFrames());
    EXPECT_EQ(h.vm.choiceDepth(), 0U);
    EXPECT_EQ(h.arena.outstandingMarks(), 0U);
  }

  for (const bool multiple : {false, true}) {
    Harness h;
    const CellRef owner  = h.cell("owner");
    const CellRef old    = h.cell("old");
    const CellRef target = h.cell("target");
    h.link(owner, "d.keep", old);
    if (multiple) {
      h.link(owner, "d.targets", target);
      const CellRef secondTarget = h.cell("second target");
      h.link(target, "d.targets", secondTarget);
    }

    const auto result = h.run("$owners{+d.new: 5, +d.copy: ./d.targets}",
                              {{"owners", {owner}}});
    ASSERT_TRUE(expectTypeContract(result));
    EXPECT_EQ(h.field(owner, "d.keep"), old);
    EXPECT_EQ(h.field(old, "d.keep", DimVector::NEG), owner);
    if (multiple) {
      EXPECT_EQ(h.field(owner, "d.targets"), target);
      EXPECT_EQ(h.field(target, "d.targets", DimVector::NEG), owner);
    }
    EXPECT_FALSE(h.field(owner, "d.new"));
    EXPECT_FALSE(h.field(owner, "d.copy"));
    EXPECT_FALSE(h.vm.hasActiveGuards());
    EXPECT_EQ(h.arena.outstandingMarks(), 0U);
  }
}

TEST(VQLProjectionTargetsTest,
     EachDuplicateOwnerOccurrenceReadsItsCurrentGraphAndRollsBackFailedGroup) {
  Harness h;
  const CellRef owner  = h.cell("owner");
  const CellRef source = h.arena.makeScalarCell(INT64_C(31));
  h.link(owner, "d.old_tag", source);

  const auto result =
      h.run("$owners{+d.tag: ./d.old_tag}", {{"owners", {owner, owner}}});
  ASSERT_TRUE(expectTypeContract(result));
  const auto tag = h.field(owner, "d.tag");
  ASSERT_TRUE(tag);
  EXPECT_EQ(h.arena.cloneMaster(*tag, h.dim("d.clone")), source);
  EXPECT_FALSE(h.field(owner, "d.old_tag"));
  EXPECT_FALSE(h.field(source, "d.old_tag", DimVector::NEG));
  EXPECT_EQ(h.core.render(*tag), h.core.render(source));
  EXPECT_EQ(h.cloneChildren(source).size(), 1U);
  EXPECT_FALSE(h.vm.hasActiveGuards());
  EXPECT_FALSE(h.vm.hasActiveFrames());
  EXPECT_EQ(h.vm.choiceDepth(), 0U);
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);
}

TEST(VQLProjectionTargetsTest,
     TypedCellGuardRollbackRestoresSourceAndAcceptsTypedCloneReplacement) {
  Harness h;
  const CellRef owner   = h.cell("owner");
  const CellRef value   = h.arena.makeScalarCell(INT64_C(12));
  const CellRef invalid = h.cell("not an integer");
  const CellRef valid   = h.arena.makeScalarCell(INT64_C(24));
  h.link(owner, "d.keep", value);
  h.link(owner, "d.bad", invalid);
  h.link(owner, "d.good", valid);

  const auto rejected =
      h.run("def weave replace($owner: Cell { +d.keep: Int }): Cell := "
            "$owner{+d.keep: ./d.bad}; replace($owner)",
            {{"owner", {owner}}});
  ASSERT_TRUE(expectTypeContract(rejected));
  EXPECT_NE(rejected.error().message.find("replace"), std::string::npos);
  EXPECT_EQ(h.field(owner, "d.keep"), value);
  EXPECT_EQ(h.field(value, "d.keep", DimVector::NEG), owner);
  EXPECT_EQ(h.field(owner, "d.bad"), invalid);
  EXPECT_EQ(h.field(invalid, "d.bad", DimVector::NEG), owner);
  EXPECT_TRUE(h.cloneChildren(invalid).empty());
  EXPECT_FALSE(h.vm.hasActiveGuards());
  EXPECT_FALSE(h.vm.hasActiveFrames());
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);

  const auto accepted =
      h.run("def weave replace($owner: Cell { +d.keep: Int }): Cell := "
            "$owner{+d.keep: ./d.good}; replace($owner)",
            {{"owner", {owner}}});
  ASSERT_TRUE(accepted) << accepted.error().message;
  EXPECT_EQ(*accepted, (std::vector<CellRef>{owner}));
  const auto replacement = h.field(owner, "d.keep");
  ASSERT_TRUE(replacement);
  EXPECT_NE(*replacement, valid);
  EXPECT_EQ(h.arena.cloneMaster(*replacement, h.dim("d.clone")), valid);
  EXPECT_EQ(h.core.render(*replacement), h.core.render(valid));
  EXPECT_FALSE(h.field(owner, "d.good"));
  EXPECT_FALSE(h.vm.hasActiveGuards());
  EXPECT_FALSE(h.vm.hasActiveFrames());
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);
}

TEST(VQLProjectionTargetsTest,
     EffectfulPathsCallsAndBinaryTargetsRefuseBeforeGraphEffects) {
  Harness h;
  const CellRef owner = h.cell("owner");
  const CellRef old   = h.cell("old");
  h.link(owner, "d.keep", old);
  for (const std::string_view source : {
           "$owners{+d.copy: string(\"value\")}",
           "$owners{+d.copy: 1 + 2}",
           "$owners{+d.copy: ./d.keep/value(\"changed\")}",
       }) {
    SCOPED_TRACE(source);
    const auto compiled = h.compile(source, {{"owners", {owner}}});
    EXPECT_FALSE(compiled.success);
    EXPECT_EQ(h.field(owner, "d.keep"), old);
    EXPECT_EQ(h.field(old, "d.keep", DimVector::NEG), owner);
    EXPECT_FALSE(h.field(owner, "d.copy"));
    EXPECT_FALSE(h.vm.hasActiveGuards());
    EXPECT_EQ(h.arena.outstandingMarks(), 0U);
  }
}

TEST(VQLProjectionTargetsTest, HostMeshAndFreshVMExportExecutePathTargets) {
  constexpr std::string_view source =
      "let $owner := ##{+d.old_tag: 41} "
      "let $projected := $owner{+d.tag: ./d.old_tag} "
      "return $projected/+d.tag[1]";
  zigzag::vortex::VortexHost host;
  const auto hostResult = host.executeVQLChecked(source);
  ASSERT_TRUE(hostResult) << hostResult.error().message;
  ASSERT_EQ(hostResult->size(), 1U);
  EXPECT_EQ(host.core().render(hostResult->front()),
            std::optional<zigzag::vortex::CellValue>{INT64_C(41)});

  namespace mesh = zigzag::vortex::mesh;
  constexpr mesh::EvaluationBudget budget{
      .maxCycles      = 100'000,
      .maxCells       = 100'000,
      .maxSolutions   = 16,
      .maxOutputBytes = 1024 * 1024,
  };
  const auto meshResult =
      mesh::evaluate(mesh::Frontend::Vquery, source, budget);
  ASSERT_TRUE(meshResult) << meshResult.error();
  auto &meshArena = meshResult->context->arena;
  std::optional<CellRef> items;
  std::optional<CellRef> value;
  for (const auto &cell : meshArena.cells()) {
    if (meshArena.textOf(cell.birthOp) == "d.mesh.items") items = cell.birthOp;
    if (meshArena.textOf(cell.birthOp) == "d.mesh.value") value = cell.birthOp;
  }
  ASSERT_TRUE(items);
  ASSERT_TRUE(value);
  const auto record =
      meshArena.linked(meshResult->root, *items, DimVector::POS);
  ASSERT_TRUE(record);
  const auto output = meshArena.linked(*record, *value, DimVector::POS);
  ASSERT_TRUE(output);
  EXPECT_EQ(meshResult->context->core.render(*output),
            std::optional<zigzag::vortex::CellValue>{INT64_C(41)});

  TemporaryDirectory temporary;
  auto scroll = std::make_shared<UserPermascroll>(
      UserPermascroll::Config{.storageDir = temporary.path / "scroll"});
  {
    zigzag::ArenaManifold sourceArena;
    zigzag::vortex::VortexCore sourceCore(sourceArena);
    MultiStoreCoordinator sourceCoordinator(sourceCore);
    zigzag::vortex::VortexVM sourceVm(sourceCore);
    VQLCompiler compiler(sourceCoordinator, sourceVm);
    const auto compiled = compiler.compile(
        "let $owner := ##{+d.old_tag: 9007199254740993} return "
        "$owner{+d.tag: ./d.old_tag}");
    ASSERT_TRUE(compiled.success) << compiled.errorMessage;
    ASSERT_TRUE(compiled.entryOpcode);
    std::size_t masks  = 0;
    std::size_t groups = 0;
    for (const CellRef opcode : compiled.generatedOpcodes) {
      const auto kind = sourceVm.getOpcodeKind(opcode);
      masks += kind == OpcodeKind::VqlMaskLinks;
      groups += kind == OpcodeKind::BeginMutationGroup ||
                kind == OpcodeKind::EndMutationGroup;
    }
    EXPECT_GT(masks, 0U);
    EXPECT_GE(groups, 2U);
    Store artifact(scroll);
    compiler.exportToStore(artifact);
    artifact.save((temporary.path / "projection-path").string());
  }

  Store reloaded(scroll);
  ASSERT_NO_THROW(reloaded.load((temporary.path / "projection-path").string()));
  ASSERT_TRUE(reloaded.structureHead());
  const auto manifold = reloaded.rebuildManifold(*reloaded.structureHead());
  zigzag::ArenaManifold arena(&manifold, reloaded);
  zigzag::vortex::VortexCore core(arena);
  MultiStoreCoordinator coordinator(core);
  zigzag::vortex::VortexVM vm(core);
  const auto entry =
      arena.linked(core.home(), core.dims().spin, DimVector::POS);
  ASSERT_TRUE(entry);
  const CellRef cursor = vm.spawnCursor(*entry, "projection-path-reload");
  const auto execution = vm.run(cursor, 100'000);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  // The exported query's returned owner is visible through the terminal slot;
  // validate the projected clone and exact scalar through the home result.
  auto terminal = entry;
  while (terminal && vm.getOpcodeKind(*terminal) != OpcodeKind::Halt)
    terminal = arena.linked(*terminal, core.dims().spin, DimVector::POS);
  ASSERT_TRUE(terminal);
  const auto resultPorts = core.inputsOf(*terminal);
  ASSERT_EQ(resultPorts.size(), 1U);
  const auto results = vm.resultCells(resultPorts.front());
  ASSERT_EQ(results.size(), 1U);
  const CellRef resultOwner = results.front();
  const auto projected      = arena.linked(
      resultOwner, coordinator.resolveDimension("d.tag"), DimVector::POS);
  ASSERT_TRUE(projected);
  const auto master =
      arena.cloneMaster(*projected, coordinator.resolveDimension("d.clone"));
  ASSERT_TRUE(master);
  EXPECT_EQ(arena.asInt64(*master), INT64_C(9007199254740993));
  EXPECT_FALSE(arena.linked(
      resultOwner, coordinator.resolveDimension("d.old_tag"), DimVector::POS));
  vm.setCursorOpcode(cursor, std::nullopt);
  EXPECT_FALSE(vm.hasActiveGuards());
  EXPECT_FALSE(vm.hasActiveFrames());
  EXPECT_EQ(vm.choiceDepth(), 0U);
  EXPECT_EQ(arena.outstandingMarks(), 0U);
}

} // namespace
} // namespace xanadu::vql
