/**
 * @file radial_formatting_test.cpp
 * @brief Unit tests for radial menu configuration and formatting attributes.
 */
#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "common/xanadu/format.hpp"
#include "common/xanadu/microversion.hpp"
#include "common/xanadu/ops.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/system_docs.hpp"

namespace {

using xanadu::FormatAttribute;
using xanadu::formatAttributeFromDecoration;
using xanadu::formatAttributeFromTextAlign;
using xanadu::formatAttributeName;
using xanadu::Link;
using xanadu::LinkType;
using xanadu::MicroversionId;
using xanadu::Store;
using xanadu::textAlignFromFormatAttribute;
using xanadu::UIConfig;
using xanadu::vocabularySpanFor;

TEST(RadialFormattingTest, LoadRadialConfigFromStore) {
  Store store;
  xanadu::initializeSystemStore(store, xanadu::SystemDocKind::UI);

  const auto cfg = UIConfig::fromStore(store);
  EXPECT_FLOAT_EQ(cfg.radialMenu.radius, 130.0F);
  EXPECT_FLOAT_EQ(cfg.radialMenu.innerRadius, 42.0F);
  EXPECT_FALSE(cfg.radialMenu.actions.empty());
}

TEST(RadialFormattingTest, LoadRadialConfigFallbackOnEmptyStore) {
  Store store;
  const auto cfg = UIConfig::fromStore(store);
  EXPECT_GT(cfg.radialMenu.radius, 0.0F);
  EXPECT_GT(cfg.radialMenu.innerRadius, 0.0F);
  EXPECT_FALSE(cfg.radialMenu.actions.empty());
}

TEST(RadialFormattingTest, FormatAttributesAndAlignments) {
  EXPECT_STREQ(formatAttributeName(FormatAttribute::Superscript),
               "superscript");
  EXPECT_STREQ(formatAttributeName(FormatAttribute::Subscript), "subscript");
  EXPECT_STREQ(formatAttributeName(FormatAttribute::AlignLeft), "align-left");
  EXPECT_STREQ(formatAttributeName(FormatAttribute::AlignCentre),
               "align-centre");
  EXPECT_STREQ(formatAttributeName(FormatAttribute::AlignRight), "align-right");
  EXPECT_STREQ(formatAttributeName(FormatAttribute::AlignJustify),
               "align-justify");

  EXPECT_EQ(formatAttributeFromDecoration(gleditor::Decoration::Superscript),
            FormatAttribute::Superscript);
  EXPECT_EQ(formatAttributeFromDecoration(gleditor::Decoration::Subscript),
            FormatAttribute::Subscript);

  EXPECT_EQ(formatAttributeFromTextAlign(gleditor::TextAlign::Left),
            FormatAttribute::AlignLeft);
  EXPECT_EQ(formatAttributeFromTextAlign(gleditor::TextAlign::Centre),
            FormatAttribute::AlignCentre);
  EXPECT_EQ(formatAttributeFromTextAlign(gleditor::TextAlign::Right),
            FormatAttribute::AlignRight);
  EXPECT_EQ(formatAttributeFromTextAlign(gleditor::TextAlign::Justify),
            FormatAttribute::AlignJustify);

  EXPECT_EQ(textAlignFromFormatAttribute(FormatAttribute::AlignLeft),
            gleditor::TextAlign::Left);
  EXPECT_EQ(textAlignFromFormatAttribute(FormatAttribute::AlignCentre),
            gleditor::TextAlign::Centre);
  EXPECT_EQ(textAlignFromFormatAttribute(FormatAttribute::AlignRight),
            gleditor::TextAlign::Right);
  EXPECT_EQ(textAlignFromFormatAttribute(FormatAttribute::AlignJustify),
            gleditor::TextAlign::Justify);
}

TEST(RadialFormattingTest, StoreFormatLinksForSuperscriptAndSubscript) {
  Store store;
  const auto version =
      store.insert(MicroversionId{}, 0, "E = mc2 and H2O molecules");
  // "2" in mc2 is at offset 8, length 1
  const auto mc2Super = store.rebuild(version).spansFor(8, 1);
  // "2" in H2O is at offset 15, length 1
  const auto h2oSub = store.rebuild(version).spansFor(15, 1);

  Link link1;
  link1.type  = LinkType::Format;
  link1.owner = "author";
  link1.left  = mc2Super;
  link1.right.push_back(vocabularySpanFor(FormatAttribute::Superscript));
  const auto v1 = store.addLink(version, link1);

  Link link2;
  link2.type  = LinkType::Format;
  link2.owner = "author";
  link2.left  = h2oSub;
  link2.right.push_back(vocabularySpanFor(FormatAttribute::Subscript));
  const auto v2 = store.addLink(v1, link2);

  ASSERT_EQ(store.links().size(), 2U);

  // Verify format attribute detection
  std::size_t countSuper = 0;
  std::size_t countSub   = 0;
  for (const auto &[_, link] : store.links()) {
    const auto attr = store.formatAttributeOf(link);
    ASSERT_TRUE(attr.has_value());
    if (*attr == FormatAttribute::Superscript) {
      ++countSuper;
    } else if (*attr == FormatAttribute::Subscript) {
      ++countSub;
    }
  }
  EXPECT_EQ(countSuper, 1U);
  EXPECT_EQ(countSub, 1U);
}

TEST(RadialFormattingTest, StoreFormatLinksForAlignment) {
  Store store;
  const auto version =
      store.insert(MicroversionId{}, 0, "Centered Headline\nJustified body.");
  const auto headlineSpans = store.rebuild(version).spansFor(0, 17);

  Link alignLink;
  alignLink.type  = LinkType::Format;
  alignLink.owner = "author";
  alignLink.left  = headlineSpans;
  alignLink.right.push_back(vocabularySpanFor(FormatAttribute::AlignCentre));
  store.addLink(version, alignLink);

  ASSERT_EQ(store.links().size(), 1U);
  const auto attr = store.formatAttributeOf(store.links().begin()->second);
  ASSERT_TRUE(attr.has_value());
  EXPECT_EQ(*attr, FormatAttribute::AlignCentre);
}

} // namespace
