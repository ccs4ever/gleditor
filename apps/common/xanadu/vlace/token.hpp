/**
 * @file token.hpp
 * @brief Token definitions for vlace, the purely functional language over
 *        Vortex (design/vlace-functional-language.md §5.1).
 */
#ifndef COMMON_XANADU_VLACE_TOKEN_HPP
#define COMMON_XANADU_VLACE_TOKEN_HPP

#include <cstdint>
#include <string>
#include <string_view>

#include "common/xanadu/scanner_base.hpp"

namespace xanadu::vlace {

using SourceLocation = ::xanadu::SourceLocation;

enum class TokenKind : std::uint8_t {
  EndOfFile,
  Error,

  // Identifiers & literals
  Identifier,     ///< lowercase-initial: x, foldr, map
  Constructor,    ///< uppercase-initial: Nil, Cons, True, Point
  IntegerLiteral, ///< 0, -7, 0xFF, 0o17, 0b1010
  FloatLiteral,   ///< 3.14, 1.0e-9
  StringLiteral,  ///< "hello"
  CharLiteral,    ///< 'a', backslash-n

  // Keywords (reserved; not usable as identifiers, spec section 5.1)
  KwLet,
  KwLetrec,
  KwIn,
  KwIf,
  KwThen,
  KwElse,
  KwCase,
  KwOf,
  KwFn,
  KwData,
  KwType,
  KwImport,
  KwAs,
  KwInfixl,
  KwInfixr,
  KwInfix,

  // Reserved symbols (spec section 5.1)
  Equals,       ///< "="
  FatArrow,     ///< "=>"
  Bar,          ///< "|"
  Underscore,   ///< "_"
  Dot,          ///< "."
  Comma,        ///< ","
  Semicolon,    ///< ";"
  OpenParen,    ///< "("
  CloseParen,   ///< ")"
  OpenBracket,  ///< "["
  CloseBracket, ///< "]"
  OpenBrace,    ///< "{"
  CloseBrace,   ///< "}"
  DoubleColon,  ///< "::"
  ThinArrow,    ///< "->"
  At,           ///< "@"

  // A maximal run of + - * / < > = ! & | : . ^ % that is not a reserved
  // symbol above. The parser resolves fixity.
  Operator,
};

struct Token {
  TokenKind kind{TokenKind::EndOfFile};
  std::string_view text;
  SourceLocation loc{};
  std::int64_t intValue{0};
  double floatValue{0.0};
  std::string stringValue;

  [[nodiscard]] bool is(TokenKind k) const noexcept { return kind == k; }
};

[[nodiscard]] std::string_view tokenKindName(TokenKind kind) noexcept;

} // namespace xanadu::vlace

#endif // COMMON_XANADU_VLACE_TOKEN_HPP
