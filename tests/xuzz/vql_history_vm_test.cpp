// Version pins and store generators lowered to Vortex (card Q3).
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/vql/vql_engine.hpp"

namespace xanadu::vql {
// Declared in compile_history.cpp; belongs in compiler.hpp.
bool compareVersionsCausally(CompOp op, const MicroversionId &lhs,
                             const MicroversionId &rhs);
} // namespace xanadu::vql

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
      ("vql_history_vm_" + UserPermascroll::generateUniqueDeviceId());
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
  const auto ops    = fixture.store->opCount();
  (void)fixture.engine.execute("##@(" + fixture.genesis.str() + " .. " +
                               fixture.linkedVersion.str() + ")");
  EXPECT_EQ(fixture.store->latest(), latest);
  EXPECT_EQ(fixture.store->opCount(), ops);
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

TEST(VQLHistoryVmTest, PinAtAnUnknownVersionYieldsNothing) {
  PinnedStore fixture;
  EXPECT_TRUE(fixture.engine.execute("##@(z999999)").empty());
  EXPECT_TRUE(fixture.engine.execute("##@(z999999)/d.pin-test").empty());
}

TEST(VQLHistoryVmTest, SecondPinOfTheSameFoldAttachesNothingNew) {
  PinnedStore fixture;
  const auto query = "##@(" + fixture.linkedVersion.str() + ")";
  (void)fixture.engine.execute(query);
  const auto attached = fixture.coordinator.arena().spaceCount();
  (void)fixture.engine.execute(query);
  EXPECT_EQ(fixture.coordinator.arena().spaceCount(), attached);
}

TEST(VQLHistoryVmTest, RefusalBeforeAnyFoldLeavesNoSpaceBehind) {
  PinnedStore fixture;
  const auto attached = fixture.coordinator.arena().spaceCount();
  EXPECT_THROW(fixture.engine.execute("##/d.pin-test@(" +
                                      fixture.linkedVersion.str() + ")"),
               std::runtime_error);
  EXPECT_EQ(fixture.coordinator.arena().spaceCount(), attached);
}

TEST(VQLHistoryVmTest, TwoPinsInOneQueryEachKeepTheirOwnAnchor) {
  PinnedStore fixture;
  const auto results = fixture.engine.execute(
      "let $early := ##@(" + fixture.cellVersion.str() + ") return ##@(" +
      fixture.linkedVersion.str() + ")/d.pin-test");
  ASSERT_EQ(results.size(), 1U);
  EXPECT_EQ(fixture.coordinator.arena().textOf(results.front()), "born");
}

TEST(VQLHistoryVmTest, PinInsideAPredicateOfAPinnedPath) {
  PinnedStore fixture;
  const auto results =
      fixture.engine.execute("##@(" + fixture.linkedVersion.str() + ")[##@(" +
                             fixture.cellVersion.str() + ")]/d.pin-test");
  ASSERT_EQ(results.size(), 1U);
  EXPECT_EQ(fixture.coordinator.arena().textOf(results.front()), "born");
}

TEST(VQLHistoryVmTest, VersionArithmeticAndBranchesReachTheValue) {
  PinnedStore fixture;
  EXPECT_EQ(fixture.engine
                .execute("##[@" + fixture.cellVersion.str() + " + 1 = @" +
                         fixture.linkedVersion.str() + "]")
                .size(),
            1U);
  EXPECT_EQ(fixture.engine
                .execute("##[@" + fixture.linkedVersion.str() + " - 1 = @" +
                         fixture.cellVersion.str() + "]")
                .size(),
            1U);
  EXPECT_EQ(fixture.engine.execute("##[@2/\"a\" = @2a1]").size(), 1U);
  EXPECT_EQ(fixture.engine.execute("##[@2/\"b\" = @2a1]").size(), 0U);
}

TEST(VQLHistoryVmTest, PinFoldChargesTheWorkBudget) {
  PinnedStore fixture;
  fixture.coordinator.arena().setExecutionLimits(1'000'000, 3, 1'000'000);
  const auto result =
      fixture.engine.tryExecute("##@(" + fixture.linkedVersion.str() + ")");
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().category, xanadu::vql::QueryErrorCategory::Budget);
}

TEST(VQLHistoryVmTest, CausalOrderingDisagreesWithTextAndIntegerOrder) {
  using xanadu::vql::compareVersionsCausally;
  using xanadu::vql::CompOp;
  const auto id = [](const char *text) { return MicroversionId::parse(text); };
  // "9" precedes "10" in history although "10" sorts first as text.
  EXPECT_TRUE(compareVersionsCausally(CompOp::LessThan, id("9"), id("10")));
  EXPECT_FALSE(compareVersionsCausally(CompOp::LessThan, id("10"), id("9")));
  EXPECT_TRUE(compareVersionsCausally(CompOp::GreaterThan, id("10"), id("9")));
  // Siblings are unordered although "2a1" sorts before "2b1" as text.
  for (const auto op :
       {CompOp::LessThan, CompOp::LessEqual, CompOp::GreaterThan,
        CompOp::GreaterEqual, CompOp::Equal}) {
    EXPECT_FALSE(compareVersionsCausally(op, id("2a1"), id("2b1")));
    EXPECT_FALSE(compareVersionsCausally(op, id("2b1"), id("2a1")));
  }
  EXPECT_TRUE(compareVersionsCausally(CompOp::NotEqual, id("2a1"), id("2b1")));
  EXPECT_TRUE(compareVersionsCausally(CompOp::LessThan, id("2"), id("2a1")));
  EXPECT_TRUE(compareVersionsCausally(CompOp::LessEqual, id("2a1"), id("2a1")));
  EXPECT_FALSE(compareVersionsCausally(CompOp::LessThan, id("2a1"), id("2a1")));
  EXPECT_TRUE(compareVersionsCausally(CompOp::GreaterEqual, id("3"), id("3")));
}

} // namespace
