#ifndef GLEDITOR_UI_INPUT_EVENT_HPP
#define GLEDITOR_UI_INPUT_EVENT_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <variant>

namespace gleditor {
enum class Key : std::uint8_t {
  Escape,
  Return,
  Tab,
  Backspace,
  Delete,
  Left,
  Right,
  Up,
  Down,
  Home,
  End,
  PageUp,
  PageDown,
  Space,
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
  Unknown
};
enum class KeyMods : std::uint16_t {
  None  = 0,
  Shift = 1U << 0U,
  Ctrl  = 1U << 1U,
  Alt   = 1U << 2U
};
[[nodiscard]] inline bool held(KeyMods mods, KeyMods which) {
  return (static_cast<std::uint16_t>(mods) &
          static_cast<std::uint16_t>(which)) != 0;
}
struct InputArea {
  int x{}, y{}, width{}, height{};
  bool operator==(const InputArea &) const = default;
};
namespace ui {
struct KeyEvent {
  Key key{Key::Unknown};
  KeyMods mods{KeyMods::None};
  std::optional<char32_t> codepoint;
};
enum class PointerPhase : std::uint8_t { Press, Move, Release, Wheel, Cancel };
struct PointerEvent {
  PointerPhase phase{PointerPhase::Move};
  int button{};
  float x{}, y{}, deltaX{}, deltaY{};
  std::uint32_t pointerId{};
};
struct TextEvent {
  std::string text;
};
struct FocusLostEvent {};
using InputEvent =
    std::variant<KeyEvent, TextEvent, PointerEvent, FocusLostEvent>;
} // namespace ui
} // namespace gleditor
#endif
