#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
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
#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/vql/execution.hpp"
#include "common/xanadu/vql/frame_eligibility.hpp"
#include "common/xanadu/vql/parser.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"

namespace xanadu::vql {
namespace {
using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;
using zigzag::vortex::OpcodeKind;

struct TempDir {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("vql_variadic_frames_" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  TempDir() { std::filesystem::create_directories(path); }
  ~TempDir() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

struct Harness {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  MultiStoreCoordinator coordinator{core};
  zigzag::vortex::VortexVM vm{core};
  DimRef next{coordinator.resolveDimension("d.next")};
  DimRef items{coordinator.resolveDimension("d.items")};
  CellRef first{core.home()};
  CellRef second{arena.makeCell("second")};
  CellRef third{arena.makeCell("third")};
  CellRef fourth{arena.makeCell("fourth")};

  Harness() {
    zigzag::expectWritten(arena.link(first, next, DimVector::POS, second));
    zigzag::expectWritten(arena.link(second, next, DimVector::POS, third));
    zigzag::expectWritten(arena.link(third, next, DimVector::POS, fourth));
    zigzag::expectWritten(
        arena.link(core.home(), items, DimVector::POS, second));
    zigzag::expectWritten(arena.link(second, items, DimVector::POS, third));
    zigzag::expectWritten(arena.link(third, items, DimVector::POS, fourth));
  }
};

std::size_t countFunctionInvokes(const CompilationResult &compiled,
                                 const zigzag::vortex::VortexVM &vm,
                                 const zigzag::ArenaManifold &arena,
                                 const std::string_view functionName) {
  std::size_t count = 0;
  const auto marker = "#INVOKE_FRAME " + std::string(functionName);
  for (const auto opcode : compiled.generatedOpcodes) {
    if (vm.getOpcodeKind(opcode) == OpcodeKind::InvokeFrame &&
        arena.textOf(opcode).find(marker) != std::string::npos)
      ++count;
  }
  return count;
}

TEST(VQLVariadicFrameTest, UntypedNamedRestUsesInvokeFrameAndKeepsStreams) {
  Harness h;
  const auto wide = h.arena.makeScalarCell(INT64_C(9007199254740993));
  VQLCompiler compiler(h.coordinator, h.vm);
  const auto source =
      "def pure collect($head: Cell, ...$rest): Stream<Cell> := $rest; "
      "let $rest := ##/d.items; collect(##, $rest, ##/d.items[-1], $rest)";
  const auto compiled = compiler.compile(source);
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  EXPECT_GT(countFunctionInvokes(compiled, h.vm, h.arena, "collect"), 0u);

  ContractBindings bindings{{"large", {wide}}};
  QueryExecutor executor(h.coordinator, h.vm, {}, std::move(bindings));
  const auto query =
      "def pure collect($head: Cell, ...$rest): Stream<Cell> := $rest; "
      "let $rest := ##/d.items; collect(##, $rest, $large, $rest)";
  const std::vector<CellRef> expected{h.second, h.third, h.fourth, wide,
                                      h.second, h.third, h.fourth};
  for (int repeat = 0; repeat < 2; ++repeat) {
    const auto result = executor.execute(query);
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(*result, expected);
  }
  EXPECT_EQ(h.arena.asInt64(wide),
            std::optional<std::int64_t>{INT64_C(9007199254740993)});
}

TEST(VQLVariadicFrameTest, EmptyRestAndNestedArgumentsUseFreshActivations) {
  Harness h;
  QueryExecutor executor(h.coordinator, h.vm);
  const auto empty = executor.execute(
      "def pure collect(...$rest): Stream<Cell> := $rest; collect()");
  ASSERT_TRUE(empty) << empty.error().message;
  EXPECT_TRUE(empty->empty());

  const auto nested = executor.execute(
      "def pure collect($head: Int, ...$rest): Stream<Int> := $rest; "
      "collect(0, collect(0, 1, 2), 3)");
  ASSERT_TRUE(nested) << nested.error().message;
  ASSERT_EQ(nested->size(), 3u);
  EXPECT_EQ(h.arena.asInt64((*nested)[0]), 1);
  EXPECT_EQ(h.arena.asInt64((*nested)[1]), 2);
  EXPECT_EQ(h.arena.asInt64((*nested)[2]), 3);
  EXPECT_FALSE(h.vm.hasActiveFrames());
}

TEST(VQLVariadicFrameTest, HostAndMeshSourceRoutesKeepExactRestValues) {
  constexpr std::string_view source =
      "def pure collect($head: Int, ...$rest): Stream<Int> := $rest; "
      "collect(0, 7, 9007199254740993, 7)";
  const std::vector<std::int64_t> expected{7, INT64_C(9007199254740993), 7};

  zigzag::vortex::VortexHost host;
  const auto hostResult = host.executeVQLChecked(source);
  ASSERT_TRUE(hostResult) << hostResult.error().message;
  ASSERT_EQ(hostResult->size(), expected.size());
  for (std::size_t index = 0; index < expected.size(); ++index)
    EXPECT_EQ(host.core().arena().asInt64((*hostResult)[index]),
              std::optional<std::int64_t>{expected[index]});

  namespace mesh        = zigzag::vortex::mesh;
  const auto meshResult = mesh::evaluate(mesh::Frontend::Vquery, source,
                                         {.maxCycles      = 10000,
                                          .maxCells       = 10000,
                                          .maxSolutions   = 16,
                                          .maxOutputBytes = 1024 * 1024});
  ASSERT_TRUE(meshResult) << meshResult.error();
  auto &arena = meshResult->context->arena;
  std::optional<zigzag::CellRef> items;
  std::optional<zigzag::CellRef> value;
  for (const auto &cell : arena.cells()) {
    if (arena.textOf(cell.birthOp) == "d.mesh.items") items = cell.birthOp;
    if (arena.textOf(cell.birthOp) == "d.mesh.value") value = cell.birthOp;
  }
  ASSERT_TRUE(items);
  ASSERT_TRUE(value);
  std::vector<std::int64_t> meshValues;
  auto record = arena.linked(meshResult->root, *items, DimVector::POS);
  while (record) {
    const auto output = arena.linked(*record, *value, DimVector::POS);
    ASSERT_TRUE(output);
    const auto integer = arena.asInt64(*output);
    ASSERT_TRUE(integer);
    meshValues.push_back(*integer);
    record = arena.linked(*record, *items, DimVector::POS);
  }
  EXPECT_EQ(meshValues, expected);
}

TEST(VQLVariadicFrameTest, DefaultsAndUfcsKeepTheirPriorArgumentOrder) {
  Harness h;
  QueryExecutor executor(h.coordinator, h.vm);
  const auto defaults = executor.execute(
      "def pure choose($first: Int := 7, $second: Int := $first, "
      "...$rest): Int := $second; choose()");
  ASSERT_TRUE(defaults) << defaults.error().message;
  ASSERT_EQ(defaults->size(), 1u);
  EXPECT_EQ(h.arena.asInt64(defaults->front()), 7);

  const auto callerDefaultSource =
      "def pure choose($head: Cell := $rest, ...$rest): Cell := $head; "
      "choose()";
  CompilationOptions options;
  options.initialBindings.emplace("rest", std::vector<CellRef>{h.fourth});
  VQLCompiler defaultCompiler(h.coordinator, h.vm);
  const auto defaultCompiled =
      defaultCompiler.compile(callerDefaultSource, options);
  ASSERT_TRUE(defaultCompiled.success) << defaultCompiled.errorMessage;
  EXPECT_GT(countFunctionInvokes(defaultCompiled, h.vm, h.arena, "choose"), 0u);
  QueryExecutor capturedExecutor(h.coordinator, h.vm, {},
                                 {{"rest", {h.fourth}}});
  const auto callerDefault = capturedExecutor.execute(callerDefaultSource);
  ASSERT_TRUE(callerDefault) << callerDefault.error().message;
  EXPECT_EQ(*callerDefault, (std::vector<CellRef>{h.fourth}));

  VQLCompiler compiler(h.coordinator, h.vm);
  const auto source =
      "def pure collect($head: Cell, ...$rest): Stream<Cell> := $rest; "
      "##/d.items/collect(##/d.items[-1])";
  const auto compiled = compiler.compile(source);
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  EXPECT_GT(countFunctionInvokes(compiled, h.vm, h.arena, "collect"), 0u);
  const auto ufcs = executor.execute(source);
  ASSERT_TRUE(ufcs) << ufcs.error().message;
  EXPECT_EQ(*ufcs, (std::vector<CellRef>{h.fourth, h.fourth, h.fourth}));
}

TEST(VQLVariadicFrameTest, TypedRestUsesNativeFramesAndChecksEachActual) {
  Harness h;
  VQLCompiler compiler(h.coordinator, h.vm);
  const auto source = "def pure collect(...$rest: Int): Stream<Int> := $rest; "
                      "collect(1, 2)";
  const auto query  = Parser(source).parseQuery();
  const auto eligibility = analyzeFrameEligibility(query);
  ASSERT_TRUE(eligibility) << eligibility.error().message;
  EXPECT_TRUE(eligibility->reusableDefinitions.contains("collect"));

  const auto compiled = compiler.compile(query);
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  EXPECT_GT(countFunctionInvokes(compiled, h.vm, h.arena, "collect"), 0u);
  QueryExecutor executor(h.coordinator, h.vm);
  const auto valid = executor.execute(source);
  ASSERT_TRUE(valid) << valid.error().message;
  ASSERT_EQ(valid->size(), 2u);
  EXPECT_EQ(h.arena.asInt64((*valid)[0]), 1);
  EXPECT_EQ(h.arena.asInt64((*valid)[1]), 2);

  const auto invalid =
      executor.execute("def pure collect(...$rest: Int): Stream<Int> := $rest; "
                       "collect(1, \"bad\")");
  ASSERT_TRUE(invalid) << invalid.error().message;
  EXPECT_TRUE(invalid->empty());
}

TEST(VQLVariadicFrameTest, FixedMismatchSkipsBodyAndReturnPromiseStillChecks) {
  Harness h;
  const auto sentinel = h.arena.makeCell("body ran");
  QueryExecutor executor(h.coordinator, h.vm, {}, {{"sentinel", {sentinel}}});
  const auto mismatch = executor.execute(
      "def pure guarded($value: Int, ...$rest): Cell := $sentinel; "
      "guarded(\"bad\")");
  ASSERT_TRUE(mismatch) << mismatch.error().message;
  EXPECT_TRUE(mismatch->empty());

  const auto badReturn =
      executor.execute("def pure broken(...$rest): String := $rest; broken(7)");
  ASSERT_FALSE(badReturn);
  EXPECT_EQ(badReturn.error().category,
            QueryExecutionError::Category::TypeContract);
  EXPECT_FALSE(h.vm.hasActiveFrames());
}

TEST(VQLVariadicFrameTest, RecursiveFramesExportAndReloadWithoutCompilerState) {
  TempDir temporary;
  auto scroll = std::make_shared<xanadu::UserPermascroll>(
      xanadu::UserPermascroll::Config{.storageDir = temporary.path / "scroll"});
  {
    Harness h;
    const auto source =
        "def pure carry($node: Cell, ...$rest): Stream<Cell> := { "
        "if $node/+d.next return carry($node/+d.next[1], $rest) "
        "else return $rest; }; carry(##, ##/d.items)";
    VQLCompiler compiler(h.coordinator, h.vm);
    const auto compiled = compiler.compile(source);
    ASSERT_TRUE(compiled.success) << compiled.errorMessage;
    EXPECT_GT(countFunctionInvokes(compiled, h.vm, h.arena, "carry"), 0u);
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
  const auto cursor    = vm.spawnCursor(*entry, "reloaded-vql-variadic-frame");
  const auto execution = vm.run(cursor, 10'000);
  ASSERT_TRUE(execution.success) << execution.errorMessage;

  auto halt = *entry;
  auto next = arena.linked(halt, core.dims().spin, DimVector::POS);
  while (next && vm.getOpcodeKind(*next) != OpcodeKind::Halt) {
    halt = *next;
    next = arena.linked(halt, core.dims().spin, DimVector::POS);
  }
  ASSERT_TRUE(next);
  const auto resultPorts = core.inputsOf(*next);
  ASSERT_EQ(resultPorts.size(), 1u);
  const auto results = vm.resultCells(resultPorts.front());
  ASSERT_EQ(results.size(), 3u);
  EXPECT_EQ(arena.textOf(results[0]), "second");
  EXPECT_EQ(arena.textOf(results[1]), "third");
  EXPECT_EQ(arena.textOf(results[2]), "fourth");
  EXPECT_FALSE(vm.hasActiveFrames());
}

TEST(VQLVariadicFrameTest, RecursiveDepthFailureCanRetryAfterRaisingLimit) {
  Harness h;
  for (int i = 0; i < 12; ++i) {
    const auto tail = zigzag::rankTail(h.arena, h.core.home(), h.next);
    const auto cell = h.arena.makeCell("long chain");
    zigzag::expectWritten(h.arena.link(tail, h.next, DimVector::POS, cell));
  }
  h.vm.setFrameLimits({.maxDepth = 6});
  QueryExecutor executor(h.coordinator, h.vm);
  const auto query = "def pure carry($node: Cell, ...$rest): Stream<Cell> := { "
                     "if $node/+d.next return carry($node/+d.next[1], $rest) "
                     "else return $rest; }; carry(##, ##/d.items)";
  const auto refused = executor.execute(query);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().category, QueryExecutionError::Category::Budget);
  EXPECT_NE(refused.error().message.find("frame depth"), std::string::npos);

  h.vm.setFrameLimits({.maxDepth = 32});
  const auto retried = executor.execute(query);
  ASSERT_TRUE(retried) << retried.error().message;
  EXPECT_EQ(*retried, (std::vector<CellRef>{h.second, h.third, h.fourth}));
  EXPECT_FALSE(h.vm.hasActiveFrames());
}

TEST(VQLVariadicFrameTest,
     CallerRestPreparationConsumesBudgetBeforeFixedTypeFiltering) {
  Harness h;
  auto tail = zigzag::rankTail(h.arena, h.core.home(), h.items);
  for (int i = 0; i < 1000; ++i) {
    const auto cell = h.arena.makeCell("wide tail");
    zigzag::expectWritten(h.arena.link(tail, h.items, DimVector::POS, cell));
    tail = cell;
  }

  const auto query = "def pure guard($value: Int, ...$rest): Int := 42; "
                     "guard(\"bad\")";
  QueryExecutor limited(h.coordinator, h.vm, {.maxCycles = 500});
  const auto empty = limited.execute(query);
  ASSERT_TRUE(empty) << empty.error().message;
  EXPECT_TRUE(empty->empty());

  const auto withLargeRest =
      "def pure guard($value: Int, ...$rest): Int := 42; "
      "guard(\"bad\", ##/d.items)";
  const auto exhausted = limited.execute(withLargeRest);
  ASSERT_FALSE(exhausted);
  EXPECT_EQ(exhausted.error().category, QueryExecutionError::Category::Budget);
  EXPECT_FALSE(h.vm.hasActiveFrames());

  QueryExecutor retry(h.coordinator, h.vm, {.maxCycles = 50'000});
  const auto recovered = retry.execute(withLargeRest);
  ASSERT_TRUE(recovered) << recovered.error().message;
  EXPECT_TRUE(recovered->empty());
  EXPECT_FALSE(h.vm.hasActiveFrames());
}

} // namespace
} // namespace xanadu::vql
