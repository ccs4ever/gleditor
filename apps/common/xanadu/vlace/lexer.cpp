/**
 * @file lexer.cpp
 * @brief Implementation of the vlace tokenizer (§5.1).
 */
#include "common/xanadu/vlace/lexer.hpp"

#include <array>
#include <cctype>
#include <string>
#include <unordered_map>
#include <utility>

namespace xanadu::vlace {

namespace {

bool isIdentStart(char c) {
  return (std::islower(static_cast<unsigned char>(c)) != 0) || c == '_';
}
bool isIdentCont(char c) {
  return (std::isalnum(static_cast<unsigned char>(c)) != 0) || c == '_' ||
         c == '\'';
}
bool isCtorStart(char c) {
  return std::isupper(static_cast<unsigned char>(c)) != 0;
}

// The symbol characters an operator run is built from (§5.1).
bool isOpChar(char c) {
  switch (c) {
  case '+':
  case '-':
  case '*':
  case '/':
  case '<':
  case '>':
  case '=':
  case '!':
  case '&':
  case '|':
  case ':':
  case '.':
  case '^':
  case '%':
  case '$':
    return true;
  default:
    return false;
  }
}

TokenKind keywordKind(std::string_view s) {
  static const std::unordered_map<std::string_view, TokenKind> kw = {
      {"let", TokenKind::KwLet},       {"letrec", TokenKind::KwLetrec},
      {"in", TokenKind::KwIn},         {"if", TokenKind::KwIf},
      {"then", TokenKind::KwThen},     {"else", TokenKind::KwElse},
      {"case", TokenKind::KwCase},     {"of", TokenKind::KwOf},
      {"fn", TokenKind::KwFn},         {"data", TokenKind::KwData},
      {"type", TokenKind::KwType},     {"import", TokenKind::KwImport},
      {"as", TokenKind::KwAs},         {"infixl", TokenKind::KwInfixl},
      {"infixr", TokenKind::KwInfixr}, {"infix", TokenKind::KwInfix},
  };
  const auto it = kw.find(s);
  return it == kw.end() ? TokenKind::Identifier : it->second;
}

// A reserved symbol spelled exactly from operator characters is not an
// operator: "=", "=>", "|", "::", "->", ".". The parser needs these as their
// own kinds.
TokenKind reservedSymbolKind(std::string_view s) {
  if (s == "=") return TokenKind::Equals;
  if (s == "=>") return TokenKind::FatArrow;
  if (s == "|") return TokenKind::Bar;
  if (s == "::") return TokenKind::DoubleColon;
  if (s == "->") return TokenKind::ThinArrow;
  if (s == ".") return TokenKind::Dot;
  return TokenKind::Operator;
}

} // namespace

Lexer::Lexer(std::string_view source) : ScannerBase(source) {}

Token Lexer::makeToken(TokenKind kind, std::string_view text,
                       SourceLocation loc) const {
  Token t;
  t.kind = kind;
  t.text = text;
  t.loc  = loc;
  return t;
}

void Lexer::skipWhitespaceAndComments() {
  for (;;) {
    const char c = peekChar();
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      advanceChar();
      continue;
    }
    // `--` line comment runs to end of line. But a run like `-->` beginning
    // with -- is a comment only when it is exactly two dashes followed by a
    // non-operator char or more dashes; vlace keeps the simple Haskell rule:
    // two or more leading dashes with no other operator char is a comment.
    if (c == '-' && peekChar(1) == '-') {
      std::size_t n = 0;
      while (peekChar(n) == '-') {
        ++n;
      }
      const char after = peekChar(n);
      if (!isOpChar(after)) {
        while (!isAtEnd() && peekChar() != '\n') {
          advanceChar();
        }
        continue;
      }
    }
    break;
  }
}

Token Lexer::scanIdentifierOrKeyword() {
  const SourceLocation loc = currentLocation();
  const std::size_t start  = position();
  const bool ctor          = isCtorStart(peekChar());
  advanceChar();
  while (isIdentCont(peekChar())) {
    advanceChar();
  }
  const std::string_view text = source().substr(start, position() - start);
  if (ctor) {
    return makeToken(TokenKind::Constructor, text, loc);
  }
  const TokenKind kind = keywordKind(text);
  return makeToken(kind, text, loc);
}

