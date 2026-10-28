#include <exception>
#include <vector>

#include <gtest/gtest.h>

#include "common/xanadu/vql/vql_engine.hpp"

namespace {

TEST(VQLArgumentScopeTest, ExplicitArgumentsUseTheCallerScope) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  xanadu::vql::VQLEngine engine(core);

  const auto result = engine.execute(R"(
    def identity($value: Int): Int := $value;
    def outer($x: Int, $y: Int): Int := $y;
    let $x := 9;
    outer(1, identity($x))
  )");

  ASSERT_EQ(result.size(), 1u);
  EXPECT_EQ(arena.asInt64(result.front()), 9);
}

TEST(VQLArgumentScopeTest, ExtraExplicitArgumentsAreEvaluatedBeforeMatching) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  xanadu::vql::VQLEngine engine(core);

  try {
    (void)engine.execute(R"(
      def broken(): Int := "not an integer";
      def one($value: Int): Int := $value;
      one("also not an integer", broken())
    )");
    FAIL() << "the extra explicit argument was not evaluated";
  } catch (const std::exception &error) {
    EXPECT_STREQ(error.what(),
                 "broken() returned a value outside its declared type");
  }
}

TEST(VQLArgumentScopeTest, UntypedFunctorArgumentsRemainCaptured) {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core(arena);
  xanadu::vql::VQLEngine engine(core);
  const auto child = arena.makeCell("child");
  ASSERT_TRUE(arena.link(core.home(), core.dims().step, false, child));

  const auto result = engine.execute(R"(
    def pure apply($f) := ##/$f;
    let $up := /+d.step;
    apply($up)
  )");

  EXPECT_EQ(result, (std::vector<zigzag::CellRef>{child}));
}

} // namespace
