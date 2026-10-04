#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>

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
