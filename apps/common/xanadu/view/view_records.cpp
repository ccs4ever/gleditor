#include "common/xanadu/view/view_records.hpp"

#include <string_view>

#include <gleditor/logging.hpp>

namespace xanadu::view {
namespace {

constexpr const char *kLayoutCategory = "view.layout";

/// Appends and answers the record's index. A full vector grows rather than
/// refuse (V-R5); the growth is logged so a layout that keeps outgrowing its
/// sink, and so allocates every frame, can be found.
template <typename Record>
std::size_t append(std::vector<Record> &records, const Record &record,
                   const std::string_view kind) {
  const auto before = records.capacity();
  records.push_back(record);
  if (records.capacity() != before) {
    GLEDITOR_LOG_DEBUG(kLayoutCategory,
                       "layout sink grew its {} from {} to {} records", kind,
                       before, records.capacity());
  }
  return records.size() - 1;
}

/// Composes @p local onto its chain of parent frames, starting at @p parent.
/// A chain longer than the number of frames has revisited one: a cycle.
std::optional<Pose> compose(const LayoutSink &sink, Pose local,
                            std::optional<std::uint32_t> parent) noexcept {
  const auto frames = sink.frames();
  for (std::size_t steps = 0; parent; ++steps) {
    if (*parent >= frames.size() || steps > frames.size()) {
      return std::nullopt;
    }
    const auto &frame = frames[*parent];
    local.centre      = frame.centre + frame.orientation * local.centre;
    local.orientation = frame.orientation * local.orientation;
    parent            = frame.parent;
  }
  return local;
}

} // namespace

std::uint32_t LayoutSink::push(const PlacedItem &item) {
  return static_cast<std::uint32_t>(append(items_, item, "items"));
}

std::uint32_t LayoutSink::push(const PlacedFrame &frame) {
  return static_cast<std::uint32_t>(append(frames_, frame, "frames"));
}

LayoutSink *LayoutSink::push(const PlacedEdge &edge) {
  append(edges_, edge, "edges");
  return this;
}

LayoutSink *LayoutSink::push(const DropTarget &target) {
  append(dropTargets_, target, "drop targets");
  return this;
}

LayoutSink *LayoutSink::push(const MotionHint &hint) {
  append(hints_, hint, "motion hints");
  return this;
}

LayoutSink *LayoutSink::clear() noexcept {
  items_.clear();
  frames_.clear();
  edges_.clear();
  dropTargets_.clear();
  hints_.clear();
  return this;
}

std::optional<Pose> placedPose(const LayoutSink &sink,
                               const PlacedFrame &frame) noexcept {
  return compose(sink, {frame.centre, frame.orientation}, frame.parent);
}

std::optional<Pose> placedPose(const LayoutSink &sink,
                               const PlacedItem &item) noexcept {
  return compose(sink, {item.centre, item.orientation}, item.frame);
}

} // namespace xanadu::view

std::size_t std::hash<xanadu::view::SubjectId>::operator()(
    const xanadu::view::SubjectId &id) const noexcept {
  // Boost's combine: the fields are small integers that a plain xor would
  // cancel against each other. The constant is 2^64 divided by the golden
  // ratio, which spreads consecutive values across the word.
  constexpr std::uint64_t goldenRatio64 = 0x9e3779b97f4a7c15ULL;
  std::size_t seed                      = std::to_underlying(id.kind);
  const auto mix                        = [&seed](const std::uint64_t value) {
    seed ^= std::hash<std::uint64_t>{}(value) + goldenRatio64 + (seed << 6U) +
            (seed >> 2U);
  };
  mix(id.slot);
  mix(id.value);
  mix(id.epoch);
  return seed;
}
