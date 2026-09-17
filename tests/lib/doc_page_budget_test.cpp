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
  EXPECT_GT(doc->numPages(), 0U);

  // A generous multiple of the configured budget: real per-page cost varies
  // (this call always builds at least one page even if it alone exceeds the
  // budget), but nothing should be anywhere close to the unbounded-backlog
  // cost this test exists to catch.
  EXPECT_LT(firstCallElapsed, render::kPageBuildFrameBudget * 20)
      << "a single buildPendingPages() call took far longer than its "
         "budget -- likely the whole backlog got built in one call again";

  const auto firstCallPages = doc->numPages();

  std::size_t calls = 1;
  while (!doc->isFullyLoaded()) {
    ASSERT_LT(calls, 10000U) << "buildPendingPages() never finished";
    doc->buildPendingPages(*state);
    ++calls;
  }

  EXPECT_GT(calls, 1U) << "the whole document finished in one call -- this "
                          "document was not large enough to exercise the "
                          "per-call budget";
  EXPECT_GT(doc->numPages(), firstCallPages)
      << "later calls made no further progress";
}

TEST_F(DocPageBudgetTest, CatchesUpFasterWhenTheCameraIsAheadOfBuildProgress) {
  doc->makePages();

  // First call: camera at its default position, which resolves near the
  // start of a document positioned at the world origin (as this fixture's
  // is), so this establishes the plain per-call rate -- including whatever
  // one-time warm-up cost (font/glyph-cache misses) the very first call
  // pays, so that cost lands in the baseline rather than skewing the
  // comparison below.
  doc->buildPendingPages(*state);
  const auto normalPacePages = doc->numPages();
  ASSERT_GT(normalPacePages, 0U);

  // Move the camera deep into this document's own stacking direction
  // (increasingly negative local Y is further into the document -- see
  // Doc::buildBudgetForThisCall()), simulating having scrolled far ahead of
  // load progress before the page there has actually been built, then make
  // the very next call on the *same* document -- so it benefits from
  // exactly the same warmed-up glyph cache and font lookups the first call
  // already paid for, isolating the catch-up budget as the only remaining
  // variable between the two page counts.
  {
    const std::lock_guard<std::mutex> lock(appState->view);
    appState->view.pos.y = -1'000'000.0F;
  }

  doc->buildPendingPages(*state);
  const auto catchUpCallPages = doc->numPages() - normalPacePages;

  // A conservative fraction of render::kPageBuildCatchUpMultiplier: comfortably
  // more than a warm cache alone would explain, comfortably less than the
  // full configured multiplier, so this does not have to track that
  // constant's exact value.
  EXPECT_GT(catchUpCallPages, normalPacePages * 2)
      << "camera looking far past build progress should let one call build "
         "noticeably more pages than the established plain-budget rate";
}

TEST_F(DocPageBudgetTest, PriorityOffsetFarAheadEngagesCatchUpToo) {
  doc->makePages();

  // Same baseline reasoning as the camera test above: the very first call's
  // one-time warm-up cost lands here rather than skewing the comparison.
  doc->buildPendingPages(*state);
  const auto normalPacePages = doc->numPages();
  ASSERT_GT(normalPacePages, 0U);

  // Push a priority offset near the very end of the document, as LinkBeams
  // would for a beam whose far end lands on a late page -- with the camera
  // left exactly where it was (near the start), so any acceleration
  // observed can only be attributed to the priority offset, not the camera.
  doc->setPriorityOffsets(std::vector<std::uint32_t>{
      static_cast<std::uint32_t>(doc->contents().size() - 1)});

  doc->buildPendingPages(*state);
  const auto catchUpCallPages = doc->numPages() - normalPacePages;

  EXPECT_GT(catchUpCallPages, normalPacePages * 2)
      << "a priority offset far past build progress should let one call "
         "build noticeably more pages than the established plain-budget "
         "rate, the same way the camera does";
}

TEST_F(DocPageBudgetTest, PageIndexForOffsetAnswersBeforeAnyPageIsBuilt) {
  doc->makePages();

  // Nothing has been built yet -- anchorFor() answers nullopt for every
  // offset here, which is exactly the gap pageIndexForOffset() exists to
  // close (see its own doc comment on Doc).
  ASSERT_EQ(doc->numPages(), 0U);
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
