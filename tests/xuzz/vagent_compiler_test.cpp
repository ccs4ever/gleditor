#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "common/xanadu/vagent/compiler.hpp"
#include "common/xanadu/vagent/parser.hpp"

namespace {

using namespace xanadu::vagent;
using zigzag::CellRef;
using zigzag::vortex::CellValue;
using zigzag::vortex::ContractViolationKind;
using zigzag::vortex::OpcodeKind;

struct ScalarHarness {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  zigzag::vortex::VortexVM vm{core};
  Compiler compiler{core, vm};

  std::expected<CompiledStatement, Diagnostic>
  compile(std::string_view source, const CompilerBindings &bindings = {},
          ResultUse use = ResultUse::Value) {
    auto expression = Parser(source).parseExpression();
    if (!expression) return std::unexpected(expression.error());
    return compiler.compileStatement(**expression, bindings, use);
  }

  CellRef run(const CompiledStatement &statement,
              std::optional<CellRef> existing = std::nullopt) {
    const auto cursor = existing ? *existing : vm.spawnCursor(statement.entry);
    vm.setCursorOpcode(cursor, statement.entry);
    const auto result = vm.run(cursor);
    EXPECT_TRUE(result.success) << result.errorMessage;
    const auto pc = vm.getCursorOpcode(cursor);
    EXPECT_TRUE(pc);
    if (pc) EXPECT_EQ(vm.getOpcodeKind(*pc), OpcodeKind::Halt);
    return cursor;
  }

  bool produced(const CompiledStatement &statement) {
    const auto value = arena.asBool(statement.produced);
    EXPECT_TRUE(value.has_value());
    return value.value_or(false);
  }
};

TEST(VAgentCompilerTest, ArithmeticExecutesOnTheActualVortexVM) {
  ScalarHarness h;
  auto statement = h.compile("1 + 2 * 3 - 4 / 2");
  ASSERT_TRUE(statement) << (statement ? ""
                                       : formatDiagnostic(statement.error()));
  EXPECT_EQ(statement->resultType, (Type{"Int", {}}));
  EXPECT_EQ(h.vm.getOpcodeKind(statement->entry), OpcodeKind::Value);
  h.run(*statement);
  EXPECT_TRUE(h.produced(*statement));
  EXPECT_EQ(h.core.render(statement->result), CellValue(std::int64_t{5}));
  EXPECT_EQ(h.arena.valueKindOf(statement->result), xanadu::ValueKind::Int64);
  EXPECT_EQ(h.vm.choiceDepth(), 0U);
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);
}

TEST(VAgentCompilerTest, FloatingPointAndStringResultsKeepTheirScalarTypes) {
  ScalarHarness h;
  auto real = h.compile("1.5 * 2.0 + 0.5");
  ASSERT_TRUE(real);
  h.run(*real);
  EXPECT_TRUE(h.produced(*real));
  EXPECT_EQ(h.core.render(real->result), CellValue(3.5));
  EXPECT_EQ(h.arena.valueKindOf(real->result), xanadu::ValueKind::Double);
  auto text = h.compile("\"7\" ++ \" bolts\"");
  ASSERT_TRUE(text);
  h.run(*text);
  EXPECT_EQ(text->resultType, (Type{"Str", {}}));
  EXPECT_EQ(h.core.render(text->result), CellValue(std::string("7 bolts")));
}

TEST(VAgentCompilerTest, FalseAndUnitAreSuccessfulValuesRatherThanFailure) {
  ScalarHarness h;
  for (const auto source : {"false", "false == false", "()", "() == ()"}) {
    SCOPED_TRACE(source);
    auto statement = h.compile(source);
    ASSERT_TRUE(statement) << (statement ? ""
                                         : formatDiagnostic(statement.error()));
    h.run(*statement);
    EXPECT_TRUE(h.produced(*statement));
    if (statement->resultType == Type{"Bool", {}})
      EXPECT_EQ(h.core.render(statement->result), CellValue(false));
    else
      EXPECT_EQ(h.core.render(statement->result), CellValue(std::string{}));
  }
}

