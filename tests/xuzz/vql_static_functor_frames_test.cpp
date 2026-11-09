#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
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
#include "common/xanadu/vql/frame_eligibility.hpp"
#include "common/xanadu/vql/functor_capture.hpp"
#include "common/xanadu/vql/parser.hpp"
#include "common/xanadu/vql/static_functor_program.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu::vql {
namespace {
using zigzag::CellRef;
using zigzag::DimVector;
using zigzag::vortex::OpcodeKind;
using Result = std::expected<std::vector<CellRef>, std::string>;
struct Harness {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  MultiStoreCoordinator coordinator{core};
  zigzag::vortex::VortexVM vm{core};
  CompilationResult compiled;

  CellRef cell(const std::string_view text) { return arena.makeCell(text); }

  CellRef integer(const std::int64_t value) {
    return arena.makeScalarCell(value);
  }

  void link(const CellRef from, const std::string_view dimension,
            const CellRef to) {
    zigzag::expectWritten(arena.link(
        from, coordinator.resolveDimension(dimension), DimVector::POS, to));
  }

  Result
  run(const std::string_view source,
      std::unordered_map<std::string, std::vector<CellRef>> bindings = {}) {
    VQLCompiler compiler(coordinator, vm);
    compiled =
        compiler.compile(source, {.initialBindings = std::move(bindings)});
    if (!compiled.success) return std::unexpected(compiled.errorMessage);
    if (!compiled.cursor)
      return std::unexpected("compiled query has no cursor");
    const auto execution = vm.run(*compiled.cursor, 100'000);
    if (!execution.success) {
      vm.abortScopedChoices(*compiled.cursor);
      vm.setCursorOpcode(*compiled.cursor, std::nullopt);
      return std::unexpected(execution.errorMessage);
    }
    std::vector<CellRef> result;
    for (const CellRef slot : compiled.resultSlots) {
      const auto values = vm.resultCells(slot);
      result.insert(result.end(), values.begin(), values.end());
    }
    vm.setCursorOpcode(*compiled.cursor, std::nullopt);
    return result;
  }

  std::size_t count(const OpcodeKind kind) const {
    std::size_t result = 0;
    for (const CellRef opcode : compiled.generatedOpcodes)
      result += vm.getOpcodeKind(opcode) == kind;
    return result;
  }
};

struct TemporaryDirectory {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("vql_static_functor_" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  TemporaryDirectory() { std::filesystem::create_directories(path); }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

std::set<CellRef> descriptors(const Harness &h, const std::string_view name) {
  std::set<CellRef> result;
  const std::string marker = "#INVOKE_FRAME " + std::string(name);
  for (const auto op : h.compiled.generatedOpcodes) {
    if (h.vm.getOpcodeKind(op) != OpcodeKind::InvokeFrame ||
        !h.arena.textOf(op).starts_with(marker))
      continue;
    const auto inputs = h.core.inputsOf(op);
    if (!inputs.empty()) result.insert(inputs.front());
  }
  return result;
}

struct Items {
  CellRef parent;
  CellRef three;
  CellRef seven;
  CellRef eleven;
};

Items items(Harness &h) {
  const Items result{h.cell("parent"), h.integer(3), h.integer(7),
                     h.integer(11)};
  h.link(result.parent, "d.items", result.three);
  h.link(result.three, "d.items", result.seven);
  h.link(result.seven, "d.items", result.eleven);
  return result;
}

TEST(VQLStaticFunctorFramesTest, PredicateAliasesResolveAgainstCalleeFormals) {
  Harness h;
  const auto data   = items(h);
  const auto result = h.run(R"(
    def pure apply($step, $threshold: Int): Stream<Int> := $input/$step;
    let $threshold := 1;
    let $step := /+d.items[. > $threshold];
    let $threshold := 9;
    return (apply($step, 5))
  )",
                            {{"input", {data.parent}}});
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, (std::vector<CellRef>{data.seven, data.eleven}));
  EXPECT_EQ(descriptors(h, "apply").size(), 1u);
}

TEST(VQLStaticFunctorFramesTest,
     BodyLocalShadowingTakesEffectAtEachApplication) {
  Harness h;
  const auto data   = items(h);
  const auto result = h.run(R"(
    def pure apply($step): Stream<Int> := {
      let $before := $input/$step;
      let $threshold := 9;
      return $before, $input/$step;
    };
    let $threshold := 5;
    let $step := /+d.items[. > $threshold];
    return (apply($step))
  )",
                            {{"input", {data.parent}}});
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result,
            (std::vector<CellRef>{data.seven, data.eleven, data.eleven}));
  EXPECT_EQ(descriptors(h, "apply").size(), 1u);
}

