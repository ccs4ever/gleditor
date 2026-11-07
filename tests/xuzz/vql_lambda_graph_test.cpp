/**
 * @file vql_lambda_graph_test.cpp
 * @brief Anonymous path lambdas lower into ordinary Vortex instruction graphs.
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

  CellRef integer(const std::int64_t value) {
    return arena.makeScalarCell(value);
  }

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
};

struct TemporaryDirectory {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("vql_lambda_graph_" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));

  TemporaryDirectory() { std::filesystem::create_directories(path); }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

std::size_t countOpcode(const CompilationResult &compiled,
                        const zigzag::vortex::VortexVM &vm,
                        const OpcodeKind kind) {
  std::size_t count = 0;
  for (const CellRef opcode : compiled.generatedOpcodes)
    count += vm.getOpcodeKind(opcode) == kind;
  return count;
}

TEST(VQLLambdaGraphTest, TypedDestructuringAndDefaultsRunInTheInlineGraph) {
  Harness h;
  const CellRef valid      = h.cell("valid");
  const CellRef validScore = h.integer(17);
  h.link(h.core.home(), "d.items", valid);
  h.link(valid, "d.score", validScore);
  const CellRef missing = h.cell("missing");
  h.link(valid, "d.items", missing);
  const CellRef malformed = h.cell("malformed");
  h.link(missing, "d.items", malformed);
  h.link(malformed, "d.score", h.cell("not an integer"));

  constexpr std::string_view source =
      "##/+d.items / \\($item: Cell { +d.score: $score: Int := 23 }) "
      "$score";
  const auto query             = Parser(source).parseQuery();
  zigzag::vortex::VortexVM &vm = h.vm;
  VQLCompiler compiler(h.coordinator, vm);
  const CompilationResult compiled = compiler.compile(query);
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  EXPECT_GT(countOpcode(compiled, h.vm, OpcodeKind::MatchContract), 0U);
  for (const CellRef opcode : compiled.generatedOpcodes) {
    if (h.vm.getOpcodeKind(opcode) == OpcodeKind::InvokeFrame)
      EXPECT_FALSE(h.arena.textOf(opcode).starts_with("#INVOKE_FRAME "));
  }

  const auto result = h.execute(source);
  ASSERT_TRUE(result) << result.error().message;
  ASSERT_EQ(result->size(), 2U);
  EXPECT_EQ(result->front(), validScore);
  EXPECT_EQ(h.arena.asInt64(result->back()), std::optional<std::int64_t>{23});
  EXPECT_FALSE(h.vm.hasActiveFrames());
}

TEST(VQLLambdaGraphTest, ContextAndZeroFormalLambdasUseTheCurrentCell) {
  Harness h;
  const CellRef first    = h.cell("first");
  const CellRef firstTag = h.cell("first tag");
  h.link(h.core.home(), "d.items", first);
  h.link(first, "d.tag", firstTag);
  const CellRef second    = h.cell("second");
  const CellRef secondTag = h.cell("second tag");
  h.link(first, "d.items", second);
  h.link(second, "d.tag", secondTag);

  const auto pointContext = h.execute("##/+d.items / \\(.) ./+d.tag");
  ASSERT_TRUE(pointContext) << pointContext.error().message;
  EXPECT_EQ(*pointContext, (std::vector<CellRef>{firstTag, secondTag}));

  const auto zeroFormal = h.execute("##/+d.items / \\() ./+d.tag");
  ASSERT_TRUE(zeroFormal) << zeroFormal.error().message;
  EXPECT_EQ(*zeroFormal, (std::vector<CellRef>{firstTag, secondTag}));
}

TEST(VQLLambdaGraphTest, NamedFrameLambdaKeepsItsDirectAndNestedContextLocal) {
  Harness h;
  const auto owner = h.cell("lambda owner");
  constexpr std::string_view definitions =
      "def pure context(): Cell := .; "
      "def pure direct($owner: Cell): Cell := $owner / \\($item) .; "
      "def pure nested($owner: Cell): Cell := $owner / \\($item) context(); ";
  for (const std::string_view name : {"direct", "nested"}) {
    const std::string source =
        std::string(definitions) + std::string(name) + "($owner)";
    const auto query       = Parser(source).parseQuery();
    const auto eligibility = analyzeFrameEligibility(query);
    ASSERT_TRUE(eligibility) << eligibility.error().message;
    EXPECT_TRUE(eligibility->reusableDefinitions.contains(std::string(name)));
    EXPECT_TRUE(eligibility->freeCaptureNames.at(std::string(name)).empty());
    VQLCompiler compiler(h.coordinator, h.vm);
    CompilationOptions options;
    options.initialBindings = {{"owner", {owner}}};
    const auto compiled     = compiler.compile(query, options);
    ASSERT_TRUE(compiled.success) << compiled.errorMessage;
    EXPECT_GT(countOpcode(compiled, h.vm, OpcodeKind::InvokeFrame), 0U);
    const auto result = h.execute(source, {{"owner", {owner}}});
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(*result, (std::vector<CellRef>{owner}));
  }
}

TEST(VQLLambdaGraphTest, OptionalTypedFieldBindsWhenPresentAndAllowsAbsence) {
  Harness h;
  const CellRef first    = h.cell("has subtitle");
  const CellRef subtitle = h.cell("footnote");
  h.link(h.core.home(), "d.items", first);
  h.link(first, "d.subtitle", subtitle);
  const CellRef second = h.cell("no subtitle");
  h.link(first, "d.items", second);

  constexpr std::string_view source =
      "##/+d.items / \\($item: Cell { +d.subtitle: $s: String? }) $item";
  const auto result = h.execute(source);
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{first, second}));
}

TEST(VQLLambdaGraphTest, BlockBodyCapturesAndNestedLambdaKeepLexicalScopes) {
  Harness h;
  const CellRef first      = h.cell("first");
  const CellRef firstTitle = h.cell("A");
  h.link(h.core.home(), "d.items", first);
  h.link(first, "d.title", firstTitle);
  const CellRef second      = h.cell("second");
  const CellRef secondTitle = h.cell("B");
  h.link(first, "d.items", second);
  h.link(second, "d.title", secondTitle);
  const CellRef outerTitle = h.cell("outer");

  constexpr std::string_view source = R"(
    let $title := $outer;
    ##/+d.items / \($item) {
      let $title := $item/+d.title;
      return . / \($inner) $title;
    }
  )";
  const auto result = h.execute(source, {{"outer", {outerTitle}}});
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{firstTitle, secondTitle}));
}

TEST(VQLLambdaGraphTest,
     CapturesAreResolvedAtTheCallSiteAndFormalNamesShadowThem) {
  Harness h;
  const CellRef first = h.cell("first");
  h.link(h.core.home(), "d.items", first);
  const CellRef second = h.cell("second");
  h.link(first, "d.items", second);
  const CellRef captured = h.cell("captured");

  const auto result = h.execute(
      "let $result := ##/+d.items / \\($captured) $captured; return $result",
      {{"captured", {captured}}});
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{first, second}));
}

TEST(VQLLambdaGraphTest, EmptyInputDoesNotEvaluateLambdaDefaultsOrBody) {
  Harness h;
  constexpr std::string_view source = R"(
    def weave mark(): Cell := {
      weave { ##/+d.touched/value("ran") };
      return ##;
    };
    ##/+d.missing / \($ignored := mark()) mark()
  )";
  const auto result                 = h.execute(source);
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_TRUE(result->empty());
  EXPECT_FALSE(
      h.arena.linked(h.core.home(), h.dim("d.touched"), DimVector::POS));
  EXPECT_FALSE(h.vm.hasActiveFrames());
}

TEST(VQLLambdaGraphTest, TypedLambdaReadKeepsEnclosingMutationGuardAtomic) {
  Harness h;
  const CellRef owner = h.cell("owner");
  const CellRef value = h.integer(12);
  h.link(h.core.home(), "d.items", owner);
  h.link(owner, "d.value", value);
  constexpr std::string_view source = R"(
    def weave replace($owner: Cell { +d.value: Int }): Cell := {
      let $read := ##/+d.items / \($item: Cell { +d.value: Int })
        $item/+d.value;
      weave { $owner/+d.value/value("bad") };
      return $owner;
    };
    replace($owner)
  )";

  const auto result = h.execute(source, {{"owner", {owner}}});
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().category,
            QueryExecutionError::Category::TypeContract);
  EXPECT_EQ(h.arena.linked(owner, h.dim("d.value"), DimVector::POS), value);
  EXPECT_EQ(h.arena.asInt64(value), std::optional<std::int64_t>{12});
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);
  EXPECT_FALSE(h.vm.hasActiveFrames());
  EXPECT_FALSE(h.vm.hasActiveGuards());
}

TEST(VQLLambdaGraphTest, NamedNativeFrameCanRecurseThroughItsInlineLambda) {
  Harness h;
  const CellRef item = h.cell("item");
  h.link(h.core.home(), "d.items", item);

  constexpr std::string_view source = R"(
    def pure visit($depth: Int): Int := {
      if $depth > 0
        return ##/+d.items / \($item) visit($depth + -1)
      else
        return $depth;
    };
    visit(3)
  )";
  const auto query                  = Parser(source).parseQuery();
  const auto eligibility            = analyzeFrameEligibility(query);
  ASSERT_TRUE(eligibility) << eligibility.error().message;
  EXPECT_TRUE(eligibility->reusableDefinitions.contains("visit"));

  const auto compiled = h.compile(source);
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  ASSERT_GT(countOpcode(compiled, h.vm, OpcodeKind::InvokeFrame), 0U);
  const auto result = h.execute(source);
  ASSERT_TRUE(result) << result.error().message;
  ASSERT_EQ(result->size(), 1U);
  EXPECT_EQ(h.arena.asInt64(result->front()), std::optional<std::int64_t>{0});
  EXPECT_FALSE(h.vm.hasActiveFrames());
}

TEST(VQLLambdaGraphTest, RecursiveInlineGraphExportsAndReloadsWithoutItsAst) {
  TemporaryDirectory temporary;
  auto scroll = std::make_shared<xanadu::UserPermascroll>(
      xanadu::UserPermascroll::Config{.storageDir = temporary.path / "scroll"});
  {
    Harness h;
    h.link(h.core.home(), "d.items", h.cell("item"));
    constexpr std::string_view source = R"(
      def pure visit($depth: Int): Int := {
        if $depth > 0
          return ##/+d.items / \($item) visit($depth + -1)
        else
          return $depth;
      };
      visit(2)
    )";
    VQLCompiler compiler(h.coordinator, h.vm);
    const auto compiled = compiler.compile(source);
    ASSERT_TRUE(compiled.success) << compiled.errorMessage;
    EXPECT_GT(countOpcode(compiled, h.vm, OpcodeKind::InvokeFrame), 0U);
    xanadu::Store store(scroll);
    compiler.exportToStore(store);
    store.save((temporary.path / "program").string());
  }

  xanadu::Store store(scroll);
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
      vm.run(vm.spawnCursor(*entry, "lambda-graph-reload"), 10'000);
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
  EXPECT_EQ(arena.asInt64(values.front()), std::optional<std::int64_t>{0});
  EXPECT_FALSE(vm.hasActiveFrames());
}

TEST(VQLLambdaGraphTest, HostAndMeshSourcePreserveIdentityOrderAndDuplicates) {
  constexpr std::string_view source =
      "def pure collect(...$values: Int): Stream<Int> := $values; "
      "collect(7, 9007199254740993, 7) / \\($value: Int) $value";
  const std::vector<std::int64_t> expected{7, INT64_C(9007199254740993), 7};

  zigzag::vortex::VortexHost host;
  const auto hostResult = host.executeVQLChecked(source);
  ASSERT_TRUE(hostResult) << hostResult.error().message;
  ASSERT_EQ(hostResult->size(), expected.size());
  for (std::size_t index = 0; index < expected.size(); ++index)
    EXPECT_EQ(host.core().arena().asInt64((*hostResult)[index]),
              expected[index]);

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

TEST(VQLLambdaGraphTest, MoreThanOneFormalAndVariadicLambdaAreRefused) {
  Harness h;
  for (const std::string_view source :
       {"##/d.items / \\($left, $right) $left",
        "##/d.items / \\(...$values) $values"}) {
    const auto compiled = h.compile(source);
    EXPECT_FALSE(compiled.success) << source;
    EXPECT_FALSE(compiled.cursor) << source;
  }
}

TEST(VQLLambdaGraphTest,
     InvalidLambdaContractsRefuseForEmptyAndNonemptyStaticInputs) {
  Harness h;
  const auto item = h.cell("input");
  const std::vector<std::string_view> invalidSources{
      "$items / \\($value: Widget) $value",
      "$items / \\($value: Stream<Int, Int>) $value"};

  for (const auto source : invalidSources) {
    for (const std::vector<CellRef> &input :
         {std::vector<CellRef>{}, std::vector<CellRef>{item}}) {
      CompilationOptions options;
      options.initialBindings.emplace("items", input);
      VQLCompiler compiler(h.coordinator, h.vm);
      const std::size_t beforeCells = h.arena.cells().size();
      const auto compiled           = compiler.compile(source, options);
      EXPECT_FALSE(compiled.success) << source;
      EXPECT_FALSE(compiled.cursor) << source;
      EXPECT_EQ(h.arena.cells().size(), beforeCells) << source;
      EXPECT_FALSE(
          h.arena.linked(h.core.home(), h.core.dims().spin, DimVector::POS))
          << source;
    }
  }

  const auto retry = h.compile("$items / \\($value: Int) $value");
  EXPECT_TRUE(retry.success) << retry.errorMessage;
}

TEST(VQLLambdaGraphTest, NullLambdaSelectorIsACompileRefusal) {
  Harness h;
  auto query = Parser("##/d.items / \\($value) $value").parseQuery();
  auto &path = std::get<PathExpression>(query.expr);
  path.steps.back().selector = std::shared_ptr<PathLambda>{};
  VQLCompiler compiler(h.coordinator, h.vm);
  const auto compiled = compiler.compile(query);
  EXPECT_FALSE(compiled.success);
  EXPECT_FALSE(compiled.cursor);
}

TEST(VQLLambdaGraphTest,
     ValueExpressionLambdasAndStoredClosuresStayUnsupported) {
  Harness h;
  for (const std::string_view source :
       {"let $f := \\($value) $value; return $f",
        "def pure save(): Any := \\($value) $value; save()"}) {
    const auto compiled = h.compile(source);
    EXPECT_FALSE(compiled.success) << source;
    EXPECT_FALSE(compiled.cursor) << source;
  }
}

TEST(VQLLambdaGraphTest,
     CyclicAndMalformedLambdaParameterAstRefuseCompilation) {
  Harness h;
  auto query = Parser("##/d.items / \\($value: Int) $value").parseQuery();
  auto &path = std::get<PathExpression>(query.expr);
  auto lambda =
      std::get<std::shared_ptr<PathLambda>>(path.steps.back().selector);
  ASSERT_TRUE(lambda);
  auto cycle = std::make_shared<TypeExpr>();
  cycle->unionType.members.push_back(PrimaryType{.kind = cycle});
  lambda->params.front().type = cycle;
  VQLCompiler compiler(h.coordinator, h.vm);
  const auto compiled = compiler.compile(query);
  cycle->unionType.members.clear();
  EXPECT_FALSE(compiled.success);
  EXPECT_FALSE(compiled.cursor);
}

TEST(VQLLambdaGraphTest, FailedLambdaCompilationRollsBackAndAllowsAValidRetry) {
  Harness h;
  const auto invalid = h.compile("##/d.items / \\($left, $right) $left");
  EXPECT_FALSE(invalid.success);
  EXPECT_FALSE(invalid.cursor);

  const auto valid = h.compile("##/d.items / \\($value) $value");
  ASSERT_TRUE(valid.success) << valid.errorMessage;
  EXPECT_TRUE(valid.cursor);
}

} // namespace
} // namespace xanadu::vql
