#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "common/xanadu/vagent/ast_clone.hpp"
#include "common/xanadu/vagent/inlang_parser.hpp"
#include "common/xanadu/vagent/parser.hpp"
#include "common/xanadu/vagent/r1_ast.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {

using namespace xanadu::vagent;

TEST(VAgentParserTest, ParsesEveryIsolatedSpecificationExample) {
  const std::filesystem::path directory("tests/samples/vagent");
  ASSERT_TRUE(std::filesystem::is_directory(directory));
  std::size_t parsedCount = 0;
  for (const auto &entry : std::filesystem::directory_iterator(directory)) {
    if (entry.path().extension() != ".vagent" ||
        !entry.path().filename().string().starts_with("mini_"))
      continue;
    SCOPED_TRACE(entry.path().string());
    std::ifstream stream(entry.path());
    const std::string source(std::istreambuf_iterator<char>(stream), {});
    auto parsed = Parser(source).parseProgram();
    ASSERT_TRUE(parsed.has_value())
        << (parsed ? "" : formatDiagnostic(parsed.error()));
    EXPECT_FALSE(parsed->declarations.empty());
    ++parsedCount;
  }
  EXPECT_EQ(parsedCount, 23U);
}

TEST(VAgentParserTest, ParsesModuleImportsTypesAndKeywordFieldNames) {
  auto parsed = Parser(R"(
    module demo:inventory
    import std:agent
    import demo:types as types (Item, Report)
    event Notice extends Report { goal: Str, to: Opt<Agent> }
    record State { values: Map<Str, [Opt<Int>]> }
    predicate reading(key sensor: Str, value: Float)
    question stock(item: Str): Int
  )")
                    .parseProgram();
  ASSERT_TRUE(parsed) << (parsed ? "" : formatDiagnostic(parsed.error()));
  EXPECT_EQ(parsed->module, "demo:inventory");
  ASSERT_EQ(parsed->imports.size(), 2U);
  EXPECT_EQ(parsed->imports[0].path, "std:agent");
  EXPECT_EQ(parsed->imports[1].alias, "types");
  EXPECT_EQ(parsed->imports[1].symbols,
            (std::vector<std::string>{"Item", "Report"}));
  ASSERT_EQ(parsed->declarations.size(), 4U);
  const auto &notice = parsed->declarations[0];
  EXPECT_EQ(notice.kind, DeclKind::Event);
  EXPECT_EQ(notice.names, (std::vector<std::string>{"Report"}));
  ASSERT_EQ(notice.parameters.size(), 2U);
  EXPECT_EQ(notice.parameters[0].name, "goal");
  EXPECT_EQ(notice.parameters[1].type, (Type{"Opt", {Type{"Agent", {}}}}));
  EXPECT_EQ(typeName(parsed->declarations[1].parameters[0].type),
            "Map<Str, [Opt<Int>]>");
  EXPECT_TRUE(parsed->declarations[2].parameters[0].key);
  EXPECT_EQ(parsed->declarations[3].result, (Type{"Int", {}}));
}

TEST(VAgentParserTest, ParsesCapacitiesSkillsAndLifecycleBodies) {
  auto parsed = Parser(R"(
    capacity Motion extends Moving, Stopping {
      effect preempts def halt() within 1s
      def speed(): Float
    }
    native skill HostMotion(maximum: Float) implements Motion
    skill Slow implements Motion {
      uses Limits
      var value: Float := 0.0
      on install { value := 1.0 }
      on uninstall { value := 0.0 }
      def speed(): Float requires value >= 0.0 ensures value <= 2.0 { value }
    }
  )")
                    .parseProgram();
  ASSERT_TRUE(parsed) << (parsed ? "" : formatDiagnostic(parsed.error()));
  ASSERT_EQ(parsed->declarations.size(), 3U);
  const auto &capacity = parsed->declarations[0];
  ASSERT_EQ(capacity.members.size(), 2U);
  EXPECT_TRUE(capacity.members[0].effect);
  EXPECT_TRUE(capacity.members[0].preempts);
  ASSERT_TRUE(capacity.members[0].deadline);
  EXPECT_EQ(capacity.members[0].deadline->text, "1s");
  EXPECT_FALSE(capacity.members[0].body);
  EXPECT_TRUE(parsed->declarations[1].native);
  const auto &skill = parsed->declarations[2];
  ASSERT_EQ(skill.members.size(), 5U);
  EXPECT_EQ(skill.members[2].kind, DeclKind::Lifecycle);
  EXPECT_EQ(skill.members[2].name, "install");
  EXPECT_EQ(skill.members[3].name, "uninstall");
  ASSERT_EQ(skill.members[4].contracts.size(), 2U);
  EXPECT_FALSE(skill.members[4].contracts[0].postcondition);
  EXPECT_TRUE(skill.members[4].contracts[1].postcondition);
}

TEST(VAgentParserTest, HandlerPriorityAcceptsEveryIntegerLiteralRadix) {
  auto parsed = Parser("agent A { on Tick priority 0x2_A {} }").parseProgram();
  ASSERT_TRUE(parsed) << (parsed ? "" : formatDiagnostic(parsed.error()));
  EXPECT_EQ(parsed->declarations[0].members[0].priority, 42);
  auto overflow = Parser("agent A { on Tick priority 9223372036854775808 {} }")
                      .parseProgram();
  ASSERT_FALSE(overflow);
  EXPECT_NE(overflow.error().detail.find("outside the Int range"),
            std::string::npos);
  EXPECT_EQ(overflow.error().loc.column, 28U);
}

TEST(VAgentParserTest, RecordLiteralNewlinesAreWhitespaceInsideGrouping) {
  auto parsed = Parser(R"(agent A {
    on Tick { let item := consume_value(Item
      {value: 2}) }
  })")
                    .parseProgram();
  ASSERT_TRUE(parsed) << (parsed ? "" : formatDiagnostic(parsed.error()));
  const auto &call =
      parsed->declarations[0].members[0].body->children[0]->children[0];
  EXPECT_EQ(call->children[1]->kind, ExprKind::Record);
  EXPECT_EQ(call->children[1]->names, (std::vector<std::string>{"value"}));
}

TEST(VAgentParserTest, ParsesGoalClausesPlanBindingsAndBeliefHandlers) {
  auto parsed = Parser(R"(
    goal Reach(target: Int) achieve {
      succeed when believes position(target)
      drop when target < 0
    }
    agent Navigator {
      on +position(?value) [value > 0] priority 2 { emit Seen{value: value} }
      plan direct for Reach(target) where believes position(?from) {
        await position(target) within 2s else fail
      }
    }
  )")
                    .parseProgram();
  ASSERT_TRUE(parsed) << (parsed ? "" : formatDiagnostic(parsed.error()));
  const auto &goal = parsed->declarations[0];
  EXPECT_EQ(goal.goalKind, GoalKind::Achieve);
  ASSERT_EQ(goal.goalClauses.size(), 2U);
  EXPECT_EQ(goal.goalClauses[0].kind, GoalClause::Kind::Succeed);
  EXPECT_EQ(goal.goalClauses[1].kind, GoalClause::Kind::Drop);
  const auto &members = parsed->declarations[1].members;
  ASSERT_EQ(members.size(), 2U);
  EXPECT_EQ(members[0].name, "+");
  EXPECT_EQ(members[0].trigger->kind, ExprKind::Term);
  EXPECT_EQ(members[0].trigger->children[0]->kind, ExprKind::LogicVariable);
  EXPECT_EQ(members[0].priority, 2);
  EXPECT_EQ(members[1].target, "Reach");
  ASSERT_TRUE(members[1].planBindings);
  EXPECT_EQ(*members[1].planBindings, (std::vector<std::string>{"target"}));
  const auto &wait = members[1].body->children[0];
  EXPECT_EQ(wait->kind, ExprKind::AwaitTerm);
  EXPECT_EQ(wait->recovery->kind, ExprKind::Fail);
}

