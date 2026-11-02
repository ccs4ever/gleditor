/**
 * @file tui_types.hpp
 * @brief Value types of the terminal UI foundation: colour, style, key events
 * and dimensions (design/terminal-ui-architecture.md §3).
 */
#ifndef COMMON_UI_TUI_TUI_TYPES_HPP
#define COMMON_UI_TUI_TUI_TYPES_HPP

#include <cstdint>

namespace xanadu::ui::tui {

struct TerminalDimensions {
  int width{120};
  int height{30};

  bool operator==(const TerminalDimensions &) const = default;
};

/// How a box is drawn. The unicode styles need a UTF-8 locale; the device
/// quietly degrades them to Ascii where there is none (Invariant T7).
enum class BorderStyle : std::uint8_t {
  Ascii,         // +, -, |
  UnicodeSingle, // ┌ ┐ └ ┘ ─ │
  UnicodeRounded // ╭ ╮ ╰ ╯ ─ │
};

/// The sixteen-colour ANSI palette, with the terminal's own background
/// preserved by `Default` so dark and light themes both stay legible.
enum class Color : std::uint8_t {
  Default,
  Black,
  Red,
  Green,
  Yellow,
  Blue,
  Magenta,
  Cyan,
  White
};

struct Style {
  Color fg{Color::Default};
  Color bg{Color::Default};
  bool bold{false};
  bool dim{false};
  bool underline{false};
  bool reverse{false};

  bool operator==(const Style &) const = default;
};

enum class KeyCode : std::uint8_t {
  None,
  Char,
  Enter,
  Backspace,
  Delete,
  Tab,
  Backtab,
  Escape,
  Up,
  Down,
  Left,
  Right,
  PageUp,
  PageDown,
  Home,
  End,
  F1,
  F2,
  F3,
  F4,
  F5,
  F6,
  F7,
  F8,
  F9,
  F10,
  F11,
  F12,
  /// The terminal changed size; query `TerminalDevice::dimensions()`.
  Resize,
  /// A left click at (`mouseX`, `mouseY`) in screen cells.
  MouseClick,
  MouseScrollUp,
  MouseScrollDown
};

struct KeyEvent {
  KeyCode code{KeyCode::None};
  char32_t codepoint{0};
  bool shift{false};
  bool ctrl{false};
  bool alt{false};
  /// Only meaningful for the mouse codes.
  int mouseX{0};
  int mouseY{0};
  char ch{'\0'};

  [[nodiscard]] constexpr char charValue() const noexcept {
    return ch != '\0' ? ch
                      : (codepoint < 128 ? static_cast<char>(codepoint) : '\0');
  }

  /// What a byte typed at a terminal means when no escape sequence is involved.
  [[nodiscard]] static KeyEvent fromChar(char c) {
    if (c == '\n' || c == '\r')
      return {KeyCode::Enter, U'\n', false, false, false, 0, 0, '\n'};
    if (c == '\t')
      return {KeyCode::Tab, U'\t', false, false, false, 0, 0, '\t'};
    if (c == 27) return {KeyCode::Escape, 0, false, false, false, 0, 0, '\0'};
    if (c == 127 || c == 8)
      return {KeyCode::Backspace, 0, false, false, false, 0, 0, '\0'};
    return {KeyCode::Char,
            static_cast<char32_t>(static_cast<unsigned char>(c)),
            false,
            false,
            false,
            0,
            0,
            c};
  }
  [[nodiscard]] static KeyEvent character(char32_t cp) {
    return {KeyCode::Char, cp, false, false,
            false,         0,  0,     static_cast<char>(cp < 128 ? cp : '\0')};
  }
  [[nodiscard]] static KeyEvent click(int x, int y) {
    KeyEvent event{KeyCode::MouseClick};
    event.mouseX = x;
    event.mouseY = y;
    return event;
  }

  bool operator==(const KeyEvent &) const = default;
};

} // namespace xanadu::ui::tui

#endif // COMMON_UI_TUI_TUI_TYPES_HPP
