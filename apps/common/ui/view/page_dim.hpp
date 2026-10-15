/**
 * @file page_dim.hpp
 * @brief Dims the xanadoc pages while a slice is the thing being read, and
 *        puts them back (view-system-implementation-plan.md §5.1, M2 finding
 *        B1).
 *
 * A page is the brightest and largest thing in a frame and a slice drawn over
 * it reads as the lesser of the two. While the slice is the subject the pages
 * keep a share of their own opacity (view.page.dim); the share each had
 * before is recorded, so leaving puts every page back as it was and a
 * background page's own dimness is not mistaken for a page to lighten.
 */
#ifndef COMMON_UI_VIEW_PAGE_DIM_HPP
#define COMMON_UI_VIEW_PAGE_DIM_HPP

#include <memory>
#include <vector>

#include <gleditor/doc.hpp>
#include <gleditor/frame_contributor.hpp>

namespace xanadu::view {

class PageDim {
public:
  /**
   * @brief Ease every open page to @p share of the opacity it settles at, or
   *        back to it when @p share is 1. Called every frame on the render
   *        thread; costs nothing while nothing is dimmed and nothing is
   *        asked.
   *
   * A page opened while dimmed is dimmed too; one closed is forgotten. A
   * writer that sets a page's opacity while it is held (the onion-skin toggle,
   * the side-by-side arrangement) is noticed: the dim is laid over the value
   * it wrote, not lost to it.
   */
  void apply(gleditor::FrameContext &ctx, float share);

  /// Pages currently held dimmed.
  [[nodiscard]] std::size_t dimmed() const noexcept { return held_.size(); }

private:
  struct Held {
    std::weak_ptr<Doc> doc;
    /// What the page settled at before it was dimmed.
    float before{1.0F};
    /// What was last written for it: a resting opacity that differs is
    /// another writer's, and the dim is laid over that.
    float set{1.0F};
  };
  std::vector<Held> held_;
  float applied_{1.0F};
};

} // namespace xanadu::view

#endif // COMMON_UI_VIEW_PAGE_DIM_HPP
