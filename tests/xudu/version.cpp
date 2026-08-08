/**
 * @file version.cpp
 * @brief A document as a list of pointers into the primedia spool.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

#include <xudu/core/spool.hpp>
#include <xudu/core/version.hpp>

namespace {

using xudu::Extent;
using xudu::localOrigin;
using xudu::PrimediaSpan;
using xudu::PrimediaSpool;
using xudu::Version;

TEST(PrimediaSpanTest, intersectionIsTheSharedPart) {
  const PrimediaSpan left{localOrigin, 10, 10};  // [10, 20)
  const PrimediaSpan right{localOrigin, 15, 10}; // [15, 25)
  EXPECT_EQ(left.intersect(right), (PrimediaSpan{localOrigin, 15, 5}));
}

TEST(PrimediaSpanTest, addressesIntoDifferentContentNeverOverlap) {
  // The property that makes an address global rather than local. Byte 100 of
  // one torrent has nothing to do with byte 100 of another, and treating the
  // numbers as comparable would report transclusions nobody made -- between
  // two documents that merely quote unrelated things at similar offsets.
  const PrimediaSpan mine{localOrigin, 10, 10};
  const PrimediaSpan theirs{7, 10, 10};
  EXPECT_TRUE(mine.intersect(theirs).empty());
  EXPECT_TRUE(theirs.intersect(mine).empty());
  // Same numbers, same origin: that is a real overlap.
  EXPECT_FALSE(theirs.intersect(PrimediaSpan{7, 12, 4}).empty());
}

TEST(PrimediaSpanTest, slicingKeepsTheOrigin) {
  // A slice of a reference is a reference to the same content. Losing the
  // origin here would silently turn part of a torrent-backed quotation into an
  // address in the local spool.
  const PrimediaSpan external{7, 100, 50};
  EXPECT_EQ(external.slice(10, 5).origin, 7U);
  EXPECT_EQ(external.slice(10, 5).start, 110U);
}

TEST(SpoolTest, aSpanIntoOtherContentIsRefusedRatherThanAnswered) {
  // The spool has no way to reach a torrent, and answering with whatever sits
  // at that offset locally would be a substitution the caller could not
  // detect -- the exact failure content addressing exists to prevent.
  PrimediaSpool spool;
  spool.append("local text");
  EXPECT_THROW((void)spool.read(PrimediaSpan{7, 0, 5}), std::runtime_error);
}

TEST(PrimediaSpanTest, spansThatDoNotMeetShareNothing) {
  EXPECT_TRUE((PrimediaSpan{localOrigin, 0, 5}).intersect(PrimediaSpan{localOrigin, 5, 5}).empty());
}

TEST(SpoolTest, appendingReportsWhereItLanded) {
  PrimediaSpool spool;
  const auto first  = spool.append("hello");
  const auto second = spool.append(" world");

  EXPECT_EQ(first, (PrimediaSpan{localOrigin, 0, 5}));
  EXPECT_EQ(second, (PrimediaSpan{localOrigin, 5, 6}));
  EXPECT_EQ(spool.read(first), "hello");
  EXPECT_EQ(spool.read(second), " world");
}

TEST(SpoolTest, theSameTextTwiceGetsTwoAddresses) {
  // Two people typing the same sentence have not quoted each other, and
  // sharing an address would invent a transclusion nobody made.
  PrimediaSpool spool;
  EXPECT_NE(spool.append("the same").start, spool.append("the same").start);
}

TEST(SpoolTest, aSpanPastTheEndReadsWhatIsThere) {
  PrimediaSpool spool;
  spool.append("abc");
  EXPECT_EQ(spool.read(PrimediaSpan{localOrigin, 1, 100}), "bc");
  EXPECT_EQ(spool.read(PrimediaSpan{localOrigin, 100, 5}), "");
}

/// A version over one spool, which most of these need.
struct VersionTest : testing::Test {
  PrimediaSpool spool;
  Version version;

  /// Type @p text at @p at, the way the store does: into the spool, then a
  /// pointer to it into the version.
  void type(const std::uint32_t at, const std::string &text) {
    version.insert(at, spool.append(text));
  }
  [[nodiscard]] std::string text() const { return version.materialize(spool); }
};

TEST_F(VersionTest, anEmptyVersionIsAnEmptyDocument) {
  EXPECT_EQ(text(), "");
  EXPECT_EQ(version.length(), 0U);
}

TEST_F(VersionTest, insertionsReadBackInOrder) {
  type(0, "world");
  type(0, "hello ");
  EXPECT_EQ(text(), "hello world");
  EXPECT_EQ(version.length(), 11U);
}

TEST_F(VersionTest, insertingInsideAPieceSplitsIt) {
  type(0, "helloworld");
  type(5, ", ");
  EXPECT_EQ(text(), "hello, world");
  // One piece became three: the two halves and the new text between them.
  EXPECT_EQ(version.pieces().size(), 3U);
}

TEST_F(VersionTest, insertingPastTheEndAppends) {
  type(0, "abc");
  type(999, "def");
  EXPECT_EQ(text(), "abcdef");
}

TEST_F(VersionTest, removingStopsPointingAtContentWithoutDestroyingIt) {
  type(0, "hello, world");
  const auto removed = version.remove(5, 2);

  EXPECT_EQ(text(), "helloworld");
  // The spool is append-and-read-only: what was removed is still there, which
  // is what keeps every earlier version resolvable.
  EXPECT_EQ(spool.read(removed.front()), ", ");
  EXPECT_EQ(spool.size(), 12U);
}

TEST_F(VersionTest, removingMoreThanIsThereTakesWhatIsThere) {
  type(0, "abc");
  version.remove(1, 100);
  EXPECT_EQ(text(), "a");
}

TEST_F(VersionTest, removingSpansSeveralPieces) {
  type(0, "aaa");
  type(3, "bbb");
  type(6, "ccc");
  version.remove(2, 5);
  EXPECT_EQ(text(), "aacc");
}

TEST_F(VersionTest, rearrangeMovesARangeForwards) {
  type(0, "abcdef");
  version.rearrange(0, 2, 4); // "ab" to where "e" starts
  EXPECT_EQ(text(), "cdabef");
}

TEST_F(VersionTest, rearrangeMovesARangeBackwards) {
  type(0, "abcdef");
  version.rearrange(4, 2, 0); // "ef" to the front
  EXPECT_EQ(text(), "efabcd");
}

TEST_F(VersionTest, rearrangingIntoTheMovedRangeDoesNothing) {
  type(0, "abcdef");
  version.rearrange(1, 3, 2);
  EXPECT_EQ(text(), "abcdef");
}

TEST_F(VersionTest, everyByteKnowsWhereItCameFrom) {
  type(0, "world");
  type(0, "hello ");
  // "hello " went into the spool second, at address 5.
  EXPECT_EQ(version.addressAt(0), 5U);
  // "world" was first, at address 0, and is now at offset 6 of the document.
  EXPECT_EQ(version.addressAt(6), 0U);
  EXPECT_FALSE(version.addressAt(11).has_value());
}

TEST_F(VersionTest, spansForNamesTheAddressesARangePointsAt) {
  type(0, "hello");
  const auto spans = version.spansFor(1, 3);
  ASSERT_EQ(spans.size(), 1U);
  EXPECT_EQ(spans.front(), (PrimediaSpan{localOrigin, 1, 3}));
  EXPECT_EQ(spool.read(spans.front()), "ell");
}

TEST_F(VersionTest, transcludedContentIsFoundByAddress) {
  // What a quotation is: a second pointer to the same address, so both
  // documents show one copy of the content.
  type(0, "the quick brown fox");
  const auto quoted = version.spansFor(4, 5); // "quick"

  Version other;
  other.insert(0, quoted.front());
  EXPECT_EQ(other.materialize(spool), "quick");

  const auto found = other.occurrencesOf(quoted.front());
  ASSERT_EQ(found.size(), 1U);
  EXPECT_EQ(found.front(), (Extent{0, 5}));
}

TEST_F(VersionTest, matchingTextIsNotATransclusion) {
  // The distinction a string search cannot make: these two documents read the
  // same and share nothing.
  type(0, "quick");
  Version other;
  other.insert(0, spool.append("quick"));

  EXPECT_EQ(other.materialize(spool), text());
  EXPECT_THAT(other.occurrencesOf(version.pieces().front()), testing::IsEmpty());
}

TEST_F(VersionTest, aPassageQuotedTwiceIsFoundTwice) {
  type(0, "abc");
  const auto span = version.pieces().front();

  Version other;
  other.insert(0, span);
  other.insert(3, spool.append("-"));
  other.insert(4, span);

  EXPECT_EQ(other.materialize(spool), "abc-abc");
  EXPECT_THAT(other.occurrencesOf(span),
              testing::ElementsAre(Extent{0, 3}, Extent{4, 7}));
}

TEST_F(VersionTest, aQuotationSplitByAnEditIsStillFound) {
  type(0, "abcdef");
  const auto span = version.pieces().front();

  Version other;
  other.insert(0, span);
  // Editing inside the quotation splits the piece; the two halves still point
  // at consecutive addresses, so this is one passage with something inserted
  // in it rather than two unrelated ones.
  other.insert(3, spool.append("!"));
  EXPECT_EQ(other.materialize(spool), "abc!def");
  EXPECT_THAT(other.occurrencesOf(span),
              testing::ElementsAre(Extent{0, 3}, Extent{4, 7}));
}

TEST_F(VersionTest, adjacentPiecesOfOneQuotationReadAsOne) {
  // Splitting a piece and putting it straight back must not leave a seam a
  // reader would see.
  type(0, "abcdef");
  const auto span = version.pieces().front();
  version.insert(3, spool.append(""));

  EXPECT_THAT(version.occurrencesOf(span), testing::ElementsAre(Extent{0, 6}));
}

} // namespace

// vi: set sw=2 sts=2 ts=2 et:
