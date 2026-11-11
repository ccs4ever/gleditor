#include "common/xanadu/vagent/code_path.hpp"

#include <string>
#include <utility>

namespace xanadu::vagent {
namespace {

std::string_view suspendingSpelling(ExprKind kind) {
  switch (kind) {
  case ExprKind::AwaitTerm:
  case ExprKind::AwaitEvent:
    return "await";
  case ExprKind::Achieve:
    return "achieve";
  case ExprKind::Ask:
    return "ask";
  case ExprKind::Yield:
    return "yield";
  default:
    return {};
  }
}

// §5.1 + M6: a fragment has no step path, so `return` and the suspending
// statements are refused inside code"…". Effect calls are not visible at
// this level — call shape versus callee signature is the checker's truth.
// A nested code"…" literal stays raw text here; its own parseCodePath
// call narrows it at its own level.
const Expr *suspendingNode(const Expr &node) {
  switch (node.kind) {
  case ExprKind::Return:
  case ExprKind::AwaitTerm:
  case ExprKind::AwaitEvent:
  case ExprKind::Achieve:
  case ExprKind::Ask:
  case ExprKind::Yield:
    return &node;
  default:
    break;
  }
  for (const auto &child : node.children) {
    if (child)
      if (const auto *hit = suspendingNode(*child)) return hit;
  }
  for (const ExprPtr *field :
       {&node.scope, &node.recipients, &node.deadline, &node.guard,
        &node.recovery}) {
    if (*field)
      if (const auto *hit = suspendingNode(**field)) return hit;
  }
  return nullptr;
}

} // namespace

Diagnostic innerDiagnostic(Diagnostic inner, SourceLocation literal) {
  inner.inner.insert(inner.inner.begin(), inner.loc);
  inner.loc = literal;
  return inner;
}

std::expected<CodeBody, Diagnostic>
parseCodePath(std::string_view source, SourceLocation loc,
              ParseLimits limits) {
  auto parsed = Parser::parseCodeBody(source, limits);
  if (!parsed) {
    return std::unexpected(
        innerDiagnostic(std::move(parsed.error()), loc));
  }
  if (const auto *hit = suspendingNode(*parsed->body)) {
    const std::string detail =
        hit->kind == ExprKind::Return
            ? "'return' is not allowed inside a code literal (a "
              "fragment's produced value is its last expression's; "
              "early produce is written with 'if')"
            : "'" + std::string(suspendingSpelling(hit->kind)) +
                  "' is not allowed inside a code literal (fragments "
                  "cannot suspend; await/achieve/ask/yield are refused "
                  "per M6)";
    return std::unexpected(innerDiagnostic(
        Diagnostic{DiagnosticCode::InvalidPosition, detail, hit->loc},
        loc));
  }
  return parsed;
}

} // namespace xanadu::vagent
