/**
 * @file radial_formatting_test.cpp
 * @brief Unit tests for radial menu configuration and formatting attributes.
 */
#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include <xudu/core/format.hpp>
#include <xudu/core/microversion.hpp>
#include <xudu/core/ops.hpp>
#include <xudu/core/store.hpp>
#include <xudu/core/system_docs.hpp>

namespace {

using xudu::FormatAttribute;
using xudu::formatAttributeFromDecoration;
using xudu::formatAttributeFromTextAlign;
using xudu::formatAttributeName;
using xudu::Link;
using xudu::LinkType;
using xudu::MicroversionId;
using xudu::parseRadialConfig;
using xudu::Store;
using xudu::textAlignFromFormatAttribute;
using xudu::vocabularySpanFor;

TEST(RadialFormattingTest, ParseRadialConfigFromYaml) {
  constexpr std::string_view kYaml = R"(
radialMenu:
  radius: 145.0
  innerRadius: 50.0
  actions:
    - id: "format:bold"
      label: "Bold"
      icon: "B"
    - id: "format:superscript"
      label: "Superscript"
      icon: "X²"
    - id: "format:subscript"
      label: "Subscript"
      icon: "X₂"
    - id: "group:align"
      label: "Align"
      icon: "="
      subActions:
        - id: "align:left"
          label: "Left"
          icon: "⇤"
        - id: "align:centre"
          label: "Centre"
          icon: "↔"
        - id: "align:right"
          label: "Right"
          icon: "⇥"
        - id: "align:justify"
          label: "Justify"
          icon: "☰"
)";

  const auto cfg = parseRadialConfig(kYaml);
  EXPECT_FLOAT_EQ(cfg.radius, 145.0F);
  EXPECT_FLOAT_EQ(cfg.innerRadius, 50.0F);
  ASSERT_EQ(cfg.actions.size(), 4U);

  EXPECT_EQ(cfg.actions[0].id, "format:bold");
  EXPECT_EQ(cfg.actions[0].label, "Bold");
  EXPECT_EQ(cfg.actions[0].icon, "B");

  EXPECT_EQ(cfg.actions[1].id, "format:superscript");
  EXPECT_EQ(cfg.actions[1].label, "Superscript");
  EXPECT_EQ(cfg.actions[1].icon, "X²");

  EXPECT_EQ(cfg.actions[2].id, "format:subscript");
  EXPECT_EQ(cfg.actions[2].label, "Subscript");
  EXPECT_EQ(cfg.actions[2].icon, "X₂");

  EXPECT_EQ(cfg.actions[3].id, "group:align");
  ASSERT_EQ(cfg.actions[3].subActions.size(), 4U);
  EXPECT_EQ(cfg.actions[3].subActions[0].id, "align:left");
  EXPECT_EQ(cfg.actions[3].subActions[1].id, "align:centre");
  EXPECT_EQ(cfg.actions[3].subActions[2].id, "align:right");
  EXPECT_EQ(cfg.actions[3].subActions[3].id, "align:justify");
}

TEST(RadialFormattingTest, ParseRadialConfigFallbackOnEmpty) {
  const auto cfg = parseRadialConfig("");
  EXPECT_GT(cfg.radius, 0.0F);
  EXPECT_GT(cfg.innerRadius, 0.0F);
  EXPECT_FALSE(cfg.actions.empty());
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
