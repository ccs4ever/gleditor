/**
 * @file test_pdf_beams.cpp
 * @brief Unit tests for multi-page forced breaks and beam links in xudu engine.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <xudu/core/framing.hpp>
#include <xudu/core/link_layout.hpp>
#include <xudu/core/ops.hpp>
#include <xudu/core/store.hpp>

namespace {

using xudu::HalfLink;
using xudu::Link;
using xudu::LinkedPair;
using xudu::LinkType;
using xudu::MicroversionId;
using xudu::Store;
using xudu::Version;

std::vector<const Version *> viewing(const std::vector<Version> &versions) {
  std::vector<const Version *> out;
  out.reserve(versions.size());
  for (const auto &one : versions) {
    out.push_back(&one);
  }
  return out;
}

TEST(PdfBeamsTest, forcedBreaksPreservePageBoundariesInStore) {
  Store store;
  // Simulate importing a 3-page document with text on each page and forced breaks
  const std::string page0 = "First Page Header\nAlpha Bravo Charlie Delta Echo\n";
  const std::string page1 = "Second Page Header\nFoxtrot Golf Hotel India Juliet\n";
  const std::string page2 = "Third Page Header\nKilo Lima Mike November Oscar\n";

  const std::string allText = page0 + page1 + page2;
  const std::vector<std::uint32_t> breaks = {
      static_cast<std::uint32_t>(page0.size()),
      static_cast<std::uint32_t>(page0.size() + page1.size()),
      static_cast<std::uint32_t>(allText.size())};

  auto imported = store.insert(MicroversionId{}, 0, allText);
  for (const auto breakAt : breaks) {
    imported = store.insertBreak(imported, breakAt);
  }

  EXPECT_FALSE(imported.isZero());
  const auto text = store.textOf(imported);
  EXPECT_EQ(text, allText);

  const auto rebuilt = store.rebuild(imported);
  const auto actualBreaks = rebuilt.forcedBreaks();
  EXPECT_EQ(actualBreaks.size(), 3U);
  EXPECT_EQ(actualBreaks, breaks);
}

TEST(PdfBeamsTest, linksBetweenMultiPageSpansCreateBeams) {
  Store store;
  const std::string page0 = "First Page Header\nAlpha Bravo Charlie Delta Echo\n";
  const std::string page1 = "Second Page Header\nFoxtrot Golf Hotel India Juliet\n";
  const std::string page2 = "Third Page Header\nKilo Lima Mike November Oscar\n";

  const std::string allText = page0 + page1 + page2;
  const std::vector<std::uint32_t> breaks = {
      static_cast<std::uint32_t>(page0.size()),
      static_cast<std::uint32_t>(page0.size() + page1.size()),
      static_cast<std::uint32_t>(allText.size())};

  auto docVersion = store.insert(MicroversionId{}, 0, allText);
  for (const auto breakAt : breaks) {
    docVersion = store.insertBreak(docVersion, breakAt);
  }

  const auto v = store.rebuild(docVersion);

  // Page 0 text span: "Alpha Bravo"
  const auto p0Pos = allText.find("Alpha Bravo");
  ASSERT_NE(p0Pos, std::string::npos);
  const auto p0Spans = v.spansFor(static_cast<std::uint32_t>(p0Pos), 11);

  // Page 1 text span: "Foxtrot Golf"
  const auto p1Pos = allText.find("Foxtrot Golf");
  ASSERT_NE(p1Pos, std::string::npos);
  const auto p1Spans = v.spansFor(static_cast<std::uint32_t>(p1Pos), 12);

  Link link;
  link.type  = LinkType::Comment;
  link.owner = "annotator";
  link.left  = p0Spans;
  link.right = p1Spans;

  const auto linkedVersion = store.addLink(docVersion, link);

  // When transcluding a quote from Page 1 into a second document:
  const auto quotedVersion = store.transclude(
      MicroversionId{}, 0, linkedVersion, static_cast<std::uint32_t>(p1Pos), 12);

  const std::vector<Version> versions{store.rebuild(linkedVersion),
                                      store.rebuild(quotedVersion)};
  std::vector<LinkedPair> placed;
  std::vector<HalfLink> unplaced;
  xudu::placeLinks(store.links(), viewing(versions), placed, unplaced);

  ASSERT_EQ(placed.size(), 1U);
  EXPECT_EQ(placed[0].type, LinkType::Comment);
  // Source end on page 0 in document 0
  EXPECT_EQ(placed[0].from.doc, 0U);
  EXPECT_EQ(placed[0].from.start, p0Pos);
  // Destination end in document 1
  EXPECT_EQ(placed[0].to.doc, 1U);
  EXPECT_EQ(placed[0].to.start, 0U);
  EXPECT_EQ(placed[0].to.end, 12U);
}

} // namespace
