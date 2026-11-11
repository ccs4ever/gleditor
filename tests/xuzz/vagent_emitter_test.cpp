#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "common/xanadu/vagent/inlang_emitter.hpp"
#include "common/xanadu/vagent/meta.hpp"
#include "common/xanadu/vagent/parser.hpp"
#include "common/xanadu/vagent/r1_ast.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {

using namespace xanadu::vagent;
using zigzag::CellRef;
using zigzag::noCell;
using zigzag::Posward;

struct EmitterHarness {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  zigzag::vortex::VortexVM vm{core};
  CellRef programRoot{noCell};
  CellRef agentCell{noCell};
  std::optional<MetaRuntime> meta;

  EmitterHarness() {
    programRoot = arena.makeCell();
    agentCell   = arena.makeCell();
    auto binds  = mintMetaBindSet(arena, core, programRoot, agentCell);
    EXPECT_TRUE(binds.has_value());
    if (binds) {
      meta.emplace(core, vm, *binds);
    }
  }

  MetaRuntime &rt() { return *meta; }
};

TEST(VAgentEmitterTest, EmitsArithmeticFragmentAndRuns) {
  EmitterHarness h;
  auto &arena      = h.arena;
  const auto &dims = h.core.dims();

  // Expression: (a + b) * 2
  auto parsedExpr = Parser("(a + b) * 2").parseExpression();
  ASSERT_TRUE(parsedExpr.has_value());
  auto r1Root = marshalAstToR1(arena, h.core, **parsedExpr);
  ASSERT_TRUE(r1Root.has_value());

  // Parameters: a: Int, b: Int
  const auto pA = arena.makeCell("a");
  zigzag::expectWritten(
      arena.link(pA, dims.values, Posward, arena.makeCell("Int")));
  const auto pB = arena.makeCell("b");
  zigzag::expectWritten(
      arena.link(pB, dims.values, Posward, arena.makeCell("Int")));
  zigzag::expectWritten(arena.link(pA, dims.step, Posward, pB));

  auto emitRes = runInlangEmitter(arena, h.core, h.vm, h.rt(), "calc:add_mul",
                                  *r1Root, pA);
  ASSERT_TRUE(emitRes.ok) << emitRes.error;
  EXPECT_TRUE(emitRes.head.has_value());

  std::ignore = h.rt().drainPendingRegisters();

  // Run fragment: a = 10, b = 5 -> (10 + 5) * 2 = 30
  auto outcome =
      h.rt().runFragment(*emitRes.head, {std::int64_t{10}, std::int64_t{5}});
  EXPECT_TRUE(outcome.produced);
  ASSERT_TRUE(outcome.result.has_value());
  EXPECT_EQ(std::get<std::int64_t>(*outcome.result), 30);
}

TEST(VAgentEmitterTest, EmitsComparisonLogicFragmentAndRuns) {
  EmitterHarness h;
  auto &arena      = h.arena;
  const auto &dims = h.core.dims();

  // Expression: a > b
  auto parsedExpr = Parser("a > b").parseExpression();
  ASSERT_TRUE(parsedExpr.has_value());
  auto r1Root = marshalAstToR1(arena, h.core, **parsedExpr);
  ASSERT_TRUE(r1Root.has_value());

  const auto pA = arena.makeCell("a");
  zigzag::expectWritten(
      arena.link(pA, dims.values, Posward, arena.makeCell("Int")));
  const auto pB = arena.makeCell("b");
  zigzag::expectWritten(
      arena.link(pB, dims.values, Posward, arena.makeCell("Int")));
  zigzag::expectWritten(arena.link(pA, dims.step, Posward, pB));

  auto emitRes =
      runInlangEmitter(arena, h.core, h.vm, h.rt(), "cmp:gt", *r1Root, pA);
  ASSERT_TRUE(emitRes.ok) << emitRes.error;

  std::ignore = h.rt().drainPendingRegisters();

  // Run fragment: a = 20, b = 10 -> true
  auto outcome =
      h.rt().runFragment(*emitRes.head, {std::int64_t{20}, std::int64_t{10}});
  EXPECT_TRUE(outcome.produced);
  ASSERT_TRUE(outcome.result.has_value());
  EXPECT_EQ(std::get<bool>(*outcome.result), true);
}

TEST(VAgentEmitterTest, EmitsUnaryNegationAndRuns) {
  EmitterHarness h;
  auto &arena      = h.arena;
  const auto &dims = h.core.dims();

  // Expression: -x
  auto parsedExpr = Parser("-x").parseExpression();
  ASSERT_TRUE(parsedExpr.has_value());
  auto r1Root = marshalAstToR1(arena, h.core, **parsedExpr);
  ASSERT_TRUE(r1Root.has_value());

  const auto pX = arena.makeCell("x");
  zigzag::expectWritten(
      arena.link(pX, dims.values, Posward, arena.makeCell("Int")));

  auto emitRes =
      runInlangEmitter(arena, h.core, h.vm, h.rt(), "math:neg", *r1Root, pX);
  ASSERT_TRUE(emitRes.ok) << emitRes.error;

  std::ignore = h.rt().drainPendingRegisters();

  // Run fragment: x = 42 -> -42
  auto outcome = h.rt().runFragment(*emitRes.head, {std::int64_t{42}});
  EXPECT_TRUE(outcome.produced);
  ASSERT_TRUE(outcome.result.has_value());
  EXPECT_EQ(std::get<std::int64_t>(*outcome.result), -42);
}

} // namespace
