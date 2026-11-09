/**
 * @file vql_functor_default_frames_test.cpp
 * @brief Closed literal defaults for required recursive VQL functors.
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
#include "common/xanadu/vql/frame_eligibility.hpp"
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
            const CellRef to) {
    zigzag::expectWritten(arena.link(from, dim(dimension), DimVector::POS, to));
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
      ("vql_functor_default_frames_" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));

  TemporaryDirectory() { std::filesystem::create_directories(path); }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

constexpr std::string_view recursiveWithDefault =
    "def pure identity($values: Stream<Cell>): Stream<Cell> := $values; "
    "def pure walk($nodes: Stream<Cell>, $depth: Int, "
    "$route := /+d.next): Stream<Cell> := { "
    "if $depth > 0 return identity(walk($nodes/$route, $depth + -1)) "
    "else return $nodes; }; ";

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

TEST(VQLFunctorDefaultFrameTest,
     OmittedClosedDefaultDrivesSelfAndMutualRecursiveFrames) {
  Harness h;
  const CellRef first  = h.cell("first");
  const CellRef second = h.cell("second");
  const CellRef third  = h.cell("third");
  h.link(first, "d.next", second);
  h.link(second, "d.next", third);

  const std::string selfSource =
      std::string(recursiveWithDefault) + "walk($nodes, 2)";
  CompilationOptions selfOptions;
  selfOptions.initialBindings.emplace("nodes", std::vector<CellRef>{first});
  const auto selfCompiled = h.compile(selfSource, selfOptions);
  ASSERT_TRUE(selfCompiled.success) << selfCompiled.errorMessage;
  EXPECT_FALSE(
      namedInvokeDescriptors(selfCompiled, h.vm, h.core, "walk").empty());
  const auto self = h.execute(selfSource, {{"nodes", {first}}});
  ASSERT_TRUE(self) << self.error().message;
  EXPECT_EQ(*self, (std::vector<CellRef>{third}));
  EXPECT_FALSE(h.vm.hasActiveFrames());

  constexpr std::string_view mutual = R"(
    def pure identity($values: Stream<Cell>): Stream<Cell> := $values;
    def pure even($nodes: Stream<Cell>, $depth: Int,
                  $route := /+d.next): Stream<Cell> := {
      if $depth > 0 return identity(odd($nodes/$route, $depth + -1))
      else return $nodes;
    };
    def pure odd($nodes: Stream<Cell>, $depth: Int,
                 $route := /+d.next): Stream<Cell> := {
      if $depth > 0 return identity(even($nodes/$route, $depth + -1))
      else return $nodes;
    };
    even($nodes, 2)
  )";
  const auto mutualResult           = h.execute(mutual, {{"nodes", {first}}});
  ASSERT_TRUE(mutualResult) << mutualResult.error().message;
  EXPECT_EQ(*mutualResult, (std::vector<CellRef>{third}));
  EXPECT_FALSE(h.vm.hasActiveFrames());

  constexpr std::string_view forwarded = R"(
    def pure apply($nodes: Stream<Cell>, $route := /+d.next):
      Stream<Cell> := $nodes/$route;
    def pure forward($nodes: Stream<Cell>, $route := /+d.next):
      Stream<Cell> := apply($nodes, $route);
    forward($nodes)
  )";
  CompilationOptions forwardOptions;
  forwardOptions.initialBindings.emplace("nodes", std::vector<CellRef>{first});
  const auto forwardCompiled = h.compile(forwarded, forwardOptions);
  ASSERT_TRUE(forwardCompiled.success) << forwardCompiled.errorMessage;
  EXPECT_FALSE(
      namedInvokeDescriptors(forwardCompiled, h.vm, h.core, "forward").empty());
  EXPECT_FALSE(
      namedInvokeDescriptors(forwardCompiled, h.vm, h.core, "apply").empty());
  const auto forwardResult = h.execute(forwarded, {{"nodes", {first}}});
  ASSERT_TRUE(forwardResult) << forwardResult.error().message;
  EXPECT_EQ(*forwardResult, (std::vector<CellRef>{second, third}));
  EXPECT_FALSE(h.vm.hasActiveFrames());
}

TEST(VQLFunctorDefaultFrameTest,
     SuppliedOverrideWinsAndExactDefaultTemplateReusesDescriptor) {
  Harness h;
  const CellRef first = h.cell("first");
  const CellRef next  = h.cell("next");
  const CellRef other = h.cell("other");
  h.link(first, "d.next", next);
  h.link(first, "d.other", other);

  constexpr std::string_view source = R"(
    def pure walk($nodes: Stream<Cell>, $depth: Int,
                  $route := /+d.next): Stream<Cell> := {
      if $depth > 0
        return walk($nodes/$route, $depth + -1, $route)
      else return $nodes;
    };
    let $omitted := walk($nodes, 1);
    let $same := walk($nodes, 1, /+d.next);
    let $override := walk($nodes, 1, /+d.other);
    return $omitted, $same, $override
  )";
  CompilationOptions options;
  options.initialBindings.emplace("nodes", std::vector<CellRef>{first});
  const auto compiled = h.compile(source, options);
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  const auto descriptors =
      namedInvokeDescriptors(compiled, h.vm, h.core, "walk");
  ASSERT_FALSE(descriptors.empty());
  std::set<CellRef> unique(descriptors.begin(), descriptors.end());
  EXPECT_EQ(unique.size(), 2U);

  const auto result = h.execute(source, {{"nodes", {first}}});
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{next, next, other}));
}

TEST(VQLFunctorDefaultFrameTest,
     ExplicitEmptyAndOrdinaryValuesDoNotSelectTheOmittedDefault) {
  constexpr std::string_view source =
      "def pure walk($nodes: Stream<Cell>, $depth: Int, "
      "$route := /+d.next): Stream<Cell> := { "
      "if $depth > 0 return walk($nodes/$route, $depth + -1, $route) "
      "else return $nodes; }; walk($nodes, 1, $supplied)";

  for (const bool emptyActual : {true, false}) {
    Harness h;
    CompilationOptions options;
    options.initialBindings.emplace("nodes",
                                    std::vector<CellRef>{h.core.home()});
    options.initialBindings.emplace(
        "supplied", emptyActual ? std::vector<CellRef>{}
                                : std::vector<CellRef>{h.cell("ordinary")});
    const auto compiled = h.compile(source, options);
    ASSERT_TRUE(compiled.success) << compiled.errorMessage;
    ASSERT_TRUE(compiled.cursor);
    const auto result = h.execute(source, options.initialBindings);
    ASSERT_FALSE(result);
    EXPECT_NE(result.error().message.find(
                  emptyActual ? "requires exactly one closure handle"
                              : "invalid Vortex closure value"),
              std::string::npos)
        << result.error().message;
    EXPECT_FALSE(h.vm.hasActiveFrames());
  }
}

TEST(VQLFunctorDefaultFrameTest,
     UfcsDefaultUsesFormalIndexAndIgnoresCallerFunctorBinding) {
  Harness h;
  const CellRef node  = h.cell("node");
  const CellRef next  = h.cell("next");
  const CellRef other = h.cell("other");
  h.link(h.core.home(), "d.items", node);
  h.link(node, "d.next", next);
  h.link(node, "d.other", other);

  constexpr std::string_view source = R"(
    def pure via($nodes: Stream<Cell>, $route := /+d.next):
      Stream<Cell> := $nodes/$route;
    let $route := /+d.other;
    let $normal := $items/via();
    let $empty := $emptyItems/via();
    return $normal, $empty
  )";
  CompilationOptions options;
  options.initialBindings.emplace("items", std::vector<CellRef>{node});
  options.initialBindings.emplace("emptyItems", std::vector<CellRef>{});
  const auto compiled = h.compile(source, options);
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  EXPECT_FALSE(namedInvokeDescriptors(compiled, h.vm, h.core, "via").empty());
  const auto result =
      h.execute(source, {{"items", {node}}, {"emptyItems", {}}});
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{next}));
  EXPECT_FALSE(h.vm.hasActiveFrames());
}

TEST(VQLFunctorDefaultFrameTest,
     UnusedAndDirectValueDefaultsKeepOrdinaryPathEvaluation) {
  Harness h;
  const CellRef item         = h.cell("item");
  const CellRef defaultValue = h.cell("default value");
  h.link(h.core.home(), "d.items", item);
  h.link(h.core.home(), "d.next", defaultValue);

  constexpr std::string_view source = R"(
    def pure ignore(., $route := /+d.next): Stream<Cell> := ##/+d.items;
    def pure expose(., $route := /+d.next): Stream<Cell> := $route;
    let $ignored := ignore(##);
    let $exposed := expose(##);
    return $ignored, $exposed
  )";
  const auto result                 = h.execute(source);
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{item, defaultValue}));
}

TEST(VQLFunctorDefaultFrameTest,
     CapturedDefaultDoesNotBecomeAClosedNativeFunctorTemplate) {
  constexpr std::string_view source = R"(
    def pure walk($nodes: Stream<Cell>, $depth: Int,
                  $route := /$outer): Stream<Cell> := {
      if $depth > 0 return walk($nodes/$route, $depth + -1)
      else return $nodes;
    };
    walk(##, 1)
  )";
  const auto query                  = Parser(source).parseQuery();
  const auto analysis               = analyzeFrameEligibility(query);
  ASSERT_TRUE(analysis) << analysis.error().message;
  EXPECT_FALSE(analysis->reusableDefinitions.contains("walk"));

  constexpr std::string_view mixedUse = R"(
    def pure mixed($nodes: Stream<Cell>, $route := /+d.next):
      Stream<Cell> := {
        let $ordinary := $route;
        return $nodes/$route;
      };
    mixed(##)
  )";
  const auto mixedQuery               = Parser(mixedUse).parseQuery();
  const auto mixedAnalysis            = analyzeFrameEligibility(mixedQuery);
  ASSERT_TRUE(mixedAnalysis) << mixedAnalysis.error().message;
  EXPECT_FALSE(mixedAnalysis->reusableDefinitions.contains("mixed"));

  constexpr std::string_view forwardedValue = R"(
    def pure consume($value): Stream<Cell> := $value;
    def pure relay($value): Stream<Cell> := {
      let $ordinary := consume($value);
      return ##/$value;
    };
    def pure mixedDirect($nodes: Stream<Cell>, $route := /+d.next):
      Stream<Cell> := {
        let $ordinary := consume($route);
        return $nodes/$route;
      };
    def pure mixedHop($nodes: Stream<Cell>, $route := /+d.next):
      Stream<Cell> := {
        let $ordinary := relay($route);
        return $nodes/$route;
      };
    mixedDirect(##)
  )";
  const auto valueQuery    = Parser(forwardedValue).parseQuery();
  const auto valueAnalysis = analyzeFrameEligibility(valueQuery);
  ASSERT_TRUE(valueAnalysis) << valueAnalysis.error().message;
  EXPECT_FALSE(valueAnalysis->reusableDefinitions.contains("mixedDirect"));
  EXPECT_FALSE(valueAnalysis->reusableDefinitions.contains("mixedHop"));
}

TEST(VQLFunctorDefaultFrameTest,
     TemplateBudgetRefusalRollsBackAndHigherLimitCanRetry) {
  Harness h;
  const CellRef first  = h.cell("first");
  const CellRef second = h.cell("second");
  h.link(first, "d.next", second);
  CompilationOptions limited;
  limited.limits.maxClosedFunctorTemplateWork = 0;
  limited.initialBindings.emplace("nodes", std::vector<CellRef>{first});
  const auto before = h.arena.cells().size();
  const auto refused =
      h.compile(std::string(recursiveWithDefault) + "walk($nodes, 1)", limited);
  EXPECT_FALSE(refused.success);
  EXPECT_FALSE(refused.cursor);
  EXPECT_NE(refused.errorMessage.find(
                "compiled VQL closed functor template work budget exhausted"),
            std::string::npos);
  EXPECT_EQ(h.arena.cells().size(), before);

  CompilationOptions retry;
  retry.initialBindings.emplace("nodes", std::vector<CellRef>{first});
  const auto compiled =
      h.compile(std::string(recursiveWithDefault) + "walk($nodes, 1)", retry);
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  const auto result =
      h.execute(std::string(recursiveWithDefault) + "walk($nodes, 1)",
                {{"nodes", {first}}});
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{second}));
}

TEST(VQLFunctorDefaultFrameTest,
     ExportedDefaultFramesReloadAndExecuteThroughHostAndMeshFrontends) {
  TemporaryDirectory temporary;
  auto scroll = std::make_shared<UserPermascroll>(
      UserPermascroll::Config{.storageDir = temporary.path / "scroll"});
  constexpr std::string_view source = R"(
    def pure identity($values: Stream<Cell>): Stream<Cell> := $values;
    def pure visit($nodes: Stream<Cell>, $depth: Int,
                   $route := /+d.next): Stream<Cell> := {
      if $depth > 0 return identity(visit($nodes/$route, $depth + -1))
      else return $nodes;
    };
    let $owner := ##/d.seed%"owner";
    let $leaf := $owner/d.next%"leaf";
    visit($owner, 1)
  )";
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
      vm.run(vm.spawnCursor(*entry, "default-functor-reload"), 100'000);
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
  EXPECT_EQ(arena.textOf(values.front()), "leaf");
  EXPECT_FALSE(vm.hasActiveFrames());

  zigzag::vortex::VortexHost host;
  const auto hostResult = host.executeVQLChecked(source);
  ASSERT_TRUE(hostResult) << hostResult.error().message;
  ASSERT_EQ(hostResult->size(), 1U);
  EXPECT_EQ(host.core().arena().textOf(hostResult->front()), "leaf");

  namespace mesh        = zigzag::vortex::mesh;
  const auto meshResult = mesh::evaluate(mesh::Frontend::Vquery, source,
                                         {.maxCycles      = 100'000,
                                          .maxCells       = 100'000,
                                          .maxSolutions   = 16,
                                          .maxOutputBytes = 1024 * 1024});
  ASSERT_TRUE(meshResult) << meshResult.error();
  auto &meshArena = meshResult->context->arena;
  std::optional<CellRef> items;
  std::optional<CellRef> value;
  bool sawVisitInvoke = false;
  for (const auto &cell : meshArena.cells()) {
    if (meshArena.textOf(cell.birthOp) == "d.mesh.items") items = cell.birthOp;
    if (meshArena.textOf(cell.birthOp) == "d.mesh.value") value = cell.birthOp;
    if (meshResult->context->vm.getOpcodeKind(cell.birthOp) ==
            OpcodeKind::InvokeFrame &&
        meshArena.textOf(cell.birthOp).starts_with("#INVOKE_FRAME visit"))
      sawVisitInvoke = true;
  }
  EXPECT_TRUE(sawVisitInvoke);
  ASSERT_TRUE(items);
  ASSERT_TRUE(value);
  const auto record =
      meshArena.linked(meshResult->root, *items, DimVector::POS);
  ASSERT_TRUE(record);
  const auto output = meshArena.linked(*record, *value, DimVector::POS);
  ASSERT_TRUE(output);
  EXPECT_EQ(meshArena.textOf(*output), "leaf");
}

} // namespace
} // namespace xanadu::vql
