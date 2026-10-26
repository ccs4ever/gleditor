/**
 * @file user_permascroll_test.cpp
 * @brief Unit tests for sovereign UserPermascroll, cross-document sharing,
 *        collaborative live ops, and DeviceDelegation.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <lmdb.h>

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "common/xanadu/identity/identity_layout.hpp"
#include "common/xanadu/ops.hpp"
#include "common/xanadu/provenance.hpp"
#include "common/xanadu/publication.hpp"
#include "common/xanadu/scroll.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"

#include "identity_fixture.hpp"

namespace {

using xanadu::MicroversionId;
using xanadu::Op;
using xanadu::OpKind;
using xanadu::PermascrollRegistry;
using xanadu::PrimediaSpan;
using xanadu::PublicKey;
using xanadu::SignedProvenance;
using xanadu::Store;
using xanadu::UserPermascroll;
using xanadu::identity::Fingerprint;

TEST(UserPermascrollTest, BasicAppendAndRead) {
  UserPermascroll scroll;
  const auto span1 = scroll.append("Hello ");
  const auto span2 = scroll.append("Permascroll!");

  EXPECT_EQ(span1.scroll, 0U);
  EXPECT_EQ(span1.start, 0U);
  EXPECT_EQ(span1.length, 6U);

  EXPECT_EQ(span2.scroll, 0U);
  EXPECT_EQ(span2.start, 6U);
  EXPECT_EQ(span2.length, 12U);

  EXPECT_EQ(scroll.size(), 18U);
  EXPECT_EQ(scroll.read(span1), "Hello ");
  EXPECT_EQ(scroll.read(span2), "Permascroll!");
  EXPECT_EQ(scroll.readView(span2), "Permascroll!");
  EXPECT_EQ(scroll.bytes(), "Hello Permascroll!");
}

TEST(UserPermascrollTest, MultipleStoresSharingOnePermascroll) {
  auto sharedPermascroll = std::make_shared<UserPermascroll>();

  Store storeA(sharedPermascroll);
  Store storeB(sharedPermascroll);

  const auto verA1 = storeA.insert(MicroversionId{}, 0, "Alice chapter 1.");
  const auto verB1 = storeB.insert(MicroversionId{}, 0, "Bob notes.");
  const auto verA2 = storeA.insert(verA1, 16, " Alice chapter 2.");

  EXPECT_EQ(storeA.textOf(verA1), "Alice chapter 1.");
  EXPECT_EQ(storeB.textOf(verB1), "Bob notes.");
  EXPECT_EQ(storeA.textOf(verA2), "Alice chapter 1. Alice chapter 2.");

  // Monotonic, continuous global byte stream across documents
  EXPECT_EQ(sharedPermascroll->bytes(),
            "Alice chapter 1.Bob notes. Alice chapter 2.");
  EXPECT_EQ(sharedPermascroll->size(), 43U);
}

TEST(UserPermascrollTest, CrossDocumentSelfTransclusion) {
  auto sharedPermascroll = std::make_shared<UserPermascroll>();

  Store storeA(sharedPermascroll);
  Store storeB(sharedPermascroll);

  const auto verA =
      storeA.insert(MicroversionId{}, 0, "The foundational theorem.");
  const auto bytesBeforeTransclusion = sharedPermascroll->size();

  // Document B transcludes the exact span from the shared author's permascroll
  // (slot 0)
  Op op;
  op.kind         = OpKind::Insert;
  op.at           = 0;
  op.span         = PrimediaSpan{0, 4, 12}; // Points to "foundational"
  const auto verB = storeB.apply(MicroversionId{}, op);

  EXPECT_EQ(storeB.textOf(verB), "foundational");

  // Zero duplicate storage allocated on transclusion
  EXPECT_EQ(sharedPermascroll->size(), bytesBeforeTransclusion);
}

TEST(UserPermascrollTest, IncrementalSealing) {
  const auto tempDir =
      std::filesystem::temp_directory_path() / "xudu_permascroll_test_seal";
  std::filesystem::remove_all(tempDir);
  std::filesystem::create_directories(tempDir);

  UserPermascroll scroll;
  scroll.append("First segment content.");

  SignedProvenance prov1;
  prov1.tsv       = "title\tPermascroll Seg 1\n";
  prov1.signature = "ed25519-sig:test-permascroll-seg-1\n";

  const auto seg1 = scroll.sealIncremental(tempDir, prov1);
  ASSERT_TRUE(seg1.has_value());
  EXPECT_EQ(seg1->at, 0U);
  EXPECT_EQ(seg1->length, 22U);
  EXPECT_EQ(seg1->fileIndex, 0U);

  scroll.append(" Second segment content.");

  SignedProvenance prov2;
  prov2.tsv       = "title\tPermascroll Seg 2\n";
  prov2.signature = "ed25519-sig:test-permascroll-seg-2\n";

  const auto seg2 = scroll.sealIncremental(tempDir, prov2);
  ASSERT_TRUE(seg2.has_value());
  EXPECT_EQ(seg2->at, 22U);
  EXPECT_EQ(seg2->length, 24U);

  const auto current = scroll.currentScroll();
  ASSERT_EQ(current.segments.size(), 2U);
  EXPECT_EQ(current.segments[0].at, 0U);
  EXPECT_EQ(current.segments[0].length, 22U);
  EXPECT_EQ(current.segments[1].at, 22U);
  EXPECT_EQ(current.segments[1].length, 24U);

  std::filesystem::remove_all(tempDir);
}

class PersistentPermascrollTest : public testing::Test {
protected:
  std::filesystem::path root = std::filesystem::temp_directory_path() /
                               ("xudu-permascroll-restart-" +
                                xanadu::createMutableKeys().publicKey.hex());
  ~PersistentPermascrollTest() override { std::filesystem::remove_all(root); }
  UserPermascroll::Config config() const {
    UserPermascroll::Config result;
    result.storageDir = root / "author";
    return result;
  }
};

TEST_F(PersistentPermascrollTest,
       IdentityAndIncrementalSegmentsSurviveRestart) {
  PublicKey publisher;
  std::string name;
  xanadu::ScrollSegment first;
  const SignedProvenance provenance;
  {
    UserPermascroll scroll(config());
    publisher = scroll.config().deviceKeys.publicKey;
    name      = scroll.globalScrollKey();
    scroll.append("Story Ideas");
    first = *scroll.sealIncremental(root / "published", provenance);
    scroll.append(" - revised");
  }
  {
    UserPermascroll scroll(config());
    EXPECT_EQ(scroll.config().deviceKeys.publicKey, publisher);
    EXPECT_EQ(scroll.globalScrollKey(), name);
    EXPECT_EQ(scroll.bytes(), "Story Ideas - revised");
    ASSERT_EQ(scroll.currentScroll().segments.size(), 1U);
    EXPECT_EQ(scroll.currentScroll().segments.front(), first);
    const auto second = scroll.sealIncremental(root / "published", provenance);
    ASSERT_TRUE(second);
    EXPECT_EQ(second->at, 11U);
    EXPECT_EQ(second->length, 10U);
    EXPECT_NE(second->torrent, first.torrent);
    EXPECT_FALSE(scroll.sealIncremental(root / "published", provenance));
  }
  UserPermascroll reopened(config());
  ASSERT_EQ(reopened.currentScroll().segments.size(), 2U);
  EXPECT_EQ(reopened.currentScroll().segments.front(), first);
  EXPECT_EQ(reopened.currentScroll().length(), reopened.size());
  EXPECT_FALSE(reopened.sealIncremental(root / "published", provenance));
  const auto permissions =
      std::filesystem::status(root / "author/publication-state/data.mdb")
          .permissions();
  EXPECT_EQ(permissions & (std::filesystem::perms::group_all |
                           std::filesystem::perms::others_all),
            std::filesystem::perms::none);
}

TEST_F(PersistentPermascrollTest, FailedOutputDoesNotCommitASeal) {
  {
    UserPermascroll scroll(config());
    scroll.append("Ideas");
    EXPECT_THROW(scroll.sealIncremental(root / "author/active.primedia", {}),
                 std::filesystem::filesystem_error);
    EXPECT_TRUE(scroll.currentScroll().segments.empty());
  }
  UserPermascroll reopened(config());
  EXPECT_TRUE(reopened.currentScroll().segments.empty());
  const auto first = reopened.sealIncremental(root / "published", {});
  ASSERT_TRUE(first);
  EXPECT_EQ(first->at, 0U);
  EXPECT_EQ(first->length, 5U);
}

TEST_F(PersistentPermascrollTest, AConflictingIdentityIsRefused) {
  {
    UserPermascroll scroll(config());
  }
  auto other       = config();
  other.deviceKeys = xanadu::createMutableKeys();
  EXPECT_THROW(UserPermascroll{other}, xanadu::PermascrollStateUnreadable);
  other          = config();
  other.deviceId = "laptop";
  EXPECT_THROW(UserPermascroll{other}, xanadu::PermascrollStateUnreadable);
}

TEST_F(PersistentPermascrollTest, MissingSealedBytesAreRefused) {
  {
    UserPermascroll scroll(config());
    scroll.append("Ideas");
    ASSERT_TRUE(scroll.sealIncremental(root / "published", {}));
  }
  std::filesystem::resize_file(root / "author/active.primedia", 2);
  EXPECT_THROW(UserPermascroll{config()}, xanadu::PermascrollStateUnreadable);
}

TEST_F(PersistentPermascrollTest, AnUnknownStateVersionIsRefusedByNumber) {
  {
    UserPermascroll scroll(config());
  }
  MDB_env *env = nullptr;
  ASSERT_EQ(mdb_env_create(&env), MDB_SUCCESS);
  std::unique_ptr<MDB_env, decltype(&mdb_env_close)> owned(env, mdb_env_close);
  ASSERT_EQ(
      mdb_env_open(env, (root / "author/publication-state").c_str(), 0, 0600),
      MDB_SUCCESS);
  MDB_txn *txn = nullptr;
  ASSERT_EQ(mdb_txn_begin(env, nullptr, 0, &txn), MDB_SUCCESS);
  MDB_dbi db;
  ASSERT_EQ(mdb_dbi_open(txn, nullptr, 0, &db), MDB_SUCCESS);
  std::string name = "state";
  MDB_val key{name.size(), name.data()}, value{};
  ASSERT_EQ(mdb_get(txn, db, &key, &value), MDB_SUCCESS);
  std::string bytes(static_cast<const char *>(value.mv_data), value.mv_size);
  bytes[3] = '2';
  value    = MDB_val{bytes.size(), bytes.data()};
  ASSERT_EQ(mdb_put(txn, db, &key, &value, 0), MDB_SUCCESS);
  ASSERT_EQ(mdb_txn_commit(txn), MDB_SUCCESS);
  owned.reset();
  try {
    UserPermascroll scroll(config());
    FAIL() << "unknown state version was accepted";
  } catch (const xanadu::PermascrollStateUnreadable &error) {
    EXPECT_THAT(error.what(), testing::HasSubstr("got byte 50"));
  }
}

TEST(UserPermascrollTest, CollaborativeLiveEditingZeroPayload) {
  auto localPermascroll = std::make_shared<UserPermascroll>();
  Store bobStore(localPermascroll);

  // Bob writes his own text in slot 0
  const auto bobV1 = bobStore.insert(MicroversionId{}, 0, "Bob says hello. ");
  const auto bobBytesAfterTyping = localPermascroll->size();

  // Remote collaborator Alice sends a live operation
  const auto aliceKeys = xanadu::createMutableKeys();
  const std::string aliceScrollKey =
      "btpk:" + aliceKeys.publicKey.hex() + ":permascroll";

  Op remoteOp;
  remoteOp.kind   = OpKind::Insert;
  remoteOp.parent = bobV1;
  remoteOp.at     = 16;
  remoteOp.span   = PrimediaSpan{0, 100, 15}; // Alice's offset 100, len 15

  // Apply remote live op with Alice's authorScrollKey (zero raw text passed)
  const auto bobV2 = bobStore.applyRemoteLiveOp(remoteOp, "", aliceScrollKey);

  EXPECT_EQ(bobV2.str(), "2");

  // Bob's local permascroll is NOT polluted with Alice's text
  EXPECT_EQ(localPermascroll->size(), bobBytesAfterTyping);

  // Alice's scroll is registered as an external scroll (> 0)
  const auto *node = bobStore.getCompactOp(bobV2);
  ASSERT_NE(node, nullptr);
  EXPECT_GT(node->scrollId, 0U); // External scroll slot!
  EXPECT_EQ(node->spanStart, 100U);
  EXPECT_EQ(node->spanLength, 15U);
}

namespace {

xanadu::DeviceDelegation fixtureDelegation() {
  return xanadu::testing::fixtureDelegation();
}

} // namespace

TEST(UserPermascrollTest, DeviceDelegationCertificateRoundTrip) {
  const auto cert = fixtureDelegation();

  const auto tsv = cert.toTsv();
  EXPECT_NE(tsv.find("thinkpad-laptop"), std::string::npos);

  const auto decoded = xanadu::DeviceDelegation::fromTsv(tsv);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(*decoded, cert);
}

TEST(UserPermascrollTest, ACertificateWithANonHexDeviceKeyIsRejected) {
  auto tsv         = fixtureDelegation().toTsv();
  const auto field = std::string{"device_public_key\t"};
  const auto at    = tsv.find(field);
  ASSERT_NE(at, std::string::npos);
  const auto valueAt = at + field.size();
  tsv.replace(valueAt, tsv.find('\n', valueAt) - valueAt, "not a key");
  // fromTsv() answers nullopt for a malformed certificate; it used to throw
  // for this one.
  EXPECT_FALSE(xanadu::DeviceDelegation::fromTsv(tsv).has_value());
}

TEST(UserPermascrollTest, DeviceDelegationVerifiesAgainstItsMasterKey) {
  const auto cert = fixtureDelegation();
  EXPECT_TRUE(cert.verify(xanadu::testing::authorMasterPublicKey()));

  // Survives a round trip through TSV, which is how it reaches another
  // machine.
  const auto decoded = xanadu::DeviceDelegation::fromTsv(cert.toTsv());
  ASSERT_TRUE(decoded.has_value());
  EXPECT_TRUE(decoded->verify(xanadu::testing::authorMasterPublicKey()));
}

// This case used to pass with the signature field set to the literal text
// "mock": verify() checked that three fields were non-empty. Every rejection
// below was accepted before there was anything here to reject it.
TEST(UserPermascrollTest, DeviceDelegationRejectsWhatItShould) {
  const auto good = fixtureDelegation();

  auto mockSignature            = good;
  mockSignature.certificate.der = {0x01, 0x02, 0x03};
  EXPECT_FALSE(mockSignature.verify(xanadu::testing::authorMasterPublicKey()))
      << "a corrupt X.509 certificate must not verify";

  // A different key, with a real signature of its own, is still not this
  // delegation's master.
  EXPECT_FALSE(good.verify(xanadu::testing::impostorMasterPublicKey()));

  // Every signed field is covered: changing any one invalidates the whole.
  auto renamed       = good;
  renamed.deviceName = "someone-elses-laptop";
  EXPECT_FALSE(renamed.verify(xanadu::testing::authorMasterPublicKey()));

  // Temporal validity RFC 5280 check:
  // Before notBefore (1700000000)
  auto beforeNotBefore            = good;
  beforeNotBefore.issuedTimestamp = 1699999999;
  EXPECT_FALSE(
      beforeNotBefore.verify(xanadu::testing::authorMasterPublicKey()));

  // After notAfter (1700000000 + 365 * 86400)
  auto afterNotAfter            = good;
  afterNotAfter.issuedTimestamp = 1700000000 + 365 * 86400 + 1;
  EXPECT_FALSE(afterNotAfter.verify(xanadu::testing::authorMasterPublicKey()));

  // Valid timestamp within [notBefore, notAfter]
  auto withinWindow            = good;
  withinWindow.issuedTimestamp = 1700000000 + 1000;
  EXPECT_TRUE(withinWindow.verify(xanadu::testing::authorMasterPublicKey()));

  // Master fingerprint mismatch must be rejected
  auto badMasterFp              = good;
  badMasterFp.masterFingerprint = *xanadu::identity::Fingerprint::fromString(
      xanadu::testing::kImpostorFingerprint);
  EXPECT_FALSE(badMasterFp.verify(xanadu::testing::authorMasterPublicKey()));

  auto swappedDevice = good;
  swappedDevice.devicePublicKey.bytes.fill(0x22);
  EXPECT_FALSE(swappedDevice.verify(xanadu::testing::authorMasterPublicKey()))
      << "a delegation was retargeted to a different device key";

  auto unsignedCert        = good;
  unsignedCert.certificate = {};
  EXPECT_FALSE(unsignedCert.verify(xanadu::testing::authorMasterPublicKey()));

  EXPECT_FALSE(good.verify(xanadu::identity::PubKey32{}))
      << "zero master key means no verification";
}

TEST(UserPermascrollTest, MainDeviceIdIsProtectedAndBlockedInConfig) {
  UserPermascroll::Config cfg;
  cfg.deviceId = "main";
  EXPECT_THROW(UserPermascroll scroll(cfg), std::invalid_argument);
}

TEST(UserPermascrollTest, DeviceActivePrimediaPathHierarchy) {
  const auto tempDir =
      std::filesystem::temp_directory_path() / "xudu_dev_path_test";
  std::error_code ec;
  std::filesystem::remove_all(tempDir, ec);
  std::filesystem::create_directories(tempDir, ec);

  UserPermascroll::Config cfg;
  cfg.storageDir = tempDir;
  UserPermascroll scroll(cfg);

  EXPECT_EQ(scroll.deviceActivePrimediaPath("laptop"),
            tempDir / "devices" / "laptop" / "active.primedia");
  EXPECT_EQ(scroll.deviceActivePrimediaPath("workstation_99"),
            tempDir / "devices" / "workstation_99" / "active.primedia");
  EXPECT_EQ(scroll.deviceActivePrimediaPath("mobile-device-1"),
            tempDir / "devices" / "mobile-device-1" / "active.primedia");

  EXPECT_EQ(scroll.deviceActivePrimediaPath(""), tempDir / "active.primedia");
  EXPECT_EQ(scroll.deviceActivePrimediaPath("main"),
            tempDir / "active.primedia");

  EXPECT_EQ(scroll.deviceActivePrimediaPath("invalid/path"),
            std::filesystem::path{});
  EXPECT_EQ(scroll.deviceActivePrimediaPath("has space"),
            std::filesystem::path{});

  UserPermascroll inMemory;
  EXPECT_TRUE(inMemory.deviceActivePrimediaPath("laptop").empty());

  std::filesystem::remove_all(tempDir, ec);
}

TEST(UserPermascrollTest, IngestSubscrollWritesBytesAndCreatesHierarchy) {
  const auto tempDir =
      std::filesystem::temp_directory_path() / "xudu_ingest_subscroll_test";
  std::error_code ec;
  std::filesystem::remove_all(tempDir, ec);
  std::filesystem::create_directories(tempDir, ec);

  UserPermascroll::Config cfg;
  cfg.storageDir = tempDir;
  UserPermascroll scroll(cfg);

  const std::string text = "Offline laptop keystrokes in subscroll";
  const std::vector<std::uint8_t> data(text.begin(), text.end());

  const auto expectedTarget =
      tempDir / "devices" / "laptop" / "active.primedia";
  EXPECT_FALSE(std::filesystem::exists(expectedTarget.parent_path()));

  const auto subscroll = scroll.ingestSubscroll("laptop", data);

  EXPECT_TRUE(std::filesystem::exists(expectedTarget.parent_path()));
  EXPECT_TRUE(
      std::filesystem::exists(expectedTarget.parent_path() / "segments"));
  EXPECT_TRUE(std::filesystem::exists(expectedTarget));

  std::ifstream in(expectedTarget, std::ios::binary);
  ASSERT_TRUE(in.is_open());
  std::string readContent((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());
  EXPECT_EQ(readContent, text);

  ASSERT_TRUE(subscroll.publisher);
  EXPECT_EQ(*subscroll.publisher, scroll.config().deviceKeys.publicKey);
  EXPECT_EQ(subscroll.salt, "permascroll/laptop");
  EXPECT_TRUE(subscroll.isNamed());
  EXPECT_EQ(xanadu::scrollKey(subscroll).value(),
            "btpk:" + scroll.config().deviceKeys.publicKey.hex() +
                ":permascroll/laptop");

  std::filesystem::remove_all(tempDir, ec);
}

TEST(UserPermascrollTest,
     RegisterSubscrollCopiesExistingPrimediaAndCreatesHierarchy) {
  const auto tempDir =
      std::filesystem::temp_directory_path() / "xudu_register_subscroll_test";
  std::error_code ec;
  std::filesystem::remove_all(tempDir, ec);
  std::filesystem::create_directories(tempDir / "external", ec);

  const auto externalFile = tempDir / "external" / "peer_laptop.primedia";
  {
    std::ofstream out(externalFile, std::ios::binary);
    out << "Registered peer primedia stream";
  }

  UserPermascroll::Config cfg;
  cfg.storageDir = tempDir;
  UserPermascroll scroll(cfg);

  const auto expectedTarget =
      tempDir / "devices" / "peer-station" / "active.primedia";
  EXPECT_FALSE(std::filesystem::exists(expectedTarget.parent_path()));

  const auto subscroll = scroll.registerSubscroll("peer-station", externalFile);

  EXPECT_TRUE(std::filesystem::exists(expectedTarget.parent_path()));
  EXPECT_TRUE(
      std::filesystem::exists(expectedTarget.parent_path() / "segments"));
  EXPECT_TRUE(std::filesystem::exists(expectedTarget));

  std::ifstream in(expectedTarget, std::ios::binary);
  ASSERT_TRUE(in.is_open());
  std::string readContent((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());
  EXPECT_EQ(readContent, "Registered peer primedia stream");

  ASSERT_TRUE(subscroll.publisher);
  EXPECT_EQ(*subscroll.publisher, scroll.config().deviceKeys.publicKey);
  EXPECT_EQ(subscroll.salt, "permascroll/peer-station");
  EXPECT_TRUE(subscroll.isNamed());
  EXPECT_EQ(xanadu::scrollKey(subscroll).value(),
            "btpk:" + scroll.config().deviceKeys.publicKey.hex() +
                ":permascroll/peer-station");

  std::filesystem::remove_all(tempDir, ec);
}

TEST(UserPermascrollTest, SubscrollRejectsInvalidDeviceIds) {
  UserPermascroll scroll;

  const std::vector<std::string> invalidIds = {"",
                                               "main",
                                               "laptop/nested",
                                               "../traversal",
                                               "device with spaces",
                                               "device@domain",
                                               "device:1",
                                               "device.dot",
                                               "device#hash",
                                               "device!bad",
                                               std::string(65, 'a')};

  for (const auto &id : invalidIds) {
    EXPECT_THROW(scroll.registerSubscroll(id, "/tmp/dummy.primedia"),
                 std::invalid_argument)
        << "Expected registerSubscroll to reject invalid device ID: '" << id
        << "'";
    EXPECT_THROW(scroll.ingestSubscroll(id, {}), std::invalid_argument)
        << "Expected ingestSubscroll to reject invalid device ID: '" << id
        << "'";
  }

  EXPECT_NO_THROW(scroll.ingestSubscroll("a", {}));
  EXPECT_NO_THROW(scroll.ingestSubscroll("laptop_01-test", {}));
  EXPECT_NO_THROW(scroll.ingestSubscroll(std::string(64, 'b'), {}));
}

TEST(UserPermascrollTest, UniqueUuidDeviceIdGeneration) {
  const auto id1 = UserPermascroll::generateUniqueDeviceId();
  const auto id2 = UserPermascroll::generateUniqueDeviceId();
  EXPECT_NE(id1, id2);
  EXPECT_EQ(id1.size(), 36U);
  EXPECT_EQ(id2.size(), 36U);
  EXPECT_EQ(id1[8], '-');
  EXPECT_EQ(id1[13], '-');
  EXPECT_EQ(id1[14], '4'); // RFC 4122 version 4
  EXPECT_EQ(id1[18], '-');
  EXPECT_TRUE(id1[19] == '8' || id1[19] == '9' || id1[19] == 'a' ||
              id1[19] == 'b'); // RFC 4122 variant 1
  EXPECT_EQ(id1[23], '-');
}

TEST(UserPermascrollTest, PermascrollRegistrySingleton) {
  auto &reg = PermascrollRegistry::instance();
  reg.clear();

  const auto fp1 =
      Fingerprint::fromString("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
  ASSERT_TRUE(fp1.has_value());

  const auto scroll1a = reg.getOrCreate(*fp1);
  const auto scroll1b = reg.getOrCreate(*fp1);
  EXPECT_EQ(scroll1a, scroll1b);

  const auto def1 = reg.defaultUser();
  const auto def2 = reg.defaultUser();
  EXPECT_EQ(def1, def2);

  reg.clear();
}

// -- the lock-free read path --------------------------------------------------
//
// Migration step 17. read()/readView()/size()/bytes() take no lock, and what
// makes that sound is one acquire/release pair on how much has been published
// plus an arena whose base address never moves. These run under the
// sanitizers in the DEBUG build, which is where a missed ordering would show
// up as a TSan report rather than as a wrong answer.

TEST(UserPermascrollTest, readingWhileAnotherThreadAppendsNeverSeesAHalfSpan) {
  UserPermascroll scroll;
  // Each record is its own recognisable block, so a reader that saw a
  // half-written append would come back with something that is not any record.
  constexpr int records   = 2000;
  const std::string block = "0123456789abcdef";

  std::vector<PrimediaSpan> spans;
  spans.reserve(records);
  std::atomic<int> published{0};
  std::atomic<bool> reading{true};
  std::atomic<int> readsChecked{0};

  std::thread reader([&] {
    bool keepGoing = true;
    while (keepGoing) {
      if (!reading.load(std::memory_order_acquire)) {
        keepGoing = false;
      }
      const auto have = published.load(std::memory_order_acquire);
      for (int i = 0; i < have; i++) {
        // Only spans the appender has already published are read, which is the
        // contract: a span handed out by append() names bytes that are there.
        const auto view = scroll.readView(spans[static_cast<std::size_t>(i)]);
        ASSERT_EQ(view.size(), block.size());
        ASSERT_EQ(std::string(view), block) << "record " << i;
        readsChecked.fetch_add(1, std::memory_order_relaxed);
      }
      // And the size never goes backwards or reports bytes that are not there.
      ASSERT_LE(static_cast<std::size_t>(have) * block.size(), scroll.size());
    }
  });

  for (int i = 0; i < records; i++) {
    spans.push_back(scroll.append(block));
    published.store(i + 1, std::memory_order_release);
  }
  reading.store(false, std::memory_order_release);
  reader.join();

  EXPECT_EQ(scroll.size(), static_cast<std::uint64_t>(records) * block.size());
  EXPECT_GT(readsChecked.load(), 0);
  for (int i = 0; i < records; i++) {
    EXPECT_EQ(scroll.read(spans[static_cast<std::size_t>(i)]), block)
        << "record " << i << " after the fact";
  }
}

TEST(UserPermascrollTest, aViewSurvivesEveryLaterAppend) {
  UserPermascroll scroll;
  const auto first = scroll.append("the first thing typed");
  const auto view  = scroll.readView(first);
  ASSERT_EQ(std::string(view), "the first thing typed");

  // Past the 4 KiB page the first append committed, and past several more, so
  // the arena has had to commit pages underneath the view's neighbourhood.
  for (int i = 0; i < 5000; i++) {
    std::ignore = scroll.append("filler filler filler filler ");
  }

  // Still the same bytes at the same address: an arena that reserved its
  // address space once and commits in place cannot move what a view points at,
  // which is the property that lets the render thread hold one across a
  // keystroke. A std::string or a vector would have reallocated long ago.
  EXPECT_EQ(std::string(view), "the first thing typed");
  EXPECT_EQ(view.data(), scroll.readView(first).data());
}

TEST(UserPermascrollTest, aSpanPastTheEndIsClampedRatherThanRead) {
  UserPermascroll scroll;
  const auto span = scroll.append("twelve bytes");

  // What the clamp is for: a reader asking for more than was published gets
  // what was published, never uninitialised arena. The difference matters
  // because the reader may be a frame behind the appender rather than wrong.
  EXPECT_EQ(scroll.readView(PrimediaSpan{span.scroll, 0, 100}), "twelve bytes");
  EXPECT_EQ(scroll.read(PrimediaSpan{span.scroll, 6, 100}), " bytes");
  EXPECT_TRUE(scroll.readView(PrimediaSpan{span.scroll, 12, 4}).empty());
  EXPECT_TRUE(scroll.readView(PrimediaSpan{span.scroll, 9999, 4}).empty());
}

} // namespace
