#include "common/xanadu/author_catalog.hpp"
#include "common/xanadu/bencode.hpp"
#include "common/xanadu/publication_discovery.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/swarm_catalog.hpp"
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <mutex>
#include <thread>

namespace {
using namespace std::chrono_literals;
namespace fs = std::filesystem;
class AuthorCatalogTest : public testing::Test {
protected:
  xanadu::MutableKeys keys = xanadu::createMutableKeys();
  fs::path root =
      fs::temp_directory_path() / ("xudu-catalog-" + keys.publicKey.hex());
  xanadu::Publication pub;
  xanadu::InfoHash hash;
  void SetUp() override {
    xanadu::Store store;
    const auto version = store.insert({}, 0, "Story Ideas");
    const auto seal    = xanadu::sealLocalSpool(
        store, keys, "permascroll", (root / "seeds").string(),
        {.tsv = "temporary test provenance", .signature = "mock"});
    pub = xanadu::publish(store, version, keys, "doc:ideas", "Story Ideas", 1,
                          1, &seal.scroll, {*seal.opsSegment}, {}, {"ideas"});
    const std::vector<xanadu::TorrentContent> files{
        {.path = "publication.xanadoc",
         .data = xanadu::encodePublication(pub)}};
    hash = xanadu::makeTorrent(files, "publication").hash;
  }
  ~AuthorCatalogTest() override { fs::remove_all(root); }
  xanadu::SignedAuthorCatalog catalog() const {
    return xanadu::signAuthorCatalog(
        {.sequence = 1,
         .entries  = {{hash, pub.salt, pub.title, pub.topics, pub.version,
                       pub.sequence}}},
        keys);
  }
  void resign(xanadu::Publication &publication) const {
    publication.signature = xanadu::signMutableItem(
        xanadu::publicationSigningBuffer(publication), keys);
  }
};

TEST_F(AuthorCatalogTest,
       SignedRecordsRoundTripAndRejectForgedMetadataAndVersions) {
  const auto wire   = xanadu::encodeAuthorCatalog(catalog());
  const auto loaded = xanadu::decodeAuthorCatalog(wire);
  EXPECT_EQ(loaded.publisher, keys.publicKey);
  EXPECT_EQ(loaded.entries.front().version, pub.version);
  auto forged                  = loaded;
  forged.entries.front().title = "Forged Ideas";
  EXPECT_THROW(
      (void)xanadu::decodeAuthorCatalog(xanadu::encodeAuthorCatalog(forged)),
      xanadu::AuthorCatalogUnreadable);
  forged           = loaded;
  forged.publisher = xanadu::createMutableKeys().publicKey;
  EXPECT_THROW(
      (void)xanadu::decodeAuthorCatalog(xanadu::encodeAuthorCatalog(forged)),
      xanadu::AuthorCatalogUnreadable);
  auto fields      = xanadu::bencode::decode(wire).asDict();
  fields["format"] = xanadu::bencode::Value::integer(3);
  try {
    (void)xanadu::decodeAuthorCatalog(
        xanadu::bencode::Value::dict(fields).encode());
    FAIL() << "version 3 accepted";
  } catch (const xanadu::AuthorCatalogUnreadable &error) {
    EXPECT_THAT(error.what(), testing::HasSubstr("version 2 expected, got 3"));
  }
  fields["format"] = xanadu::bencode::Value::integer(1);
  EXPECT_THROW((void)xanadu::decodeAuthorCatalog(
                   xanadu::bencode::Value::dict(fields).encode()),
               xanadu::AuthorCatalogUnreadable);
  EXPECT_THROW((void)xanadu::decodeAuthorCatalog(wire + "junk"),
               xanadu::AuthorCatalogUnreadable);
  EXPECT_THROW((void)xanadu::decodeAuthorCatalog(
                   std::string(xanadu::maximumAuthorCatalogBytes + 1, 'x')),
               xanadu::AuthorCatalogUnreadable);
  EXPECT_THROW((void)xanadu::decodeAuthorCatalog(std::string(1000, 'l') +
                                                 std::string(1000, 'e')),
               xanadu::AuthorCatalogUnreadable);
}

TEST_F(AuthorCatalogTest,
       DurableNamesAreIdempotentAndAuthorialVersionsArePreserved) {
  auto current =
      xanadu::updateAuthorCatalog(root / "publisher", pub, hash, keys);
  EXPECT_EQ(current.sequence, 1);
  EXPECT_EQ(
      xanadu::updateAuthorCatalog(root / "publisher", pub, hash, keys).sequence,
      1);
  auto next  = pub;
  next.salt  = "doc:other";
  next.title = "Another work";
  resign(next);
  current = xanadu::updateAuthorCatalog(root / "publisher", next, hash, keys);
  ASSERT_EQ(current.entries.size(), 2U);
  EXPECT_EQ(current.sequence, 2);
  next          = pub;
  next.sequence = 2;
  next.version  = {};
  resign(next);
  current = xanadu::updateAuthorCatalog(root / "publisher", next, hash, keys);
  EXPECT_TRUE(current.entries.front().version.isZero());
  EXPECT_EQ(current.sequence, 3);
  EXPECT_EQ(
      xanadu::updateAuthorCatalog(root / "publisher", pub, hash, keys).sequence,
      3);
  next.title = "Conflicting title";
  resign(next);
  EXPECT_THROW(
      (void)xanadu::updateAuthorCatalog(root / "publisher", next, hash, keys),
      std::invalid_argument);
  next.salt = "catalog";
  resign(next);
  EXPECT_THROW(
      (void)xanadu::updateAuthorCatalog(root / "publisher", next, hash, keys),
      xanadu::AuthorCatalogUnreadable);
}

TEST_F(AuthorCatalogTest,
       PackageAnnouncementPreservesTheAuthorsDocumentEntries) {
  const auto before =
      xanadu::updateAuthorCatalog(root / "mixed", pub, hash, keys);
  xanadu::GlobalLink link;
  link.owner         = "Response";
  link.left          = pub.pieces;
  link.right         = pub.pieces;
  const auto package = xanadu::publishLinkPackage(
      keys, "curations:ideas", "Story Ideas links", 1, 1, {link}, pub.scrolls);
  const std::vector<xanadu::TorrentContent> files{
      {.path = "links.xanalinks", .data = xanadu::encodeLinkPackage(package)}};
  const auto packageHash = xanadu::makeTorrent(files, "link-package").hash;
  const auto after =
      xanadu::updateAuthorCatalog(root / "mixed", package, packageHash, keys);
  ASSERT_EQ(after.entries.size(), 2U);
  const auto document = std::ranges::find(after.entries, pub.salt,
                                          &xanadu::AuthorCatalogEntry::salt);
  ASSERT_NE(document, after.entries.end());
  EXPECT_EQ(*document, before.entries.front());
  EXPECT_EQ(after.sequence, 2);
  xanadu::SwarmCatalog index;
  index.followAuthor(keys.publicKey);
  index.ingestAuthorCatalog(after);
  EXPECT_EQ(index.search("Ideas").size(), 1U);
  EXPECT_EQ(index.followedAuthors().front().publicationCount, 1U);
  xanadu::SwarmCatalog followedLater;
  followedLater.ingestAuthorCatalog(after);
  followedLater.followAuthor(keys.publicKey);
  EXPECT_EQ(followedLater.followedAuthors().front().publicationCount, 1U);
}

TEST_F(AuthorCatalogTest,
       ReaderHighWaterMarksRejectRollbackAndEquivocationAfterRestart) {
  const auto first = catalog();
  ASSERT_TRUE(xanadu::retainAuthorCatalog(root / "reader", first));
  auto later                     = first;
  later.sequence                 = 2;
  later.entries.front().sequence = 2;
  later.entries.front().title    = "Revised Ideas";
  later                          = xanadu::signAuthorCatalog(later, keys);
  ASSERT_TRUE(xanadu::retainAuthorCatalog(root / "reader", later));
  EXPECT_FALSE(xanadu::retainAuthorCatalog(root / "reader", first));
  EXPECT_TRUE(xanadu::retainAuthorCatalog(root / "reader", later));
  auto conflict                  = later;
  conflict.entries.front().title = "Different Ideas";
  conflict                       = xanadu::signAuthorCatalog(conflict, keys);
  EXPECT_THROW(xanadu::retainAuthorCatalog(root / "reader", conflict),
               xanadu::AuthorCatalogUnreadable);
  auto rollback     = first;
  rollback.sequence = 3;
  rollback          = xanadu::signAuthorCatalog(rollback, keys);
  EXPECT_THROW(xanadu::retainAuthorCatalog(root / "reader", rollback),
               xanadu::AuthorCatalogUnreadable);
  const auto retained = xanadu::retainedAuthorCatalogs(root / "reader");
  ASSERT_EQ(retained.size(), 1U);
  EXPECT_EQ(retained.front().entries.front().title, "Revised Ideas");
}

TEST_F(AuthorCatalogTest,
       ConcurrentWritersKeepBothNamesAndSearchReplacesOldVersions) {
  auto other = pub;
  other.salt = "doc:other";
  resign(other);
  std::jthread a([&] {
    (void)xanadu::updateAuthorCatalog(root / "publisher", pub, hash, keys);
  });
  std::jthread b([&] {
    (void)xanadu::updateAuthorCatalog(root / "publisher", other, hash, keys);
  });
  a.join();
  b.join();
  const auto both = xanadu::retainedAuthorCatalogs(root / "publisher");
  ASSERT_EQ(both.size(), 1U);
  EXPECT_EQ(both.front().entries.size(), 2U);
  EXPECT_EQ(both.front().sequence, 2);
  xanadu::SwarmCatalog search;
  search.followAuthor(keys.publicKey);
  search.ingestAuthorCatalog(catalog());
  auto revised     = catalog();
  revised.sequence = 2;
  revised.entries.front().hash.bytes[0] ^= 1;
  revised.entries.front().sequence = 2;
  revised.entries.front().title    = "Revised Ideas";
  revised                          = xanadu::signAuthorCatalog(revised, keys);
  search.ingestAuthorCatalog(revised);
  search.ingestAuthorCatalog(catalog());
  const auto results = search.search("Ideas");
  ASSERT_EQ(results.size(), 1U);
  EXPECT_EQ(results.front().entry.title, "Revised Ideas");
  EXPECT_EQ(results.front().entry.sequence, 2U);
  EXPECT_FALSE(results.front().isVerified);
  EXPECT_EQ(search.followedAuthors().front().publicationCount, 1U);
}

TEST_F(AuthorCatalogTest, TopicNamesAreCanonicalAndNamespaceSeparated) {
  EXPECT_EQ(xanadu::publicationTopicTarget("Ideas"),
            xanadu::publicationTopicTarget(" ideas "));
  EXPECT_NE(xanadu::publicationTopicTarget("ideas"),
            xanadu::publicationTopicTarget("physics"));
  EXPECT_THROW((void)xanadu::publicationTopicTarget("ideas,physics"),
               std::invalid_argument);
  EXPECT_THROW((void)xanadu::publicationTopicTarget(""), std::invalid_argument);
  EXPECT_THROW((void)xanadu::publicationTopicTarget("idea\ntext"),
               std::invalid_argument);
}

struct DiscoveryState {
  std::mutex guard;
  std::vector<std::thread::id> threads;
  std::vector<std::string> response;
  std::atomic<bool> hold{};
};
class TestDiscoveryTransport final
    : public xanadu::PublicationDiscoveryTransport {
public:
  explicit TestDiscoveryTransport(std::shared_ptr<DiscoveryState> state)
      : state_(std::move(state)) {
    record();
  }
  ~TestDiscoveryTransport() override { record(); }
  std::string author(const xanadu::PublicKey &, std::stop_token stop) override {
    wait(stop);
    return state_->response.at(0);
  }
  std::vector<std::string> backlinks(const std::vector<std::string> &,
                                     std::stop_token stop) override {
    wait(stop);
    return state_->response;
  }
  std::vector<std::string> topic(std::string_view,
                                 std::stop_token stop) override {
    wait(stop);
    return state_->response;
  }

private:
  void record() {
    const std::scoped_lock lock(state_->guard);
    state_->threads.push_back(std::this_thread::get_id());
  }
  void wait(std::stop_token stop) {
    record();
    while (state_->hold && !stop.stop_requested())
      std::this_thread::sleep_for(5ms);
    if (stop.stop_requested()) throw std::runtime_error("Stopped");
  }
  std::shared_ptr<DiscoveryState> state_;
};
class PublicationDiscoveryTest : public AuthorCatalogTest {
protected:
  std::shared_ptr<DiscoveryState> state = std::make_shared<DiscoveryState>();
  xanadu::PublicationDiscovery::Options options() {
    state->response = {xanadu::encodeAuthorCatalog(catalog())};
    return {.directory = root / "discovery", .makeTransport = [state = state] {
              return std::make_unique<TestDiscoveryTransport>(state);
            }};
  }
};
TEST_F(PublicationDiscoveryTest,
       KeyAndTopicDiscoveryUseWorkerAndRetainOfflineSignedMetadata) {
  {
    xanadu::PublicationDiscovery discovery(options());
    const auto author = discovery.submit(keys.publicKey.hex(), true);
    ASSERT_TRUE(discovery.waitFor(author, xanadu::DiscoveryPhase::Ready, 3s));
    EXPECT_EQ(discovery.status(author).catalogs.front().publisher,
              keys.publicKey);
    const auto topic = discovery.submit("Ideas", false);
    ASSERT_TRUE(discovery.waitFor(topic, xanadu::DiscoveryPhase::Ready, 3s));
    EXPECT_EQ(discovery.status(topic).query, "ideas");
  }
  xanadu::PublicationDiscovery offline({.directory = root / "discovery"});
  EXPECT_EQ(offline.cachedCatalogs().front().entries.front().title,
            "Story Ideas");
  EXPECT_EQ(offline.followedAuthors(),
            (std::vector<xanadu::PublicKey>{keys.publicKey}));
  for (const auto &thread : state->threads)
    EXPECT_NE(thread, std::this_thread::get_id());
  EXPECT_TRUE(std::ranges::all_of(state->threads, [&](auto thread) {
    return thread == state->threads.front();
  }));
}
TEST_F(PublicationDiscoveryTest,
       UnknownKeyWrongTopicAndForgedCatalogCannotEnterReaderCache) {
  xanadu::PublicationDiscovery discovery(options());
  auto id = discovery.submit(xanadu::createMutableKeys().publicKey.hex(), true);
  ASSERT_TRUE(discovery.waitFor(id, xanadu::DiscoveryPhase::Failed, 3s));
  id = discovery.submit("Physics", false);
  ASSERT_TRUE(discovery.waitFor(id, xanadu::DiscoveryPhase::Failed, 3s));
  state->response = {"forged"};
  id              = discovery.submit("Ideas", false);
  ASSERT_TRUE(discovery.waitFor(id, xanadu::DiscoveryPhase::Failed, 3s));
  EXPECT_TRUE(discovery.cachedCatalogs().empty());
}
TEST_F(PublicationDiscoveryTest,
       FailureCanRetryAndShutdownInterruptsNetworkWaits) {
  auto configured = options();
  state->response = {"forged"};
  {
    xanadu::PublicationDiscovery discovery(configured);
    const auto id = discovery.submit("Ideas", false);
    ASSERT_TRUE(discovery.waitFor(id, xanadu::DiscoveryPhase::Failed, 3s));
    state->response = {xanadu::encodeAuthorCatalog(catalog())};
    EXPECT_EQ(discovery.submit("Ideas", false), id);
    ASSERT_TRUE(discovery.waitFor(id, xanadu::DiscoveryPhase::Ready, 3s));
    state->hold        = true;
    const auto pending = discovery.submit("Physics", false);
    ASSERT_TRUE(
        discovery.waitFor(pending, xanadu::DiscoveryPhase::Searching, 3s));
  }
  EXPECT_TRUE(fs::is_empty(root / "discovery" / "followed"));
}
TEST_F(PublicationDiscoveryTest,
       BacklinksRequireTypedEntriesAndMatchedScrolls) {
  auto configured                 = options();
  auto advertised                 = catalog();
  advertised.entries.front().kind = xanadu::CatalogEntryKind::LinkPackage;
  const auto key = xanadu::scrollKey(pub.scrolls.begin()->second);
  advertised.entries.front().scrollKeys = {key};
  advertised      = xanadu::signAuthorCatalog(advertised, keys);
  state->response = {xanadu::encodeAuthorCatalog(advertised)};
  xanadu::PublicationDiscovery discovery(configured);
  const auto id = discovery.submitLinks({key, key});
  ASSERT_TRUE(discovery.waitFor(id, xanadu::DiscoveryPhase::Ready, 3s));
  EXPECT_EQ(discovery.status(id).scrollKeys.size(), 1U);
  EXPECT_TRUE(discovery.followedAuthors().empty());
  const auto missing = discovery.submitLinks({"btpk:unknown:permascroll"});
  ASSERT_TRUE(discovery.waitFor(missing, xanadu::DiscoveryPhase::Failed, 3s));
  xanadu::SwarmCatalog index;
  index.ingestAuthorCatalog(advertised);
  EXPECT_TRUE(index.search("Ideas").empty());
  EXPECT_THROW((void)discovery.submitLinks({}), std::invalid_argument);
}

TEST_F(PublicationDiscoveryTest, IdleShutdownDoesNotLoseStopWakeups) {
  const auto started = std::chrono::steady_clock::now();
  for (int attempt = 0; attempt < 100; ++attempt) {
    xanadu::PublicationDiscovery discovery(options());
    std::this_thread::sleep_for(100us);
  }
  EXPECT_LT(std::chrono::steady_clock::now() - started, 3s);
}
} // namespace
