#include "common/xanadu/bencode.hpp"
#include "common/xanadu/link_package_exchange.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <mutex>
#include <thread>

namespace {
using namespace xanadu;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
using Phase  = LinkPackagePhase;
struct WireState {
  std::mutex guard;
  std::vector<std::thread::id> threads;
  std::atomic<bool> packageAck{}, catalogAck{}, hold{}, fail{}, failPoll{};
  std::map<InfoHash, fs::path> seeds;
  std::vector<std::pair<std::int64_t, InfoHash>> announcements;
  void record() {
    const std::scoped_lock lock(guard);
    threads.push_back(std::this_thread::get_id());
  }
};
class PackagePublisher : public PublicationTransport {
public:
  explicit PackagePublisher(std::shared_ptr<WireState> state)
      : state_(std::move(state)) {
    state_->record();
  }
  ~PackagePublisher() override { state_->record(); }
  void seed(const PublicationSeed &) override { state_->record(); }
  void poll() override {
    if (state_->failPoll) throw std::runtime_error("stopped transport");
  }
  void announce(const Publication &, const InfoHash &) override {
    throw std::runtime_error("document announcement in package test");
  }
  bool acknowledged(const Publication &, const InfoHash &) override {
    return false;
  }
  void announcePackage(const LinkPackage &pkg, const InfoHash &hash) override {
    state_->record();
    const std::scoped_lock lock(state_->guard);
    state_->announcements.emplace_back(pkg.sequence, hash);
  }
  bool packageAcknowledged(const LinkPackage &, const InfoHash &) override {
    state_->record();
    return state_->packageAck;
  }
  bool advertisePackage(const LinkPackage &, const InfoHash &) override {
    state_->record();
    return state_->catalogAck;
  }

private:
  std::shared_ptr<WireState> state_;
};
class PackageDownloader : public PublicationDownloadTransport {
public:
  explicit PackageDownloader(std::shared_ptr<WireState> state)
      : state_(std::move(state)) {
    state_->record();
  }
  ~PackageDownloader() override { state_->record(); }
  MutablePointer resolve(const MutableLink &, std::stop_token) override {
    throw std::runtime_error("package download must use catalog hash");
  }
  void fetch(const InfoHash &hash, const fs::path &directory, std::uint64_t,
             std::stop_token stop) override {
    state_->record();
    while (state_->hold && !stop.stop_requested())
      std::this_thread::sleep_for(5ms);
    if (stop.stop_requested()) throw std::runtime_error("cancelled");
    if (state_->fail) throw std::runtime_error("unavailable peer");
    const std::scoped_lock lock(state_->guard);
    fs::create_directories(directory.parent_path());
    fs::copy(state_->seeds.at(hash), directory, fs::copy_options::recursive);
  }

private:
  std::shared_ptr<WireState> state_;
};
class LinkPackageExchangeTest : public testing::Test {
protected:
  MutableKeys keys = createMutableKeys();
  fs::path root =
      fs::temp_directory_path() / ("xudu-packages-" + keys.publicKey.hex());
  std::shared_ptr<WireState> wire = std::make_shared<WireState>();
  LinkPackage pkg;
  InfoHash hash;
  void SetUp() override {
    const std::vector<TorrentContent> files{
        {.path = "text", .data = "Story Ideas and commentary"}};
    const auto seal = makeTorrent(files, "scroll");
    auto scroll =
        Scroll::ofTorrentFile(seal.hash, 0, "text", 0, files[0].data.size());
    const auto key = scrollKey(scroll);
    GlobalLink link;
    link.owner = "Bob's Commentary";
    link.left  = {{key, 0, 5}, {key, 6, 5}};
    link.right = {{key, 16, 10}};
    pkg = publishLinkPackage(keys, "curations:story-ideas", "Bob's Commentary",
                             1, 1, {link}, {{key, scroll}},
                             {{.publisher = keys.publicKey,
                               .salt      = "doc:commentary",
                               .hash      = seal.hash,
                               .sequence  = 7,
                               .title     = "Bob's Commentary"}});
    reseal(pkg);
  }
  ~LinkPackageExchangeTest() override { fs::remove_all(root); }
  void reseal(LinkPackage package) {
    const std::vector<TorrentContent> files{
        {.path = "links.xanalinks", .data = encodeLinkPackage(package)}};
    const auto torrent = makeTorrent(files, "link-package");
    hash               = torrent.hash;
    wire->seeds[hash]  = writeTorrentSeed(root / "source", torrent, files);
  }
  SignedAuthorCatalog catalog(LinkPackage package) {
    return updateAuthorCatalog(root / "catalog", package, hash, keys);
  }
  LinkPackageExchange::Options options(bool mock          = true,
                                       std::string suffix = "cache") {
    LinkPackageExchange::Options opts;
    opts.directory     = root / suffix;
    opts.retryInterval = 20ms;
    if (mock)
      opts.verifyIdentity = [](const auto &) {
        return PublicationIdentity::MockVerified;
      };
    opts.makePublisher = [wire = wire] {
      return std::make_unique<PackagePublisher>(wire);
    };
    opts.makeDownloader = [wire = wire] {
      return std::make_unique<PackageDownloader>(wire);
    };
    return opts;
  }
  void resign(LinkPackage &package) {
    package.signature =
        signMutableItem(linkPackageSigningBuffer(package), keys);
  }
};
TEST_F(LinkPackageExchangeTest,
       BothAcknowledgementsAndEnrollmentGatePublishingOnWorker) {
  {
    LinkPackageExchange exchange(options());
    const auto id = exchange.submit(pkg, true);
    ASSERT_TRUE(exchange.waitFor(id, Phase::AwaitingDht, 3s));
    wire->packageAck = true;
    EXPECT_FALSE(exchange.waitFor(id, Phase::Published, 80ms));
    wire->catalogAck = true;
    ASSERT_TRUE(exchange.waitFor(id, Phase::Published, 3s));
    EXPECT_EQ(exchange.status(id).hash, hash);
    EXPECT_EQ(exchange.status(id).identity, PublicationIdentity::MockVerified);
  }
  ASSERT_FALSE(wire->threads.empty());
  for (const auto thread : wire->threads) {
    EXPECT_EQ(thread, wire->threads.front());
    EXPECT_NE(thread, std::this_thread::get_id());
  }
  for (const auto &[sequence, rootHash] : wire->announcements) {
    EXPECT_EQ(sequence, 1);
    EXPECT_EQ(rootHash, hash);
  }
  LinkPackageExchange unverified(options(false, "unverified"));
  const auto id = unverified.submit(pkg, true);
  ASSERT_TRUE(unverified.waitFor(id, Phase::NeedsVerification, 3s));
}
TEST_F(LinkPackageExchangeTest,
       StoppedTransportRequiresRetryWithoutChangingSignedSequence) {
  wire->packageAck = true;
  wire->catalogAck = true;
  LinkPackageExchange exchange(options());
  const auto id = exchange.submit(pkg, true);
  ASSERT_TRUE(exchange.waitFor(id, Phase::Published, 3s));
  wire->failPoll = true;
  ASSERT_TRUE(exchange.waitFor(id, Phase::Failed, 3s));
  wire->failPoll = false;
  exchange.retry(id);
  ASSERT_TRUE(exchange.waitFor(id, Phase::Published, 3s));
  EXPECT_EQ(exchange.status(id).hash, hash);
  EXPECT_EQ(exchange.status(id).package.sequence, 1);
}

TEST_F(LinkPackageExchangeTest,
       PreparedPackageCanBeReviewedThenPublishedWithoutNewSequence) {
  std::string id;
  {
    LinkPackageExchange exchange(options());
    id = exchange.submit(pkg, false);
    ASSERT_TRUE(exchange.waitFor(id, Phase::Ready, 3s));
    EXPECT_TRUE(wire->threads.empty());
    EXPECT_EQ(exchange.status(id).package.publications.front().sequence, 7);
  }
  LinkPackageExchange restored(options());
  ASSERT_TRUE(restored.waitFor(id, Phase::Ready, 3s));
  wire->packageAck = true;
  wire->catalogAck = true;
  EXPECT_EQ(restored.submit(pkg, true), id);
  ASSERT_TRUE(restored.waitFor(id, Phase::Published, 3s));
  EXPECT_EQ(restored.status(id).package.sequence, 1);
}
TEST_F(LinkPackageExchangeTest, VerifiedEndpointsAndCitationsReopenOffline) {
  const auto metadata = catalog(pkg);
  std::string id;
  {
    LinkPackageExchange exchange(options());
    id = exchange.fetch(metadata, metadata.entries.front(),
                        linkPackageScrollKeys(pkg));
    ASSERT_TRUE(exchange.waitFor(id, Phase::Ready, 3s))
        << exchange.status(id).error;
    EXPECT_EQ(exchange.status(id).package.links.front().left.size(), 2U);
    EXPECT_EQ(exchange.status(id).package.publications, pkg.publications);
  }
  LinkPackageExchange offline({.directory = root / "cache"});
  EXPECT_EQ(offline.status(id).phase, Phase::Ready);
  EXPECT_EQ(encodeLinkPackage(offline.status(id).package),
            encodeLinkPackage(pkg));
}
TEST_F(LinkPackageExchangeTest,
       FalseCatalogClaimsAndUnrelatedQueriesAreRefused) {
  auto metadata = catalog(pkg);
  LinkPackageExchange exchange(options());
  EXPECT_THROW((void)exchange.fetch(metadata, metadata.entries.front(),
                                    {"other-scroll"}),
               LinkPackageExchangeUnreadable);
  metadata.entries.front().title = "Forged title";
  metadata                       = signAuthorCatalog(metadata, keys);
  const auto id = exchange.fetch(metadata, metadata.entries.front());
  ASSERT_TRUE(exchange.waitFor(id, Phase::Failed, 3s));
  EXPECT_THAT(exchange.status(id).error,
              testing::HasSubstr("advertised identity"));
  EXPECT_FALSE(fs::exists(root / "cache" / id / "package"));
}
TEST_F(LinkPackageExchangeTest,
       CancelledTransferStaysCancelledAcrossRestartAndRetryWorks) {
  wire->hold          = true;
  const auto metadata = catalog(pkg);
  std::string id;
  {
    LinkPackageExchange exchange(options());
    id = exchange.fetch(metadata, metadata.entries.front());
    ASSERT_TRUE(exchange.waitFor(id, Phase::Downloading, 3s));
    exchange.cancel(id);
    EXPECT_EQ(exchange.status(id).phase, Phase::Cancelled);
  }
  wire->hold = false;
  LinkPackageExchange restored(options());
  EXPECT_EQ(restored.status(id).phase, Phase::Cancelled);
  restored.retry(id);
  ASSERT_TRUE(restored.waitFor(id, Phase::Ready, 3s));
}
TEST_F(LinkPackageExchangeTest, PieceCorruptionCannotProduceReadyPackage) {
  const auto metadata = catalog(pkg);
  std::ofstream(wire->seeds.at(hash) / "link-package/links.xanalinks",
                std::ios::app)
      << "junk";
  LinkPackageExchange exchange(options());
  const auto id = exchange.fetch(metadata, metadata.entries.front());
  ASSERT_TRUE(exchange.waitFor(id, Phase::Failed, 3s));
  EXPECT_FALSE(fs::exists(root / "cache" / id / "package"));
}
TEST_F(LinkPackageExchangeTest,
       EndpointDeclarationsAndFormatVersionsAreStrict) {
  auto invalid                              = pkg;
  invalid.links.front().left.front().length = 100;
  resign(invalid);
  EXPECT_THROW(reviewLinkPackage(invalid), LinkPackageUnreadable);
  invalid                                                  = pkg;
  invalid.scrolls.begin()->second.segments.front().torrent = {};
  resign(invalid);
  EXPECT_THROW(reviewLinkPackage(invalid), LinkPackageUnreadable);
  invalid = pkg;
  invalid.scrolls.begin()->second.segments.push_back(
      invalid.scrolls.begin()->second.segments.front());
  resign(invalid);
  EXPECT_THROW(reviewLinkPackage(invalid), LinkPackageUnreadable);
  auto fields      = bencode::decode(encodeLinkPackage(pkg)).asDict();
  fields["format"] = bencode::Value::integer(2);
  EXPECT_THROW((void)decodeLinkPackage(bencode::Value::dict(fields).encode()),
               LinkPackageUnreadable);
  fields.erase("format");
  EXPECT_THROW((void)decodeLinkPackage(bencode::Value::dict(fields).encode()),
               LinkPackageUnreadable);
}
TEST_F(LinkPackageExchangeTest,
       PackageCatalogAndReaderHighWaterMarksRejectRollback) {
  const auto metadata = catalog(pkg);
  EXPECT_EQ(metadata.entries.front().kind, CatalogEntryKind::LinkPackage);
  EXPECT_EQ(metadata.entries.front().scrollKeys, linkPackageScrollKeys(pkg));
  auto next     = pkg;
  next.sequence = 2;
  resign(next);
  reseal(next);
  const auto newer = catalog(next);
  EXPECT_EQ(newer.sequence, 2);
  LinkPackageExchange exchange(options());
  const auto latest = exchange.fetch(newer, newer.entries.front());
  ASSERT_TRUE(exchange.waitFor(latest, Phase::Ready, 3s));
  const auto stale = exchange.fetch(metadata, metadata.entries.front());
  ASSERT_TRUE(exchange.waitFor(stale, Phase::Failed, 3s));
  EXPECT_THAT(exchange.status(stale).error, testing::HasSubstr("rolls back"));
}
TEST_F(LinkPackageExchangeTest,
       DamagedRetainedCacheRefusesRatherThanDiscardingIt) {
  const auto metadata = catalog(pkg);
  std::string id;
  {
    LinkPackageExchange exchange(options());
    id = exchange.fetch(metadata, metadata.entries.front());
    ASSERT_TRUE(exchange.waitFor(id, Phase::Ready, 3s));
  }
  std::ofstream(root / "cache" / id / "package", std::ios::app) << "junk";
  EXPECT_THROW((void)LinkPackageExchange(options()),
               LinkPackageExchangeUnreadable);
}
TEST_F(LinkPackageExchangeTest, RestartDoesNotRepublishSupersededOwnPackage) {
  std::string earlier, later;
  {
    LinkPackageExchange exchange(options());
    earlier = exchange.submit(pkg, false);
    ASSERT_TRUE(exchange.waitFor(earlier, Phase::Ready, 3s));
    auto next     = pkg;
    next.sequence = 2;
    resign(next);
    later = exchange.submit(next, false);
    ASSERT_TRUE(exchange.waitFor(later, Phase::Ready, 3s));
    auto conflict  = next;
    conflict.title = "Other";
    resign(conflict);
    EXPECT_THROW((void)exchange.submit(conflict, false), std::invalid_argument);
  }
  LinkPackageExchange restored(options());
  EXPECT_EQ(restored.status(earlier).phase, Phase::Superseded);
  ASSERT_TRUE(restored.waitFor(later, Phase::Ready, 3s));
}
} // namespace
