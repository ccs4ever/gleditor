#pragma once

#include <cstdint>
#include <gleditor/text/shaping_cache.hpp>
#include <gleditor/ui/button_metrics.hpp>
#include <gleditor/ui/layout.hpp>
#include <gleditor/ui/metrics.hpp>
#include <gleditor/ui/theme.hpp>
#include <string>
#include <vector>

namespace gleditor::ui {

using WidgetId = std::uint32_t;

struct SegmentItem {
  WidgetId id{};
  std::string label;
  std::string action;
  std::string accessibleLabel;
  bool enabled{true};
  bool selected{false};
  bool isMonospace{false};
  float customWidthPx{0.0F};

  constexpr SegmentItem &setId(WidgetId val) noexcept {
    id = val;
    return *this;
  }
  SegmentItem &setLabel(std::string val) {
    label = std::move(val);
    return *this;
  }
  SegmentItem &setAction(std::string val) {
    action = std::move(val);
    return *this;
  }
  SegmentItem &setAccessibleLabel(std::string val) {
    accessibleLabel = std::move(val);
    return *this;
  }
  constexpr SegmentItem &setEnabled(bool val) noexcept {
    enabled = val;
    return *this;
  }
  constexpr SegmentItem &setSelected(bool val) noexcept {
    selected = val;
    return *this;
  }
  constexpr SegmentItem &setIsMonospace(bool val) noexcept {
    isMonospace = val;
    return *this;
  }
  constexpr SegmentItem &setCustomWidthPx(float val) noexcept {
    customWidthPx = val;
    return *this;
  }

  bool operator==(const SegmentItem &) const = default;
};

struct SegmentedPill {
  WidgetId containerId{};
  std::vector<SegmentItem> segments;
  ButtonShapeKind outerShape{ButtonShapeKind::Pill};
  bool compact{false};

  constexpr SegmentedPill &setContainerId(WidgetId val) noexcept {
    containerId = val;
    return *this;
  }
  SegmentedPill &setSegments(std::vector<SegmentItem> val) {
    segments = std::move(val);
    return *this;
  }
  SegmentedPill &addSegment(SegmentItem seg) {
    segments.push_back(std::move(seg));
    return *this;
  }
  constexpr SegmentedPill &setOuterShape(ButtonShapeKind val) noexcept {
    outerShape = val;
    return *this;
  }
  constexpr SegmentedPill &setCompact(bool val) noexcept {
    compact = val;
    return *this;
  }

  bool operator==(const SegmentedPill &) const = default;
};

[[nodiscard]] LayoutResult
layoutSegmentedPill(const SegmentedPill &pill, Rect bounds,
                    const UiMetrics &metrics, const Theme &theme,
                    text::ShapingCache *cache = nullptr);

} // namespace gleditor::ui
