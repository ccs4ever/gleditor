/**
 * @file mutable_link.cpp
 * @brief A name that outlives the content it points at.
 *
 * The vectors here are the ones printed in BEP 44 and BEP 46, not values this
 * implementation produced. That matters more here than almost anywhere else in
 * this tree: a signing buffer assembled slightly wrongly still signs, still
 * verifies against itself, and produces a name that resolves for its author
 * and for nobody else in the world. Self-consistency is exactly the failure
 * mode, so the authority has to be somebody else's document.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

#include <xudu/core/mutable_link.hpp>
#include <xudu/core/swarm.hpp>
#include <xudu/core/torrent.hpp>

namespace {

using xudu::DhtTarget;
using xudu::InfoHash;
using xudu::MutableLink;
using xudu::PublicKey;
using xudu::Signature;

/// BEP 44's test vectors, which carry a real key pair and real signatures.
constexpr std::string_view bep44Key =
    "77ff84905a91936367c01360803104f92432fcd904a43511876df5cdf3e7e548";
constexpr std::string_view bep44Secret =
    "e06d3183d14159228433ed599221b80bd0a5ce8352e4bdf0262f76786ef1c74d"
    "b7e7a9fea2c0eb269d61e3b38e450a22e754941ac78479d6c54e1faf6037881d";
constexpr std::string_view bep44Value = "12:Hello World!";
constexpr std::string_view bep44Signature =
    "305ac8aeb6c9c151fa120f120ea2cfb923564e11552d06a5d856091e5e853cff"
    "1260d3f39e4999684aa92eb73ffd136e6f4f3ecbfda0ce53a1608ecd7ae21f01";
constexpr std::string_view bep44SaltedSignature =
    "6834284b6b24c3204eb2fea824d82f88883a3d95e8b4a21b8c0ded553d17d17d"
    "df9a8a7104b1258f30bed3787e6cb896fca78c58f8e03b5f18f14951a87d9a08";

/// BEP 46's, which are about where in the DHT a name's pointer lives.
constexpr std::string_view bep46Key =
    "8543d3e6115f0f98c944077a4493dcd543e49c739fd998550a1f614ab36ed63e";

[[nodiscard]] MutableLink linkFor(const std::string_view key,
                                  const std::string_view saltHex = "") {
  std::string uri = "magnet:?xs=urn:btpk:" + std::string{key};
  if (!saltHex.empty()) {
    uri += "&s=" + std::string{saltHex};
  }
  return MutableLink::parse(uri);
}

// -- where a name's pointer lives ------------------------------------------

TEST(MutableLinkTest, theTargetMatchesBep46) {
  // "test 1 (magnet to target ID)".
  EXPECT_EQ(linkFor(bep46Key).target().hex(),
            "cc3f9d90b572172053626f9980ce261a850d050b");
}

TEST(MutableLinkTest, aSaltMovesTheTarget) {
  // "test 2 (magnet to targetID, with salt)", where the salt is the single
  // byte 0x6e. One key can therefore carry any number of independent names.
  EXPECT_EQ(linkFor(bep46Key, "6e").target().hex(),
            "59ee7c2cb9b4f7eb1986ee2d18fd2fdb8a56554f");
  EXPECT_NE(linkFor(bep46Key, "6e").target(), linkFor(bep46Key).target());
}

TEST(MutableLinkTest, theTargetMatchesBep44Too) {
  // The same computation from the other specification's vectors, whose salt is
  // six bytes rather than one.
  EXPECT_EQ(linkFor(bep44Key).target().hex(),
            "4a533d47ec9c7d95b1ad75f576cffc641853b750");
  EXPECT_EQ(linkFor(bep44Key, xudu::toHex("foobar")).target().hex(),
            "411eba73b6f087ca51a3795d9c8c938d365e32c1");
}

TEST(MutableLinkTest, theSaltIsHexInTheLinkAndBytesEverywhereElse) {
  // It is hashed and signed as bytes, so a link that wrote it literally would
  // give a different target for the same name.
  EXPECT_EQ(linkFor(bep44Key, xudu::toHex("foobar")).salt, "foobar");
  EXPECT_EQ(linkFor(bep46Key, "6e").salt, "n");
}

// -- reading and writing the link ------------------------------------------

TEST(MutableLinkTest, aMutableLinkIsToldApartFromAnOrdinaryMagnet) {
  // Both are magnets and both begin the same way; the difference is xs against
  // xt, so the parameters have to be looked at rather than the scheme.
  EXPECT_TRUE(MutableLink::looksLikeMutableLink(linkFor(bep46Key).uri()));
  EXPECT_FALSE(MutableLink::looksLikeMutableLink(
      "magnet:?xt=urn:btih:" + std::string(40, 'a')));
  EXPECT_FALSE(MutableLink::looksLikeMutableLink("sample.torrent"));
}

TEST(MutableLinkTest, aLinkNamingNoKeyIsRefused) {
  EXPECT_THROW((void)MutableLink::parse("magnet:?xt=urn:btih:" +
                                        std::string(40, 'a')),
               std::runtime_error);
  EXPECT_THROW((void)MutableLink::parse("sample.torrent"), std::runtime_error);
  // Half a key is not a key.
  EXPECT_THROW((void)linkFor("8543d3e6"), std::runtime_error);
}

TEST(MutableLinkTest, itRoundTrips) {
  MutableLink link;
  link.key         = PublicKey::fromHex(bep46Key);
  link.salt        = "scroll-2";
  link.displayName = "a permascroll";

  const auto again = MutableLink::parse(link.uri());
  EXPECT_EQ(again.key, link.key);
  EXPECT_EQ(again.salt, link.salt);
  EXPECT_EQ(again.displayName, link.displayName);
  EXPECT_EQ(again.target(), link.target());
}

TEST(MutableLinkTest, anInfoHashInTheLinkIsAHintAndNotTheName) {
  // A link may say what the name pointed at when it was written down. That is
  // a starting point for a client that cannot reach the DHT, and it is
  // expected to go stale -- which is the entire point of a mutable name.
  const auto hash = std::string(40, 'a');
  const auto link = MutableLink::parse("magnet:?xs=urn:btpk:" +
                                       std::string{bep46Key} +
                                       "&xt=urn:btih:" + hash);
  ASSERT_TRUE(link.currentWhenWritten.has_value());
  EXPECT_EQ(link.currentWhenWritten->hex(), hash);
  // The hint moves nothing: the pointer still lives where the key says.
  EXPECT_EQ(link.target(), linkFor(bep46Key).target());
}

// -- the bytes that get signed ---------------------------------------------

TEST(MutableSigningTest, theBufferMatchesBep44) {
  // "Sequence number 1 of value "Hello World!" would be converted to".
  EXPECT_EQ(xudu::mutableSigningBuffer("", 1, bep44Value),
            "3:seqi1e1:v12:Hello World!");
  EXPECT_EQ(xudu::mutableSigningBuffer("foobar", 1, bep44Value),
            "4:salt6:foobar3:seqi1e1:v12:Hello World!");
}

TEST(MutableSigningTest, itIsNotADictionary) {
  // Worth pinning down, because it looks like one and an encoder would never
  // produce it: there is no surrounding `d` and `e`, and the keys are not in
  // sorted order -- salt comes before seq, which comes before v, but only
  // because the specification says so.
  const auto buffer = xudu::mutableSigningBuffer("x", 7, "i0e");
  EXPECT_EQ(buffer, "4:salt1:x3:seqi7e1:vi0e");
  EXPECT_FALSE(buffer.starts_with("d"));
}

TEST(MutableSigningTest, theSequenceNumberIsPartOfWhatIsSigned) {
  // Which is what stops an old, genuinely signed answer being replayed as the
  // current one.
  EXPECT_NE(xudu::mutableSigningBuffer("", 1, bep44Value),
            xudu::mutableSigningBuffer("", 2, bep44Value));
}

// -- what a name points at --------------------------------------------------

TEST(MutablePointerTest, itIsADictionaryOfOneEntry) {
  const auto hash = InfoHash::fromHex(std::string(40, 'a'));
  const auto encoded = xudu::encodeMutablePointer(hash);
  EXPECT_EQ(encoded.substr(0, 8), "d2:ih20:");
  EXPECT_EQ(encoded.size(), 8U + 20U + 1U);
  EXPECT_EQ(xudu::decodeMutablePointer(encoded), hash);
}

TEST(MutablePointerTest, theHashIsRawBytesAndNotHex) {
  // Forty hex digits would fit "ih" perfectly well and would name nothing:
  // every other client reads twenty bytes.
  const auto encoded =
      xudu::encodeMutablePointer(InfoHash::fromHex(std::string(40, 'a')));
  EXPECT_EQ(encoded.find("aaaa"), std::string::npos);
}

TEST(MutablePointerTest, anythingElseUnderTheNameIsNotAnAnswer) {
  // A key may hold any bencoded value at all. One holding something else is
  // not a corrupt pointer, it is somebody else's use of the DHT.
  EXPECT_FALSE(xudu::decodeMutablePointer("not bencode").has_value());
  EXPECT_FALSE(xudu::decodeMutablePointer("i42e").has_value());
  EXPECT_FALSE(xudu::decodeMutablePointer("de").has_value());
  EXPECT_FALSE(xudu::decodeMutablePointer("d2:ihi7ee").has_value());
  // Nineteen bytes is not an info hash, and padding it out would invent one.
  EXPECT_FALSE(xudu::decodeMutablePointer("d2:ih19:" + std::string(19, 'a') +
                                          "e")
                   .has_value());
}

// -- the signature itself ---------------------------------------------------
//
// These need libtorrent, which is where the curve arithmetic comes from. The
// buffer they sign is this codebase's, so a disagreement with the published
// signature is this codebase's mistake.

class MutableCryptoTest : public testing::Test {
protected:
  void SetUp() override {
    keys.publicKey = PublicKey::fromHex(bep44Key);
    const auto secret = xudu::fromHex(bep44Secret);
    std::copy(secret.begin(), secret.end(), keys.secretKey.bytes.begin());
  }

  xudu::MutableKeys keys;
};

TEST_F(MutableCryptoTest, signingReproducesThePublishedSignature) {
  EXPECT_EQ(xudu::signMutableItem(xudu::mutableSigningBuffer("", 1, bep44Value),
                                  keys)
                .hex(),
            bep44Signature);
  EXPECT_EQ(xudu::signMutableItem(
                xudu::mutableSigningBuffer("foobar", 1, bep44Value), keys)
                .hex(),
            bep44SaltedSignature);
}

TEST_F(MutableCryptoTest, thePublishedSignatureVerifies) {
  EXPECT_TRUE(xudu::verifyMutableItem(
      xudu::mutableSigningBuffer("", 1, bep44Value),
      Signature::fromHex(bep44Signature), keys.publicKey));
}

TEST_F(MutableCryptoTest, aTamperedAnswerDoesNotVerify) {
  const auto signature = Signature::fromHex(bep44Signature);

  // Somebody else's content under this name.
  EXPECT_FALSE(xudu::verifyMutableItem(
      xudu::mutableSigningBuffer("", 1, "12:Goodbye All!"), signature,
      keys.publicKey));
  // The same content, claimed to be newer than it is.
  EXPECT_FALSE(xudu::verifyMutableItem(
      xudu::mutableSigningBuffer("", 2, bep44Value), signature,
      keys.publicKey));
  // The right answer, attributed to a name that did not give it.
  EXPECT_FALSE(xudu::verifyMutableItem(
      xudu::mutableSigningBuffer("", 1, bep44Value), signature,
      PublicKey::fromHex(bep46Key)));
  // The salted answer offered as the unsalted one, which is a different name.
  EXPECT_FALSE(xudu::verifyMutableItem(
      xudu::mutableSigningBuffer("", 1, bep44Value),
      Signature::fromHex(bep44SaltedSignature), keys.publicKey));
}

TEST_F(MutableCryptoTest, aFreshNameIsNobodyElsesAndSignsItsOwnPointer) {
  const auto mine = xudu::createMutableKeys();
  EXPECT_FALSE(mine.publicKey.isZero());
  EXPECT_NE(mine.publicKey, keys.publicKey);

  const auto hash = InfoHash::fromHex(std::string(40, 'b'));
  const auto buffer =
      xudu::mutableSigningBuffer("", 4, xudu::encodeMutablePointer(hash));
  const auto signature = xudu::signMutableItem(buffer, mine);
  EXPECT_TRUE(xudu::verifyMutableItem(buffer, signature, mine.publicKey));
  // And says nothing about anyone else's name.
  EXPECT_FALSE(xudu::verifyMutableItem(buffer, signature, keys.publicKey));
}

} // namespace

// vi: set sw=2 sts=2 ts=2 et:
