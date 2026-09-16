/**
 * @file lexer.cpp
 * @brief Implementation of the VPL zero-allocation dual-syntax tokenizer.
 */
#include "common/xanadu/vpl/lexer.hpp"

#include <cctype>
#include <charconv>

namespace xanadu::vpl {

Lexer::Lexer(std::string_view source) : ScannerBase(source) {}

Token Lexer::peekToken() {
  if (!hasPeeked_) {
    peekedToken_ = nextToken();
    hasPeeked_   = true;
  }
  return peekedToken_;
}

std::vector<Token> Lexer::tokenizeAll() {
  std::vector<Token> tokens;
  Token tok = nextToken();
  while (tok.kind != TokenKind::EndOfFile) {
    tokens.push_back(tok);
    tok = nextToken();
  }
  tokens.push_back(tok); // Include EndOfFile
  return tokens;
}

void Lexer::skipWhitespaceAndComments() {
  while (!isAtEnd()) {
    char c = peekChar();
    if (c == ' ' || c == '\t' || c == '\r') {
      advanceChar();
      continue;
    }

    std::string_view rem = source_.substr(pos_);

    // APL comment: ⍝ (\xe2\x8d\x9d)
    if (rem.starts_with("\xe2\x8d\x9d")) {
      pos_ += 3;
      column_ += 3;
      while (!isAtEnd() && peekChar() != '\n') {
        advanceChar();
      }
      continue;
    }

    // J comment: NB.
    if (rem.starts_with("NB.")) {
      pos_ += 3;
      column_ += 3;
      while (!isAtEnd() && peekChar() != '\n') {
        advanceChar();
      }
      continue;
    }

    // C++ style comment: //
    if (rem.starts_with("//")) {
      pos_ += 2;
      column_ += 2;
      while (!isAtEnd() && peekChar() != '\n') {
        advanceChar();
      }
      continue;
    }

    // C style block comment: /* ... */
    if (rem.starts_with("/*")) {
      pos_ += 2;
      column_ += 2;
      while (!isAtEnd()) {
        if (source_.substr(pos_).starts_with("*/")) {
          pos_ += 2;
          column_ += 2;
          break;
        }
        if (peekChar() == '\n') {
          advanceChar();
        } else {
          advanceChar();
        }
      }
      continue;
    }

    break;
  }
}

Token Lexer::scanNumber(bool negative) {
  SourceLocation loc = currentLocation();
  std::size_t start  = pos_;

  // APL high minus: ¯ (\xc2\xaf or \xe2\x81\xbb)
  if (source_.substr(pos_).starts_with("\xc2\xaf")) {
    pos_ += 2;
    column_ += 2;
    negative = true;
  } else if (source_.substr(pos_).starts_with("\xe2\x81\xbb")) {
    pos_ += 3;
    column_ += 3;
    negative = true;
  } else if (peekChar() == '_') {
    // J-style negative number: _5
    advanceChar();
    negative = true;
  }

  bool isFloat = false;
  while (!isAtEnd() && std::isdigit(static_cast<unsigned char>(peekChar()))) {
    advanceChar();
  }

  if (peekChar() == '.' &&
      std::isdigit(static_cast<unsigned char>(peekChar(1)))) {
    isFloat = true;
    advanceChar(); // consume '.'
    while (!isAtEnd() && std::isdigit(static_cast<unsigned char>(peekChar()))) {
      advanceChar();
    }
  }

  if (peekChar() == 'e' || peekChar() == 'E') {
    isFloat          = true;
    std::size_t ePos = pos_;
    advanceChar();
    if (peekChar() == '+' || peekChar() == '-') {
      advanceChar();
    }
    if (!std::isdigit(static_cast<unsigned char>(peekChar()))) {
      pos_ = ePos; // rollback exponent if incomplete
    } else {
      while (!isAtEnd() &&
             std::isdigit(static_cast<unsigned char>(peekChar()))) {
        advanceChar();
      }
    }
  }

  std::string_view numText = source_.substr(start, pos_ - start);
  Token tok;
  tok.kind     = TokenKind::Number;
  tok.text     = numText;
  tok.location = loc;
  tok.isFloat  = isFloat;

  // Convert for evaluation
  std::string cleanText;
  cleanText.reserve(numText.size() + 1);
  if (negative) {
    cleanText.push_back('-');
  }
  for (std::size_t i = 0; i < numText.size();) {
    if (numText.substr(i).starts_with("\xc2\xaf")) {
      i += 2;
    } else if (numText.substr(i).starts_with("\xe2\x81\xbb")) {
      i += 3;
    } else if (numText[i] == '_') {
      i++;
    } else {
      cleanText.push_back(numText[i++]);
    }
  }

  if (isFloat) {
    try {
      tok.floatValue = std::stod(cleanText);
    } catch (...) {
      tok.floatValue = 0.0;
    }
    tok.intValue = static_cast<std::int64_t>(tok.floatValue);
  } else {
    try {
      tok.intValue = std::stoll(cleanText);
    } catch (...) {
      tok.intValue = 0;
    }
    tok.floatValue = static_cast<double>(tok.intValue);
  }

  return tok;
}

Token Lexer::scanString(char quoteChar) {
  SourceLocation loc = currentLocation();
  advanceChar(); // consume opening quote
  std::size_t start = pos_;
  std::string content;

  while (!isAtEnd()) {
    char c = peekChar();
    if (c == quoteChar) {
      if (peekChar(1) == quoteChar) {
        // Escaped quote
        content.push_back(quoteChar);
        advanceChar();
        advanceChar();
        continue;
      }
      advanceChar(); // consume closing quote
      break;
    }
    if (c == '\n') {
      // String across newline
      content.push_back(c);
      advanceChar();
      continue;
    }
    content.push_back(c);
    advanceChar();
  }

  Token tok;
  tok.kind        = TokenKind::String;
  tok.text        = source_.substr(start, pos_ - start);
  tok.location    = loc;
  tok.stringValue = std::move(content);
  return tok;
}

Token Lexer::scanDimension() {
  SourceLocation loc = currentLocation();
  std::size_t start  = pos_;
  pos_ += 2; // consume 'd.'
  column_ += 2;

  while (!isAtEnd()) {
    char c = peekChar();
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' ||
        c == '.') {
      advanceChar();
    } else {
      break;
    }
  }

