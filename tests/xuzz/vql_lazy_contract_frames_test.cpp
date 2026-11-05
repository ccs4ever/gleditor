/**
 * @file vql_lazy_contract_frames_test.cpp
 * @brief Lazy structural defaults through native VQL activation frames.
 */
#include <gtest/gtest.h>

#include <chrono>
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

struct TempDirectory {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("vql_lazy_contract_frames_" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));

  TempDirectory() { std::filesystem::create_directories(path); }
  ~TempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

struct Harness {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  MultiStoreCoordinator coordinator{core};
  zigzag::vortex::VortexVM vm{core};

  void link(const CellRef owner, const std::string_view name,
            const CellRef value, const DimVector direction = DimVector::POS) {
    const DimRef dimension = coordinator.resolveDimension(name);
    zigzag::expectWritten(arena.link(owner, dimension, direction, value));
  }

  std::expected<std::vector<CellRef>, QueryExecutionError>
  run(const std::string_view source, ContractBindings bindings = {},
      const std::vector<CellRef> &context = {}) {
    QueryExecutor executor(coordinator, vm, {}, std::move(bindings));
    return executor.execute(source, context);
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
  std::size_t count = 0;
  for (const CellRef opcode : program.generatedOpcodes)
    count += h.vm.getOpcodeKind(opcode) == kind;
  return count;
}

bool hasFrameInvocationFor(const Harness &h, const CompilationResult &program,
                           const std::string_view functionName) {
  const std::string label = "#INVOKE_FRAME " + std::string(functionName);
  for (const CellRef opcode : program.generatedOpcodes) {
    if (h.arena.textOf(opcode) == label) return true;
  }
  return false;
}

TEST(VQLLazyContractFramesTest,
     ComputedPathCallAndBinaryDefaultsStayInsideNativeOuterFrames) {
  Harness h;
  const CellRef item      = h.arena.makeCell("item");
  const CellRef pathValue = h.arena.makeCell("path value");
  h.link(item, "d.fallback", pathValue);

  constexpr std::string_view source =
      "def pure called(): Cell := \"called\"; "
      "def pure by_path($item: Cell { +d.value: $value: Cell := "
      "./+d.fallback }): Cell := $value; "
      "def pure by_call($item: Cell { +d.value: $value: Cell := "
      "called() }): Cell := $value; "
      "def pure by_math($item: Cell { +d.value: $value: Int := 7 + 2 }): "
      "Int := $value; return by_path($item), by_call($item), "
      "by_math($item)";
  const auto compiled = h.compile(source, {{"item", {item}}});
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  EXPECT_TRUE(hasFrameInvocationFor(h, compiled, "by_path"));
  EXPECT_TRUE(hasFrameInvocationFor(h, compiled, "by_call"));
  EXPECT_TRUE(hasFrameInvocationFor(h, compiled, "by_math"));
  EXPECT_GT(opcodeCount(h, compiled, OpcodeKind::ScopedChoice), 0U);
  EXPECT_GT(opcodeCount(h, compiled, OpcodeKind::VqlFieldProbe), 0U);

  const auto result = h.run(source, {{"item", {item}}});
  ASSERT_TRUE(result) << result.error().message;
  ASSERT_EQ(result->size(), 3U);
  EXPECT_EQ((*result)[0], pathValue);
  EXPECT_EQ(h.arena.textOf((*result)[1]), "called");
  EXPECT_EQ(h.arena.asInt64((*result)[2]), std::optional<std::int64_t>{9});
}

TEST(VQLLazyContractFramesTest,
     PrematchCaptureBeatsSamePatternShadowAndPriorFrameParameterIsVisible) {
  Harness h;
  const CellRef item    = h.arena.makeCell("item");
  const CellRef present = h.arena.makeCell("same-pattern value");
  const CellRef caller  = h.arena.makeCell("caller value");
  h.link(item, "d.present", present);

  constexpr std::string_view shadowSource =
      "def pure identity($value: Cell): Cell := $value; "
      "def pure choose($item: Cell { +d.present: $outside: Cell, "
      "+d.missing: $value: Cell := identity($outside) }): Cell := $value; "
      "choose($item)";
  const auto shadow =
      h.run(shadowSource, {{"item", {item}}, {"outside", {caller}}});
  ASSERT_TRUE(shadow) << shadow.error().message;
  EXPECT_EQ(*shadow, (std::vector<CellRef>{caller}));
  const auto compiled =
      h.compile(shadowSource, {{"item", {item}}, {"outside", {caller}}});
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  EXPECT_TRUE(hasFrameInvocationFor(h, compiled, "choose"));

  const CellRef tagged = h.arena.makeCell("earlier formal capture");
  h.link(item, "d.tag", tagged);
  constexpr std::string_view priorSource =
      "def pure identity($value: Cell): Cell := $value; "
      "def pure choose($first: Cell { +d.tag: $tag: Cell }, "
      "$second: Cell { +d.missing: $value: Cell := identity($tag) }): "
      "Cell := $value; "
      "choose($item, $item)";
  const auto prior = h.run(priorSource, {{"item", {item}}});
  ASSERT_TRUE(prior) << prior.error().message;
  EXPECT_EQ(*prior, (std::vector<CellRef>{tagged}));
  const auto priorCompiled = h.compile(priorSource, {{"item", {item}}});
  ASSERT_TRUE(priorCompiled.success) << priorCompiled.errorMessage;
  EXPECT_TRUE(hasFrameInvocationFor(h, priorCompiled, "choose"));
}

TEST(VQLLazyContractFramesTest,
     GenericStreamValidationRollsBackCapturesAndDefaultsOnSuccess) {
  Harness h;
  const CellRef first  = h.arena.makeCell("first");
  const CellRef second = h.arena.makeCell("second");
  const CellRef third  = h.arena.makeCell("third");
  constexpr std::string_view source =
      "def pure temporary(): Cell := \"temporary\"; "
      "def pure verify($items: Stream<Cell { +d.element: "
      "$capture: Cell := temporary() }>): Stream<Cell> := $items; "
      "verify($items)";
  const auto compiled = h.compile(source, {{"items", {first, second}}});
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  EXPECT_TRUE(hasFrameInvocationFor(h, compiled, "verify"));
  EXPECT_GT(opcodeCount(h, compiled, OpcodeKind::ScopedChoice), 0U);

  const auto result = h.run(source, {{"items", {first, second, third}}});
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{first, second, third}));
  EXPECT_FALSE(h.core.findDimension("d.element"));
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);
}

