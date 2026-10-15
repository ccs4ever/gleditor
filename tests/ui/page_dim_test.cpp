/**
 * @file page_dim_test.cpp
 * @brief The dim a slice puts over the pages it stands beside holds against
 *        the other writers of a page's opacity (M2 gate 2, D5).
 */
#include <gtest/gtest.h>

#include <memory>

#include <choreograph/Choreograph.h>
#include <glm/glm.hpp>

#include <gleditor/doc.hpp>
#include <gleditor/frame_contributor.hpp>
#include <gleditor/render_state.hpp>

#include "common/ui/view/page_dim.hpp"
#include "null_device.hpp"

namespace {

constexpr float kSame = 1.0F / 512.0F;

struct Pages {
  NullRenderDevice device;
  RenderState state{&device};
  ch::Timeline timeline;
  xanadu::view::PageDim dim;

  Pages() {
    for (const auto *const name : {"one", "two"}) {
      state.docs.push_back(Doc::create({}, &device, glm::mat4(1),
                                       gleditor::MemoryTextSource({}, name)));
    }
  }

  void apply(const float share) {
    const glm::mat4 projection(1);
    gleditor::FrameContext ctx{.state          = state,
                               .viewProjection = projection,
                               .screenWidth    = 800,
                               .screenHeight   = 600,
                               .timeline       = timeline};
    dim.apply(ctx, share);
  }
};

} // namespace

TEST(PageDimTest, EveryPageKeepsTheShareAndIsPutBack) {
  Pages pages;
  pages.apply(0.5F);
  for (const auto &doc : pages.state.docs) {
    EXPECT_NEAR(doc->resting(), 0.5F, kSame);
  }
  EXPECT_EQ(pages.dim.dimmed(), 2U);
  pages.apply(1.0F);
  for (const auto &doc : pages.state.docs) {
    EXPECT_NEAR(doc->resting(), 1.0F, kSame);
  }
  EXPECT_EQ(pages.dim.dimmed(), 0U);
}

TEST(PageDimTest, AnotherWritersOpacityDoesNotUndoTheDim) {
  Pages pages;
  pages.apply(0.5F);
  // The side-by-side arrangement sets a page's opacity outright, as the
  // onion-skin toggle does with its own steps.
  pages.state.docs[0]->animateOpacity(pages.timeline, 1.0F);
  pages.state.docs[1]->animateOpacity(pages.timeline, 0.8F);
  pages.apply(0.5F);
  EXPECT_NEAR(pages.state.docs[0]->resting(), 0.5F, kSame);
  EXPECT_NEAR(pages.state.docs[1]->resting(), 0.4F, kSame)
      << "the dim stands on the value the other writer chose";

  // Leaving puts each back to what the page settled at, the other writer's
  // choice included.
  pages.apply(1.0F);
  EXPECT_NEAR(pages.state.docs[0]->resting(), 1.0F, kSame);
  EXPECT_NEAR(pages.state.docs[1]->resting(), 0.8F, kSame);
}

TEST(PageDimTest, ThePageShareChangingBetweenViewsMovesTheDim) {
  Pages pages;
  pages.apply(0.3F);
  EXPECT_NEAR(pages.state.docs[0]->resting(), 0.3F, kSame);
  // The unified view keeps more of the page than the slice alone.
  pages.apply(0.5F);
  EXPECT_NEAR(pages.state.docs[0]->resting(), 0.5F, kSame);
}
