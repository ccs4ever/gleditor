/**
 * @file lexer.hpp
 * @brief Zero-allocation tokenizer for Prolog on Vortex / Vlog.
 */
#ifndef COMMON_XANADU_VPROLOG_LEXER_HPP
#define COMMON_XANADU_VPROLOG_LEXER_HPP

#include <string_view>
#include <vector>

#include "common/xanadu/scanner_base.hpp"
#include "common/xanadu/vprolog/token.hpp"

namespace xanadu::vprolog {

class Lexer : public ScannerBase {
public:
  explicit Lexer(std::string_view source);

  /// Returns the next token and advances the scanner.
  Token nextToken();

  /// Peeks at the next token without advancing.
  Token peekToken();

  /// Scans all tokens until EndOfFile.
  std::vector<Token> tokenizeAll();

private:
  void skipWhitespaceAndComments();
  Token scanIdentifierOrKeyword();
  Token scanQuotedAtom();
  Token scanOperatorOrSymbol();

  bool hasPeeked_{false};
  Token peekedToken_{};
};

} // namespace xanadu::vprolog

#endif // COMMON_XANADU_VPROLOG_LEXER_HPP
