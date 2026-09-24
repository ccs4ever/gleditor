/**
 * @file format_resolver_test.cpp
 * @brief Comprehensive tests for FormatResolver across documents and manifold
 * cells.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <xudu/core/format.hpp>
#include <xudu/core/format_resolver.hpp>
#include <xudu/core/microversion.hpp>
#include <xudu/core/ops.hpp>
#include <xudu/core/store.hpp>
#include <zigzag/core/manifold.hpp>

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
  store.addLink(v1, commentLink);

  // 2) Add valid format link (Bold)
  Link boldLink;
  boldLink.type = LinkType::Format;
  boldLink.left = span;
  boldLink.right.push_back(vocabularySpanFor(FormatAttribute::Bold));
  store.addLink(v1, boldLink);

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
  store.addLink(v1, boldLink);

  // Italic on "media architecture" [5..22)
  Link italicLink;
  italicLink.type = LinkType::Format;
  italicLink.left = spanItalic;
  italicLink.right.push_back(vocabularySpanFor(FormatAttribute::Italic));
  store.addLink(v1, italicLink);

  // AlignCentre on whole phrase
  Link alignLink;
  alignLink.type = LinkType::Format;
  alignLink.left = spanAlign;
  alignLink.right.push_back(vocabularySpanFor(FormatAttribute::AlignCentre));
  store.addLink(v1, alignLink);

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
  store.addLink(docVer, italicLink);

  // 3) Create cell and transclude the formatted span into the cell
  auto at            = store.sliceGenesis(docVer);
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