TEST(VQLLazyContractFramesTest,
     NestedOwnerAndContextCapturesRestoreAfterInnerDefaultCall) {
  Harness h;
  const CellRef outer          = h.arena.makeCell("outer");
  const CellRef inner          = h.arena.makeCell("inner");
  const CellRef innerFallback  = h.arena.makeCell("inner fallback");
  const CellRef outerFallback  = h.arena.makeCell("outer fallback");
  const CellRef callerContext  = h.arena.makeCell("caller context");
  const CellRef callerFallback = h.arena.makeCell("caller fallback");
  h.link(outer, "d.inner", inner);
  h.link(inner, "d.fallback", innerFallback);
  h.link(outer, "d.fallback", outerFallback);
  h.link(callerContext, "d.fallback", callerFallback);

  constexpr std::string_view source =
      "def pure fallback(): Cell := ./+d.fallback; "
      "def pure choose($outer: Cell { +d.inner: Cell { "
      "+d.value: $nested: Cell := ./+d.fallback }, "
      "+d.after: $after: Cell := fallback() }): Stream<Cell> := "
      "{ return $nested, $after; }; return choose($outer), "
      "./+d.fallback";
  const auto compiled = h.compile(source, {{"outer", {outer}}});
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  EXPECT_TRUE(hasFrameInvocationFor(h, compiled, "choose"));
  EXPECT_GT(opcodeCount(h, compiled, OpcodeKind::ScopedChoice), 0U);

  const auto result = h.run(source, {{"outer", {outer}}}, {callerContext});
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{innerFallback, outerFallback,
                                           callerFallback}));
  EXPECT_NE(innerFallback, outerFallback);
}

TEST(VQLLazyContractFramesTest,
     NativeDefaultsMintFreshLiteralPerLoopCallAndMismatchesFilterEmpty) {
  Harness h;
  const CellRef first  = h.arena.makeCell("first");
  const CellRef second = h.arena.makeCell("second");
  constexpr std::string_view loopSource =
      "def pure called(): Cell := \"force graph path\"; "
      "def pure choose($item: Cell { +d.value: $value: Int := 7, "
      "+d.force: Cell := called() }): Int := $value; "
      "for $item in $items return choose($item)";
  const auto compiled = h.compile(loopSource, {{"items", {first, second}}});
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  EXPECT_TRUE(hasFrameInvocationFor(h, compiled, "choose"));
  const auto result = h.run(loopSource, {{"items", {first, second}}});
  ASSERT_TRUE(result) << result.error().message;
  ASSERT_EQ(result->size(), 2U);
  EXPECT_NE((*result)[0], (*result)[1]);
  EXPECT_EQ(h.arena.asInt64((*result)[0]), std::optional<std::int64_t>{7});
  EXPECT_EQ(h.arena.asInt64((*result)[1]), std::optional<std::int64_t>{7});

  const CellRef wrong = h.arena.makeCell("wrong type");
  h.link(first, "d.required", wrong);
  constexpr std::string_view mismatchSource =
      "def pure fallback(): Int := 1; "
      "def pure choose($item: Cell { +d.required: $value: Int, "
      "+d.optional: Int := fallback() }): Int := $value; choose($item)";
  const auto mismatchCompiled = h.compile(mismatchSource, {{"item", {first}}});
  ASSERT_TRUE(mismatchCompiled.success) << mismatchCompiled.errorMessage;
  EXPECT_TRUE(hasFrameInvocationFor(h, mismatchCompiled, "choose"));
  const auto mismatch = h.run(mismatchSource, {{"item", {first}}});
  ASSERT_TRUE(mismatch) << mismatch.error().message;
  EXPECT_TRUE(mismatch->empty());
}

