/**
 * @file token.hpp
 * @brief Token definitions for vicon, the goal-directed language on Vortex.
 */
#ifndef COMMON_XANADU_VICON_TOKEN_HPP
#define COMMON_XANADU_VICON_TOKEN_HPP

#include <cstdint>
#include <string>
#include <string_view>

#include "common/xanadu/scanner_base.hpp"

namespace xanadu::vicon {

using SourceLocation = ::xanadu::SourceLocation;

enum class TokenKind : std::uint8_t {
  EndOfFile,
  Error,

  // Identifiers & Literals
  Identifier,     ///< e.g. write, line, x
  Keyword,        ///< &name (e.g. &subject, &pos, &null, &fail)
  IntegerLiteral, ///< e.g. 42
  RealLiteral,    ///< e.g. 3.14, 1e-3
  StringLiteral,  ///< e.g. "text" (double-quoted)
  CsetLiteral,    ///< e.g. 'abc' (single-quoted cset)

  // Delimiters
  OpenParen,    ///< "("
  CloseParen,   ///< ")"
  OpenBracket,  ///< "["
  CloseBracket, ///< "]"
  OpenBrace,    ///< "{" (explicit expression sequence)
  CloseBrace,   ///< "}"
  Comma,        ///< ","
  Semicolon,    ///< ";" or a significant newline (statement separator)

  // Reserved words
  If,       ///< "if"
  Then,     ///< "then"
  Else,     ///< "else"
  While,    ///< "while"
  Do,       ///< "do"
  Until,    ///< "until"
  Every,    ///< "every"
  Case,     ///< "case"
  Of,       ///< "of"
  Break,    ///< "break"
  Next,     ///< "next"
  Return,   ///< "return"
  Fail,     ///< "fail"
  Suspend,  ///< "suspend"
  Repeat,   ///< "repeat"
  To,       ///< "to"
  By,       ///< "by"
  Not,      ///< "not"
  Procedure,///< "procedure"
  End,      ///< "end"
  Local,    ///< "local"
  Global,   ///< "global"
  Static,   ///< "static"

  // Operators
  Assign,        ///< ":=" (irreversible)
  RevAssign,     ///< "<-" (reversible, trailed)
  Alternation,   ///< "|" (generator alternation)
  Concat,        ///< "||" (string concatenation)
  Limit,         ///< "\" (result limitation)
  Scan,          ///< "?" (string scanning environment)
  Bang,          ///< "!" (element generation)
  Plus,          ///< "+"
  Minus,         ///< "-"
  Star,          ///< "*"
  Slash,         ///< "/"
  Percent,       ///< "%"
  Caret,         ///< "^" (exponentiation)
  Equal,         ///< "="  (numeric equal -- produces right or fails)
  NotEqual,      ///< "~="
  Less,          ///< "<"
  LessEqual,     ///< "<="
  Greater,       ///< ">"
  GreaterEqual,  ///< ">="
  StringEqual,   ///< "==" (string comparison family)
  StringNotEq,   ///< "~=="
  StringLess,    ///< "<<"
  StringLessEq,  ///< "<<="
  StringGreater, ///< ">>"
  StringGtrEq,   ///< ">>="
  Ampersand,     ///< "&" (conjunction; also introduces Keyword)
  At,            ///< "@" (co-expression activation, optional)
};

struct Token {
  TokenKind kind{TokenKind::EndOfFile};
  std::string_view text;
  SourceLocation loc{};
  std::int64_t intValue{0};
  double floatValue{0.0};
  std::string stringValue; ///< decoded literal text / keyword name

  [[nodiscard]] bool is(TokenKind k) const noexcept { return kind == k; }
};

[[nodiscard]] std::string_view tokenKindName(TokenKind kind) noexcept;

} // namespace xanadu::vicon

#endif // COMMON_XANADU_VICON_TOKEN_HPP
