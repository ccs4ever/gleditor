/**
 * @file vql_ordered_projection_test.cpp
 * @brief Ordered VQL projections execute at their path position.
 */
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
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
#include "common/xanadu/vql/parser.hpp"
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

  CellRef cell(const std::string_view value) { return arena.makeCell(value); }

  void link(const CellRef from, const std::string_view dimension,
            const CellRef to, const DimVector direction = DimVector::POS) {
    zigzag::expectWritten(arena.link(from, dim(dimension), direction, to));
  }

  std::expected<std::vector<CellRef>, QueryExecutionError>
  execute(const std::string_view source, ContractBindings bindings = {}) {
    QueryExecutor executor(coordinator, vm, {}, std::move(bindings));
    return executor.execute(source);
  }

  CompilationResult compile(const std::string_view source) {
    VQLCompiler compiler(coordinator, vm);
    return compiler.compile(source);
  }

  std::optional<CellRef> field(const CellRef owner,
                               const std::string_view dimension,
                               const DimVector direction = DimVector::POS) {
    return arena.linked(owner, dim(dimension), direction);
  }
};

struct TemporaryDirectory {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("vql_ordered_projection_" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));

  TemporaryDirectory() { std::filesystem::create_directories(path); }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

TEST(VQLOrderedProjectionTest, ProjectionRunsBeforeTheFollowingRead) {
  Harness h;
  const CellRef first     = h.cell("first");
  const CellRef second    = h.cell("second");
  const CellRef oldFirst  = h.arena.makeScalarCell(INT64_C(1));
  const CellRef oldSecond = h.arena.makeScalarCell(INT64_C(2));
  h.link(first, "d.score", oldFirst);
  h.link(first, "d.discard", h.cell("drop first"));
  h.link(second, "d.score", oldSecond);
  h.link(second, "d.discard", h.cell("drop second"));

  const auto result =
      h.execute("$owners{+d.score: 7}/+d.score", {{"owners", {first, second}}});
  ASSERT_TRUE(result) << result.error().message;
  ASSERT_EQ(result->size(), 2U);
  EXPECT_EQ(h.arena.asInt64((*result)[0]), INT64_C(7));
  EXPECT_EQ(h.arena.asInt64((*result)[1]), INT64_C(7));
  EXPECT_FALSE(h.field(first, "d.discard"));
  EXPECT_FALSE(h.field(second, "d.discard"));
  EXPECT_NE((*result)[0], (*result)[1]);
}

TEST(VQLOrderedProjectionTest,
     AnchorAndPostStepProjectionsPreserveStreamOrder) {
  Harness h;
  const CellRef first        = h.cell("first");
  const CellRef second       = h.cell("second");
  const CellRef firstRecord  = h.cell("first record");
  const CellRef secondRecord = h.cell("second record");
  h.link(first, "d.record", firstRecord);
  h.link(second, "d.record", secondRecord);
  h.link(firstRecord, "d.old", h.arena.makeScalarCell(INT64_C(11)));
  h.link(secondRecord, "d.old", h.arena.makeScalarCell(INT64_C(22)));

  const auto anchor = h.execute("$owners{*, +d.ready: 1}/+d.record",
                                {{"owners", {second, first, second}}});
  ASSERT_TRUE(anchor) << anchor.error().message;
  EXPECT_EQ(*anchor,
            (std::vector<CellRef>{secondRecord, firstRecord, secondRecord}));
  ASSERT_TRUE(h.field(first, "d.ready"));
  ASSERT_TRUE(h.field(second, "d.ready"));

  const auto afterStep =
      h.execute("$owners/+d.record{*, +d.status: 7}/+d.status",
                {{"owners", {first, second}}});
  ASSERT_TRUE(afterStep) << afterStep.error().message;
  ASSERT_EQ(afterStep->size(), 2U);
  EXPECT_EQ(h.arena.asInt64((*afterStep)[0]), INT64_C(7));
  EXPECT_EQ(h.arena.asInt64((*afterStep)[1]), INT64_C(7));
  EXPECT_TRUE(h.field(firstRecord, "d.old"));
  EXPECT_TRUE(h.field(secondRecord, "d.old"));
}

