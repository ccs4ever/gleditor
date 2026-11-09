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
#include <unordered_map>
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
      ("vql_closure_signatures_" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  TemporaryDirectory() { std::filesystem::create_directories(path); }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

constexpr std::string_view nestedTypedCurryingSource = R"(
  let $factory := \($left: Int) \($right: Int) string($left + $right);
  let $one := 1;
  let $increment := $one / $factory;
  let $wide := 9007199254740993;
  return $wide / $increment
)";

TEST(VQLClosureSignaturesTest,
     ZeroFormalClosureReceivesEachCandidateAsContext) {
  Harness h;
  const CellRef first  = h.cell("first");
  const CellRef second = h.cell("second");
  const auto result    = h.run(R"(
      let $f := \() .;
      return $input / $f
    )",
                               {{"input", {first, second, first}}});
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, (std::vector<CellRef>{first, second, first}));
  EXPECT_GT(h.count(OpcodeKind::CreateClosure), 0u);
  EXPECT_GT(h.count(OpcodeKind::ApplyClosure), 0u);
  EXPECT_GT(h.count(OpcodeKind::ReturnFrame), 0u);
}

TEST(VQLClosureSignaturesTest,
     NamedFormalGetsCandidateAndPreservesMultiplicity) {
  Harness h;
  const CellRef first  = h.cell("first");
  const CellRef second = h.cell("second");
  const auto result    = h.run(R"(
      let $f := \($item) $item;
      return $input / $f
    )",
                               {{"input", {first, second, first}}});
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, (std::vector<CellRef>{first, second, first}));
  EXPECT_GT(h.count(OpcodeKind::CreateClosure), 0u);
  EXPECT_GT(h.count(OpcodeKind::ApplyClosure), 0u);
}

TEST(VQLClosureSignaturesTest, DotFormalAndContextReadTheSameCandidate) {
  Harness h;
  const CellRef candidate = h.cell("candidate");
  const CellRef tag       = h.cell("tag");
  h.link(candidate, "d.tag", tag);
  const auto result = h.run(R"(
      let $f := \(. : Cell) ./+d.tag;
      return $input / $f
    )",
                            {{"input", {candidate, candidate}}});
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, (std::vector<CellRef>{tag, tag}));
  EXPECT_GT(h.count(OpcodeKind::CreateClosure), 0u);
  EXPECT_GT(h.count(OpcodeKind::ApplyClosure), 0u);
}

TEST(VQLClosureSignaturesTest, TypedFormalFiltersMismatchesBeforeBodyRuns) {
  Harness h;
  const CellRef valid   = h.integer(42);
  const CellRef invalid = h.cell("not an integer");
  const auto result     = h.run(R"(
      let $f := \($value: Int) $value;
      return $input / $f
    )",
                                {{"input", {valid, invalid, valid}}});
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, (std::vector<CellRef>{valid, valid}));
  EXPECT_EQ(h.arena.asInt64(result->front()), std::optional<std::int64_t>{42});
  EXPECT_GT(h.count(OpcodeKind::MatchContract), 0u);
}

TEST(VQLClosureSignaturesTest,
     UnionAndOptionalTypedFormalsKeepCandidateBinding) {
  Harness h;
  const CellRef integer  = h.integer(7);
  const CellRef text     = h.cell("text");
  const auto unionResult = h.run(R"(
      let $f := \($value: Int | String) $value;
      return $input / $f
    )",
                                 {{"input", {integer, text}}});
  ASSERT_TRUE(unionResult) << unionResult.error();
  EXPECT_EQ(*unionResult, (std::vector<CellRef>{integer, text}));

  const auto optionalResult = h.run(R"(
      let $f := \($value: Int?) $value;
      return $input / $f
    )",
                                    {{"input", {integer, text}}});
  ASSERT_TRUE(optionalResult) << optionalResult.error();
  EXPECT_EQ(*optionalResult, (std::vector<CellRef>{integer}));
}

TEST(VQLClosureSignaturesTest,
     DestructureFormalMatchesAndBindsCandidateFields) {
  Harness h;
  const CellRef first   = h.cell("first");
  const CellRef score1  = h.integer(17);
  const CellRef second  = h.cell("second");
  const CellRef score2  = h.cell("bad score");
  const CellRef missing = h.cell("missing score");
  h.link(first, "d.score", score1);
  h.link(second, "d.score", score2);
  const auto result = h.run(R"(
      let $f := \({ ., +d.score: $score: Int }) $score;
      return $input / $f
    )",
                            {{"input", {first, second, missing, first}}});
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, (std::vector<CellRef>{score1, score1}));
  EXPECT_GT(h.count(OpcodeKind::ApplyClosure), 0u);
  EXPECT_GT(h.count(OpcodeKind::MatchContract), 0u);
}

