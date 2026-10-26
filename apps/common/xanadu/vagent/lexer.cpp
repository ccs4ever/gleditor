#include "common/xanadu/vagent/lexer.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

namespace xanadu::vagent {
namespace {

bool identifierStart(std::optional<char> input) noexcept {
  if (!input) return false;
  const char c = *input;
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool identifierPart(std::optional<char> input) noexcept {
  if (!input) return false;
  const char c = *input;
  return identifierStart(c) || (c >= '0' && c <= '9');
}

std::optional<unsigned> digitValue(std::optional<char> input) noexcept {
  if (!input) return std::nullopt;
  const char c = *input;
  if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0');
  if (c >= 'a' && c <= 'f') return static_cast<unsigned>(c - 'a' + 10);
  if (c >= 'A' && c <= 'F') return static_cast<unsigned>(c - 'A' + 10);
  return std::nullopt;
}

bool isDigit(std::optional<char> c, int base) noexcept {
  const auto value = digitValue(c);
  return value.has_value() && *value < static_cast<unsigned>(base);
}

void appendUtf8(std::string &output, std::uint32_t value) {
  if (value <= 0x7fU) {
    output += static_cast<char>(value);
  } else if (value <= 0x7ffU) {
    output += static_cast<char>(0xc0U | (value >> 6U));
    output += static_cast<char>(0x80U | (value & 0x3fU));
  } else if (value <= 0xffffU) {
    output += static_cast<char>(0xe0U | (value >> 12U));
    output += static_cast<char>(0x80U | ((value >> 6U) & 0x3fU));
    output += static_cast<char>(0x80U | (value & 0x3fU));
  } else {
    output += static_cast<char>(0xf0U | (value >> 18U));
    output += static_cast<char>(0x80U | ((value >> 12U) & 0x3fU));
    output += static_cast<char>(0x80U | ((value >> 6U) & 0x3fU));
    output += static_cast<char>(0x80U | (value & 0x3fU));
  }
}

} // namespace

std::optional<char> Lexer::peek(std::size_t ahead) const noexcept {
  if (position_ >= source_.size() || ahead >= source_.size() - position_)
    return std::nullopt;
  return source_[position_ + ahead];
}

bool Lexer::atEnd() const noexcept { return position_ >= source_.size(); }

SourceLocation Lexer::location() const noexcept {
  return {.line = line_, .column = column_, .offset = position_};
}

std::optional<char> Lexer::advance() noexcept {
  if (atEnd()) return std::nullopt;
  const char c = source_[position_++];
  if (c == '\n' || (c == '\r' && peek() != '\n')) {
    ++line_;
    column_ = 1;
  } else {
    ++column_;
  }
  return c;
}

Token Lexer::token(TokenKind kind, SourceLocation start) const {
  return {.kind        = kind,
          .text        = source_.substr(start.offset, position_ - start.offset),
          .stringValue = {},
          .loc         = start};
}

Diagnostic Lexer::error(std::string detail, SourceLocation loc) const {
  return {
      .code = DiagnosticCode::Lexical, .detail = std::move(detail), .loc = loc};
}

Token Lexer::newline() {
  const auto start = location();
  const char first = advance().value();
  if (first == '\r' && peek() == '\n') advance();
  return token(TokenKind::Newline, start);
}

Token Lexer::identifier() {
  const auto start = location();
  while (identifierPart(peek())) advance();
  Token result = token(TokenKind::Identifier, start);
  if (isReservedWord(result.text)) result.kind = TokenKind::Keyword;
  return result;
}

std::expected<void, Diagnostic> Lexer::digits(int base) {
  if (!isDigit(peek(), base)) {
    return std::unexpected(
        error("expected a digit in numeric literal", location()));
  }
  for (;;) {
    if (isDigit(peek(), base)) {
      advance();
    } else if (peek() == '_') {
      const auto underscore = location();
      advance();
      if (!isDigit(peek(), base)) {
        return std::unexpected(
            error("numeric separators must occur between digits", underscore));
      }
    } else {
      return {};
    }
  }
}

std::expected<Token, Diagnostic> Lexer::number() {
  const auto start = location();
  bool hexadecimal = false;
  bool floating    = false;
  if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
    hexadecimal = true;
    advance();
    advance();
  }
  if (auto result = digits(hexadecimal ? 16 : 10); !result) {
    return std::unexpected(result.error());
  }
  if (!hexadecimal && peek() == '.' && isDigit(peek(1), 10)) {
    floating = true;
    advance();
    if (auto result = digits(10); !result)
      return std::unexpected(result.error());
  }
  if (!hexadecimal && (peek() == 'e' || peek() == 'E')) {
    floating = true;
    advance();
    if (peek() == '+' || peek() == '-') advance();
    if (auto result = digits(10); !result)
      return std::unexpected(result.error());
  }
  const auto numericEnd = position_;
  while (identifierPart(peek())) advance();
  const auto suffix =
      std::string_view(source_).substr(numericEnd, position_ - numericEnd);
  if (!suffix.empty() &&
      (hexadecimal || (suffix != "ms" && suffix != "s" && suffix != "min"))) {
    return std::unexpected(error("invalid numeric literal suffix", start));
  }