TEST(VAgentCompilerTest, ComparisonsProduceTheirRightOperandOrFail) {
  ScalarHarness h;
  auto successful = h.compile("2 < 9");
  ASSERT_TRUE(successful);
  h.run(*successful);
  EXPECT_TRUE(h.produced(*successful));
  EXPECT_EQ(h.core.render(successful->result), CellValue(std::int64_t{9}));
  auto failed = h.compile("9 < 2");
  ASSERT_TRUE(failed);
  h.run(*failed);
  EXPECT_FALSE(h.produced(*failed));
  EXPECT_EQ(h.vm.choiceDepth(), 0U);
}

TEST(VAgentCompilerTest, OrderedIntComparisonsRetainPrecisionBeyondDouble) {
  ScalarHarness h;
  auto statement = h.compile("9007199254740992 < 9007199254740993");
  ASSERT_TRUE(statement);
  h.run(*statement);
  EXPECT_TRUE(h.produced(*statement));
  EXPECT_EQ(h.core.render(statement->result),
            CellValue(std::int64_t{9007199254740993}));
}

TEST(VAgentCompilerTest, AStatementKeepsOnlyItsFirstSuccessfulAlternative) {
  ScalarHarness h;
  auto statement = h.compile("1 | 2 | 3");
  ASSERT_TRUE(statement);
  h.run(*statement);
  EXPECT_TRUE(h.produced(*statement));
  EXPECT_EQ(h.core.render(statement->result), CellValue(std::int64_t{1}));
  EXPECT_EQ(h.vm.choiceDepth(), 0U);
}

TEST(VAgentCompilerTest, AFailingRightOperandRetriesEarlierAlternatives) {
  ScalarHarness h;
  for (const auto source :
       {"(1 | 2) == 2", "2 == (1 | 2)", "((1 | 2) == 2) & 7"}) {
    SCOPED_TRACE(source);
    auto statement = h.compile(source);
    ASSERT_TRUE(statement) << (statement ? ""
                                         : formatDiagnostic(statement.error()));
    h.run(*statement);
    EXPECT_TRUE(h.produced(*statement));
    EXPECT_EQ(h.core.render(statement->result),
              CellValue(std::int64_t{
                  source == std::string_view("((1 | 2) == 2) & 7") ? 7 : 2}));
    EXPECT_EQ(h.vm.choiceDepth(), 0U);
  }
}

TEST(VAgentCompilerTest, ReversibleAssignmentRollsBackTheRejectedArm) {
  ScalarHarness h;
  const auto budget = h.arena.makeScalarCell(std::int64_t{5});
  CompilerBindings bindings{{"budget", {budget, Type{"Int", {}}}}};
  auto statement = h.compile("(budget <- 1 & budget > 2 & \"first\") | "
                             "(budget <- 3 & \"second\")",
                             bindings);
  ASSERT_TRUE(statement) << (statement ? ""
                                       : formatDiagnostic(statement.error()));
  EXPECT_EQ(h.core.render(budget), CellValue(std::int64_t{5}));
  h.run(*statement);
  EXPECT_TRUE(h.produced(*statement));
  EXPECT_EQ(h.core.render(statement->result), CellValue(std::string("second")));
  EXPECT_EQ(h.core.render(budget), CellValue(std::int64_t{3}));
  EXPECT_EQ(h.vm.choiceDepth(), 0U);
}

TEST(VAgentCompilerTest, FailedStatementsUndoWritesAndReturnNoValue) {
  ScalarHarness h;
  const auto value = h.arena.makeScalarCell(std::int64_t{5});
  auto statement =
      h.compile("value <- 1 & fail", {{"value", {value, Type{"Int", {}}}}});
  ASSERT_TRUE(statement);
  EXPECT_FALSE(statement->resultType.has_value());
  h.run(*statement);
  EXPECT_FALSE(h.produced(*statement));
  EXPECT_EQ(h.core.render(value), CellValue(std::int64_t{5}));
  EXPECT_EQ(h.vm.choiceDepth(), 0U);
}

