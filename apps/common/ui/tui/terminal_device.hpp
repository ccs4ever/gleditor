/**
 * @file terminal_device.hpp
 * @brief Terminal device abstraction with ncursesw driver and headless virtual
 * buffer. (design/terminal-ui-architecture.md §3, Proposal P22.1).
 */
#ifndef COMMON_UI_TUI_TERMINAL_DEVICE_HPP
#define COMMON_UI_TUI_TERMINAL_DEVICE_HPP

#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "common/ui/tui/screen_buffer.hpp"
#include "common/ui/tui/tui_types.hpp"

namespace xanadu::ui::tui {

class TerminalDevice {
public:
  struct Options {
    bool forceHeadless{false};
    int defaultWidth{120};
    int defaultHeight{30};
  };

  TerminalDevice();
  explicit TerminalDevice(Options options);
  ~TerminalDevice();

  TerminalDevice(const TerminalDevice &)            = delete;
  TerminalDevice &operator=(const TerminalDevice &) = delete;
  TerminalDevice(TerminalDevice &&) noexcept;
  TerminalDevice &operator=(TerminalDevice &&) noexcept;

  [[nodiscard]] bool isHeadless() const noexcept { return headless_; }
  [[nodiscard]] TerminalDimensions dimensions() const;
  void resize(int width, int height);

  void clear(Style style = {});
  void print(int x, int y, std::string_view text, Style style = {});
  void drawBox(int x, int y, int w, int h,
               BorderStyle border = BorderStyle::UnicodeRounded,
               Style style = {}, std::string_view title = {});
  void drawHLine(int x, int y, int w,
                 BorderStyle border = BorderStyle::UnicodeRounded,
                 Style style        = {});
  void drawVLine(int x, int y, int h,
                 BorderStyle border = BorderStyle::UnicodeRounded,
                 Style style        = {});
  void refresh();

  [[nodiscard]] std::optional<KeyEvent> pollInput();
  void injectInput(const KeyEvent &event);
  void injectInput(std::string_view text);
  [[nodiscard]] bool hasPendingInput() const noexcept;
  [[nodiscard]] bool isEof() const noexcept { return eof_; }

  [[nodiscard]] const VirtualScreenBuffer &screen() const noexcept {
    return screen_;
  }
  [[nodiscard]] VirtualScreenBuffer &screen() noexcept { return screen_; }

  void shutdown();

private:
  struct CursesState;
  std::unique_ptr<CursesState> cursesState_;
  Options options_;
  bool headless_{true};
  bool eof_{false};
  VirtualScreenBuffer screen_;
  std::deque<KeyEvent> inputQueue_;
};

} // namespace xanadu::ui::tui

#endif // COMMON_UI_TUI_TERMINAL_DEVICE_HPP