TEST(VQLClosureSignaturesTest,
     StructuralDefaultCapturesCreationScopeForMissingField) {
  Harness h;
  const CellRef owner  = h.cell("owner");
  const CellRef first  = h.integer(11);
  const CellRef before = h.integer(23);
  const CellRef after  = h.integer(99);
  h.link(owner, "d.first", first);
  const auto result =
      h.run(R"(
      let $outside := $before;
      let $f := \($item: Cell {
        +d.first: $outside: Int,
        +d.next: $value: Int := $outside
      }) $value;
      let $outside := $after;
      return $input / $f
    )",
            {{"before", {before}}, {"after", {after}}, {"input", {owner}}});
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, (std::vector<CellRef>{before}));
}

TEST(VQLClosureSignaturesTest,
     StreamRefinementAliasDoesNotLeakIntoCaptureScope) {
  Harness h;
  const CellRef candidate = h.cell("candidate");
  const CellRef score     = h.integer(8);
  const CellRef outside   = h.cell("outside alias");
  h.link(candidate, "d.score", score);
  const auto result =
      h.run(R"(
      let $element := $outside;
      let $f := \($values: Stream<Cell { +d.score: $element: Int }>) $element;
      return $input / $f
    )",
            {{"outside", {outside}}, {"input", {candidate, candidate}}});
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, (std::vector<CellRef>{outside, outside}));
}

TEST(VQLClosureSignaturesTest,
     ClosureBodySupportsScalarOperatorsAndStringConversion) {
  Harness h;
  const CellRef input = h.integer(INT64_C(9007199254740993));
  const auto result   = h.run(R"(
      let $f := \($value: Int) string($value + 7);
      return $input / $f
    )",
                              {{"input", {input}}});
  ASSERT_TRUE(result) << result.error();
  ASSERT_EQ(result->size(), 1u);
  EXPECT_EQ(h.arena.textOf(result->front()), "9007199254741000");
  EXPECT_GT(h.count(OpcodeKind::ApplyClosure), 0u);

  const CellRef decimal   = h.arena.makeScalarCell(1.25);
  const auto doubleResult = h.run(R"(
      let $f := \($value: Float) string($value + 0.5);
      return $input / $f
    )",
                                  {{"input", {decimal}}});
  ASSERT_TRUE(doubleResult) << doubleResult.error();
  ASSERT_EQ(doubleResult->size(), 1u);
  EXPECT_EQ(h.arena.textOf(doubleResult->front()), "1.75");
}

TEST(VQLClosureSignaturesTest,
     NestedFormalShadowingDoesNotReplaceOuterBinding) {
  Harness h;
  const CellRef outer       = h.cell("outer");
  const CellRef replacement = h.cell("replacement");
  const auto result =
      h.run(R"(
      let $f := \($x) \($x) $x;
      let $inner := $outer / $f;
      return $replacement / $inner
    )",
            {{"outer", {outer}}, {"replacement", {replacement}}});
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, (std::vector<CellRef>{replacement}));
  EXPECT_GT(h.count(OpcodeKind::ApplyClosure), 0u);
}

TEST(VQLClosureSignaturesTest,
     CallsiteFormalShadowsSameNamedOuterAliasInHelper) {
  Harness h;
  const CellRef outside = h.cell("outside");
  const CellRef input   = h.cell("input");
  const auto result     = h.run(R"(
      def pure read(): Any := $x;
      let $x := $outside;
      let $f := \($x) read();
      return ## / $f
    )",
                                {{"outside", {outside}}, {"input", {input}}});
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, (std::vector<CellRef>{h.core.home()}));
}

TEST(VQLClosureSignaturesTest, HelperFreeGlobalUsesClosureCreationBinding) {
  Harness h;
  const CellRef before = h.cell("before");
  const CellRef after  = h.cell("after");
  const CellRef item   = h.cell("item");
  const auto result =
      h.run(R"(
      def pure select($candidate) := $candidate / read();
      def pure read(): Any := $secret;
      let $secret := $before;
      let $f := \($candidate) select($candidate);
      let $secret := $after;
      return $input / $f
    )",
            {{"before", {before}}, {"after", {after}}, {"input", {item}}});
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, (std::vector<CellRef>{before}));
  EXPECT_GT(h.count(OpcodeKind::InvokeFrame), 0u);
}

