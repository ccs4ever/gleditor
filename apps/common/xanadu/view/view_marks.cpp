#include "common/xanadu/view/view_marks.hpp"

#include <algorithm>

namespace xanadu::view {

SubjectId markId(const MarkKind kind, const SubjectId &subject) noexcept {
  auto id  = SubjectId::marker(subject.value, static_cast<std::uint32_t>(kind));
  id.epoch = subject.epoch;
  return id;
}

float ghostOpacity(const float fadeFloor, const float share) noexcept {
  return std::clamp(fadeFloor, 0.0F, 1.0F) * std::clamp(share, 0.0F, 1.0F);
}

PlacedItem ghostOf(const PlacedItem &item, const float opacity) noexcept {
  PlacedItem ghost = item;
  ghost.id         = markId(MarkKind::Ghost, item.id);
  ghost.content    = ContentMode::None;
  ghost.flags      = itemGhost;
  ghost.window     = std::nullopt;
  markViewOnly(ghost, opacity);
  return ghost;
}

PlacedItem *markViewOnly(PlacedItem &item, const float opacity) noexcept {
  item.flags   = (item.flags & ~std::uint32_t{itemFocus}) | itemViewOnly;
  item.opacity = std::clamp(opacity, 0.0F, 1.0F);
  return &item;
}

PlacedItem *markFocus(PlacedItem &item) noexcept {
  item.flags |= itemFocus;
  item.opacity = 1.0F;
  item.content = ContentMode::Full;
  return &item;
}

ContentMode legibleContent(const float opacity, const float linePx,
                           const Legibility &steps) noexcept {
  if (opacity < steps.coarseBelow || linePx < steps.minReadableLinePx) {
    return ContentMode::Coarse;
  }
  if (opacity < steps.abbreviateBelow) {
    return ContentMode::Abbreviated;
  }
  return ContentMode::Full;
}

} // namespace xanadu::view