TEST(VAgentParserTest, AssignmentIsRightAssociativeAndConjunctionIsLoosest) {
  auto parsed = Parser("a <- b := 1 | 2 & c").parseExpression();
  ASSERT_TRUE(parsed) << (parsed ? "" : formatDiagnostic(parsed.error()));
  const auto &root = **parsed;
  EXPECT_EQ(root.text, "&");
  ASSERT_EQ(root.children.size(), 2U);
  const auto &assignment = root.children[0];
  EXPECT_EQ(assignment->text, "<-");
  EXPECT_EQ(assignment->children[1]->text, ":=");
  EXPECT_EQ(assignment->children[1]->children[1]->text, "|");
}

TEST(VAgentParserTest, NotIsLooserThanComparisonAndTighterThanAlternation) {
  auto parsed = Parser("not x == 2 | 3").parseExpression();
  ASSERT_TRUE(parsed) << (parsed ? "" : formatDiagnostic(parsed.error()));
  const auto &root = **parsed;
  EXPECT_EQ(root.text, "|");
  EXPECT_EQ(root.children[0]->kind, ExprKind::Unary);
  EXPECT_EQ(root.children[0]->text, "not");
  EXPECT_EQ(root.children[0]->children[0]->text, "==");
}

TEST(VAgentParserTest, RangeScanAndArithmeticKeepTheirSpecifiedPrecedence) {
  auto parsed = Parser("subject ? 1 | 2 to 6 by 2 + 1").parseExpression();
  ASSERT_TRUE(parsed) << (parsed ? "" : formatDiagnostic(parsed.error()));
  const auto &scan = **parsed;
  EXPECT_EQ(scan.text, "?");
  const auto &range = scan.children[1];
  EXPECT_EQ(range->kind, ExprKind::Range);
  ASSERT_EQ(range->children.size(), 3U);
  EXPECT_EQ(range->children[0]->text, "|");
  EXPECT_EQ(range->children[2]->text, "+");
}

TEST(VAgentParserTest, ComparisonAndScanDoNotChain) {
  for (const auto source :
       {"a < b < c", "a == b != c", "a ? b ? c", "1 to 3 to 4"}) {
    SCOPED_TRACE(source);
    auto parsed = Parser(source).parseExpression();
    ASSERT_FALSE(parsed);
    EXPECT_EQ(parsed.error().code, DiagnosticCode::Syntax);
    EXPECT_NE(parsed.error().detail.find("does not chain"), std::string::npos);
  }
}

TEST(VAgentParserTest, PostfixCallsIndexesAndKeywordFieldsPreserveCallee) {
  auto parsed = Parser("items[2].goal(3).to").parseExpression();
  ASSERT_TRUE(parsed) << (parsed ? "" : formatDiagnostic(parsed.error()));
  const auto &field = **parsed;
  EXPECT_EQ(field.kind, ExprKind::Field);
  EXPECT_EQ(field.text, "to");
  const auto &call = field.children[0];
  EXPECT_EQ(call->kind, ExprKind::Call);
  ASSERT_EQ(call->children.size(), 2U);
  EXPECT_EQ(call->children[0]->kind, ExprKind::Field);
  EXPECT_EQ(call->children[0]->text, "goal");
  EXPECT_EQ(call->children[0]->children[0]->kind, ExprKind::Index);
}

TEST(VAgentParserTest, ActionsOwnPostfixOperandsAndFixedRoleMetadata) {
  auto parsed = Parser("emit Notice{value: 1} in inner for parent & next()")
                    .parseExpression();
  ASSERT_TRUE(parsed) << (parsed ? "" : formatDiagnostic(parsed.error()));
  const auto &conjunction = **parsed;
  EXPECT_EQ(conjunction.text, "&");
  const auto &emission = conjunction.children[0];
  EXPECT_EQ(emission->kind, ExprKind::Emit);
  ASSERT_EQ(emission->children.size(), 1U);
  EXPECT_EQ(emission->children[0]->kind, ExprKind::Record);
  EXPECT_EQ(emission->scope->text, "inner");
  EXPECT_EQ(emission->recipients->text, "parent");
  EXPECT_EQ(conjunction.children[1]->kind, ExprKind::Call);
}

TEST(VAgentParserTest, DelegationAndQuestionsHaveExplicitDeadlineAndRecovery) {
  auto delegated = Parser("achieve Fetch(7) by courier within 5s else fail")
                       .parseExpression();
  ASSERT_TRUE(delegated) << (delegated ? ""
                                       : formatDiagnostic(delegated.error()));
  EXPECT_EQ((*delegated)->kind, ExprKind::Achieve);
  EXPECT_EQ((*delegated)->recipients->text, "courier");
  EXPECT_EQ((*delegated)->deadline->text, "5s");
  EXPECT_EQ((*delegated)->recovery->kind, ExprKind::Fail);
  auto asked = Parser("ask warehouse.stock(\"bolt\") within 1s else 0")
                   .parseExpression();
  ASSERT_TRUE(asked) << (asked ? "" : formatDiagnostic(asked.error()));
  EXPECT_EQ((*asked)->kind, ExprKind::Ask);
  EXPECT_EQ((*asked)->children[0]->children[0]->kind, ExprKind::Field);
  EXPECT_EQ((*asked)->recovery->kind, ExprKind::Integer);
}

TEST(VAgentParserTest, AwaitEventCapturesNamedEventAndGuard) {
  auto parsed = Parser("await Ready as r [r.job_id == wanted] within 2s")
                    .parseExpression();
  ASSERT_TRUE(parsed) << (parsed ? "" : formatDiagnostic(parsed.error()));
  EXPECT_EQ((*parsed)->kind, ExprKind::AwaitEvent);
  EXPECT_EQ((*parsed)->text, "Ready");
  EXPECT_EQ((*parsed)->binding, "r");
  EXPECT_EQ((*parsed)->guard->text, "==");
  EXPECT_EQ((*parsed)->deadline->text, "2s");
}

TEST(VAgentParserTest, ElseBindsToNearestWaitingAction) {
  auto parsed = Parser("if valid then ask peer.stock() within 1s else 0")
                    .parseExpression();
  ASSERT_TRUE(parsed) << (parsed ? "" : formatDiagnostic(parsed.error()));
  EXPECT_EQ((*parsed)->kind, ExprKind::If);
  ASSERT_EQ((*parsed)->children.size(), 2U);
  EXPECT_EQ((*parsed)->children[1]->kind, ExprKind::Ask);
  ASSERT_TRUE((*parsed)->children[1]->recovery);
}

TEST(VAgentParserTest, BracesSeparateOuterElseFromWaitingAction) {
  auto parsed = Parser("if valid then { ask peer.stock() within 1s } else 0")
                    .parseExpression();
  ASSERT_TRUE(parsed) << (parsed ? "" : formatDiagnostic(parsed.error()));
  EXPECT_EQ((*parsed)->kind, ExprKind::If);
  ASSERT_EQ((*parsed)->children.size(), 3U);
  EXPECT_EQ((*parsed)->children[1]->kind, ExprKind::Block);
  EXPECT_FALSE((*parsed)->children[1]->children[0]->recovery);
}

TEST(VAgentParserTest, DirectBlockStatementsAttachRecoveryToTheirCall) {
  auto parsed = Parser(R"(agent A {
    plan work for Work {
      read() else ()
      value := Sensor.read() else 0
      let result: Int := read()
        else 1
    }
  })")
                    .parseProgram();
  ASSERT_TRUE(parsed) << (parsed ? "" : formatDiagnostic(parsed.error()));
  const auto &statements = parsed->declarations[0].members[0].body->children;
  ASSERT_EQ(statements.size(), 3U);
  EXPECT_EQ(statements[0]->kind, ExprKind::Call);
  ASSERT_TRUE(statements[0]->recovery);
  EXPECT_EQ(statements[0]->recovery->kind, ExprKind::Unit);
  EXPECT_EQ(statements[1]->text, ":=");
  ASSERT_TRUE(statements[1]->children[1]->recovery);
  EXPECT_EQ(statements[1]->children[1]->recovery->text, "0");
  EXPECT_FALSE(statements[1]->recovery);
  EXPECT_EQ(statements[2]->kind, ExprKind::Let);
  EXPECT_EQ(statements[2]->annotation, (Type{"Int", {}}));
  ASSERT_TRUE(statements[2]->children[0]->recovery);
  EXPECT_EQ(statements[2]->children[0]->recovery->text, "1");
  EXPECT_FALSE(statements[2]->recovery);
}