TEST(VQLStaticFunctorFramesTest, FormalShadowsAnUnrelatedCallerStaticAlias) {
  Harness h;
  const auto data   = items(h);
  const auto result = h.run(R"(
    def pure apply($step, $threshold: Int): Stream<Int> := $input/$step;
    let $threshold := /+d.unrelated;
    let $step := /+d.items[. > $threshold];
    return (apply($step, 5))
  )",
                            {{"input", {data.parent}}});
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, (std::vector<CellRef>{data.seven, data.eleven}));
  EXPECT_EQ(descriptors(h, "apply").size(), 1u);
}

TEST(VQLStaticFunctorFramesTest,
     PriorLocalShadowsAnUnrelatedCallerStaticAlias) {
  Harness h;
  const auto data   = items(h);
  const auto result = h.run(R"(
    def pure apply($step): Stream<Int> := {
      let $threshold := 5;
      return $input/$step;
    };
    let $threshold := /+d.unrelated;
    let $step := /+d.items[. > $threshold];
    return (apply($step))
  )",
                            {{"input", {data.parent}}});
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, (std::vector<CellRef>{data.seven, data.eleven}));
  EXPECT_EQ(descriptors(h, "apply").size(), 1u);
}

TEST(VQLStaticFunctorFramesTest, AliasValuesDoNotCreateNewStructuralVariants) {
  Harness h;
  const auto data   = items(h);
  const auto result = h.run(R"(
    def pure apply($step): Stream<Int> := $input/$step;
    let $threshold := 5;
    let $step := /+d.items[. > $threshold];
    let $first := apply($step);
    let $threshold := 9;
    return $first, (apply($step))
  )",
                            {{"input", {data.parent}}});
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result,
            (std::vector<CellRef>{data.seven, data.eleven, data.eleven}));
  EXPECT_EQ(descriptors(h, "apply").size(), 1u);
}

TEST(VQLStaticFunctorFramesTest,
     ForwardingOnlyFramesTransportCurrentAliasScope) {
  for (const bool localShadow : {false, true}) {
    Harness h;
    const auto data = items(h);
    const std::string source =
        "def pure apply($step): Stream<Int> := $input/$step; "
        "def pure relay($step): Stream<Int> := { " +
        std::string(localShadow ? "let $threshold := 9; " : "") +
        "return apply($step); }; "
        "let $threshold := 5; let $step := /+d.items[. > $threshold]; "
        "return (relay($step))";
    const auto result = h.run(source, {{"input", {data.parent}}});
    ASSERT_TRUE(result) << result.error();
    const auto expected = localShadow
                              ? std::vector<CellRef>{data.eleven}
                              : std::vector<CellRef>{data.seven, data.eleven};
    EXPECT_EQ(*result, expected);
    EXPECT_EQ(descriptors(h, "apply").size(), 1u);
    EXPECT_EQ(descriptors(h, "relay").size(), 1u);
  }
}

constexpr std::string_view recursiveSource = R"(
  def pure walk($node: Cell, $threshold: Int, $n: Int, $route): Cell := {
    if $n > 0 return walk($node/$route, $threshold, $n + -1, $route)
    else return $node;
  };
  let $threshold := 1;
  let $route := /+d.next[. > $threshold][1];
  let $threshold := 99;
  return (walk($input, 5, 2, $route))
)";

TEST(VQLStaticFunctorFramesTest, RecursiveForwardingReusesTheSymbolicProgram) {
  Harness h;
  const auto data = items(h);
  h.link(data.three, "d.next", data.seven);
  h.link(data.seven, "d.next", data.eleven);
  const auto result = h.run(recursiveSource, {{"input", {data.three}}});
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, (std::vector<CellRef>{data.eleven}));
  EXPECT_EQ(descriptors(h, "walk").size(), 1u);
  EXPECT_FALSE(h.vm.hasActiveFrames());
}

