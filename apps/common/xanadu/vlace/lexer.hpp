/**
 * @file lexer.hpp
 * @brief Tokenizer for vlace (design/vlace-functional-language.md §5.1).
 *
 * ML-shaped lexical rules: lowercase-initial identifiers, uppercase-initial
 * constructors, reserved keywords and symbols, maximal operator runs, int /
 * float / string / char literals, and `--` line comments. Layout-insensitive.
 */
#ifndef COMMON_XANADU_VLACE_LEXER_HPP
#define COMMON_XANADU_VLACE_LEXER_HPP

#include <string_view>
#include <vector>

#include "common/xanadu/scanner_base.hpp"
#include "common/xanadu/vlace/token.hpp"

namespace xanadu::vlace {

class Lexer : public ScannerBase {
public:
  explicit Lexer(std::string_view source);

  /// Returns the next token and advances the scanner.
  Token nextToken();

  /// Peeks at the next token without advancing.
  Token peekToken();

  /// Scans all tokens through (and including) a final EndOfFile token.
  std::vector<Token> tokenizeAll();

private:
  void skipWhitespaceAndComments();
  Token scanIdentifierOrKeyword();
  Token scanNumber();
  Token scanString();
  Token scanChar();
  Token scanOperatorOrSymbol();
  Token makeToken(TokenKind kind, std::string_view text,
                  SourceLocation loc) const;

  bool hasPeeked_{false};
  Token peekedToken_{};
};

} // namespace xanadu::vlace

#endif // COMMON_XANADU_VLACE_LEXER_HPP
