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
    char c = advanceChar();
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
      char esc = advanceChar();
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

ScannedNumber ScannerBase::scanNumberLiteral(bool leadingMinus,
                                             bool leadingPlus) {
  std::size_t startPos = pos_;
  if (leadingMinus || leadingPlus) {
    startPos = pos_ - 1;
  }

  bool isFloat = false;
  while (pos_ < source_.size() &&
         std::isdigit(static_cast<unsigned char>(peekChar()))) {
    advanceChar();
  }

  if (peekChar() == '.' &&
      std::isdigit(static_cast<unsigned char>(peekChar(1)))) {
    isFloat = true;
    advanceChar(); // consume '.'
    while (pos_ < source_.size() &&
           std::isdigit(static_cast<unsigned char>(peekChar()))) {
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
    while (pos_ < source_.size() &&
           std::isdigit(static_cast<unsigned char>(peekChar()))) {
      advanceChar();
    }
  }

  std::string_view numText = source_.substr(startPos, pos_ - startPos);

  if (isFloat) {
    double val = 0.0;
    try {
      val = std::stod(std::string(numText));
    } catch (...) {
      val = 0.0;
    }
    return ScannedNumber{
        .isFloat = true, .intValue = 0, .floatValue = val, .text = numText};
  }

  std::int64_t intVal = 0;
  std::from_chars(numText.data(), numText.data() + numText.size(), intVal);
  return ScannedNumber{
      .isFloat = false, .intValue = intVal, .floatValue = 0.0, .text = numText};
}

} // namespace xanadu
