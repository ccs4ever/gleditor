#include "common/xanadu/vql/closed_functor_syntax.hpp"

namespace xanadu::vql {

bool isPlainClosedFunctorStep(const PathStep &step) noexcept {
  const auto *dimension = std::get_if<SignedDimensionStep>(&step.selector);
  if (!dimension || dimension->dimName.empty() ||
      dimension->dimName.starts_with('$') || dimension->isBidir ||
      dimension->placement != Placement::Default || !dimension->creates.empty())
    return false;
  if (dimension->direction != zigzag::DimVector::POS &&
      dimension->direction != zigzag::DimVector::NEG)
    return false;
  return !step.derefMaster && step.predicates.empty() &&
         step.shapePredicates.empty() && !step.projectionAfter &&
         !step.rangeClamp && !step.versionPin &&
         step.yieldMode == YieldMode::Default && !step.capturedFunctor;
}

} // namespace xanadu::vql
