#include "common/xanadu/vql/closed_functor_syntax.hpp"

namespace xanadu::vql {
namespace {

bool isCurrentContextPath(const ValueExpr &value) noexcept {
  const auto *path = std::get_if<std::shared_ptr<PathExpression>>(&value.kind);
  if (!path || !*path) return false;

  const auto &expression = **path;
  const auto &anchor     = expression.anchor;
  return !expression.isFunctor && anchor.kind == AnchorKind::Context &&
         anchor.name.empty() && anchor.cellId == 0 && !anchor.derefMaster &&
         !anchor.createValue && !anchor.generator && !anchor.storeAnchor &&
         !anchor.versionPin && anchor.microversionId.empty() &&
         expression.anchorPredicates.empty() && !expression.anchorProjection &&
         expression.steps.empty() && !expression.shapeProjection &&
         !expression.cloneTail;
}

bool isSupportedComparison(const CompOp op) noexcept {
  switch (op) {
  case CompOp::Equal:
  case CompOp::NotEqual:
  case CompOp::LessThan:
  case CompOp::GreaterThan:
  case CompOp::LessEqual:
  case CompOp::GreaterEqual:
    return true;
  }
  return false;
}

} // namespace

std::optional<ClosedLiteralComparisonView>
closedLiteralComparisonView(const BooleanFactor &factor) noexcept {
  if (!std::holds_alternative<ComparisonExpr>(factor.test)) return std::nullopt;

  const auto &comparison = std::get<ComparisonExpr>(factor.test);
  if (!isSupportedComparison(comparison.op) || !comparison.left ||
      !comparison.right)
    return std::nullopt;

  const auto literalView = [&](const ValueExpr &context,
                               const ValueExpr &literal,
                               const bool literalOnLeft)
      -> std::optional<ClosedLiteralComparisonView> {
    if (!isCurrentContextPath(context)) return std::nullopt;
    const auto *scalar = std::get_if<ScalarLiteral>(&literal.kind);
    if (!scalar) return std::nullopt;
    return ClosedLiteralComparisonView{.op            = comparison.op,
                                       .literalOnLeft = literalOnLeft,
                                       .negated       = factor.negated,
                                       .literal       = std::cref(*scalar)};
  };

  if (const auto view = literalView(*comparison.right, *comparison.left, true))
    return view;
  return literalView(*comparison.left, *comparison.right, false);
}

std::optional<ClosedLiteralPredicateView>
closedLiteralPredicateView(const BooleanExpr &predicate) noexcept {
  if (predicate.terms.empty()) return std::nullopt;
  for (const auto &term : predicate.terms) {
    if (term.factors.empty()) return std::nullopt;
    for (const auto &factor : term.factors) {
      if (!closedLiteralComparisonView(factor)) return std::nullopt;
    }
  }
  return ClosedLiteralPredicateView{
      .terms = std::span<const BooleanTerm>{predicate.terms}};
}

bool isClosedTraversalFunctorStep(const PathStep &step) noexcept {
  const auto *dimension = std::get_if<SignedDimensionStep>(&step.selector);
  if (!dimension || dimension->dimName.empty() ||
      dimension->dimName.starts_with('$') || dimension->isBidir ||
      dimension->placement != Placement::Default || !dimension->creates.empty())
    return false;
  if (dimension->direction != zigzag::DimVector::POS &&
      dimension->direction != zigzag::DimVector::NEG)
    return false;
  if (step.derefMaster || !step.shapePredicates.empty() ||
      step.projectionAfter || step.versionPin ||
      step.yieldMode != YieldMode::Default || step.capturedFunctor)
    return false;
  for (const auto &predicate : step.predicates) {
    if (!closedLiteralPredicateView(predicate)) return false;
  }
  return true;
}

} // namespace xanadu::vql
