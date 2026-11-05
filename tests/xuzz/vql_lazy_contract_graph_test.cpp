#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
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
#include "common/xanadu/zigzag/cell_views.hpp"

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

  CellRef link(const CellRef owner, const std::string_view name,
               const CellRef value,
               const DimVector direction = DimVector::POS) {
    const DimRef dimension = coordinator.resolveDimension(name);
    zigzag::expectWritten(arena.link(owner, dimension, direction, value));
    return value;
  }

  std::expected<std::vector<CellRef>, QueryExecutionError>
  run(const std::string_view source, ContractBindings bindings = {},
      QueryExecutionOptions options = {}) {
    QueryExecutor executor(coordinator, vm, options, std::move(bindings));
    return executor.execute(source);
  }

  CompilationResult compile(const std::string_view source,
                            ContractBindings bindings  = {},
                            CompilationOptions options = {}) {
    options.initialBindings = std::move(bindings);
    VQLCompiler compiler(coordinator, vm);
    return compiler.compile(source, options);
  }
};

std::size_t opcodeCount(const Harness &h, const CompilationResult &program,
                        const OpcodeKind kind) {
  return static_cast<std::size_t>(std::ranges::count_if(
      program.generatedOpcodes, [&](const CellRef opcode) {
        return h.vm.getOpcodeKind(opcode) == kind;
      }));
}

struct TempDirectory {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("vql_lazy_contract_" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  TempDirectory() { std::filesystem::create_directories(path); }
  ~TempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

TEST(VQLLazyContractGraphTest,
     PresentFalseLikeFieldsSkipInvalidDefaultsAndGraphUsesProbeScope) {
  constexpr std::string_view source =
      "def pure choose($item: Cell { +d.value: $value: Cell := "
      "string(##/d.missing) }): Cell := $value; choose($item)";
  Harness compiledHarness;
  const CellRef templateItem = compiledHarness.arena.makeCell("template");
  const auto compiled        = compiledHarness.compile(
      source, ContractBindings{{"item", {templateItem}}});
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  EXPECT_GT(opcodeCount(compiledHarness, compiled, OpcodeKind::ScopedChoice),
            0U);
  EXPECT_GT(opcodeCount(compiledHarness, compiled, OpcodeKind::VqlFieldProbe),
            0U);

  for (const int valueKind : {0, 1, 2}) {
    Harness h;
    const CellRef item  = h.arena.makeCell("item");
    const CellRef value = valueKind == 0
                              ? h.arena.makeScalarCell(std::int64_t{0})
                          : valueKind == 1 ? h.arena.makeScalarCell(false)
                                           : h.arena.makeCell("");
    h.link(item, "d.value", value);
    const auto result = h.run(source, {{"item", {item}}});
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(*result, (std::vector<CellRef>{value}));
  }

  Harness absent;
  const CellRef item = absent.arena.makeCell("item without field");
  const auto result  = absent.run(source, {{"item", {item}}});
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().category, QueryExecutionError::Category::Runtime);
}

TEST(VQLLazyContractGraphTest,
     NestedPathAndCallDefaultsUseTheirOwnerAndRestoreOuterContext) {
  Harness h;
  const CellRef outer        = h.arena.makeCell("outer");
  const CellRef inner        = h.arena.makeCell("inner");
  const CellRef innerDefault = h.arena.makeCell("inner default");
  const CellRef outerDefault = h.arena.makeCell("outer default");
  h.link(outer, "d.inner", inner);
  h.link(inner, "d.fallback", innerDefault);
  h.link(outer, "d.fallback", outerDefault);

  constexpr std::string_view source =
      "def pure fallback(): Cell := ./+d.fallback; "
      "def pure nested($outer: Cell { +d.inner: Cell { "
      "+d.value: $nested: Cell := ./+d.fallback }, "
      "+d.other: $after: Cell := fallback() }): Cell := $nested; "
      "def pure choose($outer: Cell { +d.inner: Cell { "
      "+d.value: $nested: Cell := ./+d.fallback }, "
      "+d.other: $after: Cell := fallback() }): Cell := $after; "
      "nested($outer)";
  const auto compiled = h.compile(source, {{"outer", {outer}}});
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  EXPECT_GT(opcodeCount(h, compiled, OpcodeKind::VqlFieldProbe), 0U);
  EXPECT_GT(opcodeCount(h, compiled, OpcodeKind::ScopedChoice), 0U);
  EXPECT_GT(opcodeCount(h, compiled, OpcodeKind::InvokeFrame), 0U);

  const auto result = h.run(source, {{"outer", {outer}}});
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{innerDefault}));
  EXPECT_NE(innerDefault, outerDefault);
  const auto after =
      h.run("def pure fallback(): Cell := ./+d.fallback; "
            "def pure choose($outer: Cell { +d.inner: Cell { "
            "+d.value: $nested: Cell := ./+d.fallback }, "
            "+d.other: $after: Cell := fallback() }): Cell := $after; "
            "choose($outer)",
            {{"outer", {outer}}});
  ASSERT_TRUE(after) << after.error().message;
  EXPECT_EQ(*after, (std::vector<CellRef>{outerDefault}));
}

