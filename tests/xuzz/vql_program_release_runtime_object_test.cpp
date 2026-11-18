/**
 * @file vql_program_release_runtime_object_test.cpp
 * @brief A closure names its descriptor and environment cells by integer
 * reference, so a result or binding that is one must never be re-minted by the
 * program release: the run keeps its program, saying why, rather than hand
 * back a number that names no cell. And programRetainedBecause() tells the
 * truth about the run that just finished.
 */
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vql/execution.hpp"
#include "common/xanadu/vql/vql_engine.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"

namespace xanadu::vql {
namespace {

using zigzag::CellRef;
using zigzag::DimVector;

struct Session {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  MultiStoreCoordinator coordinator{core};
  VQLEngine engine{coordinator};
  std::optional<std::string> retained;

  std::expected<std::vector<CellRef>, QueryExecutionError>
  run(const std::string_view query) {
    zigzag::vortex::VortexVM vm{core};
    QueryExecutor executor(engine, vm);
    auto result = executor.execute(query);
    retained    = executor.programRetainedBecause();
    return result;
  }
};

TEST(VqlProgramReleaseRuntimeObjectTest,
     AClosureReturnedByApplicationIsNeverCopiedOut) {
  Session s;
  ASSERT_TRUE(s.run(R"(let $f := \($x) \($y) $x; return $f)"));
  const auto applied = s.run("return $f(7)");
  ASSERT_TRUE(applied) << applied.error().message;
  ASSERT_EQ(applied->size(), 1U);
  ASSERT_TRUE(s.retained);
  EXPECT_NE(s.retained->find("runtime object"), std::string::npos)
      << *s.retained;
  s.engine.setVariable("g", applied->front());
  const auto inner = s.run("return $g(9)");
  ASSERT_TRUE(inner) << inner.error().message;
  ASSERT_EQ(inner->size(), 1U);
  EXPECT_EQ(s.arena.asInt64(inner->front()), std::optional<std::int64_t>{7});
}

TEST(VqlProgramReleaseRuntimeObjectTest,
     AClosureReachedThroughAnEarlierBindingIsNeverCopiedOut) {
  Session s;
  ASSERT_TRUE(s.run(R"(let $f := \($x) \($y) $x; return $f)"));
  ASSERT_TRUE(s.run("let $h := $f(7); return $h"));
  const auto again = s.run("return $h");
  ASSERT_TRUE(again) << again.error().message;
  s.engine.setVariable("k", *again);
  const auto inner = s.run("return $k(9)");
  ASSERT_TRUE(inner) << inner.error().message;
  ASSERT_EQ(inner->size(), 1U);
  EXPECT_EQ(s.arena.asInt64(inner->front()), std::optional<std::int64_t>{7});
}

TEST(VqlProgramReleaseRuntimeObjectTest, ProjectRefusesARuntimeObjectByName) {
  Session s;
  ASSERT_TRUE(s.run(R"(let $f := \($x) $x; return $f)"));
  const auto projected = s.run("return project($f, {*})");
  ASSERT_FALSE(projected);
  EXPECT_NE(projected.error().message.find("runtime object"), std::string::npos)
      << projected.error().message;
}

TEST(VqlProgramReleaseRuntimeObjectTest,
     AParseErrorDoesNotReportThePreviousReason) {
  Session s;
  const auto values = s.coordinator.resolveDimension("d.values");
  const auto tag    = s.coordinator.resolveDimension("d.tag");
  const auto first  = s.arena.makeScalarCell(std::int64_t{2});
  const auto second = s.arena.makeScalarCell(std::int64_t{1});
  zigzag::expectWritten(
      s.arena.link(s.core.home(), values, DimVector::POS, first));
  zigzag::expectWritten(s.arena.link(first, values, DimVector::POS, second));
  zigzag::expectWritten(
      s.arena.link(first, tag, DimVector::POS, s.arena.makeCell("b")));
  zigzag::expectWritten(
      s.arena.link(second, tag, DimVector::POS, s.arena.makeCell("a")));
  ASSERT_TRUE(s.run("sort_into(##/d.values, by: +d.tag)"));
  ASSERT_TRUE(s.retained);
  const auto broken = s.run("for $x in (((");
  ASSERT_FALSE(broken);
  EXPECT_FALSE(s.retained) << *s.retained;
}

TEST(VqlProgramReleaseRuntimeObjectTest,
     AFailedRunAfterAWriteSaysWhyItKeptItsProgram) {
  Session s;
  const auto tag = s.coordinator.resolveDimension("d.tag");
  zigzag::expectWritten(
      s.arena.link(s.core.home(), tag, DimVector::POS, s.arena.makeCell("b")));
  const auto failed =
      s.run("let $x := ##/d.tag; apply_shape($x, {!*}); return one(##/d.none)");
  ASSERT_FALSE(failed);
  ASSERT_TRUE(s.retained);
  EXPECT_NE(s.retained->find("writes cells"), std::string::npos) << *s.retained;
}

} // namespace
} // namespace xanadu::vql
