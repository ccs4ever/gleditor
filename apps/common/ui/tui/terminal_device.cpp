/**
 * @file terminal_device.cpp
 * @brief Terminal device implementation with ncursesw driver and headless
 * virtual buffer.
 */
#include "common/ui/tui/terminal_device.hpp"

#include <algorithm>
#include <clocale>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include <curses.h>

// Undefine conflicting ncurses macros
#undef erase
#undef move
#undef refresh
#undef clear
#undef timeout

namespace xanadu::ui::tui {

namespace {

bool probeHeadless(bool forceHeadless) {
  if (forceHeadless) return true;
  if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return true;
  const char *term = std::getenv("TERM");
  if (!term || std::string_view(term) == "dumb" ||
      std::string_view(term) == "offscreen") {
    return true;
  }
  return false;
}

int colorPairId(Color fg) {
  switch (fg) {
  case Color::Red:
    return 1;
  case Color::Green:
    return 2;
  case Color::Yellow:
    return 3;
  case Color::Blue:
    return 4;
  case Color::Magenta:
    return 5;
  case Color::Cyan:
    return 6;
  case Color::White:
    return 7;
  case Color::Black:
    return 8;
  default:
    return 0;
  }
}

int toCursesAttrs(Style style) {
  int attrs = 0;
  if (style.bold) attrs |= A_BOLD;
  if (style.dim) attrs |= A_DIM;
  if (style.underline) attrs |= A_UNDERLINE;
  if (style.reverse) attrs |= A_REVERSE;
  int pair = colorPairId(style.fg);
  if (pair > 0) {
    attrs |= COLOR_PAIR(pair);
  }
  return attrs;
}

} // namespace

struct TerminalDevice::CursesState {
  WINDOW *win{nullptr};
  bool initialized{false};
};

TerminalDevice::TerminalDevice() : TerminalDevice(Options{}) {}

TerminalDevice::TerminalDevice(Options options)
    : options_(options), headless_(probeHeadless(options.forceHeadless)),
      screen_(options.defaultWidth, options.defaultHeight) {
  if (!headless_) {
    std::setlocale(LC_ALL, "");
    cursesState_      = std::make_unique<CursesState>();
    cursesState_->win = initscr();
    if (cursesState_->win) {
      cursesState_->initialized = true;
      cbreak();
      noecho();
      keypad(stdscr, TRUE);
      nodelay(stdscr, TRUE);
      curs_set(0);

      if (std::getenv("ESCDELAY") == nullptr) {
        set_escdelay(25);
      }

      if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(1, COLOR_RED, -1);
        init_pair(2, COLOR_GREEN, -1);
        init_pair(3, COLOR_YELLOW, -1);
        init_pair(4, COLOR_BLUE, -1);
        init_pair(5, COLOR_MAGENTA, -1);
        init_pair(6, COLOR_CYAN, -1);
        init_pair(7, COLOR_WHITE, -1);
        init_pair(8, COLOR_BLACK, -1);
      }

      int rows = 0;
      int cols = 0;
      getmaxyx(stdscr, rows, cols);
      if (rows > 0 && cols > 0) {
        screen_.resize(cols, rows);
      }
    } else {
      headless_ = true;
    }
  }
}

TerminalDevice::~TerminalDevice() { shutdown(); }

TerminalDevice::TerminalDevice(TerminalDevice &&) noexcept            = default;
TerminalDevice &TerminalDevice::operator=(TerminalDevice &&) noexcept = default;

TerminalDimensions TerminalDevice::dimensions() const {
  return {screen_.width(), screen_.height()};
}

void TerminalDevice::resize(int width, int height) {
  screen_.resize(width, height);
}

void TerminalDevice::clear(Style style) {
  screen_.clear(style);
  if (!headless_ && cursesState_ && cursesState_->initialized) {
    wclear(stdscr);
  }
}

void TerminalDevice::print(int x, int y, std::string_view text, Style style) {
  screen_.putString(x, y, text, style);
  if (!headless_ && cursesState_ && cursesState_->initialized) {
    int attrs = toCursesAttrs(style);
    wattron(stdscr, attrs);
    mvwaddnstr(stdscr, y, x, text.data(), static_cast<int>(text.size()));
    wattroff(stdscr, attrs);
  }
}