TEST(VQLLazyContractGraphTest,
     PrematchAliasWinsAndMixedCallContractBoxesFreshLiteralResults) {
  Harness h;
  const CellRef outerFallback = h.arena.makeCell("outer fallback");
  const CellRef aliasItem     = h.arena.makeCell("alias item");
  const CellRef aliasLink     = h.arena.makeCell("alias link");
  h.link(aliasItem, "d.alias", aliasLink);

  constexpr std::string_view aliasSource =
      "def pure identity($value: Cell): Cell := $value; "
      "def pure choose($item: Cell { .: $fallback, "
      "+d.value: $value: Cell := identity($fallback) }): Cell := $value; "
      "choose($item)";
  const auto aliasResult = h.run(
      aliasSource, {{"fallback", {outerFallback}}, {"item", {aliasItem}}});
  ASSERT_TRUE(aliasResult) << aliasResult.error().message;
  EXPECT_EQ(*aliasResult, (std::vector<CellRef>{outerFallback}));

  constexpr std::string_view mixedSource =
      "def pure called(): Cell := \"called\"; "
      "def pure choose($item: Cell { +d.literal: $literal: Int := 7, "
      "+d.called: $called: Cell := called() }): Int := $literal; "
      "for $item in $items return choose($item)";
  const CellRef itemA = h.arena.makeCell("a");
  const CellRef itemB = h.arena.makeCell("b");
  const auto compiled = h.compile(mixedSource, {{"items", {itemA, itemB}}});
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  EXPECT_GT(opcodeCount(h, compiled, OpcodeKind::VqlBoxScalar), 0U);
  EXPECT_GT(opcodeCount(h, compiled, OpcodeKind::InvokeFrame), 0U);
  const auto result = h.run(mixedSource, {{"items", {itemA, itemB}}});
  ASSERT_TRUE(result) << result.error().message;
  ASSERT_EQ(result->size(), 2U);
  EXPECT_NE((*result)[0], (*result)[1]);
  EXPECT_EQ(h.arena.asInt64((*result)[0]), std::optional<std::int64_t>{7});
  EXPECT_EQ(h.arena.asInt64((*result)[1]), std::optional<std::int64_t>{7});
}

TEST(VQLLazyContractGraphTest,
     UnionFailureRollsBackCallDefaultCaptureAndRuntimeDimension) {
  Harness h;
  const CellRef item = h.arena.makeCell("item");
  constexpr std::string_view source =
      "def pure missing(): Cell := string(\"speculative\"); "
      "def pure chosen(): Cell := \"chosen\"; "
      "def pure select($item: Cell { +d.failed: $failed: Cell := missing(), "
      "+d.required: Int } | Cell { +d.chosen: $chosen: Cell := chosen() }): "
      "Cell := $chosen; select($item)";
  const auto result = h.run(source, {{"item", {item}}});
  ASSERT_TRUE(result) << result.error().message;
  ASSERT_EQ(result->size(), 1U);
  EXPECT_EQ(h.arena.textOf(result->front()), "chosen");
  EXPECT_FALSE(h.core.findDimension("d.failed"));
  EXPECT_FALSE(h.core.findDimension("d.required"));
  EXPECT_TRUE(h.core.findDimension("d.chosen"));
}

TEST(VQLLazyContractGraphTest,
     PresentWrongTypeForwardFieldDoesNotTryReverseOrDefault) {
  Harness h;
  const CellRef item    = h.arena.makeCell("item");
  const CellRef wrong   = h.arena.makeCell("present but not an integer");
  const CellRef reverse = h.arena.makeScalarCell(std::int64_t{9});
  h.link(item, "d.value", wrong);
  h.link(item, "d.value", reverse, DimVector::NEG);
  constexpr std::string_view source =
      "def pure fallback(): Int := \"would fail if evaluated\"; "
      "def pure choose($item: Cell { ~+d.value: $value: Int := fallback() }): "
      "Int := $value; choose($item)";
  const auto result = h.run(source, {{"item", {item}}});
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_TRUE(result->empty());
  EXPECT_EQ(h.arena.asInt64(reverse), std::optional<std::int64_t>{9});
}

