/**
 * @file lexer.cpp
 * @brief Implementation of the Prolog tokenizer using ScannerBase.
 */
#include "common/xanadu/vprolog/lexer.hpp"

#include <cctype>

namespace xanadu::vprolog {

std::string_view tokenKindName(TokenKind kind) noexcept {
  switch (kind) {
  case TokenKind::EndOfFile:
    return "EOF";
  case TokenKind::Error:
    return "Error";
  case TokenKind::Atom:
    return "Atom";
  case TokenKind::Variable:
    return "Variable";
  case TokenKind::IntegerLiteral:
    return "Integer";
  case TokenKind::FloatLiteral:
    return "Float";
  case TokenKind::StringLiteral:
    return "String";
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
  case TokenKind::Dot:
    return ".";
  case TokenKind::Pipe:
    return "|";
  case TokenKind::Semicolon:
    return ";";
  case TokenKind::Neck:
    return ":-";
  case TokenKind::Query:
    return "?-";
  case TokenKind::Cut:
    return "!";
  case TokenKind::Arrow:
    return "->";
  case TokenKind::Not:
    return "\\+";
  case TokenKind::Equal:
    return "=";
  case TokenKind::NotEqual:
    return "\\=";
  case TokenKind::StrictEqual:
    return "==";
  case TokenKind::StrictNotEqual:
    return "\\==";
  case TokenKind::Univ:
    return "=..";
  case TokenKind::Is:
    return "is";
  case TokenKind::EqualArith:
    return "=:=";
  case TokenKind::NotEqualArith:
    return "=\\=";
  case TokenKind::Less:
    return "<";
  case TokenKind::Greater:
    return ">";
  case TokenKind::LessEqual:
    return "=<";
  case TokenKind::GreaterEqual:
    return ">=";
  case TokenKind::Plus:
    return "+";
  case TokenKind::Minus:
    return "-";
  case TokenKind::Star:
    return "*";
  case TokenKind::Slash:
    return "/";
  case TokenKind::IntDiv:
    return "//";
  case TokenKind::Mod:
    return "mod";
  case TokenKind::Rem:
    return "rem";
  case TokenKind::Power:
    return "**";
  case TokenKind::BitAnd:
    return "/\\";
  case TokenKind::BitOr:
    return "\\/";
  case TokenKind::BitShiftLeft:
    return "<<";
  case TokenKind::BitShiftRight:
    return ">>";
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

    // Line comment: % ...
    if (c == '%') {
      advanceChar();
      while (pos_ < source_.size() && peekChar() != '\n') {
        advanceChar();
      }
      continue;
    }

    // Block comment: /* ... */
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

  // Delimiters
  switch (c) {
  case '(':
    return Token{.kind = TokenKind::OpenParen,
                 .text = source_.substr(startPos, 1),
                 .loc  = startLoc};
  case ')':
    return Token{.kind = TokenKind::CloseParen,
                 .text = source_.substr(startPos, 1),
                 .loc  = startLoc};
  case '[':
    if (match(']')) {
      return Token{.kind        = TokenKind::Atom,
                   .text        = source_.substr(startPos, 2),
                   .loc         = startLoc,
                   .stringValue = "[]"};
    }
    return Token{.kind = TokenKind::OpenBracket,
                 .text = source_.substr(startPos, 1),
                 .loc  = startLoc};
  case ']':
    return Token{.kind = TokenKind::CloseBracket,
                 .text = source_.substr(startPos, 1),
                 .loc  = startLoc};
  case '{':
    return Token{.kind = TokenKind::OpenBrace,
                 .text = source_.substr(startPos, 1),
                 .loc  = startLoc};
  case '}':
    return Token{.kind = TokenKind::CloseBrace,
                 .text = source_.substr(startPos, 1),
                 .loc  = startLoc};
  case ',':
    return Token{.kind = TokenKind::Comma,
                 .text = source_.substr(startPos, 1),
                 .loc  = startLoc};
  case ';':
    return Token{.kind = TokenKind::Semicolon,
                 .text = source_.substr(startPos, 1),
                 .loc  = startLoc};
  case '|':
    return Token{.kind = TokenKind::Pipe,
                 .text = source_.substr(startPos, 1),
                 .loc  = startLoc};
  case '!':
    return Token{.kind = TokenKind::Cut,
                 .text = source_.substr(startPos, 1),
                 .loc  = startLoc};
  case '.':
    if (std::isdigit(static_cast<unsigned char>(peekChar()))) {
      backupChar();
      auto numRes = scanNumberLiteral();
      return Token{.kind       = TokenKind::FloatLiteral,
                   .text       = numRes.text,
                   .loc        = startLoc,
                   .floatValue = numRes.floatValue};
    }
    return Token{.kind = TokenKind::Dot,
                 .text = source_.substr(startPos, 1),
                 .loc  = startLoc};
  case ':':
    if (match('-')) {
      return Token{.kind = TokenKind::Neck,
                   .text = source_.substr(startPos, 2),
                   .loc  = startLoc};
    }
    break;
  case '?':
    if (match('-')) {
      return Token{.kind = TokenKind::Query,
                   .text = source_.substr(startPos, 2),
                   .loc  = startLoc};
    }
    break;
  case '-':
    if (match('>')) {
      return Token{.kind = TokenKind::Arrow,
                   .text = source_.substr(startPos, 2),
                   .loc  = startLoc};
    }
    return Token{.kind = TokenKind::Minus,
                 .text = source_.substr(startPos, 1),
                 .loc  = startLoc};
  case '+':
    return Token{.kind = TokenKind::Plus,
                 .text = source_.substr(startPos, 1),
                 .loc  = startLoc};
  case '*':
    if (match('*')) {
      return Token{.kind = TokenKind::Power,
                   .text = source_.substr(startPos, 2),
                   .loc  = startLoc};
    }
    return Token{.kind = TokenKind::Star,
                 .text = source_.substr(startPos, 1),
                 .loc  = startLoc};
  case '/':
    if (match('/')) {
      return Token{.kind = TokenKind::IntDiv,
                   .text = source_.substr(startPos, 2),
                   .loc  = startLoc};
    }
    if (match('\\')) {
      return Token{.kind = TokenKind::BitAnd,
                   .text = source_.substr(startPos, 2),
                   .loc  = startLoc};
    }
    return Token{.kind = TokenKind::Slash,
                 .text = source_.substr(startPos, 1),
                 .loc  = startLoc};
  case '\\':
    if (match('+')) {
      return Token{.kind = TokenKind::Not,
                   .text = source_.substr(startPos, 2),
                   .loc  = startLoc};
    }
    if (match('/')) {
      return Token{.kind = TokenKind::BitOr,
                   .text = source_.substr(startPos, 2),
                   .loc  = startLoc};
    }
    if (match('=')) {
      if (match('=')) {
        return Token{.kind = TokenKind::StrictNotEqual,
                     .text = source_.substr(startPos, 3),
                     .loc  = startLoc};
      }
      return Token{.kind = TokenKind::NotEqual,
                   .text = source_.substr(startPos, 2),
                   .loc  = startLoc};
    }
    break;
  case '=':
    if (match('.')) {
      if (match('.')) {
        return Token{.kind = TokenKind::Univ,
                     .text = source_.substr(startPos, 3),
                     .loc  = startLoc};
      }
      // backtrack '.'
      backupChar();
    } else if (match('=')) {
      return Token{.kind = TokenKind::StrictEqual,
                   .text = source_.substr(startPos, 2),
                   .loc  = startLoc};
    } else if (match(':')) {
      if (match('=')) {
        return Token{.kind = TokenKind::EqualArith,
                     .text = source_.substr(startPos, 3),
                     .loc  = startLoc};
      }
      backupChar();
    } else if (match('\\')) {
      if (match('=')) {
        return Token{.kind = TokenKind::NotEqualArith,
                     .text = source_.substr(startPos, 3),
                     .loc  = startLoc};
      }
      backupChar();
    } else if (match('<')) {
      return Token{.kind = TokenKind::LessEqual,
                   .text = source_.substr(startPos, 2),
                   .loc  = startLoc};
    }
    return Token{.kind = TokenKind::Equal,
                 .text = source_.substr(startPos, 1),
                 .loc  = startLoc};
  case '<':
    if (match('<')) {
      return Token{.kind = TokenKind::BitShiftLeft,
                   .text = source_.substr(startPos, 2),
                   .loc  = startLoc};
    }
    if (match('=')) {
      return Token{.kind = TokenKind::LessEqual,
                   .text = source_.substr(startPos, 2),
                   .loc  = startLoc};
    }
    return Token{.kind = TokenKind::Less,
                 .text = source_.substr(startPos, 1),
                 .loc  = startLoc};
  case '>':
    if (match('>')) {
      return Token{.kind = TokenKind::BitShiftRight,
                   .text = source_.substr(startPos, 2),
                   .loc  = startLoc};
    }
    if (match('=')) {
      return Token{.kind = TokenKind::GreaterEqual,
                   .text = source_.substr(startPos, 2),
                   .loc  = startLoc};
    }
    return Token{.kind = TokenKind::Greater,
                 .text = source_.substr(startPos, 1),
                 .loc  = startLoc};
  }

  // Quoted atom: '...'
  if (c == '\'') {
    auto strRes = scanQuotedString('\'');
    if (!strRes.success) {
      return Token{.kind        = TokenKind::Error,
                   .text        = strRes.text,
                   .loc         = startLoc,
                   .stringValue = strRes.errorMessage};
    }
    return Token{.kind        = TokenKind::Atom,
                 .text        = strRes.text,
                 .loc         = startLoc,
                 .stringValue = std::move(strRes.value)};
  }

  // Double quoted string: "..."
  if (c == '"') {
    auto strRes = scanQuotedString('"');
    if (!strRes.success) {
      return Token{.kind        = TokenKind::Error,
                   .text        = strRes.text,
                   .loc         = startLoc,
                   .stringValue = strRes.errorMessage};
    }
    return Token{.kind        = TokenKind::StringLiteral,
                 .text        = strRes.text,
                 .loc         = startLoc,
                 .stringValue = std::move(strRes.value)};
  }

  // Numbers
  if (std::isdigit(static_cast<unsigned char>(c))) {
    backupChar();
    auto numRes = scanNumberLiteral();
    if (numRes.isFloat) {
      return Token{.kind       = TokenKind::FloatLiteral,
                   .text       = numRes.text,
                   .loc        = startLoc,
                   .floatValue = numRes.floatValue};
    }
    return Token{.kind     = TokenKind::IntegerLiteral,
                 .text     = numRes.text,
                 .loc      = startLoc,
                 .intValue = numRes.intValue};
  }

  // Variables and Atoms
  if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
    backupChar();
    return scanIdentifierOrKeyword();
  }

  return Token{.kind        = TokenKind::Error,
               .text        = source_.substr(startPos, 1),
               .loc         = startLoc,
               .stringValue = "Unexpected character"};
}

Token Lexer::scanIdentifierOrKeyword() {
  const SourceLocation startLoc = currentLocation();
  const std::size_t startPos    = pos_;
  char first                    = advanceChar();

  while (pos_ < source_.size()) {
    char c = peekChar();
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
      advanceChar();
    } else {
      break;
    }
  }

  std::string_view identText = source_.substr(startPos, pos_ - startPos);

  // Variables start with uppercase A-Z or underscore _
  if (std::isupper(static_cast<unsigned char>(first)) || first == '_') {
    return Token{.kind        = TokenKind::Variable,
                 .text        = identText,
                 .loc         = startLoc,
                 .stringValue = std::string(identText)};
  }

  // Keywords / built-in operator atoms
  if (identText == "is") {
    return Token{.kind        = TokenKind::Is,
                 .text        = identText,
                 .loc         = startLoc,
                 .stringValue = "is"};
  }
  if (identText == "mod") {
    return Token{.kind        = TokenKind::Mod,
                 .text        = identText,
                 .loc         = startLoc,
                 .stringValue = "mod"};
  }
  if (identText == "rem") {
    return Token{.kind        = TokenKind::Rem,
                 .text        = identText,
                 .loc         = startLoc,
                 .stringValue = "rem"};
  }

  // Standard atoms
  return Token{.kind        = TokenKind::Atom,
               .text        = identText,
               .loc         = startLoc,
               .stringValue = std::string(identText)};
}

} // namespace xanadu::vprolog
