#include <gtest/gtest.h>

#include <cmath>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "common/xanadu/vagent/ast_bridge.hpp"
#include "common/xanadu/vagent/checker.hpp"
#include "common/xanadu/vagent/parser.hpp"
#include "common/xanadu/vagent/runtime/compiler.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {
using namespace xanadu::vagent;

TEST(VagentAstConsolidationTest, PrimitiveAndCompositeTypeRoundtrip) {
  const std::vector<Type> testTypes = {
      Type{"Int", {}},
      Type{"Float", {}},
      Type{"Str", {}},
      Type{"Bool", {}},
      Type{"Duration", {}},
      Type{"()", {}},
      Type{"None", {}},
      Type{"List", {Type{"Int", {}}}},
      Type{"Map", {Type{"Str", {}}, Type{"Int", {}}}},
      Type{"List", {Type{"List", {Type{"Str", {}}}}}},
      Type{"Opt", {Type{"Agent", {}}}},
  };

  for (const auto &type : testTypes) {
    SCOPED_TRACE(typeName(type));
    auto lowered = lowerType(type);
    auto lifted  = liftType(lowered);
    EXPECT_EQ(lifted, type);
    EXPECT_TRUE(areEquivalent(type, lowered));
  }
}

TEST(VagentAstConsolidationTest, ExpressionLiteralsAndOperatorsRoundtrip) {
  // Integer
  {
    Expr intExpr;
    intExpr.kind = ExprKind::Integer;
    intExpr.text = "42";
    auto lowered = lowerExpr(intExpr);
    ASSERT_TRUE(lowered.has_value());
    EXPECT_TRUE(std::holds_alternative<runtime::ELiteral>((*lowered)->node));
    auto lifted = liftExpr(**lowered);
    ASSERT_TRUE(lifted.has_value());
    EXPECT_TRUE(areEquivalent(intExpr, **lowered));
    EXPECT_EQ((*lifted)->kind, ExprKind::Integer);
    EXPECT_EQ((*lifted)->text, "42");
  }

  // Float
  {
    Expr fltExpr;
    fltExpr.kind = ExprKind::Float;
    fltExpr.text = "3.14";
    auto lowered = lowerExpr(fltExpr);
    ASSERT_TRUE(lowered.has_value());
    EXPECT_TRUE(areEquivalent(fltExpr, **lowered));
    auto lifted = liftExpr(**lowered);
    ASSERT_TRUE(lifted.has_value());
    EXPECT_EQ((*lifted)->kind, ExprKind::Float);
  }

  // Boolean
  {
    Expr bExpr;
    bExpr.kind   = ExprKind::Boolean;
    bExpr.text   = "true";
    auto lowered = lowerExpr(bExpr);
    ASSERT_TRUE(lowered.has_value());
    EXPECT_TRUE(areEquivalent(bExpr, **lowered));
    auto lifted = liftExpr(**lowered);
    ASSERT_TRUE(lifted.has_value());
    EXPECT_EQ((*lifted)->text, "true");
  }

  // Duration
  {
    Expr dExpr;
    dExpr.kind   = ExprKind::Duration;
    dExpr.text   = "250ms";
    auto lowered = lowerExpr(dExpr);
    ASSERT_TRUE(lowered.has_value());
    EXPECT_TRUE(areEquivalent(dExpr, **lowered));
    auto lifted = liftExpr(**lowered);
    ASSERT_TRUE(lifted.has_value());
    EXPECT_EQ((*lifted)->text, "250ms");
  }

  // Unary negation and not
  for (const std::string &op : {"-", "not", "size", "^"}) {
    Expr uExpr;
    uExpr.kind  = ExprKind::Unary;
    uExpr.text  = op;
    auto child  = std::make_unique<Expr>();
    child->kind = ExprKind::Identifier;
    child->text = "x";
    uExpr.children.push_back(std::move(child));

    auto lowered = lowerExpr(uExpr);
    ASSERT_TRUE(lowered.has_value()) << op;
    EXPECT_TRUE(areEquivalent(uExpr, **lowered));
    auto lifted = liftExpr(**lowered);
    ASSERT_TRUE(lifted.has_value());
    EXPECT_EQ((*lifted)->kind, ExprKind::Unary);
  }

  // Binary operations
  for (const std::string &op : {"+", "-", "*", "/", "%", "++", "<", "<=", ">",
                                ">=", "==", "!=", "|", "?", ":=", "<-", "&"}) {
    Expr bExpr;
    bExpr.kind = ExprKind::Binary;
    bExpr.text = op;
    auto l     = std::make_unique<Expr>();
    l->kind    = ExprKind::Identifier;
    l->text    = "a";
    auto r     = std::make_unique<Expr>();
    r->kind    = ExprKind::Identifier;
    r->text    = "b";
    bExpr.children.push_back(std::move(l));
    bExpr.children.push_back(std::move(r));

    auto lowered = lowerExpr(bExpr);
    ASSERT_TRUE(lowered.has_value()) << op;
    EXPECT_TRUE(areEquivalent(bExpr, **lowered));
    auto lifted = liftExpr(**lowered);
    ASSERT_TRUE(lifted.has_value());
    EXPECT_EQ((*lifted)->kind, ExprKind::Binary);
  }
}