Token Lexer::scanNumber() {
  const SourceLocation loc = currentLocation();
  const std::size_t start  = position();

  // Hex / octal / binary integer prefixes.
  if (peekChar() == '0' &&
      (peekChar(1) == 'x' || peekChar(1) == 'o' || peekChar(1) == 'b')) {
    const char base = peekChar(1);
    advanceChar();
    advanceChar();
    const std::size_t digitsStart = position();
    auto isDigitFor               = [&](char c) {
      if (base == 'x') return std::isxdigit(static_cast<unsigned char>(c)) != 0;
      if (base == 'o') return c >= '0' && c <= '7';
      return c == '0' || c == '1';
    };
    while (isDigitFor(peekChar())) {
      advanceChar();
    }
    Token t = makeToken(TokenKind::IntegerLiteral,
                        source().substr(start, position() - start), loc);
    const std::string digits(
        source().substr(digitsStart, position() - digitsStart));
    const int radix = base == 'x' ? 16 : (base == 'o' ? 8 : 2);
    t.intValue =
        digits.empty()
            ? 0
            : static_cast<std::int64_t>(std::stoll(digits, nullptr, radix));
    return t;
  }

  // Decimal integer or float. scanNumberLiteral handles the fractional and
  // exponent parts and classifies int vs float.
  const ScannedNumber num = scanNumberLiteral();
  Token t      = makeToken(num.isFloat ? TokenKind::FloatLiteral
                                       : TokenKind::IntegerLiteral,
                           source().substr(start, position() - start), loc);
  t.intValue   = num.intValue;
  t.floatValue = num.floatValue;
  return t;
}

Token Lexer::scanString() {
  const SourceLocation loc = currentLocation();
  advanceChar(); // consume the opening quote; scanQuotedString expects this.
  const ScannedString str = scanQuotedString('"');
  Token t = makeToken(str.success ? TokenKind::StringLiteral : TokenKind::Error,
                      str.text, loc);
  t.stringValue = str.value;
  return t;
}

Token Lexer::scanChar() {
  const SourceLocation loc = currentLocation();
  const std::size_t start  = position();
  advanceChar(); // opening quote
  std::string value;
  if (peekChar() == '\\') {
    advanceChar();
    const char esc = advanceChar();
    switch (esc) {
    case 'n':
      value.push_back('\n');
      break;
    case 't':
      value.push_back('\t');
      break;
    case 'r':
      value.push_back('\r');
      break;
    case '0':
      value.push_back('\0');
      break;
    case '\\':
      value.push_back('\\');
      break;
    case '\'':
      value.push_back('\'');
      break;
    case '"':
      value.push_back('"');
      break;
    default:
      value.push_back(esc);
      break;
    }
  } else {
    value.push_back(advanceChar());
  }
  Token t;
  if (peekChar() == '\'') {
    advanceChar();
    t = makeToken(TokenKind::CharLiteral,
                  source().substr(start, position() - start), loc);
  } else {
    t = makeToken(TokenKind::Error, source().substr(start, position() - start),
                  loc);
  }
  t.stringValue = value;
  if (!value.empty()) {
    t.intValue =
        static_cast<std::int64_t>(static_cast<unsigned char>(value[0]));
  }
  return t;
}