TEST(VQLLazyContractFramesTest,
     StructuralDefaultCanRecurseAndMissingTailFiltersWithoutPostcondition) {
  Harness h;
  const CellRef root   = h.arena.makeCell("root");
  const CellRef child  = h.arena.makeCell("child");
  const CellRef leaf   = h.arena.makeCell("leaf");
  const CellRef answer = h.arena.makeCell("answer");
  h.link(root, "d.next", child);
  h.link(child, "d.next", leaf);
  h.link(leaf, "d.value", answer);

  constexpr std::string_view source =
      "def pure down($item: Cell { +d.next: $next: Cell? }, "
      "$again: Cell { +d.value: $value: Cell := down($next, $next) }): "
      "Cell := $value; down($root, $root)";
  const auto compiled = h.compile(source, {{"root", {root}}});
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  EXPECT_TRUE(hasFrameInvocationFor(h, compiled, "down"));
  EXPECT_GT(opcodeCount(h, compiled, OpcodeKind::ScopedChoice), 0U);
  EXPECT_GT(opcodeCount(h, compiled, OpcodeKind::VqlFieldProbe), 0U);
  const auto result = h.run(source, {{"root", {root}}});
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{answer}));

  const CellRef missingTail = h.arena.makeCell("missing tail");
  h.link(child, "d.next", missingTail);
  const auto empty = h.run(source, {{"root", {root}}});
  ASSERT_TRUE(empty) << empty.error().message;
  EXPECT_TRUE(empty->empty());
}

TEST(VQLLazyContractFramesTest,
     MutualRecursionThroughComputedDefaultsKeepsNativeFrameCalls) {
  Harness h;
  const CellRef root   = h.arena.makeCell("root");
  const CellRef child  = h.arena.makeCell("child");
  const CellRef leaf   = h.arena.makeCell("leaf");
  const CellRef answer = h.arena.makeCell("answer");
  h.link(root, "d.next", child);
  h.link(child, "d.next", leaf);
  h.link(leaf, "d.value", answer);

  constexpr std::string_view source =
      "def pure even($item: Cell { +d.next: $next: Cell? }, "
      "$again: Cell { +d.value: $value: Cell := odd($next, $next) }): "
      "Cell := $value; "
      "def pure odd($item: Cell { +d.next: $next: Cell? }, "
      "$again: Cell { +d.value: $value: Cell := even($next, $next) }): "
      "Cell := $value; even($root, $root)";
  const auto compiled = h.compile(source, {{"root", {root}}});
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  EXPECT_TRUE(hasFrameInvocationFor(h, compiled, "even"));
  EXPECT_TRUE(hasFrameInvocationFor(h, compiled, "odd"));
  EXPECT_GT(opcodeCount(h, compiled, OpcodeKind::ScopedChoice), 0U);
  EXPECT_GT(opcodeCount(h, compiled, OpcodeKind::VqlFieldProbe), 0U);

  const auto result = h.run(source, {{"root", {root}}});
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{answer}));
}

TEST(VQLLazyContractFramesTest,
     NativeFrameReturnContractEvaluatesComputedDefaultsOnlyWhenAbsent) {
  Harness h;
  const CellRef missing = h.arena.makeCell("missing field");
  const CellRef present = h.arena.makeCell("present field");
  const CellRef seven   = h.arena.makeScalarCell(std::int64_t{7});
  h.link(present, "d.value", seven);

  constexpr std::string_view source =
      "def pure bad(): Cell := \"not an integer\"; "
      "def pure make($item: Cell): Cell { +d.value: Int := bad() } := "
      "$item; make($item)";
  const auto compiled = h.compile(source, {{"item", {missing}}});
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  EXPECT_TRUE(hasFrameInvocationFor(h, compiled, "make"));
  EXPECT_GT(opcodeCount(h, compiled, OpcodeKind::ScopedChoice), 0U);
  EXPECT_GT(opcodeCount(h, compiled, OpcodeKind::VqlFieldProbe), 0U);

  const auto failed = h.run(source, {{"item", {missing}}});
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error().category,
            QueryExecutionError::Category::TypeContract);
  EXPECT_NE(failed.error().message.find("make"), std::string::npos);
  EXPECT_EQ(h.vm.choiceDepth(), 0U);
  EXPECT_FALSE(h.vm.hasActiveFrames());

  const auto presentResult = h.run(source, {{"item", {present}}});
  ASSERT_TRUE(presentResult) << presentResult.error().message;
  EXPECT_EQ(*presentResult, (std::vector<CellRef>{present}));
}