TEST(VQLLazyContractGraphTest,
     GenericStreamValidationIgnoresElementCapturesAndRollsBackSuccess) {
  Harness h;
  const CellRef first  = h.arena.makeCell("first");
  const CellRef second = h.arena.makeCell("second");
  const CellRef values[]{first, second, first};
  constexpr std::string_view source =
      "def pure temporary(): Cell := \"temporary\"; "
      "def pure verify($items: Stream<Cell { +d.element: $capture: Cell := "
      "temporary() }>): Stream<Cell> := $items; verify($items)";
  const auto compiled = h.compile(source, {{"items", {values[0], values[1]}}});
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  EXPECT_GT(opcodeCount(h, compiled, OpcodeKind::ScopedChoice), 0U);
  EXPECT_GT(opcodeCount(h, compiled, OpcodeKind::VqlFieldProbe), 0U);
  const auto result =
      h.run(source, {{"items", {values[0], values[1], values[2]}}});
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{first, second, first}));
  EXPECT_FALSE(h.core.findDimension("d.element"));
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);
}

TEST(VQLLazyContractGraphTest,
     CallDefaultPreservesOrderedMultiResultsAndDuplicates) {
  Harness h;
  const CellRef item   = h.arena.makeCell("item");
  const CellRef first  = h.arena.makeCell("first");
  const CellRef second = h.arena.makeCell("second");
  constexpr std::string_view source =
      "def pure fallback(): Stream<Cell> := $fallbacks; "
      "def pure choose($item: Cell { +d.value: $value: Stream<Cell> := "
      "fallback() }): Stream<Cell> := $value; choose($item)";
  const auto result =
      h.run(source, {{"item", {item}}, {"fallbacks", {first, second, first}}});
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{first, second, first}));
}

TEST(VQLLazyContractGraphTest,
     MixedGraphDefaultsAcceptZeroOrOneAliasAndRefuseMany) {
  constexpr std::string_view source =
      "def pure called(): Cell := \"computed\"; "
      "def pure choose($item: Cell { +d.called: $called: Cell := called(), "
      "+d.alias: $alias: Cell := $prior }): Cell := $alias; choose($item)";
  for (const int priorCount : {0, 1}) {
    Harness h;
    const CellRef item = h.arena.makeCell("item");
    std::vector<CellRef> prior;
    if (priorCount == 1) prior.push_back(h.arena.makeCell("prior"));
    ContractBindings bindings{{"item", {item}}, {"prior", prior}};
    const auto result = h.run(source, bindings);
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(*result, prior);
  }

  Harness h;
  const CellRef item   = h.arena.makeCell("item");
  const CellRef first  = h.arena.makeCell("first");
  const CellRef second = h.arena.makeCell("second");
  const auto refused =
      h.run(source, {{"item", {item}}, {"prior", {first, second}}});
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().category, QueryExecutionError::Category::Compile);
  EXPECT_NE(refused.error().message.find("at most one"), std::string::npos);
}

TEST(VQLLazyContractGraphTest, DestructuredForMissingFieldUsesBindEmpty) {
  Harness h;
  const CellRef item = h.arena.makeCell("item");
  const DimRef items = h.coordinator.resolveDimension("d.items");
  zigzag::expectWritten(
      h.arena.link(zigzag::rankTail(h.arena, h.core.home(), items), items,
                   DimVector::POS, item));
  constexpr std::string_view source =
      "for { .: $owner: Cell, +d.absent: $value } in ##/d.items "
      "return $value";
  const auto result = h.run(source);
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_TRUE(result->empty());
  EXPECT_TRUE(h.core.findDimension("d.absent"));
}

TEST(VQLLazyContractGraphTest,
     ReturnPromiseFailureIsTypedAndFailedRequestDoesNotPoisonNextQuery) {
  Harness h;
  const CellRef item = h.arena.makeCell("item");
  constexpr std::string_view bad =
      "def pure fallback(): Cell := \"not an integer\"; "
      "def pure choose($item: Cell { +d.value: $value: Cell := fallback() }): "
      "Int := $value; choose($item)";
  const auto failed = h.run(bad, {{"item", {item}}});
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error().category,
            QueryExecutionError::Category::TypeContract);
  EXPECT_NE(failed.error().message.find("choose"), std::string::npos);
  EXPECT_EQ(h.vm.choiceDepth(), 0U);
  EXPECT_FALSE(h.vm.hasActiveFrames());

  const auto next = h.run("def pure ok(): Int := 7; ok()");
  ASSERT_TRUE(next) << next.error().message;
  ASSERT_EQ(next->size(), 1U);
  EXPECT_EQ(h.arena.asInt64(next->front()), std::optional<std::int64_t>{7});
}

