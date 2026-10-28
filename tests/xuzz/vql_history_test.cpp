#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/vql/vql_engine.hpp"

namespace {

using xanadu::MicroversionId;
using xanadu::MultiStoreCoordinator;
using xanadu::Store;
using xanadu::UserPermascroll;
using xanadu::vql::VQLEngine;
using zigzag::DimVector;

struct HistoryFixture {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("vql_history_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
  UserPermascroll::Config config = [] {
    UserPermascroll::Config cfg;
    return cfg;
  }();
  std::shared_ptr<UserPermascroll> scroll;
  std::shared_ptr<Store> store;
  MultiStoreCoordinator coordinator;
  VQLEngine engine{coordinator};
  MicroversionId genesis;
  MicroversionId dimensionVersion;
  MicroversionId cellVersion;
  MicroversionId linkedVersion;
  std::optional<zigzag::CellRef> cell;

  HistoryFixture() {
    config.storageDir = path / "permascroll";
    scroll            = std::make_shared<UserPermascroll>(config);
    store             = std::make_shared<Store>(scroll);
    genesis           = store->sliceGenesis({});
    const auto dim    = store->makeDimension(genesis, "d.history-test");
    dimensionVersion  = dim.version;
    cellVersion       = store->makeCell(dimensionVersion, "at cell birth");
    cell              = store->cellRefOf(cellVersion);
    linkedVersion     = store->setLink(cellVersion, store->homeCell().value(),
                                       dim.dim, DimVector::POS, cell.value());
    coordinator.addStore("history", "primary", store, linkedVersion);
  }

  ~HistoryFixture() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }

  std::string versions(std::string_view from, std::string_view to) const {
    return "##@(" + std::string(from) + ", " + std::string(to) + ")";
  }
};

TEST(VQLHistoryTest, PinFoldsHistoricalTopologyAndLeavesSourceUntouched) {
  HistoryFixture fixture;
  const auto latest = fixture.store->latest();

  const auto results =
      fixture.engine.execute(fixture.versions(fixture.dimensionVersion.str(),
                                              fixture.linkedVersion.str()) +
                             "/d.history-test");

  ASSERT_EQ(results.size(), 1U);
  EXPECT_EQ(fixture.coordinator.arena().textOf(results.front()),
            "at cell birth");
  EXPECT_EQ(fixture.store->latest(), latest);
}

TEST(VQLHistoryTest, PinningCellMapsItsIdentityAcrossHistoricalFolds) {
  HistoryFixture fixture;
  const auto results =
      fixture.engine.execute("##/d.history-test@(" + fixture.cellVersion.str() +
                             ", " + fixture.linkedVersion.str() + ")");

  ASSERT_EQ(results.size(), 2U);
  EXPECT_NE(results[0], results[1]);
  EXPECT_EQ(fixture.coordinator.arena().textOf(results[0]), "at cell birth");
  EXPECT_EQ(fixture.coordinator.arena().textOf(results[1]), "at cell birth");
}

TEST(VQLHistoryTest, RepeatedPinSharesTheSameHistoricalCellIdentity) {
  HistoryFixture fixture;
  const auto results = fixture.engine.execute(
      "##/d.history-test@(" + fixture.linkedVersion.str() + ", " +
      fixture.linkedVersion.str() + ")");

  ASSERT_EQ(results.size(), 2U);
  EXPECT_EQ(results[0], results[1]);
}

TEST(VQLHistoryTest, AncestryRangeEnumeratesOnlyStatesWithTheCell) {
  HistoryFixture fixture;
  const auto results =
      fixture.engine.execute("##@(" + fixture.dimensionVersion.str() + " .. " +
                             fixture.linkedVersion.str() + ")/d.history-test");

  ASSERT_EQ(results.size(), 1U);
  EXPECT_EQ(fixture.coordinator.arena().textOf(results.front()),
            "at cell birth");
}

TEST(VQLHistoryTest, ArithmeticPinsAndMissingVersions) {
  HistoryFixture fixture;
  const auto arithmetic = fixture.engine.execute(
      "##@(@" + fixture.cellVersion.str() + " + 1)/d.history-test");
  ASSERT_EQ(arithmetic.size(), 1U);

  const auto missing = fixture.engine.execute("##@(z999999)");
  EXPECT_TRUE(missing.empty());
}

TEST(VQLHistoryTest, VersionOrderingUsesCausalAncestry) {
  HistoryFixture fixture;
  const auto ancestor =
      fixture.engine.execute("##[@" + fixture.genesis.str() + " < @" +
                             fixture.linkedVersion.str() + "]");
  EXPECT_EQ(ancestor.size(), 1U);

  const auto reverse =
      fixture.engine.execute("##[@" + fixture.linkedVersion.str() + " < @" +
                             fixture.genesis.str() + "]");
  EXPECT_TRUE(reverse.empty());
  const auto bound = fixture.engine.execute(
      "let $before := @" + fixture.genesis.str() + " let $after := @" +
      fixture.linkedVersion.str() + " return ##[$before < $after]");
  EXPECT_EQ(bound.size(), 1U);
}

TEST(VQLHistoryTest, WatchSinceFindsCellsAddedAfterItsHistoricalSnapshot) {
  HistoryFixture fixture;
  const auto query = "watch ##/d.history-test as $cell since @" +
                     fixture.dimensionVersion.str() + " { $cell }";

  const auto changes = fixture.engine.execute(query);
  ASSERT_EQ(changes.size(), 1U);
  EXPECT_EQ(fixture.coordinator.arena().textOf(changes.front()),
            "at cell birth");
}

TEST(VQLHistoryTest,
     StoreProxyMaskPreservesSelectedDimensionWithoutStoreWrites) {
  HistoryFixture fixture;
  const auto latest = fixture.store->latest();

  const auto projected = fixture.engine.execute("##{ !*, +d.history-test }");
  ASSERT_EQ(projected.size(), 1U);
  const auto reached = fixture.engine.execute("##/d.history-test");
  ASSERT_EQ(reached.size(), 1U);
  EXPECT_EQ(fixture.coordinator.arena().textOf(reached.front()),
            "at cell birth");
  EXPECT_EQ(fixture.store->latest(), latest);
}

TEST(VQLHistoryTest, WatchSinceSameTopologyProducesNoSnapshotDelta) {
  HistoryFixture fixture;
  const auto query = "watch ##/d.history-test as $cell since @" +
                     fixture.linkedVersion.str() + " { $cell }";
  EXPECT_TRUE(fixture.engine.execute(query).empty());
}

} // namespace
