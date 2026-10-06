#ifndef GLEDITOR_UI_METRICS_HPP
#define GLEDITOR_UI_METRICS_HPP

#include <gleditor/ui/input_event.hpp>
#include <gleditor/ui/rect.hpp>
#include <gleditor/ui/theme.hpp>

namespace gleditor {
/// Window-pixel bands occupied by scene chrome.
struct ScreenInsets {
  float top{}, bottom{}, left{}, right{};
  bool operator==(const ScreenInsets &) const = default;
};
namespace ui {
/// Content scale is the platform's pixel/window ratio. User scale changes UI
/// lengths; font scale changes typography independently. Layout uses pixels.
struct UiMetrics {
  float contentScale{1.0F};
  float userScale{1.0F};
  float fontScale{1.0F};
  /// Canvas pixel extents; chrome uses the same coordinates.
  int screenWidth{}, screenHeight{};
  ScreenInsets chrome;
  float marginShare{kSafeMarginShare};
  [[nodiscard]] float scale() const;
  [[nodiscard]] float px(float logical) const;
  [[nodiscard]] float logical(float pixels) const;
  [[nodiscard]] float fontPixels(FontRole, const Theme &) const;
  [[nodiscard]] std::string fontDescription(FontRole, const Theme &) const;
  /// Safe area converted to logical units.
  [[nodiscard]] Rect safeArea() const;
  /// Safe area in Canvas pixels, for layout and drawing.
  [[nodiscard]] Rect pixelSafeArea() const;
  /// Round edges, so adjacent rectangles keep the same shared boundary.
  [[nodiscard]] Rect rounded(Rect pixels) const;
  bool operator==(const UiMetrics &) const = default;
};
[[nodiscard]] InputArea toInputArea(Rect pixels, int screenHeight);
[[nodiscard]] Rect clampToSafeArea(Rect pixels, Rect safeArea);
[[nodiscard]] Rect placeNear(Rect anchor, float width, float height,
                             Rect safeArea, float gap = 0.0F);
} // namespace ui
} // namespace gleditor
#endif