void TerminalDevice::drawBox(int x, int y, int w, int h, BorderStyle border,
                             Style style, std::string_view title) {
  screen_.drawBox(x, y, w, h, border, style, title);
  if (!headless_ && cursesState_ && cursesState_->initialized) {
    // Sync rendered box region to curses
    for (int row = y; row < y + h; ++row) {
      if (row < 0 || row >= screen_.height()) continue;
      const std::string lineStr = screen_.line(row);
      if (x < static_cast<int>(lineStr.size())) {
        const std::string sub = lineStr.substr(static_cast<std::size_t>(x),
                                               static_cast<std::size_t>(w));
        mvwaddnstr(stdscr, row, x, sub.data(), static_cast<int>(sub.size()));
      }
    }
  }
}

void TerminalDevice::drawHLine(int x, int y, int w, BorderStyle border,
                               Style style) {
  screen_.drawHLine(x, y, w, border, style);
  if (!headless_ && cursesState_ && cursesState_->initialized) {
    int attrs = toCursesAttrs(style);
    wattron(stdscr, attrs);
    for (int col = x; col < x + w; ++col) {
      mvwaddch(stdscr, y, col, border == BorderStyle::Ascii ? '-' : ACS_HLINE);
    }
    wattroff(stdscr, attrs);
  }
}

void TerminalDevice::drawVLine(int x, int y, int h, BorderStyle border,
                               Style style) {
  screen_.drawVLine(x, y, h, border, style);
  if (!headless_ && cursesState_ && cursesState_->initialized) {
    int attrs = toCursesAttrs(style);
    wattron(stdscr, attrs);
    for (int row = y; row < y + h; ++row) {
      mvwaddch(stdscr, row, x, border == BorderStyle::Ascii ? '|' : ACS_VLINE);
    }
    wattroff(stdscr, attrs);
  }
}

void TerminalDevice::refresh() {
  screen_.markFrame();
  if (!headless_ && cursesState_ && cursesState_->initialized) {
    wrefresh(stdscr);
  }
}

