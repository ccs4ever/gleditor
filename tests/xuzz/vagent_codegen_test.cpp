#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "common/xanadu/vagent/inlang_checker.hpp"
#include "common/xanadu/vagent/inlang_emitter.hpp"
#include "common/xanadu/vagent/inlang_parser.hpp"
#include "common/xanadu/vagent/meta.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {

using namespace xanadu::vagent;
using zigzag::CellRef;
using zigzag::noCell;
using zigzag::Posward;

struct CodegenHarness {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  zigzag::vortex::VortexVM vm{core};
  CellRef programRoot{noCell};
  CellRef agentCell{noCell};
  std::optional<MetaRuntime> meta;

  CodegenHarness() {
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

TEST(VAgentCodegenTest, LowerArithmeticExpressionToExecutableFragment) {
  CodegenHarness h;
  auto &arena      = h.arena;
  const auto &dims = h.core.dims();

  // (x + 10) * (y - 2)
  const std::string exprSource = "(x + 10) * (y - 2)";
  auto r1Root                  = parseSourceToR1(arena, h.core, exprSource);
  ASSERT_TRUE(r1Root.has_value()) << r1Root.error();

  const auto pX = arena.makeCell("x");
  zigzag::expectWritten(
      arena.link(pX, dims.values, Posward, arena.makeCell("Int")));
  const auto pY = arena.makeCell("y");
  zigzag::expectWritten(
      arena.link(pY, dims.values, Posward, arena.makeCell("Int")));
  zigzag::expectWritten(arena.link(pX, dims.step, Posward, pY));

  auto checkRes =
      runInlangChecker(arena, h.core, h.vm, h.rt(), *r1Root, pX, true);
  ASSERT_TRUE(checkRes.ok) << checkRes.error;
  EXPECT_TRUE(checkRes.pure);
  EXPECT_TRUE(checkRes.closed);
  EXPECT_EQ(checkRes.arity, 2u);

  auto emitRes =
      runInlangEmitter(arena, h.core, h.vm, h.rt(), "test:calc", *r1Root, pX);
  ASSERT_TRUE(emitRes.ok) << emitRes.error;
  ASSERT_TRUE(emitRes.head.has_value());

  std::ignore = h.rt().drainPendingRegisters();

  // x = 5, y = 8 -> (5 + 10) * (8 - 2) = 15 * 6 = 90
  auto outcome =
      h.rt().runFragment(*emitRes.head, {std::int64_t{5}, std::int64_t{8}});
  EXPECT_TRUE(outcome.produced);
  ASSERT_TRUE(outcome.result.has_value());
  EXPECT_EQ(std::get<std::int64_t>(*outcome.result), 90);
}

TEST(VAgentCodegenTest, LowerRelationalLogicToExecutableFragment) {
  CodegenHarness h;
  auto &arena      = h.arena;
  const auto &dims = h.core.dims();

  // (a >= b) & (b != 0)
  const std::string exprSource = "(a >= b) & (b != 0)";
  auto r1Root                  = parseSourceToR1(arena, h.core, exprSource);
  ASSERT_TRUE(r1Root.has_value()) << r1Root.error();

  const auto pA = arena.makeCell("a");
  zigzag::expectWritten(
      arena.link(pA, dims.values, Posward, arena.makeCell("Int")));
  const auto pB = arena.makeCell("b");
  zigzag::expectWritten(
      arena.link(pB, dims.values, Posward, arena.makeCell("Int")));
  zigzag::expectWritten(arena.link(pA, dims.step, Posward, pB));

  auto checkRes =
      runInlangChecker(arena, h.core, h.vm, h.rt(), *r1Root, pA, true);
  ASSERT_TRUE(checkRes.ok) << checkRes.error;

  auto emitRes =
      runInlangEmitter(arena, h.core, h.vm, h.rt(), "test:logic", *r1Root, pA);
  ASSERT_TRUE(emitRes.ok) << emitRes.error;
  ASSERT_TRUE(emitRes.head.has_value());

  std::ignore = h.rt().drainPendingRegisters();

  // Case 1: a = 10, b = 5 -> true && true -> true
  auto outcome1 =
      h.rt().runFragment(*emitRes.head, {std::int64_t{10}, std::int64_t{5}});
  EXPECT_TRUE(outcome1.produced);
  ASSERT_TRUE(outcome1.result.has_value());
  EXPECT_EQ(std::get<bool>(*outcome1.result), true);

  // Case 2: a = 2, b = 5 -> false && true -> false
  auto outcome2 =
      h.rt().runFragment(*emitRes.head, {std::int64_t{2}, std::int64_t{5}});
  EXPECT_TRUE(outcome2.produced);
  ASSERT_TRUE(outcome2.result.has_value());
  EXPECT_EQ(std::get<bool>(*outcome2.result), false);
}

} // namespace