Token Lexer::scanOperatorOrSymbol() {
  const SourceLocation loc = currentLocation();
  const std::size_t start  = position();
  const char c             = peekChar();

  // Single-character structural punctuation that is not built from operator
  // characters.
  switch (c) {
  case '(':
    advanceChar();
    return makeToken(TokenKind::OpenParen, source().substr(start, 1), loc);
  case ')':
    advanceChar();
    return makeToken(TokenKind::CloseParen, source().substr(start, 1), loc);
  case '[':
    advanceChar();
    return makeToken(TokenKind::OpenBracket, source().substr(start, 1), loc);
  case ']':
    advanceChar();
    return makeToken(TokenKind::CloseBracket, source().substr(start, 1), loc);
  case '{':
    advanceChar();
    return makeToken(TokenKind::OpenBrace, source().substr(start, 1), loc);
  case '}':
    advanceChar();
    return makeToken(TokenKind::CloseBrace, source().substr(start, 1), loc);
  case ',':
    advanceChar();
    return makeToken(TokenKind::Comma, source().substr(start, 1), loc);
  case ';':
    advanceChar();
    return makeToken(TokenKind::Semicolon, source().substr(start, 1), loc);
  case '@':
    advanceChar();
    return makeToken(TokenKind::At, source().substr(start, 1), loc);
  default:
    break;
  }

  // A lone underscore is the wildcard; an underscore starting a longer run is
  // an identifier (handled by scanIdentifierOrKeyword, not here).
  if (isOpChar(c)) {
    while (isOpChar(peekChar())) {
      advanceChar();
    }
    const std::string_view text = source().substr(start, position() - start);
    return makeToken(reservedSymbolKind(text), text, loc);
  }

  // Unknown character: consume one and report an error token.
  advanceChar();
  return makeToken(TokenKind::Error, source().substr(start, 1), loc);
}

Token Lexer::nextToken() {
  if (hasPeeked_) {
    hasPeeked_ = false;
    return std::move(peekedToken_);
  }
  skipWhitespaceAndComments();
  const SourceLocation loc = currentLocation();
  if (isAtEnd()) {
    return makeToken(TokenKind::EndOfFile, {}, loc);
  }
  const char c = peekChar();

  if (c == '_' && !isIdentCont(peekChar(1))) {
    advanceChar();
    return makeToken(TokenKind::Underscore, source().substr(loc.offset, 1),
                     loc);
  }
  if (isIdentStart(c) || isCtorStart(c)) {
    return scanIdentifierOrKeyword();
  }
  if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
    return scanNumber();
  }
  if (c == '"') {
    return scanString();
  }
  if (c == '\'') {
    return scanChar();
  }
  return scanOperatorOrSymbol();
}

Token Lexer::peekToken() {
  if (!hasPeeked_) {
    peekedToken_ = nextToken();
    hasPeeked_   = true;
  }
  return peekedToken_;
}

std::vector<Token> Lexer::tokenizeAll() {
  std::vector<Token> out;
  for (;;) {
    Token t        = nextToken();
    const bool eof = t.kind == TokenKind::EndOfFile;
    out.push_back(std::move(t));
    if (eof) {
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
  case TokenKind::Constructor:
    return "Constructor";
  case TokenKind::IntegerLiteral:
    return "IntegerLiteral";
  case TokenKind::FloatLiteral:
    return "FloatLiteral";
  case TokenKind::StringLiteral:
    return "StringLiteral";
  case TokenKind::CharLiteral:
    return "CharLiteral";
  case TokenKind::KwLet:
    return "let";
  case TokenKind::KwLetrec:
    return "letrec";
  case TokenKind::KwIn:
    return "in";
  case TokenKind::KwIf:
    return "if";
  case TokenKind::KwThen:
    return "then";
  case TokenKind::KwElse:
    return "else";
  case TokenKind::KwCase:
    return "case";
  case TokenKind::KwOf:
    return "of";
  case TokenKind::KwFn:
    return "fn";
  case TokenKind::KwData:
    return "data";
  case TokenKind::KwType:
    return "type";
  case TokenKind::KwImport:
    return "import";
  case TokenKind::KwAs:
    return "as";
  case TokenKind::KwInfixl:
    return "infixl";
  case TokenKind::KwInfixr:
    return "infixr";
  case TokenKind::KwInfix:
    return "infix";
  case TokenKind::Equals:
    return "=";
  case TokenKind::FatArrow:
    return "=>";
  case TokenKind::Bar:
    return "|";
  case TokenKind::Underscore:
    return "_";
  case TokenKind::Dot:
    return ".";
  case TokenKind::Comma:
    return ",";
  case TokenKind::Semicolon:
    return ";";
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
  case TokenKind::DoubleColon:
    return "::";
  case TokenKind::ThinArrow:
    return "->";
  case TokenKind::At:
    return "@";
  case TokenKind::Operator:
    return "Operator";
  }
  return "Unknown";
}

} // namespace xanadu::vlace
