#include <gleditor/utf8.hpp> // IWYU pragma: associated

#include <cstdint>
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

} // namespace gleditor
