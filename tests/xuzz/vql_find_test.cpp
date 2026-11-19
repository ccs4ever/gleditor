/**
 * @file vql_find_test.cpp
 * @brief find(needle) lowered to RelationNext over the host's prose provider:
 * the hits and their order, the literal matching rules, a provider that keeps
 * nothing between queries, and what the program release does with a hit.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/vortex/vortex_vm.hpp"
#include "common/xanadu/vql/execution.hpp"
#include "common/xanadu/vql/prose_hits.hpp"
#include "common/xanadu/vql/vql_engine.hpp"

namespace {

namespace fs = std::filesystem;
using xanadu::MultiStoreCoordinator;
using xanadu::Store;
using xanadu::UserPermascroll;
using zigzag::CellRef;

struct FindFixture : ::testing::Test {
  fs::path dir;
  std::vector<std::shared_ptr<Store>> stores;
  MultiStoreCoordinator coord;

  void SetUp() override {
    dir =
        fs::temp_directory_path() /
        ("vql_find_test_" +
         std::string(
             ::testing::UnitTest::GetInstance()->current_test_info()->name()));
    fs::remove_all(dir);
    fs::create_directories(dir);
  }
  void TearDown() override { fs::remove_all(dir); }

  // A store holding one typed document; it is added to the coordinator under
  // @p label.
  std::shared_ptr<Store> addDocument(const std::string &label,
                                     const std::string &text) {
    UserPermascroll::Config config;
    config.storageDir = dir / (label + "-permascroll");
    auto store =
        std::make_shared<Store>(std::make_shared<UserPermascroll>(config));
    std::ignore = store->insert(xanadu::MicroversionId{}, 0, text);
    coord.addStore(label, "primary", store);
    stores.push_back(store);
    return store;
  }

  std::vector<std::string> find(xanadu::vql::VQLEngine &engine,
                                const std::string &query) {
    std::vector<std::string> texts;
    for (const auto hit : engine.execute(query))
      texts.push_back(coord.arena().textOf(hit));
    return texts;
  }
};

TEST_F(FindFixture, NoHitsAnswersAnEmptyResult) {
  addDocument("prose", "alpha\nbeta\n");
  xanadu::vql::VQLEngine engine(coord);
  EXPECT_TRUE(engine.execute(R"(find("gamma"))").empty());
}

TEST_F(FindFixture, HitsInSeveralStoresComeInStoreOrder) {
  addDocument("first", "one needle\nplain\n");
  addDocument("second", "needle two\n");
  addDocument("third", "nothing here\n");
  addDocument("fourth", "a needle in the fourth\n");
  xanadu::vql::VQLEngine engine(coord);
  EXPECT_THAT(find(engine, R"(find("needle"))"),
              ::testing::ElementsAre("one needle", "needle two",
                                     "a needle in the fourth"));
}

// The oracle's order inside one store: manifold cells first, then the
// document's lines. The cell is answered as the arena's proxy for it.
TEST_F(FindFixture, CellTextHitsComeBeforeLineHitsInTheSameStore) {
  UserPermascroll::Config config;
  config.storageDir = dir / "cells-permascroll";
  auto store =
      std::make_shared<Store>(std::make_shared<UserPermascroll>(config));
  const auto typed  = store->insert(xanadu::MicroversionId{}, 0,
                                    "a line with needle\nanother line\n");
  const auto sliced = store->sliceGenesis(typed);
  const auto cell   = store->makeCell(sliced, "a cell with needle");
  const auto second = store->makeCell(cell, "unrelated cell");
  std::ignore       = second;
  coord.addStore("mixed", "primary", store);
  addDocument("after", "needle again\n");

  xanadu::vql::VQLEngine engine(coord);
  const auto hits = engine.execute(R"(find("needle"))");
  ASSERT_EQ(hits.size(), 3U);
  EXPECT_EQ(coord.arena().textOf(hits[0]), "a cell with needle");
  EXPECT_EQ(coord.arena().textOf(hits[1]), "a line with needle");
  EXPECT_TRUE(coord.arena().quotedContent(hits[1]).has_value());
  // The next store follows the whole of the first.
  EXPECT_EQ(coord.arena().textOf(hits[2]), "needle again");
}

// The provider extension on its own: findHits() is proseHits() plus the cell
// half, and proseHits() alone still answers document lines only.
TEST_F(FindFixture, FindHitsAddTheCellHalfThatProseHitsLeavesOut) {
  UserPermascroll::Config config;
  config.storageDir = dir / "direct-permascroll";
  auto store =
      std::make_shared<Store>(std::make_shared<UserPermascroll>(config));
  const auto typed =
      store->insert(xanadu::MicroversionId{}, 0, "line needle\n");
  const auto sliced = store->sliceGenesis(typed);
  std::ignore       = store->makeCell(sliced, "cell needle");
  coord.addStore("direct", "primary", store);

  auto &arena = coord.arena();
  EXPECT_EQ(xanadu::vql::proseHits(coord, "needle", arena).size(), 1U);
  const auto both = xanadu::vql::findHits(coord, "needle", arena);
  ASSERT_EQ(both.size(), 2U);
  EXPECT_EQ(arena.textOf(both[0]), "cell needle");
  EXPECT_EQ(arena.textOf(both[1]), "line needle");
  EXPECT_TRUE(xanadu::vql::findHits(coord, "absent", arena).empty());
}

TEST_F(FindFixture, OnlyTheCurrentVersionIsSearched) {
  auto store    = addDocument("versions", "old needle\n");
  const auto v1 = store->latest();
  std::ignore   = store->insert(v1, 0, "new ");
  xanadu::vql::VQLEngine engine(coord);
  const auto hits = engine.execute(R"(find("needle"))");
  ASSERT_EQ(hits.size(), 1U);
  EXPECT_EQ(coord.arena().textOf(hits[0]), "new old needle");
  const auto sourceDim = coord.core().findDimension("d.source");
  ASSERT_TRUE(sourceDim.has_value());
  const auto origin = coord.arena().linked(hits[0], *sourceDim);
  ASSERT_TRUE(origin);
  EXPECT_THAT(coord.arena().textOf(*origin),
              ::testing::HasSubstr("version=" + store->latest().str()));
}

TEST_F(FindFixture, ARegexLookingNeedleMatchesLiterally) {
  addDocument("regex", "abc\nxa.cx\nf(o)o\nstar*\n");
  xanadu::vql::VQLEngine engine(coord);
  EXPECT_THAT(find(engine, R"(find("a.c"))"), ::testing::ElementsAre("xa.cx"));
  EXPECT_THAT(find(engine, R"q(find("(o)"))q"),
              ::testing::ElementsAre("f(o)o"));
  EXPECT_THAT(find(engine, R"(find("r*"))"), ::testing::ElementsAre("star*"));
  EXPECT_TRUE(find(engine, R"(find("^abc$"))").empty());
}

TEST_F(FindFixture, AUnicodeNeedleMatchesItsBytes) {
  addDocument("unicode",
              "plain\ncaf\xC3\xA9 au lait\n\xE6\x97\xA5\xE6\x9C\xAC\n");
  xanadu::vql::VQLEngine engine(coord);
  EXPECT_THAT(find(engine, "find(\"caf\xC3\xA9\")"),
              ::testing::ElementsAre("caf\xC3\xA9 au lait"));
  EXPECT_THAT(find(engine, "find(\"\xE6\x97\xA5\")"),
              ::testing::ElementsAre("\xE6\x97\xA5\xE6\x9C\xAC"));
}

// The oracle searched the whole text, so a needle holding a line break matches
// where it starts and answers that line only. It is pinned here because the
// spike expected no match.
TEST_F(FindFixture, ANeedleSpanningALineBreakAnswersTheLineItStartsOn) {
  addDocument("break", "first\nsecond\nthird\n");
  xanadu::vql::VQLEngine engine(coord);
  EXPECT_THAT(find(engine, R"(find("first\nsecond"))"),
              ::testing::ElementsAre("first"));
  EXPECT_TRUE(find(engine, R"(find("first second"))").empty());
}

TEST_F(FindFixture, AVeryLongNeedleMatchesOrMissesWithoutTrouble) {
  const std::string longLine(5000, 'x');
  addDocument("long", "short\n" + longLine + "\n");
  xanadu::vql::VQLEngine engine(coord);
  EXPECT_THAT(find(engine, "find(\"" + longLine + "\")"),
              ::testing::ElementsAre(longLine));
  EXPECT_TRUE(find(engine, "find(\"" + longLine + "y\")").empty());
}

TEST_F(FindFixture, AnEmptyNeedleVisitsEveryLineIncludingTheLastEmptyOne) {
  addDocument("empty", "a\n\nb\n");
  xanadu::vql::VQLEngine engine(coord);
  EXPECT_THAT(find(engine, R"(find(""))"),
              ::testing::ElementsAre("a", "", "b", ""));
}

TEST_F(FindFixture, TheSameQueryTwiceAnswersTheSameHits) {
  addDocument("twice", "needle one\nplain\nneedle two\n");
  xanadu::vql::VQLEngine engine(coord);
  const auto first  = find(engine, R"(find("needle"))");
  const auto second = find(engine, R"(find("needle"))");
  EXPECT_EQ(first, second);
  EXPECT_EQ(first.size(), 2U);
  // A different needle after it sees none of the first scan's list.
  EXPECT_THAT(find(engine, R"(find("plain"))"),
              ::testing::ElementsAre("plain"));
}

TEST_F(FindFixture, AMutationAfterTheFirstRunIsSeenByTheSecond) {
  auto store = addDocument("mutated", "needle one\n");
  xanadu::vql::VQLEngine engine(coord);
  EXPECT_THAT(find(engine, R"(find("needle"))"),
              ::testing::ElementsAre("needle one"));
  std::ignore = store->insert(store->latest(), 0, "needle zero\n");
  EXPECT_THAT(find(engine, R"(find("needle"))"),
              ::testing::ElementsAre("needle zero", "needle one"));
}

TEST_F(FindFixture, FindAnswersAsAnArgumentToACountingFunction) {
  addDocument("count", "needle\nneedle\nother\n");
  xanadu::vql::VQLEngine engine(coord);
  const auto counted = engine.execute(R"(count(find("needle")))");
  ASSERT_EQ(counted.size(), 1U);
  EXPECT_EQ(coord.arena().asInt64(counted.front()), 2);
}

TEST_F(FindFixture, ANeedleHeldInAVariableIsRead) {
  addDocument("variable", "xx needle xx\nnone\n");
  xanadu::vql::VQLEngine engine(coord);
  EXPECT_THAT(find(engine, R"(let $n := "needle"; return find($n))"),
              ::testing::ElementsAre("xx needle xx"));
}

TEST_F(FindFixture, FindInAStepPositionStaysRefused) {
  addDocument("step", "needle\n");
  xanadu::vql::VQLEngine engine(coord);
  for (const auto *query :
       {R"(##/find("needle"))", R"(##/d.items/find("a"))"}) {
    try {
      std::ignore = engine.execute(query);
      ADD_FAILURE() << query << " was not refused";
    } catch (const std::runtime_error &error) {
      EXPECT_THAT(std::string(error.what()),
                  ::testing::HasSubstr("find is only supported as an anchor"))
          << query;
    }
  }
}

// What the program release does with a hit. A hit is a quote cell linked along
// d.source to a cell the provider minted during the run; the release carries
// it out through CopyOut like any result the program minted.
struct FindReleaseFixture : FindFixture {
  zigzag::vortex::VortexCore &core{coord.core()};
  zigzag::vortex::VortexVM vm{core};
  xanadu::vql::QueryExecutor executor{coord, vm};
};

TEST_F(FindReleaseFixture, AHitSurvivesTheReleaseAndTheProgramCellsGo) {
  addDocument("release", "needle line\nplain\n");
  // The first run may name d.source; the arena at rest is measured after it.
  ASSERT_TRUE(executor.execute(R"(find("needle"))"));
  const auto before = coord.arena().cellCount();
  const auto result = executor.execute(R"(find("needle"))");
  ASSERT_TRUE(result) << result.error().message;
  ASSERT_EQ(result->size(), 1U);
  EXPECT_FALSE(executor.programRetainedBecause())
      << *executor.programRetainedBecause();
  EXPECT_EQ(coord.arena().outstandingMarks(), 0U);
  const auto hit = result->front();
  ASSERT_TRUE(coord.arena().contains(hit));
  EXPECT_EQ(coord.arena().textOf(hit), "needle line");
  EXPECT_TRUE(coord.arena().quotedContent(hit).has_value());
  const auto sourceDim = coord.core().findDimension("d.source");
  ASSERT_TRUE(sourceDim.has_value());
  const auto origin = coord.arena().linked(hit, *sourceDim);
  ASSERT_TRUE(origin);
  EXPECT_THAT(coord.arena().textOf(*origin), ::testing::EndsWith("&at=0"));
  // The hit and its source cell are the only growth.
  EXPECT_EQ(coord.arena().cellCount(), before + 2U);
}

} // namespace
