/**
 * @file reading_place_test.cpp
 * @brief A reader's place written to the activity store and read back.
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <tuple>
#include <unistd.h>
#include <vector>

#include "common/xanadu/reading_place.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"

namespace xanadu {
// Field by field, so a mismatch says which field rather than dumping bytes.
void PrintTo(const ReadingPlace &place, std::ostream *out) {
  *out << "{documents:";
  for (const auto &doc : place.documents) {
    *out << " [" << doc.storePath << " v" << doc.version << " caret "
         << doc.caret << " anchor " << doc.anchor << "]";
  }
  *out << " active:" << (place.active ? std::to_string(*place.active) : "-")
       << " camera:";
  if (place.camera) {
    for (const auto value : *place.camera) {
      *out << value << ",";
    }
  } else {
    *out << "-";
  }
  *out << " zigzag:" << place.zigzagStore << "@" << place.zigzagVersion << "#"
       << place.zigzagFocus << (place.zigzagHasKeyboard ? " keys" : "")
       << " link:";
  if (place.link) {
    const auto index = [](const std::optional<std::uint32_t> at) {
      return at ? std::to_string(*at) : std::string{"-"};
    };
    const auto &link = *place.link;
    *out << link.key.authority.str() << "#" << link.key.id << " side "
         << static_cast<int>(link.active) << " left " << index(link.left.member)
         << "/" << index(link.left.occurrence) << " right "
         << index(link.right.member) << "/" << index(link.right.occurrence)
         << " origin "
         << (link.origin ? std::to_string(link.origin->value) : "-");
  } else {
    *out << "-";
  }
  *out << "}";
}
} // namespace xanadu

namespace {

namespace fs = std::filesystem;

class ReadingPlaceTest : public ::testing::Test {
protected:
  void SetUp() override {
    root = fs::temp_directory_path() /
           ("reading_place_" + std::to_string(::getpid()) + "_" +
            ::testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(root);
    fs::create_directories(root);
    xanadu::UserPermascroll::Config config;
    config.storageDir = root / "permascroll";
    perma = std::make_shared<xanadu::UserPermascroll>(std::move(config));
  }
  void TearDown() override { fs::remove_all(root); }

  fs::path root;
  std::shared_ptr<xanadu::UserPermascroll> perma;
};

xanadu::ReadingPlace twoDocuments() {
  return {
      .documents =
          {{.storePath = "/data/a", .version = "1a", .caret = 12, .anchor = 12},
           {.storePath = "/data/b", .version = "3", .caret = 40, .anchor = 31}},
      .active            = 1,
      .camera            = std::array<double, 4>{1.5, -2.25, 9.0, 45.0},
      .zigzagStore       = "/data/b",
      .zigzagVersion     = "5",
      .zigzagFocus       = 7,
      .zigzagHasKeyboard = true};
}

TEST_F(ReadingPlaceTest, anEmptyStoreHasNoPlace) {
  const xanadu::Store store(perma);
  EXPECT_FALSE(xanadu::latestPlace(store).has_value());
  EXPECT_TRUE(xanadu::recordedStorePaths(store).empty());
}

TEST_F(ReadingPlaceTest, thePlaceRecordedIsThePlaceReadBack) {
  xanadu::Store store(perma);
  std::ignore = xanadu::recordPlace(store, twoDocuments());
  EXPECT_EQ(xanadu::latestPlace(store), twoDocuments());
}

// Append-only: a later place is read, and the earlier one is still there to
// be walked, which is what the activity store keeps visits for.
TEST_F(ReadingPlaceTest, theNewestPlaceWinsAndSurvivesSaving) {
  xanadu::Store store(perma);
  auto first = twoDocuments();
  first.documents.pop_back();
  first.active = 0;
  first.camera.reset();
  first.zigzagStore.clear();
  first.zigzagVersion.clear();
  first.zigzagFocus       = 0;
  first.zigzagHasKeyboard = false;
  std::ignore             = xanadu::recordPlace(store, first);
  EXPECT_EQ(xanadu::latestPlace(store), first);
  const auto opsAfterFirst = store.opCount();

  std::ignore = xanadu::recordPlace(store, twoDocuments());
  EXPECT_GT(store.opCount(), opsAfterFirst);
  store.save((root / "activity").string());

  xanadu::Store reopened(perma);
  reopened.load((root / "activity").string());
  EXPECT_EQ(xanadu::latestPlace(reopened), twoDocuments());
}

// A link selected and not entered names no visit, so only the place can
// bring it back: the panel the reader left open is part of where they were.
TEST_F(ReadingPlaceTest, theSelectedLinkComesBackWithItsCursors) {
  auto withLink = twoDocuments();
  withLink.link = xanadu::LinkVisitContext{
      .key    = {xanadu::DocumentId{}, 42},
      .active = xanadu::LinkSide::Right,
      .left   = {.member = 1, .occurrence = 0},
      .right  = {.member = 0, .occurrence = std::nullopt},
      .origin = xanadu::VisitId{3}};
  xanadu::Store store(perma);
  std::ignore = xanadu::recordPlace(store, withLink);
  store.save((root / "activity").string());

  xanadu::Store reopened(perma);
  reopened.load((root / "activity").string());
  EXPECT_EQ(xanadu::latestPlace(reopened), withLink);

  // Dismissing it before the next quit leaves the next place without one.
  std::ignore = xanadu::recordPlace(reopened, twoDocuments());
  EXPECT_EQ(xanadu::latestPlace(reopened), twoDocuments());
}

TEST_F(ReadingPlaceTest,
       closedDocumentsKeepIndependentCheckpointsAcrossRestart) {
  xanadu::Store store(perma);
  const auto session = twoDocuments();
  std::ignore        = xanadu::recordPlace(store, session);
  auto closed        = session;
  closed.documents   = {session.documents[1]};
  closed.active      = 0;
  closed.link =
      xanadu::LinkVisitContext{.key    = {xanadu::DocumentId{}, 42},
                               .active = xanadu::LinkSide::Right,
                               .left   = {.member = 1, .occurrence = 0},
                               .right  = {.member = 2, .occurrence = 1},
                               .origin = xanadu::VisitId{3}};
  std::ignore = xanadu::recordClosedPlace(store, closed);
  EXPECT_EQ(xanadu::latestPlace(store), session);
  auto another                        = closed;
  another.documents.front().storePath = "/data/c";
  another.documents.front().caret     = 5;
  std::ignore = xanadu::recordClosedPlace(store, another);
  store.save((root / "activity").string());
  xanadu::Store reopened(perma);
  reopened.load((root / "activity").string());
  EXPECT_EQ(xanadu::closedPlaceFor(reopened, "/data/b"), closed);
  EXPECT_EQ(xanadu::closedPlaceFor(reopened, "/data/c"), another);
  EXPECT_FALSE(xanadu::closedPlaceFor(reopened, "/data/missing"));
  EXPECT_EQ(xanadu::latestPlace(reopened), session);
  closed.documents.front().caret = 20;
  std::ignore                    = xanadu::recordClosedPlace(reopened, closed);
  EXPECT_EQ(xanadu::closedPlaceFor(reopened, "/data/b"), closed);
  EXPECT_EQ(xanadu::closedPlaceFor(reopened, "/data/c"), another);
}

TEST_F(ReadingPlaceTest, recordedPathsIncludeOlderSessionsAndClosedSlices) {
  xanadu::Store store(perma);
  std::ignore = xanadu::recordPlace(store, twoDocuments());
  xanadu::ReadingPlace closed{
      .documents   = {{.storePath = "/data/closed", .version = "1"}},
      .active      = 0,
      .zigzagStore = "/data/slice-only"};
  std::ignore          = xanadu::recordClosedPlace(store, closed);
  std::ignore          = xanadu::recordClosedPlace(store, closed);
  std::ignore          = xanadu::recordPlace(store, xanadu::ReadingPlace{});
  const auto directory = root / "activity";
  store.save(directory.string());
  xanadu::Store reopened(perma);
  reopened.load(directory.string());
  const auto count = reopened.opCount();
  EXPECT_EQ(xanadu::recordedStorePaths(reopened),
            (std::vector<std::string>{"/data/a", "/data/b", "/data/closed",
                                      "/data/slice-only"}));
  EXPECT_EQ(reopened.opCount(), count);
  ASSERT_TRUE(xanadu::latestPlace(reopened));
  EXPECT_TRUE(xanadu::latestPlace(reopened)->documents.empty());
}

} // namespace
