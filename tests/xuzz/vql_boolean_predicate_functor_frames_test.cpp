/**
 * @file vql_boolean_predicate_functor_frames_test.cpp
 * @brief Flat Boolean predicates in recursive VQL functor frames.
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
  CellRef real(const double value) { return arena.makeScalarCell(value); }

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
      ("vql_boolean_predicate_functor_frames_" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));

  TemporaryDirectory() { std::filesystem::create_directories(path); }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

constexpr std::string_view recursiveDefinition =
    "def pure walk($nodes: Stream<Cell>, $depth: Int, $route): "
    "Stream<Cell> := { if $depth > 0 return walk($nodes/$route, "
    "$depth + -1, $route) else return $nodes; }; ";

BooleanExpr &predicateInTopLevelWalk(QueryExpression &query,
                                     const std::size_t statementIndex) {
  auto &statement  = query.body.statements.at(statementIndex);
  auto &path       = std::get<PathExpression>(statement);
  auto &invocation = std::get<FunctionInvocation>(path.steps.front().selector);
  auto &route =
      std::get<std::shared_ptr<PathExpression>>(invocation.args.at(2).kind);
  return route->steps.front().predicates.front();
}

ComparisonExpr &comparisonAt(BooleanExpr &predicate,
                             const std::size_t termIndex,
                             const std::size_t factorIndex) {
  return std::get<ComparisonExpr>(
      predicate.terms.at(termIndex).factors.at(factorIndex).test);
}

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

std::vector<CellRef> runCompiled(Harness &h, const QueryExpression &query,
                                 const std::vector<CellRef> &nodes) {
  CompilationOptions options;
  options.initialBindings.emplace("nodes", nodes);
  const auto compiled = h.compile(query, options);
  EXPECT_TRUE(compiled.success) << compiled.errorMessage;
  if (!compiled.success || !compiled.cursor) return {};
  const auto execution = h.vm.run(*compiled.cursor, 100'000);
  h.vm.setCursorOpcode(*compiled.cursor, std::nullopt);
  EXPECT_TRUE(execution.success) << execution.errorMessage;
  if (!execution.success) return {};
  std::vector<CellRef> result;
  for (const CellRef slot : compiled.resultSlots) {
    const auto cells = h.vm.resultCells(slot);
    result.insert(result.end(), cells.begin(), cells.end());
  }
  return result;
}

std::string oneBooleanFactorSource(const std::size_t factorCount) {
  std::string source(recursiveDefinition);
  source += "walk(##, 1, /+d.next[. = \"keep\"";
  for (std::size_t index = 1; index < factorCount; ++index)
    source += " and . = \"keep\"";
  source += "]);";
  return source;
}

TEST(VQLBooleanPredicateFunctorFrameTest,
     RecursiveFramesPreserveAndOrNotPrecedenceAndOutputOrder) {
  Harness h;
  const CellRef allowOwner    = h.cell("allow owner");
  const CellRef denyOwner     = h.cell("deny owner");
  const CellRef fallbackOwner = h.cell("fallback owner");
  const CellRef otherOwner    = h.cell("other owner");
  const CellRef allow         = h.cell("allow");
  const CellRef deny          = h.cell("deny");
  const CellRef fallback      = h.cell("fallback");
  const CellRef other         = h.cell("other");
  h.link(allowOwner, "d.next", allow);
  h.link(denyOwner, "d.next", deny);
  h.link(fallbackOwner, "d.next", fallback);
  h.link(otherOwner, "d.next", other);
  const std::vector<CellRef> owners{allowOwner, denyOwner, fallbackOwner,
                                    otherOwner, allowOwner};

  constexpr std::string_view source =
      "def pure walk($nodes: Stream<Cell>, $depth: Int, $route): "
      "Stream<Cell> := { if $depth > 0 return walk($nodes/$route, "
      "$depth + -1, $route) else return $nodes; }; "
      "walk($owners, 1, /+d.next[. = \"allow\" and not . = \"deny\" "
      "or \"fallback\" = .])";
  auto query      = Parser(source).parseQuery();
  auto &predicate = predicateInTopLevelWalk(query, 0);
  ASSERT_EQ(predicate.terms.size(), 2U);
  ASSERT_EQ(predicate.terms[0].factors.size(), 2U);
  ASSERT_EQ(predicate.terms[1].factors.size(), 1U);
  EXPECT_TRUE(predicate.terms[0].factors[1].negated);

  const auto compiled =
      h.compile(source, {.initialBindings = {{"owners", owners}}});
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  EXPECT_FALSE(namedInvokeDescriptors(compiled, h.vm, h.core, "walk").empty());
  const auto result = h.execute(source, {{"owners", owners}});
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{allow, fallback, allow}));
  EXPECT_FALSE(h.vm.hasActiveFrames());
  const std::string clampedSource = std::string(source) + "[1]";
  const auto clamped = h.execute(clampedSource, {{"owners", owners}});
  ASSERT_TRUE(clamped) << clamped.error().message;
  EXPECT_EQ(*clamped, (std::vector<CellRef>{allow}));

  constexpr std::string_view mutualSource =
      "def pure even($nodes: Stream<Cell>, $depth: Int, $route): "
      "Stream<Cell> := { if $depth > 0 return odd($nodes/$route, "
      "$depth + -1, $route) else return $nodes; }; "
      "def pure odd($nodes: Stream<Cell>, $depth: Int, $route): "
      "Stream<Cell> := { if $depth > 0 return even($nodes/$route, "
      "$depth + -1, $route) else return $nodes; }; "
      "even($owners, 1, /+d.next[. = \"allow\" and not . = \"deny\" "
      "or \"fallback\" = .])";
  const auto mutual = h.execute(mutualSource, {{"owners", owners}});
  ASSERT_TRUE(mutual) << mutual.error().message;
  EXPECT_EQ(*mutual, (std::vector<CellRef>{allow, fallback, allow}));
  EXPECT_FALSE(h.vm.hasActiveFrames());
}

TEST(VQLBooleanPredicateFunctorFrameTest,
     BooleanStructureAndFactorFlagsDefineDeduplicatedFrameVariants) {
  Harness h;
  const std::string source =
      std::string(recursiveDefinition) +
      "walk($nodes, 1, /+d.next[. = \"a\" and . != \"b\" or \"c\" = .]);"
      "walk($nodes, 1, /+d.next[. = \"a\" and . != \"b\" or \"c\" = .]);"
      "walk($emptyNodes, 1, /+d.next[. = \"a\" and . != \"b\" or \"c\" = .]);"
      "walk($nodes, 1, /+d.next[. = \"a\" and . != \"b\" or \"c\" = .]);"
      "walk($nodes, 1, /+d.next[. = \"a\" and . != \"b\" or \"c\" = .]);"
      "walk($nodes, 1, /+d.next[. = \"a\" and . != \"b\" or \"c\" = .]);"
      "walk($nodes, 1, /+d.next[. = \"a\" and . != \"b\" or \"c\" = .]);"
      "walk($nodes, 1, /+d.next[. = \"a\" and . != \"b\" or \"c\" = .]);"
      "walk($nodes, 1, /+d.next[. = \"a\" and . != \"b\" or \"c\" = .]);"
      "walk($nodes, 1, /+d.next[. = \"a\" and . != \"b\" or \"c\" = .]);";
  auto query = Parser(source).parseQuery();
  std::swap(predicateInTopLevelWalk(query, 3).terms[0].factors[0],
            predicateInTopLevelWalk(query, 3).terms[0].factors[1]);
  std::swap(predicateInTopLevelWalk(query, 4).terms[0],
            predicateInTopLevelWalk(query, 4).terms[1]);
  predicateInTopLevelWalk(query, 5).terms[0].factors[1].negated = true;
  comparisonAt(predicateInTopLevelWalk(query, 6), 0, 0).op = CompOp::LessThan;
  comparisonAt(predicateInTopLevelWalk(query, 7), 0, 0).right->kind =
      ScalarLiteral{.value = std::int64_t{1}};
  comparisonAt(predicateInTopLevelWalk(query, 8), 0, 0).right->kind =
      ScalarLiteral{.value = std::string("other")};

  auto &regrouped  = predicateInTopLevelWalk(query, 9);
  auto movedFactor = std::move(regrouped.terms[0].factors.back());
  regrouped.terms[0].factors.pop_back();
  regrouped.terms[1].factors.insert(regrouped.terms[1].factors.begin(),
                                    std::move(movedFactor));

  CompilationOptions options;
  options.initialBindings.emplace("nodes", std::vector<CellRef>{h.cell("n")});
  options.initialBindings.emplace("emptyNodes", std::vector<CellRef>{});
  const auto compiled = h.compile(query, options);
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  const auto descriptors =
      namedInvokeDescriptors(compiled, h.vm, h.core, "walk");
  ASSERT_GT(descriptors.size(), 6U);
  const auto unique =
      uniqueNamedInvokeDescriptors(compiled, h.vm, h.core, "walk");
  EXPECT_EQ(unique.size(), 8U);
  for (const CellRef descriptor : unique) {
    const auto record = h.vm.frameDescriptor(descriptor);
    ASSERT_TRUE(record) << record.error().message;
    EXPECT_TRUE(record->entry);
  }
}

TEST(VQLBooleanPredicateFunctorFrameTest,
     ProgrammaticQuietNanKeepsIeeeNegationDistinctFromGreaterEqual) {
  Harness h;
  const CellRef owner = h.cell("owner");
  const CellRef value = h.real(4.0);
  h.link(owner, "d.next", value);
  constexpr std::string_view prefix =
      "def pure walk($nodes: Stream<Cell>, $depth: Int, $route): "
      "Stream<Cell> := { if $depth > 0 return walk($nodes/$route, "
      "$depth + -1, $route) else return $nodes; }; ";

  const auto runNaNPredicate = [&](const CompOp op, const bool negated) {
    auto query =
        Parser(std::string(prefix) + "walk($nodes, 1, /+d.next[. < 1.0])")
            .parseQuery();
    auto &predicate  = predicateInTopLevelWalk(query, 0);
    auto &factor     = predicate.terms.front().factors.front();
    auto &comparison = std::get<ComparisonExpr>(factor.test);
    comparison.op    = op;
    factor.negated   = negated;
    std::get<ScalarLiteral>(comparison.right->kind).value =
        std::bit_cast<double>(UINT64_C(0x7ff8000000000001));
    return runCompiled(h, query, {owner});
  };

  EXPECT_TRUE(runNaNPredicate(CompOp::LessThan, false).empty());
  EXPECT_TRUE(runNaNPredicate(CompOp::GreaterEqual, false).empty());
  EXPECT_EQ(runNaNPredicate(CompOp::LessThan, true),
            (std::vector<CellRef>{value}));
}

TEST(VQLBooleanPredicateFunctorFrameTest,
     FreeComputedAndTruthinessPredicatesUseNativeTemplates) {
  Harness h;
  const auto owner = h.cell("owner");
  const auto value = h.cell("3");
  h.link(h.core.home(), "d.items", owner);
  h.link(owner, "d.next", value);
  for (const std::string_view route :
       {"/+d.next[. = $wanted]", "/+d.next[. = (1 + 2)]", "/+d.next[.]"}) {
    const std::string source = std::string(recursiveDefinition) +
                               "walk(##/+d.items, 1, " + std::string(route) +
                               ")";
    CompilationOptions options;
    options.initialBindings.emplace("wanted", std::vector<CellRef>{value});
    const auto compiled = h.compile(source, options);
    ASSERT_TRUE(compiled.success) << compiled.errorMessage;
    EXPECT_FALSE(
        namedInvokeDescriptors(compiled, h.vm, h.core, "walk").empty());
    const auto result = h.execute(source, options.initialBindings);
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(*result, (std::vector<CellRef>{value}));
  }
}

TEST(VQLBooleanPredicateFunctorFrameTest,
     MalformedAndCyclicBooleanPredicateTreesRefuseRecursion) {
  Harness h;
  const auto owner = h.cell("owner");
  const auto leaf  = h.cell("leaf");
  h.link(h.core.home(), "d.items", owner);
  h.link(owner, "d.next", leaf);
  constexpr std::string_view source =
      "def pure walk($nodes: Stream<Cell>, $depth: Int, $route): "
      "Stream<Cell> := { if $depth > 0 return walk($nodes/$route, "
      "$depth + -1, $route) else return $nodes; }; "
      "walk(##/+d.items, 1, /+d.next[. = 1])";
  const auto parse         = [&] { return Parser(source).parseQuery(); };
  const auto expectRefused = [&](QueryExpression query) {
    const auto refused = h.compile(query);
    EXPECT_FALSE(refused.success);
    EXPECT_FALSE(refused.cursor);
  };

  auto emptyTerms = parse();
  predicateInTopLevelWalk(emptyTerms, 0).terms.clear();
  expectRefused(std::move(emptyTerms));

  auto emptyFactors = parse();
  predicateInTopLevelWalk(emptyFactors, 0).terms.front().factors.clear();
  expectRefused(std::move(emptyFactors));

  auto nested = parse();
  auto &nestedComparison =
      comparisonAt(predicateInTopLevelWalk(nested, 0), 0, 0);
  auto nestedBoolean = std::make_shared<BooleanExpr>();
  nestedBoolean->terms.push_back(BooleanTerm{
      .factors = {BooleanFactor{
          .test = ComparisonExpr{
              .left = std::make_shared<ValueExpr>(
                  ValueExpr{.kind = ScalarLiteral{.value = std::string("x")}}),
              .op    = CompOp::Equal,
              .right = std::make_shared<ValueExpr>(ValueExpr{
                  .kind = ScalarLiteral{.value = std::string("x")}})}}}});
  nestedComparison.left =
      std::make_shared<ValueExpr>(ValueExpr{.kind = std::move(nestedBoolean)});
  nestedComparison.right = std::make_shared<ValueExpr>(
      ValueExpr{.kind = ScalarLiteral{.value = true}});
  const auto nestedCompiled = h.compile(nested);
  ASSERT_TRUE(nestedCompiled.success) << nestedCompiled.errorMessage;
  EXPECT_FALSE(
      namedInvokeDescriptors(nestedCompiled, h.vm, h.core, "walk").empty());
  ASSERT_TRUE(nestedCompiled.cursor);
  const auto execution = h.vm.run(*nestedCompiled.cursor, 100'000);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  std::vector<CellRef> values;
  for (const auto slot : nestedCompiled.resultSlots) {
    const auto members = h.vm.resultCells(slot);
    values.insert(values.end(), members.begin(), members.end());
  }
  EXPECT_EQ(values, (std::vector<CellRef>{leaf}));
  h.vm.setCursorOpcode(*nestedCompiled.cursor, std::nullopt);

  auto cyclic     = parse();
  auto cycle      = std::make_shared<BooleanExpr>();
  auto cycleValue = std::make_shared<ValueExpr>(ValueExpr{.kind = cycle});
  cycle->terms.push_back(BooleanTerm{
      .factors = {BooleanFactor{
          .test = ComparisonExpr{
              .left  = cycleValue,
              .op    = CompOp::Equal,
              .right = std::make_shared<ValueExpr>(ValueExpr{
                  .kind = ScalarLiteral{.value = std::int64_t{1}}})}}}});
  auto &cycleComparison =
      comparisonAt(predicateInTopLevelWalk(cyclic, 0), 0, 0);
  cycleComparison.left = std::move(cycleValue);
  expectRefused(std::move(cyclic));
  cycle->terms.clear();
}

TEST(VQLBooleanPredicateFunctorFrameTest,
     EligibilityNodeBudgetCountsFactorsInsideOneClosedPredicate) {
  constexpr std::size_t maxNodes = 1024;
  const auto one       = Parser(oneBooleanFactorSource(1)).parseQuery();
  const auto oneResult = analyzeFrameEligibility(one, {.maxNodes = maxNodes});
  ASSERT_TRUE(oneResult) << oneResult.error().message;
  EXPECT_TRUE(oneResult->reusableDefinitions.contains("walk"));

  const auto many       = Parser(oneBooleanFactorSource(1000)).parseQuery();
  const auto manyResult = analyzeFrameEligibility(many, {.maxNodes = maxNodes});
  ASSERT_FALSE(manyResult);
  EXPECT_NE(manyResult.error().message.find("node budget"), std::string::npos);
}

TEST(VQLBooleanPredicateFunctorFrameTest,
     PredicateTemplateBudgetWeightsEveryStringLiteralPayload) {
  Harness h;
  const auto sourceFor = [&](const std::string_view secondLiteral) {
    return std::string(recursiveDefinition) +
           "walk(##, 1, /+d.next[. = \"x\" and . = \"" +
           std::string(secondLiteral) + "\"]);";
  };
  CompilationOptions options;
  options.limits.maxClosedFunctorTemplateWork = 1000;
  const auto shortPayload = h.compile(sourceFor("x"), options);
  ASSERT_TRUE(shortPayload.success) << shortPayload.errorMessage;
  const auto before = h.arena.cells().size();
  const auto longPayload =
      h.compile(sourceFor(std::string(1024, 'x')), options);
  EXPECT_FALSE(longPayload.success);
  EXPECT_FALSE(longPayload.cursor);
  EXPECT_EQ(h.arena.cells().size(), before);
  EXPECT_NE(longPayload.errorMessage.find(
                "compiled VQL closed functor template work budget exhausted"),
            std::string::npos);
  options.limits.maxClosedFunctorTemplateWork = 100'000;
  const auto retry = h.compile(sourceFor(std::string(1024, 'x')), options);
  EXPECT_TRUE(retry.success) << retry.errorMessage;
}

TEST(VQLBooleanPredicateFunctorFrameTest,
     NativeBooleanPredicateExportHostAndMeshPreserveSourceResults) {
  constexpr std::string_view source =
      "def pure visit($nodes: Stream<Cell>, $depth: Int, $route): "
      "Stream<Cell> := { if $depth > 0 return visit($nodes/$route, "
      "$depth + -1, $route) else return $nodes; }; "
      "let $owner := ##/d.seed%\"owner\"; "
      "let $leaf := $owner/d.next%\"keep\"; "
      "visit($owner, 1, /+d.next[. = \"keep\" and not . = \"skip\" "
      "or \"fallback\" = .])";
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
        vm.run(vm.spawnCursor(*entry, "boolean-predicate-reload"), 100'000);
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
