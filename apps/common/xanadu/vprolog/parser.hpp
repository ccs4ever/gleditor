/**
 * @file parser.hpp
 * @brief Recursive-descent Pratt parser for ISO-style Prolog.
 */
#ifndef COMMON_XANADU_VPROLOG_PARSER_HPP
#define COMMON_XANADU_VPROLOG_PARSER_HPP

#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/vprolog/ast.hpp"
#include "common/xanadu/vprolog/lexer.hpp"

namespace xanadu::vprolog {

class ParseError : public std::runtime_error {
public:
  ParseError(std::string_view message, SourceLocation loc);

  [[nodiscard]] const SourceLocation &location() const noexcept { return loc_; }

private:
  SourceLocation loc_;
};

class Parser {
public:
  explicit Parser(std::string_view source);
  explicit Parser(Lexer lexer);

  /// Parses an entire Prolog source text into a Program of clauses.
  Program parseProgram();

  /// Parses a single clause, fact, rule, or query ended by '.'.
  Clause parseClause();

  /// Parses an isolated term with given maximum precedence (default 1200).
  Term parseTerm(int maxPrecedence = 1200);

  /// Checks if parser has reached the end of file.
  [[nodiscard]] bool isAtEnd();

private:
  Term parsePrimary();
  Term parseList();
  Term parseParenthesized();
  std::vector<Term> parseArgumentList();

  Token advance();
  Token peek();
  Token expect(TokenKind kind, std::string_view errMsg);
  bool match(TokenKind kind);

  Lexer lexer_;
};

} // namespace xanadu::vprolog

#endif // COMMON_XANADU_VPROLOG_PARSER_HPP
