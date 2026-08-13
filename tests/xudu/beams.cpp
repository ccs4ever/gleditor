/**
 * @file beams.cpp
 * @brief Which links become beams, and between which documents.
 *
 * The drawing needs a device; this does not. What is checked here is the part
 * with something to get wrong: a link attaches to content rather than to a
 * position, so where its ends have come to rest is a question about what the
 * open versions are made of. A quotation moves an end into another document
 * without the link knowing, and that is exactly when a beam should appear.
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <vector>

#include <xudu/core/link_layout.hpp>
#include <xudu/core/ops.hpp>
#include <xudu/core/store.hpp>

namespace {

using xudu::Link;
using xudu::HalfLink;
using xudu::LinkedPair;
using xudu::LinkType;
using xudu::MicroversionId;
using xudu::Store;
using xudu::Version;

constexpr const char *sentence = "alpha beta gamma delta";
constexpr std::uint32_t alphaAt = 0;
constexpr std::uint32_t alphaLen = 5;
constexpr std::uint32_t gammaAt = 11;
constexpr std::uint32_t gammaLen = 5;

/// A document with a link from "alpha" to "gamma", and the state it produced.
MicroversionId linkedSentence(Store &store, const LinkType type = LinkType::Comment) {
  const auto typed = store.insert(MicroversionId{}, 0, sentence);
  const auto text  = store.rebuild(typed);
  Link link;
  link.type  = type;
  link.owner = "someone";
  link.left  = text.spansFor(alphaAt, alphaLen);
  link.right = text.spansFor(gammaAt, gammaLen);
  return store.addLink(typed, link);
}

std::vector<const Version *> viewing(const std::vector<Version> &versions) {
  std::vector<const Version *> out;
  out.reserve(versions.size());
  for (const auto &one : versions) {
    out.push_back(&one);
  }
  return out;
}

} // namespace

// The case the whole thing exists for: a passage is quoted into a second
// document, and the link somebody attached to it in the first is now a
// relation between two documents -- which is a beam.
TEST(LinkLayout, aQuotationTurnsALinkIntoABeamBetweenTwoDocuments) {
  Store store;
  const auto linked = linkedSentence(store);
  // A second document made of nothing but the far end of the link.
  const auto quoted =
      store.transclude(MicroversionId{}, 0, linked, gammaAt, gammaLen);

  const std::vector<Version> versions{store.rebuild(linked),
                                      store.rebuild(quoted)};
  std::vector<LinkedPair> placed;
  std::vector<HalfLink> unplaced;
  xudu::placeLinks(store.links(), viewing(versions), placed, unplaced);

  ASSERT_EQ(placed.size(), 1U);
  EXPECT_EQ(placed[0].type, LinkType::Comment);
  // From where the link was attached, in the first document...
  EXPECT_EQ(placed[0].from.doc, 0U);
  EXPECT_EQ(placed[0].from.start, alphaAt);
  EXPECT_EQ(placed[0].from.end, alphaAt + alphaLen);
  // ...to where the content it points at now also appears.
  EXPECT_EQ(placed[0].to.doc, 1U);
  EXPECT_EQ(placed[0].to.start, 0U);
  EXPECT_EQ(placed[0].to.end, gammaLen);
  EXPECT_TRUE(unplaced.empty());
}

// Both ends in one document is not a beam. The decorator already shades both
// passages, and a ribbon between them would run back across the text it
// connects.
TEST(LinkLayout, aLinkWithinOneDocumentDrawsNothing) {
  Store store;
  const auto linked = linkedSentence(store);

  const std::vector<Version> versions{store.rebuild(linked)};
  std::vector<LinkedPair> placed;
  std::vector<HalfLink> unplaced;
  xudu::placeLinks(store.links(), viewing(versions), placed, unplaced);

  EXPECT_TRUE(placed.empty());
  // And it is not waiting for a document either: both of its ends are here.
  EXPECT_TRUE(unplaced.empty());
}

// The ordinary case for a link, since a link is made to content and not to
// whatever happens to be open: one end is on screen and the other is in a
// document nobody has opened. That is what the sworph acts on, so it has to be
// reported rather than dropped.
TEST(LinkLayout, anEndInNoOpenDocumentIsReportedWithSomethingToLookFor) {
  Store store;
  const auto linked = linkedSentence(store);
  const auto quoted =
      store.transclude(MicroversionId{}, 0, linked, gammaAt, gammaLen);

  // Only the quotation is open, so the link's left end is nowhere on screen.
  const std::vector<Version> versions{store.rebuild(quoted)};
  std::vector<LinkedPair> placed;
  std::vector<HalfLink> unplaced;
  xudu::placeLinks(store.links(), viewing(versions), placed, unplaced);

  EXPECT_TRUE(placed.empty());
  ASSERT_EQ(unplaced.size(), 1U);
  EXPECT_EQ(unplaced[0].here.doc, 0U);
  EXPECT_EQ(unplaced[0].here.start, 0U);
  // The spans handed back are the end that is missing, which is what a search
  // for a document showing it has to be given.
  const auto &link = store.links().begin()->second;
  EXPECT_EQ(unplaced[0].elsewhere, link.left);
}

// A document that shares no content with either end says nothing about the
// link, however much of the same text it happens to contain.
TEST(LinkLayout, aDocumentThatMerelyReadsTheSameIsNotAnEnd) {
  Store store;
  const auto linked = linkedSentence(store);
  // The same words, typed again rather than quoted, so they are different
  // content at different addresses.
  const auto retyped = store.insert(MicroversionId{}, 0, sentence);

  const std::vector<Version> versions{store.rebuild(retyped)};
  std::vector<LinkedPair> placed;
  std::vector<HalfLink> unplaced;
  xudu::placeLinks(store.links(), viewing(versions), placed, unplaced);

  EXPECT_TRUE(placed.empty());
  EXPECT_TRUE(unplaced.empty());
  EXPECT_FALSE(linked.isZero());
}

// A link end quoted twice into one document is still one relation between the
// two documents. Two beams to the same place would claim something the link
// does not say.
TEST(LinkLayout, anEndQuotedTwiceIntoOneDocumentIsStillOneBeam) {
  Store store;
  const auto linked = linkedSentence(store);
  const auto once =
      store.transclude(MicroversionId{}, 0, linked, gammaAt, gammaLen);
  const auto twice = store.transclude(once, gammaLen, linked, gammaAt, gammaLen);

  const std::vector<Version> versions{store.rebuild(linked),
                                      store.rebuild(twice)};
  std::vector<LinkedPair> placed;
  std::vector<HalfLink> unplaced;
  xudu::placeLinks(store.links(), viewing(versions), placed, unplaced);

  ASSERT_EQ(placed.size(), 1U);
  // And it covers both quotations, so the beam lands somewhere within what it
  // is pointing at rather than at one of two arbitrary copies.
  EXPECT_EQ(placed[0].to.start, 0U);
  EXPECT_EQ(placed[0].to.end, 2 * gammaLen);
}

// Each type is told apart by colour, since a label on a beam would have to be
// legible at whatever angle the beam runs at.
TEST(LinkLayout, everyLinkTypeHasAColourOfItsOwn) {
  const std::vector<LinkType> types{
      LinkType::Comment,    LinkType::Illustration, LinkType::Disagreement,
      LinkType::Authorship, LinkType::Quotation,    LinkType::Other};
  std::map<std::uint32_t, LinkType> seen;
  for (const auto type : types) {
    const auto colour = xudu::linkColour(type);
    EXPECT_TRUE(seen.emplace(colour, type).second)
        << "two link types share a colour";
    // Visible, and not opaque: a beam crosses the space between two documents
    // and is not the thing being read.
    const auto alpha = colour & 255U;
    EXPECT_GT(alpha, 0x40U);
    EXPECT_LT(alpha, 0xF0U);
  }
}

// vi: set sw=2 sts=2 ts=2 et:
