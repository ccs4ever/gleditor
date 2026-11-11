/**
 * @file vlace_lower_context_test.cpp
 * @brief The shared lowering helpers (card L0): scopes, the constructor and
 *        record-field table, the run-time failure encoding, and budget
 *        rewording. Card-specific lowering is tested by its own card.
 */
#include <gtest/gtest.h>

#include "common/xanadu/vlace/compiler.hpp"
#include "common/xanadu/vlace/lower_context.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {

using namespace xanadu::vlace;
using namespace xanadu::vlace::lower;

struct Rig {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  zigzag::vortex::VortexVM vm{core};
  Compiler compiler{core, vm};
};

TEST(VlaceLowerContextTest, FailureEncodingRoundTrips) {
  const auto text = encodeRuntimeFailure(CompileError::NonExhaustiveMatch,
                                         SourceLocation{.line = 3, .column = 7},
                                         "no case | alternative matched");
  const auto err  = decodeRuntimeFailure(text);
  ASSERT_TRUE(err.has_value());
  EXPECT_EQ(err->code, CompileError::NonExhaustiveMatch);
  EXPECT_EQ(err->detail, "no case | alternative matched");
  EXPECT_EQ(err->loc.line, 3U);
  EXPECT_EQ(err->loc.column, 7U);
}

TEST(VlaceLowerContextTest, ForeignMessagesAreNotDecoded) {
  EXPECT_FALSE(decodeRuntimeFailure("Assertion failed").has_value());
  EXPECT_FALSE(decodeRuntimeFailure("VLACE|Nonsense|1:1|x").has_value());
  EXPECT_FALSE(decodeRuntimeFailure("VLACE|TypeMismatch|x:y|x").has_value());
}

TEST(VlaceLowerContextTest, BudgetTextsBecomeBudgetExhausted) {
  for (const char *vmText :
       {"activation frame depth limit exceeded", "Exceeded max cycles",
        "Vortex evaluation work budget exhausted"}) {
    const auto err = rewordBudgetFailure(vmText);
    ASSERT_TRUE(err.has_value()) << vmText;
    EXPECT_EQ(err->code, CompileError::BudgetExhausted);
    EXPECT_NE(err->detail.find("budget"), std::string::npos);
  }
  EXPECT_FALSE(rewordBudgetFailure("Assertion failed").has_value());
  EXPECT_EQ(compileErrorName(CompileError::BudgetExhausted), "BudgetExhausted");
}

TEST(VlaceLowerContextTest, EmittedFailureRunsOnTheVmAndDecodes) {
  Rig rig;
  LowerContext ctx(rig.compiler);
  ctx.emitRuntimeFailure(CompileError::NonExhaustiveMatch,
                         SourceLocation{.line = 2, .column = 5}, "boom");
  ASSERT_TRUE(ctx.block().entry.has_value());
  const auto cursor = rig.vm.spawnCursor(*ctx.block().entry, "vlace_test");
  const auto result = rig.vm.run(cursor);
  ASSERT_FALSE(result.success);
  const auto err = failureToEval(result);
  EXPECT_EQ(err.code, CompileError::NonExhaustiveMatch);
  EXPECT_EQ(err.detail, "boom");
  EXPECT_EQ(err.loc.line, 2U);
}

TEST(VlaceLowerContextTest, UnknownVmFailureIsNamedAsSuch) {
  zigzag::vortex::ExecutionResult r;
  r.success      = false;
  r.errorMessage = "something else";
  const auto err = failureToEval(r);
  EXPECT_EQ(err.code, CompileError::UnexpectedToken);
  EXPECT_EQ(err.detail, "vlace: VM failure: something else");
}

TEST(VlaceLowerContextTest, ScopesShadowAndCountFrames) {
  Rig rig;
  LowerContext ctx(rig.compiler);
  const auto outer = ctx.emitConstant(std::int64_t{1});
  ctx.bind("x", outer);
  {
    auto frame       = ctx.pushScope(ScopeKind::Frame);
    const auto inner = ctx.emitConstant(std::int64_t{2});
    ctx.bind("y", inner);
    const auto x = ctx.lookup("x");
    ASSERT_TRUE(x.has_value());
    EXPECT_EQ(x->operand.cell, outer.cell);
    EXPECT_EQ(x->frameDepth, 1U);
    auto block = ctx.pushScope();
    ctx.bind("x", inner);
    EXPECT_EQ(ctx.lookup("x")->operand.cell, inner.cell);
    EXPECT_EQ(ctx.lookup("x")->frameDepth, 0U);
  }
  EXPECT_FALSE(ctx.lookup("y").has_value());
  EXPECT_EQ(ctx.lookup("x")->operand.cell, outer.cell);
}

TEST(VlaceLowerContextTest, CtorTableTracksRecordFields) {
  CtorTable table;
  table.declare(CtorInfo{.name = "Point", .arity = 2, .fields = {"x", "y"}});
  table.declare(CtorInfo{.name = "Leaf", .arity = 0, .fields = {}});
  EXPECT_EQ(table.fieldOwner("y"), "Point");
  EXPECT_EQ(table.fieldIndex("Point", "y"), 1U);
  EXPECT_FALSE(table.fieldIndex("Point", "z").has_value());
  EXPECT_FALSE(table.fieldOwner("Leaf").has_value());
  ASSERT_TRUE(table.find("Leaf").has_value());
  EXPECT_EQ(table.find("Leaf")->arity, 0U);
}

TEST(VlaceLowerContextTest, CardStateIsPerCardAndPersistent) {
  Rig rig;
  struct Counter {
    int n{0};
  };
  rig.compiler.lowerState().cardState<Counter>(Card::Kernel).n = 4;
  EXPECT_EQ(rig.compiler.lowerState().cardState<Counter>(Card::Kernel).n, 4);
  EXPECT_EQ(rig.compiler.lowerState().cardState<Counter>(Card::Data).n, 0);
}

} // namespace