TEST(VQLOrderedProjectionTest,
     PrecedingPredicatesAndClampSelectTheProjectedOwners) {
  Harness h;
  const CellRef document = h.cell("document");
  const CellRef first    = h.cell("first");
  const CellRef skip     = h.cell("skip");
  const CellRef last     = h.cell("last");
  h.link(document, "d.people", first);
  h.link(first, "d.people", skip);
  h.link(skip, "d.people", last);

  const auto result = h.execute(
      "$document/+d.people[. != \"skip\"][-1]{+d.score: 7}/+d.score[1]",
      {{"document", {document}}});
  ASSERT_TRUE(result) << result.error().message;
  ASSERT_EQ(result->size(), 1U);
  EXPECT_EQ(h.arena.asInt64(result->front()), INT64_C(7));
  EXPECT_FALSE(h.field(first, "d.score"));
  EXPECT_FALSE(h.field(skip, "d.score"));
  EXPECT_TRUE(h.field(last, "d.score"));
}

TEST(VQLOrderedProjectionTest, KeepYieldProjectsTheParentStream) {
  Harness h;
  const CellRef owner  = h.cell("owner");
  const CellRef record = h.cell("record");
  h.link(owner, "d.record", record);

  const auto result = h.execute(
      "$owners/+d.record!keep{+d.score: 7}/+d.score[1]", {{"owners", {owner}}});
  ASSERT_TRUE(result) << result.error().message;
  ASSERT_EQ(result->size(), 1U);
  EXPECT_EQ(h.arena.asInt64(result->front()), INT64_C(7));
  EXPECT_TRUE(h.field(owner, "d.score"));
  EXPECT_FALSE(h.field(record, "d.score"));
}

TEST(VQLOrderedProjectionTest,
     ReadOnlyProjectionTargetUsesTheOwnerReachedByThePath) {
  Harness h;
  const CellRef document    = h.cell("document");
  const CellRef first       = h.cell("first record");
  const CellRef second      = h.cell("second record");
  const CellRef firstValue  = h.arena.makeScalarCell(INT64_C(9007199254740993));
  const CellRef secondValue = h.arena.makeScalarCell(INT64_C(9007199254740995));
  const CellRef backlink    = h.cell("backlink");
  h.link(document, "d.record", first);
  h.link(first, "d.record", second);
  h.link(first, "d.old", firstValue);
  h.link(second, "d.old", secondValue);
  h.link(firstValue, "d.annotation", backlink);

  const auto result =
      h.execute("$documents/+d.record{+d.copy: ./d.old}/+d.copy",
                {{"documents", {document}}});
  ASSERT_TRUE(result) << result.error().message;
  ASSERT_EQ(result->size(), 2U);
  for (const auto [owner, target, output] :
       {std::tuple{first, firstValue, (*result)[0]},
        std::tuple{second, secondValue, (*result)[1]}}) {
    const auto copy = h.field(owner, "d.copy");
    ASSERT_TRUE(copy);
    EXPECT_EQ(output, *copy);
    EXPECT_EQ(h.arena.cloneMaster(*copy, h.dim("d.clone")), target);
    EXPECT_FALSE(h.field(owner, "d.old"));
  }
  EXPECT_EQ(h.field(firstValue, "d.annotation"), backlink);
  EXPECT_EQ(h.field(backlink, "d.annotation", DimVector::NEG), firstValue);

  const CellRef aliasOwner  = h.cell("alias owner");
  const CellRef aliasRecord = h.cell("alias record");
  const CellRef aliasTarget = h.arena.makeScalarCell(INT64_C(47));
  h.link(aliasOwner, "d.record", aliasRecord);
  const auto aliasResult =
      h.execute("$documents/+d.record{+d.alias_copy: $target}/+d.alias_copy[1]",
                {{"documents", {aliasOwner}}, {"target", {aliasTarget}}});
  ASSERT_TRUE(aliasResult) << aliasResult.error().message;
  ASSERT_EQ(aliasResult->size(), 1U);
  const auto aliasCopy = h.field(aliasRecord, "d.alias_copy");
  ASSERT_TRUE(aliasCopy);
  EXPECT_EQ(aliasResult->front(), *aliasCopy);
  EXPECT_EQ(h.arena.cloneMaster(*aliasCopy, h.dim("d.clone")), aliasTarget);
}

TEST(VQLOrderedProjectionTest,
     InvalidOrderedTargetRollsBackEarlierChangesForThatOwner) {
  Harness h;
  const CellRef owner    = h.cell("owner");
  const CellRef previous = h.arena.makeScalarCell(INT64_C(3));
  h.link(owner, "d.status", previous);

  const auto result =
      h.execute("$owners{*, +d.status: 9, +d.copy: ./d.missing}/+d.status[1]",
                {{"owners", {owner}}});
  ASSERT_FALSE(result);
  EXPECT_EQ(h.field(owner, "d.status"), previous);
  EXPECT_EQ(h.field(previous, "d.status", DimVector::NEG), owner);
  EXPECT_FALSE(h.field(owner, "d.copy"));
  EXPECT_FALSE(h.vm.hasActiveGuards());
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);
}

