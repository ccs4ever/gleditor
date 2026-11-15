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
using Bindings = std::unordered_map<std::string, std::vector<CellRef>>;
using Result   = std::expected<std::vector<CellRef>, std::string>;

struct Harness {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  MultiStoreCoordinator coordinator{core};
  zigzag::vortex::VortexVM vm{core};
  CompilationResult compileResult;

  CellRef cell(const std::string_view text) { return arena.makeCell(text); }

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
    compileResult =
        compiler.compile(source, {.initialBindings = std::move(bindings)});
    return compileResult;
  }

  Result execute(const CompilationResult &program) {
    if (!program.success) return std::unexpected(program.errorMessage);
    if (!program.cursor) return std::unexpected("missing compiled cursor");
    const auto execution = vm.run(*program.cursor, 200'000);
    if (!execution.success) {
      vm.abortScopedChoices(*program.cursor);
      vm.setCursorOpcode(*program.cursor, std::nullopt);
      return std::unexpected(execution.errorMessage);
    }
    std::vector<CellRef> values;
    for (const CellRef slot : program.resultSlots) {
      const auto results = vm.resultCells(slot);
      values.insert(values.end(), results.begin(), results.end());
    }
    vm.abortScopedChoices(*program.cursor);
    vm.setCursorOpcode(*program.cursor, std::nullopt);
    return values;
  }

  Result run(const std::string_view source, Bindings bindings = {}) {
    return execute(compile(source, std::move(bindings)));
  }

  std::size_t count(const OpcodeKind kind) const {
    std::size_t result = 0;
    for (const auto opcode : compileResult.generatedOpcodes)
      if (vm.getOpcodeKind(opcode) == kind) ++result;
    return result;
  }
};