TEST(VAgentParserTest, OrdinaryCallRecoveryIsParsedForCheckerResolution) {
  auto parsed = Parser("agent A { def value(): Int { ordinary() else 0 } }")
                    .parseProgram();
  ASSERT_TRUE(parsed) << (parsed ? "" : formatDiagnostic(parsed.error()));
  const auto &call = parsed->declarations[0].members[0].body->children[0];
  ASSERT_TRUE(call->recovery);
  EXPECT_EQ(call->recovery->text, "0");
}

TEST(VAgentParserTest, CallRecoveryDoesNotStealAnOuterIfBranch) {
  auto parsed = Parser(R"(agent A {
    plan work for Work {
      if ready then value := read() else value := 0
    }
  })")
                    .parseProgram();
  ASSERT_TRUE(parsed) << (parsed ? "" : formatDiagnostic(parsed.error()));
  const auto &branch = parsed->declarations[0].members[0].body->children[0];
  EXPECT_EQ(branch->kind, ExprKind::If);
  ASSERT_EQ(branch->children.size(), 3U);
  EXPECT_EQ(branch->children[1]->text, ":=");
  EXPECT_FALSE(branch->children[1]->children[1]->recovery);
  EXPECT_EQ(branch->children[2]->text, ":=");
}

TEST(VAgentParserTest, BracesAllowCallRecoveryInsideControlFlow) {
  auto parsed = Parser(R"(agent A {
    plan work for Work {
      if ready then { value := read() else 0 } else { value := 1 }
      while ready do { read() else () }
      for n in [1, 2] do { let value := read() else n }
    }
  })")
                    .parseProgram();
  ASSERT_TRUE(parsed) << (parsed ? "" : formatDiagnostic(parsed.error()));
  const auto &statements = parsed->declarations[0].members[0].body->children;
  ASSERT_EQ(statements.size(), 3U);
  ASSERT_EQ(statements[0]->children.size(), 3U);
  EXPECT_TRUE(statements[0]->children[1]->children[0]->children[1]->recovery);
  EXPECT_TRUE(statements[1]->children[1]->children[0]->recovery);
  EXPECT_TRUE(statements[2]->children[1]->children[0]->children[0]->recovery);
}

TEST(VAgentParserTest, StatementRecoveryRejectsNestedAndReversibleRoots) {
  for (const std::string_view source :
       {"agent A { plan p for G { value <- read() else 0 } }",
        "agent A { plan p for G { read()[0] else 0 } }",
        "agent A { plan p for G { read().value else 0 } }",
        "agent A { plan p for G { 1 + read() else 0 } }",
        "agent A { plan p for G { let v := 1 + read() else 0 } }",
        "agent A { plan p for G { let v := { read() } else 0 } }",
        "agent A { plan p for G { let v := if c then read() else 0 else 1 } }",
        "agent A { plan p for G { while c do read() else () } }",
        "agent A { plan p for G { for n in [1] do read() else () } }",
        "agent A { plan p for G { read() else 0 else 1 } }"}) {
    SCOPED_TRACE(source);
    auto parsed = Parser(source).parseProgram();
    ASSERT_FALSE(parsed);
    EXPECT_EQ(parsed.error().code, DiagnosticCode::Syntax);
  }
  EXPECT_FALSE(Parser("read() else 0").parseExpression());
}

TEST(VAgentParserTest, RecoveryTreesRecomputeCallAndWrapperHeightLimits) {
  ParseLimits limits;
  limits.maxDepth    = 12;
  std::string source = "agent A { plan p for G { value := read() else 1";
  for (int i = 0; i < 9; ++i) source += "+1";
  source += " } }";
  auto parsed = Parser(source, limits).parseProgram();
  ASSERT_FALSE(parsed);
  EXPECT_NE(parsed.error().detail.find("expression tree"), std::string::npos);
  auto allowed = Parser(source, ParseLimits{1'000, 32}).parseProgram();
  ASSERT_TRUE(allowed) << (allowed ? "" : formatDiagnostic(allowed.error()));
}

TEST(VAgentParserTest, NewlineContinuesBinaryButUnaryStartsNewStatement) {
  auto parsed = Parser(R"(agent A {
    on Tick {
      let sum := 1
        + 2
      -3
      *[1, 2]
      ![3, 4]
      (5)
      [6]
    }
  })")
                    .parseProgram();
  ASSERT_TRUE(parsed) << (parsed ? "" : formatDiagnostic(parsed.error()));
  const auto &statements = parsed->declarations[0].members[0].body->children;
  ASSERT_EQ(statements.size(), 6U);
  EXPECT_EQ(statements[0]->children[0]->text, "+");
  EXPECT_EQ(statements[1]->text, "-");
  EXPECT_EQ(statements[2]->text, "*");
  EXPECT_EQ(statements[3]->text, "!");
  EXPECT_EQ(statements[4]->kind, ExprKind::Integer);
  EXPECT_EQ(statements[5]->kind, ExprKind::List);
}

TEST(VAgentParserTest, GroupingSuppressesNewlinesButNestedBlocksRestoreThem) {
  auto parsed = Parser(R"(agent A {
    on Tick {
      let result := combine(
        1
        + 2,
        {
          3
          4
        }
      )
    }
  })")
                    .parseProgram();
  ASSERT_TRUE(parsed) << (parsed ? "" : formatDiagnostic(parsed.error()));
  const auto &call =
      parsed->declarations[0].members[0].body->children[0]->children[0];
  ASSERT_EQ(call->children.size(), 3U);
  EXPECT_EQ(call->children[1]->text, "+");
  EXPECT_EQ(call->children[2]->kind, ExprKind::Block);
  EXPECT_EQ(call->children[2]->children.size(), 2U);
}

TEST(VAgentParserTest, DeclarationHeadersMayCrossNewlines) {
  auto parsed = Parser(R"(
    agent
      Worker
      (limit:
        Int)
    {
      var value:
        Int :=
        1
      on
        Tick
      { value := limit }
    }
  )")
                    .parseProgram();
  ASSERT_TRUE(parsed) << (parsed ? "" : formatDiagnostic(parsed.error()));
  EXPECT_EQ(parsed->declarations[0].name, "Worker");
  EXPECT_EQ(parsed->declarations[0].members.size(), 2U);
}

TEST(VAgentParserTest, ParsesLogicQueriesSourceAndDeterministicBeliefLoop) {
  auto parsed = Parser(R"(agent A {
    on Tick {
      let found := findall(?value, reading("left", ?value))
      if believes reading("left", ?value) from self then ()
      for believes reading(?sensor, ?value) do emit Seen{value: value}
    }
  })")
                    .parseProgram();
  ASSERT_TRUE(parsed) << (parsed ? "" : formatDiagnostic(parsed.error()));
  const auto &statements = parsed->declarations[0].members[0].body->children;
  ASSERT_EQ(statements.size(), 3U);
  EXPECT_EQ(statements[0]->children[0]->kind, ExprKind::FindAll);
  EXPECT_EQ(statements[1]->children[0]->scope->text, "self");
  EXPECT_EQ(statements[2]->kind, ExprKind::ForBelieves);
}

