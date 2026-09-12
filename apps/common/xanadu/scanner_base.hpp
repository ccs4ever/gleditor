/**
 * @file scanner_base.hpp
 * @brief Zero-allocation character navigation, numeric scanning, and string
 * unescaping shared between VQL and Prolog.
 */
#ifndef COMMON_XANADU_SCANNER_BASE_HPP
#define COMMON_XANADU_SCANNER_BASE_HPP

#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace xanadu {

struct SourceLocation {
  std::size_t line{1};
  std::size_t column{1};
  std::size_t offset{0};
};

struct ScannedNumber {
  bool isFloat{false};
  std::int64_t intValue{0};
  double floatValue{0.0};
  std::string_view text{};
};

struct ScannedString {
  bool success{false};
  std::string value{};
  std::string_view text{};
  std::string errorMessage{};
};

class ScannerBase {
public:
  explicit ScannerBase(std::string_view source) : source_(source) {}

  [[nodiscard]] std::string_view source() const noexcept { return source_; }
  [[nodiscard]] std::size_t position() const noexcept { return pos_; }
  [[nodiscard]] SourceLocation currentLocation() const noexcept {
    return SourceLocation{.line = line_, .column = column_, .offset = pos_};
  }
  [[nodiscard]] bool isAtEnd() const noexcept { return pos_ >= source_.size(); }

  [[nodiscard]] char peekChar(std::size_t offset = 0) const noexcept {
    if (pos_ + offset >= source_.size()) {
      return '\0';
    }
    return source_[pos_ + offset];
  }

  char advanceChar() noexcept {
    if (pos_ >= source_.size()) {
      return '\0';
    }
    char c = source_[pos_++];
    if (c == '\n') {
      line_++;
      column_ = 1;
    } else {
      column_++;
    }
    return c;
  }

  bool match(char expected) noexcept {
    if (peekChar() == expected) {
      advanceChar();
      return true;
    }
    return false;
  }

  void backupChar() noexcept {
    if (pos_ > 0) {
      pos_--;
      if (column_ > 1) {
        column_--;
      }
    }
  }

  ScannedString scanQuotedString(char quoteChar = '"');
  ScannedNumber scanNumberLiteral(bool leadingMinus = false,
                                  bool leadingPlus  = false);

protected:
  std::string_view source_;
  std::size_t pos_{0};
  std::size_t line_{1};
  std::size_t column_{1};
};

} // namespace xanadu

#endif // COMMON_XANADU_SCANNER_BASE_HPP