TEST(VAgentCompilerTest, RootAssignmentProducesUnitAndKeepsItsFirstRhsResult) {
  ScalarHarness h;
  const auto value = h.arena.makeScalarCell(std::int64_t{0});
  auto statement =
      h.compile("value := (2 | 3)", {{"value", {value, Type{"Int", {}}}}});
  ASSERT_TRUE(statement);
  h.run(*statement);
  EXPECT_TRUE(h.produced(*statement));
  EXPECT_EQ(statement->resultType, (Type{"()", {}}));
  EXPECT_EQ(h.core.render(value), CellValue(std::int64_t{2}));
  EXPECT_EQ(h.core.render(statement->result), CellValue(std::string{}));
}

TEST(VAgentCompilerTest,
     NegationUsesSuccessAndFailureRatherThanValueTruthiness) {
  ScalarHarness h;
  for (const auto source : {"not fail", "not (1 > 2)"}) {
    auto statement = h.compile(source);
    ASSERT_TRUE(statement);
    h.run(*statement);
    EXPECT_TRUE(h.produced(*statement));
    EXPECT_EQ(statement->resultType, (Type{"()", {}}));
  }
  for (const auto source : {"not ()", "not 0", "not (1 < 2)"}) {
    auto statement = h.compile(source);
    ASSERT_TRUE(statement);
    h.run(*statement);
    EXPECT_FALSE(h.produced(*statement));
  }
}

TEST(VAgentCompilerTest, NegationRollsBackSpeculativeWritesOnEitherPath) {
  ScalarHarness h;
  const auto value = h.arena.makeScalarCell(std::int64_t{5});
  CompilerBindings bindings{{"value", {value, Type{"Int", {}}}}};
  for (const auto source : {"not (value <- 1 & fail)", "not (value <- 1)"}) {
    auto statement = h.compile(source, bindings);
    ASSERT_TRUE(statement);
    h.run(*statement);
    EXPECT_EQ(h.core.render(value), CellValue(std::int64_t{5}));
    EXPECT_EQ(h.vm.choiceDepth(), 0U);
  }
}

TEST(VAgentCompilerTest, SuccessAndFailurePreserveTheCallersChoiceBarrier) {
  for (const auto source : {"value <- 3", "value <- 3 & fail"}) {
    ScalarHarness h;
    const auto value = h.arena.makeScalarCell(std::int64_t{5});
    auto statement   = h.compile(source, {{"value", {value, Type{"Int", {}}}}});
    ASSERT_TRUE(statement);
    const auto outerAlternative = h.vm.mintOpcode(OpcodeKind::Halt);
    const auto cursor           = h.vm.spawnCursor(statement->entry);
    h.vm.pushChoicePoint(cursor, outerAlternative);
    h.run(*statement, cursor);
    EXPECT_EQ(h.vm.choiceDepth(), 1U);
    EXPECT_EQ(h.arena.outstandingMarks(), 1U);
    EXPECT_TRUE(h.vm.backtrack(cursor));
    EXPECT_EQ(h.vm.getCursorOpcode(cursor), outerAlternative);
    EXPECT_EQ(h.core.render(value), CellValue(std::int64_t{5}));
    EXPECT_EQ(h.vm.choiceDepth(), 0U);
  }
}

