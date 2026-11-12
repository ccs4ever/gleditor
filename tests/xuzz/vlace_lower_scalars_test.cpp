/**
 * @file vlace_lower_scalars_test.cpp
 * @brief Card L1: scalars lowered to Vortex graphs and run on the VM -- every
 *        literal kind, operator, short-circuit and `if`, read-back and render,
 *        the compile-time and run-time error paths, and the budget path.
 */
#include <gtest/gtest.h>

#include <optional>
#include <stdexcept>
#include <string>

#include "common/xanadu/vlace/compiler.hpp"
#include "common/xanadu/vlace/driver.hpp"
#include "common/xanadu/vlace/lower_cards.hpp"
#include "common/xanadu/vlace/parser.hpp"
#include "common/xanadu/vlace/run_vm.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {

using namespace xanadu::vlace;

struct Rig {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  zigzag::vortex::VortexVM vm{core};
  Compiler compiler{core, vm};

  std::expected<ValuePtr, EvalError> eval(std::string_view src) {
    Parser parser(src);
    auto prog = parser.parseProgram();
    EXPECT_TRUE(prog.has_value()) << src;
    if (!prog) return std::unexpected(EvalError{});
    return compiler.evaluateProgram(*prog);
  }
  std::string show(std::string_view src) {
    auto v = eval(src);
    return v ? Compiler::show(*v) : "ERR:" + v.error().detail;
  }
};

TEST(VlaceLowerScalarsTest, StringThatLooksNumericStaysAString) {
  Rig rig;
  auto v = rig.eval("\"3\" ++ \"4\"");
  ASSERT_TRUE(v.has_value());
  EXPECT_EQ((*v)->kind, Value::Kind::Str);
  EXPECT_EQ((*v)->s, "34");
  EXPECT_EQ(rig.show("\"42\""), "42");
  auto lone = rig.eval("\"42\"");
  ASSERT_TRUE(lone.has_value());
  EXPECT_EQ((*lone)->kind, Value::Kind::Str);
  auto compared = rig.eval("\"3\" == \"3\"");
  ASSERT_TRUE(compared.has_value());
  EXPECT_EQ((*compared)->kind, Value::Kind::Bool);
  EXPECT_TRUE((*compared)->b);
  auto branch = rig.eval("if True then \"7\" else \"8\"");
  ASSERT_TRUE(branch.has_value());
  EXPECT_EQ((*branch)->kind, Value::Kind::Str);
  EXPECT_EQ((*branch)->s, "7");
}

TEST(VlaceLowerScalarsTest, LiteralsKeepTheirKind) {
  Rig rig;
  const auto kindOf = [&](std::string_view src) {
    auto v = rig.eval(src);
    EXPECT_TRUE(v.has_value()) << src;
    return v ? (*v)->kind : Value::Kind::Closure;
  };
  EXPECT_EQ(kindOf("7"), Value::Kind::Int);
  EXPECT_EQ(kindOf("7.5"), Value::Kind::Float);
  EXPECT_EQ(kindOf("\"seven\""), Value::Kind::Str);
  EXPECT_EQ(kindOf("True"), Value::Kind::Bool);
  EXPECT_EQ(kindOf("False"), Value::Kind::Bool);
  EXPECT_EQ(kindOf("'a'"), Value::Kind::Int); // a Char is its code point
  EXPECT_EQ(rig.show("'a'"), "97");
  EXPECT_EQ(rig.show("7.5"), "7.5");
  EXPECT_EQ(rig.show("\"hi there\""), "hi there");
}

TEST(VlaceLowerScalarsTest, ValuesRememberTheirCell) {
  Rig rig;
  auto v = rig.eval("1 + 2");
  ASSERT_TRUE(v.has_value());
  ASSERT_TRUE((*v)->cell.has_value());
  EXPECT_EQ((*v)->i, 3);
  EXPECT_EQ(rig.compiler.render(*v), "3");
}

