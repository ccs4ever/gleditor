/**
 * @file lexer.cpp
 * @brief Zero-allocation tokenizer implementation for VQL.
 */
#include "common/xanadu/vql/lexer.hpp"

#include <cctype>
#include <charconv>
#include <string>

namespace xanadu::vql {

std::string_view tokenKindName(TokenKind kind) noexcept {
  switch (kind) {
  case TokenKind::EndOfFile:
    return "EOF";
  case TokenKind::Error:
    return "Error";
  case TokenKind::Home:
    return "##";
  case TokenKind::NamedStore:
    return "##NAME";
  case TokenKind::Root:
    return "#";
  case TokenKind::Cursor:
    return "^";
  case TokenKind::NamedCursor:
    return "^NAME";
  case TokenKind::Variable:
    return "$var";
  case TokenKind::Dot:
    return ".";
  case TokenKind::Slash:
    return "/";
  case TokenKind::DoubleColon:
    return "::";
  case TokenKind::Bang:
    return "!";
  case TokenKind::Percent:
    return "%";
  case TokenKind::CloneJoin:
    return "><";
  case TokenKind::Assign:
    return ":=";
  case TokenKind::Question:
    return "?";
  case TokenKind::DerefMaster:
    return ">";
  case TokenKind::OpenParen:
    return "(";
  case TokenKind::CloseParen:
    return ")";
  case TokenKind::OpenBracket:
    return "[";
  case TokenKind::CloseBracket:
    return "]";
  case TokenKind::OpenBrace:
    return "{";
  case TokenKind::CloseBrace:
    return "}";
  case TokenKind::Comma:
    return ",";
  case TokenKind::Equal:
    return "=";
  case TokenKind::NotEqual:
    return "!=";
  case TokenKind::LessThan:
    return "<";
  case TokenKind::GreaterThan:
    return ">";
  case TokenKind::LessEqual:
    return "<=";
  case TokenKind::GreaterEqual:
    return ">=";
  case TokenKind::Plus:
    return "+";
  case TokenKind::Minus:
    return "-";
  case TokenKind::Star:
    return "*";
  case TokenKind::KwFor:
    return "for";
  case TokenKind::KwIn:
    return "in";
  case TokenKind::KwLet:
    return "let";
  case TokenKind::KwWhere:
    return "where";
  case TokenKind::KwReturn:
    return "return";
  case TokenKind::KwWeave:
    return "weave";
  case TokenKind::KwIf:
    return "if";
  case TokenKind::KwElse:
    return "else";
  case TokenKind::KwAnd:
    return "and";
  case TokenKind::KwOr:
    return "or";
  case TokenKind::KwNot:
    return "not";
  case TokenKind::KwTrue:
    return "true";
  case TokenKind::KwFalse:
    return "false";
  case TokenKind::Identifier:
    return "Identifier";
  case TokenKind::StringLiteral:
    return "StringLiteral";
  case TokenKind::IntegerLiteral:
    return "IntegerLiteral";
  case TokenKind::FloatLiteral:
    return "FloatLiteral";
  case TokenKind::BareLiteral:
    return "BareLiteral";
  }
  return "Unknown";
}

Lexer::Lexer(std::string_view source) : ScannerBase(source) {}

void Lexer::skipWhitespaceAndComments() {
  while (pos_ < source_.size()) {
    char c = peekChar();
    if (std::isspace(static_cast<unsigned char>(c))) {
      advanceChar();
      continue;
    }

    // Line comments: // ...
    if (c == '/' && peekChar(1) == '/') {
      advanceChar();
      advanceChar();
      while (pos_ < source_.size() && peekChar() != '\n') {
        advanceChar();
      }
      continue;
    }

    // Block comments: /* ... */
    if (c == '/' && peekChar(1) == '*') {
      advanceChar();
      advanceChar();
      while (pos_ < source_.size()) {
        if (peekChar() == '*' && peekChar(1) == '/') {
          advanceChar();
          advanceChar();
          break;
        }
        advanceChar();
      }
      continue;
    }

    // XQuery style comments: (: ... :)
    if (c == '(' && peekChar(1) == ':') {
      advanceChar();
      advanceChar();
      while (pos_ < source_.size()) {
        if (peekChar() == ':' && peekChar(1) == ')') {
          advanceChar();
          advanceChar();
          break;
        }
        advanceChar();
      }
      continue;
    }

    break;
  }
}

Token Lexer::peekToken() {
  if (!hasPeeked_) {
    peekedToken_ = nextToken();
    hasPeeked_   = true;
  }
  return peekedToken_;
}

Token Lexer::nextToken() {
  if (hasPeeked_) {
    hasPeeked_ = false;
    return peekedToken_;
  }

  skipWhitespaceAndComments();

  if (pos_ >= source_.size()) {
    return Token{
        .kind = TokenKind::EndOfFile, .text = {}, .loc = currentLocation()};
  }

  const SourceLocation startLoc = currentLocation();
  const std::size_t startPos    = pos_;
  const char c                  = advanceChar();

  // Anchors: ##, ##NAME, #
  if (c == '#') {
    if (match('#')) {
      if (std::isalpha(static_cast<unsigned char>(peekChar())) ||
          peekChar() == '_') {
        std::size_t nameStart = pos_;
        while (std::isalnum(static_cast<unsigned char>(peekChar())) ||
               peekChar() == '_') {
          advanceChar();
        }
        std::string_view name = source_.substr(nameStart, pos_ - nameStart);
        return Token{.kind        = TokenKind::NamedStore,
                     .text        = source_.substr(startPos, pos_ - startPos),
                     .loc         = startLoc,
                     .stringValue = std::string(name)};
      }
      return Token{.kind = TokenKind::Home,
                   .text = source_.substr(startPos, pos_ - startPos),
                   .loc  = startLoc};
    }
    return Token{.kind = TokenKind::Root,
                 .text = source_.substr(startPos, 1),
                 .loc  = startLoc};
  }

  // Cursors: ^, ^NAME
  if (c == '^') {
    if (std::isalpha(static_cast<unsigned char>(peekChar())) ||
        peekChar() == '_') {
      std::size_t nameStart = pos_;
      while (std::isalnum(static_cast<unsigned char>(peekChar())) ||
             peekChar() == '_') {
        advanceChar();
      }
      std::string_view name = source_.substr(nameStart, pos_ - nameStart);
      return Token{.kind        = TokenKind::NamedCursor,
                   .text        = source_.substr(startPos, pos_ - startPos),
                   .loc         = startLoc,
                   .stringValue = std::string(name)};
    }
    return Token{.kind = TokenKind::Cursor,
                 .text = source_.substr(startPos, 1),
                 .loc  = startLoc};
  }

  // Variable: $var
  if (c == '$') {
    std::size_t nameStart = pos_;
    while (std::isalnum(static_cast<unsigned char>(peekChar())) ||
           peekChar() == '_') {
      advanceChar();
    }
    std::string_view name = source_.substr(nameStart, pos_ - nameStart);
    return Token{.kind        = TokenKind::Variable,
                 .text        = source_.substr(startPos, pos_ - startPos),
                 .loc         = startLoc,
                 .stringValue = std::string(name)};
  }

  // Clone join: ><
  if (c == '>') {
    if (match('<')) {
      return Token{.kind = TokenKind::CloneJoin,
                   .text = source_.substr(startPos, 2),
                   .loc  = startLoc};
    }
    if (match('=')) {
      return Token{.kind = TokenKind::GreaterEqual,
                   .text = source_.substr(startPos, 2),
                   .loc  = startLoc};
    }
    // GreaterThan or DerefMaster: represented as GreaterThan
    return Token{.kind = TokenKind::GreaterThan,
                 .text = source_.substr(startPos, 1),
                 .loc  = startLoc};
  }

  // Less than / Less equal
  if (c == '<') {
    if (match('=')) {
      return Token{.kind = TokenKind::LessEqual,
                   .text = source_.substr(startPos, 2),
                   .loc  = startLoc};
    }
    return Token{.kind = TokenKind::LessThan,
                 .text = source_.substr(startPos, 1),
                 .loc  = startLoc};
  }

  // Colons: ::, :=
  if (c == ':') {
    if (match(':')) {
      return Token{.kind = TokenKind::DoubleColon,
                   .text = source_.substr(startPos, 2),
                   .loc  = startLoc};
    }
    if (match('=')) {
      return Token{.kind = TokenKind::Assign,
                   .text = source_.substr(startPos, 2),
                   .loc  = startLoc};
    }
  }

  // Exclamation: !=, !
  if (c == '!') {
    if (match('=')) {
      return Token{.kind = TokenKind::NotEqual,
                   .text = source_.substr(startPos, 2),
                   .loc  = startLoc};
    }
    return Token{.kind = TokenKind::Bang,
                 .text = source_.substr(startPos, 1),
                 .loc  = startLoc};
  }

  // Single-character tokens
  if (c == '.') {
    return Token{.kind = TokenKind::Dot,
                 .text = source_.substr(startPos, 1),
                 .loc  = startLoc};
  }
  if (c == '/') {
    return Token{.kind = TokenKind::Slash,
                 .text = source_.substr(startPos, 1),
                 .loc  = startLoc};
  }
  if (c == '%') {
    return Token{.kind = TokenKind::Percent,
                 .text = source_.substr(startPos, 1),
                 .loc  = startLoc};
  }
  if (c == '?') {
    return Token{.kind = TokenKind::Question,
                 .text = source_.substr(startPos, 1),
                 .loc  = startLoc};
  }
  if (c == '=') {
    if (match('=')) {
      return Token{.kind = TokenKind::Equal,
                   .text = source_.substr(startPos, 2),
                   .loc  = startLoc};
    }
    return Token{.kind = TokenKind::Equal,
                 .text = source_.substr(startPos, 1),
                 .loc  = startLoc};
  }
  if (c == '(') {
    return Token{.kind = TokenKind::OpenParen,
                 .text = source_.substr(startPos, 1),
                 .loc  = startLoc};
  }
  if (c == ')') {
    return Token{.kind = TokenKind::CloseParen,
                 .text = source_.substr(startPos, 1),
                 .loc  = startLoc};
  }
  if (c == '[') {
    return Token{.kind = TokenKind::OpenBracket,
                 .text = source_.substr(startPos, 1),
                 .loc  = startLoc};
  }
  if (c == ']') {
    return Token{.kind = TokenKind::CloseBracket,
                 .text = source_.substr(startPos, 1),
                 .loc  = startLoc};
  }
  if (c == '{') {
    return Token{.kind = TokenKind::OpenBrace,
                 .text = source_.substr(startPos, 1),
                 .loc  = startLoc};
  }
  if (c == '}') {
    return Token{.kind = TokenKind::CloseBrace,
                 .text = source_.substr(startPos, 1),
                 .loc  = startLoc};
  }
  if (c == ',') {
    return Token{.kind = TokenKind::Comma,
                 .text = source_.substr(startPos, 1),
                 .loc  = startLoc};
  }
  if (c == '*') {
    return Token{.kind = TokenKind::Star,
                 .text = source_.substr(startPos, 1),
                 .loc  = startLoc};
  }
  if (c == '+') {
    if (std::isdigit(static_cast<unsigned char>(peekChar()))) {
      return scanNumber(false, true);
    }
    return Token{.kind = TokenKind::Plus,
                 .text = source_.substr(startPos, 1),
                 .loc  = startLoc};
  }
  if (c == '-') {
    if (std::isdigit(static_cast<unsigned char>(peekChar()))) {
      return scanNumber(true, false);
    }
    return Token{.kind = TokenKind::Minus,
                 .text = source_.substr(startPos, 1),
                 .loc  = startLoc};
  }

  // String literals
  if (c == '"' || c == '\'') {
    return scanString(c);
  }

  // Number literals
  if (std::isdigit(static_cast<unsigned char>(c))) {
    pos_--; // back up so scanNumber reads the first digit
    if (column_ > 1) {
      column_--;
    }
    return scanNumber();
  }

  // Identifiers and Keywords
  if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
    pos_--;
    if (column_ > 1) {
      column_--;
    }
    return scanIdentifierOrKeyword();
  }