TEST(VAgentParserTest, RecordLiteralInsideHeaderRequiresParentheses) {
  EXPECT_TRUE(Parser("goal G() achieve { succeed when accepts(Point{x: 1}) }")
                  .parseProgram());
  auto invalid =
      Parser("plan p for G where Point{x: 1} == target { () }").parseProgram();
  EXPECT_FALSE(invalid);
  auto valid = Parser("plan p for G where (Point{x: 1}) == target { () }")
                   .parseProgram();
  ASSERT_TRUE(valid) << (valid ? "" : formatDiagnostic(valid.error()));
}

TEST(VAgentParserTest, RefusesMalformedAndMisplacedSyntax) {
  for (const std::string_view source :
       {"agent A {", "agent A { on Tick { 1 2 } }",
        "agent A { plan event for G { () } }", "record R",
        "predicate p(key value)", "agent A { var value: Int }",
        "question q():", "capacity C { on Tick {} }", "native agent A {}",
        "skill S implements C { on Tick {} }", "def top(): Int { 1 }",
        "event E { value Int }", "goal G() unknown",
        "agent A { on Tick as {} }", "agent A { on Tick priority 1.5 {} }"}) {
    SCOPED_TRACE(source);
    auto parsed = Parser(source).parseProgram();
    EXPECT_FALSE(parsed);
    if (!parsed) EXPECT_EQ(parsed.error().code, DiagnosticCode::Syntax);
  }
}

TEST(VAgentParserTest, RefusesMissingDeadlinesAndIncompleteActions) {
  for (const auto source :
       {"ask peer.stock()", "await ready()", "await Ready as r",
        "achieve Fetch() by peer", "spawn Child", "believe ready", "emit",
        "tell peer", "for item xs do ()"}) {
    SCOPED_TRACE(source);
    EXPECT_FALSE(Parser(source).parseExpression());
  }
}

TEST(VAgentParserTest, ScanWithinTermArgumentAcceptsExpressions) {
  auto unparen =
      Parser("believes reading(subject ? expression)").parseExpression();
  ASSERT_TRUE(unparen) << (unparen ? "" : formatDiagnostic(unparen.error()));
  EXPECT_EQ((*unparen)->children[0]->children[0]->text, "?");
  auto valid =
      Parser("believes reading((subject ? expression))").parseExpression();
  ASSERT_TRUE(valid) << (valid ? "" : formatDiagnostic(valid.error()));
  EXPECT_EQ((*valid)->children[0]->children[0]->text, "?");
}

TEST(VAgentParserTest, SyntaxErrorsCarryExactSourceLocations) {
  auto parsed =
      Parser("event Good\nagent A {\n  var count: Int :=\n}").parseProgram();
  ASSERT_FALSE(parsed);
  EXPECT_EQ(parsed.error().loc.line, 4U);
  EXPECT_EQ(parsed.error().loc.column, 1U);
  EXPECT_NE(parsed.error().detail.find("expression"), std::string::npos);
}

TEST(VAgentParserTest, PropagatesLexicalFailureWithoutPartialProgram) {
  auto parsed =
      Parser("event Good\nagent A { var text: Str := \"unterminated }")
          .parseProgram();
  ASSERT_FALSE(parsed);
  EXPECT_EQ(parsed.error().code, DiagnosticCode::Lexical);
}

