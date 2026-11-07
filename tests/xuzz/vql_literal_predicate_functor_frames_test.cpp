/**
 * @file vql_literal_predicate_functor_frames_test.cpp
 * @brief Closed scalar predicates in recursive VQL functor frames.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
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
  CellRef real(const double value) { return arena.makeScalarCell(value); }
  CellRef boolean(const bool value) { return arena.makeScalarCell(value); }

  void link(const CellRef from, const std::string_view dimension,
            const CellRef to) {
    zigzag::expectWritten(arena.link(from, dim(dimension), DimVector::POS, to));
  }

  CompilationResult compile(const std::string_view source,
                            CompilationOptions options = {}) {
    VQLCompiler compiler(coordinator, vm);
    return compiler.compile(source, options);
  }

  CompilationResult compile(const QueryExpression &query,
                            CompilationOptions options = {}) {
    VQLCompiler compiler(coordinator, vm);
    return compiler.compile(query, options);
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
      ("vql_literal_predicate_functor_frames_" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));

  TemporaryDirectory() { std::filesystem::create_directories(path); }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

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

constexpr std::string_view recursiveDefinition =
    "def pure walk($nodes: Stream<Cell>, $depth: Int, $route): "
    "Stream<Cell> := { if $depth > 0 return walk($nodes/$route, "
    "$depth + -1, $route) else return $nodes; }; ";

ScalarLiteral &literalInTopLevelWalk(QueryExpression &query,
                                     const std::size_t statementIndex) {
  auto &statement  = query.body.statements.at(statementIndex);
  auto &path       = std::get<PathExpression>(statement);
  auto &invocation = std::get<FunctionInvocation>(path.steps.front().selector);
  auto &route =
      std::get<std::shared_ptr<PathExpression>>(invocation.args.at(2).kind);
  auto &predicate = route->steps.front().predicates.front();
  auto &comparison =
      std::get<ComparisonExpr>(predicate.terms.front().factors.front().test);
  return std::get<ScalarLiteral>(comparison.right->kind);
}

std::string repeatedPredicateSource(const std::size_t count) {
  std::string source(recursiveDefinition);
  source += "walk(##, 1, /+d.next";
  for (std::size_t index = 0; index < count; ++index) source += "[. = 1]";
  source += ");";
  return source;
}

TEST(VQLLiteralPredicateFunctorFrameTest,
     SelfAndMutualRecursionFilterInOrderAndKeepDuplicateIdentities) {
  Harness h;
  const CellRef firstOwner  = h.cell("first owner");
  const CellRef secondOwner = h.cell("second owner");
  const CellRef firstValue  = h.cell("keep");
  const CellRef secondValue = h.cell("skip");
  h.link(firstOwner, "d.next", firstValue);
  h.link(secondOwner, "d.next", secondValue);
  const std::vector<CellRef> owners{firstOwner, secondOwner, firstOwner};

  constexpr std::string_view selfSource =
      "def pure walk($nodes: Stream<Cell>, $depth: Int, $route): "
      "Stream<Cell> := { if $depth > 0 return walk($nodes/$route, "
      "$depth + -1, $route) else return $nodes; }; "
      "walk($owners, 1, /+d.next[. = \"keep\"][. != \"skip\"] )";
  const auto compiled =
      h.compile(selfSource, {.initialBindings = {{"owners", owners}}});
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  const auto invokes = namedInvokeDescriptors(compiled, h.vm, h.core, "walk");
  ASSERT_FALSE(invokes.empty());
  const auto selfResult = h.execute(selfSource, {{"owners", owners}});
  ASSERT_TRUE(selfResult) << selfResult.error().message;
  EXPECT_EQ(*selfResult, (std::vector<CellRef>{firstValue, firstValue}));
  EXPECT_FALSE(h.vm.hasActiveFrames());

  constexpr std::string_view mutualSource =
      "def pure even($nodes: Stream<Cell>, $depth: Int, $route): "
      "Stream<Cell> := { if $depth > 0 return odd($nodes/$route, "
      "$depth + -1, $route) else return $nodes; }; "
      "def pure odd($nodes: Stream<Cell>, $depth: Int, $route): "
      "Stream<Cell> := { if $depth > 0 return even($nodes/$route, "
      "$depth + -1, $route) else return $nodes; }; "
      "even($owners, 1, /+d.next[\"keep\" = .])";
  const auto mutualResult = h.execute(mutualSource, {{"owners", owners}});
  ASSERT_TRUE(mutualResult) << mutualResult.error().message;
  EXPECT_EQ(*mutualResult, (std::vector<CellRef>{firstValue, firstValue}));
  EXPECT_FALSE(h.vm.hasActiveFrames());
}

TEST(VQLLiteralPredicateFunctorFrameTest,
     TypedLiteralValuesUseExactKeysAndExecuteAsOrdinaryComparisons) {
  Harness h;
  const CellRef intOwner    = h.cell("int owner");
  const CellRef doubleOwner = h.cell("double owner");
  const CellRef stringOwner = h.cell("string owner");
  const CellRef boolOwner   = h.cell("bool owner");
  const CellRef zeroOwner   = h.cell("zero owner");
  const CellRef intValue    = h.integer(INT64_C(9007199254740993));
  const CellRef doubleValue = h.real(0x1.0000000000000p53);
  const CellRef stringValue = h.cell("");
  const CellRef boolValue   = h.boolean(false);
  const CellRef zeroValue   = h.integer(0);
  h.link(intOwner, "d.next", intValue);
  h.link(doubleOwner, "d.next", doubleValue);
  h.link(stringOwner, "d.next", stringValue);
  h.link(boolOwner, "d.next", boolValue);
  h.link(zeroOwner, "d.next", zeroValue);

  constexpr std::string_view source =
      "def pure walk($nodes: Stream<Cell>, $depth: Int, $route): "
      "Stream<Cell> := { if $depth > 0 return walk($nodes/$route, "
      "$depth + -1, $route) else return $nodes; }; "
      "let $wide := walk($wideOwners, 1, /+d.next[. = 9007199254740993]); "
      "let $floating := walk($wideOwners, 1, /+d.next[. = "
      "9007199254740992.0]); "
      "let $empty := walk($stringOwner, 1, /+d.next[. = \"\"]); "
      "let $boolean := walk($boolOwner, 1, /+d.next[false = .]); "
      "let $zero := walk($zeroOwner, 1, /+d.next[. = 0]); "
      "return $wide, $floating, $empty, $boolean, $zero";
  const auto result =
      h.execute(source, {{"wideOwners", {intOwner, doubleOwner}},
                         {"stringOwner", {stringOwner}},
                         {"boolOwner", {boolOwner}},
                         {"zeroOwner", {zeroOwner}}});
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{intValue, doubleValue, stringValue,
                                           boolValue, zeroValue}));
}

TEST(VQLLiteralPredicateFunctorFrameTest,
     VariantKeyIncludesPredicateOrderOperationOperandOrderTypeAndValue) {
  Harness h;
  const std::string source =
      std::string(recursiveDefinition) +
      "walk($nodes, 1, /+d.next[. = 1]);"
      "walk($nodes, 1, /+d.next[. = 1]);"
      "walk($emptyNodes, 1, /+d.next[. = 1]);"
      "walk($nodes, 1, /+d.next[. = 9007199254740992]);"
      "walk($nodes, 1, /+d.next[. = 9007199254740993]);"
      "walk($nodes, 1, /+d.next[. = 9007199254740992.0]);"
      "walk($nodes, 1, /+d.next[. = 1]);"
      "walk($nodes, 1, /+d.next[. != 1]);"
      "walk($nodes, 1, /+d.next[. < 1]);"
      "walk($nodes, 1, /+d.next[. > 1]);"
      "walk($nodes, 1, /+d.next[. <= 1]);"
      "walk($nodes, 1, /+d.next[. >= 1]);"
      "walk($nodes, 1, /+d.next[. = \"1\"]);"
      "walk($nodes, 1, /+d.next[. = true]);"
      "walk($nodes, 1, /+d.next[. = false]);"
      "walk($nodes, 1, /+d.next[. = \"\"]);"
      "walk($nodes, 1, /+d.next[. = 1][. = 2]);"
      "walk($nodes, 1, /+d.next[. = 2][. = 1]);";
  auto query = Parser(source).parseQuery();
  // A numeric-leading bracket is parsed as an index window in source syntax.
  auto &leftPath = std::get<PathExpression>(query.body.statements.at(6));
  auto &leftCall =
      std::get<FunctionInvocation>(leftPath.steps.front().selector);
  auto &leftRoute =
      std::get<std::shared_ptr<PathExpression>>(leftCall.args.at(2).kind);
  auto &leftComparison = std::get<ComparisonExpr>(leftRoute->steps.front()
                                                      .predicates.front()
                                                      .terms.front()
                                                      .factors.front()
                                                      .test);
  std::swap(leftComparison.left, leftComparison.right);
  CompilationOptions options;
  options.initialBindings.emplace("nodes", std::vector<CellRef>{h.cell("n")});
  options.initialBindings.emplace("emptyNodes", std::vector<CellRef>{});
  const auto compiled = h.compile(query, options);
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  const auto descriptors =
      namedInvokeDescriptors(compiled, h.vm, h.core, "walk");
  EXPECT_GT(descriptors.size(), 12U);
  const auto unique =
      uniqueNamedInvokeDescriptors(compiled, h.vm, h.core, "walk");
  EXPECT_EQ(unique.size(), 16U);
  for (const CellRef descriptor : unique) {
    const auto record = h.vm.frameDescriptor(descriptor);
    ASSERT_TRUE(record) << record.error().message;
    EXPECT_TRUE(record->entry);
  }
}

TEST(VQLLiteralPredicateFunctorFrameTest,
     DoubleTemplateKeysCanonicalizeSignedZeroAndQuietNanPayloads) {
  Harness h;
  const std::string source = std::string(recursiveDefinition) +
                             "walk($nodes, 1, /+d.next[. = 1.0]);"
                             "walk($nodes, 1, /+d.next[. = 1.0]);"
                             "walk($nodes, 1, /+d.next[. = 1.0]);"
                             "walk($nodes, 1, /+d.next[. = 1.0]);";
  auto query                            = Parser(source).parseQuery();
  literalInTopLevelWalk(query, 0).value = -0.0;
  literalInTopLevelWalk(query, 1).value = 0.0;
  literalInTopLevelWalk(query, 2).value =
      std::bit_cast<double>(UINT64_C(0x7ff8000000000001));
  literalInTopLevelWalk(query, 3).value =
      std::bit_cast<double>(UINT64_C(0x7ff8000000000042));
  CompilationOptions options;
  options.initialBindings.emplace("nodes", std::vector<CellRef>{h.cell("n")});
  const auto compiled = h.compile(query, options);
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  EXPECT_EQ(uniqueNamedInvokeDescriptors(compiled, h.vm, h.core, "walk").size(),
            2U);

  const CellRef numericOwner = options.initialBindings.at("nodes").front();
  const CellRef numericValue = h.real(1.0);
  h.link(numericOwner, "d.next", numericValue);
  for (const CompOp op : {CompOp::LessThan, CompOp::NotEqual}) {
    auto runtimeQuery = Parser(std::string(recursiveDefinition) +
                               "walk($nodes, 1, /+d.next[. = 1.0])")
                            .parseQuery();
    literalInTopLevelWalk(runtimeQuery, 0).value =
        std::bit_cast<double>(UINT64_C(0x7ff8000000000001));
    auto &path = std::get<PathExpression>(runtimeQuery.body.statements.front());
    auto &call = std::get<FunctionInvocation>(path.steps.front().selector);
    auto &route =
        std::get<std::shared_ptr<PathExpression>>(call.args.at(2).kind);
    std::get<ComparisonExpr>(route->steps.front()
                                 .predicates.front()
                                 .terms.front()
                                 .factors.front()
                                 .test)
        .op                    = op;
    const auto runtimeCompiled = h.compile(runtimeQuery, options);
    ASSERT_TRUE(runtimeCompiled.success) << runtimeCompiled.errorMessage;
    ASSERT_TRUE(runtimeCompiled.cursor);
    const auto execution = h.vm.run(*runtimeCompiled.cursor, 100'000);
    ASSERT_TRUE(execution.success) << execution.errorMessage;
    std::vector<CellRef> results;
    for (const CellRef slot : runtimeCompiled.resultSlots) {
      const auto cells = h.vm.resultCells(slot);
      results.insert(results.end(), cells.begin(), cells.end());
    }
    h.vm.setCursorOpcode(*runtimeCompiled.cursor, std::nullopt);
    if (op == CompOp::LessThan)
      EXPECT_TRUE(results.empty());
    else
      EXPECT_EQ(results, (std::vector<CellRef>{numericValue}));
  }

  auto signallingQuery = Parser(source).parseQuery();
  literalInTopLevelWalk(signallingQuery, 0).value =
      std::bit_cast<double>(UINT64_C(0x7ff0000000000001));
  const std::size_t before = h.arena.cells().size();
  const auto refused       = h.compile(signallingQuery, options);
  EXPECT_FALSE(refused.success);
  EXPECT_FALSE(refused.cursor);
  EXPECT_EQ(h.arena.cells().size(), before);
  auto retryQuery                            = Parser(source).parseQuery();
  literalInTopLevelWalk(retryQuery, 0).value = 1.0;
  const auto retry                           = h.compile(retryQuery, options);
  EXPECT_TRUE(retry.success) << retry.errorMessage;
}

TEST(VQLLiteralPredicateFunctorFrameTest,
     PredicatesFilterTheCombinedRankBeforeTheStaticClamp) {
  Harness h;
  const CellRef firstOwner  = h.cell("first owner");
  const CellRef secondOwner = h.cell("second owner");
  const CellRef thirdOwner  = h.cell("third owner");
  const CellRef discarded   = h.cell("discard");
  const CellRef firstMatch  = h.cell("pass");
  const CellRef secondMatch = h.cell("pass");
  h.link(firstOwner, "d.next", discarded);
  h.link(secondOwner, "d.next", firstMatch);
  h.link(thirdOwner, "d.next", secondMatch);
  const std::vector<CellRef> owners{firstOwner, secondOwner, thirdOwner};

  constexpr std::string_view source =
      "def pure walk($nodes: Stream<Cell>, $depth: Int, $route): "
      "Stream<Cell> := { if $depth > 0 return walk($nodes/$route, "
      "$depth + -1, $route) else return $nodes; }; "
      "walk($owners, 1, /+d.next[. = \"pass\"][1])";
  const auto result = h.execute(source, {{"owners", owners}});
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{firstMatch}));
}

TEST(VQLLiteralPredicateFunctorFrameTest,
     CallerRebindingKeepsFreePredicateInlineAndFinite) {
  Harness h;
  const CellRef owner  = h.cell("owner");
  const CellRef first  = h.cell("one");
  const CellRef second = h.cell("two");
  h.link(owner, "d.next", first);
  const CellRef secondOwner = h.cell("second owner");
  h.link(secondOwner, "d.next", second);

  constexpr std::string_view source = R"(
    def pure apply($nodes: Stream<Cell>, $route): Stream<Cell> := $nodes/$route;
    let $wanted := "one";
    let $before := apply($owners, /+d.next[. = $wanted]);
    let $wanted := "two";
    let $after := apply($otherOwner, /+d.next[. = $wanted]);
    return $before, $after
  )";
  const auto result =
      h.execute(source, {{"owners", {owner}}, {"otherOwner", {secondOwner}}});
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{first, second}));
}

TEST(VQLLiteralPredicateFunctorFrameTest,
     EligibilityCountsEveryLiteralPredicateBeforeTemplateSpecialization) {
  constexpr std::size_t maxNodes = 1024;
  const auto oneQuery = Parser(repeatedPredicateSource(1)).parseQuery();
  const auto one = analyzeFrameEligibility(oneQuery, {.maxNodes = maxNodes});
  ASSERT_TRUE(one) << one.error().message;
  EXPECT_TRUE(one->reusableDefinitions.contains("walk"));

  const auto manyQuery = Parser(repeatedPredicateSource(1000)).parseQuery();
  const auto many = analyzeFrameEligibility(manyQuery, {.maxNodes = maxNodes});
  ASSERT_FALSE(many);
  EXPECT_NE(many.error().message.find("node budget"), std::string::npos);
}

TEST(VQLLiteralPredicateFunctorFrameTest,
     TemplateWorkWeightsLiteralBytesAndRefusalCanRetry) {
  Harness h;
  const auto sourceFor = [](const std::string_view literal) {
    return std::string(recursiveDefinition) + "walk(##, 1, /+d.next[. = \"" +
           std::string(literal) + "\"] )";
  };
  CompilationOptions limited;
  limited.limits.maxClosedFunctorTemplateWork = 500;
  const auto ordinary =
      h.compile("def pure identity($value: Cell): Cell := $value; identity(##)",
                {.limits = {.maxClosedFunctorTemplateWork = 0}});
  ASSERT_TRUE(ordinary.success) << ordinary.errorMessage;
  const auto shortLiteral = h.compile(sourceFor("x"), limited);
  ASSERT_TRUE(shortLiteral.success) << shortLiteral.errorMessage;
  const auto longLiteral =
      h.compile(sourceFor(std::string(1024, 'x')), limited);
  EXPECT_FALSE(longLiteral.success);
  EXPECT_FALSE(longLiteral.cursor);
  EXPECT_NE(longLiteral.errorMessage.find(
                "compiled VQL closed functor template work budget exhausted"),
            std::string::npos);

  CompilationOptions retryOptions;
  retryOptions.limits.maxClosedFunctorTemplateWork = 100'000;
  const auto retry = h.compile(sourceFor(std::string(1024, 'x')), retryOptions);
  EXPECT_TRUE(retry.success) << retry.errorMessage;
}

TEST(VQLLiteralPredicateFunctorFrameTest,
     RecursiveExportHostAndMeshKeepThePredicateInTheCompiledSource) {
  constexpr std::string_view source =
      "def pure visit($nodes: Stream<Cell>, $depth: Int, $route): "
      "Stream<Cell> := { if $depth > 0 return visit($nodes/$route, "
      "$depth + -1, $route) else return $nodes; }; "
      "let $owner := ##/d.seed%\"owner\"; "
      "let $leaf := $owner/d.next%\"keep\"; "
      "visit($owner, 1, /+d.next[. = \"keep\"] )";

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
        vm.run(vm.spawnCursor(*entry, "literal-predicate-reload"), 100'000);
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
