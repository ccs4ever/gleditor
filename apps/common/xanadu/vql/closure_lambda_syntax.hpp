/**
 * @file closure_lambda_syntax.hpp
 * @brief Bounded syntax recognition for the first VQL closure lambda slice.
 */
#ifndef COMMON_XANADU_VQL_CLOSURE_LAMBDA_SYNTAX_HPP
#define COMMON_XANADU_VQL_CLOSURE_LAMBDA_SYNTAX_HPP

#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <vector>

#include "common/xanadu/vql/frame_eligibility.hpp"

namespace xanadu::vql {

struct ClosureLambdaSyntax {
  std::size_t formalCount{0};
  /// Formal indices whose values are supplied lazily by native frame code.
  std::vector<std::size_t> defaultParameterIndices;
  /// Legacy first-formal summary retained for existing compiler clients.
  bool hasFormal{false};
  /// Nonempty only when the first formal is a named alias.
  std::string parameterName;
  /// Lexical aliases read by the body, in stable sorted order.
  std::vector<std::string> freeCaptureNames;
  std::vector<std::string> requiredDefaultCaptureBindings;
  std::vector<std::string> conditionalRequiredDefaultCaptureBindings;
  std::vector<std::string> omittedRestRequiredDefaultCaptureBindings;
  std::vector<ClosureCallSite> callSites;
};

/// Recognize a bounded fixed-formal closure value. Variadics remain
/// unsupported. Structural contracts and body traversal use the shared frame
/// scope walker. Unsupported syntax returns an empty optional; malformed ASTs
/// and exhausted limits are hard errors.
[[nodiscard]] std::expected<std::optional<ClosureLambdaSyntax>,
                            FrameEligibilityError>
analyzeClosureLambdaSyntax(const PathLambda &lambda,
                           FrameEligibilityLimits limits = {});

} // namespace xanadu::vql

#endif // COMMON_XANADU_VQL_CLOSURE_LAMBDA_SYNTAX_HPP
