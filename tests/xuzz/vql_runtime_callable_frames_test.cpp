#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vortex/mesh_evaluator.hpp"
#include "common/xanadu/vortex/vortex_host.hpp"
#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/vql/execution.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vql {
namespace {
using zigzag::CellRef;
using zigzag::DimVector;
using zigzag::vortex::OpcodeKind;
using Bindings = ContractBindings;

struct Harness {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  MultiStoreCoordinator coordinator{core};
  zigzag::vortex::VortexVM vm{core};

  CellRef cell(const std::string_view value) { return arena.makeCell(value); }

  CellRef integer(const std::int64_t value) {
    return arena.makeScalarCell(value);
  }

  void link(const CellRef from, const std::string_view dimension,
            const CellRef to) {
    zigzag::expectWritten(arena.link(
        from, coordinator.resolveDimension(dimension), DimVector::POS, to));
  }

  CompilationResult compile(const std::string_view source,
                            Bindings bindings = {}) {
    VQLCompiler compiler(coordinator, vm);
    return compiler.compile(source, {.initialBindings = std::move(bindings)});
  }

  std::expected<std::vector<CellRef>, QueryExecutionError>
  run(const std::string_view source, ContractBindings bindings = {}) {
    QueryExecutor executor(coordinator, vm, {}, std::move(bindings));
    return executor.execute(source);
  }

  std::size_t invokes(const CompilationResult &compiled) const {
    std::size_t count = 0;
    for (const CellRef opcode : compiled.generatedOpcodes)
      count += vm.getOpcodeKind(opcode) == OpcodeKind::InvokeFrame;
    return count;
  }
};

struct TemporaryDirectory {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("vql_runtime_callable_" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));

  TemporaryDirectory() { std::filesystem::create_directories(path); }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

TEST(VQLRuntimeCallableFramesTest,
     NamedFrameAppliesFormalHandleAgainstItsRuntimeStream) {
  Harness h;
  const CellRef a    = h.cell("a");
  const CellRef b    = h.cell("b");
  const CellRef tagA = h.cell("A");
  const CellRef tagB = h.cell("B");
  h.link(h.core.home(), "d.items", a);
  h.link(a, "d.items", b);
  h.link(a, "d.tag", tagA);
  h.link(b, "d.tag", tagB);

  constexpr std::string_view source =
      "def pure apply($step: Cell, $items: Stream<Cell>): Stream<Cell> := "
      "$items/$step; let $step := \\($x) $x/+d.tag; "
      "apply($step, ##/d.items)";
  const auto compiled = h.compile(source);
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  EXPECT_GT(h.invokes(compiled), 0u);
  const auto result = h.run(source);
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{tagA, tagB}));
}

TEST(VQLRuntimeCallableFramesTest,
     LambdaBodyUsesItsFormalAndAFreeClosureHandle) {
  Harness h;
  const CellRef first  = h.cell("first");
  const CellRef second = h.cell("second");
  const CellRef leaf   = h.cell("leaf");
  h.link(h.core.home(), "d.items", first);
  h.link(first, "d.items", second);
  h.link(first, "d.next", leaf);

  constexpr std::string_view source = "let $step := \\($item) $item/+d.next; "
                                      "let $map := \\($x) $x/$step; "
                                      "return ($input/$map)";
  const auto result = h.run(source, {{"input", {first, first}}});
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{leaf, leaf}));
}

TEST(VQLRuntimeCallableFramesTest,
     NestedUnaryLambdasStageAndComposeInDeclaredOrder) {
  Harness h;
  const CellRef a = h.cell("a");
  const CellRef b = h.cell("b");
  const CellRef c = h.cell("c");
  h.link(h.core.home(), "d.first", a);
  h.link(a, "d.second", b);
  h.link(b, "d.second", c);
  constexpr std::string_view source =
      "let $compose := \\($f) (\\($g) (\\($x) $x/$g/$f)); "
      "let $outer := \\($x) $x/+d.second[1]; "
      "let $inner := \\($x) $x/+d.first; "
      "let $withOuter := $outer/$compose; "
      "let $withBoth := $inner/$withOuter; "
      "return (##/$withBoth)";
  const auto result = h.run(source);
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{b}));
}

