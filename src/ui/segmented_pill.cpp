#include <gleditor/ui/segmented_pill.hpp>

#include <algorithm>
#include <cmath>
#include <gleditor/cpp26_inplace_vector.hpp>
#include <gleditor/text/fit.hpp>
#include <gleditor/text/font.hpp>
#include <numeric>

namespace gleditor::ui {

LayoutResult layoutSegmentedPill(const SegmentedPill &pill, Rect bounds,
                                 const UiMetrics &metrics, const Theme &theme,
                                 text::ShapingCache *cache) {
  const auto modality =
      metrics.touchModality ? InputModality::Touch : InputModality::Pointer;
  const float pillHeight = theme.buttonMetrics.height(modality, pill.compact);
  bounds.height =
      bounds.height > 0.0F ? std::min(bounds.height, pillHeight) : pillHeight;

  if (pill.segments.empty()) {
    return LayoutResult{.bounds = metrics.rounded(bounds)};
  }

  const float seam       = theme.buttonMetrics.seamWidthPx;
  const std::size_t n    = pill.segments.size();
  const float totalSeams = n > 1 ? static_cast<float>(n - 1) * seam : 0.0F;

  constexpr std::size_t kMaxInlineSegments = 32;
  cpp26::inplace_vector<float, kMaxInlineSegments> inlineMeasured;
  std::vector<float> heapMeasured;
  std::span<float> measuredWidths;
  if (n <= kMaxInlineSegments) {
    inlineMeasured.resize(n, 0.0F);
    measuredWidths = std::span<float>(inlineMeasured.data(), n);
  } else {
    heapMeasured.resize(n, 0.0F);
    measuredWidths = std::span<float>(heapMeasured.data(), n);
  }
  float fixedWidthSum   = 0.0F;
  std::size_t flexCount = 0;

  for (std::size_t i = 0; i < n; ++i) {
    const auto &seg = pill.segments[i];
    if (seg.customWidthPx > 0.0F) {
      measuredWidths[i] = seg.customWidthPx;
      fixedWidthSum += seg.customWidthPx;
    } else {
      const auto role     = seg.isMonospace ? FontRole::Mono : FontRole::Label;
      const auto fontDesc = metrics.fontDescription(role, theme);
      const auto font     = text::FontManager::instance().getFont(fontDesc);
      float textW         = 0.0F;
      if (font && !seg.label.empty()) {
        if (cache) {
          textW = cache->fitted(seg.label, font, {}).widthPx;
        } else {
          textW = text::fit(seg.label, font, {}).widthPx;
        }
      }
      const float padPx = font ? font->metrics().lineHeight *
                                     theme.buttonMetrics.paddingHorizontalEm
                               : 12.0F;
      float w           = textW + 2.0F * padPx;
      if (metrics.touchModality) {
        w = std::max(w, theme.buttonMetrics.virtualHitTargetMinPx);
      } else {
        w = std::max(w, theme.buttonMetrics.heightCompactPx);
      }
      measuredWidths[i] = w;
      ++flexCount;
    }
  }

  const float naturalContentWidth =
      std::accumulate(measuredWidths.begin(), measuredWidths.end(), 0.0F);
  if (bounds.width <= 0.0F) {
    bounds.width = naturalContentWidth + totalSeams;
  }

  const float availableForSegments = std::max(0.0F, bounds.width - totalSeams);
  cpp26::inplace_vector<float, kMaxInlineSegments> inlineFinal;
  std::vector<float> heapFinal;
  std::span<float> finalWidths;
  if (n <= kMaxInlineSegments) {
    inlineFinal = inlineMeasured;
    finalWidths = std::span<float>(inlineFinal.data(), n);
  } else {
    heapFinal   = heapMeasured;
    finalWidths = std::span<float>(heapFinal.data(), n);
  }

  if (flexCount > 0 && availableForSegments > fixedWidthSum) {
    const float remainingForFlex = availableForSegments - fixedWidthSum;
    float currentFlexSum         = 0.0F;
    for (std::size_t i = 0; i < n; ++i) {
      if (pill.segments[i].customWidthPx <= 0.0F) {
        currentFlexSum += measuredWidths[i];
      }
    }
    for (std::size_t i = 0; i < n; ++i) {
      if (pill.segments[i].customWidthPx <= 0.0F) {
        if (currentFlexSum > 0.0F) {
          finalWidths[i] =
              (measuredWidths[i] / currentFlexSum) * remainingForFlex;
        } else {
          finalWidths[i] = remainingForFlex / static_cast<float>(flexCount);
        }
      }
    }
  } else if (naturalContentWidth > 0.0F && availableForSegments > 0.0F) {
    const float scale = availableForSegments / naturalContentWidth;
    for (std::size_t i = 0; i < n; ++i) {
      finalWidths[i] *= scale;
    }
  }

  LayoutResult result;
  result.bounds = metrics.rounded(bounds);

  if (pill.containerId != 0) {
    result.boxes.push_back({
        .id          = pill.containerId,
        .parentId    = 0,
        .rect        = result.bounds,
        .contentRect = result.bounds,
        .focusable   = false,
        .enabled     = true,
    });
  }

  float currentX = result.bounds.left;
  for (std::size_t i = 0; i < n; ++i) {
    const auto &seg    = pill.segments[i];
    const float w      = finalWidths[i];
    const Rect segRect = metrics.rounded({
        currentX,
        result.bounds.bottom,
        w,
        result.bounds.height,
    });

    result.boxes.push_back({
        .id          = seg.id,
        .parentId    = pill.containerId,
        .rect        = segRect,
        .contentRect = segRect,
        .focusable   = seg.enabled,
        .enabled     = seg.enabled,
        .focusGroup  = pill.containerId,
    });

    if (seg.enabled && seg.id != 0) {
      result.focusOrder.push_back(seg.id);
    }

    currentX += w + seam;
  }

  return result;
}

} // namespace gleditor::ui
