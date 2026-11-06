#include "common/xanadu/vql/projection_syntax.hpp"

#include <variant>

namespace xanadu::vql {
namespace {

bool isCanonicalDimensionName(const std::string &name) noexcept {
  return name.starts_with("d.") && name.size() > 2;
}

bool isValidDirection(const zigzag::DimVector direction) noexcept {
  return direction == zigzag::DimVector::POS ||
         direction == zigzag::DimVector::NEG;
}

} // namespace

bool isSimpleProjectionPath(const PathExpression &path) noexcept {
  if (path.isFunctor || !path.anchorPredicates.empty() ||
      path.shapeProjection || path.cloneTail || path.anchor.versionPin ||
      path.anchor.createValue || path.anchor.generator ||
      path.anchor.storeAnchor || !path.anchor.microversionId.empty())
    return false;

  switch (path.anchor.kind) {
  case AnchorKind::Context:
  case AnchorKind::Home:
  case AnchorKind::Root:
  case AnchorKind::LiteralCellId:
  case AnchorKind::Variable:
    break;
  default:
    return false;
  }

  for (const PathStep &step : path.steps) {
    const auto *dimension = std::get_if<SignedDimensionStep>(&step.selector);
    if (!dimension || !isCanonicalDimensionName(dimension->dimName) ||
        !isValidDirection(dimension->direction) ||
        !dimension->creates.empty() || !step.predicates.empty() ||
        !step.shapePredicates.empty() || step.versionPin ||
        step.yieldMode != YieldMode::Default || step.capturedFunctor)
      return false;
  }
  return true;
}

bool isStaticShapeProjectionSyntax(const ShapeLiteral &shape) noexcept {
  for (const ShapeRule &entry : shape.rules) {
    if (const auto *wildcard = std::get_if<ShapeWildcard>(&entry.rule)) {
      if (*wildcard != ShapeWildcard::Everything &&
          *wildcard != ShapeWildcard::Nothing)
        return false;
      continue;
    }
    const auto *rule = std::get_if<DimensionShapeRule>(&entry.rule);
    if (!rule || !isCanonicalDimensionName(rule->dimName) ||
        !isValidDirection(rule->direction))
      return false;
    if (!rule->targetValue) continue;
    if (rule->negated || rule->isBidir) return false;
    const ValueExpr &target = *rule->targetValue;
    if (std::holds_alternative<ScalarLiteral>(target.kind) ||
        std::holds_alternative<std::string>(target.kind))
      continue;
    const auto *path =
        std::get_if<std::shared_ptr<PathExpression>>(&target.kind);
    if (!path || !*path || !isSimpleProjectionPath(**path)) return false;
  }
  return true;
}

} // namespace xanadu::vql