TEST(VQLClosureSignaturesTest, ScriptBodyLocalBindingsDoNotCaptureOuterValue) {
  Harness h;
  const CellRef outside = h.cell("outside");
  const CellRef local   = h.cell("local");
  const auto result     = h.run(R"(
      def pure read(): Any := $x;
      let $x := $outside;
      let $f := \($candidate) {
        let $x := $local;
        return read();
      };
      return $input / $f
    )",
                                {{"outside", {outside}},
                                 {"local", {local}},
                                 {"input", {h.cell("candidate")}}});
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, (std::vector<CellRef>{local}));
}

TEST(VQLClosureSignaturesTest,
     NestedLambdaCurryingKeepsExactIntegerArithmetic) {
  Harness h;
  const CellRef one = h.integer(1);
  const CellRef two = h.integer(INT64_C(9007199254740993));
  const auto result = h.run(R"(
      let $add := \($left: Int) \($right: Int) $left + $right;
      let $inc := $one / $add;
      return $two / $inc
    )",
                            {{"one", {one}}, {"two", {two}}});
  ASSERT_TRUE(result) << result.error();
  ASSERT_EQ(result->size(), 1u);
  EXPECT_EQ(h.arena.asInt64(result->front()),
            std::optional<std::int64_t>{INT64_C(9007199254740994)});
  EXPECT_GT(h.count(OpcodeKind::CreateClosure), 1u);
  EXPECT_GT(h.count(OpcodeKind::ApplyClosure), 0u);
}

TEST(VQLClosureSignaturesTest, OuterFactoryFreezesGlobalUsedOnlyByNestedBody) {
  Harness h;
  const CellRef before = h.cell("before");
  const CellRef after  = h.cell("after");
  const CellRef item   = h.cell("item");
  const auto result =
      h.run(R"(
      let $global := $before;
      let $factory := \($ignored) \($candidate) $global;
      let $global := $after;
      let $inner := $input / $factory;
      return $input / $inner
    )",
            {{"before", {before}}, {"after", {after}}, {"input", {item}}});
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, (std::vector<CellRef>{before}));
  EXPECT_GT(h.count(OpcodeKind::CreateClosure), 1u);
}

TEST(VQLClosureSignaturesTest, NamedHelperReturnPromiseViolationIsNotFiltered) {
  Harness h;
  const CellRef item = h.cell("item");
  const auto result  = h.run(R"(
      def pure broken(): Int := "not an integer";
      let $f := \() broken();
      return $input / $f
    )",
                             {{"input", {item}}});
  ASSERT_FALSE(result);
  EXPECT_NE(result.error().find("broken"), std::string::npos);
  EXPECT_FALSE(h.vm.hasActiveFrames());
  EXPECT_FALSE(h.vm.hasActiveGuards());
}

TEST(VQLClosureSignaturesTest,
     WholeFormalDefaultsRemainAnExplicitCompileError) {
  Harness h;
  const std::size_t before = h.arena.cellCount();
  VQLCompiler compiler(h.coordinator, h.vm);
  const auto refused = compiler.compile(R"(
      let $f := \($value := 7) $value;
      return $f
    )");
  EXPECT_FALSE(refused.success);
  EXPECT_EQ(h.arena.cellCount(), before);
  const auto unresolvedDefault = compiler.compile(R"(
      let $f := \($item: Cell { +d.value: Int := $missing }) $item;
      return $f
    )");
  EXPECT_FALSE(unresolvedDefault.success);
  EXPECT_EQ(h.arena.cellCount(), before);
  const CellRef withoutValue = h.cell("without value");
  const auto emptyDefault    = h.run(R"(
      let $f := \($item: Cell { +d.value: Int := $missing }) $item;
      return $input / $f
    )",
                                     {{"input", {withoutValue}}, {"missing", {}}});
  ASSERT_TRUE(emptyDefault) << emptyDefault.error();
  EXPECT_TRUE(emptyDefault->empty());
  const auto retry = h.run(R"(
      let $f := \($value: Int) $value;
      return $input / $f
    )",
                           {{"input", {h.integer(7)}}});
  ASSERT_TRUE(retry) << retry.error();
  ASSERT_EQ(retry->size(), 1u);
  EXPECT_EQ(h.arena.asInt64(retry->front()), std::optional<std::int64_t>{7});
}

TEST(VQLClosureSignaturesTest, InvocationErrorsLeaveNoActiveFrameOrGuard) {
  Harness h;
  const CellRef owner = h.cell("owner");
  const CellRef old   = h.integer(12);
  h.link(owner, "d.value", old);
  const auto result = h.run(R"(
      def weave replace($candidate: Cell { +d.value: Int }): Cell := {
        weave { $candidate/+d.value/value("bad") };
        return $candidate;
      };
      let $f := \($candidate: Cell { +d.value: Int }) replace($candidate);
      return $input / $f
    )",
                            {{"input", {owner}}});
  ASSERT_FALSE(result);
  EXPECT_EQ(h.arena.linked(owner, h.coordinator.resolveDimension("d.value"),
                           DimVector::POS),
            std::optional<CellRef>{old});
  EXPECT_EQ(h.arena.asInt64(old), std::optional<std::int64_t>{12});
  EXPECT_FALSE(h.vm.hasActiveFrames());
  EXPECT_FALSE(h.vm.hasActiveGuards());
}