TEST(VlaceLowerScalarsTest, IntegerAndFloatArithmetic) {
  Rig rig;
  EXPECT_EQ(rig.show("7 / 2"), "3"); // integer division
  EXPECT_EQ(rig.show("7.0 / 2.0"), "3.5");
  EXPECT_EQ(rig.show("7 % 4"), "3");
  EXPECT_EQ(rig.show("2.5 * 2.0"), "5");
  EXPECT_EQ(rig.show("1.5 - 0.25"), "1.25");
  EXPECT_EQ(rig.show("(1 + 2) * (3 + 4)"), "21");
}

TEST(VlaceLowerScalarsTest, ComparisonsYieldBooleans) {
  Rig rig;
  EXPECT_EQ(rig.show("3 <= 3"), "True");
  EXPECT_EQ(rig.show("3 > 3"), "False");
  EXPECT_EQ(rig.show("3 >= 4"), "False");
  EXPECT_EQ(rig.show("3 /= 4"), "True");
  EXPECT_EQ(rig.show("2.5 < 3.5"), "True");
  EXPECT_EQ(rig.show("\"a\" == \"a\""), "True");
  EXPECT_EQ(rig.show("\"a\" /= \"a\""), "False");
}

TEST(VlaceLowerScalarsTest, StringConcatenation) {
  Rig rig;
  EXPECT_EQ(rig.show("\"a\" ++ \"b\" ++ \"c\""), "abc");
  EXPECT_EQ(rig.show("\"\" ++ \"x\""), "x");
}

// Comparisons are parenthesised: the parser stops at a non-associative
// operator and drops a lower-precedence tail (`1 > 2 || 3 < 4` parses as
// `1 > 2`), which is not this card's to fix.
TEST(VlaceLowerScalarsTest, BooleanTruthTables) {
  Rig rig;
  EXPECT_EQ(rig.show("True && True"), "True");
  EXPECT_EQ(rig.show("False && True"), "False");
  EXPECT_EQ(rig.show("False || False"), "False");
  EXPECT_EQ(rig.show("False || True"), "True");
  EXPECT_EQ(rig.show("(1 < 2) && (2 < 3) && (3 < 4)"), "True");
  EXPECT_EQ(rig.show("(1 > 2) || (2 > 3) || (3 < 4)"), "True");
}

// The right operand of a decided && / || must not be reached: a run-time
// failure there (a non-exhaustive if has none, so use the VM's overflow
// refusal) would otherwise surface.
TEST(VlaceLowerScalarsTest, ShortCircuitSkipsTheRightOperand) {
  Rig rig;
  const std::string overflow = "9223372036854775807 + 1 > 0";
  EXPECT_EQ(rig.show("False && (" + overflow + ")"), "False");
  EXPECT_EQ(rig.show("True || (" + overflow + ")"), "True");
  const auto reached = rig.eval("True && (" + overflow + ")");
  ASSERT_FALSE(reached.has_value());
  EXPECT_NE(reached.error().detail.find("overflow"), std::string::npos);
}

TEST(VlaceLowerScalarsTest, IfEvaluatesOnlyTheChosenArm) {
  Rig rig;
  const std::string overflow = "(9223372036854775807 + 1)";
  EXPECT_EQ(rig.show("if True then 1 else " + overflow), "1");
  EXPECT_EQ(rig.show("if False then " + overflow + " else 2"), "2");
  EXPECT_EQ(rig.show("if 1 < 2 then \"yes\" else \"no\""), "yes");
  EXPECT_EQ(rig.show("if True then 1.5 else 2.5"), "1.5");
  EXPECT_EQ(rig.show("if 2 > 1 then (if 1 > 2 then 1 else 2) else 3"), "2");
  EXPECT_EQ(rig.show("(if True then 10 else 20) + (if False then 1 else 2)"),
            "12");
}

TEST(VlaceLowerScalarsTest, ProgramWithoutResultIsZero) {
  Rig rig;
  EXPECT_EQ(rig.show(""), "0");
}

