/**
 * @file token.hpp
 * @brief Token definitions for vagent, the agent-oriented language on Vortex
 * (design/vagent-agent-language.md §8.1).
 */
#ifndef COMMON_XANADU_VAGENT_TOKEN_HPP
#define COMMON_XANADU_VAGENT_TOKEN_HPP

#include <cstdint>
#include <string>
#include <string_view>

#include "common/xanadu/scanner_base.hpp"

namespace xanadu::vagent {

using SourceLocation = ::xanadu::SourceLocation;

// The reserved words of §8.1. One list feeds the enum, the spelling table and
// the keyword lookup so they cannot drift apart. Contextual words (`as`,
// `ensures`, `from`, `holds`, `inner`, `maintain`, `module`, `occurrence`,
// `outer`, `parent`, `perform`, `preempts`, `priority`, `requires`, `succeed`,
// `when`) are deliberately absent: they lex as identifiers and the parser
// recognises them by position.
// clang-format off
#define XANADU_VAGENT_KEYWORDS(X)                                              \
  X(Achieve, "achieve")       X(Adopt, "adopt")         X(Agent, "agent")      \
  X(Answer, "answer")         X(Ask, "ask")             X(Await, "await")      \
  X(Behavior, "behavior")     X(Believe, "believe")     X(Believes, "believes")\
  X(By, "by")                 X(Capacity, "capacity")   X(Consume, "consume")  \
  X(Def, "def")               X(Do, "do")               X(Drop, "drop")        \
  X(Effect, "effect")         X(Else, "else")           X(Emit, "emit")        \
  X(Event, "event")           X(Extends, "extends")     X(Fail, "fail")        \
  X(Findall, "findall")       X(For, "for")             X(Forget, "forget")    \
  X(Goal, "goal")             X(If, "if")               X(Implements, "implements") \
  X(Import, "import")         X(In, "in")               X(Install, "install")  \
  X(Key, "key")               X(Let, "let")             X(Mount, "mount")      \
  X(Native, "native")         X(None, "none")           X(Not, "not")          \
  X(On, "on")                 X(Plan, "plan")           X(Predicate, "predicate") \
  X(Question, "question")     X(Record, "record")       X(Return, "return")    \
  X(Revise, "revise")         X(Self, "self")           X(Skill, "skill")      \
  X(Spawn, "spawn")           X(Stop, "stop")           X(Tell, "tell")        \
  X(Terminate, "terminate")   X(Then, "then")           X(To, "to")            \
  X(Uninstall, "uninstall")   X(Unmount, "unmount")     X(Untell, "untell")    \
  X(Upgrade, "upgrade")       X(Uses, "uses")           X(Var, "var")          \
  X(Where, "where")           X(While, "while")         X(Within, "within")    \
  X(Yield, "yield")
// clang-format on

enum class TokenKind : std::uint8_t {
  EndOfFile,

  Identifier,
  IntegerLiteral,
  FloatLiteral,
  StringLiteral,
  DurationLiteral, ///< `250ms`, `2s`, `5min`
  True,
  False,

// Reserved words, contiguous so isKeyword() is a range test.
#define XANADU_VAGENT_KIND(name, text) name,
  XANADU_VAGENT_KEYWORDS(XANADU_VAGENT_KIND)
#undef XANADU_VAGENT_KIND

  // Delimiters
  OpenParen,
  CloseParen,
  OpenBracket,
  CloseBracket,
  OpenBrace,
  CloseBrace,
  Comma,
  Semicolon,
  Colon, ///< ":" -- type ascription and module paths
  Dot,

  // Operators
  Assign,       ///< ":="  irreversible
  RevAssign,    ///< "<-"  reversible; one token, so `a<-1` assigns
  Plus,         ///< "+"
  Minus,        ///< "-"
  Star,         ///< "*"   multiply, or unary size
  Slash,        ///< "/"
  Percent,      ///< "%"
  Concat,       ///< "++"
  Less,         ///< "<"
  LessEqual,    ///< "<="
  Greater,      ///< ">"
  GreaterEqual, ///< ">="
  EqualEqual,   ///< "=="
  NotEqual,     ///< "!="
  Bang,         ///< "!"   unary: generate elements
  Ampersand,    ///< "&"   conjunction
  Bar,          ///< "|"   alternation
  QuestionMark, ///< "?"   string scanning, or a logic variable in a term
};

inline constexpr TokenKind firstKeyword = TokenKind::Achieve;
inline constexpr TokenKind lastKeyword  = TokenKind::Yield;

[[nodiscard]] constexpr bool isKeyword(TokenKind k) noexcept {
  return k >= firstKeyword && k <= lastKeyword;
}

/// The grammar's `Word`: an identifier or any keyword. Accepted after `.`, in
/// field declarations, as record-literal keys and in module paths.
[[nodiscard]] constexpr bool isWord(TokenKind k) noexcept {
  return k == TokenKind::Identifier || isKeyword(k) || k == TokenKind::True ||
         k == TokenKind::False;
}

struct Token {
  TokenKind kind{TokenKind::EndOfFile};
  std::string_view text;
  SourceLocation loc{};
  /// A newline (or a comment spanning one) lies between the previous token and
  /// this one. The lexer records it and the parser applies the newline rule
  /// of §8.1, because only the parser knows whether it is inside a block.
  bool newlineBefore{false};
  std::int64_t intValue{0};
  double floatValue{0.0};
  std::int64_t durationMs{0}; ///< DurationLiteral, in milliseconds (§8.7)
  std::string stringValue;    ///< decoded StringLiteral

  [[nodiscard]] bool is(TokenKind k) const noexcept { return kind == k; }
};

[[nodiscard]] std::string_view tokenKindName(TokenKind kind) noexcept;

} // namespace xanadu::vagent

#endif // COMMON_XANADU_VAGENT_TOKEN_HPP