struct TemporaryDirectory {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("vql_closure_calls_" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  TemporaryDirectory() { std::filesystem::create_directories(path); }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

TEST(VQLClosureCallsDefaults, MultiFormalCallsAndNestedCurryingPreserveValues) {
  Harness h;
  const auto result = h.run(R"(
      let $outer := \($x: Int) \($y: Int) ($x + $y);
      let $inner := $outer(3);
      return $inner(4)
    )");
  ASSERT_TRUE(result) << result.error();
  ASSERT_EQ(result->size(), 1u);
  EXPECT_EQ(h.arena.asInt64(result->front()), std::optional<std::int64_t>{7});
  EXPECT_GT(h.count(OpcodeKind::CheckClosureCall), 0u);
  EXPECT_GT(h.count(OpcodeKind::ApplyClosure), 0u);

  const auto pair = h.run(R"(
      let $pair := \($left: Int, $right: Int) ($left + $right);
      return $pair(20, 22)
    )");
  ASSERT_TRUE(pair) << pair.error();
  ASSERT_EQ(pair->size(), 1u);
  EXPECT_EQ(h.arena.asInt64(pair->front()), std::optional<std::int64_t>{42});
}

TEST(VQLClosureCallsDefaults, EarlierDefaultCapturesOuterLaterFormalName) {
  Harness h;
  const auto result = h.run(R"(
      let $later := 7;
      let $f := \($first: Int := $later, $later: Int := 2)
                  ($first + $later);
      let $later := 99;
      return $f()
    )");
  ASSERT_TRUE(result) << result.error();
  ASSERT_EQ(result->size(), 1u);
  EXPECT_EQ(h.arena.asInt64(result->front()), std::optional<std::int64_t>{9});
}

TEST(VQLClosureCallsDefaults, EarlierPatternCaptureFeedsLaterDefault) {
  Harness h;
  const CellRef owner = h.cell("owner");
  const CellRef value = h.integer(17);
  h.link(owner, "d.value", value);
  const auto result = h.run(R"(
      let $f := \({ +d.value: $seed: Int }, $n: Int := $seed) $n;
      return $f($owner)
    )",
                            {{"owner", {owner}}});
  ASSERT_TRUE(result) << result.error();
  ASSERT_EQ(result->size(), 1u);
  EXPECT_EQ(h.arena.asInt64(result->front()), std::optional<std::int64_t>{17});
}

TEST(VQLClosureCallsDefaults, OmittedAndExplicitlyEmptyArgumentsDiffer) {
  Harness h;
  const CellRef fallback = h.integer(23);
  const auto omitted =
      h.run(R"(let $f := \($value := $fallback) $value; return $f())",
            {{"fallback", {fallback}}});
  ASSERT_TRUE(omitted) << omitted.error();
  ASSERT_EQ(omitted->size(), 1u);
  EXPECT_EQ(omitted->front(), fallback);

  const auto suppliedEmpty =
      h.run(R"(let $f := \($value := $fallback) $value; return $f($empty))",
            {{"fallback", {fallback}}, {"empty", {}}});
  ASSERT_TRUE(suppliedEmpty) << suppliedEmpty.error();
  EXPECT_TRUE(suppliedEmpty->empty());
}

TEST(VQLClosureCallsDefaults, DefaultUsesCallContextAndNoContextStaysEmpty) {
  Harness h;
  const CellRef owner = h.cell("owner");
  const CellRef other = h.cell("other");
  const auto fromContext =
      h.run(R"(let $f := \($value := .) $value; return $owner/$f())",
            {{"owner", {owner}}});
  ASSERT_TRUE(fromContext) << fromContext.error();
  ASSERT_EQ(fromContext->size(), 1u);
  EXPECT_EQ(fromContext->front(), owner);

  const auto independentContext =
      h.run(R"(let $f := \($value) .; return $owner/$f($argument))",
            {{"owner", {owner}}, {"argument", {other}}});
  ASSERT_TRUE(independentContext) << independentContext.error();
  ASSERT_EQ(independentContext->size(), 1u);
  EXPECT_EQ(independentContext->front(), owner);

  const auto noContext =
      h.run(R"(let $f := \($value := .) $value; return $f())");
  ASSERT_TRUE(noContext) << noContext.error();
  EXPECT_TRUE(noContext->empty());
}

TEST(VQLClosureCallsDefaults, ExplicitDotDefaultsRunBeforeAndAfterRebinding) {
  Harness h;
  const CellRef caller   = h.cell("caller");
  const CellRef other    = h.cell("other");
  const CellRef override = h.cell("override");
  h.link(caller, "d.target", other);
  h.link(caller, "d.seed", h.integer(11));
  h.link(other, "d.seed", h.integer(3));
  h.link(override, "d.seed", h.integer(7));
  const auto result = h.run(R"(
      let $f := \(.: Cell := ./+d.target[1], $n: Int := ./+d.seed[1]) $n;
      return ($caller/$f()), ($caller/$f($override))
    )",
                            {{"caller", {caller}}, {"override", {override}}});
  ASSERT_TRUE(result) << result.error();
  ASSERT_EQ(result->size(), 2u);
  EXPECT_EQ(h.arena.asInt64((*result)[0]), 3);
  EXPECT_EQ(h.arena.asInt64((*result)[1]), 7);
}

TEST(VQLClosureCallsDefaults,
     FailedDefaultReturnPromiseCleansFramesAndCanRetry) {
  Harness h;
  const auto supplied = h.run(R"(
      def pure broken(): Int := "bad";
      let $f := \($value: Int := broken()) $value;
      return $f(3)
    )");
  ASSERT_TRUE(supplied) << supplied.error();
  ASSERT_EQ(supplied->size(), 1u);
  EXPECT_EQ(h.arena.asInt64(supplied->front()), 3);
  const auto failed = h.run(R"(
      def pure broken(): Int := "bad";
      let $f := \($value: Int := broken()) $value;
      return $f()
    )");
  ASSERT_FALSE(failed);
  EXPECT_NE(failed.error().find("broken"), std::string::npos);
  EXPECT_FALSE(h.vm.hasActiveFrames());
  EXPECT_FALSE(h.vm.hasActiveGuards());
  EXPECT_EQ(h.arena.outstandingMarks(), 0u);
  const auto retried =
      h.run(R"(let $f := \($value: Int := 17) $value; return $f())");
  ASSERT_TRUE(retried) << retried.error();
  ASSERT_EQ(retried->size(), 1u);
  EXPECT_EQ(h.arena.asInt64(retried->front()), 17);
}

