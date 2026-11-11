// Version pins and store generators lowered to Vortex (card Q3).
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

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

struct PinnedStore {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("vql_history_vm_" +
       std::to_string(reinterpret_cast<std::uintptr_t>(this)));
  UserPermascroll::Config config = [] {
    UserPermascroll::Config cfg;
    return cfg;
  }();
  std::shared_ptr<UserPermascroll> scroll;
  std::shared_ptr<Store> store;
  MultiStoreCoordinator coordinator;
  VQLEngine engine{coordinator};
  MicroversionId genesis;
  MicroversionId cellVersion;
  MicroversionId linkedVersion;

  PinnedStore() {
    config.storageDir = path / "permascroll";
    scroll            = std::make_shared<UserPermascroll>(config);
    store             = std::make_shared<Store>(scroll);
    genesis           = store->sliceGenesis({});
    const auto dim    = store->makeDimension(genesis, "d.pin-test");
    cellVersion       = store->makeCell(dim.version, "born");
    const auto cell   = store->cellRefOf(cellVersion);
    linkedVersion     = store->setLink(cellVersion, store->homeCell().value(),
                                       dim.dim, DimVector::POS, cell.value());
    coordinator.addStore("pin", "primary", store, linkedVersion);
  }

  ~PinnedStore() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

TEST(VQLHistoryVmTest, AnchorPinReachesTheHistoricalFoldNotTheLiveOne) {
  PinnedStore fixture;
  // Before the link existed the home cell had no neighbour on the dimension.
  EXPECT_TRUE(fixture.engine
                  .execute("##@(" + fixture.cellVersion.str() + ")/d.pin-test")
                  .empty());
  EXPECT_EQ(fixture.engine
                .execute("##@(" + fixture.linkedVersion.str() + ")/d.pin-test")
                .size(),
            1U);
}

TEST(VQLHistoryVmTest, PinningTwoVersionsYieldsTwoDistinctHistoricalHomes) {
  PinnedStore fixture;
  const auto homes =
      fixture.engine.execute("##@(" + fixture.cellVersion.str() + ", " +
                             fixture.linkedVersion.str() + ")");
  ASSERT_EQ(homes.size(), 2U);
  EXPECT_NE(homes[0], homes[1]);
}

TEST(VQLHistoryVmTest, RepeatedAnchorPinSharesOneHistoricalIdentity) {
  PinnedStore fixture;
  const auto homes =
      fixture.engine.execute("##@(" + fixture.linkedVersion.str() + ", " +
                             fixture.linkedVersion.str() + ")");
  ASSERT_EQ(homes.size(), 2U);
  EXPECT_EQ(homes[0], homes[1]);
  const auto again =
      fixture.engine.execute("##@(" + fixture.linkedVersion.str() + ")");
  ASSERT_EQ(again.size(), 1U);
  EXPECT_EQ(again.front(), homes.front());
}

TEST(VQLHistoryVmTest, PinOnTheAnchorLeavesTheLiveStoreAlone) {
  PinnedStore fixture;
  const auto latest = fixture.store->latest();
  (void)fixture.engine.execute("##@(" + fixture.genesis.str() + " .. " +
                               fixture.linkedVersion.str() + ")");
  EXPECT_EQ(fixture.store->latest(), latest);
  const auto live = fixture.engine.execute("##/d.pin-test");
  ASSERT_EQ(live.size(), 1U);
  EXPECT_EQ(fixture.coordinator.arena().textOf(live.front()), "born");
}

TEST(VQLHistoryVmTest, PinOnAPathStepRefusesByName) {
  PinnedStore fixture;
  try {
    (void)fixture.engine.execute("##/d.pin-test@(" +
                                 fixture.linkedVersion.str() + ")");
    FAIL() << "a step pin must be refused";
  } catch (const std::runtime_error &error) {
    EXPECT_THAT(error.what(),
                testing::HasSubstr("version pins on a path step"));
  }
}

TEST(VQLHistoryVmTest, PinWithoutStoreRefusesInsteadOfAnsweringEmpty) {
  zigzag::ArenaManifold arena;
  VQLEngine engine(arena);
  EXPECT_THROW(engine.execute("##@(1)"), std::runtime_error);
  EXPECT_THROW(engine.execute("@1"), std::runtime_error);
}

TEST(VQLHistoryVmTest, GeneratorsOtherThanCurrentRefuseNamingThemselves) {
  zigzag::ArenaManifold arena;
  VQLEngine engine(arena);
  for (const std::string generator : {"local", "slices", "topic", "btpk",
                                      "btih", "citing", "ledger", "system"}) {
    try {
      (void)engine.execute(generator + "(\"missing\")");
      FAIL() << generator << " must be refused";
    } catch (const std::runtime_error &error) {
      EXPECT_THAT(error.what(), testing::HasSubstr(generator)) << generator;
    }
  }
  EXPECT_FALSE(engine.execute("current()").empty());
}

} // namespace
