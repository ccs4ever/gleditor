#include <gtest/gtest.h>

#include "common/xanadu/vql/vql_engine.hpp"

namespace {
TEST(VQLGeneratorsTest, MissingLocalStoreAndFilteredSlicesHaveNoFallback) {
  zigzag::ArenaManifold arena;
  xanadu::vql::VQLEngine engine(arena);
  EXPECT_FALSE(engine.execute("current()").empty());
  // R-GEN: a store that is not selected never answers with the ambient home.
  EXPECT_THROW(engine.execute("local(\"missing\")"), std::runtime_error);
  EXPECT_THROW(engine.execute("slices(\"missing\")"), std::runtime_error);
  EXPECT_THROW(engine.execute("current()/slices(\"missing\")"),
               std::runtime_error);
  EXPECT_THROW(engine.execute("topic(\"missing\")"), std::runtime_error);
}
} // namespace
