#include "common/xanadu/vql/closed_functor_syntax.hpp"
#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/vql/functor_capture.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

#include "common/xanadu/scalar.hpp"

namespace xanadu::vql {
namespace {
constexpr std::size_t closedFunctorPredicateHeaderWork     = 1;
constexpr std::size_t closedFunctorPredicateTermHeaderWork = 1;
constexpr std::size_t closedFunctorPredicateFactorHeaderWork =
    5; // Factor, operator, operand order, negation and typed-literal tag.

template <typename Variant>
std::size_t predicateTemplateWork(const Variant &literal) noexcept {
  const std::size_t payload = std::visit(
      [](const auto &value) -> std::size_t {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, std::string>)
          return value.size();
        else
          return sizeof(T);
      },
      literal);
  if (payload > std::numeric_limits<std::size_t>::max() -
                    closedFunctorPredicateFactorHeaderWork)
    return std::numeric_limits<std::size_t>::max();
  return closedFunctorPredicateFactorHeaderWork + payload;
}

void addWork(std::size_t &total, const std::size_t amount) noexcept {
  if (amount > std::numeric_limits<std::size_t>::max() - total)
    total = std::numeric_limits<std::size_t>::max();
  else
    total += amount;
}
} // namespace

bool VQLCompiler::isFunctorArgument(const ValueExpr &argument) const {
  if (const auto *name = std::get_if<std::string>(&argument.kind))
    return functorBindings_.contains(*name);
  const auto *path =
      std::get_if<std::shared_ptr<PathExpression>>(&argument.kind);
  if (!path || !*path) return false;
  const auto &expression = **path;
  const bool boundAnchor = expression.anchor.kind == AnchorKind::Variable &&
                           functorBindings_.contains(expression.anchor.name);
  if (!expression.isFunctor && !boundAnchor) return false;
  if (expression.anchorProjection || expression.shapeProjection)
    throw std::invalid_argument(
        "ordered shape projections are not supported in functors");
  validateFunctorCaptureSteps(expression.steps);
  if (expression.anchorPredicates.size() != 0 || expression.cloneTail ||
      std::ranges::any_of(expression.steps, [](const PathStep &step) {
        return step.projectionAfter.has_value();
      }))
    return false;
  if (boundAnchor &&
      (expression.anchor.versionPin || expression.anchor.derefMaster))
    throw std::invalid_argument(
        "qualified functor anchors require runtime functor handles");
  return true;
}

std::optional<std::span<const PathStep>>
VQLCompiler::closedFunctorArgumentSteps(const ValueExpr &argument) const {
  if (const auto *name = std::get_if<std::string>(&argument.kind)) {
    const auto found = functorBindings_.find(*name);
    if (found != functorBindings_.end())
      return std::span<const PathStep>{found->second};
    return std::nullopt;
  }
  const auto *path =
      std::get_if<std::shared_ptr<PathExpression>>(&argument.kind);
  if (!path || !*path) return std::nullopt;
  const auto &expression = **path;
  if (!expression.anchorPredicates.empty() || expression.anchorProjection ||
      expression.shapeProjection || expression.cloneTail)
    return std::nullopt;
  if (expression.anchor.derefMaster || expression.anchor.createValue ||
      expression.anchor.generator || expression.anchor.storeAnchor ||
      expression.anchor.versionPin || expression.anchor.cellId != 0 ||
      !expression.anchor.microversionId.empty())
    return std::nullopt;
  if (!expression.isFunctor) {
    if (expression.anchor.kind != AnchorKind::Variable ||
        !expression.steps.empty())
      return std::nullopt;
    const auto found = functorBindings_.find(expression.anchor.name);
    if (found == functorBindings_.end()) return std::nullopt;
    return std::span<const PathStep>{found->second};
  }
  if (expression.anchor.kind != AnchorKind::Context ||
      !expression.anchor.name.empty())
    return std::nullopt;
  if (expression.steps.size() == 1) {
    const auto &step      = expression.steps.front();
    const auto *dimension = std::get_if<SignedDimensionStep>(&step.selector);
    if (dimension && dimension->dimName.starts_with('$') &&
        dimension->direction == zigzag::DimVector::POS && !dimension->isBidir &&
        dimension->placement == Placement::Default &&
        dimension->creates.empty() && !step.derefMaster &&
        step.predicates.empty() && step.shapePredicates.empty() &&
        !step.projectionAfter && !step.rangeClamp && !step.versionPin &&
        step.yieldMode == YieldMode::Default && !step.capturedFunctor) {
      const auto found = functorBindings_.find(dimension->dimName.substr(1));
      if (found != functorBindings_.end()) {
        validateFunctorCaptureSteps(found->second);
        return std::span<const PathStep>{found->second};
      }
    }
  }
  return std::span<const PathStep>{expression.steps};
}

