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
};

struct Diagnostic {
  DiagnosticCode code{DiagnosticCode::Syntax};
  std::string detail;
  SourceLocation loc{};
};

[[nodiscard]] std::string_view diagnosticCodeName(DiagnosticCode code) noexcept;
[[nodiscard]] std::string formatDiagnostic(const Diagnostic &diagnostic);
[[nodiscard]] bool isReservedWord(std::string_view word) noexcept;

} // namespace xanadu::vagent

#endif // COMMON_XANADU_VAGENT_TOKEN_HPP
