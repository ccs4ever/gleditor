#include "common/xanadu/vagent/token.hpp"

#include <algorithm>
#include <iterator>

namespace xanadu::vagent {

bool isReservedWord(std::string_view word) noexcept {
  static constexpr std::string_view words[] = {std::string_view{"achieve"},
                                               "adopt",
                                               "agent",
                                               "answer",
                                               "ask",
                                               "await",
                                               "behavior",
                                               "believe",
                                               "believes",
                                               "by",
                                               "capacity",
                                               "case",
                                               "consume",
                                               "default",
                                               "def",
                                               "do",
                                               "drop",
                                               "effect",
                                               "else",
                                               "emit",
                                               "event",
                                               "extends",
                                               "fail",
                                               "findall",
                                               "for",
                                               "forget",
                                               "goal",
                                               "if",
                                               "implements",
                                               "import",
                                               "in",
                                               "install",
                                               "key",
                                               "let",
                                               "Map",
                                               "match",
                                               "mount",
                                               "native",
                                               "none",
                                               "not",
                                               "on",
                                               "plan",
                                               "predicate",
                                               "question",
                                               "record",
                                               "return",
                                               "revise",
                                               "self",
                                               "skill",
                                               "spawn",
                                               "stop",
                                               "tell",
                                               "terminate",
                                               "then",
                                               "to",
                                               "uninstall",
                                               "unmount",
                                               "untell",
                                               "uses",
                                               "var",
                                               "where",
                                               "while",
                                               "with",
                                               "within",
                                               "yield",
                                               "true",
                                               "false"};
  return std::ranges::find(words, word) != std::end(words);
}

std::string_view diagnosticCodeName(DiagnosticCode code) noexcept {
  switch (code) {
  case DiagnosticCode::Lexical:
    return "Lexical";
  case DiagnosticCode::Syntax:
    return "Syntax";
  case DiagnosticCode::InvalidIdentifier:
    return "InvalidIdentifier";
  case DiagnosticCode::DuplicateName:
    return "DuplicateName";
  case DiagnosticCode::UnknownName:
    return "UnknownName";
  case DiagnosticCode::TypeMismatch:
    return "TypeMismatch";
  case DiagnosticCode::InvalidPosition:
    return "InvalidPosition";
  case DiagnosticCode::ImpureCondition:
    return "ImpureCondition";
  case DiagnosticCode::MissingDeadline:
    return "MissingDeadline";
  case DiagnosticCode::InvalidDeclaration:
    return "InvalidDeclaration";
  case DiagnosticCode::Unsupported:
    return "Unsupported";
  }
  return "UnknownDiagnostic";
}

std::string formatDiagnostic(const Diagnostic &diagnostic) {
  std::string result = std::to_string(diagnostic.loc.line) + ":" +
                       std::to_string(diagnostic.loc.column);
  // §5.1: positions inside a code"…" body qualify the literal's own loc
  // with one '+<off-line>:<off-col>' arm per enclosing level.
  for (const auto &arm : diagnostic.inner) {
    result += "+" + std::to_string(arm.line) + ":" + std::to_string(arm.column);
  }
  result += ": " + std::string(diagnosticCodeName(diagnostic.code)) + ": " +
            diagnostic.detail;
  return result;
}

} // namespace xanadu::vagent
