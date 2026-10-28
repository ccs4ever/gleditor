#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/vql/vql_engine.hpp"

namespace {

using xanadu::MultiStoreCoordinator;
using xanadu::Store;
using xanadu::UserPermascroll;
using xanadu::vql::VQLEngine;
using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;

struct ArenaSortFixture {
  zigzag::ArenaManifold arena;
  zigzag::vortex::VortexCore core{arena};
  VQLEngine engine{core};
  DimRef videos{engine.resolveDimension("d.videos")};
  DimRef tag{engine.resolveDimension("d.tag")};
  std::array<CellRef, 3> items{arena.makeCell("one"), arena.makeCell("two"),
                               arena.makeCell("three")};

  ArenaSortFixture(const std::array<std::string, 3> &keys) {
    for (std::size_t i = 0; i < items.size(); ++i) {
      const auto value = arena.makeCell(keys[i]);
      zigzag::expectWritten(arena.link(items[i], tag, DimVector::POS, value));
      const auto previous = i == 0 ? core.home() : items[i - 1];
      zigzag::expectWritten(
          arena.link(previous, videos, DimVector::POS, items[i]));
    }
  }

  ArenaSortFixture(const std::array<std::int64_t, 3> &keys) {
    for (std::size_t i = 0; i < items.size(); ++i) {
      const auto value = arena.makeScalarCell(keys[i]);
      zigzag::expectWritten(arena.link(items[i], tag, DimVector::POS, value));
      const auto previous = i == 0 ? core.home() : items[i - 1];
      zigzag::expectWritten(
          arena.link(previous, videos, DimVector::POS, items[i]));
    }
  }
};

TEST(VQLSortTest, NumericKeysUseNumericOrderingAndRelinkReciprocally) {
  ArenaSortFixture fixture(std::array<std::int64_t, 3>{10, 2, 1});
  const auto sorted =
      fixture.engine.execute("sort_into(##/d.videos, by: +d.tag)");

  ASSERT_EQ(sorted.size(), 3U);
  EXPECT_THAT(sorted, ::testing::ElementsAre(fixture.items[2], fixture.items[1],
                                             fixture.items[0]));
  EXPECT_EQ(fixture.arena.linked(fixture.core.home(), fixture.videos),
            fixture.items[2]);
  EXPECT_EQ(fixture.arena.linked(fixture.items[2], fixture.videos),
            fixture.items[1]);
  EXPECT_EQ(fixture.arena.linked(fixture.items[1], fixture.videos),
            fixture.items[0]);
  EXPECT_EQ(
      fixture.arena.linked(fixture.items[0], fixture.videos, DimVector::NEG),
      fixture.items[1]);
}

TEST(VQLSortTest, StringKeysUseLexicalOrdering) {
  ArenaSortFixture fixture(std::array<std::string, 3>{"2", "10", "1"});
  const auto sorted =
      fixture.engine.execute("sort_into(##/d.videos, by: +d.tag)");

  EXPECT_THAT(sorted, ::testing::ElementsAre(fixture.items[2], fixture.items[1],
                                             fixture.items[0]));
}

TEST(VQLSortTest, LargeIntegerKeysRemainDistinctAndEqualKeysStayStable) {
  ArenaSortFixture fixture(std::array<std::int64_t, 3>{
      9007199254740993LL, 9007199254740992LL, 9007199254740993LL});
  const auto sorted =
      fixture.engine.execute("sort_into(##/d.videos, by: +d.tag)");
  EXPECT_THAT(sorted, ::testing::ElementsAre(fixture.items[1], fixture.items[0],
                                             fixture.items[2]));
}

TEST(VQLSortTest, LambdaReceivesEachCandidateAsItsContext) {
  ArenaSortFixture fixture(std::array<std::string, 3>{"c", "a", "b"});
  const auto sorted = fixture.engine.execute(
      R"(sort_into(##/d.videos, by: \($candidate) $candidate/+d.tag))");

  EXPECT_THAT(sorted, ::testing::ElementsAre(fixture.items[1], fixture.items[2],
                                             fixture.items[0]));
}

TEST(VQLSortTest, SortingAStoreProxyDoesNotAppendStoreOperations) {
  int unique{};
  const auto path =
      std::filesystem::temp_directory_path() /
      ("vql_sort_" + std::to_string(reinterpret_cast<std::uintptr_t>(&unique)));
  UserPermascroll::Config config;
  config.storageDir = path / "permascroll";
  auto scroll       = std::make_shared<UserPermascroll>(config);
  auto store        = std::make_shared<Store>(scroll);
  auto version      = store->sliceGenesis({});
  const auto videos = store->makeDimension(version, "d.videos");
  version           = videos.version;
  const auto tag    = store->makeDimension(version, "d.tag");
  version           = tag.version;
  const auto home   = store->homeCell().value();
  auto previous     = home;
  for (const auto [name, key] :
       std::array<std::pair<std::string_view, std::string_view>, 3>{
           {{"one", "c"}, {"two", "a"}, {"three", "b"}}}) {
    version         = store->makeCell(version, name);
    const auto cell = store->cellRefOf(version).value();
    version =
        store->setLink(version, previous, videos.dim, DimVector::POS, cell);
    previous           = cell;
    version            = store->makeCell(version, key);
    const auto keyCell = store->cellRefOf(version).value();
    version = store->setLink(version, cell, tag.dim, DimVector::POS, keyCell);
  }
  const auto before = store->latest();
  MultiStoreCoordinator coordinator;
  coordinator.addStore("videos", "primary", store, version);
  VQLEngine engine(coordinator);

  const auto sorted = engine.execute("sort_into(##/d.videos, by: +d.tag)");

  ASSERT_EQ(sorted.size(), 3U);
  EXPECT_EQ(store->latest(), before);
  EXPECT_EQ(engine.execute("##/d.videos"), sorted);
  EXPECT_EQ(coordinator.arena().textOf(sorted[0]), "two");
  EXPECT_EQ(coordinator.arena().textOf(sorted[1]), "three");
  EXPECT_EQ(coordinator.arena().textOf(sorted[2]), "one");
  std::error_code ignored;
  std::filesystem::remove_all(path, ignored);
}

} // namespace