  Token tok;
  tok.kind        = TokenKind::Dimension;
  tok.text        = source_.substr(start, pos_ - start);
  tok.location    = loc;
  tok.stringValue = std::string(tok.text);
  return tok;
}

Token Lexer::scanIdentifierOrKeyword() {
  SourceLocation loc = currentLocation();
  std::size_t start  = pos_;

  while (!isAtEnd()) {
    char c = peekChar();
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
      advanceChar();
    } else {
      break;
    }
  }

  // Check if trailed by '.' for J-style inflected words (e.g. each., copy.,
  // time., scrub., trans., read., split., print., out.)
  if (peekChar() == '.' &&
      !std::isalnum(static_cast<unsigned char>(peekChar(1)))) {
    advanceChar(); // consume '.'
  }

  std::string_view word = source_.substr(start, pos_ - start);

  // Check for J-style word keywords
  TokenKind kind = TokenKind::Identifier;
  if (word == "i.")
    kind = TokenKind::Iota;
  else if (word == "e.")
    kind = TokenKind::Member;
  else if (word == "I.")
    kind = TokenKind::Where;
  else if (word == "each.")
    kind = TokenKind::Each;
  else if (word == "copy.")
    kind = TokenKind::Compress;
  else if (word == "table." || word == "o.")
    kind = TokenKind::OuterProduct;
  else if (word == "key.")
    kind = TokenKind::Key;
  else if (word == "time." || word == "t.")
    kind = TokenKind::Hypertime;
  else if (word == "scrub." || word == "s.")
    kind = TokenKind::Scrub;
  else if (word == "trans." || word == "x.")
    kind = TokenKind::Transclude;
  else if (word == "read.")
    kind = TokenKind::QuadRead;
  else if (word == "split.")
    kind = TokenKind::QuadSplit;
  else if (word == "print." || word == "out.")
    kind = TokenKind::Quad;

  Token tok;
  tok.kind        = kind;
  tok.text        = word;
  tok.location    = loc;
  tok.stringValue = std::string(word);
  return tok;
}

