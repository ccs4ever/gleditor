/**
 * @file lexer.cpp
 * @brief Implementation of the vicon tokenizer.
 *
 * Icon's lexical rules: '#' line comments, significant newlines (a newline
 * separates statements only when the token before it can end an expression
 * and the next line does not open with a continuation token), "&name"
 * keyword references, radix integer literals, and string/cset literals with
 * backslash escapes.
 */
#include "common/xanadu/vicon/lexer.hpp"

#include <cctype>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>

namespace xanadu::vicon {

namespace {

bool isIdentStart(char c) {
  return (std::isalpha(static_cast<unsigned char>(c)) != 0) || c == '_';
}
bool isIdentCont(char c) {
  return (std::isalnum(static_cast<unsigned char>(c)) != 0) || c == '_';
}

// Icon radix literals ("16rFF") let letters carry digit values >= 10.
int radixDigitValue(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'z') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'Z') {
    return c - 'A' + 10;
  }
  return -1;
}

TokenKind keywordKind(std::string_view s) {
  static const std::unordered_map<std::string_view, TokenKind> kw = {
      {"if", TokenKind::If},
      {"then", TokenKind::Then},
      {"else", TokenKind::Else},
      {"while", TokenKind::While},
      {"do", TokenKind::Do},
      {"until", TokenKind::Until},
      {"every", TokenKind::Every},
      {"case", TokenKind::Case},
      {"of", TokenKind::Of},
      {"break", TokenKind::Break},
      {"next", TokenKind::Next},
      {"return", TokenKind::Return},
      {"fail", TokenKind::Fail},
      {"suspend", TokenKind::Suspend},
      {"repeat", TokenKind::Repeat},
      {"to", TokenKind::To},
      {"by", TokenKind::By},
      {"not", TokenKind::Not},
      {"procedure", TokenKind::Procedure},
      {"end", TokenKind::End},
      {"local", TokenKind::Local},
      {"global", TokenKind::Global},
      {"static", TokenKind::Static},
  };
  const auto it = kw.find(s);
  return it == kw.end() ? TokenKind::Identifier : it->second;
}

// The second half of Icon's newline rule: a newline does not separate
// statements when the next line opens with a token that cannot begin an
// expression. vicon's unary operators are only `!`, `-` and `not`
// (ast.hpp's EUnary), so every other operator spelling is binary-only and
// continues the expression; so do `,`, the closers, `;`, and the reserved
// words that only occur mid-expression (`then`, `else`, `do`, `of`, `to`,
// `by`, `end`).
bool beginsContinuation(const ::xanadu::ScannerBase &scanner) {
  const char c = scanner.peekChar();
  switch (c) {
  case ')':
  case ']':
  case '}':
  case ',':
  case ';':
  case ':':  // ":=" and the ':' of slices and case arms
  case '<':  // < <= << <<= <-
  case '>':  // > >= >> >>=
  case '=':  // = ==
  case '~':  // ~= ~== (a bare '~' is a lexical error either way)
  case '|':  // | || — no unary alternation in vicon
  case '\\': // limitation
  case '?':  // scanning is binary-only
  case '+':
  case '*':
  case '/':
  case '%':
  case '^':
    return true;
  case '&':
    // "&name" is a keyword primary; a bare "&" is the conjunction operator.
    return !isIdentStart(scanner.peekChar(1));
  default:
    break;
  }
  if (!isIdentStart(c)) {
    return false;
  }
  std::size_t n = 0;
  while (isIdentCont(scanner.peekChar(n))) {
    ++n;
  }
  const std::string_view word = scanner.source().substr(scanner.position(), n);
  return word == "then" || word == "else" || word == "do" || word == "of" ||
         word == "to" || word == "by" || word == "end";
}

} // namespace

Lexer::Lexer(std::string_view source) : scanner_(source) {}

Token Lexer::makeToken(TokenKind kind, SourceLocation loc,
                       std::size_t start) const {
  Token t;
  t.kind = kind;
  t.loc  = loc;
  t.text = scanner_.source().substr(start, scanner_.position() - start);
  return t;
}

