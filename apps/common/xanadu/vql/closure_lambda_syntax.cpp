#include "common/xanadu/vql/closure_lambda_syntax.hpp"

#include <algorithm>
#include <utility>
#include <variant>

namespace xanadu::vql {

std::expected<std::optional<ClosureLambdaSyntax>, FrameEligibilityError>
analyzeClosureLambdaSyntax(const PathLambda &lambda,
                           const FrameEligibilityLimits limits) {
  auto scope = analyzeFrameClosureScope(lambda, limits);
  if (!scope)
    return std::unexpected(FrameEligibilityError{scope.error().message});
  if (scope->refusalReason || lambda.params.size() > 1)
    return std::optional<ClosureLambdaSyntax>{};

  ClosureLambdaSyntax result;
  result.hasFormal = !lambda.params.empty();
  if (result.hasFormal) {
    const auto &formal = lambda.params.front();
    if (formal.defaultValue || formal.isVariadic)
      return std::optional<ClosureLambdaSyntax>{};
    if (const auto *name = std::get_if<std::string>(&formal.target)) {
      if (name->empty()) return std::optional<ClosureLambdaSyntax>{};
      result.parameterName = *name;
    }
  }

  result.freeCaptureNames.assign(scope->freeCaptureNames.begin(),
                                 scope->freeCaptureNames.end());
  std::ranges::sort(result.freeCaptureNames);
  result.requiredDefaultCaptureBindings.assign(
      scope->requiredDefaultCaptureBindings.begin(),
      scope->requiredDefaultCaptureBindings.end());
  std::ranges::sort(result.requiredDefaultCaptureBindings);
  result.conditionalRequiredDefaultCaptureBindings.assign(
      scope->conditionalRequiredDefaultCaptureBindings.begin(),
      scope->conditionalRequiredDefaultCaptureBindings.end());
  std::ranges::sort(result.conditionalRequiredDefaultCaptureBindings);
  result.omittedRestRequiredDefaultCaptureBindings.assign(
      scope->omittedRestRequiredDefaultCaptureBindings.begin(),
      scope->omittedRestRequiredDefaultCaptureBindings.end());
  std::ranges::sort(result.omittedRestRequiredDefaultCaptureBindings);
  result.callSites = std::move(scope->callSites);
  return std::optional<ClosureLambdaSyntax>{std::move(result)};
}

} // namespace xanadu::vql