Token Lexer::scanSymbolOrGlyph() {
  SourceLocation loc   = currentLocation();
  std::string_view rem = source_.substr(pos_);

  // Match APL Unicode multi-byte glyphs
  auto matchUtf8 = [&](std::string_view utf8) -> bool {
    if (rem.starts_with(utf8)) {
      pos_ += utf8.size();
      column_ += utf8.size();
      return true;
    }
    return false;
  };

  // Outer product ∘. (\xe2\x88\x98\x2e) or ∘ (\xe2\x88\x98)
  if (rem.starts_with("\xe2\x88\x98\x2e")) {
    pos_ += 4;
    column_ += 4;
    return Token{
        .kind = TokenKind::OuterProduct, .text = "∘.", .location = loc};
  }
  if (rem.starts_with("\xe2\x88\x98")) {
    pos_ += 3;
    column_ += 3;
    if (peekChar() == '.') {
      advanceChar();
    }
    return Token{
        .kind = TokenKind::OuterProduct, .text = "∘.", .location = loc};
  }

  // Multi-byte APL glyphs
  if (matchUtf8("\xe2\x8d\xb3"))
    return Token{.kind = TokenKind::Iota, .text = "⍳", .location = loc};
  if (matchUtf8("\xe2\x8d\xb4"))
    return Token{.kind = TokenKind::Rho, .text = "⍴", .location = loc};
  if (matchUtf8("\xe2\x8d\x89"))
    return Token{.kind = TokenKind::Transpose, .text = "⍉", .location = loc};
  if (matchUtf8("\xe2\x8c\xbd"))
    return Token{.kind = TokenKind::ReverseFirst, .text = "⌽", .location = loc};
  if (matchUtf8("\xe2\x8a\x96"))
    return Token{.kind = TokenKind::ReverseLast, .text = "⊖", .location = loc};
  if (matchUtf8("\xe2\x86\x91"))
    return Token{.kind = TokenKind::Take, .text = "↑", .location = loc};
  if (matchUtf8("\xe2\x86\x93"))
    return Token{.kind = TokenKind::Drop, .text = "↓", .location = loc};
  if (matchUtf8("\xe2\x8a\x82"))
    return Token{.kind = TokenKind::Enclose, .text = "⊂", .location = loc};
  if (matchUtf8("\xe2\x8a\x83"))
    return Token{.kind = TokenKind::Disclose, .text = "⊃", .location = loc};
  if (matchUtf8("\xe2\x88\x8a"))
    return Token{.kind = TokenKind::Member, .text = "∊", .location = loc};
  if (matchUtf8("\xe2\x8d\xb8"))
    return Token{.kind = TokenKind::Where, .text = "⍸", .location = loc};
  if (matchUtf8("\xe2\x89\xa1"))
    return Token{.kind = TokenKind::Match, .text = "≡", .location = loc};
  if (matchUtf8("\xe2\x89\xa2"))
    return Token{.kind = TokenKind::Tally, .text = "≢", .location = loc};
  if (matchUtf8("\xe2\x8d\x8b"))
    return Token{.kind = TokenKind::GradeUp, .text = "⍋", .location = loc};
  if (matchUtf8("\xe2\x8d\x92"))
    return Token{.kind = TokenKind::GradeDown, .text = "⍒", .location = loc};
  if (matchUtf8("\xc2\xa8"))
    return Token{.kind = TokenKind::Each, .text = "¨", .location = loc};
  if (matchUtf8("\xe2\x8c\xbf"))
    return Token{.kind = TokenKind::Compress, .text = "⌿", .location = loc};
  if (matchUtf8("\xe2\x8d\xa4"))
    return Token{.kind = TokenKind::RankOp, .text = "⍤", .location = loc};
  if (matchUtf8("\xe2\x8c\xb8"))
    return Token{.kind = TokenKind::Key, .text = "⌸", .location = loc};
  if (matchUtf8("\xe2\x8d\x9f"))
    return Token{.kind = TokenKind::Hypertime, .text = "⍟", .location = loc};
  if (matchUtf8("\xe2\x8d\xab"))
    return Token{.kind = TokenKind::Scrub, .text = "⍫", .location = loc};
  if (matchUtf8("\xe2\x8c\xba"))
    return Token{.kind = TokenKind::Transclude, .text = "⌺", .location = loc};
  if (matchUtf8("\xe2\x86\x90"))
    return Token{.kind = TokenKind::Assign, .text = "←", .location = loc};
  if (matchUtf8("\xe2\x8c\x88"))
    return Token{.kind = TokenKind::Ceiling, .text = "⌈", .location = loc};
  if (matchUtf8("\xe2\x8c\x8a"))
    return Token{.kind = TokenKind::Floor, .text = "⌊", .location = loc};
  if (matchUtf8("\xe2\x89\xa0"))
    return Token{.kind = TokenKind::NotEqual, .text = "≠", .location = loc};
  if (matchUtf8("\xe2\x89\xa4"))
    return Token{.kind = TokenKind::LessEqual, .text = "≤", .location = loc};
  if (matchUtf8("\xe2\x89\xa5"))
    return Token{.kind = TokenKind::GreaterEqual, .text = "≥", .location = loc};
  if (matchUtf8("\xe2\x88\xbc"))
    return Token{.kind = TokenKind::Not, .text = "∼", .location = loc};
  if (matchUtf8("\xe2\x88\xa7"))
    return Token{.kind = TokenKind::And, .text = "∧", .location = loc};
  if (matchUtf8("\xe2\x88\xa8"))
    return Token{.kind = TokenKind::Or, .text = "∨", .location = loc};
  if (matchUtf8("\xc3\x97"))
    return Token{.kind = TokenKind::Times, .text = "×", .location = loc};
  if (matchUtf8("\xc3\xb7"))
    return Token{.kind = TokenKind::Divide, .text = "÷", .location = loc};

  // Quad functions: ⎕READ, ⎕SPLIT, ⎕
  if (rem.starts_with("\xe2\x8e\x95")) {
    pos_ += 3;
    column_ += 3;
    if (source_.substr(pos_).starts_with("READ")) {
      pos_ += 4;
      column_ += 4;
      return Token{
          .kind = TokenKind::QuadRead, .text = "⎕READ", .location = loc};
    }
    if (source_.substr(pos_).starts_with("SPLIT")) {
      pos_ += 5;
      column_ += 5;
      return Token{
          .kind = TokenKind::QuadSplit, .text = "⎕SPLIT", .location = loc};
    }
    return Token{.kind = TokenKind::Quad, .text = "⎕", .location = loc};
  }

  // Multi-character ASCII / J-style combinations
  if (rem.starts_with("|..")) {
    pos_ += 3;
    column_ += 3;
    return Token{
        .kind = TokenKind::ReverseLast, .text = "|..", .location = loc};
  }
  if (rem.starts_with("|:")) {
    pos_ += 2;
    column_ += 2;
    return Token{.kind = TokenKind::Transpose, .text = "|:", .location = loc};
  }
  if (rem.starts_with("|.")) {
    pos_ += 2;
    column_ += 2;
    return Token{
        .kind = TokenKind::ReverseFirst, .text = "|.", .location = loc};
  }
  if (rem.starts_with("{.")) {
    pos_ += 2;
    column_ += 2;
    return Token{.kind = TokenKind::Take, .text = "{.", .location = loc};
  }
  if (rem.starts_with("}.")) {
    pos_ += 2;
    column_ += 2;
    return Token{.kind = TokenKind::Drop, .text = "}.", .location = loc};
  }
  if (rem.starts_with("-:")) {
    pos_ += 2;
    column_ += 2;
    return Token{.kind = TokenKind::Match, .text = "-:", .location = loc};
  }
  if (rem.starts_with("/:")) {
    pos_ += 2;
    column_ += 2;
    return Token{.kind = TokenKind::GradeUp, .text = "/:", .location = loc};
  }
  if (rem.starts_with("\\:")) {
    pos_ += 2;
    column_ += 2;
    return Token{.kind = TokenKind::GradeDown, .text = "\\:", .location = loc};
  }
  if (rem.starts_with("/..")) {
    pos_ += 3;
    column_ += 3;
    return Token{.kind = TokenKind::Key, .text = "/..", .location = loc};
  }
  if (rem.starts_with("#/")) {
    pos_ += 2;
    column_ += 2;
    return Token{.kind = TokenKind::Compress, .text = "#/", .location = loc};
  }
  if (rem.starts_with("=.") || rem.starts_with(":=") || rem.starts_with("<-")) {
    pos_ += 2;
    column_ += 2;
    return Token{
        .kind = TokenKind::Assign, .text = rem.substr(0, 2), .location = loc};
  }
  if (rem.starts_with(">.")) {
    pos_ += 2;
    column_ += 2;
    return Token{.kind = TokenKind::Ceiling, .text = ">.", .location = loc};
  }
  if (rem.starts_with("<.")) {
    pos_ += 2;
    column_ += 2;
    return Token{.kind = TokenKind::Floor, .text = "<.", .location = loc};
  }
  if (rem.starts_with("~=") || rem.starts_with("~:")) {
    pos_ += 2;
    column_ += 2;
    return Token{
        .kind = TokenKind::NotEqual, .text = rem.substr(0, 2), .location = loc};
  }
  if (rem.starts_with("<:=") || rem.starts_with("<=.") ||
      rem.starts_with("<=")) {
    std::size_t len = rem.starts_with("<:=") || rem.starts_with("<=.") ? 3 : 2;
    pos_ += len;
    column_ += len;
    return Token{.kind     = TokenKind::LessEqual,
                 .text     = rem.substr(0, len),
                 .location = loc};
  }
  if (rem.starts_with(">:=") || rem.starts_with(">=.") ||
      rem.starts_with(">=")) {
    std::size_t len = rem.starts_with(">:=") || rem.starts_with(">=.") ? 3 : 2;
    pos_ += len;
    column_ += len;
    return Token{.kind     = TokenKind::GreaterEqual,
                 .text     = rem.substr(0, len),
                 .location = loc};
  }
  if (rem.starts_with("*.")) {
    pos_ += 2;
    column_ += 2;
    return Token{.kind = TokenKind::And, .text = "*.", .location = loc};
  }
  if (rem.starts_with("+.")) {
    pos_ += 2;
    column_ += 2;
    return Token{.kind = TokenKind::Or, .text = "+.", .location = loc};
  }
  if (rem.starts_with("&.")) {
    pos_ += 2;
    column_ += 2;
    return Token{.kind = TokenKind::Each, .text = "&.", .location = loc};
  }

  // Single-character ASCII symbols
  char c = advanceChar();
  switch (c) {
  case '$':
    return Token{.kind = TokenKind::Rho, .text = "$", .location = loc};
  case '#':
    return Token{.kind = TokenKind::Tally, .text = "#", .location = loc};
  case '<':
    return Token{.kind = TokenKind::Enclose, .text = "<", .location = loc};
  case '>':
    return Token{.kind = TokenKind::Disclose, .text = ">", .location = loc};
  case '+':
    return Token{.kind = TokenKind::Plus, .text = "+", .location = loc};
  case '-':
    return Token{.kind = TokenKind::Minus, .text = "-", .location = loc};
  case '*':
    return Token{.kind = TokenKind::Times, .text = "*", .location = loc};
  case '%':
    return Token{.kind = TokenKind::Divide, .text = "%", .location = loc};
  case '^':
    return Token{.kind = TokenKind::Power, .text = "^", .location = loc};
  case '|':
    return Token{.kind = TokenKind::Magnitude, .text = "|", .location = loc};
  case '=':
    return Token{.kind = TokenKind::Equal, .text = "=", .location = loc};
  case '~':
    return Token{.kind = TokenKind::Not, .text = "~", .location = loc};
  case '&':
    return Token{.kind = TokenKind::And, .text = "&", .location = loc};
  case '/':
    return Token{.kind = TokenKind::Reduce, .text = "/", .location = loc};
  case '\\':
    return Token{.kind = TokenKind::Scan, .text = "\\", .location = loc};
  case '"':
    return Token{.kind = TokenKind::RankOp, .text = "\"", .location = loc};
  case '(':
    return Token{.kind = TokenKind::LParen, .text = "(", .location = loc};
  case ')':
    return Token{.kind = TokenKind::RParen, .text = ")", .location = loc};
  case '[':
    return Token{.kind = TokenKind::LBracket, .text = "[", .location = loc};
  case ']':
    return Token{.kind = TokenKind::RBracket, .text = "]", .location = loc};
  case ';':
    return Token{.kind = TokenKind::Semicolon, .text = ";", .location = loc};
  case '\n':
    return Token{.kind = TokenKind::Newline, .text = "\n", .location = loc};
  default:
    return Token{.kind     = TokenKind::Error,
                 .text     = source_.substr(pos_ - 1, 1),
                 .location = loc};
  }
}