TEST(VQLStaticFunctorFramesTest,
     MutualForwardingKeepsDifferentFormalPositions) {
  Harness h;
  const auto data = items(h);
  h.link(data.three, "d.next", data.seven);
  h.link(data.seven, "d.next", data.eleven);
  const auto result = h.run(R"(
    def pure walk($node: Cell, $threshold: Int, $n: Int, $route): Cell := {
      if $n > 0 return later($route, $node/$route, $threshold, $n + -1)
      else return $node;
    };
    def pure later($route, $node: Cell, $threshold: Int, $n: Int): Cell :=
      walk($node, $threshold, $n, $route);
    let $route := /+d.next[. > $threshold][1];
    return (walk($input, 5, 2, $route))
  )",
                            {{"input", {data.three}}});
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, (std::vector<CellRef>{data.eleven}));
  EXPECT_EQ(descriptors(h, "walk").size(), 1u);
  EXPECT_EQ(descriptors(h, "later").size(), 1u);
}

TEST(VQLStaticFunctorFramesTest,
     ComputedHelperOperandsCaptureTransitiveAliases) {
  Harness h;
  const auto data = items(h);
  const auto result =
      h.run(R"(
    def pure bound($value: Int): Int := $value + $offset;
    def pure apply($step, $threshold: Int): Stream<Int> := $input/$step;
    let $step := /+d.items[. > bound($threshold)];
    return (apply($step, 5))
  )",
            {{"input", {data.parent}}, {"offset", {h.integer(1)}}});
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, (std::vector<CellRef>{data.seven, data.eleven}));
  EXPECT_EQ(descriptors(h, "apply").size(), 1u);
  EXPECT_EQ(descriptors(h, "bound").size(), 1u);
}

TEST(VQLStaticFunctorFramesTest,
     HelperDependenciesHonorTheApplicationFormalScope) {
  Harness h;
  const auto data   = items(h);
  const auto result = h.run(R"(
    def pure bound($value: Int): Int := $value + $offset;
    def pure apply($step, $threshold: Int, $offset: Int): Stream<Int> :=
      $input/$step;
    let $offset := /+d.unrelated;
    let $step := /+d.items[. > bound($threshold)];
    return (apply($step, 5, 1))
  )",
                            {{"input", {data.parent}}});
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, (std::vector<CellRef>{data.seven, data.eleven}));
  EXPECT_EQ(descriptors(h, "apply").size(), 1u);
  EXPECT_EQ(descriptors(h, "bound").size(), 1u);
}

TEST(VQLStaticFunctorFramesTest,
     RequiredHelperAliasesKeepMissingDistinctFromEmpty) {
  Harness h;
  const auto data                   = items(h);
  constexpr std::string_view source = R"(
    def pure acceptable($candidate: Cell {+d.score: Int := $fallback}): Cell :=
      $candidate;
    def pure apply($step): Stream<Cell> := $input/$step;
    def pure relay($step): Stream<Cell> := apply($step);
    let $step := /+d.items[acceptable(.)];
    return (relay($step))
  )";
  const auto before                 = h.arena.cellCount();
  const auto missing                = h.run(source, {{"input", {data.parent}}});
  ASSERT_FALSE(missing);
  EXPECT_NE(missing.error().find("fallback"), std::string::npos);
  EXPECT_EQ(h.arena.cellCount(), before);
  const auto empty =
      h.run(source, {{"input", {data.parent}}, {"fallback", {}}});
  ASSERT_TRUE(empty) << empty.error();
  EXPECT_TRUE(empty->empty());
  EXPECT_EQ(descriptors(h, "relay").size(), 1u);
  EXPECT_EQ(descriptors(h, "apply").size(), 1u);
  EXPECT_EQ(descriptors(h, "acceptable").size(), 1u);
}

TEST(VQLStaticFunctorFramesTest, BooleanOperandsPreserveOrderAndAliasNames) {
  Harness h;
  const auto data   = items(h);
  const auto result = h.run(R"(
    def pure apply($step, $threshold: Int, $banned: Int, $keep: Int):
      Stream<Int> := $input/$step;
    let $step := /+d.items[. > $threshold and not . = $banned or $keep = .];
    return (apply($step, 5, 7, 3))
  )",
                            {{"input", {data.parent}}});
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, (std::vector<CellRef>{data.three, data.eleven}));
  EXPECT_EQ(descriptors(h, "apply").size(), 1u);
}