std::optional<KeyEvent> TerminalDevice::pollInput() {
  if (!inputQueue_.empty()) {
    auto ev = inputQueue_.front();
    inputQueue_.pop_front();
    return ev;
  }

  if (headless_ || !cursesState_ || !cursesState_->initialized) {
    return std::nullopt;
  }

  int ch = wgetch(stdscr);
  if (ch == ERR) {
    return std::nullopt;
  }

  switch (ch) {
  case KEY_UP:
    return KeyEvent{KeyCode::Up, 0};
  case KEY_DOWN:
    return KeyEvent{KeyCode::Down, 0};
  case KEY_LEFT:
    return KeyEvent{KeyCode::Left, 0};
  case KEY_RIGHT:
    return KeyEvent{KeyCode::Right, 0};
  case KEY_PPAGE:
    return KeyEvent{KeyCode::PageUp, 0};
  case KEY_NPAGE:
    return KeyEvent{KeyCode::PageDown, 0};
  case KEY_HOME:
    return KeyEvent{KeyCode::Home, 0};
  case KEY_END:
    return KeyEvent{KeyCode::End, 0};
  case KEY_DC:
    return KeyEvent{KeyCode::Delete, 0};
  case KEY_BACKSPACE:
  case 127:
  case 8:
    return KeyEvent{KeyCode::Backspace, 0};
  case 10:
  case 13:
    return KeyEvent{KeyCode::Enter, U'\n'};
  case 27: {
    int next = wgetch(stdscr);
    if (next == ERR) {
      return KeyEvent{KeyCode::Escape, 0};
    }
    if (next == '1') return KeyEvent{KeyCode::F1, U'1', false, false, true};
    if (next == '2') return KeyEvent{KeyCode::F2, U'2', false, false, true};
    if (next == '3') return KeyEvent{KeyCode::F3, U'3', false, false, true};
    if (next == 9) return KeyEvent{KeyCode::Tab, U'\t', false, false, true};
    if (next == '[' || next == 'O') {
      int seq = wgetch(stdscr);
      if (seq == 'Z') {
        KeyEvent ev{KeyCode::Tab, U'\t'};
        ev.shift = true;
        return ev;
      }
      if (seq == '9') {
        while (seq != ERR && seq != 'u' && seq != '~') {
          seq = wgetch(stdscr);
        }
        return KeyEvent{KeyCode::Tab, U'\t'};
      }
      if (seq == '1' || seq == '2' || seq == '3') {
        int seq2 = wgetch(stdscr);
        if (seq == '1' && (seq2 == '~' || seq2 == 'P'))
          return KeyEvent{KeyCode::F1, 0};
        if (seq == '2' && (seq2 == '~' || seq2 == 'Q'))
          return KeyEvent{KeyCode::F2, 0};
        if (seq == '3' && (seq2 == '~' || seq2 == 'R'))
          return KeyEvent{KeyCode::F3, 0};
        while (seq2 != ERR && seq2 != '~' && seq2 != 'u' && seq2 != 'z') {
          seq2 = wgetch(stdscr);
        }
      }
      if (seq == 'A') return KeyEvent{KeyCode::Up, 0};
      if (seq == 'B') return KeyEvent{KeyCode::Down, 0};
      if (seq == 'C') return KeyEvent{KeyCode::Right, 0};
      if (seq == 'D') return KeyEvent{KeyCode::Left, 0};
      while (seq != ERR && seq >= 0x20 && seq <= 0x3f) {
        seq = wgetch(stdscr);
      }
    }
    return KeyEvent{KeyCode::Escape, 0};
  }
  case 9:
  case KEY_STAB:
  case KEY_CTAB:
  case KEY_CATAB:
    return KeyEvent{KeyCode::Tab, U'\t'};
  case KEY_BTAB: {
    KeyEvent ev{KeyCode::Tab, U'\t'};
    ev.shift = true;
    return ev;
  }
  case 14: // Ctrl+N
    return KeyEvent{KeyCode::Tab, U'\t'};
  case 16: { // Ctrl+P
    KeyEvent ev{KeyCode::Tab, U'\t'};
    ev.shift = true;
    return ev;
  }
  case KEY_RESIZE: {
    int rows = 0;
    int cols = 0;
    getmaxyx(stdscr, rows, cols);
    if (rows > 0 && cols > 0) {
      screen_.resize(cols, rows);
    }
    return KeyEvent{KeyCode::Resize, 0};
  }
  case KEY_F(1):
    return KeyEvent{KeyCode::F1, 0};
  case KEY_F(2):
    return KeyEvent{KeyCode::F2, 0};
  case KEY_F(3):
    return KeyEvent{KeyCode::F3, 0};
  case KEY_F(4):
    return KeyEvent{KeyCode::F4, 0};
  case KEY_F(5):
    return KeyEvent{KeyCode::F5, 0};
  case KEY_F(6):
    return KeyEvent{KeyCode::F6, 0};
  case KEY_F(7):
    return KeyEvent{KeyCode::F7, 0};
  case KEY_F(8):
    return KeyEvent{KeyCode::F8, 0};
  case KEY_F(9):
    return KeyEvent{KeyCode::F9, 0};
  case KEY_F(10):
    return KeyEvent{KeyCode::F10, 0};
  case KEY_F(11):
    return KeyEvent{KeyCode::F11, 0};
  case KEY_F(12):
    return KeyEvent{KeyCode::F12, 0};
  default:
    if (ch >= 32 && ch <= 126) {
      return KeyEvent{KeyCode::Char, static_cast<char32_t>(ch)};
    }
    return std::nullopt;
  }
}

void TerminalDevice::injectInput(const KeyEvent &event) {
  inputQueue_.push_back(event);
}

void TerminalDevice::injectInput(std::string_view text) {
  for (char c : text) {
    inputQueue_.push_back(KeyEvent::fromChar(c));
  }
}

bool TerminalDevice::hasPendingInput() const noexcept {
  return !inputQueue_.empty();
}

void TerminalDevice::shutdown() {
  if (!headless_ && cursesState_ && cursesState_->initialized) {
    endwin();
    cursesState_->initialized = false;
  }
}

} // namespace xanadu::ui::tui
