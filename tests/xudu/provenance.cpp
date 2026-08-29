/**
 * @file provenance.cpp
 * @brief The authorship record, its signature, and what it is sealed into.
 *
 * The ed25519 key a publication is signed with is minted by this program and
 * bound to no person; it answers "the same publisher as last time" and nothing
 * about who that is. The record tested here is the other half: a person's name
 * and email, signed with their OpenPGP key, written before the content is
 * sealed and sealed into the same torrent -- so the info hash covers the
 * content and the claim about who wrote it together, and neither can be
 * swapped for another afterwards.
 *
 * The signing tests need gpg and a keyring to sign with. They make their own
 * in a temporary directory rather than touching anybody's, and say so and skip
 * when the environment will not let them: a machine without gpg cannot publish
 * either, and that is a fact about the machine rather than a failure here.
 */
#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <xudu/core/binary_ops.hpp>
#include <xudu/core/provenance.hpp>
#include <xudu/core/publication.hpp>
#include <xudu/core/store.hpp>
#include <xudu/core/torrent.hpp>

namespace {

using xudu::Author;
using xudu::MicroversionId;
using xudu::Provenance;
using xudu::SignedProvenance;
using xudu::Store;

/// A record with every field filled in, including the awkward ones.
Provenance sample() {
  Provenance record;
  record.author.name   = "Ada Lovelace";
  record.author.email  = "ada@example.org";
  record.author.gpgKey = "0xDEADBEEF";
  record.title         = "Notes: on the Analytical Engine";
  record.salt          = "notes";
  record.publisher     = std::string(64, 'a');
  record.version       = "2a4";
  record.published     = 1700000000;
  record.contentLength = 4096;
  record.contentDigest = std::string(64, 'b');
  record.quotes        = {"btpk:" + std::string(64, 'c') + ":permascroll"};
  return record;
}

/// The same record, asking to be signed by the key the Keyring below makes.
Provenance signable() {
  auto record          = sample();
  record.author.gpgKey = "ada@example.org";
  return record;
}

/**
 * @brief A keyring of its own, with one key in it.
 *
 * The home directory is short and outside the store because gpg-agent's socket
 * lives in it and a long path exceeds what a unix socket name can hold -- an
 * error that reads as "no agent running" and is nothing of the kind.
 */
class Keyring {
public:
  Keyring() {
    path = std::filesystem::temp_directory_path() /
           ("xudu-gpg-" + std::to_string(getpid()));
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
    std::filesystem::create_directories(path);
    std::filesystem::permissions(path, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace,
                                 ignored);
    setenv("GNUPGHOME", path.c_str(), 1);

    // Loopback, because there is no terminal here to type a passphrase into
    // and the key is made without one.
    {
      std::ofstream agent(path / "gpg-agent.conf");
      agent << "allow-loopback-pinentry\n";
      std::ofstream options(path / "gpg.conf");
      options << "pinentry-mode loopback\n";
    }
    made = 0 == std::system(("gpg --batch --passphrase '' --quick-generate-key "
                             "'Ada Lovelace <ada@example.org>' ed25519 sign "
                             "never >/dev/null 2>&1"));
  }
  ~Keyring() {
    std::system("gpgconf --kill gpg-agent >/dev/null 2>&1");
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
    unsetenv("GNUPGHOME");
  }

  Keyring(const Keyring &)            = delete;
  Keyring &operator=(const Keyring &) = delete;
  Keyring(Keyring &&)                 = delete;
  Keyring &operator=(Keyring &&)      = delete;

  /// Whether there is a key here to sign with.
  [[nodiscard]] bool usable() const { return made; }

private:
  std::filesystem::path path;
  bool made{};
};

} // namespace

// The record is meant to be read by a person deciding whether to believe it,
// so it says what it is and how to check it before it says anything else.
TEST(ProvenanceTest, theRecordExplainsItselfAndNamesTheAuthor) {
  const auto yaml = sample().toYaml();
  EXPECT_TRUE(yaml.contains("gpg --verify"));
  EXPECT_TRUE(yaml.contains("author: \"Ada Lovelace\""));
  EXPECT_TRUE(yaml.contains("email: \"ada@example.org\""));
  // And what it covers, so a reader with the bytes can tell whether the record
  // is about what arrived with it.
  EXPECT_TRUE(yaml.contains("content_length: 4096"));
  EXPECT_TRUE(yaml.contains("content_sha256:"));
}