Token Lexer::nextToken() {
  if (hasPeeked_) {
    hasPeeked_ = false;
    return peekedToken_;
  }

  skipWhitespaceAndComments();

  if (isAtEnd()) {
    return Token{
        .kind     = TokenKind::EndOfFile,
        .text     = "",
        .location = currentLocation(),
    };
  }

  std::string_view rem = source_.substr(pos_);

  // APL high minus ¯ (\xc2\xaf or \xe2\x81\xbb) followed by digit or dot
  if (rem.starts_with("\xc2\xaf") || rem.starts_with("\xe2\x81\xbb")) {
    return scanNumber(true);
  }

  // J-style negative number: _5 or _.5
  if (rem.size() >= 2 && rem[0] == '_' &&
      (std::isdigit(static_cast<unsigned char>(rem[1])) || rem[1] == '.')) {
    return scanNumber(true);
  }

  // Number literal: digit
  if (std::isdigit(static_cast<unsigned char>(peekChar()))) {
    return scanNumber(false);
  }

  // String literals
  if (peekChar() == '\'') {
    return scanString('\'');
  }
  if (peekChar() == '"') {
    // In J, '"' alone or followed by a number/whitespace is the rank
    // conjunction (e.g. u"n or u " n). If there is a closing quote on the same
    // line with content, treat as string.
    std::size_t closing = source_.find('"', pos_ + 1);
    std::size_t newline = source_.find('\n', pos_ + 1);
    if (closing != std::string_view::npos &&
        (newline == std::string_view::npos || closing < newline) &&
        closing > pos_ + 1) {
      return scanString('"');
    }
    advanceChar();
    return Token{
        .kind = TokenKind::RankOp, .text = "\"", .location = currentLocation()};
  }

  // Dimension reference: starts with "d." followed by alphanumeric (e.g. d.1,
  // d.doc, d.clone)
  if (rem.size() >= 3 && rem.starts_with("d.") &&
      (std::isalnum(static_cast<unsigned char>(rem[2])) || rem[2] == '_')) {
    return scanDimension();
  }

  // Identifiers, names, or J-style keywords (e.g. i., each., time.)
  if (std::isalpha(static_cast<unsigned char>(peekChar())) ||
      peekChar() == '_') {
    return scanIdentifierOrKeyword();
  }

  // Operators, glyphs, delimiters, and punctuation
  return scanSymbolOrGlyph();
}

} // namespace xanadu::vpl
