#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <lmdb.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <thread>

#include "common/xanadu/bencode.hpp"
#include "common/xanadu/link_package_exchange.hpp"
#include "common/xanadu/publication_discovery.hpp"
#include "common/xanadu/publication_inbox.hpp"
#include "common/xanadu/publication_outbox.hpp"
#include "common/xanadu/publication_subscriptions.hpp"
#include "common/xanadu/reader_link_packages.hpp"
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
  std::unique_ptr<xanadu::Store> authored;
  std::vector<fs::path> roots{root / "story", root / "research"};

  void SetUp() override {
    const std::vector<xanadu::TorrentContent> files{
        {.path = "notes", .data = "Research Ideas"}};
    const auto research = xanadu::makeTorrent(files, "research");
    (void)xanadu::writeTorrentSeed(roots[1], research, files);
    authored          = std::make_unique<xanadu::Store>();
    auto &store       = *authored;
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
  std::atomic<bool> failFetch{false};
  std::atomic<bool> holdFetch{false};
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
    const std::scoped_lock lock(state_->guard);
    return state_->pointer;
  }
  void fetch(const xanadu::InfoHash &hash, const fs::path &directory,
             std::uint64_t, std::stop_token stop) override {
    record();
    while (state_->holdFetch && !stop.stop_requested())
      std::this_thread::sleep_for(5ms);
    if (stop.stop_requested()) throw std::runtime_error("fetch cancelled");
    if (state_->failFetch)
      throw std::runtime_error("injected transfer failure");
    const std::scoped_lock lock(state_->guard);
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

TEST_F(PublicationInboxTest, PinnedSnapshotDoesNotResolveNewerMutableHead) {
  xanadu::PublicationPin pin{.publisher = keys.publicKey,
                             .salt      = publication.salt,
                             .hash      = downloads->pointer.hash,
                             .sequence  = publication.sequence,
                             .version   = publication.version,
                             .title     = publication.title};
  downloads->fail             = true;
  downloads->pointer.sequence = 99;
  xanadu::PublicationInbox inbox(incomingOptions());
  const auto id = inbox.submitPinned(pin);
  ASSERT_TRUE(inbox.waitFor(id, xanadu::PublicationDownloadPhase::Ready, 3s))
      << inbox.status(id).error;
  EXPECT_EQ(inbox.status(id).version, pin.version);
  EXPECT_EQ(inbox.status(id).sequence, 1);
  pin.version      = {};
  const auto wrong = inbox.submitPinned(pin);
  ASSERT_TRUE(
      inbox.waitFor(wrong, xanadu::PublicationDownloadPhase::Failed, 3s));
  EXPECT_FALSE(fs::exists(inbox.status(wrong).storePath / "ops.nodes"));
  pin.version      = publication.version;
  pin.sequence     = 2;
  const auto stale = inbox.submitPinned(pin);
  ASSERT_TRUE(
      inbox.waitFor(stale, xanadu::PublicationDownloadPhase::Failed, 3s));
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

class PublicationSubscriptionsTest : public PublicationInboxTest {
protected:
  xanadu::PublicationSubscriptions::Options
  subscriptionOptions(xanadu::PublicationInbox &inbox) {
    return {.directory     = root / "subscriptions",
            .inbox         = &inbox,
            .makeTransport = incomingOptions().makeTransport,
            .pollInterval  = 50ms,
            .retryInterval = 1s};
  }
  std::string baseline(xanadu::PublicationInbox &inbox) {
    const auto id = inbox.submit(link);
    if (!inbox.waitFor(id, xanadu::PublicationDownloadPhase::Ready, 3s))
      throw std::runtime_error(inbox.status(id).error);
    return id;
  }
  void revision(std::int64_t sequence, bool wrongPublisher = false,
                std::string title = "Story Ideas") {
    const auto version = authored->insert(publication.version, 11, " revised");
    const auto seal    = xanadu::sealLocalSpool(
        *authored, keys, "permascroll", roots[0].string(),
        {.tsv = "test record", .signature = "mock"});
    publication = xanadu::publish(
        *authored, version, keys, publication.salt, std::move(title), sequence,
        sequence, &seal.scroll, {*seal.opsSegment}, {}, {"ideas"});
    if (wrongPublisher) {
      const auto other      = xanadu::createMutableKeys();
      publication.publisher = other.publicKey;
      publication.signature = xanadu::signMutableItem(
          xanadu::publicationSigningBuffer(publication), other);
    }
    const std::vector<xanadu::TorrentContent> files{
        {.path = "publication.xanadoc",
         .data = xanadu::encodePublication(publication)}};
    const auto manifest = xanadu::makeTorrent(files, "publication");
    const auto directory =
        xanadu::writeTorrentSeed(root / "manifest", manifest, files);
    const std::scoped_lock lock(downloads->guard);
    downloads->seeds[manifest.hash] = directory;
    for (const auto &seed :
         xanadu::reviewPublicationDependencies(publication, roots))
      downloads->seeds[seed.hash] = seed.savePath;
    downloads->pointer = {.hash = manifest.hash, .sequence = sequence};
  }
};

TEST_F(PublicationSubscriptionsTest,
       VerifiesUpdatesOnceAndRetainsThePinnedEarlierStore) {
  xanadu::PublicationInbox inbox(incomingOptions());
  const auto first          = baseline(inbox);
  const auto earlierVersion = publication.version;
  xanadu::PublicationSubscriptions updates(subscriptionOptions(inbox));
  const auto id = updates.subscribe(first);
  EXPECT_EQ(updates.subscribe(first), id);
  revision(2);
  updates.checkNow(id);
  ASSERT_TRUE(updates.waitForSequence(id, 2, 3s)) << updates.status(id).error;
  const auto notices = updates.takeNotifications();
  ASSERT_EQ(notices.size(), 1U);
  EXPECT_EQ(notices.front().second.previousVersion, earlierVersion);
  EXPECT_EQ(notices.front().second.version, publication.version);
  EXPECT_EQ(notices.front().second.previousSequence, 1);
  EXPECT_TRUE(updates.takeNotifications().empty());
  updates.checkNow(id);
  EXPECT_FALSE(updates.waitForSequence(id, 3, 150ms));
  EXPECT_EQ(updates.status(id).notices.size(), 1U);
  xanadu::Store earlier, current;
  earlier.load(inbox.status(first).storePath.string());
  current.load(
      inbox.status(notices.front().second.snapshotId).storePath.string());
  EXPECT_EQ(earlier.textOf(earlierVersion), "Story Ideas");
  EXPECT_EQ(current.textOf(publication.version), "Story Ideas revised");
  EXPECT_EQ(current.userPermascroll().spool().size(), 0U);
  updates.acknowledge(id, 2);
  EXPECT_TRUE(updates.status(id).notices.front().acknowledged);
  const std::scoped_lock lock(downloads->guard);
  for (const auto thread : downloads->threads)
    EXPECT_NE(thread, std::this_thread::get_id());
}

TEST_F(PublicationSubscriptionsTest,
       OfflineUpdateAndDeliveryAcknowledgementSurviveRestart) {
  std::string id;
  {
    xanadu::PublicationInbox inbox(incomingOptions());
    xanadu::PublicationSubscriptions updates(subscriptionOptions(inbox));
    id = updates.subscribe(baseline(inbox));
  }
  revision(2);
  {
    xanadu::PublicationInbox inbox(incomingOptions());
    xanadu::PublicationSubscriptions updates(subscriptionOptions(inbox));
    ASSERT_TRUE(updates.waitForSequence(id, 2, 3s)) << updates.status(id).error;
    ASSERT_EQ(updates.takeNotifications().size(), 1U);
  }
  {
    xanadu::PublicationInbox inbox(incomingOptions());
    xanadu::PublicationSubscriptions updates(subscriptionOptions(inbox));
    EXPECT_TRUE(updates.takeNotifications().empty());
    ASSERT_EQ(updates.status(id).notices.size(), 1U);
    EXPECT_FALSE(updates.status(id).notices.front().acknowledged);
    updates.acknowledge(id, 2);
  }
  xanadu::PublicationInbox inbox(incomingOptions());
  xanadu::PublicationSubscriptions updates(subscriptionOptions(inbox));
  EXPECT_TRUE(updates.status(id).notices.front().acknowledged);
  EXPECT_TRUE(updates.takeNotifications().empty());
}

TEST_F(PublicationSubscriptionsTest,
       FailedClosureNeverNotifiesAndTheSameUpdateCanRetry) {
  xanadu::PublicationInbox inbox(incomingOptions());
  xanadu::PublicationSubscriptions updates(subscriptionOptions(inbox));
  const auto id        = updates.subscribe(baseline(inbox));
  downloads->failFetch = true;
  revision(2);
  updates.checkNow(id);
  ASSERT_TRUE(updates.waitForPhase(
      id, xanadu::PublicationSubscriptionPhase::Failed, 3s));
  EXPECT_EQ(updates.status(id).sequence, 1);
  EXPECT_THAT(updates.status(id).error, testing::HasSubstr("transfer failure"));
  EXPECT_TRUE(updates.takeNotifications().empty());
  downloads->failFetch = false;
  updates.checkNow(id);
  ASSERT_TRUE(updates.waitForSequence(id, 2, 3s)) << updates.status(id).error;
  EXPECT_EQ(updates.takeNotifications().size(), 1U);
}

TEST_F(PublicationSubscriptionsTest,
       SignedNamesCannotSubstituteAnotherPublisherOrRollBack) {
  xanadu::PublicationInbox inbox(incomingOptions());
  xanadu::PublicationSubscriptions updates(subscriptionOptions(inbox));
  const auto id       = updates.subscribe(baseline(inbox));
  const auto accepted = downloads->pointer;
  {
    const std::scoped_lock lock(downloads->guard);
    downloads->pointer.sequence = 0;
  }
  updates.checkNow(id);
  ASSERT_TRUE(updates.waitForPhase(
      id, xanadu::PublicationSubscriptionPhase::Failed, 2s));
  EXPECT_TRUE(updates.takeNotifications().empty());
  {
    const std::scoped_lock lock(downloads->guard);
    downloads->pointer = accepted;
    downloads->pointer.hash.bytes[0] ^= 1;
  }
  updates.checkNow(id);
  ASSERT_TRUE(updates.waitForPhase(
      id, xanadu::PublicationSubscriptionPhase::Failed, 2s));
  EXPECT_TRUE(updates.takeNotifications().empty());
  revision(2, true);
  updates.checkNow(id);
  ASSERT_TRUE(updates.waitForPhase(
      id, xanadu::PublicationSubscriptionPhase::Failed, 3s));
  EXPECT_EQ(updates.status(id).sequence, 1);
  EXPECT_TRUE(updates.takeNotifications().empty());
  revision(3);
  updates.checkNow(id);
  ASSERT_TRUE(updates.waitForSequence(id, 3, 4s)) << updates.status(id).error;
  const auto notices = updates.takeNotifications();
  ASSERT_EQ(notices.size(), 1U);
  EXPECT_EQ(notices.front().second.sequence, 3);
}

TEST_F(PublicationSubscriptionsTest,
       PausePersistsAndAReconnectFindsTheLatestSequence) {
  std::string id;
  {
    xanadu::PublicationInbox inbox(incomingOptions());
    xanadu::PublicationSubscriptions updates(subscriptionOptions(inbox));
    id = updates.subscribe(baseline(inbox));
    updates.setEnabled(id, false);
  }
  revision(2);
  xanadu::PublicationInbox inbox(incomingOptions());
  xanadu::PublicationSubscriptions updates(subscriptionOptions(inbox));
  EXPECT_FALSE(updates.status(id).enabled);
  EXPECT_THROW(updates.checkNow(id), std::logic_error);
  EXPECT_FALSE(updates.waitForSequence(id, 2, 100ms));
  updates.setEnabled(id, true);
  ASSERT_TRUE(updates.waitForSequence(id, 2, 3s));
  EXPECT_EQ(updates.takeNotifications().size(), 1U);
}

TEST_F(PublicationSubscriptionsTest,
       BoundsUnreviewedNoticesWithoutDiscardingAcceptedHistory) {
  xanadu::PublicationInbox inbox(incomingOptions());
  auto configured           = subscriptionOptions(inbox);
  configured.maximumNotices = 1;
  xanadu::PublicationSubscriptions updates(configured);
  const auto id = updates.subscribe(baseline(inbox));
  revision(2);
  updates.checkNow(id);
  ASSERT_TRUE(updates.waitForSequence(id, 2, 3s));
  revision(3);
  updates.checkNow(id);
  ASSERT_TRUE(updates.waitForPhase(
      id, xanadu::PublicationSubscriptionPhase::Failed, 3s));
  EXPECT_EQ(updates.status(id).sequence, 2);
  updates.acknowledge(id, 2);
  updates.checkNow(id);
  ASSERT_TRUE(updates.waitForSequence(id, 3, 3s));
  const auto notices = updates.takeNotifications();
  ASSERT_EQ(notices.size(), 1U);
  EXPECT_EQ(notices.front().second.sequence, 3);
}

TEST_F(PublicationSubscriptionsTest,
       ShutdownCancelsResolutionAndDoesNotLoseIdleWakeups) {
  xanadu::PublicationInbox inbox(incomingOptions());
  const auto snapshot = baseline(inbox);
  downloads->hold     = true;
  const auto started  = std::chrono::steady_clock::now();
  {
    xanadu::PublicationSubscriptions updates(subscriptionOptions(inbox));
    const auto id = updates.subscribe(snapshot);
    ASSERT_TRUE(updates.waitForPhase(
        id, xanadu::PublicationSubscriptionPhase::Checking, 2s));
  }
  for (int i = 0; i < 25; ++i) {
    xanadu::PublicationSubscriptions updates(subscriptionOptions(inbox));
    std::this_thread::sleep_for(100us);
  }
  EXPECT_LT(std::chrono::steady_clock::now() - started, 3s);
}

TEST_F(PublicationSubscriptionsTest,
       CancellingAPendingClosureKeepsItsVerifiedBaseline) {
  xanadu::PublicationInbox inbox(incomingOptions());
  const auto first   = baseline(inbox);
  const auto started = std::chrono::steady_clock::now();
  {
    xanadu::PublicationSubscriptions updates(subscriptionOptions(inbox));
    const auto id        = updates.subscribe(first);
    downloads->holdFetch = true;
    revision(2);
    updates.checkNow(id);
    ASSERT_TRUE(updates.waitForPhase(
        id, xanadu::PublicationSubscriptionPhase::Downloading, 2s));
  }
  downloads->holdFetch = false;
  xanadu::PublicationSubscriptions resumed(subscriptionOptions(inbox));
  ASSERT_TRUE(resumed.waitForSequence(link.target().hex(), 2, 3s));
  EXPECT_EQ(resumed.takeNotifications().size(), 1U);
  EXPECT_LT(std::chrono::steady_clock::now() - started, 4s);
}

TEST_F(PublicationSubscriptionsTest,
       RejectedTitlesLeaveRestartReadableAndLaterUpdatesRecover) {
  xanadu::PublicationInbox inbox(incomingOptions());
  std::string id;
  {
    xanadu::PublicationSubscriptions updates(subscriptionOptions(inbox));
    id = updates.subscribe(baseline(inbox));
    revision(2, false, std::string(1025, 'x'));
    updates.checkNow(id);
    ASSERT_TRUE(updates.waitForPhase(
        id, xanadu::PublicationSubscriptionPhase::Failed, 3s));
    EXPECT_THAT(updates.status(id).error, testing::HasSubstr("title"));
    EXPECT_EQ(updates.status(id).sequence, 1);
    EXPECT_TRUE(updates.takeNotifications().empty());
    auto options      = subscriptionOptions(inbox);
    options.directory = root / "rejected-subscriptions";
    xanadu::PublicationSubscriptions rejected(options);
    const auto badSnapshot = baseline(inbox);
    EXPECT_THROW((void)rejected.subscribe(badSnapshot), std::invalid_argument);
    EXPECT_TRUE(rejected.statuses().empty());
  }
  revision(3);
  xanadu::PublicationSubscriptions restored(subscriptionOptions(inbox));
  ASSERT_TRUE(restored.waitForSequence(id, 3, 3s)) << restored.status(id).error;
  const auto notices = restored.takeNotifications();
  ASSERT_EQ(notices.size(), 1U);
  EXPECT_EQ(notices.front().second.previousSequence, 1);
  EXPECT_EQ(notices.front().second.sequence, 3);
}

TEST_F(PublicationSubscriptionsTest, RefusesUnknownPersistedFormatsByNumber) {
  xanadu::PublicationInbox inbox(incomingOptions());
  std::string id;
  {
    xanadu::PublicationSubscriptions updates(subscriptionOptions(inbox));
    id = updates.subscribe(baseline(inbox));
    updates.setEnabled(id, false);
  }
  MDB_env *raw{};
  ASSERT_EQ(mdb_env_create(&raw), MDB_SUCCESS);
  std::unique_ptr<MDB_env, decltype(&mdb_env_close)> env(raw, mdb_env_close);
  ASSERT_EQ(mdb_env_open(raw, (root / "subscriptions").c_str(), 0, 0600),
            MDB_SUCCESS);
  MDB_txn *transaction{};
  ASSERT_EQ(mdb_txn_begin(raw, nullptr, 0, &transaction), MDB_SUCCESS);
  MDB_dbi db{};
  ASSERT_EQ(mdb_dbi_open(transaction, nullptr, 0, &db), MDB_SUCCESS);
  MDB_val key{id.size(), id.data()}, stored{};
  ASSERT_EQ(mdb_get(transaction, db, &key, &stored), MDB_SUCCESS);
  std::string damaged(static_cast<const char *>(stored.mv_data),
                      stored.mv_size);
  damaged[3] = '2';
  MDB_val value{damaged.size(), damaged.data()};
  ASSERT_EQ(mdb_put(transaction, db, &key, &value, 0), MDB_SUCCESS);
  ASSERT_EQ(mdb_txn_commit(transaction), MDB_SUCCESS);
  env.reset();
  try {
    xanadu::PublicationSubscriptions refused(subscriptionOptions(inbox));
    FAIL() << "Unknown subscriptions format accepted";
  } catch (const xanadu::PublicationSubscriptionsUnreadable &error) {
    EXPECT_THAT(error.what(),
                testing::HasSubstr("version 1 expected, got byte 50"));
  }
}

TEST_F(PublicationInboxTest, SubscriptionFloorRejectsRollbackBeforeFetching) {
  xanadu::PublicationInbox inbox(incomingOptions());
  auto minimum = downloads->pointer;
  ++minimum.sequence;
  const auto id = inbox.submit(link, minimum);
  ASSERT_TRUE(inbox.waitFor(id, xanadu::PublicationDownloadPhase::Failed, 3s));
  EXPECT_THAT(inbox.status(id).error, testing::HasSubstr("subscription"));
  EXPECT_FALSE(fs::exists(root / "inbox" / id));
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
       IndependentPackagesAreFoundReviewedAndOpenPinnedSnapshotsThroughUi) {
  const auto host            = std::getenv("XUDU_PEER_HOST");
  const auto port            = std::getenv("XUDU_PEER_PORT");
  const auto publisherHost   = std::getenv("XUDU_TEST_HOST");
  const auto readerHost      = std::getenv("XUDU_READER_HOST");
  const auto readerNamespace = std::getenv("XUDU_PEER_NAMESPACE");
  if (!host || !port || !publisherHost || !readerHost || !readerNamespace)
    GTEST_SKIP() << "run make test/publication-swarm";
  xanadu::Link link;
  link.owner         = "Response link";
  link.type          = xanadu::LinkType::Comment;
  link.left          = {{0, 0, 5}, {0, 6, 5}};
  link.right         = {{1, 0, 8}};
  const auto version = authored->addLink(authored->latest(), link);
  roots[0]           = root / "linked-seeds";
  const auto seal    = xanadu::sealLocalSpool(
      *authored, keys, "permascroll", roots[0].string(),
      {.tsv = "temporary test provenance", .signature = "mock"});
  publication = xanadu::publish(*authored, version, keys, publication.salt,
                                publication.title, 1, 1, &seal.scroll,
                                {*seal.opsSegment}, {}, {"ideas"});
  ASSERT_EQ(publication.links.size(), 1U);
  const auto originalOps = authored->opCount();
  xanadu::SwarmContentSource::Options network;
  network.listenInterfaces               = std::string(publisherHost) + ":0";
  network.enableLocalDiscovery           = false;
  network.enableTrackers                 = false;
  network.restrictDhtToDistinctNetworks  = false;
  network.allowManyConnectionsPerAddress = true;
  network.dhtPacketsPerSecond            = 100;
  const std::vector<std::pair<std::string, std::uint16_t>> nodes{
      {host, static_cast<std::uint16_t>(std::stoul(port))}};
  auto configured          = options();
  configured.makeTransport = xanadu::publicationSwarmTransport(
      keys, network, nodes, root / "source-catalog");
  xanadu::PublicationOutbox publisher(configured);
  const auto documentId = publisher.submit(publication, roots, true);
  ASSERT_TRUE(
      publisher.waitFor(documentId, xanadu::PublicationPhase::Published, 60s));
  const xanadu::PublicationPin pin{
      .publisher = keys.publicKey,
      .salt      = publication.salt,
      .hash      = status(publisher, documentId).manifestHash,
      .sequence  = publication.sequence,
      .version   = publication.version,
      .title     = publication.title};
  std::vector<std::unique_ptr<xanadu::LinkPackageExchange>> curators;
  for (const std::string name : {"Bob's Commentary", "Carl's Commentary"}) {
    const auto curator = xanadu::createMutableKeys();
    xanadu::LinkPackageExchange::Options opts;
    opts.directory      = root / curator.publicKey.hex();
    opts.verifyIdentity = [](const auto &) {
      return xanadu::PublicationIdentity::MockVerified;
    };
    opts.makePublisher = xanadu::publicationSwarmTransport(
        curator, network, nodes, root / "catalogs" / curator.publicKey.hex());
    auto service =
        std::make_unique<xanadu::LinkPackageExchange>(std::move(opts));
    const auto package = xanadu::publishLinkPackage(
        curator, "curations:story-ideas", name, 1, 1, publication.links,
        publication.scrolls, {pin});
    const auto id = service->submit(package, true);
    ASSERT_TRUE(service->waitFor(id, xanadu::LinkPackagePhase::Published, 60s))
        << service->status(id).error;
    curators.push_back(std::move(service));
  }
  const auto quote = [](const std::string &value) {
    std::string result{"'"};
    for (char ch : value) result += ch == '\'' ? "'\\''" : std::string(1, ch);
    return result + "'";
  };
  const auto evidence =
      fs::current_path() / "build/publication-links/network-ui";
  fs::create_directories(evidence);
  const auto profile = root / "alice-reader";
  fs::create_directories(profile / "workspace/published");
  std::ofstream(profile / "workspace/identity") << keys.publicKey.hex() << "\n"
                                                << keys.secretKey.hex() << "\n";
  std::ofstream(profile / "workspace/published/source.xanadoc",
                std::ios::binary)
      << xanadu::encodePublication(publication);
  const auto uiBase = [&](const fs::path &uiProfile, bool reader) {
    return (reader ? "ip netns exec " + quote(readerNamespace) + " " : "") +
           std::string("env") +
           " SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy "
           "LIBGL_ALWAYS_SOFTWARE=1 XDG_DATA_HOME=" +
           quote((uiProfile / "data").string()) +
           " XDG_CONFIG_HOME=" + quote((uiProfile / "config").string()) +
           " XDG_CACHE_HOME=" + quote((uiProfile / "cache").string()) +
           " ./build/xuzz " + quote((uiProfile / "workspace").string()) +
           " --permascroll " + quote((uiProfile / "permascroll").string()) +
           " --backend opengl --profile --test-publication-swarm " +
           quote(std::string(reader ? readerHost : publisherHost) + ":0") +
           " --dht-node " + quote(std::string(host) + ":" + port);
  };
  const auto base = uiBase(profile, true);
  for (int selected = 0; selected < 2; ++selected) {
    const auto stem    = "response-" + std::to_string(selected);
    const auto logPath = evidence / (stem + ".log");
    const auto select =
        selected ? " --chord Tab --chord Right --chord Shift+Tab" : "";
    const auto command =
        base +
        " --chord Ctrl+Alt+Shift+L --chord Return --wait-ms 35000 --chord "
        "Return "
        "--dump-a11y --capture " +
        quote((evidence / (stem + "-catalog.ppm")).string()) + select +
        " --chord Right --chord Return --wait-ms 35000 --chord Return "
        "--dump-a11y --capture " +
        quote((evidence / (stem + "-ready.ppm")).string()) +
        " --chord Right --chord Return --dump-a11y --capture " +
        quote((evidence / (stem + "-review.ppm")).string()) +
        " --chord Right --chord Right --chord Right --chord Right --chord "
        "Return --dump-a11y --capture " +
        quote((evidence / (stem + "-keys.ppm")).string()) +
        " --chord Tab --chord Tab --chord Tab --chord Space --dump-a11y "
        "--capture " +
        quote((evidence / (stem + "-key-parts.ppm")).string()) +
        " --chord Return --chord Shift+Tab --chord Shift+Tab --chord Shift+Tab "
        "--chord Return --chord Right --chord Return --wait-ms 30000 --chord "
        "Return "
        "--dump-a11y --capture " +
        quote((evidence / (stem + "-download.ppm")).string()) +
        " --chord Right --chord Return --dump-a11y --capture " +
        quote((evidence / (stem + "-opened.ppm")).string()) + " > " +
        quote(logPath.string()) + " 2>&1";
    ASSERT_EQ(std::system(command.c_str()), 0)
        << std::ifstream(logPath).rdbuf();
    std::ifstream input(logPath);
    const std::string log{std::istreambuf_iterator<char>(input),
                          std::istreambuf_iterator<char>()};
    EXPECT_THAT(log, testing::HasSubstr("Signed catalog metadata received"));
    EXPECT_THAT(log, testing::HasSubstr("Review independent links"));
    EXPECT_THAT(
        log, testing::HasSubstr("opened downloaded publication Story Ideas"));
  }
  xanadu::LinkPackageExchange offline(
      {.directory = profile / "data/xudu/link-packages"});
  ASSERT_EQ(offline.statuses().size(), 2U);
  for (const auto &pkg : offline.statuses()) {
    ASSERT_EQ(pkg.phase, xanadu::LinkPackagePhase::Ready);
    ASSERT_FALSE(pkg.package.publications.empty());
    EXPECT_EQ(pkg.package.publications.front(), pin);
    EXPECT_EQ(pkg.package.links.front().left.size(), 2U);
  }
  xanadu::PublicationDiscovery discovered(
      {.directory = profile / "data/xudu/publication-discovery"});
  EXPECT_TRUE(discovered.followedAuthors().empty());
  EXPECT_EQ(authored->opCount(), originalOps);
  // This source snapshot is fixture setup. The controls below prove package
  // preparation/review, without claiming commentary authorship was driven here.
  const auto devinProfile = root / "devin-publisher";
  const auto devinKeys    = xanadu::createMutableKeys();
  fs::create_directories(devinProfile / "workspace/published");
  std::ofstream(devinProfile / "workspace/identity")
      << devinKeys.publicKey.hex() << "\n"
      << devinKeys.secretKey.hex() << "\n";
  std::ofstream(devinProfile / "workspace/published/source.xanadoc",
                std::ios::binary)
      << xanadu::encodePublication(publication);
  const auto prepared =
      uiBase(devinProfile, false) +
      " --chord Ctrl+Alt+Shift+P --chord Tab --chord Tab --type " +
      quote("Devin's links") +
      " --chord Shift+Tab --chord Shift+Tab --chord Return --wait-ms 2000 "
      "--chord Return --dump-a11y --capture " +
      quote((evidence / "prepared.ppm").string()) +
      " --chord Right --chord Return --dump-a11y --capture " +
      quote((evidence / "prepared-review.ppm").string()) +
      " --chord Right --chord Right --chord Return --chord Right --chord Right "
      "--chord Right --chord Right --chord Right --chord Return --wait-ms "
      "35000 --chord Return --wait-ms 20000 --chord Return --dump-a11y "
      "--capture " +
      quote((evidence / "prepared-published.ppm").string()) + " > " +
      quote((evidence / "prepared.log").string()) + " 2>&1";
  ASSERT_EQ(std::system(prepared.c_str()), 0)
      << std::ifstream(evidence / "prepared.log").rdbuf();
  std::ifstream preparedInput(evidence / "prepared.log");
  const std::string preparedLog{std::istreambuf_iterator<char>(preparedInput),
                                std::istreambuf_iterator<char>()};
  EXPECT_THAT(preparedLog, testing::HasSubstr("Review independent links"));
  EXPECT_THAT(preparedLog, testing::HasSubstr("Published to rendezvous"));
  EXPECT_THAT(preparedLog, testing::HasSubstr("mock verification"));
  curators.clear();
  xanadu::LinkPackageExchange::Options devinOptions;
  devinOptions.directory      = devinProfile / "data/xudu/link-packages";
  devinOptions.verifyIdentity = [](const auto &) {
    return xanadu::PublicationIdentity::MockVerified;
  };
  devinOptions.makePublisher = xanadu::publicationSwarmTransport(
      devinKeys, network, nodes,
      devinProfile / "data/xudu/author-catalog" / devinKeys.publicKey.hex());
  xanadu::LinkPackageExchange devin(std::move(devinOptions));
  const auto devinStatus = devin.statuses().front();
  ASSERT_TRUE(
      devin.waitFor(devinStatus.id, xanadu::LinkPackagePhase::Published, 60s));
  const auto layerEvidence =
      fs::current_path() / "build/publication-layers/network-ui";
  fs::create_directories(layerEvidence);
  const auto read = [](const fs::path &path) {
    std::ifstream input(path, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>(input), {}};
  };
  for (const std::string user : {"Bob", "Carl"}) {
    const auto readerProfile = root / (user + "-layer-reader");
    fs::create_directories(readerProfile / "workspace/published");
    std::ofstream(readerProfile / "workspace/published/source.xanadoc",
                  std::ios::binary)
        << xanadu::encodePublication(publication);
    const auto userBase = uiBase(readerProfile, true);
    const auto userLog  = layerEvidence / (user + "-fetch.log");
    const auto fetch =
        userBase +
        " --chord Ctrl+Alt+Shift+L --chord Return --wait-ms 35000 --chord "
        "Return --dump-a11y --capture " +
        quote((layerEvidence / (user + "-catalog.ppm")).string()) +
        " --chord Right --chord Return --wait-ms 35000 --chord Return --chord "
        "Right --chord Return --dump-a11y --capture " +
        quote((layerEvidence / (user + "-review.ppm")).string()) +
        " --chord Right --chord Return --wait-ms 30000 --chord Return --chord "
        "Right --chord Return --dump-a11y --capture " +
        quote((layerEvidence / (user + "-opened.ppm")).string()) + " > " +
        quote(userLog.string()) + " 2>&1";
    ASSERT_EQ(std::system(fetch.c_str()), 0) << read(userLog);
    xanadu::LinkPackageExchange offline(
        {.directory = readerProfile / "data/xudu/link-packages"});
    const auto cached = offline.statuses();
    ASSERT_EQ(cached.size(), 1U) << read(userLog);
    ASSERT_EQ(cached[0].package.title, "Devin's links") << read(userLog);
    ASSERT_EQ(cached[0].phase, xanadu::LinkPackagePhase::Ready);
    xanadu::PublicationInbox inbox(
        {.directory = readerProfile / "data/xudu/publication-inbox"});
    const auto downloads = inbox.statuses();
    ASSERT_EQ(downloads.size(), 1U) << read(userLog);
    ASSERT_EQ(downloads[0].phase, xanadu::PublicationDownloadPhase::Ready);
    const auto native         = downloads[0].storePath;
    const auto originalNodes  = read(native / "ops.nodes"),
               originalTables = read(native / "store.tables");
    auto offlineBase =
        userBase.substr(0, userBase.find(" --test-publication-swarm"));
    const auto workspace = quote((readerProfile / "workspace").string());
    offlineBase.replace(offlineBase.find(workspace), workspace.size(),
                        quote(native.string()));
    const std::string reviewControls =
        " --chord Ctrl+Alt+Shift+L --chord Right --chord Return --chord Return "
        "--chord Right --chord Return";
    const std::string toggle = " --chord Right --chord Right --chord Right "
                               "--chord Right --chord Right --chord Return";
    const std::string navigate =
        " --chord Right --chord Right --chord Right --chord Right --chord "
        "Right --chord Right --chord Return";
    const auto enableLog = layerEvidence / (user + "-enabled.log");
    const auto enable =
        offlineBase + reviewControls + toggle + " --dump-a11y --capture " +
        quote((layerEvidence / (user + "-enabled.ppm")).string()) + navigate +
        " --chord Alt+Shift+J --chord Alt+Shift+J --chord Alt+Shift+L "
        "--chord Alt+Shift+Return "
        "--dump-a11y --capture " +
        quote((layerEvidence / (user + "-navigated.ppm")).string()) +
        " --chord Alt+Shift+X --chord Alt+Shift+Return --dump-a11y --capture " +
        quote((layerEvidence / (user + "-cell-navigated.ppm")).string()) +
        " > " + quote(enableLog.string()) + " 2>&1";
    ASSERT_EQ(std::system(enable.c_str()), 0) << read(enableLog);
    EXPECT_THAT(read(enableLog), testing::HasSubstr("reader layer enabled"));
    EXPECT_THAT(read(enableLog),
                testing::HasSubstr(
                    "Left 2/2 · occurrence 1/2 · document 0, bytes 6 to 11"));
    EXPECT_THAT(read(enableLog), testing::HasSubstr("reading: left member 2"));
    EXPECT_THAT(read(enableLog),
                testing::HasSubstr("Research [value kind: none] (Focused)"));
    EXPECT_TRUE(xanadu::ReaderLinkPackages(readerProfile /
                                           "data/xudu/package-visibility")
                    .enabled(cached[0].hash));
    const auto disableLog = layerEvidence / (user + "-restart-disabled.log");
    const auto disable =
        offlineBase + reviewControls + " --dump-a11y --capture " +
        quote((layerEvidence / (user + "-restart.ppm")).string()) + toggle +
        " --dump-a11y --capture " +
        quote((layerEvidence / (user + "-disabled.ppm")).string()) +
        " --chord Right --chord Right --chord Right --chord Return --dump-a11y "
        "--capture " +
        quote((layerEvidence / (user + "-disabled-document.ppm")).string()) +
        " > " + quote(disableLog.string()) + " 2>&1";
    ASSERT_EQ(std::system(disable.c_str()), 0) << read(disableLog);
    EXPECT_THAT(read(disableLog), testing::HasSubstr("reader layer enabled"));
    EXPECT_THAT(read(disableLog), testing::HasSubstr("reader layer disabled"));
    EXPECT_FALSE(xanadu::ReaderLinkPackages(readerProfile /
                                            "data/xudu/package-visibility")
                     .enabled(cached[0].hash));
    EXPECT_EQ(read(native / "ops.nodes"), originalNodes);
    EXPECT_EQ(read(native / "store.tables"), originalTables);
  }
}

TEST_F(PublicationOutboxNetworkTest,
       SubscriptionsRecoverLiveAndMissedUpdatesThroughTheInterface) {
  const auto host            = std::getenv("XUDU_PEER_HOST");
  const auto port            = std::getenv("XUDU_PEER_PORT");
  const auto publisherHost   = std::getenv("XUDU_TEST_HOST");
  const auto readerNamespace = std::getenv("XUDU_PEER_NAMESPACE");
  const auto bobHost         = std::getenv("XUDU_READER_HOST");
  const auto carlHost        = std::getenv("XUDU_DISCOVERY_HOST");
  if (!host || !port || !publisherHost || !readerNamespace || !bobHost ||
      !carlHost)
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
  const auto first = publisher.submit(publication, roots, true);
  ASSERT_TRUE(
      publisher.waitFor(first, xanadu::PublicationPhase::Published, 60s))
      << status(publisher, first).error;
  const auto earlierVersion = publication.version;
  xanadu::MutableLink link;
  link.key  = keys.publicKey;
  link.salt = publication.salt;
  const auto evidence =
      fs::current_path() / "build/publication-updates/network-ui";
  fs::create_directories(evidence);
  const auto quote = [](std::string_view value) {
    std::string result{"'"};
    for (const auto ch : value)
      result += ch == '\'' ? "'\\''" : std::string(1, ch);
    return result + "'";
  };
  const auto base = [&](const std::string &name, const char *address) {
    const auto profile = root / name;
    return "ip netns exec " + quote(readerNamespace) +
           " env SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy "
           "LIBGL_ALWAYS_SOFTWARE=1" +
           " XDG_DATA_HOME=" + quote((profile / "data").string()) +
           " XDG_CONFIG_HOME=" + quote((profile / "config").string()) +
           " XDG_CACHE_HOME=" + quote((profile / "cache").string()) +
           " ./build/xuzz " + quote((profile / "workspace").string()) +
           " --permascroll " + quote((profile / "permascroll").string()) +
           " --backend opengl --profile --test-publication-swarm " +
           quote(std::string(address) + ":0") + " --dht-node " +
           quote(std::string(host) + ":" + port);
  };
  const auto capture = [&](const std::string &name) {
    return " --dump-a11y --capture " +
           quote((evidence / (name + ".ppm")).string());
  };
  const auto subscribe = [&](const std::string &name) {
    return " --chord Ctrl+O --chord Tab --type " + quote(link.uri()) +
           " --chord Return" + " --wait-ms 35000 --chord Return" +
           capture(name + "-download") +
           " --chord Right --chord Right --chord Right --chord Right --chord "
           "Right --chord Return" +
           capture(name + "-subscribed") +
           " --chord Right --chord Right --chord Right --chord Right --chord "
           "Right --chord Right --chord Right --chord Return";
  };
  const auto review = [&](const std::string &name) {
    return " --chord Ctrl+Shift+U" + capture(name + "-updates") +
           " --chord Right --chord Return" + capture(name + "-compared");
  };
  const auto carlInitialLog = evidence / "carl-subscribe.log";
  auto command = base("carl", carlHost) + subscribe("carl") + " > " +
                 quote(carlInitialLog.string()) + " 2>&1";
  ASSERT_EQ(std::system(command.c_str()), 0)
      << std::ifstream(carlInitialLog).rdbuf();
  const auto bobLog = evidence / "bob-live.log";
  fs::remove(evidence / "bob-subscribed.ppm");
  std::string timeline;
  for (int step = 0; step < 18; ++step)
    timeline +=
        " --wait-ms 2000" + capture("bob-notice-" + std::to_string(step));
  command = base("bob", bobHost) + subscribe("bob") + timeline + review("bob") +
            " > " + quote(bobLog.string()) + " 2>&1";
  auto bob = std::async(std::launch::async,
                        [command] { return std::system(command.c_str()); });

  const auto deadline = std::chrono::steady_clock::now() + 65s;
  while (!fs::exists(evidence / "bob-subscribed.ppm") &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(50ms);
  ASSERT_TRUE(fs::exists(evidence / "bob-subscribed.ppm"))
      << std::ifstream(bobLog).rdbuf();
  // Alice changes real authored history while Bob is online and Carl is
  // offline.
  const auto version = authored->insert(earlierVersion, 11, " revised");
  const auto seal =
      xanadu::sealLocalSpool(*authored, keys, "permascroll", roots[0].string(),
                             {.tsv = "test record", .signature = "mock"});
  publication =
      xanadu::publish(*authored, version, keys, publication.salt, "Story Ideas",
                      2, 2, &seal.scroll, {*seal.opsSegment}, {}, {"ideas"});
  const auto second = publisher.submit(publication, roots, true);
  ASSERT_TRUE(
      publisher.waitFor(second, xanadu::PublicationPhase::Published, 60s))
      << status(publisher, second).error;
  ASSERT_EQ(bob.get(), 0) << std::ifstream(bobLog).rdbuf();
  const auto carlLog = evidence / "carl-reconnect.log";
  std::string carlTimeline;
  for (int step = 0; step < 18; ++step)
    carlTimeline +=
        " --wait-ms 2000" + capture("carl-notice-" + std::to_string(step));
  command = base("carl", carlHost) + carlTimeline + review("carl") + " > " +
            quote(carlLog.string()) + " 2>&1";
  ASSERT_EQ(std::system(command.c_str()), 0) << std::ifstream(carlLog).rdbuf();
  for (const std::string name : {"bob", "carl"}) {
    const auto logPath = name == "bob" ? bobLog : carlLog;
    std::ifstream input(logPath);
    const std::string log{std::istreambuf_iterator<char>(input),
                          std::istreambuf_iterator<char>()};
    EXPECT_THAT(log, testing::HasSubstr("Publication update: Story Ideas"));
    EXPECT_THAT(log,
                testing::HasSubstr("Open update alongside earlier version"));
    EXPECT_THAT(log, testing::HasSubstr("Story Ideas revised"));
    xanadu::PublicationInbox inbox(
        {.directory = root / name / "data/xudu/publication-inbox"});
    xanadu::PublicationSubscriptions offline(
        {.directory = root / name / "data/xudu/publication-subscriptions",
         .inbox     = &inbox});
    const auto subscriptions = offline.statuses();
    ASSERT_EQ(subscriptions.size(), 1U);
    ASSERT_EQ(subscriptions.front().sequence, 2);
    ASSERT_EQ(subscriptions.front().notices.size(), 1U);
    const auto &notice = subscriptions.front().notices.front();
    EXPECT_TRUE(notice.delivered);
    EXPECT_TRUE(notice.acknowledged);
    EXPECT_EQ(notice.previousVersion, earlierVersion);
    EXPECT_EQ(notice.version, version);
    xanadu::Store earlier, latest;
    earlier.load(inbox.status(notice.previousSnapshotId).storePath.string());
    latest.load(inbox.status(notice.snapshotId).storePath.string());
    EXPECT_EQ(earlier.documentId(), publication.storeId);
    EXPECT_EQ(latest.documentId(), publication.storeId);
    EXPECT_EQ(earlier.textOf(earlierVersion), "Story Ideas");
    EXPECT_EQ(latest.textOf(version), "Story Ideas revised");
    EXPECT_EQ(latest.userPermascroll().spool().size(), 0U);
    EXPECT_TRUE(offline.takeNotifications().empty());
  }
  const auto restartedLog = evidence / "carl-restarted.log";
  command = base("carl", carlHost) + " --wait-ms 6000 --chord Ctrl+Shift+U" +
            capture("carl-restarted") + " > " + quote(restartedLog.string()) +
            " 2>&1";
  ASSERT_EQ(std::system(command.c_str()), 0)
      << std::ifstream(restartedLog).rdbuf();
  std::ifstream restarted(restartedLog);
  const std::string log{std::istreambuf_iterator<char>(restarted),
                        std::istreambuf_iterator<char>()};
  EXPECT_THAT(
      log, testing::Not(testing::HasSubstr("Publication update: Story Ideas")));
  EXPECT_THAT(log, testing::HasSubstr("No unreviewed verified updates"));
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