TEST(VQLStaticFunctorFramesTest, LiteralNestedShapesLowerInNativeVariants) {
  Harness h;
  const auto data = items(h);
  for (const auto candidate : {data.three, data.seven, data.eleven}) {
    const auto meta = h.cell("meta");
    h.link(candidate, "d.meta", meta);
    h.link(meta, "d.tag", h.cell(candidate == data.seven ? "yes" : "no"));
  }
  const auto result = h.run(R"(
    def pure apply($step): Stream<Int> := $input/$step;
    return (apply(/+d.items[{+d.meta: {+d.tag: "yes"}}]))
  )",
                            {{"input", {data.parent}}});
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, (std::vector<CellRef>{data.seven}));
  EXPECT_EQ(descriptors(h, "apply").size(), 1u);
}

TEST(VQLStaticFunctorFramesTest,
     NestedStaticBodiesFreezeWhileDecorationsStayLate) {
  Harness h;
  const auto data = items(h);
  h.link(data.three, "d.next", data.seven);
  h.link(data.seven, "d.next", data.eleven);
  const auto result = h.run(R"(
    def pure walk($node: Cell, $cut: Int, $n: Int, $route): Cell := {
      if $n > 0 return walk($node/$route, $cut, $n + -1, $route)
      else return $node;
    };
    let $inner := /+d.next[1];
    let $outer := /$inner[. > $cut];
    let $inner := /-d.next[1];
    return (walk($input, 5, 2, $outer))
  )",
                            {{"input", {data.three}}});
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, (std::vector<CellRef>{data.eleven}));
  EXPECT_EQ(descriptors(h, "walk").size(), 1u);
}

TEST(VQLStaticFunctorFramesTest, OuterClampSeesTheCombinedOrderedBodyOutput) {
  Harness h;
  const auto data   = items(h);
  const auto result = h.run(R"(
    def pure apply($owners: Stream<Cell>, $threshold: Int, $step): Stream<Int> :=
      $owners/$step;
    let $body := /+d.items;
    let $outer := /$body[. > $threshold][2];
    return (apply($input, 5, $outer))
  )",
                            {{"input", {data.parent, data.parent}}});
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, (std::vector<CellRef>{data.eleven}));
  EXPECT_EQ(descriptors(h, "apply").size(), 1u);
}

TEST(VQLStaticFunctorFramesTest, GenericKeysRetainExactIntegerAndDoubleTags) {
  Harness h;
  const auto parent = h.cell("parent");
  const auto lower  = h.integer(INT64_C(9007199254740992));
  const auto upper  = h.integer(INT64_C(9007199254740993));
  h.link(parent, "d.items", lower);
  h.link(lower, "d.items", upper);
  const auto result = h.run(R"(
    def pure apply($route, $cut: Int): Stream<Int> := $input/$route;
    return (apply(/+d.items[. > $cut and . > 9007199254740992], 0)),
           (apply(/+d.items[. > $cut and . > 9007199254740992.0], 0))
  )",
                            {{"input", {parent}}});
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, (std::vector<CellRef>{upper, upper}));
  EXPECT_EQ(descriptors(h, "apply").size(), 2u);
}

