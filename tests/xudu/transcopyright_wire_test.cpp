/**
 * @file transcopyright_wire_test.cpp
 * @brief The BEP 10 transcopyright unlock exchange, driven end to end.
 *
 * Two plugins are wired to each other's on_extended, so every message goes
 * through the real encoders and decoders rather than being asserted about in
 * the abstract. The four handlers this exercises used to decode their payload
 * and return whether it parsed, so all of the behaviour below is new and none
 * of it was previously observable.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <xudu/core/identity/identity_network_controller.hpp>
#include <xudu/core/identity/payment_verifier.hpp>
#include <xudu/core/transcopyright_crypto.hpp>

namespace xudu::identity {
namespace {

using ::testing::Eq;
using ::testing::Ne;

constexpr std::uint64_t kPrice = 100;

Hash32 keyIdFilled(const std::uint8_t byte) {
  Hash32 h;
  h.bytes.fill(byte);
  return h;
}

/// A reader and an author, each holding the other's plugin, with frames
/// handed straight across. `link()` is what makes this a conversation rather
/// than two monologues.
struct Pair {
  IdentityNetworkController authorCtl;
  IdentityNetworkController readerCtl;
  std::shared_ptr<IdentityPeerPlugin> author;
  std::shared_ptr<IdentityPeerPlugin> reader;

  /// Frames the author tried to send after the link was cut, if it was.
  int authorSends{0};
  int readerSends{0};
  bool deliver{true};

  Pair() {
    const auto dead = libtorrent::peer_connection_handle(
        std::weak_ptr<libtorrent::aux::peer_connection>{});
    author = std::make_shared<IdentityPeerPlugin>(dead, InfoHash{}, &authorCtl);
    reader = std::make_shared<IdentityPeerPlugin>(dead, InfoHash{}, &readerCtl);
    // Both ends learn the other's extension ids, as they would from a real
    // BEP 10 handshake. Done before the sinks are wired so that the
    // authentication challenge each side issues here goes nowhere and does
    // not show up in the frame counts below.
    handshake(*author);
    handshake(*reader);
  }

  static void handshake(IdentityPeerPlugin &plugin) {
    const std::string dict = "d1:md20:xudu_identity_lookupi3e16:xudu_oracle_"
                             "votei4e19:xudu_transcopyrighti5eee";
    libtorrent::bdecode_node node;
    libtorrent::error_code ec;
    libtorrent::bdecode(dict.data(), dict.data() + dict.size(), node, ec);
    EXPECT_FALSE(ec);
    plugin.on_extension_handshake(node);
  }

  void link() {
    author->setFrameSink([this](int, std::string_view frame) {
      authorSends++;
      return deliver ? feed(*reader, frame) : true;
    });
    reader->setFrameSink([this](int, std::string_view frame) {
      readerSends++;
      return deliver ? feed(*author, frame) : true;
    });
  }

  static bool feed(IdentityPeerPlugin &to, std::string_view frame) {
    return to.on_extended(static_cast<int>(frame.size()), 0,
                          libtorrent::span<char const>(frame));
  }
};

/// An author selling one span, and the CEK that opens it.
TranscopyrightOffer makeOffer(const Hash32 &keyId, const crypto::Key32 &cek,
                              const bool flatFee = true) {
  TranscopyrightOffer offer;
  offer.keyId                       = keyId;
  offer.cek                         = cek;
  offer.descriptor.priceAtomicUnits = kPrice;
  offer.descriptor.flatFee          = flatFee;
  offer.descriptor.currencySymbol   = "XU";
  offer.descriptor.authorWallet =
      *Fingerprint::fromString("1111222233334444555566667777888899990001");
  offer.descriptor.authorPubKey.bytes.fill(0x42);
  offer.descriptor.keyId = keyId.bytes;
  return offer;
}

Fingerprint payerWallet() {
  return *Fingerprint::fromString("AAAABBBBCCCCDDDDEEEEFFFF0000111122223333");
}

// The whole exchange: query, invoice, settle, key delivery. The reader ends
// up holding the author's CEK, and holds it because it was wrapped under a
// public key the reader minted for this purchase alone.
TEST(TranscopyrightWireTest, APaidReaderReceivesTheKey) {
  Pair pair;
  pair.link();

  const auto keyId = keyIdFilled(0xC1);
  const auto cek   = crypto::generateKey();
  pair.authorCtl.offerTranscopyright(makeOffer(keyId, cek));
  pair.authorCtl.setPaymentVerifier(std::make_shared<AlwaysAcceptVerifier>());

  std::optional<crypto::Key32> delivered;
  std::uint64_t deliveredPrice = 0;
  std::string deliveredCurrency;
  pair.readerCtl.setCekSink([&](const Hash32 &, const crypto::Key32 &k,
                                std::uint64_t price, std::string_view cur) {
    delivered         = k;
    deliveredPrice    = price;
    deliveredCurrency = std::string(cur);
  });

  ASSERT_TRUE(pair.reader->beginTranscopyrightPurchase(keyId, 512,
                                                       payerWallet(), "paid"));

  ASSERT_TRUE(delivered.has_value())
      << "the reader paid and did not get the key";
  EXPECT_THAT(*delivered, Eq(cek));
  EXPECT_THAT(deliveredPrice, Eq(kPrice));
  EXPECT_THAT(deliveredCurrency, Eq("XU"));
  // The purchase is finished, so nothing is left waiting on this connection.
  EXPECT_FALSE(pair.reader->isPurchasing(keyId));
}

// The default. An author who has not said how they get paid has not said how
// they get paid -- which is not the same as giving the work away.
TEST(TranscopyrightWireTest, WithoutASettlementBackendNoKeyIsDelivered) {
  Pair pair;
  pair.link();

  const auto keyId = keyIdFilled(0xC2);
  pair.authorCtl.offerTranscopyright(makeOffer(keyId, crypto::generateKey()));
  // No setPaymentVerifier call: RefusingVerifier is the default.
  EXPECT_FALSE(pair.authorCtl.paymentVerifier().isReal());

  bool delivered = false;
  pair.readerCtl.setCekSink([&](const Hash32 &, const crypto::Key32 &,
                                std::uint64_t,
                                std::string_view) { delivered = true; });

  static_cast<void>(pair.reader->beginTranscopyrightPurchase(
      keyId, 512, payerWallet(), "unpaid"));

  EXPECT_FALSE(delivered);
}

// A span this node does not sell earns no invoice, so a relay cannot resell
// somebody else's work by forwarding queries.
TEST(TranscopyrightWireTest, ANodeWithNoOfferDoesNotInvoice) {
  Pair pair;
  pair.link();

  bool delivered = false;
  pair.readerCtl.setCekSink([&](const Hash32 &, const crypto::Key32 &,
                                std::uint64_t,
                                std::string_view) { delivered = true; });

  static_cast<void>(pair.reader->beginTranscopyrightPurchase(
      keyIdFilled(0xC3), 512, payerWallet(), "paid"));

  EXPECT_FALSE(delivered);
}

// The challenge is spent when it is used, so replaying a captured settlement
// request buys nothing the second time.
TEST(TranscopyrightWireTest, APaymentChallengeWorksExactlyOnce) {
  IdentityNetworkController ctl;
  const auto keyId = keyIdFilled(0xC4);
  ctl.offerTranscopyright(makeOffer(keyId, crypto::generateKey()));

  const auto challenge = ctl.issuePaymentChallenge(keyId);
  EXPECT_FALSE(challenge.isZero());

  EXPECT_TRUE(ctl.consumePaymentChallenge(keyId, challenge));
  EXPECT_FALSE(ctl.consumePaymentChallenge(keyId, challenge))
      << "a spent challenge was accepted a second time";
}

TEST(TranscopyrightWireTest, RejectsChallengesNeverIssued) {
  IdentityNetworkController ctl;
  const auto keyId = keyIdFilled(0xC5);
  ctl.offerTranscopyright(makeOffer(keyId, crypto::generateKey()));

  EXPECT_FALSE(ctl.consumePaymentChallenge(keyId, keyIdFilled(0x99)));
  EXPECT_FALSE(ctl.consumePaymentChallenge(keyId, Hash32{}))
      << "an all-zero challenge was treated as one we issued";

  // A challenge issued for one span does not settle another.
  const auto other = keyIdFilled(0xC6);
  const auto valid = ctl.issuePaymentChallenge(keyId);
  EXPECT_FALSE(ctl.consumePaymentChallenge(other, valid));
}

TEST(TranscopyrightWireTest, ChallengesAreUnpredictable) {
  IdentityNetworkController ctl;
  const auto keyId = keyIdFilled(0xC7);
  EXPECT_THAT(ctl.issuePaymentChallenge(keyId),
              Ne(ctl.issuePaymentChallenge(keyId)));
}

// An invoice for something this reader never asked about is unsolicited. If
// it were acted on, any peer could start a purchase on the reader's behalf.
TEST(TranscopyrightWireTest, IgnoresAnUnsolicitedInvoice) {
  Pair pair;
  pair.link();

  bool delivered = false;
  pair.readerCtl.setCekSink([&](const Hash32 &, const crypto::Key32 &,
                                std::uint64_t,
                                std::string_view) { delivered = true; });

  TcInvoiceResponseMsg unsolicited;
  unsolicited.keyId            = keyIdFilled(0xC8);
  unsolicited.priceAtomicUnits = kPrice;
  unsolicited.currencySymbol   = "XU";
  unsolicited.authorWallet =
      *Fingerprint::fromString("1111222233334444555566667777888899990001");
  unsolicited.authorPubKey.bytes.fill(0x42);
  unsolicited.paymentChallenge = keyIdFilled(0x77);

  const auto frame = encodeExtendedMessage(MessageType::TcInvoiceResponse,
                                           serialize(unsolicited));
  Pair::feed(*pair.reader, frame);

  EXPECT_FALSE(delivered);
  EXPECT_THAT(pair.readerSends, Eq(0))
      << "the reader answered an invoice it never asked for";
}

// A key wrapped for somebody else must not be accepted, even for a purchase
// this reader really is making.
TEST(TranscopyrightWireTest, RejectsAKeyWrappedForAnotherReader) {
  Pair pair;
  pair.deliver = false; // stop at the query; deliver the key by hand
  pair.link();

  const auto keyId = keyIdFilled(0xC9);
  bool delivered   = false;
  pair.readerCtl.setCekSink([&](const Hash32 &, const crypto::Key32 &,
                                std::uint64_t,
                                std::string_view) { delivered = true; });

  ASSERT_TRUE(pair.reader->beginTranscopyrightPurchase(keyId, 512,
                                                       payerWallet(), "paid"));
  ASSERT_TRUE(pair.reader->isPurchasing(keyId));

  // Wrapped under an unrelated keypair, not the one this purchase minted.
  const auto eavesdropper = crypto::X25519KeyPair::generate();
  TcKeyDeliveryMsg delivery;
  delivery.keyId = keyId;
  delivery.wrappedCek =
      crypto::wrapCek(crypto::generateKey(), eavesdropper.publicKey);

  Pair::feed(*pair.reader, encodeExtendedMessage(MessageType::TcKeyDelivery,
                                                 serialize(delivery)));

  EXPECT_FALSE(delivered)
      << "a CEK wrapped for a different reader was accepted";
  EXPECT_TRUE(pair.reader->isPurchasing(keyId))
      << "the purchase was closed by a key that could not be opened";
}

// Per-byte pricing multiplies by the bytes asked for; flat pricing does not.
TEST(TranscopyrightWireTest, PerByteAndFlatPricingDiffer) {
  const auto keyId = keyIdFilled(0xCA);

  for (const bool flat : {true, false}) {
    Pair pair;
    pair.link();
    pair.authorCtl.offerTranscopyright(
        makeOffer(keyId, crypto::generateKey(), flat));
    pair.authorCtl.setPaymentVerifier(std::make_shared<AlwaysAcceptVerifier>());

    std::uint64_t price = 0;
    pair.readerCtl.setCekSink([&](const Hash32 &, const crypto::Key32 &,
                                  std::uint64_t p,
                                  std::string_view) { price = p; });

    ASSERT_TRUE(pair.reader->beginTranscopyrightPurchase(
        keyId, 4, payerWallet(), "paid"));
    EXPECT_THAT(price, Eq(flat ? kPrice : kPrice * 4));
  }
}

// ============================================================================
// Identity lookup: the proof has to travel with the entry, and be checked
// ============================================================================

namespace {

IdentityEntry identityFor(const std::string &fp, const std::string &email) {
  IdentityEntry entry;
  entry.fingerprint = *Fingerprint::fromString(fp);
  entry.email       = email;
  entry.timestamp   = 1700000000;
  return entry;
}

} // namespace

// An identity query used to come back as a bare entry -- sendIdentityResponse
// took the proof and dropped it -- so a lookup told you what a peer wished
// were true. The proof now travels with the entry and is checked against the
// asking node's own root.
TEST(IdentityLookupWireTest, AProvenIdentityIsAccepted) {
  Pair pair;

  const auto alice =
      identityFor("1111222233334444555566667777888899990001", "a@test.org");
  const auto bob =
      identityFor("1111222233334444555566667777888899990002", "b@test.org");

  // Both ends follow the same ledger, so their roots agree.
  for (auto *ctl : {&pair.authorCtl, &pair.readerCtl}) {
    static_cast<void>(ctl->pipeline().appendIdentity(alice));
    static_cast<void>(ctl->pipeline().appendIdentity(bob));
  }
  ASSERT_THAT(pair.readerCtl.pipeline().root(),
              Eq(pair.authorCtl.pipeline().root()));

  pair.link();

  IdentityQueryMsg query;
  query.targetFingerprint = alice.fingerprint;
  Pair::feed(*pair.author, encodeExtendedMessage(MessageType::IdentityQuery,
                                                 serialize(query)));

  ASSERT_TRUE(pair.reader->lastVerifiedIdentity().has_value());
  EXPECT_THAT(pair.reader->lastVerifiedIdentity()->email, Eq("a@test.org"));
}

// The same answer, offered to a node following a different ledger. The entry
// is well formed and its proof is internally consistent; it just is not a
// proof about anything this node believes.
TEST(IdentityLookupWireTest, RejectsAnIdentityFromAnotherLedger) {
  Pair pair;

  const auto alice =
      identityFor("1111222233334444555566667777888899990001", "a@test.org");
  static_cast<void>(pair.authorCtl.pipeline().appendIdentity(alice));
  // The reader's ledger has someone else in it, so its root differs.
  static_cast<void>(pair.readerCtl.pipeline().appendIdentity(
      identityFor("1111222233334444555566667777888899990009", "z@test.org")));
  ASSERT_THAT(pair.readerCtl.pipeline().root(),
              Ne(pair.authorCtl.pipeline().root()));

  pair.link();

  IdentityQueryMsg query;
  query.targetFingerprint = alice.fingerprint;
  Pair::feed(*pair.author, encodeExtendedMessage(MessageType::IdentityQuery,
                                                 serialize(query)));

  EXPECT_FALSE(pair.reader->lastVerifiedIdentity().has_value())
      << "an identity was accepted on a proof about a ledger we do not follow";
}

// A response whose entry has been swapped for another after the proof was
// made. computeLeafHash no longer matches the proof's leaf.
TEST(IdentityLookupWireTest, RejectsATamperedEntry) {
  Pair pair;
  const auto alice =
      identityFor("1111222233334444555566667777888899990001", "a@test.org");
  static_cast<void>(pair.readerCtl.pipeline().appendIdentity(alice));

  const auto proof = pair.readerCtl.pipeline().generateProof(0);
  ASSERT_TRUE(proof.has_value());

  IdentityResponseMsg resp;
  resp.entry       = alice;
  resp.entry.email = "attacker@evil.org";
  resp.proof       = *proof;

  Pair::feed(*pair.reader, encodeExtendedMessage(MessageType::IdentityResponse,
                                                 serialize(resp)));

  EXPECT_FALSE(pair.reader->lastVerifiedIdentity().has_value());
}

TEST(IdentityLookupWireTest, ProofSurvivesEncodingIntact) {
  IdentityNetworkController ctl;
  for (int i = 0; i < 5; i++) {
    static_cast<void>(ctl.pipeline().appendIdentity(identityFor(
        "111122223333444455556666777788889999000" + std::to_string(i),
        "u" + std::to_string(i) + "@test.org")));
  }

  IdentityResponseMsg resp;
  resp.entry = *ctl.pipeline().findIdentityByFingerprint(
      *Fingerprint::fromString("1111222233334444555566667777888899990002"));
  resp.proof = *ctl.pipeline().generateProof(2);
  ASSERT_FALSE(resp.proof.path.empty()) << "a five-leaf tree has a path";

  const auto encoded = serialize(resp);
  const auto bytes   = std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t *>(encoded.data()), encoded.size());
  const auto decoded = decodeIdentityResponse(bytes);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_THAT(decoded->proof, Eq(resp.proof));
  EXPECT_THAT(decoded->entry, Eq(resp.entry));
  EXPECT_TRUE(EnginePipeline::verifyInclusion(decoded->entry, decoded->proof,
                                              ctl.pipeline().root()));
}

} // namespace
} // namespace xudu::identity
