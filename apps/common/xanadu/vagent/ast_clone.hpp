#ifndef COMMON_XANADU_VAGENT_AST_CLONE_HPP
#define COMMON_XANADU_VAGENT_AST_CLONE_HPP

#include "common/xanadu/vagent/ast.hpp"

namespace xanadu::vagent {

// Deep copies for per-site instantiation (§5.6 of
// design/vagent-metaprogramming.md): every ExprPtr field — children, scope,
// recipients, deadline, guard and recovery — is cloned, and every value
// field (text, names, annotation, binding, loc, the resolve pass's scope
// stamp) is copied verbatim, so a substituted tree keeps both its binding
// identities and the loc that maps diagnostics back through `+o:c` inner
// offsets. Cloning cannot share nodes: kernel side tables key on
// `const Expr *`, so two emissions of one node would double-register it.
[[nodiscard]] ExprPtr cloneExpr(const Expr &expr);

// The ExprPtr overload treats null as a valid field value — it clones a
// present node and propagates an absent metadata child as nullptr.
[[nodiscard]] ExprPtr cloneExpr(const ExprPtr &expr);

// Decl-granularity for M13's per-decl template instantiation; nested
// members, contracts and goal clauses are cloned with it.
[[nodiscard]] Decl cloneDecl(const Decl &decl);

[[nodiscard]] Program cloneProgram(const Program &program);

} // namespace xanadu::vagent

#endif // COMMON_XANADU_VAGENT_AST_CLONE_HPP
