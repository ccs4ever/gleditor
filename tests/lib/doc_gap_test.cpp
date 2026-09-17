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

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <glm/ext/matrix_float4x4.hpp>

#include <gleditor/a11y/documents.hpp>
#include <gleditor/a11y/tree.hpp>
#include <gleditor/doc.hpp>
#include <gleditor/enfilade/layoutfilade.hpp>
#include <gleditor/render/types.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/renderer.hpp>
#include <gleditor/state.hpp>
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
};

TEST_F(DocGapTest, PagesCanHoldAGapBeforeTheFirstBuiltPage) {
  doc->makePages();
  buildPageDirectly(5);

  EXPECT_EQ(doc->page(0), nullptr) << "page 0 should still be a gap";
  EXPECT_EQ(doc->page(4), nullptr) << "page 4 should still be a gap";
  ASSERT_NE(doc->page(5), nullptr) << "page 5 was built directly";
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
  ASSERT_EQ(doc->page(2), nullptr) << "page 2 must still be a gap";

  // No text is actually changed here (delta 0): this isolates
  // ensurePagesBuiltThrough()'s guard from the rest of reflowFrom()'s own
  // re-sync logic, which tests/lib elsewhere already covers on dense pages.
  const auto page2Hit = doc->pageIndexFilade.findEntryByIndex(2);
  ASSERT_TRUE(page2Hit.has_value());
  callReflowFrom(2, page2Hit->startByte, 0, std::vector<int>{},
                 page2Hit->entry.byteLength);

  EXPECT_NE(doc->page(2), nullptr)
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