TEST(VQLLazyContractGraphTest,
     FiniteAffectedOuterCanCallRecursiveHelperButCannotBeRecursive) {
  Harness h;
  const CellRef item = h.arena.makeCell("item");
  const CellRef next = h.arena.makeCell("next");
  const CellRef last = h.arena.makeCell("last");
  const DimRef dNext = h.coordinator.resolveDimension("d.next");
  zigzag::expectWritten(h.arena.link(item, dNext, DimVector::POS, next));
  zigzag::expectWritten(h.arena.link(next, dNext, DimVector::POS, last));
  constexpr std::string_view source =
      "def pure fallback(): Cell := \"unused\"; "
      "def pure last($node: Cell): Cell := { if $node/+d.next[1] "
      "return last($node/+d.next[1]) else return $node; }; "
      "def pure choose($item: Cell { +d.optional: Cell := fallback() }): "
      "Cell := last($item); choose($item)";
  const auto compiled = h.compile(source, {{"item", {item}}});
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  EXPECT_GT(opcodeCount(h, compiled, OpcodeKind::InvokeFrame), 0U);
  const auto result = h.run(source, {{"item", {item}}});
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{last}));

  constexpr std::string_view recursiveOuter =
      "def pure fallback(): Cell := \"unused\"; "
      "def pure choose($item: Cell { +d.optional: Cell := fallback() }): "
      "Cell := choose($item); choose($item)";
  const auto refused = h.run(recursiveOuter, {{"item", {item}}});
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().category, QueryExecutionError::Category::Compile);
}

TEST(VQLLazyContractGraphTest, GraphRunsAfterSourceVmAstAndStoreReload) {
  TempDirectory temporary;
  auto scroll = std::make_shared<UserPermascroll>(
      UserPermascroll::Config{.storageDir = temporary.path / "scroll"});
  {
    Harness h;
    constexpr std::string_view source =
        "def pure fallback(): Cell := \"reloaded\"; "
        "def pure choose($item: Cell { +d.missing: $value: Cell := "
        "fallback() }): Cell := $value; choose(##)";
    VQLCompiler compiler(h.coordinator, h.vm);
    const auto compiled = compiler.compile(source);
    ASSERT_TRUE(compiled.success) << compiled.errorMessage;
    xanadu::Store store(scroll);
    compiler.exportToStore(store);
    store.save((temporary.path / "program").string());
  }

  xanadu::Store store(scroll);
  store.load((temporary.path / "program").string());
  ASSERT_TRUE(store.structureHead());
  const auto manifold = store.rebuildManifold(*store.structureHead());
  zigzag::ArenaManifold arena(&manifold, store);
  zigzag::vortex::VortexCore core(arena);
  MultiStoreCoordinator coordinator(core);
  zigzag::vortex::VortexVM vm(core);
  vm.setContractCoordinator(coordinator);
  const auto entry =
      arena.linked(core.home(), core.dims().spin, DimVector::POS);
  ASSERT_TRUE(entry);
  const auto cursor    = vm.spawnCursor(*entry, "lazy-contract-reload");
  const auto execution = vm.run(cursor, 100'000);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  auto terminal = *entry;
  auto next     = arena.linked(terminal, core.dims().spin, DimVector::POS);
  while (next && vm.getOpcodeKind(*next) != OpcodeKind::Halt) {
    terminal = *next;
    next     = arena.linked(terminal, core.dims().spin, DimVector::POS);
  }
  ASSERT_TRUE(next);
  const auto resultSlots = core.inputsOf(*next);
  ASSERT_EQ(resultSlots.size(), 1U);
  const auto results = vm.resultCells(resultSlots.front());
  ASSERT_EQ(results.size(), 1U);
  EXPECT_EQ(arena.textOf(results.front()), "reloaded");
}

TEST(VQLLazyContractGraphTest, HostAndMeshVqueryRoutesExecuteCallDefaultGraph) {
  constexpr std::string_view source =
      "def pure fallback(): Cell := \"routed\"; "
      "def pure choose($item: Cell { +d.missing: $value: Cell := "
      "fallback() }): Cell := $value; choose(##)";
  zigzag::vortex::VortexHost host;
  const auto hostResult = host.executeVQLChecked(source);
  ASSERT_TRUE(hostResult) << hostResult.error().message;
  ASSERT_EQ(hostResult->size(), 1U);
  EXPECT_EQ(host.core().arena().textOf(hostResult->front()), "routed");

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
  EXPECT_EQ(arena.textOf(*output), "routed");
}

TEST(VQLLazyContractGraphTest, ContractEmissionBudgetRefusesAtomically) {
  Harness h;
  constexpr std::string_view source =
      "def pure fallback(): Cell := \"budget\"; "
      "def pure choose($item: Cell { +d.value: $value: Cell := "
      "fallback() }): Cell := $value; choose(##)";
  const auto cellsBefore = h.arena.cellCount();
  CompilationOptions options;
  options.maxContractEmissions = 1;
  const auto compiled          = h.compile(source, {}, options);
  ASSERT_FALSE(compiled.success);
  EXPECT_NE(compiled.errorMessage.find("budget"), std::string::npos);
  EXPECT_TRUE(compiled.generatedOpcodes.empty());
  EXPECT_EQ(h.arena.cellCount(), cellsBefore);
  EXPECT_FALSE(h.core.findDimension("d.value"));
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);
}