TEST(VAgentCompilerTest, ExistingGraphsReadFreshBindingValuesOnEachRun) {
  ScalarHarness h;
  const auto value = h.arena.makeScalarCell(std::int64_t{1});
  auto statement =
      h.compile("value + 1", {{"value", {value, Type{"Int", {}}}}});
  ASSERT_TRUE(statement);
  const auto cursor = h.run(*statement);
  EXPECT_EQ(h.core.render(statement->result), CellValue(std::int64_t{2}));
  ASSERT_TRUE(
      h.core.writeValue(value, 0, std::nullopt, CellValue(std::int64_t{9})));
  h.run(*statement, cursor);
  EXPECT_EQ(h.core.render(statement->result), CellValue(std::int64_t{10}));
}

TEST(VAgentCompilerTest, RepeatedFailureDoesNotLeaveAStaleProducedFlag) {
  ScalarHarness h;
  const auto value = h.arena.makeScalarCell(std::int64_t{1});
  auto statement =
      h.compile("value > 0", {{"value", {value, Type{"Int", {}}}}});
  ASSERT_TRUE(statement);
  const auto cursor = h.run(*statement);
  EXPECT_TRUE(h.produced(*statement));
  ASSERT_TRUE(
      h.core.writeValue(value, 0, std::nullopt, CellValue(std::int64_t{-1})));
  h.run(*statement, cursor);
  EXPECT_FALSE(h.produced(*statement));
}

TEST(VAgentCompilerTest, CanonicalLiteralsIncludeSignedMinimumAndNegativeZero) {
  ScalarHarness h;
  auto minimum = h.compile("-0x08000000000000000");
  ASSERT_TRUE(minimum);
  h.run(*minimum);
  EXPECT_EQ(h.core.render(minimum->result),
            CellValue(std::numeric_limits<std::int64_t>::min()));
  auto zero = h.compile("-0.0");
  ASSERT_TRUE(zero);
  h.run(*zero);
  ASSERT_TRUE(h.arena.asDouble(zero->result));
  EXPECT_FALSE(std::signbit(*h.arena.asDouble(zero->result)));
}

TEST(VAgentCompilerTest, DivisionDomainGuardsBecomeOrdinaryFailure) {
  ScalarHarness h;
  for (const auto source :
       {"1 / 0", "1 % 0", "1.0 / 0.0", "-9223372036854775808 / -1",
        "-9223372036854775808 % -1"}) {
    SCOPED_TRACE(source);
    auto statement = h.compile(source);
    ASSERT_TRUE(statement) << (statement ? ""
                                         : formatDiagnostic(statement.error()));
    h.run(*statement);
    EXPECT_FALSE(h.produced(*statement));
    EXPECT_EQ(h.vm.choiceDepth(), 0U);
  }
}

TEST(VAgentCompilerTest,
     NumericContractFaultsFollowTheAdapterBacktrackContract) {
  ScalarHarness h;
  const auto value = h.arena.makeScalarCell(std::int64_t{5});
  auto statement   = h.compile("value <- 2 & (9223372036854775807 + 1)",
                               {{"value", {value, Type{"Int", {}}}}});
  ASSERT_TRUE(statement);
  const auto cursor = h.vm.spawnCursor(statement->entry);
  const auto result = h.vm.run(cursor);
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.contractViolation, ContractViolationKind::Precondition);
  EXPECT_EQ(h.vm.choiceDepth(), 1U);
  ASSERT_TRUE(h.vm.backtrack(cursor));
  const auto recovered = h.vm.run(cursor);
  EXPECT_TRUE(recovered.success) << recovered.errorMessage;
  EXPECT_FALSE(h.produced(*statement));
  EXPECT_EQ(h.core.render(value), CellValue(std::int64_t{5}));
}

TEST(VAgentCompilerTest, UnsupportedSyntaxAndNamesRefuseBeforeMinting) {
  ScalarHarness h;
  for (const auto source :
       {"unknown", "method()", "[1, 2]", "1 to 3", "while 1 < 2 do ()",
        "for x in [1] do ()", "none", "1s"}) {
    SCOPED_TRACE(source);
    const auto before = h.arena.cellCount();
    auto statement    = h.compile(source);
    ASSERT_FALSE(statement);
    EXPECT_EQ(h.arena.cellCount(), before);
    EXPECT_EQ(h.vm.choiceDepth(), 0U);
  }
}