TEST(VQLLazyContractFramesTest,
     RecursiveDefaultRefusesAtConfiguredBoundAndNextRequestRecovers) {
  Harness h;
  const CellRef item = h.arena.makeCell("item");
  h.vm.setFrameLimits({.maxDepth = 4});
  constexpr std::string_view recursive =
      "def pure loop($item: Cell { +d.value: $value: Cell := "
      "loop($item) }): Cell := $value; loop($item)";
  const auto failed = h.run(recursive, {{"item", {item}}});
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error().category, QueryExecutionError::Category::Budget);
  EXPECT_EQ(h.vm.choiceDepth(), 0U);
  EXPECT_FALSE(h.vm.hasActiveFrames());
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);

  const auto recovered = h.run("def pure okay(): Int := 7; okay()");
  ASSERT_TRUE(recovered) << recovered.error().message;
  ASSERT_EQ(recovered->size(), 1U);
  EXPECT_EQ(h.arena.asInt64(recovered->front()),
            std::optional<std::int64_t>{7});
}

TEST(VQLLazyContractFramesTest,
     RecursiveStructuralDefaultExportsAndRunsAfterSourceDestruction) {
  TempDirectory temporary;
  auto scroll = std::make_shared<UserPermascroll>(
      UserPermascroll::Config{.storageDir = temporary.path / "scroll"});
  {
    Store store(scroll);
    auto head              = store.sliceGenesis({});
    const auto rootDim     = store.makeDimension(head, "d.root");
    head                   = rootDim.version;
    const auto nextDim     = store.makeDimension(head, "d.next");
    head                   = nextDim.version;
    const auto valueDim    = store.makeDimension(head, "d.value");
    head                   = valueDim.version;
    const auto expectedDim = store.makeDimension(head, "d.expected");
    head                   = expectedDim.version;

    const auto rootVersion   = store.makeCell(head, "root");
    const CellRef root       = *store.cellRefOf(rootVersion);
    head                     = rootVersion;
    const auto childVersion  = store.makeCell(head, "child");
    const CellRef child      = *store.cellRefOf(childVersion);
    head                     = childVersion;
    const auto leafVersion   = store.makeCell(head, "leaf");
    const CellRef leaf       = *store.cellRefOf(leafVersion);
    head                     = leafVersion;
    const auto answerVersion = store.makeCell(head, "answer");
    const CellRef answer     = *store.cellRefOf(answerVersion);
    head                     = answerVersion;
    head = store.setLink(head, *store.homeCell(), rootDim.dim, DimVector::POS,
                         root);
    head = store.setLink(head, root, nextDim.dim, DimVector::POS, child);
    head = store.setLink(head, child, nextDim.dim, DimVector::POS, leaf);
    head = store.setLink(head, leaf, valueDim.dim, DimVector::POS, answer);
    head = store.setLink(head, *store.homeCell(), expectedDim.dim,
                         DimVector::POS, answer);

    Harness h;
    constexpr std::string_view source =
        "def pure down($item: Cell { +d.next: $next: Cell? }, "
        "$again: Cell { +d.value: $value: Cell := down($next, $next) }): "
        "Cell := $value; "
        "down(##/d.root[1], ##/d.root[1])";
    VQLCompiler compiler(h.coordinator, h.vm);
    const auto compiled = compiler.compile(source);
    ASSERT_TRUE(compiled.success) << compiled.errorMessage;
    EXPECT_TRUE(hasFrameInvocationFor(h, compiled, "down"));
    compiler.exportToStore(store);
    store.save((temporary.path / "program").string());
  }

  Store store(scroll);
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
  const auto cursor = vm.spawnCursor(*entry, "lazy-contract-recursive-reload");
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
  const DimRef expectedDim = coordinator.resolveDimension("d.expected");
  const auto expected = arena.linked(core.home(), expectedDim, DimVector::POS);
  ASSERT_TRUE(expected);
  EXPECT_EQ(results.front(), *expected);
  EXPECT_FALSE(vm.hasActiveFrames());
}

TEST(VQLLazyContractFramesTest, HostAndMeshVqueryRoutesRunNativeDefaultGraph) {
  constexpr std::string_view source =
      "def pure fallback(): Cell := \"routed\"; "
      "def pure choose($item: Cell { +d.value: $value: Cell := "
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

} // namespace
} // namespace xanadu::vql
