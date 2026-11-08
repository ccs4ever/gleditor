/**
 * @file vql_return_value_frames_test.cpp
 * @brief Parenthesized return expressions lowered to native Vortex frames.
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
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_host.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/vql/execution.hpp"
#include "common/xanadu/vql/parser.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"

namespace xanadu::vql {
namespace {

using zigzag::CellRef;
using zigzag::DimVector;
using zigzag::vortex::OpcodeKind;

struct TemporaryDirectory {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("vql_return_value_frames_" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));

  TemporaryDirectory() { std::filesystem::create_directories(path); }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

struct Harness {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  MultiStoreCoordinator coordinator{core};
  zigzag::vortex::VortexVM vm{core};

  std::expected<std::vector<CellRef>, QueryExecutionError>
  run(const std::string_view source, ContractBindings bindings = {},
      const QueryExecutionMode mode = QueryExecutionMode::Vortex) {
    QueryExecutor executor(coordinator, vm, {.mode = mode},
                           std::move(bindings));
    return executor.execute(source);
  }

  CellRef append(const CellRef source, const std::string_view dimension,
                 const CellRef value) {
    const auto dim  = coordinator.resolveDimension(dimension);
    const auto tail = zigzag::rankTail(arena, source, dim);
    zigzag::expectWritten(arena.link(tail, dim, DimVector::POS, value));
    return value;
  }
};

std::size_t countOpcode(const zigzag::vortex::VortexVM &vm,
                        const CompilationResult &compiled,
                        const OpcodeKind kind) {
  std::size_t count = 0;
  for (const CellRef opcode : compiled.generatedOpcodes)
    count += vm.getOpcodeKind(opcode) == kind;
  return count;
}

TEST(VQLReturnValueFramesTest,
     ParserPreservesMacroGroupsEmptyGroupsRepetitionAndLegacyPaths) {
  Parser parser("##/(/+d.next)?");
  const auto query = parser.parseQuery();
  const auto &path = std::get<PathExpression>(query.expr);
  ASSERT_EQ(path.steps.size(), 1u);
  ASSERT_TRUE(std::holds_alternative<std::shared_ptr<MacroDimensionGroup>>(
      path.steps.front().selector));
  const auto group = std::get<std::shared_ptr<MacroDimensionGroup>>(
      path.steps.front().selector);
  ASSERT_NE(group, nullptr);
  ASSERT_EQ(group->steps.size(), 1u);
  EXPECT_EQ(group->repetition, Repetition::ZeroOrOne);

  Parser emptyParser("##/()");
  const auto emptyQuery = emptyParser.parseQuery();
  const auto &emptyPath = std::get<PathExpression>(emptyQuery.expr);
  ASSERT_EQ(emptyPath.steps.size(), 1u);
  const auto emptyGroup = std::get<std::shared_ptr<MacroDimensionGroup>>(
      emptyPath.steps.front().selector);
  ASSERT_NE(emptyGroup, nullptr);
  EXPECT_TRUE(emptyGroup->steps.empty());

  Parser cellIdParser("1");
  EXPECT_EQ(
      std::get<PathExpression>(cellIdParser.parseQuery().expr).anchor.kind,
      AnchorKind::LiteralCellId);
  EXPECT_THROW((void)Parser("return 0").parseQuery(), ParseError);

  Parser returnsParser("return (/+d.next)?, (), 1, identity(##)");
  const auto returnsQuery = returnsParser.parseQuery();
  const auto &returnsBlock =
      std::get<ExecutionBlock>(returnsQuery.body.statements.front());
  const auto &returns = std::get<ReturnClause>(returnsBlock.action.clause);
  ASSERT_EQ(returns.items.size(), 4u);
  for (const auto &item : returns.items)
    EXPECT_TRUE(std::holds_alternative<PathExpression>(item.item));
  EXPECT_EQ(std::get<PathExpression>(returns.items[2].item).anchor.kind,
            AnchorKind::LiteralCellId);
  EXPECT_TRUE(std::holds_alternative<FunctionInvocation>(
      std::get<PathExpression>(returns.items[3].item).steps.front().selector));

  Parser ufcsParser("##/identity()");
  const auto ufcs = std::get<PathExpression>(ufcsParser.parseQuery().expr);
  ASSERT_EQ(ufcs.steps.size(), 1u);
  ASSERT_TRUE(
      std::holds_alternative<FunctionInvocation>(ufcs.steps.front().selector));
  EXPECT_EQ(std::get<FunctionInvocation>(ufcs.steps.front().selector).name,
            "identity");

  Parser fieldWeaveParser("for $item in ## return /+d.result");
  const auto fieldWeaveQuery  = fieldWeaveParser.parseQuery();
  const auto &fieldWeaveBlock = std::get<ExecutionBlock>(fieldWeaveQuery.expr);
  const auto &fieldWeaveReturn =
      std::get<ReturnClause>(fieldWeaveBlock.action.clause);
  ASSERT_EQ(fieldWeaveReturn.items.size(), 1u);
  EXPECT_TRUE(
      std::holds_alternative<FieldWeave>(fieldWeaveReturn.items.front().item));
}

TEST(VQLReturnValueFramesTest,
     ParserMakesOnlyExplicitParenthesesValueItemsAndRejectsPathTails) {
  Parser parser("for $item in ## return (1 + 2)");
  const auto query   = parser.parseQuery();
  const auto &block  = std::get<ExecutionBlock>(query.body.statements.front());
  const auto &clause = std::get<ReturnClause>(block.action.clause);
  ASSERT_EQ(clause.items.size(), 1u);
  ASSERT_TRUE(std::holds_alternative<ReturnValue>(clause.items.front().item));
  const auto &value =
      std::get<ReturnValue>(clause.items.front().item).expression;
  EXPECT_TRUE(
      std::holds_alternative<std::shared_ptr<BinaryValueExpr>>(value.kind));

  Parser legacyParser("for $item in ## return $item");
  const auto legacyQuery   = legacyParser.parseQuery();
  const auto &legacyBlock  = std::get<ExecutionBlock>(legacyQuery.expr);
  const auto &legacyReturn = std::get<ReturnClause>(legacyBlock.action.clause);
  ASSERT_EQ(legacyReturn.items.size(), 1u);
  EXPECT_TRUE(
      std::holds_alternative<PathExpression>(legacyReturn.items.front().item));

  for (const std::string_view source : {
           "for $item in ## return (1)/d.next",
           "for $item in ## return (1)[1]",
           "for $item in ## return (1)@88",
           "for $item in ## return (1)!",
           "for $item in ## return (1)%",
           "for $item in ## return (1)+d.next",
           "for $item in ## return (1)-d.next",
           "for $item in ## return (1){*}",
       }) {
    SCOPED_TRACE(source);
    EXPECT_THROW(
        {
          Parser bad(source);
          (void)bad.parseQuery();
        },
        ParseError);
  }
  EXPECT_THROW(
      {
        Parser bad("for $item in ## return 1 + 2");
        (void)bad.parseQuery();
      },
      ParseError);

  Parser semicolonParser("return(1);return(2)");
  const auto semicolonQuery = semicolonParser.parseQuery();
  ASSERT_EQ(semicolonQuery.body.statements.size(), 2u);
  for (const auto &statement : semicolonQuery.body.statements) {
    const auto &returnBlock = std::get<ExecutionBlock>(statement);
    const auto &returnClause =
        std::get<ReturnClause>(returnBlock.action.clause);
    ASSERT_EQ(returnClause.items.size(), 1u);
    EXPECT_TRUE(
        std::holds_alternative<ReturnValue>(returnClause.items.front().item));
  }

  Parser newlineParser("return(1)\nreturn(2)");
  const auto newlineQuery = newlineParser.parseQuery();
  ASSERT_EQ(newlineQuery.body.statements.size(), 2u);
}

TEST(VQLReturnValueFramesTest, ScalarReturnsKeepKindsAndExactIntegerBits) {
  Harness h;
  const CellRef first =
      h.append(h.core.home(), "d.items", h.arena.makeCell("a"));
  h.append(h.core.home(), "d.items", h.arena.makeCell("b"));

  for (const auto &[source, expectedCount] :
       {std::pair{"for $item in ##/d.items return (0)", 2u},
        std::pair{"for $item in ##/d.items return (false)", 2u},
        std::pair{"for $item in ##/d.items return (1.25)", 2u},
        std::pair{"for $item in ##/d.items return (9007199254740993)", 2u},
        std::pair{"for $item in ##/d.items return (\"two words\")", 2u}}) {
    const auto result = h.run(source);
    ASSERT_TRUE(result) << result.error().message << " source: " << source;
    ASSERT_EQ(result->size(), expectedCount) << source;
    EXPECT_NE(result->front(), first) << source;
  }

  const auto zero = h.run("for $item in ##/d.items return (0)");
  ASSERT_TRUE(zero) << zero.error().message;
  EXPECT_EQ(h.arena.valueKindOf(zero->front()), ValueKind::Int64);
  EXPECT_EQ(h.arena.asInt64(zero->front()), std::optional<std::int64_t>{0});

  const auto boolean = h.run("for $item in ##/d.items return (false)");
  ASSERT_TRUE(boolean) << boolean.error().message;
  EXPECT_EQ(h.arena.valueKindOf(boolean->front()), ValueKind::Bool);
  EXPECT_EQ(h.arena.asBool(boolean->front()), std::optional<bool>{false});

  const auto floating = h.run("for $item in ##/d.items return (1.25)");
  ASSERT_TRUE(floating) << floating.error().message;
  EXPECT_EQ(h.arena.valueKindOf(floating->front()), ValueKind::Double);
  EXPECT_EQ(h.arena.asDouble(floating->front()), std::optional<double>{1.25});

  const auto exact = h.run("for $item in ##/d.items return (9007199254740993)");
  ASSERT_TRUE(exact) << exact.error().message;
  EXPECT_EQ(h.arena.valueKindOf(exact->front()), ValueKind::Int64);
  EXPECT_EQ(h.arena.asInt64(exact->front()),
            std::optional<std::int64_t>{INT64_C(9007199254740993)});

  const auto text = h.run("for $item in ##/d.items return (\"two words\")");
  ASSERT_TRUE(text) << text.error().message;
  EXPECT_EQ(h.arena.valueKindOf(text->front()), ValueKind::None);
  EXPECT_EQ(h.arena.textOf(text->front()), "two words");
}

TEST(VQLReturnValueFramesTest,
     PathAndVariableValueReturnsPreserveIdentityOrderAndDuplicates) {
  Harness h;
  const auto one = h.arena.makeCell("one");
  const auto two = h.arena.makeCell("two");
  h.append(h.core.home(), "d.items", one);
  h.append(h.core.home(), "d.items", two);

  const auto result =
      h.run("for $item in ##/d.items return (##/d.items), $item");
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{one, two, one, one, two, two}));

  const auto alias = h.run("for $item in ##/d.items return ($item)");
  ASSERT_TRUE(alias) << alias.error().message;
  EXPECT_EQ(*alias, (std::vector<CellRef>{one, two}));
}

TEST(VQLReturnValueFramesTest,
     ComputedReturnCellsAreFreshAcrossLoopIterationsAndKeepTextKind) {
  Harness h;
  h.append(h.core.home(), "d.items", h.arena.makeCell("first"));
  h.append(h.core.home(), "d.items", h.arena.makeCell("second"));

  VQLCompiler compiler(h.coordinator, h.vm);
  const auto compiled = compiler.compile(
      "for $item in ##/d.items return (string($item) + \"!\")");
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  EXPECT_GT(countOpcode(h.vm, compiled, OpcodeKind::VqlString), 0u);
  EXPECT_GT(countOpcode(h.vm, compiled, OpcodeKind::VqlAdd), 0u);
  EXPECT_GT(countOpcode(h.vm, compiled, OpcodeKind::VqlBoxScalar), 0u);
  ASSERT_TRUE(compiled.cursor);
  const auto execution = h.vm.run(*compiled.cursor, 10000);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  h.vm.setCursorOpcode(*compiled.cursor, std::nullopt);
  std::vector<CellRef> values;
  for (const CellRef slot : compiled.resultSlots) {
    const auto output = h.vm.resultCells(slot);
    values.insert(values.end(), output.begin(), output.end());
  }
  ASSERT_EQ(values.size(), 2u);
  EXPECT_EQ(h.arena.textOf(values[0]), "first!");
  EXPECT_EQ(h.arena.textOf(values[1]), "second!");
  EXPECT_NE(values[0], values[1]);
  EXPECT_EQ(h.arena.valueKindOf(values[0]), ValueKind::None);
  EXPECT_EQ(h.arena.valueKindOf(values[1]), ValueKind::None);
}

TEST(VQLReturnValueFramesTest,
     RecursiveValueReturnUsesRealFramesAndOmittedFunctorDefaults) {
  Harness h;
  const auto dim    = h.coordinator.resolveDimension("d.next");
  const auto second = h.arena.makeCell("second");
  const auto third  = h.arena.makeCell("third");
  zigzag::expectWritten(
      h.arena.link(h.core.home(), dim, DimVector::POS, second));
  zigzag::expectWritten(h.arena.link(second, dim, DimVector::POS, third));

  constexpr std::string_view source =
      "def pure visit($node: Cell, $next := /+d.next): Cell := { "
      "if $node/$next[1] return (visit($node/$next[1])) "
      "else return ($node); }; visit(##/d.next[1])";
  VQLCompiler compiler(h.coordinator, h.vm);
  const auto compiled = compiler.compile(source);
  ASSERT_TRUE(compiled.success) << compiled.errorMessage;
  EXPECT_GT(countOpcode(h.vm, compiled, OpcodeKind::InvokeFrame), 0u);
  ASSERT_TRUE(compiled.cursor);
  h.vm.setCursorOpcode(*compiled.cursor, std::nullopt);
  const auto result = h.run(source);
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, (std::vector<CellRef>{third}));
}

TEST(VQLReturnValueFramesTest,
     ReturnPromisesAreCheckedAndLegacyDirectRouteRefusesValueItems) {
  Harness h;
  const auto mismatch =
      h.run("def pure broken(): Int := { return (\"wrong\"); }; broken()");
  ASSERT_FALSE(mismatch);
  EXPECT_EQ(mismatch.error().category,
            QueryExecutionError::Category::TypeContract);
  EXPECT_NE(mismatch.error().message.find("broken"), std::string::npos);

  const auto direct =
      h.run("for $item in ## return (7)", {}, QueryExecutionMode::Direct);
  ASSERT_FALSE(direct);
  EXPECT_EQ(direct.error().category, QueryExecutionError::Category::Compile);
}

TEST(VQLReturnValueFramesTest,
     CyclicReturnExpressionRefusesAndCompilerCanRetry) {
  Harness h;
  Parser parser("for $item in ## return (1 + 2)");
  QueryExpression query = parser.parseQuery();
  auto &block       = std::get<ExecutionBlock>(query.body.statements.front());
  auto &clause      = std::get<ReturnClause>(block.action.clause);
  auto &returnValue = std::get<ReturnValue>(clause.items.front().item);
  VQLCompiler compiler(h.coordinator, h.vm);
  const auto before = h.arena.cells().size();

  returnValue.expression.kind = std::make_shared<PathLambda>();
  const auto refusedLambda    = compiler.compile(query);
  EXPECT_FALSE(refusedLambda.success);
  EXPECT_FALSE(refusedLambda.errorMessage.empty());
  EXPECT_FALSE(refusedLambda.cursor);
  EXPECT_EQ(h.arena.cells().size(), before);

  auto functor                = std::make_shared<PathExpression>();
  functor->isFunctor          = true;
  returnValue.expression.kind = std::move(functor);
  const auto refusedFunctor   = compiler.compile(query);
  EXPECT_FALSE(refusedFunctor.success);
  EXPECT_NE(refusedFunctor.errorMessage.find("functor"), std::string::npos);
  EXPECT_FALSE(refusedFunctor.cursor);
  EXPECT_EQ(h.arena.cells().size(), before);

  returnValue.expression.kind = std::make_shared<BinaryValueExpr>();
  auto expression =
      std::get<std::shared_ptr<BinaryValueExpr>>(returnValue.expression.kind);
  expression->left  = std::make_shared<ValueExpr>(returnValue.expression);
  expression->right = std::make_shared<ValueExpr>(
      ValueExpr{.kind = ScalarLiteral{.value = std::int64_t{0}}});

  const auto refused = compiler.compile(query);
  EXPECT_FALSE(refused.success);
  EXPECT_NE(refused.errorMessage.find("cyclic"), std::string::npos);
  EXPECT_FALSE(refused.cursor);
  EXPECT_EQ(h.arena.cells().size(), before);
  expression->left.reset();

  Parser retryParser("for $item in ## return (1 + 2)");
  const auto retry = compiler.compile(retryParser.parseQuery());
  EXPECT_TRUE(retry.success) << retry.errorMessage;
}

TEST(VQLReturnValueFramesTest,
     ExportedValueReturnRunsAfterAstDestructionAndHostMeshUseSource) {
  constexpr std::string_view source =
      "def pure rendered($depth: Int): String := { "
      "if $depth > 0 return (rendered($depth + -1)) "
      "else return (string(40 + 2)); }; return (rendered(2))";
  TemporaryDirectory temporary;
  auto scroll = std::make_shared<UserPermascroll>(
      UserPermascroll::Config{.storageDir = temporary.path / "scroll"});
  {
    Harness h;
    VQLCompiler compiler(h.coordinator, h.vm);
    Parser parser(source);
    QueryExpression ast = parser.parseQuery();
    const auto compiled = compiler.compile(ast);
    ASSERT_TRUE(compiled.success) << compiled.errorMessage;
    EXPECT_GT(countOpcode(h.vm, compiled, OpcodeKind::InvokeFrame), 0u);
    EXPECT_GT(countOpcode(h.vm, compiled, OpcodeKind::VqlAdd), 0u);
    EXPECT_GT(countOpcode(h.vm, compiled, OpcodeKind::VqlString), 0u);
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
      vm.run(vm.spawnCursor(*entry, "return-value-reload"), 10000);
  ASSERT_TRUE(execution.success) << execution.errorMessage;
  auto halt = *entry;
  auto next = arena.linked(halt, core.dims().spin, DimVector::POS);
  while (next && vm.getOpcodeKind(*next) != OpcodeKind::Halt) {
    halt = *next;
    next = arena.linked(halt, core.dims().spin, DimVector::POS);
  }
  ASSERT_TRUE(next);
  const auto outputs = core.inputsOf(*next);
  ASSERT_EQ(outputs.size(), 1u);
  const auto values = vm.resultCells(outputs.front());
  ASSERT_EQ(values.size(), 1u);
  EXPECT_EQ(arena.textOf(values.front()), "42");

  zigzag::vortex::VortexHost host;
  const auto hostResult = host.executeVQLChecked(source);
  ASSERT_TRUE(hostResult) << hostResult.error().message;
  ASSERT_EQ(hostResult->size(), 1u);
  EXPECT_EQ(host.core().arena().textOf(hostResult->front()), "42");

  namespace mesh        = zigzag::vortex::mesh;
  const auto meshResult = mesh::evaluate(mesh::Frontend::Vquery, source,
                                         {.maxCycles      = 10000,
                                          .maxCells       = 10000,
                                          .maxSolutions   = 16,
                                          .maxOutputBytes = 1024 * 1024});
  ASSERT_TRUE(meshResult) << meshResult.error();
  auto &meshArena = meshResult->context->arena;
  std::optional<CellRef> items;
  std::optional<CellRef> value;
  for (const auto &cell : meshArena.cells()) {
    if (meshArena.textOf(cell.birthOp) == "d.mesh.items") items = cell.birthOp;
    if (meshArena.textOf(cell.birthOp) == "d.mesh.value") value = cell.birthOp;
  }
  ASSERT_TRUE(items);
  ASSERT_TRUE(value);
  const auto record =
      meshArena.linked(meshResult->root, *items, DimVector::POS);
  ASSERT_TRUE(record);
  const auto output = meshArena.linked(*record, *value, DimVector::POS);
  ASSERT_TRUE(output);
  EXPECT_EQ(meshArena.textOf(*output), "42");
}

} // namespace
} // namespace xanadu::vql