void Lexer::skipBlanksAndComments() {
  for (;;) {
    const char c = scanner_.peekChar();
    if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
      scanner_.advanceChar();
      continue;
    }
    if (c == '#') {
      // A comment runs to end of line; the '\n' itself is left for next(),
      // which decides whether it is a statement separator.
      while (!scanner_.isAtEnd() && scanner_.peekChar() != '\n') {
        scanner_.advanceChar();
      }
      continue;
    }
    break;
  }
}

bool Lexer::prevEndsExpression() const noexcept {
  switch (lastKind_) {
  case TokenKind::Identifier:
  case TokenKind::Keyword:
  case TokenKind::IntegerLiteral:
  case TokenKind::RealLiteral:
  case TokenKind::StringLiteral:
  case TokenKind::CsetLiteral:
  case TokenKind::CloseParen:
  case TokenKind::CloseBracket:
  case TokenKind::CloseBrace:
  case TokenKind::Fail:
  case TokenKind::Next:
  case TokenKind::End:
    return true;
  default:
    return false;
  }
}

Token Lexer::scanIdentifierOrKeyword() {
  const SourceLocation loc = scanner_.currentLocation();
  const std::size_t start  = scanner_.position();
  while (isIdentCont(scanner_.peekChar())) {
    scanner_.advanceChar();
  }
  const std::string_view text =
      scanner_.source().substr(start, scanner_.position() - start);
  return makeToken(keywordKind(text), loc, start);
}

Token Lexer::scanNumber() {
  const SourceLocation loc = scanner_.currentLocation();
  const std::size_t start  = scanner_.position();
  const ScannedNumber num  = scanner_.scanNumberLiteral();

  // Icon's radix literal "Nrddd": the integer before 'r' is the base
  // (2..36) and letters carry digit values 10-35.
  if (!num.isFloat &&
      (scanner_.peekChar() == 'r' || scanner_.peekChar() == 'R') &&
      radixDigitValue(scanner_.peekChar(1)) >= 0) {
    const int radix = static_cast<int>(num.intValue);
    if (radix < 2 || radix > 36) {
      Token t       = makeToken(TokenKind::Error, loc, start);
      t.stringValue = "invalid radix for integer literal";
      return t;
    }
    scanner_.advanceChar(); // the 'r'
    std::uint64_t value = 0;
    for (;;) {
      const int digit = radixDigitValue(scanner_.peekChar());
      if (digit < 0) {
        break;
      }
      scanner_.advanceChar();
      if (digit >= radix) {
        Token t       = makeToken(TokenKind::Error, loc, start);
        t.stringValue = "digit out of range for radix literal";
        return t;
      }
      value = value * static_cast<std::uint64_t>(radix) +
              static_cast<std::uint64_t>(digit);
    }
    Token t    = makeToken(TokenKind::IntegerLiteral, loc, start);
    t.intValue = static_cast<std::int64_t>(value);
    return t;
  }

  Token t = makeToken(num.isFloat ? TokenKind::RealLiteral
                                  : TokenKind::IntegerLiteral,
                      loc, start);
  t.intValue   = num.intValue;
  t.floatValue = num.floatValue;
  return t;
}

Token Lexer::scanString(char quote, TokenKind kind) {
  const SourceLocation loc = scanner_.currentLocation();
  const std::size_t start  = scanner_.position();
  scanner_.advanceChar(); // opening quote; scanQuotedString expects it gone
  const ScannedString str = scanner_.scanQuotedString(quote);
  Token t = makeToken(str.success ? kind : TokenKind::Error, loc, start);
  // The shared scanner always reports "Unterminated string literal"; name
  // the cset delimiter so an unterminated '...' does not mislead.
  t.stringValue = str.success ? std::move(str.value)
                              : (kind == TokenKind::CsetLiteral
                                     ? std::string("Unterminated cset literal")
                                     : std::move(str.errorMessage));
  return t;
}

Token Lexer::scanKeywordRef() {
  const SourceLocation loc = scanner_.currentLocation();
  const std::size_t start  = scanner_.position();
  scanner_.advanceChar(); // '&'
  const std::size_t nameStart = scanner_.position();
  while (isIdentCont(scanner_.peekChar())) {
    scanner_.advanceChar();
  }
  Token t       = makeToken(TokenKind::Keyword, loc, start);
  t.stringValue = std::string(
      scanner_.source().substr(nameStart, scanner_.position() - nameStart));
  return t;
}

