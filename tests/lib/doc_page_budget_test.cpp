/**
 * @file doc_page_budget_test.cpp
 * @brief Doc::buildPendingPages() stays within its per-call time budget even
 * when the background shaping thread has produced a large backlog before the
 * render thread ever asks for pages -- the regression covered by
 * design/kjv-load-blocking-regression.md.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>

#include <glm/ext/matrix_float4x4.hpp>

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

    auto appState             = std::make_shared<AppState>();
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

} // namespace
