/**
 * @file vql_read_modifiers_functor_frames_test.cpp
 * @brief Bidirectional and clone-master modifiers in recursive VQL functors.
 */
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
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

  CompilationResult compile(const std::string_view source,
                            CompilationOptions options = {}) {
    VQLCompiler compiler(coordinator, vm);
    return compiler.compile(source, options);
  }

  std::expected<std::vector<CellRef>, QueryExecutionError>
  execute(const std::string_view source, ContractBindings bindings = {}) {
    QueryExecutor executor(coordinator, vm, {}, std::move(bindings));
    return executor.execute(source);
  }
};

struct TemporaryDirectory {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("vql_read_modifiers_functor_frames_" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));

  TemporaryDirectory() { std::filesystem::create_directories(path); }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

constexpr std::string_view recursiveDefinition =
    "def pure walk($nodes: Stream<Cell>, $depth: Int, $route): "
    "Stream<Cell> := { if $depth > 0 return walk($nodes/$route, "
    "$depth + -1, $route) else return $nodes; }; ";

std::vector<CellRef> namedInvokeDescriptors(
    const CompilationResult &compiled, const zigzag::vortex::VortexVM &vm,
    const zigzag::vortex::VortexCore &core, const std::string_view function) {
  std::vector<CellRef> result;
  const std::string marker = "#INVOKE_FRAME " + std::string(function);
  for (const CellRef opcode : compiled.generatedOpcodes) {
    if (vm.getOpcodeKind(opcode) != OpcodeKind::InvokeFrame ||
        core.arena().textOf(opcode).find(marker) == std::string::npos)
      continue;
    const auto inputs = core.inputsOf(opcode);
    if (!inputs.empty()) result.push_back(inputs.front());
  }
  return result;
}

std::set<CellRef> uniqueNamedInvokeDescriptors(
    const CompilationResult &compiled, const zigzag::vortex::VortexVM &vm,
    const zigzag::vortex::VortexCore &core, const std::string_view function) {
  const auto descriptors = namedInvokeDescriptors(compiled, vm, core, function);
  return {descriptors.begin(), descriptors.end()};
}

TEST(VQLReadModifiersFunctorFrameTest,
     BidirectionalSelfAndMutualFramesKeepPositiveThenNegativeRankOrder) {
  Harness h;
  const CellRef predecessor = h.cell("predecessor");
  const CellRef center      = h.cell("center");
  const CellRef successor   = h.cell("successor");
  h.link(predecessor, "d.items", center);
  h.link(center, "d.items", successor);
  const std::vector<CellRef> repeatedCenter{center, center};

  constexpr std::string_view selfSource =
      "def pure walk($nodes: Stream<Cell>, $depth: Int, $route): "
      "Stream<Cell> := { if $depth > 0 return walk($nodes/$route, "
      "$depth + -1, $route) else return $nodes; }; "
      "walk($nodes, 1, /~+d.items)";
  const auto compiled =
      h.compile(selfSource, {.initialBindings = {{"nodes", repeatedCenter}}});
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  EXPECT_FALSE(namedInvokeDescriptors(compiled, h.vm, h.core, "walk").empty());
  const auto self = h.execute(selfSource, {{"nodes", repeatedCenter}});
  ASSERT_TRUE(self) << self.error().message;
  EXPECT_EQ(*self, (std::vector<CellRef>{successor, predecessor, successor,
                                         predecessor}));
  EXPECT_FALSE(h.vm.hasActiveFrames());
  const auto ordinary =
      h.execute("$nodes/~+d.items", {{"nodes", repeatedCenter}});
  ASSERT_TRUE(ordinary) << ordinary.error().message;
  EXPECT_EQ(*ordinary, *self);

  constexpr std::string_view mutualSource =
      "def pure even($nodes: Stream<Cell>, $depth: Int, $route): "
      "Stream<Cell> := { if $depth > 0 return odd($nodes/$route, "
      "$depth + -1, $route) else return $nodes; }; "
      "def pure odd($nodes: Stream<Cell>, $depth: Int, $route): "
      "Stream<Cell> := { if $depth > 0 return even($nodes/$route, "
      "$depth + -1, $route) else return $nodes; }; "
      "even($nodes, 1, /~+d.items)";
  const auto mutual = h.execute(mutualSource, {{"nodes", repeatedCenter}});
  ASSERT_TRUE(mutual) << mutual.error().message;
  EXPECT_EQ(*mutual, (std::vector<CellRef>{successor, predecessor, successor,
                                           predecessor}));
  EXPECT_FALSE(h.vm.hasActiveFrames());
}

