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
  case TokenKind::AllCursors:
    return "^^";
  case TokenKind::AllNamedCursors:
    return "^^NAME";
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
  case TokenKind::Colon:
    return ":";
  case TokenKind::Arrow:
    return "=>";
  case TokenKind::Tilde:
    return "~";
  case TokenKind::At:
    return "@";
  case TokenKind::DotDot:
    return "..";
  case TokenKind::DotDotDot:
    return "...";
  case TokenKind::Transclude:
    return "<~";
  case TokenKind::DimPivot:
    return "<=>";
  case TokenKind::Backslash:
    return "\\";
  case TokenKind::Pipe:
    return "|";
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
  case TokenKind::Semicolon:
    return ";";
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
  case TokenKind::KwCurrent:
    return "current";
  case TokenKind::KwLocal:
    return "local";
  case TokenKind::KwTopic:
    return "topic";
  case TokenKind::KwBtpk:
    return "btpk";
  case TokenKind::KwSlices:
    return "slices";
  case TokenKind::KwDocs:
    return "docs";
  case TokenKind::KwCiting:
    return "citing";
  case TokenKind::KwBacklinks:
    return "backlinks";
  case TokenKind::KwFind:
    return "find";
  case TokenKind::KwSystem:
    return "system";
  case TokenKind::KwBtih:
    return "btih";
  case TokenKind::KwLedger:
    return "ledger";
  case TokenKind::KwDef:
    return "def";
  case TokenKind::KwPure:
    return "pure";
  case TokenKind::KwLensmount:
    return "lensmount";
  case TokenKind::KwLensunmount:
    return "lensunmount";
  case TokenKind::KwLensnext:
    return "lensnext";
  case TokenKind::KwLenscommit:
    return "lenscommit";
  case TokenKind::KwLensclose:
    return "lensclose";
  case TokenKind::KwMount:
    return "mount";
  case TokenKind::KwAs:
    return "as";
  case TokenKind::KwWatch:
    return "watch";
  case TokenKind::KwSince:
    return "since";
  case TokenKind::KwLens:
    return "lens";
  case TokenKind::KwList:
    return "list";
  case TokenKind::KwUntil:
    return "until";
  case TokenKind::KwWith:
    return "with";
  case TokenKind::KwPerspective:
    return "perspective";
  case TokenKind::KwBreak:
    return "break";
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
  case TokenKind::MicroversionLiteral:
    return "MicroversionLiteral";
  case TokenKind::TypedVersionLiteral:
    return "TypedVersionLiteral";
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

  // Cursors: ^, ^NAME, ^^, ^^NAME
  if (c == '^') {
    if (match('^')) {
      if (std::isalpha(static_cast<unsigned char>(peekChar())) ||
          peekChar() == '_') {
        std::size_t nameStart = pos_;
        while (std::isalnum(static_cast<unsigned char>(peekChar())) ||
               peekChar() == '_') {
          advanceChar();
        }
        std::string_view name = source_.substr(nameStart, pos_ - nameStart);
        return Token{.kind        = TokenKind::AllNamedCursors,
                     .text        = source_.substr(startPos, pos_ - startPos),
                     .loc         = startLoc,
                     .stringValue = std::string(name)};
      }
      return Token{.kind = TokenKind::AllCursors,
                   .text = source_.substr(startPos, pos_ - startPos),
                   .loc  = startLoc};
    }
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

  // Less than / Less equal / Transclude (<~) / DimPivot (<=>)
  if (c == '<') {
    if (match('~')) {
      return Token{.kind = TokenKind::Transclude,
                   .text = source_.substr(startPos, 2),
                   .loc  = startLoc};
    }
    if (match('=')) {
      if (match('>')) {
        return Token{.kind = TokenKind::DimPivot,
                     .text = source_.substr(startPos, 3),
                     .loc  = startLoc};
      }
      return Token{.kind = TokenKind::LessEqual,
                   .text = source_.substr(startPos, 2),
                   .loc  = startLoc};
    }
    return Token{.kind = TokenKind::LessThan,
                 .text = source_.substr(startPos, 1),
                 .loc  = startLoc};
  }

  // Colons: ::, :=, :
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
    return Token{.kind = TokenKind::Colon,
                 .text = source_.substr(startPos, 1),
                 .loc  = startLoc};
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

  // Single-character and operator tokens
  if (c == '~') {
    return Token{.kind = TokenKind::Tilde,
                 .text = source_.substr(startPos, 1),
                 .loc  = startLoc};
  }
  if (c == '|') {
    return Token{.kind = TokenKind::Pipe,
                 .text = source_.substr(startPos, 1),
                 .loc  = startLoc};
  }
  if (c == '\\') {
    return Token{.kind = TokenKind::Backslash,
                 .text = source_.substr(startPos, 1),
                 .loc  = startLoc};
  }
  if (c == '@') {
    if (std::isdigit(static_cast<unsigned char>(peekChar()))) {
      std::size_t verStart = pos_;
      while (std::isalnum(static_cast<unsigned char>(peekChar()))) {
        advanceChar();
      }
      std::string_view verStr = source_.substr(verStart, pos_ - verStart);
      return Token{.kind        = TokenKind::MicroversionLiteral,
                   .text        = source_.substr(startPos, pos_ - startPos),
                   .loc         = startLoc,
                   .stringValue = std::string(verStr)};
    }
    return Token{.kind = TokenKind::At,
                 .text = source_.substr(startPos, 1),
                 .loc  = startLoc};
  }
  if (c == '.') {
    if (match('.')) {
      if (match('.')) {
        return Token{.kind = TokenKind::DotDotDot,
                     .text = source_.substr(startPos, 3),
                     .loc  = startLoc};
      }
      return Token{.kind = TokenKind::DotDot,
                   .text = source_.substr(startPos, 2),
                   .loc  = startLoc};
    }
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
    if (match('>')) {
      return Token{.kind = TokenKind::Arrow,
                   .text = source_.substr(startPos, 2),
                   .loc  = startLoc};
    }
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
  if (c == ';') {
    return Token{.kind = TokenKind::Semicolon,
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

  // Typed version literal: v"..."
  if (c == 'v' && peekChar() == '"') {
    advanceChar(); // consume '"'
    std::size_t vStart = pos_;
    while (pos_ < source_.size() && peekChar() != '"') {
      advanceChar();
    }
    std::string_view vStr = source_.substr(vStart, pos_ - vStart);
    if (pos_ < source_.size() && peekChar() == '"') {
      advanceChar(); // consume closing '"'
    }
    return Token{.kind        = TokenKind::TypedVersionLiteral,
                 .text        = source_.substr(startPos, pos_ - startPos),
                 .loc         = startLoc,
                 .stringValue = std::string(vStr)};
  }

  // String literals
  if (c == '"' || c == '\'') {
    return scanString(c);
  }

  // Number literals or alphanumeric/microversion literals starting with a digit
  // (e.g. 2a9, 8c5)
  if (std::isdigit(static_cast<unsigned char>(c))) {
    pos_--; // back up so scanNumber or lookahead reads the first digit
    if (column_ > 1) {
      column_--;
    }

    // Check if this is an alphanumeric microversion literal (contains letters
    // like 2a9, 8c5)
    std::size_t lookahead = pos_;
    bool hasLetter        = false;
    bool isLikelyFloat    = false;
    while (lookahead < source_.size()) {
      char ch = source_[lookahead];
      if (ch == '.' && lookahead + 1 < source_.size() &&
          std::isdigit(static_cast<unsigned char>(source_[lookahead + 1]))) {
        isLikelyFloat = true;
        lookahead += 2;
        continue;
      }
      if (std::isalpha(static_cast<unsigned char>(ch)) || ch == '_') {
        if ((ch == 'e' || ch == 'E') && !hasLetter) {
          // Could be exponent of float
          std::size_t next = lookahead + 1;
          if (next < source_.size() &&
              (source_[next] == '+' || source_[next] == '-')) {
            next++;
          }
          if (next < source_.size() &&
              std::isdigit(static_cast<unsigned char>(source_[next]))) {
            isLikelyFloat = true;
            lookahead     = next + 1;
            continue;
          }
        }
        hasLetter = true;
      } else if (!std::isdigit(static_cast<unsigned char>(ch))) {
        break;
      }
      lookahead++;
    }

    if (hasLetter && !isLikelyFloat) {
      std::size_t startVer = pos_;
      pos_                 = lookahead;
      column_ += (pos_ - startVer);
      std::string_view verStr = source_.substr(startVer, pos_ - startVer);
      return Token{.kind        = TokenKind::MicroversionLiteral,
                   .text        = verStr,
                   .loc         = startLoc,
                   .stringValue = std::string(verStr)};
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
    if ((c == '.' || c == '-') &&
        (std::isalnum(static_cast<unsigned char>(peekChar(1))) ||
         peekChar(1) == '_')) {
      advanceChar();
      continue;
    }
    if (c == ':' &&
        (std::isalnum(static_cast<unsigned char>(peekChar(1))) ||
         peekChar(1) == '_') &&
        !(peekChar(1) == 'd' && peekChar(2) == '.')) {
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
  if (text == "current")
    return Token{.kind = TokenKind::KwCurrent, .text = text, .loc = startLoc};
  if (text == "local")
    return Token{.kind = TokenKind::KwLocal, .text = text, .loc = startLoc};
  if (text == "topic")
    return Token{.kind = TokenKind::KwTopic, .text = text, .loc = startLoc};
  if (text == "btpk")
    return Token{.kind = TokenKind::KwBtpk, .text = text, .loc = startLoc};
  if (text == "slice" || text == "slices")
    return Token{.kind = TokenKind::KwSlices, .text = text, .loc = startLoc};
  if (text == "doc" || text == "docs")
    return Token{.kind = TokenKind::KwDocs, .text = text, .loc = startLoc};
  if (text == "citing")
    return Token{.kind = TokenKind::KwCiting, .text = text, .loc = startLoc};
  if (text == "backlinks")
    return Token{.kind = TokenKind::KwBacklinks, .text = text, .loc = startLoc};
  if (text == "find")
    return Token{.kind = TokenKind::KwFind, .text = text, .loc = startLoc};
  if (text == "system")
    return Token{.kind = TokenKind::KwSystem, .text = text, .loc = startLoc};
  if (text == "btih")
    return Token{.kind = TokenKind::KwBtih, .text = text, .loc = startLoc};
  if (text == "ledger")
    return Token{.kind = TokenKind::KwLedger, .text = text, .loc = startLoc};
  if (text == "def")
    return Token{.kind = TokenKind::KwDef, .text = text, .loc = startLoc};
  if (text == "pure")
    return Token{.kind = TokenKind::KwPure, .text = text, .loc = startLoc};
  if (text == "lensmount")
    return Token{.kind = TokenKind::KwLensmount, .text = text, .loc = startLoc};
  if (text == "lensunmount")
    return Token{
        .kind = TokenKind::KwLensunmount, .text = text, .loc = startLoc};
  if (text == "lensnext")
    return Token{.kind = TokenKind::KwLensnext, .text = text, .loc = startLoc};
  if (text == "lenscommit")
    return Token{
        .kind = TokenKind::KwLenscommit, .text = text, .loc = startLoc};
  if (text == "lensclose")
    return Token{.kind = TokenKind::KwLensclose, .text = text, .loc = startLoc};
  if (text == "mount")
    return Token{.kind = TokenKind::KwMount, .text = text, .loc = startLoc};
  if (text == "as")
    return Token{.kind = TokenKind::KwAs, .text = text, .loc = startLoc};
  if (text == "watch")
    return Token{.kind = TokenKind::KwWatch, .text = text, .loc = startLoc};
  if (text == "since")
    return Token{.kind = TokenKind::KwSince, .text = text, .loc = startLoc};
  if (text == "lens")
    return Token{.kind = TokenKind::KwLens, .text = text, .loc = startLoc};
  if (text == "list")
    return Token{.kind = TokenKind::KwList, .text = text, .loc = startLoc};
  if (text == "until")
    return Token{.kind = TokenKind::KwUntil, .text = text, .loc = startLoc};
  if (text == "with")
    return Token{.kind = TokenKind::KwWith, .text = text, .loc = startLoc};
  if (text == "perspective")
    return Token{
        .kind = TokenKind::KwPerspective, .text = text, .loc = startLoc};
  if (text == "break")
    return Token{.kind = TokenKind::KwBreak, .text = text, .loc = startLoc};

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
