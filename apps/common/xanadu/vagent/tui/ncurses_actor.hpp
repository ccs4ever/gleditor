/**
 * @file ncurses_actor.hpp
 * @brief Native actor interface for ncursesw with virtual mock terminal buffer.
 */
#ifndef COMMON_XANADU_VAGENT_TUI_NCURSES_ACTOR_HPP
#define COMMON_XANADU_VAGENT_TUI_NCURSES_ACTOR_HPP

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "common/ui/tui/screen_buffer.hpp"
#include "common/ui/tui/terminal_device.hpp"
#include "common/ui/tui/tui_types.hpp"
#include "common/xanadu/identity/identity_layout.hpp"

namespace xanadu::vagent::tui {

// Re-export common TUI types in vagent::tui namespace for backward
// compatibility
using KeyCode             = xanadu::ui::tui::KeyCode;
using KeyEvent            = xanadu::ui::tui::KeyEvent;
using Color               = xanadu::ui::tui::Color;
using Style               = xanadu::ui::tui::Style;
using ScreenCell          = xanadu::ui::tui::ScreenCell;
using VirtualScreenBuffer = xanadu::ui::tui::VirtualScreenBuffer;
using TerminalDimensions  = xanadu::ui::tui::TerminalDimensions;
using BorderStyle         = xanadu::ui::tui::BorderStyle;

class NCursesActor {
public:
  struct Options {
    bool forceHeadless{false};
    int defaultWidth{120};
    int defaultHeight{30};
    std::optional<identity::ActorPid> pid;
  };

  NCursesActor();
  explicit NCursesActor(Options options);
  ~NCursesActor();

  NCursesActor(const NCursesActor &)            = delete;
  NCursesActor &operator=(const NCursesActor &) = delete;
  NCursesActor(NCursesActor &&) noexcept;
  NCursesActor &operator=(NCursesActor &&) noexcept;

  [[nodiscard]] const identity::ActorPid &pid() const noexcept { return pid_; }
  void setPid(identity::ActorPid pid) { pid_ = std::move(pid); }

  [[nodiscard]] bool isHeadless() const noexcept {
    return device_.isHeadless();
  }
  [[nodiscard]] TerminalDimensions dimensions() const {
    return device_.dimensions();
  }

  void clear(Style style = {}) { device_.clear(style); }
  void print(int x, int y, std::string_view text, Style style = {}) {
    device_.print(x, y, text, style);
  }
  void drawBox(int x, int y, int w, int h,
               BorderStyle border = BorderStyle::UnicodeRounded,
               Style style = {}, std::string_view title = "") {
    device_.drawBox(x, y, w, h, border, style, title);
  }
  void drawBox(int x, int y, int w, int h, std::string_view title,
               Style style = {}) {
    device_.drawBox(x, y, w, h, BorderStyle::UnicodeRounded, style, title);
  }
  void drawHLine(int x, int y, int w,
                 BorderStyle border = BorderStyle::UnicodeRounded,
                 Style style        = {}) {
    device_.drawHLine(x, y, w, border, style);
  }
  void drawHLine(int x, int y, int w, Style style) {
    device_.drawHLine(x, y, w, BorderStyle::UnicodeRounded, style);
  }
  void drawVLine(int x, int y, int h,
                 BorderStyle border = BorderStyle::UnicodeRounded,
                 Style style        = {}) {
    device_.drawVLine(x, y, h, border, style);
  }
  void drawVLine(int x, int y, int h, Style style) {
    device_.drawVLine(x, y, h, BorderStyle::UnicodeRounded, style);
  }
  void refresh() { device_.refresh(); }

  [[nodiscard]] std::optional<KeyEvent> pollInput() {
    return device_.pollInput();
  }
  void injectInput(const KeyEvent &event) { device_.injectInput(event); }
  void injectInput(std::string_view text) { device_.injectInput(text); }
  [[nodiscard]] bool hasPendingInput() const noexcept {
    return device_.hasPendingInput();
  }
  [[nodiscard]] bool isEof() const noexcept { return device_.isEof(); }

  [[nodiscard]] const VirtualScreenBuffer &screen() const noexcept {
    return device_.screen();
  }
  [[nodiscard]] VirtualScreenBuffer &screen() noexcept {
    return device_.screen();
  }

  [[nodiscard]] xanadu::ui::tui::TerminalDevice &device() noexcept {
    return device_;
  }
  [[nodiscard]] const xanadu::ui::tui::TerminalDevice &device() const noexcept {
    return device_;
  }

  void shutdown() { device_.shutdown(); }

private:
  Options options_;
  identity::ActorPid pid_{};
  xanadu::ui::tui::TerminalDevice device_;
};

} // namespace xanadu::vagent::tui

#endif // COMMON_XANADU_VAGENT_TUI_NCURSES_ACTOR_HPP
