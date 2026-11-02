/**
 * @file ncurses_actor.cpp
 * @brief Native actor implementation interfacing with ncursesw and virtual
 * buffer.
 */
#include "common/xanadu/vagent/tui/ncurses_actor.hpp"

#include <algorithm>
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

namespace xanadu::vagent::tui {

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

// -- VirtualScreenBuffer ------------------------------------------------------

VirtualScreenBuffer::VirtualScreenBuffer(int width, int height)
    : width_(width), height_(height),
      grid_(height, std::vector<ScreenCell>(width, ScreenCell{' ', {}})) {}

void VirtualScreenBuffer::resize(int width, int height) {
  width_  = std::max(1, width);
  height_ = std::max(1, height);
  grid_.assign(height_, std::vector<ScreenCell>(width_, ScreenCell{' ', {}}));
}

void VirtualScreenBuffer::clear() {
  for (auto &row : grid_) {
    for (auto &cell : row) {
      cell = ScreenCell{' ', {}};
    }
  }
}

void VirtualScreenBuffer::putChar(int x, int y, char ch, Style style) {
  if (y < 0 || y >= height_ || x < 0 || x >= width_) return;
  grid_[y][x] = ScreenCell{ch, style};
}

void VirtualScreenBuffer::putString(int x, int y, std::string_view str,
                                    Style style) {
  if (y < 0 || y >= height_ || x >= width_) return;
  int curX = x;
  for (char ch : str) {
    if (curX >= width_) break;
    if (curX >= 0) {
      grid_[y][curX] = ScreenCell{ch, style};
    }
    ++curX;
  }
}

std::optional<ScreenCell> VirtualScreenBuffer::at(int x, int y) const {
  if (y < 0 || y >= height_ || x < 0 || x >= width_) return std::nullopt;
  return grid_[y][x];
}

std::string VirtualScreenBuffer::line(int y) const {
  if (y < 0 || y >= height_) return {};
  std::string s;
  s.reserve(width_);
  for (const auto &cell : grid_[y]) {
    s.push_back(cell.ch);
  }
  // Trim trailing spaces for cleaner assertions
  while (!s.empty() && s.back() == ' ') {
    s.pop_back();
  }
  return s;
}

std::string VirtualScreenBuffer::dump() const {
  std::string result;
  for (int y = 0; y < height_; ++y) {
    result += line(y);
    result += '\n';
  }
  return result;
}

bool VirtualScreenBuffer::contains(std::string_view text) const {
  if (text.empty()) return true;
  for (int y = 0; y < height_; ++y) {
    std::string l;
    l.reserve(width_);
    for (const auto &cell : grid_[y]) l.push_back(cell.ch);
    if (l.find(text) != std::string::npos) return true;
  }
  return false;
}

void VirtualScreenBuffer::markFrame() { ++frameCount_; }

// -- NCursesActor -------------------------------------------------------------

struct NCursesActor::CursesState {
  WINDOW *win{nullptr};
  bool initialized{false};
};

NCursesActor::NCursesActor() : NCursesActor(Options{}) {}

NCursesActor::NCursesActor(Options options)
    : options_(options), headless_(probeHeadless(options.forceHeadless)),
      screen_(options.defaultWidth, options.defaultHeight) {
  if (!headless_) {
    cursesState_      = std::make_unique<CursesState>();
    cursesState_->win = initscr();
    if (cursesState_->win) {
      cursesState_->initialized = true;
      cbreak();
      noecho();
      keypad(stdscr, TRUE);
      nodelay(stdscr, TRUE);
      curs_set(0);

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

NCursesActor::~NCursesActor() { shutdown(); }

NCursesActor::NCursesActor(NCursesActor &&) noexcept            = default;
NCursesActor &NCursesActor::operator=(NCursesActor &&) noexcept = default;

bool NCursesActor::isHeadless() const noexcept { return headless_; }

TerminalDimensions NCursesActor::dimensions() const {
  return {screen_.width(), screen_.height()};
}

void NCursesActor::clear() {
  screen_.clear();
  if (!headless_ && cursesState_ && cursesState_->initialized) {
    wclear(stdscr);
  }
}

void NCursesActor::print(int x, int y, std::string_view text, Style style) {
  screen_.putString(x, y, text, style);
  if (!headless_ && cursesState_ && cursesState_->initialized) {
    int attrs = toCursesAttrs(style);
    wattron(stdscr, attrs);
    mvwaddnstr(stdscr, y, x, text.data(), static_cast<int>(text.size()));
    wattroff(stdscr, attrs);
  }
}

void NCursesActor::drawBox(int x, int y, int w, int h, std::string_view title,
                           Style style) {
  if (w < 2 || h < 2) return;

  // Corners
  print(x, y, "+", style);
  print(x + w - 1, y, "+", style);
  print(x, y + h - 1, "+", style);
  print(x + w - 1, y + h - 1, "+", style);

  // Horizontal borders
  for (int i = 1; i < w - 1; ++i) {
    print(x + i, y, "-", style);
    print(x + i, y + h - 1, "-", style);
  }

  // Vertical borders
  for (int j = 1; j < h - 1; ++j) {
    print(x, y + j, "|", style);
    print(x + w - 1, y + j, "|", style);
  }

  // Title
  if (!title.empty() && w > 4) {
    std::string titled = " " + std::string(title) + " ";
    int titleLen       = static_cast<int>(titled.size());
    if (titleLen > w - 2) {
      titled   = titled.substr(0, static_cast<std::size_t>(w - 2));
      titleLen = w - 2;
    }
    Style titleStyle = style;
    titleStyle.bold  = true;
    print(x + 2, y, titled, titleStyle);
  }
}

void NCursesActor::drawHLine(int x, int y, int w, Style style) {
  for (int i = 0; i < w; ++i) {
    print(x + i, y, "-", style);
  }
}

void NCursesActor::drawVLine(int x, int y, int h, Style style) {
  for (int j = 0; j < h; ++j) {
    print(x, y + j, "|", style);
  }
}

void NCursesActor::refresh() {
  screen_.markFrame();
  if (!headless_ && cursesState_ && cursesState_->initialized) {
    wrefresh(stdscr);
  }
}

std::optional<KeyEvent> NCursesActor::pollInput() {
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
    return KeyEvent{KeyCode::Up, '\0'};
  case KEY_DOWN:
    return KeyEvent{KeyCode::Down, '\0'};
  case KEY_LEFT:
    return KeyEvent{KeyCode::Left, '\0'};
  case KEY_RIGHT:
    return KeyEvent{KeyCode::Right, '\0'};
  case KEY_PPAGE:
    return KeyEvent{KeyCode::PageUp, '\0'};
  case KEY_NPAGE:
    return KeyEvent{KeyCode::PageDown, '\0'};
  case KEY_HOME:
    return KeyEvent{KeyCode::Home, '\0'};
  case KEY_END:
    return KeyEvent{KeyCode::End, '\0'};
  case KEY_DC:
    return KeyEvent{KeyCode::Delete, '\0'};
  case KEY_BACKSPACE:
  case 127:
  case 8:
    return KeyEvent{KeyCode::Backspace, '\0'};
  case 10:
  case 13:
    return KeyEvent{KeyCode::Enter, '\n'};
  case 27:
    return KeyEvent{KeyCode::Escape, '\0'};
  case 9:
    return KeyEvent{KeyCode::Tab, '\t'};
  case KEY_F(1):
    return KeyEvent{KeyCode::F1, '\0'};
  case KEY_F(2):
    return KeyEvent{KeyCode::F2, '\0'};
  case KEY_F(3):
    return KeyEvent{KeyCode::F3, '\0'};
  case KEY_F(4):
    return KeyEvent{KeyCode::F4, '\0'};
  case KEY_F(5):
    return KeyEvent{KeyCode::F5, '\0'};
  case KEY_F(6):
    return KeyEvent{KeyCode::F6, '\0'};
  case KEY_F(7):
    return KeyEvent{KeyCode::F7, '\0'};
  case KEY_F(8):
    return KeyEvent{KeyCode::F8, '\0'};
  case KEY_F(9):
    return KeyEvent{KeyCode::F9, '\0'};
  case KEY_F(10):
    return KeyEvent{KeyCode::F10, '\0'};
  case KEY_F(11):
    return KeyEvent{KeyCode::F11, '\0'};
  case KEY_F(12):
    return KeyEvent{KeyCode::F12, '\0'};
  default:
    if (ch >= 32 && ch <= 126) {
      return KeyEvent{KeyCode::Char, static_cast<char>(ch)};
    }
    return std::nullopt;
  }
}

void NCursesActor::injectInput(const KeyEvent &event) {
  inputQueue_.push_back(event);
}

void NCursesActor::injectInput(std::string_view text) {
  for (char c : text) {
    inputQueue_.push_back(KeyEvent::fromChar(c));
  }
}

bool NCursesActor::hasPendingInput() const noexcept {
  return !inputQueue_.empty();
}

void NCursesActor::shutdown() {
  if (!headless_ && cursesState_ && cursesState_->initialized) {
    endwin();
    cursesState_->initialized = false;
  }
}

} // namespace xanadu::vagent::tui
