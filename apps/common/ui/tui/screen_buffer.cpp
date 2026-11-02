/**
 * @file screen_buffer.cpp
 * @brief Virtual screen buffer implementation with flat vector and UTF-8
 * decoding.
 */
#include "common/ui/tui/screen_buffer.hpp"

#include <algorithm>
#include <stdexcept>

namespace xanadu::ui::tui {

namespace {

int codepointWidth(char32_t cp) noexcept {
  if (cp == 0 || (cp >= 0x0001 && cp <= 0x001F) ||
      (cp >= 0x007F && cp <= 0x009F)) {
    return 0; // Control characters
  }
  // Explicitly single-width box-drawing, block elements, and standard symbols
  if (cp >= 0x2500 && cp <= 0x259F) {
    return 1;
  }
  // East Asian Wide ranges and Emoji ranges
  if ((cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2329 && cp <= 0x232A) ||
      (cp >= 0x2E80 && cp <= 0x303E) || (cp >= 0x3040 && cp <= 0xA4CF) ||
      (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
      (cp >= 0xFE10 && cp <= 0xFE19) || (cp >= 0xFE30 && cp <= 0xFE6F) ||
      (cp >= 0xFF00 && cp <= 0xFF60) || (cp >= 0xFFE0 && cp <= 0xFFE6) ||
      (cp >= 0x1F300 && cp <= 0x1F64F) || (cp >= 0x1F900 && cp <= 0x1F9FF)) {
    return 2;
  }
  return 1;
}

std::string encodeUtf8(char32_t cp) {
  std::string out;
  if (cp <= 0x7F) {
    out.push_back(static_cast<char>(cp));
  } else if (cp <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp <= 0x10FFFF) {
    out.push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back('?');
  }
  return out;
}

struct BoxGlyphs {
  char32_t topLeft;
  char32_t topRight;
  char32_t bottomLeft;
  char32_t bottomRight;
  char32_t horiz;
  char32_t vert;
};

BoxGlyphs getBoxGlyphs(BorderStyle style) noexcept {
  switch (style) {
  case BorderStyle::Ascii:
    return {'+', '+', '+', '+', '-', '|'};
  case BorderStyle::UnicodeSingle:
    return {U'┌', U'┐', U'└', U'┘', U'─', U'│'};
  case BorderStyle::UnicodeRounded:
  default:
    return {U'╭', U'╮', U'╰', U'╯', U'─', U'│'};
  }
}

} // namespace

VirtualScreenBuffer::VirtualScreenBuffer(int width, int height)
    : width_(std::max(1, width)), height_(std::max(1, height)),
      grid_(static_cast<std::size_t>(width_) *
                static_cast<std::size_t>(height_),
            ScreenCell{}) {}

void VirtualScreenBuffer::resize(int width, int height) {
  const int newW = std::max(1, width);
  const int newH = std::max(1, height);
  if (newW == width_ && newH == height_) return;

  std::vector<ScreenCell> newGrid(static_cast<std::size_t>(newW) *
                                      static_cast<std::size_t>(newH),
                                  ScreenCell{});
  const int copyW = std::min(width_, newW);
  const int copyH = std::min(height_, newH);
  for (int y = 0; y < copyH; ++y) {
    for (int x = 0; x < copyW; ++x) {
      newGrid[static_cast<std::size_t>(y * newW + x)] =
          grid_[static_cast<std::size_t>(y * width_ + x)];
    }
  }

  width_  = newW;
  height_ = newH;
  grid_   = std::move(newGrid);
  dirty_  = true;
}

void VirtualScreenBuffer::clear(Style style) {
  ScreenCell cell{U' ', ' ', 1, style};
  std::fill(grid_.begin(), grid_.end(), cell);
  dirty_ = true;
}

void VirtualScreenBuffer::put(int x, int y, char32_t cp, Style style) {
  if (x < 0 || x >= width_ || y < 0 || y >= height_) return;

  const int w        = codepointWidth(cp);
  const auto idx     = static_cast<std::size_t>(y * width_ + x);
  const char asciiCh = static_cast<char>(cp <= 127 ? cp : '?');

  // If replacing continuation cell of a preceding double-width glyph, reset
  // preceding cell
  if (grid_[idx].width == 0 && x > 0) {
    grid_[idx - 1] = ScreenCell{U' ', ' ', 1, style};
  }
  // If replacing primary cell of a double-width glyph, reset following
  // continuation cell
  if (grid_[idx].width == 2 && x + 1 < width_) {
    grid_[idx + 1] = ScreenCell{U' ', ' ', 1, style};
  }

  if (w == 2) {
    if (x + 1 < width_) {
      grid_[idx]     = ScreenCell{cp, asciiCh, 2, style};
      grid_[idx + 1] = ScreenCell{U' ', ' ', 0, style};
    } else {
      grid_[idx] = ScreenCell{U' ', ' ', 1, style};
    }
  } else {
    grid_[idx] = ScreenCell{cp, asciiCh,
                            static_cast<std::uint8_t>(w == 0 ? 0 : 1), style};
  }
  dirty_ = true;
}

void VirtualScreenBuffer::putString(int x, int y, std::string_view text,
                                    Style style) {
  if (y < 0 || y >= height_ || x >= width_) return;

  int curX      = x;
  std::size_t i = 0;
  while (i < text.size() && curX < width_) {
    char32_t cp            = 0;
    const unsigned char c0 = static_cast<unsigned char>(text[i]);
    std::size_t seqLen     = 1;

    if (c0 < 0x80) {
      cp     = c0;
      seqLen = 1;
    } else if ((c0 & 0xE0) == 0xC0 && i + 1 < text.size()) {
      cp =
          ((c0 & 0x1F) << 6) | (static_cast<unsigned char>(text[i + 1]) & 0x3F);
      seqLen = 2;
    } else if ((c0 & 0xF0) == 0xE0 && i + 2 < text.size()) {
      cp     = ((c0 & 0x0F) << 12) |
               ((static_cast<unsigned char>(text[i + 1]) & 0x3F) << 6) |
               (static_cast<unsigned char>(text[i + 2]) & 0x3F);
      seqLen = 3;
    } else if ((c0 & 0xF8) == 0xF0 && i + 3 < text.size()) {
      cp     = ((c0 & 0x07) << 18) |
               ((static_cast<unsigned char>(text[i + 1]) & 0x3F) << 12) |
               ((static_cast<unsigned char>(text[i + 2]) & 0x3F) << 6) |
               (static_cast<unsigned char>(text[i + 3]) & 0x3F);
      seqLen = 4;
    } else {
      cp     = U'?';
      seqLen = 1;
    }
    i += seqLen;

    if (cp == U'\n' || cp == U'\r') break;

    const int w = codepointWidth(cp);
    if (curX >= 0) {
      put(curX, y, cp, style);
    }
    curX += std::max(1, w);
  }
}

void VirtualScreenBuffer::drawBox(int x, int y, int w, int h,
                                  BorderStyle border, Style style,
                                  std::string_view title) {
  if (w <= 0 || h <= 0) return;
  const BoxGlyphs glyphs = getBoxGlyphs(border);

  // Corners
  put(x, y, glyphs.topLeft, style);
  put(x + w - 1, y, glyphs.topRight, style);
  put(x, y + h - 1, glyphs.bottomLeft, style);
  put(x + w - 1, y + h - 1, glyphs.bottomRight, style);

  // Horizontal borders
  for (int col = x + 1; col < x + w - 1; ++col) {
    put(col, y, glyphs.horiz, style);
    put(col, y + h - 1, glyphs.horiz, style);
  }

  // Vertical borders
  for (int row = y + 1; row < y + h - 1; ++row) {
    put(x, row, glyphs.vert, style);
    put(x + w - 1, row, glyphs.vert, style);
  }

  // Title centered or left with 1 space offset
  if (!title.empty() && w > 4) {
    const int maxTitleLen = w - 4;
    const std::string_view displayTitle =
        title.size() > static_cast<std::size_t>(maxTitleLen)
            ? title.substr(0, static_cast<std::size_t>(maxTitleLen))
            : title;
    put(x + 1, y, U' ', style);
    putString(x + 2, y, displayTitle, style);
    put(x + 2 + static_cast<int>(displayTitle.size()), y, U' ', style);
  }
}

void VirtualScreenBuffer::drawHLine(int x, int y, int w, BorderStyle border,
                                    Style style) {
  const BoxGlyphs glyphs = getBoxGlyphs(border);
  for (int col = x; col < x + w; ++col) {
    put(col, y, glyphs.horiz, style);
  }
}

void VirtualScreenBuffer::drawVLine(int x, int y, int h, BorderStyle border,
                                    Style style) {
  const BoxGlyphs glyphs = getBoxGlyphs(border);
  for (int row = y; row < y + h; ++row) {
    put(x, row, glyphs.vert, style);
  }
}

void VirtualScreenBuffer::fillRect(int x, int y, int w, int h,
                                   char32_t fillChar, Style style) {
  for (int row = y; row < y + h; ++row) {
    for (int col = x; col < x + w; ++col) {
      put(col, row, fillChar, style);
    }
  }
}

const ScreenCell &VirtualScreenBuffer::at(int x, int y) const {
  if (x < 0 || x >= width_ || y < 0 || y >= height_) {
    static const ScreenCell kEmpty{U' ', 1, {}};
    return kEmpty;
  }
  return grid_[static_cast<std::size_t>(y * width_ + x)];
}

ScreenCell &VirtualScreenBuffer::at(int x, int y) {
  if (x < 0 || x >= width_ || y < 0 || y >= height_) {
    throw std::out_of_range("VirtualScreenBuffer::at out of bounds");
  }
  dirty_ = true;
  return grid_[static_cast<std::size_t>(y * width_ + x)];
}

std::optional<ScreenCell> VirtualScreenBuffer::cellAt(int x, int y) const {
  if (x < 0 || x >= width_ || y < 0 || y >= height_) return std::nullopt;
  return grid_[static_cast<std::size_t>(y * width_ + x)];
}

std::span<const ScreenCell> VirtualScreenBuffer::row(int y) const {
  if (y < 0 || y >= height_) return {};
  const auto offset = static_cast<std::size_t>(y * width_);
  return std::span<const ScreenCell>(&grid_[offset],
                                     static_cast<std::size_t>(width_));
}

std::string VirtualScreenBuffer::line(int y) const {
  if (y < 0 || y >= height_) return "";
  std::string out;
  const auto r = row(y);
  for (const auto &cell : r) {
    if (cell.width == 0) continue; // Skip continuation cell
    out += encodeUtf8(cell.codepoint);
  }
  // Trim trailing whitespace for clean test assertions
  while (!out.empty() && out.back() == ' ') {
    out.pop_back();
  }
  return out;
}

std::string VirtualScreenBuffer::dump() const {
  std::string out;
  for (int y = 0; y < height_; ++y) {
    out += line(y);
    out.push_back('\n');
  }
  return out;
}

bool VirtualScreenBuffer::contains(std::string_view text) const {
  if (text.empty()) return true;
  for (int y = 0; y < height_; ++y) {
    if (line(y).find(text) != std::string::npos) {
      return true;
    }
  }
  return false;
}

} // namespace xanadu::ui::tui