TEST(ProvenanceTest, everyFieldComesBackOutAgain) {
  const auto record = sample();
  const auto read   = xudu::parseProvenance(record.toYaml());
  ASSERT_TRUE(read.has_value());
  EXPECT_EQ(read->author, record.author);
  // A title with a colon in it is ordinary and would end a plain YAML scalar
  // in the wrong place, which is why every value is quoted.
  EXPECT_EQ(read->title, record.title);
  EXPECT_EQ(read->salt, record.salt);
  EXPECT_EQ(read->publisher, record.publisher);
  EXPECT_EQ(read->version, record.version);
  EXPECT_EQ(read->published, record.published);
  EXPECT_EQ(read->contentLength, record.contentLength);
  EXPECT_EQ(read->contentDigest, record.contentDigest);
  EXPECT_EQ(read->quotes, record.quotes);
}

TEST(ProvenanceTest, aNameWithQuotesAndNewlinesInItSurvives) {
  Provenance record   = sample();
  record.author.name  = "A \"quoted\" name\nwith a line break";
  record.author.email = "odd@example.org";
  const auto read     = xudu::parseProvenance(record.toYaml());
  ASSERT_TRUE(read.has_value());
  EXPECT_EQ(read->author.name, record.author.name);
}

TEST(ProvenanceTest, textThatIsNotARecordIsNotReadAsOne) {
  EXPECT_FALSE(xudu::parseProvenance("").has_value());
  EXPECT_FALSE(xudu::parseProvenance("# only a comment\n").has_value());
  // A record with no author names nobody, whatever else it says.
  EXPECT_FALSE(xudu::parseProvenance("title: \"Something\"\n").has_value());
}

TEST(ProvenanceTest, signingRefusesForAnAuthorWithNoNameOrEmail) {
  Provenance record;
  record.title = "Anonymous";
  EXPECT_THROW(static_cast<void>(xudu::signProvenance(record)),
               std::runtime_error);

  record.author.name = "Only a name";
  EXPECT_THROW(static_cast<void>(xudu::signProvenance(record)),
               std::runtime_error);
}

// What the whole thing rests on: gpg made the signature, and gpg accepts it.
// Not this program's own verifier agreeing with this program's own signer,
// which would prove nothing about whether anybody else can check it.
TEST(ProvenanceTest, gpgSignsTheRecordAndAcceptsItAgain) {
  const Keyring keys;
  if (!keys.usable()) {
    GTEST_SKIP() << "no gpg keyring could be made here";
  }

  const auto record  = signable();
  const auto signed_ = xudu::signProvenance(record);
  EXPECT_EQ(signed_.yaml, record.toYaml());
  EXPECT_TRUE(signed_.signature.starts_with("-----BEGIN PGP SIGNATURE-----"));

  const auto check = xudu::verifyProvenance(signed_);
  EXPECT_TRUE(check.signatureValid) << check.detail;
  EXPECT_TRUE(check.keyTrusted) << "the key was made here, so it is ours";
  EXPECT_TRUE(check.signer.contains("Ada Lovelace")) << check.signer;
  EXPECT_FALSE(check.fingerprint.empty());
}

// The point of signing the record rather than trusting it: a record altered
// after the fact does not verify, so the name on it cannot be changed to
// somebody else's.
TEST(ProvenanceTest, anAlteredRecordDoesNotVerify) {
  const Keyring keys;
  if (!keys.usable()) {
    GTEST_SKIP() << "no gpg keyring could be made here";
  }

  auto signed_ = xudu::signProvenance(signable());
  ASSERT_TRUE(xudu::verifyProvenance(signed_).signatureValid);

  auto forged = signed_;
  forged.yaml = std::string{forged.yaml}.replace(
      forged.yaml.find("Ada Lovelace"), std::string("Ada Lovelace").size(),
      "Someone Else");
  const auto check = xudu::verifyProvenance(forged);
  EXPECT_FALSE(check.signatureValid);

  // And a record with the signature stripped is not a weaker record, it is an
  // unsigned claim.
  auto unsigned_      = signed_;
  unsigned_.signature = "";
  EXPECT_FALSE(xudu::verifyProvenance(unsigned_).signatureValid);
}