  std::string normalized =
      source_.substr(start.offset, numericEnd - start.offset);
  std::erase(normalized, '_');
  if (floating) {
    double value{};
    const auto result = std::from_chars(
        normalized.data(), normalized.data() + normalized.size(), value);
    if (result.ec != std::errc{} ||
        result.ptr != normalized.data() + normalized.size() ||
        !std::isfinite(value)) {
      return std::unexpected(error("floating literal is out of range", start));
    }
  } else {
    std::uint64_t value{};
    const auto *begin = normalized.data() + (hexadecimal ? 2 : 0);
    const auto result =
        std::from_chars(begin, normalized.data() + normalized.size(), value,
                        hexadecimal ? 16 : 10);
    // One extra magnitude is necessary to spell the signed minimum as a
    // unary minus followed by an integer token. The checker rejects it when
    // it occurs without that unary minus.
    constexpr auto signedMinimumMagnitude =
        std::uint64_t{std::numeric_limits<std::int64_t>::max()} + 1U;
    if (result.ec != std::errc{} ||
        result.ptr != normalized.data() + normalized.size() ||
        value > signedMinimumMagnitude) {
      return std::unexpected(error("integer literal is out of range", start));
    }
  }
  return token(!suffix.empty() ? TokenKind::DurationLiteral
               : floating      ? TokenKind::FloatLiteral
                               : TokenKind::IntegerLiteral,
               start);
}

std::expected<Token, Diagnostic> Lexer::string() {
  const auto start = location();
  advance();
  std::string decoded;
  while (!atEnd() && peek() != '"') {
    const auto current = location();
    const auto c       = static_cast<unsigned char>(advance().value());
    if (c < 0x20U) {
      return std::unexpected(
          error("unescaped control character in string", current));
    }
    if (c != '\\') {
      decoded += static_cast<char>(c);
      continue;
    }
    if (atEnd()) break;
    switch (advance().value()) {
    case 'n':
      decoded += '\n';
      break;
    case 't':
      decoded += '\t';
      break;
    case '"':
      decoded += '"';
      break;
    case '\\':
      decoded += '\\';
      break;
    case 'u': {
      if (peek() != '{') {
        return std::unexpected(error("Unicode escape requires '{'", current));
      }
      advance();
      std::uint32_t value{};
      std::size_t count{};
      while (isDigit(peek(), 16)) {
        if (++count > 6) {
          return std::unexpected(
              error("Unicode escape has too many digits", current));
        }
        value = (value << 4U) | *digitValue(advance());
      }
      if (count == 0 || peek() != '}') {
        return std::unexpected(error("invalid Unicode escape", current));
      }
      advance();
      if (value > 0x10ffffU || (value >= 0xd800U && value <= 0xdfffU)) {
        return std::unexpected(
            error("Unicode escape is not a scalar value", current));
      }
      appendUtf8(decoded, value);
      break;
    }
    default:
      return std::unexpected(error("unknown string escape", current));
    }
  }
  if (atEnd())
    return std::unexpected(error("unterminated string literal", start));
  advance();
  Token result       = token(TokenKind::StringLiteral, start);
  result.stringValue = std::move(decoded);
  return result;
}

std::expected<Token, Diagnostic> Lexer::punctuation() {
  const auto start = location();
  const char c     = advance().value();
  auto pair        = [&](char next, TokenKind paired, TokenKind single) {
    if (peek() == next) {
      advance();
      return token(paired, start);
    }
    return token(single, start);
  };
  switch (c) {
  case '(':
    return token(TokenKind::OpenParen, start);
  case ')':
    return token(TokenKind::CloseParen, start);
  case '[':
    return token(TokenKind::OpenBracket, start);
  case ']':
    return token(TokenKind::CloseBracket, start);
  case '{':
    return token(TokenKind::OpenBrace, start);
  case '}':
    return token(TokenKind::CloseBrace, start);
  case ',':
    return token(TokenKind::Comma, start);
  case ';':
    return token(TokenKind::Semicolon, start);
  case '.':
    return token(TokenKind::Dot, start);
  case ':':
    return pair('=', TokenKind::Assign, TokenKind::Colon);
  case '+':
    return pair('+', TokenKind::Concat, TokenKind::Plus);
  case '-':
    return token(TokenKind::Minus, start);
  case '*':
    return token(TokenKind::Star, start);
  case '/':
    return token(TokenKind::Slash, start);
  case '%':
    return token(TokenKind::Percent, start);
  case '?':
    return token(TokenKind::Scan, start);
  case '!':
    return pair('=', TokenKind::NotEqual, TokenKind::Bang);
  case '<':
    if (peek() == '-') {
      advance();
      return token(TokenKind::RevAssign, start);
    }
    return pair('=', TokenKind::LessEqual, TokenKind::Less);
  case '>':
    return pair('=', TokenKind::GreaterEqual, TokenKind::Greater);
  case '=':
    if (peek() != '=')
      return std::unexpected(error("equality is spelled '=='", start));
    advance();
    return token(TokenKind::Equal, start);
  case '|':
    if (peek() == '|')
      return std::unexpected(error("use '|' or '++', not '||'", start));
    return token(TokenKind::Alternation, start);
  case '&':
    if (peek() == '&')
      return std::unexpected(error("conjunction is spelled '&'", start));
    return token(TokenKind::Ampersand, start);
  case '$':
    return std::unexpected(
        error("'$' names are reserved for agent state", start));
  default:
    return std::unexpected(error("unexpected character in source", start));
  }
}

std::expected<std::vector<Token>, Diagnostic> Lexer::tokenize() {
  std::vector<Token> result;
  while (!atEnd()) {
    const char c = peek().value();
    if (c == ' ' || c == '\t' || c == '\v' || c == '\f') {
      advance();
      continue;
    }
    if (c == '\n' || c == '\r') {
      result.push_back(newline());
      continue;
    }
    if (c == '/' && peek(1) == '/') {
      while (!atEnd() && peek() != '\n' && peek() != '\r') advance();
      continue;
    }
    if (c == '/' && peek(1) == '*') {
      const auto start = location();
      advance();
      advance();
      bool closed = false;
      while (!atEnd()) {
        if (peek() == '*' && peek(1) == '/') {
          advance();
          advance();
          closed = true;
          break;
        }
        if (peek() == '\n' || peek() == '\r') {
          result.push_back(newline());
        } else {
          advance();
        }
      }
      if (!closed)
        return std::unexpected(error("unterminated block comment", start));
      continue;
    }
    if (identifierStart(c)) {
      result.push_back(identifier());
      continue;
    }
    auto next = isDigit(c, 10) ? number() : c == '"' ? string() : punctuation();
    if (!next) return std::unexpected(next.error());
    result.push_back(std::move(*next));
  }
  result.push_back(token(TokenKind::EndOfFile, location()));
  return result;
}

} // namespace xanadu::vagent
