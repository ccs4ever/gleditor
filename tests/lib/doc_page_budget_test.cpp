/**
 * @file doc_page_budget_test.cpp
 * @brief Doc::buildPendingPages() stays within its per-call time budget even
 * when the background shaping thread has produced a large backlog before the
 * render thread ever asks for pages -- the regression covered by
 * design/kjv-load-blocking-regression.md -- and widens that budget when the
 * camera, or a page named by setPriorityOffsets(), is well past what has
 * been built so far (design/priority-page-building.md's Stage 1).
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <glm/ext/matrix_float4x4.hpp>
#include <glm/geometric.hpp>

#include <gleditor/doc.hpp>
#include <gleditor/render/constants.hpp>
#include <gleditor/render_state.hpp>
#include <gleditor/renderer.hpp>
#include <gleditor/state.hpp>
#include <gleditor/text_source.hpp>

#include "mocks/device.hpp"

using gleditor::MemoryTextSource;
using testing::NiceMock;
using testing::Return;

namespace {

/// Enough text that Letter-geometry pagination produces several hundred
/// pages -- large enough that building the whole backlog in one call (the
/// bug) and building one budgeted batch (the fix) are trivially
/// distinguishable, regardless of how fast the machine running this test is.
std::string manyPagesOfText() {
  const std::string paragraph =
      "The quick brown fox jumps over the lazy dog. Pack my box with five "
      "dozen liquor jugs. How vexingly quick daft zebras jump!\n\n";
  std::string out;
  out.reserve(512U * 1024U);
  while (out.size() < 512U * 1024U) {
    out += paragraph;
  }
  return out;
}

/// Several times manyPagesOfText()'s own size. The Stage 3 priority-order
/// tests need to observe a page built directly *and* a page between it and
/// the viewport left unbuilt in the very same call -- meaningless once a
/// single (possibly catch-up-widened) budgeted call can finish the whole
/// backlog, which a warm glyph/font cache (this fixture's text is identical
/// across every test in the same process, and later tests inherit earlier
/// ones' warm-up) can do to manyPagesOfText()'s own ~100 pages easily. This
/// gives enough headroom that it cannot, regardless of cache state.
std::string manyManyPagesOfText() {
  const std::string paragraph =
      "The quick brown fox jumps over the lazy dog. Pack my box with five "
      "dozen liquor jugs. How vexingly quick daft zebras jump!\n\n";
  std::string out;
  out.reserve(3U * 1024U * 1024U);
  while (out.size() < 3U * 1024U * 1024U) {
    out += paragraph;
  }
  return out;
}

class DocPageBudgetTest : public testing::Test {
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
                      MemoryTextSource(manyPagesOfText(), "budget-test"));
  }
};

TEST_F(DocPageBudgetTest,
       ABacklogShapedBeforeTheFirstCallStillBuildsInBoundedBatches) {
  // Mirrors the race the bug depended on: the background shaping thread
  // finishing (or getting far ahead) before the render thread ever asks for
  // pages. Calling makePages() straight through, synchronously, before the
  // first buildPendingPages() call reproduces exactly that ordering
  // deterministically, rather than depending on real thread timing.
  doc->makePages();

  const auto t0                 = std::chrono::steady_clock::now();
  const bool doneAfterFirstCall = doc->buildPendingPages(*state);
  const auto firstCallElapsed   = std::chrono::steady_clock::now() - t0;

  EXPECT_FALSE(doneAfterFirstCall)
      << "the whole backlog was built in a single call -- the per-call "
         "budget stopped taking effect";
  EXPECT_GT(doc->builtPageCount(), 0U);

  // A generous multiple of the configured budget: real per-page cost varies
  // (this call always builds at least one page even if it alone exceeds the
  // budget), but nothing should be anywhere close to the unbounded-backlog
  // cost this test exists to catch.
  EXPECT_LT(firstCallElapsed, render::kPageBuildFrameBudget * 20)
      << "a single buildPendingPages() call took far longer than its "
         "budget -- likely the whole backlog got built in one call again";

  const auto firstCallPages = doc->builtPageCount();

  std::size_t calls = 1;
  while (!doc->isFullyLoaded()) {
    ASSERT_LT(calls, 10000U) << "buildPendingPages() never finished";
    doc->buildPendingPages(*state);
    ++calls;
  }

  EXPECT_GT(calls, 1U) << "the whole document finished in one call -- this "
                          "document was not large enough to exercise the "
                          "per-call budget";
  EXPECT_GT(doc->builtPageCount(), firstCallPages)
      << "later calls made no further progress";
}

TEST_F(DocPageBudgetTest, ViewportPriorityBuildsTheTargetPageDirectly) {
  doc = Doc::create(renderer, device.get(), glm::mat4(1.0F),
                    MemoryTextSource(manyManyPagesOfText(), "priority-test"));
  doc->makePages();

  // A late offset, and the page it resolves to via pageIndexFilade -- valid
  // even before anything is built (see
  // PageIndexForOffsetAnswersBeforeAnyPageIsBuilt below).
  const auto lateOffset =
      static_cast<std::uint32_t>(doc->contents().size() - 1);
  const auto lateIndex = doc->pageIndexForOffset(lateOffset);
  ASSERT_TRUE(lateIndex.has_value());

  // Move the camera to look directly at that page -- approximateAnchorFor()
  // + worldPoint() resolve the same document-local world Y
  // Doc::viewportPriorityRange() reads the camera against.
  const auto lateAnchor = doc->approximateAnchorFor(lateOffset);
  ASSERT_TRUE(lateAnchor.has_value());
  const auto lateWorld = doc->worldPoint(*lateAnchor);
  ASSERT_TRUE(lateWorld.has_value());
  {
    const std::lock_guard<std::mutex> lock(appState->view);
    appState->view.pos.y = lateWorld->y;
  }

  doc->buildPendingPages(*state);

  // Stage 3's P0 tier (design/priority-page-building.md): the page the
  // camera is looking at is built directly, rather than waiting behind
  // every page before it in document order.
  EXPECT_NE(doc->page(*lateIndex), nullptr)
      << "camera parked on a late page should have built it in the very "
         "next call";
  // And it did not simply build the whole backlog to get there -- otherwise
  // this would not distinguish targeted priority from finishing early.
  EXPECT_LT(doc->builtPageCount(), doc->numPages());
}

TEST_F(DocPageBudgetTest, PriorityOffsetComesRightAfterTheViewport) {
  doc = Doc::create(renderer, device.get(), glm::mat4(1.0F),
                    MemoryTextSource(manyManyPagesOfText(), "priority-test"));
  doc->makePages();

  // Camera stays at its default position, which resolves near the start of
  // a document positioned at the world origin (as this fixture's is) -- so
  // its own viewport-range pages are the ones near page 0.
  const auto lateOffset =
      static_cast<std::uint32_t>(doc->contents().size() - 1);
  const auto lateIndex = doc->pageIndexForOffset(lateOffset);
  ASSERT_TRUE(lateIndex.has_value());
  doc->setPriorityOffsets(std::vector<std::uint32_t>{lateOffset});

  doc->buildPendingPages(*state);

  // P0 (the viewport, near the start) wins...
  EXPECT_NE(doc->page(0), nullptr)
      << "the viewport's own pages should still be built first";
  // ...P1 (the priority offset) comes right after...
  EXPECT_NE(doc->page(*lateIndex), nullptr)
      << "a priority offset far past build progress should have built its "
         "page directly, the same way the camera does";
  // ...and a page between the two -- neither in the viewport nor named by
  // any priority offset -- is still a gap. Otherwise this call simply built
  // everything, which would not distinguish reordering from finishing the
  // backlog.
  EXPECT_EQ(doc->page(*lateIndex / 2), nullptr)
      << "a page between the viewport and the priority target should still "
         "be unbuilt";
}

TEST_F(DocPageBudgetTest,
       DegeneratesToDocumentOrderWithNoPriorityAndAParkedCamera) {
  doc->makePages();

  // Default camera, no priority offsets pushed: build order should be
  // indistinguishable from Stage 2's, which was indistinguishable from
  // before Stage 0 -- see design/priority-page-building.md's "degeneration"
  // test.
  std::size_t calls = 0;
  while (!doc->isFullyLoaded()) {
    ASSERT_LT(calls, 10000U) << "buildPendingPages() never finished";
    doc->buildPendingPages(*state);
    ++calls;

    // The signature of ascending document order: every built page forms a
    // contiguous prefix [0, builtPageCount()) with no gap anywhere in it,
    // checked after every call so a reordering that only shows up
    // transiently cannot slip past a check made once at the end.
    const auto built = doc->builtPageCount();
    for (std::size_t i = 0; i < built; ++i) {
      EXPECT_NE(doc->page(i), nullptr)
          << "page " << i
          << " should already be built -- pages were built out of order "
             "despite no priority offsets and a parked default camera";
    }
  }
}

TEST_F(DocPageBudgetTest, PageIndexForOffsetAnswersBeforeAnyPageIsBuilt) {
  doc->makePages();

  // Nothing has been built yet -- anchorFor() answers nullopt for every
  // offset here, which is exactly the gap pageIndexForOffset() exists to
  // close (see its own doc comment on Doc).
  ASSERT_EQ(doc->builtPageCount(), 0U);
  ASSERT_FALSE(doc->anchorFor(0).has_value());

  const auto earlyPage = doc->pageIndexForOffset(0);
  const auto lateOffset =
      static_cast<std::uint32_t>(doc->contents().size() - 1);
  const auto latePage = doc->pageIndexForOffset(lateOffset);

  ASSERT_TRUE(earlyPage.has_value());
  ASSERT_TRUE(latePage.has_value());
  EXPECT_EQ(*earlyPage, 0U);
  EXPECT_GT(*latePage, *earlyPage)
      << "a byte offset near the end of the document should resolve to a "
         "later page than one at the start";

  // Cross-checked against the same offsets once building actually catches
  // up, so a wrong pageIndexFilade entry (rather than a coincidentally
  // plausible-looking one) would still be caught.
  while (!doc->isFullyLoaded()) {
    doc->buildPendingPages(*state);
  }
  const auto builtEarly = doc->anchorFor(0);
  const auto builtLate  = doc->anchorFor(lateOffset);
  ASSERT_TRUE(builtEarly.has_value());
  ASSERT_TRUE(builtLate.has_value());
  EXPECT_EQ(builtEarly->pageIndex, *earlyPage);
  EXPECT_EQ(builtLate->pageIndex, *latePage);
}

TEST_F(DocPageBudgetTest, ApproximateAnchorAgreesWithAnchorOncePageIsBuilt) {
  doc->makePages();
  while (!doc->isFullyLoaded()) {
    doc->buildPendingPages(*state);
  }

  // Comfortably past the first page, so this is not trivially "both name
  // page 0".
  const auto offset = std::min<std::uint32_t>(
      20000U, static_cast<std::uint32_t>(doc->contents().size() / 2));

  const auto exact  = doc->anchorFor(offset);
  const auto approx = doc->approximateAnchorFor(offset);
  ASSERT_TRUE(exact.has_value());
  ASSERT_TRUE(approx.has_value());
  EXPECT_EQ(exact->pageIndex, approx->pageIndex)
      << "approximateAnchorFor() named a different page than anchorFor()";

  const auto exactWorld  = doc->worldPoint(*exact);
  const auto approxWorld = doc->worldPoint(*approx);
  ASSERT_TRUE(exactWorld.has_value());
  ASSERT_TRUE(approxWorld.has_value());

  // "About a page height" per approximateAnchorFor()'s own contract,
  // checked against the same page geometry the fixture's documents actually
  // use (Letter, Fixed -- MemoryTextSource does not override pageSize()),
  // with room to spare rather than a value guessed independently of it.
  const float onePageHeightWorld =
      (gleditor::letterPage.heightPx + Doc::pageGapPx) * Doc::pixelsToWorld;
  EXPECT_LT(glm::distance(*exactWorld, *approxWorld), onePageHeightWorld * 1.5F)
      << "approximateAnchorFor()'s point should land within about one page "
         "height of anchorFor()'s exact one";
}

} // namespace