Token Lexer::scanToken() {
  const SourceLocation loc = scanner_.currentLocation();
  const std::size_t start  = scanner_.position();
  const char c             = scanner_.peekChar();

  if (isIdentStart(c)) {
    return scanIdentifierOrKeyword();
  }
  if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
    return scanNumber();
  }
  if (c == '"') {
    return scanString('"', TokenKind::StringLiteral);
  }
  if (c == '\'') {
    return scanString('\'', TokenKind::CsetLiteral);
  }
  // "&name" requires the name to be adjacent; a bare "&" is conjunction.
  if (c == '&' && isIdentStart(scanner_.peekChar(1))) {
    return scanKeywordRef();
  }

  scanner_.advanceChar();
  switch (c) {
  case '(':
    return makeToken(TokenKind::OpenParen, loc, start);
  case ')':
    return makeToken(TokenKind::CloseParen, loc, start);
  case '[':
    return makeToken(TokenKind::OpenBracket, loc, start);
  case ']':
    return makeToken(TokenKind::CloseBracket, loc, start);
  case '{':
    return makeToken(TokenKind::OpenBrace, loc, start);
  case '}':
    return makeToken(TokenKind::CloseBrace, loc, start);
  case ',':
    return makeToken(TokenKind::Comma, loc, start);
  case ';':
    return makeToken(TokenKind::Semicolon, loc, start);
  case ':':
    if (scanner_.match('=')) {
      return makeToken(TokenKind::Assign, loc, start);
    }
    return makeToken(TokenKind::Colon, loc, start);
  case '?':
    return makeToken(TokenKind::Scan, loc, start);
  case '!':
    return makeToken(TokenKind::Bang, loc, start);
  case '+':
    return makeToken(TokenKind::Plus, loc, start);
  case '-':
    return makeToken(TokenKind::Minus, loc, start);
  case '*':
    return makeToken(TokenKind::Star, loc, start);
  case '/':
    return makeToken(TokenKind::Slash, loc, start);
  case '%':
    return makeToken(TokenKind::Percent, loc, start);
  case '^':
    return makeToken(TokenKind::Caret, loc, start);
  case '\\':
    return makeToken(TokenKind::Limit, loc, start);
  case '&':
    return makeToken(TokenKind::Ampersand, loc, start);
  case '@':
    return makeToken(TokenKind::At, loc, start);
  case '|':
    if (scanner_.match('|')) {
      return makeToken(TokenKind::Concat, loc, start);
    }
    return makeToken(TokenKind::Alternation, loc, start);
  case '<':
    if (scanner_.match('-')) {
      return makeToken(TokenKind::RevAssign, loc, start);
    }
    if (scanner_.match('<')) {
      if (scanner_.match('=')) {
        return makeToken(TokenKind::StringLessEq, loc, start);
      }
      return makeToken(TokenKind::StringLess, loc, start);
    }
    if (scanner_.match('=')) {
      return makeToken(TokenKind::LessEqual, loc, start);
    }
    return makeToken(TokenKind::Less, loc, start);
  case '>':
    if (scanner_.match('>')) {
      if (scanner_.match('=')) {
        return makeToken(TokenKind::StringGtrEq, loc, start);
      }
      return makeToken(TokenKind::StringGreater, loc, start);
    }
    if (scanner_.match('=')) {
      return makeToken(TokenKind::GreaterEqual, loc, start);
    }
    return makeToken(TokenKind::Greater, loc, start);
  case '=':
    if (scanner_.match('=')) {
      return makeToken(TokenKind::StringEqual, loc, start);
    }
    return makeToken(TokenKind::Equal, loc, start);
  case '~':
    if (scanner_.match('=')) {
      if (scanner_.match('=')) {
        return makeToken(TokenKind::StringNotEq, loc, start);
      }
      return makeToken(TokenKind::NotEqual, loc, start);
    }
    break; // a bare '~' spells no vicon operator
  default:
    break;
  }

  Token t       = makeToken(TokenKind::Error, loc, start);
  t.stringValue = "unexpected character '" + std::string(1, c) + "'";
  return t;
}