TEST(VQLReadModifiersFunctorFrameTest,
     ModifierFlagsAndDirectionsSeparateVariantsAndExactCallsDeduplicate) {
  Harness h;
  const std::string source = std::string(recursiveDefinition) +
                             "walk($nodes, 1, /+d.next);"
                             "walk($nodes, 1, /+d.next);"
                             "walk($emptyNodes, 1, /+d.next);"
                             "walk($nodes, 1, /-d.next);"
                             "walk($nodes, 1, /~+d.next);"
                             "walk($nodes, 1, /~-d.next);"
                             "walk($nodes, 1, /+d.next>);"
                             "walk($nodes, 1, /~+d.next>[. = \"target\"][1]);";
  CompilationOptions options;
  options.initialBindings.emplace("nodes", std::vector<CellRef>{h.cell("n")});
  options.initialBindings.emplace("emptyNodes", std::vector<CellRef>{});
  const auto compiled = h.compile(source, options);
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  const auto descriptors =
      namedInvokeDescriptors(compiled, h.vm, h.core, "walk");
  ASSERT_GT(descriptors.size(), 6U);
  const auto unique =
      uniqueNamedInvokeDescriptors(compiled, h.vm, h.core, "walk");
  EXPECT_EQ(unique.size(), 6U);
  for (const CellRef descriptor : unique) {
    const auto record = h.vm.frameDescriptor(descriptor);
    ASSERT_TRUE(record) << record.error().message;
    EXPECT_TRUE(record->entry);
  }

  const CellRef center   = h.cell("center");
  const CellRef positive = h.cell("target");
  const CellRef negative = h.cell("target");
  h.link(center, "d.next", positive);
  h.link(center, "d.next", negative, DimVector::NEG);
  const auto combined =
      h.execute(std::string(recursiveDefinition) +
                    "walk($nodes, 1, /~+d.next>[. = \"target\"][1])",
                {{"nodes", {center}}});
  ASSERT_TRUE(combined) << combined.error().message;
  EXPECT_EQ(*combined, (std::vector<CellRef>{positive}));
}

TEST(VQLReadModifiersFunctorFrameTest,
     DereferenceUsesCloneMasterBeforePredicateClampAcrossNestedFrames) {
  Harness h;
  const CellRef firstOwner  = h.cell("first owner");
  const CellRef secondOwner = h.cell("second owner");
  const CellRef wantedFirst = h.cell("keep");
  const CellRef wantedCopy1 = h.cell("copy one");
  const CellRef wantedCopy2 = h.cell("copy two");
  const CellRef wantedNext  = h.cell("keep");
  const CellRef wantedNext1 = h.cell("next copy one");
  const CellRef wantedNext2 = h.cell("next copy two");
  const CellRef rejected    = h.cell("skip");
  const CellRef rejected1   = h.cell("rejected copy one");
  const CellRef rejected2   = h.cell("rejected copy two");
  h.link(wantedFirst, "d.clone", wantedCopy1);
  h.link(wantedCopy1, "d.clone", wantedCopy2);
  h.link(wantedNext, "d.clone", wantedNext1);
  h.link(wantedNext1, "d.clone", wantedNext2);
  h.link(rejected, "d.clone", rejected1);
  h.link(rejected1, "d.clone", rejected2);
  h.link(firstOwner, "d.next", wantedCopy2);
  h.link(secondOwner, "d.next", rejected2);
  h.link(wantedFirst, "d.next", wantedNext2);
  const CellRef firstMarker  = h.cell("first marker");
  const CellRef secondMarker = h.cell("second marker");
  h.link(firstOwner, "d.marker", firstMarker);
  h.link(secondOwner, "d.marker", secondMarker);
  const std::vector<CellRef> owners{firstOwner, secondOwner, firstOwner};

  constexpr std::string_view source = R"(
    def pure walk($nodes: Stream<Cell>, $depth: Int, $route): Stream<Cell> := {
      if $depth > 0 return walk($nodes/$route, $depth + -1, $route)
      else return $nodes;
    };
    let $walked := walk($owners, 2, /+d.next>[. = "keep"][1]);
    let $caller := $owners/d.marker;
    return $walked, $caller
  )";
  const auto result                 = h.execute(source, {{"owners", owners}});
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{wantedNext, firstMarker,
                                           secondMarker, firstMarker}));
  EXPECT_FALSE(h.vm.hasActiveFrames());
}

