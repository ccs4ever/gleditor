/**
 * @file identity_test.cpp
 * @brief Comprehensive tests for BitTorrent-native decentralized identity.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <xudu/core/identity/identity_layout.hpp>
#include <xudu/core/identity/identity_network_controller.hpp>
#include <xudu/core/identity/identity_serialization.hpp>
#include <xudu/core/identity/identity_validation.hpp>
#include <xudu/core/swarm.hpp>
#include <xudu/core/user_permascroll.hpp>

#include "pgp_fixture.hpp"

namespace xudu::identity {
namespace {

using ::testing::Eq;
using ::testing::IsNull;
using ::testing::Ne;
using ::testing::NotNull;

// ============================================================================
// Layout & Hardening Tests
// ============================================================================

TEST(IdentityLayoutTest, FingerprintNormalizationAndValidation) {
  const std::string raw =
      "4a 5b 6c 7d 8e 9f 00 11 22 33 44 55 66 77 88 99 aa bb cc dd";
  const auto fpOpt = Fingerprint::fromString(raw);
  ASSERT_TRUE(fpOpt.has_value());
  EXPECT_TRUE(fpOpt->isValid());
  EXPECT_THAT(fpOpt->toString(),
              Eq("4A5B6C7D8E9F00112233445566778899AABBCCDD"));

  // Invalid hex characters must fail
  EXPECT_FALSE(
      Fingerprint::fromString("4A5B6C7D8E9F00112233445566778899AABBCCDG")
          .has_value());

  // Wrong lengths must fail
  EXPECT_FALSE(Fingerprint::fromString("4A5B6C7D").has_value());
  EXPECT_FALSE(
      Fingerprint::fromString("4A5B6C7D8E9F00112233445566778899AABBCCDDEE")
          .has_value());
}

TEST(IdentityLayoutTest, ConstantTimeComparison) {
  const auto fp1 =
      *Fingerprint::fromString("4A5B6C7D8E9F00112233445566778899AABBCCDD");
  const auto fp2 =
      *Fingerprint::fromString("4A5B6C7D8E9F00112233445566778899AABBCCDD");
  const auto fp3 =
      *Fingerprint::fromString("0000000000000000000000000000000000000000");

  EXPECT_TRUE(constantTimeEquals(fp1.hex, fp2.hex));
  EXPECT_FALSE(constantTimeEquals(fp1.hex, fp3.hex));
  EXPECT_THAT(fp1, Eq(fp2));
  EXPECT_THAT(fp1, Ne(fp3));
}

TEST(IdentityLayoutTest, Hash32HexRoundTrip) {
  const std::string hex =
      "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  const auto hOpt = Hash32::fromHex(hex);
  ASSERT_TRUE(hOpt.has_value());
  EXPECT_FALSE(hOpt->isZero());
  EXPECT_THAT(hOpt->toHex(), Eq(hex));

  EXPECT_FALSE(Hash32::fromHex("invalid_short").has_value());
}

// ============================================================================
// Serialization & Zero-Copy Tests
// ============================================================================

TEST(IdentitySerializationTest, IdentityEntryRoundTrip) {
  IdentityEntry entry;
  entry.fingerprint =
      *Fingerprint::fromString("1111222233334444555566667777888899990000");
  entry.email            = "alice@example.org";
  entry.identityName     = "Alice Adams";
  entry.publicKeyArmored = "-----BEGIN PGP PUBLIC KEY BLOCK-----\ntest\n";
  entry.timestamp        = 1700000000;
  entry.sequence         = 42;
  entry.revoked          = false;
  entry.signature.bytes.fill(0x7A);

  const std::string serialized = serialize(entry);
  EXPECT_FALSE(serialized.empty());

  const auto spanBytes = std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t *>(serialized.data()),
      serialized.size());
  const auto decodedRes = decodeIdentityEntry(spanBytes);
  ASSERT_TRUE(decodedRes.has_value());
  EXPECT_THAT(*decodedRes, Eq(entry));
}

TEST(IdentitySerializationTest, VoteEntryRoundTrip) {
  VoteEntry vote;
  vote.voterFingerprint =
      *Fingerprint::fromString("1111222233334444555566667777888899990000");
  vote.candidateOracle =
      *Fingerprint::fromString("AAAABBBBCCCCDDDDEEEEFFFF0000111122223333");
  vote.timestamp = 1700005000;
  vote.sequence  = 7;
  vote.signature.bytes.fill(0x3C);

  const std::string serialized = serialize(vote);
  const auto spanBytes         = std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t *>(serialized.data()),
      serialized.size());
  const auto decodedRes = decodeVoteEntry(spanBytes);
  ASSERT_TRUE(decodedRes.has_value());
  EXPECT_THAT(*decodedRes, Eq(vote));
}

TEST(IdentitySerializationTest, BlockHeaderRoundTrip) {
  BlockHeader header;
  header.blockIndex = 12;
  header.timestamp  = 1700010000;
  header.previousHash.bytes.fill(0x11);
  header.merkleRoot.bytes.fill(0x22);
  header.identityCount = 50;
  header.voteCount     = 100;

  const std::string serialized = serialize(header);
  const auto spanBytes         = std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t *>(serialized.data()),
      serialized.size());
  const auto decodedRes = decodeBlockHeader(spanBytes);
  ASSERT_TRUE(decodedRes.has_value());
  EXPECT_THAT(*decodedRes, Eq(header));
}

TEST(IdentitySerializationTest, OracleAttestationRoundTrip) {
  OracleAttestation att;
  att.oracleFingerprint =
      *Fingerprint::fromString("AAAABBBBCCCCDDDDEEEEFFFF0000111122223333");
  att.targetFingerprint =
      *Fingerprint::fromString("1111222233334444555566667777888899990000");
  att.verifiedEmail    = "target@domain.com";
  att.issuedTimestamp  = 1700000000;
  att.expiresTimestamp = 1700000000 + 86400 * 90;
  att.oracleSignature.bytes.fill(0xEE);

  const std::string serialized = serialize(att);
  const auto spanBytes         = std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t *>(serialized.data()),
      serialized.size());
  const auto decodedRes = decodeOracleAttestation(spanBytes);
  ASSERT_TRUE(decodedRes.has_value());
  EXPECT_THAT(*decodedRes, Eq(att));
}

TEST(IdentitySerializationTest, RejectsMalformedPayloads) {
  // Corrupted non-bencode payload
  const std::string corrupt = "invalid_bencode_string";
  const auto spanBytes      = std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t *>(corrupt.data()), corrupt.size());
  EXPECT_FALSE(decodeIdentityEntry(spanBytes).has_value());
  EXPECT_FALSE(decodeVoteEntry(spanBytes).has_value());
  EXPECT_FALSE(decodeBlockHeader(spanBytes).has_value());
}

// ============================================================================
// Merkle Validation & Consensus Tests
// ============================================================================

TEST(IdentityValidationTest, MerkleTreeAppendAndInclusionProof) {
  EnginePipeline pipeline;
  EXPECT_TRUE(pipeline.empty());

  IdentityEntry entry1;
  entry1.fingerprint =
      *Fingerprint::fromString("1111222233334444555566667777888899990001");
  entry1.email     = "user1@network.org";
  entry1.timestamp = 1000;

  IdentityEntry entry2;
  entry2.fingerprint =
      *Fingerprint::fromString("1111222233334444555566667777888899990002");
  entry2.email     = "user2@network.org";
  entry2.timestamp = 2000;

  const auto [seq1, root1] = pipeline.appendIdentity(entry1);
  EXPECT_THAT(seq1, Eq(0U));
  EXPECT_THAT(pipeline.size(), Eq(1U));
  EXPECT_THAT(pipeline.root(), Eq(root1));

  const auto [seq2, root2] = pipeline.appendIdentity(entry2);
  EXPECT_THAT(seq2, Eq(1U));
  EXPECT_THAT(pipeline.size(), Eq(2U));
  EXPECT_THAT(root2, Ne(root1));

  // Generate and verify Merkle inclusion proof
  const auto proof1 = pipeline.generateProof(0);
  ASSERT_TRUE(proof1.has_value());
  EXPECT_TRUE(
      EnginePipeline::verifyInclusion(entry1, *proof1, pipeline.root()));

  // Tampered entry must fail verification
  auto tampered  = entry1;
  tampered.email = "attacker@evil.org";
  EXPECT_FALSE(
      EnginePipeline::verifyInclusion(tampered, *proof1, pipeline.root()));
}

// The identity tree is a second, independent Merkle implementation, so it
// needs its own domain separation and its own test: fixing merkle_ledger.cpp
// does nothing for this one. See the matching case in merkle_ledger_test.cpp
// for why the separation matters (RFC 6962 section 2.1).
TEST(IdentityValidationTest, LeafAndInteriorHashingAreDomainSeparated) {
  Hash32 left;
  Hash32 right;
  left.bytes.fill(0xA1);
  right.bytes.fill(0xB2);

  // The 64 bytes an interior node hashes over, offered instead as leaf content.
  std::array<std::uint8_t, 64> asLeafContent{};
  std::copy(left.bytes.begin(), left.bytes.end(), asLeafContent.begin());
  std::copy(right.bytes.begin(), right.bytes.end(), asLeafContent.begin() + 32);

  LedgerMerkleProof proof;
  proof.leafHash = left;
  proof.path.push_back(MerkleProofElement{.hash = right, .isLeft = false});

  EXPECT_FALSE(proof.verify(computeLeafHash(asLeafContent)))
      << "leaf and interior hashing share a domain: a 64-byte leaf is "
         "indistinguishable from an interior node over its two halves";
}

TEST(IdentityValidationTest, StagedBlockAtomicCommitAndRollback) {
  EnginePipeline pipeline;

  // Genesis block with initial identities
  IdentityEntry id1;
  id1.fingerprint =
      *Fingerprint::fromString("1111222233334444555566667777888899990001");
  id1.email     = "alice@wonderland.org";
  id1.timestamp = 1000;

  // Build expected Merkle root for staging
  EnginePipeline tempPipeline;
  std::ignore             = tempPipeline.appendIdentity(id1);
  const auto expectedRoot = tempPipeline.root();

  BlockHeader header0;
  header0.blockIndex    = 0;
  header0.timestamp     = 1000;
  header0.merkleRoot    = expectedRoot;
  header0.identityCount = 1;
  header0.voteCount     = 0;

  std::vector<IdentityEntry> blockIdentities = {id1};
  std::vector<VoteEntry> blockVotes          = {};

  // Stage block 0
  auto stageRes =
      pipeline.stageBlock(header0, blockIdentities, blockVotes, 1000);
  ASSERT_TRUE(stageRes.has_value());
  EXPECT_TRUE(pipeline.hasStagedBlock());
  EXPECT_THAT(pipeline.size(), Eq(0U)); // Committed size still 0

  // Rollback and ensure state is clean
  pipeline.rollbackStage();
  EXPECT_FALSE(pipeline.hasStagedBlock());
  EXPECT_THAT(pipeline.size(), Eq(0U));

  // Re-stage and commit
  stageRes = pipeline.stageBlock(header0, blockIdentities, blockVotes, 1000);
  ASSERT_TRUE(stageRes.has_value());
  EXPECT_TRUE(pipeline.commitStage());
  EXPECT_FALSE(pipeline.hasStagedBlock());
  EXPECT_THAT(pipeline.size(), Eq(1U));
  EXPECT_THAT(pipeline.root(), Eq(expectedRoot));
  EXPECT_THAT(pipeline.blockCount(), Eq(1U));
}

TEST(IdentityValidationTest, OracleVotingPowerAndConsensus) {
  EnginePipeline pipeline;

  // Register voter at timestamp 1,000,000
  IdentityEntry voter;
  voter.fingerprint =
      *Fingerprint::fromString("AAAA111122223333444455556666777788889999");
  voter.email     = "voter@domain.org";
  voter.timestamp = 1000000;
  std::ignore     = pipeline.appendIdentity(voter);

  const auto candidateOracle =
      *Fingerprint::fromString("BBBB111122223333444455556666777788889999");

  // Vote cast 10 days after registration (< 30 days) -> voting power = 0
  const std::uint64_t voteTimeYoung = 1000000 + 10 * 86400;
  EXPECT_THAT(pipeline.calculateVotingPower(voter.fingerprint, voteTimeYoung),
              Eq(0U));

  // Vote cast 30 days after registration -> voting power > 0
  const std::uint64_t voteTimeMature = 1000000 + 30 * 86400;
  const std::uint64_t power30 =
      pipeline.calculateVotingPower(voter.fingerprint, voteTimeMature);
  EXPECT_GT(power30, 0U);

  // Vote cast 120 days after registration -> voting power scales up
  const std::uint64_t voteTimeOld = 1000000 + 120 * 86400;
  const std::uint64_t power120 =
      pipeline.calculateVotingPower(voter.fingerprint, voteTimeOld);
  EXPECT_GT(power120, power30);

  // Append mature vote and check active quorum
  VoteEntry vote;
  vote.voterFingerprint = voter.fingerprint;
  vote.candidateOracle  = candidateOracle;
  vote.timestamp        = voteTimeMature;
  std::ignore           = pipeline.appendVote(vote);

  const auto quorum = pipeline.getActiveOracleQuorum(5, voteTimeMature);
  ASSERT_THAT(quorum.size(), Eq(1U));
  EXPECT_THAT(quorum[0], Eq(candidateOracle));
  EXPECT_TRUE(pipeline.isOracleAuthorized(candidateOracle, voteTimeMature));
}

// ============================================================================
// BEP 10 Frame & Peer Plugin Tests
// ============================================================================

TEST(IdentityBEP10Test, ExtendedMessageEnvelopeEncoding) {
  const std::string payload = "d4:test5:valuee";
  const std::string frame =
      encodeExtendedMessage(MessageType::IdentityQuery, payload);

  EXPECT_THAT(frame.size(), Eq(1 + payload.size()));
  EXPECT_THAT(static_cast<MessageType>(frame[0]),
              Eq(MessageType::IdentityQuery));

  const auto spanBytes = std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t *>(frame.data()), frame.size());
  const auto decodedFrame = decodeExtendedMessage(spanBytes);
  ASSERT_TRUE(decodedFrame.has_value());
  EXPECT_THAT(decodedFrame->type, Eq(MessageType::IdentityQuery));
  EXPECT_THAT(decodedFrame->payload.size(), Eq(payload.size()));
}

TEST(IdentityBEP10Test, HandshakeDictionaryPopulation) {
  libtorrent::entry h(libtorrent::entry::dictionary_t);
  IdentityNetworkController controller;
  IdentityPeerPlugin plugin(
      libtorrent::peer_connection_handle(
          std::weak_ptr<libtorrent::aux::peer_connection>{}),
      InfoHash{}, &controller);

  plugin.add_handshake(h);
  ASSERT_THAT(h.type(), Eq(libtorrent::entry::dictionary_t));

  const auto *m = h.find_key("m");
  ASSERT_THAT(m, NotNull());
  EXPECT_THAT(m->type(), Eq(libtorrent::entry::dictionary_t));
  EXPECT_THAT(m->find_key("xudu_identity_lookup")->integer(),
              Eq(kExtIdentityLookupMsgId));
  EXPECT_THAT(m->find_key("xudu_oracle_vote")->integer(),
              Eq(kExtOracleVoteMsgId));
  EXPECT_THAT(m->find_key("xudu_oracle_verify")->integer(),
              Eq(kExtOracleVerifyMsgId));
}

namespace {

/// Drives a peer plugin through the handshake so that it has issued a
/// challenge and is waiting on the response, which is the state every
/// authentication test starts from.
std::shared_ptr<IdentityPeerPlugin>
challengedPlugin(IdentityNetworkController &controller) {
  auto plugin = std::make_shared<IdentityPeerPlugin>(
      libtorrent::peer_connection_handle(
          std::weak_ptr<libtorrent::aux::peer_connection>{}),
      InfoHash{}, &controller);

  const std::string handshake =
      "d1:md20:xudu_identity_lookupi3e16:xudu_oracle_votei4eee";
  libtorrent::bdecode_node node;
  libtorrent::error_code ec;
  libtorrent::bdecode(handshake.data(), handshake.data() + handshake.size(),
                      node, ec);
  EXPECT_FALSE(ec);
  plugin->on_extension_handshake(node);
  EXPECT_TRUE(plugin->pendingChallengeNonce().has_value());
  return plugin;
}

/// The fixed device keypair the checked-in delegation attests to.
xudu::MutableKeys testDeviceKeys() {
  xudu::MutableKeys keys;
  keys.publicKey =
      xudu::PublicKey::fromHex(xudu::testing::kTestDevicePublicKeyHex);
  keys.secretKey =
      xudu::SecretKey::fromHex(xudu::testing::kTestDeviceSecretKeyHex);
  return keys;
}

/// Teaches @p controller that the fixture author delegated to that device
/// key -- the state a peer must reach before any of its signatures count.
[[nodiscard]] bool
trustFixtureDelegation(IdentityNetworkController &controller) {
  xudu::DeviceDelegation cert;
  cert.masterFingerprint =
      *Fingerprint::fromString(xudu::testing::kAuthorFingerprint);
  cert.devicePublicKey = testDeviceKeys().publicKey;
  cert.deviceName      = "peer-under-test";
  cert.issuedTimestamp = 1700000000;
  cert.gpgSignatureArmored =
      std::string(xudu::testing::kDelegationForTestDeviceKey);
  return controller.trustDelegation(cert, xudu::testing::kAuthorPublicKey);
}

/// Signs a challenge nonce the way a well-behaved peer does.
Signature64 signNonce(const Hash32 &nonce, const xudu::MutableKeys &keys) {
  std::string buffer = "xudu-peer-auth-v1:";
  buffer.append(reinterpret_cast<const char *>(nonce.bytes.data()),
                nonce.bytes.size());
  const auto sig = xudu::signMutableItem(buffer, keys);
  Signature64 out;
  std::memcpy(out.bytes.data(), sig.bytes.data(), out.bytes.size());
  return out;
}

/// Hands @p payload to the plugin as an incoming BEP 10 extended message.
bool deliver(IdentityPeerPlugin &plugin, const MessageType type,
             const std::string &bencoded) {
  const std::string frame = encodeExtendedMessage(type, bencoded);
  return plugin.on_extended(static_cast<int>(frame.size()),
                            kExtIdentityLookupMsgId,
                            libtorrent::span<char const>(frame));
}

} // namespace

// The whole chain, end to end: a master OpenPGP key delegates to an Ed25519
// device key, and that device key signs the nonce this connection chose.
TEST(IdentityBEP10Test, AuthenticatesAPeerWithADelegatedDeviceKey) {
  IdentityNetworkController controller;
  ASSERT_TRUE(trustFixtureDelegation(controller));

  auto plugin = challengedPlugin(controller);
  ASSERT_FALSE(plugin->isAuthenticated());

  PeerChallengeResponse resp;
  resp.nonce = *plugin->pendingChallengeNonce();
  resp.claimedIdentity =
      *Fingerprint::fromString(xudu::testing::kAuthorFingerprint);
  resp.devicePublicKey = testDeviceKeys().publicKey.bytes;
  resp.signature       = signNonce(resp.nonce, testDeviceKeys());

  deliver(*plugin, MessageType::PeerAuthResponse, serialize(resp));

  EXPECT_TRUE(plugin->isAuthenticated());
  ASSERT_TRUE(plugin->authenticatedIdentity().has_value());
  EXPECT_THAT(plugin->authenticatedIdentity()->toString(),
              Eq(std::string(xudu::testing::kAuthorFingerprint)));
}

// The bypass, in the form it actually shipped. The check was "the signature
// field is not all zeroes", so any non-zero 64 bytes claimed any fingerprint
// -- including the 0x55 filler the challenge handler itself sent when no
// signing key was configured, which was the default path.
TEST(IdentityBEP10Test, RejectsAuthResponseWithAnUnverifiableSignature) {
  IdentityNetworkController controller;
  ASSERT_TRUE(trustFixtureDelegation(controller));

  auto plugin = challengedPlugin(controller);

  PeerChallengeResponse resp;
  resp.nonce = *plugin->pendingChallengeNonce();
  resp.claimedIdentity =
      *Fingerprint::fromString(xudu::testing::kAuthorFingerprint);
  resp.devicePublicKey = testDeviceKeys().publicKey.bytes;
  resp.signature.bytes.fill(0x55); // not a signature, merely not zero

  deliver(*plugin, MessageType::PeerAuthResponse, serialize(resp));

  EXPECT_FALSE(plugin->isAuthenticated())
      << "a peer authenticated with 64 bytes that sign nothing";
  EXPECT_FALSE(plugin->authenticatedIdentity().has_value());
}

// A peer that signs correctly, but with a key its claimed identity never
// delegated to. The signature verifies against the key it names -- which is
// why the key it names cannot be the one that decides.
TEST(IdentityBEP10Test, RejectsAValidSignatureByAnUndelegatedKey) {
  IdentityNetworkController controller;
  ASSERT_TRUE(trustFixtureDelegation(controller));

  auto plugin        = challengedPlugin(controller);
  const auto ownKeys = xudu::createMutableKeys();

  PeerChallengeResponse resp;
  resp.nonce = *plugin->pendingChallengeNonce();
  resp.claimedIdentity =
      *Fingerprint::fromString(xudu::testing::kAuthorFingerprint);
  resp.devicePublicKey = ownKeys.publicKey.bytes;
  resp.signature       = signNonce(resp.nonce, ownKeys);

  deliver(*plugin, MessageType::PeerAuthResponse, serialize(resp));

  EXPECT_FALSE(plugin->isAuthenticated())
      << "a peer authenticated as an identity using a key of its own";
}

// No delegation on file means the claim cannot be checked, and an unverifiable
// claim is refused rather than believed.
TEST(IdentityBEP10Test, RejectsAnIdentityWithNoDelegationOnFile) {
  IdentityNetworkController controller; // nothing trusted
  auto plugin = challengedPlugin(controller);

  PeerChallengeResponse resp;
  resp.nonce = *plugin->pendingChallengeNonce();
  resp.claimedIdentity =
      *Fingerprint::fromString(xudu::testing::kAuthorFingerprint);
  resp.devicePublicKey = testDeviceKeys().publicKey.bytes;
  resp.signature       = signNonce(resp.nonce, testDeviceKeys());

  deliver(*plugin, MessageType::PeerAuthResponse, serialize(resp));

  EXPECT_FALSE(plugin->isAuthenticated());
}

// A correct signature over the wrong nonce is a replay of an older exchange.
TEST(IdentityBEP10Test, RejectsASignatureOverAStaleNonce) {
  IdentityNetworkController controller;
  ASSERT_TRUE(trustFixtureDelegation(controller));

  auto plugin = challengedPlugin(controller);

  Hash32 otherNonce;
  otherNonce.bytes.fill(0xAA); // the old hardcoded challenge value

  PeerChallengeResponse resp;
  resp.nonce = otherNonce;
  resp.claimedIdentity =
      *Fingerprint::fromString(xudu::testing::kAuthorFingerprint);
  resp.devicePublicKey = testDeviceKeys().publicKey.bytes;
  resp.signature       = signNonce(otherNonce, testDeviceKeys());

  deliver(*plugin, MessageType::PeerAuthResponse, serialize(resp));

  EXPECT_FALSE(plugin->isAuthenticated());
}

// Challenges must be unpredictable. A fixed nonce -- and this was 0xAA
// repeated -- means one captured response authenticates forever, from anyone,
// no matter how sound the signature check over it is.
TEST(IdentityBEP10Test, ChallengeNoncesDifferPerConnection) {
  IdentityNetworkController controller;
  const auto first  = challengedPlugin(controller)->pendingChallengeNonce();
  const auto second = challengedPlugin(controller)->pendingChallengeNonce();

  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_FALSE(first->isZero());
  EXPECT_THAT(*first, Ne(*second));
}

// Votes decide oracle consensus, so a peer that has not authenticated must not
// be able to put one in. The handler used to decode and forward without
// consulting isAuthenticated_ at all.
TEST(IdentityBEP10Test, IgnoresVotesFromUnauthenticatedPeers) {
  IdentityNetworkController controller;
  auto plugin = challengedPlugin(controller);
  ASSERT_FALSE(plugin->isAuthenticated());

  VoteEntry vote;
  vote.voterFingerprint =
      *Fingerprint::fromString("1111222233334444555566667777888899990001");
  vote.candidateOracle =
      *Fingerprint::fromString("1111222233334444555566667777888899990002");
  vote.timestamp = 1700000000;
  vote.signature.bytes.fill(0x3C);

  deliver(*plugin, MessageType::OracleVoteBroadcast, serialize(vote));

  EXPECT_THAT(controller.pipeline().size(), Eq(0U))
      << "an unauthenticated peer got a vote into the ledger";
}

// Having authenticated as one identity does not license voting as another.
TEST(IdentityBEP10Test, RejectsAVoteCastUnderAnotherIdentity) {
  IdentityNetworkController controller;
  ASSERT_TRUE(trustFixtureDelegation(controller));

  auto plugin = challengedPlugin(controller);

  PeerChallengeResponse resp;
  resp.nonce = *plugin->pendingChallengeNonce();
  resp.claimedIdentity =
      *Fingerprint::fromString(xudu::testing::kAuthorFingerprint);
  resp.devicePublicKey = testDeviceKeys().publicKey.bytes;
  resp.signature       = signNonce(resp.nonce, testDeviceKeys());
  deliver(*plugin, MessageType::PeerAuthResponse, serialize(resp));
  ASSERT_TRUE(plugin->isAuthenticated());

  VoteEntry vote;
  vote.voterFingerprint =
      *Fingerprint::fromString("1111222233334444555566667777888899990001");
  vote.candidateOracle =
      *Fingerprint::fromString("1111222233334444555566667777888899990002");
  vote.timestamp = 1700000000;
  vote.signature.bytes.fill(0x3C);

  deliver(*plugin, MessageType::OracleVoteBroadcast, serialize(vote));

  EXPECT_THAT(controller.pipeline().size(), Eq(0U))
      << "an authenticated peer voted as somebody else";
}

// ============================================================================
// Hashcash Proof-of-Work Tests
// ============================================================================

TEST(IdentityHashcashTest, MintAndVerifyProofOfWork) {
  HashcashEngine engine;
  const std::string resource    = "ada@example.org";
  const std::uint64_t timestamp = 1700000000;
  const std::uint8_t difficulty = 16; // 16 bits = ~65536 iterations

  const auto nonceOpt = HashcashEngine::mint(resource, timestamp, difficulty);
  ASSERT_TRUE(nonceOpt.has_value());

  const auto digest =
      HashcashEngine::computeDigest(resource, timestamp, *nonceOpt);
  EXPECT_GE(HashcashEngine::countLeadingZeroBits(digest), difficulty);

  // Verify stamp with engine
  auto verifyRes = engine.verify(resource, timestamp, *nonceOpt, difficulty,
                                 difficulty, timestamp);
  EXPECT_TRUE(verifyRes.has_value());
}

TEST(IdentityHashcashTest, RejectsInsufficientDifficulty) {
  HashcashEngine engine;
  const std::string resource    = "target@domain.org";
  const std::uint64_t timestamp = 1700000000;

  // Mint 8-bit stamp
  const auto nonceOpt = HashcashEngine::mint(resource, timestamp, 8);
  ASSERT_TRUE(nonceOpt.has_value());

  // Verifying requiring 20 bits must fail with InsufficientProofOfWork
  auto verifyRes =
      engine.verify(resource, timestamp, *nonceOpt, 8, 20, timestamp);
  ASSERT_FALSE(verifyRes.has_value());
  EXPECT_THAT(verifyRes.error(), Eq(ValidationError::InsufficientProofOfWork));
}

TEST(IdentityHashcashTest, RejectsExpiredTimestampAntiPremining) {
  HashcashEngine engine;
  const std::string resource       = "target@domain.org";
  const std::uint64_t oldTimestamp = 1700000000;
  const std::uint64_t currentTime =
      1700000000 + 1000; // 1000s later (> 300s skew)

  const auto nonceOpt = HashcashEngine::mint(resource, oldTimestamp, 10);
  ASSERT_TRUE(nonceOpt.has_value());

  auto verifyRes =
      engine.verify(resource, oldTimestamp, *nonceOpt, 10, 10, currentTime);
  ASSERT_FALSE(verifyRes.has_value());
  EXPECT_THAT(verifyRes.error(), Eq(ValidationError::ProofOfWorkExpired));
}

// A zero clock used to mean "skip the skew check", and the peer-wire caller
// passed zero -- so the anti-premining invariant above held in this test file
// and nowhere else. Zero is now just a clock reading like any other, which
// makes a stamp dated well after it as stale as one dated well before.
TEST(IdentityHashcashTest, ZeroClockIsNotAnEscapeHatch) {
  HashcashEngine engine;
  const std::string resource    = "zeroclock@test.org";
  const std::uint64_t timestamp = 1700000000;

  const auto nonceOpt = HashcashEngine::mint(resource, timestamp, 10);
  ASSERT_TRUE(nonceOpt.has_value());

  auto verifyRes = engine.verify(resource, timestamp, *nonceOpt, 10, 10, 0);
  ASSERT_FALSE(verifyRes.has_value());
  EXPECT_THAT(verifyRes.error(), Eq(ValidationError::ProofOfWorkExpired));
}

TEST(IdentityHashcashTest, RejectsReplayedNonce) {
  HashcashEngine engine;
  const std::string resource    = "replay@test.org";
  const std::uint64_t timestamp = 1700000000;
  const std::uint8_t difficulty = 10;

  const auto nonceOpt = HashcashEngine::mint(resource, timestamp, difficulty);
  ASSERT_TRUE(nonceOpt.has_value());

  // First verification succeeds
  auto res1 = engine.verify(resource, timestamp, *nonceOpt, difficulty,
                            difficulty, timestamp);
  EXPECT_TRUE(res1.has_value());

  // Replayed submission must be rejected
  auto res2 = engine.verify(resource, timestamp, *nonceOpt, difficulty,
                            difficulty, timestamp);
  ASSERT_FALSE(res2.has_value());
  EXPECT_THAT(res2.error(), Eq(ValidationError::ProofOfWorkReplayDetected));
}

TEST(IdentityHashcashTest, EmailVerifyRequestPoWSerializationRoundTrip) {
  EmailVerifyRequestMsg req;
  req.requesterFingerprint =
      *Fingerprint::fromString("1111222233334444555566667777888899990000");
  req.targetEmail    = "target@domain.org";
  req.timestamp      = 1700000000;
  req.powNonce       = 123456789;
  req.difficultyBits = 22;
  req.requesterSignature.bytes.fill(0x99);

  const std::string serialized = serialize(req);
  const auto spanBytes         = std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t *>(serialized.data()),
      serialized.size());

  const auto decodedRes = decodeEmailVerifyRequest(spanBytes);
  ASSERT_TRUE(decodedRes.has_value());
  EXPECT_THAT(decodedRes->powNonce, Eq(123456789ULL));
  EXPECT_THAT(decodedRes->difficultyBits, Eq(22U));
  EXPECT_THAT(*decodedRes, Eq(req));
}

} // namespace
} // namespace xudu::identity
