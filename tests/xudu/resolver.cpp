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

#include <filesystem>
#include <fstream>
#include <string>

#include <xudu/core/origin.hpp>
#include <xudu/core/resolver.hpp>
#include <xudu/core/store.hpp>

#include "torrent_data.hpp"

namespace {

using xudu::DirectoryContentSource;
using xudu::InfoHash;
using xudu::Metainfo;
using xudu::MicroversionId;
using xudu::Origin;
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

  /// The origin naming file @p index of @p hash, filled in from the torrent.
  [[nodiscard]] Origin originFor(const char *hash,
                                 const std::uint32_t index) const {
    const auto parsed = InfoHash::fromHex(hash);
    const auto *meta  = source.metainfo(parsed);
    const auto &file  = meta->files()[index];
    return Origin{parsed, index, file.path, file.offset, file.length};
  }
};

TEST_F(TorrentDataTest, aRangeOfAFileReadsBack) {
  const Resolver resolver(&source);
  const auto fox = originFor(xudu_test::singleFileHash, 0);
  EXPECT_EQ(resolver.read(fox, PrimediaSpan{1, 4, 5}), "quick");
  EXPECT_EQ(resolver.read(fox, PrimediaSpan{1, 0, 3}), "The");
}

TEST_F(TorrentDataTest, aRangeSpanningAPieceBoundaryReadsBack) {
  // Pieces are 32 bytes; this range crosses the boundary, so two pieces have
  // to be fetched and verified to answer it.
  const Resolver resolver(&source);
  const auto fox = originFor(xudu_test::singleFileHash, 0);
  EXPECT_EQ(resolver.read(fox, PrimediaSpan{1, 28, 8}),
            xudu_test::singleFileText.substr(28, 8));
}

TEST_F(TorrentDataTest, theWholeFileReadsBack) {
  const Resolver resolver(&source);
  const auto fox = originFor(xudu_test::singleFileHash, 0);
  EXPECT_EQ(resolver.read(fox, PrimediaSpan{1, 0, xudu_test::singleFileText.size()}),
            xudu_test::singleFileText);
}

TEST_F(TorrentDataTest, aFileThatDoesNotStartOnAPieceBoundaryReadsBack) {
  // The second file of the pair begins 27 bytes into the stream, so its first
  // piece is shared with the file before it. Getting the arithmetic wrong here
  // would return content from the neighbouring file.
  const Resolver resolver(&source);
  const auto second = originFor(xudu_test::multiFileHash, 1);
  EXPECT_EQ(second.fileOffset, 27U);
  EXPECT_EQ(resolver.read(second, PrimediaSpan{1, 0, 3}), "And");
  EXPECT_EQ(resolver.read(second, PrimediaSpan{1, 4, 3}), "the");
}

TEST_F(TorrentDataTest, aRangePastTheEndOfTheFileIsClamped) {
  const Resolver resolver(&source);
  const auto first = originFor(xudu_test::multiFileHash, 0);
  // The file is 27 bytes; asking for 100 must not spill into the next file.
  EXPECT_EQ(resolver.read(first, PrimediaSpan{1, 0, 100}),
            xudu_test::multiFileFirst);
}

TEST_F(TorrentDataTest, anEmptyRangeReadsNothing) {
  const Resolver resolver(&source);
  const auto fox = originFor(xudu_test::singleFileHash, 0);
  EXPECT_EQ(resolver.read(fox, PrimediaSpan{1, 0, 0}), "");
  EXPECT_EQ(resolver.read(fox, PrimediaSpan{1, 9999, 5}), "");
}

TEST_F(TorrentDataTest, alteredContentIsNotReturned) {
  // The property the whole idea rests on. One byte changed on disk, and the
  // reference stops resolving rather than quietly yielding something else.
  write(dir / "fox.txt", "Xhe quick brown fox jumped over the lazy dog. And "
                         "many more...");
  const Resolver resolver(&source);
  const auto fox = originFor(xudu_test::singleFileHash, 0);
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
  const auto fox = originFor(xudu_test::singleFileHash, 0);
  EXPECT_EQ(resolver.read(fox, PrimediaSpan{1, 0, 3}), "");
}

