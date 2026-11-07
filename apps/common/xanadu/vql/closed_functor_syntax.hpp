#ifndef COMMON_XANADU_VQL_CLOSED_FUNCTOR_SYNTAX_HPP
#define COMMON_XANADU_VQL_CLOSED_FUNCTOR_SYNTAX_HPP

#include <functional>
#include <optional>

#include "common/xanadu/vql/ast.hpp"

namespace xanadu::vql {

struct ClosedLiteralPredicateView {
  CompOp op{CompOp::Equal};
  bool literalOnLeft{};
  std::reference_wrapper<const ScalarLiteral> literal;
};

/// Views a single, closed comparison between the current context and a scalar
/// literal. The AST is inspected only at its fixed outer shape.
[[nodiscard]] std::optional<ClosedLiteralPredicateView>
closedLiteralPredicateView(const BooleanExpr &predicate) noexcept;

/// Whether a path step is a literal, read-only dimension traversal, with an
/// optional static range clamp and closed scalar predicates.
[[nodiscard]] bool isClosedTraversalFunctorStep(const PathStep &step) noexcept;

} // namespace xanadu::vql

#endif // COMMON_XANADU_VQL_CLOSED_FUNCTOR_SYNTAX_HPP
