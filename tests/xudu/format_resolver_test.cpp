/**
 * @file format_resolver_test.cpp
 * @brief Comprehensive tests for FormatResolver across documents and manifold
 * cells.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "common/xanadu/format.hpp"
#include "common/xanadu/format_resolver.hpp"
#include "common/xanadu/microversion.hpp"
#include "common/xanadu/ops.hpp"
#include "common/xanadu/publication.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/zigzag/manifold.hpp"

namespace {

using xanadu::allFormatAttributes;
using xanadu::FormatAttribute;
using xanadu::FormatResolver;
using xanadu::Link;
using xanadu::LinkType;
using xanadu::MicroversionId;
using xanadu::PrimediaSpan;
using xanadu::Store;
using xanadu::vocabularySpanFor;

TEST(FormatResolverTest, SharedPermascrollInheritsAcrossAuthorities) {
  Store source;
  Store destination(source.userPermascrollPtr());
  Store unrelated;
  const auto text  = source.insert({}, 0, "alpha bravo");
  const auto spans = source.rebuild(text).spansFor(0, 5);
  source.setFormat(text, spans, FormatAttribute::Bold, true);
  const auto count = destination.opCount();
  const auto bytes = destination.userPermascroll().size();
  FormatResolver inherited(destination);
  inherited.include(source, destination);
  const auto result = inherited.resolveSpans(spans);
  ASSERT_EQ(result.decoratedRanges.size(), 1U);
  EXPECT_EQ(result.decoratedRanges.front().end, 5U);
  EXPECT_EQ(destination.opCount(), count);
  EXPECT_EQ(destination.userPermascroll().size(), bytes);

  // Slot zero and offsets alone must never equate two authors' primedia.
  unrelated.insert({}, 0, "alpha bravo");
  FormatResolver isolated(unrelated);
  isolated.include(source, unrelated);
  EXPECT_TRUE(isolated.resolveSpans(spans).decoratedRanges.empty());
}

TEST(FormatResolverTest, ExternalScrollIdsAreTranslatedByIdentity) {
  Store source;
  Store destination;
  xanadu::Scroll shared;
  shared.publisher     = xanadu::createMutableKeys().publicKey;
  shared.salt          = "shared";
  auto other           = shared;
  other.salt           = "unrelated";
  const auto from      = source.addScroll(shared);
  const auto collision = destination.addScroll(other);
  const auto into      = destination.addScroll(shared);
  ASSERT_EQ(from, collision);
  ASSERT_NE(from, into);
  const PrimediaSpan target{.scroll = from, .start = 42, .length = 5};
  source.setFormat({}, std::span{&target, 1}, FormatAttribute::Italic, true);
  FormatResolver resolver(destination);
  resolver.include(source, destination);
  const PrimediaSpan wrong{.scroll = collision, .start = 42, .length = 5};
  EXPECT_TRUE(
      resolver.resolveSpans(std::span{&wrong, 1}).decoratedRanges.empty());
  const PrimediaSpan translated{.scroll = into, .start = 42, .length = 5};
  EXPECT_EQ(
      resolver.resolveSpans(std::span{&translated, 1}).decoratedRanges.size(),
      1U);
  EXPECT_EQ(destination.scrolls().size(), 2U);
  EXPECT_EQ(destination.opCount(), 0U);
}

TEST(FormatResolverTest,
     RemovingPartOfAttributePreservesOtherFormattingAndHistory) {
  Store store;
  auto head      = store.insert({}, 0, "alpha bravo");
  const auto all = store.rebuild(head).spansFor(0, 11);
  head           = store.setFormat(head, all, FormatAttribute::Overline, true);
  // Duplicate links and overlapping endsets must all lose the selected range.
  head = store.setFormat(head, all, FormatAttribute::Overline, true);
  head = store.setFormat(head, all, FormatAttribute::Bold, true);
  const auto oldHead  = head;
  const auto oldFold  = store.rebuildManifold(head);
  const auto oldLinks = oldFold.links(store);
  const auto selected = store.rebuild(head).spansFor(2, 5);
  const auto count    = store.opCount();
  const auto bytes    = store.userPermascroll().size();
  head = store.setFormat(head, selected, FormatAttribute::Overline, false);
  EXPECT_GT(store.opCount(), count);
  EXPECT_EQ(store.userPermascroll().size(), bytes);
  EXPECT_EQ(store.rebuild(head).materialize(store), "alpha bravo");
  EXPECT_EQ(store.rebuildManifold(oldHead).links(store), oldLinks);
  const auto result = FormatResolver(store).resolveSpans(all);
  std::vector<std::pair<std::uint32_t, std::uint32_t>> overlines;
  for (const auto &range : result.decoratedRanges) {
    if (gleditor::hasDecoration(range.decorations,
                                gleditor::Decoration::Overline)) {
      overlines.emplace_back(range.start, range.end);
    } else {
      EXPECT_EQ(range.start, 0U);
      EXPECT_EQ(range.end, 11U);
      EXPECT_TRUE(gleditor::hasDecoration(range.decorations,
                                          gleditor::Decoration::Bold));
    }
  }
  EXPECT_THAT(overlines, testing::UnorderedElementsAre(
                             std::pair{0U, 2U}, std::pair{7U, 11U},
                             std::pair{0U, 2U}, std::pair{7U, 11U}));
  head = store.setFormat(head, all, FormatAttribute::Overline, false);
  EXPECT_EQ(FormatResolver(store).resolveSpans(all).decoratedRanges.size(), 1U);
  const auto unchanged =
      store.setFormat(head, all, FormatAttribute::Overline, false);
  EXPECT_EQ(unchanged, head);
}

TEST(FormatResolverTest, FormatResolverInitializationAndFiltering) {
  Store store;
  const auto v1 =
      store.insert(MicroversionId{}, 0, "Nelsonian intertwingularity");
  const auto span = store.rebuild(v1).spansFor(0, 9);
  ASSERT_FALSE(span.empty());

  // 1) Add non-format link (Comment)
  Link commentLink;
  commentLink.type = LinkType::Comment;
  commentLink.left = span;
  auto at          = store.addLink(v1, commentLink);

  // 2) Add valid format link (Bold)
  Link boldLink;
  boldLink.type = LinkType::Format;
  boldLink.left = span;
  boldLink.right.push_back(vocabularySpanFor(FormatAttribute::Bold));
  at = store.addLink(at, boldLink);

  FormatResolver resolver(store);
  const auto res = resolver.resolveSpans(span);
  ASSERT_EQ(res.decoratedRanges.size(), 1U);
  EXPECT_EQ(res.decoratedRanges[0].start, 0U);
  EXPECT_EQ(res.decoratedRanges[0].end, 9U);
  EXPECT_TRUE(gleditor::hasDecoration(res.decoratedRanges[0].decorations,
                                      gleditor::Decoration::Bold));
  EXPECT_FALSE(gleditor::hasDecoration(res.decoratedRanges[0].decorations,
                                       gleditor::Decoration::Italic));
}

TEST(FormatResolverTest, OverlappingDecorationsAndAlignment) {
  Store store;
  const auto v1 =
      store.insert(MicroversionId{}, 0, "Hypermedia architecture in 3D");
  const auto versionObj = store.rebuild(v1);
  const auto spanBold   = versionObj.spansFor(0, 10); // "Hypermedia"
  const auto spanItalic = versionObj.spansFor(5, 17); // "media architecture"
  const auto spanAlign  = versionObj.spansFor(0, 26); // whole phrase

  // Bold on "Hypermedia" [0..10)
  Link boldLink;
  boldLink.type = LinkType::Format;
  boldLink.left = spanBold;
  boldLink.right.push_back(vocabularySpanFor(FormatAttribute::Bold));
  auto at = store.addLink(v1, boldLink);

  // Italic on "media architecture" [5..22)
  Link italicLink;
  italicLink.type = LinkType::Format;
  italicLink.left = spanItalic;
  italicLink.right.push_back(vocabularySpanFor(FormatAttribute::Italic));
  at = store.addLink(at, italicLink);

  // AlignCentre on whole phrase
  Link alignLink;
  alignLink.type = LinkType::Format;
  alignLink.left = spanAlign;
  alignLink.right.push_back(vocabularySpanFor(FormatAttribute::AlignCentre));
  at = store.addLink(at, alignLink);

  FormatResolver resolver(store);
  const auto res = resolver.resolveVersion(versionObj);

  EXPECT_EQ(res.decoratedRanges.size(), 2U);
  EXPECT_EQ(res.blockStyles.size(), 1U);
  EXPECT_EQ(res.blockStyles[0].align, gleditor::TextAlign::Centre);

  // Check flag computation
  const auto flags     = resolver.computeFormatFlags(versionObj.pieces());
  const auto boldBit   = 1U << static_cast<std::uint8_t>(FormatAttribute::Bold);
  const auto italicBit = 1U
                         << static_cast<std::uint8_t>(FormatAttribute::Italic);
  const auto alignBit =
      1U << static_cast<std::uint8_t>(FormatAttribute::AlignCentre);

  EXPECT_TRUE((flags & boldBit) != 0);
  EXPECT_TRUE((flags & italicBit) != 0);
  EXPECT_TRUE((flags & alignBit) != 0);
}

TEST(FormatResolverTest, CellResolutionAndFormatFlagsCaching) {
  Store store;
  auto at       = store.sliceGenesis(MicroversionId{});
  at            = store.makeCell(at, "Plain unformatted cell");
  const auto c1 = store.cellRefOf(at);
  at            = store.makeCell(at, "Cell with bold formatting");
  const auto c2 = store.cellRefOf(at);

  auto manifold      = store.rebuildManifold(at);
  const auto spansC2 = manifold.contentOf(c2);
  ASSERT_FALSE(spansC2.empty());

  // Attach Bold format link to c2's primedia span
  Link boldLink;
  boldLink.type = LinkType::Format;
  boldLink.left = std::vector<PrimediaSpan>(spansC2.begin(), spansC2.end());
  boldLink.right.push_back(vocabularySpanFor(FormatAttribute::Bold));
  store.addLink(at, boldLink);

  FormatResolver resolver(store);
  resolver.updateManifoldFormatFlags(manifold);

  const auto slot1 = manifold.slot(c1);
  const auto slot2 = manifold.slot(c2);
  ASSERT_TRUE(slot1.has_value());
  ASSERT_TRUE(slot2.has_value());

  // Cell 1: Fast-path zero flags
  EXPECT_EQ(slot1->formatFlags, 0U);

  // Cell 2: Non-zero flags with Bold bit
  const auto boldBit = 1U << static_cast<std::uint8_t>(FormatAttribute::Bold);
  EXPECT_EQ(slot2->formatFlags, boldBit);

  // Resolve c2 formatting
  const auto res2 = resolver.resolveCell(manifold, c2);
  ASSERT_EQ(res2.decoratedRanges.size(), 1U);
  EXPECT_TRUE(gleditor::hasDecoration(res2.decoratedRanges[0].decorations,
                                      gleditor::Decoration::Bold));
}

TEST(FormatResolverTest, CrossDomainFormatInheritanceDocToCell) {
  Store store;
  // 1) Create document text
  const auto docVer =
      store.insert(MicroversionId{}, 0, "Intertwingled universe");
  const auto docSpans =
      store.rebuild(docVer).spansFor(0, 13); // "Intertwingled"

  // 2) Attach Italic format link to document span
  Link italicLink;
  italicLink.type = LinkType::Format;
  italicLink.left = docSpans;
  italicLink.right.push_back(vocabularySpanFor(FormatAttribute::Italic));
  auto at = store.addLink(docVer, italicLink);

  // 3) Create cell and transclude the formatted span into the cell
  at                 = store.makeCell(at, "");
  const auto cellRef = store.cellRefOf(at);
  at = store.spliceCellSpan(at, cellRef, 0, 0, docSpans.front());

  // 4) Rebuild manifold and update flags
  auto manifold = store.rebuildManifold(at);
  FormatResolver resolver(store);
  resolver.updateManifoldFormatFlags(manifold);

  const auto slot = manifold.slot(cellRef);
  ASSERT_TRUE(slot.has_value());

  const auto italicBit = 1U
                         << static_cast<std::uint8_t>(FormatAttribute::Italic);
  EXPECT_EQ(slot->formatFlags, italicBit);

  const auto cellRes = resolver.resolveCell(manifold, cellRef);
  ASSERT_EQ(cellRes.decoratedRanges.size(), 1U);
  EXPECT_TRUE(gleditor::hasDecoration(cellRes.decoratedRanges[0].decorations,
                                      gleditor::Decoration::Italic));
}

} // namespace
