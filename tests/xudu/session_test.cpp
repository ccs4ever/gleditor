#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "xudu/core/microversion.hpp"
#include "xudu/core/scroll.hpp"
#include "xudu/core/store.hpp"

namespace fs = std::filesystem;

class StoreMultiStoreTest : public testing::Test {
protected:
  fs::path testDir;

  void SetUp() override {
    testDir =
        fs::temp_directory_path() /
        ("xudu_multistore_test_" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(testDir);
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(testDir, ec);
  }

  fs::path writeSampleFile(const std::string &filename,
                           const std::string &content) {
    const auto filePath = testDir / filename;
    std::ofstream out(filePath);
    out << content;
    return filePath;
  }
};

TEST_F(StoreMultiStoreTest, primaryStoreCreationAndAccess) {
  const auto mainStorePath = (testDir / "main.xanadoc").string();
  xudu::Store store;
  store.load(mainStorePath);

  EXPECT_EQ(store.opCount(), 0U);
  EXPECT_EQ(store.latest(), xudu::MicroversionId{});
}

TEST_F(StoreMultiStoreTest, independentStoresForMultipleSourceFiles) {
  // Primary store
  const auto mainStorePath = (testDir / "main.xanadoc").string();
  xudu::Store store1;
  store1.load(mainStorePath);

  const auto ver1 =
      store1.insert(xudu::MicroversionId{}, 0, "First document content.");
  EXPECT_EQ(ver1.str(), "1");
  EXPECT_EQ(store1.opCount(), 1U);
  store1.save(mainStorePath);

  // Temporary auxiliary store 1
  const auto temp1Path = (testDir / "temp1.xanadoc").string();
  xudu::Store store2;
  store2.load(temp1Path);
  const auto ver2 = store2.insert(xudu::MicroversionId{}, 0,
                                  "Second document content from file 2.");
  EXPECT_EQ(ver2.str(),
            "1"); // Starts cleanly at state 1 in its own ops scroll!
  EXPECT_EQ(store2.opCount(), 1U);
  store2.save(temp1Path);

  // Temporary auxiliary store 2
  const auto temp2Path = (testDir / "temp2.xanadoc").string();
  xudu::Store store3;
  store3.load(temp2Path);
  const auto ver3 = store3.insert(xudu::MicroversionId{}, 0,
                                  "Third document content from file 3.");
  EXPECT_EQ(ver3.str(), "1"); // Also starts at state 1 in its own ops scroll!
  EXPECT_EQ(store3.opCount(), 1U);
  store3.save(temp2Path);

  // Verify materialization of all three stores
  EXPECT_EQ(store1.rebuild(ver1).materialize(store1),
            "First document content.");
  EXPECT_EQ(store2.rebuild(ver2).materialize(store2),
            "Second document content from file 2.");
  EXPECT_EQ(store3.rebuild(ver3).materialize(store3),
            "Third document content from file 3.");
}

TEST_F(StoreMultiStoreTest, preserveTemporaryStoreToPermanentDirectory) {
  const auto tempPath = (testDir / "temp_scratch.xanadoc").string();
  xudu::Store tempStore;
  tempStore.load(tempPath);

  const auto ver = tempStore.insert(xudu::MicroversionId{}, 0,
                                    "Notes typed in temporary store.");
  tempStore.save(tempPath);
  EXPECT_TRUE(fs::exists(tempPath));

  // Preserve to permanent directory
  const auto permPath = (testDir / "my_project" / "preserved.xanadoc").string();
  fs::create_directories(fs::path(permPath).parent_path());
  tempStore.save(permPath);

  EXPECT_TRUE(fs::exists(permPath));

  // Reload preserved store and verify integrity
  xudu::Store preservedStore;
  preservedStore.load(permPath);
  EXPECT_EQ(preservedStore.opCount(), 1U);
  EXPECT_EQ(preservedStore.rebuild(ver).materialize(preservedStore),
            "Notes typed in temporary store.");
}

TEST_F(StoreMultiStoreTest, storeHypertimeHistoryAndTraversal) {
  const auto storePath = (testDir / "scrub_test.xanadoc").string();
  xudu::Store store;
  store.load(storePath);

  // Setup sequential microversions in store
  const auto v1 = store.insert(xudu::MicroversionId{}, 0, "Initial");
  const auto v2 = store.insert(v1, 7, " Version");
  const auto v3 = store.insert(v2, 15, " Three");

  const auto hist = v3.path();
  ASSERT_GE(hist.size(), 3U);
  EXPECT_EQ(hist[0], v1);
  EXPECT_EQ(hist[1], v2);
  EXPECT_EQ(hist[2], v3);

  // Rebuilding text at each step
  EXPECT_EQ(store.textOf(v1), "Initial");
  EXPECT_EQ(store.textOf(v2), "Initial Version");
  EXPECT_EQ(store.textOf(v3), "Initial Version Three");
}

// Every hole used to paint as redactionColour, so a reader could not tell an
// author's embargo from a court order, or from a span that simply has not
// been sealed yet. The reason was carried all the way through in holeRecord
// and then discarded at the last step.
TEST(HoleRenderingTest, EachHoleReasonHasItsOwnColour) {
  const auto colourFor = &xudu::colourForHole;

  const auto withheld = colourFor(xudu::HoleReason::Withheld);
  const auto revoked  = colourFor(xudu::HoleReason::Revoked);
  const auto takedown = colourFor(xudu::HoleReason::Takedown);
  const auto unsealed = colourFor(xudu::HoleReason::Unsealed);

  EXPECT_EQ(withheld, xudu::kWithheldColour);
  EXPECT_NE(revoked, withheld);
  EXPECT_NE(takedown, withheld);
  EXPECT_NE(unsealed, withheld);
  EXPECT_NE(revoked, takedown) << "withdrawn and taken down are not the same";

  // Transcopyright is its own case, not folded into withheld.
  EXPECT_NE(colourFor(xudu::HoleReason::TranscopyrightLock), withheld);
}
