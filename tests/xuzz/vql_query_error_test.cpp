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

} // namespace
