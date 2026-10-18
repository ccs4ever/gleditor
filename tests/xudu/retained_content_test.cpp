#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <unistd.h>

#include "common/xanadu/bencode.hpp"
#include "common/xanadu/resolver.hpp"
#include "common/xanadu/retained_content.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/torrent.hpp"
#include "torrent_data.hpp"

namespace {
namespace fs = std::filesystem;
class RetainedContentTest : public testing::Test {
protected:
  fs::path root;
  xanadu::MadeTorrent torrent;
  std::vector<xanadu::TorrentContent> files{
      {.path = "research", .data = std::string(70000, 'R')},
      {.path = "words", .data = "Alice's words"}};
  void SetUp() override {
    static std::atomic<unsigned> serial{};
    root    = fs::temp_directory_path() /
              ("xudu-retention-" + std::to_string(getpid()) + "-" +
               std::to_string(serial++));
    torrent = xanadu::makeTorrent(files, "source");
    (void)xanadu::writeTorrentSeed(root / "source", torrent, files);
  }
  void TearDown() override { fs::remove_all(root); }
  xanadu::PublicationSeed seed() {
    return xanadu::reviewPublicationSeed(torrent.hash,
                                         root / "source" / torrent.hash.hex());
  }
  xanadu::Scroll scroll() {
    auto result =
        xanadu::Scroll::ofTorrentFile(torrent.hash, 0, "research", 0, 70000);
    result.addSegment({.at           = 70000,
                       .length       = files[1].data.size(),
                       .torrent      = torrent.hash,
                       .streamOffset = 70000,
                       .fileIndex    = 1,
                       .path         = "words"});
    return result;
  }
};
TEST_F(RetainedContentTest,
       CompleteMultiFileCarrierSurvivesSourceRemovalAndReusesCopies) {
  const std::vector scrolls{scroll(), scroll()};
  const auto stats = xanadu::retainScrollSeeds(scrolls, {root / "source"},
                                               root / "destination");
  EXPECT_EQ(stats.copiedSeeds, 1U);
  EXPECT_EQ(stats.copiedBytes, 70000 + files[1].data.size());
  fs::remove_all(root / "source");
  const auto repeated =
      xanadu::retainScrollSeeds(scrolls, {}, root / "destination");
  EXPECT_EQ(repeated.copiedSeeds, 0U);
  EXPECT_EQ(repeated.reusedSeeds, 1U);
  const auto held = xanadu::reviewPublicationSeed(
      torrent.hash, root / "destination" / torrent.hash.hex());
  xanadu::DirectoryContentSource content;
  content.add(held.metainfo, held.savePath.string());
  EXPECT_EQ(content.readStream(torrent.hash, 65530, 20), std::string(20, 'R'));
  EXPECT_EQ(content.readStream(torrent.hash, 70000, files[1].data.size()),
            files[1].data);
}
TEST_F(RetainedContentTest, CorruptExistingDestinationIsPreservedAndRefused) {
  const auto source = seed();
  const auto held = xanadu::retainPublicationSeed(source, root / "destination");
  const auto path = held.savePath / "source/research";
  std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
  file.put('X');
  file.close();
  EXPECT_THROW(
      (void)xanadu::retainPublicationSeed(source, root / "destination"),
      std::runtime_error);
  std::ifstream saved(path);
  EXPECT_EQ(saved.get(), 'X');
}
TEST_F(RetainedContentTest, StagingOwnedByAnotherInvocationIsNotRemoved) {
  const auto stage = root / "destination" / (torrent.hash.hex() + ".partial");
  fs::create_directories(stage);
  std::ofstream(stage / "keep") << "owned elsewhere";
  EXPECT_THROW(
      (void)xanadu::retainPublicationSeed(seed(), root / "destination"),
      std::runtime_error);
  EXPECT_TRUE(fs::exists(stage / "keep"));
  EXPECT_FALSE(fs::exists(root / "destination" / torrent.hash.hex()));
}
TEST_F(RetainedContentTest, SourceAndDestinationPathEscapesAreRefused) {
  const auto source = seed();
  const auto path   = source.savePath / "source/research";
  fs::rename(path, root / "outside");
  fs::create_symlink(root / "outside", path);
  EXPECT_THROW(
      (void)xanadu::retainPublicationSeed(source, root / "destination"),
      std::runtime_error);
  fs::remove(path);
  fs::rename(root / "outside", path);
  fs::remove_all(root / "destination");
  fs::create_directory(root / "outside");
  fs::create_directory_symlink(root / "outside", root / "destination");
  EXPECT_THROW(
      (void)xanadu::retainPublicationSeed(source, root / "destination"),
      std::runtime_error);
  EXPECT_TRUE(fs::is_empty(root / "outside"));
}
TEST_F(RetainedContentTest, SymlinkedDestinationAncestorIsRefused) {
  fs::create_directory(root / "outside");
  fs::create_directory_symlink(root / "outside", root / "link");
  const auto destination = root / "link/published";
  EXPECT_THROW((void)xanadu::retainPublicationSeed(seed(), destination),
               std::runtime_error);
  const std::vector scrolls{scroll()};
  EXPECT_THROW(
      (void)xanadu::retainScrollSeeds(scrolls, {root / "source"}, destination),
      std::runtime_error);
  EXPECT_TRUE(fs::is_empty(root / "outside"));
  const auto parentTraversal = root / "link/../escaped";
  EXPECT_THROW((void)xanadu::retainPublicationSeed(seed(), parentTraversal),
               std::runtime_error);
  EXPECT_FALSE(fs::exists(root / "escaped"));
}
TEST_F(RetainedContentTest,
       MountedMetainfoIsRetainedWithoutAStoredMetadataFile) {
  const auto source = seed();
  fs::remove(source.savePath / "metainfo.torrent");
  const std::vector scrolls{scroll()};
  const auto stats =
      xanadu::retainScrollSeeds(scrolls, {}, root / "destination", {source});
  EXPECT_EQ(stats.copiedSeeds, 1U);
  EXPECT_EQ(xanadu::reviewPublicationSeed(torrent.hash, root / "destination" /
                                                            torrent.hash.hex())
                .bytes,
            70000 + files[1].data.size());
}
TEST_F(RetainedContentTest, FlatMountedMultiFilePayloadIsRetained) {
  auto source = seed();
  source.savePath /= "source";
  const std::vector scrolls{scroll()};
  EXPECT_EQ(
      xanadu::retainScrollSeeds(scrolls, {}, root / "destination", {source})
          .copiedSeeds,
      1U);
}
TEST_F(RetainedContentTest, MountedSingleFileKeepsTheTorrentPayloadLayout) {
  const auto metadata = xudu_test::singleFileTorrent;
  const auto meta     = xanadu::Metainfo::parse(metadata);
  fs::create_directory(root / "raw");
  std::ofstream(root / "raw" / meta.name()) << xudu_test::singleFileText;
  const xanadu::PublicationSeed source{
      .hash = meta.hash(), .metainfo = metadata, .savePath = root / "raw"};
  const auto held = xanadu::retainPublicationSeed(source, root / "destination");
  EXPECT_TRUE(fs::is_regular_file(held.savePath / meta.name()));
  xanadu::DirectoryContentSource content;
  content.add(held.metainfo, held.savePath.string());
  EXPECT_EQ(content.readStream(meta.hash(), 0, meta.totalLength()),
            xudu_test::singleFileText);
}
TEST_F(RetainedContentTest, MetadataSidecarDoesNotHideANestedPayload) {
  const std::vector<xanadu::TorrentContent> payload{
      {.path = "metainfo.torrent", .data = "payload is not metadata"}};
  const auto made = xanadu::makeTorrent(payload, "files");
  const auto path = xanadu::writeTorrentSeed(root / "collision", made, payload);
  const auto source = xanadu::reviewPublicationSeed(made.hash, path);
  const auto held = xanadu::retainPublicationSeed(source, root / "destination");
  EXPECT_EQ(held.bytes, payload.front().data.size());
  EXPECT_TRUE(fs::is_regular_file(held.savePath / "files/metainfo.torrent"));
  xanadu::Store draft;
  const auto version = draft.transcludeExternal(
      {}, 0,
      xanadu::Scroll::ofTorrentFile(made.hash, 0, "metainfo.torrent", 0,
                                    payload.front().data.size()),
      0, payload.front().data.size());
  draft.save((root / "draft").string());
  (void)xanadu::retainPublicationSeed(source, root / "draft/published");
  fs::remove_all(root / "collision");
  xanadu::Store reopened;
  reopened.load((root / "draft").string());
  EXPECT_EQ(reopened.textOf(version), payload.front().data);
}
TEST_F(RetainedContentTest,
       SingleFileSidecarCollisionIsRefusedWithoutOverwriting) {
  auto dictionary =
      xanadu::bencode::decode(xudu_test::singleFileTorrent).asDict();
  auto info          = dictionary.at("info").asDict();
  info["name"]       = xanadu::bencode::Value::string("metainfo.torrent");
  dictionary["info"] = xanadu::bencode::Value::dict(std::move(info));
  const auto metadata =
      xanadu::bencode::Value::dict(std::move(dictionary)).encode();
  const auto meta = xanadu::Metainfo::parse(metadata);
  fs::create_directory(root / "raw");
  const auto payload = root / "raw/metainfo.torrent";
  std::ofstream(payload) << xudu_test::singleFileText;
  const xanadu::PublicationSeed source{
      .hash = meta.hash(), .metainfo = metadata, .savePath = root / "raw"};
  EXPECT_THROW(
      (void)xanadu::retainPublicationSeed(source, root / "destination"),
      std::runtime_error);
  EXPECT_EQ(fs::file_size(payload), xudu_test::singleFileText.size());
  EXPECT_FALSE(fs::exists(root / "destination" / meta.hash().hex()));
}
TEST_F(RetainedContentTest,
       InvalidDescriptorRefusesBeforeAnyCarrierIsCommitted) {
  auto invalid                          = scroll();
  invalid.segments.front().streamOffset = 70001;
  const std::vector scrolls{scroll(), invalid};
  EXPECT_THROW((void)xanadu::retainScrollSeeds(scrolls, {root / "source"},
                                               root / "destination"),
               std::runtime_error);
  EXPECT_FALSE(fs::exists(root / "destination" / torrent.hash.hex()));
}
TEST_F(RetainedContentTest,
       WorkerOnlyReportsTheLatestSaveAndRetriesMissingSources) {
  xanadu::ContentRetention worker;
  const auto failed = worker.queue({scroll()}, {}, root / "destination");
  auto results      = worker.wait();
  ASSERT_EQ(results.size(), 1U);
  EXPECT_EQ(results[0].job, failed);
  EXPECT_FALSE(results[0].outcome);
  std::uint64_t latest{};
  for (int i = 0; i < 16; ++i)
    latest = worker.queue({scroll()}, {root / "source"}, root / "destination");
  results = worker.wait();
  ASSERT_EQ(results.size(), 1U);
  EXPECT_EQ(results[0].job, latest);
  ASSERT_TRUE(results[0].outcome) << results[0].outcome.error();
  const auto notices = worker.takeNotifications();
  ASSERT_EQ(notices.size(), 1U);
  EXPECT_EQ(notices[0].job, latest);
}
} // namespace
