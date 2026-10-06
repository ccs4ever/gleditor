#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <lmdb.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

#include "common/xanadu/bencode.hpp"
#include "common/xanadu/publication_discovery.hpp"
#include "common/xanadu/publication_inbox.hpp"
#include "common/xanadu/publication_outbox.hpp"
#include "common/xanadu/store.hpp"

namespace {
using namespace std::chrono_literals;
namespace fs = std::filesystem;

struct TransportState {
  std::mutex guard;
  std::vector<xanadu::InfoHash> seeded;
  std::vector<std::pair<std::int64_t, xanadu::InfoHash>> announcements;
  std::vector<std::thread::id> threads;
  std::atomic<bool> ack{false};
  std::atomic<bool> failSeed{false};
};

class TestTransport final : public xanadu::PublicationTransport {
public:
  explicit TestTransport(std::shared_ptr<TransportState> state)
      : state_(std::move(state)) {
    recordThread();
  }
  ~TestTransport() override { recordThread(); }
  void seed(const xanadu::PublicationSeed &seed) override {
    recordThread();
    if (state_->failSeed.load())
      throw std::runtime_error("injected seed failure");
    const std::scoped_lock lock(state_->guard);
    state_->seeded.push_back(seed.hash);
  }
  void announce(const xanadu::Publication &pub,
                const xanadu::InfoHash &hash) override {
    recordThread();
    const std::scoped_lock lock(state_->guard);
    state_->announcements.emplace_back(pub.sequence, hash);
  }
  bool acknowledged(const xanadu::Publication &,
                    const xanadu::InfoHash &) override {
    recordThread();
    return state_->ack.load();
  }

private:
  void recordThread() {
    const std::scoped_lock lock(state_->guard);
    state_->threads.push_back(std::this_thread::get_id());
  }
  std::shared_ptr<TransportState> state_;
};

class PublicationOutboxTest : public testing::Test {
protected:
  xanadu::MutableKeys keys = xanadu::createMutableKeys();
  fs::path root =
      fs::temp_directory_path() / ("xudu-outbox-" + keys.publicKey.hex());
  std::shared_ptr<TransportState> transport =
      std::make_shared<TransportState>();
  xanadu::Publication publication;
  std::vector<fs::path> roots{root / "story", root / "research"};