TEST_F(TorrentDataTest, truncatedContentIsNotReturned) {
  write(dir / "fox.txt", "The quick");
  const Resolver resolver(&source);
  const auto fox = originFor(xudu_test::singleFileHash, 0);
  EXPECT_EQ(resolver.read(fox, PrimediaSpan{1, 0, 3}), "");
}

TEST_F(TorrentDataTest, missingContentIsNotReturned) {
  std::filesystem::remove(dir / "fox.txt");
  const Resolver resolver(&source);
  const auto fox = originFor(xudu_test::singleFileHash, 0);
  EXPECT_EQ(resolver.read(fox, PrimediaSpan{1, 0, 3}), "");
}

TEST_F(TorrentDataTest, aTorrentTheSourceHasNeverHeardOfIsNotReturned) {
  const Resolver resolver(&source);
  Origin unknown;
  unknown.torrent    = InfoHash::fromHex(std::string(40, 'a'));
  unknown.fileLength = 100;
  EXPECT_FALSE(resolver.available(unknown));
  EXPECT_EQ(resolver.read(unknown, PrimediaSpan{1, 0, 3}), "");
}

TEST_F(TorrentDataTest, withNoSourceNothingResolves) {
  const Resolver resolver;
  const auto fox = originFor(xudu_test::singleFileHash, 0);
  EXPECT_FALSE(resolver.available(fox));
  EXPECT_EQ(resolver.read(fox, PrimediaSpan{1, 0, 3}), "");
}

// -- quoting a torrent into a document ----------------------------------------

TEST_F(TorrentDataTest, aDocumentCanQuoteContentItDoesNotHold) {
  Store store;
  store.setContentSource(&source);

  const auto one = store.insert(MicroversionId{}, 0, "Nelson wrote: ");
  const auto two = store.transcludeExternal(one, 14,
                                            originFor(xudu_test::singleFileHash, 0),
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
  const auto fox = originFor(xudu_test::singleFileHash, 0);

  const auto left  = store.transcludeExternal(MicroversionId{}, 0, fox, 4, 5);
  const auto right = store.insert(MicroversionId{}, 0, "see also: ");
  const auto quoted = store.transcludeExternal(right, 10, fox, 4, 5);

  const auto shared = store.rebuild(left).pieces().front();
  EXPECT_THAT(store.rebuild(quoted).occurrencesOf(shared),
              testing::ElementsAre(xudu::Extent{10, 15}));
}

TEST_F(TorrentDataTest, oneOriginIsRecordedOnceHoweverOftenItIsQuoted) {
  Store store;
  const auto fox = originFor(xudu_test::singleFileHash, 0);
  store.transcludeExternal(MicroversionId{}, 0, fox, 0, 5);
  store.transcludeExternal(MicroversionId{}, 0, fox, 10, 5);
  EXPECT_EQ(store.origins().size(), 1U);
}

TEST_F(TorrentDataTest, aDocumentOpensEvenWhenWhatItQuotesIsUnreachable) {
  // A reference that cannot be resolved right now is not a corrupt document.
  // The quotation comes out empty and everything typed here is still there,
  // which is the behaviour a reader wants when nobody is seeding.
  Store store;
  const auto one = store.insert(MicroversionId{}, 0, "before after");
  const auto two = store.transcludeExternal(
      one, 7, originFor(xudu_test::singleFileHash, 0), 4, 5);

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
                                      originFor(xudu_test::singleFileHash, 0),
                                      4, 5);
    store.save(storeDir);
  }

  Store reloaded;
  reloaded.load(storeDir);
  reloaded.setContentSource(&source);

  ASSERT_EQ(reloaded.origins().size(), 1U);
  EXPECT_EQ(reloaded.origins().front().torrent.hex(), xudu_test::singleFileHash);
  EXPECT_EQ(reloaded.origins().front().path, "fox.txt");
  EXPECT_EQ(reloaded.textOf(quoted), "Nelson wrote: quick");
}

} // namespace

// vi: set sw=2 sts=2 ts=2 et:
