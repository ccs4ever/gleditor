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
                                               "consume",
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
  }
  return "UnknownDiagnostic";
}

std::string formatDiagnostic(const Diagnostic &diagnostic) {
  return std::to_string(diagnostic.loc.line) + ":" +
         std::to_string(diagnostic.loc.column) + ": " +
         std::string(diagnosticCodeName(diagnostic.code)) + ": " +
         diagnostic.detail;
}

} // namespace xanadu::vagent