TEST(VQLOrderedProjectionTest, ParserKeepsLegacyTerminalShapeAndOrderedSteps) {
  Parser terminal("##/d.people{+d.score: 7}");
  const auto terminalQuery = terminal.parseQuery();
  ASSERT_TRUE(std::holds_alternative<PathExpression>(terminalQuery.expr));
  const auto &terminalPath = std::get<PathExpression>(terminalQuery.expr);
  ASSERT_TRUE(terminalPath.shapeProjection);
  ASSERT_EQ(terminalPath.steps.size(), 1U);
  EXPECT_FALSE(terminalPath.steps.front().projectionAfter);
  EXPECT_FALSE(terminalPath.anchorProjection);

  Parser ordered("##{+d.ready: 1}/d.people{+d.score: 7}/d.score");
  const auto orderedQuery = ordered.parseQuery();
  ASSERT_TRUE(std::holds_alternative<PathExpression>(orderedQuery.expr));
  const auto &path = std::get<PathExpression>(orderedQuery.expr);
  ASSERT_TRUE(path.anchorProjection);
  ASSERT_EQ(path.steps.size(), 2U);
  ASSERT_TRUE(path.steps.front().projectionAfter);
  EXPECT_FALSE(path.shapeProjection);
}

TEST(VQLOrderedProjectionTest,
     ConsecutiveProjectionsRemainExplicitlyUnsupported) {
  for (const auto source : {"$owner{*}{+d.score: 7}/+d.score",
                            "$owner{*}/+d.score{*}{!d.discard}"}) {
    Parser parser(source);
    EXPECT_THROW(parser.parseQuery(), ParseError) << source;
  }
}

TEST(VQLOrderedProjectionTest, ProjectionFunctorCapturesRefuseBeforeLowering) {
  Harness h;
  for (const auto source :
       {"let $f := /+d.foo{+d.score: 7}/+d.score return $f",
        "let $f := /+d.foo return $f{+d.score: 7}/+d.score"}) {
    const auto compiled = h.compile(source);
    EXPECT_FALSE(compiled.success) << source;
    EXPECT_FALSE(compiled.cursor) << source;
  }
}

TEST(VQLOrderedProjectionTest, HostAndMeshExecuteAnchorProjectionContinuation) {
  constexpr std::string_view source = "##{+d.status: 7}/+d.status[1]";
  zigzag::vortex::VortexHost host;
  const auto hostResult = host.executeVQLChecked(source);
  ASSERT_TRUE(hostResult) << hostResult.error().message;
  ASSERT_EQ(hostResult->size(), 1U);
  EXPECT_EQ(host.core().render(hostResult->front()),
            std::optional<zigzag::vortex::CellValue>{INT64_C(7)});

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
  auto &arena = meshResult->context->arena;
  std::optional<DimRef> items;
  std::optional<DimRef> value;
  for (const auto &cell : arena.cells()) {
    if (arena.textOf(cell.birthOp) == "d.mesh.items") items = cell.birthOp;
    if (arena.textOf(cell.birthOp) == "d.mesh.value") value = cell.birthOp;
  }
  ASSERT_TRUE(items);
  ASSERT_TRUE(value);
  const auto record = arena.linked(meshResult->root, *items, DimVector::POS);
  ASSERT_TRUE(record);
  const auto output = arena.linked(*record, *value, DimVector::POS);
  ASSERT_TRUE(output);
  EXPECT_EQ(meshResult->context->core.render(*output),
            std::optional<zigzag::vortex::CellValue>{INT64_C(7)});
}

TEST(VQLOrderedProjectionTest, LegacyTerminalProjectionASTStillCompiles) {
  Harness h;
  Parser parser("##{+d.status: 7}");
  auto query = parser.parseQuery();
  ASSERT_TRUE(std::holds_alternative<PathExpression>(query.expr));
  const auto &path = std::get<PathExpression>(query.expr);
  ASSERT_TRUE(path.shapeProjection);
  EXPECT_FALSE(path.anchorProjection);

  VQLCompiler compiler(h.coordinator, h.vm);
  const auto compiled = compiler.compile(query, {});
  EXPECT_TRUE(compiled.success) << compiled.errorMessage;
}

