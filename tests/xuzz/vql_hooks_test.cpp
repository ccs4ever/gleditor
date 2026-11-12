/**
 * @file vql_hooks_test.cpp
 * @brief Shared-file hooks of the second VQL integrator: whole-walk steps,
 * causal version comparison, session mounts, variable perspective names,
 * the PathMorphism stub and the prose-hits provider.
 */
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <tuple>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/vql/parser.hpp"
#include "common/xanadu/vql/prose_hits.hpp"
#include "common/xanadu/vql/vql_engine.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {

using namespace xanadu::vql;
using testing::HasSubstr;
using zigzag::DimVector;

struct VQLHooksTest : testing::Test {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  VQLEngine engine{core};

  std::string refusalOf(const std::string &query) {
    try {
      (void)engine.execute(query);
    } catch (const std::runtime_error &refusal) {
      return refusal.what();
    }
    ADD_FAILURE() << "expected a refusal: " << query;
    return {};
  }
};

// H1
TEST_F(VQLHooksTest, ArgumentFreeCountOverAnEmptyWalkIsAPresentZero) {
  const auto counted = engine.execute("##/d.absent/count()");
  ASSERT_EQ(counted.size(), 1U);
  EXPECT_EQ(arena.asInt64(counted.front()), 0);
}

TEST_F(VQLHooksTest, ArgumentFreeCountCountsTheWholeWalk) {
  const auto dim = engine.resolveDimension("d.items");
  auto tail      = core.home();
  for (const char *text : {"a", "b", "c"}) {
    const auto cell = arena.makeCell(text);
    ASSERT_TRUE(arena.link(tail, dim, DimVector::POS, cell));
    tail = cell;
  }
  const auto counted = engine.execute("##/d.items/count()");
  ASSERT_EQ(counted.size(), 1U);
  EXPECT_EQ(arena.asInt64(counted.front()), 3);
}

TEST_F(VQLHooksTest, ArgumentFreeQuantifiersStillRefuseByName) {
  for (const char *name :
       {"any", "all", "none", "one", "optional_one", "first"}) {
    SCOPED_TRACE(name);
    EXPECT_THAT(refusalOf(std::string("##/d.items/") + name + "()"),
                HasSubstr(std::string("semantic builtin '") + name + "'"));
  }
}

// H2
TEST_F(VQLHooksTest, VersionOrderingIsCausalNotTextual) {
  // "10" sorts before "9" as text, yet 9 is the ancestor of 10.
  EXPECT_EQ(engine.execute("##[@9 < @10]").size(), 1U);
  EXPECT_TRUE(engine.execute("##[@10 < @9]").empty());
  EXPECT_EQ(engine.execute("##[@10 > @9]").size(), 1U);
  EXPECT_EQ(engine.execute("##[@9 <= @9]").size(), 1U);
  // Siblings are unordered.
  for (const char *op : {"<", ">", "<=", ">=", "="}) {
    SCOPED_TRACE(op);
    EXPECT_TRUE(
        engine.execute(std::string("##[@2a1 ") + op + " @2b1]").empty());
  }
  EXPECT_EQ(engine.execute("##[@2a1 != @2b1]").size(), 1U);
}

TEST_F(VQLHooksTest, VersionValuedVariablesCompareCausally) {
  EXPECT_EQ(
      engine.execute("let $a := @9 let $b := @10 return ##[$a < $b]").size(),
      1U);
  EXPECT_TRUE(engine.execute("let $a := @2a1 let $b := @2b1 return ##[$a < $b]")
                  .empty());
}

TEST_F(VQLHooksTest, ARunTimeVersionComparisonRefusesInsteadOfComparingText) {
  const auto dim  = engine.resolveDimension("d.items");
  const auto item = arena.makeCell("3");
  ASSERT_TRUE(arena.link(core.home(), dim, DimVector::POS, item));
  EXPECT_THAT(refusalOf("for $c in ##/d.items/count() return ##[$c < @5]"),
              HasSubstr("version comparison of a run-time version is not "
                        "lowered"));
}

// H3
TEST_F(VQLHooksTest, MountFallbackOnlyAppliesToAnEmptyWalk) {
  const auto dim  = engine.resolveDimension("d.cluster");
  const auto cell = arena.makeCell("real");
  ASSERT_TRUE(arena.link(core.home(), dim, DimVector::POS, cell));
  engine.execute("mount current() as +d.cluster; ##");
  EXPECT_EQ(engine.execute("##/+d.cluster"),
            std::vector<zigzag::CellRef>{cell});
}

