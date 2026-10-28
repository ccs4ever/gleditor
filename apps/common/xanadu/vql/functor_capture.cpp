#include "common/xanadu/vql/functor_capture.hpp"

namespace xanadu::vql {

std::vector<PathStep>
captureFunctorApplications(std::span<const PathStep> steps,
                           FunctorLookup lookup) {
  std::vector<PathStep> captured;
  for (const auto &step : steps) {
    if (const auto *nested =
            std::get_if<std::shared_ptr<MacroDimensionGroup>>(&step.selector);
        nested && *nested) {
      auto group             = std::make_shared<MacroDimensionGroup>(**nested);
      group->steps           = captureFunctorApplications(group->steps, lookup);
      auto capturedGroup     = step;
      capturedGroup.selector = std::move(group);
      captured.push_back(std::move(capturedGroup));
      continue;
    }
    if (step.capturedFunctor) {
      captured.push_back(step);
      continue;
    }
    const auto *dimension = std::get_if<SignedDimensionStep>(&step.selector);
    if (!dimension || !dimension->dimName.starts_with('$')) {
      captured.push_back(step);
      continue;
    }
    auto inner = lookup(std::string_view(dimension->dimName).substr(1));
    if (!inner) {
      captured.push_back(step);
      continue;
    }
    const bool bare = dimension->creates.empty() &&
                      dimension->placement == Placement::Default &&
                      !step.derefMaster && step.predicates.empty() &&
                      step.shapePredicates.empty() && !step.rangeClamp &&
                      !step.versionPin && step.yieldMode == YieldMode::Default;
    if (bare) {
      captured.insert(captured.end(), inner->begin(), inner->end());
    } else {
      // Keep the dollar selector's empty-input semantics while freezing the
      // body independently of later rebinding of its source name.
      auto application = step;
      application.capturedFunctor =
          std::make_shared<const std::vector<PathStep>>(std::move(*inner));
      captured.push_back(std::move(application));
    }
  }
  return captured;
}

} // namespace xanadu::vql
