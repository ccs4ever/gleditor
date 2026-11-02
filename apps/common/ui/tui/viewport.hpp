/**
 * @file viewport.hpp
 * @brief Viewport virtualization helper for bounded-height scrollable lists.
 * (design/terminal-ui-architecture.md §7.2, Proposal P22.4).
 */
#ifndef COMMON_UI_TUI_VIEWPORT_HPP
#define COMMON_UI_TUI_VIEWPORT_HPP

#include <algorithm>
#include <utility>

namespace xanadu::ui::tui {

struct Viewport {
  int topIndex{0};
  int visibleRows{10};
  int totalRows{0};

  void clamp() noexcept {
    if (visibleRows <= 0) visibleRows = 1;
    if (totalRows <= visibleRows) {
      topIndex = 0;
      return;
    }
    const int maxTop = totalRows - visibleRows;
    topIndex         = std::clamp(topIndex, 0, maxTop);
  }

  void scrollUp(int delta = 1) noexcept {
    topIndex -= delta;
    clamp();
  }

  void scrollDown(int delta = 1) noexcept {
    topIndex += delta;
    clamp();
  }

  void pageUp() noexcept { scrollUp(std::max(1, visibleRows - 1)); }

  void pageDown() noexcept { scrollDown(std::max(1, visibleRows - 1)); }

  void scrollTo(int index) noexcept {
    if (index < topIndex) {
      topIndex = index;
    } else if (index >= topIndex + visibleRows) {
      topIndex = index - visibleRows + 1;
    }
    clamp();
  }

  [[nodiscard]] bool canScrollUp() const noexcept { return topIndex > 0; }

  [[nodiscard]] bool canScrollDown() const noexcept {
    return totalRows > visibleRows && topIndex + visibleRows < totalRows;
  }

  /// Calculates scroll bar thumb position (thumbOffset, thumbHeight) along a
  /// vertical track
  [[nodiscard]] std::pair<int, int>
  scrollThumb(int trackHeight) const noexcept {
    if (trackHeight <= 0 || totalRows <= 0) return {0, 0};
    if (totalRows <= visibleRows) return {0, trackHeight};

    const int thumbHeight =
        std::max(1, (visibleRows * trackHeight) / totalRows);
    const int maxOffset   = trackHeight - thumbHeight;
    const int maxTop      = totalRows - visibleRows;
    const int thumbOffset = maxTop > 0 ? (topIndex * maxOffset) / maxTop : 0;

    return {thumbOffset, thumbHeight};
  }
};

} // namespace xanadu::ui::tui

#endif // COMMON_UI_TUI_VIEWPORT_HPP
