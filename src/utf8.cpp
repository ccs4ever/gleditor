#include <gleditor/utf8.hpp> // IWYU pragma: associated

#include <cstdint>
#include <string>
#include <string_view>

namespace {

/// Whether the byte at @p offset continues a character rather than starting
/// one. Continuation bytes are 10xxxxxx.
bool continues(const std::string_view text, const std::uint32_t offset) {
  return offset < text.size() &&
         0x80 == (static_cast<unsigned char>(text[offset]) & 0xC0);
}

} // namespace

namespace gleditor {

std::uint32_t alignToCharacterStart(const std::string_view text,
                                    std::uint32_t offset) {
  while (offset > 0 && continues(text, offset)) {
    offset--;
  }
  return offset;
}

std::uint32_t alignToCharacterEnd(const std::string_view text,
                                  std::uint32_t offset) {
  while (continues(text, offset)) {
    offset++;
  }
  return offset;
}

bool validateUtf8(const std::string_view text, std::size_t &badOffsetOut) {
  const auto *bytes     = reinterpret_cast<const unsigned char *>(text.data());
  const std::size_t len = text.size();
  std::size_t i         = 0;

  while (i < len) {
    const unsigned char b0 = bytes[i];
    if (b0 <= 0x7F) {
      i++;
    } else if ((b0 & 0xE0) == 0xC0) {
      if (i + 1 >= len || (bytes[i + 1] & 0xC0) != 0x80 || b0 < 0xC2) {
        badOffsetOut = i;
        return false;
      }
      i += 2;
    } else if ((b0 & 0xF0) == 0xE0) {
      if (i + 2 >= len || (bytes[i + 1] & 0xC0) != 0x80 ||
          (bytes[i + 2] & 0xC0) != 0x80) {
        badOffsetOut = i;
        return false;
      }
      if ((b0 == 0xE0 && bytes[i + 1] < 0xA0) ||
          (b0 == 0xED && bytes[i + 1] > 0x9F)) {
        badOffsetOut = i;
        return false;
      }
      i += 3;
    } else if ((b0 & 0xF8) == 0xF0) {
      if (i + 3 >= len || (bytes[i + 1] & 0xC0) != 0x80 ||
          (bytes[i + 2] & 0xC0) != 0x80 || (bytes[i + 3] & 0xC0) != 0x80) {
        badOffsetOut = i;
        return false;
      }
      if ((b0 == 0xF0 && bytes[i + 1] < 0x90) ||
          (b0 == 0xF4 && bytes[i + 1] > 0x8F)) {
        badOffsetOut = i;
        return false;
      }
      i += 4;
    } else {
      badOffsetOut = i;
      return false;
    }
  }
  badOffsetOut = len;
  return true;
}

std::string makeValidUtf8(const std::string_view text) {
  std::string result;
  result.reserve(text.size());
  const auto *bytes     = reinterpret_cast<const unsigned char *>(text.data());
  const std::size_t len = text.size();
  std::size_t i         = 0;

  while (i < len) {
    const unsigned char b0 = bytes[i];
    if (b0 <= 0x7F) {
      result.push_back(static_cast<char>(b0));
      i++;
    } else if ((b0 & 0xE0) == 0xC0 && i + 1 < len &&
               (bytes[i + 1] & 0xC0) == 0x80 && b0 >= 0xC2) {
      result.append(text.substr(i, 2));
      i += 2;
    } else if ((b0 & 0xF0) == 0xE0 && i + 2 < len &&
               (bytes[i + 1] & 0xC0) == 0x80 && (bytes[i + 2] & 0xC0) == 0x80 &&
               !(b0 == 0xE0 && bytes[i + 1] < 0xA0) &&
               !(b0 == 0xED && bytes[i + 1] > 0x9F)) {
      result.append(text.substr(i, 3));
      i += 3;
    } else if ((b0 & 0xF8) == 0xF0 && i + 3 < len &&
               (bytes[i + 1] & 0xC0) == 0x80 && (bytes[i + 2] & 0xC0) == 0x80 &&
               (bytes[i + 3] & 0xC0) == 0x80 &&
               !(b0 == 0xF0 && bytes[i + 1] < 0x90) &&
               !(b0 == 0xF4 && bytes[i + 1] > 0x8F)) {
      result.append(text.substr(i, 4));
      i += 4;
    } else {
      result.append("\xEF\xBF\xBD"); // U+FFFD Replacement Character
      i++;
    }
  }
  return result;
}

} // namespace gleditor
