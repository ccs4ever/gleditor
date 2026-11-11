#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "common/xanadu/vagent/inlang_checker.hpp"
#include "common/xanadu/vagent/meta.hpp"
#include "common/xanadu/vagent/parser.hpp"
#include "common/xanadu/vagent/r1_ast.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {

using namespace xanadu::vagent;
using zigzag::CellRef;
using zigzag::noCell;
using zigzag::Posward;

struct CheckerHarness {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  zigzag::vortex::VortexVM vm{core};
  CellRef programRoot{noCell};
  CellRef agentCell{noCell};
  std::optional<MetaRuntime> meta;

  CheckerHarness() {
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

TEST(VAgentInlangCheckerTest, ReservedParameterNamesRejected) {
  CheckerHarness h;
  auto &arena = h.arena;

  // R1 AST: literal 42
  auto parsedExpr = Parser("42").parseExpression();
  ASSERT_TRUE(parsedExpr.has_value());
  auto r1Root = marshalAstToR1(arena, h.core, **parsedExpr);
  ASSERT_TRUE(r1Root.has_value());

  // Parameter named "result"
  const auto pRes = arena.makeCell("result");
  auto checkRes =
      runInlangChecker(arena, h.core, h.vm, h.rt(), *r1Root, pRes, true);
  EXPECT_FALSE(checkRes.ok);
  EXPECT_TRUE(checkRes.error.find("reserved") != std::string::npos);
}

TEST(VAgentInlangCheckerTest, UnimportedStdMetaReservedIdentifiersRejected) {
  CheckerHarness h;
  auto &arena = h.arena;

  // R1 AST: identifier "make_cell"
  auto parsedExpr = Parser("make_cell").parseExpression();
  ASSERT_TRUE(parsedExpr.has_value());
  auto r1Root = marshalAstToR1(arena, h.core, **parsedExpr);
  ASSERT_TRUE(r1Root.has_value());

  // Without import std:meta, "make_cell" is refused
  auto checkRes = runInlangChecker(arena, h.core, h.vm, h.rt(), *r1Root,
                                   std::nullopt, false);
  EXPECT_FALSE(checkRes.ok);
}

TEST(VAgentInlangCheckerTest, PureClosedScalarArithmeticInfersAndAnnotates) {
  CheckerHarness h;
  auto &arena      = h.arena;
  const auto &dims = h.core.dims();

  // R1 AST: a + b
  auto parsedExpr = Parser("a + b").parseExpression();
  ASSERT_TRUE(parsedExpr.has_value());
  auto r1Root = marshalAstToR1(arena, h.core, **parsedExpr);
  ASSERT_TRUE(r1Root.has_value());

  // Params: a: Int, b: Int
  const auto pA = arena.makeCell("a");
  zigzag::expectWritten(
      arena.link(pA, dims.values, Posward, arena.makeCell("Int")));
  const auto pB = arena.makeCell("b");
  zigzag::expectWritten(
      arena.link(pB, dims.values, Posward, arena.makeCell("Int")));
  zigzag::expectWritten(arena.link(pA, dims.step, Posward, pB));

  auto checkRes =
      runInlangChecker(arena, h.core, h.vm, h.rt(), *r1Root, pA, true);
  EXPECT_TRUE(checkRes.ok) << checkRes.error;
  EXPECT_EQ(checkRes.resultType, "Int");
  EXPECT_TRUE(checkRes.pure);
  EXPECT_TRUE(checkRes.closed);
  EXPECT_EQ(checkRes.arity, 2u);

  // Check annotation under +d.values
  const auto val = arena.linked(*r1Root, dims.values, Posward);
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(arena.textOf(*val), "Int");
}

TEST(VAgentInlangCheckerTest, UnresolvedVariableCapturesAndFlagsNotClosed) {
  CheckerHarness h;
  auto &arena      = h.arena;
  const auto &dims = h.core.dims();

  // R1 AST: a + y (y is free/outer)
  auto parsedExpr = Parser("a + y").parseExpression();
  ASSERT_TRUE(parsedExpr.has_value());
  auto r1Root = marshalAstToR1(arena, h.core, **parsedExpr);
  ASSERT_TRUE(r1Root.has_value());

  // Params: a: Int (y is missing)
  const auto pA = arena.makeCell("a");
  zigzag::expectWritten(
      arena.link(pA, dims.values, Posward, arena.makeCell("Int")));

  auto checkRes =
      runInlangChecker(arena, h.core, h.vm, h.rt(), *r1Root, pA, true);
  EXPECT_TRUE(checkRes.ok) << checkRes.error;
  EXPECT_FALSE(checkRes.closed); // not closed because of capture
}

TEST(VAgentInlangCheckerTest, ImpureCallMarksNotPure) {
  CheckerHarness h;
  auto &arena = h.arena;

  // R1 AST: make_cell(1)
  auto parsedExpr = Parser("make_cell(1)").parseExpression();
  ASSERT_TRUE(parsedExpr.has_value());
  auto r1Root = marshalAstToR1(arena, h.core, **parsedExpr);
  ASSERT_TRUE(r1Root.has_value());

  auto checkRes = runInlangChecker(arena, h.core, h.vm, h.rt(), *r1Root,
                                   std::nullopt, true);
  EXPECT_TRUE(checkRes.ok) << checkRes.error;
  EXPECT_FALSE(checkRes.pure); // make_cell is impure
}

} // namespace