TEST(VAgentCompilerTest,
     PreflightRejectsTypeCoercionsAndSpeculativeIrreversibility) {
  ScalarHarness h;
  const auto value = h.arena.makeScalarCell(std::int64_t{5});
  CompilerBindings bindings{{"value", {value, Type{"Int", {}}}}};
  for (const auto source :
       {"1 + 2.0", "\"1\" + \"2\"", "true & 1", "not false",
        "(value := 1) | ()", "value := \"bad\"", "true < false"}) {
    SCOPED_TRACE(source);
    const auto before = h.arena.cellCount();
    EXPECT_FALSE(h.compile(source, bindings));
    EXPECT_EQ(h.arena.cellCount(), before);
  }
}

TEST(VAgentCompilerTest, BindingDeclarationsAndActualCellKindsMustAgree) {
  ScalarHarness h;
  const auto text   = h.arena.makeCell("42");
  const auto before = h.arena.cellCount();
  EXPECT_FALSE(h.compile("value", {{"value", {text, Type{"Int", {}}}}}));
  EXPECT_FALSE(h.compile("value", {{"value", {text, Type{"Record", {}}}}}));
  EXPECT_FALSE(h.compile(
      "value",
      {{"value", {zigzag::ArenaManifold::refOf(100000), Type{"Int", {}}}}}));
  EXPECT_EQ(h.arena.cellCount(), before);
}

TEST(VAgentCompilerTest,
     CompilationRefusesLiveMarksAndForeignVMsWithoutMinting) {
  ScalarHarness h;
  auto expression = Parser("1").parseExpression();
  ASSERT_TRUE(expression);
  const auto before = h.arena.cellCount();
  const auto mark   = h.arena.mark();
  EXPECT_FALSE(h.compiler.compileStatement(**expression));
  EXPECT_EQ(h.arena.cellCount(), before);
  h.arena.release(mark);
  zigzag::ArenaManifold otherArena;
  zigzag::vortex::VortexCore otherCore(otherArena);
  zigzag::vortex::VortexVM otherVM(otherCore);
  EXPECT_FALSE(Compiler(h.core, otherVM).compileStatement(**expression));
  EXPECT_EQ(h.arena.cellCount(), before);
}

TEST(VAgentCompilerTest, MalformedAstAndCompilerBudgetsRefuseBeforeMinting) {
  ScalarHarness h;
  Expr invalid;
  invalid.kind = ExprKind::Unary;
  invalid.text = "-";
  invalid.children.push_back(nullptr);
  const auto before = h.arena.cellCount();
  EXPECT_FALSE(h.compiler.compileStatement(invalid));
  auto expression = Parser("1 + 2").parseExpression();
  ASSERT_TRUE(expression);
  EXPECT_FALSE(Compiler(h.core, h.vm, ScalarCompilerLimits{2, 256})
                   .compileStatement(**expression));
  EXPECT_FALSE(Compiler(h.core, h.vm, ScalarCompilerLimits{100, 1})
                   .compileStatement(**expression));
  EXPECT_EQ(h.arena.cellCount(), before);
}

TEST(VAgentCompilerTest, BlocksTakeEachStatementsFirstResult) {
  ScalarHarness h;
  auto block = h.compile("{ 1 | 2; 3 | 4 }");
  ASSERT_TRUE(block);
  h.run(*block);
  EXPECT_TRUE(h.produced(*block));
  EXPECT_EQ(h.core.render(block->result), CellValue(std::int64_t{3}));
  EXPECT_EQ(h.vm.choiceDepth(), 0U);
  auto noRetry = h.compile("{ 1 | 2 } == 2");
  ASSERT_TRUE(noRetry);
  h.run(*noRetry);
  EXPECT_FALSE(h.produced(*noRetry));
  auto empty = h.compile("{}");
  ASSERT_TRUE(empty);
  h.run(*empty);
  EXPECT_TRUE(h.produced(*empty));
  EXPECT_EQ(empty->resultType, (Type{"()", {}}));
}