TEST(VQLReadModifiersFunctorFrameTest,
     ModifierTemplateWorkRefusesAtomicallyAndRetriesAtHigherLimit) {
  Harness h;
  const auto sourceFor = [](const std::string_view route) {
    return std::string(recursiveDefinition) + "walk(##, 1, " +
           std::string(route) + ")";
  };
  CompilationOptions limited;
  limited.limits.maxClosedFunctorTemplateWork = 0;
  const auto ordinary                         = h.compile(
      "def pure identity($value: Cell): Cell := $value; identity(##)", limited);
  ASSERT_TRUE(ordinary.success) << ordinary.errorMessage;
  const std::size_t before = h.arena.cells().size();
  const auto refused = h.compile(sourceFor("/~+d.next>/~-d.next>"), limited);
  EXPECT_FALSE(refused.success);
  EXPECT_FALSE(refused.cursor);
  EXPECT_NE(refused.errorMessage.find(
                "compiled VQL closed functor template work budget exhausted"),
            std::string::npos);
  EXPECT_EQ(h.arena.cells().size(), before);

  CompilationOptions retryOptions;
  retryOptions.limits.maxClosedFunctorTemplateWork = 10'000;
  const auto retry = h.compile(sourceFor("/~+d.next>/~-d.next>"), retryOptions);
  EXPECT_TRUE(retry.success) << retry.errorMessage;
}

TEST(VQLReadModifiersFunctorFrameTest,
     ReadPlacementAndKeepWorkWhileCreationStillRefuses) {
  Harness h;
  const auto node = h.cell("node");
  const auto next = h.cell("next");
  h.link(h.core.home(), "d.next", node);
  h.link(node, "d.next", next);
  for (const std::string_view route : {"/~+d.next!keep", "/+d.next::head>"}) {
    const std::string source = std::string(recursiveDefinition) +
                               "walk($owners, 2, " + std::string(route) + ")";
    CompilationOptions options;
    options.initialBindings.emplace("owners", std::vector<CellRef>{node});
    const auto compiled = h.compile(source, options);
    ASSERT_TRUE(compiled.success) << compiled.errorMessage;
    EXPECT_FALSE(
        namedInvokeDescriptors(compiled, h.vm, h.core, "walk").empty());
    const auto result = h.execute(source, options.initialBindings);
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(*result, (std::vector<CellRef>{
                           route == "/~+d.next!keep" ? node : h.core.home()}));
  }
  const auto creation = h.compile(std::string(recursiveDefinition) +
                                  "walk(##, 1, /+d.next%\"new\")");
  EXPECT_FALSE(creation.success);
  EXPECT_FALSE(creation.cursor);
}

