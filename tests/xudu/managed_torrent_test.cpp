/**
 * @file managed_torrent_test.cpp
 * @brief Unit tests for SystemTorrentManager.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include <xudu/core/managed_torrent.hpp>
#include <xudu/core/merkle_ledger.hpp>

namespace xudu {
namespace {

namespace fs = std::filesystem;
using ::testing::Eq;
using ::testing::Ne;

class ManagedTorrentTest : public ::testing::Test {
protected:
  void SetUp() override {
    testDir = fs::temp_directory_path() / "gleditor_managed_torrent_test";
    std::error_code ec;
    fs::remove_all(testDir, ec);
    fs::create_directories(testDir, ec);
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(testDir, ec);
  }

  fs::path testDir;
};

TEST_F(ManagedTorrentTest, RegisterAndSeedMerkleLedger) {
  SystemTorrentManager::Options opts;
  opts.cacheRoot                      = testDir.string();
  opts.enableDht                      = false;
  opts.enableLsd                      = false;
  opts.enableTrackers                 = false;
  opts.restrictDhtToDistinctNetworks  = false;
  opts.allowManyConnectionsPerAddress = true;

  SystemTorrentManager manager(opts);

  MerkleLedger ledger;
  GpgKeyLink link;
  link.fingerprint = "1111222233334444555566667777888899990000";
  link.identity    = "Ada Lovelace";
  link.email       = "ada@example.org";
  link.timestamp   = 1700000000;
  ledger.appendKey(link);

  std::string error;
  const InfoHash hash =
      manager.registerLedger(ledger, std::nullopt, "identity_ledger", &error);
  EXPECT_FALSE(hash.isZero());
  EXPECT_TRUE(error.empty());

  const auto status = manager.getStatus(hash);
  ASSERT_TRUE(status.has_value());
  EXPECT_THAT(status->role, Eq(TorrentRole::SystemLedger));
  EXPECT_THAT(status->state, Eq(ManagedTorrentState::Seeding));
  EXPECT_THAT(status->name, Eq("identity_ledger"));

  const auto list = manager.listTorrents();
  EXPECT_THAT(list.size(), Eq(1U));

  // Verify created files in cache directory
  const std::string savePath = manager.cacheDirFor(hash);
  EXPECT_TRUE(fs::exists(savePath + "/LEDGER.yaml"));
  EXPECT_TRUE(fs::exists(savePath + "/ROOT.hex"));
}

TEST_F(ManagedTorrentTest, RegisterSpoolAndSlice) {
  SystemTorrentManager::Options opts;
  opts.cacheRoot                      = testDir.string();
  opts.enableDht                      = false;
  opts.enableLsd                      = false;
  opts.enableTrackers                 = false;
  opts.allowManyConnectionsPerAddress = true;

  SystemTorrentManager manager(opts);

  // 1. Create a dummy spool file
  const fs::path spoolFile = testDir / "sample.spool";
  {
    std::ofstream out(spoolFile);
    out << "Sample Xanadoc primedia content for spool verification.";
  }

  std::string error;
  const InfoHash spoolHash =
      manager.registerSpool(spoolFile.string(), "sample_spool", &error);
  EXPECT_FALSE(spoolHash.isZero());
  EXPECT_TRUE(error.empty());

  // 2. Create a dummy slice YAML file
  const fs::path sliceFile = testDir / "slice.yaml";
  {
    std::ofstream out(sliceFile);
    out << "meta:\n  name: TestSlice\n  author: Ada\ncells:\n  1:\n    text: "
           "Hello\n";
  }

  const InfoHash sliceHash =
      manager.registerSlice(sliceFile.string(), "slice.yaml", &error);
  EXPECT_FALSE(sliceHash.isZero());
  EXPECT_TRUE(error.empty());

  EXPECT_THAT(manager.listTorrents().size(), Eq(2U));

  const auto spoolStatus = manager.getStatus(spoolHash);
  ASSERT_TRUE(spoolStatus.has_value());
  EXPECT_THAT(spoolStatus->role, Eq(TorrentRole::DocumentSpool));

  const auto sliceStatus = manager.getStatus(sliceHash);
  ASSERT_TRUE(sliceStatus.has_value());
  EXPECT_THAT(sliceStatus->role, Eq(TorrentRole::SliceCache));
}

TEST_F(ManagedTorrentTest, PauseResumeAndRemove) {
  SystemTorrentManager::Options opts;
  opts.cacheRoot                      = testDir.string();
  opts.enableDht                      = false;
  opts.enableLsd                      = false;
  opts.enableTrackers                 = false;
  opts.allowManyConnectionsPerAddress = true;

  SystemTorrentManager manager(opts);

  MerkleLedger ledger;
  GpgKeyLink link;
  link.fingerprint = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
  link.identity    = "Test User";
  link.email       = "user@test.org";
  ledger.appendKey(link);

  const InfoHash hash = manager.registerLedger(ledger);
  EXPECT_FALSE(hash.isZero());

  EXPECT_TRUE(manager.pauseTorrent(hash));
  auto status = manager.getStatus(hash);
  ASSERT_TRUE(status.has_value());
  EXPECT_THAT(status->state, Eq(ManagedTorrentState::Paused));

  EXPECT_TRUE(manager.resumeTorrent(hash));
  status = manager.getStatus(hash);
  ASSERT_TRUE(status.has_value());
  EXPECT_THAT(status->state, Eq(ManagedTorrentState::Seeding));

  EXPECT_TRUE(manager.removeTorrent(hash, true));
  EXPECT_FALSE(manager.getStatus(hash).has_value());
  EXPECT_THAT(manager.listTorrents().size(), Eq(0U));
}

} // namespace
} // namespace xudu
