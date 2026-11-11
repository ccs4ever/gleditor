#include <gtest/gtest.h>

#include <stdexcept>
#include <tuple>

#include "common/xanadu/vql/parser.hpp"
#include "common/xanadu/vql/query_error.hpp"
#include "common/xanadu/vql/vql_engine.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {

using xanadu::vql::ParseError;
using xanadu::vql::QueryErrorCategory;
using xanadu::vql::QueryRefusal;
using xanadu::vql::TypeContractError;
using xanadu::vql::VQLEngine;

constexpr const char *unlowered = "##/d.items/sort_into(by: +d.tag)";
constexpr const char *unknown   = "frob(1)";
constexpr const char *typeError = "def pure bad(): Int := \"abc\"; bad()";
constexpr const char *badSyntax = "##/(";

struct Fixture {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  VQLEngine engine{core};
};

TEST(VQLQueryErrorTest, TryExecuteReturnsEachCategoryWithoutThrowing) {
  Fixture f;
  const auto parse = f.engine.tryExecute(badSyntax);
  ASSERT_FALSE(parse);
  EXPECT_EQ(parse.error().category, QueryErrorCategory::Parse);

  const auto refused = f.engine.tryExecute(unlowered);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().category, QueryErrorCategory::Unsupported);
  EXPECT_FALSE(refused.error().message.empty());

  const auto missing = f.engine.tryExecute(unknown);
  ASSERT_FALSE(missing);
  EXPECT_EQ(missing.error().category, QueryErrorCategory::Unknown)
      << missing.error().message;

  const auto typed = f.engine.tryExecute(typeError);
  ASSERT_FALSE(typed);
  EXPECT_EQ(typed.error().category, QueryErrorCategory::TypeContract);

  const auto ok = f.engine.tryExecute("##");
  ASSERT_TRUE(ok);
  ASSERT_EQ(ok->size(), 1u);
  EXPECT_EQ(ok->front(), f.core.home());
}

TEST(VQLQueryErrorTest, ExecuteThrowsRefusalsAsRuntimeError) {
  Fixture f;
  EXPECT_THROW(std::ignore = f.engine.execute(unlowered), QueryRefusal);
  EXPECT_THROW(std::ignore = f.engine.execute(unknown), std::runtime_error);
  try {
    std::ignore = f.engine.execute(unknown);
    FAIL() << "expected a refusal";
  } catch (const QueryRefusal &refusal) {
    EXPECT_EQ(refusal.category(), QueryErrorCategory::Unknown);
  }
}

TEST(VQLQueryErrorTest, ExecuteKeepsParseAndTypeContractExceptionTypes) {
  Fixture f;
  EXPECT_THROW(std::ignore = f.engine.execute(badSyntax), ParseError);
  EXPECT_THROW(std::ignore = f.engine.execute(typeError), TypeContractError);
}

// A wrong argument count for a lowered builtin is the caller's mistake, not a
// missing feature.
TEST(VQLQueryErrorTest, MalformedSourceIsInvalidArgument) {
  Fixture f;
  constexpr const char *badArity = "let $t := now(1); return $t";
  const auto result              = f.engine.tryExecute(badArity);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().category, QueryErrorCategory::Malformed);
  EXPECT_THROW(std::ignore = f.engine.execute(badArity), std::invalid_argument);
}

TEST(VQLQueryErrorTest, BudgetAndRuntimeFailuresAreRefusals) {
  Fixture f;
  constexpr const char *recursion =
      "def pure loop($node: Cell): Cell := loop($node); loop(##)";
  const auto budget = f.engine.tryExecute(recursion);
  ASSERT_FALSE(budget);
  EXPECT_EQ(budget.error().category, QueryErrorCategory::Budget);
  EXPECT_THROW(std::ignore = f.engine.execute(recursion), QueryRefusal);

  constexpr const char *multiCell =
      "def choose($item: Cell { +d.value: $value: Cell := $fallback }): "
      "Cell := $value; let $fallback := ##/d.fallbacks; "
      "let $item := %\"item\"; choose($item)";
  const auto first  = f.arena.makeCell("first");
  const auto second = f.arena.makeCell("second");
  const auto dim    = f.engine.resolveDimension("d.fallbacks");
  ASSERT_TRUE(f.arena.link(f.core.home(), dim, zigzag::DimVector::POS, first));
  ASSERT_TRUE(f.arena.link(first, dim, zigzag::DimVector::POS, second));
  const auto runtime = f.engine.tryExecute(multiCell);
  ASSERT_FALSE(runtime);
  EXPECT_EQ(runtime.error().category, QueryErrorCategory::Runtime);
  EXPECT_THROW(std::ignore = f.engine.execute(multiCell), QueryRefusal);
}

} // namespace