TEST(VagentAstConsolidationTest, ControlFlowAndActionExpressionsRoundtrip) {
  // If-then-else
  {
    Expr ifExpr;
    ifExpr.kind   = ExprKind::If;
    auto cond     = std::make_unique<Expr>();
    cond->kind    = ExprKind::Boolean;
    cond->text    = "true";
    auto thenArm  = std::make_unique<Expr>();
    thenArm->kind = ExprKind::Integer;
    thenArm->text = "1";
    auto elseArm  = std::make_unique<Expr>();
    elseArm->kind = ExprKind::Integer;
    elseArm->text = "2";
    ifExpr.children.push_back(std::move(cond));
    ifExpr.children.push_back(std::move(thenArm));
    ifExpr.children.push_back(std::move(elseArm));

    auto lowered = lowerExpr(ifExpr);
    ASSERT_TRUE(lowered.has_value());
    EXPECT_TRUE(areEquivalent(ifExpr, **lowered));
    auto lifted = liftExpr(**lowered);
    ASSERT_TRUE(lifted.has_value());
    EXPECT_EQ((*lifted)->kind, ExprKind::If);
    EXPECT_EQ((*lifted)->children.size(), 3U);
  }

  // While
  {
    Expr whileExpr;
    whileExpr.kind = ExprKind::While;
    auto cond      = std::make_unique<Expr>();
    cond->kind     = ExprKind::Boolean;
    cond->text     = "true";
    auto body      = std::make_unique<Expr>();
    body->kind     = ExprKind::Unit;
    whileExpr.children.push_back(std::move(cond));
    whileExpr.children.push_back(std::move(body));

    auto lowered = lowerExpr(whileExpr);
    ASSERT_TRUE(lowered.has_value());
    EXPECT_TRUE(areEquivalent(whileExpr, **lowered));
  }

  // Block with Let statement
  {
    Expr blockExpr;
    blockExpr.kind = ExprKind::Block;

    auto letExpr        = std::make_unique<Expr>();
    letExpr->kind       = ExprKind::Let;
    letExpr->text       = "foo";
    letExpr->annotation = Type{"Int", {}};
    auto val            = std::make_unique<Expr>();
    val->kind           = ExprKind::Integer;
    val->text           = "10";
    letExpr->children.push_back(std::move(val));
    blockExpr.children.push_back(std::move(letExpr));

    auto id  = std::make_unique<Expr>();
    id->kind = ExprKind::Identifier;
    id->text = "foo";
    blockExpr.children.push_back(std::move(id));

    auto lowered = lowerExpr(blockExpr);
    ASSERT_TRUE(lowered.has_value());
    EXPECT_TRUE(areEquivalent(blockExpr, **lowered));
    auto lifted = liftExpr(**lowered);
    ASSERT_TRUE(lifted.has_value());
    EXPECT_EQ((*lifted)->children.size(), 2U);
    EXPECT_EQ((*lifted)->children[0]->kind, ExprKind::Let);
    EXPECT_EQ((*lifted)->children[0]->text, "foo");
  }

  // Simple actions: return, yield, consume, fail, terminate
  for (ExprKind kind : {ExprKind::Return, ExprKind::Yield, ExprKind::Consume,
                        ExprKind::Fail, ExprKind::Terminate}) {
    Expr act;
    act.kind     = kind;
    auto lowered = lowerExpr(act);
    ASSERT_TRUE(lowered.has_value());
    auto lifted = liftExpr(**lowered);
    ASSERT_TRUE(lifted.has_value());
    EXPECT_EQ((*lifted)->kind, kind);
  }
}

