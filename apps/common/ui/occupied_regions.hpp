/**
 * @file occupied_regions.hpp
 * @brief Screen rectangles something already fills, published once a frame so
 *        panels placed over the scene can stand clear of them.
 *
 * The slice presentation knows where its cells are on screen; the overview
 * thumbnail and the link panel are placed by their own code and used to land
 * on top of them (M2 third gate, F2 to F4). One rule serves all: whoever
 * draws something the reader reads publishes its rectangles here, and a
 * panel asks placeClear() for a spot that covers none of them.
 *
 * Rectangles are in canvas pixels with Y upwards (gleditor::ui::Rect). A
 * publisher keeps its own storage, so a frame that publishes the same
 * rectangles as the last allocates nothing and changes nothing; a reader
 * sees a changed revision() only when the rectangles changed. Everything is
 * on the render thread, but the class is locked because the accessibility
 * thread may ask for the revision.
 */
#ifndef COMMON_UI_OCCUPIED_REGIONS_HPP
#define COMMON_UI_OCCUPIED_REGIONS_HPP

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <span>
#include <vector>

#include <gleditor/ui/rect.hpp>

namespace xanadu {

class OccupiedRegions {
public:
  /// Replace what @p owner occupies. An owner is any address that stays put
  /// for as long as it publishes; publishing nothing retires it.
  OccupiedRegions *publish(const void *owner,
                           std::span<const gleditor::ui::Rect> rects) {
    const std::scoped_lock lock(guard);
    auto found = std::ranges::find(entries, owner, &Entry::owner);
    if (found == entries.end()) {
      if (rects.empty()) {
        return this;
      }
      entries.push_back(Entry{.owner = owner, .rects = {}});
      found = entries.end() - 1;
    }
    if (std::ranges::equal(found->rects, rects, sameRect)) {
      return this;
    }
    found->rects.assign(rects.begin(), rects.end());
    ++revisionCount;
    return this;
  }

  /// Everything published, owners in the order they first published.
  template <typename Visit> void forEach(Visit &&visit) const {
    const std::scoped_lock lock(guard);
    for (const auto &entry : entries) {
      for (const auto &rect : entry.rects) {
        visit(rect);
      }
    }
  }

  [[nodiscard]] bool empty() const {
    const std::scoped_lock lock(guard);
    return std::ranges::all_of(
        entries, [](const Entry &entry) { return entry.rects.empty(); });
  }

  /// Changes whenever the rectangles do.
  [[nodiscard]] std::uint64_t revision() const {
    const std::scoped_lock lock(guard);
    return revisionCount;
  }

private:
  struct Entry {
    const void *owner{};
    std::vector<gleditor::ui::Rect> rects;
  };

  static bool sameRect(const gleditor::ui::Rect &a,
                       const gleditor::ui::Rect &b) {
    return a.left == b.left && a.bottom == b.bottom && a.width == b.width &&
           a.height == b.height;
  }

  mutable std::mutex guard;
  std::vector<Entry> entries;
  std::uint64_t revisionCount{1};
};

/// The area two rectangles share.
[[nodiscard]] inline float overlapArea(const gleditor::ui::Rect &a,
                                       const gleditor::ui::Rect &b) noexcept {
  const float w =
      std::min(a.left + a.width, b.left + b.width) - std::max(a.left, b.left);
  const float h = std::min(a.bottom + a.height, b.bottom + b.height) -
                  std::max(a.bottom, b.bottom);
  return w > 0.0F && h > 0.0F ? w * h : 0.0F;
}

/**
 * @brief Where to put a panel of @p wanted's size so it covers none of
 *        @p hard and as little as possible of @p soft.
 *
 * The best of @p wanted and the safe area's four corners and four edge
 * midpoints, @p gap in from the edge: least hard overlap first, then least
 * soft, then nearest to @p wanted, so a panel that covers nothing it should
 * not stays where it was put. Hard rectangles are what must stay readable (the
 * slice's cells); soft ones are what is better left readable (page text). With
 * nothing hard to avoid the panel stays where it was put, so a scene with no
 * slice is placed exactly as before. Allocates nothing.
 */
template <typename Soft>
[[nodiscard]] gleditor::ui::Rect
placeClear(const gleditor::ui::Rect wanted, const gleditor::ui::Rect safe,
           const float gap, const std::span<const gleditor::ui::Rect> hard,
           Soft &&softOverlap) {
  const auto hardOverlap = [&](const gleditor::ui::Rect &at) {
    float sum = 0.0F;
    for (const auto &rect : hard) {
      sum += overlapArea(at, rect);
    }
    return sum;
  };
  if (hard.empty()) {
    return wanted;
  }
  const float left = safe.left + gap;
  const float right =
      std::max(left, safe.left + safe.width - wanted.width - gap);
  const float bottom = safe.bottom + gap;
  const float top =
      std::max(bottom, safe.bottom + safe.height - wanted.height - gap);
  const float midX = (left + right) * 0.5F;
  const float midY = (bottom + top) * 0.5F;
  const std::array<std::array<float, 2>, 8> spots{{{left, bottom},
                                                   {right, bottom},
                                                   {left, top},
                                                   {right, top},
                                                   {midX, bottom},
                                                   {midX, top},
                                                   {left, midY},
                                                   {right, midY}}};
  auto best      = wanted;
  float bestHard = hardOverlap(wanted);
  float bestSoft = softOverlap(wanted);
  float bestAway = 0.0F;
  for (const auto &spot : spots) {
    const gleditor::ui::Rect at{spot[0], spot[1], wanted.width, wanted.height};
    const float h = hardOverlap(at);
    const float s = softOverlap(at);
    const float away =
        std::hypot(at.left - wanted.left, at.bottom - wanted.bottom);
    const bool better = h < bestHard || (h == bestHard && s < bestSoft) ||
                        (h == bestHard && s == bestSoft && away < bestAway);
    if (better) {
      best     = at;
      bestHard = h;
      bestSoft = s;
      bestAway = away;
    }
  }
  return best;
}

} // namespace xanadu

#endif // COMMON_UI_OCCUPIED_REGIONS_HPP
