#include <gtest/gtest.h>
#include <string_view>

#include "common/xanadu/vagent/inlang_checker.hpp"
#include "common/xanadu/vagent/inlang_parser.hpp"
#include "common/xanadu/vagent/meta.hpp"
#include "common/xanadu/vagent/r1_ast.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {

using namespace xanadu::vagent;

TEST(VAgentSemanticsTest, TypeCheckModuleAndInfersTypes) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  zigzag::vortex::VortexVM vm(core);
  const auto programRoot = arena.makeCell();
  const auto agentCell   = arena.makeCell();
  auto binds             = mintMetaBindSet(arena, core, programRoot, agentCell);
  ASSERT_TRUE(binds.has_value());
  MetaRuntime meta(core, vm, *binds);

  // Expression: x + 10
  auto r1Expr = parseSourceToR1(arena, core, "x + 10");
  ASSERT_TRUE(r1Expr.has_value()) << r1Expr.error();

  const auto pX = arena.makeCell("x");
  zigzag::expectWritten(arena.link(pX, core.dims().values, zigzag::Posward,
                                   arena.makeCell("Int")));

  auto checkRes = runInlangChecker(arena, core, vm, meta, *r1Expr, pX, true);
  EXPECT_TRUE(checkRes.ok) << checkRes.error;
  EXPECT_EQ(checkRes.resultType, "Int");
  EXPECT_TRUE(checkRes.pure);
  EXPECT_TRUE(checkRes.closed);
}

TEST(VAgentSemanticsTest, RejectsInvalidTypeOperations) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  zigzag::vortex::VortexVM vm(core);
  const auto programRoot = arena.makeCell();
  const auto agentCell   = arena.makeCell();
  auto binds             = mintMetaBindSet(arena, core, programRoot, agentCell);
  ASSERT_TRUE(binds.has_value());
  MetaRuntime meta(core, vm, *binds);

  // Expression: "hello" + 10
  auto r1Expr = parseSourceToR1(arena, core, "\"hello\" + 10");
  ASSERT_TRUE(r1Expr.has_value());

  auto checkRes =
      runInlangChecker(arena, core, vm, meta, *r1Expr, std::nullopt, true);
  EXPECT_FALSE(checkRes.ok);
}

} // namespace