TEST(VQLRuntimeCallableFramesTest,
     NamedCallsCanBeWrappedInHandlesAndForwarded) {
  Harness h;
  const CellRef a = h.cell("a");
  const CellRef b = h.cell("b");
  h.link(h.core.home(), "d.items", a);
  h.link(a, "d.next", b);
  constexpr std::string_view source =
      "def pure next($item: Cell): Cell := $item/+d.next; "
      "def pure forward($step, $items: Stream<Cell>): Stream<Cell> := "
      "$items/$step; def pure relay($step, $items: Stream<Cell>): "
      "Stream<Cell> := forward($step, $items); "
      "let $handle := \\($item) next($item); relay($handle, ##/d.items)";
  const auto result = h.run(source);
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{b}));
}

TEST(VQLRuntimeCallableFramesTest,
     NativeBodyCanBindAndApplyItsLocalFunctionHandle) {
  Harness h;
  const CellRef a = h.cell("a");
  const CellRef b = h.cell("b");
  h.link(a, "d.next", b);
  h.link(h.core.home(), "d.items", a);
  constexpr std::string_view source =
      "def pure identity($item: Cell): Cell := $item; "
      "def pure applyLocal($items: Stream<Cell>): Stream<Cell> := { "
      "let $handle := \\($item) identity($item); return $items/$handle; }; "
      "applyLocal(##/d.items)";
  const auto compiled = h.compile(source);
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  EXPECT_GT(h.invokes(compiled), 0u);
  const auto result = h.run(source);
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{a}));
}

TEST(VQLRuntimeCallableFramesTest,
     NamedDefaultCanCreateAndApplyARuntimeClosureHandle) {
  Harness h;
  const CellRef item = h.cell("item");
  const CellRef next = h.cell("next");
  h.link(h.core.home(), "d.items", item);
  h.link(item, "d.next", next);
  constexpr std::string_view source =
      "def pure apply($items: Stream<Cell>, $step := "
      "\\($x) $x/+d.next): Stream<Cell> := $items/$step; "
      "apply(##/d.items)";
  const auto compiled = h.compile(source);
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  EXPECT_GT(h.invokes(compiled), 0u);
  const auto result = h.run(source);
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{next}));
}

TEST(VQLRuntimeCallableFramesTest,
     NamedFrameCanReturnClosureOverItsInvocationBindings) {
  Harness h;
  const CellRef target = h.cell("target");
  h.link(h.core.home(), "d.target", target);
  constexpr std::string_view source =
      "def pure make($route) := \\($item) $item/$route; "
      "let $route := \\($x) $x/+d.target; let $step := make($route); "
      "return (##/$step)";
  const auto result = h.run(source);
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{target}));
}

TEST(VQLRuntimeCallableFramesTest,
     StaticAndRuntimeApplicationsShareOneNativeDefinition) {
  Harness h;
  const CellRef first  = h.cell("first");
  const CellRef second = h.cell("second");
  h.link(h.core.home(), "d.items", first);
  h.link(first, "d.items", second);
  h.link(first, "d.next", second);
  constexpr std::string_view source =
      "def pure apply($step, $items: Stream<Cell>): Stream<Cell> := "
      "$items/$step; let $dynamic := \\($x) $x/+d.next[1]; "
      "return (apply(/+d.items, ##)), (apply($dynamic, ##/d.items))";
  const auto compiled = h.compile(source);
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  EXPECT_GT(h.invokes(compiled), 0u);
  const auto result = h.run(source);
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{first, second, second}));
}

TEST(VQLRuntimeCallableFramesTest,
     InlineRelayForwardsAClosedStaticFunctorWithoutBindingItAsAValue) {
  Harness h;
  const CellRef input = h.cell("input");
  const CellRef child = h.cell("child");
  h.link(input, "d.next", child);
  constexpr std::string_view source =
      "def pure apply($f, $x: Cell): Cell := $x/$f; "
      "def pure relay($x: Cell): Cell := apply($route, $x); "
      "let $route := /+d.next; return (relay($input))";
  const Bindings bindings{{"input", {input}}};
  const auto compiled = h.compile(source, bindings);
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  const auto result = h.run(source, bindings);
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{child}));
}