Token Lexer::next() {
  auto emit = [this](Token t) {
    lastKind_ = t.kind;
    return t;
  };
  for (;;) {
    skipBlanksAndComments();
    const SourceLocation loc = scanner_.currentLocation();
    const std::size_t start  = scanner_.position();
    if (scanner_.isAtEnd()) {
      return emit(makeToken(TokenKind::EndOfFile, loc, start));
    }
    if (scanner_.peekChar() == '\n') {
      scanner_.advanceChar();
      if (!prevEndsExpression()) {
        continue; // the line ended inside an unfinished expression
      }
      // The expression was complete, but the next line may still continue
      // it by opening with a binary-only operator.
      skipBlanksAndComments();
      if (beginsContinuation(scanner_)) {
        continue;
      }
      return emit(makeToken(TokenKind::Semicolon, loc, start));
    }
    return emit(scanToken());
  }
}

std::vector<Token> Lexer::tokenize() {
  std::vector<Token> out;
  for (;;) {
    Token t         = next();
    const bool done = t.is(TokenKind::EndOfFile) || t.is(TokenKind::Error);
    out.push_back(std::move(t));
    if (done) {
      break;
    }
  }
  return out;
}

std::string_view tokenKindName(TokenKind kind) noexcept {
  switch (kind) {
  case TokenKind::EndOfFile:
    return "EndOfFile";
  case TokenKind::Error:
    return "Error";
  case TokenKind::Identifier:
    return "Identifier";
  case TokenKind::Keyword:
    return "Keyword";
  case TokenKind::IntegerLiteral:
    return "IntegerLiteral";
  case TokenKind::RealLiteral:
    return "RealLiteral";
  case TokenKind::StringLiteral:
    return "StringLiteral";
  case TokenKind::CsetLiteral:
    return "CsetLiteral";
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
  case TokenKind::Colon:
    return ":";
  case TokenKind::If:
    return "if";
  case TokenKind::Then:
    return "then";
  case TokenKind::Else:
    return "else";
  case TokenKind::While:
    return "while";
  case TokenKind::Do:
    return "do";
  case TokenKind::Until:
    return "until";
  case TokenKind::Every:
    return "every";
  case TokenKind::Case:
    return "case";
  case TokenKind::Of:
    return "of";
  case TokenKind::Break:
    return "break";
  case TokenKind::Next:
    return "next";
  case TokenKind::Return:
    return "return";
  case TokenKind::Fail:
    return "fail";
  case TokenKind::Suspend:
    return "suspend";
  case TokenKind::Repeat:
    return "repeat";
  case TokenKind::To:
    return "to";
  case TokenKind::By:
    return "by";
  case TokenKind::Not:
    return "not";
  case TokenKind::Procedure:
    return "procedure";
  case TokenKind::End:
    return "end";
  case TokenKind::Local:
    return "local";
  case TokenKind::Global:
    return "global";
  case TokenKind::Static:
    return "static";
  case TokenKind::Assign:
    return ":=";
  case TokenKind::RevAssign:
    return "<-";
  case TokenKind::Alternation:
    return "|";
  case TokenKind::Concat:
    return "||";
  case TokenKind::Limit:
    return "\\";
  case TokenKind::Scan:
    return "?";
  case TokenKind::Bang:
    return "!";
  case TokenKind::Plus:
    return "+";
  case TokenKind::Minus:
    return "-";
  case TokenKind::Star:
    return "*";
  case TokenKind::Slash:
    return "/";
  case TokenKind::Percent:
    return "%";
  case TokenKind::Caret:
    return "^";
  case TokenKind::Equal:
    return "=";
  case TokenKind::NotEqual:
    return "~=";
  case TokenKind::Less:
    return "<";
  case TokenKind::LessEqual:
    return "<=";
  case TokenKind::Greater:
    return ">";
  case TokenKind::GreaterEqual:
    return ">=";
  case TokenKind::StringEqual:
    return "==";
  case TokenKind::StringNotEq:
    return "~==";
  case TokenKind::StringLess:
    return "<<";
  case TokenKind::StringLessEq:
    return "<<=";
  case TokenKind::StringGreater:
    return ">>";
  case TokenKind::StringGtrEq:
    return ">>=";
  case TokenKind::Ampersand:
    return "&";
  case TokenKind::At:
    return "@";
  }
  return "Unknown";
}

} // namespace xanadu::vicon
