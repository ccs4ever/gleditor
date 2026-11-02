/**
 * @file screen_buffer.hpp
 * @brief Virtual screen buffer for headless testing and terminal diffing.
 * (design/terminal-ui-architecture.md §4).
 */
#ifndef COMMON_UI_TUI_SCREEN_BUFFER_HPP
#define COMMON_UI_TUI_SCREEN_BUFFER_HPP

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/ui/tui/tui_types.hpp"

namespace xanadu::ui::tui {

struct ScreenCell {
  char32_t codepoint{U' '};
  char ch{' '};
  std::uint8_t width{
      1}; // 0 for combining/continuation, 1 for normal, 2 for wide
  Style style{};

  bool operator==(const ScreenCell &) const = default;
};

class VirtualScreenBuffer {
public:
  VirtualScreenBuffer(int width = 120, int height = 30);

  void resize(int width, int height);
  void clear(Style style = {});

  void put(int x, int y, char32_t cp, Style style = {});
  void putChar(int x, int y, char ch, Style style = {}) {
    put(x, y, static_cast<char32_t>(static_cast<unsigned char>(ch)), style);
  }
  void putString(int x, int y, std::string_view text, Style style = {});
  void drawBox(int x, int y, int w, int h,
               BorderStyle border = BorderStyle::UnicodeRounded,
               Style style = {}, std::string_view title = {});
  void drawHLine(int x, int y, int w,
                 BorderStyle border = BorderStyle::UnicodeRounded,
                 Style style        = {});
  void drawVLine(int x, int y, int h,
                 BorderStyle border = BorderStyle::UnicodeRounded,
                 Style style        = {});
  void fillRect(int x, int y, int w, int h, char32_t fillChar = U' ',
                Style style = {});

  [[nodiscard]] const ScreenCell &at(int x, int y) const;
  [[nodiscard]] ScreenCell &at(int x, int y);
  [[nodiscard]] std::optional<ScreenCell> cellAt(int x, int y) const;
  [[nodiscard]] std::span<const ScreenCell> row(int y) const;

  [[nodiscard]] int width() const noexcept { return width_; }
  [[nodiscard]] int height() const noexcept { return height_; }

  [[nodiscard]] bool isDirty() const noexcept { return dirty_; }
  void clearDirty() noexcept { dirty_ = false; }
  void markDirty() noexcept { dirty_ = true; }

  [[nodiscard]] std::string line(int y) const;
  [[nodiscard]] std::string dump() const;
  [[nodiscard]] bool contains(std::string_view text) const;
  [[nodiscard]] std::uint64_t frameCount() const noexcept {
    return frameCount_;
  }
  void markFrame() noexcept {
    frameCount_++;
    dirty_ = false;
  }

private:
  int width_{120};
  int height_{30};
  std::vector<ScreenCell> grid_; // Flat contiguous vector: y * width_ + x
  bool dirty_{true};
  std::uint64_t frameCount_{0};
};

} // namespace xanadu::ui::tui

#endif // COMMON_UI_TUI_SCREEN_BUFFER_HPP
