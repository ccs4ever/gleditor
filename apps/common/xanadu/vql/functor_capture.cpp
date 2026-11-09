#include "common/xanadu/vql/functor_capture.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
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

std::size_t addWork(const std::size_t left, const std::size_t right) {
  return right > std::numeric_limits<std::size_t>::max() - left
             ? std::numeric_limits<std::size_t>::max()
             : left + right;
}

std::size_t multiplyWork(const std::size_t left, const std::size_t right) {
  return right != 0 && left > std::numeric_limits<std::size_t>::max() / right
             ? std::numeric_limits<std::size_t>::max()
             : left * right;
}

void chargeShapeCopyWork(const ShapeLiteral &shape, FunctorCopyCharge charge) {
  charge(sizeof(ShapeLiteral));
  charge(multiplyWork(shape.rules.size(), sizeof(ShapeRule)));
  for (const auto &rule : shape.rules) {
    if (const auto *name = std::get_if<std::string>(&rule.rule))
      charge(name->size());
    else if (const auto *dimension =
                 std::get_if<DimensionShapeRule>(&rule.rule))
      charge(dimension->dimName.size());
    else if (const auto *pivot = std::get_if<DimensionPivotRule>(&rule.rule)) {
      charge(pivot->fromDim.size());
      charge(pivot->toDim.size());
    } else if (const auto *virtualDimension =
                   std::get_if<VirtualDimensionDefRule>(&rule.rule))
      charge(virtualDimension->dimName.size());
  }
}

void chargeValueCopyWork(const ValueExpr &value, FunctorCopyCharge charge) {
  charge(sizeof(ValueExpr));
  if (const auto *name = std::get_if<std::string>(&value.kind))
    charge(name->size());
  else if (const auto *literal = std::get_if<ScalarLiteral>(&value.kind)) {
    if (const auto *text = std::get_if<std::string>(&literal->value))
      charge(text->size());
  }
}

void chargeStepCopyWork(const PathStep &step, FunctorCopyCharge charge) {
  charge(sizeof(PathStep));
  const auto chargeString = [&charge](const std::string &value) {
    charge(value.size());
  };
  if (const auto *dimension =
          std::get_if<SignedDimensionStep>(&step.selector)) {
    chargeString(dimension->dimName);
    charge(multiplyWork(dimension->creates.size(), sizeof(CreateValue)));
    for (const auto &create : dimension->creates) chargeString(create.literal);
  } else if (const auto *group =
                 std::get_if<std::shared_ptr<MacroDimensionGroup>>(
                     &step.selector);
             group && *group) {
    charge(sizeof(MacroDimensionGroup));
    charge(multiplyWork((*group)->steps.size(), sizeof(PathStep)));
  } else if (const auto *invocation =
                 std::get_if<FunctionInvocation>(&step.selector)) {
    chargeString(invocation->name);
    charge(multiplyWork(invocation->args.size(), sizeof(ValueExpr)));
    charge(multiplyWork(invocation->argNames.size(), sizeof(std::string)));
    for (const auto &name : invocation->argNames) chargeString(name);
    for (const auto &argument : invocation->args)
      chargeValueCopyWork(argument, charge);
  } else if (const auto *slice =
                 std::get_if<SliceSelectorStep>(&step.selector)) {
    if (slice->nameFilter) chargeString(*slice->nameFilter);
  } else if (const auto *doc = std::get_if<DocSelectorStep>(&step.selector)) {
    if (doc->nameFilter) chargeString(*doc->nameFilter);
  } else if (const auto *morphism = std::get_if<PathMorphism>(&step.selector)) {
    chargeString(morphism->perspective);
    chargeValueCopyWork(morphism->subject, charge);
    charge(multiplyWork(morphism->targets.size(), sizeof(ValueExpr)));
    for (const auto &target : morphism->targets)
      chargeValueCopyWork(target, charge);
  } else if (const auto *pivot =
                 std::get_if<DimensionPivotStep>(&step.selector)) {
    chargeString(pivot->fromDim);
    chargeString(pivot->toDim);
  } else if (const auto *virtualDimension =
                 std::get_if<VirtualDimensionStep>(&step.selector)) {
    chargeString(virtualDimension->dimStep.dimName);
    charge(multiplyWork(virtualDimension->dimStep.creates.size(),
                        sizeof(CreateValue)));
    for (const auto &create : virtualDimension->dimStep.creates)
      chargeString(create.literal);
    chargeValueCopyWork(virtualDimension->expr, charge);
  } else if (const auto *root = std::get_if<RootSelectorStep>(&step.selector))
    chargeString(root->name);
  else if (const auto *perspective =
               std::get_if<PerspectiveStep>(&step.selector))
    chargeString(perspective->perspective);
  charge(multiplyWork(step.predicates.size(), sizeof(BooleanExpr)));
  for (const auto &predicate : step.predicates) {
    charge(multiplyWork(predicate.terms.size(), sizeof(BooleanTerm)));
    for (const auto &term : predicate.terms)
      charge(multiplyWork(term.factors.size(), sizeof(BooleanFactor)));
  }
  charge(multiplyWork(step.shapePredicates.size(), sizeof(ShapeLiteral)));
  for (const auto &shape : step.shapePredicates)
    chargeShapeCopyWork(shape, charge);
  if (step.projectionAfter) chargeShapeCopyWork(*step.projectionAfter, charge);
  if (step.versionPin) {
    charge(multiplyWork(step.versionPin->versions.size(), sizeof(VersionExpr)));
    for (const auto &version : step.versionPin->versions) {
      charge(version.versionStr.size());
      charge(version.branchName.size());
    }
  }
}

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

