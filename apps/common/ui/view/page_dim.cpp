#include "common/ui/view/page_dim.hpp"

#include <algorithm>

#include <gleditor/render_state.hpp>

namespace xanadu::view {

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
        held_.push_back({.doc = doc, .before = doc->resting()});
        doc->animateOpacity(ctx.timeline, doc->resting() * share);
      } else if (applied_ != share) {
        doc->animateOpacity(ctx.timeline, found->before * share);
      }
    } else if (found != held_.end()) {
      doc->animateOpacity(ctx.timeline, found->before);
      held_.erase(found);
    }
  }
  applied_ = share;
}

} // namespace xanadu::view