  return Token{.kind        = TokenKind::Error,
               .text        = source_.substr(startPos, 1),
               .loc         = startLoc,
               .stringValue = "Unexpected character"};
}

Token Lexer::scanString(char quoteChar) {
  const SourceLocation startLoc = currentLocation();
  auto res                      = scanQuotedString(quoteChar);
  if (!res.success) {
    return Token{.kind        = TokenKind::Error,
                 .text        = res.text,
                 .loc         = startLoc,
                 .stringValue = res.errorMessage};
  }
  return Token{.kind        = TokenKind::StringLiteral,
               .text        = res.text,
               .loc         = startLoc,
               .stringValue = std::move(res.value)};
}

Token Lexer::scanNumber(bool leadingMinus, bool leadingPlus) {
  const SourceLocation startLoc = currentLocation();
  auto res                      = scanNumberLiteral(leadingMinus, leadingPlus);
  if (res.isFloat) {
    return Token{.kind       = TokenKind::FloatLiteral,
                 .text       = res.text,
                 .loc        = startLoc,
                 .floatValue = res.floatValue};
  }
  return Token{.kind     = TokenKind::IntegerLiteral,
               .text     = res.text,
               .loc      = startLoc,
               .intValue = res.intValue};
}

Token Lexer::scanIdentifierOrKeyword() {
  const SourceLocation startLoc = currentLocation();
  const std::size_t startPos    = pos_;

  while (pos_ < source_.size()) {
    char c = peekChar();
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
      advanceChar();
      continue;
    }
    // Allow compound identifiers like d.name, d.pinning-cursors, std:math
    if ((c == '.' || c == '-' || c == ':') &&
        (std::isalnum(static_cast<unsigned char>(peekChar(1))) ||
         peekChar(1) == '_')) {
      advanceChar();
      continue;
    }
    break;
  }

  std::string_view text = source_.substr(startPos, pos_ - startPos);

  // Check keywords
  if (text == "for")
    return Token{.kind = TokenKind::KwFor, .text = text, .loc = startLoc};
  if (text == "in")
    return Token{.kind = TokenKind::KwIn, .text = text, .loc = startLoc};
  if (text == "let")
    return Token{.kind = TokenKind::KwLet, .text = text, .loc = startLoc};
  if (text == "where")
    return Token{.kind = TokenKind::KwWhere, .text = text, .loc = startLoc};
  if (text == "return")
    return Token{.kind = TokenKind::KwReturn, .text = text, .loc = startLoc};
  if (text == "weave")
    return Token{.kind = TokenKind::KwWeave, .text = text, .loc = startLoc};
  if (text == "if")
    return Token{.kind = TokenKind::KwIf, .text = text, .loc = startLoc};
  if (text == "else")
    return Token{.kind = TokenKind::KwElse, .text = text, .loc = startLoc};
  if (text == "and")
    return Token{.kind = TokenKind::KwAnd, .text = text, .loc = startLoc};
  if (text == "or")
    return Token{.kind = TokenKind::KwOr, .text = text, .loc = startLoc};
  if (text == "not")
    return Token{.kind = TokenKind::KwNot, .text = text, .loc = startLoc};
  if (text == "true")
    return Token{.kind = TokenKind::KwTrue, .text = text, .loc = startLoc};
  if (text == "false")
    return Token{.kind = TokenKind::KwFalse, .text = text, .loc = startLoc};

  return Token{.kind        = TokenKind::Identifier,
               .text        = text,
               .loc         = startLoc,
               .stringValue = std::string(text)};
}

Token Lexer::scanBareLiteral() {
  skipWhitespaceAndComments();
  const SourceLocation startLoc = currentLocation();
  const std::size_t startPos    = pos_;

  while (pos_ < source_.size()) {
    char c = peekChar();
    if (std::isspace(static_cast<unsigned char>(c)) || c == '%' || c == ',' ||
        c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}' ||
        c == '/' || c == '!') {
      break;
    }
    advanceChar();
  }

  std::string_view text = source_.substr(startPos, pos_ - startPos);
  return Token{.kind        = TokenKind::BareLiteral,
               .text        = text,
               .loc         = startLoc,
               .stringValue = std::string(text)};
}

std::vector<Token> Lexer::tokenizeAll() {
  std::vector<Token> tokens;
  while (true) {
    Token tok = nextToken();
    tokens.push_back(tok);
    if (tok.is(TokenKind::EndOfFile) || tok.is(TokenKind::Error)) {
      break;
    }
  }
  return tokens;
}

} // namespace xanadu::vql