TEST(VQLRuntimeCallableFramesTest,
     NativeRelayForwardsARealRuntimeClosureHandle) {
  Harness h;
  const CellRef input = h.cell("input");
  const CellRef child = h.cell("child");
  h.link(input, "d.next", child);
  constexpr std::string_view source =
      "def pure apply($f, $x: Cell): Cell := $x/$f; "
      "def pure relay($step, $x: Cell): Cell := apply($step, $x); "
      "let $step := \\($item) $item/+d.next; "
      "return (relay($step, $input))";
  const Bindings bindings{{"input", {input}}};
  const auto compiled = h.compile(source, bindings);
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  EXPECT_GT(h.invokes(compiled), 0u);
  const auto result = h.run(source, bindings);
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{child}));
}

TEST(VQLRuntimeCallableFramesTest,
     ForwardRecursiveHigherOrderCallKeepsTheSameHandle) {
  Harness h;
  const CellRef a = h.cell("a");
  const CellRef b = h.cell("b");
  const CellRef c = h.cell("c");
  h.link(a, "d.next", b);
  h.link(b, "d.next", c);
  constexpr std::string_view source =
      "def pure walk($item: Cell, $step, $count: Int): Cell := { "
      "if $count > 0 return later($item/$step, $step, $count + -1) "
      "else return $item; }; "
      "def pure later($item: Cell, $step, $count: Int): Cell := "
      "walk($item, $step, $count); "
      "let $step := \\($x) $x/+d.next[1]; walk($input, $step, 2)";
  const Bindings bindings{{"input", {a}}};
  const auto compiled = h.compile(source, bindings);
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  EXPECT_GT(h.invokes(compiled), 0u);
  const auto result = h.run(source, bindings);
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{c}));
}

TEST(VQLRuntimeCallableFramesTest,
     SelfRecursiveNamedFrameForwardsTheRuntimeHandle) {
  Harness h;
  const CellRef a = h.cell("a");
  const CellRef b = h.cell("b");
  const CellRef c = h.cell("c");
  h.link(a, "d.next", b);
  h.link(b, "d.next", c);
  constexpr std::string_view source =
      "def pure walk($item: Cell, $step, $count: Int): Cell := { "
      "if $count > 0 return walk($item/$step, $step, $count + -1) "
      "else return $item; }; let $step := \\($x) $x/+d.next[1]; "
      "walk($input, $step, 2)";
  const auto result = h.run(source, {{"input", {a}}});
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{c}));
  EXPECT_FALSE(h.vm.hasActiveFrames());
}

TEST(VQLRuntimeCallableFramesTest,
     ClosureCreationSnapshotsAliasesButKeepsReferencedCellIdentity) {
  Harness h;
  const CellRef old         = h.cell("old");
  const CellRef replacement = h.cell("replacement");
  constexpr std::string_view source =
      "let $alias := $before; let $step := \\($x) $alias; "
      "let $alias := $after; return (##/$step)";
  const auto result =
      h.run(source, {{"before", {old}}, {"after", {replacement}}});
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{old}));
}

