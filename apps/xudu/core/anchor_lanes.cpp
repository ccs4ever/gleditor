#include "anchor_lanes.hpp"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace xudu {

namespace {

/// Slack allowed before two anchors are called overlapping, in world units.
/// Anchors that merely touch -- one passage ending on the line the next begins
/// on -- are left in the same lane: they are distinguishable where they meet
/// without spending a lane on it, and both keep the page edge.
constexpr float overlapSlack = 1.0e-4F;

bool overlaps(const AnchorExtent &a, const AnchorExtent &b) {
  return std::min(a.top, b.top) - std::max(a.bottom, b.bottom) > overlapSlack;
}

} // namespace

std::vector<AnchorLane>
assignAnchorLanes(const std::vector<AnchorExtent> &extents,
                  const int laneLimit) {
  const int limit = std::max(1, laneLimit);
  std::vector<AnchorLane> out(extents.size(), AnchorLane{0, 1});
  if (extents.empty()) {
    return out;
  }

  // Swept top down rather than in the order the anchors arrived: greedy
  // colouring is optimal on an interval graph in that order and merely valid
  // in any other, so this is what keeps a margin from using four lanes where
  // two would do. Ties go to whoever was given first, which is how the link
  // the reader is following ends up flush against the page edge.
  std::vector<std::size_t> sweep(extents.size());
  for (std::size_t i = 0; i < sweep.size(); i++) {
    sweep[i] = i;
  }
  std::ranges::stable_sort(sweep, [&extents](std::size_t a, std::size_t b) {
    return extents[a].top > extents[b].top;
  });

  std::vector<bool> assigned(extents.size(), false);
  for (const auto which : sweep) {
    std::vector<bool> taken(static_cast<std::size_t>(limit), false);
    for (std::size_t other = 0; other < extents.size(); other++) {
      if (other == which || !assigned[other] ||
          !overlaps(extents[which], extents[other])) {
        continue;
      }
      taken[static_cast<std::size_t>(out[other].lane)] = true;
    }
    int lane = limit - 1;
    for (int candidate = 0; candidate < limit; candidate++) {
      if (!taken[static_cast<std::size_t>(candidate)]) {
        lane = candidate;
        break;
      }
    }
    out[which].lane = lane;
    assigned[which] = true;
  }

  // How wide a slice is has to be agreed on by every anchor that could sit
  // beside it, and "could sit beside it" reaches further than "overlaps it":
  // A over B and B over C puts A and C in the same margin without their ever
  // touching. So the count comes from the connected group, which is what
  // makes the slices of a run of anchors tile the margin rather than each
  // one dividing it by however many it can see from where it is.
  std::vector<bool> grouped(extents.size(), false);
  std::vector<std::size_t> queue;
  std::vector<std::size_t> group;
  for (std::size_t seed = 0; seed < extents.size(); seed++) {
    if (grouped[seed]) {
      continue;
    }
    group.clear();
    queue.assign(1, seed);
    grouped[seed] = true;
    while (!queue.empty()) {
      const auto here = queue.back();
      queue.pop_back();
      group.push_back(here);
      for (std::size_t other = 0; other < extents.size(); other++) {
        if (!grouped[other] && overlaps(extents[here], extents[other])) {
          grouped[other] = true;
          queue.push_back(other);
        }
      }
    }
    int lanes = 1;
    for (const auto member : group) {
      lanes = std::max(lanes, out[member].lane + 1);
    }
    for (const auto member : group) {
      out[member].lanes = lanes;
    }
  }
  return out;
}

MarginLane marginLane(const float marginWidth, const int lane, const int lanes,
                      const float kerf) {
  if (!(marginWidth > 0.0F)) {
    return {};
  }
  const int total   = std::max(1, lanes);
  const int which   = std::clamp(lane, 0, total - 1);
  const float width = marginWidth / static_cast<float>(total);
  const float gap   = std::clamp(kerf, 0.0F, width * 0.5F);

  MarginLane out{.fromPageEdge = static_cast<float>(which) * width,
                 .toPageEdge   = static_cast<float>(which + 1) * width};
  // The outermost edge of lane 0 and the innermost of the last lane are left
  // alone: those are the page edge and the text edge, and an anchor that
  // stopped short of either would float in the margin instead of marking it.
  if (which > 0) {
    out.fromPageEdge += gap * 0.5F;
  }
  if (which + 1 < total) {
    out.toPageEdge -= gap * 0.5F;
  }
  return out;
}

} // namespace xudu
