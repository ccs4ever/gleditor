/**
 * @file token.hpp
 * @brief Token definitions and source metadata for VQL lexer and parser.
 */
#ifndef COMMON_XANADU_VQL_TOKEN_HPP
#define COMMON_XANADU_VQL_TOKEN_HPP

#include <cstdint>
#include <string>
#include <string_view>

#include "common/xanadu/scanner_base.hpp"

namespace xanadu::vql {

using SourceLocation = ::xanadu::SourceLocation;

enum class TokenKind : std::uint8_t {
  EndOfFile,
  Error,

  // Anchors & Sigils
  Home,        ///< "##"
  NamedStore,  ///< "##NAME"
  Root,        ///< "#"
  Cursor,      ///< "^"
  NamedCursor, ///< "^NAME"
  Variable,    ///< "$var"
  Dot,         ///< "."
  Slash,       ///< "/"
  DoubleColon, ///< "::"
  Bang,        ///< "!"
  Percent,     ///< "%"
  CloneJoin,   ///< "><"
  Assign,      ///< ":="
  Question,    ///< "?"
  DerefMaster, ///< ">"

  // Delimiters
  OpenParen,    ///< "("
  CloseParen,   ///< ")"
  OpenBracket,  ///< "["
  CloseBracket, ///< "]"
  OpenBrace,    ///< "{"
  CloseBrace,   ///< "}"
  Comma,        ///< ","

  // Comparisons & Operators
  Equal,        ///< "="
  NotEqual,     ///< "!="
  LessThan,     ///< "<"
  GreaterThan,  ///< ">"
  LessEqual,    ///< "<="
  GreaterEqual, ///< ">="
  Plus,         ///< "+"
  Minus,        ///< "-"
  Star,         ///< "*"

  // Keywords
  KwFor,
  KwIn,
  KwLet,
  KwWhere,
  KwReturn,
  KwWeave,
  KwIf,
  KwElse,
  KwAnd,
  KwOr,
  KwNot,
  KwTrue,
  KwFalse,

  // Literals & Identifiers
  Identifier,
  StringLiteral,
  IntegerLiteral,
  FloatLiteral,
  BareLiteral,
};

struct Token {
  TokenKind kind{TokenKind::EndOfFile};
  std::string_view text{};
  SourceLocation loc{};
  std::int64_t intValue{0};
  double floatValue{0.0};
  std::string stringValue{};

  [[nodiscard]] bool is(TokenKind k) const noexcept { return kind == k; }
};

[[nodiscard]] std::string_view tokenKindName(TokenKind kind) noexcept;

} // namespace xanadu::vql

#endif // COMMON_XANADU_VQL_TOKEN_HPP