TEST(VQLRuntimeCallableFramesTest,
     EscapingClosureGraphRunsAfterExportAndFreshVmReload) {
  TemporaryDirectory temporary;
  auto scroll = std::make_shared<xanadu::UserPermascroll>(
      xanadu::UserPermascroll::Config{.storageDir = temporary.path / "scroll"});
  {
    Harness h;
    constexpr std::string_view source =
        "def pure make($step) := \\($item) $item/$step; "
        "let $step := \\($x) $x; "
        "let $saved := make($step); return (##/$saved)";
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
  zigzag::vortex::VortexVM vm(core);
  const auto entry =
      arena.linked(core.home(), core.dims().spin, DimVector::POS);
  ASSERT_TRUE(entry);
  const auto execution =
      vm.run(vm.spawnCursor(*entry, "callable-reload"), 100'000);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  auto opcode = entry;
  std::optional<CellRef> halt;
  for (std::size_t count = 0; opcode && count < 100'000; ++count) {
    if (vm.getOpcodeKind(*opcode) == OpcodeKind::Halt) {
      halt = opcode;
      break;
    }
    opcode = arena.linked(*opcode, core.dims().spin, DimVector::POS);
  }
  ASSERT_TRUE(halt);
  const auto resultPorts = core.inputsOf(*halt);
  ASSERT_EQ(resultPorts.size(), 1u);
  EXPECT_EQ(vm.resultCells(resultPorts.front()),
            (std::vector<CellRef>{core.home()}));
  EXPECT_FALSE(vm.hasActiveFrames());
}

TEST(VQLRuntimeCallableFramesTest, StaticPredicateAliasesRetainCalleeScope) {
  Harness h;
  const auto result = h.run(R"(
    def pure apply($step, $threshold: Int): Stream<Int> := $input/$step;
    let $p := %"parent"; $p/d.items%"3"%"7"%"11";
    let $input := $p;
    let $threshold := 1;
    let $step := /+d.items[. > $threshold];
    let $threshold := 9;
    return (apply($step, 5))
  )");
  ASSERT_TRUE(result) << result.error().message;
  ASSERT_EQ(result->size(), 2u);
  EXPECT_EQ(h.arena.textOf((*result)[0]), "7");
  EXPECT_EQ(h.arena.textOf((*result)[1]), "11");
}

TEST(VQLRuntimeCallableFramesTest,
     RuntimeApplicationAndRestDefaultsReachHostAndMesh) {
  constexpr std::string_view source = R"(
    def pure apply($f, $x: Int): Int := $x/$f;
    def pure collect($seed: Int := 9007199254740993,
                     ...$rest: Int := $seed): Stream<Int> := $rest;
    let $f := \($x: Int) $x + 1;
    return (apply($f, collect()))
  )";
  zigzag::vortex::VortexHost host;
  const auto hosted = host.executeVQLChecked(source);
  ASSERT_TRUE(hosted) << hosted.error().message;
  ASSERT_EQ(hosted->size(), 1u);
  EXPECT_EQ(host.core().arena().asInt64(hosted->front()),
            std::optional<std::int64_t>{INT64_C(9007199254740994)});

  namespace mesh    = zigzag::vortex::mesh;
  const auto meshed = mesh::evaluate(mesh::Frontend::Vquery, source,
                                     {.maxCycles      = 100'000,
                                      .maxCells       = 100'000,
                                      .maxSolutions   = 16,
                                      .maxOutputBytes = 1024 * 1024});
  ASSERT_TRUE(meshed) << meshed.error();
  auto &arena = meshed->context->arena;
  std::optional<CellRef> items;
  std::optional<CellRef> value;
  for (const auto &cell : arena.cells()) {
    if (arena.textOf(cell.birthOp) == "d.mesh.items") items = cell.birthOp;
    if (arena.textOf(cell.birthOp) == "d.mesh.value") value = cell.birthOp;
  }
  ASSERT_TRUE(items);
  ASSERT_TRUE(value);
  const auto record = arena.linked(meshed->root, *items, DimVector::POS);
  ASSERT_TRUE(record);
  const auto output = arena.linked(*record, *value, DimVector::POS);
  ASSERT_TRUE(output);
  EXPECT_EQ(arena.asInt64(*output),
            std::optional<std::int64_t>{INT64_C(9007199254740994)});
}

TEST(VQLRuntimeCallableFramesTest,
     UnsupportedValueLambdaSignaturesStillRefuseBeforeWriting) {
  for (const auto source :
       {R"(return (\($x, $y) $x))", R"(return (\($x := 1) $x))",
        R"(return (\(...$xs) $xs))"}) {
    Harness h;
    const auto before   = h.arena.cellCount();
    const auto compiled = h.compile(source);
    EXPECT_FALSE(compiled.success) << source;
    EXPECT_EQ(h.arena.cellCount(), before) << source;
  }
}

} // namespace
} // namespace xanadu::vql
