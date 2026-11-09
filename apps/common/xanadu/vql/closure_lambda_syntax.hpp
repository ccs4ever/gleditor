/**
 * @file closure_lambda_syntax.hpp
 * @brief Bounded syntax recognition for the first VQL closure lambda slice.
 */
#ifndef COMMON_XANADU_VQL_CLOSURE_LAMBDA_SYNTAX_HPP
#define COMMON_XANADU_VQL_CLOSURE_LAMBDA_SYNTAX_HPP

#include <expected>
#include <optional>
#include <string>
#include <vector>

#include "common/xanadu/vql/frame_eligibility.hpp"

namespace xanadu::vql {

struct ClosureLambdaSyntax {
  bool hasFormal{false};
  /// Nonempty only when the one formal is a named alias.
  std::string parameterName;
  /// Lexical aliases read by the body, in stable sorted order.
  std::vector<std::string> freeCaptureNames;
  std::vector<std::string> requiredDefaultCaptureBindings;
  std::vector<std::string> conditionalRequiredDefaultCaptureBindings;
  std::vector<ClosureCallSite> callSites;
};

/// Recognize a bounded zero- or one-formal closure value. Formal-level
/// defaults and variadics are not supported. Structural contracts and body
/// traversal use the shared frame scope walker. Unsupported syntax returns an
/// empty optional; malformed ASTs and exhausted limits are hard errors.
[[nodiscard]] std::expected<std::optional<ClosureLambdaSyntax>,
                            FrameEligibilityError>
analyzeClosureLambdaSyntax(const PathLambda &lambda,
                           FrameEligibilityLimits limits = {});

} // namespace xanadu::vql

#endif // COMMON_XANADU_VQL_CLOSURE_LAMBDA_SYNTAX_HPP