TEST(VAgentCompilerTest, LexicalShadowInitializerReadsOuterBinding) {
  ScalarHarness h;
  const auto outer = h.arena.makeScalarCell(std::int64_t{10});
  const CompilerBindings bindings{{"value", {outer, Type{"Int", {}}}}};
  auto block =
      h.compile("{ let value := value + 1; "
                "{ let value: Int := value + 2; value := value + 3 }; value }",
                bindings);
  ASSERT_TRUE(block) << (block ? "" : formatDiagnostic(block.error()));
  h.run(*block);
  EXPECT_TRUE(h.produced(*block));
  EXPECT_EQ(h.core.render(block->result), CellValue(std::int64_t{11}));
  EXPECT_EQ(h.core.render(outer), CellValue(std::int64_t{10}));
}

TEST(VAgentCompilerTest, LocalInitializersAreBoundedAndReinitializedOnRerun) {
  ScalarHarness h;
  const auto outer = h.arena.makeScalarCell(std::int64_t{1});
  const CompilerBindings bindings{{"value", {outer, Type{"Int", {}}}}};
  auto block = h.compile("{ let local := value | 20; local + 1 }", bindings);
  ASSERT_TRUE(block);
  const auto cursor = h.run(*block);
  EXPECT_EQ(h.core.render(block->result), CellValue(std::int64_t{2}));
  ASSERT_TRUE(
      h.core.writeValue(outer, 0, std::nullopt, CellValue(std::int64_t{8})));
  h.run(*block, cursor);
  EXPECT_TRUE(h.produced(*block));
  EXPECT_EQ(h.core.render(block->result), CellValue(std::int64_t{9}));
}

TEST(VAgentCompilerTest, FailedNestedBlockRollsBackItsWholeEnclosingStatement) {
  ScalarHarness h;
  const auto value = h.arena.makeScalarCell(std::int64_t{9});
  const CompilerBindings bindings{{"value", {value, Type{"Int", {}}}}};
  auto block = h.compile("{ value := 1; { value := 2; fail } }", bindings);
  ASSERT_TRUE(block);
  h.run(*block);
  EXPECT_FALSE(h.produced(*block));
  EXPECT_EQ(h.core.render(value), CellValue(std::int64_t{9}));
  EXPECT_EQ(h.vm.choiceDepth(), 0U);
  EXPECT_EQ(h.arena.outstandingMarks(), 0U);
}

TEST(VAgentCompilerTest, BlockFailureAllowsSurroundingAlternative) {
  ScalarHarness h;
  const auto value = h.arena.makeScalarCell(std::int64_t{9});
  const CompilerBindings bindings{{"value", {value, Type{"Int", {}}}}};
  auto block = h.compile("{ value <- 1; fail } | { value <- 2; 3 }", bindings);
  ASSERT_TRUE(block);
  h.run(*block);
  EXPECT_TRUE(h.produced(*block));
  EXPECT_EQ(h.core.render(value), CellValue(std::int64_t{2}));
  EXPECT_EQ(h.core.render(block->result), CellValue(std::int64_t{3}));
}

TEST(VAgentCompilerTest, IfTestsSuccessIncludingSuccessfulFalseComparison) {
  ScalarHarness h;
  for (const auto source :
       {"if 0 then 1 else 2", "if false == false then 1 else 2",
        "if (1 | 2) == 2 then 1 else 2"}) {
    SCOPED_TRACE(source);
    auto branch = h.compile(source);
    ASSERT_TRUE(branch);
    h.run(*branch);
    EXPECT_TRUE(h.produced(*branch));
    EXPECT_EQ(h.core.render(branch->result), CellValue(std::int64_t{1}));
  }
  auto failed = h.compile("if fail then 1 else 2");
  ASSERT_TRUE(failed);
  h.run(*failed);
  EXPECT_EQ(h.core.render(failed->result), CellValue(std::int64_t{2}));
}