TEST(VQLReadModifiersFunctorFrameTest,
     RecursiveReadModifiersExportAndRunInFreshVmHostAndMesh) {
  constexpr std::string_view source =
      "def pure visit($nodes: Stream<Cell>, $depth: Int, $route): "
      "Stream<Cell> := { if $depth > 0 return visit($nodes/$route, "
      "$depth + -1, $route) else return $nodes; }; "
      "let $owner := ##/d.seed%\"keep\"; "
      "let $leaf := $owner/d.clone%[1]; "
      "visit($owner, 1, /~+d.clone>[. = \"keep\"][1])";
  TemporaryDirectory temporary;
  auto scroll = std::make_shared<UserPermascroll>(
      UserPermascroll::Config{.storageDir = temporary.path / "scroll"});
  {
    Harness h;
    VQLCompiler compiler(h.coordinator, h.vm);
    const auto compiled = compiler.compile(source);
    ASSERT_TRUE(compiled.success) << compiled.errorMessage;
    ASSERT_FALSE(
        namedInvokeDescriptors(compiled, h.vm, h.core, "visit").empty());
    Store store(scroll);
    compiler.exportToStore(store);
    store.save((temporary.path / "program").string());
  }
  {
    Store store(scroll);
    ASSERT_NO_THROW(store.load((temporary.path / "program").string()));
    ASSERT_TRUE(store.structureHead());
    const auto manifold = store.rebuildManifold(*store.structureHead());
    zigzag::ArenaManifold arena(&manifold, store);
    zigzag::vortex::VortexCore core(arena);
    zigzag::vortex::VortexVM vm(core);
    const auto entry =
        arena.linked(core.home(), core.dims().spin, DimVector::POS);
    ASSERT_TRUE(entry);
    const auto execution =
        vm.run(vm.spawnCursor(*entry, "read-modifier-reload"), 100'000);
    ASSERT_TRUE(execution.success) << execution.errorMessage;
    auto halt = *entry;
    auto next = arena.linked(halt, core.dims().spin, DimVector::POS);
    while (next && vm.getOpcodeKind(*next) != OpcodeKind::Halt) {
      halt = *next;
      next = arena.linked(halt, core.dims().spin, DimVector::POS);
    }
    ASSERT_TRUE(next);
    const auto ports = core.inputsOf(*next);
    ASSERT_EQ(ports.size(), 1U);
    const auto values = vm.resultCells(ports.front());
    ASSERT_EQ(values.size(), 1U);
    EXPECT_EQ(arena.textOf(values.front()), "keep");
    EXPECT_FALSE(vm.hasActiveFrames());
  }

  zigzag::vortex::VortexHost host;
  const auto hostResult = host.executeVQLChecked(source);
  ASSERT_TRUE(hostResult) << hostResult.error().message;
  ASSERT_EQ(hostResult->size(), 1U);
  EXPECT_EQ(host.core().arena().textOf(hostResult->front()), "keep");

  namespace mesh        = zigzag::vortex::mesh;
  const auto meshResult = mesh::evaluate(mesh::Frontend::Vquery, source,
                                         {.maxCycles      = 100'000,
                                          .maxCells       = 100'000,
                                          .maxSolutions   = 16,
                                          .maxOutputBytes = 1024 * 1024});
  ASSERT_TRUE(meshResult) << meshResult.error();
  auto &arena = meshResult->context->arena;
  std::optional<CellRef> items;
  std::optional<CellRef> value;
  bool sawVisitInvoke = false;
  for (const auto &cell : arena.cells()) {
    if (arena.textOf(cell.birthOp) == "d.mesh.items") items = cell.birthOp;
    if (arena.textOf(cell.birthOp) == "d.mesh.value") value = cell.birthOp;
    if (meshResult->context->vm.getOpcodeKind(cell.birthOp) ==
            OpcodeKind::InvokeFrame &&
        arena.textOf(cell.birthOp).starts_with("#INVOKE_FRAME visit"))
      sawVisitInvoke = true;
  }
  EXPECT_TRUE(sawVisitInvoke);
  ASSERT_TRUE(items);
  ASSERT_TRUE(value);
  const auto record = arena.linked(meshResult->root, *items, DimVector::POS);
  ASSERT_TRUE(record);
  const auto output = arena.linked(*record, *value, DimVector::POS);
  ASSERT_TRUE(output);
  EXPECT_EQ(arena.textOf(*output), "keep");
}

} // namespace
} // namespace xanadu::vql
