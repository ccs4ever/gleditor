/**
 * @file lexer.hpp
 * @brief vagent lexer (design/vagent-agent-language.md §8.1).
 *
 * The lexer produces the whole token stream and records, per token, whether a
 * newline precedes it. Whether that newline ends a statement depends on the
 * enclosing bracket (a block, or a `( )`, `[ ]` or record-literal `{ }`),
 * which the parser tracks, so the rule is split: `newlineSeparates()` here
 * says what the tokens on either side allow, and the parser applies it only
 * inside blocks.
 */
#ifndef COMMON_XANADU_VAGENT_RUNTIME_LEXER_HPP
#define COMMON_XANADU_VAGENT_RUNTIME_LEXER_HPP

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/scanner_base.hpp"
#include "common/xanadu/vagent/runtime/token.hpp"

namespace xanadu::vagent::runtime {

enum class LexErrorCode : std::uint8_t {
  UnexpectedCharacter,
  UnterminatedString,
  UnterminatedComment,
  BadEscape,
  BadNumber,
  UnclosedBracket,   ///< an opener never closed; loc is the opener
  MismatchedBracket, ///< a closer that does not match the innermost opener
};

struct LexFailure {
  LexErrorCode code{LexErrorCode::UnexpectedCharacter};
  std::string detail;
  SourceLocation loc{};
  char bracket{'\0'}; ///< the opener or closer, for the two bracket errors
};

[[nodiscard]] std::string_view lexErrorName(LexErrorCode code) noexcept;

/// True when a newline after a token of kind `k` may end a statement: an
/// identifier, a literal, a closer, or a keyword that is complete on its own.
[[nodiscard]] bool canEndStatement(TokenKind k) noexcept;

/// True when a line beginning with `k` continues the previous statement. A
/// line beginning with `-`, `*`, `!`, `(`, `[` or `{` begins a new one,
/// because those can start an expression.
[[nodiscard]] bool continuesStatement(TokenKind k) noexcept;

/// The newline rule for a newline between `prev` and `next` inside a block.
[[nodiscard]] bool newlineSeparates(TokenKind prev, TokenKind next) noexcept;

class Lexer {
public:
  explicit Lexer(std::string_view source);

  /// The whole stream, ending in EndOfFile, or the first lexical error.
  [[nodiscard]] std::expected<std::vector<Token>, LexFailure> tokenize();

private:
  struct Open {
    char bracket;
    SourceLocation loc;
  };

  /// Skips blanks and comments; the result says whether a newline was crossed.
  [[nodiscard]] std::expected<bool, LexFailure> skipTrivia();
  [[nodiscard]] std::expected<Token, LexFailure> scanToken();
  [[nodiscard]] std::expected<Token, LexFailure> scanNumber();
  [[nodiscard]] std::expected<Token, LexFailure> scanString();
  [[nodiscard]] Token scanWord();
  [[nodiscard]] Token make(TokenKind kind, SourceLocation loc,
                           std::size_t start) const;
  [[nodiscard]] std::expected<void, LexFailure> trackBracket(const Token &t);

  ::xanadu::ScannerBase scanner_;
  std::vector<Open> open_;
};

} // namespace xanadu::vagent::runtime

#endif // COMMON_XANADU_VAGENT_RUNTIME_LEXER_HPP
