#include <gtest/gtest.h>
#include <string_view>

#include "common/xanadu/vagent/inlang_parser.hpp"
#include "common/xanadu/vagent/r1_ast.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {

using namespace xanadu::vagent;

TEST(VAgentInlangFrontendTest, ParseSourceToR1Expression) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);

  std::string_view source = "total + a.amount";
  auto r1                 = parseSourceToR1(arena, core, source);
  ASSERT_TRUE(r1.has_value()) << r1.error();
  EXPECT_TRUE(arena.contains(*r1));
  EXPECT_EQ(arena.textOf(*r1), "binary");

  auto expr = unmarshalR1ToAst(arena, core, *r1);
  ASSERT_TRUE(expr.has_value()) << expr.error();
  EXPECT_EQ((*expr)->kind, ExprKind::Binary);
  EXPECT_EQ((*expr)->text, "+");
}

TEST(VAgentInlangFrontendTest, ParseProgramToR1Module) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);

  std::string_view source = R"(
    module test:counter
    import std:meta
    event Tick { delta: Int }
    agent Counter {
      var count: Int := 0
      on Tick as t {
        count := count + t.delta
      }
    }
  )";

  auto r1 = parseProgramToR1(arena, core, source);
  ASSERT_TRUE(r1.has_value()) << r1.error();
  EXPECT_TRUE(arena.contains(*r1));
  EXPECT_EQ(arena.textOf(*r1), "program");

  auto prog = unmarshalR1ToProgram(arena, core, *r1);
  ASSERT_TRUE(prog.has_value()) << prog.error();
  EXPECT_EQ(prog->module, "test:counter");
  ASSERT_EQ(prog->imports.size(), 1u);
  EXPECT_EQ(prog->imports[0].path, "std:meta");
  ASSERT_EQ(prog->declarations.size(), 2u);
  EXPECT_EQ(prog->declarations[0].name, "Tick");
  EXPECT_EQ(prog->declarations[0].kind, DeclKind::Event);
  EXPECT_EQ(prog->declarations[1].name, "Counter");
  EXPECT_EQ(prog->declarations[1].kind, DeclKind::Agent);
}

} // namespace