TEST(VQLLazyContractGraphTest, ContextCaptureAndDefaultRespectEmissionBudget) {
  Harness h;
  constexpr std::string_view source =
      "def pure fallback(): Cell := \"budget\"; "
      "def pure choose({ .: $parent, +d.value: $value: Cell := "
      "fallback() }): Cell := $parent; ##/choose()";
  const auto before = h.arena.cellCount();
  for (const std::size_t budget : {std::size_t{1}, std::size_t{32}}) {
    CompilationOptions options;
    options.maxContractEmissions = budget;
    const auto failed            = h.compile(source, {}, options);
    ASSERT_FALSE(failed.success);
    EXPECT_NE(failed.errorMessage.find("emission budget"), std::string::npos);
    EXPECT_EQ(h.arena.cellCount(), before);
    EXPECT_EQ(h.arena.outstandingMarks(), 0u);
  }
  const auto compiled = h.compile(source);
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  EXPECT_TRUE(std::ranges::any_of(compiled.generatedOpcodes, [&](CellRef op) {
    return h.arena.textOf(op) == "#CONTEXT_CAPTURE_END";
  }));
  ASSERT_TRUE(compiled.cursor);
  const auto result = h.vm.run(*compiled.cursor);
  ASSERT_TRUE(result.success) << result.errorMessage;
  ASSERT_EQ(compiled.resultSlots.size(), 1u);
  EXPECT_EQ(h.vm.resultCells(compiled.resultSlots.front()),
            (std::vector<CellRef>{h.core.home()}));
}

TEST(VQLLazyContractGraphTest,
     RuntimeAliasCardinalityIsLazyInComputedContract) {
  Harness h;
  const auto first    = h.arena.makeCell("first");
  const auto second   = h.arena.makeCell("second");
  const auto item     = h.arena.makeCell("item");
  const auto existing = h.arena.makeCell("existing");
  constexpr std::string_view source =
      "def pure supply($xs: Stream<Cell>): Stream<Cell> := $xs; "
      "def pure called(): Cell := \"called\"; "
      "def pure choose($fallback: Stream<Cell>, $item: Cell { "
      "+d.value: $value: Cell? := $fallback, "
      "+d.called: $c: Cell := called() }): Cell? := $value; "
      "choose(supply($items), $item)";
  const ContractBindings bindings{{"items", {first, second, first}},
                                  {"item", {item}}};
  const auto absent = h.run(source, bindings);
  ASSERT_FALSE(absent);
  EXPECT_EQ(absent.error().category, QueryExecutionError::Category::Runtime);
  EXPECT_NE(absent.error().message.find("more than one cell"),
            std::string::npos);
  EXPECT_EQ(h.arena.outstandingMarks(), 0u);
  h.link(item, "d.value", existing);
  const auto present = h.run(source, bindings);
  ASSERT_TRUE(present) << present.error().message;
  EXPECT_EQ(*present, (std::vector<CellRef>{existing}));
  EXPECT_EQ(h.arena.outstandingMarks(), 0u);
}

} // namespace
} // namespace xanadu::vql