TEST(VAgentParserTest, CellsStringParsesAsADedicatedExpression) {
  auto parsed = Parser(R"(cells"##/d.items")").parseExpression();
  ASSERT_TRUE(parsed) << formatDiagnostic(parsed.error());
  ASSERT_TRUE(*parsed);
  EXPECT_EQ((*parsed)->kind, ExprKind::Cells);
  EXPECT_EQ((*parsed)->text, "##/d.items");
}

TEST(VAgentParserTest, UnknownStringPrefixIsRefused) {
  auto parsed = Parser(R"(vql"##")").parseExpression();
  ASSERT_FALSE(parsed);
  EXPECT_NE(parsed.error().detail.find("unknown string prefix"),
            std::string::npos);
}

TEST(VAgentParserTest, CodeStringParsesAsADedicatedExpression) {
  auto parsed = Parser(R"(code"x + w")").parseExpression();
  ASSERT_TRUE(parsed) << formatDiagnostic(parsed.error());
  ASSERT_TRUE(*parsed);
  EXPECT_EQ((*parsed)->kind, ExprKind::Code);
  // The literal keeps its decoded inner text raw — expand runs
  // parseCodeBody per literal, not the parser.
  EXPECT_EQ((*parsed)->text, "x + w");
}

TEST(VAgentParserTest, SpliceParsesAPrefixedPostfixOperand) {
  auto parsed = Parser("splice c").parseExpression();
  ASSERT_TRUE(parsed) << formatDiagnostic(parsed.error());
  ASSERT_EQ((*parsed)->kind, ExprKind::Splice);
  ASSERT_EQ((*parsed)->children.size(), 1U);
  EXPECT_EQ((*parsed)->children[0]->kind, ExprKind::Identifier);
  EXPECT_EQ((*parsed)->children[0]->text, "c");
}

TEST(VAgentParserTest, SpliceCallPeelsTheArglist) {
  // §5.1's peeling rule: the operand's outermost postfix ends in parens,
  // so the arglist belongs to the splice — children[0] is the fragment
  // name, the rest its actuals.
  auto parsed = Parser("splice inc(count)").parseExpression();
  ASSERT_TRUE(parsed) << formatDiagnostic(parsed.error());
  ASSERT_EQ((*parsed)->kind, ExprKind::Splice);
  ASSERT_EQ((*parsed)->children.size(), 2U);
  EXPECT_EQ((*parsed)->children[0]->kind, ExprKind::Identifier);
  EXPECT_EQ((*parsed)->children[0]->text, "inc");
  EXPECT_EQ((*parsed)->children[1]->kind, ExprKind::Identifier);
  EXPECT_EQ((*parsed)->children[1]->text, "count");
}

TEST(VAgentParserTest, SpliceParenthesizedCallStaysCallShaped) {
  // 'splice (f(x))' splices a call result — the arglist never reached the
  // operand's outermost postfix, so it stays a call-shaped operand for
  // the checker to refuse.
  auto parsed = Parser("splice (inc(count))").parseExpression();
  ASSERT_TRUE(parsed) << formatDiagnostic(parsed.error());
  ASSERT_EQ((*parsed)->kind, ExprKind::Splice);
  ASSERT_EQ((*parsed)->children.size(), 1U);
  EXPECT_EQ((*parsed)->children[0]->kind, ExprKind::Call);
}

TEST(VAgentParserTest, SpliceFieldOrIndexOperandDoesNotPeel) {
  auto field = Parser("splice inc(count).frag").parseExpression();
  ASSERT_TRUE(field) << formatDiagnostic(field.error());
  ASSERT_EQ((*field)->kind, ExprKind::Splice);
  ASSERT_EQ((*field)->children.size(), 1U);
  EXPECT_EQ((*field)->children[0]->kind, ExprKind::Field);
  auto indexed = Parser("splice fragments[0]").parseExpression();
  ASSERT_TRUE(indexed) << formatDiagnostic(indexed.error());
  ASSERT_EQ((*indexed)->kind, ExprKind::Splice);
  ASSERT_EQ((*indexed)->children.size(), 1U);
  EXPECT_EQ((*indexed)->children[0]->kind, ExprKind::Index);
}

TEST(VAgentParserTest, SpliceRequiresAnOperand) {
  EXPECT_FALSE(Parser("splice").parseExpression());
  // A newline after 'splice' refuses rather than swallowing the next
  // statement's expression as the operand.
  EXPECT_FALSE(Parser("splice\nc + 1").parseExpression());
}

TEST(VAgentParserTest, SpliceAndStaticStayLegalMemberNames) {
  // Contextual words: any Word may follow '.', so members named splice or
  // static remain callable.
  auto parsed = Parser("x.splice(3) + y.static").parseExpression();
  ASSERT_TRUE(parsed) << formatDiagnostic(parsed.error());
  EXPECT_EQ((*parsed)->kind, ExprKind::Binary);
}

TEST(VAgentParserTest, StaticParsesAUnaryPrefixedOperand) {
  auto folded = Parser("static (60 * 1000)").parseExpression();
  ASSERT_TRUE(folded) << formatDiagnostic(folded.error());
  ASSERT_EQ((*folded)->kind, ExprKind::Static);
  ASSERT_EQ((*folded)->children.size(), 1U);
  EXPECT_EQ((*folded)->children[0]->kind, ExprKind::Binary);
  // 'static' is Unary-prefixed: the operand binds at unary level, so
  // 'static 60 * 1000' folds 60 and multiplies the result (§5.5).
  auto unparenthesized = Parser("static 60 * 1000").parseExpression();
  ASSERT_TRUE(unparenthesized) << formatDiagnostic(unparenthesized.error());
  ASSERT_EQ((*unparenthesized)->kind, ExprKind::Binary);
  EXPECT_EQ((*unparenthesized)->text, "*");
  ASSERT_EQ((*unparenthesized)->children.size(), 2U);
  EXPECT_EQ((*unparenthesized)->children[0]->kind, ExprKind::Static);
  EXPECT_EQ((*unparenthesized)->children[0]->children[0]->kind,
            ExprKind::Integer);
}

TEST(VAgentParserTest, StaticRequiresAnOperand) {
  EXPECT_FALSE(Parser("static").parseExpression());
  EXPECT_FALSE(Parser("static\n60 * 1000").parseExpression());
}

TEST(VAgentParserTest, CodeTypeSyntaxParsesTypedAndBareForms) {
  // 'Code<T>' and bare 'Code' parse through the ordinary type production —
  // the 0-or-1 arity chain is the checker's (§5.2).
  auto parsed = Parser::parseCodeBody("f: Code<Int>, g: Code | splice f");
  ASSERT_TRUE(parsed) << formatDiagnostic(parsed.error());
  ASSERT_EQ(parsed->parameters.size(), 2U);
  EXPECT_EQ(typeName(parsed->parameters[0].type), "Code<Int>");
  EXPECT_EQ(typeName(parsed->parameters[1].type), "Code");
  EXPECT_EQ(parsed->body->kind, ExprKind::Splice);
}

TEST(VAgentParserTest, ParseCodeBodyParsesPlainLabelledAndParameterized) {
  auto plain = Parser::parseCodeBody("x + w");
  ASSERT_TRUE(plain) << formatDiagnostic(plain.error());
  EXPECT_FALSE(plain->label);
  EXPECT_TRUE(plain->parameters.empty());
  ASSERT_TRUE(plain->body);
  EXPECT_EQ(plain->body->kind, ExprKind::Binary);

  auto labelled = Parser::parseCodeBody("#inc n: Int | n + 1");
  ASSERT_TRUE(labelled) << formatDiagnostic(labelled.error());
  ASSERT_TRUE(labelled->label);
  EXPECT_EQ(*labelled->label, "inc");
  ASSERT_EQ(labelled->parameters.size(), 1U);
  EXPECT_EQ(labelled->parameters[0].name, "n");
  EXPECT_EQ(labelled->parameters[0].type.name, "Int");
  ASSERT_TRUE(labelled->body);
  EXPECT_EQ(labelled->body->kind, ExprKind::Binary);

  auto two = Parser::parseCodeBody("w: Int, b: Bool | if b then w + 1 else w");
  ASSERT_TRUE(two) << formatDiagnostic(two.error());
  EXPECT_FALSE(two->label);
  ASSERT_EQ(two->parameters.size(), 2U);
  EXPECT_EQ(two->parameters[1].name, "b");
  EXPECT_EQ(two->parameters[1].type.name, "Bool");
  EXPECT_EQ(two->body->kind, ExprKind::If);
}

TEST(VAgentParserTest, ParseCodeBodyReportsLiteralRelativeLocations) {
  // The '#'-strip blanks the consumed span, so a syntax error inside the
  // literal reports literal-relative inner coordinates — the reporter
  // qualifies them into src:m:l:c+o:c (code_path's innerDiagnostic).
  auto parsed = Parser::parseCodeBody("#lab x + )");
  ASSERT_FALSE(parsed);
  EXPECT_EQ(parsed.error().loc.line, 1U);
  EXPECT_EQ(parsed.error().loc.column, 10U);
  EXPECT_TRUE(parsed.error().inner.empty());
}

TEST(VAgentParserTest, ParseCodeBodyRefusesMalformedLabelsAndTrailing) {
  EXPECT_FALSE(Parser::parseCodeBody("#"));
  EXPECT_FALSE(Parser::parseCodeBody("#9x e"));
  EXPECT_FALSE(Parser::parseCodeBody("# e"));
  EXPECT_FALSE(Parser::parseCodeBody("x y"));
  EXPECT_FALSE(Parser::parseCodeBody(""));
}

TEST(VAgentParserTest, ParseCodeBodyParsesSpliceAndStaticInside) {
  auto splice = Parser::parseCodeBody("x + splice c");
  ASSERT_TRUE(splice) << formatDiagnostic(splice.error());
  ASSERT_EQ(splice->body->children.size(), 2U);
  EXPECT_EQ(splice->body->children[1]->kind, ExprKind::Splice);
  auto staged = Parser::parseCodeBody("splice inc(static (2 * 3000))");
  ASSERT_TRUE(staged) << formatDiagnostic(staged.error());
  ASSERT_EQ(staged->body->kind, ExprKind::Splice);
  ASSERT_EQ(staged->body->children.size(), 2U);
  EXPECT_EQ(staged->body->children[1]->kind, ExprKind::Static);
  // The inner grammar is the vagent grammar, so a nested code"…" literal
  // parses as an inner literal — escaped one level in the source spelling.
  auto nested = Parser::parseCodeBody("x + splice code\"n: Int | n + 1\"(x)");
  ASSERT_TRUE(nested) << formatDiagnostic(nested.error());
  ASSERT_EQ(nested->body->kind, ExprKind::Binary);
  const auto &inner = *nested->body->children[1];
  ASSERT_EQ(inner.kind, ExprKind::Splice);
  ASSERT_EQ(inner.children.size(), 2U);
  EXPECT_EQ(inner.children[0]->kind, ExprKind::Code);
  EXPECT_EQ(inner.children[0]->text, "n: Int | n + 1");
}

TEST(VAgentParserTest, ParseCodeBodyParsesHeaderlessAndEmptyForms) {
  auto unit = Parser::parseCodeBody("()");
  ASSERT_TRUE(unit) << formatDiagnostic(unit.error());
  EXPECT_EQ(unit->body->kind, ExprKind::Unit);
  auto block = Parser::parseCodeBody("{ x := 1; x }");
  ASSERT_TRUE(block) << formatDiagnostic(block.error());
  EXPECT_EQ(block->body->kind, ExprKind::Block);
  // An assignment body is a legal Expr — its impurity is the checker's.
  auto assign = Parser::parseCodeBody("w := 3");
  ASSERT_TRUE(assign) << formatDiagnostic(assign.error());
  EXPECT_EQ(assign->body->kind, ExprKind::Binary);
}

TEST(VAgentParserTest, ConfiguredLimitsBoundRecursionAndConstructedTrees) {
  ParseLimits limits;
  limits.maxDepth = 12;
  std::string nested(30, '(');
  nested += '1';
  nested += std::string(30, ')');
  EXPECT_FALSE(Parser(nested, limits).parseExpression());
  std::string chain = "1";
  for (int i = 0; i < 30; ++i) chain += "+1";
  EXPECT_FALSE(Parser(chain, limits).parseExpression());
  std::string fields = "value";
  for (int i = 0; i < 30; ++i) fields += ".field";
  EXPECT_FALSE(Parser(fields, limits).parseExpression());
  limits.maxTokens = 2;
  EXPECT_FALSE(Parser("event E", limits).parseProgram());
}

ExprPtr makeExpr(ExprKind kind, std::string text = {}) {
  auto result  = std::make_unique<Expr>();
  result->kind = kind;
  result->text = std::move(text);
  return result;
}

void expectSameLocation(const SourceLocation &expected,
                        const SourceLocation &actual) {
  EXPECT_EQ(expected.line, actual.line);
  EXPECT_EQ(expected.column, actual.column);
  EXPECT_EQ(expected.offset, actual.offset);
}

// Structural equality plus pointer-distinctness: a clone shares no node with
// its source, because kernel side tables key on `const Expr *`.
void expectSameExpr(const Expr &expected, const Expr &actual) {
  EXPECT_NE(&expected, &actual);
  EXPECT_EQ(expected.kind, actual.kind);
  EXPECT_EQ(expected.text, actual.text);
  expectSameLocation(expected.loc, actual.loc);
  EXPECT_EQ(expected.names, actual.names);
  EXPECT_EQ(expected.annotation, actual.annotation);
  EXPECT_EQ(expected.binding, actual.binding);
  EXPECT_EQ(expected.stamp, actual.stamp);
  ASSERT_EQ(expected.children.size(), actual.children.size());
  for (std::size_t i = 0; i < expected.children.size(); ++i)
    expectSameExpr(*expected.children[i], *actual.children[i]);
  const auto metadata = [](const ExprPtr &left, const ExprPtr &right) {
    ASSERT_EQ(left == nullptr, right == nullptr);
    if (left != nullptr) expectSameExpr(*left, *right);
  };
  metadata(expected.scope, actual.scope);
  metadata(expected.recipients, actual.recipients);
  metadata(expected.deadline, actual.deadline);
  metadata(expected.guard, actual.guard);
  metadata(expected.recovery, actual.recovery);
}

void expectSameOptionalExpr(const ExprPtr &expected, const ExprPtr &actual) {
  ASSERT_EQ(expected == nullptr, actual == nullptr);
  if (expected != nullptr) expectSameExpr(*expected, *actual);
}

void expectSameDecl(const Decl &expected, const Decl &actual) {
  EXPECT_NE(&expected, &actual);
  EXPECT_EQ(expected.kind, actual.kind);
  EXPECT_EQ(expected.name, actual.name);
  expectSameLocation(expected.loc, actual.loc);
  ASSERT_EQ(expected.parameters.size(), actual.parameters.size());
  for (std::size_t i = 0; i < expected.parameters.size(); ++i) {
    EXPECT_EQ(expected.parameters[i].name, actual.parameters[i].name);
    EXPECT_EQ(expected.parameters[i].type, actual.parameters[i].type);
    EXPECT_EQ(expected.parameters[i].key, actual.parameters[i].key);
    expectSameLocation(expected.parameters[i].loc, actual.parameters[i].loc);
  }
  EXPECT_EQ(expected.result, actual.result);
  EXPECT_EQ(expected.names, actual.names);
  EXPECT_EQ(expected.target, actual.target);
  EXPECT_EQ(expected.goalKind, actual.goalKind);
  EXPECT_EQ(expected.planBindings, actual.planBindings);
  ASSERT_EQ(expected.contracts.size(), actual.contracts.size());
  for (std::size_t i = 0; i < expected.contracts.size(); ++i) {
    EXPECT_EQ(expected.contracts[i].postcondition,
              actual.contracts[i].postcondition);
    expectSameLocation(expected.contracts[i].loc, actual.contracts[i].loc);
    expectSameOptionalExpr(expected.contracts[i].condition,
                           actual.contracts[i].condition);
  }
  ASSERT_EQ(expected.goalClauses.size(), actual.goalClauses.size());
  for (std::size_t i = 0; i < expected.goalClauses.size(); ++i) {
    EXPECT_EQ(expected.goalClauses[i].kind, actual.goalClauses[i].kind);
    expectSameLocation(expected.goalClauses[i].loc, actual.goalClauses[i].loc);
    expectSameOptionalExpr(expected.goalClauses[i].condition,
                           actual.goalClauses[i].condition);
  }
  ASSERT_EQ(expected.members.size(), actual.members.size());
  for (std::size_t i = 0; i < expected.members.size(); ++i)
    expectSameDecl(expected.members[i], actual.members[i]);
  ASSERT_EQ(expected.planArms.size(), actual.planArms.size());
  for (std::size_t i = 0; i < expected.planArms.size(); ++i) {
    expectSameOptionalExpr(expected.planArms[i].pattern,
                           actual.planArms[i].pattern);
    expectSameOptionalExpr(expected.planArms[i].guard,
                           actual.planArms[i].guard);
    expectSameOptionalExpr(expected.planArms[i].body, actual.planArms[i].body);
    expectSameLocation(expected.planArms[i].loc, actual.planArms[i].loc);
  }
  expectSameOptionalExpr(expected.body, actual.body);
  expectSameOptionalExpr(expected.guard, actual.guard);
  expectSameOptionalExpr(expected.initializer, actual.initializer);
  expectSameOptionalExpr(expected.trigger, actual.trigger);
  expectSameOptionalExpr(expected.deadline, actual.deadline);
  EXPECT_EQ(expected.binding, actual.binding);
  EXPECT_EQ(expected.priority, actual.priority);
  EXPECT_EQ(expected.native, actual.native);
  EXPECT_EQ(expected.effect, actual.effect);
  EXPECT_EQ(expected.preempts, actual.preempts);
}

void expectSameProgram(const Program &expected, const Program &actual) {
  EXPECT_EQ(expected.module, actual.module);
  ASSERT_EQ(expected.imports.size(), actual.imports.size());
  for (std::size_t i = 0; i < expected.imports.size(); ++i) {
    EXPECT_EQ(expected.imports[i].path, actual.imports[i].path);
    EXPECT_EQ(expected.imports[i].alias, actual.imports[i].alias);
    EXPECT_EQ(expected.imports[i].symbols, actual.imports[i].symbols);
    expectSameLocation(expected.imports[i].loc, actual.imports[i].loc);
  }
  ASSERT_EQ(expected.declarations.size(), actual.declarations.size());
  for (std::size_t i = 0; i < expected.declarations.size(); ++i)
    expectSameDecl(expected.declarations[i], actual.declarations[i]);
}

TEST(VAgentAstCloneTest, CloneExprCopiesEveryFieldAndOwnsEveryNode) {
  Expr original;
  original.kind       = ExprKind::AwaitEvent;
  original.text       = "Ping";
  original.loc        = SourceLocation{3, 7, 42};
  original.names      = {"first", "second"};
  original.annotation = Type{"List", {Type{"Int", {}}}};
  original.binding    = "pong";
  original.stamp      = ScopeStamp{.scope = 11, .ordinal = 4};
  original.children.push_back(makeExpr(ExprKind::Integer, "1"));
  original.children.back()->stamp = ScopeStamp{.scope = 11};
  original.scope                  = makeExpr(ExprKind::Identifier, "inner");
  original.recipients             = makeExpr(ExprKind::Identifier, "parent");
  original.deadline               = makeExpr(ExprKind::Duration, "1s");
  original.guard                  = makeExpr(ExprKind::Boolean, "true");
  original.recovery               = makeExpr(ExprKind::Integer, "0");

  const auto copy = cloneExpr(original);
  ASSERT_TRUE(copy);
  expectSameExpr(original, *copy);
  // An absent metadata child clones to nullptr rather than a fresh node.
  EXPECT_EQ(cloneExpr(ExprPtr{}), nullptr);
}

TEST(VAgentAstCloneTest, CloneDeclCopiesNestedMembersClausesAndFlags) {
  // The population is intentionally ungrammatical — one Decl carrying every
  // field at once, so the coverage does not depend on any one declaration
  // kind's real shape.
  Decl original;
  original.kind         = DeclKind::Goal;
  original.name         = "Enough";
  original.loc          = SourceLocation{9, 2, 100};
  original.parameters   = {Parameter{.name = "minimum",
                                     .type = Type{"Int", {}},
                                     .key  = true,
                                     .loc  = SourceLocation{9, 13, 111}}};
  original.result       = Type{"Bool", {}};
  original.names        = {"Base", "Mixin"};
  original.target       = "parentGoal";
  original.goalKind     = GoalKind::Maintain;
  original.planBindings = std::vector<std::string>{"bound", "names"};
  Contract contract;
  contract.postcondition = true;
  contract.loc           = SourceLocation{10, 3, 120};
  contract.condition     = makeExpr(ExprKind::Boolean, "true");
  original.contracts.push_back(std::move(contract));
  GoalClause clause;
  clause.kind      = GoalClause::Kind::Drop;
  clause.loc       = SourceLocation{11, 3, 140};
  clause.condition = makeExpr(ExprKind::Believes);
  clause.condition->children.push_back(makeExpr(ExprKind::Term, "level"));
  clause.condition->scope = makeExpr(ExprKind::Identifier, "self");
  original.goalClauses.push_back(std::move(clause));
  PlanArm arm;
  arm.loc     = SourceLocation{12, 5, 150};
  arm.pattern = makeExpr(ExprKind::WildcardPattern);
  arm.guard   = makeExpr(ExprKind::Boolean, "true");
  arm.body    = makeExpr(ExprKind::Block);
  original.planArms.push_back(std::move(arm));
  Decl member;
  member.kind        = DeclKind::Field;
  member.name        = "watch";
  member.result      = Type{"Opt", {Type{"Int", {}}}};
  member.initializer = makeExpr(ExprKind::None);
  original.members.push_back(std::move(member));
  original.body     = makeExpr(ExprKind::Block);
  original.guard    = makeExpr(ExprKind::Identifier, "ready");
  original.trigger  = makeExpr(ExprKind::Term, "started");
  original.deadline = makeExpr(ExprKind::Duration, "2s");
  original.binding  = "alias";
  original.priority = 7;
  original.native   = true;
  original.effect   = true;
  original.preempts = true;

  const auto copy = cloneDecl(original);
  expectSameDecl(original, copy);
}

TEST(VAgentAstCloneTest, CloneProgramRoundTripsEverySpecificationSample) {
  const std::filesystem::path directory("tests/samples/vagent");
  ASSERT_TRUE(std::filesystem::is_directory(directory));
  std::size_t clonedCount = 0;
  for (const auto &entry : std::filesystem::directory_iterator(directory)) {
    if (entry.path().extension() != ".vagent" ||
        !entry.path().filename().string().starts_with("mini_"))
      continue;
    SCOPED_TRACE(entry.path().string());
    std::ifstream stream(entry.path());
    const std::string source(std::istreambuf_iterator<char>(stream), {});
    auto parsed = Parser(source).parseProgram();
    ASSERT_TRUE(parsed.has_value())
        << (parsed ? "" : formatDiagnostic(parsed.error()));
    const auto copy = cloneProgram(*parsed);
    expectSameProgram(*parsed, copy);
    ++clonedCount;
  }
  EXPECT_EQ(clonedCount, 23U);
}

TEST(VAgentParserTest, ParseProgramToR1AllSpecificationSamples) {
  const std::filesystem::path directory("tests/samples/vagent");
  ASSERT_TRUE(std::filesystem::is_directory(directory));
  std::size_t sampleCount = 0;
  for (const auto &entry : std::filesystem::directory_iterator(directory)) {
    if (entry.path().extension() != ".vagent" ||
        !entry.path().filename().string().starts_with("mini_"))
      continue;
    SCOPED_TRACE(entry.path().string());
    std::ifstream stream(entry.path());
    const std::string source(std::istreambuf_iterator<char>(stream), {});

    // P21: Isolated scratch compilation arena
    zigzag::ArenaManifold scratchArena;
    zigzag::vortex::VortexCore core(scratchArena);

    auto r1Prog = parseProgramToR1(scratchArena, core, source);
    ASSERT_TRUE(r1Prog.has_value())
        << "Failed to parse " << entry.path().filename().string()
        << " to R1: " << r1Prog.error();

    // Verify 2-degree rank invariants
    EXPECT_TRUE(scratchArena.contains(*r1Prog));
    EXPECT_EQ(scratchArena.textOf(*r1Prog), "program");

    auto unmarshaled = unmarshalR1ToProgram(scratchArena, core, *r1Prog);
    ASSERT_TRUE(unmarshaled.has_value())
        << "Failed to unmarshal R1 to program: " << unmarshaled.error();

    EXPECT_FALSE(unmarshaled->declarations.empty());
    ++sampleCount;
  }
  EXPECT_EQ(sampleCount, 23U);
}

TEST(VAgentParserTest, DelimiterOrthogonalityTuplesListsAndRecords) {
  auto unitExpr = Parser("()").parseExpression();
  ASSERT_TRUE(unitExpr);
  EXPECT_EQ((*unitExpr)->kind, ExprKind::Unit);

  auto parenExpr = Parser("(42)").parseExpression();
  ASSERT_TRUE(parenExpr);
  EXPECT_EQ((*parenExpr)->kind, ExprKind::Integer);
  EXPECT_EQ((*parenExpr)->text, "42");

  auto tuple1 = Parser("(42,)").parseExpression();
  ASSERT_TRUE(tuple1);
  EXPECT_EQ((*tuple1)->kind, ExprKind::Tuple);
  ASSERT_EQ((*tuple1)->children.size(), 1U);
  EXPECT_EQ((*tuple1)->children[0]->kind, ExprKind::Integer);

  auto tuple2 = Parser("(1, 2, 3)").parseExpression();
  ASSERT_TRUE(tuple2);
  EXPECT_EQ((*tuple2)->kind, ExprKind::Tuple);
  ASSERT_EQ((*tuple2)->children.size(), 3U);

  auto listSpread = Parser("[1, ...rest, 3]").parseExpression();
  ASSERT_TRUE(listSpread);
  EXPECT_EQ((*listSpread)->kind, ExprKind::List);
  ASSERT_EQ((*listSpread)->children.size(), 3U);
  EXPECT_EQ((*listSpread)->children[1]->kind, ExprKind::Unary);
  EXPECT_EQ((*listSpread)->children[1]->text, "...");
  ASSERT_EQ((*listSpread)->children[1]->children.size(), 1U);
  EXPECT_EQ((*listSpread)->children[1]->children[0]->kind,
            ExprKind::Identifier);
  EXPECT_EQ((*listSpread)->children[1]->children[0]->text, "rest");
}

TEST(VAgentParserTest, PatternMatchingAndMatchArms) {
  const std::string source = R"(
    match item with {
      0 => { false }
      ?val where val > 0 => { true }
      _ => { false }
    }
  )";
  auto expr                = Parser(source).parseExpression();
  ASSERT_TRUE(expr) << (expr ? "" : formatDiagnostic(expr.error()));
  EXPECT_EQ((*expr)->kind, ExprKind::Match);
  ASSERT_EQ((*expr)->children.size(), 4U);
  EXPECT_EQ((*expr)->children[0]->kind, ExprKind::Identifier);
  EXPECT_EQ((*expr)->children[0]->text, "item");

  // Arm 1: 0 => { false }
  EXPECT_EQ((*expr)->children[1]->kind, ExprKind::MatchArm);
  ASSERT_GE((*expr)->children[1]->children.size(), 2U);
  EXPECT_EQ((*expr)->children[1]->children[0]->kind, ExprKind::Integer);

  // Arm 2: ?val where val > 0 => { true }
  EXPECT_EQ((*expr)->children[2]->kind, ExprKind::MatchArm);
  EXPECT_EQ((*expr)->children[2]->children[0]->kind, ExprKind::LogicVariable);
  EXPECT_EQ((*expr)->children[2]->children[0]->text, "val");
  ASSERT_NE((*expr)->children[2]->guard, nullptr);
  EXPECT_EQ((*expr)->children[2]->guard->kind, ExprKind::Binary);

  // Arm 3: _ => { false }
  EXPECT_EQ((*expr)->children[3]->kind, ExprKind::MatchArm);
  EXPECT_EQ((*expr)->children[3]->children[0]->kind, ExprKind::WildcardPattern);
}

TEST(VAgentParserTest, MultiArmPlansAndDefaultArm) {
  const std::string source = R"(
    agent Worker {
      plan solve for Task(id) {
        case 0 => { yield }
        where id > 10 => { yield }
        default => { yield }
      }
    }
  )";
  auto prog                = Parser(source).parseProgram();
  ASSERT_TRUE(prog) << (prog ? "" : formatDiagnostic(prog.error()));
  ASSERT_EQ(prog->declarations.size(), 1U);
  const auto &agent = prog->declarations[0];
  ASSERT_EQ(agent.members.size(), 1U);
  const auto &plan = agent.members[0];
  EXPECT_EQ(plan.kind, DeclKind::Plan);
  ASSERT_EQ(plan.planArms.size(), 3U);

  // Arm 0: case 0 => { yield }
  ASSERT_NE(plan.planArms[0].pattern, nullptr);
  EXPECT_EQ(plan.planArms[0].pattern->kind, ExprKind::Integer);

  // Arm 1: where id > 10 => { yield }
  EXPECT_EQ(plan.planArms[1].pattern, nullptr);
  ASSERT_NE(plan.planArms[1].guard, nullptr);
  EXPECT_EQ(plan.planArms[1].guard->kind, ExprKind::Binary);

  // Arm 2: default => { yield }
  EXPECT_EQ(plan.planArms[2].pattern, nullptr);
  EXPECT_EQ(plan.planArms[2].guard, nullptr);
  ASSERT_NE(plan.planArms[2].body, nullptr);
}