  void SetUp() override {
    const std::vector<xanadu::TorrentContent> files{
        {.path = "notes", .data = "Research Ideas"}};
    const auto research = xanadu::makeTorrent(files, "research");
    (void)xanadu::writeTorrentSeed(roots[1], research, files);
    xanadu::Store store;
    const auto text   = store.insert({}, 0, "Story Ideas");
    const auto scroll = xanadu::Scroll::ofTorrentFile(research.hash, 0, "notes",
                                                      0, files[0].data.size());
    (void)store.transcludeExternal(text, 11, scroll, 0, files[0].data.size());
    const auto genesis = store.sliceGenesis(text);
    (void)store.makeCell(genesis, "Idea cell");
    const xanadu::SignedProvenance provenance{.tsv       = "test record",
                                              .signature = "test signature"};
    const auto seal = xanadu::sealLocalSpool(store, keys, "permascroll",
                                             roots[0].string(), provenance);
    publication =
        xanadu::publish(store, text, keys, "doc:story-ideas", "Story Ideas", 1,
                        1, &seal.scroll, {*seal.opsSegment}, {}, {"ideas"});
  }
  ~PublicationOutboxTest() override { fs::remove_all(root); }
  xanadu::PublicationOutbox::Options options(bool mock = true) const {
    xanadu::PublicationOutbox::Options options;
    options.directory     = root / "outbox";
    options.retryInterval = 50ms;
    if (mock)
      options.verifyIdentity = [](const auto &) {
        return xanadu::PublicationIdentity::MockVerified;
      };
    options.makeTransport = [state = transport] {
      return std::make_unique<TestTransport>(state);
    };
    return options;
  }
  xanadu::PublicationJobStatus status(xanadu::PublicationOutbox &outbox,
                                      const std::string &id) {
    for (const auto &item : outbox.statuses())
      if (item.id == id) return item;
    ADD_FAILURE() << "job is absent";
    return {};
  }
};

TEST_F(PublicationOutboxTest, ReviewsHistorySliceAndExternalDependencies) {
  const auto seeds = xanadu::reviewPublicationDependencies(publication, roots);
  ASSERT_EQ(seeds.size(), 2U);
  for (const auto &seed : seeds) EXPECT_GT(seed.bytes, 0U);
  EXPECT_THROW(
      (void)xanadu::reviewPublicationDependencies(publication, {roots[0]}),
      std::runtime_error);
}

TEST_F(PublicationOutboxTest, LocalPreparationDoesNotAnnounce) {
  xanadu::PublicationOutbox outbox(options());
  const auto id = outbox.submit(publication, roots, false);
  ASSERT_TRUE(outbox.waitFor(id, xanadu::PublicationPhase::LocalReady, 2s));
  const auto job = status(outbox, id);
  EXPECT_EQ(job.dependencyCount, 2U);
  EXPECT_FALSE(job.manifestHash.isZero());
  const std::scoped_lock lock(transport->guard);
  EXPECT_TRUE(transport->seeded.empty());
  EXPECT_TRUE(transport->announcements.empty());
}

TEST_F(PublicationOutboxTest,
       NoRemoteClaimWithoutVerificationAndAcknowledgement) {
  xanadu::PublicationOutbox outbox(options(false));
  const auto id = outbox.submit(publication, roots, true);
  ASSERT_TRUE(
      outbox.waitFor(id, xanadu::PublicationPhase::NeedsVerification, 2s));
  const std::scoped_lock lock(transport->guard);
  EXPECT_TRUE(transport->seeded.empty());
  EXPECT_TRUE(transport->announcements.empty());
}

TEST_F(PublicationOutboxTest,
       TransportLifecycleStaysOnWorkerAndCompletionWaitsForAck) {
  {
    xanadu::PublicationOutbox outbox(options());
    const auto id = outbox.submit(publication, roots, true);
    ASSERT_TRUE(outbox.waitFor(id, xanadu::PublicationPhase::AwaitingDht, 2s));
    EXPECT_FALSE(outbox.waitFor(id, xanadu::PublicationPhase::Published, 60ms));
    EXPECT_EQ(status(outbox, id).identity,
              xanadu::PublicationIdentity::MockVerified);
    transport->ack.store(true);
    ASSERT_TRUE(outbox.waitFor(id, xanadu::PublicationPhase::Published, 2s));
    const auto hash = status(outbox, id).manifestHash;
    const std::scoped_lock lock(transport->guard);
    EXPECT_EQ(transport->seeded.size(), 3U);
    for (const auto &[sequence, announced] : transport->announcements) {
      EXPECT_EQ(sequence, 1);
      EXPECT_EQ(announced, hash);
    }
  }
  const std::scoped_lock lock(transport->guard);
  ASSERT_FALSE(transport->threads.empty());
  EXPECT_NE(transport->threads.front(), std::this_thread::get_id());
  for (const auto &thread : transport->threads)
    EXPECT_EQ(thread, transport->threads.front());
}

TEST_F(PublicationOutboxTest,
       CorruptSeedCannotBeOfferedAndCanBeRetriedAfterRepair) {
  const auto hash = publication.opsSegments.front().torrent;
  const auto file = roots[0] / hash.hex() / "permascroll" / "primedia";
  std::ifstream in(file, std::ios::binary);
  const std::string original{std::istreambuf_iterator<char>(in),
                             std::istreambuf_iterator<char>()};
  in.close();
  {
    std::ofstream out(file, std::ios::binary);
    out << std::string(original.size(), 'x');
  }
  xanadu::PublicationOutbox outbox(options());
  const auto id = outbox.submit(publication, roots, true);
  ASSERT_TRUE(outbox.waitFor(id, xanadu::PublicationPhase::Failed, 2s));
  EXPECT_THAT(status(outbox, id).error,
              testing::HasSubstr("failed verification"));
  {
    const std::scoped_lock lock(transport->guard);
    EXPECT_TRUE(transport->seeded.empty());
  }
  {
    std::ofstream out(file, std::ios::binary);
    out << original;
  }
  transport->ack.store(true);
  outbox.retry(id);
  ASSERT_TRUE(outbox.waitFor(id, xanadu::PublicationPhase::Published, 2s));
  EXPECT_EQ(status(outbox, id).sequence, 1);
}

TEST_F(PublicationOutboxTest,
       QueuedPublicationSurvivesRestartAndReseedsItsClosure) {
  std::string id;
  xanadu::InfoHash manifest;
  {
    xanadu::PublicationOutbox outbox(options());
    id = outbox.submit(publication, roots, true);
    ASSERT_TRUE(outbox.waitFor(id, xanadu::PublicationPhase::AwaitingDht, 2s));
    manifest = status(outbox, id).manifestHash;
  }
  transport->ack.store(true);
  xanadu::PublicationOutbox reopened(options());
  ASSERT_TRUE(reopened.waitFor(id, xanadu::PublicationPhase::Published, 2s));
  EXPECT_EQ(status(reopened, id).manifestHash, manifest);
  EXPECT_EQ(status(reopened, id).sequence, 1);
}

TEST_F(PublicationOutboxTest,
       SeedLocationsFromAnEarlierStoreRemainAvailableAfterRestart) {
  {
    xanadu::PublicationOutbox outbox(options());
    const auto id = outbox.submit(publication, roots, false);
    ASSERT_TRUE(outbox.waitFor(id, xanadu::PublicationPhase::LocalReady, 2s));
  }
  publication.sequence  = 2;
  publication.signature = xanadu::signMutableItem(
      xanadu::publicationSigningBuffer(publication), keys);
  xanadu::PublicationOutbox reopened(options());
  const auto id = reopened.submit(publication, {roots[0]}, false);
  ASSERT_TRUE(reopened.waitFor(id, xanadu::PublicationPhase::LocalReady, 2s));
  EXPECT_EQ(status(reopened, id).dependencyCount, 2U);
}

TEST_F(PublicationOutboxTest,
       DuplicateSubmissionIsIdempotentAndConflictingSequenceIsRefused) {
  transport->ack.store(true);
  xanadu::PublicationOutbox outbox(options());
  const auto id = outbox.submit(publication, roots, true);
  ASSERT_TRUE(outbox.waitFor(id, xanadu::PublicationPhase::Published, 2s));
  EXPECT_EQ(outbox.submit(publication, roots, true), id);
  EXPECT_EQ(status(outbox, id).phase, xanadu::PublicationPhase::Published);
  auto different      = publication;
  different.title     = "Different Ideas";
  different.signature = xanadu::signMutableItem(
      xanadu::publicationSigningBuffer(different), keys);
  EXPECT_THROW((void)outbox.submit(different, roots, true),
               std::invalid_argument);
}

TEST_F(PublicationOutboxTest,
       TopicsAreCanonicalAndSignedAndUnknownFormatsAreRefused) {
  EXPECT_EQ(xanadu::publicationTopics(" Ideas, writing,IDEAS, "),
            (std::vector<std::string>{"ideas", "writing"}));
  auto altered = publication;
  altered.topics.push_back("forged");
  EXPECT_FALSE(xanadu::verifyPublication(altered));
  auto dictionary =
      xanadu::bencode::decode(xanadu::encodePublication(publication)).asDict();
  dictionary["format"] =
      xanadu::bencode::Value::integer(xanadu::publicationFormatVersion + 1);
  EXPECT_THROW((void)xanadu::decodePublication(
                   xanadu::bencode::Value::dict(dictionary).encode()),
               xanadu::PublicationUnreadable);
}

TEST_F(PublicationOutboxTest, AValidSignatureCannotHideAnIncompleteInventory) {
  publication.inventory.clear();
  publication.signature = xanadu::signMutableItem(
      xanadu::publicationSigningBuffer(publication), keys);
  EXPECT_THROW((void)xanadu::reviewPublicationDependencies(publication, roots),
               xanadu::PublicationUnreadable);
}

TEST_F(PublicationOutboxTest, ASeedSymlinkCannotEscapeTheVerifiedDirectory) {
  const auto hash = publication.opsSegments.front().torrent;
  const auto file = roots[0] / hash.hex() / "permascroll" / "primedia";
  fs::rename(file, root / "outside");
  fs::create_symlink(root / "outside", file);
  EXPECT_THROW((void)xanadu::reviewPublicationDependencies(publication, roots),
               std::runtime_error);
}
TEST_F(PublicationOutboxTest,
       AValidSignatureCannotHideInvalidSegmentCoordinates) {
  publication.scrolls.begin()->second.segments.front().fileIndex = UINT32_MAX;
  publication.signature = xanadu::signMutableItem(
      xanadu::publicationSigningBuffer(publication), keys);
  EXPECT_THROW((void)xanadu::reviewPublicationDependencies(publication, roots),
               std::runtime_error);
}

TEST_F(PublicationOutboxTest, AValidSignatureCannotHideAnUnsealedSpan) {
  publication.pieces.front().start = UINT64_MAX - 1;
  publication.signature            = xanadu::signMutableItem(
      xanadu::publicationSigningBuffer(publication), keys);
  EXPECT_THROW((void)xanadu::reviewPublicationDependencies(publication, roots),
               std::runtime_error);
}

TEST_F(PublicationOutboxTest, SeedFailureCanBeRetriedWithTheSameVersion) {
  transport->failSeed.store(true);
  xanadu::PublicationOutbox outbox(options());
  const auto id = outbox.submit(publication, roots, true);
  ASSERT_TRUE(outbox.waitFor(id, xanadu::PublicationPhase::Failed, 2s));
  EXPECT_EQ(status(outbox, id).error, "injected seed failure");
  transport->failSeed.store(false);
  transport->ack.store(true);
  outbox.retry(id);
  ASSERT_TRUE(outbox.waitFor(id, xanadu::PublicationPhase::Published, 2s));
  EXPECT_EQ(status(outbox, id).sequence, 1);
}

TEST_F(PublicationOutboxTest, TheSeedDataDirectoryCannotEscapeThroughASymlink) {
  const auto seed  = fs::directory_iterator(roots[0])->path();
  const auto data  = seed / "permascroll";
  const auto moved = root / "outside-data";
  fs::rename(data, moved);
  fs::create_directory_symlink(moved, data);
  EXPECT_THROW((void)xanadu::reviewPublicationDependencies(publication, roots),
               std::runtime_error);
}

struct DownloadState {
  std::mutex guard;
  std::map<xanadu::InfoHash, fs::path> seeds;
  xanadu::MutablePointer pointer;
  std::vector<std::thread::id> threads;
  std::atomic<bool> hold{false};
  std::atomic<bool> fail{false};
};

class TestDownloadTransport final
    : public xanadu::PublicationDownloadTransport {
public:
  explicit TestDownloadTransport(std::shared_ptr<DownloadState> state)
      : state_(std::move(state)) {
    record();
  }
  ~TestDownloadTransport() override { record(); }
  xanadu::MutablePointer resolve(const xanadu::MutableLink &,
                                 std::stop_token stop) override {
    record();
    while (state_->hold && !stop.stop_requested())
      std::this_thread::sleep_for(5ms);
    if (stop.stop_requested()) throw std::runtime_error("cancelled");
    if (state_->fail) throw std::runtime_error("injected network failure");
    return state_->pointer;
  }
  void fetch(const xanadu::InfoHash &hash, const fs::path &directory,
             std::uint64_t, std::stop_token) override {
    record();
    fs::create_directories(directory.parent_path());
    fs::copy(state_->seeds.at(hash), directory, fs::copy_options::recursive);
  }

private:
  void record() {
    const std::scoped_lock lock(state_->guard);
    state_->threads.push_back(std::this_thread::get_id());
  }
  std::shared_ptr<DownloadState> state_;
};

class PublicationInboxTest : public PublicationOutboxTest {
protected:
  std::shared_ptr<DownloadState> downloads = std::make_shared<DownloadState>();
  xanadu::MutableLink link;
  void SetUp() override {
    PublicationOutboxTest::SetUp();
    const std::vector<xanadu::TorrentContent> files{
        {.path = "publication.xanadoc",
         .data = xanadu::encodePublication(publication)}};
    const auto manifest = xanadu::makeTorrent(files, "publication");
    const auto directory =
        xanadu::writeTorrentSeed(root / "manifest", manifest, files);
    downloads->pointer              = {.hash     = manifest.hash,
                                       .sequence = publication.sequence};
    downloads->seeds[manifest.hash] = directory;
    for (const auto &seed :
         xanadu::reviewPublicationDependencies(publication, roots))
      downloads->seeds[seed.hash] = seed.savePath;
    link.key  = keys.publicKey;
    link.salt = publication.salt;
  }
  xanadu::PublicationInbox::Options incomingOptions() {
    xanadu::PublicationInbox::Options result;
    result.directory     = root / "inbox";
    result.makeTransport = [state = downloads] {
      return std::make_unique<TestDownloadTransport>(state);
    };
    return result;
  }
};

TEST_F(PublicationInboxTest,
       CompleteStoreRetainsSignedSnapshotAndReopensOffline) {
  std::string id;
  {
    xanadu::PublicationInbox inbox(incomingOptions());
    id = inbox.submit(link);
    ASSERT_TRUE(inbox.waitFor(id, xanadu::PublicationDownloadPhase::Ready, 3s))
        << inbox.status(id).error;
    const auto status = inbox.status(id);
    EXPECT_EQ(status.completedDependencies, 2U);
    EXPECT_EQ(status.title, "Story Ideas");
    xanadu::Store restored;
    restored.load(status.storePath.string());
    EXPECT_EQ(restored.documentId(), publication.storeId);
    EXPECT_EQ(restored.textOf(status.version), "Story Ideas");
    EXPECT_TRUE(restored.userPermascrollPtr()->bytes().empty());
  }
  xanadu::PublicationInbox offline({.directory = root / "inbox"});
  ASSERT_EQ(offline.statuses().size(), 1U);
  EXPECT_EQ(offline.status(id).phase, xanadu::PublicationDownloadPhase::Ready);
  EXPECT_EQ(offline.status(id).version, publication.version);
  const std::scoped_lock lock(downloads->guard);
  ASSERT_FALSE(downloads->threads.empty());
  for (const auto thread : downloads->threads) {
    EXPECT_NE(thread, std::this_thread::get_id());
    EXPECT_EQ(thread, downloads->threads.front());
  }
}

TEST_F(PublicationInboxTest, RejectsDifferentKeySaltAndPointerSequence) {
  xanadu::PublicationInbox inbox(incomingOptions());
  auto wrong = link;
  wrong.key  = xanadu::createMutableKeys().publicKey;
  auto id    = inbox.submit(wrong);
  ASSERT_TRUE(inbox.waitFor(id, xanadu::PublicationDownloadPhase::Failed, 3s));
  EXPECT_FALSE(fs::exists(root / "inbox" / id));
  wrong      = link;
  wrong.salt = "doc:another";
  id         = inbox.submit(wrong);
  ASSERT_TRUE(inbox.waitFor(id, xanadu::PublicationDownloadPhase::Failed, 3s));
  ++downloads->pointer.sequence;
  id = inbox.submit(link);
  ASSERT_TRUE(inbox.waitFor(id, xanadu::PublicationDownloadPhase::Failed, 3s));
  EXPECT_THAT(inbox.status(id).error, testing::HasSubstr("signed DHT pointer"));
}

TEST_F(PublicationInboxTest, CancelsActiveWaitAndRetriesTheSameRequest) {
  downloads->hold = true;
  xanadu::PublicationInbox inbox(incomingOptions());
  const auto id = inbox.submit(link);
  ASSERT_TRUE(
      inbox.waitFor(id, xanadu::PublicationDownloadPhase::Resolving, 2s));
  inbox.cancel(id);
  ASSERT_TRUE(
      inbox.waitFor(id, xanadu::PublicationDownloadPhase::Cancelled, 2s));
  EXPECT_FALSE(fs::exists(root / "inbox" / id));
  downloads->hold = false;
  inbox.retry(id);
  ASSERT_TRUE(inbox.waitFor(id, xanadu::PublicationDownloadPhase::Ready, 3s))
      << inbox.status(id).error;
  EXPECT_THROW(inbox.retry(id), std::logic_error);
}

TEST_F(PublicationInboxTest, NetworkFailureLeavesNoStoreAndAllowsRetry) {
  downloads->fail = true;
  xanadu::PublicationInbox inbox(incomingOptions());
  const auto id = inbox.submit(link);
  ASSERT_TRUE(inbox.waitFor(id, xanadu::PublicationDownloadPhase::Failed, 2s));
  EXPECT_FALSE(fs::exists(root / "inbox" / id));
  downloads->fail = false;
  inbox.retry(id);
  ASSERT_TRUE(inbox.waitFor(id, xanadu::PublicationDownloadPhase::Ready, 3s));
}

TEST_F(PublicationInboxTest,
       RejectsCorruptedDependencyAndConfiguredResourceLimits) {
  const auto dependency =
      xanadu::reviewPublicationDependencies(publication, roots).front();
  const auto meta = xanadu::Metainfo::parse(dependency.metainfo);
  const auto payload =
      dependency.savePath / meta.name() / meta.files().front().path;
  std::fstream damaged(payload,
                       std::ios::in | std::ios::out | std::ios::binary);
  damaged.put('!');
  damaged.close();
  xanadu::PublicationInbox inbox(incomingOptions());
  auto id = inbox.submit(link);
  ASSERT_TRUE(inbox.waitFor(id, xanadu::PublicationDownloadPhase::Failed, 3s));
  EXPECT_THAT(inbox.status(id).error, testing::HasSubstr("verification"));
  auto limited                = incomingOptions();
  limited.directory           = root / "limited";
  limited.maximumDependencies = 1;
  xanadu::PublicationInbox tooMany(limited);
  id = tooMany.submit(link);
  ASSERT_TRUE(
      tooMany.waitFor(id, xanadu::PublicationDownloadPhase::Failed, 3s));
  EXPECT_THAT(tooMany.status(id).error,
              testing::HasSubstr("too many dependencies"));
  limited.directory            = root / "small";
  limited.maximumManifestBytes = 1;
  xanadu::PublicationInbox tooLarge(limited);
  id = tooLarge.submit(link);
  ASSERT_TRUE(
      tooLarge.waitFor(id, xanadu::PublicationDownloadPhase::Failed, 3s));
  EXPECT_THAT(tooLarge.status(id).error, testing::HasSubstr("byte limit"));
}

TEST_F(PublicationInboxTest,
       RejectsDependencyBudgetAndInvalidRetainedSnapshots) {
  auto configured                   = incomingOptions();
  configured.maximumDependencyBytes = 1;
  xanadu::PublicationInbox limited(configured);
  const auto id = limited.submit(link);
  ASSERT_TRUE(
      limited.waitFor(id, xanadu::PublicationDownloadPhase::Failed, 3s));
  EXPECT_THAT(limited.status(id).error, testing::HasSubstr("byte limit"));
  EXPECT_FALSE(fs::exists(root / "inbox" / id));
  fs::create_directories(root / "broken" / keys.publicKey.hex());
  std::ofstream(root / "broken" / keys.publicKey.hex() / "publication.xanadoc")
      << "bad";
  EXPECT_THROW((xanadu::PublicationInbox{{.directory = root / "broken"}}),
               std::runtime_error);
}

TEST_F(PublicationInboxTest, ShutdownCancelsWorkerAndQueuedRequests) {
  downloads->hold  = true;
  const auto start = std::chrono::steady_clock::now();
  {
    xanadu::PublicationInbox inbox(incomingOptions());
    const auto id = inbox.submit(link);
    ASSERT_TRUE(
        inbox.waitFor(id, xanadu::PublicationDownloadPhase::Resolving, 2s));
    (void)inbox.submit(link);
  }
  EXPECT_LT(std::chrono::steady_clock::now() - start, 2s);
  EXPECT_TRUE(fs::is_empty(root / "inbox"));
}

class PublicationOutboxNetworkTest : public PublicationOutboxTest {};

TEST_F(PublicationOutboxNetworkTest,
       AuthorKeyAndTopicDiscoveryOpenFromEmptyReaderProfiles) {
  const auto host            = std::getenv("XUDU_PEER_HOST");
  const auto port            = std::getenv("XUDU_PEER_PORT");
  const auto publisherHost   = std::getenv("XUDU_TEST_HOST");
  const auto readerHost      = std::getenv("XUDU_READER_HOST");
  const auto topicHost       = std::getenv("XUDU_DISCOVERY_HOST");
  const auto readerNamespace = std::getenv("XUDU_PEER_NAMESPACE");
  if (!host || !port || !publisherHost || !readerNamespace || !readerHost ||
      !topicHost)
    GTEST_SKIP() << "run make test/publication-swarm";
  xanadu::SwarmContentSource::Options network;
  network.listenInterfaces               = std::string(publisherHost) + ":0";
  network.enableLocalDiscovery           = false;
  network.enableTrackers                 = false;
  network.restrictDhtToDistinctNetworks  = false;
  network.allowManyConnectionsPerAddress = true;
  network.dhtPacketsPerSecond            = 100;
  auto configured                        = options();
  configured.makeTransport               = xanadu::publicationSwarmTransport(
      keys, network, {{host, static_cast<std::uint16_t>(std::stoul(port))}},
      root / "catalog");
  xanadu::PublicationOutbox publisher(configured);
  const auto id = publisher.submit(publication, roots, true);
  ASSERT_TRUE(publisher.waitFor(id, xanadu::PublicationPhase::Published, 60s))
      << status(publisher, id).error;
  const auto quote = [](const std::string &value) {
    std::string result{"'"};
    for (char ch : value) result += ch == '\'' ? "'\\''" : std::string(1, ch);
    return result + "'";
  };
  const auto evidence =
      fs::current_path() / "build/publication-discovery/network-ui";
  fs::create_directories(evidence);
  for (const bool author : {true, false}) {
    const std::string name = author ? "bob" : "carl";
    const auto profile     = root / name;
    const auto logPath     = evidence / (name + ".log");
    const auto script =
        author ? " --chord Ctrl+Shift+D --chord Right --chord Tab --type " +
                     quote(keys.publicKey.hex()) + " --chord Tab --chord Return"
               : " --chord F3 --type Ideas --chord Return";
    // Only Alice's key is typed for Bob. Carl supplies just the topic. No
    // manifest, publication magnet, cache or direct BT peer is supplied.
    const auto command =
        "ip netns exec " + quote(readerNamespace) +
        " env SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy "
        "LIBGL_ALWAYS_SOFTWARE=1" +
        " XDG_DATA_HOME=" + quote((profile / "data").string()) +
        " XDG_CONFIG_HOME=" + quote((profile / "config").string()) +
        " XDG_CACHE_HOME=" + quote((profile / "cache").string()) +
        " ./build/xuzz " + quote((profile / "workspace").string()) +
        " --permascroll " + quote((profile / "permascroll").string()) +
        " --backend opengl --profile --test-publication-swarm " +
        quote(std::string(author ? readerHost : topicHost) + ":0") +
        " --dht-node " + quote(std::string(host) + ":" + port) + script +
        " --dump-a11y --capture " +
        quote((evidence / (name + "-queued.ppm")).string()) +
        " --wait-ms 35000 --chord Return --dump-a11y --capture " +
        quote((evidence / (name + "-catalog.ppm")).string()) +
        " --chord Right --chord Return --wait-ms 35000 --chord Return "
        "--dump-a11y --capture " +
        quote((evidence / (name + "-download.ppm")).string()) +
        " --chord Right --chord Return --dump-a11y --capture " +
        quote((evidence / (name + "-opened.ppm")).string()) + " > " +
        quote(logPath.string()) + " 2>&1";
    ASSERT_EQ(std::system(command.c_str()), 0)
        << std::ifstream(logPath).rdbuf();
    std::ifstream output(logPath);
    const std::string log{std::istreambuf_iterator<char>(output),
                          std::istreambuf_iterator<char>()};
    EXPECT_THAT(log, testing::HasSubstr("signed metadata received"));
    EXPECT_THAT(
        log, testing::HasSubstr("opened downloaded publication Story Ideas"));
    xanadu::PublicationDiscovery offline(
        {.directory = profile / "data/xudu/publication-discovery"});
    ASSERT_EQ(offline.cachedCatalogs().size(), 1U);
    EXPECT_EQ(offline.cachedCatalogs().front().publisher, keys.publicKey);
    EXPECT_EQ(offline.cachedCatalogs().front().entries.front().hash,
              status(publisher, id).manifestHash);
    EXPECT_EQ(offline.followedAuthors().size(), author ? 1U : 0U);
    const auto copies =
        fs::directory_iterator(profile / "data/xudu/publication-inbox");
    ASSERT_NE(copies, fs::directory_iterator{});
    xanadu::Store reader(std::make_shared<xanadu::UserPermascroll>());
    reader.load((copies->path() / "store").string());
    EXPECT_EQ(reader.documentId(), publication.storeId);
    EXPECT_EQ(reader.opCount(), publication.opsSegments.front().length);
    EXPECT_EQ(reader.textOf(publication.version), "Story Ideas");
    EXPECT_EQ(reader.userPermascroll().spool().size(), 0U);
  }
}

TEST_F(PublicationOutboxNetworkTest,
       RemoteDhtAcknowledgesAndRetainsTheSignedPointer) {
  const auto host = std::getenv("XUDU_PEER_HOST");
  const auto port = std::getenv("XUDU_PEER_PORT");
  if (!host || !port || !std::getenv("XUDU_TEST_HOST") ||
      !std::getenv("XUDU_PEER_NAMESPACE"))
    GTEST_SKIP() << "run make test/swarm for separate network stacks";
  xanadu::SwarmContentSource::Options network;
  network.listenInterfaces = std::string(std::getenv("XUDU_TEST_HOST")) + ":0";
  network.enableLocalDiscovery           = false;
  network.enableTrackers                 = false;
  network.restrictDhtToDistinctNetworks  = false;
  network.allowManyConnectionsPerAddress = true;
  network.dhtPacketsPerSecond            = 100;
  auto configured                        = options();
  configured.retryInterval               = 2s;
  configured.makeTransport               = xanadu::publicationSwarmTransport(
      keys, network, {{host, static_cast<std::uint16_t>(std::stoul(port))}},
      root / "author-catalog");
  xanadu::InfoHash expected;
  std::string id;
  {
    xanadu::PublicationOutbox outbox(configured);
    id = outbox.submit(publication, roots, true);
    ASSERT_TRUE(outbox.waitFor(id, xanadu::PublicationPhase::Published, 60s))
        << xanadu::publicationPhaseName(status(outbox, id).phase) << ": "
        << status(outbox, id).error;
    const auto job = status(outbox, id);
    expected       = job.manifestHash;
    EXPECT_EQ(job.identity, xanadu::PublicationIdentity::MockVerified);
    EXPECT_GT(job.attempts, 0U);
  }
  // The publisher has stopped: its own DHT cache cannot answer this lookup.
  xanadu::SwarmContentSource reader(network);
  reader.addDhtNode(host, static_cast<std::uint16_t>(std::stoul(port)));
  const auto link =
      xanadu::MutableLink::parse("magnet:?xs=urn:btpk:" + keys.publicKey.hex() +
                                 "&s=" + xanadu::toHex(publication.salt));
  const auto pointer = reader.resolveMutable(link, 30s);
  ASSERT_TRUE(pointer.has_value());
  EXPECT_EQ(pointer->hash, expected);
  EXPECT_EQ(pointer->sequence, publication.sequence);
  xanadu::PublicationOutbox reopened(configured);
  ASSERT_TRUE(reopened.waitFor(id, xanadu::PublicationPhase::Published, 60s));
  EXPECT_EQ(status(reopened, id).manifestHash, expected);
  const auto hash = reader.addMagnet("magnet:?xt=urn:btih:" + expected.hex(),
                                     (root / "download").string());
  reader.connectPeer(hash, std::getenv("XUDU_TEST_HOST"),
                     reopened.listenPort());
  ASSERT_TRUE(reader.waitForMetadata(hash, 30s));
  const auto meta = reader.metainfo(hash);
  ASSERT_TRUE(meta.has_value());
  const auto wire     = reader.readStream(hash, 0, meta->totalLength());
  const auto received = xanadu::decodePublication(wire);
  ASSERT_TRUE(received.has_value());
  EXPECT_EQ(received->publisher, keys.publicKey);
  EXPECT_EQ(received->topics, (std::vector<std::string>{"ideas"}));
  EXPECT_EQ(xanadu::encodePublication(*received),
            xanadu::encodePublication(publication));
  // A fresh process in the other namespace starts with an empty cache. Its
  // only inputs are the author publication link and a named DHT bootstrap node.
  const auto readerNamespace = std::getenv("XUDU_PEER_NAMESPACE");
  const auto publisherHost   = std::getenv("XUDU_TEST_HOST");
  const auto readerHost      = std::getenv("XUDU_READER_HOST");
  ASSERT_NE(readerHost, nullptr);
  ASSERT_NE(readerNamespace, nullptr);
  ASSERT_NE(publisherHost, nullptr);
  const auto cache  = root / "remote-reader";
  const auto report = root / "remote-reader.txt";
  const auto quote  = [](const std::string_view value) {
    std::string quoted{"'"};
    for (const char ch : value)
      quoted += ch == '\'' ? "'\\''" : std::string(1, ch);
    return quoted + "'";
  };
  const auto command =
      std::string("ip netns exec ") + quote(readerNamespace) +
      " ./build/xudu-swarm-peer --download-publication " + quote(link.uri()) +
      " " + quote(host) + " " + quote(port) + " " + quote(cache.string()) +
      " " + quote(readerHost) + " > " + quote(report.string()) + " 2>&1";
  ASSERT_EQ(std::system(command.c_str()), 0) << std::ifstream(report).rdbuf();
  std::ifstream restoredReport(report);
  std::string line;
  std::getline(restoredReport, line);
  EXPECT_EQ(line, "restored " + keys.publicKey.hex() + " 1 " +
                      std::to_string(publication.opsSegments.front().length) +
                      " " + std::to_string(publication.inventory.size()) +
                      " 0");
  // The child and its BitTorrent session have exited. Reopening consults only
  // the reader's retained cache and deployment metadata, across no sockets.
  xanadu::Store offline(std::make_shared<xanadu::UserPermascroll>());
  const auto copies = fs::directory_iterator(cache / "inbox");
  ASSERT_NE(copies, fs::directory_iterator{});
  offline.load((copies->path() / "store").string());
  EXPECT_EQ(offline.documentId(), publication.storeId);
  EXPECT_EQ(offline.textOf(publication.version), "Story Ideas");
  EXPECT_EQ(offline.opCount(), publication.opsSegments.front().length);
  // The keyboard path uses no incoming cache fixture: the running publisher
  // is reached by DHT peer discovery, while --wait-ms stands in for waiting.
  const auto evidence =
      fs::current_path() / "build/publication-download/network-ui";
  fs::create_directories(evidence);
  const auto uiLog  = evidence / "download.log";
  const auto uiRoot = root / "keyboard-reader";
  const auto uiCommand =
      std::string("ip netns exec ") + quote(readerNamespace) +
      " env SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy "
      "LIBGL_ALWAYS_SOFTWARE=1" +
      " XDG_DATA_HOME=" + quote((uiRoot / "data").string()) +
      " XDG_CONFIG_HOME=" + quote((uiRoot / "config").string()) +
      " XDG_CACHE_HOME=" + quote((uiRoot / "cache").string()) +
      " ./build/xuzz " + quote((uiRoot / "workspace").string()) +
      " --permascroll " + quote((uiRoot / "permascroll").string()) +
      " --backend opengl --profile --test-publication-swarm " +
      quote(std::string(readerHost) + ":0") + " --dht-node " +
      quote(std::string(host) + ":" + port) +
      " --chord Ctrl+O --chord Tab --type " + quote(link.uri()) +
      " --dump-a11y --capture " + quote((evidence / "input.ppm").string()) +
      " --chord Return" + " --dump-a11y --capture " +
      quote((evidence / "queued.ppm").string()) +
      " --wait-ms 30000 --chord Return --dump-a11y --capture " +
      quote((evidence / "ready.ppm").string()) +
      " --chord Right --chord Return --dump-a11y --capture " +
      quote((evidence / "opened.ppm").string()) + " > " +
      quote(uiLog.string()) + " 2>&1";
  ASSERT_EQ(std::system(uiCommand.c_str()), 0) << std::ifstream(uiLog).rdbuf();
  std::ifstream uiOutput(uiLog);
  const std::string log{std::istreambuf_iterator<char>(uiOutput),
                        std::istreambuf_iterator<char>()};
  EXPECT_THAT(log, testing::HasSubstr("Ready to open"));
  EXPECT_THAT(log,
              testing::HasSubstr("opened downloaded publication Story Ideas"));
  const auto inbox = uiRoot / "data/xudu/publication-inbox";
  ASSERT_TRUE(fs::exists(inbox));
  const auto uiCopies = fs::directory_iterator(inbox);
  ASSERT_NE(uiCopies, fs::directory_iterator{});
  xanadu::Store keyboardReader;
  keyboardReader.load((uiCopies->path() / "store").string());
  EXPECT_EQ(keyboardReader.documentId(), publication.storeId);
  EXPECT_EQ(keyboardReader.opCount(), publication.opsSegments.front().length);
  EXPECT_EQ(keyboardReader.textOf(publication.version), "Story Ideas");
}

} // namespace
