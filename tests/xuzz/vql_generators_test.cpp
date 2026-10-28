#include <gtest/gtest.h>

#include "common/xanadu/vql/vql_engine.hpp"

namespace {
TEST(VQLGeneratorsTest, MissingLocalStoreAndFilteredSlicesHaveNoFallback) {
  zigzag::ArenaManifold arena;
  xanadu::vql::VQLEngine engine(arena);
  EXPECT_FALSE(engine.execute("current()").empty());
  EXPECT_TRUE(engine.execute("local(\"missing\")").empty());
  EXPECT_TRUE(engine.execute("slices(\"missing\")").empty());
  EXPECT_TRUE(engine.execute("current()/slices(\"missing\")").empty());
  EXPECT_THROW(engine.execute("topic(\"missing\")"), std::runtime_error);
}
} // namespace
