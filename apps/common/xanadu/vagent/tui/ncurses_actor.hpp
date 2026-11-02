/**
 * @file ncurses_actor.hpp
 * @brief Native actor interface for ncursesw with virtual mock terminal buffer.
 */
#ifndef COMMON_XANADU_VAGENT_TUI_NCURSES_ACTOR_HPP
#define COMMON_XANADU_VAGENT_TUI_NCURSES_ACTOR_HPP

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/identity/identity_layout.hpp"

namespace xanadu::vagent::tui {

enum class KeyCode {
  None,
  Char,
  Enter,
  Backspace,
  Tab,
  Escape,
  Up,
  Down,
  Left,
  Right,
  PageUp,
  PageDown,
  Home,
  End,
  Delete,
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
  F12
};

struct KeyEvent {
  KeyCode code{KeyCode::None};
  char ch{'\0'};
  bool shift{false};
  bool ctrl{false};
  bool alt{false};

  static KeyEvent fromChar(char c) {
    if (c == '\n' || c == '\r') return {KeyCode::Enter, '\n'};
    if (c == '\t') return {KeyCode::Tab, '\t'};
    if (c == 27) return {KeyCode::Escape, '\0'};
    if (c == 127 || c == 8) return {KeyCode::Backspace, '\0'};
    return {KeyCode::Char, c};
  }

  bool operator==(const KeyEvent &) const = default;
};

enum class Color {
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

struct ScreenCell {
  char ch{' '};
  Style style{};

  bool operator==(const ScreenCell &) const = default;
};

class VirtualScreenBuffer {
public:
  VirtualScreenBuffer(int width = 120, int height = 30);

  void resize(int width, int height);
  void clear();
  void putChar(int x, int y, char ch, Style style = {});
  void putString(int x, int y, std::string_view str, Style style = {});

  [[nodiscard]] int width() const noexcept { return width_; }
  [[nodiscard]] int height() const noexcept { return height_; }
  [[nodiscard]] std::optional<ScreenCell> at(int x, int y) const;
  [[nodiscard]] std::string line(int y) const;
  [[nodiscard]] std::string dump() const;
  [[nodiscard]] bool contains(std::string_view text) const;
  [[nodiscard]] std::size_t frameCount() const noexcept { return frameCount_; }
  void markFrame();

private:
  int width_{120};
  int height_{30};
  std::vector<std::vector<ScreenCell>> grid_;
  std::size_t frameCount_{0};
};

struct TerminalDimensions {
  int width{120};
  int height{30};
};

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

  [[nodiscard]] bool isHeadless() const noexcept;
  [[nodiscard]] TerminalDimensions dimensions() const;

  void clear();
  void print(int x, int y, std::string_view text, Style style = {});
  void drawBox(int x, int y, int w, int h, std::string_view title = "",
               Style style = {});
  void drawHLine(int x, int y, int w, Style style = {});
  void drawVLine(int x, int y, int h, Style style = {});
  void refresh();

  [[nodiscard]] std::optional<KeyEvent> pollInput();
  void injectInput(const KeyEvent &event);
  void injectInput(std::string_view text);
  [[nodiscard]] bool hasPendingInput() const noexcept;

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
  identity::ActorPid pid_{};
  VirtualScreenBuffer screen_;
  std::deque<KeyEvent> inputQueue_;
};

} // namespace xanadu::vagent::tui

#endif // COMMON_XANADU_VAGENT_TUI_NCURSES_ACTOR_HPP