TEST(VQLClosureCallsDefaults, DefaultsAreLazyAndTypedArgumentsStillFilter) {
  Harness h;
  const CellRef owner = h.cell("owner");
  const CellRef input = h.integer(31);
  const CellRef mark  = h.cell("original");
  const auto supplied =
      h.run(R"(
      let $f := \($value := $mark/value("made")) $value;
      return $owner/$f($input)
    )",
            {{"owner", {owner}}, {"input", {input}}, {"mark", {mark}}});
  ASSERT_TRUE(supplied) << supplied.error();
  EXPECT_EQ(h.arena.textOf(mark), "original");

  const auto omitted = h.run(R"(
      let $f := \($value := $mark/value("made")) $value;
      return $owner/$f()
    )",
                             {{"owner", {owner}}, {"mark", {mark}}});
  ASSERT_TRUE(omitted) << omitted.error();
  EXPECT_EQ(h.arena.textOf(mark), "made");

  const auto typed =
      h.run(R"(let $f := \($value: Int) $value; return $f($good))",
            {{"good", {input}}});
  ASSERT_TRUE(typed) << typed.error();
  ASSERT_EQ(typed->size(), 1u);
  EXPECT_EQ(typed->front(), input);
  const auto filtered =
      h.run(R"(let $f := \($value: Int) $value; return $f($bad))",
            {{"bad", {h.cell("not an integer")}}});
  ASSERT_TRUE(filtered) << filtered.error();
  EXPECT_TRUE(filtered->empty());
}

TEST(VQLClosureCallsDefaults, ContextualCallsiteAcceptsDifferentFormalCounts) {
  Harness h;
  const auto makeZero = h.run(R"(return (\() .))");
  ASSERT_TRUE(makeZero) << makeZero.error();
  const auto makeOne = h.run(R"(return (\($x) $x))");
  ASSERT_TRUE(makeOne) << makeOne.error();
  const auto makeTwo = h.run(R"(return (\($x, $y := 2) $x))");
  ASSERT_TRUE(makeTwo) << makeTwo.error();
  ASSERT_EQ(makeZero->size(), 1u);
  ASSERT_EQ(makeOne->size(), 1u);
  ASSERT_EQ(makeTwo->size(), 1u);

  const CellRef candidate = h.cell("candidate");
  const auto result       = h.run(
      R"(for $f in $functions return $candidate/$f)",
      {{"functions", {makeZero->front(), makeOne->front(), makeTwo->front()}},
             {"candidate", {candidate}}});
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, (std::vector<CellRef>{candidate, candidate, candidate}));
}

TEST(VQLClosureCallsDefaults, InvalidCalleeAndExcessArityPreflightEffects) {
  Harness h;
  const CellRef owner       = h.cell("owner");
  const zigzag::DimRef mark = h.coordinator.resolveDimension("d.mark");
  const CellRef bad         = h.cell("ordinary cell, not a closure");
  const auto invalid = h.compile(R"(return $bad($owner/d.mark%"invalid"))",
                                 {{"bad", {bad}}, {"owner", {owner}}});
  ASSERT_TRUE(invalid.success) << invalid.errorMessage;
  EXPECT_FALSE(h.execute(invalid));
  EXPECT_EQ(h.arena.linked(owner, mark, DimVector::POS), std::nullopt);

  const auto excessive = h.compile(R"(
      let $f := \($only) $only;
      return $f($owner/d.mark%"first", $owner/d.mark%"second")
    )",
                                   {{"owner", {owner}}});
  ASSERT_TRUE(excessive.success) << excessive.errorMessage;
  EXPECT_FALSE(h.execute(excessive));
  EXPECT_EQ(h.arena.linked(owner, mark, DimVector::POS), std::nullopt);
  EXPECT_FALSE(h.vm.hasActiveFrames());
  EXPECT_FALSE(h.vm.hasActiveGuards());
}