TEST(VAgentParserTest, MapLiteralsAndMapTypes) {
  auto mapExpr = Parser(R"(Map { "alpha": 1, "beta": 2 })").parseExpression();
  ASSERT_TRUE(mapExpr);
  EXPECT_EQ((*mapExpr)->kind, ExprKind::Map);
  ASSERT_EQ((*mapExpr)->children.size(), 4U);
  EXPECT_EQ((*mapExpr)->children[0]->kind, ExprKind::String);
  EXPECT_EQ((*mapExpr)->children[1]->kind, ExprKind::Integer);

  const std::string progSource = R"(
    record Dictionary { entries: Map<Str, Int> }
  )";
  auto prog                    = Parser(progSource).parseProgram();
  ASSERT_TRUE(prog);
  ASSERT_EQ(prog->declarations.size(), 1U);
  const auto &rec = prog->declarations[0];
  ASSERT_EQ(rec.parameters.size(), 1U);
  EXPECT_EQ(rec.parameters[0].type.name, "Map");
  ASSERT_EQ(rec.parameters[0].type.arguments.size(), 2U);
  EXPECT_EQ(rec.parameters[0].type.arguments[0].name, "Str");
  EXPECT_EQ(rec.parameters[0].type.arguments[1].name, "Int");
}

TEST(VAgentParserTest, MicroVersionLiteralsAndSafeNavigationAndCoalescing) {
  auto mvHead = Parser("@HEAD").parseExpression();
  ASSERT_TRUE(mvHead);
  EXPECT_EQ((*mvHead)->kind, ExprKind::MicroVersionLiteral);
  EXPECT_EQ((*mvHead)->text, "HEAD");

  auto mvId = Parser("@2a4").parseExpression();
  ASSERT_TRUE(mvId);
  EXPECT_EQ((*mvId)->kind, ExprKind::MicroVersionLiteral);
  EXPECT_EQ((*mvId)->text, "2a4");

  auto safeNav = Parser("target?.state?.name").parseExpression();
  ASSERT_TRUE(safeNav);
  EXPECT_EQ((*safeNav)->kind, ExprKind::SafeNav);
  EXPECT_EQ((*safeNav)->text, "name");
  ASSERT_EQ((*safeNav)->children.size(), 1U);
  EXPECT_EQ((*safeNav)->children[0]->kind, ExprKind::SafeNav);
  EXPECT_EQ((*safeNav)->children[0]->text, "state");

  auto coalesce = Parser("opt ?? fallback").parseExpression();
  ASSERT_TRUE(coalesce);
  EXPECT_EQ((*coalesce)->kind, ExprKind::Coalesce);
  ASSERT_EQ((*coalesce)->children.size(), 2U);
  EXPECT_EQ((*coalesce)->children[0]->kind, ExprKind::Identifier);
  EXPECT_EQ((*coalesce)->children[1]->kind, ExprKind::Identifier);

  auto unwrap = Parser("opt?").parseExpression();
  ASSERT_TRUE(unwrap);
  EXPECT_EQ((*unwrap)->kind, ExprKind::Unary);
  EXPECT_EQ((*unwrap)->text, "?");
  ASSERT_EQ((*unwrap)->children.size(), 1U);
  EXPECT_EQ((*unwrap)->children[0]->kind, ExprKind::Identifier);

  auto scanExpr = Parser("scan text with { tab(1) }").parseExpression();
  ASSERT_TRUE(scanExpr);
  EXPECT_EQ((*scanExpr)->kind, ExprKind::Binary);
  EXPECT_EQ((*scanExpr)->text, "?");
  ASSERT_EQ((*scanExpr)->children.size(), 2U);
  EXPECT_EQ((*scanExpr)->children[0]->kind, ExprKind::Identifier);
  EXPECT_EQ((*scanExpr)->children[1]->kind, ExprKind::Block);
}