TEST(VlaceLowerScalarsTest, UnboundNamesFailAtCompileTime) {
  Rig rig;
  const auto before = rig.vm.totalCycles();
  auto v            = rig.eval("1 + y");
  ASSERT_FALSE(v.has_value());
  EXPECT_EQ(v.error().code, CompileError::UnboundIdentifier);
  EXPECT_EQ(v.error().detail, "unbound identifier 'y'");
  EXPECT_EQ(rig.vm.totalCycles(), before) << "nothing ran";
}

TEST(VlaceLowerScalarsTest, RuntimeOverflowIsAnEvalFailure) {
  Rig rig;
  auto v = rig.eval("9223372036854775807 + 1");
  ASSERT_FALSE(v.has_value());
  EXPECT_NE(v.error().detail.find("overflow"), std::string::npos);
}

TEST(VlaceLowerScalarsTest, RunningAdvancesTheVmCycleCounter) {
  Rig rig;
  const auto before = rig.vm.totalCycles();
  EXPECT_EQ(rig.show("1 + 2"), "3");
  EXPECT_GT(rig.vm.totalCycles(), before);
  EXPECT_EQ(rig.vm.frameLimits().maxDepth, lower::evaluationNestingBudget);
}

TEST(VlaceLowerScalarsTest, CycleLimitFailsAsBudget) {
  Rig rig;
  lower::setCycleLimit(rig.compiler, 1);
  auto v = rig.eval("1 + 2 * 3 + 4");
  ASSERT_FALSE(v.has_value());
  EXPECT_EQ(v.error().code, CompileError::BudgetExhausted);
  EXPECT_NE(v.error().detail.find("budget"), std::string::npos);
  lower::setCycleLimit(rig.compiler, std::nullopt);
  EXPECT_EQ(rig.show("1 + 2 * 3 + 4"), "11");
}

TEST(VlaceLowerScalarsTest, ArenaWorkBudgetFailsAsBudget) {
  Rig rig;
  rig.arena.setExecutionLimits(1'000'000, 0, 0);
  auto v = rig.eval("1 + 2");
  ASSERT_FALSE(v.has_value());
  EXPECT_EQ(v.error().code, CompileError::BudgetExhausted);
  EXPECT_NE(v.error().detail.find("budget"), std::string::npos);
}

TEST(VlaceLowerScalarsTest, ExpressionEntryEvaluatesToo) {
  Rig rig;
  Parser parser("2 * 21");
  auto prog = parser.parseProgram();
  ASSERT_TRUE(prog.has_value());
  auto v = rig.compiler.evaluate(*prog->result);
  ASSERT_TRUE(v.has_value());
  EXPECT_EQ(Compiler::show(*v), "42");
}

TEST(VlaceLowerScalarsTest, RenderHandlesScalarsAndNull) {
  Rig rig;
  auto v = rig.eval("\"s\"");
  ASSERT_TRUE(v.has_value());
  EXPECT_EQ(rig.compiler.render(*v, 5), "s");
  EXPECT_EQ(rig.compiler.render(*v, 0), "...");
  EXPECT_THROW((void)rig.compiler.render({}, 1), std::logic_error);
}

TEST(VlaceLowerScalarsTest, DriverNamesThePhaseThatFailed) {
  Driver driver;
  auto ok = driver.run("if 1 < 2 then \"a\" ++ \"b\" else \"c\"");
  ASSERT_TRUE(ok.has_value());
  EXPECT_EQ(ok->type, "String");
  EXPECT_EQ(ok->value, "ab");
  driver.withTypeChecking(false);
  auto runtime = driver.run("9223372036854775807 + 1");
  ASSERT_FALSE(runtime.has_value());
  EXPECT_EQ(runtime.error().phase, "eval");
  auto unbound = driver.run("zz");
  ASSERT_FALSE(unbound.has_value());
  EXPECT_EQ(unbound.error().phase, "compile");
}

TEST(VlaceLowerScalarsTest, ConstructsOwnedByLaterCardsStillRefuse) {
  Rig rig;
  auto v = rig.eval("(fn x => x) 1");
  ASSERT_FALSE(v.has_value());
  EXPECT_NE(v.error().detail.find("VM lowering is not implemented"),
            std::string::npos);
}

} // namespace
