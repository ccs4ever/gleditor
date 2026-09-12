/**
 * @file token.hpp
 * @brief Token definitions for Prolog on Vortex / Vlog.
 */
#ifndef COMMON_XANADU_VPROLOG_TOKEN_HPP
#define COMMON_XANADU_VPROLOG_TOKEN_HPP

#include <cstdint>
#include <string>
#include <string_view>

#include "common/xanadu/scanner_base.hpp"

namespace xanadu::vprolog {

using SourceLocation = ::xanadu::SourceLocation;

enum class TokenKind : std::uint8_t {
  EndOfFile,
  Error,

  // Identifiers & Literals
  Atom,           ///< Lowercase identifier or quoted atom (e.g. cat, 'foo bar')
  Variable,       ///< Uppercase identifier or underscore (e.g. X, _Var, _)
  IntegerLiteral, ///< e.g. 42, -5
  FloatLiteral,   ///< e.g. 3.14159
  StringLiteral,  ///< e.g. "text"

  // Delimiters
  OpenParen,    ///< "("
  CloseParen,   ///< ")"
  OpenBracket,  ///< "["
  CloseBracket, ///< "]"
  OpenBrace,    ///< "{"
  CloseBrace,   ///< "}"
  Comma,        ///< "," (conjunction)
  Dot,          ///< "." (clause / query terminator)
  Pipe,         ///< "|" (list head/tail divider)
  Semicolon,    ///< ";" (disjunction)

  // Operators & Control
  Neck,           ///< ":-" (rule neck)
  Query,          ///< "?-" (query prompt)
  Cut,            ///< "!"
  Arrow,          ///< "->" (if-then)
  Not,            ///< "\+" (negation by failure)
  Equal,          ///< "=" (unification)
  NotEqual,       ///< "\=" (not unifiable)
  StrictEqual,    ///< "==" (identical)
  StrictNotEqual, ///< "\==" (not identical)
  Univ,           ///< "=.." (univ decomposer)
  Is,             ///< "is" (arithmetic evaluation)
  EqualArith,     ///< "=:=" (arithmetic equal)
  NotEqualArith,  ///< "=\=" (arithmetic not equal)
  Less,           ///< "<"
  Greater,        ///< ">"
  LessEqual,      ///< "=<"
  GreaterEqual,   ///< ">="
  Plus,           ///< "+"
  Minus,          ///< "-"
  Star,           ///< "*"
  Slash,          ///< "/"
  IntDiv,         ///< "//"
  Mod,            ///< "mod"
  Rem,            ///< "rem"
  Power,          ///< "**"
  BitAnd,         ///< "/\"
  BitOr,          ///< "\/"
  BitShiftLeft,   ///< "<<"
  BitShiftRight,  ///< ">>"
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

} // namespace xanadu::vprolog

#endif // COMMON_XANADU_VPROLOG_TOKEN_HPP
