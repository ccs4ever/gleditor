#include <cstddef>
#include <fstream>
#include <ios>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

#include <gleditor/text_source.hpp>

namespace {

/// Byte at @p index widened through unsigned char. `char` is signed on most
/// targets, so comparing a raw one against 0xEF or 0xFF is never true and
/// silently disables mark detection.
unsigned char byteAt(const std::string &str, const std::size_t index) {
  return static_cast<unsigned char>(str[index]);
}

bool startsWith(const std::string &str,
                const std::initializer_list<unsigned char> mark) {
  if (str.size() < mark.size()) {
    return false;
  }
  std::size_t i = 0;
  for (const auto expected : mark) {
    if (byteAt(str, i++) != expected) {
      return false;
    }
  }
  return true;
}

} // namespace

namespace gleditor {

std::string stripByteOrderMark(std::string bytes) {
  if (startsWith(bytes, {0xEF, 0xBB, 0xBF})) {
    return bytes.substr(3);
  }
  // Tested before UTF-16, because a little-endian UTF-32 mark begins with the
  // whole of a little-endian UTF-16 one: checking the shorter first would
  // report every UTF-32LE file as UTF-16LE.
  if (startsWith(bytes, {0x00, 0x00, 0xFE, 0xFF}) ||
      startsWith(bytes, {0xFF, 0xFE, 0x00, 0x00})) {
    throw std::logic_error("utf32 not supported yet");
  }
  if (startsWith(bytes, {0xFE, 0xFF}) || startsWith(bytes, {0xFF, 0xFE})) {
    throw std::logic_error("utf16 not supported yet");
  }
  return bytes;
}

std::string FileTextSource::text() const {
  std::ifstream file(filePath, std::ios::binary);
  if (!file.is_open()) {
    throw std::runtime_error("failed to open file: " + filePath);
  }
  std::ostringstream ss;
  ss << file.rdbuf();
  return stripByteOrderMark(ss.str());
}

} // namespace gleditor