TEST(VAgentCompilerTest, SelectedThenFailureNeverRunsElse) {
  ScalarHarness h;
  const auto value = h.arena.makeScalarCell(std::int64_t{9});
  const CompilerBindings bindings{{"value", {value, Type{"Int", {}}}}};
  auto branch =
      h.compile("if 1 then { value := 1; fail } else value := 2", bindings);
  ASSERT_TRUE(branch);
  h.run(*branch);
  EXPECT_FALSE(h.produced(*branch));
  EXPECT_EQ(h.core.render(value), CellValue(std::int64_t{9}));
}

TEST(VAgentCompilerTest, ConditionalCutsConditionAlternativesBeforeItsArm) {
  ScalarHarness h;
  auto branch = h.compile("if 1 | 2 then (3 | 4) else 5");
  ASSERT_TRUE(branch);
  h.run(*branch);
  EXPECT_TRUE(h.produced(*branch));
  EXPECT_EQ(h.core.render(branch->result), CellValue(std::int64_t{3}));
  auto retryArm = h.compile("(if 1 then (3 | 4) else 5) == 4");
  ASSERT_TRUE(retryArm);
  h.run(*retryArm);
  EXPECT_TRUE(h.produced(*retryArm));
  EXPECT_EQ(h.core.render(retryArm->result), CellValue(std::int64_t{4}));
}

TEST(VAgentCompilerTest, ConditionalWithoutElseIsTotalAndUnitTyped) {
  ScalarHarness h;
  auto branch = h.compile("if fail then ()");
  ASSERT_TRUE(branch);
  h.run(*branch);
  EXPECT_TRUE(h.produced(*branch));
  EXPECT_EQ(branch->resultType, (Type{"()", {}}));
}

TEST(VAgentCompilerTest, ScopeAndConditionErrorsMintNoGraph) {
  ScalarHarness h;
  const auto value = h.arena.makeScalarCell(std::int64_t{9});
  const CompilerBindings bindings{{"value", {value, Type{"Int", {}}}}};
  for (const auto source :
       {"{ let local := 1; { let hidden := 2 }; hidden }",
        "{ let local := 1; let local := 2 }", "{ let local: Str := 1; local }",
        "{ let _: Int := 1 }", "if false then 1 else 2",
        "if value <- 1 then () else ()",
        "if { value := 1; () } then () else ()", "if 1 then 2",
        "if 1 then 2 else ()", "(if 1 then value := 1 else value := 2) | ()",
        "{ value := 1 } | ()"}) {
    SCOPED_TRACE(source);
    const auto before = h.arena.cellCount();
    EXPECT_FALSE(h.compile(source, bindings));
    EXPECT_EQ(h.arena.cellCount(), before);
    EXPECT_EQ(h.core.render(value), CellValue(std::int64_t{9}));
  }
}

TEST(VAgentCompilerTest, BoundedAssignmentStillParticipatesInOuterRollback) {
  ScalarHarness h;
  const auto value = h.arena.makeScalarCell(std::int64_t{9});
  const CompilerBindings bindings{{"value", {value, Type{"Int", {}}}}};
  auto statement = h.compile("{ value := 1 | 2; value }", bindings);
  ASSERT_TRUE(statement);
  const auto outerHalt = h.vm.mintOpcode(OpcodeKind::Halt);
  const auto cursor    = h.vm.spawnCursor(statement->entry);
  h.vm.pushChoicePoint(cursor, outerHalt);
  h.run(*statement, cursor);
  EXPECT_TRUE(h.produced(*statement));
  EXPECT_EQ(h.core.render(value), CellValue(std::int64_t{1}));
  EXPECT_EQ(h.vm.choiceDepth(), 1U);
  ASSERT_TRUE(h.vm.backtrack(cursor));
  EXPECT_EQ(h.core.render(value), CellValue(std::int64_t{9}));
  EXPECT_EQ(h.vm.choiceDepth(), 0U);
}

