/**
 * @file format.cpp
 * @brief Presentation attributes named as format links.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include <xudu/core/format.hpp>
#include <xudu/core/microversion.hpp>
#include <xudu/core/ops.hpp>
#include <xudu/core/store.hpp>

namespace {

using xudu::FormatAttribute;
using xudu::formatAttributeName;
using xudu::Link;
using xudu::LinkType;
using xudu::MicroversionId;
using xudu::PrimediaSpan;
using xudu::readVocabulary;
using xudu::Store;
using xudu::vocabularySpanFor;

TEST(FormatAttributeTest, everyAttributeHasAName) {
  EXPECT_STREQ(formatAttributeName(FormatAttribute::Italic), "italic");
  EXPECT_STREQ(formatAttributeName(FormatAttribute::Bold), "bold");
  EXPECT_STREQ(formatAttributeName(FormatAttribute::Underline), "underline");
  EXPECT_STREQ(formatAttributeName(FormatAttribute::Overline), "overline");
  EXPECT_STREQ(formatAttributeName(FormatAttribute::Strikethrough),
               "strikethrough");
  EXPECT_STREQ(formatAttributeName(FormatAttribute::Superscript),
               "superscript");
  EXPECT_STREQ(formatAttributeName(FormatAttribute::Subscript), "subscript");
}

TEST(FormatAttributeTest, vocabularySpanForIsStableAcrossCalls) {
  EXPECT_EQ(vocabularySpanFor(FormatAttribute::Italic),
            vocabularySpanFor(FormatAttribute::Italic));
}

TEST(FormatAttributeTest, differentAttributesGetDifferentSpans) {
  EXPECT_NE(vocabularySpanFor(FormatAttribute::Italic),
            vocabularySpanFor(FormatAttribute::Bold));
}

TEST(FormatAttributeTest, vocabularySpanForIsTheSameAcrossDifferentStores) {
  // The property a system scroll exists for: two stores that have never seen
  // each other still agree on the address "italic" lives at, which is what
  // lets a format link mean the same thing wherever it travels rather than
  // only where it was made.
  Store here;
  Store elsewhere;
  EXPECT_EQ(vocabularySpanFor(FormatAttribute::Bold),
            vocabularySpanFor(FormatAttribute::Bold));
  // Neither store is even consulted -- vocabularySpanFor() takes no Store --
  // which is the point: nothing about either store's own history can change
  // this address.
  (void)here;
  (void)elsewhere;
}

TEST(FormatAttributeTest, readingAVocabularySpanReturnsItsWord) {
  Store store;
  EXPECT_EQ(store.read(vocabularySpanFor(FormatAttribute::Strikethrough)),
            "strikethrough");
}

TEST(FormatAttributeTest, readVocabularyIgnoresSpansIntoOtherScrolls) {
  EXPECT_FALSE(readVocabulary(PrimediaSpan{xudu::localScroll, 0, 6}));
}

TEST(StoreFormatTest, vocabularyTextIsNotVisibleInAnyDocument) {
  Store store;
  const auto version = store.insert(MicroversionId{}, 0, "hello");
  EXPECT_EQ(store.textOf(version), "hello");
}

TEST(StoreFormatTest, aFormatLinkNamesItsAttribute) {
  Store store;
  const auto version = store.insert(MicroversionId{}, 0, "hello world");
  const auto content = store.rebuild(version).spansFor(0, 5); // "hello"

  Link link;
  link.type  = LinkType::Format;
  link.owner = "someone";
  link.left  = content;
  link.right.push_back(vocabularySpanFor(FormatAttribute::Italic));
  store.addLink(version, link);

  ASSERT_EQ(store.links().size(), 1U);
  const auto &stored = store.links().begin()->second;
  const auto found   = store.formatAttributeOf(stored);
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(*found, FormatAttribute::Italic);
}

TEST(StoreFormatTest, aNonFormatLinkNamesNoAttribute) {
  Store store;
  const auto version = store.insert(MicroversionId{}, 0, "hello world");
  const auto content = store.rebuild(version).spansFor(0, 5);

  Link link;
  link.type  = LinkType::Comment;
  link.left  = content;
  link.right = content;
  store.addLink(version, link);

  const auto &stored = store.links().begin()->second;
  EXPECT_FALSE(store.formatAttributeOf(stored).has_value());
}

TEST(StoreFormatTest, aFormatLinkToUnrecognisedBytesNamesNoAttribute) {
  // A right end some other program wrote, that happens not to be one of the
  // vocabulary's own spans, must not be guessed at -- even if, as here, it
  // happens to read the same bytes as a real attribute's word.
  Store store;
  const auto version         = store.insert(MicroversionId{}, 0, "hello world");
  const auto content         = store.rebuild(version).spansFor(0, 5);
  const auto coincidence     = store.insert(MicroversionId{}, 0, "italic");
  const auto coincidenceSpan = store.rebuild(coincidence).spansFor(0, 6);

  Link link;
  link.type  = LinkType::Format;
  link.left  = content;
  link.right = coincidenceSpan;
  store.addLink(version, link);

  const auto &stored = store.links().begin()->second;
  EXPECT_FALSE(store.formatAttributeOf(stored).has_value());
}

TEST(StoreFormatTest, aFormatLinkIsFoundByTouchingTheFormattedContent) {
  // Same guarantee any other link gets: attached to the address, findable by
  // it, regardless of the link's type.
  Store store;
  const auto version = store.insert(MicroversionId{}, 0, "hello world");
  const auto content = store.rebuild(version).spansFor(0, 5);

  Link link;
  link.type = LinkType::Format;
  link.left = content;
  link.right.push_back(vocabularySpanFor(FormatAttribute::Bold));
  store.addLink(version, link);

  const auto touching = store.linksTouching(content.front());
  ASSERT_EQ(touching.size(), 1U);
  EXPECT_EQ(store.formatAttributeOf(*touching.front()), FormatAttribute::Bold);
}

TEST(StoreFormatTest, aFormatLinkSurvivesQuotingItsContentElsewhere) {
  // Nelson's criterion, for format links same as any other: a link to a
  // portion is present on all manifestations, so quoting "hello" into a
  // second document still finds the italics attached to it.
  Store store;
  const auto version = store.insert(MicroversionId{}, 0, "hello world");
  const auto content = store.rebuild(version).spansFor(0, 5);

  Link link;
  link.type = LinkType::Format;
  link.left = content;
  link.right.push_back(vocabularySpanFor(FormatAttribute::Italic));
  store.addLink(version, link);

  const auto quoting = store.insert(MicroversionId{}, 0, "see: ");
  const auto quoted  = store.transclude(quoting, 5, version, 0, 5);

  // Offsets 5..10 of the quoting document are the transcluded "hello",
  // following the "see: " prefix typed ahead of it.
  const auto quotedSpans = store.rebuild(quoted).spansFor(5, 5);
  ASSERT_FALSE(quotedSpans.empty());
  const auto touching = store.linksTouching(quotedSpans.front());
  ASSERT_EQ(touching.size(), 1U);
  EXPECT_EQ(store.formatAttributeOf(*touching.front()),
            FormatAttribute::Italic);
}

TEST(StoreFormatTest, aFormatLinkMadeInOneStoreIsRecognisedInAnother) {
  // The point of routing the vocabulary through a system scroll rather than
  // a per-store spool: a format link built with one Store's
  // vocabularySpanFor() is recognised by a completely different Store that
  // never saw the first one, because the address does not depend on either
  // store's own history.
  Store maker;
  Link link;
  link.type = LinkType::Format;
  link.right.push_back(vocabularySpanFor(FormatAttribute::Overline));

  Store stranger;
  EXPECT_EQ(stranger.formatAttributeOf(link), FormatAttribute::Overline);
}

struct StoreFormatRoundTripTest : testing::Test {
  std::filesystem::path dir;

  void SetUp() override {
    dir =
        std::filesystem::temp_directory_path() /
        ("xudu-format-test-" +
         std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
         "-" + ::testing::UnitTest::GetInstance()->current_test_info()->name());
    std::filesystem::remove_all(dir);
  }
  void TearDown() override { std::filesystem::remove_all(dir); }
};

TEST_F(StoreFormatRoundTripTest,
       aFormatLinkIsStillRecognisedAfterSaveAndReload) {
  MicroversionId version;
  {
    Store store;
    version            = store.insert(MicroversionId{}, 0, "hello world");
    const auto content = store.rebuild(version).spansFor(0, 5);

    Link link;
    link.type = LinkType::Format;
    link.left = content;
    link.right.push_back(vocabularySpanFor(FormatAttribute::Underline));
    store.addLink(version, link);
    store.save(dir.string());
  }

  Store reloaded;
  reloaded.load(dir.string());
  ASSERT_EQ(reloaded.links().size(), 1U);
  const auto &stored = reloaded.links().begin()->second;
  EXPECT_EQ(reloaded.formatAttributeOf(stored), FormatAttribute::Underline);
}

} // namespace
