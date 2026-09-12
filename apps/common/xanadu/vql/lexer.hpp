/**
 * @file lexer.hpp
 * @brief Zero-allocation tokenizer for Vortex Query Language (VQL).
 */
#ifndef COMMON_XANADU_VQL_LEXER_HPP
#define COMMON_XANADU_VQL_LEXER_HPP

#include <cstddef>
#include <string_view>
#include <vector>

#include "common/xanadu/vql/token.hpp"

namespace xanadu::vql {

class Lexer : public ScannerBase {
public:
  explicit Lexer(std::string_view source);

  /// Returns the next token and advances the scanner.
  Token nextToken();

  /// Peeks at the next token without advancing.
  Token peekToken();

  /// Scans all remaining tokens into a vector until EndOfFile.
  std::vector<Token> tokenizeAll();

  /// Scans a bare literal token (for %VALUE where unquoted characters follow).
  Token scanBareLiteral();

private:
  void skipWhitespaceAndComments();

  Token scanString(char quoteChar = '"');
  Token scanNumber(bool leadingMinus = false, bool leadingPlus = false);
  Token scanIdentifierOrKeyword();

  bool hasPeeked_{false};
  Token peekedToken_{};
};

} // namespace xanadu::vql

#endif // COMMON_XANADU_VQL_LEXER_HPP