TEST(VAgentCompilerTest, DiscardedRootProducesUnitAcrossUnlikeConditionalArms) {
  ScalarHarness h;
  for (const auto source :
       {"if 1 then 2 else ()", "if fail then 2 else false", "false", "3 + 4"}) {
    SCOPED_TRACE(source);
    auto statement = h.compile(source, {}, ResultUse::Discard);
    ASSERT_TRUE(statement);
    h.run(*statement);
    EXPECT_TRUE(h.produced(*statement));
    EXPECT_EQ(statement->resultType, (Type{"()", {}}));
    EXPECT_EQ(h.core.render(statement->result), CellValue(std::string{}));
  }
}

TEST(VAgentCompilerTest,
     NonfinalBlockStatementsDiscardButFinalValuesRemainTyped) {
  ScalarHarness h;
  auto block = h.compile("{ if 1 then 2 else (); 9 }");
  ASSERT_TRUE(block);
  h.run(*block);
  EXPECT_TRUE(h.produced(*block));
  EXPECT_EQ(block->resultType, (Type{"Int", {}}));
  EXPECT_EQ(h.core.render(block->result), CellValue(std::int64_t{9}));
  const auto before = h.arena.cellCount();
  EXPECT_FALSE(h.compile("{ 9; if 1 then 2 else () }"));
  EXPECT_EQ(h.arena.cellCount(), before);
  auto discarded =
      h.compile("{ 9; if 1 then 2 else () }", {}, ResultUse::Discard);
  ASSERT_TRUE(discarded);
  h.run(*discarded);
  EXPECT_TRUE(h.produced(*discarded));
  EXPECT_EQ(discarded->resultType, (Type{"()", {}}));
  EXPECT_EQ(h.core.render(discarded->result), CellValue(std::string{}));
}

TEST(VAgentCompilerTest, DiscardDoesNotChangeFailureOrSelectedArmBehavior) {
  ScalarHarness h;
  for (const auto source :
       {"fail", "if 1 then fail else 2", "{ if 1 then fail else (); 9 }"}) {
    SCOPED_TRACE(source);
    auto statement = h.compile(source, {}, ResultUse::Discard);
    ASSERT_TRUE(statement);
    h.run(*statement);
    EXPECT_FALSE(h.produced(*statement));
    if (source == std::string_view("fail")) EXPECT_FALSE(statement->resultType);
  }
}

TEST(VAgentCompilerTest, DiscardDoesNotPermitAmbiguousOrImpureConditions) {
  ScalarHarness h;
  const auto value = h.arena.makeScalarCell(std::int64_t{1});
  const CompilerBindings bindings{{"value", {value, Type{"Int", {}}}}};
  for (const auto source : {"if false then 1 else ()",
                            "if value <- 2 then 1 else ()", "if 1 then 2"}) {
    SCOPED_TRACE(source);
    const auto before = h.arena.cellCount();
    EXPECT_FALSE(h.compile(source, bindings, ResultUse::Discard));
    EXPECT_EQ(h.arena.cellCount(), before);
  }
}

TEST(VAgentCompilerTest,
     DiscardingAnOuterBlockDoesNotDiscardItsLocalInitializer) {
  ScalarHarness h;
  const auto value = h.arena.makeScalarCell(std::int64_t{1});
  const CompilerBindings bindings{{"value", {value, Type{"Int", {}}}}};
  auto block = h.compile("{ let local := 8; value := local }", bindings,
                         ResultUse::Discard);
  ASSERT_TRUE(block);
  h.run(*block);
  EXPECT_TRUE(h.produced(*block));
  EXPECT_EQ(h.core.render(value), CellValue(std::int64_t{8}));
}

} // namespace
