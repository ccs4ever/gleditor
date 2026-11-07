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

std::optional<ClosedLiteralPredicateView>
closedLiteralPredicateView(const BooleanExpr &predicate) noexcept {
  if (predicate.terms.size() != 1 ||
      predicate.terms.front().factors.size() != 1)
    return std::nullopt;

  const auto &factor = predicate.terms.front().factors.front();
  if (factor.negated || !std::holds_alternative<ComparisonExpr>(factor.test))
    return std::nullopt;

  const auto &comparison = std::get<ComparisonExpr>(factor.test);
  if (!isSupportedComparison(comparison.op) || !comparison.left ||
      !comparison.right)
    return std::nullopt;

  const auto literalView = [&](const ValueExpr &context,
                               const ValueExpr &literal,
                               const bool literalOnLeft)
      -> std::optional<ClosedLiteralPredicateView> {
    if (!isCurrentContextPath(context)) return std::nullopt;
    const auto *scalar = std::get_if<ScalarLiteral>(&literal.kind);
    if (!scalar) return std::nullopt;
    return ClosedLiteralPredicateView{.op            = comparison.op,
                                      .literalOnLeft = literalOnLeft,
                                      .literal       = std::cref(*scalar)};
  };

  if (const auto view = literalView(*comparison.right, *comparison.left, true))
    return view;
  return literalView(*comparison.left, *comparison.right, false);
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