TEST(VQLClosureSignaturesTest,
     TypedNestedCurryingSurvivesAstFreeExportReloadHostAndMesh) {
  TemporaryDirectory temporary;
  auto scroll = std::make_shared<xanadu::UserPermascroll>(
      xanadu::UserPermascroll::Config{.storageDir = temporary.path / "scroll"});
  constexpr std::string_view source = nestedTypedCurryingSource;
  {
    Harness h;
    VQLCompiler compiler(h.coordinator, h.vm);
    Parser parser(source);
    auto ast            = parser.parseQuery();
    const auto compiled = compiler.compile(ast);
    ASSERT_TRUE(compiled.success) << compiled.errorMessage;
    ast = QueryExpression{};
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
      vm.run(vm.spawnCursor(*entry, "typed-nested-closure-reload"), 10'000);
  ASSERT_TRUE(execution.success) << execution.errorMessage;

  std::optional<CellRef> halt;
  auto opcode = entry;
  for (std::size_t count = 0; opcode && count < 10'000; ++count) {
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
  EXPECT_EQ(arena.textOf(values.front()), "9007199254740994");

  zigzag::vortex::VortexHost host;
  const auto hosted = host.executeVQLChecked(source);
  ASSERT_TRUE(hosted) << hosted.error().message;
  ASSERT_EQ(hosted->size(), 1u);
  EXPECT_EQ(host.core().arena().textOf(hosted->front()), "9007199254740994");

  namespace mesh    = zigzag::vortex::mesh;
  const auto meshed = mesh::evaluate(mesh::Frontend::Vquery, source,
                                     {.maxCycles      = 10'000,
                                      .maxCells       = 10'000,
                                      .maxSolutions   = 16,
                                      .maxOutputBytes = 1024 * 1024});
  ASSERT_TRUE(meshed) << meshed.error();
  auto &meshArena = meshed->context->arena;
  std::optional<CellRef> items;
  std::optional<CellRef> value;
  for (const auto &cell : meshArena.cells()) {
    if (meshArena.textOf(cell.birthOp) == "d.mesh.items") items = cell.birthOp;
    if (meshArena.textOf(cell.birthOp) == "d.mesh.value") value = cell.birthOp;
  }
  ASSERT_TRUE(items);
  ASSERT_TRUE(value);
  const auto record = meshArena.linked(meshed->root, *items, DimVector::POS);
  ASSERT_TRUE(record);
  const auto output = meshArena.linked(*record, *value, DimVector::POS);
  ASSERT_TRUE(output);
  EXPECT_EQ(meshArena.textOf(*output), "9007199254740994");
}

TEST(VQLClosureSignaturesTest, CaptureMetadataBudgetRefusesAndRetryIsClean) {
  Harness h;
  const auto outside                = h.integer(INT64_C(9007199254740993));
  constexpr std::string_view source = R"(
    def pure helper(): Int := $outside;
    let $f := \($candidate) helper();
    return ##/$f
  )";
  VQLCompiler compiler(h.coordinator, h.vm);
  CompilationOptions options;
  options.initialBindings              = {{"outside", {outside}}};
  options.limits.maxClosureCaptureWork = 0;
  const auto before                    = h.arena.cellCount();
  const auto refused                   = compiler.compile(source, options);
  EXPECT_FALSE(refused.success);
  EXPECT_NE(refused.errorMessage.find("capture metadata work budget"),
            std::string::npos);
  EXPECT_EQ(h.arena.cellCount(), before);
  EXPECT_FALSE(h.vm.hasActiveFrames());
  const auto result = h.run(source, {{"outside", {outside}}});
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, (std::vector<CellRef>{outside}));
}

TEST(VQLClosureSignaturesTest, UnknownFormalTypeIsNamedBeforeAnyApplication) {
  Harness h;
  QueryExecutor executor(h.coordinator, h.vm);
  const auto before = h.arena.cellCount();
  const auto result = executor.execute(
      R"(let $f := \($item: UnknownClosureType) $item; return $f)");
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().category,
            QueryExecutionError::Category::TypeContract);
  EXPECT_NE(result.error().message.find("UnknownClosureType"),
            std::string::npos);
  EXPECT_EQ(h.arena.cellCount(), before);
}

} // namespace
} // namespace xanadu::vql
