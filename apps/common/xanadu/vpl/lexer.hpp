/**
 * @file lexer.hpp
 * @brief Zero-allocation dual tokenizer for Vortex Parallel Language (VPL).
 *
 * Scans both authentic Unicode APL glyphs and J-style alternative ASCII
 * function names, dimensions, numbers, strings, and comments into VPL tokens.
 */
#ifndef COMMON_XANADU_VPL_LEXER_HPP
#define COMMON_XANADU_VPL_LEXER_HPP

#include <cstddef>
#include <string_view>
#include <vector>

#include "common/xanadu/scanner_base.hpp"
#include "common/xanadu/vpl/token.hpp"

namespace xanadu::vpl {

class Lexer : public ScannerBase {
public:
  explicit Lexer(std::string_view source);

  /// Returns next token and advances scanner.
  Token nextToken();

  /// Peeks at next token without advancing.
  Token peekToken();

  /// Scans all tokens until EndOfFile.
  std::vector<Token> tokenizeAll();

private:
  void skipWhitespaceAndComments();

  Token scanNumber(bool negative = false);
  Token scanString(char quoteChar);
  Token scanDimension();
  Token scanIdentifierOrKeyword();
  Token scanSymbolOrGlyph();

  bool hasPeeked_{false};
  Token peekedToken_{};
};

} // namespace xanadu::vpl

#endif // COMMON_XANADU_VPL_LEXER_HPP