TEST(VagentAstConsolidationTest, TopLevelDeclarationsRoundtrip) {
  // Event
  {
    Decl eventDecl;
    eventDecl.kind   = DeclKind::Event;
    eventDecl.name   = "MyEvent";
    eventDecl.target = "BaseEvent";
    eventDecl.parameters.push_back(Parameter{
        .name = "id", .type = Type{"Int", {}}, .key = false, .loc = {}});
    eventDecl.parameters.push_back(Parameter{
        .name = "label", .type = Type{"Str", {}}, .key = false, .loc = {}});

    auto lowered = lowerTopDecl(eventDecl);
    ASSERT_TRUE(lowered.has_value());
    EXPECT_TRUE(std::holds_alternative<runtime::EventDecl>(*lowered));
    EXPECT_TRUE(areEquivalent(eventDecl, *lowered));

    auto lifted = liftTopDecl(*lowered);
    ASSERT_TRUE(lifted.has_value());
    EXPECT_EQ(lifted->name, "MyEvent");
    EXPECT_EQ(lifted->target, "BaseEvent");
    EXPECT_EQ(lifted->parameters.size(), 2U);
  }

  // Predicate
  {
    Decl predDecl;
    predDecl.kind = DeclKind::Predicate;
    predDecl.name = "sensorReading";
    predDecl.parameters.push_back(Parameter{
        .name = "sensor", .type = Type{"Str", {}}, .key = true, .loc = {}});
    predDecl.parameters.push_back(Parameter{
        .name = "val", .type = Type{"Int", {}}, .key = false, .loc = {}});

    auto lowered = lowerTopDecl(predDecl);
    ASSERT_TRUE(lowered.has_value());
    EXPECT_TRUE(std::holds_alternative<runtime::PredicateDecl>(*lowered));
    EXPECT_TRUE(areEquivalent(predDecl, *lowered));

    auto lifted = liftTopDecl(*lowered);
    ASSERT_TRUE(lifted.has_value());
    EXPECT_EQ(lifted->name, "sensorReading");
    ASSERT_EQ(lifted->parameters.size(), 2U);
    EXPECT_TRUE(lifted->parameters[0].key);
    EXPECT_FALSE(lifted->parameters[1].key);
  }

  // Capacity with method signature and contracts
  {
    Decl capDecl;
    capDecl.kind = DeclKind::Capacity;
    capDecl.name = "Motor";
    Decl methodDecl;
    methodDecl.kind   = DeclKind::Method;
    methodDecl.name   = "setSpeed";
    methodDecl.effect = true;
    methodDecl.parameters.push_back(Parameter{
        .name = "rpm", .type = Type{"Int", {}}, .key = false, .loc = {}});
    methodDecl.result = Type{"()", {}};
    capDecl.members.push_back(std::move(methodDecl));

    auto lowered = lowerTopDecl(capDecl);
    ASSERT_TRUE(lowered.has_value());
    EXPECT_TRUE(std::holds_alternative<runtime::CapacityDecl>(*lowered));
    EXPECT_TRUE(areEquivalent(capDecl, *lowered));
  }

  // Goal and Plan
  {
    Decl goalDecl;
    goalDecl.kind     = DeclKind::Goal;
    goalDecl.name     = "ReachTarget";
    goalDecl.goalKind = GoalKind::Achieve;
    goalDecl.parameters.push_back(Parameter{
        .name = "target", .type = Type{"Str", {}}, .key = false, .loc = {}});

    auto loweredGoal = lowerTopDecl(goalDecl);
    ASSERT_TRUE(loweredGoal.has_value());
    EXPECT_TRUE(std::holds_alternative<runtime::GoalDecl>(*loweredGoal));
    EXPECT_TRUE(areEquivalent(goalDecl, *loweredGoal));

    Decl planDecl;
    planDecl.kind   = DeclKind::Plan;
    planDecl.name   = "reachPlan";
    planDecl.target = "ReachTarget";
    planDecl.names  = {"target"};
    auto body       = std::make_unique<Expr>();
    body->kind      = ExprKind::Unit;
    planDecl.body   = std::move(body);

    auto loweredPlan = lowerTopDecl(planDecl);
    ASSERT_TRUE(loweredPlan.has_value());
    EXPECT_TRUE(std::holds_alternative<runtime::PlanDecl>(*loweredPlan));
    EXPECT_TRUE(areEquivalent(planDecl, *loweredPlan));
  }
}

