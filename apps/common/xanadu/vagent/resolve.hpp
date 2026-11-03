#ifndef COMMON_XANADU_VAGENT_RESOLVE_HPP
#define COMMON_XANADU_VAGENT_RESOLVE_HPP

#include <cstddef>
#include <expected>
#include <vector>

#include "common/xanadu/vagent/ast.hpp"

namespace xanadu::vagent {

struct ResolveLimits {
  // Lookups, scope copies and minted bindings — the same accounting role as
  // CheckLimits::maxResolutionWork, budgeted separately so resolve can be
  // bounded on its own when the pipeline calls it.
  std::size_t maxResolutionWork{1'000'000};
  // Public-AST callers do not necessarily pass through the parser's limits.
  std::size_t maxExpressionDepth{256};
  std::size_t maxDeclarationDepth{256};
};

// The §5.6 resolve-only pass (design/vagent-metaprogramming.md): the
// checker's environment construction extracted into a standalone walk so the
// expand stage shares one scope truth instead of duplicating it. The walk
// visits every expression the checker's infer() would — implicit names,
// owner parameters and fields, declaration parameters, plan bindings,
// handler occurrence/as-bindings, block and branch environment copies,
// let/term/await/for bindings and the recovery `cause` — and writes each
// visited Expr's ScopeStamp: `scope` names the scope the node resolved in
// (the enclosing scope for most kinds; the binding's owning scope for an
// Identifier or LogicVariable, so the pair (scope, ordinal) is the binding's
// identity and two bindings in one scope never share a stamp). A name the
// environment does not bind keeps a scope-only stamp — name errors stay the
// checker's to report; resolve is not a second diagnostic authority.
//
// This is an extraction, not a checker rewrite: check() keeps its own env
// walk for now and the stamps are what expand's cap$N aliasing, and the
// checker's later stamp-honoring rule, consume. Per-literal seeding for
// code"…" bodies is expand's entry point into this pass — the Resolver's
// Scope records are keyed by ScopeStamp::scope so a seeded subtree overload
// is a narrow addition when phase 1 needs it.
[[nodiscard]] std::expected<void, std::vector<Diagnostic>>
resolve(Program &program, ResolveLimits limits = {});

} // namespace xanadu::vagent

#endif // COMMON_XANADU_VAGENT_RESOLVE_HPP
