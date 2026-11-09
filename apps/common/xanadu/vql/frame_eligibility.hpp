/**
 * @file frame_eligibility.hpp
 * @brief Bounded, conservative eligibility analysis for reusable VQL frames.
 */
#ifndef COMMON_XANADU_VQL_FRAME_ELIGIBILITY_HPP
#define COMMON_XANADU_VQL_FRAME_ELIGIBILITY_HPP

#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/xanadu/vql/ast.hpp"

namespace xanadu::vql {

struct FrameEligibilityLimits {
  std::size_t maxNodes{100'000};
  std::size_t maxDepth{256};
};

struct FrameEligibility {
  std::unordered_set<std::string> reusableDefinitions;
  /// Sorted free lexical aliases captured from each call site's current scope.
  std::unordered_map<std::string, std::vector<std::string>> freeCaptureNames;
  /// Formal indices that reusable activations bind as closed functors.
  std::unordered_map<std::string, std::vector<std::size_t>>
      functorFormalIndices;
  /// Sorted aliases required by every activation for field-default resolution.
  std::unordered_map<std::string, std::vector<std::string>>
      requiredDefaultCaptureBindings;
  /// Required only by typed-rest validators when at least one rest actual is
  /// present at the named callsite; zero-group variants omit these checks.
  std::unordered_map<std::string, std::vector<std::string>>
      conditionalRequiredDefaultCaptureBindings;
  /// Required only when the final variadic formal consumes its whole-rest
  /// default because no rest actual groups were supplied.
  std::unordered_map<std::string, std::vector<std::string>>
      omittedRestRequiredDefaultCaptureBindings;
  std::unordered_map<std::string, std::string> refusalReasons;
};

struct FrameEligibilityError {
  std::string message;
};

/// One named call encountered while analyzing a closure body. The visible
/// aliases are the lexical bindings at that call, so helper captures already
/// bound by the closure do not become captures of the closure itself.
struct ClosureCallSite {
  std::string callee;
  std::unordered_set<std::string> visibleNames;
  std::size_t argumentCount{0};
  bool mayInjectContext{false};
};

/// Scope data collected by the same bounded walker used for reusable frames.
struct FrameClosureScope {
  std::unordered_set<std::string> freeCaptureNames;
  std::unordered_set<std::string> requiredDefaultCaptureBindings;
  std::unordered_set<std::string> conditionalRequiredDefaultCaptureBindings;
  std::unordered_set<std::string> omittedRestRequiredDefaultCaptureBindings;
  std::vector<ClosureCallSite> callSites;
  std::optional<std::string> refusalReason;
};

/// Classify definitions safe to lower to reusable activation-frame graphs.
/// Structural contract captures enter lexical scope only after their complete
/// match; aliases used by defaults must already exist before that match.
/// Free captures and required field-default aliases are returned in stable
/// sorted order. Unsupported definitions have a refusal reason.
/// Malformed/cyclic ASTs and traversal-budget exhaustion are hard errors, so
/// they cannot fall through to recursive inline expansion.
[[nodiscard]] std::expected<FrameEligibility, FrameEligibilityError>
analyzeFrameEligibility(const QueryExpression &query,
                        FrameEligibilityLimits limits = {});

/// Analyze a closure's formal scope and body with the frame eligibility
/// walker. Unsupported language forms are returned as a refusal reason;
/// malformed ASTs and exhausted budgets remain hard errors.
[[nodiscard]] std::expected<FrameClosureScope, FrameEligibilityError>
analyzeFrameClosureScope(const PathLambda &lambda,
                         FrameEligibilityLimits limits = {});

} // namespace xanadu::vql

#endif // COMMON_XANADU_VQL_FRAME_ELIGIBILITY_HPP
