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
  std::string parameterName;
  /// Lexical aliases read by the body, in stable sorted order.
  std::vector<std::string> freeCaptureNames;
};

/// Recognize a one-parameter, untyped, read-only closure value body.
/// Unsupported syntax returns an empty optional; malformed ASTs and exhausted
/// limits are hard errors. The returned capture names are syntactic
/// dependencies only.
[[nodiscard]] std::expected<std::optional<ClosureLambdaSyntax>,
                            FrameEligibilityError>
analyzeClosureLambdaSyntax(const PathLambda &lambda,
                           FrameEligibilityLimits limits = {});

} // namespace xanadu::vql

#endif // COMMON_XANADU_VQL_CLOSURE_LAMBDA_SYNTAX_HPP
