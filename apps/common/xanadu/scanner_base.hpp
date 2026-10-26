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
#include <expected>
#include <optional>
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
  std::string_view text;
};

struct ScannedString {
  bool success{false};
  std::string value;
  std::string_view text;
  std::string errorMessage;
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

  [[nodiscard]] std::optional<char>
  peekChar(std::size_t offset = 0) const noexcept {
    if (pos_ >= source_.size() || offset >= source_.size() - pos_) {
      return std::nullopt;
    }
    return source_[pos_ + offset];
  }

  std::optional<char> advanceChar() noexcept {
    if (isAtEnd()) {
      return std::nullopt;
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
    if (const auto current = peekChar(); current && *current == expected) {
      advanceChar();
      return true;
    }
    return false;
  }

  [[nodiscard]] bool peekIsAlpha(std::size_t offset = 0) const noexcept {
    return classifies(std::isalpha, offset);
  }
  [[nodiscard]] bool peekIsAlnum(std::size_t offset = 0) const noexcept {
    return classifies(std::isalnum, offset);
  }
  [[nodiscard]] bool peekIsDigit(std::size_t offset = 0) const noexcept {
    return classifies(std::isdigit, offset);
  }
  [[nodiscard]] bool peekIsSpace(std::size_t offset = 0) const noexcept {
    return classifies(std::isspace, offset);
  }
  [[nodiscard]] bool peekIsXdigit(std::size_t offset = 0) const noexcept {
    return classifies(std::isxdigit, offset);
  }
  [[nodiscard]] bool peekIsUpper(std::size_t offset = 0) const noexcept {
    return classifies(std::isupper, offset);
  }
  [[nodiscard]] bool peekIsLower(std::size_t offset = 0) const noexcept {
    return classifies(std::islower, offset);
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
  std::expected<ScannedNumber, std::string>
  scanNumberLiteral(bool leadingMinus = false, bool leadingPlus = false);

private:
  bool classifies(int (*predicate)(int), std::size_t offset) const noexcept {
    const auto c = peekChar(offset);
    return c && predicate(static_cast<unsigned char>(*c)) != 0;
  }

protected:
  std::string_view source_;
  std::size_t pos_{0};
  std::size_t line_{1};
  std::size_t column_{1};
};

} // namespace xanadu

#endif // COMMON_XANADU_SCANNER_BASE_HPP
