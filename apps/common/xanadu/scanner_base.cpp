/**
 * @file scanner_base.cpp
 * @brief Implementation of shared character navigation and scanning routines.
 */
#include "common/xanadu/scanner_base.hpp"

#include <cmath>
#include <string>

namespace xanadu {

ScannedString ScannerBase::scanQuotedString(char quoteChar) {
  const std::size_t startPos = pos_ - 1; // Include opening quote
  std::string value;

  while (pos_ < source_.size()) {
    char c = advanceChar().value();
    if (c == quoteChar) {
      return ScannedString{.success = true,
                           .value   = std::move(value),
                           .text    = source_.substr(startPos, pos_ - startPos),
                           .errorMessage = {}};
    }
    if (c == '\\') {
      if (pos_ >= source_.size()) {
        break;
      }
      char esc = advanceChar().value();
      switch (esc) {
      case '"':
        value += '"';
        break;
      case '\'':
        value += '\'';
        break;
      case '\\':
        value += '\\';
        break;
      case 'n':
        value += '\n';
        break;
      case 't':
        value += '\t';
        break;
      case 'r':
        value += '\r';
        break;
      case 'b':
        value += '\b';
        break;
      case 'f':
        value += '\f';
        break;
      case '0':
        value += '\0';
        break;
      default:
        value += esc;
        break;
      }
    } else {
      value += c;
    }
  }

  return ScannedString{.success = false,
                       .value   = {},
                       .text    = source_.substr(startPos, pos_ - startPos),
                       .errorMessage = "Unterminated string literal"};
}

std::expected<ScannedNumber, std::string>
ScannerBase::scanNumberLiteral(bool leadingMinus, bool leadingPlus) {
  std::size_t startPos = pos_;
  if (leadingMinus || leadingPlus) {
    if (pos_ == 0) return std::unexpected("numeric sign was not consumed");
    startPos = pos_ - 1;
  }

  bool isFloat = false;
  while (pos_ < source_.size() && peekIsDigit()) {
    advanceChar();
  }

  if (peekChar() == '.' && peekIsDigit(1)) {
    isFloat = true;
    advanceChar(); // consume '.'
    while (pos_ < source_.size() && peekIsDigit()) {
      advanceChar();
    }
  }

  // Scientific notation exponent (e.g. 1e-5, 2.5E+3)
  if (peekChar() == 'e' || peekChar() == 'E') {
    isFloat = true;
    advanceChar();
    if (peekChar() == '+' || peekChar() == '-') {
      advanceChar();
    }
    while (pos_ < source_.size() && peekIsDigit()) {
      advanceChar();
    }
  }

  std::string_view numText = source_.substr(startPos, pos_ - startPos);

  auto parsed = numText;
  if (parsed.starts_with('+')) parsed.remove_prefix(1);
  if (parsed.empty()) return std::unexpected("expected numeric literal");
  const auto *const begin = parsed.data();
  const auto *const end   = begin + parsed.size();
  if (isFloat) {
    double value{};
    const auto result = std::from_chars(begin, end, value);
    if (result.ec == std::errc::result_out_of_range || !std::isfinite(value))
      return std::unexpected("floating literal is out of range");
    if (result.ec != std::errc{} || result.ptr != end)
      return std::unexpected("invalid floating literal");
    return ScannedNumber{.isFloat = true, .floatValue = value, .text = numText};
  }
  std::int64_t value{};
  const auto result = std::from_chars(begin, end, value);
  if (result.ec == std::errc::result_out_of_range)
    return std::unexpected("integer literal is out of range");
  if (result.ec != std::errc{} || result.ptr != end)
    return std::unexpected("invalid integer literal");
  return ScannedNumber{.intValue = value, .text = numText};
}

} // namespace xanadu
