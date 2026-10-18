/**
 * @file parser.cpp
 * @brief Iteration-1 stub: refuses every program as unimplemented.
 */
#include "common/xanadu/vicon/parser.hpp"

#include "common/xanadu/vicon/lexer.hpp"

namespace xanadu::vicon {

Parser::Parser(std::string_view source) : tokens_(Lexer(source).tokenize()) {}

std::expected<Program, ParseFailure> Parser::parseProgram() {
  return std::unexpected(ParseFailure{
      .code   = ParseError::Unimplemented,
      .detail = "vicon parser not implemented yet",
      .loc    = tokens_.empty() ? SourceLocation{} : tokens_.front().loc});
}

std::expected<ExprPtr, ParseFailure> Parser::parseExpression() {
  return std::unexpected(ParseFailure{
      .code   = ParseError::Unimplemented,
      .detail = "vicon parser not implemented yet",
      .loc    = tokens_.empty() ? SourceLocation{} : tokens_.front().loc});
}

std::string Parser::formatError(const ParseFailure &f) {
  return "vicon: " + std::to_string(f.loc.line) + ":" +
         std::to_string(f.loc.column) + ": " + f.detail;
}

} // namespace xanadu::vicon
