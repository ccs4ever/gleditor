#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "common/xanadu/vagent/parser.hpp"

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

TEST(VAgentParserTest, ScanWithinTermArgumentRequiresParentheses) {
  EXPECT_FALSE(
      Parser("believes reading(subject ? expression)").parseExpression());
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

} // namespace