void validateAndChargeCaptureSteps(std::span<const PathStep> steps,
                                   FunctorCopyCharge charge,
                                   CaptureScanBudget &budget,
                                   const std::size_t depth = 0) {
  if (depth > CaptureScanBudget::maxDepth)
    throw std::invalid_argument("functor capture AST depth budget exhausted");
  for (const PathStep &step : steps) {
    if (budget.nodes >= CaptureScanBudget::maxNodes)
      throw std::invalid_argument("functor capture AST node budget exhausted");
    ++budget.nodes;
    chargeStepCopyWork(step, charge);
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
      validateAndChargeCaptureSteps(nested, charge, budget, depth + 1);
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

std::vector<PathStep>
captureApplicationsBounded(const std::span<const PathStep> steps,
                           FunctorSpanLookup lookup, FunctorCopyCharge charge,
                           CaptureScanBudget &budget) {
  std::vector<PathStep> captured;
  for (const auto &step : steps) {
    chargeStepCopyWork(step, charge);
    if (const auto *nested =
            std::get_if<std::shared_ptr<MacroDimensionGroup>>(&step.selector);
        nested && *nested) {
      chargeFunctorCaptureCopyWork((*nested)->steps, charge);
      auto group = std::make_shared<MacroDimensionGroup>(**nested);
      group->steps =
          captureApplicationsBounded(group->steps, lookup, charge, budget);
      chargeStepCopyWork(step, charge);
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
    charge(std::max(dimension->dimName.size() - 1, std::size_t{1}));
    const auto inner = lookup(std::string_view(dimension->dimName).substr(1));
    if (!inner) {
      captured.push_back(step);
      continue;
    }
    validateAndChargeCaptureSteps(*inner, charge, budget);
    chargeFunctorCaptureCopyWork(*inner, charge);
    const bool bare = dimension->creates.empty() &&
                      dimension->placement == Placement::Default &&
                      !step.derefMaster && step.predicates.empty() &&
                      step.shapePredicates.empty() && !step.rangeClamp &&
                      !step.versionPin && !step.projectionAfter &&
                      step.yieldMode == YieldMode::Default;
    if (bare) {
      captured.insert(captured.end(), inner->begin(), inner->end());
    } else {
      auto application = step;
      application.capturedFunctor =
          std::make_shared<const std::vector<PathStep>>(inner->begin(),
                                                        inner->end());
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

namespace {

void chargeStepsCopyWork(std::span<const PathStep> steps,
                         FunctorCopyCharge charge, CaptureScanBudget &budget,
                         const std::size_t depth) {
  if (depth > CaptureScanBudget::maxDepth)
    throw std::invalid_argument("functor capture AST depth budget exhausted");
  charge(multiplyWork(steps.size(), sizeof(PathStep)));
  for (const auto &step : steps) {
    if (budget.nodes >= CaptureScanBudget::maxNodes)
      throw std::invalid_argument("functor capture AST node budget exhausted");
    ++budget.nodes;
    chargeStepCopyWork(step, charge);
    if (const auto *group =
            std::get_if<std::shared_ptr<MacroDimensionGroup>>(&step.selector);
        group && *group) {
      if (!budget.active.insert(group->get()).second)
        throw std::invalid_argument("functor capture found a cyclic AST");
      struct EraseOnExit {
        std::unordered_set<const void *> &active;
        const void *identity;
        ~EraseOnExit() { active.erase(identity); }
      } erase{budget.active, group->get()};
      chargeStepsCopyWork((*group)->steps, charge, budget, depth + 1);
    }
  }
}

} // namespace

void chargeFunctorCaptureCopyWork(std::span<const PathStep> steps,
                                  FunctorCopyCharge charge) {
  CaptureScanBudget budget;
  chargeStepsCopyWork(steps, charge, budget, 0);
}

std::size_t functorCaptureCopyWork(std::span<const PathStep> steps) {
  std::size_t work = 0;
  chargeFunctorCaptureCopyWork(
      steps, [&](const std::size_t amount) { work = addWork(work, amount); });
  return work;
}

std::vector<PathStep>
captureFunctorApplications(std::span<const PathStep> steps,
                           FunctorLookup lookup) {
  CaptureScanBudget budget;
  refuseProjectionCapture(steps, budget);
  return captureApplications(steps, lookup, budget);
}

std::vector<PathStep>
captureFunctorApplications(std::span<const PathStep> steps,
                           FunctorSpanLookup lookup, FunctorCopyCharge charge) {
  CaptureScanBudget budget;
  validateAndChargeCaptureSteps(steps, charge, budget);
  return captureApplicationsBounded(steps, lookup, charge, budget);
}
} // namespace xanadu::vql
