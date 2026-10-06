#include <gleditor/ui/metrics.hpp>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>

namespace gleditor::ui {
namespace {
float positiveScale(float value) {
  return std::isfinite(value) && value > 0.0F ? value : 1.0F;
}
float extent(float value) {
  return std::isfinite(value) ? std::max(0.0F, value) : 0.0F;
}
float coordinate(float value) { return std::isfinite(value) ? value : 0.0F; }
Rect validRect(Rect rect) {
  return {coordinate(rect.left), coordinate(rect.bottom), extent(rect.width),
          extent(rect.height)};
}
} // namespace
float UiMetrics::scale() const {
  return positiveScale(positiveScale(contentScale) * positiveScale(userScale));
}
float UiMetrics::px(float value) const { return coordinate(value * scale()); }
float UiMetrics::logical(float value) const {
  return coordinate(value / scale());
}
float UiMetrics::fontPixels(FontRole role, const Theme &theme) const {
  constexpr float pixelsPerPoint = 96.0F / 72.0F;
  const float points             = positiveScale(theme.font(role).points);
  return std::max(extent(theme.type.minFontPx),
                  points * pixelsPerPoint * scale() * positiveScale(fontScale));
}
std::string UiMetrics::fontDescription(FontRole role,
                                       const Theme &theme) const {
  constexpr float pointsPerPixel = 72.0F / 96.0F;
  std::ostringstream description;
  description.imbue(std::locale::classic());
  description << theme.font(role).family << ' ' << std::setprecision(7)
              << fontPixels(role, theme) * pointsPerPixel;
  return description.str();
}
Rect UiMetrics::pixelSafeArea() const {
  const float width  = static_cast<float>(std::max(0, screenWidth));
  const float height = static_cast<float>(std::max(0, screenHeight));
  const float share =
      std::isfinite(marginShare) ? std::clamp(marginShare, 0.0F, 0.5F) : 0.0F;
  const float margin = std::min(width, height) * share;
  float left         = extent(chrome.left) + margin;
  float right        = extent(chrome.right) + margin;
  float bottom       = extent(chrome.bottom) + margin;
  float top          = extent(chrome.top) + margin;
  if (left + right > width && left + right > 0.0F) {
    const float ratio = width / (left + right);
    left *= ratio;
    right *= ratio;
  }
  if (bottom + top > height && bottom + top > 0.0F) {
    const float ratio = height / (bottom + top);
    bottom *= ratio;
    top *= ratio;
  }
  return {left, bottom, std::max(0.0F, width - left - right),
          std::max(0.0F, height - bottom - top)};
}
Rect UiMetrics::safeArea() const {
  const auto area = pixelSafeArea();
  return {logical(area.left), logical(area.bottom), logical(area.width),
          logical(area.height)};
}
Rect UiMetrics::rounded(Rect pixels) const {
  const auto rect    = validRect(pixels);
  const float left   = std::round(rect.left);
  const float bottom = std::round(rect.bottom);
  return {left, bottom,
          std::max(0.0F, std::round(rect.left + rect.width) - left),
          std::max(0.0F, std::round(rect.bottom + rect.height) - bottom)};
}
InputArea toInputArea(Rect pixels, int screenHeight) {
  const auto rect = UiMetrics{}.rounded(pixels);
  return {static_cast<int>(rect.left),
          screenHeight - static_cast<int>(rect.bottom + rect.height),
          static_cast<int>(rect.width), static_cast<int>(rect.height)};
}
Rect clampToSafeArea(Rect pixels, Rect safeArea) {
  auto rect       = validRect(pixels);
  const auto safe = validRect(safeArea);
  rect.width      = std::min(rect.width, safe.width);
  rect.height     = std::min(rect.height, safe.height);
  rect.left =
      std::clamp(rect.left, safe.left, safe.left + safe.width - rect.width);
  rect.bottom = std::clamp(rect.bottom, safe.bottom,
                           safe.bottom + safe.height - rect.height);
  return rect;
}
Rect placeNear(Rect anchor, float width, float height, Rect safeArea,
               float gap) {
  const auto safe     = validRect(safeArea);
  const auto at       = validRect(anchor);
  const float spacing = extent(gap);
  Rect placed{at.left, at.bottom - extent(height) - spacing, extent(width),
              extent(height)};
  if (placed.bottom < safe.bottom &&
      at.bottom + at.height + spacing + placed.height <=
          safe.bottom + safe.height) {
    placed.bottom = at.bottom + at.height + spacing;
  }
  return clampToSafeArea(placed, safe);
}
} // namespace gleditor::ui
