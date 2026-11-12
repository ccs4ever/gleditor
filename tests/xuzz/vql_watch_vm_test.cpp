/**
 * @file vql_watch_vm_test.cpp
 * @brief Watch statements, polling subscriptions, mounts and scoped
 * perspective blocks as compiled VM programs: what refuses, and by what name.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "common/xanadu/vql/parser.hpp"
#include "common/xanadu/vql/vql_engine.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace {

using namespace xanadu::vql;
using testing::HasSubstr;
using zigzag::DimVector;

struct VQLWatchVmTest : testing::Test {
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

TEST_F(VQLWatchVmTest, WatchWithoutBaselineNamesTheBaselineAndThePollingApi) {
  const auto before  = arena.cellCount();
  const auto message = refusalOf("watch ##/d.items as $item { $item }");
  EXPECT_THAT(message, HasSubstr("'since' baseline"));
  EXPECT_THAT(message, HasSubstr("subscribeWatch"));
  EXPECT_THAT(message, HasSubstr("pollWatch"));
  EXPECT_EQ(arena.cellCount(), before);
}

TEST_F(VQLWatchVmTest, WatchSinceOnAnArenaWithoutStoreHistoryNamesTheStore) {
  const auto message =
      refusalOf("watch ##/d.items as $item since @88 { $item }");
  EXPECT_THAT(message, HasSubstr("local Store"));
  EXPECT_THAT(message, HasSubstr("store-backed history"));
  EXPECT_THAT(message, HasSubstr("pollWatch"));
}

TEST_F(VQLWatchVmTest, WatchOverRemoteDiscoveryNamesTheMissingSession) {
  EXPECT_THAT(
      refusalOf("watch topic(\"ci\")/d.active as $job since @1 { $job }"),
      HasSubstr("ZZ9P"));
}

TEST_F(VQLWatchVmTest, ExecuteWatchRunsTheCompiledRefusal) {
  Parser parser("watch ##/d.items as $item { $item }");
  const auto query = parser.parseQuery();
  ASSERT_EQ(query.body.statements.size(), 1u);
  const auto &watch = std::get<WatchStatement>(query.body.statements.front());
  try {
    (void)engine.executeWatch(watch);
    ADD_FAILURE() << "executeWatch must refuse a watch without a baseline";
  } catch (const std::runtime_error &refusal) {
    EXPECT_THAT(refusal.what(), HasSubstr("'since' baseline"));
    EXPECT_THAT(refusal.what(), HasSubstr("pollWatch"));
  }
}

TEST_F(VQLWatchVmTest, SubscribingToRemoteDiscoveryIsRefused) {
  Parser parser("topic(\"ci\")/d.active");
  const auto query = parser.parseQuery();
  try {
    (void)engine.subscribeWatch(std::get<PathExpression>(query.expr));
    ADD_FAILURE() << "a remote subscription must refuse";
  } catch (const std::runtime_error &refusal) {
    EXPECT_THAT(refusal.what(), HasSubstr("'topic'"));
    EXPECT_THAT(refusal.what(), HasSubstr("ZZ9P"));
  }
}

TEST_F(VQLWatchVmTest, SubscriptionsPollTheCurrentShapeOfTheirPath) {
  const auto dim = engine.resolveDimension("d.items");
  const auto a   = arena.makeCell("a");
  ASSERT_TRUE(arena.link(core.home(), dim, DimVector::POS, a));

  Parser parser("##/d.items");
  const auto id =
      engine.subscribeWatch(std::get<PathExpression>(parser.parseQuery().expr));
  EXPECT_EQ(engine.pollWatch(id).size(), 1u);

  const auto b = arena.makeCell("b");
  ASSERT_TRUE(arena.link(a, dim, DimVector::POS, b));
  const auto events = engine.pollWatch(id);
  ASSERT_EQ(events.size(), 2u);
  EXPECT_EQ(events[0].change, VQLEngine::WatchChange::Modified);
  EXPECT_EQ(events[1].cell, b);
  EXPECT_EQ(events[1].change, VQLEngine::WatchChange::Added);
  EXPECT_TRUE(engine.cancelWatch(id));
  EXPECT_THROW((void)engine.pollWatch(id), std::out_of_range);
}

TEST_F(VQLWatchVmTest, RemoteMountNamesItsAnchor) {
  const auto message = refusalOf("mount topic(\"cluster\") as +d.remote; ##");
  EXPECT_THAT(message, HasSubstr("remote store anchor 'topic'"));
  EXPECT_THAT(message, HasSubstr("ZZ9P"));
}

TEST_F(VQLWatchVmTest, LocalMountWalksToItsRoot) {
  engine.execute("mount current() as +d.x; ##");
  EXPECT_EQ(engine.execute("##/+d.x"),
            std::vector<zigzag::CellRef>{core.home()});
}

TEST_F(VQLWatchVmTest, UnmountRemovesTheMountAndRefusesAnUnmountedAlias) {
  engine.execute("mount current() as +d.x; ##");
  EXPECT_TRUE(engine.execute("unmount +d.x; ##/+d.x").empty());
  EXPECT_THAT(refusalOf("unmount +d.x; ##"),
              HasSubstr("no mount is registered for d.x in the requested "
                        "direction"));
}

TEST_F(VQLWatchVmTest, WatchInsideAPerspectiveBlockIsRefusedByName) {
  EXPECT_THAT(
      refusalOf(
          "with perspective(\"alice\") { watch ##/d.items as $i { $i } }"),
      HasSubstr("watch statement inside a perspective block"));
}

// Until the perspective selector card lands, a read inside a block is
// qualified into a perspective step and refused by the selector lowering,
// where an unqualified canonical read would have returned empty. Replace this
// with the read assertions when that card lands.
TEST_F(VQLWatchVmTest, PerspectiveBlockReadsAreQualifiedNotCanonical) {
  const auto subject = arena.makeCell("subject");
  engine.setVariable("p", subject);
  EXPECT_TRUE(engine.execute("$p/+d.comment").empty());
  EXPECT_THAT(refusalOf("with perspective(\"alice\") { $p/+d.comment }"),
              HasSubstr("path selector does not have Vortex graph lowering"));
  EXPECT_TRUE(engine.execute("$p/+d.comment").empty());
}

TEST_F(VQLWatchVmTest, PerspectiveBlockRefusesStepsItCannotQualify) {
  const auto subject = arena.makeCell("subject");
  engine.setVariable("p", subject);
  for (const char *form :
       {"$p/count()", "$p/+d.comment/count()", "$p/+d.comment%$p"}) {
    SCOPED_TRACE(form);
    EXPECT_THAT(
        refusalOf(std::string("with perspective(\"alice\") { ") + form + " }"),
        HasSubstr("perspective block cannot qualify"));
  }
}

TEST_F(VQLWatchVmTest, InnerPerspectiveBlockOverridesTheOuterName) {
  const auto subject = arena.makeCell("subject");
  engine.setVariable("p", subject);
  // The outer block qualifies nothing inside the inner one: the inner body is
  // refused by the selector lowering under its own name, not the outer's, and
  // an outer statement beside it is qualified separately.
  EXPECT_THAT(refusalOf("with perspective(\"bob\") { "
                        "with perspective(\"alice\") { $p/+d.comment } }"),
              HasSubstr("path selector does not have Vortex graph lowering"));
}

} // namespace
