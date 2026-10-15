#include "common/ui/view/page_dim.hpp"

#include <algorithm>
#include <cmath>

#include <gleditor/render_state.hpp>

namespace xanadu::view {

namespace {

/// Whether two opacities are the same one: they are written and read back as
/// floats, but a writer that set a value of its own may differ from ours by
/// less than anything the eye sees.
[[nodiscard]] bool sameOpacity(const float a, const float b) noexcept {
  constexpr float kSame = 1.0F / 1024.0F;
  return std::abs(a - b) <= kSame;
}

} // namespace

void PageDim::apply(gleditor::FrameContext &ctx, const float share) {
  const bool want = share < 1.0F;
  if (!want && held_.empty()) {
    applied_ = 1.0F;
    return;
  }
  // A page closed since last frame is no longer held.
  std::erase_if(held_, [](const Held &one) { return one.doc.expired(); });
  for (const auto &doc : ctx.state.docs) {
    if (!doc || doc->isClosing()) {
      continue;
    }
    const auto found = std::ranges::find_if(
        held_, [&doc](const Held &one) { return one.doc.lock() == doc; });
    if (want) {
      if (found == held_.end()) {
        held_.push_back({.doc    = doc,
                         .before = doc->resting(),
                         .set    = doc->resting() * share});
        doc->animateOpacity(ctx.timeline, held_.back().set);
      } else if (!sameOpacity(doc->resting(), found->set)) {
        // Something else wrote the page's opacity since: the onion-skin
        // toggle and the side-by-side arrangement each set it outright, which
        // would undo the dim and leave the page at full strength. What it
        // wrote is what the page settles at now, and the dim stands on that.
        found->before = doc->resting();
        found->set    = found->before * share;
        doc->animateOpacity(ctx.timeline, found->set);
      } else if (applied_ != share) {
        found->set = found->before * share;
        doc->animateOpacity(ctx.timeline, found->set);
      }
    } else if (found != held_.end()) {
      // Put back what the page settled at, unless another writer has since
      // chosen a value of its own, which is then the page's.
      if (sameOpacity(doc->resting(), found->set)) {
        doc->animateOpacity(ctx.timeline, found->before);
      }
      held_.erase(found);
    }
  }
  applied_ = share;
}

} // namespace xanadu::view
