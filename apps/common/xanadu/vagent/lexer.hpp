#ifndef COMMON_XANADU_VAGENT_LEXER_HPP
#define COMMON_XANADU_VAGENT_LEXER_HPP

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/vagent/token.hpp"

namespace xanadu::vagent {

class Lexer {
public:
  explicit Lexer(std::string_view source) : source_(source) {}

  // Newlines remain physical tokens: the parser knows whether a brace is a
  // block or a record and therefore owns statement-boundary decisions.
  [[nodiscard]] std::expected<std::vector<Token>, Diagnostic> tokenize();

private:
  [[nodiscard]] std::optional<char> peek(std::size_t ahead = 0) const noexcept;
  [[nodiscard]] bool atEnd() const noexcept;
  [[nodiscard]] SourceLocation location() const noexcept;
  std::optional<char> advance() noexcept;
  [[nodiscard]] Token token(TokenKind kind, SourceLocation start) const;
  [[nodiscard]] Diagnostic error(std::string detail, SourceLocation loc) const;
  [[nodiscard]] Token newline();
  [[nodiscard]] Token identifier();
  [[nodiscard]] std::expected<Token, Diagnostic> number();
  [[nodiscard]] std::expected<Token, Diagnostic> string();
  [[nodiscard]] std::expected<Token, Diagnostic> punctuation();
  [[nodiscard]] std::expected<void, Diagnostic> digits(int base);

  std::string source_;
  std::size_t position_{0};
  std::size_t line_{1};
  std::size_t column_{1};
};

} // namespace xanadu::vagent

#endif // COMMON_XANADU_VAGENT_LEXER_HPP
