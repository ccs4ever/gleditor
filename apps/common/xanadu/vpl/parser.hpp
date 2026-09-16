/**
 * @file parser.hpp
 * @brief Right-to-left recursive descent array parser for VPL.
 *
 * Implements APL right-to-left evaluation order, dynamic valence resolution,
 * vector stranding, adverb and conjunction operator derivations, and
 * assignments.
 */
#ifndef COMMON_XANADU_VPL_PARSER_HPP
#define COMMON_XANADU_VPL_PARSER_HPP

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/vpl/ast.hpp"
#include "common/xanadu/vpl/lexer.hpp"
#include "common/xanadu/vpl/token.hpp"

namespace xanadu::vpl {

class Parser {
public:
  explicit Parser(std::string_view source);
  explicit Parser(std::vector<Token> tokens);

  /// Parse entire source into a Program containing statements/expressions.
  std::shared_ptr<Program> parseProgram();

  /// Parse a single expression.
  std::shared_ptr<AstNode> parseExpression();

private:
  [[nodiscard]] const Token &peek(std::size_t offset = 0) const;
  const Token &advance();
  [[nodiscard]] bool check(TokenKind kind, std::size_t offset = 0) const;
  bool match(TokenKind kind);
  [[nodiscard]] bool isAtEnd() const;

  std::shared_ptr<AstNode> parseStatement();
  std::shared_ptr<AstNode> parseSubExpr();
  std::shared_ptr<AstNode>
  parseVerbOrDerived(std::shared_ptr<AstNode> leftNoun);
  std::shared_ptr<AstNode> parseNounStrand();
  std::shared_ptr<AstNode> parsePrimary();

  std::vector<Token> tokens_;
  std::size_t current_{0};
};

} // namespace xanadu::vpl

#endif // COMMON_XANADU_VPL_PARSER_HPP