TEST_F(VQLHooksTest, MountFallbackRequiresTheMountedDirection) {
  engine.execute("mount current() as +d.cluster; ##");
  EXPECT_TRUE(engine.execute("##/-d.cluster").empty());
  EXPECT_EQ(engine.execute("##/+d.cluster"),
            std::vector<zigzag::CellRef>{core.home()});
}

TEST_F(VQLHooksTest, UnmountInTheWrongDirectionNamesTheMount) {
  engine.execute("mount current() as +d.cluster; ##");
  EXPECT_THAT(refusalOf("unmount -d.cluster; ##"),
              HasSubstr("no mount is registered for d.cluster"));
  EXPECT_TRUE(engine.execute("unmount +d.cluster; ##/+d.cluster").empty());
}

TEST_F(VQLHooksTest, AMountDoesNotSurviveAFailedQuery) {
  EXPECT_THAT(refusalOf("mount current() as +d.cluster; ##/count(1, 2, 3)"),
              HasSubstr("count"));
  EXPECT_TRUE(engine.execute("##/+d.cluster").empty());
}

// H4
TEST(VQLHooksParserTest, PerspectiveNameRecordsWhetherItWasAVariable) {
  const auto parse = [](const char *source) {
    Parser parser(source);
    const auto query = parser.parseQuery();
    return std::get<PerspectiveBlock>(query.body.statements.at(0));
  };
  EXPECT_FALSE(parse(R"(with perspective("p") { ## })").perspectiveIsVariable);
  EXPECT_FALSE(parse("with perspective(p) { ## }").perspectiveIsVariable);
  const auto variable = parse("with perspective($p) { ## }");
  EXPECT_TRUE(variable.perspectiveIsVariable);
  EXPECT_EQ(variable.perspectiveName, "p");
}

TEST_F(VQLHooksTest, VariablePerspectiveNameResolvesWhenConstant) {
  engine.setVariable("who", arena.makeCell("alice"));
  engine.setVariable("subject", arena.makeCell("subject"));
  // Resolved to "alice" it reaches the selector lowering, as a literal does.
  EXPECT_THAT(
      refusalOf("with perspective($who) { $subject/+d.comment }"),
      testing::Not(HasSubstr("perspective($who) needs a variable bound")));
}

TEST_F(VQLHooksTest, VariablePerspectiveNameWithoutAConstantRefusesByName) {
  EXPECT_THAT(refusalOf("with perspective($nobody) { ## }"),
              HasSubstr("perspective($nobody) needs a variable bound to a "
                        "text constant"));
  engine.setVariable("number", arena.makeScalarCell(std::int64_t{7}));
  EXPECT_THAT(refusalOf("with perspective($number) { ## }"),
              HasSubstr("perspective($number) needs a variable bound"));
}

// H5
TEST_F(VQLHooksTest, PathMorphismRefusesThroughItsOwnStub) {
  engine.setVariable("p", arena.makeCell("subject"));
  EXPECT_THAT(refusalOf(R"($p/+d.comment => [ "note" ])"),
              HasSubstr("path selector does not have Vortex graph lowering"));
}

// H7
TEST(VQLHooksProseTest, ProseHitsFollowTheOraclesOrder) {
  const auto dir = std::filesystem::temp_directory_path() / "vql_hooks_prose";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  xanadu::UserPermascroll::Config config;
  config.storageDir = dir / "permascroll";
  auto scroll       = std::make_shared<xanadu::UserPermascroll>(config);
  auto store        = std::make_shared<xanadu::Store>(scroll);
  std::ignore       = store->insert(xanadu::MicroversionId{}, 0,
                                    "first needle\nplain\nsecond needle needle\n");
  xanadu::MultiStoreCoordinator coord;
  coord.addStore("prose", "primary", store);
  auto &arena = coord.arena();

  const auto hits = proseHits(coord, "needle", arena);
  ASSERT_EQ(hits.size(), 2U); // one hit per line, in offset order
  EXPECT_EQ(arena.textOf(hits[0]), "first needle");
  EXPECT_EQ(arena.textOf(hits[1]), "second needle needle");
  const auto quoted = arena.quotedContent(hits[0]);
  ASSERT_TRUE(quoted.has_value());
  EXPECT_EQ(quoted->store, store.get());

  EXPECT_EQ(proseHits(coord, "", arena).size(), 4U);
  EXPECT_EQ(proseHits(coord, "\n", arena).size(), 3U);
  EXPECT_TRUE(proseHits(coord, "absent", arena).empty());
  std::filesystem::remove_all(dir);
}

} // namespace