TEST(VAgentParserTest, LetDestructuringWithElseRecovery) {
  const std::string blockSource = R"({
    let (?head, ?tail) := split() else { fail }
    let _ := ignore()
  })";
  auto block                    = Parser(blockSource).parseExpression();
  ASSERT_TRUE(block) << (block ? "" : formatDiagnostic(block.error()));
  EXPECT_EQ((*block)->kind, ExprKind::Block);
  ASSERT_EQ((*block)->children.size(), 2U);

  const auto &let1 = (*block)->children[0];
  EXPECT_EQ(let1->kind, ExprKind::Let);
  ASSERT_EQ(let1->children.size(), 2U);
  EXPECT_EQ(let1->children[0]->kind, ExprKind::TuplePattern);
  EXPECT_EQ(let1->children[1]->kind, ExprKind::Call);
  ASSERT_NE(let1->recovery, nullptr);
  EXPECT_EQ(let1->recovery->kind, ExprKind::Block);

  const auto &let2 = (*block)->children[1];
  EXPECT_EQ(let2->kind, ExprKind::Let);
  ASSERT_EQ(let2->children.size(), 2U);
  EXPECT_EQ(let2->children[0]->kind, ExprKind::WildcardPattern);
}

TEST(VAgentParserTest, EventDestructuringAndWhereGuard) {
  const std::string source = R"(
    agent Receiver {
      on Message { id: ?msgId, payload: ?p } where msgId > 0 {
        yield
      }
    }
  )";
  auto prog                = Parser(source).parseProgram();
  ASSERT_TRUE(prog) << (prog ? "" : formatDiagnostic(prog.error()));
  ASSERT_EQ(prog->declarations.size(), 1U);
  const auto &agent = prog->declarations[0];
  ASSERT_EQ(agent.members.size(), 1U);
  const auto &handler = agent.members[0];
  EXPECT_EQ(handler.kind, DeclKind::Handler);
  ASSERT_NE(handler.trigger, nullptr);
  EXPECT_EQ(handler.trigger->kind, ExprKind::RecordPattern);
  EXPECT_EQ(handler.trigger->text, "Message");
  ASSERT_NE(handler.guard, nullptr);
  EXPECT_EQ(handler.guard->kind, ExprKind::Binary);
}

} // namespace