void VQLCompiler::chargeClosedFunctorTemplateWork(const std::size_t amount) {
  if (amount > remainingClosedFunctorTemplateWork_)
    throw std::invalid_argument(
        "compiled VQL closed functor template work budget exhausted");
  remainingClosedFunctorTemplateWork_ -= amount;
}

std::size_t VQLCompiler::closedFunctorClampWork(const bool hasEnd) noexcept {
  constexpr std::size_t presenceWorkUnit = 1;
  return presenceWorkUnit + sizeof(std::int64_t) + presenceWorkUnit +
         (hasEnd ? sizeof(std::int64_t) : 0);
}

std::size_t
VQLCompiler::closedFunctorPredicateWork(const BooleanExpr &predicate) noexcept {
  std::size_t work = closedFunctorPredicateHeaderWork;
  const auto view  = closedLiteralPredicateView(predicate);
  if (!view) return std::numeric_limits<std::size_t>::max();
  for (const auto &term : view->terms) {
    addWork(work, closedFunctorPredicateTermHeaderWork);
    for (const auto &factor : term.factors) {
      const auto comparison = closedLiteralComparisonView(factor);
      if (!comparison) return std::numeric_limits<std::size_t>::max();
      addWork(work, predicateTemplateWork(comparison->literal.get().value));
    }
  }
  return work;
}

std::size_t VQLCompiler::closedFunctorPredicateWork(
    const ClosedFunctorPredicateKey &predicate) noexcept {
  std::size_t work = closedFunctorPredicateHeaderWork;
  for (const auto &term : predicate.terms) {
    addWork(work, closedFunctorPredicateTermHeaderWork);
    for (const auto &factor : term.factors)
      addWork(work, predicateTemplateWork(factor.literal));
  }
  return work;
}

std::size_t VQLCompiler::closedFunctorTemplateWork(
    const std::span<const ClosedFunctorFormalKey> templates) const {
  std::size_t work = templates.size();
  const auto add   = [&work](const std::size_t amount) {
    if (amount > std::numeric_limits<std::size_t>::max() - work)
      work = std::numeric_limits<std::size_t>::max();
    else
      work += amount;
  };
  for (const auto &formal : templates) {
    add(formal.steps.size());
    for (const auto &step : formal.steps) {
      add(step.dimName.size());
      if (step.rangeClamp)
        add(closedFunctorClampWork(step.rangeClamp->end.has_value()));
      for (const auto &predicate : step.predicates)
        add(closedFunctorPredicateWork(predicate));
    }
  }
  return work;
}

std::optional<std::vector<PathStep>>
VQLCompiler::functorArgument(const ValueExpr &argument) const {
  if (const auto *path =
          std::get_if<std::shared_ptr<PathExpression>>(&argument.kind);
      path && *path)
    return functorArgument(**path);
  if (const auto *name = std::get_if<std::string>(&argument.kind)) {
    const auto found = functorBindings_.find(*name);
    if (found != functorBindings_.end()) return found->second;
  }
  return std::nullopt;
}

std::optional<std::vector<PathStep>>
VQLCompiler::functorArgument(const PathExpression &path) const {
  const bool existingFunctorAnchor =
      path.anchor.kind == AnchorKind::Variable &&
      functorBindings_.contains(path.anchor.name);
  const bool functorExpression = path.isFunctor || existingFunctorAnchor;
  if (functorExpression) {
    if (path.anchorProjection || path.shapeProjection)
      throw std::invalid_argument(
          "ordered shape projections are not supported in functors");
    validateFunctorCaptureSteps(path.steps);
  }
  if (path.anchorProjection || path.shapeProjection ||
      std::ranges::any_of(path.steps, [](const PathStep &step) {
        return step.projectionAfter.has_value();
      }))
    return std::nullopt;
  if (!path.anchorPredicates.empty() || path.cloneTail) return std::nullopt;
  std::vector<PathStep> steps;
  if (!path.isFunctor) {
    if (path.anchor.kind != AnchorKind::Variable) return std::nullopt;
    const auto found = functorBindings_.find(path.anchor.name);
    if (found == functorBindings_.end()) return std::nullopt;
    if (path.anchor.versionPin || path.anchor.derefMaster)
      throw std::invalid_argument(
          "qualified functor anchors require runtime functor handles");
    steps = found->second;
  }
  auto captured = captureFunctorApplications(
      path.steps,
      [&](std::string_view name) -> std::optional<std::vector<PathStep>> {
        const auto found = functorBindings_.find(std::string(name));
        if (found == functorBindings_.end())
          throw std::invalid_argument("unbound point-free functor '$" +
                                      std::string(name) + "'");
        return found->second;
      });
  steps.insert(steps.end(), captured.begin(), captured.end());
  return steps;
}

} // namespace xanadu::vql
