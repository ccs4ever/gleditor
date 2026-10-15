#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <thread>

#include "common/xanadu/store.hpp"
#include "common/xanadu/store_activity_log.hpp"
#include "common/xanadu/user_permascroll.hpp"

TEST(StoreActivityLogTest, BranchesAndSelectedVisitSurviveReopening) {
  const auto base =
      std::filesystem::temp_directory_path() /
      ("xuzz-activity-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::remove_all(base);
  xanadu::UserPermascroll::Config config;
  config.storageDir    = base / "permascroll";
  auto scroll          = std::make_shared<xanadu::UserPermascroll>(config);
  const auto directory = base / "activity";
  xanadu::DocumentId document;
  xanadu::VisitId origin, first, second;
  {
    xanadu::Store store(scroll);
    xanadu::StoreActivityLog log(&store, directory);
    origin = log.append({.target = xanadu::DocumentSite{document, {}, {4, 4}}});
    first  = log.append({.parent  = origin,
                         .target  = xanadu::CellSite{document, {}, 7, {2, 5}},
                         .arrival = xanadu::Arrival::EnteredEndpoint,
                         .link    = xanadu::LinkVisitContext{
                                .key    = {document, 12},
                                .active = xanadu::LinkSide::Right,
                                .left   = {.member = 0, .occurrence = 0},
                                .right  = {.member = 1, .occurrence = 0},
                                .origin = origin}});
    log.select(origin);
    second = log.append({.parent = origin,
                         .target = xanadu::CellSite{document, {}, 21, {0, 3}}});
    log.select(origin);
  }
  {
    xanadu::Store reopened(scroll);
    reopened.load(directory.string());
    xanadu::StoreActivityLog log(&reopened, directory);
    ASSERT_EQ(log.current(), origin);
    EXPECT_EQ(log.children(origin),
              (std::vector<xanadu::VisitId>{first, second}));
    ASSERT_TRUE(log.find(first));
    EXPECT_EQ(log.find(first)->link->right.member, 1U);
    EXPECT_EQ(std::get<xanadu::CellSite>(log.find(second)->target).cell, 21U);
    log.select(second);
  }
  {
    xanadu::Store reopened(scroll);
    reopened.load(directory.string());
    xanadu::StoreActivityLog log(&reopened, directory);
    EXPECT_EQ(log.current(), second);
  }
  scroll.reset();
  std::filesystem::remove_all(base);
}

TEST(SliceFocusLogTest, EachSlicesLatestRestingPlaceSurvivesReopening) {
  const auto base =
      std::filesystem::temp_directory_path() /
      ("xuzz-focus-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::remove_all(base);
  xanadu::UserPermascroll::Config config;
  config.storageDir    = base / "permascroll";
  auto scroll          = std::make_shared<xanadu::UserPermascroll>(config);
  const auto directory = base / "activity";
  xanadu::DocumentId one;
  xanadu::DocumentId other;
  ASSERT_TRUE(xanadu::DocumentId::fromBytes(std::string(16, '\x01'), one));
  ASSERT_TRUE(xanadu::DocumentId::fromBytes(std::string(16, '\x02'), other));
  {
    xanadu::Store store(scroll);
    xanadu::SliceFocusLog log(&store, directory);
    EXPECT_FALSE(log.last(one));
    ASSERT_TRUE(log.keep(one, 7));
    ASSERT_TRUE(log.keep(other, 9));
    const auto operations = store.opCount();
    ASSERT_TRUE(log.keep(one, 7)) << "a cell already kept writes nothing";
    EXPECT_EQ(store.opCount(), operations);
    ASSERT_TRUE(log.keep(one, 12));
    EXPECT_EQ(log.last(one), 12U);
    ASSERT_TRUE(log.save());
  }
  {
    xanadu::Store reopened(scroll);
    reopened.load(directory.string());
    xanadu::SliceFocusLog log(&reopened, directory);
    EXPECT_EQ(log.last(one), 12U) << "the newest record wins";
    EXPECT_EQ(log.last(other), 9U);
    EXPECT_EQ(log.records(), 3U);
  }
  scroll.reset();
  std::filesystem::remove_all(base);
}

TEST(SliceFocusLogTest, WithNoStoreItKeepsForTheSessionOnly) {
  xanadu::SliceFocusLog log(nullptr, {});
  xanadu::DocumentId slice;
  ASSERT_TRUE(log.keep(slice, 3));
  EXPECT_EQ(log.last(slice), 3U);
}

// M2 third gate, N8: a rest filed 1.4 to 5 ms of work (two folds of the store
// and a save) inside the frame that noticed it.
TEST(SliceFocusLogTest, AKeepAppendsInMemoryAndSaveIsWhatWritesToDisk) {
  const auto base =
      std::filesystem::temp_directory_path() /
      ("xuzz-focus-save-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::remove_all(base);
  xanadu::UserPermascroll::Config config;
  config.storageDir    = base / "permascroll";
  auto scroll          = std::make_shared<xanadu::UserPermascroll>(config);
  const auto directory = base / "activity";
  xanadu::DocumentId slice;
  ASSERT_TRUE(xanadu::DocumentId::fromBytes(std::string(16, '\x07'), slice));
  xanadu::Store store(scroll);
  xanadu::SliceFocusLog log(&store, directory);
  ASSERT_TRUE(log.keep(slice, 1)) << "the first makes the rank, and saves";
  EXPECT_FALSE(log.unsaved());

  const auto began     = std::chrono::steady_clock::now();
  constexpr int kKeeps = 200;
  for (int i = 0; i < kKeeps; ++i) {
    ASSERT_TRUE(log.keep(slice, static_cast<zigzag::CellRef>(2 + i)));
  }
  const auto each = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - began) /
                    kKeeps;
  // Folding the store twice and saving took 1.4 ms and up. Appending three
  // operations is a small fraction of a frame.
  EXPECT_LT(each.count(), 0.5) << "ms per keep";
  EXPECT_TRUE(log.unsaved());
  {
    xanadu::Store onDisk(scroll);
    onDisk.load(directory.string());
    const xanadu::SliceFocusLog seen(&onDisk, directory);
    EXPECT_EQ(seen.last(slice), 1U) << "nothing was written by the keeps";
  }
  ASSERT_TRUE(log.save());
  EXPECT_FALSE(log.unsaved());
  {
    xanadu::Store onDisk(scroll);
    onDisk.load(directory.string());
    const xanadu::SliceFocusLog seen(&onDisk, directory);
    EXPECT_EQ(seen.last(slice), static_cast<zigzag::CellRef>(1 + kKeeps));
    EXPECT_EQ(seen.records(), static_cast<std::size_t>(kKeeps + 1));
  }
  scroll.reset();
  std::filesystem::remove_all(base);
}

// M2 third gate, N8: a visit is filed from the thread a dialog's answer
// arrives on, a resting place from the render thread; both append to the one
// activity store.
TEST(SliceFocusLogTest,
     VisitsAndRestingPlacesFromTwoThreadsDoNotCorruptTheStore) {
  const auto base =
      std::filesystem::temp_directory_path() /
      ("xuzz-focus-race-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::remove_all(base);
  xanadu::UserPermascroll::Config config;
  config.storageDir    = base / "permascroll";
  auto scroll          = std::make_shared<xanadu::UserPermascroll>(config);
  const auto directory = base / "activity";
  xanadu::DocumentId document;
  ASSERT_TRUE(xanadu::DocumentId::fromBytes(std::string(16, '\x05'), document));
  constexpr int kEach = 60;
  {
    xanadu::Store store(scroll);
    xanadu::StoreActivityLog visits(&store, directory);
    xanadu::SliceFocusLog focus(&store, directory);
    ASSERT_TRUE(focus.keep(document, 1));
    std::thread render([&] {
      for (int i = 0; i < kEach; ++i) {
        EXPECT_TRUE(focus.keep(document, static_cast<zigzag::CellRef>(2 + i)));
      }
    });
    for (int i = 0; i < kEach; ++i) {
      visits.append({.target = xanadu::DocumentSite{document, {}, {1, 1}}});
    }
    render.join();
    ASSERT_TRUE(focus.save());
  }
  xanadu::Store reopened(scroll);
  reopened.load(directory.string());
  const xanadu::StoreActivityLog visits(&reopened, directory);
  const xanadu::SliceFocusLog focus(&reopened, directory);
  EXPECT_EQ(focus.records(), static_cast<std::size_t>(kEach + 1));
  EXPECT_EQ(focus.last(document), static_cast<zigzag::CellRef>(1 + kEach));
  EXPECT_TRUE(visits.find(xanadu::VisitId{kEach}));
  scroll.reset();
  std::filesystem::remove_all(base);
}
