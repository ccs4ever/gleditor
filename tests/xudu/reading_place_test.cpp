/**
 * @file reading_place_test.cpp
 * @brief A reader's place written to the activity store and read back.
 */
#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <ostream>
#include <string>
#include <unistd.h>

#include "common/xanadu/reading_place.hpp"
#include "xudu/core/store.hpp"
#include "xudu/core/user_permascroll.hpp"

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
       << place.zigzagFocus << (place.zigzagHasKeyboard ? " keys" : "") << "}";
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
    xudu::UserPermascroll::Config config;
    config.storageDir = root / "permascroll";
    perma = std::make_shared<xudu::UserPermascroll>(std::move(config));
  }
  void TearDown() override { fs::remove_all(root); }

  fs::path root;
  std::shared_ptr<xudu::UserPermascroll> perma;
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
  const xudu::Store store(perma);
  EXPECT_FALSE(xanadu::latestPlace(store).has_value());
}

TEST_F(ReadingPlaceTest, thePlaceRecordedIsThePlaceReadBack) {
  xudu::Store store(perma);
  static_cast<void>(xanadu::recordPlace(store, twoDocuments()));
  EXPECT_EQ(xanadu::latestPlace(store), twoDocuments());
}

// Append-only: a later place is read, and the earlier one is still there to
// be walked, which is what the activity store keeps visits for.
TEST_F(ReadingPlaceTest, theNewestPlaceWinsAndSurvivesSaving) {
  xudu::Store store(perma);
  auto first = twoDocuments();
  first.documents.pop_back();
  first.active = 0;
  first.camera.reset();
  first.zigzagStore.clear();
  first.zigzagVersion.clear();
  first.zigzagFocus       = 0;
  first.zigzagHasKeyboard = false;
  static_cast<void>(xanadu::recordPlace(store, first));
  EXPECT_EQ(xanadu::latestPlace(store), first);
  const auto opsAfterFirst = store.opCount();

  static_cast<void>(xanadu::recordPlace(store, twoDocuments()));
  EXPECT_GT(store.opCount(), opsAfterFirst);
  store.save((root / "activity").string());

  xudu::Store reopened(perma);
  reopened.load((root / "activity").string());
  EXPECT_EQ(xanadu::latestPlace(reopened), twoDocuments());
}

} // namespace
