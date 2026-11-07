#ifndef COMMON_XANADU_VQL_CLOSED_FUNCTOR_SYNTAX_HPP
#define COMMON_XANADU_VQL_CLOSED_FUNCTOR_SYNTAX_HPP

#include <functional>
#include <optional>
#include <span>

#include "common/xanadu/vql/ast.hpp"

namespace xanadu::vql {

struct ClosedLiteralComparisonView {
  CompOp op{CompOp::Equal};
  bool literalOnLeft{};
  bool negated{};
  std::reference_wrapper<const ScalarLiteral> literal;
};

/// Views one closed comparison factor between the current context and a
/// scalar literal. The AST is inspected only at its fixed outer shape.
[[nodiscard]] std::optional<ClosedLiteralComparisonView>
closedLiteralComparisonView(const BooleanFactor &factor) noexcept;

struct ClosedLiteralPredicateView {
  std::span<const BooleanTerm> terms;
};

/// Views a flat Boolean expression whose nonempty terms and factors are all
/// closed scalar comparisons. The view borrows the existing term vectors.
[[nodiscard]] std::optional<ClosedLiteralPredicateView>
closedLiteralPredicateView(const BooleanExpr &predicate) noexcept;

/// Whether a path step is a literal, read-only dimension traversal, with an
/// optional static range clamp and closed scalar predicates.
[[nodiscard]] bool isClosedTraversalFunctorStep(const PathStep &step) noexcept;

} // namespace xanadu::vql

#endif // COMMON_XANADU_VQL_CLOSED_FUNCTOR_SYNTAX_HPP
