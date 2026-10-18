/**
 * @file lexer.hpp
 * @brief vicon lexer: Icon's lexical rules including significant newlines.
 *
 * A newline is a statement separator unless the expression is incomplete —
 * i.e. the previous token cannot end an expression (binary operator, `(`,
 * `,`, `do`, `then`, `else`, `of`, `|`, `to`, `by`, `?`, ...) or the next
 * non-blank token begins a continuation (a binary operator). The lexer emits
 * a `Semicolon` token for a separating newline so the parser never looks at
 * raw whitespace.
 *
 * `#` starts a comment to end-of-line. Single quotes delimit cset literals,
 * double quotes string literals, both with backslash escapes.
 */
#ifndef COMMON_XANADU_VICON_LEXER_HPP
#define COMMON_XANADU_VICON_LEXER_HPP

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/scanner_base.hpp"
#include "common/xanadu/vicon/token.hpp"

namespace xanadu::vicon {

class Lexer {
public:
  explicit Lexer(std::string_view source);

  /// The next token. Errors come back as TokenKind::Error carrying
  /// stringValue = the message.
  [[nodiscard]] Token next();

  /// The whole token stream (ending in EndOfFile). Convenience for tests
  /// and for a parser that wants random access.
  [[nodiscard]] std::vector<Token> tokenize();

private:
  Token scanToken();
  Token scanIdentifierOrKeyword();
  Token scanNumber();
  Token scanString(char quote, TokenKind kind);
  Token scanKeywordRef(); ///< &name
  [[nodiscard]] Token makeToken(TokenKind kind, SourceLocation loc,
                                std::size_t start) const;
  [[nodiscard]] bool prevEndsExpression() const noexcept;
  void skipBlanksAndComments(); ///< spaces, tabs, and '#' comments — not '\n'

  ::xanadu::ScannerBase scanner_;
  TokenKind lastKind_{TokenKind::Error}; ///< last emitted, for newline rule
};

} // namespace xanadu::vicon

#endif // COMMON_XANADU_VICON_LEXER_HPP