// The ordering the whole design turns on: the record is signed first, and the
// seal covers the content and the record together.
TEST(ProvenanceTest, theSealCarriesTheContentAndTheRecordUnderOneHash) {
  const Keyring keys;
  if (!keys.usable()) {
    GTEST_SKIP() << "no gpg keyring could be made here";
  }

  Store store;
  static_cast<void>(store.insert(MicroversionId{}, 0, "Written here first."));
  const auto &bytes = store.primedia().bytes();

  auto record          = signable();
  record.contentLength = bytes.size();
  record.contentDigest = xudu::sha256Hex(bytes);
  const auto signed_   = xudu::signProvenance(record);

  const auto mine = xudu::createMutableKeys();
  const auto sealed =
      xudu::sealLocalSpool(store, mine, "primedia", "", signed_);

  const auto meta = xudu::Metainfo::parse(sealed.torrentFile);
  EXPECT_EQ(meta.hash(), sealed.hash);
  ASSERT_EQ(meta.files().size(), 4U);

  // The content is file zero at offset zero, which is what keeps every address
  // already handed out pointing where it did.
  EXPECT_EQ(meta.files()[0].path, "primedia");
  EXPECT_EQ(meta.files()[0].offset, 0U);
  EXPECT_EQ(meta.files()[0].length, bytes.size());
  EXPECT_EQ(sealed.scroll.segments.at(0).fileIndex, 0U);
  EXPECT_EQ(sealed.scroll.segments.at(0).streamOffset, 0U);
  EXPECT_EQ(sealed.scroll.segments.at(0).at, 0U);

  // The operations follow it: a xanadoc is its history, so what is sealed is
  // the whole tree and not the state it reached. In the compact encoding,
  // which is what crosses machines -- the array of nodes a store keeps is
  // native-endian and means nothing on the other end.
  const auto ops = store.exportBinaryOps();
  EXPECT_EQ(meta.files()[1].path, xudu::sealedOpsName);
  EXPECT_EQ(meta.files()[1].length, ops.size());
  EXPECT_EQ(meta.files()[1].offset, bytes.size());

  // And the record travels with it, under the same hash.
  EXPECT_EQ(meta.files()[2].path, xudu::provenanceFileName);
  EXPECT_EQ(meta.files()[2].length, signed_.yaml.size());
  EXPECT_EQ(meta.files()[3].path, xudu::provenanceSigName);
  EXPECT_EQ(meta.files()[3].length, signed_.signature.size());
  EXPECT_EQ(meta.totalLength(), bytes.size() + ops.size() +
                                    signed_.yaml.size() +
                                    signed_.signature.size());

  // Sealing the same content with a different record is a different address:
  // the two cannot be substituted for one another.
  auto other          = record;
  other.author.name   = "Someone Else";
  other.author.email  = "else@example.org";
  const auto elsewise = xudu::sealLocalSpool(store, mine, "primedia", "",
                                             xudu::signProvenance(other));
  EXPECT_NE(elsewise.hash, sealed.hash);
}

TEST(ProvenanceTest, theSealedOperationsAreTheWholeTreeAndDecodeBack) {
  // What is sealed has to be a history somebody else can actually replay --
  // every branch of it, not the ancestry of whichever state was published.
  Store store;
  const auto one   = store.insert(MicroversionId{}, 0, "hello");
  const auto two   = store.insert(one, 5, " world");
  const auto three = store.erase(two, 0, 1);
  // Two branches off an early state, one of them with a future of its own.
  const auto branched = store.insert(one, 5, " there");
  store.insert(branched, 0, "X");
  store.insert(one, 5, " again");

  const auto encoded = store.exportBinaryOps();
  ASSERT_FALSE(encoded.empty());

  std::istringstream in(encoded, std::ios::binary);
  std::vector<xudu::OpRecord> records;
  xudu::readOpsSpool(in, records);
  EXPECT_EQ(records.size(), store.opCount())
      << "the seal carried a version rather than a history";

  // Replayed into an empty store, every state comes back -- including the
  // branches, which a reader given only the published version's pieces could
  // not have reached at all. Compared as pieces rather than as text: the
  // spans are what the operations produce, and the bytes they name travel in
  // the torrent's other file.
  Store reader;
  for (const auto &record : records) {
    reader.putOp(record.produces, record.op);
  }
  EXPECT_EQ(reader.opCount(), store.opCount());
  EXPECT_EQ(reader.allVersions().size(), store.allVersions().size());
  for (const auto &id : store.allVersions()) {
    EXPECT_TRUE(reader.getOp(id).has_value()) << id.str() << " did not travel";
    EXPECT_EQ(reader.rebuild(id).pieces(), store.rebuild(id).pieces())
        << id.str() << " rebuilt differently";
  }
  EXPECT_EQ(reader.rebuild(three).pieces(), store.rebuild(three).pieces());
  EXPECT_EQ(reader.rebuild(branched).pieces(),
            store.rebuild(branched).pieces());
}

TEST(ProvenanceTest, sealingWithoutASignedRecordIsRefused) {
  Store store;
  static_cast<void>(store.insert(MicroversionId{}, 0, "Written here first."));
  const auto mine = xudu::createMutableKeys();

  EXPECT_THROW(
      static_cast<void>(xudu::sealLocalSpool(store, mine, "primedia", "", {})),
      std::runtime_error);
  // Half of one is no better: a record with no signature over it is a claim
  // anybody could have written.
  SignedProvenance halfway;
  halfway.yaml = sample().toYaml();
  EXPECT_THROW(static_cast<void>(
                   xudu::sealLocalSpool(store, mine, "primedia", "", halfway)),
               std::runtime_error);
}
