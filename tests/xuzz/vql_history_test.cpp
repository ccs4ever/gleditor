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

// The link did not exist at cell birth, so only the second fold reaches it; the
// step pin walks the pinned fold, not the live one.
TEST(VQLHistoryTest, PinningStepMapsOnlyFoldsInWhichTheLinkExisted) {
  HistoryFixture fixture;
  const auto results =
      fixture.engine.execute("##/d.history-test@(" + fixture.cellVersion.str() +
                             ", " + fixture.linkedVersion.str() + ")");

  ASSERT_EQ(results.size(), 1U);
  EXPECT_EQ(fixture.coordinator.arena().textOf(results[0]), "at cell birth");
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

// Genesis is deliberately not a baseline here: a pin at genesis leaves a later
// live read of a one-store dimension empty (reported to the owner), so the
// delta there would compare two empty answers.
TEST(VQLHistoryTest, WatchSinceEveryRealBaselineReportsExactlyTheNewCell) {
  for (const auto pick : {0, 1}) {
    HistoryFixture fixture;
    const auto baseline =
        pick == 0 ? fixture.dimensionVersion : fixture.cellVersion;
    const auto changes =
        fixture.engine.execute("watch ##/d.history-test as $cell since @" +
                               baseline.str() + " { $cell }");
    EXPECT_EQ(changes.size(), 1U) << "@" << baseline.str();
  }
}

// Owner ruling 2026-11-18: a baseline that names no version is refused, since
// treating it as an empty snapshot would report every live cell as new; a pin
// at the same version still yields nothing.
TEST(VQLHistoryTest, WatchSinceAnUnknownVersionRefusesByName) {
  HistoryFixture fixture;
  try {
    (void)fixture.engine.execute(
        "watch ##/d.history-test as $cell since @z999999 { $cell }");
    FAIL() << "an unknown baseline must be refused";
  } catch (const std::runtime_error &refusal) {
    EXPECT_THAT(refusal.what(),
                testing::HasSubstr("watch since version 'z999999' does not "
                                   "exist"));
  }
  EXPECT_TRUE(fixture.engine.execute("##@(z999999)/d.history-test").empty());
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