TEST(VQLOrderedProjectionTest,
     OrderedProjectionRunsInsideNamedRecursiveInvokeFrame) {
  Harness h;
  const CellRef first  = h.cell("first");
  const CellRef second = h.cell("second");
  const CellRef third  = h.cell("third");
  h.link(first, "d.next", second);
  h.link(second, "d.next", third);

  constexpr std::string_view source =
      "def pure walk($node: Cell): Cell := { "
      "$node{*, +d.score: 7}/+d.next[1]; "
      "if $node/+d.next[1] return walk($node/+d.next[1]) "
      "else return $node; }; walk($root)";
  CompilationOptions options;
  options.initialBindings = {{"root", {first}}};
  VQLCompiler compiler(h.coordinator, h.vm);
  const auto compiled = compiler.compile(source, options);
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  const bool hasNamedInvoke =
      std::ranges::any_of(compiled.generatedOpcodes, [&](const CellRef opcode) {
        return h.vm.getOpcodeKind(opcode) == OpcodeKind::InvokeFrame &&
               h.arena.textOf(opcode) == "#INVOKE_FRAME walk";
      });
  ASSERT_TRUE(hasNamedInvoke);
  ASSERT_TRUE(compiled.cursor);
  const auto execution = h.vm.run(*compiled.cursor, 1'000'000);
  h.vm.setCursorOpcode(*compiled.cursor, std::nullopt);
  ASSERT_TRUE(execution.success) << execution.errorMessage;

  for (const CellRef node : {first, second, third}) {
    const auto score = h.field(node, "d.score");
    ASSERT_TRUE(score);
    EXPECT_EQ(h.arena.asInt64(*score), std::optional<std::int64_t>{7});
  }
  ASSERT_FALSE(compiled.resultSlots.empty());
  EXPECT_EQ(h.vm.resultCells(compiled.resultSlots.front()),
            (std::vector<CellRef>{third}));
  EXPECT_FALSE(h.vm.hasActiveFrames());
  EXPECT_FALSE(h.vm.hasActiveGuards());
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);
}

TEST(VQLOrderedProjectionTest, ExportedGraphReloadsWithoutTheSourceAST) {
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
        "def pure project($n: Int): Int := { "
        "if $n > 0 return project($n + -1) else return "
        "##{+d.status: 9007199254740993}/+d.status[1]; }; project(3)");
    ASSERT_TRUE(compiled.success) << compiled.errorMessage;
    ASSERT_TRUE(compiled.entryOpcode);
    ASSERT_TRUE(std::ranges::any_of(
        compiled.generatedOpcodes, [&](const CellRef opcode) {
          return sourceVm.getOpcodeKind(opcode) == OpcodeKind::InvokeFrame &&
                 sourceArena.textOf(opcode) == "#INVOKE_FRAME project";
        }));
    Store artifact(scroll);
    compiler.exportToStore(artifact);
    artifact.save((temporary.path / "ordered-projection").string());
  }

  Store reloaded(scroll);
  ASSERT_NO_THROW(
      reloaded.load((temporary.path / "ordered-projection").string()));
  ASSERT_TRUE(reloaded.structureHead());
  const auto manifold = reloaded.rebuildManifold(*reloaded.structureHead());
  zigzag::ArenaManifold arena(&manifold, reloaded);
  zigzag::vortex::VortexCore core(arena);
  MultiStoreCoordinator coordinator(core);
  zigzag::vortex::VortexVM vm(core);
  const auto entry =
      arena.linked(core.home(), core.dims().spin, DimVector::POS);
  ASSERT_TRUE(entry);
  const CellRef cursor = vm.spawnCursor(*entry, "ordered-projection-reload");
  const auto execution = vm.run(cursor, 100'000);
  ASSERT_TRUE(execution.success) << execution.errorMessage;

  auto terminal = entry;
  while (terminal && vm.getOpcodeKind(*terminal) != OpcodeKind::Halt)
    terminal = arena.linked(*terminal, core.dims().spin, DimVector::POS);
  ASSERT_TRUE(terminal);
  const auto ports = core.inputsOf(*terminal);
  ASSERT_EQ(ports.size(), 1U);
  const auto results = vm.resultCells(ports.front());
  ASSERT_EQ(results.size(), 1U);
  EXPECT_EQ(
      core.render(results.front()),
      std::optional<zigzag::vortex::CellValue>{INT64_C(9007199254740993)});
  vm.setCursorOpcode(cursor, std::nullopt);
  EXPECT_FALSE(vm.hasActiveFrames());
  EXPECT_FALSE(vm.hasActiveGuards());
  EXPECT_EQ(arena.outstandingMarks(), 0U);
}

} // namespace
} // namespace xanadu::vql
