#include "common/xanadu/vql/functor_capture.hpp"

#include <cstddef>
#include <stdexcept>
#include <unordered_set>

namespace xanadu::vql {
namespace {

struct CaptureScanBudget {
  static constexpr std::size_t maxNodes = 100'000;
  static constexpr std::size_t maxDepth = 256;

  std::size_t nodes{0};
  std::unordered_set<const void *> active;
};

void refuseProjectionCapture(const std::span<const PathStep> steps,
                             CaptureScanBudget &budget,
                             const std::size_t depth = 0) {
  if (depth > CaptureScanBudget::maxDepth)
    throw std::invalid_argument("functor capture AST depth budget exhausted");
  for (const PathStep &step : steps) {
    if (budget.nodes >= CaptureScanBudget::maxNodes)
      throw std::invalid_argument("functor capture AST node budget exhausted");
    ++budget.nodes;
    if (step.projectionAfter)
      throw std::invalid_argument(
          "ordered shape projections are not supported in functor captures");
    const auto scanNested = [&](const void *identity,
                                const std::span<const PathStep> nested) {
      if (!budget.active.insert(identity).second)
        throw std::invalid_argument("functor capture found a cyclic AST");
      struct EraseOnExit {
        std::unordered_set<const void *> &active;
        const void *identity;
        ~EraseOnExit() { active.erase(identity); }
      } erase{budget.active, identity};
      refuseProjectionCapture(nested, budget, depth + 1);
    };
    if (const auto *group =
            std::get_if<std::shared_ptr<MacroDimensionGroup>>(&step.selector);
        group && *group)
      scanNested(group->get(), (*group)->steps);
    if (step.capturedFunctor)
      scanNested(step.capturedFunctor.get(), *step.capturedFunctor);
  }
}

std::vector<PathStep> captureApplications(const std::span<const PathStep> steps,
                                          FunctorLookup lookup,
                                          CaptureScanBudget &budget) {
  std::vector<PathStep> captured;
  for (const auto &step : steps) {
    if (const auto *nested =
            std::get_if<std::shared_ptr<MacroDimensionGroup>>(&step.selector);
        nested && *nested) {
      auto group         = std::make_shared<MacroDimensionGroup>(**nested);
      group->steps       = captureApplications(group->steps, lookup, budget);
      auto capturedGroup = step;
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
    refuseProjectionCapture(*inner, budget);
    const bool bare = dimension->creates.empty() &&
                      dimension->placement == Placement::Default &&
                      !step.derefMaster && step.predicates.empty() &&
                      step.shapePredicates.empty() && !step.rangeClamp &&
                      !step.versionPin && !step.projectionAfter &&
                      step.yieldMode == YieldMode::Default;
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

} // namespace

void validateFunctorCaptureSteps(const std::span<const PathStep> steps) {
  CaptureScanBudget budget;
  refuseProjectionCapture(steps, budget);
}

std::vector<PathStep>
captureFunctorApplications(std::span<const PathStep> steps,
                           FunctorLookup lookup) {
  CaptureScanBudget budget;
  refuseProjectionCapture(steps, budget);
  return captureApplications(steps, lookup, budget);
}
} // namespace xanadu::vql