TEST(VQLClosureCallsDefaults, ExplicitCallResultCanContinueAsAPathStream) {
  Harness h;
  const CellRef owner = h.cell("owner");
  const CellRef leaf  = h.cell("leaf");
  h.link(owner, "d.x", leaf);
  const auto result =
      h.run(R"(let $f := \($value) $value; return $f($owner)/+d.x)",
            {{"owner", {owner}}});
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, (std::vector<CellRef>{leaf}));
}

TEST(VQLClosureCallsDefaults,
     ScalarPathOperandCarriesItsOwnerIntoClosureDefaultContext) {
  Harness h;
  const CellRef owner = h.cell("owner");
  const CellRef seed  = h.integer(8);
  h.link(owner, "d.seed", seed);
  const auto result = h.run(
      R"(let $f := \($value := ./d.seed) $value; return (($owner/$f()) + 1))",
      {{"owner", {owner}}});
  ASSERT_TRUE(result) << result.error();
  ASSERT_EQ(result->size(), 1u);
  EXPECT_EQ(h.arena.asInt64(result->front()), std::optional<std::int64_t>{9});
}

TEST(VQLClosureCallsDefaults,
     NestedScalarPredicateUsesItsCandidateAsClosureDefaultContext) {
  Harness h;
  const CellRef owner = h.cell("owner");
  const CellRef seed  = h.integer(5);
  h.link(owner, "d.seed", seed);
  const auto result = h.run(
      R"(let $f := \($n: Int := ./+d.seed[1]) $n;
         return $owner[($f() + 1) = 6])",
      {{"owner", {owner}}});
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, (std::vector<CellRef>{owner}));
}

TEST(VQLClosureCallsDefaults,
     DirectComparisonOperandInheritsCandidateAndRestoresMissingContext) {
  Harness h;
  const CellRef owner = h.cell("owner");
  const auto matched  = h.run(
      R"(let $f := \($value: Cell := .) $value;
         return $owner[$f() = $owner])",
      {{"owner", {owner}}});
  ASSERT_TRUE(matched) << matched.error();
  EXPECT_EQ(*matched, (std::vector<CellRef>{owner}));
  const auto missing =
      h.run(R"(let $f := \($value: Cell := .) $value; return ($f()))");
  ASSERT_TRUE(missing) << missing.error();
  EXPECT_TRUE(missing->empty());
}

TEST(VQLClosureCallsDefaults,
     DefaultSignaturesSurviveAstFreeStoreReloadHostAndVqueryMesh) {
  TemporaryDirectory temporary;
  auto scroll = std::make_shared<xanadu::UserPermascroll>(
      xanadu::UserPermascroll::Config{.storageDir = temporary.path / "scroll"});
  constexpr std::string_view source = R"(
    let $later := 7;
    let $f := \($first: Int := $later, $later: Int := 2) ($first + $later);
    let $later := 99;
    return $f()
  )";
  {
    Harness h;
    VQLCompiler compiler(h.coordinator, h.vm);
    Parser parser(source);
    auto ast            = parser.parseQuery();
    const auto compiled = compiler.compile(ast);
    h.compileResult     = compiled;
    ASSERT_TRUE(compiled.success) << compiled.errorMessage;
    EXPECT_GT(h.count(OpcodeKind::CheckClosureCall), 0u);
    EXPECT_GT(h.count(OpcodeKind::ApplyClosure), 0u);
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
      vm.run(vm.spawnCursor(*entry, "closure-default-reload"), 10'000);
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
  EXPECT_EQ(arena.asInt64(values.front()), std::optional<std::int64_t>{9});

  zigzag::vortex::VortexHost host;
  const auto hosted = host.executeVQLChecked(source);
  ASSERT_TRUE(hosted) << hosted.error().message;
  ASSERT_EQ(hosted->size(), 1u);
  EXPECT_EQ(host.core().arena().asInt64(hosted->front()),
            std::optional<std::int64_t>{9});

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
  EXPECT_EQ(meshArena.asInt64(*output), std::optional<std::int64_t>{9});
}

} // namespace
} // namespace xanadu::vql
