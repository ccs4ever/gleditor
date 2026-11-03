#ifndef COMMON_XANADU_VAGENT_TOKEN_HPP
#define COMMON_XANADU_VAGENT_TOKEN_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/scanner_base.hpp"

namespace xanadu::vagent {

using SourceLocation = ::xanadu::SourceLocation;

enum class TokenKind : std::uint8_t {
  EndOfFile,
  Identifier,
  Keyword,
  IntegerLiteral,
  FloatLiteral,
  StringLiteral,
  PrefixedStringLiteral,
  DurationLiteral,
  Newline,
  OpenParen,
  CloseParen,
  OpenBracket,
  CloseBracket,
  OpenBrace,
  CloseBrace,
  Comma,
  Semicolon,
  Colon,
  Dot,
  Assign,
  RevAssign,
  Alternation,
  Concat,
  Scan,
  Bang,
  Plus,
  Minus,
  Star,
  Slash,
  Percent,
  Equal,
  NotEqual,
  Less,
  LessEqual,
  Greater,
  GreaterEqual,
  Ampersand,
};

// Owned spellings keep diagnostics and AST construction valid after a source
// buffer is replaced by the REPL or a module loader.
struct Token {
  TokenKind kind{TokenKind::EndOfFile};
  std::string text;
  std::string stringValue;
  SourceLocation loc{};
};

enum class DiagnosticCode : std::uint8_t {
  Lexical,
  Syntax,
  InvalidIdentifier,
  DuplicateName,
  UnknownName,
  TypeMismatch,
  InvalidPosition,
  ImpureCondition,
  MissingDeadline,
  InvalidDeclaration,
  // A construct the grammar accepts but the current pipeline phase refuses
  // — the phase-1 expand skeleton's answer to every Splice/Static node.
  Unsupported,
};

struct Diagnostic {
  DiagnosticCode code{DiagnosticCode::Syntax};
  std::string detail;
  SourceLocation loc{};
  // §5.1 inner offsets (design/vagent-metaprogramming.md): a diagnostic
  // raised inside a code"…" body is reported at the literal's own loc
  // qualified by each enclosing level's '+<off-line>:<off-col>' arm,
  // outermost first — the src:m:l:c+o:c convention. Built by code_path's
  // innerDiagnostic; empty for every other diagnostic.
  std::vector<SourceLocation> inner;
};

[[nodiscard]] std::string_view diagnosticCodeName(DiagnosticCode code) noexcept;
[[nodiscard]] std::string formatDiagnostic(const Diagnostic &diagnostic);
[[nodiscard]] bool isReservedWord(std::string_view word) noexcept;

} // namespace xanadu::vagent

#endif // COMMON_XANADU_VAGENT_TOKEN_HPP