TEST(VQLStaticFunctorFramesTest,
     RecursiveProgramsSurviveAstFreeExportHostAndMesh) {
  constexpr std::string_view source = R"(
    def pure walk($node: Cell, $threshold: Int, $n: Int, $route): Cell := {
      if $n > 0 return walk($node/$route, $threshold, $n + -1, $route)
      else return $node;
    };
    let $root := %"3"; $root/d.next%"7"%"11";
    let $route := /+d.next[. > $threshold][1];
    return (string(walk($root, 5, 2, $route)))
  )";
  TemporaryDirectory temporary;
  auto scroll = std::make_shared<xanadu::UserPermascroll>(
      xanadu::UserPermascroll::Config{.storageDir = temporary.path / "scroll"});
  {
    Harness h;
    VQLCompiler compiler(h.coordinator, h.vm);
    Parser parser(source);
    auto query          = parser.parseQuery();
    const auto compiled = compiler.compile(query);
    ASSERT_TRUE(compiled.success) << compiled.errorMessage;
    query = QueryExpression{};
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
      vm.run(vm.spawnCursor(*entry, "static-functor-reload"), 100'000);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  std::optional<CellRef> halt;
  auto opcode = entry;
  for (std::size_t count = 0; opcode && count < 100'000; ++count) {
    if (vm.getOpcodeKind(*opcode) == OpcodeKind::Halt) {
      halt = opcode;
      break;
    }
    opcode = arena.linked(*opcode, core.dims().spin, DimVector::POS);
  }
  ASSERT_TRUE(halt);
  const auto outputs = core.inputsOf(*halt);
  ASSERT_EQ(outputs.size(), 1u);
  const auto values = vm.resultCells(outputs.front());
  ASSERT_EQ(values.size(), 1u);
  EXPECT_EQ(arena.textOf(values.front()), "11");
  EXPECT_FALSE(vm.hasActiveFrames());

  zigzag::vortex::VortexHost host;
  const auto hosted = host.executeVQLChecked(source);
  ASSERT_TRUE(hosted) << hosted.error().message;
  ASSERT_EQ(hosted->size(), 1u);
  EXPECT_EQ(host.core().arena().textOf(hosted->front()), "11");
  namespace mesh    = zigzag::vortex::mesh;
  const auto meshed = mesh::evaluate(mesh::Frontend::Vquery, source,
                                     {.maxCycles      = 100'000,
                                      .maxCells       = 100'000,
                                      .maxSolutions   = 16,
                                      .maxOutputBytes = 1024 * 1024});
  ASSERT_TRUE(meshed) << meshed.error();
  auto &meshArena = meshed->context->arena;
  std::optional<CellRef> itemsDimension;
  std::optional<CellRef> valueDimension;
  for (const auto &cell : meshArena.cells()) {
    if (meshArena.textOf(cell.birthOp) == "d.mesh.items")
      itemsDimension = cell.birthOp;
    if (meshArena.textOf(cell.birthOp) == "d.mesh.value")
      valueDimension = cell.birthOp;
  }
  ASSERT_TRUE(itemsDimension);
  ASSERT_TRUE(valueDimension);
  const auto record =
      meshArena.linked(meshed->root, *itemsDimension, DimVector::POS);
  ASSERT_TRUE(record);
  const auto output =
      meshArena.linked(*record, *valueDimension, DimVector::POS);
  ASSERT_TRUE(output);
  EXPECT_EQ(meshArena.textOf(*output), "11");
}

TEST(VQLStaticFunctorFramesTest, StructuralKeysUseContentAndAliasSpelling) {
  const auto normalized = [](const std::string_view source) {
    Parser parser(source);
    const auto query = parser.parseQuery();
    return normalizeStaticFunctorProgram(
        std::get<PathExpression>(query.expr).steps, 0);
  };
  const auto first         = normalized("/+d.items[. > $limit]");
  const auto same          = normalized("/+d.items[. > $limit]");
  const auto otherAlias    = normalized("/+d.items[. > $other]");
  const auto otherOperator = normalized("/+d.items[. >= $limit]");
  ASSERT_TRUE(first);
  ASSERT_TRUE(*first);
  ASSERT_TRUE(same);
  ASSERT_TRUE(*same);
  ASSERT_TRUE(otherAlias);
  ASSERT_TRUE(*otherAlias);
  ASSERT_TRUE(otherOperator);
  ASSERT_TRUE(*otherOperator);
  EXPECT_EQ((**first).key, (**same).key);
  EXPECT_NE((**first).key, (**otherAlias).key);
  EXPECT_NE((**first).key, (**otherOperator).key);
}

TEST(VQLStaticFunctorFramesTest, CyclicCapturedProgramsAreHardErrors) {
  for (const bool unsupportedDecoration : {false, true}) {
    auto cycle = std::make_shared<std::vector<PathStep>>();
    PathStep step;
    step.selector        = SignedDimensionStep{.dimName = "$inner"};
    step.capturedFunctor = cycle;
    if (unsupportedDecoration) step.projectionAfter.emplace();
    cycle->push_back(std::move(step));
    const auto result = normalizeStaticFunctorProgram(*cycle, 0);
    cycle->front().capturedFunctor.reset();
    ASSERT_FALSE(result);
    EXPECT_NE(result.error().message.find("cycl"), std::string::npos);
  }
}

TEST(VQLStaticFunctorFramesTest, RepeatedSharedBodiesConsumeTheNodeBudget) {
  auto body = std::make_shared<std::vector<PathStep>>();
  PathStep leaf;
  leaf.selector = SignedDimensionStep{.dimName = "d.next"};
  body->push_back(leaf);
  std::vector<PathStep> repeated;
  for (std::size_t index = 0; index < 64; ++index) {
    auto wrapper            = leaf;
    wrapper.selector        = SignedDimensionStep{.dimName = "$inner"};
    wrapper.capturedFunctor = body;
    repeated.push_back(std::move(wrapper));
  }
  const auto result = normalizeStaticFunctorProgram(
      repeated, 0, {.maxNodes = 64, .maxDepth = 256, .maxWork = 1'000'000});
  ASSERT_FALSE(result);
  EXPECT_FALSE(result.error().message.empty());
}

TEST(VQLStaticFunctorFramesTest, ScopeAnalysisChargesAliasBytesToItsWorkLimit) {
  const std::string name(1024, 'x');
  const std::string source = "/+d.items[. > $" + name + "]";
  Parser parser(source);
  const auto query  = parser.parseQuery();
  const auto result = analyzeFrameStaticFunctorScope(
      std::get<PathExpression>(query.expr).steps,
      {.maxNodes = 1000, .maxDepth = 256, .maxWork = 64});
  ASSERT_FALSE(result);
  EXPECT_NE(result.error().message.find("work"), std::string::npos);
}

TEST(VQLStaticFunctorFramesTest, CapturedExpansionHonorsItsOwnedCopyBudget) {
  PathStep bodyStep;
  bodyStep.selector =
      SignedDimensionStep{.dimName = "d." + std::string(1024, 'x')};
  const std::vector<PathStep> body{bodyStep};
  PathStep application;
  application.selector = SignedDimensionStep{.dimName = "$inner"};
  const std::vector<PathStep> source{application};
  EXPECT_GE(functorCaptureCopyWork(body), 1024u);
  const auto lookup = [&](const std::string_view name)
      -> std::optional<std::span<const PathStep>> {
    EXPECT_EQ(name, "inner");
    return std::span<const PathStep>{body};
  };
  std::size_t remaining = 64;
  const auto charge     = [&](const std::size_t work) {
    if (work > remaining) throw std::runtime_error("copy budget exhausted");
    remaining -= work;
  };
  EXPECT_THROW(captureFunctorApplications(source, lookup, charge),
               std::runtime_error);
  EXPECT_EQ(std::get<SignedDimensionStep>(source.front().selector).dimName,
            "$inner");
  EXPECT_EQ(std::get<SignedDimensionStep>(body.front().selector).dimName.size(),
            1026u);
}

TEST(VQLStaticFunctorFramesTest, TemplateWorkRefusalRollsBackAndCanRetry) {
  Harness h;
  const auto data                   = items(h);
  constexpr std::string_view source = R"(
    def pure apply($step, $threshold: Int): Stream<Int> := $input/$step;
    return (apply(/+d.items[. > $threshold], 5))
  )";
  VQLCompiler compiler(h.coordinator, h.vm);
  CompilationOptions options;
  options.initialBindings.emplace("input", std::vector<CellRef>{data.parent});
  options.limits.maxClosedFunctorTemplateWork = 0;
  const auto before                           = h.arena.cellCount();
  const auto refused = compiler.compile(source, options);
  EXPECT_FALSE(refused.success);
  EXPECT_EQ(h.arena.cellCount(), before);
  const auto result = h.run(source, {{"input", {data.parent}}});
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, (std::vector<CellRef>{data.seven, data.eleven}));
  EXPECT_EQ(descriptors(h, "apply").size(), 1u);
}

TEST(VQLStaticFunctorFramesTest,
     VariantRefusalRollsBackAndLargerLimitCanRetry) {
  Harness h;
  const auto data                   = items(h);
  constexpr std::string_view source = R"(
    def pure apply($route, $cut: Int): Stream<Int> := $input/$route;
    return (apply(/+d.items[. > $cut], 5)),
           (apply(/+d.items[. >= $cut], 5))
  )";
  VQLCompiler compiler(h.coordinator, h.vm);
  CompilationOptions options;
  options.initialBindings.emplace("input", std::vector<CellRef>{data.parent});
  options.limits.maxNamedFrameVariants = 1;
  const auto before                    = h.arena.cellCount();
  const auto refused                   = compiler.compile(source, options);
  EXPECT_FALSE(refused.success);
  EXPECT_EQ(h.arena.cellCount(), before);
  const auto result = h.run(source, {{"input", {data.parent}}});
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, (std::vector<CellRef>{data.seven, data.eleven, data.seven,
                                           data.eleven}));
  EXPECT_EQ(descriptors(h, "apply").size(), 2u);
}

} // namespace
} // namespace xanadu::vql