TEST(VagentAstConsolidationTest,
     AllTwentyThreeDesignExamplesLowerAndRoundtrip) {
  for (int index = 1; index <= 23; ++index) {
    std::ostringstream path;
    path << "tests/samples/vagent/mini_" << std::setw(2) << std::setfill('0')
         << index << ".vagent";
    SCOPED_TRACE(path.str());

    std::ifstream input(path.str());
    ASSERT_TRUE(input.is_open());
    const std::string source{std::istreambuf_iterator<char>(input), {}};

    // 1. Frontend parse
    Parser parser(source);
    auto program = parser.parseProgram();
    ASSERT_TRUE(program.has_value()) << formatDiagnostic(program.error());

    // 2. Frontend check
    auto checkResult = check(*program);
    ASSERT_TRUE(checkResult.has_value());

    // 3. Lower to runtime::Unit
    auto unit = lowerProgramToUnit(*program);
    ASSERT_TRUE(unit.has_value()) << formatDiagnostic(unit.error());

    // 4. Verify semantic equivalence
    EXPECT_TRUE(areEquivalent(*program, *unit));

    // 5. Lift back to frontend Program and verify roundtrip
    auto lifted = liftUnitToProgram(*unit);
    ASSERT_TRUE(lifted.has_value()) << formatDiagnostic(lifted.error());
    EXPECT_EQ(lifted->declarations.size(), program->declarations.size());
    EXPECT_EQ(lifted->imports.size(), program->imports.size());
    EXPECT_EQ(lifted->module.has_value(), program->module.has_value());
  }
}

TEST(VagentAstConsolidationTest, CompileProgramAgentIntoVortexGraphs) {
  const std::string source = R"(
    event Add { amount: Int }

    agent Counter {
      var count: Int := 0
      on Add as a {
        count := count + a.amount
      }
    }
  )";

  Parser parser(source);
  auto program = parser.parseProgram();
  ASSERT_TRUE(program.has_value()) << formatDiagnostic(program.error());

  auto checked = check(*program);
  ASSERT_TRUE(checked.has_value());

  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  auto compiled = compileProgramAgent(core, *program, "Counter");
  ASSERT_TRUE(compiled.has_value()) << compiled.error().detail;
  EXPECT_NE(compiled->agentCell, zigzag::CellRef{});
  EXPECT_NE(compiled->initEntry, zigzag::CellRef{});
  EXPECT_NE(compiled->dispatchEntry, zigzag::CellRef{});
}

TEST(VagentAstConsolidationTest, UnsupportedOrMalformedErrorsPropagateCleanly) {
  Expr malformed;
  malformed.kind = ExprKind::Binary;
  malformed.text = "+";
  // missing children
  auto lowered = lowerExpr(malformed);
  EXPECT_FALSE(lowered.has_value());
  EXPECT_EQ(lowered.error().code, DiagnosticCode::Syntax);
}

} // namespace
