/**
 * @file vlace_lower_context_test.cpp
 * @brief The shared lowering helpers (card L0): scopes, the constructor and
 *        record-field table, the run-time failure encoding, and budget
 *        rewording. Card-specific lowering is tested by its own card.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <stdexcept>

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

TEST(VlaceLowerContextTest, CardStateWithAnotherTypeIsRefused) {
  Rig rig;
  struct A {
    int n{0};
  };
  struct B {
    int n{0};
  };
  static_cast<void>(rig.compiler.lowerState().cardState<A>(Card::Io));
  EXPECT_THROW(
      static_cast<void>(rig.compiler.lowerState().cardState<B>(Card::Io)),
      std::logic_error);
}

TEST(VlaceLowerContextTest, LabelsSkipForwardAndAreReusableBackward) {
  Rig rig;
  LowerContext ctx(rig.compiler);
  const auto skip = ctx.mintLabel("#skip");
  ctx.jumpTo(skip);
  ctx.emitRuntimeFailure(CompileError::NonExhaustiveMatch, {},
                         "must be skipped");
  ctx.placeLabel(skip);
  ctx.emitOp(zigzag::vortex::OpcodeKind::Nop, "#end");
  const auto cursor = rig.vm.spawnCursor(*ctx.block().entry, "vlace_test");
  const auto result = rig.vm.run(cursor);
  EXPECT_TRUE(result.success) << result.errorMessage;
}

TEST(VlaceLowerContextTest, BranchIfFalseTakesTheBranchOnlyWhenFalse) {
  Rig rig;
  LowerContext ctx(rig.compiler);
  const auto otherwise = ctx.mintLabel("#else");
  ctx.branchIfFalse(ctx.emitConstant(zigzag::vortex::CellValue{true}),
                    otherwise);
  ctx.emitRuntimeFailure(CompileError::NonExhaustiveMatch, {}, "fell through");
  ctx.placeLabel(otherwise);
  const auto cursor = rig.vm.spawnCursor(*ctx.block().entry, "vlace_test");
  const auto result = rig.vm.run(cursor);
  ASSERT_FALSE(result.success);
  EXPECT_EQ(failureToEval(result).detail, "fell through");
}

TEST(VlaceLowerContextTest, BlockRecordsRegistersAndOutputsAsLocals) {
  Rig rig;
  LowerContext ctx(rig.compiler);
  const auto reg = ctx.newRegister();
  const auto out = ctx.newRegister();
  const auto op  = ctx.emitOp(zigzag::vortex::OpcodeKind::Nop);
  ctx.emitOutput(op, out.cell);
  const auto constant = ctx.emitConstant(std::int64_t{3});
  const auto old      = ctx.swapBlock(Block{});
  EXPECT_NE(std::ranges::find(old.locals, reg.cell), old.locals.end());
  EXPECT_NE(std::ranges::find(old.locals, out.cell), old.locals.end());
  EXPECT_EQ(std::ranges::find(old.locals, constant.cell), old.locals.end());
  EXPECT_TRUE(ctx.block().locals.empty());
}

TEST(VlaceLowerContextTest, CapturesAreListedByTheEnclosingFrame) {
  Rig rig;
  LowerContext ctx(rig.compiler);
  const auto outer = ctx.emitConstant(std::int64_t{1});
  ctx.bind("x", outer);
  auto frame       = ctx.pushScope(ScopeKind::Frame);
  auto inner       = ctx.pushScope();
  const auto local = ctx.newRegister();
  ctx.addCapture("x", outer, local);
  ASSERT_EQ(ctx.captures().size(), 1U);
  EXPECT_EQ(ctx.captures()[0].name, "x");
  EXPECT_EQ(ctx.captures()[0].outer.cell, outer.cell);
  EXPECT_EQ(ctx.lookup("x")->frameDepth, 0U);
  EXPECT_EQ(ctx.lookup("x")->operand.cell, local.cell);
}

TEST(VlaceLowerContextTest, KernelDimensionsAreMintedOnceByName) {
  Rig rig;
  auto &state       = rig.compiler.lowerState();
  const auto &first = state.kernelDims(rig.core);
  const auto thunk  = first.thunk;
  const auto &again = state.kernelDims(rig.core);
  EXPECT_EQ(again.thunk, thunk);
  EXPECT_NE(first.memo, first.blackhole);
  EXPECT_NE(first.thunk, first.memo);
}

TEST(VlaceLowerContextTest, MintingPastTheBudgetIsReportedAsBudgetExhausted) {
  Rig rig;
  LowerContext ctx(rig.compiler);
  try {
    for (std::size_t i = 0; i <= maxGeneratedCells; ++i) {
      static_cast<void>(ctx.newRegister());
    }
    FAIL() << "the cell budget did not trip";
  } catch (const LoweringBudgetExceeded &budget) {
    EXPECT_EQ(budget.error().code, CompileError::BudgetExhausted);
    EXPECT_NE(budget.error().detail.find("budget"), std::string::npos);
  }
}

} // namespace
