/**
 * @file layoutfilade_test.cpp
 * @brief Unit tests for the True Layoutfilade 2D Coordinate & Height
 * B-Enfilade.
 */
#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>

#include "common/xanadu/enfilade/layoutfilade.hpp"
#include <gleditor/layout_box.hpp>

namespace {

using xanadu::enfilade::LayoutCrum;
using xanadu::enfilade::LayoutDsp;
using xanadu::enfilade::LayoutEntry;
using xanadu::enfilade::LayoutEntryKind;
using xanadu::enfilade::Layoutfilade;
using xanadu::enfilade::LayoutWid;

TEST(LayoutfiladeTest, MonoidAxiomsAndAction) {
  // 1. LayoutDsp Monoid
  const LayoutDsp d0{};
  EXPECT_TRUE(d0.isIdentity());

  const LayoutDsp d1{.deltaBytes = 100, .deltaYPx = 250.0F, .deltaLines = 10};
  EXPECT_FALSE(d1.isIdentity());

  const LayoutDsp d2{.deltaBytes = 50, .deltaYPx = 120.0F, .deltaLines = 5};
  const auto d12 = d1.compose(d2);
  EXPECT_EQ(d12.deltaBytes, 150U);
  EXPECT_FLOAT_EQ(d12.deltaYPx, 370.0F);
  EXPECT_EQ(d12.deltaLines, 15U);

  // Identity composition: d ∘ e == d
  EXPECT_EQ(d1.compose(d0), d1);
  EXPECT_EQ(d0.compose(d1), d1);

  // 2. LayoutWid Monoid
  const LayoutWid w0{};
  EXPECT_TRUE(w0.isEmpty());

  const LayoutWid w1{
      .totalBytes     = 200,
      .totalHeightPx  = 500.0F,
      .lineCount      = 20,
      .maxLineWidthPx = 400.0F,
      .mediaBoxCount  = 2,
  };
  EXPECT_FALSE(w1.isEmpty());

  const LayoutWid w2{
      .totalBytes     = 300,
      .totalHeightPx  = 700.0F,
      .lineCount      = 30,
      .maxLineWidthPx = 600.0F,
      .mediaBoxCount  = 3,
  };

  // Associative combine: w1 ⊕ w2
  const auto w12 = w1.combine(w2);
  EXPECT_EQ(w12.totalBytes, 500U);
  EXPECT_FLOAT_EQ(w12.totalHeightPx, 1200.0F);
  EXPECT_EQ(w12.lineCount, 50U);
  EXPECT_FLOAT_EQ(w12.maxLineWidthPx, 600.0F);
  EXPECT_EQ(w12.mediaBoxCount, 5U);

  // Identity combine: w ⊕ 0 == w
  EXPECT_EQ(w1.combine(w0), w1);
  EXPECT_EQ(w0.combine(w1), w1);

  // 3. Action
  EXPECT_EQ(d1.act(w1), w1);
}

TEST(LayoutfiladeTest, CoordinateDescentAndR9) {
  constexpr std::size_t kNumLines = 1000;
  std::vector<LayoutEntry> entries;
  entries.reserve(kNumLines);

  for (std::size_t i = 0; i < kNumLines; ++i) {
    const auto len = static_cast<std::uint32_t>(30 + (i % 50));
    const float h  = 16.0F + static_cast<float>(i % 8);
    const float w  = static_cast<float>(len) * 7.5F;

    entries.push_back(LayoutEntry{
        .byteLength       = len,
        .heightPx         = h,
        .widthPx          = w,
        .marginPx         = 0.0F,
        .baselineOffsetPx = 0.0F,
        .boxId            = 0,
        .kind             = LayoutEntryKind::TextLine,
        .placement        = 0,
        .flags            = 1,
    });
  }

  const auto filade = Layoutfilade::buildFromEntries(entries);
  EXPECT_EQ(filade.size(), kNumLines);
  EXPECT_FALSE(filade.empty());

  // Ruling R9 Verification
  EXPECT_TRUE(filade.verifyAgainstLinearScan(entries));

  // Point check in the middle
  const auto hitIdx = filade.findEntryByIndex(500);
  ASSERT_TRUE(hitIdx.has_value());
  EXPECT_EQ(hitIdx->entryIndex, 500U);

  const auto hitY = filade.findEntryAtY(hitIdx->startYPx + 2.0F);
  ASSERT_TRUE(hitY.has_value());
  EXPECT_EQ(hitY->entryIndex, 500U);

  const auto hitByte = filade.findEntryAtByte(hitIdx->startByte + 5);
  ASSERT_TRUE(hitByte.has_value());
  EXPECT_EQ(hitByte->entryIndex, 500U);
  EXPECT_EQ(hitByte->intraByteOffset, 5U);
}

TEST(LayoutfiladeTest, RichMediaBoxHandling) {
  // Construct a document with text lines and an embedded 4K image block box
  std::vector<LayoutEntry> entries;

  // 1. Heading
  entries.push_back(LayoutEntry{
      .byteLength = 25,
      .heightPx   = 32.0F,
      .widthPx    = 300.0F,
      .kind       = LayoutEntryKind::TextLine,
  });

  // 2. Paragraph (3 lines)
  for (int i = 0; i < 3; ++i) {
    entries.push_back(LayoutEntry{
        .byteLength = 80,
        .heightPx   = 18.0F,
        .widthPx    = 600.0F,
        .kind       = LayoutEntryKind::TextLine,
    });
  }

  // 3. 4K Image Block Box (U+FFFC: 3 bytes, 3840x2160 scaled to 800x450 + 20px
  // margin)
  entries.push_back(LayoutEntry{
      .byteLength = 3, // 3-byte U+FFFC sequence
      .heightPx   = 450.0F,
      .widthPx    = 800.0F,
      .marginPx   = 20.0F,
      .boxId      = 42,
      .kind       = LayoutEntryKind::BlockBox,
      .placement  = static_cast<std::uint8_t>(gleditor::BoxPlacement::Block),
  });

  // 4. Following text paragraph (2 lines)
  for (int i = 0; i < 2; ++i) {
    entries.push_back(LayoutEntry{
        .byteLength = 75,
        .heightPx   = 18.0F,
        .widthPx    = 580.0F,
        .kind       = LayoutEntryKind::TextLine,
    });
  }

  const auto filade = Layoutfilade::buildFromEntries(entries);
  EXPECT_EQ(filade.size(), 7U);

  const auto &m = filade.metrics();
  // Total bytes: 25 + 240 + 3 + 150 = 418 bytes
  EXPECT_EQ(m.totalBytes, 418U);
  // Total height: 32 + (3 * 18) + (450 + 40) + (2 * 18) = 32 + 54 + 490 + 36 =
  // 612 px
  EXPECT_FLOAT_EQ(m.totalHeightPx, 612.0F);
  EXPECT_EQ(m.mediaBoxCount, 1U);
  EXPECT_FLOAT_EQ(m.maxLineWidthPx, 840.0F); // 800 + 40 margin

  // Stabbing into the image box (Y between 86.0 and 576.0)
  const auto hitMedia = filade.findEntryAtY(250.0F);
  ASSERT_TRUE(hitMedia.has_value());
  EXPECT_EQ(hitMedia->entryIndex, 4U);
  EXPECT_TRUE(hitMedia->entry.isMediaBox());
  EXPECT_EQ(hitMedia->entry.boxId, 42U);

  // Stabbing after image box
  const auto hitAfter = filade.findEntryAtY(590.0F);
  ASSERT_TRUE(hitAfter.has_value());
  EXPECT_EQ(hitAfter->entryIndex, 5U);
  EXPECT_FALSE(hitAfter->entry.isMediaBox());

  // R9 verification on mixed media
  EXPECT_TRUE(filade.verifyAgainstLinearScan(entries));
}

TEST(LayoutfiladeTest, FromTextAndBoxes) {
  const std::string text = "Title Line\n"
                           "First paragraph line one.\n"
                           "First paragraph line two.\n"
                           "\xEF\xBF\xBC\n" // U+FFFC media anchor
                           "Concluding remarks.\n";

  std::vector<gleditor::LayoutBox> boxes;
  boxes.push_back(gleditor::LayoutBox{
      .anchor    = static_cast<std::uint32_t>(text.find("\xEF\xBF\xBC")),
      .widthPx   = 640.0F,
      .heightPx  = 360.0F,
      .marginPx  = 16.0F,
      .placement = gleditor::BoxPlacement::Block,
      .id        = 101,
  });

  const auto filade = Layoutfilade::fromTextAndBoxes(text, 20.0F, 8.0F, boxes);
  EXPECT_FALSE(filade.empty());
  EXPECT_EQ(filade.metrics().mediaBoxCount, 1U);

  // Look up the media box by its anchor byte offset
  const auto hit = filade.findEntryAtByte(boxes[0].anchor);
  ASSERT_TRUE(hit.has_value());
  EXPECT_TRUE(hit->entry.isMediaBox());
  EXPECT_EQ(hit->entry.boxId, 101U);
  EXPECT_FLOAT_EQ(hit->entry.heightPx, 360.0F);
}

TEST(LayoutfiladeTest, VisibleRangeViewportCulling) {
  constexpr std::size_t kCount = 500;
  std::vector<LayoutEntry> entries;
  for (std::size_t i = 0; i < kCount; ++i) {
    bool isMedia = (i == 100 || i == 200 || i == 300);
    entries.push_back(LayoutEntry{
        .byteLength = isMedia ? 3U : 40U,
        .heightPx   = isMedia ? 200.0F : 20.0F,
        .widthPx    = isMedia ? 400.0F : 300.0F,
        .marginPx   = isMedia ? 10.0F : 0.0F,
        .boxId      = isMedia ? static_cast<std::uint32_t>(i) : 0U,
        .kind = isMedia ? LayoutEntryKind::BlockBox : LayoutEntryKind::TextLine,
    });
  }

  const auto filade = Layoutfilade::buildFromEntries(entries);

  // Query a viewport window around entry 100
  // Entry 0..99 = 100 * 20 = 2000 px.
  // Entry 100 = 2000 px .. 2220 px.
  // Viewport [1900, 2400]
  const auto range = filade.visibleRange(1900.0F, 2400.0F);
  EXPECT_LE(range.firstEntryIndex, 95U);
  EXPECT_GE(range.lastEntryIndex, 105U);
  EXPECT_EQ(range.mediaBoxCount, 1U);
  ASSERT_EQ(range.visibleMediaBoxIndices.size(), 1U);
  EXPECT_EQ(range.visibleMediaBoxIndices[0], 100U);
}

TEST(LayoutfiladeTest, IncrementalInvalidationAndResize) {
  constexpr std::size_t kLines = 300;
  std::vector<LayoutEntry> entries;
  for (std::size_t i = 0; i < kLines; ++i) {
    entries.push_back(LayoutEntry{
        .byteLength = 50,
        .heightPx   = 20.0F,
        .widthPx    = 400.0F,
        .kind       = LayoutEntryKind::TextLine,
    });
  }

  auto filade                   = Layoutfilade::buildFromEntries(entries);
  const auto initialTotalHeight = filade.metrics().totalHeightPx;
  EXPECT_FLOAT_EQ(initialTotalHeight, 300.0F * 20.0F);

  // Dynamically resize line 150 from 20px to 80px (e.g. font size increase or
  // expansion)
  auto modifiedEntry     = entries[150];
  modifiedEntry.heightPx = 80.0F;
  entries[150]           = modifiedEntry;

  EXPECT_TRUE(filade.updateEntry(150, modifiedEntry));

  // Root metrics should have updated by +60px
  EXPECT_FLOAT_EQ(filade.metrics().totalHeightPx, initialTotalHeight + 60.0F);

  // Downstream entries must still be accurately looked up
  const auto hitAfter = filade.findEntryByIndex(200);
  ASSERT_TRUE(hitAfter.has_value());
  EXPECT_FLOAT_EQ(hitAfter->startYPx, (200.0F * 20.0F) + 60.0F);

  // Verify against full linear scan
  EXPECT_TRUE(filade.verifyAgainstLinearScan(entries));
}

} // namespace
