/**
 * @file doc_gap_test.cpp
 * @brief Doc::pages can hold a gap -- an unbuilt slot before a built one --
 * without corrupting picking, reflow, or the accessibility tree.
 *
 * design/priority-page-building.md's Stage 2 made the container ready to
 * hold one; nothing production yet leaves one (Stage 3 is what will), so
 * these tests build one directly through Doc's private page-placement
 * machinery. DocGapTest is declared a friend of Doc (see doc.hpp) expressly
 * for this file, since no public API can reach it yet.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <ranges>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <glm/ext/matrix_float4x4.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>

#include <gleditor/a11y/documents.hpp>
#include <gleditor/a11y/tree.hpp>
#include <gleditor/caret.hpp>
#include <gleditor/doc.hpp>
#include <gleditor/enfilade/layoutfilade.hpp>
#include <gleditor/render/types.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/renderer.hpp>
#include <gleditor/state.hpp>
#include <gleditor/text/diagnostics.hpp>
#include <gleditor/text/font.hpp>
#include <gleditor/text_source.hpp>

#include "mocks/device.hpp"

using gleditor::MemoryTextSource;
using testing::NiceMock;
using testing::Return;

namespace {

/// Enough text for several dozen Letter-geometry pages -- comfortably past
/// the handful of indices these tests reach into.
std::string manyPagesOfText() {
  const std::string paragraph =
      "The quick brown fox jumps over the lazy dog. Pack my box with five "
      "dozen liquor jugs. How vexingly quick daft zebras jump!\n\n";
  std::string out;
  out.reserve(64U * 1024U);
  while (out.size() < 64U * 1024U) {
    out += paragraph;
  }
  return out;
}

class GeometryMediaSource : public MemoryTextSource {
public:
  GeometryMediaSource()
      : MemoryTextSource("before\n\n\uFFFC\n\nsecond\n\uFFFC\n\nafter\n",
                         "retained-media-geometry") {}
  std::vector<gleditor::LayoutBox> layoutBoxes() const override {
    const auto contents = text();
    const auto first    = contents.find("\uFFFC");
    const auto second   = contents.find("\uFFFC", first + 3);
    return {{.anchor    = static_cast<std::uint32_t>(first),
             .widthPx   = 220,
             .heightPx  = 100,
             .marginPx  = 8,
             .placement = gleditor::BoxPlacement::Block,
             .id        = 1},
            {.anchor    = static_cast<std::uint32_t>(second),
             .widthPx   = 180,
             .heightPx  = 70,
             .marginPx  = 8,
             .placement = gleditor::BoxPlacement::Block,
             .id        = 2}};
  }
};

class ForcedBreakMemorySource : public MemoryTextSource {
public:
  ForcedBreakMemorySource(std::string text, const std::uint32_t breakAt)
      : MemoryTextSource(std::move(text), "forced-break-reflow"),
        breaks{breakAt} {}

  [[nodiscard]] std::vector<std::uint32_t> forcedBreaks() const override {
    return breaks;
  }

private:
  std::vector<std::uint32_t> breaks;
};

} // namespace

class DocGapTest : public testing::Test {
protected:
  std::unique_ptr<NiceMock<MockRenderDevice>> device;
  std::unique_ptr<RenderState> state;
  AppStateRef appState;
  RendererRef renderer;
  std::shared_ptr<Doc> doc;

  void SetUp() override {
    device = std::make_unique<NiceMock<MockRenderDevice>>();
    ON_CALL(*device, textureLimits())
        .WillByDefault(Return(render::TextureLimits{2048, 10}));
    ON_CALL(*device,
            createTextureArray(testing::_, testing::_, testing::_, testing::_))
        .WillByDefault(Return(render::TextureHandle{1}));
    ON_CALL(*device, createBuffer(testing::_, testing::_))
        .WillByDefault(Return(render::BufferHandle{1}));
    ON_CALL(*device, resizeBuffer(testing::_, testing::_))
        .WillByDefault(Return(render::BufferHandle{1}));

    state = std::make_unique<RenderState>(device.get());

    appState                  = std::make_shared<AppState>();
    appState->defaultFontName = "Monospace 10";
    renderer = Renderer::create(appState, render::Backend::OpenGL);

    doc = Doc::create(renderer, device.get(), glm::mat4(1.0F),
                      MemoryTextSource(manyPagesOfText(), "gap-test"));
  }

  /// Build page @p index directly, without touching any page before it --
  /// exactly the gap nothing production leaves today. Requires makePages()
  /// to have already shaped at least that far.
  gleditor::enfilade::LayoutHit buildPageDirectly(const std::size_t index) {
    doc->refreshPageIndexFilade();
    const auto hit = doc->pageIndexFilade.findEntryByIndex(index);
    EXPECT_TRUE(hit.has_value()) << "page " << index << " was never shaped";
    auto shaping = doc->layoutFrom(hit->startByte);
    doc->placePageAt(*state, index, std::move(shaping), hit->startByte,
                     glm::mat4(1.0F));
    return *hit;
  }

  /// TEST_F's generated fixture subclass does not inherit DocGapTest's own
  /// friendship with Doc (friend access is not inherited in C++), so a test
  /// body cannot call doc->reflowFrom() directly -- this thin wrapper, a
  /// genuine DocGapTest member, is what makes that reachable.
  void callReflowFrom(const std::size_t firstPage, const std::uint32_t at,
                      const std::int32_t delta,
                      const std::vector<int> &oldStarts,
                      const std::uint32_t oldConsumed) {
    doc->reflowFrom(*state, firstPage, at, delta, oldStarts, oldConsumed);
  }

  void shiftBuiltPageBase(const std::size_t index, const std::int32_t bytes) {
    ASSERT_TRUE(doc->pages[index]);
    doc->pages[index]->shiftBaseOffset(bytes);
  }

  /// Invoke the real reflow synchronously without needing an application event
  /// loop in this geometry-only fixture.
  void prependAndReflow(const std::string &prefix) {
    const auto oldStarts = doc->lineBreaksAround(0);
    ASSERT_TRUE(doc->page(0));
    const auto oldConsumed = doc->page(0)->textLength();
    doc->text.insert(0, prefix);
    doc->reflowFrom(*state, 0, 0, static_cast<std::int32_t>(prefix.size()),
                    oldStarts, oldConsumed);
  }

  void appendAndReflow(const std::string &suffix) {
    const auto at        = static_cast<std::uint32_t>(doc->text.size());
    const auto oldStarts = doc->lineBreaksAround(at);
    ASSERT_TRUE(doc->page(0));
    const auto oldConsumed = doc->page(0)->textLength();
    doc->text += suffix;
    doc->reflowFrom(*state, 0, at, static_cast<std::int32_t>(suffix.size()),
                    oldStarts, oldConsumed);
  }

  void insertAndReflow(const std::size_t pageIndex, const std::uint32_t at,
                       const std::string &inserted) {
    const auto oldStarts = doc->lineBreaksAround(at);
    ASSERT_TRUE(doc->page(pageIndex));
    const auto oldConsumed = doc->page(pageIndex)->textLength();
    doc->text.insert(at, inserted);
    callReflowFrom(pageIndex, at, static_cast<std::int32_t>(inserted.size()),
                   oldStarts, oldConsumed);
  }

  void eraseAndReflow(const std::size_t pageIndex, const std::uint32_t at,
                      const std::uint32_t bytes) {
    const auto oldStarts = doc->lineBreaksAround(at);
    ASSERT_TRUE(doc->page(pageIndex));
    const auto oldConsumed = doc->page(pageIndex)->textLength();
    doc->text.erase(at, bytes);
    callReflowFrom(pageIndex, at, -static_cast<std::int32_t>(bytes), oldStarts,
                   oldConsumed);
  }
};

TEST_F(DocGapTest, PagesCanHoldAGapBeforeTheFirstBuiltPage) {
  doc->makePages();
  buildPageDirectly(5);

  EXPECT_FALSE((doc->page(0)).has_value()) << "page 0 should still be a gap";
  EXPECT_FALSE((doc->page(4)).has_value()) << "page 4 should still be a gap";
  ASSERT_TRUE((doc->page(5)).has_value()) << "page 5 was built directly";
  EXPECT_EQ(doc->builtPageCount(), 1U)
      << "exactly one page was placed, regardless of its index";
}

TEST_F(DocGapTest, PickingResolvesCorrectlyForAPageBuiltOutOfOrder) {
  doc->makePages();
  const auto hit = buildPageDirectly(5);

  // A background click (not a glyph) resolves through the page it names --
  // offsetForPick() reads that straight off Doc::page(), so this is exactly
  // what would go wrong if picking still assumed pages fill in strictly from
  // index 0. The exact byte it lands on within the page is a hit-testing
  // detail (offsetForPagePoint() resolves the nearest caret to an x/y
  // position); what this guards is that it is resolved against page 5's own
  // text at all, rather than page 0's or nothing.
  const render::PickingTag tag{
      .kind         = render::tagKindPage,
      .docIndex     = doc->documentIndex(),
      .pageIndex    = 5,
      .clusterIndex = 0,
      .fraction     = 0.0F,
  };
  const auto resolved = doc->offsetForPick(tag);
  ASSERT_TRUE(resolved.has_value());
  EXPECT_GE(*resolved, hit.startByte);
  EXPECT_LE(*resolved, hit.startByte + hit.entry.byteLength);
}

TEST_F(DocGapTest, ReflowFromsGuardFillsThePageItNeeds) {
  doc->makePages();
  // Build every page around index 2 except 2 itself, so reflowFrom(state, 2,
  // ...) -- which reads pages[2]'s current offset before doing anything else
  // -- has to fill a real gap before it can proceed.
  buildPageDirectly(0);
  buildPageDirectly(1);
  buildPageDirectly(3);
  ASSERT_FALSE((doc->page(2)).has_value()) << "page 2 must still be a gap";

  // No text is actually changed here (delta 0): this isolates
  // ensurePagesBuiltThrough()'s guard from the rest of reflowFrom()'s own
  // re-sync logic, which tests/lib elsewhere already covers on dense pages.
  const auto page2Hit = doc->pageIndexFilade.findEntryByIndex(2);
  ASSERT_TRUE(page2Hit.has_value());
  callReflowFrom(2, page2Hit->startByte, 0, std::vector<int>{},
                 page2Hit->entry.byteLength);

  EXPECT_TRUE((doc->page(2)).has_value())
      << "reflowFrom()'s guard should have filled page 2 before using it";
}

TEST_F(DocGapTest, AccessibilityStillDescribesADocumentWithAGap) {
  doc->makePages();
  buildPageDirectly(5);
  state->docs.push_back(doc);

  // boundsOf() (src/a11y/documents.cpp) walks every index up to numPages()
  // and skips whatever page(index) answers nullptr for -- this is what
  // proves that tolerance still holds now that a gap is a real, reachable
  // state rather than merely an unbuilt tail.
  gleditor::a11y::DocumentsSource source;
  EXPECT_NO_FATAL_FAILURE(
      source.observe(*state, nullptr, glm::mat4(1.0F), 800, 600));

  gleditor::a11y::Tree tree;
  gleditor::a11y::Builder builder(tree, 0);
  EXPECT_NO_FATAL_FAILURE(source.describe(builder));

  // The description is built from the document's text, not from which pages
  // happen to be built, so the whole document is still there to read.
  EXPECT_FALSE(tree.nodes.empty());
}

TEST_F(DocGapTest, TextBandsClipAtPageBreaksWithoutReshaping) {
  doc->makePages();
  const auto first      = buildPageDirectly(2);
  const auto second     = buildPageDirectly(3);
  const auto firstPage  = doc->page(2);
  const auto secondPage = doc->page(3);
  ASSERT_TRUE(firstPage);
  ASSERT_TRUE(secondPage);
  const auto start      = first.startByte + first.entry.byteLength - 50;
  const auto end        = second.startByte + 50;
  const auto startCaret = firstPage->caretGeometry(start);
  const auto endCaret   = secondPage->caretGeometry(end - 1);
  ASSERT_TRUE(startCaret);
  ASSERT_TRUE(endCaret);

  gleditor::text::ShapingStatsScope shaping;
  const auto tail = firstPage->textBandFor(start, end);
  const auto head = secondPage->textBandFor(start, end);
  ASSERT_TRUE(tail);
  ASSERT_TRUE(head);
  EXPECT_FLOAT_EQ(tail->top, startCaret->y + startCaret->height / 2);
  EXPECT_FLOAT_EQ(head->bottom, endCaret->y - endCaret->height / 2);
  EXPECT_FALSE(firstPage->textBandFor(second.startByte, end));
  EXPECT_FALSE(secondPage->textBandFor(start, second.startByte));
  EXPECT_FALSE(secondPage->textBandFor(end, end));
  for (int repeat = 0; repeat < 10; ++repeat) {
    EXPECT_TRUE(firstPage->textBandFor(start, end));
    EXPECT_TRUE(secondPage->textBandFor(start, end));
  }
  EXPECT_EQ(shaping.stats().layoutCalls, 0U);
  EXPECT_EQ(shaping.stats().harfbuzzCalls, 0U);
}

TEST_F(DocGapTest, InteriorTextBandDoesNotNeedTheStartPageBuilt) {
  doc->makePages();
  const auto interior = buildPageDirectly(5);
  ASSERT_FALSE(doc->page(0));
  const auto page = doc->page(5);
  ASSERT_TRUE(page);
  const auto end      = interior.startByte + interior.entry.byteLength;
  const auto exact    = page->textBandFor(interior.startByte, end);
  const auto covering = page->textBandFor(0, end + 50);
  ASSERT_TRUE(exact);
  ASSERT_TRUE(covering);
  EXPECT_FLOAT_EQ(exact->top, covering->top);
  EXPECT_FLOAT_EQ(exact->bottom, covering->bottom);
  EXPECT_GT(covering->top, covering->bottom);
  EXPECT_EQ(doc->builtPageCount(), 1U);
}

TEST_F(DocGapTest, TextBoundsUnionWideIntermediateLinesWithoutReshaping) {
  const std::string middle(24, 'M');
  const std::string contents = "x\n" + middle + "\nx\n";
  doc = Doc::create(renderer, device.get(), glm::mat4(1.0F),
                    MemoryTextSource(contents, "retained-text-bounds"));
  doc->makePages();
  buildPageDirectly(0);
  const auto page = doc->page(0);
  ASSERT_TRUE(page);

  gleditor::text::ShapingStatsScope shaping;
  const auto end    = static_cast<std::uint32_t>(contents.size());
  const auto bounds = page->textBoundsFor(0, end);
  ASSERT_TRUE(bounds);
  const auto band = page->textBandFor(0, end);
  ASSERT_TRUE(band);
  EXPECT_FLOAT_EQ(bounds->band.top, band->top);
  EXPECT_FLOAT_EQ(bounds->band.bottom, band->bottom);
  EXPECT_FLOAT_EQ(bounds->band.lineHeight, band->lineHeight);

  float expectedLeft     = std::numeric_limits<float>::infinity();
  float expectedRight    = -std::numeric_limits<float>::infinity();
  float middleRight      = -std::numeric_limits<float>::infinity();
  float outerRight       = -std::numeric_limits<float>::infinity();
  const auto middleStart = std::uint32_t{2};
  const auto middleEnd =
      middleStart + static_cast<std::uint32_t>(middle.size());
  for (const auto &cluster : page->clusterBoxes()) {
    if (cluster.byteStart >= contents.size()) continue;
    const float width = static_cast<float>((cluster.quad >> 20U) & 4095U);
    const float left  = cluster.quadCentre[0] - width / 2;
    const float right = cluster.quadCentre[0] + width / 2;
    expectedLeft      = std::min(expectedLeft, left);
    expectedRight     = std::max(expectedRight, right);
    if (cluster.byteStart >= middleStart && cluster.byteStart < middleEnd)
      middleRight = std::max(middleRight, right);
    else
      outerRight = std::max(outerRight, right);
  }
  EXPECT_FLOAT_EQ(bounds->left, expectedLeft);
  EXPECT_FLOAT_EQ(bounds->right, expectedRight);
  EXPECT_FLOAT_EQ(bounds->right, middleRight);
  EXPECT_GT(middleRight, outerRight);

  EXPECT_FALSE(page->textBoundsFor(10, 10));
  EXPECT_FALSE(page->textBoundsFor(end, end + 1));
  EXPECT_EQ(shaping.stats().layoutCalls, 0U);
  EXPECT_EQ(shaping.stats().harfbuzzCalls, 0U);
}

TEST_F(DocGapTest, TextBoundsClipToAnInteriorBuiltPage) {
  doc->makePages();
  const auto hit  = buildPageDirectly(5);
  const auto page = doc->page(5);
  ASSERT_TRUE(page);
  const auto pageEnd = hit.startByte + page->textLength();

  gleditor::text::ShapingStatsScope shaping;
  const auto exact   = page->textBoundsFor(hit.startByte, pageEnd);
  const auto clipped = page->textBoundsFor(hit.startByte - 50, pageEnd + 50);
  ASSERT_TRUE(exact);
  ASSERT_TRUE(clipped);
  EXPECT_FLOAT_EQ(clipped->left, exact->left);
  EXPECT_FLOAT_EQ(clipped->right, exact->right);
  EXPECT_FLOAT_EQ(clipped->band.top, exact->band.top);
  EXPECT_FLOAT_EQ(clipped->band.bottom, exact->band.bottom);
  EXPECT_EQ(doc->builtPageCount(), 1U);
  EXPECT_FALSE(doc->page(0));
  EXPECT_EQ(shaping.stats().layoutCalls, 0U);
  EXPECT_EQ(shaping.stats().harfbuzzCalls, 0U);
}

TEST_F(DocGapTest, TextBoundsKeepPartialLigatureEndpointFractions) {
  appState->defaultFontName  = "DejaVu Serif 18";
  const std::string contents = "office ffi";
  doc = Doc::create(renderer, device.get(), glm::mat4(1.0F),
                    MemoryTextSource(contents, "partial-ligature-bounds"));
  doc->makePages();
  buildPageDirectly(0);
  const auto page = doc->page(0);
  ASSERT_TRUE(page);

  const auto cluster =
      std::ranges::find_if(page->clusterBoxes(), [&](const auto &box) {
        if (box.charCount < 2) return false;
        const auto text = contents.substr(box.byteStart, box.byteLength);
        return text.find("ff") != std::string::npos ||
               text.find("fi") != std::string::npos;
      });
  if (cluster == page->clusterBoxes().end()) {
    GTEST_SKIP() << "font shaping produced no multi-character ligature";
  }

  const auto start     = cluster->byteStart;
  const auto end       = start + 1;
  const auto highlight = page->highlightFor(start, end, 0);
  ASSERT_TRUE(highlight);
  ASSERT_EQ(highlight->firstCluster, highlight->lastCluster);
  ASSERT_GT(highlight->endFraction, highlight->startFraction);
  ASSERT_LT(highlight->endFraction, 1.F);

  const float width     = static_cast<float>((cluster->quad >> 20U) & 4095U);
  const float fullLeft  = cluster->quadCentre[0] - width / 2;
  const float fullRight = cluster->quadCentre[0] + width / 2;
  const float expectedLeft =
      std::lerp(fullLeft, fullRight, highlight->startFraction);
  const float expectedRight =
      std::lerp(fullLeft, fullRight, highlight->endFraction);

  gleditor::text::ShapingStatsScope shaping;
  const auto bounds = page->textBoundsFor(start, end);
  ASSERT_TRUE(bounds);
  EXPECT_FLOAT_EQ(bounds->left, expectedLeft);
  EXPECT_FLOAT_EQ(bounds->right, expectedRight);
  EXPECT_LT(bounds->right, fullRight);
  EXPECT_EQ(shaping.stats().layoutCalls, 0U);
  EXPECT_EQ(shaping.stats().harfbuzzCalls, 0U);
}

TEST_F(DocGapTest, FacingHighlightContourUsesUploadedQuads) {
  doc->makePages();
  const auto hit  = buildPageDirectly(0);
  const auto page = doc->page(0);
  ASSERT_TRUE(page);
  const auto end   = hit.startByte + page->textLength();
  const auto range = page->highlightFor(hit.startByte, end, 0);
  ASSERT_TRUE(range);
  const auto at = page->rangeTerminal(hit.startByte, end, true, true, 0);
  ASSERT_TRUE(at);
  const auto boxes  = page->clusterBoxes();
  const auto &first = boxes[range->firstCluster];
  const float top   = first.quadCentre[1] + ((first.quad >> 8U) & 4095U) / 2.0F;
  float right       = first.quadCentre[0];
  for (const auto &box : boxes) {
    const float height = (box.quad >> 8U) & 4095U;
    if (box.quadCentre[1] + height / 2 == top)
      right = std::max(right,
                       box.quadCentre[0] + ((box.quad >> 20U) & 4095U) / 2.0F);
  }
  EXPECT_FLOAT_EQ(at->point.x, right);
  EXPECT_FLOAT_EQ(at->point.y, top);
  EXPECT_FALSE(page->rangeTerminal(end, end, true, true, 0));
  EXPECT_FALSE(page->rangeTerminal(end, end + 10, false, true, 0));
  EXPECT_FALSE(page->rangeTerminal(hit.startByte, end, true, true, -1));
}

TEST_F(DocGapTest, PlacedPageBasisMatchesAnchorsAndAnimatedTranslation) {
  auto placed = glm::translate(glm::mat4(1.0F), glm::vec3(7, -11, 3));
  placed      = glm::rotate(placed, glm::radians(27.0F), glm::vec3(0, 0, 1));
  placed      = glm::rotate(placed, glm::radians(19.0F), glm::vec3(0, 1, 0));
  doc         = Doc::create(renderer, device.get(), placed,
                            MemoryTextSource(manyPagesOfText(), "placed-page"));
  doc->makePages();
  const auto hit  = buildPageDirectly(2);
  const auto page = doc->page(2);
  ASSERT_TRUE(page);
  const auto anchor = doc->anchorFor(hit.startByte + 20);
  ASSERT_TRUE(anchor);

  const auto checkFrame = [&](const glm::mat4 &expected) {
    const auto frame = doc->pageFrame(2);
    const auto point = doc->worldPoint(*anchor);
    ASSERT_TRUE(frame);
    ASSERT_TRUE(point);
    const auto expectedFrame = expected * page->getModel();
    for (int column = 0; column < 4; ++column)
      for (int row = 0; row < 4; ++row)
        EXPECT_NEAR(frame->localToWorld[column][row],
                    expectedFrame[column][row], 1e-5F);
    const auto expectedPoint =
        glm::vec3(expectedFrame * glm::vec4(anchor->x, anchor->y, 0, 1));
    EXPECT_LT(glm::distance(*point, expectedPoint), 1e-4F);
    std::vector<render::GlyphBatch> batches;
    DrawStats stats;
    doc->collect(batches, glm::mat4(1.0F), DrawBudget{.cull = false}, stats);
    ASSERT_EQ(batches.size(), 1U);
    for (int column = 0; column < 4; ++column)
      for (int row = 0; row < 4; ++row)
        EXPECT_NEAR(batches[0].uniforms.mvp[column * 4 + row],
                    expectedFrame[column][row], 1e-5F);
  };
  checkFrame(placed);

  ch::Timeline timeline;
  doc->animateMoveTo(timeline, glm::vec3(-5, 8, 12), 1.0);
  timeline.step(0.25);
  auto moving = placed;
  moving[3]   = glm::vec4(doc->currentPosition(), 1.0F);
  EXPECT_GT(glm::distance(glm::vec3(placed[3]), glm::vec3(moving[3])), 0.F);
  EXPECT_GT(glm::distance(glm::vec3(-5, 8, 12), glm::vec3(moving[3])), 0.F);
  checkFrame(moving);
}

TEST_F(DocGapTest, LiveDocumentPaletteReachesDetailedAndCoarseBatches) {
  doc->makePages();
  buildPageDirectly(0);
  DrawBudget budget{.cull = false};
  std::vector<render::GlyphBatch> batches;
  DrawStats stats;
  doc->collect(batches, glm::mat4(1.0F), budget, stats);
  ASSERT_EQ(batches.size(), 1U);
  EXPECT_EQ(batches[0].uniforms.paletteEnabled, 0U);
  const auto backing        = batches[0].vertices;
  const auto detailedOffset = batches[0].vertexByteOffset;

  budget.documentPalette =
      DocumentPalette{.paper = 0x181C24FFU, .ink = 0xE8EDF4FFU};
  batches.clear();
  doc->collect(batches, glm::mat4(1.0F), budget, stats);
  ASSERT_EQ(batches.size(), 1U);
  EXPECT_EQ(batches[0].vertices, backing);
  EXPECT_EQ(batches[0].vertexByteOffset, detailedOffset);
  EXPECT_EQ(batches[0].uniforms.paletteEnabled, 1U);
  EXPECT_EQ(batches[0].uniforms.paperColour, 0x181C24FFU);
  EXPECT_EQ(batches[0].uniforms.inkColour, 0xE8EDF4FFU);

  budget.coarseBelow = 10000.F;
  batches.clear();
  doc->collect(batches, glm::mat4(1.0F), budget, stats);
  ASSERT_EQ(batches.size(), 1U);
  EXPECT_EQ(stats.coarse, 1U);
  EXPECT_NE(batches[0].vertexByteOffset, detailedOffset);
  EXPECT_EQ(batches[0].uniforms.paletteEnabled, 1U);
  EXPECT_EQ(batches[0].uniforms.paperColour, 0x181C24FFU);

  budget.documentPalette.reset();
  batches.clear();
  doc->collect(batches, glm::mat4(1.0F), budget, stats);
  ASSERT_EQ(batches.size(), 1U);
  EXPECT_EQ(batches[0].uniforms.paletteEnabled, 0U);
}

TEST_F(DocGapTest, RetainedCaretsMatchNativeLigatureAndBidiPlacement) {
  struct Fixture {
    std::string font;
    std::string contents;
    bool rtl{};
  };
  const std::vector<Fixture> fixtures{
      {"DejaVu Serif 18", "office ffi e\u0301\n\nאבג דהו\nسَلَام\n", false},
      {"DejaVu Sans 18", "אבג דהו", true}};
  for (const auto &fixture : fixtures) {
    SCOPED_TRACE(fixture.font);
    appState->defaultFontName = fixture.font;
    const auto &contents      = fixture.contents;
    doc = Doc::create(renderer, device.get(), glm::mat4(1),
                      MemoryTextSource(contents, "caret-native-layout"));
    doc->makePages();
    buildPageDirectly(0);
    const auto page = doc->page(0);
    ASSERT_TRUE(page);
    // The independent reference is the native layout, before any repeated
    // anchor query. It is not the page's retained metadata under test.
    const auto native = page->ensureShaping();
    const auto font   = gleditor::text::FontManager::instance().getFont(
        appState->defaultFontName);
    ASSERT_TRUE(font);
    const auto height = font->metrics().lineHeight;
    std::map<std::uint32_t, CaretGeometry> expected;
    std::set<std::size_t> seen;
    bool hasLigature = false;
    for (const auto &glyph : native.glyphs) {
      ASSERT_LT(glyph.clusterIndex, native.clusters.size());
      if (!seen.insert(glyph.clusterIndex).second) continue;
      const auto &cluster = native.clusters[glyph.clusterIndex];
      const auto clusterText =
          contents.substr(cluster.byteStart, cluster.byteLength);
      if (cluster.charCount > 1 &&
          (clusterText.find("ff") != std::string::npos ||
           clusterText.find("fi") != std::string::npos))
        hasLigature = true;
      const auto top = glyph.lineIndex < native.lines.size()
                           ? native.lines[glyph.lineIndex].top
                           : 0.F;
      const CaretGeometry at{.x = page->leftPixels() + Page::marginPixels +
                                  glyph.clusterLeft + Caret::widthPixels / 2,
                             .y = page->topPixels() -
                                  (Page::marginPixels + top + height / 2),
                             .height = height};
      for (std::uint32_t byte = cluster.byteStart;
           byte < cluster.byteStart + cluster.byteLength; ++byte)
        expected.emplace(byte, at);
    }
    if (fixture.rtl) {
      // Native page shaping guesses one direction from its first strong
      // character. Hebrew must begin this separate page to exercise an actual
      // RTL layout; putting it after Latin gives the whole page LTR direction.
      ASSERT_TRUE(expected.contains(0));
      ASSERT_TRUE(expected.contains(2));
      EXPECT_GT(expected.at(0).x, expected.at(2).x)
          << "fixture must exercise reversed visual placement in its RTL run";
    } else {
      ASSERT_TRUE(hasLigature) << "fixture did not shape any native ligature";
    }
    const gleditor::text::ShapingStatsScope shaping;
    for (int repeat = 0; repeat < 100; ++repeat)
      for (const auto &[byte, reference] : expected) {
        const auto actual = page->caretGeometry(byte);
        ASSERT_TRUE(actual);
        EXPECT_FLOAT_EQ(actual->x, reference.x);
        EXPECT_FLOAT_EQ(actual->y, reference.y);
        EXPECT_FLOAT_EQ(actual->height, reference.height);
      }
    EXPECT_EQ(shaping.stats().layoutCalls, 0U);
    EXPECT_EQ(shaping.stats().harfbuzzCalls, 0U);
    EXPECT_EQ(shaping.stats().fallbackCalls, 0U);
  }
}

TEST_F(DocGapTest, RetainedBlankLineAndMediaGeometrySurvivesByteRenumbering) {
  const GeometryMediaSource source;
  doc = Doc::create(renderer, device.get(), glm::mat4(1), source);
  doc->makePages();
  buildPageDirectly(0);
  const auto page = doc->page(0);
  ASSERT_TRUE(page);
  const auto native = page->ensureShaping();
  ASSERT_EQ(native.boxes.size(), 2U);
  std::map<std::uint32_t, BoxGeometry> expectedBoxes;
  for (const auto &box : native.boxes)
    expectedBoxes.emplace(
        box.anchorByteOffset,
        BoxGeometry{.x = page->leftPixels() + Page::marginPixels + box.left,
                    .y = page->topPixels() -
                         (Page::marginPixels + box.top + box.height),
                    .width  = box.width,
                    .height = box.height});
  const auto font = gleditor::text::FontManager::instance().getFont(
      appState->defaultFontName);
  ASSERT_TRUE(font);
  const auto height = font->metrics().lineHeight;
  std::map<std::uint32_t, CaretGeometry> blankCarets;
  for (const auto &line : native.lines) {
    if (line.byteLength == 0) continue;
    const auto hasGlyph =
        std::ranges::any_of(native.glyphs, [&](const auto &glyph) {
          const auto &cluster = native.clusters[glyph.clusterIndex];
          return cluster.byteStart <= line.byteStart &&
                 line.byteStart < cluster.byteStart + cluster.byteLength;
        });
    if (!hasGlyph)
      blankCarets.emplace(
          line.byteStart,
          CaretGeometry{.x = page->leftPixels() + Page::marginPixels +
                             line.left + line.barWidth + Caret::widthPixels / 2,
                        .y = page->topPixels() -
                             (Page::marginPixels + line.top + height / 2),
                        .height = height});
  }
  ASSERT_GE(blankCarets.size(), 2U);
  const auto originalEnd = page->textLength();
  const auto finalCaret  = page->caretGeometry(originalEnd);
  ASSERT_TRUE(finalCaret);
  const auto verify = [&](const std::uint32_t shift) {
    for (const auto &[offset, reference] : expectedBoxes) {
      const auto actual = page->boxGeometry(offset + shift);
      ASSERT_TRUE(actual);
      EXPECT_FLOAT_EQ(actual->x, reference.x);
      EXPECT_FLOAT_EQ(actual->y, reference.y);
      EXPECT_FLOAT_EQ(actual->width, reference.width);
      EXPECT_FLOAT_EQ(actual->height, reference.height);
    }
    for (const auto &[offset, reference] : blankCarets) {
      const auto actual = page->caretGeometry(offset + shift);
      ASSERT_TRUE(actual);
      EXPECT_FLOAT_EQ(actual->x, reference.x);
      EXPECT_FLOAT_EQ(actual->y, reference.y);
      EXPECT_FLOAT_EQ(actual->height, reference.height);
    }
    const auto end = page->caretGeometry(originalEnd + shift);
    ASSERT_TRUE(end);
    EXPECT_FLOAT_EQ(end->x, finalCaret->x);
    EXPECT_FLOAT_EQ(end->y, finalCaret->y);
    EXPECT_FALSE(page->caretGeometry(originalEnd + shift + 1));
    EXPECT_FALSE(page->boxGeometry(shift));
  };
  const gleditor::text::ShapingStatsScope shaping;
  for (int repeat = 0; repeat < 100; ++repeat) verify(0);
  shiftBuiltPageBase(0, 1024);
  for (int repeat = 0; repeat < 100; ++repeat) verify(1024);
  EXPECT_FALSE(page->caretGeometry(0));
  EXPECT_FALSE(page->boxGeometry(native.boxes[0].anchorByteOffset));
  EXPECT_EQ(shaping.stats().layoutCalls, 0U);
  EXPECT_EQ(shaping.stats().harfbuzzCalls, 0U);
}

TEST_F(DocGapTest, ReflowReplacesRetainedCaretGeometry) {
  doc = Doc::create(
      renderer, device.get(), glm::mat4(1),
      MemoryTextSource("alpha beta\ngamma delta\n", "caret-reflow"));
  doc->makePages();
  buildPageDirectly(0);
  const auto before = doc->page(0)->caretGeometry(0);
  ASSERT_TRUE(before);
  prependAndReflow("\n");
  const auto page = doc->page(0);
  ASSERT_TRUE(page);
  const gleditor::text::ShapingStatsScope shaping;
  for (int repeat = 0; repeat < 100; ++repeat) {
    const auto after = page->caretGeometry(1);
    ASSERT_TRUE(after);
    EXPECT_FLOAT_EQ(after->x, before->x);
    EXPECT_NEAR(after->y, before->y - before->height, 1e-4F);
    EXPECT_FLOAT_EQ(after->height, before->height);
    const auto blank = page->caretGeometry(0);
    ASSERT_TRUE(blank);
    EXPECT_FLOAT_EQ(blank->y, before->y);
  }
  EXPECT_EQ(shaping.stats().layoutCalls, 0U);
  EXPECT_EQ(shaping.stats().harfbuzzCalls, 0U);
}

TEST_F(DocGapTest, ReflowIndexesTextAppendedAfterAnAuthoredPageBreak) {
  const std::string first = "The first page ends here.";
  const std::string tail  = "The second page has its own endpoint.";
  doc                     = Doc::create(
      renderer, device.get(), glm::mat4(1.0F),
      ForcedBreakMemorySource(first, static_cast<std::uint32_t>(first.size())));
  doc->makePages();
  ASSERT_EQ(doc->numPages(), 1U);
  buildPageDirectly(0);

  appendAndReflow(tail);

  ASSERT_EQ(doc->numPages(), 2U);
  const auto lastByte = static_cast<std::uint32_t>(doc->contents().size() - 1U);
  const auto firstPage  = doc->pageIndexForOffset(0);
  const auto secondPage = doc->pageIndexForOffset(lastByte);
  ASSERT_TRUE(firstPage);
  ASSERT_TRUE(secondPage);
  EXPECT_EQ(*firstPage, 0U);
  EXPECT_EQ(*secondPage, 1U);

  doc->setPriorityOffsets(std::array{lastByte});
  static_cast<void>(doc->buildPendingPages(*state));
  const auto endpoint = doc->anchorFor(lastByte);
  ASSERT_TRUE(endpoint);
  EXPECT_EQ(endpoint->pageIndex, 1U);

  // Reindexing must also notice changed lengths when the page count stays
  // the same, then drop the final entry when that tail is erased.
  ASSERT_TRUE(doc->page(1));
  const auto tailStart    = static_cast<std::uint32_t>(first.size());
  const auto editAt       = static_cast<std::uint32_t>(doc->contents().size());
  const std::string extra = " more";
  insertAndReflow(1, editAt, extra);
  EXPECT_EQ(doc->numPages(), 2U);
  EXPECT_EQ(doc->pageIndexForOffset(editAt), 1U);
  EXPECT_EQ(doc->pageIndexForOffset(
                static_cast<std::uint32_t>(doc->contents().size() - 1U)),
            1U);

  const auto eraseAt = tailStart;
  const auto eraseBytes =
      static_cast<std::uint32_t>(doc->contents().size()) - eraseAt;
  eraseAndReflow(1, eraseAt, eraseBytes);
  EXPECT_EQ(doc->numPages(), 1U);
  EXPECT_FALSE(doc->page(1));
  EXPECT_EQ(doc->builtPageCount(), 1U);
  EXPECT_EQ(doc->pageIndexForOffset(
                static_cast<std::uint32_t>(doc->contents().size() - 1U)),
            0U);
}

TEST_F(DocGapTest, ReflowKeepsAnUnbuiltPageSlotSparse) {
  doc->makePages();
  ASSERT_GT(doc->numPages(), 2U);
  buildPageDirectly(0);
  const auto firstPageLength = doc->page(0)->textLength();
  const auto oldPageTwo      = buildPageDirectly(2);
  ASSERT_FALSE(doc->page(1));
  ASSERT_TRUE(doc->page(2));
  const auto prefix = doc->contents().substr(0, firstPageLength);

  prependAndReflow(prefix);

  EXPECT_FALSE(doc->page(1));
  EXPECT_FALSE(doc->page(2));
  ASSERT_TRUE(doc->page(3));
  EXPECT_EQ(doc->page(3)->baseOffset(), oldPageTwo.startByte + prefix.size());
  EXPECT_EQ(doc->pageIndexForOffset(static_cast<std::uint32_t>(
                oldPageTwo.startByte + prefix.size())),
            3U);
}
