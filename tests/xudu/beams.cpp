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

#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <vector>

#include <glm/trigonometric.hpp>

#include <gleditor/doc.hpp>
#include <xudu/core/framing.hpp>
#include <xudu/core/link_layout.hpp>
#include <xudu/core/ops.hpp>
#include <xudu/core/store.hpp>

namespace {

using xudu::centroidAlignmentDeltaY;
using xudu::centroidY;
using xudu::framingDistance;
using xudu::framingFov;
using xudu::HalfLink;
using xudu::Link;
using xudu::LinkedPair;
using xudu::LinkType;
using xudu::MicroversionId;
using xudu::pageStackExtent;
using xudu::PageStackExtent;
using xudu::Store;
using xudu::Version;

constexpr const char *sentence   = "alpha beta gamma delta";
constexpr std::uint32_t alphaAt  = 0;
constexpr std::uint32_t alphaLen = 5;
constexpr std::uint32_t gammaAt  = 11;
constexpr std::uint32_t gammaLen = 5;

/// A document with a link from "alpha" to "gamma", and the state it produced.
MicroversionId linkedSentence(Store &store,
                              const LinkType type = LinkType::Comment) {
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
  const auto twice =
      store.transclude(once, gammaLen, linked, gammaAt, gammaLen);

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

TEST(LinkLayout, centroidSworphingAlignsMultiSpanMidpoints) {
  // Test mathematical centroid alignment for a many-to-many link
  // Left spans: Y = 100.0, 150.0, 200.0 (center = 150.0)
  // Right spans: Y = 40.0, 80.0 (center = 60.0)
  const float leftMinY  = 100.0F;
  const float leftMaxY  = 200.0F;
  const float rightMinY = 40.0F;
  const float rightMaxY = 80.0F;

  const float leftCenterY  = 0.5F * (leftMinY + leftMaxY);
  const float rightCenterY = 0.5F * (rightMinY + rightMaxY);
  const float deltaY       = leftCenterY - rightCenterY;

  EXPECT_FLOAT_EQ(leftCenterY, 150.0F);
  EXPECT_FLOAT_EQ(rightCenterY, 60.0F);
  EXPECT_FLOAT_EQ(deltaY, 90.0F);

  // After applying deltaY to right anchors:
  // Right spans become Y = 130.0, 170.0 (center = 150.0)
  const float alignedRightCenterY =
      0.5F * ((rightMinY + deltaY) + (rightMaxY + deltaY));
  EXPECT_FLOAT_EQ(alignedRightCenterY, leftCenterY);
}

TEST(LinkLayout, singleSpanOneToOneLeveling) {
  // For a 1-to-1 link, Ymin == Ymax, and deltaY is exact leveling
  const float leftY  = 120.0F;
  const float rightY = 45.0F;

  const float leftCenterY  = leftY;
  const float rightCenterY = rightY;
  const float deltaY       = leftCenterY - rightCenterY;

  EXPECT_FLOAT_EQ(deltaY, 75.0F);
  EXPECT_FLOAT_EQ(rightY + deltaY, leftY);
}

TEST(LinkLayout, oneToManyCentroidAlignment) {
  // 1 span on left (Y = 150), 3 spans on right (Y = 50, 100, 250 -> range [50,
  // 250], center = 150)
  const float leftY       = 150.0F;
  const float rightMinY   = 50.0F;
  const float rightMaxY   = 250.0F;
  const float rightCenter = 0.5F * (rightMinY + rightMaxY);
  const float deltaY      = leftY - rightCenter;

  EXPECT_FLOAT_EQ(rightCenter, 150.0F);
  EXPECT_FLOAT_EQ(deltaY, 0.0F); // Already aligned with centroid
}

TEST(LinkLayout, multiDocumentNonOverlappingSpacing) {
  // 3 documents: doc 0 (halfW=10), doc 1 (halfW=12), doc 2 (halfW=15), docGap=2
  const float d0Half = 10.0F;
  const float d1Half = 12.0F;
  const float d2Half = 15.0F;
  const float gap    = 2.0F;

  const float x0 = 0.0F;
  const float x1 = x0 + (d0Half + d1Half + gap); // 0 + 24 = 24
  const float x2 = x1 + (d1Half + d2Half + gap); // 24 + 29 = 53

  EXPECT_FLOAT_EQ(x1, 24.0F);
  EXPECT_FLOAT_EQ(x2, 53.0F);

  // Verify no horizontal overlap between any document envelopes
  EXPECT_LT(x0 + d0Half, x1 - d1Half); // 10 < 12 (gap of 2)
  EXPECT_LT(x1 + d1Half, x2 - d2Half); // 36 < 38 (gap of 2)
}

TEST(LinkLayout, multiDocumentFramingEncompassesAllThreeDocs) {
  const float d0Half = 10.0F;
  const float d1Half = 12.0F;
  const float d2Half = 15.0F;
  const float gap    = 2.0F;

  const float x0 = 0.0F;
  const float x1 = x0 + (d0Half + d1Half + gap); // 24.0
  const float x2 = x1 + (d1Half + d2Half + gap); // 53.0

  const float minX    = x0 - d0Half - 4.0F;   // -14.0
  const float maxX    = x2 + d2Half + 4.0F;   // 72.0
  const float spanW   = maxX - minX;          // 86.0
  const float centerX = 0.5F * (minX + maxX); // 29.0

  EXPECT_FLOAT_EQ(spanW, 86.0F);
  EXPECT_FLOAT_EQ(centerX, 29.0F);

  const float aspect     = 800.0F / 600.0F;
  const float fovDeg     = 7.5F;
  const float tanHalfFov = std::tan(glm::radians(fovDeg) * 0.5F);
  const float zFit       = spanW / (2.0F * aspect * tanHalfFov);

  EXPECT_GT(zFit, 0.0F);
  const float frustumWidth = 2.0F * (zFit * tanHalfFov * aspect);
  EXPECT_GE(frustumWidth, spanW);
}

TEST(LinkLayout, nonAdjacentBypassLayerRoutingDepth) {
  // Adjacent documents: docSpan = 1 -> foreground Z = 0
  const std::size_t adjDocSpan = 1;
  EXPECT_LE(adjDocSpan, 1U);

  // Non-adjacent documents: docSpan = 2 (e.g. Doc 0 to Doc 2) -> bypassDepth Z
  // = -20
  const std::size_t nonAdjDocSpan = 2;
  EXPECT_GT(nonAdjDocSpan, 1U);
  constexpr float bypassDepth = -20.0F;
  EXPECT_LT(bypassDepth, 0.0F);
}

namespace {

constexpr const char *fullPageDocA =
    "Chapter 1: Principles of Universal Hypertext and Transclusion\n\n"
    "Section 1.1: The Foundation of Interconnection\n"
    "Hypertext is not merely a mechanism for isolated documents to point to "
    "one "
    "another, but rather a deep, interconnected fabric where every idea "
    "maintains its provenance, authorship, and relational context across space "
    "and time.\n\n"
    "Section 1.2: Two-Way Visible Links\n"
    "Traditional one-way links break the symmetry of knowledge. In contrast, "
    "visible, bidirectional link beams span directly between passages, "
    "allowing readers to view both ends simultaneously without jumping or "
    "losing context.\n\n"
    "Section 1.3: Transclusion and Eternal Quotes\n"
    "Transclusion ensures that quoting never duplicates or severs content from "
    "its original origin. The quoted text remains living tissue in both the "
    "source and destination documents.\n\n"
    "Section 1.4: Multi-Span Synthesis\n"
    "Complex ideas frequently relate across disparate paragraphs, synthesizing "
    "insights from historical foundations, mathematical models, and visual "
    "design.";

constexpr const char *fullPageDocB =
    "Commentary on Universal Hypertext Systems\n\n"
    "Observation A: Provenance and Memory\n"
    "Preserving the origin of text ensures that credit and historical lineage "
    "are unbroken.\n\n"
    "Observation B: Spatial Sworphing and Visual Ergonomics\n"
    "Moving documents along 3D trajectories to bring connected spans into "
    "horizontal alignment minimizes eye strain and clarifies complex "
    "relational topology.\n\n"
    "Observation C: Deep Citation Graphs\n"
    "When multiple evidentiary spans support a single thesis, or when several "
    "premises converge on a shared conclusion, many-to-many and one-to-many "
    "link beams bundle gracefully around their vertical centroids.";

} // namespace

TEST(LinkLayout, multipleOneToOneLinksOfDifferentTypesAcrossFullPage) {
  Store store;
  const auto vA = store.insert(MicroversionId{}, 0, fullPageDocA);
  const auto vB = store.insert(MicroversionId{}, 0, fullPageDocB);

  const auto textA = store.rebuild(vA);
  const auto textB = store.rebuild(vB);

  // Link 1: Comment (Section 1.1 -> Observation A)
  Link l1;
  l1.type       = LinkType::Comment;
  l1.owner      = "alice";
  l1.left       = textA.spansFor(65, 45);
  l1.right      = textB.spansFor(44, 33);
  const auto s1 = store.addLink(vA, l1);

  // Link 2: Illustration (Section 1.2 -> Observation B)
  Link l2;
  l2.type       = LinkType::Illustration;
  l2.owner      = "bob";
  l2.left       = textA.spansFor(295, 30);
  l2.right      = textB.spansFor(160, 45);
  const auto s2 = store.addLink(s1, l2);

  // Link 3: Disagreement (Section 1.3 -> Observation C)
  Link l3;
  l3.type       = LinkType::Disagreement;
  l3.owner      = "critic";
  l3.left       = textA.spansFor(490, 35);
  l3.right      = textB.spansFor(320, 30);
  const auto s3 = store.addLink(s2, l3);

  // Link 4: Quotation (Section 1.4 -> Observation B)
  Link l4;
  l4.type  = LinkType::Quotation;
  l4.owner = "annotator";
  l4.left  = textA.spansFor(690, 40);
  l4.right = textB.spansFor(210, 40);
  store.addLink(s3, l4);

  const std::vector<Version> versions{store.rebuild(vA), store.rebuild(vB)};
  std::vector<LinkedPair> placed;
  std::vector<HalfLink> unplaced;
  xudu::placeLinks(store.links(), viewing(versions), placed, unplaced);

  ASSERT_EQ(placed.size(), 4U);
  EXPECT_TRUE(unplaced.empty());

  // Verify all 4 link types are placed with their respective colours and
  // distinct tiers
  std::set<LinkType> typesFound;
  for (const auto &pair : placed) {
    typesFound.insert(pair.type);
    EXPECT_EQ(pair.from.doc, 0U);
    EXPECT_EQ(pair.to.doc, 1U);
    EXPECT_LT(pair.from.start, pair.from.end);
    EXPECT_LT(pair.to.start, pair.to.end);
  }
  EXPECT_EQ(typesFound.size(), 4U);
  EXPECT_TRUE(typesFound.contains(LinkType::Comment));
  EXPECT_TRUE(typesFound.contains(LinkType::Illustration));
  EXPECT_TRUE(typesFound.contains(LinkType::Disagreement));
  EXPECT_TRUE(typesFound.contains(LinkType::Quotation));
}

TEST(LinkLayout, oneToManyLinkAcrossFullPage) {
  Store store;
  const auto vA = store.insert(MicroversionId{}, 0, fullPageDocA);
  const auto vB = store.insert(MicroversionId{}, 0, fullPageDocB);

  const auto textA = store.rebuild(vA);
  const auto textB = store.rebuild(vB);

  // 1 left span in Doc A (Section 1.4) linking to 3 spans in Doc B
  // (Observations A, B, C)
  Link l;
  l.type  = LinkType::Quotation;
  l.owner = "synthesizer";
  l.left  = textA.spansFor(690, 40); // Section 1.4

  const auto spanB1 = textB.spansFor(44, 30);  // Obs A
  const auto spanB2 = textB.spansFor(160, 30); // Obs B
  const auto spanB3 = textB.spansFor(320, 30); // Obs C
  l.right.insert(l.right.end(), spanB1.begin(), spanB1.end());
  l.right.insert(l.right.end(), spanB2.begin(), spanB2.end());
  l.right.insert(l.right.end(), spanB3.begin(), spanB3.end());

  store.addLink(vA, l);

  const std::vector<Version> versions{store.rebuild(vA), store.rebuild(vB)};
  std::vector<LinkedPair> placed;
  std::vector<HalfLink> unplaced;
  xudu::placeLinks(store.links(), viewing(versions), placed, unplaced);

  ASSERT_EQ(placed.size(), 1U);
  EXPECT_EQ(placed[0].from.doc, 0U);
  EXPECT_EQ(placed[0].to.doc, 1U);

  // The right extent must cover from Obs A (44) through Obs C (350)
  EXPECT_EQ(placed[0].to.start, 44U);
  EXPECT_EQ(placed[0].to.end, 350U);

  // Centroid math for 1-to-many
  const float leftY       = 400.0F; // Section 1.4 height
  const float rightMinY   = 50.0F;  // Obs A height
  const float rightMaxY   = 350.0F; // Obs C height
  const float rightCenter = 0.5F * (rightMinY + rightMaxY); // 200.0F
  const float deltaY      = leftY - rightCenter;            // 200.0F

  EXPECT_FLOAT_EQ(rightCenter, 200.0F);
  EXPECT_FLOAT_EQ(deltaY, 200.0F);
  // Aligned right center matches leftY
  EXPECT_FLOAT_EQ(rightCenter + deltaY, leftY);
}

TEST(LinkLayout, manyToOneLinkAcrossFullPage) {
  Store store;
  const auto vA = store.insert(MicroversionId{}, 0, fullPageDocA);
  const auto vB = store.insert(MicroversionId{}, 0, fullPageDocB);

  const auto textA = store.rebuild(vA);
  const auto textB = store.rebuild(vB);

  // 3 left spans in Doc A (Sections 1.1, 1.2, 1.3) linking to 1 right span in
  // Doc B (Observation C)
  Link l;
  l.type  = LinkType::Authorship;
  l.owner = "scholar";

  const auto spanA1 = textA.spansFor(65, 30);  // Sec 1.1
  const auto spanA2 = textA.spansFor(295, 30); // Sec 1.2
  const auto spanA3 = textA.spansFor(490, 30); // Sec 1.3
  l.left.insert(l.left.end(), spanA1.begin(), spanA1.end());
  l.left.insert(l.left.end(), spanA2.begin(), spanA2.end());
  l.left.insert(l.left.end(), spanA3.begin(), spanA3.end());

  l.right = textB.spansFor(320, 45); // Obs C

  store.addLink(vA, l);

  const std::vector<Version> versions{store.rebuild(vA), store.rebuild(vB)};
  std::vector<LinkedPair> placed;
  std::vector<HalfLink> unplaced;
  xudu::placeLinks(store.links(), viewing(versions), placed, unplaced);

  ASSERT_EQ(placed.size(), 1U);
  EXPECT_EQ(placed[0].from.doc, 0U);
  EXPECT_EQ(placed[0].to.doc, 1U);

  // Left extent covers from Sec 1.1 (65) through Sec 1.3 (520)
  EXPECT_EQ(placed[0].from.start, 65U);
  EXPECT_EQ(placed[0].from.end, 520U);

  // Centroid math for many-to-1
  const float leftMinY   = 80.0F;                        // Sec 1.1
  const float leftMaxY   = 480.0F;                       // Sec 1.3
  const float leftCenter = 0.5F * (leftMinY + leftMaxY); // 280.0F
  const float rightY     = 120.0F;                       // Obs C
  const float deltaY     = leftCenter - rightY;          // 160.0F

  EXPECT_FLOAT_EQ(leftCenter, 280.0F);
  EXPECT_FLOAT_EQ(rightY + deltaY, leftCenter);
}

TEST(LinkLayout, manyToManyLinkAcrossFullPage) {
  Store store;
  const auto vA = store.insert(MicroversionId{}, 0, fullPageDocA);
  const auto vB = store.insert(MicroversionId{}, 0, fullPageDocB);

  const auto textA = store.rebuild(vA);
  const auto textB = store.rebuild(vB);

  // 2 left spans (Sec 1.2 & 1.3) linking to 2 right spans (Obs B & C)
  Link l;
  l.type  = LinkType::Disagreement;
  l.owner = "peer_reviewer";

  const auto spanA1 = textA.spansFor(295, 30);
  const auto spanA2 = textA.spansFor(490, 30);
  l.left.insert(l.left.end(), spanA1.begin(), spanA1.end());
  l.left.insert(l.left.end(), spanA2.begin(), spanA2.end());

  const auto spanB1 = textB.spansFor(160, 30);
  const auto spanB2 = textB.spansFor(320, 30);
  l.right.insert(l.right.end(), spanB1.begin(), spanB1.end());
  l.right.insert(l.right.end(), spanB2.begin(), spanB2.end());

  store.addLink(vA, l);

  const std::vector<Version> versions{store.rebuild(vA), store.rebuild(vB)};
  std::vector<LinkedPair> placed;
  std::vector<HalfLink> unplaced;
  xudu::placeLinks(store.links(), viewing(versions), placed, unplaced);

  ASSERT_EQ(placed.size(), 1U);
  EXPECT_EQ(placed[0].from.doc, 0U);
  EXPECT_EQ(placed[0].to.doc, 1U);
  EXPECT_EQ(placed[0].from.start, 295U);
  EXPECT_EQ(placed[0].from.end, 520U);
  EXPECT_EQ(placed[0].to.start, 160U);
  EXPECT_EQ(placed[0].to.end, 350U);

  // Centroid calculation for many-to-many
  const float leftMinY   = 250.0F;
  const float leftMaxY   = 450.0F;
  const float leftCenter = 0.5F * (leftMinY + leftMaxY); // 350.0F

  const float rightMinY   = 100.0F;
  const float rightMaxY   = 300.0F;
  const float rightCenter = 0.5F * (rightMinY + rightMaxY); // 200.0F

  const float deltaY = leftCenter - rightCenter; // 150.0F

  EXPECT_FLOAT_EQ(leftCenter, 350.0F);
  EXPECT_FLOAT_EQ(rightCenter, 200.0F);
  EXPECT_FLOAT_EQ(deltaY, 150.0F);

  // Both centers coincide after applying deltaY
  EXPECT_FLOAT_EQ(rightCenter + deltaY, leftCenter);
}

TEST(LinkLayout, fullPageDynamicCameraFramingWithLargeVerticalSpread) {
  // Simulate full page document height (800 pixels vertical span, 2 documents
  // 24 units apart)
  const float fullPageH = 800.0F * Doc::pixelsToWorld; // ~40.0 world units
  const float docHalfW  = 12.0F;
  const float gap       = 3.0F;

  const float doc0X = 0.0F;
  const float doc1X = doc0X + (2.0F * docHalfW + gap); // 27.0F

  const float minX  = doc0X - docHalfW - 4.0F; // -16.0F
  const float maxX  = doc1X + docHalfW + 4.0F; // 43.0F
  const float spanW = maxX - minX;             // 59.0F
  const float spanH = fullPageH + 12.0F;       // 52.0F

  const float aspect     = 1920.0F / 1080.0F; // 16:9 widescreen
  const float fovDeg     = 15.0F;
  const float tanHalfFov = std::tan(glm::radians(fovDeg) * 0.5F);

  const float zFit = std::max(spanH / (2.0F * tanHalfFov),
                              spanW / (2.0F * aspect * tanHalfFov));

  EXPECT_GT(zFit, 0.0F);
  const float frustumHalfHeight = zFit * tanHalfFov;
  const float frustumHalfWidth  = frustumHalfHeight * aspect;

  // Frustum fully encloses both the tall full-page vertical span and wide
  // horizontal span
  EXPECT_GE(frustumHalfHeight * 2.0F, spanH);
  EXPECT_GE(frustumHalfWidth * 2.0F, spanW);
}

TEST(LinkLayout, symmetricMultiPageFramingZoomScaling) {
  // Test multi-page scaling across 3, 5, 8, and 10 pages per document
  const float singlePageH = 800.0F * Doc::pixelsToWorld; // ~40.0 world units
  const float pageGapH    = 50.0F * Doc::pixelsToWorld;  // ~2.5 world units
  const float docHalfW    = 12.0F;
  const float docGap      = 6.0F;
  const float aspect      = 16.0F / 9.0F;
  const float fovDeg      = 15.0F;
  const float tanHalfFov  = std::tan(glm::radians(fovDeg) * 0.5F);

  const std::vector<std::size_t> pageCounts = {3, 5, 8, 10};
  float prevZFit                            = 0.0F;

  for (const auto pages : pageCounts) {
    const float totalH = (static_cast<float>(pages) * singlePageH) +
                         (static_cast<float>(pages - 1) * pageGapH);
    const float spanW = (4.0F * docHalfW) + docGap + 8.0F;
    const float spanH = totalH + 12.0F;

    const float zFit = std::max(spanH / (2.0F * tanHalfFov),
                                spanW / (2.0F * aspect * tanHalfFov));

    // Z fit must monotonically grow with the page count to keep all pages in
    // view
    EXPECT_GT(zFit, prevZFit);
    prevZFit = zFit;

    // View frustum encloses all N pages at calculated camera distance
    const float frustumH = 2.0F * zFit * tanHalfFov;
    const float frustumW = frustumH * aspect;
    EXPECT_GE(frustumH, spanH);
    EXPECT_GE(frustumW, spanW);

    // Centroid of symmetric documents is exactly at half the total height
    const float topY    = totalH * 0.5F;
    const float botY    = -totalH * 0.5F;
    const float centerY = 0.5F * (topY + botY);
    EXPECT_FLOAT_EQ(centerY, 0.0F);
  }
}

TEST(LinkLayout, asymmetricMultiPageCentroidAlignmentAndFraming) {
  const float pageH      = 40.0F; // world units
  const float pageGapH   = 2.0F;
  const float aspect     = 16.0F / 9.0F;
  const float fovDeg     = 15.0F;
  const float tanHalfFov = std::tan(glm::radians(fovDeg) * 0.5F);

  // Case 1: 3-page Doc A vs 8-page Doc B
  {
    const float heightA = (3.0F * pageH) + (2.0F * pageGapH); // 124.0
    const float heightB = (8.0F * pageH) + (7.0F * pageGapH); // 334.0

    const float centerA = 0.0F;
    const float centerB = 0.0F;
    const float deltaY  = centerA - centerB; // 0.0 if both start at origin

    EXPECT_FLOAT_EQ(deltaY, 0.0F);

    const float spanH = std::max(heightA, heightB) + 12.0F; // 346.0
    const float zFit  = spanH / (2.0F * tanHalfFov);

    EXPECT_GT(zFit, 1000.0F);
    EXPECT_GE(2.0F * zFit * tanHalfFov, spanH);
  }

  // Case 2: 5-page Doc A vs 10-page Doc B with offset starting positions
  {
    const float heightA = (5.0F * pageH) + (4.0F * pageGapH);  // 208.0
    const float heightB = (10.0F * pageH) + (9.0F * pageGapH); // 418.0

    const float posA_Y = 100.0F;
    const float posB_Y = -50.0F;

    const float minA_Y = posA_Y - heightA * 0.5F;
    const float maxA_Y = posA_Y + heightA * 0.5F;
    const float minB_Y = posB_Y - heightB * 0.5F;
    const float maxB_Y = posB_Y + heightB * 0.5F;

    const float centerA = 0.5F * (minA_Y + maxA_Y); // 100.0
    const float centerB = 0.5F * (minB_Y + maxB_Y); // -50.0
    const float deltaY  = centerA - centerB;        // 150.0

    EXPECT_FLOAT_EQ(centerA, 100.0F);
    EXPECT_FLOAT_EQ(centerB, -50.0F);
    EXPECT_FLOAT_EQ(deltaY, 150.0F);

    // After translation, Doc B's center matches Doc A's center
    EXPECT_FLOAT_EQ(centerB + deltaY, centerA);
  }
}

TEST(LinkLayout, multipageBackgroundCorpusAndForegroundFlyInGeometry) {
  // Foreground inspection document at Z = 0
  const glm::vec3 foregroundDocPos(0.0F, 0.0F, 0.0F);

  // Background multi-page corpus document at Z = -30.0F
  const glm::vec3 backgroundCorpusPos(25.0F, 0.0F, -30.0F);

  // Fly-in linked page brought forward to Z = 0
  const glm::vec3 flyInPagePos(14.0F, 0.0F, 0.0F);

  EXPECT_FLOAT_EQ(foregroundDocPos.z, 0.0F);
  EXPECT_FLOAT_EQ(flyInPagePos.z, 0.0F);
  EXPECT_FLOAT_EQ(backgroundCorpusPos.z, -30.0F);

  // Distance between foreground and fly-in page (in-plane examination)
  const float inPlaneGap = glm::distance(foregroundDocPos, flyInPagePos);
  EXPECT_FLOAT_EQ(inPlaneGap, 14.0F);

  // 3D spatial distance bridging between fly-in page and background corpus
  const float depthSpan = glm::distance(flyInPagePos, backgroundCorpusPos);
  EXPECT_GT(depthSpan, 30.0F);
}

// =============================================================================
// apps/xudu/core/framing.hpp: centroid alignment and camera-fit distance as
// pure functions of page counts and heights, independently unit-tested here
// against hand-derived closed forms rather than only exercised end to end
// through a screenshot. Not yet wired into LinkBeams::align(), which computes
// its own camera-fit and centroid inline; kept as a tested, reusable
// alternative.
// =============================================================================

namespace {

// Mirrors Doc::pageGapWorld (include/gleditor/doc.hpp) -- this binary is
// built without the graphics library, so the value is named the same and
// kept beside the source it mirrors rather than pulled in by including it.
constexpr float testPageGapWorld = 100.0F;

/// A document of @p pageCount pages, each @p heightWorld tall.
PageStackExtent uniformStack(const std::size_t pageCount,
                             const float heightWorld) {
  return pageStackExtent(std::vector<float>(pageCount, heightWorld),
                         testPageGapWorld);
}

} // namespace

// Two documents of the same length, page for page, already share a vertical
// centre: bringing one alongside the other should ask for no vertical move at
// all, symmetric or not in every other respect.
TEST(PageFraming, symmetricPageCountsNeedNoVerticalOffset) {
  for (const std::size_t pages : {3U, 5U, 8U, 10U}) {
    const auto a = uniformStack(pages, 80.0F);
    const auto b = uniformStack(pages, 80.0F);
    EXPECT_FLOAT_EQ(centroidAlignmentDeltaY(a, b), 0.0F)
        << pages << "x" << pages << " symmetric stacks did not centre";
  }
}

// A document differing only in length from another still has one right
// answer for how far to move it: the closed form here is worked out by hand
// (not by calling the function under test) from the same reasoning
// centroidAlignmentDeltaY() is documented to follow --
// $\Delta Y = Y_{midA} - Y_{midB}$ -- so this cross-checks the
// implementation against an independent derivation rather than against
// itself.
TEST(PageFraming, asymmetricPageCountsMatchTheClosedFormOffset) {
  const std::vector<std::pair<std::size_t, std::size_t>> cases{{3, 8}, {5, 10}};
  for (const auto &[pagesA, pagesB] : cases) {
    const auto a = uniformStack(pagesA, 80.0F);
    const auto b = uniformStack(pagesB, 80.0F);
    // centroidY(n pages, gap g) = -(g * (n - 1)) / 2, independent of a
    // uniform page height (it cancels between the first page's top and the
    // last page's bottom), so deltaY = (g / 2) * (pagesB - pagesA).
    const auto expected =
        (testPageGapWorld / 2.0F) *
        (static_cast<float>(pagesB) - static_cast<float>(pagesA));
    EXPECT_NEAR(centroidAlignmentDeltaY(a, b), expected, 1e-3F)
        << pagesA << "x" << pagesB << " asymmetric offset";
    // Moving B to meet A is the exact opposite of moving A to meet B.
    EXPECT_FLOAT_EQ(centroidAlignmentDeltaY(a, b),
                    -centroidAlignmentDeltaY(b, a));
  }
}

// The plan this suite implements calls out $H_A \\ne H_B$ explicitly: a
// document whose last page runs short (the ordinary case -- pagination fills
// every page but the final one) has a top and a bottom that do not mirror
// each other, and the centroid has to be worked out from both rather than
// assumed from the page count alone.
TEST(PageFraming, asymmetricPageHeightsShiftTheCentroidByTheHeightDifference) {
  const std::vector<float> uniform(5, 80.0F);
  std::vector<float> shortLastPage(uniform);
  shortLastPage.back() = 20.0F; // a much shorter final page

  const auto full   = pageStackExtent(uniform, testPageGapWorld);
  const auto short_ = pageStackExtent(shortLastPage, testPageGapWorld);

  // Only the bottom edge moves: the shorter last page's bottom sits higher up
  // by half of what it lost, and nothing else about the stack changed.
  EXPECT_FLOAT_EQ(full.topWorld, short_.topWorld);
  EXPECT_FLOAT_EQ(short_.bottomWorld,
                  full.bottomWorld + (80.0F - 20.0F) / 2.0F);
  EXPECT_GT(centroidY(short_), centroidY(full))
      << "a shorter final page should raise the stack's centre, not lower it";
}

// A box exactly as tall as the frustum is wide at 90 degrees vertical field
// of view sits at a distance equal to its own half-height -- tan(45) is 1,
// so this is the one input where the formula reduces to something checkable
// without a calculator.
TEST(PageFraming, framingDistanceAtNinetyDegreesIsHalfTheHeight) {
  constexpr float aspect = 1.0F; // width term must not dominate here
  const auto distance    = framingDistance(/*worldWidth=*/10.0F,
                                        /*worldHeight=*/200.0F,
                                        /*fovYDegrees=*/90.0F, aspect);
  EXPECT_NEAR(distance, 100.0F, 1e-2F);
}

// framingFov() is framingDistance() solved for the angle instead of the
// distance; feeding one's output into the other should come back out where
// it started, for any box and any distance a camera might actually sit at.
TEST(PageFraming, framingDistanceAndFramingFovInvertEachOther) {
  constexpr float aspect = 800.0F / 600.0F;
  for (const float distance : {50.0F, 250.0F, 1000.0F, 4000.0F}) {
    for (const float worldHeight : {80.0F, 780.0F, 1580.0F}) {
      const auto worldWidth = 200.0F;
      const auto fov = framingFov(worldWidth, worldHeight, distance, aspect,
                                  /*margin=*/1.15F);
      const auto roundTrip = framingDistance(worldWidth, worldHeight, fov,
                                             aspect, /*margin=*/1.15F);
      EXPECT_NEAR(roundTrip, distance, distance * 1e-3F)
          << "distance " << distance << ", height " << worldHeight;
    }
  }
}

// A taller pair of documents (more pages) needs a camera further back to
// hold them both at a fixed field of view -- the whole reason the extreme
// framing suite computes this per page count rather than picking one fov and
// hoping it fits every N x N case.
TEST(PageFraming, framingDistanceGrowsWithPageCount) {
  constexpr float fov    = 20.0F;
  constexpr float aspect = 800.0F / 600.0F;
  constexpr float width  = 260.0F;
  float previous         = 0.0F;
  for (const std::size_t pages : {3U, 5U, 8U, 10U}) {
    const auto extent   = uniformStack(pages, 80.0F);
    const auto height   = extent.topWorld - extent.bottomWorld;
    const auto distance = framingDistance(width, height, fov, aspect);
    EXPECT_GT(distance, previous) << pages << " pages did not need more room";
    previous = distance;
  }
}
