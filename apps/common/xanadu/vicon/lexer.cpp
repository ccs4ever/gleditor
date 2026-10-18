/**
 * @file lexer.cpp
 * @brief Iteration-1 stub: reports every input as unimplemented.
 *
 * The real scanner lands with the iteration-2 lexer subagent; this stub
 * exists so the contract headers link and the drivers can exercise their
 * error paths.
 */
#include "common/xanadu/vicon/lexer.hpp"

namespace xanadu::vicon {

Lexer::Lexer(std::string_view source) : scanner_(source) {}

Token Lexer::next() {
  Token t;
  t.kind = TokenKind::EndOfFile;
  return t;
}

std::vector<Token> Lexer::tokenize() {
  std::vector<Token> out;
  for (;;) {
    Token t = next();
    out.push_back(t);
    if (t.is(TokenKind::EndOfFile) || t.is(TokenKind::Error)) break;
  }
  return out;
}

} // namespace xanadu::vicon
