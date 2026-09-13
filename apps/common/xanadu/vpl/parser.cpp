/**
 * @file parser.cpp
 * @brief Implementation of the VPL right-to-left recursive descent array
 * parser.
 */
#include "common/xanadu/vpl/parser.hpp"

namespace xanadu::vpl {
namespace {

bool isAssignToken(TokenKind k) noexcept { return k == TokenKind::Assign; }

bool isNounToken(TokenKind k) noexcept {
  return k == TokenKind::Number || k == TokenKind::String ||
         k == TokenKind::Dimension || k == TokenKind::Identifier ||
         k == TokenKind::LParen;
}

} // namespace

Parser::Parser(std::string_view source) {
  Lexer lexer(source);
  tokens_ = lexer.tokenizeAll();
}

Parser::Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

const Token &Parser::peek(std::size_t offset) const {
  if (current_ + offset >= tokens_.size()) {
    return tokens_.back(); // EndOfFile
  }
  return tokens_[current_ + offset];
}

const Token &Parser::advance() {
  if (!isAtEnd()) {
    current_++;
  }
  return tokens_[current_ - 1];
}

bool Parser::check(TokenKind kind, std::size_t offset) const {
  if (current_ + offset >= tokens_.size()) {
    return kind == TokenKind::EndOfFile;
  }
  return tokens_[current_ + offset].kind == kind;
}

bool Parser::match(TokenKind kind) {
  if (check(kind)) {
    advance();
    return true;
  }
  return false;
}

bool Parser::isAtEnd() const {
  return current_ >= tokens_.size() ||
         tokens_[current_].kind == TokenKind::EndOfFile;
}

std::shared_ptr<Program> Parser::parseProgram() {
  SourceLocation loc = peek().location;
  std::vector<std::shared_ptr<AstNode>> stmts;

  while (!isAtEnd()) {
    // Skip extra newlines and semicolons
    while (match(TokenKind::Newline) || match(TokenKind::Semicolon)) {
    }
    if (isAtEnd()) break;

    auto stmt = parseStatement();
    if (stmt) {
      stmts.push_back(std::move(stmt));
    }

    // Statements may be delimited by newlines or semicolons
    while (match(TokenKind::Newline) || match(TokenKind::Semicolon)) {
    }
  }

  return std::make_shared<Program>(loc, std::move(stmts));
}

std::shared_ptr<AstNode> Parser::parseExpression() { return parseSubExpr(); }

std::shared_ptr<AstNode> Parser::parseStatement() { return parseSubExpr(); }

std::shared_ptr<AstNode> Parser::parseSubExpr() {
  if (isAtEnd() || check(TokenKind::Newline) || check(TokenKind::Semicolon) ||
      check(TokenKind::RParen) || check(TokenKind::RBracket)) {
    return nullptr;
  }

  // 1. Assignment: name ← expr or name =. expr
  if (check(TokenKind::Identifier) && isAssignToken(peek(1).kind)) {
    Token idTok = advance();
    advance(); // consume assignment token
    auto val = parseSubExpr();
    return std::make_shared<AssignExpr>(idTok.location, std::string(idTok.text),
                                        std::move(val));
  }

  // 2. Expression starting directly with a verb or operator
  if (isVerb(peek().kind) || peek().kind == TokenKind::OuterProduct ||
      peek().kind == TokenKind::Quad || peek().kind == TokenKind::QuadRead ||
      peek().kind == TokenKind::QuadSplit) {
    return parseVerbOrDerived(nullptr);
  }

  // 3. Expression starting with a noun / strand
  auto leftNoun = parseNounStrand();
  if (!leftNoun) {
    return nullptr;
  }

  // Check if followed by a verb or operator (dyadic)
  if (!isAtEnd() &&
      (isVerb(peek().kind) || peek().kind == TokenKind::OuterProduct ||
       peek().kind == TokenKind::Compress || isConjunction(peek().kind))) {
    return parseVerbOrDerived(std::move(leftNoun));
  }

  return leftNoun;
}

std::shared_ptr<AstNode>
Parser::parseVerbOrDerived(std::shared_ptr<AstNode> leftNoun) {
  SourceLocation loc = peek().location;

  // Outer Product conjunction: ∘. or o. or table.
  if (peek().kind == TokenKind::OuterProduct) {
    advance(); // consume ∘.
    if (isAtEnd()) {
      return nullptr;
    }
    Token verbTok    = advance();
    auto derivedVerb = std::make_shared<ConjunctionExpr>(
        loc, TokenKind::OuterProduct,
        std::make_shared<VerbExpr>(verbTok.location, verbTok.kind), nullptr);

    auto right = parseSubExpr();
    if (leftNoun) {
      return std::make_shared<DyadicExpr>(loc, derivedVerb, std::move(leftNoun),
                                          std::move(right));
    }
    return std::make_shared<MonadicExpr>(loc, derivedVerb, std::move(right));
  }

  // Direct conjunction after noun (e.g. W ⌸ ≢¨ W or A " 1 B)
  if (leftNoun && isConjunction(peek().kind)) {
    Token conjTok = advance();
    auto right    = parseSubExpr();
    return std::make_shared<ConjunctionExpr>(
        loc, conjTok.kind, std::move(leftNoun), std::move(right));
  }

  // Quad functions
  if (check(TokenKind::QuadRead) || check(TokenKind::QuadSplit) ||
      check(TokenKind::Quad)) {
    Token qTok = advance();
    auto arg   = parseSubExpr();
    return std::make_shared<QuadExpr>(qTok.location, qTok.kind, std::move(arg));
  }

  // Standard verb
  Token verbTok = advance();
  std::shared_ptr<AstNode> customVerb;

  // Check if followed by an adverb: / (reduce), \ (scan), ¨ (each), ⌿
  // (compress)
  if (!isAtEnd() && isAdverb(peek().kind)) {
    Token advTok = advance();
    customVerb   = std::make_shared<AdverbExpr>(
        advTok.location, advTok.kind,
        std::make_shared<VerbExpr>(verbTok.location, verbTok.kind));
  } else if (!isAtEnd() && isConjunction(peek().kind)) {
    // Followed by conjunction: ⍤ (rank) or ⌸ (key)
    Token conjTok = advance();
    auto conjArg  = parsePrimary();
    customVerb    = std::make_shared<ConjunctionExpr>(
        conjTok.location, conjTok.kind,
        std::make_shared<VerbExpr>(verbTok.location, verbTok.kind),
        std::move(conjArg));
  }

  auto right = parseSubExpr();

  if (leftNoun) {
    if (customVerb) {
      return std::make_shared<DyadicExpr>(loc, customVerb, std::move(leftNoun),
                                          std::move(right));
    }
    return std::make_shared<DyadicExpr>(loc, verbTok.kind, std::move(leftNoun),
                                        std::move(right));
  }

  if (customVerb) {
    return std::make_shared<MonadicExpr>(loc, customVerb, std::move(right));
  }
  return std::make_shared<MonadicExpr>(loc, verbTok.kind, std::move(right));
}

std::shared_ptr<AstNode> Parser::parseNounStrand() {
  SourceLocation loc = peek().location;
  auto first         = parsePrimary();
  if (!first) {
    return nullptr;
  }

  std::vector<std::shared_ptr<AstNode>> strand;
  strand.push_back(std::move(first));

  while (!isAtEnd() && isNounToken(peek().kind)) {
    auto next = parsePrimary();
    if (!next) break;
    strand.push_back(std::move(next));
  }

  if (strand.size() == 1) {
    return strand[0];
  }

  return std::make_shared<VectorExpr>(loc, std::move(strand));
}

std::shared_ptr<AstNode> Parser::parsePrimary() {
  if (isAtEnd()) {
    return nullptr;
  }

  Token tok = peek();

  if (tok.kind == TokenKind::Number) {
    advance();
    if (tok.isFloat) {
      return std::make_shared<ScalarExpr>(tok.location, tok.floatValue, true);
    }
    return std::make_shared<ScalarExpr>(tok.location, tok.intValue);
  }

  if (tok.kind == TokenKind::String) {
    advance();
    return std::make_shared<ScalarExpr>(tok.location, tok.stringValue);
  }

  if (tok.kind == TokenKind::Dimension) {
    advance();
    return std::make_shared<DimensionExpr>(tok.location, tok.stringValue);
  }

  if (tok.kind == TokenKind::Identifier) {
    advance();
    std::shared_ptr<AstNode> node =
        std::make_shared<IdentifierExpr>(tok.location, tok.stringValue);

    // Check for Indexing: target[index; index...]
    while (match(TokenKind::LBracket)) {
      std::vector<std::shared_ptr<AstNode>> indices;
      if (!check(TokenKind::RBracket)) {
        indices.push_back(parseSubExpr());
        while (match(TokenKind::Semicolon)) {
          indices.push_back(parseSubExpr());
        }
      }
      match(TokenKind::RBracket);
      node = std::make_shared<IndexingExpr>(tok.location, node,
                                            std::move(indices));
    }
    return node;
  }

  if (match(TokenKind::LParen)) {
    auto inner = parseSubExpr();
    match(TokenKind::RParen);

    // Parenthesized expression could also be indexed: (expr)[index]
    while (match(TokenKind::LBracket)) {
      std::vector<std::shared_ptr<AstNode>> indices;
      if (!check(TokenKind::RBracket)) {
        indices.push_back(parseSubExpr());
        while (match(TokenKind::Semicolon)) {
          indices.push_back(parseSubExpr());
        }
      }
      match(TokenKind::RBracket);
      inner = std::make_shared<IndexingExpr>(tok.location, inner,
                                             std::move(indices));
    }
    return inner;
  }

  return nullptr;
}

} // namespace xanadu::vpl
