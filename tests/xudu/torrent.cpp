/**
 * @file torrent.cpp
 * @brief Content-addressed names, and checking that content is what was named.
 *
 * The torrent files below were built by an independent bencoder in Python, and
 * the expected info hashes were computed there with hashlib. Testing this
 * implementation against itself would prove only that it is self-consistent,
 * which is worthless for an identity every reference depends on: a systematic
 * mistake in the encoding would produce a stable, agreed-upon, wrong hash.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>

#include <xudu/core/torrent.hpp>

#include "torrent_data.hpp"

namespace {

using xudu::InfoHash;
using xudu::Metainfo;

using xudu_test::multiFileHash;
using xudu_test::multiFileTorrent;
using xudu_test::multiFileWhole;
using xudu_test::singleFileHash;
using xudu_test::singleFileText;
using xudu_test::singleFileTorrent;

TEST(InfoHashTest, hexRoundTrips) {
  const auto hash = InfoHash::fromHex(singleFileHash);
  EXPECT_EQ(hash.hex(), singleFileHash);
}

TEST(InfoHashTest, uppercaseHexIsAccepted) {
  EXPECT_EQ(InfoHash::fromHex("F38D28D42380FD20594A3FE9B7C6A012BC818650").hex(),
            singleFileHash);
}

TEST(InfoHashTest, anythingElseIsRefused) {
  EXPECT_THROW((void)InfoHash::fromHex("abc"), std::runtime_error);
  EXPECT_THROW((void)InfoHash::fromHex(std::string(40, 'z')),
               std::runtime_error);
}

TEST(InfoHashTest, theEmptyHashIsRecognisable) {
  EXPECT_TRUE(InfoHash{}.isZero());
  EXPECT_FALSE(InfoHash::fromHex(singleFileHash).isZero());
}

TEST(Sha1Test, matchesAKnownVector) {
  // The canonical one, so a broken hash is caught here rather than showing up
  // as every torrent having the wrong name.
  EXPECT_EQ(InfoHash{xudu::sha1("abc")}.hex(),
            "a9993e364706816aba3e25717850c26c9cd0d89d");
  EXPECT_EQ(InfoHash{xudu::sha1("")}.hex(),
            "da39a3ee5e6b4b0d3255bfef95601890afd80709");
}

// -- single file --------------------------------------------------------------

TEST(MetainfoTest, theInfoHashMatchesAnIndependentImplementation) {
  EXPECT_EQ(Metainfo::parse(singleFileTorrent).hash().hex(), singleFileHash);
}

TEST(MetainfoTest, theShapeIsRead) {
  const auto meta = Metainfo::parse(singleFileTorrent);
  EXPECT_EQ(meta.name(), "fox.txt");
  EXPECT_EQ(meta.pieceLength(), 32U);
  EXPECT_EQ(meta.pieceCount(), 2U);
  EXPECT_EQ(meta.totalLength(), singleFileText.size());
  ASSERT_EQ(meta.files().size(), 1U);
  EXPECT_EQ(meta.files().front().path, "fox.txt");
  EXPECT_EQ(meta.files().front().offset, 0U);
}

TEST(MetainfoTest, theLastPieceIsShort) {
  const auto meta = Metainfo::parse(singleFileTorrent);
  EXPECT_EQ(meta.lengthOfPiece(0), 32U);
  EXPECT_EQ(meta.lengthOfPiece(1), singleFileText.size() - 32);
}

TEST(MetainfoTest, contentVerifiesAgainstItsPieceHashes) {
  const auto meta = Metainfo::parse(singleFileTorrent);
  EXPECT_TRUE(meta.verifyPiece(0, singleFileText.substr(0, 32)));
  EXPECT_TRUE(meta.verifyPiece(1, singleFileText.substr(32)));
}

TEST(MetainfoTest, oneAlteredByteFailsVerification) {
  // The property the whole idea rests on: a reader can tell that what arrived
  // is not what was referenced.
  const auto meta = Metainfo::parse(singleFileTorrent);
  auto tampered   = singleFileText.substr(0, 32);
  tampered[10]    = 'X';
  EXPECT_FALSE(meta.verifyPiece(0, tampered));
}

TEST(MetainfoTest, aShortPieceIsNotAPartialAnswer) {
  const auto meta = Metainfo::parse(singleFileTorrent);
  EXPECT_FALSE(meta.verifyPiece(0, singleFileText.substr(0, 31)));
  EXPECT_FALSE(meta.verifyPiece(0, ""));
}

TEST(MetainfoTest, aPieceThatDoesNotExistDoesNotVerify) {
  const auto meta = Metainfo::parse(singleFileTorrent);
  EXPECT_FALSE(meta.verifyPiece(99, singleFileText));
  EXPECT_THROW((void)meta.lengthOfPiece(99), std::runtime_error);
}

TEST(MetainfoTest, rangesMapOntoPieces) {
  const auto meta = Metainfo::parse(singleFileTorrent);
  EXPECT_EQ(meta.piecesForRange(0, 10),
            (std::pair<std::size_t, std::size_t>{0, 1}));
  EXPECT_EQ(meta.piecesForRange(40, 5),
            (std::pair<std::size_t, std::size_t>{1, 2}));
  // Spanning the boundary needs both.
  EXPECT_EQ(meta.piecesForRange(20, 20),
            (std::pair<std::size_t, std::size_t>{0, 2}));
}

TEST(MetainfoTest, aRangeEndingOnABoundaryDoesNotClaimTheNextPiece) {
  const auto meta = Metainfo::parse(singleFileTorrent);
  EXPECT_EQ(meta.piecesForRange(0, 32),
            (std::pair<std::size_t, std::size_t>{0, 1}));
}

TEST(MetainfoTest, anEmptyRangeNeedsNoPieces) {
  const auto meta = Metainfo::parse(singleFileTorrent);
  EXPECT_EQ(meta.piecesForRange(10, 0),
            (std::pair<std::size_t, std::size_t>{0, 0}));
  EXPECT_EQ(meta.piecesForRange(9999, 10),
            (std::pair<std::size_t, std::size_t>{0, 0}));
}

TEST(MetainfoTest, aMagnetLinkNamesTheContent) {
  const auto meta = Metainfo::parse(singleFileTorrent);
  EXPECT_THAT(meta.magnet(),
              testing::StartsWith(std::string("magnet:?xt=urn:btih:") +
                                  singleFileHash));
}

// -- magnet links -------------------------------------------------------------

TEST(MagnetTest, theHashIsRead) {
  const auto link = xudu::MagnetLink::parse(
      std::string("magnet:?xt=urn:btih:") + singleFileHash);
  EXPECT_EQ(link.hash.hex(), singleFileHash);
}

TEST(MagnetTest, whatATorrentEmitsIsWhatThisReads) {
  // The round trip that matters: a reference written down by one copy of this
  // program is read back by another as the same content.
  const auto meta = Metainfo::parse(singleFileTorrent);
  const auto link = xudu::MagnetLink::parse(meta.magnet());
  EXPECT_EQ(link.hash, meta.hash());
  EXPECT_EQ(link.displayName, meta.name());
}

TEST(MagnetTest, theBase32FormIsAccepted) {
  // Older links spell the same twenty bytes in thirty-two base-32 characters.
  // This one was produced by Python's base64.b32encode, so the decoder is
  // checked against something other than itself.
  const auto link = xudu::MagnetLink::parse(
      "magnet:?xt=urn:btih:6OGSRVBDQD6SAWKKH7U3PRVACK6IDBSQ");
  EXPECT_EQ(link.hash.hex(), singleFileHash);
}

TEST(MagnetTest, theDisplayNameAndTrackersAreRead) {
  const auto link = xudu::MagnetLink::parse(
      std::string("magnet:?xt=urn:btih:") + singleFileHash +
      "&dn=The%20Fox&tr=http%3A%2F%2Fa.invalid%2Fannounce&tr=udp%3A%2F%2Fb."
      "invalid");
  EXPECT_EQ(link.displayName, "The Fox");
  EXPECT_THAT(link.trackers, testing::ElementsAre("http://a.invalid/announce",
                                                  "udp://b.invalid"));
}

TEST(MagnetTest, selectedFileRangesAreExpanded) {
  // BEP 53: "0-2,4" names files 0, 1, 2 and 4.
  const auto link = xudu::MagnetLink::parse(
      std::string("magnet:?xt=urn:btih:") + singleFileHash + "&so=0-2,4");
  EXPECT_THAT(link.selectedFiles, testing::ElementsAre(0U, 1U, 2U, 4U));
}

TEST(MagnetTest, aLinkThatNarrowsToNothingSelectsEverything) {
  const auto link = xudu::MagnetLink::parse(
      std::string("magnet:?xt=urn:btih:") + singleFileHash);
  EXPECT_THAT(link.selectedFiles, testing::IsEmpty());
}

TEST(MagnetTest, aVersionTwoHashAloneIsRefused) {
  // btmh names content by a SHA-256 merkle root. Accepting it would mean
  // claiming to identify content this cannot verify a single byte of.
  EXPECT_THROW((void)xudu::MagnetLink::parse(
                   "magnet:?xt=urn:btmh:1220caf1e1f2" + std::string(52, 'a')),
               std::runtime_error);
}

TEST(MagnetTest, aVersionTwoHashBesideAVersionOneOneIsFine) {
  // A hybrid torrent's link carries both; the v1 hash is the one usable here.
  const auto link =
      xudu::MagnetLink::parse("magnet:?xt=urn:btmh:1220ffff&xt=urn:btih:" +
                              std::string(singleFileHash));
  EXPECT_EQ(link.hash.hex(), singleFileHash);
}

TEST(MagnetTest, somethingThatIsNotAMagnetIsRefused) {
  EXPECT_FALSE(xudu::MagnetLink::looksLikeMagnet("fox.torrent"));
  EXPECT_TRUE(xudu::MagnetLink::looksLikeMagnet("magnet:?xt=urn:btih:x"));
  EXPECT_THROW((void)xudu::MagnetLink::parse("fox.torrent"),
               std::runtime_error);
  EXPECT_THROW((void)xudu::MagnetLink::parse("magnet:?dn=nothing"),
               std::runtime_error);
}

TEST(MagnetTest, aMalformedHashIsRefused) {
  EXPECT_THROW((void)xudu::MagnetLink::parse("magnet:?xt=urn:btih:nonsense"),
               std::runtime_error);
}

// -- several files ------------------------------------------------------------

TEST(MetainfoMultiFileTest, theInfoHashMatchesAnIndependentImplementation) {
  EXPECT_EQ(Metainfo::parse(multiFileTorrent).hash().hex(), multiFileHash);
}

TEST(MetainfoMultiFileTest, filesAreConcatenatedInOrder) {
  // "the multi-file case is treated as only having a single file by
  // concatenating the files in the order they appear in the files list".
  const auto meta = Metainfo::parse(multiFileTorrent);
  ASSERT_EQ(meta.files().size(), 2U);
  EXPECT_EQ(meta.files()[0].path, "one.txt");
  EXPECT_EQ(meta.files()[0].offset, 0U);
  EXPECT_EQ(meta.files()[0].length, 27U);
  // Path components joined, and the offset picking up where the first stopped.
  EXPECT_EQ(meta.files()[1].path, "sub/two.txt");
  EXPECT_EQ(meta.files()[1].offset, 27U);
  EXPECT_EQ(meta.totalLength(), multiFileWhole.size());
}

TEST(MetainfoMultiFileTest, aPieceStraddlesTheFileBoundary) {
  // Which is why a file's content cannot be verified without the neighbouring
  // file's bytes, and why the resolver works in stream offsets.
  const auto meta = Metainfo::parse(multiFileTorrent);
  EXPECT_TRUE(meta.verifyPiece(0, multiFileWhole.substr(0, 32)));
  EXPECT_TRUE(meta.verifyPiece(1, multiFileWhole.substr(32)));
  EXPECT_GT(meta.files()[1].offset, 0U);
  EXPECT_LT(meta.files()[1].offset, meta.pieceLength());
}

TEST(MetainfoMultiFileTest, filesAreFoundByPath) {
  const auto meta = Metainfo::parse(multiFileTorrent);
  EXPECT_EQ(meta.fileIndex("one.txt"), 0U);
  EXPECT_EQ(meta.fileIndex("sub/two.txt"), 1U);
  EXPECT_FALSE(meta.fileIndex("absent.txt").has_value());
}

// -- malformed ----------------------------------------------------------------

TEST(MetainfoTest, somethingThatIsNotATorrentIsRefused) {
  EXPECT_THROW((void)Metainfo::parse("i1e"), std::runtime_error);
  EXPECT_THROW((void)Metainfo::parse("d4:infoi1ee"), std::runtime_error);
  EXPECT_THROW((void)Metainfo::parse("de"), std::runtime_error);
}

TEST(MetainfoTest, aTruncatedPieceListIsRefused) {
  // 30 bytes is not a whole number of 20-byte hashes, so some piece has no
  // hash and could never be checked.
  EXPECT_THROW((void)Metainfo::parse(
                   "d4:infod6:lengthi62e4:name1:x12:piece "
                   "lengthi32e6:pieces30:012345678901234567890123456789ee"),
               std::runtime_error);
}

TEST(MetainfoTest, tooFewPieceHashesForTheContentIsRefused) {
  // One 20-byte hash, but 62 bytes at 32 per piece needs two. A reference near
  // the end would name a piece that does not exist.
  EXPECT_THROW(
      (void)Metainfo::parse("d4:infod6:lengthi62e4:name1:x12:piece "
                            "lengthi32e6:pieces20:01234567890123456789ee"),
      std::runtime_error);
}

TEST(MetainfoTest, aZeroPieceLengthIsRefused) {
  EXPECT_THROW(
      (void)Metainfo::parse("d4:infod6:lengthi62e4:name1:x12:piece "
                            "lengthi0e6:pieces20:01234567890123456789ee"),
      std::runtime_error);
}

} // namespace
