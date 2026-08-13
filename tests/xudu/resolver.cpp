/**
 * @file resolver.cpp
 * @brief Reading content that was not typed here, and refusing to be lied to.
 *
 * The point of a content-addressed reference is that a reader can tell whether
 * the bytes in front of them are the bytes that were referred to. Most of what
 * is checked below is that failure is handled as failure: unreachable content,
 * altered content and truncated content must all read as nothing, because
 * anything else is a substitution that everything downstream would believe.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include <xudu/core/bencode.hpp>
#include <xudu/core/resolver.hpp>
#include <xudu/core/scroll.hpp>
#include <xudu/core/store.hpp>
#include <xudu/core/torrent.hpp>

#include "torrent_data.hpp"

namespace {

using xudu::DirectoryContentSource;
using xudu::InfoHash;
using xudu::Metainfo;
using xudu::MicroversionId;
using xudu::Scroll;
using xudu::PrimediaSpan;
using xudu::Resolver;
using xudu::Store;

/// A directory holding the torrents' data, laid out as the torrents describe.
struct TorrentDataTest : testing::Test {
  std::filesystem::path dir;
  DirectoryContentSource source;

  void SetUp() override {
    dir = std::filesystem::temp_directory_path() /
          ("xudu-torrent-" +
           std::string(testing::UnitTest::GetInstance()->current_test_info()->name()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "pair" / "sub");

    write(dir / "fox.txt", xudu_test::singleFileText);
    write(dir / "pair" / "one.txt", xudu_test::multiFileFirst);
    write(dir / "pair" / "sub" / "two.txt", xudu_test::multiFileSecond);

    source.add(xudu_test::singleFileTorrent, dir.string());
    source.add(xudu_test::multiFileTorrent, (dir / "pair").string());
  }
  void TearDown() override { std::filesystem::remove_all(dir); }

  static void write(const std::filesystem::path &path, const std::string &text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
  }

  /// The scroll that is file @p index of @p hash, filled in from the torrent.
  [[nodiscard]] Scroll scrollFor(const char *hash,
                                 const std::uint32_t index) const {
    const auto parsed = InfoHash::fromHex(hash);
    const auto *meta  = source.metainfo(parsed);
    const auto &file  = meta->files()[index];
    return Scroll::ofTorrentFile(parsed, index, file.path, file.offset,
                                 file.length);
  }
};

TEST_F(TorrentDataTest, aRangeOfAFileReadsBack) {
  const Resolver resolver(&source);
  const auto fox = scrollFor(xudu_test::singleFileHash, 0);
  EXPECT_EQ(resolver.read(fox, PrimediaSpan{1, 4, 5}), "quick");
  EXPECT_EQ(resolver.read(fox, PrimediaSpan{1, 0, 3}), "The");
}

TEST_F(TorrentDataTest, aRangeSpanningAPieceBoundaryReadsBack) {
  // Pieces are 32 bytes; this range crosses the boundary, so two pieces have
  // to be fetched and verified to answer it.
  const Resolver resolver(&source);
  const auto fox = scrollFor(xudu_test::singleFileHash, 0);
  EXPECT_EQ(resolver.read(fox, PrimediaSpan{1, 28, 8}),
            xudu_test::singleFileText.substr(28, 8));
}

TEST_F(TorrentDataTest, theWholeFileReadsBack) {
  const Resolver resolver(&source);
  const auto fox = scrollFor(xudu_test::singleFileHash, 0);
  EXPECT_EQ(resolver.read(fox, PrimediaSpan{1, 0, xudu_test::singleFileText.size()}),
            xudu_test::singleFileText);
}

TEST_F(TorrentDataTest, aFileThatDoesNotStartOnAPieceBoundaryReadsBack) {
  // The second file of the pair begins 27 bytes into the stream, so its first
  // piece is shared with the file before it. Getting the arithmetic wrong here
  // would return content from the neighbouring file.
  const Resolver resolver(&source);
  const auto second = scrollFor(xudu_test::multiFileHash, 1);
  // The segment maps scroll offset 0 to stream offset 27, which is what makes
  // "the fourth byte of this file" mean the right thing inside a torrent that
  // holds several.
  EXPECT_EQ(second.segments.front().streamOffset, 27U);
  EXPECT_EQ(resolver.read(second, PrimediaSpan{1, 0, 3}), "And");
  EXPECT_EQ(resolver.read(second, PrimediaSpan{1, 4, 3}), "the");
}

TEST_F(TorrentDataTest, aRangeRunningOffTheEndIsNotAShorterRange) {
  const Resolver resolver(&source);
  const auto first = scrollFor(xudu_test::multiFileHash, 0);

  // Exactly the scroll reads back whole.
  EXPECT_EQ(resolver.read(first, PrimediaSpan{1, 0, 27}),
            xudu_test::multiFileFirst);

  // Asking for 100 bytes of a 27-byte scroll must certainly not spill into the
  // neighbouring file, and it does not come back as 27 bytes either. Nothing
  // downstream can tell a clamped answer from a complete one, so a caller that
  // asked for a hundred bytes and was handed twenty-seven would have no way to
  // know it was reading a fragment -- the same reason a piece that fails its
  // hash takes the whole read down with it.
  //
  // It matters more once a scroll is something that grows: a quotation
  // reaching past the last sealed segment is a quotation of content nobody has
  // published yet, and showing the part that exists would misrepresent it.
  EXPECT_EQ(resolver.read(first, PrimediaSpan{1, 0, 100}), "");
}

TEST_F(TorrentDataTest, anEmptyRangeReadsNothing) {
  const Resolver resolver(&source);
  const auto fox = scrollFor(xudu_test::singleFileHash, 0);
  EXPECT_EQ(resolver.read(fox, PrimediaSpan{1, 0, 0}), "");
  EXPECT_EQ(resolver.read(fox, PrimediaSpan{1, 9999, 5}), "");
}

TEST_F(TorrentDataTest, alteredContentIsNotReturned) {
  // The property the whole idea rests on. One byte changed on disk, and the
  // reference stops resolving rather than quietly yielding something else.
  write(dir / "fox.txt", "Xhe quick brown fox jumped over the lazy dog. And "
                         "many more...");
  const Resolver resolver(&source);
  const auto fox = scrollFor(xudu_test::singleFileHash, 0);
  EXPECT_EQ(resolver.read(fox, PrimediaSpan{1, 4, 5}), "");
}

TEST_F(TorrentDataTest, alterationOutsideTheRangeStillFailsIt) {
  // Verification is per piece, and the reference asked for part of that piece.
  // Returning the requested bytes because they happen to be untouched would
  // mean trusting content whose hash did not check out.
  auto tampered = xudu_test::singleFileText;
  tampered[20]  = '!';
  write(dir / "fox.txt", tampered);
  const Resolver resolver(&source);
  const auto fox = scrollFor(xudu_test::singleFileHash, 0);
  EXPECT_EQ(resolver.read(fox, PrimediaSpan{1, 0, 3}), "");
}

TEST_F(TorrentDataTest, truncatedContentIsNotReturned) {
  write(dir / "fox.txt", "The quick");
  const Resolver resolver(&source);
  const auto fox = scrollFor(xudu_test::singleFileHash, 0);
  EXPECT_EQ(resolver.read(fox, PrimediaSpan{1, 0, 3}), "");
}

TEST_F(TorrentDataTest, missingContentIsNotReturned) {
  std::filesystem::remove(dir / "fox.txt");
  const Resolver resolver(&source);
  const auto fox = scrollFor(xudu_test::singleFileHash, 0);
  EXPECT_EQ(resolver.read(fox, PrimediaSpan{1, 0, 3}), "");
}

TEST_F(TorrentDataTest, aTorrentTheSourceHasNeverHeardOfIsNotReturned) {
  const Resolver resolver(&source);
  const auto unknown = Scroll::ofTorrentFile(
      InfoHash::fromHex(std::string(40, 'a')), 0, "nowhere.txt", 0, 100);
  EXPECT_FALSE(resolver.available(unknown));
  EXPECT_EQ(resolver.read(unknown, PrimediaSpan{1, 0, 3}), "");
}

TEST_F(TorrentDataTest, withNoSourceNothingResolves) {
  const Resolver resolver;
  const auto fox = scrollFor(xudu_test::singleFileHash, 0);
  EXPECT_FALSE(resolver.available(fox));
  EXPECT_EQ(resolver.read(fox, PrimediaSpan{1, 0, 3}), "");
}

// -- quoting a torrent into a document ----------------------------------------

TEST_F(TorrentDataTest, aDocumentCanQuoteContentItDoesNotHold) {
  Store store;
  store.setContentSource(&source);

  const auto one = store.insert(MicroversionId{}, 0, "Nelson wrote: ");
  const auto two = store.transcludeExternal(one, 14,
                                            scrollFor(xudu_test::singleFileHash, 0),
                                            4, 5);

  EXPECT_EQ(store.textOf(two), "Nelson wrote: quick");
  // Nothing was copied: the spool holds only what was typed here.
  EXPECT_EQ(store.primedia().size(), 14U);
}

TEST_F(TorrentDataTest, twoDocumentsQuotingOneTorrentShareThatContent) {
  // Transclusion between documents that never met, detected by address. This
  // is what a stable global name buys that a local offset cannot.
  Store store;
  store.setContentSource(&source);
  const auto fox = scrollFor(xudu_test::singleFileHash, 0);

  const auto left  = store.transcludeExternal(MicroversionId{}, 0, fox, 4, 5);
  const auto right = store.insert(MicroversionId{}, 0, "see also: ");
  const auto quoted = store.transcludeExternal(right, 10, fox, 4, 5);

  const auto shared = store.rebuild(left).pieces().front();
  EXPECT_THAT(store.rebuild(quoted).occurrencesOf(shared),
              testing::ElementsAre(xudu::Extent{10, 15}));
}

TEST_F(TorrentDataTest, oneScrollIsRecordedOnceHoweverOftenItIsQuoted) {
  Store store;
  const auto fox = scrollFor(xudu_test::singleFileHash, 0);
  store.transcludeExternal(MicroversionId{}, 0, fox, 0, 5);
  store.transcludeExternal(MicroversionId{}, 0, fox, 10, 5);
  EXPECT_EQ(store.scrolls().size(), 1U);
}

TEST_F(TorrentDataTest, aDocumentOpensEvenWhenWhatItQuotesIsUnreachable) {
  // A reference that cannot be resolved right now is not a corrupt document.
  // The quotation comes out empty and everything typed here is still there,
  // which is the behaviour a reader wants when nobody is seeding.
  Store store;
  const auto one = store.insert(MicroversionId{}, 0, "before after");
  const auto two = store.transcludeExternal(
      one, 7, scrollFor(xudu_test::singleFileHash, 0), 4, 5);

  // No content source at all.
  EXPECT_EQ(store.textOf(two), "before after");

  store.setContentSource(&source);
  EXPECT_EQ(store.textOf(two), "before quickafter");
}

// -- persistence --------------------------------------------------------------

struct TorrentStoreRoundTripTest : TorrentDataTest {};

TEST_F(TorrentStoreRoundTripTest, aTorrentBackedQuotationSurvivesAReload) {
  const auto storeDir = (dir / "xanadoc").string();
  MicroversionId quoted;
  {
    Store store;
    store.setContentSource(&source);
    const auto one = store.insert(MicroversionId{}, 0, "Nelson wrote: ");
    quoted = store.transcludeExternal(one, 14,
                                      scrollFor(xudu_test::singleFileHash, 0),
                                      4, 5);
    store.save(storeDir);
  }

  Store reloaded;
  reloaded.load(storeDir);
  reloaded.setContentSource(&source);

  ASSERT_EQ(reloaded.scrolls().size(), 1U);
  ASSERT_EQ(reloaded.scrolls().front().segments.size(), 1U);
  EXPECT_EQ(reloaded.scrolls().front().segments.front().torrent.hex(),
            xudu_test::singleFileHash);
  EXPECT_EQ(reloaded.scrolls().front().segments.front().path, "fox.txt");
  EXPECT_EQ(reloaded.textOf(quoted), "Nelson wrote: quick");
}

TEST_F(TorrentDataTest, aStoreWrittenBeforeScrollsStillReads) {
  // Stores on disk name their content the old way, as a torrent and a file. A
  // one-segment scroll's offsets are that file's offsets, so those spans mean
  // exactly what they always meant -- but only if the old table is still read.
  const auto storeDir = (dir / "legacy").string();
  std::filesystem::create_directories(storeDir);
  write(std::filesystem::path(storeDir) / "origins.spool",
        std::string{xudu_test::singleFileHash} + " 0 0 " +
            std::to_string(xudu_test::singleFileText.size()) + " fox.txt\n");
  // One transclude of scroll 1, offsets 4..9, at position 0.
  write(std::filesystem::path(storeDir) / "ops.spool",
        "1 transclude 0 0 0 4 5 0 0 0 0 1\n");

  Store loaded;
  loaded.load(storeDir);
  loaded.setContentSource(&source);

  ASSERT_EQ(loaded.scrolls().size(), 1U);
  ASSERT_EQ(loaded.scrolls().front().segments.size(), 1U);
  EXPECT_EQ(loaded.scrolls().front().segments.front().torrent.hex(),
            xudu_test::singleFileHash);
  EXPECT_EQ(loaded.textOf(MicroversionId::parse("1")), "quick");

  // Saved again, it is written in the new shape, and still says the same.
  const auto again = (dir / "migrated").string();
  std::filesystem::create_directories(again);
  loaded.save(again);
  EXPECT_TRUE(std::filesystem::exists(std::filesystem::path(again) /
                                      "scrolls.spool"));
  Store reloaded;
  reloaded.load(again);
  reloaded.setContentSource(&source);
  EXPECT_EQ(reloaded.textOf(MicroversionId::parse("1")), "quick");
}

// -- scrolls carried by more than one torrent -------------------------------
//
// The case the addressing was rearranged for. A scroll that grew was sealed
// more than once, so its bytes live in a succession of torrents; none of that
// is allowed to reach an address.

/// Build a torrent for @p text under @p name, with a piece length of its own.
/// Two torrents of the same bytes and different piece lengths have different
/// info hashes, which is what makes them usable as two packagings of one
/// scroll.
[[nodiscard]] std::string torrentOf(const std::string &name,
                                    const std::string &text,
                                    const std::uint64_t pieceLength) {
  std::string pieces;
  for (std::size_t at = 0; at < text.size(); at += pieceLength) {
    const auto hash = xudu::sha1(
        std::string_view{text}.substr(at, static_cast<std::size_t>(pieceLength)));
    pieces.append(reinterpret_cast<const char *>(hash.data()), hash.size());
  }
  using xudu::bencode::Value;
  const auto info = Value::dict({
      {"length", Value::integer(static_cast<std::int64_t>(text.size()))},
      {"name", Value::string(name)},
      {"piece length", Value::integer(static_cast<std::int64_t>(pieceLength))},
      {"pieces", Value::string(pieces)},
  });
  return Value::dict({{"info", info}}).encode();
}

struct SealedScrollTest : TorrentDataTest {
  /// A scroll sealed in two goes: the first stretch ended up in one torrent
  /// and the second in another, which is what a permascroll looks like after
  /// it has been sealed twice.
  [[nodiscard]] Scroll twoSegments() const {
    Scroll scroll;
    scroll.addSegment(xudu::ScrollSegment{
        0, xudu_test::multiFileFirst.size(),
        InfoHash::fromHex(xudu_test::multiFileHash), 0, 0, "one.txt"});
    scroll.addSegment(xudu::ScrollSegment{
        xudu_test::multiFileFirst.size(), xudu_test::singleFileText.size(),
        InfoHash::fromHex(xudu_test::singleFileHash), 0, 0, "fox.txt"});
    return scroll;
  }

  /// What that scroll says, end to end.
  [[nodiscard]] static std::string joined() {
    return xudu_test::multiFileFirst + xudu_test::singleFileText;
  }
};

TEST_F(SealedScrollTest, theWholeScrollReadsAcrossItsSeals) {
  const Resolver resolver(&source);
  const auto scroll = twoSegments();
  EXPECT_EQ(resolver.read(scroll, PrimediaSpan{1, 0, joined().size()}),
            joined());
  EXPECT_TRUE(resolver.available(scroll));
}

TEST_F(SealedScrollTest, aRangeStraddlingASealIsOneRead) {
  // Fetched from two different torrents and verified against two different
  // sets of piece hashes, and the caller cannot tell: it asked for a range of
  // a scroll and got that range.
  const Resolver resolver(&source);
  const auto scroll = twoSegments();
  const auto seal   = xudu_test::multiFileFirst.size();

  EXPECT_EQ(resolver.read(scroll, PrimediaSpan{1, seal - 4, 8}),
            joined().substr(seal - 4, 8));
  // And the seal is at no special offset as far as an address is concerned.
  EXPECT_EQ(resolver.read(scroll, PrimediaSpan{1, seal, 5}),
            joined().substr(seal, 5));
  EXPECT_EQ(resolver.read(scroll, PrimediaSpan{1, seal - 1, 2}),
            joined().substr(seal - 1, 2));
}

TEST_F(SealedScrollTest, aStretchNobodyHasSealedReadsAsNothing) {
  // A gap between segments is content that has not been published. Returning
  // the parts either side of it would be presenting two passages as one.
  const Resolver resolver(&source);
  Scroll gapped;
  gapped.addSegment(xudu::ScrollSegment{
      0, 10, InfoHash::fromHex(xudu_test::multiFileHash), 0, 0, "one.txt"});
  gapped.addSegment(xudu::ScrollSegment{
      20, 10, InfoHash::fromHex(xudu_test::multiFileHash), 20, 0, "one.txt"});

  EXPECT_EQ(resolver.read(gapped, PrimediaSpan{1, 0, 10}),
            xudu_test::multiFileFirst.substr(0, 10));
  EXPECT_EQ(resolver.read(gapped, PrimediaSpan{1, 5, 20}), "");
  EXPECT_EQ(resolver.read(gapped, PrimediaSpan{1, 12, 4}), "");
}

TEST_F(SealedScrollTest, aQuotationAcrossASealIsOnePieceOfTheDocument) {
  // The defect this replaced a worse fix for. Addressed by torrent, a
  // quotation crossing a seal was two spans into two unrelated origins, and
  // showed as two shaded runs where a reader sees one passage. In scroll
  // coordinates it is one span, so nothing above here has to be taught that
  // seals exist.
  Store store;
  store.setContentSource(&source);
  const auto seal = xudu_test::multiFileFirst.size();

  const auto quoted =
      store.transcludeExternal(MicroversionId{}, 0, twoSegments(), seal - 4, 8);

  const auto version = store.rebuild(quoted);
  ASSERT_EQ(version.pieces().size(), 1U) << "a seal split the quotation";
  EXPECT_EQ(store.textOf(quoted), joined().substr(seal - 4, 8));

  // And it is found as one occurrence rather than two.
  const auto found = version.occurrencesOf(version.pieces().front());
  ASSERT_EQ(found.size(), 1U);
  EXPECT_EQ(found.front().start, 0U);
  EXPECT_EQ(found.front().end, 8U);
}

TEST_F(SealedScrollTest, resealingDoesNotDisturbAnAddressAlreadyHandedOut) {
  // The property the whole rearrangement exists for. A reference is made, the
  // content is then repackaged into an entirely different torrent -- different
  // info hash, different piece length, different everything a reference used
  // to be made of -- and the reference still names the same passage.
  Store store;
  store.setContentSource(&source);

  const auto id = store.addScroll(scrollFor(xudu_test::singleFileHash, 0));
  const auto quoted =
      store.transcludeExternal(MicroversionId{}, 0, *store.scroll(id), 14, 5);
  const auto before = store.textOf(quoted);
  EXPECT_EQ(before, xudu_test::singleFileText.substr(14, 5));

  // Sealed again, into a torrent this store has never seen before.
  write(dir / "resealed.txt", xudu_test::singleFileText);
  const auto resealed =
      source.add(torrentOf("resealed.txt", xudu_test::singleFileText, 16),
                 dir.string());
  ASSERT_NE(resealed.hex(), std::string{xudu_test::singleFileHash})
      << "the two packagings have to be genuinely different torrents";

  store.addSegment(id, xudu::ScrollSegment{0, xudu_test::singleFileText.size(),
                                           resealed, 0, 0, "resealed.txt"});

  // The carrier moved...
  ASSERT_EQ(store.scroll(id)->segments.size(), 1U);
  EXPECT_EQ(store.scroll(id)->segments.front().torrent, resealed);
  // ...and the address did not.
  EXPECT_EQ(store.textOf(quoted), before);
}

TEST_F(SealedScrollTest, aNamedScrollIsItselfWhicheverTorrentCarriesIt) {
  // Identity by publisher rather than by container. Without this, learning
  // that a scroll had been re-sealed would create a second entry, and every
  // reference to the first would stop being recognised as the same content --
  // which is transclusion silently coming apart.
  Store store;
  Scroll first;
  first.publisher = xudu::PublicKey::fromHex(std::string(64, 'a'));
  first.addSegment(xudu::ScrollSegment{
      0, 27, InfoHash::fromHex(xudu_test::multiFileHash), 0, 0, "one.txt"});

  Scroll later;
  later.publisher = first.publisher;
  later.addSegment(xudu::ScrollSegment{
      27, 10, InfoHash::fromHex(xudu_test::singleFileHash), 0, 0, "fox.txt"});

  const auto id = store.addScroll(first);
  EXPECT_EQ(store.addScroll(later), id) << "one scroll became two";
  // Both seals are now known about, under the one identity.
  EXPECT_EQ(store.scrolls().size(), 1U);
  EXPECT_EQ(store.scroll(id)->segments.size(), 2U);

  // An unnamed scroll of the same bytes is a different scroll, and honestly
  // so: nothing binds two packagings together without a publisher to say so.
  EXPECT_NE(store.addScroll(scrollFor(xudu_test::multiFileHash, 0)), id);
}

// Where a multi-file torrent's files are is ambiguous in practice: its paths
// are relative to a directory named after the torrent, and what somebody has
// is as often the directory holding that one -- which is where a downloader
// leaves it and where sealing writes it. Guessing wrong produces a document
// that reads as empty, a long way from the mistake that caused it, so both
// spellings are accepted.
TEST_F(TorrentDataTest, aMultiFileTorrentIsFoundFromEitherDirectory) {
  const auto hash = InfoHash::fromHex(xudu_test::multiFileHash);
  const auto want = xudu_test::multiFileFirst;

  // The directory the torrent's name refers to, which is what SetUp gave.
  EXPECT_EQ(source.readStream(hash, 0, want.size()), want);

  // And the directory above it. The layout on disk is unchanged; only what
  // the caller said about it differs.
  DirectoryContentSource above;
  above.add(xudu_test::multiFileTorrent, dir.string());
  EXPECT_EQ(above.readStream(hash, 0, want.size()), want);
}

// The case that is easy to get wrong twice over: a torrent whose name is also
// its first file's name, which is what sealing a scroll produces, since the
// scroll and the content it carries are both called after the salt. Testing
// for the file's existence rather than its being a file finds the directory
// and then fails to read anything out of it.
TEST_F(TorrentDataTest, aTorrentNamedAfterItsOwnFirstFileStillResolves) {
  const std::string content = "the content that was sealed";
  const std::string record  = "author: \"somebody\"\n";
  const std::array<xudu::TorrentContent, 2> files{
      xudu::TorrentContent{"spool", content},
      xudu::TorrentContent{"AUTHORSHIP.yaml", record},
  };
  const auto made = xudu::makeTorrent(files, "spool");

  const auto laid = dir / "sealed";
  std::filesystem::create_directories(laid / "spool");
  write(laid / "spool" / "spool", content);
  write(laid / "spool" / "AUTHORSHIP.yaml", record);

  DirectoryContentSource sealed;
  // The directory above, where "spool" is a directory rather than the file.
  sealed.add(made.file, laid.string());
  EXPECT_EQ(sealed.readStream(made.hash, 0, content.size()), content);
  EXPECT_EQ(sealed.readStream(made.hash, content.size(), record.size()),
            record);
}

} // namespace

// vi: set sw=2 sts=2 ts=2 et:
